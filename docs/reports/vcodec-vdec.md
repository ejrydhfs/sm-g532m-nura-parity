# vcodec-vdec — MT6735 VDEC engine driver

## Worktree

`$M/worktrees/vcodec-vdec` (detached HEAD of `nura-linux`, Linux 7.0.10-mt6735, arm64).
$M = /run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work

## Files changed

| File | Change |
|---|---|
| `drivers/misc/mediatek/vcodec/vcodec-vdec.c` | **new**, 340 lines — VDEC platform driver + `struct vcodec_engine` provider |

Not touched: `vcodec.h`, `Kconfig`, `Makefile`, `vcodec-core.c`, `vcodec-venc.c`, any DTS, `.config`.
The file defines no `module_init`/`module_exit`/`MODULE_LICENSE`/`module_platform_driver()`; it only
exposes `vcodec_vdec_register()` / `vcodec_vdec_unregister()` (declared in `vcodec.h`), which the
shared core calls.

## Downstream / ROM evidence

Downstream reference: `src/downstream-grandpplte/drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c`
(Android 6.0.1, kernel 3.18.19, SM-G532M).

| Fact | Evidence |
|---|---|
| VDEC physical base/region | D1:337-338 `VDEC_BASE_PHY 0x16000000`, `VDEC_REGION 0x29000`; D1:369-370 `VDEC_MISC_BASE = VDEC_BASE + 0`, `VDEC_VLD_BASE = VDEC_BASE + 0x1000` |
| VDEC node lookup, MISC/VLD split | D1:2429-2437: `of_find_compatible_node(..., "mediatek,mt6735-vdec")`, `of_iomap(node, 0)`, `KVA_VDEC_MISC_BASE = KVA_VDEC_BASE + 0x0000`, `KVA_VDEC_VLD_BASE = KVA_VDEC_BASE + 0x1000` |
| Separate GCON window | D1:2439-2443: second node `"mediatek,mt6735-vdec_gcon"`, `of_iomap(node, 0)` |
| Clock enable order | D1:395-397 (CLKMGR: `MT_CG_DISP0_SMI_COMMON`, `MT_CG_VDEC0_VDEC`, `MT_CG_VDEC1_LARB`) and D1:402-431 (`SCP_SYS_DIS`, `MT_CG_DISP0_SMI_COMMON`, `SCP_SYS_VDE`, `MT_CG_VDEC0_VDEC`, `MT_CG_VDEC1_LARB`) |
| Clock disable order (reverse) | D1:451-455 |
| Power gating (both DIS and VDE domains) | D1:402 `clk_prepare_enable(clk_MT_SCP_SYS_DIS)`, D1:415 `clk_prepare_enable(clk_MT_SCP_SYS_VDE)`; disabled at D1:453-455 |
| IRQ request + initial mask | D1:2272 `request_irq(VDEC_IRQ_ID, ..., IRQF_TRIGGER_LOW, ...)`, D1:2286 `disable_irq(VDEC_IRQ_ID)` |
| IRQ unmask/mask from the HW-lock path | D1:855 `enable_irq(VDEC_IRQ_ID)`, D1:848 and D1:1089 `disable_irq(VDEC_IRQ_ID)` |
| IRQ is level/low | D1:2378-2380 `mt_irq_set_sens(VDEC_IRQ_ID, MT_LEVEL_SENSITIVE)`, `mt_irq_set_polarity(VDEC_IRQ_ID, MT_POLARITY_LOW)` |
| ISR sequence | D1:541-594: GCON read, abort if `& 0x10`; `VDEC_BASE + 0xA4` read, abort if `(s & bit16) != 0x10000`; ack `writel(readl(|0x11))`, `writel(readl(& ~0x10))`; wake `DecIsrEvent` |
| Kernel does not program codec data registers | No `writel`/`readl` of codec data registers anywhere in D1; only the ISR status/ack at D1:553,557,582-584 |
| Userspace maps the register windows | D1:2175-2176 `vcodec_mmap()` range check against `VENC_BASE`/`VDEC_BASE_PHY` + region |
| Reset | **no reset handling downstream** (nothing in `vdec_power_on`/`vdec_power_off`, D1:385-459) |
| Per-engine power/ISR counters | D1:388-390/437-441 `gu4VdecPWRCounter`; D1:560-577 `gu4DecISRCount`/`gu4LockDecHWCount` |

