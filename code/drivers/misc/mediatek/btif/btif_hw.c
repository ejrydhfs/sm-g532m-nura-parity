// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek BTIF controller hardware operations (PIO path, interrupts,
 * wakeup signalling and clock control).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/btif/common/btif_plat.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>

#include "btif_core.h"
#include "btif_regs.h"

u32 btif_readl(struct mtk_btif *btif, u32 off)
{
	return readl(btif->base + off);
}

void btif_writel(struct mtk_btif *btif, u32 val, u32 off)
{
	writel(val, btif->base + off);
}

u8 btif_readb(struct mtk_btif *btif, u32 off)
{
	return readb(btif->base + off);
}

void btif_writeb(struct mtk_btif *btif, u8 val, u32 off)
{
	writeb(val, btif->base + off);
}

/*
 * Clock control.  Downstream keeps two independent reference counters
 * (hal_btif_clk_ctrl() in btif_plat.c:321 and hal_btif_dma_clk_ctrl() in
 * btif_dma_plat.c:263, each wrapping clk_enable()/clk_disable()).  The CCF
 * already reference counts enable/disable, so wrap it directly.
 */
int btif_clk_enable(struct mtk_btif *btif)
{
	return clk_enable(btif->clk_btif);
}

void btif_clk_disable(struct mtk_btif *btif)
{
	clk_disable(btif->clk_btif);
}

int btif_apdma_clk_enable(struct mtk_btif *btif)
{
	return clk_enable(btif->clk_apdma);
}

void btif_apdma_clk_disable(struct mtk_btif *btif)
{
	clk_disable(btif->clk_apdma);
}

/* Downstream btif_plat.c:535-575 */
void btif_hw_tx_ier(struct mtk_btif *btif, bool en)
{
	u32 val = btif_readl(btif, BTIF_REG_IER);

	if (en)
		val |= BTIF_IER_TXEEN;
	else
		val &= ~BTIF_IER_TXEEN;

	btif_writel(btif, val, BTIF_REG_IER);
}

/* Downstream btif_plat.c:535-548 */
void btif_hw_rx_ier(struct mtk_btif *btif, bool en)
{
	u32 val = btif_readl(btif, BTIF_REG_IER);

	if (en)
		val |= BTIF_IER_RXFEN;
	else
		val &= ~BTIF_IER_RXFEN;

	btif_writel(btif, val, BTIF_REG_IER);
}

/* Downstream btif_plat.c:938-979 */
int btif_hw_tx_mode(struct mtk_btif *btif, enum btif_mode mode)
{
	u32 val = btif_readl(btif, BTIF_REG_DMA_EN);

	if (mode == BTIF_MODE_DMA)
		val |= BTIF_DMA_EN_TX;
	else
		val &= ~BTIF_DMA_EN_TX;

	btif_writel(btif, val, BTIF_REG_DMA_EN);
	return 0;
}

int btif_hw_rx_mode(struct mtk_btif *btif, enum btif_mode mode)
{
	u32 val = btif_readl(btif, BTIF_REG_DMA_EN);

	if (mode == BTIF_MODE_DMA)
		val |= BTIF_DMA_EN_RX;
	else
		val &= ~BTIF_DMA_EN_RX;

	btif_writel(btif, val, BTIF_REG_DMA_EN);
	return 0;
}

/* Downstream btif_plat.c:708-722 */
int btif_hw_loopback(struct mtk_btif *btif, bool en)
{
	u32 val = btif_readl(btif, BTIF_REG_TRI_LVL);

	if (en)
		val |= BTIF_TRI_LOOP_EN;
	else
		val &= ~BTIF_TRI_LOOP_EN;

	btif_writel(btif, val, BTIF_REG_TRI_LVL);
	return 0;
}

/*
 * Downstream btif_plat.c:476-522 (hal_btif_hw_init): normal mode, new
 * handshake, FIFO flush, trigger levels, PIO default, RX auto-reset and
 * RX interrupt enable.  The TX interrupt stays masked until data is queued.
 */
