# Camera remaining-gap coordinator report (MT6737T grandpplte -> Nura 7.0.10 arm64)

Date: 2026-10-24
Coordinator worktree: `$M/worktrees/rem-camera-integration`
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

Baseline: `$M/patches/parity-baseline-all.patch`, `git diff --binary` vs `$M/src/nura-linux`
HEAD `046883c0c`, sha256 `fbd5585b5c06000965496eb149ec4ef3c0b6ce20f3b4bbd10c02cb7d8083f5b6`
(regenerated 22:00:51 with the two `drivers/misc` fixups). `git apply --check` on a clean
worktree: clean. The two baseline fixups (`source "drivers/misc/mediatek/Kconfig"` and
`obj-y += mediatek/`) are present in the baseline used here and were additionally hand-applied to
`worktrees/rem-camera-base` before that copy was regenerated; they are not part of any camera
patch.

## 1. Result summary

| # | Leaf | Worktree | Patch | Report | Kernel build | Userspace |
|---|---|---|---|---|---|---|
| 1 | MIPI D-PHY / lane / settle (SENINF) | `worktrees/rem-camera-dphy` | `patches/rem-camera-dphy.patch` (5 files, +2199/−18) | `logs/rem-camera-dphy.md` | 0 warn / 0 err, `W=1` | extractor helper builds, `make check` 0 mismatches |
| 2 | ISP Pass1/Pass2/DSP/CQ/DMA + char ABI | `worktrees/rem-camera-isp` | `patches/rem-camera-isp.patch` (11 files, +4900/−76) | `logs/rem-camera-isp.md` | 0 warn / 0 err, `W=1` | `make all`, `make check`, `abi32` pass |
| 3 | OTP / lens-shading / 3A tables | `worktrees/rem-camera-otp` | `patches/rem-camera-otp.patch` (12 files, +3141/−6) | `logs/rem-camera-otp.md` | 0 warn / 0 err, `W=1` | selftest PASS, live table extraction from real libs |
| 4 | IMGSYS / SMI / clocks / interrupts | `worktrees/rem-camera-imgsys` | `patches/rem-camera-imgsys.patch` (4 files, +280/−65) | `logs/rem-camera-imgsys.md` | 0 warn / 0 err, `W=1` | n/a |
| — | Integration | `worktrees/rem-camera-integration` | `patches/rem-camera-all.patch` (31 files, +10520/−165) | this file | 0 warn / 0 err, `W=1` | all targets pass |

No leaf commit, no leaf edited another worktree, no DTS/DTSI file is modified by any patch.
All four per-driver patches apply cleanly, in the order dphy → imgsys → otp → isp, on
`HEAD + parity-baseline-all.patch` (verified with `GIT_INDEX_FILE=… git apply --cached --check`
against a baseline tree, and by sequential application).

## 2. Leaves

Each leaf got a fresh detached worktree (`rem-camera-<x>`), the baseline patch, the same
evidence set, and an explicit file-ownership boundary. Evidence sources used:
`zip/vmlinux.elf` (ELF32 ARM, local symbols), `zip/symbols.txt`, `zip/rom_dtb_0.dts`,
`zip/system.raw.img` (debugfs), pre-extracted vendor ELFs in `zip/vendor-libs/`,
`src/downstream-grandpplte`, Ghidra/llvm-objdump.

Ownership split that kept the patches conflict-free:

| Slice | Owned files |
|---|---|
| dphy | `drivers/media/platform/mediatek/seninf/*`, new `tools/mediatek/camera/seninf-dphy/*` |
| isp | new `drivers/media/platform/mediatek/isp/*`, parent `mediatek/{Kconfig,Makefile}`, `tools/mediatek/camera/{Makefile,libcamdrv/*,camdrv/*,tests/*}` |
| otp | new `tools/mediatek/camera/otp/*`, `drivers/media/i2c/s5k5e3yx.c` (OTP attribute only) |
| imgsys | `drivers/media/platform/mediatek/imgsys/*`, `drivers/memory/mtk-smi.c` |

