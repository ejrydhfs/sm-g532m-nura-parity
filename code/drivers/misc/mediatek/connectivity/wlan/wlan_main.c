// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 module lifecycle and WMT glue.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/gl_init.c
 *     (initWlan / exitWlan, wlanProbe / wlanRemove)
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/ahb.c
 *     (HifAhbProbe / HifAhbRemove / HifAhbBusCntGet / mtk_wcn_wmt_wlan_reg)
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/gl_rst.c
 *     (whole-chip reset callback)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>
#include <linux/workqueue.h>

#include "mtk_wcn_wmt.h"
#include "wlan_hif.h"
#include "wlan_mac.h"

/* Exactly one connectivity subsystem instance. */
static struct mtk_wlan_hif *mtk_wlan_hif;

/* WMTRSTMSG_*, gl_rst.h:60-66. */
enum mtk_wlan_reset_msg {
	MTK_WLAN_RESET_START = 0,
	MTK_WLAN_RESET_END = 1,
	MTK_WLAN_RESET_END_FAIL = 2,
};

static void mtk_wlan_reset_work(struct work_struct *work)
{
	struct mtk_wlan_hif *hif = container_of(work, struct mtk_wlan_hif,
						reset_work);

	if (hif->reset_failed) {
		dev_err(hif->dev, "chip reset failed, WLAN stays down\n");
		return;
	}

	/*
	 * Same recovery as wifi_reset_end(): let WMT power the connectivity
	 * subsystem again, which calls the probe callback below.
	 */
	if (!hif->was_up)
		return;

	if (!mtk_wcn_wmt_func_on(WMTDRV_WIFI))
		dev_err(hif->dev, "cannot re-open WLAN after chip reset\n");
}

/*
 * Whole-chip reset notifier, ABI of glResetCallback() gl_rst.c:148-189.
 * WMT sends WMTRSTMSG_RESET_START before pulling the subsystem reset and
 * RESET_END once it is powered again; the re-open is deferred because it
 * would re-enter the WMT operation thread.
 */
static void mtk_wlan_reset_cb(enum mtk_wcn_wmt_drv src,
			      enum mtk_wcn_wmt_drv dst,
			      enum mtk_wcn_wmt_msg msg, void *data, u32 len)
{
	struct mtk_wlan_hif *hif = READ_ONCE(mtk_wlan_hif);
	u32 rst;

	if (!hif || msg != WMTMSG_RESET || !data || len < sizeof(rst))
		return;

	rst = *(u32 *)data;
	switch (rst) {
	case MTK_WLAN_RESET_START:
		dev_warn(hif->dev, "whole chip reset start\n");
		hif->was_up = READ_ONCE(hif->up);
		WRITE_ONCE(hif->up, false);
		WRITE_ONCE(hif->resetting, true);
		if (hif->was_up)
			mtk_wlan_hif_set_int_enable(hif, false);
		/*
		 * Same user visible effect as wifi_reset_start(): the media
		 * is disconnected and the interface stops pretending to be
		 * up while the connectivity chip is reset.
		 */
		mtk_wlan_mac_fw_down(hif);
		break;
	case MTK_WLAN_RESET_END:
	case MTK_WLAN_RESET_END_FAIL:
		dev_warn(hif->dev, "whole chip reset %s\n",
			 rst == MTK_WLAN_RESET_END ? "done" : "failed");
		hif->reset_failed = rst == MTK_WLAN_RESET_END_FAIL;
		WRITE_ONCE(hif->resetting, false);
		schedule_work(&hif->reset_work);
		break;
	default:
		break;
	}
}

