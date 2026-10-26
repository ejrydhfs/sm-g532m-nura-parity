// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735 RF clock buffer control.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.c
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.h
 *   drivers/misc/mediatek/base/power/spm_v1/mt_spm.c (spm_ap_bsi_gen)
 *   drivers/misc/mediatek/base/power/spm_v1/mt_spm_sleep.c (spm_ap_mdsrc_req)
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * The four RF clock buffers (BB/MD, CONN, NFC, AUDIO) are requested either
 * through the SPM "MD BSI" control register while the modem is powered, or
 * through a bit-banged AP BSI command word written into the SPM protocol
 * generator.  Both sequences, the command-word layout and the software
 * shadow semantics are identical to the downstream driver.
 *
 * All SPM registers are accessed through the "mediatek,spm" syscon phandle,
 * which is the 0x10006000 bank: downstream reaches the same registers via
 * SPM_BASE, which the vendor DTB declares as sleep@10006000
 * (arch/arm/boot/dts/mt6735.dtsi:367-369) and the ROM
 * DTB declares as the second reg range of scpsys (rom_dtb_0.dts:307-311).
 *
 * Deviations from downstream, all reported by the leaf report:
 *  - a missing/unusable DT node or SPM phandle fails the probe instead of
 *    hitting BUG() (mt_clkbuf_ctl.c:174-187), and every entry point then
 *    returns the documented failure value instead of dereferencing garbage;
 *  - the unbounded SPM_BSI_START poll (:941-944) and the busy-wait loop of
 *    the AP BSI generator are bounded;
 *  - the sysfs store takes the driver lock the downstream version lacks.
 */

#include <linux/clkbuf.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "mt_clkbuf_ctl.h"

#define DEFINE_ATTR_RW(_name)			\
static struct kobj_attribute _name##_attr = {	\
	.attr	= {				\
		.name = #_name,			\
		.mode = 0644,			\
	},					\
	.show	= _name##_show,			\
	.store	= _name##_store,		\
}

#define __ATTR_OF(_name)	(&_name##_attr.attr)

/*
 * mt_clkbuf_ctl.c:59-64 -- software shadow, BB/MD starts enabled.
 */
static struct mt_clkbuf clkbuf = {
	.lock = __MUTEX_INITIALIZER(clkbuf.lock),
	.swctrl = {
		CLK_BUF_SW_ENABLE,
		CLK_BUF_SW_DISABLE,
		CLK_BUF_SW_DISABLE,
		CLK_BUF_SW_DISABLE,
	},
};

/*
 * AP BSI command word
 */

/* mt_clkbuf_ctl.c:85-96 */
static u32 mt_clkbuf_cw(const enum clk_buf_swctrl_status *status)
{
	unsigned int bsi_clk_setting, bsi_cw_data;

	bsi_clk_setting = ((status[3] << 2) | (status[2] << 1) |
			   (status[1] << 0));
	bsi_cw_data = ((BSI_CW_DEFAULT & (~(BSI_CLK_MASK << 4))) |
		       ((bsi_clk_setting & BSI_CLK_MASK) << 4));

	return ((BSI_DATA_BIT & 0x1) << 29) | ((BSI_READ_BIT & 0x1) << 28) |
	       ((BSI_CW_ADDR & 0xFF) << 20) | ((bsi_cw_data & 0xFFFFF) << 0);
}

/*
 * spm_ap_bsi_gen() (spm_v1/mt_spm.c:934-945): latch the five shift-register
 * images, start the generator and wait for it to clear the start bit.
 */
