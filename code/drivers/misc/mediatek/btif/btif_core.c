// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek BTIF driver: platform glue, owner/channel model, state machine,
 * receive staging and the exported mtk_btif API.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/btif/common/mtk_btif.c
 *   drivers/misc/mediatek/btif/common/mtk_btif_exp.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "btif_core.h"
#include "btif_regs.h"

/*
 * One BTIF controller exists on the SoC and the exported API has no device
 * argument, so the instance is published here at probe time.
 */
static struct mtk_btif *g_btif;

static const char *const btif_state_name[] = {
	"OFF",
	"SUSPEND",
	"DPIDLE",
	"ON",
};

static void btif_log_record(struct btif_log *log, const void *buf,
			    unsigned int len);

/* ------------------------------------------------------------------ */
/* Receive staging ring                                               */
/* ------------------------------------------------------------------ */

unsigned int btif_rx_ring_count(struct mtk_btif *btif)
{
	return (btif->rx_wr - btif->rx_rd) & (btif->rx_size - 1);
}

/*
 * Called from hard IRQ context by the PIO drain and the RX DMA handler.
 * One byte is always kept free so count() unambiguously distinguishes empty
 * from full, as downstream does (BBS_LEFT() - 1 in mtk_btif.c:2574).
 */
unsigned int btif_rx_ring_write(struct mtk_btif *btif, const u8 *buf,
				unsigned int len)
{
	unsigned long flags;
	unsigned int space;
	unsigned int tail;

	if (!btif->rx_buf)
		return 0;

	spin_lock_irqsave(&btif->rx_lock, flags);
	space = btif->rx_size - 1 - btif_rx_ring_count(btif);
	if (len > space) {
		dev_warn_ratelimited(btif->dev,
				     "BTIF RX ring overrun: %u dropped\n",
				     len - space);
		len = space;
	}

	if (len) {
		tail = min(len, btif->rx_size - btif->rx_wr);
		memcpy(btif->rx_buf + btif->rx_wr, buf, tail);
		if (len > tail)
			memcpy(btif->rx_buf, buf + tail, len - tail);
		btif->rx_wr = (btif->rx_wr + len) & (btif->rx_size - 1);
	}
	spin_unlock_irqrestore(&btif->rx_lock, flags);

	if (len)
		btif_log_record(&btif->rx_log, buf, len);

	return len;
}

/*
 * Hand staged bytes to the owner.  The callback runs in the rx thread, i.e.
 * in process context, because the STP parser it feeds may sleep.  A return
 * value of 1..chunk consumes that many bytes; 0 or a negative/oversized value
 * keeps the historical downstream behaviour of consuming the whole chunk
 * (mtk_btif.c:2067-2123 advances rd_idx to wr_idx unconditionally).
 */
static void btif_rx_consume(struct mtk_btif *btif)
{
	for (;;) {
		unsigned long flags;
		unsigned int count;
		unsigned int chunk;
		u8 *ptr;
		int consumed;

		spin_lock_irqsave(&btif->rx_lock, flags);
		count = btif_rx_ring_count(btif);
		ptr = btif->rx_buf + btif->rx_rd;
		chunk = min(count, btif->rx_size - btif->rx_rd);
		spin_unlock_irqrestore(&btif->rx_lock, flags);

		if (!chunk || !btif->rx_cb)
			break;

		consumed = btif->rx_cb(ptr, chunk);
		if (consumed <= 0 || consumed > (int)chunk)
			consumed = chunk;

		spin_lock_irqsave(&btif->rx_lock, flags);
		btif->rx_rd = (btif->rx_rd + consumed) & (btif->rx_size - 1);
		spin_unlock_irqrestore(&btif->rx_lock, flags);
	}
}

