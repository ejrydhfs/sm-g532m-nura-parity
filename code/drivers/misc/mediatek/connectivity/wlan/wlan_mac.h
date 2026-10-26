/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WLAN gen2 802.11 MAC layer - private definitions.
 *
 * The host side of the MT6628/MT6735 WLAN function is split in two:
 *
 *  - the HIF (wlan_ahb.c) moves HIF_TX_HEADER_T prefixed buffers to the
 *    firmware and hands every received buffer up unchanged;
 *  - this layer owns the cfg80211 wiphy, the Ethernet netdev, the TX
 *    queue with its TC credit accounting, the 802.3 <-> 802.11 data
 *    conversion and the STA management state machine (scan/auth/assoc).
 *
 * Firmware data frames are exchanged in 802.3 format, management frames in
 * 802.11 format; see the comments at the individual frame builders.
 *
 * Ported from Samsung/MTK downstream:
 *   include/nic/hif_tx.h, include/nic/hif_rx.h   (HIF headers)
 *   include/nic/nic_tx.h, nic/nic_tx.c           (TC accounting, TX header)
 *   os/linux/gl_init.c, os/linux/gl_kal.c        (wiphy/netdev glue, RX)
 *   mgmt/auth.c, mgmt/assoc.c                    (auth/assoc frames)
 *   mgmt/scan.c, mgmt/scan_fsm.c                 (BSS results, probe)
 *   os/linux/gl_rst.c                            (reset handling)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#ifndef _MTK_WLAN_MAC_H
#define _MTK_WLAN_MAC_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#include "wlan_hif.h"

/*
 * HIF TX header, hif_tx.h:145-163.  Field offsets are the ones annotated by
 * the DW/byte comments at hif_tx.h:106-137:
 *
 *   DW0.0-1 u2TxByteCount_UserPriority   DW2.0-1 u2SeqNo
 *   DW0.2   ucEtherTypeOffset            DW2.2   ucStaRecIdx
 *   DW0.3   ucResource_PktType_CSflags   DW2.3   ucForwardingType_SessionID
 *   DW1.0   ucWlanHeaderLength           DW3.0   ucPacketSeqNo
 *   DW1.1   ucPktFormtId_Flags           DW3.1   ucAck_BIP_BasicRate
 *   DW1.2-3 u2LLH                        DW3.2-3 aucReserved
 */
#define MTK_WLAN_HIF_TX_HDR_LEN		16
#define MTK_WLAN_HIF_RX_HDR_LEN		12

/* hif_tx.h:106-137. */
#define HIF_TX_HDR_TX_BYTE_COUNT_MASK		GENMASK(11, 0)
#define HIF_TX_HDR_USER_PRIORITY_OFFSET		12
#define HIF_TX_HDR_ETHER_TYPE_OFFSET_MASK	GENMASK(7, 0)
#define HIF_TX_HDR_RESOURCE_MASK		GENMASK(5, 2)
#define HIF_TX_HDR_RESOURCE_OFFSET		2
#define HIF_TX_HDR_PACKET_TYPE_MASK		GENMASK(7, 6)
#define HIF_TX_HDR_PACKET_TYPE_OFFSET		6
#define HIF_TX_HDR_WLAN_HEADER_LEN_MASK		GENMASK(5, 0)
#define HIF_TX_HDR_FORMAT_ID_MASK		GENMASK(2, 0)
#define HIF_TX_HDR_NETWORK_TYPE_MASK		GENMASK(5, 4)
#define HIF_TX_HDR_NETWORK_TYPE_OFFSET		4
#define HIF_TX_HDR_FLAG_1X_FRAME		BIT(6)
#define HIF_TX_HDR_FLAG_802_11_FORMAT		BIT(7)
#define HIF_TX_HDR_PS_FORWARDING_TYPE_MASK	GENMASK(1, 0)
#define HIF_TX_HDR_PS_SESSION_ID_OFFSET		2
#define HIF_TX_HDR_BURST_END			BIT(5)
#define HIF_TX_HDR_NEED_ACK			BIT(0)
#define HIF_TX_HDR_BIP				BIT(1)
#define HIF_TX_HDR_BASIC_RATE			BIT(2)
#define HIF_TX_HDR_NEED_TX_DONE_STATUS		BIT(3)
#define HIF_TX_HDR_RTS				BIT(4)

