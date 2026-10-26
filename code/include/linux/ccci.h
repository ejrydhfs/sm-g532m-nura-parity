/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCCI (Cross Chip Communication Interface) public kernel API.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/include/mt-plat/mt_ccci_common.h
 *   drivers/misc/mediatek/eccci/ccci_core.h
 *   drivers/misc/mediatek/ccmni/ccmni.h
 *
 * This file is the frozen contract between the CCCI core, the CLDMA engine,
 * the CCMNI netdev driver, the CCCI util driver and the C2K/dual-modem driver.
 * Do not change existing declarations; append only if unavoidable.
 */
#ifndef __LINUX_CCCI_H
#define __LINUX_CCCI_H

#include <linux/types.h>
#include <linux/io.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/device.h>

/* Modem ids. Downstream md_id is 0-based (MD_SYS1 == 0). */
enum ccci_md_id {
	CCCI_MD_SYS1 = 0,
	CCCI_MD_SYS2,
	CCCI_MD_SYS3,
	CCCI_MD_SYS4,
	CCCI_MD_SYS5,
	CCCI_MAX_MD_NUM,
};

/* Modem states, mirror downstream MD_STATE (eccci/ccci_core.h). */
enum ccci_md_state {
	CCCI_MD_INVALID = 0,
	CCCI_MD_BOOTING,
	CCCI_MD_READY,
	CCCI_MD_EXCEPTION,
	CCCI_MD_STOPPED,
};