## 3. Integration

Applied in `worktrees/rem-camera-integration` (detached `046883c0c` + baseline): dphy, imgsys,
otp, isp. No conflicts; no hunk had to be resolved. The parent Kconfig/Makefile hook is owned by
isp and the otp tools hook by otp, so they never touch the same lines.

Corrections made by the coordinator during integration (patches and reports regenerated where
needed):

1. **Removed a dead cross-driver API.** `rem-camera-imgsys` had exported
   `mtk_imgsys_reset()` + `include/linux/mtk-imgsys.h` for the char driver, but the char driver
   implements the same vendor sequences itself under its register spinlock (it cannot sleep, so it
   cannot use a helper that takes a runtime-PM reference and a mutex). The export had zero callers.
   Removed the function, export, header and the stale report text; the reset enum is now private to
   `mtk-imgsys.c`. Probe-time resets are unchanged. `rem-camera-imgsys.patch` was regenerated
   (4 files, +280/−65) and re-verified to apply on the baseline.
2. **OTP tools wired into the parent build.** `tools/mediatek/camera/Makefile` now builds the OTP
   provider from `all:` and runs its selftest from both `check:` variants; this is included in
   `rem-camera-otp.patch` (12 files) instead of being left as a post-apply manual step.
3. **Report corrections**: imgsys §1/§2/§4/§8 updated for (1); the abbreviated `camera_isp` node
   in the imgsys fragment is marked superseded by the complete node in `rem-camera-isp.md` §8; the
   otp report §2.1 now documents the build wiring as part of the patch.
4. **Baseline fixups** requested by Main/RemWcn (`drivers/misc/Kconfig` sourcing
   `drivers/misc/mediatek/Kconfig`, `drivers/misc/Makefile` descending into `mediatek/`) are in the
   baseline and in the integration tree. They are unrelated to camera (no camera config depends on
   a `CONFIG_MTK_*` misc symbol) but were verified present.

## 4. Build and test evidence

Toolchain: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, `ARCH=arm64 LLVM=1`,
`TMPDIR=$M/tmp` (the shared `/tmp` is a full 12 GiB tmpfs).

Kernel (`logs/rem-camera-integration-build.log`): `cp $M/nura.config .config`, then
`scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV
-e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC -e VIDEO_CAMERA_SENSOR
-e MEDIA_CAMERA_SUPPORT -e I2C -e REGULATOR -e COMMON_CLK -e NVMEM -e DEBUG_FS
-e MEDIA_PLATFORM_SUPPORT -e VIDEO_MTK_IMGSYS -e VIDEO_MTK_SENINF -e VIDEO_MTK_ISP
-e VIDEO_IMX219 -e VIDEO_S5K5E3YX -e VIDEO_DW9807_VCM -e VIDEO_IMX219_EEPROM
-e COMMON_CLK_MT6735_IMGSYS -e MTK_SMI -e MTK_IOMMU`, `olddefconfig` (rc=0), then

```
make ARCH=arm64 LLVM=1 -j2 W=1 \
  drivers/media/platform/mediatek/imgsys/ \
  drivers/media/platform/mediatek/seninf/ \
  drivers/media/platform/mediatek/isp/ \
  drivers/media/i2c/imx219.o drivers/media/i2c/s5k5e3yx.o \
  drivers/media/i2c/dw9807-vcm.o drivers/media/i2c/imx219-eeprom.o \
  drivers/memory/mtk-smi.o drivers/clk/mediatek/clk-mt6735-imgsys.o
```

