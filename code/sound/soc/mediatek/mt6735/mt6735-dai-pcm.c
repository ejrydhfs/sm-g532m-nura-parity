// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE front-end DAIs (DL1, DL2, VUL).
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/:
 *   mt_soc_pcm_dl1.c      - DL1 playback (codec path)
 *   mt_soc_pcm_dl2.c      - DL2 playback (codec path)
 *   mt_soc_pcm_capture.c  - VUL capture
 *   mt_soc_afe_control.c  - memif/IRQ/rate/channel programming
 */

#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/*
 * SetDLSrc2() sample rate table, mt_soc_afe_control.c:1596-1616.  Note that
 * this field uses a dense 0..8 encoding, unlike the AFE_DAC_CON1 / I2S fields
 * which use the sparse mt_soc_digital_type.h encoding.
 */
static const unsigned int mt6735_afe_dlsrc2_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
};

bool mt6735_afe_playback_enabled(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	return priv->memif_enabled[MT6735_AFE_MEMIF_DL1] ||
	       priv->memif_enabled[MT6735_AFE_MEMIF_DL2];
}

static bool mt6735_afe_capture_enabled(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	return priv->memif_enabled[MT6735_AFE_MEMIF_VUL];
}

/*
 * SetMemoryPathEnable(), mt_soc_afe_control.c:1968-2000: the memory
 * interface enable bit is AFE_DAC_CON0 bit (interface + 1).
 */
int mt6735_afe_memif_enable(struct mtk_base_afe *afe, int id,
			    bool enable)
{
	const struct mtk_base_memif_data *data = afe->memif[id].data;
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int bit = BIT(data->enable_shift);

	regmap_update_bits(afe->regmap, data->enable_reg, bit,
			   enable ? bit : 0);
	priv->memif_enabled[id] = enable;
	mt6735_afe_update_power(afe);

	return 0;
}

/*
 * SetConnection(), mt_soc_afe_connection.c:433-500.  Only the four routes
 * used by this driver are implemented; see the table entries cited in
 * mt6735-afe-regs.h.
 */
