// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN GPS vendor character device (/dev/stpgps).
 *
 * This is the UAPI face used by the Android GPS HAL of the SM-G532M ROM.
 * It talks to the same STP GPS channel as the standard GNSS device in
 * gps.c; both faces are mutually exclusive (see mtk_gps_claim()).
 *
 * Port from Samsung/MTK downstream connectivity/gps/stp_chrdev_gps.c of the
 * SM-G532M Android 6.0.1 kernel.
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "gps.h"
#include "mtk_wcn_stp.h"
#include "mtk_wcn_wmt.h"

/*
 * Downstream on-stack ioctl numbering (stp_chrdev_gps.c:31-37).  The
 * commands are plain integers, not _IO() encoded, because the ROM HAL
 * passes them verbatim.
 */
#define COMBO_IOC_GPS_HWVER		6
#define COMBO_IOC_GPS_IC_HW_VERSION	7
#define COMBO_IOC_GPS_IC_FW_VERSION	8
#define COMBO_IOC_D1_EFUSE_GET		9
#define COMBO_IOC_RTC_FLAG		10
#define COMBO_IOC_CO_CLOCK_FLAG		11

#define STPGPS_DRIVER_NAME	"stpgps"

/*
 * Downstream has a module parameter GPS_major defaulting to 191 ("never
 * used number", stp_chrdev_gps.c:24).  Keep the knob but default to 0 so the
 * major is allocated dynamically, as required for a modern kernel.
 */
static int stpgps_major;
module_param(stpgps_major, int, 0444);
MODULE_PARM_DESC(stpgps_major, "Fixed major for /dev/stpgps (0 = dynamic)");

static struct mtk_gps *stpgps_core;
static dev_t stpgps_devt;
static struct cdev stpgps_cdev;
static struct class *stpgps_class;
static struct device *stpgps_device;

static int stpgps_open(struct inode *inode, struct file *file)
{
	struct mtk_gps *gps = READ_ONCE(stpgps_core);
	int ret;

	if (!gps)
		return -ENODEV;

	/*
	 * Downstream GPS_open() rejects opens while a whole-chip reset is in
	 * progress (stp_chrdev_gps.c:397-400).
	 */
	mutex_lock(&gps->lock);
	ret = mtk_gps_claim(gps, MTK_GPS_OWNER_VENDOR);
	if (!ret)
		file->private_data = gps;
	mutex_unlock(&gps->lock);

	return ret;
}

static int stpgps_release(struct inode *inode, struct file *file)
{
	struct mtk_gps *gps = file->private_data;

	if (!gps)
		return 0;

	mutex_lock(&gps->lock);
	mtk_gps_release(gps, MTK_GPS_OWNER_VENDOR);
	mutex_unlock(&gps->lock);

	file->private_data = NULL;

	return 0;
}

/*
 * Blocking read of the raw GNSS byte stream, exactly like downstream
 * GPS_read() (stp_chrdev_gps.c:177-250): the bytes are dequeued from the
 * STP GPS channel, and the reader sleeps on the channel event callback.
 * The sequence counter removes the lost-wakeup window of the downstream
 * "flag" variable while keeping the same observable semantics.
 */
static ssize_t stpgps_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct mtk_gps *gps = file->private_data;
	u32 seq;
	int ret;

	if (!count)
		return 0;

	if (!gps)
		return -ENODEV;

	if (count > MTKSTP_BUFFER_SIZE)
		count = MTKSTP_BUFFER_SIZE;

	if (mutex_lock_interruptible(&gps->rd_mtx))
		return -ERESTARTSYS;

	for (;;) {
		/* A whole-chip reset invalidates pending transfers. */
		if (READ_ONCE(gps->resetting)) {
			ret = -EIO;
			break;
		}

		if (!READ_ONCE(gps->channel_on)) {
			ret = -ENODEV;
			break;
		}

		seq = READ_ONCE(gps->rx_seq);
		ret = mtk_wcn_stp_receive_data(gps->rd_buf, count,
					       GPS_TASK_INDX);
		if (ret)
			break;

		if (wait_event_interruptible(gps->rx_wait,
					     READ_ONCE(gps->rx_seq) != seq ||
					     READ_ONCE(gps->resetting) ||
					     !READ_ONCE(gps->channel_on))) {
			ret = -ERESTARTSYS;
			break;
		}
	}

	mutex_unlock(&gps->rd_mtx);

	if (ret > 0 && copy_to_user(buf, gps->rd_buf, ret))
		return -EFAULT;

	return ret;
}

