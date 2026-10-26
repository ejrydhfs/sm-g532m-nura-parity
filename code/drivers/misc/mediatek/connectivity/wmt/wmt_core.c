// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT control protocol engine.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_core.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_func.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * WMT is the management protocol spoken with the connectivity firmware over
 * the STP WMT channel.  Every exchange is a command PDU followed by an event
 * PDU:
 *
 *	command: type=1, opcode, len, params...
 *	event:   type=2, opcode, len, status, params...
 *
 * All exchanges are serialised by wmt_lib_run_op() and complete when the
 * event has been received (or the 2000 ms receive timeout expires).
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>

#include <mtk_wcn_stp.h>

#include "wmt_local.h"

/* WMT command/event PDUs (wmt_core.c:203-259, wmt_ic_soc.c:100-160). */
static const u8 wmt_therm_cmd[] = { 0x01, 0x11, 0x01, 0x00, 0x00 };
static const u8 wmt_therm_ctrl_evt[] = { 0x02, 0x11, 0x01, 0x00, 0x00 };
static const u8 wmt_therm_read_evt[] = { 0x02, 0x11, 0x02, 0x00, 0x00, 0x00 };
static const u8 wmt_dsns_evt[] = { 0x02, 0x0e, 0x01, 0x00, 0x00 };
static const u8 wmt_assert_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x08 };

/* Power-saving PDUs (wmt_core.c:143-150).  WMT_WAKEUP_CMD is the BTIF
 * out-of-band wakeup, issued through WMT_CTRL_SOC_WAKEUP_CONSYS, so only the
 * event is compared.
 */
static const u8 wmt_sleep_cmd[] = { 0x01, 0x03, 0x01, 0x00, 0x01 };
static const u8 wmt_sleep_evt[] = { 0x02, 0x03, 0x02, 0x00, 0x00, 0x01 };
static const u8 wmt_host_awake_cmd[] = { 0x01, 0x03, 0x01, 0x00, 0x02 };
static const u8 wmt_host_awake_evt[] = {
	0x02, 0x03, 0x02, 0x00, 0x00, 0x02,
};
static const u8 wmt_wakeup_evt[] = { 0x02, 0x03, 0x02, 0x00, 0x00, 0x03 };

/* Test command PDUs (wmt_core.c:1565-1600).  The assert, exception and
 * no-ack triggers are fire-and-forget: the firmware never answers them
 * (wmt_core.c:1660-1670), so no expected event is defined for them.
 */
static const u8 wmt_test_assert_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x08 };
static const u8 wmt_test_exception_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x09 };
static const u8 wmt_test_noack_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x0a };
static const u8 wmt_test_warnrst_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x0b };
static const u8 wmt_test_warnrst_evt[] = { 0x02, 0x02, 0x00, 0x00, 0x00 };
static const u8 wmt_test_fwlog_cmd[] = { 0x01, 0x02, 0x01, 0x00, 0x0c };
static const u8 wmt_test_fwlog_evt[] = { 0x02, 0x02, 0x00, 0x00, 0x00 };
static const u8 wmt_test_coexdbg_cmd[] = {
	0x01, 0x10, 0x02, 0x00, 0x08, 0x00,
};

/* BGW desense forward (wmt_core.c:1972-1979). */
static const u8 wmt_bgw_ds_cmd[] = {
	0x01, 0x0e, 0x0f, 0x00,
	0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00,
};
static const u8 wmt_bgw_ds_evt[] = { 0x02, 0x0e, 0x01, 0x00, 0x00 };

/* LTE IDC command/event (wmt_core.c:2174-2177). */
static const u8 wmt_idc_evt[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
#define WMT_IDC_MAX_PAYLOAD	1200

/* WMT_SET_REG_CMD / WR_EVT / RD_EVT (wmt_core.c:236-258) */
static u8 wmt_set_reg_cmd[] = {
	0x01, 0x08, 0x10, 0x00,	/* length */
	0x00,			/* op: w(1) & r(2) */
	0x01,			/* type: reg */
	0x00,			/* reserved */
	0x01,			/* one register */
	0x00, 0x00, 0x00, 0x00,	/* address */
	0x00, 0x00, 0x00, 0x00,	/* value */
	0xff, 0xff, 0xff, 0xff,	/* mask */
};
static const u8 wmt_set_reg_wr_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const u8 wmt_set_reg_rd_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00,	/* address */
	0x00, 0x00, 0x00, 0x00,	/* value */
};

static int opfunc_pwr_sv(struct wmt_op *op);
static int opfunc_cmd_test(struct wmt_op *op);
static int opfunc_bgw_ds(struct wmt_op *op);
static int opfunc_idc_msg_handling(struct wmt_op *op);
static int wmt_core_stp_init(void);
static int wmt_core_stp_deinit(void);
static int wmt_core_hw_check(void);
static void wmt_core_bgw_desense_update(void);

/*
 * Low level WMT transport helpers
 */