/* ENUM_HIF_TX_PKT_TYPE_T, hif_tx.h:178-185. */
enum mtk_wlan_hif_tx_pkt_type {
	HIF_TX_PKT_TYPE_DATA = 0,
	HIF_TX_PKT_TYPE_CMD,
	HIF_TX_PKT_TYPE_HIF_LOOPBACK,
	HIF_TX_PKT_TYPE_MANAGEMENT,
};

/* ENUM_HIF_RX_PKT_TYPE_T, hif_rx.h:118-124. */
enum mtk_wlan_hif_rx_pkt_type {
	HIF_RX_PKT_TYPE_DATA = 0,
	HIF_RX_PKT_TYPE_EVENT,
	HIF_RX_PKT_TYPE_TX_LOOPBACK,
	HIF_RX_PKT_TYPE_MANAGEMENT,
};

/* hif_rx.h:94-112. */
#define HIF_RX_HDR_PACKET_TYPE_MASK	GENMASK(1, 0)
#define HIF_RX_HDR_SEC_MODE_MASK	GENMASK(5, 2)
#define HIF_RX_HDR_SEC_MODE_OFFSET	2
#define HIF_RX_HDR_HEADER_LEN		GENMASK(7, 2)
#define HIF_RX_HDR_HEADER_LEN_OFFSET	2
#define HIF_RX_HDR_HEADER_OFFSET_MASK	GENMASK(1, 0)
#define HIF_RX_HDR_80211_HEADER_FORMAT	BIT(0)
#define HIF_RX_HDR_DO_REORDER		BIT(1)
#define HIF_RX_HDR_PAL			BIT(2)
#define HIF_RX_HDR_TCL			BIT(3)
#define HIF_RX_HDR_NETWORK_IDX_MASK	GENMASK(7, 4)
#define HIF_RX_HDR_NETWORK_IDX_OFFSET	4
#define HIF_RX_HDR_SEQ_NO_MASK		GENMASK(11, 0)
#define HIF_RX_HDR_TID_MASK		GENMASK(14, 12)
#define HIF_RX_HDR_TID_OFFSET		12
#define HIF_RX_HDR_BAR_FRAME		BIT(15)
#define HIF_RX_HW_CHNL_NUM_MAX_2G4	14
#define HIF_RX_HW_CHNL_NUM_MAX_4G_5G	(255 - HIF_RX_HW_CHNL_NUM_MAX_2G4)

/*
 * Traffic classes, nic_tx.h:357-363.  TC0..TC3 carry WMM access categories
 * on HIF TX port 0, TC4 carries commands and management frames on port 1,
 * TC5 carries broadcast/data frames that have no peer record.
 */
enum mtk_wlan_tc {
	MTK_WLAN_TC0 = 0,
	MTK_WLAN_TC1,
	MTK_WLAN_TC2,
	MTK_WLAN_TC3,
	MTK_WLAN_TC4,
	MTK_WLAN_TC5,
	MTK_WLAN_TC_NUM
};

/*
 * Initial TC buffer counts, NIC_TX_BUFF_COUNT_* with CFG_SRAM_SIZE_OPTION 1
 * chosen for MT6735 (nic_tx.h:299-304, gen2/Makefile:82-84).
 */
#define MTK_WLAN_TC_CREDITS_INIT { 1, 36, 1, 1, 4, 1 }

/* Default association listen interval, DEFAULT_LISTEN_INTERVAL mac.h:227. */
#define MTK_WLAN_LISTEN_INTERVAL	10

/* Supported 2.4 GHz 500 kb/s rate codes, mac.h:234-247. */
#define MTK_WLAN_RATE_1M		2
#define MTK_WLAN_RATE_2M		4
#define MTK_WLAN_RATE_5_5M		11
#define MTK_WLAN_RATE_11M		22
#define MTK_WLAN_RATE_6M		12
#define MTK_WLAN_RATE_9M		18
#define MTK_WLAN_RATE_12M		24
#define MTK_WLAN_RATE_18M		36
#define MTK_WLAN_RATE_24M		48
#define MTK_WLAN_RATE_36M		72
#define MTK_WLAN_RATE_48M		96
#define MTK_WLAN_RATE_54M		108
#define MTK_WLAN_RATE_BASIC_BIT		BIT(7)

