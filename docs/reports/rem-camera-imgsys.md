# rem-camera-imgsys — IMGSYS/SMI/clock/interrupt platform slice

Worktree: `$M/worktrees/rem-camera-imgsys` (detached `046883c0c` + `patches/parity-baseline-all.patch`,
regenerated 22:00 copy including the two `drivers/misc` fixups)
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Date: 2026-10-24

## 1. Status summary

| Item | Status |
|---|---|
| Pass-1 soft reset fidelity (baseline used a status poll that the hardware/ROM does not implement) | **closed** — poll removed, sequence now byte-for-byte the ROM sequence |
| Pass-2 soft reset (`CAM_SW_CTL` bits 6:4) | **closed** |
| CAMSV soft reset (`ISP+0x5010`) | **closed** — new optional CAMSV window mapping |
| CAMSV2 soft reset (`ISP+0x5810`) | **closed** |
| DSP reset | **N/A** — no such path exists in the D1 downstream source or the ROM (see §6) |
| Reset at power-on vs. on demand | **closed** — resets moved out of `runtime_resume` (would corrupt an active char-driver stream) to probe + exported on-demand helper |
| Six IMGSYS gate set / parents / enable order / set-clr-sta offsets | **verified, no change needed** |
| LARB2 `CMD_THRT_CON` (0x24) bit 27 clear | **closed** — ported |
| LARB2 OSTDL (`0x200 + 4*port`) | **closed** — vendor table ported for LARB2 |
| LARB2 `OSTDL_SOFT_EN` (0x64) | **reported, not implemented** (unproven semantics, §6) |
| SMI common init table / 0xfc0 MMU bitmap | **verified, unchanged** |
| Definitive ISP interrupt map (5 lines, 9 status registers, bits, ordering, ack model) | **closed** — table in §3.3 |
| Interface for the char driver | **closed (revised at integration)** — the char driver implements the same vendor reset sequences atomically under its own register lock, so no cross-driver call is needed; the coordinator removed the initially exported `mtk_imgsys_reset()` helper as dead code. The reset enum is private to `mtk-imgsys.c`. |
| Build: imgsys/, `clk-mt6735-imgsys.o`, `mtk-smi.o`, plain + `W=1` | **0 errors, 0 warnings** |

No `request_irq`/`devm_request_irq` anywhere in this slice. No DTS/DTSI file edited.

## 2. Files changed

| File | Change |
|---|---|
| `drivers/media/platform/mediatek/imgsys/mtk-imgsys.c` | 4 reset paths (private enum, probe-time), CAMSV window mapping, reset-at-probe policy, `runtime_resume` clocks-only, debugfs hazard note |
| `drivers/media/platform/mediatek/imgsys/mtk-imgsys-regs.h` | P2 fields, CAMSV/CAMSV2 window + `SW_CTL` offsets |
| `drivers/media/platform/mediatek/imgsys/Kconfig` | help text updated (reset paths, probe-time resets, char-driver ownership) |
| `drivers/memory/mtk-smi.c` | `mtk_smi_larb_mt6735` gen + `mtk_smi_larb_config_port_mt6735()` (0xfc0 bitmap unchanged, +0x24 bit 27, +LARB2 OSTDL) |
| `drivers/misc/Kconfig`, `drivers/misc/Makefile` | baseline fixups from the coordinator (present in the regenerated baseline; added here only so BT/LT match — **not** part of the leaf patch) |

`drivers/clk/mediatek/clk-mt6735-imgsys.c` is **not** changed: the audit found nothing to fix (see §3.2).

Patch: `$M/patches/rem-camera-imgsys.patch` (20 550 B, 4 files, +280/−65; no DTS, no
`drivers/clk`, no `drivers/media/platform/mediatek/{Kconfig,Makefile}`, no char driver).

## 3. Evidence