int wmt_core_tx(const u8 *data, u32 size, u32 *written)
{
	u32 retry = 0;
	int ret;

	*written = 0;

	ret = wmt_ctrl_tx_ex(data, size, written, false);
	while (ret == -EAGAIN && retry < 3) {
		wmt_warn("no STP window space, retry %u\n", retry + 1);
		msleep(360);
		ret = wmt_ctrl_tx_ex(data, size, written, false);
		retry++;
	}

	return ret;
}

int wmt_core_rx(u8 *buf, u32 len, u32 *read_size)
{
	struct wmt_ctrl_data data = { };

	if (read_size)
		*read_size = 0;

	data.ctrl_id = WMT_CTRL_RX;
	data.data[0] = (unsigned long)buf;
	data.data[1] = len;
	data.data[2] = (unsigned long)read_size;

	return wmt_ctrl(&data);
}

/* wmt_core.c:428-472 */
int wmt_core_ctrl(enum wmt_ctrl_id id, unsigned long *pa1, unsigned long *pa2)
{
	struct wmt_ctrl_data data = { };
	int ret;

	data.ctrl_id = id;
	data.data[0] = pa1 ? *pa1 : 0;
	data.data[1] = pa2 ? *pa2 : 0;

	ret = wmt_ctrl(&data);
	if (ret) {
		wmt_err("ctrl %d failed (%d)\n", id, ret);
		return ret;
	}

	if (pa1)
		*pa1 = data.data[0];
	if (pa2)
		*pa2 = data.data[1];

	return 0;
}

/* wmt_core.c:376-426 */
int wmt_core_func_ctrl_cmd(enum mtk_wcn_wmt_drv type, bool enable)
{
	u8 cmd[WMT_HDR_LEN + WMT_FLAG_LEN + WMT_FUNC_CTRL_PARAM_LEN];
	u8 evt[WMT_HDR_LEN + WMT_STS_LEN];
	u32 cmd_len = sizeof(cmd);
	u32 evt_len = sizeof(evt);
	u32 written = 0, read = 0;
	int ret;

	cmd[0] = 0x01;			/* PKT_TYPE_CMD */
	cmd[1] = 0x06;			/* OPCODE_FUNC_CTRL */
	cmd[2] = WMT_FLAG_LEN + WMT_FUNC_CTRL_PARAM_LEN;
	cmd[3] = 0x00;
	cmd[4] = (u8)type;
	cmd[5] = enable ? 1 : 0;

	ret = wmt_core_tx(cmd, cmd_len, &written);
	if (ret || written != cmd_len) {
		wmt_err("func ctrl tx failed (%d, %u/%u)\n", ret, written,
			cmd_len);
		return -EIO;
	}

	ret = wmt_core_rx(evt, evt_len, &read);
	if (ret || read != evt_len) {
		wmt_err("func ctrl rx failed (%d, %u/%u)\n", ret, read, evt_len);
		return -EIO;
	}

	if (evt[0] != 0x02 /* PKT_TYPE_EVENT */ ||
	    evt[1] != cmd[1] || evt[4] != 0) {
		wmt_err("bad func ctrl event %02x %02x %02x %02x %02x\n",
			evt[0], evt[1], evt[2], evt[3], evt[4]);
		return -EIO;
	}

	return 0;
}

/* wmt_core.c:500-562 */
int wmt_core_reg_rw_raw(u32 is_write, u32 offset, u32 *val, u32 mask)
{
	u8 evt[16] = { };
	u32 evt_len, written = 0, read = 0;
	const u8 *exp;
	u32 tx_addr, rx_addr;
	int ret;

	wmt_set_reg_cmd[4] = is_write ? 0x1 : 0x2;
	memcpy(&wmt_set_reg_cmd[8], &offset, 4);
	memcpy(&wmt_set_reg_cmd[12], val, 4);
	memcpy(&wmt_set_reg_cmd[16], &mask, 4);

	ret = wmt_core_tx(wmt_set_reg_cmd, sizeof(wmt_set_reg_cmd), &written);
	if (ret || written != sizeof(wmt_set_reg_cmd)) {
		wmt_err("reg rw tx failed (%d, %u)\n", ret, written);
		return -EIO;
	}

	if (is_write) {
		exp = wmt_set_reg_wr_evt;
		evt_len = sizeof(wmt_set_reg_wr_evt);
	} else {
		exp = wmt_set_reg_rd_evt;
		evt_len = sizeof(wmt_set_reg_rd_evt);
	}

	ret = wmt_core_rx(evt, evt_len, &read);
	if (ret || read != evt_len) {
		wmt_err("reg rw rx failed (%d, %u/%u)\n", ret, read, evt_len);
		return -EIO;
	}

	/* Event type/opcode must echo the command. */
	if (evt[0] != exp[0] || evt[1] != exp[1]) {
		wmt_err("bad register event %02x %02x\n", evt[0], evt[1]);
		return -EIO;
	}

	if (is_write)
		return 0;

	memcpy(&tx_addr, &wmt_set_reg_cmd[8], 4);
	memcpy(&rx_addr, &evt[8], 4);
	if (tx_addr != rx_addr) {
		wmt_err("read address mismatch 0x%08x/0x%08x\n",
			rx_addr, tx_addr);
		return -EIO;
	}

	memcpy(val, &evt[12], 4);
	return 0;
}

