/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN FM radio - MT6627 FM I2S audio path (ALSA SoC codec).
 *
 * The MT6627 FM receiver outputs its demodulated audio on the connectivity
 * "2 wire" I2S port.  Downstream exposes that port as the DAI links
 * "FM_I2S_RX_Playback"/"FM_I2S_RX_Capture" of the mt-snd-card machine driver
 * and as the codec DAIs "mt-soc-fm-i2s-dai-codec"/"mt-soc-fm-i2s-dummy-dai-
 * codec" of the MT63xx codec driver (sound/soc/mediatek/mt_soc_audio_v3/
 * AudDrv_Def.h:230-231,302-307,325-343, mt_soc_machine.c:1068-1086).  The FM
 * register sequences only ever enable or disable that port; neither the
 * downstream source nor the ROM image contains a sequence that changes the
 * MT6627 I2S clock mode or sample rate (mt6627_I2s_Setting() has no
 * implementation: declaration and use are both inside #if 0,
 * mt6627_fm_lib.c:88-90,1825-1827).
 *
 * This file declares the kernel-internal interface of the ASoC glue.  When
 * CONFIG_SND_SOC is disabled the glue is compiled out (see the Makefile) and
 * the inline fallbacks below keep the FM driver building and running with the
 * chip level I2S hooks only.
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_WCN_FM_I2S_H_
#define _MTK_WCN_FM_I2S_H_

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct mtk_wcn_fm_dev;

/*
 * Opaque FM I2S/ASoC state.  One instance per FM device, allocated by
 * mtk_wcn_fm_i2s_init() and reachable through mtk_wcn_fm_dev.i2s.
 */
struct mtk_wcn_fm_i2s;

/*
 * The ASoC glue can only be used when the sound core is present and linkable
 * from this driver: built-in code cannot reference the symbols of a modular
 * snd-soc-core, so a built-in FM driver with SND_SOC=m falls back to the chip
 * level hooks.  The Makefile implements the same rule for the object list.
 */
#define MTK_WCN_FM_ASOC_ENABLED						\
	(IS_ENABLED(CONFIG_SND_SOC) &&					\
	 !(IS_BUILTIN(CONFIG_MTK_WCN_FM) && IS_MODULE(CONFIG_SND_SOC)))

#if MTK_WCN_FM_ASOC_ENABLED

/*
 * mtk_wcn_fm_i2s_init - register the MT6627 FM I2S codec component
 * @fm: FM device
 *
 * Registers a codec component named "mt6627-fm-i2s" carrying the DAI
 * "mt-soc-fm-i2s-dai-codec" (downstream MT_SOC_CODEC_FM_I2S_DAI_NAME,
 * AudDrv_Def.h:307).  No sound card is registered: the board machine driver
 * owns the card, and has to reference this codec in its FM DAI link (see
 * $M/logs/rem-wcn-fm-i2s.md, "AUDIO-FRAGMENT").  Until it does, the component
 * stays unbound and FM_IOCTL_I2S_SETTING reports -ENODEV.
 *
 * Returns 0 on success, a negative errno otherwise.
 */
int mtk_wcn_fm_i2s_init(struct mtk_wcn_fm_dev *fm);

/*
 * mtk_wcn_fm_i2s_exit - unregister the codec component
 * @fm: FM device
 */
void mtk_wcn_fm_i2s_exit(struct mtk_wcn_fm_dev *fm);

/*
 * mtk_wcn_fm_i2s_power - notify the ASoC glue about an FM power transition
 * @on: true after a successful chip power up, false before a chip power down
 *
 * Only bookkeeping: the chip layer programs the port in both directions.  Call
 * with @on == true after mtk_wcn_fm_chip_power_up() and with @on == false
 * before mtk_wcn_fm_chip_power_down() so that no stream callback can reach the
 * chip while it is powering down.
 */
void mtk_wcn_fm_i2s_power(struct mtk_wcn_fm_dev *fm, bool on);

/*
 * mtk_wcn_fm_i2s_resync - re-apply the wanted port state
 * @fm: FM device
 *
 * Called after mtk_wcn_fm_chip_tune(), which re-enables the chip I2S port.
 */
void mtk_wcn_fm_i2s_resync(struct mtk_wcn_fm_dev *fm);

/*
 * mtk_wcn_fm_i2s_route - apply an FM_IOCTL_I2S_SETTING request
 * @fm: FM device
 * @on: true for FM_I2S_ON, false for FM_I2S_OFF
 *
 * Programs the chip port through mtk_wcn_fm_chip_i2s_set() and keeps the
 * route state used by the stream callbacks in step.  Returns 0 on success,
 * -ENODEV when @on is requested but no machine link bound the codec (the
 * request is then rejected instead of enabling a port that cannot produce
 * audio) and the negative errno of the chip access otherwise.
 */
int mtk_wcn_fm_i2s_route(struct mtk_wcn_fm_dev *fm, bool on);

#else /* !MTK_WCN_FM_ASOC_ENABLED */

/* Provided by mtk_wcn_fm_chip.c; also declared by mtk_wcn_fm.h. */
int mtk_wcn_fm_chip_i2s_set(struct mtk_wcn_fm_dev *fm, bool on);

static inline int mtk_wcn_fm_i2s_init(struct mtk_wcn_fm_dev *fm)
{
	return -EOPNOTSUPP;
}

static inline void mtk_wcn_fm_i2s_exit(struct mtk_wcn_fm_dev *fm)
{
}

static inline void mtk_wcn_fm_i2s_power(struct mtk_wcn_fm_dev *fm, bool on)
{
}

static inline void mtk_wcn_fm_i2s_resync(struct mtk_wcn_fm_dev *fm)
{
}

/*
 * Without ASoC there is no route to keep in step, but the chip port itself is
 * still controllable: the ioctl keeps programming the real registers.
 */
static inline int mtk_wcn_fm_i2s_route(struct mtk_wcn_fm_dev *fm, bool on)
{
	return mtk_wcn_fm_chip_i2s_set(fm, on);
}

#endif /* MTK_WCN_FM_ASOC_ENABLED */

#endif /* _MTK_WCN_FM_I2S_H_ */