/* WMT WIFI function on callback: power up the firmware. */
static int mtk_wlan_wmt_probe(void)
{
	struct mtk_wlan_hif *hif = READ_ONCE(mtk_wlan_hif);
	int ret;

	if (!hif)
		return -ENODEV;

	mutex_lock(&hif->state_lock);
	if (READ_ONCE(hif->up)) {
		/* Duplicate function-on, firmware is already running. */
		ret = 0;
	} else {
		ret = mtk_wlan_fw_bringup(hif);
	}
	mutex_unlock(&hif->state_lock);

	/*
	 * Publish or withdraw the netdev.  Done outside &hif->state_lock and
	 * from a work item because registering and unregistering a netdev
	 * takes the RTNL.
	 */
	if (ret)
		mtk_wlan_mac_fw_down(hif);
	else
		mtk_wlan_mac_fw_up(hif);

	return ret;
}

/* WMT WIFI function off callback: quiesce the HIF. */
static int mtk_wlan_wmt_remove(void)
{
	struct mtk_wlan_hif *hif = READ_ONCE(mtk_wlan_hif);

	if (!hif)
		return 0;

	mutex_lock(&hif->state_lock);
	mtk_wlan_hif_stop(hif);
	mutex_unlock(&hif->state_lock);

	mtk_wlan_mac_fw_down(hif);

	return 0;
}

/* Frames handed to the HIF, reported to WMT (ahb.c:1418-1438). */
static int mtk_wlan_wmt_bus_tx_cnt(void)
{
	struct mtk_wlan_hif *hif = READ_ONCE(mtk_wlan_hif);

	return hif ? atomic_read(&hif->tx_count) : 0;
}

static int mtk_wlan_wmt_bus_tx_cnt_clr(void)
{
	struct mtk_wlan_hif *hif = READ_ONCE(mtk_wlan_hif);

	if (hif)
		atomic_set(&hif->tx_count, 0);
	return 0;
}

static const struct mtk_wcn_wmt_wlan_cb mtk_wlan_wmt_cb = {
	.probe = mtk_wlan_wmt_probe,
	.remove = mtk_wlan_wmt_remove,
	.bus_tx_cnt = mtk_wlan_wmt_bus_tx_cnt,
	.bus_tx_cnt_clr = mtk_wlan_wmt_bus_tx_cnt_clr,
};

/*
 * rfkill drives the WMT WIFI function.  Blocking it powers the connectivity
 * subsystem down, which calls mtk_wlan_wmt_remove() and therefore withdraws
 * the netdev as well, so a blocked switch can never leave an interface that
 * looks up while the firmware is gone.  The ROM has no rfkill integration
 * (neither gl_init.c nor gl_kal.c register one); the control channel there is
 * /dev/wmtWifi (wmt_chrdev_wifi.c).  The switch is optional: without
 * CONFIG_RFKILL, WMT remains the only control path, exactly as on the ROM.
 */
#if IS_ENABLED(CONFIG_RFKILL)
static int mtk_wlan_rfkill_set_block(void *data, bool blocked)
{
	struct mtk_wlan_hif *hif = data;

	if (READ_ONCE(hif->resetting))
		return 0;

	if (blocked) {
		if (READ_ONCE(hif->up))
			mtk_wcn_wmt_func_off(WMTDRV_WIFI);
		/* Make sure no interface survives a failed power-off. */
		mtk_wlan_mac_fw_down(hif);
	} else if (!READ_ONCE(hif->up)) {
		if (!mtk_wcn_wmt_func_on(WMTDRV_WIFI)) {
			dev_err(hif->dev, "cannot power WLAN on\n");
			return -EIO;
		}
	}

	return 0;
}

static const struct rfkill_ops mtk_wlan_rfkill_ops = {
	.set_block = mtk_wlan_rfkill_set_block,
};
#endif /* CONFIG_RFKILL */

