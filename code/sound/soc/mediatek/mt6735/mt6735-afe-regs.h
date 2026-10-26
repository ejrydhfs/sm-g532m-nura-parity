/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6735/MT6737T audio front end (AFE) register definitions.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Afe.h and
 * sound/soc/mediatek/mt_soc_audio_v3/mt_soc_afe_control.c.
 *
 * Every offset below carries the downstream file:line it was transcribed
 * from.  The AFE register block is mapped at 0x11220000 (AudDrv_Afe.h:361).
 */

#ifndef _MT6735_AFE_REGS_H_
#define _MT6735_AFE_REGS_H_

#include <linux/bits.h>

#define MT6735_AFE_PHYS_BASE		0x11220000	/* AudDrv_Afe.h:361 */
#define MT6735_AFE_SIZE			0x10000		/* rom_dtb_0.dts audio@11220000 */

/*
 * AFE internal SRAM: physical 0x11221000, 0x8000 bytes
 * (AudDrv_Afe.h:375-377; Get_Afe_SramBase_Pointer(), AudDrv_Afe.c:126).
 * It lies inside the AFE ioremap window, 0x1000 bytes above the register
 * base.
 */
#define MT6735_AFE_INTERNAL_SRAM_OFFSET	0x1000
#define MT6735_AFE_INTERNAL_SRAM_SIZE	0x8000

/* Top level clock/power control */
#define AUDIO_TOP_CON0			0x0000		/* AudDrv_Afe.h:416 */
#define AUDIO_TOP_CON1			0x0004		/* AudDrv_Afe.h:423 */
#define AUDIO_TOP_CON2			0x0008		/* AudDrv_Afe.h:425 */
#define AUDIO_TOP_CON3			0x000c		/* AudDrv_Afe.h:426 */

/*
 * AUDIO_TOP_CON0 bit 14 must be set before any other AFE register is
 * accessed; the bootloader leaves it cleared (AudDrv_Clk.c:265).
 */
#define AUDIO_TOP_CON0_AFE_POWER_ON	BIT(14)		/* AudDrv_Clk.c:265,467 */

/* AUDIO_TOP_CON1 */
#define AUDIO_TOP_CON1_I2S_SOFT_RST	BIT(1)		/* mt_soc_afe_control.c:3749 */
#define AUDIO_TOP_CON1_I2S0_CG		BIT(4)		/* mt_soc_afe_control.c:3750 */
#define AUDIO_TOP_CON1_I2S3_CG		BIT(7)		/* mt_soc_afe_control.c:3751 */

/* DAC/memory interface control */
#define AFE_DAC_CON0			0x0010		/* AudDrv_Afe.h:427 */
#define AFE_DAC_CON1			0x0014		/* AudDrv_Afe.h:428 */

#define AFE_DAC_CON0_AFE_ON		BIT(0)		/* mt_soc_afe_control.c:981,1029 */
#define AFE_DAC_CON0_MEMIF_EN(id)	BIT((id) + 1)	/* mt_soc_afe_control.c:1995-1997 */

/* AFE_DAC_CON1 sample rate fields (ftell: mt_soc_afe_control.c:1073-1128) */
#define AFE_DAC_CON1_DL1_FS_SHIFT	0		/* mt_soc_afe_control.c:1080 */
#define AFE_DAC_CON1_DL2_FS_SHIFT	4		/* mt_soc_afe_control.c:1084 */
#define AFE_DAC_CON1_I2S_FS_SHIFT	8		/* mt_soc_afe_control.c:1088 */
#define AFE_DAC_CON1_AWB_FS_SHIFT	12		/* mt_soc_afe_control.c:1092 */
#define AFE_DAC_CON1_VUL_FS_SHIFT	16		/* mt_soc_afe_control.c:1096 */
#define AFE_DAC_CON1_FS_MASK		GENMASK(3, 0)

