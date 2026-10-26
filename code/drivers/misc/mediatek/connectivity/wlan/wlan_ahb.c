// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WLAN gen2 AHB host interface driver.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/ahb.c
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/mt6735/ahb_pdma.c
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/include/hif_pdma.h
 *   drivers/misc/mediatek/connectivity/wlan/gen2/include/nic/mtreg.h
 *
 * Copyright (C) 2013 MediaTek Inc.
 *
 * The block implements the register/PIO/PDMA half of the gen2 HIF only.
 * Received buffers are handed to the 802.11 MAC layer in wlan_mac.c
 * unchanged, exactly like HifAhbISR()/wlanIST() pass them to
 * nicRxProcessRFBs() in the downstream driver (ahb.c:1215-1249,
 * wlan_lib.c:1800-1850, nic_rx.c:2734-2823).
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>

#include "mtk_wcn_wmt.h"
#include "wlan_hif.h"

/* HIF register window is 0x5c bytes long (hif.h:217-218). */
#define MTK_WLAN_HIF_MIN_SIZE		(MCR_HSTCR + 4)

/* PDMA completion poll budget (ahb.c:953,1128). */
#define MTK_WLAN_HIF_DMA_TIMEOUT	(5 * HZ)
#define MTK_WLAN_HIF_DMA_POLL_MAX	100000

/* IST loop count, CFG_IST_LOOP_COUNT config.h:1296. */
#define MTK_WLAN_HIF_RX_DRAIN_MAX	8

static int mtk_wlan_ahb_probe(struct platform_device *pdev);
static void mtk_wlan_ahb_remove(struct platform_device *pdev);

static const struct of_device_id mtk_wlan_of_match[] = {
	{ .compatible = "mediatek,wifi" },
	{ .compatible = "mediatek,WIFI" },	/* ROM carries this spelling */
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_wlan_of_match);

/*
 * HIF register access helpers.
 */

/* PIO/PDMA burst and transaction setup, ahb.c:1451-1468. */
static void mtk_wlan_hif_dma_enhance(struct mtk_wlan_hif *hif, u32 burst,
				     u32 target, u32 trans)
{
	u32 val;

	val = FIELD_PREP(HSTCR_AFF_BURST_LEN, burst) |
	      FIELD_PREP(HSTCR_TRANS_TARGET, target) |
	      (((trans & 0x3) == 0) ? (trans & HSTCR_HSIF_TRANS_CNT) :
	       ((trans + 4) & HSTCR_HSIF_TRANS_CNT));
	mtk_wlan_hif_writel(hif, MCR_HSTCR, val);
}

void mtk_wlan_hif_set_int_enable(struct mtk_wlan_hif *hif, bool enable)
{
	mtk_wlan_hif_writel(hif, MCR_WHLPCR,
			    enable ? WHLPCR_INT_EN_SET : WHLPCR_INT_EN_CLR);
	hif->int_enabled = enable;
}

/*
 * AP-DMA (PDMA) primitives, ahb_pdma.c:225-592.
 */

static u32 mtk_wlan_pdma_readl(struct mtk_wlan_hif *hif, u32 off)
{
	return readl(hif->dma_base + off);
}

static void mtk_wlan_pdma_writel(struct mtk_wlan_hif *hif, u32 off, u32 val)
{
	writel(val, hif->dma_base + off);
}

/* ahb_pdma.c:225-258. */
static void mtk_wlan_pdma_config(struct mtk_wlan_hif *hif, bool tx, u32 count,
				 phys_addr_t src, phys_addr_t dst)
{
	u32 val;

	/* Burst is fixed to 4/4 and address fix-up is disabled (ahb_pdma.c:231-233). */
	val = mtk_wlan_pdma_readl(hif, AP_DMA_HIF_CON);
	val &= ~(ADH_CR_BURST_LEN | ADH_CR_FIX_EN | ADH_CR_DIR);
	val |= FIELD_PREP(ADH_CR_BURST_LEN, HIF_PDMA_BURST_4_4) |
	       (tx ? 0 : ADH_CR_DIR);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_CON, val);

	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_SRC_ADDR, lower_32_bits(src));
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_DST_ADDR, lower_32_bits(dst));
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_LEN, count & ADH_CR_LEN);
}

