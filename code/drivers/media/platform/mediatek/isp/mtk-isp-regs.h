/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735/MT6737T camera ISP register map.
 *
 * Every offset/value in this file is taken from the downstream GPL driver
 * drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c (register
 * defines at :216-700) and inc/camera_isp_D1.h (interrupt bits at
 * :48-260), and cross-checked against the ROM kernel image
 * (zip/vmlinux.elf) where the same driver is built.
 *
 * Two addressing conventions appear below:
 *
 *  - ISP_xxx: offset from the ISP core base (0x15004000).
 *  - CAM_xxx: offset from 0x15000000, i.e. the addressing the userspace ABI
 *    uses (ISP_REG_STRUCT.Addr).  The downstream register accessors compute
 *    ISP_ADDR_CAMINF + Addr, where ISP_ADDR_CAMINF is 0x15000000
 *    (camera_isp_D1.c:3387-3455), so a valid Addr is in
 *    [0x4000, 0x10000).
 */

#ifndef _MTK_ISP_REGS_H
#define _MTK_ISP_REGS_H

/* ------------------------------------------------------------------ */
/* Physical windows (downstream ISP_CAM_BASEADDR_ENUM, camera_isp_D1.c:167) */
/* ------------------------------------------------------------------ */

#define MTK_ISP_PHYS_ISP		0x15004000
#define MTK_ISP_SIZE_ISP		0x9000	/* ROM DTB ispsys reg[0] */
#define MTK_ISP_PHYS_ISP_INNER		0x1500d000
#define MTK_ISP_SIZE_ISP_INNER		0x1000
#define MTK_ISP_PHYS_IMGSYS		0x15000000
#define MTK_ISP_SIZE_IMGSYS		0x10000	/* ROM DTB ispsys reg[2] */
#define MTK_ISP_PHYS_MIPI_ANA		0x10215000
#define MTK_ISP_SIZE_MIPI_ANA		0x3000
#define MTK_ISP_PHYS_GPIO		0x10211000
#define MTK_ISP_SIZE_GPIO		0x1000

/* Windows reachable through mmap(2) (camera_isp_D1.c:11263-11335). */
#define MTK_ISP_PHYS_SENINF		0x15008000
#define MTK_ISP_SIZE_SENINF		0x4000
#define MTK_ISP_PHYS_MIPIRX_CFG		0x1500c000
#define MTK_ISP_SIZE_MIPIRX_CFG		0x1000
#define MTK_ISP_PHYS_PLL		0x10000000
#define MTK_ISP_SIZE_PLL		0x1000

/* Register-space base used by ISP_REG_STRUCT.Addr and its valid range. */
#define MTK_ISP_CAM_BASE		0x15000000
#define MTK_ISP_CAM_ADDR_MIN		0x4000
#define MTK_ISP_CAM_ADDR_MAX		0x10000

/* ------------------------------------------------------------------ */
/* Interrupt / status / control registers (camera_isp_D1.c:237-300)    */
/* ------------------------------------------------------------------ */