Result: rc=0, `grep -c 'warning:|error:'` = 0. `.config` has `CONFIG_VIDEO_MTK_IMGSYS=y`,
`CONFIG_VIDEO_MTK_SENINF=y`, `CONFIG_VIDEO_MTK_ISP=y`, `CONFIG_VIDEO_IMX219=y`,
`CONFIG_VIDEO_S5K5E3YX=y`, `CONFIG_VIDEO_DW9807_VCM=y`, `CONFIG_VIDEO_IMX219_EEPROM=y`,
`CONFIG_COMMON_CLK_MT6735_IMGSYS=y`, `CONFIG_MTK_SMI=y`, `CONFIG_MTK_IOMMU=y`.

Userspace (`logs/rem-camera-integration-userspace.log`, `-check.log`, `-tests.log`):

- `make -C tools/mediatek/camera` → 0 warnings; builds `libcamdrv.a/.so`, `camdrv`,
  `camservice`, `abi32`, and `otp/{libcamotp,camotp,camotp_selftest}`.
- `make -C tools/mediatek/camera check` → ENODEV behaviour for both char-device tools,
  `abi32` ABI layout (`ISP_WAIT_IRQ 0x40606b09`, `ISP_BUFFER_CTRL 0xc0206b0e`, structure sizes
  16/96/16/32/136/152/64/1032), `camotp selftest PASS`.
- `make -C tools/mediatek/camera/seninf-dphy check OBJDUMP="llvm-objdump --triple=thumbv7-none-eabi"`
  → "all proof addresses verified", 0 mismatches.
- `camotp tables --libdir $M/zip/vendor-libs --list` reads the seven documented tables out of the
  real vendor ELFs with ELF-size verification; `--dump ShadingATNTable --decode` prints
  address 0x0050af18, 504 bytes and the decoded polynomial convention.

No on-hardware run was possible; every hardware claim below is static evidence plus compile
testing.

## 5. What is now closed

### 5.1 MIPI D-PHY / lane / settle (`logs/rem-camera-dphy.md`)

Recovered from `libcam.halsensor.so` (Thumb-2, symbols from `.dynsym`) and implemented in
`mtk-seninf.c`/`mtk-seninf-regs.h`:

- Per-instance MIPI-RX analog windows: SENINF1 `0x10215000+0x800`, SENINF2 `+0xc00`,
  SENINF3 `+0x1000` (init proof `0x1827e/0x18348/0x1834a/0x18350`, 0x3000 mmap length at
  `0x1823e`).
- `setSeninf1NCSI2 @0x16288`: shared-CFG clears (`0xfefbefbe` on `+0x4c/+0x50`,
  `0x16294/0x1629e`), five lane pre-charge (`|= BIT(3)`, `0x162aa..0x162da`), `+0x24 |= BIT(0)`
  and `+0x20 |= GENMASK(1,0)`, lane enable (`|= BIT(0)`, `0x1630a..0x1633a`), and the nCSI2
  control group at `+0x3a0/+0x3a8/+0x3ac..+0x3b0` with the lane mask
  `(1 << (lane_num + 1)) - 1` (`0x165bc..0x165d8`).
- Settle count: `settle_ns * 300 / 1000` (`mov.w r5,#0x12c @0x1647e`, `mul @0x164d2`,
  `udiv #1000 @0x164da`), default 85 ns (`moveq r7,#0x55 @0x164cc`); lane count from the
  endpoint, settle from both sensors' `mipi_data_lp2hs_settle_dc = 85`.
- Stream-off reverse sequence; registers that the vendor only writes when a runtime value is
  non-zero are left untouched (`this[0x58] == 0` path, `0x16340`).
- The SENINF window only needs `0x1000` (highest offset `0xd20`), freeing the CAMSV bank for the
  ISP platform driver.

Blocked, with exact missing input, in `logs/rem-camera-dphy.md` §6:

1. `SENINF_SOURCE_ENUM`, `PAD2CAM_DATA_ENUM`, `TG_FORMAT_ENUM` numeric values for
   `setSeninfN{Ctrl,MuxCtrl}`/`setSeninfTopMuxCtrl`: call sites in `HalSensor::seninfControl`
   (`0xb426/0xb458/0xb4be/0xb766`) take them from per-sensor structures built from
   `cfg_setting_imgsensor`; no table exists in any extracted binary or in the downstream GPL
   drop. Field layouts are recovered; the fields are deliberately not written.
2. MIPI-RX per-lane map (`this[0x58]`): supplied by an ioctl (`0x80046401`, `0x18392`) to a vendor
   ISP kernel driver that does not exist in this tree. The vendor itself skips the writes when the
   value is zero, which is what the driver now does.

HS deskew (`autoDeskewCalibrationSeninf1 @0x16f34`) is recovered and documented (enable bits,
delay fields, `0x3b0..0x3bc` handshake) but not wired: it is an unbounded hardware scan with
undocumented field semantics and is not part of the stream-on path.

### 5.2 ISP Pass1/Pass2/DSP/CQ/DMA (`logs/rem-camera-isp.md`)

New `drivers/media/platform/mediatek/isp/` (3682-line char driver + 426-line register header):

- `misc` device `camera-isp`, mmap restricted to the five documented windows, register
  read/write(s), hold-register buffers (VD/EXPDONE), per-user IRQ status/wait/mark/clear with user
  keys, RT buffer rings (IMGO/RRZO/IMGO_D/RRZO_D/CAMSV/CAMSV2) with SOF-time base address and
  `RCNT_INC` programming, CQ0C ring download, P1/P2 ED queue, ref-count, register scenario, burst
  queue, reset, SOF/DMA-error/drop-frame queries, debugfs.
- IRQ semantics proven on the ROM ISR: `ISP_Irq_CAM @0xc04ddce8` contains exactly seven status
  loads (`0x4c/0x54/0x5c/0x64/0x70/0x74/0x78`, e.g. `ldr r1,[r3,#0x4c] @0xc04ddda4`,
  `ldr r2,[r3,#0x54] @0xc04dddd4`, `ldr r1,[r3,#0x5c] @0xc04dddec`) and zero stores to them →
  read-to-clear; DONE serviced before SOF; no hardware enable/mask register exists (software
  `Mask|ErrMask` AND). Interrupt lines: SPI 183 (`ISP_Irq_CAM`), 206 (`ISP_Irq_CAMSV`),
  207 (`ISP_Irq_CAMSV2`), all level-low; 184/185 are never requested by the vendor build.
- Frame completion is exclusively `wait_event_interruptible_timeout` woken from the ISR
  (`irq_publish` + `wake_up_interruptible_all`); there is no timer, poll loop, loopback or
  synthetic frame anywhere in the driver (verified by inspection: 0 `complete()`, 0 completion
  sleeps).
- Userspace `libcamdrv` completed to the vendor IspDrv API, `camdrv_cq.c` rewired to real CQ
  encodings recovered from `libcamdrv.so`: 0x7300/0x7320/0x74d4/0x74f4 (IMGO/RRZO/IMGO_D/RRZO_D
  base-address writes), 0x40b4/0x40cc (self-chaining descriptor), 0xfc000000 (ring end),
  0x4184 (table end), operand-PA layout at 0xcd5c/0xcdbc/0xcdee, ring kick at 0xd4aa-0xd520,
  VF enable 0x4414=1 at 0xcc32; `mIspCQModuleInfo` (134 × 12-byte records at `.data 0x2d514`).
  `camservice` provides the real flow: buffer allocation/PFN mapping → queue → CQ program →
  start → IRQ wait → timestamped dequeue.
- The uapi header and `tests/abi32.c` are untouched; every ioctl number and structure size is
  byte-identical (abi32 passes).

Blocked / not fabricated (`logs/rem-camera-isp.md` §6):

