// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT control dispatch.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_ctrl.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * wmt_ctrl() is the single entry point the WMT core uses to reach the STP
 * engine, the platform glue and the firmware patch store.
 */

#include <linux/delay.h>
#include <linux/module.h>

#include <mtk_wcn_stp.h>

#include "wmt_local.h"

static int wmt_ctrl_hw_pwr_off(struct wmt_ctrl_data *d);
static int wmt_ctrl_hw_pwr_on(struct wmt_ctrl_data *d);
static int wmt_ctrl_hw_rst(struct wmt_ctrl_data *d);
static int wmt_ctrl_stp_close(struct wmt_ctrl_data *d);
static int wmt_ctrl_stp_open(struct wmt_ctrl_data *d);
static int wmt_ctrl_stp_conf(struct wmt_ctrl_data *d);
static int wmt_ctrl_free_patch(struct wmt_ctrl_data *d);
static int wmt_ctrl_get_patch(struct wmt_ctrl_data *d);
static int wmt_ctrl_hwidver_set(struct wmt_ctrl_data *d);
static int wmt_ctrl_stp_rst(struct wmt_ctrl_data *d);
static int wmt_ctrl_get_wmt_conf(struct wmt_ctrl_data *d);
static int wmt_ctrl_tx(struct wmt_ctrl_data *d);
static int wmt_ctrl_rx(struct wmt_ctrl_data *d);
static int wmt_ctrl_rx_flush(struct wmt_ctrl_data *d);
static int wmt_ctrl_patch_search(struct wmt_ctrl_data *d);
static int wmt_ctrl_hw_state_show(struct wmt_ctrl_data *d);
static int wmt_ctrl_get_patch_num(struct wmt_ctrl_data *d);
static int wmt_ctrl_get_patch_info(struct wmt_ctrl_data *d);
static int wmt_ctrl_soc_paldo_ctrl(struct wmt_ctrl_data *d);
static int wmt_ctrl_soc_wakeup_consys(struct wmt_ctrl_data *d);
static int wmt_ctrl_evt_err_trg_assert(struct wmt_ctrl_data *d);
static int wmt_ctrl_gps_sync_set(struct wmt_ctrl_data *d);
static int wmt_ctrl_gps_lna_set(struct wmt_ctrl_data *d);
static int wmt_ctrl_others(struct wmt_ctrl_data *d);

/* wmt_ctrl.c:161-197 */
static int (*const wmt_ctrl_func[WMT_CTRL_MAX])(struct wmt_ctrl_data *d) = {
	[WMT_CTRL_HW_PWR_OFF] = wmt_ctrl_hw_pwr_off,
	[WMT_CTRL_HW_PWR_ON] = wmt_ctrl_hw_pwr_on,
	[WMT_CTRL_HW_RST] = wmt_ctrl_hw_rst,
	[WMT_CTRL_STP_CLOSE] = wmt_ctrl_stp_close,
	[WMT_CTRL_STP_OPEN] = wmt_ctrl_stp_open,
	[WMT_CTRL_STP_CONF] = wmt_ctrl_stp_conf,
	[WMT_CTRL_FREE_PATCH] = wmt_ctrl_free_patch,
	[WMT_CTRL_GET_PATCH] = wmt_ctrl_get_patch,
	[WMT_CTRL_HWIDVER_SET] = wmt_ctrl_hwidver_set,
	[WMT_CTRL_STP_RST] = wmt_ctrl_stp_rst,
	[WMT_CTRL_GET_WMT_CONF] = wmt_ctrl_get_wmt_conf,
	[WMT_CTRL_TX] = wmt_ctrl_tx,
	[WMT_CTRL_RX] = wmt_ctrl_rx,
	[WMT_CTRL_RX_FLUSH] = wmt_ctrl_rx_flush,
	[WMT_CTRL_GPS_SYNC_SET] = wmt_ctrl_gps_sync_set,
	[WMT_CTRL_GPS_LNA_SET] = wmt_ctrl_gps_lna_set,
	[WMT_CTRL_PATCH_SEARCH] = wmt_ctrl_patch_search,
	[WMT_CTRL_HW_STATE_DUMP] = wmt_ctrl_hw_state_show,
	[WMT_CTRL_GET_PATCH_NUM] = wmt_ctrl_get_patch_num,
	[WMT_CTRL_GET_PATCH_INFO] = wmt_ctrl_get_patch_info,
	[WMT_CTRL_SOC_PALDO_CTRL] = wmt_ctrl_soc_paldo_ctrl,
	[WMT_CTRL_SOC_WAKEUP_CONSYS] = wmt_ctrl_soc_wakeup_consys,
	[WMT_CTRL_EVT_ERR_TRG_ASSERT] = wmt_ctrl_evt_err_trg_assert,
};

