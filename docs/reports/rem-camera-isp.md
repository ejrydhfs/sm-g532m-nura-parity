# rem-camera-isp — ISP Pass1/Pass2/DSP/CQ/DMA capture pipeline and the "camera-isp" character ABI

Worktree: `$M/worktrees/rem-camera-isp` (detached at `src/nura-linux` HEAD `046883c0c`,
`parity-baseline-all.patch` applied on top).
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1 Status summary

| # | Item | Status |
|---|---|---|
| 1 | misc device `/dev/camera-isp` on its own platform node | **closed** |
| 2 | mmap restricted to the documented windows (+ the 64 KiB RT buffer table) | **closed** |
| 3 | `ISP_READ_REGISTER` / `ISP_WRITE_REGISTER` over the CAMINF address space | **closed** |
| 4 | hold-register buffers applied at VD / EXPDONE | **closed** |
| 5 | per-user IRQ status / wait / mark / clear / user-key API (v1 + v3) | **closed** |
| 6 | RT DMA rings IMGO / RRZO / IMGO_D / RRZO_D / CAMSV / CAMSV2 with SOF-time base-address and RCNT_INC programming | **closed** |
| 7 | CQ0C ring: instruction encodings + module/queue layout recovered | **closed** (kernel side: proven that the kernel must *not* own the ring; userspace side: encodings recovered and implemented) |
| 8 | P1 ED / P2 ED queue | **closed** |
| 9 | reset / scenario / burst-queue controls | **closed** |
| 10 | IRQ semantics proven against the ROM ISR | **closed** |
| 11 | userspace `libcamdrv` completed to the downstream `IspDrv` API | **closed** |
| 12 | usable capture flow in `camservice` | **partial**: allocate/map/queue/CQ0C/start/IRQ-wait/dequeue are real; the Pass-1/Pass-2/DSP *module* programming tables do not exist in any available source (see §6), so no frame can complete on a virgin pipeline |
| 13 | `ISP_SET_USER_PID`, `ISP_REGISTER_IRQ`, `ISP_UNREGISTER_IRQ` | **blocked by design**: the ROM build has no implementation; documented errno returned (§5) |
| 14 | `ISP_IRQ_TYPE_INT_P2_ST` event delivery | **blocked by ROM behaviour**: the ROM ISR never samples `ISP+0x6c` (§3.1); Pass-2 completion is delivered through `ISP_IRQ_TYPE_INT_STATUSX` |

No stubs, no fabricated register values, no timer-based completion. Every ioctl that touches
hardware writes real registers or returns a documented errno.

## 2 Files changed

| File | Lines | Change |
|---|---|---|
| `drivers/media/platform/mediatek/isp/mtk-isp.c` | 3682 | new: the character driver |
| `drivers/media/platform/mediatek/isp/mtk-isp-regs.h` | 426 | new: register map, IRQ masks, FBC bitfields, windows |
| `drivers/media/platform/mediatek/isp/Kconfig` | 29 | new: `VIDEO_MTK_ISP` |
| `drivers/media/platform/mediatek/isp/Makefile` | 2 | new |
| `drivers/media/platform/mediatek/Kconfig` | +1 | `source …/isp/Kconfig` |
| `drivers/media/platform/mediatek/Makefile` | +1 | `obj-y += isp/` |
| `tools/mediatek/camera/libcamdrv/camdrv_cq.c` | 251 | CQ0C ring builder switched to the recovered encodings |
| `tools/mediatek/camera/libcamdrv/camdrv_mem.c` | 175 | new: physically contiguous buffer helper |
| `tools/mediatek/camera/libcamdrv/camdrv.h` | 102 | ring/`camdrv_buffer` API |
| `tools/mediatek/camera/camdrv/camservice.c` | 305 | capture flow |
| `tools/mediatek/camera/Makefile` | 2 | build `camdrv_mem.c` |

Not touched: any DTS/DTSI, `imgsys/`, `seninf/`, `drivers/media/i2c/*`, `include/uapi/linux/mtk_isp.h`
(the uapi stayed byte-identical; `tests/abi32.c` still passes unmodified), other worktrees.

## 3 Evidence

