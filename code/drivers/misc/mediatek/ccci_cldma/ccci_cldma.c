// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735/MT6737T CLDMA engine (MD1 data transport).
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/eccci/modem_cldma.c
 *   drivers/misc/mediatek/eccci/modem_cldma.h
 *   drivers/misc/mediatek/eccci/mt6735/cldma_reg.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CLDMA is a pair of scatter/gather DMA engines between the AP and MD1.
 * The AP side owns 8 TX queues (AP -> MD, "UL" in the register naming) and
 * 8 RX queues (MD -> AP, "SO"); each queue walks a ring of 16-byte GPDs that
 * are chained through next_gpd_ptr and owned by the hardware while the HWO
 * bit (bit 0 of gpd_flags) is set. A GPD either points directly at the
 * payload (RING_GPD) or at a chain of BDs (RING_GPD_BD, used by the network
 * queues 3/4/5 to support skb fragments).
 *
 * The public entry points live in include/linux/ccci.h. The CCCI core owns
 * the modem lifecycle and hands RX skbs to the port layer through the
 * callback registered with ccci_cldma_set_rx_cb().
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "ccci_cldma.h"

/*
 * Ring geometry, modem_cldma.c:82-101. The index is the hardware queue
 * number; the ring sets are indexed through the *_queue2ring tables.
 */
static const int cldma_net_rx_queue_buffer_size[CLDMA_RXQ_NUM] = {
	0, 0, 0, CLDMA_NET_RX_PKT_SIZE, CLDMA_NET_RX_PKT_SIZE,
	CLDMA_NET_RX_PKT_SIZE, 0, 0
};
static const int cldma_normal_rx_queue_buffer_size[CLDMA_RXQ_NUM] = {
	CLDMA_NORMAL_RX_PKT_SIZE, CLDMA_NORMAL_RX_PKT_SIZE,
	CLDMA_NORMAL_RX_PKT_SIZE, CLDMA_NORMAL_RX_PKT_SIZE,
	0, 0, CLDMA_NORMAL_RX_PKT_SIZE, CLDMA_NORMAL_RX_PKT_SIZE
};
static const int cldma_net_rx_queue_buffer_number[CLDMA_RXQ_NUM] = {
	0, 0, 0, 256, 256, 64, 0, 0
};
static const int cldma_net_tx_queue_buffer_number[CLDMA_TXQ_NUM] = {
	0, 0, 0, 256, 256, 64, 0, 0
};
static const int cldma_normal_rx_queue_buffer_number[CLDMA_RXQ_NUM] = {
	16, 16, 16, 16, 0, 0, 16, 16
};
static const int cldma_normal_tx_queue_buffer_number[CLDMA_TXQ_NUM] = {
	16, 16, 16, 16, 0, 0, 16, 16
};
static const int cldma_net_rx_queue2ring[CLDMA_RXQ_NUM] = {
	-1, -1, -1, 0, 1, 2, -1, -1
};
static const int cldma_net_tx_queue2ring[CLDMA_TXQ_NUM] = {
	-1, -1, -1, 0, 1, 2, -1, -1
};
static const int cldma_normal_rx_queue2ring[CLDMA_RXQ_NUM] = {
	0, 1, 2, 3, -1, -1, 4, 5
};
static const int cldma_normal_tx_queue2ring[CLDMA_TXQ_NUM] = {
	0, 1, 2, 3, -1, -1, 4, 5
};
static const int cldma_net_rx_ring2queue[CLDMA_NET_RXQ_NUM] = { 3, 4, 5 };
static const int cldma_net_tx_ring2queue[CLDMA_NET_TXQ_NUM] = { 3, 4, 5 };
static const int cldma_normal_rx_ring2queue[CLDMA_NORMAL_RXQ_NUM] = {
	0, 1, 2, 3, 6, 7
};
static const int cldma_normal_tx_ring2queue[CLDMA_NORMAL_TXQ_NUM] = {
	0, 1, 2, 3, 6, 7
};

/* modem_cldma.c:1354-1380: how long to wait for a queue to drain, in polls. */
#define CLDMA_STOP_POLL_MAX		10000
#define CLDMA_STOP_POLL_US		20
/* A refill is retried rather than spinning forever, modem_cldma.c:362-371. */
#define CLDMA_REFILL_RETRY_MAX		100
#define CLDMA_REFILL_RETRY_MS		20

static inline void cldma_write32(void __iomem *base, u32 off, u32 val)
{
	writel(val, base + off);
}

static inline u32 cldma_read32(void __iomem *base, u32 off)
{
	return readl(base + off);
}

/*
 * GPD checksum, modem_cldma.c:275-286. Disabled on MT6735
 * (CLDMA_CHECKSUM_SIZE == 0, modem_cldma.h:48), so the compiler removes it.
 */
static void cldma_checksum(void *address, u8 first_byte)
{
#if CLDMA_CHECKSUM_SIZE
	u8 *p = address;
	u8 sum = first_byte;
	int i;

	for (i = 2; i < CLDMA_CHECKSUM_SIZE; i++)
		sum += p[i];
	p[1] = 0xFF - sum;
#else
	(void)address;
	(void)first_byte;
#endif
}

static struct cldma_request *cldma_ring_step_forward(struct cldma_ring *ring,
						     struct cldma_request *req)
{
	if (req->entry.next == &ring->gpd_ring)
		return list_first_entry(&ring->gpd_ring, struct cldma_request,
					entry);

	return list_entry(req->entry.next, struct cldma_request, entry);
}

/* Buffer capacity of an skb, i.e. what dma_map_single() must cover. */
static size_t cldma_skb_data_size(const struct sk_buff *skb)
{
	return skb_end_pointer(skb) - skb->data;
}

static struct sk_buff *cldma_alloc_rx_skb(unsigned int size)
{
	struct sk_buff *skb = __netdev_alloc_skb(NULL, size, GFP_KERNEL);

	if (!skb)
		return NULL;
	skb_reset_tail_pointer(skb);
	return skb;
}

static void cldma_dump_register(struct ccci_cldma *cd)
{
	dev_info(cd->dev,
		 "CLDMA regs: CODA_VERSION=0x%08x L2TISAR0=0x%08x L2RISAR0=0x%08x L2TIMR0=0x%08x L2RIMR0=0x%08x\n",
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_CLDMA_CODA_VERSION),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2TISAR0),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2TIMR0),
		 cldma_read32(cd->ap_base, CLDMA_AP_L2RIMR0));
	dev_info(cd->dev,
		 "CLDMA regs: L3TISAR0=0x%08x L3TISAR1=0x%08x L3RISAR0=0x%08x L3RISAR1=0x%08x\n",
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TISAR0),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TISAR1),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RISAR0),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RISAR1));
	dev_info(cd->dev, "CLDMA regs: UL_STATUS=0x%08x SO_STATUS=0x%08x IP_BUSY=0x%08x\n",
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_UL_STATUS),
		 cldma_read32(cd->ap_base, CLDMA_AP_SO_STATUS),
		 cldma_read32(cd->ap_pdn_base, CLDMA_AP_CLDMA_IP_BUSY));
}

/* modem_cldma.c:131-163 */
static void cldma_dump_gpd_queue(struct ccci_cldma *cd, unsigned int qno)
{
	struct cldma_request *req;
	u32 *tmp;

	if (qno >= CLDMA_TXQ_NUM)
		return;

	dev_info(cd->dev, "dump txq %u request\n", qno);
	if (cd->txq[qno].tr_ring) {
		list_for_each_entry(req, &cd->txq[qno].tr_ring->gpd_ring,
				    entry) {
			tmp = req->gpd;
			dev_info(cd->dev, " 0x%p: %08x %08x %08x %08x\n",
				 req->gpd, tmp[0], tmp[1], tmp[2], tmp[3]);
		}
	}

	dev_info(cd->dev, "dump rxq %u request\n", qno);
	if (cd->rxq[qno].tr_ring) {
		list_for_each_entry(req, &cd->rxq[qno].tr_ring->gpd_ring,
				    entry) {
			tmp = req->gpd;
			dev_info(cd->dev, " 0x%p/0x%p: %08x %08x %08x %08x\n",
				 req->gpd, req->skb, tmp[0], tmp[1], tmp[2],
				 tmp[3]);
		}
	}
}

/*
 * TX watchdog, modem_cldma.c:308-330. Downstream only arms it for the VT
 * queue and, on expiry, dumps the queue and asks the core to force-assert the
 * modem through CCIF; this module only owns CLDMA, so the dump is all it can
 * do (see the deferred list in the report).
 */
static void cldma_timeout_timer_func(struct timer_list *t)
{
	struct cldma_queue *queue = timer_container_of(queue, t, timeout_timer);
	struct ccci_cldma *cd = queue->cd;

	dev_err(cd->dev, "CLDMA txq%u no response for %d seconds\n",
		queue->index, CLDMA_ACTIVE_T);
	cldma_dump_gpd_queue(cd, queue->index);
	cldma_dump_register(cd);
}

/* ------------------------------------------------------------------ */
/* TX descriptors                                                      */
/* ------------------------------------------------------------------ */

/* modem_cldma.c:2698-2760, CLDMA_NET_TX_BD path */
static int cldma_gpd_bd_handle_tx_request(struct cldma_queue *queue,
					  struct cldma_request *tx_req,
					  struct sk_buff *skb)
{
	struct ccci_cldma *cd = queue->cd;
	struct skb_shared_info *info = skb_shinfo(skb);
	struct cldma_tgpd *tgpd = tx_req->gpd;
	struct cldma_request *bd_req;
	struct cldma_tbd *tbd;
	int cur_frag;

