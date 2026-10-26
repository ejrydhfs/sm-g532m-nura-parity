# vcodec subsystem coordinator report (MT6735 VENC + VDEC + shared core)

Date: 2026-10-24
Subsystem owner: `VcodecCoordinator`
Base: Nura `v7.0.10-mt6735` arm64, nura-linux HEAD `046883c0c`
ROM reference: SM-G532M Android 6.0.1 kernel `3.18.19-14373813` (`$M/zip/vmlinux.elf`, recovered kallsyms in `$M/zip/symbols.txt`)
Downstream reference: `$M/src/downstream-grandpplte/drivers/misc/mediatek/videocodec` (MT6735 variant `mt6735/videocodec_kernel_driver_D1.c`)

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Result summary

Three cooperating drivers, one module, fully compile-tested and integrated with no conflicts:

| Leaf | Worktree | Files (new unless noted) | Patch | Report |
|---|---|---|---|---|
| shared core | `$M/worktrees/vcodec-core` | `drivers/misc/mediatek/vcodec/vcodec-core.c`, `include/uapi/linux/vcodec.h`, DT binding, `Kconfig`/`Makefile` hooks (+ `drivers/misc/{Kconfig,Makefile}` edited) | `$M/patches/vcodec-core.patch` | `$M/logs/vcodec-core.md` |
| VDEC | `$M/worktrees/vcodec-vdec` | `drivers/misc/mediatek/vcodec/vcodec-vdec.c` | `$M/patches/vcodec-vdec.patch` | `$M/logs/vcodec-vdec.md` |
| VENC | `$M/worktrees/vcodec-venc` | `drivers/misc/mediatek/vcodec/vcodec-venc.c` | `$M/patches/vcodec-venc.patch` | `$M/logs/vcodec-venc.md` |
| integration | `$M/worktrees/vcodec-integration` | full series incl. coordinator-owned `vcodec.h` | `$M/patches/vcodec-integration.patch` | this file |

Coordinator-owned infrastructure (not in any leaf patch, present in the combined patch):
`drivers/misc/mediatek/vcodec/vcodec.h` (engine contract: `struct vcodec_engine`, ops,
`vcodec_engine_register/unregister/irq`, `vcodec_engine_is_locked`,
`vcodec_engine_get_driver_type`, `vcodec_{vdec,venc}_register/unregister`).

Build: `CONFIG_MTK_VCODEC_MT6735` (tristate), single module `vcodec-mt6735.ko` built from
`vcodec-core.o + vcodec-vdec.o + vcodec-venc.o`. Only `vcodec-core.c` owns
`module_init/module_exit` and `MODULE_*`; the engine files are platform drivers registered
by the core at init and unregistered at exit.

The subsystem is **not** a V4L2 mem2mem codec driver. Section 2 explains why, with evidence.
No firmware blob is used, requested or invented: MT6735 has no codec firmware (section 2.3).

---

## 2. Architecture decision: hardware-service driver, not V4L2 mem2mem

### 2.1 What the ROM kernel actually does

The ROM kernel driver is a register/power/IRQ service, not a codec:

* `videocodec_kernel_driver_D1.c` contains no `readl`/`writel` of bitstream, motion-vector,
  coefficient or frame registers. Its only MMIO accesses are ISR validation and ack
  (`dec_isr`, `enc_isr`) and the VDEC clock-gate active check.
* Userspace programs the codec. `vcodec_mmap()` (D1:2165-2212) remaps arbitrary physical
  register windows chosen by the caller (`vma->vm_pgoff`), after a weak range check against
  `VENC_BASE 0x17002000`, `VDEC_BASE_PHY 0x16000000`, `HW_BASE 0x07fff000`,
  `INFO_BASE 0x10000000` (D1:336-354, 2178-2205).
* The ABI (`videocodec_kernel_driver.h:19-52`) is only: LOCKHW, UNLOCKHW, WAITISR,
  EMI/PWR user accounting, THREAD_ID, CORE_LOADING/NUMBER, CPU_LOADING_INFO,
  CPU_OPP_LIMIT, MB, and the non-cache/sysram commands that the driver itself refuses.
