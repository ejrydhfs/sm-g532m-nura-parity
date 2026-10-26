// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI core: modem objects, per-MD state machine, logical port table,
 * Rx dispatch and Tx queueing.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/{ccci_core.c,port_cfg.c}.
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 *
 * State machine: downstream uses MD_STATE {INVALID, GATED, BOOTING, READY,
 * EXCEPTION, RESET, RX_IRQ, TX_IRQ, TX_FULL, BOOT_FAIL}.  The frozen shared
 * header exposes enum ccci_md_state {INVALID, BOOTING, READY, EXCEPTION,
 * STOPPED}, so INVALID/GATED/RESET are collapsed into CCCI_MD_STOPPED and the
 * port-private pseudo events are delivered as CCCI_EV_* below.
 */
#include <linux/ccci.h>
#include <linux/init.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ccci_priv.h"
#include "ccci_rpc.h"

LIST_HEAD(ccci_md_list);
static DEFINE_MUTEX(ccci_md_list_lock);
struct class *ccci_class;

static struct ccci_md *ccci_md_alloc(unsigned int md_id);
static void ccci_md_free(struct ccci_md *md);

/* ------------------------------------------------------------------ */
/* skb helpers                                                         */
/* ------------------------------------------------------------------ */
struct sk_buff *ccci_alloc_skb(unsigned int len, gfp_t gfp)
{
	if (!len)
		return NULL;
	/* the ccci_header is always the first thing the modem sees and the
	 * ports pull it, which leaves CCCI_HEADER_SIZE bytes of headroom
	 */
	return alloc_skb(len, gfp);
}

/* ------------------------------------------------------------------ */
/* sequence numbers, downstream FEATURE_SEQ_CHECK_EN handling           */
/* ------------------------------------------------------------------ */
void ccci_reset_seq(struct ccci_md *md)
{
	unsigned long flags;

	spin_lock_irqsave(&md->lock, flags);
	memset(md->seq_out, 0, sizeof(md->seq_out));
	memset(md->seq_in, 0xFF, sizeof(md->seq_in));	/* -1 per entry */
	spin_unlock_irqrestore(&md->lock, flags);
}

void ccci_inc_tx_seq(struct ccci_md *md, struct ccci_header *ccci_h)
{
	unsigned long flags;

	if (ccci_h->channel >= CCCI_MAX_CH_NUM)
		return;		/* e.g. CCCI_FORCE_ASSERT_CH */

	spin_lock_irqsave(&md->lock, flags);
	ccci_h->seq_num = md->seq_out[ccci_h->channel]++;
	ccci_h->assert_bit = 1;
	/* RPC/FS tasks are preemptible once the modem runs multi-task */
	if ((ccci_h->channel == CCCI_RPC_TX || ccci_h->channel == CCCI_FS_TX) &&
	    md->boot_stage != CCCI_BOOT_STAGE_1)
		ccci_h->assert_bit = 0;
	spin_unlock_irqrestore(&md->lock, flags);
}

void ccci_chk_rx_seq(struct ccci_md *md, struct ccci_header *ccci_h,
		     unsigned int qno)
{
	unsigned long flags;
	u16 channel = ccci_h->channel;
	s16 prev;

	if (channel >= CCCI_MAX_CH_NUM || !ccci_h->assert_bit)
		return;

	spin_lock_irqsave(&md->lock, flags);
	prev = md->seq_in[channel];
	if (prev != -1 && ((ccci_h->seq_num - prev) & 0x7FFF) != 1) {
		spin_unlock_irqrestore(&md->lock, flags);
		ccci_md_err(md, "channel %u seq out-of-order %d->%u (q%u)\n",
			    channel, prev, ccci_h->seq_num, qno);
		ccci_ccif_dump_regs(md, NULL);
		/* downstream force_assert(md, CCIF_INTR_SEQ) */
		if (md->ccif)
			ccci_ccif_send(md, AP_MD_SEQ_ERROR);
		else
			ccci_md_force_assert(md, 0);
		return;
	}
	md->seq_in[channel] = ccci_h->seq_num;
	spin_unlock_irqrestore(&md->lock, flags);
}

/* ------------------------------------------------------------------ */
/* state machine                                                       */
/* ------------------------------------------------------------------ */
int ccci_md_set_state(struct ccci_md *md, enum ccci_md_state state)
{
	unsigned long flags;
	enum ccci_md_state old;

	spin_lock_irqsave(&md->lock, flags);
	old = md->state;
	md->state = state;
	spin_unlock_irqrestore(&md->lock, flags);

	/*
	 * CCCI gap closure (CLDMA leaf): select the CLDMA ring set.
	 *
	 * Downstream decides per queue in cldma_queue_switch_ring(), keyed on
	 * md->ex_stage != EX_INIT_DONE (modem_cldma.c:1102,1116), and only
	 * ever re-points rings when the engine is initialised or re-armed by
	 * cldma_start() (modem_cldma.c:1569-1617); the exception handshake
	 * sets EX_INIT_DONE immediately before that restart at
	 * HIF_EX_ALLQ_RESET (modem_cldma.c:2021-2024).  The frozen API has no
	 * stage-aware setter and md->ex_stage is not updated by Nura's
	 * exception handshake, so the state machine is the trigger here: the
	 * engine is switched to the exception map on entering EXCEPTION and
	 * back to the net map on BOOTING/READY (and STOPPED, which always
	 * precedes a new boot).  ccci_cldma_set_exception_mode() only latches
	 * the map while the engine runs and ccci_cldma_start() applies it
	 * before arming, i.e. at exactly the downstream restart point.
	 * Entering EXCEPTION can therefore only change the rings at the
	 * ALLQ_RESET restart, never on a live ring.
	 */
	if (md->cldma) {
		switch (state) {
		case CCCI_MD_EXCEPTION:
			ccci_cldma_set_exception_mode(md->cldma, true);
			break;
		case CCCI_MD_BOOTING:
		case CCCI_MD_READY:
		case CCCI_MD_STOPPED:
			ccci_cldma_set_exception_mode(md->cldma, false);
			break;
		default:
			break;
		}
	}

	if (old == state)
		return 0;

	ccci_md_info(md, "state %d -> %d\n", old, state);
	ccci_md_notify_ports(md, state);

	/*
	 * First entry into CCCI_MD_EXCEPTION (MD_EX, MD_EX_REC_OK, MD_EX_PASS,
	 * the CCIF exception handshake).  Downstream reaches ccci_aed() from
	 * the exception timers, and ccci_ee_info_dump() (port_kernel.c:
	 * 3079-3088) picks EX_PKT before the boot handshake and the EX_MEM
	 * shared-memory window afterwards.  Messages that arrive while the
	 * modem is already in CCCI_MD_EXCEPTION do not change the state; they
	 * are dumped by ccci_rx_exception_packet().
	 */
	if (state == CCCI_MD_EXCEPTION) {
		ccci_exception_trigger(md,
				       md->ex_stage == CCCI_EX_DHL_DL_RDY ?
				       CCCI_EX_DUMP_MD_EX_REC_OK :
				       CCCI_EX_DUMP_MD_EX,
				       ccci_exception_dump_flags(md));
	}
	return 0;
}

void ccci_md_notify_ports(struct ccci_md *md, int event)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->ops && port->ops->md_state_notice)
			port->ops->md_state_notice(port, event);
	}
}

