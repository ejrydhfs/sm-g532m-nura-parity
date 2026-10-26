// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN FM radio - STP link layer.
 *
 * Port from Samsung/MTK downstream drivers/misc/mediatek/fmradio/core/fm_link.c
 * and drivers/misc/mediatek/fmradio/core/fm_eint.c.
 *
 * The FM function of the connectivity SoC is driven over the STP FM channel
 * (FM_TASK_INDX): commands are framed as
 *
 *	[0x01][opcode][length:le16][payload]
 *
 * and firmware events as
 *
 *	[0x04][opcode][length:le16][payload].
 *
 * Every command waits for the event carrying the matching opcode (or a
 * derived completion bit such as FLAG_TUNE_DONE), retrying once like the
 * downstream driver does.
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "mtk_wcn_fm.h"

/* Single FM instance, as downstream g_fm_struct (core/fm_main.c:26). */
static struct mtk_wcn_fm_dev *mtk_wcn_fm_instance;

enum mtk_wcn_fm_parser_state {
	FM_RX_PKT_TYPE = 0,
	FM_RX_OPCODE,
	FM_RX_LEN_LO,
	FM_RX_LEN_HI,
	FM_RX_PAYLOAD,
};

static void mtk_wcn_fm_evt_set(struct mtk_wcn_fm_dev *fm, u64 bits)
{
	spin_lock(&fm->evt_lock);
	fm->evt_bits |= bits;
	spin_unlock(&fm->evt_lock);
	wake_up_interruptible(&fm->evt_wq);
}

static int mtk_wcn_fm_wait_event(struct mtk_wcn_fm_dev *fm, u64 mask,
				 unsigned int timeout_s)
{
	long left;

	left = wait_event_interruptible_timeout(fm->evt_wq,
						(READ_ONCE(fm->evt_bits) & mask) != 0,
						msecs_to_jiffies(timeout_s * 1000));
	if (left > 0) {
		spin_lock(&fm->evt_lock);
		fm->evt_bits &= ~mask;
		spin_unlock(&fm->evt_lock);
		return 0;
	}

	return left ? -ERESTARTSYS : -ETIMEDOUT;
}

/**
 * mtk_wcn_fm_cmd_tx - hand a command packet to the FM firmware and wait
 * @fm: driver state
 * @buf: command packet, built with the header already filled in
 * @len: packet length including the 4 byte header
 * @mask: event bits that complete the command
 * @timeout_s: per-attempt timeout in seconds
 *
 * The caller must hold fm->cmd_lock, which protects the single outstanding
 * command (downstream cmd_buf_lock in mt6627_fm_lib.c:158).
 *
 * Return: 0 on completion, negative errno on failure.
 */
int mtk_wcn_fm_cmd_tx(struct mtk_wcn_fm_dev *fm, const u8 *buf, u16 len,
		      u64 mask, unsigned int timeout_s)
{
	int ret;
	int retry = FM_SW_RETRY_CNT;

	if (!fm->link_ready)
		return -ENODEV;
	if (!buf || !len || !mask)
		return -EINVAL;

	spin_lock(&fm->evt_lock);
	fm->evt_bits &= ~mask;
	spin_unlock(&fm->evt_lock);

	do {
		ret = mtk_wcn_stp_send_data(buf, len, FM_TASK_INDX);
		if (ret <= 0) {
			dev_err(fm->dev, "STP send failed (%d)\n", ret);
			return -EIO;
		}

		ret = mtk_wcn_fm_wait_event(fm, mask, timeout_s);
		if (ret != -ETIMEDOUT)
			return ret;

		dev_warn(fm->dev, "event 0x%02x timed out (%us)\n", buf[1],
			 timeout_s);
	} while (retry-- > 0);

	return ret;
}

static void mtk_wcn_fm_rds_push(struct mtk_wcn_fm_dev *fm, const u8 *payload,
				u16 len)
{
	struct mtk_wcn_fm_rds_data data;

	if (len > sizeof(data)) {
		dev_warn_ratelimited(fm->dev,
				     "oversized RDS event (%u bytes)\n", len);
		return;
	}

	memset(&data, 0, sizeof(data));
	memcpy(&data, payload, len);

	if (kfifo_in(&fm->rds_fifo, &data, sizeof(data)) != sizeof(data))
		dev_warn_ratelimited(fm->dev, "RDS fifo overrun\n");

	wake_up_interruptible(&fm->rds_wq);
}

