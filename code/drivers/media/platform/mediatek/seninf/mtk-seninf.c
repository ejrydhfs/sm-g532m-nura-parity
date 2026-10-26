// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735 SENINF (sensor interface) driver
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c
 *   drivers/misc/mediatek/cameraisp/src/mt6735/inc/camera_isp_D1.h
 *   drivers/misc/mediatek/imgsensor/src/mt6735/kd_sensorlist.c
 *   drivers/misc/mediatek/imgsensor/src/mt6735/camera_hw/kd_camera_hw.c
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2016 Samsung Electronics Co., Ltd.
 *
 * The SENINF block is the CSI-2 receiver of the MT6735 camera subsystem.  It
 * owns the per-camera MCLK output gates, the per-camera error status
 * registers and the CSI-2 receiver including its MIPI D-PHY lane and settle
 * time programming.  The downstream GPL source only describes the MCLK and
 * error status registers; the CSI-2/D-PHY programming exists solely in the
 * proprietary userspace "seninf_drv" (referenced at camera_isp_D1.c:125-133),
 * which is shipped as libcam.halsensor.so on this device.  That library was
 * disassembled and the recovered programming is implemented here; every
 * register value cites the instruction address it was taken from, see
 * mtk-seninf-regs.h and the report.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sprintf.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "mtk-seninf-regs.h"

#define MTK_SENINF_NAME			"mtk-seninf"

/*
 * SENINF1..SENINF4 have a status block each (camera_isp_D1.c:257-260), so up
 * to four sink ports can be described in DT.
 */
#define MTK_SENINF_NUM_CAMS		4

/*
 * Only ISP_MCLK1_EN..ISP_MCLK3_EN exist in the downstream source
 * (camera_isp_D1.c:12048-12137, EXPORT_SYMBOL at 12076/12107/12137); no
 * ISP_MCLK4_EN is defined anywhere in the GPL drop.
 */
#define MTK_SENINF_NUM_MCLKS		3

/*
 * Upper bound on the number of MIPI CSI-2 data lanes checked against the
 * remote sensor configuration.  Four lanes are in use on this platform: the
 * IMX219 sensor driver configures SENSOR_MIPI_4_LANE
 * (imx219_mipi_raw/imx219mipiraw_Sensor.c:160) and is routed through this
 * receiver, so the MIPI RX block accepts at least four data lanes.
 */
#define MTK_SENINF_MAX_DATA_LANES	4

struct mtk_seninf;

/**
 * struct mtk_seninf_mclk - one MCLK output gate
 * @hw: clock hardware exposed through the #clock-cells=<1> provider
 * @priv: back pointer to the SENINF instance
 * @parent_hws: single parent, the "camtg" clock
 * @idx: camera index, selects the ISP_MCLKn_EN register
 */
struct mtk_seninf_mclk {
	struct clk_hw hw;
	struct mtk_seninf *priv;
	const struct clk_hw *parent_hws[1];
	unsigned int idx;
};

/**
 * struct mtk_seninf_asc - async connection carrying the local sink pad index
 * @asc: V4L2 async connection, must be the first member
 * @pad: sink pad (== DT port number) the remote sensor is bound to
 */
struct mtk_seninf_asc {
	struct v4l2_async_connection asc;
	unsigned int pad;
};

/**
 * struct mtk_seninf - driver instance
 * @dev: device
 * @base: SENINF register window
 * @mipi_rx: MIPIRX_CONFIG register window (debugfs only)
 * @mipi_ana: MIPIRX_ANALOG register window (debugfs only)
 * @camtg: camera timing clock feeding the MCLK outputs
 * @mclk: MCLK output gates
 * @v4l2_dev: V4L2 device
 * @mdev: media device, the single one of the camera subsystem
 * @sd: SENINF V4L2 subdevice
 * @pads: sink pads (one per DT port) plus one source pad
 * @num_sink_pads: number of sink pads
 * @notifier: async notifier for the sensor subdevices
 * @lock: serialises s_stream
 * @streaming: streaming state
 * @streaming_pads: sink pads currently enabled in the CSI-2 receiver
 * @debugfs: debugfs directory
 */
struct mtk_seninf {
	struct device *dev;
	void __iomem *base;
	void __iomem *mipi_rx;
	void __iomem *mipi_ana;
	struct clk *camtg;

	struct mtk_seninf_mclk mclk[MTK_SENINF_NUM_MCLKS];

	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct v4l2_subdev sd;
	struct media_pad *pads;
	unsigned int num_sink_pads;

	struct v4l2_async_notifier notifier;

	struct mutex lock;
	bool streaming;
	u32 streaming_pads;

	struct dentry *debugfs;
};

static inline struct mtk_seninf *to_mtk_seninf(struct v4l2_subdev *sd)
{
	return v4l2_get_subdevdata(sd);
}

static inline struct mtk_seninf_mclk *to_mtk_seninf_mclk(struct clk_hw *hw)
{
	return container_of(hw, struct mtk_seninf_mclk, hw);
}

/*
 * Error status
 *
 * The SENINF status registers are polled through the ISP CAM0..CAM2 interrupt
 * lines downstream (arch/arm/boot/dts/mt6735.dtsi:1314-1320, camera_isp_D1.c
 * ISP_IRQ_TYPE_INT_SENINF1..4), never through an interrupt of their own, so this
 * driver does not request an IRQ and has no interrupt-enable programming.
 */

