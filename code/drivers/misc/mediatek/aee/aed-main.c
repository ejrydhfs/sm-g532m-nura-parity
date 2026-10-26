// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek AED (Android Exception Device) driver.
 *
 * The ROM exposes two misc devices, "aed0" (external exception, EE) and
 * "aed1" (kernel exception, KE) (vmlinux.strings:45359-45360, symbols
 * aed_ee_open 0xc062a328 / aed_ke_open 0xc062a5a0, aed_ioctl 0xc062a614).
 * They carry the AE_Msg request/response protocol of the AEE client
 * (downstream aed/aed.h:63-167) and the exception-dump ("DB") ioctls.
 *
 * The modem exception path enters through aed_md_exception_api()
 * (ROM 0xc0628298) and is served by the EE record queue below; the detail
 * response carries the CCCI debug window through aee_dump_ccci_debug_info()
 * exactly like downstream ee_gen_detail_msg() (aed-main.c:744-813).
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/aee/aed/aed-main.c and
 * drivers/misc/mediatek/aee/common/aee-common.c:
 *   msg_create/msg_destroy/msg_copy_to_user()  aed-main.c:203-260
 *   ee_gen_*_msg()                             aed-main.c:685-914
 *   ee_destroy_log()/ee_queue_request()/
 *   ee_worker()                                aed-main.c:816-968
 *   aed_ee_open/release/poll/read/write()      aed-main.c:975-1105
 *   aed_ke_open/release/poll/read/write()      aed-main.c:1107-1265
 *   aed_ioctl_bt()                             aed-main.c:1268-1300
 *   aed_ioctl()                                aed-main.c:1302-1526
 *   external_exception()                       aed-main.c:1844-1917
 *   aed_proc_init()/aed_proc_done()            aed-main.c:1961-2015
 *   misc device registration + init/exit       aed-main.c:2036-2100
 *   aed_md_exception_api() and the md32/scp/
 *   combo variants                             aee-common.c:224-300
 * Original copyright: Copyright (C) 2010/2015 MediaTek Inc. GPL-2.0.
 *
 * Deviations from the downstream code are listed in
 * $M/logs/rem2-aee.md, section 6; every one of them is marked below with
 * "DEVIATION".
 */
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/coredump.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/aee.h>

#if IS_ENABLED(CONFIG_MTK_CCCI) && !IS_MODULE(CONFIG_MTK_CCCI)
#include <linux/ccci.h>
#define AED_HAS_CCCI_DEBUG_INFO 1
#endif

#include "aed.h"

/* aed-main.c:165 */
#define MAX_EE_COREDUMP		0x800000
/* aed-main.c:938: the EE worker waits up to 5 minutes for AE_IND_LOG_CLOSE */
#define AED_EE_CLOSE_TIMEOUT	(5 * 60 * HZ)

#define CURRENT_EE_COREDUMP	"current-ee-coredump"

struct aee_req_queue {
	struct list_head list;
	spinlock_t lock;
};

/* aed-main.c:169-182 */
struct aed_kerec {
	char *msg;
};

struct aed_dev {
	struct aed_eerec *eerec;
	wait_queue_head_t eewait;

	struct aed_kerec kerec;
	wait_queue_head_t kewait;

	/* protects eerec and every message buffer of both devices */
	struct mutex msg_lock;
};

static struct aee_req_queue ee_queue;
static struct work_struct ee_work;
static DECLARE_COMPLETION(aed_ee_com);

/*
 * aed-main.c:66-84.  aee_mode defaults to AEE_MODE_NOT_INIT downstream and
 * external_exception() refuses to run while it is >= AEE_MODE_CUSTOMER_USER
 * (aed-main.c:1862); the ROM's AED daemon raises it to a working mode through
 * AEEIOCTL_SET_AEE_MODE.  This tree has no such daemon, so the default is the
 * non-suppressing MTK user mode (DEVIATION, see the report).
 */
static int aee_mode = AEE_MODE_MTK_USER;

static struct aed_dev aed_dev;
/* aed-main.c:2036-2050 */
static struct miscdevice aed_ee_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aed0",
	.mode = 0600,
};

static struct miscdevice aed_ke_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aed1",
	.mode = 0600,
};

static struct proc_dir_entry *aed_proc_dir;
static struct proc_dir_entry *aed_ee_coredump;

static long aed_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* ------------------------------------------------------------------ */
/* Message utilities, aed-main.c:203-260                              */
/* ------------------------------------------------------------------ */

static void msg_destroy(char **ppmsg)
{
	if (*ppmsg) {
		vfree(*ppmsg);
		*ppmsg = NULL;
	}
}

