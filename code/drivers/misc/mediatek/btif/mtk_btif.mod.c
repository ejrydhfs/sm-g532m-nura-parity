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

KSYMTAB_FUNC(mtk_wcn_btif_open, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_close, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_write, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_read, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_dpidle_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_rx_cb_register, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_wakeup_consys, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_loopback_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_dbg_ctrl, "_gpl", "");
KSYMTAB_FUNC(mtk_wcn_btif_parser_wmt_evt, "_gpl", "");

MODULE_INFO(depends, "");

MODULE_ALIAS("of:N*T*Cmediatek,btif");
MODULE_ALIAS("of:N*T*Cmediatek,btifC*");
