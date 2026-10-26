/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN FM radio driver - internal interface.
 *
 * Port from Samsung/MTK downstream:
 *   drivers/misc/mediatek/fmradio/core/fm_link.c
 *   drivers/misc/mediatek/fmradio/core/fm_main.c
 *   drivers/misc/mediatek/fmradio/inc/fm_link.h
 *   drivers/misc/mediatek/fmradio/inc/fm_main.h
 *   drivers/misc/mediatek/fmradio/inc/fm_config.h
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_cmd.c
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_lib.c
 *   drivers/misc/mediatek/fmradio/mt6627/pub/mt6627_fm_rds.c
 *   drivers/misc/mediatek/fmradio/mt6627/inc/mt6627_fm_reg.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _MTK_WCN_FM_H_
#define _MTK_WCN_FM_H_

#include <linux/build_bug.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/rfkill.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/videodev2.h>

#include <uapi/linux/mtk_fm_i2s.h>
#include <uapi/linux/mtk_fm_rds.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>

#include "mtk_wcn_stp.h"
#include "mtk_wcn_wmt.h"

#define MTK_WCN_FM_DRIVER_NAME	"mtk_wcn_fm"
#define MTK_WCN_FM_CARD_NAME	"MT6627 FM Receiver"

/*
 * FM <-> firmware protocol (downstream inc/fm_link.h:29-62)
 */
#define FM_PKT_TYPE_CMD		0x01	/* fm_link.h:30 */
#define FM_PKT_TYPE_EVENT	0x04	/* fm_link.h:31 */

#define FM_STP_TEST_OPCODE		0x00
#define FSPI_ENABLE_OPCODE		0x01
#define FSPI_MUX_SEL_OPCODE		0x02
#define FSPI_READ_OPCODE		0x03
#define FSPI_WRITE_OPCODE		0x04
#define FI2C_READ_OPCODE		0x05
#define FI2C_WRITE_OPCODE		0x06
#define FM_ENABLE_OPCODE		0x07
#define FM_RESET_OPCODE			0x08
#define FM_TUNE_OPCODE			0x09
#define FM_SEEK_OPCODE			0x0a
#define FM_SCAN_OPCODE			0x0b
#define RDS_RX_ENABLE_OPCODE		0x0c
#define RDS_RX_DATA_OPCODE		0x0d
#define FM_RAMPDOWN_OPCODE		0x0e
#define FM_MCUCLK_SEL_OPCODE		0x0f
#define FM_MODEMCLK_SEL_OPCODE		0x10
#define RDS_TX_OPCODE			0x11
#define FM_PATCH_DOWNLOAD_OPCODE	0x12
#define FM_COEFF_DOWNLOAD_OPCODE	0x13
#define FM_HWCOEFF_DOWNLOAD_OPCODE	0x14
#define FM_ROM_DOWNLOAD_OPCODE		0x15
#define FM_SOFT_MUTE_TUNE_OPCODE	0x17
#define FM_HOST_READ_OPCODE		0x18
#define FM_HOST_WRITE_OPCODE		0x19
#define CSPI_WRITE_OPCODE		0x20
#define CSPI_READ_OPCODE		0x21

/*
 * Firmware completion bits, one per event opcode (downstream fm_link.h:64-97).
 *
 * Opcodes 0x20/0x21/0x11 do not fit the (1 << opcode) scheme used for the
 * other opcodes, so downstream assigns them explicit bits; the same values
 * are kept here because the event parser must set the bit the waiting
 * command expects.
 */
#define FM_EVT_BIT(op)		(1ULL << (op))
#define FM_EVT_CSPI_READ	(1ULL << 22)	/* fm_link.h:87 */
#define FM_EVT_SM_TUNE		(1ULL << 23)	/* fm_link.h:88 */
#define FM_EVT_HOST_READ	(1ULL << 24)	/* fm_link.h:89 */
#define FM_EVT_HOST_WRITE	(1ULL << 25)	/* fm_link.h:90 */
#define FM_EVT_CSPI_WRITE	(1ULL << 26)	/* fm_link.h:91 */
#define FM_EVT_CQI_DONE		(1ULL << 27)	/* fm_link.h:92 */
#define FM_EVT_TUNE_DONE	(1ULL << 28)	/* fm_link.h:93 */
#define FM_EVT_SEEK_DONE	(1ULL << 29)	/* fm_link.h:94 */
#define FM_EVT_SCAN_DONE	(1ULL << 30)	/* fm_link.h:95 */

