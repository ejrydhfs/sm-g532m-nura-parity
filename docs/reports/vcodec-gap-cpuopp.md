# vcodec gap: VCODEC_SET_CPU_OPP_LIMIT

Date: 2026-10-24
Leaf: `VcodecCpuOpp`
Worktree: `$M/worktrees/vcodec-gap-cpuopp` (nura-linux `046883c0c` + `$M/patches/vcodec-integration.patch` staged)
Patch: `$M/patches/vcodec-gap-cpuopp.patch`

---

## 1. Files changed

```
drivers/misc/mediatek/vcodec/vcodec-core.c   (only file; 254 insertions, 1 deletion)
```

`git status --short` shows only `AM drivers/misc/mediatek/vcodec/vcodec-core.c` beyond the staged baseline
series (`git diff` = the change set).

What was added:

* `#include <linux/cpufreq.h>`, `#include <linux/pm_qos.h>`.
* `struct vcodec_cpu_opp { policy, freq_qos_request, visited }` table (`NR_CPUS` entries), one
  mutex, and:
  * `vcodec_cpu_opp_find()`, `vcodec_cpu_opp_free()` — policy/request table lookup and bounds;
  * `__vcodec_cpu_opp_release()` — update one request to `FREQ_QOS_MAX_DEFAULT_VALUE`,
    `freq_qos_remove_request()`, `cpufreq_cpu_put()`, clear slot;
  * `__vcodec_cpu_opp_release_all()` / `vcodec_cpu_opp_release_all()` — release every slot;
  * `vcodec_cpu_opp_enable()` — mask iteration over possible CPUs, one `FREQ_QOS_MAX` request per
    policy (`freq_qos_add_request()` new, `freq_qos_update_request()` existing), then release every
    active policy the call did not visit (`visited` flag) so a re-issue replaces the ceiling set;
  * `vcodec_cpu_opp_disable()`;
  * `vcodec_ioc_cpu_opp_limit()` — argument validation (including a negative `limited_cpu`);
    `-EOPNOTSUPP` build with CPU_FREQ=n.
* `VCODEC_SET_CPU_OPP_LIMIT` removed from `vcodec_unsupported_cmds[]` and dispatched in both
  `vcodec_unlocked_ioctl()` and `vcodec_compat_ioctl()` (`copy_from_user` of the three-s32 struct).
* `vcodec_cpu_opp_release_all()` at the last close in `vcodec_release()` (after
  `vcodec_cleanup_engines()`) and in `vcodec_core_exit()`.

No other ioctl behaviour, counter, engine contract, uapi header or DTS was touched.

---

## 2. Evidence

### 2.1 Downstream handler does nothing

`drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c:1533-1555`:

```
1533  case VCODEC_SET_CPU_OPP_LIMIT: {
1534      MODULE_MFV_LOGE("VCODEC_SET_CPU_OPP_LIMIT [EMPTY] + - tid = %d\n", ...);
1535      user_data_addr = (VAL_UINT8_T *)arg;
1536      ret = copy_from_user(&rCpuOppLimit, user_data_addr, sizeof(VAL_VCODEC_CPU_OPP_LIMIT_T));
...
1543      /* TODO: Check if cpu_opp_limit is available */
1544      /*
1545      ret = cpu_opp_limit(EVENT_VIDEO, rCpuOppLimit.limited_freq,
1546      rCpuOppLimit.limited_cpu, rCpuOppLimit.enable); // 0: PASS, other: FAIL
...
1551      */
```

The identical commented-out block exists in `videocodec_kernel_driver_D2.c:2138-2143` and
`videocodec_kernel_driver_D3.c:1623-1629`. Payload definition:
`videocodec/include/val_types_public.h:288-292` (`int limited_freq; int limited_cpu; int enable;`).

### 2.2 `cpu_opp_limit()` exists in no available source

```
$ grep -c "cpu_opp_limit" $M/zip/symbols.txt
0
$ grep -rn "cpu_opp_limit" $M/src/downstream-grandpplte
...D3.c:1623:  /* TODO: Check if cpu_opp_limit is available */
...D3.c:1625:  ret = cpu_opp_limit(...          (inside the /* ... */ block)
...D3.c:1628:  MODULE_MFV_LOGE("[VCODEC][ERROR] cpu_opp_limit failed: ...
...D2.c:2138-2143  (same, commented)
...D1.c:1543-1548  (same, commented)
$ grep -rln "cpu_opp_limit" $M/src/downstream-grandpplte --include=*.h
none in headers
```

