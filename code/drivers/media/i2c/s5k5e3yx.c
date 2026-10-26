// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K5E3YX 5MP MIPI RAW image sensor driver
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/imgsensor/src/mt6735/s5k5e3yx_mipi_raw/
 * (s5k5e3yxmipiraw_Sensor.c and s5k5e3yxmipiraw_Sensor.h) of the SM-G532M
 * (grandpplte, MT6737T) Android 6.0.1 kernel source drop.
 *
 * The register sequences, mode geometry, exposure/gain formulas and the
 * power-up/down ordering are taken from that driver. The per-sensor power
 * sequencing that downstream implemented in
 * drivers/misc/mediatek/imgsensor/src/mt6735/camera_hw/kd_camera_hw.c
 * (kdCISModulePowerOn(), S5K5E3YX branch) is implemented here in
 * s5k5e3yx_power_on()/s5k5e3yx_power_off().
 *
 * Hardware summary (downstream s5k5e3yxmipiraw_Sensor.c:144-151):
 * 24 MHz MCLK, 2 MIPI CSI-2 data lanes, RAW10, GRBG Bayer order.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Sensor identification, kd_imgsensor.h:184 */
#define S5K5E3YX_CHIP_ID			0x5e30

/*
 * Downstream used the 8-bit I2C write addresses 0x20 and 0x6c
 * (s5k5e3yxmipiraw_Sensor.c:151, .i2c_addr_table = {0x20, 0x6c, 0xff}).
 * iWriteRegI2C()/iReadRegI2C() shift the write address right by one before
 * handing it to the i2c core, so the 7-bit addresses are 0x10 and 0x36.
 */
#define S5K5E3YX_I2C_ADDR_1			0x10	/* 0x20 >> 1 */
#define S5K5E3YX_I2C_ADDR_2			0x36	/* 0x6c >> 1 */

#define S5K5E3YX_XCLK_FREQ			24000000	/* .mclk = 24 */

#define S5K5E3YX_NUM_SUPPLIES			3

/* Register map (16-bit register addresses, 8-bit register values) */
#define S5K5E3YX_REG_CHIP_ID_MSB		CCI_REG8(0x0000)
#define S5K5E3YX_REG_CHIP_ID_LSB		CCI_REG8(0x0001)
#define S5K5E3YX_REG_CTRL_MODE			CCI_REG8(0x0100)
#define S5K5E3YX_REG_EXPOSURE			CCI_REG16(0x0202)
#define S5K5E3YX_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define S5K5E3YX_REG_FRAME_LENGTH		CCI_REG16(0x0340)
#define S5K5E3YX_REG_LINE_LENGTH		CCI_REG16(0x0342)
#define S5K5E3YX_REG_TEST_PATTERN		CCI_REG8(0x0601)
/* 0x3200 is written to 0x00 together with 0x0601 by set_test_pattern_mode() */
#define S5K5E3YX_REG_TEST_PATTERN_CTRL		CCI_REG8(0x3200)

#define S5K5E3YX_MODE_STANDBY			0x00
#define S5K5E3YX_MODE_STREAMING			0x01

/*
 * Exposure/gain limits, s5k5e3yxmipiraw_Sensor.c:128-130 and :158:
 *   .margin = 4, .min_shutter = 1, .max_frame_length = 0xffff,
 *   .shutter = 0x3D0 (AE seed in the imgsensor struct; the mode register
 *   tables program their own coarse integration time, which is used as the
 *   control default instead).
 */
#define S5K5E3YX_EXPOSURE_MIN			1
#define S5K5E3YX_EXPOSURE_MARGIN		4
#define S5K5E3YX_FRAME_LENGTH_MAX		0xffff

/*
 * set_gain() (s5k5e3yxmipiraw_Sensor.c:474-493) takes a gain in the MTK AE
 * domain, whose unity is 0x40 ("sensor global gain(base: 0x40)", :465), and
 * writes reg_gain = gain >> 1 to 0x0204/0x0205, i.e. 0x0020 for 1x.
 * The downstream driver does not clamp the gain at all; the upper bound below
 * is the u16 domain of the downstream AE interface.
 */
#define S5K5E3YX_ANALOG_GAIN_MIN		0x40
#define S5K5E3YX_ANALOG_GAIN_MAX		0xffff
#define S5K5E3YX_ANALOG_GAIN_DEFAULT		0x40

/*
 * Link frequencies: 896 Mbps/lane (PLL 24 MHz / 6 * 224, PLLS = 0, and
 * 0x0820/0x0821 = 0x0380 = 896) for the 30 fps modes, and 436 Mbps/lane
 * (24 MHz / 6 * 218 / 2, PLLS = 1, 0x0820/0x0821 = 0x01b4 = 436) for the
 * 2560x1920 15 fps capture mode. V4L2 link frequency is the DDR clock, i.e.
 * half the bit rate.
 */
static const s64 s5k5e3yx_link_freq_menu[] = {
	448000000,
	218000000,
};

static const char * const s5k5e3yx_test_pattern_menu[] = {
	"Disabled",
	"Color bar",
};

struct s5k5e3yx_mode {
	u32 width;
	u32 height;
	u32 pclk;
	u32 line_length;
	u32 frame_length;
	u32 exposure_default;
	u32 native_width;
	u32 native_height;
	u32 crop_left;
	u32 crop_top;
	u32 crop_width;
	u32 crop_height;
	u32 link_freq_index;
	const struct cci_reg_sequence *regs;
	u32 num_regs;
};

