// SPDX-License-Identifier: GPL-2.0
/*
 * MT6328 PMIC accessory detection (ACCDET) driver.
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/accdet/mt6735/accdet.c
 * (Samsung SM-G532M "grandpplte" GPL kernel, MT6737T + MT6328 PMIC).
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * The ROM build enables CONFIG_ACCDET_EINT
 * (arch/arm/configs/grandpplte_00_defconfig:1402) and detects plug/unplug
 * through an AP GPIO EINT while the PMIC ACCDET interrupt reports the
 * hook-switch/button state.  This driver uses the PMIC ACCDET interrupt for
 * both: the MT6735 EINT in this kernel has no programmable debounce
 * (mt6735_eint_hw has no .db_time, so gpiod_set_debounce() returns
 * -EOPNOTSUPP) whereas the ACCDET block has its own debounce registers that
 * are programmed from DT.  The unit therefore stays enabled while idle, which
 * is the downstream non-EINT configuration (accdet.c:1451-1453).
 *
 * Not ported (deliberate): the /dev/accdet misc device and ioctls, the
 * Samsung SEC_JACK_SYSFS jack factory, the TS3A225E accessory switch, pin
 * swap / pin recognition and the call-state handling; all of those are HAL or
 * factory interfaces.  The input device that downstream uses to inject key
 * codes is replaced by the accdet notifier.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mfd/mt6328/accdet.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "accdet-regs.h"

/* Downstream enum accdet_status, accdet.h:96-103.  STAND_BY is only reached
 * from the HAL ioctl and is not implemented.
 */
enum accdet_hw_state {
	ACCDET_HW_PLUG_OUT = 0,
	ACCDET_HW_MIC_BIAS = 1,
	ACCDET_HW_HOOK_SWITCH = 2,
	ACCDET_HW_EXT_CABLE = 3,
};

/* Downstream struct headset_mode_settings, accdet.h:79-87. */
struct accdet_settings {
	u32 pwm_width;
	u32 pwm_thresh;
	u32 fall_delay;
	u32 rise_delay;
	u32 debounce0;
	u32 debounce1;
	u32 debounce3;
};

struct accdet_priv {
	struct device *dev;
	struct regmap *regmap;
	int irq;

	struct accdet_settings set;

	/* headset-vol-threshold, accdet.c:1318-1337 */
	u32 vol_min_3pole;
	u32 vol_max_3pole;
	u32 vol_min_4pole;
	u32 vol_max_4pole;
	u32 vol_bias;

	/* headset-{three,four}-key-threshold, accdet.c:1310-1324 */
	bool four_key;
	u32 key_mid;
	u32 key_voice;
	u32 key_up;
	u32 key_down;

	u32 mic_mode_vol;		/* accdet-mic-vol */
	u32 mic_mode;			/* accdet-mic-mode */
	/*
	 * accdet-plugout-debounce: debounce of the plug-out edge for the AP
	 * GPIO EINT path.  Parsed for DT parity; unused because this driver
	 * takes plug-out from the PMIC ACCDET comparator.
	 */
	u32 plugout_debounce;

	s8 auxadc_offset;

	enum accdet_cable_type cable;
	bool plugged;
	int button;
	int mic_mv;
	unsigned int hw_state;

	bool pending_plug;
	bool pending_button;

	struct work_struct work;
	struct workqueue_struct *wq;
	struct mutex lock;
	bool suspended;
};

/* Defaults are the ROM DTB values (rom_dtb_0.dts:4024). */
static const u32 accdet_default_mode_setting[] = {
	0x500, 0x500, 0x1, 0x1f0, 0x800, 0x800, 0x20,
};

static const u32 accdet_default_vol_threshold[] = {
	0x0, 0x252, 0x253, 0x270f, 0xaf0,
};

static const u32 accdet_default_three_key[] = { 0x0, 0x7a, 0xd4, 0x1e8 };
static const u32 accdet_default_four_key[] = { 0x0, 0x55, 0x7a, 0xd4, 0x1e8 };

static struct accdet_priv *accdet_instance;
static BLOCKING_NOTIFIER_HEAD(accdet_notifier);

static void accdet_fill_state(struct accdet_priv *priv,
			      struct accdet_state *state)
{
	state->cable = priv->cable;
	state->plugged = priv->plugged;
	state->button = priv->button;
	state->mic_adc = priv->mic_mv;
}

/*
 * One-shot AUXADC conversion of the mic line, downstream
 * Accdet_PMIC_IMM_GetOneChannelValue(), accdet.c:252-300.
 */