static int mt_clkbuf_bsi_gen(const u32 *cfg)
{
	static const u32 regs[CLK_BUF_BSI_PAD_NUM] = {
		SPM_BSI_EN_SR, SPM_BSI_CLK_SR, SPM_BSI_D0_SR, SPM_BSI_D1_SR,
		SPM_BSI_D2_SR,
	};
	unsigned int i;
	u32 val;
	int ret;

	for (i = 0; i < CLK_BUF_BSI_PAD_NUM; i++) {
		ret = regmap_write(clkbuf.spm, regs[i], cfg[i]);
		if (ret)
			return ret;
	}

	ret = regmap_read(clkbuf.spm, SPM_BSI_GEN, &val);
	if (ret)
		return ret;

	ret = regmap_write(clkbuf.spm, SPM_BSI_GEN, val | SPM_BSI_START);
	if (ret)
		return ret;

	/* mt_spm.c:941-944, bounded here (see header). */
	for (i = 0; i < BSI_GEN_POLL_US; i++) {
		ret = regmap_read(clkbuf.spm, SPM_BSI_GEN, &val);
		if (ret)
			return ret;
		if (!(val & SPM_BSI_START))
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

/* mt_clkbuf_ctl.c:98-149 */
static int mt_clkbuf_send_bsi_cw(const enum clk_buf_swctrl_status *status)
{
	bool d0, d1, d2;
	int i;
	u32 cw;

	clkbuf.bsi_cfg[BSI_EN_SR] = 0x20000000;
	clkbuf.bsi_cfg[BSI_CLK_SR] = 0x00000000;
	clkbuf.bsi_cfg[BSI_D0_SR] = 0x00000000;
	clkbuf.bsi_cfg[BSI_D1_SR] = 0x00000000;
	clkbuf.bsi_cfg[BSI_D2_SR] = 0x00000000;
	clkbuf.bit_ctrl = 29;

	cw = mt_clkbuf_cw(status);

	for (i = (BSI_CW_CNT - 1); i >= 0; i = i - 3) {
		d0 = ((cw >> (i - 2)) & 0x1) ? true : false;
		d1 = ((cw >> (i - 1)) & 0x1) ? true : false;
		d2 = ((cw >> (i - 0)) & 0x1) ? true : false;

		clkbuf.bit_ctrl -= 1;
		clkbuf.bsi_cfg[BSI_EN_SR] |= (1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_CLK_SR] &= ~(1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D0_SR] |= (d0 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D1_SR] |= (d1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D2_SR] |= (d2 << clkbuf.bit_ctrl);

		clkbuf.bit_ctrl -= 1;
		clkbuf.bsi_cfg[BSI_EN_SR] |= (1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_CLK_SR] |= (1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D0_SR] |= (d0 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D1_SR] |= (d1 << clkbuf.bit_ctrl);
		clkbuf.bsi_cfg[BSI_D2_SR] |= (d2 << clkbuf.bit_ctrl);
	}

	clkbuf.bit_ctrl -= 1;
	clkbuf.bsi_cfg[BSI_EN_SR] |= (1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_CLK_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D0_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D1_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D2_SR] &= ~(1 << clkbuf.bit_ctrl);

	clkbuf.bit_ctrl -= 1;
	clkbuf.bsi_cfg[BSI_EN_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_CLK_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D0_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D1_SR] &= ~(1 << clkbuf.bit_ctrl);
	clkbuf.bsi_cfg[BSI_D2_SR] &= ~(1 << clkbuf.bit_ctrl);

	return mt_clkbuf_bsi_gen(clkbuf.bsi_cfg);
}

/*
 * spm_ap_mdsrc_req() (spm_v1/mt_spm_sleep.c:2031-2096): raise/relax the AP
 * request for the MD1 SRCCLKENA/APSRC handshake.  The request is reference
 * counted so nested users do not cut the modem's clock source early.
 */
static int mt_clkbuf_mdsrc_req(bool set)
{
	u32 val;
	int i, ret;

	if (set) {
		if (clkbuf.mdsrc_req_cnt < 0) {
			dev_warn(clkbuf.dev,
				 "unbalanced MDSRC request (set=%d, count=%d)\n",
				 set, clkbuf.mdsrc_req_cnt);
			return -EINVAL;
		}

		clkbuf.mdsrc_req_cnt++;

		ret = regmap_read(clkbuf.spm, SPM_POWER_ON_VAL1, &val);
		if (ret)
			return ret;
		ret = regmap_write(clkbuf.spm, SPM_POWER_ON_VAL1,
				   val | SPMPWR_AP_MDSRC_REQ);
		if (ret)
			return ret;

		/* spm_ap_mdsrc_req(): if md_apsrc_req = 1'b0, wait 26M
		 * settling time (3 ms) (mt_spm_sleep.c:2054-2058).
		 */
		ret = regmap_read(clkbuf.spm, SPM_PCM_REG13_DATA, &val);
		if (ret)
			return ret;
		if (!(val & R13_MD1_APSRC_REQ)) {
			dev_warn(clkbuf.dev,
				 "MD1 APSRC not requested, waiting %d ms for 26M settling\n",
				 MDSRC_SETTLE_MS);
			mdelay(MDSRC_SETTLE_MS);
		}

		/* Check ap_mdsrc_ack = 1'b1 (mt_spm_sleep.c:2060-2071). */
		for (i = 0; i < MDSRC_ACK_RETRY; i++) {
			ret = regmap_read(clkbuf.spm, SPM_PCM_REG13_DATA, &val);
			if (ret)
				return ret;
			if (val & R13_AP_MD1SRC_ACK)
				break;
			mdelay(MDSRC_ACK_DELAY_MS);
		}
		if (i == MDSRC_ACK_RETRY)
			dev_warn(clkbuf.dev,
				 "AP_MD1SRC_ACK not seen after %d ms\n",
				 MDSRC_ACK_RETRY * MDSRC_ACK_DELAY_MS);

		return 0;
	}

	clkbuf.mdsrc_req_cnt--;

	if (clkbuf.mdsrc_req_cnt < 0) {
		dev_warn(clkbuf.dev, "unbalanced MDSRC request (set=%d, count=%d)\n",
			 set, clkbuf.mdsrc_req_cnt);
		return -EINVAL;
	}

	if (clkbuf.mdsrc_req_cnt)
		return 0;

	ret = regmap_read(clkbuf.spm, SPM_POWER_ON_VAL1, &val);
	if (ret)
		return ret;

	return regmap_write(clkbuf.spm, SPM_POWER_ON_VAL1,
			    val & ~SPMPWR_AP_MDSRC_REQ);
}

/* mt_clkbuf_ctl.c:151-169 -- MD BSI control while the modem is powered. */
static int mt_clkbuf_spm_ctrl(const enum clk_buf_swctrl_status *status)
{
	u32 spm_val;
	int i, ret;

	ret = mt_clkbuf_mdsrc_req(true);
	if (ret)
		return ret;

	ret = regmap_read(clkbuf.spm, SPM_SLEEP_MDBSI_CON, &spm_val);
	if (ret)
		goto out_req;

	spm_val &= ~SPM_SLEEP_MDBSI_MASK;

	for (i = 1; i < CLKBUF_NUM; i++)
		spm_val |= (u32)status[i] << (i - 1);

	ret = regmap_write(clkbuf.spm, SPM_SLEEP_MDBSI_CON, spm_val);
	if (ret)
		goto out_req;

	udelay(2);

out_req:
	if (mt_clkbuf_mdsrc_req(false) && !ret)
		ret = -EIO;

	return ret;
}

/*
 * Frozen API (include/linux/clkbuf.h)
 */

/* mt_clkbuf_ctl.c:170-217 */
bool clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	u32 pwr_status, pwr_status_2nd;
	int ret;

	if (!clkbuf.ready)
		return false;

	if ((unsigned int)id >= CLK_BUF_INVALID)
		return false;

	if (clkbuf.config[id] == CLOCK_BUFFER_HW_CONTROL)
		return false;

	mutex_lock(&clkbuf.lock);

	clkbuf.swctrl[id] = onoff ? CLK_BUF_SW_ENABLE : CLK_BUF_SW_DISABLE;

	ret = regmap_read(clkbuf.spm, SPM_PWR_STATUS, &pwr_status);
	if (!ret)
		ret = regmap_read(clkbuf.spm, SPM_PWR_STATUS_2ND,
				  &pwr_status_2nd);
	if (ret)
		goto out_unlock;

	if ((pwr_status & SPM_PWR_STATUS_MD) &&
	    (pwr_status_2nd & SPM_PWR_STATUS_MD))
		ret = mt_clkbuf_spm_ctrl(clkbuf.swctrl);
	else
		ret = mt_clkbuf_send_bsi_cw(clkbuf.swctrl);

out_unlock:
	mutex_unlock(&clkbuf.lock);

	if (ret) {
		dev_err(clkbuf.dev, "clock buffer %d %s failed (%d)\n", id,
			onoff ? "enable" : "disable", ret);
		return false;
	}

	return true;
}
EXPORT_SYMBOL_GPL(clk_buf_ctrl);

/* mt_clkbuf_ctl.c:219-225 */
void clk_buf_get_swctrl_status(enum clk_buf_swctrl_status *status)
{
	int i;

	for (i = 0; i < CLKBUF_NUM; i++)
		status[i] = clkbuf.swctrl[i];
}
EXPORT_SYMBOL_GPL(clk_buf_get_swctrl_status);

/*
 * /sys/power/clk_buf/clk_buf_ctrl (mt_clkbuf_ctl.c:227-282)
 */

static ssize_t clk_buf_ctrl_store(struct kobject *kobj,
				  struct kobj_attribute *attr, const char *buf,
				  size_t count)
{
	enum clk_buf_swctrl_status clk_buf_en[CLKBUF_NUM];
	unsigned int en[CLKBUF_NUM];
	char cmd[32];
	int i, ret;

	if (!clkbuf.ready)
		return -ENODEV;

	if (sscanf(buf, "%31s %x %x %x %x", cmd, &en[0], &en[1], &en[2],
		   &en[3]) != 5)
		return -EPERM;

	for (i = 0; i < CLKBUF_NUM; i++)
		clk_buf_en[i] = (enum clk_buf_swctrl_status)en[i];

	mutex_lock(&clkbuf.lock);

	/* Downstream updates the shadow before validating the command. */
	for (i = 0; i < CLKBUF_NUM; i++)
		clkbuf.swctrl[i] = clk_buf_en[i];

	if (!strcmp(cmd, "bsi"))
		ret = mt_clkbuf_spm_ctrl(clkbuf.swctrl);
	else if (!strcmp(cmd, "apbsi"))
		ret = mt_clkbuf_send_bsi_cw(clkbuf.swctrl);
	else
		ret = -EINVAL;

	mutex_unlock(&clkbuf.lock);

	if (ret)
		return ret;

	return count;
}

static ssize_t clk_buf_ctrl_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	int len = 0;
	int i;

	len += sysfs_emit_at(buf, len,
			     "********** clock buffer state **********\n");
	for (i = 0; i < CLKBUF_NUM; i++)
		len += sysfs_emit_at(buf, len,
				     "CKBUF%d SW(1)/HW(2) CTL: %d, Disable(0)/Enable(1): %d\n",
				     i + 1, clkbuf.config[i], clkbuf.swctrl[i]);
	len += sysfs_emit_at(buf, len,
			     "\n********** clock buffer command help **********\n");
	len += sysfs_emit_at(buf, len,
			     "BSI  switch on/off: echo bsi en1 en2 en3 en4 > /sys/power/clk_buf/clk_buf_ctrl\n");
	len += sysfs_emit_at(buf, len,
			     "APBSI switch on/off: echo apbsi en1 en2 en3 en4 > /sys/power/clk_buf/clk_buf_ctrl\n");
	len += sysfs_emit_at(buf, len, "BB   :en1\n");
	len += sysfs_emit_at(buf, len, "6605 :en2\n");
	len += sysfs_emit_at(buf, len, "5193 :en3\n");
	len += sysfs_emit_at(buf, len, "AUDIO:en4\n");

	return len;
}