/* Downstream mtk_btif.c:2296-2323 */
static int btif_rx_thread(void *data)
{
	struct mtk_btif *btif = data;

	while (!kthread_should_stop()) {
		if (btif->enabled)
			wait_for_completion_interruptible_timeout
				(&btif->rx_comp, msecs_to_jiffies(360));
		else
			wait_for_completion_interruptible(&btif->rx_comp);

		if (kthread_should_stop())
			break;

		mutex_lock(&btif->rx_mtx);
		btif_rx_consume(btif);
		mutex_unlock(&btif->rx_mtx);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Debug packet log                                                   */
/* ------------------------------------------------------------------ */

static void btif_log_init(struct btif_log *log, const char *name,
			  struct btif_log_entry *entries)
{
	log->name = name;
	log->enabled = true;
	log->realtime = false;
	log->in = 0;
	log->count = 0;
	log->entries = entries;
	spin_lock_init(&log->lock);
}

static void btif_log_record(struct btif_log *log, const void *buf,
			    unsigned int len)
{
	struct btif_log_entry *entry;
	unsigned long flags;

	if (!log->entries || !len)
		return;

	spin_lock_irqsave(&log->lock, flags);
	if (!log->enabled) {
		spin_unlock_irqrestore(&log->lock, flags);
		return;
	}

	entry = &log->entries[log->in];
	entry->ts = ktime_get();
	entry->len = min_t(unsigned int, len, BTIF_LOG_SZ);
	memcpy(entry->buf, buf, entry->len);
	log->in = (log->in + 1) % BTIF_LOG_ENTRY_NUM;
	if (log->count < BTIF_LOG_ENTRY_NUM)
		log->count++;

	if (log->realtime)
		print_hex_dump(KERN_INFO, log->name, DUMP_PREFIX_OFFSET,
			       16, 1, entry->buf, entry->len, false);
	spin_unlock_irqrestore(&log->lock, flags);
}

static void btif_log_clear(struct btif_log *log)
{
	unsigned long flags;

	spin_lock_irqsave(&log->lock, flags);
	log->in = 0;
	log->count = 0;
	spin_unlock_irqrestore(&log->lock, flags);
}

static void btif_log_set(struct btif_log *log, bool enabled)
{
	unsigned long flags;

	spin_lock_irqsave(&log->lock, flags);
	log->enabled = enabled;
	spin_unlock_irqrestore(&log->lock, flags);
}

static void btif_log_set_realtime(struct btif_log *log, bool realtime)
{
	unsigned long flags;

	spin_lock_irqsave(&log->lock, flags);
	log->realtime = realtime;
	spin_unlock_irqrestore(&log->lock, flags);
}

/* Downstream mtk_btif.c:2999-3046 (btif_log_buf_dmp_out) */
static void btif_log_dump(struct mtk_btif *btif, struct btif_log *log)
{
	struct btif_log_entry *entry;
	unsigned long flags;
	unsigned int count;
	unsigned int index;
	unsigned int i;

	spin_lock_irqsave(&log->lock, flags);
	count = log->count;
	index = (log->in + BTIF_LOG_ENTRY_NUM - count) % BTIF_LOG_ENTRY_NUM;
	dev_info(btif->dev, "BTIF %s log: %u entries\n", log->name, count);
	for (i = 0; i < count; i++) {
		entry = &log->entries[index];
		dev_info(btif->dev, "%s[%u] t=%lldus len=%u\n", log->name, i,
			 ktime_to_us(entry->ts), entry->len);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 1,
			       entry->buf, entry->len, false);
		index = (index + 1) % BTIF_LOG_ENTRY_NUM;
	}
	spin_unlock_irqrestore(&log->lock, flags);
}

/* ------------------------------------------------------------------ */
/* Controller / channel bring-up                                      */
/* ------------------------------------------------------------------ */

static irqreturn_t btif_isr(int irq, void *data)
{
	struct mtk_btif *btif = data;
	unsigned int budget = 64;
	bool received = false;

	while (budget--) {
		u32 iir = btif_readl(btif, BTIF_REG_IIR);

		if (iir & (BTIF_IIR_RX | BTIF_IIR_RX_TIMEOUT)) {
			if (!btif_hw_rx_drain(btif))
				break;
			received = true;
		} else if (iir & BTIF_IIR_TX_EMPTY) {
			btif_hw_tx_irq(btif);
		} else {
			break;
		}
	}

	if (received)
		complete(&btif->rx_comp);

	return IRQ_HANDLED;
}

/*
 * Downstream mtk_btif.c:1438-1478 (_btif_controller_setup).  The controller
 * interrupt is requested auto-disabled and only unmasked when a PIO path is
 * active (see btif_rx_setup()).
 */
static int btif_controller_setup(struct mtk_btif *btif)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&btif->rx_lock, flags);
	btif->rx_rd = 0;
	btif->rx_wr = 0;
	spin_unlock_irqrestore(&btif->rx_lock, flags);

	ret = btif_clk_enable(btif);
	if (ret) {
		dev_err(btif->dev, "failed to enable BTIF clock: %d\n", ret);
		return ret;
	}

	ret = btif_hw_init(btif);
	if (ret) {
		btif_clk_disable(btif);
		return ret;
	}

	/* restore the loopback state requested by the owner */
	btif_hw_loopback(btif, btif->lpbk);

	ret = request_irq(btif->irq, btif_isr, IRQF_NO_AUTOEN, "mtk-btif",
			  btif);
	if (ret) {
		dev_err(btif->dev, "failed to request BTIF irq %d: %d\n",
			btif->irq, ret);
		btif_clk_disable(btif);
		return ret;
	}
	btif->irq_registered = true;

	return 0;
}

static void btif_controller_free(struct mtk_btif *btif)
{
	if (btif->irq_registered) {
		free_irq(btif->irq, btif);
		btif->irq_registered = false;
	}
	btif_clk_disable(btif);
}

/* Downstream mtk_btif.c:1236-1247 */
static int btif_tx_pio_setup(struct mtk_btif *btif)
{
	btif->tx_mode = BTIF_MODE_PIO;
	return btif_hw_tx_mode(btif, BTIF_MODE_PIO);
}

/* Downstream mtk_btif.c:1324-1367 */
static int btif_tx_dma_setup(struct mtk_btif *btif)
{
	struct btif_dma_chan *chan = &btif->tx_dma;
	int ret;

	btif_dma_vfifo_reset(chan);

	ret = btif_apdma_clk_enable(btif);
	if (ret) {
		dev_err(btif->dev, "failed to enable APDMA clock: %d\n", ret);
		return ret;
	}

	btif_dma_hw_init(chan);

	ret = btif_dma_ctrl(chan, true);
	if (ret) {
		btif_apdma_clk_disable(btif);
		return ret;
	}

	btif_hw_tx_mode(btif, BTIF_MODE_DMA);
	btif->tx_mode = BTIF_MODE_DMA;

	ret = request_irq(chan->irq, btif_dma_tx_irq, 0, "mtk-btif-tx", chan);
	if (ret) {
		dev_err(btif->dev, "failed to request TX DMA irq %d: %d\n",
			chan->irq, ret);
		btif_dma_ctrl(chan, false);
		btif_apdma_clk_disable(btif);
		return ret;
	}
	chan->irq_registered = true;

	return 0;
}

