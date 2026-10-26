// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 firmware bring-up.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/gl_kal.c
 *     (kalFirmwareOpen / kalFirmwareSize / kalFirmwareLoad)
 *   drivers/misc/mediatek/connectivity/wlan/gen2/common/wlan_lib.c
 *     (wlanAdapterStart, wlanImageSectionDownload, wlanConfigWifiFunc)
 *   drivers/misc/mediatek/connectivity/wlan/gen2/nic/nic_pwr_mgt.c
 *     (nicpmSetDriverOwn, nicpmSetAcpiPowerD0)
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/ahb.c
 *     (glGetChipInfo)
 *
 * Copyright (C) 2011 MediaTek Inc.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "wlan_fw.h"
#include "wlan_hif.h"

/* Standard reflected CRC-32, wlanCRC32() wlan_lib.c:3288-3297. */
static u32 mtk_wlan_fw_crc32(const u8 *data, u32 len)
{
	return ~crc32_le(~0, data, len);
}

/*
 * Chip suffix for the firmware file name, glGetChipInfo() ahb.c:534-548.
 */
static void mtk_wlan_fw_name(struct mtk_wlan_hif *hif, char *buf, size_t len)
{
	switch (hif->chip_id) {
	case 0x6571:
	case 0x8127:
	case 0x6752:
	case 0x8163:
	case 0x6735:
	case 0x6580:
	case 0x6755:
		snprintf(buf, len, WLAN_FW_PATH_PREFIX "%04x", hif->chip_id);
		break;
	default:
		snprintf(buf, len, WLAN_FW_PATH_PREFIX "SOC");
		break;
	}
}

/*
 * Hand one init command to the HIF.  @cmd is the command payload, @data an
 * optional trailing payload (firmware chunk).
 */