/* fm_link.h:175-180 */
#define FM_RX_BUF_SIZE		256
#define FM_TX_BUF_SIZE		1024
#define FM_SW_RETRY_CNT		1
#define FM_SW_RETRY_CNT_MAX	5

/* Per-command timeouts in seconds (downstream inc/fm_link.h:181-204). */
#define FM_EN_TIMEOUT		3
#define FM_FSPI_RD_TIMEOUT	3
#define FM_FSPI_WR_TIMEOUT	3
#define FM_TUNE_TIMEOUT		3
#define FM_SM_TUNE_TIMEOUT	6
#define FM_SEEK_TIMEOUT		15
#define FM_RAMPDOWN_TIMEOUT	3
#define FM_PATCH_TIMEOUT	3
#define FM_COEFF_TIMEOUT	3

/* Firmware primitive opcodes (downstream mt6627/inc/mt6627_fm_cmd.h:21-43) */
#define FM_BOP_BASE			0x80
#define FM_WRITE_BASIC_OP		(FM_BOP_BASE + 0x00)
#define FM_UDELAY_BASIC_OP		(FM_BOP_BASE + 0x01)
#define FM_RD_UNTIL_BASIC_OP		(FM_BOP_BASE + 0x02)
#define FM_MODIFY_BASIC_OP		(FM_BOP_BASE + 0x03)
#define FM_MSLEEP_BASIC_OP		(FM_BOP_BASE + 0x04)
#define FM_TOP_WRITE_BASIC_OP		(FM_BOP_BASE + 0x05)
#define FM_TOP_RD_UNTIL_BASIC_OP	(FM_BOP_BASE + 0x06)
#define FM_TOP_MODIFY_BASIC_OP		(FM_BOP_BASE + 0x07)

#define FM_WRITE_BASIC_OP_SIZE		3
#define FM_UDELAY_BASIC_OP_SIZE		4
#define FM_RD_UNTIL_BASIC_OP_SIZE	5
#define FM_MODIFY_BASIC_OP_SIZE		5
#define FM_MSLEEP_BASIC_OP_SIZE		4
#define FM_TOP_WRITE_BOP_SIZE		7
#define FM_TOP_RD_UNTIL_BOP_SIZE	11
#define FM_TOP_MODIFY_BOP_SIZE		11

/*
 * MT6627 register map (downstream mt6627/inc/mt6627_fm_reg.h:17-49)
 */
#define FM_MAIN_CG1_CTRL	0x60
#define FM_MAIN_CG2_CTRL	0x61
#define FM_MAIN_HWVER		0x62
#define FM_MAIN_CTRL		0x63
#define FM_CHANNEL_SET		0x65
#define FM_MAIN_CFG1		0x66
#define FM_MAIN_CFG2		0x67
#define FM_MAIN_INTR		0x69
#define FM_MAIN_INTRMASK	0x6A
#define FM_MAIN_EXTINTRMASK	0x6B
#define FM_RSSI_IND		0x6C
#define FM_RSSI_TH		0x6D
#define FM_MAIN_RESET		0x6E
#define FM_MAIN_CHANDETSTAT	0x6F
#define FM_RDS_CFG0		0x80
#define FM_RDS_INFO		0x81
#define FM_RDS_DATA_REG		0x82
#define FM_RDS_GOODBK_CNT	0x83
#define FM_RDS_BADBK_CNT	0x84
#define FM_ADDR_PAMD		0xB4
#define FM_RDS_POINTER		0xF0