/* sensor_init(), s5k5e3yxmipiraw_Sensor.c:593-700 (stream-on write at :699 stripped) */
static const struct cci_reg_sequence s5k5e3yx_init_regs[] = {
	{ CCI_REG8(0x3000), 0x04 },
	{ CCI_REG8(0x3002), 0x03 },
	{ CCI_REG8(0x3003), 0x04 },
	{ CCI_REG8(0x3004), 0x02 },
	{ CCI_REG8(0x3005), 0x00 },
	{ CCI_REG8(0x3006), 0x10 },
	{ CCI_REG8(0x3007), 0x03 },
	{ CCI_REG8(0x3008), 0x55 },
	{ CCI_REG8(0x3039), 0x00 },
	{ CCI_REG8(0x303a), 0x00 },
	{ CCI_REG8(0x303b), 0x00 },
	{ CCI_REG8(0x3009), 0x05 },
	{ CCI_REG8(0x300a), 0x55 },
	{ CCI_REG8(0x300b), 0x38 },
	{ CCI_REG8(0x300c), 0x10 },
	{ CCI_REG8(0x3012), 0x05 },
	{ CCI_REG8(0x3013), 0x00 },
	{ CCI_REG8(0x3014), 0x22 },
	{ CCI_REG8(0x300e), 0x79 },
	{ CCI_REG8(0x3010), 0x68 },
	{ CCI_REG8(0x3019), 0x03 },
	{ CCI_REG8(0x301a), 0x00 },
	{ CCI_REG8(0x301b), 0x06 },
	{ CCI_REG8(0x301c), 0x00 },
	{ CCI_REG8(0x301d), 0x22 },
	{ CCI_REG8(0x301e), 0x00 },
	{ CCI_REG8(0x301f), 0x10 },
	{ CCI_REG8(0x3020), 0x00 },
	{ CCI_REG8(0x3021), 0x00 },
	{ CCI_REG8(0x3022), 0x0a },
	{ CCI_REG8(0x3023), 0x1e },
	{ CCI_REG8(0x3024), 0x00 },
	{ CCI_REG8(0x3025), 0x00 },
	{ CCI_REG8(0x3026), 0x00 },
	{ CCI_REG8(0x3027), 0x00 },
	{ CCI_REG8(0x3028), 0x1a },
	{ CCI_REG8(0x3015), 0x00 },
	{ CCI_REG8(0x3016), 0x84 },
	{ CCI_REG8(0x3017), 0x00 },
	{ CCI_REG8(0x3018), 0xa0 },
	{ CCI_REG8(0x302b), 0x10 },
	{ CCI_REG8(0x302c), 0x0a },
	{ CCI_REG8(0x302d), 0x06 },
	{ CCI_REG8(0x302e), 0x05 },
	{ CCI_REG8(0x302f), 0x0e },
	{ CCI_REG8(0x3030), 0x2f },
	{ CCI_REG8(0x3031), 0x08 },
	{ CCI_REG8(0x3032), 0x05 },
	{ CCI_REG8(0x3033), 0x09 },
	{ CCI_REG8(0x3034), 0x05 },
	{ CCI_REG8(0x3035), 0x00 },
	{ CCI_REG8(0x3036), 0x00 },
	{ CCI_REG8(0x3037), 0x00 },
	{ CCI_REG8(0x3038), 0x00 },
	{ CCI_REG8(0x3088), 0x06 },
	{ CCI_REG8(0x308a), 0x08 },
	{ CCI_REG8(0x308c), 0x05 },
	{ CCI_REG8(0x308e), 0x07 },
	{ CCI_REG8(0x3090), 0x06 },
	{ CCI_REG8(0x3092), 0x08 },
	{ CCI_REG8(0x3094), 0x05 },
	{ CCI_REG8(0x3096), 0x21 },
	{ CCI_REG8(0x3099), 0x0e },
	{ CCI_REG8(0x3070), 0x10 },
	{ CCI_REG8(0x3085), 0x11 },
	{ CCI_REG8(0x3086), 0x01 },
	{ CCI_REG8(0x3064), 0x00 },
	{ CCI_REG8(0x3062), 0x08 },
	{ CCI_REG8(0x3061), 0x11 },
	{ CCI_REG8(0x307b), 0x20 },
	{ CCI_REG8(0x3068), 0x00 },
	{ CCI_REG8(0x3074), 0x00 },
	{ CCI_REG8(0x307d), 0x00 },
	{ CCI_REG8(0x3045), 0x01 },
	{ CCI_REG8(0x3046), 0x05 },
	{ CCI_REG8(0x3047), 0x78 },
	{ CCI_REG8(0x307f), 0xb1 },
	{ CCI_REG8(0x3098), 0x01 },
	{ CCI_REG8(0x305c), 0xf6 },
	{ CCI_REG8(0x306b), 0x10 },
	{ CCI_REG8(0x3063), 0x27 },
	{ CCI_REG8(0x3400), 0x01 },
	{ CCI_REG8(0x3235), 0x49 },
	{ CCI_REG8(0x3233), 0x00 },
	{ CCI_REG8(0x3234), 0x00 },
	{ CCI_REG8(0x3300), 0x0c },
	{ CCI_REG8(0x3203), 0x45 },
	{ CCI_REG8(0x3205), 0x4d },
	{ CCI_REG8(0x320b), 0x40 },
	{ CCI_REG8(0x320c), 0x06 },
	{ CCI_REG8(0x320d), 0xc0 },
	{ CCI_REG8(0x3929), 0x07 },
};