/* ahb_pdma.c:269-289. */
static void mtk_wlan_pdma_start(struct mtk_wlan_hif *hif)
{
	u32 val;

	val = mtk_wlan_pdma_readl(hif, AP_DMA_HIF_INT_EN);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_INT_EN, val | ADH_CR_INTEN_FLAG_0);

	val = mtk_wlan_pdma_readl(hif, AP_DMA_HIF_EN);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_EN, val | ADH_CR_EN);
}

/* ahb_pdma.c:300-324. */
static void mtk_wlan_pdma_stop(struct mtk_wlan_hif *hif)
{
	u32 val;

	val = mtk_wlan_pdma_readl(hif, AP_DMA_HIF_INT_EN);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_INT_EN, val & ~ADH_CR_INTEN_FLAG_0);
}

/* ahb_pdma.c:354-362. */
static bool mtk_wlan_pdma_poll_intr(struct mtk_wlan_hif *hif)
{
	return mtk_wlan_pdma_readl(hif, AP_DMA_HIF_INT_FLAG) & ADH_CR_FLAG_0;
}

/* ahb_pdma.c:373-382. */
static void mtk_wlan_pdma_ack_intr(struct mtk_wlan_hif *hif)
{
	u32 val;

	val = mtk_wlan_pdma_readl(hif, AP_DMA_HIF_INT_FLAG);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_INT_FLAG, val & ~ADH_CR_FLAG_0);
}

/* ahb_pdma.c:335-343. */
static bool mtk_wlan_pdma_poll_start(struct mtk_wlan_hif *hif)
{
	return mtk_wlan_pdma_readl(hif, AP_DMA_HIF_EN) & ADH_CR_EN;
}

/* ahb_pdma.c:568-592. */
static void mtk_wlan_pdma_reset(struct mtk_wlan_hif *hif)
{
	unsigned int i;

	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_RST, ADH_CR_WARM_RST);
	for (i = 0; i < 10000; i++) {
		if (!mtk_wlan_pdma_poll_start(hif))
			return;
	}

	dev_info(hif->dev, "PDMA warm reset failed, using hard reset\n");
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_RST, ADH_CR_HARD_RST);
	mdelay(1);
	mtk_wlan_pdma_writel(hif, AP_DMA_HIF_RST, 0);
}

static int mtk_wlan_clk_enable(struct mtk_wlan_hif *hif)
{
	int ret;

	if (!hif->clk)
		return 0;

	ret = clk_prepare_enable(hif->clk);
	if (ret)
		dev_err(hif->dev, "cannot enable wifi-dma clock: %d\n", ret);
	return ret;
}

static void mtk_wlan_clk_disable(struct mtk_wlan_hif *hif)
{
	if (hif->clk)
		clk_disable_unprepare(hif->clk);
}

static void mtk_wlan_pdma_dump(struct mtk_wlan_hif *hif)
{
	dev_err(hif->dev,
		"PDMA timeout: WCIR=0x%08x WHLPCR=0x%08x CON=0x%08x EN=0x%08x INT_FLAG=0x%08x\n",
		mtk_wlan_hif_readl(hif, MCR_WCIR),
		mtk_wlan_hif_readl(hif, MCR_WHLPCR),
		mtk_wlan_pdma_readl(hif, AP_DMA_HIF_CON),
		mtk_wlan_pdma_readl(hif, AP_DMA_HIF_EN),
		mtk_wlan_pdma_readl(hif, AP_DMA_HIF_INT_FLAG));
}

/*
 * Run one PDMA transaction over @size bytes.  @src/@dst are physical
 * addresses; the HIF side is always one of the data ports.
 */
