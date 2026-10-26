// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI CCIF control transport.
 *
 * The CCIF block is the doorbell/exception path between AP and modem:
 *   APCCIF_CON     arbitration enable
 *   APCCIF_BUSY    bitmask of channels currently owned by AP
 *   APCCIF_START   bitmask of channels granted to AP
 *   APCCIF_TCHNUM  write channel id to raise an AP->MD interrupt
 *   APCCIF_RCHNUM  channel ids raised by the MD
 *   APCCIF_ACK     write back RCHNUM to acknowledge
 *   APCCIF_CHDATA  512 byte shared SRAM window
 *
 * Two data paths are supported:
 *   - CLDMA modems (MD1): data runs over the CLDMA engine, CCIF only carries
 *     the exception handshake and the SRAM runtime block.
 *   - CCIF-ring modems (MD3/C2K): eight framed rings in shared memory, each
 *     with a doorbell channel H2D_RINGQ0+n / D2H_RINGQ0+n.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/{modem_ccif.c,modem_cldma.c,modem_ccif_c2k.c},
 * mt6735/ccif_c2k_platform.h.  Original copyright (C) 2015 MediaTek Inc.,
 * GPL-2.0.
 */
#include <linux/aee.h>
#include <linux/ccci.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqreturn.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/seq_file.h>

#include "ccci_priv.h"

#define CCCI_CCIF_RING_MASK	(GENMASK(D2H_RINGQ0 + CCCI_MAX_QUEUES - 1, \
					 D2H_RINGQ0))

#define CCIF_IRQ_POLL_MS	20	/* downstream polling_ready(): 20ms */
#define CCIF_IRQ_POLL_CNT	500	/* 500 * 20ms = 10s, MD timeout */

/* queue sizes in bytes, downstream modem_ccif.c:104 and
 * modem_ccif_c2k.c:146
 */
static const unsigned int ccif_rx_size_md1[CCCI_MAX_QUEUES] = {
	32 * 1024, 100 * 1024, 100 * 1024, 100 * 1024,
	16 * 1024, 16 * 1024, 16 * 1024, 16 * 1024,
};
static const unsigned int ccif_tx_size_md1[CCCI_MAX_QUEUES] = {
	32 * 1024, 100 * 1024, 16 * 1024, 100 * 1024,
	16 * 1024, 16 * 1024, 16 * 1024, 16 * 1024,
};
static const unsigned int ccif_rx_size_md3[CCCI_MAX_QUEUES] = {
	10 * 1024, 100 * 1024, 100 * 1024, 150 * 1024,
	50 * 1024, 10 * 1024, 10 * 1024, 10 * 1024,
};
static const unsigned int ccif_tx_size_md3[CCCI_MAX_QUEUES] = {
	10 * 1024, 100 * 1024, 50 * 1024, 50 * 1024,
	50 * 1024, 10 * 1024, 10 * 1024, 10 * 1024,
};

static const unsigned int *ccif_rx_sizes(struct ccci_md *md)
{
	return md->id == CCCI_MD_SYS1 ? ccif_rx_size_md1 : ccif_rx_size_md3;
}

static const unsigned int *ccif_tx_sizes(struct ccci_md *md)
{
	return md->id == CCCI_MD_SYS1 ? ccif_tx_size_md1 : ccif_tx_size_md3;
}

size_t ccci_ccif_ring_total_size(void)
{
	size_t md1 = 0, md3 = 0;
	unsigned int i;

	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		md1 += CCCI_RINGBUF_CTL_LEN + ccif_rx_size_md1[i] +
		       ccif_tx_size_md1[i];
		md3 += CCCI_RINGBUF_CTL_LEN + ccif_rx_size_md3[i] +
		       ccif_tx_size_md3[i];
	}
	return max(md1, md3);
}

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */
static inline u32 ccif_rd(struct ccci_ccif *ccif, u32 reg)
{
	return readl(ccif->ap_base + reg);
}

static inline void ccif_wr(struct ccci_ccif *ccif, u32 reg, u32 val)
{
	writel(val, ccif->ap_base + reg);
}

void ccif_enable_irq(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;

	if (!ccif || !ccif->started)
		return;
	if (ccif->irq_enabled)
		return;
	enable_irq(ccif->irq);
	if (ccif->wdt_irq)
		enable_irq(ccif->wdt_irq);
	ccif->irq_enabled = true;
}

