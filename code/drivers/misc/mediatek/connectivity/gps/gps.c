// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN GPS driver: GNSS character device on top of the STP GPS
 * channel, with the GPS function of the connectivity SoC powered through
 * WMT.
 *
 * The same STP channel is also exposed through the vendor /dev/stpgps
 * character device (stp_chrdev_gps.c).  Both faces share the channel
 * state machine in this file; only one of them may be open at a time.
 *
 * Port from Samsung/MTK downstream connectivity/gps (stp_chrdev_gps.c and
 * gps.c) of the SM-G532M Android 6.0.1 kernel.
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 * Copyright (C) 2004,2005 TomTom BV <http://www.tomtom.com/>
 *
 * The downstream start/stop state machine of gps.c is not carried over: on
 * MT6735/MT6737 the MT3326 power, clock and reset callbacks are NULL
 * (gps.c:164-167, never assigned anywhere in the tree) so mt3326_gps_power()
 * only ever updates a software flag and, at most, a 120 ms delay.  The only
 * observable part of that state machine, the "suspend" notification used to
 * tell the positioning daemon about system suspend, is preserved here as a
 * sysfs attribute on the GNSS device.
 */

#include <linux/device.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/rfkill.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "gps.h"
#include "mtk_wcn_stp.h"
#include "mtk_wcn_wmt.h"

/*
 * Payload of a WMTMSG_RESET notification, see downstream
 * common/conn_soc/include/wmt_exp.h:186-192.  The value is passed as a
 * native enum (4 bytes), see common/conn_soc/core/wmt_lib.c:1423-1435.
 */
#define MTK_WMTRSTMSG_RESET_START	0x0
#define MTK_WMTRSTMSG_RESET_END		0x1
#define MTK_WMTRSTMSG_RESET_END_FAIL	0x2

/* Size of one receive burst; the STP channel is drained without sleeping. */
#define MTK_GPS_RX_CHUNK		1024
/* Bound a single drain to the STP per-channel queue size. */
#define MTK_GPS_RX_CHUNKS		(MTKSTP_BUFFER_SIZE / MTK_GPS_RX_CHUNK)

/*
 * The STP event callback takes no argument (mtk_wcn_stp_event_cb), so the
 * single GPS channel of the connectivity SoC is reached through this pointer.
 */
static struct mtk_gps *mtk_gps_singleton;

/*
 * Wake a /dev/stpgps reader blocked in read().
 *
 * Called from the STP receive path and the reset path, both of which may run
 * in atomic context.
 */
static void mtk_gps_wake_reader(struct mtk_gps *gps)
{
	wake_up_interruptible(&gps->rx_wait);
}

#if IS_ENABLED(CONFIG_GNSS)
static void mtk_gps_set_suspend(struct mtk_gps *gps, bool suspended)
{
	if (gps->suspended == suspended)
		return;

	gps->suspended = suspended;

	/* Downstream wakes the positioning daemon the same way, see gps.c:264-283. */
	sysfs_notify(&gps->gdev->dev.kobj, NULL, "suspend");
}
#else
static void mtk_gps_set_suspend(struct mtk_gps *gps, bool suspended)
{
	gps->suspended = suspended;
}
#endif

/*
 * Drain the STP GPS channel into the GNSS read FIFO.
 *
 * Called from the STP receive path, which may be atomic, so everything here
 * is spinlock protected and non-blocking.
 */
static void mtk_gps_rx_cb(void)
{
	struct mtk_gps *gps = READ_ONCE(mtk_gps_singleton);
	unsigned long flags;
#if IS_ENABLED(CONFIG_GNSS)
	unsigned int dropped = 0;
	int i, n, copied;
#endif

	if (!gps)
		return;

	spin_lock_irqsave(&gps->rx_lock, flags);
	gps->rx_seq++;
#if IS_ENABLED(CONFIG_GNSS)
	if (!gps->rx_enabled || READ_ONCE(gps->owner) != MTK_GPS_OWNER_GNSS)
		goto out;

	/*
	 * The vendor character device reads the STP queue itself, exactly as
	 * downstream stp_chrdev_gps.c:182-247 does; it only needs the wakeup
	 * below.
	 */
	for (i = 0; i < MTK_GPS_RX_CHUNKS; i++) {
		n = mtk_wcn_stp_receive_data(gps->rx_buf, MTK_GPS_RX_CHUNK,
					     GPS_TASK_INDX);
		if (n <= 0)
			break;

		copied = gnss_insert_raw(gps->gdev, gps->rx_buf, n);
		if (copied < n)
			dropped += n - copied;
	}

out:
#endif
	spin_unlock_irqrestore(&gps->rx_lock, flags);

	mtk_gps_wake_reader(gps);

#if IS_ENABLED(CONFIG_GNSS)
	if (dropped)
		dev_warn_ratelimited(gps->dev,
				     "read FIFO full, dropped %u bytes\n",
				     dropped);
#endif
}

