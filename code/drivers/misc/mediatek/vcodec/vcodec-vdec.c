// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735 VDEC engine driver.
 *
 * The VDEC register windows are mapped into the vendor userspace codec
 * libraries by the shared core; this driver only owns the clock tree, the
 * power domains, the optional block reset, the IRQ line and the decoder
 * ISR validation/acknowledgment sequence.
 *
 * Port from Samsung/MediaTek downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c
 * (Android 6.0.1, kernel 3.18.19, SM-G532M).
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "vcodec.h"

/* Downstream videocodec_kernel_driver_D1.c:557 */
#define VCODEC_VDEC_DEC_DONE		0xA4
#define VCODEC_VDEC_DEC_DONE_BIT	BIT(16)
/* Downstream videocodec_kernel_driver_D1.c:582-584 */
#define VCODEC_VDEC_IRQ_ACK		(BIT(0) | BIT(4))
#define VCODEC_VDEC_IRQ_CLR		BIT(4)
/* Downstream videocodec_kernel_driver_D1.c:553 */
#define VCODEC_VDEC_GCON_VDEC_ACTIVE	BIT(4)

/*
 * Downstream videocodec_kernel_driver_D1.c:337-338 (VDEC_BASE_PHY 0x16000000,
 * VDEC_REGION 0x29000) and :2175-2176 (mmap accepts a start anywhere in that
 * window).  The whole window is published to userspace, including the SMI
 * LARB1 block at 0x16010000 that the vendor HAL is allowed to map.
 */
#define VCODEC_VDEC_MMAP_SIZE		0x29000

/*
 * VDEC dynamic frequency scaling, ported from the ENABLE_MMDVFS_VDEC block of
 * videocodec_kernel_driver_D1.c:107-260 and SendDvfsRequest() at :122-162.
 *
 * The decoder is clocked from the TOP_VDEC_SEL mux.  Its parent 1 is
 * SYSPLL1_D2 and parent 3 is SYSPLL1_D4, which the vendor clock tables
 * document as 273 MHz and 136.5 MHz (mt_pm_init.c:300-320).  The monitor
 * measures how much of every 4 s window the decoder was actually locked and
 * moves the mux between the two parents.
 */
#define VCODEC_DVFS_MONITOR_DURATION_MS	4000
#define VCODEC_DVFS_DROP_PERCENTAGE	50
#define VCODEC_DVFS_RAISE_PERCENTAGE	90
#define VCODEC_DVFS_SW_OVERHEAD_MS	1

/*
 * MMDVFS_VOLTAGE_LOW / _HIGH as used by downstream; the value is also
 * visible in the ROM: SendDvfsRequest @ 0xc0396240 treats 0 as low and 1 as
 * high (mov r0,#3 / mov r1,#0|#1 before mmdvfs_set_step @ 0xc04cacdc).
 */
enum vcodec_vdec_dvfs_level {
	VCODEC_DVFS_LOW = 0,
	VCODEC_DVFS_HIGH,
};

/* Clock gates driven by power_on(), in device-tree enable order. */
static const char * const vcodec_vdec_clk_names[] = {
	"smi_common",
	"vdec",
	"smi_larb1",
};

struct vcodec_vdec {
	struct device *dev;
	void __iomem *vdec;
	void __iomem *gcon;
	phys_addr_t gcon_phys;
	struct clk_bulk_data *clks;
	int nclks;
	struct reset_control *rst;
	int irq;
	bool registered;

	/* TOP_VDEC_SEL mux and the two SYSPLL1 parents of the DVFS steps */
	struct clk *clk_sel;
	struct clk *clk_sel_high;
	struct clk *clk_sel_low;
	bool dvfs_enabled;
	/* protects the monitor bookkeeping below */
	struct mutex dvfs_lock;
	bool monitor_active;
	bool first_lock;
	u32 monitor_counts;
	ktime_t monitor_start;
	ktime_t last_lock;
	u32 hwlock_interval_ms;
	s32 hwlock_max_duration_ms;

	struct vcodec_engine engine;
};

