/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCCI core private definitions: modem object, ports, CCIF transport,
 * boot loader and ring buffer.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/eccci/ccci_core.h
 *   drivers/misc/mediatek/eccci/ccci_config.h            (mt6735/ccci_config.h)
 *   drivers/misc/mediatek/eccci/mt6735/modem_reg_base.h
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.h
 *   drivers/misc/mediatek/eccci/port_cfg.c
 *   drivers/misc/mediatek/eccci/ccci_ringbuf.[ch]
 *   drivers/misc/mediatek/eccci/modem_cldma.c
 *   drivers/misc/mediatek/eccci/modem_ccif.c
 *   drivers/misc/mediatek/include/mt-plat/mt_ccci_common.h
 *
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#ifndef __CCCI_PRIV_H__
#define __CCCI_PRIV_H__

#include <linux/ccci.h>
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "ccci_exception.h"
#include "ccci_mpu.h"

#define CCCI_DEV_NAME		"ccci"
#define CCCI_MAGIC_NUM		0xFFFFFFFFu
#define CCCI_DBG_TAG		"ccci"

/* ------------------------------------------------------------------ */
/* Shared memory / ring / MTU constants                                */
/* (downstream mt6735/ccci_config.h)                                   */
/* ------------------------------------------------------------------ */
#define CCCI_MTU		3456		/* 3584 - 128 */
#define CCCI_NET_MTU		1500
#define CCCI_HEADER_SIZE	sizeof(struct ccci_header)

#define CCCI_SMEM_SIZE_EXCEPTION	0x10000
#define CCCI_SMEM_SIZE_RUNTIME_AP	0x800
#define CCCI_SMEM_SIZE_RUNTIME_MD	0x800
#define CCCI_SMEM_SIZE_RUNTIME		(CCCI_SMEM_SIZE_RUNTIME_AP + \
					 CCCI_SMEM_SIZE_RUNTIME_MD)
#define CCCI_SMEM_OFFSET_EXREC		2048
#define CCCI_SMEM_OFFSET_EPON		0xC64
#define CCCI_SMEM_OFFSET_SEQERR		0x34
#define CCCI_SMEM_OFFSET_CCCI_DEBUG	0
#define CCCI_SMEM_CCCI_DEBUG_SIZE	2048
#define CCCI_SMEM_OFFSET_MDSS_DEBUG	2048
#define CCCI_SMEM_MDSS_DEBUG_SIZE	2048
#define CCCI_SMEM_SLEEP_MODE_DBG_SIZE	1024

#define CCCI_DRIVER_VER		0x20110118
#define AP_PLATFORM_INFO	"MT6735E1"
#define AP_MD_HS_V2		2

/* Ring buffer placed in the modem shared memory (downstream ccci_ringbuf.h) */
#define CCCI_RINGBUF_CTL_LEN	40	/* 8 + 24 + 8 */

#define CCCI_MAX_QUEUES		8

/* ------------------------------------------------------------------ */
/* CCIF register map (downstream mt6735/ccif_c2k_platform.h)           */
/* ------------------------------------------------------------------ */
#define APCCIF_CON		0x00
#define APCCIF_BUSY		0x04
#define APCCIF_START		0x08
#define APCCIF_TCHNUM		0x0C
#define APCCIF_RCHNUM		0x10
#define APCCIF_ACK		0x14
#define APCCIF_CHDATA		0x100
#define CCIF_SRAM_SIZE		512

/* MD watchdog / reset generation unit (downstream ccif_c2k_platform.h) */
#define WDT_MD_MODE		0x00
#define WDT_MD_LENGTH		0x04
#define WDT_MD_RESTART		0x08
#define WDT_MD_STA		0x0C
#define WDT_MD_SWRST		0x1C
#define WDT_MD_MODE_KEY		0x0000220E

/* MD boot slave registers (downstream modem_reg_base.h) */
#define MD_BOOT_VECTOR_VALUE		0x00000000
#define MD_BOOT_VECTOR_KEY_VALUE	0x3567C766
#define MD_BOOT_VECTOR_EN_VALUE		0xA3B66175

/* downstream mt_ccci_common.h HIF_EX_STAGE */
#define HIF_EX_INIT		0
#define HIF_EX_ACK		1
#define HIF_EX_INIT_DONE	2
#define HIF_EX_CLEARQ_DONE	3
#define HIF_EX_CLEARQ_ACK	4
#define HIF_EX_ALLQ_RESET	5

/* CCIF logical channel numbers (downstream modem_ccif.c / modem_cldma.c) */
#define RINGQ_EXP_BASE		0
#define RINGQ_SRAM		7
#define RINGQ_BASE		8

#define H2D_EXCEPTION_ACK		(RINGQ_EXP_BASE + 1)
#define H2D_EXCEPTION_CLEARQ_ACK	(RINGQ_EXP_BASE + 2)
#define H2D_FORCE_MD_ASSERT		(RINGQ_EXP_BASE + 3)
#define H2D_SRAM			(RINGQ_SRAM)
#define H2D_RINGQ0			(RINGQ_BASE)

