# VDEC DVFS (ENABLE_MMDVFS_VDEC monitor) — MT6735 vcodec

Worktree: `$M/worktrees/vcodec-gap-dvfs` (nura-linux HEAD `046883c0c` + `vcodec-integration.patch` staged)
Patch: `$M/patches/vcodec-gap-dvfs.patch`
Date: 2026-10-24

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Downstream shorthand `D1` = `$M/src/downstream-grandpplte/drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c`

---

## 1. Files changed

| File | Change |
|---|---|
| `drivers/misc/mediatek/vcodec/vcodec.h` | four optional `dvfs_*` callbacks in `struct vcodec_engine_ops` + contract documentation (process context, `eng->lock` held, no re-locking, no sleeping in atomic context) |
| `drivers/misc/mediatek/vcodec/vcodec-core.c` | `vcodec_engine_by_id()` split out of `vcodec_lookup_engine()`; calls at the four downstream points: INC_DEC_EMI_USER (native + compat dispatch), successful LOCKHW, UNLOCKHW, last close |
| `drivers/misc/mediatek/vcodec/vcodec-vdec.c` | `TOP_VDEC_SEL` mux + two SYSPLL1 parent handles (optional), explicit named power-on clock set, full D1 monitor state machine, level transitions |

`vcodec-venc.c` is untouched (VENC has no DVFS in the ROM or downstream, see §3).

`git status --short` in the worktree shows only those three files as `AM` on top of the staged
baseline; `.config` is gitignored.

---

## 2. Evidence

### 2.1 Clock selection (`SendDvfsRequest`)

Downstream `D1:122-162`:

* LOW (`D1:128`) → `clk_set_parent(mux, SYSPLL1_D4)` = 136.5 MHz, mux wrapped in
  `clk_prepare_enable()`/`clk_disable_unprepare()` (`D1:132-138`).
* HIGH (`D1:140`) → `clk_set_parent(mux, SYSPLL1_D2)` = 273 MHz (`D1:147-153`).
* Clock handles fetched in `D1:2321-2338` (`MT_CG_TOP_MUX_VDEC`, `MT_CG_TOP_SYSPLL1_D2`,
  `MT_CG_TOP_SYSPLL1_D4`), i.e. **after** the three gates fetched at `D1:2290-2304`
  (`MT_CG_DISP0_SMI_COMMON`, `MT_CG_VDEC0_VDEC`, `MT_CG_VDEC1_LARB`).

Mux/parent mapping (vendor clock tables, same parent list as `vdec_sel` in Nura):

* `mt_pm_init.c:298-320` (`mt_check_bus_freq`): mux input 1 = `SYSPLL1_D2` **273 MHz**,
  input 3 = `SYSPLL1_D4` **136.5 MHz**, input 0 = 26 MHz, input 2 = `SYSPLL_D5` 218.4 MHz.
* `mt_clkmgr.c:2038-2044` (`MUX_VDEC`): `sel_mask = 0x00000700` (shift 8, 3 bits),
  `pdn_mask = 0x00008000`, `nr_inputs = 8`.
* Nura `drivers/clk/mediatek/clk-mt6735-topckgen.c:149-158` registers
  `vdec_sel_parents[] = { "clk26m", "syspll1_d2", "syspll_d5", "syspll1_d4", ... }` and
  `MUX_GATE_CLR_SET_UPD(CLK_TOP_VDEC_SEL, "vdec_sel", ..., CLK_CFG_1, ..., gate_shift 15)` —
  the same 3-bit field and the same bit-15 power-down gate. Selecting by parent *handle* is
  therefore physically correct.
* Pre-existing Nura rate model: `clk-mt6735-topckgen.c:75-76` models
  `syspll1_d2 = mainpll/2` and `syspll1_d4 = mainpll/4`; with MAINPLL = 1092 MHz
  (`mt_cpufreq.c:2270`) that reports 546/273 MHz instead of the vendor 273/136.5 MHz, i.e. every
  rate is **2x**. The mux *positions* are right, so the hardware frequency is right; only
  `clk_get_rate()` is wrong. This driver deliberately never calls `clk_get_rate()`/`clk_set_rate()`.
  The rate-model mismatch is a shared-clock-driver follow-up, out of this leaf's scope.

ROM (`$M/zip/vmlinux.elf`):

