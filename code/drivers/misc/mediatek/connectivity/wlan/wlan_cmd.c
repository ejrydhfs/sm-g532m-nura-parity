// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 firmware command/event channel.
 *
 * The gen2 firmware is configured with Set commands written to HIF TX port 1
 * (traffic class TC4) and answers them with EVENT_ID_CMD_RESULT events on the
 * HIF RX side.  This file implements that transport and the three commands
 * needed to make the firmware aware of a station association:
 *
 *   CMD_ID_BSS_ACTIVATE_CTRL   network activation, nicActivateNetwork()
 *   CMD_ID_UPDATE_STA_RECORD   AP peer record, cnmStaSendUpdateCmd()
 *   CMD_ID_SET_BSS_INFO        BSS state, nicUpdateBss()
 *   CMD_ID_REMOVE_STA_RECORD   peer removal, cnmStaSendRemoveCmd()
 *
 * Ported from Samsung/MTK downstream:
 *   include/nic/cmd_buf.h      (CMD_INFO_T, CMD_HDR_SIZE)
 *   include/nic_cmd_event.h    (WIFI_CMD_T / WIFI_EVENT_T, CMD_* structs)
 *   nic/nic_tx.c:1597-1761     (nicTxCmd: command PDU framing, port 1)
 *   common/wlan_lib.c:2037-2098 (wlanSendCommand: TC4, one credit)
 *   nic/nic_rx.c:1789-1830     (EVENT_ID_CMD_RESULT, matched by sequence)
 *   nic/nic.c:1966-2103        (nicUpdateBss)
 *   nic/nic.c:1902-1927        (nicActivateNetwork)
 *   mgmt/cnm_mem.c:1129-1238   (cnmStaSendUpdateCmd / cnmStaSendRemoveCmd)
 *   mgmt/rate.c:80-254         (rate IE to rate-set conversion)
 *   mgmt/rlm.c:1713-1745       (rlmFillSyncCmdParam)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <net/cfg80211.h>

#include "wlan_cmd.h"
#include "wlan_mac.h"

/*
 * Values copied from the downstream enums:
 *
 *   ENUM_PARAM_MEDIA_STATE_T       wlan_oid.h:394-399   CONNECTED = 0
 *   ENUM_PARAM_AUTH_MODE_T         wlan_oid.h:302-315   AUTH_MODE_OPEN = 0 or 1
 *   ENUM_WEP_STATUS_T              wlan_oid.h:321-335   ENCRYPTION_DISABLED = 1
 *   ENUM_STA_TYPE_T                wlan_def.h:769-783   LEGACY_AP = BIT(0)|BIT(6)
 *   ENUM_PHY_TYPE_INDEX_T          wlan_def.h:547-555   HR_DSSS = 0, ERP = 1
 *   ENUM_OP_MODE_T                 wlan_def.h:563-571   INFRASTRUCTURE = 0
 *   ENUM_BAND_T                    wlan_def.h:582-587   BAND_2G4 = 1
 *   ENUM_CHNL_EXT_T                wlan_def.h:574-579   CHNL_EXT_SCN = 0
 *   ENUM_HIDDEN_SSID_TYPE_T        p2p_fsm.h:500-505    NONE = 0
 *   STA_STATE_1 / STA_STATE_3      mac.h:261-263         0 / 2
 *   PHY_TYPE_BIT_* / RATE_SET_*    wlan_def.h:293-388
 *
 * The firmware AUTH_MODE_OPEN value is 0: nicUpdateBss() stores
 * eAuthMode - 1 when CFG_SUPPORT_802_11R is built in (nic.c:2049-2058), and
 * AUTH_MODE_OPEN is followed by no other value without it (wlan_oid.h:302-315);
 * both cases write 0 for an open network.
 */