/* AFE_DAC_CON1 mono (single channel) fields (mt_soc_afe_control.c:1129-1160) */
#define AFE_DAC_CON1_DL1_MONO_SHIFT	21		/* mt_soc_afe_control.c:1135 */
#define AFE_DAC_CON1_DL2_MONO_SHIFT	22		/* mt_soc_afe_control.c:1139 */
#define AFE_DAC_CON1_AWB_MONO_SHIFT	24		/* mt_soc_afe_control.c:1143 */
#define AFE_DAC_CON1_VUL_MONO_SHIFT	27		/* mt_soc_afe_control.c:1147 */

/* I2S interfaces */
#define AFE_I2S_CON			0x0018		/* AudDrv_Afe.h:429 */
#define AFE_I2S_CON1			0x0034		/* AudDrv_Afe.h:438 */
#define AFE_I2S_CON2			0x0038		/* AudDrv_Afe.h:439 */

/*
 * AFE_I2S_CON layout, from Enable4pin_I2S0_I2S3() in
 * mt_soc_afe_control.c:3740-3786 and SetI2SDacOut()/SetI2SDacEnable()
 * in mt_soc_afe_control.c:1637-1655, 2010-2034.
 */
#define AFE_I2S_CON_EN			BIT(0)		/* mt_soc_afe_control.c:3781 */
#define AFE_I2S_CON_WLEN_SHIFT		1		/* mt_soc_afe_control.c:3760 */
#define AFE_I2S_CON_SLAVE		BIT(2)		/* mt_soc_pcm_hdmi.c:889 */
#define AFE_I2S_CON_FMT_SHIFT		3		/* mt_soc_afe_control.c:3759 */
#define AFE_I2S_CON_LRCK_INV		BIT(5)		/* mt_soc_afe_control.c:3758 */
#define AFE_I2S_CON_FS_SHIFT		8		/* mt_soc_pcm_dl1_i2s0.c:188 */
#define AFE_I2S_CON_LOW_JITTER		BIT(12)		/* mt_soc_afe_control.c:3757 */
#define AFE_I2S_CON_PAD_SEL_IO_MUX	BIT(28)		/* mt_soc_afe_control.c:3755 */
#define AFE_I2S_CON_PHASE_SHIFT_FIX	BIT(31)		/* mt_soc_afe_control.c:3754 */

#define AFE_I2S_CON1_LR_SWAP		BIT(31)		/* mt_soc_afe_control.c:1645 */
#define AFE_I2S_CON1_EN			BIT(0)		/* mt_soc_afe_control.c:2018 */

/*
 * Hardware digital gain block 1, used by the FM I2S path
 * (SetHwDigitalGainMode()/SetHwDigitalGainEnable()/SetHwDigitalGain(),
 * mt_soc_afe_control.c:1657-1711).
 */
#define AFE_GAIN1_CON0			0x0410		/* AudDrv_Afe.h:618 */
#define AFE_GAIN1_CON1			0x0414		/* AudDrv_Afe.h:619 */
#define AFE_GAIN1_CONN			0x0420		/* AudDrv_Afe.h:622 */
#define AFE_GAIN1_CUR			0x0424		/* AudDrv_Afe.h:623 */
#define AFE_GAIN1_CONN2			0x0448		/* AudDrv_Afe.h:632 */

/* AFE_GAIN1_CON0 mode field: bits [15:4], written as
 * (sample per step << 8) | (fs << 4) with the 0xfff0 mask of
 * SetHwDigitalGainMode() (mt_soc_afe_control.c:1663,1667); bit 0 is the
 * enable bit of SetHwDigitalGainEnable() (:1686-1688).
 */
#define AFE_GAIN1_CON0_MODE_MASK	GENMASK(15, 4)	/* 0xfff0, :1667 */
#define AFE_GAIN1_CON0_EN		BIT(0)		/* :1688 */

/* I2S format field values (mt_soc_digital_type.h:245-247, 260-263) */
#define AFE_I2S_FORMAT_EIAJ		0
#define AFE_I2S_FORMAT_I2S		1
#define AFE_I2S_WLEN_16BIT		0
#define AFE_I2S_WLEN_32BIT		1

