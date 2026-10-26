# vcodec-core: MT6735 video codec hardware service core

## Worktree

`/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/worktrees/vcodec-core`
(detached HEAD `046883c0c`, Nura linux arm64)

Patch: `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/patches/vcodec-core.patch`

## Files changed (all new except the two hook files)

| File | State |
|---|---|
| `drivers/misc/mediatek/vcodec/vcodec-core.c` | new, 1159 lines |
| `include/uapi/linux/vcodec.h` | new, 227 lines |
| `Documentation/devicetree/bindings/media/mediatek,mt6735-vcodec.yaml` | new |
| `drivers/misc/mediatek/Kconfig` | new (scaffolded hook, included per assignment) |
| `drivers/misc/mediatek/Makefile` | new (scaffolded hook) |
| `drivers/misc/mediatek/vcodec/Kconfig` | new (scaffolded hook) |
| `drivers/misc/mediatek/vcodec/Makefile` | new (scaffolded hook) |
| `drivers/misc/Kconfig` | modified: `source "drivers/misc/mediatek/Kconfig"` |
| `drivers/misc/Makefile` | modified: `obj-y += mediatek/` |

Not in the patch (coordinator-owned): `drivers/misc/mediatek/vcodec/vcodec.h`,
`vcodec-vdec.c`, `vcodec-venc.c`, any DTS.

## Design summary

`vcodec-core.c` owns the whole userspace-facing service:

* engine registry (`list_head` + `vcodec_engines_lock`), with
  `vcodec_engine_register()`/`vcodec_engine_unregister()` initialising the
  per-engine mutex, the two wait queues and the ISR spinlock;
* the `/dev/Vcodec` misc device (`VCODEC_DEVNAME`, mode 0660) with
  `open/release/unlocked_ioctl/compat_ioctl/mmap`;
* the hardware ownership lock, the interrupt wait, the EMI/PWR accounting,
  the CPU load queries, and the register-window mmap whitelist;
* `vcodec_engine_irq()`, `vcodec_engine_is_locked()`,
  `vcodec_engine_get_driver_type()` for the two engine ISRs;
* `module_init/module_exit` (the only module macros in the module) which
  register the misc device and then call `vcodec_vdec_register()` /
  `vcodec_venc_register()`, unwinding in reverse on failure.

Locking: `vcodec_engines_lock` (registry/mmap/cleanup) -> `eng->lock`
(per-engine state) is the only nesting order. The ISR path only takes
`eng->irq_lock` (spinlock, irqsave) and never sleeps, so `ops->disable_irq()`
in process context cannot deadlock against it.

### LOCKHW commit ordering

`power_on()` -> record `owner`/`driver_type` -> `smp_store_release(hw_locked,
true)` -> `enable_irq()`. The owner is published *before* the interrupt line
is unmasked because a level-low completion may already be pending; if the
engine ISR saw `hw_locked == false` it would treat the completion as spurious
and ack every status bit, destroying a legitimate interrupt. Waiters only
observe `hw_locked`, so publishing early is safe. If `enable_irq()` fails the
transition is undone under the same mutex (owner/type cleared, `hw_locked`
cleared, `power_off()`, waiters woken) and the error is returned.

The JPEG encoder is special-cased: the lock is recorded (owner/type/
`hw_locked`) but the engine is neither powered nor unmasked, exactly like
downstream's `H264_ENC/HEVC_ENC`-only power block.

### CONFIG_COMPAT=n

The compat structs, the three compat translation helpers and
`vcodec_compat_ioctl()` live inside one `#if IS_ENABLED(CONFIG_COMPAT)` block,
and the `.compat_ioctl` initialiser is guarded the same way, so the object
builds with `CONFIG_COMPAT=n` (no unused statics, no missing reference).

### 32-bit payload handling

`GET_CORE_LOADING` and `SET_THREAD_ID` are plain u32/s32 payloads and are
copied whole. `GET_CPU_LOADING_INFO` is the only pointer-free compat payload
with 64-bit members, so it has its own struct
(`struct vcodec_compat_cpu_loading_info`: three `u64` then one `u32`) and is
written field by field. This keeps the write independent of the ABI's tail
padding: a check with `clang --target=arm-linux-gnueabi` and
`--target=arm-linux-gnueabihf -mabi=aapcs-linux` puts `sizeof` of the 32-bit
shape at 32 bytes (three 8-byte members + `u32` + 4 bytes of ABI padding), so
the caller's buffer has room either way, but the driver now never touches the
padding word and cannot grow a write past the caller's struct if the padding
disappears.