/* preview_setting(), :703-757 (identical to capture 30fps :777-890 and normal_video_setting :893-947; stream writes stripped) */
static const struct cci_reg_sequence s5k5e3yx_2576x1932_30_regs[] = {
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xe0 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x80 },
	{ CCI_REG8(0x3c1c), 0x58 },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xe9 },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x02 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x0f },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x8d },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x10 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x8c },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x00 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0202), 0x02 },
	{ CCI_REG8(0x0203), 0x00 },
	{ CCI_REG8(0x0200), 0x04 },
	{ CCI_REG8(0x0201), 0x98 },
	{ CCI_REG8(0x3941), 0x04 },
	{ CCI_REG8(0x3942), 0xb5 },
	{ CCI_REG8(0x3924), 0x2f },
	{ CCI_REG8(0x3925), 0x10 },
	{ CCI_REG8(0x3c31), 0x54 },
	{ CCI_REG8(0x3c32), 0x60 },
	{ CCI_REG8(0x3c08), 0x5d },
	{ CCI_REG8(0x3c09), 0xc0 },
	{ CCI_REG8(0x3320), 0x02 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
};

/* capture_setting(150), :759-836 (15 fps variant; stream writes stripped) */
static const struct cci_reg_sequence s5k5e3yx_2560x1920_15_regs[] = {
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xda },
	{ CCI_REG8(0x3c1f), 0x01 },
	{ CCI_REG8(0x0820), 0x01 },
	{ CCI_REG8(0x0821), 0xb4 },
	{ CCI_REG8(0x3c1c), 0x54 },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xe9 },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x07 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x80 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x20 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0202), 0x02 },
	{ CCI_REG8(0x0203), 0x00 },
	{ CCI_REG8(0x0200), 0x04 },
	{ CCI_REG8(0x0201), 0x98 },
	{ CCI_REG8(0x3407), 0x00 },
	{ CCI_REG8(0x3408), 0x00 },
	{ CCI_REG8(0x3409), 0x00 },
	{ CCI_REG8(0x340a), 0x00 },
	{ CCI_REG8(0x340b), 0x00 },
	{ CCI_REG8(0x340c), 0x00 },
	{ CCI_REG8(0x340d), 0x00 },
	{ CCI_REG8(0x340e), 0x00 },
	{ CCI_REG8(0x3401), 0x50 },
	{ CCI_REG8(0x3402), 0x3c },
	{ CCI_REG8(0x3403), 0x03 },
	{ CCI_REG8(0x3404), 0x33 },
	{ CCI_REG8(0x3405), 0x04 },
	{ CCI_REG8(0x3406), 0x44 },
	{ CCI_REG8(0x3458), 0x03 },
	{ CCI_REG8(0x3459), 0x33 },
	{ CCI_REG8(0x345a), 0x04 },
	{ CCI_REG8(0x345b), 0x44 },
	{ CCI_REG8(0x3400), 0x01 },
};

/* hs_video_setting(), :949-1005 (stream writes stripped) */
static const struct cci_reg_sequence s5k5e3yx_640x480_120_regs[] = {
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xe0 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x80 },
	{ CCI_REG8(0x3c1c), 0x58 },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0340), 0x01 },
	{ CCI_REG8(0x0341), 0xfa },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x07 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x034c), 0x02 },
	{ CCI_REG8(0x034d), 0x80 },
	{ CCI_REG8(0x034e), 0x01 },
	{ CCI_REG8(0x034f), 0xe0 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x44 },
	{ CCI_REG8(0x0383), 0x03 },
	{ CCI_REG8(0x0387), 0x07 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0202), 0x01 },
	{ CCI_REG8(0x0203), 0xf4 },
	{ CCI_REG8(0x0200), 0x04 },
	{ CCI_REG8(0x0201), 0x98 },
	{ CCI_REG8(0x3941), 0x04 },
	{ CCI_REG8(0x3942), 0xb5 },
	{ CCI_REG8(0x3924), 0x2f },
	{ CCI_REG8(0x3925), 0x10 },
	{ CCI_REG8(0x3c31), 0x54 },
	{ CCI_REG8(0x3c32), 0x60 },
	{ CCI_REG8(0x3c08), 0x5d },
	{ CCI_REG8(0x3c09), 0xc0 },
	{ CCI_REG8(0x3320), 0x02 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
};

/* slim_video_setting(), :1007-1062 (stream writes stripped) */
static const struct cci_reg_sequence s5k5e3yx_1280x960_15_regs[] = {
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xe0 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x80 },
	{ CCI_REG8(0x3c1c), 0x58 },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0340), 0x0f },
	{ CCI_REG8(0x0341), 0xd2 },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x07 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x034c), 0x05 },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x03 },
	{ CCI_REG8(0x034f), 0xc0 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0387), 0x03 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x20 },
	{ CCI_REG8(0x0202), 0x02 },
	{ CCI_REG8(0x0203), 0x00 },
	{ CCI_REG8(0x0200), 0x04 },
	{ CCI_REG8(0x0201), 0x98 },
	{ CCI_REG8(0x3941), 0x04 },
	{ CCI_REG8(0x3942), 0xb5 },
	{ CCI_REG8(0x3924), 0x2f },
	{ CCI_REG8(0x3925), 0x10 },
	{ CCI_REG8(0x3c31), 0x54 },
	{ CCI_REG8(0x3c32), 0x60 },
	{ CCI_REG8(0x3c08), 0x5d },
	{ CCI_REG8(0x3c09), 0xc0 },
	{ CCI_REG8(0x3320), 0x02 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
};


/*
 * Downstream mode table: imgsensor_info (pre/cap/normal_video, :61-105) plus
 * imgsensor_winsize_info (:172-179). pre, cap and normal_video are identical,
 * so a single mode entry covers the preview, capture (30 fps) and video
 * scenarios.
 */
