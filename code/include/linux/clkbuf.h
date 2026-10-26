/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6735 RF clock buffer control.
 *
 * Coordinator-owned frozen interface for the rem2 gap-closure work.  The
 * identifiers are copied verbatim from the Samsung/MTK downstream GPL tree
 * that matches the SM-G532M ROM:
 *
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.h:23-51
 *   drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.c
 *
 * The implementation (drivers/misc/mediatek/clkbuf/) is delivered by the
 * clkbuf gap-closure leaf.
 */
#ifndef __LINUX_CLKBUF_H
#define __LINUX_CLKBUF_H

#include <linux/kconfig.h>
#include <linux/types.h>

#define CLKBUF_NUM		4
#define STA_CLK_ON		1
#define STA_CLK_OFF		0

/* mt_clkbuf_ctl.h:23-29 -- buffer index used by consumers and by the
 * IPC_RPC_GET_RF_CLK_BUF_OP response (port_kernel.c:1682-1706).
 */
enum clk_buf_id {
	CLK_BUF_BB_MD		= 0,
	CLK_BUF_CONN		= 1,
	CLK_BUF_NFC		= 2,
	CLK_BUF_AUDIO		= 3,
	CLK_BUF_INVALID		= 4,
};

/* mt_clkbuf_ctl.h:31-35 -- control mode from "mediatek,clkbuf-config". */
enum clk_buf_status {
	CLK_BUF_DISABLE		= 0,
	CLOCK_BUFFER_SW_CONTROL	= 1,
	CLOCK_BUFFER_HW_CONTROL	= 2,
};

/* mt_clkbuf_ctl.h:37-40 -- last software request per buffer. */
enum clk_buf_swctrl_status {
	CLK_BUF_SW_DISABLE	= 0,
	CLK_BUF_SW_ENABLE	= 1,
};

#if IS_ENABLED(CONFIG_MTK_CLKBUF)
bool clk_buf_ctrl(enum clk_buf_id id, bool onoff);
void clk_buf_get_swctrl_status(enum clk_buf_swctrl_status *status);
bool clk_buf_init(void);
#else
static inline bool clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	return false;
}

static inline void clk_buf_get_swctrl_status(enum clk_buf_swctrl_status *status)
{
	int i;

	for (i = 0; i < CLKBUF_NUM; i++)
		status[i] = CLK_BUF_SW_DISABLE;
}

static inline bool clk_buf_init(void)
{
	return false;
}
#endif

#endif /* __LINUX_CLKBUF_H */
