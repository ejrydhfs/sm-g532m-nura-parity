// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CCCI character devices: /dev/ccci_log (debug log ring) and /dev/ccci_dump
 * (per-MD dump buffers), plus the event log ring.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_private_log.c
 *     - ccci_log_write()       (line 54)
 *     - ccci_log_write_raw()   (line 104)
 *     - ccci_log_read()        (line 165)
 *     - ccci_log_poll/open/close (lines 212, 223, 231)
 *     - ccci_dump_read/poll/open/close (lines 472, 615, 620, 636)
 *     - ccci_event_log()       (line 867)
 *     - ccci_event_log_cpy()   (line 941)
 *     - ccci_log_init()        (line 969)
 *
 * Downstream exposed the two interfaces as /proc/ccci_log and /proc/ccci_dump
 * with proc_create() (ROM: proc_create_data at 0xc0197e64, called from
 * ccci_log_init at 0xc0504e30 and 0xc0504e98).  This driver exposes them as
 * misc character devices, which is what Android/MTK userspace expects on
 * current kernels; the byte stream is unchanged.
 *
 * /dev/ccci_dump write format (one record per write):
 *	struct ccci_util_dump_rec {
 *		__le32 type;	// CCCI_DUMP_* selector
 *		__le32 len;	// payload length
 *		u8     data[];
 *	};
 * The target MD is selected with CCCI_UTIL_IOC_SET_MD (default MD_SYS1).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/ccci.h>

#include "ccci_util.h"

#define CCCI_LOG_BUF_SIZE	CCCI_UTIL_LOG_SIZE

struct ccci_log_ring {
	u8 *buffer;
	u32 write_pos;
	u32 data_size;		/* valid bytes, capped at CCCI_LOG_BUF_SIZE */
	atomic_t reader_cnt;
	wait_queue_head_t log_wq;
	spinlock_t write_lock;
};

static struct ccci_log_ring ccci_log_buf;

static int ccci_util_ring_put(u8 *buffer, u32 size, u32 *write_pos,
			      u32 *data_size, const void *data, u32 len,
			      spinlock_t *lock)
{
	unsigned long flags;
	u32 start, first;

	spin_lock_irqsave(lock, flags);

	start = *write_pos & (size - 1);
	first = min(len, size - start);
	memcpy(buffer + start, data, first);
	if (len > first)
		memcpy(buffer, data + first, len - first);

	*write_pos = (start + len) & (size - 1);
	if (*data_size + len > size)
		*data_size = size;	/* full: oldest byte at *write_pos */
	else
		*data_size += len;

	spin_unlock_irqrestore(lock, flags);

	return len;
}

static int ccci_log_ring_write(const void *data, u32 len)
{
	if (!ccci_log_buf.buffer)
		return -ENODEV;

	if (!len)
		return 0;
	if (len > CCCI_LOG_BUF_SIZE) {
		data += len - CCCI_LOG_BUF_SIZE;
		len = CCCI_LOG_BUF_SIZE;
	}

	ccci_util_ring_put(ccci_log_buf.buffer, CCCI_LOG_BUF_SIZE,
			   &ccci_log_buf.write_pos, &ccci_log_buf.data_size,
			   data, len, &ccci_log_buf.write_lock);

	wake_up_interruptible_all(&ccci_log_buf.log_wq);

	return len;
}

int ccci_log_write(const char *fmt, ...)
{
	char temp_log[CCCI_UTIL_MAX_WRITE];
	unsigned long rem_nsec;
	u64 ts_nsec;
	va_list args;
	int cpu;
	int write_len;

	ts_nsec = local_clock();
	rem_nsec = do_div(ts_nsec, NSEC_PER_SEC);
	cpu = get_cpu();
	put_cpu();
	write_len = scnprintf(temp_log, sizeof(temp_log),
			      "[%5lu.%06lu]%c(%x)[%d:%s]",
			      (unsigned long)ts_nsec, rem_nsec / 1000,
			      irqs_disabled() ? '-' : ' ', cpu, current->pid,
			      current->comm);

	va_start(args, fmt);
	write_len += vscnprintf(temp_log + write_len,
				sizeof(temp_log) - write_len, fmt, args);
	va_end(args);

	return ccci_log_ring_write(temp_log,
				   min_t(int, write_len, sizeof(temp_log) - 1));
}
EXPORT_SYMBOL_GPL(ccci_log_write);