static struct AE_Msg *msg_create(char **ppmsg, int extra_size)
{
	struct AE_Msg *msg;

	msg_destroy(ppmsg);
	*ppmsg = vzalloc(sizeof(struct AE_Msg) + extra_size);
	if (!*ppmsg)
		return NULL;

	msg = (struct AE_Msg *)*ppmsg;
	msg->len = extra_size;
	return msg;
}

/*
 * aed-main.c:219-260.  DEVIATION: a short user buffer is clamped to the
 * remaining message instead of returning -EFAULT with a shortened count,
 * which is the normal read(2) contract.
 */
static ssize_t msg_copy_to_user(const char *msg, char __user *buf,
				size_t count, loff_t *f_pos)
{
	size_t len;

	if (!msg)
		return 0;

	len = ((struct AE_Msg *)msg)->len + sizeof(struct AE_Msg);
	if (*f_pos >= len)
		return 0;

	if (*f_pos + count > len)
		count = len - *f_pos;

	if (copy_to_user(buf, msg + *f_pos, count))
		return -EFAULT;

	*f_pos += count;
	return count;
}

/* ------------------------------------------------------------------ */
/* External exception record                                          */
/* ------------------------------------------------------------------ */

/* caller holds aed_dev.msg_lock */
static void ee_free_record(struct aed_eerec *eerec)
{
	msg_destroy(&eerec->msg);
	kfree(eerec->ee_log);
	kfree(eerec->ee_phy);
	kfree(eerec);
}

static void ee_destroy_log(void)
{
	struct aed_eerec *eerec;

	mutex_lock(&aed_dev.msg_lock);
	eerec = aed_dev.eerec;
	aed_dev.eerec = NULL;
	if (eerec)
		ee_free_record(eerec);
	mutex_unlock(&aed_dev.msg_lock);
}

/* ------------------------------------------------------------------ */
/* Text builder for the EE detail response                            */
/* ------------------------------------------------------------------ */

struct aed_detail {
	char *buf;
	size_t size;
	size_t len;
};

static void aed_detail_printf(struct aed_detail *d, const char *fmt, ...)
	__printf(2, 3);
static void aed_detail_printf(struct aed_detail *d, const char *fmt, ...)
{
	va_list args;
	int n;

	if (d->len >= d->size)
		return;

	va_start(args, fmt);
	n = vscnprintf(d->buf + d->len, d->size - d->len, fmt, args);
	va_end(args);
	d->len += n;
}

/* aed-main.c:790-800: four 32-bit words per line */
static void aed_detail_hexwords(struct aed_detail *d, const void *data, size_t len)
{
	const u32 *words = data;
	size_t off;

	for (off = 0; off + 16 <= len; off += 16)
		aed_detail_printf(d, "0x%08X 0x%08X 0x%08X 0x%08X\n",
				  words[off / 4 + 0], words[off / 4 + 1],
				  words[off / 4 + 2], words[off / 4 + 3]);

	/* a length that is not a multiple of 16 must not lose its tail */
	for (; off + 4 <= len; off += 4)
		aed_detail_printf(d, "0x%08X\n", words[off / 4]);
}

/* ------------------------------------------------------------------ */
/* EE message generators, aed-main.c:685-914                         */
/* ------------------------------------------------------------------ */

static void ee_gen_notavail_msg(struct aed_eerec *eerec)
{
	struct AE_Msg *rep_msg = msg_create(&eerec->msg, 0);

	if (!rep_msg)
		return;

	rep_msg->cmdType = AE_RSP;
	rep_msg->arg = AE_NOT_AVAILABLE;
	rep_msg->len = 0;
}

static void ee_gen_class_msg(struct aed_eerec *eerec)
{
#define EX_CLASS_EE_STR "External (EE)"
#define EX_CLASS_EE_SIZE 14
	struct AE_Msg *rep_msg;
	char *data;

	rep_msg = msg_create(&eerec->msg, EX_CLASS_EE_SIZE);
	if (!rep_msg)
		return;

	data = (char *)rep_msg + sizeof(struct AE_Msg);
	rep_msg->cmdType = AE_RSP;
	rep_msg->cmdId = AE_REQ_CLASS;
	rep_msg->len = EX_CLASS_EE_SIZE;
	strncpy(data, EX_CLASS_EE_STR, EX_CLASS_EE_SIZE);
}