/* ------------------------------------------------------------------ */
/* port lookups and dispatch                                           */
/* ------------------------------------------------------------------ */
struct ccci_md *ccci_md_get(unsigned int md_id)
{
	struct ccci_md *md, *found = NULL;

	mutex_lock(&ccci_md_list_lock);
	list_for_each_entry(md, &ccci_md_list, list) {
		if (md->id == md_id) {
			found = md;
			break;
		}
	}
	mutex_unlock(&ccci_md_list_lock);
	return found;
}

struct ccci_port *ccci_md_get_port_by_channel(struct ccci_md *md, unsigned int ch)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->tx_ch == ch || port->rx_ch == ch)
			return port;
	}
	return NULL;
}

struct ccci_port *ccci_md_get_port_by_minor(struct ccci_md *md, unsigned int minor)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->minor == minor && port->cdev_added)
			return port;
	}
	return NULL;
}

struct ccci_port *ccci_get_port_for_node(dev_t devt)
{
	struct ccci_md *md;

	mutex_lock(&ccci_md_list_lock);
	list_for_each_entry(md, &ccci_md_list, list) {
		if (md->major == MAJOR(devt)) {
			struct ccci_port *port = ccci_md_get_port_by_minor(md,
							MINOR(devt));

			mutex_unlock(&ccci_md_list_lock);
			return port;
		}
	}
	mutex_unlock(&ccci_md_list_lock);
	return NULL;
}

int ccci_port_recv_skb(struct ccci_port *port, struct sk_buff *skb)
{
	unsigned long flags;

	spin_lock_irqsave(&port->rx_lock, flags);
	if (port->rx_len >= port->rx_len_th) {
		port->rx_full = true;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		if (port->flags & PORT_F_ALLOW_DROP) {
			port->rx_drops++;
			dev_kfree_skb_any(skb);
			return 0;
		}
		return -CCCI_ERR_PORT_RX_FULL;
	}
	port->rx_full = false;
	port->rx_len++;
	__skb_queue_tail(&port->rx_q, skb);
	spin_unlock_irqrestore(&port->rx_lock, flags);

	port->rx_pkts++;
	wake_up_interruptible(&port->rx_wq);
	if (port->ops->md_state_notice)
		port->ops->md_state_notice(port, CCCI_EV_RX_IRQ);

	return 0;
}

/*
 * ccci_rx_exception_packet - record/dump exception control messages
 *
 * MD_EX_REC_OK carries the EX_LOG_T record; downstream validates it and
 * copies sizeof(EX_LOG_T) bytes out of the packet (port_kernel.c:551-582).
 * The other exception messages are dumped as they arrive so that a repeat or
 * a message that does not change md->state still produces a snapshot
 * (downstream re-arms the exception timers on each of them).
 */
static void ccci_rx_exception_packet(struct ccci_md *md, struct sk_buff *skb)
{
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
	unsigned int flags;
	bool in_exception;

	if (ccci_h->channel != CCCI_CONTROL_RX)
		return;

	in_exception = READ_ONCE(md->state) == CCCI_MD_EXCEPTION;
	flags = ccci_exception_dump_flags(md);

	switch (ccci_h->data[1]) {
	case MD_EX_REC_OK:
		if (ccci_h->reserved != MD_EX_REC_OK_CHK_ID)
			return;
		if (skb->len < CCCI_HEADER_SIZE + sizeof(struct ccci_ex_log)) {
			/* downstream rejects the short record as invalid */
			ccci_md_err(md,
				    "invalid MD_EX_REC_OK: len %u, expected %zu\n",
				    skb->len, CCCI_HEADER_SIZE +
				    sizeof(struct ccci_ex_log));
			return;
		}
		ccci_exception_capture_packet(md, skb->data,
					      CCCI_HEADER_SIZE +
					      sizeof(struct ccci_ex_log));
		if (in_exception)
			ccci_exception_trigger(md, CCCI_EX_DUMP_MD_EX_REC_OK,
					       flags);
		break;
	case MD_EX:
		if (!in_exception || ccci_h->reserved != MD_EX_CHK_ID)
			return;
		ccci_exception_trigger(md, CCCI_EX_DUMP_MD_EX, flags);
		break;
	case MD_EX_PASS:
	case MD_EX_RESUME:
		if (!in_exception)
			return;
		ccci_exception_trigger(md, CCCI_EX_DUMP_MD_EX, flags);
		break;
	default:
		break;
	}
}

/*
 * ccci_rx_dispatch_qno - hand a received packet from hardware queue @qno to
 * its port
 *
 * Return 0 when the skb was consumed (or dropped), -CCCI_ERR_PORT_RX_FULL
 * when the caller must retry with the same skb.
 */
int ccci_rx_dispatch_qno(struct ccci_md *md, struct sk_buff *skb,
			 unsigned int qno)
{
	struct ccci_header *ccci_h;
	struct ccci_port *port;
	enum ccci_md_state state;
	int ret = -CCCI_ERR_DROP_PACKET;

	if (!skb || skb->len < CCCI_HEADER_SIZE) {
		dev_kfree_skb_any(skb);
		return -CCCI_ERR_INVALID_PARAM;
	}

	ccci_h = (struct ccci_header *)skb->data;
	if (ccci_h->channel >= CCCI_MAX_CH_NUM) {
		ccci_md_err(md, "drop packet on invalid channel %u\n",
			    ccci_h->channel);
		dev_kfree_skb_any(skb);
		return -CCCI_ERR_CHANNEL_NUM_MIS_MATCH;
	}

	state = md->state;
	if ((state == CCCI_MD_STOPPED || state == CCCI_MD_INVALID) &&
	    ccci_h->channel != CCCI_MONITOR_CH) {
		dev_kfree_skb_any(skb);
		return -CCCI_ERR_HIF_NOT_POWER_ON;
	}

	ccci_rx_exception_packet(md, skb);

	ccci_chk_rx_seq(md, ccci_h, qno);
	ccci_md_dbg(md, "rx ch%u len%u\n", ccci_h->channel, skb->len);

	list_for_each_entry(port, &md->rx_ch_ports[ccci_h->channel], rx_node) {
		if (port->rx_ch != ccci_h->channel)
			continue;
		/*
		 * Kernel RPC demux: downstream port_char_req_match()
		 * (port_char.c:1295-1306) hands an RPC packet to the char
		 * "ccci_rpc" port only when port_kernel_req_match() returns 0,
		 * which happens for the LGE userspace op ids alone
		 * (port_kernel.c:2166-2197).  CONFIG_MTK_TC1_FEATURE is not
		 * set in this ROM configuration, so every op belongs to the
		 * kernel "ccci_rpc_k" port.  Without this skip the char port,
		 * which is earlier in the table, would swallow the request.
		 */
		if (ccci_h->channel == CCCI_RPC_RX &&
		    port->ops != &ccci_kernel_port_ops)
			continue;
		if (!port->ops->recv_skb)
			break;
		ret = port->ops->recv_skb(port, skb);
		/* the port consumed the skb on 0, kept it on -RX_FULL */
		if (ret == 0 || ret == -CCCI_ERR_PORT_RX_FULL)
			return ret;
		break;
	}

	dev_kfree_skb_any(skb);
	return -CCCI_ERR_DROP_PACKET;
}

int ccci_rx_dispatch(struct ccci_md *md, struct sk_buff *skb)
{
	return ccci_rx_dispatch_qno(md, skb, 0);
}

