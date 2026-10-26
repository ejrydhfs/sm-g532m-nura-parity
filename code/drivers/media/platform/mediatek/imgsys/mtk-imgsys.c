// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735/MT6737T IMGSYS/ISP platform driver
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c
 *
 * Copyright 2008-2009 MediaTek Co., Ltd.
 *
 * This driver implements the parts of the MT6735 ISP block that are fully
 * documented by the downstream GPL source drop: register window mapping, the
 * IMGSYS clock gates, the ISP power domain and the four vendor-defined soft
 * resets (Pass 1, Pass 2, CAMSV, CAMSV2).
 *
 * The ISP capture pipeline (Pass 1 / Pass 2 / CAM_SV / DSP programming, CQ
 * command queues, interrupt driven frame completion) is driven by the
 * proprietary MTK camera userspace ABI (libcamdrv) and requires ISP firmware
 * images, so it lives in the separate camera-ISP character driver.  This
 * driver therefore registers no V4L2 entity, no video node and no interrupt
 * handler: the camera-ISP character driver owns the pipeline, its own register
 * mapping and the ioctl-triggered resets under its register lock.  This driver
 * runs the resets once at probe, when nothing else can be using the block.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include "mtk-imgsys-regs.h"

/*
 * Clock gates, in the order mandated by the downstream ISP driver
 * (camera_isp_D1.c:3062-3100, "must keep this clk open order"):
 * CG_IMAGE_CAM_SMI, CG_IMAGE_CAM_CAM, CG_IMAGE_SEN_TG, CG_IMAGE_SEN_CAM,
 * CG_IMAGE_CAM_SV, CG_IMAGE_LARB2_SMI.
 *
 * The remaining two gates of the controller (IMG_IMAGE_SUFOD bit 10,
 * IMG_IMAGE_FD bit 11, clk-mt6735.c:899-900) are not used by the camera
 * pipeline: IMG_IMAGE_FD belongs to the fdvt block.
 *
 * The downstream order also enables CG_SCP_SYS_DIS, CG_DISP0_SMI_COMMON and
 * CG_SCP_SYS_ISP first.  In this kernel those are the DIS/ISP SCP power
 * domains (genpd, from the power-domains property) and the DIS SMI common gate
 * (owned by the mtk-smi common device, MTK_SMI_CLK "smi"), so they are not
 * part of this driver's clock list.
 */
static const char *const mtk_imgsys_clk_names[] = {
	"CG_IMAGE_CAM_SMI",
	"CG_IMAGE_CAM_CAM",
	"CG_IMAGE_SEN_TG",
	"CG_IMAGE_SEN_CAM",
	"CG_IMAGE_CAM_SV",
	"CG_IMAGE_LARB2_SMI",
};

#define MTK_IMGSYS_CLK_NUM	ARRAY_SIZE(mtk_imgsys_clk_names)

struct mtk_imgsys {
	struct device *dev;
	void __iomem *isp_base;
	void __iomem *inner_base;
	void __iomem *camsv_base;
	phys_addr_t isp_phys;
	phys_addr_t inner_phys;
	struct clk *clks[MTK_IMGSYS_CLK_NUM];
	struct dentry *debugfs_dir;
	/* Serialises the reset sequences against each other. */
	struct mutex reset_lock;
};

static int mtk_imgsys_clks_enable(struct mtk_imgsys *priv)
{
	int i, ret;

	for (i = 0; i < MTK_IMGSYS_CLK_NUM; i++) {
		ret = clk_prepare_enable(priv->clks[i]);
		if (ret) {
			dev_err(priv->dev, "failed to enable clock %s: %d\n",
				mtk_imgsys_clk_names[i], ret);
			while (--i >= 0)
				clk_disable_unprepare(priv->clks[i]);
			return ret;
		}
	}

	return 0;
}

static void mtk_imgsys_clks_disable(struct mtk_imgsys *priv)
{
	int i;

	for (i = MTK_IMGSYS_CLK_NUM - 1; i >= 0; i--)
		clk_disable_unprepare(priv->clks[i]);
}