void ccif_wdt_irq(struct ccci_md *md, bool enable)
{
	struct ccci_ccif *ccif = md->ccif;

	if (!ccif || !ccif->wdt_irq)
		return;
	if (enable)
		enable_irq(ccif->wdt_irq);
	else
		disable_irq_nosync(ccif->wdt_irq);
}

void ccif_disable_irq(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;

	if (!ccif || !ccif->irq_enabled)
		return;
	disable_irq_nosync(ccif->irq);
	if (ccif->wdt_irq)
		disable_irq(ccif->wdt_irq);
	ccif->irq_enabled = false;
}

/**
 * ccci_ccif_send - raise one CCIF channel towards the modem
 *
 * Downstream returns -1 immediately when the channel is busy; the modem needs
 * the previous transfer to complete, so wait for the BUSY bit to clear with a
 * bounded timeout instead.
 */
int ccci_ccif_send(struct ccci_md *md, unsigned int channel)
{
	struct ccci_ccif *ccif = md->ccif;
	unsigned int i;
	u32 busy;

	if (!ccif || !ccif->ap_base)
		return -ENODEV;
	if (channel >= CCCI_CCIF_CH_NUM)
		return -EINVAL;

	for (i = 0; i < CCCI_CCIF_SEND_RETRIES; i++) {
		busy = ccif_rd(ccif, APCCIF_BUSY);
		if (!(busy & BIT(channel)))
			break;
		/* udelay: this path also runs from the softirq/tasklet Rx */
		udelay(10);
	}
	if (i == CCCI_CCIF_SEND_RETRIES) {
		ccci_md_dbg(md, "ccif channel %u busy timeout\n", channel);
		return -EBUSY;
	}

	ccif_wr(ccif, APCCIF_BUSY, BIT(channel));
	ccif_wr(ccif, APCCIF_TCHNUM, channel);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ring transport                                                      */
/* ------------------------------------------------------------------ */
static void ccci_ccif_rx_tasklet(struct tasklet_struct *t)
{
	struct ccci_ccif_queue *q = from_tasklet(q, t, rx_task);
	struct ccci_md *md = q->md;
	struct ccci_ringbuf *rb = q->rx_ring;
	struct sk_buff *skb;
	int len, ret, count = 0;

	if (!rb)
		return;

	while (count++ < 32) {
		len = ccci_ringbuf_readable(rb);
		if (len < 0) {
			if (len != -7)	/* -CCCI_RINGBUF_EMPTY */
				ccci_md_err(md, "q%u ring read error %d\n",
					    q->index, len);
			return;
		}

		skb = ccci_alloc_skb(len + CCCI_HEADER_SIZE, GFP_ATOMIC);
		if (!skb) {
			ccci_md_err(md, "q%u rx alloc %d failed\n", q->index,
				    len);
			return;
		}
		if (ccci_ringbuf_read(rb, skb_put(skb, len), len) != len) {
			dev_kfree_skb_any(skb);
			return;
		}

		ret = ccci_rx_dispatch(md, skb);
		if (ret == -CCCI_ERR_PORT_RX_FULL) {
			/* keep the packet in shared memory and retry later */
			dev_kfree_skb_any(skb);
			queue_work(system_wq, &q->rx_retry_work);
			return;
		}
		ccci_ringbuf_move_rpointer(rb, len);
	}
	/* more data may be waiting */
	tasklet_schedule(&q->rx_task);
}

static void ccci_ccif_rx_retry_work(struct work_struct *work)
{
	struct ccci_ccif_queue *q = container_of(work, struct ccci_ccif_queue,
						 rx_retry_work);

	msleep(5);
	tasklet_schedule(&q->rx_task);
}

int ccci_ccif_ring_send(struct ccci_md *md, unsigned int qno,
			struct sk_buff *skb)
{
	struct ccci_ccif *ccif = md->ccif;
	struct ccci_ccif_queue *q;
	unsigned long flags;
	int ret;

	if (!ccif || !md->ring_mode || qno >= CCCI_MAX_QUEUES)
		return -ENODEV;

	q = &ccif->q[qno];
	if (!q->tx_ring)
		return -ENODEV;

	spin_lock_irqsave(&q->tx_lock, flags);
	if (ccci_ringbuf_writeable(q->tx_ring, skb->len) <= 0) {
		spin_unlock_irqrestore(&q->tx_lock, flags);
		return -EBUSY;
	}
	ccci_inc_tx_seq(md, (struct ccci_header *)skb->data);
	ret = ccci_ringbuf_write(q->tx_ring, skb->data, skb->len);
	spin_unlock_irqrestore(&q->tx_lock, flags);
	if (ret != skb->len) {
		ccci_md_err(md, "q%u ring write %d/%u failed\n", qno, ret,
			    skb->len);
		return -EIO;
	}

	dev_kfree_skb_any(skb);
	ccci_ccif_send(md, H2D_RINGQ0 + qno);
	return 0;
}

int ccci_ccif_ring_write_room(struct ccci_md *md, unsigned int qno)
{
	struct ccci_ccif *ccif = md->ccif;
	unsigned long flags;
	int ret;

	if (!ccif || qno >= CCCI_MAX_QUEUES || !ccif->q[qno].tx_ring)
		return 0;

	spin_lock_irqsave(&ccif->q[qno].tx_lock, flags);
	ret = ccci_ringbuf_writeable(ccif->q[qno].tx_ring, CCCI_HEADER_SIZE);
	spin_unlock_irqrestore(&ccif->q[qno].tx_lock, flags);
	return ret > 0 ? 1 : 0;
}

int ccci_ccif_ring_rx_poll(struct ccci_md *md, unsigned int qno, int budget)
{
	struct ccci_ccif *ccif = md->ccif;

	if (!ccif || qno >= CCCI_MAX_QUEUES || !ccif->q[qno].rx_ring)
		return 0;
	/* budget 0 means "kick, do not poll in this context" */
	if (budget == 0) {
		if (!atomic_read(&ccif->q[qno].rx_running))
			tasklet_schedule(&ccif->q[qno].rx_task);
		return 0;
	}
	ccci_ccif_rx_tasklet(&ccif->q[qno].rx_task);
	return 0;
}

static int ccci_ccif_ring_init(struct ccci_ccif *ccif)
{
	struct ccci_md *md = ccif->md;
	const unsigned int *rx = ccif_rx_sizes(md);
	const unsigned int *tx = ccif_tx_sizes(md);
	size_t off, remain, total = 0;
	void __iomem *base;
	unsigned int i;

	/* downstream: MD1 reserves 2MB for the EE dump at the head of smem,
	 * MD3 starts after exception+runtime regions
	 */
	off = (md->id == CCCI_MD_SYS1) ? CCIF_MD_SMEM_RESERVE :
	      (CCCI_SMEM_SIZE_EXCEPTION + CCCI_SMEM_SIZE_RUNTIME);
	if (off >= md->smem_size)
		return -ENOMEM;

	base = md->smem_vir + off;
	remain = md->smem_size - off;

	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		struct ccci_ccif_queue *q = &ccif->q[i];
		size_t size = CCCI_RINGBUF_CTL_LEN + rx[i] + tx[i];

		if (size > remain) {
			ccci_md_err(md,
				    "smem too small for q%u rings (need 0x%zx, have 0x%zx)\n",
				    i, size, remain);
			return -ENOMEM;
		}
		q->rx_ring = ccci_ringbuf_create(base, size, rx[i], tx[i]);
		if (!q->rx_ring) {
			ccci_md_err(md, "q%u ring create failed\n", i);
			return -ENOMEM;
		}
		q->tx_ring = q->rx_ring;
		base += size;
		remain -= size;
		total += size;
	}
	ccif->total_smem_size = total;
	ccci_md_info(md, "ccif rings at smem+0x%zx, 0x%zx bytes\n", off, total);
	return 0;
}