/*
 * Downstream videocodec_kernel_driver_D1.c:122-162 (SendDvfsRequest): select
 * the TOP_VDEC_SEL parent that corresponds to @level, with the mux prepared
 * and enabled around clk_set_parent() exactly as downstream does.  The
 * parents come from the device tree as clock handles, so no rate is ever
 * computed here.
 *
 * Downstream additionally sends the matching SMI bandwidth / VCORE vote
 * through mmdvfs_set_step(SMI_BWC_SCEN_VP, @level).  That half is not
 * implemented: this kernel has no MMDVFS/vcorefs framework (see the report).
 */
static void vcodec_vdec_dvfs_request(struct vcodec_vdec *vdec, int level)
{
	struct clk *parent;
	int ret;

	if (!vdec->dvfs_enabled)
		return;

	parent = level == VCODEC_DVFS_LOW ? vdec->clk_sel_low :
					    vdec->clk_sel_high;

	ret = clk_prepare_enable(vdec->clk_sel);
	if (ret) {
		dev_err(vdec->dev, "failed to enable the VDEC DVFS mux: %d\n",
			ret);
		return;
	}

	ret = clk_set_parent(vdec->clk_sel, parent);
	if (ret)
		dev_err(vdec->dev,
			"failed to select the VDEC clock parent: %d\n", ret);

	clk_disable_unprepare(vdec->clk_sel);

	dev_dbg(vdec->dev, "VDEC DVFS request: %s\n",
		level == VCODEC_DVFS_LOW ? "LOW" : "HIGH");
}

/* Downstream videocodec_kernel_driver_D1.c:163-173 */
static void vcodec_vdec_dvfs_begin(struct vcodec_vdec *vdec)
{
	vdec->monitor_active = true;
	vdec->monitor_counts = 0;
	vdec->hwlock_interval_ms = 0;
	vdec->first_lock = true;
	vdec->hwlock_max_duration_ms = 0;
}

/* Downstream videocodec_kernel_driver_D1.c:175-182 */
static void vcodec_vdec_dvfs_end(struct vcodec_vdec *vdec, int level)
{
	dev_dbg(vdec->dev,
		"VDEC DVFS monitor %u ms, %u locks, busy %u ms, max %d ms, level %d\n",
		VCODEC_DVFS_MONITOR_DURATION_MS, vdec->monitor_counts,
		vdec->hwlock_interval_ms, vdec->hwlock_max_duration_ms,
		level);

	vdec->monitor_active = false;
	vdec->monitor_counts = 0;
	vdec->hwlock_interval_ms = 0;
	vdec->hwlock_max_duration_ms = 0;
}

/* Downstream videocodec_kernel_driver_D1.c:183-189 */
static u32 vcodec_vdec_dvfs_monitor_duration(struct vcodec_vdec *vdec)
{
	return (u32)ktime_ms_delta(ktime_get(), vdec->monitor_start);
}

/* Downstream videocodec_kernel_driver_D1.c:191-205 */
static u32 vcodec_vdec_dvfs_step(struct vcodec_vdec *vdec)
{
	u32 diff = (u32)ktime_ms_delta(ktime_get(), vdec->last_lock);

	if (diff > vdec->hwlock_max_duration_ms)
		vdec->hwlock_max_duration_ms = diff;
	vdec->hwlock_interval_ms += diff + VCODEC_DVFS_SW_OVERHEAD_MS;

	return diff;
}

/*
 * Downstream videocodec_kernel_driver_D1.c:207-241 (VdecDvfsAdjustment):
 * close the measurement window that started with the first lock after the
 * previous evaluation and move the mux when the decoder was busy for less
 * than 50% or more than 90% of it.
 */
static void vcodec_vdec_dvfs_adjustment(struct vcodec_vdec *vdec)
{
	u32 duration, busier;

	mutex_lock(&vdec->dvfs_lock);
	if (vdec->monitor_active && vdec->monitor_counts > 0) {
		duration = vcodec_vdec_dvfs_monitor_duration(vdec);
		if (duration < VCODEC_DVFS_MONITOR_DURATION_MS) {
			vcodec_vdec_dvfs_step(vdec);
		} else {
			vcodec_vdec_dvfs_step(vdec);
			busier = 100 * vdec->hwlock_interval_ms / duration;
			dev_dbg(vdec->dev,
				"VDEC DVFS window %u ms, busy %u%%\n",
				duration, busier);

			if (busier < VCODEC_DVFS_DROP_PERCENTAGE) {
				vcodec_vdec_dvfs_request(vdec, VCODEC_DVFS_LOW);
				vcodec_vdec_dvfs_end(vdec, VCODEC_DVFS_LOW);
			} else if (busier > VCODEC_DVFS_RAISE_PERCENTAGE) {
				vcodec_vdec_dvfs_request(vdec,
							 VCODEC_DVFS_HIGH);
				vcodec_vdec_dvfs_end(vdec, VCODEC_DVFS_HIGH);
			} else {
				vcodec_vdec_dvfs_end(vdec, -1);
			}
		}
	}
	vdec->monitor_counts++;
	mutex_unlock(&vdec->dvfs_lock);
}

