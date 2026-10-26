# camera-imx219 (Samsung grandpplte IMX219 V4L2 subdev)

## 1. Driver and files changed

| File | Change |
|---|---|
| `drivers/media/i2c/imx219.c` | Variant support (`samsung,grandpplte-imx219` before `sony,imx219`), Samsung mode set + per-mode register tables, per-mode pixel rate / link frequency, Samsung rail-sequenced power on/off, optional powerdown GPIO, Samsung orientation register mapping. Generic `sony,imx219` path unchanged. |

No Kconfig/Makefile changes (CONFIG_VIDEO_IMX219 already builds this file). No new files. No DTS edits.
EEPROM (`imx219_eeprom.c` / cam_cal) and autofocus (`dw9807-vcm.c`) are separate drivers and out of scope here.

## 2. ROM/downstream evidence

Downstream source root: `src/downstream-grandpplte/drivers/misc/mediatek/`.

`imgsensor/src/mt6735/imx219_mipi_raw/imx219mipiraw_Sensor.c`:

| Item | Line(s) |
|---|---|
| `imgsensor_info`: pre `pclk 278400000/ll 3448/fl 2691/startx 8/starty 0/3264x2456/300` | 71-82 |
| `cap` (same as pre) | 83-92 |
| `cap1` `pclk 224000000/ll 3448/fl 2691/startx 0/starty 0/3280x2456/240` | 93-103 |
| `normal_video` (same as pre) | 104-113 |
| `hs_video` `pclk 278400000/ll 3448/fl 673/816x460/1200` | 114-123 |
| `slim_video` `pclk 278400000/ll 3448/fl 5378/1640x1228/150` | 124-133 |
| `margin 5`, `min_shutter 2`, `max_frame_length 0xffff`, IHDR off, `sensor_mode_num 5` | 134-146 |
| `sensor_output_dataformat SENSOR_OUTPUT_FORMAT_RAW_R`, `mclk 24`, `MIPI_4_LANE`, `i2c_addr_table {0x34,0xff}` | 158-161 |
| Output windows (`SENSOR_WINSIZE_INFO_STRUCT`, x/y/out/crop) | 184-190 |
| ISP-gain (64 = 1x) → register 0x0157 table, 0..232 | 193-294 |
| `write_shutter` / `set_shutter`: frame length and shutter written in lines, even values | 356-410, 425-475 |
| `set_mirror_flip`: 0x0172 = 0x03 normal, 0x01 V, 0x02 H, 0x00 HV | 576-628 |
| `preview_setting` (3264x2456 mode registers) | 640-696 |
| `capture_setting` 24 fps branch (cap1) | 697-732 |
| `capture_setting` 30 fps branch (same data as preview) | 733-783 |
| `normal_video_setting` (same data as preview) | 824-886 |
| `hs_video_setting` | 887-948 |
| `slim_video_setting` | 950-1009 |
| `set_test_pattern_mode` (0x0600/0x0601 = 0x0002 for colour bars) | 1011-1109 |
| `get_imgsensor_id` / `open` (chip ID over 0x34 write address) | 1127-1154, 1173-1226 |

`imgsensor/src/mt6735/camera_hw/kd_camera_hw.c`:

| Item | Line(s) |
|---|---|
| Voltage macros `VOL_2800/1800/1500/1200/1000` | 213-217 |
| Pin polarity `IDX_PS_*`, `GPIO_OUT_ONE` = ON | 208-220 |
| `mtkcam_gpio_set()` (pinctrl states `cam0_rst0/1`, `cam0_pnd0/1`) | 156-207 |
| IMX219 power-on branch: PDN low, RST low, VCAMA 2.8 V, VCAMD **1.2 V**, VCAMAF 2.8 V, `mdelay(2)`, VCAMIO 1.8 V, `mdelay(3)`, MCLK on, `mdelay(1)`, PDN high, RST high | 487-531 |
| IMX219 power-off branch: AF off, `mdelay(1)`, MCLK off, `mdelay(1)`, PDN/RST low, `mdelay(1)`, IO off, `mdelay(1)`, D off, `mdelay(1)`, A off | 766-808 |
| Generic branch VCAMD for IMX219 = 1.2 V (same conclusion) | 558-570 |

