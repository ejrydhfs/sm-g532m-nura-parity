// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN FM radio - MT6627 FM I2S audio path (ALSA SoC codec).
 *
 * Port from Samsung/MTK downstream:
 *   sound/soc/mediatek/mt_soc_audio_v3/mt_soc_machine.c:1068-1086
 *   sound/soc/mediatek/mt_soc_audio_v3/mt_soc_pcm_fm_i2s.c:279-323
 *   sound/soc/mediatek/mt_soc_audio_v3/mt_soc_afe_control.c:2214-2296
 *   sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Def.h:230-231,302-307,325-343
 *
 * Downstream wires the FM I2S port into the single machine card ("mt-snd-card")
 * with the DAI links "FM_I2S_RX_Playback"/"FM_I2S_RX_Capture", the CPU DAI
 * "mt-soc-fmi2S-driver" and the codec DAIs "mt-soc-fm-i2s-dai-codec" /
 * "mt-soc-fm-i2s-dummy-dai-codec".  The data path is the AFE "2nd I2S in"
 * block, programmed from mtk_pcm_fm_i2s_prepare() as a consumer of the
 * connectivity I2S clocks (mI2S_SLAVE = Soc_Aud_I2S_SRC_SLAVE_MODE), at a fixed
 * 32000 Hz, 16 bit words, I2S framing, LRCK not inverted and the input pads
 * taken from CONNSYS (mI2S_IN_PAD_SEL = false), mt_soc_pcm_fm_i2s.c:303-320,
 * mt_soc_afe_control.c:2214-2248.
 *
 * This file owns the *chip* side of that port: a codec component named
 * "mt6627-fm-i2s" carrying the DAI "mt-soc-fm-i2s-dai-codec" that enforces the
 * real MT6627 constraints (master mode, a fixed 32 kHz, 16 bit, stereo I2S
 * frame) and enables/disables the port through mtk_wcn_fm_chip_i2s_set().
 * The AP side - the AFE 2nd I2S input, its
 * connections and the ASRC that downstream used to reach 44.1/48 kHz - belongs
 * to the audio subsystem and is specified in
 * $M/logs/rem-wcn-fm-i2s.md ("AUDIO-FRAGMENT"), because this kernel's
 * mt6735 AFE implements I2S0 only (sound/soc/mediatek/mt6735/mt6735-dai-i2s.c)
 * and remapping AFE registers from the FM driver would be a second, competing
 * audio driver.
 *
 * This driver registers no sound card.  A card can only be owned by the board
 * machine driver (mt6735-mt6331-sound), and ASoC binds a component to exactly
 * one card (soc_probe_component(), soc-core.c:1602-1622), so a second card
 * that shares the AFE would always fail.  Instead the machine driver references
 * "mt6627-fm-i2s" as the codec of the FM link; until it does, the component
 * stays unbound and FM_IOCTL_I2S_SETTING reports -ENODEV.
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mtk_wcn_fm.h"

/*
 * Codec/DAI name and stream names.  The stream names follow downstream
 * MT_SOC_FM_I2S_PLAYBACK_STREAM_NAME / MT_SOC_FM_I2S_CAPTURE_STREAM_NAME
 * (AudDrv_Def.h:342-343) and the DAI name is downstream
 * MT_SOC_CODEC_FM_I2S_DAI_NAME (AudDrv_Def.h:307), which is the name the
 * machine link in mt_soc_machine.c:1068-1086 binds.  The component name is new
 * because this kernel has no MT63xx codec driver: ASoC binds a component by
 * name, and "mt6627-fm-i2s" is the name the board machine driver has to put in
 * its FM DAI link (COMP_CODEC("mt6627-fm-i2s", "mt-soc-fm-i2s-dai-codec")).
 */
#define MTK_WCN_FM_I2S_CODEC_NAME	"mt6627-fm-i2s"
#define MTK_WCN_FM_I2S_DAI_NAME		"mt-soc-fm-i2s-dai-codec"
#define MTK_WCN_FM_I2S_PB_STREAM	"FM_I2S_Playback"
#define MTK_WCN_FM_I2S_CAP_STREAM	"FM_I2S_Capture"