static const struct s5k5e3yx_mode s5k5e3yx_modes[] = {
	{
		/* 2576x1932 30 fps, pre/cap/normal_video */
		.width = 2576,
		.height = 1932,
		.pclk = 179200000,
		.line_length = 2950,
		.frame_length = 2025,
		.exposure_default = 512,   /* 0x0202/0x0203 = 0x0200 */
		.native_width = 2576,
		.native_height = 1936,
		.crop_left = 0,
		.crop_top = 2,
		.crop_width = 2576,
		.crop_height = 1932,
		.link_freq_index = 0,
		.regs = s5k5e3yx_2576x1932_30_regs,
		.num_regs = ARRAY_SIZE(s5k5e3yx_2576x1932_30_regs),
	},
	{
		/* 2560x1920 15 fps capture (cap1, :83-94) */
		.width = 2560,
		.height = 1920,
		.pclk = 89616000,
		.line_length = 2950,
		.frame_length = 2025,
		.exposure_default = 512,   /* 0x0202/0x0203 = 0x0200 */
		.native_width = 2576,
		.native_height = 1936,
		.crop_left = 8,
		.crop_top = 8,
		.crop_width = 2560,
		.crop_height = 1920,
		.link_freq_index = 1,
		.regs = s5k5e3yx_2560x1920_15_regs,
		.num_regs = ARRAY_SIZE(s5k5e3yx_2560x1920_15_regs),
	},
	{
		/* 640x480 120 fps high speed video (:106-116) */
		.width = 640,
		.height = 480,
		.pclk = 179200000,
		.line_length = 2950,
		.frame_length = 506,
		.exposure_default = 500,   /* 0x0202/0x0203 = 0x01f4 */
		.native_width = 2576,
		.native_height = 1936,
		.crop_left = 8,
		.crop_top = 8,
		.crop_width = 2560,
		.crop_height = 1920,
		.link_freq_index = 0,
		.regs = s5k5e3yx_640x480_120_regs,
		.num_regs = ARRAY_SIZE(s5k5e3yx_640x480_120_regs),
	},
	{
		/* 1280x960 15 fps slim video (:117-127) */
		.width = 1280,
		.height = 960,
		.pclk = 179200000,
		.line_length = 2950,
		.frame_length = 4050,
		.exposure_default = 512,   /* 0x0202/0x0203 = 0x0200 */
		.native_width = 2576,
		.native_height = 1936,
		.crop_left = 8,
		.crop_top = 8,
		.crop_width = 2560,
		.crop_height = 1920,
		.link_freq_index = 0,
		.regs = s5k5e3yx_1280x960_15_regs,
		.num_regs = ARRAY_SIZE(s5k5e3yx_1280x960_15_regs),
	},
};

/*
 * Regulator order and voltages as used by the S5K5E3YX branch of
 * kdCISModulePowerOn(): VCAM_A 2.8 V, VCAM_D 1.2 V, VCAM_IO 1.8 V
 * (kd_camera_hw.c:335-351). The rails are the shared camera LDOs
 * (mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:681-696).
 */
static const struct {
	const char *supply;
	u32 min_uV;
	u32 max_uV;
} s5k5e3yx_supplies[S5K5E3YX_NUM_SUPPLIES] = {
	{ "vcama",  2800000, 2800000 },
	{ "vcamd",  1200000, 1200000 },
	{ "vcamio", 1800000, 1800000 },
};

struct s5k5e3yx {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct regmap *regmap;
	struct clk *xclk;

	struct gpio_desc *reset_gpio;

	struct regulator_bulk_data supplies[S5K5E3YX_NUM_SUPPLIES];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	const struct s5k5e3yx_mode *cur_mode;

	/* Serialises the OTP page reads against the streaming sequences. */
	struct mutex otp_lock;
};

static inline struct s5k5e3yx *to_s5k5e3yx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k5e3yx, sd);
}

/* -------------------------------------------------------------------------
 * Controls
 */

static int s5k5e3yx_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k5e3yx *s = container_of(ctrl->handler, struct s5k5e3yx,
					  ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	const struct s5k5e3yx_mode *mode = s->cur_mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		int exposure_max, exposure_def;

		/* write_shutter(): frame_length = max(shutter + margin, ...) */
		exposure_max = mode->height + ctrl->val - S5K5E3YX_EXPOSURE_MARGIN;
		exposure_def = min_t(int, mode->exposure_default, exposure_max);
		ret = __v4l2_ctrl_modify_range(s->exposure,
					       s->exposure->minimum, exposure_max,
					       s->exposure->step, exposure_def);
		if (ret)
			return ret;
	}

	/*
	 * Applying V4L2 control values only happens when power is up for
	 * streaming.
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		/* set_gain(): reg_gain = gain >> 1 -> 0x0204/0x0205 */
		cci_write(s->regmap, S5K5E3YX_REG_ANALOG_GAIN, ctrl->val >> 1,
			  &ret);
		break;
	case V4L2_CID_EXPOSURE:
		/* write_shutter(): coarse integration time, 0x0202/0x0203 */
		cci_write(s->regmap, S5K5E3YX_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		/*
		 * write_shutter()/set_dummy(): frame_length_lines goes to
		 * 0x0340/0x0341 before the shutter is updated. The exposure
		 * range was reduced above, so use the (possibly clamped)
		 * exposure value.
		 */
		cci_write(s->regmap, S5K5E3YX_REG_FRAME_LENGTH,
			  mode->height + ctrl->val, &ret);
		cci_write(s->regmap, S5K5E3YX_REG_EXPOSURE, s->exposure->val,
			  &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		/* set_test_pattern_mode(), :1657-1676 */
		cci_write(s->regmap, S5K5E3YX_REG_TEST_PATTERN, ctrl->val, &ret);
		cci_write(s->regmap, S5K5E3YX_REG_TEST_PATTERN_CTRL, 0x00, &ret);
		break;
	case V4L2_CID_LINK_FREQ:
	case V4L2_CID_PIXEL_RATE:
		/* Selected by the mode, nothing to program */
		break;
	default:
		dev_err(&client->dev, "ctrl 0x%x not handled\n", ctrl->id);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k5e3yx_ctrl_ops = {
	.s_ctrl = s5k5e3yx_set_ctrl,
};

static int s5k5e3yx_init_controls(struct s5k5e3yx *s)
{
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	const struct s5k5e3yx_mode *mode = &s5k5e3yx_modes[0];
	struct v4l2_ctrl_handler *ctrl_hdlr = &s->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	int exposure_max, exposure_def;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	s->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
					  V4L2_CID_PIXEL_RATE, mode->pclk,
					  mode->pclk, 1, mode->pclk);

	s->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
					      &s5k5e3yx_ctrl_ops,
					      V4L2_CID_LINK_FREQ,
					      ARRAY_SIZE(s5k5e3yx_link_freq_menu) - 1,
					      mode->link_freq_index,
					      s5k5e3yx_link_freq_menu);
	if (s->link_freq)
		s->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/*
	 * min_frame_length is the mode frame length (s5k5e3yxmipiraw_Sensor.c
	 * :1180), max_frame_length is 0xffff (:130), so blanking is
	 * expressed relative to those two limits here.
	 */
	s->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
				      V4L2_CID_VBLANK,
				      mode->frame_length - mode->height,
				      S5K5E3YX_FRAME_LENGTH_MAX - mode->height, 1,
				      mode->frame_length - mode->height);

	/* The sensor has a single, fixed line length (0x0342/0x0343 = 2950) */
	s->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
				      V4L2_CID_HBLANK,
				      mode->line_length - mode->width,
				      mode->line_length - mode->width, 1,
				      mode->line_length - mode->width);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exposure_max = mode->frame_length - S5K5E3YX_EXPOSURE_MARGIN;
	exposure_def = min_t(int, mode->exposure_default, exposure_max);
	s->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
					V4L2_CID_EXPOSURE,
					S5K5E3YX_EXPOSURE_MIN, exposure_max, 1,
					exposure_def);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, S5K5E3YX_ANALOG_GAIN_MIN,
			  S5K5E3YX_ANALOG_GAIN_MAX, 1,
			  S5K5E3YX_ANALOG_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k5e3yx_test_pattern_menu) - 1,
				     0, 0, s5k5e3yx_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err_probe(&client->dev, ret, "control init failed\n");
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k5e3yx_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	s->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