#define D2H_EXCEPTION_INIT		(RINGQ_EXP_BASE + 1)
#define D2H_EXCEPTION_INIT_DONE		(RINGQ_EXP_BASE + 2)
#define D2H_EXCEPTION_CLEARQ_DONE	(RINGQ_EXP_BASE + 3)
#define D2H_EXCEPTION_ALLQ_RESET	(RINGQ_EXP_BASE + 4)
#define AP_MD_SEQ_ERROR			(RINGQ_EXP_BASE + 6)
#define D2H_SRAM			(RINGQ_SRAM)
#define D2H_RINGQ0			(RINGQ_BASE)

#define CCCI_CCIF_CH_NUM		16
#define CCCI_CCIF_IRQ_TIMEOUT_US	1000	/* one CCIF handshake slot */
#define CCCI_CCIF_SEND_RETRIES		100

/* Shared memory reserved in front of the CCIF rings (EE dump) */
#define CCIF_MD_SMEM_RESERVE	0x200000

/* ------------------------------------------------------------------ */
/* Firmware / boot info (downstream ccci_util_lib_main.h)              */
/* ------------------------------------------------------------------ */
#define CCCI_MD_HEADER_MAGIC	"CHECK_HEADER"
#define CCCI_IMG_ERR_STR_LEN	64
#define CCCI_IMG_POSTFIX_LEN	16
#define CCCI_IMG_PATH_LEN	64

#define CCCI_NORMAL_BOOT_ID	0
#define CCCI_META_BOOT_ID	1
#define CCCI_FACTORY_BOOT_ID	2

/* boot stages, downstream ccci_core.h MD_BOOT_STAGE */
#define CCCI_BOOT_STAGE_0	0
#define CCCI_BOOT_STAGE_1	1
#define CCCI_BOOT_STAGE_2	2

/* MD exception stages, downstream ccci_core.h MD_EX_STAGE */
#define CCCI_EX_NONE		0
#define CCCI_EX_INIT		1
#define CCCI_EX_DHL_DL_RDY	2
#define CCCI_EX_INIT_DONE	3
#define CCCI_EX_NO_RESPONSE	4
#define CCCI_EX_WDT		5

/* ------------------------------------------------------------------ */
/* Messages (downstream mt_ccci_common.h)                              */
/* ------------------------------------------------------------------ */
enum ccci_md_msg {
	/* AP -> MD control/monitor messages */
	CCCI_MD_MSG_BOOT_READY		= 0xFAF50001,
	CCCI_MD_MSG_BOOT_UP		= 0xFAF50002,
	CCCI_MD_MSG_EXCEPTION		= 0xFAF50003,
	CCCI_MD_MSG_RESET		= 0xFAF50004,
	CCCI_MD_MSG_RESET_RETRY		= 0xFAF50005,
	CCCI_MD_MSG_READY_TO_RESET	= 0xFAF50006,
	CCCI_MD_MSG_BOOT_TIMEOUT	= 0xFAF50007,
	CCCI_MD_MSG_STOP_MD_REQUEST	= 0xFAF50008,
	CCCI_MD_MSG_START_MD_REQUEST	= 0xFAF50009,
	CCCI_MD_MSG_ENTER_FLIGHT_MODE	= 0xFAF5000A,
	CCCI_MD_MSG_LEAVE_FLIGHT_MODE	= 0xFAF5000B,
	CCCI_MD_MSG_POWER_ON_REQUEST	= 0xFAF5000C,
	CCCI_MD_MSG_POWER_OFF_REQUEST	= 0xFAF5000D,
	CCCI_MD_MSG_SEND_BATTERY_INFO	= 0xFAF5000E,
	CCCI_MD_MSG_NOTIFY		= 0xFAF5000F,
	CCCI_MD_MSG_STORE_NVRAM_MD_TYPE	= 0xFAF50010,
	CCCI_MD_MSG_CFG_UPDATE		= 0xFAF50011,
};

/* control / system channel protocol message ids */
#define MD_INIT_START_BOOT	0x0
#define MD_NORMAL_BOOT		0x0
#define MD_EX			0x4
#define CCCI_DRV_VER_ERROR	0x5
#define MD_EX_REC_OK		0x6
#define MD_EX_RESUME		0x7
#define MD_EX_PASS		0x8
#define MD_INIT_CHK_ID		0x5555FFFF
#define MD_EX_CHK_ID		0x45584350
#define MD_EX_REC_OK_CHK_ID	0x45524543

#define MD_DORMANT_NOTIFY	0x100
#define MD_SLP_REQUEST		0x101
#define MD_TX_POWER		0x102
#define MD_RF_TEMPERATURE	0x103
#define MD_RF_TEMPERATURE_3G	0x104
#define MD_GET_BATTERY_INFO	0x105
#define MD_SIM_TYPE		0x107
#define MD_ICUSB_NOTIFY		0x108
#define MD_LOW_BATTERY_LEVEL	0x10A
#define MD_PAUSE_LTE		0x10D
#define MD_THROTTLING		0x112
#define MD_THERMAL_LOW_POWER	0x122
#define MD_WDT_MONITOR		0x1000
#define MD_WAKEN_UP		0x10000

