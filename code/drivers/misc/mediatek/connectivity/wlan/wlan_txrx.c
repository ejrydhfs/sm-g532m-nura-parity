// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 HIF TX framing, TX queueing and data conversion.
 *
 * Data frames travel in 802.3 format and the firmware builds their 802.11
 * header (nic_tx.c:1400-1480, 2000-2018).  Management frames are built by
 * wlan_mgmt.c and travel as complete 802.11 frames on TC4.  Received 802.11
 * format data frames (HIF RX header format bit) are converted back to 802.3
 * before they reach the stack.
 *
 * Ported from Samsung/MTK downstream:
 *   nic/nic_tx.c              (nicTxMsduQueue header fill, TC resource
 *                              accounting, TX port selection)
 *   include/nic/hif_tx.h      (HIF_TX_HEADER_T and its flag bit fields)
 *   include/nic/hif_rx.h      (HIF_RX_HEADER_T)
 *   nic/nic_rx.c              (port read length with the appended DW)
 *   nic/que_mgt.c             (TC per access category, no-STA fallback)
 *   os/linux/gl_kal.c         (kalRxIndicatePkts: 802.3 frames to the stack)
 *   include/nic/mac.h         (margins, RCPI)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include "wlan_mac.h"

/*
 * Firmware TX buffer credits.  WTSR0 holds one released-buffer count per
 * byte for TC0..TC3, WTSR1 does the same for TC4..TC5; the driver adds the
 * released counts to its own per-TC counters (nicTxReleaseResource(),
 * nic_tx.c:698-738, and HAL_READ_TX_RELEASED_COUNT(), hal.h:511-521).
 */
void mtk_wlan_tc_release(struct mtk_wlan_mac *mac, unsigned int tc,
			 unsigned int count)
{
	spin_lock_bh(&mac->txq_lock);
	if (mac->tc_credits[tc] + count > mac->tc_max[tc])
		mac->tc_credits[tc] = mac->tc_max[tc];
	else
		mac->tc_credits[tc] += count;
	spin_unlock_bh(&mac->txq_lock);
}

/**
 * mtk_wlan_tc_acquire - reserve one firmware TX buffer of @tc
 * @mac: MAC instance
 * @tc: traffic class, %MTK_WLAN_TC0 .. %MTK_WLAN_TC5
 *
 * Return: %true when a buffer was reserved.
 */
bool mtk_wlan_tc_acquire(struct mtk_wlan_mac *mac, unsigned int tc)
{
	bool ok = false;

	spin_lock_bh(&mac->txq_lock);
	if (mac->tc_credits[tc]) {
		mac->tc_credits[tc]--;
		ok = true;
	}
	spin_unlock_bh(&mac->txq_lock);

	return ok;
}

/*
 * Reserve one buffer of @tc only when at least @min buffers are free.  The
 * management path uses a two-buffer watermark for TC4, exactly like
 * nicTxAcquireResource() does for security/management frames
 * (nic_tx.c:559-571); one buffer is then consumed.
 */
static bool mtk_wlan_tc_acquire_min(struct mtk_wlan_mac *mac, unsigned int tc,
				    u8 min)
{
	bool ok = false;

	spin_lock_bh(&mac->txq_lock);
	if (mac->tc_credits[tc] >= min) {
		mac->tc_credits[tc]--;
		ok = true;
	}
	spin_unlock_bh(&mac->txq_lock);

	return ok;
}

/**
 * mtk_wlan_mac_tx_done - account TX buffers released by the firmware
 * @hif: host interface
 * @wtsr0: MCR_WTSR0 contents
 * @wtsr1: MCR_WTSR1 contents
 *
 * Called from the HIF interrupt bottom half while the firmware is up.  Each
 * byte of the two status registers is the number of TX buffers the firmware
 * released for the corresponding traffic class (nic_tx.c:712-716).
 */
