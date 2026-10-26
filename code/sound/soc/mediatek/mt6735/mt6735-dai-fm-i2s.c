// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE FM I2S back-end DAI.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/:
 *   mt_soc_pcm_fm_i2s.c     - mtk_pcm_fm_i2s_open()/close()/prepare(),
 *                             mtk_fm_i2s_hardware, mfm_i2s_Volume
 *   mt_soc_afe_control.c    - Set2ndI2SIn(), Set2ndI2SInEnable(),
 *                             SetI2SASRCConfig(), SetHwDigitalGainMode(),
 *                             SetHwDigitalGainEnable(), SetHwDigitalGain(),
 *                             SetI2SDacOut(), SetMemoryPathEnable()
 *   mt_soc_afe_connection.c - mConnectionReg[]/mConnectionbits[] FM routes
 *
 * The FM audio path has no AP memory interface: the MT6627 tuner feeds the AFE
 * through the 2nd I2S input from CONNSYS (I2S_IN_2, digital block 17), through
 * hardware digital gain 1, to the PMIC codec DAC (I2S_OUT_DAC, digital block
 * 12).  The stream therefore never advances its ALSA pointer, exactly like the
 * I2S0 and voice back ends.
 *
 * AFE_I2S_CON (0x0018) is shared with the I2S0 four wire path: this DAI and
 * mt6735-dai-i2s.c refuse to run at the same time (-EBUSY) so neither can
 * overwrite the other's register programming.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/* mfm_i2s_Volume, mt_soc_pcm_fm_i2s.c:73 */
#define MT6735_AFE_FM_I2S_GAIN		0x10000
/* SamplePerStep handed to SetHwDigitalGainMode(), mt_soc_pcm_fm_i2s.c:288-289 */
#define MT6735_AFE_FM_I2S_GAIN_STEP	0x40

/*
 * soc_fm_supported_sample_rates[] is { 32000, 44100, 48000 }
 * (mt_soc_pcm_common.h:210-212).  Downstream uses 44.1/48 kHz for the stream
 * and resamples the fixed 32 kHz 2nd I2S input with SetI2SASRCConfig()
 * (mt_soc_pcm_fm_i2s.c:316-321).  This port keeps the whole path - 2nd I2S
 * input, ASRC bypass and PMIC DAC out - at the 32 kHz input rate, which is the
 * only rate the FM I2S link carries, so no resampling is required.
 */
static const unsigned int mt6735_afe_fm_i2s_rates[] = {
	MT6735_AFE_FM_I2S_RATE,
};

static const struct snd_pcm_hw_constraint_list mt6735_afe_fm_i2s_rate_constraint = {
	.count = ARRAY_SIZE(mt6735_afe_fm_i2s_rates),
	.list = mt6735_afe_fm_i2s_rates,
	.mask = 0,
};

static int mt6735_afe_fm_i2s_startup(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	/*
	 * Two shared interfaces have to be free before the FM path may run:
	 *
	 * - AFE_I2S_CON (0x0018) belongs to the I2S0 four wire path while
	 *   that PCM is open (Enable4pin_I2S0_I2S3(),
	 *   mt_soc_afe_control.c:3749-3761 writes the same register).
	 *   mt6735-dai-i2s.c has the symmetric check.
	 * - The PMIC codec DAC (AFE_I2S_CON1, I2S_OUT_DAC block 12) is shared
	 *   with the modem voice paths, which program it at their own 8/16 kHz
	 *   rates (mt6735-dai-voice.c, SetI2SDacOut() :1637-1655).  Starting
	 *   the 32 kHz FM stream underneath a call would reprogram it.
	 */
	mutex_lock(&priv->i2s_con_lock);
	if (priv->i2s0_active || priv->voice_afe_users) {
		mutex_unlock(&priv->i2s_con_lock);
		return -EBUSY;
	}
	priv->fm_i2s_active = true;
	mutex_unlock(&priv->i2s_con_lock);

	/* AudDrv_Clk_On()/AudDrv_I2S_Clk_On(), mt_soc_pcm_fm_i2s.c:196-197 */
	ret = pm_runtime_get_sync(afe->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		goto err_release;
	}

	/* runtime->hw = mtk_fm_i2s_hardware (:134-150), mt_soc_pcm_fm_i2s.c:200 */
	snd_soc_set_runtime_hwparams(substream, afe->mtk_afe_hardware);

	/* snd_pcm_hw_constraint_list() for the FM rates, :204-205 */
	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &mt6735_afe_fm_i2s_rate_constraint);
	if (ret < 0)
		goto err_put;

	ret = snd_pcm_hw_constraint_minmax(runtime,
					   SNDRV_PCM_HW_PARAM_CHANNELS,
					   MT6735_AFE_FM_I2S_CHANNELS,
					   MT6735_AFE_FM_I2S_CHANNELS);
	if (ret < 0)
		goto err_put;

	ret = snd_pcm_hw_constraint_mask64(runtime, SNDRV_PCM_HW_PARAM_FORMAT,
					   MT6735_AFE_FM_I2S_FORMATS);
	if (ret < 0)
		goto err_put;

	/* snd_pcm_hw_constraint_integer(), :206 */
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto err_put;

	return 0;

