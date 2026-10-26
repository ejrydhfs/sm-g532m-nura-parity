# vcodec gap: SYSRAM, non-cache buffer and pmem/ion commands

Date: 2026-10-24
Worktree: `$M/worktrees/vcodec-gap-commands` (pristine base `046883c0c` + staged
`$M/patches/vcodec-integration.patch`)
ROM: SM-G532M `3.18.19-14373813`, `$M/zip/vmlinux.elf`
Downstream: `$M/src/downstream-grandpplte`

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

**Result: no functional closure for the 13 commands (no implementable semantics);
one dispatch-consistency fix is delivered.**
All 13 commands are dead in the shipped ROM, so `VCODEC_ALLOC_NON_CACHE_BUFFER` and
`VCODEC_FREE_NON_CACHE_BUFFER` stay blocked and the other eleven stay closed. The only code
change (`vcodec_compat_ioctl()` now answers `-EOPNOTSUPP`, not `-ENOTTY`, for the
known-but-unimplemented commands, matching the native dispatcher) is one `if` block in
`vcodec-core.c`; it adds no command semantics.

---

## 1. Files changed

`drivers/misc/mediatek/vcodec/vcodec-core.c` — compat dispatch consistency (9 lines added,
0 removed, `git diff --stat`: `1 file changed, 9 insertions(+)`).
Patch: `$M/patches/vcodec-gap-commands.patch` (767 bytes). No other file touched; the rest
of the worktree is the pre-staged baseline.

---

## 2. Evidence

### 2.1 Downstream MT6735 dispatcher (D1, the MT6735 variant)

`drivers/misc/mediatek/videocodec/mt6735/Makefile:16-34` shows D1/D2/D3 are three
*different SoCs*, not three versions of one driver:

```
16: ifeq ($(strip $(CONFIG_ARCH_MT6735)),y)
18:     videocodec_kernel_driver_D1.o
23: ifeq ($(strip $(CONFIG_ARCH_MT6735M)),y)
25:     videocodec_kernel_driver_D2.o
30: ifeq ($(strip $(CONFIG_ARCH_MT6753)),y)
32:     videocodec_kernel_driver_D3.o
```

D1 is therefore the only valid MT6735 reference; D2 (MT6735M) and D3 (MT6753) are other
SoCs.

The complete `switch (cmd)` of `vcodec_unlocked_ioctl()` (`videocodec_kernel_driver_D1.c:1244`,
switch at `:1259`) has exactly these cases (enumerated from the source, nothing elided):
`SET_THREAD_ID`, `ALLOC_NON_CACHE_BUFFER`, `FREE_NON_CACHE_BUFFER`,
`INC/DEC_{DEC,ENC}_EMI_USER`, `LOCKHW`, `UNLOCKHW`, `INC/DEC_PWR_USER`, `WAITISR`,
`INITHWLOCK`, `DEINITHWLOCK`, `GET_CPU_LOADING_INFO`, `GET_CORE_LOADING`,
`GET_CORE_NUMBER`, `SET_CPU_OPP_LIMIT`, `MB`, `default`.

The two non-cache cases are log-only:

```c
/* videocodec_kernel_driver_D1.c:1266-1274 */
	case VCODEC_ALLOC_NON_CACHE_BUFFER: {
		MODULE_MFV_LOGE("Don't support VCODEC_ALLOC_NON_CACHE_BUFFER - tid = %d\n", current->pid);
	}
	break;

	case VCODEC_FREE_NON_CACHE_BUFFER: {
		MODULE_MFV_LOGE("Don't support VCODEC_FREE_NON_CACHE_BUFFER - tid = %d\n", current->pid);
	}
	break;
```

No `copy_from_user`, no allocation: the caller's `VAL_MEMORY_T` is never read or written.

Everything else falls through to:

```c
/* videocodec_kernel_driver_D1.c:1562-1568 */
	default: {
		MODULE_MFV_LOGE("========[ERROR] vcodec_ioctl default case======== %u\n", cmd);
	}
	break;
	}
	return 0xFF;
}
```

`return 0xFF` is unconditional, including for *successful* commands (`:1568`).

Compat path (`:1892-2027`): `ALLOC_NON_CACHE_BUFFER`/`FREE_NON_CACHE_BUFFER` (`:1897-1926`)
run a `VAL_MEMORY_T` compat copy, call the native ioctl (which only logs), then copy the
unchanged struct back. Every other command goes through `default:` (`:2020-2021`) straight
to `vcodec_unlocked_ioctl()`. So 32-bit userspace sees the same log-only behaviour.