/* Downstream mtk_btif.c:1368-1379 */
static int btif_tx_dma_free(struct mtk_btif *btif)
{
	struct btif_dma_chan *chan = &btif->tx_dma;

	if (chan->irq_registered) {
		free_irq(chan->irq, chan);
		chan->irq_registered = false;
	}
	btif_dma_ctrl(chan, false);
	btif_apdma_clk_disable(btif);

	return 0;
}

static int btif_tx_setup(struct mtk_btif *btif)
{
	int ret;

	if (btif->tx_mode != BTIF_MODE_DMA)
		return btif_tx_pio_setup(btif);

	ret = btif_tx_dma_setup(btif);
	if (!ret)
		return 0;

	dev_warn(btif->dev, "TX DMA setup failed (%d), using PIO\n", ret);
	return btif_tx_pio_setup(btif);
}

static int btif_tx_free(struct mtk_btif *btif)
{
	if (btif->tx_mode == BTIF_MODE_DMA)
		return btif_tx_dma_free(btif);
	return 0;
}

/* Downstream mtk_btif.c:1248-1261 */
static int btif_rx_pio_setup(struct mtk_btif *btif)
{
	btif->rx_mode = BTIF_MODE_PIO;
	return btif_hw_rx_mode(btif, BTIF_MODE_PIO);
}

/* Downstream mtk_btif.c:1262-1310 */
static int btif_rx_dma_setup(struct mtk_btif *btif)
{
	struct btif_dma_chan *chan = &btif->rx_dma;
	int ret;

	btif_dma_vfifo_reset(chan);

	ret = btif_apdma_clk_enable(btif);
	if (ret) {
		dev_err(btif->dev, "failed to enable APDMA clock: %d\n", ret);
		return btif_rx_pio_setup(btif);
	}

	btif_dma_hw_init(chan);

	ret = btif_dma_ctrl(chan, true);
	if (ret) {
		btif_apdma_clk_disable(btif);
		return btif_rx_pio_setup(btif);
	}

	btif_hw_rx_mode(btif, BTIF_MODE_DMA);
	btif->rx_mode = BTIF_MODE_DMA;

	ret = request_irq(chan->irq, btif_dma_rx_irq, 0, "mtk-btif-rx", chan);
	if (ret) {
		dev_err(btif->dev, "failed to request RX DMA irq %d: %d\n",
			chan->irq, ret);
		btif_dma_ctrl(chan, false);
		btif_apdma_clk_disable(btif);
		return btif_rx_pio_setup(btif);
	}
	chan->irq_registered = true;

	return 0;
}

/* Downstream mtk_btif.c:1311-1323 */
static int btif_rx_dma_free(struct mtk_btif *btif)
{
	struct btif_dma_chan *chan = &btif->rx_dma;

	if (chan->irq_registered) {
		free_irq(chan->irq, chan);
		chan->irq_registered = false;
	}
	btif_dma_ctrl(chan, false);
	btif_apdma_clk_disable(btif);

	return 0;
}

/*
 * Downstream enables the BTIF interrupt line only in the RX PIO setup path
 * (mtk_btif.c:1248-1255).  The TX PIO path needs the same line for its
 * TX-empty interrupt, so unmask it whenever either direction is PIO
 * [INFERENCE].
 */
static int btif_rx_setup(struct mtk_btif *btif)
{
	int ret;

	if (btif->rx_mode == BTIF_MODE_DMA) {
		ret = btif_rx_dma_setup(btif);
		if (ret)
			dev_warn(btif->dev, "RX DMA setup failed (%d)\n", ret);
	} else {
		ret = btif_rx_pio_setup(btif);
	}

	if (btif->irq_registered &&
	    (btif->tx_mode == BTIF_MODE_PIO || btif->rx_mode == BTIF_MODE_PIO))
		enable_irq(btif->irq);

	return ret;
}

static int btif_rx_free(struct mtk_btif *btif)
{
	if (btif->rx_mode == BTIF_MODE_DMA)
		return btif_rx_dma_free(btif);
	return 0;
}

/* Downstream mtk_btif.c:1488-1522 (_btif_init) */
static int btif_hw_start(struct mtk_btif *btif)
{
	int ret;

	ret = btif_controller_setup(btif);
	if (ret)
		return ret;

	ret = btif_tx_setup(btif);
	if (ret) {
		btif_controller_free(btif);
		return ret;
	}

	ret = btif_rx_setup(btif);
	if (ret) {
		btif_tx_free(btif);
		btif_controller_free(btif);
		return ret;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* State machine                                                      */
/* ------------------------------------------------------------------ */

/* Downstream mtk_btif.c:1204-1241 / btif_dma_plat.c:883-906 */
static bool btif_is_tx_complete(struct mtk_btif *btif)
{
	if (btif->tx_mode == BTIF_MODE_DMA &&
	    !btif_dma_is_tx_complete(&btif->tx_dma))
		return false;

	return btif_hw_is_tx_complete(btif);
}

/*
 * Downstream mtk_btif.c:1712-1763 (_btif_enter_dpidle_from_on): wait up to
 * 5 s for the TX path to drain, then gate all clocks.  The downstream loop
 * counts 5000 msleep(1) iterations; a real deadline is used here because
 * msleep(1) only guarantees a lower bound.
 */
static int btif_enter_dpidle_from_on(struct mtk_btif *btif)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), 5000);

	while (!btif_is_tx_complete(btif)) {
		if (ktime_after(ktime_get(), deadline)) {
			dev_warn(btif->dev,
				 "BTIF TX busy, deep-idle entry refused\n");
			return -EBUSY;
		}
		usleep_range(1000, 2000);
	}

	if (btif->tx_mode == BTIF_MODE_DMA)
		btif_apdma_clk_disable(btif);
	if (btif->rx_mode == BTIF_MODE_DMA)
		btif_apdma_clk_disable(btif);
	btif_clk_disable(btif);

	return 0;
}

