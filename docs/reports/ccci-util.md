# ccci-util — CCCI utility/log/dump driver (SM-G532M MT6737T ROM → Nura 7.0.10 arm64)

Worktree: `$M/worktrees/ccci-util` (base `046883c0c`)
Patch: `$M/patches/ccci-util.patch`
Config symbol: `MTK_CCCI_UTIL` (bool)

## 1. Files changed

| File | Status | Purpose |
|---|---|---|
| `include/linux/ccci.h` | new, **byte-identical** to `CCCI_CONTRACT.md` §5 | frozen shared CCCI API |
| `drivers/misc/mediatek/ccci_util/ccci_util.h` | new | private header: downstream check-header structs (verbatim), dump types/flags, image helper and ring/dump/device prototypes |
| `drivers/misc/mediatek/ccci_util/ccci_util.c` | new | `ccci_util_init/exit`, `ccci_util_log_init/exit`, `ccci_util_log`, `ccci_util_dump_mem`, per-MD log + 6 dump rings, debugfs `ccci/mdN/{log,dump,log_stats}`, `ccci_dump_write`, `ccci_util_mem_dump`-family plumbing, `get_dump_buf_usage` |
| `drivers/misc/mediatek/ccci_util/ccci_util_dump.c` | new | hex/ASCII dump, downstream word/compact dump, modem-smem window registry, debugfs `ccci/dump` (`md_id addr len`) and `ccci/dump_window` |
| `drivers/misc/mediatek/ccci_util/ccci_util_img.c` | new | MD check-header v3/v4/v5 parse + validate, DSP check, RMPU region/domain extraction, `ccci_get_md_info_str` |
| `drivers/misc/mediatek/ccci_util/ccci_util_log.c` | new | `/dev/ccci_log`, `/dev/ccci_dump` misc devices, blocking/O_NONBLOCK read, poll, raw dump-record write, per-MD ioctl, event log |
| `drivers/misc/mediatek/Kconfig`, `drivers/misc/mediatek/Makefile` | new | `menu "MediaTek device drivers"` + `source …/ccci_util/Kconfig`, `obj-y += ccci_util/` |
| `drivers/misc/mediatek/ccci_util/{Kconfig,Makefile}` | new | `MTK_CCCI_UTIL` bool; four objects |
| `drivers/misc/Kconfig`, `drivers/misc/Makefile` | modified, 1 line each | `source "drivers/misc/mediatek/Kconfig"`, `obj-y += mediatek/` |

No shared DTS, `$M/nura.config` or other leaf files were touched. `git status --porcelain` lists exactly the files above.

Dependency direction: `ccci_util` calls **no** core function (`ccci_register_modem`, `ccci_md_get_state`, `get_smem_start_addr`, … are absent). The only cross-driver flow is the contract's core → util API, plus two optional registration helpers exported for the core (`ccci_util_dump_set_window/clear_window`).

## 2. ROM evidence

### Symbols (llvm-nm on `$M/zip/vmlinux.elf`, 32-bit ARM)

| Symbol | Address | Used for |
|---|---|---|
| `ccci_util_init` | `0xc0db2a58` | initcall shape |
| `ccci_log_init` | `0xc0504e04` | ring/dump/device init |
| `ccci_log_fops` / `ccci_dump_fops` | `0xc0a8f9a0` / `0xc0a8fa18` | open/read/poll/close fops |
| `ccci_log_poll/open/close/read` | `0xc05032f8`, `0xc0503348`, `0xc0503390`, `0xc05033e4` | log device semantics |
| `ccci_dump_poll/open/close/read` | `0xc05033d0`, `0xc0503678`, `0xc0503610`, `0xc05036d4` | dump device semantics |
| `ccci_log_write`, `ccci_log_write_raw` | `0xc0503d7c`, `0xc0503fb4` | global 4 KiB debug ring |
| `ccci_dump_write` | `0xc0504248` | per-MD dump ring writer |
| `ccci_util_mem_dump`, `ccci_util_cmpt_mem_dump` | `0xc05046f4`, `0xc0504870` | word-dump formats |
| `ccci_event_log`, `ccci_event_log_cpy` | `0xc0504a94`, `0xc0504d60` | event ring |
| `get_dump_buf_usage` | `0xc05045b4` | ring usage report |
| `ccci_get_md_info_str` | `0xc0501624` | MD info string |
| `ccci_get_md_check_hdr_inf` | `0xc0501f28` | header parse entry |
| `ccci_get_opt_val`, `ccci_dump_opt_tbl` | `0xc04fef04`, `0xc04fec50` | deferred (opt/meta) |