There is no handler at all (no `case`, no helper) anywhere in the downstream tree for
`MFV_ALLOC_MEM_CMD`, `MFV_FREE_MEM_CMD`, `MFV_MAKE_PMEM_TO_NONCACHED`,
`MFV_ALLOC_INT_MEM_CMD`, `MFV_FREE_INT_MEM_CMD`, `VCODEC_PMEM_FLUSH`, `VCODEC_PMEM_CLEAN`,
`VCODEC_INC_SYSRAM_USER`, `VCODEC_DEC_SYSRAM_USER`, `VCODEC_SET_SYSRAM_INFO`,
`VCODEC_GET_SYSRAM_INFO` — a recursive `grep -rn <symbol> $M/src/downstream-grandpplte
--include=*.c --include=*.h` matches only their `#define`s in
`videocodec_kernel_driver.h:26-47`.

### 2.2 ROM dispatcher (`vcodec_unlocked_ioctl @ 0xc03974fc`)

Dispatch is a 42-entry jump table over the *ioctl number*, not a compare chain:

```
c039750c: movw r3, #0xb2f5
c0397510: movt r3, #0xbffb        ; r3 = -0x40044d0b
c0397514: add  r3, r1, r3         ; r3 = cmd - 0x40044d0b == _IOW('M',nr,u32) - 0x40044d00
c039751c: cmp  r3, #41
c0397520: ldrls pc, [pc, r3, lsl #2]
c0397524: b    0xc0397c3c         ; nr outside 0x0b..0x34 -> default
```

(table at `0xc0397528`, 42 words, indices `nr = 0x0b .. 0x34`.)

Parsing the table gives, for the commands in scope:

| cmd | table index | target | meaning |
|---|---|---|---|
| 0x06-0x0a | — (nr < 0x0b) | `b 0xc0397c3c` | default |
| 0x10 | 5 | `0xc0397c3c` | default |
| 0x11 | 6 | `0xc0397c3c` | default |
| 0x13 | 8 | `0xc0397c3c` | default |
| 0x14 | 9 | `0xc0397c3c` | default |
| 0x22 | 23 | `0xc039784c` | dedicated log-only case |
| 0x23 | 24 | `0xc0397874` | dedicated log-only case |
| 0x25 | 26 | `0xc0397c3c` | default |
| 0x26 | 27 | `0xc0397c3c` | default |

For comparison, the slots that *do* reach a real handler are 0x0b (WAITISR `0xc03975e0`),
0x0d (LOCKHW `0xc03975fc`), 0x15-0x18 (EMI counters `0xc0397624`/`0xc0397690`/`0xc03976fc`/
`0xc0397768`), 0x20/0x21 (`0xc03977d4`/`0xc0397810`), 0x24 (`0xc039789c`), 0x27/0x28
(`0xc03978d8`/`0xc0397924`), 0x29 (`0xc039797c`), 0x30 (`0xc03979b8`), 0x31 (`0xc0397a5c`),
0x32 (`0xc0397aa8`), 0x33 (`0xc0397b50`), 0x34 (`0xc03975d0`).

The default path and the two non-cache cases, disassembled:

```
c0397c3c: movw r0, #0x7650 ; movt r0, #0xc0c2   ; fmt = 0xc0c27650
c0397c44: bl   printk
c0397c48: b    0xc03975f0                        ; r0 = 255 -> return 255
...
c03975f0: mov  r0, #255
c03975f4: sub  sp, r11, #28
c03975f8: ldm  sp, {r4,r5,r6,r7,r11,sp,pc}

c039784c: ... movw r0, #0x6f70 ; movt r0, #0xc0c2 ; fmt = 0xc0c26f70
c0397868: bl   printk
c039786c: mov  r0, #255
c0397870: b    0xc03975d8                        ; return 255

c0397874: ... movw r0, #0x6fac ; movt r0, #0xc0c2 ; fmt = 0xc0c26fac
c0397890: bl   printk
c0397894: mov  r0, #255
c0397898: b    0xc03975d8                        ; return 255
```

Neither non-cache case calls `copy_from_user`/`copy_to_user`, `dma_alloc_coherent` or any
allocator: it is one `printk` plus `return 255`. So the ROM never allocates a non-cache
buffer and never writes anything into the caller's argument.