#define C2K_STATUS_IND_MSG	0x201
#define C2K_STATUS_QUERY_MSG	0x202
#define C2K_HB_MSG		0x207

/* kernel service ids, downstream mt_ccci_common.h ID_* */
#define ID_GET_MD_WAKEUP_SRC	0
#define ID_GET_TXPOWER		6
#define ID_PAUSE_LTE		9
#define ID_STORE_SIM_SWITCH_MODE 10
#define ID_GET_SIM_SWITCH_MODE	11
#define ID_GET_MD_STATE		12
#define ID_THROTTLING_CFG	13
#define ID_RESET_MD		14
#define ID_DUMP_MD_REG		15
#define ID_DUMP_MD_SLEEP_MODE	16
#define ID_UPDATE_MD_BOOT_MODE	20

/* ------------------------------------------------------------------ */
/* ioctl numbers, copied verbatim from downstream mt_ccci_common.h     */
/* ------------------------------------------------------------------ */
#define CCCI_IOC_MAGIC 'C'
#define CCCI_IOC_MD_RESET		_IO(CCCI_IOC_MAGIC, 0)
#define CCCI_IOC_GET_MD_STATE		_IOR(CCCI_IOC_MAGIC, 1, unsigned int)
#define CCCI_IOC_PCM_BASE_ADDR		_IOR(CCCI_IOC_MAGIC, 2, unsigned int)
#define CCCI_IOC_PCM_LEN		_IOR(CCCI_IOC_MAGIC, 3, unsigned int)
#define CCCI_IOC_FORCE_MD_ASSERT	_IO(CCCI_IOC_MAGIC, 4)
#define CCCI_IOC_ALLOC_MD_LOG_MEM	_IO(CCCI_IOC_MAGIC, 5)
#define CCCI_IOC_DO_MD_RST		_IO(CCCI_IOC_MAGIC, 6)
#define CCCI_IOC_SEND_RUN_TIME_DATA	_IO(CCCI_IOC_MAGIC, 7)
#define CCCI_IOC_GET_MD_INFO		_IOR(CCCI_IOC_MAGIC, 8, unsigned int)
#define CCCI_IOC_GET_MD_EX_TYPE		_IOR(CCCI_IOC_MAGIC, 9, unsigned int)
#define CCCI_IOC_SEND_STOP_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 10)
#define CCCI_IOC_SEND_START_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 11)
#define CCCI_IOC_DO_STOP_MD		_IO(CCCI_IOC_MAGIC, 12)
#define CCCI_IOC_DO_START_MD		_IO(CCCI_IOC_MAGIC, 13)
#define CCCI_IOC_ENTER_DEEP_FLIGHT	_IO(CCCI_IOC_MAGIC, 14)
#define CCCI_IOC_LEAVE_DEEP_FLIGHT	_IO(CCCI_IOC_MAGIC, 15)
#define CCCI_IOC_POWER_ON_MD		_IO(CCCI_IOC_MAGIC, 16)
#define CCCI_IOC_POWER_OFF_MD		_IO(CCCI_IOC_MAGIC, 17)
#define CCCI_IOC_POWER_ON_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 18)
#define CCCI_IOC_POWER_OFF_MD_REQUEST	_IO(CCCI_IOC_MAGIC, 19)
#define CCCI_IOC_SEND_BATTERY_INFO	_IO(CCCI_IOC_MAGIC, 21)
#define CCCI_IOC_GET_MD_MEM_SIZE	_IOR(CCCI_IOC_MAGIC, 37, unsigned int)
#define CCCI_IOC_GET_MD_PROTOCOL_TYPE	_IOR(CCCI_IOC_MAGIC, 42, char[16])
#define CCCI_IOC_GET_MD_BOOT_MODE	_IOR(CCCI_IOC_MAGIC, 59, unsigned int)
#define CCCI_IOC_SET_MD_BOOT_MODE	_IOW(CCCI_IOC_MAGIC, 58, unsigned int)
#define CCCI_IOC_LOG_LVL			_IOW(CCCI_IOC_MAGIC, 64, unsigned int)

/* ------------------------------------------------------------------ */
/* Error numbers, downstream mt_ccci_common.h CCCI_ERR_*_REGION        */
/* ------------------------------------------------------------------ */
#define CCCI_ERR_MD_IN_RESET		102
#define CCCI_ERR_INVALID_PARAM		106
#define CCCI_ERR_MD_NOT_READY		304
#define CCCI_ERR_ALLOCATE_MEMORY_FAIL	305
#define CCCI_ERR_DROP_PACKET		317
#define CCCI_ERR_PORT_RX_FULL		318
#define CCCI_ERR_INVALID_QUEUE_INDEX	321
#define CCCI_ERR_HIF_NOT_POWER_ON	322
#define CCCI_ERR_CHANNEL_NUM_MIS_MATCH	311
#define CCCI_ERR_FUNC_ID_ERROR		321
#define CCCI_ERR_LOAD_IMG_NOT_FOUND	413

