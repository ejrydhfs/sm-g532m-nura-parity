// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S2MU005 switching charger and MUIC driver
 *
 * The S2MU005 integrates a switch-mode charger, a micro-USB MUIC and LED
 * drivers behind a single I2C address (0x3d on this board).  The fuel gauge
 * lives at a separate I2C address and is handled by s2mu005-battery.c.
 *
 * Ported from the Samsung/MediaTek downstream kernel shipped with the
 * SM-G532M (grandpplte):
 *
 *   drivers/battery_v2/s2mu005_charger.c
 *   drivers/muic/s2mu005-muic.c
 *   drivers/mfd/s2mu005_core.c
 *   drivers/mfd/s2mu005_irq.c
 *   include/linux/battery/charger/s2mu005_charger.h
 *   include/linux/mfd/samsung/s2mu005-private.h
 *
 * The downstream driver was split across an MFD, a charger and a MUIC
 * platform device.  Mainline has neither the s2mu005 MFD nor the Samsung
 * MUIC core, so both blocks are driven from the single I2C client here.
 * The MUIC state is exported through extcon, the charger through
 * power_supply.
 *
 * Copyright (C) 2015 Samsung Electronics
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/extcon-provider.h>
#include <linux/extcon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/* Interrupt and status registers, see downstream s2mu005-private.h */
#define S2MU005_REG_SC_INT		0x00
#define S2MU005_REG_SC_INT_MASK		0x01
#define S2MU005_REG_FLED_INT		0x02
#define S2MU005_REG_FLED_INT_MASK	0x03
#define S2MU005_REG_MUIC_INT1		0x04
#define S2MU005_REG_MUIC_INT2		0x05
#define S2MU005_REG_MUIC_INT1_MASK	0x06
#define S2MU005_REG_MUIC_INT2_MASK	0x07

#define S2MU005_REG_SC_STATUS0		0x08
#define S2MU005_REG_SC_STATUS1		0x09
#define S2MU005_REG_SC_STATUS2		0x0a
#define S2MU005_REG_SC_STATUS3		0x0b
#define S2MU005_REG_SC_STATUS4		0x0c
#define S2MU005_REG_SC_STATUS5		0x0d

/* Charger control registers, see downstream s2mu005_charger.h */
#define S2MU005_REG_SC_CTRL0		0x0e
#define S2MU005_REG_SC_CTRL1		0x0f
#define S2MU005_REG_SC_CTRL2		0x10
#define S2MU005_REG_SC_CTRL3		0x11
#define S2MU005_REG_SC_CTRL4		0x12
#define S2MU005_REG_SC_CTRL5		0x13
#define S2MU005_REG_SC_CTRL6		0x14
#define S2MU005_REG_SC_CTRL7		0x15
#define S2MU005_REG_SC_CTRL8		0x16
#define S2MU005_REG_SC_CTRL9		0x17
#define S2MU005_REG_SC_CTRL10		0x18
#define S2MU005_REG_SC_CTRL11		0x19
#define S2MU005_REG_SC_CTRL12		0x1a
#define S2MU005_REG_SC_CTRL18		0x20

/* Test/hidden registers used by the downstream initialisation sequence */
#define S2MU005_REG_SC_TEST5		0x27
#define S2MU005_REG_SC_TEST8		0x2a
#define S2MU005_REG_MRSTB		0x59
#define S2MU005_REG_PWRSEL_CTRL0	0x5a
#define S2MU005_REG_SELFDIS_CFG2	0x5f
#define S2MU005_REG_SELFDIS_CFG3	0x60
#define S2MU005_REG_REV_ID		0x73
#define S2MU005_REG_FG_CTRL1		0x72
#define S2MU005_REG_FG_CLOCK		0x7b
#define S2MU005_REG_I2C_RESET		0x7c

/* MUIC registers, confirmed against the ROM disassembly of
 * s2mu005_muic_reg_init() (reads 0x49, 0x4a, 0x4b, 0x4c and 0xb5).
 */
#define S2MU005_REG_MUIC_ADC		0x49
#define S2MU005_REG_MUIC_DEVICE_TYPE1	0x4a
#define S2MU005_REG_MUIC_DEVICE_TYPE2	0x4b
#define S2MU005_REG_MUIC_DEVICE_TYPE3	0x4c
#define S2MU005_REG_MUIC_CHG_TYPE	0x50
#define S2MU005_REG_MUIC_DEVICE_APPLE	0x51
#define S2MU005_REG_MUIC_CTRL1		0xb2
#define S2MU005_REG_MUIC_SW_CTRL	0xb5

/* S2MU005_REG_MUIC_CTRL1 bits */
#define S2MU005_MUIC_CTRL1_SWITCH_OPEN	BIT(4)
#define S2MU005_MUIC_CTRL1_WAIT		BIT(1)
#define S2MU005_MUIC_CTRL1_INT_MASK	BIT(0)
/* Downstream writes this value in s2mu005_muic_reg_init() (CTRL_MASK
 * without CONFIG_MUIC_S2MU005_ENABLE_AUTOSW, which is disabled in the
 * grandpplte defconfig).
 */
#define S2MU005_MUIC_CTRL1_INIT		(S2MU005_MUIC_CTRL1_SWITCH_OPEN | \
					 S2MU005_MUIC_CTRL1_WAIT | \
					 S2MU005_MUIC_CTRL1_INT_MASK)
/* The MUIC resets this register to 0xdf after a power glitch. */
#define S2MU005_MUIC_CTRL1_RESET_VALUE	0xdf

/* Manual switch: D- [7:5], D+ [4:2], keep [1:0] (charger/OTG) */
#define S2MU005_MUIC_SW_DM_DP_MASK	GENMASK(7, 2)
#define S2MU005_MUIC_SW_OPEN		0x00
#define S2MU005_MUIC_SW_USB		(0x1 << 5 | 0x1 << 2)
#define S2MU005_MUIC_SW_UART		(0x2 << 5 | 0x2 << 2)

/* Device type 1 (0x4a) */
#define S2MU005_DEV_TYPE1_USB_OTG	BIT(7)
#define S2MU005_DEV_TYPE1_DEDICATED_CHG	BIT(6)
#define S2MU005_DEV_TYPE1_CDP		BIT(5)
#define S2MU005_DEV_TYPE1_T1_T2_CHG	BIT(4)
#define S2MU005_DEV_TYPE1_UART		BIT(3)
#define S2MU005_DEV_TYPE1_USB		BIT(2)
#define S2MU005_DEV_TYPE1_AUDIO_2	BIT(1)
#define S2MU005_DEV_TYPE1_AUDIO_1	BIT(0)
#define S2MU005_DEV_TYPE1_USB_TYPES	(S2MU005_DEV_TYPE1_USB_OTG | \
					 S2MU005_DEV_TYPE1_CDP | \
					 S2MU005_DEV_TYPE1_USB)

/* Device type 2 (0x4b) */
#define S2MU005_DEV_TYPE2_SDP_1P8S	BIT(7)
#define S2MU005_DEV_TYPE2_AV		BIT(6)
#define S2MU005_DEV_TYPE2_JIG_UART_OFF	BIT(3)
#define S2MU005_DEV_TYPE2_JIG_UART_ON	BIT(2)
#define S2MU005_DEV_TYPE2_JIG_USB_OFF	BIT(1)
#define S2MU005_DEV_TYPE2_JIG_USB_ON	BIT(0)
#define S2MU005_DEV_TYPE2_JIG_USB_TYPES	(S2MU005_DEV_TYPE2_JIG_USB_OFF | \
					 S2MU005_DEV_TYPE2_JIG_USB_ON)
#define S2MU005_DEV_TYPE2_JIG_UART_TYPES S2MU005_DEV_TYPE2_JIG_UART_OFF