* `SendDvfsRequest @ 0xc0396240` (`symbols.txt:16833`). Level 0 (`beq 0xc0396308`) → `printk`
  "…SendDvfsRequest(MMDVFS_VOLTAGE_LOW)", `clk_prepare`/`clk_enable` on `[0xc102f760+0xbc]`,
  `clk_set_parent(mux, [r5+0xc4])` (`0xc0396340-0xc0396348`), `clk_disable`/`clk_unprepare`,
  then `mmdvfs_set_step(3, 0)` (`0xc0396360-0xc0396368`). Level 1 → `mmdvfs_set_step(3, 1)`
  first (`0xc0396284-0xc039628c`), then the same clock sequence with the other parent
  (`[r5+0xc0]`, `0xc03962b4`). Any other level prints "OOPS: level = %d".
  So `SMI_BWC_SCEN_VP = 3`, `MMDVFS_VOLTAGE_LOW = 0`, `MMDVFS_VOLTAGE_HIGH = 1`;
  `mmdvfs_set_step @ 0xc04cacdc` (`symbols.txt:20491`), `clkmux_sel @ 0xc034d410`
  (`symbols.txt:15440`, the `CONFIG_MTK_CLKMGR` variant, not used by the ROM build).
* Strings `$M/zip/vmlinux.strings:27263-27274,27314` contain the whole monitor log set
  ("SendDvfsRequest(MMDVFS_VOLTAGE_LOW/HIGH)", "VdecDvfsBegin", "VdecDVFS monitor %dms,
  decoded %d frames", "reset monitor duration (%d ms), percent: %d", "INC_DEC_EMI MM DVFS init").

### 2.2 Monitor state machine and call sites

| Downstream | Behaviour |
|---|---|
| `D1:107-121` | constants: `MONITOR_DURATION_MS 4000`, `DROP_PERCENTAGE 50`, `RAISE_PERCENTAGE 90`, `SW_OVERHEAD_MS 1`, `DVFS_DEFAULT = MMDVFS_VOLTAGE_HIGH` |
| `D1:163-173` `VdecDvfsBegin` | monitor on, counts 0, interval 0, first lock, max 0 |
| `D1:175-182` `VdecDvfsEnd` | log + reset bookkeeping |
| `D1:191-205` `VdecDvfsStep` | diff since last lock, update max, interval += diff + 1 ms |
| `D1:207-241` `VdecDvfsAdjustment` | if active and counts > 0: window < 4000 ms → just step; else percentage = 100·interval/duration, `< 50` → LOW, `> 90` → HIGH, else keep; then counts++ |
| `D1:243-260` `VdecDvfsMonitorStart` | begin if not active; on the first lock of a window reset the window start; always record the lock time |
| `D1:858-860` | `VdecDvfsMonitorStart()` after power-on + `enable_irq`, secure instance included |
| `D1:1094-1096` | `VdecDvfsAdjustment()` after `vdec_power_off()`, before releasing the HW lock |
| `D1:1291-1296` | INC_DEC_EMI_USER: `SendDvfsRequest(DVFS_DEFAULT)` then `VdecDvfsBegin()` |
| `D1:2121-2137` | last close: if monitor active → reset bookkeeping and `SendDvfsRequest(DVFS_LOW)` (the EMI counter was already zeroed at `D1:2092`) |

Nura uses `ktime_get()`/`ktime_ms_delta()` (monotonic) instead of downstream's
`eVideoGetTimeOfDay()`; `timekeeping` semantics are equivalent for a 4 s window.

### 2.3 VENC: not applicable

* `$M/zip/symbols.txt`: `VencDvfs` — 0 hits (the ROM only has `VdecDvfsBegin`,
  `VdecDvfsGetMonitorDuration`, `VdecDvfsEnd`, `VdecDvfsStep`, `VdecDvfsAdjustment`,
  `VdecDvfsMonitorStart` at lines 16835-16840).
* `$M/zip/vmlinux.strings`: `VencDvfs` — 0 hits.
* `D1` has a single `#define ENABLE_MMDVFS_VDEC` block (`D1:87-88`), used only at
  `D1:858,1096,1291,2121,2133,2320`; no encoder reference. `grep -rn VencDvfs
  downstream .../videocodec` = 0 hits.
* Nura has no VENC OPP table and none is invented: `vcodec-venc.c` is unchanged and its
  `dvfs_*` ops stay NULL.

### 2.4 Missing framework: MMDVFS / vcorefs / SMI bandwidth

* Nura has **no** MMDVFS or vcorefs code at all:
  `grep -rl "mmdvfs\|vcorefs" $M/src/nura-linux/drivers $M/src/nura-linux/include` → no files;
  `grep -rn "mmdvfs_set_step\|vcorefs_request_dvfs_opp\|SMI_BWC_SCEN"` → no hits;
  there is no `drivers/misc/mediatek/smi` directory (only the SMI bus driver
  `drivers/memory/mtk-smi.c`, which has no DVFS/vcorefs API).
* Downstream entry point that the vote would need:
  `drivers/misc/mediatek/smi/mmdvfs_mgr.c:233-302` (`mmdvfs_set_step()`): maintains the
  per-scenario voltage table and, at `:296-302`, calls
  `vcorefs_request_dvfs_opp(KIR_MM, OPPI_PERF)` / `(KIR_MM, OPPI_UNREQ)`; `mmdvfs_set_step()`
  itself is compiled to a no-op when `MMDVFS_ENABLE` is 0 (`:239-241`).
  Scenario/voltage constants: `include/mt-plat/mt_smi.h:50` (`SMI_BWC_SCEN_VP`) and `:68,70`
  (`MMDVFS_VOLTAGE_LOW = MMDVFS_VOLTAGE_0`, `MMDVFS_VOLTAGE_HIGH = MMDVFS_VOLTAGE_1`).
* Consequence: the **clock half** of DVFS is implemented (mux parent switching, exact D1
  sequence); the **VCORE/SMI vote** (`mmdvfs_set_step(SMI_BWC_SCEN_VP, level)`) is not, because
  the framework (vcorefs + MMDVFS manager + SMI bandwidth monitor) does not exist in this tree.
  Adding it would be a separate subsystem port (SMI BWC monitor, vcorefs/EMI DVFS, regulator
  coupling), not a vcodec change.

---

## 3. Parity matrix

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| VDEC clock step LOW = `VDEC_SEL` parent 3 (`syspll1_d4`, 136.5 MHz) | `SendDvfsRequest` level 0, `clk_set_parent` on `MT_CG_TOP_MUX_VDEC` (`D1:128-138`); ROM `0xc0396308-0xc039636c` | none | `vcodec_vdec_dvfs_request(vdec, VCODEC_DVFS_LOW)` → `clk_set_parent(clk_sel, clk_sel_low)` inside `clk_prepare_enable`/`clk_disable_unprepare` | done |
| VDEC clock step HIGH = parent 1 (`syspll1_d2`, 273 MHz) | `D1:140-153`; ROM `0xc0396270-0xc03962d8` | none | same with `clk_sel_high` | done |
| Mux/parent handles from DT, optional | `devm_clk_get` (required) `D1:2321-2338` | none | `devm_clk_get_optional(dev, "vdec_sel" / "vdec_sel_high" / "vdec_sel_low")`; one `dev_info`, boot parent kept, probe never fails | done (deviation: optional instead of fatal) |
| Power-on clock set excludes the DVFS parents | gates only (`D1:2290-2304`, `D1:395-431`) | `devm_clk_bulk_get_all()` → whatever DT lists | explicit `devm_clk_bulk_get()` for `"smi_common"`, `"vdec"`, `"smi_larb1"`; mux clocks are never prepared by `power_on()` | done |
| Monitor: 4 s window, 50% drop / 90% raise, 1 ms overhead | `D1:107-260` | none | same state machine (`monitor_active/first_lock/counts/start/last/interval/max`) | done |
| Start monitor on LOCKHW | `D1:858-860` | none | `ops->dvfs_monitor_start` after power-on + unmask, before `mutex_unlock`, secure instances included; JPEG path returns before it | done |
| Evaluate on UNLOCKHW | `D1:1094-1096` | none | `ops->dvfs_adjustment` after power-off, before the release is published | done |
| INC_DEC_EMI_USER: default HIGH + begin | `D1:1291-1296` | counter only | `ops->dvfs_begin` after the counter is published (native and compat dispatch) | done |
| Last close: LOW + reset | `D1:2121-2137` | engines forced idle | `ops->dvfs_idle` in the last-close engine loop, after the engine is idle | done |
| Level transitions logged | `MODULE_MFV_LOGE` | — | `dev_dbg` for each request/window/end, one `dev_info` at probe when enabled | done |
| SMI bandwidth / VCORE vote | `mmdvfs_set_step(SMI_BWC_SCEN_VP, level)` (`D1:139,143`; ROM `0xc039628c`/`0xc0396368` → `mmdvfs_set_step @ 0xc04cacdc`) | no framework | **not implemented** — no MMDVFS/vcorefs/SMI-BWC in Nura (§2.4) | deferred (missing framework) |
| VENC DVFS | none (no `VencDvfs*` in ROM, no encoder reference in D1) | none | none, ops NULL | n/a |
| Locking/ordering constraints | — | — | callbacks run under `eng->lock`, monitor state behind `vdec->dvfs_lock`; no re-lock, no atomic context, JPEG early-return and owner-publish order untouched | done |

---

## 4. Compile evidence

Environment: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, worktree `$M/worktrees/vcodec-gap-dvfs`,
`.config` from `$M/nura.config` (`CONFIG_MTK_VCODEC_MT6735=m`).

```sh
cp $M/nura.config .config
make ARCH=arm64 LLVM=1 olddefconfig            # exit 0
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/misc/mediatek/vcodec/vcodec-core.o \
      drivers/misc/mediatek/vcodec/vcodec-vdec.o \
      drivers/misc/mediatek/vcodec/vcodec-venc.o
```

Result: exit 0, `CC [M]` for all three objects, no diagnostics.

Same command with `W=1` (objects removed first): exit 0, **0 warnings** for the three objects.

`perl scripts/checkpatch.pl --no-tree --strict -f` on the three files: 0 errors, 0 warnings,
0 checks.

Not run: full kernel build, `dt_binding_check` (no DTS edited). No on-hardware test (no phone).

---

## 5. Inferred / hardware-unverified

1. `[INFERENCE]` Enabling the `TOP_VDEC_SEL` gate (bit 15 of `CLK_CFG_1`) around `set_parent`
   and leaving it at the `clk_disable_unprepare()` state is exactly what `D1:132-138/147-153`
   and the ROM (`clk_disable`/`clk_unprepare` at `0xc03962c8-0xc03962d0`) do; it is assumed the
   bit is a mux/clock-switch enable and not the VDEC functional gate (which is the vdecsys
   `CLK_VDEC_VDEC` gate). Not verified on silicon.
2. `[INFERENCE]` Nura's CCF applies the parent change immediately (the mux driver writes
   set/clr and the update bit, `clk-mt6735-topckgen.c` + `drivers/clk/mediatek/clk-mux.c:183-221`);
   the vendor `pdn_mask` semantics are matched.
