// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN per-function power control (BT / FM / GPS / WIFI).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_func.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * Every WCN function is enabled by a WMT FUNC_CTRL command to the firmware.
 * In addition:
 *   - BT and Wi-Fi use their own VCN33 rail (BT_PALDO / WIFI_PALDO),
 *   - in co-clock mode FM and GPS share VCN28 (powered here, not by the
 *     CONSYS power-on sequence),
 *   - GPS drives the GPS_SYNC handshake pin towards the modem and enables the
 *     external LNA through a host pinctrl state.
 */

#include <linux/delay.h>
#include <linux/module.h>

#include "wmt_local.h"

/* Downstream ENUM_BT_GPS_ONOFF_STATE_T bit indices (wmt_exp.h:157-165). */
#define WMT_BT_ON_BIT	0
#define WMT_GPS_ON_BIT	1
#define WMT_WIFI_ON_BIT	2
#define WMT_FM_ON_BIT	3

#define WMT_GPS_ON	BIT(WMT_GPS_ON_BIT)
#define WMT_FM_ON	BIT(WMT_FM_ON_BIT)

/* stp_core.c:1481-1495 - the SoC co-clock setting is global. */
static int wmt_func_bt_on(void)
{
	unsigned long pa1, pa2;
	int ret;

	pa1 = BT_PALDO;
	pa2 = PALDO_ON;
	ret = wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	if (ret) {
		wmt_err("BT PALDO on failed (%d)\n", ret);
		return ret;
	}

	ret = wmt_core_func_ctrl_cmd(WMTDRV_BT, true);
	if (ret) {
		wmt_err("BT function on failed (%d)\n", ret);
		pa1 = BT_PALDO;
		pa2 = PALDO_OFF;
		wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
		return ret;
	}

	g_wmt.func_on_bits |= BIT(WMT_BT_ON_BIT);
	return 0;
}

static int wmt_func_bt_off(void)
{
	unsigned long pa1, pa2;
	int ret, ret2;

	ret = wmt_core_func_ctrl_cmd(WMTDRV_BT, false);
	if (ret)
		wmt_err("BT function off failed (%d)\n", ret);

	pa1 = BT_PALDO;
	pa2 = PALDO_OFF;
	ret2 = wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	if (ret2)
		wmt_err("BT PALDO off failed (%d)\n", ret2);

	g_wmt.func_on_bits &= ~BIT(WMT_BT_ON_BIT);
	return ret ? ret : ret2;
}

/* wmt_func.c:480-540 */
static int wmt_func_fm_on(void)
{
	unsigned long pa1, pa2;
	int ret;

	if (wmt_lib_co_clock() && !(g_wmt.func_on_bits & WMT_GPS_ON)) {
		pa1 = FM_PALDO;
		pa2 = PALDO_ON;
		wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	} else if (wmt_lib_co_clock()) {
		wmt_info("VCN28 already enabled by GPS\n");
	}

	ret = wmt_core_func_ctrl_cmd(WMTDRV_FM, true);
	if (ret) {
		wmt_err("FM function on failed (%d)\n", ret);
		return ret;
	}

	if (wmt_lib_co_clock())
		g_wmt.func_on_bits |= WMT_FM_ON;

	return 0;
}

static int wmt_func_fm_off(void)
{
	unsigned long pa1, pa2;
	int ret;

	ret = wmt_core_func_ctrl_cmd(WMTDRV_FM, false);

	if (wmt_lib_co_clock()) {
		if (g_wmt.func_on_bits & WMT_GPS_ON) {
			wmt_info("VCN28 still needed by GPS\n");
		} else {
			pa1 = FM_PALDO;
			pa2 = PALDO_OFF;
			wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
		}
		g_wmt.func_on_bits &= ~WMT_FM_ON;
	}

	return ret;
}

