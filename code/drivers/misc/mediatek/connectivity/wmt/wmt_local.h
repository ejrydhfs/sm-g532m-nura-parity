/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN WMT/CONSYS core - private definitions.
 *
 * Port from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/include/
 *   drivers/misc/mediatek/connectivity/common/conn_soc/include/
 *   drivers/misc/mediatek/connectivity/common/conn_soc/mt6735/include/mtk_wcn_consys_hw.h
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#ifndef _WMT_LOCAL_H_
#define _WMT_LOCAL_H_

#include <linux/firmware.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <mtk_wcn_consys.h>
#include <mtk_wcn_stp.h>
#include <mtk_wcn_wmt.h>

#define WMT_PFX		"WMT: "
#define WMT_STP_PFX	"STP: "

#define wmt_dbg(fmt, ...)	pr_debug(WMT_PFX fmt, ##__VA_ARGS__)
#define wmt_info(fmt, ...)	pr_info(WMT_PFX fmt, ##__VA_ARGS__)
#define wmt_warn(fmt, ...)	pr_warn(WMT_PFX fmt, ##__VA_ARGS__)
#define wmt_err(fmt, ...)	pr_err(WMT_PFX fmt, ##__VA_ARGS__)
#define stp_dbg(fmt, ...)	pr_debug(WMT_STP_PFX fmt, ##__VA_ARGS__)
#define stp_info(fmt, ...)	pr_info(WMT_STP_PFX fmt, ##__VA_ARGS__)
#define stp_warn(fmt, ...)	pr_warn(WMT_STP_PFX fmt, ##__VA_ARGS__)
#define stp_err(fmt, ...)	pr_err(WMT_STP_PFX fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------- *
 * STP wire protocol constants
 * (stp_core.h:118-140, stp_core.c:34)
 * ------------------------------------------------------------------------- */
#define STP_DEL_SIZE		2
#define MTKSTP_CRC_SIZE		2
#define MTKSTP_HEADER_SIZE	4
#define MTKSTP_SEQ_SIZE		8
#define MTKSTP_WINSIZE		7
#define MTKSTP_TX_TIMEOUT	180	/* ms */
#define MTKSTP_RETRY_LIMIT	10

#define MTKSTP_UART_FULL_MODE	BIT(0)
#define MTKSTP_UART_MAND_MODE	BIT(1)
#define MTKSTP_BTIF_FULL_MODE	BIT(2)
#define MTKSTP_BTIF_MAND_MODE	BIT(3)
#define MTKSTP_SDIO_MODE	BIT(4)

/* WMT-LTE coexistence sub-type byte, dropped on non-LTE projects
 * (stp_core.c:1251-1270).
 */
#define WMT_LTE_COEX_FLAG	0x16

/* ------------------------------------------------------------------------- *
 * WMT wire protocol constants (wmt_core.h:82-91)
 * ------------------------------------------------------------------------- */
#define WMT_HDR_LEN		4
#define WMT_STS_LEN		1
#define WMT_FLAG_LEN		1
#define WMT_FUNC_CTRL_PARAM_LEN	1
#define BCNT_PATCH_BUF_HEADROOM	8
#define DWCNT_HIF_CONF		4
#define DWCNT_STRAP_CONF	4

#define WMT_LIB_RX_TIMEOUT	2000	/* ms, wmt_lib.h:69 */
#define MAX_EACH_WMT_CMD	(WMT_LIB_RX_TIMEOUT + 1000)
#define MAX_EACH_FUNC_OFF	(WMT_LIB_RX_TIMEOUT + 1000)
#define MAX_FUNC_ON_TIME	12000
#define MAX_FUNC_OFF_TIME	12000
#define MAX_GPIO_CTRL_TIME	2000
#define WMT_PWRON_RTY_DFT	2
#define MAX_PATCH_NUM		10