#define MTK_WLAN_MEDIA_STATE_CONNECTED		0
#define MTK_WLAN_MEDIA_STATE_DISCONNECTED	1
#define MTK_WLAN_AUTH_MODE_OPEN_FW		0
#define MTK_WLAN_ENCRYPTION_DISABLED		1
#define MTK_WLAN_STA_TYPE_LEGACY_AP		(BIT(0) | BIT(6))
#define MTK_WLAN_OP_MODE_INFRASTRUCTURE		0
#define MTK_WLAN_BAND_2G4			1
#define MTK_WLAN_CHNL_EXT_SCN			0
#define MTK_WLAN_HIDDEN_SSID_NONE		0
#define MTK_WLAN_PHY_TYPE_HR_DSSS_INDEX		0
#define MTK_WLAN_PHY_TYPE_ERP_INDEX		1
#define MTK_WLAN_PHY_TYPE_BIT_HR_DSSS		BIT(0)
#define MTK_WLAN_PHY_TYPE_BIT_ERP		BIT(1)
#define MTK_WLAN_PHY_TYPE_SET_802_11BG \
	(MTK_WLAN_PHY_TYPE_BIT_HR_DSSS | MTK_WLAN_PHY_TYPE_BIT_ERP)

/* RATE_SET_* bits, wlan_def.h:322-360. */
#define MTK_WLAN_RATE_SET_HR_DSSS \
	(BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define MTK_WLAN_RATE_SET_OFDM \
	(BIT(6) | BIT(7) | BIT(8) | BIT(9) | BIT(10) | BIT(11) | \
	 BIT(12) | BIT(13))
#define MTK_WLAN_RATE_SET_ALL_ABG \
	(MTK_WLAN_RATE_SET_HR_DSSS | MTK_WLAN_RATE_SET_OFDM)

/* ERP IE bits, mac.h:848-849. */
#define MTK_WLAN_ERP_USE_PROTECTION		BIT(1)
#define MTK_WLAN_ERP_BARKER_PREAMBLE		BIT(2)

/* ucCheckId of CMD_SET_BSS_RLM_PARAM_T, rlm.c:1741. */
#define MTK_WLAN_RLM_CHECK_ID			0x72

/*
 * Supported-rate codes in 500 kb/s units, in RATE_*_INDEX order
 * (aucDataRate[], rate.c:80-96; values from mac.h:234-247).
 */
static const u8 mtk_wlan_data_rates[] = {
	2, 4, 11, 22, 44, 66, 12, 18, 24, 36, 48, 72, 96, 108,
};

/* RATE_BASIC_BIT / RATE_MASK, mac.h:250-251. */
#define MTK_WLAN_RATE_MASK			GENMASK(6, 0)
#define MTK_WLAN_RATE_BASIC			BIT(7)

static_assert(sizeof(struct mtk_wlan_cmd_hdr) == 8);
static_assert(sizeof(struct mtk_wlan_event_hdr) == 8);
static_assert(sizeof(struct mtk_wlan_event_cmd_result) == 4);
static_assert(sizeof(struct mtk_wlan_cmd_bss_activate) == 4);
static_assert(sizeof(struct mtk_wlan_cmd_set_bss_rlm) == 16);
static_assert(sizeof(struct mtk_wlan_cmd_set_bss_info) == 80);
static_assert(offsetof(struct mtk_wlan_cmd_set_bss_info, rlm) == 64);
static_assert(sizeof(struct mtk_wlan_cmd_update_sta) == 40);
static_assert(sizeof(struct mtk_wlan_cmd_remove_sta) == 8);

/**
 * mtk_wlan_cmd_init - initialize the command channel state
 * @mac: MAC instance
 */
void mtk_wlan_cmd_init(struct mtk_wlan_mac *mac)
{
	mutex_init(&mac->cmd.lock);
	spin_lock_init(&mac->cmd.wait_lock);
	init_completion(&mac->cmd.done);
	mac->cmd.aborted = false;
}

/* Allow commands again after the firmware came back up. */
void mtk_wlan_cmd_reset(struct mtk_wlan_mac *mac)
{
	spin_lock_bh(&mac->cmd.wait_lock);
	mac->cmd.aborted = false;
	mac->cmd.pending = false;
	spin_unlock_bh(&mac->cmd.wait_lock);
}

/**
 * mtk_wlan_cmd_cancel - release the command in flight without poisoning the channel
 * @mac: MAC instance
 *
 * Used when a connect attempt is cancelled while the firmware is still up:
 * the waiting command returns -ECANCELED, and the teardown commands that
 * follow still work.
 */
void mtk_wlan_cmd_cancel(struct mtk_wlan_mac *mac)
{
	bool wake = false;

	spin_lock_bh(&mac->cmd.wait_lock);
	if (mac->cmd.pending) {
		mac->cmd.result = -ECANCELED;
		wake = true;
	}
	spin_unlock_bh(&mac->cmd.wait_lock);

	if (wake)
		complete(&mac->cmd.done);
}

/**
 * mtk_wlan_cmd_abort - fail the command in flight after a firmware loss
 * @mac: MAC instance
 *
 * Called from the firmware-down path.  The sender is released with -ENETDOWN
 * and every later command fails immediately until mtk_wlan_cmd_reset().
 */
void mtk_wlan_cmd_abort(struct mtk_wlan_mac *mac)
{
	bool wake = false;

	spin_lock_bh(&mac->cmd.wait_lock);
	mac->cmd.aborted = true;
	if (mac->cmd.pending) {
		mac->cmd.result = -ENETDOWN;
		wake = true;
	}
	spin_unlock_bh(&mac->cmd.wait_lock);

	if (wake)
		complete(&mac->cmd.done);
}

/**
 * mtk_wlan_cmd_event - dispatch one HIF RX event buffer
 * @mac: MAC instance
 * @buf: event payload starting at WIFI_EVENT_T
 * @len: payload length
 *
 * Only command results are consumed here.  The firmware may emit further
 * unsolicited events (STA record activation, channel privilege, ...); they
 * have no consumer in this port and are ignored.  Matching is by sequence
 * number like nicGetPendingCmdInfo() (nic.c:1406-1437); the command id in the
 * payload is only checked for diagnostics.
 */
void mtk_wlan_cmd_event(struct mtk_wlan_mac *mac, const u8 *buf, size_t len)
{
	const struct mtk_wlan_event_hdr *ev = (const void *)buf;
	const struct mtk_wlan_event_cmd_result *res;
	bool wake = false, mismatch = false;
	u8 pending_cid = 0;
	int result;

	if (len < MTK_WLAN_EVENT_HDR_LEN + sizeof(*res))
		return;
	if (ev->eid != MTK_WLAN_EVENT_ID_CMD_RESULT)
		return;
	if (le16_to_cpu(ev->packet_len) <
	    MTK_WLAN_EVENT_HDR_LEN + sizeof(*res))
		return;

	res = (const void *)(buf + MTK_WLAN_EVENT_HDR_LEN);
	switch (res->status) {
	case MTK_WLAN_CMD_STATUS_SUCCESS:
		result = 0;
		break;
	case MTK_WLAN_CMD_STATUS_REJECTED:
		result = -EIO;
		break;
	case MTK_WLAN_CMD_STATUS_UNKNOWN:
		result = -EOPNOTSUPP;
		break;
	default:
		result = -EIO;
		break;
	}

	spin_lock_bh(&mac->cmd.wait_lock);
	if (!mac->cmd.pending || ev->seq != mac->cmd.seq) {
		spin_unlock_bh(&mac->cmd.wait_lock);
		return;
	}
	pending_cid = mac->cmd.cid;
	mismatch = res->cmd_id != pending_cid;
	mac->cmd.result = result;
	wake = true;
	spin_unlock_bh(&mac->cmd.wait_lock);

	if (mismatch)
		dev_warn_ratelimited(mac->dev,
				     "command result cid 0x%02x for pending 0x%02x\n",
				     res->cmd_id, pending_cid);
	if (wake)
		complete(&mac->cmd.done);
}

/*
 * Send one command PDU and optionally wait for its result event.
 *
 * Framing follows nicTxCmd() (nic_tx.c:1700-1740): the buffer is
 * WIFI_CMD_T + payload, its first DW is the HIF TX header's first DW, the
 * packet type is HIF_TX_PKT_TYPE_CMD on TC4/port 1 and the length is padded
 * to a DW (TFCB_FRAME_PAD_TO_DW(), hif_tx.h:206).  One TC4 buffer is used
 * (wlanSendCommand(), wlan_lib.c:2077-2086); the TX-done interrupt returns it.
 */
static int mtk_wlan_cmd_send(struct mtk_wlan_mac *mac, u8 cid, bool set_query,
			     bool need_resp, const void *payload, size_t len)
{
	struct mtk_wlan_hif *hif = mac->hif;
	struct mtk_wlan_cmd_hdr *hdr;
	u8 buf[MTK_WLAN_CMD_BUF_MAX];
	size_t total = ALIGN(MTK_WLAN_CMD_HDR_LEN + len, 4);
	unsigned int tries;
	unsigned long timeout;
	int ret;

	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;
	if (MTK_WLAN_CMD_HDR_LEN + len > sizeof(buf))
		return -EMSGSIZE;

	mutex_lock(&mac->cmd.lock);

	for (tries = 0; tries < MTK_WLAN_CMD_TRIES; tries++) {
		if (mtk_wlan_tc_acquire(mac, MTK_WLAN_TC4))
			break;
		msleep(MTK_WLAN_CMD_POLL_MS);
	}
	if (tries == MTK_WLAN_CMD_TRIES) {
		mutex_unlock(&mac->cmd.lock);
		return -EBUSY;
	}

	memset(buf, 0, total);
	hdr = (struct mtk_wlan_cmd_hdr *)buf;

	spin_lock_bh(&mac->cmd.wait_lock);
	if (mac->cmd.aborted || !READ_ONCE(mac->fw_up)) {
		spin_unlock_bh(&mac->cmd.wait_lock);
		mtk_wlan_tc_release(mac, MTK_WLAN_TC4, 1);
		mutex_unlock(&mac->cmd.lock);
		return -ENETDOWN;
	}
	mac->cmd.seq++;
	mac->cmd.cid = cid;
	mac->cmd.result = 0;
	mac->cmd.pending = need_resp;
	reinit_completion(&mac->cmd.done);
	hdr->seq = mac->cmd.seq;
	spin_unlock_bh(&mac->cmd.wait_lock);

	hdr->tx_byte_count_user_priority =
		cpu_to_le16((u16)total & HIF_TX_HDR_TX_BYTE_COUNT_MASK);
	hdr->ether_type_offset = 0;
	hdr->resource_pkttype_csflags =
		FIELD_PREP(HIF_TX_HDR_RESOURCE_MASK, MTK_WLAN_TC4) |
		FIELD_PREP(HIF_TX_HDR_PACKET_TYPE_MASK, HIF_TX_PKT_TYPE_CMD);
	hdr->cid = cid;
	hdr->set_query = set_query ? 1 : 0;
	if (len)
		memcpy(buf + MTK_WLAN_CMD_HDR_LEN, payload, len);

	mutex_lock(&mac->tx_lock);
	ret = mtk_wlan_hif_port_write(hif, MCR_WTDR1, total, buf, total);
	mutex_unlock(&mac->tx_lock);

	if (ret) {
		mtk_wlan_tc_release(mac, MTK_WLAN_TC4, 1);
		spin_lock_bh(&mac->cmd.wait_lock);
		mac->cmd.pending = false;
		spin_unlock_bh(&mac->cmd.wait_lock);
		mutex_unlock(&mac->cmd.lock);
		return ret;
	}

	if (need_resp) {
		timeout = msecs_to_jiffies(MTK_WLAN_CMD_TIMEOUT_MS);
		if (!wait_for_completion_timeout(&mac->cmd.done, timeout)) {
			dev_warn(mac->dev, "command 0x%02x seq %u timed out\n",
				 cid, hdr->seq);
			ret = -ETIMEDOUT;
		} else {
			ret = mac->cmd.result;
		}
	}

	spin_lock_bh(&mac->cmd.wait_lock);
	mac->cmd.pending = false;
	spin_unlock_bh(&mac->cmd.wait_lock);
	mutex_unlock(&mac->cmd.lock);

	return ret;
}

/* Convert one Supported/Extended Supported Rates element to rate-set bits. */
static u16 mtk_wlan_rate_set_from_element(const struct element *ie, u16 *basic)
{
	unsigned int i, j;
	u16 set = 0;

	if (!ie)
		return 0;

	for (i = 0; i < ie->datalen; i++) {
		u8 code = ie->data[i] & MTK_WLAN_RATE_MASK;

		for (j = 0; j < ARRAY_SIZE(mtk_wlan_data_rates); j++) {
			if (code != mtk_wlan_data_rates[j])
				continue;
			set |= BIT(j);
			if (ie->data[i] & MTK_WLAN_RATE_BASIC)
				*basic |= BIT(j);
			break;
		}
	}

	return set;
}

/**
 * mtk_wlan_cmd_prepare_ap - derive the firmware AP parameters from a BSS entry
 * @mac: MAC instance
 * @bss: BSS the station is about to join, reference held by the caller
 *
 * Fills the operational/basic rate sets, PHY type set and protection
 * parameters the BSS and STA record commands carry.  Downstream derives them
 * from the beacon/probe response in rateGetRateSetFromIEs() (rate.c:178-254)
 * and the scan descriptor, and copies them into STA_RECORD_T/BSS_INFO_T in
 * bssCreateStaRecFromBssDesc() (bss.c:614-662).
 *
 * Return: 0 on success, -EINVAL when the AP advertises no usable 2.4 GHz rate.
 */
int mtk_wlan_cmd_prepare_ap(struct mtk_wlan_mac *mac, struct cfg80211_bss *bss)
{
	const struct element *ie;
	u16 op = 0, basic = 0;
	u8 phy = 0;
	bool barker = false;

	ie = ieee80211_bss_get_elem(bss, WLAN_EID_SUPP_RATES);
	op |= mtk_wlan_rate_set_from_element(ie, &basic);
	ie = ieee80211_bss_get_elem(bss, WLAN_EID_EXT_SUPP_RATES);
	op |= mtk_wlan_rate_set_from_element(ie, &basic);
	if (!op) {
		dev_dbg(mac->dev, "AP %pM advertises no supported rates\n",
			bss->bssid);
		return -EINVAL;
	}

	if (op & MTK_WLAN_RATE_SET_HR_DSSS)
		phy |= MTK_WLAN_PHY_TYPE_BIT_HR_DSSS;
	if (op & MTK_WLAN_RATE_SET_OFDM)
		phy |= MTK_WLAN_PHY_TYPE_BIT_ERP;
	phy &= MTK_WLAN_PHY_TYPE_SET_802_11BG;
	if (!phy)
		return -EINVAL;

	ie = ieee80211_bss_get_elem(bss, WLAN_EID_ERP_INFO);
	if (ie && ie->datalen >= 1) {
		mac->ap_erp_protect = !!(ie->data[0] & MTK_WLAN_ERP_USE_PROTECTION);
		barker = !!(ie->data[0] & MTK_WLAN_ERP_BARKER_PREAMBLE);
	} else {
		mac->ap_erp_protect = false;
	}

	mac->ap_rate_set = op;
	mac->ap_basic_rate_set = basic;
	mac->ap_phy_type_set = phy;
	mac->ap_nonht_phy = (phy & MTK_WLAN_PHY_TYPE_BIT_ERP) ?
			    MTK_WLAN_PHY_TYPE_ERP_INDEX :
			    MTK_WLAN_PHY_TYPE_HR_DSSS_INDEX;
	mac->ap_short_preamble =
		(bss->capability & WLAN_CAPABILITY_SHORT_PREAMBLE) && !barker;
	mac->ap_short_slot =
		!!(bss->capability & WLAN_CAPABILITY_SHORT_SLOT_TIME);

	return 0;
}

/**
 * mtk_wlan_cmd_bss_activate - activate or deactivate the AIS network
 * @mac: MAC instance
 * @active: %true to activate
 *
 * nicActivateNetwork()/nicDeactivateNetwork(), nic.c:1902-1958.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int mtk_wlan_cmd_bss_activate(struct mtk_wlan_mac *mac, bool active)
{
	struct mtk_wlan_cmd_bss_activate cmd = {
		.net_type_index = MTK_WLAN_NETWORK_TYPE_AIS,
		.active = active ? 1 : 0,
	};

	return mtk_wlan_cmd_send(mac, MTK_WLAN_CMD_ID_BSS_ACTIVATE_CTRL, true,
				 false, &cmd, sizeof(cmd));
}

/**
 * mtk_wlan_cmd_set_bss_info - sync the BSS state to the firmware
 * @mac: MAC instance
 * @connected: %true for the connected state
 *
 * nicUpdateBss(), nic.c:1966-2090, with rlmFillSyncCmdParam()
 * (rlm.c:1713-1745) for the embedded RLM parameters.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int mtk_wlan_cmd_set_bss_info(struct mtk_wlan_mac *mac, bool connected)
{
	struct mtk_wlan_cmd_set_bss_info cmd = { };
	u8 ssid_len = min_t(u8, mac->ssid_len, sizeof(cmd.ssid));

	cmd.net_type_index = MTK_WLAN_NETWORK_TYPE_AIS;
	cmd.connection_state = connected ? MTK_WLAN_MEDIA_STATE_CONNECTED :
			       MTK_WLAN_MEDIA_STATE_DISCONNECTED;
	cmd.current_op_mode = MTK_WLAN_OP_MODE_INFRASTRUCTURE;
	cmd.ssid_len = ssid_len;
	memcpy(cmd.ssid, mac->ssid, ssid_len);
	memcpy(cmd.bssid, mac->bssid, ETH_ALEN);
	cmd.is_qbss = 0;
	cmd.operational_rate_set = cpu_to_le16(mac->ap_rate_set);
	cmd.bss_basic_rate_set = cpu_to_le16(mac->ap_basic_rate_set);
	cmd.sta_rec_idx_of_ap = mac->sta_valid ? mac->sta_index :
				MTK_WLAN_STA_REC_NOT_FOUND;
	cmd.nonht_basic_phy_type = mac->ap_nonht_phy;
	cmd.auth_mode = MTK_WLAN_AUTH_MODE_OPEN_FW;
	cmd.enc_status = MTK_WLAN_ENCRYPTION_DISABLED;
	cmd.phy_type_set = mac->ap_phy_type_set;
	memcpy(cmd.own_mac, mac->mac_addr, ETH_ALEN);
	cmd.wapi_mode = 0;
	cmd.is_ap_mode = 0;
	cmd.hidden_ssid_mode = MTK_WLAN_HIDDEN_SSID_NONE;
	cmd.rlm.net_type_index = MTK_WLAN_NETWORK_TYPE_AIS;
	cmd.rlm.rf_band = MTK_WLAN_BAND_2G4;
	cmd.rlm.primary_channel = (u8)mac->channel;
	cmd.rlm.rf_sco = MTK_WLAN_CHNL_EXT_SCN;
	cmd.rlm.erp_protect_mode = mac->ap_erp_protect ? 1 : 0;
	cmd.rlm.ht_protect_mode = 0;	/* HT_PROTECT_MODE_NONE, rlm_protection.h:74 */
	cmd.rlm.gf_operation_mode = 0;	/* GF_MODE_NORMAL, rlm_protection.h:83 */
	cmd.rlm.tx_rifs_mode = 0;	/* RIFS_MODE_NORMAL, rlm_protection.h:91 */
	cmd.rlm.use_short_preamble = mac->ap_short_preamble ? 1 : 0;
	cmd.rlm.use_short_slot_time = mac->ap_short_slot ? 1 : 0;
	cmd.rlm.check_id = MTK_WLAN_RLM_CHECK_ID;

	return mtk_wlan_cmd_send(mac, MTK_WLAN_CMD_ID_SET_BSS_INFO, true, false,
				 &cmd, sizeof(cmd));
}