static int btif_enter_dpidle(struct mtk_btif *btif)
{
	int ret = 0;

	switch (btif->state) {
	case B_S_ON:
		ret = btif_enter_dpidle_from_on(btif);
		break;
	case B_S_SUSPEND:
		ret = btif_hw_start(btif);
		if (!ret)
			ret = btif_enter_dpidle_from_on(btif);
		break;
	case B_S_DPIDLE:
		return 0;
	default:
		dev_warn(btif->dev, "cannot enter deep idle from %s\n",
			 btif_state_name[btif->state]);
		return E_BTIF_INVAL_STATE;
	}

	if (!ret)
		btif->state = B_S_DPIDLE;

	return ret;
}

/* Downstream mtk_btif.c:1670-1692 / 1693-1701 */
static int btif_exit_dpidle(struct mtk_btif *btif)
{
	int ret = 0;

	switch (btif->state) {
	case B_S_DPIDLE:
		if (btif->tx_mode == BTIF_MODE_DMA) {
			ret = btif_apdma_clk_enable(btif);
			if (ret)
				return ret;
		}
		if (btif->rx_mode == BTIF_MODE_DMA) {
			ret = btif_apdma_clk_enable(btif);
			if (ret) {
				if (btif->tx_mode == BTIF_MODE_DMA)
					btif_apdma_clk_disable(btif);
				return ret;
			}
		}
		ret = btif_clk_enable(btif);
		if (ret)
			return ret;
		break;
	case B_S_SUSPEND:
	case B_S_OFF:
		ret = btif_hw_start(btif);
		break;
	case B_S_ON:
		return 0;
	default:
		return E_BTIF_INVAL_PARAM;
	}

	if (!ret)
		btif->state = B_S_ON;

	return ret;
}

/* ------------------------------------------------------------------ */
/* Transmit path                                                      */
/* ------------------------------------------------------------------ */

/* Downstream mtk_btif.c:2882-2933 (_btif_dma_write) */
static int btif_dma_write(struct mtk_btif *btif, const u8 *buf,
			  unsigned int len)
{
	struct btif_dma_chan *chan = &btif->tx_dma;
	int ret;
	int retry;

	for (retry = 0; retry < 10; retry++) {
		if (!btif_dma_is_tx_allow(chan))
			continue;

		ret = btif_dma_tx_room(chan);
		if (ret < (int)len)
			return E_BTIF_AGAIN;

		return btif_dma_send(chan, buf, len);
	}

	dev_warn_ratelimited(btif->dev, "BTIF TX DMA not allowed\n");
	return E_BTIF_AGAIN;
}

/* Downstream mtk_btif.c:2938-2968 (_btif_pio_write) */
static int btif_pio_write(struct mtk_btif *btif, const u8 *buf,
			  unsigned int len)
{
	unsigned int sent = 0;
	unsigned int retry = 0;
	int ret;

	while (sent < len && retry < 10) {
		ret = btif_hw_send(btif, buf + sent, len - sent);
		if (ret > 0) {
			sent += ret;
			retry = 0;
		} else {
			retry++;
		}
	}

	return sent;
}

