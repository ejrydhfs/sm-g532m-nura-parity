// SPDX-License-Identifier: GPL-2.0
/*
 * Battery Fuel Gauge Driver for Samsung S2MU005 PMIC.
 *
 * Copyright (C) 2015 Samsung Electronics
 * Copyright (C) 2023 Yassine Oudjana <y.oudjana@protonmail.com>
 * Copyright (C) 2025 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/units.h>

#define S2MU005_FG_REG_STATUS		0x00
#define S2MU005_FG_REG_IRQ		0x02
#define S2MU005_FG_REG_RVBAT		0x04
#define S2MU005_FG_REG_RCURCC		0x06
#define S2MU005_FG_REG_RSOC		0x08
#define S2MU005_FG_REG_MONOUT		0x0a
#define S2MU005_FG_REG_MONOUTSEL	0x0c
#define S2MU005_FG_REG_RBATCAP		0x0e
#define S2MU005_FG_REG_RZADJ		0x12
#define S2MU005_FG_REG_RBATZ0		0x16
#define S2MU005_FG_REG_RBATZ1		0x18
#define S2MU005_FG_REG_IRQLVL		0x1a
#define S2MU005_FG_REG_START		0x1e

/* Byte-wide registers, programmed through a separate 8-bit regmap */
#define S2MU005_FG_REG_POR_STATE	0x1f
#define S2MU005_FG_REG_TOPOFF_CFG	0x27
#define S2MU005_FG_REG_CHG_WA		0x41
#define S2MU005_FG_REG_CURRENT_SENSE1	0x44
#define S2MU005_FG_REG_CURRENT_SENSE2	0x45
#define S2MU005_FG_REG_REVISION		0x48

/*
 * S2MU005_FG_REG_CHG_WA values, from the downstream POWER_SUPPLY_PROP_
 * ENERGY_AVG handler: 0x10 "charger start", 0x04 "charger end".
 */
#define S2MU005_FG_CHG_WA_START		0x10
#define S2MU005_FG_CHG_WA_END		0x04

#define S2MU005_FG_MONOUTSEL_AVGCURRENT		0x26
#define S2MU005_FG_MONOUTSEL_AVGVOLTAGE		0x27
#define S2MU005_FG_MONOUTSEL_TEMPERATURE	0x10

#define S2MU005_FG_PSY_NAME		"s2mu005-fuel-gauge"
#define S2MU005_CHG_PSY_NAME		"s2mu005-charger"

struct s2mu005_fg {
	struct device *dev;
	struct regmap *regmap;
	struct regmap *regmap8;
	struct power_supply *psy;
	struct mutex monout_mutex;
	struct notifier_block chg_nb;
	unsigned int revision;
	bool wakeup;
	bool chg_online;
	bool bat_present;
};

static const struct regmap_config s2mu005_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static const struct regmap_config s2mu005_fg_regmap8_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static irqreturn_t s2mu005_handle_irq(int irq, void *data)
{
	struct s2mu005_fg *priv = data;

	msleep(100);
	power_supply_changed(priv->psy);

	return IRQ_HANDLED;
}

static int s2mu005_fg_get_voltage_now(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int ret;

	ret = regmap_read(regmap, S2MU005_FG_REG_RVBAT, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read voltage register (%d)\n", ret);
		return ret;
	}

	*value = (val * MICRO) >> 13;

	return 0;
}

static int s2mu005_fg_get_voltage_avg(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int ret;

	mutex_lock(&priv->monout_mutex);

	ret = regmap_write(regmap, S2MU005_FG_REG_MONOUTSEL,
			   S2MU005_FG_MONOUTSEL_AVGVOLTAGE);
	if (ret < 0) {
		dev_err(priv->dev, "failed to enable average voltage monitoring (%d)\n",
			ret);
		goto unlock;
	}

	ret = regmap_read(regmap, S2MU005_FG_REG_MONOUT, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read current register (%d)\n", ret);
		goto unlock;
	}

	*value = (val * MICRO) >> 12;

unlock:
	mutex_unlock(&priv->monout_mutex);

	return ret;
}
static int s2mu005_fg_get_current_now(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int ret;

	ret = regmap_read(regmap, S2MU005_FG_REG_RCURCC, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read current register (%d)\n", ret);
		return ret;
	}

	*value = -((s16)val * MICRO) >> 12;

	return 0;
}