err_put:
	pm_runtime_put_noidle(afe->dev);
err_release:
	mutex_lock(&priv->i2s_con_lock);
	priv->fm_i2s_active = false;
	mutex_unlock(&priv->i2s_con_lock);

	return ret;
}

/*
 * Set2ndI2SIn()/Set2ndI2SInEnable(false) + mtk_pcm_fm_i2s_close(),
 * mt_soc_pcm_fm_i2s.c:227-264 and mt_soc_afe_control.c:2214-2249.
 * Idempotent so that a failed prepare or a double shutdown is harmless.
 */
static void mt6735_afe_fm_i2s_teardown(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	/* Set2ndI2SInEnable(false) */
	regmap_update_bits(afe->regmap, AFE_I2S_CON, AFE_I2S_CON_EN, 0);
	priv->fm_i2s_on = false;

	/* SetMemoryPathEnable(I2S_IN_2, false) */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_I2S_IN_2_EN, 0);

	/*
	 * SetMemoryPathEnable(I2S_OUT_DAC, false) followed by
	 * SetI2SDacEnable(false), mt_soc_pcm_fm_i2s.c:242-244.  The
	 * I2S_OUT_DAC block is also enabled by the modem voice paths, so it is
	 * only released once no call is running, and AFE_I2S_CON1 is only
	 * returned when this DAI programmed it (fm_i2s_dac_owner) and no
	 * multimedia stream holds the PMIC codec DAC.
	 */
	if (!priv->voice_afe_users) {
		regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				   AFE_DAC_CON0_I2S_OUT_DAC_EN, 0);
		if (priv->fm_i2s_dac_owner &&
		    !mt6735_afe_playback_enabled(afe)) {
			mt6735_afe_dac_disable(afe);
			priv->fm_i2s_dac_owner = false;
		}
	}

	/*
	 * Disconnect the four FM routes, mt_soc_pcm_fm_i2s.c:246-254.  The
	 * hardware gain block itself is left programmed, as downstream does;
	 * the next prepare() resets its current gain value.
	 */
	regmap_update_bits(afe->regmap, AFE_GAIN1_CONN2,
			   AFE_CONN_FM_I2S_I00_TO_O13 |
			   AFE_CONN_FM_I2S_I01_TO_O14, 0);
	regmap_update_bits(afe->regmap, AFE_GAIN1_CONN,
			   AFE_CONN_FM_I2S_I10_TO_O03 |
			   AFE_CONN_FM_I2S_I11_TO_O04, 0);

	priv->fm_i2s_prepared = false;

	/* EnableAfe(false), mt_soc_pcm_fm_i2s.c:257 */
	mt6735_afe_update_power(afe);

	/*
	 * EnableAfe(false) also returns the PMIC audio bus pins to GPIO mode
	 * once no audio path is running (AudDrv_Gpio.c:215-236).
	 */
	if (!mt6735_afe_playback_enabled(afe) && !priv->adc_path_on &&
	    !priv->voice_afe_users)
		mt6735_afe_select_pmic_pins(afe, false);
}