/* -------------------------------------------------------------------------
 * Subdev operations
 */

static const struct s5k5e3yx_mode *
s5k5e3yx_find_mode(struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format =
		v4l2_subdev_state_get_format(state, 0);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s5k5e3yx_modes); i++)
		if (s5k5e3yx_modes[i].width == format->width &&
		    s5k5e3yx_modes[i].height == format->height)
			return &s5k5e3yx_modes[i];

	return &s5k5e3yx_modes[0];
}

static int s5k5e3yx_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad != 0 || code->index != 0)
		return -EINVAL;

	/* SENSOR_OUTPUT_FORMAT_RAW_Gr (s5k5e3yxmipiraw_Sensor.c:148) */
	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int s5k5e3yx_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0 || fse->index >= ARRAY_SIZE(s5k5e3yx_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = s5k5e3yx_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k5e3yx_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k5e3yx_set_pad_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_format *fmt)
{
	struct s5k5e3yx *s = to_s5k5e3yx(sd);
	const struct s5k5e3yx_mode *mode;
	struct v4l2_mbus_framefmt *format;
	int exposure_max, exposure_def;
	int ret;

	mode = v4l2_find_nearest_size(s5k5e3yx_modes,
				      ARRAY_SIZE(s5k5e3yx_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	fmt->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	format = v4l2_subdev_state_get_format(state, 0);
	*format = fmt->format;

	if (fmt->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return 0;

	s->cur_mode = mode;

	/* Frame length limits come from the mode (min) and 0xffff (max) */
	ret = __v4l2_ctrl_modify_range(s->vblank,
				       mode->frame_length - mode->height,
				       S5K5E3YX_FRAME_LENGTH_MAX - mode->height, 1,
				       mode->frame_length - mode->height);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(s->vblank, mode->frame_length - mode->height);
	if (ret)
		return ret;

	exposure_max = mode->frame_length - S5K5E3YX_EXPOSURE_MARGIN;
	exposure_def = min_t(int, mode->exposure_default, exposure_max);
	ret = __v4l2_ctrl_modify_range(s->exposure,
				       s->exposure->minimum, exposure_max,
				       s->exposure->step, exposure_def);
	if (ret)
		return ret;

	/* Line length never changes: 0x0342/0x0343 = 2950 in every mode */
	ret = __v4l2_ctrl_modify_range(s->hblank,
				       mode->line_length - mode->width,
				       mode->line_length - mode->width, 1,
				       mode->line_length - mode->width);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(s->pixel_rate, mode->pclk, mode->pclk,
				       1, mode->pclk);
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(s->link_freq, mode->link_freq_index);
}

static int s5k5e3yx_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	const struct s5k5e3yx_mode *mode = s5k5e3yx_find_mode(state);

	if (sel->pad != 0)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = mode->native_width;
		sel->r.height = mode->native_height;
		return 0;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		/*
		 * The crop window is part of the sensor mode tables
		 * (imgsensor_winsize_info, :172-179); the downstream driver
		 * exposes no crop control.
		 */
		sel->r.left = mode->crop_left;
		sel->r.top = mode->crop_top;
		sel->r.width = mode->crop_width;
		sel->r.height = mode->crop_height;
		return 0;
	}

	return -EINVAL;
}

static int s5k5e3yx_set_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	/*
	 * The crop rectangle is fixed by the mode register tables, so a
	 * selection request can only report it back.
	 */
	return s5k5e3yx_get_selection(sd, state, sel);
}

