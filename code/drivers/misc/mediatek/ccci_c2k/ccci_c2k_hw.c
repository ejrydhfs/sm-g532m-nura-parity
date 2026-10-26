// SPDX-License-Identifier: GPL-2.0
/*
 * C2K (MD3) hardware sequencing for the MT6735/MT6737T CCCI subsystem.
 *
 * Port from Samsung/MTK downstream (SM-G532M, MT6737T, kernel 3.18):
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.c
 *   drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.h
 *   drivers/misc/mediatek/eccci/mt6735/modem_reg_base.h
 *   drivers/misc/mediatek/c2k_ccci/mt6735/c2k_platform.c
 *   drivers/watchdog/mediatek/wdt/mt6735/mtk_wdt.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 The Nura project
 *
 * The register sequences below reproduce md_ccif_power_on()/power_off() and
 * md_ccif_let_md_go() of the MD_SYS3 (C2K) path one to one; only the kernel
 * API (ioremap, clk, regulator, polling helpers) is current.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "ccci_c2k.h"

/* Wake-up handshake timeout; downstream busy-waits forever (c2k_status poll). */
#define C2K_WAKE_POLL_US	100
#define C2K_WAKE_TIMEOUT_US	1000000

#define C2K_REG_BOOT_VECTOR	"boot-vector"
#define C2K_REG_BOOT_VECTOR_KEY	"boot-vector-key"
#define C2K_REG_BOOT_VECTOR_EN	"boot-vector-en"
#define C2K_REG_MD_RGU		"md-rgu"
#define C2K_REG_CHIP_ID		"chip-id"
#define C2K_REG_MD1_PCCIF	"md1-pccif"
#define C2K_REG_MD3_PCCIF	"md3-pccif"

/**
 * c2k_map_resource() - map one named MMIO resource of the C2K node
 * @dev: C2K platform device
 * @name: resource name in the device tree
 *
 * Returns the mapping, NULL if the resource is absent, or an ERR_PTR().
 */
static void __iomem *c2k_map_resource(struct device *dev, const char *name)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return NULL;

	return devm_ioremap_resource(dev, res);
}

/**
 * c2k_map_default() - map a resource, falling back to a downstream address
 * @dev: C2K platform device
 * @name: resource name in the device tree
 * @phys: downstream physical address (see ccci_c2k.h for the citation)
 * @size: window size in bytes
 *
 * The MD3 boot slave and RGU windows are fixed SoC registers that the
 * downstream kernel never described in the device tree; they are only mapped
 * from the built-in address when the board does not provide them.
 */
static void __iomem *c2k_map_default(struct device *dev, const char *name,
				     phys_addr_t phys, resource_size_t size)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (res)
		return devm_ioremap_resource(dev, res);

	base = devm_ioremap(dev, phys, size);
	if (!base)
		return ERR_PTR(-ENOMEM);

	dev_info(dev, "%s: no DT resource, using 0x%pa\n", name, &phys);
	return base;
}

/**
 * c2k_phys() - physical address of a resource, or a downstream fallback
 * @dev: C2K platform device
 * @name: resource name in the device tree
 * @fallback: downstream address used when the board does not describe it
 */
static phys_addr_t c2k_phys(struct device *dev, const char *name,
			    phys_addr_t fallback)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	return res ? res->start : fallback;
}

/* devres action for windows taken with of_iomap() */
static void c2k_iounmap_action(void *addr)
{
	iounmap(addr);
}

/**
 * c2k_map_syscon() - map the first reg window of a phandle referenced node
 * @dev: C2K platform device
 * @propname: property holding the phandle
 *
 * Covers the three shared blocks the downstream code grabs with
 * of_find_compatible_node(): INFRA_AO (C2K config/status/SPM control),
 * SLEEP (clock gating) and TOP_RGU (C2K sysreset).
 */