/* Downstream mtk_btif.c:2897-2969 (_btif_send_data) */
static int btif_send(struct mtk_btif *btif, const u8 *buf, unsigned int len)
{
	int ret;

	if (!btif->enabled)
		return E_BTIF_NOT_OPEN;

	mutex_lock(&btif->tx_mtx);
	mutex_lock(&btif->state_mtx);
	if (btif->state == B_S_ON)
		ret = 0;
	else
		ret = btif_exit_dpidle(btif);

	if (ret) {
		mutex_unlock(&btif->state_mtx);
		mutex_unlock(&btif->tx_mtx);
		return E_BTIF_INVAL_STATE;
	}

	if (btif->tx_mode == BTIF_MODE_DMA)
		ret = btif_dma_write(btif, buf, len);
	else
		ret = btif_pio_write(btif, buf, len);

	if (ret > 0)
		btif_log_record(&btif->tx_log, buf, ret);

	mutex_unlock(&btif->state_mtx);
	mutex_unlock(&btif->tx_mtx);

	return ret;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                       */
/* ------------------------------------------------------------------ */

/* Downstream mtk_btif.c:1524-1553 (btif_open) */
static int btif_enable(struct mtk_btif *btif)
{
	int ret;

	if (btif->enabled)
		return E_BTIF_ALREADY_OPEN;

	mutex_lock(&btif->state_mtx);
	ret = btif_hw_start(btif);
	if (!ret) {
		btif->enabled = true;
		btif->state = B_S_ON;
	}
	btif_log_clear(&btif->tx_log);
	btif_log_clear(&btif->rx_log);
	btif_log_set(&btif->tx_log, true);
	btif_log_set(&btif->rx_log, true);
	mutex_unlock(&btif->state_mtx);

	dev_info(btif->dev, "BTIF opened, tx %s rx %s\n",
		 btif->tx_mode == BTIF_MODE_DMA ? "DMA" : "PIO",
		 btif->rx_mode == BTIF_MODE_DMA ? "DMA" : "PIO");

	return ret;
}

/* Downstream mtk_btif.c:1555-1588 (btif_close) */
static int btif_disable(struct mtk_btif *btif)
{
	if (!btif->enabled)
		return E_BTIF_NOT_OPEN;

	mutex_lock(&btif->state_mtx);

	/* return to ON so the teardown runs with live clocks */
	btif_exit_dpidle(btif);

	btif->enabled = false;
	btif_tx_free(btif);
	btif_rx_free(btif);
	btif_controller_free(btif);

	btif->rx_cb = NULL;
	btif->lpbk = false;
	btif->state = B_S_OFF;
	mutex_unlock(&btif->state_mtx);

	btif_log_set(&btif->tx_log, false);
	btif_log_set(&btif->rx_log, false);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Exported API                                                       */
/* ------------------------------------------------------------------ */

static struct btif_user *btif_user_find(unsigned long id)
{
	struct mtk_btif *btif = g_btif;

	if (!btif || !btif->owner || btif->owner->id != id)
		return NULL;

	return btif->owner;
}

int mtk_wcn_btif_open(char *owner, unsigned long *id)
{
	struct mtk_btif *btif = g_btif;
	struct btif_user *user;
	int ret;

	if (!btif)
		return E_BTIF_BAD_POINTER;
	if (!owner || !id) {
		if (id)
			*id = 0;
		return E_BTIF_INVAL_PARAM;
	}

	mutex_lock(&btif->ops_mtx);
	if (btif->owner) {
		dev_warn(btif->dev, "BTIF already owned by %s\n",
			 btif->owner->name);
		ret = E_BTIF_ALREADY_OPEN;
		goto out;
	}

	user = kzalloc_obj(*user);
	if (!user) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btif_enable(btif);
	if (ret) {
		kfree(user);
		goto out;
	}

	strscpy(user->name, owner, sizeof(user->name));
	user->id = (unsigned long)user;
	user->btif = btif;
	btif->owner = user;
	*id = user->id;

out:
	mutex_unlock(&btif->ops_mtx);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_open);

int mtk_wcn_btif_close(unsigned long id)
{
	struct mtk_btif *btif = g_btif;
	int ret = E_BTIF_INVAL_PARAM;

	if (!btif)
		return E_BTIF_BAD_POINTER;

	mutex_lock(&btif->ops_mtx);
	if (!btif->owner || btif->owner->id != id)
		goto out;

	ret = btif_disable(btif);
	kfree(btif->owner);
	btif->owner = NULL;

out:
	mutex_unlock(&btif->ops_mtx);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_close);

int mtk_wcn_btif_write(unsigned long id, const unsigned char *buf,
		       unsigned int len)
{
	struct btif_user *user = btif_user_find(id);

	if (!user)
		return E_BTIF_INVAL_PARAM;
	if (!buf)
		return E_BTIF_INVAL_PARAM;
	if (!len || len > BTIF_MAX_LEN_PER_PKT) {
		dev_err(user->btif->dev, "invalid BTIF TX length %u\n", len);
		return E_BTIF_INVAL_PARAM;
	}

	return btif_send(user->btif, buf, len);
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_write);

int mtk_wcn_btif_read(unsigned long id, unsigned char *buf, unsigned int len)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	unsigned long flags;
	unsigned int count;
	unsigned int chunk;
	unsigned int tail;

	if (!user)
		return E_BTIF_INVAL_PARAM;
	btif = user->btif;
	if (!buf || !len)
		return E_BTIF_INVAL_PARAM;

	/* owners using a callback never read from the staging ring */
	if (btif->rx_cb)
		return E_BTIF_INVAL_STATE;

	mutex_lock(&btif->rx_mtx);
	spin_lock_irqsave(&btif->rx_lock, flags);
	count = btif_rx_ring_count(btif);
	chunk = min(count, len);
	tail = min(chunk, btif->rx_size - btif->rx_rd);
	if (chunk) {
		memcpy(buf, btif->rx_buf + btif->rx_rd, tail);
		if (chunk > tail)
			memcpy(buf + tail, btif->rx_buf, chunk - tail);
		btif->rx_rd = (btif->rx_rd + chunk) & (btif->rx_size - 1);
	}
	spin_unlock_irqrestore(&btif->rx_lock, flags);
	mutex_unlock(&btif->rx_mtx);

	return chunk;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_read);

int mtk_wcn_btif_dpidle_ctrl(unsigned long id,
			     enum mtk_btif_dpidle_ctrl flag)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	int ret;

	if (!user)
		return E_BTIF_INVAL_PARAM;
	btif = user->btif;
	if (!btif->enabled)
		return E_BTIF_NOT_OPEN;

	mutex_lock(&btif->state_mtx);
	if (flag == BTIF_DPIDLE_DISABLE)
		ret = btif_exit_dpidle(btif);
	else
		ret = btif_enter_dpidle(btif);
	mutex_unlock(&btif->state_mtx);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_dpidle_ctrl);