/* Default launcher-supplied patch file name (wmt_core.h:52). */
#define WMT_DEFAULT_PATCH_NAME	"mt66xx_patch_hdr.bin"
/* wmt_conf.h:33-34 */
#define WMT_CFG_PREFIX		"/system/etc/firmware/"
#define WMT_CFG_NAME		"WMT_SOC.cfg"

/* WMT register space seen from the firmware (wmt_ic.h:54-60). */
#define GEN_CONFG_BASE		0x80000000
#define GEN_HVR			(GEN_CONFG_BASE + 0x0)
#define GEN_FVR			(GEN_CONFG_BASE + 0x4)
#define GEN_VER_MASK		0x0000ffff
#define GEN_HCR			(GEN_CONFG_BASE + 0x8)
#define GEN_HCR_MASK		0x0000ffff

#define MAJORNUM(x)		((x) & 0x00f0)
#define MINORNUM(x)		((x) & 0x000f)

/* ------------------------------------------------------------------------- *
 * STP parser states (stp_core.h:229-245)
 * ------------------------------------------------------------------------- */
enum stp_parser_state {
	STP_SYNC = 0,
	STP_SEQ,
	STP_ACK,
	STP_NAK,
	STP_TYPE,
	STP_LENGTH,
	STP_CHECKSUM,
	STP_DATA,
	STP_CRC1,
	STP_CRC2,
	STP_RESYNC1,
	STP_RESYNC2,
	STP_RESYNC3,
	STP_RESYNC4,
	STP_FW_MSG,
};

struct stp_parser {
	enum stp_parser_state state;
	u8 seq;
	u8 ack;
	u8 nak;
	u8 type;
	u16 length;
	u8 checksum;
	u16 crc;
	u8 wmt_subtype;
};

struct stp_sequence {
	u8 txseq;		/* last tx pkt's seq + 1 */
	u8 txack;		/* last tx pkt's ack */
	u8 rxack;		/* last rx pkt's ack */
	u8 winspace;		/* current sliding window size */
	u8 expected_rxseq;	/* last rx pkt's seq + 1 */
	u8 retry_times;
};

struct stp_ring {
	spinlock_t lock;
	u8 buffer[MTKSTP_BUFFER_SIZE];
	u32 read_p;
	u32 write_p;
};

/* ------------------------------------------------------------------------- *
 * WMT driver types
 * ------------------------------------------------------------------------- */
enum wmt_drv_status {
	DRV_STS_POWER_OFF = 0,
	DRV_STS_POWER_ON = 1,
	DRV_STS_FUNC_ON = 2,
	DRV_STS_MAX
};

/* Downstream ENUM_WMT_OPID_T (wmt_core.h:118-153).  Only the operations
 * implemented by this driver are listed.
 */
enum wmt_opid {
	WMT_OPID_HIF_CONF = 0,
	WMT_OPID_PWR_ON = 1,
	WMT_OPID_PWR_OFF = 2,
	WMT_OPID_FUNC_ON = 3,
	WMT_OPID_FUNC_OFF = 4,
	WMT_OPID_REG_RW = 5,
	WMT_OPID_PWR_SV = 7,
	WMT_OPID_DSNS = 8,
	WMT_OPID_CMD_TEST = 10,
	WMT_OPID_HW_RST = 11,
	WMT_OPID_SW_RST = 12,
	WMT_OPID_STP_RST = 14,
	WMT_OPID_THERM_CTRL = 15,
	WMT_OPID_GPIO_CTRL = 17,
	WMT_OPID_GPIO_STATE = 19,
	WMT_OPID_BGW_DS = 20,
	WMT_OPID_IDC_MSG_HANDLING = 23,
	WMT_OPID_ASSERT = 30,
	WMT_OPID_MAX
};

struct wmt_op {
	enum wmt_opid op_id;
	unsigned long data[4];
	u32 info_bit;
	int result;
};