/* Interconnection (AFE_CONNx) */
#define AFE_CONN0			0x0020		/* AudDrv_Afe.h:432 */
#define AFE_CONN1			0x0024		/* AudDrv_Afe.h:433 */
#define AFE_CONN2			0x0028		/* AudDrv_Afe.h:434 */
#define AFE_CONN3			0x002c		/* AudDrv_Afe.h:435 */
#define AFE_CONN4			0x0030		/* AudDrv_Afe.h:436 */
#define AFE_CONN_24BIT			0x006c		/* AudDrv_Afe.h:458 */

/*
 * Bit positions inside the AFE_CONNx registers.  These are entries of the
 * mConnectionReg/mConnectionbits tables in mt_soc_afe_connection.c.
 *
 *	DL1 left  (I05) -> O03 : AFE_CONN1 bit 21
 *	DL1 right (I06) -> O04 : AFE_CONN2 bit 6
 *	DL2 left  (I07) -> O03 : AFE_CONN1 bit 23
 *	DL2 right (I08) -> O04 : AFE_CONN2 bit 8
 *	ADC left  (I03) -> O09 : AFE_CONN3 bit 0
 *	ADC right (I04) -> O10 : AFE_CONN3 bit 3
 */
#define AFE_CONN_DL1_L_TO_O03		BIT(21)		/* mConnectionReg[I05][O03] :293, bits :188 */
#define AFE_CONN_DL1_R_TO_O04		BIT(6)		/* mConnectionReg[I06][O04] :295, bits :190 */
#define AFE_CONN_DL2_L_TO_O03		BIT(23)		/* mConnectionReg[I07][O03] :297, bits :192 */
#define AFE_CONN_DL2_R_TO_O04		BIT(8)		/* mConnectionReg[I08][O04] :299, bits :194 */
#define AFE_CONN_ADC_L_TO_O09		BIT(0)		/* mConnectionReg[I03][O09] :289, bits :184 */
#define AFE_CONN_ADC_R_TO_O10		BIT(3)		/* mConnectionReg[I04][O10] :291, bits :186 */

/*
 * Physical I2S0 output ports O00/O01, fed from the DL1 interface when the
 * I2S0 PCM is used (mt_soc_pcm_dl1_i2s0Dl1.c:514-518).
 */
#define AFE_CONN_DL1_L_TO_O00		BIT(5)		/* mConnectionReg[I05][O00] :293, bits :187 */
#define AFE_CONN_DL1_R_TO_O01		BIT(22)		/* mConnectionReg[I06][O01] :295, bits :189 */

/*
 * Voice and BT SCO routes.  AFE input nodes used here:
 *
 *	I02 - DAI_BT data from the BT controller (MRGIF/DAIBT input)
 *	I03/I04 - AFE ADDA ADC (I2S_IN_ADC) left/right
 *	I09 - MODEM_PCM_2 (external modem) downlink data
 *	I14 - MODEM_PCM_1 (internal modem) downlink data
 *
 * AFE output nodes:
 *
 *	O02 - DAI_BT data to the BT controller
 *	O03/O04 - AFE ADDA DAC (I2S_OUT_DAC) left/right
 *	O07/O08 - MODEM_PCM_2 (external modem) uplink
 *	O11 - MEM_DAI memory interface (BT SCO capture buffer)
 *	O17/O18 - MODEM_PCM_1 (internal modem) uplink
 *
 * Each macro is named after the AFE_CONNx register it belongs to; the
 * (input, output) pair, register and bit come from the mConnectionTable /
 * mConnectionReg / mConnectionbits tables of mt_soc_afe_connection.c.
 */
#define AFE_CONN1_I09_TO_O02		BIT(9)		/* mConnectionReg[I09][O02] :301-302, mConnectionbits[I09][O02] :195-196 */
#define AFE_CONN1_I09_TO_O03		BIT(25)		/* mConnectionReg[I09][O03] :301-302, mConnectionbits[I09][O03] :195-196 */