/* wmt_func.c:590-660 */
static int wmt_func_gps_pre_ctrl(bool on)
{
	const struct wmt_gen_conf *conf = wmt_lib_gen_conf();
	int ret;

	/* GPS_SYNC handshake with the modem (host SoC side). */
	wmt_plat_gpio_ctrl(PIN_GPS_SYNC, on ? PIN_STA_MUX : PIN_STA_DEINIT);

	/* GPS_SYNC pin inside the connectivity SoC (wmt_ic_soc.c:1426-1445). */
	ret = wmt_ic_soc_pin_ctrl(WMT_IC_PIN_GSYNC,
				  on ? WMT_IC_PIN_MUX : WMT_IC_PIN_GPIO, 1);
	if (ret)
		wmt_err("failed to configure GPS_SYNC (%d)\n", ret);

	if (!conf || conf->wmt_gps_lna_enable) {
		/* Chip-side LNA control through EEDI/EEDO needs the WMT
		 * register tables that are not ported; the host side LNA pin
		 * keeps its default state.
		 */
		wmt_warn("chip-side GPS LNA control is not supported\n");
		return 0;
	}

	wmt_plat_gpio_ctrl(PIN_GPS_LNA,
			   on ? PIN_STA_OUT_H : PIN_STA_DEINIT);
	return 0;
}

static int wmt_func_gps_on(void)
{
	unsigned long pa1, pa2;
	int ret;

	if (wmt_lib_co_clock() && !(g_wmt.func_on_bits & WMT_FM_ON)) {
		pa1 = GPS_PALDO;
		pa2 = PALDO_ON;
		wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	} else if (wmt_lib_co_clock()) {
		wmt_info("VCN28 already enabled by FM\n");
	}

	ret = wmt_func_gps_pre_ctrl(true);
	if (ret)
		return ret;

	ret = wmt_core_func_ctrl_cmd(WMTDRV_GPS, true);
	if (ret) {
		wmt_err("GPS function on failed (%d)\n", ret);
		return ret;
	}

	if (wmt_lib_co_clock())
		g_wmt.func_on_bits |= WMT_GPS_ON;

	return 0;
}

static int wmt_func_gps_off(void)
{
	unsigned long pa1, pa2;
	int ret;

	wmt_func_gps_pre_ctrl(false);

	ret = wmt_core_func_ctrl_cmd(WMTDRV_GPS, false);
	if (ret)
		wmt_err("GPS function off failed (%d)\n", ret);

	if (wmt_lib_co_clock()) {
		if (g_wmt.func_on_bits & WMT_FM_ON) {
			wmt_info("VCN28 still needed by FM\n");
		} else {
			pa1 = GPS_PALDO;
			pa2 = PALDO_OFF;
			wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
		}
		g_wmt.func_on_bits &= ~WMT_GPS_ON;
	}

	return ret;
}

/* wmt_func.c:640-712 */
static int wmt_func_wifi_on(void)
{
	int ret;

	if (!g_wmt.wlan_cb.probe) {
		wmt_err("no WLAN driver registered\n");
		g_wmt.wlan_probed = true;
		return -ENODEV;
	}

	ret = g_wmt.wlan_cb.probe();
	if (ret) {
		wmt_err("WLAN probe failed (%d)\n", ret);
		return ret;
	}

	g_wmt.func_on_bits |= BIT(WMT_WIFI_ON_BIT);
	return 0;
}

static int wmt_func_wifi_off(void)
{
	int ret;

	if (!g_wmt.wlan_cb.remove) {
		wmt_err("no WLAN driver registered\n");
		return -ENODEV;
	}

	ret = g_wmt.wlan_cb.remove();
	if (ret) {
		wmt_err("WLAN remove failed (%d)\n", ret);
		return ret;
	}

	g_wmt.func_on_bits &= ~BIT(WMT_WIFI_ON_BIT);
	return 0;
}

int wmt_func_on(int type)
{
	switch (type) {
	case WMTDRV_BT:
		return wmt_func_bt_on();
	case WMTDRV_FM:
		return wmt_func_fm_on();
	case WMTDRV_GPS:
		return wmt_func_gps_on();
	case WMTDRV_WIFI:
		return wmt_func_wifi_on();
	default:
		return -EINVAL;
	}
}

int wmt_func_off(int type)
{
	switch (type) {
	case WMTDRV_BT:
		return wmt_func_bt_off();
	case WMTDRV_FM:
		return wmt_func_fm_off();
	case WMTDRV_GPS:
		return wmt_func_gps_off();
	case WMTDRV_WIFI:
		return wmt_func_wifi_off();
	default:
		return -EINVAL;
	}
}