void ccci_port_ask_more(struct ccci_port *port)
{
	struct ccci_md *md = port->md;

	port->rx_full = false;
	/* CCIF ring transport only pushes packets on a doorbell or after the
	 * port drains; wake the queue worker so it re-scans the ring.
	 */
	if (md->ring_mode && md->ccif) {
		unsigned int qno = ccci_port_rxq(port);

		if (qno < CCCI_MAX_QUEUES)
			ccci_ccif_ring_rx_poll(md, qno, 0);
	}
	if (!skb_queue_empty(&md->rx_backlog))
		schedule_work(&md->rx_backlog_work);
}

/*
 * ccci_md_rx_backlog_work - retry packets whose port was full
 *
 * The CLDMA engine hands us skbs one by one; a busy character port must not
 * make us drop modem traffic, so pending packets wait here until the port
 * drains.
 */
static void ccci_md_rx_backlog_work(struct work_struct *work)
{
	struct ccci_md *md = container_of(work, struct ccci_md, rx_backlog_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&md->rx_backlog))) {
		unsigned int retry = 0;

		while (ccci_rx_dispatch(md, skb) == -CCCI_ERR_PORT_RX_FULL) {
			/* the port drains in process context; give it time and
			 * eventually drop instead of stalling the modem Rx path
			 */
			if (++retry > 20) {
				ccci_md_err(md, "port still full, drop rx packet\n");
				dev_kfree_skb_any(skb);
				break;
			}
			msleep(5);
		}
	}
}

/* CLDMA rx callback: called with a packet owned by us */
void ccci_md_rx_cb(void *ctx, unsigned int qno, struct sk_buff *skb)
{
	struct ccci_md *md = ctx;

	if (!md || !skb)
		return;

	if (ccci_rx_dispatch_qno(md, skb, qno) == -CCCI_ERR_PORT_RX_FULL) {
		if (skb_queue_len(&md->rx_backlog) > 256) {
			ccci_md_err(md, "rx backlog overflow on q%u\n", qno);
			dev_kfree_skb_any(skb);
			return;
		}
		skb_queue_tail(&md->rx_backlog, skb);
		schedule_work(&md->rx_backlog_work);
	}
}

/* ------------------------------------------------------------------ */
/* Tx                                                                  */
/* ------------------------------------------------------------------ */
int ccci_md_write_room(struct ccci_md *md, unsigned int qno)
{
	if (qno >= CCCI_MAX_QUEUES)
		return -CCCI_ERR_INVALID_QUEUE_INDEX;

	if (md->cldma)
		return 1;	/* the CLDMA engine reports -EBUSY on send */

	if (md->ring_mode && md->ccif)
		return ccci_ccif_ring_write_room(md, qno);

	return -CCCI_ERR_INVALID_QUEUE_INDEX;
}

/**
 * ccci_md_send_skb - send one CCCI packet to the modem
 *
 * On success the skb is consumed here.  On failure the caller still owns the
 * skb and must free it (documented in the leaf report).
 */
int ccci_md_send_skb(struct ccci_md *md, unsigned int qno, struct sk_buff *skb)
{
	struct ccci_header *ccci_h;
	int ret;

	if (!skb)
		return -CCCI_ERR_INVALID_PARAM;
	if (skb->len < CCCI_HEADER_SIZE)
		return -CCCI_ERR_INVALID_PARAM;
	if (qno >= CCCI_MAX_QUEUES)
		return -CCCI_ERR_INVALID_QUEUE_INDEX;

	ccci_h = (struct ccci_header *)skb->data;

	/* room is checked before the sequence number is applied, so a busy
	 * queue does not leave a hole in the per-channel sequence
	 */
	if (ccci_md_write_room(md, qno) <= 0)
		return -EBUSY;

	if (md->cldma) {
		/* the CLDMA engine owns the per-channel sequence counter, as
		 * in downstream modem_cldma.c:2862.  RPC/FS must only assert
		 * while the modem is in its single-task phase (ccci_core.h:902)
		 */
		if (ccci_h->channel == CCCI_RPC_TX ||
		    ccci_h->channel == CCCI_FS_TX)
			ccci_h->assert_bit =
				md->boot_stage == CCCI_BOOT_STAGE_1;
		ret = ccci_cldma_send(md->cldma, qno, skb);
		return ret;
	}

	if (md->ring_mode && md->ccif) {
		/* ring send applies the sequence number under its tx lock */
		ret = ccci_ccif_ring_send(md, qno, skb);
		return ret;
	}

	return -CCCI_ERR_INVALID_QUEUE_INDEX;
}

int ccci_send_msg_to_md(struct ccci_md *md, unsigned int ch, u32 msg, u32 resv,
			bool blocking)
{
	struct ccci_port *port;
	struct ccci_header *ccci_h;
	struct sk_buff *skb;
	unsigned int qno;
	gfp_t gfp;
	int ret;

	if (!md)
		return -CCCI_ERR_INVALID_PARAM;

	if (md->state != CCCI_MD_BOOTING && md->state != CCCI_MD_READY &&
	    md->state != CCCI_MD_EXCEPTION)
		return -CCCI_ERR_MD_NOT_READY;
	if (ch == CCCI_SYSTEM_TX && md->state != CCCI_MD_READY)
		return -CCCI_ERR_MD_NOT_READY;

	port = ccci_md_get_port_by_channel(md, ch);
	if (!port)
		return -CCCI_ERR_CHANNEL_NUM_MIS_MATCH;

	qno = ccci_port_txq(port);
	if (qno >= CCCI_MAX_QUEUES)
		return -CCCI_ERR_INVALID_QUEUE_INDEX;

	if (blocking) {
		unsigned int retry = 0;

		/* ID_PAUSE_LTE is called by DVFS with interrupts disabled, so
		 * this wait must not sleep (downstream spins with udelay(5))
		 */
		while (ccci_md_write_room(md, qno) <= 0 && retry++ < 200)
			udelay(5);
	}

	/* DVFS may call this with interrupts disabled */
	gfp = (in_interrupt() || irqs_disabled()) ? GFP_ATOMIC : GFP_KERNEL;
	skb = ccci_alloc_skb(CCCI_HEADER_SIZE, gfp);
	if (!skb)
		return -CCCI_ERR_ALLOCATE_MEMORY_FAIL;

	ccci_h = (struct ccci_header *)skb_put(skb, CCCI_HEADER_SIZE);
	memset(ccci_h, 0, CCCI_HEADER_SIZE);
	ccci_h->data[0] = CCCI_MAGIC_NUM;
	ccci_h->data[1] = msg;
	ccci_h->channel = ch;
	ccci_h->reserved = resv;

	port->tx_pkts++;
	ret = ccci_md_send_skb(md, qno, skb);
	if (ret) {
		if (ret == -EBUSY)
			port->tx_busy_cnt++;
		dev_kfree_skb_any(skb);
	}
	return ret;
}

