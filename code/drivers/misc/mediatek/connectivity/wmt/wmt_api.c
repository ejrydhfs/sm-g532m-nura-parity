// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT public API (frozen mtk_wcn_wmt.h).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/wmt_exp.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#include <linux/module.h>

#include "wmt_local.h"

/* Downstream ENUM_WMTTHERM_TYPE_T (wmt_exp.h:71-78). */
#define WMTTHERM_ENABLE		1
#define WMTTHERM_READ		2
#define WMTTHERM_DISABLE	3
#define WMTTHERM_MAX		4

static bool wmt_public_op(enum wmt_opid op_id, enum mtk_wcn_wmt_drv type)
{
	struct wmt_op op = {
		.op_id = op_id,
		.data = { type },
	};
	bool ret;

	if (!g_wmt.ready)
		return false;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op) == 0;
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}

bool mtk_wcn_wmt_func_on(enum mtk_wcn_wmt_drv type)
{
	if (type > WMTDRV_WIFI) {
		wmt_warn("function %d is not supported\n", type);
		return false;
	}

	return wmt_public_op(WMT_OPID_FUNC_ON, type);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_func_on);

bool mtk_wcn_wmt_func_off(enum mtk_wcn_wmt_drv type)
{
	if (type > WMTDRV_WIFI) {
		wmt_warn("function %d is not supported\n", type);
		return false;
	}

	return wmt_public_op(WMT_OPID_FUNC_OFF, type);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_func_off);

int mtk_wcn_wmt_msgcb_reg(enum mtk_wcn_wmt_drv type, mtk_wcn_wmt_msgcb cb)
{
	if (type < 0 || type >= WMTDRV_MAX)
		return -EINVAL;

	g_wmt.msgcb[type] = cb;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_msgcb_reg);

int mtk_wcn_wmt_msgcb_unreg(enum mtk_wcn_wmt_drv type)
{
	if (type < 0 || type >= WMTDRV_MAX)
		return -EINVAL;

	g_wmt.msgcb[type] = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_msgcb_unreg);

/* wmt_exp.c:426-462 */
int mtk_wcn_wmt_wlan_reg(const struct mtk_wcn_wmt_wlan_cb *cb)
{
	if (!cb || !cb->probe || !cb->remove)
		return -EINVAL;

	memcpy(&g_wmt.wlan_cb, cb, sizeof(g_wmt.wlan_cb));
	wmt_info("WLAN driver registered\n");

	if (g_wmt.wlan_probed) {
		int ret = cb->probe();

		if (ret) {
			wmt_err("deferred WLAN probe failed (%d)\n", ret);
			return ret;
		}
		g_wmt.wlan_probed = false;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_wlan_reg);

int mtk_wcn_wmt_wlan_unreg(void)
{
	memset(&g_wmt.wlan_cb, 0, sizeof(g_wmt.wlan_cb));
	wmt_info("WLAN driver unregistered\n");
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_wlan_unreg);

/* wmt_exp.c:340-400 */
bool mtk_wcn_wmt_assert(enum mtk_wcn_wmt_drv type, u32 reason)
{
	struct wmt_op op = { .op_id = WMT_OPID_ASSERT };
	unsigned long pa1 = 0, pa2 = 0;
	bool ret;

	if (type < 0 || type >= WMTDRV_MAX)
		return false;

	if (!g_wmt.ready)
		return false;

	wmt_warn("driver %d asserts the firmware (reason %u)\n", type, reason);

	/* Wake the connectivity MCU before sending the assert command; the
	 * downstream driver does the same through the STP power-saving engine
	 * (wmt_exp.c:209).
	 */
	wmt_core_ctrl(WMT_CTRL_SOC_WAKEUP_CONSYS, &pa1, &pa2);

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ret = wmt_lib_run_op(&op) == 0;
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_assert);

int mtk_wcn_wmt_system_state_reset(void)
{
	g_wmt.func_on_bits = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_system_state_reset);

/* wmt_exp.c:265-335 */
bool mtk_wcn_wmt_dsns_ctrl(enum mtk_wcn_wmt_dsns type)
{
	struct wmt_op op = {
		.op_id = WMT_OPID_DSNS,
		.data = { WMTDRV_FM, type },
	};
	bool ok;

	if (type >= WMTDSNS_MAX)
		return false;

	if (!g_wmt.ready || !mtk_wcn_stp_is_ready())
		return false;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	ok = wmt_lib_run_op(&op) == 0;
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ok;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_dsns_ctrl);

/* wmt_exp.c:180-260 */
s8 mtk_wcn_wmt_therm_ctrl(u8 type)
{
	struct wmt_op op = { .op_id = WMT_OPID_THERM_CTRL };
	s8 ret;

	if (type < WMTTHERM_ENABLE || type >= WMTTHERM_MAX) {
		wmt_err("invalid thermal control command %u\n", type);
		return 0;
	}

	if (!g_wmt.ready || !mtk_wcn_stp_is_ready()) {
		wmt_err("thermal control needs a ready STP\n");
		return 0;
	}

	op.data[0] = type;

	wmt_plat_wake_lock_ctrl(WL_OP_GET);
	if (wmt_lib_run_op(&op)) {
		wmt_warn("thermal command %u failed\n", type);
		ret = type == WMTTHERM_READ ? (s8)0xff : 0;
	} else {
		ret = (s8)op.data[1];
	}
	wmt_plat_wake_lock_ctrl(WL_OP_PUT);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_therm_ctrl);

enum mtk_wcn_wmt_hwver mtk_wcn_wmt_hwver_get(void)
{
	return wmt_lib_hwver();
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_hwver_get);

u32 mtk_wcn_wmt_hwver_raw_get(void)
{
	return wmt_lib_get_icinfo(WMTCHIN_HWVER);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_hwver_raw_get);

u32 mtk_wcn_wmt_fwver_get(void)
{
	return wmt_lib_get_icinfo(WMTCHIN_FWVER);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_fwver_get);

bool mtk_wcn_wmt_co_clock_flag_get(void)
{
	return wmt_lib_co_clock() != 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_co_clock_flag_get);

u32 mtk_wcn_wmt_ic_info_get(enum mtk_wcn_wmt_icinfo type)
{
	switch (type) {
	case WMT_ICINFO_CHIPID:
		return wmt_lib_get_icinfo(WMTCHIN_CHIPID);
	case WMT_ICINFO_HWVER:
		return wmt_lib_get_icinfo(WMTCHIN_HWVER);
	case WMT_ICINFO_MAPPINGHWVER:
		return wmt_lib_get_icinfo(WMTCHIN_MAPPINGHWVER);
	case WMT_ICINFO_FWVER:
		return wmt_lib_get_icinfo(WMTCHIN_FWVER);
	default:
		return 0;
	}
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_ic_info_get);