/*
 * Convert an RCPI value from the HIF RX header to dBm.  RCPI_TO_dBm()
 * mac.h:2533-2534 with RCPI_HIGH_BOUND (220) and NDBM_LOW_BOUND_FOR_RCPI
 * (110) from mac.h:265-267.
 */
static inline int mtk_wlan_rcpi_to_dbm(u8 rcpi)
{
	if (rcpi > 220)
		rcpi = 220;
	return (rcpi >> 1) - 110;
}

/* Network type index, NETWORK_TYPE_AIS_INDEX wlan_def.h:513-517. */
#define MTK_WLAN_NETWORK_TYPE_AIS	0

/* STA_REC_INDEX_NOT_FOUND, cnm_mem.h:501 / que_mgt.h:309. */
#define MTK_WLAN_STA_REC_NOT_FOUND	0xFE

/* 802.11 header/LLC sizes used by the frame builders. */
#define MTK_WLAN_MAC_HDR_LEN		24	/* WLAN_MAC_MGMT_HEADER_LEN */
#define MTK_WLAN_QOS_CTRL_LEN		2
#define MTK_WLAN_LLC_LEN		8	/* LLC(3) + SNAP(5) */
#define MTK_WLAN_ETH_HDR_LEN		14

/* Largest 802.11 frame this driver builds (firmware limit 2312 body). */
#define MTK_WLAN_TX_FRAME_MAX		MTK_WLAN_RX_MAX_PKT_SIZE
#define MTK_WLAN_TX_BUF_SIZE		(MTK_WLAN_HIF_TX_HDR_LEN + \
					 MTK_WLAN_TX_FRAME_MAX)
/* Largest management frame (probe/assoc request with all user IEs). */
#define MTK_WLAN_MGMT_FRAME_MAX		512
/* Associated IEs kept for cfg80211_connect_result(). */
#define MTK_WLAN_ASSOC_IES_MAX		256

/* TX queue depth before the netdev queue is stopped. */
#define MTK_WLAN_TXQ_DEPTH		64

/* Management frame retries and per-attempt timeout. */
#define MTK_WLAN_MGMT_RETRIES		3
#define MTK_WLAN_MGMT_TIMEOUT_MS	1000

/*
 * Dwell time per channel when cfg80211 does not request one (ms), and the
 * polling interval used so an abort is noticed while dwelling.
 */
#define MTK_WLAN_SCAN_DWELL_MS		40
#define MTK_WLAN_SCAN_POLL_MS		10
#define MTK_WLAN_SCAN_DWELL_MAX_MS	1000
/* Maximum number of SSIDs probed per channel (max_scan_ssids). */
#define MTK_WLAN_MAX_SCAN_SSIDS		4

/**
 * struct mtk_wlan_hif_tx_header - HIF TX header, 16 bytes (hif_tx.h:145-163)
 * @tx_byte_count_user_priority: TX byte count in bits 11:0, user priority
 *	(or, for TC4, any value) in bits 15:12
 * @ether_type_offset: offset of the EtherType in 16-bit words from the start
 *	of this header (nic_tx.c:1409-1416)
 * @resource_pkttype_csflags: TC in bits 5:2, packet type in bits 7:6, TX
 *	checksum offload flags in bits 1:0 (unused here)
 * @wlan_header_length: length of the 802.11 MAC header, 0 for 802.3 frames
 * @pktfmtid_flags: format id bits 2:0, network type bits 5:4, 802.1x flag
 *	bit 6, 802.11 format flag bit 7
 * @llh: PAL/BOW link header, unused
 * @seq_no: ACL sequence number, unused
 * @sta_rec_idx: firmware STA record index, %MTK_WLAN_STA_REC_NOT_FOUND when
 *	the driver has no peer record
 * @forwarding_type_sessionid: PS forwarding type bits 1:0, PS session id
 *	bits 4:2, burst end bit 5
 * @packet_seq_no: driver packet sequence for TX-done matching, unused
 * @ack_bip_basicrate: need-ack bit 0, BIP bit 1, basic rate bit 2, need
 *	TX-done status bit 3, RTS bit 4
 * @reserved: padding to 16 bytes
 */