/* Downstream videocodec_kernel_driver_D1.c:243-260 */
static void vcodec_vdec_dvfs_monitor_start(struct vcodec_vdec *vdec)
{
	mutex_lock(&vdec->dvfs_lock);
	if (!vdec->monitor_active)
		vcodec_vdec_dvfs_begin(vdec);

	if (vdec->monitor_counts > 0) {
		if (vdec->first_lock) {
			vdec->first_lock = false;
			vdec->monitor_start = ktime_get();
		}
		vdec->last_lock = ktime_get();
	}
	mutex_unlock(&vdec->dvfs_lock);
}

/* Downstream videocodec_kernel_driver_D1.c:2121-2137 */
static void vcodec_vdec_dvfs_idle(struct vcodec_vdec *vdec)
{
	mutex_lock(&vdec->dvfs_lock);
	if (vdec->monitor_active) {
		vdec->monitor_active = false;
		vdec->monitor_counts = 0;
		vdec->hwlock_interval_ms = 0;
		vdec->hwlock_max_duration_ms = 0;
		vcodec_vdec_dvfs_request(vdec, VCODEC_DVFS_LOW);
	}
	mutex_unlock(&vdec->dvfs_lock);
}

/* Engine callbacks.  The core holds eng->lock and is in process context. */
static void vcodec_vdec_dvfs_begin_op(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	if (!vdec->dvfs_enabled)
		return;

	mutex_lock(&vdec->dvfs_lock);
	vcodec_vdec_dvfs_request(vdec, VCODEC_DVFS_HIGH);
	vcodec_vdec_dvfs_begin(vdec);
	mutex_unlock(&vdec->dvfs_lock);
}

static void vcodec_vdec_dvfs_monitor_start_op(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	if (vdec->dvfs_enabled)
		vcodec_vdec_dvfs_monitor_start(vdec);
}

static void vcodec_vdec_dvfs_adjustment_op(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	if (vdec->dvfs_enabled)
		vcodec_vdec_dvfs_adjustment(vdec);
}

static void vcodec_vdec_dvfs_idle_op(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	if (vdec->dvfs_enabled)
		vcodec_vdec_dvfs_idle(vdec);
}

static int vcodec_vdec_power_on(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;
	int ret;

	/*
	 * Resuming the device also resumes the power domains attached in
	 * probe, in device-tree order (DIS, then VDE), through the PM
	 * runtime device links.  Downstream
	 * videocodec_kernel_driver_D1.c:402-431 powers SCP_SYS_DIS before
	 * SCP_SYS_VDE.
	 */
	ret = pm_runtime_resume_and_get(vdec->dev);
	if (ret) {
		dev_err(vdec->dev, "failed to resume VDEC power domains: %d\n", ret);
		return ret;
	}

	/*
	 * Downstream videocodec_kernel_driver_D1.c:395-397,402-431 enables the
	 * SCP_SYS_DIS, DISP0_SMI_COMMON, SCP_SYS_VDE, VDEC0_VDEC and
	 * VDEC1_LARB clocks in this order.
	 */
	ret = clk_bulk_prepare_enable(vdec->nclks, vdec->clks);
	if (ret) {
		dev_err(vdec->dev, "failed to enable VDEC clocks: %d\n", ret);
		pm_runtime_put_sync(vdec->dev);
		return ret;
	}

	if (vdec->rst) {
		ret = reset_control_deassert(vdec->rst);
		if (ret) {
			dev_err(vdec->dev, "failed to deassert VDEC reset: %d\n", ret);
			clk_bulk_disable_unprepare(vdec->nclks, vdec->clks);
			pm_runtime_put_sync(vdec->dev);
			return ret;
		}
	}

	return 0;
}

