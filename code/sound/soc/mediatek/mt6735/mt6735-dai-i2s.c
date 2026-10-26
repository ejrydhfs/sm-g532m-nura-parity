// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE I2S0 back-end DAI.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/:
 *   mt_soc_afe_control.c  - Enable4pin_I2S0_I2S3(), SetCLkMclk()
 *   mt_soc_pcm_dl1_i2s0.c - I2S0 register programming
 *   AudDrv_Gpio.c         - audi2s1-mode0/mode1 pin muxing
 *
 * The MCLK/BCLK dividers live in the audiosys clock controller
 * (AUDIO_CLK_AUDDIV_0/1 at 0x1021xxxx, SetCLkMclk()/SetCLkBclk(),
 * mt_soc_afe_control.c:751-813) and are therefore programmed by the clock
 * provider, not by this driver.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/* Default MCLK/sample rate ratio, MCLKFS in mt_soc_afe_control.c:137 */
#define MT6735_AFE_I2S_MCLK_FS	128

static int mt6735_afe_i2s_startup(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int ret;

	/*
	 * AFE_I2S_CON (0x0018) is shared with the FM I2S back end, which takes
	 * the same register for the 2nd I2S input (Set2ndI2SIn(),
	 * mt_soc_afe_control.c:2231).  Refuse the I2S0 stream while the FM
	 * path owns it instead of overwriting its programming;
	 * mt6735-dai-fm-i2s.c has the symmetric check.
	 */
	mutex_lock(&priv->i2s_con_lock);
	if (priv->fm_i2s_active) {
		mutex_unlock(&priv->i2s_con_lock);
		return -EBUSY;
	}
	priv->i2s0_active = true;
	mutex_unlock(&priv->i2s_con_lock);

	ret = pm_runtime_get_sync(afe->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		goto err_release;
	}

	snd_soc_set_runtime_hwparams(substream, afe->mtk_afe_hardware);

	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		goto err_release;
	}

	return 0;

err_release:
	mutex_lock(&priv->i2s_con_lock);
	priv->i2s0_active = false;
	mutex_unlock(&priv->i2s_con_lock);

	return ret;
}

static void mt6735_afe_i2s_shutdown(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	mt6735_afe_select_i2s_pins(afe, false);
	pm_runtime_put(afe->dev);

	mutex_lock(&priv->i2s_con_lock);
	priv->i2s0_active = false;
	mutex_unlock(&priv->i2s_con_lock);
}

/*
 * Translate the ASoC DAI format into the AFE_I2S_CON format fields.
 * AFE_I2S_CON bit 2 selects slave mode ("clock from : slave mode",
 * mt_soc_pcm_hdmi.c:889), bit 5 inverts LRCK and bit 3 selects the I2S
 * (rather than EIAJ) framing (mt_soc_afe_control.c:3758-3763).
 */
static int mt6735_afe_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int con;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		priv->i2s_slave = false;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		priv->i2s_slave = true;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		con = 0;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		con = AFE_I2S_CON_LRCK_INV;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Fixed settings for the four wire I2S0 interface, from
	 * Enable4pin_I2S0_I2S3() (mt_soc_afe_control.c:3749-3763): phase
	 * shift fix, data taken from the IO mux, low jitter clock and I2S
	 * framing.
	 */
	con |= AFE_I2S_CON_PHASE_SHIFT_FIX |
	       AFE_I2S_CON_PAD_SEL_IO_MUX |
	       AFE_I2S_CON_LOW_JITTER |
	       (AFE_I2S_FORMAT_I2S << AFE_I2S_CON_FMT_SHIFT);
	if (priv->i2s_slave)
		con |= AFE_I2S_CON_SLAVE;

	priv->i2s_con = con;

	return 0;
}

/*
 * MCLK is routed through the audiosys clock mux; only its value is validated
 * here so that hw_params can check it against the stream rate the same way
 * SetCLkMclk() derives the divider (mt_soc_afe_control.c:751-786).
 */
static int mt6735_afe_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				     unsigned int freq, int dir)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	if (clk_id != 0)
		return -EINVAL;
	if (dir != SND_SOC_CLOCK_OUT)
		return -EINVAL;

	priv->i2s_sysclk = freq;

	return 0;
}