/* internal port flag bits, downstream ccci_core.h PORT_F_* */
#define PORT_F_ALLOW_DROP	BIT(0)
#define PORT_F_RX_FULLED	BIT(1)
#define PORT_F_USER_HEADER	BIT(2)
#define PORT_F_RX_EXCLUSIVE	BIT(3)

#define CCCI_PORT_RX_LEN_TH	32
#define CCCI_MAX_PORT_MINOR	128
#define CCCI_IPC_MINOR_BASE	100

/* transport independent pseudo events delivered to ports */
#define CCCI_EV_RX_IRQ		0x100
#define CCCI_EV_TX_IRQ		0x101
#define CCCI_EV_TX_FULL		0x102
#define CCCI_EV_BOOT_FAIL	0x103

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */
struct ccci_md;
struct ccci_port;
struct ccci_ccif;
struct ccci_ringbuf;
struct ccci_cldma;

struct ccci_port_ops {
	int (*init)(struct ccci_port *port);
	/* returns 0 if the skb was consumed, -CCCI_ERR_PORT_RX_FULL to retry */
	int (*recv_skb)(struct ccci_port *port, struct sk_buff *skb);
	void (*md_state_notice)(struct ccci_port *port, int event);
	void (*dump_info)(struct ccci_port *port, unsigned int flag);
};

struct ccci_port {
	/* identity, order matches downstream port_cfg.c tables */
	unsigned int tx_ch;
	unsigned int rx_ch;
	u8 txq;
	u8 rxq;
	u8 txq_exp;
	u8 rxq_exp;
	u8 flags;
	const struct ccci_port_ops *ops;
	unsigned int minor;
	const char *name;

	struct ccci_md *md;
	void *priv;
	atomic_t open_count;

	struct list_head node;		/* md->port_list */
	struct list_head rx_node;	/* md->rx_ch_ports[rx_ch] */

	struct sk_buff_head rx_q;
	struct sk_buff *rx_current;	/* partially read packet */
	bool rx_current_user_header;
	unsigned int rx_len;
	unsigned int rx_len_th;
	bool rx_full;
	wait_queue_head_t rx_wq;
	spinlock_t rx_lock;

	struct cdev cdev;
	bool cdev_added;
	struct device *dev;

	u64 rx_pkts;
	u64 tx_pkts;
	u64 rx_drops;
	u64 tx_busy_cnt;
};

/* ccif sram layout mirrored from downstream modem_ccif.h: the first
 * 16 bytes of the SRAM window carry the uplink ccci_header.
 */
struct ccci_ccif_sram {
	struct ccci_header up_header;
	u8 runtime[CCIF_SRAM_SIZE - sizeof(struct ccci_header)];
};

struct ccci_ccif_queue {
	struct ccci_md *md;
	unsigned int index;
	struct ccci_ringbuf *rx_ring;
	struct ccci_ringbuf *tx_ring;
	spinlock_t tx_lock;
	struct tasklet_struct rx_task;
	struct work_struct rx_retry_work;
	atomic_t rx_running;
	unsigned int retry;
	unsigned int drop_count;
};

struct ccci_ccif {
	struct ccci_md *md;
	struct device *dev;
	void __iomem *ap_base;
	void __iomem *md_base;
	void __iomem *rgu_base;
	void __iomem *boot_vector;
	void __iomem *boot_key;
	void __iomem *boot_en;
	unsigned int irq;
	unsigned int wdt_irq;
	unsigned int sram_size;
	bool ring_mode;
	bool started;
	bool irq_enabled;
	spinlock_t lock;
	unsigned long channels;		/* pending channel bitmap */
	struct tasklet_struct irq_task;
	struct work_struct exc_work;
	struct work_struct wdt_work;
	struct ccci_ccif_queue q[CCCI_MAX_QUEUES];
	struct ccci_ccif_sram *sram;
	u32 total_smem_size;
};

/* boot / firmware context */
struct ccci_boot {
	struct ccci_md *md;
	struct timer_list timer;
	bool timer_armed;
	const struct firmware *fw;
	bool fw_valid;
	struct clk *clk;
	char err_str[CCCI_IMG_ERR_STR_LEN];
	char post_fix[CCCI_IMG_POSTFIX_LEN];
	char file_name[CCCI_IMG_PATH_LEN];
	u32 hdr_verno;
	u32 md_img_size;
	u32 loaded_size;	/* image bytes copied into md->img_vir */
	/* DSP sub-image slot from check header v3+, ccci_util_lib_load_img.c:238 */
	u32 dsp_offset;
	u32 dsp_size;
	u32 mem_size;
	u32 image_type;
	u32 product_ver;
	u8 bind_sys_id;
	char md_platform[17];
	char build_ver[65];
	char build_time[65];
};

struct ccci_md {
	unsigned int id;
	char name[16];
	char fw_name[64];
	struct device *dev;
	struct platform_device *pdev;
	const struct ccci_md_hw_ops *hw_ops;

