// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN FM radio - MT6627 chip layer.
 *
 * Port from Samsung/MTK downstream:
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_cmd.c
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_lib.c
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_rds.c
 *
 * All register writes below are the exact sequences of the downstream
 * driver; the file:line citations point at the downstream statement each
 * step was taken from.  Register level commands travel to the FM firmware
 * on the STP FM channel (see mtk_wcn_fm_link.c).
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mtk_wcn_fm.h"

/*
 * DSP images.  Downstream open()s /etc/firmware/mt6627/mt6627_fm_vN_*.bin
 * (mt6627_fm_lib.c:46-57); request_firmware() resolves the same names below
 * the kernel firmware search path, so userspace has to install the
 * downstream files as /lib/firmware/mt6627/mt6627_fm_vN_{patch,coeff}.bin
 * (or point CONFIG_EXTRA_FIRMWARE_DIR at /etc/firmware).
 */
static const char *const mtk_wcn_fm_patch_names[FM_MT6627_ROM_VER_MAX] = {
	"mt6627/mt6627_fm_v1_patch.bin",
	"mt6627/mt6627_fm_v2_patch.bin",
	"mt6627/mt6627_fm_v3_patch.bin",
	"mt6627/mt6627_fm_v4_patch.bin",
	"mt6627/mt6627_fm_v5_patch.bin",
};

static const char *const mtk_wcn_fm_coeff_names[FM_MT6627_ROM_VER_MAX] = {
	"mt6627/mt6627_fm_v1_coeff.bin",
	"mt6627/mt6627_fm_v2_coeff.bin",
	"mt6627/mt6627_fm_v3_coeff.bin",
	"mt6627/mt6627_fm_v4_coeff.bin",
	"mt6627/mt6627_fm_v5_coeff.bin",
};

/* Downstream mt6627_fm_lib.c:1296-1300. */
static const u16 mtk_wcn_fm_vol_tbl[16] = {
	0x0000, 0x0519, 0x066A, 0x0814, 0x0A2B, 0x0CCD, 0x101D, 0x1449,
	0x198A, 0x2027, 0x287A, 0x32F5, 0x4027, 0x50C3, 0x65AD, 0x7FFF,
};

/* Downstream mt6627_fm_lib.c:2005-2007. */
static const u16 mtk_wcn_fm_tdd_mask[4] = { 0x0001, 0x0010, 0x0100, 0x1000 };

/* Downstream mt6627_fm_lib.c:1366 (FM_BF_STEREO). */
#define FM_RSSI_IND_STEREO	0x1000

/* DSP coefficient addresses used by the soft mute / blending controls. */
#define FM_COEFF_SMG		0x31D8	/* mt6627_fm_lib.c:1605 */
#define FM_COEFF_BLEND_RSSI	0x3154	/* mt6627_fm_lib.c:1641 */
#define FM_COEFF_BLEND_PAMD	0x3155	/* mt6627_fm_lib.c:1646 */
#define FM_COEFF_BLEND_VALUE	0x3157	/* mt6627_fm_lib.c:1651 */

/*
 * 0x83/0x84 double as the DSP ROM code version and ready flag while the
 * ASIP is held in ROM mode (mt6627_fm_lib.c:463-479).
 */
#define FM_REG_DSP_ROM_READY	0x84
#define FM_REG_DSP_ROM_VERSION	0x83

/*
 * Channel side parameters, generated from mt6627_fm_lib.c:1903-1949.
 * 861 entries, 50 kHz steps from 6500 to 10800 (10 kHz units); the index is
 * (freq - 6500) / 5, downstream mt6627_fm_lib.c:2106-2128.
 */
static const u8 mtk_wcn_fm_chan_para[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,	/* 6500~6595 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 6600~6695 */
	2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0,	/* 6700~6795 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 6800~6895 */
	0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 6900~6995 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7000~7095 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,	/* 7100~7195 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0,	/* 7200~7295 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7300~7395 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7400~7495 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7500~7595 */
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,	/* 7600~7695 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7700~7795 */
	8, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 7800~7895 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,	/* 7900~7995 */
	0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,	/* 8000~8095 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8100~8195 */
	0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8200~8295 */
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8300~8395 */
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8400~8495 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8500~8595 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8600~8695 */
	0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8700~8795 */
	0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8800~8895 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 8900~8995 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9000~9095 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9100~9195 */
	0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9200~9295 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0,	/* 9300~9395 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9400~9495 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9500~9595 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9600~9695 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9700~9795 */
	0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 9800~9895 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0,	/* 9900~9995 */
	0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10000~10095 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10100~10195 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,	/* 10200~10295 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10300~10395 */
	8, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10400~10495 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10500~10595 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	/* 10600~10695 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0,	/* 10700~10795 */
	0,	/* 10800 */
};

/* Downstream mt6627_fm_lib.c:1958-2003: TDD cell mode per 4 x 50 kHz channel. */
static const u16 mtk_wcn_fm_tdd_list[] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 6500~6595 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 6600~6695 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 6700~6795 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 6800~6895 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 6900~6995 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7000~7095 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7100~7195 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7200~7295 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7300~7395 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7400~7495 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7500~7595 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7600~7695 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7700~7795 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7800~7895 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 7900~7995 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8000~8095 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8100~8195 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8200~8295 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8300~8395 */
	0x0101, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8400~8495 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8500~8595 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8600~8695 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8700~8795 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8800~8895 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 8900~8995 */
	0x0000, 0x0000, 0x0101, 0x0101, 0x0101,	/* 9000~9095 */
	0x0101, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9100~9195 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9200~9295 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9300~9395 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9400~9495 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9500~9595 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 9600~9695 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0100,	/* 9700~9795 */
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101,	/* 9800~9895 */
	0x0101, 0x0101, 0x0001, 0x0000, 0x0000,	/* 9900~9995 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10000~10095 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10100~10195 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10200~10295 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10300~10395 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10400~10495 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000,	/* 10500~10595 */
	0x0000, 0x0000, 0x0000, 0x0000, 0x0100,	/* 10600~10695 */
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101,	/* 10700~10795 */
	0x0001,	/* 10800 */
};

/* The channel tables must cover 6500..10800 kHz in 50 kHz steps. */
static_assert(ARRAY_SIZE(mtk_wcn_fm_chan_para) == 861);
static_assert(ARRAY_SIZE(mtk_wcn_fm_tdd_list) == 216);

/* The RDS event payload is downstream rds_rx_t verbatim (148 bytes). */
static_assert(sizeof(struct mtk_wcn_fm_rds_data) == 148);

/*
 * Firmware primitive builders (downstream mt6627_fm_cmd.c:30-164)
 */
struct mtk_wcn_fm_pkt {
	u8 *buf;
	int cap;
	int len;
	int err;
};

static void mtk_wcn_fm_pkt_init(struct mtk_wcn_fm_pkt *pkt, u8 *buf, int cap,
				u8 opcode)
{
	pkt->buf = buf;
	pkt->cap = cap;
	pkt->len = 4;
	pkt->err = 0;
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = opcode;
	buf[2] = 0;
	buf[3] = 0;
}