/* ------------------------------------------------------------------ */
/* exception handshake                                                 */
/* ------------------------------------------------------------------ */
static int ccci_ccif_poll_ready(struct ccci_md *md, unsigned int channel)
{
	struct ccci_ccif *ccif = md->ccif;
	unsigned int i;
	u32 ch;

	for (i = 0; i < CCIF_IRQ_POLL_CNT; i++) {
		ch = ccif_rd(ccif, APCCIF_RCHNUM);
		if (ch & BIT(channel)) {
			ccif_wr(ccif, APCCIF_ACK, BIT(channel));
			return 0;
		}
		msleep(CCIF_IRQ_POLL_MS);
	}
	ccci_md_err(md, "poll ccif ch%u timeout, RCHNUM=%08x\n", channel, ch);
	return -ETIMEDOUT;
}

void ccci_ccif_exception(struct ccci_md *md, unsigned int stage)
{
	struct ccci_ccif *ccif = md->ccif;
	char detail[96];
	unsigned int i;
	int err;

	ccci_md_err(md, "MD exception HIF stage %u\n", stage);

	switch (stage) {
	case HIF_EX_INIT:
		/*
		 * Downstream md_cd_exception()/md_ccif_exception() notify
		 * EX_INIT here (modem_cldma.c:2003, modem_ccif.c:400), which
		 * also broadcasts the EXCEPTION state and stops the watchdog
		 * timers.
		 */
		md->ex_stage = CCCI_EX_INIT;
		ccci_md_set_state(md, CCCI_MD_EXCEPTION);
		if (md->cldma) {
			/*
			 * modem_cldma.c:2005: stop every TX queue and every RX
			 * queue outside the DHL set, clearing and masking only
			 * those latches, so the DHL RX queues (0..3) stay armed
			 * for the rest of the handshake. The exception ring map
			 * is already latched by ccci_md_set_state() above
			 * (ccci_core.c:139-148), so the ccci_cldma_start() at
			 * HIF_EX_ALLQ_RESET skips the INFRA block reset
			 * (ccci_cldma.c:1911-1927).
			 */
			err = ccci_cldma_stop_for_ee(md->cldma);
			if (err) {
				/*
				 * modem_cldma.c:1468-1476,1495-1503: report the
				 * stalled engine and continue the handshake. The
				 * latch/mask programming was applied regardless.
				 */
				scnprintf(detail, sizeof(detail),
					  "md%d:\nUNKNOWN Exception\nstop Tx/Rx CLDMA for EE failed.\n",
					  md->id + 1);
				ccci_md_err(md, "stop CLDMA for EE failed: %d\n",
					    err);
				aed_md_exception_api(NULL, 0, NULL, 0, detail,
						    DB_OPT_DEFAULT);
			}
		}
		ccci_exception_trigger(md, CCCI_EX_DUMP_MD_EX,
				       ccci_exception_dump_flags(md));
		/* dispatch must not depend on queue indices, so Rx can still
		 * reach the ports during the exception flow
		 */
		ccci_ccif_send(md, H2D_EXCEPTION_ACK);
		break;
	case HIF_EX_INIT_DONE:
		/* modem_cldma.c:2012 / modem_ccif.c:405 */
		md->ex_stage = CCCI_EX_DHL_DL_RDY;
		ccci_md_set_state(md, CCCI_MD_EXCEPTION);
		ccci_exception_trigger(md, CCCI_EX_DUMP_MD_EX_REC_OK,
				       ccci_exception_dump_flags(md));
		break;
	case HIF_EX_CLEARQ_DONE:
		/*
		 * Downstream gives DHL two seconds to flush its data before the
		 * AP acknowledges the queue clear (modem_cldma.c:2015) and only
		 * then stops CLDMA and flushes its TX/RX workers
		 * (md_cd_ccif_delayed_work(), modem_cldma.c:1966-1983).
		 * The DHL RX window opened at HIF_EX_INIT is closed first:
		 * ccci_cldma_dhl_rx_alive(cd, false) stops the RX queues that
		 * were kept armed and masks their latches, and the full stop
		 * then masks every source and flushes the per-queue delayed TX
		 * work and the RX/refill work, which is the purge downstream
		 * performs in the delayed work.
		 */
		if (md->cldma) {
			msleep(2000);
			err = ccci_cldma_dhl_rx_alive(md->cldma, false);
			if (err)
				ccci_md_err(md, "close DHL Rx window failed: %d\n",
					    err);
			ccci_cldma_stop(md->cldma);
		}
		if (ccif) {
			for (i = 0; i < CCCI_MAX_QUEUES; i++) {
				if (ccif->q[i].rx_ring) {
					ccci_ringbuf_reset(ccif->q[i].rx_ring,
							   false);
					ccci_ringbuf_reset(ccif->q[i].rx_ring,
							   true);
				}
			}
		}
		/*
		 * Snapshot after the flush so the DHL data collected during the
		 * window is part of the dump, and before the ACK so it is
		 * ordered before the modem restarts CLDMA (ROM: "Modem
		 * exception dump start, please wait up to 5 minutes.",
		 * vmlinux.elf 0xc0c9b534, printed by
		 * md_cd_ccif_delayed_work()).
		 */
		ccci_exception_trigger_sync(md, CCCI_EX_DUMP_MD_EX,
					    ccci_exception_dump_flags(md));
		ccci_ccif_send(md, H2D_EXCEPTION_CLEARQ_ACK);
		break;
	case HIF_EX_ALLQ_RESET:
		/*
		 * modem_cldma.c:2023-2035 / modem_ccif.c:413. ccci_cldma_start()
		 * re-arms all eight queues, re-applies the latched exception
		 * ring map, re-programmes the L2/L3 unmask registers
		 * (cldma_hw_start(), ccci_cldma.c:1261-1270) and re-checks that
		 * OUT DMA accepted SO_CFG bit 0 (ccci_cldma.c:1941-1951), which
		 * downstream does explicitly after cldma_start()
		 * (modem_cldma.c:2026-2033).
		 */
		md->ex_stage = CCCI_EX_INIT_DONE;
		if (md->cldma)
			ccci_cldma_start(md->cldma);
		md->boot_stage = CCCI_BOOT_STAGE_2;
		break;
	default:
		break;
	}
}

