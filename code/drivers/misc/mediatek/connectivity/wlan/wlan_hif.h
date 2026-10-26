/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WLAN gen2 AHB host interface - private definitions.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/include/hif.h
 *   drivers/misc/mediatek/connectivity/wlan/gen2/os/linux/hif/ahb/include/hif_pdma.h
 *   drivers/misc/mediatek/connectivity/wlan/gen2/include/nic/mtreg.h
 *
 * Copyright (C) 2013 MediaTek Inc.
 */

#ifndef _MTK_WLAN_HIF_H
#define _MTK_WLAN_HIF_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/rfkill.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/*
 * HIF host registers.  Offsets and field layouts are copied verbatim from
 * mtreg.h:81-143 ("1 MT6628 MCR Definition" / "2 Host Interface").
 * The HIF block lives at the first "reg" of the DT node and is 0x5c bytes
 * long (hif.h:217-218).
 */
#define MCR_WCIR			0x0000	/* chip id / revision */
#define MCR_WHLPCR			0x0004	/* HIF low power control */
#define MCR_WHCR			0x000c	/* HIF control */
#define MCR_WHISR			0x0010	/* HIF interrupt status */
#define MCR_WHIER			0x0014	/* HIF interrupt enable */
#define MCR_WASR			0x0018	/* abnormal status */
#define MCR_WTSR0			0x0020	/* WLAN TX status 0 */
#define MCR_WTSR1			0x0024	/* WLAN TX status 1 */
#define MCR_WTDR0			0x0028	/* WLAN TX data 0 */
#define MCR_WTDR1			0x002c	/* WLAN TX data 1 */
#define MCR_WRDR0			0x0030	/* WLAN RX data 0 */
#define MCR_WRDR1			0x0034	/* WLAN RX data 1 */
#define MCR_D2HRM0R			0x0040	/* device to host mailbox 0 */
#define MCR_D2HRM2R			0x0048	/* device to host mailbox 2 */
#define MCR_WRPLR			0x0050	/* RX packet length */
#define MCR_HSTCR			0x0058	/* HSIF transaction count */

/* WCIR (mtreg.h:175-178). */
#define WCIR_CHIP_ID			GENMASK(15, 0)
#define WCIR_REVISION_ID		GENMASK(19, 16)
#define WCIR_WLAN_READY			BIT(21)

/* WHLPCR (mtreg.h:187-190). */
#define WHLPCR_INT_EN_SET		BIT(0)
#define WHLPCR_INT_EN_CLR		BIT(1)
#define WHLPCR_FW_OWN_REQ_SET		BIT(8)
#define WHLPCR_FW_OWN_REQ_CLR		BIT(9)

/* WHCR (mtreg.h:204-205). */
#define WHCR_W_INT_CLR_CTRL		BIT(1)
#define WHCR_W_MAILBOX_RD_CLR_EN	BIT(2)

/* WHISR / WHIER (mtreg.h:213-225). */
#define WHISR_TX_DONE_INT		BIT(0)
#define WHISR_RX0_DONE_INT		BIT(1)
#define WHISR_RX1_DONE_INT		BIT(2)
#define WHISR_ABNORMAL_INT		BIT(3)

#define WHIER_TX_DONE_INT_EN		BIT(0)
#define WHIER_RX0_DONE_INT_EN		BIT(1)
#define WHIER_RX1_DONE_INT_EN		BIT(2)
#define WHIER_ABNORMAL_INT_EN		BIT(3)

/* Driver-own acquisition, nicpmSetDriverOwn() nic_pwr_mgt.c:313-315. */
#define WLAN_FW_OWN_TOTAL_MS		2000
#define WLAN_FW_OWN_LOOP_MS		1
#define WLAN_FW_OWN_CLR_ITERATION	256

/* Value programmed by nicInitializeAdapter() (hal.h:231-238, nic.c:1251). */
#define WHIER_DEFAULT			(WHIER_RX0_DONE_INT_EN | \
					 WHIER_RX1_DONE_INT_EN | \
					 WHIER_TX_DONE_INT_EN | \
					 WHIER_ABNORMAL_INT_EN | \
					 GENMASK(31, 8))

/* WRPLR (mtreg.h:243-244). */
#define WRPLR_RX0_PACKET_LENGTH		GENMASK(15, 0)
#define WRPLR_RX1_PACKET_LENGTH		GENMASK(31, 16)