`$M/zip/vmlinux.elf`: ELF32 ARM, `.kernel` LOAD vaddr `0xc0004000` at file offset `0x124`, so
`vaddr = file_offset + 0xc0003edc`. All ROM addresses below are disassembly addresses
(`llvm-objdump --triple=armv7-linux-gnueabihf`). `$M/zip/symbols.txt` names, sizes from
`readelf -sW`.

### 3.1 Soft resets

| Path | Register | Off. | Mask | Trig | Status | HW rst | Downstream | ROM (exact instructions) |
|---|---|---|---|---|---|---|---|---|
| CAM P1 | `CAM_SW_CTL` | ISP+0x8C | 0x7 | b0 | b1 | b2 | `camera_isp_D1.c:370-373`, `:3276-3302` | `ISP_ioctl` `0xc04e75e4` read, `:7600` `bic #7`/`orr #1`, `:760c` write, `:7620` re-read, `:7638` `str #4`, `:7650` write `val&~7` |
| CAM P2 | `CAM_SW_CTL` | ISP+0x8C | 0x70 | b4 | b5 | b6 | `camera_isp_D1.c:374-377`, `:3304-3330` | `ISP_MDPResetCallback` `0xc04d3100`, `:3110` `bic #0x70`/`orr #0x10`, `:311c` write, `:3130` re-read, `:3148` `str #0x40`, `:3160` write |
| CAMSV | `CAMSV_SW_CTL` | ISP+0x5010 | — | b0 | b1 | b2 | `camera_isp_D1.c:263`, `:3332-3340` | `ISP_ioctl` `0xc04e7a64` write 1 (`add r3,r3,#20480` + `str [r3,#0x10]`), `:7a94` write 0, `:7aac` read, `:7ac8` write 4, `:7ae4` write 0 |
| CAMSV2 | `CAMSV2_SW_CTL` | ISP+0x5810 | — | b0 | b1 | b2 | `camera_isp_D1.c:267`, `:3341-3349` | `ISP_ioctl` `0xc04e8414` write 1 (`add r3,r3,#22528` + `str [r3,#0x10]`), `:843c` 0, `:8454` read, `:8470` 4, `:848c` 0 |

Key finding that changes the baseline behaviour: the downstream completion loop

```c
do { Reg = ISP_RD32(ISP_REG_ADDR_CAM_SW_CTL); }
while ((!Reg) & ISP_REG_SW_CTL_SW_RST_STATUS);      /* camera_isp_D1.c:3285-3288, :3311-3314 */
```

can never iterate — `(!Reg)` is 0/1 and bitwise-AND with `BIT(1)` is 0 either way — and the ROM
shows the compiler dropped it: **one** read between trigger and `HW_RST`
(P1 `0xc04e7620`, P2 `0xc04d3130`; the CAMSV read at `0xc04e7aac` is dead, overwritten by the
`mov r2,#4` at `0xc04e7ab8`). The baseline polled `SW_RST_STATUS` with a 10 ms/10 s timeout and
failed probe on timeout, i.e. it could fail on silicon where the bit never asserts. Replaced with
the ROM sequence; the read is kept because it also orders the two posted writes on the
Device mapping.

Reset call sites in the vendor kernel (proof that resets are on *demand*, not at power-on):
`camera_isp_D1.c:9891-9911` (`ISP_RESET_*` ioctls), `:12161` (`ISP_MDPResetCallback`), `:4026/:4033`
(refcount release); `ISP_EnableClock()` (`:3190-3256`) enables clocks only.

### 3.2 IMGSYS clocks — audit result: baseline correct, nothing added

- Gate table: `clk-mt6735.c:877-881` regs (`set 0x4`, `clr 0x8`, `sta 0x0`) and
  `:885-900` eight gates. Driver matches offsets and bits exactly:
  `smi_larb2` b0, `cam_smi` b5, `cam_cam` b6, `sen_tg` b7, `sen_cam` b8 (parent `camtg_sel`),
  `cam_sv` b9, `sufod` b10, `fd` b11.
