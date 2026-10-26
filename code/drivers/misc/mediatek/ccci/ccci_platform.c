// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI platform driver for the MT6735/MT6737T mdcldma block.
 *
 * DT node (downstream arch/arm64/boot/dts/mt6735.dtsi, node
 * "mdcldma@1000A000"): reg order AP_CLDMA_AO, MD_CLDMA_AO, AP_CLDMA_PDN,
 * MD_CLDMA_PDN, AP_CCIF, MD_CCIF; interrupts CLDMA, CCIF, MDWDT.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/mt6735/cldma_platform.c and
 * drivers/misc/mediatek/eccci/modem_cldma.c (probe/remove/PM).
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#include <linux/ccci.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ccci_priv.h"

#define CCCI_MD1_RES_NAME	"md1"

/* Downstream mt6735/modem_reg_base.h: MD1 boot slave + RGU. */
#define CCCI_MD1_BOOT_VECTOR	0x20190000
#define CCCI_MD1_BOOT_VECTOR_KEY 0x2019379C
#define CCCI_MD1_BOOT_VECTOR_EN	0x20195488
#define CCCI_MD1_RGU_BASE	0x20050000

struct ccci_plat {
	struct ccci_md *md;
	struct ccci_ccif *ccif;
	struct ccci_cldma *cldma;
	struct clk *clk;
	void __iomem *cldma_ao;
	void __iomem *cldma_pdn;
	void __iomem *ccif_ap;
	unsigned int cldma_irq;
	unsigned int md_wdt_irq;
};

static int ccci_plat_parse_dt(struct platform_device *pdev,
			      struct ccci_md_cfg *cfg, phys_addr_t *rgu,
			      phys_addr_t *bv, phys_addr_t *bv_key,
			      phys_addr_t *bv_en)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	const char *fw;
	u32 val;
	int nreg = 0, ret;

	ret = of_property_read_u32(np, "mediatek,md_id", &val);
	if (ret)
		val = 0;
	if (val >= CCCI_MAX_MD_NUM)
		return -EINVAL;

	cfg->md_id = val;
	cfg->name = CCCI_MD1_RES_NAME;
	cfg->irat_md_id = CCCI_MAX_MD_NUM;

	if (!of_property_read_string(np, "firmware-name", &fw))
		cfg->firmware_name = fw;
	else
		cfg->firmware_name = "md1img";

	if (!of_property_read_u32(np, "mediatek,cldma_capability", &val))
		cfg->capability = val;

	/* boot slave / RGU addresses: DT overrides, otherwise the fixed MT6735
	 * values from downstream mt6735/modem_reg_base.h
	 */
	*bv = CCCI_MD1_BOOT_VECTOR;
	*bv_key = CCCI_MD1_BOOT_VECTOR_KEY;
	*bv_en = CCCI_MD1_BOOT_VECTOR_EN;
	*rgu = CCCI_MD1_RGU_BASE;
	of_property_read_u32(np, "mediatek,md-boot-vector", &val);
	if (val)
		*bv = val;
	of_property_read_u32(np, "mediatek,md-boot-vector-key", &val);
	if (val)
		*bv_key = val;
	of_property_read_u32(np, "mediatek,md-boot-vector-en", &val);
	if (val)
		*bv_en = val;
	if (!of_property_read_u32(np, "mediatek,rgu-base", &val) && val)
		*rgu = val;

	while (platform_get_resource(pdev, IORESOURCE_MEM, nreg))
		nreg++;

	if (nreg < 2)
		return -ENODEV;

	res = platform_get_resource(pdev, IORESOURCE_MEM, nreg >= 6 ? 4 : 0);
	cfg->ccif_phys = res->start;
	cfg->ccif_size = resource_size(res);

	cfg->md_boot_vector_phys = *bv;
	cfg->md_boot_vector_key_phys = *bv_key;
	cfg->md_boot_vector_en_phys = *bv_en;
	cfg->rgu_phys = *rgu;
	cfg->sram_size = CCIF_SRAM_SIZE;

	return 0;
}

static int ccci_plat_hw_get(struct platform_device *pdev, struct ccci_plat *pl,
			    struct ccci_md_cfg *cfg, bool *has_cldma)
{
	int nreg = 0, irq;

	while (platform_get_resource(pdev, IORESOURCE_MEM, nreg))
		nreg++;

