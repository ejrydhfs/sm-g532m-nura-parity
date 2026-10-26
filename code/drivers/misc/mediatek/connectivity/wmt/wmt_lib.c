// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT library: driver state, configuration and recovery.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_lib.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/wmt_exp.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The WMT protocol is strictly request/event, so all exchanges are executed
 * one at a time under g_wmt.op_lock.  Downstream serialised them through the
 * mtk_wmtd kernel thread; with the STP power-saving engine not ported a
 * mutex around the command engine has the same ordering guarantees and
 * removes the extra thread.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "wmt_local.h"
#include "stp_psm.h"

struct wmt_dev g_wmt;

static struct work_struct wmt_reset_work;

static void wmt_lib_reset_work_fn(struct work_struct *work)
{
	wmt_lib_cmb_rst(WMTRSTSRC_RESET_STP);
}

void wmt_lib_schedule_reset(enum wmt_rst_src src)
{
	wmt_dbg("scheduling CONSYS reset (source %d)\n", src);
	schedule_work(&wmt_reset_work);
}

/* wmt_lib.c:1417-1443 */
void wmt_lib_notify_reset(enum wmt_rst_msg msg)
{
	int i;

	for (i = 0; i < WMTDRV_MAX; i++) {
		if (!g_wmt.msgcb[i])
			continue;

		g_wmt.msgcb[i](WMTDRV_WMT, i, WMTMSG_RESET, &msg,
			       sizeof(msg));
		wmt_dbg("reset message sent to driver %d\n", i);
	}
}

/* Downstream wmt_lib_ps_irq_cb() (wmt_lib.c:799-810): the BGF wakeup line
 * fired, so the MCU has data for the AP while it was allowed to sleep.
 */
void wmt_lib_ps_irq_cb(void)
{
	mtk_wcn_stp_psm_notify_stp(MTK_WCN_STP_PSM_EIRQ);
}

/* wmt_lib.c:1617-1712 */
bool wmt_lib_cmb_rst(enum wmt_rst_src src)
{
	struct wmt_op op = { };
	bool ok = false;
	int retries = 10;
	int ret;

	if (test_and_set_bit(WMT_STAT_RST_ON, &g_wmt.state)) {
		wmt_info("reset already in progress\n");
		return false;
	}

	mtk_wcn_stp_enable(0);
	wmt_lib_notify_reset(WMTRSTMSG_RESET_START);

	while (retries--) {
		op.op_id = WMT_OPID_HW_RST;
		ret = wmt_lib_run_op(&op);
		if (ret) {
			wmt_err("hardware reset failed (%d)\n", ret);
			continue;
		}

		op.op_id = WMT_OPID_SW_RST;
		ret = wmt_lib_run_op(&op);
		if (ret) {
			wmt_err("software reset failed (%d)\n", ret);
			continue;
		}

		ok = true;
		break;
	}

	clear_bit(WMT_STAT_RST_ON, &g_wmt.state);
	mtk_wcn_stp_set_wmt_evt_err_trg_assert(0);

	if (ok) {
		wmt_info("CONSYS reset succeeded\n");
		wmt_lib_notify_reset(WMTRSTMSG_RESET_END);
		/* Restore the power-saving state the user asked for; the
		 * link was torn down and rebuilt above (psm_core.c:769-820
		 * resets to "disabled").
		 */
		stp_psm_reapply();
	} else {
		wmt_err("CONSYS reset failed\n");
		wmt_lib_notify_reset(WMTRSTMSG_RESET_END_FAIL);
	}

	return ok;
}

int wmt_lib_run_op(struct wmt_op *op)
{
	int ret;

	if (!op)
		return -EINVAL;

	mutex_lock(&g_wmt.op_lock);
	ret = wmt_core_opid(op);
	mutex_unlock(&g_wmt.op_lock);

	return ret;
}