static void __iomem *c2k_map_syscon(struct device *dev, const char *propname)
{
	struct device_node *np;
	void __iomem *base;

	np = of_parse_phandle(dev->of_node, propname, 0);
	if (!np)
		return NULL;

	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base)
		return ERR_PTR(-ENOMEM);

	if (devm_add_action_or_reset(dev, c2k_iounmap_action, base))
		return ERR_PTR(-ENOMEM);

	return base;
}

/* Read-modify-write helper for the C2K control registers. */
static void c2k_rmw(void __iomem *base, unsigned int reg, u32 mask, u32 val)
{
	u32 tmp = readl(base + reg);

	tmp = (tmp & ~mask) | (val & mask);
	writel(tmp, base + reg);
}

/*
 * C2K sysreset, equivalent of mtk_wdt_set_c2k_sysrst() in the downstream
 * watchdog driver (mtk_wdt.c:472-504).  Bit 15 of TOP_RGU_WDT_SWSYSRST holds
 * the C2K domain in reset; every write must carry the 0x88000000 key.
 */
static void c2k_sysrst_set(struct ccci_c2k *c2k, bool asserted)
{
	u32 val;

	if (!c2k->toprgu)
		return;

	val = readl(c2k->toprgu + C2K_TOP_RGU_WDT_SWSYSRST);
	if (asserted)
		val |= BIT(C2K_RGU_C2K_SYSRST_BIT);
	else
		val &= ~BIT(C2K_RGU_C2K_SYSRST_BIT);
	writel(val | C2K_RGU_SWSYS_RST_KEY,
	       c2k->toprgu + C2K_TOP_RGU_WDT_SWSYSRST);
}

/*
 * Wake the C2K subsystem: pulse INFRA_AO_C2K_SPM_CTRL[0] and wait for
 * INFRA_AO_C2K_STATUS[1], then drop the request again.  Downstream
 * ccif_c2k_platform.c:326-345.
 */
static int c2k_wake(struct ccci_c2k *c2k)
{
	u32 val;
	int ret;

	if (!c2k->infra_ao)
		return 0;

	c2k_rmw(c2k->infra_ao, C2K_INFRA_AO_C2K_SPM_CTRL,
		C2K_SPM_WAKE_BIT, C2K_SPM_WAKE_BIT);

	ret = readl_poll_timeout(c2k->infra_ao + C2K_INFRA_AO_C2K_STATUS, val,
				 val & C2K_STATUS_AWAKE_BIT, C2K_WAKE_POLL_US,
				 C2K_WAKE_TIMEOUT_US);
	if (ret) {
		dev_err(c2k->dev, "[C2K] wake timeout, C2K_STATUS=0x%x\n", val);
		return ret;
	}

	c2k_rmw(c2k->infra_ao, C2K_INFRA_AO_C2K_SPM_CTRL,
		C2K_SPM_WAKE_BIT, 0);
	return 0;
}

/**
 * ccci_c2k_hw_read_chip_id() - read the C2K chip id register
 * @c2k: C2K instance
 *
 * Downstream reads this register through c2k_chip_id_base (ROM DTS node
 * mdc2k@3a00b01c) and expects 0x020AC000; the wait loop around it is compiled
 * out downstream (c2k_platform.c:536-543), so the value is only read and
 * reported.
 */
unsigned int ccci_c2k_hw_read_chip_id(struct ccci_c2k *c2k)
{
	if (!c2k->chip_id)
		return 0;

	c2k->chip_id_val = readl(c2k->chip_id);
	c2k->chip_id_valid = c2k->chip_id_val == C2K_CHIP_ID_EXPECTED;
	return c2k->chip_id_val;
}

/**
 * ccci_c2k_hw_init() - map the C2K hardware and acquire its supplies
 * @c2k: C2K instance
 *
 * Everything is devres managed, so there is no teardown counterpart.
 */