static void mtk_wcn_fm_pkt_add(struct mtk_wcn_fm_pkt *pkt, int written)
{
	if (pkt->err)
		return;
	if (written < 0)
		pkt->err = written;
	else
		pkt->len += written;
}

/* mt6627_fm_cmd.c:30-47 */
static int mtk_wcn_fm_bop_write(u8 addr, u16 value, u8 *buf, int size)
{
	if (size < FM_WRITE_BASIC_OP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_WRITE_BASIC_OP;
	buf[1] = FM_WRITE_BASIC_OP_SIZE;
	buf[2] = addr;
	buf[3] = value & 0xff;
	buf[4] = (value >> 8) & 0xff;

	return FM_WRITE_BASIC_OP_SIZE + 2;
}

/* mt6627_fm_cmd.c:49-67 */
static int mtk_wcn_fm_bop_udelay(u32 value, u8 *buf, int size)
{
	if (size < FM_UDELAY_BASIC_OP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_UDELAY_BASIC_OP;
	buf[1] = FM_UDELAY_BASIC_OP_SIZE;
	buf[2] = value & 0xff;
	buf[3] = (value >> 8) & 0xff;
	buf[4] = (value >> 16) & 0xff;
	buf[5] = (value >> 24) & 0xff;

	return FM_UDELAY_BASIC_OP_SIZE + 2;
}

/* mt6627_fm_cmd.c:69-89 */
static int mtk_wcn_fm_bop_rd_until(u8 addr, u16 mask, u16 value, u8 *buf,
				   int size)
{
	if (size < FM_RD_UNTIL_BASIC_OP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_RD_UNTIL_BASIC_OP;
	buf[1] = FM_RD_UNTIL_BASIC_OP_SIZE;
	buf[2] = addr;
	buf[3] = mask & 0xff;
	buf[4] = (mask >> 8) & 0xff;
	buf[5] = value & 0xff;
	buf[6] = (value >> 8) & 0xff;

	return FM_RD_UNTIL_BASIC_OP_SIZE + 2;
}

/* mt6627_fm_cmd.c:91-111 */
static int mtk_wcn_fm_bop_modify(u8 addr, u16 mask_and, u16 mask_or, u8 *buf,
				 int size)
{
	if (size < FM_MODIFY_BASIC_OP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_MODIFY_BASIC_OP;
	buf[1] = FM_MODIFY_BASIC_OP_SIZE;
	buf[2] = addr;
	buf[3] = mask_and & 0xff;
	buf[4] = (mask_and >> 8) & 0xff;
	buf[5] = mask_or & 0xff;
	buf[6] = (mask_or >> 8) & 0xff;

	return FM_MODIFY_BASIC_OP_SIZE + 2;
}

/* mt6627_fm_cmd.c:113-135 */
static int mtk_wcn_fm_bop_top_write(u16 addr, u32 value, u8 *buf, int size)
{
	if (size < FM_TOP_WRITE_BOP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_TOP_WRITE_BASIC_OP;
	buf[1] = FM_TOP_WRITE_BOP_SIZE;
	buf[2] = 0x04;	/* top register space */
	buf[3] = addr & 0xff;
	buf[4] = (addr >> 8) & 0xff;
	buf[5] = value & 0xff;
	buf[6] = (value >> 8) & 0xff;
	buf[7] = (value >> 16) & 0xff;
	buf[8] = (value >> 24) & 0xff;

	return FM_TOP_WRITE_BOP_SIZE + 2;
}

/* mt6627_fm_cmd.c:137-164 */
static int mtk_wcn_fm_bop_top_rd_until(u16 addr, u32 mask, u32 value, u8 *buf,
				       int size)
{
	int i;

	if (size < FM_TOP_RD_UNTIL_BOP_SIZE + 2)
		return -ENOSPC;

	buf[0] = FM_TOP_RD_UNTIL_BASIC_OP;
	buf[1] = FM_TOP_RD_UNTIL_BOP_SIZE;
	buf[2] = 0x04;	/* top register space */
	buf[3] = addr & 0xff;
	buf[4] = (addr >> 8) & 0xff;
	for (i = 0; i < 4; i++) {
		buf[5 + i] = (mask >> (8 * i)) & 0xff;
		buf[9 + i] = (value >> (8 * i)) & 0xff;
	}

	return FM_TOP_RD_UNTIL_BOP_SIZE + 2;
}

/*
 * Register access primitives
 */
static int mtk_wcn_fm_send(struct mtk_wcn_fm_dev *fm, struct mtk_wcn_fm_pkt *pkt,
			   u64 mask, unsigned int timeout_s)
{
	u16 payload = pkt->len - 4;

	if (pkt->err)
		return pkt->err;

	pkt->buf[2] = payload & 0xff;
	pkt->buf[3] = (payload >> 8) & 0xff;

	return mtk_wcn_fm_cmd_tx(fm, pkt->buf, pkt->len, mask, timeout_s);
}

/* mt6627_get_reg(), mt6627_fm_cmd.c:839-852 */
static int mtk_wcn_fm_reg_read(struct mtk_wcn_fm_dev *fm, u8 addr, u16 *val)
{
	u8 *buf = fm->cmd_buf;
	int ret;

	mutex_lock(&fm->cmd_lock);
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = FSPI_READ_OPCODE;
	buf[2] = 0x01;
	buf[3] = 0x00;
	buf[4] = addr;
	ret = mtk_wcn_fm_cmd_tx(fm, buf, 5, FM_EVT_BIT(FSPI_READ_OPCODE),
				FM_FSPI_RD_TIMEOUT);
	if (!ret && val)
		*val = fm->fspi_rd;
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_set_reg(), mt6627_fm_cmd.c:854-870 */
static int mtk_wcn_fm_reg_write(struct mtk_wcn_fm_dev *fm, u8 addr, u16 val)
{
	u8 *buf = fm->cmd_buf;
	int ret;

	mutex_lock(&fm->cmd_lock);
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = FSPI_WRITE_OPCODE;
	buf[2] = 0x03;
	buf[3] = 0x00;
	buf[4] = addr;
	buf[5] = val & 0xff;
	buf[6] = (val >> 8) & 0xff;
	ret = mtk_wcn_fm_cmd_tx(fm, buf, 7, FM_EVT_BIT(FSPI_WRITE_OPCODE),
				FM_FSPI_WR_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_set_bits_reg(), mt6627_fm_cmd.c:872-888 */
static int mtk_wcn_fm_reg_set_bits(struct mtk_wcn_fm_dev *fm, u8 addr,
				   u16 bits, u16 mask)
{
	struct mtk_wcn_fm_pkt pkt;
	int ret;

	mutex_lock(&fm->cmd_lock);
	/* Opcode 0x11 is not an FM opcode: the firmware echoes it back as the
	 * event opcode, which is what the command waits for.
	 */
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf), 0x11);
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(addr, mask, bits,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(0x11), FM_FSPI_WR_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_top_set_reg(), mt6627_fm_cmd.c:909-929 */
static int mtk_wcn_fm_top_write(struct mtk_wcn_fm_dev *fm, u16 addr, u32 val)
{
	u8 *buf = fm->cmd_buf;
	int i;
	int ret;

	mutex_lock(&fm->cmd_lock);
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = CSPI_WRITE_OPCODE;
	buf[2] = 0x07;
	buf[3] = 0x00;
	buf[4] = 0x04;	/* top register space; 0x02 selects the FM space */
	buf[5] = addr & 0xff;
	buf[6] = (addr >> 8) & 0xff;
	for (i = 0; i < 4; i++)
		buf[7 + i] = (val >> (8 * i)) & 0xff;
	ret = mtk_wcn_fm_cmd_tx(fm, buf, 11, FM_EVT_CSPI_WRITE,
				FM_FSPI_WR_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_host_get_reg(), mt6627_fm_cmd.c:932-949 */
static int mtk_wcn_fm_host_read(struct mtk_wcn_fm_dev *fm, u32 addr, u32 *val)
{
	u8 *buf = fm->cmd_buf;
	int i;
	int ret;

	mutex_lock(&fm->cmd_lock);
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = FM_HOST_READ_OPCODE;
	buf[2] = 0x04;
	buf[3] = 0x00;
	for (i = 0; i < 4; i++)
		buf[4 + i] = (addr >> (8 * i)) & 0xff;
	ret = mtk_wcn_fm_cmd_tx(fm, buf, 8, FM_EVT_HOST_READ,
				FM_FSPI_RD_TIMEOUT);
	if (!ret && val)
		*val = fm->cspi_rd;
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_host_set_reg(), mt6627_fm_cmd.c:951-972 */
static int mtk_wcn_fm_host_write(struct mtk_wcn_fm_dev *fm, u32 addr, u32 val)
{
	u8 *buf = fm->cmd_buf;
	int i;
	int ret;

	mutex_lock(&fm->cmd_lock);
	buf[0] = FM_PKT_TYPE_CMD;
	buf[1] = FM_HOST_WRITE_OPCODE;
	buf[2] = 0x08;
	buf[3] = 0x00;
	for (i = 0; i < 4; i++)
		buf[4 + i] = (addr >> (8 * i)) & 0xff;
	for (i = 0; i < 4; i++)
		buf[8 + i] = (val >> (8 * i)) & 0xff;
	ret = mtk_wcn_fm_cmd_tx(fm, buf, 12, FM_EVT_HOST_WRITE,
				FM_FSPI_WR_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* DSP coefficient memory write, mt6627_fm_lib.c:942-951 */
static int mtk_wcn_fm_coeff_write(struct mtk_wcn_fm_dev *fm, u16 addr, u16 value)
{
	int ret;

	ret = mtk_wcn_fm_reg_write(fm, FM_REG_DSP_MEM_ADDR, addr);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_DSP_MEM_DATA, value);
	if (ret)
		return ret;

	return mtk_wcn_fm_reg_write(fm, FM_REG_DSP_MEM_CTRL, 0x0002);
}

/*
 * Channel parameter / TDD helpers (mt6627_fm_lib.c:1895-2128)
 */
static bool mtk_wcn_fm_tdd_chan_check(u16 freq)
{
	u32 pos;

	if (freq < 6500 || freq > 10800)
		return false;

	pos = (freq - 6500) / 5;
	return mtk_wcn_fm_tdd_list[pos / 4] & mtk_wcn_fm_tdd_mask[pos % 4];
}

static u16 mtk_wcn_fm_chan_para_get(u16 freq)
{
	u32 pos;

	if (freq < 6500)
		return 0;

	pos = (freq - 6500) / 5;
	if (pos >= ARRAY_SIZE(mtk_wcn_fm_chan_para))
		pos = ARRAY_SIZE(mtk_wcn_fm_chan_para) - 1;

	return mtk_wcn_fm_chan_para[pos];
}

/*
 * Power up / power down (mt6627_fm_lib.c:664-940)
 */
/* mt6627_pwrup_clock_on(), mt6627_fm_cmd.c:172-221 */
static int mtk_wcn_fm_send_pwrup_clock_on(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	u16 de_emphasis = fm->deemphasis & 0x1;
	int ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_ENABLE_OPCODE);
	/* turn on the top clock */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_DIG_TOP,
							   0xffffffff,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* enable MTCMOS */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000030,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000035,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_rd_until(FM_TOP_REG_MTCMOS,
							     0x0000000a, 0xa,
							     pkt.buf + pkt.len,
							     pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000015,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000005,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_udelay(10, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000045,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* CSPI FM slave dummy count */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CSPI_DUMMY,
							   0x0000003f,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* enable the digital OSC */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000001,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_udelay(3000, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* route the OSC clock to FM, release HW clock gating */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000003,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000007,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* I2S current driving */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_AUD_DRV,
							   0x00000000,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* enable DSP auto clock gating */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_DSP_CLK, 0x0040,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* de-emphasis setting */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CG2_CTRL,
						       (u16)~FM_CG2_DE_EMPHASIS,
						       de_emphasis << 12,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(FM_ENABLE_OPCODE),
			      FM_EN_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_pwrup_digital_init(), mt6627_fm_cmd.c:422-468 */
static int mtk_wcn_fm_send_pwrup_digital_init(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	int ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_ENABLE_OPCODE);
	/* FM RF & ADPLL divider setting */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_PLL_DIV, 0xe000,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_SDM_COEFF0_H,
						      0x03f0, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_SDM_COEFF0_L,
						      0x3f04, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_SDM_COEFF1_H,
						      0x0014, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_SDM_COEFF1_L,
						      0x2a38, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* 26 MHz reference clock */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_CLK26M, 0x4000,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* FM digital init: interrupt masks */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_MAIN_INTRMASK, 0x0021,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_MAIN_EXTINTRMASK,
						      0x0021, pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x0000000f,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* release ASIP reset, disable ASIP power down */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CG2_CTRL,
						       0xfffd, 0x0002,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CG2_CTRL,
						       0xfffe, 0x0000,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_udelay(100000,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* poll 0x64[4:0] == 2 (DSP ready) */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_rd_until(0x64, 0x001f, 0x0002,
							 pkt.buf + pkt.len,
							 pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(FM_ENABLE_OPCODE),
			      FM_EN_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_pwrup_fine_tune(), mt6627_fm_cmd.c:476-504 */
static int mtk_wcn_fm_send_pwrup_fine_tune(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	int ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_ENABLE_OPCODE);
	/* let the host control the RF registers */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000007,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_01, 0xbee8,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_03, 0xf6ed,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_15, 0x0d80,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_16, 0x0068,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_17, 0x092a,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_34, 0x807f,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_REG_RF_35, 0x311e,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* hand the RF registers back to the DSP */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x0000000f,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(FM_ENABLE_OPCODE),
			      FM_EN_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_pwrdown(), mt6627_fm_cmd.c:512-551 */
static int mtk_wcn_fm_send_pwrdown(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	int i;
	int ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_ENABLE_OPCODE);
	/* audio output to I2S TX mode off */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_REG_AUDIO_CTRL0,
						       0xfff8, 0x0000,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* disable HW clock control */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x0000330f,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* reset ASIP */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_write(FM_MAIN_CG2_CTRL, 0x0001,
						      pkt.buf + pkt.len,
						      pkt.cap - pkt.len));
	/* digital core + register file reset */
	for (i = 0; i < 4; i++)
		mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_RESET,
							       0xfff8, 0x0000,
							       pkt.buf + pkt.len,
							       pkt.cap - pkt.len));
	/* disable all clocks */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000000,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* reset the RF register file */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00004000,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_CLK_CTRL,
							   0x00000000,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	/* MTCMOS power off */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000005,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000015,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000035,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_write(FM_TOP_REG_MTCMOS,
							   0x00000030,
							   pkt.buf + pkt.len,
							   pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_top_rd_until(FM_TOP_REG_MTCMOS,
							     0x0000000a, 0x0,
							     pkt.buf + pkt.len,
							     pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(FM_ENABLE_OPCODE),
			      FM_EN_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/* mt6627_get_rom_version(), mt6627_fm_lib.c:447-485 */
static int mtk_wcn_fm_get_rom_version(struct mtk_wcn_fm_dev *fm)
{
	unsigned long deadline = jiffies + 10 * HZ;
	u16 tmp = 0;
	int ret;

	/* DSP ROM code version request enable (0x61 b15 = 1) */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL, 0x8000, 0x7fff);
	if (ret)
		return ret;
	/* release ASIP reset (0x61 b1 = 1) */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0002, 0xfffd);
	if (ret)
		return ret;
	/* enable ASIP power (0x61 b0 = 0) */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0000, 0xfffe);
	if (ret)
		return ret;

	do {
		usleep_range(1000, 1500);
		ret = mtk_wcn_fm_reg_read(fm, FM_REG_DSP_ROM_READY, &tmp);
		if (ret)
			return ret;
	} while (tmp != 0x0001 && time_before(jiffies, deadline));

	if (tmp != 0x0001) {
		dev_err(fm->dev, "DSP ROM version never became ready\n");
		return -ETIMEDOUT;
	}

	/* DSP code version is reported in 0x83[15:8] */
	ret = mtk_wcn_fm_reg_read(fm, FM_REG_DSP_ROM_VERSION, &tmp);
	if (ret)
		return ret;
	tmp >>= 8;

	/* request disable (0x61 b15 = 0) */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0000, 0x7fff);
	if (ret)
		return ret;
	/* reset ASIP (0x61[1:0] = 1) */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL, 0x0001, 0xfffc);
	if (ret)
		return ret;

	return tmp;
}

/*
 * Load a DSP image for the running DSP ROM version.  Downstream's patch
 * lookup requires an exact match (mt6627_get_patch_path() returns 1 on the
 * fallback and mt6627_PowerUp() treats that as an error,
 * mt6627_fm_lib.c:487-513, 775-784), while the coefficient lookup falls back
 * to the newest file and then disables the patch selector
 * (mt6627_fm_lib.c:515-547, 806-809).
 */
static int mtk_wcn_fm_fw_request(struct mtk_wcn_fm_dev *fm,
				 const char *const *names, int rom_ver,
				 bool allow_fallback,
				 const struct firmware **fw, bool *exact)
{
	int i, ret;

	*exact = false;
	if (rom_ver >= FM_MT6627_ROM_VER_MIN &&
	    rom_ver <= FM_MT6627_ROM_VER_MAX) {
		ret = request_firmware(fw, names[rom_ver - 1], fm->dev);
		if (!ret) {
			*exact = true;
			return 0;
		}
	}

	if (!allow_fallback)
		return -ENOENT;

	for (i = FM_MT6627_ROM_VER_MAX - 1; i >= 0; i--) {
		ret = request_firmware(fw, names[i], fm->dev);
		if (!ret) {
			dev_warn(fm->dev,
				 "no image for DSP ROM v%d, using %s\n",
				 rom_ver, names[i]);
			return 0;
		}
	}

	return -ENOENT;
}

/* mt6627_DspPatch(), mt6627_fm_lib.c:549-662 */
static int mtk_wcn_fm_dsp_patch(struct mtk_wcn_fm_dev *fm, const u8 *img,
				size_t len, bool is_patch)
{
	u8 opcode = is_patch ? FM_PATCH_DOWNLOAD_OPCODE :
			       FM_COEFF_DOWNLOAD_OPCODE;
	u64 mask = FM_EVT_BIT(opcode);
	unsigned int timeout = is_patch ? FM_PATCH_TIMEOUT : FM_COEFF_TIMEOUT;
	u8 seg_num, seg_id;
	int seg_len, ret;
	u16 pkt_len;

	if (!img || !len)
		return -EINVAL;
	if (len > U8_MAX * FM_MT6627_PATCH_SEG_LEN)
		return -EFBIG;

	seg_num = len / FM_MT6627_PATCH_SEG_LEN + 1;

	for (seg_id = 0; seg_id < seg_num; seg_id++) {
		seg_len = ((seg_id + 1) < seg_num) ? FM_MT6627_PATCH_SEG_LEN :
						     (len % FM_MT6627_PATCH_SEG_LEN);

		mutex_lock(&fm->cmd_lock);
		fm->cmd_buf[0] = FM_PKT_TYPE_CMD;
		fm->cmd_buf[1] = opcode;
		fm->cmd_buf[4] = seg_num;
		fm->cmd_buf[5] = seg_id;
		if (seg_len > (sizeof(fm->cmd_buf) - 6)) {
			mutex_unlock(&fm->cmd_lock);
			return -ENOSPC;
		}
		memcpy(&fm->cmd_buf[6], img + seg_id * FM_MT6627_PATCH_SEG_LEN,
		       seg_len);
		pkt_len = 6 + seg_len;
		fm->cmd_buf[2] = (pkt_len - 4) & 0xff;
		fm->cmd_buf[3] = ((pkt_len - 4) >> 8) & 0xff;
		ret = mtk_wcn_fm_cmd_tx(fm, fm->cmd_buf, pkt_len, mask, timeout);
		mutex_unlock(&fm->cmd_lock);
		if (ret) {
			dev_err(fm->dev, "DSP image segment %d/%d failed (%d)\n",
				seg_id + 1, seg_num, ret);
			return ret;
		}
	}

	return 0;
}

/*
 * Full power up.  Sequence order and every value come from
 * mt6627_PowerUp(), mt6627_fm_lib.c:664-863.
 */
static int mtk_wcn_fm_power_up_seq(struct mtk_wcn_fm_dev *fm)
{
	const struct firmware *patch = NULL, *coeff = NULL;
	bool exact = false;
	u8 *img = NULL;
	size_t len;
	u16 chip_id = 0;
	int rom_ver, ret;

	/* step 1: common SPI parameter */
	ret = mtk_wcn_fm_host_write(fm, FM_HOST_REG_CSPI_CFG, 0x0000801f);
	if (ret) {
		dev_err(fm->dev, "cannot configure CSPI (%d)\n", ret);
		return ret;
	}

	/* step 2: FM digital clock on */
	ret = mtk_wcn_fm_send_pwrup_clock_on(fm);
	if (ret) {
		dev_err(fm->dev, "clock on sequence failed (%d)\n", ret);
		return ret;
	}

	/* read back the chip id */
	ret = mtk_wcn_fm_reg_read(fm, FM_MAIN_HWVER, &chip_id);
	if (ret) {
		dev_err(fm->dev, "cannot read chip id (%d)\n", ret);
		return ret;
	}
	if (chip_id != FM_MT6627_CHIP_ID && chip_id != FM_MT6625_CHIP_ID) {
		dev_err(fm->dev, "unexpected FM chip id 0x%04x\n", chip_id);
		return -EIO;
	}
	fm->hw.chip_id = chip_id;
	fm->hw.eco_ver = mtk_wcn_wmt_hwver_get() + 1;
	dev_info(fm->dev, "MT6627 FM: chip 0x%04x, ECO %d\n", chip_id,
		 fm->hw.eco_ver);

	rom_ver = mtk_wcn_fm_get_rom_version(fm);
	if (rom_ver < 0) {
		dev_err(fm->dev, "cannot read DSP ROM version (%d)\n", rom_ver);
		return rom_ver;
	}
	fm->hw.rom_ver = rom_ver;

	/* step 3: download the DSP patch (exact ROM version required) */
	ret = mtk_wcn_fm_fw_request(fm, mtk_wcn_fm_patch_names, rom_ver, false,
				    &patch, &exact);
	if (ret) {
		dev_err(fm->dev,
			"DSP patch mt6627/mt6627_fm_v%d_patch.bin not available\n",
			rom_ver);
		return ret;
	}
	ret = mtk_wcn_fm_dsp_patch(fm, patch->data, patch->size, true);
	release_firmware(patch);
	if (ret)
		return ret;

	/* ... and the DSP coefficients (fallback allowed) */
	ret = mtk_wcn_fm_fw_request(fm, mtk_wcn_fm_coeff_names, rom_ver, true,
				    &coeff, &exact);
	if (ret) {
		dev_err(fm->dev,
			"DSP coefficient mt6627/mt6627_fm_v%d_coeff.bin not available\n",
			rom_ver);
		return ret;
	}

	img = kmalloc(FM_MT6627_PATCH_BUF_SIZE, GFP_KERNEL);
	if (!img) {
		release_firmware(coeff);
		return -ENOMEM;
	}
	len = min_t(size_t, coeff->size, FM_MT6627_PATCH_BUF_SIZE);
	memcpy(img, coeff->data, len);
	release_firmware(coeff);

	if (!exact) {
		/* Unknown ROM version: the downstream driver clears the patch
		 * selector words so the DSP keeps running on its ROM code
		 * (mt6627_fm_lib.c:807-809).
		 */
		if (len > 5) {
			img[4] = 0x00;
			img[5] = 0x00;
		}
	}
	if (len >= 40)
		fm->hw.patch_ver = img[38] | (img[39] << 8);

	ret = mtk_wcn_fm_dsp_patch(fm, img, len, false);
	kfree(img);
	if (ret)
		return ret;

	/* Downstream reports the ROM version incremented by one after the
	 * coefficient download (mt6627_fm_lib.c:800).
	 */
	fm->hw.rom_ver = rom_ver + 1;
	dev_info(fm->dev, "MT6627 FM: ROM v%d, patch 0x%04x\n", fm->hw.rom_ver,
		 fm->hw.patch_ver);

	ret = mtk_wcn_fm_reg_write(fm, FM_REG_DSP_PATCH_DATA, 0x0000);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_DSP_PATCH_CTRL, 0x0040);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_DSP_PATCH_CTRL, 0x0000);
	if (ret)
		return ret;

	/* step 4: FM digital init */
	ret = mtk_wcn_fm_send_pwrup_digital_init(fm);
	if (ret) {
		dev_err(fm->dev, "digital init failed (%d)\n", ret);
		return ret;
	}

	/* step 5: RF fine tune */
	ret = mtk_wcn_fm_send_pwrup_fine_tune(fm);
	if (ret) {
		dev_err(fm->dev, "RF fine tune failed (%d)\n", ret);
		return ret;
	}

	/* enable the connectivity SoC FM 2-wire RX audio path */
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_AUDIO_CTRL0, 0xf9ab);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_host_write(fm, FM_HOST_REG_AUDIO_CTRL, 0x00003f35);
	if (ret)
		return ret;

	/* The power up sequence leaves the I2S port enabled. */
	fm->i2s_on = true;

	return 0;
}

int mtk_wcn_fm_chip_power_up(struct mtk_wcn_fm_dev *fm)
{
	int ret;

	if (fm->powered)
		return 0;

	/* mt6627_pwron(), mt6627_fm_lib.c:98-115 */
	if (!mtk_wcn_wmt_func_on(WMTDRV_FM)) {
		dev_err(fm->dev, "WMT cannot power on the FM function\n");
		return -EIO;
	}

	if (!mtk_wcn_stp_is_ready()) {
		dev_err(fm->dev, "STP is not ready after powering on FM\n");
		ret = -ENODEV;
		goto err_func_off;
	}

	ret = mtk_wcn_fm_power_up_seq(fm);
	if (ret)
		goto err_func_off;

	fm->powered = true;

	return 0;

err_func_off:
	mtk_wcn_wmt_func_off(WMTDRV_FM);
	return ret;
}

/* mt6627_PowerDown(), mt6627_fm_lib.c:865-940 */
int mtk_wcn_fm_chip_power_down(struct mtk_wcn_fm_dev *fm)
{
	u32 host_reg;
	u16 intr;
	int ret = 0, err;

	if (!fm->powered)
		return 0;

	/*
	 * SW workaround for MCUFA: clear a pending STC interrupt before the
	 * ramp down, otherwise the DSP cannot switch MCUFA back.
	 */
	err = mtk_wcn_fm_reg_read(fm, FM_MAIN_INTR, &intr);
	if (!err && (intr & FM_INTR_STC_DONE))
		mtk_wcn_fm_reg_write(fm, FM_MAIN_INTR, intr);

	/* set the audio output to I2S RX mode */
	err = mtk_wcn_fm_host_read(fm, FM_HOST_REG_AUDIO_CTRL, &host_reg);
	if (!err) {
		host_reg &= 0xffff9fff;
		err = mtk_wcn_fm_host_write(fm, FM_HOST_REG_AUDIO_CTRL,
					    host_reg);
	}
	if (err) {
		dev_warn(fm->dev, "audio path shutdown failed (%d)\n", err);
		ret = err;
	}

	err = mtk_wcn_fm_send_pwrdown(fm);
	if (err) {
		dev_warn(fm->dev, "power down sequence failed (%d)\n", err);
		if (!ret)
			ret = err;
	} else {
		err = mtk_wcn_fm_reg_write(fm, FM_MAIN_EXTINTRMASK, 0x0000);
		if (err && !ret)
			ret = err;
	}

	/* mt6627_pwroff(), mt6627_fm_lib.c:117-126 */
	if (!mtk_wcn_wmt_func_off(WMTDRV_FM)) {
		dev_err(fm->dev, "WMT cannot power off the FM function\n");
		if (!ret)
			ret = -EIO;
	}

	fm->powered = false;
	fm->rds_on = false;
	/* The power down sequence turned the I2S port off (mt6627_fm_lib.c:
	 * 892-894, mt6627_fm_cmd.c:523) and the registers are gone.
	 */
	fm->i2s_on = false;

	return ret;
}

/*
 * Tune / seek (mt6627_fm_lib.c:953-1136 and 1146-1179)
 */
/* mt6627_tune(), mt6627_fm_cmd.c:595-630 */
static int mtk_wcn_fm_send_tune(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	int ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_TUNE_OPCODE);
	/* start the hardware tune and wait for the STC interrupt */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CTRL, 0xfff8,
						       FM_MAIN_CTRL_TUNE,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt,
			      FM_EVT_BIT(FM_TUNE_OPCODE) | FM_EVT_TUNE_DONE,
			      FM_TUNE_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

int mtk_wcn_fm_chip_rampdown(struct mtk_wcn_fm_dev *fm)
{
	struct mtk_wcn_fm_pkt pkt;
	int ret;

	/* mt6627_RampDown(), mt6627_fm_lib.c:384-445 */
	ret = mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x00000007);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_0F_RF_MODE, 0x0000, 0xf800);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x0000000f);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_write(fm, FM_MAIN_INTRMASK, 0x0000);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_write(fm, FM_MAIN_EXTINTRMASK, 0x0000);
	if (ret)
		return ret;

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_RAMPDOWN_OPCODE);
	/* clear the DSP state */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CTRL, 0xfff0,
						       0x0000,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* enter ramp down, the firmware waits for STC_DONE */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CTRL, 0xffff,
						       FM_MAIN_CTRL_RAMP_DOWN,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_rd_until(FM_MAIN_INTR,
							 FM_INTR_STC_DONE,
							 FM_INTR_STC_DONE,
							 pkt.buf + pkt.len,
							 pkt.cap - pkt.len));
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CTRL,
						       (u16)~FM_MAIN_CTRL_RAMP_DOWN,
						       0x0000, pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* write 1 to clear the STC_DONE status flag */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_INTR, 0xffff,
						       FM_INTR_STC_DONE,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt, FM_EVT_BIT(FM_RAMPDOWN_OPCODE),
			      FM_RAMPDOWN_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);
	if (ret)
		return ret;

	ret = mtk_wcn_fm_reg_write(fm, FM_MAIN_EXTINTRMASK, 0x0021);
	if (ret)
		return ret;

	return mtk_wcn_fm_reg_write(fm, FM_MAIN_INTRMASK, 0x0021);
}

int mtk_wcn_fm_chip_tune(struct mtk_wcn_fm_dev *fm, u16 freq)
{
	u16 chan_para, freq_reg;
	int ret;

	if (freq < 6500 || freq > 10800)
		return -EINVAL;

	/* mt6627_SetFreq(), mt6627_fm_lib.c:953-1136 */
	ret = mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x00000007);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_0F_RF_MODE, 0x0455, 0xf800);
	if (ret)
		return ret;

	if (mtk_wcn_fm_tdd_chan_check(freq))
		ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_RF_30, 0x0008, 0xfff3);
	else
		ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_RF_30, 0x0000, 0xfff3);
	if (ret)
		return ret;

	ret = mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x0000000f);
	if (ret)
		return ret;

	chan_para = mtk_wcn_fm_chan_para_get(freq);
	freq_reg = (freq - 6400) * 2 / 10;

	ret = mtk_wcn_fm_reg_set_bits(fm, FM_CHANNEL_SET, freq_reg, 0xfc00);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_CHANNEL_SET, chan_para << 12,
				      0x0fff);
	if (ret)
		return ret;

	/* enable the connectivity SoC FM 2-wire RX audio path */
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_AUDIO_CTRL0, 0xf9ab);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_host_write(fm, FM_HOST_REG_AUDIO_CTRL, 0x00003f35);
	if (ret)
		return ret;
	/* Every retune re-enables the I2S port (mt6627_fm_lib.c:1023-1024). */
	fm->i2s_on = true;

	ret = mtk_wcn_fm_send_tune(fm);
	if (ret) {
		dev_err(fm->dev, "tune to %u failed (%d)\n", freq, ret);
		return ret;
	}

	fm->cur_freq = freq;

	/*
	 * Re-apply the user soft mute gain / blending settings after the tune;
	 * the DSP reloads its coefficient memory (mt6627_fm_lib.c:1073-1118).
	 */
	if (fm->smg != FM_MT6627_SMG_DEFAULT) {
		ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_SMG, fm->smg);
		if (ret)
			return ret;
	}
	if (!fm->blend) {
		ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_RSSI, 0);
		if (ret)
			return ret;
		ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_PAMD, 0);
		if (ret)
			return ret;
		ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_VALUE, 0);
		if (ret)
			return ret;
	}

	return 0;
}