int mt6735_afe_connect(struct mtk_base_afe *afe, int id, bool connect)
{
	unsigned int reg, bits;

	switch (id) {
	case MT6735_AFE_MEMIF_DL1:
		reg = AFE_CONN1;
		bits = AFE_CONN_DL1_L_TO_O03;
		regmap_update_bits(afe->regmap, reg, bits, connect ? bits : 0);
		reg = AFE_CONN2;
		bits = AFE_CONN_DL1_R_TO_O04;
		break;
	case MT6735_AFE_MEMIF_DL2:
		reg = AFE_CONN1;
		bits = AFE_CONN_DL2_L_TO_O03;
		regmap_update_bits(afe->regmap, reg, bits, connect ? bits : 0);
		reg = AFE_CONN2;
		bits = AFE_CONN_DL2_R_TO_O04;
		break;
	case MT6735_AFE_MEMIF_VUL:
		reg = AFE_CONN3;
		bits = AFE_CONN_ADC_L_TO_O09 | AFE_CONN_ADC_R_TO_O10;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(afe->regmap, reg, bits, connect ? bits : 0);

	return 0;
}

/* SetoutputConnectionFormat(), mt_soc_afe_control.c:2371-2377 */
void mt6735_afe_set_conn_width(struct mtk_base_afe *afe, int id, bool wlen24)
{
	unsigned int bits;

	switch (id) {
	case MT6735_AFE_MEMIF_DL1:
	case MT6735_AFE_MEMIF_DL2:
		bits = AFE_CONN_24BIT_O03 | AFE_CONN_24BIT_O04;
		break;
	case MT6735_AFE_MEMIF_VUL:
		bits = AFE_CONN_24BIT_O09 | AFE_CONN_24BIT_O10;
		break;
	default:
		return;
	}

	regmap_update_bits(afe->regmap, AFE_CONN_24BIT, bits,
			   wlen24 ? bits : 0);
}

/*
 * Downstream codec playback interface: SetI2SDacOut()/SetDLSrc2()/
 * SetI2SDacEnable() from mt_soc_afe_control.c:1592-1655 and 2010-2034.
 * AFE_I2S_CON1 is the I2S-style bus between the AFE and the PMIC codec.
 */
int mt6735_afe_dac_prepare(struct mtk_base_afe *afe, unsigned int rate,
			   bool wlen32)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int val;
	int fs, idx;

	fs = mt6735_afe_rate_to_fs(rate);
	if (fs < 0)
		return fs;

	for (idx = 0; idx < ARRAY_SIZE(mt6735_afe_dlsrc2_rates); idx++)
		if (mt6735_afe_dlsrc2_rates[idx] == rate)
			break;
	if (idx == ARRAY_SIZE(mt6735_afe_dlsrc2_rates))
		return -EINVAL;

	/* CleanPreDistortion(), mt_soc_afe_control.c:1583-1590 */
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);

	/*
	 * SetDLSrc2(), mt_soc_afe_control.c:1592-1635.  8k and 16k use the
	 * voice mode configuration; the final 0x1 << 1 applies the -0.3 dB
	 * speech gain MediaTek recommends for every rate.
	 */
	val = (idx << 28) | (0x3 << 24) | (0x3 << 11);
	if (idx == 0 || idx == 3)
		val |= (0x1 << 5);
	val |= (0x1 << 1);
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0, val);
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);

	/* SetI2SDacOut(), mt_soc_afe_control.c:1637-1655 */
	val = (AFE_I2S_FORMAT_I2S << AFE_I2S_CON_FMT_SHIFT) |
	      ((wlen32 ? AFE_I2S_WLEN_32BIT : AFE_I2S_WLEN_16BIT) <<
	       AFE_I2S_CON_WLEN_SHIFT) |
	      (fs << AFE_I2S_CON_FS_SHIFT);
	regmap_write(afe->regmap, AFE_I2S_CON1, val);

	/* SetSampleRate(MEM_I2S, rate), mt_soc_pcm_dl1.c:337 */
	regmap_update_bits(afe->regmap, AFE_DAC_CON1,
			   AFE_DAC_CON1_FS_MASK << AFE_DAC_CON1_I2S_FS_SHIFT,
			   fs << AFE_DAC_CON1_I2S_FS_SHIFT);

	/* SetI2SDacEnable(true), mt_soc_afe_control.c:2010-2026 */
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
			   AFE_ADDA_UL_DL_CON0_EN, AFE_ADDA_UL_DL_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   AFE_ADDA_DL_SRC2_CON0_EN,
			   AFE_ADDA_DL_SRC2_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN,
			   AFE_I2S_CON1_EN);

	priv->dac_path_on = true;

	return 0;
}

/* SetI2SDacEnable(false), mt_soc_afe_control.c:2010-2034 */
void mt6735_afe_dac_disable(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   AFE_ADDA_DL_SRC2_CON0_EN, 0);
	regmap_update_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN, 0);

	if (!priv->adc_path_on)
		regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
				   AFE_ADDA_UL_DL_CON0_EN, 0);

	priv->dac_path_on = false;
}

/*
 * PMIC codec capture interface: SetI2SAdcIn() internal ADC path and
 * SetI2SAdcEnable(), mt_soc_afe_control.c:1346-1370 and 1553-1572.
 */
