# S5K5E3YX 5MP MIPI RAW sensor driver (front camera) — port report

## 1. Driver and files changed

Worktree: `$M/worktrees/camera-s5k5e3yx` (base 046883c0c, not committed)

| File | Change |
|---|---|
| `drivers/media/i2c/s5k5e3yx.c` | **new** (1343 lines): self-contained I2C V4L2 subdev, CCI/regmap register access, chip-ID probe, runtime-PM power on/off, 4 mode register tables covering the 5 downstream scenarios, V4L2 controls, async sensor registration |
| `drivers/media/i2c/Kconfig` | **+10**: `config VIDEO_S5K5E3YX` (tristate, `select V4L2_CCI_I2C`), placed between `VIDEO_S5K5BAF` and `VIDEO_S5K6A3` inside `if VIDEO_CAMERA_SENSOR` |
| `drivers/media/i2c/Makefile` | **+1**: `obj-$(CONFIG_VIDEO_S5K5E3YX) += s5k5e3yx.o` |

No other file was touched. The driver creates no media device (mtk-seninf owns the
single `media_device`) and registers with `v4l2_async_register_subdev_sensor()`.

Driver model: mainline-`imx219.c` style (CCI regmap, `enable_streams`/`disable_streams`
pad ops + `s_stream` helper, `v4l2_subdev_init_finalize()`, runtime PM
`SET_RUNTIME_PM_OPS`, `devm_v4l2_sensor_clk_get()`), with the downstream register
sequences and power sequencing ported 1:1.

## 2. ROM/downstream evidence

Downstream source (SM-G532M GPL drop):
`$M/src/downstream-grandpplte/drivers/misc/mediatek/imgsensor/src/mt6735/s5k5e3yx_mipi_raw/`
- `s5k5e3yxmipiraw_Sensor.c` (1824 lines), `s5k5e3yxmipiraw_Sensor.h` (129 lines)

| Evidence | Location |
|---|---|
| Sensor ID `0x5e30` | `imgsensor/inc/kd_imgsensor.h:184` (`S5K5E3YX_SENSOR_ID`) |
| Chip ID read: `(reg 0x0000 << 8) | reg 0x0001` | `s5k5e3yxmipiraw_Sensor.c:216-220` |
| Downstream I2C write-address table `{0x20, 0x6c, 0xff}` | `:151` |
| Two addresses tried, 2 read retries each | `:1082-1125` (`get_imgsensor_id()`), `:1128-1190` (`open()`) |
| 24 MHz MCLK, 2 lanes, RAW10, GRBG | `:148-150` (`SENSOR_OUTPUT_FORMAT_RAW_Gr`, `.mclk = 24`, `SENSOR_MIPI_2_LANE`) |
| Exposure limits: `margin = 4`, `min_shutter = 1`, `max_frame_length = 0xffff` | `:128-130` |
| AE shutter seed `0x3D0`, gain seed `0x100` | `:158-159` |
| Mode table pre/cap/video/cap1/hs_video/slim_video | `:61-127` (`imgsensor_info`) |
| Crop windows | `:172-179` (`imgsensor_winsize_info[5]`) |
| Scenario frame delays 3/3/3/3/3 | `:138-142` |
| `write_shutter()` frame-length extension + exposure clamp | `:258-300` |
| `set_dummy()` 0x0340/0x0341 + 0x0342/0x0343 | `:206-214` |
| `set_gain()`: `reg_gain = gain >> 1` → 0x0204/0x0205, AE unity 0x40 | `:464-493` |
| `sensor_init()` table (94 writes) | `:593-700` |
| `preview_setting()` (47 writes) | `:703-757` |
| `capture_setting()`: 15 fps branch (53 writes) / 30 fps branch (47 writes) | `:759-836` / `:837-891` |
| `normal_video_setting()` (47 writes, byte-identical to preview) | `:893-947` |
| `hs_video_setting()` 640x480@120 (47 writes) | `:949-1005` |
| `slim_video_setting()` 1280x960 (47 writes) | `:1007-1062` |
| 40 ms wait after stream-off, 30 fps capture path | `:840` (`mdelay(40)`) |
| `set_test_pattern_mode()`: 0x0601 = 0/1, then 0x3200 = 0x00 | `:1657-1676` |
| Mirror/flip control **disabled** (`#if 0`) | `:535-570` |
| No digital-gain register write anywhere; only `SENSOR_FEATURE_SET_GAIN` (analogue) | grep: no `0x020e`/`0x0210`/`DIGITAL_GAIN` in the file |
| Power on (S5K5E3YX branch) | `imgsensor/src/mt6735/camera_hw/kd_camera_hw.c:325-366` |
| Power off (S5K5E3YX branch) | same file `:642-683` |
| Rail voltages in that branch: VCAMA 2.8 V, VCAMD 1.2 V, VCAMIO 1.8 V | same file `:335-351` |
| Rails shared main/sub, mt6328 LDOs | `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:681-696`, `:640-679` |
| cam1 reset+powerdown pinctrl both drive GPIO12, on=high/off=low | `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_03.dts:542-569`, active states `:643-656` |
| `camera_sub@20` on i2c0 (8-bit write address 0x20 → 7-bit 0x10) | `mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:775-779` |

