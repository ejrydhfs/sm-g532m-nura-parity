# SPDX-License-Identifier: GPL-2.0
#
# MediaTek GPS driver (WMT GPS function + STP GPS channel).
#
ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/include

mtk_wcn_gps-y := gps.o stp_chrdev_gps.o
obj-$(CONFIG_MTK_WCN_GPS) += mtk_wcn_gps.o
