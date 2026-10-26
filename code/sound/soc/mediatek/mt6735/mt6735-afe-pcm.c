// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE platform driver.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/:
 *   AudDrv_Afe.h          - AFE register map
 *   mt_soc_afe_control.c  - memif enable, IRQ handler, sample rate/channel
 *   AudDrv_Clk.c          - audio clock gating
 *   AudDrv_Gpio.c         - audio pin muxing
 *
 * The downstream driver binds the AFE with the legacy platform driver
 * "mt_soc_dl1_pcm" (ROM node mt_soc_dl1_pcm@11220000, compatible
 * "mediatek,mt-soc-dl1-pcm"); this driver exposes the same hardware as a
 * single ASoC component named "mtk-afe-pcm".
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/*
 * Memory interface descriptors.
 *
 *	DL1: AFE_DL1_BASE/CUR/END	AudDrv_Afe.h:443-445
 *	DL2: AFE_DL2_BASE/CUR/END	AudDrv_Afe.h:454-456
 *	VUL: AFE_VUL_BASE/END/CUR	AudDrv_Afe.h:462-464
 */
static const struct mtk_base_memif_data mt6735_afe_memif_data[MT6735_AFE_MEMIF_NUM] = {
	[MT6735_AFE_MEMIF_DL1] = {
		.id = MT6735_AFE_MEMIF_DL1,
		.name = "DL1",
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.reg_ofs_end = AFE_DL1_END,
		.reg_ofs_base_msb = -1,
		.reg_ofs_cur_msb = -1,
		.reg_ofs_end_msb = -1,
		.fs_reg = AFE_DAC_CON1,			/* mt_soc_afe_control.c:1080 */
		.fs_shift = AFE_DAC_CON1_DL1_FS_SHIFT,
		.fs_maskbit = AFE_DAC_CON1_FS_MASK,
		.mono_reg = AFE_DAC_CON1,		/* mt_soc_afe_control.c:1135 */
		.mono_shift = AFE_DAC_CON1_DL1_MONO_SHIFT,
		.quad_ch_reg = -1,
		.int_odd_flag_reg = -1,
		.enable_reg = AFE_DAC_CON0,		/* mt_soc_afe_control.c:1995-1997 */
		.enable_shift = MT6735_AFE_MEMIF_DL1 + 1,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,		/* mt_soc_afe_control.c:2303 */
		.hd_shift = AFE_MEMIF_PBUF_DL1_FMT_SHIFT,
		.hd_align_reg = -1,
		.agent_disable_reg = -1,
		.msb_reg = -1,
		.msb_end_reg = -1,
		.ch_num_reg = -1,
		.pbuf_reg = -1,
		.minlen_reg = -1,
	},
	[MT6735_AFE_MEMIF_DL2] = {
		.id = MT6735_AFE_MEMIF_DL2,
		.name = "DL2",
		/*
		 * SetChannels() for DL2 writes the mono value with a bit 21
		 * shift but masks bit 22 (mt_soc_afe_control.c:1139), so the
		 * downstream driver can never set the field.  The register
		 * mask identifies bit 22 as the DL2 mono bit; use it.
		 */
		.reg_ofs_base = AFE_DL2_BASE,
		.reg_ofs_cur = AFE_DL2_CUR,
		.reg_ofs_end = AFE_DL2_END,
		.reg_ofs_base_msb = -1,
		.reg_ofs_cur_msb = -1,
		.reg_ofs_end_msb = -1,
		.fs_reg = AFE_DAC_CON1,			/* mt_soc_afe_control.c:1084 */
		.fs_shift = AFE_DAC_CON1_DL2_FS_SHIFT,
		.fs_maskbit = AFE_DAC_CON1_FS_MASK,
		.mono_reg = AFE_DAC_CON1,		/* mt_soc_afe_control.c:1139 */
		.mono_shift = AFE_DAC_CON1_DL2_MONO_SHIFT,
		.quad_ch_reg = -1,
		.int_odd_flag_reg = -1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = MT6735_AFE_MEMIF_DL2 + 1,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,		/* mt_soc_afe_control.c:2315 */
		.hd_shift = AFE_MEMIF_PBUF_DL2_FMT_SHIFT,
		.hd_align_reg = -1,
		.agent_disable_reg = -1,
		.msb_reg = -1,
		.msb_end_reg = -1,
		.ch_num_reg = -1,
		.pbuf_reg = -1,
		.minlen_reg = -1,
	},
	[MT6735_AFE_MEMIF_DAI] = {
		.id = MT6735_AFE_MEMIF_DAI,
		.name = "DAI",
		/*
		 * BT SCO capture buffer, mt_soc_pcm_bt_dai.c:228-240.  The
		 * sample rate field is AFE_DAC_CON0[25:24] with a dense
		 * 8k/16k/32k encoding (SetSampleRate(MEM_DAI),
		 * mt_soc_afe_control.c:1099-1112); there is no channel field
		 * for this interface (SetChannels() has no MEM_DAI case,
		 * mt_soc_afe_control.c:1129-1160).
		 */
		.reg_ofs_base = AFE_DAI_BASE,
		.reg_ofs_cur = AFE_DAI_CUR,
		.reg_ofs_end = AFE_DAI_END,
		.reg_ofs_base_msb = -1,
		.reg_ofs_cur_msb = -1,
		.reg_ofs_end_msb = -1,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = AFE_DAC_CON0_DAI_FS_SHIFT,
		.fs_maskbit = AFE_DAC_CON0_DAI_FS_MASK,
		.mono_reg = -1,
		.mono_shift = 0,
		.quad_ch_reg = -1,
		.int_odd_flag_reg = -1,
		.enable_reg = AFE_DAC_CON0,		/* mt_soc_afe_control.c:1995-1997 */
		.enable_shift = MT6735_AFE_MEMIF_DAI + 1,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,		/* mt_soc_afe_control.c:2343-2346 */
		.hd_shift = AFE_MEMIF_PBUF_DAI_FMT_SHIFT,
		.hd_align_reg = -1,
		.agent_disable_reg = -1,
		.msb_reg = -1,
		.msb_end_reg = -1,
		.ch_num_reg = -1,
		.pbuf_reg = -1,
		.minlen_reg = -1,
	},
	[MT6735_AFE_MEMIF_VUL] = {
		.id = MT6735_AFE_MEMIF_VUL,
		.name = "VUL",
		.reg_ofs_base = AFE_VUL_BASE,
		.reg_ofs_cur = AFE_VUL_CUR,
		.reg_ofs_end = AFE_VUL_END,
		.reg_ofs_base_msb = -1,
		.reg_ofs_cur_msb = -1,
		.reg_ofs_end_msb = -1,
		.fs_reg = AFE_DAC_CON1,			/* mt_soc_afe_control.c:1096 */
		.fs_shift = AFE_DAC_CON1_VUL_FS_SHIFT,
		.fs_maskbit = AFE_DAC_CON1_FS_MASK,
		.mono_reg = AFE_DAC_CON1,		/* mt_soc_afe_control.c:1147 */
		.mono_shift = AFE_DAC_CON1_VUL_MONO_SHIFT,
		.quad_ch_reg = -1,
		.int_odd_flag_reg = -1,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = MT6735_AFE_MEMIF_VUL + 1,
		.hd_reg = AFE_MEMIF_PBUF_SIZE,		/* mt_soc_afe_control.c:2335 */
		.hd_shift = AFE_MEMIF_PBUF_VUL_FMT_SHIFT,
		.hd_align_reg = -1,
		.agent_disable_reg = -1,
		.msb_reg = -1,
		.msb_end_reg = -1,
		.ch_num_reg = -1,
		.pbuf_reg = -1,
		.minlen_reg = -1,
	},
};