/* CLDMA exception flow after H2D_EXCEPTION_ACK: the following stages are
 * signalled by polling, downstream md_cd_ccif_work()
 */
static void ccci_ccif_exc_work(struct work_struct *work)
{
	struct ccci_ccif *ccif = container_of(work, struct ccci_ccif, exc_work);
	struct ccci_md *md = ccif->md;

	ccci_ccif_exception(md, HIF_EX_INIT);
	if (ccci_ccif_poll_ready(md, D2H_EXCEPTION_INIT_DONE))
		return;
	ccci_ccif_exception(md, HIF_EX_INIT_DONE);
	if (ccci_ccif_poll_ready(md, D2H_EXCEPTION_CLEARQ_DONE))
		return;
	ccci_ccif_exception(md, HIF_EX_CLEARQ_DONE);
	if (ccci_ccif_poll_ready(md, D2H_EXCEPTION_ALLQ_RESET))
		return;
	ccci_ccif_exception(md, HIF_EX_ALLQ_RESET);
}

/* ------------------------------------------------------------------ */
/* SRAM message                                                        */
/* ------------------------------------------------------------------ */
static void ccci_ccif_sram_rx(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;
	struct ccci_header *h = &ccif->sram->up_header;
	struct sk_buff *skb;
	u32 header[4];
	int i;

	/* downstream md_ccif_sram_rx_work(): only the 16 byte header lives in
	 * the SRAM window, payload follows in shared memory
	 */
	for (i = 0; i < 4; i++)
		header[i] = readl(((u32 __iomem *)h) + i);

	skb = ccci_alloc_skb(CCCI_HEADER_SIZE, GFP_ATOMIC);
	if (!skb)
		return;
	memcpy(skb_put(skb, CCCI_HEADER_SIZE), header, CCCI_HEADER_SIZE);
	if (ccci_rx_dispatch(md, skb) == -CCCI_ERR_PORT_RX_FULL)
		dev_kfree_skb_any(skb);
}

