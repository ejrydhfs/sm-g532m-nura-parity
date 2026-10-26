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

KSYMTAB_FUNC(mtk_wcn_wlan_gen2_init, "", "");
KSYMTAB_FUNC(mtk_wcn_wlan_gen2_exit, "", "");

MODULE_INFO(depends, "mtk_wcn_wmt,rfkill,cfg80211");

MODULE_ALIAS("of:N*T*Cmediatek,wifi");
MODULE_ALIAS("of:N*T*Cmediatek,wifiC*");
MODULE_ALIAS("of:N*T*Cmediatek,WIFI");
MODULE_ALIAS("of:N*T*Cmediatek,WIFIC*");