static int accdet_measure_mic(struct accdet_priv *priv, unsigned int *mv)
{
	u32 val;
	int ret;

	ret = regmap_write(priv->regmap, ACCDET_AUXADC_CTL_SET,
			   ACCDET_CH_REQ_EN);
	if (ret)
		return ret;

	usleep_range(3000, 3500);	/* downstream mdelay(3), accdet.c:287 */

	ret = regmap_read_poll_timeout(priv->regmap, ACCDET_AUXADC_REG, val,
				       (val & ACCDET_DATA_READY) ==
							ACCDET_DATA_READY,
				       ACCDET_ADC_POLL_US,
				       ACCDET_ADC_TIMEOUT_US);
	if (ret)
		return ret;

	val &= ACCDET_DATA_MASK;
	/* accdet.c:296-298: 12-bit code, 1.8 V reference, efuse offset */
	val = val * ACCDET_ADC_FULL_SCALE_MV / ACCDET_ADC_FULL_SCALE;
	if (val > priv->auxadc_offset)
		val -= priv->auxadc_offset;
	else
		val = 0;

	*mv = val;
	return 0;
}

/* Downstream key_check(), accdet.c:728-744. */
static int accdet_key_check(struct accdet_priv *priv, unsigned int mv)
{
	if (mv < priv->key_down && mv >= priv->key_up)
		return ACCDET_BUTTON_VOLDOWN;

	if (priv->four_key) {
		if (mv < priv->key_up && mv >= priv->key_voice)
			return ACCDET_BUTTON_VOLUP;
		if (mv < priv->key_voice && mv >= priv->key_mid)
			return ACCDET_BUTTON_VOICE;
	} else if (mv < priv->key_up && mv >= priv->key_mid) {
		return ACCDET_BUTTON_VOLUP;
	}

	if (mv < priv->key_mid)
		return ACCDET_BUTTON_MEDIA;

	return ACCDET_BUTTON_NONE;
}

/*
 * Downstream multi_key_detection(), accdet.c:772-806.  A press is seen as the
 * comparator dropping back to the 3-pole code, a release as it returning to
 * the 4-pole code.
 */
static void accdet_multi_key_detection(struct accdet_priv *priv,
				       unsigned int current_status)
{
	unsigned int mv;
	u32 sts;
	int ret;

	if (current_status == ACCDET_AB_3POLE) {
		ret = accdet_measure_mic(priv, &mv);
		if (ret) {
			dev_warn_ratelimited(priv->dev,
					     "key ADC read failed: %d\n", ret);
			return;
		}
		priv->mic_mv = mv;
		priv->button = accdet_key_check(priv, mv);
	} else {
		priv->button = ACCDET_BUTTON_NONE;
	}

	msleep(30);	/* downstream mdelay(30), accdet.c:789 */

	ret = regmap_read(priv->regmap, ACCDET_IRQ_STS, &sts);
	if (ret || !(sts & ACCDET_IRQ_STATUS_BIT)) {
		priv->pending_button = true;
	} else {
		/* Plug-out side effect, downstream accdet.c:799-801 */
		dev_dbg(priv->dev, "ignoring key %d, cable event pending\n",
			priv->button);
		priv->button = ACCDET_BUTTON_NONE;
	}

	if (current_status != ACCDET_AB_3POLE)
		priv->button = ACCDET_BUTTON_NONE;
}

/* Downstream headset_plug_out(), accdet.c:285-304. */
static void accdet_plug_out(struct accdet_priv *priv)
{
	priv->hw_state = ACCDET_HW_PLUG_OUT;
	priv->cable = ACCDET_CABLE_NONE;
	priv->plugged = false;
	priv->mic_mv = -1;

	if (priv->button != ACCDET_BUTTON_NONE) {
		priv->button = ACCDET_BUTTON_NONE;
		priv->pending_button = true;
	}
}

/*
 * Downstream check_cable_type(), accdet.c:894-1267, ported for the
 * NO_USE_COMPARATOR configuration with the state changes guarded by
 * eint_accdet_sync_flag (always 1 here, accdet.c:1885).  Plug-out is taken
 * from the ACCDET comparator instead of the AP EINT.
 */