#define ISP_REG_EN1		0x0004
#define ISP_REG_INT_P1_ST	0x004c
#define ISP_REG_INT_P1_ST2	0x0054
#define ISP_REG_INT_P1_ST_D	0x005c
#define ISP_REG_INT_P1_ST2_D	0x0064
#define ISP_REG_INT_P2_ST	0x006c
#define ISP_REG_INT_STATUSX	0x0070
#define ISP_REG_INT_STATUS2X	0x0074
#define ISP_REG_INT_STATUS3X	0x0078
#define ISP_REG_CAM_SW_CTL	0x008c
#define ISP_REG_IMGO_FBC		0x00f0
#define ISP_REG_RRZO_FBC		0x00f4
#define ISP_REG_IMGO_D_FBC	0x00f8
#define ISP_REG_RRZO_D_FBC	0x00fc
#define ISP_REG_TG_VF_CON	0x0414
#define ISP_REG_TG_INTER_ST	0x044c
#define ISP_REG_TG2_VF_CON	0x2414
#define ISP_REG_TG2_INTER_ST	0x244c
#define ISP_REG_IMGO_BASE_ADDR	0x3300
#define ISP_REG_RRZO_BASE_ADDR	0x3320
#define ISP_REG_IMGO_D_BASE_ADDR	0x34d4
#define ISP_REG_RRZO_D_BASE_ADDR	0x34f4
#define ISP_REG_SENINF1_INT	0x4128
#define ISP_REG_SENINF2_INT	0x4528
#define ISP_REG_SENINF3_INT	0x4928
#define ISP_REG_SENINF4_INT	0x4d28
#define ISP_REG_CAMSV_FMT_SEL	0x5004
#define ISP_REG_CAMSV_INT	0x500c
#define ISP_REG_CAMSV_SW_CTL	0x5010
#define ISP_REG_CAMSV_TG_INTER_ST	0x544c
#define ISP_REG_CAMSV2_FMT_SEL	0x5804
#define ISP_REG_CAMSV2_INT	0x580c
#define ISP_REG_CAMSV2_SW_CTL	0x5810
#define ISP_REG_CAMSV_TG2_INTER_ST	0x5c4c
#define ISP_REG_CAMSV_IMGO_FBC	0x501c
#define ISP_REG_CAMSV2_IMGO_FBC	0x581c
#define ISP_REG_IMGO_SV_BASE_ADDR	0x5208
#define ISP_REG_IMGO_SV_XSIZE	0x5210
#define ISP_REG_IMGO_SV_YSIZE	0x5214
#define ISP_REG_IMGO_SV_STRIDE	0x5218
#define ISP_REG_IMGO_SV_D_BASE_ADDR	0x5228
#define ISP_REG_IMGO_SV_D_XSIZE	0x5230
#define ISP_REG_IMGO_SV_D_YSIZE	0x5234
#define ISP_REG_IMGO_SV_D_STRIDE	0x5238
#define ISP_REG_TG_GRAB_W	0x0418
#define ISP_REG_TG_GRAB_H	0x041c
#define ISP_REG_TG2_GRAB_W	0x2418
#define ISP_REG_TG2_GRAB_H	0x241c
#define ISP_REG_CAMSV_TG_VF_CON	0x5414
#define ISP_REG_CAMSV_TG2_VF_CON	0x5c14

/* Camera-interface (CAMINF) window, 0x15000000 relative. */
#define ISP_REG_TG_MAGIC_0	0x75dc
#define ISP_REG_TG2_MAGIC_0	0x75e4
#define ISP_REG_TG_RRZ_CROP_IN	0x75e0
#define ISP_REG_TG_RRZ_CROP_IN_D	0x75e8
#define ISP_REG_RRZ_W		0x4094
#define ISP_REG_RRZ_W_D		0x409c

/* Inner (TPIPE) registers, 0x15000000 relative. */
#define ISP_INNER_FMT_SEL_P1	0x4028
#define ISP_INNER_FMT_SEL_P1_D	0x402c
#define ISP_INNER_CAM_CTRL_SEL_P1	0x4034
#define ISP_INNER_CAM_CTRL_SEL_P1_D	0x4038
#define ISP_INNER_IMGO_XSIZE	0xf308
#define ISP_INNER_IMGO_YSIZE	0xf30c
#define ISP_INNER_IMGO_STRIDE	0xf310
#define ISP_INNER_IMGO_CROP	0xf31c
#define ISP_INNER_RRZO_XSIZE	0xf328
#define ISP_INNER_RRZO_YSIZE	0xf32c
#define ISP_INNER_RRZO_STRIDE	0xf330
#define ISP_INNER_RRZO_CROP	0xf33c
#define ISP_INNER_IMGO_D_XSIZE	0xf4dc
#define ISP_INNER_IMGO_D_YSIZE	0xf4e0
#define ISP_INNER_IMGO_D_STRIDE	0xf4e4
#define ISP_INNER_IMGO_D_CROP	0xf4f0
#define ISP_INNER_RRZO_D_XSIZE	0xf4fc
#define ISP_INNER_RRZO_D_YSIZE	0xf500
#define ISP_INNER_RRZO_D_STRIDE	0xf504
#define ISP_INNER_RRZO_D_CROP	0xf510
#define ISP_INNER_RRZ_HORI_INT_OFST	0xd7b4
#define ISP_INNER_RRZ_VERT_INT_OFST	0xd7bc
#define ISP_INNER_RRZ_IN_IMG	0xd7a4
#define ISP_INNER_RRZ_OUT_IMG	0xd7a8
#define ISP_INNER_RRZ_D_HORI_INT_OFST	0xe7b4
#define ISP_INNER_RRZ_D_VERT_INT_OFST	0xe7bc
#define ISP_INNER_RRZ_D_IN_IMG	0xe7a4
#define ISP_INNER_RRZ_D_OUT_IMG	0xe7a8

