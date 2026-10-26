# MT6735/MT6737T IMGSYS/ISP platform driver (camera-imgsys)

## 1. Driver and files changed

New driver directory: `drivers/media/platform/mediatek/imgsys/`

| File | Purpose |
|---|---|
| `drivers/media/platform/mediatek/imgsys/Kconfig` | `VIDEO_MTK_IMGSYS` (tristate), module name `mtk-imgsys` |
| `drivers/media/platform/mediatek/imgsys/Makefile` | `obj-$(CONFIG_VIDEO_MTK_IMGSYS) += mtk-imgsys.o` |
| `drivers/media/platform/mediatek/imgsys/mtk-imgsys.c` | platform driver: 2 register windows, 6 clock gates, ISP genpd, P1 soft reset, debugfs register dump, runtime PM |
| `drivers/media/platform/mediatek/imgsys/mtk-imgsys-regs.h` | documented register offsets / P1 reset field masks only |

Parent-file edits (intended for coordinator conflict merge):
- `drivers/media/platform/mediatek/Kconfig`: `source "drivers/media/platform/mediatek/imgsys/Kconfig"`
- `drivers/media/platform/mediatek/Makefile`: `obj-y += imgsys/`

No other file was touched. No DTS/DTSI file was modified.

Interface contract implemented:
- OF compatible `mediatek,mt6735-ispsys`.
- `reg` entry 0 = ISP core `0x15004000`, size `0x4000`; `reg` entry 1 = inner ISP `0x1500d000`, size `0x1000`.
- `clocks`/`clock-names`: the six IMGSYS gates, in downstream enable order.
- `power-domains = <&spm MT6735_POWER_DOMAIN_ISP>`.
- `debugfs` `/sys/kernel/debug/mtk-imgsys/regs` (read-only, runtime-resumes the block for the duration of the dump).
- No V4L2/media entity, no video node, no async notifier, no interrupt handler (see parity matrix).

## 2. ROM/downstream evidence

### Device tree
- ROM: `$M/zip/rom_dtb_0.dts:3705-3711` — `ispsys@15000000`:
  `compatible = "mediatek,mt6735-ispsys"`; reg entries
  `<0x15004000 0x9000>, <0x1500d000 0x1000>, <0x15000000 0x10000>,
   <0x10215000 0x3000>, <0x10211000 0x1000>`; interrupts
  `<0 0xb7 8>, <0 0xb8 8>, <0 0xb9 8>, <0 0xce 8>, <0 0xcf 8>`
  (= GIC SPI 183, 184, 185, 206, 207, level-low); 9 clocks with names
  `CG_SCP_SYS_DIS, CG_SCP_SYS_ISP, CG_DISP0_SMI_COMMON, CG_IMAGE_CAM_SMI,
   CG_IMAGE_CAM_CAM, CG_IMAGE_SEN_TG, CG_IMAGE_SEN_CAM, CG_IMAGE_CAM_SV,
   CG_IMAGE_LARB2_SMI`.
- Downstream DT: `$M/src/downstream-grandpplte/arch/arm/boot/dts/mt6735.dtsi:1265-1298`
  (same node, same 5 reg entries, same 5 interrupts, same 9 clock names;
  interrupts at `:1273-1277`, clocks/clock-names at `:1279-1297`); imgsys
  clock/syscon node at `:1259-1263`;
  `kd_camera_hw1` (SENINF `0x15008000`) at `:1307`.

### Downstream ISP driver
`$M/src/downstream-grandpplte/drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c`
- `:171-176` base-address resource enum: index 0 = ISP base, 1 = inner ISP,
  2 = imgsys config, 3 = MIPI analog, 4 = GPIO.
- `:216-218` `ISP_ADDR = gISPSYS_Reg[ISP_BASE_ADDR]`,
  `ISP_ADDR_CAMINF = gISPSYS_Reg[ISP_IMGSYS_CONFIG_BASE_ADDR]`.
- `:122-125` `IMGSYS_BASE_ADDR 0x15000000`, `ISP_REG_RANGE 0x10000`,
  `SENINF_BASE_ADDR 0x15008000`; `:129` `MIPIRX_CONFIG_ADDR 0x1500C000`;
  `:131` `MIPIRX_ANALOG_ADDR 0x10215000`; `:133` `GPIO_BASE_ADDR 0x10211000`.