### 3.1 ROM kernel (`zip/vmlinux.elf`, ELF32 ARM, local symbols)

`readelf -sW` gives `ISP_Irq_CAM` @ `0xc04ddce8`; the next symbol is `ISP_mmap` @ `0xc04e0180`,
so the function spans **0x2498 bytes**. Full disassembly was inspected
(`llvm-objdump -d --start-address=0xc04ddce8 --stop-address=0xc04e0180`).

**Status registers are read-to-clear.** The function contains exactly seven loads of the status
words and **zero stores** to any of them:

| load | address | register |
|---|---|---|
| `ldr r1,[r3,#0x4c]` | `0xc04ddda4` | `ISP+0x4c` INT_P1_ST |
| `ldr r2,[r3,#0x54]` | `0xc04dddd4` | `ISP+0x54` INT_P1_ST2 (read unmasked) |
| `ldr r1,[r3,#0x5c]` | `0xc04dddec` | `ISP+0x5c` INT_P1_ST_D |
| `ldr r1,[r3,#0x64]` | `0xc04dde1c` | `ISP+0x64` INT_P1_ST2_D |
| `ldr r1,[r3,#0x70]` | `0xc04dde4c` | `ISP+0x70` INT_STATUSX |
| `ldr r1,[r3,#0x74]` | `0xc04dde7c` | `ISP+0x74` INT_STATUS2X |
| `ldr r1,[r3,#0x78]` | `0xc04ddeac` | `ISP+0x78` INT_STATUS3X |

`grep -cE "ldr.*\[r[0-9]+, #0x(4c|54|5c|64|6c|70|74|78)\]"` = 7;
`grep -nE "str.*\[r[0-9]+, #0x(4c|54|5c|64|6c|70|74|78)\]"` = no matches.
There is **no `ISP+0x6c` (INT_P2_ST) load**, matching the downstream `#ifdef __debugused__`
(`camera_isp_D1.c:9140`). Consequently this driver reports Pass-2 completion only through
`ISP_IRQ_TYPE_INT_STATUSX` (`PASS2_DON` bit 14, `CQ_DON` bit 19, `TILE_DON` bit 15), exactly like
the ROM.

**FBC registers are read-modify-written with RCNT_INC, followed by `dsb sy`:**

| action | address | register |
|---|---|---|
| `ldr r2,[r3,#0xf0]` | `0xc04ddedc` | IMGO_FBC |
| `str r3,[r2,#0xf0]` (after `orr r3,r3,#2048`) | `0xc04de558` + `dsb sy` `0xc04de55c` | IMGO_FBC |
| `str r3,[r2,#0xf4]` | `0xc04de57c` + `dsb sy` `0xc04de580` | RRZO_FBC |
| `str r3,[r2,#0xf8]` | `0xc04deac8` | IMGO_D_FBC |
| `str r3,[r2,#0xfc]` | `0xc04deaec` | RRZO_D_FBC |

`orr r3,r3,#2048` = bit 11 = `RCNT_INC` in `union _CQ_RTBC_FBC_`.
`ldr r2,[r3,#0x44c]` @ `0xc04de38c` + `ubfx r2,r2,#0x10,#0x8` is the TG vertical counter;
the TG2 pair at `0x2414/0x244c` appears as `add r3,r3,#9280` @ `0xc04de3bc`.
DONE is serviced before SOF in the same ISR (ROM: `ISP_DONE_Buf_Time` call @ `0xc04de994`, SOF
bit-12 test @ `0xc04de9a0`), which is the ordering implemented here.

The ROM lines confirm RemImgsys' independent findings (183→`ISP_Irq_CAM`, 206→`ISP_Irq_CAMSV`,
207→`ISP_Irq_CAMSV2`, all `GIC_SPI` level-low).

### 3.2 Downstream GPL source (`src/downstream-grandpplte/.../camera_isp_D1.{c,h}`)

