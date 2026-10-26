/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN WMT (Wireless Management Task) public interface.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/common/conn_soc/include/wmt_exp.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_WCN_WMT_H_
#define _MTK_WCN_WMT_H_

#include <linux/types.h>

/* Downstream ENUM_WMTDRV_TYPE_T. */
enum mtk_wcn_wmt_drv {
	WMTDRV_BT = 0,
	WMTDRV_FM = 1,
	WMTDRV_GPS = 2,
	WMTDRV_WIFI = 3,
	WMTDRV_WMT = 4,
	WMTDRV_STP = 5,
	WMTDRV_LPBK = 6,
	WMTDRV_COREDUMP = 7,
	WMTDRV_MAX
};

/* Downstream ENUM_WMTMSG_TYPE_T. */
enum mtk_wcn_wmt_msg {
	WMTMSG_POWER_ON = 0,
	WMTMSG_POWER_OFF = 1,
	WMTMSG_RESET = 2,
	WMTMSG_STP_RDY = 3,
	WMTMSG_HW_FUNC_ON = 4,
	WMTMSG_MAX
};

/* Downstream ENUM_WMTHWVER_TYPE_T. */
enum mtk_wcn_wmt_hwver {
	WMTHWVER_E1 = 0x0,
	WMTHWVER_E2 = 0x1,
	WMTHWVER_E3 = 0x2,
	WMTHWVER_E4 = 0x3,
	WMTHWVER_E5 = 0x4,
	WMTHWVER_E6 = 0x5,
	WMTHWVER_E7 = 0x6,
	WMTHWVER_MAX,
	WMTHWVER_INVALID = 0xff
};

/* Downstream ENUM_WMTDSNS_TYPE_T. */
enum mtk_wcn_wmt_dsns {
	WMTDSNS_FM_DISABLE = 0,
	WMTDSNS_FM_ENABLE = 1,
	WMTDSNS_FM_GPS_DISABLE = 2,
	WMTDSNS_FM_GPS_ENABLE = 3,
	WMTDSNS_MAX
};

/**
 * typedef mtk_wcn_wmt_msgcb - WMT message callback
 * @src: source driver
 * @dst: destination driver
 * @msg: message type
 * @data: read-only payload, freed by WMT after the callback returns
 * @len: payload length in bytes
 */
typedef void (*mtk_wcn_wmt_msgcb)(enum mtk_wcn_wmt_drv src,
				  enum mtk_wcn_wmt_drv dst,
				  enum mtk_wcn_wmt_msg msg,
				  void *data, u32 len);

/* WLAN driver callbacks, registered with mtk_wcn_wmt_wlan_reg(). */
struct mtk_wcn_wmt_wlan_cb {
	int (*probe)(void);
	int (*remove)(void);
	int (*bus_tx_cnt)(void);
	int (*bus_tx_cnt_clr)(void);
};

/* Power on/off one WCN function on the connectivity SoC. */
bool mtk_wcn_wmt_func_on(enum mtk_wcn_wmt_drv type);
bool mtk_wcn_wmt_func_off(enum mtk_wcn_wmt_drv type);

/* Register/unregister the per-driver message callback. */
int mtk_wcn_wmt_msgcb_reg(enum mtk_wcn_wmt_drv type, mtk_wcn_wmt_msgcb cb);
int mtk_wcn_wmt_msgcb_unreg(enum mtk_wcn_wmt_drv type);

/* Register/unregister the WLAN driver with WMT. */
int mtk_wcn_wmt_wlan_reg(const struct mtk_wcn_wmt_wlan_cb *cb);
int mtk_wcn_wmt_wlan_unreg(void);

/* Force a connectivity SoC reset after a fatal error. */
bool mtk_wcn_wmt_assert(enum mtk_wcn_wmt_drv type, u32 reason);
int mtk_wcn_wmt_system_state_reset(void);