static int mt6735_afe_adc_prepare(struct mtk_base_afe *afe, unsigned int rate)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int voice_mode;
	int fs;

	fs = mt6735_afe_rate_to_fs(rate);
	if (fs < 0)
		return fs;

	/*
	 * The downstream driver leaves the voice mode selection at 0 for any
	 * rate that is not 8k/16k/32k/48k (mt_soc_afe_control.c:1352-1366).
	 */
	switch (rate) {
	case 8000:
		voice_mode = 0;
		break;
	case 16000:
		voice_mode = 1;
		break;
	case 32000:
		voice_mode = 2;
		break;
	case 48000:
		voice_mode = 3;
		break;
	default:
		voice_mode = 0;
		break;
	}

	/* SetI2SAdcIn(), mt_soc_afe_control.c:1346-1370 */
	regmap_update_bits(afe->regmap, AFE_ADDA_TOP_CON0,
			   AFE_ADDA_TOP_CON0_EXT_ADC, 0);
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0, 0x001e0000,
			   (voice_mode << 19) | (voice_mode << 17));
	regmap_write(afe->regmap, AFE_ADDA_NEWIF_CFG0, 0x03f87201);
	regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG1, 0x00000c00,
			   ((voice_mode < 3) ? 1 : 3) << 10);

	/* SetI2SAdcEnable(true), mt_soc_afe_control.c:1553-1566 */
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
			   AFE_ADDA_UL_DL_CON0_EN, AFE_ADDA_UL_DL_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
			   AFE_ADDA_UL_SRC_CON0_EN, AFE_ADDA_UL_SRC_CON0_EN);

	priv->adc_path_on = true;

	return 0;
}

static void mt6735_afe_adc_disable(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
			   AFE_ADDA_UL_SRC_CON0_EN, 0);

	if (!priv->dac_path_on)
		regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
				   AFE_ADDA_UL_DL_CON0_EN, 0);

	priv->adc_path_on = false;
}

static int mt6735_afe_fe_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int id = dai->id;
	int ret;

	ret = pm_runtime_get_sync(afe->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		return ret;
	}

	/*
	 * The HP impedance FE stream rebases the DL1 memory interface onto the
	 * AFE internal SRAM.  Refuse a second DL1 user while it is held; the
	 * HP impedance DAI likewise refuses to start while DL1 is playing.
	 * Both reservations happen under hp_imp_lock, so the two DAIs cannot
	 * slip past each other's check.
	 */
	mutex_lock(&priv->hp_imp_lock);
	if (id == MT6735_AFE_DAI_DL1 && priv->hp_imp_open) {
		mutex_unlock(&priv->hp_imp_lock);
		pm_runtime_put(afe->dev);
		return -EBUSY;
	}
	afe->memif[id].substream = substream;
	mutex_unlock(&priv->hp_imp_lock);

	snd_soc_set_runtime_hwparams(substream, afe->mtk_afe_hardware);

	/*
	 * The memory interface consumes whole 64 byte blocks
	 * (Align64ByteSize(), mt_soc_afe_control.c:3702-3710) and the DL1
	 * buffer index is masked with 0xffffffc0 in the interrupt handler
	 * (mt_soc_afe_control.c:3025,3065).
	 */
	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 64);
	if (ret < 0)
		goto err_put;

	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto err_put;

	/*
	 * Capture keeps a ping-pong buffer in the AFE, so the reported read
	 * pointer can trail the interrupt point by up to a period.
	 */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		ret = snd_pcm_hw_constraint_minmax(runtime,
						   SNDRV_PCM_HW_PARAM_PERIODS, 3,
						   runtime->hw.periods_max);
		if (ret < 0)
			goto err_put;
	}

	return 0;

err_put:
	mutex_lock(&priv->hp_imp_lock);
	afe->memif[id].substream = NULL;
	mutex_unlock(&priv->hp_imp_lock);
	pm_runtime_put_noidle(afe->dev);
	return ret;
}

static void mt6735_afe_fe_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	mutex_lock(&priv->hp_imp_lock);
	afe->memif[dai->id].substream = NULL;
	mutex_unlock(&priv->hp_imp_lock);

	/*
	 * EnableAfe(false) returns the PMIC audio pins to GPIO mode once no
	 * memory interface is running (AudDrv_Gpio.c:215-236).
	 */
	if (!mt6735_afe_playback_enabled(afe) && !mt6735_afe_capture_enabled(afe))
		mt6735_afe_select_pmic_pins(afe, false);

	pm_runtime_put(afe->dev);
}

