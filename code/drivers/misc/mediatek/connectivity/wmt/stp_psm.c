// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN STP power-saving engine (PSM).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/psm_core.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/include/psm_core.h
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_lib.c
 *     (wmt_lib_ps_handler, wmt_lib_ps_action)
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * Port notes:
 *   - The downstream "mtk_stp_psm" thread and its op queue are replaced by a
 *     single-threaded workqueue and a redundant-filtered request ring.  The
 *     four-state machine, the flag handshake and the WMT SLEEP / WAKEUP /
 *     HOST_AWAKE command sequence are unchanged (psm_core.c:1000-1380).
 *   - Every WMT exchange is issued through wmt_lib_run_op(), i.e. on the
 *     existing WMT operation thread under g_wmt.op_lock.  PSM never calls a
 *     WMT operation while the caller holds op_lock: the internal entry points
 *     only queue a request (see stp_psm_request_*).
 *   - The MMC/SDIO ownership handshake and the AP deep-idle callback of the
 *     downstream handler do not exist here; the sleep side effect is the BTIF
 *     DPIDLE control of the STP channel, which is the same hardware state the
 *     downstream driver reached through mt_combo_plt_enter_deep_idle().
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "wmt_local.h"
#include "stp_psm.h"

/* Request ring entry.  The order of the values matters: ENABLE/DISABLE are
 * control requests, the rest are the downstream PSM actions.
 */
enum psm_req {
	PSM_REQ_ENABLE = 0x80,
	PSM_REQ_DISABLE = 0x81,
};

struct stp_psm {
	struct mutex api_lock;		/* data path <-> state machine */
	spinlock_t lock;		/* state, flags, FIFO, requests */

	enum mtk_wcn_stp_psm_state state;
	unsigned long flags;
	int sleep_en;
	u32 idle_time;

	bool initialized;
	bool enabled;			/* monitor armed by the current boot */
	bool user_enabled;		/* requested through the public API */
	u32 user_idle_time;
	bool quick_ps_support;
	bool pm_suspending;

	struct timer_list timer;
	struct work_struct work;
	wait_queue_head_t wmt_wq;
	unsigned int wmt_event;		/* bit mask of STP_PSM_WMT_EVENT_*_EN */

	u8 req[STP_OP_BUF_SIZE];
	unsigned int req_head;
	unsigned int req_count;
	u8 last_done;

	u8 hold[STP_PSM_FIFO_SIZE];
	u32 hold_len;
	u8 out_buf[STP_PSM_TX_SIZE];

	/* Traffic density sampling (psm_core.c:1401-1490). */
	ktime_t sample_end;
	bool sample_start;
	int tx_sum_len;
	int rx_sum_len;

	int wl_count;
	int last_err;			/* result of the last enable/disable */
	struct notifier_block pm_nb;
};

static struct stp_psm *psm;
static unsigned int psm_dbg_level = 2;
module_param_named(psm_dbg_level, psm_dbg_level, uint, 0644);
MODULE_PARM_DESC(psm_dbg_level, "STP PSM trace level, 0=err .. 4=loud");

static bool psm_quick_ps = true;
module_param_named(quick_ps, psm_quick_ps, bool, 0644);
MODULE_PARM_DESC(quick_ps, "allow the quick-PS path while the system suspends");

#define PSM_ERR(fmt, ...)	pr_err("[STP-PSM][E]%s: " fmt, __func__, ##__VA_ARGS__)
#define PSM_WRN(fmt, ...)	do { if (psm_dbg_level >= 1) \
		pr_warn("[STP-PSM][W]%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define PSM_INF(fmt, ...)	do { if (psm_dbg_level >= 2) \
		pr_info("[STP-PSM][I]%s: " fmt, __func__, ##__VA_ARGS__); } while (0)
#define PSM_DBG(fmt, ...)	do { if (psm_dbg_level >= 3) \
		pr_debug("[STP-PSM][D]%s: " fmt, __func__, ##__VA_ARGS__); } while (0)

static const char * const psm_state_name[] = {
	"ACT", "ACT_INACT", "INACT", "INACT_ACT",
};

/*
 * Small helpers
 */

static bool psm_flag(int bit)
{
	unsigned long flags;
	bool set;

	spin_lock_irqsave(&psm->lock, flags);
	set = test_bit(bit, &psm->flags);
	spin_unlock_irqrestore(&psm->lock, flags);
	return set;
}

static void psm_set_flag(int bit, bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&psm->lock, flags);
	if (on)
		set_bit(bit, &psm->flags);
	else
		clear_bit(bit, &psm->flags);
	spin_unlock_irqrestore(&psm->lock, flags);
}

static enum mtk_wcn_stp_psm_state psm_get_state(void)
{
	unsigned long flags;
	enum mtk_wcn_stp_psm_state state;

	spin_lock_irqsave(&psm->lock, flags);
	state = psm->state;
	spin_unlock_irqrestore(&psm->lock, flags);
	return state;
}

static void psm_set_state_internal(enum mtk_wcn_stp_psm_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&psm->lock, flags);
	psm->state = state;
	/* psm_core.c:640-660: leaving ACT blocks traffic. */
	if (state != MTK_WCN_STP_PSM_ACT)
		set_bit(STP_PSM_BLOCK_DATA_EN, &psm->flags);
	spin_unlock_irqrestore(&psm->lock, flags);
	PSM_DBG("state -> %s\n", psm_state_name[state]);
}