Board DTS / ROM DTS:

| Item | Location |
|---|---|
| PMIC LDO labels `vcama/vcamaf/vcamd/vcamio` and `kd_camera_hw1` supply wiring | `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:640-699` |
| `camera_main@34`, `camera_main_af@18` on i2c0 | same file 765-780 |
| cam0 pinctrl: `cam0_rst0/1`, `cam0_pnd0/1` **all mux GPIO44** | `mt6737t_grandpplte_ltn_dtv_03.dts:514-545` |
| camera default state: GPIO44 `output-low` (CAM_RST_N), GPIO45 = `CMMCLK0` | same file 584-613 |
| `kd_camera_hw1` pinctrl-names list | same file 642-656 |
| ROM: `kd_camera_hw1@15008000` supplies + pinctrl states | `zip/rom_dtb_0.dts:3719-3749` |
| ROM: `i2c@11007000` / `camera_main@34` | `zip/rom_dtb_0.dts:3265-3299` |
| ROM: cam0 pinctrl nodes, `pins = <0x2c00>` (= pin 44) for all four states | `zip/rom_dtb_0.dts:2459-2500` |

Nura base: `mt6328.dtsi` LDO constraints allow vcama 1.5–2.8 V, vcamaf 1.2–3.3 V, vcamd 0.9–1.5 V,
vcamio 1.2–1.8 V, i.e. every ported voltage is settable. Seninf MCLK provider: `mtk-seninf.c`
(`#clock-cells=<1>`, clocks `mclk0/mclk1/mclk2`).

ROM symbols confirming the same code shipped in the GPL vmlinux (`zip/symbols.txt`):

| symbol | line |
|---|---|
| `CAMERA_HW_probe` (downstream `kd_camera_hw` equivalent) | 20808 |
| `mtkcam_gpio_set`, `kdCISModulePowerOn` | 20846, 20847 |
| `CAM_CAL_probe` (EEPROM, separate driver) | 20848 |
| `read_imx219_eeprom` | 20852 |
| `imx219_update_sysfs_information` | 20856 |
| `set_test_pattern_mode` | 20864 |
| `preview.isra.6`, `set_gain.part.8` | 20871, 20873 |
| `IMX219_MIPI_RAW_SensorInit` | 20875 |

ROM DT: `kd_camera_hw1@15008000` (rom_dtb_0.dts:3719), `camera_main@34` (3282), cam0 pinctrl
(2459-2500), `ispsys@15000000` (3705).

## 3. Parity matrix

Sizes below are the V4L2 format sizes offered for `samsung,grandpplte-imx219` (downstream
grabwindow sizes); the matching register tables are copied verbatim, so the sensor readout window
(larger) is exposed through the crop selection, exactly as downstream's
`SENSOR_WINSIZE_INFO_STRUCT` describes.