static void ee_gen_type_msg(struct aed_eerec *eerec)
{
	struct AE_Msg *rep_msg;
	char *data;
	size_t len = strlen(eerec->assert_type) + 1;

	rep_msg = msg_create(&eerec->msg, len);
	if (!rep_msg)
		return;

	data = (char *)rep_msg + sizeof(struct AE_Msg);
	rep_msg->cmdType = AE_RSP;
	rep_msg->cmdId = AE_REQ_TYPE;
	rep_msg->len = len;
	strscpy(data, eerec->assert_type, len);
}

static void ee_gen_process_msg(struct aed_eerec *eerec)
{
#define PROCESS_STRLEN 512
	struct AE_Msg *rep_msg;
	char *data;
	int n;

	rep_msg = msg_create(&eerec->msg, PROCESS_STRLEN);
	if (!rep_msg)
		return;

	data = (char *)rep_msg + sizeof(struct AE_Msg);
	n = snprintf(data, PROCESS_STRLEN, "%s", eerec->exp_filename);

	rep_msg->cmdType = AE_RSP;
	rep_msg->cmdId = AE_REQ_PROCESS;
	rep_msg->len = n + 1;
}

/*
 * aed-main.c:744-813.  For an assert_type of "modem" the detail string starts
 * with "mdN:" and the CCCI debug record of that modem is appended as a hex
 * dump, fetched through aee_dump_ccci_debug_info() (frozen <linux/ccci.h> ABI,
 * ROM 0xc0508b54).
 *
 * DEVIATION: the CCCI window is ioremapped by the Nura CCCI core
 * (ccci_boot.c:302/332/382), so it is copied out with memcpy_fromio() before
 * being formatted; the downstream ARM32 code dereferenced it directly.  The
 * message size is the exact byte count instead of downstream's
 * (ee_log_size + ccci_log_size) * 4 + 128.
 */
static void ee_gen_detail_msg(struct aed_eerec *eerec)
{
	struct AE_Msg *rep_msg = NULL;
	struct aed_detail d;
	void *ccci_win = NULL;
	char *ccci = NULL;
	int ccci_size = 0;
	size_t total;

	if (!strncmp(eerec->assert_type, "md32", 4)) {
		total = (size_t)eerec->ee_log_size + 128;
		if (total > INT_MAX / 2)
			return;

		rep_msg = msg_create(&eerec->msg, (int)total);
		if (!rep_msg)
			return;

		memset(&d, 0, sizeof(d));
		d.buf = (char *)rep_msg + sizeof(struct AE_Msg);
		d.size = total;
		aed_detail_printf(&d, "== EXTERNAL EXCEPTION LOG ==\n%s\n",
				  (char *)eerec->ee_log);
		goto out;
	}

#ifdef AED_HAS_CCCI_DEBUG_INFO
	if (!strncmp(eerec->assert_type, "modem", 5)) {
		int md_id = 0;

		if (sscanf(eerec->exp_filename, "md%d:", &md_id) != 1 ||
		    aee_dump_ccci_debug_info(md_id, &ccci_win, &ccci_size) ||
		    !ccci_win || ccci_size <= 0) {
			ccci_win = NULL;
			ccci_size = 0;
		}
	}
#endif

	if (ccci_size > 0) {
		ccci = kmalloc(ccci_size, GFP_KERNEL);
		if (!ccci) {
			ccci_size = 0;
		} else {
			memcpy_fromio(ccci, (const void __iomem *)ccci_win,
				      ccci_size);
		}
	}

	total = (size_t)eerec->ee_log_size + ccci_size + 256;
	if (total > INT_MAX / 2)
		goto out;

	rep_msg = msg_create(&eerec->msg, (int)total);
	if (!rep_msg)
		goto out;

	memset(&d, 0, sizeof(d));
	d.buf = (char *)rep_msg + sizeof(struct AE_Msg);
	d.size = total;

	aed_detail_printf(&d, "== EXTERNAL EXCEPTION LOG ==\n");
	if (eerec->ee_log) {
		aed_detail_hexwords(&d, eerec->ee_log, eerec->ee_log_size);
	} else {
		aed_detail_printf(&d, "kmalloc fail, no log available\n");
	}
	aed_detail_printf(&d, "== MEM DUMP(%d) ==\n", eerec->ee_phy_size);

	if (ccci) {
		aed_detail_printf(&d, "== CCCI LOG ==\n");
		aed_detail_hexwords(&d, ccci, ccci_size);
		aed_detail_printf(&d, "== MEM DUMP(%d) ==\n", ccci_size);
	}

out:
	kfree(ccci);

	if (!rep_msg)
		return;

	rep_msg->cmdType = AE_RSP;
	rep_msg->cmdId = AE_REQ_DETAIL;
	rep_msg->arg = AE_PASS_BY_MEM;
	rep_msg->len = d.len + 1;
}