1. Pass-1/Pass-2/DSP **module and TPIPE tuning tables** exist only in the closed HAL
   (`libcam.iopipe.so`, `lib3a.so`, `libcam.hal3a.v3.so`) and in sensor tuning blobs that are not
   in the extracted image. `camservice --capture` programs the proven parts and, with no program,
   reports an interrupt timeout instead of a fake frame.
2. `ISP_SET_USER_PID` is compiled out downstream and `ISP_REGISTER_IRQ`/`ISP_UNREGISTER_IRQ` have
   no case in the ROM ioctl switch (they fall to `-EPERM`); both return documented errors.
3. Per-port DMA error counters (`g_DmaErr_p1[]`) are inside a disabled downstream `#ifdef`; the
   aggregate error is exposed via `ISP_GET_INT_ERR`.
4. `ISP_IRQ_TYPE_INT_P2_ST` is never sampled by the ROM ISR; Pass-2 done is delivered through
   `ISP_IRQ_TYPE_INT_STATUSX` exactly as the ROM does.
5. `camdrv_mem.c` needs `CAP_SYS_ADMIN` for pagemap PFNs; without it the tool fails with `-EPERM`
   rather than inventing addresses.

### 5.3 OTP / lens-shading / 3A (`logs/rem-camera-otp.md`)

New `tools/mediatek/camera/otp/` (read-only library + CLI + selftest) plus a read-only `otp`
sysfs attribute on `s5k5e3yx.c`:

- IMX219 MTK OTP: 0x3bb-byte image assembled exactly like
  `imx219_eeprom.c read_imx219_eeprom_mtk_fmt()` (`:192-225`, `:365-387`), fields at
  0x001/4, 0x005/2, 0x007, 0x008, 0x009/4 + 0x00d/4, 0x011/2 + 0x013/2, 0x015/2, 0x017..,
  module id 0x050/10, mfr info 0x020/11, calibration version `0xff000b01`.
- S5K5E3YX OTP: page-select + 5-byte burst sequence ported from
  `s5k5e3yxmipiraw_Sensor.c:392-414` (0x0a00=0x04, 0x0a02=page, 0x0a00=0x01, 1 ms,
  read 0x0a04..0x0a08, 0x0a00=0x04/0x00), real CCI regmap burst, `-EBUSY` while streaming,
  mutex shared with stream start/stop.
- Lens shading: the OTP→ISP gain-table conversion is ported instruction-for-instruction from
  `libcameracustom.so` (`vLSC_AvgRaw_Load 0xf4151`, pedestal/clamp `0xf40a6-0xf4134`, per-channel
  max `0xf42c0`, distance `isqrt` `0xf4306`, polynomial `0xf3e66`, packing `0xf4462-0xf4542`),
  with `ShadingATNTable` (0x50af18, 504 B) read from the shipped library at run time, never
  compiled in. `DoISPDynamicShadingLoad`/`DoISPFixShadingLoad` returning 0 and
  `DoISPSensorShadingLoad` returning 0x1000 were re-verified by disassembly
  (`0xf6254/0xf626c/0xf6284`).
- 3A: seven static tables are exposed with symbol/address/size/decode, all read from the vendor
  ELFs with the ELF symbol size re-checked on every access: `ShadingATNTable` 504 B,
  `AwbAlgo::m_i4AWBLogLUT1E` 1024 B, `…LUT2E` 8192 B, `LumLog2x1000` 2052 B, `g_i4PLweight`
  128 B, `GmaCore::m_i4DefGmaBase0E`/`m_i4GmaSectionE` 580 B each.

Blocked, with the exact missing asset:

1. NVRAM shading coefficient sets (`channel_lsc_data*`, `g_lsc_cali_info`, `g_lsc_param`,
   `g_lsc_result`, `g_rLSCCaliINIParam`) are `.bss` in the library and are only filled from the
   device NVRAM image, which is absent from `zip/system.raw.img` (verified: `/data` has only
   `misc/`, `/cameradata` only XML + YUV, no camera tuning files under `/etc`).
