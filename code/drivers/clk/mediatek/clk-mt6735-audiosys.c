// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 *
 * Port from Samsung/MTK downstream drivers/clk/mediatek/clk-mt6735.c:1005-1027
 * (aud_cg_regs, audio_clks) and :1284-1307 (mt_audiosys_init, CLK_OF_DECLARE).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6735-audiosys.h>

/*
 * Downstream models the audio gate bank as a single register: set, clear and
 * status all live at offset 0x0 (clk-mt6735.c:1005-1009, aud_cg_regs), i.e.
 * AUDIO_TOP_CON0 in the AFE register space (mt6735-afe-regs.h:24). All
 * entries use CLK_GATE_NO_SETCLR_REG, which in the downstream gate code
 * (clk-gate-v1.c:52-73) means read-modify-write of the bit in the status
 * register: enable clears the bit, disable sets it, is_enabled tests the bit
 * being clear. That is exactly Nura's mtk_clk_gate_ops_no_setclr
 * (clk-gate.c:117-133, 162-199); do not substitute the setclr variant.
 *
 * The MMIO region (0x11220000/0x10000) is shared with the AFE driver, which
 * maps it with devm_platform_ioremap_resource() and therefore owns the
 * request_mem_region() claim. mtk_clk_simple_probe() with a gate-only
 * descriptor does not ioremap the node: mtk_clk_register_gates() obtains its
 * regmap through device_node_to_regmap(), which instantiates it with
 * of_iomap() (drivers/mfd/syscon.c:61) without claiming the resource. Both
 * drivers can therefore probe, and both only read-modify-write individual
 * bits of AUDIO_TOP_CON0, as downstream did.
 */
static const struct mtk_gate_regs aud_cg_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0000,
	.sta_ofs = 0x0000,
};

/*
 * CLK_IGNORE_UNUSED matches downstream clk-gate-v1.c:172 so the audio gates
 * left running by the bootloader are not disabled by clk_disable_unused()
 * before the AFE driver has taken them.
 */
static const struct mtk_gate audiosys_gates[] = {
	GATE_MTK_FLAGS(AUDIO_AFE, "audio_afe", "aud_intbus_sel",
		       &aud_cg_regs, 2, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_I2S, "audio_i2s", "aud_intbus_sel",
		       &aud_cg_regs, 6, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_22M, "audio_22m", "aud_intbus_sel",
		       &aud_cg_regs, 8, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_24M, "audio_24m", "aud_intbus_sel",
		       &aud_cg_regs, 9, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_APLL2_TUNER, "audio_apll2_tuner", "aud_intbus_sel",
		       &aud_cg_regs, 18, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_APLL_TUNER, "audio_apll_tuner", "aud_intbus_sel",
		       &aud_cg_regs, 19, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_ADC, "audio_adc", "aud_intbus_sel",
		       &aud_cg_regs, 24, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_DAC, "audio_dac", "aud_intbus_sel",
		       &aud_cg_regs, 25, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_DAC_PREDIS, "audio_dac_predis", "aud_intbus_sel",
		       &aud_cg_regs, 26, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(AUDIO_TML, "audio_tml", "aud_intbus_sel",
		       &aud_cg_regs, 27, &mtk_clk_gate_ops_no_setclr,
		       CLK_IGNORE_UNUSED),
};

static const struct mtk_clk_desc audiosys_clks = {
	.clks = audiosys_gates,
	.num_clks = ARRAY_SIZE(audiosys_gates),
};

static const struct of_device_id of_match_mt6735_audiosys[] = {
	{ .compatible = "mediatek,mt6735-audiosys", .data = &audiosys_clks },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt6735_audiosys = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6735-audiosys",
		.of_match_table = of_match_mt6735_audiosys,
	},
};
module_platform_driver(clk_mt6735_audiosys);

MODULE_AUTHOR("James Liao <jamesjj.liao@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT6735 audiosys clock driver");
MODULE_LICENSE("GPL");