int btif_hw_init(struct mtk_btif *btif)
{
	struct kfifo *fifo = btif->tx_fifo;

	if (fifo)
		kfifo_reset(fifo);

	/* normal (non-UART) mode */
	btif_writel(btif, BTIF_FAKELCR_NORMAL_MODE, BTIF_REG_FAKELCR);

	/* new handshake mode */
	btif_writel(btif, btif_readl(btif, BTIF_REG_HANDSHAKE) |
			  BTIF_HANDSHAKE_EN, BTIF_REG_HANDSHAKE);

	/* flush both FIFOs */
	btif_writel(btif, btif_readl(btif, BTIF_REG_FIFOCTRL) |
			  BTIF_FIFOCTRL_CLR_RX, BTIF_REG_FIFOCTRL);
	btif_writel(btif, btif_readl(btif, BTIF_REG_FIFOCTRL) &
			  ~BTIF_FIFOCTRL_CLR_RX, BTIF_REG_FIFOCTRL);
	btif_writel(btif, btif_readl(btif, BTIF_REG_FIFOCTRL) |
			  BTIF_FIFOCTRL_CLR_TX, BTIF_REG_FIFOCTRL);
	btif_writel(btif, btif_readl(btif, BTIF_REG_FIFOCTRL) &
			  ~BTIF_FIFOCTRL_CLR_TX, BTIF_REG_FIFOCTRL);

	btif_writel(btif, BTIF_TRI_LVL_TX(BTIF_TX_FIFO_THRE) |
			  BTIF_TRI_LVL_RX(BTIF_RX_FIFO_THRE),
		    BTIF_REG_TRI_LVL);
	btif_hw_loopback(btif, false);

	/* default to PIO, DMA is enabled later by the DMA setup path */
	btif_hw_tx_mode(btif, BTIF_MODE_PIO);
	btif_hw_rx_mode(btif, BTIF_MODE_PIO);

	/* auto reset of the RX timeout counter */
	btif_writel(btif, btif_readl(btif, BTIF_REG_DMA_EN) |
			  BTIF_DMA_EN_AUTORST_EN, BTIF_REG_DMA_EN);

	btif_hw_tx_ier(btif, false);
	btif_hw_rx_ier(btif, true);
	return 0;
}

/*
 * PIO transmit: queue into the controller FIFO staging kfifo and let the
 * TX-empty interrupt push bytes into THR.  Downstream btif_plat.c:993-1075.
 */
int btif_hw_send(struct mtk_btif *btif, const u8 *buf, unsigned int len)
{
	unsigned long flags;
	unsigned int room;
	unsigned int queued;

	room = kfifo_avail(btif->tx_fifo);
	queued = min(len, room);
	if (!queued)
		return 0;

	spin_lock_irqsave(&btif->tx_lock, flags);
	queued = kfifo_in(btif->tx_fifo, buf, queued);
	if (queued)
		btif_hw_tx_ier(btif, true);
	spin_unlock_irqrestore(&btif->tx_lock, flags);

	return queued;
}

/* Downstream btif_plat.c:874-925 */
int btif_hw_tx_irq(struct mtk_btif *btif)
{
	unsigned long flags;
	unsigned int lsr;
	unsigned int avail;
	unsigned int count;
	u8 local[BTIF_TX_FIFO_DEPTH];
	unsigned int i;

	lsr = btif_readl(btif, BTIF_REG_LSR);
	if (lsr & BTIF_LSR_TEMT)
		avail = BTIF_TX_FIFO_DEPTH;
	else if (lsr & BTIF_LSR_THRE)
		avail = BTIF_TX_FIFO_DEPTH - BTIF_TX_FIFO_THRE;
	else
		return 0;

	spin_lock_irqsave(&btif->tx_lock, flags);
	count = kfifo_out(btif->tx_fifo, local, avail);
	spin_unlock_irqrestore(&btif->tx_lock, flags);

	for (i = 0; i < count; i++)
		btif_writeb(btif, local[i], BTIF_REG_THR);

	spin_lock_irqsave(&btif->tx_lock, flags);
	if (kfifo_is_empty(btif->tx_fifo))
		btif_hw_tx_ier(btif, false);
	spin_unlock_irqrestore(&btif->tx_lock, flags);

	return count;
}