static void accdet_check_cable_type(struct accdet_priv *priv)
{
	enum accdet_cable_type old_cable = priv->cable;
	bool old_plugged = priv->plugged;
	unsigned int status, mv;
	u32 ab;
	int ret;

	ret = regmap_read(priv->regmap, ACCDET_STATE_RG, &ab);
	if (ret)
		return;

	ab = (ab & ACCDET_AB_MASK) >> ACCDET_AB_SHIFT;

	if (ab == ACCDET_AB_PLUG_OUT) {
		accdet_plug_out(priv);
		goto out;
	}

	ret = accdet_measure_mic(priv, &mv);
	if (ret) {
		dev_warn_ratelimited(priv->dev, "mic ADC read failed: %d\n",
				     ret);
		return;
	}
	priv->mic_mv = mv;

	/* Downstream check_pole_type(), accdet.c:356-381. */
	if (mv < priv->vol_max_4pole + 1 &&
	    (int)mv > (int)priv->vol_min_4pole - 1)
		status = ACCDET_AB_4POLE;
	else if (mv < priv->vol_max_3pole + 1 && mv > priv->vol_min_3pole)
		status = ACCDET_AB_3POLE;
	else
		status = ACCDET_AB_EXTERNAL;

	switch (priv->hw_state) {
	case ACCDET_HW_PLUG_OUT:
		/* accdet.c:918-1015 */
		switch (status) {
		case ACCDET_AB_3POLE:
			priv->cable = ACCDET_CABLE_HEADPHONE;
			priv->hw_state = ACCDET_HW_HOOK_SWITCH;
			break;
		case ACCDET_AB_4POLE:
			priv->cable = ACCDET_CABLE_HEADSET;
			priv->hw_state = ACCDET_HW_MIC_BIAS;
			/* AB=11 debounce 30 ms, accdet.c:1004-1006 */
			regmap_write(priv->regmap, ACCDET_DEBOUNCE3,
				     priv->set.debounce3 * 30);
			regmap_write(priv->regmap, ACCDET_DEBOUNCE0,
				     ACCDET_BUTTON_PRESS_DEBOUNCE);
			break;
		default:
			/* External cable, downstream EXT_CABLE/NO_DEVICE */
			priv->cable = ACCDET_CABLE_NONE;
			priv->hw_state = ACCDET_HW_EXT_CABLE;
			break;
		}
		break;

	case ACCDET_HW_MIC_BIAS:
		/* accdet.c:1018-1097: restore the hook switch debounce */
		regmap_write(priv->regmap, ACCDET_DEBOUNCE0,
			     priv->set.debounce0);
		switch (status) {
		case ACCDET_AB_3POLE:
			priv->hw_state = ACCDET_HW_HOOK_SWITCH;
			/* reduce the release key debounce, accdet.c:1071 */
			regmap_write(priv->regmap, ACCDET_DEBOUNCE1,
				     ACCDET_KEY_RELEASE_DEBOUNCE);
			accdet_multi_key_detection(priv, status);
			/* recover PWM frequency and duty, accdet.c:1077-1078 */
			regmap_write(priv->regmap, ACCDET_PWM_WIDTH,
				     priv->set.pwm_width - 1);
			regmap_write(priv->regmap, ACCDET_PWM_THRESH,
				     priv->set.pwm_thresh - 1);
			break;
		case ACCDET_AB_4POLE:
			/* no change, accdet.c:1088-1096 */
			break;
		default:
			priv->cable = ACCDET_CABLE_NONE;
			priv->hw_state = ACCDET_HW_EXT_CABLE;
			break;
		}
		break;

	case ACCDET_HW_HOOK_SWITCH:
		/* accdet.c:1100-1174: restore the mic bias debounce */
		regmap_write(priv->regmap, ACCDET_DEBOUNCE1,
			     priv->set.debounce1);
		switch (status) {
		case ACCDET_AB_3POLE:
			/* keep the state, accdet.c:1134-1143 */
			break;
		case ACCDET_AB_4POLE:
			accdet_multi_key_detection(priv, status);
			priv->hw_state = ACCDET_HW_MIC_BIAS;
			priv->cable = ACCDET_CABLE_HEADSET;
			regmap_write(priv->regmap, ACCDET_DEBOUNCE0,
				     ACCDET_BUTTON_PRESS_DEBOUNCE);
			break;
		default:
			priv->cable = ACCDET_CABLE_NONE;
			priv->hw_state = ACCDET_HW_EXT_CABLE;
			break;
		}
		break;

	case ACCDET_HW_EXT_CABLE:
		/* accdet.c:1177-1210 */
		switch (status) {
		case ACCDET_AB_3POLE:
			priv->cable = ACCDET_CABLE_HEADPHONE;
			priv->hw_state = ACCDET_HW_HOOK_SWITCH;
			break;
		case ACCDET_AB_4POLE:
			accdet_multi_key_detection(priv, status);
			priv->hw_state = ACCDET_HW_MIC_BIAS;
			priv->cable = ACCDET_CABLE_HEADSET;
			break;
		default:
			break;
		}
		break;
	}

	priv->plugged = priv->cable == ACCDET_CABLE_HEADPHONE ||
			priv->cable == ACCDET_CABLE_HEADSET;

out:
	if (old_cable != priv->cable || old_plugged != priv->plugged)
		priv->pending_plug = true;
}