	bd_req = list_first_entry(&tx_req->bd, struct cldma_request, entry);
	for (cur_frag = -1; cur_frag < info->nr_frags; cur_frag++) {
		unsigned int frag_len;
		void *frag_addr;
		dma_addr_t addr;

		if (cur_frag == -1) {
			frag_len = skb_headlen(skb);
			frag_addr = skb->data;
		} else {
			skb_frag_t *frag = &info->frags[cur_frag];

			frag_len = skb_frag_size(frag);
			frag_addr = skb_frag_address(frag);
		}

		addr = dma_map_single(cd->dev, frag_addr, frag_len,
				      DMA_TO_DEVICE);
		if (dma_mapping_error(cd->dev, addr)) {
			struct cldma_request *undo;

			dev_err(cd->dev, "dma map TX frag failed on q%u\n",
				queue->index);
			/* Undo the fragments already accepted. */
			list_for_each_entry(undo, &tx_req->bd, entry) {
				struct cldma_tbd *utbd = undo->gpd;

				if (utbd->non_used != 1)
					continue;
				utbd->non_used = 0;
				dma_unmap_single(cd->dev,
						 undo->data_buffer_ptr_saved,
						 utbd->data_buff_len,
						 DMA_TO_DEVICE);
			}
			return -EIO;
		}

		tbd = bd_req->gpd;
		bd_req->data_buffer_ptr_saved = addr;
		tbd->data_buff_ptr = lower_32_bits(addr);
		tbd->data_buff_len = frag_len;
		tbd->non_used = 1;
		tbd->bd_flags &= ~CLDMA_BD_EOL;
		cldma_checksum(tbd, tbd->bd_flags);
		bd_req = list_entry(bd_req->entry.next, struct cldma_request,
				    entry);
	}
	/* tbd is the last BD actually used: terminate the chain there. */
	tbd = bd_req->gpd;
	tbd->bd_flags |= CLDMA_BD_EOL;
	cldma_checksum(tbd, tbd->bd_flags);

	tgpd->data_buff_len = skb->len;
	tgpd->debug_id = queue->debug_id++;
	tgpd->non_used = 1;
	cldma_checksum(tgpd, tgpd->gpd_flags | CLDMA_HWO);

	/* Publish the descriptors before handing ownership to the engine. */
	wmb();
	spin_lock(&cd->timeout_lock);
	if (cd->txq_active & BIT(queue->index))
		WRITE_ONCE(tgpd->gpd_flags,
			   READ_ONCE(tgpd->gpd_flags) | CLDMA_HWO);
	spin_unlock(&cd->timeout_lock);

	tx_req->skb = skb;
	return 0;
}

/* modem_cldma.c:2762-2799, plain GPD path */
static int cldma_gpd_handle_tx_request(struct cldma_queue *queue,
				       struct cldma_request *tx_req,
				       struct sk_buff *skb)
{
	struct ccci_cldma *cd = queue->cd;
	struct cldma_tgpd *tgpd = tx_req->gpd;
	dma_addr_t addr;

	addr = dma_map_single(cd->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(cd->dev, addr)) {
		dev_err(cd->dev, "dma map TX failed on q%u\n", queue->index);
		return -EIO;
	}

	tx_req->data_buffer_ptr_saved = addr;
	tgpd->data_buff_bd_ptr = lower_32_bits(addr);
	tgpd->data_buff_len = skb->len;
	tgpd->debug_id = queue->debug_id++;
	tgpd->non_used = 1;
	cldma_checksum(tgpd, tgpd->gpd_flags | CLDMA_HWO);

	/* Publish the GPD contents before the HWO bit that hands it to CLDMA. */
	wmb();
	spin_lock(&cd->timeout_lock);
	if (cd->txq_active & BIT(queue->index))
		WRITE_ONCE(tgpd->gpd_flags,
			   READ_ONCE(tgpd->gpd_flags) | CLDMA_HWO);
	spin_unlock(&cd->timeout_lock);

	tx_req->skb = skb;
	return 0;
}

static void cldma_unmap_tx_request(struct ccci_cldma *cd,
				   struct cldma_ring *ring,
				   struct cldma_request *req)
{
	struct cldma_tgpd *tgpd = req->gpd;

	if (ring->type == RING_GPD_BD) {
		struct cldma_request *bd_req;

		list_for_each_entry(bd_req, &req->bd, entry) {
			struct cldma_tbd *tbd = bd_req->gpd;

			if (tbd->non_used != 1)
				continue;
			tbd->non_used = 2;
			dma_unmap_single(cd->dev, bd_req->data_buffer_ptr_saved,
					 tbd->data_buff_len, DMA_TO_DEVICE);
		}
	} else {
		dma_unmap_single(cd->dev, req->data_buffer_ptr_saved,
				 tgpd->data_buff_len, DMA_TO_DEVICE);
	}
}

/* modem_cldma.c:798-860 */
static int cldma_gpd_bd_tx_collect(struct cldma_queue *queue)
{
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	struct cldma_request *req;
	struct cldma_tgpd *tgpd;
	struct sk_buff *skb;
	int count = 0;

	for (;;) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->tr_done;
		tgpd = (struct cldma_tgpd *)req->gpd;
		if ((READ_ONCE(tgpd->gpd_flags) & CLDMA_HWO) || !req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}
		tgpd->non_used = 2;
		cldma_unmap_tx_request(cd, queue->tr_ring, req);
		skb = req->skb;
		req->skb = NULL;
		queue->tr_done = cldma_ring_step_forward(queue->tr_ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		dev_kfree_skb_any(skb);
		count++;
	}

	return count;
}

/* modem_cldma.c:862-939 */
static int cldma_gpd_tx_collect(struct cldma_queue *queue)
{
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	struct cldma_request *req;
	struct cldma_tgpd *tgpd;
	struct sk_buff *skb;
	int count = 0;

	for (;;) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->tr_done;
		tgpd = (struct cldma_tgpd *)req->gpd;
		if ((READ_ONCE(tgpd->gpd_flags) & CLDMA_HWO) || !req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}
		tgpd->non_used = 2;
		cldma_unmap_tx_request(cd, queue->tr_ring, req);
		skb = req->skb;
		req->skb = NULL;
		queue->tr_done = cldma_ring_step_forward(queue->tr_ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		dev_kfree_skb_any(skb);
		count++;
	}

	return count;
}

/* modem_cldma.c:941-995 */
static void cldma_tx_done(struct work_struct *work)
{
	struct cldma_queue *queue =
		container_of(to_delayed_work(work), struct cldma_queue, tx_work);
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	int count;

	count = queue->tr_ring->handle_tx_done(queue);
	if (count) {
		queue_delayed_work(queue->worker, &queue->tx_work,
				   msecs_to_jiffies(10));
	} else {
		/* Re-enable the per-queue TX_DONE interrupt, cldma:985-991. */
		spin_lock_irqsave(&cd->timeout_lock, flags);
		if (cd->txq_active & BIT(queue->index))
			cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMCR0,
				      BIT(queue->index));
		spin_unlock_irqrestore(&cd->timeout_lock, flags);
	}
}

/* ------------------------------------------------------------------ */
/* RX descriptors                                                      */
/* ------------------------------------------------------------------ */

/*
 * Put one new skb behind the producer cursor, modem_cldma.c:333-384 and
 * 531-577 (network fast path).
 */
static int cldma_gpd_rx_refill(struct cldma_queue *queue)
{
	struct ccci_cldma *cd = queue->cd;
	struct cldma_ring *ring = queue->tr_ring;
	struct cldma_request *req;
	struct cldma_rgpd *rgpd;
	struct sk_buff *skb;
	unsigned long flags;
	int count = 0;
	int retry;

	for (;;) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->rx_refill;
		if (!req || req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		retry = 0;
		skb = cldma_alloc_rx_skb(ring->pkt_size);
		while (!skb && retry++ < CLDMA_REFILL_RETRY_MAX) {
			msleep(CLDMA_REFILL_RETRY_MS);
			skb = cldma_alloc_rx_skb(ring->pkt_size);
		}
		if (!skb) {
			dev_err(cd->dev, "alloc skb fail on rxq%u\n",
				queue->index);
			break;
		}

		rgpd = (struct cldma_rgpd *)req->gpd;
		req->data_buffer_ptr_saved =
			dma_map_single(cd->dev, skb->data,
				       cldma_skb_data_size(skb),
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(cd->dev, req->data_buffer_ptr_saved)) {
			dev_err(cd->dev, "dma map RX failed on q%u\n",
				queue->index);
			dev_kfree_skb_any(skb);
			break;
		}
		rgpd->data_buff_bd_ptr =
			lower_32_bits(req->data_buffer_ptr_saved);
		rgpd->data_buff_len = 0;
		cldma_checksum(rgpd, CLDMA_RGPD_HWO_IOC);

		spin_lock_irqsave(&queue->ring_lock, flags);
		/* The cursor may have moved while the skb was allocated. */
		if (queue->rx_refill != req || req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			dma_unmap_single(cd->dev, req->data_buffer_ptr_saved,
					 cldma_skb_data_size(skb),
					 DMA_FROM_DEVICE);
			dev_kfree_skb_any(skb);
			continue;
		}
		/* Publish the RGPD contents before the HWO bit. */
		wmb();
		spin_lock(&cd->timeout_lock);
		WRITE_ONCE(rgpd->gpd_flags, CLDMA_RGPD_HWO_IOC);
		spin_unlock(&cd->timeout_lock);
		req->skb = skb;
		queue->rx_refill = cldma_ring_step_forward(ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);
		count++;
	}

	return count;
}

/*
 * Validate and unmap a completed RGPD. Returns the skb with its length set,
 * or NULL when the descriptor carried an impossible length.
 */
static struct sk_buff *cldma_rx_unmap(struct ccci_cldma *cd,
				      struct cldma_request *req,
				      struct cldma_rgpd *rgpd,
				      struct sk_buff *skb)
{
	unsigned int len = rgpd->data_buff_len;
	size_t size = cldma_skb_data_size(skb);

	dma_unmap_single(cd->dev, req->data_buffer_ptr_saved, size,
			 DMA_FROM_DEVICE);
	if (len > size) {
		dev_err(cd->dev, "RX length %u overflows %zu byte skb\n", len,
			size);
		dev_kfree_skb_any(skb);
		return NULL;
	}
	skb_put(skb, len);
	return skb;
}

static void cldma_deliver_rx(struct ccci_cldma *cd, unsigned int qno,
			     struct sk_buff *skb)
{
	void (*cb)(void *ctx, unsigned int qno, struct sk_buff *skb) =
		READ_ONCE(cd->rx_cb);
	void *ctx = READ_ONCE(cd->rx_ctx);

	if (cb)
		cb(ctx, qno, skb);
	else
		dev_kfree_skb_any(skb);
}

