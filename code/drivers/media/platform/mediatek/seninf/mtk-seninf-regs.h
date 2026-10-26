/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735 SENINF register definitions
 *
 * Port from Samsung/MTK downstream:
 *   drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c
 *   drivers/misc/mediatek/cameraisp/src/mt6735/inc/camera_isp_D1.h
 *   drivers/misc/mediatek/imgsensor/src/mt6735/kd_sensorlist.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2016 Samsung Electronics Co., Ltd.
 *
 * Downstream maps the SENINF block through the ISP driver and reaches the
 * registers below as ISP_ADDR + offset, where ISP_ADDR is 0x15004000
 * (camera_isp_D1.c:124-133) while the SENINF block itself starts at
 * 0x15008000 (SENINF_BASE_ADDR, same lines, and the kd_camera_hw1 node in
 * arch/arm/boot/dts/mt6735.dtsi:1307-1320).  The offsets in this file are
 * expressed relative to the SENINF base, i.e. downstream offset - 0x4000.
 *
 * The CSI-2 receiver and MIPI D-PHY programming is not present in the
 * downstream GPL source at all: it lives in the proprietary userspace
 * "seninf_drv" (camera_isp_D1.c:125-133).  The layout of that part of this
 * file therefore comes from the Thumb-2 implementation in the vendor
 * library libcam.halsensor.so (class SeninfDrvImp), and every definition
 * cites the instruction address (file offset of the ELF image) that
 * programs it.  Instruction addresses are relative to the start of the
 * library; the disassembly is generated with
 *   llvm-objdump -d --triple=thumbv7-none-eabi libcam.halsensor.so
 */

#ifndef __MTK_SENINF_REGS_H__
#define __MTK_SENINF_REGS_H__

#include <linux/bits.h>

/* SENINF_REG_RANGE, camera_isp_D1.c:125 */
#define MTK_SENINF_REG_RANGE		0x4000
/* MIPIRX_CONFIG_RANGE, camera_isp_D1.c:129 */
#define MTK_SENINF_MIPIRX_REG_RANGE	0x1000
/* MIPIRX_ANALOG_RANGE, camera_isp_D1.c:131 */
#define MTK_SENINF_MIPI_ANA_RANGE	0x3000

/*
 * Per-camera SENINF register block stride.  The four SENINF instances are
 * 0x400 apart: SENINFn_IRQ_INTSTA is ISP_ADDR + 0x4128/0x4528/0x4928/0x4D28
 * (camera_isp_D1.c:257-260) and the MCLK control registers are
 * ISP_ADDR + 0x4200/0x4600/0x4A00 (camera_isp_D1.c:12060/12093/12123).
 * The vendor library confirms the same stride for the CSI-2 registers:
 * setSeninf1NCSI2 touches 0x100/0x120/0x338..0x3d8 (@0x16240..0x166dc),
 * setSeninf2NCSI2 the same set + 0x400 (@0x16712..0x169fc) and
 * setSeninf3NCSI2 + 0x800 (@0x16c..0x16ee0).
 */
#define MTK_SENINF_CAM_STRIDE		0x400

/*
 * SENINFn_IRQ_INTSTA, relative to the SENINF base:
 *   SENINF1: 0x15008128 = 0x128, SENINF2: 0x15008528 = 0x528,
 *   SENINF3: 0x15008928 = 0x928, SENINF4: 0x15008D28 = 0xD28.
 */
#define MTK_SENINF_CAM_INTSTA(n)	(0x128 + (n) * MTK_SENINF_CAM_STRIDE)

/* SENINFn_IRQ_INTSTA error bits, camera_isp_D1.h:268-298 */
#define MTK_SENINF_IRQ_OVERRUN		BIT(0)
#define MTK_SENINF_IRQ_CRCERR		BIT(1)
#define MTK_SENINF_IRQ_FSMERR		BIT(2)
#define MTK_SENINF_IRQ_VSIZEERR		BIT(3)
#define MTK_SENINF_IRQ_HSIZEERR		BIT(4)
#define MTK_SENINF_IRQ_SENSOR_VSIZEERR	BIT(5)
#define MTK_SENINF_IRQ_SENSOR_HSIZEERR	BIT(6)