`vcodec_fops` (`@ 0xc0a73e74`, 3.18 layout with ten pointers before `unlocked_ioctl`):
`.owner = 0`, `.unlocked_ioctl = 0xc03974fc` (slot `+0x28`), `.compat_ioctl = 0`
(slot `+0x2c` is zero), `.mmap = 0xc03959e0`, `.open = 0xc0395980`,
`.flush = 0xc0395940`, `.release = 0xc0396394`. There is **no** ROM compat ioctl, so the
ROM's 32-bit userspace always reaches the native handler above.

### 2.3 ROM strings

Format strings resolved by virtual address (ELF `LOAD` vaddr `0xc0004000` = file offset
`0x124`):

| addr | string | referenced from |
|---|---|---|
| `0xc0c26f70` | `\0013Don't support VCODEC_ALLOC_NON_CACHE_BUFFER - tid = %d\n` | `0xc039784c` (cmd 0x22) |
| `0xc0c26fac` | `\0013Don't support VCODEC_FREE_NON_CACHE_BUFFER - tid = %d\n` | `0xc0397874` (cmd 0x23) |
| `0xc0c27650` | `\0013========[ERROR] vcodec_ioctl default case======== %u\n` | `0xc0397c3c` (default) |

(`\0013` = `KERN_ALERT`, i.e. `MODULE_MFV_LOGE`.) They are at `$M/zip/vmlinux.strings:27310`,
`:27311` and `:27343`.

These strings prove the ROM binary is built from D1, not D2/D3: D1's log-only cases and
default case (`videocodec_kernel_driver_D1.c:1267,1272,1563`) are byte-identical to the
strings above, whereas D2 (`:1376`, `:1442`) and D3 (`:1344`, `:1353`) contain no such
message — they really allocate with `dma_alloc_coherent()`.

### 2.4 SYSRAM: a camera driver, never reached from vcodec

ROM symbols (`llvm-nm -n $M/zip/vmlinux.elf`):
`SYSRAM_Init @ 0xc0db2368`, `SYSRAM_Probe @ 0xc04ea604`, `SYSRAM_Open @ 0xc04ea930`,
`SYSRAM_mmap @ 0xc04eaa6c`, `SYSRAM_Ioctl @ 0xc04eba64`, `SYSRAM_TryAllocUser @ 0xc04eb684`,
`SYSRAM_IOC_Free @ 0xc04eb00c`, `SYSRAM_Flush @ 0xc04eb4d4`.

`SYSRAM_Init @ 0xc0db2368` is the *camera* sysram driver:

```
c0db23ac: bl __platform_driver_register     ; arg 0xc0e857b0
c0db23e4: bl proc_create_data               ; name 0xc0c84c58 = "sysram"
c0db2400: bl proc_create_data               ; name 0xc0c84c60 = "sysram_flag"
```

and the failure string at `0xc0c84c2e` is `SYSRAMfailed to register sysram driver`.
Further ROM strings `[name:camera_sysram_D1&]SYSRAME` / `...X` at `0xc0c84140` /
`0xc0c84160`, and `camera-sysram` at `0xc0c84438`, match downstream
`drivers/misc/mediatek/cameraisp/src/mt6735/camera_sysram_D1.c` exactly:
`SYSRAM_Init()` at `:1259` (platform driver + `proc_create("sysram", ...)` at `:1274-1275`,
`module_init` at `:1305`, `MODULE_DESCRIPTION("Camera sysram driver")` at `:1307`),
char-device fops `SysramFileOper` at `:985` (`.unlocked_ioctl = SYSRAM_Ioctl`), and device
name `SYSRAM_DEV_NAME "camera-sysram"` in `inc/camera_sysram_D1.h:17` with its own ABI
`SYSRAM_ALLOC`/`SYSRAM_FREE` at `camera_sysram_D1.h:42-43`.

Cross-reference check on the ROM binary: there is **no** `bl` from any address in the whole
vcodec driver text range (`0xc039590c`-`0xc0397f80`, i.e. `vcodec_vma_open` ..
`vcodec_unlocked_ioctl`) into the SYSRAM driver (`0xc04ea2d8`-`0xc04ebfff`), and none from
`vcodec_driver_init @ 0xc0daf914` (scanned to 0xc0dafcac). Combined with the jump table (0x13/0x14/0x25/0x26 all map
to `default`), the vcodec ioctl path never calls SYSRAM — the two are independent drivers
(`/dev/Vcodec` vs the camera-sysram char device).