/* wmt_core.c:598-644 */
int wmt_core_init_script(const struct wmt_init_script *script, int count)
{
	u8 evt[256];
	u32 written = 0, read = 0;
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = wmt_core_tx(script[i].cmd, script[i].cmd_sz, &written);
		if (ret || written != script[i].cmd_sz) {
			wmt_err("init script %s tx failed (%d, %u/%u)\n",
				script[i].str, ret, written, script[i].cmd_sz);
			return -EIO;
		}

		memset(evt, 0, sizeof(evt));
		ret = wmt_core_rx(evt, script[i].evt_sz, &read);
		if (ret || read != script[i].evt_sz) {
			wmt_err("init script %s rx failed (%d, %u/%u)\n",
				script[i].str, ret, read, script[i].evt_sz);
			return -EIO;
		}

		/* RF calibration events carry chip data; their contents are
		 * not compared (wmt_core.c:629-640).
		 */
		if (script[i].evt_sz > 1 && evt[1] == 0x14)
			continue;

		if (memcmp(evt, script[i].evt, script[i].evt_sz)) {
			wmt_err("init script %s unexpected event\n",
				script[i].str);
			return -EIO;
		}
	}

	return 0;
}

/* wmt_core.c:660-667 */
bool wmt_core_patch_check(u32 patch_ver, u32 hw_ver)
{
	if (MAJORNUM(hw_ver) != MAJORNUM(patch_ver)) {
		wmt_err("hw version 0x%x does not match patch 0x%x\n",
			hw_ver, patch_ver);
		return false;
	}

	return true;
}

/*
 * STP bring-up / tear-down around the WMT protocol
 */

/* wmt_core.c:700-790 */
static int wmt_core_stp_init(void)
{
	struct wmt_hif_conf *hif = wmt_lib_hif();
	unsigned long pa1 = 0, pa2 = 0;
	int ret;

	if (!wmt_conf_get())
		wmt_dbg("no WMT_SOC.cfg, using defaults\n");

	if (!(g_wmt.state & WMT_STAT_STP_OPEN)) {
		ret = wmt_core_ctrl(WMT_CTRL_STP_OPEN, &pa1, &pa2);
		if (ret)
			return ret;
	}

	/* Disable and un-ready STP while the firmware is reconfigured. */
	pa1 = WMT_STP_CONF_EN;
	pa2 = 0;
	ret = wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
	pa1 = WMT_STP_CONF_RDY;
	pa2 = 0;
	ret += wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);

	/* The firmware always starts in mandatory mode. */
	pa1 = WMT_STP_CONF_MODE;
	pa2 = MTKSTP_BTIF_MAND_MODE;
	ret += wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);

	pa1 = WMT_STP_CONF_EN;
	pa2 = 1;
	ret += wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
	if (ret)
		return -EIO;

	ret = wmt_core_hw_check();
	if (ret)
		return ret;

	wmt_ic_soc_co_clock_ctrl(wmt_lib_co_clock());

	ret = wmt_ic_soc_sw_init(hif);
	if (ret) {
		wmt_err("SOC software init failed (%d)\n", ret);
		return ret;
	}

	pa1 = WMT_STP_CONF_RDY;
	pa2 = 1;
	return wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
}

/* wmt_core.c:792-845 */
static int wmt_core_stp_deinit(void)
{
	struct wmt_hif_conf *hif = wmt_lib_hif();
	unsigned long pa1, pa2;
	int ret;

	wmt_ic_soc_sw_deinit(hif);

	pa1 = WMT_STP_CONF_RDY;
	pa2 = 0;
	ret = wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
	pa1 = WMT_STP_CONF_EN;
	pa2 = 0;
	ret += wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
	pa1 = 0;
	pa2 = 0;
	ret += wmt_core_ctrl(WMT_CTRL_STP_CLOSE, &pa1, &pa2);

	return ret;
}

/*
 * Chip identification (wmt_core.c:861-940)
 */
static int wmt_core_hw_check(void)
{
	u32 chip_id = 0;
	int ret;

	ret = wmt_core_reg_rw_raw(false, GEN_HCR, &chip_id, GEN_HCR_MASK);
	if (ret) {
		wmt_err("failed to read chip id (%d)\n", ret);
		return ret;
	}

	wmt_info("chip id 0x%x\n", chip_id);

	switch (chip_id) {
	case 0x6572:
	case 0x6582:
	case 0x6592:
	case 0x8127:
	case 0x6571:
	case 0x6752:
	case 0x0279:
	case 0x0326:
	case 0x0321:
	case 0x0335:
	case 0x0337:
	case 0x8163:
	case 0x6580:
		break;
	default:
		wmt_err("unsupported chip id 0x%x\n", chip_id);
		return -ENODEV;
	}

	g_wmt.chip_id = chip_id;

	ret = wmt_ic_soc_ver_check();
	if (ret) {
		wmt_err("chip 0x%x version check failed (%d)\n", chip_id, ret);
		return ret;
	}

	return 0;
}

/*
 * Operation handlers
 */

static int opfunc_hif_conf(struct wmt_op *op)
{
	if (!(op->info_bit & BIT(0)))
		return -EINVAL;

	memcpy(&g_wmt.hif, &op->data[0], sizeof(g_wmt.hif));
	return 0;
}

