// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE voice and BT SCO front-end DAIs.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/:
 *   mt_soc_pcm_voice_md1.c     - internal modem voice (PCM2_INTF_CON)
 *   mt_soc_pcm_voice_md2.c     - external modem voice (PCM_INTF_CON)
 *   mt_soc_pcm_voice_md1_bt.c  - internal modem <-> BT SCO
 *   mt_soc_pcm_voice_md2_bt.c  - external modem <-> BT SCO
 *   mt_soc_pcm_bt_dai.c        - BT SCO capture into the DAI memory interface
 *   mt_soc_afe_control.c       - SetModemPcmConfig/SetModemPcmEnable,
 *                                SetDaiBt/SetDaiBtEnable, codec path setup
 *   mt_soc_afe_connection.c    - AFE_CONNx routing used by the voice PCMs
 *
 * The downstream voice PCMs have no AP-side DMA: the modem PCM interface
 * (or the DAI_BT block for BT SCO) moves the samples between the modem / BT
 * controller and the AFE, and the AP stream only configures the hardware.
 * mtk_voice_pcm_copy() and mtk_voice1_prepare() in mt_soc_pcm_voice_md1.c
 * therefore never touch the ALSA buffer, which is why the stream position of
 * these DAIs does not advance (see mt6735_afe_dai_memif_id() in
 * mt6735-afe-pcm.c).  The BT SCO DAI is the exception: it is a real memory
 * interface (MEM_DAI), so its pointer tracks AFE_DAI_CUR.
 *
 * Not implemented, with the downstream input that is missing:
 *   - MRG/HDMI/FM/TDM/AWB/offload PCMs (out of scope).
 *   - BT SCO playback: the downstream DAI memory interface has no documented
 *     route towards DAI_BT (mConnectionReg[I10..I13][O02] are all -1); the
 *     AP -> BT direction exists only as DL1 -> O02 in mt_soc_pcm_dl1_bt.c,
 *     which is the separate VOIP_CALL_BT PCM, not this DAI.
 *   - BT -> external-modem second uplink channel: mConnectionReg[I02][O08] is
 *     -1, the downstream SetConnection() call is a no-op.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6735-afe-common.h"

/* Voice DAI kind, selected from the DAI id. */
enum mt6735_voice_kind {
	MT6735_VOICE_MD1 = 0,	/* internal modem, PCM2_INTF_CON (0x53c) */
	MT6735_VOICE_MD2,	/* external modem, PCM_INTF_CON (0x530) */
	MT6735_VOICE_MD1_BT,	/* internal modem <-> DAI_BT */
	MT6735_VOICE_MD2_BT,	/* external modem <-> DAI_BT */
	MT6735_VOICE_DAIBT,	/* BT SCO <-> MEM_DAI */
};

/*
 * Voice hardware parameters, mt_soc_pcm_voice_md1.c:102-118: MAX_BUFFER_SIZE
 * and MAX_PERIOD_SIZE are 36 KiB (mt_soc_pcm_common.h:132-134) and the period
 * count runs 1..4096.  The internal modem PCMs and the two BT variants use
 * that set; the external modem PCM (mt_soc_pcm_voice_md2.c:123-138) uses
 * MAX_BUFFER_SIZE_MD2 = 16 KiB instead (mt_soc_pcm_voice_md2.c:74).
 *
 * 8 kHz (narrow band) and 16 kHz (wide band) mono S16_LE are the PCM modes
 * the AFE implements.  Downstream also lists 32000
 * (soc_voice_supported_sample_rates, mt_soc_pcm_common.h:214-216), but its
 * prepare path programs any rate other than 8 kHz as 16 kHz PCM mode
 * (mt_soc_pcm_voice_md1.c:303-304, mt_soc_pcm_voice_md2.c:296-297), so 32 kHz
 * is rejected here instead of running at half the requested rate.
 */
static const struct snd_pcm_hardware mt6735_voice_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_RESUME |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = MT6735_VOICE_FORMATS,
	.rates = MT6735_VOICE_RATES,
	.rate_min = MT6735_VOICE_RATE_MIN,
	.rate_max = MT6735_VOICE_RATE_MAX,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = MT6735_VOICE_BUFFER_MAX,
	.period_bytes_min = 64,
	.period_bytes_max = MT6735_VOICE_BUFFER_MAX,
	.periods_min = 1,
	.periods_max = 4096,
	.fifo_size = 0,
};

/* External modem PCM, mt_soc_pcm_voice_md2.c:123-138. */
static const struct snd_pcm_hardware mt6735_voice_md2_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_RESUME |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = MT6735_VOICE_FORMATS,
	.rates = MT6735_VOICE_RATES,
	.rate_min = MT6735_VOICE_RATE_MIN,
	.rate_max = MT6735_VOICE_RATE_MAX,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = MT6735_VOICE_MD2_BUFFER_MAX,
	.period_bytes_min = 64,
	.period_bytes_max = MT6735_VOICE_MD2_BUFFER_MAX,
	.periods_min = 1,
	.periods_max = 4096,
	.fifo_size = 0,
};