/* mt6627_seek(), mt6627_fm_cmd.c:696-746 */
static int mtk_wcn_fm_send_seek(struct mtk_wcn_fm_dev *fm, u16 min_freq,
				u16 max_freq, u16 space, u8 dir, bool wrap)
{
	struct mtk_wcn_fm_pkt pkt;
	u16 max_reg, min_reg, spacing;
	int ret;

	max_reg = (max_freq - 6400) * 2 / 10;
	min_reg = (min_freq - 6400) * 2 / 10;

	switch (space) {
	case FM_SPACE_50K:
		spacing = 0x1000;
		break;
	case FM_SPACE_200K:
		spacing = 0x4000;
		break;
	case FM_SPACE_100K:
	default:
		spacing = 0x2000;
		break;
	}

	mutex_lock(&fm->cmd_lock);
	mtk_wcn_fm_pkt_init(&pkt, fm->cmd_buf, sizeof(fm->cmd_buf),
			    FM_SEEK_OPCODE);
	/* seek direction: 0x66[10] = 1 for seek down */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CFG1, 0xfbff,
						       dir ? 0x0400 : 0x0000,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* channel spacing: 0x66[14:12] */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CFG1, 0x8fff,
						       spacing,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* 0x66[11]: wrap around at the band edge.  Downstream seek always sets
	 * this bit (mt6627_fm_cmd.c:728) while the scan variant clears it
	 * (mt6627_fm_cmd.c:795); the requested V4L2 wrap flag selects which.
	 */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CFG1, 0xf7ff,
						       wrap ? 0x0800 : 0x0000,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* 0x66[9:0] upper bound */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CFG1, 0xfc00,
						       max_reg,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* 0x67[9:0] lower bound */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CFG2, 0xfc00,
						       min_reg,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	/* enable the hardware controlled seek sequence */
	mtk_wcn_fm_pkt_add(&pkt, mtk_wcn_fm_bop_modify(FM_MAIN_CTRL, 0xfff8,
						       FM_MAIN_CTRL_SEEK,
						       pkt.buf + pkt.len,
						       pkt.cap - pkt.len));
	ret = mtk_wcn_fm_send(fm, &pkt,
			      FM_EVT_BIT(FM_SEEK_OPCODE) | FM_EVT_SEEK_DONE,
			      FM_SEEK_TIMEOUT);
	mutex_unlock(&fm->cmd_lock);

	return ret;
}

