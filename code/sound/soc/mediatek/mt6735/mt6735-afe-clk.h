/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE clock and pin control helpers.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Clk.c and AudDrv_Gpio.c.
 */

#ifndef _MT6735_AFE_CLK_H_
#define _MT6735_AFE_CLK_H_

struct mtk_base_afe;

int mt6735_afe_init_clk(struct mtk_base_afe *afe);
int mt6735_afe_enable_clk(struct mtk_base_afe *afe);
void mt6735_afe_disable_clk(struct mtk_base_afe *afe);

int mt6735_afe_init_pinctrl(struct mtk_base_afe *afe);
int mt6735_afe_select_pmic_pins(struct mtk_base_afe *afe, bool active);
int mt6735_afe_select_i2s_pins(struct mtk_base_afe *afe, bool active);

#endif /* _MT6735_AFE_CLK_H_ */