/*
 * BT SCO hardware parameters, mt_soc_pcm_bt_dai.c:69-78 with
 * BT_DAI_MAX_BUFFER_SIZE = 16 KiB (mt_soc_pcm_common.h:119) and 1..4 periods
 * (SOC_NORMAL_USE_PERIODS_MIN/MAX, mt_soc_pcm_common.h:193-194).
 *
 * SNDRV_PCM_INFO_MMAP/MMAP_VALID are added to the downstream flags
 * (INTERLEAVED only): this DAI has no custom .copy callback, the AFE writes
 * the managed DMA buffer directly, so the standard mmap path is used.
 */
static const struct snd_pcm_hardware mt6735_btcvsd_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = MT6735_VOICE_FORMATS,
	.rates = MT6735_VOICE_RATES,
	.rate_min = MT6735_VOICE_RATE_MIN,
	.rate_max = MT6735_VOICE_RATE_MAX,
	.channels_min = 1,
	.channels_max = 1,
	.buffer_bytes_max = MT6735_BTCVSD_BUFFER_MAX,
	.period_bytes_min = 64,
	.period_bytes_max = MT6735_BTCVSD_BUFFER_MAX,
	.periods_min = 1,
	.periods_max = MT6735_BTCVSD_PERIODS_MAX,
	.fifo_size = 0,
};

static const struct snd_pcm_hardware *
mt6735_voice_pcm_hardware(enum mt6735_voice_kind kind)
{
	switch (kind) {
	case MT6735_VOICE_MD2:
		return &mt6735_voice_md2_hardware;
	case MT6735_VOICE_DAIBT:
		return &mt6735_btcvsd_hardware;
	default:
		return &mt6735_voice_hardware;
	}
}

/*
 * MRG I2S sample rate field used by the merge interface the DAI_BT block is
 * fed from.  SetDaiBtEnable() (mt_soc_afe_control.c:1231-1234) programs it
 * from mAudioMrg->Mrg_I2S_SampleRate, which mt_soc_afe_control.c:501
 * initialises to SampleRateTransform(44100) = 9; the voice-BT and DAI_BT
 * paths never change that value.
 */
#define MT6735_MRGIF_FS_44K	9

static enum mt6735_voice_kind mt6735_voice_kind(int dai_id)
{
	switch (dai_id) {
	case MT6735_AFE_DAI_VOICE_MD1:
		return MT6735_VOICE_MD1;
	case MT6735_AFE_DAI_VOICE_MD2:
		return MT6735_VOICE_MD2;
	case MT6735_AFE_DAI_VOICE_MD1_BT:
		return MT6735_VOICE_MD1_BT;
	case MT6735_AFE_DAI_VOICE_MD2_BT:
		return MT6735_VOICE_MD2_BT;
	default:
		return MT6735_VOICE_DAIBT;
	}
}

static struct mt6735_voice_state *mt6735_voice_state(struct mtk_base_afe *afe,
						    int dai_id)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	return &priv->voice[MT6735_AFE_VOICE_DAI_IDX(dai_id)];
}

/* SetConnection(), one AFE_CONNx bit (mt_soc_afe_connection.c:415-504). */
static void mt6735_voice_conn(struct mtk_base_afe *afe, unsigned int reg,
			      unsigned int bits, bool connect)
{
	regmap_update_bits(afe->regmap, reg, bits, connect ? bits : 0);
}

/*
 * AFE interconnection for each voice PCM, from the prepare/close sequences of
 * the downstream PCMs.  Register and bit for every route are in
 * mt6735-afe-regs.h.
 *
 *   MD1    : mt_soc_pcm_voice_md1.c:219-222, 287-290
 *   MD2    : mt_soc_pcm_voice_md2.c:212-215, 280-283
 *   MD1_BT : mt_soc_pcm_voice_md1_bt.c:192-194, 278-280
 *   MD2_BT : mt_soc_pcm_voice_md2_bt.c:176-178, 262-264
 *   DAIBT  : mt_soc_pcm_bt_dai.c:92,134
 */
static void mt6735_voice_connect(struct mtk_base_afe *afe,
				 enum mt6735_voice_kind kind, bool on)
{
	switch (kind) {
	case MT6735_VOICE_MD1:
		/* modem PCM1 downlink -> codec DAC */
		mt6735_voice_conn(afe, AFE_CONN3, AFE_CONN3_I14_TO_O03, on);
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I14_TO_O04, on);
		/* codec ADC -> modem PCM1 uplink */
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I03_TO_O17, on);
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I04_TO_O18, on);
		break;
	case MT6735_VOICE_MD2:
		/* codec ADC -> external modem uplink */
		mt6735_voice_conn(afe, AFE_CONN2, AFE_CONN2_I03_TO_O07, on);
		mt6735_voice_conn(afe, AFE_CONN2, AFE_CONN2_I04_TO_O08, on);
		/* external modem downlink -> codec DAC */
		mt6735_voice_conn(afe, AFE_CONN1, AFE_CONN1_I09_TO_O03, on);
		mt6735_voice_conn(afe, AFE_CONN2, AFE_CONN2_I09_TO_O04, on);
		break;
	case MT6735_VOICE_MD1_BT:
		/* BT SCO -> modem PCM1 uplink */
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I02_TO_O17, on);
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I02_TO_O18, on);
		/* modem PCM1 downlink -> BT SCO */
		mt6735_voice_conn(afe, AFE_CONN3, AFE_CONN3_I14_TO_O02, on);
		break;
	case MT6735_VOICE_MD2_BT:
		/* BT SCO -> external modem uplink (ch2 has no register) */
		mt6735_voice_conn(afe, AFE_CONN4, AFE_CONN4_I02_TO_O07, on);
		/* external modem downlink -> BT SCO */
		mt6735_voice_conn(afe, AFE_CONN1, AFE_CONN1_I09_TO_O02, on);
		break;
	case MT6735_VOICE_DAIBT:
		/* BT SCO -> MEM_DAI capture buffer */
		mt6735_voice_conn(afe, AFE_CONN3, AFE_CONN3_I02_TO_O11, on);
		break;
	}
}