void mtk_wlan_mac_tx_done(struct mtk_wlan_hif *hif, u32 wtsr0, u32 wtsr1)
{
	struct mtk_wlan_mac *mac = hif->mac;
	unsigned int tc;

	if (!mac || !READ_ONCE(mac->fw_up))
		return;

	for (tc = 0; tc < 4; tc++)
		mtk_wlan_tc_release(mac, tc, (wtsr0 >> (tc * 8)) & 0xff);
	mtk_wlan_tc_release(mac, MTK_WLAN_TC4, wtsr1 & 0xff);
	mtk_wlan_tc_release(mac, MTK_WLAN_TC5, (wtsr1 >> 8) & 0xff);

	mtk_wlan_netdev_wake_tx(mac);
	if (READ_ONCE(mac->connected))
		schedule_work(&mac->tx_work);
}

/* EtherType is two octets (802.11-2016 9.2.2); the offset is measured from
 * the HIF TX header like the downstream ETHER_TYPE_LEN use at nic_tx.c:1414.
 */
#define MTK_WLAN_ETH_TYPE_LEN		2

/*
 * Everything the HIF TX header needs for one frame.  Management frames carry
 * a complete 802.11 header (nic_tx.c:1660-1700); data frames carry an 802.3
 * frame and the firmware builds the 802.11 header itself (nic_tx.c:1400-1460,
 * fgIs802_11 = FALSE, ucMacHeaderLength = ETH_HLEN from nicTxMsduInfoList
 * nic_tx.c:2000-2018).
 */
struct mtk_wlan_tx_params {
	unsigned int tc;
	unsigned int pkt_type;
	u8 wlan_hdr_len;
	u8 ether_off;
	u8 sta_idx;
	u8 user_priority;
	bool is_80211;
	bool ack;
	bool basic_rate;
	size_t total;
};

/*
 * Fill the 16-byte HIF TX header in @mac->tx_buf.  The caller holds
 * &mac->tx_lock and has already placed the frame right behind the header.
 *
 * The flag set follows nic_tx.c:1400-1480: @is_80211 selects
 * HIF_TX_HDR_FLAG_802_11_FORMAT, @ack and @basic_rate are HIF_TX_HDR_NEED_ACK
 * and HIF_TX_HDR_BASIC_RATE (management sets both, auth.c:442 and
 * assoc.c:934; data sets neither, nic_tx.c:1461-1474 and 1940).  @ether_off
 * is the EtherType offset in 16-bit words counted from the HIF header.
 */
static void mtk_wlan_tx_hdr_fill(struct mtk_wlan_mac *mac,
				 const struct mtk_wlan_tx_params *p)
{
	struct mtk_wlan_hif_tx_header *hdr =
		(struct mtk_wlan_hif_tx_header *)mac->tx_buf;

	memset(hdr, 0, MTK_WLAN_HIF_TX_HDR_LEN);
	hdr->tx_byte_count_user_priority =
		cpu_to_le16(((u16)p->total & HIF_TX_HDR_TX_BYTE_COUNT_MASK) |
			    ((u16)p->user_priority <<
			     HIF_TX_HDR_USER_PRIORITY_OFFSET));
	hdr->ether_type_offset = p->ether_off & HIF_TX_HDR_ETHER_TYPE_OFFSET_MASK;
	hdr->resource_pkttype_csflags =
		FIELD_PREP(HIF_TX_HDR_RESOURCE_MASK, p->tc) |
		FIELD_PREP(HIF_TX_HDR_PACKET_TYPE_MASK, p->pkt_type);
	hdr->wlan_header_length = p->wlan_hdr_len &
				  HIF_TX_HDR_WLAN_HEADER_LEN_MASK;
	hdr->pktfmtid_flags =
		FIELD_PREP(HIF_TX_HDR_NETWORK_TYPE_MASK,
			   MTK_WLAN_NETWORK_TYPE_AIS) |
		(p->is_80211 ? HIF_TX_HDR_FLAG_802_11_FORMAT : 0);
	hdr->sta_rec_idx = p->sta_idx;
	hdr->forwarding_type_sessionid = 0;	/* PS_FORWARDING_TYPE_NON_PS */
	hdr->ack_bip_basicrate =
		(p->ack ? HIF_TX_HDR_NEED_ACK : 0) |
		(p->basic_rate ? HIF_TX_HDR_BASIC_RATE : 0);
}

/*
 * Push the prepared buffer through the data port.  Called with &mac->tx_lock
 * held and one TC buffer already reserved.
 */
