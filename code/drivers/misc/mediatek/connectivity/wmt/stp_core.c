// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN STP (Serial Transport Protocol) engine.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/stp_core.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/stp_exp.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * STP is a sliding-window protocol carried over the BTIF serial link.  Each
 * frame is
 *
 *	seq/ack | type/len_hi | len_lo | header checksum | payload | crc16
 *
 * with an optional 0x55 0x55 delimiter.  Payloads are demultiplexed into one
 * ring buffer per logical channel (BT/FM/GPS/WIFI/WMT/STP/INFO/ANT) and the
 * registered channel callback is invoked without any STP lock held.
 *
 * The power-saving (PSM) and firmware-coredump (BTM) engines live in
 * stp_psm.c and stp_btm.c.  The STP transmit path funnels every non-WMT
 * frame through the PSM filter, and a firmware assert message received on the
 * STP channel starts the coredump collector instead of resetting the chip
 * immediately.
 */

#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <mtk_wcn_stp.h>

#include "wmt_local.h"
#include "stp_psm.h"

/* Downstream static configuration: NAK and delimiter generation disabled
 * (stp_core.c:40-41).
 */
#define STP_ENABLE_NAK		0
#define STP_ENABLE_DELIMITER	0

static const u8 stp_delimiter[STP_DEL_SIZE] = { 0x55, 0x55 };

struct stp_ctx {
	struct mutex lock;		/* protects tx window, ack state */
	struct mutex parse_lock;	/* serialises the rx parser */
	struct stp_ring ring[MTKSTP_MAX_TASK_NUM];
	struct stp_parser parser;
	struct stp_sequence sequence;

	u8 rx_buf[MTKSTP_BUFFER_SIZE];
	u32 rx_counter;

	u8 tx_buf[MTKSTP_BUFFER_SIZE];
	u32 tx_read;
	u32 tx_write;
	u32 tx_start_addr[MTKSTP_SEQ_SIZE];
	u32 tx_length[MTKSTP_SEQ_SIZE];

	struct timer_list tx_timer;
	struct work_struct tx_timeout_work;
	struct work_struct reset_work;

	bool enabled;
	bool ready;
	bool bluez;
	u32 mode;			/* MTKSTP_*_MODE bits */
	u8 pending_type;
	u32 wmt_last_close;
	u32 evt_err_assert;
	bool en_coredump;		/* collection allowed */
	bool f_coredump;		/* firmware is asserting, TX blocked */
};

static struct stp_ctx stp;
static int stp_ctx_ready;
static enum stp_parser_state stp_prev_state;
static int stp_packet_fail_count;

static int (*stp_if_tx[STP_TX_IF_MAX])(const u8 *data, u32 size, u32 *written);
static mtk_wcn_stp_if_rx stp_if_rx;
static enum mtk_wcn_stp_tx_if stp_if_type = STP_TX_IF_MAX;
static mtk_wcn_stp_event_cb stp_event_cb[MTKSTP_MAX_TASK_NUM];
static mtk_wcn_stp_event_cb stp_tx_event_cb[MTKSTP_MAX_TASK_NUM];

/*
 * TX side helpers
 */

static int stp_sys_if_tx(const u8 *data, u32 size, u32 *written)
{
	if (stp_if_type >= STP_TX_IF_MAX || !stp_if_tx[stp_if_type])
		return -EIO;

	return stp_if_tx[stp_if_type](data, size, written);
}

static bool stp_tx_room(u32 length)
{
	u32 room;

	if (stp.tx_read <= stp.tx_write)
		room = MTKSTP_BUFFER_SIZE - stp.tx_write + stp.tx_read - 1;
	else
		room = stp.tx_read - stp.tx_write - 1;

	if (room < length) {
		stp_err("tx queue room shortage\n");
		return false;
	}

	return true;
}

static void stp_add_to_tx_queue(const u8 *buffer, u32 length)
{
	u32 last_len;

	if (length + stp.tx_write < MTKSTP_BUFFER_SIZE) {
		memcpy(stp.tx_buf + stp.tx_write, buffer, length);
		stp.tx_write += length;
	} else {
		last_len = MTKSTP_BUFFER_SIZE - stp.tx_write;
		memcpy(stp.tx_buf + stp.tx_write, buffer, last_len);
		memcpy(stp.tx_buf, buffer + last_len, length - last_len);
		stp.tx_write = length - last_len;
	}
}

/* stp_core.c:267-291 */
static void stp_update_tx_queue(u32 txseq)
{
	u32 tx_read = stp.tx_start_addr[txseq];
	u8 checksum = 0;
	int i;

	stp.tx_buf[tx_read] &= 0xf8;
	stp.tx_buf[tx_read] |= stp.sequence.txack;

	for (i = 0; i < 3; i++) {
		checksum += stp.tx_buf[tx_read];
		tx_read++;
		if (tx_read >= MTKSTP_BUFFER_SIZE)
			tx_read -= MTKSTP_BUFFER_SIZE;
	}

	stp.tx_buf[tx_read] = checksum;
}

/* stp_core.c:712-752 */
static void stp_send_tx_queue(u32 txseq)
{
	u32 ret = 0;
	u32 tx_read = stp.tx_start_addr[txseq];
	u32 tx_length = stp.tx_length[txseq];
	u32 last_len;

	stp_update_tx_queue(txseq);

	if (tx_read + tx_length < MTKSTP_BUFFER_SIZE) {
		stp_sys_if_tx(&stp.tx_buf[tx_read], tx_length, &ret);
		if (ret != tx_length)
			stp_err("stp_send_tx_queue %u/%u\n", tx_length, ret);
	} else {
		last_len = MTKSTP_BUFFER_SIZE - tx_read;
		stp_sys_if_tx(&stp.tx_buf[tx_read], last_len, &ret);
		if (ret != last_len)
			stp_err("stp_send_tx_queue %u/%u\n", last_len, ret);

		stp_sys_if_tx(&stp.tx_buf[0], tx_length - last_len, &ret);
		if (ret != tx_length - last_len)
			stp_err("stp_send_tx_queue %u/%u\n",
				tx_length - last_len, ret);
	}
}

