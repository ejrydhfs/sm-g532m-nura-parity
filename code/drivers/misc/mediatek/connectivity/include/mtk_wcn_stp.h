/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN STP (Serial Transport Protocol) public interface.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/common/conn_soc/include/stp_exp.h
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/include/stp_wmt.h
 *
 * The API names are kept identical to downstream so that the ported
 * Bluetooth/GPS/FM/WLAN glue does not need to be rewritten. Types are
 * converted to native kernel types.
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_WCN_STP_H_
#define _MTK_WCN_STP_H_

#include <linux/types.h>

/* Logical STP channels ("task indices" downstream). */
#define BT_TASK_INDX		0
#define FM_TASK_INDX		1
#define GPS_TASK_INDX		2
#define WIFI_TASK_INDX		3
#define WMT_TASK_INDX		4
#define STP_TASK_INDX		5
#define INFO_TASK_INDX		6
#define ANT_TASK_INDX		7
/* BT/Wi-Fi <-> LTE IDC frames (WMT_LTE_COEX_FLAG sub-type on the WMT channel).
 * Downstream stp_exp.h:48.
 */
#define COEX_TASK_INDX		8
#define MTKSTP_MAX_TASK_NUM	9

/* Size of the per-channel receive queues. */
#define MTKSTP_BUFFER_SIZE	16384

/* Largest STP frame accepted from the transport layer. */
#define MTKSTP_MAX_PKT_SIZE	2048

/**
 * typedef mtk_wcn_stp_event_cb - channel event notifier
 *
 * Called without any STP internal lock held; the callee is expected to drain
 * the channel with mtk_wcn_stp_receive_data().
 */
typedef void (*mtk_wcn_stp_event_cb)(void);

/**
 * typedef mtk_wcn_stp_if_tx - transport transmit callback
 * @data: buffer to transmit
 * @size: number of bytes
 * @written: number of bytes actually accepted by the transport
 *
 * Return: 0 on success, negative errno on failure.
 */
typedef int (*mtk_wcn_stp_if_tx)(const u8 *data, u32 size, u32 *written);

/**
 * typedef mtk_wcn_stp_if_rx - transport receive callback
 * @data: received bytes
 * @size: number of bytes received
 *
 * Feed bytes coming from the connectivity SoC into the STP parser.
 */
typedef void (*mtk_wcn_stp_if_rx)(const u8 *data, int size);

/* Transport selection, see mtk_wcn_stp_register_if_tx(). */
enum mtk_wcn_stp_tx_if {
	STP_TX_IF_UART = 0,
	STP_TX_IF_SDIO = 1,
	STP_TX_IF_BTIF = 2,
	STP_TX_IF_MAX,
};

/* STP is open and the WMT handshake completed. */
bool mtk_wcn_stp_is_ready(void);

/* Queue a payload on a logical channel. */
int mtk_wcn_stp_send_data(const u8 *buffer, u32 length, u8 type);

/* Dequeue up to @length bytes from a logical channel. */
int mtk_wcn_stp_receive_data(u8 *buffer, u32 length, u8 type);

/* True when nothing is pending for the channel. */
bool mtk_wcn_stp_is_rxqueue_empty(u8 type);

/* Feed raw bytes from the transport into the STP parser. */
int mtk_wcn_stp_parser_data(u8 *buffer, u32 length);

/* Register a wakeup-notification callback for a channel. */
int mtk_wcn_stp_register_event_cb(int type, mtk_wcn_stp_event_cb func);

/* Register a "tx queue has room" callback for a channel. */
int mtk_wcn_stp_register_tx_event_cb(int type, mtk_wcn_stp_event_cb func);

/* Register the transport receive handler (called by the transport). */
int mtk_wcn_stp_register_if_rx(mtk_wcn_stp_if_rx func);