/*
 * The MT6627 I2S port is fixed at master mode with a 32000 Hz frame:
 * mt6627_fm_config.c:116-118 initialises the audio path to FM_AUD_I2S with
 * i2s_info = {FM_I2S_OFF, FM_I2S_MASTER, FM_I2S_32K}, and the AP side is
 * configured for mI2S_SAMPLERATE = 32000 with 16 bit words
 * (mt_soc_pcm_fm_i2s.c:308-312).  No downstream or ROM sequence changes either
 * (mt6627_I2s_Setting() has no implementation, mt6627_fm_lib.c:88-90,1825-1827).
 * The port is a stereo I2S frame (mI2S_IN_PAD_SEL = false feeds both pads from
 * CONNSYS, mt_soc_pcm_fm_i2s.c:306-307); FM mono content is carried inside it
 * and selected by the chip's mono/stereo control (FM_REG_AUDIO_MONO).
 */
#define MTK_WCN_FM_I2S_RATE		32000
#define MTK_WCN_FM_I2S_CHANNELS		2

/*
 * The only DAI format the FM link can carry: I2S framing, no BCLK/LRCK
 * inversion, and the AP side as the bit clock/frame clock consumer while the
 * MT6627 drives BCLK/LRCK (downstream mI2S_SLAVE = Soc_Aud_I2S_SRC_SLAVE_MODE,
 * mt_soc_pcm_fm_i2s.c:309).  This kernel's AFE DAI maps
 * SND_SOC_DAIFMT_CBC_CFC to AFE_I2S_CON_SLAVE (mt6735-dai-i2s.c:79-89) and the
 * audio machine card spells the same interface the same way
 * (mt6735-mt6331.c:152-155); the machine link must therefore use this value.
 */
#define MTK_WCN_FM_I2S_DAIFMT \
	(SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC)

struct mtk_wcn_fm_i2s {
	struct mtk_wcn_fm_dev *fm;
	/* Serialises the state below and every chip access of this file. */
	struct mutex lock;
	/* A card bound the component: a machine link uses this DAI. */
	bool bound;
	/* The FM function is powered (mirror of fm->powered). */
	bool powered;
	/* Userspace enabled the audio path (FM_IOCTL_I2S_SETTING). */
	bool route;
	/* Bitmask of substreams set up with hw_params(). */
	unsigned int armed;
	/* Port state last written to the chip. */
	bool chip_on;
};

static struct mtk_wcn_fm_i2s *mtk_wcn_fm_i2s_from_component(
					struct snd_soc_component *component)
{
	struct mtk_wcn_fm_dev *fm = snd_soc_component_get_drvdata(component);

	return fm ? fm->i2s : NULL;
}

/*
 * The port has to be on while the chip is powered and either userspace asked
 * for audio or a PCM is set up.  The chip power up sequence enables it anyway
 * (mtk_wcn_fm_chip.c, mt6627_fm_lib.c:849-851), so the state only has to be
 * written when it actually differs.  Called with i2s->lock held.
 */
static bool mtk_wcn_fm_i2s_wanted(struct mtk_wcn_fm_i2s *i2s)
{
	return i2s->powered && (i2s->route || i2s->armed);
}

/*
 * Bring the chip port in line with the wanted state.  Called with i2s->lock
 * held from the sleepable ASoC callbacks (component probe/remove, startup,
 * hw_params, hw_free, mute) and from the V4L2 paths; never from an atomic
 * context - the register access goes over STP and sleeps.
 */
static int mtk_wcn_fm_i2s_apply_locked(struct mtk_wcn_fm_i2s *i2s, bool force)
{
	struct mtk_wcn_fm_dev *fm = i2s->fm;
	bool on;
	int ret;

	on = mtk_wcn_fm_i2s_wanted(i2s);

	/*
	 * fm->i2s_on is the last port state known to the chip layer; the power
	 * up, retune and power down sequences update it too, which is how a
	 * retune that re-enabled the port behind our back is caught.
	 */
	if (!force && on == i2s->chip_on && on == READ_ONCE(fm->i2s_on))
		return 0;

	if (!i2s->powered) {
		/* The chip is off: its registers are gone, nothing to write. */
		i2s->chip_on = false;
		return 0;
	}

	ret = mtk_wcn_fm_chip_i2s_set(fm, on);
	if (ret) {
		dev_warn(fm->dev, "FM I2S port %s failed (%d)\n",
			 on ? "enable" : "disable", ret);
		return ret;
	}

	i2s->chip_on = on;

	return 0;
}