static void ee_gen_coredump_msg(struct aed_eerec *eerec)
{
	struct AE_Msg *rep_msg;
	char *data;

	rep_msg = msg_create(&eerec->msg, 256);
	if (!rep_msg)
		return;

	data = (char *)rep_msg + sizeof(struct AE_Msg);
	rep_msg->cmdType = AE_RSP;
	rep_msg->cmdId = AE_REQ_COREDUMP;
	rep_msg->arg = 0;
	snprintf(data, 256, "/proc/aed/%s", CURRENT_EE_COREDUMP);
	rep_msg->len = strlen(data) + 1;
}

/* ------------------------------------------------------------------ */
/* EE queue, worker and notification, aed-main.c:938-968, 1884-1917   */
/* ------------------------------------------------------------------ */

static void ee_queue_request(struct aed_eerec *eerec)
{
	unsigned long flags;

	spin_lock_irqsave(&ee_queue.lock, flags);
	list_add_tail(&eerec->list, &ee_queue.list);
	spin_unlock_irqrestore(&ee_queue.lock, flags);
	queue_work(system_wq, &ee_work);
}

/*
 * aed-main.c:884-914 (ee_gen_ind_msg).  Returns true when the record became
 * the current one and was announced to userspace.
 */
static bool ee_gen_ind_msg(struct aed_eerec *eerec)
{
	struct AE_Msg *rep_msg;

	mutex_lock(&aed_dev.msg_lock);
	if (aed_dev.eerec) {
		mutex_unlock(&aed_dev.msg_lock);
		pr_warn("AED: more than one EE message queued, dropping\n");
		return false;
	}
	aed_dev.eerec = eerec;

	/*
	 * DEVIATION: the completion is armed before the message can be seen,
	 * so an immediate AE_IND_LOG_CLOSE cannot be lost (downstream arms it
	 * after publishing, aed-main.c:901-904).
	 */
	init_completion(&aed_ee_com);

	rep_msg = msg_create(&eerec->msg, 0);
	if (rep_msg) {
		rep_msg->cmdType = AE_IND;
		rep_msg->cmdId = AE_IND_EXP_RAISED;
		rep_msg->arg = AE_EE;
		rep_msg->len = 0;
		rep_msg->dbOption = eerec->db_opt;
	}
	mutex_unlock(&aed_dev.msg_lock);

	wake_up(&aed_dev.eewait);
	if (!wait_for_completion_timeout(&aed_ee_com, AED_EE_CLOSE_TIMEOUT))
		pr_err("AED: EE TIMEOUT, no close event, skip\n");

	return true;
}

static void ee_worker(struct work_struct *work)
{
	struct aed_eerec *eerec;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&ee_queue.lock, flags);
		if (list_empty(&ee_queue.list)) {
			spin_unlock_irqrestore(&ee_queue.lock, flags);
			break;
		}
		eerec = list_first_entry(&ee_queue.list, struct aed_eerec,
					 list);
		list_del(&eerec->list);
		spin_unlock_irqrestore(&ee_queue.lock, flags);

		if (ee_gen_ind_msg(eerec)) {
			ee_destroy_log();
		} else {
			/*
			 * DEVIATION: downstream destroys the *current* record in
			 * this case (ee_worker(), aed-main.c:956-967); free the
			 * dropped one instead.
			 */
			mutex_lock(&aed_dev.msg_lock);
			ee_free_record(eerec);
			mutex_unlock(&aed_dev.msg_lock);
		}
	}
}

/* ------------------------------------------------------------------ */
/* /dev/aed0 file operations, aed-main.c:975-1105                     */
/* ------------------------------------------------------------------ */

static int aed_ee_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int aed_ee_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static __poll_t aed_ee_poll(struct file *file, struct poll_table_struct *ptable)
{
	__poll_t mask = 0;

	poll_wait(file, &aed_dev.eewait, ptable);

	mutex_lock(&aed_dev.msg_lock);
	if (aed_dev.eerec && aed_dev.eerec->msg)
		mask = EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&aed_dev.msg_lock);

	return mask;
}

static ssize_t aed_ee_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *f_pos)
{
	ssize_t ret;

	mutex_lock(&aed_dev.msg_lock);
	ret = msg_copy_to_user(aed_dev.eerec ? aed_dev.eerec->msg : NULL,
			       buf, count, f_pos);
	mutex_unlock(&aed_dev.msg_lock);

	return ret;
}

