// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN SOC_CONSYS firmware bring-up: version detection, WMT init
 * scripts and firmware patch download.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_ic_soc.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The bring-up sequence is the one the downstream driver runs on MT6735/
 * MT6737T (CFG_WMT_POWER_ON_DLM, CFG_WMT_MULTI_PATCH, CFG_WMT_PATCH_DL_OPTM,
 * CFG_WMT_FILTER_MODE_SETTING all enabled):
 *
 *	query STP defaults -> set STP options -> switch host STP to full mode
 *	-> power-on DLM patch -> patch download (one PDU fragment chain per
 *	patch file) -> WMT reset -> RF calibration -> coexistence -> FM strap
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/string.h>

#include "wmt_local.h"

#define DEFAULT_PATCH_FRAG_SIZE	1000
#define WMT_PATCH_FRAG_1ST	0x1
#define WMT_PATCH_FRAG_MID	0x2
#define WMT_PATCH_FRAG_LAST	0x3

/* WMT_STRAP_CONF_CMD / EVT for the FM interface (wmt_ic_soc.c:255-257). */
static const u8 wmt_query_stp_cmd[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
static const u8 wmt_query_stp_evt_default[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00,
};
static const u8 wmt_query_stp_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01,
};
static const u8 wmt_set_stp_cmd[] = {
	0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01,
};
static const u8 wmt_set_stp_evt[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
static const u8 wmt_reset_cmd[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
static const u8 wmt_reset_evt[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
static const u8 wmt_patch_evt[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
static const u8 wmt_strap_fm_cmd[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
static const u8 wmt_strap_fm_evt[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };
static const u8 wmt_coex_cmd[] = { 0x01, 0x10, 0x02, 0x00, 0x01, 0x00 };
static const u8 wmt_coex_evt[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
static const u8 wmt_co_clock_cmd[] = { 0x01, 0x0a, 0x02, 0x00, 0x08, 0x03 };
static const u8 wmt_co_clock_evt[] = { 0x02, 0x0a, 0x01, 0x00, 0x00 };
static const u8 wmt_rf_calibration_cmd[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
static const u8 wmt_rf_calibration_evt[] = {
	0x02, 0x14, 0x02, 0x00, 0x00, 0x01,
};
static const u8 wmt_bt_tssi_cmd[] = {
	0x01, 0x02, 0x04, 0x00, 0x0d, 0x01, 0x1e, 0x00,
};
static const u8 wmt_bt_tssi_evt[] = { 0x02, 0x02, 0x01, 0x00, 0x00 };
static const u8 wmt_power_ctrl_dlm_cmd1[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x0f, 0x00, 0x00,
};
static const u8 wmt_power_ctrl_dlm_cmd2[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xf0, 0x00, 0x00, 0x00,
};
static const u8 wmt_power_ctrl_dlm_cmd3[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x60, 0x00, 0x10, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x00, 0x00, 0x00,
};
static const u8 wmt_power_ctrl_dlm_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const u8 wmt_set_mcu_clk_en_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x34, 0x03, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_set_mcu_clk_138_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x59, 0x4d, 0x84, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_set_mcu_clk_26_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x0c, 0x01, 0x00, 0x80, 0x00, 0x4d, 0x84, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_set_mcu_clk_dis_cmd[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	0x34, 0x03, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_set_mcu_clk_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* Patch RAM address programming PDUs (wmt_ic_soc.c:214-233). */
static const u8 wmt_patch_address_cmd[] = {
	0x01, 0x08, 0x10, 0x00,
	0x01, 0x01, 0x00, 0x01,
	0x3c, 0x02, 0x09, 0x02,
	0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_patch_address_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const u8 wmt_patch_p_address_cmd[] = {
	0x01, 0x08, 0x10, 0x00,
	0x01, 0x01, 0x00, 0x01,
	0xc4, 0x04, 0x09, 0x02,
	0x00, 0x3f, 0x00, 0x01,
	0xff, 0xff, 0xff, 0xff,
};
static const u8 wmt_patch_p_address_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* wmt_ic_soc.c:508-650 */
static const struct wmt_init_script query_stp_script[] = {
	{ wmt_query_stp_cmd, sizeof(wmt_query_stp_cmd),
	  wmt_query_stp_evt_default, sizeof(wmt_query_stp_evt_default),
	  "query stp default" },
};
static const struct wmt_init_script query_stp_full_script[] = {
	{ wmt_query_stp_cmd, sizeof(wmt_query_stp_cmd),
	  wmt_query_stp_evt, sizeof(wmt_query_stp_evt), "query stp" },
};
static const struct wmt_init_script set_stp_script[] = {
	{ wmt_set_stp_cmd, sizeof(wmt_set_stp_cmd),
	  wmt_set_stp_evt, sizeof(wmt_set_stp_evt), "set stp" },
};
static const struct wmt_init_script wmt_reset_script[] = {
	{ wmt_reset_cmd, sizeof(wmt_reset_cmd),
	  wmt_reset_evt, sizeof(wmt_reset_evt), "wmt reset" },
};
static const struct wmt_init_script calibration_script[] = {
	{ wmt_rf_calibration_cmd, sizeof(wmt_rf_calibration_cmd),
	  wmt_rf_calibration_evt, sizeof(wmt_rf_calibration_evt),
	  "start RF calibration" },
};
static const struct wmt_init_script coex_script[] = {
	{ wmt_coex_cmd, sizeof(wmt_coex_cmd),
	  wmt_coex_evt, sizeof(wmt_coex_evt), "coex wmt" },
};
static const struct wmt_init_script co_clock_script[] = {
	{ wmt_co_clock_cmd, sizeof(wmt_co_clock_cmd),
	  wmt_co_clock_evt, sizeof(wmt_co_clock_evt), "osc type" },
};
static const struct wmt_init_script power_on_dlm_script[] = {
	{ wmt_power_ctrl_dlm_cmd1, sizeof(wmt_power_ctrl_dlm_cmd1),
	  wmt_power_ctrl_dlm_evt, sizeof(wmt_power_ctrl_dlm_evt),
	  "power on dlm cmd1" },
	{ wmt_power_ctrl_dlm_cmd2, sizeof(wmt_power_ctrl_dlm_cmd2),
	  wmt_power_ctrl_dlm_evt, sizeof(wmt_power_ctrl_dlm_evt),
	  "power on dlm cmd2" },
	{ wmt_power_ctrl_dlm_cmd3, sizeof(wmt_power_ctrl_dlm_cmd3),
	  wmt_power_ctrl_dlm_evt, sizeof(wmt_power_ctrl_dlm_evt),
	  "power on dlm cmd3" },
};
static const struct wmt_init_script mcu_clk_high_script[] = {
	{ wmt_set_mcu_clk_en_cmd, sizeof(wmt_set_mcu_clk_en_cmd),
	  wmt_set_mcu_clk_evt, sizeof(wmt_set_mcu_clk_evt),
	  "enable set mcu clk" },
	{ wmt_set_mcu_clk_138_cmd, sizeof(wmt_set_mcu_clk_138_cmd),
	  wmt_set_mcu_clk_evt, sizeof(wmt_set_mcu_clk_evt),
	  "set mcu clk to 138.67MHz" },
};
static const struct wmt_init_script mcu_clk_low_script[] = {
	{ wmt_set_mcu_clk_26_cmd, sizeof(wmt_set_mcu_clk_26_cmd),
	  wmt_set_mcu_clk_evt, sizeof(wmt_set_mcu_clk_evt),
	  "set mcu clk to 26MHz" },
	{ wmt_set_mcu_clk_dis_cmd, sizeof(wmt_set_mcu_clk_dis_cmd),
	  wmt_set_mcu_clk_evt, sizeof(wmt_set_mcu_clk_evt),
	  "disable set mcu clk" },
};

/* wmt_ic_soc.c:790-830 */
struct wmt_ic_info {
	u32 hw_ver;
	const char *chip_name;
	const char *chip_version;
	const char *patch_ext;
	enum mtk_wcn_wmt_hwver e_wmt_hwver;
	bool psm_support;
};

static const struct wmt_ic_info wmt_soc_info_table[] = {
	{
		.hw_ver = 0x8a00,
		.chip_name = "SOC_CONSYS",
		.chip_version = "E1",
		.patch_ext = "_e1",
		.e_wmt_hwver = WMTHWVER_E1,
		.psm_support = true,
	},
	{
		.hw_ver = 0x8a01,
		.chip_name = "SOC_CONSYS",
		.chip_version = "E2",
		.patch_ext = "_e1",
		.e_wmt_hwver = WMTHWVER_E2,
		.psm_support = true,
	},
	{
		.hw_ver = 0x8b01,
		.chip_name = "SOC_CONSYS",
		.chip_version = "E3",
		.patch_ext = "_e1",
		.e_wmt_hwver = WMTHWVER_E3,
		.psm_support = true,
	},
};

static const struct wmt_ic_info *soc_info;
static int soc_co_clock;	/* WMT_CO_CLOCK_EN / DIS */

/* wmt_ic_soc.c:1560-1605 */
static const struct wmt_ic_info *wmt_soc_find_info(u32 hw_ver)
{
	int i;

	for (i = ARRAY_SIZE(wmt_soc_info_table) - 1; i >= 0; i--)
		if (hw_ver == wmt_soc_info_table[i].hw_ver)
			return &wmt_soc_info_table[i];

	for (i = ARRAY_SIZE(wmt_soc_info_table) - 1; i >= 0; i--)
		if (MAJORNUM(hw_ver) ==
		    MAJORNUM(wmt_soc_info_table[i].hw_ver))
			return &wmt_soc_info_table[i];

	wmt_err("no IC info for hw version 0x%x, assuming E1\n", hw_ver);
	return &wmt_soc_info_table[0];
}

/* wmt_ic_soc.c:1512-1558 */
int wmt_ic_soc_ver_check(void)
{
	u32 hw_ver = 0, fw_ver = 0;
	unsigned long pa1, pa2;
	const struct wmt_ic_info *info;
	int ret;

	ret = wmt_core_reg_rw_raw(false, GEN_HVR, &hw_ver, GEN_VER_MASK);
	if (ret) {
		wmt_err("failed to read hw version (%d)\n", ret);
		return ret;
	}

	ret = wmt_core_reg_rw_raw(false, GEN_FVR, &fw_ver, GEN_VER_MASK);
	if (ret) {
		wmt_err("failed to read fw version (%d)\n", ret);
		return ret;
	}

	info = wmt_soc_find_info(hw_ver);
	wmt_info("IC %s.%s hw 0x%x fw 0x%x patch ext %s\n",
		 info->chip_name, info->chip_version, hw_ver, fw_ver,
		 info->patch_ext);

	pa1 = ((u32)g_wmt.chip_id << 16) | (hw_ver & 0xffff);
	pa2 = ((u32)info->e_wmt_hwver << 16) | (fw_ver & 0xffff);
	ret = wmt_core_ctrl(WMT_CTRL_HWIDVER_SET, &pa1, &pa2);
	if (ret)
		wmt_warn("failed to store chip information (%d)\n", ret);

	soc_info = info;
	return 0;
}

int wmt_ic_soc_co_clock_ctrl(int on)
{
	if (on != 0 && on != 1)
		return -EINVAL;

	soc_co_clock = on;
	wmt_dbg("co-clock %s\n", on ? "enabled" : "disabled");
	return 0;
}

/* wmt_ic_soc.c:1473-1600 (AIF and GPS_SYNC chip-side pin control) */
int wmt_ic_soc_pin_ctrl(enum wmt_ic_pin_id id, enum wmt_ic_pin_state state,
			u32 flag)
{
	u32 val;
	int ret = 0;

	switch (id) {
	case WMT_IC_PIN_AUDIO:
		/* wmt_ic_soc.c:1308-1420 */
		switch (state) {
		case WMT_IC_AIF_2:
			val = 0x01110000;
			ret = wmt_core_reg_rw_raw(true, 0x80050078, &val,
						  0x0fff0000);
			break;
		case WMT_IC_AIF_0:
		case WMT_IC_AIF_1:
		case WMT_IC_AIF_3:
			ret = 0;
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;
	case WMT_IC_PIN_GSYNC:
		/* wmt_ic_soc.c:1426-1445 */
		val = state == WMT_IC_PIN_MUX ? (0x1 << 28) : (0x5 << 28);
		ret = wmt_core_reg_rw_raw(true, 0x80050078, &val,
					  0x7 << 28);
		break;
	case WMT_IC_PIN_EEDI:
	case WMT_IC_PIN_EEDO:
		/* Chip-side GPS LNA control is not ported (wmt_ic_soc.c:1447-1471
		 * marks these pins TBD in the downstream driver as well).
		 */
		wmt_warn("chip-side pin %d control is not supported\n", id);
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/*
 * Firmware patch download
 */

int wmt_ic_soc_patch_info_prepare(void)
{
	struct wmt_ctrl_data data = { };

	data.ctrl_id = WMT_CTRL_PATCH_SEARCH;
	return wmt_ctrl(&data);
}

/* wmt_ic_soc.c:1937-2188 */
int wmt_ic_soc_patch_download(u32 index)
{
	struct wmt_ctrl_data data = { };
	u8 patch_name[NAME_MAX + 1] = { };
	u8 address[4] = { };
	const struct wmt_patch *hdr;
	u8 *patch_buf = NULL;
	size_t patch_size = 0;
	u8 *pbuf;
	u32 frag_seq, frag_num, patch_size_per_frag = DEFAULT_PATCH_FRAG_SIZE;
	u16 hw_ver, sw_ver;
	u16 frag_size = 0, cmd_len;
	u32 offset;
	u32 written = 0, read = 0;
	u8 evt[16];
	u8 cmd[sizeof(wmt_patch_address_cmd)];
	u8 patch_cmd[5] = { 0x01, 0x01, 0x00, 0x00, 0x00 };
	int ret;

	if (!soc_info)
		return -EINVAL;

	/* <1> name and patch RAM address of this patch file */
	data.ctrl_id = WMT_CTRL_GET_PATCH_INFO;
	data.data[0] = index + 1;
	data.data[1] = (unsigned long)patch_name;
	data.data[2] = (unsigned long)address;
	ret = wmt_ctrl(&data);
	if (ret)
		return ret;

	/* <2> load the patch file by the name given by the launcher */
	data.ctrl_id = WMT_CTRL_GET_PATCH;
	data.data[0] = 0;
	data.data[1] = (unsigned long)patch_name;
	data.data[2] = (unsigned long)&patch_buf;
	data.data[3] = (unsigned long)&patch_size;
	ret = wmt_ctrl(&data);
	if (ret) {
		wmt_err("failed to load patch %s (%d)\n", patch_name, ret);
		return ret;
	}

	if (patch_size < sizeof(*hdr)) {
		wmt_err("patch %s is too short (%zu)\n", patch_name, patch_size);
		ret = -EINVAL;
		goto out_put;
	}

	hdr = (const struct wmt_patch *)(patch_buf + BCNT_PATCH_BUF_HEADROOM);
	/* The 16-bit header fields are stored big endian; the downstream
	 * driver byte-swaps them when reporting (wmt_ic_soc.c:2006-2012).
	 */
	hw_ver = (hdr->hw_ver >> 8) | (hdr->hw_ver << 8);
	sw_ver = (hdr->sw_ver >> 8) | (hdr->sw_ver << 8);
	wmt_info("patch %s built %.*s, hw 0x%04x, sw 0x%04x, ver 0x%08x, plat %.4s\n",
		 patch_name, 15, (const char *)hdr->date_time,
		 hw_ver, sw_ver, hdr->patch_ver, (const char *)hdr->plat);

	if (!wmt_core_patch_check(hw_ver, soc_info->hw_ver))
		wmt_warn("patch %s hw version does not match the chip\n",
			 patch_name);

	/* Patch body starts after the 28-byte header; keep the payload
	 * writable so that the per-fragment WMT header can be prepended.
	 */
	patch_size -= sizeof(*hdr);
	pbuf = patch_buf + BCNT_PATCH_BUF_HEADROOM + sizeof(*hdr) -
	       sizeof(patch_cmd);

	/* The patch RAM base depends on the SoC (wmt_ic_soc.c:2010-2030). */
	memcpy(cmd, wmt_patch_address_cmd, sizeof(cmd));
	if (soc_info->hw_ver == 0x8a00 || soc_info->hw_ver == 0x8a01 ||
	    soc_info->hw_ver == 0x8b01) {
		/* MT6735/MT6737T class: patch RAM base 0x02090240 */
		cmd[8] = 0x40;
	}

	ret = wmt_core_tx(cmd, sizeof(cmd), &written);
	if (ret || written != sizeof(cmd)) {
		wmt_err("patch address command failed (%d)\n", ret);
		ret = -EIO;
		goto out_put;
	}
	memset(evt, 0, sizeof(evt));
	ret = wmt_core_rx(evt, sizeof(wmt_patch_address_evt), &read);
	if (ret || read != sizeof(wmt_patch_address_evt) ||
	    memcmp(evt, wmt_patch_address_evt,
		   sizeof(wmt_patch_address_evt))) {
		wmt_err("patch address event failed (%d)\n", ret);
		ret = -EIO;
		goto out_put;
	}

	memcpy(cmd, wmt_patch_p_address_cmd, sizeof(cmd));
	if (soc_info->hw_ver == 0x8a00 || soc_info->hw_ver == 0x8a01 ||
	    soc_info->hw_ver == 0x8b01)
		cmd[8] = 0xc8;
	memcpy(&cmd[12], address, sizeof(address));
	wmt_dbg("patch load address %02x %02x %02x %02x\n",
		address[0], address[1], address[2], address[3]);

	ret = wmt_core_tx(cmd, sizeof(cmd), &written);
	if (ret || written != sizeof(cmd)) {
		wmt_err("part patch address command failed (%d)\n", ret);
		ret = -EIO;
		goto out_put;
	}
	memset(evt, 0, sizeof(evt));
	ret = wmt_core_rx(evt, sizeof(wmt_patch_p_address_evt), &read);
	if (ret || read != sizeof(wmt_patch_p_address_evt) ||
	    memcmp(evt, wmt_patch_p_address_evt,
		   sizeof(wmt_patch_p_address_evt))) {
		wmt_err("part patch address event failed (%d)\n", ret);
		ret = -EIO;
		goto out_put;
	}

	/* <3> send the patch body in DEFAULT_PATCH_FRAG_SIZE fragments */
	frag_num = patch_size / patch_size_per_frag;
	frag_num += (frag_num * patch_size_per_frag) == patch_size ? 0 : 1;
	wmt_dbg("patch size %zu, fragments %u\n", patch_size, frag_num);

	offset = sizeof(patch_cmd);
	for (frag_seq = 0; frag_seq < frag_num; frag_seq++) {
		if (frag_seq == frag_num - 1) {
			frag_size = (u16)(patch_size -
					  frag_seq * patch_size_per_frag);
			patch_cmd[4] = WMT_PATCH_FRAG_LAST;
		} else {
			frag_size = patch_size_per_frag;
			patch_cmd[4] = frag_seq == 0 ? WMT_PATCH_FRAG_1ST
						     : WMT_PATCH_FRAG_MID;
		}

		cmd_len = 1 + frag_size;
		memcpy(&patch_cmd[2], &cmd_len, 2);
		memcpy(pbuf + offset - sizeof(patch_cmd), patch_cmd,
		       sizeof(patch_cmd));

		ret = wmt_core_tx(pbuf + offset - sizeof(patch_cmd),
				  frag_size + sizeof(patch_cmd), &written);
		if (ret || written != frag_size + sizeof(patch_cmd)) {
			wmt_err("patch fragment %u failed (%d, %u)\n",
				frag_seq, ret, written);
			ret = -EIO;
			break;
		}

		memset(evt, 0, sizeof(evt));
		ret = wmt_core_rx(evt, sizeof(wmt_patch_evt), &read);
		if (ret || read != sizeof(wmt_patch_evt) ||
		    memcmp(evt, wmt_patch_evt, sizeof(wmt_patch_evt))) {
			wmt_err("patch fragment %u event failed (%d)\n",
				frag_seq, ret);
			ret = -EIO;
			break;
		}

		offset += patch_size_per_frag;
	}

	if (frag_seq != frag_num) {
		wmt_err("patch %s download failed at fragment %u/%u\n",
			patch_name, frag_seq, frag_num);
		ret = -EIO;
	} else {
		wmt_info("patch %s download ok\n", patch_name);
		ret = 0;
	}

out_put:
	data.ctrl_id = WMT_CTRL_FREE_PATCH;
	data.data[0] = index + 1;
	wmt_ctrl(&data);

	return ret;
}

/* wmt_ic_soc.c:898-1290 */
int wmt_ic_soc_sw_init(struct wmt_hif_conf *hif)
{
	struct wmt_gen_conf *conf = wmt_lib_gen_conf();
	u8 strap_cmd[sizeof(wmt_strap_fm_cmd)];
	u8 strap_evt[sizeof(wmt_strap_fm_evt)];
	struct wmt_init_script strap_script[1];
	u8 tssi_cmd[sizeof(wmt_bt_tssi_cmd)];
	struct wmt_init_script tssi_script[1];
	unsigned long pa1, pa2;
	u32 patch_num, i;
	int ret;

	if (!soc_info || !hif) {
		wmt_err("SOC not identified yet\n");
		return -EINVAL;
	}

	if (hif->hif_type == WMT_HIF_BTIF) {
		/* 1. query chip STP defaults */
		ret = wmt_core_init_script(query_stp_script,
					   ARRAY_SIZE(query_stp_script));
		if (ret) {
			wmt_err("query STP defaults failed (%d)\n", ret);
			return ret;
		}

		/* 2. configure chip STP options */
		ret = wmt_core_init_script(set_stp_script,
					   ARRAY_SIZE(set_stp_script));
		if (ret) {
			wmt_err("set STP options failed (%d)\n", ret);
			return ret;
		}

		/* 3. switch the host to STP full mode */
		pa1 = WMT_STP_CONF_MODE;
		pa2 = MTKSTP_BTIF_FULL_MODE;
		ret = wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
		pa1 = WMT_STP_CONF_EN;
		pa2 = 1;
		ret += wmt_core_ctrl(WMT_CTRL_STP_CONF, &pa1, &pa2);
		if (ret) {
			wmt_err("failed to enable STP full mode (%d)\n", ret);
			return ret;
		}

		/* 4. the chip needs at least 2 ms to switch */
		msleep(10);

		/* 5. verify the chip STP options */
		ret = wmt_core_init_script(query_stp_full_script,
					   ARRAY_SIZE(query_stp_full_script));
		if (ret) {
			wmt_err("query STP options failed (%d)\n", ret);
			return ret;
		}
	}

	/* 6. power-on DLM patch */
	ret = wmt_core_init_script(power_on_dlm_script,
				   ARRAY_SIZE(power_on_dlm_script));
	if (ret)
		wmt_warn("power-on DLM patch failed (%d)\n", ret);

	/* 7. firmware patch download, one file at a time */
	ret = wmt_ic_soc_patch_info_prepare();
	if (ret) {
		wmt_err("patch info prepare failed (%d)\n", ret);
		return ret;
	}

	pa1 = 0;
	pa2 = 0;
	ret = wmt_core_ctrl(WMT_CTRL_GET_PATCH_NUM, &pa1, &pa2);
	patch_num = pa1;
	wmt_dbg("patch count %u\n", patch_num);
	if (!patch_num)
		wmt_warn("no patch registered by userspace, the firmware will run from ROM\n");

	ret = wmt_core_init_script(mcu_clk_high_script,
				   ARRAY_SIZE(mcu_clk_high_script));
	if (ret)
		wmt_warn("failed to raise the MCU clock (%d)\n", ret);

	for (i = 0; i < patch_num; i++) {
		ret = wmt_ic_soc_patch_download(i);
		if (ret) {
			wmt_err("patch %u download failed (%d)\n", i, ret);
			return ret;
		}

		ret = wmt_core_init_script(wmt_reset_script,
					   ARRAY_SIZE(wmt_reset_script));
		if (ret) {
			wmt_err("WMT reset after patch failed (%d)\n", ret);
			return ret;
		}
	}

	ret = wmt_core_init_script(mcu_clk_low_script,
				   ARRAY_SIZE(mcu_clk_low_script));
	if (ret)
		wmt_warn("failed to restore the MCU clock (%d)\n", ret);

	/* 8. RF calibration with the BT and Wi-Fi rails enabled */
	pa1 = BT_PALDO;
	pa2 = PALDO_ON;
	wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	pa1 = WIFI_PALDO;
	pa2 = PALDO_ON;
	wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);

	ret = wmt_core_init_script(calibration_script,
				   ARRAY_SIZE(calibration_script));

	pa1 = BT_PALDO;
	pa2 = PALDO_OFF;
	wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);
	pa1 = WIFI_PALDO;
	pa2 = PALDO_OFF;
	wmt_core_ctrl(WMT_CTRL_SOC_PALDO_CTRL, &pa1, &pa2);

	if (ret) {
		wmt_err("RF calibration failed (%d)\n", ret);
		return ret;
	}

	/* 9. coexistence configuration */
	ret = wmt_core_init_script(coex_script, ARRAY_SIZE(coex_script));
	if (ret) {
		wmt_err("coexistence setup failed (%d)\n", ret);
		return ret;
	}

	/* 10. co-clock oscillator type */
	if (soc_co_clock) {
		ret = wmt_core_init_script(co_clock_script,
					   ARRAY_SIZE(co_clock_script));
		if (ret) {
			wmt_err("oscillator type setup failed (%d)\n", ret);
			return ret;
		}
	}

	/* 11. FM strap configuration */
	memcpy(strap_cmd, wmt_strap_fm_cmd, sizeof(strap_cmd));
	memcpy(strap_evt, wmt_strap_fm_evt, sizeof(strap_evt));
	strap_cmd[5] = hif->strap_conf[0];
	strap_evt[5] = hif->strap_conf[0];
	strap_script[0].cmd = strap_cmd;
	strap_script[0].cmd_sz = sizeof(strap_cmd);
	strap_script[0].evt = strap_evt;
	strap_script[0].evt_sz = sizeof(strap_evt);
	strap_script[0].str = "configure FM interface";
	ret = wmt_core_init_script(strap_script, 1);
	if (ret) {
		wmt_err("FM strap configuration failed (%d)\n", ret);
		return ret;
	}

	/* 12. BT TSSI source (WMT_SOC.cfg) */
	if (conf && conf->bt_tssi_from_wifi) {
		memcpy(tssi_cmd, wmt_bt_tssi_cmd, sizeof(tssi_cmd));
		tssi_cmd[5] = conf->bt_tssi_from_wifi;
		tssi_cmd[6] = conf->bt_tssi_target & 0xff;
		tssi_cmd[7] = (conf->bt_tssi_target >> 8) & 0xff;
		tssi_script[0].cmd = tssi_cmd;
		tssi_script[0].cmd_sz = sizeof(tssi_cmd);
		tssi_script[0].evt = wmt_bt_tssi_evt;
		tssi_script[0].evt_sz = sizeof(wmt_bt_tssi_evt);
		tssi_script[0].str = "bt tssi from wifi";
		ret = wmt_core_init_script(tssi_script, 1);
		if (ret)
			wmt_warn("BT TSSI configuration failed (%d)\n", ret);
	}

	return 0;
}

int wmt_ic_soc_sw_deinit(struct wmt_hif_conf *hif)
{
	soc_info = NULL;
	return 0;
}