/* modem_cldma.c:386-537 */
static int cldma_gpd_rx_collect(struct cldma_queue *queue)
{
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	struct cldma_request *req;
	struct cldma_rgpd *rgpd;
	struct sk_buff *skb;
	int count = 0;

	for (;;) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->tr_done;
		rgpd = (struct cldma_rgpd *)req->gpd;
		if ((READ_ONCE(rgpd->gpd_flags) & CLDMA_HWO) || !req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		skb = cldma_rx_unmap(cd, req, rgpd, req->skb);

		spin_lock_irqsave(&queue->ring_lock, flags);
		req->skb = NULL;
		rgpd->data_buff_bd_ptr = 0;
		queue->tr_done = cldma_ring_step_forward(queue->tr_ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		if (skb)
			cldma_deliver_rx(cd, queue->index, skb);
		queue_work(queue->refill_worker, &queue->refill_work);
		count++;
	}

	return count;
}

/* modem_cldma.c:539-639 */
static int cldma_gpd_net_rx_collect(struct cldma_queue *queue)
{
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	struct cldma_request *req;
	struct cldma_rgpd *rgpd;
	struct sk_buff *skb;
	int count = 0;

	for (;;) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->tr_done;
		rgpd = (struct cldma_rgpd *)req->gpd;
		if ((READ_ONCE(rgpd->gpd_flags) & CLDMA_HWO) || !req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}
		skb = req->skb;
		req->skb = NULL;
		rgpd->data_buff_bd_ptr = 0;
		queue->tr_done = cldma_ring_step_forward(queue->tr_ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);

		skb = cldma_rx_unmap(cd, req, rgpd, skb);
		if (skb) {
			skb_queue_tail(&queue->rx_list, skb);
			wake_up_interruptible(&queue->rx_wq);
		}
		/* Refill immediately, there is no port-level flow control. */
		cldma_gpd_rx_refill(queue);
		queue_work(queue->refill_worker, &queue->refill_work);
		count++;
	}

	return count;
}

/* modem_cldma.c:641-690 */
static int cldma_net_rx_push_thread(void *arg)
{
	struct cldma_queue *queue = arg;
	struct ccci_cldma *cd = queue->cd;
	struct sk_buff *skb;

	while (!kthread_should_stop()) {
		wait_event_interruptible(queue->rx_wq,
					 !skb_queue_empty(&queue->rx_list) ||
					 READ_ONCE(queue->rx_stop));
		if (READ_ONCE(queue->rx_stop))
			break;

		while ((skb = skb_dequeue(&queue->rx_list)))
			cldma_deliver_rx(cd, queue->index, skb);
	}

	return 0;
}

/* modem_cldma.c:692-720 */
static void cldma_rx_refill(struct work_struct *work)
{
	struct cldma_queue *queue =
		container_of(work, struct cldma_queue, refill_work);
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	int count;

	count = queue->tr_ring->handle_rx_refill(queue);
	dev_dbg(cd->dev, "CLDMA Rxq%u refilled, count=%d\n", queue->index,
		count);

	spin_lock_irqsave(&cd->timeout_lock, flags);
	if (cd->rxq_active & BIT(queue->index)) {
		/* Resume the queue now that it has descriptors again. */
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_SO_RESUME_CMD,
			      BIT(queue->index));
		cldma_read32(cd->ap_pdn_base, CLDMA_AP_SO_RESUME_CMD);
	}
	spin_unlock_irqrestore(&cd->timeout_lock, flags);
}

/* modem_cldma.c:722-796 */
static void cldma_rx_done(struct work_struct *work)
{
	struct cldma_queue *queue =
		container_of(work, struct cldma_queue, rx_work);
	struct ccci_cldma *cd = queue->cd;
	unsigned long flags;
	u32 l2risar0;
	int retry = 0;

again:
	queue->tr_ring->handle_rx_done(queue);

	spin_lock_irqsave(&cd->timeout_lock, flags);
	if (cd->rxq_active & BIT(queue->index)) {
		l2risar0 = cldma_read32(cd->ap_pdn_base,
					CLDMA_AP_L2RISAR0);
		if (retry < 10000 &&
		    (l2risar0 & CLDMA_BM_INT_DONE & BIT(queue->index))) {
			cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0,
				      BIT(queue->index));
			spin_unlock_irqrestore(&cd->timeout_lock, flags);
			retry++;
			goto again;
		}
		/* Re-enable the per-queue RX_DONE interrupt. */
		cldma_write32(cd->ap_base, CLDMA_AP_L2RIMCR0,
			      BIT(queue->index));
	}
	spin_unlock_irqrestore(&cd->timeout_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Ring and queue construction                                         */
/* ------------------------------------------------------------------ */

static void cldma_ring_free(struct ccci_cldma *cd, struct cldma_ring *ring)
{
	struct cldma_request *req, *tmp;
	struct cldma_request *bd_req, *bd_tmp;

	list_for_each_entry_safe(req, tmp, &ring->gpd_ring, entry) {
		if (req->skb) {
			if (ring->tx)
				cldma_unmap_tx_request(cd, ring, req);
			else
				dma_unmap_single(cd->dev,
						 req->data_buffer_ptr_saved,
						 cldma_skb_data_size(req->skb),
						 DMA_FROM_DEVICE);
			dev_kfree_skb_any(req->skb);
			req->skb = NULL;
		}
		list_for_each_entry_safe(bd_req, bd_tmp, &req->bd, entry) {
			list_del(&bd_req->entry);
			dma_pool_free(cd->gpd_pool, bd_req->gpd,
				      bd_req->gpd_addr);
			kfree(bd_req);
		}
		list_del(&req->entry);
		dma_pool_free(cd->gpd_pool, req->gpd, req->gpd_addr);
		kfree(req);
	}
	INIT_LIST_HEAD(&ring->gpd_ring);
}

/* modem_cldma.c:997-1031 */
static int cldma_rx_ring_init(struct ccci_cldma *cd, struct cldma_ring *ring)
{
	struct cldma_request *item, *first_item = NULL;
	struct cldma_rgpd *gpd = NULL, *prev_gpd = NULL;
	int i;

	for (i = 0; i < ring->length; i++) {
		item = kzalloc_obj(*item, GFP_KERNEL);
		if (!item)
			return -ENOMEM;
		item->gpd = dma_pool_alloc(cd->gpd_pool, GFP_KERNEL,
					   &item->gpd_addr);
		if (!item->gpd) {
			kfree(item);
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&item->bd);
		INIT_LIST_HEAD(&item->entry);
		list_add_tail(&item->entry, &ring->gpd_ring);

		item->skb = cldma_alloc_rx_skb(ring->pkt_size);
		if (!item->skb)
			return -ENOMEM;
		item->data_buffer_ptr_saved =
			dma_map_single(cd->dev, item->skb->data,
				       cldma_skb_data_size(item->skb),
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(cd->dev,
				      item->data_buffer_ptr_saved)) {
			dev_kfree_skb_any(item->skb);
			item->skb = NULL;
			return -EIO;
		}

		gpd = (struct cldma_rgpd *)item->gpd;
		memset(gpd, 0, sizeof(*gpd));
		gpd->data_buff_bd_ptr =
			lower_32_bits(item->data_buffer_ptr_saved);
		gpd->data_allow_len = ring->pkt_size;
		gpd->gpd_flags = CLDMA_RGPD_HWO_IOC;	/* IOC | HWO */
		if (i == 0) {
			first_item = item;
		} else {
			prev_gpd->next_gpd_ptr =
				lower_32_bits(item->gpd_addr);
			cldma_checksum(prev_gpd, CLDMA_RGPD_HWO_IOC);
		}
		prev_gpd = gpd;
	}
	if (!first_item)
		return 0;
	prev_gpd->next_gpd_ptr = lower_32_bits(first_item->gpd_addr);
	cldma_checksum(prev_gpd, CLDMA_RGPD_HWO_IOC);
	return 0;
}

/* modem_cldma.c:1033-1092 */
static int cldma_tx_ring_init(struct ccci_cldma *cd, struct cldma_ring *ring)
{
	struct cldma_request *item, *first_item = NULL;
	struct cldma_tgpd *gpd = NULL, *prev_gpd = NULL;
	int i, j;

	for (i = 0; i < ring->length; i++) {
		item = kzalloc_obj(*item, GFP_KERNEL);
		if (!item)
			return -ENOMEM;
		item->gpd = dma_pool_alloc(cd->gpd_pool, GFP_KERNEL,
					   &item->gpd_addr);
		if (!item->gpd) {
			kfree(item);
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&item->bd);
		INIT_LIST_HEAD(&item->entry);
		list_add_tail(&item->entry, &ring->gpd_ring);

		gpd = (struct cldma_tgpd *)item->gpd;
		memset(gpd, 0, sizeof(*gpd));

		if (ring->type == RING_GPD_BD) {
			struct cldma_request *bd_item, *prev_bd = NULL;

			gpd->gpd_flags = CLDMA_TGPD_IOC_BDP;	/* IOC | BDP */
			for (j = 0; j < CLDMA_MAX_BD_NUM + 1; j++) {
				struct cldma_tbd *bd;

				bd_item = kzalloc_obj(*bd_item, GFP_KERNEL);
				if (!bd_item)
					return -ENOMEM;
				bd_item->gpd = dma_pool_alloc(cd->gpd_pool,
							      GFP_KERNEL,
							      &bd_item->gpd_addr);
				if (!bd_item->gpd) {
					kfree(bd_item);
					return -ENOMEM;
				}
				INIT_LIST_HEAD(&bd_item->entry);
				list_add_tail(&bd_item->entry, &item->bd);

				bd = (struct cldma_tbd *)bd_item->gpd;
				memset(bd, 0, sizeof(*bd));
				if (j == 0) {
					gpd->data_buff_bd_ptr =
						lower_32_bits(bd_item->gpd_addr);
				} else {
					struct cldma_tbd *prev_bd_gpd =
						prev_bd->gpd;

					prev_bd_gpd->next_bd_ptr =
						lower_32_bits(bd_item->gpd_addr);
				}
				prev_bd = bd_item;
			}
			((struct cldma_tbd *)prev_bd->gpd)->bd_flags |=
				CLDMA_BD_EOL;
		} else {
			gpd->gpd_flags = CLDMA_TGPD_IOC;	/* IOC */
		}

		if (i == 0) {
			first_item = item;
		} else {
			prev_gpd->next_gpd_ptr =
				lower_32_bits(item->gpd_addr);
			cldma_checksum(prev_gpd, prev_gpd->gpd_flags);
		}
		prev_gpd = gpd;
	}
	if (!first_item)
		return 0;
	prev_gpd->next_gpd_ptr = lower_32_bits(first_item->gpd_addr);
	cldma_checksum(prev_gpd, prev_gpd->gpd_flags);
	return 0;
}

/*
 * modem_cldma.c:1094-1133. Network queues (3/4/5) use their 1.5K net ring in
 * normal mode. In exception mode a network queue moves to the 4K normal ring
 * only when CLDMA_NORMAL_QUEUE_MASK has its bit: queue 3 does (it carries the
 * exception-mode MD logger control traffic), queues 4/5 have no normal ring
 * (normal_{tx,rx}_queue2ring[4/5] == -1) and keep the net ring, mirroring the
 * downstream "else if" branch that has no else. The non-network queues always
 * use the normal ring.
 */
static void cldma_queue_switch_ring(struct ccci_cldma *cd,
				    struct cldma_queue *queue)
{
	struct cldma_ring *ring;
	struct cldma_request *req;
	int mapped;

	if (BIT(queue->index) & CLDMA_NET_QUEUE_MASK) {
		if (!cd->exception_mode) {
			if (queue->tx) {
				mapped = cldma_net_tx_queue2ring[queue->index];
				ring = &cd->net_tx_ring[mapped];
			} else {
				mapped = cldma_net_rx_queue2ring[queue->index];
				ring = &cd->net_rx_ring[mapped];
			}
		} else if (BIT(queue->index) & CLDMA_NORMAL_QUEUE_MASK) {
			if (queue->tx) {
				mapped = cldma_normal_tx_queue2ring[queue->index];
				ring = &cd->normal_tx_ring[mapped];
			} else {
				mapped = cldma_normal_rx_queue2ring[queue->index];
				ring = &cd->normal_rx_ring[mapped];
			}
		} else {
			/* Queues 4/5: no exception ring, keep the net ring. */
			ring = queue->tr_ring;
			if (!ring) {
				if (queue->tx)
					ring = &cd->net_tx_ring[
						cldma_net_tx_queue2ring[queue->index]];
				else
					ring = &cd->net_rx_ring[
						cldma_net_rx_queue2ring[queue->index]];
			}
		}
	} else {
		if (queue->tx) {
			mapped = cldma_normal_tx_queue2ring[queue->index];
			ring = &cd->normal_tx_ring[mapped];
		} else {
			mapped = cldma_normal_rx_queue2ring[queue->index];
			ring = &cd->normal_rx_ring[mapped];
		}
	}

	queue->tr_ring = ring;
	req = list_first_entry(&ring->gpd_ring, struct cldma_request, entry);
	queue->tr_done = req;
	if (queue->tx)
		queue->tx_xmit = req;
	else
		queue->rx_refill = req;

	dev_dbg(cd->dev, "queue %u/%s switch ring to length %d\n",
		queue->index, queue->tx ? "tx" : "rx", ring->length);
}

/*
 * Re-point every queue at the ring selected by cd->exception_mode and reset
 * its cursors. Only valid while the engine is idle: downstream does the same
 * from cldma_queue_switch_ring() when a queue is initialised or when
 * cldma_start() arms the rings (modem_cldma.c:1135-1180,1569-1617).
 */
static void cldma_apply_ring_map(struct ccci_cldma *cd)
{
	int i;

	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		spin_lock(&cd->txq[i].ring_lock);
		cldma_queue_switch_ring(cd, &cd->txq[i]);
		spin_unlock(&cd->txq[i].ring_lock);
	}
	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		spin_lock(&cd->rxq[i].ring_lock);
		cldma_queue_switch_ring(cd, &cd->rxq[i]);
		spin_unlock(&cd->rxq[i].ring_lock);
	}
}