- Enable order `camera_isp_D1.c:3062-3100`: CAM_SMI (`:3078`), CAM_CAM (`:3082`), SEN_TG
  (`:3086`), SEN_CAM (`:3090`), CAM_SV (`:3094`), LARB2_SMI (`:3098`) — identical to
  `mtk_imgsys_clk_names[]`.
- The three clocks downstream enables *before* the IMGSYS gates are not IMGSYS gates:
  `CG_SCP_SYS_DIS`/`CG_SCP_SYS_ISP` are the SCP MTCMOS domains (here: `power-domains`) and
  `CG_DISP0_SMI_COMMON` is the DIS SMI common gate, held by the `mtk-smi` common device
  (`mtk6735.dtsi:846-862` `smi_common` `clocks = <&clk26m>, <&mmsys CLK_MM_SMI_COMMON>`).
- No gate is missing for a camera consumer. `IMG_IMAGE_SUFOD`/`IMG_IMAGE_FD` are used by other
  blocks (`fdvt@1500b000`, `rom_dtb_0.dts:3756-3761`, whose `FD-IMG_IMAGE_FD` clock is index 8 in
  the vendor 1-based enum `include/dt-bindings/clock/mt6735-clk.h:175`, i.e. `CLK_IMG_FD` 7 in the
  0-based mainline binding); adding them to the camera driver would raise unrelated domains. `SEN_CAM -> camtg_sel` was already fixed by the previous leaf and re-verified
  against `clk-mt6735.c:896`.

### 3.3 Interrupt map (definitive)

Lines — `rom_dtb_0.dts:3708`, `mt6735.dtsi:1272-1276` (downstream), all `GIC_SPI`,
`IRQ_TYPE_LEVEL_LOW`:

| SPI | Vendor label | Handler | ROM handler | ROM registration |
|---|---|---|---|---|
| 183 (0xb7) | CAM0 | `ISP_Irq_CAM` | `0xc04ddce8` | `request_threaded_irq` @`0xc04db0d0`, handler `0xc04ddce8`, flags 0, dev_id NULL |
| 184 (0xb8) | CAM1 | **none** | — | `irq_of_parse_and_map` only; the ROM probe loop branches for index 0/3/4 only (`0xc04dabd0-0xc04dabe4`) |
| 185 (0xb9) | CAM2 | **none** | — | same |
| 206 (0xce) | CAMSV0 | `ISP_Irq_CAMSV` | `0xc04dc71c` | `request_threaded_irq` @`0xc04db19c` |
| 207 (0xcf) | CAMSV1 | `ISP_Irq_CAMSV2` | `0xc04db9c8` | `request_threaded_irq` @`0xc04db128` |

Status registers — read once per IRQ entry, **nothing anywhere writes them** (no write-to-clear
register exists):

| IRQ | Register | ISP offset | ROM read site | Masked with | Evidence |
|---|---|---|---|---|---|
| CAM | `INT_P1_ST` | +0x4c | `0xc04ddda4` | `Mask \| ErrMask[P1]` (RAM: `0xc11891f8+0x914 \| +0x8dc`) | `camera_isp_D1.c:9089-9092` |
| CAM | `INT_P1_ST2` | +0x54 | `0xc04dddd4` | **unmasked** | `:9093` |
| CAM | `INT_P1_ST_D` | +0x5c | `0xc04dddec` | `Mask \| ErrMask[P1_D]` (`+0x91c \| +0x8e4`) | `:9095-9098` |
| CAM | `INT_P1_ST2_D` | +0x64 | `0xc04dde1c` | `Mask \| ErrMask[P1_ST2_D]` (`+0x920 \| +0x8e8`) | `:9099-9102` |
| CAM | `INT_STATUSX` | +0x70 | `0xc04dde4c` | `Mask \| ErrMask[STATUSX]` (`+0x928 \| +0x8f0`) | `:9110-9113` |
| CAM | `INT_STATUS2X` | +0x74 | `0xc04dde7c` | `+0x92c \| +0x8f4` | `:9114-9117` |
| CAM | `INT_STATUS3X` | +0x78 | `0xc04ddeac` | `+0x930 \| +0x8f8` | `:9118-9121` |
| CAM | `INT_P2_ST` | +0x6c | **not read** | — | only under `#ifdef __debugused__`, `:9104-9106` |
| CAMSV | `CAMSV_INT` | +0x500c | `0xc04dc814` (`add #20480` @`0xc04dc810`) | `Mask \| ErrMask[CAMSV]` (`+0x90c \| +0x944`) | `:8705` |
| CAMSV2 | `CAMSV2_INT` | +0x580c | `0xc04dbac0` (`add #22528` @`0xc04dbabc`) | `+0x910 \| +0x948` | `:8888` |