/* stp_core.c:762-802 */
static void stp_send_ack(u8 tx_ack, u8 nak)
{
	u8 header[MTKSTP_HEADER_SIZE];
	u32 ret = 0;

	header[0] = 0x80 + (0 << 3) + tx_ack;
	if (STP_ENABLE_NAK)
		header[1] = nak ? 0x80 : 0x00;
	else
		header[1] = 0x00;
	header[2] = 0;
	header[3] = (header[0] + header[1] + header[2]) & 0xff;

	if (STP_ENABLE_DELIMITER)
		stp_sys_if_tx(stp_delimiter, STP_DEL_SIZE, &ret);

	ret = 0;
	stp_sys_if_tx(&header[0], MTKSTP_HEADER_SIZE, &ret);
	if (ret != MTKSTP_HEADER_SIZE)
		stp_err("stp_send_ack %u/%u\n", MTKSTP_HEADER_SIZE, ret);
}

/*
 * RX side helpers
 */

/* Requires stp.parse_lock and stp.lock. */
static void stp_rest_ctx_state(void)
{
	int i;

	stp.rx_counter = 0;

	for (i = 0; i < MTKSTP_MAX_TASK_NUM; i++) {
		unsigned long flags;

		spin_lock_irqsave(&stp.ring[i].lock, flags);
		stp.ring[i].read_p = 0;
		stp.ring[i].write_p = 0;
		spin_unlock_irqrestore(&stp.ring[i].lock, flags);
	}

	stp.tx_write = 0;
	stp.tx_read = 0;

	stp.parser.state = STP_SYNC;
	stp.sequence.txseq = 0;
	stp.sequence.txack = 7;
	stp.sequence.rxack = 7;
	stp.sequence.winspace = MTKSTP_WINSIZE;
	stp.sequence.expected_rxseq = 0;
	stp.sequence.retry_times = 0;
}

static void stp_change_rx_state(enum stp_parser_state next)
{
	stp_prev_state = stp.parser.state;
	stp.parser.state = next;
}

/* stp_core.c:604-652 */
static int stp_add_to_rx_queue(u8 *buffer, u32 length, u8 type)
{
	struct stp_ring *ring;
	unsigned long flags;
	u32 room, last_len;

	if (type >= MTKSTP_MAX_TASK_NUM)
		return -EINVAL;

	ring = &stp.ring[type];
	spin_lock_irqsave(&ring->lock, flags);

	if (ring->read_p <= ring->write_p)
		room = MTKSTP_BUFFER_SIZE - ring->write_p + ring->read_p - 1;
	else
		room = ring->read_p - ring->write_p - 1;

	if (room < length) {
		spin_unlock_irqrestore(&ring->lock, flags);
		stp_err("queue %u is full\n", type);
		return -ENOSPC;
	}

	if (length + ring->write_p < MTKSTP_BUFFER_SIZE) {
		memcpy(ring->buffer + ring->write_p, buffer, length);
		ring->write_p += length;
	} else {
		last_len = MTKSTP_BUFFER_SIZE - ring->write_p;
		memcpy(ring->buffer + ring->write_p, buffer, last_len);
		memcpy(ring->buffer, buffer + last_len, length - last_len);
		ring->write_p = length - last_len;
	}

	spin_unlock_irqrestore(&ring->lock, flags);

	return 0;
}

/* stp_core.c:921-963 */
static int stp_process_rxack(void)
{
	u8 rxack;
	int j = 0, k = 0;

	if (stp.sequence.rxack == stp.parser.ack)
		return -1;

	rxack = stp.sequence.rxack;
	rxack = (rxack + 1) & 0x7;
	while (rxack != stp.sequence.txseq) {
		j++;
		if (rxack == stp.parser.ack) {
			k = 1;
			break;
		}
		rxack = (rxack + 1) & 0x7;
	}

	if (k != 1)
		return -1;

	stp.sequence.rxack = stp.parser.ack;
	stp.tx_read = stp.tx_start_addr[rxack] + stp.tx_length[rxack];
	if (stp.tx_read >= MTKSTP_BUFFER_SIZE)
		stp.tx_read -= MTKSTP_BUFFER_SIZE;

	stp.sequence.winspace += j;
	stp.sequence.retry_times = 0;

	timer_delete(&stp.tx_timer);
	if (stp.sequence.winspace != MTKSTP_WINSIZE)
		mod_timer(&stp.tx_timer,
			  jiffies + msecs_to_jiffies(MTKSTP_TX_TIMEOUT));

	return 0;
}

static bool stp_function_active(u8 type)
{
	return type < MTKSTP_MAX_TASK_NUM && stp_event_cb[type];
}

/* stp_core.c:965-1160
 *
 * Runs with stp.parse_lock held; the TX window and ack state are protected by
 * stp.lock inside the critical sections.  Channel callbacks are invoked after
 * stp.lock has been released, as required by mtk_wcn_stp.h.
 */
