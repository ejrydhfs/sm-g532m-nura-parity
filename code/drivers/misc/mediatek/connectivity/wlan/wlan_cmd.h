/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WLAN gen2 firmware command/event channel - wire definitions.
 *
 * Ported from Samsung/MTK downstream:
 *   include/nic_cmd_event.h   (WIFI_CMD_T, WIFI_EVENT_T, CMD and EVENT structs)
 *   include/nic/cmd_buf.h     (CMD_INFO_T, CMD_HDR_SIZE)
 *   nic/nic.c                 (nicUpdateBss, nicActivateNetwork,
 *                              cnmStaRecChangeState callers)
 *   mgmt/cnm_mem.c            (cnmStaSendUpdateCmd, cnmStaSendRemoveCmd)
 *   nic/nic_tx.c              (nicTxCmd command framing, TC4/port 1)
 *   nic/nic_rx.c              (EVENT_ID_CMD_RESULT matching, nic_rx.c:1789)
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#ifndef _MTK_WLAN_CMD_H
#define _MTK_WLAN_CMD_H

#include <linux/types.h>

struct mtk_wlan_mac;
struct cfg80211_bss;

/* WIFI_CMD_T, nic_cmd_event.h:919-929; CMD_HDR_SIZE is hif_tx.h:98. */
#define MTK_WLAN_CMD_HDR_LEN		8
/* WIFI_EVENT_T, nic_cmd_event.h:932-941; EVENT_HDR_SIZE is nic_cmd_event.h:660. */
#define MTK_WLAN_EVENT_HDR_LEN		8
/* Largest PDU this port sends: CMD_SET_BSS_INFO (80 bytes). */
#define MTK_WLAN_CMD_BUF_MAX		128

/* ENUM_CMD_ID_T, nic_cmd_event.h:678-702. */
#define MTK_WLAN_CMD_ID_BSS_ACTIVATE_CTRL	0x15
#define MTK_WLAN_CMD_ID_SET_BSS_INFO		0x16
#define MTK_WLAN_CMD_ID_UPDATE_STA_RECORD	0x17
#define MTK_WLAN_CMD_ID_REMOVE_STA_RECORD	0x18

/* ENUM_EVENT_ID_T, nic_cmd_event.h:809-811. */
#define MTK_WLAN_EVENT_ID_CMD_RESULT		1

/* STA_STATE_1 / STA_STATE_3, mac.h:261-263. */
#define MTK_WLAN_STA_STATE_1			0
#define MTK_WLAN_STA_STATE_3			2

/* EVENT_CMD_RESULT.ucStatus, nic_cmd_event.h:1175-1179. */
#define MTK_WLAN_CMD_STATUS_SUCCESS		0
#define MTK_WLAN_CMD_STATUS_REJECTED		1
#define MTK_WLAN_CMD_STATUS_UNKNOWN		2

/*
 * Response timeout.  The downstream OID layer waits for
 * kalOidComplete() with a 5 s timeout (the value documented next to the
 * wait in gl_kal.c:2445-2449).
 */
#define MTK_WLAN_CMD_TIMEOUT_MS			5000
/*
 * Local policy: how long to wait for a free TC4 buffer before giving up.
 * Downstream polls TC4 resources in the caller (nicTxPollingResource(),
 * nic_tx.c:633-697) instead of inside the HIF write.
 */
#define MTK_WLAN_CMD_TRIES			100
#define MTK_WLAN_CMD_POLL_MS			2

/* WIFI_CMD_T, nic_cmd_event.h:919-929.  First DW shared with HIF_TX_HEADER_T. */
struct mtk_wlan_cmd_hdr {
	__le16 tx_byte_count_user_priority;
	u8 ether_type_offset;
	u8 resource_pkttype_csflags;
	u8 cid;
	u8 set_query;
	u8 seq;
	u8 reserved;
} __packed;

/* WIFI_EVENT_T, nic_cmd_event.h:932-941. */
struct mtk_wlan_event_hdr {
	__le16 packet_len;
	__le16 packet_type;
	u8 eid;
	u8 seq;
	u8 reserved[2];
} __packed;

/* EVENT_CMD_RESULT, nic_cmd_event.h:1175-1179. */
struct mtk_wlan_event_cmd_result {
	u8 cmd_id;
	u8 status;
	u8 reserved[2];
} __packed;