3. `[INFERENCE]` The monitor's 4 s cadence is wall-clock; `ktime_get()` is monotonic and
   unaffected by suspend in the downstream `eVideoGetTimeOfDay()` meaning, so the numbers can
   differ across a suspend/resume cycle (downstream used RTC-based time).
4. `clk_get_rate()` on `vdec_sel` reports 2x the vendor rate in this tree (§2.1). No code
   depends on it, but an OPP table built from it would be wrong — shared clock-driver issue.
5. The `dvfs_*` callbacks are documented as process-context only; the core indeed only calls
   them from ioctl/release paths, but nothing enforces it at runtime.
6. The percentage arithmetic (`100 * interval / duration`) uses 32-bit `u32`; with
   `duration <= ~4.3e7 ms` (12 h) it cannot overflow, matching downstream.
7. Hardware-only: whether the 136.5 MHz step is stable under the VCORE level that the absent
   MMDVFS vote would have raised. Without the vote the clock can drop to 136.5 MHz at the
   current VCORE; downstream couples both. This is the main risk of the deferred half.

---

## 6. DTS-FRAGMENT

Shared DTS was **not** edited. Update the `vdec` node (from `$M/logs/vcodec-coordinator.md` §7)
so that the three DVFS clocks are described; the first three `clocks` entries and their names
must stay in the order below because `power_on()` bulk-enables exactly `smi_common`, `vdec`,
`smi_larb1`:

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
```

Notes:

* `CLK_TOP_VDEC_SEL` (46), `CLK_TOP_SYSPLL1_D2` (8) and `CLK_TOP_SYSPLL1_D4` (9) come from
  `include/dt-bindings/clock/mediatek,mt6735-topckgen.h:15,16,54`, already included by
  `arch/arm64/boot/dts/mediatek/mt6735.dtsi:16`.
* Omitting the last three entries is legal: the driver logs
  "VDEC DVFS clock parents not described, DVFS disabled" and keeps the boot parent.
* No new `#include` is needed beyond the existing clock bindings and
  `dt-bindings/reset/mediatek,mt6735-vdecsys.h`.

---

## 7. Patch

`$M/patches/vcodec-gap-dvfs.patch` (545 lines, 3 files, +385/−23).