/* ------------------------------------------------------------------ */
/* IRQ                                                                 */
/* ------------------------------------------------------------------ */
static irqreturn_t ccci_ccif_isr(int irq, void *data)
{
	struct ccci_ccif *ccif = data;
	u32 ch;

	ch = ccif_rd(ccif, APCCIF_RCHNUM);
	if (!ch)
		return IRQ_NONE;

	/* the MD asserts the line until APCCIF_ACK is written back */
	ccif_wr(ccif, APCCIF_ACK, ch);
	spin_lock(&ccif->lock);
	ccif->channels |= ch;
	spin_unlock(&ccif->lock);

	if (ch & (BIT(D2H_EXCEPTION_INIT) | BIT(D2H_EXCEPTION_INIT_DONE) |
		  BIT(D2H_EXCEPTION_CLEARQ_DONE) |
		  BIT(D2H_EXCEPTION_ALLQ_RESET) | BIT(AP_MD_SEQ_ERROR) |
		  BIT(D2H_SRAM)))
		return IRQ_WAKE_THREAD;

	tasklet_schedule(&ccif->irq_task);
	return IRQ_HANDLED;
}

static irqreturn_t ccci_ccif_thread(int irq, void *data)
{
	struct ccci_ccif *ccif = data;
	struct ccci_md *md = ccif->md;
	unsigned long pending;

	spin_lock_irq(&ccif->lock);
	pending = ccif->channels &
		  (BIT(D2H_EXCEPTION_INIT) | BIT(D2H_EXCEPTION_INIT_DONE) |
		   BIT(D2H_EXCEPTION_CLEARQ_DONE) |
		   BIT(D2H_EXCEPTION_ALLQ_RESET) | BIT(AP_MD_SEQ_ERROR) |
		   BIT(D2H_SRAM));
	ccif->channels &= ~pending;
	spin_unlock_irq(&ccif->lock);

	if (!pending) {
		tasklet_schedule(&ccif->irq_task);
		return IRQ_HANDLED;
	}

	if (pending & BIT(AP_MD_SEQ_ERROR)) {
		ccci_md_err(md, "MD detected a sequence error\n");
		ccci_ccif_dump_regs(md, NULL);
	}

	if (pending & BIT(D2H_SRAM))
		ccci_ccif_sram_rx(md);

	if (md->cldma) {
		if (pending & (BIT(D2H_EXCEPTION_INIT) |
			       BIT(D2H_EXCEPTION_INIT_DONE) |
			       BIT(D2H_EXCEPTION_CLEARQ_DONE) |
			       BIT(D2H_EXCEPTION_ALLQ_RESET)))
			schedule_work(&ccif->exc_work);
	} else {
		if (pending & BIT(D2H_EXCEPTION_INIT))
			ccci_ccif_exception(md, HIF_EX_INIT);
		if (pending & BIT(D2H_EXCEPTION_INIT_DONE))
			ccci_ccif_exception(md, HIF_EX_INIT_DONE);
		if (pending & BIT(D2H_EXCEPTION_CLEARQ_DONE))
			ccci_ccif_exception(md, HIF_EX_CLEARQ_DONE);
		if (pending & BIT(D2H_EXCEPTION_ALLQ_RESET))
			ccci_ccif_exception(md, HIF_EX_ALLQ_RESET);
	}

	/* anything the thread could not classify (unknown bits) is dropped by
	 * the tasklet, ring bits are handled there as well
	 */
	tasklet_schedule(&ccif->irq_task);
	return IRQ_HANDLED;
}