int wmt_ctrl(struct wmt_ctrl_data *data)
{
	if (!data || data->ctrl_id >= WMT_CTRL_MAX)
		return -EINVAL;

	if (!wmt_ctrl_func[data->ctrl_id])
		return wmt_ctrl_others(data);

	return wmt_ctrl_func[data->ctrl_id](data);
}

static int wmt_ctrl_tx(struct wmt_ctrl_data *d)
{
	const u8 *data = (const u8 *)d->data[0];
	u32 size = d->data[1];
	u32 *written = (u32 *)d->data[2];
	bool raw = !!d->data[3];

	return wmt_ctrl_tx_ex(data, size, written, raw);
}

/* wmt_ctrl.c:302-380 */
int wmt_ctrl_tx_ex(const u8 *data, u32 size, u32 *written, bool raw)
{
	int ret;

	if (written)
		*written = 0;

	if (!size)
		return -EINVAL;

	if (!(g_wmt.state & WMT_STAT_STP_OPEN) ||
	    !(g_wmt.state & WMT_STAT_STP_EN)) {
		wmt_err("STP not open/enabled\n");
		return -EPIPE;
	}

	mtk_wcn_stp_flush_rx_queue(WMT_TASK_INDX);

	if (raw)
		ret = mtk_wcn_stp_send_data_raw(data, size, WMT_TASK_INDX);
	else
		ret = mtk_wcn_stp_send_data(data, size, WMT_TASK_INDX);

	if (written)
		*written = ret > 0 ? ret : 0;

	if (ret != size) {
		wmt_dbg("tx %d/%u\n", ret, size);
		if (ret == 0)
			return -EAGAIN;	/* no STP window space */
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

/* wmt_ctrl.c:331-425 */
static int wmt_ctrl_rx(struct wmt_ctrl_data *d)
{
	u8 *buf = (u8 *)d->data[0];
	u32 len = d->data[1];
	u32 *read_size = (u32 *)d->data[2];
	int read_len;

	if (read_size)
		*read_size = 0;

	if (!len)
		return 0;

	if (!(g_wmt.state & WMT_STAT_STP_OPEN))
		return -EPIPE;

	wmt_dev_rx_clear();
	read_len = mtk_wcn_stp_receive_data(buf, len, WMT_TASK_INDX);
	while (read_len == 0) {
		if (wmt_dev_wait_rx(WMT_LIB_RX_TIMEOUT)) {
			wmt_err("WMT rx timeout\n");
			return -ETIMEDOUT;
		}

		read_len = mtk_wcn_stp_receive_data(buf, len, WMT_TASK_INDX);
	}

	if (read_size)
		*read_size = read_len;

	return 0;
}

static int wmt_ctrl_rx_flush(struct wmt_ctrl_data *d)
{
	mtk_wcn_stp_flush_rx_queue(d->data[0]);
	return 0;
}

/* wmt_ctrl.c:427-450 */
static int wmt_ctrl_hw_pwr_off(struct wmt_ctrl_data *d)
{
	if (!test_and_clear_bit(WMT_STAT_PWR, &g_wmt.state)) {
		wmt_warn("CONSYS already off\n");
		return 0;
	}

	return wmt_plat_pwr_ctrl(FUNC_OFF);
}

/* wmt_ctrl.c:452-467 */
static int wmt_ctrl_hw_pwr_on(struct wmt_ctrl_data *d)
{
	if (test_and_set_bit(WMT_STAT_PWR, &g_wmt.state)) {
		wmt_warn("CONSYS already on\n");
		return 0;
	}

	return wmt_plat_pwr_ctrl(FUNC_ON);
}

static int wmt_ctrl_hw_rst(struct wmt_ctrl_data *d)
{
	return wmt_plat_pwr_ctrl(FUNC_RST);
}

static int wmt_ctrl_hw_state_show(struct wmt_ctrl_data *d)
{
	return wmt_plat_pwr_ctrl(FUNC_STAT);
}

/* wmt_ctrl.c:506-545 */
static int wmt_ctrl_stp_close(struct wmt_ctrl_data *d)
{
	int ret;

	if (!(g_wmt.state & WMT_STAT_STP_OPEN))
		return -EALREADY;

	ret = mtk_wcn_stp_register_event_cb(WMT_TASK_INDX, NULL);
	if (ret)
		return ret;

	ret = stp_btif_rx_unregister();
	if (ret)
		wmt_warn("failed to unregister BTIF rx callback (%d)\n", ret);

	ret = stp_btif_close();
	if (ret)
		return ret;

	clear_bit(WMT_STAT_STP_OPEN, &g_wmt.state);
	return 0;
}

/* wmt_ctrl.c:547-578 */
static int wmt_ctrl_stp_open(struct wmt_ctrl_data *d)
{
	int ret;

	ret = stp_btif_open();
	if (ret)
		return ret;

	ret = stp_btif_rx_register();
	if (ret) {
		wmt_err("failed to register BTIF rx callback (%d)\n", ret);
		return ret;
	}

	ret = mtk_wcn_stp_register_event_cb(WMT_TASK_INDX,
					    wmt_dev_rx_event_cb);
	if (ret)
		return ret;

	set_bit(WMT_STAT_STP_OPEN, &g_wmt.state);
	return 0;
}

/* The downstream handler is empty (wmt_ctrl.c:1005-1008); resetting the STP
 * state machine is the only meaningful implementation.  [INFERENCE]
 */
static int wmt_ctrl_stp_rst(struct wmt_ctrl_data *d)
{
	mtk_wcn_stp_flush_context();
	mtk_wcn_stp_ready(0);
	return 0;
}

static int wmt_ctrl_stp_conf_ex(enum wmt_stp_conf_type type, u32 value)
{
	switch (type) {
	case WMT_STP_CONF_EN:
		return mtk_wcn_stp_enable(value);
	case WMT_STP_CONF_RDY:
		return mtk_wcn_stp_ready(value);
	case WMT_STP_CONF_MODE:
		mtk_wcn_stp_set_mode(value);
		return 0;
	default:
		return -EINVAL;
	}
}

/* wmt_ctrl.c:700-740 */
static int wmt_ctrl_stp_conf(struct wmt_ctrl_data *d)
{
	enum wmt_stp_conf_type type = d->data[0];
	u32 value = d->data[1];
	int ret;

	if (!(g_wmt.state & WMT_STAT_STP_OPEN))
		return -EPIPE;

	ret = wmt_ctrl_stp_conf_ex(type, value);
	if (ret)
		return ret;

	if (type == WMT_STP_CONF_EN) {
		if (value)
			set_bit(WMT_STAT_STP_EN, &g_wmt.state);
		else
			clear_bit(WMT_STAT_STP_EN, &g_wmt.state);
	}

	return 0;
}

/*
 * Patch store
 */

static int wmt_ctrl_patch_search(struct wmt_ctrl_data *d)
{
	/* With the multi-patch ABI the launcher has already provided the patch
	 * list through WMT_IOCTL_SET_PATCH_NUM/SET_PATCH_INFO before power on
	 * (wmt_ctrl.c:602-616).  The request is still queued on the command
	 * channel for compatibility with the launcher protocol.
	 */
	wmt_dev_cmd_request("srh_patch");
	return 0;
}

static int wmt_ctrl_get_patch_num(struct wmt_ctrl_data *d)
{
	d->data[0] = wmt_dev_patch_num();
	return 0;
}

/* wmt_ctrl.c:640-665 */
static int wmt_ctrl_get_patch_info(struct wmt_ctrl_data *d)
{
	return wmt_dev_patch_info_get(d->data[0], (u8 *)d->data[1],
				      (u8 *)d->data[2]);
}

/* wmt_ctrl.c:718-772 */
static int wmt_ctrl_get_patch(struct wmt_ctrl_data *d)
{
	const char *full_name = (const char *)d->data[1];
	const char *def_name = (const char *)d->data[0];
	u8 **pp_buf = (u8 **)d->data[2];
	size_t *p_size = (size_t *)d->data[3];
	int ret;

	if (!pp_buf || !p_size)
		return -EINVAL;

	if (full_name && full_name[0]) {
		ret = wmt_dev_patch_get(full_name, pp_buf, p_size);
		if (!ret)
			return 0;
	}

	if (def_name && def_name[0]) {
		ret = wmt_dev_patch_get(def_name, pp_buf, p_size);
		if (!ret)
			return 0;
	}

	return -ENOENT;
}

/* wmt_ctrl.c:690-706 */
static int wmt_ctrl_free_patch(struct wmt_ctrl_data *d)
{
	u32 seq = d->data[0];

	wmt_dev_patch_put();
	if (seq && seq >= wmt_dev_patch_num())
		wmt_dev_patch_info_free_all();
	return 0;
}

/* wmt_ctrl.c:864-878 */
static int wmt_ctrl_hwidver_set(struct wmt_ctrl_data *d)
{
	wmt_lib_set_chip_info((d->data[0] & 0xffff0000) >> 16,
			      d->data[0] & 0x0000ffff,
			      d->data[1] & 0x0000ffff,
			      (d->data[1] & 0xffff0000) >> 16);
	return 0;
}

static int wmt_ctrl_get_wmt_conf(struct wmt_ctrl_data *d)
{
	d->data[0] = (unsigned long)wmt_lib_gen_conf();
	return 0;
}

/* wmt_ctrl.c:668-690 */
static int wmt_ctrl_soc_paldo_ctrl(struct wmt_ctrl_data *d)
{
	int ret;

	ret = wmt_plat_soc_paldo_ctrl(d->data[0], d->data[1]);
	if (ret && d->data[0] == PMIC_CHIPID_PALDO)
		d->data[2] = ret;

	return ret;
}

static int wmt_ctrl_soc_wakeup_consys(struct wmt_ctrl_data *d)
{
	int ret = stp_btif_wakeup();

	if (ret)
		wmt_err("failed to wake up CONSYS (%d)\n", ret);

	return ret;
}

/* wmt_ctrl.c:900-930 */
static int wmt_ctrl_evt_err_trg_assert(struct wmt_ctrl_data *d)
{
	wmt_warn("driver %lu reason %lu triggers a firmware assert\n",
		 d->data[0], d->data[1]);

	if (mtk_wcn_stp_get_wmt_evt_err_trg_assert()) {
		wmt_info("assert already triggered\n");
		return 0;
	}

	mtk_wcn_stp_set_wmt_evt_err_trg_assert(1);
	/* The firmware-assert-via-EMI mechanism is not ported; recover by
	 * resetting CONSYS (see wmt_lib_cmb_rst()).
	 */
	wmt_lib_schedule_reset(WMTRSTSRC_RESET_STP);
	return 0;
}

/* wmt_ctrl.c:960-985 */
static int wmt_ctrl_gps_sync_set(struct wmt_ctrl_data *d)
{
	wmt_plat_gpio_ctrl(PIN_GPS_SYNC,
			   d->data[0] ? PIN_STA_MUX : PIN_STA_DEINIT);
	return 0;
}

static int wmt_ctrl_gps_lna_set(struct wmt_ctrl_data *d)
{
	wmt_plat_gpio_ctrl(PIN_GPS_LNA,
			   d->data[0] ? PIN_STA_OUT_H : PIN_STA_DEINIT);
	return 0;
}

static int wmt_ctrl_others(struct wmt_ctrl_data *d)
{
	wmt_err("invalid control id %lu\n", d->ctrl_id);
	return -EINVAL;
}
