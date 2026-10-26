/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735/MT6737T CLDMA engine (MD1 data transport) - private defs.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/eccci/modem_cldma.h
 *   drivers/misc/mediatek/eccci/modem_cldma.c
 *   drivers/misc/mediatek/eccci/mt6735/cldma_reg.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * The public API (struct ccci_cldma_cfg and the ccci_cldma_*() entry points)
 * lives in include/linux/ccci.h and is shared with the CCCI core.
 */

#ifndef __CCCI_CLDMA_H
#define __CCCI_CLDMA_H

#include <linux/ccci.h>
#include <linux/bits.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

/*
 * Queue and ring geometry, modem_cldma.h:31-37.
 * 8 hardware queues per direction; queues 3/4/5 are the network queues,
 * 0/1/2/6/7 carry the control / logger traffic.
 */
#define CLDMA_TXQ_NUM			8
#define CLDMA_RXQ_NUM			8
#define CLDMA_NET_TXQ_NUM		3
#define CLDMA_NET_RXQ_NUM		3
#define CLDMA_NORMAL_TXQ_NUM		6
#define CLDMA_NORMAL_RXQ_NUM		6

/* One BD per skb head + per fragment, plus the EOL terminator, modem_cldma.h:37. */
#define CLDMA_MAX_BD_NUM		(MAX_SKB_FRAGS + 1)

/* modem_cldma.h:48: checksum offload is disabled on this platform. */
#define CLDMA_CHECKSUM_SIZE		0

/* modem_cldma.c:117: TX watchdog period for the VT queue, in seconds. */
#define CLDMA_ACTIVE_T			30

/* modem_cldma.c:121: queue 4 is the low priority (MD logger) queue. */
#define CLDMA_LOW_PRIORITY_QUEUE	4

/* modem_cldma.c:106: no queue is prioritised by hardware. */
#define CLDMA_HIGH_PRIORITY_QUEUE_MASK	0x00

/* modem_cldma.c:108: queues 3, 4 and 5. */
#define CLDMA_NET_QUEUE_MASK		0x38
/*
 * modem_cldma.c:113: queues that have a ring in the normal set. In exception
 * mode cldma_queue_switch_ring() (modem_cldma.c:1094-1133) only moves a
 * network queue to the normal 4K ring when it is also part of this mask, so
 * queue 3 moves and queues 4/5 keep their net ring (their
 * normal_*_queue2ring[] entry is -1 as well).
 */
#define CLDMA_NORMAL_QUEUE_MASK		0xCF

/* modem_cldma.c:113: queue 7 is the VT/UART logging queue watched by the timer. */
#define CLDMA_VT_QUEUE			7

/* All eight CLDMA queues, cldma_reg.h:258. */
#define CLDMA_BM_ALL_QUEUE		0xFF

/*
 * modem_cldma.c:113-114. The mask the exception-time stop hands to
 * CLDMA_AP_SO_STOP_CMD and clears from rxq_active. The downstream comment
 * ("queue 0,1,2,3 are non-stop") and the code at modem_cldma.c:1481-1487 are
 * unambiguous once the bit numbering of the status registers is taken into
 * account (queue q is bit q, see md_cd_stop_queue(), modem_cldma.c:1714-1718):
 * the queues in this mask (4..7) are the ones that get stopped, so RX queues
 * 0..3 keep running for the duration of the exception handshake.
 */
#define CLDMA_NONSTOP_QUEUE_MASK	0xF0
#define CLDMA_NONSTOP_QUEUE_MASK_32	0xF0F0F0F0

/*
 * RX queues the exception-time stop leaves armed: 0, 1, 2 and 3. These are the
 * queues the exception ("DHL") traffic is delivered on - the CCCI port table
 * gives the log control port .rxq_exp = 3 (ccci_core.c:1283-1285), ccci_fs
 * .rxq_exp = 1 (ccci_core.c:1289-1291) and ttyC1 .rxq_exp = 2
 * (ccci_core.c:1298-1300), while every network port has .rxq_exp = 0xFF
 * (ccci_core.c:1251-1273).
 */
#define CLDMA_EE_ALIVE_RXQ_MASK \
	(CLDMA_BM_ALL_QUEUE & ~CLDMA_NONSTOP_QUEUE_MASK)

