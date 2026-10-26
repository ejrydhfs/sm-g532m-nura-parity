# vcodec gap: hibernation IRQ re-sensitization

Worktree: `$M/worktrees/vcodec-gap-hibernation` (fresh from nura-linux HEAD `046883c0c`,
baseline `$M/patches/vcodec-integration.patch` applied and staged)
Patch: `$M/patches/vcodec-gap-hibernation.patch`
Scope: `vcodec-core.c` / `vcodec.h` untouched.

---

## 1. Files changed

| File | Change |
|---|---|
| `drivers/misc/mediatek/vcodec/vcodec-vdec.c` | add `vcodec_vdec_restore_noirq()`, `vcodec_vdec_pm_ops`, wire `.driver.pm`; include `<linux/irq.h>`, `<linux/pm.h>` |
| `drivers/misc/mediatek/vcodec/vcodec-venc.c` | add `vcodec_venc_restore_noirq()`, `vcodec_venc_pm_ops`, wire `.driver.pm`; include `<linux/irq.h>`, `<linux/pm.h>` |

`git diff --stat`: 79 insertions, 0 deletions, exactly those two files.
No DTS, no core, no header, no Kconfig/Makefile change.

---

## 2. Evidence

### 2.1 Downstream reference

* `drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c:2376-2386`
  (`vcodec_pm_restore_noirq`), compiled only under `CONFIG_MTK_HIBERNATION`:
  ```c
  static int vcodec_pm_restore_noirq(struct device *device)
  {
          /* vdec: IRQF_TRIGGER_LOW */
          mt_irq_set_sens(VDEC_IRQ_ID, MT_LEVEL_SENSITIVE);
          mt_irq_set_polarity(VDEC_IRQ_ID, MT_POLARITY_LOW);
          /* venc: IRQF_TRIGGER_LOW */
          mt_irq_set_sens(VENC_IRQ_ID, MT_LEVEL_SENSITIVE);
          mt_irq_set_polarity(VENC_IRQ_ID, MT_POLARITY_LOW);

          return 0;
  }
  ```
* Registration: `videocodec_kernel_driver_D1.c:2547`
  `register_swsusp_restore_noirq_func(ID_M_VCODEC, vcodec_pm_restore_noirq, NULL);`
  (the MTK hibcore framework; declared at
  `drivers/misc/mediatek/hibcore/mtk_hibernate_dpm.h:48-51`).
* Polarity/sensitivity constants: `include/linux/irqchip/mtk-gic-extend.h:19-20`
  `MT_LEVEL_SENSITIVE 1`, `MT_POLARITY_LOW 0` (identical values in
  `include/linux/irqchip/mt-eic.h:91-92`).
* IRQ lines requested as level-low: `videocodec_kernel_driver_D1.c:2272`
  (`request_irq(VDEC_IRQ_ID, ..., IRQF_TRIGGER_LOW, ...)`) and `:2279` (VENC).
* Interrupts come from DT: `videocodec_kernel_driver_D1.c:2425,2434`
  (`irq_of_parse_and_map`) from the downstream DTS
  `arch/arm/boot/dts/mt6735.dtsi:1350-1354`
  `interrupts = <GIC_SPI 179 IRQ_TYPE_LEVEL_LOW>` (vdec_gcon) and
  `:1399-1403` `interrupts = <GIC_SPI 180 IRQ_TYPE_LEVEL_LOW>` (venc_gcon).

### 2.2 ROM

`vcodec_pm_restore_noirq` @ `0xc0396198` (`llvm-nm -n vmlinux.elf`, symbol list
`$M/zip/symbols.txt`). Disassembly
(`llvm-objdump --triple=armv7a-linux-gnueabi -d --start-address=0xc0396198 --stop-address=0xc03961e8`):

```
c03961ac: mov r1, #1                 ; MT_LEVEL_SENSITIVE
c03961b0: ldr r0, [r4, #0x10]        ; VDEC_IRQ_ID
c03961b4: bl  0xc02eae48 <mt_irq_set_sens>
c03961bc: mov r1, #0                 ; MT_POLARITY_LOW
c03961c0: bl  0xc02ea84c <mt_irq_set_polarity>
c03961c8: ldr r0, [r4, #0xc]         ; VENC_IRQ_ID
c03961cc: bl  0xc02eae48 <mt_irq_set_sens>
c03961d4: mov r1, #0
c03961d8: bl  0xc02ea84c <mt_irq_set_polarity>
c03961dc: mov r0, #0                 ; return 0
```

Symbols: `mt_irq_set_sens @ 0xc02eae48`, `mt_irq_set_polarity @ 0xc02ea84c`
(`llvm-nm -n`). The order and argument values match the downstream source exactly;
both engines are re-armed and the callback always returns 0.

