# Camera subsystem coordinator report (MT6737T grandpplte -> Nura 7.0.10 arm64)

Date: 2026-10-24
Coordinator worktree: `$M/worktrees/camera-integration` (fresh from `src/nura-linux` HEAD `046883c0c`)
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## Result summary

| # | Driver | Worktree | Patch | Leaf report | Build |
|---|---|---|---|---|---|
| 1 | IMGSYS/ISP platform | `worktrees/camera-imgsys` | `patches/camera-imgsys.patch` | `logs/camera-imgsys.md` | pass, 0 warnings |
| 2 | SENINF + camera hardware control | `worktrees/camera-seninf` | `patches/camera-seninf.patch` | `logs/camera-seninf.md` | pass, 0 warnings |
| 3 | IMX219 sensor (V4L2 subdev) | `worktrees/camera-imx219` | `patches/camera-imx219.patch` | `logs/camera-imx219.md` | pass, 0 warnings |
| 4 | S5K5E3YX sensor (V4L2 subdev) | `worktrees/camera-s5k5e3yx` | `patches/camera-s5k5e3yx.patch` | `logs/camera-s5k5e3yx.md` | pass, 0 warnings |
| 5 | Lens/VCM + cam_cal EEPROM | `worktrees/camera-lens-cal` | `patches/camera-lens-cal.patch` | `logs/camera-lens-cal.md` | pass, 0 warnings |

Subsystem integration: all five patches apply to a clean HEAD (one parent-file conflict, resolved below) and the six changed objects plus both new platform directories compile with `W=1` and zero warnings/errors.

Counts from the leaf parity matrices: 81 `done`, 4 `partial` (all in IMX219 control semantics), 23 `deferred` (all hardware/proprietary-interface items listed in section 10).

## 1. Leaf workers

Five leaf workers were spawned, one per worktree, each from `$M/src/nura-linux` HEAD. No two workers shared a worktree. Deliverables per leaf: source changes, `$M/patches/camera-<driver>.patch` (`git diff --binary`, new files added with `git add -N`), `$M/logs/camera-<driver>.md` with the required 7 sections, and `$M/logs/camera-<driver>-build.log`.

Files owned/changed by each leaf:

- **camera-imgsys**: `drivers/media/platform/mediatek/imgsys/{Kconfig,Makefile,mtk-imgsys.c,mtk-imgsys-regs.h}` (new), parent `drivers/media/platform/mediatek/{Kconfig,Makefile}`.
- **camera-seninf**: `drivers/media/platform/mediatek/seninf/{Kconfig,Makefile,mtk-seninf.c,mtk-seninf-regs.h}` (new), parent `drivers/media/platform/mediatek/{Kconfig,Makefile}`.
- **camera-imx219**: `drivers/media/i2c/imx219.c` only (variant support, generic path preserved).
- **camera-s5k5e3yx**: `drivers/media/i2c/s5k5e3yx.c` (new), `drivers/media/i2c/{Kconfig,Makefile}`.
- **camera-lens-cal**: `drivers/media/i2c/dw9807-vcm.c` (edit), `drivers/media/i2c/imx219-eeprom.c` (new), `drivers/media/i2c/{Kconfig,Makefile}`.

No worker edited DTS/DTSI, committed, or touched another worktree.

## 2. Architecture (fixed for the subsystem)

1. **Sensors own their power.** Each sensor is a self-contained V4L2 subdev with regulators, reset (and optional powerdown) GPIOs, and an `xclk` from CCF. This is the modern replacement for downstream `kdCISModulePowerOn()`; the rail order/voltages/delays and GPIO polarity are ported into each sensor's `power_on()/power_off()` and mapped in the leaf reports.
2. **`mtk-seninf` owns the only media device.** It creates `v4l2_device` + `media_device`, registers the `mtk-seninf` subdev (sink pad per DT port, one source pad), binds sensor subdevs through fwnode endpoints with `v4l2_async_nf_*`, and creates sensor->SENINF links in `bound`. `v4l2-async` registers the remote subdev into the notifier's `v4l2_device`, so the sensor entities land in the same media device (verified in `drivers/media/v4l2-core/v4l2-async.c:341-380`).
3. **`mtk-seninf` exposes per-camera MCLK clocks** (`#clock-cells=<1>`, `mclk0..mclk2`). Enable/disable toggles the documented `ISP_MCLKn_EN` bit (bit 29 at `SENINF+0x200/+0x600/+0xA00`), with `camtg` as the CCF parent so the core provides rate and refcounting. This keeps sensor drivers free of any private cross-driver API.
4. **`mtk-imgsys` is a platform/clock/power/reset driver without a V4L2 entity or video node.** The MTK ISP pipeline requires the proprietary userspace ABI (`libcamdrv`) and ISP firmware; registering a capture node would be a fake success path. The driver implements only documented behaviour and documents the deferred pipe.
5. **One media device** for the whole camera subsystem. No other driver creates one.

