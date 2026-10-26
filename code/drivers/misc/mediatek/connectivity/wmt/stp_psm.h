/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN STP power-saving engine (PSM).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/psm_core.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/include/psm_core.h
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The engine puts the connectivity MCU to sleep with the WMT SLEEP command
 * once the STP link has been idle for STP_PSM_IDLE_TIME_SLEEP ms and wakes it
 * up with the WMT WAKEUP/HOST_AWAKE handshake when a channel needs to
 * transmit.  While the MCU is asleep, payload is queued in a hold FIFO and
 * released after the wakeup event.
 *
 * Scope of this port vs. the downstream thread-based design:
 *   - the downstream op queue + "mtk_stp_psm" kthread are replaced by a
 *     workqueue plus a small request ring; PSM never sleeps with a spinlock
 *     held and all WMT traffic is issued from process context under the
 *     existing WMT op_lock,
 *   - the MMC/SDIO ownership and AP deep-idle hooks do not exist in this
 *     kernel; the equivalent "leave deep idle" step is the BTIF DPIDLE
 *     control of the STP channel,
 *   - the early-suspend state used by quick-PS is provided by a PM notifier.
 */

#ifndef _STP_PSM_H_
#define _STP_PSM_H_

#include <linux/types.h>

#include <mtk_wcn_stp.h>

/* psm_core.h:52-60, 69 */
#define STP_PSM_FIFO_SIZE		0x2000	/* 8 KiB hold FIFO */
#define STP_PSM_TX_SIZE			0x800	/* 2 KiB release scratch */
#define STP_PSM_PACKET_SIZE_MAX		2000
#define STP_PSM_IDLE_TIME_SLEEP		30	/* ms */
#define STP_PSM_IDLE_TIME_SLEEP_1000	1000	/* ms, high traffic */
#define STP_PSM_WAIT_EVENT_TIMEOUT	6000	/* ms */
#define STP_PSM_SAMPLE_DURATION_MS	1000
#define STP_PSM_RTX_SPEED_THRESHOLD	50000	/* bytes per sample */

/* psm_core.h:90-98: flag bit indices (not masks). */
#define STP_PSM_WMT_EVENT_SLEEP_EN			0
#define STP_PSM_WMT_EVENT_WAKEUP_EN			1
#define STP_PSM_BLOCK_DATA_EN				2
#define STP_PSM_WMT_EVENT_DISABLE_MONITOR		3
#define STP_PSM_WMT_EVENT_ROLL_BACK_EN			4
#define STP_PSM_RESET_EN				5
#define STP_PSM_WMT_EVENT_HOST_WAKEUP_EN		6
#define STP_PSM_WMT_EVENT_DISABLE_MONITOR_TX_HIGH_DENSITY 7
#define STP_PSM_WMT_EVENT_DISABLE_MONITOR_RX_HIGH_DENSITY 8

/* psm_core.h:107-113 */
#define STP_OP_BUF_SIZE		16
#define STP_PSM_DBG_SIZE	16

struct stp_psm;

/* One-shot lifecycle, called by stp_drv_init()/stp_drv_exit(). */
int stp_psm_init(void);
void stp_psm_deinit(void);

/* Data path hooks used by stp_core.c.  stp_psm_tx_begin() returns > 0 when
 * the packet was queued in the hold FIFO (the caller must not transmit it),
 * 0 when the caller must transmit while holding @locked, or a negative errno.
 */
int stp_psm_tx_begin(const u8 *buffer, u32 len, u8 type, bool *locked);
void stp_psm_tx_end(u8 type, bool locked);
void stp_psm_rx_done(u8 type, int length);
int stp_psm_hold_data(const u8 *buffer, u32 len, u8 type);
int stp_psm_release_data(void);
int stp_psm_has_pending_data(void);
bool stp_psm_is_to_block_traffic(void);
int stp_psm_start_monitor(void);
void stp_psm_disable_by_tx_rx_density(int dir, int length);

/* State / control used by stp_core.c and wmt_core.c. */
int stp_psm_set_sleep_enable(void);
int stp_psm_set_sleep_disable(void);
int stp_psm_check_sleep_enable(void);
int stp_psm_do_wakeup(void);
int stp_psm_sleep_for_thermal(void);
int stp_psm_is_disable(void);

/* Completion notification from the WMT engine (wmt_core.c). */
int stp_psm_notify_action_done(enum mtk_wcn_stp_psm_action action, bool ok);

/* Quick-PS capability: SOC supports it and the system is suspending. */
void stp_psm_set_pm_suspend(bool suspending);

/* Async control used from contexts that hold g_wmt.op_lock; they only queue
 * a request for the PSM workqueue.  stp_psm_reapply() restores the state a
 * user request asked for after a chip reset/re-init.
 */
void stp_psm_request_enable(int idle_ms);
void stp_psm_request_disable(void);
void stp_psm_reapply(void);

#endif /* _STP_PSM_H_ */