/*
 * Write the raw GNSS byte stream into the STP GPS channel, like downstream
 * GPS_write() (stp_chrdev_gps.c:103-175).  A short transfer is reported as
 * the number of bytes accepted by STP; a full STP window is -ENOSPC.
 */
static ssize_t stpgps_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct mtk_gps *gps = file->private_data;
	int ret;

	if (!gps)
		return -ENODEV;

	/* Downstream rejects a zero-length write with -EFAULT. */
	if (!count)
		return -EFAULT;

	if (count > MTKSTP_BUFFER_SIZE)
		count = MTKSTP_BUFFER_SIZE;

	if (mutex_lock_interruptible(&gps->wr_mtx))
		return -ERESTARTSYS;

	if (READ_ONCE(gps->resetting)) {
		ret = -EIO;
		goto out;
	}

	if (!READ_ONCE(gps->channel_on)) {
		ret = -ENODEV;
		goto out;
	}

	if (copy_from_user(gps->wr_buf, buf, count)) {
		ret = -EFAULT;
		goto out;
	}

	ret = mtk_wcn_stp_send_data(gps->wr_buf, count, GPS_TASK_INDX);
	if (ret <= 0)
		ret = -ENOSPC;

out:
	mutex_unlock(&gps->wr_mtx);

	return ret;
}

/*
 * COMBO_IOC_D1_EFUSE_GET: downstream ioremaps 0x10206198 and returns the
 * 32-bit efuse word as the ioctl result (stp_chrdev_gps.c:330-343), it does
 * not copy it to userspace.  In this tree the word is only reachable through
 * an nvmem cell; there is no MT6735 efuse provider, so a board may supply
 * "gps-efuse" and otherwise the ioctl reports -EOPNOTSUPP instead of
 * inventing a value.
 */
static long stpgps_efuse_get(struct mtk_gps *gps)
{
	void *cell_data;
	size_t len = 0;
	u32 val;

	if (!gps->efuse_cell)
		return -EOPNOTSUPP;

	cell_data = nvmem_cell_read(gps->efuse_cell, &len);
	if (IS_ERR(cell_data))
		return PTR_ERR(cell_data);

	if (len < sizeof(val)) {
		kfree(cell_data);
		return -ENODATA;
	}

	memcpy(&val, cell_data, sizeof(val));
	kfree(cell_data);

	/* Downstream returns an int, so keep the same sign extension. */
	return (long)(s32)val;
}

static long stpgps_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	struct mtk_gps *gps = file->private_data;
	enum mtk_wcn_wmt_hwver hw_ver;
	u32 version;

	if (!gps)
		return -ENODEV;

	switch (cmd) {
	case 0:
		/*
		 * Downstream refuses to let the GPS device control STP
		 * directly: the STP state is owned by WMT only
		 * (stp_chrdev_gps.c:257-266).  Return -EINVAL, as it does.
		 */
		return -EINVAL;

	case 1:
		/* Raw-data mode is likewise disabled downstream (:268-271). */
		return -EINVAL;

	case COMBO_IOC_GPS_HWVER:
		/* stp_chrdev_gps.c:273-283: native enum, 4 bytes. */
		hw_ver = mtk_wcn_wmt_hwver_get();
		if (copy_to_user((void __user *)arg, &hw_ver, sizeof(hw_ver)))
			return -EFAULT;
		return 0;

	case COMBO_IOC_GPS_IC_HW_VERSION:
		version = mtk_wcn_wmt_hwver_raw_get();
		if (copy_to_user((void __user *)arg, &version, sizeof(version)))
			return -EFAULT;
		return 0;

	case COMBO_IOC_GPS_IC_FW_VERSION:
		version = mtk_wcn_wmt_fwver_get();
		if (copy_to_user((void __user *)arg, &version, sizeof(version)))
			return -EFAULT;
		return 0;

	case COMBO_IOC_D1_EFUSE_GET:
		return stpgps_efuse_get(gps);

	case COMBO_IOC_RTC_FLAG:
		/*
		 * Downstream rtc_GPS_low_power_detected()
		 * (stp_chrdev_gps.c:88-99) has a first-query branch and a
		 * second branch that both yield 0 while the RTC call is
		 * commented out.  Reproduce the documented result.
		 */
		return 0;

	case COMBO_IOC_CO_CLOCK_FLAG:
		/*
		 * Downstream compiles this to 0 with SOC_CO_CLOCK_FLAG
		 * disabled (:321-326); the WMT core does carry the flag, so
		 * report it live.
		 */
		return mtk_wcn_wmt_co_clock_flag_get() ? 1 : 0;

	default:
		/* Downstream returns -EFAULT for unknown commands (:345-348). */
		return -EFAULT;
	}
}