/* Device type 3 (0x4c) */
#define S2MU005_DEV_TYPE3_AV_WITH_VBUS	BIT(4)

/* Apple device type (0x51) */
#define S2MU005_DEV_TYPE_APPLE_CHG_MASK	GENMASK(7, 4)
#define S2MU005_DEV_TYPE_APPLE_VBUS	BIT(1)

/* Charger type (0x50): VBUS_R255 | U200 | SDP_1P8S */
#define S2MU005_DEV_TYPE_CHG_TYPE	(BIT(7) | BIT(4) | BIT(3))

/* MUIC ADC values (downstream include/linux/muic/muic.h) */
#define S2MU005_ADC_CEA936ATYPE1_CHG	0x17
#define S2MU005_ADC_JIG_USB_OFF		0x18
#define S2MU005_ADC_JIG_USB_ON		0x19
#define S2MU005_ADC_DESKDOCK		0x1a
#define S2MU005_ADC_CEA936ATYPE2_CHG	0x1b
#define S2MU005_ADC_JIG_UART_OFF	0x1c
#define S2MU005_ADC_JIG_UART_ON		0x1d
#define S2MU005_ADC_OPEN		0x1f
#define S2MU005_ADC_CONVERSION		BIT(7)

/* SC_STATUS0: charger state machine */
#define S2MU005_CHG_STATUS_MASK		GENMASK(3, 0)
#define S2MU005_CHG_STATUS_OFF		0x0
#define S2MU005_CHG_STATUS_PRE		0x2
#define S2MU005_CHG_STATUS_TOP_OFF	0x7
#define S2MU005_CHG_STATUS_DONE_FLAG	0x6
#define S2MU005_CHG_STATUS_DONE		0x8
#define S2MU005_CHG_STATUS_INVALID	0xf
#define S2MU005_CHG_STATUS_OK		BIT(4)

/* SC_STATUS2 */
#define S2MU005_VMID_MASK		GENMASK(2, 0)
#define S2MU005_VMID_OTG		0x4

/* SC_STATUS1 */
#define S2MU005_DET_BAT_STATUS		BIT(7)
#define S2MU005_VBUS_STATUS_SHIFT	4
#define S2MU005_VBUS_STATUS_MASK	GENMASK(6, 4)
#define S2MU005_VBUS_STATUS_OVP		2
#define S2MU005_VBUS_STATUS_GOOD_1	3
#define S2MU005_VBUS_STATUS_GOOD_2	5

/* SC_CTRL0 */
#define S2MU005_CHG_MODE_MASK		GENMASK(2, 0)
#define S2MU005_CHG_MODE_OFF		0
#define S2MU005_CHG_MODE_CHARGER	2
#define S2MU005_CHG_MODE_OTG		4

/* SC_CTRL1 */
#define S2MU005_VIN_DROP_MASK		GENMASK(6, 4)
#define S2MU005_MIVR_4500MV		3

/* SC_CTRL2 / SC_CTRL6 / SC_CTRL7 */
#define S2MU005_CURRENT_LIMIT_MASK	GENMASK(5, 0)
#define S2MU005_COOL_CURRENT_LIMIT	0x13	/* 1 A, see downstream */

/* SC_CTRL4 */
#define S2MU005_OTG_OCP_SW_ON		BIT(5)
#define S2MU005_OTG_OCP_SW_OFF		BIT(4)
#define S2MU005_SET_OTG_OCP_SHIFT	2
#define S2MU005_SET_OTG_OCP_MASK	GENMASK(3, 2)

/* SC_CTRL5 */
#define S2MU005_VF_VMID_BST_MASK	GENMASK(4, 0)
#define S2MU005_VF_VMID_BST_5100MV	0x16

/* SC_CTRL8 */
#define S2MU005_VF_VBAT_SHIFT		1
#define S2MU005_VF_VBAT_MASK		GENMASK(6, 1)

/* SC_CTRL10 */
#define S2MU005_FIRST_TOPOFF_MASK	GENMASK(3, 0)
#define S2MU005_SECOND_TOPOFF_SHIFT	4
#define S2MU005_SECOND_TOPOFF_MASK	GENMASK(7, 4)

/* SC_CTRL11 */
#define S2MU005_OSC_BST_SHIFT		5
#define S2MU005_OSC_BST_MASK		GENMASK(6, 5)

/* SC_INT_MASK */
#define S2MU005_INT_VMID_MASK		BIT(5)

/*
 * Interrupt mask initialisation: unmask only what this driver consumes.
 * The registers are write-1-to-mask.
 *   SC_INT     : DET_BAT (bit 0), CHG (bit 4)
 *   MUIC_INT1  : ATTACH (bit 0), DETACH (bit 1)
 *   MUIC_INT2  : VBUS_ON (bit 0), ADC_CHANGE (bit 2), VBUS_OFF (bit 7)
 */
#define S2MU005_SC_INT_MASK_INIT	0xee
#define S2MU005_MUIC_INT1_MASK_INIT	0xfc
#define S2MU005_MUIC_INT2_MASK_INIT	0x7a

#define S2MU005_CHG_PSY_NAME		"s2mu005-charger"
#define S2MU005_FG_PSY_NAME		"s2mu005-fuel-gauge"
#define S2MU005_HEALTH_DEBOUNCE_CNT	3

enum s2mu005_cable {
	S2MU005_CABLE_NONE = 0,
	S2MU005_CABLE_USB_SDP,
	S2MU005_CABLE_USB_CDP,
	S2MU005_CABLE_TA_DCP,
	S2MU005_CABLE_UNDEFINED,
	S2MU005_CABLE_JIG_USB_OFF,
	S2MU005_CABLE_JIG_USB_ON,
	S2MU005_CABLE_JIG_UART,
	S2MU005_CABLE_JIG_UART_VBUS,
	S2MU005_CABLE_OTG,
	S2MU005_CABLE_DESKDOCK,
	S2MU005_CABLE_DESKDOCK_VBUS,
};

struct s2mu005_chg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct extcon_dev *edev;
	struct notifier_block host_nb;

	struct mutex lock;
	struct work_struct detect_work;
	struct work_struct host_work;

	int irq;
	unsigned int rev;

	enum s2mu005_cable cable;
	bool online;
	bool charging_enabled;
	bool suspended;
	bool need_notify;
	int usb_type;
	int charge_behaviour;

	/* Limits in micro units.  The float voltage comes from the device
	 * tree, the current limits are replaced with the per-cable defaults
	 * on every attach and can be overridden through the power supply
	 * properties.
	 */
	unsigned int input_current_limit_ua;
	unsigned int fast_charge_current_ua;
	unsigned int float_voltage_uv;
	unsigned int term_current_ua;
	bool eoc_dualpath;
	bool always_enable;
	bool wakeup;

	/* Charger health debounce, as in s2mu005_get_charging_health() */
	int unhealth_cnt;
	bool ovp;

	/* Fuel-gauge clock divider saved at probe, used by suspend/resume */
	unsigned int fg_clock;

	/* Flash/torch LED child platform device */
	struct platform_device *fled_pdev;
};

static const unsigned int s2mu005_extcon_cables[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_SLOW,
	EXTCON_DOCK,
	EXTCON_JIG,
	EXTCON_NONE,
};

static const struct regmap_config s2mu005_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

/* Charger helpers */

/*
 * Per-cable charging parameters from the SM-G532M battery device tree
 * (mt6737t_grandpplte_cis_ser_battery_01.dtsi, battery,input_current_limit /
 * battery,fast_charging_current / battery,full_check_current_1st), indexed by
 * POWER_SUPPLY_TYPE in sec_bat_cable_check() (sec_battery.c):
 *
 *   USB (SDP, JIG USB OFF)  :  500 mA /  700 mA / 240 mA
 *   USB_CDP                 : 1000 mA / 1550 mA / 240 mA
 *   MAINS (TA, deskdock+VBUS,
 *          undefined cable) : 1000 mA / 1550 mA / 240 mA
 *   UARTOFF (JIG UART OFF+VBUS) : 1000 mA / 2100 mA / 240 mA
 *
 * The terminal current is the downstream full_check_current_1st value, which
 * the battery stack programs through POWER_SUPPLY_PROP_CURRENT_FULL and uses
 * for its full-charge detection.
 */
