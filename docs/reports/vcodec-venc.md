# vcodec-venc: MT6735 VENC platform driver

## Worktree

`/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/worktrees/vcodec-venc`
(detached HEAD of nura-linux, Linux 7.0.10-mt6735, arm64)

## Files changed

| File | Change |
|---|---|
| `drivers/misc/mediatek/vcodec/vcodec-venc.c` | new, 314 lines. `struct vcodec_engine` provider + `platform_driver` for `mediatek,mt6735-venc`, exported to the shared module through `vcodec_venc_register()` / `vcodec_venc_unregister()` |

No other file was created or modified. `vcodec.h`, `Kconfig`, `Makefile`, the DTS and the
core/VDEC files were left untouched (coordinator-owned / other workers).

## What the driver does

The MT6735 VENC register file is programmed by the vendor userspace codec libraries through
the register window exported by `vcodec-core.c` (`vcodec_mmap()`); the kernel owns only
clocks, power domains, the ownership-locked interrupt, and the mmap whitelist entry for
`0x17002000`. The driver contains no codec register programming, no firmware and no VPU
mailbox, matching downstream exactly.

* OF match `mediatek,mt6735-venc`; one `reg` entry mapped with
  `devm_platform_ioremap_resource()`; the same resource supplies the single mmap region
  `{phys = res->start, size = resource_size(res)}` (= `0x17002000`, `0x1000`).
* `devm_clk_bulk_get_all()` for the `clocks` property; `pm_runtime_enable()` in probe,
  `pm_runtime_disable()` in remove.
* `devm_pm_domain_attach_list()` in probe attaches **both** power domains described by
  `power-domains` (`DIS`, `VEN`). This is required: `genpd_dev_pm_attach()` silently
  returns 0 for a node with more than one `power-domains` entry
  (`drivers/pmdomain/core.c:3261-3288`), so without the explicit list attach
  `pm_runtime_resume_and_get()` would switch on nothing. `devm_pm_domain_attach_list()`
  (`drivers/base/power/common.c:329`, `EXPORT_SYMBOL_GPL`) creates
  `DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME` device links, so the existing
  `pm_runtime_resume_and_get()` / `pm_runtime_put_sync()` calls drive DIS then VEN.
  `-EEXIST` (single domain already attached by the bus) is accepted and the driver keeps
  working; a 0 return means the node describes no PM domain at all and probe fails with
  `-ENODEV` instead of silently succeeding without power.
* `platform_get_irq()` + `devm_request_irq()` with flags `0` (the trigger comes from the DT
  interrupt specifier, level-low), then `disable_irq()` so the core's LOCKHW path enables it
  on demand.
* `power_on`: `pm_runtime_resume_and_get()` → `clk_bulk_prepare_enable()`; a clock failure
  rolls the runtime PM reference back with `pm_runtime_put_sync()` and returns the error.
  `power_off`: `clk_bulk_disable_unprepare()` → `pm_runtime_put_sync()`.
* ISR: reads `base + 0x05c`, then the downstream branch sequence (no owner → log + ack all
  six bits one write at a time and return; H264 → ack each set bit; HEVC → log only;
  anything else → log invalid holder), then `vcodec_engine_irq(eng, status)` and
  `IRQ_HANDLED`.
* `MODULE_DEVICE_TABLE(of, vcodec_venc_of_match)` provides the OF autoload alias. It is a
  device-table annotation, not a lifecycle macro, so the shared module keeps its single
  `module_init` in `vcodec-core.c`.
* `vcodec_venc_register()` registers the static `platform_driver`;
  `vcodec_venc_unregister()` is a no-op if registration never succeeded. No
  `module_init`/`module_exit`/`MODULE_*`/`module_platform_driver()`.

## Downstream / ROM evidence

All downstream citations are
`src/downstream-grandpplte/drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c`
unless stated otherwise.