/* ------------------------------------------------------------------ */
/* CAM_CTL_SEL_P1 / CAM_CTL_SEL_P1_D bits (camera_isp_D1.h:137-139). */
#define ISP_CAM_CTL_SEL_P1_IMGO		(1U << 6)
#define ISP_CAM_CTL_SEL_P1_D_IMGO	(1U << 6)

/* CAM_SW_CTL bits (camera_isp_D1.c:369-380)                          */
/* ------------------------------------------------------------------ */

#define ISP_SW_CTL_SW_RST_P1_MASK	0x00000007
#define ISP_SW_CTL_SW_RST_TRIG		0x00000001
#define ISP_SW_CTL_SW_RST_STATUS	0x00000002
#define ISP_SW_CTL_HW_RST		0x00000004
#define ISP_SW_CTL_SW_RST_P2_MASK	0x00000070
#define ISP_SW_CTL_SW_RST_P2_TRIG	0x00000010
#define ISP_SW_CTL_SW_RST_P2_STATUS	0x00000020
#define ISP_SW_CTL_HW_RST_P2		0x00000040

/* Reset path identifiers (camera_isp_D1.c:380-384). */
#define ISP_REG_SW_CTL_RST_CAM_P1	1
#define ISP_REG_SW_CTL_RST_CAM_P2	2
#define ISP_REG_SW_CTL_RST_CAMSV	3
#define ISP_REG_SW_CTL_RST_CAMSV2	4

/* CQ0C base-address / FBC ring registers used by the RT ring.
 *
 * ISP_REG_CQ0C_BASE (ISP+0xB4) and ISP_REG_CQ0C2_BASE (ISP+0xCC) are the
 * heads of the two hardware CQ0C rings; the RT ring descriptors program them
 * through the "next_cq0ci" instruction (see the userspace CQ builder).  The
 * D1 kernel itself only ever reads 0xB4 for logging
 * (camera_isp_D1.c:6458, 6571).
 */
#define ISP_REG_CQ0C_BASE	0x00b4
#define ISP_REG_CQ0C2_BASE	0x00cc

/* CQ0 control register: trigger mode/source (ROM libcamdrv.so
 * IspDrv::setCQTriggerMode @0x6dfc-0x6e82 writes 0x4020 and sets bit 23).
 */
#define ISP_REG_CQ0_CTL		0x4020

/* Per-camera MCLK enable registers.  ISP_release() writes 1 to each of them
 * to stop the sensor clocks when the last user closes the device
 * (camera_isp_D1.c:11200-11210, ISP_MCLK1_EN/ISP_MCLK2_EN/ISP_MCLK3_EN).
 */
#define ISP_REG_MCLK1_EN	0x4200
#define ISP_REG_MCLK2_EN	0x4600
#define ISP_REG_MCLK3_EN	0x4a00

/* FBC bitfields (camera_isp_D1.h, union _CQ_RTBC_FBC_). */
#define ISP_FBC_CNT_MASK	0x0000000f
#define ISP_FBC_RCNT_INC	(1U << 11)
#define ISP_FBC_RCNT_SHIFT	16
#define ISP_FBC_RCNT_MASK	(0xfU << ISP_FBC_RCNT_SHIFT)

