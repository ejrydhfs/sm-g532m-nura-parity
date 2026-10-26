/* SPDX-License-Identifier: GPL-2.0 */
/*
 * C2K (MD3) dual-modem support for the MT6735/MT6737T CCCI subsystem.
 *
 * Port from Samsung/MTK downstream (SM-G532M, MT6737T, kernel 3.18):
 *   drivers/misc/mediatek/eccci/modem_ccif_c2k.c
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.c
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.h
 *   drivers/misc/mediatek/eccci/mt6735/modem_reg_base.h
 *   drivers/misc/mediatek/eccci/port_cfg.c
 *   drivers/misc/mediatek/c2k_ccci/mt6735/c2k_platform.c (chip id, boot)
 *   drivers/watchdog/mediatek/wdt/mt6735/mtk_wdt.c (C2K sysreset)
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 The Nura project
 */

#ifndef __CCCI_C2K_H
#define __CCCI_C2K_H

#include <linux/ccci.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/types.h>

/* CCIF register map, downstream mt6735/ccif_c2k_platform.h:35-42 */
#define C2K_APCCIF_CON		0x000
#define C2K_APCCIF_BUSY		0x004
#define C2K_APCCIF_START	0x008
#define C2K_APCCIF_TCHNUM	0x00c
#define C2K_APCCIF_RCHNUM	0x010
#define C2K_APCCIF_ACK		0x014

#define C2K_CCIF_SRAM_SIZE	512	/* mt6735/modem_reg_base.h:43 */

/* INFRA_AO C2K block, downstream mt6735/ccif_c2k_platform.h:44-54 */
#define C2K_INFRA_AO_C2K_CONFIG		0x330
#define C2K_INFRA_AO_C2K_STATUS		0x334
#define C2K_INFRA_AO_C2K_SPM_CTRL	0x338
#define C2K_SLEEP_CLK_CON		0x400
#define C2K_TOP_RGU_WDT_SWSYSRST	0x018

/*
 * INFRA_AO_C2K_CONFIG bits.
 * Boot mode field [10:8], value 5 selects the C2K boot path: downstream
 * eccci/mt6735/ccif_c2k_platform.c:269-275.
 */
#define C2K_CONFIG_BOOTMODE_MASK	GENMASK(10, 8)
#define C2K_CONFIG_BOOTMODE_C2K		5
/* ETS (trace) select, downstream mt6735/ccif_c2k_platform.h:54 */
#define C2K_ETS_SEL_BIT			BIT(13)

/* srcclkena selection mask, downstream ccif_c2k_platform.c:279-288 */
#define C2K_SPM_SRCCLKENA_MASK		GENMASK(5, 4)
#define C2K_SPM_SRCCLKENA_C2K		2
/* C2K wake handshake, downstream ccif_c2k_platform.c:326-345 */
#define C2K_SPM_WAKE_BIT		BIT(0)
#define C2K_STATUS_AWAKE_BIT		BIT(1)

/*
 * SLEEP_CLK_CON programming, downstream ccif_c2k_platform.c:292-303:
 * set bits 2,3,12,27 and clear bit 14.
 */
#define C2K_SLEEP_CLK_CON_SET		(BIT(2) | BIT(3) | BIT(12) | BIT(27))
#define C2K_SLEEP_CLK_CON_CLR		BIT(14)

/*
 * TOP_RGU software sysreset.  Key and bit 15 (C2K domain) from
 * drivers/watchdog/mediatek/wdt/mt6735/mt_wdt.h:78 and mtk_wdt.c:472-504
 * (mtk_wdt_set_c2k_sysrst()); the same register/key/bit appear in the ROM
 * disassembly of mtk_wdt_set_c2k_sysrst at 0xc0715d60 (register +0x18,
 * key 0x88000000, bit 15).
 */
#define C2K_RGU_SWSYS_RST_KEY		0x88000000
#define C2K_RGU_C2K_SYSRST_BIT		15

/*
 * MD3 RGU (modem watch dog), downstream mt6735/ccif_c2k_platform.h:27-33.
 * The MD WD is deliberately left untouched for MD3: md_ccif_power_on() only
 * writes the mode key when md->index != MD_SYS3 (ccif_c2k_platform.c:392-396).
 */
#define C2K_MD_WDT_MODE			0x00
#define C2K_MD_WDT_STA			0x0c

/*
 * MD3 boot slave.  Downstream mt6735/modem_reg_base.h:64-70 declares the MD3
 * boot vector block; its values are the MD2 block (lines 48-55), which is the
 * same 0x30190000 window.
 */
#define C2K_MD_BOOT_VECTOR_VALUE	0x00000000
#define C2K_MD_BOOT_VECTOR_KEY_VALUE	0x3567c766
#define C2K_MD_BOOT_VECTOR_EN_VALUE	0xa3b66175