* ROM disassembly confirms the same shape:
  * `dec_isr` @ `c0397f80`: GCON `tst #16` (`c0397f9c`), decode-done `[base+0xA4] tst #65536`
    (`c0397fac`), ack `orr #17`/`bic #16` on `[base+0xA4]` (`c0397ff4`-`c0398010`).
  * `enc_isr` @ `c0398098`: lock-holder type compares `#5` (H264_ENC) and `#28` (HEVC_ENC)
    (`c0398100`-`c039810c`), per-bit acks `#16,#32,#8,#1,#2,...` (`c0398154`+).
  * `vcodec_driver_init` @ `c0daf914` stores `venc_base+0x5c` and `venc_base+0x60`
    (`add r12,r3,#92` / `add r3,r3,#96`, `c0daf98c`-`c0daf998`).
  * `vcodec_probe` @ `c0395dac`, `vdec_power_on` @ `c0396888`, `venc_power_on` @ `c0396ab8`,
    `vcodec_lockhw`/`vcodec_unlockhw`/`vcodec_waitisr`/`vcodec_unlocked_ioctl`.
* ROM symbol table has **no** VPU/SCP symbols and the downstream driver has **no**
  `request_firmware()` call. Nothing is loaded into the codec engines at boot.

### 2.2 Why V4L2 mem2mem is not implementable from the available sources

A V4L2 mem2mem driver must program the codec in the kernel. On MT6735 those sequences
(the H.264/VP8/HEVC command streams, DMV/DPB/RC setup, coefficient buffers) live only in
the vendor userspace codec libraries (`libMtkOmxVdec/Venc`), reached through the mmap'ed
register windows. Reimplementing them would mean reverse-engineering the Android codec
HAL, which the assignment forbids, and the register semantics are not documented in either
the downstream tree or the ROM. Inventing them would violate the "no invented registers"
rule. The existing Nura `drivers/media/platform/mediatek/vcodec` framework is also not
usable: `nura.config` has `# CONFIG_MEDIA_SUPPORT is not set`, and its backends require a
VPU (MT8173) or SCP (MT8183+) firmware/IPI path that MT6735 does not have.

The assignment permits "a standard platform interface consistent with Nura". The delivered
interface is: two standard `platform_driver`s bound to DT nodes plus one misc device
(`/dev/Vcodec`) that exposes the ROM ABI. This is the complete kernel half of the ROM
contract; the userspace codec libraries remain the codec implementation, exactly as on the
ROM. It is a drop-in for the ROM kernel interface (ioctl numbers, structures, compat
layouts, mmap semantics) rather than a port of the Android HAL.

### 2.3 Firmware statement

* No codec firmware exists for MT6735 in the ROM, in the downstream tree, or in the
  Nura tree. No `request_firmware()`/VPU/SCP/mailbox path is added. Nothing is faked.
* The only "firmware-adjacent" ROM services (internal-memory/SYSRAM user counters and the
  non-cache buffer allocation commands) were dead in the ROM driver (no ioctl case); they
  return `-EOPNOTSUPP` here instead of a fake success.
* If a future MT6735 firmware path appears, it would attach to the same core: power_on /
  enable_irq / WAITISR semantics are already firmware-agnostic.

---

## 3. Leaf coordination

Worktrees were created fresh from `$M/src/nura-linux` HEAD (`046883c0c`), one per driver,
one worker each (`opencode-worker`): `VcodecCore`, `VcodecVdec`, `VcodecVenc`. No worktree
was shared. `$M/worktrees/integration` and the pre-existing `$M/worktrees/vcodec` were not
touched.