static void ccci_ccif_irq_task(struct tasklet_struct *t)
{
	struct ccci_ccif *ccif = from_tasklet(ccif, t, irq_task);
	unsigned long pending;
	int i;

	/* the tasklet owns the ring doorbells only; exception and SRAM bits
	 * stay queued for the threaded handler
	 */
	spin_lock_irq(&ccif->lock);
	pending = ccif->channels & CCCI_CCIF_RING_MASK;
	ccif->channels &= ~CCCI_CCIF_RING_MASK;
	spin_unlock_irq(&ccif->lock);

	if (!pending)
		return;

	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		if ((pending & BIT(D2H_RINGQ0 + i)) && ccif->q[i].rx_ring)
			tasklet_schedule(&ccif->q[i].rx_task);
	}

}

/* ------------------------------------------------------------------ */
/* MD watchdog                                                         */
/* ------------------------------------------------------------------ */
static void ccci_ccif_wdt_work(struct work_struct *work)
{
	struct ccci_ccif *ccif = container_of(work, struct ccci_ccif, wdt_work);
	struct ccci_md *md = ccif->md;

	ccci_md_err(md, "MD watchdog IRQ, recovering\n");
	ccif_disable_irq(md);
	ccci_ccif_dump_regs(md, NULL);
	/*
	 * downstream md_cd_wdt_work() dumps the RGU registers and the
	 * shared-memory windows before it resets the modem
	 * (modem_cldma.c:1838-1897, md_cd_dump_debug_register()).  The reset
	 * is destructive, so take the snapshot synchronously first.
	 */
	md->ex_stage = CCCI_EX_WDT;
	ccci_exception_trigger_sync(md, CCCI_EX_DUMP_WDT,
				    CCCI_AED_DUMP_EX_MEM |
				    CCCI_AED_DUMP_CCIF_REG);
	ccci_md_reset(md);
	ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH, CCCI_MD_MSG_RESET, 0);
}