/* Coexistence (desense) and thermal controls. */
bool mtk_wcn_wmt_dsns_ctrl(enum mtk_wcn_wmt_dsns type);
s8 mtk_wcn_wmt_therm_ctrl(u8 type);

/* Running hardware version of the connectivity SoC. */
enum mtk_wcn_wmt_hwver mtk_wcn_wmt_hwver_get(void);
u32 mtk_wcn_wmt_hwver_raw_get(void);
u32 mtk_wcn_wmt_fwver_get(void);
bool mtk_wcn_wmt_co_clock_flag_get(void);

/* ------------------------------------------------------------------------- *
 * Read-only queries used by the GPS vendor character device.
 * Downstream COMBO_IOC_GPS_IC_HW_VERSION / COMBO_IOC_GPS_IC_FW_VERSION and
 * COMBO_IOC_CO_CLOCK_FLAG (wmt_exp.c:424-429).
 * ------------------------------------------------------------------------- */

/* Downstream ENUM_WMTCHIN_TYPE_T (wmt_lib.h:139-145). */
enum mtk_wcn_wmt_icinfo {
	WMT_ICINFO_CHIPID = 0,
	WMT_ICINFO_HWVER = 1,
	WMT_ICINFO_MAPPINGHWVER = 2,
	WMT_ICINFO_FWVER = 3,
	WMT_ICINFO_MAX
};

u32 mtk_wcn_wmt_ic_info_get(enum mtk_wcn_wmt_icinfo type);
bool mtk_wcn_wmt_co_clock_flag_get(void);

/* ------------------------------------------------------------------------- *
 * Coexistence
 * ------------------------------------------------------------------------- */

/* Downstream COEX_BUF (wmt_dev.h:37-40). */
#define WMT_COEX_BUF_SIZE	128

struct wmt_coex_buf {
	__u8 buffer[WMT_COEX_BUF_SIZE];
	__s32 avail_size;
};

/* Downstream ENUM_WMTDRV_CMD_T coexistence entries (wmt_dev.h:45-60).
 * The values are the WMTDRV_CMD_* command ids the vendor debug interface
 * uses, numbered from COEXDBG_00.
 */
enum mtk_wcn_wmt_coex_dbg {
	WMTDRV_CMD_COEXDBG_00 = 0,
	WMTDRV_CMD_COEXDBG_01 = 1,
	WMTDRV_CMD_COEXDBG_02 = 2,
	WMTDRV_CMD_COEXDBG_03 = 3,
	WMTDRV_CMD_COEXDBG_04 = 4,
	WMTDRV_CMD_COEXDBG_05 = 5,
	WMTDRV_CMD_COEXDBG_06 = 6,
	WMTDRV_CMD_COEXDBG_07 = 7,
	WMTDRV_CMD_COEXDBG_08 = 8,
	WMTDRV_CMD_COEXDBG_09 = 9,
	WMTDRV_CMD_COEXDBG_10 = 10,
	WMTDRV_CMD_COEXDBG_11 = 11,
	WMTDRV_CMD_COEXDBG_12 = 12,
	WMTDRV_CMD_COEXDBG_13 = 13,
	WMTDRV_CMD_COEXDBG_14 = 14,
	WMTDRV_CMD_COEXDBG_15 = 15,
	WMTDRV_CMD_COEXDBG_MAX
};

/* Run one coexistence debug command against the firmware and return the
 * event payload in @out (COEX_BUF).  Requires an enabled STP link.
 */
int mtk_wcn_wmt_coex_dbg(u32 cmd, struct wmt_coex_buf *out);

/* Downstream WMT_BGW_DESENSE netlink command values (bgw_desense.h:56-59). */
#define WMT_BGW_DS_CMD_OFF	0
#define WMT_BGW_DS_CMD_ON	1
#define WMT_BGW_DS_CMD_ACK	2

/* Ask the vendor BGW daemon to start/stop sending desense commands. */
int mtk_wcn_wmt_bgw_desense_ctrl(bool enable);