/* IRQ line used by each memory interface, mt_soc_afe_control.c:613-636 */
static const int mt6735_afe_memif_irq[MT6735_AFE_MEMIF_NUM] = {
	[MT6735_AFE_MEMIF_DL1] = MT6735_AFE_IRQ1,
	[MT6735_AFE_MEMIF_DL2] = MT6735_AFE_IRQ1,
	[MT6735_AFE_MEMIF_VUL] = MT6735_AFE_IRQ2,
	/* irq_add_user(..., IRQ2), mt_soc_pcm_bt_dai.c:127-129 */
	[MT6735_AFE_MEMIF_DAI] = MT6735_AFE_IRQ2,
};

/*
 * 36 KiB per direction, the downstream AFE SRAM/DRAM buffer sizes
 * (mt_soc_pcm_common.h:123,136; AudioSramPlaybackFullSize =
 * 1024 * 36 in mt_soc_afe_control.c:158-163).
 */
static const struct snd_pcm_hardware mt6735_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = MT6735_AFE_FORMATS,
	.rates = MT6735_AFE_RATES,
	.rate_min = MT6735_AFE_RATE_MIN,
	.rate_max = MT6735_AFE_RATE_MAX,
	.channels_min = MT6735_AFE_CHANNELS_MIN,
	.channels_max = MT6735_AFE_CHANNELS_MAX,
	.buffer_bytes_max = 36 * 1024,
	.period_bytes_min = 64,
	.period_bytes_max = 36 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.fifo_size = 0,
};