static int mtk_wlan_tx_flush(struct mtk_wlan_mac *mac, unsigned int tc,
			     size_t total, size_t frame_len)
{
	struct mtk_wlan_hif *hif = mac->hif;
	u16 port;
	int ret;

	/* TC4 is the only class that uses HIF TX port 1 (nic_tx.c:872-880). */
	port = (tc == MTK_WLAN_TC4) ? MCR_WTDR1 : MCR_WTDR0;

	ret = mtk_wlan_hif_port_write(hif, port, total, mac->tx_buf, total);
	if (ret) {
		/* Give the buffer back, the firmware never saw it. */
		mtk_wlan_tc_release(mac, tc, 1);
		atomic_inc(&mac->tx_dropped);
		if (mac->registered)
			mac->ndev->stats.tx_errors++;
		return ret;
	}

	if (mac->registered) {
		mac->ndev->stats.tx_packets++;
		mac->ndev->stats.tx_bytes += frame_len;
	}
	return 0;
}

/**
 * mtk_wlan_mgmt_tx - transmit one 802.11 management frame
 * @mac: MAC instance
 * @frame: complete 802.11 management frame including the MAC header
 * @len: frame length
 *
 * Management frames travel in 802.11 format on TC4 (auth.c:430-442), with
 * the WLAN header length and the EtherType offset filled in exactly like
 * nicTxMsduQueue() does for TX_PACKET_MGMT (nic_tx.c:1409-1420).
 *
 * Return: 0, -EBUSY when no TX buffer is free, or another negative errno.
 */
int mtk_wlan_mgmt_tx(struct mtk_wlan_mac *mac, const u8 *frame, size_t len)
{
	struct mtk_wlan_tx_params p = {
		.tc = MTK_WLAN_TC4,
		.pkt_type = HIF_TX_PKT_TYPE_MANAGEMENT,
		.wlan_hdr_len = MTK_WLAN_MAC_HDR_LEN,
		.ether_off = (MTK_WLAN_HIF_TX_HDR_LEN +
			      MTK_WLAN_MAC_HDR_LEN) >> 1,
		.sta_idx = mac->sta_valid ? mac->sta_index :
			   MTK_WLAN_STA_REC_NOT_FOUND,
		.is_80211 = true,
		.ack = true,
		.basic_rate = true,
	};
	size_t total;
	unsigned int tries;
	int ret;

	if (!READ_ONCE(mac->fw_up))
		return -ENETDOWN;
	if (len > MTK_WLAN_MGMT_FRAME_MAX)
		return -EMSGSIZE;

	total = ALIGN(MTK_WLAN_HIF_TX_HDR_LEN + len, 4);

	/*
	 * TC4 only has four buffers in this configuration; the firmware
	 * returns them with the TX-done interrupt, so poll briefly like
	 * nicTxPollingResource() (nic_tx.c:633-697) instead of failing the
	 * management exchange on a transient shortage.  Two buffers must be
	 * free before one is taken, the same watermark nicTxAcquireResource()
	 * applies to management/802.1x frames (nic_tx.c:559-571).
	 */
	for (tries = 0; tries < 100; tries++) {
		if (mtk_wlan_tc_acquire_min(mac, MTK_WLAN_TC4, 2))
			break;
		msleep(2);
	}
	if (tries == 100)
		return -EBUSY;

	mutex_lock(&mac->tx_lock);
	memset(mac->tx_buf, 0, total);
	p.total = total;
	mtk_wlan_tx_hdr_fill(mac, &p);
	memcpy(mac->tx_buf + MTK_WLAN_HIF_TX_HDR_LEN, frame, len);
	ret = mtk_wlan_tx_flush(mac, MTK_WLAN_TC4, total, len);
	mutex_unlock(&mac->tx_lock);

	return ret;
}

/**
 * mtk_wlan_data_tx - hand one Ethernet frame to the firmware data path
 * @mac: MAC instance
 * @skb: Ethernet frame
 *
 * Data frames are written in 802.3 format; the firmware builds the 802.11
 * header, which is downstream's default data path (nic_tx.c:2000-2018 sets
 * fgIs802_11 = FALSE and ucMacHeaderLength = ETH_HLEN; the EtherType offset
 * comes from the 802.3 branch at nic_tx.c:1404-1416).  The peer record index
 * from CMD_UPDATE_STA_RECORD is carried in the HIF header, so the firmware
 * can find the station instead of using STA_REC_INDEX_NOT_FOUND.
 *
 * Ownership: the frame is consumed on every return value except -EBUSY; on
 * -EBUSY no firmware TX buffer was available and the caller keeps the skb
 * (the data path has 36 TC1 buffers, nic_tx.h:298-303, and the firmware
 * returns them from the TX-done interrupt).
 *
 * Return: 0 on success, -EBUSY when no TX buffer was free, another negative
 * errno when the frame was dropped.
 */
