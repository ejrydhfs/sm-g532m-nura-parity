// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CCCI utility driver: MD log/dump ring buffers, debugfs views and the
 * module entry points.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_private_log.c
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_main.c
 *
 * Downstream kept one 4 KiB debug ring plus six per-MD dump buffers
 * (init/bootup/normal/repeat/mem_dump/history).  This driver keeps the same
 * buffer set per MD, but allocates it lazily in ccci_util_log_init() and
 * exposes the contents through debugfs instead of /proc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/ccci.h>

#include "ccci_util.h"

struct ccci_util_ring {
	u8 *buf;
	u32 size;		/* power of two */
	u32 write_pos;
	u32 data_size;		/* valid bytes, capped at @size */
	u64 overflow;		/* bytes overwritten before being read */
	u64 total;		/* bytes accepted since allocation */
	spinlock_t lock;
};

struct ccci_util_md_ctx {
	struct device *dev;
	unsigned int md_id;
	bool inited;
	struct ccci_util_ring log;
	struct ccci_util_ring dump[CCCI_DUMP_MAX];
	struct dentry *dbg_dir;
	struct dentry *dbg_log;
	struct dentry *dbg_dump;
	struct dentry *dbg_stats;
};

static struct ccci_util_md_ctx ccci_md_ctx[CCCI_MAX_MD_NUM];
static struct dentry *ccci_util_root;
static bool ccci_util_root_owned;
static DEFINE_MUTEX(ccci_util_init_lock);
static bool ccci_util_inited;

static const char * const ccci_dump_buf_name[CCCI_DUMP_MAX] = {
	[CCCI_DUMP_INIT]	= "init",
	[CCCI_DUMP_BOOTUP]	= "bootup",
	[CCCI_DUMP_NORMAL]	= "normal",
	[CCCI_DUMP_REPEAT]	= "repeat",
	[CCCI_DUMP_MEM_DUMP]	= "mem_dump",
	[CCCI_DUMP_HISTORY]	= "history",
};

static const char * const ccci_dump_sep_str[CCCI_DUMP_MAX] = {
	[CCCI_DUMP_INIT]	= " Dump init log ",
	[CCCI_DUMP_BOOTUP]	= " Dump boot up log ",
	[CCCI_DUMP_NORMAL]	= " Dump normal log ",
	[CCCI_DUMP_REPEAT]	= " Dump repeat log ",
	[CCCI_DUMP_MEM_DUMP]	= " Dump mem dump log ",
	[CCCI_DUMP_HISTORY]	= " Dump history log ",
};

/* ------------------------------------------------------------------ */
/* Ring buffer helpers                                                 */
/* ------------------------------------------------------------------ */

static int ccci_util_ring_alloc(struct ccci_util_ring *ring, u32 size)
{
	if (!is_power_of_2(size))
		return -EINVAL;

	ring->buf = kvzalloc(size, GFP_KERNEL);
	if (!ring->buf)
		return -ENOMEM;

	ring->size = size;
	ring->write_pos = 0;
	ring->data_size = 0;
	ring->overflow = 0;
	ring->total = 0;
	spin_lock_init(&ring->lock);

	return 0;
}

static void ccci_util_ring_free(struct ccci_util_ring *ring)
{
	kvfree(ring->buf);
	ring->buf = NULL;
	ring->size = 0;
	ring->write_pos = 0;
	ring->data_size = 0;
	ring->overflow = 0;
	ring->total = 0;
}

static u32 ccci_util_ring_write(struct ccci_util_ring *ring, const void *data,
				u32 len)
{
	unsigned long flags;
	u32 first;

	if (!ring->buf || !ring->size)
		return 0;

	if (!len)
		return 0;

	spin_lock_irqsave(&ring->lock, flags);

	if (len > ring->size) {
		/* Keep the tail of an oversized record, like a truncated line. */
		u32 drop = len - ring->size;

		data += drop;
		len = ring->size;
		ring->overflow += drop;
	}

	if (ring->data_size + len > ring->size) {
		ring->overflow += ring->data_size + len - ring->size;
		ring->data_size = ring->size;
	} else {
		ring->data_size += len;
	}

	first = min(len, ring->size - ring->write_pos);
	memcpy(ring->buf + ring->write_pos, data, first);
	if (len > first)
		memcpy(ring->buf, data + first, len - first);

	ring->write_pos = (ring->write_pos + len) & (ring->size - 1);
	ring->total += len;

	spin_unlock_irqrestore(&ring->lock, flags);

	return len;
}