| Behaviour | Evidence |
|---|---|
| register offsets, status/FBC/base-address/CAMSV registers | `camera_isp_D1.c:237-300` |
| IRQ mask and error-mask tables | `camera_isp_D1.c:383-700` |
| reset sequences, no status poll | `camera_isp_D1.c:3263-3387` |
| CAMINF addressing (`ISP_ADDR_CAMINF + Addr`, valid `[0x4000,0x10000)`) | `camera_isp_D1.c:3387-3455` |
| hold buffers, 6 × 1024 B, flush at VD/EXPDONE from the tasklets | `camera_isp_D1.c:3462-3736, 3745-3800, 3897-3960` |
| ref-count control incl. reset-on-last | `camera_isp_D1.c:3961-4110` |
| RTBC enqueue | `camera_isp_D1.c:4115-4270` |
| deque / exchange / DMA_EN / clear / CUR_STATUS | `camera_isp_D1.c:4510-6158` |
| `ISP_SOF_Buf_Get` (metadata + timestamps + drop) | `camera_isp_D1.c:6177-6608` |
| `ISP_CAMSV_SOF_Buf_Get`, `ISP_CAMSV_DONE_Buf_Time` | `camera_isp_D1.c:6608-6830` |
| `ISP_DONE_Buf_Time` (shift handling) | `camera_isp_D1.c:6830-7084` |
| P2 early-done queue (ring 512, mgr 128, 5 s wait) | `camera_isp_D1.c:7084-7712` |
| user-key table (32 keys, names VSIrq/AFIrq/EIS/VHDR) | `camera_isp_D1.c:7712-7810` |
| mark / mark2query / flush v1+v3 / wait v1+v3 | `camera_isp_D1.c:7810-8640` |
| CAMSV ISR incl. the RCNT_INC double-write workaround | `camera_isp_D1.c:8680-9053` |
| `ISP_Irq_CAM` | `camera_isp_D1.c:9053-9718` |
| ioctl switch, `-EPERM` default, `SET_USER_PID` compiled out | `camera_isp_D1.c:9748-10370` |
| open / release (VF off, MCLK off, session reset) | `camera_isp_D1.c:10949-11231` |
| `ISP_mmap` windows and lengths | `camera_isp_D1.c:11263-11335` |
| probe: 5 windows, 5 IRQs, only CAM/CAMSV/CAMSV2 requested | `camera_isp_D1.c:11432-11700` |
| RT buffer table: 16 pages, page-aligned, `SetPageReserved` | `camera_isp_D1.c:13042-13065` |

### 3.3 ROM userspace `libcamdrv.so` (stripped, Thumb-2; `nm -D` + `llvm-objdump -d --triple=thumbv7`)

**CQ module table.** `mIspCQModuleInfo` @ `.data` vaddr `0x2d514` (file offset `0x2c514`) is an
array of 134 twelve-byte records `{module, reg_addr, reg_count}`:

```
0x2d514: (0,0x4004,2) (1,0x4010,2) (2,0x4018,2) (3,0x4024,1) (4,0x4028,1) …
0x2db14: (0x80,0x75e4,1) (0x81,0x75e8,1) (0x82,0x4094,1) (0x83,0x409c,1) (0x84,0x4184,1) (0x85,0x4184,1)
```
`IspDrvImp::getCqModuleInf(CAM_MODULE_ENUM, u32&, u32&)` @ `0x6b0c` computes
`entry = table + 12*module` (`mla r1, r0, r1, r4` @ `0x6b16`) and returns `[entry+4]`, `[entry+8]`.
`IspDrvVirImp::getIspCQModuleInfo(...)` @ `0x6b54` copies all three words.

**CQ descriptor layout** (`IspDrv::cqAddModule` @ `0xa414`):
```
0xa43e  r2 = [entry+4]                     ; reg addr
0xa440  r1 = [entry+8]                     ; count
0xa444  r3 = uxth(r2)                      ; addr & 0xffff
0xa44c  r12 = (count-1) & 0x3ff
0xa45c  r3 |= r12 << 16                    ; word0 = addr | ((count-1) << 16)
0xa46a  r2 += [mpIspCQDescBufferPhy][cq]   ; word1 = phy_base + reg addr
0xa46e  str r2, [desc+module*8+4]
0xa476  str r3, [desc+module*8+0]
```
`IspDrv::cqDelModule` @ `0xa490` writes `0x4184` into `word0` of the module slot.
`IspDrv::setCQTriggerMode` @ `0x6dd4` writes register `0x4020` and sets bit 23 (`orr #0x800000`
@ `0x6e42`/`0x6e74`); `0x4020` is `ISP+0x20`, the CQ0 control register.