/*
 * Positions of those queues in the L2 RX registers. CLDMA_BM_INT_DONE carries
 * queue q in bit q and CLDMA_BM_INT_ERROR carries it in bit 16 + q
 * (cldma_rx_done() and cldma_irq_thread() address a queue as BIT(index) in
 * both halves). Queue 0..3 therefore is 0x0000000F | 0x000F0000 = 0x000F000F,
 * the RX counterpart of downstream's 0xF0F0F0F0 for queues 4..7.
 */
#define CLDMA_EE_ALIVE_RXQ_INT \
	((CLDMA_BM_INT_DONE & CLDMA_EE_ALIVE_RXQ_MASK) | \
	 (CLDMA_BM_INT_ERROR & (CLDMA_EE_ALIVE_RXQ_MASK << 16)))

/*
 * modem_cldma.c:1466-1476,1493-1503: the exception-time stop loops dump the
 * engine state every 100000 polls and give up after 1600000 (downstream then
 * raises AEE from inside the loop; here the timeout is returned to the caller).
 */
#define CLDMA_EE_STOP_DUMP_INTERVAL	100000
#define CLDMA_EE_STOP_POLL_MAX		1600000

/*
 * INFRA_AO RGU reset registers for the CLDMA block, verbatim from
 * mt6735/cldma_reg.h:18-21:
 *   INFRA_RST0_REG 0x0030 - set CLDMA reset
 *   INFRA_RST1_REG 0x0034 - clear CLDMA reset
 *   CLDMA_AO_RST_MASK BIT(8), CLDMA_PD_RST_MASK BIT(26)
 * Downstream asserts both masks in RST0 and clears them in RST1,
 * mt6735/cldma_platform.c:55-71.
 */
#define CLDMA_INFRA_RST0_REG		0x0030
#define CLDMA_INFRA_RST1_REG		0x0034
#define CLDMA_AO_RST_MASK		BIT(8)
#define CLDMA_PD_RST_MASK		BIT(26)
#define CLDMA_RGU_RST_MASK		(CLDMA_AO_RST_MASK | CLDMA_PD_RST_MASK)

/* cldma_reg.h:239/242: L2 (per queue) interrupt groups. */
#define CLDMA_BM_INT_ALL		0xFFFFFFFF
#define CLDMA_BM_INT_ERROR		0x00FF0000
#define CLDMA_BM_INT_DONE		0x000000FF

/* GPD / BD flag bits, modem_cldma.c:1020, 1047, 1067, 1076, 1084. */
#define CLDMA_GPD_IOC			BIT(7)	/* interrupt on completion */
#define CLDMA_GPD_BDP			BIT(1)	/* GPD points to a BD chain */
#define CLDMA_HWO			BIT(0)	/* hardware owns the descriptor */
#define CLDMA_BD_EOL			BIT(0)	/* last BD of a chain */

#define CLDMA_RGPD_HWO_IOC		(CLDMA_GPD_IOC | CLDMA_HWO)	/* 0x81 */
#define CLDMA_TGPD_IOC			CLDMA_GPD_IOC			/* 0x80 */
#define CLDMA_TGPD_IOC_BDP		(CLDMA_GPD_IOC | CLDMA_GPD_BDP)	/* 0x82 */

/*
 * Queue sizes and packet sizes, modem_cldma.c:82-101.
 * Index is the hardware queue number; -1 marks a queue that does not use
 * the corresponding ring set.
 */
#define CLDMA_NET_RX_PKT_SIZE		1516	/* SKB_1_5K: 1500 + 16, ccci_config.h:94 */
#define CLDMA_NORMAL_RX_PKT_SIZE	3584	/* SKB_4K: (3584-128) + 128, ccci_config.h:93 */

/*
 * Register offsets. The CLDMA register file is split in four 4 KiB banks;
 * cfg.ap_base is AP_CLDMA_AO (0x1000A000) and cfg.ap_pdn_base is
 * AP_CLDMA_PDN (0x1021A000). All offsets below are taken verbatim from
 * mt6735/cldma_reg.h; the cited line is the definition in that file.
 */