/* ------------------------------------------------------------------ */
/* Interrupt masks (camera_isp_D1.c:383-700)                          */
/* ------------------------------------------------------------------ */

/* CAM_CTL_INT_P1_STATUS */
#define ISP_IRQ_P1_VS1			(1U << 0)
#define ISP_IRQ_P1_TG1_INT1		(1U << 1)
#define ISP_IRQ_P1_TG1_INT2		(1U << 2)
#define ISP_IRQ_P1_EXPDON1		(1U << 3)
#define ISP_IRQ_P1_TG1_ERR		(1U << 4)
#define ISP_IRQ_P1_TG1_GBERR		(1U << 5)
#define ISP_IRQ_P1_CQ0_ERR		(1U << 6)
#define ISP_IRQ_P1_CQ0_VS_ERR		(1U << 7)
#define ISP_IRQ_P1_IMGO_DROP		(1U << 8)
#define ISP_IRQ_P1_RRZO_DROP		(1U << 9)
#define ISP_IRQ_P1_PASS1_DON		(1U << 10)
#define ISP_IRQ_P1_SOF1			(1U << 12)
#define ISP_IRQ_P1_PSEUDO_P1_DON	(1U << 15)
#define ISP_IRQ_P1_AF_DON		(1U << 16)
#define ISP_IRQ_P1_FLK_DON		(1U << 17)
#define ISP_IRQ_P1_IMGO_ERR		(1U << 20)
#define ISP_IRQ_P1_AAO_ERR		(1U << 21)
#define ISP_IRQ_P1_LCSO_ERR		(1U << 22)
#define ISP_IRQ_P1_RRZO_ERR		(1U << 23)
#define ISP_IRQ_P1_ESFKO_ERR		(1U << 24)
#define ISP_IRQ_P1_FLK_ERR		(1U << 25)
#define ISP_IRQ_P1_LSC_ERR		(1U << 26)
#define ISP_IRQ_P1_FBC_RRZO_DON		(1U << 27)
#define ISP_IRQ_P1_FBC_IMGO_DON		(1U << 28)
#define ISP_IRQ_P1_DMA_ERR		(1U << 30)

/* CAM_CTL_INT_P1_STATUS2 (pass-1 DMA done bits) */
#define ISP_IRQ_P1S2_IMGO_DONE		(1U << 0)
#define ISP_IRQ_P1S2_UFEO_DONE		(1U << 1)
#define ISP_IRQ_P1S2_RRZO_DONE		(1U << 2)
#define ISP_IRQ_P1S2_ESFKO_DONE		(1U << 3)
#define ISP_IRQ_P1S2_LCSO_DONE		(1U << 4)
#define ISP_IRQ_P1S2_AAO_DONE		(1U << 5)
#define ISP_IRQ_P1S2_BPCI_DONE		(1U << 6)
#define ISP_IRQ_P1S2_LSCI_DONE		(1U << 7)
#define ISP_IRQ_P1S2_AF_TAR_DONE	(1U << 8)
#define ISP_IRQ_P1S2_AF_FLO1_DONE	(1U << 9)
#define ISP_IRQ_P1S2_AF_FLO2_DONE	(1U << 10)
#define ISP_IRQ_P1S2_AF_FLO3_DONE	(1U << 11)

/* CAM_CTL_INT_P1_STATUS_D has the same bit layout as P1 except for the
 * AFO/LSC error bits; only the bits the driver consumes are listed. */
#define ISP_IRQ_P1D_AFO_ERR		(1U << 24)