Enable/mask: **no hardware register**. The ROM performs no store to any of the nine addresses
across `ISP_Irq_CAM`/`CAMSV`/`CAMSV2`, `ISP_ioctl`, `ISP_Buf_CTRL_FUNC` and `ISP_probe`
(checked instruction-by-instruction; the only `str [#0x78]` hit is a store into the driver's
`IspInfo` array at `0xc04d6bb4`, base `[r8,#0x994]`). `ISP+0x4` is `CAM_CTL_EN1` (module enable;
its interrupt-clear line is commented out at `camera_isp_D1.c:3266`). Masking is a pure software
AND with `Mask[]`/`ErrMask[]` arrays in RAM.

Ack model: nothing clears these bits in software and the GIC lines are level-low, so **the read is
the acknowledgement** — each ISR must read each status word exactly once and must not read them
outside the ISR ([INFERENCE] on "read-to-clear" naming; the "no write path + level-low line +
single read only" triplet is proven). Consequence recorded in the driver: the debugfs full-window
dump is destructive while the pipeline streams.

Bits (`cameraisp/src/mt6735/inc/camera_isp_D1.h`, `:48-120`, `:300-340`):

- P1 / P1_D (+0x4c / +0x5c): `VS1` b0, `TG1_ERR` b4, `CQ0_ERR` b6, `IMGO_DROP` b8,
  `RRZO_DROP` b9, **`PASS1_DON` b10**, **`SOF1_INT` b12**, `AF_DON` b16, `FLK_DON` b17,
  `IMGO_ERR` b20, `AAO_ERR` b21, `LCSO_ERR` b22, `RRZO_ERR` b23, `ESFKO_ERR` b24,
  `FBC_RRZO_DON` b27, `FBC_IMGO_DON` b28, `DMA_ERR` b30. P1_ST2 (+0x54): per-DMA done b0-b11.
- STATUSX (+0x70): same P1 layout plus `CQ_ERR` b13, `PASS2_DON` b14, `TILE_DON` b15,
  `CQ_DON` b19.
- CAMSV/CAMSV2 (+0x500c / +0x580c): `VS1` b0, `TG_ERR` b4, `TG_GBERR` b5, `TG_DROP` b6,
  **`TG_SOF1` b7**, **`PASS1_DON` b10**, `IMGO_ERR` b16, `IMGO_OVERR` b17, `IMGO_DROP` b19.

SOF/DONE ordering — DONE is serviced first inside the same ISR:

- CAM: `ISP_DONE_Buf_Time.isra.6` (`0xc04dd3b8`) called at `0xc04de994`, the `b10` DONE test is
  `0xc04de370`, and the `b12` SOF block starts at `0xc04de9a0`; the P1_D pair repeats at
  `0xc04df308`/`0xc04dee78`. Downstream states it explicitly:
  `camera_isp_D1.c:9221` "service pass1_done first once if SOF/PASS1_DONE are coming together",
  DONE at `:9226`, SOF at `:9282`.
- CAMSV: `ISP_CAMSV_DONE_Buf_Time.isra.5` (`0xc04db354`) at `0xc04dca50`, then the `b7` SOF test
  `0xc04dca58` (`camera_isp_D1.c:8750` DONE, `:8776` SOF). CAMSV2 identically at `0xc04dbbec`.

Other registers the ISRs read (not status): CAM `+0xf0/+0xf4/+0xf8/+0xfc` FBC,
`0x3300/0x3320/0x34d4/0x34f4` DMA base addresses, `+0x44c`/`+0x244c` TG inter status
(V_CNT = bits 23:16, ROM `0xc04de38c`/`0xc04de3c0`); CAMSV `+0x501c/+0x5208/+0x544c`; CAMSV2
`+0x581c/+0x5228/+0x5c4c`.

### 3.4 SMI / LARB

- `m4u_config_port` @`0xc03814d8`: per-port RMW of `larb_base+0xfc0` (`ldr [r2,#0xfc0]`
  @`0xc03816a8`, `str` @`0xc03816c8`) and `+0xfc4` (`0xc03816d0/0xc03816ec`) — the baseline's
  0xfc0 bitmap writer is exactly right and is left untouched.
- LARB2 `CMD_THRT_CON` (0x24) bit 27: `smi_configuration.c:1276` (SMI_D1 branch)
  `M4U_WriteReg32(LARB2_BASE, 0x24, read & 0xf7ffffff)`; ROM
  `smi_set_nonconstant_variable` `ldr [r3,#0x24]` @`0xc04ca99c`, `bic #0x08000000` @`0xc04ca9a4`,
  `str [r3,#0x24]` @`0xc04ca9ac`. Register name from `smi/smi_reg.h:339`
  (`SMI_LARB_CMD_THRT_CON`); bit has no name anywhere in the vendor tree.
- LARB2 OSTDL `0x200 + 4*port`: `smi_configuration.c:127-134`
  (`smi_profile_setting_larb2_init`, 21 entries 0x200..0x250, value 1). Applied by
  `smi_bus_regs_setting()` (`smi_config_util.c:22`, `smi_common.c:1255`); ROM copy of the loop at
  `0xc04ca85c-0xc04ca890` over the per-larb bases at `0xc117da00+4*larb`. The table itself is in
  the ROM image at file offset `0xe80ad8` (vaddr `0xc0e849b4`), byte-identical to the source.
  Mainline already has the same slot: `SMI_LARB_OSTDL_PORT 0x200` +
  `SMI_LARB_OSTDL_PORTx()` (`mtk-smi.c:51-52`).
- SMI common init table (baseline) re-verified: offsets `0x100/0x234/0x230` =
  `0x1b/0xf7e19085/0x0e9f` (`smi_configuration.c:114-117`, ROM `.rodata`). Mainline's macro name
  for 0x230 is `SMI_READ_FIFO_TH` (`mtk-smi.c:33`), downstream calls the same offset
  `FIFO_TH1` — same register, no change.
- LARB2 port count 21 (`smi_reg.h:308`), bitmap bits 0..20 — unchanged.

## 4. Implementation description

`mtk-imgsys.c`
- `mtk_imgsys_cam_sw_ctl_reset(sw_ctl, mask, trig, hw_rst)` — P1/P2 sequence, exact ROM order.
- `mtk_imgsys_camsv_reset(sw_ctl)` — trigger, 0, read-back, HW_RST, 0.
- `mtk_imgsys_reset_one(priv, path)` — dispatch; `-ENODEV` if the CAMSV bank is unmapped,
  `-EINVAL` for an unknown path. `enum mtk_imgsys_reset_path` is private to this file and uses
  values 1..4, the downstream `ISP_REG_SW_CTL_RST_*` numbering.
- probe: runs `mtk_imgsys_reset_all()` once under `reset_lock`; there is no exported reset
  entry point (integration removed the unused helper — the camera-ISP character driver runs the
  same sequences itself under its register lock, which cannot sleep and therefore cannot use a
  PM-ref-taking helper).
- probe: maps ISP core + inner ISP reserved as before, then maps `isp_phys + 0x5000` (0x1000) for
  the CAMSV bank with `devm_ioremap()` (unreserved) — the vendor DT describes one 0x9000 ISP
  window (`rom_dtb_0.dts:3706`) so this bank is physically part of resource 0 but outside the
  0x4000 claim, and the seninf node holds 0x15008000..0x1500bfff; reserving would make the two
  drivers mutually fatal. Powers on, resets all four sub-blocks once (nothing can be using the
  block yet), powers off.
- `runtime_resume` now **only** enables the six gates. Rationale: downstream never resets on
  clock-enable, and this device's runtime PM state is independent of the char driver's pipeline —
  a debugfs read would otherwise reset the ISP mid-stream (the baseline did exactly that).
- debugfs `regs` unchanged (with a comment that the ISP/CAMSV status words are read-destructive).

`mtk-smi.c`
- `mtk_smi_larb_mt6735` (`config_port = mtk_smi_larb_config_port_mt6735`, `ostd` table) replaces
  `mtk_smi_larb_mt8167` for `mediatek,mt6735-smi-larb`.
- The config function keeps the identical `writel(*larb->mmu, base + 0xfc0)` bitmap write, adds
  the LARB2-only `0x24` bit-27 clear and the LARB2 OSTDL values, and returns early if
  `larb->mmu` is NULL (bind not run yet) instead of dereferencing it.
- Runs on every larb resume, which is where these registers need re-applying after the ISP/DIS
  power domains cycle.

## 5. Parity matrix

| Behaviour | Vendor reference | Status |
|---|---|---|
| P1 reset sequence (no status handshake) | `camera_isp_D1.c:3276-3302`, ROM `0xc04e75e4` | done |
| P2 reset sequence | `:3304-3330`, ROM `0xc04d3100`/`0xc04e8e44` | done |
| CAMSV reset | `:3332-3340`, ROM `0xc04e7a64` | done |
| CAMSV2 reset | `:3341-3349`, ROM `0xc04e840c` | done |
| Reset on demand through the vendor ioctls | `:9890-9911`, `:12161` | done (exported helper) |
| No reset on clock enable | `:3190-3256` | done (resume is clocks only) |
| Six gates, order, parents, offsets | `clk-mt6735.c:877-900`, `camera_isp_D1.c:3062-3100` | done (verified, unchanged) |
| SUFOD/FD gates | `clk-mt6735.c:899-900` | n/a for camera (other consumers) |
| ISP power domain, SRAM masks | `mt_spm_mtcmos.c:1137-1170` | done (baseline, verified) |
| LARB2 MMU bitmap 0xfc0 (+0xfc4 elsewhere) | `m4u_reg.h:314`, ROM `0xc03816a8` | done (unchanged) |
| LARB2 0x24 bit 27 | `smi_configuration.c:1276`, ROM `0xc04ca9a4` | done |
| LARB2 OSTDL 0x200+4i | `smi_configuration.c:127-134`, ROM `0xc04ca85c`+table | done |
| LARB2 OSTDL_SOFT_EN 0x64 | `smi_common.c:793` | **partial: reported, not implemented** |
| SMI common L1LEN/M4U_TH/FIFO_TH | `smi_configuration.c:114-117` | done (baseline, verified) |
| SMI common L1ARB 0x104-0x114 | `smi_configuration.c:1273-1282`+OOB loop | **blocked: undocumented layout, downstream OOB** |
| ISP interrupt map + ack model | ROM ISRs, `camera_isp_D1.c` | done (char driver's job; evidence delivered) |
| IRQ request from this driver | — | intentionally none |

## 6. Irrecoverable / not implemented (with attempts)

1. **`SMI_LARB_OSTDL_SOFT_EN` (0x64) = 0xffffffff.** The vendor writes it only from `smi_larb_init()`
   (`smi_common.c:776-797`), guarded by `SMI_LARB_STAT == 0`, with the comment "Got to enable OSTD
   before engine starts". No header or document defines 0x60/0x64 field semantics, and mainline
   `mtk-smi` programs `OSTDL_PORT` on every supported SoC (mt8186/8188/8189/8192/8195) **without**
   this write, so it is likely a software-override enable rather than a prerequisite. Writing it
   could change arbitration behaviour in an unprovable direction; the per-port values (proven) are
   therefore written and 0x64 is left at its hardware default. **Not implemented, documented.**
2. **`CMD_THRT_CON` bit 27 meaning.** Value and ROM write proven (§3.4); the field name is absent
   from `smi_reg.h`, `m4u_reg.h` and the ROM strings. Implemented as a pure bit clear; the
   semantic name is unknown.
3. **SMI common L1 arbitration 0x104/0x108/0x10c** (baseline deferral, re-checked): the downstream
   D1 loop runs `i < SMI_LARB_NR (4)` over a 3-entry `smi_common_l1arb_offset[]`
   (`smi_configuration.c:1278-1281`), i.e. it reads/writes one entry past the array; the values
   depend on the boot-time default saved by `save_default_common_val()`. Unreproducible without
   inventing a value. Not implemented.
4. **No DSP reset exists.** `grep -in 'dsp' camera_isp_D1.c` → no match; `ISP_Reset()` has only
   the four paths of §3.1, and the only soft-reset stores in the ROM ISP code are those four
   (every store to `0x8c`, `0x5010`, `0x5810` in `ISP_ioctl`, `ISP_MDPResetCallback`,
   `ISP_Buf_CTRL_FUNC` and `ISP_probe` was inspected); `readelf -sW` lists no `ISP_DSP*` symbol.
   The assignment's "DSP" variant does not exist for MT6735 D1. Nothing was invented.
5. **CAM1/CAM2 (SPI 184/185) have no handler in the vendor kernel** (ROM probe loop and
   `camera_isp_D1.c:11483-11501`). They are mapped but never requested; whether the hardware
   asserts them on this SoC is unknown. Reported, not claimed.
6. **Read-to-clear naming.** The absence of any write path plus level-low lines proves a read-only
   ack, but no vendor document names the mechanism. Marked `[INFERENCE]`; the driver does not rely
   on it beyond a debugfs warning comment.
7. **CAMSV window reservation.** The CAMSV bank physically overlaps the seninf node's declared
   range; the driver maps it unreserved on purpose. RemDphy confirmed (message of 2026-10-24) that
   every SENINF offset used by their slice is < 0x1000 and that their DTS fragment will declare
   `<0x15008000 0x1000>` (which matches `rom_dtb_0.dts:3720` `kd_camera_hw1@15008000
   reg = <0x15008000 0x1000>`), so 0x15009000 becomes free. The mapping is left unreserved anyway:
   it then works whether or not the seninf node is shrunk and whichever of the two probes first.
   Switching to a reserved `devm_ioremap_resource()` later needs no other change.
8. **`devm_ioremap` is not resource-reserved**, so nothing prevents a future driver from also
   touching 0x15009000. Documented in code; not fixable from this slice.

## 7. Build evidence

Log: `$M/logs/rem-camera-imgsys-build.log` (full, both passes). `TMPDIR=$M/tmp` (host `/tmp` is a
full 12 GiB tmpfs).

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export TMPDIR="$M/tmp"; export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/rem-camera-imgsys"
cp "$M/nura.config" .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV \
  -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC -e VIDEO_CAMERA_SENSOR \
  -e MEDIA_CAMERA_SUPPORT -e I2C -e REGULATOR -e COMMON_CLK -e NVMEM -e DEBUG_FS \
  -e MEDIA_PLATFORM_SUPPORT -e VIDEO_MTK_IMGSYS -e VIDEO_MTK_SENINF -e VIDEO_IMX219 \
  -e VIDEO_S5K5E3YX -e VIDEO_DW9807_VCM -e VIDEO_IMX219_EEPROM -e MTK_SMI -e MTK_IOMMU
make ARCH=arm64 LLVM=1 olddefconfig                # exit 0
make ARCH=arm64 LLVM=1 -j2 drivers/media/platform/mediatek/imgsys/ \
     drivers/clk/mediatek/clk-mt6735-imgsys.o drivers/memory/mtk-smi.o      # exit 0
make ARCH=arm64 LLVM=1 W=1 -j2 <same objects>                             # exit 0
```

Config after `olddefconfig`: `CONFIG_VIDEO_MTK_IMGSYS=y`, `CONFIG_COMMON_CLK_MT6735_IMGSYS=y`,
`CONFIG_MTK_SMI=y`, `CONFIG_MTK_IOMMU=y`.

Both passes:
```
  CC      drivers/clk/mediatek/clk-mt6735-imgsys.o
  CC      drivers/media/platform/mediatek/imgsys/mtk-imgsys.o
  CC      drivers/memory/mtk-smi.o
  AR      drivers/media/platform/mediatek/imgsys/built-in.a
```
`warning:`/`error:` occurrences in the log: **0**. No userspace component changed, so
`tools/mediatek/camera` was not touched.

Patch verification:
```
GIT_INDEX_FILE=…/b git apply --cached --check $M/patches/rem-camera-imgsys.patch   # against baseline tree -> OK
git apply --check -R $M/patches/rem-camera-imgsys.patch                            # against worktree     -> OK
```
Patch content: 4 files, +280/−65, 20 550 bytes; `git diff --stat` shows no file outside this
slice.

## 8. DTS fragment (text only; no DTS file was edited)

Recommended split: `ispsys` keeps registers/clocks/power; the five interrupt lines belong to
RemIsp's `mediatek,mt6735-camera-isp` node, because that driver owns the handlers.

```dts
/* arch/arm64/boot/dts/mediatek/mt6735.dtsi, inside the soc node */
ispsys: isp@15004000 {
	compatible = "mediatek,mt6735-ispsys";
	/*
	 * ISP core + inner ISP. The CAMSV/CAMSV2 bank at ISP_ADDR + 0x5000
	 * (0x15009000..0x15009fff) is mapped by the driver without a resource
	 * claim, because the vendor describes it as part of the same 0x9000
	 * ISP window while the seninf node owns the overlapping
	 * 0x15008000..0x1500bfff range.
	 */
	reg = <0 0x15004000 0 0x4000>,
	      <0 0x1500d000 0 0x1000>;
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	status = "disabled";	/* board DTS enables it */
};

/* Interrupt map — put these on the camera-ISP character driver node: */
camera_isp: camera-isp@15004000 {
	compatible = "mediatek,mt6735-camera-isp";
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>,	/* CAM0   -> ISP_Irq_CAM    */
		     <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,	/* CAM1   -> unused         */
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>,	/* CAM2   -> unused         */
		     <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,	/* CAMSV0 -> ISP_Irq_CAMSV  */
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;	/* CAMSV1 -> ISP_Irq_CAMSV2 */
	status = "disabled";
};
```

If the integrator prefers the interrupt list on `ispsys` (as in the earlier camera fragment), it is
harmless dead data, but the char driver would then have to fetch the numbers from that node.

**Superseded at integration:** the authoritative node for this character device is the complete
fragment in `$M/logs/rem-camera-isp.md` §8 (it also carries `reg`, `clocks` and `power-domains`).
Use that one; the abbreviated `camera_isp` block above only documents the interrupt map.

Reset sequencing is intentionally not exported: the camera-ISP character driver runs the same
vendor sequences itself under its register lock (atomic context, so it cannot call a helper that
takes a runtime-PM reference). The `enum mtk_imgsys_reset_path` values 1..4 are private to
`mtk-imgsys.c` and appear in the interrupt/reset evidence above for cross-checking.