/* FM_MAIN_CTRL bits (mt6627_fm_reg.h:72-81) */
#define FM_MAIN_CTRL_TUNE	0x0001
#define FM_MAIN_CTRL_SEEK	0x0002
#define FM_MAIN_CTRL_SCAN	0x0004
#define FM_MAIN_CTRL_CQI_READ	0x0008
#define FM_MAIN_CTRL_RDS_MASK	0x0010
#define FM_MAIN_CTRL_MUTE	0x0020
#define FM_MAIN_CTRL_RDS_BRST	0x0040
#define FM_MAIN_CTRL_RAMP_DOWN	0x0100

/* FM_MAIN_INTR bits (mt6627_fm_reg.h:84-91) */
#define FM_INTR_STC_DONE	0x0001
#define FM_INTR_IQCAL_DONE	0x0002
#define FM_INTR_DESENSE_HIT	0x0004
#define FM_INTR_CHNL_CHG	0x0008
#define FM_INTR_SW_INTR		0x0010
#define FM_INTR_RDS		0x0020

/* FM_MAIN_CG2_CTRL bits (mt6627_fm_reg.h:94-97) */
#define FM_CG2_ANTENNA_TYPE	0x0010	/* 0: long antenna, 1: short antenna */
#define FM_CG2_ANALOG_I2S	0x0080	/* 0: lineout, 1: I2S */
#define FM_CG2_DE_EMPHASIS	0x1000	/* 0: 50us, 1: 75us */

/*
 * Registers used by the MT6627 sequences.  Downstream addresses them with
 * numeric literals inside mt6627_fm_cmd.c / mt6627_fm_lib.c; the names below
 * only exist to keep the ported sequences readable.
 */
#define FM_REG_0F_RF_MODE	0x0F	/* mt6627_fm_lib.c:983 */
#define FM_REG_RF_PLL_DIV	0x21	/* mt6627_fm_cmd.c:437 */
#define FM_REG_RF_CLK26M	0x23	/* mt6627_fm_cmd.c:447 */
#define FM_REG_RF_26		0x26	/* cap array read, mt6627_fm_lib.c:1408 */
#define FM_REG_RF_30		0x30	/* TDD/FDD cell mode, mt6627_fm_lib.c:988 */
#define FM_REG_RF_01		0x01	/* fine tune, mt6627_fm_cmd.c:490 */
#define FM_REG_RF_03		0x03
#define FM_REG_RF_15		0x15
#define FM_REG_RF_16		0x16
#define FM_REG_RF_17		0x17
#define FM_REG_RF_34		0x34
#define FM_REG_RF_35		0x35
#define FM_REG_SDM_COEFF0_H	0xD8	/* mt6627_fm_cmd.c:439 */
#define FM_REG_SDM_COEFF0_L	0xD9
#define FM_REG_SDM_COEFF1_H	0xDA
#define FM_REG_SDM_COEFF1_L	0xDB
#define FM_REG_DSP_CLK		0x70	/* mt6627_fm_cmd.c:211 */
#define FM_REG_AUDIO_MONO	0x75	/* mt6627_fm_lib.c:1388 */
#define FM_REG_VOLUME		0x7D	/* mt6627_fm_lib.c:1306 */
#define FM_REG_AUDIO_CTRL0	0x9B	/* I2S TX mode, mt6627_fm_lib.c:1023 */
#define FM_REG_AUDIO_CTRL1	0x9C	/* mute, mt6627_fm_lib.c:344 */
#define FM_REG_DSP_PATCH_CTRL	0x90	/* mt6627_fm_lib.c:423 */
#define FM_REG_DSP_PATCH_DATA	0x92	/* mt6627_fm_lib.c:422 */
#define FM_REG_DSP_MEM_CTRL	0xE1	/* mt6627_fm_lib.c:549 */
#define FM_REG_DSP_MEM_ADDR	0xE2
#define FM_REG_DSP_MEM_DATA	0xE3