/* Downstream ENUM_WMT_CTRL_T (wmt_ctrl.h:47-76). */
enum wmt_ctrl_id {
	WMT_CTRL_HW_PWR_OFF = 0,
	WMT_CTRL_HW_PWR_ON = 1,
	WMT_CTRL_HW_RST = 2,
	WMT_CTRL_STP_CLOSE = 3,
	WMT_CTRL_STP_OPEN = 4,
	WMT_CTRL_STP_CONF = 5,
	WMT_CTRL_FREE_PATCH = 6,
	WMT_CTRL_GET_PATCH = 7,
	WMT_CTRL_GET_PATCH_NAME = 8,
	WMT_CTRL_HWIDVER_SET = 9,
	WMT_CTRL_STP_RST = 10,
	WMT_CTRL_GET_WMT_CONF = 11,
	WMT_CTRL_TX = 12,
	WMT_CTRL_RX = 13,
	WMT_CTRL_RX_FLUSH = 14,
	WMT_CTRL_GPS_SYNC_SET = 15,
	WMT_CTRL_GPS_LNA_SET = 16,
	WMT_CTRL_PATCH_SEARCH = 17,
	WMT_CTRL_HW_STATE_DUMP = 20,
	WMT_CTRL_GET_PATCH_NUM = 21,
	WMT_CTRL_GET_PATCH_INFO = 22,
	WMT_CTRL_SOC_PALDO_CTRL = 23,
	WMT_CTRL_SOC_WAKEUP_CONSYS = 24,
	WMT_CTRL_EVT_ERR_TRG_ASSERT = 27,
	WMT_CTRL_MAX
};

#define DWCNT_CTRL_DATA		16

struct wmt_ctrl_data {
	unsigned long ctrl_id;
	unsigned long data[DWCNT_CTRL_DATA];
};

/* Downstream WMT_STP_CONF_TYPE (wmt_core.h:329-334). */
enum wmt_stp_conf_type {
	WMT_STP_CONF_EN = 0,
	WMT_STP_CONF_RDY = 1,
	WMT_STP_CONF_MODE = 2,
};

enum wmt_hif_type {
	WMT_HIF_UART = 0,
	WMT_HIF_SDIO = 1,
	WMT_HIF_BTIF = 2,
	WMT_HIF_MAX
};

/* Downstream STP HIF selector values used by WMT_IOCTL_SET_STP_MODE
 * (wmt_dev.h:22-25).
 */
#define STP_UART_FULL	0x01
#define STP_UART_MAND	0x02
#define STP_BTIF_FULL	0x03
#define STP_SDIO	0x04

enum wmt_fm_mode {
	WMT_FM_INVALID = 0,
	WMT_FM_I2C = 1,
	WMT_FM_COMM = 2,
};

struct wmt_hif_conf {
	u32 hif_type;
	u32 hif_conf[DWCNT_HIF_CONF];
	u32 strap_conf[DWCNT_STRAP_CONF];
};

/* Parsed WMT_SOC.cfg (wmt_core.h:156-207).  The coexistence group is the
 * subset applied by the WMT coexistence engine (wmt_coex.c) and by the SOC
 * bring-up scripts (wmt_ic_soc.c:1686-1770).
 */
struct wmt_gen_conf {
	bool cfg_exist;
	u8 co_clock_flag;
	u8 wmt_gps_lna_pin;
	u8 wmt_gps_lna_enable;
	u8 bt_tssi_from_wifi;
	u16 bt_tssi_target;

	u8 coex_wmt_ant_mode;
	u8 coex_wmt_wifi_time_ctl;
	u8 coex_wmt_ext_pta_dev_on;
	u8 coex_wmt_filter_mode;
	bool coex_wmt_filter_mode_set;

	u8 coex_bt_rssi_upper_limit;
	u8 coex_bt_rssi_mid_limit;
	u8 coex_bt_rssi_lower_limit;
	u8 coex_bt_pwr_high;
	u8 coex_bt_pwr_mid;
	u8 coex_bt_pwr_low;

