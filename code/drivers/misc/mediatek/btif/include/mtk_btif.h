/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek BTIF (Bluetooth interface) public API.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/btif/common/inc/mtk_btif_exp.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_BTIF_H_
#define _MTK_BTIF_H_

#include <linux/types.h>

#define BTIF_MAX_LEN_PER_PKT	2048

enum mtk_btif_dpidle_ctrl {
	BTIF_DPIDLE_DISABLE = 0,
	BTIF_DPIDLE_ENABLE = 1,
	BTIF_DPIDLE_MAX,
};

enum mtk_btif_lpbk_mode {
	BTIF_LPBK_DISABLE = 0,
	BTIF_LPBK_ENABLE = 1,
	BTIF_LPBK_MAX,
};

enum mtk_btif_dbg_id {
	BTIF_DISABLE_LOGGER = 0,
	BTIF_ENABLE_LOGGER = 1,
	BTIF_DUMP_LOG = 2,
	BTIF_CLR_LOG = 3,
	BTIF_DUMP_BTIF_REG = 4,
	BTIF_ENABLE_RT_LOG = 5,
	BTIF_DISABLE_RT_LOG = 6,
	BTIF_DBG_MAX,
};

/* Error codes, negative values are returned as errno-compatible ints. */
enum mtk_btif_error {
	E_BTIF_AGAIN = 0,
	E_BTIF_FAIL = -1,
	E_BTIF_BAD_POINTER = -2,
	E_BTIF_NO_SPACE = -3,
	E_BTIF_INTR = -4,
	E_BTIF_INVAL_PARAM = -5,
	E_BTIF_ALREADY_OPEN = -6,
	E_BTIF_NOT_OPEN = -7,
	E_BTIF_INVAL_STATE = -8,
};

/**
 * typedef mtk_btif_rx_cb - BTIF receive callback
 * @buf: received bytes
 * @len: number of bytes
 *
 * Return: number of bytes consumed.
 */
typedef int (*mtk_btif_rx_cb)(const unsigned char *buf, unsigned int len);

/* Open a BTIF channel. @owner is a short owner name ("stp", "hci", ...). */
int mtk_wcn_btif_open(char *owner, unsigned long *id);

/* Close a BTIF channel. */
int mtk_wcn_btif_close(unsigned long id);

/* Queue @len bytes for transmission. Returns bytes accepted or an error. */
int mtk_wcn_btif_write(unsigned long id, const unsigned char *buf,
		       unsigned int len);

/* Read received bytes (only for owners that did not register a callback). */
int mtk_wcn_btif_read(unsigned long id, unsigned char *buf, unsigned int len);

/* Enable/disable deep-idle handling for the channel. */
int mtk_wcn_btif_dpidle_ctrl(unsigned long id,
			     enum mtk_btif_dpidle_ctrl flag);

/* Register the receive callback of the channel. */
int mtk_wcn_btif_rx_cb_register(unsigned long id, mtk_btif_rx_cb rx_cb);

/* Wake the connectivity SoC before a transmission. */
int mtk_wcn_btif_wakeup_consys(unsigned long id);

/* Loopback test mode. */
int mtk_wcn_btif_loopback_ctrl(unsigned long id,
			       enum mtk_btif_lpbk_mode enable);

/* Debug/logging controls. */
int mtk_wcn_btif_dbg_ctrl(unsigned long id, enum mtk_btif_dbg_id flag);

/**
 * mtk_wcn_btif_parser_wmt_evt() - peek for a WMT event in a BTIF RX buffer
 *
 * Used by the STP layer to detect a WMT event before normal parsing.
 *
 * Return: true if a WMT event was consumed.
 */
bool mtk_wcn_btif_parser_wmt_evt(unsigned long id, const unsigned char *buf,
				 unsigned int len);

#endif /* _MTK_BTIF_H_ */