/* HSTCR (mtreg.h:246-260). */
#define HSTCR_HSIF_TRANS_CNT		GENMASK(19, 2)
#define HSTCR_TRANS_TARGET		GENMASK(22, 20)
#define HSTCR_TRANS_TARGET_OFFSET	20
#define HSTCR_AFF_BURST_LEN		GENMASK(25, 24)
#define HSTCR_AFF_BURST_LEN_OFFSET	24

/* Burst length / transfer target (hif.h:220-232). */
enum mtk_wlan_hif_burst {
	HIF_BURST_1DW = 0,
	HIF_BURST_4DW,
	HIF_BURST_8DW,
};

enum mtk_wlan_hif_target {
	HIF_TARGET_TXD0 = 0,
	HIF_TARGET_TXD1,
	HIF_TARGET_RXD0,
	HIF_TARGET_RXD1,
	HIF_TARGET_WHISR,
};

/*
 * AP-DMA (PDMA) channel 1 register window, hif_pdma.h:61-106.  The window
 * itself is the second "reg" of the DT node; the driver never ioremaps a
 * hard-coded address.
 */
#define AP_DMA_HIF_INT_FLAG		0x0000
#define AP_DMA_HIF_INT_EN		0x0004
#define AP_DMA_HIF_EN			0x0008
#define AP_DMA_HIF_RST			0x000c
#define AP_DMA_HIF_CON			0x0018
#define AP_DMA_HIF_SRC_ADDR		0x001c
#define AP_DMA_HIF_DST_ADDR		0x0020
#define AP_DMA_HIF_LEN			0x0024
#define AP_DMA_HIF_LENGTH		0x0054

#define ADH_CR_FLAG_0			BIT(0)
#define ADH_CR_INTEN_FLAG_0		BIT(0)
#define ADH_CR_EN			BIT(0)
#define ADH_CR_WARM_RST			BIT(0)
#define ADH_CR_HARD_RST			BIT(1)
#define ADH_CR_DIR			BIT(0)
#define ADH_CR_FIX_EN			BIT(1)
#define ADH_CR_BURST_LEN		GENMASK(17, 16)
#define ADH_CR_BURST_LEN_OFFSET		16
#define ADH_CR_LEN			GENMASK(19, 0)

/* hif_pdma.h:45-50. */
enum mtk_wlan_pdma_burst {
	HIF_PDMA_BURST_1_4 = 0,
	HIF_PDMA_BURST_2_4,
	HIF_PDMA_BURST_3_4,
	HIF_PDMA_BURST_4_4,
};

/* hif_rx.h:115-116: HIF_RX_HW_APPENDED_LEN. */
#define MTK_WLAN_HIF_HW_APPENDED_LEN	4

/*
 * Largest frame the firmware can hand up: CFG_RX_MAX_PKT_SIZE,
 * config.h:1179 (28 + 2312 + 12).
 */
#define MTK_WLAN_RX_MAX_PKT_SIZE	2352

/*
 * The HIF data port carries ALIGN_4(u2PacketLen) bytes plus one hardware
 * appended DW holding the checksum status (nic_rx.c:2887-2890).  Both
 * together may exceed MTK_WLAN_RX_MAX_PKT_SIZE by up to four bytes, so the
 * bounce buffer keeps that much slack.
 */
#define MTK_WLAN_HIF_RX_BUF_SIZE	(MTK_WLAN_RX_MAX_PKT_SIZE + 4)

struct mtk_wlan_mac;