/* CMD_BSS_ACTIVATE_CTRL, nic_cmd_event.h:1414-1418. */
struct mtk_wlan_cmd_bss_activate {
	u8 net_type_index;
	u8 active;
	u8 reserved[2];
} __packed;

/* CMD_SET_BSS_RLM_PARAM_T, nic_cmd_event.h:1420-1435. */
struct mtk_wlan_cmd_set_bss_rlm {
	u8 net_type_index;
	u8 rf_band;
	u8 primary_channel;
	u8 rf_sco;
	u8 erp_protect_mode;
	u8 ht_protect_mode;
	u8 gf_operation_mode;
	u8 tx_rifs_mode;
	__le16 ht_op_info3;
	__le16 ht_op_info2;
	u8 ht_op_info1;
	u8 use_short_preamble;
	u8 use_short_slot_time;
	u8 check_id;
} __packed;

/* CMD_SET_BSS_INFO, nic_cmd_event.h:1437-1460. */
struct mtk_wlan_cmd_set_bss_info {
	u8 net_type_index;
	u8 connection_state;
	u8 current_op_mode;
	u8 ssid_len;
	u8 ssid[32];
	u8 bssid[6];
	u8 is_qbss;
	u8 reserved1;
	__le16 operational_rate_set;
	__le16 bss_basic_rate_set;
	u8 sta_rec_idx_of_ap;
	u8 reserved2;
	u8 reserved3;
	u8 nonht_basic_phy_type;
	u8 auth_mode;
	u8 enc_status;
	u8 phy_type_set;
	u8 own_mac[6];
	u8 wapi_mode;
	u8 is_ap_mode;
	u8 hidden_ssid_mode;
	struct mtk_wlan_cmd_set_bss_rlm rlm;
} __packed;

/* CMD_UPDATE_STA_RECORD_T, nic_cmd_event.h:1462-1488. */
struct mtk_wlan_cmd_update_sta {
	u8 index;
	u8 sta_type;
	u8 mac_addr[6];
	__le16 assoc_id;
	__le16 listen_interval;
	u8 net_type_index;
	u8 desired_phy_type_set;
	__le16 desired_nonht_rate_set;
	__le16 bss_basic_rate_set;
	u8 is_qos;
	u8 is_uapsd_supported;
	u8 sta_state;
	u8 mcs_set;
	u8 sup_mcs32;
	u8 ampdu_param;
	__le16 ht_cap_info;
	__le16 ht_extended_cap;
	__le32 tx_beamforming_cap;
	u8 asel_cap;
	u8 rcpi;
	u8 need_resp;
	u8 uapsd_ac;
	u8 uapsd_sp;
	u8 reserved[3];
} __packed;

/* CMD_REMOVE_STA_RECORD_T, nic_cmd_event.h:1490-1494. */
struct mtk_wlan_cmd_remove_sta {
	u8 index;
	u8 reserved;
	u8 mac_addr[6];
} __packed;

/* wlan_cmd.c */
void mtk_wlan_cmd_init(struct mtk_wlan_mac *mac);
void mtk_wlan_cmd_reset(struct mtk_wlan_mac *mac);
void mtk_wlan_cmd_abort(struct mtk_wlan_mac *mac);
void mtk_wlan_cmd_cancel(struct mtk_wlan_mac *mac);
void mtk_wlan_cmd_event(struct mtk_wlan_mac *mac, const u8 *buf, size_t len);
int mtk_wlan_cmd_prepare_ap(struct mtk_wlan_mac *mac, struct cfg80211_bss *bss);
int mtk_wlan_cmd_bss_activate(struct mtk_wlan_mac *mac, bool active);
int mtk_wlan_cmd_set_bss_info(struct mtk_wlan_mac *mac, bool connected);
int mtk_wlan_cmd_update_sta(struct mtk_wlan_mac *mac, u8 state, u16 aid,
			    bool need_resp);
int mtk_wlan_cmd_remove_sta(struct mtk_wlan_mac *mac);
int mtk_wlan_cmd_teardown(struct mtk_wlan_mac *mac);

#endif /* _MTK_WLAN_CMD_H */