/* Register the transport transmit handler for @stp_if. */
int mtk_wcn_stp_register_if_tx(enum mtk_wcn_stp_tx_if stp_if,
			       mtk_wcn_stp_if_tx func);

/* ------------------------------------------------------------------------- *
 * STP power saving (PSM), downstream psm_core.h / stp_exp.h
 *
 * PSM is disabled until a consumer calls mtk_wcn_stp_psm_enable(); while it
 * is disabled the link stays awake and every byte is transmitted immediately,
 * which is the behaviour of this driver without the PSM engine.
 * ------------------------------------------------------------------------- */

/* Downstream MTKSTP_PSM_STATE_T (psm_core.h:101-107). */
enum mtk_wcn_stp_psm_state {
	MTK_WCN_STP_PSM_ACT = 0,
	MTK_WCN_STP_PSM_ACT_INACT,
	MTK_WCN_STP_PSM_INACT,
	MTK_WCN_STP_PSM_INACT_ACT,
	MTK_WCN_STP_PSM_STATE_MAX
};

/* Downstream MTKSTP_PSM_ACTION_T (stp_wmt.h:56-63). */
enum mtk_wcn_stp_psm_action {
	MTK_WCN_STP_PSM_SLEEP = 0,
	MTK_WCN_STP_PSM_HOST_AWAKE,
	MTK_WCN_STP_PSM_WAKEUP,
	MTK_WCN_STP_PSM_EIRQ,
	MTK_WCN_STP_PSM_ROLLBACK,
	MTK_WCN_STP_PSM_ACTION_MAX
};

/* Enable PSM; @idle_time_ms arms the idle timer that puts the MCU to sleep.
 * Downstream STP_PSM_IDLE_TIME_SLEEP is 30 ms (psm_core.h:56).
 */
int mtk_wcn_stp_psm_enable(int idle_time_ms);

/* Disable PSM and force the MCU awake. */
int mtk_wcn_stp_psm_disable(void);

/* Reset the PSM state machine (called from the STP reset path). */
int mtk_wcn_stp_psm_reset(void);

/* Downstream stp_psm_set_state(); also clears BLOCK_DATA_EN for ACT. */
int mtk_wcn_stp_psm_set_state(enum mtk_wcn_stp_psm_state state);

/* Complete an action previously requested by the PSM engine. */
int mtk_wcn_stp_psm_notify_stp(enum mtk_wcn_stp_psm_action action);

/* Wake the MCU with a WMT WAKEUP / HOST_AWAKE handshake. */
int mtk_wcn_stp_psm_notify_wmt_wakeup(void);

/* Ask the MCU to enter sleep (WMT SLEEP handshake). */
int mtk_wcn_stp_psm_notify_wmt_sleep(void);

/* Thermal mitigation: force a sleep handshake if PSM is enabled. */
int mtk_wcn_stp_psm_sleep_for_thermal(void);

/* PSM trace level, 0 (errors) .. 4 (loud). */
bool mtk_wcn_stp_psm_dbg_level(u32 level);

/* True when the quick-PS path is usable (SOC supports it and the system is
 * suspending).  Downstream wmt_lib_is_quick_ps_support().
 */
bool mtk_wcn_stp_psm_is_quick_ps_support(void);

/* ------------------------------------------------------------------------- *
 * Firmware coredump / assert, downstream stp_core.h:330-350
 * ------------------------------------------------------------------------- */

/* Mark the firmware as asserting: blocks STP TX except WMT control traffic. */
int mtk_wcn_stp_coredump_start_ctrl(u32 value);

/* Non-zero while the firmware assert/coredump sequence is in progress. */
u32 mtk_wcn_stp_coredump_start_get(void);

/* Enable/disable coredump collection (downstream mtk_wcn_stp_coredump_flag). */
int mtk_wcn_stp_coredump_flag_ctrl(u32 on);
int mtk_wcn_stp_coredump_flag_get(void);

#endif /* _MTK_WCN_STP_H_ */
