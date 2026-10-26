/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WLAN gen2 firmware download definitions.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/wlan/gen2/include/nic_init_cmd_event.h
 *   drivers/misc/mediatek/connectivity/wlan/gen2/include/wlan_lib.h
 *   drivers/misc/mediatek/connectivity/wlan/gen2/include/config.h
 *
 * Copyright (C) 2011 MediaTek Inc.
 */

#ifndef _MTK_WLAN_FW_H
#define _MTK_WLAN_FW_H

#include <linux/bits.h>
#include <linux/types.h>

/*
 * "/etc/firmware/" CFG_FW_FILENAME "_" <chip> as built by kalFirmwareOpen()
 * (gl_kal.c:851-856) with the chip suffix from glGetChipInfo()
 * (ahb.c:534-548).  The ROM carries the same prefix (vmlinux.strings:41764).
 */
#define WLAN_FW_PATH_PREFIX		"WIFI_RAM_CODE_"

/* CFG_FW_FILENAME, config.h:923-926. */
#define WLAN_FW_BASENAME		"WIFI_RAM_CODE"

/* CFG_FW_LOAD_ADDRESS / CFG_FW_START_ADDRESS for MT6628, config.h:1352-1355. */
#define WLAN_FW_LOAD_ADDRESS		0x00060000
#define WLAN_FW_START_ADDRESS		0x00060000

/* CMD_PKT_SIZE_FOR_IMAGE, hif_tx.h:100. */
#define WLAN_FW_CHUNK_SIZE		2048

/* CFG_RESPONSE_POLLING_TIMEOUT, config.h:1204 (x 10 ms). */
#define WLAN_FW_READY_POLL_COUNT	512
#define WLAN_FW_READY_POLL_MS		10

/* RX_RESPONSE_TIMEOUT, nic_rx.c:834. */
#define WLAN_FW_RESPONSE_TIMEOUT_MS	1000

/* BUILD_SIGN('M', 'T', 'K', 'W'), wlan_lib.h:733-737. */
#define WLAN_FW_DIVIDED_SIGNATURE	0x574b544d

/* Offset of u4NumOfEntries, i.e. the start of the CRC-covered area. */
#define WLAN_FW_CRC_OFFSET		8

/* INIT_CMD_ID_*, nic_init_cmd_event.h:69-74. */
#define WLAN_INIT_CMD_ID_DOWNLOAD_BUF	1
#define WLAN_INIT_CMD_ID_WIFI_START	2
#define WLAN_INIT_CMD_ID_QUERY_ERROR	4

/* INIT_EVENT_ID_*, nic_init_cmd_event.h:76-81. */
#define WLAN_INIT_EVENT_ID_CMD_RESULT	1
#define WLAN_INIT_EVENT_ID_PENDING_ERROR 3

/* DOWNLOAD_BUF_*, nic_init_cmd_event.h:103-105. */
#define WLAN_DOWNLOAD_BUF_ENCRYPTION_MODE	BIT(0)
#define WLAN_DOWNLOAD_BUF_NO_CRC_CHECKING	BIT(30)
#define WLAN_DOWNLOAD_BUF_ACK_OPTION		BIT(31)

/* INIT_CMD_STATUS_*, nic_init_cmd_event.h:61-65. */
#define WLAN_INIT_CMD_STATUS_SUCCESS		0
#define WLAN_INIT_CMD_STATUS_INVALID_PARAMS	1
#define WLAN_INIT_CMD_STATUS_CRC_ERROR		2
#define WLAN_INIT_CMD_STATUS_DECRYPT_FAIL	3
#define WLAN_INIT_CMD_STATUS_UNKNOWN		4

/* INIT_WIFI_CMD_T / INIT_HIF_TX_HEADER_T, nic_init_cmd_event.h:89-102. */
struct wlan_init_wifi_cmd {
	u8 cid;
	u8 seq;
	__le16 reserved;
	u8 buffer[];
} __packed;

struct wlan_init_hif_tx_header {
	__le16 tx_byte_count;
	u8 ether_type_offset;
	u8 cs_flags;
	struct wlan_init_wifi_cmd cmd;
} __packed;

/* INIT_CMD_DOWNLOAD_BUF, nic_init_cmd_event.h:106-112. */
struct wlan_init_cmd_download_buf {
	__le32 address;
	__le32 length;
	__le32 crc32;
	__le32 data_mode;
	u8 buffer[];
} __packed;

/* INIT_CMD_WIFI_START, nic_init_cmd_event.h:114-117. */
struct wlan_init_cmd_wifi_start {
	__le32 override;
	__le32 address;
} __packed;

/* INIT_HIF_RX_HEADER_T, nic_init_cmd_event.h:127-141. */
struct wlan_init_hif_rx_header {
	__le16 rx_byte_count;
	u8 eid;
	u8 seq;
	u8 buffer[];
} __packed;

/* FWDL_SECTION_INFO_T / FIRMWARE_DIVIDED_DOWNLOAD_T, wlan_lib.h:598-611. */
struct wlan_fw_section {
	__le32 offset;
	__le32 reserved;
	__le32 length;
	__le32 dest_addr;
} __packed;

struct wlan_fw_divided_header {
	__le32 signature;
	__le32 crc;
	__le32 num_of_entries;
	__le32 reserved;
	struct wlan_fw_section sections[];
} __packed;

#endif /* _MTK_WLAN_FW_H */