/*
 * mt6627_Seek(), mt6627_fm_lib.c:1146-1179.  Downstream never wired this
 * helper into the basic interface (the assignment is inside #if 0), so the
 * post-seek ramp down / unmute below is ported from the disabled code and is
 * unverified on hardware.
 */
int mtk_wcn_fm_chip_seek(struct mtk_wcn_fm_dev *fm, u16 min_freq,
			 u16 max_freq, u16 space, u8 dir, bool wrap,
			 u16 *result)
{
	u16 ctrl = 0;
	int ret;

	if (min_freq >= max_freq)
		return -EINVAL;

	ret = mtk_wcn_fm_chip_rampdown(fm);
	if (ret)
		return ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_MAIN_CTRL, &ctrl);
	if (ret)
		return ret;

	ret = mtk_wcn_fm_chip_mute(fm, true);
	if (ret)
		return ret;

	ret = mtk_wcn_fm_send_seek(fm, min_freq, max_freq, space, dir, wrap);
	if (ret)
		return ret;

	if (fm->seek_result < min_freq || fm->seek_result > max_freq) {
		dev_warn(fm->dev, "seek returned out of range frequency %u\n",
			 fm->seek_result);
		return -ERANGE;
	}

	*result = fm->seek_result;
	fm->cur_freq = fm->seek_result;

	ret = mtk_wcn_fm_chip_rampdown(fm);
	if (ret)
		dev_warn(fm->dev, "post-seek ramp down failed (%d)\n", ret);

	/* restore the mute state the caller had before the seek */
	if (!(ctrl & FM_MAIN_CTRL_MUTE))
		ret = mtk_wcn_fm_chip_mute(fm, false);

	return ret;
}

