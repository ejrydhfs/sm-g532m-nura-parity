// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT/CONSYS core module lifecycle.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/wmt_dev.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * Bring-up order matters:
 *   1. register the CONSYS platform driver so that its device (and its
 *      power domain, clocks and regulators) is bound before the STP/WMT
 *      stack starts using it,
 *   2. initialise the STP engine,
 *   3. initialise the WMT library (configuration, platform, CONSYS shared
 *      memory),
 *   4. expose /dev/stpwmt and /dev/wmtWifi.
 */

#include <linux/module.h>

#include "wmt_local.h"

static int __init wmt_module_init(void)
{
	int ret;

	ret = wmt_plat_consys_driver_register();
	if (ret) {
		wmt_err("failed to register the CONSYS driver (%d)\n", ret);
		return ret;
	}

	ret = stp_drv_init();
	if (ret)
		goto err_consys;

	ret = wmt_lib_init();
	if (ret)
		goto err_stp;

	ret = wmt_dev_stpwmt_register();
	if (ret)
		goto err_lib;

	ret = mtk_wcn_wmt_wifi_init();
	if (ret)
		goto err_stpwmt;

	return 0;

err_stpwmt:
	wmt_dev_stpwmt_unregister();
err_lib:
	wmt_lib_deinit();
err_stp:
	stp_drv_exit();
err_consys:
	wmt_plat_consys_driver_unregister();
	return ret;
}

static void __exit wmt_module_exit(void)
{
	mtk_wcn_wmt_wifi_exit();
	wmt_dev_stpwmt_unregister();
	wmt_lib_deinit();
	stp_drv_exit();
	wmt_plat_consys_driver_unregister();
}

module_init(wmt_module_init);
module_exit(wmt_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MediaTek Inc WCN");
MODULE_DESCRIPTION("MediaTek WCN WMT/CONSYS core driver (STP, WMT, MT6735 CONSYS)");
