# SPDX-License-Identifier: GPL-2.0
#
# The object list is maintained by the WLAN driver.
#
ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/include

obj-$(CONFIG_MTK_WCN_WLAN) += mtk_wcn_wlan.o

mtk_wcn_wlan-y := wlan_main.o wlan_ahb.o wlan_fw.o \
		  wlan_mac.o wlan_netdev.o wlan_mgmt.o wlan_txrx.o wlan_cmd.o