/*
 * Copy the current ring content, oldest byte first, into a newly allocated
 * buffer.  Returns 0 and a NULL buffer when the ring is empty.
 */
static int ccci_util_ring_snapshot(struct ccci_util_ring *ring, char **out,
				   size_t *out_len)
{
	unsigned long flags;
	unsigned int start, len, first;
	char *buf;

	*out = NULL;
	*out_len = 0;

	if (!ring->buf || !ring->size)
		return 0;

	buf = kvmalloc(ring->size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	spin_lock_irqsave(&ring->lock, flags);
	len = min(ring->data_size, ring->size);
	start = (ring->write_pos + ring->size - len) & (ring->size - 1);
	first = min(len, ring->size - start);
	memcpy(buf, ring->buf + start, first);
	if (len > first)
		memcpy(buf + first, ring->buf, len - first);
	spin_unlock_irqrestore(&ring->lock, flags);

	if (!len) {
		kvfree(buf);
		return 0;
	}

	*out = buf;
	*out_len = len;

	return 0;
}

void ccci_util_snapshot_free(char *buf)
{
	kvfree(buf);
}
EXPORT_SYMBOL_GPL(ccci_util_snapshot_free);

/* ------------------------------------------------------------------ */
/* Timestamped record writer used by the log rings and dump rings      */
/* ------------------------------------------------------------------ */

static int __printf(4, 0)
ccci_util_format_record(char *buf, size_t bufsize, unsigned int flags,
			const char *fmt, va_list args)
{
	unsigned long rem_nsec;
	u64 ts_nsec;
	int len = 0;
	int cpu;

	if (flags & CCCI_DUMP_TIME_FLAG) {
		char state = irqs_disabled() ? '-' : ' ';

		ts_nsec = local_clock();
		rem_nsec = do_div(ts_nsec, NSEC_PER_SEC);
		cpu = get_cpu();
		put_cpu();
		len += scnprintf(buf + len, bufsize - len,
				 "[%5lu.%06lu]%c(%x)[%d:%s]",
				 (unsigned long)ts_nsec, rem_nsec / 1000,
				 state, cpu, current->pid, current->comm);
	}

	if (flags & CCCI_DUMP_CURR_FLAG)
		len += scnprintf(buf + len, bufsize - len, "[%d:%s]",
				 current->pid, current->comm);

	len += vscnprintf(buf + len, bufsize - len, fmt, args);

	return len;
}

static int __printf(4, 0)
__ccci_util_dump_write(int md_id, int buf_type, unsigned int flag,
		       const char *fmt, va_list args)
{
	struct ccci_util_ring *ring;
	struct ccci_util_md_ctx *ctx;
	char tmp[CCCI_UTIL_MAX_WRITE];
	int len;

	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	if (buf_type < 0 || buf_type >= CCCI_DUMP_MAX)
		return -EINVAL;

	ctx = &ccci_md_ctx[md_id];
	if (!ctx->inited)
		return -ENODEV;

	ring = &ctx->dump[buf_type];
	if (!ring->buf)
		return -ENODEV;

	if (flag & CCCI_DUMP_CLR_BUF_FLAG) {
		unsigned long flags;

		spin_lock_irqsave(&ring->lock, flags);
		ring->write_pos = 0;
		ring->data_size = 0;
		spin_unlock_irqrestore(&ring->lock, flags);
	}

	len = ccci_util_format_record(tmp, sizeof(tmp), flag, fmt, args);
	if (len > 0)
		ccci_util_ring_write(ring, tmp, min_t(int, len, sizeof(tmp) - 1));

	return len;
}

int ccci_util_dump_write(int md_id, int buf_type, unsigned int flag,
			 const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = __ccci_util_dump_write(md_id, buf_type, flag, fmt, args);
	va_end(args);

	return len;
}
EXPORT_SYMBOL_GPL(ccci_util_dump_write);

/* Downstream entry point name, ccci_private_log.c:328. */
int ccci_dump_write(int md_id, int buf_type, unsigned int flag,
		    const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = __ccci_util_dump_write(md_id, buf_type, flag, fmt, args);
	va_end(args);

	return len;
}
EXPORT_SYMBOL_GPL(ccci_dump_write);

int ccci_util_dump_append_raw(unsigned int md_id, unsigned int buf_type,
			      const void *data, size_t len)
{
	struct ccci_util_md_ctx *ctx;

	if (md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	if (buf_type >= CCCI_DUMP_MAX)
		return -EINVAL;

	ctx = &ccci_md_ctx[md_id];
	if (!ctx->inited || !ctx->dump[buf_type].buf)
		return -ENODEV;

	if (!len)
		return 0;
	if (len > U32_MAX)
		return -EINVAL;

	return ccci_util_ring_write(&ctx->dump[buf_type], data, len);
}
EXPORT_SYMBOL_GPL(ccci_util_dump_append_raw);

bool ccci_util_dump_ring_ready(unsigned int md_id, unsigned int buf_type)
{
	if (md_id >= CCCI_MAX_MD_NUM || buf_type >= CCCI_DUMP_MAX)
		return false;

	return ccci_md_ctx[md_id].inited &&
	       ccci_md_ctx[md_id].dump[buf_type].buf;
}
EXPORT_SYMBOL_GPL(ccci_util_dump_ring_ready);

/* ------------------------------------------------------------------ */
/* Public log API (contract, <linux/ccci.h>)                           */
/* ------------------------------------------------------------------ */

void ccci_util_log(unsigned int md_id, const char *fmt, ...)
{
	struct ccci_util_md_ctx *ctx;
	char tmp[CCCI_UTIL_MAX_WRITE];
	unsigned long rem_nsec;
	u64 ts_nsec;
	va_list args;
	int cpu;
	int len;

	if (md_id >= CCCI_MAX_MD_NUM)
		return;

	ctx = &ccci_md_ctx[md_id];
	if (!ctx->inited || !ctx->log.buf)
		return;

	ts_nsec = local_clock();
	rem_nsec = do_div(ts_nsec, NSEC_PER_SEC);
	cpu = get_cpu();
	put_cpu();
	len = scnprintf(tmp, sizeof(tmp), "[%5lu.%06lu]%c(%x)[%d:%s][ccci%u/util] ",
			(unsigned long)ts_nsec, rem_nsec / 1000,
			irqs_disabled() ? '-' : ' ', cpu, current->pid,
			current->comm, md_id + 1);

	va_start(args, fmt);
	len += vscnprintf(tmp + len, sizeof(tmp) - len, fmt, args);
	va_end(args);

	ccci_util_ring_write(&ctx->log, tmp, min_t(int, len, sizeof(tmp) - 1));
}
EXPORT_SYMBOL_GPL(ccci_util_log);

void ccci_util_dump_mem(unsigned int md_id, const void *buf, size_t len)
{
	if (md_id >= CCCI_MAX_MD_NUM)
		return;

	ccci_util_dump_hex(md_id, CCCI_DUMP_MEM_DUMP, buf, len);
}
EXPORT_SYMBOL_GPL(ccci_util_dump_mem);

/* ------------------------------------------------------------------ */
/* Snapshot builders for debugfs and /dev/ccci_dump                    */
/* ------------------------------------------------------------------ */

static void ccci_util_sep_fill(char *dst, const char *text)
{
	memset(dst, '_', 64);
	dst[0] = '\n';
	dst[63] = '\n';
	if (text)
		memcpy(dst + 8, text, min_t(size_t, strlen(text), 55));
}

static void ccci_util_md_sep_fill(char *dst, int md_id)
{
	memset(dst, '=', 64);
	dst[0] = '\n';
	dst[8] = ' ';
	dst[9] = 'B';
	dst[10] = 'U';
	dst[11] = 'F';
	dst[12] = 'F';
	dst[13] = '0' + (md_id % 10);
	dst[14] = ' ';
	dst[63] = '\n';
}

int ccci_util_log_snapshot(int md_id, char **out_buf)
{
	struct ccci_util_md_ctx *ctx;
	size_t len = 0;
	int ret;

	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;

	*out_buf = NULL;
	ctx = &ccci_md_ctx[md_id];
	if (!ctx->inited)
		return -ENODEV;

	ret = ccci_util_ring_snapshot(&ctx->log, out_buf, &len);
	if (ret)
		return ret;
	if (len > INT_MAX) {
		kvfree(*out_buf);
		*out_buf = NULL;
		return -EOVERFLOW;
	}

	return len;
}
EXPORT_SYMBOL_GPL(ccci_util_log_snapshot);

/*
 * Concatenate the dump buffers in the same shape as downstream
 * ccci_dump_read(): one "BUFF=<n>" separator per MD, then a named separator
 * followed by the buffer content for each enabled dump type.
 * @md_id < 0 selects every initialised MD.
 */
int ccci_util_dump_snapshot(int md_id, char **out_buf)
{
	unsigned int md, type;
	size_t total = 0;
	size_t pos = 0;
	char *buf;
	char sep[64];
	unsigned long flags;

	*out_buf = NULL;

	for (md = 0; md < CCCI_MAX_MD_NUM; md++) {
		if (md_id >= 0 && md != (unsigned int)md_id)
			continue;
		if (!ccci_md_ctx[md].inited)
			continue;

		total += 64;
		for (type = 0; type < CCCI_DUMP_MAX; type++) {
			struct ccci_util_ring *ring = &ccci_md_ctx[md].dump[type];

			total += 64;
			if (!ring->buf)
				continue;
			spin_lock_irqsave(&ring->lock, flags);
			total += min(ring->data_size, ring->size);
			spin_unlock_irqrestore(&ring->lock, flags);
		}
	}

	if (!total)
		return 0;

	buf = kvmalloc(total, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (md = 0; md < CCCI_MAX_MD_NUM; md++) {
		if (md_id >= 0 && md != (unsigned int)md_id)
			continue;
		if (!ccci_md_ctx[md].inited)
			continue;

		ccci_util_md_sep_fill(sep, md);
		memcpy(buf + pos, sep, 64);
		pos += 64;

		for (type = 0; type < CCCI_DUMP_MAX; type++) {
			struct ccci_util_ring *ring = &ccci_md_ctx[md].dump[type];
			unsigned int len, start, first;

			ccci_util_sep_fill(sep, ccci_dump_sep_str[type]);
			memcpy(buf + pos, sep, 64);
			pos += 64;

			if (!ring->buf)
				continue;

			spin_lock_irqsave(&ring->lock, flags);
			len = min(ring->data_size, ring->size);
			start = (ring->write_pos + ring->size - len) &
				(ring->size - 1);
			if (pos + len > total) {
				spin_unlock_irqrestore(&ring->lock, flags);
				continue;
			}
			first = min(len, ring->size - start);
			memcpy(buf + pos, ring->buf + start, first);
			if (len > first)
				memcpy(buf + pos + first, ring->buf, len - first);
			spin_unlock_irqrestore(&ring->lock, flags);
			pos += len;
		}
	}

	*out_buf = buf;

	if (pos > INT_MAX) {
		kvfree(buf);
		*out_buf = NULL;
		return -EOVERFLOW;
	}

	return pos;
}
EXPORT_SYMBOL_GPL(ccci_util_dump_snapshot);

/* Downstream get_dump_buf_usage(), ccci_private_log.c:734. */
int get_dump_buf_usage(char buf[], int size)
{
	unsigned int md, type;
	int ret = 0;
	unsigned long flags;

	for (md = 0; md < CCCI_MAX_MD_NUM; md++) {
		if (!ccci_md_ctx[md].inited)
			continue;

		if (ret >= size)
			break;
		ret += scnprintf(buf + ret, size - ret, "For dump buf [%u]\n", md);
		for (type = 0; type < CCCI_DUMP_MAX; type++) {
			struct ccci_util_ring *ring = &ccci_md_ctx[md].dump[type];
			u32 used;
			u64 overflow;

			spin_lock_irqsave(&ring->lock, flags);
			used = ring->data_size;
			overflow = ring->overflow;
			spin_unlock_irqrestore(&ring->lock, flags);

			if (ret >= size)
				break;
			ret += scnprintf(buf + ret, size - ret, "  %s:%u overflow:%llu\n",
					 ccci_dump_buf_name[type], used, overflow);
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(get_dump_buf_usage);

/* ------------------------------------------------------------------ */
/* debugfs views: ccci/mdN/log, ccci/mdN/dump, ccci/mdN/log_stats      */
/* ------------------------------------------------------------------ */

struct ccci_util_dbg_buf {
	char *buf;
	size_t len;
};

static ssize_t ccci_util_dbg_read(struct file *file, char __user *ubuf,
				  size_t size, loff_t *ppos)
{
	struct ccci_util_dbg_buf *dbg = file->private_data;
	size_t len;

	if (!dbg || !dbg->buf)
		return 0;
	if (*ppos >= dbg->len)
		return 0;

	len = min(size, dbg->len - *ppos);
	if (copy_to_user(ubuf, dbg->buf + *ppos, len))
		return -EFAULT;

	*ppos += len;

	return len;
}

static int ccci_util_dbg_open(struct inode *inode, struct file *file,
			      int (*snapshot)(int, char **),
			      int md_id)
{
	struct ccci_util_dbg_buf *dbg;
	char *buf = NULL;
	int ret;

	dbg = kzalloc(sizeof(*dbg), GFP_KERNEL);
	if (!dbg)
		return -ENOMEM;

	ret = snapshot(md_id, &buf);
	if (ret < 0) {
		kfree(dbg);
		return ret;
	}

	dbg->buf = buf;
	dbg->len = ret;
	file->private_data = dbg;

	return 0;
}

static int ccci_util_log_dbg_open(struct inode *inode, struct file *file)
{
	struct ccci_util_md_ctx *ctx = inode->i_private;

	return ccci_util_dbg_open(inode, file, ccci_util_log_snapshot,
				  ctx->md_id);
}

static int ccci_util_dump_dbg_open(struct inode *inode, struct file *file)
{
	struct ccci_util_md_ctx *ctx = inode->i_private;

	return ccci_util_dbg_open(inode, file, ccci_util_dump_snapshot,
				  ctx->md_id);
}

static int ccci_util_dbg_release(struct inode *inode, struct file *file)
{
	struct ccci_util_dbg_buf *dbg = file->private_data;

	if (dbg) {
		ccci_util_snapshot_free(dbg->buf);
		kfree(dbg);
	}

	return 0;
}

static ssize_t ccci_util_log_dbg_write(struct file *file,
				       const char __user *ubuf, size_t len,
				       loff_t *ppos)
{
	struct ccci_util_md_ctx *ctx = file->f_inode->i_private;
	char *kbuf;
	size_t n;
	int ret;

	if (!len)
		return 0;

	n = min_t(size_t, len, CCCI_UTIL_MAX_WRITE - 1);
	kbuf = memdup_user_nul(ubuf, n);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	strim(kbuf);
	ccci_util_log(ctx->md_id, "%s", kbuf);
	ret = len;
	kfree(kbuf);

	return ret;
}

static const struct file_operations ccci_util_log_dbg_fops = {
	.open		= ccci_util_log_dbg_open,
	.read		= ccci_util_dbg_read,
	.write		= ccci_util_log_dbg_write,
	.release	= ccci_util_dbg_release,
	.llseek		= noop_llseek,
};

static const struct file_operations ccci_util_dump_dbg_fops = {
	.open		= ccci_util_dump_dbg_open,
	.read		= ccci_util_dbg_read,
	.release	= ccci_util_dbg_release,
	.llseek		= noop_llseek,
};

static ssize_t ccci_util_stats_dbg_read(struct file *file, char __user *ubuf,
					size_t size, loff_t *ppos)
{
	struct ccci_util_md_ctx *ctx = file->private_data;
	char *buf;
	int len = 0;
	int ret = 0;
	unsigned int type;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "md%u log: size=%u used=%u overflow=%llu total=%llu\n",
			 ctx->md_id + 1, ctx->log.size, ctx->log.data_size,
			 ctx->log.overflow, ctx->log.total);

	for (type = 0; type < CCCI_DUMP_MAX; type++) {
		struct ccci_util_ring *ring = &ctx->dump[type];

		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "md%u dump[%s]: size=%u used=%u overflow=%llu total=%llu\n",
				 ctx->md_id + 1, ccci_dump_buf_name[type],
				 ring->size, ring->data_size, ring->overflow,
				 ring->total);
	}

	if (*ppos < len) {
		ret = min(size, (size_t)(len - *ppos));
		if (copy_to_user(ubuf, buf + *ppos, ret))
			ret = -EFAULT;
		else
			*ppos += ret;
	}

	kfree(buf);

	return ret;
}

static const struct debugfs_short_fops ccci_util_stats_dbg_fops = {
	.read	= ccci_util_stats_dbg_read,
	.llseek	= noop_llseek,
};

static void ccci_util_dbg_remove_md(struct ccci_util_md_ctx *ctx)
{
	debugfs_remove(ctx->dbg_log);
	debugfs_remove(ctx->dbg_dump);
	debugfs_remove(ctx->dbg_stats);
	ctx->dbg_log = NULL;
	ctx->dbg_dump = NULL;
	ctx->dbg_stats = NULL;
	ctx->dbg_dir = NULL;
}

static void ccci_util_dbg_create_md(struct ccci_util_md_ctx *ctx)
{
	struct dentry *dir;
	char name[16];

	if (!ccci_util_root)
		return;

	snprintf(name, sizeof(name), "md%u", ctx->md_id + 1);

	dir = debugfs_create_dir(name, ccci_util_root);
	if (IS_ERR_OR_NULL(dir)) {
		/* The CCCI core may have created ccci/mdN first. */
		dir = debugfs_lookup(name, ccci_util_root);
		if (IS_ERR_OR_NULL(dir)) {
			pr_warn("[ccci%u/util] no debugfs dir %s\n",
				ctx->md_id + 1, name);
			return;
		}
	}

	ctx->dbg_dir = dir;
	ctx->dbg_log = debugfs_create_file("log", 0600, dir, ctx,
					   &ccci_util_log_dbg_fops);
	ctx->dbg_dump = debugfs_create_file("dump", 0444, dir, ctx,
					    &ccci_util_dump_dbg_fops);
	ctx->dbg_stats = debugfs_create_file("log_stats", 0444, dir, ctx,
					     &ccci_util_stats_dbg_fops);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void ccci_util_md_release(struct ccci_util_md_ctx *ctx)
{
	unsigned int type;

	ccci_util_dbg_remove_md(ctx);

	ccci_util_ring_free(&ctx->log);
	for (type = 0; type < CCCI_DUMP_MAX; type++)
		ccci_util_ring_free(&ctx->dump[type]);

	ctx->dev = NULL;
	ctx->inited = false;
}

static int ccci_util_md_alloc(struct ccci_util_md_ctx *ctx)
{
	static const u32 dump_size[CCCI_DUMP_MAX] = {
		[CCCI_DUMP_INIT]	= CCCI_UTIL_INIT_SETTING_BUF,
		[CCCI_DUMP_BOOTUP]	= CCCI_UTIL_BOOT_UP_BUF,
		[CCCI_DUMP_NORMAL]	= CCCI_UTIL_NORMAL_BUF,
		[CCCI_DUMP_REPEAT]	= CCCI_UTIL_REPEAT_BUF,
		[CCCI_DUMP_MEM_DUMP]	= CCCI_UTIL_REG_DUMP_BUF,
		[CCCI_DUMP_HISTORY]	= CCCI_UTIL_HISTORY_BUF,
	};
	unsigned int type;
	int ret;

	ret = ccci_util_ring_alloc(&ctx->log, CCCI_UTIL_LOG_SIZE);
	if (ret)
		return ret;

	for (type = 0; type < CCCI_DUMP_MAX; type++) {
		ret = ccci_util_ring_alloc(&ctx->dump[type], dump_size[type]);
		if (ret)
			goto err;
	}

	return 0;

err:
	ccci_util_ring_free(&ctx->log);
	while (type--)
		ccci_util_ring_free(&ctx->dump[type]);

	return ret;
}

static int __ccci_util_init(void)
{
	int ret;

	if (ccci_util_inited)
		return 0;

	ccci_util_root = debugfs_create_dir("ccci", NULL);
	if (IS_ERR_OR_NULL(ccci_util_root)) {
		/* debugfs is optional: the char devices still work. */
		ccci_util_root = NULL;
		ccci_util_root_owned = false;
	} else {
		ccci_util_root_owned = true;
	}

	ret = ccci_util_dump_debugfs_init(ccci_util_root);
	if (ret)
		goto err_root;

	ret = ccci_util_logdev_init();
	if (ret)
		goto err_dump;

	ccci_util_inited = true;

	return 0;

err_dump:
	ccci_util_dump_debugfs_exit();
err_root:
	if (ccci_util_root_owned)
		debugfs_remove(ccci_util_root);
	ccci_util_root = NULL;
	ccci_util_root_owned = false;

	return ret;
}

int ccci_util_init(void)
{
	int ret;

	mutex_lock(&ccci_util_init_lock);
	ret = __ccci_util_init();
	mutex_unlock(&ccci_util_init_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ccci_util_init);

void ccci_util_exit(void)
{
	unsigned int md;

	mutex_lock(&ccci_util_init_lock);
	if (!ccci_util_inited)
		goto out;

	for (md = 0; md < CCCI_MAX_MD_NUM; md++) {
		if (ccci_md_ctx[md].inited)
			ccci_util_md_release(&ccci_md_ctx[md]);
	}

	ccci_util_dump_debugfs_exit();
	ccci_util_logdev_exit();

	if (ccci_util_root_owned)
		debugfs_remove(ccci_util_root);
	ccci_util_root = NULL;
	ccci_util_root_owned = false;
	ccci_util_inited = false;
out:
	mutex_unlock(&ccci_util_init_lock);
}
EXPORT_SYMBOL_GPL(ccci_util_exit);

int ccci_util_log_init(unsigned int md_id, struct device *dev)
{
	struct ccci_util_md_ctx *ctx;
	int ret;

	if (md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;

	mutex_lock(&ccci_util_init_lock);

	ret = __ccci_util_init();
	if (ret)
		goto out;

	ctx = &ccci_md_ctx[md_id];
	if (ctx->inited) {
		ret = 0;
		goto out;
	}

	ret = ccci_util_md_alloc(ctx);
	if (ret) {
		dev_err(dev, "md%u log buffers: %d\n", md_id + 1, ret);
		goto out;
	}

	ctx->md_id = md_id;
	ctx->dev = dev;
	ctx->inited = true;
	ccci_util_dbg_create_md(ctx);

	dev_info(dev, "md%u CCCI log/dump rings ready\n", md_id + 1);
out:
	mutex_unlock(&ccci_util_init_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ccci_util_log_init);

void ccci_util_log_exit(unsigned int md_id)
{
	mutex_lock(&ccci_util_init_lock);
	if (md_id < CCCI_MAX_MD_NUM && ccci_md_ctx[md_id].inited)
		ccci_util_md_release(&ccci_md_ctx[md_id]);
	mutex_unlock(&ccci_util_init_lock);
}
EXPORT_SYMBOL_GPL(ccci_util_log_exit);

static int __init ccci_util_module_init(void)
{
	return ccci_util_init();
}

static void __exit ccci_util_module_exit(void)
{
	ccci_util_exit();
}

module_exit(ccci_util_module_exit);
/* Downstream: subsys_initcall(ccci_util_init), ccci_util_lib_main.c:38. */
subsys_initcall(ccci_util_module_init);

MODULE_DESCRIPTION("MediaTek CCCI utility/log driver");
MODULE_LICENSE("GPL");