/*
 * modem_cldma.c:308-330 timer and 1135-1180 queue setup. Each CLDMA queue
 * gets its own ordered workqueue so that work for one queue never runs
 * concurrently with itself.
 */
static int cldma_queue_init(struct ccci_cldma *cd, struct cldma_queue *queue,
			    bool tx, unsigned int index)
{
	unsigned int wq_flags = WQ_UNBOUND | WQ_MEM_RECLAIM;
	int ret;

	queue->index = index;
	queue->tx = tx;
	queue->cd = cd;
	queue->initialized = true;
	spin_lock_init(&queue->ring_lock);

	if (tx) {
		queue->worker = alloc_workqueue("cldma_tx%u",
						wq_flags | WQ_HIGHPRI, 1,
						index);
		if (!queue->worker)
			return -ENOMEM;
		INIT_DELAYED_WORK(&queue->tx_work, cldma_tx_done);
		timer_setup(&queue->timeout_timer,
			    cldma_timeout_timer_func, 0);
	} else {
		if (index != CLDMA_LOW_PRIORITY_QUEUE)
			wq_flags |= WQ_HIGHPRI;

		skb_queue_head_init(&queue->rx_list);
		init_waitqueue_head(&queue->rx_wq);
		queue->rx_stop = false;

		queue->worker = alloc_workqueue("cldma_rx%u", wq_flags, 1,
						index);
		if (!queue->worker)
			return -ENOMEM;
		queue->refill_worker = alloc_workqueue("cldma_rx%u_refill",
						       wq_flags, 1, index);
		if (!queue->refill_worker) {
			destroy_workqueue(queue->worker);
			queue->worker = NULL;
			return -ENOMEM;
		}
		INIT_WORK(&queue->rx_work, cldma_rx_done);
		INIT_WORK(&queue->refill_work, cldma_rx_refill);

		if (BIT(index) & CLDMA_NET_QUEUE_MASK) {
			queue->rx_thread =
				kthread_run(cldma_net_rx_push_thread, queue,
					    "cldma_rxq%u", index);
			if (IS_ERR(queue->rx_thread)) {
				ret = PTR_ERR(queue->rx_thread);
				queue->rx_thread = NULL;
				destroy_workqueue(queue->refill_worker);
				queue->refill_worker = NULL;
				destroy_workqueue(queue->worker);
				queue->worker = NULL;
				return ret;
			}
		}
	}

	cldma_queue_switch_ring(cd, queue);
	return 0;
}

static void cldma_queue_free(struct cldma_queue *queue)
{
	struct sk_buff *skb;

	if (!queue->initialized)
		return;

	if (queue->rx_thread) {
		WRITE_ONCE(queue->rx_stop, true);
		wake_up_interruptible_all(&queue->rx_wq);
		kthread_stop(queue->rx_thread);
		queue->rx_thread = NULL;
	}

	if (queue->worker) {
		if (queue->tx) {
			timer_delete_sync(&queue->timeout_timer);
			cancel_delayed_work_sync(&queue->tx_work);
		} else {
			cancel_work_sync(&queue->rx_work);
			cancel_work_sync(&queue->refill_work);
		}
		destroy_workqueue(queue->worker);
		queue->worker = NULL;
	}
	if (queue->refill_worker) {
		destroy_workqueue(queue->refill_worker);
		queue->refill_worker = NULL;
	}

	if (!queue->tx) {
		while ((skb = skb_dequeue(&queue->rx_list)))
			dev_kfree_skb_any(skb);
	}
}

/* ------------------------------------------------------------------ */
/* Interrupt handling                                                  */
/* ------------------------------------------------------------------ */

static irqreturn_t cldma_isr(int irq, void *data)
{
	return IRQ_WAKE_THREAD;
}

/*
 * modem_cldma.c:1207-1327. Runs in the threaded IRQ handler with the
 * controller's line masked (IRQF_ONESHOT), which replaces downstream's
 * disable_irq_nosync() + dedicated irq worker.
 */
static irqreturn_t cldma_irq_thread(int irq, void *data)
{
	struct ccci_cldma *cd = data;
	u32 l2t, l2r, l2tm, l2rm;
	u32 l3t0, l3t1, l3r0, l3r1, l3tm0, l3tm1, l3rm0, l3rm1;
	u32 coda_version;
	int i;

	l2t = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2TISAR0);
	l2r = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0);
	l2tm = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L2TIMR0);
	l2rm = cldma_read32(cd->ap_base, CLDMA_AP_L2RIMR0);

	l3t0 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TISAR0);
	l3t1 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TISAR1);
	l3r0 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RISAR0);
	l3r1 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RISAR1);
	l3tm0 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TIMR0);
	l3tm1 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3TIMR1);
	l3rm0 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RIMR0);
	l3rm1 = cldma_read32(cd->ap_pdn_base, CLDMA_AP_L3RIMR1);

	l2t &= ~l2tm;
	l3t0 &= ~l3tm0;
	l3t1 &= ~l3tm1;
	l2r &= ~l2rm;
	l3r0 &= ~l3rm0;
	l3r1 &= ~l3rm1;

	dev_dbg(cd->dev, "CLDMA IRQ L2(0x%08x/0x%08x) L3(0x%08x/0x%08x)\n",
		l2t, l2r, l3t0, l3r0);

	if (l2t & CLDMA_BM_INT_ERROR)
		dev_err_ratelimited(cd->dev, "CLDMA TX error, L2TISAR0=0x%08x L3TISAR0=0x%08x L3TISAR1=0x%08x\n",
				     l2t, l3t0, l3t1);
	if (l2r & CLDMA_BM_INT_ERROR)
		dev_err_ratelimited(cd->dev, "CLDMA RX error, L2RISAR0=0x%08x L3RISAR0=0x%08x L3RISAR1=0x%08x\n",
				     l2r, l3r0, l3r1);

	if (!(l2r & CLDMA_BM_INT_DONE) && !(l2t & CLDMA_BM_INT_DONE)) {
		coda_version = cldma_read32(cd->ap_pdn_base,
					    CLDMA_AP_CLDMA_CODA_VERSION);
		if (!coda_version)
			dev_err_ratelimited(cd->dev,
					    "CLDMA IRQ without Tx/Rx done, CODA_VERSION=0\n");
	}

	/* Acknowledge and dispatch TX completions. */
	if (l2t) {
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TISAR0, l2t);
		for (i = 0; i < CLDMA_TXQ_NUM; i++) {
			if (!(l2t & CLDMA_BM_INT_DONE & BIT(i)))
				continue;
			if (i == CLDMA_VT_QUEUE)
				timer_delete(&cd->txq[i].timeout_timer);
			/* Mask TX_DONE until tx_done() has drained the ring. */
			cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMSR0,
				      BIT(i));
			queue_delayed_work(cd->txq[i].worker,
					   &cd->txq[i].tx_work,
					   msecs_to_jiffies((BIT(i) &
						CLDMA_NET_QUEUE_MASK) ? 10 : 0));
		}
	}

	/* Acknowledge and dispatch RX completions. */
	if (l2r) {
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0, l2r);
		/* Clear the IP-busy wakeup latched by the MD side. */
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_CLDMA_IP_BUSY,
			      cldma_read32(cd->ap_pdn_base,
					   CLDMA_AP_CLDMA_IP_BUSY));
		for (i = 0; i < CLDMA_RXQ_NUM; i++) {
			if (!(l2r & CLDMA_BM_INT_DONE & BIT(i)))
				continue;
			/* Mask RX_DONE until rx_done() has drained the ring. */
			cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR0, BIT(i));
			cldma_read32(cd->ap_base, CLDMA_AP_L2RIMSR0);
			queue_work(cd->rxq[i].worker, &cd->rxq[i].rx_work);
		}
	}

	return IRQ_HANDLED;
}