| Item | Downstream evidence |
|---|---|
| VENC base `0x17002000`, region `0x1000` | D1:332-334 (`VENC_BASE`, `VENC_REGION`) |
| IRQ status offset `0x05c`, ack offset `0x060` | D1:347-348; applied at D1:2426-2427 |
| Status bits SPS `0x1`, PPS `0x2`, FRM `0x4`, DRAM `0x8`, PAUSE `0x10`, SWITCH `0x20` | D1:360-365 |
| Power-on clock order `SCP_SYS_DIS`, `DISP0_SMI_COMMON`, `SCP_SYS_VEN`, `VENC_VENC`, `VENC_LARB` | D1:479-504 |
| Power-off order is the exact reverse (`VENC_LARB`, `VENC_VENC`, `SCP_SYS_VEN`, `SMI_COMMON`, `SCP_SYS_DIS`) | D1:530-534 |
| Encoder power counter (`gu4VencPWRCounter`) | D1:461-467, 514-521 |
| ISR: no owner → log + clear all status bits | D1:620-633 |
| ISR: H264 → per-set-bit ack via status read | D1:636-662 |
| ISR: HEVC → log only | D1:663-665 |
| ISR: invalid holder type → log | D1:666-669 |
| ISR: wake ISR event | D1:671-675 |
| IRQ requested at init with level-low, then `disable_irq()` | D1:2279-2287; polarity set at 2382-2383 |
| LOCKHW/UNLOCKHW enable/disable the encoder IRQ | D1:977 (`enable_irq(VENC_IRQ_ID)`), 1119 (`disable_irq(VENC_IRQ_ID)`) |
| Encoder irq handler is `video_intr_dlr2` → `enc_isr` | D1:682-686 |
| DT node `mediatek,mt6735-venc`, reg `0x17002000 0x1000`, `GIC_SPI 180 IRQ_TYPE_LEVEL_LOW` | `src/downstream-grandpplte/arch/arm64/boot/dts/mt6735.dtsi:1492-1496` |
| No `writel`/`readl` of codec data registers anywhere (only ISR status/ack) | whole file; grep shows HW access only at D1:626-661 |
| No VPU/SCP codec firmware symbols | `zip/symbols.txt` has no `vpu`/codec firmware loader symbols |

Nura-side evidence used for the binding: `arch/arm64/boot/dts/mediatek/mt6735.dtsi:885-899`
(`vencsys` syscon + `CLK_VENC_SMI_LARB3`), `include/dt-bindings/clock/mediatek,mt6735-vencsys.h:6-8`,
`drivers/clk/mediatek/clk-mt6735-vencsys.c:22-30` (four gates, **no reset controller**),
`include/dt-bindings/power/mediatek,mt6735-power-controller.h:8,12` (`DIS`=2, `VEN`=6).

## Parity matrix

| Behavior | ROM / downstream implementation | Nura before | After | Status |
|---|---|---|---|---|
| VENC register window `0x17002000`+`0x1000` | of_iomap of `mediatek,mt6735-venc` (D1:2423-2427) | no node, no driver | `devm_platform_ioremap_resource`, window also published as the single mmap region | done |
| IRQ 180, level-low | `request_irq(..., IRQF_TRIGGER_LOW)` (D1:2279), explicit sensitivity/polarity (D1:2382-2383) | absent | `platform_get_irq` + `devm_request_irq` with DT trigger (fragment is level-low) | done |
| IRQ masked until LOCKHW | `disable_irq(VENC_IRQ_ID)` at init (D1:2287), enabled at D1:977 | absent | `disable_irq()` in probe; `enable_irq`/`disable_irq` ops driven by the core | done |
| Clock enable order | DIS, SMI_COMMON, VEN(scpsys), VENC_VENC, VENC_LARB (D1:479-504) | absent | `pm_runtime_resume_and_get` (DIS+VEN via genpd) then `clk_bulk_prepare_enable` in DT order SMI_COMMON, VENC_VENC, SMI_LARB3 | done |
| Clock disable order | exact reverse of enable (D1:530-534) | absent | `clk_bulk_disable_unprepare` disables in reverse list order = LARB3, VENC_VENC, SMI_COMMON, then `pm_runtime_put_sync` drops VEN/DIS | done |
| Nested power users | `gu4VencPWRCounter` refcount (D1:461-467, 514-521) | absent | the core serialises on its own `->powered` state transition, so only one engine power-on/off pair exists per lock window; `pm_runtime` carries the domain refcount | done (different mechanism, same observable behavior) |
| ISR with no HW owner | log + six separate ack writes (D1:620-633) | absent | same, `dev_err_ratelimited` + six `writel`s of PAUSE/SWITCH/DRAM/SPS/PPS/FRM, returns without waking | done |
| ISR H264 | read status, ack each set bit (D1:636-662) | absent | same, six conditional `writel`s | done |
| ISR HEVC | log only, no ack (D1:663-665) | absent | `dev_err_ratelimited`, no ack; userspace library acks through its mmap | done |
| ISR invalid holder | log type (D1:666-669) | absent | `dev_err_ratelimited` with type value | done |
| ISR completion event | `eVideoSetEvent(&EncIsrEvent, ...)` (D1:671) | absent | `vcodec_engine_irq(eng, status)` wakes `VCODEC_WAITISR` | done |
| Encoder power domains DIS+VEN | downstream enables `SCP_SYS_DIS` then `SCP_SYS_VEN` clocks (D1:479-492) | absent | explicit `devm_pm_domain_attach_list()` + runtime PM; `genpd_dev_pm_attach()` alone cannot attach two domains | done |
| Encoder reset controller | none used downstream either (vencsys gate driver has only CG set/clr/sta) | absent | not implemented — Nura's `clk-mt6735-vencsys.c` registers no `#reset-cells` | not applicable (documented) |
| VPU / SCP codec firmware | none (no vpu symbols in `symbols.txt`) | absent | not implemented | not applicable |

