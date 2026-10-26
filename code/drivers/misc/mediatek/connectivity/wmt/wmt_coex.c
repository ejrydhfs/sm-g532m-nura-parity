// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN coexistence engine: firmware debug commands, BGW desense
 * notifications and the BT/Wi-Fi <-> LTE IDC bridge.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_core.c
 *     (opfunc_cmd_test coexistence branch, wmt_core.c:1560-1680;
 *      opfunc_bgw_ds, wmt_core.c:1962-2025;
 *      opfunc_idc_msg_handling, wmt_core.c:2170-2230)
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pub/bgw_desense.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pub/wmt_idc.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/include/wmt_idc.h
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * WMT coexistence protocol (all PDUs live on the WMT STP channel, opcode
 * 0x10):
 *   - COEXDBG: 01 10 02 00 08 <idx>  ->  02 10 <len> 00 00 <payload...>
 *   - BGW DS : 01 0e 0f 00 02 <14 B> ->  02 0e 01 00 00
 *   - IDC TX : 01 10 <u16 len> 00 <opcode> <payload> -> 02 10 01 00 00
 *   - IDC RX : WMT frame with sub-type 0x16 routed to COEX_TASK_INDX
 *
 * The modem side of the IDC path is the MTK conn_md IPC service, which is
 * not present in this kernel.  mtk_wcn_wmt_idc_register() is the explicit
 * boundary: until a CCCI/modem bridge registers, LTE->firmware frames are
 * rejected with -ENODEV and firmware->LTE frames are dropped without
 * fabricating any data (the COEX channel is not even enabled).
 */

#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "wmt_local.h"

#include <mtk_wcn_stp.h>

/* Downstream bgw_desense.h:31-44.  Protocol 17 is the value the vendor daemon
 * uses (the uapi header reserves it without naming it); the downstream header
 * defined NETLINK_TEST to 17.
 */
#define WMT_BGW_NETLINK_PROTO	17
#define WMT_BGW_MAX_MSG_LEN	1024

#define IDC_RX_BUFFER_SIZE	1024
/* wmt_idc.h:26-30 */
#define WMT_IDC_RX_OPCODE_DEBUG_MONITOR	0x02
#define WMT_IDC_HEADER_LEN		5

struct wmt_idc {
	struct mutex lock;
	const struct mtk_wcn_wmt_idc_ops *ops;
	struct work_struct work;
	u8 buffer[IDC_RX_BUFFER_SIZE];
	unsigned long pending;
};

static struct wmt_idc idc;
static struct sock *bgw_nl_sk;
static int bgw_daemon_pid;
static bool coex_ready;

#define COEX_ERR(fmt, ...)	pr_err("[WMT-COEX][E]%s: " fmt, __func__, ##__VA_ARGS__)
#define COEX_WRN(fmt, ...)	pr_warn("[WMT-COEX][W]%s: " fmt, __func__, ##__VA_ARGS__)
#define COEX_INF(fmt, ...)	pr_info("[WMT-COEX][I]%s: " fmt, __func__, ##__VA_ARGS__)
#define COEX_DBG(fmt, ...)	pr_debug("[WMT-COEX][D]%s: " fmt, __func__, ##__VA_ARGS__)

/*
 * BGW desense netlink daemon (bgw_desense.c)
 */

static int bgw_nl_send_cmd_locked(int command)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int ret;

	if (!bgw_nl_sk) {
		COEX_DBG("no netlink socket\n");
		return -ENODEV;
	}
	if (!bgw_daemon_pid) {
		COEX_DBG("BGW daemon not bound\n");
		return -ENOTCONN;
	}

	skb = alloc_skb(NLMSG_SPACE(sizeof(u8)), GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	nlh = nlmsg_put(skb, 0, 0, 0, sizeof(u8), 0);
	if (!nlh) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	*(u8 *)NLMSG_DATA(nlh) = (u8)command;
	NETLINK_CB(skb).portid = 0;

	/* Downstream checked "res == 0" as an error, but netlink_unicast()
	 * returns the number of bytes delivered; only a negative value is a
	 * failure.
	 */
	ret = netlink_unicast(bgw_nl_sk, skb, bgw_daemon_pid, MSG_DONTWAIT);
	if (ret < 0) {
		COEX_WRN("unicast command %d failed (%d)\n", command, ret);
		return ret;
	}

	COEX_DBG("command %d sent\n", command);
	return 0;
}