int ccci_log_write_raw(unsigned int set_flags, const char *fmt, ...)
{
	char temp_log[CCCI_UTIL_MAX_WRITE];
	unsigned long rem_nsec;
	u64 ts_nsec;
	va_list args;
	int write_len = 0;
	int cpu;

	if (set_flags & CCCI_DUMP_TIME_FLAG) {
		ts_nsec = local_clock();
		rem_nsec = do_div(ts_nsec, NSEC_PER_SEC);
		cpu = get_cpu();
		put_cpu();
		write_len += scnprintf(temp_log + write_len,
				       sizeof(temp_log) - write_len,
				       "[%5lu.%06lu]%c(%x)",
				       (unsigned long)ts_nsec, rem_nsec / 1000,
				       irqs_disabled() ? '-' : ' ', cpu);
	}

	if (set_flags & CCCI_DUMP_CURR_FLAG)
		write_len += scnprintf(temp_log + write_len,
				       sizeof(temp_log) - write_len, "[%d:%s]",
				       current->pid, current->comm);

	va_start(args, fmt);
	write_len += vscnprintf(temp_log + write_len,
				sizeof(temp_log) - write_len, fmt, args);
	va_end(args);

	return ccci_log_ring_write(temp_log,
				   min_t(int, write_len, sizeof(temp_log) - 1));
}
EXPORT_SYMBOL_GPL(ccci_log_write_raw);

/* ------------------------------------------------------------------ */
/* /dev/ccci_log                                                       */
/* ------------------------------------------------------------------ */

static ssize_t ccci_log_read(struct file *file, char __user *buf, size_t size,
			     loff_t *ppos)
{
	unsigned int available, read_len, read_pos, first;
	unsigned long flags;
	int ret;

retry:
	spin_lock_irqsave(&ccci_log_buf.write_lock, flags);
	available = ccci_log_buf.data_size;
	if (available >= CCCI_LOG_BUF_SIZE) {
		/* Overflow: the write pointer is the oldest byte. */
		available = CCCI_LOG_BUF_SIZE;
		read_pos = ccci_log_buf.write_pos;
	} else {
		read_pos = (ccci_log_buf.write_pos + CCCI_LOG_BUF_SIZE -
			    available) & (CCCI_LOG_BUF_SIZE - 1);
	}
	spin_unlock_irqrestore(&ccci_log_buf.write_lock, flags);

	if (!available) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(ccci_log_buf.log_wq,
					       READ_ONCE(ccci_log_buf.data_size));
		if (ret)
			return -ERESTARTSYS;
		goto retry;
	}

	read_len = min_t(size_t, size, available);
	first = min(read_len, CCCI_LOG_BUF_SIZE - read_pos);

	if (copy_to_user(buf, ccci_log_buf.buffer + read_pos, first))
		return -EFAULT;
	if (read_len > first &&
	    copy_to_user(buf + first, ccci_log_buf.buffer, read_len - first))
		return -EFAULT;

	spin_lock_irqsave(&ccci_log_buf.write_lock, flags);
	ccci_log_buf.data_size -= read_len;
	spin_unlock_irqrestore(&ccci_log_buf.write_lock, flags);

	return read_len;
}

static __poll_t ccci_log_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &ccci_log_buf.log_wq, wait);
	if (READ_ONCE(ccci_log_buf.data_size))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static int ccci_log_open(struct inode *inode, struct file *file)
{
	if (!ccci_log_buf.buffer)
		return -ENODEV;

	if (atomic_inc_return(&ccci_log_buf.reader_cnt) > 1) {
		atomic_dec(&ccci_log_buf.reader_cnt);
		return -EBUSY;
	}

	return 0;
}

