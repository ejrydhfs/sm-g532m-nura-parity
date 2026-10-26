/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek BTIF internal interface.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/btif/common/inc/mtk_btif.h
 *   drivers/misc/mediatek/btif/common/plat_inc/btif_pub.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef __BTIF_CORE_H__
#define __BTIF_CORE_H__

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "mtk_btif.h"

/* Downstream mtk_btif.h:42 (BTIF_USER_NAME_MAX_LEN) */
#define BTIF_USER_NAME_MAX_LEN	32
/* Downstream mtk_btif.h:48 (BTIF_RX_BUFFER_SIZE) */
#define BTIF_RX_BUFFER_SIZE	(64 * 1024)
/* Downstream plat_common.h:199 (BTIF_HAL_TX_FIFO_SIZE) */
#define BTIF_TX_FIFO_SIZE	(4 * 1024)
/* Downstream mtk_btif.h:173 (BTIF_LOG_SZ) / :170 (BTIF_LOG_ENTRY_NUM) */
#define BTIF_LOG_SZ		1536
#define BTIF_LOG_ENTRY_NUM	30
/* Downstream btif_priv.h:97-101 */
#define BTIF_TX_FIFO_DEPTH	16
#define BTIF_RX_FIFO_DEPTH	8
#define BTIF_TX_FIFO_THRE	(BTIF_TX_FIFO_DEPTH / 2)
#define BTIF_RX_FIFO_THRE	0x1
/* Downstream btif_dma_priv.h:30-34 */
#define BTIF_TX_DMA_VFIFO_SIZE	(8 * 1024)
#define BTIF_RX_DMA_VFIFO_SIZE	(8 * 1024)
#define BTIF_DMA_TX_THRE(n)	((n) - 7)
#define BTIF_DMA_RX_THRE(n)	((n) * 3 / 4)

/* Downstream btif_pub.h:21-25 (ENUM_BTIF_MODE) */
enum btif_mode {
	BTIF_MODE_PIO = 0,
	BTIF_MODE_DMA,
};

/* Downstream mtk_btif.h:70-75 (ENUM_BTIF_STATE) */
enum btif_state {
	B_S_OFF = 0,
	B_S_SUSPEND,
	B_S_DPIDLE,
	B_S_ON,
};

enum btif_dma_dir {
	BTIF_DMA_RX = 0,
	BTIF_DMA_TX,
};

struct mtk_btif;

struct btif_vfifo {
	void *vir;
	dma_addr_t phys;
	unsigned int size;
	unsigned int thre;
	unsigned int wpt;
	unsigned int last_wpt_wrap;
	unsigned int rpt;
	unsigned int last_rpt_wrap;
};

struct btif_dma_chan {
	struct mtk_btif *btif;
	void __iomem *base;
	int irq;
	bool irq_registered;
	enum btif_dma_dir dir;
	struct btif_vfifo vfifo;
	unsigned int flush_irq_count;
};

struct btif_log_entry {
	ktime_t ts;
	unsigned int len;
	u8 buf[BTIF_LOG_SZ];
};

struct btif_log {
	const char *name;
	bool enabled;
	bool realtime;
	unsigned int in;
	unsigned int count;
	spinlock_t lock;
	struct btif_log_entry *entries;
};

struct btif_user {
	char name[BTIF_USER_NAME_MAX_LEN];
	unsigned long id;
	struct mtk_btif *btif;
};

struct mtk_btif {
	struct device *dev;
	void __iomem *base;		/* BTIF controller */
	int irq;
	bool irq_registered;

	struct clk *clk_btif;
	struct clk *clk_apdma;

	struct btif_dma_chan tx_dma;
	struct btif_dma_chan rx_dma;

	enum btif_mode tx_mode;
	enum btif_mode rx_mode;
	enum btif_state state;
	bool enabled;
	bool lpbk;

	struct mutex state_mtx;		/* state machine */
	struct mutex ops_mtx;		/* open/close */
	struct mutex tx_mtx;		/* TX serialisation */

	struct btif_user *owner;
	mtk_btif_rx_cb rx_cb;

	/* RX staging ring, filled from IRQ context */
	u8 *rx_buf;
	unsigned int rx_size;
	unsigned int rx_rd;
	unsigned int rx_wr;
	spinlock_t rx_lock;

	struct task_struct *rx_thread;
	struct completion rx_comp;
	struct mutex rx_mtx;		/* RX ring consumption */

	/* PIO TX staging */
	struct kfifo *tx_fifo;
	spinlock_t tx_lock;

	struct btif_log tx_log;
	struct btif_log rx_log;
};

/* mtk_btif.c */
unsigned int btif_rx_ring_write(struct mtk_btif *btif, const u8 *buf,
				unsigned int len);
unsigned int btif_rx_ring_count(struct mtk_btif *btif);

/* btif_hw.c */
int btif_clk_enable(struct mtk_btif *btif);
void btif_clk_disable(struct mtk_btif *btif);
int btif_apdma_clk_enable(struct mtk_btif *btif);
void btif_apdma_clk_disable(struct mtk_btif *btif);
int btif_hw_init(struct mtk_btif *btif);
int btif_hw_tx_mode(struct mtk_btif *btif, enum btif_mode mode);
int btif_hw_rx_mode(struct mtk_btif *btif, enum btif_mode mode);
void btif_hw_tx_ier(struct mtk_btif *btif, bool en);
void btif_hw_rx_ier(struct mtk_btif *btif, bool en);
int btif_hw_loopback(struct mtk_btif *btif, bool en);
int btif_hw_send(struct mtk_btif *btif, const u8 *buf, unsigned int len);
int btif_hw_tx_irq(struct mtk_btif *btif);
int btif_hw_rx_drain(struct mtk_btif *btif);
int btif_hw_raise_wake(struct mtk_btif *btif);
bool btif_hw_is_tx_complete(struct mtk_btif *btif);
void btif_hw_dump_regs(struct mtk_btif *btif);
u32 btif_readl(struct mtk_btif *btif, u32 off);
void btif_writel(struct mtk_btif *btif, u32 val, u32 off);
u8 btif_readb(struct mtk_btif *btif, u32 off);
void btif_writeb(struct mtk_btif *btif, u8 val, u32 off);

/* btif_dma.c */
int btif_dma_hw_init(struct btif_dma_chan *chan);
void btif_dma_vfifo_reset(struct btif_dma_chan *chan);
int btif_dma_ctrl(struct btif_dma_chan *chan, bool enable);
int btif_dma_send(struct btif_dma_chan *chan, const u8 *buf,
		  unsigned int len);
bool btif_dma_is_tx_allow(struct btif_dma_chan *chan);
int btif_dma_tx_room(struct btif_dma_chan *chan);
bool btif_dma_is_tx_complete(struct btif_dma_chan *chan);
irqreturn_t btif_dma_tx_irq(int irq, void *data);
irqreturn_t btif_dma_rx_irq(int irq, void *data);
void btif_dma_dump_regs(struct btif_dma_chan *chan);

#endif /* __BTIF_CORE_H__ */