static irqreturn_t ccci_ccif_wdt_isr(int irq, void *data)
{
	struct ccci_ccif *ccif = data;

	schedule_work(&ccif->wdt_work);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* create / start / stop / destroy                                     */
/* ------------------------------------------------------------------ */
struct ccci_ccif *ccci_ccif_create(struct ccci_md *md, struct device *dev,
				   void __iomem *ap_base, void __iomem *md_base,
				   void __iomem *rgu_base, phys_addr_t boot_vector,
				   phys_addr_t boot_key, phys_addr_t boot_en,
				   unsigned int irq, unsigned int wdt_irq,
				   unsigned int sram_size, bool ring_mode)
{
	struct ccci_ccif *ccif;
	unsigned int i;

	if (!md || !ap_base || !irq)
		return ERR_PTR(-EINVAL);

	ccif = kzalloc(sizeof(*ccif), GFP_KERNEL);
	if (!ccif)
		return ERR_PTR(-ENOMEM);

	ccif->md = md;
	ccif->dev = dev;
	ccif->ap_base = ap_base;
	ccif->md_base = md_base;
	ccif->rgu_base = rgu_base;
	if (boot_vector)
		ccif->boot_vector = ioremap(boot_vector, 4);
	if (boot_key)
		ccif->boot_key = ioremap(boot_key, 4);
	if (boot_en)
		ccif->boot_en = ioremap(boot_en, 4);
	ccif->irq = irq;
	ccif->wdt_irq = wdt_irq;
	ccif->sram_size = sram_size ? sram_size : CCIF_SRAM_SIZE;
	ccif->ring_mode = ring_mode;
	ccif->sram = (struct ccci_ccif_sram *)(ap_base + APCCIF_CHDATA);
	spin_lock_init(&ccif->lock);
	tasklet_setup(&ccif->irq_task, ccci_ccif_irq_task);
	INIT_WORK(&ccif->exc_work, ccci_ccif_exc_work);
	INIT_WORK(&ccif->wdt_work, ccci_ccif_wdt_work);
	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		ccif->q[i].md = md;
		ccif->q[i].index = i;
		spin_lock_init(&ccif->q[i].tx_lock);
		atomic_set(&ccif->q[i].rx_running, 0);
		tasklet_setup(&ccif->q[i].rx_task, ccci_ccif_rx_tasklet);
		INIT_WORK(&ccif->q[i].rx_retry_work,
			  ccci_ccif_rx_retry_work);
	}
	return ccif;
}

int ccci_ccif_start(struct ccci_ccif *ccif)
{
	struct ccci_md *md;
	unsigned int i;
	int ret;

	if (!ccif)
		return -EINVAL;
	md = ccif->md;
	if (ccif->started)
		return 0;

	/* downstream md_ccif_hw_init(): arbitration on, clear all acks,
	 * clear the SRAM window
	 */
	ccif_wr(ccif, APCCIF_CON, 0x01);
	ccif_wr(ccif, APCCIF_ACK, 0xFFFF);
	for (i = 0; i < ccif->sram_size / sizeof(u32); i++)
		writel(0, ccif->ap_base + APCCIF_CHDATA + i * sizeof(u32));

	ret = request_threaded_irq(ccif->irq, ccci_ccif_isr, ccci_ccif_thread,
				   0, "CCIF_AP", ccif);
	if (ret) {
		ccci_md_err(md, "request CCIF irq %u failed %d\n", ccif->irq,
			    ret);
		return ret;
	}
	if (ccif->wdt_irq) {
		ret = request_threaded_irq(ccif->wdt_irq, NULL,
					   ccci_ccif_wdt_isr, 0, "MD_WDT",
					   ccif);
		if (ret) {
			ccci_md_err(md, "request MD WDT irq %u failed %d\n",
				    ccif->wdt_irq, ret);
			free_irq(ccif->irq, ccif);
			return ret;
		}
		/* balanced by ccif_enable_irq() at modem start */
		disable_irq_nosync(ccif->wdt_irq);
	}
	ccif->irq_enabled = true;

	if (ccif->ring_mode) {
		ret = ccci_ccif_ring_init(ccif);
		if (ret) {
			free_irq(ccif->irq, ccif);
			if (ccif->wdt_irq)
				free_irq(ccif->wdt_irq, ccif);
			ccif->irq_enabled = false;
			return ret;
		}
	}

	ccif->started = true;
	ccci_md_info(md, "ccif started (%s mode), irq %u wdt %u\n",
		     ccif->ring_mode ? "ring" : "control", ccif->irq,
		     ccif->wdt_irq);
	return 0;
}

