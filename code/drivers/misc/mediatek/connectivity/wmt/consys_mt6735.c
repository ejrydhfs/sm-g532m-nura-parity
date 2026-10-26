// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735/MT6737T CONSYS platform glue.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/mt6735/mtk_wcn_consys_hw.c
 *   drivers/misc/mediatek/connectivity/common/conn_soc/mt6735/wmt_plat_alps.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * The connectivity subsystem is a separate MCU with its own power domain,
 * clock gates and LDOs.  Register layout (see the DT binding of
 * "mediatek,mt6735-consys"):
 *
 *	reg[0] CONN_MCU_CONFIG_BASE	chip id, MBIST control, CPUPCR
 *	reg[1] AP_RGU_BASE		CPU software reset
 *	reg[2] TOPCKGEN_BASE		EMI remapping window
 *	reg[3] SPM_BASE			power-on configuration
 *
 * The SPM power gate and AXI bus protection of the downstream sequence
 * (CONSYS_TOP1_PWR_CTRL_REG / CONSYS_TOPAXI_PROT_*) are handled by the
 * MT6735 power-domain controller declared through "power-domains"; this
 * driver asserts the reset, powers the rails, gates the clocks and polls the
 * chip id exactly like the downstream driver does around those steps.
 */

#include <linux/clk.h>
#include <linux/clkbuf.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "wmt_local.h"

/* mtk_wcn_consys_hw.h:57-86 */
#define CONSYS_CPU_SW_RST_OFFSET	0x00000018
#define CONSYS_CPU_SW_RST_BIT		BIT(12)
#define CONSYS_CPU_SW_RST_CTRL_KEY	(0x88 << 24)

#define CONSYS_PWRON_CONFG_EN_OFFSET	0x00000000
#define CONSYS_PWRON_CONFG_EN_VALUE	0x0b160001

#define CONSYS_EMI_MAPPING_OFFSET	0x00000320
#define CONSYS_CHIP_ID_OFFSET		0x00000008
#define CONSYS_MCU_CFG_ACR_OFFSET	0x00000110
#define CONSYS_MCU_CFG_ACR_MBIST_BIT	BIT(18)
#define CONSYS_CPUPCR_OFFSET		0x00000160

/* mtk_wcn_consys_hw.h:171-181 */
#define CONSYS_EMI_AP_PHY_OFFSET	0x80000
#define CONSYS_EMI_MEM_SIZE		(343 * 1024)
#define CONSYS_EMI_MAPPING_EN		BIT(12)
#define CONSYS_EMI_ADDR_MASK		0xfff00000

/* Accepted CONSYS chip ids (mtk_wcn_consys_hw.c:487-489). */
#define CONSYS_CHIP_ID_MT6735		0x0321
#define CONSYS_CHIP_ID_MT6735M		0x0335
#define CONSYS_CHIP_ID_MT6753		0x0337

/* PMIC LDO voltages, downstream VOL_1800 / VOL_2800 / VOL_3300. */
#define CONSYS_VCN18_UV			1800000
#define CONSYS_VCN28_UV			2800000
#define CONSYS_VCN33_UV			3300000

/* mtk_wcn_consys_hw.c:481-490 (10 retries, 20 ms apart). */
#define CONSYS_CHIPID_RETRY		10
#define CONSYS_CHIPID_DELAY_MS		20

struct mtk_consys {
	struct device *dev;
	void __iomem *mcu;
	void __iomem *ap_rgu;
	void __iomem *topckgen;
	void __iomem *spm;

	struct clk *clk_conn;
	struct clk *clk_bus;		/* optional, ROM DT has a second gate */

	struct regulator *vcn18;
	struct regulator *vcn28;
	struct regulator *vcn33_bt;
	struct regulator *vcn33_wifi;

	struct pinctrl *pinctrl;

	int irq_bgf;
	bool bgf_irq_requested;
	bool bgf_irq_enabled;
	bool powered;
	bool probed;

	/* Resources currently enabled by this driver. */
	bool pm_on;
	bool clk_conn_on;
	bool clk_bus_on;
	bool vcn18_on;
	bool vcn28_on;
	bool clk_buf_on;	/* CONN RF clock buffer, co-clock mode only */

	phys_addr_t emi_phys;
	void __iomem *emi_virt;

	struct mutex wake_lock;
	int wake_count;
};

static struct mtk_consys consys;