static const char * const mtk_seninf_err_names[] = {
	"overrun", "crc", "fsm", "vsize", "hsize", "sensor-vsize", "sensor-hsize",
};

static void mtk_seninf_check_errors(struct mtk_seninf *priv, const char *where)
{
	unsigned int i;

	for (i = 0; i < MTK_SENINF_NUM_CAMS; i++) {
		u32 reg = MTK_SENINF_CAM_INTSTA(i);
		u32 err;
		u32 sta;
		char names[128];
		size_t len = 0;
		unsigned int bit;

		sta = readl(priv->base + reg);
		err = sta & MTK_SENINF_IRQ_ERR_MASK;
		if (!err)
			continue;

		names[0] = '\0';
		for (bit = 0; bit < ARRAY_SIZE(mtk_seninf_err_names); bit++) {
			if (!(err & BIT(bit)))
				continue;
			len += scnprintf(names + len, sizeof(names) - len, "%s%s",
					 len ? "|" : "", mtk_seninf_err_names[bit]);
			if (len >= sizeof(names))
				break;
		}

		dev_err(priv->dev, "SENINF%u error status 0x%08x at %s: %s\n",
			i + 1, sta, where, names);

		/*
		 * [INFERENCE] The status register is cleared by writing back the
		 * set bits (write-one-to-clear), like the other MTK IRQ_STA
		 * registers.  The downstream source only ever reads this register
		 * (camera_isp_D1.c:9203-9217, inside "#if 0"), so the clear
		 * convention cannot be cited from the GPL drop.
		 */
		writel(err, priv->base + reg);
		if (readl(priv->base + reg) & err)
			dev_warn(priv->dev,
				 "SENINF%u error status 0x%08x did not clear\n",
				 i + 1, err);
	}
}

/*
 * MCLK clock provider
 *
 * ISP_MCLKn_EN sits in the per-camera SENINF register block and is raised while
 * the "camtg" clock runs: kd_sensorlist.c:2453-2459 enables camtg before
 * kdCISModulePowerOn() raises MCLK (kd_camera_hw.c:519-523) and disables it
 * after the MCLK gate is dropped.
 *
 * The "camtg" clock is modelled as the CCF parent of every MCLK output, so
 * clk_prepare_enable() on an MCLK output prepares/enables camtg through the
 * clock core (correct refcounting, no manual user counters) and the rate
 * reported to the sensor drivers is camtg's rate, exactly what the downstream
 * kdSetSensorMclk() programs (kd_sensorlist.c:2442-2461).
 *
 * The SENINF registers themselves only need the ISP power domain, which is a
 * pure SPM power domain (no clocks property in its DT node,
 * arch/arm64/boot/dts/mediatek/mt6735.dtsi:223-227).  .prepare therefore takes
 * a runtime PM reference so the domain is on, and .unprepare drops it.  Calling
 * runtime PM from .prepare is safe: nothing in the resume path takes the CCF
 * prepare_lock, because camtg is brought up by the clock core parent chain, not
 * by the resume callback.
 */
static int mtk_seninf_mclk_prepare(struct clk_hw *hw)
{
	struct mtk_seninf_mclk *mclk = to_mtk_seninf_mclk(hw);
	struct mtk_seninf *priv = mclk->priv;
	u32 reg = MTK_SENINF_MCLK_REG(mclk->idx);
	int ret;

	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0) {
		dev_err(priv->dev, "mclk%u: failed to resume: %d\n",
			mclk->idx, ret);
		return ret;
	}

	writel(readl(priv->base + reg) | MTK_SENINF_MCLK_EN, priv->base + reg);

	return 0;
}

static void mtk_seninf_mclk_unprepare(struct clk_hw *hw)
{
	struct mtk_seninf_mclk *mclk = to_mtk_seninf_mclk(hw);
	struct mtk_seninf *priv = mclk->priv;
	u32 reg = MTK_SENINF_MCLK_REG(mclk->idx);

	writel(readl(priv->base + reg) & ~MTK_SENINF_MCLK_EN, priv->base + reg);

	/* No autosuspend is used, so this matches the get in .prepare. */
	pm_runtime_put(priv->dev);
}

static const struct clk_ops mtk_seninf_mclk_ops = {
	.prepare = mtk_seninf_mclk_prepare,
	.unprepare = mtk_seninf_mclk_unprepare,
};

static const char * const mtk_seninf_mclk_names[MTK_SENINF_NUM_MCLKS] = {
	"mclk0", "mclk1", "mclk2",
};