static int s5k5e3yx_enable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct s5k5e3yx *s = to_s5k5e3yx(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	const struct s5k5e3yx_mode *mode = s->cur_mode;
	int ret;

	if (pad != 0)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	mutex_lock(&s->otp_lock);

	/* All downstream mode tables start by stopping the stream */
	ret = cci_write(s->regmap, S5K5E3YX_REG_CTRL_MODE,
			S5K5E3YX_MODE_STANDBY, NULL);
	if (ret)
		goto err_unlock_put;

	/*
	 * The 30 fps capture path waits 40 ms between stream off and the
	 * register writes (capture_setting(), :840 mdelay(40)). Applied to
	 * every mode; the other mode setups have no delay between the two.
	 */
	msleep(40);

	ret = cci_multi_reg_write(s->regmap, s5k5e3yx_init_regs,
				  ARRAY_SIZE(s5k5e3yx_init_regs), NULL);
	if (ret)
		goto err_unlock_put;

	ret = cci_multi_reg_write(s->regmap, mode->regs, mode->num_regs, NULL);
	if (ret)
		goto err_unlock_put;

	ret = cci_write(s->regmap, S5K5E3YX_REG_CTRL_MODE,
			S5K5E3YX_MODE_STREAMING, NULL);
	if (ret)
		goto err_unlock_put;

	mutex_unlock(&s->otp_lock);

	return 0;

err_unlock_put:
	mutex_unlock(&s->otp_lock);
	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static int s5k5e3yx_disable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state, u32 pad,
				    u64 streams_mask)
{
	struct s5k5e3yx *s = to_s5k5e3yx(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	int ret;

	if (pad != 0)
		return -EINVAL;

	mutex_lock(&s->otp_lock);
	ret = cci_write(s->regmap, S5K5E3YX_REG_CTRL_MODE,
			S5K5E3YX_MODE_STANDBY, NULL);
	mutex_unlock(&s->otp_lock);
	if (ret)
		dev_err(&client->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static int s5k5e3yx_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SGRBG10_1X10,
			.width = s5k5e3yx_modes[0].width,
			.height = s5k5e3yx_modes[0].height,
		},
	};

	return s5k5e3yx_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k5e3yx_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k5e3yx_pad_ops = {
	.enum_mbus_code = s5k5e3yx_enum_mbus_code,
	.enum_frame_size = s5k5e3yx_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k5e3yx_set_pad_format,
	.get_selection = s5k5e3yx_get_selection,
	.set_selection = s5k5e3yx_set_selection,
	.enable_streams = s5k5e3yx_enable_streams,
	.disable_streams = s5k5e3yx_disable_streams,
};

static const struct v4l2_subdev_ops s5k5e3yx_subdev_ops = {
	.video = &s5k5e3yx_video_ops,
	.pad = &s5k5e3yx_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k5e3yx_internal_ops = {
	.init_state = s5k5e3yx_init_state,
};

/* -------------------------------------------------------------------------
 * Power management
 *
 * Sequence from the S5K5E3YX branch of kdCISModulePowerOn():
 *   power on  (kd_camera_hw.c:325-366)
 *     CMPDN low, CMRST low -> VCAMA 2.8 V -> VCAMD 1.2 V -> VCAMIO 1.8 V
 *     -> 1 ms -> CMRST high, CMPDN high -> 1 ms -> ISP_MCLK2_EN(1)
 *   power off (kd_camera_hw.c:642-683)
 *     ISP_MCLK2_EN(0) -> 1 ms -> CMPDN low, CMRST low -> 1 ms
 *     -> VCAM_IO off -> 1 ms -> VCAM_D off -> 1 ms -> VCAM_A off
 *
 * The sub camera reset and powerdown pinctrl states both drive GPIO12
 * (mt6737t_grandpplte_ltn_dtv_03.dts:542-569), so a single active-high
 * reset GPIO reproduces both signals.
 */

static int s5k5e3yx_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e3yx *s = to_s5k5e3yx(sd);
	unsigned int i;
	int ret;

	/* "First Power Pin low and Reset Pin Low" */
	gpiod_set_value_cansleep(s->reset_gpio, 0);

	for (i = 0; i < S5K5E3YX_NUM_SUPPLIES; i++) {
		ret = regulator_set_voltage(s->supplies[i].consumer,
					    s5k5e3yx_supplies[i].min_uV,
					    s5k5e3yx_supplies[i].max_uV);
		if (ret) {
			dev_err(dev, "failed to set %s voltage: %d\n",
				s5k5e3yx_supplies[i].supply, ret);
			goto err_disable;
		}

		ret = regulator_enable(s->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				s5k5e3yx_supplies[i].supply, ret);
			goto err_disable;
		}
	}

	usleep_range(1000, 1100);

	/* "enable active sensor" */
	gpiod_set_value_cansleep(s->reset_gpio, 1);

	usleep_range(1000, 1100);

	/* ISP_MCLK2_EN(1) */
	ret = clk_prepare_enable(s->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		gpiod_set_value_cansleep(s->reset_gpio, 0);
		i = S5K5E3YX_NUM_SUPPLIES;
		goto err_disable;
	}

	return 0;

err_disable:
	while (i--)
		regulator_disable(s->supplies[i].consumer);

	return ret;
}

static int s5k5e3yx_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e3yx *s = to_s5k5e3yx(sd);

	/* ISP_MCLK2_EN(0) */
	clk_disable_unprepare(s->xclk);

	usleep_range(1000, 1100);

	gpiod_set_value_cansleep(s->reset_gpio, 0);

	usleep_range(1000, 1100);

	regulator_disable(s->supplies[2].consumer);	/* VCAM_IO */
	usleep_range(1000, 1100);
	regulator_disable(s->supplies[1].consumer);	/* VCAM_D */
	usleep_range(1000, 1100);
	regulator_disable(s->supplies[0].consumer);	/* VCAM_A */

	return 0;
}