/* Logical channel ids, verbatim from downstream mt_ccci_common.h. */
enum ccci_ch {
	CCCI_CONTROL_RX = 0,
	CCCI_CONTROL_TX = 1,
	CCCI_SYSTEM_RX = 2,
	CCCI_SYSTEM_TX = 3,
	CCCI_PCM_RX = 4,
	CCCI_PCM_TX = 5,
	CCCI_UART1_RX = 6,
	CCCI_UART1_RX_ACK = 7,
	CCCI_UART1_TX = 8,
	CCCI_UART1_TX_ACK = 9,
	CCCI_UART2_RX = 10,
	CCCI_UART2_RX_ACK = 11,
	CCCI_UART2_TX = 12,
	CCCI_UART2_TX_ACK = 13,
	CCCI_FS_RX = 14,
	CCCI_FS_TX = 15,
	CCCI_PMIC_RX = 16,
	CCCI_PMIC_TX = 17,
	CCCI_UEM_RX = 18,
	CCCI_UEM_TX = 19,
	CCCI_CCMNI1_RX = 20,
	CCCI_CCMNI1_RX_ACK = 21,
	CCCI_CCMNI1_TX = 22,
	CCCI_CCMNI1_TX_ACK = 23,
	CCCI_CCMNI2_RX = 24,
	CCCI_CCMNI2_RX_ACK = 25,
	CCCI_CCMNI2_TX = 26,
	CCCI_CCMNI2_TX_ACK = 27,
	CCCI_CCMNI3_RX = 28,
	CCCI_CCMNI3_RX_ACK = 29,
	CCCI_CCMNI3_TX = 30,
	CCCI_CCMNI3_TX_ACK = 31,
	CCCI_RPC_RX = 32,
	CCCI_RPC_TX = 33,
	CCCI_IPC_RX = 34,
	CCCI_IPC_RX_ACK = 35,
	CCCI_IPC_TX = 36,
	CCCI_IPC_TX_ACK = 37,
	CCCI_IPC_UART_RX = 38,
	CCCI_IPC_UART_RX_ACK = 39,
	CCCI_IPC_UART_TX = 40,
	CCCI_IPC_UART_TX_ACK = 41,
	CCCI_MD_LOG_RX = 42,
	CCCI_MD_LOG_TX = 43,
	CCCI_IT_RX = 50,
	CCCI_IT_TX = 51,
	CCCI_IMSV_UL = 52,
	CCCI_IMSV_DL = 53,
	CCCI_IMSC_UL = 54,
	CCCI_IMSC_DL = 55,
	CCCI_IMSA_UL = 56,
	CCCI_IMSA_DL = 57,
	CCCI_IMSDC_UL = 58,
	CCCI_IMSDC_DL = 59,
	CCCI_ICUSB_RX = 60,
	CCCI_ICUSB_TX = 61,
	CCCI_LB_IT_RX = 62,
	CCCI_LB_IT_TX = 63,
	CCCI_CCMNI1_DL_ACK = 64,
	CCCI_CCMNI2_DL_ACK = 65,
	CCCI_CCMNI3_DL_ACK = 66,
	CCCI_STATUS_RX = 67,
	CCCI_STATUS_TX = 68,
	CCCI_CCMNI4_RX = 69,
	CCCI_CCMNI4_RX_ACK = 70,
	CCCI_CCMNI4_TX = 71,
	CCCI_CCMNI4_TX_ACK = 72,
	CCCI_CCMNI4_DLACK_RX = 73,
	CCCI_CCMNI5_RX = 74,
	CCCI_CCMNI5_RX_ACK = 75,
	CCCI_CCMNI5_TX = 76,
	CCCI_CCMNI5_TX_ACK = 77,
	CCCI_CCMNI5_DLACK_RX = 78,
	CCCI_CCMNI6_RX = 79,
	CCCI_CCMNI6_RX_ACK = 80,
	CCCI_CCMNI6_TX = 81,
	CCCI_CCMNI6_TX_ACK = 82,
	CCCI_CCMNI6_DLACK_RX = 83,
	CCCI_CCMNI7_RX = 84,
	CCCI_CCMNI7_RX_ACK = 85,
	CCCI_CCMNI7_TX = 86,
	CCCI_CCMNI7_TX_ACK = 87,
	CCCI_CCMNI7_DLACK_RX = 88,
	CCCI_CCMNI8_RX = 89,
	CCCI_CCMNI8_RX_ACK = 90,
	CCCI_CCMNI8_TX = 91,
	CCCI_CCMNI8_TX_ACK = 92,
	CCCI_CCMNI8_DLACK_RX = 93,
	CCCI_MDL_MONITOR_DL = 94,
	CCCI_MDL_MONITOR_UL = 95,
	CCCI_RIL_IPC0_RX = 148,
	CCCI_RIL_IPC0_TX = 149,
	CCCI_RIL_IPC1_RX = 150,
	CCCI_RIL_IPC1_TX = 151,
	CCCI_VT_CTL_RX = 152,
	CCCI_VT_CTL_TX = 153,
	CCCI_C2K_PPP_DATA,
	CCCI_C2K_AT,
	CCCI_C2K_AT2,
	CCCI_C2K_AT3,
	CCCI_C2K_LB_DL,
	CCCI_MONITOR_CH,
	CCCI_DUMMY_CH,
	CCCI_MAX_CH_NUM,
	CCCI_MONITOR_CH_ID = 0xf0000000,
	CCCI_FORCE_ASSERT_CH = 20090215,
	CCCI_INVALID_CH_ID = 0xffffffff,
};

/* C2K channel ids, verbatim from downstream mt_ccci_common.h. */
enum c2k_channel {
	CTRL_CH_C2K = 0,
	AUDIO_CH_C2K = 1,
	DATA_PPP_CH_C2K = 2,
	MDLOG_CTRL_CH_C2K = 3,
	FS_CH_C2K = 4,
	AT_CH_C2K = 5,
	AGPS_CH_C2K = 6,
	AT2_CH_C2K = 7,
	AT3_CH_C2K = 8,
	MDLOG_CH_C2K = 9,
	STATUS_CH_C2K = 11,
	NET1_CH_C2K = 12,
	NET2_CH_C2K = 13,
	NET3_CH_C2K = 14,
	NET4_CH_C2K = 15,
	NET5_CH_C2K = 16,
	C2K_MAX_CH_NUM,
};

/* Downstream struct ccci_header (16 bytes, little endian). */
struct ccci_header {
	__le32 data[2];
	u16 channel;
	u16 seq_num:15;
	u16 assert_bit:1;
	__le32 reserved;
} __packed;