void wmt_bgw_send_cmd(int command)
{
	if (!coex_ready)
		return;

	bgw_nl_send_cmd_locked(command);
}

static void bgw_nl_input(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;
	const char *payload;
	u32 len;

	if (!bgw_nl_sk || !pskb_may_pull(skb, NLMSG_HDRLEN + 3)) {
		kfree_skb(skb);
		return;
	}

	nlh = nlmsg_hdr(skb);
	len = nlh->nlmsg_len - NLMSG_HDRLEN;
	if (len < 3 || !pskb_may_pull(skb, NLMSG_HDRLEN + 3)) {
		kfree_skb(skb);
		return;
	}

	payload = NLMSG_DATA(nlh);
	if (memcmp(payload, "BGW", 3)) {
		COEX_DBG("ignoring non-BGW netlink message\n");
		kfree_skb(skb);
		return;
	}

	bgw_daemon_pid = nlh->nlmsg_pid;
	COEX_INF("BGW daemon bound, pid %d\n", bgw_daemon_pid);
	kfree_skb(skb);

	bgw_nl_send_cmd_locked(WMT_BGW_DS_CMD_ACK);
}

int wmt_bgw_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = bgw_nl_input,
	};

	bgw_daemon_pid = 0;
	bgw_nl_sk = netlink_kernel_create(&init_net, WMT_BGW_NETLINK_PROTO,
					  &cfg);
	if (!bgw_nl_sk) {
		COEX_ERR("failed to create the BGW netlink socket\n");
		return -ENOMEM;
	}

	COEX_INF("BGW desense netlink ready (protocol %d)\n",
		 WMT_BGW_NETLINK_PROTO);
	return 0;
}

void wmt_bgw_deinit(void)
{
	if (bgw_nl_sk) {
		netlink_kernel_release(bgw_nl_sk);
		bgw_nl_sk = NULL;
	}
	bgw_daemon_pid = 0;
}

/*
 * Coexistence debug commands (WMTDRV_CMD_COEXDBG_00..15)
 */

int mtk_wcn_wmt_coex_dbg(u32 cmd, struct wmt_coex_buf *out)
{
	u32 len = 0;
	int ret;

	if (!out)
		return -EINVAL;
	if (cmd >= WMTDRV_CMD_COEXDBG_MAX)
		return -EINVAL;

	out->avail_size = 0;

	if (!g_wmt.ready || !mtk_wcn_stp_is_ready())
		return -ENODEV;

	ret = wmt_core_cmd_test(WMT_TEST_CMD_COEXDBG, (u8)cmd,
				out->buffer, sizeof(out->buffer), &len);
	if (ret)
		return ret;

	out->avail_size = len;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_coex_dbg);

int mtk_wcn_wmt_bgw_desense_ctrl(bool enable)
{
	if (!coex_ready)
		return -ENODEV;
	if (!g_wmt.ready)
		return -ENODEV;

	COEX_INF("BGW desense %s\n", enable ? "enable" : "disable");
	return bgw_nl_send_cmd_locked(enable ? WMT_BGW_DS_CMD_ON :
					       WMT_BGW_DS_CMD_OFF);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_bgw_desense_ctrl);

int mtk_wcn_wmt_bgw_ds_send(const u8 *payload, u32 len)
{
	if (!payload || len != WMT_BGW_DS_PAYLOAD_SIZE)
		return -EINVAL;
	if (!g_wmt.ready || !mtk_wcn_stp_is_ready())
		return -ENODEV;

	return wmt_core_bgw_ds(payload, len);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_bgw_ds_send);