int ccci_send_virtual_md_msg(struct ccci_md *md, unsigned int ch, u32 msg,
			     u32 resv)
{
	struct ccci_header *ccci_h;
	struct sk_buff *skb;
	int ret, retry = 0;

	if (ch != CCCI_MONITOR_CH)
		return -CCCI_ERR_CHANNEL_NUM_MIS_MATCH;
	if (in_interrupt())
		return -CCCI_ERR_INVALID_PARAM;

	skb = ccci_alloc_skb(CCCI_HEADER_SIZE, GFP_KERNEL);
	if (!skb)
		return -CCCI_ERR_ALLOCATE_MEMORY_FAIL;

	ccci_h = (struct ccci_header *)skb_put(skb, CCCI_HEADER_SIZE);
	memset(ccci_h, 0, CCCI_HEADER_SIZE);
	ccci_h->data[0] = CCCI_MAGIC_NUM;
	ccci_h->data[1] = msg;
	ccci_h->channel = ch;
	ccci_h->reserved = resv;

	for (;;) {
		ret = ccci_rx_dispatch(md, skb);
		if (ret != -CCCI_ERR_PORT_RX_FULL)
			return ret;
		if (++retry > 20) {
			ccci_md_err(md, "port full, drop virtual msg 0x%x\n",
				    msg);
			return ret;
		}
		msleep(100);
	}
}