## Downstream evidence

All paths relative to
`src/downstream-grandpplte/drivers/misc/mediatek/videocodec/`.

| Behaviour | Downstream location |
|---|---|
| ioctl numbers, magic `'M'` | `mt6735/videocodec_kernel_driver.h:19-52` |
| driver type enum order/values | `include/val_types_public.h:135-170` |
| `VAL_HW_LOCK_T`, `VAL_POWER_T`, `VAL_ISR_T` field order | `include/val_types_public.h:139-147, 414-445` |
| `VAL_VCODEC_CORE_LOADING_T`, `_CPU_LOADING_INFO_T`, `_CPU_OPP_LIMIT_T`, `_THREAD_ID_T` | `include/val_types_private.h:221-224`, `include/val_types_public.h:256-292` |
| `IRQ_STATUS_MAX_NUM`, `VCODEC_THREAD_MAX_NUM` = 16 | `include/val_types_public.h:23-24` |
| device name `"Vcodec"` | `mt6735/videocodec_kernel_driver_D1.c:266` |
| decode driver-type list | `D1.c:735-742` (LOCKHW), `1130-1137` (UNLOCK), `1258-1265` (WAITISR) |
| encode driver-type list (H264/HEVC/JPEG) | `D1.c:886-888`, `1110-1111` |
| JPEG: lock recorded without power/unmask | `D1.c:953-957`, `971-977` |
| `enc_isr()` treats a JPEG holder as invalid | `D1.c:664-667` |
| WAITISR knows only dec + H264/HEVC enc | `D1.c:1233-1236` |
| HW lock wait, 1000 ms default | `D1.c:744-800`, `886`, `915` |
| lock acquisition + power/irq enable | `D1.c:820-845` (dec), `948-978` (enc) |
| unlock: owner check, irq off, power off | `D1.c:1155-1180`, `1187-1210` |
| WAITISR ownership check, timeout, enc status copy-back | `D1.c:1250-1395`, status at `1183-1197` |
| WAITISR dispatcher maps every failure to `-EFAULT` | `D1.c:1443-1448` |
| EMI counters (enc/dec) | `D1.c:1276-1358` |
| PWR user (L2C block compiled out) | `D1.c:1360-1450` |
| `VCODEC_GET_CPU_LOADING_INFO` (copies uninitialised stack) | `D1.c:1595-1620` |
| `VCODEC_GET_CORE_LOADING` / `VCODEC_GET_CORE_NUMBER` | `D1.c:1622-1680` |
| `VCODEC_SET_CPU_OPP_LIMIT` (cpu_opp_limit commented out) | `D1.c:1682-1705` |
| `VCODEC_MB`, `INITHWLOCK`, `DEINITHWLOCK` | `D1.c:1468-1490, 1707-1710` |
| unknown command -> `0xFF` | `D1.c:1712-1716` |
| 32-bit struct layouts | `D1.c:1672-1740` |
| compat translation | `D1.c:1742-2030` |
| open / release / last-close reset | `D1.c:2035-2158` |
| mmap (4 hardcoded physical windows + pgprot_noncached) | `D1.c:2158-2200` |
| IRQ request + initial `disable_irq()` | `D1.c:2272-2290` |
| clock acquisition/enable order | `D1.c:402-427`, `451-455` (VDEC), `485-504`, `530-534` (VENC) |
| MMDVFS monitor (compiled in downstream) | `D1.c:83-262`, `772-776`, `1176-1179` |
| `pmem_user_v2p_video()` PTE walk | `videocodec_kernel.c:42-90` |

## Parity matrix

Status: done / partial / deferred.

| # | Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|---|
| 1 | `/dev/Vcodec` node | `cdev` on major 160 + class + device_create (`D1.c:2205-2265`); ueventd sets node | nothing | `misc_register()`, dynamic minor, mode 0660 | done |
| 2 | open counting | `Driver_Open_Count` + mutex (`D1.c:2035-2048`) | nothing | same accounting under `vcodec_open_lock` | done |
| 3 | last-close forced idle | clears owner/type/time, EMI/PWR/L2C/ISR/lock counters (`D1.c:2050-2158`) | nothing | `vcodec_cleanup_engines()`: irq off, power off, owner/type/status cleared, wakes lock waiters; EMI/PWR/CPU-load counters reset | done (deviation D9 for SYSRAM) |
| 4 | LOCKHW (decoder types) | waits on event, 1000 ms, never steals (`D1.c:688-856`) | nothing | `wait_event_interruptible_timeout()` on `hwlock_wq`, default 1000 ms, `-ETIMEDOUT` on expiry, remaining budget kept across spurious wakeups | done |
| 5 | LOCKHW (encoder types) | waits; H264/HEVC power on + unmask, JPEG only records the handle (`D1.c:857-1075`, guard at `971-977`) | nothing | same lists; H264/HEVC power on + unmask, JPEG records owner/type only | done |
| 6 | secure instance | `bSecureInst` skips `enable_irq()` | nothing | same; unlock masks only if the core actually unmasked it | done (D8) |
| 7 | UNLOCKHW | owner-only, else `-EFAULT`; disable irq, power off, wake (`D1.c:1120-1243`) | nothing | owner-only, else `-EPERM`; disable irq if enabled, power off, wake all | done (D5) |
| 8 | WAITISR | owner check, wait event, timeout -> internal `-2` -> dispatcher `-EFAULT`; enc copies `u4IrqStatus[0]`; JPEG is "Unknown instance" (`D1.c:1244-1395`, `1443-1448`) | nothing | `vcodec_ioc_waitisr()`: seq snapshot + `wait_event_interruptible_timeout()`; timeout `-EFAULT` (parity), signal `-ERESTARTSYS` (documented improvement), JPEG `-EINVAL`, enc status copied back when `irq_status_num > 0` | done (D6) |
| 9 | INC/DEC `{ENC,DEC}_EMI_USER` | per-codec u32 counters, value copied back (`D1.c:1276-1358`) | nothing | per-codec counters under `vcodec_emi_lock`, value copied with `put_user` | done (D3) |
| 10 | INC/DEC `PWR_USER` | refcounts an L2C user; `config_L2()` inside `#ifdef VENC_USE_L2C`, undefined in this ROM | nothing | per-engine power-user counter, no hardware action, `-EINVAL` for an unknown driver type | done |
| 11 | `GET_CORE_NUMBER` | `copy_to_user(nr_cpu_ids)` (`D1.c:1655-1668`) | nothing | same | done |
| 12 | `GET_CORE_LOADING` | `get_cpu_load()` (not in Nura) with an off-by-one CPU bound (`D1.c:1622-1653`) | nothing | per-CPU load from `kcpustat` deltas since the previous query, first call 0, `0..nr_cpu_ids-1` validated | done (D10) |
| 13 | `GET_CPU_LOADING_INFO` | copies an uninitialised stack struct (`D1.c:1595-1620`) | nothing | zero-filled, `sched_clock()` and locked-engine count filled, rest 0 | done (D4) |
| 14 | `SET_CPU_OPP_LIMIT` | `cpu_opp_limit()` commented out, returns success (`D1.c:1682-1705`) | nothing | logs at debug, returns `-EOPNOTSUPP` | done (D11) |
| 15 | `VCODEC_MB` | `mb()` (`D1.c:1707`) | nothing | `mb()` | done |
| 16 | `SET_THREAD_ID` | logs only (`D1.c:1257-1260`) | nothing | copies the struct, validates `thread_num <= 16`, logs at debug | done |
| 17 | `INITHWLOCK` / `DEINITHWLOCK` | logs only (`D1.c:1468-1490`) | nothing | accepted, debug log | done |
| 18 | `ALLOC/FREE_NON_CACHE_BUFFER` | prints "Don't support" and returns success (`D1.c:1262-1270`) | nothing | `-EOPNOTSUPP` | done (D2) |
| 19 | `*SYSRAM*`, `PMEM_FLUSH/CLEAN`, `ALLOC/FREE_MEM`, `MAKE_PMEM_TO_NONCACHED`, `ALLOC/FREE_INT_MEM`, `INIT/DEINIT/TEST/SET_CMD/SET_PWR/SET_ISR` | not handled -> `0xFF` (`D1.c:1712-1716`) | nothing | `-EOPNOTSUPP` | done (D2, D9) |
| 20 | mmap of codec registers | any address inside 4 hardcoded windows, `pgprot_noncached`, `remap_pfn_range` (`D1.c:2158-2200`) | nothing | only ranges fully inside a region an engine registered, `pgprot_noncached`, `remap_pfn_range`, else `-EINVAL` | done (D1) |
| 21 | compat ioctl (32-bit) | `compat_copy_struct()` + `compat_alloc_user_space()` for HW_LOCK/POWER/ISR/MEMORY (`D1.c:1667-2030`) | nothing | field-by-field `get_user`/`put_user` on 32-bit layouts; LOCKHW, UNLOCKHW, WAITISR, PWR_USER, EMI, GET_CORE_LOADING, GET_CPU_LOADING_INFO (dedicated 3x u64 + u32 struct, field-wise), SET_THREAD_ID, MB; rest `-ENOTTY` | done |
| 22 | engine ISR completion | `dec_isr()`/`enc_isr()` ack HW and set an event (`D1.c:544-681`) | nothing | core provides `vcodec_engine_irq(eng,status)` (status + seq under spinlock, wakes `isr_wq`); the engines own the ack | done |
| 23 | register windows exposed to userspace | `VENC_BASE` 0x17002000, `VDEC_BASE`/`GCON`, `HW_BASE`, `INFO_BASE` | nothing | regions come from the engine DT nodes (`vdec`, `gcon`, `venc`) | done |
| 24 | MMDVFS (decode clock scaling) | `ENABLE_MMDVFS_VDEC` on: `mmdvfs_set_step()`, VDEC mux switch, 4 s monitor in LOCKHW/UNLOCK (`D1.c:83-262, 772-776, 1176-1179`) | nothing | not implemented; no MMDVFS/`mt_smi` API in Nura and the VDEC mux is a plain clock in the engine node | deferred (D12) |
| 25 | VENC L2C reconfiguration | inside `#ifdef VENC_USE_L2C`, **undefined** in this ROM | - | no action, identical | done |
| 26 | hibernation IRQ re-sensitivity | `CONFIG_MTK_HIBERNATION` restore-noirq hook (`D1.c:2320-2336`) | - | not needed; trigger is described in DT (`IRQ_TYPE_LEVEL_LOW`) and re-applied by the irqchip restore | deferred (documented) |