static int mtk_wlan_pdma_transfer(struct mtk_wlan_hif *hif, bool tx, u8 *buf,
				  u32 size, phys_addr_t port_phys)
{
	enum dma_data_direction dir = tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	unsigned long timeout;
	dma_addr_t phys;
	u32 i;
	int ret = 0;

	phys = dma_map_single(hif->dev, buf, size, dir);
	if (dma_mapping_error(hif->dev, phys)) {
		dev_err(hif->dev, "cannot map %u bytes for PDMA\n", size);
		return -EIO;
	}

	if (tx)
		mtk_wlan_pdma_config(hif, true, size, phys, port_phys);
	else
		mtk_wlan_pdma_config(hif, false, size, port_phys, phys);

	mtk_wlan_pdma_start(hif);

	timeout = jiffies + MTK_WLAN_HIF_DMA_TIMEOUT;
	while (!mtk_wlan_pdma_poll_intr(hif)) {
		if (time_after(jiffies, timeout)) {
			mtk_wlan_pdma_dump(hif);
			ret = -ETIMEDOUT;
			goto unmap;
		}
		udelay(1);
	}

	mtk_wlan_pdma_ack_intr(hif);
	mtk_wlan_pdma_stop(hif);

	/* Wait for the channel to release EN before the next user (ahb.c:1042-1052). */
	for (i = 0; i < MTK_WLAN_HIF_DMA_POLL_MAX; i++) {
		if (!mtk_wlan_pdma_poll_start(hif))
			break;
		cpu_relax();
	}
	if (i == MTK_WLAN_HIF_DMA_POLL_MAX)
		dev_warn(hif->dev, "PDMA still enabled after completion\n");

unmap:
	dma_unmap_single(hif->dev, phys, size, dir);
	return ret;
}

/*
 * Read @size bytes from a HIF port.  @buf must have three bytes of slack
 * because the hardware is accessed in 32-bit words (ahb.c:1005-1012).
 */
int mtk_wlan_hif_port_read(struct mtk_wlan_hif *hif, u16 port, u32 size,
			   u8 *buf, u32 max)
{
	u32 whlpcr, words, i;
	int ret = 0;

	if (!size)
		return 0;
	if (size > max) {
		dev_err(hif->dev, "port %#x read of %u exceeds buffer %u\n",
			port, size, max);
		return -EINVAL;
	}

	/* Burst length must match the one used by PDMA (ahb.c:877-883). */
	if (port == MCR_WRDR0)
		mtk_wlan_hif_dma_enhance(hif, HIF_BURST_4DW, HIF_TARGET_RXD0, size);
	else if (port == MCR_WRDR1)
		mtk_wlan_hif_dma_enhance(hif, HIF_BURST_4DW, HIF_TARGET_RXD1, size);
	else if (port == MCR_WHISR)
		mtk_wlan_hif_dma_enhance(hif, HIF_BURST_4DW, HIF_TARGET_WHISR, size);

	/* Keep the HIF from raising an interrupt while its data port is busy. */
	whlpcr = mtk_wlan_hif_readl(hif, MCR_WHLPCR);
	if (whlpcr & WHLPCR_INT_EN_SET)
		mtk_wlan_hif_writel(hif, MCR_WHLPCR, WHLPCR_INT_EN_CLR);

	if (hif->dma_enabled && (port == MCR_WRDR0 || port == MCR_WRDR1)) {
		mutex_lock(&hif->io_lock);
		ret = mtk_wlan_clk_enable(hif);
		if (!ret) {
			ret = mtk_wlan_pdma_transfer(hif, false, buf, size,
						     hif->hif_phys + port);
			mtk_wlan_clk_disable(hif);
		}
		mutex_unlock(&hif->io_lock);
	} else {
		words = DIV_ROUND_UP(size, 4);
		for (i = 0; i < words; i++)
			((u32 *)buf)[i] =
				mtk_wlan_hif_readl(hif, port);
	}

	if (whlpcr & WHLPCR_INT_EN_SET)
		mtk_wlan_hif_writel(hif, MCR_WHLPCR, WHLPCR_INT_EN_SET);

	return ret;
}

/*
 * Write @size bytes to a HIF port.  @buf must be padded with zeroes to a
 * 4-byte boundary (ahb.c:1177-1186).
 */