static int ccci_log_close(struct inode *inode, struct file *file)
{
	atomic_dec(&ccci_log_buf.reader_cnt);

	return 0;
}

static const struct file_operations ccci_log_fops = {
	.owner		= THIS_MODULE,
	.read		= ccci_log_read,
	.open		= ccci_log_open,
	.release	= ccci_log_close,
	.poll		= ccci_log_poll,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* /dev/ccci_dump                                                      */
/* ------------------------------------------------------------------ */

#define CCCI_UTIL_IOC_MAGIC	'C'
#define CCCI_UTIL_IOC_SET_MD	_IOW(CCCI_UTIL_IOC_MAGIC, 0x40, __u32)
#define CCCI_UTIL_IOC_GET_MD	_IOR(CCCI_UTIL_IOC_MAGIC, 0x41, __u32)

struct ccci_util_dump_rec {
	__le32 type;
	__le32 len;
	u8 data[];
} __packed;

struct ccci_dump_file {
	u32 md_id;
	char *buf;
	size_t len;
	size_t pos;
	bool snapshotted;
};

static void ccci_dump_file_reset_snapshot(struct ccci_dump_file *df)
{
	ccci_util_snapshot_free(df->buf);
	df->buf = NULL;
	df->len = 0;
	df->pos = 0;
	df->snapshotted = false;
}

static int ccci_dump_open(struct inode *inode, struct file *file)
{
	struct ccci_dump_file *df;

	df = kzalloc(sizeof(*df), GFP_KERNEL);
	if (!df)
		return -ENOMEM;

	df->md_id = CCCI_MD_SYS1;
	file->private_data = df;

	return nonseekable_open(inode, file);
}

static int ccci_dump_release(struct inode *inode, struct file *file)
{
	struct ccci_dump_file *df = file->private_data;

	if (df) {
		ccci_dump_file_reset_snapshot(df);
		kfree(df);
	}

	return 0;
}

static ssize_t ccci_dump_read(struct file *file, char __user *buf, size_t size,
			      loff_t *ppos)
{
	struct ccci_dump_file *df = file->private_data;
	size_t len;

	if (!df)
		return -EBADF;

	if (!df->snapshotted) {
		int ret = ccci_util_dump_snapshot(df->md_id, &df->buf);

		if (ret < 0)
			return ret;
		df->len = ret;
		df->snapshotted = true;
	}

	if (df->pos >= df->len)
		return 0;

	len = min(size, df->len - df->pos);
	if (copy_to_user(buf, df->buf + df->pos, len))
		return -EFAULT;

	df->pos += len;

	return len;
}

static ssize_t ccci_dump_dev_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct ccci_dump_file *df = file->private_data;
	struct ccci_util_dump_rec rec;
	u32 type, len;
	void *data;
	int ret;

	if (!df)
		return -EBADF;
	if (count < sizeof(rec))
		return -EINVAL;

	if (copy_from_user(&rec, buf, sizeof(rec)))
		return -EFAULT;

	type = le32_to_cpu(rec.type);
	len = le32_to_cpu(rec.len);

	if (type >= CCCI_DUMP_MAX)
		return -EINVAL;
	if (len != count - sizeof(rec))
		return -EINVAL;
	if (!len)
		return -EINVAL;

	data = memdup_user(buf + sizeof(rec), len);
	if (IS_ERR(data))
		return PTR_ERR(data);

	ret = ccci_util_dump_append_raw(df->md_id, type, data, len);
	kfree(data);
	if (ret < 0)
		return ret;

	return count;
}

static __poll_t ccci_dump_poll(struct file *file, poll_table *wait)
{
	/* Downstream ccci_dump_poll() always reports readable data. */
	return EPOLLIN | EPOLLRDNORM;
}