2. NVRAM default structs (ISP 0x28000, 3A 0x7c04, shading 0x10081c, AE pline 0x512b4) are produced
   by generator code, not stored as blobs.
3. 3A runtime singletons (`AwbAlgoDev`, `AeAlgoDev`, `AfAlgoDev`, `PdAlgoDev`, managers) are
   `.bss` and exist only after runtime initialisation.

### 5.4 IMGSYS / SMI / clocks / interrupts (`logs/rem-camera-imgsys.md`)

- P1/P2 soft reset in `CAM_SW_CTL` (+0x8c) and CAMSV/CAMSV2 reset at `ISP+0x5010/+0x5810`,
  byte-for-byte the ROM sequences (`0xc04e7614-0xc04e7650`, `0xc04d3124-0xc04d3160`,
  `0xc04e7a64-0xc04e7ae8`, `0xc04e840c-0xc04e8490`). **Real defect fixed:** the baseline polled
  `SW_RST_STATUS` with a 10 ms/10 s timeout, but the downstream completion loop is dead code
  (`(!Reg) & BIT(1)` is always 0) and the ROM performs exactly one read; polling could fail probe
  on silicon. Resets moved from `runtime_resume` (a debugfs read there would reset an active
  stream) to probe-once.
- CAMSV bank mapped at `isp_phys + 0x5000` (0x1000) with `devm_ioremap()` because the vendor
  describes a single 0x9000 ISP window; SENINF now claims only 0x1000, so the overlap is gone in
  the recommended DTS.
- SMI: the 0xfc0 MMU bitmap writer is unchanged; `mtk_smi_larb_mt6735` adds the LARB2
  `CMD_THRT_CON` (0x24) bit-27 clear and the 21-port OSTDL table (0x200+4i = 1). Proof: ROM
  `bic r2,r2,#0x08000000 @0xc04ca9a4` + `str r2,[r3,#0x24] @0xc04ca9ac`; OSTDL table at ROM
  virtual 0xc0e849b4 applied by the word loop at `0xc04ca85c-0xc04ca890`; downstream
  `smi_configuration.c:127-134,1276`.
- Clocks: six-gate set, parents and set/clr/status offsets verified against `clk-mt6735.c:885-899`
  and `camera_isp_D1.c:3062-3100`; nothing to change.
- Definitive interrupt map (5 GIC lines, 9 status registers, bits, DONE-before-SOF ordering,
  read-to-clear model) delivered as a table plus DTS fragment. No `request_irq` in this slice.

Reported, not implemented (undocumented): `OSTDL_SOFT_EN` (0x64), SMI common L1 arbitration
0x104-0x114 (downstream loop is out-of-bounds over a 3-entry array), DSP reset (does not exist for
D1: zero `dsp` matches in `camera_isp_D1.c`, no ISP_DSP symbol in the ROM), CAM1/CAM2 IRQs
(184/185, never requested by the vendor kernel).

## 6. Cross-subsystem contracts

- Register windows: `mtk-imgsys` ioremaps its two windows with `devm_ioremap_resource`; the
  char driver and the CAMSV bank use non-reserving `devm_ioremap`, so the deliberate overlap on
  `0x15004000+0x9000` cannot make either driver fail. SENINF uses 0x15008000+0x1000,
  0x1500c000+0x1000, 0x10215000+0x3000.
- Power/clocks: the char driver takes its own runtime-PM reference and its own references on the
  six IMGSYS gates; the platform driver holds the ISP genpd and the same gates at probe. CCF and
  genpd refcounting make this safe.
- Resets: `mtk-imgsys` runs all four vendor resets once at probe; the char driver runs the
  same sequences for the `ISP_RESET_*` ioctls under its register spinlock. No exported helper, no
  shared mutable state.
- Media topology is unchanged: `mtk-seninf` owns the only media device; `mtk-imgsys` and
  `mtk-isp` register no V4L2 entity.