struct mtk_wlan_hif_tx_header {
	__le16 tx_byte_count_user_priority;
	u8 ether_type_offset;
	u8 resource_pkttype_csflags;
	u8 wlan_header_length;
	u8 pktfmtid_flags;
	__le16 llh;
	__le16 seq_no;
	u8 sta_rec_idx;
	u8 forwarding_type_sessionid;
	u8 packet_seq_no;
	u8 ack_bip_basicrate;
	u8 reserved[2];
} __packed;

/**
 * struct mtk_wlan_hif_rx_header - HIF RX header, 12 bytes (hif_rx.h:96-113)
 * @packet_len: length of header plus payload, hardware appended DW excluded
 * @packet_type: packet type bits 1:0, security mode bits 5:2
 * @hdr_len_offset: 802.11 MAC header length in bits 7:2, header offset in
 *	bits 1:0
 * @reorder_pal_tcl: 802.11 format bit 0, do-reorder bit 1, PAL bit 2, TCL
 *	bit 3, network index bits 7:4
 * @seq_no_tid: sequence number bits 11:0, TID bits 14:12, BAR flag bit 15
 * @sta_rec_idx: firmware STA record index
 * @rcpi: received channel power indicator, RCPI_TO_dBm() mac.h:2533
 * @hw_channel: hardware channel number, HIF_RX_HDR_GET_CHNL_NUM() hif_rx.h:152
 * @reserved: padding to 12 bytes
 */
struct mtk_wlan_hif_rx_header {
	__le16 packet_len;
	__le16 packet_type;
	u8 hdr_len_offset;
	u8 reorder_pal_tcl;
	__le16 seq_no_tid;
	u8 sta_rec_idx;
	u8 rcpi;
	u8 hw_channel;
	u8 reserved;
} __packed;

/**
 * enum mtk_wlan_state - connection state of the STA interface
 * @MTK_WLAN_STATE_DOWN: firmware is not running
 * @MTK_WLAN_STATE_IDLE: firmware runs, interface not connected
 * @MTK_WLAN_STATE_SCANNING: a software scan is in progress
 * @MTK_WLAN_STATE_CONNECTING: auth/assoc exchange in progress
 * @MTK_WLAN_STATE_CONNECTED: associated to &struct mtk_wlan_mac.bssid
 */
enum mtk_wlan_state {
	MTK_WLAN_STATE_DOWN,
	MTK_WLAN_STATE_IDLE,
	MTK_WLAN_STATE_SCANNING,
	MTK_WLAN_STATE_CONNECTING,
	MTK_WLAN_STATE_CONNECTED,
};

/**
 * struct mtk_wlan_mgmt_rsp - a management response matched by the waiter
 * @status: status code field of the response
 * @aid: association id, valid for an association response only
 * @ies: IEs following the fixed fields
 * @ies_len: length of @ies
 */
struct mtk_wlan_mgmt_rsp {
	u16 status;
	u16 aid;
	u8 ies[MTK_WLAN_ASSOC_IES_MAX];
	size_t ies_len;
};

/**
 * struct mtk_wlan_cmd - one gen2 firmware command in flight
 * @lock: serializes command senders
 * @wait_lock: protects @pending, @seq, @cid, @result and @aborted
 * @done: completed by the command-result event or by an abort
 * @seq: sequence number of the pending command
 * @cid: command id of the pending command
 * @result: completion status, 0 or a negative errno
 * @pending: a command is waiting for its result
 * @aborted: the firmware went away; further commands fail with -ENETDOWN
 */
struct mtk_wlan_cmd {
	struct mutex lock;
	spinlock_t wait_lock;
	struct completion done;
	u8 seq;
	u8 cid;
	int result;
	bool pending;
	bool aborted;
};

