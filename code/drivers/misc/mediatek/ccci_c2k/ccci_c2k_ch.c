// SPDX-License-Identifier: GPL-2.0
/*
 * C2K (MD3) channel map, IRAT and dual-SIM switch state.
 *
 * Port from Samsung/MTK downstream (SM-G532M, MT6737T, kernel 3.18):
 *   drivers/misc/mediatek/eccci/modem_ccif_c2k.c (c2k_ports[])
 *   drivers/misc/mediatek/eccci/port_cfg.c (md3_ccci_ports[])
 *   drivers/misc/mediatek/eccci/port_net.c (eccci_cc3mni_ops)
 *   drivers/misc/mediatek/eccci/ccci_support.c (sim switch mode)
 *   drivers/misc/mediatek/include/mt-plat/mt_ccci_common.h (enum c2k_channel)
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 The Nura project
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "ccci_c2k.h"

/*
 * Loopback channel ids that downstream defines next to enum c2k_channel
 * (mt_ccci_common.h:502-503).  They are not part of the frozen shared enum,
 * so they are kept local to the translation table.
 */
#define C2K_LOOPBACK_CH			255
#define C2K_MD2AP_LOOPBACK_CH		256

/*
 * MD3 channel map, verbatim from modem_ccif_c2k.c:112-152.  The first entry
 * of every C2K channel wins, which is what the downstream lookup loops do.
 * "excp" is the C2K channel reported while MD3 is in exception; it carries a
 * CCCI channel id for the control row, exactly like downstream.
 */
struct ccci_c2k_ch_map {
	int c2k_ch;
	int excp_ch;
	int tx_ch;		/* enum ccci_ch */
	int rx_ch;		/* enum ccci_ch */
	const char *name;	/* MD3 port name, port_cfg.c:127-155 */
};

static const struct ccci_c2k_ch_map c2k_ch_map[] = {
	{ CTRL_CH_C2K,		CCCI_CONTROL_TX,	CCCI_CONTROL_TX,
	  CCCI_CONTROL_RX,	"ccci3_ctrl" },
	{ CTRL_CH_C2K,		CTRL_CH_C2K,		CCCI_STATUS_TX,
	  CCCI_STATUS_RX,	"ccci3_poll" },
	{ AUDIO_CH_C2K,		AUDIO_CH_C2K,		CCCI_PCM_TX,
	  CCCI_PCM_RX,		"ccci3_aud" },
	{ NET1_CH_C2K,		NET1_CH_C2K,		CCCI_CCMNI1_TX,
	  CCCI_CCMNI1_RX,	"cc3mni0" },
	{ NET1_CH_C2K,		NET1_CH_C2K,		CCCI_CCMNI1_DL_ACK,
	  CCCI_CCMNI1_DL_ACK,	"cc3mni0" },
	{ NET2_CH_C2K,		NET2_CH_C2K,		CCCI_CCMNI2_TX,
	  CCCI_CCMNI2_RX,	"cc3mni1" },
	{ NET2_CH_C2K,		NET2_CH_C2K,		CCCI_CCMNI2_DL_ACK,
	  CCCI_CCMNI2_DL_ACK,	"cc3mni1" },
	{ NET3_CH_C2K,		NET3_CH_C2K,		CCCI_CCMNI3_TX,
	  CCCI_CCMNI3_RX,	"cc3mni2" },
	{ NET3_CH_C2K,		NET3_CH_C2K,		CCCI_CCMNI3_DL_ACK,
	  CCCI_CCMNI3_DL_ACK,	"cc3mni2" },
	{ MDLOG_CTRL_CH_C2K,	MDLOG_CTRL_CH_C2K,	CCCI_UART1_TX,
	  CCCI_UART1_RX,	"ccci3_md_log_ctrl" },
	{ MDLOG_CH_C2K,		MDLOG_CH_C2K,		CCCI_MD_LOG_TX,
	  CCCI_MD_LOG_RX,	"ccci3_tty1" },
	{ FS_CH_C2K,		FS_CH_C2K,		CCCI_FS_TX,
	  CCCI_FS_RX,		"ccci3_fs" },
	{ DATA_PPP_CH_C2K,	DATA_PPP_CH_C2K,	CCCI_C2K_PPP_DATA,
	  CCCI_C2K_PPP_DATA,	"ccci3_data" },
	{ AT_CH_C2K,		AT_CH_C2K,		CCCI_C2K_AT,
	  CCCI_C2K_AT,		"ccci3_at" },
	{ AT2_CH_C2K,		AT2_CH_C2K,		CCCI_C2K_AT2,
	  CCCI_C2K_AT2,		"ccci3_at2" },
	{ AT3_CH_C2K,		AT3_CH_C2K,		CCCI_C2K_AT3,
	  CCCI_C2K_AT3,		"ccci3_at3" },
	{ AGPS_CH_C2K,		AGPS_CH_C2K,		CCCI_IPC_UART_TX,
	  CCCI_IPC_UART_RX,	"ccci3_tty2" },
	{ C2K_MD2AP_LOOPBACK_CH, C2K_MD2AP_LOOPBACK_CH,	CCCI_C2K_LB_DL,
	  CCCI_C2K_LB_DL,	"ccci3_lb_dl" },
	{ C2K_LOOPBACK_CH,	C2K_LOOPBACK_CH,	CCCI_LB_IT_TX,
	  CCCI_LB_IT_RX,	"ccci3_lb_it" },
	{ STATUS_CH_C2K,	STATUS_CH_C2K,		CCCI_CONTROL_TX,
	  CCCI_CONTROL_RX,	"ccci3_poll" },
};