## 3. Integration

Worktree: `$M/worktrees/camera-integration` (created from HEAD, `.config` untracked, no commits).

Application order: imgsys, seninf, imx219, s5k5e3yx, lens-cal.

**Conflict**: `camera-imgsys` and `camera-seninf` both append to `drivers/media/platform/mediatek/Kconfig` and `Makefile` (the same trailing hunk). Resolution:
- Applied `camera-seninf.patch` with `--exclude` for those two files, then appended `source "drivers/media/platform/mediatek/seninf/Kconfig"` and `obj-y += seninf/` after the imgsys entries.
- Result (final):
  - `Kconfig`: `... mdp3/Kconfig`, `imgsys/Kconfig`, `seninf/Kconfig`
  - `Makefile`: `obj-y += mdp3/`, `obj-y += imgsys/`, `obj-y += seninf/`
- The two `drivers/media/i2c/Kconfig`/`Makefile` edits (lens-cal near `VIDEO_IMX219`/`imx219.o`, s5k5e3yx near `VIDEO_S5K5BAF`/`rj54n1cb0c.o`) are in disjoint regions and applied without conflict.

Integrated diffstat (`git diff --stat` after `git add -N` of the new files):

```
 drivers/media/i2c/Kconfig                                  |   22 +
 drivers/media/i2c/Makefile                                 |    2 +
 drivers/media/i2c/dw9807-vcm.c                             |   83 +-
 drivers/media/i2c/imx219-eeprom.c                          |  392 +
 drivers/media/i2c/imx219.c                                 |  895 +-
 drivers/media/i2c/s5k5e3yx.c                               | 1343 +
 drivers/media/platform/mediatek/Kconfig                    |    2 +
 drivers/media/platform/mediatek/Makefile                   |    2 +
 drivers/media/platform/mediatek/imgsys/Kconfig             |   25 +
 drivers/media/platform/mediatek/imgsys/Makefile            |    2 +
 drivers/media/platform/mediatek/imgsys/mtk-imgsys-regs.h   |   38 +
 drivers/media/platform/mediatek/imgsys/mtk-imgsys.c        |  300 +
 drivers/media/platform/mediatek/seninf/Kconfig             |   16 +
 drivers/media/platform/mediatek/seninf/Makefile            |    2 +
 drivers/media/platform/mediatek/seninf/mtk-seninf-regs.h   |   73 +
 drivers/media/platform/mediatek/seninf/mtk-seninf.c        |  822 +
 16 files changed, 3915 insertions(+), 104 deletions(-)
```

No DTS/DTSI file is modified in any worktree (`git status --porcelain | grep -E '\.dts|\.dtsi'` is empty).

## 4. Compile results

Toolchain/PATH per the assignment:

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-integration"
cp "$M/nura.config" .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CONTROLLER \
  -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC \
  -e VIDEO_CAMERA_SENSOR -e MEDIA_CAMERA_SUPPORT -e I2C -e REGULATOR \
  -e COMMON_CLK -e NVMEM -e DEBUG_FS -e MEDIA_PLATFORM_SUPPORT \
  -e VIDEO_MTK_IMGSYS -e VIDEO_MTK_SENINF -e VIDEO_IMX219 \
  -e VIDEO_S5K5E3YX -e VIDEO_DW9807_VCM -e VIDEO_IMX219_EEPROM
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
  drivers/media/platform/mediatek/imgsys/ \
  drivers/media/platform/mediatek/seninf/ \
  drivers/media/i2c/imx219.o drivers/media/i2c/s5k5e3yx.o \
  drivers/media/i2c/dw9807-vcm.o drivers/media/i2c/imx219-eeprom.o