/* Modem capabilities, verbatim downstream MODEM_CAP_*. */
#define CCCI_MD_CAP_NAPI		(1 << 0)
#define CCCI_MD_CAP_TXBUSY_STOP		(1 << 1)
#define CCCI_MD_CAP_SGIO		(1 << 2)
#define CCCI_MD_CAP_CCMNI_DISABLE	(1 << 16)
#define CCCI_MD_CAP_DATA_ACK_DVD	(1 << 17)
#define CCCI_MD_CAP_CCMNI_SEQNO		(1 << 18)
#define CCCI_MD_CAP_CCMNI_IRAT		(1 << 19)
#define CCCI_MD_CAP_WORLD_PHONE		(1 << 20)
#define CCCI_MD_CAP_CCMNI_MQ		(1 << 21)

/* ------------------------------------------------------------------ */
/* CCMNI <-> CCCI core binding (implemented by ccmni, used by core)    */
/* ------------------------------------------------------------------ */
struct ccmni_ch {
	int rx;
	int rx_ack;
	int tx;
	int tx_ack;
	int dl_ack;
	int multiq;
};

#define CCMNI_DRV_V0	0
#define CCMNI_DRV_V1	1
#define CCMNI_DRV_V2	2

struct ccmni_ccci_ops {
	int ccmni_ver;
	int ccmni_num;
	unsigned char name[16];
	unsigned int md_ability;
	unsigned int irat_md_id;
	unsigned int napi_poll_weigh;
	int (*send_pkt)(int md_id, int tx_ch, void *data);
	int (*napi_poll)(int md_id, int rx_ch, struct napi_struct *napi, int weight);
	int (*get_ccmni_ch)(int md_id, int ccmni_idx, struct ccmni_ch *channel);
};

int ccmni_init(int md_id, struct ccmni_ccci_ops *ccci_info);
void ccmni_exit(int md_id);
int ccmni_rx_callback(int md_id, int ccmni_idx, struct sk_buff *skb);
void ccmni_md_state_callback(int md_id, int ccmni_idx, enum ccci_md_state state);

/* ------------------------------------------------------------------ */
/* Modem instantiation (implemented by the core, used by platform/C2K) */
/* ------------------------------------------------------------------ */
struct ccci_md_cfg {
	unsigned int md_id;			/* enum ccci_md_id */
	const char *name;			/* "md1", "md3" */
	const char *firmware_name;		/* request_firmware() name */
	phys_addr_t ccif_phys;			/* AP CCIF register block */
	resource_size_t ccif_size;
	unsigned int ccif_irq;
	phys_addr_t md_boot_vector_phys;	/* 0 if unused */
	phys_addr_t md_boot_vector_key_phys;
	phys_addr_t md_boot_vector_en_phys;
	phys_addr_t rgu_phys;
	unsigned int sram_size;			/* CCIF SRAM bytes */
	unsigned int capability;		/* CCCI_MD_CAP_* */
	unsigned int irat_md_id;		/* CCCI_MAX_MD_NUM if none */
};

int ccci_register_modem(const struct ccci_md_cfg *cfg);
void ccci_unregister_modem(unsigned int md_id);
int ccci_md_get_state(unsigned int md_id);
int ccci_md_set_irat(unsigned int md_id, unsigned int irat_md_id);

/* ------------------------------------------------------------------ */
/* CLDMA engine (implemented by ccci_cldma, used by the core)          */
/* ------------------------------------------------------------------ */
#define CCCI_CLDMA_Q_BASIC	0
#define CCCI_CLDMA_Q_NET	1

struct ccci_cldma;
struct ccci_cldma_cfg {
	struct device *dev;
	void __iomem *ap_base;		/* AP_CLDMA_AO */
	void __iomem *ap_pdn_base;	/* AP_CLDMA_PDN */
	unsigned int irq;
	phys_addr_t smem_phys;
	size_t smem_size;
};

struct ccci_cldma *ccci_cldma_create(const struct ccci_cldma_cfg *cfg);
void ccci_cldma_destroy(struct ccci_cldma *cd);
int ccci_cldma_start(struct ccci_cldma *cd);
void ccci_cldma_stop(struct ccci_cldma *cd);
int ccci_cldma_send(struct ccci_cldma *cd, unsigned int qno, struct sk_buff *skb);
void ccci_cldma_set_rx_cb(struct ccci_cldma *cd,
			  void (*cb)(void *ctx, unsigned int qno, struct sk_buff *skb),
			  void *ctx);