static void mt6735_afe_fm_i2s_shutdown(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	mt6735_afe_fm_i2s_teardown(afe);

	mutex_lock(&priv->i2s_con_lock);
	priv->fm_i2s_active = false;
	mutex_unlock(&priv->i2s_con_lock);

	/* AudDrv_I2S_Clk_Off()/AudDrv_Clk_Off(), mt_soc_pcm_fm_i2s.c:259-260 */
	pm_runtime_put(afe->dev);
}

static int mt6735_afe_fm_i2s_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params,
				       struct snd_soc_dai *dai)
{
	/*
	 * The 2nd I2S input is a fixed link: Set2ndI2SIn() is always called
	 * with 32000 Hz, 16 bit stereo (mt_soc_pcm_fm_i2s.c:305-313) and the
	 * FM codec DAI configures the same format.  Anything else would
	 * silently play at the wrong rate, so reject it.
	 */
	if (params_rate(params) != MT6735_AFE_FM_I2S_RATE)
		return -EINVAL;
	if (params_channels(params) != MT6735_AFE_FM_I2S_CHANNELS)
		return -EINVAL;
	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	return 0;
}

/*
 * mtk_pcm_fm_i2s_prepare(), mt_soc_pcm_fm_i2s.c:267-329.  The final enable
 * (Set2ndI2SInEnable(true) and EnableAfe(true)) is left to the trigger so the
 * data path only starts when the PCM actually runs; mPrepareDone becomes
 * fm_i2s_prepared and short circuits a repeated prepare.
 */
static int mt6735_afe_fm_i2s_prepare(struct snd_pcm_substream *substream,
				     struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int con;
	int fs, ret;

	if (priv->fm_i2s_prepared)
		return 0;

	fs = mt6735_afe_rate_to_fs(MT6735_AFE_FM_I2S_RATE);
	if (fs < 0)
		return fs;

	/* EnableAfe(true) muxes the PMIC audio bus, mt_soc_afe_control.c:993 */
	mt6735_afe_select_pmic_pins(afe, true);

	/* Interconnection: I00->O13, I01->O14, I10->O03, I11->O04 (:278-285) */
	regmap_update_bits(afe->regmap, AFE_GAIN1_CONN2,
			   AFE_CONN_FM_I2S_I00_TO_O13 |
			   AFE_CONN_FM_I2S_I01_TO_O14,
			   AFE_CONN_FM_I2S_I00_TO_O13 |
			   AFE_CONN_FM_I2S_I01_TO_O14);
	regmap_update_bits(afe->regmap, AFE_GAIN1_CONN,
			   AFE_CONN_FM_I2S_I10_TO_O03 |
			   AFE_CONN_FM_I2S_I11_TO_O04,
			   AFE_CONN_FM_I2S_I10_TO_O03 |
			   AFE_CONN_FM_I2S_I11_TO_O04);

	/*
	 * SetHwDigitalGainMode(GAIN1, runtime->rate, 0x40), :288-289:
	 * the 0xfff0 mode field is (sample per step << 8) | (fs << 4).
	 */
	regmap_update_bits(afe->regmap, AFE_GAIN1_CON0,
			   AFE_GAIN1_CON0_MODE_MASK,
			   (MT6735_AFE_FM_I2S_GAIN_STEP << 8) | (fs << 4));

	/* SetHwDigitalGainEnable(GAIN1, true): ramp up from a zero gain, :290 */
	regmap_write(afe->regmap, AFE_GAIN1_CUR, 0);
	regmap_update_bits(afe->regmap, AFE_GAIN1_CON0, AFE_GAIN1_CON0_EN,
			   AFE_GAIN1_CON0_EN);

	/* SetHwDigitalGain(mfm_i2s_Volume, GAIN1), :291 */
	regmap_write(afe->regmap, AFE_GAIN1_CON1, MT6735_AFE_FM_I2S_GAIN);

	/*
	 * Start the I2S DAC out: SetI2SDacOut(rate, false, 16 bit) followed by
	 * SetMemoryPathEnable(I2S_OUT_DAC) and SetI2SDacEnable(true), :294-299.
	 * When another stream already owns the PMIC codec DAC, keep its rate
	 * and only take the memory path, as downstream does (:298-299).
	 */
	if (!priv->dac_path_on) {
		ret = mt6735_afe_dac_prepare(afe, MT6735_AFE_FM_I2S_RATE, false);
		if (ret)
			return ret;
		priv->fm_i2s_dac_owner = true;
	}
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_I2S_OUT_DAC_EN,
			   AFE_DAC_CON0_I2S_OUT_DAC_EN);

	/* SetMemoryPathEnable(I2S_IN_2, true), :302 */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_I2S_IN_2_EN, AFE_DAC_CON0_I2S_IN_2_EN);

	/*
	 * Set2ndI2SIn(), mt_soc_afe_control.c:2224-2231:
	 *	bit  1 = 0	16 bit samples
	 *	bit  2 = 1	I2S slave (clock comes from CONNSYS)
	 *	bit  3 = 1	I2S framing
	 *	bit  5 = 0	LRCK not inverted
	 *	bit 28 = 0	I2S input pad select: from CONNSYS, not the IO mux
	 *	bit 31 = 1	phase shift fix
	 * Bit 0 (enable) is set by the trigger, and the register carries no
	 * low jitter or LR swap bit for this path.  Downstream masks the write
	 * with 0xfffffffe, i.e. every bit except the enable is overwritten.
	 */
	con = AFE_I2S_CON_PHASE_SHIFT_FIX |
	      (AFE_I2S_FORMAT_I2S << AFE_I2S_CON_FMT_SHIFT) |
	      AFE_I2S_CON_SLAVE |
	      (AFE_I2S_WLEN_16BIT << AFE_I2S_CON_WLEN_SHIFT);
	regmap_write(afe->regmap, AFE_I2S_CON, con);

	/*
	 * SetI2SASRCConfig(false, 0): bypass the ASRC.  Downstream only uses
	 * it to convert the fixed 32 kHz input to a 44.1/48 kHz stream
	 * (mt_soc_pcm_fm_i2s.c:316-321); everything runs at 32 kHz here, so
	 * the ASRC block stays out of the path (mt_soc_afe_control.c:2274).
	 */
	regmap_update_bits(afe->regmap, AFE_CONN4, AFE_CONN4_ASRC_BYPASS,
			   AFE_CONN4_ASRC_BYPASS);

	priv->fm_i2s_prepared = true;

	return 0;
}