struct s2mu005_cable_cfg {
	unsigned int input_ua;
	unsigned int fast_ua;
	unsigned int term_ua;
};

#define S2MU005_CFG_USB \
	{ .input_ua = 500 * 1000, .fast_ua = 700 * 1000, .term_ua = 240 * 1000 }
#define S2MU005_CFG_CDP \
	{ .input_ua = 1000 * 1000, .fast_ua = 1550 * 1000, .term_ua = 240 * 1000 }
#define S2MU005_CFG_MAINS \
	{ .input_ua = 1000 * 1000, .fast_ua = 1550 * 1000, .term_ua = 240 * 1000 }
#define S2MU005_CFG_UARTOFF \
	{ .input_ua = 1000 * 1000, .fast_ua = 2100 * 1000, .term_ua = 240 * 1000 }
#define S2MU005_CFG_NONE \
	{ }

static const struct s2mu005_cable_cfg s2mu005_cable_cfgs[] = {
	[S2MU005_CABLE_NONE]		= S2MU005_CFG_NONE,
	[S2MU005_CABLE_USB_SDP]		= S2MU005_CFG_USB,
	[S2MU005_CABLE_USB_CDP]		= S2MU005_CFG_CDP,
	[S2MU005_CABLE_TA_DCP]		= S2MU005_CFG_MAINS,
	[S2MU005_CABLE_UNDEFINED]	= S2MU005_CFG_MAINS,
	[S2MU005_CABLE_JIG_USB_OFF]	= S2MU005_CFG_USB,
	[S2MU005_CABLE_JIG_USB_ON]	= S2MU005_CFG_NONE,
	[S2MU005_CABLE_JIG_UART]	= S2MU005_CFG_NONE,
	[S2MU005_CABLE_JIG_UART_VBUS]	= S2MU005_CFG_UARTOFF,
	[S2MU005_CABLE_OTG]		= S2MU005_CFG_NONE,
	[S2MU005_CABLE_DESKDOCK]	= S2MU005_CFG_NONE,
	[S2MU005_CABLE_DESKDOCK_VBUS]	= S2MU005_CFG_MAINS,
};

static bool s2mu005_cable_is_charger(enum s2mu005_cable cable)
{
	switch (cable) {
	case S2MU005_CABLE_USB_SDP:
	case S2MU005_CABLE_USB_CDP:
	case S2MU005_CABLE_TA_DCP:
	case S2MU005_CABLE_UNDEFINED:
	case S2MU005_CABLE_JIG_USB_OFF:
	case S2MU005_CABLE_JIG_UART_VBUS:
	case S2MU005_CABLE_DESKDOCK_VBUS:
		return true;
	default:
		return false;
	}
}

/*
 * Set the input current limit.  Downstream s2mu005_set_input_current_limit():
 * 100 mA to 2600 mA in 50 mA steps.
 */
static int s2mu005_set_input_current_limit(struct s2mu005_chg *priv,
					   unsigned int ua)
{
	unsigned int ma = ua / 1000;
	unsigned int data;

	if (ma <= 100)
		data = 0;
	else if (ma <= 2600)
		data = (ma - 100) / 50;
	else
		data = S2MU005_CURRENT_LIMIT_MASK;

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL2,
				  S2MU005_CURRENT_LIMIT_MASK, data);
}

static int s2mu005_get_input_current_limit(struct s2mu005_chg *priv,
					   int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL2, &val);
	if (ret)
		return ret;

	val &= S2MU005_CURRENT_LIMIT_MASK;
	*ua = (val * 50 + 100) * 1000;

	return 0;
}

/*
 * Set the fast charge current.  Downstream s2mu005_set_fast_charging_current()
 * programs the fast-charge and the cool-charge current (the latter capped at
 * 1 A as a boot stability workaround).
 */
static int s2mu005_set_fast_charge_current(struct s2mu005_chg *priv,
					   unsigned int ua)
{
	unsigned int ma = ua / 1000;
	unsigned int data, cool;
	int ret;

	if (ma <= 100)
		data = 0;
	else if (ma <= 2600)
		data = ((ma - 100) / 50) + 1;
	else
		data = 0x33;

	cool = min(data, (unsigned int)S2MU005_COOL_CURRENT_LIMIT);

	ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL7,
				 S2MU005_CURRENT_LIMIT_MASK, data);
	if (ret)
		return ret;

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL6,
				  S2MU005_CURRENT_LIMIT_MASK, cool);
}

static int s2mu005_get_fast_charge_current(struct s2mu005_chg *priv, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL7, &val);
	if (ret)
		return ret;

	val &= S2MU005_CURRENT_LIMIT_MASK;
	*ua = ((int)val - 1) * 50 * 1000 + 100 * 1000;

	return 0;
}

/* Downstream s2mu005_set_regulation_voltage(): 3.9 V to 4.4 V in 10 mV steps */
static int s2mu005_set_float_voltage(struct s2mu005_chg *priv,
				     unsigned int uv)
{
	unsigned int mv = uv / 1000;
	unsigned int data;

	if (mv <= 3900)
		data = 0;
	else if (mv <= 4400)
		data = (mv - 3900) / 10;
	else
		data = 0x32;

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL8,
				  S2MU005_VF_VBAT_MASK,
				  data << S2MU005_VF_VBAT_SHIFT);
}

static int s2mu005_get_float_voltage(struct s2mu005_chg *priv, int *uv)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL8, &val);
	if (ret)
		return ret;

	val = (val & S2MU005_VF_VBAT_MASK) >> S2MU005_VF_VBAT_SHIFT;
	*uv = (3900 + val * 10) * 1000;

	return 0;
}

/* Downstream s2mu005_set_topoff_current(): 100 mA to 475 mA in 25 mA steps */
static int s2mu005_set_term_current(struct s2mu005_chg *priv, int slot,
				    unsigned int ua)
{
	unsigned int ma = ua / 1000;
	unsigned int data;

	if (ma <= 100)
		data = 0;
	else if (ma <= 475)
		data = (ma - 100) / 25;
	else
		data = S2MU005_FIRST_TOPOFF_MASK;

	if (slot == 1)
		return regmap_update_bits(priv->regmap,
					  S2MU005_REG_SC_CTRL10,
					  S2MU005_FIRST_TOPOFF_MASK, data);

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL10,
				  S2MU005_SECOND_TOPOFF_MASK,
				  data << S2MU005_SECOND_TOPOFF_SHIFT);
}

static int s2mu005_get_term_current(struct s2mu005_chg *priv, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL10, &val);
	if (ret)
		return ret;

	val &= S2MU005_FIRST_TOPOFF_MASK;
	*ua = (val * 25 + 100) * 1000;

	return 0;
}

/* Downstream s2mu005_set_mivr_level(): enable MIVR and select 4.5 V */
static int s2mu005_set_mivr_level(struct s2mu005_chg *priv)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL12, &val);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL12,
			   val | BIT(7));
	if (ret)
		return ret;

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL1,
				  S2MU005_VIN_DROP_MASK,
				  S2MU005_MIVR_4500MV << 4);
}

/*
 * Downstream s2mu005_enable_charger_switch().  The fast charge current is
 * momentarily raised to 1.7 A to prevent a VSYS drop, and the asynchronous
 * mode change time is extended to 150 ms around the OFF->CHARGER transition.
 * The configured fast charge current is restored by the caller afterwards.
 */