## Compile test

```
cd $M/worktrees/vcodec-venc
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/vcodec/vcodec-venc.o
  CC      drivers/misc/mediatek/vcodec/vcodec-venc.o        (clean, no warnings)
```

Re-run with `W=1` after touching the source: also clean, no warnings.
`scripts/checkpatch.pl --no-tree -f` → `total: 0 errors, 0 warnings, 0 checks, 314 lines`.

The object was rebuilt and checkpatch re-run after the two review fixes (multi power-domain
attach list + `MODULE_DEVICE_TABLE`); both results above are from that final source.

Strictly an object build: no full kernel build, `.config` untouched.

## Inferred / unverified

* `[INFERENCE]` The VENC ack register is write-1-to-clear per bit; the kernel only reproduces
  the downstream writes without reading the ack register back, exactly as downstream does.
  Hardware behavior of the ack writes is unverified on a device.
* `[INFERENCE]` For HEVC the vendor library acknowledges the encoder itself through the
  mmap'ed window, since downstream neither acks nor disables the line in that branch
  (D1:663-665). Consequence: an HEVC interrupt left unacked by userspace will keep the
  level-triggered line asserted and re-enter the ISR, matching downstream's behavior.
* `[INFERENCE]` The two `power-domains` are resumed in DT property order (DIS then VEN) by
  the device links created by `devm_pm_domain_attach_list()`; this was not measured on
  hardware.
* The larb3 gate is shared with the `smi_larb3` device node (`mt6735.dtsi:892-900`); the
  clock framework refcounts it, so double enable is harmless. Not exercised at runtime here.
* The ISR's `vcodec_engine_is_locked()` / `vcodec_engine_get_driver_type()` helpers are
  assumed to be hardirq-safe reads (they are used from the hardirq handler as specified by
  the internal contract; `vcodec-core.c` is coordinator-owned and was not inspected for
  this task beyond `vcodec.h`).

## DTS fragment (not applied)

Adjusted clock order versus the brief: downstream enables `VENC_VENC` **before** `VENC_LARB`
(D1:498-504) and disables in exact reverse (D1:530-534). `clk_bulk_prepare_enable()` enables
in property order and `clk_bulk_disable_unprepare()` disables in reverse order, so listing
`CLK_VENC_VENC` before `CLK_VENC_SMI_LARB3` reproduces the downstream sequence exactly. The
power-domain order (DIS then VEN) is left as given, matching D1:479-492.

```dts
venc: video-codec@17002000 {
	compatible = "mediatek,mt6735-venc";
	reg = <0 0x17002000 0 0x1000>;
	interrupts = <GIC_SPI 180 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&mmsys CLK_MM_SMI_COMMON>,
		 <&vencsys CLK_VENC_VENC>,
		 <&vencsys CLK_VENC_SMI_LARB3>;
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
			<&spm MT6735_POWER_DOMAIN_VEN>;
};
```

## Patch

`/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/patches/vcodec-venc.patch`
(one `diff --git`, only `drivers/misc/mediatek/vcodec/vcodec-venc.c`).
