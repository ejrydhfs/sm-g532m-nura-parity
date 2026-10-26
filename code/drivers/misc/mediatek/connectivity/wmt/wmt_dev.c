// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN /dev/stpwmt character device and patch store.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/wmt_dev.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The MTK connectivity launcher owns the firmware patch files.  It tells the
 * driver which patches to download through WMT_IOCTL_SET_PATCH_NUM and
 * WMT_IOCTL_SET_PATCH_INFO; this file loads the named files from firmware
 * storage when the WMT power-on sequence asks for them.  No firmware is
 * embedded in the driver.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/kernel_read_file.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "wmt_local.h"

/* wmt_dev.c:72-94 */
#define WMT_IOC_MAGIC		0xa0
#define WMT_IOCTL_SET_PATCH_NAME	_IOW(WMT_IOC_MAGIC, 4, char *)
#define WMT_IOCTL_SET_STP_MODE		_IOW(WMT_IOC_MAGIC, 5, int)
#define WMT_IOCTL_FUNC_ONOFF_CTRL	_IOW(WMT_IOC_MAGIC, 6, int)
#define WMT_IOCTL_GET_CHIP_INFO		_IOR(WMT_IOC_MAGIC, 12, int)
#define WMT_IOCTL_SET_PATCH_NUM		_IOW(WMT_IOC_MAGIC, 14, int)
#define WMT_IOCTL_SET_PATCH_INFO	_IOW(WMT_IOC_MAGIC, 15, char *)
#define WMT_IOCTL_PORT_NAME		_IOWR(WMT_IOC_MAGIC, 20, char *)
#define WMT_IOCTL_WMT_CFG_NAME		_IOWR(WMT_IOC_MAGIC, 21, char *)
#define WMT_IOCTL_WMT_QUERY_CHIPID	_IOR(WMT_IOC_MAGIC, 22, int)
#define WMT_IOCTL_WMT_TELL_CHIPID	_IOW(WMT_IOC_MAGIC, 23, int)

#define WMT_DRIVER_NAME		"mtk_stp_wmt"
#define WMT_DEV_MAJOR		190
#define WMT_DEV_NUM		1

static int wmt_major = WMT_DEV_MAJOR;
module_param(wmt_major, uint, 0444);

static struct cdev wmt_cdev;
static struct class *wmt_class;

/*
 * Firmware file access
 */