/* Wake lock, nesting counted locally (platform helper is not nesting-safe
 * for unbalanced calls).
 */
static void psm_wake_lock_get(void)
{
	unsigned long flags;
	bool get = false;

	spin_lock_irqsave(&psm->lock, flags);
	if (psm->wl_count++ == 0)
		get = true;
	spin_unlock_irqrestore(&psm->lock, flags);

	if (get)
		wmt_plat_wake_lock_ctrl(WL_OP_GET);
}

static void psm_wake_lock_put(void)
{
	unsigned long flags;
	bool put = false;

	spin_lock_irqsave(&psm->lock, flags);
	if (psm->wl_count > 0 && --psm->wl_count == 0)
		put = true;
	spin_unlock_irqrestore(&psm->lock, flags);

	if (put)
		wmt_plat_wake_lock_ctrl(WL_OP_PUT);
}

/*
 * Request ring (replacement of the downstream op queue)
 */

static bool psm_action_is_wakeup(u8 action)
{
	return action == MTK_WCN_STP_PSM_WAKEUP ||
	       action == MTK_WCN_STP_PSM_HOST_AWAKE;
}

static u8 psm_req_at(unsigned int index)
{
	return psm->req[(psm->req_head + index) % STP_OP_BUF_SIZE];
}

/* psm_core.c:196-260: drop redundant sleep/wakeup requests. */
static bool psm_req_is_redundant(u8 action)
{
	u8 prev;

	if (action == PSM_REQ_ENABLE || action == PSM_REQ_DISABLE)
		return false;

	if (psm->req_count)
		prev = psm_req_at(psm->req_count - 1);
	else
		prev = psm->last_done;

	if (action == MTK_WCN_STP_PSM_SLEEP)
		return prev == MTK_WCN_STP_PSM_SLEEP;

	if (psm_action_is_wakeup(action))
		return psm_action_is_wakeup(prev);

	return false;
}

/* psm_core.c:262-290 */
static void psm_req_cleanup(void)
{
	while (psm->req_count > 2) {
		u8 prev = psm_req_at(psm->req_count - 1);
		u8 prev2 = psm_req_at(psm->req_count - 2);

		if ((prev == MTK_WCN_STP_PSM_SLEEP && psm_action_is_wakeup(prev2)) ||
		    (psm_action_is_wakeup(prev) && prev2 == MTK_WCN_STP_PSM_SLEEP)) {
			psm->req_count -= 2;
		} else if (prev == prev2) {
			psm->req_count--;
		} else {
			break;
		}
	}
}

static void psm_req_put(u8 action)
{
	unsigned long flags;
	unsigned int tail;

	spin_lock_irqsave(&psm->lock, flags);
	if (psm_req_is_redundant(action)) {
		spin_unlock_irqrestore(&psm->lock, flags);
		PSM_DBG("skip redundant request 0x%02x\n", action);
		return;
	}

	if (psm->req_count >= STP_OP_BUF_SIZE) {
		/* Mirror the downstream "active queue full" behaviour: the
		 * newest request is dropped, the state machine still converges
		 * through the idle monitor.
		 */
		spin_unlock_irqrestore(&psm->lock, flags);
		PSM_WRN("request queue full, dropping 0x%02x\n", action);
		return;
	}

	tail = (psm->req_head + psm->req_count) % STP_OP_BUF_SIZE;
	psm->req[tail] = action;
	psm->req_count++;
	psm_req_cleanup();
	spin_unlock_irqrestore(&psm->lock, flags);

	schedule_work(&psm->work);
}

static bool psm_req_get(u8 *action)
{
	unsigned long flags;
	bool got = false;

	spin_lock_irqsave(&psm->lock, flags);
	if (psm->req_count) {
		*action = psm->req[psm->req_head];
		psm->req_head = (psm->req_head + 1) % STP_OP_BUF_SIZE;
		psm->req_count--;
		got = true;
	}
	spin_unlock_irqrestore(&psm->lock, flags);

	return got;
}

static void psm_req_drain(void)
{
	unsigned long flags;

	spin_lock_irqsave(&psm->lock, flags);
	psm->req_count = 0;
	spin_unlock_irqrestore(&psm->lock, flags);
}

/*
 * Hold FIFO (psm_core.c:745-860)
 */

