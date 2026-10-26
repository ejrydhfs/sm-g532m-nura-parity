// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S2MU005 flash/torch LED driver
 *
 * Ported from the Samsung/MTK downstream driver:
 *   drivers/leds/leds_s2mu005.c
 *   Copyright (C) 2015 Samsung Electronics
 *
 * The S2MU005 charger/MUIC driver creates a "s2mu005-fled" platform
 * device.  This driver uses the parent regmap and exposes the rear
 * flash/torch channel through the LED flash class.
 */

#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* S2MU005 register numbers, from downstream s2mu005-private.h */
#define S2MU005_REG_FLED_CH1_CTRL0	46
#define S2MU005_REG_FLED_CH1_CTRL1	47
#define S2MU005_REG_FLED_CH1_CTRL3	49
#define S2MU005_REG_FLED_CTRL1		55
#define S2MU005_REG_FLED_CTRL2		56
#define S2MU005_REG_FLED_RSVD		60

#define S2MU005_FLASH_IOUT_MASK		0x1f
#define S2MU005_TORCH_IOUT_MASK		0x0f
#define S2MU005_EN_CHANNEL_SHARE_MASK	0x80
#define S2MU005_BOOST_VOUT_FLASH_MASK	0x1f
#define S2MU005_FLASH_BOOST_EN_MASK	0x40
#define S2MU005_TIMEOUT_MAX		0xf

#define S2MU005_FLASH_ON_I2C		0xc0
#define S2MU005_TORCH_ON_I2C		0x30

#define S2MU005_FLASH_BRIGHTNESS(mA) \
	((mA) <= 400 ? ((((mA) - 25) / 25) & 0x1f) : \
	 (((((mA) - 400) / 50) + 0x0f) & 0x1f))
#define S2MU005_TORCH_BRIGHTNESS(mA) ((((mA) - 25) / 25) & 0x0f)

struct s2mu005_fled {
	struct regmap *regmap;
	struct led_classdev_flash flash;
	struct mutex lock;
};

static int s2mu005_fled_flash_brightness_set(struct led_classdev_flash *fled,
					     u32 brightness_ua)
{
	struct s2mu005_fled *led = container_of(fled, struct s2mu005_fled,
						flash);
	unsigned int ma = brightness_ua / 1000;
	unsigned int val;

	if (ma < 25 || ma > 1200)
		return -EINVAL;

	val = S2MU005_FLASH_BRIGHTNESS(ma);
	return regmap_update_bits(led->regmap, S2MU005_REG_FLED_CH1_CTRL0,
				  S2MU005_FLASH_IOUT_MASK, val);
}

static int s2mu005_fled_strobe_set(struct led_classdev_flash *fled, bool on)
{
	struct s2mu005_fled *led = container_of(fled, struct s2mu005_fled,
						flash);
	unsigned int val = on ? S2MU005_FLASH_ON_I2C : 0;
	int ret;

	mutex_lock(&led->lock);
	ret = regmap_update_bits(led->regmap, S2MU005_REG_FLED_RSVD,
				 S2MU005_FLASH_ON_I2C, val);
	mutex_unlock(&led->lock);

	return ret;
}

static int s2mu005_fled_strobe_get(struct led_classdev_flash *fled, bool *on)
{
	struct s2mu005_fled *led = container_of(fled, struct s2mu005_fled,
						flash);
	unsigned int val;
	int ret;

	ret = regmap_read(led->regmap, S2MU005_REG_FLED_RSVD, &val);
	if (ret)
		return ret;

	*on = !!(val & S2MU005_FLASH_ON_I2C);
	return 0;
}

static int s2mu005_fled_timeout_set(struct led_classdev_flash *fled,
				    u32 timeout_us)
{
	struct s2mu005_fled *led = container_of(fled, struct s2mu005_fled,
						flash);
	unsigned int steps = DIV_ROUND_CLOSEST(timeout_us, 62000);

	if (steps > S2MU005_TIMEOUT_MAX)
		steps = S2MU005_TIMEOUT_MAX;

	return regmap_update_bits(led->regmap, S2MU005_REG_FLED_CH1_CTRL3,
				  S2MU005_TIMEOUT_MAX, steps);
}

