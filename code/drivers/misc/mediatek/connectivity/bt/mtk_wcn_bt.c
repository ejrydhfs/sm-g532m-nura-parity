// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN Bluetooth HCI driver (HCI over the STP Bluetooth channel).
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pub/stp_chrdev_bt.c
 *
 * Downstream exposes the raw HCI byte stream to userspace through /dev/stpbt.
 * This driver keeps the same WMT/STP lifecycle but presents the stream to the
 * kernel Bluetooth stack as an HCI device instead.
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "mtk_wcn_stp.h"
#include "mtk_wcn_wmt.h"

/*
 * STP transmit retry policy, downstream stp_btif.c:64-65
 * (STP_BTIF_TX_RTY_LMT / STP_BTIF_TX_RTY_DLY).
 */
#define BT_STP_TX_RETRY		10
#define BT_STP_TX_DELAY_MS	5

/* Downstream BUFFER_SIZE used for a single STP read, stp_chrdev_bt.c:87 */
#define BT_STP_RX_CHUNK		2048

/* Largest H4 header: command/ACL/SCO/event/ISO */
#define BT_H4_HDR_MAX		4

/*
 * Whole-chip reset stages, downstream wmt_exp.h:186-189.
 * The frozen WMT message callback only reports WMTMSG_RESET, so the stage is
 * carried in the payload.
 */
enum bt_wmt_reset_stage {
	BT_WMT_RST_START = 0,
	BT_WMT_RST_END = 1,
	BT_WMT_RST_END_FAIL = 2,
};

/*
 * HCI Hardware Error event sent after a whole-chip reset so that the stack
 * restarts Bluetooth, downstream stp_chrdev_bt.c:105.
 */
static const u8 bt_hw_error_evt[] = { 0x10, 0x01, 0x00 };

struct mtk_bt {
	struct hci_dev *hdev;
	struct work_struct rx_work;
	struct work_struct rst_work;
	/* protects the STP receive drain and the H4 parser state below */
	struct mutex rx_lock;

	u8 *rx_buf;
	struct sk_buff *rx_skb;
	u8 rx_type;
	u8 rx_hdr[BT_H4_HDR_MAX];
	u8 rx_hdr_len;
	u8 rx_hdr_got;
	u32 rx_payload_len;
	u32 rx_payload_got;

	bool open;
	bool rst_in_progress;
	u32 rst_stage;
};

static struct mtk_bt *g_bt;

static void bt_rx_reset(struct mtk_bt *bt)
{
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->rx_type = 0;
	bt->rx_hdr_len = 0;
	bt->rx_hdr_got = 0;
	bt->rx_payload_len = 0;
	bt->rx_payload_got = 0;
}

static int bt_h4_hdr_len(u8 type)
{
	switch (type) {
	case HCI_COMMAND_PKT:
		return HCI_COMMAND_HDR_SIZE;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE;
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE;
	case HCI_ISODATA_PKT:
		return HCI_ISO_HDR_SIZE;
	default:
		return -EINVAL;
	}
}

static u32 bt_h4_payload_len(u8 type, const u8 *hdr)
{
	switch (type) {
	case HCI_COMMAND_PKT:
		return hdr[2];
	case HCI_ACLDATA_PKT:
		return get_unaligned_le16(hdr + 2);
	case HCI_SCODATA_PKT:
		return hdr[2];
	case HCI_EVENT_PKT:
		return hdr[1];
	case HCI_ISODATA_PKT:
		return get_unaligned_le16(hdr + 2);
	default:
		return 0;
	}
}

static void bt_rx_deliver(struct mtk_bt *bt)
{
	struct sk_buff *skb = bt->rx_skb;
	unsigned int len = skb->len;

	bt->rx_skb = NULL;
	bt->rx_type = 0;
	if (hci_recv_frame(bt->hdev, skb))
		bt->hdev->stat.err_rx++;
	else
		bt->hdev->stat.byte_rx += len;
}

/*
 * H4 byte-stream demultiplexer.  The STP Bluetooth channel carries the same
 * type-prefixed HCI frames the downstream character device forwarded to
 * userspace verbatim (stp_chrdev_bt.c BT_read()).
 */
