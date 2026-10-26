// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek BTIF APDMA (virtual FIFO) channel operations.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/btif/common/btif_dma_plat.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include "btif_core.h"
#include "btif_regs.h"

static u32 btif_dma_readl(struct btif_dma_chan *chan, u32 off)
{
	return readl(chan->base + off);
}

static void btif_dma_writel(struct btif_dma_chan *chan, u32 val, u32 off)
{
	writel(val, chan->base + off);
}

/*
 * Reset the software view of the virtual FIFO pointers.
 * Downstream btif_dma_plat.c:576-591 (hal_btif_vfifo_reset).
 */
void btif_dma_vfifo_reset(struct btif_dma_chan *chan)
{
	chan->vfifo.rpt = 0;
	chan->vfifo.last_rpt_wrap = 0;
	chan->vfifo.wpt = 0;
	chan->vfifo.last_wpt_wrap = 0;
}

/*
 * Downstream btif_dma_plat.c:397-460 (hal_btif_dma_hw_init): warm reset the
 * channel, program the virtual FIFO base/length/pointers/threshold and mask
 * or clear the channel interrupt.
 */
int btif_dma_hw_init(struct btif_dma_chan *chan)
{
	struct btif_vfifo *vfifo = &chan->vfifo;
	u32 dat;

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_RST) |
			      BTIF_DMA_WARM_RST, BTIF_DMA_REG_RST);
	do {
		dat = btif_dma_readl(chan, BTIF_DMA_REG_EN);
	} while (dat & BTIF_DMA_EN_BIT);

	btif_dma_writel(chan, lower_32_bits(vfifo->phys),
			BTIF_DMA_REG_VFF_ADDR);
	btif_dma_writel(chan, vfifo->size, BTIF_DMA_REG_VFF_LEN);
	btif_dma_writel(chan, vfifo->wpt, BTIF_DMA_REG_VFF_WPT);
	btif_dma_writel(chan, vfifo->rpt, BTIF_DMA_REG_VFF_RPT);
	btif_dma_writel(chan, vfifo->thre, BTIF_DMA_REG_VFF_THRE);

	if (chan->dir == BTIF_DMA_RX) {
		btif_dma_writel(chan,
				btif_dma_readl(chan, BTIF_DMA_REG_INT_FLAG) |
				BTIF_RX_DMA_INT_DONE | BTIF_RX_DMA_INT_THRE,
				BTIF_DMA_REG_INT_FLAG);
		btif_dma_writel(chan,
				btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) |
				BTIF_RX_DMA_INT_THRE_EN |
				BTIF_RX_DMA_INT_DONE_EN, BTIF_DMA_REG_INT_EN);
	} else {
		btif_dma_writel(chan,
				btif_dma_readl(chan, BTIF_DMA_REG_INT_FLAG) &
				~BTIF_TX_DMA_INT_FLAG_MASK,
				BTIF_DMA_REG_INT_FLAG);
		btif_dma_writel(chan,
				btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) &
				~BTIF_TX_DMA_INTEN_BIT, BTIF_DMA_REG_INT_EN);
	}

	chan->flush_irq_count = 0;
	return 0;
}

/*
 * Downstream btif_dma_plat.c:502-564.  Disable is a graceful stop: setting
 * STOP lets the current transaction finish, so poll until the hardware
 * clears it.
 */
int btif_dma_ctrl(struct btif_dma_chan *chan, bool enable)
{
	u32 dat;

	if (enable) {
		btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_EN) |
				      BTIF_DMA_EN_BIT, BTIF_DMA_REG_EN);
		return 0;
	}

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_STOP) |
			      BTIF_DMA_STOP_BIT, BTIF_DMA_REG_STOP);
	do {
		dat = btif_dma_readl(chan, BTIF_DMA_REG_STOP);
	} while (dat & BTIF_DMA_STOP_BIT);

	return 0;
}