int ccci_c2k_hw_init(struct ccci_c2k *c2k)
{
	struct device *dev = c2k->dev;
	int ret;

	c2k->md1_pccif = c2k_map_resource(dev, C2K_REG_MD1_PCCIF);
	if (IS_ERR(c2k->md1_pccif))
		return PTR_ERR(c2k->md1_pccif);

	c2k->md3_pccif = c2k_map_resource(dev, C2K_REG_MD3_PCCIF);
	if (IS_ERR(c2k->md3_pccif))
		return PTR_ERR(c2k->md3_pccif);

	c2k->chip_id = c2k_map_resource(dev, C2K_REG_CHIP_ID);
	if (IS_ERR(c2k->chip_id))
		return PTR_ERR(c2k->chip_id);

	c2k->boot_vector = c2k_map_default(dev, C2K_REG_BOOT_VECTOR,
					   C2K_MD_BOOT_VECTOR_PHYS, 4);
	if (IS_ERR(c2k->boot_vector))
		return PTR_ERR(c2k->boot_vector);

	c2k->boot_vector_key = c2k_map_default(dev, C2K_REG_BOOT_VECTOR_KEY,
					       C2K_MD_BOOT_VECTOR_KEY_PHYS, 4);
	if (IS_ERR(c2k->boot_vector_key))
		return PTR_ERR(c2k->boot_vector_key);

	c2k->boot_vector_en = c2k_map_default(dev, C2K_REG_BOOT_VECTOR_EN,
					      C2K_MD_BOOT_VECTOR_EN_PHYS, 4);
	if (IS_ERR(c2k->boot_vector_en))
		return PTR_ERR(c2k->boot_vector_en);

	c2k->md_rgu = c2k_map_default(dev, C2K_REG_MD_RGU, C2K_MD_RGU_PHYS,
				      0x40);
	if (IS_ERR(c2k->md_rgu))
		return PTR_ERR(c2k->md_rgu);

	c2k->boot_vector_phys = c2k_phys(dev, C2K_REG_BOOT_VECTOR,
					 C2K_MD_BOOT_VECTOR_PHYS);
	c2k->boot_vector_key_phys = c2k_phys(dev, C2K_REG_BOOT_VECTOR_KEY,
					     C2K_MD_BOOT_VECTOR_KEY_PHYS);
	c2k->boot_vector_en_phys = c2k_phys(dev, C2K_REG_BOOT_VECTOR_EN,
					    C2K_MD_BOOT_VECTOR_EN_PHYS);
	c2k->md_rgu_phys = c2k_phys(dev, C2K_REG_MD_RGU, C2K_MD_RGU_PHYS);

	c2k->infra_ao = c2k_map_syscon(dev, "mediatek,infracfg");
	if (IS_ERR(c2k->infra_ao))
		return PTR_ERR(c2k->infra_ao);
	if (!c2k->infra_ao)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,infracfg phandle\n");

	c2k->sleep = c2k_map_syscon(dev, "mediatek,spm");
	if (IS_ERR(c2k->sleep))
		return PTR_ERR(c2k->sleep);
	if (!c2k->sleep)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,spm phandle\n");

	c2k->toprgu = c2k_map_syscon(dev, "mediatek,toprgu");
	if (IS_ERR(c2k->toprgu))
		return PTR_ERR(c2k->toprgu);
	if (!c2k->toprgu)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,toprgu phandle\n");

	c2k->clk_md_main = devm_clk_get_optional(dev, "scp-sys-md2-main");
	if (IS_ERR(c2k->clk_md_main))
		return PTR_ERR(c2k->clk_md_main);
	if (!c2k->clk_md_main)
		dev_warn(dev,
			 "no scp-sys-md2-main clock, relying on power domain\n");

	/*
	 * On mainline the SCP_SYS_MD2 gate is the MT6735 MD2 power domain;
	 * the platform bus attaches it from the DT power-domains phandle and
	 * powers it on before probe.  Runtime PM is used to keep it on while
	 * MD3 runs (same pattern as the MD1 core in ccci_platform.c:208).
	 */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	c2k->vtcxo1 = devm_regulator_get_optional(dev, "vtcxo1");
	if (IS_ERR(c2k->vtcxo1))
		return PTR_ERR(c2k->vtcxo1);
	if (!c2k->vtcxo1)
		dev_warn(dev,
			 "no vtcxo1 supply, C2K VTCXO must be enabled elsewhere\n");

	c2k->ets_sel = of_property_read_bool(dev->of_node, "mediatek,ets-sel");

	ccci_c2k_hw_read_chip_id(c2k);
	dev_info(dev, "chip id 0x%08x%s\n", c2k->chip_id_val,
		 c2k->chip_id_valid ? "" : " (unexpected)");
	return 0;
}