int mtk_wcn_btif_rx_cb_register(unsigned long id, mtk_btif_rx_cb rx_cb)
{
	struct btif_user *user = btif_user_find(id);

	if (!user)
		return E_BTIF_INVAL_PARAM;
	if (!rx_cb)
		return E_BTIF_INVAL_PARAM;

	if (user->btif->rx_cb && user->btif->rx_cb != rx_cb)
		dev_warn(user->btif->dev, "replacing BTIF RX callback\n");

	user->btif->rx_cb = rx_cb;

	/* deliver anything that arrived before the callback was registered */
	complete(&user->btif->rx_comp);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_rx_cb_register);

int mtk_wcn_btif_wakeup_consys(unsigned long id)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	int ret;

	if (!user)
		return E_BTIF_INVAL_PARAM;
	btif = user->btif;
	if (!btif->enabled)
		return E_BTIF_NOT_OPEN;

	/*
	 * The WAK register lives in the BTIF block, so its clock must be on
	 * even when the channel is in deep idle.  Downstream does the same
	 * around hal_btif_raise_wak_sig() (mtk_btif.c:1779-1795).
	 */
	mutex_lock(&btif->state_mtx);
	ret = btif_clk_enable(btif);
	if (!ret) {
		ret = btif_hw_raise_wake(btif);
		btif_clk_disable(btif);
	}
	mutex_unlock(&btif->state_mtx);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_wakeup_consys);

int mtk_wcn_btif_loopback_ctrl(unsigned long id,
			       enum mtk_btif_lpbk_mode enable)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	bool en;
	int ret;

	if (!user)
		return E_BTIF_INVAL_PARAM;
	btif = user->btif;
	if (!btif->enabled)
		return E_BTIF_NOT_OPEN;
	en = enable == BTIF_LPBK_ENABLE;

	mutex_lock(&btif->state_mtx);
	ret = btif_exit_dpidle(btif);
	if (!ret) {
		ret = btif_hw_loopback(btif, en);
		if (!ret) {
			btif->lpbk = en;
			dev_info(btif->dev, "BTIF loopback %s\n",
				 en ? "enabled" : "disabled");
		}
	} else {
		ret = E_BTIF_INVAL_STATE;
	}
	mutex_unlock(&btif->state_mtx);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_loopback_ctrl);

int mtk_wcn_btif_dbg_ctrl(unsigned long id, enum mtk_btif_dbg_id flag)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	enum btif_state saved;
	int ret = 0;

	if (!user)
		return E_BTIF_INVAL_PARAM;
	btif = user->btif;

	switch (flag) {
	case BTIF_DISABLE_LOGGER:
		btif_log_set(&btif->tx_log, false);
		btif_log_set(&btif->rx_log, false);
		break;
	case BTIF_ENABLE_LOGGER:
		btif_log_set(&btif->tx_log, true);
		btif_log_set(&btif->rx_log, true);
		break;
	case BTIF_DUMP_LOG:
		btif_log_dump(btif, &btif->tx_log);
		btif_log_dump(btif, &btif->rx_log);
		break;
	case BTIF_CLR_LOG:
		btif_log_clear(&btif->tx_log);
		btif_log_clear(&btif->rx_log);
		break;
	case BTIF_DUMP_BTIF_REG:
		mutex_lock(&btif->state_mtx);
		saved = btif->state;
		if (saved == B_S_OFF) {
			ret = E_BTIF_INVAL_STATE;
		} else {
			if (saved != B_S_ON)
				btif_exit_dpidle(btif);
			btif_hw_dump_regs(btif);
			if (btif->tx_mode == BTIF_MODE_DMA)
				btif_dma_dump_regs(&btif->tx_dma);
			if (btif->rx_mode == BTIF_MODE_DMA)
				btif_dma_dump_regs(&btif->rx_dma);
			if (saved != B_S_ON)
				btif_enter_dpidle(btif);
		}
		mutex_unlock(&btif->state_mtx);
		break;
	case BTIF_ENABLE_RT_LOG:
		btif_log_set_realtime(&btif->tx_log, true);
		btif_log_set_realtime(&btif->rx_log, true);
		break;
	case BTIF_DISABLE_RT_LOG:
		btif_log_set_realtime(&btif->tx_log, false);
		btif_log_set_realtime(&btif->rx_log, false);
		break;
	default:
		ret = E_BTIF_INVAL_PARAM;
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_dbg_ctrl);

/*
 * Downstream btif_parser_wmt_evt() (mtk_btif.c:1102-1166) copies the whole
 * staging buffer and searches for the supplied byte sequence without
 * consuming it; it is used by WMT to verify that a known sleep/wakeup event
 * went through the RX path (wmt_ctrl.c:948-975).
 */