#define AFE_CONN2_I03_TO_O07		BIT(26)		/* mConnectionReg[I03][O07] :289-290, mConnectionbits[I03][O07] :183-184 */
#define AFE_CONN2_I04_TO_O08		BIT(29)		/* mConnectionReg[I04][O08] :291-292, mConnectionbits[I04][O08] :185-186 */
#define AFE_CONN2_I09_TO_O04		BIT(9)		/* mConnectionReg[I09][O04] :301-302, mConnectionbits[I09][O04] :195-196 */

#define AFE_CONN3_I14_TO_O02		BIT(22)		/* mConnectionReg[I14][O02] :311-312, mConnectionbits[I14][O02] :205-206 */
#define AFE_CONN3_I14_TO_O03		BIT(27)		/* mConnectionReg[I14][O03] :311-312, mConnectionbits[I14][O03] :205-206 */
#define AFE_CONN3_I02_TO_O11		BIT(6)		/* mConnectionReg[I02][O11] :287-288, mConnectionbits[I02][O11] :181-182 */

#define AFE_CONN4_I02_TO_O07		BIT(21)		/* mConnectionReg[I02][O07] :287-288, mConnectionbits[I02][O07] :181-182 */
#define AFE_CONN4_I02_TO_O17		BIT(23)		/* mConnectionReg[I02][O17] :287-288, mConnectionbits[I02][O17] :181-182 */
#define AFE_CONN4_I02_TO_O18		BIT(24)		/* mConnectionReg[I02][O18] :287-288, mConnectionbits[I02][O18] :181-182 */
#define AFE_CONN4_I03_TO_O17		BIT(13)		/* mConnectionReg[I03][O17] :289-290, mConnectionbits[I03][O17] :183-184 */
#define AFE_CONN4_I04_TO_O18		BIT(16)		/* mConnectionReg[I04][O18] :291-292, mConnectionbits[I04][O18] :185-186 */
#define AFE_CONN4_I14_TO_O04		BIT(0)		/* mConnectionReg[I14][O04] :311-312, mConnectionbits[I14][O04] :205-206 */

/*
 * FM I2S routing: the four routes of mtk_pcm_fm_i2s_prepare()
 * (mt_soc_pcm_fm_i2s.c:278-285), disconnected by mtk_pcm_fm_i2s_close()
 * (:246-254).  All of them end in hardware gain block 1, so the registers and
 * bit positions are the mConnectionReg[]/mConnectionbits[] entries of
 * mt_soc_afe_connection.c:
 *
 *	[I00][O13] -> AFE_GAIN1_CONN2 bit 2	(regs :283-284, bits :177-178)
 *	[I01][O14] -> AFE_GAIN1_CONN2 bit 16	(regs :285-286, bits :179-180)
 *	[I10][O03] -> AFE_GAIN1_CONN bit 8	(regs :303-304, bits :197-198)
 *	[I11][O04] -> AFE_GAIN1_CONN bit 10	(regs :305-306, bits :199-200)
 */
#define AFE_CONN_FM_I2S_I00_TO_O13	BIT(2)
#define AFE_CONN_FM_I2S_I01_TO_O14	BIT(16)
#define AFE_CONN_FM_I2S_I10_TO_O03	BIT(8)
#define AFE_CONN_FM_I2S_I11_TO_O04	BIT(10)

/* SetI2SASRCConfig(): bit 30 of AFE_CONN4 selects the ASRC input path (0) or
 * bypasses the ASRC (1) (mt_soc_afe_control.c:2257, 2274).
 */
#define AFE_CONN4_ASRC_BYPASS		BIT(30)

/*
 * mConnectionReg[I02][O08] is -1, so the second channel of the BT ->
 * external-modem uplink (mt_soc_pcm_voice_md2_bt.c:264-266) has no
 * register and is silently dropped by SetConnectionState(); see the
 * md2_bt notes in mt6735-dai-voice.c.
 */