static inline u32 consys_read(void __iomem *base, u32 off)
{
	return readl(base + off);
}

static inline void consys_write(void __iomem *base, u32 off, u32 val)
{
	writel(val, base + off);
}

static void consys_read_modify_write(void __iomem *base, u32 off, u32 set,
				     u32 clr)
{
	u32 val = consys_read(base, off);

	val &= ~clr;
	val |= set;
	consys_write(base, off, val);
}

static bool consys_chipid_valid(u32 id)
{
	return id == CONSYS_CHIP_ID_MT6735 || id == CONSYS_CHIP_ID_MT6735M ||
	       id == CONSYS_CHIP_ID_MT6753;
}

/*
 * Regulators (mtk_wcn_consys_hw.c:349-410, 900-1050)
 */

static int consys_regulator_enable(struct regulator *reg, u32 uv,
				   const char *name)
{
	int ret;

	if (!reg)
		return -ENODEV;

	ret = regulator_set_voltage(reg, uv, uv);
	if (ret) {
		wmt_err("failed to set %s to %u uV (%d)\n", name, uv, ret);
		return ret;
	}

	ret = regulator_enable(reg);
	if (ret) {
		wmt_err("failed to enable %s (%d)\n", name, ret);
		return ret;
	}

	return 0;
}

static void consys_regulator_disable(struct regulator *reg, const char *name)
{
	if (reg &&
	    regulator_disable(reg))
		wmt_err("failed to disable %s\n", name);
}