static void vcodec_vdec_power_off(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	/*
	 * Downstream videocodec_kernel_driver_D1.c:444-455 disables the same
	 * clocks in reverse order and never touches the block reset; the
	 * reset is kept asserted here so that userspace always starts from
	 * a defined block state on the next power_on.
	 */
	if (vdec->rst)
		reset_control_assert(vdec->rst);

	clk_bulk_disable_unprepare(vdec->nclks, vdec->clks);

	/*
	 * Drop the power domain references last, after the clock gates are
	 * off, as downstream does at
	 * videocodec_kernel_driver_D1.c:451-455.
	 */
	pm_runtime_put_sync(vdec->dev);
}

static int vcodec_vdec_enable_irq(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	/* Downstream videocodec_kernel_driver_D1.c:855 */
	enable_irq(vdec->irq);

	return 0;
}

static void vcodec_vdec_disable_irq(struct vcodec_engine *eng)
{
	struct vcodec_vdec *vdec = eng->priv;

	/* Downstream videocodec_kernel_driver_D1.c:848 */
	disable_irq(vdec->irq);
}

static const struct vcodec_engine_ops vcodec_vdec_ops = {
	.power_on = vcodec_vdec_power_on,
	.power_off = vcodec_vdec_power_off,
	.enable_irq = vcodec_vdec_enable_irq,
	.disable_irq = vcodec_vdec_disable_irq,
	.dvfs_begin = vcodec_vdec_dvfs_begin_op,
	.dvfs_monitor_start = vcodec_vdec_dvfs_monitor_start_op,
	.dvfs_adjustment = vcodec_vdec_dvfs_adjustment_op,
	.dvfs_idle = vcodec_vdec_dvfs_idle_op,
};

/*
 * Hibernation restore: the boot loader or the kernel that loaded the image
 * may have reset the MT6735 interrupt polarity registers, so re-apply the
 * level-low trigger of the decoder line.  The irq core runs all
 * ->restore_noirq callbacks before resume_device_irqs(), so the line is
 * still masked here and cannot fire while it is being reconfigured.
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c:2375-2390
 * (vcodec_pm_restore_noirq), which issues
 * mt_irq_set_sens(VDEC_IRQ_ID, MT_LEVEL_SENSITIVE) and
 * mt_irq_set_polarity(VDEC_IRQ_ID, MT_POLARITY_LOW).
 * IRQ_TYPE_LEVEL_LOW is the irq-core spelling of that pair: on the
 * mediatek,mt6735-sysirq parent that the codec interrupt is routed through
 * it sets the INTPOL bit and hands IRQ_TYPE_LEVEL_HIGH to the GIC, as the
 * downstream MTK interrupt controller code does
 * (drivers/irqchip/irq-mtk-sysirq.c: mtk_sysirq_set_type()).
 *
 * The ROM logs nothing and always returns 0; a failure is reported here for
 * debuggability but, like the ROM, not propagated to abort the restore.
 */
static int __maybe_unused vcodec_vdec_restore_noirq(struct device *dev)
{
	struct vcodec_vdec *vdec = dev_get_drvdata(dev);
	int ret;

	ret = irq_set_irq_type(vdec->irq, IRQ_TYPE_LEVEL_LOW);
	if (ret)
		dev_err(dev, "failed to restore VDEC IRQ polarity: %d\n", ret);

	return 0;
}

static const struct dev_pm_ops vcodec_vdec_pm_ops = {
	.restore_noirq = pm_sleep_ptr(vcodec_vdec_restore_noirq),
};

/*
 * Downstream videocodec_kernel_driver_D1.c:541-594 (dec_isr): a valid
 * decoder interrupt requires the VDEC GCON "active" bit to be clear and the
 * decode-done bit to be set; the status is cleared by first setting bit 0
 * together with bit 4 and then clearing bit 4 again.
 */