/**
 * ccci_c2k_ch_to_ccci() - translate a C2K channel to a CCCI channel
 * @c2k_ch: enum c2k_channel value received from MD3
 * @tx: true for the AP->MD direction, false for MD->AP
 *
 * Returns the enum ccci_ch value, or -ENOENT if the channel is unknown,
 * mirroring c2k_ch_to_ccci_ch() in modem_ccif_c2k.c:215-238.
 */
int ccci_c2k_ch_to_ccci(int c2k_ch, bool tx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(c2k_ch_map); i++) {
		if (c2k_ch_map[i].c2k_ch == c2k_ch)
			return tx ? c2k_ch_map[i].tx_ch : c2k_ch_map[i].rx_ch;
	}

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(ccci_c2k_ch_to_ccci);

/**
 * ccci_c2k_ccci_to_ch() - translate a CCCI channel to a C2K channel
 * @ccci_ch: enum ccci_ch value
 * @tx: true for the AP->MD direction, false for MD->AP
 * @md_exception: true while MD3 is in exception state
 *
 * Mirrors ccci_ch_to_c2k_ch() in modem_ccif_c2k.c:240-267: the exception
 * channel is reported instead of the normal one while MD3 is down.
 */
int ccci_c2k_ccci_to_ch(int ccci_ch, bool tx, bool md_exception)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(c2k_ch_map); i++) {
		int mapped = tx ? c2k_ch_map[i].tx_ch : c2k_ch_map[i].rx_ch;

		if (mapped == ccci_ch)
			return md_exception ? c2k_ch_map[i].excp_ch :
					      c2k_ch_map[i].c2k_ch;
	}

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(ccci_c2k_ccci_to_ch);

/**
 * ccci_c2k_ch_dump() - print the MD3 channel map
 * @m: seq_file, e.g. the debugfs "channels" file
 */
int ccci_c2k_ch_dump(struct seq_file *m)
{
	int i;

	seq_puts(m, "c2k_ch  ccci_tx  ccci_rx  excp_ch  port\n");

	for (i = 0; i < ARRAY_SIZE(c2k_ch_map); i++)
		seq_printf(m, "%6d  %7d  %7d  %7d  %s\n",
			   c2k_ch_map[i].c2k_ch, c2k_ch_map[i].tx_ch,
			   c2k_ch_map[i].rx_ch, c2k_ch_map[i].excp_ch,
			   c2k_ch_map[i].name);

	return 0;
}

/**
 * ccci_c2k_set_irat() - point the MD3 IRAT peer at another modem
 * @irat_md_id: enum ccci_md_id of the IRAT peer
 *
 * The downstream C2K CCMNI instance is registered with .irat_md_id =
 * MD_SYS1 (port_net.c:289), so IRAT moves the packet interfaces between MD1
 * and MD3.
 */
int ccci_c2k_set_irat(unsigned int irat_md_id)
{
	struct ccci_c2k *c2k = ccci_c2k_get();
	int ret;

	if (!c2k)
		return -ENODEV;

	ret = ccci_md_set_irat(CCCI_MD_SYS3, irat_md_id);
	if (ret) {
		dev_err(c2k->dev, "set IRAT peer MD%d failed %d\n",
			irat_md_id + 1, ret);
		return ret;
	}

	c2k->irat_md_id = irat_md_id;
	dev_info(c2k->dev, "IRAT peer set to MD%d\n", irat_md_id + 1);
	return 0;
}

/**
 * ccci_c2k_sim_switch_mode_set() - store the dual-SIM switch mode
 * @mode: mode written through CCCI_IOC_SIM_SWITCH
 *
 * Storage half of ccci_store_sim_switch_mode() (ccci_support.c:45-54).  The
 * downstream implementation additionally sends CCCI_MD_MSG_CFG_UPDATE on the
 * MONITOR channel; that virtual message belongs to the core and has no API in
 * include/linux/ccci.h, see the report.
 */
void ccci_c2k_sim_switch_mode_set(int mode)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return;

	mutex_lock(&c2k->lock);
	if (c2k->sim_switch_mode != mode) {
		c2k->sim_switch_mode = mode;
		dev_info(c2k->dev, "sim switch mode 0x%x\n", mode);
	}
	mutex_unlock(&c2k->lock);
}

/**
 * ccci_c2k_sim_switch_mode_get() - read the stored dual-SIM switch mode
 *
 * Equivalent of ccci_get_sim_switch_mode() (ccci_support.c:56-59).
 */
int ccci_c2k_sim_switch_mode_get(void)
{
	struct ccci_c2k *c2k = ccci_c2k_get();
	int mode;

	if (!c2k)
		return 0;

	mutex_lock(&c2k->lock);
	mode = c2k->sim_switch_mode;
	mutex_unlock(&c2k->lock);

	return mode;
}