/* ------------------------------------------------------------------ */
/* CCCI util (implemented by ccci_util, used by core)                  */
/* ------------------------------------------------------------------ */
int ccci_util_init(void);
void ccci_util_exit(void);
int ccci_util_log_init(unsigned int md_id, struct device *dev);
void ccci_util_log_exit(unsigned int md_id);
void ccci_util_log(unsigned int md_id, const char *fmt, ...) __printf(2, 3);
void ccci_util_dump_mem(unsigned int md_id, const void *buf, size_t len);

/* ------------------------------------------------------------------ */
/* Optional per-MD hardware sequence callbacks (appended per the CCCI  */
/* contract addendum; used by MD3/C2K, MD1 registers none)             */
/* ------------------------------------------------------------------ */
struct ccci_md_hw_ops {
	int  (*power_on)(unsigned int md_id);   /* after fw load, before any MD register access */
	int  (*let_md_go)(unsigned int md_id);  /* after boot-vector/key/enable write */
	int  (*power_off)(unsigned int md_id);  /* modem stop */
	void (*reset)(unsigned int md_id);      /* exception recovery */
};
int ccci_register_md_hw_ops(unsigned int md_id, const struct ccci_md_hw_ops *ops);
void ccci_unregister_md_hw_ops(unsigned int md_id);


/* ------------------------------------------------------------------ */
/* Gap-closure API                                                     */
/*                                                                     */
/* Coordinator-owned scaffold (frozen): the declarations below are     */
/* implemented by the CCCI gap-closure leaves. They are appended per   */
/* CCCI contract section 5 and do not modify any earlier declaration. */
/* ------------------------------------------------------------------ */

/*
 * Modem MPU/RMPU: program the DSP region protection, called from the
 * kernel RPC service IPC_RPC_DSP_EMI_MPU_SETTING. Returns 0 when the
 * platform has no DSP region (nothing to protect).
 */
int ccci_md_set_dsp_mpu_protection(unsigned int md_id);

/*
 * ROM aee_dump_ccci_debug_info(): hand the CCCI debug record inside the
 * modem shared memory to an exception-dump consumer. MD ids are 1-based
 * here because that is the AEE convention (ROM ccci_core.c:580).
 */
int aee_dump_ccci_debug_info(int md_id, void **addr, int *size);

/*
 * CLDMA hardware reset and exception-mode ring switch. The INFRA_AO
 * window comes from the modem device tree node ("mediatek,infracfg-ao").
 */
void ccci_cldma_set_infra_ao(struct ccci_cldma *cd, void __iomem *infra_ao);
int ccci_cldma_hw_reset(struct ccci_cldma *cd);
int ccci_cldma_set_exception_mode(struct ccci_cldma *cd, bool on);

/* DHL exception handshake: stop queues and hold RX alive for the dump. */
int ccci_cldma_stop_for_ee(struct ccci_cldma *cd);
int ccci_cldma_dhl_rx_alive(struct ccci_cldma *cd, bool alive);

/*
 * Kernel RPC service registration. A handler fills the response argument
 * vector and returns 0, or returns a negative errno. The RPC engine owns
 * the wire format (downstream struct rpc_buffer/rpc_pkt).
 */
#define CCCI_RPC_MAX_ARGS	6
struct ccci_rpc_arg {
	void *buf;
	unsigned int len;
};

struct ccci_rpc_msg {
	u32 op_id;
	u32 arg_count;
	struct ccci_rpc_arg args[CCCI_RPC_MAX_ARGS];
};

typedef int (*ccci_rpc_handler_t)(int md_id, const struct ccci_rpc_msg *req,
				  struct ccci_rpc_msg *rsp);

int ccci_rpc_register_service(u32 op_id, const char *name,
			      ccci_rpc_handler_t handler);
int ccci_rpc_unregister_service(u32 op_id);

#endif /* __LINUX_CCCI_H */
