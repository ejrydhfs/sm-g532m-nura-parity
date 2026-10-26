/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6328 PMIC accessory detection (ACCDET) register and bit definitions.
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/accdet/mt6735/reg_accdet.h (MediaTek, GPL-2.0).
 *
 * All addresses are absolute MT6328 PMIC register addresses and are accessed
 * through the parent PMIC regmap (16-bit register/16-bit value).
 */

#ifndef __MT6328_ACCDET_REGS_H
#define __MT6328_ACCDET_REGS_H

/* Register addresses, downstream reg_accdet.h:4-45. */
#define ACCDET_TOP_RST_ACCDET		0x0298	/* reg_accdet.h:5 */
#define ACCDET_TOP_RST_ACCDET_SET	0x029a	/* reg_accdet.h:6 */
#define ACCDET_TOP_RST_ACCDET_CLR	0x029c	/* reg_accdet.h:7 */

#define ACCDET_INT_CON			0x02ac	/* reg_accdet.h:9 */
#define ACCDET_INT_CON_SET		0x02ae	/* reg_accdet.h:10 */
#define ACCDET_INT_CON_CLR		0x02b0	/* reg_accdet.h:11 */

#define ACCDET_TOP_CKPDN		0x0248	/* reg_accdet.h:16 */
#define ACCDET_TOP_CKPDN_SET		0x024a	/* reg_accdet.h:17 */
#define ACCDET_TOP_CKPDN_CLR		0x024c	/* reg_accdet.h:18 */

#define ACCDET_RSV			0x0f14	/* reg_accdet.h:20 */
#define ACCDET_CTRL			0x0f16	/* reg_accdet.h:21 */
#define ACCDET_STATE_SWCTRL		0x0f18	/* reg_accdet.h:22 */
#define ACCDET_PWM_WIDTH		0x0f1a	/* reg_accdet.h:23 */
#define ACCDET_PWM_THRESH		0x0f1c	/* reg_accdet.h:24 */
#define ACCDET_EN_DELAY_NUM		0x0f1e	/* reg_accdet.h:25 */
#define ACCDET_DEBOUNCE0		0x0f20	/* reg_accdet.h:26 */
#define ACCDET_DEBOUNCE1		0x0f22	/* reg_accdet.h:27 */
#define ACCDET_DEBOUNCE3		0x0f26	/* reg_accdet.h:29 */
#define ACCDET_DEBOUNCE4		0x0f28	/* reg_accdet.h:30 */
#define ACCDET_IRQ_STS			0x0f2c	/* reg_accdet.h:32 */
#define ACCDET_STATE_RG			0x0f30	/* reg_accdet.h:34 */

#define ACCDET_AUXADC_CTL		0x0e88	/* reg_accdet.h:40 */
#define ACCDET_AUXADC_CTL_SET		0x0e8a	/* reg_accdet.h:41 */
#define ACCDET_AUXADC_REG		0x0e0a	/* reg_accdet.h:42 */
#define ACCDET_AUXADC_AUTO_SPL		0x0eca	/* reg_accdet.h:43 */

/* Downstream calls this ACCDET_ADC_REG (reg_accdet.h:45); it is
 * MT6328_AUDENC_ANA_CON9 (upmu_hw.h:612) and holds the mic bias fields.
 */
#define ACCDET_ADC_REG			0x0d00

/* Register bits, downstream reg_accdet.h:47-131. */
#define ACCDET_AUXADC_AUTO_SET		BIT(0)	/* reg_accdet.h:49 */
#define ACCDET_DATA_READY		BIT(15)	/* reg_accdet.h:50 */
#define ACCDET_CH_REQ_EN		BIT(5)	/* reg_accdet.h:51 */
#define ACCDET_DATA_MASK		GENMASK(11, 0)	/* reg_accdet.h:52 */

#define ACCDET_INPUT_MICP		BIT(3)	/* reg_accdet.h:62 */

#define ACCDET_ENABLE			BIT(0)	/* reg_accdet.h:89 */
#define ACCDET_DISABLE			0	/* reg_accdet.h:90 */

#define ACCDET_RESET_SET		BIT(4)	/* reg_accdet.h:92 */
#define ACCDET_RESET_CLR		BIT(4)	/* reg_accdet.h:93 */

#define ACCDET_IRQ_CLR_BIT		0x100	/* reg_accdet.h:95 */
#define ACCDET_IRQ_STATUS_BIT		BIT(0)	/* reg_accdet.h:98 */
/* Bit 15 of ACCDET_IRQ_STS is preserved on every downstream
 * read-modify-write (accdet.c:883, 889).
 */
#define ACCDET_IRQ_STS_RSVD		BIT(15)