Nura: there is no vcodec-related SYSRAM service and no camera sysram driver. The
`drivers/misc/mediatek/` tree in this worktree exists only because the vcodec patch adds it
(`drivers/misc/mediatek/{Kconfig,Makefile}` + `.../vcodec/`), and
`grep -rn sysram $M/worktrees/vcodec-gap-commands/drivers/misc/mediatek/` returns nothing.
`include/linux/memblock.h`/`kernel/resource.c`-style hits in the base tree are unrelated
("system RAM"), not a driver.

### 2.5 Non-cache buffer on the other SoC variants (MT6735M / MT6753) — not a parity target

`videocodec_kernel_driver_D3.c:688-728` allocates with `dma_alloc_coherent(0, ...)` (`:704`),
storing the kernel VA in `u4ReservedSize` and the DMA address in `pvMemPa` (`:703-705`), then
copies the struct back; the ioctl case at `:1344-1346` calls it. `videocodec_kernel_driver_D2.c:1376-1434`
does the same with `vcodec_device` (`:1393`) plus per-thread `Add_NonCacheMemoryList()`
bookkeeping (`:1424`); its free case is `:1442-1462` (`dma_free_coherent` `:1455`). Neither
exists on MT6735 (D1) and neither exists in the ROM (2.3).

Why this cannot be adopted as-is:

* It is not ROM behaviour: the ROM returns 255 without allocating (2.2), and its userspace
  was shipped against exactly that.
* It hands userspace a *kernel* VA (in the `u4ReservedSize` slot of `VAL_MEMORY_T`, see
  `include/val_types_public.h:314-358`) and a physical address. Neither the ROM ABI nor
  D2/D3 defines how userspace is supposed to obtain a usable CPU VA (no mmap offset, no
  fd), and no allocation is tracked per file, so a second allocation/leak or a
  double-free is unobservable.
* D3's `dma_alloc_coherent(NULL, ...)` is not usable on this kernel:
  `kernel/dma/mapping.c:628` dereferences `dev->coherent_dma_mask` in
  `dma_alloc_attrs()`, so a NULL device is a NULL dereference, not a fallback.
* Nura has no buffer allocator that could back the ABI: `nura.config:3804` has
  `# CONFIG_DMABUF_HEAPS is not set`, there is no `CONFIG_ION`, and the only buffer-related
  config is `CONFIG_UDMABUF=y`/`CONFIG_DMA_CMA=y`. The ROM kernel, by contrast, has ION
  (`__ion_alloc` `@ symbols.txt:30309`, `ion_ioctl` `@ :30326`,
  `ion_device_create` `@ :30328`).

Also absent everywhere: implementations of the declared-but-unimplemented HAL entry points
that the dead commands would have used — `eVideoMemAlloc`/`eVideoMemFree`
(`include/val_api_public.h:194,209`), `eVideoIntMemAlloc`/`eVideoIntMemFree`
(`include/val_api_private.h:29-30`), `eVideoFlushCache`/`eVideoInvalidateCache`
(`include/val_api_public.h:160,177`) exist only as prototypes; `VAL_INTMEM_T`
(`include/val_types_private.h:89-97`) is only a struct declaration.

### 2.6 Nura current behaviour

`drivers/misc/mediatek/vcodec/vcodec-core.c`:
`vcodec_unsupported_cmds[]` (`:789-810`) contains all 13 commands; the native dispatcher's
`default:` (`:906-913`) returns `-EOPNOTSUPP` for them (and `-ENOTTY` for anything else).
The compat dispatcher (`:1016`) does not forward unknown commands. Its `default:` arm now
mirrors the native one: `vcodec_cmd_is_unsupported()` (`:1095`) -> `-EOPNOTSUPP` (`:1096`),
otherwise `-ENOTTY` (`:1097`). Before this change a 32-bit caller got `-ENOTTY` for the same
commands a 64-bit caller saw as `-EOPNOTSUPP`.

---

## 3. Parity matrix

Verdict vocabulary: **closed** = the ROM/downstream provides no service, so Nura's refusal is
already the complete port and no code can be written; **blocked** = a real implementation
exists somewhere but a required input (ABI/userspace contract) is missing from every source.