/* CAM_CTL_INT_P2_STATUS */
#define ISP_IRQ_P2_CQ_ERR		(1U << 0)
#define ISP_IRQ_P2_PASS2_DON		(1U << 1)
#define ISP_IRQ_P2_TILE_DON		(1U << 2)
#define ISP_IRQ_P2_CQ_DON		(1U << 3)
#define ISP_IRQ_P2_TDR_ERR		(1U << 4)
#define ISP_IRQ_P2_PASS2A_DON		(1U << 5)
#define ISP_IRQ_P2_PASS2B_DON		(1U << 6)
#define ISP_IRQ_P2_PASS2C_DON		(1U << 7)
#define ISP_IRQ_P2_CQ1_DONE		(1U << 8)
#define ISP_IRQ_P2_CQ2_DONE		(1U << 9)
#define ISP_IRQ_P2_CQ3_DONE		(1U << 10)
#define ISP_IRQ_P2_PASS2A_ERR		(1U << 11)
#define ISP_IRQ_P2_PASS2B_ERR		(1U << 12)
#define ISP_IRQ_P2_PASS2C_ERR		(1U << 13)
#define ISP_IRQ_P2_IMGI_DONE		(1U << 15)
#define ISP_IRQ_P2_UFDI_DONE		(1U << 16)
#define ISP_IRQ_P2_VIPI_DONE		(1U << 17)
#define ISP_IRQ_P2_VIP2I_DONE		(1U << 18)
#define ISP_IRQ_P2_VIP3I_DONE		(1U << 19)
#define ISP_IRQ_P2_LCEI_DONE		(1U << 20)
#define ISP_IRQ_P2_MFBO_DONE		(1U << 21)
#define ISP_IRQ_P2_IMG2O_DONE		(1U << 22)
#define ISP_IRQ_P2_IMG3O_DONE		(1U << 23)
#define ISP_IRQ_P2_IMG3BO_DONE		(1U << 24)
#define ISP_IRQ_P2_IMG3CO_DONE		(1U << 25)
#define ISP_IRQ_P2_FEO_DONE		(1U << 26)

/* CAM_CTL_INT_STATUSX (P1 and P2 share the layout). */
#define ISP_IRQ_STATUSX_CQ_ERR_ST	(1U << 13)
#define ISP_IRQ_STATUSX_PASS2_DON	(1U << 14)
#define ISP_IRQ_STATUSX_TILE_DON	(1U << 15)
#define ISP_IRQ_STATUSX_CQ_DON		(1U << 19)

/* ISP_IRQ_STATUS3X: all DMA done bits, bit n for DMA n of the P1/P1_D set. */
#define ISP_IRQ_STATUS3X_IMGO_DONE	(1U << 0)
#define ISP_IRQ_STATUS3X_UFEO_DONE	(1U << 1)
#define ISP_IRQ_STATUS3X_RRZO_DONE	(1U << 2)
#define ISP_IRQ_STATUS3X_ESFKO_DONE	(1U << 3)
#define ISP_IRQ_STATUS3X_LCSO_DONE	(1U << 4)
#define ISP_IRQ_STATUS3X_AAO_DONE	(1U << 5)
#define ISP_IRQ_STATUS3X_BPCI_DONE	(1U << 6)
#define ISP_IRQ_STATUS3X_LSCI_DONE	(1U << 7)
#define ISP_IRQ_STATUS3X_IMGO_D_DONE	(1U << 8)
#define ISP_IRQ_STATUS3X_RRZO_D_DONE	(1U << 9)
#define ISP_IRQ_STATUS3X_AFO_D_DONE	(1U << 10)

/* REG_CAMSV_INT_STATUS / REG_ISP_IRQ_CAMSV2_STATUS */
#define ISP_IRQ_CAMSV_VS1		(1U << 0)
#define ISP_IRQ_CAMSV_TG_ST1		(1U << 1)
#define ISP_IRQ_CAMSV_TG_ST2		(1U << 2)
#define ISP_IRQ_CAMSV_EXPDON1		(1U << 3)
#define ISP_IRQ_CAMSV_TG_ERR		(1U << 4)
#define ISP_IRQ_CAMSV_TG_GBERR		(1U << 5)
#define ISP_IRQ_CAMSV_TG_DROP		(1U << 6)
#define ISP_IRQ_CAMSV_TG_SOF1		(1U << 7)
#define ISP_IRQ_CAMSV_PASS1_DON		(1U << 10)
#define ISP_IRQ_CAMSV_IMGO_ERR		(1U << 16)
#define ISP_IRQ_CAMSV_IMGO_OVERR	(1U << 17)
#define ISP_IRQ_CAMSV_IMGO_DROP		(1U << 19)