	enum ccci_md_state state;
	unsigned int boot_stage;
	unsigned int ex_stage;
	unsigned int ex_type;
	u32 capability;
	unsigned int irat_md_id;
	unsigned int runtime_version;
	u32 sbp_code;
	u32 sbp_code_default;
	u32 boot_count;
	u32 md_boot_mode;
	int wakeup_src;
	unsigned char critical_user[4];
	u32 sim_type;
	int sim_switch_mode;
	u32 heartbeat;
	int dtr_state;

	spinlock_t lock;
	struct mutex op_lock;
	struct list_head list;

	struct ccci_port *ports;
	unsigned int port_count;
	struct list_head ports_list;
	struct list_head rx_ch_ports[CCCI_MAX_CH_NUM];
	struct sk_buff_head rx_backlog;
	struct work_struct rx_backlog_work;
	s16 seq_out[CCCI_MAX_CH_NUM];
	s16 seq_in[CCCI_MAX_CH_NUM];

	/* memory regions */
	void __iomem *smem_vir;
	phys_addr_t smem_phy;
	size_t smem_size;
	bool smem_coherent;
	void __iomem *img_vir;
	phys_addr_t img_phy;
	size_t img_size;
	bool img_present;

	/* transports */
	struct ccci_ccif *ccif;
	struct ccci_cldma *cldma;
	bool ring_mode;

	/* ccmni binding */
	struct ccmni_ccci_ops *ccmni_ops;
	bool ccmni_inited;

	/* character device accounting */
	dev_t devt;
	unsigned int major;

	/* boot */
	struct ccci_boot boot;
	bool powered;

	/* debug */
	struct dentry *dbg_dir;
	char boot_info[192];
	char version[16];
	bool registered;

	/*
	 * INFRA_AO register window (MT6735 0x10001000). Used by the MPU/RMPU
	 * remap programming (MD1/MD3 BANKn_MAPx, offset 0x300) and by the
	 * CLDMA hardware reset (INFRA_RST0/1, offset 0x030). Resolved from
	 * the modem DT node's "mediatek,infracfg-ao" phandle.
	 */
	void __iomem *infra_ao;
	phys_addr_t infra_ao_phys;
	size_t infra_ao_size;

	/*
	 * Memory protection state, filled in by ccci_mpu.c:
	 * smem_offset_ap_to_md is non-zero only after the RMPU windows were
	 * programmed (downstream mem_layout.smem_offset_AP_to_MD), dsp_phy /
	 * dsp_size are the DSP sub-image slot taken from the parsed modem
	 * image header (downstream mem_layout.dsp_region_*).
	 */
	u32 smem_offset_ap_to_md;
	phys_addr_t dsp_phy;
	size_t dsp_size;
	bool mpu_smc;		/* EL3/ATF SMC conduit available */

	/* exception/AEE dump engine state (ccci_exception.c) */
	struct ccci_exception ex;
};

/* ------------------------------------------------------------------ */
/* ccif sram runtime data structures (downstream mt_ccci_common.h)     */
/* ------------------------------------------------------------------ */
struct ccci_runtime_feature {
	u8 feature_id;
	struct {
		u8 support_mask:4;
		u8 version:4;
	} support_info;
	u8 reserved[2];
	u32 data_len;
	u32 data[];
} __packed;

struct ccci_runtime_share_memory {
	u32 addr;
	u32 size;
};

struct ccci_misc_info_element {
	u32 feature[4];
};

struct ccci_runtime_boot_info {
	u32 boot_channel;
	u32 booting_start_id;
	u32 boot_attributes;
	u32 boot_ready_id;
};

#define CCCI_FEATURE_COUNT		64
#define CCCI_MD_FEATURE_QUERY_PATTERN	0x49434343	/* "CCCI" */
#define CCCI_AP_FEATURE_QUERY_PATTERN	0x43434349	/* "ICCC" */

struct md_query_ap_feature {
	u32 head_pattern;
	struct {
		u8 support_mask:4;
		u8 version:4;
	} feature_set[CCCI_FEATURE_COUNT];
	u32 tail_pattern;
} __packed;

struct ap_query_md_feature {
	u32 head_pattern;
	struct {
		u8 support_mask:4;
		u8 version:4;
	} feature_set[CCCI_FEATURE_COUNT];
	u32 share_memory_support;
	u32 ap_runtime_data_addr;
	u32 ap_runtime_data_size;
	u32 md_runtime_data_addr;
	u32 md_runtime_data_size;
	u32 set_md_mpu_start_addr;
	u32 set_md_mpu_total_size;
	u32 tail_pattern;
} __packed;

/* runtime feature ids, downstream ccci_core.h MD_CCCI_RUNTIME_FEATURE_ID */
enum ccci_runtime_feature_id {
	CCCI_RT_BOOT_INFO = 0,
	CCCI_RT_EXCEPTION_SHARE_MEMORY,
	CCCI_RT_CCIF_SHARE_MEMORY,
	CCCI_RT_DHL_SHARE_MEMORY,
	CCCI_RT_MD1MD3_SHARE_MEMORY,
	CCCI_RT_MISC_INFO_HIF_DMA_REMAP,
	CCCI_RT_MISC_INFO_RTC_32K_LESS,
	CCCI_RT_MISC_INFO_RANDOM_SEED_NUM,
	CCCI_RT_MISC_INFO_GPS_COCLOCK,
	CCCI_RT_MISC_INFO_SBP_ID,
	CCCI_RT_MISC_INFO_CCCI,
	CCCI_RT_MISC_INFO_CLIB_TIME,
	CCCI_RT_MISC_INFO_C2K,
	CCCI_RT_MAX,
};