static int s2mu005_fg_get_current_avg(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int ret;

	mutex_lock(&priv->monout_mutex);

	ret = regmap_write(regmap, S2MU005_FG_REG_MONOUTSEL,
			   S2MU005_FG_MONOUTSEL_AVGCURRENT);
	if (ret < 0) {
		dev_err(priv->dev, "failed to enable average current monitoring (%d)\n",
			ret);
		goto unlock;
	}

	ret = regmap_read(regmap, S2MU005_FG_REG_MONOUT, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read current register (%d)\n", ret);
		goto unlock;
	}

	*value = -((s16)val * MICRO) >> 12;

unlock:
	mutex_unlock(&priv->monout_mutex);

	return ret;
}

/*
 * Battery temperature, as reported by the fuel gauge monitor output.
 * Downstream s2mu005_get_temperature(): the value is a 16-bit two's
 * complement number in 1/256 degrees Celsius; POWER_SUPPLY_PROP_TEMP is
 * reported in tenths of a degree.
 */
static int s2mu005_fg_get_temperature(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int temperature;
	int ret;

	mutex_lock(&priv->monout_mutex);

	ret = regmap_write(regmap, S2MU005_FG_REG_MONOUTSEL,
			   S2MU005_FG_MONOUTSEL_TEMPERATURE);
	if (ret < 0) {
		dev_err(priv->dev, "failed to select the temperature monitor (%d)\n",
			ret);
		goto unlock;
	}

	ret = regmap_read(regmap, S2MU005_FG_REG_MONOUT, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read the temperature (%d)\n", ret);
		goto unlock;
	}

	temperature = (s16)val;
	*value = ((temperature * 100) >> 8) / 10;

unlock:
	mutex_unlock(&priv->monout_mutex);

	return ret;
}

static int s2mu005_fg_get_capacity(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u32 val;
	int ret;

	ret = regmap_read(regmap, S2MU005_FG_REG_RSOC, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read capacity register (%d)\n", ret);
		return ret;
	}

	*value = (val * CENTI) >> 14;

	return 0;
}

static int s2mu005_fg_get_status(struct s2mu005_fg *priv, int *value)
{
	int current_now, current_avg, capacity;
	int ret;

	ret = s2mu005_fg_get_current_now(priv, &current_now);
	if (ret < 0)
		return ret;

	ret = s2mu005_fg_get_current_avg(priv, &current_avg);
	if (ret < 0)
		return ret;

	/*
	 * Verify both current values reported to reduce inaccuracies due to
	 * internal hysteresis.
	 */
	if (current_now < 0 && current_avg < 0) {
		*value = POWER_SUPPLY_STATUS_DISCHARGING;
	} else if (current_now == 0) {
		*value = POWER_SUPPLY_STATUS_NOT_CHARGING;
	} else {
		*value = POWER_SUPPLY_STATUS_CHARGING;

		ret = s2mu005_fg_get_capacity(priv, &capacity);
		if (!ret && capacity > 98)
			*value = POWER_SUPPLY_STATUS_FULL;
		return ret;
	}

	return 0;
}

/*
 * Downstream s2mu005_init_regs(): reduce the top-off current difference
 * between power-on and power-off charging, and set the current-sense
 * sampling time to 500 ms on pre-EVT2 silicon.
 */
static int s2mu005_fg_hw_init(struct s2mu005_fg *priv)
{
	int ret;

	ret = regmap_update_bits(priv->regmap8, S2MU005_FG_REG_TOPOFF_CFG,
				 BIT(4), BIT(4));
	if (ret)
		return ret;

	if (priv->revision < 2)
		ret = regmap_update_bits(priv->regmap8,
					 S2MU005_FG_REG_CURRENT_SENSE2,
					 GENMASK(7, 6), 0);

	return ret;
}

/*
 * Cross-driver workarounds that the downstream battery stack applies through
 * the fuel gauge whenever the charger attachment state changes:
 *
 *  - POWER_SUPPLY_PROP_ENERGY_AVG (s2mu005_fuelgauge.c): write 0x10 on
 *    charger start and 0x04 on charger end ("WA for power off issue"),
 *    requested by the charger driver from its POWER_SUPPLY_PROP_ONLINE
 *    setter (s2mu005_charger.c).
 *  - POWER_SUPPLY_PROP_CHARGE_EMPTY (s2mu005_fuelgauge.c): write 0x01 to
 *    0x1F when the battery is removed ("WA for battery 0 percent"),
 *    requested from s2mu005_det_bat_work().
 */