static void stp_process_packet(void)
{
	int trigger_resume = -1;
	u8 tx_ack = 0;
	static int rx_ok;
	bool active;
	bool notify = false;
	int ret;

	mutex_lock(&stp.lock);

	if (stp.sequence.expected_rxseq == stp.parser.seq &&
	    stp.parser.type == BT_TASK_INDX && stp.bluez) {
		/* BlueZ stack: hand the HCI packet straight to the HCI core. */
		mutex_unlock(&stp.lock);
		if (stp_if_rx)
			stp_if_rx(stp.rx_buf, stp.rx_counter);
		else
			stp_err("bluez packet without registered rx handler\n");
		mutex_lock(&stp.lock);

		trigger_resume = stp_process_rxack();
		stp.sequence.txack = stp.parser.seq;
		stp.sequence.expected_rxseq =
			(stp.sequence.expected_rxseq + 1) & 0x7;
		tx_ack = stp.sequence.txack;
		stp_send_ack(tx_ack, 0);
		rx_ok = 0;
	} else if (stp.sequence.expected_rxseq == stp.parser.seq) {
		active = stp_function_active(stp.parser.type);

		trigger_resume = stp_process_rxack();
		stp.sequence.txack = stp.parser.seq;
		stp.sequence.expected_rxseq =
			(stp.sequence.expected_rxseq + 1) & 0x7;
		tx_ack = stp.sequence.txack;
		stp_send_ack(tx_ack, 0);

		if (stp.parser.type < MTKSTP_MAX_TASK_NUM && active) {
			if (stp.parser.type == WMT_TASK_INDX &&
			    stp.parser.wmt_subtype == WMT_LTE_COEX_FLAG) {
				stp_warn("LTE coex frame on non-LTE project, dropped\n");
				rx_ok = 0;
			} else {
				ret = stp_add_to_rx_queue(stp.rx_buf,
							  stp.rx_counter,
							  stp.parser.type);
				rx_ok = ret ? -1 : 0;
			}
		} else {
			if (active)
				stp_err("type %u is invalid, dropped\n",
					stp.parser.type);
			else
				stp_err("function %u is inactive, dropped\n",
					stp.parser.type);
			rx_ok = 0;
		}

		if (!rx_ok) {
			stp_packet_fail_count = 0;
			if (stp.parser.type == WMT_TASK_INDX &&
			    stp.parser.wmt_subtype == WMT_LTE_COEX_FLAG)
				stp_warn("LTE coex frame dropped\n");
			else
				notify = true;
		} else {
			stp_packet_fail_count++;
			/* A full GPS queue is drained rather than retried. */
			if (stp.parser.type == GPS_TASK_INDX)
				mtk_wcn_stp_flush_rx_queue(GPS_TASK_INDX);
			else
				notify = true;
			/* Do not ack: let the peer retry. */
			stp_err("enqueue to rx queue %u failed\n",
				stp.parser.type);
		}
	} else if (!rx_ok) {
		stp_err("expected_rxseq %u, parser.seq %u\n",
			stp.sequence.expected_rxseq, stp.parser.seq);
		stp_packet_fail_count++;
		stp_send_ack(stp.sequence.txack, 1);
	} else {
		stp_packet_fail_count++;
		stp_err("sequence mismatch after enqueue failure\n");
	}

	mutex_unlock(&stp.lock);

	if (notify && stp_event_cb[stp.parser.type])
		stp_event_cb[stp.parser.type]();

	if (trigger_resume == 0) {
		int i;

		for (i = 0; i < MTKSTP_MAX_TASK_NUM; i++)
			if (stp_tx_event_cb[i])
				stp_tx_event_cb[i]();
	}

	if (stp_packet_fail_count > MTKSTP_RETRY_LIMIT) {
		stp_packet_fail_count = 0;
		stp_err("too many packet failures, resetting CONSYS\n");
		stp.ready = false;
		schedule_work(&stp.reset_work);
	}
}

/* stp_core.c:1956-2210 (mandatory mode, no sequence/ack/crc) */
static int stp_parser_data_in_mand_mode(u32 length, u8 *p_data)
{
	u8 padding_len;
	int remain_length;
	int i = length;

	while (i > 0) {
		switch (stp.parser.state) {
		case STP_SYNC:
			if ((*p_data & 0x80) == 0x80) {
				if (*p_data != 0x80)
					stp_warn("mandatory mode sync byte 0x%x\n",
						 *p_data);
				stp_change_rx_state(STP_NAK);
				stp.rx_counter++;
			} else {
				stp_warn("sync to sync, byte 0x%x\n", *p_data);
				stp_change_rx_state(STP_SYNC);
			}
			break;
		case STP_NAK:
			stp_change_rx_state(STP_LENGTH);
			stp.parser.type = (*p_data & 0x70) >> 4;
			if (stp.parser.type <= MTKSTP_MAX_TASK_NUM) {
				stp.parser.length = (*p_data & 0x0f) << 8;
				stp.rx_counter++;
			} else {
				stp_warn("invalid type, back to sync\n");
				stp_change_rx_state(STP_SYNC);
			}
			break;
		case STP_LENGTH:
			stp_change_rx_state(STP_CHECKSUM);
			stp.parser.length += *p_data;
			if (stp.parser.length < 2000) {
				stp.rx_counter++;
			} else {
				stp_warn("invalid STP length %u\n",
					 stp.parser.length);
				stp_change_rx_state(STP_SYNC);
				stp.rx_counter = 0;
			}
			break;
		case STP_CHECKSUM:
			if (stp.parser.type == STP_TASK_INDX ||
			    stp.parser.type == INFO_TASK_INDX) {
				stp_change_rx_state(STP_FW_MSG);
				stp.rx_counter = 0;
				i -= 1;
				if (i != 0)
					p_data += 1;
				continue;
			}
			if (stp.parser.length == 0) {
				stp_change_rx_state(STP_SYNC);
				stp.rx_counter = 0;
			} else {
				stp_change_rx_state(STP_DATA);
				stp.rx_counter = 0;
			}
			break;
		case STP_DATA:
			if (stp.parser.length < stp.rx_counter) {
				stp_err("abnormal length %u/%u in DATA\n",
					stp.parser.length, stp.rx_counter);
				stp_change_rx_state(STP_SYNC);
				stp.rx_counter = 0;
				return -1;
			}
			remain_length = stp.parser.length - stp.rx_counter;
			if (i >= remain_length) {
				if (stp.rx_counter + remain_length >=
				    MTKSTP_BUFFER_SIZE) {
					stp_err("rx buffer overrun\n");
					stp_change_rx_state(STP_SYNC);
					stp.rx_counter = 0;
					return -1;
				}
				memcpy(stp.rx_buf + stp.rx_counter, p_data,
				       remain_length);
				i -= remain_length;
				p_data += remain_length;
				stp.rx_counter = stp.parser.length;
				stp.parser.state = STP_CRC1;
				continue;
			}
			if (i + stp.rx_counter >= MTKSTP_BUFFER_SIZE) {
				stp_err("rx buffer overrun 2\n");
				stp.rx_counter = 0;
				return -1;
			}
			memcpy(stp.rx_buf + stp.rx_counter, p_data, i);
			stp.rx_counter += i;
			i = 0;
			continue;
		case STP_CRC1:
			stp_change_rx_state(STP_CRC2);
			break;
		case STP_CRC2:
			if (stp.parser.type == WMT_TASK_INDX)
				stp.parser.wmt_subtype = stp.rx_buf[1];

			/* STP packets are 4-byte aligned in mandatory mode. */
			if (i <= 4) {
				p_data += (i - 1);
				i -= (i - 1);
			} else {
				padding_len = (0x04 -
					((stp.parser.length + 6) & 0x03)) & 0x03;
				p_data += padding_len;
				i -= padding_len;
			}

			if (stp.parser.type == BT_TASK_INDX && stp.bluez) {
				if (stp_if_rx)
					stp_if_rx(stp.rx_buf, stp.rx_counter);
				else
					stp_err("bluez packet without handler\n");
			} else if (stp_function_active(stp.parser.type)) {
				if (stp.parser.type == WMT_TASK_INDX &&
				    stp.parser.wmt_subtype ==
				    WMT_LTE_COEX_FLAG) {
					stp_warn("LTE coex frame dropped\n");
				} else {
					stp_add_to_rx_queue(stp.rx_buf,
							    stp.rx_counter,
							    stp.parser.type);
					if (stp_event_cb[stp.parser.type])
						stp_event_cb[stp.parser.type]();
				}
			} else {
				stp_err("function %u inactive, dropped\n",
					stp.parser.type);
			}

			stp.rx_counter = 0;
			stp_change_rx_state(STP_SYNC);
			break;
		case STP_FW_MSG:
			stp_err("firmware message, length %u (not dumped)\n",
				stp.parser.length);
			stp.rx_counter = 0;
			stp_change_rx_state(STP_SYNC);
			break;
		default:
			break;
		}
		p_data++;
		i--;
	}

	return 0;
}