/* AP_CLDMA_AO (0x1000A000): OUT DMA + RX interrupt mask, cldma_reg.h:42-75 */
#define CLDMA_AP_SO_CFG				0x0A04	/* cldma_reg.h:42 */
#define CLDMA_AP_SO_CHECKSUM_CHANNEL_ENABLE	0x0A74	/* cldma_reg.h:45 */
#define CLDMA_AP_SO_START_ADDR_0		0x0A78	/* cldma_reg.h:46 */
#define CLDMA_AP_SO_CURRENT_ADDR_0		0x0A98	/* cldma_reg.h:54 */
#define CLDMA_AP_SO_STATUS			0x0AB8	/* cldma_reg.h:62 */
#define CLDMA_AP_L2RIMR0			0x0C58	/* cldma_reg.h:65 */
#define CLDMA_AP_L2RIMR1			0x0C5C	/* cldma_reg.h:66 */
#define CLDMA_AP_L2RIMCR0			0x0C60	/* cldma_reg.h:67 */
#define CLDMA_AP_L2RIMCR1			0x0C64	/* cldma_reg.h:68 */
#define CLDMA_AP_L2RIMSR0			0x0C68	/* cldma_reg.h:69 */
#define CLDMA_AP_L2RIMSR1			0x0C6C	/* cldma_reg.h:70 */
#define CLDMA_AP_BUS_CFG			0x0C90	/* cldma_reg.h:71 */

/* AP_CLDMA_PDN (0x1021A000): IN DMA, OUT DMA commands + interrupts */
#define CLDMA_AP_UL_START_ADDR_0		0x0004	/* cldma_reg.h:80 */
#define CLDMA_AP_UL_CURRENT_ADDR_0		0x0028	/* cldma_reg.h:88 */
#define CLDMA_AP_UL_STATUS			0x0050	/* cldma_reg.h:96 */
#define CLDMA_AP_UL_START_CMD			0x0054	/* cldma_reg.h:97 */
#define CLDMA_AP_UL_RESUME_CMD			0x0058	/* cldma_reg.h:98 */
#define CLDMA_AP_UL_STOP_CMD			0x005C	/* cldma_reg.h:99 */
#define CLDMA_AP_UL_CFG				0x0074	/* cldma_reg.h:101 */
#define CLDMA_AP_HPQR				0x0118	/* cldma_reg.h:140 */
#define CLDMA_AP_TCR0				0x011C	/* cldma_reg.h:141 */
#define CLDMA_AP_UL_CHECKSUM_CHANNEL_ENABLE	0x0140	/* cldma_reg.h:150 */
#define CLDMA_AP_SO_START_CMD			0x03BC	/* cldma_reg.h:179 */
#define CLDMA_AP_SO_RESUME_CMD			0x03C0	/* cldma_reg.h:180 */
#define CLDMA_AP_SO_STOP_CMD			0x03C4	/* cldma_reg.h:181 */
#define CLDMA_AP_CLDMA_CODA_VERSION		0x0400	/* cldma_reg.h:183 */
#define CLDMA_AP_L2TISAR0			0x0410	/* cldma_reg.h:184 */
#define CLDMA_AP_L2TISAR1			0x0414	/* cldma_reg.h:185 */
#define CLDMA_AP_L2TIMR0			0x0418	/* cldma_reg.h:186 */
#define CLDMA_AP_L2TIMR1			0x041C	/* cldma_reg.h:187 */
#define CLDMA_AP_L2TIMCR0			0x0420	/* cldma_reg.h:188 */
#define CLDMA_AP_L2TIMCR1			0x0424	/* cldma_reg.h:189 */
#define CLDMA_AP_L2TIMSR0			0x0428	/* cldma_reg.h:190 */
#define CLDMA_AP_L2TIMSR1			0x042C	/* cldma_reg.h:191 */
#define CLDMA_AP_L3TISAR0			0x0430	/* cldma_reg.h:192 */
#define CLDMA_AP_L3TISAR1			0x0434	/* cldma_reg.h:193 */
#define CLDMA_AP_L3TIMR0			0x0438	/* cldma_reg.h:194 */
#define CLDMA_AP_L3TIMR1			0x043C	/* cldma_reg.h:195 */
#define CLDMA_AP_L3TIMCR0			0x0440	/* cldma_reg.h:196 */
#define CLDMA_AP_L3TIMCR1			0x0444	/* cldma_reg.h:197 */
#define CLDMA_AP_L3TIMSR0			0x0448	/* cldma_reg.h:198 */
#define CLDMA_AP_L3TIMSR1			0x044C	/* cldma_reg.h:199 */
#define CLDMA_AP_L2RISAR0			0x0450	/* cldma_reg.h:200 */
#define CLDMA_AP_L2RISAR1			0x0454	/* cldma_reg.h:201 */
#define CLDMA_AP_L3RISAR0			0x0470	/* cldma_reg.h:202 */
#define CLDMA_AP_L3RISAR1			0x0474	/* cldma_reg.h:203 */
#define CLDMA_AP_L3RIMR0			0x0478	/* cldma_reg.h:204 */
#define CLDMA_AP_L3RIMR1			0x047C	/* cldma_reg.h:205 */
#define CLDMA_AP_L3RIMCR0			0x0480	/* cldma_reg.h:206 */
#define CLDMA_AP_L3RIMCR1			0x0484	/* cldma_reg.h:207 */
#define CLDMA_AP_L3RIMSR0			0x0488	/* cldma_reg.h:208 */
#define CLDMA_AP_L3RIMSR1			0x048C	/* cldma_reg.h:209 */
#define CLDMA_AP_CLDMA_IP_BUSY			0x04B4	/* cldma_reg.h:216 */

