// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN firmware coredump / assert collector.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/btm_core.c
 *     (STP_OPID_BTM_PAGED_DUMP handler, btm_core.c:492-670, and the netlink
 *      forwarder btm_core.c:189-242)
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/stp_core.c
 *     (MTKSTP_FW_MSG assert path, stp_core.c:1670-1780;
 *      mtk_wcn_stp_coredump_start_ctrl, stp_core.c:2342-2375)
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * When the firmware asserts it sends an STP_TASK_INDX firmware message.  The
 * STP layer blocks all further TX (f_coredump) and calls stp_btm_assert().
 * The collector then walks the CONSYS paged-dump protocol in the shared EMI
 * window:
 *
 *	host_sync_state = GET	-> chip_sync_state must be PUT_DONE
 *	read chip_sync_addr / chip_sync_len
 *	copy the page through the AP mapping of the EMI window
 *	host_sync_num++ ; host_sync_state = GET_DONE
 *	wait chip_sync_state == END, repeat until counter > page_num and
 *	chip_paged_dump_end is set
 *
 * Each page is streamed to userspace with the downstream STP_DBG framing
 * ('[M]' | u16 len | data) and the whole-chip WMT reset/recovery is requested
 * when the collection finishes, times out (STP_CORE_DUMP_TIMEOUT) or cannot
 * run because the EMI window is unavailable.
 *
 * The proprietary MTK AEE backend is absent from this kernel: the netlink
 * stream is the only sink.  dev_coredump is not used because the WMT core has
 * no struct device of its own (downstream also delivered through its own
 * userspace ABI, stp_dbg_aee_send()/stp_dbg_nl_send()).
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include "wmt_local.h"
#include "stp_psm.h"

/* stp_dbg.h:127 */
#define STP_CORE_DUMP_TIMEOUT		(5 * 60 * 1000)
/* btm_core.c:186-187 */
#define STP_BTM_SUB_PKT_SIZE		1024
#define STP_BTM_SUB_PKT_HEADER		5	/* '[', 'M', ']', u16 len */
#define STP_BTM_PAGE_BUF_SIZE		(32 * 1024)
/* btm_core.c:504: default page count when the register reads back zero. */
#define STP_BTM_DEFAULT_PACKET_NUM	35
/* btm_core.c:546-670 polling budget (10 x 10 ms / 10 x 5 ms). */
#define STP_BTM_STATE_RETRY		10
#define STP_BTM_STATE_DELAY_MS		10
#define STP_BTM_SETTLE_MS		100

struct stp_btm {
	struct mutex lock;		/* serialises collectors */
	spinlock_t state_lock;		/* active/abort flags */
	struct work_struct work;
	struct timer_list timer;
	wait_queue_head_t wq;

	u8 *page_buf;

	/* Assert context captured by the parser. */
	char assert_info[256];
	u32 assert_len;
	u32 chip_id;
	u32 page_count;
	u32 dump_len;

	bool initialized;
	bool active;			/* collection queued/running */
	bool abort;
	bool reset_done;
};

static struct stp_btm btm;

#define BTM_ERR(fmt, ...)	pr_err("[STP-BTM][E]%s: " fmt, __func__, ##__VA_ARGS__)
#define BTM_WRN(fmt, ...)	pr_warn("[STP-BTM][W]%s: " fmt, __func__, ##__VA_ARGS__)
#define BTM_INF(fmt, ...)	pr_info("[STP-BTM][I]%s: " fmt, __func__, ##__VA_ARGS__)
#define BTM_DBG(fmt, ...)	pr_debug("[STP-BTM][D]%s: " fmt, __func__, ##__VA_ARGS__)

static void stp_btm_schedule_reset(void)
{
	unsigned long flags;
	bool first = false;

	spin_lock_irqsave(&btm.state_lock, flags);
	if (!btm.reset_done) {
		btm.reset_done = true;
		first = true;
	}
	spin_unlock_irqrestore(&btm.state_lock, flags);

	if (first) {
		BTM_INF("requesting whole-chip recovery\n");
		wmt_lib_schedule_reset(WMTRSTSRC_RESET_STP);
	}
}