/*
 * Sample rate field encoding used by AFE_DAC_CON1, AFE_I2S_CON(x) and
 * AFE_IRQ_MCU_CON (mt_soc_digital_type.h:265-279, SampleRateTransform(),
 * mt_soc_afe_control.c:1035-1072).
 */
static const struct {
	unsigned int rate;
	int fs;
} mt6735_afe_rates[] = {
	{ 8000, 0 },
	{ 11025, 1 },
	{ 12000, 2 },
	{ 16000, 4 },
	{ 22050, 5 },
	{ 24000, 6 },
	{ 32000, 8 },
	{ 44100, 9 },
	{ 48000, 10 },
};

int mt6735_afe_rate_to_fs(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6735_afe_rates); i++)
		if (mt6735_afe_rates[i].rate == rate)
			return mt6735_afe_rates[i].fs;

	return -EINVAL;
}

static bool mt6735_afe_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AFE_DL1_CUR:
	case AFE_DL2_CUR:
	case AFE_VUL_CUR:
	case AFE_DAI_CUR:
	case AFE_IRQ_MCU_STATUS:
	case AFE_IRQ_MCU_CLR:
	case AFE_IRQ_MCU_CNT1:
	case AFE_IRQ_MCU_CNT2:
	case AFE_IRQ1_MCU_CNT_MON:
	case AFE_IRQ2_MCU_CNT_MON:
	case AFE_MEMIF_MON0:
	case AFE_MEMIF_MON1:
	case AFE_MEMIF_MON2:
	case AFE_MEMIF_MON4:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config mt6735_afe_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = MT6735_AFE_SIZE - 4,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = mt6735_afe_volatile_reg,
};

/*
 * Keep AFE_DAC_CON0 bit 0 in sync with the set of enabled memory
 * interfaces.  This is the register write part of EnableAfe() from
 * mt_soc_afe_control.c:981,1029: the AFE is only powered when at least one
 * memory interface needs it.  Callers run in atomic context, so the PMIC bus
 * pin mux (also part of EnableAfe()) is programmed from the sleepable DAI
 * prepare/shutdown paths instead.
 */
void mt6735_afe_update_power(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	/*
	 * The modem PCM interfaces, DAI_BT and the FM I2S back end have no
	 * memory interface, so their EnableAfe() calls are tracked separately
	 * (mt6735-dai-voice.c and mt6735-dai-fm-i2s.c, EnableAfe()
	 * mt_soc_afe_control.c:951-1032).
	 */
	bool enabled = priv->voice_afe_users > 0 || priv->fm_i2s_on;
	int i;

	for (i = 0; i < MT6735_AFE_MEMIF_NUM; i++)
		enabled |= priv->memif_enabled[i];

	regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_DAC_CON0_AFE_ON,
			   enabled ? AFE_DAC_CON0_AFE_ON : 0);
}