/*
 * LTE IDC
 */

static int wmt_idc_opcode_to_msg(u8 opcode, enum mtk_wcn_wmt_idc_msg *msg)
{
	switch (opcode) {
	case 0x00:
		*msg = MTK_WCN_WMT_IDC_MSG_BTWF_DEF_PARA;
		return 0;
	case 0x01:
		*msg = MTK_WCN_WMT_IDC_MSG_BTWF_CHAN_RAN;
		return 0;
	case 0x03:
		*msg = MTK_WCN_WMT_IDC_MSG_LTE_FREQ_IDX_TABLE;
		return 0;
	case 0x04:
		*msg = MTK_WCN_WMT_IDC_MSG_BTWF_PROFILE_IND;
		return 0;
	case 0x05:
		*msg = MTK_WCN_WMT_IDC_MSG_UART_PIN_SEL;
		return 0;
	default:
		return -EINVAL;
	}
}

/* wmt_idc.c:118-200: walk the concatenated frames of one COEX packet. */
static void wmt_idc_parse_frames(u8 *data, u32 len)
{
	u32 handled = 0;

	while (handled + WMT_IDC_HEADER_LEN <= len) {
		u8 *frame = data + handled;
		u16 payload_len;
		u8 opcode;
		enum mtk_wcn_wmt_idc_msg msg;

		/* frame[0..1] direction/opcode, frame[2..3] length, frame[4]
		 * flag/opcode byte, then payload_len - 1 payload bytes.
		 */
		memcpy(&payload_len, frame + 2, sizeof(payload_len));
		if (!payload_len) {
			COEX_WRN("zero length IDC frame, resync\n");
			break;
		}
		payload_len -= 1;

		if (handled + WMT_IDC_HEADER_LEN + payload_len > len) {
			COEX_ERR("truncated IDC frame (%u/%u)\n",
				 handled + WMT_IDC_HEADER_LEN + payload_len,
				 len);
			break;
		}

		opcode = frame[4];
		if (opcode == WMT_IDC_RX_OPCODE_DEBUG_MONITOR) {
			COEX_DBG("firmware monitor frame, %u bytes\n",
				 payload_len);
		} else if (!wmt_idc_opcode_to_msg(opcode, &msg)) {
			if (idc.ops && idc.ops->send_to_modem) {
				int ret = idc.ops->send_to_modem(
					msg, frame + WMT_IDC_HEADER_LEN,
					payload_len);

				if (ret)
					COEX_WRN("modem delivery of %u failed (%d)\n",
						 msg, ret);
			} else {
				COEX_WRN("no modem bridge, dropping msg %u\n",
					 msg);
			}
		} else {
			COEX_WRN("unknown firmware IDC opcode 0x%02x\n", opcode);
		}

		handled += WMT_IDC_HEADER_LEN + payload_len;
	}
}

static void wmt_idc_work_fn(struct work_struct *work)
{
	int read_len;

	mutex_lock(&idc.lock);
	clear_bit(0, &idc.pending);

	read_len = mtk_wcn_stp_receive_data(idc.buffer, sizeof(idc.buffer),
					    COEX_TASK_INDX);
	if (read_len < 0) {
		mutex_unlock(&idc.lock);
		COEX_ERR("COEX queue read failed (%d)\n", read_len);
		return;
	}

	if (read_len > 0) {
		COEX_DBG("%d bytes from the firmware\n", read_len);
		wmt_idc_parse_frames(idc.buffer, read_len);
	}
	mutex_unlock(&idc.lock);
}

/* STP channel callback (registered only while a modem bridge exists). */
void wmt_idc_channel_data(void)
{
	if (!coex_ready || !idc.ops)
		return;

	if (test_and_set_bit(0, &idc.pending))
		return;

	schedule_work(&idc.work);
}