static long ccci_dump_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct ccci_dump_file *df = file->private_data;
	u32 md_id;

	if (!df)
		return -EBADF;

	switch (cmd) {
	case CCCI_UTIL_IOC_SET_MD:
		if (get_user(md_id, (u32 __user *)arg))
			return -EFAULT;
		if (md_id >= CCCI_MAX_MD_NUM)
			return -EINVAL;
		df->md_id = md_id;
		ccci_dump_file_reset_snapshot(df);
		return 0;
	case CCCI_UTIL_IOC_GET_MD:
		if (put_user(df->md_id, (u32 __user *)arg))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ccci_dump_fops = {
	.owner		= THIS_MODULE,
	.read		= ccci_dump_read,
	.write		= ccci_dump_dev_write,
	.open		= ccci_dump_open,
	.release	= ccci_dump_release,
	.poll		= ccci_dump_poll,
	.unlocked_ioctl	= ccci_dump_ioctl,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* Event log ring (ccci_event_log / ccci_event_log_cpy)                */
/* ------------------------------------------------------------------ */

#define CCCI_EVENT_BUF_SIZE	4096

struct ccci_event_buf {
	u8 *buffer;
	u32 write_pos;
	u32 data_size;
	spinlock_t lock;
};

static struct ccci_event_buf ccci_event_buffer;
static bool ccci_logdev_registered;

int ccci_event_log(const char *fmt, ...)
{
	char temp_log[CCCI_UTIL_MAX_WRITE];
	struct timespec64 ts;
	struct tm tm, tm_android;
	unsigned long rem_nsec;
	unsigned long flags;
	u64 ts_nsec;
	va_list args;
	u32 wr_pos, can_be_write;
	int write_len;
	int cpu;

	if (!ccci_event_buffer.buffer)
		return 0;

	/* Kernel monotonic stamp, as in downstream. */
	ts_nsec = local_clock();
	rem_nsec = do_div(ts_nsec, NSEC_PER_SEC);
	cpu = get_cpu();
	put_cpu();

	/* Wall clock plus the configured timezone offset. */
	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec, 0, &tm);
	time64_to_tm(ts.tv_sec - sys_tz.tz_minuteswest * 60, 0, &tm_android);

	write_len = scnprintf(temp_log, sizeof(temp_log),
			      "%ld%02d%02d-%02d:%02d:%02d.%03d [%5lu.%06lu]%c(%x)[%d:%s]",
			      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			      tm_android.tm_hour, tm_android.tm_min,
			      tm_android.tm_sec,
			      (unsigned int)(ts.tv_nsec / NSEC_PER_MSEC),
			      (unsigned long)ts_nsec, rem_nsec / 1000,
			      irqs_disabled() ? '-' : ' ', cpu, current->pid,
			      current->comm);

	va_start(args, fmt);
	write_len += vscnprintf(temp_log + write_len,
				sizeof(temp_log) - write_len, fmt, args);
	va_end(args);

	write_len = min_t(int, write_len, sizeof(temp_log) - 1);
	if (!write_len)
		return 0;

	spin_lock_irqsave(&ccci_event_buffer.lock, flags);

	wr_pos = ccci_event_buffer.write_pos;
	ccci_event_buffer.data_size += write_len;
	if (ccci_event_buffer.data_size > CCCI_EVENT_BUF_SIZE)
		ccci_event_buffer.data_size = CCCI_EVENT_BUF_SIZE + 1;

	ccci_event_buffer.write_pos = (wr_pos + write_len) &
				      (CCCI_EVENT_BUF_SIZE - 1);

	if (wr_pos + write_len > CCCI_EVENT_BUF_SIZE) {
		can_be_write = CCCI_EVENT_BUF_SIZE - wr_pos;
		memcpy(ccci_event_buffer.buffer + wr_pos, temp_log, can_be_write);
		memcpy(ccci_event_buffer.buffer, temp_log + can_be_write,
		       write_len - can_be_write);
	} else {
		memcpy(ccci_event_buffer.buffer + wr_pos, temp_log, write_len);
	}

	spin_unlock_irqrestore(&ccci_event_buffer.lock, flags);

	return write_len;
}
EXPORT_SYMBOL_GPL(ccci_event_log);

int ccci_event_log_cpy(char buf[], int size)
{
	unsigned long flags;
	unsigned int rd_pos;
	int cpy_size, first_half;

	if (!ccci_event_buffer.buffer || !buf || size <= 0)
		return 0;

	spin_lock_irqsave(&ccci_event_buffer.lock, flags);

	if (ccci_event_buffer.data_size > CCCI_EVENT_BUF_SIZE) {
		cpy_size = min_t(int, CCCI_EVENT_BUF_SIZE, size);
		rd_pos = ccci_event_buffer.write_pos;
		first_half = CCCI_EVENT_BUF_SIZE - rd_pos;
		first_half = min(first_half, cpy_size);
		memcpy(buf, ccci_event_buffer.buffer + rd_pos, first_half);
		if (cpy_size > first_half)
			memcpy(buf + first_half, ccci_event_buffer.buffer,
			       cpy_size - first_half);
	} else {
		cpy_size = min_t(int, ccci_event_buffer.data_size, size);
		memcpy(buf, ccci_event_buffer.buffer, cpy_size);
	}

	spin_unlock_irqrestore(&ccci_event_buffer.lock, flags);

	return cpy_size;
}
EXPORT_SYMBOL_GPL(ccci_event_log_cpy);

/* ------------------------------------------------------------------ */
/* Device registration                                                 */
/* ------------------------------------------------------------------ */

static struct miscdevice ccci_log_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ccci_log",
	.fops	= &ccci_log_fops,
	.mode	= 0400,
};