/*
 * IRQ1 is shared by DL1 and DL2.  The downstream IRQ manager programs the
 * counter for the user with the smallest period measured at the highest
 * supported rate (IRQ_MAX_RATE, mt_soc_digital_type.h:865) and further
 * rescaled by 100 to avoid overflow (get_tgt_count(),
 * mt_soc_afe_control.c:3890-3895).  IRQ2 only has the capture interface.
 */
int mt6735_afe_irq_update(struct mtk_base_afe *afe, int irq)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	struct snd_pcm_runtime *best = NULL;
	u64 best_period = U64_MAX;
	unsigned int cnt_reg, fs_shift, en_bit;
	int i, fs;

	switch (irq) {
	case MT6735_AFE_IRQ1:
		cnt_reg = AFE_IRQ_MCU_CNT1;		/* mt_soc_afe_control.c:2159 */
		fs_shift = AFE_IRQ_MCU_CON_IRQ1_FS_SHIFT;
		en_bit = AFE_IRQ_MCU_CON_IRQ1_EN;
		break;
	case MT6735_AFE_IRQ2:
		cnt_reg = AFE_IRQ_MCU_CNT2;		/* mt_soc_afe_control.c:2162 */
		fs_shift = AFE_IRQ_MCU_CON_IRQ2_FS_SHIFT;
		en_bit = AFE_IRQ_MCU_CON_IRQ2_EN;
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < MT6735_AFE_MEMIF_NUM; i++) {
		struct snd_pcm_runtime *runtime;
		u64 period;

		if (afe->memif[i].irq_usage != irq || !priv->memif_enabled[i])
			continue;
		if (!afe->memif[i].substream)
			continue;

		runtime = afe->memif[i].substream->runtime;
		if (!runtime || !runtime->rate)
			continue;

		period = div_u64((u64)runtime->period_size *
				 MT6735_AFE_IRQ_MAX_RATE, runtime->rate);
		if (period < best_period) {
			best_period = period;
			best = runtime;
		}
	}

	if (!best) {
		/* disable and clear, disable_aud_irq(), mt_soc_afe_control.c:3847-3856 */
		regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON, en_bit, 0);
		regmap_write(afe->regmap, cnt_reg, 0);
		return 0;
	}

	fs = mt6735_afe_rate_to_fs(best->rate);
	if (fs < 0)
		return fs;

	/* SetIrqMcuCounter(), mt_soc_afe_control.c:2154-2178 */
	regmap_write(afe->regmap, cnt_reg, (unsigned int)best->period_size);
	/* SetIrqMcuSampleRate(), mt_soc_afe_control.c:2128-2152 */
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
			   AFE_IRQ_MCU_CON_FS_MASK << fs_shift, fs << fs_shift);
	/* SetIrqEnable(), mt_soc_afe_control.c:2099-2126 */
	regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON, en_bit, en_bit);

	return 0;
}

/*
 * AudDrv_IRQ_handler(), mt_soc_afe_control.c:574-642.  IRQ1 serves DL1 and
 * DL2, IRQ2 serves the capture interface.  AFE_IRQ_MCU_CLR is write-one to
 * clear for the bits reported in AFE_IRQ_MCU_STATUS.
 */