DEFINE_ATTR_RW(clk_buf_ctrl);

static struct attribute *clk_buf_attrs[] = {
	__ATTR_OF(clk_buf_ctrl),
	NULL,
};

static struct attribute_group clk_buf_attr_group = {
	.name = "clk_buf",
	.attrs = clk_buf_attrs,
};

/* mt_clkbuf_ctl.c:296-313 */
bool clk_buf_init(void)
{
	int ret;

	if (clkbuf.sysfs_added)
		return clkbuf.ready;

	/*
	 * power_kobj is not exported to modules and the attribute must exist
	 * for the whole uptime, so CONFIG_MTK_CLKBUF is bool (built-in).
	 */
	ret = sysfs_create_group(power_kobj, &clk_buf_attr_group);
	if (ret) {
		pr_err("clk_buf: failed to create /sys/power/clk_buf (%d)\n",
		       ret);
		return false;
	}

	clkbuf.sysfs_added = true;

	return clkbuf.ready;
}
EXPORT_SYMBOL_GPL(clk_buf_init);

static int __init mt_clkbuf_late_init(void)
{
	if (!clk_buf_init())
		pr_err("clk_buf: clock buffer control unavailable\n");

	return 0;
}
/* mt_pm_init.c:550-559 calls clk_buf_init() from late_initcall. */
late_initcall(mt_clkbuf_late_init);

