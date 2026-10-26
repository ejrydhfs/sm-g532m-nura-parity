// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 802.11 MAC and cfg80211 instance.
 *
 * This file creates the wiphy and the Ethernet netdev, keeps them in sync
 * with the firmware state, owns the received-frame dispatch and implements
 * the cfg80211 operations that are backed by real hardware state: software
 * scan, open-system connect/disconnect and get_station.
 *
 * Ported from Samsung/MTK downstream:
 *   os/linux/gl_init.c      (wlanNetRegister/wlanNetUnregister, wiphy setup)
 *   os/linux/gl_cfg80211.c  (mtk_wlan_ops, scan/connect result reporting)
 *   nic/nic_rx.c            (nicRxProcessRFBs: RX packet type dispatch,
 *                            nic_rx.c:2734-2823)
 *   os/linux/gl_kal.c       (kalIndicateStatusAndComplete)
 *   os/linux/gl_rst.c       (reset start/end notifications)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/slab.h>
#include <net/cfg80211.h>

#include "wlan_cmd.h"
#include "wlan_mac.h"

/* Largest IE blob accepted for a scan request (fits a probe request). */
#define MTK_WLAN_SCAN_IE_MAX	256

/* 2.4 GHz channels 1..14, IEEE 802.11-2016 17.4.2.4. */
static struct ieee80211_channel mtk_wlan_channels_2ghz[] = {
	{ .center_freq = 2412, .hw_value = 1, .max_power = 20, },
	{ .center_freq = 2417, .hw_value = 2, .max_power = 20, },
	{ .center_freq = 2422, .hw_value = 3, .max_power = 20, },
	{ .center_freq = 2427, .hw_value = 4, .max_power = 20, },
	{ .center_freq = 2432, .hw_value = 5, .max_power = 20, },
	{ .center_freq = 2437, .hw_value = 6, .max_power = 20, },
	{ .center_freq = 2442, .hw_value = 7, .max_power = 20, },
	{ .center_freq = 2447, .hw_value = 8, .max_power = 20, },
	{ .center_freq = 2452, .hw_value = 9, .max_power = 20, },
	{ .center_freq = 2457, .hw_value = 10, .max_power = 20, },
	{ .center_freq = 2462, .hw_value = 11, .max_power = 20, },
	{ .center_freq = 2467, .hw_value = 12, .max_power = 20, },
	{ .center_freq = 2472, .hw_value = 13, .max_power = 20, },
	/* Channel 14 is Japan only, DSSS/CCK, passive scan only. */
	{ .center_freq = 2484, .hw_value = 14, .max_power = 20,
	  .flags = IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_OFDM, },
};

/*
 * Supported 2.4 GHz rates, in cfg80211 units of 100 kb/s.  hw_value is the
 * 500 kb/s rate code the firmware and the 802.11 Supported Rates element use
 * (mac.h:234-247); bitrate and rate code therefore agree by construction.
 * There is no HT capability here: this port associates without HT IEs.
 */
static struct ieee80211_rate mtk_wlan_rates_2ghz[] = {
	{ .bitrate = 10,  .hw_value = MTK_WLAN_RATE_1M, },
	{ .bitrate = 20,  .hw_value = MTK_WLAN_RATE_2M, },
	{ .bitrate = 55,  .hw_value = MTK_WLAN_RATE_5_5M, },
	{ .bitrate = 110, .hw_value = MTK_WLAN_RATE_11M, },
	{ .bitrate = 60,  .hw_value = MTK_WLAN_RATE_6M, },
	{ .bitrate = 90,  .hw_value = MTK_WLAN_RATE_9M, },
	{ .bitrate = 120, .hw_value = MTK_WLAN_RATE_12M, },
	{ .bitrate = 180, .hw_value = MTK_WLAN_RATE_18M, },
	{ .bitrate = 240, .hw_value = MTK_WLAN_RATE_24M, },
	{ .bitrate = 360, .hw_value = MTK_WLAN_RATE_36M, },
	{ .bitrate = 480, .hw_value = MTK_WLAN_RATE_48M, },
	{ .bitrate = 540, .hw_value = MTK_WLAN_RATE_54M, },
};

static struct ieee80211_supported_band mtk_wlan_band_2ghz = {
	.band		= NL80211_BAND_2GHZ,
	.channels	= mtk_wlan_channels_2ghz,
	.n_channels	= ARRAY_SIZE(mtk_wlan_channels_2ghz),
	.bitrates	= mtk_wlan_rates_2ghz,
	.n_bitrates	= ARRAY_SIZE(mtk_wlan_rates_2ghz),
};

/*
 * cfg80211 operations.  Only what the hardware can actually do is listed:
 * no key offload (so the wiphy advertises no cipher suite), no AP/IBSS/P2P,
 * no remain-on-channel, no scheduled scan, no hardware scan.
 */