- `:235-244` register offsets: `EN1 +0x4`, `INT_P1_ST +0x4C`, `INT_P1_ST2 +0x54`,
  `INT_P1_ST_D +0x5C`, `INT_P1_ST2_D +0x64`, `INT_P2_ST +0x6C`,
  `INT_STATUSX +0x70`, `INT_STATUS2X +0x74`, `INT_STATUS3X +0x78`,
  `CAM_SW_CTL +0x8C`. All relative to `ISP_ADDR` = `0x15004000`.
- `:370-377` `CAM_SW_CTL` P1 field: `SW_RST_P1_MASK 0x7`, `SW_RST_TRIG 0x1`,
  `SW_RST_STATUS 0x2`, `HW_RST 0x4`; P2 field `0x70/0x10/0x20/0x40`.
- `:3263-3300` `ISP_Reset(ISP_REG_SW_CTL_RST_CAM_P1)`, the enabled `#if 1`
  sequence used by this driver.
- `:3331-3348` CAMSV / CAMSV2 resets use `ISP_ADDR + 0x5010` / `+0x5810`,
  i.e. addresses `0x15009010` / `0x15009810`, outside the `0x4000` ISP window
  this driver maps (see window split below).
- `:3062-3100` `Prepare_Enable_ccf_clock()`: `:3065` "must keep this clk open
  order: CG_SCP_SYS_DIS -> CG_DISP0_SMI_COMMON -> CG_SCP_SYS_ISP -> ISP clk",
  then CAM_SMI, CAM_CAM, SEN_TG, SEN_CAM, CAM_SV, LARB2_SMI.
- `:3172-3183` `Disable_Unprepare_ccf_clock()` close order.
- `:519-613` software IRQ mask tables for `INT_STATUSX`/`2X`/`3X`;
  `:11626-11648` initialises `IspInfo.IrqInfo.Mask[]`/`ErrMask[]`;
  `:9073-9121` reads `INT_P1_ST`, `INT_P1_ST2`, `INT_P1_ST_D`, `INT_P1_ST2_D`,
  `INT_STATUSX`, `INT_STATUS2X`, `INT_STATUS3X` and ANDs them with those masks.
- Verified by grep over the whole downstream camera ISP source: there is **no
  write** to any `INT_*`/`STATUS*` register and **no interrupt-enable register**
  in the register map; the "mask" is a purely software filter. See the
  deferred-row rationale below.
- `:11469-11480` `of_iomap(of_node, i)` for the 5 reg entries;
  `:11482-11498` `irq_of_parse_and_map` + `request_irq(IRQF_TRIGGER_NONE)`;
  `:11540-11548` `devm_clk_get()` for the 9 named clocks.

### Nura bindings used
- Clock controller: `drivers/clk/mediatek/clk-mt6735-imgsys.c:41` binds
  `mediatek,mt6735-imgsys` (same node as the `syscon@15000000` in
  `arch/arm64/boot/dts/mediatek/mt6735.dtsi:846-852`). Gates used:
  `include/dt-bindings/clock/mediatek,mt6735-imgsys.h` —
  `CLK_IMG_CAM_SMI 1`, `CLK_IMG_CAM_CAM 2`, `CLK_IMG_SEN_TG 3`,
  `CLK_IMG_SEN_CAM 4`, `CLK_IMG_CAM_SV 5`, `CLK_IMG_SMI_LARB2 0`.
  (The task text says `CLK_IMG_LARB2_SMI`; the binding's actual identifier is
  `CLK_IMG_SMI_LARB2`, same gate bit 0 — see `clk-mt6735-imgsys.c:26`.)
- Power: `include/dt-bindings/power/mt6735-power.h:10`
  `MT6735_POWER_DOMAIN_ISP 4` (the header included by
  `mt6735.dtsi:21`), genpd data in
  `drivers/pmdomain/mediatek/mt6735-pm-domains.h` (`isp`, `SPM_ISP_PWR_CON`).