/* Downstream btif_dma_plat.c:1256-1284 */
static void btif_dma_tx_flush(struct btif_dma_chan *chan)
{
	u32 stop = btif_dma_readl(chan, BTIF_DMA_REG_STOP);

	if (stop & BTIF_DMA_STOP_BIT) {
		dev_warn_ratelimited(chan->btif->dev,
				     "BTIF TX DMA stopped, skip flush\n");
		return;
	}

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_FLUSH) |
			      BTIF_DMA_FLUSH_BIT, BTIF_DMA_REG_FLUSH);
}

static bool btif_dma_tx_in_flush(struct btif_dma_chan *chan)
{
	return btif_dma_readl(chan, BTIF_DMA_REG_FLUSH) & BTIF_DMA_FLUSH_BIT;
}

/*
 * Downstream btif_dma_plat.c:942-959.  A flush needs to complete before the
 * next WPT update; the worst case wait derived from the BTIF baud rate is
 * 8 bytes at the minimum transfer rate (26 MHz / 13 = 2 MB/s), i.e. 4 us.
 */
bool btif_dma_is_tx_allow(struct btif_dma_chan *chan)
{
#define BTIF_MIN_TX_MB	((26 * 1000000 / 13) / 1000000)
	unsigned int wait_us = 8 / BTIF_MIN_TX_MB;

	if (!btif_dma_tx_in_flush(chan))
		return true;

	usleep_range(wait_us, 2 * wait_us);

	if (btif_dma_tx_in_flush(chan)) {
		dev_warn_ratelimited(chan->btif->dev,
				     "BTIF TX DMA flush still running\n");
		return false;
	}

	return true;
}

/* Downstream btif_dma_plat.c:918-930 */
int btif_dma_tx_room(struct btif_dma_chan *chan)
{
	return btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEFT_SIZE);
}

/*
 * Downstream btif_dma_plat.c:775-871 (hal_dma_send_data).  The caller has
 * already checked that the virtual FIFO holds enough room; the length is
 * still clamped here because a hardware ring wrap must never overrun.
 */
int btif_dma_send(struct btif_dma_chan *chan, const u8 *buf, unsigned int len)
{
	struct btif_vfifo *vfifo = &chan->vfifo;
	unsigned int avail;
	unsigned int wpt;
	unsigned int wrap;
	unsigned int send;

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) &
			      ~BTIF_TX_DMA_INTEN_BIT, BTIF_DMA_REG_INT_EN);

	avail = btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEFT_SIZE);
	wpt = btif_dma_readl(chan, BTIF_DMA_REG_VFF_WPT);
	wrap = wpt & BTIF_DMA_WPT_WRAP;
	wpt &= BTIF_DMA_WPT_MASK;

	send = min(len, avail);
	if (send < len)
		dev_warn_ratelimited(chan->btif->dev,
				     "BTIF TX vFIFO short: %u of %u\n",
				     send, len);

	if (send + wpt >= vfifo->size) {
		unsigned int tail = vfifo->size - wpt;

		memcpy((u8 *)vfifo->vir + wpt, buf, tail);
		memcpy(vfifo->vir, buf + tail, send - tail);
		wpt = send - tail;
		wrap ^= BTIF_DMA_WPT_WRAP;
	} else {
		memcpy((u8 *)vfifo->vir + wpt, buf, send);
		wpt += send;
	}

	/* data must reach memory before the hardware sees the new WPT */
	wmb();

	vfifo->wpt = wpt;
	vfifo->last_wpt_wrap = wrap;

	if (!btif_dma_is_tx_allow(chan)) {
		btif_dma_writel(chan, btif_dma_readl(chan,
					BTIF_DMA_REG_INT_EN) |
					BTIF_TX_DMA_INTEN_BIT,
				BTIF_DMA_REG_INT_EN);
		return 0;
	}

	btif_dma_ctrl(chan, true);
	btif_dma_writel(chan, wpt | wrap, BTIF_DMA_REG_VFF_WPT);

	/* a sub 8-byte remainder would never trigger the threshold */
	avail = btif_dma_readl(chan, BTIF_DMA_REG_VFF_VALID_SIZE);
	if (avail > 0 && avail < 8)
		btif_dma_tx_flush(chan);

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) |
			      BTIF_TX_DMA_INTEN_BIT, BTIF_DMA_REG_INT_EN);

	return send;
}