/**
 * ccci_c2k_hw_power_on() - release C2K clocks and VTCXO
 * @c2k: C2K instance
 *
 * Downstream md_ccif_power_on() MD_SYS3 branch (ccif_c2k_platform.c:376-389)
 * enables "scp-sys-md2-main"; the VTCXO_1 PMIC write of the same bring-up
 * (step 3, PMIC register 0x0A02 == MT6328_VTCXO_1_CON0) is done through the
 * regulator framework here.
 */
int ccci_c2k_hw_power_on(struct ccci_c2k *c2k)
{
	int ret;

	if (c2k->powered)
		return 0;

	if (c2k->clk_md_main) {
		ret = clk_prepare_enable(c2k->clk_md_main);
		if (ret) {
			dev_err(c2k->dev, "scp-sys-md2-main enable failed %d\n",
				ret);
			return ret;
		}
		c2k->clk_enabled = true;
	}

	/* SCP_SYS_MD2 gate modelled as a power domain (MT6735_POWER_DOMAIN_MD2) */
	ret = pm_runtime_resume_and_get(c2k->dev);
	if (ret) {
		dev_err(c2k->dev, "MD2 power domain resume failed %d\n", ret);
		if (c2k->clk_enabled) {
			clk_disable_unprepare(c2k->clk_md_main);
			c2k->clk_enabled = false;
		}
		return ret;
	}

	if (c2k->vtcxo1) {
		ret = regulator_enable(c2k->vtcxo1);
		if (ret) {
			dev_err(c2k->dev, "vtcxo1 enable failed %d\n", ret);
			pm_runtime_put_sync(c2k->dev);
			if (c2k->clk_enabled) {
				clk_disable_unprepare(c2k->clk_md_main);
				c2k->clk_enabled = false;
			}
			return ret;
		}
		c2k->vtcxo_enabled = true;
	}

	c2k->powered = true;
	dev_info(c2k->dev, "[C2K] powered on\n");
	return 0;
}

/**
 * ccci_c2k_hw_power_off() - gate C2K clocks and VTCXO again
 * @c2k: C2K instance
 *
 * Downstream md_ccif_power_off() MD_SYS3 branch (ccif_c2k_platform.c:412-418).
 */
int ccci_c2k_hw_power_off(struct ccci_c2k *c2k)
{
	if (!c2k->powered)
		return 0;

	if (c2k->vtcxo_enabled) {
		regulator_disable(c2k->vtcxo1);
		c2k->vtcxo_enabled = false;
	}

	pm_runtime_put_sync(c2k->dev);

	if (c2k->clk_enabled) {
		clk_disable_unprepare(c2k->clk_md_main);
		c2k->clk_enabled = false;
	}

	c2k->powered = false;
	dev_info(c2k->dev, "[C2K] powered off\n");
	return 0;
}

/**
 * ccci_c2k_hw_let_md_go() - start MD3
 * @c2k: C2K instance
 *
 * Equivalent of md_ccif_let_md_go() for MD_SYS3 (ccif_c2k_platform.c:241-355):
 * program the boot slave, put C2K in its boot mode, select the sleep clock
 * sources, release the C2K sysreset and complete the wake handshake.  Must run
 * after ccci_c2k_hw_power_on() and after the firmware has been staged.
 */
