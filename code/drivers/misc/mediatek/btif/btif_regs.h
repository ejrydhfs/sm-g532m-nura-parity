/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek BTIF register map.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/btif/common/plat_inc/btif_priv.h
 *   drivers/misc/mediatek/btif/common/plat_inc/btif_dma_priv.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef __BTIF_REGS_H__
#define __BTIF_REGS_H__

/* BTIF controller registers, downstream btif_priv.h:32-45 */
#define BTIF_REG_RBR		0x00	/* RX buffer register, read only */
#define BTIF_REG_THR		0x00	/* TX holding register, write only */
#define BTIF_REG_IER		0x04	/* interrupt enable */
#define BTIF_REG_IIR		0x08	/* interrupt identification, read only */
#define BTIF_REG_FIFOCTRL	0x08	/* FIFO control, write only */
#define BTIF_REG_FAKELCR	0x0c	/* fake LCR */
#define BTIF_REG_LSR		0x14	/* line status */
#define BTIF_REG_SLEEP_EN	0x48	/* sleep enable */
#define BTIF_REG_DMA_EN		0x4c	/* DMA enable */
#define BTIF_REG_RTOCNT		0x54	/* RX timeout count */
#define BTIF_REG_TRI_LVL	0x60	/* TX/RX trigger level */
#define BTIF_REG_WAK		0x64	/* module wakeup, write only */
#define BTIF_REG_WAT_TIME	0x68	/* async wait time */
#define BTIF_REG_HANDSHAKE	0x6c	/* new handshake control */

/* BTIF_REG_IER bits, downstream btif_priv.h:48-49 */
#define BTIF_IER_TXEEN		BIT(1)	/* TX holding register empty */
#define BTIF_IER_RXFEN		BIT(0)	/* RX buffer contains data */

/* BTIF_REG_IIR bits, downstream btif_priv.h:52-55 */
#define BTIF_IIR_NINT		BIT(0)	/* no interrupt pending */
#define BTIF_IIR_TX_EMPTY	BIT(1)	/* TX holding register empty */
#define BTIF_IIR_RX		BIT(2)	/* RX data received */
#define BTIF_IIR_RX_TIMEOUT	(0x11 << 2)

/* BTIF_REG_LSR bits, downstream btif_priv.h:58-60 */
#define BTIF_LSR_DR		BIT(0)
#define BTIF_LSR_THRE		BIT(5)
#define BTIF_LSR_TEMT		BIT(6)

/* BTIF_REG_FIFOCTRL bits, downstream btif_priv.h:63-64 */
#define BTIF_FIFOCTRL_CLR_TX	BIT(2)
#define BTIF_FIFOCTRL_CLR_RX	BIT(1)

/* BTIF_REG_FAKELCR values, downstream btif_priv.h:67 */
#define BTIF_FAKELCR_NORMAL_MODE	0x0

/* BTIF_REG_SLEEP_EN bits, downstream btif_priv.h:70-71 */
#define BTIF_SLEEP_EN_BIT	BIT(0)

/* BTIF_REG_DMA_EN bits, downstream btif_priv.h:74-78 */
#define BTIF_DMA_EN_RX		BIT(0)
#define BTIF_DMA_EN_TX		BIT(1)
#define BTIF_DMA_EN_AUTORST_EN	BIT(2)

/* BTIF_REG_TRI_LVL bits, downstream btif_priv.h:81-88 */
#define BTIF_TRI_LVL_TX_MASK	(0xf << 0)
#define BTIF_TRI_LVL_RX_MASK	(0x7 << 4)
#define BTIF_TRI_LVL_TX(x)	(((x) & 0xf) << 0)
#define BTIF_TRI_LVL_RX(x)	(((x) & 0x7) << 4)
#define BTIF_TRI_LOOP_EN	BIT(7)

/* BTIF_REG_WAK bits, downstream btif_priv.h:91 */
#define BTIF_WAK_BIT		BIT(0)

/* BTIF_REG_HANDSHAKE values, downstream btif_priv.h:94-95 */
#define BTIF_HANDSHAKE_EN	1

/*
 * APDMA BTIF channel registers.  Both directions use the same layout at a
 * different base address (0x11000880 for TX, 0x11000900 for RX).
 * Downstream btif_dma_priv.h:44-96.
 */
#define BTIF_DMA_REG_INT_FLAG		0x00
#define BTIF_DMA_REG_INT_EN		0x04
#define BTIF_DMA_REG_EN			0x08
#define BTIF_DMA_REG_RST		0x0c
#define BTIF_DMA_REG_STOP		0x10
#define BTIF_DMA_REG_FLUSH		0x14
#define BTIF_DMA_REG_VFF_ADDR		0x1c
#define BTIF_DMA_REG_VFF_LEN		0x24
#define BTIF_DMA_REG_VFF_THRE		0x28
#define BTIF_DMA_REG_VFF_WPT		0x2c
#define BTIF_DMA_REG_VFF_RPT		0x30
#define BTIF_DMA_REG_INT_BUF_SIZE	0x38
#define BTIF_DMA_REG_VFF_VALID_SIZE	0x3c
#define BTIF_DMA_REG_VFF_LEFT_SIZE	0x40
#define BTIF_DMA_REG_DBG_STATUS		0x50

/* Downstream btif_dma_priv.h:98-145 */
#define BTIF_DMA_EN_BIT			BIT(0)
#define BTIF_DMA_STOP_BIT		BIT(0)
#define BTIF_DMA_RST_BIT		BIT(0)
#define BTIF_DMA_FLUSH_BIT		BIT(0)

#define BTIF_DMA_WARM_RST		BIT(0)
#define BTIF_DMA_HARD_RST		BIT(1)

#define BTIF_DMA_WPT_MASK		0x0000ffff
#define BTIF_DMA_WPT_WRAP		0x00010000
#define BTIF_DMA_RPT_MASK		0x0000ffff
#define BTIF_DMA_RPT_WRAP		0x00010000

/* TX channel interrupt bits, downstream btif_dma_priv.h:113-125 */
#define BTIF_TX_DMA_INT_FLAG_MASK	0x1
#define BTIF_TX_DMA_INTEN_BIT		BIT(0)

/* RX channel interrupt bits, downstream btif_dma_priv.h:128-145 */
#define BTIF_RX_DMA_INT_THRE		BIT(0)
#define BTIF_RX_DMA_INT_DONE		BIT(1)
#define BTIF_RX_DMA_INT_THRE_EN		BIT(0)
#define BTIF_RX_DMA_INT_DONE_EN		BIT(1)

#endif /* __BTIF_REGS_H__ */
