#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

MODULE_INFO(intree, "Y");

KSYMTAB_FUNC(mtk_wcn_stp_is_ready, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_send_data, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_receive_data, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_is_rxqueue_empty, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_parser_data, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_register_event_cb, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_register_tx_event_cb, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_register_if_rx, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_register_if_tx, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_set_bluez, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_coredump_start_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_coredump_start_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_coredump_flag_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_coredump_flag_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_hw_pwr_on, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_hw_pwr_off, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_hw_rst, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_emi_phys_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_emi_virt_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_soc_chipid_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_hw_init, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_consys_hw_deinit, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_enable, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_disable, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_reset, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_set_state, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_notify_stp, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_notify_wmt_wakeup, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_notify_wmt_sleep, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_sleep_for_thermal, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_dbg_level, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_stp_psm_is_quick_ps_support, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_coex_dbg, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_bgw_desense_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_bgw_ds_send, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_idc_register, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_idc_unregister, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_idc_is_registered, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_idc_send, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_wifi_init, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_wifi_exit, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_func_on, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_func_off, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_msgcb_reg, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_msgcb_unreg, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_wlan_reg, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_wlan_unreg, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_assert, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_system_state_reset, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_dsns_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_therm_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_hwver_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_hwver_raw_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_fwver_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_co_clock_flag_get, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_wmt_ic_info_get, "_gpl", "");

MODULE_INFO(depends, "mtk_btif");

MODULE_ALIAS("of:N*T*Cmediatek,mt6735-consys");
MODULE_ALIAS("of:N*T*Cmediatek,mt6735-consysC*");