## ioctl status

Fully implemented: `VCODEC_LOCKHW`, `VCODEC_UNLOCKHW`, `VCODEC_WAITISR`,
`VCODEC_INC_ENC_EMI_USER`, `VCODEC_DEC_ENC_EMI_USER`,
`VCODEC_INC_DEC_EMI_USER`, `VCODEC_DEC_DEC_EMI_USER`,
`VCODEC_INC_PWR_USER`, `VCODEC_DEC_PWR_USER`, `VCODEC_GET_CORE_NUMBER`,
`VCODEC_GET_CORE_LOADING`, `VCODEC_GET_CPU_LOADING_INFO`,
`VCODEC_SET_THREAD_ID`, `VCODEC_INITHWLOCK`, `VCODEC_DEINITHWLOCK`,
`VCODEC_MB` (native and compat).

`-EOPNOTSUPP`: `MFV_TEST_CMD`, `MFV_INIT_CMD`, `MFV_DEINIT_CMD`,
`MFV_SET_CMD_CMD`, `MFV_SET_PWR_CMD`, `MFV_SET_ISR_CMD`,
`MFV_ALLOC_MEM_CMD`, `MFV_FREE_MEM_CMD`, `MFV_MAKE_PMEM_TO_NONCACHED`,
`MFV_ALLOC_INT_MEM_CMD`, `MFV_FREE_INT_MEM_CMD`, `VCODEC_PMEM_FLUSH`,
`VCODEC_PMEM_CLEAN`, `VCODEC_INC_SYSRAM_USER`, `VCODEC_DEC_SYSRAM_USER`,
`VCODEC_SET_SYSRAM_INFO`, `VCODEC_GET_SYSRAM_INFO`,
`VCODEC_ALLOC_NON_CACHE_BUFFER`, `VCODEC_FREE_NON_CACHE_BUFFER`,
`VCODEC_SET_CPU_OPP_LIMIT`.

`-ENOTTY`: every other command (native and compat).

## Intentional deviations from the ROM driver

* **D1 — mmap hardening.** Downstream accepted any address inside four
  hardcoded physical windows (`VENC_BASE`, `VDEC_BASE`, `HW_BASE`
  0x07fff000, `INFO_BASE` 0x10000000), so userspace could map unrelated
  MMIO. Here a mapping must lie completely inside a region that an engine
  registered from its DT node, and the whole VMA is checked (not just its
  first page). Rejection is `-EINVAL` instead of downstream's `-EAGAIN`
  (which userspace mistakes for "retry").