static irqreturn_t vcodec_vdec_isr(int irq, void *data)
{
	struct vcodec_vdec *vdec = data;
	u32 cg_status, dec_status;

	cg_status = readl(vdec->gcon);
	if (cg_status & VCODEC_VDEC_GCON_VDEC_ACTIVE) {
		dev_err_ratelimited(vdec->dev,
				    "DEC ISR, VDEC active is not 0x0 (0x%08x)\n",
				    cg_status);
		return IRQ_HANDLED;
	}

	dec_status = readl(vdec->vdec + VCODEC_VDEC_DEC_DONE);
	if (!(dec_status & VCODEC_VDEC_DEC_DONE_BIT)) {
		dev_err_ratelimited(vdec->dev,
				    "DEC ISR, decode done status is not 0x1 (0x%08x)\n",
				    dec_status);
		return IRQ_HANDLED;
	}

	writel(readl(vdec->vdec + VCODEC_VDEC_DEC_DONE) | VCODEC_VDEC_IRQ_ACK,
	       vdec->vdec + VCODEC_VDEC_DEC_DONE);
	writel(readl(vdec->vdec + VCODEC_VDEC_DEC_DONE) & ~VCODEC_VDEC_IRQ_CLR,
	       vdec->vdec + VCODEC_VDEC_DEC_DONE);

	vcodec_engine_irq(&vdec->engine, 0);

	return IRQ_HANDLED;
}