/* Top (CSPI) registers (mt6627_fm_cmd.c) */
#define FM_TOP_REG_DIG_TOP	0x0A10	/* mt6627_fm_cmd.c:189 */
#define FM_TOP_REG_CLK_CTRL	0x0050	/* mt6627_fm_cmd.c:202 */
#define FM_TOP_REG_MTCMOS	0x0060	/* mt6627_fm_cmd.c:191 */
#define FM_TOP_REG_CSPI_DUMMY	0x0068	/* mt6627_fm_cmd.c:199 */
#define FM_TOP_REG_AUD_DRV	0x0000	/* mt6627_fm_cmd.c:209 */

/* Host (WCN top) registers (mt6627_fm_lib.c) */
#define FM_HOST_REG_CSPI_CFG	0x8013000C	/* mt6627_fm_lib.c:693 */
#define FM_HOST_REG_AUDIO_CTRL	0x80101054	/* mt6627_fm_lib.c:851 */

/* Chip identification (mt6627_fm_lib.c:739-750) */
#define FM_MT6627_CHIP_ID	0x6627
#define FM_MT6625_CHIP_ID	0x6625

/* DSP image download (mt6627_fm_lib.c:67, 664-667) */
#define FM_MT6627_PATCH_SEG_LEN		512
#define FM_MT6627_PATCH_BUF_SIZE	(4096 * 6)
#define FM_MT6627_ROM_VER_MIN		1
#define FM_MT6627_ROM_VER_MAX		5

/*
 * Defaults from mt6627/inc/mt6627_fm.h:20-31 and
 * mt6627/inc/mt6627_fm_cust_cfg.h:46-60.
 */
#define FM_MT6627_SEEK_SPACE_DEFAULT	2	/* 100 kHz, fm_config.h:48-52 */
#define FM_MT6627_DEEMPHASIS_DEFAULT	0	/* 0: 50us (cust_cfg.h:54) */

/* Soft mute gain / blending defaults (mt6627_fm_lib.c:855-860) */
#define FM_MT6627_SMG_DEFAULT		0x15
#define FM_MT6627_SMG_MAX		0x15
#define FM_MT6627_BLEND_RSSI_DEFAULT	0x164
#define FM_MT6627_BLEND_PAMD_DEFAULT	0x19a
#define FM_MT6627_BLEND_VALUE_DEFAULT	0x2

/* Bands, in 10 kHz units (downstream inc/fm_config.h:24-41) */
#define FM_BAND_UE		1	/* 87.5 - 108.0 MHz */
#define FM_BAND_JAPAN		2	/* 76.0 -  90.0 MHz */
#define FM_BAND_JAPANW		3	/* 76.0 - 108.0 MHz */
#define FM_BAND_SPECIAL		4
#define FM_BAND_DEFAULT		FM_BAND_UE	/* fm_config.h:29 */
#define FM_UE_FREQ_MIN		8750	/* fm_config.h:35 */
#define FM_UE_FREQ_MAX		10800	/* fm_config.h:36 */
#define FM_JP_FREQ_MIN		7600	/* fm_config.h:37 */
#define FM_JP_FREQ_MAX		9000	/* fm_config.h:26 (90 MHz) */
#define FM_SPECIAL_FREQ_MIN	8750	/* fm_config.h:40 */
#define FM_SPECIAL_FREQ_MAX	10800	/* fm_config.h:41 */

/* Seek direction / space encoding (fm_config.h:47-62, mt6627_fm_cmd.c:691) */
#define FM_SEEK_UP		0
#define FM_SEEK_DOWN		1
#define FM_SPACE_50K		1
#define FM_SPACE_100K		2
#define FM_SPACE_200K		4

/* V4L2 frequency unit is 62.5 Hz; internal frequency unit is 10 kHz. */
#define FM_KHZ10_TO_V4L2(f)	((f) * 160)
#define FM_V4L2_TO_KHZ10(f)	((f) / 160)

/* WMT reset payload (downstream wmt_exp.h:185-191) */
#define FM_WMTRSTMSG_RESET_START	0x0
#define FM_WMTRSTMSG_RESET_END		0x1
#define FM_WMTRSTMSG_RESET_END_FAIL	0x2

/*
 * Driver-private V4L2 controls.  Userspace ABI: the numeric values are
 * stable and must not change.
 */