/* -------------------------------------------------------------------------
 * OTP sysfs attribute
 *
 * Port of get_otps_data() (s5k5e3yxmipiraw_Sensor.c:392-414).  The sensor's
 * NVM controller exposes one five-byte page at a time through registers
 * 0x0a04..0x0a08; the sequence below is the downstream one unchanged:
 *
 *   0x0a00 = 0x04   initial state
 *   0x0a02 = page   0..3
 *   0x0a00 = 0x01   read mode
 *   delay 1 ms      "to wait Tmin = 47us" (:400)
 *   read 0x0a04..0x0a08 as one burst
 *   0x0a00 = 0x04
 *   0x0a00 = 0x00
 *
 * The attribute is read-only and refuses to run while the sensor streams, so
 * a calibration read can never disturb a capture.
 */

#define S5K5E3YX_REG_NVM_CTRL			CCI_REG8(0x0a00)
#define S5K5E3YX_REG_NVM_PAGE			CCI_REG8(0x0a02)
#define S5K5E3YX_REG_NVM_DATA			CCI_REG8(0x0a04)

#define S5K5E3YX_NVM_CTRL_RESET			0x04
#define S5K5E3YX_NVM_CTRL_READ			0x01
#define S5K5E3YX_NVM_CTRL_OFF			0x00

#define S5K5E3YX_OTP_PAGES			4
#define S5K5E3YX_OTP_PAGE_BYTES			5
/* Four lines of "pageN:" plus five "0aXX=VV" pairs fit in 256 bytes. */
#define S5K5E3YX_OTP_TEXT_MAX			256

static int s5k5e3yx_read_otp_page(struct s5k5e3yx *s, unsigned int page,
				  u8 *data)
{
	int ret;

	/* s5k5e3yxmipiraw_Sensor.c:395-397 */
	cci_write(s->regmap, S5K5E3YX_REG_NVM_CTRL, S5K5E3YX_NVM_CTRL_RESET,
		  &ret);
	cci_write(s->regmap, S5K5E3YX_REG_NVM_PAGE, page, &ret);
	cci_write(s->regmap, S5K5E3YX_REG_NVM_CTRL, S5K5E3YX_NVM_CTRL_READ,
		  &ret);
	if (ret)
		goto out_idle;

	usleep_range(1000, 1100);

	ret = regmap_bulk_read(s->regmap,
			       CCI_REG_ADDR(S5K5E3YX_REG_NVM_DATA), data,
			       S5K5E3YX_OTP_PAGE_BYTES);

out_idle:
	/*
	 * s5k5e3yxmipiraw_Sensor.c:408-409: the controller always goes back to
	 * its initial state, so this runs on the error path as well.
	 */
	cci_write(s->regmap, S5K5E3YX_REG_NVM_CTRL, S5K5E3YX_NVM_CTRL_RESET,
		  NULL);
	cci_write(s->regmap, S5K5E3YX_REG_NVM_CTRL, S5K5E3YX_NVM_CTRL_OFF,
		  NULL);

	return ret;
}

static ssize_t otp_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e3yx *s = to_s5k5e3yx(sd);
	u8 raw[S5K5E3YX_OTP_PAGES][S5K5E3YX_OTP_PAGE_BYTES];
	unsigned int page, i;
	ssize_t len = 0;
	u64 mode;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	mutex_lock(&s->otp_lock);

	ret = cci_read(s->regmap, S5K5E3YX_REG_CTRL_MODE, &mode, NULL);
	if (ret)
		goto out_unlock;

	if (mode == S5K5E3YX_MODE_STREAMING) {
		ret = -EBUSY;
		goto out_unlock;
	}

	for (page = 0; page < S5K5E3YX_OTP_PAGES; page++) {
		ret = s5k5e3yx_read_otp_page(s, page, raw[page]);
		if (ret)
			goto out_unlock;
	}

	for (page = 0; page < S5K5E3YX_OTP_PAGES; page++) {
		len += scnprintf(buf + len, S5K5E3YX_OTP_TEXT_MAX - len,
				 "page%u:", page);
		for (i = 0; i < S5K5E3YX_OTP_PAGE_BYTES; i++)
			len += scnprintf(buf + len, S5K5E3YX_OTP_TEXT_MAX - len,
					 " %04x=%02x",
					 (unsigned int)CCI_REG_ADDR(
						S5K5E3YX_REG_NVM_DATA) + i,
					 raw[page][i]);
		len += scnprintf(buf + len, S5K5E3YX_OTP_TEXT_MAX - len, "\n");
	}

	ret = len;

out_unlock:
	mutex_unlock(&s->otp_lock);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static DEVICE_ATTR_RO(otp);

static struct attribute *s5k5e3yx_attrs[] = {
	&dev_attr_otp.attr,
	NULL
};
ATTRIBUTE_GROUPS(s5k5e3yx);

/* -------------------------------------------------------------------------
 * Probe & remove
 */

static int s5k5e3yx_get_regulators(struct s5k5e3yx *s)
{
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	unsigned int i;

	for (i = 0; i < S5K5E3YX_NUM_SUPPLIES; i++)
		s->supplies[i].supply = s5k5e3yx_supplies[i].supply;

	return devm_regulator_bulk_get(&client->dev, S5K5E3YX_NUM_SUPPLIES,
				       s->supplies);
}

static int s5k5e3yx_read_chip_id(struct s5k5e3yx *s, u32 *id)
{
	u64 msb, lsb;
	int ret;

	/* return_sensor_id(): read 0x0000 and 0x0001, MSB first (:216-220) */
	ret = cci_read(s->regmap, S5K5E3YX_REG_CHIP_ID_MSB, &msb, NULL);
	if (ret)
		return ret;

	ret = cci_read(s->regmap, S5K5E3YX_REG_CHIP_ID_LSB, &lsb, NULL);
	if (ret)
		return ret;

	*id = (msb << 8) | lsb;

	return 0;
}