bool mtk_wcn_btif_parser_wmt_evt(unsigned long id, const unsigned char *buf,
				 unsigned int len)
{
	struct btif_user *user = btif_user_find(id);
	struct mtk_btif *btif;
	unsigned long flags;
	unsigned int count;
	unsigned int alloc;
	unsigned int tail;
	unsigned int i;
	u8 *snapshot;
	bool found = false;

	if (!user || !buf || !len)
		return false;
	btif = user->btif;

	mutex_lock(&btif->rx_mtx);
	spin_lock_irqsave(&btif->rx_lock, flags);
	count = btif_rx_ring_count(btif);
	spin_unlock_irqrestore(&btif->rx_lock, flags);
	if (count < len) {
		mutex_unlock(&btif->rx_mtx);
		return false;
	}

	alloc = count;
	snapshot = kmalloc(alloc, GFP_KERNEL);
	if (!snapshot) {
		mutex_unlock(&btif->rx_mtx);
		return false;
	}

	spin_lock_irqsave(&btif->rx_lock, flags);
	count = min(btif_rx_ring_count(btif), alloc);
	tail = min(count, btif->rx_size - btif->rx_rd);
	memcpy(snapshot, btif->rx_buf + btif->rx_rd, tail);
	if (count > tail)
		memcpy(snapshot + tail, btif->rx_buf, count - tail);
	spin_unlock_irqrestore(&btif->rx_lock, flags);

	for (i = 0; i + len <= count; i++) {
		if (!memcmp(snapshot + i, buf, len)) {
			found = true;
			break;
		}
	}

	kfree(snapshot);
	mutex_unlock(&btif->rx_mtx);

	dev_info(btif->dev, "WMT event parser %s at %u of %u bytes\n",
		 found ? "match" : "miss", found ? i : 0, count);

	return found;
}
EXPORT_SYMBOL_GPL(mtk_wcn_btif_parser_wmt_evt);

/* ------------------------------------------------------------------ */
/* Platform driver                                                    */
/* ------------------------------------------------------------------ */

static int btif_dma_chan_init(struct mtk_btif *btif,
			      struct btif_dma_chan *chan,
			      enum btif_dma_dir dir, unsigned int size,
			      unsigned int thre, void __iomem *base, int irq)
{
	chan->btif = btif;
	chan->dir = dir;
	chan->base = base;
	chan->irq = irq;
	chan->irq_registered = false;
	chan->vfifo.size = size;
	chan->vfifo.thre = thre;
	chan->vfifo.vir = dma_alloc_coherent(btif->dev, size,
					     &chan->vfifo.phys, GFP_KERNEL);
	if (!chan->vfifo.vir)
		return -ENOMEM;
	memset(chan->vfifo.vir, 0, size);

	return 0;
}

static void btif_dma_chan_deinit(struct mtk_btif *btif,
				 struct btif_dma_chan *chan)
{
	if (!chan->vfifo.vir)
		return;
	dma_free_coherent(btif->dev, chan->vfifo.size, chan->vfifo.vir,
			  chan->vfifo.phys);
	chan->vfifo.vir = NULL;
}