/* wmt_core.c:947-1029 */
static int opfunc_pwr_on(struct wmt_op *op)
{
	unsigned long pa1 = 0, pa2 = 0;
	int attempt, ret;

	if (g_wmt.drv_status[WMTDRV_WMT] != DRV_STS_POWER_OFF) {
		wmt_err("WMT already powered on (status %d)\n",
			g_wmt.drv_status[WMTDRV_WMT]);
		return -EALREADY;
	}

	if (op->info_bit & BIT(0))
		opfunc_hif_conf(op);

	for (attempt = 0; attempt <= WMT_PWRON_RTY_DFT; attempt++) {
		ret = wmt_core_ctrl(WMT_CTRL_HW_PWR_ON, &pa1, &pa2);
		if (ret) {
			wmt_err("CONSYS power on failed (%d)\n", ret);
			continue;
		}

		g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_ON;

		ret = wmt_core_stp_init();
		if (!ret) {
			g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_FUNC_ON;
			return 0;
		}

		wmt_err("STP init failed (%d), retry %d\n", ret, attempt);
		wmt_core_stp_deinit();
		pa1 = 0;
		pa2 = 0;
		wmt_core_ctrl(WMT_CTRL_HW_PWR_OFF, &pa1, &pa2);
		g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_OFF;
	}

	return ret;
}

/* wmt_core.c:1031-1075 */
static int opfunc_pwr_off(struct wmt_op *op)
{
	unsigned long pa1, pa2;
	int ret = 0;

	if (g_wmt.drv_status[WMTDRV_WMT] == DRV_STS_POWER_OFF) {
		wmt_warn("WMT already powered off\n");
		return -EALREADY;
	}

	if (g_wmt.drv_status[WMTDRV_WMT] == DRV_STS_FUNC_ON)
		ret = wmt_core_stp_deinit();

	g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_ON;
	pa1 = 0;
	pa2 = 0;
	ret |= wmt_core_ctrl(WMT_CTRL_HW_PWR_OFF, &pa1, &pa2);
	g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_OFF;

	return ret;
}

static bool wmt_all_funcs_off(void)
{
	int i;

	for (i = 0; i < WMTDRV_WMT; i++)
		if (g_wmt.drv_status[i] != DRV_STS_POWER_OFF)
			return false;

	return true;
}

/* wmt_core.c:1077-1170 */
/* Downstream wmt_func.c:302-340, 510-545 and 668-710: the BGW desense
 * daemon is asked to start streaming desense commands while GPS coexists
 * with BT or Wi-Fi, and to stop when that pairing disappears.
 */
static void wmt_core_bgw_desense_update(void)
{
	bool gps = g_wmt.drv_status[WMTDRV_GPS] == DRV_STS_FUNC_ON;
	bool bt_wifi = g_wmt.drv_status[WMTDRV_BT] == DRV_STS_FUNC_ON ||
		       g_wmt.drv_status[WMTDRV_WIFI] == DRV_STS_FUNC_ON;

	wmt_bgw_send_cmd((gps && bt_wifi) ? WMT_BGW_DS_CMD_ON :
					   WMT_BGW_DS_CMD_OFF);
}

static int opfunc_func_on(struct wmt_op *op)
{
	unsigned int type = op->data[0];
	int ret;

	if (type >= WMTDRV_MAX)
		return -EINVAL;

	if (g_wmt.drv_status[type] == DRV_STS_FUNC_ON) {
		wmt_warn("function %u already on\n", type);
		return 0;
	}

	if (g_wmt.drv_status[WMTDRV_WMT] != DRV_STS_FUNC_ON) {
		ret = opfunc_pwr_on(op);
		if (ret)
			return ret;
	}

	if (type < WMTDRV_WMT) {
		ret = wmt_func_on(type);
		if (ret) {
			wmt_err("function %u on failed (%d)\n", type, ret);
			g_wmt.drv_status[type] = DRV_STS_POWER_OFF;
			if (wmt_all_funcs_off()) {
				mtk_wcn_wmt_system_state_reset();
				opfunc_pwr_off(op);
			}
			return ret;
		}
	}

	g_wmt.drv_status[type] = DRV_STS_FUNC_ON;
	if (type == WMTDRV_BT || type == WMTDRV_GPS ||
	    type == WMTDRV_WIFI)
		wmt_core_bgw_desense_update();
	return 0;
}

/* wmt_core.c:1172-1265 */
static int opfunc_func_off(struct wmt_op *op)
{
	unsigned int type = op->data[0];
	int ret = 0;

	if (type >= WMTDRV_MAX)
		return -EINVAL;

	if (g_wmt.drv_status[type] != DRV_STS_FUNC_ON) {
		wmt_warn("function %u is not on\n", type);
		return 0;
	}

	if (type < WMTDRV_WMT)
		ret = wmt_func_off(type);

	g_wmt.drv_status[type] = DRV_STS_POWER_OFF;
	if (type == WMTDRV_BT || type == WMTDRV_GPS ||
	    type == WMTDRV_WIFI)
		wmt_core_bgw_desense_update();

	if (wmt_all_funcs_off())
		ret |= opfunc_pwr_off(op);

	return ret;
}