`$M/zip/symbols.txt` lines 20991–21060, 44481–44482, 54323 list the same set.

### Disassembly

- `ccci_log_init` (`0xc0504e04`) calls `proc_create_data` (`0xc0197e64`) at `0xc0504e30` and `0xc0504e98` with mode `292` (`S_IFREG|0444`) and names `ccci_log`/`ccci_dump`; it allocates a 4096-byte ring via `kmem_cache_alloc_trace` (`0xc013aad4`) and calls `__init_waitqueue_head` (`0xc0060dcc`). → confirms a single 4 KiB log ring + a separate dump interface.
- `ccci_util_mem_dump` (`0xc05046f4`) loops 16 bytes/line calling `ccci_dump_write` (`0xc0504248`), i.e. the downstream `%03X: %08X %08X %08X %08X` format reproduced here.

### Real firmware header check (parser validation)

`$M/zip/CP_G532MUBU1ASA1_…tar.md5` contains `md1rom.img` (10,514,944 B). It carries a real check header at file offset `0x00a065f4`:

```
CHECK_HEADER, header_verno=3, product_ver=2 (release), image_type=5 (lwg),
platform="MT6735_S00", build_time="2019/01/08 05:12",
build_ver="MOLY.LR9.W1444.MD.LWTG.TC10.SP.W16.40",
bind_sys_id=1, mem_size=0x04d00000, md_img_size=0x00e00000,
dsp_img_offset=0x00c00000, dsp_img_size=0x00200000,
size(field at +280)=0x11c = 284
```

All fields satisfy the implemented v3 validation (magic, v3 layout 284, `size` field 284, `bind_sys_id == md_id+1`, `image_type ≤ 21`, `dsp_offset+dsp_size ≤ md_mem_size`). Struct sizes/offsets were confirmed independently with a host C program: v3 = v4 = 284, v5 = 344; offsets `verno 12, product 16, image_type 20, platform 24, build_time 40, build_ver 104, bind 168, mem 172, img 176, rpc 180, dsp 184/188, region_num 192, region_info 196, domain_attr 260, arm7 276/280, size 280/340`. They match the parser constants and the `static_assert()`s.

## 3. Downstream references

- `ccci_util_lib_main.h:15-37` (v3), `:39-66` (v4), `:68-99` (v5) — structs copied verbatim into `ccci_util.h`.
- `mt-plat/mt_ccci_common.h:136-139` (`struct _md_regin_info`), `:89` (magic), `:344-358` (`CCCI_ERR_LOAD_IMG_*`), `:896-906` (`CCCI_DUMP_*` flags/types), `:164/:190` (region/domain totals), `:52-65` (MD type enum, `MAX_IMG_NUM = 21`).
- `ccci_util_lib_load_img.c:54-75` (`type_str`), `:84-88` (`product_str`), `:102` (`check_md_header_v3`), `:143-274` (mem-size/DSP/region checks), `:285` (`md_check_header_parser`), `:508` (`check_md_header`), `:646` (`ccci_get_md_info_str`), `:1059` (`ccci_get_md_check_hdr_inf`), `:1116` (`check_if_bypass_header`).
- `ccci_private_log.c:36-37` (ring/max write), `:54/:104` (`ccci_log_write[_raw]`), `:165/:212/:223/:231` (log read/poll/open/close), `:248-253` (dump buffer sizes), `:309` (buffer table), `:328` (`ccci_dump_write`), `:431` (separator strings), `:472/:615/:620/:636` (dump read/poll/open/close), `:734` (`get_dump_buf_usage`), `:752/:793` (word/compact dumps), `:867/:941` (event log), `:969` (`ccci_log_init`).
- `ccci_util_lib_main.c:28-38` — `subsys_initcall(ccci_util_init)`.
- `eccci/ccci_core.c:243-256` and `:773` — MD reserve/smem layout and `get_smem_start_addr()` (why the dump window is an explicit registration on this tree).