/**
 * struct mtk_wlan_mac - 802.11 MAC and cfg80211 instance
 * @hif: host interface this instance is bound to
 * @dev: device used for DMA, firmware and logging
 * @wiphy: registered wiphy, created in mtk_wlan_mac_attach()
 * @ndev: Ethernet netdev, registered while the firmware is up
 * @wdev: wireless_dev embedded in @ndev
 * @link_work: registers/unregisters @ndev after a firmware transition
 * @tx_work: drains &txq into the HIF TX data port
 * @scan_work: software scan, builds and transmits probe requests
 * @connect_work: auth/assoc state machine
 * @lock: serializes the scan and connect operations
 * @tx_lock: serializes HIF frame assembly and port writes
 * @txq_lock: protects &txq and &tc_credits
 * @txq: frames queued by the netdev transmit path
 * @tc_credits: remaining firmware TX buffers per traffic class
 * @tc_max: initial TX buffers per traffic class
 * @fw_up: firmware finished downloading and the HIF is live
 * @registered: @ndev is registered with the stack
 * @state: connection state
 * @mac_addr: own MAC address
 * @bssid: associated AP, valid when connected
 * @ssid: SSID of the current association
 * @ssid_len: length of @ssid
 * @channel: 2.4 GHz channel of the current association
 * @assoc_ts: jiffies when the association completed
 * @scan_req: scan request being processed, owned by cfg80211
 * @scan_aborted: scan was cancelled and must report %aborted
 * @connect_aborted: a local disconnect cancelled the association attempt
 * @connect_bss: BSS entry taken by %mtk_wlan_mgmt_connect() and handed to
 *	cfg80211 with the connect result (cfg80211.h:8620-8640)
 * @mgmt_done: completed when a matching management response arrives
 * @mgmt_stype: frame subtype the waiter expects, 0 when nobody waits
 * @mgmt_peer: transmitter address the waiter expects
 * @mgmt_err: set when the firmware went down while waiting
 * @mgmt_rsp: response stored by the RX path
 * @rx_lock: protects the management response slot and the last RCPI
 * @assoc_ie: association IEs supplied by cfg80211, copied out of @sme
 * @assoc_ie_len: length of @assoc_ie
 * @req_ie: association request IEs reported to cfg80211
 * @req_ie_len: length of @req_ie
 * @connected: an association is established
 * @last_signal: last RCPI reported by the HIF RX header
 * @have_signal: @last_signal is valid
 * @ap_rate_set: operational rate set of the AP, RATE_SET_* bits
 * @ap_basic_rate_set: basic rate set of the AP, RATE_SET_* bits
 * @ap_phy_type_set: PHY_TYPE_BIT_* set derived from the AP's rate set
 * @ap_nonht_phy: PHY_TYPE_*_INDEX used for slot time and CWmin
 * @ap_short_preamble: AP allows the short preamble
 * @ap_short_slot: AP uses the short slot time
 * @ap_erp_protect: AP ERP IE requests protection
 * @sta_index: firmware STA record index of the AP
 * @sta_valid: @sta_index exists in the firmware
 * @aid: association id from the association response
 * @cmd: firmware command/event channel state
 * @tx_dropped: frames dropped by the driver before reaching the HIF
 * @rx_dropped: frames dropped because they were malformed or unroutable
 * @tx_buf: assembly buffer for one HIF TX header plus frame
 */
struct mtk_wlan_mac {
	struct mtk_wlan_hif *hif;
	struct device *dev;
	struct wiphy *wiphy;
	struct net_device *ndev;
	struct wireless_dev *wdev;

	struct work_struct link_work;
	struct work_struct tx_work;
	struct work_struct scan_work;
	struct work_struct connect_work;

	struct mutex lock;
	struct mutex tx_lock;
	spinlock_t txq_lock;
	struct sk_buff_head txq;
	u8 tc_credits[MTK_WLAN_TC_NUM];
	u8 tc_max[MTK_WLAN_TC_NUM];

	bool fw_up;
	bool registered;
	enum mtk_wlan_state state;