/* Per queue start address helpers, cldma_reg.h:219-222. */
#define CLDMA_AP_TQSAR(i)	(CLDMA_AP_UL_START_ADDR_0 + 4 * (i))
#define CLDMA_AP_TQCPR(i)	(CLDMA_AP_UL_CURRENT_ADDR_0 + 4 * (i))
#define CLDMA_AP_RQSAR(i)	(CLDMA_AP_SO_START_ADDR_0 + 4 * (i))
#define CLDMA_AP_RQCPR(i)	(CLDMA_AP_SO_CURRENT_ADDR_0 + 4 * (i))

/*
 * Hardware descriptors, verbatim from modem_cldma.h:250-300. Every one is
 * 16 bytes; the DMA engine walks the 32-bit links.
 */
struct cldma_tgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	u16 debug_id;
	u32 next_gpd_ptr;
	u32 data_buff_bd_ptr;
	u16 data_buff_len;
	u8 desc_ext_len;
	u8 non_used;		/* debug: 1 = queued, 2 = collected */
} __packed;

struct cldma_rgpd {
	u8 gpd_flags;
	u8 gpd_checksum;
	u16 data_allow_len;
	u32 next_gpd_ptr;
	u32 data_buff_bd_ptr;
	u16 data_buff_len;
	u16 debug_id;
} __packed;

struct cldma_tbd {
	u8 bd_flags;
	u8 bd_checksum;
	u16 reserved;
	u32 next_bd_ptr;
	u32 data_buff_ptr;
	u16 data_buff_len;
	u8 desc_ext_len;
	u8 non_used;		/* debug: 1 = queued, 2 = collected */
} __packed;

struct cldma_rbd {
	u8 bd_flags;
	u8 bd_checksum;
	u16 data_allow_len;
	u32 next_bd_ptr;
	u32 data_buff_ptr;
	u16 data_buff_len;
	u16 reserved;
} __packed;

struct cldma_rspd {
	u8 spd_flags;
	u8 spd_checksum;
	u16 data_allow_len;
	u32 next_spd_ptr;
	u32 data_buff_ptr;
	u16 data_buff_len;
	u8 reserve_len;
	u8 spd_flags2;
} __packed;

enum cldma_ring_type {
	RING_GPD = 0,
	RING_GPD_BD = 1,
	RING_SPD = 2,
};

/*
 * One slot of a ring, modem_cldma.h:55-70. The descriptor itself is allocated
 * from a dma_pool; the payload is an skb that is bounced through dma_map_single.
 */
struct cldma_request {
	void *gpd;			/* CPU address of the GPD/BD */
	dma_addr_t gpd_addr;		/* DMA address of the GPD/BD */
	struct sk_buff *skb;
	dma_addr_t data_buffer_ptr_saved;
	struct list_head entry;		/* ring linkage */
	struct list_head bd;		/* BD chain (RING_GPD_BD only) */
};

struct cldma_queue;

/*
 * A descriptor ring, modem_cldma.h:78-92. The handlers are selected once at
 * creation time and never change.
 */