	u8 coex_wifi_rssi_upper_limit;
	u8 coex_wifi_rssi_mid_limit;
	u8 coex_wifi_rssi_lower_limit;
	u8 coex_wifi_pwr_high;
	u8 coex_wifi_pwr_mid;
	u8 coex_wifi_pwr_low;

	u8 coex_ext_pta_hi_tx_tag;
	u8 coex_ext_pta_hi_rx_tag;
	u8 coex_ext_pta_lo_tx_tag;
	u8 coex_ext_pta_lo_rx_tag;
	u16 coex_ext_pta_sample_t1;
	u16 coex_ext_pta_sample_t2;
	u8 coex_ext_pta_wifi_bt_con_trx;

	u32 coex_misc_ext_pta_on;
	u32 coex_misc_ext_feature_set;
};

/* Downstream WMT_PATCH (wmt_core.h:343-349), 28 bytes on the wire. */
struct wmt_patch {
	u8 date_time[16];
	u8 plat[4];
	u16 hw_ver;
	u16 sw_ver;
	u32 patch_ver;
} __packed;

struct wmt_patch_info {
	u32 download_seq;
	u8 address[4];
	u8 name[256];
};

enum wmt_chipinfo_type {
	WMTCHIN_CHIPID = 0,
	WMTCHIN_HWVER,
	WMTCHIN_MAPPINGHWVER,
	WMTCHIN_FWVER,
};

enum wmt_pin_id {
	PIN_BGF_EINT = 0,
	PIN_I2S_GRP = 1,
	PIN_GPS_SYNC = 2,
	PIN_GPS_LNA = 3,
	PIN_ID_MAX
};

enum wmt_pin_state {
	PIN_STA_INIT = 0,
	PIN_STA_OUT_L = 1,
	PIN_STA_OUT_H = 2,
	PIN_STA_IN_L = 3,
	PIN_STA_MUX = 4,
	PIN_STA_EINT_EN = 5,
	PIN_STA_EINT_DIS = 6,
	PIN_STA_DEINIT = 7,
};

enum wmt_paldo_type {
	BT_PALDO = 0,
	WIFI_PALDO = 1,
	FM_PALDO = 2,
	GPS_PALDO = 3,
	PMIC_CHIPID_PALDO = 4,
	WIFI_5G_PALDO = 5,
	PALDO_TYPE_MAX
};

enum wmt_paldo_op {
	PALDO_OFF = 0,
	PALDO_ON = 1,
};

enum wmt_func_state {
	FUNC_ON = 0,
	FUNC_OFF = 1,
	FUNC_RST = 2,
	FUNC_STAT = 3,
};

enum wmt_wl_op {
	WL_OP_GET = 0,
	WL_OP_PUT = 1,
};

/* Downstream ENUM_WMT_IC_PIN_ID / _STATE (wmt_core.h:242-268). */
enum wmt_ic_pin_id {
	WMT_IC_PIN_AUDIO = 0,
	WMT_IC_PIN_EEDI = 1,
	WMT_IC_PIN_EEDO = 2,
	WMT_IC_PIN_GSYNC = 3,
	WMT_IC_PIN_MAX
};

enum wmt_ic_pin_state {
	WMT_IC_PIN_EN = 0,
	WMT_IC_PIN_DIS = 1,
	WMT_IC_AIF_0 = 2,
	WMT_IC_AIF_1 = 3,
	WMT_IC_AIF_2 = 4,
	WMT_IC_AIF_3 = 5,
	WMT_IC_PIN_MUX = 6,
	WMT_IC_PIN_GPIO = 7,
	WMT_IC_PIN_GPIO_HIGH = 8,
	WMT_IC_PIN_GPIO_LOW = 9,
	WMT_IC_PIN_STATE_MAX
};