int mtk_wlan_hif_port_write(struct mtk_wlan_hif *hif, u16 port, u32 size,
			    const u8 *buf, u32 max)
{
	u32 whlpcr, words, i;
	int ret = 0;

	if (!size)
		return 0;
	if (size > max) {
		dev_err(hif->dev, "port %#x write of %u exceeds buffer %u\n",
			port, size, max);
		return -EINVAL;
	}

	atomic_inc(&hif->tx_count);

	if (port == MCR_WTDR0)
		mtk_wlan_hif_dma_enhance(hif, HIF_BURST_4DW, HIF_TARGET_TXD0, size);
	else if (port == MCR_WTDR1)
		mtk_wlan_hif_dma_enhance(hif, HIF_BURST_4DW, HIF_TARGET_TXD1, size);

	whlpcr = mtk_wlan_hif_readl(hif, MCR_WHLPCR);
	if (whlpcr & WHLPCR_INT_EN_SET)
		mtk_wlan_hif_writel(hif, MCR_WHLPCR, WHLPCR_INT_EN_CLR);

	if (hif->dma_enabled && (port == MCR_WTDR0 || port == MCR_WTDR1)) {
		mutex_lock(&hif->io_lock);
		ret = mtk_wlan_clk_enable(hif);
		if (!ret) {
			ret = mtk_wlan_pdma_transfer(hif, true, (u8 *)buf, size,
						     hif->hif_phys + port);
			mtk_wlan_clk_disable(hif);
		}
		mutex_unlock(&hif->io_lock);
	} else {
		words = size >> 2;
		if (size & 0x3)
			words++;
		for (i = 0; i < words; i++)
			mtk_wlan_hif_writel(hif, port, ((const u32 *)buf)[i]);
	}

	if (whlpcr & WHLPCR_INT_EN_SET)
		mtk_wlan_hif_writel(hif, MCR_WHLPCR, WHLPCR_INT_EN_SET);

	return ret;
}

/*
 * Claim HIF ownership from the firmware, nic_pwr_mgt.c:311-406.
 * WHLPCR bit 8 reads back set once the host owns the interface.
 */
int mtk_wlan_hif_claim_driver_own(struct mtk_wlan_hif *hif)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(WLAN_FW_OWN_TOTAL_MS);
	u32 i = 0, val;

	for (;;) {
		val = mtk_wlan_hif_readl(hif, MCR_WHLPCR);
		if (val & WHLPCR_FW_OWN_REQ_SET)
			return 0;

		if (time_after(jiffies, deadline)) {
			dev_err(hif->dev,
				"driver own failed: WHLPCR=0x%08x D2HRM2R=0x%08x\n",
				val, mtk_wlan_hif_readl(hif, MCR_D2HRM2R));
			return -ETIMEDOUT;
		}

		/* Keep asking the firmware to hand the interface over. */
		if ((i & (WLAN_FW_OWN_CLR_ITERATION - 1)) == 0)
			mtk_wlan_hif_writel(hif, MCR_WHLPCR,
					    WHLPCR_FW_OWN_REQ_CLR);

		msleep(WLAN_FW_OWN_LOOP_MS);
		i++;
	}
}

/*
 * Chip identification and HIF interrupt setup, ahb.c:455-458 and
 * nic.c:1172-1258.  Only reachable while the connectivity subsystem is
 * powered, i.e. from the WMT WIFI function callback.
 */
void mtk_wlan_hif_hw_init(struct mtk_wlan_hif *hif)
{
	u32 val;

	val = mtk_wlan_hif_readl(hif, MCR_WCIR);
	hif->chip_id = FIELD_GET(WCIR_CHIP_ID, val);
	switch (hif->chip_id) {
	case 0x0321:
	case 0x0335:
	case 0x0337:
		hif->chip_id = 0x6735;	/* Denali chip id transition */
		break;
	case 0x0326:
		hif->chip_id = 0x6755;
		break;
	default:
		break;
	}

	dev_info(hif->dev, "HIF chip id 0x%04x revision %u\n",
		 hif->chip_id, (u32)FIELD_GET(WCIR_REVISION_ID, val));

	/* Firmware download runs with interrupts masked (wlan_lib.c:1306-1307). */
	mtk_wlan_hif_set_int_enable(hif, false);

	/* nicInitializeAdapter(): WHIER default mask (nic.c:1251). */
	mtk_wlan_hif_writel(hif, MCR_WHIER, WHIER_DEFAULT);
}