	u8 mac_addr[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 ssid_len;
	u16 channel;
	unsigned long assoc_ts;

	struct cfg80211_scan_request *scan_req;
	bool scan_aborted;
	bool connect_aborted;
	struct cfg80211_bss *connect_bss;

	struct completion mgmt_done;
	u16 mgmt_stype;
	u8 mgmt_peer[ETH_ALEN];
	int mgmt_err;
	struct mtk_wlan_mgmt_rsp mgmt_rsp;
	spinlock_t rx_lock;

	u8 assoc_ie[MTK_WLAN_ASSOC_IES_MAX];
	size_t assoc_ie_len;
	u8 req_ie[MTK_WLAN_ASSOC_IES_MAX];
	size_t req_ie_len;

	bool connected;
	u8 last_signal;
	bool have_signal;

	/* AP parameters for the firmware BSS/STA record commands. */
	u16 ap_rate_set;
	u16 ap_basic_rate_set;
	u8 ap_phy_type_set;
	u8 ap_nonht_phy;
	bool ap_short_preamble;
	bool ap_short_slot;
	bool ap_erp_protect;
	u8 sta_index;
	bool sta_valid;
	u16 aid;

	struct mtk_wlan_cmd cmd;

	atomic_t tx_dropped;
	atomic_t rx_dropped;

	u8 *tx_buf;
};

/* wlan_mac.c */
int mtk_wlan_mac_attach(struct mtk_wlan_hif *hif);
void mtk_wlan_mac_detach(struct mtk_wlan_hif *hif);
void mtk_wlan_mac_fw_up(struct mtk_wlan_hif *hif);
void mtk_wlan_mac_fw_down(struct mtk_wlan_hif *hif);

/* wlan_netdev.c */
int mtk_wlan_netdev_create(struct mtk_wlan_mac *mac);
void mtk_wlan_netdev_destroy(struct mtk_wlan_mac *mac);
int mtk_wlan_netdev_register(struct mtk_wlan_mac *mac);
void mtk_wlan_netdev_unregister(struct mtk_wlan_mac *mac);
void mtk_wlan_netdev_rx(struct mtk_wlan_mac *mac, struct sk_buff *skb);
void mtk_wlan_netdev_wake_tx(struct mtk_wlan_mac *mac);
void mtk_wlan_netdev_link_changed(struct mtk_wlan_mac *mac, bool up);

/* wlan_txrx.c */
int mtk_wlan_mgmt_tx(struct mtk_wlan_mac *mac, const u8 *frame, size_t len);
int mtk_wlan_data_tx(struct mtk_wlan_mac *mac, struct sk_buff *skb);
void mtk_wlan_tx_work(struct work_struct *work);
struct sk_buff *mtk_wlan_rx_80211_to_8023(struct mtk_wlan_mac *mac,
					  const u8 *frame, size_t len);
bool mtk_wlan_tc_acquire(struct mtk_wlan_mac *mac, unsigned int tc);
void mtk_wlan_tc_release(struct mtk_wlan_mac *mac, unsigned int tc,
			 unsigned int count);
void mtk_wlan_rx_data(struct mtk_wlan_mac *mac, const u8 *frame, size_t len,
		      bool is_80211);

/* wlan_mgmt.c */
int mtk_wlan_mgmt_scan(struct mtk_wlan_mac *mac,
		       struct cfg80211_scan_request *req);
void mtk_wlan_mgmt_abort_scan(struct mtk_wlan_mac *mac);
void mtk_wlan_mgmt_scan_work(struct work_struct *work);
int mtk_wlan_mgmt_connect(struct mtk_wlan_mac *mac,
			  struct cfg80211_connect_params *sme);
int mtk_wlan_mgmt_disconnect(struct mtk_wlan_mac *mac, u16 reason);
void mtk_wlan_mgmt_connect_work(struct work_struct *work);
void mtk_wlan_mgmt_quiesce(struct mtk_wlan_mac *mac);
void mtk_wlan_rx_mgmt(struct mtk_wlan_mac *mac, const u8 *frame, size_t len,
		      u8 channel, u8 rcpi);
void mtk_wlan_mgmt_deauth_rx(struct mtk_wlan_mac *mac, u16 reason,
			     bool locally);

#endif /* _MTK_WLAN_MAC_H */