static void accdet_work_fn(struct work_struct *work)
{
	struct accdet_priv *priv = container_of(work, struct accdet_priv, work);
	struct accdet_state state;
	bool plug = false, button = false;

	mutex_lock(&priv->lock);
	if (!priv->suspended) {
		priv->pending_plug = false;
		priv->pending_button = false;
		accdet_check_cable_type(priv);
		plug = priv->pending_plug;
		button = priv->pending_button;
	}
	accdet_fill_state(priv, &state);
	mutex_unlock(&priv->lock);

	/* Downstream reports the key first, then the plug state. */
	if (button)
		blocking_notifier_call_chain(&accdet_notifier,
					     ACCDET_EVENT_BUTTON, &state);
	if (plug)
		blocking_notifier_call_chain(&accdet_notifier,
					     ACCDET_EVENT_PLUG, &state);
}

/* Downstream clear_accdet_interrupt(), accdet.c:881-887. */
static void accdet_clear_irq(struct accdet_priv *priv)
{
	u32 val;

	if (regmap_read(priv->regmap, ACCDET_IRQ_STS, &val))
		return;

	regmap_write(priv->regmap, ACCDET_IRQ_STS,
		     (val & ACCDET_IRQ_STS_RSVD) | ACCDET_IRQ_CLR_BIT);
}

/*
 * Downstream accdet_irq_handler() for the non-EINT configuration,
 * accdet.c:854-878.
 */
static irqreturn_t accdet_irq_thread(int irq, void *data)
{
	struct accdet_priv *priv = data;
	u32 val;

	if (!regmap_read(priv->regmap, ACCDET_IRQ_STS, &val) &&
	    (val & ACCDET_IRQ_STATUS_BIT))
		accdet_clear_irq(priv);

	if (priv->hw_state == ACCDET_HW_MIC_BIAS) {
		/* accdet.c:859-862 */
		regmap_write(priv->regmap, ACCDET_PWM_WIDTH,
			     priv->set.pwm_width - 1);
		regmap_write(priv->regmap, ACCDET_PWM_THRESH,
			     priv->set.pwm_width - 1);
	}

	queue_work(priv->wq, &priv->work);

	/* Wait for the unit to drop the status bit, 400 us, accdet.c:874-877 */
	regmap_read_poll_timeout(priv->regmap, ACCDET_IRQ_STS, val,
				 !(val & ACCDET_IRQ_STATUS_BIT), 10,
				 ACCDET_IRQ_TIMEOUT_NS / 1000);

	return IRQ_HANDLED;
}

/*
 * Downstream disable_accdet(), accdet.c:319-355.  The build with
 * CONFIG_ACCDET_EINT also clears ACCDET_STATE_SWCTRL and gates the clock.
 */
static void accdet_disable(struct accdet_priv *priv)
{
	u32 val;

	regmap_write(priv->regmap, ACCDET_INT_CON_CLR, ACCDET_RG_IRQ_CLR);
	accdet_clear_irq(priv);
	udelay(200);

	regmap_read_poll_timeout(priv->regmap, ACCDET_IRQ_STS, val,
				 !(val & ACCDET_IRQ_STATUS_BIT), 20000,
				 ACCDET_ADC_TIMEOUT_US);

	regmap_read(priv->regmap, ACCDET_IRQ_STS, &val);
	regmap_write(priv->regmap, ACCDET_IRQ_STS, val & ~ACCDET_IRQ_CLR_BIT);

	regmap_write(priv->regmap, ACCDET_STATE_SWCTRL, 0);
	regmap_write(priv->regmap, ACCDET_CTRL, ACCDET_DISABLE);
	regmap_write(priv->regmap, ACCDET_TOP_CKPDN_SET, ACCDET_RG_CLK_SET);
}

/*
 * Downstream accdet_init(), accdet.c:1368-1462, with the non-EINT tail
 * (accdet.c:1451-1453) so the unit keeps running to detect the next plug.
 * Re-running this is also how the driver restores its registers after
 * suspend/hibernation.
 */