| behavior | ROM/downstream implementation | Nura before | after | status |
|---|---|---|---|---|
| DT binding | `camera_main@34` on i2c0, module driven by `kd_camera_hw` | only `sony,imx219` | `samsung,grandpplte-imx219` first, `sony,imx219` fallback, match data selects variant | done |
| Chip ID probe | `get_imgsensor_id`, ID 0x0219 @0x0000 | same | same (unchanged) | done |
| I2C address | `i2c_addr_table {0x34}` 8-bit = 0x1a 7-bit (Sensor.c:161, board dts:765) | DT dependent | `reg = <0x1a>` in fragment | done |
| Mode set | 5 scenarios: preview/cap/normal_video 3264x2456@30, cap1 3280x2456@24, hs 816x460@120, slim 1640x1228@15 | 4 generic modes | 4 Samsung formats covering the 5 scenarios (pre/cap/video share one table) | done |
| Mode registers | `preview/capture/normal_video/hs/slim` setting functions | derived from format (generic) | 4 verbatim `cci_reg_sequence` tables selected by format | done |
| Link frequency | output PLL `PLL_OP_MPY` 104 / 86 / 85 (0x030d) | 364 MHz (4-lane) only | Samsung menu 416/344/340 MHz, selected per mode | done |
| Pixel rate | `.pclk` 278.4 / 224 / 278.4 / 278.4 MHz | fixed 281.6 MHz (4-lane) | per-mode value, control updated on set_fmt | done |
| Frame length / vblank | FLL register written in lines, never scaled | FLL = (h+vblank)/rate_factor | Samsung: rate_factor 1, FLL = h+vblank, default = downstream FLL | done |
| Line length / hblank | fixed 3448 for every mode, never changed | LLP from format | Samsung default LLP = 3448 per mode, hblank min/default = LLP-width | done |
| Exposure | shutter in lines, min 2, max frame_len-5, frame length auto-extends | min 4, max FLL-4, no auto-extend | unchanged V4L2 model (min 4, exposure ≤ vblank); auto-extend not implemented | partial |
| Analogue gain | ISP gain (64 = 1x) → 0x0157 via 97-entry table (Sensor.c:193-294) | raw register 0..232 | unchanged (raw register 0..232, upstream semantics) | partial |
| Digital gain | not present downstream | 0x0158 control | unchanged | done |
| Flip | `set_mirror_flip`: normal 0x03, V 0x01, H 0x02, HV 0x00 | 0x0172 = h<<0 \| v<<1 (normal 0x00) | Samsung variant XORs with 0x03, reproducing downstream's four values; generic path unchanged | done |
| Test pattern | 0x0600/0x0601 = 0x0002; window 0x0624/26 hardcoded 3280x2464 | colour bars/solid/grey/PN9, window = active format | unchanged; window uses active format size | partial |
| Power on rails | A 2.8 → D 1.2 → AF 2.8 → 2 ms → IO 1.8 → 3 ms → MCLK → 1 ms → PDN high → RST high | bulk enable of 3 rails, clk, reset high | exact per-rail order, voltages and delays (kd_camera_hw.c:494-525) | done |
| Power off | AF off → MCLK off → PDN/RST low → IO → D → A, 1 ms each | RST low → rails off → clk off | exact downstream order (kd_camera_hw.c:767-808) | done |
| MCLK | ISP_MCLK1_EN (cam0), 24 MHz | `xclk` clock, 24 MHz required | same; `clocks = <&seninf 0>` per architecture | done |
| Reset / powerdown | separate pinctrl calls, both mux GPIO44 on this board | optional `reset` GPIO, active-high | optional `reset` (requested low) + optional `powerdown`, both active-high; fragment uses GPIO44 | partial |
| MIPI lanes | 4 lanes, fixed | 2 or 4 per DT, PLL written by lane table | Samsung variant requires 4 lanes (EINVAL otherwise), PLL in mode table | done |
| Streaming order | mode registers, then AE values, then 0x0100=1 | common, lanes, framefmt, ctrls, 0x0100=1 | Samsung: mode table, ctrls, 0x0100=1 (identical ordering) | done |
| Link-freq consistency | n/a | unchecked | stream-on: non-exact mode or link freq not advertised in DT → `-EINVAL` | done |
| MIPI LP2HS settle | `mipi_data_lp2hs_settle_dc 85 ns`, manual mode | none | not driven by the sensor; belongs to the CSI-2 receiver (seninf) | deferred |
| ISP pipeline / MTK HAL | libcamdrv + ISP firmware | none | out of scope by architecture (imgsys/seninf) | deferred |
| EEPROM / cam_cal | `imx219_eeprom.c`, CAM_CAL | none | separate driver | deferred |
| Autofocus | `dw9807` VCM | `dw9807-vcm.c` exists | separate driver | deferred |