/*
 * Legacy modem_runtime MISC feature indexes, downstream
 * mt-plat/mt_ccci_common.h:734-744 (MISC_FEATURE_ID).  These are NOT the
 * MD_CCCI_RUNTIME_FEATURE_ID values above: the legacy block addresses its
 * slots as feature_<id>_val, i.e. "MISC_MD_SBP_SETTING" is feature_4_val[],
 * while the handshake-v2 block indexes feature_set[] by
 * MD_CCCI_RUNTIME_FEATURE_ID.
 */
enum ccci_misc_feature_id {
	CCCI_MISC_DMA_ADDR = 0,
	CCCI_MISC_32K_LESS,
	CCCI_MISC_RAND_SEED,
	CCCI_MISC_MD_COCLK_SETTING,
	CCCI_MISC_MD_SBP_SETTING,
	CCCI_MISC_MD_SEQ_CHECK,
	CCCI_MISC_MD_CLIB_TIME,
	CCCI_MISC_MD_C2K_ON,
	CCCI_MISC_MD_SLPHISTLOG,
};

/* downstream CCCI_FEATURE_* / MISC_FEATURE_STATE */
#define CCCI_FEATURE_NOT_EXIST		0
#define CCCI_FEATURE_NOT_SUPPORT	1
#define CCCI_FEATURE_MUST_SUPPORT	2
#define CCCI_FEATURE_OPTIONAL_SUPPORT	3
#define CCCI_FEATURE_SUPPORT_BACKWARD_COMPAT 4
/* downstream MISC_FEATURE_STATE: FEATURE_SUPPORT == 2, same numeric value as
 * CCCI_FEATURE_MUST_SUPPORT above, used in the legacy SRAM support mask
 */
#define CCCI_FEATURE_SUPPORT	2

/* legacy runtime data block, downstream mt_ccci_common.h modem_runtime */
#define CCCI_RUNTIME_FEATURE_VALUES 16
struct modem_runtime {
	u32 Prefix;
	u32 Platform_L;
	u32 Platform_H;
	u32 DriverVersion;
	u32 BootChannel;
	u32 BootingStartID;
	u32 BootAttributes;
	u32 BootReadyID;
	u32 FileShareMemBase;
	u32 FileShareMemSize;
	u32 ExceShareMemBase;
	u32 ExceShareMemSize;
	u32 CCIFShareMemBase;
	u32 CCIFShareMemSize;
	u32 TotalShareMemBase;
	u32 TotalShareMemSize;
	u32 CheckSum;
	u32 Postfix;
	u32 misc_prefix;
	u32 support_mask;
	u32 index;
	u32 next;
	u32 feature_val[CCCI_RUNTIME_FEATURE_VALUES][4];
	u32 reserved_2[3];
	u32 misc_postfix;
} __packed;

/* ------------------------------------------------------------------ */
/* Ring buffer wire format (downstream ccci_ringbuf.c)                 */
/* ------------------------------------------------------------------ */
#define CCCI_RBF_HEADER		0xEE0000EE
#define CCCI_RBF_FOOTER		0xFF0000FF
#define CCIF_PKG_HEADER		0xAABBAABB
#define CCIF_PKG_FOOTER		0xCCDDEEFF
#define CCIF_PKG_HDR_LEN	8
#define CCIF_PKG_FTR_LEN	8

/* ------------------------------------------------------------------ */
/* core (ccci_core.c)                                                  */
/* ------------------------------------------------------------------ */
extern struct class *ccci_class;
extern struct list_head ccci_md_list;

struct ccci_md *ccci_md_get(unsigned int md_id);
struct ccci_port *ccci_md_get_port_by_channel(struct ccci_md *md, unsigned int ch);
struct ccci_port *ccci_md_get_port_by_minor(struct ccci_md *md, unsigned int minor);
struct ccci_port *ccci_get_port_for_node(dev_t devt);

int ccci_md_set_state(struct ccci_md *md, enum ccci_md_state state);
void ccci_md_notify_ports(struct ccci_md *md, int event);
int ccci_rx_dispatch(struct ccci_md *md, struct sk_buff *skb);
int ccci_rx_dispatch_qno(struct ccci_md *md, struct sk_buff *skb,
			 unsigned int qno);
void ccci_md_rx_cb(void *ctx, unsigned int qno, struct sk_buff *skb);
int ccci_md_send_skb(struct ccci_md *md, unsigned int qno, struct sk_buff *skb);
int ccci_md_write_room(struct ccci_md *md, unsigned int qno);
int ccci_send_msg_to_md(struct ccci_md *md, unsigned int ch, u32 msg, u32 resv,
			bool blocking);