### 2.3 Nura before the patch

* No `register_swsusp_restore_noirq_func`, no `exec_swsusp_restore_noirq_func`,
  no `mt_irq_set_sens`/`mt_irq_set_polarity` anywhere in the tree
  (`grep -rn` over `*.c`/`*.h` returns only the two comment lines added by this
  patch). There is no MTK hibernation DPM framework to hook into.
* Neither engine driver had any `dev_pm_ops`; `.driver.pm` was unset, so no
  callback ran in the hibernation restore path and the INTPOL register was
  whatever the boot loader/restore kernel left behind.
* Standard hibernation is enabled: `nura.config:575 CONFIG_HIBERNATION=y`,
  `:581 CONFIG_PM_SLEEP=y`, `:587 CONFIG_PM=y`.

---

## 3. Parity matrix

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| VDEC IRQ trigger after hibernation restore | `vcodec_pm_restore_noirq` @ `0xc0396198`: `mt_irq_set_sens(VDEC_IRQ_ID, MT_LEVEL_SENSITIVE(1))` + `mt_irq_set_polarity(VDEC_IRQ_ID, MT_POLARITY_LOW(0))` | never re-applied; no PM callback | `vcodec_vdec_restore_noirq()` → `irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW)` | done |
| VENC IRQ trigger after hibernation restore | same callback @ `0xc03961c8-0xc03961d8` | never re-applied | `vcodec_venc_restore_noirq()` → `irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW)` | done |
| Failure handling | no check, always returns 0 | n/a | `dev_err()` on error, returns 0 (PM core only logs resume-phase errors anyway) | done |
| IRQ mask state across restore | ROM does not touch it | n/a | callback does not touch it; depth is preserved by the irq core (see §3.1) | done |
| Restore phase | MTK `exec_swsusp_restore_noirq` hook (equivalent point in the restore flow) | n/a | `.restore_noirq` of `dev_pm_ops` (PMSG_RESTORE) | done |
| s2ram / runtime PM | not applicable (ROM also only has the hib hook) | n/a | untouched (non-goal) | n/a |

### 3.1 Why `.restore_noirq` and why no mask bookkeeping is needed

**Phase/context (verified in this tree).**

* Hibernation restore in the image kernel resumes through
  `kernel/power/hibernate.c:482` `msg = in_suspend ? ... : PMSG_RESTORE;` followed by
  `dpm_resume(msg)`.
* `dpm_resume()` → `dpm_resume_noirq(state)` (`drivers/base/power/main.c:881`),
  which walks `dpm_noirq_list` in `dpm_noirq_resume_devices()` (`main.c:825-869`,
  `dpm_list_mtx` dropped around each callback) and calls
  `device_resume_noirq()` (`main.c:727`). There
  `pm_noirq_op()` maps `PM_EVENT_RESTORE` to `ops->restore_noirq`
  (`main.c:449-450`), and the driver callback is selected at `main.c:795`.
* `dev->power.is_noirq_suspended` (`main.c:740`) is set by
  `device_suspend_noirq()` (`main.c:1482`, the `Skip:` label) even when the driver
  has no `freeze_noirq` callback, so the image carries `is_noirq_suspended == true`
  and the restore callback is guaranteed to be invoked. `dev_pm_skip_resume()`
  always returns false for `PM_EVENT_RESTORE` (`main.c:611-616`).
* Context: process context, run by the task executing the hibernation transition
  (`hibernation_snapshot()` → `dpm_resume()`); local IRQs enabled; `dpm_list_mtx`
  not held during the callback; **device IRQs are still suspended** — the irq core
  only calls `resume_device_irqs()` after *all* `->restore_noirq` callbacks
  (`main.c:883`, immediately after `dpm_noirq_resume_devices()`). This is the
  correct window: the line is reconfigured while masked, before handlers can run,
  and `pm.h:219-222` states the driver IRQ handler is guaranteed not to run during
  `->restore_noirq`.
* Sleeping: noirq callbacks must not sleep. `irq_set_irq_type()`
  (`kernel/irq/chip.c:61-67`) takes the descriptor via
  `scoped_irqdesc_get_and_buslock()`; the codec interrupt is routed through
  `mediatek,mt6735-sysirq` (Nura `arch/arm64/boot/dts/mediatek/mt6735.dtsi:30`
  `interrupt-parent = <&sysirq>`), whose chip (`drivers/irqchip/irq-mtk-sysirq.c:60-69`)
  registers no `irq_bus_lock`/`irq_bus_sync_unlock`, so no mutex is taken.
  `mtk_sysirq_set_type()` (`irq-mtk-sysirq.c:26-58`) is MMIO + `raw_spin_lock_irqsave`,
  and it forwards `IRQ_TYPE_LEVEL_HIGH` to the GIC. `__irq_set_trigger()`
  (`kernel/irq/manage.c:925-980`) likewise only uses raw spinlocks. The call is
  therefore non-sleeping and legal in the noirq phase.