```

`olddefconfig` accepted every new symbol: `CONFIG_VIDEO_MTK_IMGSYS=y`, `CONFIG_VIDEO_MTK_SENINF=y`, `CONFIG_VIDEO_IMX219=y`, `CONFIG_VIDEO_S5K5E3YX=y`, `CONFIG_VIDEO_DW9807_VCM=y`, `CONFIG_VIDEO_IMX219_EEPROM=y`.

Integration build result (`logs/camera-integration-build.log`):

```
CC drivers/media/i2c/imx219.o
CC drivers/media/platform/mediatek/imgsys/mtk-imgsys.o
AR drivers/media/platform/mediatek/imgsys/built-in.a
CC drivers/media/platform/mediatek/seninf/mtk-seninf.o
AR drivers/media/platform/mediatek/seninf/built-in.a
CC drivers/media/i2c/s5k5e3yx.o
CC drivers/media/i2c/dw9807-vcm.o
CC drivers/media/i2c/imx219-eeprom.o
exit 0, 0 warnings / 0 errors
```

Second pass with `W=1` (`logs/camera-integration-w1.log`): all six objects recompiled, `0 warnings / 0 errors`.

Leaf-level builds (each in its own worktree) also passed with 0 warnings; logs at `logs/camera-{imgsys,seninf,imx219,s5k5e3yx,lens-cal}-build.log`.

Config note for the board: `nura.config` currently has `CONFIG_MEDIA_SUPPORT` unset and `CONFIG_MEDIA_SUPPORT_FILTER=y`. Enabling `MEDIA_SUPPORT` while the filter is on silently hides `VIDEO_DEV`/`VIDEO_CAMERA_SENSOR`, which drops every camera option at `olddefconfig`. The board defconfig must therefore set `CONFIG_MEDIA_SUPPORT=y`, `# CONFIG_MEDIA_SUPPORT_FILTER is not set` (or `CONFIG_MEDIA_CAMERA_SUPPORT=y`), plus the options above. This is a configuration change only; no source file encodes it.

## 5. Parity summary per driver

Full matrices are in the leaf reports; this is the subsystem-level status.

### 5.1 IMGSYS/ISP platform (`logs/camera-imgsys.md`): 7 done / 0 partial / 8 deferred
Implemented: ISP core + inner-ISP windows (`0x15004000+0x4000`, `0x1500d000+0x1000`), ISP genpd (`MT6735_POWER_DOMAIN_ISP`), the six IMGSYS clock gates in downstream enable order, documented Pass-1 `CAM_SW_CTL` soft reset on probe and resume (`camera_isp_D1.c:244,370-377,3263-3300`), runtime/system PM, debugfs register dump.
Deferred: SMI/DMA clock, Pass-2/CAMSV resets (registers outside the claimed window), all ISP interrupt handling (downstream has no documented source-enable or write-to-clear register, so requesting the level-low IRQs would storm), the entire Pass1/Pass2/DSP/CQ/DMA pipeline, and any V4L2 entity (by design).

### 5.2 SENINF / camera hardware control (`logs/camera-seninf.md`): 14 done / 0 partial / 4 deferred
Implemented: three documented windows (SENINF `0x15008000`, MIPIRX config `0x1500c000`, MIPI RX analog `0x10215000`), ISP genpd, `camtg` parent clock, per-camera MCLK clock provider with documented bit 29 semantics, SENINF error status decode/log/clear (bits from `camera_isp_D1.h:267-298`), single media device + `mtk-seninf` subdev + async notifier, stream-on link/lane validation and logging, debugfs dumps.
Deferred: MIPI D-PHY/lane/settle programming (register table exists only in proprietary `seninf_drv`; the ROM kernel has zero SENINF symbols), `set_rate`/`set_parent` on the MCLK outputs (the shared `camtg` mux is configured from DT), the legacy `/dev/camera_hw` ABI, and the ISP pipeline connection (source pad intentionally unlinked).