static ssize_t aed_ee_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *f_pos)
{
	struct aed_eerec *eerec;
	struct AE_Msg msg;
	ssize_t ret;

	*f_pos = 0;

	mutex_lock(&aed_dev.msg_lock);
	eerec = aed_dev.eerec;
	if (!eerec) {
		/* aed-main.c:1004-1006 */
		ret = -ENODEV;
		goto out;
	}

	/* a new request invalidates the previous response (aed-main.c:998) */
	msg_destroy(&eerec->msg);

	if (count != sizeof(struct AE_Msg)) {
		ret = -EINVAL;
		goto out;
	}
	if (copy_from_user(&msg, buf, count)) {
		ret = -EFAULT;
		goto out;
	}

	if (msg.cmdType == AE_REQ) {
		switch (msg.cmdId) {
		case AE_REQ_CLASS:
			ee_gen_class_msg(eerec);
			break;
		case AE_REQ_TYPE:
			ee_gen_type_msg(eerec);
			break;
		case AE_REQ_DETAIL:
			ee_gen_detail_msg(eerec);
			break;
		case AE_REQ_PROCESS:
			ee_gen_process_msg(eerec);
			break;
		case AE_REQ_BACKTRACE:
			/* aed-main.c:1038: EE has no kernel backtrace */
			ee_gen_notavail_msg(eerec);
			break;
		case AE_REQ_COREDUMP:
			ee_gen_coredump_msg(eerec);
			break;
		default:
			ee_gen_notavail_msg(eerec);
			break;
		}
	} else if (msg.cmdType == AE_IND && msg.cmdId == AE_IND_LOG_CLOSE) {
		complete(&aed_ee_com);
	}

	ret = count;
out:
	mutex_unlock(&aed_dev.msg_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* /dev/aed1 file operations, aed-main.c:1107-1265                    */
/* ------------------------------------------------------------------ */

/*
 * There is no kernel exception ("KE") producer in this tree: ipanic, mrdump,
 * the DRAM console and aee_oops_create()/aee_kernel_exception_api() are not
 * ported (see the report).  The device is registered because the ROM exposes
 * it and because a client may poll/open it; every request is answered with the
 * AE_NOT_AVAILABLE indication, which is exactly what downstream returns when
 * no oops is queued (ke_log_avail() false, aed-main.c:1014-1020).
 */
static void ke_gen_notavail_msg(void)
{
	struct AE_Msg *rep_msg = msg_create(&aed_dev.kerec.msg, 0);

	if (!rep_msg)
		return;

	rep_msg->cmdType = AE_RSP;
	rep_msg->arg = AE_NOT_AVAILABLE;
	rep_msg->len = 0;
}

static int aed_ke_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int aed_ke_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static __poll_t aed_ke_poll(struct file *file, struct poll_table_struct *ptable)
{
	__poll_t mask = 0;

	poll_wait(file, &aed_dev.kewait, ptable);

	mutex_lock(&aed_dev.msg_lock);
	if (aed_dev.kerec.msg)
		mask = EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&aed_dev.msg_lock);

	return mask;
}

static ssize_t aed_ke_read(struct file *filp, char __user *buf, size_t count,
			   loff_t *f_pos)
{
	ssize_t ret;

	mutex_lock(&aed_dev.msg_lock);
	ret = msg_copy_to_user(aed_dev.kerec.msg, buf, count, f_pos);
	mutex_unlock(&aed_dev.msg_lock);

	return ret;
}

static ssize_t aed_ke_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *f_pos)
{
	struct AE_Msg msg;

	*f_pos = 0;

	if (count != sizeof(struct AE_Msg))
		return -EINVAL;
	if (copy_from_user(&msg, buf, count))
		return -EFAULT;

	if (msg.cmdType == AE_REQ) {
		mutex_lock(&aed_dev.msg_lock);
		ke_gen_notavail_msg();
		mutex_unlock(&aed_dev.msg_lock);
	}

	return count;
}

/* ------------------------------------------------------------------ */
/* ioctls, aed-main.c:1268-1526                                       */
/* ------------------------------------------------------------------ */

/*
 * aed-main.c:1268-1300 (aed_ioctl_bt).  On success the caller's aee_ioctl is
 * updated (detail = AE_DEFAULT, size = frame count) and the frames are written
 * to ioctl.out.
 */