/* ------------------------------------------------------------------ */
/* force assert                                                        */
/* ------------------------------------------------------------------ */
int ccci_md_force_assert(struct ccci_md *md, unsigned int ccif_only)
{
	struct ccci_header *ccci_h;
	struct sk_buff *skb;
	int ret;

	if (ccif_only && md->ccif)
		return ccci_ccif_send(md, H2D_FORCE_MD_ASSERT);

	skb = ccci_alloc_skb(CCCI_HEADER_SIZE, GFP_ATOMIC);
	if (!skb)
		return -CCCI_ERR_ALLOCATE_MEMORY_FAIL;

	ccci_h = (struct ccci_header *)skb_put(skb, CCCI_HEADER_SIZE);
	memset(ccci_h, 0, CCCI_HEADER_SIZE);
	ccci_h->data[0] = 0xFFFFFFFF;
	ccci_h->data[1] = 0x5A5A5A5A;
	/* the header channel field is 16 bit wide, the ROM relies on the
	 * truncation of CCCI_FORCE_ASSERT_CH
	 */
	ccci_h->channel = (u16)CCCI_FORCE_ASSERT_CH;
	ccci_h->reserved = 0xA5A5A5A5;

	/* downstream hardcodes queue 0 */
	ret = ccci_md_send_skb(md, 0, skb);
	if (ret)
		dev_kfree_skb_any(skb);
	if (ret && md->ccif)
		ret = ccci_ccif_send(md, H2D_FORCE_MD_ASSERT);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Modem life cycle                                                    */
/* ------------------------------------------------------------------ */
int ccci_md_start(struct ccci_md *md)
{
	unsigned int ret = 0;
	int err;

	mutex_lock(&md->op_lock);
	if (md->state != CCCI_MD_STOPPED && md->state != CCCI_MD_INVALID) {
		mutex_unlock(&md->op_lock);
		return -EBUSY;
	}

	ccci_md_info(md, "start (fw=%s)\n", md->fw_name);

	/* downstr+ md_cd_start(): security state first, then drop the MD
	 * region protection so the AP can write the image
	 */
	ccci_init_security(md);
	ccci_mpu_clear_md_protection(md);

	if (!md->boot.fw_valid) {
		err = ccci_boot_load_firmware(md);
		if (err) {
			ccci_md_err(md, "firmware load failed: %s\n",
				    md->boot.err_str[0] ? md->boot.err_str :
				    "unknown");
			ret = err;
			goto fail;
		}
	}

	/* downstr+ md_cd_start() step 3: program the RMPU windows and enable
	 * the EMI MPU before the modem is powered and released
	 */
	ccci_mpu_set_mem_remap(md);
	ccci_mpu_set_md_protection(md);
	/* downstream md_cd_start() also protects the DSP sub-image when the
	 * loaded image carries one (modem_cldma.c:2320-2321); no-op otherwise
	 */
	ccci_mpu_dsp_protection(md, false);

	if (md->hw_ops && md->hw_ops->power_on)
		err = md->hw_ops->power_on(md->id);
	else
		err = ccci_boot_power_on(md);
	if (err) {
		ccci_md_err(md, "power on failed: %d\n", err);
		ret = err;
		goto fail;
	}
	md->powered = true;

	ccci_reset_seq(md);
	if (md->ccif) {
		err = ccci_ccif_start(md->ccif);
		if (err) {
			ccci_md_err(md, "ccif start failed: %d\n", err);
			ret = err;
			goto fail;
		}
		ccci_ccif_reset_rings(md);
	}
	if (md->cldma) {
		/*
		 * CCCI gap closure (CLDMA leaf): a cold boot always arms the
		 * normal ring set. Downstream's cldma_queue_switch_ring()
		 * picks the net rings whenever ex_stage != EX_INIT_DONE
		 * (modem_cldma.c:1102,1116) and a boot never has EX_INIT_DONE,
		 * so clear a stale latch left by an exception that has not
		 * gone through ccci_md_stop()/ccci_md_reset().
		 */
		ccci_cldma_set_exception_mode(md->cldma, false);
		err = ccci_cldma_start(md->cldma);
		if (err) {
			ccci_md_err(md, "cldma start failed: %d\n", err);
			ret = err;
			goto fail;
		}
	}

	md->boot_stage = CCCI_BOOT_STAGE_0;
	ccci_md_set_state(md, CCCI_MD_BOOTING);
	ccci_boot_arm_timer(md);

	if (md->hw_ops && md->hw_ops->let_md_go)
		err = md->hw_ops->let_md_go(md->id);
	else
		err = ccci_boot_let_md_go(md);
	if (err) {
		ccci_md_err(md, "let md go failed: %d\n", err);
		ret = err;
		goto fail;
	}

	if (md->ccif)
		ccif_enable_irq(md);

	md->boot_count++;
	mutex_unlock(&md->op_lock);
	return 0;

fail:
	ccci_boot_disarm_timer(md);
	if (md->powered) {
		if (md->hw_ops && md->hw_ops->power_off)
			md->hw_ops->power_off(md->id);
		else
			ccci_boot_power_off(md);
		md->powered = false;
	}
	ccci_md_set_state(md, CCCI_MD_STOPPED);
	ccci_md_notify_ports(md, CCCI_EV_BOOT_FAIL);
	mutex_unlock(&md->op_lock);
	return ret;
}

int ccci_md_stop(struct ccci_md *md, unsigned int timeout)
{
	mutex_lock(&md->op_lock);
	ccci_md_info(md, "stop (timeout=%u)\n", timeout);

	ccci_boot_disarm_timer(md);
	if (md->ccif) {
		ccif_disable_irq(md);
		ccci_ccif_stop(md->ccif);
	}
	if (md->cldma)
		ccci_cldma_stop(md->cldma);

	ccci_mpu_clear_md_protection(md);

	if (md->powered) {
		if (md->hw_ops && md->hw_ops->power_off)
			md->hw_ops->power_off(md->id);
		else
			ccci_boot_power_off(md);
		md->powered = false;
	}

	ccci_md_set_state(md, CCCI_MD_STOPPED);
	mutex_unlock(&md->op_lock);
	return 0;
}

int ccci_md_reset(struct ccci_md *md)
{
	mutex_lock(&md->op_lock);
	ccci_md_info(md, "reset\n");

	/*
	 * No exception dump here: this is also the normal userspace reset path
	 * (CCCI_IOC_MD_RESET, flight mode, stop request, port_kernel.c:130).
	 * Downstream only dumps when the MD watchdog fires while the mdlogger
	 * is attached; that hook is in ccci_ccif_wdt_work().
	 */
	ccci_boot_disarm_timer(md);
	if (md->ccif)
		ccif_disable_irq(md);
	if (md->hw_ops && md->hw_ops->reset)
		md->hw_ops->reset(md->id);
	ccci_mpu_clear_md_protection(md);
	md->boot_stage = CCCI_BOOT_STAGE_0;
	ccci_md_set_state(md, CCCI_MD_STOPPED);
	mutex_unlock(&md->op_lock);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration API (frozen shared header)                             */
/* ------------------------------------------------------------------ */
static void ccci_md_class_dev_release(struct device *dev)
{
	kfree(dev);
}

static int ccci_md_class_dev_register(struct ccci_md *md, struct device *parent)
{
	struct device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	device_initialize(dev);
	dev->class = ccci_class;
	dev->parent = parent;
	dev->release = ccci_md_class_dev_release;
	dev_set_name(dev, "md%u", md->id + 1);
	if (device_add(dev)) {
		put_device(dev);
		return -ENODEV;
	}
	dev_set_drvdata(dev, md);
	md->dev = dev;
	return 0;
}

static void ccci_md_class_dev_unregister(struct ccci_md *md)
{
	if (!md->dev)
		return;
	device_unregister(md->dev);
	md->dev = NULL;
}

/*
 * Map the INFRA_AO register window referenced by the modem node's
 * "mediatek,infracfg-ao" phandle. Both the MPU/RMPU remap programming and the
 * CLDMA hardware reset need it; it is optional because the CCIF-only
 * transports never touch those registers.
 */
static void ccci_md_map_infra_ao(struct ccci_md *md, struct device *parent)
{
	struct device_node *np, *ao;
	struct resource res;

	if (!parent || !parent->of_node)
		return;

	np = parent->of_node;
	ao = of_parse_phandle(np, "mediatek,infracfg-ao", 0);
	if (!ao) {
		ccci_md_dbg(md, "no mediatek,infracfg-ao phandle\n");
		return;
	}
	if (of_address_to_resource(ao, 0, &res)) {
		ccci_md_warn(md, "infracfg-ao node has no reg\n");
		of_node_put(ao);
		return;
	}
	of_node_put(ao);

	md->infra_ao = devm_ioremap(parent, res.start, resource_size(&res));
	if (!md->infra_ao) {
		ccci_md_warn(md, "cannot map INFRA_AO %pa+0x%llx\n", &res.start,
			     (unsigned long long)resource_size(&res));
		return;
	}
	md->infra_ao_phys = res.start;
	md->infra_ao_size = resource_size(&res);
	ccci_md_dbg(md, "INFRA_AO %pa size 0x%zx\n", &md->infra_ao_phys,
		    md->infra_ao_size);
}

int ccci_md_register(const struct ccci_md_cfg *cfg, struct device *parent)
{
	struct ccci_md *md;
	int ret;

	if (!cfg || cfg->md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	if (ccci_md_get(cfg->md_id))
		return -EEXIST;

	md = ccci_md_alloc(cfg->md_id);
	if (!md)
		return -ENOMEM;

	/* Per-modem device: also the parent of every port char device and the
	 * home of the per-modem sysfs attributes.
	 */
	ret = ccci_md_class_dev_register(md, parent);
	if (ret)
		goto err_free;

	ccci_md_map_infra_ao(md, parent);
	ccci_mpu_init(md);

	strscpy(md->name, cfg->name ? cfg->name : "md", sizeof(md->name));
	strscpy(md->fw_name, cfg->firmware_name ? cfg->firmware_name :
		(cfg->md_id == CCCI_MD_SYS3 ? "md3img" : "md1img"),
		sizeof(md->fw_name));
	md->capability = cfg->capability;
	md->irat_md_id = cfg->irat_md_id;
	md->ring_mode = true;

	ret = ccci_boot_init(md);
	if (ret)
		goto err_device;

	/* shared memory: image region + smem (DT reserved-memory, or a
	 * coherent fallback sized for the CCIF rings)
	 */
	ret = ccci_boot_smem_setup(md, parent);
	if (ret)
		goto err_boot;

	/* character device range for this modem */
	ret = alloc_chrdev_region(&md->devt, 0, CCCI_MAX_PORT_MINOR, md->name);
	if (ret) {
		ccci_md_err(md, "alloc_chrdev_region failed %d\n", ret);
		goto err_boot;
	}
	md->major = MAJOR(md->devt);

	ret = ccci_md_port_cfg(md);
	if (ret)
		goto err_chrdev;

	mutex_lock(&ccci_md_list_lock);
	list_add_tail(&md->list, &ccci_md_list);
	mutex_unlock(&ccci_md_list_lock);
	md->registered = true;

	ret = ccci_port_char_init(md);
	if (ret)
		goto err_registered;

	ret = ccci_sysfs_md_init(md);
	if (ret)
		goto err_sysfs;

	/* exception dump engine (kernel log, ccci_dump ring, debugfs) */
	ret = ccci_exception_init(md);
	if (ret)
		goto err_exception;

	if (ccci_util_log_init(md->id, md->dev))
		ccci_md_warn(md, "util log init failed\n");

	ccci_md_info(md, "registered, major %u, %u ports, %s transport\n",
		     md->major, md->port_count,
		     md->ring_mode ? "CCIF-ring" : "CLDMA");
	return 0;

	/* each label unwinds exactly the step taken by the goto above it */
err_exception:
	ccci_exception_exit(md);
err_sysfs:
	ccci_sysfs_md_exit(md);
	ccci_port_kernel_exit(md);
	ccci_port_net_exit(md);
	ccci_port_char_exit(md);
err_registered:
	mutex_lock(&ccci_md_list_lock);
	list_del_init(&md->list);
	mutex_unlock(&ccci_md_list_lock);
	md->registered = false;
err_chrdev:
	unregister_chrdev_region(md->devt, CCCI_MAX_PORT_MINOR);
err_boot:
	ccci_boot_exit(md);
err_device:
	ccci_md_class_dev_unregister(md);
	md->dev = NULL;
err_free:
	ccci_md_free(md);
	return ret;
}

int ccci_register_modem(const struct ccci_md_cfg *cfg)
{
	/*
	 * External modem drivers (e.g. the C2K/MD3 platform driver) are built as
	 * separate initcalls.  If such a driver probes before the core finished
	 * initialising, ask the driver core to retry instead of creating a modem
	 * device without the CCCI class.
	 */
	if (!ccci_class)
		return -EPROBE_DEFER;

	return ccci_md_register(cfg, NULL);
}
EXPORT_SYMBOL_GPL(ccci_register_modem);

void ccci_unregister_modem(unsigned int md_id)
{
	struct ccci_md *md = ccci_md_get(md_id);

	if (!md)
		return;

	if (md->state == CCCI_MD_BOOTING || md->state == CCCI_MD_READY ||
	    md->state == CCCI_MD_EXCEPTION)
		ccci_md_stop(md, 0);

	mutex_lock(&ccci_md_list_lock);
	list_del_init(&md->list);
	mutex_unlock(&ccci_md_list_lock);
	md->registered = false;

	ccci_util_log_exit(md->id);
	ccci_exception_exit(md);
	ccci_sysfs_md_exit(md);
	ccci_port_kernel_exit(md);
	ccci_port_net_exit(md);
	ccci_port_char_exit(md);
	unregister_chrdev_region(md->devt, CCCI_MAX_PORT_MINOR);
	ccci_boot_exit(md);
	ccci_md_class_dev_unregister(md);
	md->dev = NULL;
	ccci_md_free(md);
}
EXPORT_SYMBOL_GPL(ccci_unregister_modem);

int ccci_md_attach_ccif(struct ccci_md *md, struct ccci_ccif *ccif)
{
	if (!md || !ccif)
		return -EINVAL;
	md->ccif = ccif;
	return 0;
}

int ccci_md_attach_cldma(struct ccci_md *md, struct ccci_cldma *cldma)
{
	if (!md || !cldma)
		return -EINVAL;
	md->cldma = cldma;
	md->ring_mode = false;
	return 0;
}

int ccci_md_get_state(unsigned int md_id)
{
	struct ccci_md *md = ccci_md_get(md_id);
	int state;

	if (!md)
		return -ENODEV;
	state = (int)READ_ONCE(md->state);
	return state;
}
EXPORT_SYMBOL_GPL(ccci_md_get_state);

int ccci_md_set_irat(unsigned int md_id, unsigned int irat_md_id)
{
	struct ccci_md *md = ccci_md_get(md_id);

	if (!md)
		return -ENODEV;
	if (irat_md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	md->irat_md_id = irat_md_id;
	return 0;
}
EXPORT_SYMBOL_GPL(ccci_md_set_irat);

/*
 * aee_dump_ccci_debug_info - ROM ABI for an AEE exception consumer
 *
 * Downstream eccci/ccci_core.c:580-599.  AEE strings use 1-based modem ids.
 * Returns the CCCI debug record at the start of the modem shared memory
 * (CCCI_SMEM_OFFSET_CCCI_DEBUG, CCCI_SMEM_CCCI_DEBUG_SIZE bytes).
 *
 * The AEE client itself (aed_md_exception_api, ROM 0xc0628298) does not
 * exist in this kernel tree; this export is the ABI such a client would use.
 */
int aee_dump_ccci_debug_info(int md_id, void **addr, int *size)
{
	struct ccci_md *md;
	void __iomem *vir;
	size_t len;
	int ret;

	if (!addr || !size)
		return -CCCI_ERR_INVALID_PARAM;

	md_id--;	/* EE strings use 1 and 2, not 0 and 1 */

	md = ccci_md_get(md_id);
	if (!md)
		return -CCCI_ERR_MD_INDEX_NOT_FOUND;

	/*
	 * The region is ccci_exp_smem_base_vir + CCCI_SMEM_OFFSET_CCCI_DEBUG,
	 * CCCI_SMEM_CCCI_DEBUG_SIZE bytes (downstream ccci_core.c:271-273,
	 * returned at ccci_core.c:596-597).
	 */
	ret = ccci_exception_ccci_debug_window(md, &vir, &len);
	if (ret)
		return ret;

	*addr = (void __force *)vir;
	*size = (int)len;
	return 0;
}
EXPORT_SYMBOL_GPL(aee_dump_ccci_debug_info);

int ccci_register_md_hw_ops(unsigned int md_id, const struct ccci_md_hw_ops *ops)
{
	struct ccci_md *md = ccci_md_get(md_id);

	if (!md)
		return -ENODEV;
	md->hw_ops = ops;
	return 0;
}
EXPORT_SYMBOL_GPL(ccci_register_md_hw_ops);

void ccci_unregister_md_hw_ops(unsigned int md_id)
{
	struct ccci_md *md = ccci_md_get(md_id);

	if (!md)
		return;
	md->hw_ops = NULL;
}
EXPORT_SYMBOL_GPL(ccci_unregister_md_hw_ops);

/* ------------------------------------------------------------------ */
/* modem object allocation                                             */
/* ------------------------------------------------------------------ */
/*
 * Default SBP (Single Binary Platform) bind value, downstream
 * ccci_util/Kconfig:99-113 and eccci/port_char.c:914-947.  Downstream parses
 * the Kconfig string lazily from CCCI_IOC_GET_MD_SBP_CFG; here it is resolved
 * once when the modem object is created.  An empty string is the ROM default
 * and disables SBP (value 0).
 */
static u32 ccci_md_sbp_default(unsigned int md_id)
{
	const char *cfg = NULL;
	u32 val = 0;

#if defined(CONFIG_MTK_MD_SBP_CUSTOM_VALUE)
	if (md_id == CCCI_MD_SYS1)
		cfg = CONFIG_MTK_MD_SBP_CUSTOM_VALUE;
#endif
#if defined(CONFIG_MTK_MD2_SBP_CUSTOM_VALUE)
	if (md_id == CCCI_MD_SYS2)
		cfg = CONFIG_MTK_MD2_SBP_CUSTOM_VALUE;
#endif
	if (!cfg || !cfg[0])
		return 0;

	if (kstrtouint(cfg, 0, &val)) {
		pr_warn("ccci: md%u invalid SBP config \"%s\", using 0\n",
			md_id + 1, cfg);
		return 0;
	}

	pr_info("ccci: md%u default SBP code 0x%x from Kconfig\n", md_id + 1,
		val);
	return val;
}

static struct ccci_md *ccci_md_alloc(unsigned int md_id)
{
	struct ccci_md *md;
	int i;

	md = kzalloc(sizeof(*md), GFP_KERNEL);
	if (!md)
		return NULL;

	md->id = md_id;
	md->dev = NULL;
	md->state = CCCI_MD_INVALID;
	md->ex_type = 0;
	md->irat_md_id = CCCI_MAX_MD_NUM;
	md->runtime_version = AP_MD_HS_V2;
	md->md_boot_mode = 0;	/* MD_BOOT_MODE_INVALID */
	md->sim_type = 0xEEEEEEEE;
	md->sbp_code = 0;
	md->sbp_code_default = ccci_md_sbp_default(md_id);
	spin_lock_init(&md->lock);
	mutex_init(&md->op_lock);
	INIT_LIST_HEAD(&md->list);
	INIT_LIST_HEAD(&md->ports_list);
	skb_queue_head_init(&md->rx_backlog);
	INIT_WORK(&md->rx_backlog_work, ccci_md_rx_backlog_work);
	for (i = 0; i < CCCI_MAX_CH_NUM; i++)
		INIT_LIST_HEAD(&md->rx_ch_ports[i]);
	ccci_reset_seq(md);
	snprintf(md->version, sizeof(md->version), "%u", CCCI_DRIVER_VER);
	return md;
}

static void ccci_md_free(struct ccci_md *md)
{
	cancel_work_sync(&md->rx_backlog_work);
	skb_queue_purge(&md->rx_backlog);
	mutex_destroy(&md->op_lock);
	kfree(md->ports);
	kfree(md);
}

/* ------------------------------------------------------------------ */
/* Port table (downstream port_cfg.c)                                  */
/* ------------------------------------------------------------------ */
static const struct ccci_port ccci_md1_ports[] = {
	/* network ports first, downstream port_cfg.c */
	{ .tx_ch = CCCI_CCMNI1_TX, .rx_ch = CCCI_CCMNI1_RX, .txq = 3, .rxq = 3,
	  .txq_exp = 0xF4, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF1, .name = "ccmni0" },
	{ .tx_ch = CCCI_CCMNI2_TX, .rx_ch = CCCI_CCMNI2_RX, .txq = 3, .rxq = 4,
	  .txq_exp = 0xF4, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF2, .name = "ccmni1" },
	{ .tx_ch = CCCI_CCMNI3_TX, .rx_ch = CCCI_CCMNI3_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF3, .name = "ccmni2" },
	{ .tx_ch = CCCI_CCMNI4_TX, .rx_ch = CCCI_CCMNI4_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF4, .name = "ccmni3" },
	{ .tx_ch = CCCI_CCMNI5_TX, .rx_ch = CCCI_CCMNI5_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF5, .name = "ccmni4" },
	{ .tx_ch = CCCI_CCMNI6_TX, .rx_ch = CCCI_CCMNI6_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF6, .name = "ccmni5" },
	{ .tx_ch = CCCI_CCMNI7_TX, .rx_ch = CCCI_CCMNI7_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF7, .name = "ccmni6" },
	{ .tx_ch = CCCI_CCMNI8_TX, .rx_ch = CCCI_CCMNI8_RX, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF5, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF8, .name = "ccmni7" },
	/* char ports; ccci_monitor must stay first (downstream comment) */
	{ .tx_ch = CCCI_MONITOR_CH, .rx_ch = CCCI_MONITOR_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .flags = PORT_F_USER_HEADER, .ops = &ccci_char_port_ops,
	  .minor = 0, .name = "ccci_monitor" },
	{ .tx_ch = CCCI_PCM_TX, .rx_ch = CCCI_PCM_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 1, .name = "ccci_aud" },
	{ .tx_ch = CCCI_UART1_TX, .rx_ch = CCCI_UART1_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 3, .rxq_exp = 3, .ops = &ccci_char_port_ops,
	  .minor = 2, .name = "ccci_md_log_ctrl" },
	{ .tx_ch = CCCI_UART2_TX, .rx_ch = CCCI_UART2_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 3, .name = "ttyC0" },
	{ .tx_ch = CCCI_FS_TX, .rx_ch = CCCI_FS_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 1, .rxq_exp = 1, .ops = &ccci_char_port_ops,
	  .minor = 4, .name = "ccci_fs" },
	{ .tx_ch = CCCI_IPC_UART_TX, .rx_ch = CCCI_IPC_UART_RX, .txq = 1,
	  .rxq = 1, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 5, .name = "ttyC2" },
	{ .tx_ch = CCCI_ICUSB_TX, .rx_ch = CCCI_ICUSB_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 6, .name = "ttyC3" },
	{ .tx_ch = CCCI_MD_LOG_TX, .rx_ch = CCCI_MD_LOG_RX, .txq = 2, .rxq = 2,
	  .txq_exp = 2, .rxq_exp = 2, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_char_port_ops, .minor = 7, .name = "ttyC1" },
	{ .tx_ch = CCCI_IMSV_UL, .rx_ch = CCCI_IMSV_DL, .txq = 6, .rxq = 6,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 8, .name = "ccci_imsv" },
	{ .tx_ch = CCCI_IMSC_UL, .rx_ch = CCCI_IMSC_DL, .txq = 6, .rxq = 6,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 9, .name = "ccci_imsc" },
	{ .tx_ch = CCCI_IMSA_UL, .rx_ch = CCCI_IMSA_DL, .txq = 6, .rxq = 6,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 10, .name = "ccci_imsa" },
	{ .tx_ch = CCCI_IMSDC_UL, .rx_ch = CCCI_IMSDC_DL, .txq = 6, .rxq = 6,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 11, .name = "ccci_imsdc" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 12, .name = "ccci_ioctl0" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 13, .name = "ccci_ioctl1" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 14, .name = "ccci_ioctl2" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 15, .name = "ccci_ioctl3" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 16, .name = "ccci_ioctl4" },
	{ .tx_ch = CCCI_IT_TX, .rx_ch = CCCI_IT_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 17, .name = "ccci_it" },
	{ .tx_ch = CCCI_LB_IT_TX, .rx_ch = CCCI_LB_IT_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 18, .name = "ccci_lb_it" },
	{ .tx_ch = CCCI_MDL_MONITOR_UL, .rx_ch = CCCI_MDL_MONITOR_DL, .txq = 1,
	  .rxq = 1, .txq_exp = 3, .rxq_exp = 3, .ops = &ccci_char_port_ops,
	  .minor = 19, .name = "ccci_mdl_monitor" },
	{ .tx_ch = CCCI_RIL_IPC0_TX, .rx_ch = CCCI_RIL_IPC0_RX, .txq = 1,
	  .rxq = 1, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 20, .name = "ccci_umts_ipc0" },
	{ .tx_ch = CCCI_RIL_IPC1_TX, .rx_ch = CCCI_RIL_IPC1_RX, .txq = 1,
	  .rxq = 1, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 21, .name = "ccci_umts_ipc1" },
	{ .tx_ch = CCCI_RPC_TX, .rx_ch = CCCI_RPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 1, .rxq_exp = 1, .ops = &ccci_char_port_ops,
	  .minor = 22, .name = "ccci_rpc" },
	{ .tx_ch = CCCI_VT_CTL_TX, .rx_ch = CCCI_VT_CTL_RX, .txq = 7, .rxq = 7,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 23, .name = "ttyC4" },
	/* kernel ports */
	{ .tx_ch = CCCI_RPC_TX, .rx_ch = CCCI_RPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 1, .rxq_exp = 1, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci_rpc_k" },
	/* IPC char ports, minor = IPC index + CCCI_IPC_MINOR_BASE */
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 0, .name = "ccci_ipc_1220_0" },
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 2, .name = "ccci_ipc_2" },
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_kernel_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 3, .name = "ccci_ipc_3" },
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 4, .name = "ccci_ipc_4" },
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 5, .name = "ccci_ipc_5" },
	{ .tx_ch = CCCI_IPC_TX, .rx_ch = CCCI_IPC_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_kernel_port_ops,
	  .minor = CCCI_IPC_MINOR_BASE + 6, .name = "ccci_ipc_6" },
	/* system ports */
	{ .tx_ch = CCCI_CONTROL_TX, .rx_ch = CCCI_CONTROL_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0, .rxq_exp = 0, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci_ctrl" },
	{ .tx_ch = CCCI_SYSTEM_TX, .rx_ch = CCCI_SYSTEM_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci_sys" },
	{ .tx_ch = CCCI_STATUS_TX, .rx_ch = CCCI_STATUS_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0, .rxq_exp = 0, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci_poll" },
};

static const struct ccci_port ccci_md3_ports[] = {
	/* network ports, downstream port_cfg.c md3_ccci_ports */
	{ .tx_ch = CCCI_CCMNI1_TX, .rx_ch = CCCI_CCMNI1_RX, .txq = 2, .rxq = 2,
	  .txq_exp = 0xF2, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF1, .name = "cc3mni0" },
	{ .tx_ch = CCCI_CCMNI2_TX, .rx_ch = CCCI_CCMNI2_RX, .txq = 2, .rxq = 2,
	  .txq_exp = 0xF2, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF2, .name = "cc3mni1" },
	{ .tx_ch = CCCI_CCMNI3_TX, .rx_ch = CCCI_CCMNI3_RX, .txq = 2, .rxq = 2,
	  .txq_exp = 0xF2, .rxq_exp = 0xFF, .flags = PORT_F_RX_EXCLUSIVE,
	  .ops = &ccci_net_port_ops, .minor = 0xF3, .name = "cc3mni2" },
	{ .tx_ch = CCCI_MONITOR_CH, .rx_ch = CCCI_MONITOR_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .flags = PORT_F_USER_HEADER, .ops = &ccci_char_port_ops,
	  .minor = 0, .name = "ccci3_monitor" },
	{ .tx_ch = CCCI_PCM_TX, .rx_ch = CCCI_PCM_RX, .txq = 1, .rxq = 1,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 1, .name = "ccci3_aud" },
	{ .tx_ch = CCCI_UART1_TX, .rx_ch = CCCI_UART1_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0, .rxq_exp = 0, .ops = &ccci_char_port_ops,
	  .minor = 2, .name = "ccci3_md_log_ctrl" },
	{ .tx_ch = CCCI_FS_TX, .rx_ch = CCCI_FS_RX, .txq = 4, .rxq = 4,
	  .txq_exp = 4, .rxq_exp = 4, .ops = &ccci_char_port_ops,
	  .minor = 4, .name = "ccci3_fs" },
	{ .tx_ch = CCCI_IPC_UART_TX, .rx_ch = CCCI_IPC_UART_RX, .txq = 6,
	  .rxq = 6, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 5, .name = "ccci3_tty2" },
	{ .tx_ch = CCCI_MD_LOG_TX, .rx_ch = CCCI_MD_LOG_RX, .txq = 3, .rxq = 3,
	  .txq_exp = 3, .rxq_exp = 3, .ops = &ccci_char_port_ops,
	  .minor = 7, .name = "ccci3_tty1" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 12, .name = "ccci3_ioctl0" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 13, .name = "ccci3_ioctl1" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 14, .name = "ccci3_ioctl2" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 15, .name = "ccci3_ioctl3" },
	{ .tx_ch = CCCI_DUMMY_CH, .rx_ch = CCCI_DUMMY_CH, .txq = 0xFF,
	  .rxq = 0xFF, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 16, .name = "ccci3_ioctl4" },
	{ .tx_ch = CCCI_IT_TX, .rx_ch = CCCI_IT_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 17, .name = "ccci3_it" },
	{ .tx_ch = CCCI_LB_IT_TX, .rx_ch = CCCI_LB_IT_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 18, .name = "ccci3_lb_it" },
	/* c2k only ports */
	{ .tx_ch = CCCI_C2K_PPP_DATA, .rx_ch = CCCI_C2K_PPP_DATA, .txq = 2,
	  .rxq = 2, .txq_exp = 0xFF, .rxq_exp = 0xFF,
	  .ops = &ccci_char_port_ops, .minor = 19, .name = "ccci3_data" },
	{ .tx_ch = CCCI_C2K_AT, .rx_ch = CCCI_C2K_AT, .txq = 5, .rxq = 5,
	  .txq_exp = 0xF4, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 20, .name = "ccci3_at" },
	{ .tx_ch = CCCI_C2K_AT2, .rx_ch = CCCI_C2K_AT2, .txq = 5, .rxq = 5,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 21, .name = "ccci3_at2" },
	{ .tx_ch = CCCI_C2K_AT3, .rx_ch = CCCI_C2K_AT3, .txq = 5, .rxq = 5,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 22, .name = "ccci3_at3" },
	{ .tx_ch = CCCI_C2K_LB_DL, .rx_ch = CCCI_C2K_LB_DL, .txq = 5, .rxq = 5,
	  .txq_exp = 0xFF, .rxq_exp = 0xFF, .ops = &ccci_char_port_ops,
	  .minor = 23, .name = "ccci3_lb_dl" },
	/* system ports */
	{ .tx_ch = CCCI_CONTROL_TX, .rx_ch = CCCI_CONTROL_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0, .rxq_exp = 0, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci3_ctrl" },
	{ .tx_ch = CCCI_STATUS_TX, .rx_ch = CCCI_STATUS_RX, .txq = 0, .rxq = 0,
	  .txq_exp = 0, .rxq_exp = 0, .ops = &ccci_kernel_port_ops,
	  .minor = 0, .name = "ccci3_poll" },
};

int ccci_md_port_cfg(struct ccci_md *md)
{
	const struct ccci_port *table;
	unsigned int count, i;
	struct ccci_port *ports;

	switch (md->id) {
	case CCCI_MD_SYS1:
		table = ccci_md1_ports;
		count = ARRAY_SIZE(ccci_md1_ports);
		break;
	case CCCI_MD_SYS3:
		table = ccci_md3_ports;
		count = ARRAY_SIZE(ccci_md3_ports);
		break;
	default:
		ccci_md_err(md, "no port table for this modem\n");
		return -ENODEV;
	}

	ports = kmemdup(table, sizeof(*ports) * count, GFP_KERNEL);
	if (!ports)
		return -ENOMEM;

	md->ports = ports;
	md->port_count = count;

	for (i = 0; i < count; i++) {
		struct ccci_port *port = &ports[i];

		port->md = md;
		port->rx_len_th = CCCI_PORT_RX_LEN_TH;
		skb_queue_head_init(&port->rx_q);
		spin_lock_init(&port->rx_lock);
		init_waitqueue_head(&port->rx_wq);
		atomic_set(&port->open_count, 0);
		INIT_LIST_HEAD(&port->node);
		INIT_LIST_HEAD(&port->rx_node);
		list_add_tail(&port->node, &md->ports_list);
		if (port->rx_ch < CCCI_MAX_CH_NUM)
			list_add_tail(&port->rx_node, &md->rx_ch_ports[port->rx_ch]);

		if (port->ops && port->ops->init) {
			int ret = port->ops->init(port);

			if (ret)
				ccci_md_warn(md, "port %s init failed %d\n",
					     port->name, ret);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* module init                                                         */
/* ------------------------------------------------------------------ */
static int __init ccci_init(void)
{
	int ret;

	ccci_class = class_create(CCCI_DEV_NAME);
	if (IS_ERR(ccci_class))
		return PTR_ERR(ccci_class);

	ret = ccci_sysfs_init();
	if (ret)
		goto err_class;

	ret = ccci_util_init();
	if (ret) {
		pr_err("ccci: util init failed %d\n", ret);
		goto err_sysfs;
	}

	/*
	 * Kernel RPC services (GPIO/ADC/EINT, DSP MPU, queue remap), the
	 * counterpart of downstream ccci_subsys_kernel_init(); registered
	 * before the platform drivers probe so a booting modem can query
	 * them immediately.
	 */
	ret = ccci_rpc_init();
	if (ret) {
		pr_err("ccci: rpc init failed %d\n", ret);
		goto err_util;
	}

	ret = ccci_platform_register();
	if (ret)
		goto err_rpc;

	pr_info("ccci: core initialised (driver ver 0x%08x)\n", CCCI_DRIVER_VER);
	return 0;

err_rpc:
	ccci_rpc_exit();
err_util:
	ccci_util_exit();
err_sysfs:
	ccci_sysfs_exit();
err_class:
	class_destroy(ccci_class);
	return ret;
}

static void __exit ccci_exit(void)
{
	ccci_platform_unregister();
	ccci_rpc_exit();
	ccci_util_exit();
	ccci_sysfs_exit();
	class_destroy(ccci_class);
}

module_init(ccci_init);
module_exit(ccci_exit);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek CCCI modem core (MT6735/MT6737T port)");
MODULE_LICENSE("GPL");