/*
 * Status / audio controls
 */
/* mt6627_GetCurRSSI() and mt6627_GetMonoStereo(), mt6627_fm_lib.c:1277-1294,
 * 1364-1379.
 */
int mtk_wcn_fm_chip_status(struct mtk_wcn_fm_dev *fm, s32 *dbm, bool *stereo)
{
	u16 reg;
	int ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_RSSI_IND, &reg);
	if (ret)
		return ret;

	if (dbm) {
		u16 rssi = reg & 0x03ff;

		*dbm = (rssi > 511) ? (((rssi - 1024) * 6) >> 4) :
				      ((rssi * 6) >> 4);
	}
	if (stereo)
		*stereo = !!(reg & FM_RSSI_IND_STEREO);

	return 0;
}

/* mt6627_Mute(), mt6627_fm_lib.c:333-351 */
int mtk_wcn_fm_chip_mute(struct mtk_wcn_fm_dev *fm, bool mute)
{
	u16 reg;
	int ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_REG_AUDIO_CTRL1, &reg);
	if (ret)
		return ret;

	reg &= 0xfffc;
	if (mute)
		reg |= 0x0003;

	ret = mtk_wcn_fm_reg_write(fm, FM_REG_AUDIO_CTRL1, reg);
	if (!ret)
		fm->mute = mute;

	return ret;
}