int wmt_plat_soc_paldo_ctrl(enum wmt_paldo_type type, enum wmt_paldo_op op)
{
	bool on = op == PALDO_ON;
	int ret;

	switch (type) {
	case BT_PALDO:
		if (on)
			ret = consys_regulator_enable(consys.vcn33_bt,
						      CONSYS_VCN33_UV,
						      "vcn33_bt");
		else {
			consys_regulator_disable(consys.vcn33_bt, "vcn33_bt");
			ret = 0;
		}
		break;
	case WIFI_PALDO:
		if (on)
			ret = consys_regulator_enable(consys.vcn33_wifi,
						      CONSYS_VCN33_UV,
						      "vcn33_wifi");
		else {
			consys_regulator_disable(consys.vcn33_wifi,
						 "vcn33_wifi");
			ret = 0;
		}
		break;
	case FM_PALDO:
	case GPS_PALDO:
		/* In co-clock mode FM/GPS power VCN28 explicitly
		 * (mtk_wcn_consys_hw.c:1084-1110).
		 */
		if (on)
			ret = consys_regulator_enable(consys.vcn28,
						      CONSYS_VCN28_UV,
						      "vcn28");
		else {
			consys_regulator_disable(consys.vcn28, "vcn28");
			ret = 0;
		}
		break;
	case PMIC_CHIPID_PALDO:
	case WIFI_5G_PALDO:
	default:
		wmt_warn("PALDO %d is not supported\n", type);
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

/*
 * Power on / off (mtk_wcn_consys_hw.c:335-845)
 */

static int consys_reg_ctrl(bool on)
{
	u32 chip_id = 0;
	int retry = CONSYS_CHIPID_RETRY;
	int ret;

	wmt_dbg("CONSYS register control (%d)\n", on);

	if (on) {
		/* 1. VCN18 always on (mtk_wcn_consys_hw.c:356-370) */
		ret = consys_regulator_enable(consys.vcn18, CONSYS_VCN18_UV,
					      "vcn18");
		if (ret)
			return ret;
		consys.vcn18_on = true;
		udelay(150);

		/* 2. VCN28: software controlled in co-clock mode, powered
		 * here otherwise (mtk_wcn_consys_hw.c:372-400).
		 */
		if (!wmt_lib_co_clock()) {
			ret = consys_regulator_enable(consys.vcn28,
						      CONSYS_VCN28_UV,
						      "vcn28");
			if (ret)
				goto err_vcn18;
			consys.vcn28_on = true;
		} else {
			/* Co-clock mode turns on the CONN RF clock buffer and
			 * leaves VCN28 to software control
			 * (mtk_wcn_consys_hw.c:353-358, CONSYS_CLOCK_BUF_CTRL).
			 */
			if (clk_buf_ctrl(CLK_BUF_CONN, 1)) {
				consys.clk_buf_on = true;
			} else {
				wmt_warn("failed to enable the CONN clock buffer\n");
			}
		}

		/* 3. assert the CONNSYS CPU software reset */
		consys_write(consys.ap_rgu, CONSYS_CPU_SW_RST_OFFSET,
			     consys_read(consys.ap_rgu,
					 CONSYS_CPU_SW_RST_OFFSET) |
			     CONSYS_CPU_SW_RST_BIT |
			     CONSYS_CPU_SW_RST_CTRL_KEY);

		/* 4. SPM clock gating for the power-on configuration */
		consys_write(consys.spm, CONSYS_PWRON_CONFG_EN_OFFSET,
			     CONSYS_PWRON_CONFG_EN_VALUE);

		/* 5. power up the CONN power domain */
		ret = pm_runtime_get_sync(consys.dev);
		if (ret < 0) {
			wmt_err("failed to power on the CONN domain (%d)\n",
				ret);
			goto err_vcn28;
		}
		consys.pm_on = true;

		/* 6. CONN bus clock.  The downstream driver waits 10 us
		 * after the domain is powered before enabling it
		 * (mtk_wcn_consys_hw.c:487-501).
		 */
		udelay(10);
		ret = clk_prepare_enable(consys.clk_conn);
		if (ret) {
			wmt_err("failed to enable the conn clock (%d)\n", ret);
			goto err_pm;
		}
		consys.clk_conn_on = true;

		if (consys.clk_bus) {
			ret = clk_prepare_enable(consys.clk_bus);
			if (ret) {
				wmt_err("failed to enable the bus clock (%d)\n",
					ret);
				goto err_clk;
			}
			consys.clk_bus_on = true;
		}

		/* 7. wait until the MCU reports its chip id */
		while (retry-- > 0) {
			chip_id = consys_read(consys.mcu, CONSYS_CHIP_ID_OFFSET);
			if (consys_chipid_valid(chip_id)) {
				wmt_info("CONSYS chip id 0x%08x\n", chip_id);
				break;
			}
			wmt_dbg("CONSYS chip id 0x%08x, retrying\n", chip_id);
			msleep(CONSYS_CHIPID_DELAY_MS);
		}

		if (!consys_chipid_valid(chip_id)) {
			wmt_err("CONSYS did not report a valid chip id (0x%08x)\n",
				chip_id);
			wmt_err("RGU 0x%08x SPM 0x%08x\n",
				consys_read(consys.ap_rgu,
					    CONSYS_CPU_SW_RST_OFFSET),
				consys_read(consys.spm,
					    CONSYS_PWRON_CONFG_EN_OFFSET));
			ret = -ENODEV;
			goto err_bus_clk;
		}

		/* 8. run the memory MBIST at full MCU speed */
		consys_read_modify_write(consys.mcu,
					 CONSYS_MCU_CFG_ACR_OFFSET,
					 CONSYS_MCU_CFG_ACR_MBIST_BIT, 0);

		/* 9. release the CPU software reset */
		consys_write(consys.ap_rgu, CONSYS_CPU_SW_RST_OFFSET,
			     (consys_read(consys.ap_rgu,
					  CONSYS_CPU_SW_RST_OFFSET) &
			      ~CONSYS_CPU_SW_RST_BIT) |
			     CONSYS_CPU_SW_RST_CTRL_KEY);

		/* 10. give the firmware time to boot (msleep < 20 ms can
		 * sleep up to 20 ms, mtk_wcn_consys_hw.c:845).
		 */
		msleep(20);
		return 0;
	}

	/* Power off in the reverse order (mtk_wcn_consys_hw.c:640-720). */
	if (consys.clk_bus_on) {
		clk_disable_unprepare(consys.clk_bus);
		consys.clk_bus_on = false;
	}
	if (consys.clk_conn_on) {
		clk_disable_unprepare(consys.clk_conn);
		consys.clk_conn_on = false;
	}
	if (consys.pm_on) {
		pm_runtime_put_sync(consys.dev);
		consys.pm_on = false;
	}

	if (consys.clk_buf_on) {
		/* Co-clock mode releases the CONN RF clock buffer instead of
		 * powering VCN28 (mtk_wcn_consys_hw.c:729-734).
		 */
		if (!clk_buf_ctrl(CLK_BUF_CONN, 0))
			wmt_warn("failed to disable the CONN clock buffer\n");
		consys.clk_buf_on = false;
	}

	if (consys.vcn28_on) {
		consys_regulator_disable(consys.vcn28, "vcn28");
		consys.vcn28_on = false;
	}
	if (consys.vcn18_on) {
		consys_regulator_disable(consys.vcn18, "vcn18");
		consys.vcn18_on = false;
	}

	return 0;

err_bus_clk:
	if (consys.clk_bus_on) {
		clk_disable_unprepare(consys.clk_bus);
		consys.clk_bus_on = false;
	}
err_clk:
	if (consys.clk_conn_on) {
		clk_disable_unprepare(consys.clk_conn);
		consys.clk_conn_on = false;
	}
err_pm:
	if (consys.pm_on) {
		pm_runtime_put_sync(consys.dev);
		consys.pm_on = false;
	}
err_vcn28:
	/* Undo the co-clock CONN clock buffer enable of power-on step 2. */
	if (consys.clk_buf_on) {
		clk_buf_ctrl(CLK_BUF_CONN, 0);
		consys.clk_buf_on = false;
	}
	if (consys.vcn28_on) {
		consys_regulator_disable(consys.vcn28, "vcn28");
		consys.vcn28_on = false;
	}
err_vcn18:
	if (consys.vcn18_on) {
		consys_regulator_disable(consys.vcn18, "vcn18");
		consys.vcn18_on = false;
	}
	return ret;
}

/*
 * BGF (BTIF wakeup) interrupt, mtk_wcn_consys_hw.c:800-845 and
 * wmt_plat_alps.c:224-270.
 */
static irqreturn_t consys_bgf_irq(int irq, void *data)
{
	/* Downstream wmt_plat_alps.c:225-239 routes the BGF edge to
	 * wmt_lib_ps_irq_cb(), which becomes a PSM HOST_AWAKE handshake
	 * (psm_core.c:1037-1135).  The line is level triggered, so it is
	 * masked here and re-armed by the PSM engine on the next SLEEP.
	 */
	if (consys.bgf_irq_enabled) {
		disable_irq_nosync(irq);
		consys.bgf_irq_enabled = false;
	}

	wmt_lib_ps_irq_cb();
	return IRQ_HANDLED;
}

/* Downstream wmt_plat_eirq_ctrl(PIN_BGF_EINT, ...) (wmt_plat_alps.c:460-520):
 * the STP power-saving engine arms the wakeup line just before the MCU is
 * allowed to sleep and masks it again as soon as the MCU is awake.
 */
int wmt_plat_eirq_ctrl(bool enable)
{
	if (consys.irq_bgf <= 0 || !consys.bgf_irq_requested)
		return -ENODEV;

	if (enable == consys.bgf_irq_enabled)
		return 0;

	if (enable) {
		enable_irq(consys.irq_bgf);
		consys.bgf_irq_enabled = true;
	} else {
		disable_irq_nosync(consys.irq_bgf);
		consys.bgf_irq_enabled = false;
	}

	return 0;
}

static int consys_irq_init(bool on)
{
	int ret;

	if (on) {
		if (consys.irq_bgf > 0 && !consys.bgf_irq_requested) {
			ret = request_irq(consys.irq_bgf, consys_bgf_irq,
					  irq_get_trigger_type(consys.irq_bgf),
					  "BTIF_WAKEUP_IRQ", NULL);
			if (ret) {
				wmt_err("failed to request BGF IRQ %d (%d)\n",
					consys.irq_bgf, ret);
				return ret;
			}
			consys.bgf_irq_requested = true;
		}

		/* The wakeup interrupt starts masked; the PSM engine arms it
		 * from wmt_plat_eirq_ctrl() just before the MCU sleeps.
		 */
		disable_irq_nosync(consys.irq_bgf);
		consys.bgf_irq_enabled = false;
		wmt_dbg("BGF IRQ registered and disabled\n");
		return 0;
	}

	if (consys.bgf_irq_requested) {
		if (consys.bgf_irq_enabled) {
			disable_irq_nosync(consys.irq_bgf);
			consys.bgf_irq_enabled = false;
		}
		free_irq(consys.irq_bgf, NULL);
		consys.bgf_irq_requested = false;
	}

	return 0;
}

int mtk_wcn_consys_hw_pwr_on(void)
{
	int ret;

	if (!consys.probed)
		return -ENODEV;

	if (consys.powered)
		return -EALREADY;

	wmt_info("CONSYS power on\n");
	ret = consys_reg_ctrl(true);
	if (ret) {
		wmt_err("CONSYS register control failed (%d)\n", ret);
		return ret;
	}

	ret = consys_irq_init(true);
	consys.powered = true;
	wmt_info("CONSYS power on done (%d)\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_pwr_on);


int mtk_wcn_consys_hw_pwr_off(void)
{
	if (!consys.probed)
		return -ENODEV;

	if (!consys.powered)
		return -EALREADY;

	wmt_info("CONSYS power off\n");
	consys_irq_init(false);
	consys.powered = false;
	return consys_reg_ctrl(false);
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_pwr_off);


/* mtk_wcn_consys_hw.c:825-845 */
int mtk_wcn_consys_hw_rst(void)
{
	int ret;

	wmt_info("CONSYS hardware reset\n");

	if (!consys.probed)
		return -ENODEV;

	consys_irq_init(false);
	ret = consys_reg_ctrl(false);
	if (ret)
		wmt_err("CONSYS power off during reset failed (%d)\n", ret);

	consys.powered = false;
	ret = consys_reg_ctrl(true);
	if (ret) {
		wmt_err("CONSYS power on during reset failed (%d)\n", ret);
		return ret;
	}

	consys.powered = true;
	ret = consys_irq_init(true);
	wmt_info("CONSYS hardware reset done (%d)\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_rst);


/* mtk_wcn_consys_hw.c:1190-1233 */
phys_addr_t mtk_wcn_consys_emi_phys_get(void)
{
	return consys.emi_phys;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_emi_phys_get);


void __iomem *mtk_wcn_consys_emi_virt_get(void)
{
	return consys.emi_virt;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_emi_virt_get);

/*
 * CONSYS shared-memory control block accessors.
 *
 * Downstream wmt_plat_alps.c:887-1053 (wmt_plat_get_emi_virt_add(),
 * wmt_plat_get_dump_info(), wmt_plat_set_host_dump_state(),
 * wmt_plat_update_host_sync_num()).  Offsets are the EXP_APMEM_CTRL_*
 * values from mt6735/include/mtk_wcn_consys_hw.h:218-233.
 *
 * All accesses are bounds checked against the mapped window: the coredump
 * collector runs on values read back from an asserting firmware, so a
 * corrupt register must not be able to walk outside the reserved region.
 */
u32 wmt_plat_get_dump_info(u32 offset)
{
	if (!consys.emi_virt || offset >= WMT_EMI_MEM_SIZE ||
	    offset + sizeof(u32) > WMT_EMI_MEM_SIZE)
		return 0;

	return consys_read(consys.emi_virt, offset);
}

int wmt_plat_set_host_dump_state(enum wmt_host_dump_state state)
{
	if (!consys.emi_virt)
		return -ENODEV;

	consys_write(consys.emi_virt, WMT_EMI_CTRL_HOST_SYNC_STATE, state);
	return 0;
}

int wmt_plat_update_host_sync_num(void)
{
	u32 num;

	if (!consys.emi_virt)
		return -ENODEV;

	num = consys_read(consys.emi_virt, WMT_EMI_CTRL_HOST_SYNC_NUM);
	consys_write(consys.emi_virt, WMT_EMI_CTRL_HOST_SYNC_NUM, num + 1);
	return 0;
}

void __iomem *wmt_plat_emi_virt(u32 offset)
{
	if (!consys.emi_virt || offset >= WMT_EMI_MEM_SIZE)
		return NULL;

	return consys.emi_virt + offset;
}


/* mtk_wcn_consys_hw.c:1235-1240 */
u32 mtk_wcn_consys_soc_chipid_get(void)
{
	/* Never touch the CONNSYS register window while the power domain is
	 * off: such an access can stall the AP bus.
	 */
	if (!consys.probed || !consys.powered)
		return 0;

	return consys_read(consys.mcu, CONSYS_CHIP_ID_OFFSET) & 0xffff;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_soc_chipid_get);


int mtk_wcn_consys_hw_init(void)
{
	struct reserved_mem *rmem;
	struct device_node *np;
	u32 addr_phy;

	if (!consys.probed) {
		wmt_err("CONSYS device not probed\n");
		return -ENODEV;
	}

	if (consys.emi_virt)
		return 0;

	np = of_parse_phandle(consys.dev->of_node, "memory-region", 0);
	if (!np) {
		wmt_err("no memory-region for CONSYS\n");
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem || !rmem->size) {
		wmt_err("CONSYS reserved memory is invalid\n");
		return -ENODEV;
	}

	consys.emi_phys = rmem->base;
	wmt_info("CONSYS shared memory at 0x%pa (0x%llx bytes)\n",
		 &consys.emi_phys, (unsigned long long)rmem->size);

	/* Enable the CONSYS -> AP EMI remapping window
	 * (mtk_wcn_consys_hw.c:1140-1150).
	 */
	addr_phy = ((u32)consys.emi_phys & CONSYS_EMI_ADDR_MASK) >> 20;
	addr_phy |= CONSYS_EMI_MAPPING_EN;
	consys_write(consys.topckgen, CONSYS_EMI_MAPPING_OFFSET,
		     consys_read(consys.topckgen, CONSYS_EMI_MAPPING_OFFSET) |
		     addr_phy);
	wmt_info("EMI mapping register 0x%08x\n",
		 consys_read(consys.topckgen, CONSYS_EMI_MAPPING_OFFSET));

	/* The firmware uses the upper half of the reserved 512 KiB region. */
	consys.emi_virt = ioremap(consys.emi_phys + CONSYS_EMI_AP_PHY_OFFSET,
				  CONSYS_EMI_MEM_SIZE);
	if (!consys.emi_virt) {
		wmt_err("failed to map the CONSYS shared memory\n");
		return -ENOMEM;
	}

	memset_io(consys.emi_virt, 0, CONSYS_EMI_MEM_SIZE);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_init);


void mtk_wcn_consys_hw_deinit(void)
{
	if (consys.emi_virt) {
		iounmap(consys.emi_virt);
		consys.emi_virt = NULL;
	}

	if (consys.powered)
		mtk_wcn_consys_hw_pwr_off();
}

/*
 * Platform glue
 */
EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_deinit);


int wmt_plat_pwr_ctrl(enum wmt_func_state state)
{
	switch (state) {
	case FUNC_ON:
		return mtk_wcn_consys_hw_pwr_on();
	case FUNC_OFF:
		return mtk_wcn_consys_hw_pwr_off();
	case FUNC_RST:
		return mtk_wcn_consys_hw_rst();
	case FUNC_STAT:
		wmt_info("CONSYS %s, chip id 0x%04x\n",
			 consys.powered ? "on" : "off",
			 mtk_wcn_consys_soc_chipid_get());
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * The GPS_SYNC/I2S pins of the connectivity SoC are configured through
 * wmt_ic_soc_pin_ctrl(); the host side only drives the GPS LNA enable pin
 * through its pinctrl states (wmt_plat_alps.c:700-780).
 */
int wmt_plat_gpio_ctrl(enum wmt_pin_id id, enum wmt_pin_state state)
{
	static const char * const state_name[] = {
		[PIN_STA_INIT] = "gps_lna_state_init",
		[PIN_STA_OUT_H] = "gps_lna_state_oh",
		[PIN_STA_OUT_L] = "gps_lna_state_ol",
	};
	struct pinctrl_state *pstate;
	const char *name;
	int ret;

	if (id == PIN_GPS_SYNC || id == PIN_I2S_GRP)
		return 0;

	if (id != PIN_GPS_LNA)
		return -EINVAL;

	if (state == PIN_STA_DEINIT)
		state = PIN_STA_INIT;

	name = state_name[state];
	if (!name)
		return -EINVAL;

	if (!consys.pinctrl) {
		wmt_dbg("no pinctrl for the GPS LNA pin\n");
		return 0;
	}

	pstate = pinctrl_lookup_state(consys.pinctrl, name);
	if (IS_ERR(pstate)) {
		wmt_err("missing pinctrl state %s\n", name);
		return PTR_ERR(pstate);
	}

	ret = pinctrl_select_state(consys.pinctrl, pstate);
	if (ret)
		wmt_err("failed to select %s (%d)\n", name, ret);

	return ret;
}

int wmt_plat_wake_lock_ctrl(enum wmt_wl_op op)
{
	int ret = 0;

	mutex_lock(&consys.wake_lock);

	if (op == WL_OP_GET) {
		if (consys.wake_count++ == 0)
			pm_stay_awake(consys.dev);
	} else if (op == WL_OP_PUT) {
		if (consys.wake_count == 0) {
			wmt_warn("unbalanced wake lock release\n");
			ret = -EINVAL;
		} else if (--consys.wake_count == 0) {
			pm_relax(consys.dev);
		}
	} else {
		ret = -EINVAL;
	}

	mutex_unlock(&consys.wake_lock);
	return ret;
}

int wmt_plat_init(void)
{
	int ret;

	if (!consys.probed)
		return -ENODEV;

	ret = mtk_wcn_consys_hw_init();
	wmt_dbg("platform init (%d)\n", ret);
	return ret;
}

void wmt_plat_deinit(void)
{
	mtk_wcn_consys_hw_deinit();
}

/*
 * Platform driver
 */

static int mtk_consys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	memset(&consys, 0, sizeof(consys));
	consys.dev = dev;
	mutex_init(&consys.wake_lock);

	consys.mcu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(consys.mcu))
		return PTR_ERR(consys.mcu);

	consys.ap_rgu = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(consys.ap_rgu))
		return PTR_ERR(consys.ap_rgu);

	consys.topckgen = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(consys.topckgen))
		return PTR_ERR(consys.topckgen);

	consys.spm = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(consys.spm))
		return PTR_ERR(consys.spm);

	consys.clk_conn = devm_clk_get(dev, "conn");
	if (IS_ERR(consys.clk_conn)) {
		wmt_err("failed to get the conn clock (%ld)\n",
			PTR_ERR(consys.clk_conn));
		return PTR_ERR(consys.clk_conn);
	}

	consys.clk_bus = devm_clk_get_optional(dev, "bus");
	if (IS_ERR(consys.clk_bus)) {
		wmt_err("failed to get the bus clock (%ld)\n",
			PTR_ERR(consys.clk_bus));
		return PTR_ERR(consys.clk_bus);
	}

	consys.vcn18 = devm_regulator_get_optional(dev, "vcn18");
	if (IS_ERR(consys.vcn18))
		return dev_err_probe(dev, PTR_ERR(consys.vcn18),
				     "failed to get vcn18\n");
	if (!consys.vcn18)
		wmt_warn("no vcn18 supply, CONSYS may not power up\n");

	consys.vcn28 = devm_regulator_get_optional(dev, "vcn28");
	if (IS_ERR(consys.vcn28))
		return dev_err_probe(dev, PTR_ERR(consys.vcn28),
				     "failed to get vcn28\n");

	consys.vcn33_bt = devm_regulator_get_optional(dev, "vcn33_bt");
	if (IS_ERR(consys.vcn33_bt))
		return dev_err_probe(dev, PTR_ERR(consys.vcn33_bt),
				     "failed to get vcn33_bt\n");

	consys.vcn33_wifi = devm_regulator_get_optional(dev, "vcn33_wifi");
	if (IS_ERR(consys.vcn33_wifi))
		return dev_err_probe(dev, PTR_ERR(consys.vcn33_wifi),
				     "failed to get vcn33_wifi\n");

	consys.pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(consys.pinctrl)) {
		ret = PTR_ERR(consys.pinctrl);
		if (ret != -ENODEV) {
			wmt_err("failed to get pinctrl (%d)\n", ret);
			return ret;
		}
		consys.pinctrl = NULL;
	}

	consys.irq_bgf = platform_get_irq(pdev, 0);
	if (consys.irq_bgf < 0) {
		wmt_err("failed to get the BGF interrupt (%d)\n",
			consys.irq_bgf);
		return consys.irq_bgf;
	}

	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, &consys);
	consys.probed = true;

	wmt_info("CONSYS platform probed (irq %d)\n", consys.irq_bgf);
	return 0;
}

static void mtk_consys_remove(struct platform_device *pdev)
{
	if (consys.powered)
		mtk_wcn_consys_hw_pwr_off();

	consys_irq_init(false);
	pm_runtime_disable(&pdev->dev);
	mutex_destroy(&consys.wake_lock);
	consys.probed = false;
}

static const struct of_device_id mtk_consys_of_ids[] = {
	{ .compatible = "mediatek,mt6735-consys" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_consys_of_ids);

static struct platform_driver mtk_consys_driver = {
	.probe = mtk_consys_probe,
	.remove = mtk_consys_remove,
	.driver = {
		.name = "mtk-wcn-consys",
		.of_match_table = mtk_consys_of_ids,
	},
};

int wmt_plat_consys_driver_register(void)
{
	return platform_driver_register(&mtk_consys_driver);
}

void wmt_plat_consys_driver_unregister(void)
{
	platform_driver_unregister(&mtk_consys_driver);
}