static int s2mu005_enable_charger_switch(struct s2mu005_chg *priv, bool on)
{
	int ret;

	ret = s2mu005_set_fast_charge_current(priv, 1700 * 1000);
	if (ret)
		return ret;

	msleep(20);

	if (on) {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
					 S2MU005_CHG_MODE_MASK,
					 S2MU005_CHG_MODE_OFF);
		if (ret)
			return ret;
		msleep(50);

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_TEST8,
					 BIT(3), 0);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
					 S2MU005_CHG_MODE_MASK,
					 S2MU005_CHG_MODE_CHARGER);
		if (ret)
			return ret;
		msleep(150);

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_TEST8,
					 BIT(3), BIT(3));
		if (ret)
			return ret;
	} else {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
					 S2MU005_CHG_MODE_MASK,
					 S2MU005_CHG_MODE_OFF);
		if (ret)
			return ret;
	}

	priv->charging_enabled = on;

	return 0;
}

/*
 * Downstream s2mu005_charger_otg_control(): enable the 5.1 V boost, 1 MHz
 * switching frequency and a 1.5 A OTG over-current limit, and switch the
 * charger block into OTG mode.
 */
static int s2mu005_set_otg(struct s2mu005_chg *priv, bool on)
{
	int ret;

	if (on) {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_INT_MASK,
					 S2MU005_INT_VMID_MASK, 0);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
					 S2MU005_CHG_MODE_MASK,
					 S2MU005_CHG_MODE_OTG);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL11,
					 S2MU005_OSC_BST_MASK,
					 2 << S2MU005_OSC_BST_SHIFT);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL4,
					 S2MU005_SET_OTG_OCP_MASK,
					 3 << S2MU005_SET_OTG_OCP_SHIFT);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL4,
					 S2MU005_OTG_OCP_SW_OFF,
					 S2MU005_OTG_OCP_SW_OFF);
		if (ret)
			return ret;

		return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL5,
					  S2MU005_VF_VMID_BST_MASK,
					  S2MU005_VF_VMID_BST_5100MV);
	}

	ret = regmap_update_bits(priv->regmap, S2MU005_REG_SC_INT_MASK,
				 S2MU005_INT_VMID_MASK,
				 S2MU005_INT_VMID_MASK);
	if (ret)
		return ret;

	if (priv->charging_enabled && priv->online)
		return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
					  S2MU005_CHG_MODE_MASK,
					  S2MU005_CHG_MODE_CHARGER);

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_CTRL0,
				  S2MU005_CHG_MODE_MASK,
				  S2MU005_CHG_MODE_OFF);
}

/*
 * Downstream s2mu005_charger_initialize(), run by the vendor battery stack
 * at boot and again after a fuel-gauge reset.  The register values are
 * copied verbatim from the downstream driver; 0x87/0x89/0x92/0x97/0x9a/
 * 0x9c/0x9e/0xa1/0xa4/0xa5/0xa8 are undocumented test registers on the
 * charger side of the die.
 */
static int s2mu005_charger_hw_init(struct s2mu005_chg *priv)
{
	static const u8 evt0_init[][2] = {
		{ 0x87, 0x00 }, { 0x92, 0xe5 }, { 0x97, 0x85 },
		{ 0x9a, 0x67 }, { 0x9c, 0xea }, { 0x9e, 0x6e },
		{ 0xa1, 0x20 }, { 0xa4, 0x0a }, { 0xa5, 0x45 },
	};
	unsigned int val, adc;
	size_t i;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_PWRSEL_CTRL0, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, S2MU005_REG_PWRSEL_CTRL0,
			   val | BIT(7));
	if (ret)
		return ret;

	if (priv->rev == 0) {
		for (i = 0; i < ARRAY_SIZE(evt0_init); i++) {
			ret = regmap_write(priv->regmap, evt0_init[i][0],
					   evt0_init[i][1]);
			if (ret)
				return ret;
		}

		/* Workaround for an undefined accessory with VBUS present. */
		ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_APPLE,
				  &val);
		if (ret)
			return ret;
		if (val & S2MU005_DEV_TYPE_APPLE_VBUS) {
			ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_ADC,
					  &adc);
			if (ret)
				return ret;

			switch (adc & 0x1f) {
			case S2MU005_ADC_JIG_USB_OFF:
			case S2MU005_ADC_JIG_USB_ON:
			case S2MU005_ADC_JIG_UART_OFF:
			case S2MU005_ADC_JIG_UART_ON:
				break;
			default:
				ret = regmap_read(priv->regmap, 0x89, &val);
				if (ret)
					return ret;
				ret = regmap_write(priv->regmap, 0x89,
						   (val & ~GENMASK(1, 0)) | 0x01);
				if (ret)
					return ret;
				break;
			}
		}
	}

	/* Fastest speed for the QBAT switch. */
	ret = regmap_read(priv->regmap, 0x87, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, 0x87, val & ~GENMASK(7, 4));
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_SC_TEST5, 0x51);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL12, 0x91);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL5, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL5,
			   val & ~GENMASK(6, 5));
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL9, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL9,
			   val & ~GENMASK(5, 4));
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, 0xa8, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, 0xa8, (val & ~BIT(7)) | BIT(7));
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL1, 0x50);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, 0x89, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, 0x89, val & ~BIT(7));
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, 0xa5, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, 0xa5, val & ~BIT(2));
	if (ret)
		return ret;

	/* TOP-OFF timer: 90 minutes. */
	ret = regmap_read(priv->regmap, S2MU005_REG_SC_CTRL18, &val);
	if (ret)
		return ret;
	ret = regmap_write(priv->regmap, S2MU005_REG_SC_CTRL18,
			   (val & ~GENMASK(5, 3)) | (0x6 << 3));
	if (ret)
		return ret;

	ret = s2mu005_set_mivr_level(priv);
	if (ret)
		return ret;

	ret = s2mu005_set_float_voltage(priv, priv->float_voltage_uv);
	if (ret)
		return ret;

	ret = s2mu005_set_term_current(priv, 1, priv->term_current_ua);
	if (ret)
		return ret;

	if (priv->eoc_dualpath)
		return s2mu005_set_term_current(priv, 2, priv->term_current_ua);

	return 0;
}

/*
 * Downstream s2mu005_chg_init(): read the silicon revision, enable the
 * self-discharge mode, save the fuel-gauge clock divider and shorten the
 * asynchronous mode change time.
 */
static int s2mu005_charger_early_init(struct s2mu005_chg *priv)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_REV_ID, &val);
	if (ret)
		return ret;
	priv->rev = val & 0x0f;

	ret = regmap_update_bits(priv->regmap, S2MU005_REG_SELFDIS_CFG3,
				 BIT(7), BIT(7));
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_FG_CLOCK, &val);
	if (ret)
		return ret;
	priv->fg_clock = val;

	return regmap_update_bits(priv->regmap, S2MU005_REG_SC_TEST8,
				  BIT(3), BIT(3));
}

/* Charger status and properties */

static int s2mu005_get_avg_current(struct s2mu005_chg *priv, int *ua)
{
	struct power_supply *fg;
	union power_supply_propval val;
	int ret;

	fg = power_supply_get_by_name(S2MU005_FG_PSY_NAME);
	if (!fg)
		return -ENODEV;

	ret = power_supply_get_property(fg, POWER_SUPPLY_PROP_CURRENT_AVG, &val);
	power_supply_put(fg);
	if (ret)
		return ret;

	*ua = val.intval;

	return 0;
}