static int vcodec_vdec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dev_pm_domain_list *pd_list = NULL;
	struct vcodec_vdec *vdec;
	struct resource *res;
	unsigned int i;
	int ret;

	vdec = devm_kzalloc(dev, sizeof(*vdec), GFP_KERNEL);
	if (!vdec)
		return -ENOMEM;

	vdec->dev = dev;
	mutex_init(&vdec->dvfs_lock);
	vdec->clks = devm_kcalloc(dev, ARRAY_SIZE(vcodec_vdec_clk_names),
				  sizeof(*vdec->clks), GFP_KERNEL);
	if (!vdec->clks)
		return -ENOMEM;
	platform_set_drvdata(pdev, vdec);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vdec");
	if (!res) {
		dev_err(dev, "missing \"vdec\" register window\n");
		return -ENODEV;
	}
	vdec->vdec = devm_ioremap_resource(dev, res);
	if (IS_ERR(vdec->vdec))
		return dev_err_probe(dev, PTR_ERR(vdec->vdec),
				     "failed to map \"vdec\" register window\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gcon");
	if (!res) {
		dev_err(dev, "missing \"gcon\" register window\n");
		return -ENODEV;
	}
	vdec->gcon_phys = res->start;
	vdec->gcon = devm_ioremap_resource(dev, res);
	if (IS_ERR(vdec->gcon))
		return dev_err_probe(dev, PTR_ERR(vdec->gcon),
				     "failed to map \"gcon\" register window\n");

	/*
	 * Downstream videocodec_kernel_driver_D1.c:2290-2357 enables the SMI
	 * common, VDEC and SMI LARB1 gates in its power-on path; only those
	 * three are prepared here so that the DVFS parent clocks, which are
	 * switched while the engine is off, stay out of power_on().
	 */
	for (i = 0; i < ARRAY_SIZE(vcodec_vdec_clk_names); i++)
		vdec->clks[i].id = vcodec_vdec_clk_names[i];
	vdec->nclks = ARRAY_SIZE(vcodec_vdec_clk_names);
	ret = devm_clk_bulk_get(dev, vdec->nclks, vdec->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get VDEC clocks\n");

	/*
	 * Downstream videocodec_kernel_driver_D1.c:2321-2338 gets the
	 * TOP_MUX_VDEC mux and its two SYSPLL1 parents last, after the gates.
	 * They are optional here: without them the decoder keeps the parent
	 * the boot loader selected and only the clock half of DVFS is lost.
	 */
	vdec->clk_sel = devm_clk_get_optional(dev, "vdec_sel");
	if (IS_ERR(vdec->clk_sel))
		return dev_err_probe(dev, PTR_ERR(vdec->clk_sel),
				     "failed to get the VDEC clock mux\n");
	vdec->clk_sel_high = devm_clk_get_optional(dev, "vdec_sel_high");
	if (IS_ERR(vdec->clk_sel_high))
		return dev_err_probe(dev, PTR_ERR(vdec->clk_sel_high),
				     "failed to get the VDEC high clock parent\n");
	vdec->clk_sel_low = devm_clk_get_optional(dev, "vdec_sel_low");
	if (IS_ERR(vdec->clk_sel_low))
		return dev_err_probe(dev, PTR_ERR(vdec->clk_sel_low),
				     "failed to get the VDEC low clock parent\n");

	vdec->dvfs_enabled = vdec->clk_sel && vdec->clk_sel_high &&
			     vdec->clk_sel_low;
	if (vdec->dvfs_enabled)
		dev_info(dev, "VDEC DVFS enabled (136.5/273 MHz clock steps)\n");
	else
		dev_info(dev, "VDEC DVFS clock parents not described, DVFS disabled\n");

	/*
	 * The PM core only attaches a single power domain per device
	 * (drivers/pmdomain/core.c: genpd_dev_pm_attach() bails out when the
	 * node has more than one), so attach the DIS and VDE domains of the
	 * DT node here.  The device links created for them make the
	 * pm_runtime calls in power_on/power_off drive both domains in DT
	 * order.  Downstream videocodec_kernel_driver_D1.c:402-455 powers
	 * and gates SCP_SYS_DIS and SCP_SYS_VDE around the VDEC clocks.
	 *
	 * -EEXIST means the node has a single domain, already attached to
	 * @dev by the platform bus; the pm_runtime calls then drive it
	 * directly.
	 */
	ret = devm_pm_domain_attach_list(dev, NULL, &pd_list);
	if (ret < 0 && ret != -EEXIST)
		return dev_err_probe(dev, ret, "failed to attach VDEC power domains\n");
	if (!ret) {
		dev_err(dev, "no VDEC power domains described in device tree\n");
		return -ENODEV;
	}

	/*
	 * Downstream has no block reset handling.  The MT6735 vdecsys clock
	 * driver exports MT6735_VDEC_RST0_VDEC, so use it when the board
	 * describes it and work without it otherwise.
	 */
	vdec->rst = devm_reset_control_get_optional_exclusive(dev, "vdec");
	if (IS_ERR(vdec->rst))
		return dev_err_probe(dev, PTR_ERR(vdec->rst),
				     "failed to get VDEC reset\n");

	vdec->irq = platform_get_irq(pdev, 0);
	if (vdec->irq < 0)
		return vdec->irq;

	pm_runtime_enable(dev);

	/* Downstream videocodec_kernel_driver_D1.c:2272-2286 */
	ret = devm_request_irq(dev, vdec->irq, vcodec_vdec_isr, 0,
			       dev_name(dev), vdec);
	if (ret) {
		dev_err(dev, "failed to request VDEC IRQ %d: %d\n", vdec->irq, ret);
		goto err_pm;
	}

	/* The core unmasks the line from its LOCKHW ioctl. */
	disable_irq(vdec->irq);

	vdec->engine.name = "vdec";
	vdec->engine.id = VCODEC_ENGINE_DEC;
	vdec->engine.dev = dev;
	vdec->engine.ops = &vcodec_vdec_ops;
	vdec->engine.priv = vdec;
	vdec->engine.regions[0].phys = vdec->gcon_phys;
	vdec->engine.regions[0].size = VCODEC_VDEC_MMAP_SIZE;
	vdec->engine.num_regions = 1;

	ret = vcodec_engine_register(&vdec->engine);
	if (ret) {
		dev_err(dev, "failed to register VDEC engine: %d\n", ret);
		goto err_pm;
	}
	vdec->registered = true;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	return ret;
}

static void vcodec_vdec_remove(struct platform_device *pdev)
{
	struct vcodec_vdec *vdec = platform_get_drvdata(pdev);

	if (vdec->registered) {
		vcodec_engine_unregister(&vdec->engine);
		vdec->registered = false;
	}

	pm_runtime_disable(vdec->dev);
}

static const struct of_device_id vcodec_vdec_of_match[] = {
	{ .compatible = "mediatek,mt6735-vdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, vcodec_vdec_of_match);

static struct platform_driver vcodec_vdec_driver = {
	.probe = vcodec_vdec_probe,
	.remove = vcodec_vdec_remove,
	.driver = {
		.name = "mtk-vcodec-vdec",
		.of_match_table = vcodec_vdec_of_match,
		.pm = &vcodec_vdec_pm_ops,
		.suppress_bind_attrs = true,
	},
};

int vcodec_vdec_register(void)
{
	return platform_driver_register(&vcodec_vdec_driver);
}

void vcodec_vdec_unregister(void)
{
	platform_driver_unregister(&vcodec_vdec_driver);
}