static long aed_ioctl_bt(unsigned long arg)
{
	struct aee_process_bt bt;
	struct aee_ioctl ioctl;
	void *frames;
	long ret;

	if (copy_from_user(&ioctl, (struct aee_ioctl __user *)arg,
			   sizeof(ioctl)))
		return -EFAULT;

	/* aed-process.c:171-204 writes at most AEE_NR_FRAME frames */
	frames = kzalloc(sizeof(struct aee_bt_frame) * AEE_NR_FRAME,
			 GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	bt.pid = ioctl.pid;
	bt.nr_entries = 0;
	bt.entries = frames;

	ret = aed_get_process_bt(&bt);
	if (ret)
		goto out;

	ioctl.detail = AE_DEFAULT;
	ioctl.size = bt.nr_entries;
	if (copy_to_user((struct aee_ioctl __user *)arg, &ioctl,
			 sizeof(ioctl))) {
		ret = -EFAULT;
		goto out;
	}
	if (!ioctl.out ||
	    copy_to_user((struct aee_bt_frame __user *)(unsigned long)ioctl.out,
			 frames, sizeof(struct aee_bt_frame) * AEE_NR_FRAME))
		ret = -EFAULT;

out:
	kfree(frames);
	return ret;
}

/*
 * aed-main.c:1360-1435 (AEEIOCTL_GET_THREAD_REG).  The client library tries
 * the 296-byte encoding first and falls back to the 76-byte one; the 76-byte
 * payload only describes an AArch32 task, so it is refused for a 64-bit one.
 */
static long aed_ioctl_get_thread_reg(unsigned long arg, bool wide)
{
	struct task_struct *task;
	const struct pt_regs *regs;
	int i;

	if (wide) {
		struct aee_thread_reg64 out;

		if (copy_from_user(&out, (void __user *)arg, sizeof(out)))
			return -EFAULT;
		if (out.tid <= 0)
			return -EINVAL;

		task = aed_find_task(out.tid);
		if (!task)
			return -EINVAL;

		regs = task_pt_regs(task);
		if (!regs) {
			put_task_struct(task);
			return -EINVAL;
		}

		/* arch/arm64/include/asm/ptrace.h:154-167 */
		memcpy(out.regs, &regs->user_regs, sizeof(regs->user_regs));
		out.regs[34] = regs->orig_x0;
		out.regs[35] = (u32)regs->syscallno;

		put_task_struct(task);

		if (copy_to_user((void __user *)arg, &out, sizeof(out)))
			return -EFAULT;
		return 0;
	}

	{
		struct aee_thread_reg out;

		if (copy_from_user(&out, (void __user *)arg, sizeof(out)))
			return -EFAULT;
		if (out.tid <= 0)
			return -EINVAL;

		task = aed_find_task(out.tid);
		if (!task)
			return -EINVAL;

		if (!is_compat_thread(task_thread_info(task))) {
			/* a 64-bit task has no AArch32 register file */
			put_task_struct(task);
			return -EOPNOTSUPP;
		}

		regs = task_pt_regs(task);
		if (!regs) {
			put_task_struct(task);
			return -EINVAL;
		}

		/*
		 * AArch32 view of an arm64 pt_regs: r0-r12 live in regs[0..12],
		 * the AArch32 SP/PC are the low halves of sp/pc, CPSR is PSTATE
		 * and ARM_orig_r0 is orig_x0.  [INFERENCE] from the
		 * architecture register mapping.
		 */
		for (i = 0; i < 13; i++)
			out.regs[i] = (u32)regs->regs[i];
		out.regs[13] = (u32)regs->sp;
		out.regs[14] = (u32)regs->regs[14];
		out.regs[15] = (u32)regs->pc;
		out.regs[16] = (u32)regs->pstate;
		out.regs[17] = (u32)regs->orig_x0;

		put_task_struct(task);

		if (copy_to_user((void __user *)arg, &out, sizeof(out)))
			return -EFAULT;
		return 0;
	}
}

/* aed-main.c:1460-1495 (AEEIOCTL_CHECK_SUID_DUMPABLE) */
static long aed_ioctl_check_dumpable(unsigned long arg)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int pid, dumpable;
	long ret = 0;

	if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
		return -EFAULT;
	if (pid <= 0)
		return -EINVAL;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (!task) {
		rcu_read_unlock();
		return -EINVAL;
	}
	mm = get_task_mm(task);
	rcu_read_unlock();
	if (!mm)
		return -EINVAL;

	dumpable = get_dumpable(mm);
	if (dumpable == 0)
		set_dumpable(mm, 1);

	mmput(mm);
	return ret;
}