/* One '[' 'M' ']' | u16 len | payload record (btm_core.c:189-242). */
static int stp_btm_send_record(const u8 *data, u32 len)
{
	u8 frame[STP_BTM_SUB_PKT_SIZE + STP_BTM_SUB_PKT_HEADER];
	u32 remain = len;
	u32 offset = 0;

	while (remain) {
		u32 chunk = min(remain, (u32)STP_BTM_SUB_PKT_SIZE);
		u16 wire_len = chunk;

		frame[0] = '[';
		frame[1] = 'M';
		frame[2] = ']';
		memcpy(&frame[3], &wire_len, sizeof(wire_len));
		memcpy(&frame[STP_BTM_SUB_PKT_HEADER], data + offset, chunk);

		stp_dbg_nl_send(frame, STP_DBG_NL_CMD_DUMP,
				STP_BTM_SUB_PKT_HEADER + chunk);

		offset += chunk;
		remain -= chunk;
	}

	return 0;
}

static u32 stp_btm_read_ctrl(u32 offset)
{
	return wmt_plat_get_dump_info(offset);
}

static bool stp_btm_wait_chip_state(enum wmt_chip_dump_state want)
{
	int i;

	for (i = 0; i < STP_BTM_STATE_RETRY; i++) {
		if (btm.abort)
			return false;
		if ((enum wmt_chip_dump_state)stp_btm_read_ctrl(
			    WMT_EMI_CTRL_CHIP_SYNC_STATE) == want)
			return true;
		msleep(STP_BTM_STATE_DELAY_MS);
	}

	return false;
}

