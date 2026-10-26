/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735 RF clock buffer control: SPM/BSI register definitions.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.c
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.h
 *   drivers/misc/mediatek/base/power/mt6735/mt_spm.h
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef __MT_CLKBUF_CTL_H__
#define __MT_CLKBUF_CTL_H__

#include <linux/clkbuf.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>

/*
 * SPM register offsets inside the SPM bank.  Downstream reaches them through
 * SPM_BASE, which the vendor DTB declares as sleep@10006000
 * (arch/arm/boot/dts/mt6735.dtsi:367-369 => 0x10006000); here "mediatek,spm"
 * points at the same 0x10006000 syscon bank
 * (mt6735.dtsi:182-185 "scpsys: syscon@10006000").
 *
 * mt_spm.h:87,156,176-177,179-185
 */
#define SPM_POWER_ON_VAL1	0x014
#define SPM_PCM_REG13_DATA	0x3b4
#define SPM_PWR_STATUS		0x60c
#define SPM_PWR_STATUS_2ND	0x610
#define SPM_SLEEP_MDBSI_CON	0x614
#define SPM_BSI_GEN		0x620
#define SPM_BSI_EN_SR		0x624
#define SPM_BSI_CLK_SR		0x628
#define SPM_BSI_D0_SR		0x62c
#define SPM_BSI_D1_SR		0x630
#define SPM_BSI_D2_SR		0x634

/* spm_v1/mt_spm.c:932 -- start bit of the AP BSI protocol generator. */
#define SPM_BSI_START		BIT(0)

/* mt_spm_internal.h:77-78 -- R13 MD1 APSRC request / AP ACK bits;
 * SPM_POWER_ON_VAL1 bit 17 is set by spm_ap_mdsrc_req()
 * (spm_v1/mt_spm_sleep.c:2049).
 */
#define R13_MD1_APSRC_REQ	BIT(4)
#define R13_AP_MD1SRC_ACK	BIT(5)
#define SPMPWR_AP_MDSRC_REQ	BIT(17)

/* mt_clkbuf_ctl.c:170 -- modem power-status bit of SPM_PWR_STATUS[_2ND]. */
#define SPM_PWR_STATUS_MD	BIT(0)

/* mt_clkbuf_ctl.c:158 -- MD BSI control field of SPM_SLEEP_MDBSI_CON. */
#define SPM_SLEEP_MDBSI_MASK	0x7

/*
 * mt_spm.h:321-327 -- index of each BSI shift register inside the config
 * array handed to the protocol generator.
 */
enum clk_buf_bsi_pad_id {
	BSI_EN_SR = 0,
	BSI_CLK_SR = 1,
	BSI_D0_SR = 2,
	BSI_D1_SR = 3,
	BSI_D2_SR = 4,
};

#define CLK_BUF_BSI_PAD_NUM	5

/*
 * BSI command word (mt_clkbuf_ctl.c:66-73).
 *
 * BSI_CW_CNT is the number of command-word bits shifted out; the data and
 * read bits are both 0 (write access), the address is the clock-buffer
 * control register and BSI_CW_DEFAULT holds the reset value 0x01E8F.
 */
#define BSI_CW_CNT		30
#define BSI_DATA_BIT		0
#define BSI_READ_BIT		0
#define BSI_CLK_MASK		0x7
#define BSI_CW_ADDR		252
#define BSI_CW_DEFAULT		0x01E8F

/*
 * [INFERENCE] the downstream generator busy-waits for SPM_BSI_START to clear
 * without any bound (spm_v1/mt_spm.c:941-944); a hardware timeout keeps a
 * stuck SPM from hanging the caller forever.
 */
#define BSI_GEN_POLL_US		1000
/* spm_ap_mdsrc_req(): 3 ms 26M settle, then at most 10 x 1 ms ACK poll
 * (spm_v1/mt_spm_sleep.c:2054-2071).
 */
#define MDSRC_SETTLE_MS		3
#define MDSRC_ACK_RETRY		10
#define MDSRC_ACK_DELAY_MS	1

/**
 * struct mt_clkbuf - MT6735 RF clock buffer controller state
 * @dev: platform device of the "mediatek,rf_clock_buffer" node
 * @spm: regmap of the SPM syscon ("mediatek,spm" phandle)
 * @lock: serializes BSI/SPM accesses and protects @swctrl
 * @swctrl: last software request per buffer (mt_clkbuf_ctl.c:59-64)
 * @config: "mediatek,clkbuf-config" control mode per buffer
 * @bsi_cfg: BSI shift-register image being built (mt_clkbuf_ctl.c:76-83)
 * @bit_ctrl: current BSI shift-register bit position
 * @mdsrc_req_cnt: nesting counter of spm_ap_mdsrc_req()
 *	(spm_v1/mt_spm_sleep.c:2031-2088)
 * @sysfs_added: /sys/power/clk_buf registered
 * @ready: probe completed, SPM/BSI accesses are valid
 */
struct mt_clkbuf {
	struct device *dev;
	struct regmap *spm;
	struct mutex lock;
	enum clk_buf_swctrl_status swctrl[CLKBUF_NUM];
	u32 config[CLKBUF_NUM];
	u32 bsi_cfg[CLK_BUF_BSI_PAD_NUM];
	unsigned int bit_ctrl;
	int mdsrc_req_cnt;
	bool sysfs_added;
	bool ready;
};

#endif /* __MT_CLKBUF_CTL_H__ */