static int s2mu005_get_status(struct s2mu005_chg *priv, int *status)
{
	unsigned int val;
	int avg_ua = 0;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_STATUS0, &val);
	if (ret)
		return ret;

	switch (val & S2MU005_CHG_STATUS_MASK) {
	case S2MU005_CHG_STATUS_OFF:
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case S2MU005_CHG_STATUS_PRE:
	case 0x3:		/* cool charge */
	case 0x4:		/* constant current */
	case 0x5:		/* constant voltage */
		*status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case S2MU005_CHG_STATUS_TOP_OFF:
	case S2MU005_CHG_STATUS_DONE_FLAG:
	case S2MU005_CHG_STATUS_DONE:
		ret = s2mu005_get_avg_current(priv, &avg_ua);
		if (!ret && avg_ua < (int)priv->term_current_ua)
			*status = POWER_SUPPLY_STATUS_FULL;
		else
			*status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case S2MU005_CHG_STATUS_INVALID:
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static int s2mu005_get_charge_type(struct s2mu005_chg *priv, int *type)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_STATUS0, &val);
	if (ret)
		return ret;

	if (val & S2MU005_CHG_STATUS_OK)
		*type = POWER_SUPPLY_CHARGE_TYPE_FAST;
	else
		*type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;

	return 0;
}

static int s2mu005_get_health(struct s2mu005_chg *priv, int *health)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_STATUS1, &val);
	if (ret)
		return ret;

	switch ((val & S2MU005_VBUS_STATUS_MASK) >>
		S2MU005_VBUS_STATUS_SHIFT) {
	case S2MU005_VBUS_STATUS_GOOD_1:
	case S2MU005_VBUS_STATUS_GOOD_2:
		priv->ovp = false;
		priv->unhealth_cnt = 0;
		*health = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	case S2MU005_VBUS_STATUS_OVP:
		priv->ovp = true;
		break;
	default:
		break;
	}

	if (priv->unhealth_cnt < S2MU005_HEALTH_DEBOUNCE_CNT) {
		priv->unhealth_cnt++;
		*health = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	}

	priv->unhealth_cnt = S2MU005_HEALTH_DEBOUNCE_CNT;
	*health = priv->ovp ? POWER_SUPPLY_HEALTH_OVERVOLTAGE :
			      POWER_SUPPLY_HEALTH_UNDERVOLTAGE;

	return 0;
}

static int s2mu005_get_battery_present(struct s2mu005_chg *priv, int *present)
{
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_SC_STATUS1, &val);
	if (ret)
		return ret;

	*present = !!(val & S2MU005_DET_BAT_STATUS);

	return 0;
}

static int s2mu005_chg_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct s2mu005_chg *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&priv->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = priv->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = s2mu005_get_status(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = s2mu005_get_charge_type(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		val->intval = priv->charge_behaviour;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = s2mu005_get_health(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = s2mu005_get_battery_present(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = priv->usb_type;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = s2mu005_get_input_current_limit(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (priv->charging_enabled) {
			int aicr, chg;

			ret = s2mu005_get_input_current_limit(priv, &aicr);
			if (!ret)
				ret = s2mu005_get_fast_charge_current(priv, &chg);
			if (!ret)
				val->intval = min(aicr, chg);
		} else {
			val->intval = 0;
		}
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = s2mu005_get_fast_charge_current(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = s2mu005_get_float_voltage(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = s2mu005_get_term_current(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "S2MU005";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Samsung";
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&priv->lock);

	return ret;
}

static int s2mu005_chg_set_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    const union power_supply_propval *val)
{
	struct s2mu005_chg *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&priv->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		if (val->intval == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) {
			priv->charge_behaviour =
				POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
			if (priv->online && priv->cable != S2MU005_CABLE_OTG)
				ret = s2mu005_enable_charger_switch(priv, true);
		} else if (val->intval ==
			   POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
			priv->charge_behaviour =
				POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
			ret = s2mu005_enable_charger_switch(priv, false);
		} else {
			ret = -EINVAL;
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = s2mu005_set_input_current_limit(priv, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (priv->charging_enabled)
			ret = s2mu005_set_fast_charge_current(priv, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = s2mu005_set_float_voltage(priv, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		priv->term_current_ua = val->intval;
		ret = s2mu005_set_term_current(priv, 1, val->intval);
		if (!ret && priv->eoc_dualpath)
			ret = s2mu005_set_term_current(priv, 2, 100 * 1000);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&priv->lock);

	if (!ret)
		power_supply_changed(priv->psy);

	return ret;
}

static int s2mu005_chg_property_is_writeable(struct power_supply *psy,
					     enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		return 1;
	default:
		return 0;
	}
}

static const enum power_supply_property s2mu005_chg_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static const struct power_supply_desc s2mu005_chg_desc = {
	.name = S2MU005_CHG_PSY_NAME,
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP),
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
	.properties = s2mu005_chg_properties,
	.num_properties = ARRAY_SIZE(s2mu005_chg_properties),
	.get_property = s2mu005_chg_get_property,
	.set_property = s2mu005_chg_set_property,
	.property_is_writeable = s2mu005_chg_property_is_writeable,
};

/* MUIC handling */

static int s2mu005_muic_set_manual_sw(struct s2mu005_chg *priv, u8 val)
{
	unsigned int cur;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_SW_CTRL, &cur);
	if (ret)
		return ret;

	if ((val & S2MU005_MUIC_SW_DM_DP_MASK) ==
	    (cur & S2MU005_MUIC_SW_DM_DP_MASK))
		return 0;

	return regmap_write(priv->regmap, S2MU005_REG_MUIC_SW_CTRL,
			    (val & S2MU005_MUIC_SW_DM_DP_MASK) | (cur & 0x3));
}

/*
 * Downstream s2mu005_muic_reg_init(): program the manual switch according to
 * the accessory already present at boot, then enable the MUIC interrupts.
 */
static int s2mu005_muic_hw_init(struct s2mu005_chg *priv)
{
	unsigned int dev1, dev2, sw;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_TYPE1, &dev1);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_TYPE2, &dev2);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_SW_CTRL, &sw);
	if (ret)
		return ret;

	if ((dev1 & S2MU005_DEV_TYPE1_USB_TYPES) ||
	    (dev2 & S2MU005_DEV_TYPE2_JIG_USB_TYPES))
		ret = regmap_write(priv->regmap, S2MU005_REG_MUIC_SW_CTRL,
				   (sw & 0x3) | S2MU005_MUIC_SW_USB);
	else if (dev2 & S2MU005_DEV_TYPE2_JIG_UART_TYPES)
		ret = regmap_write(priv->regmap, S2MU005_REG_MUIC_SW_CTRL,
				   (sw & 0x3) | S2MU005_MUIC_SW_UART);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_MUIC_CTRL1,
			   S2MU005_MUIC_CTRL1_INIT);
	if (ret)
		return ret;

	/* Unmask the MUIC interrupt. */
	return regmap_update_bits(priv->regmap, S2MU005_REG_MUIC_CTRL1,
				  S2MU005_MUIC_CTRL1_INT_MASK, 0);
}

static int s2mu005_muic_irq_masks_init(struct s2mu005_chg *priv)
{
	int ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_SC_INT_MASK,
			   S2MU005_SC_INT_MASK_INIT);
	if (ret)
		return ret;

	/* The flash LED block is not driven here: mask all of its sources as
	 * the downstream MFD interrupt initialisation does (s2mu005_irq.c).
	 */
	ret = regmap_write(priv->regmap, S2MU005_REG_FLED_INT_MASK, 0xff);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, S2MU005_REG_MUIC_INT1_MASK,
			   S2MU005_MUIC_INT1_MASK_INIT);
	if (ret)
		return ret;

	return regmap_write(priv->regmap, S2MU005_REG_MUIC_INT2_MASK,
			    S2MU005_MUIC_INT2_MASK_INIT);
}

/*
 * Downstream s2mu005_muic_detect_dev().  Returns -EAGAIN while the MUIC ADC
 * is still converting, in which case the previous attachment state is kept.
 */