static int s5k5e3yx_identify_module(struct s5k5e3yx *s)
{
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	unsigned short addr = client->addr;
	unsigned short alt_addr;
	unsigned int attempt, retry;
	u32 id = 0;

	alt_addr = (addr == S5K5E3YX_I2C_ADDR_1) ? S5K5E3YX_I2C_ADDR_2 :
		   (addr == S5K5E3YX_I2C_ADDR_2) ? S5K5E3YX_I2C_ADDR_1 : 0;

	/*
	 * get_imgsensor_id()/open(): try every address of the downstream
	 * i2c_addr_table, two read attempts per address (:1082-1125, :1128-1190)
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		for (retry = 0; retry < 2; retry++) {
			if (!s5k5e3yx_read_chip_id(s, &id) &&
			    id == S5K5E3YX_CHIP_ID) {
				dev_info(&client->dev,
					 "i2c address 0x%02x, sensor id 0x%04x\n",
					 client->addr, id);
				return 0;
			}
		}

		if (!alt_addr)
			break;

		dev_dbg(&client->dev, "no sensor at 0x%02x, trying 0x%02x\n",
			client->addr, alt_addr);
		client->addr = alt_addr;
		alt_addr = 0;
	}

	dev_err(&client->dev, "sensor id mismatch, read 0x%04x\n", id);

	return -ENODEV;
}

static int s5k5e3yx_check_hwcfg(struct device *dev, struct s5k5e3yx *s)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long link_freq_bitmap;
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "endpoint node not found\n");

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err_probe(dev, -EINVAL, "could not parse endpoint\n");
		goto error_out;
	}

	/* .mipi_lane_num = SENSOR_MIPI_2_LANE (:150) */
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err_probe(dev, -EINVAL, "only 2 data lanes are supported\n");
		goto error_out;
	}

	ret = v4l2_link_freq_to_bitmap(dev, ep_cfg.link_frequencies,
				       ep_cfg.nr_of_link_frequencies,
				       s5k5e3yx_link_freq_menu,
				       ARRAY_SIZE(s5k5e3yx_link_freq_menu),
				       &link_freq_bitmap);
	if (ret || !link_freq_bitmap) {
		ret = -EINVAL;
		dev_err_probe(dev, -EINVAL, "link frequency not supported\n");
	}

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static int s5k5e3yx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k5e3yx *s;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->otp_lock);

	v4l2_i2c_subdev_init(&s->sd, client, &s5k5e3yx_subdev_ops);
	s->sd.internal_ops = &s5k5e3yx_internal_ops;

	ret = s5k5e3yx_check_hwcfg(dev, s);
	if (ret)
		return ret;

	s->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s->regmap))
		return dev_err_probe(dev, PTR_ERR(s->regmap),
				     "failed to initialize CCI\n");

	/* MCLK provided by the MTK SENINF per-camera clock */
	s->xclk = devm_v4l2_sensor_clk_get(dev, "xclk");
	if (IS_ERR(s->xclk))
		return dev_err_probe(dev, PTR_ERR(s->xclk),
				     "failed to get xclk\n");

	if (clk_get_rate(s->xclk) != S5K5E3YX_XCLK_FREQ)
		dev_warn(dev, "xclk is %lu Hz, expected %u Hz\n",
			 clk_get_rate(s->xclk), S5K5E3YX_XCLK_FREQ);

	ret = s5k5e3yx_get_regulators(s);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	s->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(s->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s->reset_gpio),
				     "failed to get reset gpio\n");

	/* The sensor must be powered to read the chip ID */
	ret = s5k5e3yx_power_on(dev);
	if (ret)
		return ret;

	ret = s5k5e3yx_identify_module(s);
	if (ret)
		goto error_power_off;

	s->cur_mode = &s5k5e3yx_modes[0];

	ret = s5k5e3yx_init_controls(s);
	if (ret)
		goto error_power_off;

	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init entity pads\n");
		goto error_handler_free;
	}

	s->sd.state_lock = s->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&s->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	ret = device_add_groups(dev, s5k5e3yx_groups);
	if (ret) {
		dev_err_probe(dev, ret, "failed to add the otp attribute\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to register sensor sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	return 0;

error_subdev_cleanup:
	device_remove_groups(dev, s5k5e3yx_groups);
	v4l2_subdev_cleanup(&s->sd);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

error_media_entity:
	media_entity_cleanup(&s->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(&s->ctrl_handler);

error_power_off:
	s5k5e3yx_power_off(dev);

	return ret;
}

static void s5k5e3yx_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k5e3yx *s = to_s5k5e3yx(sd);

	v4l2_async_unregister_subdev(sd);
	device_remove_groups(&client->dev, s5k5e3yx_groups);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		s5k5e3yx_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static const struct of_device_id s5k5e3yx_dt_ids[] = {
	{ .compatible = "samsung,s5k5e3yx" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k5e3yx_dt_ids);

static const struct dev_pm_ops s5k5e3yx_pm_ops = {
	SET_RUNTIME_PM_OPS(s5k5e3yx_power_off, s5k5e3yx_power_on, NULL)
};

static struct i2c_driver s5k5e3yx_i2c_driver = {
	.driver = {
		.name = "s5k5e3yx",
		.of_match_table = s5k5e3yx_dt_ids,
		.pm = &s5k5e3yx_pm_ops,
	},
	.probe = s5k5e3yx_probe,
	.remove = s5k5e3yx_remove,
};

module_i2c_driver(s5k5e3yx_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K5E3YX sensor driver");
MODULE_LICENSE("GPL");
