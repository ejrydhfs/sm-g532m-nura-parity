// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN /dev/wmtWifi power control channel.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pub/wmt_chrdev_wifi.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The Wi-Fi HAL powers the WCN Wi-Fi function by writing '1' (on) or '0'
 * (off) to /dev/wmtWifi.  The downstream driver also forwarded 'D<n>' debug
 * level changes and P2P/AP mode reconfiguration to the WLAN driver; neither
 * is part of the frozen WMT interface, so Wi-Fi recovery after a WCN reset is
 * left to the WLAN driver's WMTMSG_RESET callback.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "wmt_local.h"

/* wmt_chrdev_wifi.c:39-41 */
#define WIFI_DRIVER_NAME	"wmtWifi"
#define WIFI_DEV_MAJOR		153

/* wmt_chrdev_wifi.c:170-180 */
static int wifi_major = WIFI_DEV_MAJOR;
module_param(wifi_major, uint, 0444);

static struct cdev wifi_cdev;
static struct class *wifi_class;

enum wifi_power_state {
	WIFI_POWER_OFF,
	WIFI_POWER_ON,
};

static enum wifi_power_state wifi_powered = WIFI_POWER_OFF;
static DEFINE_MUTEX(wifi_mutex);

/* wmt_chrdev_wifi.c:185-196 */
#define WMT_CHECK_DO_CHIP_RESET()					\
	do {								\
		wmt_err("Wi-Fi power control failed, asserting CONNSYS\n"); \
		mtk_wcn_wmt_assert(WMTDRV_WIFI, 0x40);			\
	} while (0)

static int wifi_open(struct inode *inode, struct file *file)
{
	wmt_dbg("wmtWifi open by pid %d\n", current->pid);
	return 0;
}

static int wifi_release(struct inode *inode, struct file *file)
{
	wmt_dbg("wmtWifi release by pid %d\n", current->pid);
	return 0;
}

/* wmt_chrdev_wifi.c:349-470 */
static ssize_t wifi_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	char cmd[16] = { };
	size_t len = min(count, sizeof(cmd) - 1);
	ssize_t ret = count;

	if (!len)
		return -EINVAL;

	if (copy_from_user(cmd, buf, len))
		return -EFAULT;

	mutex_lock(&wifi_mutex);

	switch (cmd[0]) {
	case '0':
		if (wifi_powered == WIFI_POWER_OFF) {
			wmt_dbg("Wi-Fi already off\n");
			break;
		}

		if (!mtk_wcn_wmt_func_off(WMTDRV_WIFI)) {
			WMT_CHECK_DO_CHIP_RESET();
			ret = -EIO;
			break;
		}

		wifi_powered = WIFI_POWER_OFF;
		wmt_info("Wi-Fi powered off\n");
		break;

	case '1':
		if (wifi_powered == WIFI_POWER_ON) {
			wmt_dbg("Wi-Fi already on\n");
			break;
		}

		if (!mtk_wcn_wmt_func_on(WMTDRV_WIFI)) {
			WMT_CHECK_DO_CHIP_RESET();
			ret = -EIO;
			break;
		}

		wifi_powered = WIFI_POWER_ON;
		wmt_info("Wi-Fi powered on\n");
		break;

	case 'D':
		/* Dynamic WLAN debug levels are not part of the frozen WMT
		 * interface.
		 */
		ret = -EOPNOTSUPP;
		break;

	default:
		wmt_warn("invalid wmtWifi command '%c'\n", cmd[0]);
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&wifi_mutex);
	return ret;
}

static const struct file_operations wifi_fops = {
	.owner = THIS_MODULE,
	.open = wifi_open,
	.release = wifi_release,
	.write = wifi_write,
};

int mtk_wcn_wmt_wifi_init(void)
{
	dev_t dev = MKDEV(wifi_major, 0);
	int ret;

	ret = register_chrdev_region(dev, 1, WIFI_DRIVER_NAME);
	if (ret) {
		wmt_err("failed to register the wmtWifi device (%d)\n", ret);
		return ret;
	}

	cdev_init(&wifi_cdev, &wifi_fops);
	wifi_cdev.owner = THIS_MODULE;

	ret = cdev_add(&wifi_cdev, dev, 1);
	if (ret)
		goto err_region;

	wifi_class = class_create("wmtWifi");
	if (IS_ERR(wifi_class)) {
		ret = PTR_ERR(wifi_class);
		wifi_class = NULL;
		goto err_cdev;
	}

	if (IS_ERR(device_create(wifi_class, NULL, dev, NULL, "wmtWifi"))) {
		ret = -ENODEV;
		goto err_class;
	}

	wifi_powered = WIFI_POWER_OFF;
	wmt_info("/dev/wmtWifi registered (major %d)\n", wifi_major);
	return 0;

err_class:
	class_destroy(wifi_class);
	wifi_class = NULL;
err_cdev:
	cdev_del(&wifi_cdev);
err_region:
	unregister_chrdev_region(dev, 1);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_wifi_init);

void mtk_wcn_wmt_wifi_exit(void)
{
	dev_t dev = MKDEV(wifi_major, 0);

	device_destroy(wifi_class, dev);
	class_destroy(wifi_class);
	wifi_class = NULL;
	cdev_del(&wifi_cdev);
	unregister_chrdev_region(dev, 1);
	wifi_powered = WIFI_POWER_OFF;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_wifi_exit);