ROM:
- `$M/zip/symbols.txt:20888`: `S5K5E3YX_MIPI_RAW_SensorInit`
- `$M/zip/vmlinux.strings:36728` `s5k5e3yxmipiraw`, `:36819-36825` driver log
  strings incl. `"s5k5e3yx,MIPI 2LANE"` and
  `"preview 1280*960@30fps,864Mbps/lane; ... capture 5M@30fps,864Mbps/lane"`
  (`s5k5e3yxmipiraw_Sensor.c:40-41`) — stale template text: the programmed
  registers are 2576x1932 and the PLL/`0x0820` pair encode 896 Mbps, not 864.
- `$M/zip/rom_dtb_0.dts:3282-3299` i2c0 `camera_main@34`, `camera_main_af@18`,
  `camera_sub@20`; `:3719-3749` `kd_camera_hw1@15008000` with
  vcama/vcamd/vcamaf/vcamio (+ `_sub` variants) and the 11 camera pinctrl states.

I2C address conversion: downstream hands the *write* address (0x20, 0x6c) to
`iWriteRegI2C()`/`iReadRegI2C()`, which shift it right by one for the i2c core.
The 7-bit addresses therefore are `0x20 >> 1 = 0x10` and `0x6c >> 1 = 0x36`
(`S5K5E3YX_I2C_ADDR_1/2`). DT `reg` supplies the primary address; the driver
retries the other one, two read attempts each, like `get_imgsensor_id()`.

Link frequency derivation (register values from the mode tables):
- 30 fps modes: `0x0305=0x06` (PLLP), `0x0307=0xE0` (PLLM 224), `0x3C1F=0x00`
  (PLLS), `0x0820/0x0821 = 0x0380 = 896` (requested Mbps/lane).
  PLL: 24 MHz / 6 * 224 / 2^0 = 896 Mbps/lane; pclk-derived:
  179200000 * 10 / 2 = 896 Mbps/lane — both agree.
  V4L2 link frequency = bit rate / 2 = **448 MHz**.
- 15 fps capture: `0x0307=0xDA` (218), `0x3C1F=0x01` (PLLS=1),
  `0x0820/0x0821 = 0x01b4 = 436`. PLL: 24 / 6 * 218 / 2 = 436 Mbps/lane.
  V4L2 link frequency = **218 MHz**. (pclk-derived would be
  89616000 * 10 / 2 = 448.08 Mbps — 2.7 % higher than the PLL; the PLL/`0x0820`
  value is used, see §5.)
- hs_video and slim_video reuse the 896 Mbps settings (same PLL/`0x0820` writes)
  → 448 MHz.

## 3. Parity matrix