static int mtk_wlan_cfg80211_scan(struct wiphy *wiphy,
				  struct cfg80211_scan_request *request)
{
	return mtk_wlan_mgmt_scan(wiphy_priv(wiphy), request);
}

static void mtk_wlan_cfg80211_abort_scan(struct wiphy *wiphy,
					 struct wireless_dev *wdev)
{
	mtk_wlan_mgmt_abort_scan(wiphy_priv(wiphy));
}

static int mtk_wlan_cfg80211_connect(struct wiphy *wiphy,
				     struct net_device *dev,
				     struct cfg80211_connect_params *sme)
{
	return mtk_wlan_mgmt_connect(wiphy_priv(wiphy), sme);
}

static int mtk_wlan_cfg80211_disconnect(struct wiphy *wiphy,
					struct net_device *dev, u16 reason_code)
{
	return mtk_wlan_mgmt_disconnect(wiphy_priv(wiphy), reason_code);
}

static int mtk_wlan_cfg80211_get_station(struct wiphy *wiphy,
					 struct net_device *dev, const u8 *mac,
					 struct station_info *sinfo)
{
	struct mtk_wlan_mac *m = wiphy_priv(wiphy);
	bool have_signal;
	u8 rcpi;

	if (!READ_ONCE(m->connected) || !ether_addr_equal(mac, m->bssid))
		return -ENOENT;

	/*
	 * Only the fields the HIF actually reports are filled.  The RX header
	 * carries RCPI for every received frame (hif_rx.h:96-113,
	 * RCPI_TO_dBm() mac.h:2533-2534); it carries no rate, so no rate
	 * information is invented here.  Packet and byte counters are MAC
	 * level frames, not the per-station counters downstream derives from
	 * wlanoidQueryStaStatistics(), so they are not reported either.
	 */
	spin_lock_bh(&m->rx_lock);
	have_signal = m->have_signal;
	rcpi = m->last_signal;
	spin_unlock_bh(&m->rx_lock);

	if (have_signal) {
		sinfo->signal = (s8)mtk_wlan_rcpi_to_dbm(rcpi);
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	}

	sinfo->connected_time = jiffies_to_msecs(jiffies - m->assoc_ts) / 1000;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME);

	return 0;
}

static const struct cfg80211_ops mtk_wlan_cfg80211_ops = {
	.scan		= mtk_wlan_cfg80211_scan,
	.abort_scan	= mtk_wlan_cfg80211_abort_scan,
	.connect	= mtk_wlan_cfg80211_connect,
	.disconnect	= mtk_wlan_cfg80211_disconnect,
	.get_station	= mtk_wlan_cfg80211_get_station,
};

/*
 * Expected channels from the HIF RX header, HIF_RX_HDR_GET_CHNL_NUM()
 * hif_rx.h:152-155.
 */
static u8 mtk_wlan_hw_channel(u8 hw_channel)
{
	if (hw_channel > HIF_RX_HW_CHNL_NUM_MAX_4G_5G)
		return hw_channel - HIF_RX_HW_CHNL_NUM_MAX_4G_5G;
	return hw_channel;
}

/**
 * mtk_wlan_mac_rx - dispatch one frame drained from a HIF data port
 * @hif: host interface
 * @buf: complete buffer as it left the data port
 * @len: buffer length including the hardware appended DW
 *
 * Data and management frames use the layout nicRxFillRFB()/nicRxProcessRFBs()
 * work on (nic_rx.c:1044-1085 and 2734-2823): HIF_RX_HEADER_T, then the
 * payload at HIF_RX_HDR_SIZE + header offset, with the packet length covering
 * both.  Events are different: the first DW of WIFI_EVENT_T is shared with
 * HIF_RX_HEADER_T (nic_cmd_event.h:613), so the event header starts at offset
 * 0 and its body at EVENT_HDR_SIZE = 8 (nic_rx.c:1750-1775).
 */