static int psm_hold_data_locked(const u8 *buffer, u32 len, u8 type)
{
	unsigned long flags;
	const u8 delimiter[2] = { 0xbb, 0xbb };
	u32 need = len + sizeof(u8) + sizeof(u32) + sizeof(delimiter);

	if (len > STP_PSM_PACKET_SIZE_MAX) {
		PSM_ERR("packet too long (%u)\n", len);
		return -EMSGSIZE;
	}

	spin_lock_irqsave(&psm->lock, flags);
	if (STP_PSM_FIFO_SIZE - psm->hold_len < need) {
		PSM_ERR("hold FIFO full, dropping %u queued bytes\n",
			psm->hold_len);
		psm->hold_len = 0;
	}
	memcpy(psm->hold + psm->hold_len, &type, sizeof(type));
	psm->hold_len += sizeof(type);
	memcpy(psm->hold + psm->hold_len, &len, sizeof(len));
	psm->hold_len += sizeof(len);
	memcpy(psm->hold + psm->hold_len, buffer, len);
	psm->hold_len += len;
	memcpy(psm->hold + psm->hold_len, delimiter, sizeof(delimiter));
	psm->hold_len += sizeof(delimiter);
	spin_unlock_irqrestore(&psm->lock, flags);

	return len;
}

static int psm_has_pending_data(void)
{
	unsigned long flags;
	u32 len;

	spin_lock_irqsave(&psm->lock, flags);
	len = psm->hold_len;
	spin_unlock_irqrestore(&psm->lock, flags);
	return len;
}

/* Release queued packets to STP without re-entering PSM.  The caller holds
 * api_lock, so no other context can consume the FIFO concurrently.
 */
static int psm_release_data(void)
{
	int i;

	for (i = 0; i < 20 && psm_has_pending_data(); i++) {
		unsigned long flags;
		u8 type = 0;
		u32 len = 0;
		u8 delimiter[2] = { 0, 0 };
		u32 avail;

		spin_lock_irqsave(&psm->lock, flags);
		avail = psm->hold_len;

		memcpy(&type, psm->hold, sizeof(type));
		memcpy(&len, psm->hold + sizeof(type), sizeof(len));

		if (len > STP_PSM_PACKET_SIZE_MAX ||
		    sizeof(type) + sizeof(len) + len + sizeof(delimiter) > avail) {
			psm->hold_len = 0;
			spin_unlock_irqrestore(&psm->lock, flags);
			PSM_ERR("corrupt hold FIFO, dropped\n");
			return -EIO;
		}

		memcpy(psm->out_buf, psm->hold + sizeof(type) + sizeof(len),
		       len);
		memcpy(delimiter,
		       psm->hold + sizeof(type) + sizeof(len) + len,
		       sizeof(delimiter));
		memmove(psm->hold,
			psm->hold + sizeof(type) + sizeof(len) + len +
				sizeof(delimiter),
			avail - (sizeof(type) + sizeof(len) + len +
				 sizeof(delimiter)));
		psm->hold_len -= sizeof(type) + sizeof(len) + len +
				 sizeof(delimiter);
		spin_unlock_irqrestore(&psm->lock, flags);

		if (delimiter[0] != 0xbb || delimiter[1] != 0xbb) {
			PSM_ERR("hold FIFO delimiter missing, packet dropped\n");
			continue;
		}

		PSM_DBG("release %u bytes on channel %u\n", len, type);
		mtk_wcn_stp_send_data_no_ps(psm->out_buf, len, type);
	}

	return 0;
}

/*
 * Idle monitor (psm_core.c:662-760)
 */

static void psm_stop_monitor(void)
{
	timer_delete_sync(&psm->timer);
}

int stp_psm_start_monitor(void)
{
	unsigned long flags;
	u32 idle;

	if (!psm || !psm->initialized)
		return -ENODEV;

	if (psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR))
		return 0;

	spin_lock_irqsave(&psm->lock, flags);
	idle = psm->idle_time;
	spin_unlock_irqrestore(&psm->lock, flags);

	PSM_DBG("start monitor (%u ms)\n", idle);
	mod_timer(&psm->timer, jiffies + msecs_to_jiffies(idle));
	return 0;
}

static void psm_idle_cb(struct timer_list *timer)
{
	if (!psm || !psm->initialized)
		return;

	psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR_RX_HIGH_DENSITY, false);
	psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR_TX_HIGH_DENSITY, false);

	if (psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR))
		return;

	PSM_INF("idle for %u ms, requesting sleep\n", psm->idle_time);
	psm_req_put(MTK_WCN_STP_PSM_SLEEP);
}

/*
 * WMT handshake
 */

static int psm_wmt_notify(enum mtk_wcn_stp_psm_action action)
{
	int ret;

	switch (action) {
	case MTK_WCN_STP_PSM_SLEEP:
		PSM_DBG("send WMT SLEEP\n");
		break;
	case MTK_WCN_STP_PSM_WAKEUP:
	case MTK_WCN_STP_PSM_HOST_AWAKE:
		PSM_DBG("send WMT %s\n",
			action == MTK_WCN_STP_PSM_WAKEUP ? "WAKEUP" : "HOST_AWAKE");
		break;
	default:
		return -EINVAL;
	}

	ret = wmt_core_psm_action(action);
	if (ret) {
		u32 reason;

		PSM_WRN("WMT action %d failed (%d)\n", action, ret);
		switch (action) {
		case MTK_WCN_STP_PSM_SLEEP:
			reason = 33;
			break;
		case MTK_WCN_STP_PSM_WAKEUP:
			reason = 34;
			break;
		default:
			reason = 35;
			break;
		}
		if (mtk_wcn_stp_is_ready()) {
			/* Downstream wmt_core.c:1166-1290 triggers the WMT
			 * error-assert recovery when the peer does not answer
			 * the power-saving command.
			 */
			wmt_core_evt_err_trg_assert(reason);
		}
	}

	return ret;
}