static void s2mu005_fg_sync_charger(struct s2mu005_fg *priv, bool force)
{
	struct power_supply *chg;
	union power_supply_propval val;
	bool online = false, present = true;

	chg = power_supply_get_by_name(S2MU005_CHG_PSY_NAME);
	if (!chg)
		return;

	if (!power_supply_get_property(chg, POWER_SUPPLY_PROP_ONLINE, &val))
		online = !!val.intval;
	if (!power_supply_get_property(chg, POWER_SUPPLY_PROP_PRESENT, &val))
		present = !!val.intval;
	power_supply_put(chg);

	if (force || online != priv->chg_online) {
		priv->chg_online = online;
		regmap_write(priv->regmap8, S2MU005_FG_REG_CHG_WA,
			     online ? S2MU005_FG_CHG_WA_START :
				     S2MU005_FG_CHG_WA_END);
	}

	if (present) {
		priv->bat_present = true;
	} else if (priv->bat_present) {
		priv->bat_present = false;
		regmap_write(priv->regmap8, S2MU005_FG_REG_POR_STATE, 0x01);
	}
}

static int s2mu005_fg_charger_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct s2mu005_fg *priv = container_of(nb, struct s2mu005_fg, chg_nb);
	struct power_supply *psy = ptr;

	if (!(event & PSY_EVENT_PROP_CHANGED))
		return NOTIFY_OK;
	if (strcmp(psy->desc->name, S2MU005_CHG_PSY_NAME))
		return NOTIFY_OK;

	s2mu005_fg_sync_charger(priv, false);

	return NOTIFY_OK;
}

static const enum power_supply_property s2mu005_fg_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
};

