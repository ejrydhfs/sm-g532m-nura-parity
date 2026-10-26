# MT6735 SENINF driver (camera-seninf)

Port of the Samsung/MTK downstream camera hardware control (`SENINF` block,
`kd_camera_hw`/`kdCISModulePowerOn`, `kdSetSensorMclk`) to a self-contained
V4L2/media-controller driver for the Nura 7.0.10 arm64 tree.

## 1. Driver and files changed

New:

| File | Contents |
|---|---|
| `drivers/media/platform/mediatek/seninf/Kconfig` | `VIDEO_MTK_SENINF` tristate |
| `drivers/media/platform/mediatek/seninf/Makefile` | `mtk-seninf.o` |
| `drivers/media/platform/mediatek/seninf/mtk-seninf.c` | platform driver, media device, V4L2 subdev, async notifier, MCLK clock provider, runtime PM, error status, debugfs |
| `drivers/media/platform/mediatek/seninf/mtk-seninf-regs.h` | register offsets/bits, each citing downstream |

Modified (one line each, merged by the coordinator):

* `drivers/media/platform/mediatek/Kconfig`: `source ".../seninf/Kconfig"`
* `drivers/media/platform/mediatek/Makefile`: `obj-y += seninf/`

Driver behaviour summary:

* compatible `mediatek,mt6735-seninf`; three register windows mapped
  (`SENINF` 0x15008000/0x4000, `MIPIRX_CONFIG` 0x1500c000/0x1000,
  `MIPIRX_ANALOG` 0x10215000/0x3000).
* one `v4l2_device` + **the single subsystem `media_device`** + one
  `v4l2_subdev` named `mtk-seninf` (`MEDIA_ENT_F_VID_IF_BRIDGE`) with one sink
  pad per DT port plus one source pad.
* `v4l2_async_nf_*` notifier with one fwnode-remote connection per endpoint;
  `bound` creates sensor-source → SENINF-sink immutable links; `complete`
  registers the subdev device nodes.
* `#clock-cells = <1>` provider with `mclk0..mclk2`; each is a child of the
  `camtg` clock, so CCF prepares/enables `camtg` (with correct refcounting)
  around every MCLK consumer and `prepare` sets bit 29 of the per-camera MCLK
  register.