static void cldma_enable_irq(struct ccci_cldma *cd)
{
	if (atomic_cmpxchg(&cd->irq_enabled, 0, 1) == 0)
		enable_irq(cd->irq);
}

static void cldma_disable_irq(struct ccci_cldma *cd)
{
	if (atomic_cmpxchg(&cd->irq_enabled, 1, 0) == 1)
		disable_irq(cd->irq);
}

/* ------------------------------------------------------------------ */
/* Hardware bring-up / shutdown                                        */
/* ------------------------------------------------------------------ */

/* modem_cldma.c:1526-1567 */
static void cldma_hw_config(struct ccci_cldma *cd)
{
	u32 val;

	/* Enable OUT DMA and wait for RGPD write responses. */
	val = cldma_read32(cd->ap_base, CLDMA_AP_SO_CFG);
	cldma_write32(cd->ap_base, CLDMA_AP_SO_CFG, val | 0x5);
	/* Enable SPLIT_EN on the bus interface. */
	val = cldma_read32(cd->ap_base, CLDMA_AP_BUS_CFG);
	cldma_write32(cd->ap_base, CLDMA_AP_BUS_CFG, val | 0x02);
	/* No queue is high priority. */
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_HPQR,
		      CLDMA_HIGH_PRIORITY_QUEUE_MASK);
	/* CHECKSUM_SIZE == 0: checksum checking disabled on all queues. */
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_UL_CHECKSUM_CHANNEL_ENABLE, 0);
	cldma_write32(cd->ap_base, CLDMA_AP_SO_CHECKSUM_CHANNEL_ENABLE, 0);
}

/* modem_cldma.c:1569-1617 */
static void cldma_hw_start(struct ccci_cldma *cd)
{
	unsigned long flags, ring_flags;
	int i;

	cldma_enable_irq(cd);

	spin_lock_irqsave(&cd->timeout_lock, flags);
	/*
	 * Tell the engine where each ring starts. The ring lock keeps a
	 * concurrent ccci_cldma_send() (the core still accepts TX in the
	 * EXCEPTION state) or a late tx_done() from moving the cursor while
	 * the ring is re-pointed and armed; lock order is timeout_lock ->
	 * ring_lock everywhere (see cldma_apply_ring_map()).
	 */
	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		spin_lock_irqsave(&cd->txq[i].ring_lock, ring_flags);
		cldma_queue_switch_ring(cd, &cd->txq[i]);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_TQSAR(i),
			      lower_32_bits(cd->txq[i].tr_done->gpd_addr));
		spin_unlock_irqrestore(&cd->txq[i].ring_lock, ring_flags);
	}
	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		spin_lock_irqsave(&cd->rxq[i].ring_lock, ring_flags);
		cldma_queue_switch_ring(cd, &cd->rxq[i]);
		cldma_write32(cd->ap_base, CLDMA_AP_RQSAR(i),
			      lower_32_bits(cd->rxq[i].tr_done->gpd_addr));
		spin_unlock_irqrestore(&cd->rxq[i].ring_lock, ring_flags);
	}
	/* Make sure the start addresses are visible before the start command. */
	wmb();

	cldma_write32(cd->ap_pdn_base, CLDMA_AP_UL_START_CMD,
		      CLDMA_BM_ALL_QUEUE);
	cldma_read32(cd->ap_pdn_base, CLDMA_AP_UL_START_CMD);
	cd->txq_active |= CLDMA_BM_ALL_QUEUE;

	cldma_write32(cd->ap_pdn_base, CLDMA_AP_SO_START_CMD,
		      CLDMA_BM_ALL_QUEUE);
	cldma_read32(cd->ap_pdn_base, CLDMA_AP_SO_START_CMD);
	cd->rxq_active |= CLDMA_BM_ALL_QUEUE;

	/* Unmask L2 DONE and ERROR for both directions. */
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMCR0,
		      CLDMA_BM_INT_DONE | CLDMA_BM_INT_ERROR);
	cldma_write32(cd->ap_base, CLDMA_AP_L2RIMCR0,
		      CLDMA_BM_INT_DONE | CLDMA_BM_INT_ERROR);
	/* Unmask every L3 error source. */
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMCR0, 0xFFFFFFFF);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMCR1, 0xFFFFFFFF);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMCR0, 0xFFFFFFFF);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMCR1, 0xFFFFFFFF);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);
}

/* modem_cldma.c:1354-1447 */
static void cldma_hw_stop(struct ccci_cldma *cd)
{
	unsigned long flags;
	u32 status;
	int count, i;

	/* Freeze the queues first so nobody can kick them again. */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	cd->txq_active &= ~CLDMA_BM_ALL_QUEUE;
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_UL_STOP_CMD,
		      CLDMA_BM_ALL_QUEUE);
	cldma_read32(cd->ap_pdn_base, CLDMA_AP_UL_STOP_CMD);
	cd->rxq_active &= ~CLDMA_BM_ALL_QUEUE;
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_SO_STOP_CMD,
		      CLDMA_BM_ALL_QUEUE);
	cldma_read32(cd->ap_pdn_base, CLDMA_AP_SO_STOP_CMD);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	/* Wait for the TX engine to go idle. */
	for (count = 0; count < CLDMA_STOP_POLL_MAX; count++) {
		status = cldma_read32(cd->ap_pdn_base, CLDMA_AP_UL_STATUS);
		if (!status)
			break;
		usleep_range(CLDMA_STOP_POLL_US, CLDMA_STOP_POLL_US * 2);
	}
	if (status)
		dev_err(cd->dev, "stop Tx CLDMA timed out, UL_STATUS=0x%08x\n",
			status);

	/* Wait for the RX engine to go idle. */
	for (count = 0; count < CLDMA_STOP_POLL_MAX; count++) {
		status = cldma_read32(cd->ap_base, CLDMA_AP_SO_STATUS);
		if (!status)
			break;
		usleep_range(CLDMA_STOP_POLL_US, CLDMA_STOP_POLL_US * 2);
	}
	if (status) {
		dev_err(cd->dev, "stop Rx CLDMA timed out, SO_STATUS=0x%08x\n",
			status);
		cldma_dump_register(cd);
	}

	/* Clear every L2/L3 status and mask all sources. */
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TISAR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TISAR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR1, CLDMA_BM_INT_ALL);

	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR1, CLDMA_BM_INT_ALL);

	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		timer_delete_sync(&cd->txq[i].timeout_timer);

	/* Stop the interrupt source before flushing the workers. */
	cldma_disable_irq(cd);

	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		flush_delayed_work(&cd->txq[i].tx_work);
	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		flush_work(&cd->rxq[i].rx_work);
		flush_work(&cd->rxq[i].refill_work);
	}
}

/* ------------------------------------------------------------------ */
/* Exception-time stop and the DHL RX window                           */
/* ------------------------------------------------------------------ */

/*
 * Port from Samsung/MTK downstream cldma_stop_for_ee() and md_cd_stop_queue()
 * (modem_cldma.c:1449-1520, 1694-1730, inlined in the ROM inside
 * md_cd_ccif_work, vmlinux.elf 0xc05285cc-0xc0528768 and
 * 0xc0528714-0xc0528744):
 *
 *	write CLDMA_AP_{UL,SO}_STOP_CMD <- mask
 *	read  CLDMA_AP_{UL,SO}_STOP_CMD		(dummy read, posts the write)
 *	read  CLDMA_AP_{UL,SO}_STATUS & mask	(only the queues we stop)
 *	repeat until the masked status is zero
 *
 * Every CLDMA_EE_STOP_DUMP_INTERVAL polls the engine state is dumped; the poll
 * is bounded by CLDMA_EE_STOP_POLL_MAX. Downstream runs this loop with
 * cldma_timeout_lock held and raises AEE from inside it; here the loop runs
 * outside the spinlock (like cldma_hw_stop() above) under start_lock, and the
 * timeout is reported to the caller, which owns the AEE notification.
 */
static int cldma_ee_stop_poll(struct ccci_cldma *cd, void __iomem *cmd_base,
			      u32 cmd_off, void __iomem *status_base,
			      u32 status_off, u32 mask, const char *what)
{
	u32 status;
	int count = 0;

	do {
		cldma_write32(cmd_base, cmd_off, mask);
		cldma_read32(cmd_base, cmd_off);	/* dummy read */
		status = cldma_read32(status_base, status_off) & mask;
		if (++count % CLDMA_EE_STOP_DUMP_INTERVAL == 0) {
			dev_info(cd->dev,
				 "stop %s CLDMA E, status=0x%08x, count=%d\n",
				 what, status, count);
			cldma_dump_register(cd);
			if (count >= CLDMA_EE_STOP_POLL_MAX) {
				dev_err(cd->dev,
					"stop %s CLDMA for EE failed, status=0x%08x\n",
					what, status);
				return -ETIMEDOUT;
			}
		}
	} while (status);

	return 0;
}