/* SENINF1..4_IRQ_INTSTA */
#define ISP_IRQ_SENINF_OVERRUN		(1U << 0)
#define ISP_IRQ_SENINF_CRCERR		(1U << 1)
#define ISP_IRQ_SENINF_FSMERR		(1U << 2)
#define ISP_IRQ_SENINF_VSIZEERR		(1U << 3)
#define ISP_IRQ_SENINF_HSIZEERR		(1U << 4)
#define ISP_IRQ_SENINF_SENSOR_VSIZEERR	(1U << 5)
#define ISP_IRQ_SENINF_SENSOR_HSIZEERR	(1U << 6)

/* Aggregate masks, verbatim from camera_isp_D1.c:383-700. */

#define ISP_MASK_INT_P1_ST							\
	(ISP_IRQ_P1_VS1 | ISP_IRQ_P1_TG1_INT1 | ISP_IRQ_P1_TG1_INT2 |		\
	 ISP_IRQ_P1_EXPDON1 | ISP_IRQ_P1_PASS1_DON | ISP_IRQ_P1_SOF1 |		\
	 ISP_IRQ_P1_AF_DON | ISP_IRQ_P1_FLK_DON | ISP_IRQ_P1_FBC_RRZO_DON |	\
	 ISP_IRQ_P1_FBC_IMGO_DON)

#define ISP_MASK_INT_P1_ST_ERR							\
	(ISP_IRQ_P1_TG1_ERR | ISP_IRQ_P1_TG1_GBERR | ISP_IRQ_P1_CQ0_ERR |	\
	 ISP_IRQ_P1_CQ0_VS_ERR | ISP_IRQ_P1_IMGO_DROP | ISP_IRQ_P1_RRZO_DROP |	\
	 ISP_IRQ_P1_IMGO_ERR | ISP_IRQ_P1_AAO_ERR | ISP_IRQ_P1_LCSO_ERR |	\
	 ISP_IRQ_P1_RRZO_ERR | ISP_IRQ_P1_ESFKO_ERR | ISP_IRQ_P1_FLK_ERR |	\
	 ISP_IRQ_P1_LSC_ERR | ISP_IRQ_P1_DMA_ERR)

#define ISP_MASK_INT_P1_ST2							\
	(ISP_IRQ_P1S2_IMGO_DONE | ISP_IRQ_P1S2_UFEO_DONE |			\
	 ISP_IRQ_P1S2_RRZO_DONE | ISP_IRQ_P1S2_ESFKO_DONE |			\
	 ISP_IRQ_P1S2_LCSO_DONE | ISP_IRQ_P1S2_AAO_DONE |			\
	 ISP_IRQ_P1S2_BPCI_DONE | ISP_IRQ_P1S2_LSCI_DONE |			\
	 ISP_IRQ_P1S2_AF_TAR_DONE | ISP_IRQ_P1S2_AF_FLO1_DONE |		\
	 ISP_IRQ_P1S2_AF_FLO2_DONE | ISP_IRQ_P1S2_AF_FLO3_DONE)

/* P1_D uses the same non-error mask as P1 (camera_isp_D1.c:477-486). */
#define ISP_MASK_INT_P1_ST_D							\
	(ISP_IRQ_P1_VS1 | ISP_IRQ_P1_TG1_INT1 | ISP_IRQ_P1_TG1_INT2 |		\
	 ISP_IRQ_P1_EXPDON1 | ISP_IRQ_P1_PASS1_DON | ISP_IRQ_P1_SOF1 |		\
	 ISP_IRQ_P1_AF_DON | ISP_IRQ_P1_FBC_RRZO_DON | ISP_IRQ_P1_FBC_IMGO_DON)