static int opfunc_reg_rw(struct wmt_op *op)
{
	if (g_wmt.drv_status[WMTDRV_WMT] != DRV_STS_FUNC_ON)
		return -EPERM;

	return wmt_core_reg_rw_raw(op->data[0], op->data[1],
				   (u32 *)op->data[2], op->data[3]);
}

/* wmt_core.c:1420-1520 */
static int opfunc_hw_rst(struct wmt_op *op)
{
	unsigned long pa1 = 0, pa2 = 0;
	int ret;

	g_wmt.drv_status[WMTDRV_FM] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_GPS] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_LPBK] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_STP] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_COREDUMP] = DRV_STS_POWER_OFF;

	if (g_wmt.drv_status[WMTDRV_WMT] == DRV_STS_POWER_OFF) {
		ret = wmt_core_ctrl(WMT_CTRL_HW_PWR_ON, &pa1, &pa2);
		if (ret)
			return ret;
		g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_ON;
	}

	if (g_wmt.drv_status[WMTDRV_BT] == DRV_STS_FUNC_ON) {
		pa1 = BT_PALDO;
		pa2 = PALDO_OFF;
		wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
		g_wmt.drv_status[WMTDRV_BT] = DRV_STS_POWER_OFF;
	}

	if (g_wmt.drv_status[WMTDRV_WIFI] == DRV_STS_FUNC_ON) {
		wmt_func_off(WMTDRV_WIFI);
		g_wmt.drv_status[WMTDRV_WIFI] = DRV_STS_POWER_OFF;
	}

	g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_OFF;

	ret = wmt_core_ctrl(WMT_CTRL_HW_RST, &pa1, &pa2);
	if (ret)
		wmt_err("hardware reset failed (%d)\n", ret);

	g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_ON;

	ret = wmt_core_ctrl(WMT_CTRL_STP_CLOSE, &pa1, &pa2);
	if (ret)
		wmt_err("STP close after reset failed (%d)\n", ret);

	return ret;
}

/* wmt_core.c:1522-1531 */
static int opfunc_sw_rst(struct wmt_op *op)
{
	int ret;

	ret = wmt_core_stp_init();
	if (!ret)
		g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_FUNC_ON;

	return ret;
}

/* wmt_core.c:1533-1537 */
static int opfunc_stp_rst(struct wmt_op *op)
{
	unsigned long pa1 = 0, pa2 = 0;

	return wmt_core_ctrl(WMT_CTRL_STP_RST, &pa1, &pa2);
}

/* wmt_core.c:1539-1590 */
static int opfunc_therm_ctrl(struct wmt_op *op)
{
	u8 cmd[sizeof(wmt_therm_cmd)];
	u8 evt[16] = { };
	u32 written = 0, read = 0;
	int ret;

	memcpy(cmd, wmt_therm_cmd, sizeof(cmd));
	cmd[4] = op->data[0];

	ret = wmt_core_tx(cmd, sizeof(cmd), &written);
	if (ret || written != sizeof(cmd))
		return -EIO;

	ret = wmt_core_rx(evt, sizeof(evt), &read);
	if (ret || (read != sizeof(wmt_therm_ctrl_evt) &&
		    read != sizeof(wmt_therm_read_evt)))
		return -EIO;

	if (read == sizeof(wmt_therm_ctrl_evt)) {
		op->data[1] = memcmp(evt, wmt_therm_ctrl_evt,
				     sizeof(wmt_therm_ctrl_evt)) ? 0 : 1;
	} else {
		op->data[1] = memcmp(evt, wmt_therm_read_evt,
				     sizeof(wmt_therm_read_evt) - 1) ?
			      0xff : evt[5];
	}

	return 0;
}

/* Downstream wmt_lib_ps_handler(): the PSM engine waits for the MCU to
 * acknowledge SLEEP/WAKEUP/HOST_AWAKE.  The result is reported back to PSM
 * through stp_psm_notify_action_done() by the caller.
 */
