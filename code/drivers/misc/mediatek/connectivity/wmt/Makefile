# SPDX-License-Identifier: GPL-2.0
#
# The object list is maintained by the WMT/CONSYS core driver.
#
ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/btif/include

obj-$(CONFIG_MTK_WCN_WMT) += mtk_wcn_wmt.o

mtk_wcn_wmt-y := \
	wmt_main.o \
	stp_core.o \
	stp_btif.o \
	wmt_core.o \
	wmt_ctrl.o \
	wmt_func.o \
	wmt_lib.o \
	wmt_conf.o \
	wmt_ic_soc.o \
	consys_mt6735.o \
	stp_psm.o \
	stp_dbg.o \
	stp_btm.o \
	wmt_coex.o \
	wmt_dev.o \
	wmt_chrdev_wifi.o \
	wmt_api.o