int mtk_wlan_data_tx(struct mtk_wlan_mac *mac, struct sk_buff *skb)
{
	struct mtk_wlan_tx_params p = {
		.tc = MTK_WLAN_TC1,
		.pkt_type = HIF_TX_PKT_TYPE_DATA,
		.wlan_hdr_len = MTK_WLAN_ETH_HDR_LEN,
		.ether_off = (MTK_WLAN_ETH_HDR_LEN - MTK_WLAN_ETH_TYPE_LEN +
			      MTK_WLAN_HIF_TX_HDR_LEN) >> 1,
		.sta_idx = mac->sta_valid ? mac->sta_index :
			   MTK_WLAN_STA_REC_NOT_FOUND,
		.is_80211 = false,
		.ack = false,
		.basic_rate = false,
	};
	size_t total;
	int ret;

	if (skb->len < MTK_WLAN_ETH_HDR_LEN ||
	    skb->len > MTK_WLAN_TX_FRAME_MAX) {
		atomic_inc(&mac->tx_dropped);
		if (mac->registered)
			mac->ndev->stats.tx_dropped++;
		consume_skb(skb);
		return -EMSGSIZE;
	}

	if (!mtk_wlan_tc_acquire(mac, MTK_WLAN_TC1))
		return -EBUSY;

	total = ALIGN(MTK_WLAN_HIF_TX_HDR_LEN + skb->len, 4);
	mutex_lock(&mac->tx_lock);
	memset(mac->tx_buf, 0, total);
	memcpy(mac->tx_buf + MTK_WLAN_HIF_TX_HDR_LEN, skb->data, skb->len);
	p.total = total;
	mtk_wlan_tx_hdr_fill(mac, &p);
	ret = mtk_wlan_tx_flush(mac, MTK_WLAN_TC1, total, skb->len);
	mutex_unlock(&mac->tx_lock);

	consume_skb(skb);
	return ret;
}

/**
 * mtk_wlan_tx_work - drain the netdev TX queue into the HIF
 * @work: &struct mtk_wlan_mac.tx_work
 */
void mtk_wlan_tx_work(struct work_struct *work)
{
	struct mtk_wlan_mac *mac = container_of(work, struct mtk_wlan_mac,
						tx_work);
	struct sk_buff *skb;

	while (READ_ONCE(mac->fw_up) && READ_ONCE(mac->connected)) {
		skb = skb_dequeue(&mac->txq);
		if (!skb)
			break;

		/*
		 * The HIF write reads skb->data linearly.  Linearising here
		 * (process context) rather than in ndo_start_xmit keeps the
		 * atomic transmit path cheap.
		 */
		if (skb_is_nonlinear(skb) && skb_linearize(skb)) {
			atomic_inc(&mac->tx_dropped);
			if (mac->registered)
				mac->ndev->stats.tx_dropped++;
			dev_kfree_skb(skb);
			continue;
		}

		if (mtk_wlan_data_tx(mac, skb) == -EBUSY) {
			/* No TC1 buffer left: keep the frame for the next round. */
			skb_queue_head(&mac->txq, skb);
			if (mac->registered)
				netif_tx_stop_all_queues(mac->ndev);
			return;
		}
	}

	if (mac->registered && READ_ONCE(mac->fw_up) && mac->connected)
		netif_tx_wake_all_queues(mac->ndev);
}

/**
 * mtk_wlan_rx_80211_to_8023 - convert a received 802.11 data frame
 * @mac: MAC instance
 * @frame: 802.11 frame including the MAC header
 * @len: frame length
 *
 * The firmware normally decapsulates data frames and hands up 802.3
 * (kalRxIndicatePkts() feeds the skb straight to eth_type_trans(),
 * gl_kal.c:1583-1612), but the HIF RX header can announce 802.11 format.
 * Then the driver strips the header itself: FromDS frames reverse the ToDS
 * address mapping and carry the EtherType in an RFC 1042 LLC/SNAP header.
 *
 * Return: new skb holding the Ethernet frame, %NULL when it cannot be used.
 */