/* AFE_CONN_24BIT: bit N selects 24 bit data for output O<N>
 * (SetoutputConnectionFormat(), mt_soc_afe_control.c:2371-2377).
 */
#define AFE_CONN_24BIT_O03		BIT(3)
#define AFE_CONN_24BIT_O04		BIT(4)
#define AFE_CONN_24BIT_O09		BIT(9)
#define AFE_CONN_24BIT_O10		BIT(10)

/* Memory interfaces */
#define AFE_DL1_BASE			0x0040		/* AudDrv_Afe.h:443 */
#define AFE_DL1_CUR			0x0044		/* AudDrv_Afe.h:444 */
#define AFE_DL1_END			0x0048		/* AudDrv_Afe.h:445 */
#define AFE_DL2_BASE			0x0050		/* AudDrv_Afe.h:454 */
#define AFE_DL2_CUR			0x0054		/* AudDrv_Afe.h:455 */
#define AFE_DL2_END			0x0058		/* AudDrv_Afe.h:456 */
#define AFE_VUL_BASE			0x0080		/* AudDrv_Afe.h:462 */
#define AFE_VUL_END			0x0088		/* AudDrv_Afe.h:463 */
#define AFE_VUL_CUR			0x008c		/* AudDrv_Afe.h:464 */

/* Memory interface monitor registers, read only */
#define AFE_MEMIF_MON0			0x00d0		/* AudDrv_Afe.h:473 */
#define AFE_MEMIF_MON1			0x00d4		/* AudDrv_Afe.h:474 */
#define AFE_MEMIF_MON2			0x00d8		/* AudDrv_Afe.h:475 */
#define AFE_MEMIF_MON4			0x00e0		/* AudDrv_Afe.h:477 */

/*
 * Per-sample fetch format (16 bit vs 24 bit data) for each memory
 * interface, SetMemIfFetchFormatPerSample() mt_soc_afe_control.c:2294-2369.
 */
#define AFE_MEMIF_PBUF_SIZE		0x03d8		/* AudDrv_Afe.h:611 */
#define AFE_MEMIF_PBUF_DL1_FMT_SHIFT	16		/* mt_soc_afe_control.c:2303 */
#define AFE_MEMIF_PBUF_DL2_FMT_SHIFT	18		/* mt_soc_afe_control.c:2315 */
#define AFE_MEMIF_PBUF_VUL_FMT_SHIFT	22		/* mt_soc_afe_control.c:2332 */
#define AFE_MEMIF_PBUF_FMT_MASK		GENMASK(1, 0)

/* AFE_WLEN values used in AFE_MEMIF_PBUF_SIZE (mt_soc_digital_type.h:507-509) */
#define AFE_WLEN_16_BIT			0
#define AFE_WLEN_24_BIT			1

/* ADDA (PMIC codec) digital interface */
#define AFE_ADDA_DL_SRC2_CON0		0x0108		/* AudDrv_Afe.h:481 */
#define AFE_ADDA_DL_SRC2_CON1		0x010c		/* AudDrv_Afe.h:482 */
#define AFE_ADDA_UL_SRC_CON0		0x0114		/* AudDrv_Afe.h:483 */
#define AFE_ADDA_TOP_CON0		0x0120		/* AudDrv_Afe.h:485 */
#define AFE_ADDA_UL_DL_CON0		0x0124		/* AudDrv_Afe.h:486 */
#define AFE_ADDA_NEWIF_CFG0		0x0138		/* AudDrv_Afe.h:490 */
#define AFE_ADDA_NEWIF_CFG1		0x013c		/* AudDrv_Afe.h:491 */
#define AFE_ADDA_PREDIS_CON0		0x0260		/* AudDrv_Afe.h:539 */
#define AFE_ADDA_PREDIS_CON1		0x0264		/* AudDrv_Afe.h:540 */