/* stp_core.c:1779-1954 (full mode) */
static int stp_parser_data_in_full_mode(u32 length, u8 *p_data)
{
	int remain_length;
	int i = length;

	while (i > 0) {
		switch (stp.parser.state) {
		case STP_RESYNC1:
			stp_change_rx_state(*p_data == 0x7f ?
					    STP_RESYNC2 : STP_RESYNC1);
			break;
		case STP_RESYNC2:
			stp_change_rx_state(*p_data == 0x7f ?
					    STP_RESYNC3 : STP_RESYNC1);
			break;
		case STP_RESYNC3:
			stp_change_rx_state(*p_data == 0x7f ?
					    STP_RESYNC4 : STP_RESYNC1);
			break;
		case STP_RESYNC4:
			stp_change_rx_state(*p_data == 0x7f ?
					    STP_SYNC : STP_RESYNC1);
			break;
		case STP_SYNC:
			if ((*p_data & 0x80) == 0x80 &&
			    (*p_data & 0x40) == 0x00) {
				stp_change_rx_state(STP_NAK);
				stp.parser.seq = (*p_data & 0x38) >> 3;
				stp.parser.ack = *p_data & 0x07;
				stp.rx_buf[0] = *p_data;
			} else if (*p_data == 0x7f &&
				   stp_prev_state == STP_RESYNC4) {
				stp_err("continuous resync pattern\n");
			} else if (*p_data == 0x7f) {
				stp_change_rx_state(STP_RESYNC2);
				stp_err("resync pattern start\n");
			} else if (*p_data == 0x55) {
				/* STP delimiter, ignored */
			} else {
				stp_err("unexpected byte 0x%x in sync\n",
					*p_data);
			}
			break;
		case STP_NAK:
			stp.parser.nak = STP_ENABLE_NAK ?
				((*p_data & 0x80) >> 7) : 0;
			stp.parser.type = (*p_data & 0x70) >> 4;
			stp.parser.length = (*p_data & 0x0f) << 8;
			stp.rx_buf[1] = *p_data;
			if (stp.parser.nak)
				stp_err("NAK received\n");

			if (stp.parser.type < MTKSTP_MAX_TASK_NUM)
				stp_change_rx_state(STP_LENGTH);
			else
				stp_change_rx_state(STP_SYNC);
			break;
		case STP_LENGTH:
			stp_change_rx_state(STP_CHECKSUM);
			stp.parser.length += *p_data;
			if (stp.parser.length > MTKSTP_MAX_PKT_SIZE) {
				stp_err("invalid STP length %u\n",
					stp.parser.length);
				stp_change_rx_state(STP_RESYNC1);
				stp.rx_counter = 0;
				return -1;
			}
			stp.rx_buf[2] = *p_data;
			break;
		case STP_CHECKSUM:
			if (stp.parser.type == STP_TASK_INDX ||
			    stp.parser.type == INFO_TASK_INDX) {
				stp_change_rx_state(STP_FW_MSG);
				stp.rx_counter = 0;
				i -= 1;
				if (i != 0)
					p_data += 1;
				continue;
			}

			if (((stp.rx_buf[0] + stp.rx_buf[1] +
			      stp.rx_buf[2]) & 0xff) != *p_data) {
				stp_err("header checksum error %02x %02x %02x %02x\n",
					stp.rx_buf[0], stp.rx_buf[1],
					stp.rx_buf[2], *p_data);
				stp_change_rx_state(STP_RESYNC1);
				stp.rx_counter = 0;
				return -1;
			}

			if (stp.parser.length == 0) {
				int trigger_resume;

				mutex_lock(&stp.lock);
				trigger_resume = stp_process_rxack();
				mutex_unlock(&stp.lock);

				if (trigger_resume == 0) {
					int t;

					for (t = 0; t < MTKSTP_MAX_TASK_NUM; t++)
						if (stp_tx_event_cb[t])
							stp_tx_event_cb[t]();
				}
				stp_change_rx_state(STP_SYNC);
				stp.rx_counter = 0;
			} else {
				stp_change_rx_state(STP_DATA);
				stp.rx_counter = 0;
			}
			break;
		case STP_DATA:
			if (stp.parser.length < stp.rx_counter) {
				stp_err("abnormal length %u/%u in DATA\n",
					stp.parser.length, stp.rx_counter);
				stp_change_rx_state(STP_RESYNC1);
				stp.rx_counter = 0;
				return -1;
			}
			remain_length = stp.parser.length - stp.rx_counter;
			if (i >= remain_length) {
				memcpy(stp.rx_buf + stp.rx_counter, p_data,
				       remain_length);
				i -= remain_length;
				p_data += remain_length;
				stp.rx_counter = stp.parser.length;
				stp.parser.state = STP_CRC1;
				continue;
			}
			if (i + stp.rx_counter >= MTKSTP_BUFFER_SIZE) {
				stp_err("rx buffer overrun\n");
				stp_change_rx_state(STP_RESYNC1);
				stp.rx_counter = 0;
				return -1;
			}
			memcpy(stp.rx_buf + stp.rx_counter, p_data, i);
			stp.rx_counter += i;
			i = 0;
			continue;
		case STP_CRC1:
			stp_change_rx_state(STP_CRC2);
			stp.parser.crc = *p_data;
			break;
		case STP_CRC2:
			stp_change_rx_state(STP_SYNC);
			stp.parser.crc += (*p_data) << 8;
			if (stp.parser.type == WMT_TASK_INDX)
				stp.parser.wmt_subtype = stp.rx_buf[1];

			if (crc16(0, stp.rx_buf, stp.rx_counter) !=
			    stp.parser.crc) {
				stp_err("payload CRC error (len %u)\n",
					stp.rx_counter);
				stp_change_rx_state(STP_RESYNC1);
				stp.rx_counter = 0;
				return -1;
			}
			stp_process_packet();
			break;
		case STP_FW_MSG:
			/* Firmware assert/trace messages are not decoded; the
			 * payload is consumed so that the stream resynchronises.
			 * stp_core.c:2035-2165.
			 */
			if (stp.parser.length < stp.rx_counter) {
				stp_err("abnormal length %u/%u in FW_MSG\n",
					stp.parser.length, stp.rx_counter);
				stp_change_rx_state(STP_SYNC);
				stp.rx_counter = 0;
				return -1;
			}
			remain_length = stp.parser.length - stp.rx_counter;
			if (i < remain_length) {
				if (i + stp.rx_counter >= MTKSTP_BUFFER_SIZE) {
					stp_change_rx_state(STP_RESYNC1);
					stp.rx_counter = 0;
					return -1;
				}
				memcpy(stp.rx_buf + stp.rx_counter, p_data, i);
				stp.rx_counter += i;
				i = 0;
				continue;
			}

			memcpy(stp.rx_buf + stp.rx_counter, p_data,
			       remain_length);
			i -= remain_length;
			p_data += remain_length;
			stp.rx_counter = stp.parser.length;
			stp_change_rx_state(STP_SYNC);

			if (stp.parser.type == STP_TASK_INDX) {
				/* mtk_wcn_stp_ctx_save() (stp_core.c:3300-3310):
				 * stop transmitting and stop asking the MCU to
				 * sleep while the dump is collected.
				 */
				mtk_wcn_stp_coredump_start_ctrl(1);
				stp_psm_set_sleep_disable();
				stp.ready = false;

				if (stp.en_coredump && stp.rx_counter) {
					stp_warn("firmware assert message (%u bytes), collecting coredump\n",
						 stp.rx_counter);
					stp_btm_assert(stp.rx_buf,
						       stp.rx_counter);
				} else {
					stp_warn("firmware assert message (%u bytes), resetting\n",
						 stp.rx_counter);
					mtk_wcn_stp_coredump_start_ctrl(0);
					schedule_work(&stp.reset_work);
				}
			} else {
				stp_warn("firmware trace message (%u bytes)\n",
					 stp.rx_counter);
			}
			stp.rx_counter = 0;

			if (i >= 2) {
				i -= 2;
				if (i > 0)
					p_data += 2;
			}
			continue;
		default:
			break;
		}
		p_data++;
		i--;
	}

	return 0;
}