static int mtk_btif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_btif *btif;
	void __iomem *tx_base;
	void __iomem *rx_base;
	int tx_irq;
	int rx_irq;
	int ret;

	if (g_btif)
		return -EBUSY;

	btif = devm_kzalloc(dev, sizeof(*btif), GFP_KERNEL);
	if (!btif)
		return -ENOMEM;
	btif->dev = dev;
	btif->tx_mode = BTIF_MODE_DMA;
	btif->rx_mode = BTIF_MODE_DMA;
	btif->state = B_S_OFF;

	mutex_init(&btif->state_mtx);
	mutex_init(&btif->ops_mtx);
	mutex_init(&btif->tx_mtx);
	mutex_init(&btif->rx_mtx);
	spin_lock_init(&btif->rx_lock);
	spin_lock_init(&btif->tx_lock);
	init_completion(&btif->rx_comp);

	btif->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(btif->base))
		return PTR_ERR(btif->base);

	btif->irq = platform_get_irq(pdev, 0);
	if (btif->irq < 0)
		return btif->irq;

	btif->clk_btif = devm_clk_get(dev, "btifc");
	if (IS_ERR(btif->clk_btif))
		return dev_err_probe(dev, PTR_ERR(btif->clk_btif),
				     "failed to get btifc clock\n");

	btif->clk_apdma = devm_clk_get(dev, "apdmac");
	if (IS_ERR(btif->clk_apdma))
		return dev_err_probe(dev, PTR_ERR(btif->clk_apdma),
				     "failed to get apdmac clock\n");

	/*
	 * The APDMA vFIFO base is programmed through a 32-bit register and the
	 * downstream driver allocates it with GFP_DMA32 (mtk_btif.c:1862-1864), so
	 * the controller only addresses the low 4 GiB [INFERENCE].
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = clk_prepare(btif->clk_btif);
	if (ret)
		return ret;
	ret = clk_prepare(btif->clk_apdma);
	if (ret) {
		clk_unprepare(btif->clk_btif);
		return ret;
	}

	btif->rx_buf = devm_kzalloc(dev, BTIF_RX_BUFFER_SIZE, GFP_KERNEL);
	if (!btif->rx_buf) {
		ret = -ENOMEM;
		goto err_clk;
	}
	btif->rx_size = BTIF_RX_BUFFER_SIZE;

	btif->tx_fifo = devm_kzalloc(dev, sizeof(*btif->tx_fifo), GFP_KERNEL);
	if (!btif->tx_fifo) {
		ret = -ENOMEM;
		goto err_clk;
	}
	ret = kfifo_alloc(btif->tx_fifo, BTIF_TX_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto err_clk;

	tx_base = devm_platform_ioremap_resource(pdev, 1);
	rx_base = devm_platform_ioremap_resource(pdev, 2);
	tx_irq = platform_get_irq(pdev, 1);
	rx_irq = platform_get_irq(pdev, 2);
	if (IS_ERR(tx_base) || IS_ERR(rx_base) || tx_irq < 0 || rx_irq < 0) {
		dev_err(dev, "missing APDMA resources\n");
		ret = -EINVAL;
		goto err_kfifo;
	}

	ret = btif_dma_chan_init(btif, &btif->tx_dma, BTIF_DMA_TX,
				 BTIF_TX_DMA_VFIFO_SIZE,
				 BTIF_DMA_TX_THRE(BTIF_TX_DMA_VFIFO_SIZE),
				 tx_base, tx_irq);
	if (ret)
		goto err_kfifo;

	ret = btif_dma_chan_init(btif, &btif->rx_dma, BTIF_DMA_RX,
				 BTIF_RX_DMA_VFIFO_SIZE,
				 BTIF_DMA_RX_THRE(BTIF_RX_DMA_VFIFO_SIZE),
				 rx_base, rx_irq);
	if (ret)
		goto err_tx_dma;

	btif_log_init(&btif->tx_log, "tx",
		      devm_kcalloc(dev, BTIF_LOG_ENTRY_NUM,
				   sizeof(struct btif_log_entry), GFP_KERNEL));
	btif_log_init(&btif->rx_log, "rx",
		      devm_kcalloc(dev, BTIF_LOG_ENTRY_NUM,
				   sizeof(struct btif_log_entry), GFP_KERNEL));
	if (!btif->tx_log.entries || !btif->rx_log.entries) {
		ret = -ENOMEM;
		goto err_rx_dma;
	}

	btif->rx_thread = kthread_run(btif_rx_thread, btif, "btif_rxd");
	if (IS_ERR(btif->rx_thread)) {
		ret = PTR_ERR(btif->rx_thread);
		btif->rx_thread = NULL;
		goto err_rx_dma;
	}

	platform_set_drvdata(pdev, btif);
	g_btif = btif;

	dev_info(dev, "BTIF registered (irq %d, tx dma irq %d, rx dma irq %d)\n",
		 btif->irq, btif->tx_dma.irq, btif->rx_dma.irq);
	return 0;

err_rx_dma:
	btif_dma_chan_deinit(btif, &btif->rx_dma);
err_tx_dma:
	btif_dma_chan_deinit(btif, &btif->tx_dma);
err_kfifo:
	kfifo_free(btif->tx_fifo);
err_clk:
	clk_unprepare(btif->clk_apdma);
	clk_unprepare(btif->clk_btif);
	return ret;
}

static void mtk_btif_remove(struct platform_device *pdev)
{
	struct mtk_btif *btif = platform_get_drvdata(pdev);

	if (!btif)
		return;

	g_btif = NULL;
	platform_set_drvdata(pdev, NULL);

	if (btif->rx_thread)
		kthread_stop(btif->rx_thread);

	mutex_lock(&btif->ops_mtx);
	if (btif->enabled) {
		dev_warn(btif->dev, "BTIF still open at remove, forcing down\n");
		btif_disable(btif);
	}
	kfree(btif->owner);
	btif->owner = NULL;
	mutex_unlock(&btif->ops_mtx);

	btif_dma_chan_deinit(btif, &btif->rx_dma);
	btif_dma_chan_deinit(btif, &btif->tx_dma);
	kfifo_free(btif->tx_fifo);
	clk_unprepare(btif->clk_apdma);
	clk_unprepare(btif->clk_btif);
}

/*
 * Downstream mtk_btif.c:259-435 keeps the controller off across system
 * suspend and re-initialises it on resume; a busy channel refuses the
 * transition so that a pending transfer is not torn down.
 */
static int mtk_btif_suspend(struct device *dev)
{
	struct mtk_btif *btif = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&btif->state_mtx);
	if (btif->enabled) {
		if (btif->state == B_S_ON) {
			dev_err(dev, "BTIF busy, refusing suspend\n");
			ret = -EBUSY;
		} else {
			btif_exit_dpidle(btif);
			btif_tx_free(btif);
			btif_rx_free(btif);
			btif_controller_free(btif);
			btif->state = B_S_SUSPEND;
		}
	}
	mutex_unlock(&btif->state_mtx);

	return ret;
}

static int mtk_btif_resume(struct device *dev)
{
	struct mtk_btif *btif = dev_get_drvdata(dev);

	mutex_lock(&btif->state_mtx);
	if (btif->enabled && btif->state == B_S_SUSPEND)
		btif_enter_dpidle(btif);
	mutex_unlock(&btif->state_mtx);

	return 0;
}

static const struct dev_pm_ops mtk_btif_pm_ops = {
	.suspend = mtk_btif_suspend,
	.resume = mtk_btif_resume,
};

static const struct of_device_id mtk_btif_of_match[] = {
	{ .compatible = "mediatek,btif" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_btif_of_match);

static struct platform_driver mtk_btif_driver = {
	.probe = mtk_btif_probe,
	.remove = mtk_btif_remove,
	.driver = {
		.name = "mtk_btif",
		.of_match_table = mtk_btif_of_match,
		.pm = &mtk_btif_pm_ops,
	},
};
module_platform_driver(mtk_btif_driver);

MODULE_AUTHOR("MBJ/WCN/SE/SS1/Chaozhong.Liang");
MODULE_DESCRIPTION("MediaTek BTIF driver");
MODULE_LICENSE("GPL");