/*
 * Pass 1 / Pass 2 soft reset of CAM_CTL_SW_CTL, ported from
 * camera_isp_D1.c:3276-3330 (the enabled "#if 1" branches):
 *
 *	val = readl(CAM_SW_CTL);
 *	writel((val & ~mask) | (trig & mask), CAM_SW_CTL);
 *	val = readl(CAM_SW_CTL);
 *	writel(hw_rst, CAM_SW_CTL);
 *	writel(val & ~mask, CAM_SW_CTL);
 *
 * The downstream completion loop
 *	do { val = readl(CAM_SW_CTL); } while ((!val) & STATUS);
 * can never iterate twice: "(!val)" is 0 or 1 and the bitwise AND with BIT(1)
 * is 0 for both values, so the loop always ends after one read.  The ROM image
 * proves the compiler removed it entirely: the inlined copies read
 * CAM_SW_CTL exactly once between the trigger write and the HW_RST write
 * (P1 in ISP_ioctl @0xc04e7614-0xc04e7650, P2 in ISP_MDPResetCallback
 * @0xc04d3124-0xc04d3160).  There is therefore no reset-status handshake to
 * wait for, and polling for SW_RST_STATUS would only add a failure mode.  The
 * intervening read also orders the two posted writes on the Device mapping.
 */
static void mtk_imgsys_cam_sw_ctl_reset(void __iomem *sw_ctl, u32 mask,
					u32 trig, u32 hw_rst)
{
	u32 val;

	val = readl(sw_ctl);
	writel((val & ~mask) | (trig & mask), sw_ctl);

	val = readl(sw_ctl);

	writel(hw_rst, sw_ctl);
	writel(val & ~mask, sw_ctl);
}

/*
 * CAMSV / CAMSV2 soft reset, camera_isp_D1.c:3332-3349:
 *	trig, 0, read-back, hw_rst, 0 - plain writes, no read-modify-write.
 * ROM: ISP_ioctl @0xc04e7a64-0xc04e7ae8 (CAMSV, ISP_ADDR + 0x5010) and
 * @0xc04e840c-0xc04e8490 (CAMSV2, ISP_ADDR + 0x5810).
 */
static void mtk_imgsys_camsv_reset(void __iomem *sw_ctl)
{
	writel(MTK_IMGSYS_SW_CTL_SW_RST_TRIG, sw_ctl);
	writel(0, sw_ctl);
	/* Read-back of the vendor sequence; also orders the posted writes. */
	readl(sw_ctl);
	writel(MTK_IMGSYS_SW_CTL_HW_RST, sw_ctl);
	writel(0, sw_ctl);
}

/*
 * Reset paths, numbered exactly like the downstream ISP_REG_SW_CTL_RST_*
 * identifiers (camera_isp_D1.c:378-381) that the vendor driver passes to
 * ISP_Reset() and that user space selects through ISP_RESET_CAM_P1/P2/CAMSV/
 * CAMSV2 (include/uapi/linux/mtk_isp.h).
 */
enum mtk_imgsys_reset_path {
	MTK_IMGSYS_RESET_CAM_P1 = 1,
	MTK_IMGSYS_RESET_CAM_P2 = 2,
	MTK_IMGSYS_RESET_CAMSV = 3,
	MTK_IMGSYS_RESET_CAMSV2 = 4,
};

/*
 * Runs one reset path.  The caller guarantees that the ISP block is powered
 * and that resets are serialised.
 */
static int mtk_imgsys_reset_one(struct mtk_imgsys *priv,
				enum mtk_imgsys_reset_path path)
{
	void __iomem *sw_ctl;