/*
 * EnableAfe(), mt_soc_afe_control.c:949-1032: the modem PCM and DAI_BT paths
 * have no memory interface, so they are counted separately and OR-ed into
 * the AFE_DAC_CON0 power bit by mt6735_afe_update_power().  The PMIC audio
 * pins follow the AFE state (AudDrv_GPIO_PMIC_Select(),
 * AudDrv_Gpio.c:215-236), but EnableAfe() only returns them to GPIO when
 * CheckMemIfEnable() reports no memory interface left
 * (mt_soc_afe_control.c:903-916, 980-993), hence the memif check below.
 */
static bool mt6735_voice_memif_busy(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	int i;

	for (i = 0; i < MT6735_AFE_MEMIF_NUM; i++)
		if (priv->memif_enabled[i])
			return true;

	return false;
}

static void mt6735_voice_afe_enable(struct mtk_base_afe *afe, bool enable)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	if (enable) {
		if (!priv->voice_afe_users)
			mt6735_afe_select_pmic_pins(afe, true);
		priv->voice_afe_users++;
		mt6735_afe_update_power(afe);
		return;
	}

	if (!priv->voice_afe_users)
		return;

	if (!--priv->voice_afe_users) {
		mt6735_afe_update_power(afe);
		if (!mt6735_voice_memif_busy(afe))
			mt6735_afe_select_pmic_pins(afe, false);
	}
}

/* SetDLSrc2(), mt_soc_afe_control.c:1592-1635 (dense 8k..48k index). */
static int mt6735_voice_dlsrc2_index(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return 0;
	case 16000:
		return 3;
	default:
		return -EINVAL;
	}
}

/*
 * SetI2SDacOut()/SetI2SDacEnable() for the voice rates,
 * mt_soc_afe_control.c:1637-1655 and :2010-2026.  The voice PCMs only use
 * the 16 bit I2S format (Soc_Aud_I2S_WLEN_WLEN_16BITS).
 */
static int mt6735_voice_dac_on(struct mtk_base_afe *afe, unsigned int rate)
{
	int fs, idx;

	fs = mt6735_afe_rate_to_fs(rate);
	if (fs < 0)
		return fs;

	idx = mt6735_voice_dlsrc2_index(rate);
	if (idx < 0)
		return idx;

	/* CleanPreDistortion(), mt_soc_afe_control.c:1582-1590 */
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
	regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);

	/* SetDLSrc2(): voice mode and the -0.3 dB speech gain */
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
		     (idx << 28) | (0x3 << 24) | (0x3 << 11) |
		     (0x1 << 5) | (0x1 << 1));
	regmap_write(afe->regmap, AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);

	/* SetI2SDacOut(): I2S format, 16 bit, no LRCK inversion, no LR swap */
	regmap_write(afe->regmap, AFE_I2S_CON1,
		     (fs << AFE_I2S_CON_FS_SHIFT) |
		     (AFE_I2S_FORMAT_I2S << AFE_I2S_CON_FMT_SHIFT) |
		     (AFE_I2S_WLEN_16BIT << AFE_I2S_CON_WLEN_SHIFT));

	/* SetMemoryPathEnable(I2S_OUT_DAC, true) */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_I2S_OUT_DAC_EN,
			   AFE_DAC_CON0_I2S_OUT_DAC_EN);

	/* SetI2SDacEnable(true) */
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
			   AFE_ADDA_UL_DL_CON0_EN, AFE_ADDA_UL_DL_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
			   AFE_ADDA_DL_SRC2_CON0_EN, AFE_ADDA_DL_SRC2_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_I2S_CON1, AFE_I2S_CON1_EN,
			   AFE_I2S_CON1_EN);

	return 0;
}

/*
 * SetI2SAdcIn()/SetI2SAdcEnable() with the internal ADC,
 * mt_soc_afe_control.c:1346-1370 and :1553-1572.  AudioAdcI2SStatus is false
 * unless an external ADC is selected (mt_soc_afe_control.c:497), so the
 * internal ADC branch is the one the voice PCMs use.
 */
static int mt6735_voice_adc_on(struct mtk_base_afe *afe, unsigned int rate)
{
	unsigned int voice_mode;

	switch (rate) {
	case 8000:
		voice_mode = 0;
		break;
	case 16000:
		voice_mode = 1;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(afe->regmap, AFE_ADDA_TOP_CON0,
			   AFE_ADDA_TOP_CON0_EXT_ADC, 0);
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0, 0x001e0000,
			   (voice_mode << 19) | (voice_mode << 17));
	regmap_write(afe->regmap, AFE_ADDA_NEWIF_CFG0, 0x03f87201);
	/* dVoiceModeSelect is 0 or 1 for the two voice rates */
	regmap_update_bits(afe->regmap, AFE_ADDA_NEWIF_CFG1, 0x00000c00,
			   1 << 10);

	/* SetMemoryPathEnable(I2S_IN_ADC, true) */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_I2S_IN_ADC_EN,
			   AFE_DAC_CON0_I2S_IN_ADC_EN);

	/* SetI2SAdcEnable(true) */
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
			   AFE_ADDA_UL_DL_CON0_EN, AFE_ADDA_UL_DL_CON0_EN);
	regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
			   AFE_ADDA_UL_SRC_CON0_EN, AFE_ADDA_UL_SRC_CON0_EN);

	return 0;
}