/*
 * TX timeout handling (stp_core.c:345-495)
 */

static void stp_do_tx_timeout(void)
{
	u8 resync[4];
	u32 seq, ret = 0;

	mutex_lock(&stp.lock);

	if (stp.sequence.retry_times > MTKSTP_RETRY_LIMIT) {
		stp_info("retry limit reached, stop\n");
		mutex_unlock(&stp.lock);
		return;
	}

	seq = (stp.sequence.rxack + 1) & 0x7;
	if (seq != stp.sequence.txseq) {
		memset(resync, 0x7f, sizeof(resync));
		stp_sys_if_tx(resync, sizeof(resync), &ret);
		if (ret != sizeof(resync))
			stp_err("failed to send resync pattern\n");

		do {
			stp_warn("resend STP packet seq %u\n", seq);
			stp_send_tx_queue(seq);
			seq = (seq + 1) & 0x7;
		} while (seq != stp.sequence.txseq);
	}

	timer_delete(&stp.tx_timer);
	if (stp.sequence.winspace != MTKSTP_WINSIZE) {
		mod_timer(&stp.tx_timer,
			  jiffies + msecs_to_jiffies(MTKSTP_TX_TIMEOUT));
		stp.sequence.retry_times++;
		if (stp.sequence.retry_times > MTKSTP_RETRY_LIMIT) {
			timer_delete(&stp.tx_timer);
			stp_err("TX retry limit %d, resetting CONSYS\n",
				MTKSTP_RETRY_LIMIT);
			stp.ready = false;
			mutex_unlock(&stp.lock);
			schedule_work(&stp.reset_work);
			return;
		}
	}

	mutex_unlock(&stp.lock);
}