#define ISP_MASK_INT_P1_ST_D_ERR						\
	(ISP_IRQ_P1_TG1_ERR | ISP_IRQ_P1_TG1_GBERR | ISP_IRQ_P1_CQ0_ERR |	\
	 ISP_IRQ_P1_CQ0_VS_ERR | ISP_IRQ_P1_IMGO_DROP | ISP_IRQ_P1_RRZO_DROP |	\
	 ISP_IRQ_P1_IMGO_ERR | ISP_IRQ_P1_AAO_ERR | ISP_IRQ_P1_LCSO_ERR |	\
	 ISP_IRQ_P1_RRZO_ERR | ISP_IRQ_P1D_AFO_ERR | ISP_IRQ_P1_LSC_ERR |	\
	 ISP_IRQ_P1_DMA_ERR)

#define ISP_MASK_INT_P1_ST2_D	ISP_MASK_INT_P1_ST2

#define ISP_MASK_INT_P2_ST							\
	(ISP_IRQ_P2_PASS2_DON | ISP_IRQ_P2_TILE_DON | ISP_IRQ_P2_CQ_DON |	\
	 ISP_IRQ_P2_PASS2A_DON | ISP_IRQ_P2_PASS2B_DON |			\
	 ISP_IRQ_P2_PASS2C_DON | ISP_IRQ_P2_CQ1_DONE | ISP_IRQ_P2_CQ2_DONE |	\
	 ISP_IRQ_P2_CQ3_DONE | ISP_IRQ_P2_IMGI_DONE | ISP_IRQ_P2_UFDI_DONE |	\
	 ISP_IRQ_P2_VIPI_DONE | ISP_IRQ_P2_VIP2I_DONE |			\
	 ISP_IRQ_P2_VIP3I_DONE | ISP_IRQ_P2_LCEI_DONE | ISP_IRQ_P2_MFBO_DONE |	\
	 ISP_IRQ_P2_IMG2O_DONE | ISP_IRQ_P2_IMG3O_DONE |			\
	 ISP_IRQ_P2_IMG3BO_DONE | ISP_IRQ_P2_IMG3CO_DONE | ISP_IRQ_P2_FEO_DONE)

#define ISP_MASK_INT_P2_ST_ERR							\
	(ISP_IRQ_P2_CQ_ERR | ISP_IRQ_P2_TDR_ERR | ISP_IRQ_P2_PASS2A_ERR |	\
	 ISP_IRQ_P2_PASS2B_ERR | ISP_IRQ_P2_PASS2C_ERR)

#define ISP_MASK_INT_STATUSX							\
	(ISP_IRQ_P1_VS1 | ISP_IRQ_P1_TG1_INT1 | ISP_IRQ_P1_TG1_INT2 |		\
	 ISP_IRQ_P1_EXPDON1 | ISP_IRQ_P1_PASS1_DON | ISP_IRQ_P1_SOF1 |		\
	 ISP_IRQ_STATUSX_PASS2_DON | ISP_IRQ_STATUSX_TILE_DON |			\
	 ISP_IRQ_P1_AF_DON | ISP_IRQ_P1_FLK_DON | ISP_IRQ_STATUSX_CQ_DON |	\
	 ISP_IRQ_P1_FBC_RRZO_DON | ISP_IRQ_P1_FBC_IMGO_DON)

#define ISP_MASK_INT_STATUSX_ERR						\
	(ISP_IRQ_P1_TG1_ERR | ISP_IRQ_P1_TG1_GBERR | ISP_IRQ_P1_CQ0_ERR |	\
	 ISP_IRQ_P1_CQ0_VS_ERR | ISP_IRQ_P1_IMGO_DROP | ISP_IRQ_P1_RRZO_DROP |	\
	 ISP_IRQ_STATUSX_CQ_ERR_ST | ISP_IRQ_P1_IMGO_ERR | ISP_IRQ_P1_AAO_ERR |	\
	 ISP_IRQ_P1_LCSO_ERR | ISP_IRQ_P1_RRZO_ERR | ISP_IRQ_P1_ESFKO_ERR |	\
	 ISP_IRQ_P1_FLK_ERR | ISP_IRQ_P1_LSC_ERR | ISP_IRQ_P1_DMA_ERR)