/*
 * SetI2SDacEnable(false)/SetI2SAdcEnable(false),
 * mt_soc_afe_control.c:2010-2034 and :1553-1572.  The downstream driver
 * gates these on the I2S_OUT_DAC / I2S_IN_ADC user counts; the multimedia
 * path keeps its state in priv->dac_path_on / adc_path_on and in the memory
 * interfaces, so those are honoured here as well.
 */
static void mt6735_voice_codec_off(struct mtk_base_afe *afe)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	bool dac_used = priv->dac_path_on ||
			priv->memif_enabled[MT6735_AFE_MEMIF_DL1] ||
			priv->memif_enabled[MT6735_AFE_MEMIF_DL2];
	bool adc_used = priv->adc_path_on ||
			priv->memif_enabled[MT6735_AFE_MEMIF_VUL];

	if (!dac_used) {
		regmap_update_bits(afe->regmap, AFE_ADDA_DL_SRC2_CON0,
				   AFE_ADDA_DL_SRC2_CON0_EN, 0);
		regmap_update_bits(afe->regmap, AFE_I2S_CON1,
				   AFE_I2S_CON1_EN, 0);
		regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				   AFE_DAC_CON0_I2S_OUT_DAC_EN, 0);
	}

	if (!adc_used) {
		regmap_update_bits(afe->regmap, AFE_ADDA_UL_SRC_CON0,
				   AFE_ADDA_UL_SRC_CON0_EN, 0);
		regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				   AFE_DAC_CON0_I2S_IN_ADC_EN, 0);
	}

	if (!dac_used && !adc_used)
		regmap_update_bits(afe->regmap, AFE_ADDA_UL_DL_CON0,
				   AFE_ADDA_UL_DL_CON0_EN, 0);
}

/*
 * SetModemPcmConfig() for the internal modem, mt_soc_afe_control.c:1732-1785:
 * PCM2_INTF_CON plus the ASRC2/ASRC3 ratio registers.  Values are the
 * Voice1Pcm attributes of mt_soc_pcm_voice_md1.c:81-96 with
 * mAsyncFifoSel = Soc_Aud_BYPASS_SRC_SLAVE_USE_ASYNC_FIFO and
 * mPcmModeWidebandSel from the stream rate (mt_soc_pcm_voice_md1.c:303-304).
 */
static int mt6735_voice_md1_config(struct mtk_base_afe *afe, unsigned int rate)
{
	unsigned int wideband, val;

	if (rate == 8000) {
		wideband = PCM_MODE_8K;
		regmap_write(afe->regmap, AFE_ASRC2_CON1, 0x00098580);
		regmap_write(afe->regmap, AFE_ASRC2_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC2_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC2_CON4, 0x00098580);
		regmap_write(afe->regmap, AFE_ASRC2_CON7, 0x0004c2c0);
		regmap_write(afe->regmap, AFE_ASRC3_CON1, 0x00098580);
		regmap_write(afe->regmap, AFE_ASRC3_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC3_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC3_CON4, 0x00098580);
		regmap_write(afe->regmap, AFE_ASRC3_CON7, 0x0004c2c0);
	} else if (rate == 16000) {
		wideband = PCM_MODE_16K;
		regmap_write(afe->regmap, AFE_ASRC2_CON1, 0x0004c2c0);
		regmap_write(afe->regmap, AFE_ASRC2_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC2_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC2_CON4, 0x0004c2c0);
		regmap_write(afe->regmap, AFE_ASRC2_CON7, 0x00026160);
		regmap_write(afe->regmap, AFE_ASRC3_CON1, 0x0004c2c0);
		regmap_write(afe->regmap, AFE_ASRC3_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC3_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC3_CON4, 0x0004c2c0);
		regmap_write(afe->regmap, AFE_ASRC3_CON7, 0x00026160);
	} else {
		return -EINVAL;
	}

	val = (PCM_TX_LCH_RPT_NO_REPEAT << 13) |
	      (PCM_VBT_16K_DISABLE << 12) |
	      (PCM_BT_MODE_DUAL_MIC_ON_TX << 7) |
	      (PCM_AFIFO_ASYNC_FIFO << 6) |
	      (PCM_WLEN_16BIT << 5) |
	      (wideband << 3) |
	      (PCM_FMT_MODE_B << 1);

	/* Afe_Set_Reg(PCM2_INTF_CON, val, MASK_ALL), :1742 */
	regmap_write(afe->regmap, PCM2_INTF_CON, val);

	return 0;
}

/*
 * SetModemPcmConfig() for the external modem, mt_soc_afe_control.c:1786-1838:
 * PCM_INTF_CON plus the ASRC/ASRC4 ratio registers.  Values are the
 * Voice2IntPcm attributes of mt_soc_pcm_voice_md2.c:87-106 (mAsyncFifoSel
 * stays Soc_Aud_BYPASS_SRC_SLAVE_USE_ASRC, the async FIFO assignment is
 * commented out at mt_soc_pcm_voice_md2.c:298).
 */