/* psm_core.c:1230-1320: event processing after the WMT round trip. */
static void psm_wmt_event_done(void)
{
	unsigned long flags;
	unsigned int event;

	spin_lock_irqsave(&psm->lock, flags);
	event = psm->wmt_event;
	psm->wmt_event = 0;
	spin_unlock_irqrestore(&psm->lock, flags);

	if (event & BIT(STP_PSM_WMT_EVENT_WAKEUP_EN)) {
		stp_psm_release_data();
		psm_set_flag(STP_PSM_BLOCK_DATA_EN, false);
		psm_set_state_internal(MTK_WCN_STP_PSM_ACT);

		if (mtk_wcn_stp_psm_is_quick_ps_support())
			psm_req_put(MTK_WCN_STP_PSM_SLEEP);
		else
			stp_psm_start_monitor();
	} else if (event & BIT(STP_PSM_WMT_EVENT_SLEEP_EN)) {
		psm_set_state_internal(MTK_WCN_STP_PSM_INACT);
		/* Arm the wakeup line before the MCU may sleep, so that
		 * firmware-originated traffic can still wake the AP
		 * (wmt_lib.c:690-700).
		 */
		wmt_plat_eirq_ctrl(true);
		/* The MCU is asleep: let the BTIF controller enter deep idle
		 * too, exactly what mt_combo_plt_enter_deep_idle(COMBO_IF_BTIF)
		 * did downstream.
		 */
		stp_btif_dpidle_ctrl(true);
		psm_wake_lock_put();
	} else if (event & BIT(STP_PSM_WMT_EVENT_ROLL_BACK_EN)) {
		enum mtk_wcn_stp_psm_state state = psm_get_state();

		if (state == MTK_WCN_STP_PSM_ACT_INACT) {
			stp_psm_release_data();
			psm_set_flag(STP_PSM_BLOCK_DATA_EN, false);
			psm_set_state_internal(MTK_WCN_STP_PSM_ACT);
		} else if (state == MTK_WCN_STP_PSM_INACT_ACT) {
			psm_set_state_internal(MTK_WCN_STP_PSM_INACT);
			PSM_INF("wakeup rolled back, staying asleep\n");
		}
	} else if (event & BIT(STP_PSM_RESET_EN)) {
		PSM_DBG("reset event\n");
	} else {
		PSM_WRN("no PSM event, state %s\n",
			psm_state_name[psm_get_state()]);
	}
}

/* psm_core.c:956-1220: the state machine proper. */
static void psm_notify_wmt(enum mtk_wcn_stp_psm_action action)
{
	enum mtk_wcn_stp_psm_state state = psm_get_state();
	int ret;

	if (state == MTK_WCN_STP_PSM_ACT) {
		if (action == MTK_WCN_STP_PSM_SLEEP) {
			if (psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR)) {
				PSM_DBG("monitor disabled, no sleep\n");
				return;
			}

			psm_set_state_internal(MTK_WCN_STP_PSM_ACT_INACT);
			stp_psm_release_data();

			ret = psm_wmt_notify(MTK_WCN_STP_PSM_SLEEP);
			stp_psm_notify_action_done(MTK_WCN_STP_PSM_SLEEP, !ret);
			psm_wmt_event_done();
		} else if (psm_action_is_wakeup(action)) {
			PSM_DBG("already awake, release pending data\n");
			stp_psm_release_data();
		} else {
			PSM_WRN("invalid action %d in ACT\n", action);
		}
		return;
	}

	if (state == MTK_WCN_STP_PSM_INACT) {
		if (psm_action_is_wakeup(action)) {
			psm_set_state_internal(MTK_WCN_STP_PSM_INACT_ACT);
			psm_wake_lock_get();
			wmt_plat_eirq_ctrl(false);
			stp_btif_dpidle_ctrl(false);

			ret = psm_wmt_notify(action);
			stp_psm_notify_action_done(action, !ret);
			psm_wmt_event_done();
		} else if (action == MTK_WCN_STP_PSM_SLEEP) {
			PSM_DBG("already asleep\n");
		} else {
			PSM_WRN("invalid action %d in INACT\n", action);
		}
		return;
	}

	/* ACT_INACT / INACT_ACT: a request is already in flight.  The event
	 * handler will converge the state, and the next request re-runs.
	 */
	PSM_DBG("action %d ignored in state %s\n", action,
		psm_state_name[state]);
}