/**
 * mtk_wlan_cmd_update_sta - create or update the AP peer record
 * @mac: MAC instance
 * @state: %MTK_WLAN_STA_STATE_1 or %MTK_WLAN_STA_STATE_3
 * @aid: association id, only used for state 3
 * @need_resp: wait for the firmware result event
 *
 * cnmStaSendUpdateCmd(), cnm_mem.c:1129-1215, with the fields
 * cnmStaRecChangeState() synchronises (cnm_mem.c:1032-1056).  The peer is the
 * AP itself, so the record type is STA_TYPE_LEGACY_AP (ais_fsm.c:1372).
 *
 * Return: 0 on success, negative errno otherwise.
 */
int mtk_wlan_cmd_update_sta(struct mtk_wlan_mac *mac, u8 state, u16 aid,
			    bool need_resp)
{
	struct mtk_wlan_cmd_update_sta cmd = { };

	cmd.index = mac->sta_index;
	cmd.sta_type = MTK_WLAN_STA_TYPE_LEGACY_AP;
	memcpy(cmd.mac_addr, mac->bssid, ETH_ALEN);
	cmd.assoc_id = cpu_to_le16(aid);
	cmd.listen_interval = cpu_to_le16(MTK_WLAN_LISTEN_INTERVAL);
	cmd.net_type_index = MTK_WLAN_NETWORK_TYPE_AIS;
	cmd.desired_phy_type_set = mac->ap_phy_type_set;
	cmd.desired_nonht_rate_set =
		cpu_to_le16(mac->ap_rate_set & MTK_WLAN_RATE_SET_ALL_ABG);
	cmd.bss_basic_rate_set = cpu_to_le16(mac->ap_basic_rate_set);
	cmd.is_qos = 0;
	cmd.is_uapsd_supported = 0;
	cmd.sta_state = state;
	cmd.mcs_set = 0;
	cmd.sup_mcs32 = 0;
	cmd.ampdu_param = 0;
	cmd.ht_cap_info = 0;
	cmd.ht_extended_cap = 0;
	cmd.tx_beamforming_cap = 0;
	cmd.asel_cap = 0;
	cmd.rcpi = READ_ONCE(mac->last_signal);
	cmd.need_resp = need_resp ? 1 : 0;
	cmd.uapsd_ac = 0;
	cmd.uapsd_sp = 0;

	return mtk_wlan_cmd_send(mac, MTK_WLAN_CMD_ID_UPDATE_STA_RECORD, true,
				 need_resp, &cmd, sizeof(cmd));
}

