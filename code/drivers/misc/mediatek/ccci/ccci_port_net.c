// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI network ports: bind the logical CCMNI channels to the CCMNI netdev
 * driver through struct ccmni_ccci_ops.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_net.c.  Original copyright
 * (C) 2015 MediaTek Inc., GPL-2.0.
 */
#include <linux/ccci.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/string.h>

#include "ccci_priv.h"

#define CCCI_CCMNI_IDX(port)	(((port)->minor & 0xF) - 1)

/* downstream ccmni.h:121-124 */
#define CCMNI_ERR_TX_OK		0
#define CCMNI_ERR_TX_BUSY	(-1)
#define CCMNI_ERR_MD_NO_READY	(-2)
#define CCMNI_ERR_TX_INVAL	(-3)

#define CCMNI_NAME_MD1		"ccmni"
#define CCMNI_NAME_MD3		"cc3mni"

/* ------------------------------------------------------------------ */
/* channel mapping                                                     */
/* ------------------------------------------------------------------ */
static void ccci_ccmni_default_channel(int md_id, int idx,
				       struct ccmni_ch *channel)
{
	/* downstream port_net.c:56 ccmni_get_default_channel() */
	switch (idx) {
	case 0:
		channel->rx = CCCI_CCMNI1_RX;
		channel->tx = CCCI_CCMNI1_TX;
		channel->dl_ack = CCCI_CCMNI1_DL_ACK;
		break;
	case 1:
		channel->rx = CCCI_CCMNI2_RX;
		channel->tx = CCCI_CCMNI2_TX;
		channel->dl_ack = CCCI_CCMNI2_DL_ACK;
		break;
	case 2:
		channel->rx = CCCI_CCMNI3_RX;
		channel->tx = CCCI_CCMNI3_TX;
		channel->dl_ack = CCCI_CCMNI3_DL_ACK;
		break;
	case 3:
		channel->rx = CCCI_CCMNI4_RX;
		channel->tx = CCCI_CCMNI4_TX;
		channel->dl_ack = CCCI_CCMNI4_DLACK_RX;
		break;
	case 4:
		channel->rx = CCCI_CCMNI5_RX;
		channel->tx = CCCI_CCMNI5_TX;
		channel->dl_ack = CCCI_CCMNI5_DLACK_RX;
		break;
	case 5:
		channel->rx = CCCI_CCMNI6_RX;
		channel->tx = CCCI_CCMNI6_TX;
		channel->dl_ack = CCCI_CCMNI6_DLACK_RX;
		break;
	case 6:
		channel->rx = CCCI_CCMNI7_RX;
		channel->tx = CCCI_CCMNI7_TX;
		channel->dl_ack = CCCI_CCMNI7_DLACK_RX;
		break;
	case 7:
		channel->rx = CCCI_CCMNI8_RX;
		channel->tx = CCCI_CCMNI8_TX;
		channel->dl_ack = CCCI_CCMNI8_DLACK_RX;
		break;
	default:
		pr_err("ccci: invalid ccmni index %d for md%d\n", idx,
		       md_id + 1);
		channel->rx = CCCI_MAX_CH_NUM;
		channel->tx = CCCI_MAX_CH_NUM;
		channel->dl_ack = CCCI_MAX_CH_NUM;
	}

	channel->rx_ack = 0xFF;
	channel->tx_ack = 0xFF;
	channel->multiq = 0;
}

static int ccci_ccmni_dl_ack(int idx)
{
	static const int dl_ack[8] = {
		CCCI_CCMNI1_DL_ACK, CCCI_CCMNI2_DL_ACK, CCCI_CCMNI3_DL_ACK,
		CCCI_CCMNI4_DLACK_RX, CCCI_CCMNI5_DLACK_RX,
		CCCI_CCMNI6_DLACK_RX, CCCI_CCMNI7_DLACK_RX,
		CCCI_CCMNI8_DLACK_RX,
	};

	return idx >= 0 && idx < 8 ? dl_ack[idx] : CCCI_MAX_CH_NUM;
}

int ccci_get_ccmni_channel(int md_id, int ccmni_idx,
			   struct ccmni_ch *channel)
{
	struct ccci_md *md = ccci_md_get(md_id);
	struct ccci_port *port;

	if (!channel)
		return -EINVAL;

	if (!md) {
		ccci_ccmni_default_channel(md_id, ccmni_idx, channel);
		return 0;
	}