/*
 * Read one frame from a HIF data port.  The port carries ALIGN_4(packet
 * length) bytes plus one hardware appended DW with the checksum status
 * (nic_rx.c:2887-2890), so the read length is not the value in WRPLR.
 */
static void mtk_wlan_hif_rx_drain(struct mtk_wlan_hif *hif, u32 port)
{
	unsigned int i;

	for (i = 0; i < MTK_WLAN_HIF_RX_DRAIN_MAX; i++) {
		u32 len, read_len, val;

		val = mtk_wlan_hif_readl(hif, MCR_WRPLR);
		if (port == MCR_WRDR1)
			len = FIELD_GET(WRPLR_RX1_PACKET_LENGTH, val);
		else
			len = FIELD_GET(WRPLR_RX0_PACKET_LENGTH, val);
		if (!len)
			break;

		read_len = ALIGN(len, 4) + MTK_WLAN_HIF_HW_APPENDED_LEN;
		if (len > MTK_WLAN_RX_MAX_PKT_SIZE ||
		    read_len > MTK_WLAN_HIF_RX_BUF_SIZE) {
			/*
			 * A length beyond the documented maximum means the
			 * port is out of sync; there is no way to resynchronise
			 * a PIO/PDMA FIFO, so stop draining this port.
			 */
			dev_warn(hif->dev,
				 "RX port %#x frame of %u bytes dropped\n",
				 port, len);
			atomic_inc(&hif->rx_dropped);
			break;
		}

		if (mtk_wlan_hif_port_read(hif, port, read_len, hif->rx_buf,
					   MTK_WLAN_HIF_RX_BUF_SIZE)) {
			atomic_inc(&hif->rx_dropped);
			break;
		}

		mtk_wlan_mac_rx(hif, hif->rx_buf, read_len);
	}

	if (i)
		dev_dbg_ratelimited(hif->dev, "drained %u RX frame(s) on port %#x\n",
				    i, port);
}

static void mtk_wlan_hif_fatal_work(struct work_struct *work)
{
	struct mtk_wlan_hif *hif = container_of(to_delayed_work(work),
						struct mtk_wlan_hif, fatal_work);

	if (!READ_ONCE(hif->up))
		return;

	dev_err(hif->dev, "fatal HIF error, asserting connectivity chip\n");
	/* Same reason code as glDoChipReset(), gl_rst.h:80-89. */
	mtk_wcn_wmt_assert(WMTDRV_WIFI, 0x40);
}

static void mtk_wlan_hif_irq_work(struct work_struct *work)
{
	struct mtk_wlan_hif *hif = container_of(work, struct mtk_wlan_hif,
						irq_work);
	u32 hisr = 0;
	int ret;

	if (!READ_ONCE(hif->up))
		return;

	ret = mtk_wlan_hif_port_read(hif, MCR_WHISR, sizeof(hisr),
				     (u8 *)&hisr, sizeof(hisr));
	if (ret) {
		dev_err(hif->dev, "cannot read HIF interrupt status: %d\n", ret);
		/* Let this work return before WMT may cancel it from teardown. */
		schedule_delayed_work(&hif->fatal_work, msecs_to_jiffies(100));
		return;
	}

	if (hisr & (WHISR_RX0_DONE_INT | WHISR_RX1_DONE_INT)) {
		if (hisr & WHISR_RX0_DONE_INT)
			mtk_wlan_hif_rx_drain(hif, MCR_WRDR0);
		if (hisr & WHISR_RX1_DONE_INT)
			mtk_wlan_hif_rx_drain(hif, MCR_WRDR1);
	}

	if (hisr & WHISR_ABNORMAL_INT)
		dev_warn(hif->dev, "HIF abnormal interrupt, WASR=0x%08x\n",
			 mtk_wlan_hif_readl(hif, MCR_WASR));

	if (hisr & WHISR_TX_DONE_INT) {
		u32 wtsr0 = mtk_wlan_hif_readl(hif, MCR_WTSR0);
		u32 wtsr1 = mtk_wlan_hif_readl(hif, MCR_WTSR1);

		/* Each byte is the released TX buffer count of one TC. */
		dev_dbg(hif->dev, "TX done, WTSR0=0x%08x WTSR1=0x%08x\n",
			wtsr0, wtsr1);
		mtk_wlan_mac_tx_done(hif, wtsr0, wtsr1);
	}

	/* Re-enable unconditionally, like nicEnableInterrupt() at the end of
	 * wlanIST() (wlan_lib.c:1846-1847).
	 */
	mtk_wlan_hif_set_int_enable(hif, true);
}