static void stp_tx_timeout_work_fn(struct work_struct *work)
{
	stp_do_tx_timeout();
}

static void stp_tx_timeout_cb(struct timer_list *timer)
{
	schedule_work(&stp.tx_timeout_work);
}

static void stp_reset_work_fn(struct work_struct *work)
{
	wmt_lib_schedule_reset(WMTRSTSRC_RESET_STP);
}

/*
 * Public STP API
 */

bool mtk_wcn_stp_is_ready(void)
{
	return stp_ctx_ready && stp.ready;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_is_ready);

/* The actual frame builder, shared by the PSM-filtered path and the
 * hold-FIFO release path.
 */
static int __stp_send_data(const u8 *buffer, u32 length, u8 type)
{
	u8 header[MTKSTP_HEADER_SIZE], crc_bytes[2];
	u16 crc;
	u32 written = 0;
	int ret = 0;

	mutex_lock(&stp.lock);

	if (!stp.enabled && type != WMT_TASK_INDX) {
		mutex_unlock(&stp.lock);
		return -EAGAIN;
	}

	if (stp.enabled && mtk_wcn_stp_is_btif_mand_mode()) {
		/* stp_core.c:2512-2555 */
		header[0] = 0x80;
		header[1] = (type << 4) + ((length >> 8) & 0x0f);
		header[2] = length & 0xff;
		header[3] = 0x00;

		memcpy(stp.tx_buf, header, MTKSTP_HEADER_SIZE);
		memcpy(stp.tx_buf + MTKSTP_HEADER_SIZE, buffer, length);
		crc_bytes[0] = 0;
		crc_bytes[1] = 0;
		memcpy(stp.tx_buf + MTKSTP_HEADER_SIZE + length, crc_bytes, 2);

		stp_sys_if_tx(stp.tx_buf,
			      MTKSTP_HEADER_SIZE + length + MTKSTP_CRC_SIZE,
			      &written);
		if (written == MTKSTP_HEADER_SIZE + length + MTKSTP_CRC_SIZE)
			ret = length;
		else
			ret = -EIO;
		mutex_unlock(&stp.lock);
		return ret;
	}

	if (stp.enabled && mtk_wcn_stp_is_btif_fullset_mode()) {
		if (stp.sequence.winspace > 0 &&
		    stp_tx_room(MTKSTP_HEADER_SIZE + length +
				MTKSTP_CRC_SIZE)) {
			header[0] = 0x80 +
				    (stp.sequence.txseq << 3) +
				    stp.sequence.txack;
			header[1] = (type << 4) + ((length & 0xf00) >> 8);
			header[2] = length & 0xff;
			header[3] = (header[0] + header[1] + header[2]) & 0xff;

			stp.tx_start_addr[stp.sequence.txseq] = stp.tx_write;
			stp.tx_length[stp.sequence.txseq] =
				MTKSTP_HEADER_SIZE + length + MTKSTP_CRC_SIZE;

			if (STP_ENABLE_DELIMITER) {
				stp.tx_length[stp.sequence.txseq] +=
					STP_DEL_SIZE;
				stp_add_to_tx_queue(stp_delimiter,
						    STP_DEL_SIZE);
			}

			stp_add_to_tx_queue(header, MTKSTP_HEADER_SIZE);
			stp_add_to_tx_queue(buffer, length);

			crc = crc16(0, buffer, length);
			crc_bytes[0] = crc & 0xff;
			crc_bytes[1] = (crc & 0xff00) >> 8;
			stp_add_to_tx_queue(crc_bytes, 2);

			stp_send_tx_queue(stp.sequence.txseq);
			stp.sequence.txseq =
				(stp.sequence.txseq + 1) & 0x7;
			stp.sequence.winspace--;

			timer_delete(&stp.tx_timer);
			if (stp.sequence.winspace != MTKSTP_WINSIZE)
				mod_timer(&stp.tx_timer,
					  jiffies +
					  msecs_to_jiffies(MTKSTP_TX_TIMEOUT));

			ret = length;
		} else {
			stp_err("no window space / tx room\n");
			ret = 0;
		}
	}

	mutex_unlock(&stp.lock);

	return ret;
}

int mtk_wcn_stp_send_data(const u8 *buffer, u32 length, u8 type)
{
	bool psm_locked = false;
	int ret;

	if (!buffer)
		return -EINVAL;
	if (length > MTKSTP_MAX_PKT_SIZE)
		return -EINVAL;

	if (stp.wmt_last_close) {
		stp_err("link closed by WMT, dropping tx\n");
		return length;
	}

	/* Firmware assert/coredump in progress: drop everything, as the
	 * downstream driver does while f_coredump is set (stp_core.c:2420).
	 */
	if (stp.f_coredump) {
		stp_warn("firmware coredump in progress, dropping tx\n");
		return length;
	}

	/* PSM filtering (stp_core.c:2428-2510).  When PSM is disabled this
	 * returns 0 and no lock is taken, i.e. the link stays always-awake.
	 */
	ret = stp_psm_tx_begin(buffer, length, type, &psm_locked);
	if (ret) {
		if (ret > 0)
			return ret;	/* queued in the hold FIFO */
		return ret;
	}

	ret = __stp_send_data(buffer, length, type);
	stp_psm_tx_end(type, psm_locked);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_send_data);

