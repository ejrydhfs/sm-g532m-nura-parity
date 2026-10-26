// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CCCI memory dump helpers and the debugfs request interface for dumping the
 * modem shared-memory window.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_private_log.c
 *     - ccci_util_mem_dump()       (line 752)
 *     - ccci_util_cmpt_mem_dump()  (line 793)
 *   drivers/misc/mediatek/eccci/ccci_core.c: get_smem_start_addr() (line 773)
 *
 * Downstream obtained the modem shared-memory mapping from the CCCI core via
 * get_smem_start_addr(); the util driver must not depend on the core, so the
 * window is registered with ccci_util_dump_set_window() instead.  A request
 * for an MD without a registered window returns -ENODEV.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/ccci.h>

#include "ccci_util.h"

/* Upper bound for a single debugfs dump request. */
#define CCCI_UTIL_DUMP_REQ_MAX		SZ_64K

struct ccci_util_dump_win {
	phys_addr_t phys;
	size_t size;
	void *base;
	bool valid;
};

static struct ccci_util_dump_win ccci_util_win[CCCI_MAX_MD_NUM];
static DEFINE_MUTEX(ccci_util_win_lock);
static struct dentry *ccci_util_dump_req;
static struct dentry *ccci_util_dump_window;

/**
 * ccci_util_dump_set_window() - register the modem shared-memory window
 * @md_id:	modem index (0 == MD_SYS1)
 * @phys:	physical base of the AP view of the MD shared memory
 * @size:	window size in bytes
 *
 * The CCCI core (or the platform code that reserved the MD shared memory)
 * calls this once the region is known.  Returns 0 on success.
 */