/**
 * struct mtk_wlan_hif - WLAN AHB host interface instance
 * @dev: platform device
 * @hif_base: HIF register window (DT reg 0)
 * @dma_base: AP-DMA channel window (DT reg 1), %NULL for PIO-only
 * @hif_phys: physical address of @hif_base, used for the PDMA source/dest
 * @irq: BGF_EINT line (DT interrupt 0)
 * @clk: "wifi-dma" AP-DMA gate
 * @chip_id: WLAN chip id read from WCIR, after the Denali remap
 * @dma_enabled: PDMA is available and selected
 * @int_enabled: HIF interrupt enable is currently asserted
 * @irq_requested: @irq is installed
 * @up: firmware is downloaded and running
 * @resetting: a whole-chip reset is in progress
 * @suspended: system suspend masked the HIF interrupt
 * @tx_count: frames handed to the HIF TX data port (WMT bus counter)
 * @rx_dropped: RX frames drained without a valid length or read error
 * @cmd_seq: command sequence number used by the firmware download
 * @io_lock: serializes port/DMA transactions
 * @state_lock: serializes bring-up and teardown
 * @mac: 802.11 MAC/cfg80211 instance, owned by wlan_mac.c
 * @rx_buf: bounce buffer for the RX drain in &irq_work
 * @irq_work: HIF interrupt bottom half
 * @fatal_work: deferred chip assert after a fatal HIF error
 * @reset_work: deferred re-open after a whole-chip reset
 * @reset_failed: last whole-chip reset failed
 * @was_up: WLAN was running when the reset started
 * @rfkill: WLAN rfkill switch
 * @wmt_registered: WMT WLAN callbacks are installed
 */
struct mtk_wlan_hif {
	struct device *dev;
	void __iomem *hif_base;
	void __iomem *dma_base;
	phys_addr_t hif_phys;
	int irq;
	struct clk *clk;
	u32 chip_id;
	bool dma_enabled;
	bool int_enabled;
	bool irq_requested;
	bool up;
	bool resetting;
	bool suspended;
	atomic_t tx_count;
	atomic_t rx_dropped;
	u8 cmd_seq;
	struct mutex io_lock;		/* serializes port/DMA transactions */
	struct mutex state_lock;	/* serializes bring-up and teardown */
	struct mtk_wlan_mac *mac;	/* 802.11 MAC/cfg80211 instance */
	u8 *rx_buf;
	struct work_struct irq_work;
	struct delayed_work fatal_work;
	struct work_struct reset_work;
	bool reset_failed;
	bool was_up;
	struct rfkill *rfkill;
	bool wmt_registered;
	bool msgcb_registered;
};

static inline u32 mtk_wlan_hif_readl(struct mtk_wlan_hif *hif, u32 off)
{
	return readl(hif->hif_base + off);
}

static inline void mtk_wlan_hif_writel(struct mtk_wlan_hif *hif, u32 off,
				       u32 val)
{
	writel(val, hif->hif_base + off);
}

/* wlan_ahb.c */
int mtk_wlan_ahb_driver_register(void);
void mtk_wlan_ahb_driver_unregister(void);

void mtk_wlan_hif_hw_init(struct mtk_wlan_hif *hif);
int mtk_wlan_hif_claim_driver_own(struct mtk_wlan_hif *hif);
int mtk_wlan_hif_port_read(struct mtk_wlan_hif *hif, u16 port, u32 size,
			   u8 *buf, u32 max);
int mtk_wlan_hif_port_write(struct mtk_wlan_hif *hif, u16 port, u32 size,
			    const u8 *buf, u32 max);
void mtk_wlan_hif_set_int_enable(struct mtk_wlan_hif *hif, bool enable);
int mtk_wlan_hif_request_irq(struct mtk_wlan_hif *hif);
void mtk_wlan_hif_free_irq(struct mtk_wlan_hif *hif);
int mtk_wlan_hif_start(struct mtk_wlan_hif *hif);
void mtk_wlan_hif_stop(struct mtk_wlan_hif *hif);

/* wlan_fw.c */
int mtk_wlan_fw_bringup(struct mtk_wlan_hif *hif);

/*
 * wlan_mac.c hooks used by the HIF.  @buf holds one complete RX buffer as it
 * left the data port: HIF_RX_HEADER_T, payload and the hardware appended
 * length/checksum DW, @len bytes in total.
 */
void mtk_wlan_mac_rx(struct mtk_wlan_hif *hif, const u8 *buf, u32 len);
void mtk_wlan_mac_tx_done(struct mtk_wlan_hif *hif, u32 wtsr0, u32 wtsr1);

/* wlan_main.c */
int mtk_wlan_main_attach(struct mtk_wlan_hif *hif);
void mtk_wlan_main_detach(struct mtk_wlan_hif *hif);
int mtk_wcn_wlan_gen2_init(void);
void mtk_wcn_wlan_gen2_exit(void);

#endif /* _MTK_WLAN_HIF_H */
