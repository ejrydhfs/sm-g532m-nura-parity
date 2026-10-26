# vcodec gap-closure coordinator report (MT6735 VDEC + VENC + shared core)

Date: 2026-10-24
Subsystem coordinator: `VcodecGap`
Base: Nura `v7.0.10-mt6735` arm64, `$M/src/nura-linux` HEAD `046883c0c`
Baseline: `$M/patches/vcodec-integration.patch` (staged in every worktree before work started)
ROM reference: SM-G532M Android 6.0.1 kernel `3.18.19-14373813` (`$M/zip/vmlinux.elf`, `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`)
Downstream reference: `$M/src/downstream-grandpplte` (MT6735 variant `videocodec_kernel_driver_D1.c`; D2 = MT6735M, D3 = MT6753)

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Result summary

Four gap leaves, each in its own worktree, one worker each, one integration worktree:

| Leaf | Worktree | Files | Patch | Report |
|---|---|---|---|---|
| MMDVFS/DVFS (VDEC) | `$M/worktrees/vcodec-gap-dvfs` | `vcodec.h`, `vcodec-core.c`, `vcodec-vdec.c` | `$M/patches/vcodec-gap-dvfs.patch` (545 lines) | `$M/logs/vcodec-gap-dvfs.md` |
| hibernation IRQ restore | `$M/worktrees/vcodec-gap-hibernation` | `vcodec-vdec.c`, `vcodec-venc.c` | `$M/patches/vcodec-gap-hibernation.patch` (135 lines) | `$M/logs/vcodec-gap-hibernation.md` |
| CPU OPP limit | `$M/worktrees/vcodec-gap-cpuopp` | `vcodec-core.c` | `$M/patches/vcodec-gap-cpuopp.patch` (329 lines) | `$M/logs/vcodec-gap-cpuopp.md` |
| remaining `/dev/Vcodec` cmds | `$M/worktrees/vcodec-gap-commands` | `vcodec-core.c` | `$M/patches/vcodec-gap-commands.patch` (20 lines) | `$M/logs/vcodec-gap-commands.md` |
| integration | `$M/worktrees/vcodec-gap-integration` | whole series | `$M/patches/vcodec-gap-integration.patch` | this file |

Combined gap delta on top of the baseline (5 files, 768 insertions, 44 deletions):

```
Documentation/devicetree/bindings/media/mediatek,mt6735-vcodec.yaml |  41 +  20 -
drivers/misc/mediatek/vcodec/vcodec-core.c                          | 336 +  13 -
drivers/misc/mediatek/vcodec/vcodec-vdec.c                          | 327 +   8 -
drivers/misc/mediatek/vcodec/vcodec-venc.c                          |  39 +   0 -
drivers/misc/mediatek/vcodec/vcodec.h                               |  25 +   3 -
```

No worktree was shared. No worker touched shared DTS, `$M/worktrees/integration`, or another
worker's tree. No stubs, `TODO: implement`, placeholder probes or fake success paths
(`grep -rn "TODO\|FIXME\|placeholder probe\|fake success"` over the vcodec sources: no hits).

---

## 2. What is now closed vs still blocked