**CQ0C ring instruction words** (builder inlined in the module at `0xcbf0`; `llvm-objdump`
attributes it to `writeReg` because the previous symbol is `0xcadc`):

| word | meaning | evidence |
|---|---|---|
| `0x7300` | write `ISP+0x3300` IMGO_BASE_ADDR from memory | `movw r12,#0x7300` @ `0xcd26`, `str [ring+0]` @ `0xcd40` |
| `0x7320` | write `ISP+0x3320` RRZO_BASE_ADDR | `movw r0,#0x7320` @ `0xcd86`, `str [ring+8]` @ `0xcd98` |
| `0x74d4` | write `ISP+0x34d4` IMGO_D_BASE_ADDR | `movw r12,#0x74d4` @ `0xcd38` |
| `0x74f4` | write `ISP+0x34f4` RRZO_D_BASE_ADDR | `movw r0,#0x74f4` @ `0xcd94` |
| `0x40b4` | load next descriptor into `ISP+0x00b4` CQ0C base | `movw r0,#0x40b4` @ `0xcdc8`, `str [ring+16]` @ `0xcdcc` |
| `0x40cc` | same for `ISP+0x00cc` CQ0C2 base (P1_D) | `movw r0,#0x40cc` @ `0xcdda` |
| `0xfc000000` | ring end word | `movw r10,#0xfc000000` @ `0xcde6`, `str [ring+24]` @ `0xcdf6` |
| `0x4184` | CQ table end / unused slot | `movw r10,#0x4184` @ `0xcd10`, `movw r0,#0x4184` @ `0xcd6e` |

Operand addresses are physical addresses of record fields:
`(slot_off + 0x20) + ring_pa` for `imgo_base_pAddr` (`0xcd5c-0xcd60`), `+0x24` for
`rrzo_base_pAddr` (`0xcdbc-0xcdc4`), `(slot_off + 0x30) + ring_pa` for the *next* record's
`next_pa` (`0xcdee-0xcdf2`), which is how `next_cq0ci` makes the ring self-chaining.

**Ring counters.** `CQ_RTBC_RING_ST.imgo_ring_size` / `rrzo_ring_size` (offsets `0x380`/`0x384`
on 32-bit, i.e. right after the 16 × 56-byte records) are incremented once per generated record
(`0xce32-0xce34`, `0xd040-0xd044`) and their low nibble is written into FBC bits **16..19**
(`FB_NUM`) with `bfi r2,r8,#16,#4` (`0xce86`, `0xcf7a`, `0xd09a`, …). They are *not* byte sizes.

**Ring start.** After building the records the code writes the CQ0C base register:
`writeReg(0x40b4, ring_pa)` or `ring_pa + 0x38` (`0xd4aa-0xd4e6`, `0xd4f2-0xd520`);
streaming is then enabled with `writeReg(0x4414, 1)` (`0xcc32-0xcc42`), i.e. `TG_VF_CON` bit 0.

**Ring reset.** `IspDrvImp::cqRingBuf(void *)` @ `0xd78c` command 7 `memset`s `0x388` bytes —
`sizeof(CQ_RTBC_RING_ST)` on 32-bit — confirming the structure identity used by the uapi.

## 4 Implementation

### 4.1 Kernel driver `mtk-isp.c`

* **Node.** Own platform driver, compatible `mediatek,mt6735-camera-isp`, five memory resources in
  the ROM order (`0x15004000+0x9000`, `0x1500d000+0x1000`, `0x15000000+0x10000`,
  `0x10215000+0x3000`, `0x10211000+0x1000`) and the ROM IRQ order. Windows are mapped with
  `devm_ioremap()` (not `devm_ioremap_resource()`) so the reservations taken by `mtk-imgsys`
  do not collide, as RemImgsys requested.
