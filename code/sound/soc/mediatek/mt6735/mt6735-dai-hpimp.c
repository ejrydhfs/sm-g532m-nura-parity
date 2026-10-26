// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC HP impedance front-end DAI.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/mt_soc_pcm_hp_impedance.c and the DL1
 * pieces it calls in mt_soc_afe_control.c.
 *
 * The HP impedance measurement plays a constant DC level out of the AFE and
 * reads the resulting voltage on PMIC AUXADC channel 9.  The DC level comes
 * from the AFE component control "HP Impedance DC Value"
 * (mt6735-afe-pcm.c), which fills the AFE internal SRAM; this DAI points the
 * DL1 memory interface at that SRAM and enables the normal AFE downlink
 * (DL1 -> I05/I06 -> O03/O04 -> I2S DAC -> PMIC codec) so the value is
 * actually clocked out.
 *
 * Why the internal SRAM and not the ALSA ring buffer: the downstream build
 * defines AUDIO_MEMORY_SRAM (AudDrv_Def.h:45), so AudDrv_Allocate_DL1_Buffer()
 * hands the DL1 memory interface AFE_INTERNAL_SRAM_PHY_BASE (0x11221000,
 * AFE_INTERNAL_SRAM_SIZE 0x8000) and the ioremapped SRAM pointer
 * (mt_soc_afe_control.c:2563-2584, AudDrv_Afe.h:375-377,
 * AudDrv_Afe.c:118-129).  The same buffer address is what
 * mtk_pcm_dl1_params() installs into runtime->dma_addr in the SRAM state
 * (mt_soc_pcm_dl1.c:265-273).  The vendor HP impedance PCM instead rebinds
 * its ALSA buffer, but the DC value it is given is written to the SRAM by
 * Audio_HP_ImpeDance_Set() (mt_soc_pcm_hp_impedance.c:361-362, 377-378), so
 * the SRAM is the buffer this port must play.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/* AFE_INTERNAL_SRAM_PHY_BASE (AudDrv_Afe.h:375) inside the AFE mapping. */
#define MT6735_AFE_HPIMP_SRAM_PHYS	(MT6735_AFE_PHYS_BASE + \
					 MT6735_AFE_INTERNAL_SRAM_OFFSET)

static int mt6735_afe_hpimp_startup(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int ret;

	/*
	 * DL1 is shared with the normal MultiMedia1 playback.  Refuse to take
	 * it over while another stream owns it.  afe->memif[].substream is set
	 * by the generic DL1 DAI at startup, so a merely opened DL1 stream is
	 * rejected as well as a running one.
	 */
	mutex_lock(&priv->hp_imp_lock);
	if (priv->hp_imp_open || afe->memif[MT6735_AFE_MEMIF_DL1].substream) {
		mutex_unlock(&priv->hp_imp_lock);
		return -EBUSY;
	}
	priv->hp_imp_open = true;
	mutex_unlock(&priv->hp_imp_lock);

	ret = pm_runtime_get_sync(afe->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		mutex_lock(&priv->hp_imp_lock);
		priv->hp_imp_open = false;
		mutex_unlock(&priv->hp_imp_lock);
		return ret;
	}

	/* mtk_pcm_hp_impedance_hardware, mt_soc_pcm_hp_impedance.c:100-116 */
	snd_soc_set_runtime_hwparams(substream, afe->mtk_afe_hardware);

	return 0;
}

static void mt6735_afe_hpimp_shutdown(struct snd_pcm_substream *substream,
				      struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	mutex_lock(&priv->hp_imp_lock);
	priv->hp_imp_open = false;
	mutex_unlock(&priv->hp_imp_lock);

	/*
	 * EnableAfe(false) returns the PMIC audio pins to GPIO mode once no
	 * memory interface is running (AudDrv_Gpio.c:215-236).
	 */
	if (!mt6735_afe_playback_enabled(afe) &&
	    !priv->memif_enabled[MT6735_AFE_MEMIF_VUL])
		mt6735_afe_select_pmic_pins(afe, false);

	pm_runtime_put(afe->dev);
}

static int mt6735_afe_hpimp_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mtk_base_afe_memif *memif = &afe->memif[MT6735_AFE_MEMIF_DL1];
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	int fs;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
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

	/*
	 * SetDL1Buffer(), mt_soc_pcm_hp_impedance.c:137-160, with the buffer
	 * address forced to the AFE internal SRAM instead of the ALSA ring
	 * buffer (see the file header).  The vendor writes
	 * base .. base + size - 1 to AFE_DL1_BASE/END.
	 */
	regmap_write(afe->regmap, AFE_DL1_BASE, MT6735_AFE_HPIMP_SRAM_PHYS);
	regmap_write(afe->regmap, AFE_DL1_END,
		     MT6735_AFE_HPIMP_SRAM_PHYS +
		     MT6735_AFE_INTERNAL_SRAM_SIZE - 1);

	memif->phys_buf_addr = MT6735_AFE_HPIMP_SRAM_PHYS;
	memif->buffer_size = MT6735_AFE_INTERNAL_SRAM_SIZE;

	return 0;
}