| Command | Downstream D1 behaviour | ROM behaviour (symbol @ addr) | Nura current | Verdict |
|---|---|---|---|---|
| `MFV_ALLOC_MEM_CMD` 0x06 | no `case`; `default:` `D1:1562-1565`, then `return 0xFF` `D1:1568` | `vcodec_unlocked_ioctl @ 0xc03974fc`: nr < 0x0b → `b 0xc0397c3c`; prints `0xc0c27650`, returns **255** | `-EOPNOTSUPP` on both paths (core:789-810, 907-909, 1095-1096) | closed — no semantics in any source |
| `MFV_FREE_MEM_CMD` 0x07 | same | `0xc0397c3c` (default), returns **255** | same | closed |
| `MFV_MAKE_PMEM_TO_NONCACHED` 0x08 | same | `0xc0397c3c` (default), returns **255** | same | closed |
| `MFV_ALLOC_INT_MEM_CMD` 0x09 | same | `0xc0397c3c` (default), returns **255** | same | closed |
| `MFV_FREE_INT_MEM_CMD` 0x0a | same | `0xc0397c3c` (default), returns **255** | same | closed |
| `VCODEC_PMEM_FLUSH` 0x10 | same | table slot 5 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_PMEM_CLEAN` 0x11 | same | table slot 6 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_INC_SYSRAM_USER` 0x13 | same | table slot 8 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_DEC_SYSRAM_USER` 0x14 | same | table slot 9 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_SET_SYSRAM_INFO` 0x25 | same | table slot 26 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_GET_SYSRAM_INFO` 0x26 | same | table slot 27 → `0xc0397c3c`, returns **255** | same | closed |
| `VCODEC_ALLOC_NON_CACHE_BUFFER` 0x22 | log-only `case` `D1:1266-1269`, no copy, no alloc; `return 0xFF` `D1:1568` | `0xc039784c`-`0xc0397870`: `printk(0xc0c26f70)`, `mov r0,#255`, returns **255**; touched no user memory | `-EOPNOTSUPP` on both paths (core:789-810, 1095-1096) | blocked (see missing input) |
| `VCODEC_FREE_NON_CACHE_BUFFER` 0x23 | log-only `case` `D1:1271-1274`; `return 0xFF` `D1:1568` | `0xc0397874`-`0xc0397898`: `printk(0xc0c26fac)`, returns **255** | `-EOPNOTSUPP` on both paths (core:789-810, 1095-1096) | blocked (see missing input) |

| Command dispatch consistency (all 13) | D1 compat `default:` (`:2020-2021`) forwards to the native ioctl, so both word sizes log and `return 0xFF` | no compat ioctl exists (`vcodec_fops.compat_ioctl` slot is 0), a 32-bit caller runs the native jump table -> **255** | before: native `-EOPNOTSUPP`, compat `-ENOTTY`; after the fix: `-EOPNOTSUPP` on both | closed (fixed by this patch) |

Missing input for the two blocked rows (nothing else is missing — these are the exact gaps):

1. **The userspace side of the buffer contract.** No source states how the caller is meant
   to obtain a CPU mapping of the returned buffer. D2/D3 return a kernel VA in
   `u4ReservedSize` and a PA in `pvMemPa` (`D3:703-705`, `D2:1392-1394`) with no mmap
   convention and no fd; the ROM ABI (`VAL_MEMORY_T`) has no field for a dmabuf/ion handle
   that this driver ever filled. Determining the real contract requires disassembling the
   vendor codec libraries, which this program forbids.