int ccci_send_virtual_md_msg(struct ccci_md *md, unsigned int ch, u32 msg, u32 resv);
void ccci_port_ask_more(struct ccci_port *port);
int ccci_port_recv_skb(struct ccci_port *port, struct sk_buff *skb);
struct sk_buff *ccci_alloc_skb(unsigned int len, gfp_t gfp);
void ccci_inc_tx_seq(struct ccci_md *md, struct ccci_header *h);
void ccci_chk_rx_seq(struct ccci_md *md, struct ccci_header *h, unsigned int qno);
void ccci_reset_seq(struct ccci_md *md);

int ccci_md_start(struct ccci_md *md);
int ccci_md_stop(struct ccci_md *md, unsigned int timeout);
int ccci_md_reset(struct ccci_md *md);
int ccci_md_force_assert(struct ccci_md *md, unsigned int ccif_only);

/* port tables (port_cfg.c) */
int ccci_md_port_cfg(struct ccci_md *md);

/* ------------------------------------------------------------------ */
/* CCIF transport (ccci_ccif.c)                                        */
/* ------------------------------------------------------------------ */
struct ccci_ccif *ccci_ccif_create(struct ccci_md *md, struct device *dev,
				   void __iomem *ap_base, void __iomem *md_base,
				   void __iomem *rgu_base, phys_addr_t boot_vector,
				   phys_addr_t boot_key, phys_addr_t boot_en,
				   unsigned int irq, unsigned int wdt_irq,
				   unsigned int sram_size, bool ring_mode);
void ccci_ccif_destroy(struct ccci_ccif *ccif);
int ccci_ccif_start(struct ccci_ccif *ccif);
void ccci_ccif_stop(struct ccci_ccif *ccif);
int ccci_ccif_send(struct ccci_md *md, unsigned int channel);
void ccif_enable_irq(struct ccci_md *md);
void ccif_disable_irq(struct ccci_md *md);
void ccif_wdt_irq(struct ccci_md *md, bool enable);
void ccci_ccif_ring_sram_send(struct ccci_md *md);
int ccci_ccif_ring_send(struct ccci_md *md, unsigned int qno, struct sk_buff *skb);
int ccci_ccif_ring_write_room(struct ccci_md *md, unsigned int qno);
int ccci_ccif_ring_rx_poll(struct ccci_md *md, unsigned int qno, int budget);
void ccci_ccif_reset_rings(struct ccci_md *md);
void ccci_ccif_dump_regs(struct ccci_md *md, struct seq_file *s);
size_t ccci_ccif_ring_total_size(void);
void ccci_ccif_exception(struct ccci_md *md, unsigned int stage);

/* ------------------------------------------------------------------ */
/* boot (ccci_boot.c)                                                  */
/* ------------------------------------------------------------------ */
int ccci_boot_init(struct ccci_md *md);
void ccci_boot_exit(struct ccci_md *md);
int ccci_boot_load_firmware(struct ccci_md *md);
int ccci_boot_power_on(struct ccci_md *md);
int ccci_boot_let_md_go(struct ccci_md *md);
void ccci_boot_power_off(struct ccci_md *md);
int ccci_boot_prepare_runtime_data(struct ccci_md *md, struct sk_buff *skb);
int ccci_boot_send_runtime_data(struct ccci_md *md, u32 sbp_code);
void ccci_boot_arm_timer(struct ccci_md *md);
void ccci_boot_disarm_timer(struct ccci_md *md);
int ccci_boot_smem_setup(struct ccci_md *md, struct device *dev);

/* ------------------------------------------------------------------ */
/* ports                                                               */
/* ------------------------------------------------------------------ */
extern const struct ccci_port_ops ccci_char_port_ops;
extern const struct ccci_port_ops ccci_net_port_ops;
extern const struct ccci_port_ops ccci_kernel_port_ops;

int ccci_port_char_init(struct ccci_md *md);
void ccci_port_char_exit(struct ccci_md *md);

int ccci_port_net_init(struct ccci_md *md);
void ccci_port_net_exit(struct ccci_md *md);
int ccci_get_ccmni_channel(int md_id, int ccmni_idx, struct ccmni_ch *channel);
int ccmni_send_pkt_passthrough(int md_id, int tx_ch, void *data);

void ccci_port_kernel_exit(struct ccci_md *md);

/* kernel service API, ROM exec_ccci_kern_func_by_md_id() and friends */
int exec_ccci_kern_func_by_md_id(int md_id, unsigned int id, char *buf,
				 unsigned int len);
int register_ccci_sys_call_back(int md_id, unsigned int id,
				int (*func)(int md_id, int data));
unsigned long ccci_get_md_boot_count(int md_id);

/* ------------------------------------------------------------------ */
/* ring buffer (ccci_ringbuf.c)                                        */
/* ------------------------------------------------------------------ */
struct ccci_ringbuf *ccci_ringbuf_create(void __iomem *mem, unsigned int mem_size,
					 unsigned int rx_size, unsigned int tx_size);