	switch (path) {
	case MTK_IMGSYS_RESET_CAM_P1:
		sw_ctl = priv->isp_base + MTK_IMGSYS_CAM_SW_CTL;
		mtk_imgsys_cam_sw_ctl_reset(sw_ctl,
					    MTK_IMGSYS_SW_CTL_SW_RST_P1_MASK,
					    MTK_IMGSYS_SW_CTL_SW_RST_TRIG,
					    MTK_IMGSYS_SW_CTL_HW_RST);
		break;
	case MTK_IMGSYS_RESET_CAM_P2:
		sw_ctl = priv->isp_base + MTK_IMGSYS_CAM_SW_CTL;
		mtk_imgsys_cam_sw_ctl_reset(sw_ctl,
					    MTK_IMGSYS_SW_CTL_SW_RST_P2_MASK,
					    MTK_IMGSYS_SW_CTL_SW_RST_P2_TRIG,
					    MTK_IMGSYS_SW_CTL_HW_RST_P2);
		break;
	case MTK_IMGSYS_RESET_CAMSV:
		if (!priv->camsv_base)
			return -ENODEV;
		sw_ctl = priv->camsv_base + MTK_IMGSYS_CAMSV_SW_CTL;
		mtk_imgsys_camsv_reset(sw_ctl);
		break;
	case MTK_IMGSYS_RESET_CAMSV2:
		if (!priv->camsv_base)
			return -ENODEV;
		sw_ctl = priv->camsv_base + MTK_IMGSYS_CAMSV2_SW_CTL;
		mtk_imgsys_camsv_reset(sw_ctl);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Bring every ISP sub-block to the documented post-reset state.  Only used
 * once, from probe, while nothing else can be using the block.
 */
static void mtk_imgsys_reset_all(struct mtk_imgsys *priv)
{
	mtk_imgsys_reset_one(priv, MTK_IMGSYS_RESET_CAM_P1);
	mtk_imgsys_reset_one(priv, MTK_IMGSYS_RESET_CAM_P2);

	if (!priv->camsv_base)
		return;

	mtk_imgsys_reset_one(priv, MTK_IMGSYS_RESET_CAMSV);
	mtk_imgsys_reset_one(priv, MTK_IMGSYS_RESET_CAMSV2);
}

static int mtk_imgsys_runtime_resume(struct device *dev)
{
	struct mtk_imgsys *priv = dev_get_drvdata(dev);

	/*
	 * Clocks only: downstream performs no reset when the ISP clocks are
	 * enabled (ISP_EnableClock(), camera_isp_D1.c:3190-3250, is clocks
	 * only), resets are on demand (camera_isp_D1.c:9890-9911, :12161).
	 * Resetting here would also corrupt a stream owned by the camera-ISP
	 * character driver, because this device's runtime PM state is
	 * independent of that driver's pipeline state - a debugfs read is
	 * enough to resume it.  The character driver runs resets through
	 * mtk_imgsys_reset() at the points the vendor ABI asks for them.
	 */
	return mtk_imgsys_clks_enable(priv);
}

static int mtk_imgsys_runtime_suspend(struct device *dev)
{
	struct mtk_imgsys *priv = dev_get_drvdata(dev);

	mtk_imgsys_clks_disable(priv);

	return 0;
}

static const struct dev_pm_ops mtk_imgsys_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(mtk_imgsys_runtime_suspend, mtk_imgsys_runtime_resume,
		       NULL)
};

static int mtk_imgsys_regs_show(struct seq_file *s, void *unused)
{
	struct mtk_imgsys *priv = s->private;
	unsigned int i, off;
	int ret;

	/*
	 * The register windows are only accessible while the ISP power domain
	 * and the clock gates are on, so resume the device for the duration of
	 * the dump.
	 *
	 * Note: the ISP interrupt status registers (CAM_SW_CTL +0x4c..+0x78,
	 * CAMSV_INT +0x500c, CAMSV2_INT +0x580c) are read-to-clear, so this
	 * dump destroys pending interrupt information.  It is a power-on
	 * diagnostic, not something to run while the ISP pipeline streams.
	 */
	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret)
		return ret;

	seq_printf(s, "ISP core @ %pa (0x%x bytes)\n", &priv->isp_phys,
		   MTK_IMGSYS_ISP_WINDOW_SIZE);
	for (i = 0, off = 0; i < MTK_IMGSYS_ISP_WINDOW_SIZE / sizeof(u32);
	     i++, off += sizeof(u32))
		seq_printf(s, "%04x: %08x\n", off,
			   readl(priv->isp_base + off));