#define V4L2_CID_USER_MTK_WCN_FM_BASE	(V4L2_CID_USER_BASE + 0x11f0)
#define V4L2_CID_MTK_WCN_FM_BAND		(V4L2_CID_USER_MTK_WCN_FM_BASE + 0)
#define V4L2_CID_MTK_WCN_FM_ANTENNA		(V4L2_CID_USER_MTK_WCN_FM_BASE + 1)
#define V4L2_CID_MTK_WCN_FM_SOFTMUTE_GAIN	(V4L2_CID_USER_MTK_WCN_FM_BASE + 2)
#define V4L2_CID_MTK_WCN_FM_BLEND		(V4L2_CID_USER_MTK_WCN_FM_BASE + 3)

struct mtk_wcn_fm_i2s;

/*
 * FM I2S control ABI (include/uapi/linux/mtk_fm_i2s.h, shared with
 * tools/mediatek/wcn/fm_i2s).  The layouts are fixed: three little endian
 * 32 bit words, 12 bytes per structure.
 */
static_assert(sizeof(struct fm_i2s_setting) == 12);
static_assert(sizeof(struct fm_i2s_info) == 12);

/* Maximum number of raw RDS records buffered for read(). */
#define MTK_WCN_FM_RDS_FIFO_RECORDS	32

/*
 * The raw RDS record layout is part of the userspace ABI.  It is defined once
 * in include/uapi/linux/mtk_fm_rds.h, which is included above; the compiler
 * checks that the driver uses exactly that layout.
 */
static_assert(sizeof(struct mtk_wcn_fm_rds_block) == 12);
static_assert(sizeof(struct mtk_wcn_fm_rds_data) == MTK_FM_RDS_RECORD_SIZE);

struct mtk_wcn_fm_hw_info {
	int chip_id;		/* 0x6627 / 0x6625 */
	int eco_ver;		/* WMT hardware version + 1 */
	int rom_ver;		/* DSP ROM version reported by the firmware */
	int patch_ver;		/* patch version, word at coeff offset 38 */
};

struct mtk_wcn_fm_dev {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler ctrl_handler;

	/* Serialises ioctls, power transitions and control writes. */
	struct mutex lock;
	/* Serialises access to cmd_buf and the single outstanding command. */
	struct mutex cmd_lock;
	u8 cmd_buf[FM_TX_BUF_SIZE];

	/* Event completion: bits are set by the STP receive work. */
	spinlock_t evt_lock;
	u64 evt_bits;
	wait_queue_head_t evt_wq;

	/* STP receive path. */
	struct work_struct rx_work;
	struct work_struct rst_work;
	u8 rx_buf[FM_RX_BUF_SIZE];
	u8 rx_payload[FM_RX_BUF_SIZE];
	u8 parser_state;
	u8 parser_opcode;
	u16 parser_len;
	u16 parser_pos;

	/* Results of the last command, downstream fm_res_ctx members used. */
	u16 fspi_rd;
	u16 seek_result;
	u32 cspi_rd;

	/* RDS data delivered to userspace through read()/poll(). */
	struct kfifo rds_fifo;
	wait_queue_head_t rds_wq;

	struct rfkill *rfkill;

	/* Antenna switch GPIO (downstream "fmradio_ant_sel"). */
	int ant_sel_gpio;
	bool ant_sel_valid;

	bool link_ready;
	bool resetting;
	bool powered;
	bool opened;

	/* MT6627 I2S audio port state (FM_REG_AUDIO_CTRL0 + host reg). */
	bool i2s_on;
	/* ASoC glue state, NULL when CONFIG_SND_SOC is disabled. */
	struct mtk_wcn_fm_i2s *i2s;

	u16 cur_freq;		/* 10 kHz units */
	u8 band;
	u8 antenna;		/* 0: long, 1: short */
	u8 deemphasis;		/* 0: 50us, 1: 75us */
	u8 vol;			/* 0..15 */
	bool mute;
	bool mono;		/* true: forced mono, false: automatic */
	bool rds_on;
	u8 smg;			/* soft mute gain, 0..0x15 */
	bool blend;

	struct mtk_wcn_fm_hw_info hw;
};