/* psm_core.c:1234-1287 */
int stp_psm_do_wakeup(void)
{
	int retry;

	if (!psm || !psm->initialized)
		return -ENODEV;

	psm_stop_monitor();
	psm_req_drain();

	for (retry = 0; retry < 10; retry++) {
		psm_notify_wmt(MTK_WCN_STP_PSM_WAKEUP);
		if (psm_get_state() == MTK_WCN_STP_PSM_ACT) {
			/* Idempotent: the line is masked while the MCU is
			 * awake (psm_core.c:1050-1090).
			 */
			wmt_plat_eirq_ctrl(false);
			return 0;
		}
		msleep(20);
	}

	PSM_ERR("failed to bring the MCU back to ACT\n");
	return -EIO;
}

static void psm_handle_req(u8 action)
{
	int ret = 0;

	switch (action) {
	case PSM_REQ_ENABLE:
		psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR, true);
		ret = stp_psm_do_wakeup();
		if (!ret) {
			psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR, false);
			psm_wake_lock_get();
			stp_psm_start_monitor();
			PSM_INF("enabled, idle time %u ms\n", psm->idle_time);
		} else {
			PSM_ERR("enable failed, monitor stays off\n");
		}
		break;
	case PSM_REQ_DISABLE:
		psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR, true);
		psm_stop_monitor();
		ret = stp_psm_do_wakeup();
		psm_wake_lock_put();
		PSM_INF("disabled\n");
		break;
	case MTK_WCN_STP_PSM_SLEEP:
		if (stp_psm_check_sleep_enable() > 0 &&
		    !stp_psm_is_disable())
			ret = psm_wmt_notify(MTK_WCN_STP_PSM_SLEEP);
		else
			PSM_DBG("sleep cancelled\n");
		break;
	case MTK_WCN_STP_PSM_WAKEUP:
	case MTK_WCN_STP_PSM_HOST_AWAKE:
		psm_notify_wmt(action);
		break;
	case MTK_WCN_STP_PSM_ROLLBACK:
		psm_set_flag(STP_PSM_WMT_EVENT_ROLL_BACK_EN, true);
		psm_wmt_event_done();
		break;
	default:
		PSM_WRN("unknown request 0x%02x\n", action);
		break;
	}

	psm->last_err = ret;
}

static void psm_work_fn(struct work_struct *work)
{
	u8 action;

	if (!psm->initialized)
		return;

	mutex_lock(&psm->api_lock);
	while (psm_req_get(&action)) {
		psm->last_done = action;
		psm_handle_req(action);
	}
	mutex_unlock(&psm->api_lock);

	wake_up_all(&psm->wmt_wq);
}

/*
 * Data path API
 */

int stp_psm_hold_data(const u8 *buffer, u32 len, u8 type)
{
	if (!psm || !buffer)
		return -EINVAL;

	return psm_hold_data_locked(buffer, len, type);
}

int stp_psm_release_data(void)
{
	if (!psm)
		return -ENODEV;

	return psm_release_data();
}

int stp_psm_has_pending_data(void)
{
	return psm ? psm_has_pending_data() : 0;
}

bool stp_psm_is_to_block_traffic(void)
{
	return psm && psm_flag(STP_PSM_BLOCK_DATA_EN);
}

int stp_psm_is_disable(void)
{
	return psm && psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR);
}

/*
 * Traffic density: psm_core.c:1460-1490.  High throughput pushes the idle
 * time to one second and keeps the MCU awake instead of toggling it.
 */
void stp_psm_disable_by_tx_rx_density(int dir, int length)
{
	unsigned long flags;
	ktime_t now;
	bool sample_done = false;
	int total;

	if (!psm)
		return;

	now = ktime_get();
	spin_lock_irqsave(&psm->lock, flags);

	if (!psm->sample_start) {
		psm->sample_start = true;
		psm->sample_end = ktime_add_ms(now, STP_PSM_SAMPLE_DURATION_MS);
		psm->tx_sum_len = 0;
		psm->rx_sum_len = 0;
	}
	if (dir)
		psm->rx_sum_len += length;
	else
		psm->tx_sum_len += length;

	if (ktime_after(now, psm->sample_end)) {
		total = psm->tx_sum_len + psm->rx_sum_len;
		if (total > STP_PSM_RTX_SPEED_THRESHOLD) {
			set_bit(STP_PSM_WMT_EVENT_DISABLE_MONITOR_TX_HIGH_DENSITY,
				&psm->flags);
			psm->idle_time = STP_PSM_IDLE_TIME_SLEEP_1000;
		} else {
			psm->idle_time = STP_PSM_IDLE_TIME_SLEEP;
			clear_bit(STP_PSM_WMT_EVENT_DISABLE_MONITOR_TX_HIGH_DENSITY,
				  &psm->flags);
		}
		psm->sample_start = false;
		psm->tx_sum_len = 0;
		psm->rx_sum_len = 0;
		sample_done = true;
	}
	spin_unlock_irqrestore(&psm->lock, flags);

	if (sample_done) {
		PSM_INF("traffic %d bytes/s, idle time %u ms\n", total,
			psm->idle_time);
		/* psm_core.c:1460-1490 restarts the monitor only in the
		 * high-throughput branch, where the idle time was stretched
		 * to STP_PSM_IDLE_TIME_SLEEP_1000.
		 */
		if (total > STP_PSM_RTX_SPEED_THRESHOLD)
			stp_psm_start_monitor();
	}
}