static int mt6735_afe_i2s_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int con, wlen;
	int fs, ret;

	fs = mt6735_afe_rate_to_fs(rate);
	if (fs < 0)
		return -EINVAL;

	if (channels < MT6735_AFE_CHANNELS_MIN ||
	    channels > MT6735_AFE_CHANNELS_MAX)
		return -EINVAL;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		wlen = AFE_I2S_WLEN_16BIT;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		wlen = AFE_I2S_WLEN_32BIT;
		break;
	default:
		return -EINVAL;
	}

	if (priv->i2s_sysclk) {
		if (priv->i2s_sysclk % rate)
			return -EINVAL;
		if (priv->i2s_sysclk < MT6735_AFE_I2S_MCLK_FS * rate)
			dev_dbg(afe->dev,
				"I2S0 MCLK %u Hz below %u x %u Hz\n",
				priv->i2s_sysclk, MT6735_AFE_I2S_MCLK_FS, rate);
	}

	con = priv->i2s_con;
	if (!con)
		con = AFE_I2S_CON_PHASE_SHIFT_FIX |
		      AFE_I2S_CON_PAD_SEL_IO_MUX |
		      AFE_I2S_CON_LOW_JITTER |
		      (AFE_I2S_FORMAT_I2S << AFE_I2S_CON_FMT_SHIFT);

	con |= (fs << AFE_I2S_CON_FS_SHIFT) |
	       (wlen << AFE_I2S_CON_WLEN_SHIFT);

	/* AudDrv_GPIO_I2S_Select(true), AudDrv_Gpio.c:238-259 */
	ret = mt6735_afe_select_i2s_pins(afe, true);
	if (ret)
		return ret;

	/*
	 * Bring-up sequence of Enable4pin_I2S0_I2S3(),
	 * mt_soc_afe_control.c:3749-3782: soft reset, gate the I2S0 clock,
	 * program the interface, ungate, release the reset.
	 */
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON1,
			   AUDIO_TOP_CON1_I2S_SOFT_RST,
			   AUDIO_TOP_CON1_I2S_SOFT_RST);
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON1, AUDIO_TOP_CON1_I2S0_CG,
			   AUDIO_TOP_CON1_I2S0_CG);
	regmap_write(afe->regmap, AFE_I2S_CON, con);
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON1, AUDIO_TOP_CON1_I2S0_CG,
			   0);
	udelay(200);
	regmap_update_bits(afe->regmap, AUDIO_TOP_CON1,
			   AUDIO_TOP_CON1_I2S_SOFT_RST, 0);

	return 0;
}

static int mt6735_afe_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/*
		 * When DL1 feeds I2S0 (I2S0DL1 link) the DL1 samples are also
		 * routed to the physical O00/O01 ports
		 * (mt_soc_pcm_dl1_i2s0Dl1.c:514-518).
		 */
		if (priv->memif_enabled[MT6735_AFE_MEMIF_DL1])
			regmap_update_bits(afe->regmap, AFE_CONN0,
					   AFE_CONN_DL1_L_TO_O00 |
					   AFE_CONN_DL1_R_TO_O01,
					   AFE_CONN_DL1_L_TO_O00 |
					   AFE_CONN_DL1_R_TO_O01);

		return regmap_update_bits(afe->regmap, AFE_I2S_CON,
					  AFE_I2S_CON_EN, AFE_I2S_CON_EN);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		regmap_update_bits(afe->regmap, AFE_I2S_CON, AFE_I2S_CON_EN, 0);
		regmap_update_bits(afe->regmap, AFE_CONN0,
				   AFE_CONN_DL1_L_TO_O00 |
				   AFE_CONN_DL1_R_TO_O01, 0);

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6735_afe_i2s_ops = {
	.startup = mt6735_afe_i2s_startup,
	.shutdown = mt6735_afe_i2s_shutdown,
	.set_fmt = mt6735_afe_i2s_set_fmt,
	.set_sysclk = mt6735_afe_i2s_set_sysclk,
	.hw_params = mt6735_afe_i2s_hw_params,
	.trigger = mt6735_afe_i2s_trigger,
};

const struct snd_soc_dai_driver mt6735_afe_i2s_dai = {
	.name = "mt-soc-i2s0dai-driver",
	.id = MT6735_AFE_DAI_I2S0,
	.ops = &mt6735_afe_i2s_ops,
	.playback = {
		.stream_name = "I2S0 Playback",
		.channels_min = MT6735_AFE_CHANNELS_MIN,
		.channels_max = MT6735_AFE_CHANNELS_MAX,
		.rates = MT6735_AFE_RATES,
		.rate_min = MT6735_AFE_RATE_MIN,
		.rate_max = MT6735_AFE_RATE_MAX,
		.formats = MT6735_AFE_FORMATS,
	},
	.capture = {
		.stream_name = "I2S0 Capture",
		.channels_min = MT6735_AFE_CHANNELS_MIN,
		.channels_max = MT6735_AFE_CHANNELS_MAX,
		.rates = MT6735_AFE_RATES,
		.rate_min = MT6735_AFE_RATE_MIN,
		.rate_max = MT6735_AFE_RATE_MAX,
		.formats = MT6735_AFE_FORMATS,
	},
};