static int wmt_request_firmware_file(const char *name, u8 **buf, size_t *size)
{
	const struct firmware *fw;
	char fw_name[NAME_MAX + 1];
	int ret;

	strscpy(fw_name, name, sizeof(fw_name));

	/* request_firmware() resolves names relative to /lib/firmware, so drop
	 * the leading '/' of Android-style absolute paths.
	 */
	if (fw_name[0] == '/')
		memmove(fw_name, fw_name + 1, strlen(fw_name));

	ret = request_firmware(&fw, fw_name, NULL);
	if (ret)
		return ret;

	*buf = kmemdup(fw->data, fw->size, GFP_KERNEL);
	if (!*buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	*size = fw->size;
	release_firmware(fw);
	return 0;
}

static int wmt_read_file_path(const char *path, u8 **buf, size_t *size)
{
	size_t file_size = 0;
	void *vbuf = NULL;
	ssize_t ret;

	ret = kernel_read_file_from_path(path, 0, &vbuf, SIZE_MAX, &file_size,
					 READING_FIRMWARE);
	if (ret < 0)
		return ret;

	*buf = kmemdup(vbuf, file_size, GFP_KERNEL);
	vfree(vbuf);
	if (!*buf)
		return -ENOMEM;

	*size = file_size;
	return 0;
}

int wmt_dev_read_file(const char *name, u8 **buf, size_t *size)
{
	int ret;

	if (!name || !name[0])
		return -EINVAL;

	*buf = NULL;
	*size = 0;

	ret = wmt_request_firmware_file(name, buf, size);
	if (!ret)
		return 0;

	wmt_dbg("request_firmware(%s) failed (%d), trying the path directly\n",
		name, ret);
	return wmt_read_file_path(name, buf, size);
}

int wmt_dev_patch_get(const char *name, u8 **buf, size_t *size)
{
	u8 *file = NULL;
	size_t file_size = 0;
	u8 *patch;
	int ret;

	if (!name || !name[0])
		return -EINVAL;

	ret = wmt_dev_read_file(name, &file, &file_size);
	if (ret)
		return ret;

	/* Reserve BCNT_PATCH_BUF_HEADROOM bytes in front of the image so that
	 * the per-fragment WMT header can be written in place
	 * (wmt_ic_soc.c:2000-2030).
	 */
	patch = kmalloc(file_size + BCNT_PATCH_BUF_HEADROOM, GFP_KERNEL);
	if (!patch) {
		kfree(file);
		return -ENOMEM;
	}

	memcpy(patch + BCNT_PATCH_BUF_HEADROOM, file, file_size);
	kfree(file);

	wmt_dev_patch_put();
	g_wmt.patch_buf = patch;
	g_wmt.patch_size = file_size;
	strscpy(g_wmt.patch_fw_name, name, sizeof(g_wmt.patch_fw_name));

	*buf = patch;
	*size = file_size;
	wmt_dbg("loaded %s (%zu bytes)\n", name, file_size);
	return 0;
}

void wmt_dev_patch_put(void)
{
	kfree(g_wmt.patch_buf);
	g_wmt.patch_buf = NULL;
	g_wmt.patch_size = 0;
	g_wmt.patch_fw_name[0] = '\0';
}

/*
 * Patch list provided by the launcher
 */

int wmt_dev_patch_set_num(u32 num)
{
	struct wmt_patch_info *info;

	if (!num || num > MAX_PATCH_NUM)
		return -EINVAL;

	info = kcalloc(num, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	mutex_lock(&g_wmt.patch_lock);
	kfree(g_wmt.patch_info);
	g_wmt.patch_info = info;
	g_wmt.patch_num = num;
	mutex_unlock(&g_wmt.patch_lock);

	wmt_dbg("patch count set to %u\n", num);
	return 0;
}

int wmt_dev_patch_set_info(const struct wmt_patch_info *info)
{
	u32 seq;

	if (!info)
		return -EINVAL;

	seq = info->download_seq;
	if (!seq || seq > g_wmt.patch_num)
		return -EINVAL;

	mutex_lock(&g_wmt.patch_lock);
	memcpy(&g_wmt.patch_info[seq - 1], info, sizeof(*info));
	g_wmt.patch_info[seq - 1].name[sizeof(info->name) - 1] = '\0';
	mutex_unlock(&g_wmt.patch_lock);

	wmt_dbg("patch %u: %s\n", seq, g_wmt.patch_info[seq - 1].name);
	return 0;
}

u32 wmt_dev_patch_num(void)
{
	return g_wmt.patch_num;
}

int wmt_dev_patch_info_get(u32 seq, u8 *name, u8 addr[4])
{
	int ret = 0;

	if (!seq || seq > g_wmt.patch_num || !name || !addr)
		return -EINVAL;

	mutex_lock(&g_wmt.patch_lock);
	if (!g_wmt.patch_info) {
		ret = -ENOENT;
	} else {
		memcpy(name, g_wmt.patch_info[seq - 1].name,
		       sizeof(g_wmt.patch_info[seq - 1].name));
		memcpy(addr, g_wmt.patch_info[seq - 1].address, 4);
	}
	mutex_unlock(&g_wmt.patch_lock);

	return ret;
}

void wmt_dev_patch_info_free_all(void)
{
	mutex_lock(&g_wmt.patch_lock);
	kfree(g_wmt.patch_info);
	g_wmt.patch_info = NULL;
	g_wmt.patch_num = 0;
	mutex_unlock(&g_wmt.patch_lock);
}

/*
 * Kernel -> launcher command channel (read/poll/write)
 */

void wmt_dev_cmd_request(const char *cmd)
{
	strscpy(g_wmt.cmd, cmd, sizeof(g_wmt.cmd));
	set_bit(WMT_STAT_CMD, &g_wmt.state);
	wake_up_interruptible(&g_wmt.cmd_wq);
	wmt_dbg("command '%s' queued\n", cmd);
}

bool wmt_dev_cmd_pending(void)
{
	return test_bit(WMT_STAT_CMD, &g_wmt.state);
}

const char *wmt_dev_cmd_string(void)
{
	return g_wmt.cmd;
}

void wmt_dev_cmd_ack(int result)
{
	clear_bit(WMT_STAT_CMD, &g_wmt.state);
	if (result)
		wmt_warn("launcher rejected command '%s'\n", g_wmt.cmd);
}

/*
 * STP receive notification (registered on the WMT channel)
 */

void wmt_dev_rx_event_cb(void)
{
	g_wmt.rx_pending = true;
	wake_up_interruptible(&g_wmt.rx_wq);
}

void wmt_dev_rx_clear(void)
{
	g_wmt.rx_pending = false;
}

int wmt_dev_wait_rx(u32 timeout_ms)
{
	int ret;

	ret = wait_event_interruptible_timeout(g_wmt.rx_wq, g_wmt.rx_pending,
					       msecs_to_jiffies(timeout_ms));
	if (ret > 0) {
		g_wmt.rx_pending = false;
		return 0;
	}

	return ret < 0 ? ret : -ETIMEDOUT;
}

/*
 * Character device
 */

static int wmt_open(struct inode *inode, struct file *file)
{
	wmt_dbg("open by pid %d\n", current->pid);
	return 0;
}

static int wmt_release(struct inode *inode, struct file *file)
{
	wmt_dbg("release by pid %d\n", current->pid);
	return 0;
}

static ssize_t wmt_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	const char *cmd;
	size_t len;

	if (!wmt_dev_cmd_pending()) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(g_wmt.cmd_wq,
					     wmt_dev_cmd_pending()))
			return -ERESTARTSYS;
	}

	cmd = wmt_dev_cmd_string();
	len = strnlen(cmd, NAME_MAX);
	if (count < len)
		return -EINVAL;

	if (copy_to_user(buf, cmd, len))
		return -EFAULT;

	return len;
}