static void bt_rx_stream(struct mtk_bt *bt, const u8 *data, u32 len)
{
	while (len) {
		u32 n;

		if (!bt->rx_type) {
			int hdr_len = bt_h4_hdr_len(*data);

			if (hdr_len < 0) {
				bt->hdev->stat.err_rx++;
				data++;
				len--;
				continue;
			}
			bt->rx_type = *data++;
			bt->rx_hdr_len = hdr_len;
			bt->rx_hdr_got = 0;
			len--;
			continue;
		}

		if (bt->rx_hdr_got < bt->rx_hdr_len) {
			n = min(len, (u32)(bt->rx_hdr_len - bt->rx_hdr_got));
			memcpy(bt->rx_hdr + bt->rx_hdr_got, data, n);
			bt->rx_hdr_got += n;
			data += n;
			len -= n;
			if (bt->rx_hdr_got < bt->rx_hdr_len)
				break;

			bt->rx_payload_len = bt_h4_payload_len(bt->rx_type,
							       bt->rx_hdr);
			if (bt->rx_payload_len > HCI_MAX_FRAME_SIZE) {
				bt->hdev->stat.err_rx++;
				bt->rx_type = 0;
				continue;
			}

			bt->rx_skb = bt_skb_alloc(bt->rx_hdr_len +
						  bt->rx_payload_len,
						  GFP_KERNEL);
			if (!bt->rx_skb) {
				bt->rx_type = 0;
				return;
			}
			hci_skb_pkt_type(bt->rx_skb) = bt->rx_type;
			skb_put_data(bt->rx_skb, bt->rx_hdr, bt->rx_hdr_len);
			bt->rx_payload_got = 0;

			if (!bt->rx_payload_len)
				bt_rx_deliver(bt);
			continue;
		}

		n = min(len, bt->rx_payload_len - bt->rx_payload_got);
		skb_put_data(bt->rx_skb, data, n);
		bt->rx_payload_got += n;
		data += n;
		len -= n;

		if (bt->rx_payload_got == bt->rx_payload_len)
			bt_rx_deliver(bt);
	}
}

static void bt_rx_work(struct work_struct *work)
{
	struct mtk_bt *bt = container_of(work, struct mtk_bt, rx_work);
	int ret;

	mutex_lock(&bt->rx_lock);
	if (!bt->open) {
		mutex_unlock(&bt->rx_lock);
		return;
	}

	for (;;) {
		ret = mtk_wcn_stp_receive_data(bt->rx_buf, BT_STP_RX_CHUNK,
						BT_TASK_INDX);
		if (ret <= 0)
			break;
		bt_rx_stream(bt, bt->rx_buf, ret);
		if (ret < BT_STP_RX_CHUNK)
			break;
	}
	mutex_unlock(&bt->rx_lock);
}

/* Downstream BT_event_cb(), stp_chrdev_bt.c:149-173 */
static void bt_stp_event_cb(void)
{
	if (g_bt)
		schedule_work(&g_bt->rx_work);
}

static void bt_hw_error_event(struct mtk_bt *bt)
{
	struct sk_buff *skb;

	if (!bt->open)
		return;

	skb = bt_skb_alloc(sizeof(bt_hw_error_evt), GFP_KERNEL);
	if (!skb)
		return;

	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
	skb_put_data(skb, bt_hw_error_evt, sizeof(bt_hw_error_evt));

	if (hci_recv_frame(bt->hdev, skb))
		bt->hdev->stat.err_rx++;
}

/*
 * The reset notification is only recorded here so that the callback never
 * sleeps; the parser teardown and the HCI event are done in bt_rst_work().
 */
static void bt_wmt_msg_cb(enum mtk_wcn_wmt_drv src, enum mtk_wcn_wmt_drv dst,
			  enum mtk_wcn_wmt_msg msg, void *data, u32 len)
{
	struct mtk_bt *bt = g_bt;
	u32 stage = 0;

	if (!bt)
		return;
	if (src != WMTDRV_WMT || dst != WMTDRV_BT || msg != WMTMSG_RESET)
		return;

	if (len >= sizeof(stage))
		memcpy(&stage, data, sizeof(stage));
	else if (len)
		stage = *(const u8 *)data;

	switch (stage) {
	case BT_WMT_RST_START:
		WRITE_ONCE(bt->rst_in_progress, true);
		break;
	case BT_WMT_RST_END:
	case BT_WMT_RST_END_FAIL:
		WRITE_ONCE(bt->rst_in_progress, false);
		break;
	default:
		bt_dev_warn(bt->hdev, "unknown reset stage %u\n", stage);
		return;
	}

	bt->rst_stage = stage;
	schedule_work(&bt->rst_work);
}

/*
 * Whole-chip reset handling, downstream bt_cdev_rst_cb() in
 * stp_chrdev_bt.c:111-147.  Transmissions are refused while the connectivity
 * SoC is being reset; once it is back the stack is told through an HCI
 * Hardware Error event so that it re-initialises the controller.
 */
static void bt_rst_work(struct work_struct *work)
{
	struct mtk_bt *bt = container_of(work, struct mtk_bt, rst_work);
	u32 stage = bt->rst_stage;

	mutex_lock(&bt->rx_lock);
	bt_rx_reset(bt);
	mutex_unlock(&bt->rx_lock);

	if (stage == BT_WMT_RST_START) {
		bt_dev_info(bt->hdev, "whole chip reset start\n");
		return;
	}

	bt_dev_info(bt->hdev, "whole chip reset %s\n",
		    stage == BT_WMT_RST_END ? "end" : "failed");
	bt_hw_error_event(bt);
}