static int mtk_seninf_register_mclks(struct mtk_seninf *priv)
{
	struct clk_hw *camtg_hw = __clk_get_hw(priv->camtg);
	struct clk_hw_onecell_data *data;
	unsigned int i;
	int ret;

	data = devm_kzalloc(priv->dev, struct_size(data, hws, MTK_SENINF_NUM_MCLKS),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num = MTK_SENINF_NUM_MCLKS;

	for (i = 0; i < MTK_SENINF_NUM_MCLKS; i++) {
		struct mtk_seninf_mclk *mclk = &priv->mclk[i];
		struct clk_init_data init = {
			.name = mtk_seninf_mclk_names[i],
			.ops = &mtk_seninf_mclk_ops,
			.parent_hws = mclk->parent_hws,
			.num_parents = 1,
		};

		mclk->priv = priv;
		mclk->idx = i;
		mclk->parent_hws[0] = camtg_hw;
		mclk->hw.init = &init;

		ret = devm_clk_hw_register(priv->dev, &mclk->hw);
		if (ret) {
			dev_err(priv->dev, "failed to register mclk%u: %d\n", i, ret);
			return ret;
		}

		data->hws[i] = &mclk->hw;
	}

	return devm_of_clk_add_hw_provider(priv->dev, of_clk_hw_onecell_get, data);
}

/* Re-apply the MCLK gates after the ISP power domain came back. */
static void mtk_seninf_mclk_restore(struct mtk_seninf *priv)
{
	unsigned int i;

	for (i = 0; i < MTK_SENINF_NUM_MCLKS; i++) {
		if (!clk_hw_is_prepared(&priv->mclk[i].hw))
			continue;
		writel(readl(priv->base + MTK_SENINF_MCLK_REG(i)) | MTK_SENINF_MCLK_EN,
		       priv->base + MTK_SENINF_MCLK_REG(i));
	}
}

/*
 * Register defaults applied by SeninfDrvImp::init() @0x182e8..0x1833e, i.e.
 * before any source is selected:
 *   SENINF1..4 MUX_CTRL |= bit 31   (@0x182f2/0x182fe/0x1830a/0x18316)
 *   SENINF1..3 CTRL |= bit 0        (@0x18322/0x1832e/0x1833e)
 * This is the only part of the SENINF mux configuration that does not depend
 * on the vendor HAL's per-sensor source selection, which is not available to
 * the kernel (see the report).
 */
static void mtk_seninf_hw_init(struct mtk_seninf *priv)
{
	unsigned int i;

	for (i = 0; i < MTK_SENINF_NUM_CAMS - 1; i++)
		writel(readl(priv->base + MTK_SENINF_CAM_CTRL(i)) |
		       MTK_SENINF_CTRL_EN,
		       priv->base + MTK_SENINF_CAM_CTRL(i));

	for (i = 0; i < MTK_SENINF_NUM_CAMS; i++)
		writel(readl(priv->base + MTK_SENINF_CAM_MUX_CTRL(i)) |
		       MTK_SENINF_MUX_EN,
		       priv->base + MTK_SENINF_CAM_MUX_CTRL(i));
}

static int mtk_seninf_runtime_resume(struct device *dev)
{
	struct mtk_seninf *priv = dev_get_drvdata(dev);

	/*
	 * The ISP power domain has just been switched on again, so the SENINF
	 * registers lost their contents: re-apply the init defaults, the MCLK
	 * gates of every output a consumer still holds and poll the error
	 * status.
	 */
	mtk_seninf_hw_init(priv);
	mtk_seninf_mclk_restore(priv);
	mtk_seninf_check_errors(priv, "runtime resume");

	return 0;
}

static int mtk_seninf_runtime_suspend(struct device *dev)
{
	/*
	 * Nothing to do: the ISP power domain is switched off by genpd and the
	 * "camtg" clock is kept on by the CCF parent chain for as long as a
	 * consumer holds its MCLK output.
	 */
	return 0;
}

static const struct dev_pm_ops mtk_seninf_pm_ops = {
	RUNTIME_PM_OPS(mtk_seninf_runtime_suspend, mtk_seninf_runtime_resume, NULL)
};

/*
 * Link configuration
 *
 * The remote sensor describes the CSI-2 link through its mbus configuration
 * (link frequency and number of data lanes).  The lane count is the value the
 * CSI-2 receiver needs; the link frequency is validated and logged.
 */
struct mtk_seninf_link {
	unsigned int lanes;
	s64 link_freq;
};

static int mtk_seninf_get_link(struct mtk_seninf *priv, unsigned int pad,
			       struct mtk_seninf_link *link)
{
	struct media_pad *remote;
	struct v4l2_subdev *remote_sd;
	s64 link_freq;
	int lanes;

	link->lanes = 0;
	link->link_freq = 0;

	remote = media_pad_remote_pad_first(&priv->pads[pad]);
	if (!remote)
		return -ENODEV;

	remote_sd = media_entity_to_v4l2_subdev(remote->entity);

	link_freq = v4l2_get_link_freq(remote, 1, 1);
	if (link_freq < 0) {
		if (link_freq != -ENOENT) {
			dev_err(priv->dev, "pad%u: invalid link frequency: %lld\n",
				pad, link_freq);
			return link_freq;
		}
		dev_warn(priv->dev, "pad%u: %s does not report a link frequency\n",
			 pad, remote_sd->name);
	} else {
		link->link_freq = link_freq;
		dev_info(priv->dev, "pad%u: %s link frequency %lld Hz\n",
			 pad, remote_sd->name, link_freq);
	}

	lanes = v4l2_get_active_data_lanes(remote, MTK_SENINF_MAX_DATA_LANES);
	if (lanes < 0) {
		dev_err(priv->dev, "pad%u: unusable data lane count: %d\n",
			pad, lanes);
		return lanes;
	}

	dev_info(priv->dev, "pad%u: %s %d data lane(s)\n",
		 pad, remote_sd->name, lanes);

	link->lanes = lanes;

	return 0;
}

/*
 * CSI-2 receiver and MIPI D-PHY programming
 *
 * Recovered from the proprietary userspace SENINF driver that the vendor
 * camera HAL uses on this SoC: libcam.halsensor.so, class SeninfDrvImp,
 * methods init() @0x18078, setSeninf1NCSI2 @0x16240, setSeninf2NCSI2
 * @0x166f8, setSeninf3NCSI2 @0x16b34.  The code is Thumb-2
 * (readelf -A: Tag_THUMB_ISA_use: Thumb-2).
 *
 * The vendor function takes the D-PHY parameters from the sensor static
 * information block (ACDK_SENSOR_INFO_STRUCT, kd_imgsensor_define.h:427-460).
 * Both sensors of this board report MIPI_OPHY_NCSI2, 85 ns data lane settle
 * time, ECC order 1 and no DPCM, so the NCSI2 register group below is the one
 * that applies; see mtk-seninf-regs.h for the full per-register evidence.
 */

/*
 * Static sensor information that has no V4L2/DT representation on this
 * platform.  Both sensors wired to this receiver report the same values:
 *   .mipi_sensor_type = MIPI_OPHY_NCSI2  (imx219mipiraw_Sensor.c:156,
 *                                         s5k5e3yxmipiraw_Sensor.c:146)
 *   sensor_info->SensorPacketECCOrder = 1 (imx219mipiraw_Sensor.c:1476,
 *                                          s5k5e3yxmipiraw_Sensor.c:1433)
 *   sensor_info->MIPIDataLowPwr2HighSpeedTermDelayCount = 0
 *                                        (imx219mipiraw_Sensor.c:1472)
 *   .mipi_data_lp2hs_settle_dc = 85 ns   (imx219mipiraw_Sensor.c:78,
 *                                         s5k5e3yxmipiraw_Sensor.c:69)
 */
#define MTK_SENINF_ECC_ORDER		1

static u32 mtk_seninf_settle_count(unsigned int settle_ns)
{
	/* 0x164d2/0x164da: settle_count = settle_ns * 300 / 1000 */
	return settle_ns * MTK_SENINF_SETTLE_CLK_MHZ / 1000;
}

/*
 * MIPI D-PHY / MIPI RX digital bring-up, SeninfDrvImp::setSeninf1NCSI2
 * @0x16288..0x1633a.  The register window is the MIPI RX analog block plus
 * 0x800 per SENINF instance (init @0x1827c..0x18356).
 */
static void mtk_seninf_mipi_rx_enable(struct mtk_seninf *priv, unsigned int pad)
{
	void __iomem *rx = priv->mipi_ana + MTK_SENINF_MIPI_RX_OFF(pad);
	unsigned int i;

	/* @0x16294/0x1629e: clear the configuration bit fields */
	writel(readl(rx + MTK_SENINF_MIPI_SHARED0) & MTK_SENINF_MIPI_CFG_CLR,
	       rx + MTK_SENINF_MIPI_SHARED0);
	writel(readl(rx + MTK_SENINF_MIPI_SHARED1) & MTK_SENINF_MIPI_CFG_CLR,
	       rx + MTK_SENINF_MIPI_SHARED1);

	/* @0x162aa..0x162da: pre-charge all five lane registers */
	for (i = 0; i < MTK_SENINF_MIPI_LANES; i++)
		writel(readl(rx + MTK_SENINF_MIPI_LANE_REG(i)) |
		       MTK_SENINF_MIPI_LANE_PWR,
		       rx + MTK_SENINF_MIPI_LANE_REG(i));

	/* @0x162e8, then usleep(30) @0x162ea */
	writel(readl(rx + MTK_SENINF_MIPI_CTL1) | BIT(0),
	       rx + MTK_SENINF_MIPI_CTL1);
	usleep_range(30, 50);

	/* @0x162f8, then usleep(en) @0x162fc */
	writel(readl(rx + MTK_SENINF_MIPI_CTL0) | MTK_SENINF_MIPI_CTL0_BITS,
	       rx + MTK_SENINF_MIPI_CTL0);
	usleep_range(1, 10);

	/* @0x1630a..0x1633a: enable all five lane registers */
	for (i = 0; i < MTK_SENINF_MIPI_LANES; i++)
		writel(readl(rx + MTK_SENINF_MIPI_LANE_REG(i)) |
		       MTK_SENINF_MIPI_LANE_EN,
		       rx + MTK_SENINF_MIPI_LANE_REG(i));
}

/*
 * SeninfDrvImp::setSeninf1NCSI2 @0x165f6..0x166b4, the "NCSI2_en != 1" branch.
 * The vendor function never programs the lane map (this[0x58]) on this board:
 * the value comes from an ioctl into the HAL and the block is skipped when it
 * is zero (@0x16340), so it is not reproduced here.
 */
static void mtk_seninf_mipi_rx_disable(struct mtk_seninf *priv, unsigned int pad)
{
	void __iomem *rx = priv->mipi_ana + MTK_SENINF_MIPI_RX_OFF(pad);
	unsigned int i;

	writel(readl(rx + MTK_SENINF_MIPI_CTL1) & ~BIT(0),
	       rx + MTK_SENINF_MIPI_CTL1);
	writel(readl(rx + MTK_SENINF_MIPI_CTL0) & ~BIT(0),
	       rx + MTK_SENINF_MIPI_CTL0);

	for (i = 0; i < MTK_SENINF_MIPI_LANES; i++)
		writel(readl(rx + MTK_SENINF_MIPI_LANE_REG(i)) &
		       ~MTK_SENINF_MIPI_LANE_EN,
		       rx + MTK_SENINF_MIPI_LANE_REG(i));

	/* @0x1666e/0x16678 */
	writel(readl(rx + MTK_SENINF_MIPI_SHARED0) | MTK_SENINF_MIPI_CFG_SET,
	       rx + MTK_SENINF_MIPI_SHARED0);
	writel(readl(rx + MTK_SENINF_MIPI_SHARED1) | MTK_SENINF_MIPI_CFG_SET,
	       rx + MTK_SENINF_MIPI_SHARED1);

	for (i = 0; i < MTK_SENINF_MIPI_LANES; i++)
		writel(readl(rx + MTK_SENINF_MIPI_LANE_REG(i)) &
		       ~MTK_SENINF_MIPI_LANE_PWR,
		       rx + MTK_SENINF_MIPI_LANE_REG(i));
}

/*
 * "CSI-0 calibration" handshake, SeninfDrvImp::setSeninf1NCSI2
 * (@0x163ca..0x164bc; SENINF3 @0x16c60..0x16d52, SENINF2 @0x16854..0x16920).
 *
 * SENINF1 and SENINF3 start the sequence, write its parameter and poll two
 * status registers; SENINF2 only sets the mode bit and runs no start/sequence
 * write at all (@0x16894 is its single write to the control register).  A
 * failed handshake is only reported by the vendor driver, it does not abort
 * the stream-on; the same is done here.
 */
static void mtk_seninf_csi2_calibrate(struct mtk_seninf *priv, unsigned int pad)
{
	void __iomem *cal = priv->base + MTK_SENINF_CSI2_CAL_CTRL(pad);
	u32 sta0;
	u32 sta1;

	/* @0x163ca..0x163fe: lane/deskew control bits, set one by one */
	writel(readl(priv->base + MTK_SENINF_CSI2_LANE_CTRL(pad)) |
	       MTK_SENINF_CSI2_LANE_MASK,
	       priv->base + MTK_SENINF_CSI2_LANE_CTRL(pad));

	if (pad != 1) {
		/* @0x1640a / @0x16ca0 */
		writel(readl(cal) | MTK_SENINF_CSI2_CAL_START, cal);
		/* @0x16412 / @0x16caa */
		writel(MTK_SENINF_CSI2_CAL_SEQ_VAL,
		       priv->base + MTK_SENINF_CSI2_CAL_SEQ(pad));
	}

	/* @0x16422 / @0x16894 / @0x16cb8 */
	writel(readl(cal) | MTK_SENINF_CSI2_CAL_MODE, cal);

	usleep_range(500, 600);

	sta0 = readl(priv->base + MTK_SENINF_CSI2_CAL_STA0(pad));
	sta1 = readl(priv->base + MTK_SENINF_CSI2_CAL_STA1(pad));
	if (!(sta0 & MTK_SENINF_CSI2_CAL_STA0_MASK) ||
	    !(sta1 & MTK_SENINF_CSI2_CAL_STA1_MASK))
		dev_warn(priv->dev,
			 "pad%u: CSI-2 calibration status 0x%08x/0x%08x\n",
			 pad, sta0, sta1);

	if (pad != 1) {
		/* @0x1646a / @0x16d00 */
		writel(readl(cal) & ~MTK_SENINF_CSI2_CAL_START, cal);
	}

	/* @0x1647a..0x164bc: clear the lane/deskew control bits */
	writel(readl(priv->base + MTK_SENINF_CSI2_LANE_CTRL(pad)) &
	       ~MTK_SENINF_CSI2_LANE_MASK,
	       priv->base + MTK_SENINF_CSI2_LANE_CTRL(pad));
}

/*
 * Stream-on programming for one input, SeninfDrvImp::setSeninf1NCSI2
 * @0x16282..0x166dc with NCSI2_en == 1 and MIPI_OPHY_NCSI2.
 */
static void mtk_seninf_csi2_enable(struct mtk_seninf *priv, unsigned int pad,
				   const struct mtk_seninf_link *link)
{
	u32 lane_mask = (1U << link->lanes) - 1;
	u32 settle = mtk_seninf_settle_count(MTK_SENINF_SETTLE_DEFAULT_NS);
	u32 ctrl;

	/* MIPI D-PHY / MIPI RX first, in the vendor's order */
	mtk_seninf_mipi_rx_enable(priv, pad);

	mtk_seninf_csi2_calibrate(priv, pad);

	/* @0x1651e: DPCM dependent value, 0 for DPCM_INFO outside 42..49 */
	writel(0, priv->base + MTK_SENINF_CSI2_DPCM(pad));

	/* @0x16526..0x165a4: CSI-2 mode, bits 15:12 of SENINFn_CTRL */
	ctrl = readl(priv->base + MTK_SENINF_CAM_CTRL(pad));
	ctrl &= ~MTK_SENINF_CTRL_SRC;
	writel(ctrl | MTK_SENINF_CTRL_CSI2_MODE,
	       priv->base + MTK_SENINF_CAM_CTRL(pad));

	/* @0x165a8..0x165b4 */
	writel(readl(priv->base + MTK_SENINF_CSI2_CTRL(pad)) &
	       ~MTK_SENINF_CSI2_CTRL_CLR,
	       priv->base + MTK_SENINF_CSI2_CTRL(pad));

	/* @0x165b8: settle time in SENINF clock cycles, bits 15:8 */
	writel((settle << 8) & 0xffff,
	       priv->base + MTK_SENINF_CSI2_SETTLE(pad));

	/* @0x165be..0x165d8: enable, high speed mode, ECC order, lane count */
	writel(readl(priv->base + MTK_SENINF_CSI2_CTRL(pad)) |
	       MTK_SENINF_CSI2_CTRL_EN | MTK_SENINF_CSI2_CTRL_HS |
	       (MTK_SENINF_ECC_ORDER << 16) |
	       (lane_mask & MTK_SENINF_CSI2_CTRL_LANES),
	       priv->base + MTK_SENINF_CSI2_CTRL(pad));

	/* @0x165dc */
	writel(MTK_SENINF_CSI2_CTRL2_MAGIC,
	       priv->base + MTK_SENINF_CSI2_CTRL2(pad));

	/*
	 * @0x165e0..0x165f0: set and immediately clear bits 1:0 of
	 * SENINFn_MUX_CTRL.  The vendor reads the register once and writes
	 * both values back to back, so the two writes are reproduced here.
	 */
	ctrl = readl(priv->base + MTK_SENINF_CAM_MUX_CTRL(pad));
	writel(ctrl | GENMASK(1, 0), priv->base + MTK_SENINF_CAM_MUX_CTRL(pad));
	writel(ctrl & ~GENMASK(1, 0), priv->base + MTK_SENINF_CAM_MUX_CTRL(pad));

	dev_dbg(priv->dev,
		"pad%u: CSI-2 on, %u lane(s), settle %u ns (%u cycles), freq %lld Hz\n",
		pad, link->lanes, MTK_SENINF_SETTLE_DEFAULT_NS, settle,
		link->link_freq);
}

/* Stream-off for one input, SeninfDrvImp::setSeninf1NCSI2 @0x165f6..0x166b4 */
static void mtk_seninf_csi2_disable(struct mtk_seninf *priv, unsigned int pad)
{
	writel(readl(priv->base + MTK_SENINF_CSI2_CTRL(pad)) &
	       ~MTK_SENINF_CSI2_CTRL_LANES,
	       priv->base + MTK_SENINF_CSI2_CTRL(pad));
	writel(readl(priv->base + MTK_SENINF_CSI2_CSI2_CFG(pad)) &
	       ~MTK_SENINF_CSI2_CTRL_LANES,
	       priv->base + MTK_SENINF_CSI2_CSI2_CFG(pad));

	mtk_seninf_mipi_rx_disable(priv, pad);

	dev_dbg(priv->dev, "pad%u: CSI-2 off\n", pad);
}

static int mtk_seninf_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_seninf *priv = to_mtk_seninf(sd);
	unsigned int i;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (enable == priv->streaming) {
		mutex_unlock(&priv->lock);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(priv->dev);
		if (ret < 0) {
			dev_err(priv->dev, "failed to resume: %d\n", ret);
			goto out_unlock;
		}

		/*
		 * s_stream() carries no pad argument, so every sink pad that has
		 * a sensor bound to it is brought up; each SENINF instance has
		 * its own register set and its own sensor's link parameters, so
		 * this is exactly the set of active inputs of this bridge.
		 */
		for (i = 0; i < priv->num_sink_pads; i++) {
			struct mtk_seninf_link link;

			ret = mtk_seninf_get_link(priv, i, &link);
			if (ret == -ENODEV)
				continue;
			if (ret)
				goto err_disable;
			if (!link.lanes) {
				ret = -EINVAL;
				goto err_disable;
			}

			mtk_seninf_csi2_enable(priv, i, &link);
			priv->streaming_pads |= BIT(i);
		}

		mtk_seninf_check_errors(priv, "stream on");
		priv->streaming = true;
	} else {
		for (i = 0; i < priv->num_sink_pads; i++) {
			if (!(priv->streaming_pads & BIT(i)))
				continue;
			mtk_seninf_csi2_disable(priv, i);
		}
		priv->streaming_pads = 0;

		mtk_seninf_check_errors(priv, "stream off");
		priv->streaming = false;
		pm_runtime_put(priv->dev);
	}

	mutex_unlock(&priv->lock);
	return 0;

err_disable:
	for (i = 0; i < priv->num_sink_pads; i++) {
		if (!(priv->streaming_pads & BIT(i)))
			continue;
		mtk_seninf_csi2_disable(priv, i);
	}
	priv->streaming_pads = 0;
	pm_runtime_put(priv->dev);
out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops mtk_seninf_video_ops = {
	/*
	 * Classic single-interface streaming hook, invoked through
	 * v4l2_subdev_call(sd, video, s_stream, enable).  The newer per-pad
	 * enable_streams/disable_streams pad operations require subdev state and
	 * routing, which this bridge without a capture pipeline does not have.
	 */
	.s_stream = mtk_seninf_s_stream,
};

static const struct v4l2_subdev_ops mtk_seninf_subdev_ops = {
	.video = &mtk_seninf_video_ops,
};

/*
 * Async binding
 */
static int mtk_seninf_notifier_bound(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *subdev,
				     struct v4l2_async_connection *asc)
{
	struct mtk_seninf *priv = container_of(notifier, struct mtk_seninf,
					       notifier);
	struct mtk_seninf_asc *sasc = container_of(asc, struct mtk_seninf_asc,
						   asc);
	unsigned int src_pad;
	unsigned int i;
	int ret;

	for (i = 0, src_pad = 0; i < subdev->entity.num_pads; i++) {
		if (subdev->entity.pads[i].flags & MEDIA_PAD_FL_SOURCE) {
			src_pad = i;
			break;
		}
	}
	if (i == subdev->entity.num_pads) {
		dev_err(priv->dev, "%s has no source pad\n", subdev->name);
		return -EINVAL;
	}

	ret = media_create_pad_link(&subdev->entity, src_pad, &priv->sd.entity,
				    sasc->pad,
				    MEDIA_LNK_FL_IMMUTABLE | MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(priv->dev, "failed to link %s to pad%u: %d\n",
			subdev->name, sasc->pad, ret);
		return ret;
	}

	dev_info(priv->dev, "bound %s to pad%u\n", subdev->name, sasc->pad);

	return 0;
}

static int mtk_seninf_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_seninf *priv = container_of(notifier, struct mtk_seninf,
					       notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&priv->v4l2_dev);
	if (ret) {
		dev_err(priv->dev, "failed to register subdev nodes: %d\n", ret);
		return ret;
	}

	dev_info(priv->dev, "all sensor subdevices bound\n");

	return 0;
}