bool wmt_idc_channel_enabled(void)
{
	return coex_ready && idc.ops != NULL;
}

int mtk_wcn_wmt_idc_register(const struct mtk_wcn_wmt_idc_ops *ops)
{
	int ret;

	if (!ops || !ops->send_to_modem)
		return -EINVAL;
	if (!coex_ready)
		return -ENODEV;

	mutex_lock(&idc.lock);
	if (idc.ops) {
		mutex_unlock(&idc.lock);
		return -EBUSY;
	}
	idc.ops = ops;
	mutex_unlock(&idc.lock);

	/* Activate the COEX channel only now: without a consumer the STP
	 * parser keeps dropping LTE coex frames as before.
	 */
	ret = mtk_wcn_stp_register_event_cb(COEX_TASK_INDX,
					    wmt_idc_channel_data);
	if (ret) {
		mutex_lock(&idc.lock);
		idc.ops = NULL;
		mutex_unlock(&idc.lock);
		return ret;
	}

	COEX_INF("modem bridge registered\n");
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_idc_register);

void mtk_wcn_wmt_idc_unregister(void)
{
	if (!coex_ready)
		return;

	mutex_lock(&idc.lock);
	if (!idc.ops) {
		mutex_unlock(&idc.lock);
		return;
	}
	idc.ops = NULL;
	mutex_unlock(&idc.lock);

	mtk_wcn_stp_register_event_cb(COEX_TASK_INDX, NULL);
	flush_work(&idc.work);
	COEX_INF("modem bridge unregistered\n");
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_idc_unregister);

bool mtk_wcn_wmt_idc_is_registered(void)
{
	return coex_ready && idc.ops != NULL;
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_idc_is_registered);

int mtk_wcn_wmt_idc_send(u8 opcode, const u8 *data, u32 len)
{
	if (!data || !len)
		return -EINVAL;
	if (!coex_ready || !idc.ops)
		return -ENODEV;
	if (!g_wmt.ready || !mtk_wcn_stp_is_ready())
		return -ENODEV;

	switch (opcode) {
	case MTK_WCN_WMT_IDC_TX_LTE_PARA:
	case MTK_WCN_WMT_IDC_TX_LTE_FREQ:
	case MTK_WCN_WMT_IDC_TX_WIFI_MAX_POWER:
	case MTK_WCN_WMT_IDC_TX_DEBUG_MONITOR:
	case MTK_WCN_WMT_IDC_TX_SPLIT_FILTER:
	case MTK_WCN_WMT_IDC_TX_LTE_CONNECTION_STATUS:
	case MTK_WCN_WMT_IDC_TX_LTE_HW_IF_INDICATION:
	case MTK_WCN_WMT_IDC_TX_LTE_INDICATION:
		break;
	default:
		return -EINVAL;
	}

	return wmt_core_idc_tx(opcode, data, len);
}
EXPORT_SYMBOL_GPL(mtk_wcn_wmt_idc_send);

int wmt_coex_init(void)
{
	int ret;

	if (coex_ready)
		return 0;

	mutex_init(&idc.lock);
	INIT_WORK(&idc.work, wmt_idc_work_fn);
	idc.ops = NULL;
	idc.pending = 0;

	ret = wmt_bgw_init();
	if (ret)
		COEX_WRN("BGW desense daemon socket unavailable (%d)\n", ret);

	coex_ready = true;
	return 0;
}

void wmt_coex_deinit(void)
{
	if (!coex_ready)
		return;

	if (idc.ops) {
		/* The bridge must unregister first; force it here so that the
		 * module can always be unloaded.
		 */
		COEX_WRN("modem bridge still registered, forcing unregister\n");
		mtk_wcn_wmt_idc_unregister();
	}

	cancel_work_sync(&idc.work);
	wmt_bgw_deinit();
	mutex_destroy(&idc.lock);
	coex_ready = false;
}