#ifdef CONFIG_COMPAT
static long stpgps_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	return stpgps_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations stpgps_fops = {
	.owner = THIS_MODULE,
	.open = stpgps_open,
	.release = stpgps_release,
	.read = stpgps_read,
	.write = stpgps_write,
	.unlocked_ioctl = stpgps_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = stpgps_compat_ioctl,
#endif
};

/*
 * Register /dev/stpgps for @gps.  The device node is created and destroyed
 * with the GPS platform device; it does not depend on CONFIG_GNSS.
 */
int mtk_gps_vendor_init(struct mtk_gps *gps)
{
	int ret;

	if (stpgps_major) {
		stpgps_devt = MKDEV(stpgps_major, 0);
		ret = register_chrdev_region(stpgps_devt, 1, STPGPS_DRIVER_NAME);
	} else {
		ret = alloc_chrdev_region(&stpgps_devt, 0, 1,
					  STPGPS_DRIVER_NAME);
	}
	if (ret) {
		dev_err(gps->dev, "failed to reserve stpgps major: %d\n", ret);
		return ret;
	}

	cdev_init(&stpgps_cdev, &stpgps_fops);
	stpgps_cdev.owner = THIS_MODULE;

	ret = cdev_add(&stpgps_cdev, stpgps_devt, 1);
	if (ret) {
		dev_err(gps->dev, "failed to add stpgps cdev: %d\n", ret);
		goto err_region;
	}

	stpgps_class = class_create(STPGPS_DRIVER_NAME);
	if (IS_ERR(stpgps_class)) {
		ret = PTR_ERR(stpgps_class);
		stpgps_class = NULL;
		goto err_cdev;
	}

	WRITE_ONCE(stpgps_core, gps);

	stpgps_device = device_create(stpgps_class, gps->dev, stpgps_devt,
				      NULL, STPGPS_DRIVER_NAME);
	if (IS_ERR(stpgps_device)) {
		ret = PTR_ERR(stpgps_device);
		stpgps_device = NULL;
		WRITE_ONCE(stpgps_core, NULL);
		goto err_class;
	}

	dev_info(gps->dev, "/dev/%s registered (major %d)\n",
		 STPGPS_DRIVER_NAME, MAJOR(stpgps_devt));

	return 0;

err_class:
	class_destroy(stpgps_class);
	stpgps_class = NULL;
err_cdev:
	cdev_del(&stpgps_cdev);
err_region:
	unregister_chrdev_region(stpgps_devt, 1);

	return ret;
}

void mtk_gps_vendor_exit(struct mtk_gps *gps)
{
	WRITE_ONCE(stpgps_core, NULL);

	if (stpgps_device) {
		device_destroy(stpgps_class, stpgps_devt);
		stpgps_device = NULL;
	}

	if (stpgps_class) {
		class_destroy(stpgps_class);
		stpgps_class = NULL;
	}

	cdev_del(&stpgps_cdev);
	unregister_chrdev_region(stpgps_devt, 1);
}