#define AFE_ADDA_DL_SRC2_CON0_EN	BIT(0)		/* mt_soc_afe_control.c:2017 */
#define AFE_ADDA_UL_SRC_CON0_EN		BIT(0)		/* mt_soc_afe_control.c:1560 */
#define AFE_ADDA_UL_DL_CON0_EN		BIT(0)		/* mt_soc_afe_control.c:2016 */
#define AFE_ADDA_TOP_CON0_EXT_ADC	BIT(0)		/* mt_soc_afe_control.c:1356,1374 */

/* Interrupt controller */
#define AFE_IRQ_MCU_CON			0x03a0		/* AudDrv_Afe.h:584 */
#define AFE_IRQ_MCU_STATUS		0x03a4		/* AudDrv_Afe.h:586 */
#define AFE_IRQ_MCU_CLR			0x03a8		/* AudDrv_Afe.h:588 */
#define AFE_IRQ_MCU_CNT1		0x03ac		/* AudDrv_Afe.h:590 */
#define AFE_IRQ_MCU_CNT2		0x03b0		/* AudDrv_Afe.h:592 */
#define AFE_IRQ_MCU_EN			0x03b4		/* AudDrv_Afe.h:593 */
#define AFE_IRQ1_MCU_CNT_MON		0x03c0		/* AudDrv_Afe.h:599 */
#define AFE_IRQ2_MCU_CNT_MON		0x03c4		/* AudDrv_Afe.h:602 */

/* AFE_IRQ_MCU_CON fields, SetIrqEnable()/SetIrqMcuSampleRate() */
#define AFE_IRQ_MCU_CON_IRQ1_EN		BIT(0)		/* mt_soc_afe_control.c:2105 */
#define AFE_IRQ_MCU_CON_IRQ2_EN		BIT(1)		/* mt_soc_afe_control.c:2105 */
#define AFE_IRQ_MCU_CON_IRQ1_FS_SHIFT	4		/* mt_soc_afe_control.c:2136 */
#define AFE_IRQ_MCU_CON_IRQ2_FS_SHIFT	8		/* mt_soc_afe_control.c:2139 */
#define AFE_IRQ_MCU_CON_FS_MASK		GENMASK(3, 0)

/*
 * BT SCO (DAI_BT) interface, SetDaiBtEnable() mt_soc_afe_control.c:1220-1258
 * and the merge interface it is fed from.
 */
#define AFE_DAIBT_CON0			0x001c		/* AudDrv_Afe.h:430 */
#define AFE_MRGIF_CON			0x003c		/* AudDrv_Afe.h:440 */

#define AFE_DAIBT_CON0_EN		GENMASK(1, 0)	/* mt_soc_afe_control.c:1229,1238 */
#define AFE_DAIBT_CON0_DATA_READY	BIT(3)		/* mt_soc_afe_control.c:1228 */
#define AFE_DAIBT_CON0_MODE_SHIFT	9		/* mt_soc_afe_control.c:1225 */
#define AFE_DAIBT_CON0_USE_MERGE	BIT(12)		/* mt_soc_afe_control.c:1227 */

/* AFE_DAIBT_CON0 mode field values (mt_soc_digital_type.h:387-390) */
#define AFE_DAIBT_MODE_8K		0		/* Soc_Aud_DATBT_MODE_Mode8K */
#define AFE_DAIBT_MODE_16K		1		/* Soc_Aud_DATBT_MODE_Mode16K */

/* AFE_MRGIF_CON fields, SetDaiBtEnable()/SetMrgI2SEnable() */
#define AFE_MRGIF_CON_EN		BIT(0)		/* mt_soc_afe_control.c:1234 */
#define AFE_MRGIF_CON_I2S_EN		BIT(16)		/* mt_soc_afe_control.c:1233 */
#define AFE_MRGIF_CON_FS_SHIFT		20		/* mt_soc_afe_control.c:1231 */
#define AFE_MRGIF_CON_FS_MASK		GENMASK(3, 0)