static int mtk_wlan_fw_xmit(struct mtk_wlan_hif *hif, u8 cid,
			    const void *cmd, u32 cmd_len,
			    const void *data, u32 data_len, u8 *seq)
{
	struct wlan_init_hif_tx_header *hdr;
	u32 payload_len = cmd_len + data_len;
	u32 aligned = ALIGN(sizeof(*hdr) + payload_len, 4);
	u8 *buf;
	int ret;

	buf = kzalloc(aligned, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	hdr = (struct wlan_init_hif_tx_header *)buf;
	hdr->tx_byte_count = cpu_to_le16((u16)aligned);
	hdr->ether_type_offset = 0;
	hdr->cs_flags = 0;

	hif->cmd_seq++;
	hdr->cmd.cid = cid;
	hdr->cmd.seq = hif->cmd_seq;
	*seq = hif->cmd_seq;

	if (cmd_len)
		memcpy(hdr->cmd.buffer, cmd, cmd_len);
	if (data_len)
		memcpy(hdr->cmd.buffer + cmd_len, data, data_len);

	/* Only TC0, i.e. TX data port 0, is used for init commands
	 * (wlan_lib.c:2905-2908, nic_tx.c:2132-2148).
	 */
	ret = mtk_wlan_hif_port_write(hif, MCR_WTDR0, aligned, buf, aligned);
	kfree(buf);
	return ret;
}

/*
 * Wait for INIT_EVENT_ID_CMD_RESULT, wlanImageSectionDownloadStatus()
 * wlan_lib.c:3059-3145, and nicRxWaitResponse() nic_rx.c:3619-3704.
 */
static int mtk_wlan_fw_wait_result(struct mtk_wlan_hif *hif, u8 seq)
{
	u32 max_len = sizeof(struct wlan_init_hif_rx_header) + 4;
	struct wlan_init_hif_rx_header *rx;
	unsigned long deadline;
	u8 *buf;
	u32 pkt_len;
	int ret;

	buf = kzalloc(max_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	deadline = jiffies + msecs_to_jiffies(WLAN_FW_RESPONSE_TIMEOUT_MS);
	for (;;) {
		pkt_len = FIELD_GET(WRPLR_RX0_PACKET_LENGTH,
				    mtk_wlan_hif_readl(hif, MCR_WRPLR));
		if (pkt_len)
			break;
		if (time_after(jiffies, deadline)) {
			dev_err(hif->dev, "no download response for seq %u\n",
				seq);
			ret = -ETIMEDOUT;
			goto out;
		}
		/* kalUdelay(50), nic_rx.c:3655. */
		usleep_range(50, 100);
	}

	if (pkt_len > max_len) {
		dev_err(hif->dev, "download response of %u bytes is too long\n",
			pkt_len);
		ret = -EMSGSIZE;
		goto out;
	}

	ret = mtk_wlan_hif_port_read(hif, MCR_WRDR0, pkt_len, buf, max_len);
	if (ret)
		goto out;

	rx = (struct wlan_init_hif_rx_header *)buf;
	if (rx->eid != WLAN_INIT_EVENT_ID_CMD_RESULT) {
		dev_err(hif->dev, "unexpected event id %u for seq %u\n",
			rx->eid, seq);
		ret = -EIO;
		goto out;
	}
	if (rx->seq != seq) {
		dev_err(hif->dev, "event seq %u does not match command %u\n",
			rx->seq, seq);
		ret = -EIO;
		goto out;
	}

	ret = rx->buffer[0];
	if (ret) {
		dev_err(hif->dev, "download command rejected, status %d\n", ret);
		ret = -EIO;
	}
out:
	kfree(buf);
	return ret;
}

/* Download @len bytes to @dest, wlan_lib.c:2865-2965. */
static int mtk_wlan_fw_download_chunk(struct mtk_wlan_hif *hif, u32 dest,
				      const u8 *data, u32 len)
{
	struct wlan_init_cmd_download_buf dl;
	u8 seq;
	int ret;

	dl.address = cpu_to_le32(dest);
	dl.length = cpu_to_le32(len);
	dl.crc32 = cpu_to_le32(mtk_wlan_fw_crc32(data, len));
	/*
	 * CFG_ENABLE_FW_DOWNLOAD_ACK and CFG_ENABLE_FW_ENCRYPTION are both 1
	 * in this build (config.h:1329-1330), see wlan_lib.c:2914-2927.
	 */
	dl.data_mode = cpu_to_le32(WLAN_DOWNLOAD_BUF_ACK_OPTION |
				   WLAN_DOWNLOAD_BUF_ENCRYPTION_MODE);

	ret = mtk_wlan_fw_xmit(hif, WLAN_INIT_CMD_ID_DOWNLOAD_BUF,
			       &dl, sizeof(dl), data, len, &seq);
	if (ret)
		return ret;

	return mtk_wlan_fw_wait_result(hif, seq);
}

static int mtk_wlan_fw_download_section(struct mtk_wlan_hif *hif, u32 dest,
					const u8 *data, u32 len)
{
	u32 done = 0;
	int ret;

	while (done < len) {
		u32 chunk = min_t(u32, len - done, WLAN_FW_CHUNK_SIZE);

		ret = mtk_wlan_fw_download_chunk(hif, dest + done,
						 data + done, chunk);
		if (ret)
			return ret;
		done += chunk;
	}

	return 0;
}

/* Send INIT_CMD_ID_WIFI_START, wlanConfigWifiFunc() wlan_lib.c:3156-3221. */
static int mtk_wlan_fw_kick(struct mtk_wlan_hif *hif, u32 start_addr)
{
	struct wlan_init_cmd_wifi_start cmd;
	u8 seq;

	/* CFG_OVERRIDE_FW_START_ADDRESS is 1 for MT6628, config.h:1353. */
	cmd.override = cpu_to_le32(1);
	cmd.address = cpu_to_le32(start_addr);

	return mtk_wlan_fw_xmit(hif, WLAN_INIT_CMD_ID_WIFI_START,
				&cmd, sizeof(cmd), NULL, 0, &seq);
}

/*
 * Download the image.  A file with a valid MTKW header is downloaded
 * section by section (CFG_ENABLE_FW_DIVIDED_DOWNLOAD, config.h:1334);
 * otherwise it is a flat image loaded at CFG_FW_LOAD_ADDRESS.
 */
static int mtk_wlan_fw_download(struct mtk_wlan_hif *hif,
				const struct firmware *fw)
{
	const struct wlan_fw_divided_header *head;
	u32 num, i, start_addr;
	int ret;

	head = (const struct wlan_fw_divided_header *)fw->data;
	if (fw->size >= sizeof(*head) &&
	    le32_to_cpu(head->signature) == WLAN_FW_DIVIDED_SIGNATURE &&
	    le32_to_cpu(head->crc) ==
		mtk_wlan_fw_crc32(fw->data + WLAN_FW_CRC_OFFSET,
				  fw->size - WLAN_FW_CRC_OFFSET)) {
		num = le32_to_cpu(head->num_of_entries);
		if (!num ||
		    sizeof(*head) + num * sizeof(struct wlan_fw_section) >
			fw->size) {
			dev_err(hif->dev, "invalid section table (%u entries)\n",
				num);
			return -EINVAL;
		}

		for (i = 0; i < num; i++) {
			u32 off = le32_to_cpu(head->sections[i].offset);
			u32 len = le32_to_cpu(head->sections[i].length);
			u32 dest = le32_to_cpu(head->sections[i].dest_addr);

			if (!len || (u64)off + len > fw->size) {
				dev_err(hif->dev, "invalid section %u\n", i);
				return -EINVAL;
			}

			ret = mtk_wlan_fw_download_section(hif, dest,
							   fw->data + off, len);
			if (ret)
				return ret;
		}

		/* CFG_START_ADDRESS_IS_1ST_SECTION_ADDR, config.h:1355. */
		start_addr = le32_to_cpu(head->sections[0].dest_addr);
		dev_info(hif->dev, "divided image: %u sections, start %#x\n",
			 num, start_addr);
	} else {
		dev_warn(hif->dev, "no MTKW header, flat load at %#x\n",
			 WLAN_FW_LOAD_ADDRESS);
		ret = mtk_wlan_fw_download_section(hif, WLAN_FW_LOAD_ADDRESS,
						   fw->data, fw->size);
		if (ret)
			return ret;
		start_addr = WLAN_FW_START_ADDRESS;
	}

	return mtk_wlan_fw_kick(hif, start_addr);
}

/* Wait for WCIR.WLAN_READY, wlan_lib.c:1428-1452. */
static int mtk_wlan_fw_wait_ready(struct mtk_wlan_hif *hif)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < WLAN_FW_READY_POLL_COUNT; i++) {
		val = mtk_wlan_hif_readl(hif, MCR_WCIR);
		if (val & WCIR_WLAN_READY) {
			dev_info(hif->dev, "firmware ready after %u ms\n",
				 i * WLAN_FW_READY_POLL_MS);
			return 0;
		}
		msleep(WLAN_FW_READY_POLL_MS);
	}

	val = mtk_wlan_hif_readl(hif, MCR_D2HRM0R);
	dev_err(hif->dev, "firmware ready timeout, mailbox id %u\n",
		val & 0xffff);
	return -ETIMEDOUT;
}