static int accdet_hw_init(struct accdet_priv *priv)
{
	struct regmap *map = priv->regmap;
	u32 val;
	int ret;

	/* Clock gate */
	ret = regmap_write(map, ACCDET_TOP_CKPDN_CLR, ACCDET_RG_CLK_CLR);
	if (ret)
		return ret;

	/* Reset the ACCDET unit */
	ret = regmap_write(map, ACCDET_TOP_RST_ACCDET_SET, ACCDET_RESET_SET);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_TOP_RST_ACCDET_CLR, ACCDET_RESET_CLR);
	if (ret)
		return ret;

	/* PWM frequency and duty */
	ret = regmap_write(map, ACCDET_PWM_WIDTH, priv->set.pwm_width - 1);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_PWM_THRESH, priv->set.pwm_thresh - 1);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_STATE_SWCTRL, ACCDET_SWCTRL_EN);
	if (ret)
		return ret;

	/* Rise and fall delay of the PWM */
	ret = regmap_write(map, ACCDET_EN_DELAY_NUM,
			   (priv->set.fall_delay << 15) | priv->set.rise_delay);
	if (ret)
		return ret;

	/* Debounce times */
	ret = regmap_write(map, ACCDET_DEBOUNCE0, priv->set.debounce0);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_DEBOUNCE1, priv->set.debounce1);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_DEBOUNCE3, priv->set.debounce3);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_DEBOUNCE4, ACCDET_DE4);
	if (ret)
		return ret;

	/* Enable the ACCDET interrupt inside the PMIC block */
	ret = regmap_read(map, ACCDET_IRQ_STS, &val);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_IRQ_STS, val & ~ACCDET_IRQ_CLR_BIT);
	if (ret)
		return ret;
	ret = regmap_write(map, ACCDET_INT_CON_SET, ACCDET_RG_IRQ_SET);
	if (ret)
		return ret;

	/* Mic bias reference from accdet-mic-vol */
	ret = regmap_update_bits(map, ACCDET_ADC_REG,
				 ACCDET_AUDMICBIASVREF_MASK,
				 priv->mic_mode_vol << ACCDET_AUDMICBIASVREF_SHIFT);
	if (ret)
		return ret;

	ret = regmap_write(map, ACCDET_RSV, 0x1290);
	if (ret)
		return ret;

	/* accdet-mic-mode, accdet.c:1425-1435 */
	switch (priv->mic_mode) {
	case 1:		/* ACC mode */
		ret = regmap_update_bits(map, ACCDET_ADC_REG,
					 ACCDET_AUDMICBIAS1DCSWPEN, 0);
		break;
	case 2:		/* low cost mode without internal bias */
		ret = regmap_update_bits(map, ACCDET_RSV, ACCDET_INPUT_MICP,
					 ACCDET_INPUT_MICP);
		break;
	case 6:		/* low cost mode with internal bias */
		ret = regmap_update_bits(map, ACCDET_RSV, ACCDET_INPUT_MICP,
					 ACCDET_INPUT_MICP);
		if (!ret)
			ret = regmap_update_bits(map, ACCDET_ADC_REG,
						 ACCDET_AUDMICBIAS1DCSWPEN,
						 ACCDET_AUDMICBIAS1DCSWPEN);
		break;
	default:
		dev_warn(priv->dev, "unsupported accdet-mic-mode %u\n",
			 priv->mic_mode);
		break;
	}
	if (ret)
		return ret;

	/* External bias of 2.8 V, accdet.c:1456-1458 */
	if (priv->vol_bias == 2800) {
		ret = regmap_update_bits(map, ACCDET_ADC_REG,
					 ACCDET_ADC_2V8_TEST,
					 ACCDET_ADC_2V8_TEST);
		if (ret)
			return ret;
	}

	/* Enable AUXADC auto sampling, accdet.c:1460 */
	ret = regmap_update_bits(map, ACCDET_AUXADC_AUTO_SPL,
				 ACCDET_AUXADC_AUTO_SET,
				 ACCDET_AUXADC_AUTO_SET);
	if (ret)
		return ret;

	/* Downstream non-EINT tail: keep the unit enabled. */
	return regmap_write(map, ACCDET_CTRL, ACCDET_ENABLE);
}

/*
 * Headset ADC offset from the PMIC OTP, downstream
 * accdet_pmic_Read_Efuse_HPOffset(), accdet.c:1358-1366, and
 * pmic_Read_Efuse_HPOffset(), power/mt6735/pmic.c:5155-5195.
 */