/* Error mask used downstream by ISP_REG_MASK_INT_SENINFn_ERR, camera_isp_D1.c:618-662 */
#define MTK_SENINF_IRQ_ERR_MASK		(GENMASK(6, 0))

/*
 * MCLK control register, relative to the SENINF base:
 *   ISP_MCLK1_EN: 0x15008200 = 0x200 (camera_isp_D1.c:12060)
 *   ISP_MCLK2_EN: 0x15008600 = 0x600 (camera_isp_D1.c:12093)
 *   ISP_MCLK3_EN: 0x15008A00 = 0xA00 (camera_isp_D1.c:12123)
 * There is no ISP_MCLK4_EN in the downstream GPL source, so only three MCLK
 * outputs are modelled.
 */
#define MTK_SENINF_MCLK_REG(n)		(0x200 + (n) * MTK_SENINF_CAM_STRIDE)

/* Bit 29 (0x20000000) of the ISP_MCLKn_EN registers, camera_isp_D1.c:12063-12069 */
#define MTK_SENINF_MCLK_EN		BIT(29)

/*
 * SENINFn_CTRL (n = 0..3), programmed by SeninfDrvImp::setSeninf1Ctrl
 * @0x1508a and its siblings @0x150b8/0x150e6/0x15114:
 *
 *   0x15090  ldr.w r0, [r3, #0x100]     read
 *   0x15094  orr   r0, r0, #0x1         bit 0
 *   0x150a0  bfi   r0, r1, #28, #3      bits 30:28 = PAD2CAM_DATA_ENUM
 *   0x150ae  bfi   r1, r2, #12, #4      bits 15:12 = SENINF_SOURCE_ENUM
 *
 * Bit 0 is also raised unconditionally for SENINF1..3 by init()
 * (@0x182e8..0x1833e), i.e. before any source is selected.
 */
#define MTK_SENINF_CAM_CTRL(n)		(0x100 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CTRL_EN		BIT(0)
#define MTK_SENINF_CTRL_SRC		GENMASK(15, 12)	/* SENINF_SOURCE_ENUM */
#define MTK_SENINF_CTRL_PAD2CAM		GENMASK(30, 28)	/* PAD2CAM_DATA_ENUM */
#define MTK_SENINF_CTRL_CSI2_MODE	BIT(15)	/* set for MIPI_OPHY_NCSI2 */

/*
 * SENINFn_MUX_CTRL (n = 0..3), programmed by SeninfDrvImp::setSeninf1MuxCtrl
 * @0x14d5e and its siblings @0x14e2a/0x14ef6/0x14fc2:
 *
 *   0x14d64  ldr.w r5, [r0, #0x120]
 *   0x14d68  orr   r6, r5, #0x80000000   bit 31 = mux enable
 *   0x14d74  bfi   r5, r3, #12, #4       bits 15:12 = SENINF_SOURCE_ENUM
 *   0x14d7e  bit 8 = (arg6 == 1)         (line/lane related)
 *   0x14d9e  bfi   r5, 1, #28, #2        bits 29:28 = (TG_FORMAT_ENUM == 7 ? 0 : 1)
 *   0x14de6  bfi   r3, 0x1b, #22, #6     bits 27:22 = source dependent
 *   0x14e06  bfi   r5, 0x1f, #16, #6     bits 21:16 = source dependent
 *   0x14e12  bfi   r5, r1, #10, #1       bit 10 = arg1
 *   0x14e1e  bfi   r1, r2, #9, #1        bit 9  = arg2
 *
 * The source dependent values (0x18/0x1e, 0x1b/0x1f, 0x3b/0x3f) and the
 * SENINF_SOURCE_ENUM/PAD2CAM_DATA_ENUM numbers themselves are supplied by
 * the vendor camera HAL from its per-sensor configuration; they are not
 * recoverable from the binaries (see the report).  Only bit 31 is applied
 * by this driver, from init() below.
 */
#define MTK_SENINF_CAM_MUX_CTRL(n)	(0x120 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_MUX_EN		BIT(31)