/* Downstream btif_dma_plat.c:883-906 */
bool btif_dma_is_tx_complete(struct btif_dma_chan *chan)
{
	unsigned int valid = btif_dma_readl(chan,
					    BTIF_DMA_REG_VFF_VALID_SIZE);
	unsigned int internal = btif_dma_readl(chan,
					       BTIF_DMA_REG_INT_BUF_SIZE);
	bool irq_done = !(btif_dma_readl(chan, BTIF_DMA_REG_INT_FLAG) &
			  BTIF_TX_DMA_INT_FLAG_MASK);

	return valid == 0 && internal == 0 && irq_done;
}

/* Downstream btif_dma_plat.c:687-761 (hal_tx_dma_irq_handler) */
irqreturn_t btif_dma_tx_irq(int irq, void *data)
{
	struct btif_dma_chan *chan = data;
	unsigned int vff_len;
	unsigned int valid_size;
	unsigned int left_len;

	vff_len = btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEN);
	valid_size = btif_dma_readl(chan, BTIF_DMA_REG_VFF_VALID_SIZE);
	left_len = btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEFT_SIZE);

	if (valid_size > 0 && valid_size < 8) {
		btif_dma_tx_flush(chan);
		if (++chan->flush_irq_count >= 512) {
			dev_err(chan->btif->dev,
				"BTIF TX stalled, disabling TX DMA irq\n");
			btif_dma_writel(chan,
					btif_dma_readl(chan,
						BTIF_DMA_REG_INT_FLAG) &
					~BTIF_TX_DMA_INT_FLAG_MASK,
					BTIF_DMA_REG_INT_FLAG);
			btif_dma_writel(chan,
					btif_dma_readl(chan,
						BTIF_DMA_REG_INT_EN) &
					~BTIF_TX_DMA_INTEN_BIT,
					BTIF_DMA_REG_INT_EN);
		}
	} else if (vff_len == left_len) {
		chan->flush_irq_count = 0;
		btif_dma_writel(chan, btif_dma_readl(chan,
					BTIF_DMA_REG_INT_FLAG) &
					~BTIF_TX_DMA_INT_FLAG_MASK,
				BTIF_DMA_REG_INT_FLAG);
		btif_dma_writel(chan, btif_dma_readl(chan,
					BTIF_DMA_REG_INT_EN) &
					~BTIF_TX_DMA_INTEN_BIT,
				BTIF_DMA_REG_INT_EN);
	} else {
		dev_dbg(chan->btif->dev,
			"spurious BTIF TX DMA irq, vff_len %u left %u valid %u\n",
			vff_len, left_len, valid_size);
	}

	return IRQ_HANDLED;
}

/*
 * Downstream btif_dma_plat.c:974-1079 (hal_rx_dma_irq_handler): move the
 * received bytes out of the virtual FIFO, hand them to the staging ring and
 * publish the read pointer back to the hardware.
 */
