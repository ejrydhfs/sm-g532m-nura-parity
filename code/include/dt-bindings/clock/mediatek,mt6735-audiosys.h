/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_CLK_MT6735_AUDIOSYS_H
#define _DT_BINDINGS_CLK_MT6735_AUDIOSYS_H

/*
 * Port from Samsung/MTK downstream include/dt-bindings/clock/mt6735-clk.h:216-227
 * (AUDIO_SYS, audiosys). The numbering is part of the DT ABI: the ROM DTB
 * (rom_dtb_0.dts:3545-3562) references these ids directly.
 */
#define AUDIO_AFE		1
#define AUDIO_I2S		2
#define AUDIO_22M		3
#define AUDIO_24M		4
#define AUDIO_APLL2_TUNER	5
#define AUDIO_APLL_TUNER	6
#define AUDIO_ADC		7
#define AUDIO_DAC		8
#define AUDIO_DAC_PREDIS	9
#define AUDIO_TML		10
#define AUDIO_NR_CLK		11

#endif /* _DT_BINDINGS_CLK_MT6735_AUDIOSYS_H */