#define ISP_MASK_INT_STATUS2X							\
	(ISP_IRQ_P1_VS1 | ISP_IRQ_P1_TG1_INT1 | ISP_IRQ_P1_TG1_INT2 |		\
	 ISP_IRQ_P1_EXPDON1 | ISP_IRQ_P1_PASS1_DON | ISP_IRQ_P1_SOF1 |		\
	 ISP_IRQ_P1_AF_DON | ISP_IRQ_P1_FBC_RRZO_DON | ISP_IRQ_P1_FBC_IMGO_DON)

#define ISP_MASK_INT_STATUS2X_ERR	ISP_MASK_INT_P1_ST_D_ERR

#define ISP_MASK_INT_STATUS3X							\
	(ISP_IRQ_STATUS3X_IMGO_DONE | ISP_IRQ_STATUS3X_UFEO_DONE |		\
	 ISP_IRQ_STATUS3X_RRZO_DONE | ISP_IRQ_STATUS3X_ESFKO_DONE |		\
	 ISP_IRQ_STATUS3X_LCSO_DONE | ISP_IRQ_STATUS3X_AAO_DONE |		\
	 ISP_IRQ_STATUS3X_BPCI_DONE | ISP_IRQ_STATUS3X_LSCI_DONE |		\
	 ISP_IRQ_STATUS3X_IMGO_D_DONE | ISP_IRQ_STATUS3X_RRZO_D_DONE |		\
	 ISP_IRQ_STATUS3X_AFO_D_DONE)

#define ISP_MASK_INT_STATUS3X_ERR	0

#define ISP_MASK_INT_SENINF (0U)
#define ISP_MASK_INT_SENINF_ERR							\
	(ISP_IRQ_SENINF_OVERRUN | ISP_IRQ_SENINF_CRCERR |			\
	 ISP_IRQ_SENINF_FSMERR | ISP_IRQ_SENINF_VSIZEERR |			\
	 ISP_IRQ_SENINF_HSIZEERR | ISP_IRQ_SENINF_SENSOR_VSIZEERR |		\
	 ISP_IRQ_SENINF_SENSOR_HSIZEERR)

#define ISP_MASK_CAMSV								\
	(ISP_IRQ_CAMSV_VS1 | ISP_IRQ_CAMSV_TG_ST1 | ISP_IRQ_CAMSV_TG_ST2 |	\
	 ISP_IRQ_CAMSV_EXPDON1 | ISP_IRQ_CAMSV_TG_SOF1 |			\
	 ISP_IRQ_CAMSV_PASS1_DON)

#define ISP_MASK_CAMSV_ERR							\
	(ISP_IRQ_CAMSV_TG_ERR | ISP_IRQ_CAMSV_TG_GBERR |			\
	 ISP_IRQ_CAMSV_TG_DROP | ISP_IRQ_CAMSV_IMGO_ERR |			\
	 ISP_IRQ_CAMSV_IMGO_OVERR | ISP_IRQ_CAMSV_IMGO_DROP)

#define ISP_MASK_CAMSV2		ISP_MASK_CAMSV
#define ISP_MASK_CAMSV2_ERR	ISP_MASK_CAMSV_ERR

/* Error bits the driver treats as warnings, not failures
 * (STATUSX_WARNING, camera_isp_D1.c:9140-9147). */
#define ISP_STATUSX_WARNING							\
	(ISP_IRQ_P1_IMGO_ERR | ISP_IRQ_P1_RRZO_ERR | ISP_IRQ_P1_LCSO_ERR |	\
	 ISP_IRQ_P1_AAO_ERR)

#endif /* _MTK_ISP_REGS_H */