int ccci_c2k_hw_let_md_go(struct ccci_c2k *c2k)
{
	u32 val;
	int ret;

	if (!c2k->powered)
		return -EPERM;

	/* Boot slave: key, vector, enable (modem_reg_base.h:52-54). */
	writel(C2K_MD_BOOT_VECTOR_KEY_VALUE, c2k->boot_vector_key);
	writel(C2K_MD_BOOT_VECTOR_VALUE, c2k->boot_vector);
	writel(C2K_MD_BOOT_VECTOR_EN_VALUE, c2k->boot_vector_en);

	/*
	 * Step 1: boot mode.  ETS select first when the board boots in
	 * meta/factory mode (ccif_c2k_platform.c:261-268), then field [10:8]=5
	 * (ccif_c2k_platform.c:269-275).
	 */
	if (c2k->ets_sel)
		c2k_rmw(c2k->infra_ao, C2K_INFRA_AO_C2K_CONFIG,
			C2K_ETS_SEL_BIT, C2K_ETS_SEL_BIT);
	c2k_rmw(c2k->infra_ao, C2K_INFRA_AO_C2K_CONFIG,
		C2K_CONFIG_BOOTMODE_MASK,
		C2K_CONFIG_BOOTMODE_C2K << 8);

	/* Step 2: srcclkena selection mask (ccif_c2k_platform.c:279-288). */
	c2k_rmw(c2k->infra_ao, C2K_INFRA_AO_C2K_SPM_CTRL,
		C2K_SPM_SRCCLKENA_MASK, C2K_SPM_SRCCLKENA_C2K << 4);
	val = readl(c2k->infra_ao + C2K_INFRA_AO_C2K_SPM_CTRL);

	/* Sleep clock configuration (ccif_c2k_platform.c:292-303). */
	c2k_rmw(c2k->sleep, C2K_SLEEP_CLK_CON, C2K_SLEEP_CLK_CON_SET,
		C2K_SLEEP_CLK_CON_SET);
	c2k_rmw(c2k->sleep, C2K_SLEEP_CLK_CON, C2K_SLEEP_CLK_CON_CLR, 0);

	/*
	 * Step 4: release the C2K domain reset.  The PMIC VTCXO_1 write that
	 * downstream performs as step 3 is done in ccci_c2k_hw_power_on().
	 */
	c2k_sysrst_set(c2k, false);

	/* Step 6: wake up C2K and wait for the hardware handshake. */
	ret = c2k_wake(c2k);
	if (ret)
		return ret;

	dev_info(c2k->dev,
		 "[C2K] C2K_CONFIG=0x%x C2K_SPM_CTRL=0x%x C2K_STATUS=0x%x SLEEP_CLK_CON=0x%x SWSYSRST=0x%x\n",
		 readl(c2k->infra_ao + C2K_INFRA_AO_C2K_CONFIG), val,
		 readl(c2k->infra_ao + C2K_INFRA_AO_C2K_STATUS),
		 readl(c2k->sleep + C2K_SLEEP_CLK_CON),
		 readl(c2k->toprgu + C2K_TOP_RGU_WDT_SWSYSRST));

	ccci_c2k_hw_read_chip_id(c2k);
	return 0;
}

/**
 * ccci_c2k_hw_reset() - assert and release the C2K domain reset
 * @c2k: C2K instance
 *
 * Exception recovery path, equivalent of c2k_modem_reset_platform()
 * (c2k_ccci/mt6735/c2k_platform.c:599-650): hold C2K in reset, release it and
 * redo the wake handshake.
 */
void ccci_c2k_hw_reset(struct ccci_c2k *c2k)
{
	if (!c2k->powered)
		return;

	dev_info(c2k->dev, "[C2K] reset\n");
	c2k_sysrst_set(c2k, true);
	c2k_sysrst_set(c2k, false);

	if (c2k_wake(c2k))
		dev_err(c2k->dev, "[C2K] reset wake handshake failed\n");
}

/**
 * ccci_c2k_hw_format_regs() - format the C2K register dump
 * @c2k: C2K instance
 * @buf: destination buffer
 * @size: buffer size
 */