static const struct v4l2_async_notifier_operations mtk_seninf_notifier_ops = {
	.bound = mtk_seninf_notifier_bound,
	.complete = mtk_seninf_notifier_complete,
};

static int mtk_seninf_register_notifier(struct mtk_seninf *priv)
{
	struct device_node *ep;
	unsigned int count = 0;
	int ret;

	v4l2_async_nf_init(&priv->notifier, &priv->v4l2_dev);
	priv->notifier.ops = &mtk_seninf_notifier_ops;

	for_each_endpoint_of_node(priv->dev->of_node, ep) {
		struct mtk_seninf_asc *sasc;
		struct of_endpoint of_ep;

		ret = of_graph_parse_endpoint(ep, &of_ep);
		if (ret) {
			dev_err(priv->dev, "failed to parse %pOF: %d\n", ep, ret);
			of_node_put(ep);
			goto err_cleanup;
		}

		if (of_ep.port >= MTK_SENINF_NUM_CAMS) {
			dev_err(priv->dev, "%pOF: port %u out of range\n",
				ep, of_ep.port);
			of_node_put(ep);
			ret = -EINVAL;
			goto err_cleanup;
		}

		sasc = (struct mtk_seninf_asc *)
			v4l2_async_nf_add_fwnode_remote(&priv->notifier,
							of_fwnode_handle(ep),
							struct mtk_seninf_asc);
		if (IS_ERR(sasc)) {
			ret = PTR_ERR(sasc);
			dev_err(priv->dev, "failed to add %pOF: %d\n", ep, ret);
			of_node_put(ep);
			goto err_cleanup;
		}

		sasc->pad = of_ep.port;
		count++;
	}

	if (!count) {
		dev_err(priv->dev, "no sensor endpoint described in DT\n");
		ret = -ENODEV;
		goto err_cleanup;
	}

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret) {
		dev_err(priv->dev, "failed to register async notifier: %d\n", ret);
		goto err_cleanup;
	}

	return 0;