static int mt6735_afe_fe_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mtk_base_afe_memif *memif = &afe->memif[dai->id];
	const struct mtk_base_memif_data *data = memif->data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int mono;
	int fs;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
	    dai->id == MT6735_AFE_DAI_VUL)
		return -EINVAL;
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
	    dai->id != MT6735_AFE_DAI_VUL)
		return -EINVAL;

	fs = mt6735_afe_rate_to_fs(rate);
	if (fs < 0)
		return -EINVAL;
	if (channels < MT6735_AFE_CHANNELS_MIN ||
	    channels > MT6735_AFE_CHANNELS_MAX)
		return -EINVAL;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
		break;
	default:
		return -EINVAL;
	}

	if (!runtime->dma_bytes || !runtime->dma_addr)
		return -EINVAL;

	/*
	 * The AFE memory interfaces only decode 32 bit addresses.  A 33 bit
	 * address would need AFE_MEMIF_MSB (AudDrv_Afe.h:470), which the
	 * downstream driver never programs.
	 */
	if (upper_32_bits(runtime->dma_addr))
		return -EINVAL;

	/* SetDL1Buffer()/SetDL2Buffer(), mt_soc_pcm_dl1.c:234-255 */
	regmap_write(afe->regmap, data->reg_ofs_base,
		     lower_32_bits(runtime->dma_addr));
	regmap_write(afe->regmap, data->reg_ofs_end,
		     lower_32_bits(runtime->dma_addr) + runtime->dma_bytes - 1);

	memif->phys_buf_addr = lower_32_bits(runtime->dma_addr);
	memif->buffer_size = runtime->dma_bytes;
	memif->dma_area = runtime->dma_area;
	memif->dma_addr = runtime->dma_addr;
	memif->dma_bytes = runtime->dma_bytes;

	/* SetSampleRate(), mt_soc_afe_control.c:1073-1128 */
	regmap_update_bits(afe->regmap, data->fs_reg,
			   data->fs_maskbit << data->fs_shift,
			   fs << data->fs_shift);

	/* SetChannels(), mt_soc_afe_control.c:1129-1160 */
	mono = (channels == 1) ? 1 : 0;
	regmap_update_bits(afe->regmap, data->mono_reg,
			   BIT(data->mono_shift), mono << data->mono_shift);

	return 0;
}

static int mt6735_afe_fe_prepare(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	const struct mtk_base_memif_data *data = afe->memif[dai->id].data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	bool wlen24 = (runtime->format == SNDRV_PCM_FORMAT_S24_LE);
	unsigned int wlen;

	/*
	 * EnableAfe(true) also switches the PMIC audio bus pins to their
	 * audio function (AudDrv_Gpio.c:215-236, AudDrv_GPIO_PMIC_Select()).
	 * This runs from prepare because trigger is atomic.
	 */
	mt6735_afe_select_pmic_pins(afe, true);

	/* SetMemIfFetchFormatPerSample(), mt_soc_afe_control.c:2294-2369 */
	wlen = wlen24 ? AFE_WLEN_24_BIT : AFE_WLEN_16_BIT;
	regmap_update_bits(afe->regmap, data->hd_reg,
			   AFE_MEMIF_PBUF_FMT_MASK << data->hd_shift,
			   wlen << data->hd_shift);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		mt6735_afe_set_conn_width(afe, dai->id, wlen24);
		if (priv->dac_path_on)
			return 0;
		return mt6735_afe_dac_prepare(afe, runtime->rate, wlen24);
	}

	mt6735_afe_set_conn_width(afe, dai->id, wlen24);
	if (priv->adc_path_on)
		return 0;
	return mt6735_afe_adc_prepare(afe, runtime->rate);
}