static int mt6735_afe_fm_i2s_trigger(struct snd_pcm_substream *substream,
				     int cmd, struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/* Set2ndI2SInEnable(true), mt_soc_afe_control.c:2241-2248 */
		ret = regmap_update_bits(afe->regmap, AFE_I2S_CON,
					 AFE_I2S_CON_EN, AFE_I2S_CON_EN);
		if (ret)
			return ret;

		/* EnableAfe(true), mt_soc_pcm_fm_i2s.c:326 */
		priv->fm_i2s_on = true;
		mt6735_afe_update_power(afe);

		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		ret = regmap_update_bits(afe->regmap, AFE_I2S_CON,
					 AFE_I2S_CON_EN, 0);
		if (ret)
			return ret;

		/* EnableAfe(false): the FM path no longer needs the AFE clock */
		priv->fm_i2s_on = false;
		mt6735_afe_update_power(afe);

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6735_afe_fm_i2s_ops = {
	.startup = mt6735_afe_fm_i2s_startup,
	.shutdown = mt6735_afe_fm_i2s_shutdown,
	.hw_params = mt6735_afe_fm_i2s_hw_params,
	.prepare = mt6735_afe_fm_i2s_prepare,
	.trigger = mt6735_afe_fm_i2s_trigger,
};

const struct snd_soc_dai_driver mt6735_afe_fm_i2s_dai = {
	.name = MT6735_AFE_FM_I2S_DAI_NAME,
	.id = MT6735_AFE_DAI_FM_I2S,
	.ops = &mt6735_afe_fm_i2s_ops,
	.playback = {
		.stream_name = "FM_I2S Playback",
		.channels_min = MT6735_AFE_FM_I2S_CHANNELS,
		.channels_max = MT6735_AFE_FM_I2S_CHANNELS,
		.rates = SNDRV_PCM_RATE_32000,
		.rate_min = MT6735_AFE_FM_I2S_RATE,
		.rate_max = MT6735_AFE_FM_I2S_RATE,
		.formats = MT6735_AFE_FM_I2S_FORMATS,
	},
};
