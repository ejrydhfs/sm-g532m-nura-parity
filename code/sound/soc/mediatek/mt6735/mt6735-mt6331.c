// SPDX-License-Identifier: GPL-2.0
/*
 * mt6735-mt6331.c -- MT6735 / MT6331 sound card machine driver
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * Port from Samsung/MTK downstream
 *   sound/soc/mediatek/mt_soc_audio_v3/mt_soc_machine.c
 *   sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Def.h
 *
 * The downstream driver built its PCM/platform devices from static tables and
 * registered the card through platform_device_alloc("soc-audio").  This port
 * keeps the card name, DAI-link names, stream names and CPU/codec DAI names of
 * the ROM, but registers the card as an ordinary platform driver whose device
 * is described by "mediatek,mt6735-sound".
 *
 * Headset detection: the downstream machine driver had no jack at all; the
 * ACCDET driver (drivers/misc/mediatek/accdet) registered its own input device
 * and reported KEY_PLAYPAUSE/KEY_VOLUMEUP/KEY_VOLUMEDOWN/KEY_VOICECOMMAND on
 * it.  This driver instead owns a single ASoC jack, lets the ASoC jack supply
 * the input device, and translates the ACCDET blocking-notifier events into
 * snd_soc_jack_report() calls.  That deliberately avoids the duplicate
 * reporting path of the downstream driver, where both ACCDET and (on some
 * projects) the codec reported the same keys.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/input.h>
#include <linux/mfd/mt6328/accdet.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <sound/jack.h>
#include <sound/soc.h>

/* Card name, mt_soc_machine.c:1138 */
#define MT6735_CARD_NAME	"mt-snd-card"

/* Stream names, AudDrv_Def.h:311-329 */
#define MT6735_MM1_STREAM	"MultiMedia1_PLayback"
#define MT6735_MM2_STREAM	"MultiMedia1_Capture"
#define MT6735_DL2_STREAM	"MultiMedia2_PLayback"
#define MT6735_I2S0_STREAM	"I2S0_PLayback"
#define MT6735_VOICE_MD1_STREAM	"Voice_MD1_PLayback"
#define MT6735_VOICE_MD2_STREAM	"Voice_MD2_PLayback"
#define MT6735_VOICE_MD1_BT_STREAM "Voice_MD1_BT_Playback"
#define MT6735_VOICE_MD2_BT_STREAM "Voice_MD2_BT_Playback"
#define MT6735_DAI_STREAM	"MultiMedia_dai_Capture"

#define MT6735_JACK_BTN_MASK	(SND_JACK_BTN_0 | SND_JACK_BTN_1 | \
				 SND_JACK_BTN_2 | SND_JACK_BTN_3)
#define MT6735_JACK_MASK	(SND_JACK_HEADSET | MT6735_JACK_BTN_MASK)

struct mt6735_mt6331_priv {
	struct snd_soc_card card;
	struct snd_soc_jack jack;
	struct notifier_block accdet_nb;
	struct accdet_state state;
	struct mutex lock;	/* protects state, have_state, jack_ready */
	bool have_state;
	bool jack_ready;
	bool jack_created;
	bool accdet_registered;
	unsigned int button_bit;	/* last pressed key, for its release */
};

/*
 * FrontEnd DAI links, mt_soc_machine.c:858-884 and 1098-1106.
 *
 * The three multimedia links carry the on-SoC codec (MT6331 downlink/uplink)
 * and therefore leave .dai_fmt unset: downstream never programmed an interface
 * format for them, the AFE and the codec DAI selected their own internal
 * formats.  I2S0 is a real four-wire interface; its format follows the AFE
 * I2S configuration that downstream writes into AFE_I2S_CON/I2S_CON3
 * (mt_soc_pcm_dl1_i2s0.c:183-193, mt_soc_digital_type.h:236-262):
 * I2S mode (Soc_Aud_I2S_FORMAT_I2S), LRCK not inverted
 * (Soc_Aud_INV_LRCK_NO_INVERSE), no LR swap (Soc_Aud_LR_SWAP_NO_SWAP) and the
 * AFE as clock master (Soc_Aud_I2S_SRC_MASTER_MODE) => NB_NF | CBC_CFC.
 */