static int opfunc_pwr_sv(struct wmt_op *op)
{
	enum mtk_wcn_stp_psm_action action = op->data[0];
	u8 evt[8] = { };
	u32 written = 0, read = 0;
	unsigned long pa1 = 0, pa2 = 0;
	int ret;

	switch (action) {
	case MTK_WCN_STP_PSM_SLEEP:
		ret = wmt_core_tx(wmt_sleep_cmd, sizeof(wmt_sleep_cmd),
				  &written);
		if (ret || written != sizeof(wmt_sleep_cmd)) {
			wmt_err("sleep command tx failed (%d, %u)\n", ret,
				written);
			return -EIO;
		}
		ret = wmt_core_rx(evt, sizeof(wmt_sleep_evt), &read);
		if (ret || read != sizeof(wmt_sleep_evt) ||
		    memcmp(evt, wmt_sleep_evt, sizeof(wmt_sleep_evt))) {
			wmt_err("sleep event failed (%d, %u)\n", ret, read);
			return -EIO;
		}
		return 0;

	case MTK_WCN_STP_PSM_WAKEUP:
		/* Downstream wakes the MCU with the BTIF handshake and only
		 * waits for the WAKEUP event (wmt_core.c:1215-1260).
		 */
		ret = wmt_core_ctrl(WMT_CTRL_SOC_WAKEUP_CONSYS, &pa1, &pa2);
		if (ret) {
			wmt_err("BTIF wakeup failed (%d)\n", ret);
			return ret;
		}
		ret = wmt_core_rx(evt, sizeof(wmt_wakeup_evt), &read);
		if (ret || read != sizeof(wmt_wakeup_evt) ||
		    memcmp(evt, wmt_wakeup_evt, sizeof(wmt_wakeup_evt))) {
			wmt_err("wakeup event failed (%d, %u)\n", ret, read);
			return -EIO;
		}
		return 0;

	case MTK_WCN_STP_PSM_HOST_AWAKE:
		ret = wmt_core_tx(wmt_host_awake_cmd,
				  sizeof(wmt_host_awake_cmd), &written);
		if (ret || written != sizeof(wmt_host_awake_cmd)) {
			wmt_err("host awake command tx failed (%d, %u)\n", ret,
				written);
			return -EIO;
		}
		ret = wmt_core_rx(evt, sizeof(wmt_host_awake_evt), &read);
		if (ret || read != sizeof(wmt_host_awake_evt) ||
		    memcmp(evt, wmt_host_awake_evt,
			   sizeof(wmt_host_awake_evt))) {
			wmt_err("host awake event failed (%d, %u)\n", ret, read);
			return -EIO;
		}
		return 0;

	default:
		return -EINVAL;
	}
}

/* Downstream opfunc_cmd_test (wmt_core.c:1545-1690).  Command 2 is the
 * coexistence debug path exposed through /dev/stpwmt; commands 0/1/3 are the
 * firmware assert/exception/no-ack triggers and 4/5 the warm-reset and
 * firmware-log triggers.
 */
static int opfunc_cmd_test(struct wmt_op *op)
{
	u8 cmd_no = op->data[0];
	u8 param = op->data[1];
	u8 *out = (u8 *)op->data[2];
	u32 room = op->data[3];
	const u8 *tst_cmd;
	const u8 *exp_evt = NULL;
	u32 cmd_sz, evt_sz = 0;
	u8 evt[32] = { };
	u8 coexdbg[sizeof(wmt_test_coexdbg_cmd)];
	u32 written = 0, read = 0;
	u32 payload_len;
	int ret;

	switch (cmd_no) {
	case WMT_TEST_CMD_ASSERT:
		tst_cmd = wmt_test_assert_cmd;
		cmd_sz = sizeof(wmt_test_assert_cmd);
		break;
	case WMT_TEST_CMD_EXCEPTION:
		tst_cmd = wmt_test_exception_cmd;
		cmd_sz = sizeof(wmt_test_exception_cmd);
		break;
	case WMT_TEST_CMD_NOACK:
		tst_cmd = wmt_test_noack_cmd;
		cmd_sz = sizeof(wmt_test_noack_cmd);
		break;
	case WMT_TEST_CMD_WARNRST:
		tst_cmd = wmt_test_warnrst_cmd;
		cmd_sz = sizeof(wmt_test_warnrst_cmd);
		exp_evt = wmt_test_warnrst_evt;
		evt_sz = sizeof(wmt_test_warnrst_evt);
		break;
	case WMT_TEST_CMD_FWLOG:
		tst_cmd = wmt_test_fwlog_cmd;
		cmd_sz = sizeof(wmt_test_fwlog_cmd);
		exp_evt = wmt_test_fwlog_evt;
		evt_sz = sizeof(wmt_test_fwlog_evt);
		break;
	case WMT_TEST_CMD_COEXDBG:
		if (param >= WMTDRV_CMD_COEXDBG_MAX)
			return -EINVAL;
		if (!out || !room)
			return -EINVAL;

		memcpy(coexdbg, wmt_test_coexdbg_cmd, sizeof(coexdbg));
		coexdbg[5] = param;
		tst_cmd = coexdbg;
		cmd_sz = sizeof(coexdbg);

		/* wmt_core.c:1597-1612: the event payload size depends on
		 * the command index.
		 */
		if (param <= 0x04)
			payload_len = 4;
		else if (param == 0x05)
			payload_len = 6;
		else
			payload_len = 8;
		evt_sz = 4 + 1 + payload_len;
		break;
	default:
		return -EINVAL;
	}

	ret = wmt_core_tx(tst_cmd, cmd_sz, &written);
	if (ret || written != cmd_sz) {
		wmt_err("test command %u tx failed (%d, %u)\n", cmd_no, ret,
			written);
		return -EIO;
	}

	/* The assert/exception/no-ack triggers do not answer with an event. */
	if (cmd_no == WMT_TEST_CMD_ASSERT || cmd_no == WMT_TEST_CMD_EXCEPTION ||
	    cmd_no == WMT_TEST_CMD_NOACK)
		return 0;

	ret = wmt_core_rx(evt, evt_sz, &read);
	if (ret || read != evt_sz) {
		wmt_err("test command %u event failed (%d, %u/%u)\n", cmd_no,
			ret, read, evt_sz);
		return -EIO;
	}

	if (cmd_no == WMT_TEST_CMD_COEXDBG) {
		/* type=event, opcode=0x10, len=evt_sz-4, status=0 */
		if (evt[0] != 0x02 || evt[1] != 0x10 ||
		    evt[2] != evt_sz - 4 || evt[4] != 0x00) {
			wmt_err("bad coexdbg event %02x %02x %02x %02x\n",
				evt[0], evt[1], evt[2], evt[4]);
			return -EIO;
		}

		payload_len = min_t(u32, payload_len, room);
		memcpy(out, &evt[5], payload_len);
		op->data[3] = payload_len;
		return 0;
	}

	if (memcmp(evt, exp_evt, evt_sz)) {
		wmt_err("test command %u unexpected event\n", cmd_no);
		return -EIO;
	}

	return 0;
}