* **D2 — no fake success.** Commands downstream accepted and ignored
  (non-cache buffer alloc/free, INIT/DEINIT/TEST/SET_*) or answered with
  `0xFF` return `-EOPNOTSUPP` / `-ENOTTY`, so a HAL can detect that the
  service is unavailable instead of believing a no-op succeeded.
* **D3 — counter saturation.** `DEC_*_EMI_USER` clamps at 0; downstream
  underflows an unsigned counter to 0xFFFFFFFF. The value copied back is
  therefore never a stale huge number.
* **D4 — uninitialised stack fix.** `GET_CPU_LOADING_INFO` zero-fills the
  struct before filling `_sched_clock` and `_inst_count`; downstream copied
  kernel stack garbage to userspace (`D1.c:1595-1620`).
* **D5 — error codes for the ownership failures.** Non-owner unlock/waitisr
  returns `-EPERM` (ROM: `-EFAULT`), lock timeout returns `-ETIMEDOUT`
  (ROM: `-EFAULT`). The downstream dispatcher collapsed every failure to
  `-EFAULT`, which made "busy" indistinguishable from "bad pointer".
  `VCODEC_WAITISR` keeps `-EFAULT` for a timeout (see D6) because that is
  what the vendor libraries actually saw.
* **D6 — WAITISR return values.** Timeout returns `-EFAULT`, matching what
  downstream's dispatcher exposed to userspace (`D1.c:1443-1448`) even
  though its inner helper used `-2`. Signal interruption returns
  `-ERESTARTSYS` instead of `-EFAULT`, a deliberate improvement so libc can
  restart the call; downstream logged "RESTARTSYS" and then flattened it.
  `JPEG_ENC` is rejected with `-EINVAL`; downstream's WAITISR only knows the
  decode types and H264/HEVC and reported "Unknown instance" as `-EFAULT`
  (`D1.c:1233-1236`) — a stricter errno for the same rejection.
* **D7 — LOCKHW commit ordering (hardware-visible fix).** The owner and
  driver type are published *before* `enable_irq()` rather than after. With
  a level-low shared completion line a pending interrupt can arrive the
  moment the line is unmasked; publishing first guarantees the engine ISR
  sees `vcodec_engine_is_locked()`/`get_driver_type()` as valid instead of
  acking a real completion as spurious. Downstream had the same ordering
  (handle/type recorded before `enable_irq()`, `D1.c:966-977`).
* **D8 — secure unlock.** Unlock masks the IRQ based on the recorded
  `irq_enabled` state instead of re-reading `bSecureInst`, so a secure
  instance can never leave a Linux-visible interrupt enabled, and a JPEG
  holder (never unmasked) is not disabled twice.
* **D9 — no SYSRAM counters.** The SYSRAM ioctls have no handler in the ROM
  either (they fell into the `default:` branch), so there is no counter to
  reset on last close; the reset covers the EMI, PWR and CPU-load counters
  that this driver actually maintains.
* **D10 — CPU bounds and load source.** Downstream compared
  `CPUid > num_possible_cpus()` (off-by-one, allows `nr_cpu_ids`), and used
  `get_cpu_load()`, which has no counterpart in Nura. Here the bound is
  `0 .. nr_cpu_ids-1` and the value is a real per-CPU busy percentage from
  `kcpustat_cpu_fetch()`/`kcpustat_field()` deltas; the first query of a CPU
  returns 0 because there is no baseline yet.
* **D11 — `SET_CPU_OPP_LIMIT` returns `-EOPNOTSUPP`.** The MTK
  `cpu_opp_limit()` API does not exist in Nura; the ROM never called it
  either (commented out) but reported success.
* **D12 — MMDVFS not implemented.** The downstream VDEC clock scaling
  (`mmdvfs_set_step()`, `SMI_BWC_SCEN_VP`, VDEC mux switching) needs the MTK
  SMI/MMDVFS framework, which does not exist here. LOCKHW/UNLOCKHW therefore
  do not touch the VDEC mux; the engine node keeps `CLK_TOP_VDEC_SEL` at its
  DT-provided parent. Hardware-unverified consequence: no dynamic VDEC
  voltage/frequency scaling, functional decode unaffected.