/*
 * SENINF_TOP_MUX_CTRL (relative to the SENINF base), programmed by
 * SeninfDrvImp::setSeninfTopMuxCtrl @0x14d12: one 4-bit source field per
 * "top mux" input, in nibble order {1, 0, 2, 3} for input 0..3
 * (@0x14d2e/0x14d24/0x14d3c/0x14d4c, write at @0x14d56).
 */
#define MTK_SENINF_TOP_MUX_CTRL		0x8

/*
 * CSI-2 receiver, SENINF1 instances.  All offsets + n * MTK_SENINF_CAM_STRIDE
 * for SENINF2 (pad 1) and SENINF3 (pad 2).
 *
 * The whole block below is programmed by SeninfDrvImp::setSeninf1NCSI2
 * @0x16240 (SENINF2 @0x166f8, SENINF3 @0x16b34).  The vendor function takes
 * ten arguments; the format string of its own trace message (@0x2863c)
 * names them:
 *   "[%s]TermDelay:%d SettleDelay:%d ClkTermDelay:%d Vsync:%d lane_num:%d
 *    NCSI2_en:%d HeaderOrder:%d SettleDelay(cnt):%d ISP_clk:%d dpcm:%d"
 * plus a second message (@0x285eb) for the calibration phase:
 *   "[%s]CSI-0 calibration start,MIPIType=%d,HSRXDE=%d"
 * The values come from the sensor static information block, whose layout is
 * ACDK_SENSOR_INFO_STRUCT in
 * drivers/misc/mediatek/imgsensor/inc/kd_imgsensor_define.h:427-460.
 */

/*
 * CSI-2 protocol control (NCSI2 path, MIPI_OPHY_NCSI2 = 0):
 *   0x165a8  ldr   r2, [r4, #0x3a0]
 *   0x165ae  bic   r1, r2, #0x80          clear bit 7
 *   0x165b4  str   r1, [r4, #0x3a0]
 *   0x165c4  orr   r6, r0, #0x4000000     bit 26
 *   0x165ca  orr   r1, r6, #0x10          bit 4
 *   0x165d0  orr.w r5, r1, r2, lsl #16    bits 31:16 = HeaderOrder
 *   0x165d6  orrs  r7, r5                  bits lane_num+1:0 = lane mask
 *   0x165d8  str.w r7, [r4, #0x3a0]
 */
#define MTK_SENINF_CSI2_CTRL(n)		(0x3a0 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CTRL_CLR	BIT(7)
#define MTK_SENINF_CSI2_CTRL_EN		BIT(26)
#define MTK_SENINF_CSI2_CTRL_HS		BIT(4)
#define MTK_SENINF_CSI2_CTRL_ECC_ORDER	GENMASK(31, 16)	/* HeaderOrder << 16 */
#define MTK_SENINF_CSI2_CTRL_LANES	GENMASK(3, 0)	/* (1 << (lane_num + 1)) - 1 */

/* Settle time, in SENINF clock cycles << 8: @0x165b8 / @0x169c2 / @0x16df2 */
#define MTK_SENINF_CSI2_SETTLE(n)	(0x3a8 + (n) * MTK_SENINF_CAM_STRIDE)

/* Constant written by the vendor: @0x165dc / @0x169e8 / @0x16e18 */
#define MTK_SENINF_CSI2_CTRL2(n)	(0x3b0 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CTRL2_MAGIC	0x80007fff

/*
 * DPCM dependent value: 0x16506..0x1651e indexes an 8 entry u16 table for
 * dpcm values 42..49 and writes the result (0 otherwise) to 0x3ac.  Both
 * sensors of this board leave DPCM_INFO at its zero-initialised value
 * (imx219mipiraw_Sensor.c / s5k5e3yxmipiraw_Sensor.c never assign
 * sensor_info->DPCM_INFO), so the value written is 0.
 */
#define MTK_SENINF_CSI2_DPCM(n)		(0x3ac + (n) * MTK_SENINF_CAM_STRIDE)