* **Power.** `devm_pm_runtime_enable()` + the six IMGSYS gate clocks (`CG_IMAGE_*`) are held for
  the whole time a file is open; the ISP genpd comes from the node's `power-domains`. The IRQs
  are `disable_irq()`d at probe and around every session, so the handler can never touch a
  powered-down block; on the last close the status registers are drained once (they are
  read-to-clear) before the clocks are gated.
* **mmap.** The eight uapi windows are accepted with the documented base and up to the window
  size, mapped non-cached via `remap_pfn_range(vma, …, vma->vm_pgoff, …)`; page offset
  `RT_BUF_TBL_NPAGES` (16) maps the 64 KiB ring table (`alloc_pages(order 4)`), which is where
  the kernel keeps `ISP_RT_BUF_STRUCT` exactly as downstream does.
* **Register access.** `ISP_REG_STRUCT.Addr` is a CAMINF offset; valid range `[0x4000,0x10000)`
  (`camera_isp_D1.c:3399-3405`); reads use `get_user`/`put_user` per entry, writes are either
  direct (`spin_lock` around the sequence) or appended to a hold buffer.
* **Hold registers.** 6 × 1024 B buffers; `ISP_HOLD_REG` disabled converts HOLD→READY; the VD and
  EXPDONE tasklets flush READY buffers to hardware when `hold_time` matches.
* **Interrupts.** `mtk_isp_irq_cam()` samples the seven CAM status words exactly once per
  interrupt, the four FBC values and the base addresses; `mtk_isp_irq_camsv_common()` does the
  same for `+0x500c` / `+0x580c`. Nothing else reads a status register (the dump uses the cached
  software status). Both handlers publish into the 32 × 17 per-user status/mark/time/passed-by
  arrays and wake the wait queue.
* **RT rings.** Enqueue while a port is stopped arms the DMA base address at `empty_count == 1`;
  while streaming it re-arms the matching slot; the exchange path swaps by `bufIdx` (≠ `0xffff`)
  or by physical address. At SOF the driver reloads a deferred base address, pulls `RCNT_INC`
  for every queued round (with the `mb()` that mirrors the ROM `dsb sy`), then stamps the frame
  and fills the per-frame geometry from the inner (TPIPE) registers. DONE marks
  `start` as FILLED and advances; DEQUE hands out only a FILLED slot (`-EAGAIN` otherwise).
  CAMSV additionally reprograms its destination from the software index at DONE and pulses
  `RCNT_INC` with the documented double write.
* **ED queue.** Ring 512 / manager 128 / 5 s deque wait, with the downstream first/current/last
  pointer bookkeeping.
* **No hardware interrupt-enable register exists** (verified by RemImgsys and by the absence of
  any store to the status addresses): masking is purely the software `Mask|ErrMask` AND.

### 4.2 Userspace

* `camdrv_cq.c` now builds the ring with the recovered words
  (`camdrv_cq_ring_init_ex()`), computes every operand address from the real struct offsets,
  counts descriptors per ring, kicks the CQ0C base register (`camdrv_cq_ring_kick()`), and
  programs `FB_NUM`/`LOCK_EN` (`camdrv_cq_ring_fbc_program()`). The explicit
  `camdrv_cq_ring_init()` remains for callers with another source and still rejects zero words.
* `camdrv_mem.c` allocates from the upstream DMA-BUF heap and *verifies* physical contiguity
  through `/proc/self/pagemap`; it fails with `-EPERM`/`-EOPNOTSUPP` instead of guessing a PA.
* `camservice --capture` allocates one contiguous region split into N frame buffers, programs the
  DMA destination geometry, enqueues, builds and starts the CQ0C ring, enables the DMA port and
  VF, waits for the real SOF and PASS1_DON interrupts, dequeues with timestamps and re-arms the
  buffer.

## 5 Parity matrix