## 4. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| MD check-header v3/v4/v5 magic + verno + `size` field | `load_img.c:102,285,508`; ROM `ccci_get_md_check_hdr_inf` | absent | `ccci_util_md_check_header()` | done |
| `bind_sys_id`, `image_type`, `mem_size`, `md_img_size` validation | `load_img.c:143-200` | absent | same checks; `-CCCI_ERR_LOAD_IMG_MD_CHECK/CHECK_HEAD` | done |
| DSP offset/size sanity (`0xCDCDCDAA`, 64 KiB align for v3, range) | `load_img.c:225-251` | absent | same | done |
| RMPU region table + domain attrs (v4/v5) | `load_img.c:254-274` | absent | parse + `ccci_util_md_region_info()` | done |
| ARM7 offset/size (v5) | `load_img.c:378-386` | absent | parsed into info | done |
| `ccci_get_md_info_str()` | `load_img.c:646`; ROM `0xc0501624` | absent | same format, per-MD 256-byte buffer | done |
| Per-MD MD-log ring, timestamps, overflow accounting | new contract API (downstream ring is global) | absent | 4 KiB ring/MD, `[ts][cpu][pid:comm][ccciN/util]`, `overflow`/`total` counters | done |
| 6 dump buffers per MD (init/bootup/normal/repeat/mem_dump/history) | `private_log.c:248-309` | absent | same sizes per MD, rings | done |
| Dump stream separators (`BUFF=n`, ` Dump … log `) | `private_log.c:431-470` | absent | same 64-byte separator layout | done |
| debugfs views | downstream `/proc/ccci_log`, `/proc/ccci_dump`, sysfs | absent | `ccci/mdN/log` (rw), `ccci/mdN/dump` (ro), `ccci/mdN/log_stats` | done (interface modernised) |
| Hex/ASCII memory dump | new (downstream word-only) | absent | `ccci_util_dump_hex()`, 16 B/line, offset+hex+ASCII | done |
| `ccci_util_mem_dump` / `ccci_util_cmpt_mem_dump` | `private_log.c:752,793`; ROM symbols | absent | identical formats, `Base:` line | done |
| `ccci_util_dump_mem()` contract API | — | absent | hex/ASCII into `CCCI_DUMP_MEM_DUMP` | done |
| Debugfs `dump` request `(md_id, addr, len)` against MD smem | downstream AED dump via `get_smem_start_addr` | absent | range-checked (`-EINVAL` outside window, `-ENODEV` no window), output into mdN dump ring | done (window must be registered) |
| MD smem window discovery | core resolves reserved-memory | — | `ccci_util_dump_set_window()` exported + `ccci/dump_window` debugfs | partial: no automatic DT discovery |
| `/dev/ccci_log`: blocking read, `-EAGAIN` on O_NONBLOCK, poll, single reader, `-EBUSY` | `private_log.c:165-243`; ROM `0xc05032f8…` | absent | misc device, same semantics | done |
| `/dev/ccci_dump`: non-blocking dump read, poll always readable, per-MD buffers | `private_log.c:472-670`; ROM `0xc05033d0…` | absent | misc device; snapshot read; ioctl `SET_MD/GET_MD` | done |
| Raw dump-record write (`type`+`len` header) | ROM has no user write path (`proc` 0444) | absent | `/dev/ccci_dump` write appends record to selected MD/ring | done (new interface) |
| `ccci_event_log` / `ccci_event_log_cpy` | `private_log.c:867,941`; ROM `0xc0504a94` | absent | 4 KiB ring, RTC + monotonic stamp | done |
| `get_dump_buf_usage` | `private_log.c:734`; ROM `0xc05045b4` | absent | usage/overflow report | done |
| `ccci_util_fo_init`, block-partition image file I/O | `ccci_util_lib_fo.c` | absent | not implemented — firmware comes from core `request_firmware()` | deferred |
| `ccci_load_firmware`, `get_raw_check_hdr`, `ccci_get_md_check_hdr_inf` | `load_img.c:731,1059` | absent | core owns the firmware blob; util exposes the header parser only | deferred |
| Secure image auth (`ccci_init_security`) | `load_img.c:934` | absent | not implemented | deferred |
| Meta/opt parsing (`ccci_get_opt_val`, `ccci_dump_opt_tbl`, `ccci_parse_meta_md_setting`, `ccci_md_mem_reserve`) | `ccci_util_lib_sys.c`, `_fo.c`; ROM symbols | absent | — | deferred |
| `/sys/kernel/ccci/*` attributes (`boot`, `md_en`, …) | `ccci_util_lib_sys.c` | absent | — | deferred (agreed with ccci-core: nobody owns it; documented in both reports) |
| v1/v2 and v6 check headers | `load_img.c:539,285` | absent | rejected with `-EOPNOTSUPP` | deferred (v3/v4/v5 only, as assigned) |
| `/proc/ccci_log`, `/proc/ccci_dump` | ROM `proc_create_data` | absent | replaced by `/dev` + debugfs | done with documented divergence |

## 5. Compile test

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-util"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI_UTIL
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
  drivers/misc/mediatek/ccci_util/ccci_util.o \
  drivers/misc/mediatek/ccci_util/ccci_util_dump.o \
  drivers/misc/mediatek/ccci_util/ccci_util_img.o \
  drivers/misc/mediatek/ccci_util/ccci_util_log.o