/* mtk_wcn_fm_link.c */
int mtk_wcn_fm_link_init(struct mtk_wcn_fm_dev *fm);
void mtk_wcn_fm_link_exit(struct mtk_wcn_fm_dev *fm);
int mtk_wcn_fm_cmd_tx(struct mtk_wcn_fm_dev *fm, const u8 *buf, u16 len,
		      u64 mask, unsigned int timeout_s);

/* mtk_wcn_fm_chip.c */
int mtk_wcn_fm_chip_power_up(struct mtk_wcn_fm_dev *fm);
int mtk_wcn_fm_chip_power_down(struct mtk_wcn_fm_dev *fm);
int mtk_wcn_fm_chip_tune(struct mtk_wcn_fm_dev *fm, u16 freq);
int mtk_wcn_fm_chip_seek(struct mtk_wcn_fm_dev *fm, u16 min_freq,
			 u16 max_freq, u16 space, u8 dir, bool wrap,
			 u16 *result);
int mtk_wcn_fm_chip_rampdown(struct mtk_wcn_fm_dev *fm);
int mtk_wcn_fm_chip_status(struct mtk_wcn_fm_dev *fm, s32 *dbm, bool *stereo);
int mtk_wcn_fm_chip_mute(struct mtk_wcn_fm_dev *fm, bool mute);
int mtk_wcn_fm_chip_vol_set(struct mtk_wcn_fm_dev *fm, u8 vol);
int mtk_wcn_fm_chip_vol_get(struct mtk_wcn_fm_dev *fm, u8 *vol);
int mtk_wcn_fm_chip_rds(struct mtk_wcn_fm_dev *fm, bool on);
int mtk_wcn_fm_chip_antenna(struct mtk_wcn_fm_dev *fm, u8 type);
int mtk_wcn_fm_chip_deemphasis(struct mtk_wcn_fm_dev *fm, u8 deemphasis);
int mtk_wcn_fm_chip_mono(struct mtk_wcn_fm_dev *fm, bool mono);
int mtk_wcn_fm_chip_smg(struct mtk_wcn_fm_dev *fm, u8 value);
int mtk_wcn_fm_chip_blend(struct mtk_wcn_fm_dev *fm, bool on);

/*
 * MT6627 FM I2S audio port.
 *
 * mtk_wcn_fm_chip_i2s_set() enables/disables the port with the exact
 * downstream register writes:
 *   enable : FM_REG_AUDIO_CTRL0 = 0xf9ab and host 0x80101054 = 0x00003f35
 *            (mt6627_fm_lib.c:1023-1024 after every retune, :850-851 in the
 *            power up sequence and :1511,1513 after a soft-mute scan);
 *   disable: host 0x80101054 bits 13:14 cleared (mt6627_PowerDown(),
 *            mt6627_fm_lib.c:892-894) and FM_REG_AUDIO_CTRL0 bits 15:3
 *            cleared (mt6627_pwrdown(), mt6627_fm_cmd.c:523).
 * Both require the FM function to be powered.  The ASoC glue
 * (mtk_wcn_fm_i2s.c) and its inline fallback are the only callers; the power
 * up and power down sequences of this file program the same registers
 * themselves.
 */
int mtk_wcn_fm_chip_i2s_set(struct mtk_wcn_fm_dev *fm, bool on);
/* Read the real port state back from the chip.  -EAGAIN if unpowered. */
int mtk_wcn_fm_chip_i2s_get(struct mtk_wcn_fm_dev *fm, bool *on);
/*
 * Fill a downstream struct fm_i2s_info.  The status is the real port state
 * (FM_I2S_OFF while the FM function is unpowered); mode and rate are the fixed
 * MT6627 values, mt6627_fm_config.c:116-118.
 */
int mtk_wcn_fm_chip_i2s_info(struct mtk_wcn_fm_dev *fm,
			     struct fm_i2s_info *info);

/* ASoC glue; must stay the last include of this header. */
#include "mtk_wcn_fm_i2s.h"

#endif /* _MTK_WCN_FM_H_ */