err_cleanup:
	v4l2_async_nf_cleanup(&priv->notifier);
	return ret;
}

/*
 * Debugfs
 *
 * MIPIRX_CONFIG and MIPIRX_ANALOG are owned by the proprietary "seninf_drv"
 * userspace component; the GPL drop contains no programming sequence for them,
 * so they are mapped but only exposed through a read-only dump.
 */
static void mtk_seninf_dump_window(struct seq_file *s, const char *name,
				   void __iomem *base, unsigned int size)
{
	unsigned int off;

	seq_printf(s, "%s (%u bytes)\n", name, size);
	for (off = 0; off < size; off += 4)
		seq_printf(s, "  %04x: %08x\n", off, readl(base + off));
}

static int mtk_seninf_regs_show(struct seq_file *s, void *unused)
{
	struct mtk_seninf *priv = s->private;
	int ret;

	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0)
		return ret;

	mtk_seninf_dump_window(s, "SENINF", priv->base, MTK_SENINF_REG_RANGE);
	mtk_seninf_dump_window(s, "MIPIRX_CONFIG", priv->mipi_rx,
			       MTK_SENINF_MIPIRX_REG_RANGE);
	mtk_seninf_dump_window(s, "MIPIRX_ANALOG", priv->mipi_ana,
			       MTK_SENINF_MIPI_ANA_RANGE);

	pm_runtime_put(priv->dev);

	return 0;
}