/*
 * Port from Samsung/MTK downstream cldma_stop_for_ee() (modem_cldma.c:1449-1520),
 * inlined in the ROM in md_cd_ccif_work() at vmlinux.elf 0xc05285d0-0xc0528928.
 *
 * Stop all eight TX queues and the RX queues outside CLDMA_EE_ALIVE_RXQ_MASK,
 * then clear and mask the L2/L3 latches of exactly those queues, so the DHL RX
 * queues (0..3) stay armed and keep their DONE|ERROR interrupts unmasked for
 * the rest of the exception handshake.
 *
 * Returns 0, or -ETIMEDOUT when the engine never reported the stopped queues
 * idle. The latch and mask programming is applied in both cases, exactly like
 * downstream, which continues the handshake after reporting to AEE.
 */
int ccci_cldma_stop_for_ee(struct ccci_cldma *cd)
{
	unsigned long flags;
	int ret_tx, ret_rx;

	if (!cd || IS_ERR(cd))
		return -EINVAL;

	mutex_lock(&cd->start_lock);

	/* Stop every TX queue: UL_STOP_CMD = CLDMA_BM_ALL_QUEUE (0xFF). */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	cd->txq_active &= ~CLDMA_BM_ALL_QUEUE;
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	ret_tx = cldma_ee_stop_poll(cd, cd->ap_pdn_base, CLDMA_AP_UL_STOP_CMD,
				    cd->ap_pdn_base, CLDMA_AP_UL_STATUS,
				    CLDMA_BM_ALL_QUEUE, "Tx");

	/*
	 * Stop the RX queues outside the DHL set: SO_STOP_CMD = 0xF0. The
	 * active mask is updated first so that no refill work resumes them
	 * while they are being stopped.
	 */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	cd->rxq_active &= ~(CLDMA_BM_ALL_QUEUE & CLDMA_NONSTOP_QUEUE_MASK);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	ret_rx = cldma_ee_stop_poll(cd, cd->ap_pdn_base, CLDMA_AP_SO_STOP_CMD,
				    cd->ap_base, CLDMA_AP_SO_STATUS,
				    CLDMA_BM_ALL_QUEUE &
				    CLDMA_NONSTOP_QUEUE_MASK, "Rx");

	/*
	 * Clear the latches of the stopped queues (downstream comment: "clear
	 * all L2 and L3 interrupts, but non-stop Rx ones") and mask their
	 * sources. The RX DONE|ERROR bits of the DHL queues are left untouched
	 * in L2RIMSR0, i.e. they stay unmasked.
	 */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TISAR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR1,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TISAR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TISAR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR0,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR1,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);

	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2TIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR0,
		      (CLDMA_BM_INT_DONE | CLDMA_BM_INT_ERROR) &
		      CLDMA_NONSTOP_QUEUE_MASK_32);
	cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMSR0, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3TIMSR1, CLDMA_BM_INT_ALL);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR0,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);
	cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR1,
		      CLDMA_BM_INT_ALL & CLDMA_NONSTOP_QUEUE_MASK_32);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	mutex_unlock(&cd->start_lock);

	return ret_tx ? ret_tx : ret_rx;
}
EXPORT_SYMBOL_GPL(ccci_cldma_stop_for_ee);

/*
 * Open/close the DHL RX window.
 *
 * stop_for_ee() leaves RX queues 0..3 armed and unmasked; that is the window
 * the modem pushes its exception log through (the log control port uses RX
 * queue 3 in exception mode, see CLDMA_EE_ALIVE_RXQ_MASK). alive == true
 * re-arms that set with the per-queue sequence of downstream
 * md_cd_start_queue(IN) (modem_cldma.c:1746-1770): SO_START_CMD with the queue
 * bits, clear their latches, unmask DONE|ERROR through L2RIMCR0 and mark them
 * active. alive == false closes it with the per-queue sequence of
 * md_cd_stop_queue(IN) (modem_cldma.c:1694-1730): mask their L2 sources,
 * SO_STOP_CMD with the queue bits, poll SO_STATUS until they report idle, then
 * clear the latches.
 *
 * L3 has no per-queue bit attribution in any available source: downstream
 * stop_for_ee() masks L3RIMSR0/1 with CLDMA_NONSTOP_QUEUE_MASK_32 and the full
 * stop masks them with CLDMA_BM_INT_ALL (modem_cldma.c:1417-1435). Closing the
 * window is immediately followed by the full stop in the exception flow, so
 * the full-stop value is used here.
 *
 * Returns 0, or -ETIMEDOUT if the DHL queues never report stopped when closing.
 */