static int mtk_wcn_fm_i2s_apply(struct mtk_wcn_fm_i2s *i2s, bool force)
{
	int ret;

	mutex_lock(&i2s->lock);
	ret = mtk_wcn_fm_i2s_apply_locked(i2s, force);
	mutex_unlock(&i2s->lock);

	return ret;
}

/*
 * Codec DAI
 */
static int mtk_wcn_fm_i2s_dai_startup(struct snd_pcm_substream *substream,
				      struct snd_soc_dai *dai)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);
	int ret = 0;

	if (!i2s)
		return -ENODEV;

	mutex_lock(&i2s->lock);
	if (!i2s->powered)
		ret = -ENODEV;
	mutex_unlock(&i2s->lock);

	if (ret)
		dev_dbg(i2s->fm->dev,
			"FM I2S stream opened while the FM function is off\n");

	return ret;
}

static int mtk_wcn_fm_i2s_dai_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params,
					struct snd_soc_dai *dai)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	int ret = 0;

	if (!i2s)
		return -ENODEV;

	if (rate != MTK_WCN_FM_I2S_RATE) {
		dev_err(i2s->fm->dev,
			"MT6627 I2S is fixed at %u Hz, %u Hz rejected\n",
			MTK_WCN_FM_I2S_RATE, rate);
		return -EINVAL;
	}
	if (channels != MTK_WCN_FM_I2S_CHANNELS) {
		dev_err(i2s->fm->dev,
			"MT6627 I2S is a stereo frame, %u channels rejected\n",
			channels);
		return -EINVAL;
	}
	if (params_format(params) != SNDRV_PCM_FORMAT_S16_LE) {
		dev_err(i2s->fm->dev,
			"MT6627 I2S is 16 bit, format %u rejected\n",
			params_format(params));
		return -EINVAL;
	}

	mutex_lock(&i2s->lock);
	if (!i2s->powered) {
		ret = -ENODEV;
	} else {
		i2s->armed |= BIT(substream->stream);
		ret = mtk_wcn_fm_i2s_apply_locked(i2s, false);
	}
	mutex_unlock(&i2s->lock);

	return ret;
}

static int mtk_wcn_fm_i2s_dai_hw_free(struct snd_pcm_substream *substream,
				      struct snd_soc_dai *dai)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);
	int ret;

	if (!i2s)
		return -ENODEV;

	mutex_lock(&i2s->lock);
	i2s->armed &= ~BIT(substream->stream);
	ret = mtk_wcn_fm_i2s_apply_locked(i2s, false);
	mutex_unlock(&i2s->lock);

	return ret;
}

/*
 * shutdown() is the safety net for a stream that was closed without hw_free();
 * it is idempotent with hw_free().
 */
static void mtk_wcn_fm_i2s_dai_shutdown(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);

	if (!i2s)
		return;

	mutex_lock(&i2s->lock);
	i2s->armed &= ~BIT(substream->stream);
	mtk_wcn_fm_i2s_apply_locked(i2s, false);
	mutex_unlock(&i2s->lock);
}

/*
 * trigger() runs with the PCM stream lock held (Documentation/sound/kernel-api/
 * writing-an-alsa-driver.rst:1870-1874: "the trigger callback is atomic"), so
 * it must not touch the chip.  It only refuses to start a stream on a chip that
 * has been powered down under it; the port itself is programmed in hw_params().
 */
static int mtk_wcn_fm_i2s_dai_trigger(struct snd_pcm_substream *substream,
				      int cmd, struct snd_soc_dai *dai)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);

	if (!i2s)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (!READ_ONCE(i2s->powered))
			return -ENODEV;
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		return 0;
	default:
		return -EINVAL;
	}
}