static int accdet_read_efuse_offset(struct accdet_priv *priv)
{
	struct regmap *map = priv->regmap;
	unsigned int val;
	s16 efuse;
	int ret;

	/* 1. Enable the efuse control engine clock */
	ret = regmap_write(map, ACCDET_CKHWEN_CON0_CLR, 0x0040);
	if (!ret)
		ret = regmap_write(map, ACCDET_CKPDN_CON3_CLR, 0x0004);
	if (ret)
		goto disable;

	/* 2. Enable the OTP engine */
	ret = regmap_update_bits(map, ACCDET_OTP_CON11, ACCDET_OTP_ENGINE_EN,
				 ACCDET_OTP_ENGINE_EN);
	if (ret)
		goto disable;

	/* 3. Row to read */
	ret = regmap_update_bits(map, ACCDET_OTP_CON0, ACCDET_OTP_ROW_MASK,
				 ACCDET_OTP_ROW << ACCDET_OTP_ROW_SHIFT);
	if (ret)
		goto disable;

	/* 4. Toggle the read trigger */
	udelay(100);
	ret = regmap_read(map, ACCDET_OTP_CON8, &val);
	if (ret)
		goto disable;
	ret = regmap_update_bits(map, ACCDET_OTP_CON8, ACCDET_OTP_READ_TRIGGER,
				 (val & ACCDET_OTP_READ_TRIGGER) ? 0 :
						ACCDET_OTP_READ_TRIGGER);
	if (ret)
		goto disable;

	/* 5. Wait for the engine */
	udelay(300);
	ret = regmap_read_poll_timeout(map, ACCDET_OTP_CON13, val,
				       !(val & ACCDET_OTP_READY),
				       ACCDET_OTP_READY_POLL_US,
				       ACCDET_OTP_READY_TIMEOUT_US);
	if (ret)
		goto disable;

	/* 0x0C1A and 0x0C18 need at least 1 ms between them */
	udelay(1000);

	/* 6. Read the data */
	ret = regmap_read(map, ACCDET_OTP_CON12, &val);
	if (ret)
		goto disable;

	efuse = (s16)val;
	priv->auxadc_offset = ((efuse >> 8) & 0xff) / 2;
	dev_dbg(priv->dev, "efuse 0x%x, auxadc offset %d mV\n",
		val, priv->auxadc_offset);

	/* 7. Disable the efuse control engine clock */
disable:
	regmap_write(map, ACCDET_CKPDN_CON3_SET, 0x0004);
	regmap_write(map, ACCDET_CKHWEN_CON0_SET, 0x0040);

	return ret;
}

/*
 * Map the A/B comparator to a cable state, as downstream
 * mt_accdet_pm_restore_noirq() does, accdet.c:1989-2013.
 */
static void accdet_sync_state_from_ab(struct accdet_priv *priv)
{
	unsigned int ab;
	u32 val;

	if (regmap_read(priv->regmap, ACCDET_STATE_RG, &val))
		return;

	ab = (val & ACCDET_AB_MASK) >> ACCDET_AB_SHIFT;

	switch (ab) {
	case ACCDET_AB_3POLE:
		priv->cable = ACCDET_CABLE_HEADPHONE;
		priv->hw_state = ACCDET_HW_HOOK_SWITCH;
		break;
	case ACCDET_AB_4POLE:
		priv->cable = ACCDET_CABLE_HEADSET;
		priv->hw_state = ACCDET_HW_MIC_BIAS;
		break;
	case ACCDET_AB_EXTERNAL:
		priv->cable = ACCDET_CABLE_NONE;
		priv->hw_state = ACCDET_HW_EXT_CABLE;
		break;
	default:
		priv->cable = ACCDET_CABLE_NONE;
		priv->hw_state = ACCDET_HW_PLUG_OUT;
		break;
	}

	priv->plugged = priv->cable == ACCDET_CABLE_HEADPHONE ||
			priv->cable == ACCDET_CABLE_HEADSET;
}