Counts: 16 done, 4 partial, 4 deferred.

## 4. Compile-tested

```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-imx219"
cp "$M/nura.config" .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CAMERA_SUPPORT \
  -e MEDIA_CONTROLLER -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC \
  -e VIDEO_CAMERA_SENSOR -e VIDEO_IMX219 -e I2C -e REGULATOR -e COMMON_CLK -e V4L2_CCI_I2C
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/media/i2c/imx219.o 2>&1 | tee "$M/logs/camera-imx219-build.log"
```

Note: `nura.config` sets `CONFIG_MEDIA_SUPPORT_FILTER=y`, which hides `VIDEO_DEV`,
`VIDEO_V4L2_SUBDEV_API` etc. (`MEDIA_CAMERA_SUPPORT` is only prompted when filtering is on).
`MEDIA_SUPPORT_FILTER` therefore has to be disabled (or `MEDIA_CAMERA_SUPPORT` enabled) for the
required options to stick — with only the option list from the task, `olddefconfig` silently drops
them and the build fails on undeclared `v4l2_subdev_state_get_format()`.

Result:

```
  CC      drivers/media/i2c/imx219.o
```

0 errors, 0 warnings. `scripts/checkpatch.pl -f drivers/media/i2c/imx219.c`:
`total: 0 errors, 0 warnings, 2048 lines checked`.

## 5. Inferred / hardware-unverified items

1. **Link frequencies 416/344/340 MHz — [INFERENCE].** Not stated downstream. Derivation:
   `link_freq = INCK * PLL_OP_MPY / (2 * PREPLLCK_OP_DIV)` with `INCK = 24 MHz`
   (`imgsensor_info.mclk`, Sensor.c:159) and `PREPLLCK_OP_DIV = 3` (0x0305 = 0x03, the "AUTO"
   value the generic tables use). The formula reproduces both rates the existing driver trusts
   exactly (114 → 456 MHz 2-lane, 91 → 364 MHz 4-lane), and the Samsung mode tables program
   `PLL_OP_MPY` 0x68 / 0x56 / 0x55 (Sensor.c:688, 744, 936, 996) giving 416 / 344 / 340 MHz.
2. **VCAM_D voltage.** The task text said 1.0 V, but the IMX219 branch of `kdCISModulePowerOn`
   programs **1.2 V** (`_hwPowerOn(VCAMD, VOL_1200)`, kd_camera_hw.c:500; the generic else branch
   also selects 1.2 V for IMX219 at 558-570). The 1.0 V / 1-1-1-2 ms sequence belongs to the
   imx135/imx220 branch (kd_camera_hw.c:274-322). 1.2 V is used, per the actual IMX219 code.
3. **Reset and powerdown are the same pin.** All four cam0 pinctrl states (`rst0/rst1/pnd0/pnd1`)
   mux GPIO44 both in the board dts (514-545) and in the ROM DTB (2459-2500), so downstream
   physically drove one line. The fragment therefore requests only `reset-gpios` on GPIO44 and not
   `powerdown-gpios` (requesting the same line twice would fail with -EBUSY). If the module really
   has a separate PDN net, add `powerdown-gpios` and use a different pin — unverified without
   hardware/schematic.
4. **Reset polarity/level.** Downstream drives CAM_RST_N (GPIO44) `output-low` as its default state
   (board dts 606-613) and 1 = release. The fragment uses `GPIO_ACTIVE_HIGH` and the Samsung probe
   requests the line low.
5. **XCLR delay.** The downstream sequence has no long wait after reset release, but the sensor
   requires ≥ 6.2 ms before leaving software standby; the generic `IMX219_XCLR_MIN_DELAY_US`
   handling is kept on the Samsung path (superset of downstream, still required for the generic
   compatible).
6. **Delay granularity.** `mdelay(1/2/3)` is reproduced with `usleep_range(ms*1000, ms*1000+100)`,
   i.e. never shorter than downstream.