static int mt6735_afe_fe_trigger(struct snd_pcm_substream *substream, int cmd,
				 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	int id = dai->id;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = mt6735_afe_connect(afe, id, true);
		if (ret)
			return ret;

		ret = mt6735_afe_memif_enable(afe, id, true);
		if (ret)
			return ret;

		/*
		 * irq_add_user(), mt_soc_afe_control.c:4013-4062: program the
		 * sample rate and counter, then enable the interrupt.
		 */
		return mt6735_afe_irq_update(afe, afe->memif[id].irq_usage);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mt6735_afe_memif_enable(afe, id, false);
		mt6735_afe_irq_update(afe, afe->memif[id].irq_usage);
		mt6735_afe_connect(afe, id, false);

		/* mt_soc_pcm_dl1.c:347-358 / mt_soc_pcm_capture.c:101-116 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			if (!mt6735_afe_playback_enabled(afe))
				mt6735_afe_dac_disable(afe);
		} else {
			if (!mt6735_afe_capture_enabled(afe))
				mt6735_afe_adc_disable(afe);
		}

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6735_afe_fe_ops = {
	.startup = mt6735_afe_fe_startup,
	.shutdown = mt6735_afe_fe_shutdown,
	.hw_params = mt6735_afe_fe_hw_params,
	.prepare = mt6735_afe_fe_prepare,
	.trigger = mt6735_afe_fe_trigger,
};

/*
 * The PCM DAI id doubles as the memory interface index.  DL1 must stay the
 * first entry: ASoC assigns id 0 to the first DAI of an array when its
 * driver id is 0 (snd_soc_register_dai(), soc-core.c:2728-2735).
 */
const struct snd_soc_dai_driver mt6735_afe_pcm_dais[MT6735_AFE_PCM_DAI_NUM] = {
	[MT6735_AFE_DAI_DL1] = {
		.name = "mt-soc-dl1dai-driver",
		.id = MT6735_AFE_DAI_DL1,
		.ops = &mt6735_afe_fe_ops,
		.playback = {
			.stream_name = "DL1 Playback",
			.channels_min = MT6735_AFE_CHANNELS_MIN,
			.channels_max = MT6735_AFE_CHANNELS_MAX,
			.rates = MT6735_AFE_RATES,
			.rate_min = MT6735_AFE_RATE_MIN,
			.rate_max = MT6735_AFE_RATE_MAX,
			.formats = MT6735_AFE_FORMATS,
		},
	},
	[MT6735_AFE_DAI_DL2] = {
		.name = "mt-soc-dl2dai-driver",
		.id = MT6735_AFE_DAI_DL2,
		.ops = &mt6735_afe_fe_ops,
		.playback = {
			.stream_name = "DL2 Playback",
			.channels_min = MT6735_AFE_CHANNELS_MIN,
			.channels_max = MT6735_AFE_CHANNELS_MAX,
			.rates = MT6735_AFE_RATES,
			.rate_min = MT6735_AFE_RATE_MIN,
			.rate_max = MT6735_AFE_RATE_MAX,
			.formats = MT6735_AFE_FORMATS,
		},
	},
	[MT6735_AFE_DAI_VUL] = {
		.name = "mt-soc-ul1dai-driver",
		.id = MT6735_AFE_DAI_VUL,
		.ops = &mt6735_afe_fe_ops,
		.capture = {
			.stream_name = "UL1 Capture",
			.channels_min = MT6735_AFE_CHANNELS_MIN,
			.channels_max = MT6735_AFE_CHANNELS_MAX,
			.rates = MT6735_AFE_RATES,
			.rate_min = MT6735_AFE_RATE_MIN,
			.rate_max = MT6735_AFE_RATE_MAX,
			.formats = MT6735_AFE_FORMATS,
		},
	},
};