int ccci_cldma_dhl_rx_alive(struct ccci_cldma *cd, bool alive)
{
	unsigned long flags;
	int ret = 0;

	if (!cd || IS_ERR(cd))
		return -EINVAL;

	mutex_lock(&cd->start_lock);

	if (alive) {
		spin_lock_irqsave(&cd->timeout_lock, flags);
		cd->rxq_active |= CLDMA_EE_ALIVE_RXQ_MASK;
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_SO_START_CMD,
			      CLDMA_EE_ALIVE_RXQ_MASK);
		cldma_read32(cd->ap_pdn_base, CLDMA_AP_SO_START_CMD);
		/* Drop stale latches before unmasking the sources. */
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0,
			      CLDMA_EE_ALIVE_RXQ_INT);
		cldma_write32(cd->ap_base, CLDMA_AP_L2RIMCR0,
			      CLDMA_EE_ALIVE_RXQ_INT);
		spin_unlock_irqrestore(&cd->timeout_lock, flags);
		dev_info(cd->dev, "CLDMA DHL Rx window open (rxq 0..3)\n");
	} else {
		spin_lock_irqsave(&cd->timeout_lock, flags);
		cd->rxq_active &= ~CLDMA_EE_ALIVE_RXQ_MASK;
		cldma_write32(cd->ap_base, CLDMA_AP_L2RIMSR0,
			      CLDMA_EE_ALIVE_RXQ_INT);
		spin_unlock_irqrestore(&cd->timeout_lock, flags);

		ret = cldma_ee_stop_poll(cd, cd->ap_pdn_base,
					 CLDMA_AP_SO_STOP_CMD, cd->ap_base,
					 CLDMA_AP_SO_STATUS,
					 CLDMA_EE_ALIVE_RXQ_MASK, "DHL Rx");

		spin_lock_irqsave(&cd->timeout_lock, flags);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L2RISAR0,
			      CLDMA_EE_ALIVE_RXQ_INT);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR0,
			      CLDMA_BM_INT_ALL);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RISAR1,
			      CLDMA_BM_INT_ALL);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR0,
			      CLDMA_BM_INT_ALL);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_L3RIMSR1,
			      CLDMA_BM_INT_ALL);
		spin_unlock_irqrestore(&cd->timeout_lock, flags);
		dev_info(cd->dev, "CLDMA DHL Rx window closed (rxq 0..3)\n");
	}

	mutex_unlock(&cd->start_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(ccci_cldma_dhl_rx_alive);

/* modem_cldma.c:1619-1693, only valid while the engine is stopped. */
static void cldma_clear_tx_queue(struct ccci_cldma *cd,
				 struct cldma_queue *queue)
{
	struct cldma_request *req;
	struct cldma_tgpd *tgpd;
	unsigned long flags;

	if (!queue->tr_ring || queue->tr_ring->length == 0)
		return;

	spin_lock_irqsave(&queue->ring_lock, flags);
	req = list_first_entry(&queue->tr_ring->gpd_ring,
			       struct cldma_request, entry);
	queue->tr_done = req;
	queue->tx_xmit = req;
	queue->debug_id = 0;
	list_for_each_entry(req, &queue->tr_ring->gpd_ring, entry) {
		tgpd = (struct cldma_tgpd *)req->gpd;
		WRITE_ONCE(tgpd->gpd_flags,
			   READ_ONCE(tgpd->gpd_flags) & ~CLDMA_HWO);
		if (queue->tr_ring->type != RING_GPD_BD)
			tgpd->data_buff_bd_ptr = 0;
		tgpd->data_buff_len = 0;
		if (req->skb) {
			cldma_unmap_tx_request(cd, queue->tr_ring, req);
			dev_kfree_skb_any(req->skb);
			req->skb = NULL;
		}
	}
	spin_unlock_irqrestore(&queue->ring_lock, flags);
}

static void cldma_clear_rx_queue(struct ccci_cldma *cd,
				 struct cldma_queue *queue)
{
	struct cldma_request *req;
	struct cldma_rgpd *rgpd;
	struct sk_buff *skb;
	unsigned long flags;

	if (!queue->tr_ring || queue->tr_ring->length == 0)
		return;

	spin_lock_irqsave(&queue->ring_lock, flags);
	req = list_first_entry(&queue->tr_ring->gpd_ring,
			       struct cldma_request, entry);
	queue->tr_done = req;
	queue->rx_refill = req;
	list_for_each_entry(req, &queue->tr_ring->gpd_ring, entry) {
		rgpd = (struct cldma_rgpd *)req->gpd;
		rgpd->gpd_flags = CLDMA_RGPD_HWO_IOC;
		rgpd->data_buff_len = 0;
		cldma_checksum(rgpd, CLDMA_RGPD_HWO_IOC);
		if (req->skb) {
			req->skb->len = 0;
			skb_reset_tail_pointer(req->skb);
			rgpd->data_buff_bd_ptr =
				lower_32_bits(req->data_buffer_ptr_saved);
		}
	}
	spin_unlock_irqrestore(&queue->ring_lock, flags);

	/* Re-arm the descriptors whose skb was consumed. */
	list_for_each_entry(req, &queue->tr_ring->gpd_ring, entry) {
		if (req->skb)
			continue;
		skb = cldma_alloc_rx_skb(queue->tr_ring->pkt_size);
		if (!skb) {
			dev_err(cd->dev, "refill skb fail on rxq%u\n",
				queue->index);
			continue;
		}
		rgpd = (struct cldma_rgpd *)req->gpd;
		req->data_buffer_ptr_saved =
			dma_map_single(cd->dev, skb->data,
				       cldma_skb_data_size(skb),
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(cd->dev,
				      req->data_buffer_ptr_saved)) {
			dev_err(cd->dev, "dma map RX failed on q%u\n",
				queue->index);
			dev_kfree_skb_any(skb);
			continue;
		}
		rgpd->data_buff_bd_ptr =
			lower_32_bits(req->data_buffer_ptr_saved);
		rgpd->data_buff_len = 0;
		cldma_checksum(rgpd, CLDMA_RGPD_HWO_IOC);
		/* Publish the RGPD contents before the HWO bit. */
		wmb();
		WRITE_ONCE(rgpd->gpd_flags, CLDMA_RGPD_HWO_IOC);
		req->skb = skb;
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * TX sequence numbers, modem_cldma.c:2862 and ccci_core.h:895-910
 * (FEATURE_SEQ_CHECK_EN is enabled on mt6735, mt6735/ccci_config.h:32).
 *
 * Called with queue->ring_lock held, at the same point the GPD is reserved,
 * so the sequence number follows the order in which descriptors are handed
 * to the engine. Downstream keeps the counters in struct ccci_modem but
 * increments them here.
 */
static bool cldma_tx_seq_inc(struct ccci_cldma *cd, struct ccci_header *h)
{
	u16 ch = h->channel;

	if (ch >= CCCI_MAX_CH_NUM) {
		/* Force-assert / monitor channels have no sequence space. */
		dev_info_ratelimited(cd->dev,
				     "ignore seq inc on channel 0x%x\n", ch);
		return false;
	}

	h->seq_num = cd->tx_seq_num[ch]++;
	/*
	 * ccci_core.h:907-908: RPC and FS may only assert in the single-task
	 * phase. The core owns the boot stage, so it sets assert_bit for those
	 * two channels; every other channel always asserts.
	 */
	if (ch != CCCI_RPC_TX && ch != CCCI_FS_TX)
		h->assert_bit = 1;

	return true;
}

static void cldma_tx_seq_dec(struct ccci_cldma *cd, const struct ccci_header *h)
{
	u16 ch = h->channel;

	if (ch < CCCI_MAX_CH_NUM)
		cd->tx_seq_num[ch]--;
}

struct ccci_cldma *ccci_cldma_create(const struct ccci_cldma_cfg *cfg)
{
	struct ccci_cldma *cd;
	int i, ret;

	if (!cfg || !cfg->dev || !cfg->ap_base || !cfg->ap_pdn_base ||
	    !cfg->irq)
		return ERR_PTR(-EINVAL);

	cd = kzalloc_obj(*cd, GFP_KERNEL);
	if (!cd)
		return ERR_PTR(-ENOMEM);

	cd->dev = cfg->dev;
	cd->ap_base = cfg->ap_base;
	cd->ap_pdn_base = cfg->ap_pdn_base;
	cd->irq = cfg->irq;
	spin_lock_init(&cd->timeout_lock);
	mutex_init(&cd->start_lock);
	atomic_set(&cd->irq_enabled, 0);

	/* Always initialise the list heads: the error paths free them. */
	for (i = 0; i < CLDMA_NET_TXQ_NUM; i++)
		INIT_LIST_HEAD(&cd->net_tx_ring[i].gpd_ring);
	for (i = 0; i < CLDMA_NET_RXQ_NUM; i++)
		INIT_LIST_HEAD(&cd->net_rx_ring[i].gpd_ring);
	for (i = 0; i < CLDMA_NORMAL_TXQ_NUM; i++)
		INIT_LIST_HEAD(&cd->normal_tx_ring[i].gpd_ring);
	for (i = 0; i < CLDMA_NORMAL_RXQ_NUM; i++)
		INIT_LIST_HEAD(&cd->normal_rx_ring[i].gpd_ring);

	/*
	 * The MT6735 CLDMA is a 32-bit DMA master; downstream set the same
	 * mask on the platform device (modem_cldma.c:3825-3826).
	 */
	ret = dma_set_mask_and_coherent(cd->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(cd->dev, "failed to set 32-bit DMA mask: %d\n", ret);
		goto err_release;
	}

	cd->gpd_pool = dma_pool_create("cldma_gpd", cd->dev,
				       sizeof(struct cldma_tgpd), 16, 0);
	if (!cd->gpd_pool) {
		ret = -ENOMEM;
		goto err_release;
	}

	/* Network TX rings are BD based (CLDMA_NET_TX_BD, modem_cldma.h:53). */
	for (i = 0; i < CLDMA_NET_TXQ_NUM; i++) {
		struct cldma_ring *ring = &cd->net_tx_ring[i];
		int q = cldma_net_tx_ring2queue[i];

		ring->tx = true;
		ring->length = cldma_net_tx_queue_buffer_number[q];
		ring->type = RING_GPD_BD;
		ring->handle_tx_request = cldma_gpd_bd_handle_tx_request;
		ring->handle_tx_done = cldma_gpd_bd_tx_collect;
		ret = cldma_tx_ring_init(cd, ring);
		if (ret)
			goto err_release;
	}
	for (i = 0; i < CLDMA_NET_RXQ_NUM; i++) {
		struct cldma_ring *ring = &cd->net_rx_ring[i];
		int q = cldma_net_rx_ring2queue[i];

		ring->tx = false;
		ring->length = cldma_net_rx_queue_buffer_number[q];
		ring->pkt_size = cldma_net_rx_queue_buffer_size[q];
		ring->type = RING_GPD;
		ring->handle_rx_done = cldma_gpd_net_rx_collect;
		ring->handle_rx_refill = cldma_gpd_rx_refill;
		ret = cldma_rx_ring_init(cd, ring);
		if (ret)
			goto err_release;
	}
	for (i = 0; i < CLDMA_NORMAL_TXQ_NUM; i++) {
		struct cldma_ring *ring = &cd->normal_tx_ring[i];
		int q = cldma_normal_tx_ring2queue[i];

		ring->tx = true;
		ring->type = RING_GPD;
		ring->handle_tx_request = cldma_gpd_handle_tx_request;
		ring->handle_tx_done = cldma_gpd_tx_collect;
		/*
		 * Queue 3 has a normal ring only for exception mode
		 * (CLDMA_NORMAL_QUEUE_MASK, modem_cldma.c:113,1110); build it
		 * now because the engine may be switched later.
		 */
		ring->length = cldma_normal_tx_queue_buffer_number[q];
		ret = cldma_tx_ring_init(cd, ring);
		if (ret)
			goto err_release;
	}
	for (i = 0; i < CLDMA_NORMAL_RXQ_NUM; i++) {
		struct cldma_ring *ring = &cd->normal_rx_ring[i];
		int q = cldma_normal_rx_ring2queue[i];

		ring->tx = false;
		ring->type = RING_GPD;
		ring->handle_rx_done = cldma_gpd_rx_collect;
		ring->handle_rx_refill = cldma_gpd_rx_refill;
		ring->length = cldma_normal_rx_queue_buffer_number[q];
		ring->pkt_size = cldma_normal_rx_queue_buffer_size[q];
		ret = cldma_rx_ring_init(cd, ring);
		if (ret)
			goto err_release;
	}

	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		ret = cldma_queue_init(cd, &cd->txq[i], true, i);
		if (ret)
			goto err_release;
	}
	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		ret = cldma_queue_init(cd, &cd->rxq[i], false, i);
		if (ret)
			goto err_release;
	}

	ret = request_threaded_irq(cd->irq, cldma_isr, cldma_irq_thread,
				   IRQF_ONESHOT, "CLDMA_AP", cd);
	if (ret) {
		dev_err(cd->dev, "request CLDMA IRQ %u failed: %d\n",
			cd->irq, ret);
		goto err_release;
	}
	cd->irq_requested = true;
	/* request_threaded_irq() leaves the line enabled. */
	atomic_set(&cd->irq_enabled, 1);
	/* The engine is unmasked explicitly by ccci_cldma_start(). */
	cldma_disable_irq(cd);

	dev_info(cd->dev, "CLDMA engine ready, irq %u\n", cd->irq);
	return cd;

err_release:
	ccci_cldma_destroy(cd);
	return ERR_PTR(ret);
}

void ccci_cldma_destroy(struct ccci_cldma *cd)
{
	int i;

	if (!cd || IS_ERR(cd))
		return;

	if (READ_ONCE(cd->started))
		ccci_cldma_stop(cd);

	if (cd->irq_requested) {
		cldma_disable_irq(cd);
		free_irq(cd->irq, cd);
		cd->irq_requested = false;
	}

	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		cldma_queue_free(&cd->txq[i]);
	for (i = 0; i < CLDMA_RXQ_NUM; i++)
		cldma_queue_free(&cd->rxq[i]);

	for (i = 0; i < CLDMA_NET_TXQ_NUM; i++)
		cldma_ring_free(cd, &cd->net_tx_ring[i]);
	for (i = 0; i < CLDMA_NET_RXQ_NUM; i++)
		cldma_ring_free(cd, &cd->net_rx_ring[i]);
	for (i = 0; i < CLDMA_NORMAL_TXQ_NUM; i++)
		cldma_ring_free(cd, &cd->normal_tx_ring[i]);
	for (i = 0; i < CLDMA_NORMAL_RXQ_NUM; i++)
		cldma_ring_free(cd, &cd->normal_rx_ring[i]);

	dma_pool_destroy(cd->gpd_pool);

	kfree(cd);
}

int ccci_cldma_start(struct ccci_cldma *cd)
{
	unsigned long flags;
	u32 val;
	int i, ret;

	if (!cd || IS_ERR(cd))
		return -EINVAL;

	mutex_lock(&cd->start_lock);
	if (READ_ONCE(cd->started)) {
		mutex_unlock(&cd->start_lock);
		return -EBUSY;
	}

	/*
	 * Publish "started" before the rings are touched: a concurrent
	 * ccci_cldma_set_exception_mode() takes the same timeout_lock and
	 * only re-points rings while "started" is clear, so publishing here
	 * keeps the map stable across the whole arm sequence. The whole
	 * sequence itself is serialised by start_lock, so a parallel
	 * ccci_cldma_stop() cannot interleave either.
	 */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	WRITE_ONCE(cd->started, true);
	cldma_apply_ring_map(cd);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	/*
	 * RGU reset only on a cold start. Downstream resets the block in
	 * md_cd_start() (modem_cldma.c:2340, guarded by ENABLE_CLDMA_AP_SIDE
	 * in mt6735/ccci_config.h:44) and at the end of md_cldma_clear()
	 * (modem_cldma.c:2468). The exception restart path
	 * (HIF_EX_ALLQ_RESET) instead runs cldma_reset() + cldma_start()
	 * (modem_cldma.c:2024-2027) without any block reset; the MD has
	 * already acknowledged that stage and may still be draining data, so
	 * asserting INFRA_RST0 there would drop its in-flight descriptors.
	 * -ENODEV means the DT has no INFRA_AO window; normal mode still
	 * works without the reset, so it is not an error.
	 */
	if (!READ_ONCE(cd->exception_mode)) {
		ret = ccci_cldma_hw_reset(cd);
		if (ret && ret != -ENODEV) {
			/* Nothing was armed yet: roll the publication back. */
			WRITE_ONCE(cd->started, false);
			mutex_unlock(&cd->start_lock);
			return ret;
		}
	}
	cldma_hw_config(cd);
	/* modem_cldma.c:2316: reset the TX sequence counters before arming. */
	memset(cd->tx_seq_num, 0, sizeof(cd->tx_seq_num));
	/* modem_cldma.c:2310-2316: reset every cursor before arming. */
	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		cldma_clear_tx_queue(cd, &cd->txq[i]);
	for (i = 0; i < CLDMA_RXQ_NUM; i++)
		cldma_clear_rx_queue(cd, &cd->rxq[i]);

	cldma_hw_start(cd);

	/*
	 * modem_cldma.c:2026-2033: after arming, OUT DMA must have accepted
	 * SO_CFG bit 0 (the register is in the PD domain and a write can be
	 * lost). Downstream performs this check on the exception restart
	 * only; doing it on every arm is a superset and costs one read.
	 */
	val = cldma_read32(cd->ap_base, CLDMA_AP_SO_CFG);
	if (!(val & BIT(0))) {
		dev_err(cd->dev, "AP OUTCLDMA enable lost, SO_CFG=0x%08x\n",
			val);
		cldma_write32(cd->ap_base, CLDMA_AP_SO_CFG, val | 0x05);
	}
	mutex_unlock(&cd->start_lock);
	return 0;
}

void ccci_cldma_stop(struct ccci_cldma *cd)
{
	int i;

	if (!cd || IS_ERR(cd))
		return;

	mutex_lock(&cd->start_lock);
	if (!READ_ONCE(cd->started)) {
		mutex_unlock(&cd->start_lock);
		return;
	}

	cldma_hw_stop(cd);

	/* modem_cldma.c:2455-2467: drop pending TX, re-arm RX. */
	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		cldma_clear_tx_queue(cd, &cd->txq[i]);
	for (i = 0; i < CLDMA_RXQ_NUM; i++)
		cldma_clear_rx_queue(cd, &cd->rxq[i]);

	WRITE_ONCE(cd->started, false);

	/*
	 * Downstream finishes md_cldma_clear() with md_cldma_hw_reset()
	 * (modem_cldma.c:2468, again under ENABLE_CLDMA_AP_SIDE), after the
	 * queues are cleared, so the next start begins from the RGU reset
	 * state. The exception path (HIF_EX_INIT) calls cldma_stop_for_ee()
	 * instead (modem_cldma.c:2005) and does not reset the block, so skip
	 * it while the exception map is latched.
	 */
	if (!READ_ONCE(cd->exception_mode))
		ccci_cldma_hw_reset(cd);
	mutex_unlock(&cd->start_lock);
}

/*
 * Queue numbers are the hardware TX queues (0..7). On success the engine
 * owns the skb and frees it when the TX GPD is collected; on failure the
 * caller keeps ownership of the skb.
 */
int ccci_cldma_send(struct ccci_cldma *cd, unsigned int qno,
		    struct sk_buff *skb)
{
	struct cldma_queue *queue;
	struct cldma_request *req;
	struct ccci_header *ccci_h;
	unsigned long flags;
	bool seq_assigned;
	int ret;

	if (!cd || IS_ERR(cd) || !skb)
		return -EINVAL;
	if (qno >= CLDMA_TXQ_NUM)
		return -EINVAL;
	if (!skb->len || skb->len > U16_MAX)
		return -EINVAL;
	/* Every CLDMA packet starts with the 16-byte CCCI header. */
	if (skb->len < sizeof(*ccci_h))
		return -EINVAL;
	ccci_h = (struct ccci_header *)skb->data;

	queue = &cd->txq[qno];

	spin_lock_irqsave(&queue->ring_lock, flags);
	req = queue->tx_xmit;
	if (!req || req->skb) {
		spin_unlock_irqrestore(&queue->ring_lock, flags);
		dev_dbg(cd->dev, "CLDMA txq%u ring full\n", qno);
		return -EBUSY;
	}

	/* Assign the sequence number before the skb is mapped for DMA. */
	seq_assigned = cldma_tx_seq_inc(cd, ccci_h);
	/* Publish the skb payload before the GPD points at it. */
	wmb();
	ret = queue->tr_ring->handle_tx_request(queue, req, skb);
	if (ret) {
		if (seq_assigned)
			cldma_tx_seq_dec(cd, ccci_h);
		spin_unlock_irqrestore(&queue->ring_lock, flags);
		return ret;
	}
	queue->tx_xmit = cldma_ring_step_forward(queue->tr_ring, req);
	spin_unlock_irqrestore(&queue->ring_lock, flags);

	/* Ring the doorbell: the UL resume command starts the queue. */
	spin_lock_irqsave(&cd->timeout_lock, flags);
	if (cd->txq_active & BIT(qno)) {
		if (qno == CLDMA_VT_QUEUE)
			mod_timer(&queue->timeout_timer,
				  jiffies + CLDMA_ACTIVE_T * HZ);
		cldma_write32(cd->ap_pdn_base, CLDMA_AP_UL_RESUME_CMD,
			      BIT(qno));
		cldma_read32(cd->ap_pdn_base, CLDMA_AP_UL_RESUME_CMD);
	} else {
		/*
		 * modem_cldma.c:2930-2937: the modem is not running, the
		 * descriptor will be dropped by the next engine stop. The
		 * packet is still consumed, exactly like downstream.
		 */
		dev_info_ratelimited(cd->dev,
				     "CLDMA stopped, txq%u packet will be dropped\n",
				     qno);
	}
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	return 0;
}

void ccci_cldma_set_rx_cb(struct ccci_cldma *cd,
			  void (*cb)(void *ctx, unsigned int qno,
				     struct sk_buff *skb),
			  void *ctx)
{
	if (!cd || IS_ERR(cd))
		return;

	WRITE_ONCE(cd->rx_ctx, ctx);
	WRITE_ONCE(cd->rx_cb, cb);
}

/*
 * INFRA_AO window used by the RGU CLDMA reset. The core resolves the optional
 * "mediatek,infracfg-ao" phandle into md->infra_ao (ccci_core.c,
 * ccci_md_map_infra_ao()) and the platform probe hands it over; it is the
 * same block downstream maps for "mediatek,INFRACFG_AO"
 * (eccci/mt6735/ccci_platform.c:748-754) and holds INFRA_RST0/INFRA_RST1 at
 * 0x0030/0x0034 (mt6735/cldma_reg.h:18-19).
 */
void ccci_cldma_set_infra_ao(struct ccci_cldma *cd, void __iomem *infra_ao)
{
	if (!cd || IS_ERR(cd))
		return;
	if (!infra_ao) {
		dev_warn(cd->dev, "empty INFRA_AO window, RGU reset disabled\n");
		return;
	}

	cd->infra_ao = infra_ao;
	dev_dbg(cd->dev, "INFRA_AO window %p\n", infra_ao);
}

/*
 * RGU reset of the CLDMA block, verbatim from downstream
 * md_cldma_hw_reset() (eccci/mt6735/cldma_platform.c:55-71): assert
 * CLDMA_AO_RST_MASK | CLDMA_PD_RST_MASK in INFRA_RST0, then write the same
 * masks to INFRA_RST1 to release the reset. Callers are ccci_cldma_start()
 * and ccci_cldma_stop(), which mirror md_cd_start() (modem_cldma.c:2340) and
 * the end of md_cldma_clear() (modem_cldma.c:2468).
 */
int ccci_cldma_hw_reset(struct ccci_cldma *cd)
{
	unsigned long flags;
	u32 reg_value;

	if (!cd || IS_ERR(cd))
		return -EINVAL;
	if (!cd->infra_ao)
		return -ENODEV;

	spin_lock_irqsave(&cd->timeout_lock, flags);
	reg_value = cldma_read32(cd->infra_ao, CLDMA_INFRA_RST0_REG);
	reg_value &= ~CLDMA_RGU_RST_MASK;
	reg_value |= CLDMA_RGU_RST_MASK;
	cldma_write32(cd->infra_ao, CLDMA_INFRA_RST0_REG, reg_value);

	reg_value = cldma_read32(cd->infra_ao, CLDMA_INFRA_RST1_REG);
	reg_value &= ~CLDMA_RGU_RST_MASK;
	reg_value |= CLDMA_RGU_RST_MASK;
	cldma_write32(cd->infra_ao, CLDMA_INFRA_RST1_REG, reg_value);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	dev_dbg(cd->dev, "CLDMA RGU reset via INFRA_RST0/1\n");
	return 0;
}

/*
 * modem_cldma.c:1094-1133. Latch the ring set used by the queues: network
 * queues move from the 1.5K net ring to the 4K normal ring when their bit is
 * in CLDMA_NORMAL_QUEUE_MASK (queue 3 moves, queues 4/5 keep the net ring),
 * the other queues always use the normal ring. Downstream only re-points a
 * queue when the engine is (re)armed, so a running engine keeps its current
 * rings until the next ccci_cldma_start(), which applies the latched map
 * before arming (same point as downstream's HIF_EX_ALLQ_RESET restart).
 */
int ccci_cldma_set_exception_mode(struct ccci_cldma *cd, bool on)
{
	unsigned long flags;

	if (!cd || IS_ERR(cd))
		return -EINVAL;

	spin_lock_irqsave(&cd->timeout_lock, flags);
	if (cd->exception_mode == on) {
		spin_unlock_irqrestore(&cd->timeout_lock, flags);
		return 0;
	}
	cd->exception_mode = on;
	/*
	 * Only latch while the engine runs; ccci_cldma_start() applies the
	 * map before arming. "started" is written under start_lock, so this
	 * READ_ONCE() can only observe a stable value: either the engine is
	 * fully stopped (safe to re-point) or fully published (start owns
	 * the map for this arm).
	 */
	if (!READ_ONCE(cd->started))
		cldma_apply_ring_map(cd);
	spin_unlock_irqrestore(&cd->timeout_lock, flags);

	dev_info(cd->dev, "CLDMA %s ring set\n",
		 on ? "exception" : "normal");
	return 0;
}