/* Reset notifications, see downstream gps_cdev_rst_cb() stp_chrdev_gps.c:354-387. */
static void mtk_gps_wmt_cb(enum mtk_wcn_wmt_drv src, enum mtk_wcn_wmt_drv dst,
			   enum mtk_wcn_wmt_msg msg, void *data, u32 len)
{
	struct mtk_gps *gps = READ_ONCE(mtk_gps_singleton);
	u32 rst_msg;

	if (!gps)
		return;

	if (src != WMTDRV_WMT || dst != WMTDRV_GPS || msg != WMTMSG_RESET)
		return;

	if (!data || len < sizeof(rst_msg))
		return;

	memcpy(&rst_msg, data, sizeof(rst_msg));

	switch (rst_msg) {
	case MTK_WMTRSTMSG_RESET_START:
		WRITE_ONCE(gps->resetting, true);
		break;
	case MTK_WMTRSTMSG_RESET_END:
	case MTK_WMTRSTMSG_RESET_END_FAIL:
		WRITE_ONCE(gps->resetting, false);
		break;
	default:
		break;
	}

	mtk_gps_wake_reader(gps);
}

/*
 * Bring up the GPS function: WMT power, reset notification, STP channel.
 *
 * The order matches downstream GPS_open() (stp_chrdev_gps.c:402-420): the
 * function is turned on first because the WMT handshake is what brings STP
 * up, then STP readiness is checked and the event callback registered.
 *
 * Caller holds gps->lock.
 */
int mtk_gps_channel_on(struct mtk_gps *gps)
{
	unsigned long flags;
	int ret;

	if (gps->channel_on)
		return 0;

	if (READ_ONCE(gps->resetting)) {
		dev_warn(gps->dev, "connectivity SoC is resetting\n");
		return -EPERM;
	}

	if (!mtk_wcn_wmt_func_on(WMTDRV_GPS)) {
		dev_warn(gps->dev, "WMT failed to turn the GPS function on\n");
		return -ENODEV;
	}

	ret = mtk_wcn_wmt_msgcb_reg(WMTDRV_GPS, mtk_gps_wmt_cb);
	if (ret) {
		dev_warn(gps->dev, "failed to register WMT callback: %d\n", ret);
		goto err_func_off;
	}

	if (!mtk_wcn_stp_is_ready()) {
		dev_err(gps->dev, "STP is not ready\n");
		ret = -ENODEV;
		goto err_msgcb_unreg;
	}

	spin_lock_irqsave(&gps->rx_lock, flags);
	gps->rx_enabled = true;
	spin_unlock_irqrestore(&gps->rx_lock, flags);

	ret = mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, mtk_gps_rx_cb);
	if (ret) {
		spin_lock_irqsave(&gps->rx_lock, flags);
		gps->rx_enabled = false;
		spin_unlock_irqrestore(&gps->rx_lock, flags);
		goto err_msgcb_unreg;
	}

	gps->channel_on = true;

	return 0;

err_msgcb_unreg:
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_GPS);
err_func_off:
	if (!mtk_wcn_wmt_func_off(WMTDRV_GPS))
		dev_warn(gps->dev, "WMT failed to turn the GPS function off\n");

	return ret;
}

/*
 * Release the STP channel and the WMT GPS function, see GPS_close()
 * (stp_chrdev_gps.c:459-487).
 *
 * Caller holds gps->lock.
 */
void mtk_gps_channel_off(struct mtk_gps *gps)
{
	unsigned long flags;

	if (!gps->channel_on)
		return;

	spin_lock_irqsave(&gps->rx_lock, flags);
	gps->rx_enabled = false;
	spin_unlock_irqrestore(&gps->rx_lock, flags);

	/* Unregistering also flushes the GPS receive queue. */
	mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, NULL);
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_GPS);

	if (!mtk_wcn_wmt_func_off(WMTDRV_GPS))
		dev_warn(gps->dev, "WMT failed to turn the GPS function off\n");

	gps->channel_on = false;

	/* Blocked readers observe channel_on and retry/report -ENODEV. */
	mtk_gps_wake_reader(gps);
}

/*
 * Claim the single GPS link for @owner and power the function up.
 *
 * The two faces of the link (GNSS and the vendor chardev) are mutually
 * exclusive: opening one while the other is open returns -EBUSY.
 *
 * Caller holds gps->lock.
 */
int mtk_gps_claim(struct mtk_gps *gps, enum mtk_gps_owner owner)
{
	int ret;

	if (gps->owner != MTK_GPS_OWNER_NONE) {
		dev_dbg(gps->dev, "GPS link already owned by %d\n",
			gps->owner);
		return -EBUSY;
	}

	if (rfkill_blocked(gps->rfkill))
		return -ERFKILL;

	ret = mtk_gps_channel_on(gps);
	if (ret)
		return ret;

	gps->owner = owner;

	return 0;
}