/* Downstream opfunc_bgw_ds (wmt_core.c:1962-2025): forward the payload the
 * BGW daemon handed to WMT_IOCTL_SEND_BGW_DS_CMD.
 */
static int opfunc_bgw_ds(struct wmt_op *op)
{
	u8 cmd[sizeof(wmt_bgw_ds_cmd)];
	u8 evt[8] = { };
	const u8 *payload = (const u8 *)op->data[1];
	u32 len = op->data[0];
	u32 written = 0, read = 0;
	int ret;

	if (!payload || len != WMT_BGW_DS_PAYLOAD_SIZE)
		return -EINVAL;

	memcpy(cmd, wmt_bgw_ds_cmd, sizeof(cmd));
	memcpy(&cmd[5], payload, len);

	ret = wmt_core_tx(cmd, sizeof(cmd), &written);
	if (ret || written != sizeof(cmd)) {
		wmt_err("bgw desense tx failed (%d, %u)\n", ret, written);
		return -EIO;
	}

	ret = wmt_core_rx(evt, sizeof(wmt_bgw_ds_evt), &read);
	if (ret || read != sizeof(wmt_bgw_ds_evt) ||
	    memcmp(evt, wmt_bgw_ds_evt, sizeof(wmt_bgw_ds_evt))) {
		wmt_err("bgw desense event failed (%d, %u)\n", ret, read);
		return -EIO;
	}

	return 0;
}

/* Downstream opfunc_idc_msg_handling (wmt_core.c:2170-2230). */
static int opfunc_idc_msg_handling(struct wmt_op *op)
{
	u8 cmd[8];
	u8 evt[8] = { };
	const u8 *payload = (const u8 *)op->data[0];
	u8 opcode = op->data[1];
	u32 len = op->data[2];
	u32 written = 0, read = 0;
	u16 msg_len;
	int ret;

	if (!payload || !len || len > WMT_IDC_MAX_PAYLOAD)
		return -EINVAL;

	msg_len = len + 1;		/* payload + flag byte */
	cmd[0] = 0x01;
	cmd[1] = 0x10;
	memcpy(&cmd[2], &msg_len, sizeof(msg_len));
	cmd[4] = opcode;

	ret = wmt_core_tx(cmd, 5, &written);
	if (ret || written != 5) {
		wmt_err("IDC header tx failed (%d, %u)\n", ret, written);
		return -EIO;
	}

	ret = wmt_core_tx(payload, len, &written);
	if (ret || written != len) {
		wmt_err("IDC payload tx failed (%d, %u)\n", ret, written);
		return -EIO;
	}

	ret = wmt_core_rx(evt, sizeof(wmt_idc_evt), &read);
	if (ret || read != sizeof(wmt_idc_evt) ||
	    memcmp(evt, wmt_idc_evt, sizeof(wmt_idc_evt))) {
		wmt_err("IDC event failed (%d, %u)\n", ret, read);
		return -EIO;
	}

	return 0;
}

static int opfunc_dsns(struct wmt_op *op)
{
	u8 cmd[] = { 0x01, 0x0e, 0x02, 0x00, 0x01, 0x00 };
	u8 evt[16] = { };
	u32 written = 0, read = 0;
	int ret;

	cmd[4] = op->data[0];
	cmd[5] = op->data[1];

	ret = wmt_core_tx(cmd, sizeof(cmd), &written);
	if (ret || written != sizeof(cmd))
		return -EIO;

	ret = wmt_core_rx(evt, sizeof(wmt_dsns_evt), &read);
	if (ret || read != sizeof(wmt_dsns_evt))
		return -EIO;

	if (memcmp(evt, wmt_dsns_evt, sizeof(wmt_dsns_evt))) {
		wmt_err("unexpected desense event\n");
		return -EIO;
	}

	return 0;
}

/* wmt_core.c:1898-1930 */
static int opfunc_gpio_ctrl(struct wmt_op *op)
{
	if (g_wmt.drv_status[WMTDRV_WMT] != DRV_STS_FUNC_ON)
		return -EPERM;

	return wmt_ic_soc_pin_ctrl(op->data[0], op->data[1], op->data[2]);
}

static int opfunc_pin_state(struct wmt_op *op)
{
	unsigned long pa1 = 0, pa2 = 0;

	return wmt_core_ctrl(WMT_CTRL_HW_STATE_DUMP, &pa1, &pa2);
}