/*
 * Platform glue
 */

static int mt_clkbuf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *spm_np;
	struct regmap *spm;
	int ret;

	if (clkbuf.ready) {
		dev_err(dev, "only one RF clock buffer controller is supported\n");
		return -EBUSY;
	}

	/*
	 * Downstream tolerates a failed read and then uses uninitialised
	 * stack data (mt_clkbuf_ctl.c:174-187); a configuration this driver
	 * cannot read is a hard probe failure instead.
	 */
	ret = of_property_read_u32_array(dev->of_node, "mediatek,clkbuf-config",
					 clkbuf.config, CLKBUF_NUM);
	if (ret) {
		dev_err(dev, "missing or short \"mediatek,clkbuf-config\" (%d)\n",
			ret);
		return ret;
	}

	spm_np = of_parse_phandle(dev->of_node, "mediatek,spm", 0);
	if (!spm_np) {
		dev_err(dev, "missing \"mediatek,spm\" phandle to the SPM syscon\n");
		return -ENODEV;
	}

	spm = device_node_to_regmap(spm_np);
	of_node_put(spm_np);
	if (IS_ERR(spm)) {
		dev_err(dev, "cannot map the SPM syscon (%ld)\n",
			PTR_ERR(spm));
		return PTR_ERR(spm);
	}

	clkbuf.dev = dev;
	clkbuf.spm = spm;
	clkbuf.ready = true;

	dev_info(dev,
		 "RF clock buffers: BB/MD %u, CONN %u, NFC %u, AUDIO %u (1=SW, 2=HW control)\n",
		 clkbuf.config[CLK_BUF_BB_MD], clkbuf.config[CLK_BUF_CONN],
		 clkbuf.config[CLK_BUF_NFC], clkbuf.config[CLK_BUF_AUDIO]);

	return 0;
}

static void mt_clkbuf_remove(struct platform_device *pdev)
{
	clkbuf.ready = false;
	clkbuf.spm = NULL;
	clkbuf.dev = NULL;
}

static const struct of_device_id mt_clkbuf_of_match[] = {
	{ .compatible = "mediatek,rf_clock_buffer" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt_clkbuf_of_match);

static struct platform_driver mt_clkbuf_driver = {
	.probe	= mt_clkbuf_probe,
	.remove	= mt_clkbuf_remove,
	.driver	= {
		.name		= "mtk-clkbuf",
		.of_match_table	= mt_clkbuf_of_match,
	},
};
module_platform_driver(mt_clkbuf_driver);

MODULE_DESCRIPTION("MediaTek MT6735 RF clock buffer control");
MODULE_LICENSE("GPL");