/* mt6627_SetVol(), mt6627_fm_lib.c:1301-1320 */
int mtk_wcn_fm_chip_vol_set(struct mtk_wcn_fm_dev *fm, u8 vol)
{
	int ret;

	vol = min_t(u8, vol, 15);
	ret = mtk_wcn_fm_reg_write(fm, FM_REG_VOLUME, mtk_wcn_fm_vol_tbl[vol]);
	if (!ret)
		fm->vol = vol;

	return ret;
}

/* mt6627_GetVol(), mt6627_fm_lib.c:1321-1349 */
int mtk_wcn_fm_chip_vol_get(struct mtk_wcn_fm_dev *fm, u8 *vol)
{
	u16 reg;
	int i, ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_REG_VOLUME, &reg);
	if (ret)
		return ret;

	*vol = 0;
	for (i = 0; i < ARRAY_SIZE(mtk_wcn_fm_vol_tbl); i++) {
		if (mtk_wcn_fm_vol_tbl[i] == reg) {
			*vol = i;
			break;
		}
	}

	return 0;
}

/* mt6627_SetAntennaType(), mt6627_fm_lib.c:303-318 */
int mtk_wcn_fm_chip_antenna(struct mtk_wcn_fm_dev *fm, u8 type)
{
	u16 reg;
	int ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_MAIN_CG2_CTRL, &reg);
	if (ret)
		return ret;

	if (type)
		reg |= FM_CG2_ANTENNA_TYPE;
	else
		reg &= ~FM_CG2_ANTENNA_TYPE;

	ret = mtk_wcn_fm_reg_write(fm, FM_MAIN_CG2_CTRL, reg);
	if (!ret)
		fm->antenna = type ? 1 : 0;

	return ret;
}