ROM confirmation (`$M/zip/symbols.txt`): the shipped kernel contains the same service driver, not a
V4L2/VPU codec — `vcodec_mmap` (16828), `vcodec_lockhw` (16844), `vcodec_waitisr` (16829),
`vcodec_unlocked_ioctl` (16846), `video_intr_dlr` (16848), `vcodec_driver_init` (54270); zero `vpu`
symbols (`grep -ic vpu` = 0). This confirms the fixed architecture: kernel owns clocks/power/IRQ/lock/
mmap only, and userspace programs the codec registers.

## Nura bindings used by the DTS fragment (all present)

- `vdecsys: syscon@16000000` — `arch/arm64/boot/dts/mediatek/mt6735.dtsi:865-871`, has `#clock-cells` **and** `#reset-cells = <1>`.
- `CLK_MM_SMI_COMMON` — `include/dt-bindings/clock/mediatek,mt6735-mmsys.h:6`.
- `CLK_VDEC_VDEC`, `CLK_VDEC_SMI_LARB1` — `include/dt-bindings/clock/mediatek,mt6735-vdecsys.h:6-7`.
- `CLK_TOP_VDEC_SEL` — `include/dt-bindings/clock/mediatek,mt6735-topckgen.h:54`.
- `MT6735_POWER_DOMAIN_DIS`(2)/`MT6735_POWER_DOMAIN_VDE`(5) — `include/dt-bindings/power/mt6735-power.h:8,11`.
- `MT6735_VDEC_RST0_VDEC` — `include/dt-bindings/reset/mediatek,mt6735-vdecsys.h:6`.
- Reset controller provider: `drivers/clk/mediatek/clk-mt6735-vdecsys.c:48-60` (`MTK_RST_SIMPLE`, bank `VDEC_RESETB_CON`).
- Labels `vdec:`/`venc:` are unused in `mt6735.dtsi`.

## Parity matrix

| Behavior | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| VDEC register window | `of_iomap("mediatek,mt6735-vdec")` (D1:2433) | none | reg `vdec` (0x16020000) parsed via `platform_get_resource_byname`, `devm_ioremap_resource`, phys+size exported as mmap region 0 | done |
| GCON window | separate node `vdec_gcon` (D1:2441) | none | reg `gcon` (0x16000000) parsed/remapped, mmap region 1 | done |
| Clock tree on/off | 5 clocks prepared/enabled (D1:402-431), disabled in reverse (D1:451-455) | none | `devm_clk_bulk_get_all()`, `clk_bulk_prepare_enable()`/`clk_bulk_disable_unprepare()`, failure unwinds PM runtime | done |
| Power domains DIS + VDE | `clk_prepare_enable(SCP_SYS_DIS/VDE)` (D1:402,415) | none | DT `power-domains` attached with `devm_pm_domain_attach_list()`; `pm_runtime_resume_and_get()` powers DIS then VDE via PM links, `pm_runtime_put_sync()` releases them after clocks are off | done (see deviation 1) |
| Block reset | not implemented downstream | none | optional `"vdec"` reset: deassert in `power_on`, assert in `power_off`; absent property is accepted | done (new, hardware-unverified) |
| IRQ request + initial mask | `request_irq(IRQF_TRIGGER_LOW)` then `disable_irq()` (D1:2272-2286) | none | `platform_get_irq()`, `devm_request_irq(..., flags 0, ...)` (DT level-low trigger), then `disable_irq()` | done |
| IRQ enable/disable from ioctl | `enable_irq`/`disable_irq(VDEC_IRQ_ID)` in the lock path (D1:848,855,1089) | none | `enable_irq`/`disable_irq` engine ops | done |
| ISR validation + ack | D1:541-594 | none | identical: GCON bit 4 must be clear, dec-done bit 16 must be set, ack `readl()|0x11` then `readl()&~0x10`, no ack on invalid status | done |
| ISR notification | `eVideoSetEvent(DecIsrEvent)` (D1:589) | none | `vcodec_engine_irq(&vdec->engine, 0)` | done |
| ISR/lock counters | `gu4DecISRCount`, `gu4LockDecHWCount` (D1:560-577) | none | owned by the core (`isr_seq`, `irq_status`) — engine only reports the completion | deferred to core (by design) |
| Power usage counting | `gu4VdecPWRCounter` (D1:388-390,437-441) | none | owned by the core (`powered` transition bookkeeping) | deferred to core (by design) |
| mmap whitelist | `vcodec_mmap()` accepts any start in [0x16000000, 0x16029000) with length <= 0x29000 (D1:2165-2212, check at 2175-2176) | none | engine publishes the single downstream window `{0x16000000, 0x29000}` (base taken from the `gcon` resource); the core whitelists it and requires full containment, which is stricter than the ROM's pfn/length test | done |
| Module lifecycle | `module_init(vcodec_driver_init)` (ROM symbol 54270) | none | no module macros; `vcodec_vdec_register()`/`_unregister()` register/unregister the static `platform_driver` | done |