/*
 * "CSI-0 calibration" handshake (the vendor's own name, @0x285eb/0x2861c):
 * SENINF1 (@0x163ca..0x164bc) and SENINF3 (@0x16c60..0x16d52):
 *   bits 4,0,1,2,3 of 0x3d8 set          (lane/deskew control)
 *   0x338 |= 0x1                         (start, @0x1640a / @0x16ca0)
 *   0x33c = 0x1541                       (sequence, @0x16412 / @0x16caa)
 *   0x338 |= 0x4                         (mode, @0x16422 / @0x16cb8)
 *   usleep(500)                          (@0x1643e / @0x16cd2)
 *   completion: 0x344 & 0x10001, 0x348 & 0x101  (@0x16442..0x1645a)
 *   0x338 &= ~0x1, 0x3d8 &= ~0x1f        (@0x1646a..0x164bc)
 * SENINF2 (@0x16854..0x16920) only sets 0x738 |= 0x4 (@0x16894): the vendor
 * driver runs no start/sequence/status handshake on that instance.
 * The completion test is not fatal in the vendor driver: it only logs
 * "CSI-0 calibration failed!, NCSI2Config Reg 0x44=..., 0x48=..." (@0x286c1,
 * emitted from @0x16f14).
 */
#define MTK_SENINF_CSI2_CAL_CTRL(n)	(0x338 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CAL_START	BIT(0)
#define MTK_SENINF_CSI2_CAL_MODE	BIT(2)
#define MTK_SENINF_CSI2_CAL_SEQ(n)	(0x33c + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CAL_SEQ_VAL	0x1541
#define MTK_SENINF_CSI2_CAL_STA0(n)	(0x344 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CAL_STA0_MASK	0x10001
#define MTK_SENINF_CSI2_CAL_STA1(n)	(0x348 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CAL_STA1_MASK	0x101
#define MTK_SENINF_CSI2_LANE_CTRL(n)	(0x3d8 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_LANE_MASK	GENMASK(4, 0)

/*
 * Alternate CSI-2 group used when MIPIType == MIPI_OPHY_CSI2 (1)
 * (@0x16534..0x1655a): 0x360 = (0x360 | 0x30) | lane mask,
 * 0x364 = settle_count << 16, 0x368 |= 0xf.  Neither sensor of this board
 * selects it (both set MIPIsensorType = MIPI_OPHY_NCSI2) and no DT/kernel
 * input selects the PHY type, so the driver does not program it.
 */
#define MTK_SENINF_CSI2_CSI2_CFG(n)	(0x360 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CSI2_SETTLE(n)	(0x364 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_CSI2_LANES(n)	(0x368 + (n) * MTK_SENINF_CAM_STRIDE)

/*
 * MIPI D-PHY / MIPI RX digital block.
 *
 * The vendor library maps 0x10215000 for 0x3000 bytes (mmap call @0x1824a,
 * literal pool @0x18408) and uses that window + 0x800 for SENINF1, + 0xc00
 * for SENINF2 and + 0x1000 for SENINF3 (init @0x1827c..0x18356 stores the
 * three pointers to this[0x2c], this[0x30], this[0x34]).  The same window
 * and range are what camera_isp_D1.c:131 declares as MIPIRX_ANALOG_ADDR, and
 * the ROM device tree lists it as the fourth register of the ispsys node
 * (zip/rom_dtb_0.dts:3703).
 *
 * Per lane registers, lane 0 = clock lane, lanes 1..4 = data lanes: the
 * deskew code writes the clock lane at +0x0 (bit 20, @0x1728c) and the four
 * data lanes at +0x4..+0x10 (bit 24, @0x17298..0x172c2).
 */
#define MTK_SENINF_MIPI_RX_OFF(n)	(0x800 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_MIPI_LANES		5
#define MTK_SENINF_MIPI_LANE_REG(i)	((i) * 4)

/*
 * Lane enable sequence, SeninfDrvImp::setSeninf1NCSI2 @0x16288..0x1633a
 * (same sequence in setSeninf2NCSI2 @0x16712..0x167c2 and setSeninf3NCSI2):
 *   +0x4c/+0x50 &= 0xfefbefbe  (@0x16294/0x1629e, clears bits 24,18,12,7,0)
 *   lane regs |= 0x8           (@0x162aa..0x162da)
 *   +0x24 |= 0x1, usleep(30)   (@0x162e8/0x162ea)
 *   +0x20 |= 0x3, usleep(1)    (@0x162f8/0x162fc)
 *   lane regs |= 0x1           (@0x1630a..0x1633a)
 * Disable (@0x165f6..0x166b4):
 *   +0x24 &= ~0x1, +0x20 &= ~0x1, lane regs &= ~0x1,
 *   +0x4c/+0x50 |= 0x01041041  (@0x1666e/0x16678, sets bits 24,22,12,6,0),
 *   lane regs &= ~0x8
 */