* Semantics: `IRQ_TYPE_LEVEL_LOW` on the sysirq domain sets the INTPOL bit
  (`irq-mtk-sysirq.c:40-50`: level-low → set bit, then parent type `LEVEL_HIGH`),
  i.e. exactly `MT_LEVEL_SENSITIVE` + `MT_POLARITY_LOW`. This matches the DT
  fragment's `GIC_SPI 179/180 IRQ_TYPE_LEVEL_LOW`.
  Note: a direct-GIC routing of these SPIs would reject `IRQ_TYPE_LEVEL_LOW`
  (`drivers/irqchip/irq-gic.c:301-304` only accepts `LEVEL_HIGH`/`EDGE_RISING`
  for SPIs); the error is logged by the callback. The MT6735 DTS routes them
  through sysirq, so this is not hit.

**Mask state / disable depth.**

* At image creation the engines' lines are already suspended by
  `suspend_device_irqs()` (`kernel/irq/pm.c:126-141`), which for each action'd,
  non-wakeup line sets `IRQS_SUSPENDED` and calls `__disable_irq()`
  (`pm.c:94-96`); `__disable_irq()` increments `desc->depth`
  (`kernel/irq/manage.c:675-679`).
* The whole `irq_desc` array (including `depth` and `istate`) is part of the
  hibernation image and is reinstated by `swsusp_arch_resume()`; the restored
  kernel therefore resumes with `IRQS_SUSPENDED` set and `depth` = pre-suspend
  depth + 1.
* After all `->restore_noirq` callbacks, `resume_device_irqs()`
  (`kernel/irq/pm.c:246-249`) → `resume_irq()` (`pm.c:144-172`) clears
  `IRQS_SUSPENDED` and calls `__enable_irq()`, which for `depth == 1` unmasks and
  for `depth > 1` only decrements (`kernel/irq/manage.c:769-799`). A line the
  engine had masked with `disable_irq()` before hibernation (probe does this, and
  the core only unmasks from `LOCKHW`) therefore comes back still masked, and an
  unmasked line comes back unmasked.
* Conclusion: the disable depth survives the restore and the engines' existing
  `enable_irq`/`disable_irq` ops remain the only owners of mask state. No local
  bool was added, and the new callback deliberately does not touch mask state.

---

## 4. Compile test

Environment: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, worktree
`$M/worktrees/vcodec-gap-hibernation`, `cp $M/nura.config .config`.

```
make ARCH=arm64 LLVM=1 olddefconfig                     # exit 0
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/misc/mediatek/vcodec/vcodec-core.o \
      drivers/misc/mediatek/vcodec/vcodec-vdec.o \
      drivers/misc/mediatek/vcodec/vcodec-venc.o
```
Result: exit 0; `CC drivers/misc/mediatek/vcodec/vcodec-core.o`,
`vcodec-vdec.o`, `vcodec-venc.o`; no warnings.

```
# after touching all three sources
same make line with W=1
```
Result: exit 0; all three objects recompiled (`CC` lines above); **0 warnings**.

No full kernel build was run.

---

## 5. Inferred / not hardware-verified

* The hibernation restore of the MT6735 `INTPOL` register contents is assumed to be
  lost across the power-down (the reason the ROM re-applies it). Re-applying is
  harmless when it was not lost; the exact register reset behaviour is
  `[INFERENCE]` from the ROM's existence of the callback, not measured.
* `IRQ_TYPE_LEVEL_LOW` ↔ `MT_LEVEL_SENSITIVE + MT_POLARITY_LOW` equivalence is
  read from `irq-mtk-sysirq.c:26-58`; it was not exercised on a device. If a board
  ever routes these SPIs directly to the GIC, `irq_set_irq_type()` returns
  `-EINVAL` and only a `dev_err()` is emitted (the GIC cannot express low-level
  SPIs).
* The callback runs for both engines only once the codec DT nodes exist
  (binding + fragment owned by the core leaf); no DTS node is present in this
  worktree, so the path is compile-tested only.
* Ordering guarantee relied on: `resume_device_irqs()` runs after every
  `->restore_noirq` (verified in source, not at runtime).
* No hibernation image was created or resumed (no phone attached).

---

## 6. DTS-FRAGMENT

DTS-FRAGMENT: none

---

## 7. Patch path

`$M/patches/vcodec-gap-hibernation.patch`