struct cldma_ring {
	struct list_head gpd_ring;
	int length;
	int pkt_size;
	bool tx;
	enum cldma_ring_type type;
	int (*handle_tx_request)(struct cldma_queue *queue,
				 struct cldma_request *req, struct sk_buff *skb);
	int (*handle_tx_done)(struct cldma_queue *queue);
	int (*handle_rx_done)(struct cldma_queue *queue);
	int (*handle_rx_refill)(struct cldma_queue *queue);
};

/*
 * One CLDMA queue (TX or RX), modem_cldma.h:128-183. Both directions use the
 * same layout; tx_xmit/rx_refill is the producer cursor, tr_done the consumer.
 */
struct cldma_queue {
	unsigned int index;
	bool tx;
	bool initialized;
	struct ccci_cldma *cd;
	struct cldma_ring *tr_ring;
	struct cldma_request *tr_done;
	struct cldma_request *tx_xmit;		/* TX producer */
	struct cldma_request *rx_refill;	/* RX producer */
	spinlock_t ring_lock;
	u16 debug_id;

	struct workqueue_struct *worker;
	struct work_struct rx_work;
	struct delayed_work tx_work;

	struct workqueue_struct *refill_worker;
	struct work_struct refill_work;

	/* Network queues hand skbs to the push thread instead of the callback. */
	struct sk_buff_head rx_list;
	wait_queue_head_t rx_wq;
	struct task_struct *rx_thread;
	bool rx_stop;

	struct timer_list timeout_timer;
};

/*
 * Engine instance. The device and register bases come from
 * struct ccci_cldma_cfg; everything else is owned by this module.
 */
struct ccci_cldma {
	struct device *dev;
	void __iomem *ap_base;		/* AP_CLDMA_AO */
	void __iomem *ap_pdn_base;	/* AP_CLDMA_PDN */
	/*
	 * Optional INFRA_AO window holding the RGU CLDMA reset registers
	 * INFRA_RST0/INFRA_RST1 (mt6735/cldma_reg.h:18-21). Set by the core
	 * through ccci_cldma_set_infra_ao(); NULL disables the RGU reset and
	 * keeps the normal-mode engine working.
	 */
	void __iomem *infra_ao;
	unsigned int irq;
	struct dma_pool *gpd_pool;

	struct cldma_queue txq[CLDMA_TXQ_NUM];
	struct cldma_queue rxq[CLDMA_RXQ_NUM];

	struct cldma_ring net_tx_ring[CLDMA_NET_TXQ_NUM];
	struct cldma_ring net_rx_ring[CLDMA_NET_RXQ_NUM];
	struct cldma_ring normal_tx_ring[CLDMA_NORMAL_TXQ_NUM];
	struct cldma_ring normal_rx_ring[CLDMA_NORMAL_RXQ_NUM];

	unsigned short txq_active;
	unsigned short rxq_active;
	/* Serialises the "queue is active" decisions against start/stop. */
	spinlock_t timeout_lock;

	void (*rx_cb)(void *ctx, unsigned int qno, struct sk_buff *skb);
	void *rx_ctx;

	/*
	 * Per-channel TX sequence counters (FEATURE_SEQ_CHECK_EN, enabled on
	 * mt6735 by mt6735/ccci_config.h:32). Downstream keeps them in
	 * struct ccci_modem but increments them from the CLDMA send path
	 * under the hardware queue lock (modem_cldma.c:2862).
	 */
	u16 tx_seq_num[CCCI_MAX_CH_NUM];

	/*
	 * Serialises the whole (re)arm / stop sequence, which touches the
	 * RGU, the start addresses and the queue cursors. Held across the
	 * MMIO sequences and never taken from atomic context; timeout_lock
	 * and the queue ring_locks nest inside it.
	 */
	struct mutex start_lock;
	/*
	 * Written under start_lock with WRITE_ONCE(), read with READ_ONCE()
	 * by ccci_cldma_set_exception_mode() so that it can never re-point a
	 * ring that ccci_cldma_start() has published but not yet armed.
	 */
	bool started;
	/*
	 * Non-network queues always use the normal rings; network queues use
	 * the net rings unless this is set and they have a normal ring. The
	 * core latches it through ccci_cldma_set_exception_mode() and the map
	 * is applied when the engine is armed, modem_cldma.c:1094-1133.
	 */
	bool exception_mode;
	bool irq_requested;
	atomic_t irq_enabled;
};

#endif /* __CCCI_CLDMA_H */