void mtk_wlan_mac_rx(struct mtk_wlan_hif *hif, const u8 *buf, u32 len)
{
	const struct mtk_wlan_hif_rx_header *rx =
		(const struct mtk_wlan_hif_rx_header *)buf;
	struct mtk_wlan_mac *mac = hif->mac;
	u32 pkt_len, payload_off, payload_len;
	u16 type_field;
	unsigned int pkt_type;
	bool is_80211;
	u8 channel, rcpi;

	if (!mac)
		return;

	if (len < MTK_WLAN_HIF_RX_HDR_LEN + MTK_WLAN_HIF_HW_APPENDED_LEN) {
		atomic_inc(&mac->rx_dropped);
		return;
	}

	pkt_len = le16_to_cpu(rx->packet_len);
	if (pkt_len < MTK_WLAN_EVENT_HDR_LEN ||
	    ALIGN(pkt_len, 4) + MTK_WLAN_HIF_HW_APPENDED_LEN > len) {
		atomic_inc(&mac->rx_dropped);
		return;
	}

	type_field = le16_to_cpu(rx->packet_type);
	pkt_type = FIELD_GET(HIF_RX_HDR_PACKET_TYPE_MASK, type_field);

	if (pkt_type == HIF_RX_PKT_TYPE_EVENT) {
		/*
		 * Firmware command results and unsolicited events.  The
		 * command channel consumes EVENT_ID_CMD_RESULT and ignores
		 * the rest, like nicRxProcessEventPacket() (nic_rx.c:1750).
		 */
		mtk_wlan_cmd_event(mac, buf, len);
		return;
	}

	if (pkt_len < MTK_WLAN_HIF_RX_HDR_LEN) {
		atomic_inc(&mac->rx_dropped);
		return;
	}

	payload_off = MTK_WLAN_HIF_RX_HDR_LEN +
		      (rx->hdr_len_offset & HIF_RX_HDR_HEADER_OFFSET_MASK);
	if (payload_off > pkt_len) {
		atomic_inc(&mac->rx_dropped);
		return;
	}
	payload_len = pkt_len - payload_off;

	is_80211 = rx->reorder_pal_tcl & HIF_RX_HDR_80211_HEADER_FORMAT;
	channel = mtk_wlan_hw_channel(rx->hw_channel);
	rcpi = rx->rcpi;

	switch (pkt_type) {
	case HIF_RX_PKT_TYPE_DATA:
		mtk_wlan_rx_data(mac, buf + payload_off, payload_len,
				 is_80211);
		break;
	case HIF_RX_PKT_TYPE_MANAGEMENT:
		mtk_wlan_rx_mgmt(mac, buf + payload_off, payload_len, channel,
				 rcpi);
		break;
	default:
		atomic_inc(&mac->rx_dropped);
		break;
	}
}

/* Register or unregister the netdev after a firmware transition. */
static void mtk_wlan_link_work(struct work_struct *work)
{
	struct mtk_wlan_mac *mac = container_of(work, struct mtk_wlan_mac,
						link_work);

	if (READ_ONCE(mac->fw_up)) {
		if (!mac->registered)
			mtk_wlan_netdev_register(mac);
		return;
	}

	if (!mac->registered)
		return;

	/*
	 * Let the scan and connect workers finish: both report their own
	 * abort/failure through cfg80211, so the wiphy state stays consistent
	 * until the netdev goes away.
	 */
	mtk_wlan_mgmt_quiesce(mac);
	cancel_work_sync(&mac->tx_work);
	mtk_wlan_netdev_unregister(mac);
}

/**
 * mtk_wlan_mac_fw_up - the firmware finished downloading
 * @hif: host interface
 *
 * Called after mtk_wlan_fw_bringup() reported success.  The netdev is
 * published from a work item because the caller may hold WMT internal locks
 * that must not be taken underneath the RTNL.
 */
void mtk_wlan_mac_fw_up(struct mtk_wlan_hif *hif)
{
	struct mtk_wlan_mac *mac = hif->mac;

	if (!mac)
		return;

	WRITE_ONCE(mac->fw_up, true);
	WRITE_ONCE(mac->state, MTK_WLAN_STATE_IDLE);
	WRITE_ONCE(mac->connected, false);
	mac->sta_valid = false;
	mac->aid = 0;
	mtk_wlan_cmd_reset(mac);
	schedule_work(&mac->link_work);
}

/**
 * mtk_wlan_mac_fw_down - the firmware stopped or the chip is resetting
 * @hif: host interface
 *
 * Aborts a pending scan or connect, reports a lost association and drops the
 * netdev so userspace never sees an interface that claims to be up while the
 * firmware is gone.
 */
void mtk_wlan_mac_fw_down(struct mtk_wlan_hif *hif)
{
	struct mtk_wlan_mac *mac = hif->mac;

	if (!mac)
		return;

	WRITE_ONCE(mac->fw_up, false);
	mtk_wlan_mgmt_quiesce(mac);
	schedule_work(&mac->link_work);
}