### Clock-name mapping (downstream 9 -> Nura)
| Downstream clock-name (`mt6735.dtsi:1279-1297`) | Nura representation | Used by this driver |
|---|---|---|
| `CG_SCP_SYS_DIS` | genpd `MT6735_POWER_DOMAIN_DIS` (separate SPM domain, `mt6735-pm-domains.h`) | no (not a clock; DIS domain is not a parent of ISP in nura's SPM data) |
| `CG_SCP_SYS_ISP` | genpd `MT6735_POWER_DOMAIN_ISP` via `power-domains` | yes, implicitly (genpd resumes before `runtime_resume`) |
| `CG_DISP0_SMI_COMMON` | mmsys gate `CLK_MM_SMI_COMMON` (`mediatek,mt6735-mmsys.h:6`) | no — SMI/DMA path is deferred with the ISP pipe |
| `CG_IMAGE_CAM_SMI` | `<&imgsys CLK_IMG_CAM_SMI>` | yes |
| `CG_IMAGE_CAM_CAM` | `<&imgsys CLK_IMG_CAM_CAM>` | yes |
| `CG_IMAGE_SEN_TG` | `<&imgsys CLK_IMG_SEN_TG>` | yes |
| `CG_IMAGE_SEN_CAM` | `<&imgsys CLK_IMG_SEN_CAM>` | yes |
| `CG_IMAGE_CAM_SV` | `<&imgsys CLK_IMG_CAM_SV>` | yes |
| `CG_IMAGE_LARB2_SMI` | `<&imgsys CLK_IMG_SMI_LARB2>` | yes |

### Register window split
Downstream maps one `0x9000` blob at `0x15004000` (`mt6735.dtsi:1267`), which
overlaps other blocks:

```
0x15004000 - 0x15007fff   ISP core (Pass1/Pass2 register file)     -> this driver
0x15008000 - 0x1500bfff   SENINF (kd_camera_hw1 reg, mt6735.dtsi:1308) -> mtk-seninf driver
0x1500c000 - 0x1500cfff   MIPIRX config (camera_isp_D1.c:129)      -> mtk-seninf driver
0x1500d000 - 0x1500dfff   inner ISP (ISP_INNER_BASE_ADDR)           -> this driver
0x15000000 - 0x15000fff   imgsys config/syscon + clock gates        -> clk-mt6735-imgsys (syscon)
```

Consequences encoded in this driver:
- `reg` is only `0x15004000+0x4000` and `0x1500d000+0x1000`;
  the `0x15008000`/`0x1500c000` windows are not requested.
- CAMSV registers (`ISP_ADDR+0x5000..0x5C4C` = `0x15009000..0x15009C4C`) land
  inside the SENINF window and are not touched.
- The imgsys config block is *not* accessed at all. It is only needed for ISP
  pipe programming (`ISP_INNER_REG_ADDR_*` at `camera_isp_D1.c:323-355`), which
  is deferred, and the node is already bound by the clock driver; if it ever
  becomes necessary, `syscon_regmap_lookup_by_compatible("mediatek,mt6735-imgsys")`
  must be used (the syscon node is `0x1000` long, so only the gate registers
  are reachable through it).

## 3. Parity matrix

| # | Behavior | ROM/downstream implementation | Nura before | After | Status |
|---|---|---|---|---|---|
| 1 | Probe + map ISP core and inner ISP windows | `camera_isp_D1.c:11469-11480` `of_iomap(of_node, 0..4)`; windows `0x15004000+0x9000`, `0x1500d000+0x1000` (`mt6735.dtsi:1267-1268`) | nothing | `devm_ioremap_resource()` of both windows, phys addresses kept for debugfs | done |
| 2 | ISP power domain | `CG_SCP_SYS_ISP` clock of scpsys (`mt6735.dtsi:1278`) | ISP genpd exists but unused | `power-domains = <&spm MT6735_POWER_DOMAIN_ISP>`; genpd on at runtime resume, off at runtime suspend | done |
| 3 | Six IMGSYS clock gates, downstream enable order | `camera_isp_D1.c:3062-3100` | gate driver exists, no consumer | `devm_clk_get()` by downstream names + `clk_prepare_enable()` in the documented order; reverse order disable | done |
| 4 | `CG_SCP_SYS_DIS` / `CG_SCP_SYS_ISP` mapping | SPM domains, `camera_isp_D1.c:3065-3078` | domains exist in scpsys | ISP domain via power-domains; DIS domain is owned by its own consumers (not a clock in nura) | done |
| 5 | `CG_DISP0_SMI_COMMON` (mmsys SMI common) | `camera_isp_D1.c:3065-3073` | mmsys gate exists | not consumed: only needed for ISP DMA (deferred); enabling it would couple imgsys probe to the mmsys clock provider | deferred |
| 6 | Pass 1 ISP soft reset, every probe and resume | `camera_isp_D1.c:3263-3300` (`CAM_SW_CTL` +0x8C, mask 0x7, trig 0x1, status 0x2, hw reset 0x4) | nothing | `mtk_imgsys_soft_reset()`: read/modify/write trig, bounded poll of status bit, `HW_RST`, restore `reg & ~mask` | done |
| 7 | Pass 2 soft reset | `camera_isp_D1.c:3303-3330` | nothing | not implemented on purpose: it is P2 pipeline state, requires the ISP pipe | deferred |
| 8 | CAMSV / CAMSV2 soft reset | `camera_isp_D1.c:3331-3348` (`ISP_ADDR+0x5010/+0x5810`) | nothing | registers are outside the `0x4000` ISP window (inside the SENINF window); no window is claimed for them | deferred |
| 9 | ISP (CAM0/CAM1/CAM2) interrupt handling | `camera_isp_D1.c:9053-9180` reads `INT_STATUS*` and ANDs software masks; no enable/clear register exists anywhere in the source (`:235-283`, `:11626-11648`) | nothing | IRQs deliberately not requested: with no documented source-enable and no documented write-to-clear, a level-low IRQ could never be acknowledged (storm). Status offsets are documented in the report for the future pipe work | deferred |
| 10 | CAMSV0/CAMSV1 interrupt handling | `camera_isp_D1.c:8680-8860`, `:8863-...` | nothing | same rationale as #9; CAMSV registers also lie in the SENINF window | deferred |
| 11 | ISP Pass1/Pass2/DSP programming, CQ command queues, DMA/RT buffers, frame completion | the bulk of `camera_isp_D1.c` (TPIPE, `ISP_TPIPE_ADDR` `:367`, IOMMU/m4u, cmdq) | nothing | not implemented: requires the proprietary MTK camera userspace ABI (libcamdrv) and ISP firmware; no register programming is guessed | deferred |
| 12 | V4L2/media entity, video node, async notifier | downstream is a char device (`ISP_RegCharDev`, `camera_isp_D1.c:11512`) | nothing | intentionally not registered (architecture decision #3; single media_device is owned by mtk-seninf) | deferred (by design) |
| 13 | Runtime/system suspend-resume of clocks + genpd | downstream `ISP_EnableClock`, `:3190-3250` | nothing | `RUNTIME_PM_OPS` + `pm_runtime_force_suspend/resume` for system sleep; devm-managed enable; no manual refcounts | done |
| 14 | Register visibility for diagnostics | downstream `ISP_DumpReg()` (procfs/debug) | nothing | `/sys/kernel/debug/mtk-imgsys/regs` dumps both windows (5120 32-bit registers), read-only, runtime-resumed for the dump | done |
| 15 | imgsys config (CAMINF) block access | `camera_isp_D1.c:216-218`, `:323-355` | clock driver owns syscon node | not accessed; would need `syscon_regmap_lookup_by_compatible()`; only used by deferred pipe code | deferred |

Counts: **done 7** (#1,2,3,4,6,13,14), **partial 0**, **deferred 8** (#5,7,8,9,10,11,12,15).

### Deferred ISP IRQ rationale (detail)
`camera_isp_D1.c:235-244` documents only status registers
(`INT_P1_ST`, `INT_P1_ST2`, `INT_P1_ST_D`, `INT_P1_ST2_D`, `INT_P2_ST`,
`INT_STATUSX`, `INT_STATUS2X`, `INT_STATUS3X`) plus `EN1` (module enable at
`+0x4`, not an interrupt enable). The handler at `:9053-9121` only *reads*
those registers; a grep over the whole downstream ISP source shows no write to
any of them. `IspInfo.IrqInfo.Mask[]`/`ErrMask[]` (`:11626-11648`) are used to
filter the read values in software. There is therefore no derivable way to
enable or acknowledge these sources, so the driver registers no IRQ at all
rather than enabling a level-low line that can never be cleared.

## 4. Compile-tested

```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-imgsys"
cp "$M/nura.config" .config
./scripts/config -e MEDIA_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV \
  -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC -e I2C \
  -e REGULATOR -e COMMON_CLK -e VIDEO_CAMERA_SENSOR -e DEBUG_FS \
  -e MEDIA_PLATFORM_SUPPORT -e MEDIA_PLATFORM_DRIVERS -e V4L_PLATFORM_DRIVERS \
  -e VIDEO_MTK_IMGSYS
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/media/platform/mediatek/imgsys/mtk-imgsys.o
make ARCH=arm64 LLVM=1 -j2 drivers/media/platform/mediatek/imgsys/
```

Results: `olddefconfig` rc=0 (`CONFIG_VIDEO_MTK_IMGSYS=y`,
`CONFIG_PM=y`, `CONFIG_DEBUG_FS=y`); `mtk-imgsys.o` compiled rc=0 with **no
warnings** after fixing two `%x`/`size_t` format warnings found on the first
build; the directory target archived `built-in.a` rc=0. Build log:
`$M/logs/camera-imgsys-build.log`, config log
`$M/logs/camera-imgsys-olddefconfig.log`.

Note: `MEDIA_PLATFORM_SUPPORT`/`MEDIA_PLATFORM_DRIVERS`/`V4L_PLATFORM_DRIVERS`
and `DEBUG_FS` had to be enabled in addition, because `nura.config` ships with
`# CONFIG_MEDIA_SUPPORT is not set` and with `MEDIA_SUPPORT_FILTER=y`; the
`mediatek` platform Kconfig is only sourced under `MEDIA_PLATFORM_DRIVERS`.

## 5. Inferred / hardware-unverified items

- **Reset status polarity/timing** — the downstream completion loop
  `do { Reg = read; } while ((!Reg) & SW_RST_STATUS);`
  (`camera_isp_D1.c:3285-3288`) always terminates after one read (`&` instead
  of `&&`, plus the inverted test), so it documents neither polarity nor
  timing. This driver waits for `SW_RST_STATUS` (bit 1, `:372`) to read 1,
  with a 10 us poll and 10 ms timeout. [INFERENCE] The bit name and the
  `TRIG -> STATUS -> HW_RST` ordering come from the downstream sequence; the
  "wait until set" interpretation and the timeouts are not proven by any
  source.
- **Register restore value after reset** — step 4 of the sequence writes
  `val & ~P1_MASK` where `val` is the *last polled* read of `CAM_SW_CTL`
  (mirroring downstream, which reuses its loop variable `Reg`). Downstream
  never verifies the resulting value.
- **Whether the mmsys SMI-common gate is needed** for `CAM_SW_CTL` writes /
  reads. Downstream enables it first; this driver does not, because no DMA is
  performed. Unverified on hardware; if register access hangs without it, the
  clock has to be added to the DTS fragment and the driver, not worked around.
- **genpd ordering** — assumes the ISP power domain is switched on by genpd
  before the driver's `runtime_resume` callback and off after
  `runtime_suspend`, which is the documented `__rpm_callback` ordering in
  `drivers/base/power/runtime.c`. Not exercised on hardware.
- **Inner ISP window contents** — `0x1500d000` is documented only as
  `ISP_INNER_BASE_ADDR` and as a mirror of `0x15004000`
  (`camera_isp_D1.c:319-320` comment). The dump is a raw read of the window;
  no field interpretation is claimed.
- **DTS fragment** — not compiled or booted; the nura DTSI was not modified
  (per instructions). Interrupt numbers in the fragment are retained from the
  ROM/downstream nodes but are intentionally not consumed by the driver.
- **`depends on PM`** — added to Kconfig beyond the four requested
  dependencies because the driver's power/reset contract is implemented
  through runtime PM; without `CONFIG_PM` the probe would leave the block
  unpowered.

## DTS-FRAGMENT

Insert into `arch/arm64/boot/dts/mediatek/mt6735.dtsi`, directly after the
`imgsys: syscon@15000000` node and before `smi_larb2`. The node is
`status = "disabled"`; the grandpplte board DTS must set `status = "okay"`
(and it must not overlap the seninf node at `0x15008000`).

```dts
		ispsys: isp@15004000 {
			compatible = "mediatek,mt6735-ispsys";
			reg = <0 0x15004000 0 0x4000>,	/* ISP core */
			      <0 0x1500d000 0 0x1000>;	/* inner ISP */

			interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>,
				     <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
				     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>,
				     <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,
				     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;

			clocks = <&imgsys CLK_IMG_CAM_SMI>,
				 <&imgsys CLK_IMG_CAM_CAM>,
				 <&imgsys CLK_IMG_SEN_TG>,
				 <&imgsys CLK_IMG_SEN_CAM>,
				 <&imgsys CLK_IMG_CAM_SV>,
				 <&imgsys CLK_IMG_SMI_LARB2>;
			clock-names = "CG_IMAGE_CAM_SMI",
				      "CG_IMAGE_CAM_CAM",
				      "CG_IMAGE_SEN_TG",
				      "CG_IMAGE_SEN_CAM",
				      "CG_IMAGE_CAM_SV",
				      "CG_IMAGE_LARB2_SMI";

			power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;

			status = "disabled";
		};
```

The DTSI already includes both bindings used above
(`mt6735.dtsi:11` imgsys clocks, `mt6735.dtsi:21` power domains); no include
change is needed.

## 7. Patch path

`$M/patches/camera-imgsys.patch` (6 files, +367 lines; no `.config`, no build
artifacts, no unrelated files).