static irqreturn_t mt6735_afe_irq_handler(int irq, void *data)
{
	struct mtk_base_afe *afe = data;
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int status;
	int i;

	if (regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status))
		return IRQ_HANDLED;

	status &= AFE_IRQ_STATUS_ALL;
	if (!status) {
		/*
		 * Spurious interrupt recovery, mt_soc_afe_control.c:594-610:
		 * clear every source and stop the MCU interrupts after
		 * AudioInterruptLimiter (100) consecutive bogus interrupts.
		 */
		regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_STATUS_ALL);
		if (++priv->spurious_irqs > MT6735_AFE_IRQ_LIMIT) {
			regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
					   AFE_IRQ_MCU_CON_IRQ1_EN |
					   AFE_IRQ_MCU_CON_IRQ2_EN, 0);
			priv->spurious_irqs = 0;
		}
		return IRQ_HANDLED;
	}
	priv->spurious_irqs = 0;

	if (status & AFE_IRQ_STATUS_IRQ1) {
		for (i = MT6735_AFE_MEMIF_DL1; i <= MT6735_AFE_MEMIF_DL2; i++) {
			if (!priv->memif_enabled[i] || !afe->memif[i].substream)
				continue;
			snd_pcm_period_elapsed(afe->memif[i].substream);
		}
	}

	/*
	 * IRQ2 serves the capture interfaces: VUL and the BT SCO DAI memory
	 * interface, which share the line (mt_soc_pcm_bt_dai.c:124-129).
	 */
	if (status & AFE_IRQ_STATUS_IRQ2) {
		for (i = MT6735_AFE_MEMIF_VUL; i <= MT6735_AFE_MEMIF_DAI; i++) {
			if (!priv->memif_enabled[i] || !afe->memif[i].substream)
				continue;
			snd_pcm_period_elapsed(afe->memif[i].substream);
		}
	}

	regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status);

	return IRQ_HANDLED;
}

/*
 * Map a CPU DAI id to the memory interface it drives.  DL1/DL2/VUL are the
 * memory interface with the same id; the BT SCO DAI was appended after the
 * memif-less I2S0 DAI, so it needs an explicit translation.  The I2S0, HP
 * impedance, voice and FM I2S DAIs have no memory interface: the modem PCM
 * interfaces and the FM 2nd I2S path move data inside the AFE without AP
 * memory, so their stream position never advances
 * (mt_soc_pcm_voice_md1.c:253-257 leaves the ALSA buffer untouched).
 */
static int mt6735_afe_dai_memif_id(unsigned int dai_id)
{
	switch (dai_id) {
	case MT6735_AFE_DAI_DL1:
		return MT6735_AFE_MEMIF_DL1;
	case MT6735_AFE_DAI_DL2:
		return MT6735_AFE_MEMIF_DL2;
	case MT6735_AFE_DAI_VUL:
		return MT6735_AFE_MEMIF_VUL;
	case MT6735_AFE_DAI_BTCVSD:
		return MT6735_AFE_MEMIF_DAI;
	default:
		return -1;
	}
}

/*
 * The hardware reports the DMA read (playback) / write (capture) pointer,
 * mtk_pcm_pointer() in mt_soc_pcm_dl1.c:189-235 does the same computation.
 * The same AFE_DAI_BASE/AFE_DAI_CUR pair drives the BT SCO capture buffer
 * (mt_soc_pcm_bt_dai.c:175-210).
 */
static snd_pcm_uframes_t mt6735_afe_pcm_pointer(struct snd_soc_component *component,
					       struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	const struct mtk_base_memif_data *data;
	unsigned int hw_ptr = 0, hw_base = 0;
	int memif_id = mt6735_afe_dai_memif_id(snd_soc_rtd_to_cpu(rtd, 0)->id);

	if (memif_id < 0 || memif_id >= afe->memif_size)
		return 0;

	data = afe->memif[memif_id].data;
	if (!data || !afe->memif[memif_id].substream)
		return 0;

	if (regmap_read(afe->regmap, data->reg_ofs_cur, &hw_ptr) ||
	    regmap_read(afe->regmap, data->reg_ofs_base, &hw_base))
		return 0;

	if (!hw_ptr || !hw_base || hw_ptr < hw_base)
		return 0;

	return bytes_to_frames(substream->runtime, hw_ptr - hw_base);
}

static int mt6735_afe_pcm_new(struct snd_soc_component *component,
			      struct snd_soc_pcm_runtime *rtd)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	size_t size = afe->mtk_afe_hardware->buffer_bytes_max;

	/*
	 * The BT SCO interface only addresses BT_DAI_MAX_BUFFER_SIZE
	 * (mt_soc_pcm_common.h:119, mt_soc_pcm_bt_dai.c:69-78).
	 */
	if (snd_soc_rtd_to_cpu(rtd, 0)->id == MT6735_AFE_DAI_BTCVSD)
		size = MT6735_BTCVSD_BUFFER_MAX;

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV, afe->dev,
				       0, size);

	return 0;
}