/*
 * MEM_DAI memory interface: the BT SCO capture buffer used by
 * mt_soc_pcm_bt_dai.c (SetDAIBuffer(), :228-240; pointer, :175-210).
 */
#define AFE_DAI_BASE			0x0090		/* AudDrv_Afe.h:465 */
#define AFE_DAI_END			0x0098		/* AudDrv_Afe.h:466 */
#define AFE_DAI_CUR			0x009c		/* AudDrv_Afe.h:467 */

/*
 * AFE_DAC_CON0 additions.  SetMemoryPathEnable() enables the digital block
 * with bit (block + 1) (mt_soc_afe_control.c:1995-1997); block numbers come
 * from the Soc_Aud_Digital_Block enum (mt_soc_digital_type.h:48-82):
 * MEM_DAI = 3, I2S_OUT_DAC = 12 (mt_soc_digital_type.h:68), I2S_IN_ADC = 14
 * and I2S_IN_2 = 17 (mt_soc_digital_type.h:74).
 */
#define AFE_DAC_CON0_DAI_FS_SHIFT	24		/* mt_soc_afe_control.c:1099-1112 */
#define AFE_DAC_CON0_DAI_FS_MASK	GENMASK(1, 0)
#define AFE_DAC_CON0_I2S_OUT_DAC_EN	BIT(13)		/* block 12 + 1 */
#define AFE_DAC_CON0_I2S_IN_ADC_EN	BIT(15)		/* block 14 + 1 */
#define AFE_DAC_CON0_I2S_IN_2_EN	BIT(18)		/* block 17 + 1 */

/* MEM_DAI rate field values, SetSampleRate(MEM_DAI) :1099-1112 */
#define AFE_DAI_FS_8K			0
#define AFE_DAI_FS_16K			1

/* MEM_DAI fetch format, SetMemIfFetchFormatPerSample() :2343-2346 */
#define AFE_MEMIF_PBUF_DAI_FMT_SHIFT	24

/*
 * Modem PCM interfaces (AudDrv_Afe.h:670-672).  The internal modem uses
 * PCM2_INTF_CON and the external modem PCM_INTF_CON, see the comments in
 * SetModemPcmEnable() mt_soc_afe_control.c:1845-1880.
 */
#define PCM_INTF_CON			0x0530
#define PCM_INTF_CON2			0x0538
#define PCM2_INTF_CON			0x053c

/* PCM_INTF_CON bits used by SetModemPcmEnable() :1868,1870 and the async
 * FIFO selection read back at :1861
 */
#define PCM_INTF_CON_EN			BIT(0)		/* :1868,1870 */
#define PCM_INTF_CON_BYP_ASRC		BIT(6)		/* :1832 */

/* PCM2_INTF_CON bit, SetModemPcmEnable() :1854 */
#define PCM2_INTF_CON_EN		BIT(0)

/* PCM_INTF_CON2 bit, SetModemPcmEnable() :1859-1860 */
#define PCM_INTF_CON2_MD3_SEL		BIT(8)

/* PCM field values (mt_soc_digital_type.h:313-363) */
#define PCM_TX_LCH_RPT_NO_REPEAT	0	/* Soc_Aud_TX_LCH_RPT_TX_LCH_NO_REPEAT */
#define PCM_VBT_16K_DISABLE		0	/* Soc_Aud_VBT_16K_MODE_VBT_16K_MODE_DISABLE */
#define PCM_EXT_MODEM_INTERNAL		0	/* Soc_Aud_EXT_MODEM_MODEM_2_USE_INTERNAL_MODEM */
#define PCM_SYNC_ONE_BCK_CYCLE		0	/* Soc_Aud_PCM_SYNC_TYPE_BCK_CYCLE_SYNC */
#define PCM_BT_MODE_DUAL_MIC_ON_TX	0	/* Soc_Aud_BT_MODE_DUAL_MIC_ON_TX */
#define PCM_AFIFO_ASRC			0	/* Soc_Aud_BYPASS_SRC_SLAVE_USE_ASRC */
#define PCM_AFIFO_ASYNC_FIFO		1	/* Soc_Aud_BYPASS_SRC_SLAVE_USE_ASYNC_FIFO */
#define PCM_CLOCK_SLAVE			1	/* Soc_Aud_PCM_CLOCK_SOURCE_SALVE_MODE */
#define PCM_WLEN_16BIT			0	/* Soc_Aud_PCM_WLEN_LEN_PCM_16BIT */
#define PCM_MODE_8K			0	/* Soc_Aud_PCM_MODE_PCM_MODE_8K */
#define PCM_MODE_16K			1	/* Soc_Aud_PCM_MODE_PCM_MODE_16K */
#define PCM_FMT_MODE_B			3	/* Soc_Aud_PCM_FMT_PCM_MODE_B */