static irqreturn_t mtk_wlan_hif_isr(int irq, void *data)
{
	struct mtk_wlan_hif *hif = data;

	/* Mask the source before waking the bottom half (ahb.c:1226-1249). */
	mtk_wlan_hif_writel(hif, MCR_WHLPCR, WHLPCR_INT_EN_CLR);

	if (READ_ONCE(hif->up))
		schedule_work(&hif->irq_work);

	return IRQ_HANDLED;
}

int mtk_wlan_hif_request_irq(struct mtk_wlan_hif *hif)
{
	int ret;

	if (hif->irq_requested)
		return 0;

	ret = request_irq(hif->irq, mtk_wlan_hif_isr, 0, "WLAN AHB ISR", hif);
	if (ret) {
		dev_err(hif->dev, "cannot request IRQ %d: %d\n", hif->irq, ret);
		return ret;
	}
	hif->irq_requested = true;
	return 0;
}

void mtk_wlan_hif_free_irq(struct mtk_wlan_hif *hif)
{
	if (!hif->irq_requested)
		return;

	free_irq(hif->irq, hif);
	hif->irq_requested = false;
}

/*
 * Quiesce the HIF.  Called from the WMT WIFI function off callback, while
 * the connectivity subsystem is still powered.
 */
void mtk_wlan_hif_stop(struct mtk_wlan_hif *hif)
{
	WRITE_ONCE(hif->up, false);

	if (hif->int_enabled)
		mtk_wlan_hif_set_int_enable(hif, false);

	mtk_wlan_hif_free_irq(hif);
	cancel_work_sync(&hif->irq_work);
}

int mtk_wlan_hif_start(struct mtk_wlan_hif *hif)
{
	bool reenable_irq = false;
	int ret;

	/*
	 * On a re-start after a whole-chip reset the IRQ is still installed.
	 * Park it while the HIF is re-initialised so the bottom half cannot
	 * touch a half-programmed interface.
	 */
	if (hif->irq_requested) {
		disable_irq(hif->irq);
		reenable_irq = true;
	}

	ret = mtk_wlan_hif_request_irq(hif);
	if (ret)
		goto out;

	if (hif->dma_base) {
		mutex_lock(&hif->io_lock);
		if (!mtk_wlan_clk_enable(hif)) {
			mtk_wlan_pdma_reset(hif);
			mtk_wlan_clk_disable(hif);
		}
		mutex_unlock(&hif->io_lock);
	}

out:
	if (reenable_irq)
		enable_irq(hif->irq);
	return ret;
}

/*
 * Platform driver.
 */

static struct device_node *mtk_wlan_consys_node(void)
{
	return of_find_compatible_node(NULL, NULL, "mediatek,mt6735-consys");
}