/* Forward one 14-byte BGW desense payload to the firmware
 * (downstream WMT_IOCTL_SEND_BGW_DS_CMD).
 */
#define WMT_BGW_DS_PAYLOAD_SIZE	14
int mtk_wcn_wmt_bgw_ds_send(const __u8 *payload, u32 len);

/* ------------------------------------------------------------------------- *
 * LTE IDC (BT/Wi-Fi <-> LTE in-device coexistence)
 *
 * Downstream wmt_idc.c talks to the modem through the MTK conn_md IPC
 * service.  conn_md is not part of this kernel, so the WMT side implements
 * the complete firmware protocol and exports a registration API that a
 * future CCCI/modem bridge must use.  Until such a bridge registers, the
 * firmware->modem direction reports -ENODEV and the modem->firmware
 * direction is rejected with -ENODEV: no LTE data is ever fabricated.
 * ------------------------------------------------------------------------- */

/* Downstream WMT_IDC_TX_OPCODE (wmt_idc.h:66-78). */
enum mtk_wcn_wmt_idc_tx_opcode {
	MTK_WCN_WMT_IDC_TX_LTE_PARA = 0x0a,
	MTK_WCN_WMT_IDC_TX_LTE_FREQ = 0x0b,
	MTK_WCN_WMT_IDC_TX_WIFI_MAX_POWER = 0x0c,
	MTK_WCN_WMT_IDC_TX_DEBUG_MONITOR = 0x0e,
	MTK_WCN_WMT_IDC_TX_SPLIT_FILTER = 0x0f,
	MTK_WCN_WMT_IDC_TX_LTE_CONNECTION_STATUS = 0x16,
	MTK_WCN_WMT_IDC_TX_LTE_HW_IF_INDICATION = 0x17,
	MTK_WCN_WMT_IDC_TX_LTE_INDICATION = 0x20,
};

/* Downstream WMT_IDC_RX_OPCODE (wmt_idc.h:81-90), translated into the msg
 * ids the modem bridge receives.  The debug monitor frame is consumed by
 * WMT and never forwarded.
 */
enum mtk_wcn_wmt_idc_msg {
	MTK_WCN_WMT_IDC_MSG_BTWF_DEF_PARA = 0,
	MTK_WCN_WMT_IDC_MSG_BTWF_CHAN_RAN,
	MTK_WCN_WMT_IDC_MSG_LTE_FREQ_IDX_TABLE,
	MTK_WCN_WMT_IDC_MSG_BTWF_PROFILE_IND,
	MTK_WCN_WMT_IDC_MSG_UART_PIN_SEL,
	MTK_WCN_WMT_IDC_MSG_MAX
};

struct mtk_wcn_wmt_idc_ops {
	/* Deliver one message received from the connectivity firmware to the
	 * modem (LTE) side.  Runs in process context.  Return 0 on success.
	 */
	int (*send_to_modem)(enum mtk_wcn_wmt_idc_msg msg,
			     const u8 *data, u32 len);
};

int mtk_wcn_wmt_idc_register(const struct mtk_wcn_wmt_idc_ops *ops);
void mtk_wcn_wmt_idc_unregister(void);
bool mtk_wcn_wmt_idc_is_registered(void);

/* Modem -> connectivity firmware.  @opcode is one of the
 * mtk_wcn_wmt_idc_tx_opcode values; the payload is forwarded verbatim inside
 * the WMT LTE-coex command (wmt_core.c:2170-2230).
 * Returns -ENODEV when the STP link/WMT engine is not available.
 */
int mtk_wcn_wmt_idc_send(u8 opcode, const u8 *data, u32 len);

/* /dev/wmtWifi control channel (downstream wmt_chrdev_wifi.c). */
int mtk_wcn_wmt_wifi_init(void);
void mtk_wcn_wmt_wifi_exit(void);

#endif /* _MTK_WCN_WMT_H_ */