/*
 * De-emphasis.  Downstream programs 0x61[12] while powering up
 * (mt6627_fm_cmd.c:213) and exposes the value through the search threshold
 * ioctl (mt6627_fm_lib.c:1740-1747); the chip accepts the same modify
 * operation at run time.
 */
int mtk_wcn_fm_chip_deemphasis(struct mtk_wcn_fm_dev *fm, u8 deemphasis)
{
	int ret;

	ret = mtk_wcn_fm_reg_set_bits(fm, FM_MAIN_CG2_CTRL,
				      (deemphasis ? 1 : 0) << 12,
				      (u16)~FM_CG2_DE_EMPHASIS);
	if (!ret)
		fm->deemphasis = deemphasis ? 1 : 0;

	return ret;
}

/*
 * mt6627_SetMonoStereo(), mt6627_fm_lib.c:1380-1395.  @mono forces mono,
 * otherwise the chip decides automatically.
 */
int mtk_wcn_fm_chip_mono(struct mtk_wcn_fm_dev *fm, bool mono)
{
	int ret;

	ret = mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x00000007);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_AUDIO_MONO,
				      mono ? 0x0008 : 0x0000,
				      (u16)~0x0008);
	if (ret)
		return ret;

	return mtk_wcn_fm_top_write(fm, FM_TOP_REG_CLK_CTRL, 0x0000000f);
}

/* mt6627_soft_mute_control(), mt6627_fm_lib.c:1597-1628 */
int mtk_wcn_fm_chip_smg(struct mtk_wcn_fm_dev *fm, u8 value)
{
	int ret;

	if (value > FM_MT6627_SMG_MAX)
		return -EINVAL;

	/* 0 disables the soft mute gain, 0x15 restores the default level */
	ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_SMG, value ? value : 0x1);
	if (!ret)
		fm->smg = value;

	return ret;
}