int wmt_lib_init(void)
{
	int ret;

	memset(&g_wmt, 0, sizeof(g_wmt));
	mutex_init(&g_wmt.op_lock);
	init_waitqueue_head(&g_wmt.cmd_wq);
	init_waitqueue_head(&g_wmt.rx_wq);

	strscpy(g_wmt.patch_name, WMT_DEFAULT_PATCH_NAME,
		sizeof(g_wmt.patch_name));
	strscpy(g_wmt.cfg_name, WMT_CFG_PREFIX WMT_CFG_NAME,
		sizeof(g_wmt.cfg_name));

	g_wmt.hif.hif_type = WMT_HIF_BTIF;
	g_wmt.hif.strap_conf[0] = WMT_FM_COMM;
	g_wmt.e_hw_ver = WMTHWVER_INVALID;

	INIT_WORK(&wmt_reset_work, wmt_lib_reset_work_fn);

	ret = wmt_conf_read_file();
	if (ret)
		wmt_warn("no usable %s, using defaults\n", g_wmt.cfg_name);

	wmt_core_init();

	ret = wmt_plat_init();
	if (ret) {
		wmt_err("platform init failed (%d)\n", ret);
		wmt_lib_deinit();
		return ret;
	}

	/* Downstream initialises the LTE IDC bridge in wmt_lib_init()
	 * (wmt_lib.c:283) and the BGW desense socket with the WMT device
	 * (wmt_dev.c:2401).  A missing netlink socket must not take the
	 * whole connectivity stack down, so the failure is only logged.
	 */
	ret = wmt_coex_init();
	if (ret)
		wmt_warn("coexistence init failed (%d)\n", ret);

	g_wmt.ready = true;

	wmt_info("WMT driver initialised\n");
	return 0;
}

void wmt_lib_deinit(void)
{
	struct wmt_op op = { };
	int i;

	cancel_work_sync(&wmt_reset_work);

	/* Tear the firmware down before releasing the platform resources so
	 * that the BTIF channel is closed while the transport still exists.
	 */
	for (i = 0; i < WMTDRV_WMT; i++) {
		if (g_wmt.drv_status[i] != DRV_STS_FUNC_ON)
			continue;
		op.op_id = WMT_OPID_FUNC_OFF;
		op.data[0] = i;
		wmt_lib_run_op(&op);
	}

	if (g_wmt.drv_status[WMTDRV_WMT] != DRV_STS_POWER_OFF) {
		op.op_id = WMT_OPID_PWR_OFF;
		memset(op.data, 0, sizeof(op.data));
		wmt_lib_run_op(&op);
	}

	wmt_coex_deinit();
	wmt_plat_deinit();
	wmt_core_deinit();
	kfree(g_wmt.patch_info);
	g_wmt.patch_info = NULL;
	wmt_dev_patch_put();
	g_wmt.ready = false;
	mutex_destroy(&g_wmt.op_lock);
}

struct wmt_hif_conf *wmt_lib_hif(void)
{
	return &g_wmt.hif;
}

struct wmt_gen_conf *wmt_lib_gen_conf(void)
{
	return &g_wmt.gen_conf;
}

/* wmt_lib.c:393-429 */
int wmt_lib_set_hif(unsigned long cfg)
{
	u32 val = cfg & 0xf;

	switch (val) {
	case STP_BTIF_FULL:
		g_wmt.hif.hif_type = WMT_HIF_BTIF;
		break;
	case STP_SDIO:
	case STP_UART_FULL:
	case STP_UART_MAND:
		wmt_err("HIF mode 0x%x is not supported on this platform\n",
			val);
		return -EOPNOTSUPP;
	default:
		wmt_err("invalid STP mode 0x%x\n", val);
		return -EINVAL;
	}

	val = (cfg & 0xf0) >> 4;
	if (val == WMT_FM_COMM || val == WMT_FM_I2C)
		g_wmt.hif.strap_conf[0] = val;
	else
		return -EINVAL;

	wmt_info("HIF mode %u, FM interface %u\n", g_wmt.hif.hif_type,
		 g_wmt.hif.strap_conf[0]);
	return 0;
}

void wmt_lib_set_patch_name(const char *name)
{
	if (!name)
		return;

	strscpy(g_wmt.patch_name, name, sizeof(g_wmt.patch_name));
}

u32 wmt_lib_get_icinfo(enum wmt_chipinfo_type type)
{
	switch (type) {
	case WMTCHIN_CHIPID:
		return g_wmt.chip_id;
	case WMTCHIN_HWVER:
		return g_wmt.hw_ver;
	case WMTCHIN_MAPPINGHWVER:
		return g_wmt.e_hw_ver;
	case WMTCHIN_FWVER:
		return g_wmt.fw_ver;
	default:
		return 0;
	}
}

enum mtk_wcn_wmt_hwver wmt_lib_hwver(void)
{
	return g_wmt.e_hw_ver;
}

void wmt_lib_set_chip_info(u32 chip_id, u32 hw_ver, u32 fw_ver,
			   enum mtk_wcn_wmt_hwver e_hw_ver)
{
	g_wmt.chip_id = chip_id;
	g_wmt.hw_ver = hw_ver;
	g_wmt.fw_ver = fw_ver;
	g_wmt.e_hw_ver = e_hw_ver;
}

u32 wmt_lib_co_clock(void)
{
	return g_wmt.gen_conf.co_clock_flag & 0x0f ? 1 : 0;
}