static ssize_t wmt_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	char ack[NAME_MAX + 1] = { };
	size_t len = min(count, (size_t)NAME_MAX);

	if (copy_from_user(ack, buf, len))
		return -EFAULT;

	wmt_dev_cmd_ack(strncasecmp(ack, "ok", strlen("ok")) ? -1 : 0);
	return count;
}

static __poll_t wmt_poll(struct file *file, struct poll_table_struct *wait)
{
	__poll_t mask = POLLOUT | POLLWRNORM;

	poll_wait(file, &g_wmt.cmd_wq, wait);
	if (wmt_dev_cmd_pending())
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static long wmt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	char name[NAME_MAX + 1] = { };
	struct wmt_patch_info info;
	u32 chip_id;
	int ret = 0;

	switch (cmd) {
	case WMT_IOCTL_SET_PATCH_NAME:
		if (copy_from_user(name, (void __user *)arg, NAME_MAX))
			return -EFAULT;
		name[NAME_MAX] = '\0';
		wmt_lib_set_patch_name(name);
		break;

	case WMT_IOCTL_SET_STP_MODE:
		ret = wmt_lib_set_hif(arg);
		break;

	case WMT_IOCTL_FUNC_ONOFF_CTRL:
		if (arg & 0x80000000)
			ret = mtk_wcn_wmt_func_on(arg & 0xf) ? 0 : -EIO;
		else
			ret = mtk_wcn_wmt_func_off(arg & 0xf) ? 0 : -EIO;
		break;

	case WMT_IOCTL_GET_CHIP_INFO:
		if (arg > 2)
			return -EINVAL;
		return wmt_lib_get_icinfo(arg);

	case WMT_IOCTL_SET_PATCH_NUM:
		ret = wmt_dev_patch_set_num(arg);
		break;

	case WMT_IOCTL_SET_PATCH_INFO:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
			return -EFAULT;
		ret = wmt_dev_patch_set_info(&info);
		break;

	case WMT_IOCTL_PORT_NAME:
		/* The STP transport is BTIF on this platform; there is no UART
		 * port to (re)name.
		 */
		ret = -EOPNOTSUPP;
		break;

	case WMT_IOCTL_WMT_CFG_NAME:
		if (copy_from_user(name, (void __user *)arg, NAME_MAX))
			return -EFAULT;
		name[NAME_MAX] = '\0';
		ret = wmt_conf_set_cfg_file(name);
		break;

	case WMT_IOCTL_WMT_QUERY_CHIPID:
		chip_id = g_wmt.chip_id;
		if (!chip_id)
			chip_id = mtk_wcn_consys_soc_chipid_get();
		return chip_id;

	case WMT_IOCTL_WMT_TELL_CHIPID:
		if (arg != 0x0321 && arg != 0x0335 && arg != 0x0337)
			return -EINVAL;
		g_wmt.chip_id = arg;
		break;

	default:
		wmt_warn("unknown ioctl 0x%x\n", cmd);
		return -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
/* The Android launcher is a 32-bit process on this 64-bit kernel, so the
 * pointer-sized ioctl numbers differ (wmt_dev.c:63-69).
 */
#define COMPAT_WMT_IOCTL_SET_PATCH_NAME	_IOW(WMT_IOC_MAGIC, 4, compat_uptr_t)
#define COMPAT_WMT_IOCTL_SET_PATCH_INFO	_IOW(WMT_IOC_MAGIC, 15, compat_uptr_t)
#define COMPAT_WMT_IOCTL_PORT_NAME	_IOWR(WMT_IOC_MAGIC, 20, compat_uptr_t)
#define COMPAT_WMT_IOCTL_WMT_CFG_NAME	_IOWR(WMT_IOC_MAGIC, 21, compat_uptr_t)

static long wmt_compat_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case COMPAT_WMT_IOCTL_SET_PATCH_NAME:
		return wmt_ioctl(file, WMT_IOCTL_SET_PATCH_NAME,
				 (unsigned long)compat_ptr(arg));
	case COMPAT_WMT_IOCTL_SET_PATCH_INFO:
		return wmt_ioctl(file, WMT_IOCTL_SET_PATCH_INFO,
				 (unsigned long)compat_ptr(arg));
	case COMPAT_WMT_IOCTL_PORT_NAME:
		return wmt_ioctl(file, WMT_IOCTL_PORT_NAME,
				 (unsigned long)compat_ptr(arg));
	case COMPAT_WMT_IOCTL_WMT_CFG_NAME:
		return wmt_ioctl(file, WMT_IOCTL_WMT_CFG_NAME,
				 (unsigned long)compat_ptr(arg));
	default:
		return wmt_ioctl(file, cmd, arg);
	}
}
#endif