static int s2mu005_muic_detect(struct s2mu005_chg *priv,
			       enum s2mu005_cable *cable)
{
	enum s2mu005_cable new = S2MU005_CABLE_NONE;
	unsigned int dev1, dev2, dev3, adc, apple, chg_type, status2;
	bool vbool;
	int ret;

	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_TYPE1, &dev1);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_TYPE2, &dev2);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_TYPE3, &dev3);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_ADC, &adc);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_APPLE, &apple);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_CHG_TYPE, &chg_type);
	if (ret)
		return ret;
	ret = regmap_read(priv->regmap, S2MU005_REG_SC_STATUS2, &status2);
	if (ret)
		return ret;

	if (adc & S2MU005_ADC_CONVERSION)
		return -EAGAIN;

	vbool = !!(apple & S2MU005_DEV_TYPE_APPLE_VBUS);

	/* EVT0 workaround: with VBUS present always report a USB cable. */
	if (priv->rev == 0 && vbool)
		new = S2MU005_CABLE_USB_SDP;

	switch (dev1) {
	case S2MU005_DEV_TYPE1_CDP:
		new = S2MU005_CABLE_USB_CDP;
		break;
	case S2MU005_DEV_TYPE1_USB:
		new = S2MU005_CABLE_USB_SDP;
		break;
	case S2MU005_DEV_TYPE1_DEDICATED_CHG:
		new = S2MU005_CABLE_TA_DCP;
		break;
	case S2MU005_DEV_TYPE1_USB_OTG:
		new = S2MU005_CABLE_OTG;
		if ((status2 & S2MU005_VMID_MASK) == S2MU005_VMID_OTG)
			vbool = true;
		break;
	case S2MU005_DEV_TYPE1_T1_T2_CHG:
		if (adc == S2MU005_ADC_CEA936ATYPE2_CHG)
			new = S2MU005_CABLE_TA_DCP;
		else
			new = S2MU005_CABLE_USB_SDP;
		break;
	default:
		break;
	}

	switch (dev2) {
	case S2MU005_DEV_TYPE2_SDP_1P8S:
		new = S2MU005_CABLE_USB_SDP;
		break;
	case S2MU005_DEV_TYPE2_JIG_UART_OFF:
		if (vbool)
			new = S2MU005_CABLE_JIG_UART_VBUS;
		else
			new = S2MU005_CABLE_JIG_UART;
		break;
	case S2MU005_DEV_TYPE2_JIG_UART_ON:
		new = S2MU005_CABLE_JIG_UART;
		break;
	case S2MU005_DEV_TYPE2_JIG_USB_OFF:
		if (vbool)
			new = S2MU005_CABLE_JIG_USB_OFF;
		break;
	case S2MU005_DEV_TYPE2_JIG_USB_ON:
		if (vbool)
			new = S2MU005_CABLE_JIG_USB_ON;
		break;
	default:
		break;
	}

	if (priv->rev > 0) {
		/* Apple chargers */
		if (vbool && (apple & S2MU005_DEV_TYPE_APPLE_CHG_MASK))
			new = S2MU005_CABLE_TA_DCP;

		/* Workaround for the LG USB cable with a 219k ID resistor */
		if ((chg_type & S2MU005_DEV_TYPE_CHG_TYPE) &&
		    new == S2MU005_CABLE_NONE) {
			if (adc == S2MU005_ADC_CEA936ATYPE1_CHG ||
			    adc == S2MU005_ADC_JIG_USB_OFF)
				new = S2MU005_CABLE_USB_SDP;
			else
				new = S2MU005_CABLE_TA_DCP;
		}

		if ((dev2 & S2MU005_DEV_TYPE2_AV) ||
		    (dev3 & S2MU005_DEV_TYPE3_AV_WITH_VBUS))
			new = vbool ? S2MU005_CABLE_DESKDOCK_VBUS :
				      S2MU005_CABLE_DESKDOCK;

		/* Fall back to the ADC when the device type registers are empty */
		if (new == S2MU005_CABLE_NONE) {
			switch (adc) {
			case S2MU005_ADC_CEA936ATYPE1_CHG:
				if (vbool)
					new = S2MU005_CABLE_USB_SDP;
				break;
			case S2MU005_ADC_CEA936ATYPE2_CHG:
				if (vbool)
					new = S2MU005_CABLE_TA_DCP;
				break;
			case S2MU005_ADC_JIG_USB_OFF:
				if (vbool)
					new = S2MU005_CABLE_JIG_USB_OFF;
				break;
			case S2MU005_ADC_JIG_USB_ON:
				if (vbool)
					new = S2MU005_CABLE_JIG_USB_ON;
				break;
			case S2MU005_ADC_JIG_UART_OFF:
				new = vbool ? S2MU005_CABLE_JIG_UART_VBUS :
					      S2MU005_CABLE_JIG_UART;
				break;
			case S2MU005_ADC_JIG_UART_ON:
				new = S2MU005_CABLE_JIG_UART;
				break;
			case S2MU005_ADC_DESKDOCK:
				new = vbool ? S2MU005_CABLE_DESKDOCK_VBUS :
					      S2MU005_CABLE_DESKDOCK;
				break;
			default:
				break;
			}
		}
	}

	if (new == S2MU005_CABLE_NONE && adc != S2MU005_ADC_OPEN && vbool)
		new = S2MU005_CABLE_UNDEFINED;

	*cable = new;

	return 0;
}

/* Downstream com_to_*() helpers */
static int s2mu005_muic_route(struct s2mu005_chg *priv,
			      enum s2mu005_cable cable)
{
	switch (cable) {
	case S2MU005_CABLE_USB_SDP:
	case S2MU005_CABLE_USB_CDP:
	case S2MU005_CABLE_JIG_USB_OFF:
	case S2MU005_CABLE_JIG_USB_ON:
	case S2MU005_CABLE_OTG:
		return s2mu005_muic_set_manual_sw(priv, S2MU005_MUIC_SW_USB);
	case S2MU005_CABLE_JIG_UART:
	case S2MU005_CABLE_JIG_UART_VBUS:
		return s2mu005_muic_set_manual_sw(priv, S2MU005_MUIC_SW_UART);
	case S2MU005_CABLE_TA_DCP:
	case S2MU005_CABLE_UNDEFINED:
	case S2MU005_CABLE_NONE:
		return s2mu005_muic_set_manual_sw(priv, S2MU005_MUIC_SW_OPEN);
	default:
		/* Deskdocks keep the current switch setting. */
		return 0;
	}
}

static void s2mu005_muic_report(struct s2mu005_chg *priv,
				enum s2mu005_cable cable)
{
	bool usb = false, host = false, sdp = false, cdp = false;
	bool dcp = false, slow = false, dock = false, jig = false;

	switch (cable) {
	case S2MU005_CABLE_USB_SDP:
		usb = true;
		sdp = true;
		break;
	case S2MU005_CABLE_JIG_USB_OFF:
		usb = true;
		sdp = true;
		jig = true;
		break;
	case S2MU005_CABLE_JIG_USB_ON:
		usb = true;
		jig = true;
		break;
	case S2MU005_CABLE_USB_CDP:
		usb = true;
		cdp = true;
		break;
	case S2MU005_CABLE_TA_DCP:
		dcp = true;
		break;
	case S2MU005_CABLE_UNDEFINED:
		slow = true;
		break;
	case S2MU005_CABLE_JIG_UART:
	case S2MU005_CABLE_JIG_UART_VBUS:
		jig = true;
		break;
	case S2MU005_CABLE_OTG:
		host = true;
		break;
	case S2MU005_CABLE_DESKDOCK:
	case S2MU005_CABLE_DESKDOCK_VBUS:
		dock = true;
		break;
	default:
		break;
	}