static int s2mu005_fg_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct s2mu005_fg *priv = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return s2mu005_fg_get_voltage_now(priv, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		return s2mu005_fg_get_voltage_avg(priv, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return s2mu005_fg_get_current_now(priv, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		return s2mu005_fg_get_current_avg(priv, &val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		return s2mu005_fg_get_capacity(priv, &val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		return s2mu005_fg_get_status(priv, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		return s2mu005_fg_get_temperature(priv, &val->intval);
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc s2mu005_fg_desc = {
	.name = S2MU005_FG_PSY_NAME,
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = s2mu005_fg_properties,
	.num_properties = ARRAY_SIZE(s2mu005_fg_properties),
	.get_property = s2mu005_fg_get_property,
};

static int s2mu005_fg_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s2mu005_fg *priv;
	struct power_supply_config psy_cfg = {};
	const struct power_supply_desc *psy_desc;
	unsigned int val;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);
	priv->dev = dev;

	priv->regmap = devm_regmap_init_i2c(client, &s2mu005_fg_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to initialize regmap\n");

	priv->regmap8 = devm_regmap_init_i2c(client, &s2mu005_fg_regmap8_config);
	if (IS_ERR(priv->regmap8))
		return dev_err_probe(dev, PTR_ERR(priv->regmap8),
				     "failed to initialize byte regmap\n");

	ret = regmap_read(priv->regmap8, S2MU005_FG_REG_REVISION, &val);
	if (ret) {
		/* Do not apply the pre-EVT2 workarounds when in doubt. */
		dev_warn(dev, "failed to read the revision (%d)\n", ret);
		priv->revision = 2;
	} else {
		priv->revision = (val & 0xf0) >> 4;
	}
	priv->wakeup = device_property_read_bool(dev, "wakeup-source");

	ret = s2mu005_fg_hw_init(priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize the fuel gauge\n");

	psy_desc = device_get_match_data(dev);

	psy_cfg.drv_data = priv;
	psy_cfg.fwnode = dev_fwnode(dev);
	priv->psy = devm_power_supply_register(priv->dev, psy_desc, &psy_cfg);
	if (IS_ERR(priv->psy))
		return dev_err_probe(dev, PTR_ERR(priv->psy),
				     "failed to register power supply subsystem\n");

	ret = devm_mutex_init(dev, &priv->monout_mutex);
	if (ret)
		dev_err_probe(dev, ret, "failed to initialize MONOUT mutex\n");

	ret = devm_request_threaded_irq(priv->dev, client->irq, NULL,
					s2mu005_handle_irq, IRQF_ONESHOT,
					psy_desc->name, priv);
	if (ret)
		dev_err_probe(dev, ret, "failed to request IRQ\n");

	device_init_wakeup(dev, priv->wakeup);
	if (priv->wakeup) {
		ret = enable_irq_wake(client->irq);
		if (ret)
			dev_err_probe(dev, ret, "failed to enable the wake IRQ\n");
	}

	priv->bat_present = true;
	priv->chg_nb.notifier_call = s2mu005_fg_charger_event;
	ret = power_supply_reg_notifier(&priv->chg_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register the charger notifier\n");
	s2mu005_fg_sync_charger(priv, true);

	dev_info(dev, "S2MU005 fuel gauge revision %u\n", priv->revision);

	return 0;
}

static void s2mu005_fg_i2c_remove(struct i2c_client *client)
{
	struct s2mu005_fg *priv = i2c_get_clientdata(client);

	power_supply_unreg_notifier(&priv->chg_nb);
}

/*
 * Downstream s2mu005_fuelgauge_shutdown(): stop the current sensing block on
 * pre-EVT2 silicon before power-off.
 */
static void s2mu005_fg_i2c_shutdown(struct i2c_client *client)
{
	struct s2mu005_fg *priv = i2c_get_clientdata(client);

	if (priv->revision >= 2)
		return;

	regmap_update_bits(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE2,
			   GENMASK(3, 0), 0);
	regmap_write(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE1, 0x00);
}

/*
 * Downstream s2mu005_fuelgauge_suspend()/resume(): on pre-EVT2 silicon the
 * current-sense block is parked in a low power configuration while the
 * device is discharging.  The vendor battery stack only did this when its
 * "current mode" was active, which is the fuel gauge's normal mode; the
 * low-voltage mode is a battery policy that does not exist here.
 */
static int s2mu005_fg_get_charger_online(struct s2mu005_fg *priv, bool *online)
{
	struct power_supply *chg;
	union power_supply_propval val;
	int ret;

	*online = false;

	chg = power_supply_get_by_name(S2MU005_CHG_PSY_NAME);
	if (!chg)
		return 0;

	ret = power_supply_get_property(chg, POWER_SUPPLY_PROP_ONLINE, &val);
	power_supply_put(chg);
	if (ret)
		return ret;

	*online = !!val.intval;

	return 0;
}

static int s2mu005_fg_suspend(struct device *dev)
{
	struct s2mu005_fg *priv = dev_get_drvdata(dev);
	unsigned int val;
	bool online;
	int ret;

	if (priv->revision >= 2)
		return 0;

	ret = s2mu005_fg_get_charger_online(priv, &online);
	if (ret || online)
		return 0;

	ret = regmap_read(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE2, &val);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE2,
			   (val & 0xf0) | 0x06);
	if (ret)
		return ret;

	return regmap_write(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE1, 0xbd);
}

static int s2mu005_fg_resume(struct device *dev)
{
	struct s2mu005_fg *priv = dev_get_drvdata(dev);
	unsigned int val;
	bool online;
	int ret;

	if (priv->revision >= 2)
		return 0;

	ret = s2mu005_fg_get_charger_online(priv, &online);
	if (ret || online)
		return 0;

	ret = regmap_read(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE2, &val);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE2,
			   (val & 0xf0) | 0x07);
	if (ret)
		return ret;

	return regmap_write(priv->regmap8, S2MU005_FG_REG_CURRENT_SENSE1, 0xcc);
}

static DEFINE_SIMPLE_DEV_PM_OPS(s2mu005_fg_pm_ops, s2mu005_fg_suspend,
				s2mu005_fg_resume);

static const struct of_device_id s2mu005_fg_of_match_table[] = {
	{
		.compatible = "samsung,s2mu005-fuel-gauge",
		.data = &s2mu005_fg_desc,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, s2mu005_fg_of_match_table);

static struct i2c_driver s2mu005_fg_i2c_driver = {
	.probe = s2mu005_fg_i2c_probe,
	.remove = s2mu005_fg_i2c_remove,
	.shutdown = s2mu005_fg_i2c_shutdown,
	.driver = {
		.name = S2MU005_FG_PSY_NAME,
		.of_match_table = s2mu005_fg_of_match_table,
		.pm = pm_sleep_ptr(&s2mu005_fg_pm_ops),
	},
};
module_i2c_driver(s2mu005_fg_i2c_driver);

MODULE_DESCRIPTION("Samsung S2MU005 PMIC Battery Fuel Gauge Driver");
MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_LICENSE("GPL");