enum wmt_rst_src {
	WMTRSTSRC_RESET_BT = 0,
	WMTRSTSRC_RESET_FM = 1,
	WMTRSTSRC_RESET_GPS = 2,
	WMTRSTSRC_RESET_WIFI = 3,
	WMTRSTSRC_RESET_STP = 4,
	WMTRSTSRC_RESET_TEST = 5,
	WMTRSTSRC_RESET_MAX
};

enum wmt_rst_msg {
	WMTRSTMSG_RESET_START = 0,
	WMTRSTMSG_RESET_END = 1,
	WMTRSTMSG_RESET_END_FAIL = 2,
};

/* WMT_STAT bits (wmt_lib.h:112-124). */
#define WMT_STAT_PWR		BIT(0)
#define WMT_STAT_STP_OPEN	BIT(2)
#define WMT_STAT_STP_EN		BIT(3)
#define WMT_STAT_STP_RDY	BIT(4)
#define WMT_STAT_RX		BIT(5)
#define WMT_STAT_CMD		BIT(6)
#define WMT_STAT_RST_ON		BIT(7)

struct wmt_init_script {
	const u8 *cmd;
	u32 cmd_sz;
	const u8 *evt;
	u32 evt_sz;
	const char *str;
};

/* Global WMT driver state (single instance, as downstream gDevWmt). */
struct wmt_dev {
	struct mutex op_lock;		/* serialises the WMT command engine */
	unsigned long state;

	struct wmt_hif_conf hif;
	struct wmt_gen_conf gen_conf;

	u32 chip_id;
	u32 hw_ver;
	u32 fw_ver;
	enum mtk_wcn_wmt_hwver e_hw_ver;

	enum wmt_drv_status drv_status[WMTDRV_MAX];
	mtk_wcn_wmt_msgcb msgcb[WMTDRV_MAX];
	struct mtk_wcn_wmt_wlan_cb wlan_cb;
	bool wlan_probed;

	char patch_name[NAME_MAX + 1];	/* default patch, WMT_IOCTL_SET_PATCH_NAME */
	char cfg_name[NAME_MAX + 1];	/* WMT_SOC.cfg path, WMT_IOCTL_WMT_CFG_NAME */
	struct wmt_patch_info *patch_info;
	u32 patch_num;
	u8 *patch_buf;			/* loaded patch file + headroom */
	size_t patch_size;
	char patch_fw_name[NAME_MAX + 1];

	/* kernel -> launcher command channel (/dev/stpwmt read/poll/write) */
	char cmd[NAME_MAX + 1];
	struct mutex patch_lock;
	wait_queue_head_t cmd_wq;
	wait_queue_head_t rx_wq;
	bool rx_pending;

	bool wifi_resetting;
	bool ready;
	unsigned long func_on_bits;
};

extern struct wmt_dev g_wmt;

/* ------------------------------------------------------------------------- *
 * STP engine (stp_core.c)
 * ------------------------------------------------------------------------- */
int stp_drv_init(void);
void stp_drv_exit(void);
int mtk_wcn_stp_enable(int value);
int mtk_wcn_stp_ready(int value);
int mtk_wcn_stp_send_data_raw(const u8 *buffer, u32 length, u8 type);
void mtk_wcn_stp_flush_rx_queue(u32 type);
void mtk_wcn_stp_flush_context(void);
void mtk_wcn_stp_set_mode(u32 mode);
void mtk_wcn_stp_set_bluez(bool bluez);
void mtk_wcn_stp_set_if_tx_type(enum mtk_wcn_stp_tx_if type);
bool mtk_wcn_stp_is_btif_fullset_mode(void);
bool mtk_wcn_stp_is_btif_mand_mode(void);
int mtk_wcn_stp_set_wmt_last_close(u32 value);
u32 mtk_wcn_stp_get_wmt_evt_err_trg_assert(void);
void mtk_wcn_stp_set_wmt_evt_err_trg_assert(u32 value);

/* Transmit without re-entering the PSM filter (used when releasing data the
 * PSM engine held while the MCU was asleep).
 */