static int mt6735_afe_component_probe(struct snd_soc_component *component)
{
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	snd_soc_component_init_regmap(component, afe->regmap);

	return 0;
}

/*
 * AFE internal SRAM DC excitation for the HP impedance measurement.
 *
 * The vendor writes AFE_INTERNAL_SRAM_SIZE (0x8000) bytes at
 * Get_Afe_SramBase_Pointer() with the 16 bit DC value supplied by userspace
 * (mt_soc_pcm_hp_impedance.c:361-362, 377-378) while its HP impedance PCM
 * owns the DL1 memory interface.  The fill is only accepted while the HP
 * impedance FE stream is open (mt6735-dai-hpimp.c), so a control write can
 * never move the memory interface underneath an active stream; DL1 and the
 * HP impedance DAI are mutually exclusive there.
 */
static int mt6735_afe_hp_dc_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_kcontrol_chip(kcontrol);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned long value = ucontrol->value.integer.value[0];
	void __iomem *sram;
	int i, ret;

	/* The AFE sample width is 16 bit. */
	if (value > U16_MAX)
		return -EINVAL;

	mutex_lock(&priv->hp_imp_lock);
	if (!priv->hp_imp_open) {
		mutex_unlock(&priv->hp_imp_lock);
		return -EBUSY;
	}

	ret = pm_runtime_resume_and_get(afe->dev);
	if (ret) {
		mutex_unlock(&priv->hp_imp_lock);
		return ret;
	}

	sram = afe->base_addr + MT6735_AFE_INTERNAL_SRAM_OFFSET;
	for (i = 0; i < MT6735_AFE_INTERNAL_SRAM_SIZE / 2; i++)
		writew(value, sram + i * 2);

	pm_runtime_put(afe->dev);
	mutex_unlock(&priv->hp_imp_lock);

	return 0;
}

static int mt6735_afe_hp_dc_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_kcontrol_chip(kcontrol);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	int ret;

	ret = pm_runtime_resume_and_get(afe->dev);
	if (ret)
		return ret;

	ucontrol->value.integer.value[0] =
		readw(afe->base_addr + MT6735_AFE_INTERNAL_SRAM_OFFSET);

	pm_runtime_put(afe->dev);

	return 0;
}

static const struct snd_kcontrol_new mt6735_afe_controls[] = {
	SOC_SINGLE_EXT("HP Impedance DC Value", SND_SOC_NOPM, 0, 0xffff, 0,
		       mt6735_afe_hp_dc_get, mt6735_afe_hp_dc_put),
};

static const struct snd_soc_component_driver mt6735_afe_component = {
	.name = "mtk-afe-pcm",
	.probe = mt6735_afe_component_probe,
	.pcm_construct = mt6735_afe_pcm_new,
	.pointer = mt6735_afe_pcm_pointer,
	.controls = mt6735_afe_controls,
	.num_controls = ARRAY_SIZE(mt6735_afe_controls),
};

static int mt6735_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);

	/* AudDrv_Clk_Off(): clear IRQ status, then gate the audio clocks. */
	mt6735_afe_disable_clk(afe);
	regcache_cache_only(afe->regmap, true);

	return 0;
}

static int mt6735_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int ret;

	regcache_cache_only(afe->regmap, false);

	ret = mt6735_afe_enable_clk(afe);
	if (ret) {
		regcache_cache_only(afe->regmap, true);
		return ret;
	}

	regcache_mark_dirty(afe->regmap);

	return regcache_sync(afe->regmap);
}