irqreturn_t btif_dma_rx_irq(int irq, void *data)
{
	struct btif_dma_chan *chan = data;
	struct mtk_btif *btif = chan->btif;
	struct btif_vfifo *vfifo = &chan->vfifo;
	unsigned int valid_len;
	unsigned int wpt;
	unsigned int rpt;
	unsigned int real_len;
	unsigned int tail_len;
	unsigned int budget = 64;

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) &
			      ~(BTIF_RX_DMA_INT_THRE_EN |
				BTIF_RX_DMA_INT_DONE_EN), BTIF_DMA_REG_INT_EN);
	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_INT_FLAG) |
			      BTIF_RX_DMA_INT_DONE | BTIF_RX_DMA_INT_THRE,
			BTIF_DMA_REG_INT_FLAG);

	valid_len = btif_dma_readl(chan, BTIF_DMA_REG_VFF_VALID_SIZE);
	rpt = btif_dma_readl(chan, BTIF_DMA_REG_VFF_RPT);
	wpt = btif_dma_readl(chan, BTIF_DMA_REG_VFF_WPT);

	while ((valid_len > 0 || rpt != wpt) && budget--) {
		unsigned int rpt_wrap = rpt & BTIF_DMA_RPT_WRAP;
		unsigned int wpt_wrap = wpt & BTIF_DMA_WPT_WRAP;

		rpt &= BTIF_DMA_RPT_MASK;
		wpt &= BTIF_DMA_WPT_MASK;

		if (wpt_wrap != vfifo->last_wpt_wrap)
			real_len = wpt + vfifo->size - rpt;
		else
			real_len = wpt - rpt;

		tail_len = vfifo->size - rpt;
		if (tail_len >= real_len) {
			btif_rx_ring_write(btif, (u8 *)vfifo->vir + rpt, real_len);
		} else {
			btif_rx_ring_write(btif, (u8 *)vfifo->vir + rpt, tail_len);
			btif_rx_ring_write(btif, vfifo->vir,
					   real_len - tail_len);
		}

		/* bytes are consumed now; publish the read pointer */
		rmb();
		rpt += real_len;
		if (rpt >= vfifo->size) {
			rpt_wrap ^= BTIF_DMA_RPT_WRAP;
			rpt %= vfifo->size;
		}
		rpt |= rpt_wrap;

		vfifo->wpt = wpt;
		vfifo->last_wpt_wrap = wpt_wrap;
		vfifo->rpt = rpt;
		vfifo->last_rpt_wrap = rpt_wrap;
		btif_dma_writel(chan, rpt, BTIF_DMA_REG_VFF_RPT);

		valid_len = btif_dma_readl(chan, BTIF_DMA_REG_VFF_VALID_SIZE);
		rpt = btif_dma_readl(chan, BTIF_DMA_REG_VFF_RPT);
		wpt = btif_dma_readl(chan, BTIF_DMA_REG_VFF_WPT);
	}

	if (!budget)
		dev_warn_ratelimited(btif->dev, "BTIF RX DMA drain budget hit\n");

	btif_dma_writel(chan, btif_dma_readl(chan, BTIF_DMA_REG_INT_EN) |
			      BTIF_RX_DMA_INT_THRE_EN |
			      BTIF_RX_DMA_INT_DONE_EN, BTIF_DMA_REG_INT_EN);

	complete(&btif->rx_comp);
	return IRQ_HANDLED;
}

/* Downstream btif_dma_plat.c:1081-1254 */
void btif_dma_dump_regs(struct btif_dma_chan *chan)
{
	struct device *dev = chan->btif->dev;
	const char *dir = chan->dir == BTIF_DMA_TX ? "TX" : "RX";

	dev_info(dev, "BTIF %s DMA base %p\n", dir, chan->base);
	dev_info(dev, "%s EN 0x%08x STOP 0x%08x FLUSH 0x%08x INT_FLAG 0x%08x INT_EN 0x%08x\n",
		 dir, btif_dma_readl(chan, BTIF_DMA_REG_EN),
		 btif_dma_readl(chan, BTIF_DMA_REG_STOP),
		 btif_dma_readl(chan, BTIF_DMA_REG_FLUSH),
		 btif_dma_readl(chan, BTIF_DMA_REG_INT_FLAG),
		 btif_dma_readl(chan, BTIF_DMA_REG_INT_EN));
	dev_info(dev, "%s VFF_ADDR 0x%08x LEN 0x%08x THRE 0x%08x WPT 0x%08x RPT 0x%08x\n",
		 dir, btif_dma_readl(chan, BTIF_DMA_REG_VFF_ADDR),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEN),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_THRE),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_WPT),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_RPT));
	dev_info(dev, "%s INT_BUF_SIZE 0x%08x VALID 0x%08x LEFT 0x%08x DBG 0x%08x\n",
		 dir, btif_dma_readl(chan, BTIF_DMA_REG_INT_BUF_SIZE),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_VALID_SIZE),
		 btif_dma_readl(chan, BTIF_DMA_REG_VFF_LEFT_SIZE),
		 btif_dma_readl(chan, BTIF_DMA_REG_DBG_STATUS));
}