/*
 * TX/RX hooks used by stp_core.c.  The PSM lock is taken across the real STP
 * transmit so that a sleep transition cannot start between the "can send"
 * check and the enqueue (psm_core.c:2436-2510).
 *
 * Like the downstream stp_psm_thread_lock, this is a sleeping lock: every
 * in-tree STP producer (BT hci send, GPS/FM character devices, WMT engine)
 * runs in process context.
 */

int stp_psm_tx_begin(const u8 *buffer, u32 len, u8 type, bool *locked)
{
	bool quick;

	*locked = false;

	if (!psm || !psm->initialized || type == WMT_TASK_INDX)
		return 0;

	quick = mtk_wcn_stp_psm_is_quick_ps_support();
	if (quick)
		stp_psm_disable_by_tx_rx_density(0, len);

	mutex_lock(&psm->api_lock);
	*locked = true;

	if (stp_psm_is_to_block_traffic()) {
		int ret = stp_psm_hold_data(buffer, len, type);

		PSM_INF("MCU asleep, hold %u bytes on channel %u\n", len, type);
		psm_req_put(MTK_WCN_STP_PSM_WAKEUP);
		mutex_unlock(&psm->api_lock);
		*locked = false;
		return ret;
	}

	if (stp_psm_has_pending_data()) {
		PSM_WRN("releasing held data before normal TX\n");
		stp_psm_release_data();
	}

	/* In quick-PS mode the idle monitor is not re-armed: the MCU is put
	 * back to sleep by stp_psm_tx_end() as soon as the frame is out
	 * (psm_core.c:2440-2465).
	 */
	if (!quick)
		stp_psm_start_monitor();
	return 0;
}

void stp_psm_tx_end(u8 type, bool locked)
{
	if (!psm)
		return;

	/* Downstream queues the sleep request before releasing the PSM
	 * thread lock (stp_core.c:2620-2635).
	 */
	if (type != WMT_TASK_INDX &&
	    mtk_wcn_stp_psm_is_quick_ps_support())
		mtk_wcn_stp_psm_notify_wmt_sleep();

	if (locked)
		mutex_unlock(&psm->api_lock);
}

void stp_psm_rx_done(u8 type, int length)
{
	if (!psm || !psm->initialized)
		return;

	if (type != WMT_TASK_INDX && psm->enabled &&
	    mtk_wcn_stp_psm_is_quick_ps_support())
		stp_psm_disable_by_tx_rx_density(1, length);
}

/*
 * Control API
 */

static void psm_request_enable(u32 idle_ms, bool from_user)
{
	unsigned long flags;

	if (!psm || !psm->initialized)
		return;

	if (idle_ms < 1 || idle_ms > 20000)
		idle_ms = STP_PSM_IDLE_TIME_SLEEP;

	spin_lock_irqsave(&psm->lock, flags);
	psm->idle_time = idle_ms;
	psm->sleep_en = 1;
	psm->enabled = true;
	psm->last_err = 0;
	if (from_user) {
		psm->user_enabled = true;
		psm->user_idle_time = idle_ms;
	}
	spin_unlock_irqrestore(&psm->lock, flags);

	psm_req_put(PSM_REQ_ENABLE);
}

static void psm_request_disable(bool from_user)
{
	unsigned long flags;

	if (!psm || !psm->initialized)
		return;

	spin_lock_irqsave(&psm->lock, flags);
	psm->enabled = false;
	psm->last_err = 0;
	if (from_user)
		psm->user_enabled = false;
	spin_unlock_irqrestore(&psm->lock, flags);

	psm_req_put(PSM_REQ_DISABLE);
}

void stp_psm_request_enable(int idle_ms)
{
	psm_request_enable(idle_ms, false);
}

void stp_psm_request_disable(void)
{
	psm_request_disable(false);
}

void stp_psm_reapply(void)
{
	unsigned long flags;
	bool enabled;
	u32 idle;

	if (!psm || !psm->initialized)
		return;

	spin_lock_irqsave(&psm->lock, flags);
	enabled = psm->user_enabled;
	idle = psm->user_idle_time;
	spin_unlock_irqrestore(&psm->lock, flags);

	if (enabled)
		psm_request_enable(idle, false);
	else
		psm_request_disable(false);
}

static int psm_wait_requests(u32 timeout_ms)
{
	if (wait_event_timeout(psm->wmt_wq, !READ_ONCE(psm->req_count),
			       msecs_to_jiffies(timeout_ms)))
		return 0;

	PSM_ERR("request queue not drained within %u ms\n", timeout_ms);
	return -ETIMEDOUT;
}