static int mtk_wcn_fm_i2s_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);

	if (!i2s)
		return -ENODEV;

	/*
	 * Only the fixed MT6627 configuration can be carried: I2S framing, no
	 * BCLK/LRCK inversion, and the AFE as clock consumer while the chip
	 * drives the clocks.  Anything else is rejected instead of being
	 * accepted and ignored, because there is no register that could change
	 * the chip's I2S mode (see the file header).
	 */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EOPNOTSUPP;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EOPNOTSUPP;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    SND_SOC_DAIFMT_CBC_CFC)
		return -EOPNOTSUPP;

	return 0;
}

static int mtk_wcn_fm_i2s_dai_mute(struct snd_soc_dai *dai, int mute,
				   int stream)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(dai->component);
	int ret = 0;

	if (!i2s)
		return -ENODEV;

	mutex_lock(&i2s->lock);
	if (i2s->powered)
		ret = mtk_wcn_fm_chip_mute(i2s->fm, mute);
	mutex_unlock(&i2s->lock);

	return ret;
}

static const struct snd_soc_dai_ops mtk_wcn_fm_i2s_dai_ops = {
	.startup = mtk_wcn_fm_i2s_dai_startup,
	.shutdown = mtk_wcn_fm_i2s_dai_shutdown,
	.hw_params = mtk_wcn_fm_i2s_dai_hw_params,
	.hw_free = mtk_wcn_fm_i2s_dai_hw_free,
	.trigger = mtk_wcn_fm_i2s_dai_trigger,
	.set_fmt = mtk_wcn_fm_i2s_dai_set_fmt,
	.mute_stream = mtk_wcn_fm_i2s_dai_mute,
};

static struct snd_soc_dai_driver mtk_wcn_fm_i2s_dai = {
	.name = MTK_WCN_FM_I2S_DAI_NAME,
	.id = 0,
	.ops = &mtk_wcn_fm_i2s_dai_ops,
	.playback = {
		.stream_name = MTK_WCN_FM_I2S_PB_STREAM,
		.channels_min = MTK_WCN_FM_I2S_CHANNELS,
		.channels_max = MTK_WCN_FM_I2S_CHANNELS,
		.rates = SNDRV_PCM_RATE_32000,
		.rate_min = MTK_WCN_FM_I2S_RATE,
		.rate_max = MTK_WCN_FM_I2S_RATE,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = MTK_WCN_FM_I2S_CAP_STREAM,
		.channels_min = MTK_WCN_FM_I2S_CHANNELS,
		.channels_max = MTK_WCN_FM_I2S_CHANNELS,
		.rates = SNDRV_PCM_RATE_32000,
		.rate_min = MTK_WCN_FM_I2S_RATE,
		.rate_max = MTK_WCN_FM_I2S_RATE,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.symmetric_rate = 1,
	.symmetric_channels = 1,
	.symmetric_sample_bits = 1,
};

/*
 * Component.  No DAPM widgets: the codec holds no analog switch, the whole FM
 * audio path is the AFE's 2nd I2S input, so the routing the DAI widgets of this
 * component would drive is empty.
 */
static int mtk_wcn_fm_i2s_component_probe(struct snd_soc_component *component)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(component);

	if (!i2s)
		return -ENODEV;

	mutex_lock(&i2s->lock);
	i2s->bound = true;
	mutex_unlock(&i2s->lock);

	dev_info(i2s->fm->dev,
		 "FM I2S codec bound to sound card \"%s\"\n",
		 component->card->name);

	return 0;
}

static void mtk_wcn_fm_i2s_component_remove(struct snd_soc_component *component)
{
	struct mtk_wcn_fm_i2s *i2s = mtk_wcn_fm_i2s_from_component(component);

	if (!i2s)
		return;

	mutex_lock(&i2s->lock);
	i2s->bound = false;
	i2s->route = false;
	i2s->armed = 0;
	mtk_wcn_fm_i2s_apply_locked(i2s, false);
	mutex_unlock(&i2s->lock);
}

static const struct snd_soc_component_driver mtk_wcn_fm_i2s_component = {
	.name = MTK_WCN_FM_I2S_CODEC_NAME,
	.probe = mtk_wcn_fm_i2s_component_probe,
	.remove = mtk_wcn_fm_i2s_component_remove,
};

