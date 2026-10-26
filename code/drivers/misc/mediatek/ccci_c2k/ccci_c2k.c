// SPDX-License-Identifier: GPL-2.0
/*
 * C2K (MD3) dual-modem platform driver for the MT6735/MT6737T CCCI subsystem.
 *
 * Port from Samsung/MTK downstream (SM-G532M, MT6737T, kernel 3.18):
 *   drivers/misc/mediatek/eccci/modem_ccif_c2k.c (md_ccif_probe, md_ccif_ops)
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.c
 *     (md_ccif_get_modem_hw_info, MD_SYS3 branch)
 *   drivers/misc/mediatek/eccci/port_cfg.c (md3_ccci_ports)
 *   Documentation/devicetree/bindings/ccci/eccci/mediatek,ap2c2k_ccif.txt
 *
 * On this platform MD3 is a second CCIF modem instance (no SDIO/CBP transport
 * is built); the ROM only exports mtk_wdt_set_c2k_sysrst for "c2k".
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 The Nura project
 */

#include <linux/ccci.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ccci_c2k.h"

#define C2K_DRV_NAME	"ccci_c2k"

/* The ROM device tree describes the MD3 CCIF instance as ap2c2k_ccif. */
#define C2K_OF_COMPAT_ROM	"mediatek,ap2c2k_ccif"
#define C2K_OF_COMPAT_NEW	"mediatek,mt6735-ccif-md3"

/* MD_SYS3 is index 2 in the downstream device tree ("cell-index"). */
#define C2K_MD_INDEX		2

static struct ccci_c2k *c2k_instance;
static DEFINE_MUTEX(c2k_instance_lock);

/**
 * ccci_c2k_get() - return the single MD3 instance
 *
 * The C2K subsystem has one MD3 (the ROM device tree has one ap2c2k_ccif
 * node); hooks and sysfs attributes reach it through this accessor.  Returns
 * NULL before probe and after remove.
 */
struct ccci_c2k *ccci_c2k_get(void)
{
	return READ_ONCE(c2k_instance);
}

static int c2k_check_md_index(struct ccci_c2k *c2k)
{
	struct device *dev = c2k->dev;
	u32 index;

	if (of_property_read_u32(dev->of_node, "cell-index", &index) &&
	    of_property_read_u32(dev->of_node, "mediatek,md_id", &index))
		return 0;

	if (index != C2K_MD_INDEX)
		return dev_err_probe(dev, -EINVAL,
				     "wrong modem index %u, MD3 is %d\n",
				     index, C2K_MD_INDEX);

	return 0;
}

static int c2k_init_cfg(struct ccci_c2k *c2k)
{
	struct device *dev = c2k->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct ccci_md_cfg *cfg = &c2k->cfg;
	struct resource *res;
	u32 capability = 0;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ap-ccif");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENODEV, "no AP CCIF resource\n");

	c2k->ccif = devm_ioremap_resource(dev, res);
	if (IS_ERR(c2k->ccif))
		return PTR_ERR(c2k->ccif);

	/* CCIF interrupt, downstream irq_of_parse_and_map(dev, 0). */
	ret = platform_get_irq_byname_optional(pdev, "ccif");
	if (ret == -ENXIO)
		ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no CCIF interrupt\n");
	cfg->ccif_irq = ret;

	/*
	 * MD3 watchdog interrupt, downstream irq_of_parse_and_map(dev, 1).
	 * It is reported for diagnostics only: struct ccci_md_cfg carries the
	 * CCIF line, and MD exception handling belongs to the core.
	 */
	ret = platform_get_irq_byname_optional(pdev, "md-wdt");
	if (ret == -ENXIO)
		ret = platform_get_irq_optional(pdev, 1);
	c2k->md_wdt_irq = ret < 0 ? 0 : ret;

	/* Capability granted by the board, downstream "ccif,capability". */
	of_property_read_u32(dev->of_node, "ccif,capability", &capability);

	cfg->md_id = CCCI_MD_SYS3;
	cfg->name = "md3";
	cfg->firmware_name = C2K_MD_FIRMWARE_NAME;
	cfg->ccif_phys = res->start;
	cfg->ccif_size = resource_size(res);
	cfg->md_boot_vector_phys = c2k->boot_vector_phys;
	cfg->md_boot_vector_key_phys = c2k->boot_vector_key_phys;
	cfg->md_boot_vector_en_phys = c2k->boot_vector_en_phys;
	cfg->rgu_phys = c2k->md_rgu_phys;
	cfg->sram_size = C2K_CCIF_SRAM_SIZE;
	cfg->capability = C2K_MD_CAPABILITY_DEFAULT | capability;
	cfg->irat_md_id = C2K_IRAT_MD_ID;

	dev_info(dev,
		 "MD3: ccif 0x%pa+0x%llx irq %u wdt irq %u capability 0x%x\n",
		 &cfg->ccif_phys, (unsigned long long)cfg->ccif_size,
		 cfg->ccif_irq, c2k->md_wdt_irq, cfg->capability);
	return 0;
}

/*
 * Modem hardware callbacks, invoked by the CCCI core around its start/stop
 * sequence.  See include/linux/ccci.h and the report for the exact call
 * sites (downstream md_ccif_op_start/power_on/let_md_go).
 */
static struct ccci_c2k *c2k_from_md(unsigned int md_id)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k || md_id != CCCI_MD_SYS3)
		return NULL;

	return c2k;
}