/*
 * Release the link if @owner still holds it, powering the GPS function down.
 *
 * Caller holds gps->lock.
 */
void mtk_gps_release(struct mtk_gps *gps, enum mtk_gps_owner owner)
{
	if (gps->owner != owner)
		return;

	mtk_gps_channel_off(gps);
	gps->owner = MTK_GPS_OWNER_NONE;
}

#if IS_ENABLED(CONFIG_GNSS)
static int mtk_gps_open(struct gnss_device *gdev)
{
	struct mtk_gps *gps = gnss_get_drvdata(gdev);
	int ret;

	mutex_lock(&gps->lock);
	ret = mtk_gps_claim(gps, MTK_GPS_OWNER_GNSS);
	mutex_unlock(&gps->lock);

	return ret;
}

static void mtk_gps_close(struct gnss_device *gdev)
{
	struct mtk_gps *gps = gnss_get_drvdata(gdev);

	mutex_lock(&gps->lock);
	mtk_gps_release(gps, MTK_GPS_OWNER_GNSS);
	mutex_unlock(&gps->lock);
}

static int mtk_gps_write_raw(struct gnss_device *gdev, const unsigned char *buf,
			     size_t count)
{
	struct mtk_gps *gps = gnss_get_drvdata(gdev);
	int written;

	/* A whole-chip reset invalidates the outstanding STP transfer. */
	if (READ_ONCE(gps->resetting))
		return -EIO;

	/* The STP channel is torn down while the radio is rfkill blocked. */
	if (!READ_ONCE(gps->channel_on))
		return -ENETDOWN;

	/*
	 * Downstream caps a write at MTKSTP_BUFFER_SIZE and reports -ENOSPC
	 * when STP has no window space (stp_chrdev_gps.c:108-170).
	 */
	if (count > MTKSTP_BUFFER_SIZE)
		count = MTKSTP_BUFFER_SIZE;

	written = mtk_wcn_stp_send_data(buf, count, GPS_TASK_INDX);
	if (written <= 0)
		return -ENOSPC;

	return written;
}

static const struct gnss_operations mtk_gps_gnss_ops = {
	.open = mtk_gps_open,
	.close = mtk_gps_close,
	.write_raw = mtk_gps_write_raw,
};
#endif /* IS_ENABLED(CONFIG_GNSS) */

static int mtk_gps_rfkill_set_block(void *data, bool blocked)
{
	struct mtk_gps *gps = data;
	int ret = 0;

	mutex_lock(&gps->lock);

	if (blocked) {
		mtk_gps_channel_off(gps);
	} else if (gps->owner != MTK_GPS_OWNER_NONE) {
		ret = mtk_gps_channel_on(gps);
	}

	mutex_unlock(&gps->lock);

	return ret;
}

static const struct rfkill_ops mtk_gps_rfkill_ops = {
	.set_block = mtk_gps_rfkill_set_block,
};

#if IS_ENABLED(CONFIG_GNSS)
static ssize_t suspend_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct mtk_gps *gps = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gps->suspended ? 1 : 0);
}

static ssize_t suspend_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mtk_gps *gps = dev_get_drvdata(dev);
	bool suspended;
	int ret;

	ret = kstrtobool(buf, &suspended);
	if (ret)
		return ret;

	mutex_lock(&gps->lock);
	mtk_gps_set_suspend(gps, suspended);
	mutex_unlock(&gps->lock);

	return count;
}

static DEVICE_ATTR_RW(suspend);
#endif

static int mtk_gps_pm_suspend(struct device *dev)
{
	struct mtk_gps *gps = dev_get_drvdata(dev);

	mutex_lock(&gps->lock);
	mtk_gps_set_suspend(gps, true);
	mutex_unlock(&gps->lock);

	return 0;
}

static int mtk_gps_pm_resume(struct device *dev)
{
	struct mtk_gps *gps = dev_get_drvdata(dev);

	mutex_lock(&gps->lock);
	mtk_gps_set_suspend(gps, false);
	mutex_unlock(&gps->lock);

	return 0;
}

static const struct dev_pm_ops mtk_gps_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(mtk_gps_pm_suspend, mtk_gps_pm_resume)
};