#define C2K_MD_BOOT_VECTOR_PHYS		0x30190000
#define C2K_MD_BOOT_VECTOR_KEY_PHYS	0x3019379c
#define C2K_MD_BOOT_VECTOR_EN_PHYS	0x30195488
#define C2K_MD_RGU_PHYS			0x30050000

/*
 * C2K chip id.  Downstream c2k_ccci/mt6735/c2k_platform.c:537 compares the
 * chip id against 0x020AC000 (that poll loop is inside "#if 0", so only the
 * read is kept here).
 */
#define C2K_CHIP_ID_EXPECTED		0x020ac000

/* Downstream defaults for the MD3 CCIF instance; see the report. */
#define C2K_MD_FIRMWARE_NAME		"md3img"

/*
 * Modem capabilities granted to MD3 by the downstream build:
 * CCMNI_IRAT is set unconditionally, WORLD_PHONE is set for C2K slot2
 * (downstream eccci/port_net.c:283-289).  The ROM device tree provides the
 * "sim1_md3_sim2_md1" SIM switch mode, i.e. C2K on a second slot.
 */
#define C2K_MD_CAPABILITY_DEFAULT \
	(CCCI_MD_CAP_CCMNI_IRAT | CCCI_MD_CAP_WORLD_PHONE)

/* IRAT peer of MD3, downstream eccci/port_net.c:289 */
#define C2K_IRAT_MD_ID			CCCI_MD_SYS1

struct ccci_c2k {
	struct device *dev;

	/* MMIO windows (optional ones may stay NULL) */
	void __iomem *ccif;		/* AP CCIF, MD3 instance */
	void __iomem *md1_pccif;	/* INFRA_MD PCCIF window 1 */
	void __iomem *md3_pccif;	/* INFRA_MD PCCIF window 2 */
	void __iomem *chip_id;		/* C2K chip id register */
	void __iomem *boot_vector;
	void __iomem *boot_vector_key;
	void __iomem *boot_vector_en;
	void __iomem *md_rgu;		/* MD3 RGU / modem WDT */
	void __iomem *infra_ao;		/* INFRA_AO C2K block */
	void __iomem *sleep;		/* SLEEP_CLK_CON */
	void __iomem *toprgu;		/* TOP_RGU WDT */

	/* physical addresses handed to the core through struct ccci_md_cfg */
	phys_addr_t boot_vector_phys;
	phys_addr_t boot_vector_key_phys;
	phys_addr_t boot_vector_en_phys;
	phys_addr_t md_rgu_phys;

	struct clk *clk_md_main;
	struct regulator *vtcxo1;

	struct ccci_md_cfg cfg;
	unsigned int md_wdt_irq;
	bool ets_sel;

	unsigned int boot_count;
	unsigned int chip_id_val;
	bool chip_id_valid;

	bool powered;
	bool vtcxo_enabled;
	bool clk_enabled;

	int sim_switch_mode;
	unsigned int irat_md_id;

	struct mutex lock;		/* sim_switch_mode, dump consistency */
	struct kobject *kobj;
	struct dentry *debugfs_dir;
};

/* ccci_c2k.c */
struct ccci_c2k *ccci_c2k_get(void);

/* ccci_c2k_hw.c */
int ccci_c2k_hw_init(struct ccci_c2k *c2k);
int ccci_c2k_hw_power_on(struct ccci_c2k *c2k);
int ccci_c2k_hw_power_off(struct ccci_c2k *c2k);
int ccci_c2k_hw_let_md_go(struct ccci_c2k *c2k);
void ccci_c2k_hw_reset(struct ccci_c2k *c2k);
unsigned int ccci_c2k_hw_read_chip_id(struct ccci_c2k *c2k);
int ccci_c2k_hw_format_regs(struct ccci_c2k *c2k, char *buf, size_t size);
int ccci_c2k_hw_format_wdt(struct ccci_c2k *c2k, char *buf, size_t size);

/* ccci_c2k_ch.c */
int ccci_c2k_ch_to_ccci(int c2k_ch, bool tx);
int ccci_c2k_ccci_to_ch(int ccci_ch, bool tx, bool md_exception);
int ccci_c2k_ch_dump(struct seq_file *m);
int ccci_c2k_set_irat(unsigned int irat_md_id);
void ccci_c2k_sim_switch_mode_set(int mode);
int ccci_c2k_sim_switch_mode_get(void);

/* ccci_c2k_sysfs.c */
int ccci_c2k_sysfs_init(struct ccci_c2k *c2k);
void ccci_c2k_sysfs_exit(struct ccci_c2k *c2k);

#endif /* __CCCI_C2K_H */