static struct miscdevice ccci_dump_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ccci_dump",
	.fops	= &ccci_dump_fops,
	.mode	= 0600,
};

int ccci_util_logdev_init(void)
{
	int ret;

	if (ccci_logdev_registered)
		return 0;

	ccci_log_buf.buffer = kzalloc(CCCI_LOG_BUF_SIZE, GFP_KERNEL);
	if (!ccci_log_buf.buffer)
		return -ENOMEM;

	spin_lock_init(&ccci_log_buf.write_lock);
	init_waitqueue_head(&ccci_log_buf.log_wq);
	ccci_log_buf.write_pos = 0;
	ccci_log_buf.data_size = 0;
	atomic_set(&ccci_log_buf.reader_cnt, 0);

	ccci_event_buffer.buffer = kzalloc(CCCI_EVENT_BUF_SIZE, GFP_KERNEL);
	if (!ccci_event_buffer.buffer) {
		ret = -ENOMEM;
		goto err_log;
	}
	spin_lock_init(&ccci_event_buffer.lock);
	ccci_event_buffer.write_pos = 0;
	ccci_event_buffer.data_size = 0;

	ret = misc_register(&ccci_log_dev);
	if (ret)
		goto err_event;

	ret = misc_register(&ccci_dump_dev);
	if (ret)
		goto err_dump;

	ccci_logdev_registered = true;

	return 0;

err_dump:
	misc_deregister(&ccci_log_dev);
err_event:
	kfree(ccci_event_buffer.buffer);
	ccci_event_buffer.buffer = NULL;
err_log:
	kfree(ccci_log_buf.buffer);
	ccci_log_buf.buffer = NULL;

	return ret;
}

void ccci_util_logdev_exit(void)
{
	if (!ccci_logdev_registered)
		return;

	misc_deregister(&ccci_dump_dev);
	misc_deregister(&ccci_log_dev);
	ccci_logdev_registered = false;

	kfree(ccci_event_buffer.buffer);
	ccci_event_buffer.buffer = NULL;
	ccci_event_buffer.write_pos = 0;
	ccci_event_buffer.data_size = 0;

	kfree(ccci_log_buf.buffer);
	ccci_log_buf.buffer = NULL;
	ccci_log_buf.write_pos = 0;
	ccci_log_buf.data_size = 0;
}

/* Downstream entry point name, ccci_private_log.c:969. */
void ccci_log_init(void)
{
	int ret = ccci_util_logdev_init();

	if (ret)
		pr_err("[ccci0/util] fail to init log devices: %d\n", ret);
}
EXPORT_SYMBOL_GPL(ccci_log_init);