static int mtk_wlan_ahb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wlan_hif *hif;
	struct resource *res;
	struct device_node *np;
	struct platform_device *consys;
	int ret;

	hif = devm_kzalloc(dev, sizeof(*hif), GFP_KERNEL);
	if (!hif)
		return -ENOMEM;

	hif->dev = dev;
	hif->irq = platform_get_irq(pdev, 0);
	if (hif->irq < 0)
		return hif->irq;

	hif->hif_base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hif->hif_base))
		return PTR_ERR(hif->hif_base);
	if (resource_size(res) < MTK_WLAN_HIF_MIN_SIZE)
		dev_warn(dev, "HIF window is only %#llx bytes\n",
			 (u64)resource_size(res));
	hif->hif_phys = res->start;

	/*
	 * The AP-DMA channel is optional: without it the HIF falls back to
	 * PIO, which is slow but functional (hif.h:71).
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		hif->dma_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(hif->dma_base))
			return PTR_ERR(hif->dma_base);
		if (resource_size(res) < AP_DMA_HIF_LENGTH) {
			dev_warn(dev, "AP-DMA window too small, using PIO\n");
			hif->dma_base = NULL;
		}
	} else {
		dev_warn(dev, "no AP-DMA window in DT, using PIO\n");
	}
	hif->dma_enabled = !!hif->dma_base;

	hif->clk = devm_clk_get(dev, "wifi-dma");
	if (IS_ERR(hif->clk))
		return dev_err_probe(dev, PTR_ERR(hif->clk),
				     "cannot get wifi-dma clock\n");

	hif->rx_buf = devm_kmalloc(dev, MTK_WLAN_HIF_RX_BUF_SIZE, GFP_KERNEL);
	if (!hif->rx_buf)
		return -ENOMEM;

	mutex_init(&hif->io_lock);
	mutex_init(&hif->state_lock);
	atomic_set(&hif->tx_count, 0);
	atomic_set(&hif->rx_dropped, 0);
	INIT_WORK(&hif->irq_work, mtk_wlan_hif_irq_work);
	INIT_DELAYED_WORK(&hif->fatal_work, mtk_wlan_hif_fatal_work);

	/*
	 * The connectivity subsystem must be resumed before this device; the
	 * link is informational when the CONSYS node is present.
	 */
	np = mtk_wlan_consys_node();
	if (np) {
		consys = of_find_device_by_node(np);
		if (consys) {
			if (!device_link_add(dev, &consys->dev,
					     DL_FLAG_STATELESS |
					     DL_FLAG_AUTOREMOVE_CONSUMER))
				dev_dbg(dev, "cannot link to the consys device\n");
			put_device(&consys->dev);
		} else {
			dev_dbg(dev, "consys device not available yet\n");
		}
		of_node_put(np);
	}

	platform_set_drvdata(pdev, hif);

	ret = mtk_wlan_main_attach(hif);
	if (ret)
		return ret;

	dev_info(dev, "MediaTek WLAN AHB HIF ready (%s)\n",
		 hif->dma_enabled ? "PDMA" : "PIO");
	return 0;
}

static void mtk_wlan_ahb_remove(struct platform_device *pdev)
{
	struct mtk_wlan_hif *hif = platform_get_drvdata(pdev);

	if (!hif)
		return;

	mtk_wlan_main_detach(hif);
	cancel_delayed_work_sync(&hif->fatal_work);
	mtk_wlan_hif_stop(hif);
}

static int mtk_wlan_ahb_suspend(struct device *dev)
{
	struct mtk_wlan_hif *hif = dev_get_drvdata(dev);

	if (hif && READ_ONCE(hif->up) && hif->int_enabled) {
		mtk_wlan_hif_set_int_enable(hif, false);
		hif->suspended = true;
	}
	return 0;
}

static int mtk_wlan_ahb_resume(struct device *dev)
{
	struct mtk_wlan_hif *hif = dev_get_drvdata(dev);

	if (hif && hif->suspended) {
		hif->suspended = false;
		if (READ_ONCE(hif->up))
			mtk_wlan_hif_set_int_enable(hif, true);
	}
	return 0;
}

static const struct dev_pm_ops mtk_wlan_pm_ops = {
	.suspend = mtk_wlan_ahb_suspend,
	.resume = mtk_wlan_ahb_resume,
};

static struct platform_driver mtk_wlan_ahb_driver = {
	.probe = mtk_wlan_ahb_probe,
	.remove = mtk_wlan_ahb_remove,
	.driver = {
		.name = "mt-wifi",
		.of_match_table = mtk_wlan_of_match,
		.pm = &mtk_wlan_pm_ops,
	},
};

int mtk_wlan_ahb_driver_register(void)
{
	return platform_driver_register(&mtk_wlan_ahb_driver);
}

void mtk_wlan_ahb_driver_unregister(void)
{
	platform_driver_unregister(&mtk_wlan_ahb_driver);
}
