/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek connectivity subsystem (CONSYS) hardware interface.
 *
 * Ported from Samsung/MTK downstream:
 *   drivers/misc/mediatek/connectivity/common/conn_soc/mt6735/include/mtk_wcn_consys_hw.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_WCN_CONSYS_H_
#define _MTK_WCN_CONSYS_H_

#include <linux/io.h>
#include <linux/types.h>

/* Register layout is described by Documentation/devicetree/bindings/net/
 * mediatek,mt6735-consys.yaml (reg order: MCU config, AP RGU, TOPCKGEN, SPM).
 */
#define MTK_WCN_CONSYS_REG_MCU		0
#define MTK_WCN_CONSYS_REG_AP_RGU	1
#define MTK_WCN_CONSYS_REG_TOPCKGEN	2
#define MTK_WCN_CONSYS_REG_SPM		3
#define MTK_WCN_CONSYS_REG_MAX		4

/**
 * mtk_wcn_consys_hw_init() - map registers and reserved memory
 *
 * Called once by the WMT core before the first power-on.
 *
 * Return: 0 on success, negative errno on failure.
 */
int mtk_wcn_consys_hw_init(void);

/* Release everything acquired by mtk_wcn_consys_hw_init(). */
void mtk_wcn_consys_hw_deinit(void);

/**
 * mtk_wcn_consys_hw_pwr_on() - power up the connectivity subsystem
 *
 * Performs the SPM/TOPCKGEN/RGU sequence and releases the CONSYS reset.
 */
int mtk_wcn_consys_hw_pwr_on(void);

/* Power down the connectivity subsystem. */
int mtk_wcn_consys_hw_pwr_off(void);

/* Assert and release the CONSYS CPU software reset. */
int mtk_wcn_consys_hw_rst(void);

/* Physical base of the CONSYS/AP shared memory window (0 if absent). */
phys_addr_t mtk_wcn_consys_emi_phys_get(void);

/* CPU mapping of the shared memory window (NULL if absent). */
void __iomem *mtk_wcn_consys_emi_virt_get(void);

/* SoC id read back from the CONSYS MCU configuration registers. */
u32 mtk_wcn_consys_soc_chipid_get(void);

#endif /* _MTK_WCN_CONSYS_H_ */
