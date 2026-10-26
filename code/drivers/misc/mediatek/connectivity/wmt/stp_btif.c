// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN STP transport binding for BTIF.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/stp_btif.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The connectivity SoC is attached to the application processor through the
 * BTIF DMA controller on MT6735/MC6737T.  This file owns the BTIF channel
 * used by STP ("CONSYS_STP"), registers the STP transmit path with the STP
 * engine and forwards received bytes to the STP parser.
 *
 * The channel is opened lazily by WMT_CTRL_STP_OPEN and closed by
 * WMT_CTRL_STP_CLOSE, matching the downstream power-on/off ordering.
 */

#include <linux/delay.h>
#include <linux/module.h>

#include <mtk_btif.h>
#include <mtk_wcn_stp.h>

#include "wmt_local.h"

#define BTIF_OWNER_NAME		"CONSYS_STP"

/* stp_btif.c:43-46 */
#define STP_MAX_PACKAGE_ALLOWED	2000
#define STP_BTIF_TX_RTY_LMT	10
#define STP_BTIF_TX_RTY_DLY	5	/* ms */

static unsigned long stp_btif_id;

/* stp_btif.c:129-200 */
static int stp_btif_tx(const u8 *buf, u32 len, u32 *written_len)
{
	int retry_left = STP_BTIF_TX_RTY_LMT;
	int wr_count, written;

	*written_len = 0;

	if (!stp_btif_id) {
		wmt_warn("no BTIF channel\n");
		return -ENODEV;
	}

	if (len == 0)
		return 0;

	if (len > STP_MAX_PACKAGE_ALLOWED) {
		wmt_warn("abnormal packet length %u\n", len);
		return -EMSGSIZE;
	}

	wr_count = mtk_wcn_btif_write(stp_btif_id, buf, len);
	if (wr_count < 0) {
		wmt_err("btif write failed (%d)\n", wr_count);
		return -EIO;
	}
	if (wr_count == len) {
		*written_len = wr_count;
		return 0;
	}

	while (retry_left-- && wr_count < len) {
		msleep(STP_BTIF_TX_RTY_DLY);
		written = mtk_wcn_btif_write(stp_btif_id, buf + wr_count,
					     len - wr_count);
		if (written < 0) {
			wmt_err("btif write failed (%d) while recovering\n",
				written);
			break;
		}
		wr_count += written;
	}

	if (wr_count == len) {
		wmt_info("btif write recovered, len %u, retries left %d\n",
			 len, retry_left);
		*written_len = wr_count;
		return 0;
	}

	wmt_err("btif write %u/%d, retries left %d\n", len, wr_count,
		retry_left);
	return -EIO;
}

static int stp_btif_parser(const unsigned char *buf, unsigned int len)
{
	mtk_wcn_stp_parser_data((u8 *)(uintptr_t)buf, len);
	return len;
}

int stp_btif_open(void)
{
	int ret;

	if (stp_btif_id)
		return -EALREADY;

	ret = mtk_wcn_btif_open(BTIF_OWNER_NAME, &stp_btif_id);
	if (ret) {
		wmt_err("btif open failed (%d)\n", ret);
		return ret;
	}

	ret = mtk_wcn_stp_register_if_tx(STP_TX_IF_BTIF, stp_btif_tx);
	if (ret) {
		mtk_wcn_btif_close(stp_btif_id);
		stp_btif_id = 0;
		return ret;
	}

	mtk_wcn_stp_set_if_tx_type(STP_TX_IF_BTIF);
	wmt_dbg("STP BTIF channel opened\n");
	return 0;
}

int stp_btif_close(void)
{
	int ret;

	if (!stp_btif_id)
		return -ENODEV;

	ret = mtk_wcn_btif_close(stp_btif_id);
	if (ret) {
		wmt_err("btif close failed (%d)\n", ret);
		return ret;
	}

	stp_btif_id = 0;
	return 0;
}

int stp_btif_rx_register(void)
{
	if (!stp_btif_id)
		return -ENODEV;

	return mtk_wcn_btif_rx_cb_register(stp_btif_id, stp_btif_parser);
}

int stp_btif_rx_unregister(void)
{
	if (!stp_btif_id)
		return -ENODEV;

	return mtk_wcn_btif_rx_cb_register(stp_btif_id, NULL);
}

int stp_btif_wakeup(void)
{
	if (!stp_btif_id)
		return -ENODEV;

	return mtk_wcn_btif_wakeup_consys(stp_btif_id);
}

/* Downstream psm_core.c:895-905 calls mt_combo_plt_enter_deep_idle() /
 * mt_combo_plt_exit_deep_idle() with COMBO_IF_BTIF once the MCU acknowledges
 * the SLEEP handshake (and on the way out of it).  On this platform that is
 * the BTIF controller's deep-idle state machine.
 */
int stp_btif_dpidle_ctrl(bool enable)
{
	if (!stp_btif_id)
		return -ENODEV;

	return mtk_wcn_btif_dpidle_ctrl(stp_btif_id,
					enable ? BTIF_DPIDLE_ENABLE :
						 BTIF_DPIDLE_DISABLE);
}