int mtk_wcn_stp_send_data_no_ps(const u8 *buffer, u32 length, u8 type);

/* ------------------------------------------------------------------------- *
 * STP <-> BTIF transport binding (stp_btif.c)
 * ------------------------------------------------------------------------- */
int stp_btif_open(void);
int stp_btif_close(void);
int stp_btif_rx_register(void);
int stp_btif_rx_unregister(void);
int stp_btif_wakeup(void);
int stp_btif_dpidle_ctrl(bool enable);

/* ------------------------------------------------------------------------- *
 * WMT core / control / function / IC (wmt_core.c, wmt_ctrl.c, wmt_func.c,
 * wmt_ic_soc.c)
 * ------------------------------------------------------------------------- */
int wmt_core_init(void);
void wmt_core_deinit(void);
int wmt_core_opid(struct wmt_op *op);
int wmt_core_ctrl(enum wmt_ctrl_id id, unsigned long *pa1, unsigned long *pa2);
int wmt_core_func_ctrl_cmd(enum mtk_wcn_wmt_drv type, bool enable);
int wmt_core_reg_rw_raw(u32 is_write, u32 offset, u32 *val, u32 mask);
int wmt_core_init_script(const struct wmt_init_script *script, int count);
int wmt_core_tx(const u8 *data, u32 size, u32 *written);
int wmt_core_rx(u8 *buf, u32 len, u32 *read_size);
bool wmt_core_patch_check(u32 patch_ver, u32 hw_ver);

/* WMT power-saving command sequence (downstream opfunc_pwr_sv) */
int wmt_core_psm_action(enum mtk_wcn_stp_psm_action action);
void wmt_core_evt_err_trg_assert(u32 reason);

/* Downstream opfunc_cmd_test command numbers (wmt_core.c:1565-1585). */
#define WMT_TEST_CMD_ASSERT	0
#define WMT_TEST_CMD_EXCEPTION	1
#define WMT_TEST_CMD_COEXDBG	2
#define WMT_TEST_CMD_NOACK	3
#define WMT_TEST_CMD_WARNRST	4
#define WMT_TEST_CMD_FWLOG	5

int wmt_core_cmd_test(u8 cmd, u8 param, u8 *out, u32 room, u32 *out_len);

/* WMT BGW desense payload (downstream opfunc_bgw_ds) */
int wmt_core_bgw_ds(const u8 *payload, u32 len);

/* WMT LTE-coex message (downstream opfunc_idc_msg_handling) */
int wmt_core_idc_tx(u8 opcode, const u8 *data, u32 len);

int wmt_ctrl(struct wmt_ctrl_data *data);
int wmt_ctrl_tx_ex(const u8 *data, u32 size, u32 *written, bool raw);

int wmt_ic_soc_sw_init(struct wmt_hif_conf *hif);
int wmt_ic_soc_sw_deinit(struct wmt_hif_conf *hif);
int wmt_ic_soc_ver_check(void);
int wmt_ic_soc_pin_ctrl(enum wmt_ic_pin_id id, enum wmt_ic_pin_state state,
			u32 flag);
int wmt_ic_soc_co_clock_ctrl(int on);
int wmt_ic_soc_patch_info_prepare(void);
int wmt_ic_soc_patch_download(u32 index);

/* Per-function power control (wmt_func.c). */
int wmt_func_on(int type);
int wmt_func_off(int type);

/* ------------------------------------------------------------------------- *
 * WMT platform glue (consys_mt6735.c)
 * ------------------------------------------------------------------------- */
int wmt_plat_init(void);
void wmt_plat_deinit(void);
int wmt_plat_pwr_ctrl(enum wmt_func_state state);
int wmt_plat_gpio_ctrl(enum wmt_pin_id id, enum wmt_pin_state state);
int wmt_plat_eirq_ctrl(bool enable);
int wmt_plat_soc_paldo_ctrl(enum wmt_paldo_type type, enum wmt_paldo_op op);
int wmt_plat_wake_lock_ctrl(enum wmt_wl_op op);
int wmt_plat_consys_driver_register(void);
void wmt_plat_consys_driver_unregister(void);