### 5.3 IMX219 (`logs/camera-imx219.md`): 16 done / 4 partial / 4 deferred
Implemented: `samsung,grandpplte-imx219` compatible (generic `sony,imx219` path unchanged), four mode tables covering the five downstream scenarios (3264x2456@30, 3280x2464@24, 816x460@120, 1640x1228@15) copied verbatim from the downstream setting functions, per-mode pixel rate/link frequency, frame-line and line-length semantics, per-rail power sequence and delays of the IMX219 branch (`kd_camera_hw.c:485-530`, `:766-808`), module orientation, 4-lane enforcement, stream-time link-freq validation.
Partial: exposure min/auto-extension (V4L2 model, userspace raises vblank), analogue gain kept as the raw register value instead of the downstream 64=1x ISP-gain table, test-pattern window uses the active format, reset/powerdown are physically the same GPIO44 on this board (only one descriptor).
Deferred: sensor-side MIPI settle (receiver parameter), EEPROM/AF (separate drivers), ISP pipeline.

### 5.4 S5K5E3YX (`logs/camera-s5k5e3yx.md`): 25 done / 0 partial / 5 deferred
Implemented: full 1343-line driver with chip-ID probe at both 7-bit addresses (0x10/0x36), init table and four mode tables (2576x1932@30, 2560x1920@15, 640x480@120, 1280x960@15) verified against downstream (92 init + 45/51/45/45 entries; the two missing init writes are the separate standby/stream-on writes), exposure/gain formulas, test pattern, PLL-derived link frequencies 448/218 MHz, and the S5K5E3YX branch power sequence (`kd_camera_hw.c:325-366`, `:642-683`).
Deferred: digital gain and hflip/vflip (not implemented downstream / inside `#if 0`), OTP/lens-shading pages (MTK NVM HAL ABI only), scenario frame delays (AE/ISP, not sensor), MIPI settle 85 ns (SENINF-side).