#define ACCDET_RG_IRQ_SET		BIT(12)	/* reg_accdet.h:108 */
#define ACCDET_RG_IRQ_CLR		BIT(12)	/* reg_accdet.h:109 */

#define ACCDET_RG_CLK_SET		BIT(9)	/* reg_accdet.h:121 */
#define ACCDET_RG_CLK_CLR		BIT(9)	/* reg_accdet.h:122 */

#define ACCDET_SWCTRL_EN		0x07	/* reg_accdet.h:128 */
#define ACCDET_SWCTRL_IDLE_EN		(0x07 << 4)	/* reg_accdet.h:143 */
#define ACCDET_DE4			0x42	/* 2 ms, reg_accdet.h:131 */

/* ACCDET_STATE_RG[7:6] = A/B comparator state (accdet.c:944). */
#define ACCDET_AB_MASK			GENMASK(7, 6)
#define ACCDET_AB_SHIFT			6

/* A/B comparator codes (accdet.c:35-38). */
#define ACCDET_AB_3POLE			0x0	/* TYPE_AB_00 */
#define ACCDET_AB_4POLE			0x1	/* TYPE_AB_01 */
#define ACCDET_AB_EXTERNAL		0x2	/* TYPE_AB_10 */
#define ACCDET_AB_PLUG_OUT		0x3	/* TYPE_AB_11 */

/* MT6328_AUDENC_ANA_CON9 (ACCDET_ADC_REG) mic bias fields, upmu_hw.h:7106-7117. */
#define ACCDET_AUDMICBIAS1DCSWPEN	BIT(5)	/* upmu_hw.h:7108 */
#define ACCDET_AUDMICBIASVREF_SHIFT	8	/* upmu_hw.h:7117 */
#define ACCDET_AUDMICBIASVREF_MASK	GENMASK(10, 8)	/* upmu_hw.h:7116 */

/* Test mode bit that raises the mic bias reference to 2.8 V (accdet.c:1457). */
#define ACCDET_ADC_2V8_TEST		BIT(7)

/* Timing and conversion constants. */
#define ACCDET_IRQ_TIMEOUT_NS		0x61a80		/* 400 us, accdet.h:24 */
#define ACCDET_BUTTON_PRESS_DEBOUNCE	0x400		/* accdet.c:69 */
#define ACCDET_KEY_RELEASE_DEBOUNCE	0x800		/* accdet.c:1071 */
#define ACCDET_ADC_FULL_SCALE_MV	1800		/* accdet.c:296 */
#define ACCDET_ADC_FULL_SCALE		4096		/* accdet.c:296 */

/* Downstream polls the ready bit without a bound (accdet.c:288-289); the
 * timeout below only prevents a stuck interrupt thread. [INFERENCE]
 */
#define ACCDET_ADC_POLL_US		100
#define ACCDET_ADC_TIMEOUT_US		100000

/* PMIC OTP/efuse read sequence used for the headset ADC offset,
 * downstream power/mt6735/pmic.c:5155-5195.  Register names from upmu_hw.h.
 */
#define ACCDET_CKHWEN_CON0_CLR		0x027c	/* upmu_hw.h:115 */
#define ACCDET_CKPDN_CON3_CLR		0x0252	/* upmu_hw.h:94 */
#define ACCDET_CKPDN_CON3_SET		0x0250	/* upmu_hw.h:93 */
#define ACCDET_CKHWEN_CON0_SET		0x027a	/* upmu_hw.h:114 */
#define ACCDET_OTP_CON0			0x0c00	/* upmu_hw.h:484 */
#define ACCDET_OTP_CON8			0x0c10	/* upmu_hw.h:492 */
#define ACCDET_OTP_CON11		0x0c16	/* upmu_hw.h:495 */
#define ACCDET_OTP_CON12		0x0c18	/* upmu_hw.h:496 */
#define ACCDET_OTP_CON13		0x0c1a	/* upmu_hw.h:497 */

#define ACCDET_OTP_ENGINE_EN		BIT(0)	/* OTP_CON11, pmic.c:5166 */
#define ACCDET_OTP_READ_TRIGGER		BIT(0)	/* OTP_CON8,  pmic.c:5176 */
#define ACCDET_OTP_READY		BIT(0)	/* OTP_CON13, pmic.c:5182 */
#define ACCDET_OTP_ROW_MASK		GENMASK(5, 1)	/* pmic.c:5172 */
#define ACCDET_OTP_ROW_SHIFT		1
#define ACCDET_OTP_ROW		0x10	/* accdet.c:1362 */
#define ACCDET_OTP_READY_POLL_US	1000
#define ACCDET_OTP_READY_TIMEOUT_US	20000	/* [INFERENCE] downstream loops forever */

#endif /* __MT6328_ACCDET_REGS_H */