/* Transmit without the PSM filter.  Used by the PSM engine itself when it
 * releases data after a wakeup: re-entering the filter would deadlock on the
 * PSM API lock.
 */
int mtk_wcn_stp_send_data_no_ps(const u8 *buffer, u32 length, u8 type)
{
	if (!buffer)
		return -EINVAL;
	if (length > MTKSTP_MAX_PKT_SIZE)
		return -EINVAL;
	if (stp.wmt_last_close)
		return length;
	if (stp.f_coredump)
		return length;

	return __stp_send_data(buffer, length, type);
}

/* Raw channel used by WMT before STP is formally enabled (stp_core.c:2653-2700).
 */
int mtk_wcn_stp_send_data_raw(const u8 *buffer, u32 length, u8 type)
{
	u32 written = 0;

	if (!buffer)
		return -EINVAL;

	if (stp.wmt_last_close) {
		stp_err("link closed by WMT, dropping raw tx\n");
		return length;
	}

	mutex_lock(&stp.lock);
	stp.pending_type = type;
	stp_sys_if_tx(buffer, length, &written);
	mutex_unlock(&stp.lock);

	if (written == length)
		return written;

	stp_err("raw tx %u/%u\n", length, written);
	return -EIO;
}

int mtk_wcn_stp_receive_data(u8 *buffer, u32 length, u8 type)
{
	struct stp_ring *ring;
	unsigned long flags;
	u32 copy_len = 0, tail_len;

	if (!buffer || type >= MTKSTP_MAX_TASK_NUM)
		return -EINVAL;

	ring = &stp.ring[type];
	spin_lock_irqsave(&ring->lock, flags);

	while (ring->read_p != ring->write_p) {
		if (ring->write_p > ring->read_p) {
			copy_len = ring->write_p - ring->read_p;
			if (copy_len > length)
				copy_len = length;
			memcpy(buffer, ring->buffer + ring->read_p, copy_len);
			ring->read_p += copy_len;
		} else {
			tail_len = MTKSTP_BUFFER_SIZE - ring->read_p;
			if (tail_len > length) {
				copy_len = length;
				memcpy(buffer, ring->buffer + ring->read_p,
				       copy_len);
				ring->read_p += copy_len;
			} else {
				memcpy(buffer, ring->buffer + ring->read_p,
				       tail_len);
				buffer += tail_len;

				copy_len = length - tail_len;
				copy_len = min(copy_len, ring->write_p);
				if (copy_len)
					memcpy(buffer, ring->buffer, copy_len);
				ring->read_p = copy_len;
				copy_len += tail_len;
			}
		}
		break;
	}

	spin_unlock_irqrestore(&ring->lock, flags);

	stp_psm_rx_done(type, copy_len);

	return copy_len;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_receive_data);

bool mtk_wcn_stp_is_rxqueue_empty(u8 type)
{
	struct stp_ring *ring;
	unsigned long flags;
	bool empty;

	if (type >= MTKSTP_MAX_TASK_NUM)
		return true;

	ring = &stp.ring[type];
	spin_lock_irqsave(&ring->lock, flags);
	empty = ring->read_p == ring->write_p;
	spin_unlock_irqrestore(&ring->lock, flags);

	return empty;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_is_rxqueue_empty);