static long aed_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret;

	switch (cmd) {
	case AEEIOCTL_GET_THREAD_REG:
		return aed_ioctl_get_thread_reg(arg, false);
	case AEEIOCTL_GET_THREAD_REG64:
		return aed_ioctl_get_thread_reg(arg, true);
	case AEEIOCTL_GET_PROCESS_BT:
		return aed_ioctl_bt(arg);
	case AEEIOCTL_SET_AEE_MODE:
		if (copy_from_user(&aee_mode, (void __user *)arg,
				   sizeof(aee_mode)))
			return -EFAULT;
		pr_info("AED: aee mode = %d\n", aee_mode);
		return 0;
	case AEEIOCTL_CHECK_SUID_DUMPABLE:
		return aed_ioctl_check_dumpable(arg);
	default:
		/*
		 * DAL/red-screen/RT-monitor/WDT ioctls need subsystems that this
		 * tree does not have; they are listed in the report as deferred.
		 */
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

/* ------------------------------------------------------------------ */
/* /proc/aed/current-ee-coredump, aed-main.c:1961-1985                */
/* ------------------------------------------------------------------ */

static ssize_t aed_ee_coredump_read(struct file *filp, char __user *buf,
				    size_t count, loff_t *pos)
{
	ssize_t ret;

	mutex_lock(&aed_dev.msg_lock);
	if (!aed_dev.eerec || !aed_dev.eerec->ee_phy) {
		ret = 0;
	} else {
		ret = simple_read_from_buffer(buf, count, pos,
					      aed_dev.eerec->ee_phy,
					      aed_dev.eerec->ee_phy_size);
	}
	mutex_unlock(&aed_dev.msg_lock);

	return ret;
}

static const struct proc_ops aed_ee_coredump_ops = {
	.proc_read = aed_ee_coredump_read,
};

static int aed_proc_init(void)
{
	aed_proc_dir = proc_mkdir("aed", NULL);
	if (!aed_proc_dir)
		return -ENOMEM;

	aed_ee_coredump = proc_create(CURRENT_EE_COREDUMP, 0400, aed_proc_dir,
				      &aed_ee_coredump_ops);
	if (!aed_ee_coredump) {
		remove_proc_entry("aed", NULL);
		aed_proc_dir = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void aed_proc_done(void)
{
	if (aed_ee_coredump)
		proc_remove(aed_ee_coredump);
	if (aed_proc_dir)
		remove_proc_entry("aed", NULL);
}

/* ------------------------------------------------------------------ */
/* External exception entry, aed-main.c:1844-1917 + aee-common.c:224-300 */
/* ------------------------------------------------------------------ */

/*
 * Runs in the CCCI exception worker and may be called from any context, so
 * every allocation is GFP_ATOMIC and nothing sleeps.  DEVIATION: the record
 * payloads are kmalloc'ed instead of vmalloc_user()'ed (aed-main.c:1893-1907),
 * because vmalloc may sleep; they are exported to userspace through
 * /proc/aed/current-ee-coredump instead.
 */
static void external_exception(const char *assert_type, const int *log,
			       int log_size, const int *phy, int phy_size,
			       const char *detail, const int db_opt)
{
	struct aed_eerec *eerec;

	if (aee_mode >= AEE_MODE_CUSTOMER_USER) {
		pr_info("AED: external exception in mode %d, skipped\n",
			aee_mode);
		return;
	}

	eerec = kzalloc(sizeof(*eerec), GFP_ATOMIC);
	if (!eerec) {
		pr_err("AED: cannot allocate EE record\n");
		return;
	}
	INIT_LIST_HEAD(&eerec->list);

	if (log && log_size > 0) {
		eerec->ee_log = kmalloc(log_size, GFP_ATOMIC);
		if (eerec->ee_log) {
			memcpy(eerec->ee_log, log, log_size);
			eerec->ee_log_size = log_size;
		}
	}
	if (!eerec->ee_log) {
		/* aed-main.c:1876-1882: keep a zeroed placeholder log */
		eerec->ee_log = kzalloc(16, GFP_ATOMIC);
		if (!eerec->ee_log) {
			kfree(eerec);
			return;
		}
		eerec->ee_log_size = 16;
	}

	if (in_interrupt() || phy_size < 0 || phy_size > MAX_EE_COREDUMP)
		phy_size = 0;

	if (phy && phy_size > 0) {
		eerec->ee_phy = kmalloc(phy_size, GFP_ATOMIC);
		if (eerec->ee_phy) {
			memcpy(eerec->ee_phy, phy, phy_size);
			eerec->ee_phy_size = phy_size;
		}
	}

	strscpy(eerec->assert_type, assert_type ?: "", sizeof(eerec->assert_type));
	strscpy(eerec->exp_filename, detail ?: "", sizeof(eerec->exp_filename));
	eerec->db_opt = db_opt;

	ee_queue_request(eerec);
}

void aed_md_exception_api(const int *log, int log_size, const int *phy,
			  int phy_size, const char *detail, const int db_opt)
{
	external_exception("modem", log, log_size, phy, phy_size, detail,
			   db_opt);
}
EXPORT_SYMBOL_GPL(aed_md_exception_api);

void aed_md32_exception_api(const int *log, int log_size, const int *phy,
			    int phy_size, const char *detail, const int db_opt)
{
	external_exception("md32", log, log_size, phy, phy_size, detail,
			   db_opt);
}
EXPORT_SYMBOL_GPL(aed_md32_exception_api);

void aed_scp_exception_api(const int *log, int log_size, const int *phy,
			   int phy_size, const char *detail, const int db_opt)
{
	external_exception("scp", log, log_size, phy, phy_size, detail,
			   db_opt);
}
EXPORT_SYMBOL_GPL(aed_scp_exception_api);

void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			     int phy_size, const char *detail, const int db_opt)
{
	external_exception("combo", log, log_size, phy, phy_size, detail,
			   db_opt);
}
EXPORT_SYMBOL_GPL(aed_combo_exception_api);

/* ------------------------------------------------------------------ */
/* File operations and module plumbing, aed-main.c:2020-2100          */
/* ------------------------------------------------------------------ */

static const struct file_operations aed_ee_fops = {
	.owner = THIS_MODULE,
	.open = aed_ee_open,
	.release = aed_ee_release,
	.poll = aed_ee_poll,
	.read = aed_ee_read,
	.write = aed_ee_write,
	.unlocked_ioctl = aed_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aed_ioctl,
#endif
};

static const struct file_operations aed_ke_fops = {
	.owner = THIS_MODULE,
	.open = aed_ke_open,
	.release = aed_ke_release,
	.poll = aed_ke_poll,
	.read = aed_ke_read,
	.write = aed_ke_write,
	.unlocked_ioctl = aed_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = aed_ioctl,
#endif
};

static int __init aed_init(void)
{
	int ret;

	/* the uapi numbers must match the ioctl encodings of aed.h:151-161 */
	BUILD_BUG_ON(AEEIOCTL_GET_THREAD_REG !=
		     _IOW('p', 0x07, struct aee_thread_reg));
	BUILD_BUG_ON(AEEIOCTL_GET_THREAD_REG64 !=
		     _IOW('p', 0x07, struct aee_thread_reg64));
	BUILD_BUG_ON(AEEIOCTL_GET_PROCESS_BT !=
		     _IOW('p', 0x04, struct aee_ioctl));
	BUILD_BUG_ON(AEEIOCTL_CHECK_SUID_DUMPABLE != _IOR('p', 0x08, int));
	BUILD_BUG_ON(AEEIOCTL_SET_AEE_MODE != _IOR('p', 0x06, int));
	BUILD_BUG_ON(sizeof(struct AE_Msg) != 24);
	BUILD_BUG_ON(sizeof(struct aee_ioctl) != 32);
	BUILD_BUG_ON(sizeof(struct aee_thread_reg) != 76);
	BUILD_BUG_ON(sizeof(struct aee_thread_reg64) != 296);
	BUILD_BUG_ON(sizeof(struct aee_bt_frame) != 256);

	ret = aed_proc_init();
	if (ret)
		return ret;

	mutex_init(&aed_dev.msg_lock);
	spin_lock_init(&ee_queue.lock);
	INIT_LIST_HEAD(&ee_queue.list);
	init_waitqueue_head(&aed_dev.eewait);
	init_waitqueue_head(&aed_dev.kewait);
	INIT_WORK(&ee_work, ee_worker);

	aed_ee_dev.fops = &aed_ee_fops;
	ret = misc_register(&aed_ee_dev);
	if (ret) {
		pr_err("AED: failed to register aed0(ee) device!\n");
		aed_proc_done();
		return ret;
	}

	aed_ke_dev.fops = &aed_ke_fops;
	ret = misc_register(&aed_ke_dev);
	if (ret) {
		pr_err("AED: failed to register aed1(ke) device!\n");
		misc_deregister(&aed_ee_dev);
		aed_proc_done();
		return ret;
	}

	pr_info("AED: /dev/aed0 (ee) and /dev/aed1 (ke) registered\n");
	return 0;
}

static void __exit aed_exit(void)
{
	cancel_work_sync(&ee_work);
	ee_destroy_log();

	misc_deregister(&aed_ke_dev);
	misc_deregister(&aed_ee_dev);

	mutex_lock(&aed_dev.msg_lock);
	msg_destroy(&aed_dev.kerec.msg);
	mutex_unlock(&aed_dev.msg_lock);

	aed_proc_done();
}
module_init(aed_init);
module_exit(aed_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek AED (AEE exception dump) driver");
MODULE_AUTHOR("MediaTek Inc.");