/* CONSYS firmware coredump window (downstream wmt_plat.h:169-230,
 * mtk_wcn_consys_hw.h:171-233).  Offsets are relative to the AP mapping of
 * the shared memory window, which starts at firmware address 0xf0080000.
 */
#define WMT_EMI_FW_PHY_BASE	0xf0080000
#define WMT_EMI_MEM_SIZE	(343 * 1024)
#define WMT_EMI_PAGED_DUMP_OFFSET 0x8400
#define WMT_EMI_FULL_DUMP_OFFSET  0x10400

enum wmt_emi_ctrl_offset {
	WMT_EMI_CTRL_STATE = 0x00,
	WMT_EMI_CTRL_HOST_SYNC_STATE = 0x04,
	WMT_EMI_CTRL_HOST_SYNC_NUM = 0x08,
	WMT_EMI_CTRL_CHIP_SYNC_STATE = 0x0c,
	WMT_EMI_CTRL_CHIP_SYNC_NUM = 0x10,
	WMT_EMI_CTRL_CHIP_SYNC_ADDR = 0x14,
	WMT_EMI_CTRL_CHIP_SYNC_LEN = 0x18,
	WMT_EMI_CTRL_CHIP_PRINT_BUFF_START = 0x1c,
	WMT_EMI_CTRL_CHIP_PRINT_BUFF_LEN = 0x20,
	WMT_EMI_CTRL_CHIP_PRINT_BUFF_IDX = 0x24,
	WMT_EMI_CTRL_CHIP_INT_STATUS = 0x28,
	WMT_EMI_CTRL_CHIP_PAGED_DUMP_END = 0x2c,
	WMT_EMI_CTRL_HOST_OUTBAND_ASSERT_W1 = 0x30,
	WMT_EMI_CTRL_CHIP_PAGE_DUMP_NUM = 0x44,
	WMT_EMI_CTRL_MAX,
};

/* Downstream ENUM_HOST_DUMP_STATE / ENUM_CHIP_DUMP_STATE (wmt_plat.h:162-197) */
enum wmt_host_dump_state {
	WMT_HOST_DUMP_NOT_START = 0,
	WMT_HOST_DUMP_GET = 1,
	WMT_HOST_DUMP_GET_DONE = 2,
	WMT_HOST_DUMP_END = 3,
};

enum wmt_chip_dump_state {
	WMT_CHIP_DUMP_NOT_START = 0,
	WMT_CHIP_DUMP_PUT = 1,
	WMT_CHIP_DUMP_PUT_DONE = 2,
	WMT_CHIP_DUMP_END = 3,
};

u32 wmt_plat_get_dump_info(u32 offset);
int wmt_plat_set_host_dump_state(enum wmt_host_dump_state state);
int wmt_plat_update_host_sync_num(void);
void __iomem *wmt_plat_emi_virt(u32 offset);

/* ------------------------------------------------------------------------- *
 * WMT library / state machine (wmt_lib.c)
 * ------------------------------------------------------------------------- */
int wmt_lib_init(void);
void wmt_lib_deinit(void);
int wmt_lib_run_op(struct wmt_op *op);
struct wmt_hif_conf *wmt_lib_hif(void);
struct wmt_gen_conf *wmt_lib_gen_conf(void);
u32 wmt_lib_get_icinfo(enum wmt_chipinfo_type type);
enum mtk_wcn_wmt_hwver wmt_lib_hwver(void);
int wmt_lib_set_hif(unsigned long cfg);
void wmt_lib_set_patch_name(const char *name);
u32 wmt_lib_co_clock(void);
void wmt_lib_set_chip_info(u32 chip_id, u32 hw_ver, u32 fw_ver,
			   enum mtk_wcn_wmt_hwver e_hw_ver);