```

Result: `olddefconfig` wrote `.config` with `CONFIG_MTK_CCCI_UTIL=y`; the four `CC` lines completed and `make` exited **0**.

```
$ make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/ccci_util/ccci_util.o \
      drivers/misc/mediatek/ccci_util/ccci_util_dump.o \
      drivers/misc/mediatek/ccci_util/ccci_util_img.o \
      drivers/misc/mediatek/ccci_util/ccci_util_log.o
  CC      drivers/misc/mediatek/ccci_util/ccci_util.o
  CC      drivers/misc/mediatek/ccci_util/ccci_util_dump.o
  CC      drivers/misc/mediatek/ccci_util/ccci_util_img.o
  CC      drivers/misc/mediatek/ccci_util/ccci_util_log.o
$ echo $?
0
```

Additional checks:
- `make ARCH=arm64 LLVM=1 W=1 -j2 drivers/misc/mediatek/ccci_util/` → exit 0, **no warnings**.
- `make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/` → built `ccci_util/built-in.a` and `mediatek/built-in.a` (validates the Kbuild recursion and `obj-y += mediatek/` hook).

No full kernel build was run.

## 6. Inferred / unverified on hardware

- **`[INFERENCE]` debugfs dir numbering** `ccci/mdN` uses `md_id + 1` (md1, md3), matching downstream `[ccci%d/util]` and the contract's "MD SYS counts from 1" convention.
- **`[INFERENCE]` `/dev/ccci_dump` ioctl ABI**: `_IOW('C', 0x40, u32)` / `_IOR('C', 0x41, u32)`; chosen above the highest downstream `CCCI_IOC_*` number (119) to avoid collision. No ROM equivalent exists.
- **`[INFERENCE]` raw dump-record wire format** `{le32 type; le32 len; u8 data[];}` (one record per `write()`); the request said "length/type header" without an encoding.
- **`[INFERENCE]` dump window mapping**: `memremap(phys, size, MEMREMAP_WB)` for the AP view of MD shared memory. Not exercised on hardware; no MD smem base is available to util (core resolves it from reserved memory, downstream `eccci/ccci_core.c:243-256`). Until someone calls `ccci_util_dump_set_window()` (or writes `ccci/dump_window`), `ccci/dump` requests return `-ENODEV` — the documented one-way-dependency consequence.
- **`[INFERENCE]` `ap_platform` / `md_mem_size` / `ap_img_size`** are caller-supplied to `ccci_util_md_check_header()`; util has no AP platform string or reserved-size source. Info string prints `unknown`/`0` when not supplied.
- **v1/v2 and v6 headers** are rejected (`-EOPNOTSUPP`), not parsed.
- **Divergences from ROM, deliberate**:
  - ROM/downstream expose `/proc/ccci_log` and `/proc/ccci_dump` via `proc_create_data` (ROM `0xc0504e30`, `0xc0504e98`); this driver exposes `/dev/ccci_log` (0400) and `/dev/ccci_dump` (0600) as instructed.
  - `/dev/ccci_dump` read returns one consistent snapshot per open (downstream streams continuously through persistent read indices).
  - `ccci_event_log()` prints milliseconds in the `.%03d` field; downstream passed microseconds into `%03d` (a formatting bug).
  - Image-header `image_type`/`product_ver` are bounds-checked before string lookup (downstream indexes `type_str[]`/`product_str[]` unvalidated).
- **Hardware-only unknowns**: actual MD log traffic rates/overflows, real `/dev/ccci_log` blocking wakeups, MD smem read-side effects (none expected, read-only), and whether the C2K/MD3 instance ever opens the dump device with a filter.
- `ccci_util_log_init(md_id, dev)` allocates ~416 KiB of rings per MD (6 dump buffers + 4 KiB log), matching downstream sizes; it is only called for instantiated modems (MD1 and optionally MD3 on this platform).

## 7. DTS-FRAGMENT

None required. The driver is not a platform driver, maps no registers, and takes no DT properties. The modem shared-memory window used by `ccci/dump` is provided at runtime by `ccci_util_dump_set_window(md_id, phys, size)` (or `echo "md_id phys size" > /sys/kernel/debug/ccci/dump_window`); the physical base belongs to the reserved region already described by the downstream `mdcldma@1000A000` / `reserve-memory-ccci_md1` nodes and is owned by the CCCI core.

```
/* no DTS change */
```

## 8. Patch

`$M/patches/ccci-util.patch` (12 files, staged with `git add -A`, `git diff --cached --binary`, not committed).