static int mt6735_afe_hpimp_prepare(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	const struct mtk_base_memif_data *data =
		afe->memif[MT6735_AFE_MEMIF_DL1].data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	bool wlen24 = (runtime->format == SNDRV_PCM_FORMAT_S24_LE);
	unsigned int wlen, mono;
	int fs, ret;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	fs = mt6735_afe_rate_to_fs(runtime->rate);
	if (fs < 0)
		return fs;

	/*
	 * EnableAfe(true) also switches the PMIC audio bus pins to their audio
	 * function (AudDrv_Gpio.c:215-236, AudDrv_GPIO_PMIC_Select()).
	 */
	mt6735_afe_select_pmic_pins(afe, true);

	/*
	 * SetMemIfFetchFormatPerSample(DL1, ...), mt_soc_pcm_hp_impedance.c:
	 * 239-250: AFE_WLEN_32_BIT_ALIGN_8BIT_0_24BIT_DATA for a 32/24 bit
	 * stream (the port supports S24_LE) and AFE_WLEN_16_BIT otherwise.
	 */
	wlen = wlen24 ? AFE_WLEN_24_BIT : AFE_WLEN_16_BIT;
	regmap_update_bits(afe->regmap, data->hd_reg,
			   AFE_MEMIF_PBUF_FMT_MASK << data->hd_shift,
			   wlen << data->hd_shift);

	/* SetoutputConnectionFormat() for O03/O04, :243-253 */
	mt6735_afe_set_conn_width(afe, MT6735_AFE_MEMIF_DL1, wlen24);

	/* SetI2SDacOut()/SetI2SDacEnable(), :257-263 and 2010-2026 */
	if (!priv->dac_path_on) {
		ret = mt6735_afe_dac_prepare(afe, runtime->rate, wlen24);
		if (ret)
			return ret;
	}

	/* SetSampleRate(DL1)/SetChannels(DL1), :270-271 */
	regmap_update_bits(afe->regmap, data->fs_reg,
			   data->fs_maskbit << data->fs_shift,
			   fs << data->fs_shift);
	mono = (runtime->channels == 1) ? 1 : 0;
	regmap_update_bits(afe->regmap, data->mono_reg,
			   BIT(data->mono_shift), mono << data->mono_shift);

	/*
	 * SetConnection(I05 -> O03) and SetConnection(I06 -> O04),
	 * mt_soc_pcm_hp_impedance.c:265-268.
	 */
	return mt6735_afe_connect(afe, MT6735_AFE_MEMIF_DL1, true);
}

static int mt6735_afe_hpimp_trigger(struct snd_pcm_substream *substream,
				    int cmd, struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/*
		 * SetMemoryPathEnable(MEM_DL1, true) plus EnableAfe(true),
		 * mt_soc_pcm_hp_impedance.c:272-274.  The vendor enables both
		 * in prepare; the port keeps ALSA's prepare/trigger split.
		 */
		return mt6735_afe_memif_enable(afe, MT6735_AFE_MEMIF_DL1, true);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/* mtk_soc_pcm_hp_impedance_close(), :281-290 */
		ret = mt6735_afe_memif_enable(afe, MT6735_AFE_MEMIF_DL1,
					      false);
		if (ret)
			return ret;
		mt6735_afe_connect(afe, MT6735_AFE_MEMIF_DL1, false);

		if (!mt6735_afe_playback_enabled(afe) &&
		    !priv->memif_enabled[MT6735_AFE_MEMIF_VUL])
			mt6735_afe_dac_disable(afe);

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6735_afe_hpimp_ops = {
	.startup = mt6735_afe_hpimp_startup,
	.shutdown = mt6735_afe_hpimp_shutdown,
	.hw_params = mt6735_afe_hpimp_hw_params,
	.prepare = mt6735_afe_hpimp_prepare,
	.trigger = mt6735_afe_hpimp_trigger,
};

/*
 * MT_SOC_HP_IMPEDANCE_NAME (AudDrv_Def.h:229) and
 * MT_SOC_HP_IMPEDANCE_STREAM_NAME (AudDrv_Def.h:336).
 */
const struct snd_soc_dai_driver mt6735_afe_hp_imp_dai = {
	.name = "mt-soc-hpimpedancedai-driver",
	.id = MT6735_AFE_DAI_HPIMP,
	.ops = &mt6735_afe_hpimp_ops,
	.playback = {
		.stream_name = "HP_IMPEDANCE_Playback",
		.channels_min = MT6735_AFE_CHANNELS_MIN,
		.channels_max = MT6735_AFE_CHANNELS_MAX,
		.rates = MT6735_AFE_RATES,
		.rate_min = MT6735_AFE_RATE_MIN,
		.rate_max = MT6735_AFE_RATE_MAX,
		.formats = MT6735_AFE_FORMATS,
	},
};