struct sk_buff *mtk_wlan_rx_80211_to_8023(struct mtk_wlan_mac *mac,
					  const u8 *frame, size_t len)
{
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)frame;
	const u8 *da, *sa, *snap;
	struct sk_buff *skb;
	size_t hdr_len, payload_len;
	__le16 fc;

	if (len < MTK_WLAN_MAC_HDR_LEN)
		return NULL;

	fc = hdr->frame_control;
	if (!ieee80211_is_data(fc))
		return NULL;

	/* No key offload, so encrypted frames are unusable here. */
	if (ieee80211_has_protected(fc)) {
		atomic_inc(&mac->rx_dropped);
		return NULL;
	}

	if (!ieee80211_has_fromds(fc)) {
		/*
		 * An infrastructure STA receives only FromDS data.  ToDS and
		 * IBSS frames belong to paths this driver does not implement.
		 */
		atomic_inc(&mac->rx_dropped);
		return NULL;
	}

	hdr_len = MTK_WLAN_MAC_HDR_LEN;
	if (ieee80211_is_data_qos(fc)) {
		__le16 qos;

		if (len < hdr_len + MTK_WLAN_QOS_CTRL_LEN)
			return NULL;
		qos = get_unaligned_le16(frame + hdr_len);
		/* An A-MSDU payload is not an LLC/SNAP frame; drop it. */
		if (le16_to_cpu(qos) & IEEE80211_QOS_CTL_A_MSDU_PRESENT) {
			atomic_inc(&mac->rx_dropped);
			return NULL;
		}
		hdr_len += MTK_WLAN_QOS_CTRL_LEN;
	}
	if (ieee80211_has_a4(fc))
		hdr_len += 2 * ETH_ALEN;
	if (len < hdr_len + MTK_WLAN_LLC_LEN)
		return NULL;

	snap = frame + hdr_len;
	if (snap[0] != 0xaa || snap[1] != 0xaa || snap[2] != 0x03)
		return NULL;

	da = (const u8 *)ieee80211_get_DA((struct ieee80211_hdr *)frame);
	sa = (const u8 *)ieee80211_get_SA((struct ieee80211_hdr *)frame);
	payload_len = len - hdr_len - MTK_WLAN_LLC_LEN;

	skb = dev_alloc_skb(MTK_WLAN_ETH_HDR_LEN + payload_len + NET_IP_ALIGN);
	if (!skb) {
		atomic_inc(&mac->rx_dropped);
		return NULL;
	}
	skb_reserve(skb, NET_IP_ALIGN);

	skb_put(skb, MTK_WLAN_ETH_HDR_LEN);
	ether_addr_copy(skb->data, da);
	ether_addr_copy(skb->data + ETH_ALEN, sa);
	memcpy(skb->data + 2 * ETH_ALEN, snap + 6, 2);
	skb_put_data(skb, frame + hdr_len + MTK_WLAN_LLC_LEN, payload_len);

	return skb;
}

/**
 * mtk_wlan_rx_data - deliver one firmware data frame to the netdev
 * @mac: MAC instance
 * @frame: payload after the HIF RX header
 * @len: payload length
 * @is_80211: HIF RX header announced 802.11 format
 */
void mtk_wlan_rx_data(struct mtk_wlan_mac *mac, const u8 *frame, size_t len,
		      bool is_80211)
{
	struct sk_buff *skb;

	if (is_80211) {
		skb = mtk_wlan_rx_80211_to_8023(mac, frame, len);
		if (!skb)
			return;
	} else {
		/* Already an Ethernet frame (kalProcessRxPacket()). */
		if (len < MTK_WLAN_ETH_HDR_LEN) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		skb = dev_alloc_skb(len + NET_IP_ALIGN);
		if (!skb) {
			atomic_inc(&mac->rx_dropped);
			return;
		}
		skb_reserve(skb, NET_IP_ALIGN);
		skb_put_data(skb, frame, len);
	}

	mtk_wlan_netdev_rx(mac, skb);
}