/* stp_core.c:2233-2300 */
int mtk_wcn_stp_parser_data(u8 *buffer, u32 length)
{
	int ret = 0;

	if (!buffer)
		return -EINVAL;

	if (!stp_ctx_ready)
		return -ENODEV;

	if (!stp.enabled) {
		if (stp.pending_type == WMT_TASK_INDX) {
			/* Raw WMT traffic before STP is enabled. */
			stp_add_to_rx_queue(buffer, length, WMT_TASK_INDX);
			if (stp_event_cb[WMT_TASK_INDX])
				stp_event_cb[WMT_TASK_INDX]();
		}
		return 0;
	}

	mutex_lock(&stp.parse_lock);
	if (mtk_wcn_stp_is_btif_mand_mode())
		ret = stp_parser_data_in_mand_mode(length, buffer);
	else if (mtk_wcn_stp_is_btif_fullset_mode())
		ret = stp_parser_data_in_full_mode(length, buffer);
	mutex_unlock(&stp.parse_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_parser_data);

int mtk_wcn_stp_register_event_cb(int type, mtk_wcn_stp_event_cb func)
{
	if (type < 0 || type >= MTKSTP_MAX_TASK_NUM)
		return -EINVAL;

	stp_event_cb[type] = func;

	/* stp_exp.c:200-216: a new consumer always starts with an empty queue. */
	mtk_wcn_stp_flush_rx_queue(type);

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_register_event_cb);

int mtk_wcn_stp_register_tx_event_cb(int type, mtk_wcn_stp_event_cb func)
{
	if (type < 0 || type >= MTKSTP_MAX_TASK_NUM)
		return -EINVAL;

	stp_tx_event_cb[type] = func;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_register_tx_event_cb);

int mtk_wcn_stp_register_if_rx(mtk_wcn_stp_if_rx func)
{
	stp_if_rx = func;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_register_if_rx);

int mtk_wcn_stp_register_if_tx(enum mtk_wcn_stp_tx_if stp_interface,
			       mtk_wcn_stp_if_tx func)
{
	if (stp_interface >= STP_TX_IF_MAX) {
		stp_warn("STP_IF_TX(%d) out of boundary\n", stp_interface);
		return -EINVAL;
	}

	stp_if_tx[stp_interface] = func;
	if (func && stp_if_type == STP_TX_IF_MAX)
		stp_if_type = stp_interface;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_register_if_tx);

/*
 * Internal STP control used by the WMT core
 */

int stp_drv_init(void)
{
	int i;

	memset(&stp, 0, sizeof(stp));
	mutex_init(&stp.lock);
	mutex_init(&stp.parse_lock);

	for (i = 0; i < MTKSTP_MAX_TASK_NUM; i++)
		spin_lock_init(&stp.ring[i].lock);

	timer_setup(&stp.tx_timer, stp_tx_timeout_cb, 0);
	INIT_WORK(&stp.tx_timeout_work, stp_tx_timeout_work_fn);
	INIT_WORK(&stp.reset_work, stp_reset_work_fn);

	stp.enabled = false;
	stp.ready = false;
	stp.bluez = false;
	stp.mode = 0;
	stp.pending_type = 0;
	stp.wmt_last_close = 0;
	stp.evt_err_assert = 0;
	stp.en_coredump = true;
	stp.f_coredump = false;

	i = stp_psm_init();
	if (i)
		return i;

	i = stp_btm_init();
	if (i) {
		stp_psm_deinit();
		return i;
	}

	stp_ctx_ready = 1;
	return 0;
}

void stp_drv_exit(void)
{
	if (!stp_ctx_ready)
		return;

	stp_ctx_ready = 0;
	cancel_work_sync(&stp.reset_work);
	cancel_work_sync(&stp.tx_timeout_work);
	timer_delete_sync(&stp.tx_timer);

	stp_btm_deinit();
	stp_psm_deinit();

	mutex_lock(&stp.lock);
	stp.enabled = false;
	stp.ready = false;
	memset(stp_event_cb, 0, sizeof(stp_event_cb));
	memset(stp_tx_event_cb, 0, sizeof(stp_tx_event_cb));
	memset(stp_if_tx, 0, sizeof(stp_if_tx));
	stp_if_rx = NULL;
	stp_if_type = STP_TX_IF_MAX;
	mutex_unlock(&stp.lock);

	mutex_destroy(&stp.parse_lock);
	mutex_destroy(&stp.lock);
}

int mtk_wcn_stp_enable(int value)
{
	mutex_lock(&stp.parse_lock);
	mutex_lock(&stp.lock);
	stp_rest_ctx_state();
	stp.enabled = !!value;
	mutex_unlock(&stp.lock);
	mutex_unlock(&stp.parse_lock);

	/* stp_core.c:2280-2290 */
	if (!value) {
		mtk_wcn_stp_psm_reset();
		stp_btm_reset();
		stp.f_coredump = false;
	} else {
		mtk_wcn_stp_coredump_start_ctrl(0);
	}

	stp_dbg("STP %s\n", value ? "enabled" : "disabled");
	return 0;
}

int mtk_wcn_stp_ready(int value)
{
	if (!stp_ctx_ready)
		return -ENODEV;

	stp.ready = !!value;
	stp_dbg("STP %s\n", value ? "ready" : "not ready");
	return 0;
}

void mtk_wcn_stp_flush_rx_queue(u32 type)
{
	struct stp_ring *ring;
	unsigned long flags;

	if (type >= MTKSTP_MAX_TASK_NUM)
		return;

	ring = &stp.ring[type];
	spin_lock_irqsave(&ring->lock, flags);
	ring->read_p = 0;
	ring->write_p = 0;
	spin_unlock_irqrestore(&ring->lock, flags);
}

void mtk_wcn_stp_flush_context(void)
{
	if (!stp_ctx_ready)
		return;

	mutex_lock(&stp.parse_lock);
	mutex_lock(&stp.lock);
	stp_rest_ctx_state();
	mutex_unlock(&stp.lock);
	mutex_unlock(&stp.parse_lock);
}

void mtk_wcn_stp_set_mode(u32 mode)
{
	stp.mode = mode;
	stp_dbg("STP mode 0x%08x\n", mode);
}

void mtk_wcn_stp_set_if_tx_type(enum mtk_wcn_stp_tx_if type)
{
	if (type >= STP_TX_IF_MAX) {
		stp_err("invalid tx interface %d\n", type);
		return;
	}

	stp_if_type = type;
}

void mtk_wcn_stp_set_bluez(bool bluez)
{
	stp.bluez = bluez;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_set_bluez);

bool mtk_wcn_stp_is_btif_fullset_mode(void)
{
	return stp.mode & MTKSTP_BTIF_FULL_MODE;
}

bool mtk_wcn_stp_is_btif_mand_mode(void)
{
	return stp.mode & MTKSTP_BTIF_MAND_MODE;
}

int mtk_wcn_stp_set_wmt_last_close(u32 value)
{
	stp.wmt_last_close = value;
	stp_dbg("wmt_last_close %u\n", value);
	return 0;
}

/* stp_core.c:3300-3330 */
u32 mtk_wcn_stp_get_wmt_evt_err_trg_assert(void)
{
	return stp.evt_err_assert;
}

void mtk_wcn_stp_set_wmt_evt_err_trg_assert(u32 value)
{
	stp_info("event error assert flag %u\n", value);
	stp.evt_err_assert = value;
}

/* stp_core.c:2342-2375 */
int mtk_wcn_stp_coredump_start_ctrl(u32 value)
{
	stp_info("set f/w assert (%u)\n", value);
	stp.f_coredump = !!value;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_coredump_start_ctrl);

u32 mtk_wcn_stp_coredump_start_get(void)
{
	return stp.f_coredump;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_coredump_start_get);

int mtk_wcn_stp_coredump_flag_ctrl(u32 on)
{
	stp.en_coredump = !!on;
	stp_info("coredump collection %s\n", on ? "enabled" : "disabled");
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_coredump_flag_ctrl);

int mtk_wcn_stp_coredump_flag_get(void)
{
	return stp.en_coredump;
}
EXPORT_SYMBOL_GPL(mtk_wcn_stp_coredump_flag_get);