	*has_cldma = nreg >= 6;
	if (*has_cldma) {
		pl->cldma_ao = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(pl->cldma_ao))
			return PTR_ERR(pl->cldma_ao);
		/* MD side registers are mapped by the CLDMA engine owner; keep
		 * only AP_CLDMA_PDN here for suspend/resume book-keeping.
		 */
		pl->cldma_pdn = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(pl->cldma_pdn))
			return PTR_ERR(pl->cldma_pdn);
		pl->ccif_ap = devm_platform_ioremap_resource(pdev, 4);
		if (IS_ERR(pl->ccif_ap))
			return PTR_ERR(pl->ccif_ap);

		pl->cldma_irq = platform_get_irq(pdev, 0);
		cfg->ccif_irq = platform_get_irq(pdev, 1);
	} else {
		pl->ccif_ap = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(pl->ccif_ap))
			return PTR_ERR(pl->ccif_ap);
		cfg->ccif_irq = platform_get_irq(pdev, 0);
	}
	irq = platform_get_irq_optional(pdev, *has_cldma ? 2 : 1);
	pl->md_wdt_irq = irq > 0 ? irq : 0;

	if ((int)cfg->ccif_irq < 0) {
		dev_err(&pdev->dev, "no CCIF interrupt in DT\n");
		return -ENODEV;
	}
	return 0;
}