## Compile test

```sh
cd $M/worktrees/vcodec-vdec
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/vcodec/vcodec-vdec.o
```

Result (final source state: single `{0x16000000, 0x29000}` mmap region, "acknowledgment" spelling):
**exit 0, zero warnings** (`CC drivers/misc/mediatek/vcodec/vcodec-vdec.o` only; the object was removed
before the run so it was a genuine fresh compile). `./scripts/checkpatch.pl --no-tree --no-signoff -f`
reports `total: 0 errors, 0 warnings, 340 lines checked`.

Note: `.config` has `# CONFIG_MTK_VCODEC_MT6735 is not set`; the explicit object target still builds,
and the file contains no config-conditional code. No full kernel build was run.

## Deviations from the assignment (with reasons)

1. **Power domains are attached by the driver, not by the platform bus.** In this kernel
   `genpd_dev_pm_attach()` refuses to attach when the node has more than one `power-domains` entry
   (`drivers/pmdomain/core.c:3274-3286`, *"Devices with multiple PM domains must be attached
   separately"*), and `dev_pm_domain_attach()` is what the platform bus calls
   (`drivers/base/platform.c:1412`). A node with `power-domains = <&spm DIS>, <&spm VDE>` would
   therefore power **no** domain at all and `pm_runtime_resume_and_get(dev)` would be a no-op.
   The driver calls `devm_pm_domain_attach_list(dev, NULL, &pd_list)` on the DT node (domain order =
   DT order = DIS, VDE, matching D1:402/415), which creates PM-runtime device links
   (`drivers/base/power/common.c:193-299`). `pm_runtime_resume_and_get(dev)` then resumes both
   domains synchronously through `__rpm_callback()`/`rpm_get_suppliers()`
   (`drivers/base/power/runtime.c:374-397`), and `pm_runtime_put_sync()` releases them after the
   clocks are off. A node with a single domain already attached by the bus (`-EEXIST`) is accepted.
   **The DTS fragment is unchanged by this.**
2. **`MODULE_DEVICE_TABLE(of, ...)` is kept in the engine file.** It is not a module-lifecycle macro
   (no `module_init`/`module_exit`/`MODULE_LICENSE` here) but it is what emits the OF modalias for
   the single `vcodec-mt6735.ko`; without it the module cannot be auto-loaded from the DT node and the
   core would have to duplicate the compatible strings. Verified the object builds with it while all
   three objects remain one module.

## Inferred / unverified

- **[INFERENCE]** Reset polarity/placement: the MT6735 vdecsys reset is deasserted in `power_on` and
  asserted in `power_off`. Downstream has no reset handling at all, so this ordering is untested on
  hardware. It is optional: a board without `resets`/`reset-names` runs exactly like downstream.
- **[INFERENCE]** ISR semantics (GCON bit 4 = VDEC active, `0xA4` bit 16 = decode done, ack pattern
  `|0x11` then `&~0x10`) are transcribed from D1:553-584 and not exercised against silicon.
- **[INFERENCE]** "Resuming the consumer resumes its suppliers" relies on `dev->power.no_callbacks`
  being 0 for a platform device without driver PM callbacks (it is only set by
  `pm_runtime_no_callbacks()`), verified in `drivers/base/power/runtime.c:931-938` (resume proceeds
  through `rpm_callback()` even with a NULL callback) and `:380-397` (`__rpm_callback()` resumes
  suppliers).
- **Unverified on hardware:** GIC SPI 179 is taken from the downstream DTS
  (`mt6735.dtsi:1433,1469` in the downstream tree); the Nura GIC mapping must be confirmed by the
  DTS owner. `IRQ_TYPE_LEVEL_LOW` matches D1:2378-2380.
- **mmap window:** the published region is `{0x16000000, 0x29000}`, exactly the downstream
  `VDEC_BASE_PHY`/`VDEC_REGION` window (D1:337-338) that `vcodec_mmap()` allows (D1:2165-2212). It
  intentionally includes the SMI LARB1 block at 0x16010000..0x1601ffff because the ROM exposed it to
  the vendor HAL; the ISR itself only touches the `gcon` word and `vdec + 0xA4`. Note the core's
  full-containment rule is stricter than the ROM check, which only compared the start pfn and the
  length: a HAL mapping of 0x16000000/0x29000 or any start inside the window with length <= 0x29000
  is accepted; a mapping that starts inside and runs past 0x16029000 is not (the ROM would have
  accepted it, and then `remap_pfn_range()` would have mapped unmapped SoC space).

## DTS fragment (report only — no DTS was edited)

Add the include for the reset binding (the board DTS does not include it; `mt6735.dtsi` includes only
the clock header):

```dts
#include <dt-bindings/reset/mediatek,mt6735-vdecsys.h>

&vdecsys { status = "okay"; };

vdec: video-codec@16020000 {
	compatible = "mediatek,mt6735-vdec";
	reg = <0 0x16020000 0 0x10000>, <0 0x16000000 0 0x1000>;
	reg-names = "vdec", "gcon";
	interrupts = <GIC_SPI 179 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&mmsys CLK_MM_SMI_COMMON>,
		 <&vdecsys CLK_VDEC_VDEC>,
		 <&vdecsys CLK_VDEC_SMI_LARB1>,
		 <&topckgen CLK_TOP_VDEC_SEL>;
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
			<&spm MT6735_POWER_DOMAIN_VDE>;
	resets = <&vdecsys MT6735_VDEC_RST0_VDEC>;
	reset-names = "vdec";
};
```

Notes:
- Two `power-domains` entries are **required** and are handled by the driver (deviation 1); order is
  DIS then VDE, matching D1:402/415.
- `SCP_SYS_DIS`/`SCP_SYS_VDE` from downstream are represented by these power domains, not clocks;
  `TOP_SYSPLL1_D2/D4` are parents of `vdec_sel` and are enabled through the CCF parent chain.
- `&vdecsys` already has `#reset-cells = <1>` in `mt6735.dtsi:870`, so `resets` resolves.
- The `status = "okay"` line is a no-op today (the `vdecsys` node has no `status` property); kept as
  specified.

## Patch

`$M/patches/vcodec-vdec.patch` — contains only `drivers/misc/mediatek/vcodec/vcodec-vdec.c`
(`git add -N` + `git diff --binary -- <path>`; round-trip verified with `git apply --check -R`).