/*
 * FM driver interface
 */
int mtk_wcn_fm_i2s_init(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_i2s *i2s;
	int ret;

	i2s = devm_kzalloc(fm->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->fm = fm;
	mutex_init(&i2s->lock);
	i2s->chip_on = READ_ONCE(fm->i2s_on);
	fm->i2s = i2s;

	ret = snd_soc_register_component(fm->dev, &mtk_wcn_fm_i2s_component,
					 &mtk_wcn_fm_i2s_dai, 1);
	if (ret) {
		dev_err(fm->dev, "cannot register the FM I2S codec (%d)\n", ret);
		fm->i2s = NULL;
		return ret;
	}

	dev_dbg(fm->dev, "FM I2S codec \"%s\" registered\n",
		MTK_WCN_FM_I2S_CODEC_NAME);

	return 0;
}

void mtk_wcn_fm_i2s_exit(struct mtk_wcn_fm_dev *fm)
{
	if (!fm->i2s)
		return;

	/*
	 * A card that bound this component holds a module reference on this
	 * driver (snd_soc_component_module_get_when_probe()), so the codec can
	 * only disappear once that card released it - or when the component was
	 * never bound, in which case removing it is trivially safe.
	 */
	snd_soc_unregister_component_by_driver(fm->dev, &mtk_wcn_fm_i2s_component);
	fm->i2s = NULL;
}

/*
 * mtk_wcn_fm_i2s_power - chip power transition notification.
 *
 * Ordering (mtk_wcn_fm_v4l2.c): power_on calls this after
 * mtk_wcn_fm_chip_power_up() succeeded, power_off calls it before
 * mtk_wcn_fm_chip_power_down().  The chip layer owns the register sequence in
 * both directions (the power up sequence enables the port, mt6627_fm_lib.c:
 * 849-851; the power down sequence clears it, mt6627_fm_lib.c:877-880 and
 * mt6627_fm_cmd.c:512-517), so this callback only has to keep the ASoC view in
 * step.  Taking i2s->lock means a stream callback that is already inside
 * mtk_wcn_fm_i2s_apply_locked() completes before the chip starts powering
 * down, and no later callback issues a chip access for the dead function.
 */
void mtk_wcn_fm_i2s_power(struct mtk_wcn_fm_dev *fm, bool on)
{
	struct mtk_wcn_fm_i2s *i2s = fm->i2s;

	if (!i2s)
		return;

	mutex_lock(&i2s->lock);
	if (on) {
		i2s->powered = true;
		/*
		 * The power up sequence leaves the port enabled, which is what
		 * downstream userspace expects; FM_IOCTL_I2S_SETTING(OFF) turns
		 * it off again.
		 */
		i2s->route = true;
		i2s->armed = 0;
		i2s->chip_on = true;
	} else {
		i2s->powered = false;
		i2s->route = false;
		i2s->armed = 0;
		i2s->chip_on = false;
	}
	mutex_unlock(&i2s->lock);
}

/*
 * The chip re-enables the I2S port on every retune (mtk_wcn_fm_chip_tune()),
 * so re-apply the wanted state afterwards.
 */
void mtk_wcn_fm_i2s_resync(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_i2s *i2s = fm->i2s;

	if (!i2s)
		return;

	mtk_wcn_fm_i2s_apply(i2s, false);
}

int mtk_wcn_fm_i2s_route(struct mtk_wcn_fm_dev *fm, bool on)
{
	struct mtk_wcn_fm_i2s *i2s = fm->i2s;
	int ret = 0;

	if (!i2s)
		/* Registration failed: the chip side still works. */
		return mtk_wcn_fm_chip_i2s_set(fm, on);

	mutex_lock(&i2s->lock);
	if (!i2s->powered) {
		ret = -EAGAIN;
	} else if (on && !i2s->bound) {
		/*
		 * No machine link uses this codec, so enabling the chip port
		 * could not produce audio: report the missing link instead of
		 * succeeding silently.
		 */
		ret = -ENODEV;
	} else {
		i2s->route = on;
		ret = mtk_wcn_fm_i2s_apply_locked(i2s, false);
	}
	mutex_unlock(&i2s->lock);

	return ret;
}