int stp_psm_set_sleep_enable(void)
{
	if (!psm)
		return -ENODEV;

	psm->sleep_en = 1;
	return 0;
}

int stp_psm_set_sleep_disable(void)
{
	if (!psm)
		return -ENODEV;

	psm->sleep_en = 0;
	return 0;
}

int stp_psm_check_sleep_enable(void)
{
	return psm ? psm->sleep_en : -ENODEV;
}

int stp_psm_sleep_for_thermal(void)
{
	if (!psm || !psm->initialized)
		return -ENODEV;

	if (!psm->enabled || stp_psm_is_disable() ||
	    stp_psm_check_sleep_enable() <= 0)
		return -EOPNOTSUPP;

	psm_req_put(MTK_WCN_STP_PSM_SLEEP);
	return 0;
}

int stp_psm_notify_action_done(enum mtk_wcn_stp_psm_action action, bool ok)
{
	unsigned long flags;
	enum mtk_wcn_stp_psm_state state;
	int bit = -1;

	if (!psm)
		return -ENODEV;

	if (!ok)
		action = MTK_WCN_STP_PSM_ROLLBACK;

	spin_lock_irqsave(&psm->lock, flags);
	state = psm->state;

	/* psm_core.c:1050-1130 */
	if (state == MTK_WCN_STP_PSM_ACT_INACT) {
		if (action == MTK_WCN_STP_PSM_SLEEP) {
			clear_bit(STP_PSM_WMT_EVENT_WAKEUP_EN, &psm->flags);
			set_bit(STP_PSM_WMT_EVENT_SLEEP_EN, &psm->flags);
			bit = STP_PSM_WMT_EVENT_SLEEP_EN;
		} else if (action == MTK_WCN_STP_PSM_ROLLBACK) {
			set_bit(STP_PSM_WMT_EVENT_ROLL_BACK_EN, &psm->flags);
			bit = STP_PSM_WMT_EVENT_ROLL_BACK_EN;
		}
	} else if (state == MTK_WCN_STP_PSM_INACT_ACT) {
		if (psm_action_is_wakeup(action)) {
			clear_bit(STP_PSM_WMT_EVENT_SLEEP_EN, &psm->flags);
			set_bit(STP_PSM_WMT_EVENT_WAKEUP_EN, &psm->flags);
			bit = STP_PSM_WMT_EVENT_WAKEUP_EN;
		} else if (action == MTK_WCN_STP_PSM_ROLLBACK) {
			set_bit(STP_PSM_WMT_EVENT_ROLL_BACK_EN, &psm->flags);
			bit = STP_PSM_WMT_EVENT_ROLL_BACK_EN;
		}
	}
	spin_unlock_irqrestore(&psm->lock, flags);

	if (bit >= 0)
		wake_up_all(&psm->wmt_wq);

	return bit < 0 ? -EINVAL : 0;
}

static bool stp_psm_is_quick_ps_support(void)
{
	return psm && psm->quick_ps_support && psm->pm_suspending;
}

void stp_psm_set_pm_suspend(bool suspending)
{
	if (!psm)
		return;

	psm->pm_suspending = suspending;
	PSM_DBG("system %s\n", suspending ? "suspending" : "resumed");
}

/*
 * Public API (mtk_wcn_stp.h)
 */

int mtk_wcn_stp_psm_enable(int idle_time_ms)
{
	int ret;

	if (!psm || !psm->initialized)
		return -ENODEV;
	if (!g_wmt.ready)
		return -ENODEV;

	psm_request_enable(idle_time_ms, true);

	ret = psm_wait_requests(5000);
	if (ret)
		return ret;

	return psm->last_err;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_enable);

int mtk_wcn_stp_psm_disable(void)
{
	int ret;

	if (!psm || !psm->initialized)
		return -ENODEV;

	psm_request_disable(true);

	ret = psm_wait_requests(5000);
	if (ret)
		return ret;

	return psm->last_err;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_disable);

int mtk_wcn_stp_psm_reset(void)
{
	unsigned long flags;

	if (!psm)
		return -ENODEV;

	/* Called from the STP reset path, possibly while the WMT operation
	 * thread holds g_wmt.op_lock.  It must not wait for the workqueue.
	 */
	psm_set_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR, true);
	timer_delete(&psm->timer);
	psm_req_drain();

	spin_lock_irqsave(&psm->lock, flags);
	psm->flags = 0;
	set_bit(STP_PSM_WMT_EVENT_DISABLE_MONITOR, &psm->flags);
	psm->state = MTK_WCN_STP_PSM_ACT;
	psm->hold_len = 0;
	psm->wmt_event = 0;
	psm->sample_start = false;
	psm->tx_sum_len = 0;
	psm->rx_sum_len = 0;
	psm->enabled = false;
	psm->last_done = 0xff;
	psm->last_err = 0;
	spin_unlock_irqrestore(&psm->lock, flags);

	/* The link is being torn down: mask the wakeup line and leave the
	 * BTIF controller out of deep idle (psm_core.c:769-820).
	 */
	wmt_plat_eirq_ctrl(false);
	stp_btif_dpidle_ctrl(false);
	stp_psm_set_sleep_enable();
	psm_wake_lock_put();
	PSM_INF("reset\n");
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_reset);