static int mt6735_voice_md2_config(struct mtk_base_afe *afe, unsigned int rate)
{
	unsigned int wideband, val;

	if (rate == 8000) {
		wideband = PCM_MODE_8K;
		regmap_write(afe->regmap, AFE_ASRC_CON1, 0x00065900);
		regmap_write(afe->regmap, AFE_ASRC_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC_CON4, 0x00065900);
		regmap_write(afe->regmap, AFE_ASRC_CON6, 0x007f188f);
		regmap_write(afe->regmap, AFE_ASRC_CON7, 0x00032c80);
		regmap_write(afe->regmap, AFE_ASRC4_CON1, 0x00065900);
		regmap_write(afe->regmap, AFE_ASRC4_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC4_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC4_CON4, 0x00065900);
		regmap_write(afe->regmap, AFE_ASRC4_CON6, 0x007f188f);
		regmap_write(afe->regmap, AFE_ASRC4_CON7, 0x00032c80);
	} else if (rate == 16000) {
		wideband = PCM_MODE_16K;
		regmap_write(afe->regmap, AFE_ASRC_CON1, 0x00032c80);
		regmap_write(afe->regmap, AFE_ASRC_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC_CON4, 0x00032c80);
		regmap_write(afe->regmap, AFE_ASRC_CON6, 0x007f188f);
		regmap_write(afe->regmap, AFE_ASRC_CON7, 0x00019640);
		regmap_write(afe->regmap, AFE_ASRC4_CON1, 0x00032c80);
		regmap_write(afe->regmap, AFE_ASRC4_CON2, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC4_CON3, 0x00400000);
		regmap_write(afe->regmap, AFE_ASRC4_CON4, 0x00032c80);
		regmap_write(afe->regmap, AFE_ASRC4_CON6, 0x007f188f);
		regmap_write(afe->regmap, AFE_ASRC4_CON7, 0x00019640);
	} else {
		return -EINVAL;
	}

	val = (0 << 22) |				/* mBclkOutInv = false */
	      (PCM_TX_LCH_RPT_NO_REPEAT << 19) |
	      (PCM_VBT_16K_DISABLE << 18) |
	      (PCM_EXT_MODEM_INTERNAL << 17) |
	      (0 << 9) |				/* mExtendBckSyncLength = 0 */
	      (PCM_SYNC_ONE_BCK_CYCLE << 8) |
	      (PCM_BT_MODE_DUAL_MIC_ON_TX << 7) |
	      (PCM_AFIFO_ASRC << 6) |
	      (PCM_CLOCK_SLAVE << 5) |
	      (wideband << 3) |
	      (PCM_FMT_MODE_B << 1);

	/* Afe_Set_Reg(PCM_INTF_CON, val, MASK_ALL), :1838 */
	regmap_write(afe->regmap, PCM_INTF_CON, val);

	return 0;
}

/*
 * SetModemPcmEnable(), mt_soc_afe_control.c:1845-1886.  MODEM_1 only flips
 * PCM2_INTF_CON bit 0; the external modem also enables the ASRC blocks on
 * start and tears them down on stop.
 */
static int mt6735_voice_modem_enable(struct mtk_base_afe *afe,
				     enum mt6735_voice_kind kind, bool enable)
{
	unsigned int pcm1_afifo;

	if (kind == MT6735_VOICE_MD1 || kind == MT6735_VOICE_MD1_BT) {
		regmap_update_bits(afe->regmap, PCM2_INTF_CON, PCM2_INTF_CON_EN,
				   enable ? PCM2_INTF_CON_EN : 0);
		return 0;
	}

	if (enable) {
		int ret;

		regmap_update_bits(afe->regmap, PCM_INTF_CON2,
				   PCM_INTF_CON2_MD3_SEL,
				   PCM_INTF_CON2_MD3_SEL);
		/*
		 * The external modem PCM runs through the ASRC unless the
		 * interface was configured for the async FIFO; the voice path
		 * sets mAsyncFifoSel = ASRC, so this is the branch taken.
		 */
		ret = regmap_read(afe->regmap, PCM_INTF_CON, &pcm1_afifo);
		if (ret)
			return ret;
		if (!(pcm1_afifo & PCM_INTF_CON_BYP_ASRC)) {
			regmap_write(afe->regmap, AFE_ASRC_CON0, 0x86083031);
			regmap_write(afe->regmap, AFE_ASRC4_CON0, 0x06003031);
		}
		regmap_update_bits(afe->regmap, PCM_INTF_CON, PCM_INTF_CON_EN,
				   PCM_INTF_CON_EN);
		return 0;
	}

	regmap_update_bits(afe->regmap, PCM_INTF_CON, PCM_INTF_CON_EN, 0);
	regmap_write(afe->regmap, AFE_ASRC_CON6, 0);
	regmap_update_bits(afe->regmap, AFE_ASRC_CON0,
			   BIT(4) | BIT(5) | BIT(0), 0);
	regmap_update_bits(afe->regmap, AFE_ASRC_CON0, BIT(0), 0);
	regmap_write(afe->regmap, AFE_ASRC4_CON6, 0);
	regmap_update_bits(afe->regmap, AFE_ASRC4_CON0, BIT(4) | BIT(5), 0);
	regmap_update_bits(afe->regmap, AFE_ASRC4_CON0, BIT(0), 0);

