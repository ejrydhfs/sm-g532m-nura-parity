/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MT6735/MT6737T ALSA SoC AFE platform driver - shared definitions.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 * sound/soc/mediatek/mt_soc_audio_v3/ (AudDrv_Clk.c, AudDrv_Gpio.c,
 * mt_soc_afe_control.c, mt_soc_pcm_dl1.c, mt_soc_pcm_capture.c,
 * mt_soc_pcm_dl2.c, mt_soc_pcm_dl1_i2s0.c).
 */

#ifndef _MT6735_AFE_COMMON_H_
#define _MT6735_AFE_COMMON_H_

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/pinctrl/consumer.h>
#include <sound/soc.h>

#include "../common/mtk-base-afe.h"
#include "mt6735-afe-clk.h"
#include "mt6735-afe-regs.h"

/*
 * Memory interfaces implemented by this driver.  The values are the memory
 * interface (digital block) numbers used by the downstream driver, because
 * AFE_DAC_CON0 enables block N with bit N + 1
 * (SetMemoryPathEnable(), mt_soc_afe_control.c:1995-1997).
 */
enum {
	MT6735_AFE_MEMIF_DL1 = 0,
	MT6735_AFE_MEMIF_DL2,
	MT6735_AFE_MEMIF_VUL,
	MT6735_AFE_MEMIF_DAI,		/* Soc_Aud_Digital_Block_MEM_DAI = 3 */
	MT6735_AFE_MEMIF_NUM,
};

/* MCU interrupt sources used by the memory interfaces above. */
enum {
	MT6735_AFE_IRQ1 = 0,
	MT6735_AFE_IRQ2,
	MT6735_AFE_IRQ_NUM,
};

/*
 * DAI ids.  The three PCM DAIs must use their memory interface number as
 * id because the PCM pointer calculation indexes afe->memif[] with it.
 * The BT SCO DAI (MT6735_AFE_DAI_BTCVSD) is the fourth MEM_DAI user but it
 * is appended after the I2S0 DAI, so mt6735_afe_dai_memif_id() translates
 * its id to MT6735_AFE_MEMIF_DAI.
 */
enum {
	MT6735_AFE_DAI_DL1 = 0,
	MT6735_AFE_DAI_DL2,
	MT6735_AFE_DAI_VUL,
	MT6735_AFE_DAI_I2S0,
	MT6735_AFE_DAI_HPIMP,
	MT6735_AFE_DAI_BTCVSD,
	MT6735_AFE_DAI_VOICE_MD1,
	MT6735_AFE_DAI_VOICE_MD2,
	MT6735_AFE_DAI_VOICE_MD1_BT,
	MT6735_AFE_DAI_VOICE_MD2_BT,
	/*
	 * FM I2S back end (mt6735-dai-fm-i2s.c).  It is memif-less like the
	 * I2S0 and voice DAIs and is appended after the contiguous voice/BT
	 * block so that MT6735_AFE_VOICE_DAI_IDX() keeps working.
	 */
	MT6735_AFE_DAI_FM_I2S,
	MT6735_AFE_DAI_NUM,
};

#define MT6735_AFE_PCM_DAI_NUM	3

/* Voice and BT SCO DAIs, registered from mt6735-dai-voice.c. */
#define MT6735_AFE_VOICE_DAI_NUM	5
#define MT6735_AFE_VOICE_DAI_IDX(dai_id)	\
	((dai_id) - MT6735_AFE_DAI_BTCVSD)

/* Supported voice parameters, mt_soc_pcm_voice_md1.c:87-101 */
#define MT6735_VOICE_FORMATS	SNDRV_PCM_FMTBIT_S16_LE
#define MT6735_VOICE_RATES	(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000)
#define MT6735_VOICE_RATE_MIN	8000
#define MT6735_VOICE_RATE_MAX	16000
#define MT6735_VOICE_BUFFER_MAX	(36 * 1024)	/* MAX_BUFFER_SIZE, mt_soc_pcm_common.h:132 */
#define MT6735_VOICE_MD2_BUFFER_MAX	(16 * 1024)	/* MAX_BUFFER_SIZE_MD2, mt_soc_pcm_voice_md2.c:74 */

/* BT SCO (BT_DAI_MAX_BUFFER_SIZE, mt_soc_pcm_common.h:119) */
#define MT6735_BTCVSD_BUFFER_MAX	(16 * 1024)
#define MT6735_BTCVSD_PERIODS_MAX	4	/* SOC_NORMAL_USE_PERIODS_MAX, mt_soc_pcm_common.h:194 */