/* mt6627_blending_control(), mt6627_fm_lib.c:1630-1718 */
int mtk_wcn_fm_chip_blend(struct mtk_wcn_fm_dev *fm, bool on)
{
	u16 rssi = on ? FM_MT6627_BLEND_RSSI_DEFAULT : 0;
	u16 pamd = on ? FM_MT6627_BLEND_PAMD_DEFAULT : 0;
	u16 blend = on ? FM_MT6627_BLEND_VALUE_DEFAULT : 0;
	int ret;

	ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_RSSI, rssi);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_PAMD, pamd);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_coeff_write(fm, FM_COEFF_BLEND_VALUE, blend);
	if (!ret)
		fm->blend = on;

	return ret;
}

/* mt6627_RDS_enable() / mt6627_RDS_disable(), mt6627_fm_rds.c:45-93 */
int mtk_wcn_fm_chip_rds(struct mtk_wcn_fm_dev *fm, bool on)
{
	u16 ctrl;
	int ret;

	if (on)
		ret = mtk_wcn_fm_reg_write(fm, FM_RDS_CFG0, 6);
	else
		ret = 0;
	if (ret)
		return ret;

	ret = mtk_wcn_fm_reg_read(fm, FM_MAIN_CTRL, &ctrl);
	if (ret)
		return ret;

	if (on)
		ctrl |= FM_MAIN_CTRL_RDS_MASK;
	else
		ctrl &= ~FM_MAIN_CTRL_RDS_MASK;

	ret = mtk_wcn_fm_reg_write(fm, FM_MAIN_CTRL, ctrl);
	if (!ret)
		fm->rds_on = on;

	return ret;
}
/*
 * MT6627 FM I2S audio port.
 *
 * The port is enabled by two register writes that the downstream driver
 * issues on every power up and on every retune:
 *
 *	mt6627_write(0x9B, 0xF9AB);  -- FM_REG_AUDIO_CTRL0
 *	mt6627_host_write(0x80101054, 0x00003f35);  -- host audio ctrl
 *
 * (mt6627_fm_lib.c:1023-1024 after a retune, :850-851 in the power up
 * sequence, :1511,1513 after a soft-mute scan).  The matching disable is what
 * mt6627_PowerDown()/mt6627_pwrdown() do before the DSP clocks are gated: host
 * bits 13:14 are cleared (mt6627_fm_lib.c:892-894) and FM_REG_AUDIO_CTRL0 bits
 * 15:3 are cleared (mt6627_fm_cmd.c:523).  There is no downstream or ROM
 * register sequence that changes the MT6627 I2S clock mode or sample rate
 * (mt6627_I2s_Setting() has no body, mt6627_fm_lib.c:88-90,1825-1827), so this
 * file only ever enables or disables the port.
 */
#define FM_MT6627_I2S_CTRL0_ON		0xf9ab	/* mt6627_fm_lib.c:1023 */
#define FM_MT6627_HOST_AUDIO_ON		0x00003f35 /* mt6627_fm_lib.c:851 */
/* Bits 13:14: cleared by mt6627_PowerDown(), mt6627_fm_lib.c:893. */
#define FM_HOST_AUDIO_I2S_MASK		0x00006000
/* Bits 15:3: cleared by mt6627_pwrdown(), mt6627_fm_cmd.c:523. */
#define FM_AUDIO_CTRL0_I2S_MASK		0xfff8
/*
 * The enable value only sets bit 13 of that field, so the read back comparison
 * has to use the pattern of the ON value rather than the whole mask.
 */
#define FM_HOST_AUDIO_I2S_ON_BITS \
	(FM_MT6627_HOST_AUDIO_ON & FM_HOST_AUDIO_I2S_MASK)

int mtk_wcn_fm_chip_i2s_set(struct mtk_wcn_fm_dev *fm, bool on)
{
	u32 host;
	int ret;

	if (!fm->powered)
		return -EAGAIN;

	if (on) {
		ret = mtk_wcn_fm_reg_write(fm, FM_REG_AUDIO_CTRL0,
					   FM_MT6627_I2S_CTRL0_ON);
		if (ret)
			return ret;
		ret = mtk_wcn_fm_host_write(fm, FM_HOST_REG_AUDIO_CTRL,
					    FM_MT6627_HOST_AUDIO_ON);
		if (ret)
			return ret;
		fm->i2s_on = true;
		return 0;
	}

	/* mt6627_PowerDown(), mt6627_fm_lib.c:892-894 */
	ret = mtk_wcn_fm_host_read(fm, FM_HOST_REG_AUDIO_CTRL, &host);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_host_write(fm, FM_HOST_REG_AUDIO_CTRL,
				    host & ~FM_HOST_AUDIO_I2S_MASK);
	if (ret)
		return ret;

	/* mt6627_pwrdown(), mt6627_fm_cmd.c:523 */
	ret = mtk_wcn_fm_reg_set_bits(fm, FM_REG_AUDIO_CTRL0, 0x0000,
				      FM_AUDIO_CTRL0_I2S_MASK);
	if (ret)
		return ret;

	fm->i2s_on = false;

	return 0;
}

int mtk_wcn_fm_chip_i2s_get(struct mtk_wcn_fm_dev *fm, bool *on)
{
	u16 ctrl0;
	u32 host;
	int ret;

	if (!on)
		return -EINVAL;
	if (!fm->powered)
		return -EAGAIN;

	ret = mtk_wcn_fm_reg_read(fm, FM_REG_AUDIO_CTRL0, &ctrl0);
	if (ret)
		return ret;
	ret = mtk_wcn_fm_host_read(fm, FM_HOST_REG_AUDIO_CTRL, &host);
	if (ret)
		return ret;

	*on = (ctrl0 & FM_AUDIO_CTRL0_I2S_MASK) != 0 &&
	      (host & FM_HOST_AUDIO_I2S_MASK) == FM_HOST_AUDIO_I2S_ON_BITS;
	fm->i2s_on = *on;

	return 0;
}

int mtk_wcn_fm_chip_i2s_info(struct mtk_wcn_fm_dev *fm,
			     struct fm_i2s_info *info)
{
	bool on;
	int ret;

	if (!info)
		return -EINVAL;

	/* mt6627_fm_config.c:116-118: fixed master / 32 kHz configuration. */
	info->mode = FM_I2S_MASTER;
	info->rate = FM_I2S_32K;

	if (!fm->powered) {
		/*
		 * The port cannot be on while the FM function is off, and the
		 * registers are not reachable, so report the known state.
		 */
		info->status = FM_I2S_OFF;
		return 0;
	}

	ret = mtk_wcn_fm_chip_i2s_get(fm, &on);
	if (ret) {
		info->status = FM_I2S_STATE_ERR;
		return ret;
	}

	info->status = on ? FM_I2S_ON : FM_I2S_OFF;

	return 0;
}