| ABI item | Implementation | Status |
|---|---|---|
| `ISP_RESET_CAM_P1/P2/CAMSV/CAMSV2` | `mtk_isp_reset()` — ROM RMW sequences, single read-back, no status poll | done |
| `ISP_RESET_BUF` | resets the hold buffers and both enable flags | done |
| `ISP_READ_REGISTER` | CAMINF range check, per-entry `get_user`/`put_user`, out-of-range logs + returns 0 like downstream | done |
| `ISP_WRITE_REGISTER` | direct or hold-buffered, range checked | done |
| `ISP_HOLD_REG_TIME` | `VD`/`EXPDONE`, invalid values `-EINVAL` | done |
| `ISP_HOLD_REG` | 1-byte ABI value, HOLD→READY on disable | done |
| `ISP_WAIT_IRQ` | v1 (`UserNumber > 0`) and v3 (user key), `CLEAR_WAIT/STATUS/ALL`, EIS meta, timeout `-EFAULT`, signal `-ERESTARTSYS` | done |
| `ISP_READ_IRQ` / `ISP_CLEAR_IRQ` | per user/type | done |
| `ISP_REGISTER_IRQ_USER_KEY` | 32 slots, duplicate names reuse the key, VSIrq/AFIrq/EIS/VHDR flush sets | done |
| `ISP_MARK_IRQ_REQUEST` / `ISP_GET_MARK2QUERY_TIME` / `ISP_FLUSH_IRQ_REQUEST` | mark, timing, flush v1+v3 | done |
| `ISP_REGISTER_IRQ` / `ISP_UNREGISTER_IRQ` | no case in the ROM switch → `-EPERM` (as the ROM default) | documented errno |
| `ISP_BUFFER_CTRL` ENQUE / ENQUE_IMD / exchange / DEQUE / IS_RDY / DMA_EN / GET_SIZE / CLEAR / CUR_STATUS | full RT ring + RCNT machinery; EXCHANGE_ENQUE is a no-op like downstream | done |
| `ISP_REF_CNT_CTRL` | per-id atomic counters, GET/INC/DEC + reset-on-last P1/P2 | done |
| `ISP_ED_QUEBUF_CTRL` | all 7 controls | done |
| `ISP_UPDATE_REGSCEN` / `ISP_QUERY_REGSCEN` | default `0xa5a5a5a5` | done |
| `ISP_UPDATE_BURSTQNUM` / `ISP_QUERY_BURSTQNUM` | 1..4 (`.h:_MAX_SUPPORT_P2_BURSTQ_NUM_`) | done |
| `ISP_DUMP_REG` | curated dump that never samples read-to-clear registers | done |
| `ISP_DUMP_ISR_LOG` | TG index validated, state dumped (the ROM's `IRQ_LOG_KEEPER` is `pr_warn`) | done |
| `ISP_GET_CUR_SOF` / `ISP_GET_DROP_FRAME` | per TG/channel counters | done |
| `ISP_GET_DMA_ERR` | returns the array; the per-port counters are never filled (see §6.4) | honest zeros |
| `ISP_GET_INT_ERR` | sticky per-domain error words | done |
| `ISP_SET_FPS` | one-shot latch, T_STAMP_2_0 interpolation above 100 fps | done |
| `ISP_WAKELOCK_CTRL` | `wakeup_source` stay/relax, released on close | done |
| `ISP_DEBUG_FLAG` | debug mask stored | done |
| `ISP_SET_USER_PID` | compiled out downstream → `-EOPNOTSUPP` | documented errno |
| `ISP_IRQ_TYPE_INT_P2_ST` | not sampled by the ROM ISR; P2 done arrives via STATUSX | ROM-faithful |
| CQ0C ring | encodings recovered (§3.3); kernel never writes `ISP+0xb4` (Rom-faithful), userspace owns the ring | done |

### 5.1 Deliberate deviation (the only one)

The ROM ISR schedules the VD/EXPDONE tasklets under
`(st[P1] & VS1) & (st[P1_D] & VS1)` — a bitwise AND of two words, i.e. true only when **both**
TGs signal. For a single-sensor session the hold-register flush would therefore never run. The
driver uses `||`, which is the only reading under which the documented purpose (apply held
registers at VD/EXPDONE) works. Cited at the call site in `mtk_isp_irq_cam()`.

## 6 Irrecoverable items

### 6.1 ISP module programming tables (blocker for a complete pass-1/pass-2/DSP pipeline)
The `CAM_*` module registers, the TPIPE (inner) block, the AE/AWB/AA statistics configuration and
the CQ0 table the ISP firmware executes are generated by the closed MTK camera HAL
(`libcam.iopipe.so`, `lib3a.so`, `libcam.hal3a.v3.so`) from sensor-specific tuning data. They are
not present in the downstream GPL drop (which only ships the driver), not in the ROM kernel, and
the vendor HALs are stripped binaries whose tuning blobs are loaded from `/system/etc/*.bin`
tuning files that are not in the extracted set. Nothing in this slice invents those values; the
capture flow therefore programs only what is proven (DMA destination geometry, FBC, CQ0C, VF) and
reports an interrupt timeout on an unprogrammed pipeline instead of a fake frame.

### 6.2 CQ0C ring start ownership
The D1 kernel never writes `ISP+0x00b4`; the only references in `camera_isp_D1.c` are read-only
logging (`:6458`, `:6571`), and the ROM `ISP_Buf_CTRL_FUNC` has no store to that offset. The ring
is started by userspace (`writeReg(0x40b4, ring_pa)`, libcamdrv.so `0xd4aa-0xd520`). The driver
therefore exposes the ISP window and does not duplicate that write.

### 6.3 Per-port DMA error counters
`DMAErrHandler()` in the downstream source is inside `#if defined(_debug_dma_err_)` and is not
compiled into the ROM build, so `g_DmaErr_p1[]` is only ever zeroed. `ISP_GET_DMA_ERR` returns
that (empty) array; the aggregate DMA error is reported through `ISP_GET_INT_ERR` (STATUSX bit 30)
and dumps the state.

### 6.4 `ISP_SET_USER_PID`
Compiled out behind `ISP_KERNEL_MOTIFY_SINGAL_TEST` (`camera_isp_D1.c:10300-10310`); the ioctl
returns `-EOPNOTSUPP` rather than pretending to signal a process.

### 6.5 Pass-2 status word
`ISP+0x6c` is not sampled by the ROM ISR, so `ISP_IRQ_TYPE_INT_P2_ST` waits cannot complete and
the driver reports nothing for that type (rather than the uninitialised stack contents the ROM
would OR into the per-user status). Pass-2 completion is available through
`ISP_IRQ_TYPE_INT_STATUSX`.

## 7 Build evidence

Log: `$M/logs/rem-camera-isp-build.log` (`warning|error` matches = 0; kernel passes 1 and 2 plus a
final `W=1` rebuild after the last edit, and the userspace build/`check`/`abi32`).

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export TMPDIR=$M/tmp                     # /tmp is a full 12G tmpfs on this host
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/rem-camera-isp
cp $M/nura.config .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CONTROLLER \
  -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC \
  -e VIDEO_CAMERA_SENSOR -e MEDIA_CAMERA_SUPPORT -e I2C -e REGULATOR -e COMMON_CLK \
  -e NVMEM -e DEBUG_FS -e MEDIA_PLATFORM_SUPPORT -e VIDEO_MTK_IMGSYS \
  -e VIDEO_MTK_SENINF -e VIDEO_MTK_ISP -e VIDEO_IMX219 -e VIDEO_S5K5E3YX \
  -e VIDEO_DW9807_VCM -e VIDEO_IMX219_EEPROM
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/media/platform/mediatek/isp/ \
     drivers/media/platform/mediatek/imgsys/ drivers/media/platform/mediatek/seninf/
make ARCH=arm64 LLVM=1 W=1 -j1 drivers/media/platform/mediatek/isp/mtk-isp.o \
     drivers/media/platform/mediatek/imgsys/mtk-imgsys.o \
     drivers/media/platform/mediatek/seninf/mtk-seninf.o
```

Result:

```
CC drivers/media/platform/mediatek/imgsys/mtk-imgsys.o
CC drivers/media/platform/mediatek/isp/mtk-isp.o
CC drivers/media/platform/mediatek/seninf/mtk-seninf.o
pass1 exit=0
=== kernel build: W=1
CC drivers/media/platform/mediatek/isp/mtk-isp.o
CC drivers/media/platform/mediatek/imgsys/mtk-imgsys.o
CC drivers/media/platform/mediatek/seninf/mtk-seninf.o
pass2 exit=0
```

`CONFIG_VIDEO_MTK_ISP=y` (plus `VIDEO_MTK_IMGSYS`/`VIDEO_MTK_SENINF`) after `olddefconfig`.
Kernel: 0 errors, 0 warnings in both passes. Only the three media objects and their directories
were built; the full kernel was never built (disk space).

Userspace (host gcc, log appended to the same file):

```
make -C tools/mediatek/camera all     → make all exit=0    (0 warnings, -Wall -Wextra)
make -C tools/mediatek/camera check   → make check exit=0
  nodev-check: camdrv reports the missing device as ENODEV
  nodev-check: camservice reports the missing device as ENODEV
  compat32: built build/bin/abi32 (32-bit ABI layout checked)
./build/bin/abi32                     → abi32 exit=0
  ISP_CQ_RING_CMD_ST 64 / ISP_CQ_RTBC_RING_ST 1032
  ISP_WAIT_IRQ 0x40606b09 / ISP_BUFFER_CTRL 0xc0206b0e
```

The uapi header and `tests/abi32.c` are unchanged, so every ioctl number and structure size is
byte-identical to the baseline.

## 8 DTS fragment (text only; no DTS/DTSI file was modified)

```dts
/*
 * Camera ISP character device.  Separate node from "mediatek,mt6735-ispsys"
 * (which mtk-imgsys binds); the register windows overlap on purpose and both
 * drivers map them with non-reserving ioremaps.
 */
camera_isp: camera-isp@15004000 {
	compatible = "mediatek,mt6735-camera-isp";
	/* ROM DTB ispsys reg order: ISP core + CAMSV banks, inner/TPIPE,
	 * CAMINF, MIPI RX analog, GPIO. */
	reg = <0 0x15004000 0 0x9000>,
	      <0 0x1500d000 0 0x1000>,
	      <0 0x15000000 0 0x10000>,
	      <0 0x10215000 0 0x3000>,
	      <0 0x10211000 0 0x1000>;
	/* 0: CAM0 (SPI 183), 1/2 unused CAM1/CAM2, 3: CAMSV0 (206),
	 * 4: CAMSV1 (207).  All GIC_SPI level-low. */
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MTK6735_POWER_DOMAIN_ISP>;
	/* no iommus: the ABI carries physical buffer addresses (IOMMU bypass) */
	status = "disabled";	/* board DTS enables it */
};
```

Notes for the integrator:

* `reg` entry 0 must be at least `0x9000` long: the CAMSV/CAMSV2 banks (`ISP+0x5000`) live above
  the 4 KiB ISP core window; probe rejects a shorter window with a message.
* The six clocks are the same gates `mtk-imgsys` uses; CCF reference counting makes the overlap
  safe and keeps register access valid while any file is open.
* If `mtk-imgsys` later exports a runtime-PM/clock helper, the clock list here can be dropped
  without touching the register logic (all of it is behind `mtk_isp_power_on/off`).
* The `power-domains` phandle only needs to exist; the ISP domain is shared with the ispsys node.

## 9 Hardware-unverified (no board access)

* The exact FBC semantics used for the drop decision (`FB_NUM == FBC_CNT` at SOF) are taken from
  the downstream source and the ROM bit tests; no silicon run was possible.
* `ISP+0x4` is written by the ROM `/dev/camera-isp` release path only as the MCLK disable
  (`ISP+0x4200/0x4600/0x4a00`); this driver performs the same writes but their effect on the
  sensor clock tree is unverified.
* `camdrv_mem.c` relies on `CAP_SYS_ADMIN` for pagemap PFNs; on a stock Android-style policy the
  capture tool will stop with `-EPERM` rather than fabricate addresses.
* The CQ0C instruction words are proven only as far as libcamdrv.so shows them; the ISP's actual
  interpretation of `0xfc000000` (end-of-ring) is inferred from its position and is not
  documented anywhere.

## 10 Patch

`$M/patches/rem-camera-isp.patch` (174 KB, 11 files, +4900/−76). Verified to apply on top of the
baseline: `git apply --cached --check` against a tree built as
`HEAD + parity-baseline-all.patch` succeeds. Contains no DTS/DTSI and no build artefacts.