/*
 * FM I2S CPU DAI name, downstream MT_SOC_FM_I2S_NAME
 * (AudDrv_Def.h:230).  The FM codec DAI (rem-wcn-fm-i2s) is the other end of
 * the mt-snd-card link in mt6735-mt6331.c.
 */
#define MT6735_AFE_FM_I2S_DAI_NAME	"mt-soc-fmi2S-driver"

/*
 * The 2nd I2S input from CONNSYS is a fixed 32 kHz, 16 bit stereo link:
 * Set2ndI2SIn() is always called with mI2S_SAMPLERATE = 32000,
 * mI2S_WLEN = WLEN_16BITS (mt_soc_pcm_fm_i2s.c:305-313) and the AFE has no
 * 24 bit or mono mode for this path.
 */
#define MT6735_AFE_FM_I2S_RATE		32000
#define MT6735_AFE_FM_I2S_FORMATS	SNDRV_PCM_FMTBIT_S16_LE
#define MT6735_AFE_FM_I2S_CHANNELS	2

/*
 * Per-DAI state for the voice and BT SCO DAIs.  The AFE has one modem PCM
 * interface (or one DAI_BT block) per DAI, so the state is per DAI and not
 * per substream.
 */
struct mt6735_voice_state {
	unsigned int rate;
	unsigned int prepared_rate;
	int users;
	bool prepared;		/* PCM interface / DAI_BT registers programmed */
	bool running;		/* PCM interface or DAI_BT enabled */
};

/* Supported PCM parameters (mt_soc_pcm_common.h:168-201). */
#define MT6735_AFE_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE)
#define MT6735_AFE_RATES	(SNDRV_PCM_RATE_8000 | \
				 SNDRV_PCM_RATE_11025 | \
				 SNDRV_PCM_RATE_12000 | \
				 SNDRV_PCM_RATE_16000 | \
				 SNDRV_PCM_RATE_22050 | \
				 SNDRV_PCM_RATE_24000 | \
				 SNDRV_PCM_RATE_32000 | \
				 SNDRV_PCM_RATE_44100 | \
				 SNDRV_PCM_RATE_48000)
#define MT6735_AFE_RATE_MIN	8000
#define MT6735_AFE_RATE_MAX	48000
#define MT6735_AFE_CHANNELS_MIN	1
#define MT6735_AFE_CHANNELS_MAX	2

/* Reference rate used to compare interrupt periods (mt_soc_digital_type.h:865) */
#define MT6735_AFE_IRQ_MAX_RATE	260000

/* AudioInterruptLimiter, mt_soc_afe_control.c:164 */
#define MT6735_AFE_IRQ_LIMIT	100

/*
 * Audio clocks listed for the ROM audio node (rom_dtb_0.dts
 * mt_soc_dl1_pcm@11220000 clock-names) and looked up by name in
 * AudDrv_Clk.c:137-157.
 */
enum mt6735_afe_clk_id {
	MT6735_AFE_CLK_AFE,
	MT6735_AFE_CLK_I2S,
	MT6735_AFE_CLK_DAC,
	MT6735_AFE_CLK_DAC_PREDIS,
	MT6735_AFE_CLK_ADC,
	MT6735_AFE_CLK_TML,
	MT6735_AFE_CLK_APLL22M,
	MT6735_AFE_CLK_APLL24M,
	MT6735_AFE_CLK_APLL1_TUNER,
	MT6735_AFE_CLK_APLL2_TUNER,
	MT6735_AFE_CLK_INFRA,
	MT6735_AFE_CLK_MUX1,
	MT6735_AFE_CLK_MUX2,
	MT6735_AFE_CLK_TOP_AD_APLL1,
	MT6735_AFE_CLK_TOP_WHPLL_AUDIO,
	MT6735_AFE_CLK_TOP_MUX_AUDIO,
	MT6735_AFE_CLK_TOP_MUX_AUDIO_INT,
	MT6735_AFE_CLK_TOP_SYS_PLL1_D4,
	MT6735_AFE_CLK_APMIXED_APLL1,
	MT6735_AFE_CLK_APMIXED_APLL2,
	MT6735_AFE_CLK_TOP_CLK26M,
	MT6735_AFE_CLK_NUM,
};