void wmt_lib_notify_reset(enum wmt_rst_msg msg);
bool wmt_lib_cmb_rst(enum wmt_rst_src src);
void wmt_lib_schedule_reset(enum wmt_rst_src src);
/* Wakeup line fired while the MCU was asleep (wmt_lib_ps_irq_cb()). */
void wmt_lib_ps_irq_cb(void);

/* ------------------------------------------------------------------------- *
 * WMT char devices (wmt_dev.c, wmt_chrdev_wifi.c)
 * ------------------------------------------------------------------------- */
void wmt_dev_rx_event_cb(void);
void wmt_dev_rx_clear(void);
int wmt_dev_wait_rx(u32 timeout_ms);
int wmt_dev_patch_search(void);
u32 wmt_dev_patch_num(void);
int wmt_dev_patch_info_get(u32 seq, u8 *name, u8 addr[4]);
int wmt_dev_patch_set_num(u32 num);
int wmt_dev_patch_set_info(const struct wmt_patch_info *info);
int wmt_dev_read_file(const char *name, u8 **buf, size_t *size);
int wmt_dev_patch_get(const char *name, u8 **buf, size_t *size);
void wmt_dev_patch_put(void);
void wmt_dev_patch_info_free_all(void);
void wmt_dev_cmd_request(const char *cmd);
bool wmt_dev_cmd_pending(void);
const char *wmt_dev_cmd_string(void);
void wmt_dev_cmd_ack(int result);
int wmt_dev_stpwmt_register(void);
void wmt_dev_stpwmt_unregister(void);
int wmt_wifi_chrdev_register(void);
void wmt_wifi_chrdev_unregister(void);

/* ------------------------------------------------------------------------- *
 * WMT_SOC.cfg parser (wmt_conf.c)
 * ------------------------------------------------------------------------- */
int wmt_conf_read_file(void);
int wmt_conf_set_cfg_file(const char *name);
const struct wmt_gen_conf *wmt_conf_get(void);

/* ------------------------------------------------------------------------- *
 * Firmware coredump / assert (stp_btm.c) and its userspace ABI (stp_dbg.c)
 * ------------------------------------------------------------------------- */
int stp_btm_init(void);
void stp_btm_deinit(void);
/* Called by the STP parser when a firmware assert message arrives. */
void stp_btm_assert(const u8 *info, u32 len);
/* Called when STP is reset/disabled: abort and clear the assert state. */
void stp_btm_reset(void);
bool stp_btm_active(void);

/* Command id used for coredump records: downstream passed 2, which is
 * STP_DBG_COMMAND_RESET (btm_core.c:221).
 */
#define STP_DBG_NL_CMD_DUMP	2

int stp_dbg_nl_init(void);
void stp_dbg_nl_deinit(void);
/* Downstream stp_dbg_nl_send(): @cmd matches the STP_DBG generic-netlink
 * command id (2, STP_DBG_COMMAND_RESET, is what the dump path uses).
 */
int stp_dbg_nl_send(const u8 *data, u8 cmd, u32 len);

/* ------------------------------------------------------------------------- *
 * Coexistence (wmt_coex.c): debug commands, BGW desense, LTE IDC
 * ------------------------------------------------------------------------- */
int wmt_coex_init(void);
void wmt_coex_deinit(void);

/* True when a modem bridge is registered, i.e. the COEX channel must be
 * queued instead of dropped.
 */
bool wmt_idc_channel_enabled(void);
/* Data arrived on the COEX channel: parse and forward to the modem. */
void wmt_idc_channel_data(void);

/* BGW desense netlink daemon notifications (bgw_desense.c). */
int wmt_bgw_init(void);
void wmt_bgw_deinit(void);
void wmt_bgw_send_cmd(int command);

#endif /* _WMT_LOCAL_H_ */