| Gap | Status | Where |
|---|---|---|
| VDEC clock DVFS (the `clkmux_sel` half of ROM `SendDvfsRequest`) | **closed** | leaf 1 |
| VDEC DVFS load monitor (`VdecDvfsBegin/Step/Adjustment/MonitorStart/End`) | **closed** | leaf 1 |
| MMDVFS SMI-bandwidth / VCORE vote (`mmdvfs_set_step(SMI_BWC_SCEN_VP, …)`) | **blocked** — no MMDVFS/vcorefs/SMI-BWC framework in Nura | leaf 1 report §2.4, §5 |
| VENC DVFS | **not applicable** — neither downstream nor the ROM has any encoder DVFS | leaf 1 report §2.5 |
| Hibernation `pm_restore_noirq` IRQ re-sensitization | **closed** | leaf 2 |
| `SET_CPU_OPP_LIMIT` | **closed with freq_qos** (ROM's own call is commented out; helper absent everywhere) | leaf 3 |
| `INC/DEC_SYSRAM_USER`, `SET/GET_SYSRAM_INFO`, `MFV_*MEM*`, `PMEM_FLUSH/CLEAN` (11 commands) | **closed as "no ROM service"** — vcodec never handled them; ROM SYSRAM is the camera ISP driver | leaf 4 |
| `ALLOC/FREE_NON_CACHE_BUFFER` | **blocked** — no defined userspace buffer contract; the ROM refuses both | leaf 4 |
| compat/native dispatch consistency for the refused commands | **closed** | leaf 4 |

Leaf-4 ROM verification (jump-table disassembly of `vcodec_unlocked_ioctl @ 0xc03974fc`):
the ROM dispatches a 42-entry table over `cmd - 0x40044d0b`; `0x06-0x0a` are below the table
base (default case), `0x10/0x11/0x13/0x14/0x25/0x26` index default slots, `0x22/0x23` are
dedicated one-`printk` cases (`"Don't support VCODEC_ALLOC_NON_CACHE_BUFFER"`,
`vmlinux.strings:27310-27311`). Every path returns 255 and touches no user memory. The ROM
`vcodec_fops` has no `compat_ioctl` slot (=0).

---

## 3. Key evidence

### 3.1 DVFS / MMDVFS

* Downstream `videocodec_kernel_driver_D1.c`:
  `SendDvfsRequest()` `:122-162` (LOW → mux input 3 = SYSPLL1_D4, HIGH → input 1 = SYSPLL1_D2,
  wrapped in `clk_prepare_enable`/`clk_set_parent`/`clk_disable_unprepare`);
  monitor `:107-260`; call sites `:858-860` (LOCKHW), `:1094-1096` (UNLOCKHW),
  `:1291-1296` (INC_DEC_EMI_USER), `:2121-2137` (last close).
* Vendor mux mapping: `mt_pm_init.c:300-320` (input 1 = 273 MHz, input 3 = 136.5 MHz);
  `mt_cpufreq.c:2270` (MAINPLL = 1092 MHz). `mt_clkmgr.c:2038-2044` mux enable bit 15.
* ROM: `SendDvfsRequest @ 0xc0396240`, `mmdvfs_set_step @ 0xc04cacdc`, `clkmux_sel @ 0xc034d410`,
  `VdecDvfs*` in `$M/zip/symbols.txt:16833-16840`; the SMI/VCORE vote is therefore real in the
  ROM and is the part that cannot be ported (`mmdvfs_mgr.c:233-301` →
  `vcorefs_request_dvfs_opp(KIR_MM, OPPI_PERF)`).
* Nura: no `mmdvfs`, no vcorefs, no SMI bandwidth provider for MT6735 (`drivers/interconnect`
  has only emi/mt8183/mt8195/mt8196; `drivers/memory/mtk-smi.c` has no bandwidth or icc API).

### 3.2 Hibernation

* Downstream `videocodec_kernel_driver_D1.c:2376-2386` (`vcodec_pm_restore_noirq`:
  `mt_irq_set_sens(..., MT_LEVEL_SENSITIVE)` + `mt_irq_set_polarity(..., MT_POLARITY_LOW)`
  for both IRQs), registered at `:2547`.
* ROM `vcodec_pm_restore_noirq @ 0xc0396198`: disassembly shows `mov r1,#1` →
  `mt_irq_set_sens @ 0xc02eae48`, `mov r1,#0` → `mt_irq_set_polarity @ 0xc02ea84c`, twice
  (VDEC then VENC), return 0.
* Nura: standard hibernation (`CONFIG_HIBERNATION=y`), root `interrupt-parent = <&sysirq>`
  (`mt6735.dtsi:30`), `mtk_sysirq_set_type()` maps level-low to INTPOL + level-high with a raw
  spinlock (no bus lock), so `irq_set_irq_type()` is the exact equivalent and is non-sleeping.
  `.restore_noirq` runs before `resume_device_irqs()`, so the line is still masked.

### 3.3 CPU OPP limit

* Downstream `videocodec_kernel_driver_D1.c:1533-1555`: struct copied, then
  `cpu_opp_limit(EVENT_VIDEO, …)` **commented out** (`/* TODO: Check if cpu_opp_limit is
  available */`). No `cpu_opp_limit` symbol in the ROM, no definition anywhere in the
  downstream tree.
* Nura uses cpufreq-dt (`CONFIG_CPUFREQ_DT=y`, `# CONFIG_ARM_MEDIATEK_CPUFREQ is not set`);
  `freq_qos_add/update/remove_request` are exported (`kernel/power/qos.c:644,680,723`).
* Argument semantics are `[INFERENCE]`: `limited_freq` = kHz ceiling, `limited_cpu` = 32-bit
  CPU bitmask (0 = all), `enable` 1/0.

### 3.4 Remaining commands

Covered in §2 and in `$M/logs/vcodec-gap-commands.md`: SYSRAM symbols in the ROM belong to
`camera_sysram_D1`, not vcodec (no call from any vcodec text range); non-cache buffers are
implemented only on MT6735M/MT6753 (D2/D3) with a kernel-VA-in-`u4ReservedSize` contract that
has no userspace mapping path, and the shipped MT6735 ROM refuses them.

---

## 4. Integration

```sh
cd $M/worktrees/vcodec-gap-integration          # fresh from HEAD, baseline staged
git apply --3way $M/patches/vcodec-gap-dvfs.patch          # clean
git apply --3way $M/patches/vcodec-gap-cpuopp.patch        # clean
git apply --3way $M/patches/vcodec-gap-hibernation.patch   # 1 conflict, resolved
git apply --3way $M/patches/vcodec-gap-commands.patch      # clean
```

* **Only conflict**: the include block of `vcodec-vdec.c` (DVFS added `<linux/ktime.h>`,
  hibernation added `<linux/irq.h>`). Resolved by keeping both, alphabetically ordered.
  No semantic hunk conflicted: DVFS confines itself to the DVFS block, struct fields, probe and
  time functions; hibernation adds only the PM callback and `.driver.pm`.
* **Coordinator integration fix — DT binding**: the baseline binding still described 3–4 clocks,
  hyphenated `clock-names` and claimed the driver "does not look them up by name". The DVFS leaf
  now requires named clocks for the decoder (`smi_common`, `vdec`, `smi_larb1`, optional
  `vdec_sel`, `vdec_sel_high`, `vdec_sel_low`). The binding was updated to the implemented
  contract (max 6 clocks, vdec `clock-names` required, matching example). Clock names were
  aligned to the driver/underscore spelling rather than rewriting working driver code; this is
  purely a binding/example change.
* **Patch chain verified**: `git apply --check` of the combined delta on a pristine
  `baseline-applied` tree succeeds, and applying it reproduces the integration files
  byte-for-byte (`cmp` over all five files: identical).

Review corrections that were sent back to leaves and applied:

| Leaf | Corrections |
|---|---|
| cpuopp | reject negative `limited_cpu`; re-enable releases policies outside the new mask; guard the 32-bit mask shift for `cpu >= 32` |
| commands | compat `default:` must return `-EOPNOTSUPP` for known-but-unsupported commands like the native path, not `-ENOTTY` |
| dvfs, hibernation | none |

---

## 5. Compile verification (exercised, not inferred)

Environment for every row: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`,
`.config` = `$M/nura.config` + `make ARCH=arm64 LLVM=1 olddefconfig`, plus
`CONFIG_MTK_VCODEC_MT6735=m` (the driver is new, so the reference config does not set it).

| Check | Result |
|---|---|
| `make ARCH=arm64 LLVM=1 … W=1 -j2 vcodec-core.o vcodec-vdec.o vcodec-venc.o` | exit 0, `CC [M]` each, **0 warnings** (`$M/logs/vcodec-gap-integration-build.log`) |
| `make … vcodec-mt6735.o` (all three objects linked) | exit 0, `LD [M] drivers/misc/mediatek/vcodec/vcodec-mt6735.o` |
| `CONFIG_COMPAT=n` (compat code is fully guarded) | exit 0 for all three objects, 0 warnings |
| `CONFIG_CPU_FREQ=n` (freq_qos arm compiled out) | `vcodec-core.o` exit 0, no `freq_qos_*`/`cpufreq_cpu_*` references |
| `perl scripts/checkpatch.pl --no-tree --strict -f` on 5 sources | 0 errors / 0 warnings / 0 checks (core 1482, vdec 659, venc 353, header 153, uapi 227 lines) |
| `git diff --check` | no whitespace errors |
| `git apply --check` of the delta on pristine baseline | clean; re-application byte-identical |

The leaf worktrees additionally ran their own object builds (reported in the leaf files); the
integration build above is the authoritative one because it contains the merged series.

**Not run** (deliberate, environment-limited): full kernel build / `.ko` modpost (this worktree
has no `vmlinux.o`/`Module.symvers`; `LD [M]` of the module object succeeded, so the cross-file
symbols resolve), `dt_binding_check` (dtschema not installed; the YAML parses cleanly), and any
on-hardware test.

---

## 6. DTS fragment (shared DTS not edited)

`arch/arm64/boot/dts/mediatek/mt6735.dtsi` already provides the clock/power/reset providers but
no codec nodes. Board/SoC owner must add (fragment; `#include
<dt-bindings/reset/mediatek,mt6735-vdecsys.h>` is needed for the reset cell):

```dts
vdec: video-codec@16020000 {
	compatible = "mediatek,mt6735-vdec";
	reg = <0 0x16020000 0 0x10000>,
	      <0 0x16000000 0 0x1000>;
	reg-names = "vdec", "gcon";
	interrupts = <GIC_SPI 179 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&mmsys CLK_MM_SMI_COMMON>,
		 <&vdecsys CLK_VDEC_VDEC>,
		 <&vdecsys CLK_VDEC_SMI_LARB1>,
		 <&topckgen CLK_TOP_VDEC_SEL>,
		 <&topckgen CLK_TOP_SYSPLL1_D2>,
		 <&topckgen CLK_TOP_SYSPLL1_D4>;
	clock-names = "smi_common",
		      "vdec",
		      "smi_larb1",
		      "vdec_sel",
		      "vdec_sel_high",
		      "vdec_sel_low";
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
			<&spm MT6735_POWER_DOMAIN_VDE>;
	resets = <&vdecsys MT6735_VDEC_RST0_VDEC>;
	reset-names = "vdec";
};

venc: video-codec@17002000 {
	compatible = "mediatek,mt6735-venc";
	reg = <0 0x17002000 0 0x1000>;
	interrupts = <GIC_SPI 180 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&mmsys CLK_MM_SMI_COMMON>,
		 <&vencsys CLK_VENC_VENC>,
		 <&vencsys CLK_VENC_SMI_LARB3>;
	clock-names = "smi_common", "venc", "smi_larb3";
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
			<&spm MT6735_POWER_DOMAIN_VEN>;
};
```

Notes:

* The first three clocks of the decoder are the power-on set and must stay in this order; the
  driver enables them in DT order and disables them in reverse. The three DVFS clocks are
  optional — without them the driver logs one line and keeps the boot-selected parent.
* Placement: root level (inherits `interrupt-parent = <&sysirq>`), or inside `/soc` via
  `&{/soc} { … }` (Nura's `soc` node has no label). Both use 2/2 address/size cells.
* Interrupt numbers and level-low trigger come from the downstream DTS; the codec IRQ must be
  routed through `sysirq` for the hibernation re-sensitization to be meaningful (a direct GIC
  SPI would return `-EINVAL`, which is logged).
* Device-node permissions: `/dev/Vcodec` is created `0660 root:root`; add a udev/ueventd rule for
  the codec process (e.g. `SUBSYSTEM=="misc", KERNEL=="Vcodec", GROUP="video", MODE="0660"`).

---

## 7. Parity matrix (gap level)

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| VDEC clock step 273/136.5 MHz | `SendDvfsRequest` → `clkmux_sel(MT_MUX_VDEC, 1/3)` (`D1:122-162`, ROM `0xc0396240`) | none | `clk_set_parent()` on `CLK_TOP_VDEC_SEL` between `syspll1_d2`/`syspll1_d4`, mux enabled around the switch | done (hardware-unverified) |
| VDEC DVFS monitor (4 s window, 50 %/90 % thresholds, 1 ms overhead) | `VdecDvfs*` (`D1:107-260`, ROM `0xc0396240`+) | none | full port, `ktime_get()`, private mutex, driven from the four core call sites | done |
| SMI bandwidth / VCORE step | `mmdvfs_set_step(SMI_BWC_SCEN_VP, …)` (ROM `0xc04cacdc`) | none | not ported — Nura has no MMDVFS/vcorefs; documented | blocked |
| VENC DVFS | none | none | none (proven) | n/a |
| Hibernation IRQ re-sensitivity | `mt_irq_set_sens`/`mt_irq_set_polarity` (ROM `0xc0396198`) | none | `.restore_noirq` → `irq_set_irq_type(LEVEL_LOW)` on both engines | done |
| CPU OPP ceiling | `cpu_opp_limit()` call commented out; returns without effect | `-EOPNOTSUPP` | real `FREQ_QOS_MAX` request per policy, apply/replace/release, released on last close and module exit | done (semantics inferred) |
| SYSRAM user / info commands | no vcodec handler (ROM default case → 255) | `-EOPNOTSUPP` | unchanged refusal; 32-bit caller now sees the same `-EOPNOTSUPP` | closed |
| non-cache buffer alloc/free | ROM refuses ("Don't support …") | `-EOPNOTSUPP` | unchanged; buffer contract missing | blocked |
| pmem/ion memory commands | no vcodec handler | `-EOPNOTSUPP` | unchanged refusal | closed |
| Command dispatch consistency | ROM: one return value for everything | native `-EOPNOTSUPP`, compat `-ENOTTY` | both `-EOPNOTSUPP` | done |

---

## 8. Deviations and hardware-only unknowns

Deviations (all documented in the leaf reports):

1. `SET_CPU_OPP_LIMIT` removes every ceiling at the last close of `/dev/Vcodec` and at module
   exit; the inferred Samsung helper had no release path (it never ran). Disabling releases all
   policies regardless of the supplied mask; re-enabling replaces the previous ceiling set.
2. `limited_cpu` is treated as a 32-bit bitmask, so CPUs >= 32 can only be covered by mask 0.
3. WAITISR/NON-cache/SYSRAM returns remain `-EOPNOTSUPP`/`-ENOTTY` instead of the ROM's `255`.
4. DVFS steps are skipped when the DT does not describe the mux and both parents (one log line).

Hardware-only unknowns (no phone attached):

* The 273/136.5 MHz mux positions are taken from the vendor clock tables and the ROM code; the
  actual VDEC rate on silicon is unverified.
* Nura's `clk-mt6735-topckgen.c` models `syspll1_d2`/`syspll1_d4` as `mainpll/2` and `mainpll/4`
  (2× the vendor rates) while attaching the correct names to the correct mux positions. Parent
  selection is therefore correct, but `clk_get_rate()` on that mux reports the wrong value. This
  is a pre-existing shared-clock-driver issue, deliberately not touched here; it must be fixed
  before anyone builds an OPP table on those rates.
* The SMI/VCORE vote is absent, so the decoder runs at whatever VCORE the boot chain left
  configured — the same situation as the vendor MMDVFS "vcorefs cannot work" fallback, which
  still switches the mux (`mmdvfs_set_step` is called but the clock switch is unconditional).
* ISR acknowledgement, IRQ numbering (SPI 179/180), power-domain order, and the reset placement
  remain as documented in `$M/logs/vcodec-coordinator.md` §9.
* Hibernation restore was verified against the in-tree call path only; the sysirq re-programming
  cannot be observed without hardware.

---

## 9. Deliverables

```
$M/patches/vcodec-gap-dvfs.patch           (545 lines)
$M/patches/vcodec-gap-hibernation.patch    (135 lines)
$M/patches/vcodec-gap-cpuopp.patch         (329 lines)
$M/patches/vcodec-gap-commands.patch       ( 20 lines)
$M/patches/vcodec-gap-integration.patch    (1116 lines, delta on top of vcodec-integration.patch)

$M/logs/vcodec-gap-dvfs.md
$M/logs/vcodec-gap-hibernation.md
$M/logs/vcodec-gap-cpuopp.md
$M/logs/vcodec-gap-commands.md
$M/logs/vcodec-gap-coordinator.md          (this file)
$M/logs/vcodec-gap-integration-build.log   (final W=1 objects + LD [M] log)
```

Apply order for the integration owner:

1. `git apply $M/patches/vcodec-integration.patch` (existing subsystem baseline).
2. `git apply $M/patches/vcodec-gap-integration.patch` (this gap series; verified clean).
   The four leaf patches are equivalent but conflict with each other on `vcodec-core.c` /
   `vcodec-vdec.c`; use the combined patch.
3. Enable `CONFIG_MTK_VCODEC_MT6735`, add the §6 DTS nodes and the device-node permission rule.
4. Full build produces `vmlinux.o`/`Module.symvers`, then link `vcodec-mt6735.ko`.