- The uapi (`include/uapi/linux/mtk_isp.h`) is byte-identical to the baseline; nothing in this
  series adds or renumbers an ioctl.

## 7. Consolidated DTS fragment (text only; no DTS file modified)

SoC level (`mt6735.dtsi`), superseding the equivalent fragments in the earlier camera report:

```dts
/* IMGSYS platform: unchanged from the camera subsystem fragment except that it now
 * performs all four resets at probe. */
ispsys: isp@15004000 {
	compatible = "mediatek,mt6735-ispsys";
	reg = <0 0x15004000 0 0x4000>, <0 0x1500d000 0 0x1000>;
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;	/* not requested by this driver */
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MTK6735_POWER_DOMAIN_ISP>;
	status = "disabled";
};

/* SENINF: first window shrinks to 0x1000 (max offset 0xd20); rest unchanged. */
seninf: seninf@15008000 {
	compatible = "mediatek,mt6735-seninf";
	reg = <0 0x15008000 0 0x1000>,
	      <0 0x1500c000 0 0x1000>,
	      <0 0x10215000 0 0x3000>;
	clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
	clock-names = "camtg";
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
		port@0 { reg = <0>; seninf_in0: endpoint { remote-endpoint = <&imx219_out>; }; };
		port@1 { reg = <1>; seninf_in1: endpoint { remote-endpoint = <&s5k5e3yx_out>; }; };
	};
};

/* camera-isp character device.  Overlaps the two nodes above on purpose; both drivers
 * map the windows without reserving them, and clocks/power domains are refcounted. */
camera_isp: camera-isp@15004000 {
	compatible = "mediatek,mt6735-camera-isp";
	reg = <0 0x15004000 0 0x9000>,	/* ISP core + CAMSV/CAMSV2 banks */
	      <0 0x1500d000 0 0x1000>,	/* inner ISP / TPIPE */
	      <0 0x15000000 0 0x10000>,	/* CAMINF */
	      <0 0x10215000 0 0x3000>,	/* MIPI RX analog (also SENINF's) */
	      <0 0x10211000 0 0x1000>;	/* GPIO */
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>,	/* ISP_Irq_CAM    */
		     <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,	/* unused         */
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>,	/* unused         */
		     <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,	/* ISP_Irq_CAMSV  */
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;	/* ISP_Irq_CAMSV2 */
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MTK6735_POWER_DOMAIN_ISP>;
	status = "disabled";	/* board DTS enables it */
};
```

Board level: the IMX219 (0x1a), S5K5E3YX (0x10), DW9807 (0x0c) and EEPROM (0x58) nodes, the
`camera_pins_default` state and the endpoint labels are unchanged from
`logs/camera-coordinator.md` §8; the S5K5E3YX OTP attribute needs no DT property.
`mtk6735.dtsi` has no `soc` label, so a board overlay must use `&{/soc}`.

## 8. Independent verification performed by the coordinator

Beyond applying and compiling, these leaf claims were checked directly against the binaries:

- Baseline patch applies cleanly to a fresh HEAD worktree; all four leaf patches and the combined
  patch apply on the baseline (temp-index `git apply --cached --check`), sequentially and
  individually.
- D-PHY: `mov.w r5,#0x12c @0x1647e`, `mul/udiv` at `0x164d2/0x164da`, `moveq r7,#0x55 @0x164cc`,
  `lsls/subs` lane mask at `0x165ce/0x165d4`, latch at `0x165d8`; window derivation
  (`add.w #0x800 @0x1827e`, `+0x400 @0x1834a`, `+0x800 @0x18350`).
- IMGSYS: `bic r2,r2,#0x08000000 @0xc04ca9a4`, `str r2,[r3,#0x24] @0xc04ca9ac`.
- ISP: ROM ISR status reads at `0xc04ddda4` (0x4c), `0xc04dddd4` (0x54), `0xc04dddec` (0x5c) and
  no write-back; IRQ wakeups are `wait_event_interruptible_timeout`/`wake_up_interruptible_all`.