/**
 * mtk_wlan_mac_attach - create the wiphy, netdev and MAC state
 * @hif: host interface
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wlan_mac_attach(struct mtk_wlan_hif *hif)
{
	struct mtk_wlan_mac *mac;
	struct wiphy *wiphy;
	u8 addr[ETH_ALEN];
	u8 credits[MTK_WLAN_TC_NUM] = MTK_WLAN_TC_CREDITS_INIT;
	int ret;

	wiphy = wiphy_new(&mtk_wlan_cfg80211_ops, sizeof(*mac));
	if (!wiphy) {
		dev_err(hif->dev, "cannot allocate wiphy\n");
		return -ENOMEM;
	}
	mac = wiphy_priv(wiphy);
	mac->hif = hif;
	mac->wiphy = wiphy;
	mac->dev = hif->dev;

	mutex_init(&mac->lock);
	mutex_init(&mac->tx_lock);
	spin_lock_init(&mac->txq_lock);
	spin_lock_init(&mac->rx_lock);
	skb_queue_head_init(&mac->txq);
	init_completion(&mac->mgmt_done);
	mtk_wlan_cmd_init(mac);
	memcpy(mac->tc_max, credits, sizeof(credits));
	memcpy(mac->tc_credits, credits, sizeof(credits));

	/*
	 * The single AIS peer uses STA record index 0: cnmStaRecAlloc()
	 * (cnm_mem.c:808-830) returns the first free slot and the transmit
	 * path rejects only STA_REC_INDEX_BMCAST/NOT_FOUND (que_mgt.h:766).
	 */
	mac->sta_index = 0;

	mac->tx_buf = devm_kmalloc(hif->dev, MTK_WLAN_TX_BUF_SIZE, GFP_KERNEL);
	if (!mac->tx_buf) {
		ret = -ENOMEM;
		goto err_wiphy;
	}

	INIT_WORK(&mac->link_work, mtk_wlan_link_work);
	INIT_WORK(&mac->tx_work, mtk_wlan_tx_work);
	INIT_WORK(&mac->scan_work, mtk_wlan_mgmt_scan_work);
	INIT_WORK(&mac->connect_work, mtk_wlan_mgmt_connect_work);

	/*
	 * The permanent address lives in the "WIFI" NVRAM section the
	 * downstream driver reads in glLoadNvram() (gl_init.c:1119-1150,
	 * WIFI_CFG_PARAM_STRUCT.aucMacAddress) and then programs through
	 * wlanoidSetCurrentAddr.  Neither the NVRAM file nor the firmware
	 * command queue exists in this port, so the address comes from the
	 * device tree ("local-mac-address") when present; otherwise the
	 * netdev gets a random locally administered address in
	 * mtk_wlan_netdev_create().
	 */
	if (of_get_mac_address(hif->dev->of_node, addr))
		eth_zero_addr(mac->mac_addr);
	else
		ether_addr_copy(mac->mac_addr, addr);

	set_wiphy_dev(wiphy, hif->dev);
	/*
	 * One station interface on 2.4 GHz only.  No 5 GHz band pointer, no
	 * AP/IBSS/P2P mode, no remain-on-channel and no scheduled scan: the
	 * firmware command interface needed for all of those is not part of
	 * this port.  Hardware scan does not exist either - only the .scan
	 * callback below is implemented (gl_init.c:1583-1612 sets the same
	 * station interface, but also ADHOC and the 5 GHz band, both of which
	 * this port does not support).
	 */
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->bands[NL80211_BAND_2GHZ] = &mtk_wlan_band_2ghz;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = MTK_WLAN_MAX_SCAN_SSIDS;
	wiphy->max_scan_ie_len = MTK_WLAN_SCAN_IE_MAX;
	/*
	 * No cipher suites at all: there is no key offload, so cfg80211 has
	 * nothing to install and only open networks can be joined.
	 */
	wiphy->cipher_suites = NULL;
	wiphy->n_cipher_suites = 0;

	ret = wiphy_register(wiphy);
	if (ret) {
		dev_err(hif->dev, "cannot register wiphy: %d\n", ret);
		goto err_wiphy;
	}

	ret = mtk_wlan_netdev_create(mac);
	if (ret)
		goto err_wiphy_unregister;

	hif->mac = mac;

	return 0;

err_wiphy_unregister:
	wiphy_unregister(wiphy);
err_wiphy:
	wiphy_free(wiphy);
	return ret;
}

/**
 * mtk_wlan_mac_detach - tear the MAC layer down
 * @hif: host interface
 */
void mtk_wlan_mac_detach(struct mtk_wlan_hif *hif)
{
	struct mtk_wlan_mac *mac = hif->mac;

	if (!mac)
		return;

	WRITE_ONCE(mac->fw_up, false);

	/* Let pending scan/connect work report their abort to cfg80211. */
	mtk_wlan_mgmt_quiesce(mac);
	cancel_work_sync(&mac->link_work);
	cancel_work_sync(&mac->tx_work);

	mtk_wlan_netdev_unregister(mac);
	mtk_wlan_netdev_destroy(mac);
	skb_queue_purge(&mac->txq);

	wiphy_unregister(mac->wiphy);
	wiphy_free(mac->wiphy);	/* frees @mac, which is wiphy_priv() */
	hif->mac = NULL;
}