static void accdet_parse_dt(struct accdet_priv *priv)
{
	struct device_node *np = priv->dev->of_node;
	u32 vals[7];
	u32 four[5];
	u32 three[4];

	memcpy(vals, accdet_default_mode_setting, sizeof(vals));
	of_property_read_u32_array(np, "headset-mode-setting", vals,
				   ARRAY_SIZE(vals));
	priv->set.pwm_width = vals[0];
	priv->set.pwm_thresh = vals[1];
	priv->set.fall_delay = vals[2];
	priv->set.rise_delay = vals[3];
	priv->set.debounce0 = vals[4];
	priv->set.debounce1 = vals[5];
	priv->set.debounce3 = vals[6];

	priv->mic_mode_vol = 7;
	of_property_read_u32(np, "accdet-mic-vol", &priv->mic_mode_vol);

	priv->mic_mode = 1;
	of_property_read_u32(np, "accdet-mic-mode", &priv->mic_mode);

	priv->plugout_debounce = 20;
	of_property_read_u32(np, "accdet-plugout-debounce",
			     &priv->plugout_debounce);

	memcpy(vals, accdet_default_vol_threshold,
	       sizeof(accdet_default_vol_threshold));
	of_property_read_u32_array(np, "headset-vol-threshold", vals,
				   ARRAY_SIZE(accdet_default_vol_threshold));
	priv->vol_min_3pole = vals[0];
	priv->vol_max_3pole = vals[1];
	priv->vol_min_4pole = vals[2];
	priv->vol_max_4pole = vals[3];
	priv->vol_bias = vals[4];

	/* accdet.c:1330-1335 */
	if (priv->vol_bias > 2500)
		priv->vol_bias = 2800;

	/*
	 * Both tables are filled with their ROM defaults; a board that only
	 * describes the three-key table gets the three-key mapping, everything
	 * else (including an empty DT) uses the four-key mapping of the ROM
	 * build (defconfig:1406).
	 */
	memcpy(four, accdet_default_four_key, sizeof(four));
	memcpy(three, accdet_default_three_key, sizeof(three));
	of_property_read_u32_array(np, "headset-four-key-threshold", four,
				   ARRAY_SIZE(four));
	of_property_read_u32_array(np, "headset-three-key-threshold", three,
				   ARRAY_SIZE(three));

	priv->four_key = !of_property_present(np, "headset-three-key-threshold") ||
			 of_property_present(np, "headset-four-key-threshold");

	if (priv->four_key) {
		priv->key_mid = four[1];
		priv->key_voice = four[2];
		priv->key_up = four[3];
		priv->key_down = four[4];
	} else {
		priv->key_mid = three[1];
		priv->key_up = three[2];
		priv->key_down = three[3];
	}

	dev_dbg(priv->dev,
		"pwm 0x%x/0x%x delay %u/0x%x deb 0x%x/0x%x/0x%x, mic vol %u, mic mode %u, %s-key\n",
		priv->set.pwm_width, priv->set.pwm_thresh,
		priv->set.fall_delay, priv->set.rise_delay,
		priv->set.debounce0, priv->set.debounce1, priv->set.debounce3,
		priv->mic_mode_vol, priv->mic_mode,
		priv->four_key ? "four" : "three");
}

/*
 * Downstream accdet_setup_eint() selects "state_eint_as_int" to mux the
 * headset-detect pins as plain GPIO (accdet.c:601-611).  This driver does not
 * use the AP EINT, but the state is part of the board description and keeps
 * the pins in the ROM configuration.
 */
static void accdet_setup_pinctrl(struct accdet_priv *priv)
{
	struct pinctrl_state *state;
	struct pinctrl *pctl;

	pctl = devm_pinctrl_get(priv->dev);
	if (IS_ERR(pctl))
		return;

	state = pinctrl_lookup_state(pctl, "state_eint_as_int");
	if (IS_ERR(state))
		return;

	if (pinctrl_select_state(pctl, state))
		dev_warn(priv->dev, "cannot select state_eint_as_int\n");
}

static int accdet_probe(struct platform_device *pdev)
{
	struct accdet_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!priv->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no parent PMIC regmap\n");

	if (!pdev->dev.of_node) {
		dev_dbg(&pdev->dev, "no DT node, nothing to do\n");
		return -ENODEV;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	accdet_parse_dt(priv);

	if (!priv->set.pwm_width || !priv->set.pwm_thresh)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid headset-mode-setting\n");
	if (priv->mic_mode_vol > (ACCDET_AUDMICBIASVREF_MASK >>
				  ACCDET_AUDMICBIASVREF_SHIFT))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid accdet-mic-vol %u\n",
				     priv->mic_mode_vol);

	accdet_setup_pinctrl(priv);

	priv->cable = ACCDET_CABLE_NONE;
	priv->plugged = false;
	priv->button = ACCDET_BUTTON_NONE;
	priv->mic_mv = -1;
	priv->hw_state = ACCDET_HW_PLUG_OUT;
	mutex_init(&priv->lock);

	priv->wq = alloc_ordered_workqueue("mt6328-accdet", 0);
	if (!priv->wq)
		return -ENOMEM;
	INIT_WORK(&priv->work, accdet_work_fn);

	platform_set_drvdata(pdev, priv);

	ret = accdet_hw_init(priv);
	if (ret) {
		dev_err(&pdev->dev, "hardware init failed: %d\n", ret);
		goto err_wq;
	}

	ret = accdet_read_efuse_offset(priv);
	if (ret)
		dev_warn(&pdev->dev, "efuse read failed, offset 0: %d\n", ret);

	/* Boot-time jack state from the comparator.  No notification: the
	 * consumer queries it through accdet_get_state().
	 */
	accdet_sync_state_from_ab(priv);

	ret = devm_request_threaded_irq(&pdev->dev, priv->irq, NULL,
					accdet_irq_thread,
					IRQF_ONESHOT, "mt6328-accdet", priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq %d: %d\n",
			priv->irq, ret);
		goto err_disable;
	}

	accdet_instance = priv;

	dev_info(&pdev->dev, "MT6328 ACCDET ready, irq %d, initial cable %d\n",
		 priv->irq, priv->cable);
	return 0;