| Behavior | ROM/downstream implementation | Nura before | after | status |
|---|---|---|---|---|
| Chip ID probe | read 0x0000/0x0001, expect 0x5e30, two addresses, 2 retries each (`:216-220`, `:1082-1125`, `:1128-1190`) | none | `s5k5e3yx_identify_module()`: DT addr then alternate (0x10↔0x36), 2 attempts each | done |
| I2C transport | 3-byte/2-byte msgs, 16-bit register addresses, 8-bit data | none | CCI regmap, `reg_addr_bits = 16` (`devm_cci_regmap_init_i2c`) | done |
| Power on order/delays | CMPDN+CMRST low → VCAMA 2.8 V → VCAMD 1.2 V → VCAMIO 1.8 V → 1 ms → CMPDN+CMRST high → 1 ms → ISP_MCLK2_EN(1) (`kd_camera_hw.c:329-365`) | none | `s5k5e3yx_power_on()`: same order, `usleep_range(1000,1100)` for each `mdelay(1)`, per-rail `regulator_set_voltage()` then `regulator_enable()` | done |
| Power off order/delays | MCLK off → 1 ms → reset/pdn low → 1 ms → VCAMIO off → 1 ms → VCAMD off → 1 ms → VCAMA off (`:646-682`) | none | `s5k5e3yx_power_off()`: identical order and delays | done |
| Reset/powerdown polarity | both signals on GPIO12; "off"=low before rails, "on"=high after rails (`mt6737t_grandpplte_ltn_dtv_03.dts:542-569`) | none | single `reset-gpios` (`GPIOD_OUT_LOW`, logical 1 = high = operational); powerdown folded into it | done |
| MCLK 24 MHz | `ISP_MCLK2_EN(1/0)` last on / first off | none | `clk_prepare_enable/clk_disable_unprepare(xclk)` at the same points, CCF clock `<&seninf 1>`; rate warning if not 24 MHz | done |
| Regulators | VCAMA/VCAMD/VCAMIO via `_hwPowerOn`, shared with main sensor | none | `vcama-supply`/`vcamd-supply`/`vcamio-supply`, bulk get, per-rail voltage+enable | done |
| Init sequence | 94 writes incl. stream on (`:593-700`) | none | 92 writes (`s5k5e3yx_init_regs`, `0x0100` writes stripped), written on every stream start | done |
| 2576x1932@30 (pre/cap/video) | 47 writes, pclk 179.2 MHz, ll 2950, fll 2025 (`:61-105`, tables `:703-757`/`:837-891`/`:893-947`) | none | mode 0, 45 writes, pclk 179200000, 2950/2025, VBLANK 93, HBLANK 374 | done |
| 2560x1920@15 capture (cap1) | 53 writes, pclk 89.616 MHz, ll 2950, fll 2025 (`:83-94`, `:759-836`) | none | mode 1, 51 writes, pclk 89616000, 2950/2025, VBLANK 105, HBLANK 390, link 218 MHz | done |
| 640x480@120 hs_video | 47 writes, pclk 179.2 MHz, ll 2950, fll 506 (`:106-116`, `:949-1005`) | none | mode 2, 45 writes, 2950/506, VBLANK 26, HBLANK 2310, crop 8/8/2560/1920 | done |
| 1280x960@15 slim_video | 47 writes, pclk 179.2 MHz, ll 2950, fll 4050 (`:117-127`, `:1007-1062`) | none | mode 3, 45 writes, 2950/4050, VBLANK 3090, HBLANK 1670 | done |
| Exposure (shutter) | `write_shutter()`: coarse int time 0x0202/0x0203; frame_length = max(shutter+4, min_fll), clamp 0xffff; shutter clamp [1, 0xffff-4] (`:258-300`) | none | `V4L2_CID_EXPOSURE`, min 1, step 1, max = mode_fll+vblank-4, default = mode table value; VTS clamped ≤ 0xffff via VBLANK max | done |
| Frame length / VBLANK | 0x0340/0x0341 (`set_dummy()` `:206-214`), `min_frame_length`=mode fll, `max_frame_length`=0xffff | none | `V4L2_CID_VBLANK`: min = fll-height, max = 0xffff-height, writes VTS and re-writes clamped exposure | done |
| Line length / HBLANK | 0x0342/0x0343 written only with the constant 2950; never changed at runtime | none | `V4L2_CID_HBLANK` read-only, value = 2950 - width per mode | done |
| Analogue gain | `set_gain()`: register = gain >> 1, AE unity 0x40 (`:464-493`); no gain table in this driver | none | `V4L2_CID_ANALOGUE_GAIN`, min 0x40 (1x), max 0xffff, default 0x40 → 0x0204/0x0205 = 0x0020 | done |
| Digital gain | not implemented downstream (no register write, no feature id) | none | not registered (no invented register) | deferred |
| Pixel rate | `SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ` returns per-mode `imgsensor.pclk` (`:1696-1700`) | none | `V4L2_CID_PIXEL_RATE`, per-mode 179200000 / 89616000 | done |
| Link frequency | not exposed; implied by PLL (`0x0305/0x0307/0x3C1F/0x0820/0x0821`) | none | `V4L2_CID_LINK_FREQ` menu {448 MHz, 218 MHz}, mode-selected; DT validated with `v4l2_link_freq_to_bitmap()` | done |
| Test pattern | `set_test_pattern_mode()`: 0x0601 0/1, 0x3200 = 0x00 (`:1657-1676`) | none | `V4L2_CID_TEST_PATTERN` menu {Disabled, Color bar} → same two writes | done |
| HFLIP/VFLIP | `set_mirror_flip()` inside `#if 0` (`:535-570`), never compiled | none | not registered | deferred |
| Stream on/off | 0x0100 = 1/0 at end/start of every table | none | `enable_streams`: 0x0100=0, 40 ms, init table, mode table, 0x0100=1; `disable_streams`: 0x0100=0 | done |
| Mode-change delay | 40 ms between stream-off and register writes (30 fps capture, `:840`); other scenarios: no delay | none | `msleep(40)` once, applied to all modes (see §5) | done |
| Scenario frame delays (3 frames) | `cap/pre/video/hs/slim_delay_frame = 3` (`:138-142`) — AE/ISP-side, not a sensor wait | none | no driver action; documented | deferred (not a sensor behavior) |
| MIPI LP→HS settle 85 | `mipi_data_lp2hs_settle_dc = 85` per mode, consumed by SENINF, not by the sensor | none | not a sensor control; must be programmed by mtk-seninf (integration note) | deferred (owner: mtk-seninf) |
| OTP / lens shading (pages 0..3) | `get_otps_data()`/`set_otps_data()` via MTK NVM interface, exposed only through `SENSOR_FEATURE_*_SENSOR_ATT_PERC_OFT` for the vendor HAL (`:311-445`, `:1770-1778`) | none | not registered (no V4L2 equivalent; MTK HAL ABI not present) | deferred |
| Crop / selection | fixed windows from `imgsensor_winsize_info` (`:172-179`) | none | `get_selection`/`set_selection` report the mode's fixed crop + native size; no arbitrary crop accepted | done |
| Format enumeration | RAW10 GRBG, no flips/format list | none | single `MEDIA_BUS_FMT_SGRBG10_1X10`; `enum_frame_size` over the 4 modes | done |
| Subdev registration | n/a (MTK kd_sensorlist) | none | `v4l2_async_register_subdev_sensor()`, `MEDIA_ENT_F_CAM_SENSOR`, `V4L2_SUBDEV_FL_HAS_DEVNODE` | done |
| Media device ownership | n/a | none | none created (mtk-seninf owns the only media_device) | done |

