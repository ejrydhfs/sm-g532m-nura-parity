# rem2-aee — AEE/AED exception ("DB") client interface

## 1. Gap, files changed, worktree, result

**Gap.** The Nura MT6735 tree had no AEE/AED client: nothing registers the ROM's
`/dev/aed0` (EE) / `/dev/aed1` (KE) misc devices, `aed_md_exception_api()` did
not exist, the CCCI exception worker built its dump but never handed it to AEE,
and no userspace program could speak the AED ABI. The frozen
`include/linux/aee.h` declared the four `aed_*_exception_api()` entry points
(guarded by `CONFIG_MTK_AEE_FEATURE`); this leaf implements them.

**Worktree.** `$M2/worktrees/rem2-aee`, detached `046883c0c` ("arm64: dts:
mediatek: Add support for Doogee X5 Pro") + `$M/patches/parity-baseline-all.patch`
+ the coordinator-owned frozen `include/linux/aee.h` (with the
`IS_ENABLED(CONFIG_MTK_AEE_FEATURE)` guard and the no-op fallbacks), frozen
`include/linux/clkbuf.h` and the appended CLDMA block of
`include/linux/ccci.h`. Nothing was committed, reset or checked out.

**Result (one line).** New `drivers/misc/mediatek/aee/` implements the EE/KE
misc devices, the AE_Msg protocol, the EE record queue with the CCCI detail
section, both `AEEIOCTL_GET_THREAD_REG` encodings, `AEEIOCTL_GET_PROCESS_BT`,
`aed_md_exception_api()` + the md32/scp/combo variants (exported, GFP_ATOMIC),
`ccci_ex_build()` now calls it, and `tools/mediatek/aee/aee-db` speaks the ABI;
all objects build at W=1 with zero warnings and `make check` passes.

**Diffstat** (`$M/patches/rem2-aee.patch`, delta against the parity baseline):

```
 drivers/misc/mediatek/Kconfig               |    1 +
 drivers/misc/mediatek/Makefile              |    1 +
 drivers/misc/mediatek/aee/Kconfig           |   20 +
 drivers/misc/mediatek/aee/Makefile          |    3 +
 drivers/misc/mediatek/aee/aed-main.c        | 1160 +++++++++++++++++++++++++++
 drivers/misc/mediatek/aee/aed-process.c     |   84 ++
 drivers/misc/mediatek/aee/aed.h             |   65 ++
 drivers/misc/mediatek/ccci/ccci_exception.c |   38 +
 include/uapi/linux/aee.h                    |  194 +++++
 tools/mediatek/aee/.gitignore               |    1 +
 tools/mediatek/aee/Makefile                 |   23 +
 tools/mediatek/aee/aee-db.c                 |  788 ++++++++++++++++++
 12 files changed, 2378 insertions(+)
```

## 2. Evidence

### 2.1 ROM kernel symbols (`llvm-nm $M/zip/vmlinux.elf`)

| symbol | address |
|---|---|
| `aed_md_exception_api` | `0xc0628298` |
| `aed_md32_exception_api` | `0xc0628354` |
| `aed_scp_exception_api` | `0xc0628410` |
| `aed_combo_exception_api` | `0xc06284cc` |
| `aed_ee_open` / `aed_ee_read` / `aed_ee_write` / `aed_ee_poll` | `0xc062a328` / `0xc062bb0c` / `0xc062c3f0` / `0xc062ae60` |
| `aed_ke_open` / `aed_ke_read` / `aed_ke_write` / `aed_ke_poll` | `0xc062a5a0` / `0xc062bac8` / `0xc062bb50` / `0xc062aec8` |
| `aed_ioctl` | `0xc062a614` |
| `aed_get_traces` / `aed_get_process_bt` | `0xc062ad60` / `0xc062ea84` |
| `aed_proc_debug_init` / `aed_init` | `0xc062e4a8` / `0xc0db5c90` |
| `aee_dump_ccci_debug_info` | `0xc0508b54` |

ROM strings `$M/zip/vmlinux.strings:45359-45360`: `failed to register aed0(ee)
device!`, `failed to register aed1(ke) device!` — the device names and the
registration order reproduced here.

### 2.2 Downstream source (`$M/src/downstream-grandpplte`)

`drivers/misc/mediatek/aee/aed/aed.h`
: `39-41` `AE_INVALID`/`AE_NOT_AVAILABLE`/`AE_DEFAULT`; `43-49` `enum AEE_MODE`;
`58-61` `enum AE_PASS_METHOD`; `63` `enum AE_CMD_TYPE`; `65-108` `enum
AE_CMD_ID` (incl. `AE_REQ_USERSPACEBACKTRACE = 40`); `110-125` `struct AE_Msg`;
`130-140`/`141-148` `struct aee_ioctl`; `151-161` the `AEEIOCTL_*` list;
`147-149` `AEE_NR_FRAME 32`.

`drivers/misc/mediatek/include/mt-plat/aee.h`
: `25-31` `AE_DEFECT_ATTR`; `33-53` `AE_EXP_CLASS`; `65-84` `struct aee_bt_frame`
(256 B) and `struct aee_process_bt`; `87-90` `struct aee_thread_reg`
(`pid_t tid; struct pt_regs regs;`); `186-218` `DB_OPT_*`; `246-253` the four
`aed_*_exception_api()` prototypes.

`drivers/misc/mediatek/aee/aed/aed-main.c`
: `71-84` globals (`aed_device_lock`, `aee_mode`, `force_red_screen`);
`155-182` `struct aed_eerec`/`aed_kerec`/`aed_dev`; `165` `MAX_EE_COREDUMP`
0x800000; `203-260` `msg_destroy`/`msg_create`/`msg_copy_to_user`;
`685-914` the EE message generators — `ee_gen_class_msg` ("External (EE)", 14 B),
`ee_gen_type_msg`, `ee_gen_process_msg`, `ee_gen_detail_msg` (`744-813`),
`ee_gen_coredump_msg`; `816-838` `ee_destroy_log`; `840-850` `ee_log_avail`;
`884-914` `ee_gen_ind_msg` (AE_IND_EXP_RAISED, arg `AE_EE`, `dbOption`,
5-minute `wait_for_completion_timeout`); `938-968` `ee_queue_request`/`ee_worker`;
`975-1105` EE fops (`read`/`write` switch, `AE_IND_LOG_CLOSE` → `complete`);
`1107-1265` KE fops (all requests answered `AE_NOT_AVAILABLE` when no oops is
queued, `1014-1020`); `1268-1300` `aed_ioctl_bt`; `1302-1526` `aed_ioctl`
(`1360-1435` `AEEIOCTL_GET_THREAD_REG`, `1460-1495`
`AEEIOCTL_CHECK_SUID_DUMPABLE`); `1844-1917` `external_exception`
(`GFP_ATOMIC`, `in_interrupt()` guard, the `aee_mode >=
AEE_MODE_CUSTOMER_USER` refusal at `1862`); `1961-1985`
`/proc/aed/current-ee-coredump`; `2036-2100` misc devices, `aed_init`/`aed_exit`
and the two registration failure strings.

`drivers/misc/mediatek/aee/common/aee-common.c`
: `224-300` `aed_md_exception_api` → `"modem"`, `aed_md32_exception_api` →
`"md32"`, `aed_scp_exception_api` → `"scp"`, `aed_combo_exception_api` →
`"combo"`, each `EXPORT_SYMBOL`.

`drivers/misc/mediatek/aee/aed/aed-process.c`
: `41-56` `per_cpu_get_bt` barrier; `58-92` `aed_save_trace` (fills `pc`/`lr` +
`%pS` symbols; `lr` only `#ifndef __aarch64__`); `94-137` `aed_get_bt`;
`171-204` `aed_get_process_bt` (`find_task_by_vpid`,
`cred_guard_mutex` + `ptrace_may_access(PTRACE_MODE_ATTACH)`).

`drivers/misc/mediatek/eccci/port_kernel.c`
: `2280-2338` `ccci_aed()` — `2295` `ccci_get_md_info_str()`, `2304`
`snprintf(buff, AED_STR_LEN, "md%d:%s%s", ...)`, `2307-2333` record selection
(CCIF_REG, EX_MEM = MDSS debug window, EX_PKT = `&md->ex_info`/`EX_LOG_T`,
MD_IMG_MEM = `md_region_vir`/`MD_IMG_DUMP_SIZE`), `2335-2336` the
`aed_md_exception_api(ex_log_addr, ex_log_len, md_img_addr, md_img_len, buff,
db_opt)` call; `2347`/`2904` `db_opt = DB_OPT_DEFAULT | DB_OPT_FTRACE`.

`drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c:646`
: `ccci_get_md_info_str()` returns `md_img_info_str[md_id]` — the image-info
suffix that this tree has no equivalent for (§6).

### 2.3 Vendor userspace ELF — `$M/zip/vendor-libs/libaed.so`

ELF32 ARM, Thumb-2, stripped, 22 sections; `.text` vaddr 0x3770-0xe268
(vaddr == file offset), `.rodata` 0xeb04-0x1120c. Tools:
`llvm-readelf -sW/-SW/-rW`, `llvm-objdump -d --triple=thumbv7a-none-eabi`, and
two small host helpers written for this analysis:

* a literal-pool scanner (word `W` at `L` resolves a `add rX, pc` string when
  `W + (L+4) == target`, the 16-bit Thumb rule);
* a veneer decoder: the linker emits 16-byte Thumb→ARM call veneers starting at
  `0xdc38`; each is `bx pc; nop; ldr r12,[pc]; add pc,r12,pc; .word off`, and
  `(V+0x10)+off` equals the ARM PLT entry `0x32c0 + 12*index` of `.rel.plt`.
  This maps every `bl 0xdc..` in the library to its libc symbol (e.g.
  `0xddc8 → open@plt 0x3404`, `0xddd8 → ioctl@plt 0x3410`,
  `0xdc98 → close@plt`).

Findings (all from disassembly; the annotated listings were produced from the
two tools above):

* **`aee_ioctl_get_reg`** (`dynsym 0x4675`, size 300; disassembly window
  `0x4674-0x475c`) opens `/dev/aed0` and issues **two** ioctls:
  * `0x4684 bl 0xddc8 <open>` with the string from the literal at `0x4764`
    (`0xaa24`; `add r0,pc` at `0x467e` has PC `0x4682` → `0xf0a6` =
    `"/dev/aed0"`, `.rodata` vaddr 0xf0a6);
  * `0x46be bl 0xddd8 <ioctl>` with `r1` from the literal at `0x4774` =
    **`0x41287007`** = `_IOW('p', 0x07, 296 B)`;
  * on failure `0x4702 bl 0xddd8 <ioctl>` with `r1` from the literal at
    `0x4784` = **`0x404c7007`** = `_IOW('p', 0x07, 76 B)`;
  * `0x4758 bl 0xdc98 <close>`. There is **no `read`/`write`** on this fd.
  A whole-library scan of 32-bit words for the `_IOC` pattern
  (`(dir<<30)|(size<<16)|('p'<<8)|nr`) yields exactly one command word,
  `0x404c7007` (file offset 0x4784); the second command word `0x41287007` is
  the literal at file offset 0x4774 loaded by `ldr r1,[pc,#0xc0]` at 0x46b2.
  Those are the library's only two kernel ioctl encodings, both for
  `AEEIOCTL_GET_THREAD_REG`. 76 = `sizeof(pid_t) + sizeof(struct pt_regs)` on
  ARM32 (72); 296 = 4 + 4 + 288 = `tid + pad + 3.18/4.x arm64 pt_regs`
  (34 u64 of `user_pt_regs`, `orig_x0`, `syscallno`) — [INFERENCE] for the
  internal field split, the size itself is the ABI.
* **`aee_aed_raise_exception`** (`dynsym 0x3b8d`, size 348) and the internal
  helper at `0x3828` raise events over the **local socket** to
  `com.mtk.aee.aed` (string at 0xeb5c, `aee_socket_local_client` at 0x3828-
  0x383a), building a 24-byte `struct AE_Msg` (`memset(...,24)`; `[0]=2`
  `AE_IND`; `[4]=0x0b/0x0c/0x0d/0x0e` = `AE_IND_FATAL/EXP/WRN/REM_RAISED`),
  writing it with `fop_safe_write(fd, msg, 0x18)` and reading the 24-byte reply
  with `fop_safe_read_timeout(fd, msg, 0x18, 0x258 /*600 ms*/)`.
* `aee_kernel_dump_user_ioctl_get_reg` (0x4d94) and
  `aee_kernel_dump_user_ioctl_get_userstack` (0x4e34) only read the library's
  cached `NativeInfo`/`userstack_info` buffers; no kernel calls.
* `/dev/aed1` does not appear in any string of the library — the KE device is
  never opened by libaed.so.

**Consequence.** libaed.so's only kernel interface is the 76/296-byte
`AEEIOCTL_GET_THREAD_REG` on `/dev/aed0`. The AE_Msg char-device protocol is the
AED daemon's interface (the Java `com.mtk.aee.aed` process answers the socket),
so it is implemented in the driver and exercised by the new tool, and the
detail/CCCI path is exercised from the kernel side by
`aed_md_exception_api()`.

## 3. Implementation

In this section a bare `file:line` (e.g. `aed-main.c:1844-1917`) refers to the
downstream tree; references to the files delivered here are written as
"this driver, ...".

### 3.1 `include/uapi/linux/aee.h` (new, 194 lines)

The client ABI, copied from `aed.h:39-167` and `mt-plat/aee.h:65-90` with the
citation for every value: `AE_INVALID`/`AE_NOT_AVAILABLE`/`AE_DEFAULT`,
`enum AEE_MODE`, `enum AE_PASS_METHOD`, `enum AE_CMD_TYPE`, the complete
`enum AE_CMD_ID` (values 0-24, `AE_REQ_USERSPACEBACKTRACE = 40`, `_USER_REG =
41`, `_USER_MAPS = 42`, `AE_CMD_ID_END = 43`), `struct AE_Msg` (24 B),
`struct aee_ioctl` (32 B), `AEE_NR_FRAME`/`AEE_SZ_SYMBOL_S`/`AEE_SZ_SYMBOL_L`
and `struct aee_bt_frame` (256 B), the 76-byte `struct aee_thread_reg` and the
296-byte `struct aee_thread_reg64`, and all twelve `AEEIOCTL_*` numbers with the
`_IOW`/`_IOR` expansion in a comment. Kernel and tool include the same file
(the kernel as `<uapi/linux/aee.h>`, the tool through `-I$(srctree)/include/uapi`
and `<linux/aee.h>`), and the driver re-derives the numbers with
`BUILD_BUG_ON(AEEIOCTL_x != _IOW(...))` at init (this driver, aed-main.c:1095-1107).

### 3.2 `drivers/misc/mediatek/aee/aed.h` (new)

Driver-private: `struct aed_eerec` (aed-main.c:155-167 subset — `assert_type`,
`exp_filename`, `ee_log`/`ee_log_size`, `ee_phy`/`ee_phy_size`, `msg`,
`db_opt`), `struct aee_process_bt` (`mt-plat/aee.h:80-84`), the
`aed_get_process_bt()` prototype and the shared `aed_find_task()` helper
(`find_task_by_vpid` under RCU + `get_task_struct`).

### 3.3 `drivers/misc/mediatek/aee/aed-main.c` (new, 1160 lines)

* **Devices.** `misc_register()` of `aed0` (EE) and `aed1` (KE), mode 0600,
  with the downstream fops (`open/release/poll/read/write/unlocked_ioctl`,
  `compat_ioctl` under `CONFIG_COMPAT`) — aed-main.c:2036-2090.
* **AE_Msg framing.** `msg_create`/`msg_destroy`/`msg_copy_to_user`
  (aed-main.c:203-260): the response is `struct AE_Msg` + `len` payload bytes,
  `read()` continues at `*f_pos`, `write()` resets `*f_pos` and invalidates the
  previous response.
* **EE record.** `external_exception()` (aed-main.c:1844-1917): refuses in
  `aee_mode >= AEE_MODE_CUSTOMER_USER`, allocates the record and the log with
  `GFP_ATOMIC`, copies `log_size` bytes (or a 16-byte zeroed placeholder),
  copies at most `MAX_EE_COREDUMP` of the image payload when not in interrupt
  context, `strscpy()`s `detail` into `exp_filename`, then
  `ee_queue_request()` (`list_add_tail` + `queue_work(system_wq)`). Nothing in
  this path sleeps and nothing blocks.
* **Worker / indication.** `ee_worker()` pops the record, `ee_gen_ind_msg()`
  publishes it (only one record at a time; a second one is dropped, logged and
  freed), builds `AE_IND`/`AE_IND_EXP_RAISED`/`arg=AE_EE`/`dbOption=db_opt`,
  arms the completion **before** publishing, wakes the wait queue and waits up
  to 5 minutes for `AE_IND_LOG_CLOSE`; afterwards the record is released.
* **EE requests.** `AE_REQ_CLASS` → `"External (EE)"`; `AE_REQ_TYPE` →
  `assert_type`; `AE_REQ_PROCESS` → `exp_filename`; `AE_REQ_DETAIL` →
  `ee_gen_detail_msg()`; `AE_REQ_BACKTRACE` → `AE_NOT_AVAILABLE` (aed-main.c:1038);
  `AE_REQ_COREDUMP` → `"/proc/aed/current-ee-coredump"`; anything else →
  `AE_NOT_AVAILABLE`. `AE_IND_LOG_CLOSE` completes the worker.
* **Detail + CCCI.** `ee_gen_detail_msg()` (aed-main.c:744-813): for
  `assert_type == "md32"` the raw log text; otherwise the
  `== EXTERNAL EXCEPTION LOG ==` hex block, `== MEM DUMP(ee_phy_size) ==`, and
  for `assert_type == "modem"` with `sscanf(exp_filename, "md%d:")` the CCCI
  debug window obtained from `aee_dump_ccci_debug_info(md, &win, &size)`
  (`<linux/ccci.h>`, EXPORT_SYMBOL_GPL, ccci_core.c:1116-1145) printed as
  `== CCCI LOG ==` + hex + `== MEM DUMP(size) ==`, with `arg = AE_PASS_BY_MEM`.
* **KE device.** No kernel-exception producer exists in this tree, so every
  `AE_REQ_*` is answered with the `AE_NOT_AVAILABLE` response, exactly the
  downstream behaviour with no queued oops (aed-main.c:1014-1020), and
  `AE_IND_LOG_CLOSE` is accepted and ignored.
* **ioctls.** `AEEIOCTL_GET_THREAD_REG` (76 B) and
  `AEEIOCTL_GET_THREAD_REG64` (296 B): `find_task_by_vpid` + `task_pt_regs`;
  the wide form copies the `user_pt_regs` prefix plus `orig_x0` and
  `syscallno`, the narrow form produces the AArch32 view and returns
  `-EOPNOTSUPP` for a 64-bit task. `AEEIOCTL_GET_PROCESS_BT` (frames via
  `aed_get_process_bt`, `detail = AE_DEFAULT`, `size = nr_entries`,
  `AEE_NR_FRAME` frames written to `ioctl.out`). `AEEIOCTL_SET_AEE_MODE`
  (stores the mode). `AEEIOCTL_CHECK_SUID_DUMPABLE`
  (`get_task_mm`/`get_dumpable`/`set_dumpable`). Everything else returns
  `-EOPNOTSUPP`.
* **/proc.** `/proc/aed/current-ee-coredump` (0400) streams `ee_phy`
  through `simple_read_from_buffer` (aed-main.c:1961-1985).
* **Entry points.** `aed_md_exception_api()` (`"modem"`),
  `aed_md32_exception_api()` (`"md32"`), `aed_scp_exception_api()` (`"scp"`),
  `aed_combo_exception_api()` (`"combo"`), all `EXPORT_SYMBOL_GPL`, all direct
  wrappers around `external_exception()` with no `__weak` and no registration
  indirection (deviation from `aee-common.c:224-300`, see §6).

Failure codes: `-ENODEV` write before any record exists; `-EINVAL` for a wrong
`AE_Msg` size, a bad pid/tid, or a missing register frame; `-EFAULT` for a bad
user pointer; `-EOPNOTSUPP` for the deferred ioctls and for the 76-byte
register view of a 64-bit task; `-ENOMEM` when the frame buffer cannot be
allocated.

### 3.4 `drivers/misc/mediatek/aee/aed-process.c` (new)

`aed_get_process_bt()`: `find_task_by_vpid` + task reference,
`mutex_lock_killable(&task->signal->cred_guard_mutex)` +
`ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS)` (aed-process.c:181-190),
`stack_trace_save_tsk(task, pcs, AEE_NR_FRAME, 0)`, and one `aee_bt_frame` per
PC with `pc` and `pc_symbol` (`%pS`). `lr`/`lr_symbol` stay zero, as in the
downstream arm64 build (aed-process.c:88-91).

### 3.5 CCCI wiring — `drivers/misc/mediatek/ccci/ccci_exception.c` (+38)

At the end of `ccci_ex_build()`, guarded by
`#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)`:

* `log/log_size` = the `record`/`record_len` already selected earlier in the
  function (EX_LOG_T from the `MD_EX_REC_OK` packet or from the EXREC smem
  window, port_kernel.c:2318-2333). Both are kernel-memory buffers there
  (`memcpy_fromio` into `smem_record`/`packet`), so the API's plain `memcpy`
  is safe.
* `phy/phy_size` = the first `CCCI_MD_IMG_DUMP_SIZE` (256, `ccci_priv.h:852`)
  bytes of `md->img_vir`, copied out with `memcpy_fromio`; NULL/0 when the
  image window is not mapped or too small (same predicate as
  `ccci_ex_dump_image()`).
* `detail` = `"md%u:%s"` with `md->id + 1` and the reason name
  (`ccci_ex_reason_name()`), the downstream `"md%d:%s%s"` shape of
  port_kernel.c:2304 without the image-info suffix.
* `db_opt = DB_OPT_DEFAULT | DB_OPT_FTRACE` (port_kernel.c:2347).

Only this block was added; no other logic changed.

### 3.6 Kconfig/Makefile

`drivers/misc/mediatek/aee/Kconfig`: `config MTK_AEE_FEATURE`, bool, "MediaTek
AEE/AED exception dump client", `depends on ARCH_MEDIATEK || COMPILE_TEST`.
`drivers/misc/mediatek/aee/Makefile`:
`obj-$(CONFIG_MTK_AEE_FEATURE) += aed-main.o` and `+= aed-process.o`.
One appended line each in `drivers/misc/mediatek/Kconfig`
(`source "drivers/misc/mediatek/aee/Kconfig"`) and
`drivers/misc/mediatek/Makefile` (`obj-$(CONFIG_MTK_AEE_FEATURE) += aee/`).

### 3.7 `tools/mediatek/aee/{Makefile,aee-db.c}` (new)

Host tool, `gcc -O2 -Wall -Wextra -Werror`, `-I$(srctree)/include/uapi`.
Commands: `query` (poll for a pending indication), `class`, `type`, `process`,
`module`, `backtrace`, `detail`, `coredump` (one `AE_REQ_*` each, printing the
`AE_Msg` header and the payload), `ccci [--md N]` (reads `AE_REQ_PROCESS`,
verifies the `mdN:` prefix, then `AE_REQ_DETAIL`, prints the payload and reports
whether `== CCCI LOG ==` is present, then sends `AE_IND_LOG_CLOSE`), `close`,
`regs --pid N` (try `AEEIOCTL_GET_THREAD_REG64`, fall back to
`AEEIOCTL_GET_THREAD_REG`, exactly like libaed.so), `bt --pid N`,
`--selftest`. `--selftest` = static assertions on every struct size/offset,
re-computation of the ioctl numbers with `_IOC`, the command-id values, and
encoder/decoder KATs (exact wire bytes and round trip for a request and an
indication).

## 4. Parity matrix

| behaviour | ROM / downstream reference | Nura before | after | status |
|---|---|---|---|---|
| `/dev/aed0`, `/dev/aed1` misc devices | ROM strings 45359-45360; aed-main.c:2036-2090 | absent | registered 0600, same names/fops | done |
| AE_Msg request/response, 24-byte header + payload | aed.h:110-125; aed-main.c:203-260, 975-1265 | absent | implemented | done |
| EE class/type/process/coredump responses | aed-main.c:685-838 | absent | implemented | done |
| EE backtrace | aed-main.c:1038 (`AE_NOT_AVAILABLE`) | absent | `AE_NOT_AVAILABLE` | done |
| EE detail incl. `== CCCI LOG ==` via `aee_dump_ccci_debug_info()` | aed-main.c:744-813; ccci_core.c:1116-1145 | absent | implemented (iomem-safe) | done |
| EE record queue + 5-min close timeout | aed-main.c:884-968 | absent | implemented | done |
| `aed_md/md32/scp/combo_exception_api()`, atomic-safe, exported | aee-common.c:224-300; ROM 0xc0628298… | absent | implemented, `EXPORT_SYMBOL_GPL`, `GFP_ATOMIC` | done |
| CCCI exception hands its record to AEE | port_kernel.c:2335-2336, 2304, 2329-2333, 2347 | not called | called from `ccci_ex_build()` | done |
| `AEEIOCTL_GET_THREAD_REG` 76-byte (client-verified) | libaed.so 0x4702/pool 0x4784; aed-main.c:1360-1435 | absent | implemented (AArch32 view) | done |
| `AEEIOCTL_GET_THREAD_REG64` 296-byte (client-verified command) | libaed.so 0x46be/pool 0x4774; size field 0x128 | absent | implemented (`user_pt_regs` + `orig_x0` + `syscallno`) | done |
| `AEEIOCTL_GET_PROCESS_BT` | aed-main.c:1268-1300; aed-process.c:171-204 | absent | implemented (`stack_trace_save_tsk`) | done |
| `AEEIOCTL_CHECK_SUID_DUMPABLE`, `SET_AEE_MODE` | aed-main.c:1460-1495, 1437-1448 | absent | implemented | done |
| `/proc/aed/current-ee-coredump` | aed-main.c:1961-1985 | absent | implemented | done |
| AE_Msg raise path of libaed.so | `aee_socket_local_client` → `com.mtk.aee.aed` (libaed.so 0x3828) | n/a | not kernel: it is the userspace daemon socket, out of kernel scope | deferred — needs the AED daemon (Java) binary/service |
| KE exception capture (kernel oops → `/dev/aed1`) | aee_oops_create()/ipanic/mrdump (aed-main.c:1107-1265) | absent | device exists, requests → `AE_NOT_AVAILABLE` | deferred — needs `aee_oops`/ipanic/mrdump producer (§6) |
| `aee_kernel_exception_api()`/`warning`/`reminding`, `aee_kernel_dal_api` | aed-main.c:1726-1842; aee-common.c:150-220 | absent | not implemented | deferred — needs the KE oops path |
| DAL show/clean/setcolor ioctls | aed-main.c:1290-1375 (DAL framed buffer) | absent | `-EOPNOTSUPP` | deferred — needs the DAL/FB assert layer (`disp_assert_layer.h`) |
| red screen (`AEE FORCE_RED_SCREEN`) / RT monitor / WDT powerkey / SF state | aed-main.c:1497-1526; mt-plat/aee.h:210-215 | absent | `-EOPNOTSUPP` | deferred — need `/dev/RT_Monitor`, `/dev/kick_powerkey`, SurfaceFlinger state |
| `aed-debug.c` test entries, `aed_proc_init` extra files | aed-main.c:1970-2015; aed-debug.c | absent | not ported (test/debug only) | deferred — need ipanic/dram console/kprobe test scaffolding |
| `aee_dump_ccci_debug_info()` | ccci_core.c:1116-1145 | already exported | consumed by the detail path | done |

## 5. Build evidence

Environment: `M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`,
`M2=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work`,
`TMPDIR=$M2/tmp`, `PATH=$M/llvmpkg/usr/bin:$M/bin:$PATH`, worktree
`$M2/worktrees/rem2-aee`.

Config: `cp $M/nura.config .config`, then
`./scripts/config --enable MTK_AEE_FEATURE --enable MTK_CCCI` and
`make ARCH=arm64 LLVM=1 olddefconfig` (rc=0). Result:
`CONFIG_MTK_AEE_FEATURE=y`, `CONFIG_MTK_CCCI=y`, `CONFIG_MTK_CCCI_CLDMA=y`,
`CONFIG_MTK_CCCI_UTIL=y`, `CONFIG_MTK_CCMNI=y` (the last four are the
`select`s of `MTK_CCCI`, `drivers/misc/mediatek/ccci/Kconfig:1-14`).
`$M/nura.config` itself was not modified; no other tree config was touched.

Kernel objects (W=1, `-j2`):

```
$ make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/aee/
  CC      drivers/misc/mediatek/aee/aed-main.o
  CC      drivers/misc/mediatek/aee/aed-process.o
  AR      drivers/misc/mediatek/aee/built-in.a
rc=0, warning: 0, error: 0

$ make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci/ccci_exception.o
  CC      drivers/misc/mediatek/ccci/ccci_exception.o
rc=0, warning: 0, error: 0
```

Cross-checks (same commands, config flips, then the final config restored):

* `CONFIG_MTK_CCCI=n`, `MTK_AEE_FEATURE=y` → aed-main.o/aed-process.o rc=0,
  0 warnings (the `AED_HAS_CCCI_DEBUG_INFO` path compiles away).
* `CONFIG_MTK_AEE_FEATURE=n`, `MTK_CCCI=y` → ccci_exception.o rc=0,
  0 warnings (the call site and the header's static-inline no-ops agree).
* `make ... drivers/misc/mediatek/ccci/` rc=0; it emits one pre-existing,
  unrelated warning in `drivers/misc/mediatek/ccci/ccci_ringbuf.c:209`
  (`variable 'read' set but not used`) — not from this leaf.

Symbol check (`llvm-nm`): `aed-main.o` defines `aed_md_exception_api`,
`aed_md32_exception_api`, `aed_scp_exception_api`, `aed_combo_exception_api`
(each with a `.export_symbol` entry, i.e. `EXPORT_SYMBOL_GPL` under the 7.x
export mechanism) and has `U aee_dump_ccci_debug_info`;
`ccci_exception.o` has `U aed_md_exception_api`;
`ccci_core.o` defines and exports `aee_dump_ccci_debug_info`.

Userspace tool:

```
$ cd tools/mediatek/aee && make
cc -I../../../include/uapi -O2 -Wall -Wextra -Werror -o aee-db aee-db.c
rc=0, warning: 0

$ make check
./aee-db --selftest
selftest: Oct 25 2026 OK: 5 KAT groups, 24-byte AE_Msg, 32-byte aee_ioctl, 0x404c7007/0x41287007 thread-reg ioctls
rc=0
```

The built `aee-db` binary is listed in `tools/mediatek/aee/.gitignore` and is
not part of the patch; the kernel objects/`built-in.a`/`modules.order` are
covered by the kernel's own ignore rules and are likewise not in the patch
(the delivered patch contains exactly the 12 source files of §1).

Patch application test: the patch was `git apply --check`ed and then applied to
a directory holding the pristine baseline blobs of the three touched baseline
files; the result is byte-identical to this worktree (`diff -q` clean, all four
checked files). No full `vmlinux` build was run.

## 6. Hardware-unverified items and deliberate deviations

Hardware-unverified (no device): every runtime path — the misc devices, the
EE/KE protocol, the CCCI hand-off, the ioctls. Only compilation, the ABI
assertions and the patch application were exercised.

Deviations, each deliberate:

1. **`aee_mode` default.** Downstream starts at `AEE_MODE_NOT_INIT`, which makes
   `external_exception()` drop everything until the (closed-source) AED daemon
   sets a mode through `AEEIOCTL_SET_AEE_MODE` (aed-main.c:1862). This tree has
   no daemon, so the default is `AEE_MODE_MTK_USER` and the ioctl still
   overrides it. Without this the CCCI hand-off would be dead code.
2. **Direct entry points.** `aed_md_exception_api()` and the md32/scp/combo
   variants are implemented directly instead of going through
   `g_aee_api->md_exception` (`aee-common.c:224-300`); nothing else registers
   that table in this tree, so the indirection would only add an unused
   registration API.
3. **Record payloads are `kmalloc`'ed with `GFP_ATOMIC`** instead of
   `vmalloc_user()` (aed-main.c:1893-1907). `vmalloc` may sleep and the API
   must be callable from atomic context; the coredump is served by
   `/proc/aed/current-ee-coredump` instead of a user mapping. A very large
   `phy_size` (up to `MAX_EE_COREDUMP`) can therefore fail to allocate, which is
   logged by omission of the payload, exactly as downstream drops an
   unallocatable `ee_phy`.
4. **CCCI window read with `memcpy_fromio()`** and a detail message sized to the
   exact byte count (downstream dereferenced the window directly and
   over-allocated by 4x, aed-main.c:779-781). The Nura CCCI window is
   `ioremap`ed or `dma_alloc_coherent`ed (ccci_boot.c:302/332/382), so plain
   loads are not valid on arm64.
5. **`read()` clamps a short user buffer** to the rest of the message instead of
   returning `-EFAULT` after shortening `count` (aed-main.c:240-255).
6. **Second EE record is freed, not the current one.** Downstream's
   `ee_worker()` destroys `aed_dev.eerec` even when it just dropped the queued
   record (aed-main.c:956-967); this port frees the dropped record.
7. **Completion armed before publication** so an immediate `AE_IND_LOG_CLOSE`
   cannot be missed (downstream arms it after publishing, aed-main.c:901-904).
8. **`-EINVAL`/`-ENODEV`/`-EOPNOTSUPP` instead of the bare `-1`** the downstream
   character handlers return for a bad size or a missing record.
9. **`aee_mode`-independent device registration / no `ksysfs_bootinfo`**
   (`aed_init`, aed-main.c:2050-2100) — the boot-info proc entries belong to the
   KE/ipanic subsystem, which is not ported.
10. **`AEEIOCTL_GET_PROCESS_BT` unwinds with `stack_trace_save_tsk()`** and
    without the cross-CPU stop barrier (aed-process.c:41-56, 139-166). The
    barrier only widened the sample window on the 32-bit kernel.
11. **`AEEIOCTL_GET_THREAD_REG` (76-byte) returns `-EOPNOTSUPP` for a 64-bit
    task**; downstream's 32-bit encoding cannot exist on an arm64 kernel, and
    its own `struct aee_thread_reg` is `sizeof(pt_regs)`-dependent
    (mt-plat/aee.h:87-90).
12. **`AEEIOCTL_SET_FORECE_RED_SCREEN` is not implemented** (returns
    `-EOPNOTSUPP`) rather than storing a flag that nothing consumes; the red
    screen itself is deferred.
13. **CCCI image-info suffix omitted** from the detail string: downstream
    appends `ccci_get_md_info_str()` (port_kernel.c:2295,
    ccci_util_lib_load_img.c:646), which Nura does not have. The `"md%d:"`
    prefix, which is the part the AED detail path parses, is exact.

[INFERENCE] items: the internal split of the 296-byte register payload
(`regs[0..33]` = `user_pt_regs`, `regs[34]` = `orig_x0`, `regs[35]` =
`syscallno`) and the AArch32 mapping of the 76-byte payload (`regs[13]` = `sp`,
`regs[15]` = `pc`, `regs[16]` = `pstate`, `regs[17]` = `orig_x0`). The command
values and the payload sizes are not inferred — they are read from
libaed.so.

**ABI pieces absent from every available source** (and therefore not
implemented/invented):

* The field names of the 296-byte register payload: no source defines the struct
  (downstream's `struct aee_thread_reg` uses the *kernel's* `pt_regs`); only the
  command number `0x41287007` and its 296-byte size exist.
* The userspace `AE_Msg` payload layouts for `AE_REQ_USERSPACEBACKTRACE`,
  `AE_REQ_USER_REG`, `AE_REQ_USER_MAPS` (aed-main.c:472-560 describes them, but
  they need `struct aee_user_thread_stack/maps` and an `aee_oops`, i.e. the KE
  producer).
* Anything behind the deferred features: `aee_oops_create()`/ipanic/mrdump/DRAM
  console (`mrdump.h`, `mt-plat/mtk_ram_console.h`), the DAL assert layer
  (`disp_assert_layer.h`), `/dev/RT_Monitor`, `/dev/kick_powerkey`, the
  SurfaceFlinger state ioctls, and `AED_STR_LEN` image-info text.

## 7. DTS-FRAGMENT

None. The driver needs no DT node or property: both devices are
`MISC_DYNAMIC_MINOR` misc devices, the proc entry is created at init, and the
exception data arrives through the existing CCCI objects (`struct ccci_md`
`smem_vir`/`img_vir`, already described by the Nura CCCI bindings). No DTS/DTSI
file was touched.

## 8. Patch path

`$M/patches/rem2-aee.patch` (delta against the parity baseline: 12 files,
+2378, 0 deletions; `git apply --check` clean, applied content verified
byte-identical). Report: `$M/logs/rem2-aee.md`.

How the delta was produced: the parity baseline is applied but uncommitted in
this worktree, so `git status --porcelain` necessarily also lists the baseline
paths. To keep the deliverable a clean delta, the patch was generated with a
temporary `GIT_INDEX_FILE` whose base tree is `HEAD` plus the three baseline
blobs that this leaf touches (`ccci_exception.c` `fa2fbff6c`, `mediatek/Kconfig`
`cba2e7fc4`, `mediatek/Makefile` `e77fb43c0`), and then
`git diff --binary <base-tree> <worktree-tree>`. Owned files in the worktree
are: `drivers/misc/mediatek/aee/` (5 files), `include/uapi/linux/aee.h`,
`tools/mediatek/aee/` (3 files) and the two one-line appends plus the CCCI
block in `drivers/misc/mediatek/ccci/ccci_exception.c`. The frozen
`include/linux/aee.h` is not part of the patch.