err_disable:
	accdet_disable(priv);
err_wq:
	destroy_workqueue(priv->wq);
	return ret;
}

static void accdet_remove(struct platform_device *pdev)
{
	struct accdet_priv *priv = platform_get_drvdata(pdev);

	disable_irq(priv->irq);
	cancel_work_sync(&priv->work);

	mutex_lock(&priv->lock);
	accdet_disable(priv);
	mutex_unlock(&priv->lock);

	destroy_workqueue(priv->wq);
	accdet_instance = NULL;
}

static int accdet_suspend(struct device *dev)
{
	struct accdet_priv *priv = dev_get_drvdata(dev);

	disable_irq(priv->irq);
	cancel_work_sync(&priv->work);

	mutex_lock(&priv->lock);
	priv->suspended = true;
	/* Keep the unit running while a headset is attached. */
	if (!priv->plugged)
		accdet_disable(priv);
	mutex_unlock(&priv->lock);

	return 0;
}

static int accdet_resume(struct device *dev)
{
	struct accdet_priv *priv = dev_get_drvdata(dev);
	struct accdet_state state;
	enum accdet_cable_type old_cable;
	bool old_plugged;
	int ret = 0;

	mutex_lock(&priv->lock);
	old_cable = priv->cable;
	old_plugged = priv->plugged;
	if (!priv->plugged) {
		ret = accdet_hw_init(priv);
		if (!ret)
			accdet_sync_state_from_ab(priv);
	}
	priv->suspended = false;
	accdet_fill_state(priv, &state);
	mutex_unlock(&priv->lock);

	enable_irq(priv->irq);

	if (ret) {
		dev_err(priv->dev, "resume init failed: %d\n", ret);
		return ret;
	}

	if (old_cable != priv->cable || old_plugged != priv->plugged)
		blocking_notifier_call_chain(&accdet_notifier,
					     ACCDET_EVENT_PLUG, &state);

	return 0;
}

/*
 * Downstream mt_accdet_pm_restore_noirq(), accdet.c:1980-2060, for the
 * hibernation path.  Unlike downstream, the full init sequence is replayed
 * because the PMIC register contents are not preserved across hibernation.
 */
static int accdet_restore_noirq(struct device *dev)
{
	struct accdet_priv *priv = dev_get_drvdata(dev);
	struct accdet_state state;
	enum accdet_cable_type old_cable;
	bool old_plugged;
	int ret;

	mutex_lock(&priv->lock);
	old_cable = priv->cable;
	old_plugged = priv->plugged;
	ret = accdet_hw_init(priv);
	if (!ret)
		accdet_sync_state_from_ab(priv);
	priv->suspended = false;
	accdet_fill_state(priv, &state);
	mutex_unlock(&priv->lock);

	if (ret)
		return ret;

	if (old_cable != priv->cable || old_plugged != priv->plugged)
		blocking_notifier_call_chain(&accdet_notifier,
					     ACCDET_EVENT_PLUG, &state);

	return 0;
}

int accdet_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&accdet_notifier, nb);
}
EXPORT_SYMBOL_GPL(accdet_register_notifier);

void accdet_unregister_notifier(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&accdet_notifier, nb);
}
EXPORT_SYMBOL_GPL(accdet_unregister_notifier);

int accdet_get_state(struct accdet_state *state)
{
	struct accdet_priv *priv = READ_ONCE(accdet_instance);

	if (!priv)
		return -ENODEV;

	mutex_lock(&priv->lock);
	accdet_fill_state(priv, state);
	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(accdet_get_state);

static const struct of_device_id accdet_of_match[] = {
	{ .compatible = "mediatek,mt6328-accdet" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, accdet_of_match);

static const struct dev_pm_ops accdet_pm_ops = {
	.suspend = accdet_suspend,
	.resume = accdet_resume,
	.restore_noirq = accdet_restore_noirq,
};

static struct platform_driver accdet_driver = {
	.probe = accdet_probe,
	.remove = accdet_remove,
	.driver = {
		.name = "mt6328-accdet",
		.of_match_table = accdet_of_match,
		.pm = &accdet_pm_ops,
	},
};
module_platform_driver(accdet_driver);

MODULE_DESCRIPTION("MediaTek MT6328 PMIC accessory detection driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:mt6328-accdet");