- OTP/3A: dynamic symbol sizes match the registry exactly (`ShadingATNTable` 504,
  `LumLog2x1000` 2052, `AWBLogLUT2E` 8192, `g_i4PLweight` 128, both GMA tables 580); vendor shading
  entry points at `0xf6254/0xf626c/0xf6284` return 0/0/0x1000; downstream OTP sequences at
  `imx219_eeprom.c:192-225` and `s5k5e3yxmipiraw_Sensor.c:392-414` match the implemented reads.
- Stub/dead-code scan of the integrated tree: no `TODO/FIXME/XXX/placeholder`, no simulated
  completion, no synthetic table, no invented register value; `grep -c 'warning:|error:'` on the
  integration build log = 0; no DTS/DTSI in any patch.

## 9. Hardware-unverified items (no board access)

- SENINF: reset/default state of the block before `init()`, and whether the 500 µs calibration
  handshake completes on silicon (vendor treats failure as non-fatal; the driver warns).
  `MIPI_OPHY_CSI2` group (`0x360/0x364/0x368`) is recovered but not programmed because both
  sensors select `MIPI_OPHY_NCSI2`.
- ISP: FBC drop decision (`FB_NUM == FBC_CNT` at SOF), MCLK-disable writes, and the ISP's
  interpretation of `0xfc000000` as end-of-ring (inferred from its position in the vendor ring).
- IMGSYS: CAMSV reset effect through an unreserved mapping; the exact meaning of the LARB2 0x24
  bit-27 clear (position proven, name absent everywhere).
- Shading: the LSC conversion was verified against the vendor instruction stream, not against a
  captured frame; the NVRAM-only coefficients remain unavailable by design.
- The whole series is compile-tested only; no camera capture has been run on the device.

## 10. Reproduction

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
export TMPDIR=$M/tmp          # /tmp is a full tmpfs on this host
cd $M/worktrees/rem-camera-integration     # or a fresh worktree from $M/src/nura-linux HEAD
git apply $M/patches/parity-baseline-all.patch
git apply $M/patches/rem-camera-dphy.patch
git apply $M/patches/rem-camera-imgsys.patch
git apply $M/patches/rem-camera-otp.patch
git apply $M/patches/rem-camera-isp.patch
# or, as one patch: git apply $M/patches/rem-camera-all.patch   (same tree)
# then the config + make commands in section 4
```

Per-leaf patches remain the reviewable units; `rem-camera-all.patch` is the exact content of
`worktrees/rem-camera-integration`.

## 11. File inventory

Patches (`$M/patches/`): `rem-camera-dphy.patch`, `rem-camera-isp.patch`,
`rem-camera-otp.patch`, `rem-camera-imgsys.patch`, `rem-camera-all.patch`.

Reports (`$M/logs/`): `rem-camera-dphy.md`, `rem-camera-isp.md`, `rem-camera-otp.md`,
`rem-camera-imgsys.md`, this file.

Build/test logs (`$M/logs/`): `rem-camera-dphy-build.log`, `rem-camera-isp-build.log`,
`rem-camera-otp-build.log`, `rem-camera-otp-olddefconfig.log`, `rem-camera-imgsys-build.log`,
`rem-camera-baseline-build.log`, `rem-camera-baseline-olddefconfig.log`,
`rem-camera-integration-build.log`, `rem-camera-integration-userspace.log`,
`rem-camera-integration-check.log`, `rem-camera-integration-tests.log`,
`rem-camera-dphy-extract.log`.

Worktrees (`$M/worktrees/`): `rem-camera-base` (verified baseline), `rem-camera-dphy`,
`rem-camera-isp`, `rem-camera-otp`, `rem-camera-imgsys`, `rem-camera-integration`.
