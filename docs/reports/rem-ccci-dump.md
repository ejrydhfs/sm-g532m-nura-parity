# CCCI gap — AEE/SBP/DHL exception dump + userspace tooling

`M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Worktree `$M/worktrees/rem-ccci-dump`, branch `rem-ccci-dump`, HEAD
`509789971 ccci: gap-closure scaffold (shared API, INFRA_AO mapping)`
(parent `e4a4be64e ccci: integration baseline`, base Nura `046883c0c`).
Continued from the aborted first run (860-line draft + shared-file edits),
audited value by value; nothing was restarted.

## 1. Gap, files changed, result

**Gap.** The Nura CCCI core had no exception/AEE path: no EX_LOG_T decoder, no
dump of the modem exception record or the shared-memory debug windows, no
`aee_dump_ccci_debug_info()` ABI, no exception-stage bookkeeping during the
CLDMA/CCIF handshake, no DHL flush window at `HIF_EX_CLEARQ_DONE`, no dump
before the watchdog reset, and no way for userspace to record/decode a dump.
The legacy SRAM runtime block also published the SBP value into the wrong
`modem_runtime` slot (handshake-v2 feature ids were used as legacy
`feature_<n>_val` indexes).

**Files changed**

| File | Change |
|---|---|
| `drivers/misc/mediatek/ccci/ccci_exception.h` (new) | EX_LOG_T wire layout, AED flags, ex_type ids, engine state/API |
| `drivers/misc/mediatek/ccci/ccci_exception.c` (new) | exception engine: packet capture, record/window dump, decoder, kernel log + util ring + debugfs + workqueue, AEE window accessor |
| `drivers/misc/mediatek/ccci/ccci_core.c` | `aee_dump_ccci_debug_info()` (only definition), MD_EX/MD_EX_REC_OK capture hook, dump on EXCEPTION entry, init/teardown wiring, error-path labels |
| `drivers/misc/mediatek/ccci/ccci_ccif.c` | exception stage bookkeeping, DHL flush + engine stop/flush + ring purge + sync dump at `HIF_EX_CLEARQ_DONE`, dump before the watchdog reset |
| `drivers/misc/mediatek/ccci/ccci_boot.c` | boot-timeout dump request; legacy SRAM runtime MISC slots fixed (SBP/RAND/SEQ) |
| `drivers/misc/mediatek/ccci/ccci_port_char.c` | META-mode DHL close gate, `CCCI_IOC_GET/SET_MD_SBP_CFG` |
| `drivers/misc/mediatek/ccci/ccci_priv.h` | `struct ccci_exception ex`, legacy `MISC_FEATURE_ID`, dump constants, SBP ioctls |
| `drivers/misc/mediatek/ccci/Kconfig` | `MTK_MD_SBP_CUSTOM_VALUE`, `MTK_MD2_SBP_CUSTOM_VALUE` |
| `drivers/misc/mediatek/ccci/Makefile` | `ccci_exception.o` in `ccci-y` |
| `tools/mediatek/ccci/{Makefile,.gitignore,ccci-dump.c,ccci_exrec.h}` (new) | host tool: record `/dev/ccci_dump`, read debugfs, decode EX_LOG_T |

**Result.** The full AEE/SBP/DHL exception-dump path is implemented and
compiles (`ccci_exception.o`, `ccci_core.o`, `ccci_boot.o`,
`ccci_port_char.o`, `ccci_ccif.o`, directory `built-in.a`), the frozen
`aee_dump_ccci_debug_info()` has exactly one definition matching
`include/linux/ccci.h`, and `tools/mediatek/ccci` builds and passes its
no-device checks with the host compiler. Two behaviours remain partial and are
called out in §3/§5/§7: the DHL RX ring cannot stay alive during the 2 s flush
window (frozen CLDMA API lacks `stop_for_ee`) and the legacy SBP WM_ID half has
no source in this tree.

## 2. ROM + downstream evidence

### 2.1 Exception record layout (`eccci/ccci_core.h`)

| Item | Evidence |
|---|---|
| `EX_HEADER_T` 4 B: ex_type, ex_nvram, ex_serial_num | `ccci_core.h:86-91` |
| `EX_ENVINFO_T` 164 B, boot_mode +0x10, status +0x21, ELM_status +0x22 | `ccci_core.h:93-101` |
| `EX_FATALERR_CODE_T` 8 B / `EX_FATALERR_T` 296 B | `ccci_core.h:103-113` |
| `EX_ASSERTFAIL_T` 296 B: filename[24], line, params[3] | `ccci_core.h:115-121` |
| `EX_CONTENT_T` union 296 B, `EX_LOG_T` 512 B | `ccci_core.h:123-135` |
| offender string at record offset 288, `0xCC` = none | `port_kernel.h:33`, `port_kernel.c:3138-3145` |
| AED selectors `EX_MEM/MD_IMG_MEM/CCIF_REG/EX_PKT` = bits 0-3 | `port_kernel.h:19-22` |
| ex_type ids 0-13, cross-core 15, C2K `0x25`, EMI check 99 | `port_kernel.h:36-57` |
| `EMI_MPU_VIOLATION = 0x30` | `ccci_core.h:246` |
| `ECT_SRC_RMPU = 1 << 11` | `ccci_core.h:352` |
| record from packet iff SYS3 or (normal EE && boot stage 0), else smem | `port_kernel.c:3125-3135` |
| decoder names/fields per type (incl. "Fatal error (rmpu violation)") | `port_kernel.c:3147-3250`, `:2752-2765` |
| C2K fatal error (`err_code1 == 0x3000`): no AEE DB trigger | `port_kernel.c:3100-3104`, `port_kernel.h:57` |
| MD image head dump size 256 | `ccci_core.h:640` |
| MD_EX_STAGE `EX_NONE/EX_INIT/EX_DHL_DL_RDY/EX_INIT_DONE` | `ccci_core.h:66-74` |

The compiled file asserts these sizes/offsets with `BUILD_BUG_ON`
(`sizeof(EX_LOG_T)==512`, header 4, envinfo 164, content 296, offender 288 <
512). `CCCI_EXREC_OFFSET_OFFENDER` matches.

### 2.2 Shared-memory exception windows

| Item | Evidence |
|---|---|
| `CCCI_SMEM_DUMP_SIZE 4096`, `CCCI_SMEM_SIZE_EXCEPTION 0x10000` | `mt6735/ccci_config.h:70-71` |
| `OFFSET_EXREC 2048`, `CCIF_SRAM_SIZE 16`, `OFFSET_CCIF_SRAM = 2048+1024-16 = 0xBF0` | `mt6735/ccci_config.h:75-77` |
| `OFFSET_EPON 0xC64` (SW-WDT marker), `OFFSET_SEQERR 0x34` | `mt6735/ccci_config.h:78-79` |
| `OFFSET_CCCI_DEBUG 0`, `CCCI_DEBUG_SIZE 2048` | `mt6735/ccci_config.h:80-81` |
| `OFFSET_MDSS_DEBUG 2048`, `MDSS_DEBUG_SIZE 2048` | `mt6735/ccci_config.h:82-83` |
| AEE window = base + `OFFSET_CCCI_DEBUG`, size `CCCI_DEBUG_SIZE` | `eccci/ccci_core.c:271-273` (downstream), returned at `:596-597` |
| smem layout init (`exp_smem_base`, `exp_dump_size`, `rec_base`) | `eccci/ccci_core.c:262-294` (downstream) |
| CCIF SRAM 16-byte snapshot dump | `port_kernel.c:2311-2312` |
| 4 KiB EE dump of both windows | `port_kernel.c:3085-3087` (`ccci_exp_dump_size`) |

### 2.3 `aee_dump_ccci_debug_info()`

| Item | Evidence |
|---|---|
| Source implementation: 1-based md id, returns `ccci_exp_smem_ccci_debug_vir/size`, `-CCCI_ERR_MD_INDEX_NOT_FOUND` if absent | `eccci/ccci_core.c:580-599` (downstream) |
| ROM symbol | `aee_dump_ccci_debug_info @ 0xc0508b54` (`llvm-nm zip/vmlinux.elf`) |
| 1-based conversion | `sub r0, r0, #1` @ `0xc0508b68` |
| returned pointer/size = two adjacent md fields | `ldr r12,[r3,#0x850]`/`str r12,[r1]` @ `0xc0508ba4-0xbac`; `ldr r3,[r3,#0x854]`/`str r3,[r2]` @ `0xc0508bb0-0xbb4`; `mov r0,#0` @ `0xc0508ba8` |
| not-found return `-316` | `e30f0ec4` (movw r0,#0xfec4) @ `0xc0508bbc` + `e34f0fff` (movt r0,#0xffff) @ `0xc0508bc0` → `0xfffffec4`; `CCCI_ERR_MD_INDEX_NOT_FOUND = 300+16` at `mt_ccci_common.h:276,334` |
| ROM error region | `mt_ccci_common.h:272-278` |

Implementation: `ccci_core.c:1039-1068`, one definition, `EXPORT_SYMBOL_GPL`,
signature identical to `include/linux/ccci.h:320`.

### 2.4 Exception handshake, DHL, AEE notification

| Stage / behaviour | Evidence |
|---|---|
| `md_cd_ccif_delayed_work()`: AEE "Modem exception dump start..." message, `cldma_stop()`, flush TX delayed work + RX/refill work, then `H2D_EXCEPTION_CLEARQ_ACK` | `modem_cldma.c:1966-1983` |
| `md_cd_exception()` stage machine | `modem_cldma.c:1986-2033` |
| `HIF_EX_INIT`: notify EX_INIT, `cldma_stop_for_ee()`, purge Tx, ACK | `modem_cldma.c:1995-2010` |
| `HIF_EX_INIT_DONE`: notify `EX_DHL_DL_RDY` | `modem_cldma.c:2011-2013` |
| `HIF_EX_CLEARQ_DONE`: `msleep(2000)` DHL flush, then delayed work | `modem_cldma.c:2014-2018` |
| `HIF_EX_ALLQ_RESET`: reset, notify `EX_INIT_DONE`, start, SO_CFG check | `modem_cldma.c:2019-2033` |
| CCIF-ring variant: no 2 s delay; dump + ring reset + ACK | `modem_ccif.c:395-418` |
| ROM string "Modem exception dump start, please wait up to 5 minutes." | `zip/vmlinux.strings:38635`, VMA `0xC0C9B534` |
| ROM AEE notification: `aed_md_exception_api @ 0xc0628298` looks up a global `g_aee_api` (`movw ip,#0x29d0/movt ip,#0xc121` @ `0xc06282c8-0xccc`), checks `.md_exception` at `[api+4]` (`0xc06282dc-0x2e4`), calls it (`blx ip` @ `0xc0628310`); prints "g_aee_api is null" / "g_aee_api->md_exception = 0x%p" / "%s out" | disassembly @ `0xc0628298-0xc062834c`; strings VMA `0xC0CE8254`, `0xC0CE8230`, `0xC0CE826c` |
| ROM validates exception messages | strings "receive invalid MD_EX", "receive invalid MD_EX_REC_OK, resv=%x, len=%d", "get MD_EX_REC_OK without exception MD_EX" (`vmlinux.strings:38056,38060,38061`) |
| DHL control port gate (`META`): only `critical_user_active[3]` must be free | `port_char.c:64-96` |
| DHL protocol type ioctl returns `"DHL"` (4 bytes incl. NUL) | `port_char.c:575-590` |
| ROM prints the same gate error | string "DHL ctrl is still open in META mode" (`vmlinux.strings:37546`) |
| `FEATURE_DHL_LOG_EN` is never defined in this ROM tree, so no DHL shared-memory carve-out exists | only `#ifdef` uses: `port_kernel.c:215`, `mt_ccci_common.h:780` |
| WDT worker dumps RGU + smem before reset (when mdlogger is open) | `modem_cldma.c:1838-1897` |
| SW-WDT marker `0xBAEBAE10` at `smem+0xC64` | `modem_cldma.c:1862-1872` |

### 2.5 SBP

| Item | Evidence |
|---|---|
| handshake-v2 feature ids (SBP = 9) | `ccci_core.h:692-707` (`MD_CCCI_RUNTIME_FEATURE_ID`) |
| AP mask table: SBP = `CCCI_FEATURE_MUST_SUPPORT` | `port_kernel.c:201-257` (line 244) |
| v2 request publishes `sbp_code` | `modem_cldma.c:3210-3244` |
| legacy `modem_runtime`: `feature_2_val[0]` seed, `feature_4_val[0]` SBP, `feature_5_val[0]` seq check; SBP advertised only `if (sbp_code > 0)` | `modem_cldma.c:3352-3360`; `modem_ccif.c:930-958` |
| legacy MISC indexes `MISC_RAND_SEED=2`, `MISC_MD_SBP_SETTING=4`, `MISC_MD_SEQ_CHECK=5` | `mt_ccci_common.h:734-743` |
| `feature_4_val[1] = WM_ID` from `load_type` | `modem_cldma.c:3357-3360`, `ccci_core.c:236` (downstream), `ccci_support.c:29-31` |
| GET/SET SBP ioctls, Kconfig string default | `port_char.c:914-958`, `ccci_util/Kconfig:99-113` |
| control message ids / check ids used by the capture hook | `mt_ccci_common.h:580-591` |

## 3. Parity matrix

| Behaviour | ROM + downstream | Nura before | After | Status |
|---|---|---|---|---|
| EX_LOG_T layout (4/164/296/512, offender 288) | `ccci_core.h:86-135`, `port_kernel.h:33` | absent | `ccci_exception.h` packed structs + `BUILD_BUG_ON`s | done |
| ex_type ids incl. `EMI_MPU_VIOLATION 0x30` | `port_kernel.h:36-57`, `ccci_core.h:246` | absent | `CCCI_EX_TYPE_*`, names verbatim from the decoder | done |
| `aee_dump_ccci_debug_info()` 1-based id, debug window, -316 | `eccci/ccci_core.c:580-599`; ROM @ `0xc0508b54` | absent | one definition in `ccci_core.c`, frozen signature | done |
| MD_EX_REC_OK packet validation + capture (header+512 B) | `port_kernel.c:551-582` | absent | `ccci_rx_exception_packet()` validates `reserved`/length, stores the packet | done |
| Record source: packet for SYS3 / boot stage 0, else smem+2048 | `port_kernel.c:3125-3135` | absent | same rule in `ccci_ex_build()`; falls back to the packet when smem is absent | done (no `more_info` EE-case flag in this tree — §5) |
| Dump destinations | AEE DB via `aed_md_exception_api` | nothing | kernel log, `/dev/ccci_dump` ring (`ccci_util_dump_mem`), debugfs `ccci/mdN/exception_{dump,packet,stage}` | done (AEE client itself deferred) |
| Decode metadata: header, envinfo, assert file/line/params, fatal codes, offender, RMPU/C2K notes | `port_kernel.c:3147-3250` | absent | `ccci_ex_decode_record()` + userspace decoder | done |
| Dump on MD_EX / MD_EX_REC_OK / MD_EX_PASS arrival | `port_kernel.c:551-600` | absent | `ccci_rx_exception_packet()` + trigger on EXCEPTION entry | done |
| Stage EX_INIT on handshake entry | `modem_cldma.c:2003`, `modem_ccif.c:400` | state only | `md->ex_stage = CCCI_EX_INIT` + dump | done |
| Stage EX_DHL_DL_RDY | `modem_cldma.c:2012` | absent in handshake | set + dump | done |
| Stage EX_INIT_DONE at ALLQ_RESET | `modem_cldma.c:2023` | absent | set before CLDMA start | done |
| 2000 ms DHL flush at CLEARQ_DONE | `modem_cldma.c:2016` | absent | `msleep(2000)` (CLDMA modems only) | done |
| Stop + flush TX/RX workers before CLEARQ_ACK | `modem_cldma.c:1975-1983` | absent | `ccci_cldma_stop()` (flushes tx delayed work, rx/refill work in `cldma_hw_stop()`), then ring purge, then ACK | done (superset: the engine is already stopped at EX_INIT — §3 note) |
| Dump after the flush, before the ACK | `modem_cldma.c:1971-1983` | absent | `ccci_exception_trigger_sync()` before `H2D_EXCEPTION_CLEARQ_ACK` | done |
| DHL RX ring alive during the flush window | `cldma_stop_for_ee()` `modem_cldma.c:1449-1520` | n/a | whole engine stopped at EX_INIT | **partial** — needs `stop_for_ee` (§7) |
| Dump before MD watchdog reset | `modem_cldma.c:1838-1897` | reg dump + reset | reg dump + sync exception dump + reset | done (superset: ROM only dumps when mdlogger is open — §5) |
| Boot-timeout (handshake 1/2) dump selection | `port_kernel.c:3421-3456` | image-head log only | async dump with `CCIF_REG` + (`MD_IMG_MEM` or `EX_MEM`) | done |
| DHL control-port META gate | `port_char.c:64-96`; ROM string | absent | `ccci_char_close_check()` META branch (same error text) | done |
| `CCCI_IOC_GET_MD_PROTOCOL_TYPE` = "DHL" | `port_char.c:575-590` | present (baseline) | unchanged, verified 4 bytes | done |
| SBP handshake-v2 (id 9, `feature_set`, value) | `ccci_core.h:692-707`, `port_kernel.c:244` | present | verified, not duplicated | done |
| Legacy SRAM runtime SBP slot | `modem_ccif.c:930-958` (slot 4, gated on `sbp_code>0`) | wrote feature index 9, support bit always set | `CCCI_MISC_*` indexes (2/4/5), SBP gated | done |
| Legacy SBP `WM_ID` (`feature_4_val[1]`) from modem load type | `modem_cldma.c:3357-3360` | absent | absent — no `load_type`/`get_md_wm_id_map()` in this tree | deferred (§5/§7) |
| `CCCI_IOC_GET/SET_MD_SBP_CFG` + Kconfig default | `port_char.c:914-958` | absent | implemented; default resolved once at modem creation | done |
| Userspace record/decode | ROM userspace AEE tooling (absent from tree) | none | `tools/mediatek/ccci/ccci-dump` (`/dev/ccci_dump` + debugfs + `-D`) | done |

**Dump stages**: `EX_INIT`, `EX_DHL_DL_RDY`, `HIF_EX_CLEARQ_DONE` (flush →
stop → purge → sync dump → ACK), `HIF_EX_ALLQ_RESET`, `MD_WDT`,
`MD_NO_RESPONSE` (boot timeout) are implemented. Deferred: the AEE database
client (`aed_md_exception_api`) does not exist in this tree — the dump is
delivered to the kernel log, the util dump ring and debugfs instead, and the
ROM ABI used by such a client (`aee_dump_ccci_debug_info`) is provided; and
the DHL RX-alive window, blocked by the frozen CLDMA API (§7).

## 4. Compile-tested

Configuration (per coordinator instruction; `nura.config` has no CCCI symbol):

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/rem-ccci-dump"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K -e MTK_CCCI_UTIL -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig          # exit 0
grep -E '^CONFIG_MTK_CCCI' .config
# CONFIG_MTK_CCCI=y
# CONFIG_MTK_CCCI_C2K=y
# CONFIG_MTK_CCCI_CLDMA=y
# CONFIG_MTK_CCCI_UTIL=y
```

Kernel objects (forced rebuild, `W=1`):

```sh
rm -f drivers/misc/mediatek/ccci/{ccci_exception,ccci_core,ccci_boot,ccci_port_char,ccci_ccif}.o
make ARCH=arm64 LLVM=1 -j1 W=1 \
  drivers/misc/mediatek/ccci/ccci_exception.o \
  drivers/misc/mediatek/ccci/ccci_core.o \
  drivers/misc/mediatek/ccci/ccci_boot.o \
  drivers/misc/mediatek/ccci/ccci_port_char.o \
  drivers/misc/mediatek/ccci/ccci_ccif.o
# exit 0; 5/5 objects rebuilt; zero `warning:`/`error:` lines
make ARCH=arm64 LLVM=1 -j1 drivers/misc/mediatek/ccci/built-in.a
# exit 0; 10 members:
# ccci_core.o ccci_platform.o ccci_ccif.o ccci_boot.o ccci_port_char.o
# ccci_port_net.o ccci_port_kernel.o ccci_ringbuf.o ccci_sysfs.o ccci_exception.o
```

(`drivers/misc/mediatek/ccci/ccci.o` has no rule because `CONFIG_MTK_CCCI=y`
builds the objects straight into `built-in.a`; the archive target is the
composite build.) The only archive-target output is kbuild's own
`Makefile.build:541/462: overriding recipe` GNU-make notices for invoking the
directory archive directly; no code warning.

Pre-existing `W=1` kernel-doc warnings (functions untouched by this patch,
present at HEAD; the coordinator already names the `ccci_core` one):

```
Warning: ccci_core.c:446 parameter 'md'/'qno'/'skb' not described in 'ccci_md_send_skb'
Warning: ccci_boot.c:641 parameter 'md'/'skb' not described in 'ccci_boot_prepare_runtime_data'
Warning: ccci_ccif.c:145 parameter 'md'/'channel' not described in 'ccci_ccif_send'
```

Userspace tool:

```sh
make -C tools/mediatek/ccci        # exit 0
# cc -O2 -Wall -Wextra -std=gnu11 -c ccci-dump.c -o build/obj/ccci-dump.o
# cc -O2 -Wall -Wextra -std=gnu11 -o build/bin/ccci-dump build/obj/ccci-dump.o
# `cc` is /usr/bin/cc = cc (GCC) 16.2.1 20260810
make -C tools/mediatek/ccci check  # exit 0
# check: help, input validation and error paths OK
```

No-device behaviour (honest failure, no fabricated record):

```
$ ./build/bin/ccci-dump -m 1
ccci-dump: /dev/ccci_dump: No such file or directory      (exit 1)
```

Record layout self-test (throwaway generator in `/tmp`, **not** hardware data;
verifies packing/endianness/offender offset of the delivered header):

```
$ ccci-dump -D /tmp/exrec_fatal.bin      # EX_LOG_T, ex_type 0x30
ex_type 48 (0x30, Fatal error (rmpu violation)) ... code1 0xdeadbeef
code2 0xcafebabe ... offender "MODEM_TASK"
$ ccci-dump -D /tmp/exrec_assert.bin     # ex_type 5
assert file "l1_rrc.c", line 4242, p1/p2/p3 0x11111111 0x22222222 0x33333333
offender none (0xcc marker, port_kernel.c:3138)
```

Cross-leaf symbols: no stub was added for `ccci_mpu_*`/`ccci_rpc_*`/CCMNI.
`aee_dump_ccci_debug_info` is the only symbol this leaf owns.

## 5. `[INFERENCE]` and hardware-unverified

- `[INFERENCE]` The engine has no `more_info` EE-case flag, so the record-source
  rule is `md_id == SYS3 || boot_stage == 0` (downstream additionally requires
  `more_info == MD_EE_CASE_NORMAL`). For the exception flows this tree can
  generate (MD_EX / MD_EX_REC_OK / MD_EX_PASS / WDT / boot timeout) the packet
  is valid exactly when that condition holds.
- `[INFERENCE]` Downstream's watchdog worker only takes the dump path when the
  mdlogger is open (`critical_user_active[2] == 0` resets instead). This tree
  always resets from the WDT work, so the snapshot is taken unconditionally
  before the reset — a superset, not a change of reset policy.
- `[INFERENCE]` `ccci_boot_timeout()` runs in timer context; it uses the
  asynchronous trigger only (never `flush_work`).
- `[INFERENCE]` `aee_dump_ccci_debug_info()` returns `-ENODEV` when the modem
  has no mapped shared memory; downstream returns the (possibly stale) pointer
  unconditionally. Valid callers see identical values.
- `[INFERENCE]` `ccci_boot_send_runtime_data()` resolves the Kconfig SBP default
  when userspace passes 0; downstream leaves the value to
  `CCCI_IOC_SET_MD_SBP_CFG`. Bytes are identical when userspace follows the
  GET→SET protocol.
- Hardware-unverified: the record contents the modem actually writes, the
  2000 ms DHL window, the RMPU offender string placement, and every register
  value printed by `ccci_ccif_dump_regs()`. No device was attached; nothing
  was simulated.

## 6. DTS-FRAGMENT

none. The engine uses the existing `smem`/`md-image` regions (`md->smem_vir`,
`md->smem_size`, `md->img_vir`, `md->img_size`) already resolved by
`ccci_boot_smem_setup()`; no new property is consumed.

## 7. SCAFFOLD CHANGE REQUIRED

One frozen declaration is insufficient for full DHL parity: the flush window
needs the downstream `cldma_stop_for_ee()` behaviour (stop TX and RX 4-7, keep
RX 0-3 running/unmasked, no block reset — `modem_cldma.c:1449-1520`). Today the
engine can only stop fully, so `HIF_EX_INIT` stops CLDMA and the 2 s window at
`HIF_EX_CLEARQ_DONE` carries no RX traffic. This is the same gap the CLDMA leaf
reported; the exact proposal is:

```c
/*
 * CLDMA exception stop, downstream cldma_stop_for_ee(): stop all TX queues
 * and the RX queues in @stop_mask (SO_STOP_CMD), leaving the others running
 * and unmasked.  modem_cldma.c:1449-1520 uses NONSTOP_QUEUE_MASK 0xF0, i.e.
 * RX 0-3 stay alive.  Must not reset the block.
 */
void ccci_cldma_stop_for_ee(struct ccci_cldma *cd, u16 stop_mask);
```

Caller that would change in this leaf's file: `ccci_ccif.c` `HIF_EX_INIT`
(`ccci_cldma_stop(md->cldma)` → `ccci_cldma_stop_for_ee(md->cldma, 0xF0)`).

Missing input (not fabricated): the legacy SBP `WM_ID` half
(`feature_4_val[1]`, `modem_cldma.c:3357-3360`) needs the modem load-type
(`md->config.load_type` / `get_modem_support_cap()` / `get_md_wm_id_map()` and
the `opt_eccci_c2k` selection), none of which exists in this tree or the frozen
`struct ccci_md_cfg`. The SBP code itself is published correctly.

## 8. Patch and status

Patch: `$M/patches/rem-ccci-dump.patch` (2560 lines, diff against HEAD
`509789971`, verified with `git apply --check --reverse`).

`git status --porcelain`:

```
M  drivers/misc/mediatek/ccci/Kconfig
M  drivers/misc/mediatek/ccci/Makefile
M  drivers/misc/mediatek/ccci/ccci_boot.c
M  drivers/misc/mediatek/ccci/ccci_ccif.c
M  drivers/misc/mediatek/ccci/ccci_core.c
A  drivers/misc/mediatek/ccci/ccci_exception.c
A  drivers/misc/mediatek/ccci/ccci_exception.h
M  drivers/misc/mediatek/ccci/ccci_port_char.c
M  drivers/misc/mediatek/ccci/ccci_priv.h
A  tools/mediatek/ccci/.gitignore
A  tools/mediatek/ccci/Makefile
A  tools/mediatek/ccci/ccci-dump.c
A  tools/mediatek/ccci/ccci_exrec.h
```

Shared-file hunks for the coordinator: `ccci_core.c` (includes + state-change
trigger + `ccci_rx_exception_packet()` + dispatch hook + WDT comment + register
error-path/init/teardown), `ccci_ccif.c` (handshake + WDT dump),
`ccci_boot.c` (timeout + legacy SBP slots), `ccci_priv.h` (append-only blocks),
`ccci_port_char.c` (META gate + SBP ioctls). No shared DTS, no other leaf's
worktree, no commit.