* **D13 — engine selection lists are exactly the ROM ones.** Decode:
  `MP4_DEC`, `HEVC_DEC`, `H264_DEC`, `MP1_MP2_DEC`, `VC1_DEC`,
  `VC1_ADV_DEC`, `VP8_DEC`. Encode: `H264_ENC`, `HEVC_ENC`, `JPEG_ENC`.
  Other decode types (`H264_DEC_MAIN`, `VP9_DEC`, `MP2_DEC`, ...) are
  rejected with `-EINVAL` just as the ROM rejected them with `-EFAULT` and
  never selected an engine.

Known behavioural note (not a deviation): the lock is a process-context
mutex + wait queue, so a waiter woken spuriously keeps only the remaining
part of its timeout budget instead of restarting the full 1000 ms.

## Compile test

```sh
cd /run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/worktrees/vcodec-core
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/vcodec/vcodec-core.o
```

Result:

```
  CC      drivers/misc/mediatek/vcodec/vcodec-core.o
```

Zero warnings, also with `W=1`:

```
touch drivers/misc/mediatek/vcodec/vcodec-core.c
make ARCH=arm64 LLVM=1 W=1 -j2 drivers/misc/mediatek/vcodec/vcodec-core.o
# no diagnostics
```

`perl scripts/checkpatch.pl --no-tree --strict -f` on both new source files:

```
vcodec-core.c: total: 0 errors, 0 warnings, 0 checks, 1159 lines checked
vcodec.h (uapi): total: 0 errors, 0 warnings, 0 checks, 227 lines checked
```

Note: `CONFIG_MTK_VCODEC_MT6735` is *not* set in the pre-existing `.config`
(untouched). The explicit `.o` target compiles the file regardless;
`vcodec-vdec.o`/`vcodec-venc.o` do not exist in this worktree, so linking the
module was not attempted (engines are separate tasks). The
`CONFIG_COMPAT=n` variant of this file cannot be built without editing
`.config`; its portability rests on the single guard block described above
(all compat types/helpers/function and the `.compat_ioctl` initialiser are
inside `#if IS_ENABLED(CONFIG_COMPAT)`).

## Exported kernel symbols used

All are exported in this tree, so the single module links against them:

| Symbol | Export |
|---|---|
| `kcpustat_cpu_fetch` | `EXPORT_SYMBOL_GPL`, `kernel/sched/cputime.c:1098` |
| `kcpustat_field` | `EXPORT_SYMBOL_GPL`, `kernel/sched/cputime.c:1010` |
| `sched_clock` | `EXPORT_SYMBOL_GPL`, `kernel/sched/clock.c:70` |
| `nr_cpu_ids` | `EXPORT_SYMBOL`, `kernel/smp.c:982` |
| `pin_user_pages_fast` | `EXPORT_SYMBOL_GPL`, `mm/gup.c:3317` |
| `unpin_user_page` | `EXPORT_SYMBOL`, `mm/gup.c:190` |
| `remap_pfn_range` | `EXPORT_SYMBOL`, `mm/memory.c:3166` |
| `misc_register` / `misc_deregister` | `EXPORT_SYMBOL`, `drivers/char/misc.c:274` |

`page_to_phys()`/`offset_in_page()` are inline helpers from
`asm-generic/memory_model.h` / `linux/mm.h`, not symbols.

The engine-facing helpers (`vcodec_engine_register/unregister/irq/
is_locked/get_driver_type`) are intentionally **not** exported: the three
objects are one module.

## Inferred / unverified

* `[INFERENCE]` The encoder status word copied back to userspace is whatever
  the VENC engine ISR passes to `vcodec_engine_irq()`. The core only stores
  and forwards it; the value semantics (SPS/PPS/FRAME bits at 0x05C) belong
  to `vcodec-venc.c`. Not exercised on hardware.
* `[INFERENCE]` The publish-before-unmask ordering assumes the engine ISR
  reads the owner/type through `vcodec_engine_is_locked()`/
  `vcodec_engine_get_driver_type()` (as `vcodec-venc.c` does); it is correct
  regardless of whether the engine also reads them via `eng->priv`.
* `[INFERENCE]` `vcodec_user_v2p()` pins the handle page to build the owner
  identity (downstream walked the page tables). Pinning can transiently fail
  under memory pressure, in which case the raw user pointer value is used as
  the identity; owner comparisons stay self-consistent because every call
  site uses the same translation at the same moment. If a handle spanned a
  page boundary the offset is preserved, so the identity is still stable.