static int c2k_md_power_on(unsigned int md_id)
{
	struct ccci_c2k *c2k = c2k_from_md(md_id);

	if (!c2k)
		return -ENODEV;

	return ccci_c2k_hw_power_on(c2k);
}

static int c2k_md_let_md_go(unsigned int md_id)
{
	struct ccci_c2k *c2k = c2k_from_md(md_id);
	int ret;

	if (!c2k)
		return -ENODEV;

	ret = ccci_c2k_hw_let_md_go(c2k);
	if (ret)
		return ret;

	c2k->boot_count++;
	return 0;
}

static int c2k_md_power_off(unsigned int md_id)
{
	struct ccci_c2k *c2k = c2k_from_md(md_id);

	if (!c2k)
		return -ENODEV;

	return ccci_c2k_hw_power_off(c2k);
}

static void c2k_md_reset(unsigned int md_id)
{
	struct ccci_c2k *c2k = c2k_from_md(md_id);

	if (c2k)
		ccci_c2k_hw_reset(c2k);
}

static const struct ccci_md_hw_ops c2k_md_hw_ops = {
	.power_on = c2k_md_power_on,
	.let_md_go = c2k_md_let_md_go,
	.power_off = c2k_md_power_off,
	.reset = c2k_md_reset,
};

static int ccci_c2k_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ccci_c2k *c2k;
	int ret;

	c2k = devm_kzalloc(dev, sizeof(*c2k), GFP_KERNEL);
	if (!c2k)
		return -ENOMEM;

	c2k->dev = dev;
	mutex_init(&c2k->lock);

	ret = c2k_check_md_index(c2k);
	if (ret)
		return ret;

	ret = ccci_c2k_hw_init(c2k);
	if (ret)
		return ret;

	ret = c2k_init_cfg(c2k);
	if (ret)
		return ret;

	mutex_lock(&c2k_instance_lock);
	if (c2k_instance) {
		mutex_unlock(&c2k_instance_lock);
		return dev_err_probe(dev, -EBUSY, "MD3 already registered\n");
	}
	WRITE_ONCE(c2k_instance, c2k);
	mutex_unlock(&c2k_instance_lock);

	ret = ccci_register_md_hw_ops(CCCI_MD_SYS3, &c2k_md_hw_ops);
	if (ret) {
		dev_err(dev, "register MD3 hardware ops failed %d\n", ret);
		goto err_instance;
	}

	ret = ccci_register_modem(&c2k->cfg);
	if (ret) {
		dev_err(dev, "register MD3 modem failed %d\n", ret);
		goto err_ops;
	}

	ret = ccci_c2k_sysfs_init(c2k);
	if (ret) {
		dev_err(dev, "C2K sysfs init failed %d\n", ret);
		goto err_modem;
	}

	if (c2k->cfg.capability & CCCI_MD_CAP_CCMNI_IRAT) {
		ret = ccci_c2k_set_irat(c2k->cfg.irat_md_id);
		if (ret)
			dev_warn(dev, "IRAT setup incomplete, %d\n", ret);
	}

	platform_set_drvdata(pdev, c2k);
	dev_info(dev, "MD3 C2K modem registered\n");
	return 0;

err_modem:
	ccci_unregister_modem(CCCI_MD_SYS3);
err_ops:
	ccci_unregister_md_hw_ops(CCCI_MD_SYS3);
err_instance:
	mutex_lock(&c2k_instance_lock);
	WRITE_ONCE(c2k_instance, NULL);
	mutex_unlock(&c2k_instance_lock);
	return ret;
}

static void ccci_c2k_remove(struct platform_device *pdev)
{
	struct ccci_c2k *c2k = platform_get_drvdata(pdev);

	if (!c2k)
		return;

	ccci_c2k_sysfs_exit(c2k);
	ccci_unregister_modem(CCCI_MD_SYS3);
	ccci_unregister_md_hw_ops(CCCI_MD_SYS3);
	ccci_c2k_hw_power_off(c2k);

	mutex_lock(&c2k_instance_lock);
	WRITE_ONCE(c2k_instance, NULL);
	mutex_unlock(&c2k_instance_lock);
}

static void ccci_c2k_shutdown(struct platform_device *pdev)
{
	struct ccci_c2k *c2k = platform_get_drvdata(pdev);

	if (!c2k)
		return;

	ccci_c2k_sysfs_exit(c2k);
	ccci_unregister_modem(CCCI_MD_SYS3);
	ccci_unregister_md_hw_ops(CCCI_MD_SYS3);
	ccci_c2k_hw_power_off(c2k);

	mutex_lock(&c2k_instance_lock);
	WRITE_ONCE(c2k_instance, NULL);
	mutex_unlock(&c2k_instance_lock);

	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id ccci_c2k_of_match[] = {
	{ .compatible = C2K_OF_COMPAT_ROM },
	{ .compatible = C2K_OF_COMPAT_NEW },
	{ }
};
MODULE_DEVICE_TABLE(of, ccci_c2k_of_match);

static struct platform_driver ccci_c2k_driver = {
	.probe = ccci_c2k_probe,
	.remove = ccci_c2k_remove,
	.shutdown = ccci_c2k_shutdown,
	.driver = {
		.name = C2K_DRV_NAME,
		.of_match_table = ccci_c2k_of_match,
	},
};
module_platform_driver(ccci_c2k_driver);

MODULE_DESCRIPTION("MediaTek MT6735/MT6737T C2K (MD3) CCCI modem driver");
MODULE_LICENSE("GPL");