static int mtk_gps_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_gps *gps;
	int ret;

	if (READ_ONCE(mtk_gps_singleton))
		return -EEXIST;

	gps = devm_kzalloc(dev, sizeof(*gps), GFP_KERNEL);
	if (!gps)
		return -ENOMEM;

	gps->dev = dev;
	mutex_init(&gps->lock);
	mutex_init(&gps->rd_mtx);
	mutex_init(&gps->wr_mtx);
	spin_lock_init(&gps->rx_lock);
	init_waitqueue_head(&gps->rx_wait);
	platform_set_drvdata(pdev, gps);

	gps->rx_buf = devm_kmalloc(dev, MTK_GPS_RX_CHUNK, GFP_KERNEL);
	gps->rd_buf = devm_kmalloc(dev, MTKSTP_BUFFER_SIZE, GFP_KERNEL);
	gps->wr_buf = devm_kmalloc(dev, MTKSTP_BUFFER_SIZE, GFP_KERNEL);
	if (!gps->rx_buf || !gps->rd_buf || !gps->wr_buf)
		return -ENOMEM;

	gps->efuse_cell = devm_nvmem_cell_get(dev, "gps-efuse");
	if (IS_ERR(gps->efuse_cell)) {
		ret = PTR_ERR(gps->efuse_cell);
		gps->efuse_cell = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		/*
		 * -ENOENT means the board does not describe the efuse word;
		 * COMBO_IOC_D1_EFUSE_GET then reports -EOPNOTSUPP.
		 */
		if (ret != -ENOENT && ret != -ENODEV && ret != -EOPNOTSUPP)
			dev_warn(dev, "no gps-efuse nvmem cell: %d\n", ret);
	}

	WRITE_ONCE(mtk_gps_singleton, gps);

	gps->rfkill = rfkill_alloc("mtk-wcn-gps", dev, RFKILL_TYPE_GPS,
				   &mtk_gps_rfkill_ops, gps);
	if (!gps->rfkill) {
		ret = -ENOMEM;
		goto err_singleton;
	}

	ret = rfkill_register(gps->rfkill);
	if (ret)
		goto err_rfkill_registered;

#if IS_ENABLED(CONFIG_GNSS)
	gps->gdev = gnss_allocate_device(dev);
	if (!gps->gdev) {
		ret = -ENOMEM;
		goto err_rfkill_registered;
	}

	gps->gdev->type = GNSS_TYPE_MTK;
	gps->gdev->ops = &mtk_gps_gnss_ops;
	gnss_set_drvdata(gps->gdev, gps);

	ret = gnss_register_device(gps->gdev);
	if (ret)
		goto err_gnss_put;

	ret = device_create_file(&gps->gdev->dev, &dev_attr_suspend);
	if (ret)
		goto err_gnss_dereg;
#endif

	ret = mtk_gps_vendor_init(gps);
	if (ret)
		goto err_suspend_file;

	dev_info(dev, "MediaTek WCN GPS on STP channel %d\n", GPS_TASK_INDX);

	return 0;

err_suspend_file:
#if IS_ENABLED(CONFIG_GNSS)
	device_remove_file(&gps->gdev->dev, &dev_attr_suspend);
err_gnss_dereg:
	gnss_deregister_device(gps->gdev);
err_gnss_put:
	gnss_put_device(gps->gdev);
#endif
err_rfkill_registered:
#if IS_ENABLED(CONFIG_GNSS)
	rfkill_unregister(gps->rfkill);
#endif
	rfkill_destroy(gps->rfkill);
err_singleton:
	WRITE_ONCE(mtk_gps_singleton, NULL);

	return ret;
}

static void mtk_gps_remove(struct platform_device *pdev)
{
	struct mtk_gps *gps = platform_get_drvdata(pdev);
	unsigned long flags;

	mtk_gps_vendor_exit(gps);

#if IS_ENABLED(CONFIG_GNSS)
	device_remove_file(&gps->gdev->dev, &dev_attr_suspend);

	/* Closing an open device releases the STP channel and WMT function. */
	gnss_deregister_device(gps->gdev);
#endif

	/* Wait for an STP callback which is already running. */
	spin_lock_irqsave(&gps->rx_lock, flags);
	gps->rx_enabled = false;
	spin_unlock_irqrestore(&gps->rx_lock, flags);

	WRITE_ONCE(mtk_gps_singleton, NULL);

	rfkill_unregister(gps->rfkill);
	rfkill_destroy(gps->rfkill);
#if IS_ENABLED(CONFIG_GNSS)
	gnss_put_device(gps->gdev);
#endif
}

static const struct of_device_id mtk_gps_of_match[] = {
	{ .compatible = "mediatek,mt3326-gps" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_gps_of_match);

static struct platform_driver mtk_gps_driver = {
	.probe = mtk_gps_probe,
	.remove = mtk_gps_remove,
	.driver = {
		.name = "mt3326-gps",
		.of_match_table = mtk_gps_of_match,
		.pm = &mtk_gps_pm_ops,
	},
};
module_platform_driver(mtk_gps_driver);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek WCN GPS driver (STP channel, WMT GPS function)");
MODULE_LICENSE("GPL");