All nine hits are inside comment blocks or are log strings; there is no declaration, definition or
export anywhere in the downstream tree, and no such ROM symbol. The ROM carries only the handler's
log strings (`$M/zip/vmlinux.strings:27338-27342`, e.g.
`3VCODEC_SET_CPU_OPP_LIMIT [EMPTY] + - tid = %d`), which confirms the "[EMPTY]" downstream code is
what shipped. The helper's argument semantics are therefore unknown; see §5.

### 2.3 Available CPU frequency QoS API in Nura

Nura uses cpufreq-dt (`$M/nura.config`: `CONFIG_CPU_FREQ=y`, `CONFIG_CPUFREQ_DT=y`,
`# CONFIG_ARM_MEDIATEK_CPUFREQ is not set`). The frequency ceiling is expressed on each policy's
maximum-frequency constraint:

* `kernel/power/qos.c:644` `freq_qos_add_request()` (export :667),
  `:680` `freq_qos_update_request()` (export :694), `:706` `freq_qos_remove_request()` (export :723);
  all `EXPORT_SYMBOL_GPL`. Value validation: `:540` `freq_qos_value_invalid()`.
* `include/linux/pm_qos.h:78` `#define FREQ_QOS_MAX_DEFAULT_VALUE S32_MAX`; `:80-96` enum/structs
  `freq_qos_req_type`, `freq_constraints`, `freq_qos_request`; `:313-317` prototypes.
* `include/linux/cpufreq.h:81` `struct freq_constraints constraints;` in `struct cpufreq_policy`;
  `:205-208` `cpufreq_cpu_get()` / `cpufreq_cpu_put()` (`EXPORT_SYMBOL_GPL` at
  `drivers/cpufreq/cpufreq.c:254,264`), with NULL stubs when `CONFIG_CPU_FREQ=n` (`:210-224`).
* Reference usage pattern: `drivers/thermal/cpufreq_cooling.c:580`,
  `drivers/acpi/processor_thermal.c:173` (`freq_qos_add_request(&policy->constraints, ...)`).

Policy lifetime safety of holding a request across policy teardown:
`drivers/cpufreq/cpufreq.c:247` `cpufreq_cpu_get()` takes a `kobject_get(&policy->kobj)`;
`cpufreq_policy_free()` (`:1340-1385`) only calls `cpufreq_policy_put_kobj()` and `kfree(policy)` at
the end, so the policy/constraints memory stays alive until our `cpufreq_cpu_put()`. Our release
order (update → remove request → put) is therefore safe even if the policy was torn down first.

---

## 3. Parity matrix

| Behaviour | ROM / downstream implementation | Nura before | After | Status |
|---|---|---|---|---|
| Accept `VCODEC_SET_CPU_OPP_LIMIT` (cmd 0x32) | D1:1533-1555 copies the struct and returns 0 | `-EOPNOTSUPP` (listed in `vcodec_unsupported_cmds`) | dispatched in native + compat ioctl, struct copied with `copy_from_user` | done |
| Apply a CPU frequency ceiling | helper `cpu_opp_limit()` commented out; no definition anywhere (D1:1543-1551, symbols.txt: 0 hits) | nothing | `FREQ_QOS_MAX` request with `limited_freq` kHz on every selected policy via `freq_qos_add_request()` | done |
| Mask handling `limited_cpu` | unknown (helper absent) | n/a | 32-bit ABI bitmask over possible CPUs, 0 = all, CPUs >= 32 never selectable via a nonzero mask; one request per policy even when several selected CPUs share it | done (`[INFERENCE]`, §5) |
| Re-issue with a different ceiling | unknown | n/a | existing request for a visited policy is `freq_qos_update_request()`d; active policies the new mask does not select are released in the same call (re-enable replaces the previous ceiling set) | done (`[INFERENCE]`) |
| `enable == 0` removes the ceiling | unknown | n/a | all requests updated to `FREQ_QOS_MAX_DEFAULT_VALUE`, removed, policy refs dropped | done (`[INFERENCE]`) |
| Invalid `enable` / negative `limited_cpu` / non-positive `limited_freq` | none (values ignored) | n/a | `-EINVAL` | done |
| No matching policy | n/a | n/a | `-ENODEV` | done |
| `CONFIG_CPU_FREQ=n` | n/a | n/a | `-EOPNOTSUPP` (compile-time arm) | done |
| Ceiling removed when the setter dies | never released (ROM does nothing) | n/a | released at last close of `/dev/Vcodec` and at module exit | done (deliberate deviation, §5) |
| Serialisation | none | n/a | `vcodec_cpu_opp_lock` mutex, table bounded by `NR_CPUS` | done |

---

## 4. Compile test (exercised)

Environment for every command:

```
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/vcodec-gap-cpuopp
cp $M/nura.config .config
scripts/config --module MTK_VCODEC_MT6735
make ARCH=arm64 LLVM=1 olddefconfig
```

(`nura.config` does not set `CONFIG_MTK_VCODEC_MT6735`; the baseline series' Kconfig entry has to be
enabled for the objects to exist. `CONFIG_CPU_FREQ=y`, `CONFIG_CPUFREQ_DT=y` as shipped.)

Tool overrides used in every build:
`AR=ar NM=aarch64-linux-gnu-nm OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump
READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip`.

| # | Command | Result |
|---|---|---|
| 1 | `make ARCH=arm64 LLVM=1 ... -j2 drivers/misc/mediatek/vcodec/vcodec-core.o` | exit 0, `CC [M] .../vcodec-core.o`, no warnings |
| 2 | same three objects with `W=1` (`vcodec-core.o vcodec-vdec.o vcodec-venc.o`, core touched) | exit 0, `CC [M]` ×3, **0 warnings** |
| 3 | `scripts/config --disable CPU_FREQ; make ... olddefconfig; make ... W=1 vcodec-core.o` | exit 0, `CC [M] .../vcodec-core.o`, 0 warnings; `nm -u` shows no `freq_qos_*` / `cpufreq_cpu_*` references (the `#else` arm was compiled) |
| 4 | restore saved `.config` (`CPU_FREQ=y`) + `olddefconfig`, rebuild all three objects `W=1` | exit 0, clean |
| 5 | `make ... drivers/misc/mediatek/vcodec/vcodec-mt6735.o` | exit 0, `LD [M]` — objects link together |
| 6 | `perl scripts/checkpatch.pl --no-tree --strict -f drivers/misc/mediatek/vcodec/vcodec-core.c` | `0 errors, 0 warnings, 0 checks` (1412 lines) |

Rows 1-6 were re-run after the review corrections (negative-mask rejection, release of policies
outside a re-issued mask, 32-bit mask shifted only for CPUs < 32). Final artifact `.config` is back
at the `CPU_FREQ=y` configuration.

---

## 5. Inferred / hardware-unverified

1. **`cpu_opp_limit()` semantics are `[INFERENCE]`.** No available source defines the helper. The
   behaviour implemented follows the argument names and the downstream call site:
   `limited_freq` = kHz ceiling, `limited_cpu` = CPU bitmask (0 = all), `enable` 1/0 = apply/remove.
   The ROM itself performed no action, so there is no binary behaviour to compare against.
2. The ceiling is enforced with `FREQ_QOS_MAX` on the policy's `max_freq` constraint. That is the
   mechanism cpufreq-dt's `cpufreq_driver->setpolicy`/`target` path reads, but the effective clock
   on MT6735 (including any firmware/CPPC style floor above the requested kHz) is hardware
   unverified.
3. `limited_freq` is not validated against the policy's min/max; a value above the policy maximum is
   a no-op constraint, a value below the policy minimum clamps the CPU to its lowest OPP. The
   downstream helper's clamping behaviour is unknown.
4. `limited_cpu` is the 32-bit ABI field (`int` downstream, `__s32` in the uapi header), so CPUs
   with an index >= 32 can never be selected through a nonzero mask; they are only covered when
   the mask is 0 (all CPUs). Bits at or beyond the number of possible CPUs are ignored (the loop
   only visits possible CPUs), and a negative `limited_cpu` is rejected with `-EINVAL`. The uapi
   header has no field that could address CPUs above 31.
5. Disabling drops *all* requests this driver owns regardless of the mask. Documented in the code:
   a stale cap is worse than an over-broad release, and it is exactly what the last-close path does.
6. Enabling again replaces the previous ceiling set: after applying the new mask, every active
   request whose policy this call did not visit is released, so a narrower re-issue cannot leave a
   CPU outside the new mask capped. A mask of 0 selects every CPU and therefore keeps all policies.
7. Deliberate deviation: the ROM/downstream never released anything (it never applied anything).
   Here the ceiling is removed when the last file descriptor on `/dev/Vcodec` closes and at module
   exit, so a crashed codec process cannot leave the CPU capped.
8. CPU hotplug/policy teardown while a request is active: the held `cpufreq_cpu_get()` reference
   keeps the policy valid (see §2.3), so the later update/remove is safe. After the policy is
   re-created the stale request no longer applies to the new policy; it is released by the next
   enable call that does not visit that policy, by a disable, or at last close/module exit. No
   cpufreq policy notifier is registered (not required by the task spec).
9. Not exercised on hardware; no phone attached.

---

## 6. DTS-FRAGMENT

```
none
```

---

## 7. Patch

```
$M/patches/vcodec-gap-cpuopp.patch
```