* `[INFERENCE]` `power_on()` is idempotent-safe when called after a forced
  cleanup, per the `vcodec.h` contract.
* The 32-bit compat layouts are transcribed from the ROM's 32-bit structs;
  the native 64-bit layouts are new by construction (no 64-bit ROM to
  compare with). The ioctl *numbers* are byte-identical to the ROM because
  the payload-size field is `sizeof(u32)` in both.
* `dt_binding_check` could **not** be run: `dt-doc-validate`/dtschema is not
  installed in this environment. The YAML was validated for syntax
  (PyYAML) and every referenced `dt-bindings` header/symbol was checked to
  exist in the tree (`mt6735-mmsys.h`, `mt6735-vdecsys.h`,
  `mt6735-vencsys.h`, `mt6735-topckgen.h`, `mt6735-power.h`,
  `mediatek,mt6735-vdecsys.h` reset ids).
* No hardware was available; nothing in this driver was run on a device.

## DTS fragment (documentation only; DTS not edited)

Matches the binding example. Clock order matters because both engine
drivers use `devm_clk_bulk_get_all()`, which enables in DT order and disables
in reverse order; the lists therefore follow the downstream enable sequence:
VDEC -> `SMI_COMMON, VDEC0_VDEC, VDEC1_LARB` (`D1.c:408-427`, disable
`D1.c:451-455`) plus the topckgen VDEC mux last, VENC ->
`SMI_COMMON, VENC_VENC, VENC_LARB` (`D1.c:485-504`, disable `D1.c:530-534`).
`vdec` requires the two `reg-names`; `venc` uses `reg` index 0. The SYS_DIS /
SYS_VDE / SYS_VEN "clocks" of the ROM are power domains here and appear in
`power-domains`.

```dts
&soc {
	vdec: vcodec@16020000 {
		compatible = "mediatek,mt6735-vdec";
		reg = <0x16020000 0x10000>, <0x16000000 0x1000>;
		reg-names = "vdec", "gcon";
		interrupts = <GIC_SPI 179 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&mmsys CLK_MM_SMI_COMMON>,
			 <&vdecsys CLK_VDEC_VDEC>,
			 <&vdecsys CLK_VDEC_SMI_LARB1>,
			 <&topckgen CLK_TOP_VDEC_SEL>;
		clock-names = "smi-common", "vdec", "smi-larb1", "vdec-sel";
		power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
				<&spm MT6735_POWER_DOMAIN_VDE>;
		resets = <&vdecsys MT6735_VDEC_RST0_VDEC>;
		reset-names = "vdec";
	};

	venc: vcodec@17002000 {
		compatible = "mediatek,mt6735-venc";
		reg = <0x17002000 0x1000>;
		reg-names = "venc";
		interrupts = <GIC_SPI 180 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&mmsys CLK_MM_SMI_COMMON>,
			 <&vencsys CLK_VENC_VENC>,
			 <&vencsys CLK_VENC_SMI_LARB3>;
		clock-names = "smi-common", "venc", "smi-larb3";
		power-domains = <&spm MT6735_POWER_DOMAIN_DIS>,
				<&spm MT6735_POWER_DOMAIN_VEN>;
	};
};
```

Required includes: `dt-bindings/clock/{mediatek,mt6735-mmsys,
mediatek,mt6735-vdecsys, mediatek,mt6735-vencsys, mediatek,mt6735-topckgen}.h`,
`dt-bindings/power/mt6735-power.h`,
`dt-bindings/reset/mediatek,mt6735-vdecsys.h`, `arm-gic.h`.

## Cross-task observation (not my file)

`vcodec-venc.c` initially only called `pm_runtime_enable()`; in this kernel
`genpd_dev_pm_attach()` deliberately attaches nothing when a node has more
than one `power-domains` entry (`drivers/pmdomain/core.c:3279-3285` returns 0
for the multi-domain case), so DIS/VEN would have stayed off. Flagged to
VcodecVenc, who has since added `devm_pm_domain_attach_list(dev, NULL,
&pd_list)` (accepting `-EEXIST`, failing `-ENODEV` when no domain is
described) and recompiled clean. The two-domain node above is correct for
both engines; integration should still verify the encoder power path on
hardware, together with the VENC clock order stated above (the encoder's DT
fragment lists `VENC_VENC` before `VENC_LARB3`, matching this binding).