int ccci_ringbuf_readable(struct ccci_ringbuf *rb);
int ccci_ringbuf_writeable(struct ccci_ringbuf *rb, unsigned int size);
int ccci_ringbuf_read(struct ccci_ringbuf *rb, void *dst, unsigned int len);
int ccci_ringbuf_write(struct ccci_ringbuf *rb, const void *src, unsigned int len);
void ccci_ringbuf_move_rpointer(struct ccci_ringbuf *rb, unsigned int len);
void ccci_ringbuf_reset(struct ccci_ringbuf *rb, bool tx);

/* ------------------------------------------------------------------ */
/* sysfs / debugfs (ccci_sysfs.c)                                      */
/* ------------------------------------------------------------------ */
int ccci_sysfs_md_init(struct ccci_md *md);
void ccci_sysfs_md_exit(struct ccci_md *md);
int ccci_sysfs_init(void);
void ccci_sysfs_exit(void);

/* platform driver entry points (ccci_platform.c), called by ccci_core.c */
int ccci_platform_register(void);
void ccci_platform_unregister(void);

/* optional hook exported by drivers/misc/mediatek/ccci_util */
void ccci_util_dump_set_window(unsigned int md_id, phys_addr_t phys,
			       size_t size);

/* internal registration path: the platform driver passes its device as the
 * parent of the modem device, the public ccci_register_modem() passes NULL.
 */
int ccci_md_register(const struct ccci_md_cfg *cfg, struct device *parent);
int ccci_md_attach_ccif(struct ccci_md *md, struct ccci_ccif *ccif);
int ccci_md_attach_cldma(struct ccci_md *md, struct ccci_cldma *cldma);

/*
 * Logging: the kernel log gets the device prefix, and the ccci_util log ring
 * (mdlog) receives the same record, per the cross-leaf contract.
 */
#define ccci_md_err(md, fmt, ...) do {					\
	dev_err((md)->dev, "[md%u] " fmt, (md)->id + 1, ##__VA_ARGS__);	\
	ccci_util_log((md)->id, fmt, ##__VA_ARGS__);			\
} while (0)
#define ccci_md_warn(md, fmt, ...) do {					\
	dev_warn((md)->dev, "[md%u] " fmt, (md)->id + 1, ##__VA_ARGS__);	\
	ccci_util_log((md)->id, fmt, ##__VA_ARGS__);			\
} while (0)
#define ccci_md_info(md, fmt, ...) do {					\
	dev_info((md)->dev, "[md%u] " fmt, (md)->id + 1, ##__VA_ARGS__);	\
	ccci_util_log((md)->id, fmt, ##__VA_ARGS__);			\
} while (0)
#define ccci_md_dbg(md, fmt, ...)					\
	dev_dbg((md)->dev, "[md%u] " fmt, (md)->id + 1, ##__VA_ARGS__)

static inline unsigned int ccci_port_txq(struct ccci_port *p)
{
	return p->md->state == CCCI_MD_EXCEPTION ? p->txq_exp : p->txq;
}

static inline unsigned int ccci_port_rxq(struct ccci_port *p)
{
	return p->md->state == CCCI_MD_EXCEPTION ? p->rxq_exp : p->rxq;
}

/* ------------------------------------------------------------------ */
/* Gap-closure additions (append-only, CCCI gap leaves)                */
/* ------------------------------------------------------------------ */

/* downstream mt6735/ccci_config.h:70: EE dump size of the smem window */
#define CCCI_SMEM_DUMP_SIZE		4096

/* downstream mt6735/ccci_config.h:76-77: 16 byte CCIF SRAM header
 * snapshot, CCCI_SMEM_OFFSET_EXREC + 1024 - 16 == 3056
 */
#define CCCI_SMEM_CCIF_SRAM_SIZE	16
#define CCCI_SMEM_OFFSET_CCIF_SRAM	(CCCI_SMEM_OFFSET_EXREC + 1024 - \
					 CCCI_SMEM_CCIF_SRAM_SIZE)

/* downstream modem_cldma.c:1862: SW watchdog marker in smem+0xC64 */
#define CCCI_SMEM_EPON_SW_WDT		0xBAEBAE10

/* downstream eccci/ccci_core.h:640 */
#define CCCI_MD_IMG_DUMP_SIZE		(1 << 8)

/* downstream eccci/ccci_core.h:774-777 MD_BOOT_MODE_* */
#define CCCI_MD_BOOT_MODE_META		2

/* downstream mt_ccci_common.h:334: CCCI_ERR_CCCI_REGION_START_ID + 16 */
#define CCCI_ERR_MD_INDEX_NOT_FOUND	316

/* downstream mt_ccci_common.h:240-241 (both are _IOW there as well) */
#define CCCI_IOC_SET_MD_SBP_CFG		_IOW(CCCI_IOC_MAGIC, 40, unsigned int)
#define CCCI_IOC_GET_MD_SBP_CFG		_IOW(CCCI_IOC_MAGIC, 41, unsigned int)

#endif /* __CCCI_PRIV_H__ */