	return 0;
}

/*
 * SetDaiBt()/SetDaiBtEnable(), mt_soc_afe_control.c:1207-1258.  The merge
 * interface is brought up once (this port has no other MRGIF user) and the
 * DAI_BT block is fed from it; the BT mode comes from the stream rate
 * (mt_soc_pcm_voice_md1_bt.c:265-276, mt_soc_pcm_bt_dai.c:104-115).
 */
static int mt6735_voice_daibt_config(struct mtk_base_afe *afe,
				     unsigned int rate)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int mode;

	if (rate == 8000)
		mode = AFE_DAIBT_MODE_8K;
	else if (rate == 16000)
		mode = AFE_DAIBT_MODE_16K;
	else
		return -EINVAL;

	if (!priv->daibt_merge_on) {
		regmap_update_bits(afe->regmap, AFE_MRGIF_CON,
				   AFE_MRGIF_CON_FS_MASK <<
				   AFE_MRGIF_CON_FS_SHIFT,
				   MT6735_MRGIF_FS_44K <<
				   AFE_MRGIF_CON_FS_SHIFT);
		regmap_update_bits(afe->regmap, AFE_MRGIF_CON,
				   AFE_MRGIF_CON_I2S_EN, AFE_MRGIF_CON_I2S_EN);
		regmap_update_bits(afe->regmap, AFE_MRGIF_CON,
				   AFE_MRGIF_CON_EN, AFE_MRGIF_CON_EN);
		udelay(100);
		priv->daibt_merge_on = true;
	}

	regmap_update_bits(afe->regmap, AFE_DAIBT_CON0,
			   BIT(AFE_DAIBT_CON0_MODE_SHIFT) |
			   AFE_DAIBT_CON0_USE_MERGE |
			   AFE_DAIBT_CON0_DATA_READY,
			   (mode << AFE_DAIBT_CON0_MODE_SHIFT) |
			   AFE_DAIBT_CON0_USE_MERGE |
			   AFE_DAIBT_CON0_DATA_READY);

	return 0;
}

/*
 * The DAI_BT / BT SCO "on" state is AFE_DAIBT_CON0 bits 1:0,
 * mt_soc_afe_control.c:1229,1238.  Tearing down the merge interface when the
 * last DAI_BT user stops mirrors mt_soc_afe_control.c:1244-1252.
 */
static void mt6735_voice_daibt_enable(struct mtk_base_afe *afe, bool enable)
{
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);

	if (enable) {
		regmap_update_bits(afe->regmap, AFE_DAIBT_CON0,
				   AFE_DAIBT_CON0_EN, AFE_DAIBT_CON0_EN);
		priv->daibt_users++;
		return;
	}

	if (!priv->daibt_users)
		return;

	if (--priv->daibt_users)
		return;

	regmap_update_bits(afe->regmap, AFE_DAIBT_CON0, AFE_DAIBT_CON0_EN, 0);
	udelay(100);
	regmap_update_bits(afe->regmap, AFE_MRGIF_CON, AFE_MRGIF_CON_I2S_EN, 0);
	regmap_update_bits(afe->regmap, AFE_MRGIF_CON, AFE_MRGIF_CON_EN, 0);
	priv->daibt_merge_on = false;
}

/*
 * SetMemoryPathEnable(MEM_DAI, ...), mt_soc_afe_control.c:1968-2000.  This is
 * the same register bit the memory interface descriptors use
 * (mt6735-afe-pcm.c), the helper exists because mt6735_afe_memif_enable() is
 * private to mt6735-dai-pcm.c.
 */
static void mt6735_voice_memif_dai_enable(struct mtk_base_afe *afe, bool enable)
{
	const struct mtk_base_memif_data *data =
		afe->memif[MT6735_AFE_MEMIF_DAI].data;
	struct mt6735_afe_priv *priv = mt6735_afe_priv(afe);
	unsigned int bit = BIT(data->enable_shift);

	regmap_update_bits(afe->regmap, data->enable_reg, bit,
			   enable ? bit : 0);
	priv->memif_enabled[MT6735_AFE_MEMIF_DAI] = enable;
	mt6735_afe_update_power(afe);
}

static int mt6735_voice_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_voice_state *st = mt6735_voice_state(afe, dai->id);
	enum mt6735_voice_kind kind = mt6735_voice_kind(dai->id);
	const struct snd_pcm_hardware *hw;
	int ret;

	hw = mt6735_voice_pcm_hardware(kind);

	ret = pm_runtime_get_sync(afe->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(afe->dev);
		return ret;
	}

	if (!st->users) {
		st->rate = 0;
		st->prepared = false;
		st->running = false;
	}
	st->users++;

	snd_soc_set_runtime_hwparams(substream, hw);

	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto err_put;

	if (kind == MT6735_VOICE_DAIBT) {
		/*
		 * The memory interface consumes whole 64 byte blocks
		 * (Align64ByteSize(), mt_soc_afe_control.c:3702-3710).
		 */
		ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
						 64);
		if (ret < 0)
			goto err_put;

		afe->memif[MT6735_AFE_MEMIF_DAI].substream = substream;
	}

	return 0;