int ccci_c2k_hw_format_regs(struct ccci_c2k *c2k, char *buf, size_t size)
{
	int len = 0;
	int n;

	n = scnprintf(buf + len, size - len,
		      "ap_ccif: CON=%08x BUSY=%08x START=%08x TCHNUM=%08x RCHNUM=%08x ACK=%08x\n",
		      readl(c2k->ccif + C2K_APCCIF_CON),
		      readl(c2k->ccif + C2K_APCCIF_BUSY),
		      readl(c2k->ccif + C2K_APCCIF_START),
		      readl(c2k->ccif + C2K_APCCIF_TCHNUM),
		      readl(c2k->ccif + C2K_APCCIF_RCHNUM),
		      readl(c2k->ccif + C2K_APCCIF_ACK));
	len += n;
	if (len >= size)
		return len;

	n = scnprintf(buf + len, size - len,
		      "infra_ao: C2K_CONFIG=%08x C2K_STATUS=%08x C2K_SPM_CTRL=%08x\n",
		      readl(c2k->infra_ao + C2K_INFRA_AO_C2K_CONFIG),
		      readl(c2k->infra_ao + C2K_INFRA_AO_C2K_STATUS),
		      readl(c2k->infra_ao + C2K_INFRA_AO_C2K_SPM_CTRL));
	len += n;
	if (len >= size)
		return len;

	n = scnprintf(buf + len, size - len,
		      "sleep: SLEEP_CLK_CON=%08x toprgu: SWSYSRST=%08x\n",
		      readl(c2k->sleep + C2K_SLEEP_CLK_CON),
		      readl(c2k->toprgu + C2K_TOP_RGU_WDT_SWSYSRST));
	len += n;
	if (len >= size)
		return len;

	n = scnprintf(buf + len, size - len,
		      "md_rgu: WDT_MODE=%08x WDT_STA=%08x\n",
		      readl(c2k->md_rgu + C2K_MD_WDT_MODE),
		      readl(c2k->md_rgu + C2K_MD_WDT_STA));
	len += n;
	if (len >= size)
		return len;

	if (c2k->md1_pccif && c2k->md3_pccif) {
		n = scnprintf(buf + len, size - len,
			      "pccif: md1_BUSY=%08x md3_BUSY=%08x\n",
			      readl(c2k->md1_pccif + C2K_APCCIF_BUSY),
			      readl(c2k->md3_pccif + C2K_APCCIF_BUSY));
		len += n;
		if (len >= size)
			return len;
	}

	if (c2k->chip_id) {
		n = scnprintf(buf + len, size - len,
			      "chip_id=%08x%s\n", c2k->chip_id_val,
			      c2k->chip_id_valid ? "" : " (unexpected)");
		len += n;
	}

	return len;
}

/**
 * ccci_c2k_hw_format_wdt() - format the C2K watchdog/reset status
 * @c2k: C2K instance
 * @buf: destination buffer
 * @size: buffer size
 */
int ccci_c2k_hw_format_wdt(struct ccci_c2k *c2k, char *buf, size_t size)
{
	u32 sysrst = readl(c2k->toprgu + C2K_TOP_RGU_WDT_SWSYSRST);
	u32 status = readl(c2k->infra_ao + C2K_INFRA_AO_C2K_STATUS);
	u32 spm = readl(c2k->infra_ao + C2K_INFRA_AO_C2K_SPM_CTRL);
	int len = 0;

	len += scnprintf(buf + len, size - len, "c2k_sysrst=%s\n",
			 sysrst & BIT(C2K_RGU_C2K_SYSRST_BIT) ? "asserted" :
								 "released");
	len += scnprintf(buf + len, size - len, "top_rgu_swsysrst=0x%08x\n",
			 sysrst);
	len += scnprintf(buf + len, size - len, "c2k_status=0x%08x awake=%u\n",
			 status, !!(status & C2K_STATUS_AWAKE_BIT));
	len += scnprintf(buf + len, size - len, "c2k_spm_ctrl=0x%08x wake=%u\n",
			 spm, !!(spm & C2K_SPM_WAKE_BIT));

	return len;
}