static int s2mu005_fled_torch_brightness_set(struct led_classdev *lcdev,
					     enum led_brightness level)
{
	struct led_classdev_flash *fled = lcdev_to_flcdev(lcdev);
	struct s2mu005_fled *led = container_of(fled, struct s2mu005_fled,
						flash);
	unsigned int val = min_t(unsigned int, level,
				 S2MU005_TORCH_IOUT_MASK);
	int ret;

	mutex_lock(&led->lock);
	ret = regmap_update_bits(led->regmap, S2MU005_REG_FLED_CH1_CTRL1,
				 S2MU005_TORCH_IOUT_MASK, val);
	if (!ret)
		ret = regmap_update_bits(led->regmap, S2MU005_REG_FLED_RSVD,
					 S2MU005_TORCH_ON_I2C,
					 val ? S2MU005_TORCH_ON_I2C : 0);
	mutex_unlock(&led->lock);

	return ret;
}

static const struct led_flash_ops s2mu005_fled_ops = {
	.flash_brightness_set = s2mu005_fled_flash_brightness_set,
	.strobe_set = s2mu005_fled_strobe_set,
	.strobe_get = s2mu005_fled_strobe_get,
	.timeout_set = s2mu005_fled_timeout_set,
};

static int s2mu005_fled_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2mu005_fled *led;
	struct regmap *regmap;
	int ret;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV,
				     "parent S2MU005 regmap not available\n");

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->regmap = regmap;
	mutex_init(&led->lock);

	/* Downstream s2mu005_led_setup(): independent channels, 4.5 V boost */
	ret = regmap_update_bits(regmap, S2MU005_REG_FLED_CTRL2,
				 S2MU005_EN_CHANNEL_SHARE_MASK, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, S2MU005_REG_FLED_CTRL2,
				 S2MU005_BOOST_VOUT_FLASH_MASK, 0x0a);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, S2MU005_REG_FLED_CTRL1,
				 S2MU005_FLASH_BOOST_EN_MASK,
				 S2MU005_FLASH_BOOST_EN_MASK);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, S2MU005_REG_FLED_CH1_CTRL3, BIT(7),
				 BIT(7));
	if (ret)
		return ret;

	ret = regmap_write(regmap, S2MU005_REG_FLED_CH1_CTRL0, 0);
	if (ret)
		return ret;
	ret = regmap_write(regmap, S2MU005_REG_FLED_CH1_CTRL1, 0);
	if (ret)
		return ret;
	ret = regmap_write(regmap, S2MU005_REG_FLED_RSVD, 0);
	if (ret)
		return ret;

	led->flash.led_cdev.name = "s2mu005:flash";
	led->flash.led_cdev.max_brightness = S2MU005_TORCH_IOUT_MASK;
	led->flash.led_cdev.brightness_set_blocking =
		s2mu005_fled_torch_brightness_set;
	led->flash.ops = &s2mu005_fled_ops;

	led->flash.brightness.min = 25000;
	led->flash.brightness.max = 1200000;
	led->flash.brightness.step = 25000;
	led->flash.brightness.val = 150000;

	led->flash.timeout.min = 62000;
	led->flash.timeout.max = 992000;
	led->flash.timeout.step = 62000;
	led->flash.timeout.val = 992000;

	led->flash.duration.min = 0;
	led->flash.duration.max = 0;
	led->flash.duration.step = 0;
	led->flash.duration.val = 0;

	platform_set_drvdata(pdev, led);

	return devm_led_classdev_flash_register(dev, &led->flash);
}

static struct platform_driver s2mu005_fled_driver = {
	.probe = s2mu005_fled_probe,
	.driver = {
		.name = "s2mu005-fled",
	},
};
module_platform_driver(s2mu005_fled_driver);

MODULE_DESCRIPTION("Samsung S2MU005 flash/torch LED");
MODULE_LICENSE("GPL");