/*
 * Event payload handling, downstream fm_event_parser() switch on opcode
 * (core/fm_link.c:304-408).
 */
static void mtk_wcn_fm_dispatch(struct mtk_wcn_fm_dev *fm, const u8 *payload,
				u16 len)
{
	u8 opcode = fm->parser_opcode;
	u64 bits = 0;

	if (!len) {
		/* core/fm_link.c:294-300 */
		if (opcode == CSPI_WRITE_OPCODE)
			bits = FM_EVT_CSPI_WRITE;
		else if (opcode < 32)
			bits = FM_EVT_BIT(opcode);
		goto out;
	}

	switch (opcode) {
	case FM_TUNE_OPCODE:
		/* core/fm_link.c:307-312: payload[0] == 1 means tuned */
		if (len == 1 && payload[0] == 1)
			bits = FM_EVT_TUNE_DONE;
		break;
	case FM_SEEK_OPCODE:
		/* core/fm_link.c:323-329 */
		if (len >= 2)
			fm->seek_result = payload[0] | (payload[1] << 8);
		bits = FM_EVT_SEEK_DONE;
		break;
	case FSPI_READ_OPCODE:
		/* core/fm_link.c:353-358 */
		if (len >= 2)
			fm->fspi_rd = payload[0] | (payload[1] << 8);
		bits = FM_EVT_BIT(opcode);
		break;
	case CSPI_READ_OPCODE:
	case FM_HOST_READ_OPCODE:
		/* core/fm_link.c:360-381 */
		if (len >= 4)
			fm->cspi_rd = payload[0] | (payload[1] << 8) |
				      (payload[2] << 16) |
				      ((u32)payload[3] << 24);
		bits = (opcode == CSPI_READ_OPCODE) ? FM_EVT_CSPI_READ :
						      FM_EVT_HOST_READ;
		break;
	case RDS_RX_DATA_OPCODE:
		/* core/fm_link.c:383-403 */
		mtk_wcn_fm_rds_push(fm, payload, len);
		bits = FM_EVT_BIT(opcode);
		break;
	default:
		/* core/fm_link.c:405-407 */
		if (opcode < 32)
			bits = FM_EVT_BIT(opcode);
		break;
	}

out:
	if (bits)
		mtk_wcn_fm_evt_set(fm, bits);
}

/*
 * Packet parser.  The state is kept across calls because the STP transport
 * may deliver a frame in several chunks (downstream kept it in a function
 * static, core/fm_link.c:231).
 */
static void mtk_wcn_fm_parse(struct mtk_wcn_fm_dev *fm, const u8 *buf, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		u8 ch = buf[i];

		switch (fm->parser_state) {
		case FM_RX_PKT_TYPE:
			if (ch == FM_PKT_TYPE_EVENT)
				fm->parser_state = FM_RX_OPCODE;
			else
				dev_warn_ratelimited(fm->dev,
						     "bad event packet type 0x%02x\n",
						     ch);
			break;
		case FM_RX_OPCODE:
			fm->parser_opcode = ch;
			fm->parser_state = FM_RX_LEN_LO;
			break;
		case FM_RX_LEN_LO:
			fm->parser_len = ch;
			fm->parser_state = FM_RX_LEN_HI;
			break;
		case FM_RX_LEN_HI:
			fm->parser_len |= (u16)ch << 8;
			fm->parser_pos = 0;
			if (fm->parser_len > sizeof(fm->rx_payload)) {
				dev_warn_ratelimited(fm->dev,
						     "oversized event 0x%02x (%u bytes)\n",
						     fm->parser_opcode,
						     fm->parser_len);
				fm->parser_state = FM_RX_PKT_TYPE;
			} else if (!fm->parser_len) {
				mtk_wcn_fm_dispatch(fm, NULL, 0);
				fm->parser_state = FM_RX_PKT_TYPE;
			} else {
				fm->parser_state = FM_RX_PAYLOAD;
			}
			break;
		case FM_RX_PAYLOAD:
			fm->rx_payload[fm->parser_pos++] = ch;
			if (fm->parser_pos == fm->parser_len) {
				mtk_wcn_fm_dispatch(fm, fm->rx_payload,
						    fm->parser_len);
				fm->parser_state = FM_RX_PKT_TYPE;
			}
			break;
		}
	}
}