/* wmt_core.c:2073-2082, reached through mtk_wcn_wmt_assert() */
static int opfunc_assert(struct wmt_op *op)
{
	u32 written = 0;
	int ret;

	ret = wmt_core_tx(wmt_assert_cmd, sizeof(wmt_assert_cmd), &written);
	if (ret || written != sizeof(wmt_assert_cmd))
		return -EIO;

	/* The firmware asserts; no event is returned (wmt_core.c:1720-1723). */
	return 0;
}

static int (*const wmt_core_opfunc[WMT_OPID_MAX])(struct wmt_op *op) = {
	[WMT_OPID_HIF_CONF] = opfunc_hif_conf,
	[WMT_OPID_PWR_ON] = opfunc_pwr_on,
	[WMT_OPID_PWR_OFF] = opfunc_pwr_off,
	[WMT_OPID_FUNC_ON] = opfunc_func_on,
	[WMT_OPID_FUNC_OFF] = opfunc_func_off,
	[WMT_OPID_REG_RW] = opfunc_reg_rw,
	[WMT_OPID_PWR_SV] = opfunc_pwr_sv,
	[WMT_OPID_DSNS] = opfunc_dsns,
	[WMT_OPID_CMD_TEST] = opfunc_cmd_test,
	[WMT_OPID_BGW_DS] = opfunc_bgw_ds,
	[WMT_OPID_IDC_MSG_HANDLING] = opfunc_idc_msg_handling,
	[WMT_OPID_HW_RST] = opfunc_hw_rst,
	[WMT_OPID_SW_RST] = opfunc_sw_rst,
	[WMT_OPID_STP_RST] = opfunc_stp_rst,
	[WMT_OPID_THERM_CTRL] = opfunc_therm_ctrl,
	[WMT_OPID_GPIO_CTRL] = opfunc_gpio_ctrl,
	[WMT_OPID_GPIO_STATE] = opfunc_pin_state,
	[WMT_OPID_ASSERT] = opfunc_assert,
};

int wmt_core_opid(struct wmt_op *op)
{
	if (!op || op->op_id >= WMT_OPID_MAX || !wmt_core_opfunc[op->op_id]) {
		wmt_err("invalid operation %d\n", op ? op->op_id : -1);
		return -EINVAL;
	}

	return wmt_core_opfunc[op->op_id](op);
}

int wmt_core_init(void)
{
	memset(&g_wmt.hif, 0, sizeof(g_wmt.hif));
	g_wmt.drv_status[WMTDRV_BT] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_FM] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_GPS] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_WIFI] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_WMT] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_STP] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_LPBK] = DRV_STS_POWER_OFF;
	g_wmt.drv_status[WMTDRV_COREDUMP] = DRV_STS_POWER_OFF;
	return 0;
}

void wmt_core_deinit(void)
{
	memset(&g_wmt.drv_status, 0, sizeof(g_wmt.drv_status));
}

/* ------------------------------------------------------------------------- *
 * Entry points used by the PSM and coexistence engines
 * ------------------------------------------------------------------------- */

int wmt_core_psm_action(enum mtk_wcn_stp_psm_action action)
{
	struct wmt_op op = { .op_id = WMT_OPID_PWR_SV };
	int ret;

	if (action >= MTK_WCN_STP_PSM_ACTION_MAX)
		return -EINVAL;

	/* Downstream wmt_lib_ps_handler() rolls back when the link is not
	 * ready instead of sending power-saving commands.
	 */
	if (!g_wmt.ready || !mtk_wcn_stp_is_ready())
		return -ENODEV;

	op.data[0] = action;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op);
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}

void wmt_core_evt_err_trg_assert(u32 reason)
{
	unsigned long pa1 = WMTDRV_WMT;
	unsigned long pa2 = reason;

	/* wmt_ctrl.c:900-930: marks the assert and schedules whole-chip
	 * recovery.
	 */
	wmt_core_ctrl(WMT_CTRL_EVT_ERR_TRG_ASSERT, &pa1, &pa2);
}

int wmt_core_cmd_test(u8 cmd, u8 param, u8 *out, u32 room, u32 *out_len)
{
	struct wmt_op op = { .op_id = WMT_OPID_CMD_TEST };
	int ret;

	if (out_len)
		*out_len = 0;

	op.data[0] = cmd;
	op.data[1] = param;
	op.data[2] = (unsigned long)out;
	op.data[3] = room;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op);
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	if (!ret && out_len)
		*out_len = op.data[3];

	return ret;
}

int wmt_core_bgw_ds(const u8 *payload, u32 len)
{
	struct wmt_op op = { .op_id = WMT_OPID_BGW_DS };
	int ret;

	op.data[0] = len;
	op.data[1] = (unsigned long)payload;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op);
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}

int wmt_core_idc_tx(u8 opcode, const u8 *data, u32 len)
{
	struct wmt_op op = { .op_id = WMT_OPID_IDC_MSG_HANDLING };
	int ret;

	op.data[0] = (unsigned long)data;
	op.data[1] = opcode;
	op.data[2] = len;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op);
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}