/* Downstream BT_open(), stp_chrdev_bt.c:401-437 */
static int bt_open(struct hci_dev *hdev)
{
	struct mtk_bt *bt = hci_get_drvdata(hdev);
	int ret;

	if (bt->open)
		return -EALREADY;

	if (!mtk_wcn_wmt_func_on(WMTDRV_BT)) {
		bt_dev_err(hdev, "WMT BT power on failed\n");
		return -EIO;
	}

	if (!mtk_wcn_stp_is_ready()) {
		bt_dev_err(hdev, "STP is not ready\n");
		ret = -EIO;
		goto err_func_off;
	}

	mutex_lock(&bt->rx_lock);
	bt_rx_reset(bt);
	bt->rst_in_progress = false;
	bt->open = true;
	mutex_unlock(&bt->rx_lock);

	ret = mtk_wcn_stp_register_event_cb(BT_TASK_INDX, bt_stp_event_cb);
	if (ret) {
		bt_dev_err(hdev, "failed to register STP BT event callback: %d\n",
			   ret);
		goto err_close;
	}

	ret = mtk_wcn_wmt_msgcb_reg(WMTDRV_BT, bt_wmt_msg_cb);
	if (ret) {
		bt_dev_err(hdev, "failed to register WMT BT callback: %d\n", ret);
		mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
		goto err_close;
	}

	bt_dev_info(hdev, "Bluetooth enabled\n");
	return 0;

err_close:
	mutex_lock(&bt->rx_lock);
	bt->open = false;
	bt_rx_reset(bt);
	mutex_unlock(&bt->rx_lock);
err_func_off:
	mtk_wcn_wmt_func_off(WMTDRV_BT);
	return ret;
}

/* Downstream BT_close(), stp_chrdev_bt.c:439-456 */
static int bt_close(struct hci_dev *hdev)
{
	struct mtk_bt *bt = hci_get_drvdata(hdev);

	mtk_wcn_wmt_msgcb_unreg(WMTDRV_BT);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);

	cancel_work_sync(&bt->rx_work);
	cancel_work_sync(&bt->rst_work);

	mutex_lock(&bt->rx_lock);
	bt->open = false;
	bt_rx_reset(bt);
	mutex_unlock(&bt->rx_lock);

	if (!mtk_wcn_wmt_func_off(WMTDRV_BT)) {
		bt_dev_err(hdev, "WMT BT power off failed\n");
		return -EIO;
	}

	bt_dev_info(hdev, "Bluetooth disabled\n");
	return 0;
}

/*
 * Downstream BT_write(), stp_chrdev_bt.c:205-250: a short write is retried
 * with a 5 ms delay up to STP_BTIF_TX_RTY_LMT times.
 */
static int bt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mtk_bt *bt = hci_get_drvdata(hdev);
	unsigned int len;
	unsigned int sent = 0;
	int ret = -EIO;
	int retry;
	int written;

	if (!bt->open || READ_ONCE(bt->rst_in_progress))
		return -ENODEV;

	/* prepend the H4 packet indicator expected by the controller */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	len = skb->len;

	for (retry = 0; retry < BT_STP_TX_RETRY; retry++) {
		written = mtk_wcn_stp_send_data(skb->data + sent, len - sent,
						BT_TASK_INDX);
		if (written < 0) {
			ret = written;
			break;
		}
		sent += written;
		if (sent == len) {
			ret = 0;
			break;
		}
		msleep(BT_STP_TX_DELAY_MS);
	}

	if (ret) {
		/* the HCI core frees the skb when send() reports an error */
		hdev->stat.err_tx++;
		bt_dev_dbg(hdev, "HCI TX failed, %u of %u bytes\n", sent, len);
		return ret;
	}

	hdev->stat.byte_tx += len;
	kfree_skb(skb);
	return 0;
}

static int __init mtk_wcn_bt_init(void)
{
	struct mtk_bt *bt;
	struct hci_dev *hdev;
	int ret;

	bt = kzalloc_obj(*bt);
	if (!bt)
		return -ENOMEM;

	mutex_init(&bt->rx_lock);
	INIT_WORK(&bt->rx_work, bt_rx_work);
	INIT_WORK(&bt->rst_work, bt_rst_work);

	bt->rx_buf = kzalloc(BT_STP_RX_CHUNK, GFP_KERNEL);
	if (!bt->rx_buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto err_free;
	}

	hdev->bus = HCI_VIRTUAL;
	hdev->open = bt_open;
	hdev->close = bt_close;
	hdev->send = bt_send_frame;

	hci_set_drvdata(hdev, bt);
	bt->hdev = hdev;

	ret = hci_register_dev(hdev);
	if (ret < 0) {
		hci_free_dev(hdev);
		goto err_free;
	}

	g_bt = bt;
	return 0;

err_free:
	kfree(bt);
	return ret;
}

static void __exit mtk_wcn_bt_exit(void)
{
	struct mtk_bt *bt = g_bt;

	if (!bt)
		return;

	g_bt = NULL;
	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
	cancel_work_sync(&bt->rst_work);
	cancel_work_sync(&bt->rx_work);
	kfree(bt->rx_buf);
	kfree(bt);
}

module_init(mtk_wcn_bt_init);
module_exit(mtk_wcn_bt_exit);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek WCN Bluetooth HCI driver");
MODULE_LICENSE("GPL");