	/* the ccmni ports carry different minors (0xF1..0xF8) than their index */
	list_for_each_entry(port, &md->ports_list, node) {
		if (port->ops != &ccci_net_port_ops)
			continue;
		if (port->minor != (0xF0 | (ccmni_idx + 1)))
			continue;
		channel->rx = port->rx_ch;
		channel->rx_ack = 0xFF;
		channel->tx = port->tx_ch;
		channel->tx_ack = 0xFF;
		channel->dl_ack = ccci_ccmni_dl_ack(ccmni_idx);
		channel->multiq = md_id == CCCI_MD_SYS1 ? 1 : 0;
		return 0;
	}

	ccci_ccmni_default_channel(md_id, ccmni_idx, channel);
	return 0;
}
EXPORT_SYMBOL_GPL(ccci_get_ccmni_channel);

/* ------------------------------------------------------------------ */
/* Tx                                                                  */
/* ------------------------------------------------------------------ */
static bool ccci_net_ack_channel(unsigned int tx_ch, unsigned int *port_ch)
{
	switch (tx_ch) {
	case CCCI_CCMNI1_DL_ACK:
		*port_ch = CCCI_CCMNI1_TX;
		return true;
	case CCCI_CCMNI2_DL_ACK:
		*port_ch = CCCI_CCMNI2_TX;
		return true;
	case CCCI_CCMNI3_DL_ACK:
		*port_ch = CCCI_CCMNI3_TX;
		return true;
	case CCCI_CCMNI4_DLACK_RX:
		*port_ch = CCCI_CCMNI4_TX;
		return true;
	case CCCI_CCMNI5_DLACK_RX:
		*port_ch = CCCI_CCMNI5_TX;
		return true;
	case CCCI_CCMNI6_DLACK_RX:
		*port_ch = CCCI_CCMNI6_TX;
		return true;
	case CCCI_CCMNI7_DLACK_RX:
		*port_ch = CCCI_CCMNI7_TX;
		return true;
	case CCCI_CCMNI8_DLACK_RX:
		*port_ch = CCCI_CCMNI8_TX;
		return true;
	default:
		*port_ch = tx_ch;
		return false;
	}
}

/*
 * ccmni_send_pkt - CCMNI -> modem packet path
 *
 * The skb arrives with the L3 payload at skb->data and at least
 * sizeof(struct ccci_header) of headroom; the CCCI header is pushed here.
 * On success (0) the skb is consumed.  On -CCMNI_ERR_TX_BUSY the header is
 * pulled again so CCMNI can retry with the same skb.
 */
static int ccci_ccmni_send_pkt(int md_id, int tx_ch, void *data)
{
	struct ccci_md *md = ccci_md_get(md_id);
	struct ccci_port *port;
	struct ccci_header *ccci_h;
	struct sk_buff *skb = data;
	unsigned int port_ch, qno;
	bool is_ack;
	int ret;

	if (!md || !skb)
		return CCMNI_ERR_TX_INVAL;
	if (md->state != CCCI_MD_READY)
		return CCMNI_ERR_MD_NO_READY;

	is_ack = ccci_net_ack_channel(tx_ch, &port_ch);
	port = ccci_md_get_port_by_channel(md, port_ch);
	if (!port)
		return CCMNI_ERR_TX_INVAL;

	if (skb_headroom(skb) < CCCI_HEADER_SIZE)
		return CCMNI_ERR_TX_INVAL;

	/* downstream NET_ACK_TXQ_INDEX(): ack packets share the lower nibble
	 * of the exception queue index
	 */
	if (is_ack)
		qno = port->txq_exp == 0xFF ? port->txq :
		      (port->txq_exp & 0x0F);
	else
		qno = ccci_port_txq(port);
	if (qno >= CCCI_MAX_QUEUES)
		return CCMNI_ERR_TX_INVAL;

	ccci_h = (struct ccci_header *)skb_push(skb, CCCI_HEADER_SIZE);
	memset(ccci_h, 0, CCCI_HEADER_SIZE);
	ccci_h->data[0] = 0;
	ccci_h->data[1] = skb->len;
	ccci_h->channel = tx_ch;
	ccci_h->reserved = 0;

	port->tx_pkts++;
	ret = ccci_md_send_skb(md, qno, skb);
	if (ret == 0)
		return CCMNI_ERR_TX_OK;

	skb_pull(skb, CCCI_HEADER_SIZE);
	if (ret == -EBUSY) {
		port->tx_busy_cnt++;
		return CCMNI_ERR_TX_BUSY;
	}
	return CCMNI_ERR_TX_INVAL;
}