void ccci_ccif_stop(struct ccci_ccif *ccif)
{
	struct ccci_md *md;
	unsigned int i;

	if (!ccif || !ccif->started)
		return;
	md = ccif->md;

	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		cancel_work_sync(&ccif->q[i].rx_retry_work);
		tasklet_kill(&ccif->q[i].rx_task);
	}
	cancel_work_sync(&ccif->exc_work);
	tasklet_kill(&ccif->irq_task);

	free_irq(ccif->irq, ccif);
	if (ccif->wdt_irq)
		free_irq(ccif->wdt_irq, ccif);
	ccif->started = false;
	ccif->irq_enabled = false;
	ccif->channels = 0;

	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		kfree(ccif->q[i].rx_ring);
		ccif->q[i].rx_ring = NULL;
		ccif->q[i].tx_ring = NULL;
	}
	ccci_md_dbg(md, "ccif stopped\n");
}

void ccci_ccif_destroy(struct ccci_ccif *ccif)
{
	if (!ccif)
		return;
	ccci_ccif_stop(ccif);
	cancel_work_sync(&ccif->wdt_work);
	if (ccif->boot_vector)
		iounmap(ccif->boot_vector);
	if (ccif->boot_key)
		iounmap(ccif->boot_key);
	if (ccif->boot_en)
		iounmap(ccif->boot_en);
	if (ccif->md)
		ccif->md->ccif = NULL;
	kfree(ccif);
}

void ccci_ccif_reset_rings(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;
	unsigned int i;

	if (!ccif)
		return;
	for (i = 0; i < CCCI_MAX_QUEUES; i++) {
		if (!ccif->q[i].rx_ring)
			continue;
		ccci_ringbuf_reset(ccif->q[i].rx_ring, false);
		ccci_ringbuf_reset(ccif->q[i].rx_ring, true);
	}
}

void ccci_ccif_dump_regs(struct ccci_md *md, struct seq_file *s)
{
	struct ccci_ccif *ccif = md->ccif;
	u32 con, busy, start, tchnum, rchnum, ack;

	if (!ccif || !ccif->ap_base) {
		if (s)
			seq_puts(s, "ccif: not available\n");
		return;
	}

	con = ccif_rd(ccif, APCCIF_CON);
	busy = ccif_rd(ccif, APCCIF_BUSY);
	start = ccif_rd(ccif, APCCIF_START);
	tchnum = ccif_rd(ccif, APCCIF_TCHNUM);
	rchnum = ccif_rd(ccif, APCCIF_RCHNUM);
	ack = ccif_rd(ccif, APCCIF_ACK);

	if (s) {
		seq_printf(s, "AP_CON     0x%08x\n", con);
		seq_printf(s, "AP_BUSY    0x%08x\n", busy);
		seq_printf(s, "AP_START   0x%08x\n", start);
		seq_printf(s, "AP_TCHNUM  0x%08x\n", tchnum);
		seq_printf(s, "AP_RCHNUM  0x%08x\n", rchnum);
		seq_printf(s, "AP_ACK     0x%08x\n", ack);
		seq_printf(s, "irq         %u (enabled %d), wdt irq %u\n",
			   ccif->irq, ccif->irq_enabled, ccif->wdt_irq);
		seq_printf(s, "channels    0x%08lx\n", ccif->channels);
		seq_printf(s, "ring mode   %d, smem 0x%zx\n", ccif->ring_mode,
			   (size_t)ccif->total_smem_size);
	} else {
		ccci_md_err(md, "CON %08x BUSY %08x START %08x TCHNUM %08x RCHNUM %08x ACK %08x\n",
			    con, busy, start, tchnum, rchnum, ack);
	}
}