7. **Readout window vs format size.** The 3264x2456 mode's register table outputs 3280x2456
   (downstream's ISP crops 8 columns, `startx = 8`); the cap1 table outputs 3280x2464 while
   downstream reports grabwindow 3280x2456 (`starty 0`, 2456 rows). The register values are kept
   verbatim and the crop selection publishes the real readout window.
8. **Orientation.** Downstream treats 0x03 as "normal"; the driver keeps the mainline flip
   semantics and inverts the register value for the Samsung variant. Whether the resulting Bayer
   order for hflip/vflip combinations matches the module's physical mounting is unverified.
9. **Exposure / gain model.** V4L2 exposure stays in lines with min 4 (downstream min_shutter 2)
   and does not extend the frame length automatically (userspace raises vblank instead). Analogue
   gain stays the raw 0x0157 value (0..232); downstream's ISP-gain table (64 = 1x) is not
   reproduced because it is a userspace-facing convention.
10. **Test pattern window.** The driver writes 0x0624/0x0626 from the active format; downstream
    always writes 3280x2464 in the non-JPEG branch. Colour-bar value (0x0600/0x0601 = 0x0002)
    matches.
11. `MIPI_SETTLEDELAY_MANNUAL` (85 ns) is a receiver-side setting and is not programmed by this
    driver.

## DTS-FRAGMENT

```dts
/*
 * Samsung grandpplte (SM-G532M) IMX219 main camera.
 * - i2c0, 7-bit address 0x1a (downstream 8-bit write id 0x34,
 *   imx219mipiraw_Sensor.c:161, rom_dtb_0.dts:3282-3286)
 * - MCLK from the mtk-seninf provider (clocks = <&seninf 0>, mclk0 == CMMCLK0,
 *   mt6737t_grandpplte_ltn_dtv_03.dts:614-619)
 * - reset/powerdown: downstream muxes GPIO44 for both cam0 rst and pnd
 *   (mt6737t_grandpplte_ltn_dtv_03.dts:514-545, rom_dtb_0.dts:2459-2500),
 *   so only one descriptor is described here; powerdown-gpios is optional.
 */

&pio {
	imx219_pins_default: imx219-default-pins {
		pins-rst {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			bias-disable;
			output-low;
		};
	};
};

&i2c0 {
	status = "okay";

	imx219: camera-sensor@1a {
		compatible = "samsung,grandpplte-imx219", "sony,imx219";
		reg = <0x1a>;

		clocks = <&seninf 0>;
		clock-names = "xclk";
		clock-frequency = <24000000>;

		/* mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:681-698 */
		vcama-supply = <&mt6328_vcama_reg>;	/* 2.8 V, kd_camera_hw.c:495 */
		vcamd-supply = <&mt6328_vcamd_reg>;	/* 1.2 V, kd_camera_hw.c:500 */
		vcamaf-supply = <&mt6328_vcam_af_reg>;	/* 2.8 V, kd_camera_hw.c:506 */
		vcamio-supply = <&mt6328_vcam_io_reg>;	/* 1.8 V, kd_camera_hw.c:514 */

		pinctrl-names = "default";
		pinctrl-0 = <&imx219_pins_default>;

		/* active high: 1 = out of reset, kd_camera_hw.c:527-532 */
		reset-gpios = <&pio 44 GPIO_ACTIVE_HIGH>;

		port {
			imx219_out: endpoint {
				/* label must match the seninf node's port@0 endpoint */
				remote-endpoint = <&seninf_in0>;
				data-lanes = <1 2 3 4>;
				link-frequencies = /bits/ 64 <416000000 344000000 340000000>;
			};
		};
	};
};
```

`&seninf_in0` is a placeholder for the endpoint label created by the mtk-seninf DTS fragment
(port 0 = cam0); rename it there and here consistently. The seninf side must also mux GPIO45 as
`CMMCLK0` (downstream default state) and expose mclk0 at 24 MHz.

## 7. Patch path

`$M/patches/camera-imx219.patch`