/*
 * Downstream ccmni_napi_poll() returns 0: the ECCCI/CCMNI split delivers Rx
 * straight through ccmni_rx_callback(), the CCMNI napi is not the Rx path.
 */
static int ccci_ccmni_napi_poll(int md_id, int rx_ch, struct napi_struct *napi,
				int weight)
{
	return 0;
}

static int ccci_net_port_init(struct ccci_port *port);

/* ------------------------------------------------------------------ */
/* Rx                                                                  */
/* ------------------------------------------------------------------ */
static int ccci_net_recv_skb(struct ccci_port *port, struct sk_buff *skb)
{
	struct ccci_md *md = port->md;
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
	int idx = CCCI_CCMNI_IDX(port);

	ccci_md_dbg(md, "rx net ch%u len%u idx%d\n", ccci_h->channel, skb->len,
		    idx);

	if (idx < 0 || idx >= 8) {
		ccci_md_err(md, "bad ccmni index for port %s (minor 0x%x)\n",
			    port->name, port->minor);
		dev_kfree_skb_any(skb);
		return -CCCI_ERR_DROP_PACKET;
	}

	/* CCMNI expects the L3 payload at skb->data; the 16 byte ccci header
	 * leaves exactly the headroom it needs for the Ethernet header.
	 */
	skb_pull(skb, CCCI_HEADER_SIZE);
	port->rx_pkts++;

	/* consumes the skb in every path */
	ccmni_rx_callback(md->id, idx, skb);
	return 0;
}

static void ccci_net_md_state_notice(struct ccci_port *port, int event)
{
	struct ccci_md *md = port->md;
	int idx = CCCI_CCMNI_IDX(port);

	switch (event) {
	case CCCI_MD_READY:
	case CCCI_MD_EXCEPTION:
	case CCCI_MD_STOPPED:
		if (idx >= 0 && idx < 8)
			ccmni_md_state_callback(md->id, idx, event);
		break;
	case CCCI_EV_TX_FULL:
		port->rx_full = true;
		break;
	case CCCI_EV_TX_IRQ:
		port->rx_full = false;
		break;
	default:
		break;
	}
}

const struct ccci_port_ops ccci_net_port_ops = {
	.init = ccci_net_port_init,
	.recv_skb = ccci_net_recv_skb,
	.md_state_notice = ccci_net_md_state_notice,
};

/* ------------------------------------------------------------------ */
/* per modem binding                                                   */
/* ------------------------------------------------------------------ */
int ccci_port_net_init(struct ccci_md *md)
{
	struct ccmni_ccci_ops *ops;
	bool has_net = false;
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->ops == &ccci_net_port_ops) {
			has_net = true;
			break;
		}
	}
	if (!has_net)
		return 0;
	if (md->ccmni_inited)
		return 0;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	ops->ccmni_ver = CCMNI_DRV_V0;
	ops->ccmni_num = md->id == CCCI_MD_SYS1 ? 8 : 3;
	strscpy(ops->name, md->id == CCCI_MD_SYS1 ? CCMNI_NAME_MD1 :
		CCMNI_NAME_MD3, sizeof(ops->name));
	ops->md_ability = md->capability | CCCI_MD_CAP_DATA_ACK_DVD;
	ops->irat_md_id = md->irat_md_id;
	ops->napi_poll_weigh = NAPI_POLL_WEIGHT;
	ops->send_pkt = ccci_ccmni_send_pkt;
	ops->napi_poll = ccci_ccmni_napi_poll;
	ops->get_ccmni_ch = ccci_get_ccmni_channel;

	md->ccmni_ops = ops;
	if (ccmni_init(md->id, ops)) {
		ccci_md_err(md, "ccmni_init failed\n");
		md->ccmni_ops = NULL;
		kfree(ops);
		return -EINVAL;
	}
	md->ccmni_inited = true;
	ccci_md_info(md, "%s bound (%u devices, ability 0x%x)\n", ops->name,
		     ops->ccmni_num, ops->md_ability);
	return 0;
}

void ccci_port_net_exit(struct ccci_md *md)
{
	if (!md->ccmni_inited)
		return;
	ccmni_exit(md->id);
	md->ccmni_inited = false;
	kfree(md->ccmni_ops);
	md->ccmni_ops = NULL;
}

/* used by the net port init hook in the port table walk */
static int ccci_net_port_init(struct ccci_port *port)
{
	return ccci_port_net_init(port->md);
}