#define MTK_SENINF_MIPI_LANE_PWR	BIT(3)
#define MTK_SENINF_MIPI_LANE_EN		BIT(0)
#define MTK_SENINF_MIPI_CFG_CLR		0xfefbefbe	/* bits 24,18,12,7,0 */
#define MTK_SENINF_MIPI_CFG_SET		0x01041041	/* bits 24,22,12,6,0 */
#define MTK_SENINF_MIPI_SHARED0		0x4c
#define MTK_SENINF_MIPI_SHARED1		0x50
#define MTK_SENINF_MIPI_CTL0		0x20
#define MTK_SENINF_MIPI_CTL1		0x24
#define MTK_SENINF_MIPI_CTL0_BITS	GENMASK(1, 0)

/*
 * Per-lane HS deskew, recovered from
 * SeninfDrvImp::autoDeskewCalibrationSeninf1 @0x16f34:
 *   bit 24 of a data lane register enables the deskew measurement and bits
 *   28:25 hold its delay (@0x16fe0..0x16ff2 for lane 1, @0x17052.. for lane
 *   2, @0x170c4.. lane 3, @0x17136.. lane 4), the clock lane uses bit 20 and
 *   bits 24:21 (@0x1728c..0x172ea);
 *   SENINF + 0x3b0 = 0x80007fff control (@0x16f92),
 *   SENINF + 0x3b4 = status/result, polled with 0xfb8 (@0x16f96/0x16fe6),
 *   SENINF + 0x3b8 = 0x12 (@0x16f8e), SENINF + 0x3bc = free running counter
 *   used as the measurement handshake (@0x16ffe).
 * The routine is not called from anywhere in the shipped camera stack (no
 * call site of vtable slot 52 exists in libcam.halsensor.so and no other
 * library imports it), so this driver does not run it; the definitions are
 * kept for documentation and debugfs.
 */
#define MTK_SENINF_CSI2_DESKEW_CTL(n)	(0x3b0 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_DESKEW_STA(n)	(0x3b4 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_DESKEW_STA_MASK	0xfb8
#define MTK_SENINF_CSI2_DESKEW_CFG(n)	(0x3b8 + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_CSI2_DESKEW_CFG_VAL	0x12
#define MTK_SENINF_CSI2_DESKEW_CNT(n)	(0x3bc + (n) * MTK_SENINF_CAM_STRIDE)
#define MTK_SENINF_MIPI_LANE_DESKEW_DATA	BIT(24)
#define MTK_SENINF_MIPI_LANE_DESKEW_DLY		GENMASK(28, 25)
#define MTK_SENINF_MIPI_LANE_DESKEW_CLK		BIT(20)
#define MTK_SENINF_MIPI_LANE_DESKEW_CLK_DLY	GENMASK(24, 21)

/*
 * Settle time conversion, SeninfDrvImp::setSeninf1NCSI2:
 *   0x164c8  cmp   r7, #0
 *   0x164cc  moveq r7, #0x55        default 85 ns when the caller passes 0
 *   0x1647e  mov.w r5, #0x12c       SENINF clock, 300 MHz
 *   0x164ce  mov.w r1, #0x3e8       divisor
 *   0x164d2  mul   r2, r5, r7
 *   0x164da  udiv  r5, r2, r1       settle_count = settle_ns * 300 / 1000
 * The value handed to setSeninf1NCSI2 for both sensors of this board is
 * 85 ns: .mipi_data_lp2hs_settle_dc = 85 in
 * imx219_mipi_raw/imx219mipiraw_Sensor.c:78 and
 * s5k5e3yx_mipi_raw/s5k5e3yxmipiraw_Sensor.c:69.
 */
#define MTK_SENINF_SETTLE_CLK_MHZ	300
#define MTK_SENINF_SETTLE_DEFAULT_NS	85

#endif /* __MTK_SENINF_REGS_H__ */