/* ASRC blocks programmed by SetModemPcmConfig()/SetModemPcmEnable(). */
#define AFE_ASRC_CON0			0x0500		/* AudDrv_Afe.h:657 */
#define AFE_ASRC_CON1			0x0504		/* AudDrv_Afe.h:658 */
#define AFE_ASRC_CON2			0x0508		/* AudDrv_Afe.h:659 */
#define AFE_ASRC_CON3			0x050c		/* AudDrv_Afe.h:660 */
#define AFE_ASRC_CON4			0x0510		/* AudDrv_Afe.h:661 */
#define AFE_ASRC_CON6			0x0518		/* AudDrv_Afe.h:663 */
#define AFE_ASRC_CON7			0x051c		/* AudDrv_Afe.h:664 */
#define AFE_ASRC4_CON0			0x06c0		/* AudDrv_Afe.h:685 */
#define AFE_ASRC4_CON1			0x06c4		/* AudDrv_Afe.h:686 */
#define AFE_ASRC4_CON2			0x06c8		/* AudDrv_Afe.h:687 */
#define AFE_ASRC4_CON3			0x06cc		/* AudDrv_Afe.h:688 */
#define AFE_ASRC4_CON4			0x06d0		/* AudDrv_Afe.h:689 */
#define AFE_ASRC4_CON6			0x06d8		/* AudDrv_Afe.h:691 */
#define AFE_ASRC4_CON7			0x06dc		/* AudDrv_Afe.h:692 */
#define AFE_ASRC2_CON1			0x0704		/* AudDrv_Afe.h:702 */
#define AFE_ASRC2_CON2			0x0708		/* AudDrv_Afe.h:703 */
#define AFE_ASRC2_CON3			0x070c		/* AudDrv_Afe.h:704 */
#define AFE_ASRC2_CON4			0x0710		/* AudDrv_Afe.h:705 */
#define AFE_ASRC2_CON7			0x071c		/* AudDrv_Afe.h:708 */
#define AFE_ASRC3_CON1			0x0744		/* AudDrv_Afe.h:717 */
#define AFE_ASRC3_CON2			0x0748		/* AudDrv_Afe.h:718 */
#define AFE_ASRC3_CON3			0x074c		/* AudDrv_Afe.h:719 */
#define AFE_ASRC3_CON4			0x0750		/* AudDrv_Afe.h:720 */
#define AFE_ASRC3_CON7			0x075c		/* AudDrv_Afe.h:723 */

/* AFE_IRQ_MCU_STATUS / AFE_IRQ_MCU_CLR bits (AudDrv_Kernel.h:246-251) */
#define AFE_IRQ_STATUS_IRQ1		BIT(0)
#define AFE_IRQ_STATUS_IRQ2		BIT(1)
#define AFE_IRQ_STATUS_IRQ3		BIT(2)
#define AFE_IRQ_STATUS_IRQ4		BIT(3)
#define AFE_IRQ_STATUS_IRQ5		BIT(4)
#define AFE_IRQ_STATUS_IRQ7		BIT(6)
#define AFE_IRQ_STATUS_ALL		GENMASK(7, 0)

#endif /* _MT6735_AFE_REGS_H_ */