/*
 * Reset the interrupt status left over by the download and restore the
 * hardware clear mode, nic_pwr_mgt.c:598-602 and wlan_lib.c:1649-1653.
 */
static void mtk_wlan_fw_clear_state(struct mtk_wlan_hif *hif)
{
	u32 hisr = 0, val;

	mtk_wlan_hif_port_read(hif, MCR_WHISR, sizeof(hisr), (u8 *)&hisr,
			       sizeof(hisr));
	if (hisr & WHISR_TX_DONE_INT)
		dev_dbg(hif->dev, "stale TX status: WTSR0=0x%08x WTSR1=0x%08x\n",
			mtk_wlan_hif_readl(hif, MCR_WTSR0),
			mtk_wlan_hif_readl(hif, MCR_WTSR1));

	val = mtk_wlan_hif_readl(hif, MCR_WHCR);
	val &= ~(WHCR_W_INT_CLR_CTRL | WHCR_W_MAILBOX_RD_CLR_EN);
	mtk_wlan_hif_writel(hif, MCR_WHCR, val);
}

/**
 * mtk_wlan_fw_bringup() - power the WLAN firmware up
 * @hif: host interface instance
 *
 * Called from the WMT WIFI function on callback, i.e. after the connectivity
 * subsystem has been powered and its MCU reset released by
 * mtk_wcn_consys_hw_pwr_on() (mtk_wcn_consys_hw.c:498-501).  The caller
 * holds &hif->state_lock.
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wlan_fw_bringup(struct mtk_wlan_hif *hif)
{
	const struct firmware *fw = NULL;
	char name[32];
	int ret;

	lockdep_assert_held(&hif->state_lock);

	ret = mtk_wlan_hif_start(hif);
	if (ret)
		return ret;

	mtk_wlan_hif_hw_init(hif);

	mtk_wlan_fw_name(hif, name, sizeof(name));
	ret = request_firmware(&fw, name, hif->dev);
	if (ret) {
		dev_err(hif->dev, "firmware %s not available: %d\n", name, ret);
		goto err;
	}
	if (!fw->size) {
		dev_err(hif->dev, "firmware %s is empty\n", name);
		ret = -EINVAL;
		goto out_free;
	}

	ret = mtk_wlan_hif_claim_driver_own(hif);
	if (ret)
		goto out_free;

	ret = mtk_wlan_fw_download(hif, fw);
	if (ret)
		goto out_free;

	ret = mtk_wlan_fw_wait_ready(hif);
	if (ret)
		goto out_free;

	mtk_wlan_fw_clear_state(hif);

	WRITE_ONCE(hif->up, true);
	mtk_wlan_hif_set_int_enable(hif, true);
	dev_info(hif->dev, "%s running, chip id 0x%04x\n", name, hif->chip_id);

out_free:
	release_firmware(fw);
err:
	if (ret) {
		mtk_wlan_hif_set_int_enable(hif, false);
		dev_err(hif->dev, "firmware bring-up failed: %d\n", ret);
	}
	return ret;
}

MODULE_FIRMWARE(WLAN_FW_BASENAME "_6735");