/**
 * mtk_wlan_cmd_remove_sta - delete the AP peer record
 * @mac: MAC instance
 *
 * cnmStaSendRemoveCmd(), cnm_mem.c:1220-1238.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int mtk_wlan_cmd_remove_sta(struct mtk_wlan_mac *mac)
{
	struct mtk_wlan_cmd_remove_sta cmd = { };

	cmd.index = mac->sta_index;
	memcpy(cmd.mac_addr, mac->bssid, ETH_ALEN);

	return mtk_wlan_cmd_send(mac, MTK_WLAN_CMD_ID_REMOVE_STA_RECORD, true,
				 false, &cmd, sizeof(cmd));
}

/**
 * mtk_wlan_cmd_teardown - release the firmware association state
 * @mac: MAC instance
 *
 * Downstream disconnects by moving AIS to PARAM_MEDIA_STATE_DISCONNECTED and
 * calling nicUpdateBss() (ais_fsm.c:4072-4080), which is the SET_BSS_INFO
 * command below.  The driver-side STA record is then freed without a chip
 * sync (cnmStaRecFree(..., FALSE), ais_fsm.c:4975), so downstream does not
 * send CMD_ID_REMOVE_STA_RECORD for an AIS peer (that command is used by P2P
 * and TDLS, p2p_func.c:1103 and tdls.c:4331).  This port does send it: the
 * command and its payload are part of the same firmware ABI, it makes the
 * next association start from a clean record, and the result is not awaited.
 * The network is finally deactivated like nicDeactivateNetwork() does.
 */
int mtk_wlan_cmd_teardown(struct mtk_wlan_mac *mac)
{
	int ret;

	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;

	ret = mtk_wlan_cmd_set_bss_info(mac, false);
	if (mac->sta_valid) {
		if (mtk_wlan_cmd_remove_sta(mac))
			dev_dbg(mac->dev, "cannot remove STA record %u\n",
				mac->sta_index);
	}
	mtk_wlan_cmd_bss_activate(mac, false);

	mac->sta_valid = false;
	mac->aid = 0;
	return ret;
}