struct mt6735_afe_priv {
	struct clk_bulk_data clks[MT6735_AFE_CLK_NUM];

	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_pmic_mode0;
	struct pinctrl_state *pins_pmic_mode1;
	struct pinctrl_state *pins_i2s_mode0;
	struct pinctrl_state *pins_i2s_mode1;

	/* Memory interface enable state, mirrors mAudioMEMIF[].mState. */
	bool memif_enabled[MT6735_AFE_MEMIF_NUM];

	/* PMIC codec interface state, mirrors I2S_OUT_DAC/I2S_IN_ADC users. */
	bool dac_path_on;
	bool adc_path_on;

	/* AFE_I2S_CON configuration bits, excluding the enable bit. */
	unsigned int i2s_con;
	bool i2s_slave;
	unsigned int i2s_sysclk;

	/*
	 * AFE_I2S_CON (0x0018) is shared between the I2S0 4 wire path
	 * (Enable4pin_I2S0_I2S3(), mt_soc_afe_control.c:3749-3761) and the FM
	 * 2nd I2S input (Set2ndI2SIn(), mt_soc_afe_control.c:2231).  The two
	 * users are mutually exclusive; i2s_con_lock serialises their open
	 * paths and the *_active flags make the second user fail with -EBUSY
	 * instead of overwriting the register.
	 */
	struct mutex i2s_con_lock;
	bool i2s0_active;
	bool fm_i2s_active;

	/* FM I2S back end state, mt6735-dai-fm-i2s.c */
	bool fm_i2s_prepared;	/* FM I2S registers programmed */
	bool fm_i2s_on;		/* AFE_I2S_CON enabled, counts as EnableAfe() */
	bool fm_i2s_dac_owner;	/* this DAI programmed the PMIC I2S DAC */

	unsigned int spurious_irqs;

	/*
	 * HP impedance FE: while hp_imp_open is set the stream holds the DL1
	 * memory interface (rebased onto the AFE internal SRAM) and the AFE
	 * "HP Impedance DC Value" control is allowed to fill the SRAM.
	 * hp_imp_lock serialises that flag against the kcontrol and the DL1
	 * startup path.
	 */
	struct mutex hp_imp_lock;
	bool hp_imp_open;

	/* Voice/BT SCO front ends, mt6735-dai-voice.c */
	struct mt6735_voice_state voice[MT6735_AFE_VOICE_DAI_NUM];
	unsigned int voice_afe_users;	/* EnableAfe() reference count */
	unsigned int daibt_users;	/* AFE_DAIBT_CON0 users */
	bool daibt_merge_on;		/* AFE_MRGIF_CON programmed */
};

static inline struct mt6735_afe_priv *mt6735_afe_priv(struct mtk_base_afe *afe)
{
	return afe->platform_priv;
}

/* mt6735-afe-pcm.c */
int mt6735_afe_rate_to_fs(unsigned int rate);
void mt6735_afe_update_power(struct mtk_base_afe *afe);
int mt6735_afe_irq_update(struct mtk_base_afe *afe, int irq);

/*
 * Front-end register helpers, implemented in mt6735-dai-pcm.c and shared
 * with the HP impedance FE DAI in mt6735-dai-hpimp.c.
 */
bool mt6735_afe_playback_enabled(struct mtk_base_afe *afe);
int mt6735_afe_memif_enable(struct mtk_base_afe *afe, int id, bool enable);
int mt6735_afe_connect(struct mtk_base_afe *afe, int id, bool connect);
void mt6735_afe_set_conn_width(struct mtk_base_afe *afe, int id, bool wlen24);
int mt6735_afe_dac_prepare(struct mtk_base_afe *afe, unsigned int rate,
			   bool wlen32);
void mt6735_afe_dac_disable(struct mtk_base_afe *afe);

/* DAI drivers */
extern const struct snd_soc_dai_driver mt6735_afe_pcm_dais[MT6735_AFE_PCM_DAI_NUM];
extern const struct snd_soc_dai_driver mt6735_afe_i2s_dai;
extern const struct snd_soc_dai_driver mt6735_afe_hp_imp_dai;
extern const struct snd_soc_dai_driver mt6735_afe_fm_i2s_dai;
extern const struct snd_soc_dai_driver
	mt6735_afe_voice_dais[MT6735_AFE_VOICE_DAI_NUM];

#endif /* _MT6735_AFE_COMMON_H_ */
