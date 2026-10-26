// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE clock and pin control helpers.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Clk.c and AudDrv_Gpio.c.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>

#include "mt6735-afe-common.h"

/*
 * Clock names are taken verbatim from the ROM audio node
 * (rom_dtb_0.dts, mt_soc_dl1_pcm@11220000 clock-names) which is the same
 * list AudDrv_Clk.c:137-157 looks up with devm_clk_get().
 */
static const char * const mt6735_afe_clk_names[MT6735_AFE_CLK_NUM] = {
	[MT6735_AFE_CLK_AFE]			= "aud_afe_clk",
	[MT6735_AFE_CLK_I2S]			= "aud_i2s_clk",
	[MT6735_AFE_CLK_DAC]			= "aud_dac_clk",
	[MT6735_AFE_CLK_DAC_PREDIS]		= "aud_dac_predis_clk",
	[MT6735_AFE_CLK_ADC]			= "aud_adc_clk",
	[MT6735_AFE_CLK_TML]			= "aud_tml_clk",
	[MT6735_AFE_CLK_APLL22M]		= "aud_apll22m_clk",
	[MT6735_AFE_CLK_APLL24M]		= "aud_apll24m_clk",
	[MT6735_AFE_CLK_APLL1_TUNER]		= "aud_apll1_tuner_clk",
	[MT6735_AFE_CLK_APLL2_TUNER]		= "aud_apll2_tuner_clk",
	[MT6735_AFE_CLK_INFRA]			= "aud_infra_clk",
	[MT6735_AFE_CLK_MUX1]			= "aud_mux1_clk",
	[MT6735_AFE_CLK_MUX2]			= "aud_mux2_clk",
	[MT6735_AFE_CLK_TOP_AD_APLL1]		= "top_ad_apll1_clk",
	[MT6735_AFE_CLK_TOP_WHPLL_AUDIO]	= "top_whpll_audio_clk",
	[MT6735_AFE_CLK_TOP_MUX_AUDIO]		= "top_mux_audio",
	[MT6735_AFE_CLK_TOP_MUX_AUDIO_INT]	= "top_mux_audio_int",
	[MT6735_AFE_CLK_TOP_SYS_PLL1_D4]	= "top_sys_pll1_d4",
	[MT6735_AFE_CLK_APMIXED_APLL1]		= "apmixed_apll1_clk",
	[MT6735_AFE_CLK_APMIXED_APLL2]		= "apmixed_apll2_clk",
	[MT6735_AFE_CLK_TOP_CLK26M]		= "top_clk26m_clk",
};

int mt6735_afe_init_clk(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int i;

	for (i = 0; i < MT6735_AFE_CLK_NUM; i++)
		priv->clks[i].id = mt6735_afe_clk_names[i];

	/*
	 * The audiosys clock gate provider is not part of this driver; the
	 * clocks are optional so that the AFE can bind on a device tree where
	 * the provider is not (yet) present.  AudDrv_Clk.c:171 uses a plain
	 * devm_clk_get() because the provider is always there on Android.
	 */
	return devm_clk_bulk_get_optional(afe->dev, MT6735_AFE_CLK_NUM,
					  priv->clks);
}

int mt6735_afe_enable_clk(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int ret;

	/* AudDrv_Clk_On() enables infra/afe/dac/dac_predis, AudDrv_Clk.c:437-470 */
	ret = clk_bulk_prepare_enable(MT6735_AFE_CLK_NUM, priv->clks);
	if (ret) {
		dev_err(afe->dev, "failed to enable audio clocks: %d\n", ret);
		return ret;
	}

	/*
	 * "must set, system will default set bit14 to 0" - the AFE register
	 * block is not accessible before this bit is set (AudDrv_Clk.c:265,
	 * and the same write in AudDrv_Clk_On(), AudDrv_Clk.c:467).
	 */
	return regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				  AUDIO_TOP_CON0_AFE_POWER_ON,
				  AUDIO_TOP_CON0_AFE_POWER_ON);
}

void mt6735_afe_disable_clk(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	/*
	 * AudDrv_Clk_Off() clears all pending IRQ status before gating the
	 * clocks (AudDrv_Clk.c:511).
	 */
	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_STATUS_ALL);

	clk_bulk_disable_unprepare(MT6735_AFE_CLK_NUM, priv->clks);
}

static struct pinctrl_state *mt6735_afe_lookup_state(struct mtk_base_afe *afe,
						     const char *name)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	struct pinctrl_state *state;

	if (!priv->pinctrl)
		return NULL;

	state = pinctrl_lookup_state(priv->pinctrl, name);
	if (IS_ERR(state)) {
		dev_dbg(afe->dev, "pinctrl state %s not available\n", name);
		return NULL;
	}

	return state;
}

int mt6735_afe_init_pinctrl(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	/*
	 * AudDrv_GPIO_probe() (AudDrv_Gpio.c:104-137) resolves every state
	 * used by the audio driver.  Here only the states owned by the AFE are
	 * looked up; the codec/amplifier states belong to the machine driver.
	 */
	priv->pinctrl = devm_pinctrl_get(afe->dev);
	if (IS_ERR(priv->pinctrl)) {
		dev_dbg(afe->dev, "no audio pinctrl, continuing without it\n");
		priv->pinctrl = NULL;
		return 0;
	}

	priv->pins_default = mt6735_afe_lookup_state(afe, "default");
	priv->pins_pmic_mode0 = mt6735_afe_lookup_state(afe, "audpmicclk-mode0");
	priv->pins_pmic_mode1 = mt6735_afe_lookup_state(afe, "audpmicclk-mode1");
	priv->pins_i2s_mode0 = mt6735_afe_lookup_state(afe, "audi2s1-mode0");
	priv->pins_i2s_mode1 = mt6735_afe_lookup_state(afe, "audi2s1-mode1");

	if (priv->pins_default)
		return pinctrl_select_state(priv->pinctrl, priv->pins_default);

	return 0;
}

static int mt6735_afe_select_pins(struct mtk_base_afe *afe,
				  struct pinctrl_state *state)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	if (!priv->pinctrl || !state)
		return 0;

	return pinctrl_select_state(priv->pinctrl, state);
}

/*
 * The PMIC audio bus pins (AUD_CLK_MOSI/AUD_DAT_MISO/AUD_DAT_MOSI) are muxed
 * to their audio function by "audpmicclk-mode1" and returned to GPIO by
 * "audpmicclk-mode0" (downstream DTS AUD_pins_pmicclk_mode0/mode1).
 * AudDrv_GPIO_PMIC_Select() therefore selects mode1 when the AFE path is on
 * and mode0 when it is off (AudDrv_Gpio.c:215-236), which is what
 * EnableAfe() uses (mt_soc_afe_control.c:993,1017).
 */
int mt6735_afe_select_pmic_pins(struct mtk_base_afe *afe, bool active)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	return mt6735_afe_select_pins(afe,
				      active ? priv->pins_pmic_mode1 :
					       priv->pins_pmic_mode0);
}

int mt6735_afe_select_i2s_pins(struct mtk_base_afe *afe, bool active)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	return mt6735_afe_select_pins(afe,
				      active ? priv->pins_i2s_mode1 :
					       priv->pins_i2s_mode0);
}