2. **A kernel allocation/ownership model to pair with it.** D1/ROM have no
   `NonCacheMemoryList` on MT6735 (D2's is MT6735M-only, `D2:1376-1434`, `:1424`), so lifetime,
   per-file ownership and double-free behaviour are undefined for this platform.
3. **ROM parity is explicitly negative**: implementing D2/D3 semantics would make Nura
   *diverge* from the shipped ROM, which returns 255 and allocates nothing.

Consequence: **no kernel patch: no implementable semantics.** Nothing is gained by adding an
allocator that the ROM does not have, whose userspace side cannot be established from any
available source, and that would leak a kernel virtual address to userspace.

### 3.1 Note on return values

The ROM returns 255 for *every* command, including the ones that succeed: `0xc03975f0`
(`mov r0,#255`) is reached both after a successful `vcodec_waitisr`/`vcodec_lockhw`
(`0xc03975e8` `bne` not taken / `0xc0397608` `beq 0xc03975f0`) and from the default path
(`b 0xc0397c48` -> `0xc03975f0`); D1 does the same with its unconditional `return 0xFF`
(`D1:1568`). So 255 is not a "not supported" code and carries no errno
meaning; it is the function's unconditional return. Nura's standard errno return is the
documented, deliberate deviation (coordinator report §6 and §9); this patch only makes the
two Nura dispatch paths agree with each other, it does not change the value itself.

Compat/native consistency (fixed by this patch): before the change the same command returned
`-EOPNOTSUPP` from a 64-bit caller (native `default:`) and `-ENOTTY` from a 32-bit caller
(compat `default:`), even though the vendor codec libraries on this device are 32-bit. The
compat `default:` now calls `vcodec_cmd_is_unsupported()` exactly like the native one. The
ROM has no compat handler at all (2.2), so this is a Nura-internal consistency fix, not a
ROM behaviour being mirrored; both errno values remain documented deviations from the ROM's
unconditional 255.

---

## 4. Compile

`vcodec-core.c` was modified, so it was rebuilt together with the two objects that share the
subsystem (unchanged, rebuilt fresh for completeness). Mandated command plus `W=1`, after
`touch`ing all three sources so every `CC` line below is a real compile:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/vcodec-gap-commands
cp $M/nura.config .config
make ARCH=arm64 LLVM=1 olddefconfig                      # exit 0, run once at
                                                        # task start; the final
                                                        # rebuild reused this .config
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
  W=1 -j2 drivers/misc/mediatek/vcodec/vcodec-core.o \
          drivers/misc/mediatek/vcodec/vcodec-vdec.o \
          drivers/misc/mediatek/vcodec/vcodec-venc.o
```

Result: exit 0, `CC drivers/misc/mediatek/vcodec/vcodec-core.o`,
`CC .../vcodec-vdec.o`, `CC .../vcodec-venc.o`, **0 warnings / 0 errors**
(`grep -c "warning:"` = 0). Log: `$M/logs/vcodec-gap-commands-build.log`.

Note: `$M/nura.config` does not set `CONFIG_MTK_VCODEC_MT6735`, so
`olddefconfig` leaves it unset (`# CONFIG_MTK_VCODEC_MT6735 is not set`); the three objects
still compile individually, which is what the leaf build exercises. Module linking and the
Kconfig enablement are the integration owner's step.

---

## 5. Inferred or hardware-unverified items

* `[INFERENCE]` Because the shipped ROM driver refuses 0x22/0x23 (2.3) and the ROM kernel is
  the only buffer broker available, the vendor codec libraries must obtain their buffers
  elsewhere — ION is present in the ROM (`__ion_alloc @ symbols.txt:30309`). Not verified
  against the userspace libraries: inspecting them is the HAL reverse-engineering this
  program excludes.
* `[INFERENCE]` No shipped userspace depends on these 13 commands doing work; otherwise the
  production ROM (whose driver only logs and returns 255) would be broken. This is why
  Nura's explicit refusal is not a functional regression.
* Not verified on hardware (none added): the entire report is static evidence; there is no
  new code to exercise.
* Not investigated by design: the MT6735 `pmem` helper used by `LOCKHW`
  (`pmem_user_v2p_video @ 0xc039582c`, `videocodec_kernel.h:17`) — that is the
  existing lock-ownership path, not one of the commands in scope.
* D2/D3 were read only to prove they belong to other SoCs (2.1) and that their non-cache
  semantics are not MT6735/ROM behaviour (2.5).

---

## 6. DTS-FRAGMENT

DTS-FRAGMENT: none.

---

## 7. Patch path

`$M/patches/vcodec-gap-commands.patch` (767 bytes, `git diff` from
`$M/worktrees/vcodec-gap-commands`), touching only
`drivers/misc/mediatek/vcodec/vcodec-core.c`:

```diff
@@ -1085,6 +1085,15 @@ static long vcodec_compat_ioctl(struct file *file, unsigned int cmd,
 		mb();
 		return 0;
 	default:
+		/*
+		 * Same distinction as the native dispatcher: a command that
+		 * exists in the ABI but has no implementation is refused with
+		 * -EOPNOTSUPP, anything else is not our ioctl at all.  Without
+		 * this a 32-bit caller would get -ENOTTY for the very commands
+		 * a 64-bit caller sees as -EOPNOTSUPP.
+		 */
+		if (vcodec_cmd_is_unsupported(cmd))
+			return -EOPNOTSUPP;
 		return -ENOTTY;
 	}
 }
```

No functional closure for the 13 commands: the allocation/SYSRAM/pmem services remain
absent, exactly as in the ROM.