static const struct file_operations wmt_fops = {
	.owner = THIS_MODULE,
	.open = wmt_open,
	.release = wmt_release,
	.read = wmt_read,
	.write = wmt_write,
	.poll = wmt_poll,
	.unlocked_ioctl = wmt_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = wmt_compat_ioctl,
#endif
};

int wmt_dev_stpwmt_register(void)
{
	dev_t dev = MKDEV(wmt_major, 0);
	int ret;

	ret = register_chrdev_region(dev, WMT_DEV_NUM, WMT_DRIVER_NAME);
	if (ret) {
		wmt_err("failed to register the char device (%d)\n", ret);
		return ret;
	}

	cdev_init(&wmt_cdev, &wmt_fops);
	wmt_cdev.owner = THIS_MODULE;

	ret = cdev_add(&wmt_cdev, dev, WMT_DEV_NUM);
	if (ret) {
		wmt_err("failed to add the char device (%d)\n", ret);
		goto err_region;
	}

	wmt_class = class_create("stpwmt");
	if (IS_ERR(wmt_class)) {
		ret = PTR_ERR(wmt_class);
		wmt_class = NULL;
		goto err_cdev;
	}

	if (IS_ERR(device_create(wmt_class, NULL, dev, NULL, "stpwmt"))) {
		ret = -ENODEV;
		goto err_class;
	}

	wmt_info("/dev/stpwmt registered (major %d)\n", wmt_major);
	return 0;

err_class:
	class_destroy(wmt_class);
	wmt_class = NULL;
err_cdev:
	cdev_del(&wmt_cdev);
err_region:
	unregister_chrdev_region(dev, WMT_DEV_NUM);
	return ret;
}

void wmt_dev_stpwmt_unregister(void)
{
	dev_t dev = MKDEV(wmt_major, 0);

	device_destroy(wmt_class, dev);
	class_destroy(wmt_class);
	wmt_class = NULL;
	cdev_del(&wmt_cdev);
	unregister_chrdev_region(dev, WMT_DEV_NUM);
}
