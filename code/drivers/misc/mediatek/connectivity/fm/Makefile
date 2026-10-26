# SPDX-License-Identifier: GPL-2.0
#
# The object list is maintained by the FM driver.
#
ccflags-y += -I$(srctree)/drivers/misc/mediatek/connectivity/include

obj-$(CONFIG_MTK_WCN_FM) += mtk_wcn_fm.o

mtk_wcn_fm-y := mtk_wcn_fm_link.o \
		mtk_wcn_fm_chip.o \
		mtk_wcn_fm_v4l2.o

# The ASoC glue (FM codec component; no sound card) is built when the SoC
# audio core is present and can be linked from this driver.  Without it the FM
# driver uses the chip level I2S hooks and the inline fallbacks of
# mtk_wcn_fm_i2s.h.  A built-in FM driver cannot reference the symbols of a
# modular snd-soc-core, so CONFIG_MTK_WCN_FM=y with CONFIG_SND_SOC=m takes the
# fallback path exactly like CONFIG_SND_SOC=n (mtk_wcn_fm_i2s.h implements the
# same rule for the C code).
ifeq ($(CONFIG_MTK_WCN_FM)$(CONFIG_SND_SOC),ym)
# no ASoC glue: snd-soc-core is a module and this driver is built in
else ifneq ($(CONFIG_SND_SOC),)
mtk_wcn_fm-y += mtk_wcn_fm_i2s.o
endif