static const struct of_device_id ccci_mdcldma_of_match[] = {
	{ .compatible = "mediatek,mdcldma" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ccci_mdcldma_of_match);

static int ccci_mdcldma_probe(struct platform_device *pdev)
{
	struct ccci_plat *pl;
	struct ccci_md_cfg cfg = {};
	struct ccci_cldma_cfg ccfg = {};
	phys_addr_t rgu = 0, bv = 0, bv_key = 0, bv_en = 0;
	struct device *dev = &pdev->dev;
	void __iomem *md_ccif;
	bool has_cldma = false;
	int ret;

	if (!dev->of_node) {
		dev_err(dev, "missing DT node\n");
		return -ENODEV;
	}

	pl = devm_kzalloc(dev, sizeof(*pl), GFP_KERNEL);
	if (!pl)
		return -ENOMEM;

	ret = ccci_plat_parse_dt(pdev, &cfg, &rgu, &bv, &bv_key, &bv_en);
	if (ret) {
		dev_err(dev, "DT parse failed %d\n", ret);
		return ret;
	}

	ret = ccci_plat_hw_get(pdev, pl, &cfg, &has_cldma);
	if (ret) {
		dev_err(dev, "resource mapping failed %d\n", ret);
		return ret;
	}

	/* MD side CCIF view, used for register dumps only */
	md_ccif = devm_platform_ioremap_resource(pdev, has_cldma ? 5 : 1);
	if (IS_ERR(md_ccif))
		md_ccif = NULL;

	pl->clk = devm_clk_get_optional(dev, "scp-sys-md1-main");
	if (IS_ERR(pl->clk)) {
		dev_err(dev, "cannot get scp-sys-md1-main: %ld\n",
			PTR_ERR(pl->clk));
		return PTR_ERR(pl->clk);
	}
	if (!pl->clk)
		dev_warn(dev, "no scp-sys-md1-main clock, relying on power domain\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = ccci_md_register(&cfg, dev);
	if (ret) {
		dev_err(dev, "modem registration failed %d\n", ret);
		return ret;
	}

	pl->md = ccci_md_get(cfg.md_id);
	if (!pl->md) {
		ccci_unregister_modem(cfg.md_id);
		return -ENODEV;
	}
	pl->md->pdev = pdev;
	pl->md->boot.clk = pl->clk;

	pl->ccif = ccci_ccif_create(pl->md, dev, pl->ccif_ap, md_ccif,
				    devm_ioremap(dev, rgu, 0x40),
				    bv, bv_key, bv_en, cfg.ccif_irq,
				    pl->md_wdt_irq, cfg.sram_size, !has_cldma);
	if (IS_ERR(pl->ccif)) {
		ret = PTR_ERR(pl->ccif);
		pl->ccif = NULL;
		dev_err(dev, "CCIF init failed %d\n", ret);
		goto err_unregister;
	}
	ret = ccci_md_attach_ccif(pl->md, pl->ccif);
	if (ret)
		goto err_ccif;

	if (has_cldma) {
		ccfg.dev = dev;
		ccfg.ap_base = pl->cldma_ao;
		ccfg.ap_pdn_base = pl->cldma_pdn;
		ccfg.irq = pl->cldma_irq;
		ccfg.smem_phys = pl->md->smem_phy;
		ccfg.smem_size = pl->md->smem_size;
		pl->cldma = ccci_cldma_create(&ccfg);
		if (!pl->cldma) {
			dev_err(dev, "CLDMA engine create failed\n");
			ret = -ENOMEM;
			goto err_ccif;
		}
		ccci_cldma_set_rx_cb(pl->cldma, ccci_md_rx_cb, pl->md);
		/* Gap closure: INFRA_AO window for the RGU CLDMA reset. */
		ccci_cldma_set_infra_ao(pl->cldma, pl->md->infra_ao);
		ccci_md_attach_cldma(pl->md, pl->cldma);
	}

	platform_set_drvdata(pdev, pl);
	dev_info(dev, "md%u probed (%s data path, fw %s)\n", cfg.md_id + 1,
		 has_cldma ? "CLDMA" : "CCIF-ring", cfg.firmware_name);
	return 0;

err_ccif:
	ccci_ccif_destroy(pl->ccif);
err_unregister:
	ccci_unregister_modem(cfg.md_id);
	return ret;
}

static void ccci_mdcldma_remove(struct platform_device *pdev)
{
	struct ccci_plat *pl = platform_get_drvdata(pdev);
	unsigned int md_id;

	if (!pl || !pl->md)
		return;

	md_id = pl->md->id;
	if (pl->cldma) {
		ccci_cldma_destroy(pl->cldma);
		pl->cldma = NULL;
	}
	if (pl->ccif) {
		ccci_ccif_destroy(pl->ccif);
		pl->ccif = NULL;
	}
	ccci_unregister_modem(md_id);
	platform_set_drvdata(pdev, NULL);
}

static void ccci_mdcldma_shutdown(struct platform_device *pdev)
{
	struct ccci_plat *pl = platform_get_drvdata(pdev);

	if (pl && pl->md)
		ccci_md_stop(pl->md, 0);
}

#ifdef CONFIG_PM_SLEEP
static int ccci_mdcldma_suspend(struct device *dev)
{
	struct ccci_plat *pl = dev_get_drvdata(dev);

	if (!pl || !pl->md)
		return 0;
	/* the modem keeps running; only the AP side is quiesced */
	dev_dbg(dev, "suspend, md state %d\n", pl->md->state);
	return 0;
}

static int ccci_mdcldma_resume(struct device *dev)
{
	struct ccci_plat *pl = dev_get_drvdata(dev);

	if (!pl || !pl->md || !pl->ccif)
		return 0;
	/* downstream md_ccif_resume(): restore CCIF arbitration */
	if (pl->ccif->ap_base)
		writel(0x01, pl->ccif->ap_base + APCCIF_CON);
	return 0;
}

static int ccci_mdcldma_restore_noirq(struct device *dev)
{
	struct ccci_plat *pl = dev_get_drvdata(dev);

	if (!pl || !pl->md)
		return 0;
	/* IPO-H: the modem lost power, force a full reload on next start */
	pl->md->boot.fw_valid = false;
	pl->md->boot_stage = CCCI_BOOT_STAGE_0;
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops ccci_modem_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = ccci_mdcldma_suspend,
	.resume = ccci_mdcldma_resume,
	.freeze = ccci_mdcldma_suspend,
	.thaw = ccci_mdcldma_resume,
	.poweroff = ccci_mdcldma_suspend,
	.restore = ccci_mdcldma_resume,
	.restore_noirq = ccci_mdcldma_restore_noirq,
#endif
};

static struct platform_driver ccci_mdcldma_driver = {
	.probe = ccci_mdcldma_probe,
	.remove = ccci_mdcldma_remove,
	.shutdown = ccci_mdcldma_shutdown,
	.driver = {
		.name = "ccci_mdcldma",
		.of_match_table = ccci_mdcldma_of_match,
		.pm = &ccci_modem_pm_ops,
	},
};

int ccci_platform_register(void)
{
	return platform_driver_register(&ccci_mdcldma_driver);
}

void ccci_platform_unregister(void)
{
	platform_driver_unregister(&ccci_mdcldma_driver);
}