/* btm_core.c:520-600 */
static void stp_btm_collect(void)
{
	enum wmt_chip_dump_state chip_state;
	u32 packet_num, counter = 0;
	int ret;

	btm.abort = false;

	/* Let the firmware settle after the assert message (btm_core.c:530). */
	msleep(STP_BTM_SETTLE_MS);

	packet_num = stp_btm_read_ctrl(WMT_EMI_CTRL_CHIP_PAGE_DUMP_NUM);
	if (!packet_num) {
		packet_num = STP_BTM_DEFAULT_PACKET_NUM;
		BTM_WRN("page dump count unavailable, assuming %u\n",
			packet_num);
	} else {
		BTM_INF("page dump count %u\n", packet_num);
	}

	wmt_plat_set_host_dump_state(WMT_HOST_DUMP_NOT_START);

	for (;;) {
		u32 dump_addr, dump_len, offset;
		void __iomem *virt;

		if (btm.abort) {
			BTM_WRN("collection aborted\n");
			break;
		}

		chip_state = (enum wmt_chip_dump_state)stp_btm_read_ctrl(
			WMT_EMI_CTRL_CHIP_SYNC_STATE);
		if (chip_state != WMT_CHIP_DUMP_PUT_DONE &&
		    !stp_btm_wait_chip_state(WMT_CHIP_DUMP_PUT_DONE)) {
			BTM_ERR("chip did not provide page %u (state %d)\n",
				counter, chip_state);
			break;
		}

		wmt_plat_set_host_dump_state(WMT_HOST_DUMP_GET);

		dump_addr = stp_btm_read_ctrl(WMT_EMI_CTRL_CHIP_SYNC_ADDR);
		dump_len = stp_btm_read_ctrl(WMT_EMI_CTRL_CHIP_SYNC_LEN);

		/* Validate against the mapped window before touching memory:
		 * a corrupt assert-time register must not walk the kernel out
		 * of the reserved region.
		 */
		if (dump_addr < WMT_EMI_FW_PHY_BASE ||
		    dump_addr - WMT_EMI_FW_PHY_BASE >= WMT_EMI_MEM_SIZE) {
			BTM_ERR("page %u address 0x%08x outside the EMI window\n",
				counter, dump_addr);
			break;
		}
		offset = dump_addr - WMT_EMI_FW_PHY_BASE;

		if (!dump_len || dump_len > STP_BTM_PAGE_BUF_SIZE ||
		    offset + dump_len > WMT_EMI_MEM_SIZE) {
			BTM_ERR("page %u length %u invalid (offset 0x%x)\n",
				counter, dump_len, offset);
			break;
		}

		virt = wmt_plat_emi_virt(offset);
		if (!virt) {
			BTM_ERR("EMI window not mapped\n");
			break;
		}

		memcpy_fromio(btm.page_buf, virt, dump_len);
		BTM_INF("page %u: addr 0x%08x len %u\n", counter, dump_addr,
			dump_len);

		ret = stp_btm_send_record(btm.page_buf, dump_len);
		if (ret)
			BTM_WRN("userspace delivery failed (%d)\n", ret);

		btm.dump_len += dump_len;
		btm.page_count++;

		wmt_plat_update_host_sync_num();
		wmt_plat_set_host_dump_state(WMT_HOST_DUMP_GET_DONE);

		/* Wait for the chip to mark this page consumed (btm_core.c:625). */
		if (!stp_btm_wait_chip_state(WMT_CHIP_DUMP_END)) {
			chip_state = (enum wmt_chip_dump_state)stp_btm_read_ctrl(
				WMT_EMI_CTRL_CHIP_SYNC_STATE);
			if (chip_state != WMT_CHIP_DUMP_END) {
				BTM_WRN("chip did not finish page %u\n",
					counter);
				wmt_plat_set_host_dump_state(
					WMT_HOST_DUMP_NOT_START);
				break;
			}
		}

		wmt_plat_set_host_dump_state(WMT_HOST_DUMP_NOT_START);
		counter++;

		if (counter > packet_num) {
			if (stp_btm_read_ctrl(
				    WMT_EMI_CTRL_CHIP_PAGED_DUMP_END)) {
				BTM_INF("paged dump complete: %u pages, %u bytes\n",
					btm.page_count, btm.dump_len);
				wmt_plat_set_host_dump_state(
					WMT_HOST_DUMP_END);
			} else {
				BTM_ERR("paged dump not terminated by the chip\n");
			}
			break;
		}

	}

	if (!btm.page_count)
		BTM_ERR("no dump page collected (%u bytes)\n", btm.dump_len);
}

static void stp_btm_work_fn(struct work_struct *work)
{
	mutex_lock(&btm.lock);
	stp_btm_collect();
	btm.active = false;
	timer_delete_sync(&btm.timer);
	mutex_unlock(&btm.lock);

	wake_up_all(&btm.wq);
	stp_btm_schedule_reset();
}

static void stp_btm_timeout_cb(struct timer_list *timer)
{
	if (!READ_ONCE(btm.active))
		return;

	BTM_ERR("coredump timed out after %u ms, flushing\n",
		STP_CORE_DUMP_TIMEOUT);
	btm.abort = true;
	wake_up_all(&btm.wq);
	stp_btm_schedule_reset();
}