	seq_printf(s, "inner ISP @ %pa (0x%x bytes)\n", &priv->inner_phys,
		   MTK_IMGSYS_INNER_ISP_WINDOW_SIZE);
	for (i = 0, off = 0; i < MTK_IMGSYS_INNER_ISP_WINDOW_SIZE / sizeof(u32);
	     i++, off += sizeof(u32))
		seq_printf(s, "%04x: %08x\n", off,
			   readl(priv->inner_base + off));

	pm_runtime_put(priv->dev);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_imgsys_regs);

static int mtk_imgsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_imgsys *priv;
	struct resource *res;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->reset_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	priv->isp_phys = res->start;
	priv->isp_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->isp_base))
		return PTR_ERR(priv->isp_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -ENODEV;
	priv->inner_phys = res->start;
	priv->inner_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->inner_base))
		return PTR_ERR(priv->inner_base);

	/*
	 * CAMSV/CAMSV2 control bank at ISP_ADDR + 0x5000 (CAMSV_SW_CTL =
	 * +0x5010, CAMSV2_SW_CTL = +0x5810, camera_isp_D1.c:263,267).  It is
	 * part of the vendor's single 0x9000 ISP window (rom_dtb_0.dts:3706)
	 * but lies outside the 0x4000 window resource 0 declares, and the
	 * seninf driver owns 0x15008000..0x1500bfff.  Map it unreserved -
	 * downstream uses of_iomap() for the whole ISP window as well - so
	 * that neither driver's resource claim can fail because of the other.
	 */
	priv->camsv_base = devm_ioremap(dev,
					priv->isp_phys +
						MTK_IMGSYS_CAMSV_WINDOW_OFFSET,
					MTK_IMGSYS_CAMSV_WINDOW_SIZE);
	if (!priv->camsv_base)
		dev_info(dev,
			 "CAMSV register bank unavailable, CAMSV resets disabled\n");

	for (i = 0; i < MTK_IMGSYS_CLK_NUM; i++) {
		priv->clks[i] = devm_clk_get(dev, mtk_imgsys_clk_names[i]);
		if (IS_ERR(priv->clks[i])) {
			dev_err(dev, "failed to get clock %s: %ld\n",
				mtk_imgsys_clk_names[i],
				PTR_ERR(priv->clks[i]));
			return PTR_ERR(priv->clks[i]);
		}
	}

	platform_set_drvdata(pdev, priv);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/*
	 * Power the block on (ISP power domain through genpd, then the clock
	 * gates) and reset every sub-block once.  Nothing can be using the
	 * block yet, so this cannot disturb a running pipeline; later resets
	 * come from the character driver through mtk_imgsys_reset().
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err(dev, "failed to power on ISP block: %d\n", ret);
		return ret;
	}

	mutex_lock(&priv->reset_lock);
	mtk_imgsys_reset_all(priv);
	mutex_unlock(&priv->reset_lock);

	priv->debugfs_dir = debugfs_create_dir("mtk-imgsys", NULL);
	if (!IS_ERR_OR_NULL(priv->debugfs_dir))
		debugfs_create_file("regs", 0444, priv->debugfs_dir, priv,
				    &mtk_imgsys_regs_fops);

	pm_runtime_put(dev);

	dev_info(dev, "MT6735 IMGSYS/ISP block registered (ISP pipeline deferred)\n");

	return 0;
}

static void mtk_imgsys_remove(struct platform_device *pdev)
{
	struct mtk_imgsys *priv = platform_get_drvdata(pdev);

	debugfs_remove(priv->debugfs_dir);
}

static const struct of_device_id mtk_imgsys_of_match[] = {
	{ .compatible = "mediatek,mt6735-ispsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_imgsys_of_match);

static struct platform_driver mtk_imgsys_driver = {
	.probe = mtk_imgsys_probe,
	.remove = mtk_imgsys_remove,
	.driver = {
		.name = "mtk-imgsys",
		.of_match_table = mtk_imgsys_of_match,
		.pm = pm_ptr(&mtk_imgsys_pm_ops),
	},
};
module_platform_driver(mtk_imgsys_driver);

MODULE_AUTHOR("MediaTek Co., Ltd.");
MODULE_DESCRIPTION("MediaTek MT6735 IMGSYS/ISP platform driver");
MODULE_LICENSE("GPL");