### 5.5 Lens/VCM + cam_cal (`logs/camera-lens-cal.md`): 19 done / 0 partial / 2 deferred
Implemented: `samsung,dw9807-vcm` compatible with match-data-gated ring-control power-up, existing dongwoon path byte-for-byte unchanged, 10-bit DAC math verified against downstream, power-down unchanged; read-only nvmem provider for the IMX219 module EEPROM (16-bit big-endian byte addressing, 7-bit 0x58 from `0xB0 >> 1`), sysfs `mfr_info`/`module_id`/`af_macro_cal`/`af_pan_cal`/`afcal` matching downstream getters and `rear_afcal` format, lazy reads (no I2C at probe, matching downstream's power ordering).
Deferred: `read_imx219_eeprom_mtk_fmt()` (different slave aliases, only used by the mt6735m sensor variant) and the Samsung LED-class firmware string checks (policy, not calibration data).

## 6. Review fixes applied during integration

1. **DW9807 ring-control mode was applied to every compatible** (would change existing `dongwoon,dw9807-vcm` behaviour). Fixed by the leaf: match data (`ring_mode` true only for `samsung,dw9807-vcm`), the generic path still writes `0x02 = 0x00` on resume. Rebuilt, patch/report regenerated.
2. **SENINF MCLK was not driven during sensor probe.** The original clock `prepare` deferred the enable bit while SENINF was runtime-suspended, so a sensor's probe-time `power_on()` would run without MCLK. Fixed by the leaf: `prepare` takes a runtime-PM reference before writing bit 29, `unprepare` clears it and drops the reference; the SENINF runtime callbacks no longer touch `camtg` (it is enabled through the CCF parent chain), which also removes any prepare-lock re-entrancy. This was verified safe because the MT6735 ISP genpd node has no clocks (`arch/arm64/boot/dts/mediatek/mt6735.dtsi:223-227`), so genpd power-on is SPM register access only. Rebuilt with `W=1`, patch/report regenerated.
3. **Sensor local pinctrl states vs SENINF camera default state.** The IMX219 fragment defined a local pinctrl state for GPIO44 while the SENINF fragment defines the downstream `camera_pins_default` covering GPIO12/42/43/44/45/46/58. Duplicate claims are unnecessary; the consolidated fragment below keeps the SENINF default state and drops the sensor-local one. Sensor nodes keep `reset-gpios` (the pin mux is already GPIO; the sensor clock phandle forces probe after SENINF, so the default state is applied first).
4. **Report correction applied**: the SENINF report's `kd_camera_hw` mapping table had used generic `avdd/dvdd/dovdd/afvdd` and `GPIO_ACTIVE_LOW`; the delivered drivers use `vcama/vcamd/vcamaf/vcamio` and active-high reset. The SENINF report was updated (table rows, sensor examples in §6.2, pinctrl dedup note); no code, build, or patch change.

## 7. Cross-driver contract checks

- **Media device count**: exactly one, created and destroyed by `mtk-seninf`; imgsys registers no entity; sensor drivers call only `v4l2_async_register_subdev_sensor()`.
- **Clock contract**: sensors `clocks = <&seninf N>`, `clock-names = "xclk"`; N=0 for IMX219 (`ISP_MCLK1_EN`, downstream cam0/GPIO45), N=1 for S5K5E3YX (`ISP_MCLK2_EN`, cam1/GPIO46). The MCLK provider holds the ISP domain on while any MCLK is prepared, so sensor probe-time power-on gets a real clock.
- **I2C addresses** (8-bit downstream -> 7-bit DT): IMX219 `0x34 -> 0x1a`; S5K5E3YX `0x20/0x6c -> 0x10/0x36` (driver retries both); DW9807 `0x18 -> 0x0c`; module EEPROM `0xB0 -> 0x58`.
- **Rails**: all four camera LDOs are shared on this board. IMX219 programs vcama 2.8 V, vcamd 1.2 V, vcamaf 2.8 V, vcamio 1.8 V; S5K5E3YX programs vcama 2.8 V, vcamd 1.2 V, vcamio 1.8 V. Both sensors use vcamd = 1.2 V, so the shared-rail voltage note in `logs/camera-s5k5e3yx.md` ("main sensor uses 1.0 V") is superseded: the 1.0 V figure belongs to the imx135/imx220 branch, and the actual IMX219 branch uses 1.2 V (`kd_camera_hw.c:500`).
- **Pinctrl**: downstream cam0 rst and pnd both mux GPIO44, cam1 rst and pnd both mux GPIO12; the drivers request one active-high reset line per camera and keep `powerdown-gpios` optional.
- **Reset polarity**: `GPIO_ACTIVE_HIGH`, driver value 1 = out of reset, matching downstream `output-high` as the ON level.

## 8. Consolidated DTS fragment

The leaf reports contain the complete text (`logs/camera-imgsys.md`, `logs/camera-seninf.md` §6, `logs/camera-imx219.md`, `logs/camera-s5k5e3yx.md`, `logs/camera-lens-cal.md`). When applying them, use the following node-level result and the noted deduplication. No DTS is changed by this subsystem patch set.

SoC level (`arch/arm64/boot/dts/mediatek/mt6735.dtsi`):

```dts
/* 1. ISP platform node (from camera-imgsys) */
ispsys: isp@15004000 {
	compatible = "mediatek,mt6735-ispsys";
	reg = <0 0x15004000 0 0x4000>, <0 0x1500d000 0 0x1000>;
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	status = "disabled";	/* board DTS enables it */
};

/* 2. SENINF + MCLK provider + media device owner (from camera-seninf §6) */
seninf: seninf@15008000 {
	compatible = "mediatek,mt6735-seninf";
	reg = <0 0x15008000 0 0x4000>,	/* SENINF */
	      <0 0x1500c000 0 0x1000>,	/* MIPIRX config */
	      <0 0x10215000 0 0x3000>;	/* MIPI RX analog */
	clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
	clock-names = "camtg";
	assigned-clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
	assigned-clock-parents = <&topckgen CLK_TOP_UNIVPLL_D26>;	/* 24 MHz group */
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	pinctrl-names = "default";
	pinctrl-0 = <&camera_pins_default>;	/* GPIO12/42/43/44/45/46/58, seninf §6.1 */
	#clock-cells = <1>;
	clock-output-names = "mclk0", "mclk1", "mclk2";
	status = "okay";
	ports {
		#address-cells = <1>;
		#size-cells = <0>;
		port@0 { reg = <0>; seninf_in0: endpoint { remote-endpoint = <&imx219_out>; }; };
		port@1 { reg = <1>; seninf_in1: endpoint { remote-endpoint = <&s5k5e3yx_out>; }; };
	};
};
```

Board level (`mt6737t-samsung-grandpplte.dts`, `&i2c0`): IMX219 at 0x1a (`clocks = <&seninf 0>`, vcama/vcamd/vcamaf/vcamio supplies, `reset-gpios = <&pio 44 GPIO_ACTIVE_HIGH>`, `data-lanes = <1 2 3 4>`, `link-frequencies = /bits/ 64 <416000000 344000000 340000000>`), S5K5E3YX at 0x10 (`clocks = <&seninf 1>`, vcama/vcamd/vcamio, `reset-gpios = <&pio 12 GPIO_ACTIVE_HIGH>`, `data-lanes = <1 2>`, `link-frequencies = /bits/ 64 <448000000 218000000>`), DW9807 at 0x0c (`samsung,dw9807-vcm`), EEPROM at 0x58 (`samsung,imx219-eeprom` + fixed-layout cells). Endpoint labels on the sensor side are `imx219_out` and `s5k5e3yx_out`, linked to `seninf_in0`/`seninf_in1`; the `seninf_cam1_in` placeholder in the S5K5E3YX fragment must be renamed to `seninf_in1`.

Deduplication rule: keep the SENINF `camera_pins_default` state and remove the sensor-local `imx219_pins_default` state from the IMX219 fragment. Sensor nodes reference only `reset-gpios`/optional `powerdown-gpios`; they do not need pinctrl states because SENINF is guaranteed to probe first (the sensor clock phandle defers otherwise).

Application notes (verified against the Nura tree):
- `mt6735.dtsi` has no `soc` label. A board-level overlay must use `&{/soc} { ... }`, not `&soc { ... }`. The two SoC nodes above can also be inserted textually into the dtsi (as the leaf fragments say).
- The SoC node uses `#address-cells=<2>`, `#size-cells=<2>`, so every SoC-level `reg` is four cells: `<0 ADDR 0 SIZE>`, exactly as shown above.
- `i2c0` has `#address-cells=<1>`, `#size-cells=<0>`, so the sensor/VCM/EEPROM `reg` properties are the single 7-bit address cells shown.
- GPIO/pinctrl macros require `<dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h>`; the pin-function names (`PINMUX_GPIO44__FUNC_GPIO44`, `PINMUX_GPIO45__FUNC_CMMCLK0`, ...) are the ones used by Nura's `pinctrl-mtk-mt6735.h`.

Known integration risk to verify on hardware: two devices referencing the same pins in pinctrl is tolerated only while the mux function is identical (both are `PINMUX_GPIOxx__FUNC_GPIOxx`), but the clean form is a single default-state owner.

## 9. Unresolved gaps (subsystem level)

1. **MIPI D-PHY/SENINF lane and settle-time programming** is not implemented. The register offset/value table exists only in the proprietary userspace `seninf_drv` referenced by `camera_isp_D1.c:125-133`; the ROM kernel contains no SENINF symbols (`zip/symbols.txt`, zero matches), so there is no kernel-side source to port. Nothing was invented. Consequence: sensor -> SENINF streaming is not functional end-to-end even though clocks, MCLK, MIPI errors, topology and controls are implemented.
2. **ISP pipeline / capture** is deferred. `cameraisp` is a userspace-driven char device (`libcamdrv`) plus ISP firmware; no V4L2 capture node was fabricated. The IMGSYS driver therefore has no V4L2 entity.
3. **ISP interrupt handling** is not enabled: the downstream source shows software masks (`camera_isp_D1.c:519-613`) but no interrupt-enable or write-to-clear register, so requesting the level-low CAM/CAMSV lines would produce an unacknowledgeable storm.
4. **OTP / lens shading** for S5K5E3YX and the legacy cam_cal `mtk_fmt` block are MTK HAL-only interfaces with no V4L2/nvmem equivalent in scope.
5. **Partial IMX219 control semantics**: exposure does not auto-extend the frame length and analogue gain keeps raw-register units rather than the downstream ISP-gain table. Both are userspace-model choices, documented in `logs/camera-imx219.md` §3/§5.
6. **Register values from ROM** were used only as confirmation; no ROM disassembly was needed for the ported register programming. `mt_imgsys_init` in the ROM is the downstream clock init (`clk-mt6735.c:1189`), already provided by Nura's `clk-mt6735-imgsys.c`; it is not reimplemented.

## 10. Hardware-unverified items

- SENINF MCLK bit clear semantics and the error-status write-one-to-clear convention (`[INFERENCE]`; the helper warns if bits persist).
- Link frequencies 416/344/340 MHz (IMX219) and 448/218 MHz (S5K5E3YX) are derived from PLL registers (`[INFERENCE]`, formulas in the leaf reports; the IMX219 formula reproduces the generic 456/364 MHz rates exactly).
- Exact 24 MHz MCLK depends on the bootloader-programmed UNIVPLL rate feeding `univpll_d26`.
- Reset/powerdown are the same physical pin on both camera ports; only one descriptor is used.
- Power rail ordering/delays come from `kd_camera_hw.c` but regulator ramp times and the shared-LDO coupling are board-dependent.
- I2C readback of chip IDs requires the camera rails and MCLK; sensor probe ordering is guaranteed by the clock phandle, but the electrical timing is unverified.
- Bayer order after flip for the Samsung IMX219 module mounting.

## 11. Evidence anchors used across the subsystem

- ROM symbols: `CAMERA_HW_probe`, `CAMERA_HW_i2c_probe`, `CAM_CAL_probe`, `IMX219_MIPI_RAW_SensorInit`, `S5K5E3YX_MIPI_RAW_SensorInit`, `s4DW9807AF_Eflash_ReadReg/WriteReg`, `get_imx219_*`, `mt_imgsys_init` (`zip/symbols.txt`).
- ROM device tree: `rom_dtb_0.dts:3282-3299` (i2c0 camera/af/sub nodes), `:3697-3760` (imgsys/ispsys/kd_camera_hw1/2), `:3128-3129` (SENINF CMDQ events).
- Downstream: `imgsensor/src/mt6735/{imx219_mipi_raw,s5k5e3yx_mipi_raw,camera_hw,kd_sensorlist.c}`, `cameraisp/src/mt6735/{camera_isp_D1.c,camera_isp_D1.h}`, `cam_cal/src/mt6735/imx219_eeprom`, `lens/common/dw9807af`, board DTS `mt6737t_grandpplte_ltn_dtv_03.dts` / `..._gpio_04.dtsi`, `mt6735.dtsi:1259-1330`.
- ROM strings: SENINF/CAMSV CMDQ event names, `MUX_CAMTG`, `camtg_sel`, `rear_afcal`/`moduleid` consumers.

## 12. Reproduction

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
cd "$M/worktrees/camera-integration"     # or a clean worktree from $M/src/nura-linux HEAD
git apply "$M/patches/camera-imgsys.patch"
git apply --exclude=drivers/media/platform/mediatek/Kconfig \
          --exclude=drivers/media/platform/mediatek/Makefile \
          "$M/patches/camera-seninf.patch"
printf 'source "drivers/media/platform/mediatek/seninf/Kconfig"\n' >> drivers/media/platform/mediatek/Kconfig
printf 'obj-y += seninf/\n' >> drivers/media/platform/mediatek/Makefile
git apply "$M/patches/camera-imx219.patch"
git apply "$M/patches/camera-s5k5e3yx.patch"
git apply "$M/patches/camera-lens-cal.patch"
# then the config + make commands in section 4
```

The combined series is what `worktrees/camera-integration` contains and has also been exported as a single patch that applies cleanly to `$M/src/nura-linux` HEAD (`git apply --check` verified):

- `$M/patches/camera-subsystem.patch` (all 16 files, 4466 lines)
- `$M/worktrees/camera-integration` (built and `W=1` verified)

The five per-driver patches remain the reviewable units.

## 13. Deferred-item index

- imgsys: SMI/DMA clock, Pass-2 and CAMSV resets, CAM/CAMSV interrupts, ISP Pass1/Pass2/DSP/CQ/DMA pipeline, V4L2 entity, CAMINF config access.
- seninf: MIPI D-PHY/lane/settle programming, MCLK rate/parent control at runtime, legacy `/dev/camera_hw` ABI, ISP pipeline link.
- imx219: sensor-side settle, EEPROM/AF (separate drivers), ISP pipeline; partial exposure/gain/test-window semantics.
- s5k5e3yx: digital gain, flips, OTP pages, scenario frame delays, 85 ns settle (SENINF-side).
- lens-cal: legacy `mtk_fmt` EEPROM block, Samsung firmware string policy checks.