* `prepare`/`unprepare` take/drop a runtime PM reference (this also covers a
  sensor's probe-time `power_on`, which enables `xclk` before any stream starts)
  so the `MT6735_POWER_DOMAIN_ISP` domain is on when the bit is written and read.
  On resume the MCLK bits are re-applied after the domain was switched off and
  the SENINF error status is checked/cleared. `camtg` itself is never touched by
  the runtime PM callbacks: it is switched by the CCF parent chain.
* `video_ops.s_stream` takes a runtime PM reference, validates the remote
  sensor link frequency and data-lane count, reports any SENINF error status and
  logs the negotiated parameters; no IRQ is requested.
* debugfs `mtk-seninf/regs` dumps the three mapped windows read-only.

## 2. ROM/downstream evidence

### Register windows and offsets

| Item | Evidence |
|---|---|
| `SENINF_BASE_ADDR 0x15008000`, `SENINF_REG_RANGE 0x4000` | `cameraisp/src/mt6735/camera_isp_D1.c:124-126` |
| `MIPIRX_CONFIG_ADDR 0x1500C000`, range `0x1000` | `camera_isp_D1.c:128-129` |
| `MIPIRX_ANALOG_ADDR 0x10215000`, range `0x3000` | `camera_isp_D1.c:130-131` |
| Downstream SENINF node (`reg = <0x15008000 0x1000>`, `TOP_CAMTG_SEL` + `TOP_UNIVPLL_D26` + `TOP_UNIVPLL2_D2`) | `arch/arm/boot/dts/mt6735.dtsi:1307-1320` |
| ROM equivalent `kd_camera_hw1@15008000` (same reg/clocks; note the ROM DT window is 0x1000, the ISP driver maps 0x4000) | `zip/rom_dtb_0.dts:3719-3721` |
| ROM string proving the SENINF mapping in the ROM kernel | `zip/vmlinux.strings:36350` `"[ISP]mmap range error : length(0x%lx),SENINF_REG_RANGE(0x%x)!"` |

### Per-camera status registers

| Item | Evidence |
|---|---|
| `SENINF1_INT = ISP_ADDR+0x4128`, `SENINF2_INT +0x4528`, `SENINF3_INT +0x4928`, `SENINF4_INT +0x4D28`; with `ISP_ADDR = 0x15004000` these are SENINF+0x128/0x528/0x928/0xD28 | `camera_isp_D1.c:257-260`, `camera_isp_D1.c:124` |
| Bits 0..6 overrun/CRC/FSM/VSIZE/HSIZE/SENSOR_VSIZE/SENSOR_HSIZE (SENINF1..4) | `cameraisp/src/mt6735/inc/camera_isp_D1.h:267-298` |
| Error mask composition `ISP_REG_MASK_INT_SENINF1..4_ERR` | `camera_isp_D1.c:618-662` |
| Status is only *read*, in a disabled (`#if 0`) block, from the ISP IRQ handler | `camera_isp_D1.c:9199-9218` |
| ISP owns the SENINF interrupt lines (CAM0/CAM1/CAM2 GIC 183/184/185) | `arch/arm/boot/dts/mt6735.dtsi:1273-1277` |

### MCLK

| Item | Evidence |
|---|---|
| `ISP_MCLK1_EN` sets bit 29 (0x20000000) of `ISP_ADDR+0x4200` = SENINF+0x200, with an internal `mMclk1User` refcount | `camera_isp_D1.c:12048-12076` |
| `ISP_MCLK2_EN` → `ISP_ADDR+0x4600` = SENINF+0x600 | `camera_isp_D1.c:12081-12107` |
| `ISP_MCLK3_EN` → `ISP_ADDR+0x4A00` = SENINF+0xA00 | `camera_isp_D1.c:12111-12137` |
| no `ISP_MCLK4_EN`/`MCLK4` anywhere in the GPL drop (grep over `cameraisp/`) | - |
| `camtg` gate/parent is the clock that protects MCLK: `clk_prepare_enable(g_camclk_camtg_sel)`, parent `univpll_d26` (48 MHz group) or `univpll2_d2` (52 MHz group) | `imgsensor/src/mt6735/kd_sensorlist.c:2420-2433`, `:2442-2461` |
| MCLK raised before/after reset per sensor, e.g. IMX219: rails → 3 ms → MCLK → 1 ms → powerdown/reset release; S5K5E3YX: rails → 1 ms → reset/powerdown release → 1 ms → MCLK | `camera_hw/kd_camera_hw.c:519-530`, `:336-365` |
| MCLK dropped first in power-off | `camera_hw/kd_camera_hw.c:766-790` |
| `ISP_MCLK1_EN`/`2`/`3` exported in the ROM kernel | `zip/symbols.txt:20750-20752` |

### Section 3 helper evidence (power sequencing replaced by this port)

| Item | Evidence |
|---|---|
| `CAMERA_HW_probe` gets the CCF clocks (`TOP_CAMTG_SEL`, `TOP_UNIVPLL_D26`, `TOP_UNIVPLL2_D2`) and inits the camera pinctrl | `kd_sensorlist.c:3700-3712`, `:2420-2433` |
| `mtkcam_gpio_set()` selects `camX_rst_l/h`, `camX_pnd_l/h` pinctrl states | `camera_hw/kd_camera_hw.c:153-198` |
| `kdCISModulePowerOn()` rail order and voltages (VCAMIO 1.8 V, VCAMA 2.8 V, VCAMD 1.2 V, VCAMAF 2.8 V) and delays | `camera_hw/kd_camera_hw.c:203-230`, `:336-365`, `:486-530` |
| Board pinctrl: cam0 reset **and** powerdown are GPIO44, cam1 reset **and** powerdown are GPIO12 | `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_03.dts:514-570` |
| Rails wired from PMIC | `mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:681-693`, `mt6735.dtsi:1310-1313`, `rom_dtb_0.dts:3722-3725` |
| Sensors on i2c0: `camera_main@34` (IMX219, `i2c_addr_table = {0x34}`), `camera_main_af@18`, `camera_sub@20` (S5K5E3YX, `{0x20, 0x6c}`) | `mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:762-779`, `imx219mipiraw_Sensor.c:161`, `s5k5e3yxmipiraw_Sensor.c:151` |
| MIPI lanes: IMX219 4-lane, S5K5E3YX 2-lane | `imx219mipiraw_Sensor.c:160`, `s5k5e3yxmipiraw_Sensor.c:150` |
| Sensor MCLK requirement: 24 MHz for both sensors | `imx219mipiraw_Sensor.c:159` (`.mclk = 24`), `s5k5e3yxmipiraw_Sensor.c:149` (`.mclk = 24`), exported as `SensorClockFreq` at `:1465` / `:1422` and handed to `kdSetSensorMclk()` at `kd_sensorlist.c:3248` |
| `CAMERA_HW_probe` symbol in the ROM kernel | `zip/symbols.txt:20808` |

### Nura-side facts used

| Item | Evidence |
|---|---|
| `CLK_TOP_CAMTG_SEL = 48` | `include/dt-bindings/clock/mediatek,mt6735-topckgen.h:56` |
| `camtg_sel` mux+gate, parents incl. `univpll_d26`, `univpll2_d2` | `drivers/clk/mediatek/clk-mt6735-topckgen.c:177-183`, `:342`, `:88`, `:92` |
| `MT6735_POWER_DOMAIN_ISP = 4` | `include/dt-bindings/power/mt6735-power.h:10` |
| imgsys syscon / `CLK_IMG_SEN_TG`, `CLK_IMG_SEN_CAM` gate the SENINF/ISP logic | `arch/arm64/boot/dts/mediatek/mt6735.dtsi:846-851`, `drivers/clk/mediatek/clk-mt6735-imgsys.c:28-29` |

## 3. Parity matrix

| Behavior | ROM/downstream implementation | Nura before | after | status |
|---|---|---|---|---|
| SENINF register window | `SENINF_BASE_ADDR 0x15008000` + `SENINF_REG_RANGE 0x4000`, `camera_isp_D1.c:124-126`; DT node `mt6735.dtsi:1307-1309`; ROM `rom_dtb_0.dts:3719-3721` | none | reg[0] ioremapped, used for MCLK + status | done |
| MIPIRX_CONFIG window | `MIPIRX_CONFIG_ADDR 0x1500C000/0x1000`, `camera_isp_D1.c:128-129` | none | reg[1] mapped, read-only debugfs dump | done |
| MIPIRX_ANALOG window | `MIPIRX_ANALOG_ADDR 0x10215000/0x3000`, `camera_isp_D1.c:130-131` | none | reg[2] mapped, read-only debugfs dump | done |
| MIPI D-PHY / lane / settle-time programming | only in proprietary `seninf_drv` (referenced as the source of the addresses, `camera_isp_D1.c:125-133`); **0** SENINF symbols in `zip/symbols.txt` (case-insensitive grep), no register table in the GPL drop | none | not implemented (no addresses/values exist to port) | deferred |
| Per-camera MCLK enable (bit 29) | `ISP_MCLK1/2/3_EN`, `camera_isp_D1.c:12048-12137` (`+0x4200/+0x4600/+0x4A00`, bit 0x20000000) | none | `mclk0..mclk2` clocks, `prepare/unprepare` set/clear bit 29 at SENINF+0x200/0x600/0xA00 | done |
| MCLK user refcounting | `mMclk1User..3User` counters inside `ISP_MCLKn_EN` | none | CCF `prepare_count` (no manual counters) | done |
| `camtg` enable around MCLK | `clk_prepare_enable(g_camclk_camtg_sel)` … `clk_disable_unprepare()`, `kd_sensorlist.c:2453-2459` | none | `camtg` is the CCF parent of every MCLK output, so it is prepared/enabled by the clock core while any MCLK is held; runtime PM only switches the ISP domain, so the bit write is safe even during a sensor's probe-time power_on | done (see note A) |
| MCLK frequency selection | `kdSetSensorMclk()` sets the `camtg_sel` mux parent: `univpll_d26` (48 MHz group) / `univpll2_d2` (52 MHz group), `kd_sensorlist.c:2454-2457`; both sensors declare `.mclk = 24` (`imx219mipiraw_Sensor.c:159`, `s5k5e3yxmipiraw_Sensor.c:149`) | none | `set_rate`/`set_parent` not implemented on the MCLK outputs; the shared `camtg` mux is configured from DT (`assigned-clock-parents`, see section 6), MCLK rate = `clk_get_rate(camtg)` | deferred (DT-driven) |
| SENINF error status | regs `SENINF1..4_INT`, bits 0..6 (`camera_isp_D1.h:267-298`), read in ISP ISR `camera_isp_D1.c:9199-9218` | none | `mtk_seninf_check_errors()` decodes + logs + clears, called from `s_stream` (on/off) and runtime resume | done (see note B) |
| SENINF interrupt | none of its own; status polled through ISP CAM0..2 IRQs (`mt6735.dtsi:1273-1277`) | none | no IRQ requested, no interrupt-enable writes | done (by design) |
| Media device | none (downstream exposes `/dev/camera_hw` ioctls instead) | none | one `media_device` (`MT6735 SENINF`), owned by seninf | done |
| Sensor discovery / binding | `CAMERA_HW_probe` registers `kd_sensorlist` i2c drivers which probe every supported sensor at boot (`kd_sensorlist.c:3700-3712`) | none | fwnode endpoints + `v4l2_async_nf_*`, one connection per port, links created on `bound` | done |
| Stream start/stop | `CAMERA_HW` ioctl `SENSOR_INIT`/power-on through `kdCISModulePowerOn` | none | `video_ops.s_stream`: runtime PM, link freq/lane validation, error status check, parameter log | done (see note C) |
| Per-camera rails | `_hwPowerOn(VCAMA/VCAMD/VCAMIO/VCAMAF)` with 1.2/1.8/2.8 V and per-sensor ordering, `kd_camera_hw.c:336-365`, `:486-530` | none | moved to the sensor drivers' regulator bindings | done (sensor drivers) |
| Reset / powerdown pins | `mtkcam_gpio_set()` + pinctrl states `cam0_rst0/1`, `cam0_pnd0/1`, `cam1_rst0/1`, `cam1_pnd0/1` | none | sensor drivers' `reset-gpios`/`powerdown-gpios` | done (sensor drivers) |
| `/dev/camera_hw` legacy ABI | `CAMERA_HW_i2c_driver` + ioctl set in `kd_sensorlist.c` | none | not reimplemented (replaced by standard bindings) | deferred (by design) |
| ISP pipeline (SENINF → ISP → frames) | `cameraisp` + proprietary `libcamdrv`/ISP firmware, `mt_imgsys_init` (`zip/symbols.txt:54699`) | none | out of scope; SENINF source pad left unconnected | deferred |

### Mapping: `kd_camera_hw.c` / `kdCISModulePowerOn` → standard bindings + SENINF MCLK provider

| Downstream | Evidence | Nura equivalent |
|---|---|---|
| rail VCAM_A 2.8 V | `kd_camera_hw.c:336`, `:495`; `mt6735.dtsi:1310` | sensor `vcama-supply` (both sensors; first rail of the imx219 sequence) |
| rail VCAM_D 1.2 V | `kd_camera_hw.c:342`, `:500`; `mt6735.dtsi:1311` | sensor `vcamd-supply` (both sensors) |
| rail VCAM_IO 1.8 V | `kd_camera_hw.c:348`, `:514`; `mt6735.dtsi:1313` | sensor `vcamio-supply` (both sensors; `i2c0` `vbus-supply` in the board DTS is the same PMIC LDO) |
| rail VCAM_AF 2.8 V | `kd_camera_hw.c:288`, `:506`; `mt6735.dtsi:1312` | sensor `vcamaf-supply` (imx219 only, last rail of its sequence; the S5K5E3YX sub path has no VCAM_AF) |
| `CAMRST` pin 0 = GPIO44, pin 1 = GPIO12 | `kd_camera_hw.c:153-172`, board DTS `:514-541` | `reset-gpios = <&pio 44 GPIO_ACTIVE_HIGH>` (cam0) / `<&pio 12 GPIO_ACTIVE_HIGH>` (cam1) |
| `CAMPDN` pin 0 = GPIO44, pin 1 = GPIO12 (same pins as reset on this board) | board DTS `:528-541`, `:556-569` | optional `powerdown-gpios` = the same two GPIOs, active-high |
| polarity: `cam0_rst1`/`cam0_pnd1` = `output-high` is the ON (out-of-reset / enabled) level | board DTS `:521-527`, `:535-541` | `GPIO_ACTIVE_HIGH` in the sensor node; driver value 1 = out of reset / enabled |
| `ISP_MCLK1_EN` for pinSetIdx 0, `ISP_MCLK2_EN` for pinSetIdx 1 | `kd_camera_hw.c:277-278`, `:522-523` | `clocks = <&seninf 0>` / `<&seninf 1>`, `clock-names = "xclk"` |
| `camtg` prepared before MCLK, parent = `univpll_d26` (48 MHz group) | `kd_sensorlist.c:2453-2457` | MCLK clock's CCF parent is `camtg`; `assigned-clock-parents = <&topckgen CLK_TOP_UNIVPLL_D26>` on the seninf node |
| power-on delays (1 ms/2 ms/3 ms) and rail order | `kd_camera_hw.c:336-365`, `:519-530` | sensor driver `power_on()` ordering (peer-owned); the SENINF MCLK clock is enabled at the driver's chosen point |
| power-off order: AF → MCLK → GPIO low → IO → D → A | `kd_camera_hw.c:766-805` | sensor driver `power_off()` ordering (peer-owned) |
| `/dev/camera_hw` + `MISC` ioctls for power/MCLK/sensor list | `kd_sensorlist.c:3700-3712`, `CAMERA_HW_probe` (`symbols.txt:20808`) | not ported: no legacy misc ABI; userspace uses V4L2 subdev/controls |

**Note A (camtg refcount, runtime PM and deadlock avoidance).** The downstream
enable sequence is "`clk_prepare_enable(camtg)` then raise the MCLK bit".
Calling `clk_prepare_enable(camtg)` from inside our clock's `prepare` callback
would take the CCF global `prepare_lock` recursively and deadlock. The port
therefore models `camtg` as the MCLK clock's CCF parent: `clk_prepare_enable()`
on an MCLK output prepares/enables `camtg` through the clock core with exact
refcounting, and our `prepare` only sets bit 29.

The SENINF register block is in the ISP power domain, which is a pure SPM power
domain: its DT node has no `clocks` property
(`arch/arm64/boot/dts/mediatek/mt6735.dtsi:223-227`, unlike MD1/CONN/DIS/MFG
which reference `&infracfg`), so genpd's power on/off is SPM register access
only. `mtk_seninf_mclk_prepare()` therefore takes a runtime PM reference before
writing the MCLK bit and `mtk_seninf_mclk_unprepare()` drops it again; this is
what makes the enable work during a sensor driver's probe-time `power_on()`,
when the SENINF device is still runtime suspended. No runtime PM call in that
path re-enters the CCF `prepare_lock` (the resume callback no longer touches any
clock), so the sequence cannot deadlock. `pm_runtime_put()` (not
`..._put_autosuspend()`) is used because the driver does not enable autosuspend.

`camtg` is deliberately *not* enabled by the runtime PM callbacks: it is needed
only while an MCLK output is enabled, which the CCF parent chain already covers.

**Note B (clear convention).** The downstream GPL source never writes the
`SENINFn_IRQ_INTSTA` registers (the only access is the disabled read at
`camera_isp_D1.c:9199-9218`). Clearing by writing back the set bits is
`[INFERENCE]` from the usual MTK IRQ status convention; the helper verifies the
clear and warns if the bits persist. See section 5.

**Note C (no D-PHY programming).** `s_stream` validates and logs the link, but
the MIPI D-PHY/SENINF lane programming cannot be ported: the register table is
only in the proprietary `seninf_drv` (the downstream kernel comments point at it
for every address, `camera_isp_D1.c:125-133`) and the ROM kernel contains no
SENINF symbols at all (`zip/symbols.txt`, 0 matches for `seninf`). Streaming is
not exercised by anything in this tree because there is no capture video node:
the ISP pipeline needs the MTK userspace ABI and firmware.

## 4. Compile-tested

```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-seninf"
cp "$M/nura.config" .config
./scripts/config -e MEDIA_SUPPORT -e MEDIA_CAMERA_SUPPORT -e MEDIA_PLATFORM_SUPPORT \
  -e MEDIA_CONTROLLER -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC \
  -e I2C -e REGULATOR -e COMMON_CLK -e VIDEO_CAMERA_SENSOR -e VIDEO_MTK_SENINF
make ARCH=arm64 LLVM=1 olddefconfig
rm -f drivers/media/platform/mediatek/seninf/mtk-seninf.o
make ARCH=arm64 LLVM=1 -j2 drivers/media/platform/mediatek/seninf/mtk-seninf.o \
  2>&1 | tee "$M/logs/camera-seninf-build.log"
# then, extra warnings:
touch drivers/media/platform/mediatek/seninf/mtk-seninf.c
make ARCH=arm64 LLVM=1 W=1 -j2 drivers/media/platform/mediatek/seninf/mtk-seninf.o \
  2>&1 | tee -a "$M/logs/camera-seninf-build.log"
```

Result: `CC drivers/media/platform/mediatek/seninf/mtk-seninf.o` twice, **no
warnings and no errors**, including `W=1`. `CONFIG_VIDEO_MTK_SENINF=y` after
`olddefconfig`. Note: `nura.config` has `# CONFIG_MEDIA_SUPPORT is not set` and
the resulting `MEDIA_SUPPORT_FILTER=y` hides the media menu, so
`MEDIA_CAMERA_SUPPORT`/`MEDIA_PLATFORM_SUPPORT` must be enabled in addition to
the options listed in the task. Log:
`$M/logs/camera-seninf-build.log` (56 lines, no `error`/`warning` lines).

## 5. Inferred / hardware-unverified items

1. **`SENINFn_IRQ_INTSTA` write-one-to-clear** (`mtk_seninf_check_errors`).
   Downstream only reads the register (inside `#if 0`). The clear write is
   `[INFERENCE]` from the MTK IRQ-status convention; the code reads the register
   back and warns if the bits did not clear. If the register turns out to be
   read-only, the failure mode is one warning per check, no data corruption.
2. **MCLK enable at probe time (fixed in review).** `prepare` takes a runtime PM
   reference before writing the MCLK bit, so a sensor that enables `xclk` in its
   probe-time `power_on()` (to read the chip ID) gets a driven MCLK even though
   SENINF was never streamed. This relies on the ISP power domain having no
   clocks (`mt6735.dtsi:223-227`), otherwise genpd's power-on would re-enter the
   CCF `prepare_lock` held by the caller.
3. **MCLK frequency is `camtg`'s rate and depends on the parent chosen in DT.**
   Both sensors require 24 MHz (`.mclk = 24`). In this tree `univpll_d26` is
   `univpll / 26` (`clk-mt6735-topckgen.c:88`), so the 24 MHz result depends on
   the PLL rate programmed by the bootloader; it has not been measured on
   hardware. If it does not come out at 24 MHz, the fix belongs in the camtg
   parent selection in DT (or a `set_rate` on the MCLK outputs), not in the
   SENINF driver.
4. **`data-lanes` maximum = 4.** Taken from the IMX219 4-lane configuration
   (`imx219mipiraw_Sensor.c:160`). The exact SENINF/MIPI-RX lane limit is not
   stated in the GPL source.
5. **MIPI D-PHY/SENINF programming, ISP pipeline, `/dev/camera_hw` ABI**: not
   ported (no source of register values; proprietary ABI/firmware) — see
   section 3.
6. **Register windows never written** except the documented bit 29
   (`ISP_MCLKn_EN`) and the status-clear write. `MIPIRX_CONFIG` and
   `MIPIRX_ANALOG` are mapped read-only (debugfs dump) exactly because no
   documented programming exists.
7. **No hardware has been run.** All behaviour above is compile-tested only.
8. `s_stream` uses `video_ops.s_stream`. This hook is marked deprecated in 7.0
   in favour of `pad_ops.enable_streams/disable_streams`; the pad variant
   requires subdev state and routing, which a bridge without a capture pipeline
   does not have. `v4l2_subdev_s_stream_helper` was not used for the same reason.

## 6. DTS-FRAGMENT

### 6.1 SoC level (`arch/arm64/boot/dts/mediatek/mt6735.dtsi`)

Camera pin state, transcribed from the downstream board file
`mt6737t_grandpplte_ltn_dtv_03.dts:584-622` (`camera_pins_default`) and
`:514-584` (`cam0_rst0/1`, `cam0_pnd0/1`, `cam1_rst0/1`, `cam1_pnd0/1`,
`cam_ldo0_0/1`). Macros come from
`<dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h>`:

```
&pio {
	camera_pins_default: camdefault {
		/* VTCAM_RST_N, downstream board DTS:586-591 */
		pins_cmd_dat1 {
			pinmux = <PINMUX_GPIO12__FUNC_GPIO12>;
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		/* CAM_TORCH_EN, downstream board DTS:593-598 */
		pins_cmd_dat2 {
			pinmux = <PINMUX_GPIO42__FUNC_GPIO42>;
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		/* CAM_FLASH_EN, downstream board DTS:600-605 */
		pins_cmd_dat3 {
			pinmux = <PINMUX_GPIO43__FUNC_GPIO43>;
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		/* CAM_RST_N, downstream board DTS:607-612 */
		pins_cmd_dat4 {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		/* CAM_MCLK (main sensor MCLK), downstream board DTS:614-619 */
		pins_cmd_dat5 {
			pinmux = <PINMUX_GPIO45__FUNC_CMMCLK0>;
			drive-strength = <2>;	/* 2 mA, downstream drive-strength = <0> */
			slew-rate = <1>;
			bias-disable;
		};
		/* VTCAM_MCLK (sub sensor MCLK), downstream board DTS:621-626 */
		pins_cmd_dat6 {
			pinmux = <PINMUX_GPIO46__FUNC_CMMCLK1>;
			drive-strength = <2>;
			slew-rate = <1>;
			bias-disable;
		};
	};

	/*
	 * Downstream per-sensor reset/powerdown states, kept here for the
	 * downstream mapping record only.  The merged DTS does NOT use them:
	 * the SENINF camera_pins_default state below sets the pin function and
	 * the sensor drivers toggle the lines through reset-gpios /
	 * powerdown-gpios.
	 */
	camera_pins_cam0_rst0: cam0-rst0-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			slew-rate = <1>;
			output-low;
		};
	};
	camera_pins_cam0_rst1: cam0-rst1-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			slew-rate = <1>;
			output-high;
		};
	};
	camera_pins_cam0_pnd0: cam0-pnd0-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			slew-rate = <1>;
			output-low;
		};
	};
	camera_pins_cam0_pnd1: cam0-pnd1-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO44__FUNC_GPIO44>;
			slew-rate = <1>;
			output-high;
		};
	};
	camera_pins_cam1_rst0: cam1-rst0-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO12__FUNC_GPIO12>;
			slew-rate = <1>;
			output-low;
		};
	};
	camera_pins_cam1_rst1: cam1-rst1-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO12__FUNC_GPIO12>;
			slew-rate = <1>;
			output-high;
		};
	};
	camera_pins_cam1_pnd0: cam1-pnd0-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO12__FUNC_GPIO12>;
			slew-rate = <1>;
			output-low;
		};
	};
	camera_pins_cam1_pnd1: cam1-pnd1-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO12__FUNC_GPIO12>;
			slew-rate = <1>;
			output-high;
		};
	};
	camera_pins_ldo0_0: cam-ldo0-0-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO58__FUNC_GPIO58>;
			slew-rate = <1>;
			output-low;
		};
	};
	camera_pins_ldo0_1: cam-ldo0-1-pins {
		pins_cmd_dat {
			pinmux = <PINMUX_GPIO58__FUNC_GPIO58>;
			slew-rate = <1>;
			output-high;
		};
	};
};
```

SENINF node, inside `/soc` (`dt-bindings/power/mt6735-power.h` and
`dt-bindings/clock/mediatek,mt6735-topckgen.h` are already included there):

```
		seninf: seninf@15008000 {
			compatible = "mediatek,mt6735-seninf";
			reg = <0 0x15008000 0 0x4000>,	/* SENINF, SENINF_REG_RANGE */
			      <0 0x1500c000 0 0x1000>,	/* MIPIRX_CONFIG */
			      <0 0x10215000 0 0x3000>;	/* MIPIRX_ANALOG */

			clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
			clock-names = "camtg";
			/*
			 * Sensor MCLK must be 24 MHz: .mclk = 24 in
			 * imx219mipiraw_Sensor.c:159 and
			 * s5k5e3yxmipiraw_Sensor.c:149.  Downstream selects the
			 * "48 MHz group" camtg parent univpll_d26
			 * (kd_sensorlist.c:2454-2455); univpll_d26 is
			 * univpll / 26 (clk-mt6735-topckgen.c:88), so the
			 * resulting MCLK is 24 MHz for the 624 MHz UNIVPLL rate.
			 */
			assigned-clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
			assigned-clock-parents = <&topckgen CLK_TOP_UNIVPLL_D26>;

			power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;

			pinctrl-names = "default";
			pinctrl-0 = <&camera_pins_default>;

			#clock-cells = <1>;
			clock-output-names = "mclk0", "mclk1", "mclk2";

			status = "okay";

			ports {
				#address-cells = <1>;
				#size-cells = <0>;

				/* main camera: IMX219, ISP_MCLK1_EN */
				port@0 {
					reg = <0>;

					seninf_in0: endpoint {
						remote-endpoint = <&imx219_out>;
					};
				};

				/* sub camera: S5K5E3YX, ISP_MCLK2_EN */
				port@1 {
					reg = <1>;

					seninf_in1: endpoint {
						remote-endpoint = <&s5k5e3yx_out>;
					};
				};
			};
		};
```

### 6.2 Board level (`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`)

Consumer side of the MCLK provider and the endpoint back-links; the sensor
nodes themselves belong to the sensor drivers:

```
&i2c0 {
	camera_main: camera@34 {
		compatible = "sony,imx219";
		reg = <0x34>;

		/* ISP_MCLK1_EN, bit 29 of SENINF+0x200 */
		clocks = <&seninf 0>;
		clock-names = "xclk";

		/* cam0_rst0/1 + cam0_pnd0/1 are both GPIO44 on this board */
		reset-gpios = <&pio 44 GPIO_ACTIVE_HIGH>;
		powerdown-gpios = <&pio 44 GPIO_ACTIVE_HIGH>;
		vcama-supply = <&mt6328_vcama_reg>;
		vcamd-supply = <&mt6328_vcamd_reg>;
		vcamaf-supply = <&mt6328_vcam_af_reg>;
		vcamio-supply = <&mt6328_vcam_io_reg>;

		port {
			imx219_out: endpoint {
				remote-endpoint = <&seninf_in0>;
				data-lanes = <1 2 3 4>;	/* SENSOR_MIPI_4_LANE */
			};
		};
	};

	camera_sub: camera@20 {
		compatible = "samsung,s5k5e3yx";
		reg = <0x20>;

		/* ISP_MCLK2_EN, bit 29 of SENINF+0x600 */
		clocks = <&seninf 1>;
		clock-names = "xclk";

		/* cam1_rst0/1 + cam1_pnd0/1 are both GPIO12 on this board */
		reset-gpios = <&pio 12 GPIO_ACTIVE_HIGH>;
		powerdown-gpios = <&pio 12 GPIO_ACTIVE_HIGH>;
		vcama-supply = <&mt6328_vcama_reg>;
		vcamd-supply = <&mt6328_vcamd_reg>;
		vcamio-supply = <&mt6328_vcam_io_reg>;	/* no VCAM_AF on the sub path */

		port {
			s5k5e3yx_out: endpoint {
				remote-endpoint = <&seninf_in1>;
				data-lanes = <1 2>;	/* SENSOR_MIPI_2_LANE */
			};
		};
	};
};
```

Notes for the merge:

* Regulator node names must match the MT6328 bindings already used by the board
  DTS (`mt6328_vcama_reg`, `mt6328_vcamd_reg`, `mt6328_vcam_io_reg`,
  `mt6328_vcam_af_reg`); the downstream PMIC node names are
  `mt_pmic_vcama_ldo_reg` etc. (`mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:640-680`).
* `reset-gpios` and the optional `powerdown-gpios` intentionally reference the
  same GPIO, as the board ties CAMRST and CMPDN together (board DTS
  `:514-570`). Both are `GPIO_ACTIVE_HIGH`: driver value 1 is the out-of-reset /
  enabled level, matching the downstream `output-high` ON state
  (board DTS `:521-527`, `:535-541`).
* The subsystem DTS must not define both the SENINF `camera_pins_default` state
  and per-sensor pinctrl states for GPIO44/GPIO12/GPIO45/GPIO46. The merged DTS
  keeps the SENINF default state (section 6.1) and drops the sensor-local
  `cam0_rst*/cam0_pnd*/cam1_rst*/cam1_pnd*` states, so reset/powerdown toggling
  happens only through `reset-gpios`/`powerdown-gpios`.
* GPIO45 (CMMCLK0) and GPIO46 (CMMCLK1) are the two MCLK pads; downstream sets
  both in the same default group (`:614-626`), as done here. GPIO47/48 are
  omitted from the group because `i2c0` already owns them through
  `i2c0_default` (`mt6735.dtsi:303`).
* `camera_pins_ldo0_0/1` (GPIO58) are provided for completeness; downstream
  disables the LDO0 path in software (`kd_camera_hw.c:184-191`, `#if 0`).
* The single `camtg` mux feeds all three MCLK outputs (as downstream), so all
  cameras run at the same MCLK frequency.
* Both sensors require 24 MHz (`imx219mipiraw_Sensor.c:159`,
  `s5k5e3yxmipiraw_Sensor.c:149`). The parent chosen above reproduces the
  downstream 48 MHz-group selection; the exact rate follows the
  bootloader-programmed UNIVPLL rate and must be verified on hardware
  (`clk_get_rate()` on the sensor's `xclk`), see section 5 item 3.
* `link-frequencies` are deliberately omitted: no sensor mode table value has
  been verified for the port, and the sensor drivers own that property.

## 7. Patch path

`$M/patches/camera-seninf.patch` (6 files: 2 parent Kconfig/Makefile one-liners
plus the 4 new files; no `.config`, no build artefacts).