	extcon_set_state_sync(priv->edev, EXTCON_USB, usb);
	extcon_set_state_sync(priv->edev, EXTCON_CHG_USB_SDP, sdp);
	extcon_set_state_sync(priv->edev, EXTCON_CHG_USB_CDP, cdp);
	extcon_set_state_sync(priv->edev, EXTCON_CHG_USB_DCP, dcp);
	extcon_set_state_sync(priv->edev, EXTCON_CHG_USB_SLOW, slow);
	extcon_set_state_sync(priv->edev, EXTCON_DOCK, dock);
	extcon_set_state_sync(priv->edev, EXTCON_JIG, jig);
	extcon_set_state_sync(priv->edev, EXTCON_USB_HOST, host);
}

/*
 * Apply the charger state that follows from the attached cable, mirroring
 * what downstream sec_battery does through POWER_SUPPLY_PROP_ONLINE,
 * CURRENT_MAX, CURRENT_NOW and CHARGING_ENABLED.
 */
static int s2mu005_charger_update(struct s2mu005_chg *priv)
{
	const struct s2mu005_cable_cfg *cfg = &s2mu005_cable_cfgs[priv->cable];
	bool charger = s2mu005_cable_is_charger(priv->cable);
	int ret;

	priv->online = charger;

	switch (priv->cable) {
	case S2MU005_CABLE_USB_CDP:
		priv->usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		break;
	case S2MU005_CABLE_TA_DCP:
		priv->usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		break;
	case S2MU005_CABLE_USB_SDP:
	case S2MU005_CABLE_JIG_USB_OFF:
		priv->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		break;
	default:
		priv->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		break;
	}

	if (priv->cable == S2MU005_CABLE_OTG)
		return 0;

	if (!charger) {
		if (priv->charging_enabled)
			return s2mu005_enable_charger_switch(priv, false);
		return 0;
	}

	/* Apply the downstream per-cable charging parameters. */
	priv->input_current_limit_ua = cfg->input_ua;
	priv->fast_charge_current_ua = cfg->fast_ua;
	priv->term_current_ua = cfg->term_ua;

	ret = s2mu005_set_mivr_level(priv);
	if (ret)
		return ret;

	ret = s2mu005_set_float_voltage(priv, priv->float_voltage_uv);
	if (ret)
		return ret;

	ret = s2mu005_set_term_current(priv, 1, priv->term_current_ua);
	if (ret)
		return ret;

	if (priv->eoc_dualpath) {
		/* Downstream fixes the second EOC path at 100 mA. */
		ret = s2mu005_set_term_current(priv, 2, 100 * 1000);
		if (ret)
			return ret;
	}

	if (priv->charge_behaviour != POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO)
		return 0;

	ret = s2mu005_enable_charger_switch(priv, true);
	if (ret)
		return ret;

	/* Restore the configured current after the 1.7 A enable sequence. */
	ret = s2mu005_set_fast_charge_current(priv, priv->fast_charge_current_ua);
	if (ret)
		return ret;

	return s2mu005_set_input_current_limit(priv, priv->input_current_limit_ua);
}

static void s2mu005_detect_work(struct work_struct *work)
{
	struct s2mu005_chg *priv = container_of(work, struct s2mu005_chg,
						detect_work);
	enum s2mu005_cable cable;
	bool changed = false;
	unsigned int ctrl;
	int ret;

	mutex_lock(&priv->lock);

	/* The MUIC resets its control register after a power glitch. */
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_CTRL1, &ctrl);
	if (!ret && ctrl == S2MU005_MUIC_CTRL1_RESET_VALUE) {
		dev_info(priv->dev, "MUIC was reset, re-initialising\n");
		s2mu005_muic_hw_init(priv);
		s2mu005_muic_irq_masks_init(priv);
	}

	ret = s2mu005_muic_detect(priv, &cable);
	if (!ret && cable != priv->cable) {
		priv->cable = cable;
		ret = s2mu005_muic_route(priv, cable);
		if (ret)
			dev_err(priv->dev, "failed to route MUIC switch: %d\n",
				ret);
		ret = s2mu005_charger_update(priv);
		if (ret)
			dev_err(priv->dev, "failed to update charger: %d\n",
				ret);
		changed = true;
	}

	/*
	 * Downstream s2mu005_det_bat_work(): stop charging when the battery
	 * is removed.
	 */
	if (!ret) {
		int present;

		ret = s2mu005_get_battery_present(priv, &present);
		if (!ret && !present && priv->charging_enabled) {
			dev_warn(priv->dev, "battery removed, disabling charger\n");
			ret = s2mu005_enable_charger_switch(priv, false);
			if (ret)
				dev_err(priv->dev,
					"failed to disable the charger: %d\n",
					ret);
			changed = true;
		}
	}

	mutex_unlock(&priv->lock);

	if (!changed)
		goto notify;

	if (priv->suspended) {
		priv->need_notify = true;
		goto notify;
	}

	s2mu005_muic_report(priv, cable);

notify:
	power_supply_changed(priv->psy);
}

static void s2mu005_host_work(struct work_struct *work)
{
	struct s2mu005_chg *priv = container_of(work, struct s2mu005_chg,
						host_work);
	bool host;
	int ret;

	host = extcon_get_state(priv->edev, EXTCON_USB_HOST) > 0;

	mutex_lock(&priv->lock);
	ret = s2mu005_set_otg(priv, host);
	mutex_unlock(&priv->lock);

	if (ret)
		dev_err(priv->dev, "failed to %s OTG boost: %d\n",
			host ? "enable" : "disable", ret);
}

static int s2mu005_host_evt(struct notifier_block *nb, unsigned long event,
			    void *ptr)
{
	struct s2mu005_chg *priv = container_of(nb, struct s2mu005_chg,
						host_nb);

	schedule_work(&priv->host_work);

	return NOTIFY_OK;
}

static irqreturn_t s2mu005_irq_thread(int irq, void *data)
{
	struct s2mu005_chg *priv = data;
	unsigned int sc_int, fled_int, muic_int1, muic_int2;
	int ret;

	/* Reading the status registers clears the latched interrupts. */
	ret = regmap_read(priv->regmap, S2MU005_REG_SC_INT, &sc_int);
	if (ret)
		return IRQ_NONE;
	ret = regmap_read(priv->regmap, S2MU005_REG_FLED_INT, &fled_int);
	if (ret)
		return IRQ_NONE;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_INT1, &muic_int1);
	if (ret)
		return IRQ_NONE;
	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_INT2, &muic_int2);
	if (ret)
		return IRQ_NONE;

	/* VMID interrupts are reported through the charger interrupt. */
	if (sc_int & S2MU005_INT_VMID_MASK)
		muic_int2 |= BIT(0);

	/*
	 * EVT0 accessory detection workaround, from the downstream MFD
	 * interrupt thread: tune the ID detection threshold in the hidden
	 * 0x89 register when VBUS is present with an undefined accessory.
	 */
	if (priv->rev == 0) {
		unsigned int adc, apple;

		if (!regmap_read(priv->regmap, S2MU005_REG_MUIC_ADC, &adc) &&
		    !regmap_read(priv->regmap, S2MU005_REG_MUIC_DEVICE_APPLE,
				 &apple)) {
			adc &= 0x1f;
			if ((apple & S2MU005_DEV_TYPE_APPLE_VBUS) &&
			    adc != S2MU005_ADC_JIG_USB_OFF &&
			    adc != S2MU005_ADC_JIG_USB_ON &&
			    adc != S2MU005_ADC_JIG_UART_OFF &&
			    adc != S2MU005_ADC_JIG_UART_ON)
				regmap_update_bits(priv->regmap, 0x89,
						   GENMASK(1, 0), 0x01);
		}

		if (muic_int2 & BIT(7))
			regmap_update_bits(priv->regmap, 0x89,
					   GENMASK(1, 0), 0x03);
	}

	if (sc_int || muic_int1 || muic_int2)
		schedule_work(&priv->detect_work);

	return IRQ_HANDLED;
}