err_put:
	st->users--;
	pm_runtime_put_noidle(afe->dev);
	return ret;
}

static void mt6735_voice_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_voice_state *st = mt6735_voice_state(afe, dai->id);
	enum mt6735_voice_kind kind = mt6735_voice_kind(dai->id);

	if (!st->users)
		return;

	if (!--st->users) {
		if (kind == MT6735_VOICE_DAIBT ||
		    kind == MT6735_VOICE_MD1_BT ||
		    kind == MT6735_VOICE_MD2_BT) {
			mt6735_voice_daibt_enable(afe, false);
		}
		if (kind != MT6735_VOICE_DAIBT)
			mt6735_voice_modem_enable(afe, kind, false);

		/* mtk_voice_close()/mtk_voice_md2_close() :219-236 / :212-229 */
		mt6735_voice_connect(afe, kind, false);

		if (kind == MT6735_VOICE_MD1 || kind == MT6735_VOICE_MD2)
			mt6735_voice_codec_off(afe);

		if (kind == MT6735_VOICE_DAIBT) {
			mt6735_voice_memif_dai_enable(afe, false);
			afe->memif[MT6735_AFE_MEMIF_DAI].substream = NULL;
			mt6735_afe_irq_update(afe, MT6735_AFE_IRQ2);
		}

		mt6735_voice_afe_enable(afe, false);

		st->rate = 0;
		st->prepared = false;
		st->running = false;
	}

	pm_runtime_put(afe->dev);
}

/* Reject everything the voice / BT SCO interfaces cannot represent. */
static int mt6735_voice_check_params(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	if (rate != 8000 && rate != 16000)
		return -EINVAL;

	if (params_channels(params) != 1)
		return -EINVAL;

	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	return 0;
}

static int mt6735_voice_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_voice_state *st = mt6735_voice_state(afe, dai->id);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	ret = mt6735_voice_check_params(params);
	if (ret)
		return ret;

	st->rate = params_rate(params);

	if (mt6735_voice_kind(dai->id) != MT6735_VOICE_DAIBT)
		return 0;

	if (!runtime->dma_bytes || !runtime->dma_addr)
		return -EINVAL;

	/* The memory interfaces only decode 32 bit addresses, AudDrv_Afe.h:470 */
	if (upper_32_bits(runtime->dma_addr))
		return -EINVAL;

	/* SetDAIBuffer(), mt_soc_pcm_bt_dai.c:228-240 */
	regmap_write(afe->regmap, AFE_DAI_BASE,
		     lower_32_bits(runtime->dma_addr));
	regmap_write(afe->regmap, AFE_DAI_END,
		     lower_32_bits(runtime->dma_addr) + runtime->dma_bytes - 1);

	/* SetSampleRate(MEM_DAI), mt_soc_afe_control.c:1099-1112 */
	regmap_update_bits(afe->regmap, AFE_DAC_CON0,
			   AFE_DAC_CON0_DAI_FS_MASK << AFE_DAC_CON0_DAI_FS_SHIFT,
			   ((st->rate == 8000 ? AFE_DAI_FS_8K : AFE_DAI_FS_16K)
			    << AFE_DAC_CON0_DAI_FS_SHIFT));

	/* SetMemIfFetchFormatPerSample(MEM_DAI, 16 bit), :2343-2346 */
	regmap_update_bits(afe->regmap, AFE_MEMIF_PBUF_SIZE,
			   AFE_MEMIF_PBUF_FMT_MASK << AFE_MEMIF_PBUF_DAI_FMT_SHIFT,
			   AFE_WLEN_16_BIT << AFE_MEMIF_PBUF_DAI_FMT_SHIFT);

	return 0;
}

static int mt6735_voice_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_voice_state *st = mt6735_voice_state(afe, dai->id);
	enum mt6735_voice_kind kind = mt6735_voice_kind(dai->id);
	bool codec_path = (kind == MT6735_VOICE_MD1 || kind == MT6735_VOICE_MD2);
	bool afe_on = false;
	int ret;

	if (st->prepared && st->prepared_rate == st->rate)
		return 0;

	if (!st->rate)
		return -EINVAL;

	if (st->prepared) {
		/* The rate changed while the DAI stayed open: reprogram. */
		mt6735_voice_connect(afe, kind, false);
		if (codec_path)
			mt6735_voice_codec_off(afe);
		mt6735_voice_afe_enable(afe, false);
		st->prepared = false;
	}

	mt6735_voice_connect(afe, kind, true);

	/*
	 * Order of mtk_voice1_prepare()/mtk_voice_md2_prepare(),
	 * mt_soc_pcm_voice_md1.c:284-300 / mt_soc_pcm_voice_md2.c:268-300:
	 * codec downlink/uplink, EnableAfe(), then the modem PCM attributes.
	 * The BT variants (mt_soc_pcm_voice_md1_bt.c:257-284) configure the
	 * DAI_BT block instead of the codec, and the DAI_BT PCM
	 * (mt_soc_pcm_bt_dai.c:119-147) has no AFE power call of its own.
	 */
	if (codec_path) {
		ret = mt6735_voice_dac_on(afe, st->rate);
		if (ret)
			goto err_disconnect;
		ret = mt6735_voice_adc_on(afe, st->rate);
		if (ret)
			goto err_disconnect;
	} else
		ret = mt6735_voice_daibt_config(afe, st->rate);

	if (ret)
		goto err_disconnect;

	if (kind != MT6735_VOICE_DAIBT) {
		mt6735_voice_afe_enable(afe, true);
		afe_on = true;
	}

	if (kind != MT6735_VOICE_DAIBT) {
		if (kind == MT6735_VOICE_MD2)
			ret = mt6735_voice_md2_config(afe, st->rate);
		else
			ret = mt6735_voice_md1_config(afe, st->rate);
		if (ret)
			goto err_afe;
	}

	st->prepared_rate = st->rate;
	st->prepared = true;

	return 0;