static int mt6735_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_dai_driver *dais;
	struct mtk_base_afe *afe;
	struct mt6735_afe_priv *priv;
	int ret, irq, i;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->dev = dev;
	platform_set_drvdata(pdev, afe);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	afe->platform_priv = priv;
	mutex_init(&priv->hp_imp_lock);
	mutex_init(&priv->i2s_con_lock);

	afe->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base_addr))
		return dev_err_probe(dev, PTR_ERR(afe->base_addr),
				     "failed to map AFE registers\n");

	afe->regmap = devm_regmap_init_mmio(dev, afe->base_addr,
					    &mt6735_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return dev_err_probe(dev, PTR_ERR(afe->regmap),
				     "failed to init regmap\n");

	/*
	 * The AFE memory interfaces take 32 bit physical addresses; the MSB
	 * register (AFE_MEMIF_MSB, AudDrv_Afe.h:470) is never programmed by
	 * the downstream driver.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA support\n");

	afe->memif_size = MT6735_AFE_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < MT6735_AFE_MEMIF_NUM; i++) {
		afe->memif[i].data = &mt6735_afe_memif_data[i];
		afe->memif[i].irq_usage = mt6735_afe_memif_irq[i];
		afe->memif[i].const_irq = 1;
	}

	afe->mtk_afe_hardware = &mt6735_afe_hardware;

	ret = mt6735_afe_init_pinctrl(afe);
	if (ret)
		return ret;

	ret = mt6735_afe_init_clk(afe);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/* Power the block up once to make the IRQ request and regmap valid. */
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_put;
	}

	ret = devm_request_irq(dev, irq, mt6735_afe_irq_handler, 0,
			       "Afe_ISR_Handle", afe);
	if (ret)
		goto err_put;

	dais = devm_kcalloc(dev, MT6735_AFE_DAI_NUM, sizeof(*dais),
			    GFP_KERNEL);
	if (!dais) {
		ret = -ENOMEM;
		goto err_put;
	}

	memcpy(dais, mt6735_afe_pcm_dais, sizeof(mt6735_afe_pcm_dais));
	dais[MT6735_AFE_DAI_I2S0] = mt6735_afe_i2s_dai;
	dais[MT6735_AFE_DAI_HPIMP] = mt6735_afe_hp_imp_dai;
	/*
	 * Voice and BT SCO DAIs are contiguous from MT6735_AFE_DAI_BTCVSD and
	 * keep the order and .id of mt6735_afe_voice_dais[].
	 */
	memcpy(&dais[MT6735_AFE_DAI_BTCVSD], mt6735_afe_voice_dais,
	       sizeof(mt6735_afe_voice_dais));
	/* FM I2S back end, memif-less like I2S0, mt6735-dai-fm-i2s.c */
	dais[MT6735_AFE_DAI_FM_I2S] = mt6735_afe_fm_i2s_dai;

	ret = devm_snd_soc_register_component(dev, &mt6735_afe_component,
					      dais, MT6735_AFE_DAI_NUM);
	if (ret)
		goto err_put;

	pm_runtime_put(dev);

	return 0;

err_put:
	pm_runtime_put_sync(dev);
	return ret;
}

static void mt6735_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct mtk_base_afe *afe = platform_get_drvdata(pdev);
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	if (!pm_runtime_status_suspended(afe->dev))
		pm_runtime_force_suspend(afe->dev);

	/* Return the audio pins to their boot state (AudDrv_GPIO_probe()). */
	if (priv->pinctrl && priv->pins_default)
		pinctrl_select_state(priv->pinctrl, priv->pins_default);
}

static const struct of_device_id mt6735_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6735-afe" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6735_afe_pcm_dt_match);

static const struct dev_pm_ops mt6735_afe_pm_ops = {
	RUNTIME_PM_OPS(mt6735_afe_runtime_suspend, mt6735_afe_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static struct platform_driver mt6735_afe_pcm_driver = {
	.driver = {
		.name = "mt6735-afe-pcm",
		.of_match_table = mt6735_afe_pcm_dt_match,
		.pm = pm_ptr(&mt6735_afe_pm_ops),
	},
	.probe = mt6735_afe_pcm_dev_probe,
	.remove = mt6735_afe_pcm_dev_remove,
};

module_platform_driver(mt6735_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek MT6735/MT6737T ALSA SoC AFE platform driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mt6735-afe-pcm");