static void stp_btm_log_assert(void)
{
	char line[65];
	u32 i, n = 0;

	BTM_ERR("firmware assert: %u bytes\n", btm.assert_len);

	if (!btm.assert_len)
		return;

	/* Keep the printable part in one line for logcat/ramoops users and
	 * dump the raw bytes at debug level.
	 */
	for (i = 0; i < btm.assert_len && n < sizeof(line) - 1; i++) {
		char c = btm.assert_info[i];

		line[n++] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	line[n] = '\0';
	pr_err("[STP-BTM] %s\n", line);
	print_hex_dump(KERN_DEBUG, "[STP-BTM] ", DUMP_PREFIX_OFFSET, 16, 1,
		       btm.assert_info, btm.assert_len, false);
}

/*
 * API
 */

bool stp_btm_active(void)
{
	return btm.initialized && btm.active;
}

void stp_btm_assert(const u8 *info, u32 len)
{
	unsigned long flags;

	if (!btm.initialized || !btm.page_buf)
		return;

	spin_lock_irqsave(&btm.state_lock, flags);
	if (btm.active) {
		spin_unlock_irqrestore(&btm.state_lock, flags);
		BTM_WRN("assert already being collected\n");
		return;
	}
	btm.active = true;
	btm.abort = false;
	btm.reset_done = false;
	btm.page_count = 0;
	btm.dump_len = 0;
	btm.assert_len = min_t(u32, len, sizeof(btm.assert_info) - 1);
	if (info && btm.assert_len)
		memcpy(btm.assert_info, info, btm.assert_len);
	btm.assert_info[btm.assert_len] = '\0';
	spin_unlock_irqrestore(&btm.state_lock, flags);

	stp_btm_log_assert();

	/* Downstream also forwards the firmware message itself as an FW DMP
	 * packet (stp_dbg.c:1187-1206 stp_dbg_log_pkt(STP_DBG_FW_DMP, ...),
	 * btm_core.c:150-185).  Send it before the paged dump so that the
	 * daemon can print the assert reason.
	 */
	if (btm.assert_len)
		stp_btm_send_record((const u8 *)btm.assert_info, btm.assert_len);

	if (!mtk_wcn_stp_coredump_flag_get()) {
		BTM_WRN("coredump collection disabled by userspace\n");
		goto no_dump;
	}

	if (!wmt_plat_emi_virt(0)) {
		BTM_ERR("no CONSYS shared memory window, cannot collect the dump\n");
		goto no_dump;
	}

	mod_timer(&btm.timer,
		  jiffies + msecs_to_jiffies(STP_CORE_DUMP_TIMEOUT));
	schedule_work(&btm.work);
	return;

no_dump:
	spin_lock_irqsave(&btm.state_lock, flags);
	btm.active = false;
	spin_unlock_irqrestore(&btm.state_lock, flags);
	stp_btm_schedule_reset();
}

void stp_btm_reset(void)
{
	unsigned long flags;

	if (!btm.initialized)
		return;

	spin_lock_irqsave(&btm.state_lock, flags);
	btm.abort = true;
	btm.active = false;
	btm.reset_done = false;
	btm.page_count = 0;
	btm.dump_len = 0;
	btm.assert_len = 0;
	spin_unlock_irqrestore(&btm.state_lock, flags);

	timer_delete_sync(&btm.timer);

	/* mtk_wcn_stp_ctx_restore() (stp_core.c:3312-3322): the dump is over,
	 * allow the MCU to sleep again.
	 */
	stp_psm_set_sleep_enable();
}

int stp_btm_init(void)
{
	int ret;

	memset(&btm, 0, sizeof(btm));
	mutex_init(&btm.lock);
	spin_lock_init(&btm.state_lock);
	init_waitqueue_head(&btm.wq);
	INIT_WORK(&btm.work, stp_btm_work_fn);
	timer_setup(&btm.timer, stp_btm_timeout_cb, 0);

	btm.page_buf = kmalloc(STP_BTM_PAGE_BUF_SIZE, GFP_KERNEL);
	if (!btm.page_buf) {
		mutex_destroy(&btm.lock);
		return -ENOMEM;
	}

	ret = stp_dbg_nl_init();
	if (ret) {
		kfree(btm.page_buf);
		btm.page_buf = NULL;
		mutex_destroy(&btm.lock);
		return ret;
	}

	btm.initialized = true;
	BTM_INF("coredump collector ready (timeout %u ms)\n",
		STP_CORE_DUMP_TIMEOUT);
	return 0;
}

void stp_btm_deinit(void)
{
	if (!btm.initialized)
		return;

	btm.initialized = false;
	btm.abort = true;
	timer_delete_sync(&btm.timer);
	cancel_work_sync(&btm.work);
	stp_dbg_nl_deinit();
	kfree(btm.page_buf);
	btm.page_buf = NULL;
	mutex_destroy(&btm.lock);
}