SND_SOC_DAILINK_DEFS(mm1,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-dl1dai-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec", "mt-soc-codec-tx-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(mm2,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-ul1dai-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec", "mt-soc-codec-rx-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(dl2,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-dl2dai-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec", "mt-soc-codec-tx-dai2")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(i2s0,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-i2s0dai-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * HP impedance link, mt_soc_machine.c:1057-1066: CPU DAI
 * MT_SOC_HP_IMPEDANCE_NAME, stream MT_SOC_HP_IMPEDANCE_STREAM_NAME, codec
 * DAI MT_SOC_CODEC_HP_IMPEDANCE_NAME (AudDrv_Def.h:229, 287, 336).
 */
SND_SOC_DAILINK_DEFS(hp_impedance,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-hpimpedancedai-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec",
				      "mt-soc-codec-hp-impedance-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * FM I2S playback, mt_soc_machine.c:1068-1076: CPU DAI MT_SOC_FM_I2S_NAME,
 * stream MT_SOC_FM_I2S_PLAYBACK_STREAM_NAME, codec DAI
 * MT_SOC_CODEC_FM_I2S_DAI_NAME (AudDrv_Def.h:230, 307, 342).  The codec side
 * is the separate FM I2S codec component provided by the WCN FM driver; that
 * component registers no card of its own, so this link is its only binding and
 * the card stays mt-snd-card / mediatek,mt6735-sound.
 *
 * Downstream always lists the link because its FM codec is part of the
 * always-built codec driver; here the FM component is an optional module, so
 * the link is only compiled in when CONFIG_MTK_WCN_FM is enabled - a link whose
 * codec can never appear would make soc_bind_dai_link() defer the whole card
 * forever and take the base audio links down with it.
 */
#if IS_ENABLED(CONFIG_MTK_WCN_FM)
SND_SOC_DAILINK_DEFS(fm_i2s,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-fmi2S-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6627-fm-i2s",
				      "mt-soc-fm-i2s-dai-codec")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
#endif

/*
 * Voice links, mt_soc_machine.c:863-869 ("Voice_MD1"), :985-992
 * ("Voice_MD1_BT"), :1047-1056 ("Voice_MD2") and :1087-1094
 * ("Voice_MD2_BT").
 *
 * Downstream paired the two modem voice links with dedicated PMIC voice
 * codec DAIs (MT_SOC_CODEC_VOICE_MD1DAI_NAME/VOICE_MD2DAI_NAME,
 * AudDrv_Def.h:280-281) that mt_soc_codec_63xx.c implements with the AIF1
 * ops, i.e. they power and configure the PMIC DAC/ADC exactly like the
 * multimedia DAIs.  The mt6331 codec driver exports those two DAIs, so the
 * voice links use them; the AFE voice DAI programs the SoC side of the same
 * interface (AFE_ADDA_xxx / AFE_I2S_CON1, see mt6735-dai-voice.c), matching
 * mtk_voice1_prepare() in mt_soc_pcm_voice_md1.c:284-300.
 *
 * The BT variants used MT_SOC_CODEC_DUMMY_NAME downstream as well
 * (mt_soc_machine.c:990, :1092): their audio path is DAI_BT <-> modem PCM
 * and never reaches the PMIC codec.
 */
SND_SOC_DAILINK_DEFS(voice_md1,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-voice-md1-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec",
				      "mt-soc-codec-voicemd1-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_md2,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-voice-md2-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt-soc-codec",
				      "mt-soc-codec-voicemd2-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_md1_bt,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-voice-md1-bt-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(voice_md2_bt,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-voice-md2-bt-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * BT SCO capture, mt_soc_pcm_bt_dai.c.  The PCM is capture-only there
 * (:304-307 rejects playback) and captures from the DAI_BT block into the
 * MEM_DAI memory interface.  mtk-btcvsd.c exposes only component PCM ops and
 * no DAI (it registers num_dai = 0 at mtk-btcvsd.c:1374-1378), so the codec
 * side is the dummy DAI here too; the in-tree btcvsd component additionally
 * expects the "mediatek,mtk-btcvsd-snd" DT node, while the ROM node is
 * "mediatek,audio_bt_cvsd" (rom_dtb_0.dts:3836-3845).
 */
SND_SOC_DAILINK_DEFS(btcvsd,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-btcvsd-dai")),
	DAILINK_COMP_ARRAY(COMP_CODEC("snd-soc-dummy", "snd-soc-dummy-dai")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_jack_pin mt6735_mt6331_jack_pins[] = {
	{
		.pin = "Headphone",
		.mask = SND_JACK_HEADPHONE,
	},
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
};

/*
 * Card-level jack pins.  These are not routing widgets: they are the DAPM
 * anchors snd_soc_jack_report() toggles when the ACCDET reports a plug event,
 * exactly like the headset pins of the other MediaTek machine drivers.  All
 * audio routing stays in the codec and AFE, so no routes are added here.
 */
static const struct snd_soc_dapm_widget mt6735_mt6331_jack_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static int mt6735_mt6331_mm1_init(struct snd_soc_pcm_runtime *rtd);
static void mt6735_mt6331_mm1_exit(struct snd_soc_pcm_runtime *rtd);

static struct snd_soc_dai_link mt6735_mt6331_dai_links[] = {
	{
		.name = "MultiMedia1",
		.stream_name = MT6735_MM1_STREAM,
		.init = mt6735_mt6331_mm1_init,
		.exit = mt6735_mt6331_mm1_exit,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(mm1),
	},
	{
		.name = "MultiMedia2",
		.stream_name = MT6735_MM2_STREAM,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(mm2),
	},
	{
		.name = "MultiMedia_DL2",
		.stream_name = MT6735_DL2_STREAM,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(dl2),
	},
	{
		.name = "I2S0OUTPUT",
		.stream_name = MT6735_I2S0_STREAM,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		SND_SOC_DAILINK_REG(i2s0),
	},
	{
		.name = "HP_IMPEDANCE",
		.stream_name = "HP_IMPEDANCE_Playback",
		.playback_only = 1,
		SND_SOC_DAILINK_REG(hp_impedance),
	},
#if IS_ENABLED(CONFIG_MTK_WCN_FM)
	{
		/*
		 * mt_soc_machine.c:1068-1076.  Link, stream and DAI names and
		 * the I2S/NB_NF/CBC_CFC format are the frozen FM I2S contract
		 * shared with the WCN FM codec driver.
		 */
		.name = "FM_I2S_RX_Playback",
		.stream_name = "FM_I2S_Playback",
		.playback_only = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		SND_SOC_DAILINK_REG(fm_i2s),
	},
#endif
	{
		.name = "Voice_MD1",
		.stream_name = MT6735_VOICE_MD1_STREAM,
		SND_SOC_DAILINK_REG(voice_md1),
	},
	{
		.name = "Voice_MD2",
		.stream_name = MT6735_VOICE_MD2_STREAM,
		SND_SOC_DAILINK_REG(voice_md2),
	},
	{
		.name = "Voice_MD1_BT",
		.stream_name = MT6735_VOICE_MD1_BT_STREAM,
		SND_SOC_DAILINK_REG(voice_md1_bt),
	},
	{
		.name = "Voice_MD2_BT",
		.stream_name = MT6735_VOICE_MD2_BT_STREAM,
		SND_SOC_DAILINK_REG(voice_md2_bt),
	},
	{
		.name = "BTCVSD",
		.stream_name = MT6735_DAI_STREAM,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(btcvsd),
	},
};

/*
 * ACCDET button index -> ASoC jack button.  The indices are ordered by
 * increasing mic-ADC voltage (accdet.h, enum accdet_button), which matches
 * the downstream four-key detection (accdet.c:712-769).  The keycodes are the
 * ones downstream reports on its own input device.
 */
static unsigned int mt6735_mt6331_button_bit(int button)
{
	switch (button) {
	case ACCDET_BUTTON_MEDIA:
		return SND_JACK_BTN_0;	/* KEY_PLAYPAUSE */
	case ACCDET_BUTTON_VOICE:
		return SND_JACK_BTN_1;	/* KEY_VOICECOMMAND */
	case ACCDET_BUTTON_VOLUP:
		return SND_JACK_BTN_2;	/* KEY_VOLUMEUP */
	case ACCDET_BUTTON_VOLDOWN:
		return SND_JACK_BTN_3;	/* KEY_VOLUMEDOWN */
	case ACCDET_BUTTON_NONE:
	default:
		return 0;
	}
}

/*
 * Cable type -> jack status.  The ROM distinguishes a three-pole headphone
 * (HEADPHONE) from a four-pole headset with microphone (HEADSET_MIC,
 * accdet.c:989-1057); the MT6328 ACCDET driver additionally reports a
 * five-pole headset, which is electrically a four-pole cable with a second
 * ground ring and is reported as a headset.
 */
static unsigned int mt6735_mt6331_plug_status(const struct accdet_state *state)
{
	if (!state->plugged)
		return 0;

	switch (state->cable) {
	case ACCDET_CABLE_HEADPHONE:
		return SND_JACK_HEADPHONE;
	case ACCDET_CABLE_HEADSET:
	case ACCDET_CABLE_HEADSET_5POLE:
		return SND_JACK_HEADSET;
	case ACCDET_CABLE_NONE:
	case ACCDET_CABLE_UNKNOWN:
	default:
		return 0;
	}
}

/*
 * ACCDET emits no notification for the state it finds at probe time, so the
 * machine driver asks for it explicitly.  The call fails with -ENODEV when
 * ACCDET is not built in or has not probed yet; a later event will update the
 * cache in that case.
 */
static void mt6735_mt6331_seed_state(struct mt6735_mt6331_priv *priv)
{
	struct accdet_state state;

	if (priv->have_state)
		return;

	if (accdet_get_state(&state))
		return;

	priv->state = state;
	priv->have_state = true;
}

static int mt6735_mt6331_accdet_notify(struct notifier_block *nb,
				       unsigned long event, void *data)
{
	struct mt6735_mt6331_priv *priv =
		container_of(nb, struct mt6735_mt6331_priv, accdet_nb);
	const struct accdet_state *state = data;
	unsigned int status, mask;

	if (!state)
		return NOTIFY_DONE;

	if (event != ACCDET_EVENT_PLUG && event != ACCDET_EVENT_BUTTON)
		return NOTIFY_DONE;

	/*
	 * Cache the state so that a notification that arrives before the card
	 * is bound is replayed when the jack is created in mm1_init().  The
	 * ACCDET notification is a blocking notifier, so sleeping is fine.
	 */
	mutex_lock(&priv->lock);
	priv->state = *state;
	priv->have_state = true;

	if (event == ACCDET_EVENT_PLUG) {
		status = mt6735_mt6331_plug_status(state);
		mask = MT6735_JACK_MASK;
		if (!state->plugged)
			priv->button_bit = 0;
	} else if (state->button != ACCDET_BUTTON_NONE) {
		priv->button_bit = mt6735_mt6331_button_bit(state->button);
		status = priv->button_bit;
		mask = MT6735_JACK_BTN_MASK;
	} else {
		/*
		 * Release carries ACCDET_BUTTON_NONE; release the key whose
		 * press we remembered.  Fall back to all buttons if a press
		 * was missed.
		 */
		status = 0;
		mask = priv->button_bit ? priv->button_bit :
					 MT6735_JACK_BTN_MASK;
		priv->button_bit = 0;
	}

	if (priv->jack_ready)
		snd_soc_jack_report(&priv->jack, status, mask);
	mutex_unlock(&priv->lock);

	return NOTIFY_OK;
}

static int mt6735_mt6331_mm1_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct mt6735_mt6331_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;
	int ret;

	if (!priv->jack_created) {
		ret = snd_soc_dapm_new_controls(snd_soc_card_to_dapm(card),
						mt6735_mt6331_jack_widgets,
						ARRAY_SIZE(mt6735_mt6331_jack_widgets));
		if (ret) {
			dev_err(rtd->dev, "failed to add jack pins: %d\n", ret);
			return ret;
		}

		ret = snd_soc_card_jack_new_pins(card, "Headset Jack",
						 MT6735_JACK_MASK, &priv->jack,
						 mt6735_mt6331_jack_pins,
						 ARRAY_SIZE(mt6735_mt6331_jack_pins));
		if (ret) {
			dev_err(rtd->dev, "failed to create jack: %d\n", ret);
			return ret;
		}

		snd_jack_set_key(priv->jack.jack, SND_JACK_BTN_0,
				 KEY_PLAYPAUSE);
		snd_jack_set_key(priv->jack.jack, SND_JACK_BTN_1,
				 KEY_VOICECOMMAND);
		snd_jack_set_key(priv->jack.jack, SND_JACK_BTN_2,
				 KEY_VOLUMEUP);
		snd_jack_set_key(priv->jack.jack, SND_JACK_BTN_3,
				 KEY_VOLUMEDOWN);
		priv->jack_created = true;
	}

	ret = snd_soc_component_set_jack(component, &priv->jack, NULL);
	if (ret == -ENOTSUPP || ret == -EOPNOTSUPP) {
		/*
		 * The codec DAI does not implement jack detection; the jack is
		 * driven by ACCDET, so the card is still fully usable.
		 */
		dev_warn(rtd->dev, "codec %s has no set_jack, using ACCDET only\n",
			 component->name);
		ret = 0;
	} else if (ret) {
		dev_err(rtd->dev, "failed to set jack on %s: %d\n",
			component->name, ret);
		return ret;
	}

	mutex_lock(&priv->lock);
	priv->jack_ready = true;
	mt6735_mt6331_seed_state(priv);
	if (priv->have_state) {
		unsigned int status = 0;
		unsigned int mask = MT6735_JACK_MASK;

		priv->button_bit = 0;
		if (priv->state.plugged) {
			status = mt6735_mt6331_plug_status(&priv->state);
			priv->button_bit =
				mt6735_mt6331_button_bit(priv->state.button);
			status |= priv->button_bit;
		}
		snd_soc_jack_report(&priv->jack, status, mask);
	}
	mutex_unlock(&priv->lock);

	return 0;
}

static void mt6735_mt6331_mm1_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct mt6735_mt6331_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component =
		snd_soc_rtd_to_codec(rtd, 0)->component;

	mutex_lock(&priv->lock);
	priv->jack_ready = false;
	mutex_unlock(&priv->lock);

	snd_soc_component_set_jack(component, NULL, NULL);
}

static int mt6735_mt6331_probe(struct platform_device *pdev)
{
	struct mt6735_mt6331_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	priv->accdet_nb.notifier_call = mt6735_mt6331_accdet_notify;

	priv->card.name = MT6735_CARD_NAME;
	priv->card.dev = &pdev->dev;
	priv->card.owner = THIS_MODULE;
	priv->card.dai_link = mt6735_mt6331_dai_links;
	priv->card.num_links = ARRAY_SIZE(mt6735_mt6331_dai_links);
	snd_soc_card_set_drvdata(&priv->card, priv);
	platform_set_drvdata(pdev, priv);

	/*
	 * Subscribe before registering the card so that an ACCDET event during
	 * binding is cached and replayed by mm1_init().
	 */
	ret = accdet_register_notifier(&priv->accdet_nb);
	if (ret) {
		if (ret != -ENODEV)
			return ret;
		dev_warn(&pdev->dev, "ACCDET not available, no jack reporting\n");
	} else {
		priv->accdet_registered = true;
	}

	mutex_lock(&priv->lock);
	mt6735_mt6331_seed_state(priv);
	mutex_unlock(&priv->lock);

	ret = devm_snd_soc_register_card(&pdev->dev, &priv->card);
	if (ret) {
		if (priv->accdet_registered)
			accdet_unregister_notifier(&priv->accdet_nb);
		return ret;
	}

	return 0;
}

static void mt6735_mt6331_remove(struct platform_device *pdev)
{
	struct mt6735_mt6331_priv *priv = platform_get_drvdata(pdev);

	if (priv->accdet_registered) {
		accdet_unregister_notifier(&priv->accdet_nb);
		priv->accdet_registered = false;
	}
}

static const struct of_device_id mt6735_mt6331_of_match[] = {
	{ .compatible = "mediatek,mt6735-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6735_mt6331_of_match);

static struct platform_driver mt6735_mt6331_driver = {
	.driver = {
		.name = "mt6735-mt6331-sound",
		.of_match_table = mt6735_mt6331_of_match,
	},
	.probe = mt6735_mt6331_probe,
	.remove = mt6735_mt6331_remove,
};
module_platform_driver(mt6735_mt6331_driver);

MODULE_AUTHOR("ChiPeng <chipeng.chang@mediatek.com>");
MODULE_DESCRIPTION("ALSA SoC MT6735/MT6331 machine driver");
MODULE_LICENSE("GPL");