int mtk_wcn_stp_psm_set_state(enum mtk_wcn_stp_psm_state state)
{
	unsigned long flags;

	if (!psm || state >= MTK_WCN_STP_PSM_STATE_MAX)
		return -EINVAL;

	spin_lock_irqsave(&psm->lock, flags);
	psm->state = state;
	/* psm_core.c:542-565: BLOCK_DATA is raised on leaving ACT and must be
	 * cleared again when a caller forces ACT.
	 */
	if (state != MTK_WCN_STP_PSM_ACT)
		set_bit(STP_PSM_BLOCK_DATA_EN, &psm->flags);
	else
		clear_bit(STP_PSM_BLOCK_DATA_EN, &psm->flags);
	spin_unlock_irqrestore(&psm->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_set_state);

int mtk_wcn_stp_psm_notify_stp(enum mtk_wcn_stp_psm_action action)
{
	if (!psm)
		return -ENODEV;

	switch (action) {
	case MTK_WCN_STP_PSM_EIRQ:
		/* Unexpected traffic while asleep: wake the MCU. */
		psm_req_put(MTK_WCN_STP_PSM_HOST_AWAKE);
		return 0;
	case MTK_WCN_STP_PSM_SLEEP:
	case MTK_WCN_STP_PSM_WAKEUP:
	case MTK_WCN_STP_PSM_HOST_AWAKE:
	case MTK_WCN_STP_PSM_ROLLBACK:
		return stp_psm_notify_action_done(action, true);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_notify_stp);

int mtk_wcn_stp_psm_notify_wmt_wakeup(void)
{
	if (!psm)
		return -ENODEV;

	psm_req_put(MTK_WCN_STP_PSM_WAKEUP);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_notify_wmt_wakeup);

int mtk_wcn_stp_psm_notify_wmt_sleep(void)
{
	if (!psm)
		return -ENODEV;

	if (psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR) ||
	    psm_flag(STP_PSM_WMT_EVENT_DISABLE_MONITOR_TX_HIGH_DENSITY))
		return 0;

	psm_req_put(MTK_WCN_STP_PSM_SLEEP);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_notify_wmt_sleep);

int mtk_wcn_stp_psm_sleep_for_thermal(void)
{
	return stp_psm_sleep_for_thermal();
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_sleep_for_thermal);

bool mtk_wcn_stp_psm_dbg_level(u32 level)
{
	if (level > 4)
		return false;

	psm_dbg_level = level;
	PSM_INF("debug level %u\n", level);
	return true;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_dbg_level);

bool mtk_wcn_stp_psm_is_quick_ps_support(void)
{
	return stp_psm_is_quick_ps_support();
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_psm_is_quick_ps_support);

/*
 * Lifecycle
 */

static int psm_pm_notify(struct notifier_block *nb, unsigned long event,
			 void *unused)
{
	if (!psm)
		return NOTIFY_DONE;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		stp_psm_set_pm_suspend(true);
		break;
	case PM_POST_SUSPEND:
		stp_psm_set_pm_suspend(false);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

int stp_psm_init(void)
{
	psm = kzalloc(sizeof(*psm), GFP_KERNEL);
	if (!psm)
		return -ENOMEM;

	mutex_init(&psm->api_lock);
	spin_lock_init(&psm->lock);
	init_waitqueue_head(&psm->wmt_wq);
	psm->state = MTK_WCN_STP_PSM_ACT;
	psm->sleep_en = 1;
	psm->idle_time = STP_PSM_IDLE_TIME_SLEEP;
	psm->user_idle_time = STP_PSM_IDLE_TIME_SLEEP;
	psm->quick_ps_support = psm_quick_ps;
	psm->last_done = 0xff;

	timer_setup(&psm->timer, psm_idle_cb, 0);
	INIT_WORK(&psm->work, psm_work_fn);

	psm->pm_nb.notifier_call = psm_pm_notify;
	register_pm_notifier(&psm->pm_nb);

	/* Safe default: monitor disabled, link always awake
	 * (downstream psm_core.c:1865 calls _stp_psm_disable at init too).
	 */
	set_bit(STP_PSM_WMT_EVENT_DISABLE_MONITOR, &psm->flags);
	psm->initialized = true;

	PSM_INF("initialised, quick-PS %s\n",
		psm->quick_ps_support ? "capable" : "off");
	return 0;
}

void stp_psm_deinit(void)
{
	if (!psm)
		return;

	psm->initialized = false;
	unregister_pm_notifier(&psm->pm_nb);
	timer_delete_sync(&psm->timer);
	psm_req_drain();
	cancel_work_sync(&psm->work);

	wmt_plat_eirq_ctrl(false);
	stp_btif_dpidle_ctrl(false);
	psm_wake_lock_put();

	mutex_destroy(&psm->api_lock);
	kfree(psm);
	psm = NULL;
}