static void s2mu005_read_limits(struct s2mu005_chg *priv)
{
	struct device *dev = priv->dev;

	/*
	 * Downstream s2mu005_charger_initialize() starts with a 100 mA
	 * termination current; the per-cable values are applied on attach.
	 */
	priv->term_current_ua = 100 * 1000;

	/* battery,chg_float_voltage = <4340> on this board. */
	priv->float_voltage_uv = 4340 * 1000;
	device_property_read_u32(dev, "constant-charge-voltage-max-microvolt",
				 &priv->float_voltage_uv);

	priv->eoc_dualpath = device_property_read_bool(dev,
						       "samsung,eoc-dualpath");
	priv->always_enable = device_property_read_bool(dev,
							"samsung,always-enable");
	priv->wakeup = device_property_read_bool(dev, "wakeup-source");
}

static int s2mu005_chg_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = {};
	struct s2mu005_chg *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (client->irq <= 0)
		return dev_err_probe(dev, -ENODEV, "no interrupt configured\n");

	priv->dev = dev;
	priv->irq = client->irq;
	priv->charge_behaviour = POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
	priv->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	i2c_set_clientdata(client, priv);

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	priv->regmap = devm_regmap_init_i2c(client, &s2mu005_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to initialise regmap\n");

	s2mu005_read_limits(priv);

	ret = s2mu005_charger_early_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to detect the revision\n");

	if (priv->rev == 0)
		dev_info(dev, "S2MU005 EVT0 detected\n");

	ret = s2mu005_muic_hw_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise the MUIC\n");

	ret = s2mu005_muic_irq_masks_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise interrupts\n");

	ret = s2mu005_charger_hw_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise the charger\n");

	psy_cfg.drv_data = priv;
	psy_cfg.fwnode = dev_fwnode(dev);
	priv->psy = devm_power_supply_register(dev, &s2mu005_chg_desc,
					       &psy_cfg);
	if (IS_ERR(priv->psy))
		return dev_err_probe(dev, PTR_ERR(priv->psy),
				     "failed to register the charger supply\n");

	priv->edev = devm_extcon_dev_allocate(dev, s2mu005_extcon_cables);
	if (IS_ERR(priv->edev))
		return dev_err_probe(dev, PTR_ERR(priv->edev),
				     "failed to allocate extcon device\n");

	ret = devm_extcon_dev_register(dev, priv->edev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register extcon\n");

	priv->host_nb.notifier_call = s2mu005_host_evt;
	ret = devm_extcon_register_notifier(dev, priv->edev, EXTCON_USB_HOST,
					    &priv->host_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register the host notifier\n");

	INIT_WORK(&priv->detect_work, s2mu005_detect_work);
	INIT_WORK(&priv->host_work, s2mu005_host_work);

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					s2mu005_irq_thread, IRQF_ONESHOT,
					dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request the IRQ\n");

	device_init_wakeup(dev, priv->wakeup);

	priv->fled_pdev = platform_device_register_data(dev, "s2mu005-fled",
							-1, NULL, 0);
	if (IS_ERR(priv->fled_pdev))
		return dev_err_probe(dev, PTR_ERR(priv->fled_pdev),
				     "failed to create flash LED device\n");

	/* Initial cable detection, as done by the downstream MUIC probe. */
	s2mu005_detect_work(&priv->detect_work);

	dev_info(dev, "S2MU005 charger/MUIC driver loaded, revision %u\n",
		 priv->rev);

	return 0;
}

static void s2mu005_chg_remove(struct i2c_client *client)
{
	struct s2mu005_chg *priv = i2c_get_clientdata(client);

	platform_device_unregister(priv->fled_pdev);
	cancel_work_sync(&priv->detect_work);
	cancel_work_sync(&priv->host_work);
}

/*
 * Downstream s2mu005_charger_shutdown(): make sure charging is enabled in
 * off mode so that the bootloader can charge the battery, and leave the
 * manual reset / I2C reset controls to the bootloader.
 */
static void s2mu005_chg_shutdown(struct i2c_client *client)
{
	struct s2mu005_chg *priv = i2c_get_clientdata(client);

	/*
	 * Downstream s2mu005_muic_shutdown(): open D+/D- and hand the switch
	 * back to the automatic mode so that off-mode charging works.
	 */
	s2mu005_muic_set_manual_sw(priv, S2MU005_MUIC_SW_OPEN);
	regmap_update_bits(priv->regmap, S2MU005_REG_MUIC_CTRL1, BIT(2), BIT(2));

	regmap_update_bits(priv->regmap, S2MU005_REG_MRSTB, BIT(3), 0);
	regmap_update_bits(priv->regmap, S2MU005_REG_I2C_RESET, BIT(0), 0);

	if (!priv->always_enable)
		s2mu005_enable_charger_switch(priv, true);
}

static int s2mu005_chg_suspend(struct device *dev)
{
	struct s2mu005_chg *priv = dev_get_drvdata(dev);
	unsigned int val;
	int ret = 0;

	mutex_lock(&priv->lock);
	priv->suspended = true;

	/*
	 * Downstream s2mu005_charger_suspend(): for pre-EVT2 silicon, slow
	 * down the fuel-gauge clock while discharging so that the gauge keeps
	 * running in low power mode.
	 */
	if (priv->rev < 2 && !priv->charging_enabled) {
		ret = regmap_read(priv->regmap, S2MU005_REG_FG_CTRL1, &val);
		if (ret)
			goto out;

		ret = regmap_write(priv->regmap, S2MU005_REG_FG_CTRL1,
				   val | BIT(7));
		if (ret)
			goto out;

		val = min(priv->fg_clock + 64, 0xffU);
		ret = regmap_write(priv->regmap, S2MU005_REG_FG_CLOCK, val);
	}
out:
	mutex_unlock(&priv->lock);

	if (!ret && priv->wakeup)
		enable_irq_wake(priv->irq);

	return ret;
}

static int s2mu005_chg_resume(struct device *dev)
{
	struct s2mu005_chg *priv = dev_get_drvdata(dev);
	unsigned int val;
	int ret = 0;

	if (priv->wakeup)
		disable_irq_wake(priv->irq);

	mutex_lock(&priv->lock);
	priv->suspended = false;

	if (priv->rev < 2 && !priv->charging_enabled) {
		ret = regmap_read(priv->regmap, S2MU005_REG_FG_CTRL1, &val);
		if (ret)
			goto out;

		ret = regmap_write(priv->regmap, S2MU005_REG_FG_CTRL1,
				   val & ~BIT(7));
		if (ret)
			goto out;

		ret = regmap_write(priv->regmap, S2MU005_REG_FG_CLOCK,
				   priv->fg_clock);
	}
out:
	mutex_unlock(&priv->lock);

	/* Re-check the accessory state: registers may have been lost. */
	schedule_work(&priv->detect_work);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(s2mu005_chg_pm_ops, s2mu005_chg_suspend,
				s2mu005_chg_resume);

static const struct of_device_id s2mu005_chg_of_match[] = {
	{ .compatible = "samsung,s2mu005-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mu005_chg_of_match);

static const struct i2c_device_id s2mu005_chg_id[] = {
	{ "s2mu005-charger", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s2mu005_chg_id);

static struct i2c_driver s2mu005_chg_driver = {
	.driver = {
		.name = S2MU005_CHG_PSY_NAME,
		.of_match_table = s2mu005_chg_of_match,
		.pm = pm_sleep_ptr(&s2mu005_chg_pm_ops),
	},
	.probe = s2mu005_chg_probe,
	.remove = s2mu005_chg_remove,
	.shutdown = s2mu005_chg_shutdown,
	.id_table = s2mu005_chg_id,
};
module_i2c_driver(s2mu005_chg_driver);

MODULE_DESCRIPTION("Samsung S2MU005 charger and MUIC driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