static void mtk_wcn_fm_rx_work(struct work_struct *work)
{
	struct mtk_wcn_fm_dev *fm = container_of(work, struct mtk_wcn_fm_dev,
						 rx_work);
	int len;
	int i;

	for (i = 0; i < 16; i++) {
		len = mtk_wcn_stp_receive_data(fm->rx_buf, sizeof(fm->rx_buf),
					       FM_TASK_INDX);
		if (len <= 0)
			break;

		mtk_wcn_fm_parse(fm, fm->rx_buf, len);
	}
}

/*
 * STP channel callback (downstream fm_request_eint() registers
 * fm_eint_handler(), core/fm_eint.c:34-39; the handler only schedules work,
 * core/fm_main.c:2267-2275).
 */
static void mtk_wcn_fm_stp_event_cb(void)
{
	struct mtk_wcn_fm_dev *fm = mtk_wcn_fm_instance;

	if (fm && fm->link_ready)
		schedule_work(&fm->rx_work);
}

/*
 * WMT message callback (downstream WCNfm_wholechip_rst_cb(),
 * core/fm_link.c:34-63).  The connectivity SoC reset destroys the FM
 * firmware state, so the driver has to re-run the power up sequence and
 * restore the user visible state.
 */
static void mtk_wcn_fm_wmt_msgcb(enum mtk_wcn_wmt_drv src,
				 enum mtk_wcn_wmt_drv dst,
				 enum mtk_wcn_wmt_msg msg, void *data, u32 len)
{
	struct mtk_wcn_fm_dev *fm = mtk_wcn_fm_instance;
	u32 rst;

	if (!fm || src != WMTDRV_WMT || dst != WMTDRV_FM ||
	    msg != WMTMSG_RESET)
		return;

	if (!data || len < sizeof(rst)) {
		dev_warn(fm->dev, "malformed WMT reset message\n");
		return;
	}
	memcpy(&rst, data, sizeof(rst));

	switch (rst) {
	case FM_WMTRSTMSG_RESET_START:
		fm->resetting = true;
		break;
	case FM_WMTRSTMSG_RESET_END:
		fm->resetting = false;
		schedule_work(&fm->rst_work);
		break;
	default:
		break;
	}
}

int mtk_wcn_fm_link_init(struct mtk_wcn_fm_dev *fm)
{
	int ret;

	if (mtk_wcn_fm_instance)
		return -EBUSY;

	mtk_wcn_fm_instance = fm;
	fm->parser_state = FM_RX_PKT_TYPE;
	INIT_WORK(&fm->rx_work, mtk_wcn_fm_rx_work);

	ret = mtk_wcn_stp_register_event_cb(FM_TASK_INDX,
					    mtk_wcn_fm_stp_event_cb);
	if (ret) {
		dev_err(fm->dev, "cannot register STP event callback (%d)\n",
			ret);
		mtk_wcn_fm_instance = NULL;
		return ret;
	}

	ret = mtk_wcn_wmt_msgcb_reg(WMTDRV_FM, mtk_wcn_fm_wmt_msgcb);
	if (ret) {
		dev_err(fm->dev, "cannot register WMT callback (%d)\n", ret);
		mtk_wcn_stp_register_event_cb(FM_TASK_INDX, NULL);
		mtk_wcn_fm_instance = NULL;
		return ret;
	}

	fm->link_ready = true;
	dev_dbg(fm->dev, "FM link established\n");

	return 0;
}

void mtk_wcn_fm_link_exit(struct mtk_wcn_fm_dev *fm)
{
	if (!fm->link_ready)
		return;

	fm->link_ready = false;
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_FM);
	/* The STP API has no dedicated unregister call: registering a NULL
	 * callback clears the slot (downstream stp_exp.c:173-186 does exactly
	 * that, see also gps/stp_chrdev_gps.c:449).
	 */
	mtk_wcn_stp_register_event_cb(FM_TASK_INDX, NULL);
	cancel_work_sync(&fm->rx_work);
	mtk_wcn_fm_instance = NULL;

	dev_dbg(fm->dev, "FM link released\n");
}