## 4. Compile-tested

Commands (exact):

```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-s5k5e3yx"
cp "$M/nura.config" .config
./scripts/config -e MEDIA_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API \
  -e V4L2_FWNODE -e V4L2_ASYNC -e I2C -e REGULATOR -e COMMON_CLK -e VIDEO_CAMERA_SENSOR \
  -e MEDIA_CAMERA_SUPPORT -e VIDEO_S5K5E3YX -e V4L2_CCI_I2C
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/media/i2c/s5k5e3yx.o 2>&1 | tee "$M/logs/camera-s5k5e3yx-build.log"
```

Result: `CC drivers/media/i2c/s5k5e3yx.o`, exit status 0, **no warnings** from the new
file (an earlier unused-variable warning was fixed). `CONFIG_MEDIA_CAMERA_SUPPORT`
had to be enabled in addition to the options listed in the task, otherwise
`VIDEO_CAMERA_SENSOR`/`VIDEO_DEV` are forced off by `MEDIA_SUPPORT_FILTER`.
Only the sensor object was built (no full kernel/modules build).

The five generated register tables were verified byte-for-byte against the
downstream source by re-parsing both files: 92/45/51/45/45 entries, all identical.

## 5. Inferred / hardware-unverified items

1. **Link frequency of the 2560x1920@15 mode** — PLL registers and `0x0820/0x0821`
   encode 436 Mbps/lane (218 MHz), while `pclk` 89616000 * 10 / 2 implies
   448.08 Mbps. The PLL value is used, as instructed ("consistent with the PLL
   registers"); the 2.7 % discrepancy between the downstream `imgsensor_info`
   `pclk` and the PLL is noted, not resolved. `0x3C1C` (D-PHY divider) also
   differs in that table (0x54 vs 0x58), so the 15 fps mode genuinely runs a
   different PHY rate. [INFERENCE: bit rate = MCLK/PLLP*PLLM/2^PLLS, confirmed by
   two independent exact matches to the `0x0820/0x0821` request bytes.]
2. **Analogue-gain upper bound** — downstream does not clamp gain; the control
   max (0xffff) is the u16 domain of its AE interface, not a datasheet limit.
   Unity is 0x40 (AE domain) → reg 0x20. [INFERENCE on the physical gain range.]
3. **Exposure default** — the register tables program coarse integration
   0x0200 (512) / 0x01f4 (500, hs mode); the downstream struct's `0x3D0` is an
   AE seed that is always overwritten by AE. The control default is the table
   value so that streaming without user controls matches the programmed state.
4. **xclk rate check** — a mismatch is only warned about, never fatal: the
   mtk-seninf clock provider may not implement `recalc_rate`, in which case
   `clk_get_rate()` returns 0.
5. **`msleep(40)` placement** — downstream has this wait only in the 30 fps
   capture path; it is applied before every mode-table write here (strictest
   path, harmless extra latency).
6. **Single reset GPIO** — downstream's `cam1_rst*` and `cam1_pnd*` pinctrl states
   both drive GPIO12, so reset and powerdown are modelled as one active-high
   GPIO. If a board wires them to different pins, the driver must gain an
   optional `powerdown-gpios` property (not present because there is no
   evidence for a second pin).
7. **Rail voltages** — set by `regulator_set_voltage()` in the driver, matching
   `_hwPowerOn(rail, voltage)`; the Nura mt6328 regulator driver supports all
   three values (vcama table1 2.8 V, vcamd table11 1.2 V, vcamio table13 1.8 V).
   If DT constraints forbid a value, probe fails loudly instead of silently
   running at the wrong rail voltage.
8. **No hardware validation** — nothing was run on a device; register values,
   delays, GPIO polarity and the power sequencing are taken from the downstream
   source and ROM DT only.
9. **Integration notes (other owners)**:
   - the 85 (`mipi_data_lp2hs_settle_dc`) settle value and the per-scenario
     3-frame delays must be applied by mtk-seninf / userspace, not by the sensor;
   - `drivers/regulator/mt6328-regulator.c` currently has the `ldo_vcamio`
     `MT6328_LDO_TABLE` entry **twice** (lines 395-397 and 398-400); harmless
     (first match wins) but worth cleaning by its owner;
   - the sensor needs `<&seninf 1>` as xclk, i.e. the per-camera MCLK channel 1
     of the SENINF clock provider (sub camera = `ISP_MCLK2_EN` in downstream).

## DTS-FRAGMENT

Node for the front camera on i2c0. 7-bit address 0x10; the alternative module
address 0x6c>>1 = 0x36 is handled by the driver (no second node needed). The
`remote-endpoint` label must match the endpoint created by the mtk-seninf
fragment (placeholder `seninf_cam1_in` — rename on merge). No dts/dtsi file in
the worktree was modified.

```dts
&i2c0 {
	status = "okay";
	#address-cells = <1>;
	#size-cells = <0>;

	s5k5e3yx: camera-sensor@10 {
		compatible = "samsung,s5k5e3yx";
		reg = <0x10>;

		/* MTK SENINF per-camera MCLK (downstream ISP_MCLK2_EN) */
		clock-names = "xclk";
		clocks = <&seninf 1>;

		/* kd_camera_hw.c S5K5E3YX branch: VCAM_A 2.8 V, VCAM_D 1.2 V, VCAM_IO 1.8 V */
		vcama-supply = <&mt6328_vcama_reg>;
		vcamd-supply = <&mt6328_vcamd_reg>;
		vcamio-supply = <&mt6328_vcam_io_reg>;

		/*
		 * downstream cam1 reset and powerdown pinctrl states both drive
		 * GPIO12 (output-low = off, output-high = on)
		 */
		reset-gpios = <&pio 12 GPIO_ACTIVE_HIGH>;

		status = "okay";

		port {
			s5k5e3yx_out: endpoint {
				remote-endpoint = <&seninf_cam1_in>; /* label from mtk-seninf */
				clock-lanes = <0>;
				data-lanes = <1 2>;
				/* 896 Mbps/lane (30 fps modes), 436 Mbps/lane (15 fps capture) */
				link-frequencies = /bits/ 64 <448000000 218000000>;
			};
		};
	};
};
```

## 7. Patch path

`$M/patches/camera-s5k5e3yx.patch` (Kconfig + Makefile + new driver, no `.config`,
no build artifacts).