int ccci_util_dump_set_window(unsigned int md_id, phys_addr_t phys, size_t size)
{
	struct ccci_util_dump_win *win;
	void *base;

	if (md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	if (!size || !IS_ALIGNED(phys, PAGE_SIZE))
		return -EINVAL;
	if (phys + size < phys)
		return -EINVAL;

	base = memremap(phys, size, MEMREMAP_WB);
	if (!base)
		return -ENOMEM;

	mutex_lock(&ccci_util_win_lock);

	win = &ccci_util_win[md_id];
	if (win->valid)
		memunmap(win->base);

	win->phys = phys;
	win->size = size;
	win->base = base;
	win->valid = true;

	mutex_unlock(&ccci_util_win_lock);

	pr_info("[ccci%u/util] dump window %pa+0x%zx\n", md_id + 1, &phys, size);

	return 0;
}
EXPORT_SYMBOL_GPL(ccci_util_dump_set_window);

void ccci_util_dump_clear_window(unsigned int md_id)
{
	struct ccci_util_dump_win *win;

	if (md_id >= CCCI_MAX_MD_NUM)
		return;

	mutex_lock(&ccci_util_win_lock);
	win = &ccci_util_win[md_id];
	if (win->valid) {
		memunmap(win->base);
		win->base = NULL;
		win->phys = 0;
		win->size = 0;
		win->valid = false;
	}
	mutex_unlock(&ccci_util_win_lock);
}
EXPORT_SYMBOL_GPL(ccci_util_dump_clear_window);

/* ------------------------------------------------------------------ */
/* Hex/ASCII dump helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * ccci_util_dump_hex() - append a hex+ASCII dump of a kernel buffer
 * @md_id:	modem index selecting the dump ring
 * @buf_type:	CCCI_DUMP_* ring selector
 * @buf:	buffer to dump
 * @len:	number of bytes
 */
void ccci_util_dump_hex(int md_id, int buf_type, const void *buf, size_t len)
{
	const u8 *p = buf;
	char line[128];
	size_t off, n;

	if (!buf) {
		ccci_util_dump_write(md_id, buf_type, 0, "start_addr <NULL>\n");
		return;
	}
	if (!len) {
		ccci_util_dump_write(md_id, buf_type, 0, "len [0]\n");
		return;
	}

	for (off = 0; off < len; off += 16) {
		n = min_t(size_t, 16, len - off);
		hex_dump_to_buffer(p + off, n, 16, 1, line, sizeof(line), true);
		ccci_util_dump_write(md_id, buf_type, 0, "%08zX: %s\n", off,
				     line);
	}
}
EXPORT_SYMBOL_GPL(ccci_util_dump_hex);

/* Downstream ccci_util_mem_dump(), ccci_private_log.c:752. */
void ccci_util_mem_dump(int md_id, int buf_type, void *start_addr, int len)
{
	unsigned int *curr_p = (unsigned int *)start_addr;
	unsigned char *curr_ch_p;
	int _16_fix_num = len / 16;
	int tail_num = len % 16;
	unsigned char tail[16];
	int i, j;

	if (curr_p == NULL) {
		ccci_util_dump_write(md_id, buf_type, 0, "start_addr <NULL>\n");
		return;
	}
	if (len == 0) {
		ccci_util_dump_write(md_id, buf_type, 0, "len [0]\n");
		return;
	}

	ccci_util_dump_write(md_id, buf_type, 0, "Base: %px\n", start_addr);
	for (i = 0; i < _16_fix_num; i++) {
		ccci_util_dump_write(md_id, buf_type, 0,
				     "%03X: %08X %08X %08X %08X\n",
				     i * 16, *curr_p, *(curr_p + 1),
				     *(curr_p + 2), *(curr_p + 3));
		curr_p += 4;
	}

	if (tail_num > 0) {
		curr_ch_p = (unsigned char *)curr_p;
		for (j = 0; j < tail_num; j++)
			tail[j] = *curr_ch_p++;
		for (; j < 16; j++)
			tail[j] = 0;
		curr_p = (unsigned int *)tail;
		ccci_util_dump_write(md_id, buf_type, 0,
				     "%03X: %08X %08X %08X %08X\n",
				     i * 16, *curr_p, *(curr_p + 1),
				     *(curr_p + 2), *(curr_p + 3));
	}
}
EXPORT_SYMBOL_GPL(ccci_util_mem_dump);

/* Downstream ccci_util_cmpt_mem_dump(), ccci_private_log.c:793. */
void ccci_util_cmpt_mem_dump(int md_id, int buf_type, void *start_addr, int len)
{
	unsigned int *curr_p = (unsigned int *)start_addr;
	unsigned char *curr_ch_p;
	int _64_fix_num = len / 64;
	int tail_num = len % 64;
	unsigned char tail[64];
	int i, j;

	if (curr_p == NULL) {
		ccci_util_dump_write(md_id, buf_type, 0, "start_addr <NULL>\n");
		return;
	}
	if (len == 0) {
		ccci_util_dump_write(md_id, buf_type, 0, "len [0]\n");
		return;
	}

	for (i = 0; i < _64_fix_num; i++) {
		ccci_util_dump_write(md_id, buf_type, 0,
				     "%03X: %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X\n",
				     i * 64,
				     *curr_p, *(curr_p + 1), *(curr_p + 2),
				     *(curr_p + 3), *(curr_p + 4), *(curr_p + 5),
				     *(curr_p + 6), *(curr_p + 7), *(curr_p + 8),
				     *(curr_p + 9), *(curr_p + 10),
				     *(curr_p + 11), *(curr_p + 12),
				     *(curr_p + 13), *(curr_p + 14),
				     *(curr_p + 15));
		curr_p += 64 / 4;
	}

	if (tail_num > 0) {
		curr_ch_p = (unsigned char *)curr_p;
		for (j = 0; j < tail_num; j++)
			tail[j] = *curr_ch_p++;
		for (; j < 64; j++)
			tail[j] = 0;
		curr_p = (unsigned int *)tail;
		ccci_util_dump_write(md_id, buf_type, 0,
				     "%03X: %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X\n",
				     i * 64,
				     *curr_p, *(curr_p + 1), *(curr_p + 2),
				     *(curr_p + 3), *(curr_p + 4), *(curr_p + 5),
				     *(curr_p + 6), *(curr_p + 7), *(curr_p + 8),
				     *(curr_p + 9), *(curr_p + 10),
				     *(curr_p + 11), *(curr_p + 12),
				     *(curr_p + 13), *(curr_p + 14),
				     *(curr_p + 15));
	}
}
EXPORT_SYMBOL_GPL(ccci_util_cmpt_mem_dump);

/* ------------------------------------------------------------------ */
/* debugfs request interface: "md_id addr len"                         */
/* ------------------------------------------------------------------ */

static int ccci_util_dump_window_read(unsigned int md_id, u64 addr, u32 len,
				      void **out)
{
	struct ccci_util_dump_win *win;
	u64 offset;
	void *buf;
	int ret = 0;

	*out = NULL;

	if (md_id >= CCCI_MAX_MD_NUM || !len)
		return -EINVAL;
	if (len > CCCI_UTIL_DUMP_REQ_MAX)
		return -EINVAL;

	mutex_lock(&ccci_util_win_lock);
	win = &ccci_util_win[md_id];
	if (!win->valid) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (addr < win->phys || addr - win->phys >= win->size) {
		ret = -EINVAL;
		goto out_unlock;
	}
	offset = addr - win->phys;
	if (len > win->size - offset) {
		ret = -EINVAL;
		goto out_unlock;
	}

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	memcpy(buf, win->base + offset, len);
	*out = buf;
out_unlock:
	mutex_unlock(&ccci_util_win_lock);

	return ret;
}

static ssize_t ccci_util_dump_req_write(struct file *file,
					const char __user *ubuf, size_t count,
					loff_t *ppos)
{
	unsigned int md_id, len;
	unsigned long long addr;
	char *kbuf;
	void *data = NULL;
	int ret;

	if (!count)
		return 0;
	if (count > 64)
		return -EINVAL;

	kbuf = memdup_user_nul(ubuf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	if (sscanf(kbuf, "%u %llx %x", &md_id, &addr, &len) != 3) {
		kfree(kbuf);
		return -EINVAL;
	}
	kfree(kbuf);

	ret = ccci_util_dump_window_read(md_id, addr, len, &data);
	if (ret)
		return ret;

	if (!ccci_util_dump_ring_ready(md_id, CCCI_DUMP_MEM_DUMP)) {
		kfree(data);
		return -ENODEV;
	}

	ccci_util_dump_write(md_id, CCCI_DUMP_MEM_DUMP, 0,
			     "Base: 0x%llx len: %u\n", addr, len);
	ccci_util_dump_hex(md_id, CCCI_DUMP_MEM_DUMP, data, len);
	kfree(data);

	return count;
}

static const struct file_operations ccci_util_dump_req_fops = {
	.write		= ccci_util_dump_req_write,
	.llseek		= noop_llseek,
};

/*
 * Window registration through debugfs, for configurations where the CCCI
 * core does not know the AP view of the MD shared memory yet:
 *	echo "md_id phys size" > /sys/kernel/debug/ccci/dump_window
 * A zero size unregisters the window.
 */
static ssize_t ccci_util_dump_window_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	unsigned long long phys, size;
	unsigned int md_id;
	char *kbuf;
	int ret;

	if (!count)
		return 0;
	if (count > 64)
		return -EINVAL;

	kbuf = memdup_user_nul(ubuf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	if (sscanf(kbuf, "%u %llx %llx", &md_id, &phys, &size) != 3) {
		kfree(kbuf);
		return -EINVAL;
	}
	kfree(kbuf);

	if (md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;

	if (!size) {
		ccci_util_dump_clear_window(md_id);
		return count;
	}

	ret = ccci_util_dump_set_window(md_id, (phys_addr_t)phys,
					(size_t)size);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations ccci_util_dump_window_fops = {
	.write		= ccci_util_dump_window_write,
	.llseek		= noop_llseek,
};

int ccci_util_dump_debugfs_init(struct dentry *root)
{
	if (!root)
		return 0;

	ccci_util_dump_req = debugfs_create_file("dump", 0200, root, NULL,
						 &ccci_util_dump_req_fops);
	ccci_util_dump_window = debugfs_create_file("dump_window", 0200, root,
						    NULL,
						    &ccci_util_dump_window_fops);

	return 0;
}

void ccci_util_dump_debugfs_exit(void)
{
	debugfs_remove(ccci_util_dump_req);
	debugfs_remove(ccci_util_dump_window);
	ccci_util_dump_req = NULL;
	ccci_util_dump_window = NULL;
}

MODULE_DESCRIPTION("MediaTek CCCI memory dump helpers");
MODULE_LICENSE("GPL");