err_afe:
	if (afe_on)
		mt6735_voice_afe_enable(afe, false);
err_disconnect:
	mt6735_voice_connect(afe, kind, false);
	return ret;
}

static int mt6735_voice_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	struct mt6735_voice_state *st = mt6735_voice_state(afe, dai->id);
	enum mt6735_voice_kind kind = mt6735_voice_kind(dai->id);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!st->prepared)
			return -EINVAL;
		if (st->running)
			return 0;

		if (kind == MT6735_VOICE_DAIBT) {
			mt6735_voice_memif_dai_enable(afe, true);
			mt6735_voice_daibt_enable(afe, true);
			mt6735_afe_irq_update(afe, MT6735_AFE_IRQ2);
		} else if (kind == MT6735_VOICE_MD1_BT ||
			   kind == MT6735_VOICE_MD2_BT) {
			mt6735_voice_daibt_enable(afe, true);
			mt6735_voice_modem_enable(afe, kind, true);
		} else {
			mt6735_voice_modem_enable(afe, kind, true);
		}

		st->running = true;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (!st->running)
			return 0;

		if (kind == MT6735_VOICE_DAIBT) {
			mt6735_voice_daibt_enable(afe, false);
			mt6735_voice_memif_dai_enable(afe, false);
			mt6735_afe_irq_update(afe, MT6735_AFE_IRQ2);
		} else if (kind == MT6735_VOICE_MD1_BT ||
			   kind == MT6735_VOICE_MD2_BT) {
			mt6735_voice_modem_enable(afe, kind, false);
			mt6735_voice_daibt_enable(afe, false);
		} else {
			mt6735_voice_modem_enable(afe, kind, false);
		}

		st->running = false;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mt6735_voice_ops = {
	.startup = mt6735_voice_startup,
	.shutdown = mt6735_voice_shutdown,
	.hw_params = mt6735_voice_hw_params,
	.prepare = mt6735_voice_prepare,
	.trigger = mt6735_voice_trigger,
};

/*
 * Voice / BT SCO DAI drivers.  The array order and the .id values must match
 * the id block in mt6735-afe-common.h, mt6735-afe-pcm.c copies it right after
 * MT6735_AFE_DAI_I2S0.
 *
 * The modem PCM interface is shared by both directions, so the rate, channel
 * count and sample width are symmetric.  8 kHz is narrow band and 16 kHz is
 * wide band; both are mono S16_LE.
 */
const struct snd_soc_dai_driver mt6735_afe_voice_dais[MT6735_AFE_VOICE_DAI_NUM] = {
	{
		.name = "mt-soc-btcvsd-dai",
		.id = MT6735_AFE_DAI_BTCVSD,
		.ops = &mt6735_voice_ops,
		/* mt_soc_pcm_bt_dai.c:304-307 rejects playback */
		.capture = {
			.stream_name = "BT SCO Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
	},
	{
		.name = "mt-soc-voice-md1-dai",
		.id = MT6735_AFE_DAI_VOICE_MD1,
		.ops = &mt6735_voice_ops,
		.playback = {
			.stream_name = "Voice MD1 Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.capture = {
			.stream_name = "Voice MD1 Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.symmetric_rate = 1,
		.symmetric_channels = 1,
		.symmetric_sample_bits = 1,
	},
	{
		.name = "mt-soc-voice-md2-dai",
		.id = MT6735_AFE_DAI_VOICE_MD2,
		.ops = &mt6735_voice_ops,
		.playback = {
			.stream_name = "Voice MD2 Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.capture = {
			.stream_name = "Voice MD2 Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.symmetric_rate = 1,
		.symmetric_channels = 1,
		.symmetric_sample_bits = 1,
	},
	{
		.name = "mt-soc-voice-md1-bt-dai",
		.id = MT6735_AFE_DAI_VOICE_MD1_BT,
		.ops = &mt6735_voice_ops,
		.playback = {
			.stream_name = "Voice MD1 BT Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.capture = {
			.stream_name = "Voice MD1 BT Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.symmetric_rate = 1,
		.symmetric_channels = 1,
		.symmetric_sample_bits = 1,
	},
	{
		.name = "mt-soc-voice-md2-bt-dai",
		.id = MT6735_AFE_DAI_VOICE_MD2_BT,
		.ops = &mt6735_voice_ops,
		.playback = {
			.stream_name = "Voice MD2 BT Playback",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.capture = {
			.stream_name = "Voice MD2 BT Capture",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MT6735_VOICE_RATES,
			.rate_min = MT6735_VOICE_RATE_MIN,
			.rate_max = MT6735_VOICE_RATE_MAX,
			.formats = MT6735_VOICE_FORMATS,
		},
		.symmetric_rate = 1,
		.symmetric_channels = 1,
		.symmetric_sample_bits = 1,
	},
};