Coordinator pre-created the build scaffolding (`drivers/misc/mediatek/{Kconfig,Makefile}`,
`drivers/misc/mediatek/vcodec/{Kconfig,Makefile}`, `vcodec.h`) in every worktree so each
leaf could compile its object; only the core leaf carries the scaffolding in its patch.
Leaf patches contain only their own driver file (plus core's scaffolding/uapi/binding).

Review corrections requested by the coordinator and applied by the leaves:

* VDEC: mmap window widened to the ROM-compatible `0x16000000 + 0x29000` range.
* VENC: added `devm_pm_domain_attach_list()` (multi-power-domain DT nodes are not
  auto-attached by the PM core) and `MODULE_DEVICE_TABLE(of, ...)` for module autoload.
* Core: IRQ-unmask vs lock-publish race fixed (owner/type/hw_locked are published before
  `enable_irq()` so the VENC ISR never mis-acks a pending completion as spurious);
  JPEG_ENC lock no longer powers the encoder or unmasks VENC_IRQ (ROM parity); WAITISR
  timeout return matched to the ROM user-visible `-EFAULT`; `CONFIG_COMPAT=n` build fixed;
  compat CPU-loading struct no longer depends on 64-bit tail padding.

No leaf touched shared DTS. No leaf produced stubs, `TODO: implement`, placeholder probe or
fake success paths (verified by grep over the integrated tree).

---

## 4. Integration and conflicts

`$M/worktrees/vcodec-integration` (fresh from HEAD):

```sh
git apply $M/patches/vcodec-core.patch
git apply $M/patches/vcodec-vdec.patch
git apply $M/patches/vcodec-venc.patch
cp .../vcodec.h drivers/misc/mediatek/vcodec/vcodec.h     # coordinator-owned
```

* All three patches applied with `git apply --check` clean; **no conflicts, no fuzz**.
* The combined series `$M/patches/vcodec-integration.patch` (2457 lines, 12 files,
  2380 insertions) applies cleanly to pristine nura-linux HEAD (checked with
  `git apply --check` from `$M/src/nura-linux`).
* No shared DTS file is modified by any patch (`git status` in every worktree shows no DTS).

Final integrated diff:

```
 Documentation/devicetree/bindings/media/mediatek,mt6735-vcodec.yaml |  178 +++
 drivers/misc/Kconfig                                               |    1 +
 drivers/misc/Makefile                                              |    1 +
 drivers/misc/mediatek/Kconfig                                      |    3 +
 drivers/misc/mediatek/Makefile                                     |    3 +
 drivers/misc/mediatek/vcodec/Kconfig                               |   16 +
 drivers/misc/mediatek/vcodec/Makefile                              |    7 +
 drivers/misc/mediatek/vcodec/vcodec-core.c                         | 1159 +++++++++
 drivers/misc/mediatek/vcodec/vcodec-vdec.c                         |  340 +++
 drivers/misc/mediatek/vcodec/vcodec-venc.c                         |  314 +++
 drivers/misc/mediatek/vcodec/vcodec.h                              |  131 +++
 include/uapi/linux/vcodec.h                                        |  227 +++
 12 files changed, 2380 insertions(+)
```

---

## 5. Compile results (exercised, not inferred)

Environment: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, `.config` from
`$M/nura.config` + `olddefconfig`, `CONFIG_MTK_VCODEC_MT6735=m`.

| Check | Command | Result |
|---|---|---|
| Leaf objects | `make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/vcodec/vcodec-{core,vdec,venc}.o` | exit 0, zero warnings (each leaf, fresh compile) |
| Leaf `W=1` | same with `W=1` | exit 0, no diagnostics |
| Integrated objects | `make ARCH=arm64 LLVM=1 W=1 -j2 ...vcodec-{core,vdec,venc}.o` | exit 0, `CC [M]` each, **0 warnings** (`$M/logs/vcodec-integration-build.log`) |
| Combined module object | `make ARCH=arm64 LLVM=1 -j2 .../vcodec-mt6735.o` | exit 0, `LD [M] drivers/misc/mediatek/vcodec/vcodec-mt6735.o` — all three objects link with no duplicate/undefined cross-file symbols |
| `CONFIG_COMPAT=n` | `scripts/config --disable COMPAT; olddefconfig; make ... vcodec-core.o` | exit 0, clean (compat code is fully guarded) |
| checkpatch | `perl scripts/checkpatch.pl --no-tree --strict -f` on all 5 sources | 0 errors / 0 warnings / 0 checks |
| `.ko` modpost | `make ... vcodec-mt6735.ko` | blocked by environment: no `vmlinux.o`/`Module.symvers` in this worktree (no full build). Expected; requires the integration owner's full build. Not a code failure: `LD [M]` of the module object succeeded and all core-kernel symbols used are exported (`kcpustat_field`/`kcpustat_cpu_fetch`, `sched_clock`, `nr_cpu_ids`, `pin_user_pages_fast`, `unpin_user_page`, `remap_pfn_range`, `misc_register`, `devm_pm_domain_attach_list` — verified EXPORT_SYMBOL(_GPL)). |

Not run (out of leaf scope, by design): full kernel build, DTS build, `dt_binding_check`
(dtschema/dt-doc-validate are not installed here), on-hardware probes.

---

## 6. Parity matrix (subsystem level)

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| DT description of codec engines | downstream DTS: `vdec@16020000`, `vdec_gcon@16000000`, `venc@17002000`, IRQ SPI 179/180, level-low | only `vdecsys`/`vencsys` syscons + clock providers; no codec nodes | binding doc + DTS fragment (section 7); shared DTS not edited per rules | done (fragment pending DTS owner) |
| Engine power/clock sequencing | `vdec_power_on/off` D1:385-460; `venc_power_on/off` D1:461-540 (SCP_SYS_DIS → SMI common → engine domain → engine clocks) | no driver | genpd DIS+VDE/VEN via `devm_pm_domain_attach_list()`, then `clk_bulk_prepare_enable()`; reverse on power-off | done |
| VDEC block reset | not handled in ROM | n/a | optional `resets = <&vdecsys MT6735_VDEC_RST0_VDEC>`: deassert on power-on, assert on power-off | done (inference, see §9) |
| VDEC ISR/ack | `dec_isr` D1:541-594: GCON bit4 clear, done bit16 set, ack OR 0x11 then clear bit4 | none | identical sequence, `readl/writel`, error logs rate-limited | done |
| VENC ISR/ack | `enc_isr` D1:596-675: no owner → ack all; H264 → ack set bits; HEVC → log only | none | identical branch structure; status word returned to WAITISR | done |
| Hardware ownership lock (blocking, timeout, owner identity) | `vcodec_lockhw` D1:688-1119; owner = physical address of handle (`pmem_user_v2p_video`) | none | per-engine mutex + waitqueue; owner from `pin_user_pages_fast()` (raw-pointer fallback); default 1000 ms; `-ETIMEDOUT` on expiry instead of ROM's endless retry | done (documented timeout deviation) |
| Unlock / IRQ mask / power off | `vcodec_unlockhw` D1:1057-1147 | none | owner-checked; disable_irq then power_off; wake waiters | done |
| ISR wait | `vcodec_waitisr` D1:1148-1243 | none | `wait_event_interruptible_timeout()` on per-engine sequence; encoder status copied back | done |
| EMI / power-user accounting | INC/DEC_{ENC,DEC}_EMI_USER, INC/DEC_PWR_USER | none | real counters; DEC saturates at 0; PWR counter only (ROM's L2C path compiled out) | done |
| Register-window mmap | `vcodec_mmap` D1:2165-2212 (4 hardcoded windows) | none | strict containment in engine-registered windows: VENC `0x17002000+0x1000`, VDEC `0x16000000+0x29000` | done (hardened; see §9) |
| `/dev/Vcodec` ABI | magic `'M'`, 34 commands, 32-bit structs | none | same numbers/encodings, native 64-bit structs + byte-exact 32-bit compat structs | done |
| CPU queries | GET_CORE_NUMBER, GET_CORE_LOADING (`get_cpu_load`), GET_CPU_LOADING_INFO | none | `nr_cpu_ids`; real kcpustat delta load; sched_clock + locked-engine count | partial (see §9) |
| CPU OPP limit | command accepted, handler empty (`cpu_opp_limit` commented out) | none | `-EOPNOTSUPP` (no MTK API in Nura) | deferred |
| Non-cache buffer / SYSRAM / ion commands | no ioctl handler in ROM | none | `-EOPNOTSUPP` | deferred (ROM parity) |
| MMDVFS vdec clock/voltage scaling | `VdecDvfs*` + `SendDvfsRequest` under `ENABLE_MMDVFS_VDEC` | none | not ported: no MMDVFS/SMI DVFS framework in Nura | deferred |
| Hibernation IRQ re-sensitivity | `vcodec_pm_restore_noirq` under `CONFIG_MTK_HIBERNATION` | none | not ported: no MTK hibernation; DT describes the level-low trigger | deferred |
| JPEG encode | lock records handle only, no VENC power/IRQ | none | same (lock records only; WAITISR rejects JPEG) | done |
| V4L2 mem2mem codec | none on MT6735 | n/a | not implemented (section 2) | n/a (documented) |

---

## 7. DTS fragment (shared DTS not touched)

`arch/arm64/boot/dts/mediatek/mt6735.dtsi` already includes the clock and power bindings
but **not** the VDEC reset binding. The integration owner needs to add:

```dts
#include <dt-bindings/reset/mediatek,mt6735-vdecsys.h>
```

and the two nodes (board DTS or SoC dtsi, status enabled by the board):

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
		 <&topckgen CLK_TOP_VDEC_SEL>;
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
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
			<&spm MT6735_POWER_DOMAIN_VEN>;
};
```

Notes for the DTS owner:

* Placement: the fragment above is written as root-level nodes and can be pasted at the top
  level of the board DTS — the root node has `#address-cells = <2>` /
  `#size-cells = <2>`, so the four-cell `reg` entries are correct. If the nodes are put
  inside the SoC bus instead, use `&{/soc} { ... }` because Nura's `mt6735.dtsi` has an
  unlabelled `soc` node (`soc { ... }`, no `soc:` label); `&soc` does not resolve. The SoC
  node also uses 2/2 cells, so the same `reg` form applies.

* The VDEC `gcon` window intentionally overlaps the `vdecsys` syscon node
  (`0x16000000`, 4 KiB). Both map the same registers; the clock provider keeps using the
  syscon, the codec ISR reads the VDEC-active bit. This mirrors downstream, which maps the
  same region through a second compatible node.
* Clock order is the downstream enable order (D1:402-431 for VDEC, D1:479-504 for VENC);
  `clk_bulk` enables in DT order and disables in reverse.
* Both engines list two power domains (DIS + engine domain). The PM core does not
  auto-attach multi-domain nodes; the drivers call `devm_pm_domain_attach_list()` and the
  runtime-PM device links power DIS before VDE/VEN.
* IRQ numbers are from the downstream DTS (SPI 179 VDEC, 180 VENC, level-low).
* The overlapping `gcon` mapping was checked against the existing providers in this tree:
  `mtk_clk_simple_probe()` maps a clock node only when it has composite/divider clocks
  (the MT6735 vdecsys provider has gate clocks only, `clk-mt6735-vdecsys.c`), and its reset
  controller uses `device_node_to_regmap()`, while syscon maps with `of_iomap()`
  (`drivers/mfd/syscon.c:61`). None of them calls `request_mem_region()` on
  `0x16000000`, so the VDEC driver's `devm_ioremap_resource()` on that window does not
  conflict.

Device-node permissions: the misc device is created as `/dev/Vcodec`, mode `0660`,
root:root. A udev/ueventd rule is required for the codec userspace process (ROM ueventd
exposed it to media/mediaserver), e.g.:

```
SUBSYSTEM=="misc", KERNEL=="Vcodec", GROUP="video", MODE="0660"
```

or the Android equivalent `/dev/Vcodec 0660 media media`.

---

## 8. ROM / downstream evidence index

| Item | Evidence |
|---|---|
| VDEC base `0x16020000`, MISC offset 0, VLD +0x1000, GCON `0x16000000` | D1:336-338, 2423-2446; ROM `vcodec_driver_init` @ `c0daf914` |
| VDEC done status `base+0xA4` bit 16; ack OR 0x11 then clear bit 4; GCON bit 4 | D1:541-594; ROM `dec_isr` @ `c0397f80` (`c0397f9c`, `c0397fac`, `c0397ff4`-`c0398010`) |
| VENC base `0x17002000`, status +0x5C, ack +0x60, bits 0x1/0x2/0x4/0x8/0x10/0x20 | D1:332-365, 2423-2430; ROM `enc_isr` @ `c0398098`, `vcodec_driver_init` `c0daf98c`-`c0daf998` |
| Clock enable/disable order | D1:385-460 (VDEC), D1:461-540 (VENC) |
| Ownership lock / wait / unlock semantics | D1:688-1119, 1057-1243 |
| ABI command numbers and encodings | `videocodec_kernel_driver.h:19-52`; compat structs D1:1672-1740 |
| Driver-type enum values (H264_ENC=5, HEVC_ENC=28, …) | `include/val_types_public.h:135-170`; ROM `enc_isr` compares `#5`, `#28` |
| mmap semantics | D1:2165-2212 |
| No firmware/VPU/SCP | no `request_firmware` in downstream videocodec; no vpu/scp codec symbols in `symbols.txt`; VPU driver in Nura only matches `mediatek,mt8173-vpu` |
| Nura clock/reset/power bindings | `include/dt-bindings/clock/mediatek,mt6735-{vdecsys,vencsys,mmsys,topckgen}.h`, `include/dt-bindings/reset/mediatek,mt6735-vdecsys.h`, `mt6735.dtsi` spm domains VDE/VEN |

---

## 9. Unresolved gaps and deliberate deviations

Hardware-unverified (no phone attached):
* All register offsets/status bits are transcribed from downstream and cross-checked against
  ROM disassembly; none were exercised on silicon.
* ISR ack semantics are assumed write-1-to-clear per bit (downstream writes single bits).
* VDEC reset deassert/assert placement is new (downstream has no reset handling).
* GIC SPI numbering comes from the downstream DTS; Nura's sysirq mapping is unverified.
* Power-domain resume order relies on genpd device links (verified in this tree's
  `rpm_get_suppliers()` for `DL_FLAG_PM_RUNTIME`).

Deliberate deviations from the ROM (all documented in the leaf reports):
1. Lock timeout terminates with `-ETIMEDOUT` instead of the ROM's unbounded retry loop.
2. WAITISR timeout returns the ROM's user-visible `-EFAULT`; signal interruption returns
   `-ERESTARTSYS` (ROM collapsed both to `-EFAULT`).
3. Non-owner unlock/waitisr return `-EPERM` (ROM: `-EFAULT`).
4. mmap accepts only engine-registered windows; the ROM also allowed arbitrary
   `HW_BASE`/`INFO_BASE` MMIO. VDEC deliberately publishes the ROM-compatible
   `0x16000000+0x29000` window, which includes SMI LARB1 (`0x16010000`), as the ROM did.
5. EMI decrement saturates at 0 (ROM underflows).
6. `GET_CPU_LOADING_INFO` zero-initialises the struct (ROM copied uninitialised stack).
7. `SET_CPU_OPP_LIMIT` returns `-EOPNOTSUPP` (ROM accepted and did nothing).
8. CPU load is computed from kcpustat deltas instead of MTK's `get_cpu_load()` (not present).
9. Last-close cleanup powers engines off and masks IRQs (ROM reset bookkeeping only,
   leaking clocks).
10. JPEG lock records ownership without powering VENC/unmasking IRQ (ROM parity) and
    WAITISR rejects JPEG with `-EINVAL`.

Deferred, not implemented:
* MMDVFS/VDEC DVFS monitor and `SendDvfsRequest()` (no MMDVFS framework in Nura).
* Hibernation IRQ re-sensitivity (no MTK hibernation).
* The non-cache buffer, SYSRAM and ion-facing commands that the ROM driver itself never
  handled (return `-EOPNOTSUPP`).
* L2 cache reconfiguration for the H.264 encoder (`VENC_USE_L2C` is disabled downstream).
* Any V4L2 mem2mem presentation layer (section 2 — would require the prohibited HAL port).

Integration follow-ups for the main/integration owner:
1. Apply `$M/patches/vcodec-integration.patch` (or the three leaf patches plus
   `vcodec.h`), enable `CONFIG_MTK_VCODEC_MT6735`, add the DTS include/nodes and the
   device-node permission rule.
2. Run the full build (produces `vmlinux.o`/`Module.symvers`) and then link
   `vcodec-mt6735.ko`; object-level and module-object-level links are already clean here.
3. On hardware: verify SPI 179/180, the two power-domain attach path, the VDEC
   done/ack sequence, and the VENC status bits with a real userspace codec library.

---

## 10. Deliverables

```
$M/patches/vcodec-core.patch           (9 files, scaffolding + core + uapi + binding)
$M/patches/vcodec-vdec.patch           (vcodec-vdec.c only)
$M/patches/vcodec-venc.patch           (vcodec-venc.c only)
$M/patches/vcodec-integration.patch    (complete series incl. vcodec.h, 12 files)
$M/logs/vcodec-core.md
$M/logs/vcodec-vdec.md
$M/logs/vcodec-venc.md
$M/logs/vcodec-coordinator.md          (this file)
$M/logs/vcodec-integration-build.log   (integrated W=1 objects + LD [M] log)
```