/*
 * PIO receive: drain the RX FIFO into the staging ring.
 * Downstream btif_plat.c:816-860.
 */
int btif_hw_rx_drain(struct mtk_btif *btif)
{
	u8 local[256];
	unsigned int len = 0;
	unsigned int iir;

	iir = btif_readl(btif, BTIF_REG_IIR);
	while ((iir & (BTIF_IIR_RX | BTIF_IIR_RX_TIMEOUT)) &&
	       len < sizeof(local)) {
		local[len++] = btif_readb(btif, BTIF_REG_RBR);
		iir = btif_readl(btif, BTIF_REG_IIR);
	}

	if (len)
		btif_rx_ring_write(btif, local, len);

	return len;
}

/*
 * Wake the connectivity SoC.  Writing 0 pulses the wake line low for longer
 * than a 32 kHz period (downstream uses a 64..96 us sleep) before releasing
 * it.  Downstream btif_plat.c:1088-1112.
 */
int btif_hw_raise_wake(struct mtk_btif *btif)
{
	btif_writel(btif, btif_readl(btif, BTIF_REG_WAK) & ~BTIF_WAK_BIT,
		    BTIF_REG_WAK);
	usleep_range(64, 96);
	btif_writel(btif, btif_readl(btif, BTIF_REG_WAK) | BTIF_WAK_BIT,
		    BTIF_REG_WAK);
	return 0;
}

/*
 * TX is complete when THR and the shift register are empty, the TX interrupt
 * is masked, no RX byte is pending and the staging kfifo drained.
 * Downstream btif_plat.c:1204-1241.
 */
bool btif_hw_is_tx_complete(struct mtk_btif *btif)
{
	unsigned long flags;
	bool complete;
	u32 lsr;

	lsr = btif_readl(btif, BTIF_REG_LSR);
	complete = (lsr & BTIF_LSR_TEMT) &&
		   !(btif_readl(btif, BTIF_REG_IER) & BTIF_IER_TXEEN) &&
		   !(lsr & BTIF_LSR_DR);

	if (complete) {
		spin_lock_irqsave(&btif->tx_lock, flags);
		complete = kfifo_is_empty(btif->tx_fifo);
		spin_unlock_irqrestore(&btif->tx_lock, flags);
	}

	return complete;
}

/* Downstream btif_plat.c:1125-1192 (register dump used by debug control) */
void btif_hw_dump_regs(struct mtk_btif *btif)
{
	struct device *dev = btif->dev;
	unsigned int i;

	dev_info(dev, "BTIF register dump:\n");
	dev_info(dev, "IER 0x%08x IIR 0x%08x LSR 0x%08x\n",
		 btif_readl(btif, BTIF_REG_IER),
		 btif_readl(btif, BTIF_REG_IIR),
		 btif_readl(btif, BTIF_REG_LSR));
	dev_info(dev, "FAKELCR 0x%08x SLEEP_EN 0x%08x DMA_EN 0x%08x\n",
		 btif_readl(btif, BTIF_REG_FAKELCR),
		 btif_readl(btif, BTIF_REG_SLEEP_EN),
		 btif_readl(btif, BTIF_REG_DMA_EN));
	dev_info(dev, "RTOCNT 0x%08x TRI_LVL 0x%08x WAT_TIME 0x%08x HANDSHAKE 0x%08x\n",
		 btif_readl(btif, BTIF_REG_RTOCNT),
		 btif_readl(btif, BTIF_REG_TRI_LVL),
		 btif_readl(btif, BTIF_REG_WAT_TIME),
		 btif_readl(btif, BTIF_REG_HANDSHAKE));

	/*
	 * Skip offset 0x00 (RBR/THR) so the debug dump cannot consume a
	 * received byte, as downstream does (btif_plat.c:1147-1150).
	 */
	for (i = 1; i < 0xE0 / 4; i++)
		dev_info(dev, "BTIF[0x%02x] = 0x%08x\n", i * 4,
			 btif_readl(btif, i * 4));
}