int mtk_wlan_main_attach(struct mtk_wlan_hif *hif)
{
	int ret;

	WRITE_ONCE(mtk_wlan_hif, hif);

	INIT_WORK(&hif->reset_work, mtk_wlan_reset_work);

	/*
	 * Create the wiphy and the netdev first: the WMT callbacks registered
	 * below may power the firmware up before this function returns.
	 */
	ret = mtk_wlan_mac_attach(hif);
	if (ret) {
		dev_err(hif->dev, "cannot create the 802.11 interface: %d\n",
			ret);
		WRITE_ONCE(mtk_wlan_hif, NULL);
		return ret;
	}

	/* Register the reset notifier first so it covers bring-up failures. */
	ret = mtk_wcn_wmt_msgcb_reg(WMTDRV_WIFI, mtk_wlan_reset_cb);
	if (ret)
		dev_err(hif->dev, "cannot register reset callback: %d\n", ret);
	else
		hif->msgcb_registered = true;

#if IS_ENABLED(CONFIG_RFKILL)
	hif->rfkill = rfkill_alloc("mtk-wlan", hif->dev, RFKILL_TYPE_WLAN,
				   &mtk_wlan_rfkill_ops, hif);
	if (IS_ERR(hif->rfkill)) {
		dev_warn(hif->dev, "cannot allocate rfkill\n");
		hif->rfkill = NULL;
	} else {
		ret = rfkill_register(hif->rfkill);
		if (ret) {
			dev_err(hif->dev, "cannot register rfkill: %d\n", ret);
			rfkill_destroy(hif->rfkill);
			hif->rfkill = NULL;
		}
	}
#endif /* CONFIG_RFKILL */

	if (mtk_wcn_wmt_wlan_reg(&mtk_wlan_wmt_cb)) {
		dev_err(hif->dev, "cannot register with WMT\n");
		if (hif->rfkill) {
			rfkill_unregister(hif->rfkill);
			rfkill_destroy(hif->rfkill);
			hif->rfkill = NULL;
		}
		if (hif->msgcb_registered) {
			mtk_wcn_wmt_msgcb_unreg(WMTDRV_WIFI);
			hif->msgcb_registered = false;
		}
		mtk_wlan_mac_detach(hif);
		WRITE_ONCE(mtk_wlan_hif, NULL);
		return -EIO;
	}

	hif->wmt_registered = true;
	return 0;
}

void mtk_wlan_main_detach(struct mtk_wlan_hif *hif)
{
	/* Stop reset recovery before the callbacks it would use go away. */
	if (hif->msgcb_registered) {
		mtk_wcn_wmt_msgcb_unreg(WMTDRV_WIFI);
		hif->msgcb_registered = false;
	}
	cancel_work_sync(&hif->reset_work);

	if (hif->wmt_registered) {
		/* Drop the function while the connectivity subsystem is alive. */
		if (READ_ONCE(hif->up))
			mtk_wcn_wmt_func_off(WMTDRV_WIFI);
		mtk_wcn_wmt_wlan_unreg();
		hif->wmt_registered = false;
	}

	if (hif->rfkill) {
		rfkill_unregister(hif->rfkill);
		rfkill_destroy(hif->rfkill);
		hif->rfkill = NULL;
	}

	mtk_wlan_mac_detach(hif);

	WRITE_ONCE(mtk_wlan_hif, NULL);
}

/**
 * mtk_wcn_wlan_gen2_init() - load the MediaTek WLAN driver
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wcn_wlan_gen2_init(void)
{
	int ret = mtk_wlan_ahb_driver_register();

	if (ret)
		pr_err("mtk_wcn_wlan: cannot register AHB driver: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(mtk_wcn_wlan_gen2_init);

void mtk_wcn_wlan_gen2_exit(void)
{
	mtk_wlan_ahb_driver_unregister();
}
EXPORT_SYMBOL(mtk_wcn_wlan_gen2_exit);

module_init(mtk_wcn_wlan_gen2_init);
module_exit(mtk_wcn_wlan_gen2_exit);

MODULE_DESCRIPTION("MediaTek WLAN (CONSYS gen2) AHB host interface");
MODULE_LICENSE("GPL");