static int mtk_seninf_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtk_seninf_regs_show, inode->i_private);
}

static const struct file_operations mtk_seninf_regs_fops = {
	.owner = THIS_MODULE,
	.open = mtk_seninf_regs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void mtk_seninf_debugfs_init(struct mtk_seninf *priv)
{
	priv->debugfs = debugfs_create_dir(MTK_SENINF_NAME, NULL);
	if (IS_ERR_OR_NULL(priv->debugfs)) {
		dev_dbg(priv->dev, "debugfs not available\n");
		priv->debugfs = NULL;
		return;
	}

	debugfs_create_file("regs", 0444, priv->debugfs, priv,
			    &mtk_seninf_regs_fops);
}

/*
 * Probe / remove
 */
static int mtk_seninf_init_entity(struct mtk_seninf *priv)
{
	struct device_node *ep;
	unsigned int num_sink = 0;
	unsigned int i;
	int ret;

	for_each_endpoint_of_node(priv->dev->of_node, ep) {
		struct of_endpoint of_ep;

		ret = of_graph_parse_endpoint(ep, &of_ep);
		if (ret)
			continue;

		if (of_ep.port >= MTK_SENINF_NUM_CAMS)
			continue;

		num_sink = max(num_sink, of_ep.port + 1);
	}

	if (!num_sink)
		return -ENODEV;

	priv->num_sink_pads = num_sink;
	priv->pads = devm_kcalloc(priv->dev, num_sink + 1, sizeof(*priv->pads),
				  GFP_KERNEL);
	if (!priv->pads)
		return -ENOMEM;

	for (i = 0; i < num_sink; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;
	priv->pads[num_sink].flags = MEDIA_PAD_FL_SOURCE;

	v4l2_subdev_init(&priv->sd, &mtk_seninf_subdev_ops);
	priv->sd.dev = priv->dev;
	priv->sd.owner = THIS_MODULE;
	priv->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	strscpy(priv->sd.name, MTK_SENINF_NAME, sizeof(priv->sd.name));
	v4l2_set_subdevdata(&priv->sd, priv);

	ret = media_entity_pads_init(&priv->sd.entity, num_sink + 1, priv->pads);
	if (ret)
		return ret;

	return v4l2_device_register_subdev(&priv->v4l2_dev, &priv->sd);
}

static int mtk_seninf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_seninf *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return dev_err_probe(dev, PTR_ERR(priv->base),
				     "failed to map SENINF registers\n");

	priv->mipi_rx = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->mipi_rx))
		return dev_err_probe(dev, PTR_ERR(priv->mipi_rx),
				     "failed to map MIPIRX_CONFIG registers\n");

	priv->mipi_ana = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(priv->mipi_ana))
		return dev_err_probe(dev, PTR_ERR(priv->mipi_ana),
				     "failed to map MIPIRX_ANALOG registers\n");

	priv->camtg = devm_clk_get(dev, "camtg");
	if (IS_ERR(priv->camtg))
		return dev_err_probe(dev, PTR_ERR(priv->camtg),
				     "failed to get camtg clock\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	priv->mdev.dev = dev;
	strscpy(priv->mdev.model, "MT6735 SENINF", sizeof(priv->mdev.model));
	media_device_init(&priv->mdev);
	priv->v4l2_dev.mdev = &priv->mdev;

	ret = v4l2_device_register(dev, &priv->v4l2_dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register v4l2 device\n");
		goto err_media_cleanup;
	}

	ret = media_device_register(&priv->mdev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register media device\n");
		goto err_v4l2_unregister;
	}

	ret = mtk_seninf_init_entity(priv);
	if (ret) {
		if (ret == -ENODEV)
			dev_err_probe(dev, ret,
				      "no sensor endpoints described in DT\n");
		else
			dev_err_probe(dev, ret, "failed to init subdevice\n");
		goto err_media_unregister;
	}

	ret = mtk_seninf_register_mclks(priv);
	if (ret)
		goto err_subdev_unregister;

	ret = mtk_seninf_register_notifier(priv);
	if (ret)
		goto err_subdev_unregister;

	mtk_seninf_debugfs_init(priv);

	dev_info(dev, "%u sink pad(s), %u MCLK output(s)\n",
		 priv->num_sink_pads, MTK_SENINF_NUM_MCLKS);

	return 0;

err_subdev_unregister:
	v4l2_device_unregister_subdev(&priv->sd);
err_media_unregister:
	media_device_unregister(&priv->mdev);
err_v4l2_unregister:
	v4l2_device_unregister(&priv->v4l2_dev);
err_media_cleanup:
	media_device_cleanup(&priv->mdev);
	mutex_destroy(&priv->lock);
	return ret;
}

