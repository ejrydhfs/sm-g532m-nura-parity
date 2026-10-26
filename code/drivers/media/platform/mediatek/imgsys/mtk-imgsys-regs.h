/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735/MT6737T IMGSYS/ISP register definitions
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c
 *
 * Copyright 2008-2009 MediaTek Co., Ltd.
 *
 * Only the registers needed to power up, reset and inspect the ISP block are
 * described here.  The downstream ISP register map is much larger, but every
 * other register belongs to the Pass 1/Pass 2/CAM_SV programming sequences
 * which are driven by the proprietary MTK camera userspace ABI (libcamdrv)
 * and are intentionally not ported.
 */

#ifndef __MTK_IMGSYS_REGS_H
#define __MTK_IMGSYS_REGS_H

/* ISP core window, DT reg entry 0 (camera_isp_D1.c:110-200, :1278). */
#define MTK_IMGSYS_ISP_WINDOW_SIZE		0x4000

/* Inner ISP window, DT reg entry 1 (camera_isp_D1.c:311-313). */
#define MTK_IMGSYS_INNER_ISP_WINDOW_SIZE	0x1000

/*
 * CAM_CTL_SW_CTL, ISP base + 0x8C
 * (camera_isp_D1.c:244 "ISP_REG_ADDR_CAM_SW_CTL (ISP_ADDR + 0x8C)").
 */
#define MTK_IMGSYS_CAM_SW_CTL			0x008c

/* P1 soft reset field, camera_isp_D1.c:370-373. */
#define MTK_IMGSYS_SW_CTL_SW_RST_P1_MASK	GENMASK(2, 0)
#define MTK_IMGSYS_SW_CTL_SW_RST_TRIG		BIT(0)
#define MTK_IMGSYS_SW_CTL_SW_RST_STATUS		BIT(1)
#define MTK_IMGSYS_SW_CTL_HW_RST		BIT(2)

/* P2 soft reset field, camera_isp_D1.c:374-377. */
#define MTK_IMGSYS_SW_CTL_SW_RST_P2_MASK	GENMASK(6, 4)
#define MTK_IMGSYS_SW_CTL_SW_RST_P2_TRIG	BIT(4)
#define MTK_IMGSYS_SW_CTL_SW_RST_P2_STATUS	BIT(5)
#define MTK_IMGSYS_SW_CTL_HW_RST_P2		BIT(6)

/*
 * CAMSV / CAMSV2 register bank.
 *
 * The vendor DT describes the ISP block as a single 0x9000 window starting at
 * 0x15004000 (zip/rom_dtb_0.dts:3705-3706, mt6735.dtsi:1265-1272), and the
 * CAMSV banks start half way into it (camera_isp_D1.c:262-267):
 *
 *	CAMSV_SW_CTL	= ISP_ADDR + 0x5010 (camera_isp_D1.c:263)
 *	CAMSV2_SW_CTL	= ISP_ADDR + 0x5810 (camera_isp_D1.c:267)
 *
 * Both fall inside the first 0x1000 page of ISP_ADDR + 0x5000, which is mapped
 * separately by the driver (see mtk-imgsys.c).
 */
#define MTK_IMGSYS_CAMSV_WINDOW_OFFSET		0x5000
#define MTK_IMGSYS_CAMSV_WINDOW_SIZE		0x1000
#define MTK_IMGSYS_CAMSV_SW_CTL			0x0010
#define MTK_IMGSYS_CAMSV2_SW_CTL		0x0810

#endif /* __MTK_IMGSYS_REGS_H */