static void mtk_seninf_remove(struct platform_device *pdev)
{
	struct mtk_seninf *priv = platform_get_drvdata(pdev);
	unsigned int i;

	if (priv->streaming) {
		for (i = 0; i < priv->num_sink_pads; i++) {
			if (priv->streaming_pads & BIT(i))
				mtk_seninf_csi2_disable(priv, i);
		}
		priv->streaming_pads = 0;
		priv->streaming = false;
		pm_runtime_put(priv->dev);
	}

	debugfs_remove_recursive(priv->debugfs);
	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_device_unregister_subdev(&priv->sd);
	media_device_unregister(&priv->mdev);
	v4l2_device_unregister(&priv->v4l2_dev);
	media_device_cleanup(&priv->mdev);
	mutex_destroy(&priv->lock);
}

static const struct of_device_id mtk_seninf_of_match[] = {
	{ .compatible = "mediatek,mt6735-seninf" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_seninf_of_match);

static struct platform_driver mtk_seninf_driver = {
	.probe = mtk_seninf_probe,
	.remove = mtk_seninf_remove,
	.driver = {
		.name = MTK_SENINF_NAME,
		.of_match_table = mtk_seninf_of_match,
		.pm = pm_ptr(&mtk_seninf_pm_ops),
	},
};
module_platform_driver(mtk_seninf_driver);

MODULE_DESCRIPTION("MediaTek MT6735 SENINF driver");
MODULE_LICENSE("GPL");
