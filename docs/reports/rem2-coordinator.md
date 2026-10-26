# rem2 internals gap-closure — coordinator report

Round: final internals reverse-engineering (camera IOPIPE/3A, clk-buf, AEE DB client,
CLDMA DHL/EE stop).
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
`$M2` = `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work`

## 1. Result summary

| # | Gap | Leaf worktree | Patch | Report | Kernel objects | Userspace |
|---|---|---|---|---|---|---|
| 1 | Camera TPIPE / Pass-1 / Pass-2 tuning + pipeline programming | `$M2/worktrees/rem2-iopipe` | `patches/rem2-iopipe.patch` (16 files, +3576/−0) | `logs/rem2-iopipe.md` | none changed (no constant is grounded kernel-side) | `tools/mediatek/camera/iopipe`: `make` rc 0, `make check` 41 checks pass |
| 2 | clk-buf driver + RPC 0x4008 + consumers | `$M2/worktrees/rem2-clkbuf` | `patches/rem2-clkbuf.patch` (8 files, +814/−17) | `logs/rem2-clkbuf.md` | `mt_clkbuf_ctl.o`, `ccci_rpc.o`, `consys_mt6735.o`, `built-in.a`: rc 0, `W=1` clean | n/a |
| 3 | AEE/AED DB client interface | `$M2/worktrees/rem2-aee` | `patches/rem2-aee.patch` (12 files, +2378/−0) | `logs/rem2-aee.md` | `aee/` + `ccci_exception.o`: rc 0, `W=1` clean | `tools/mediatek/aee`: `make` rc 0, `make check` OK |
| 4 | CLDMA `stop_for_ee` + DHL RX-alive window | `$M2/worktrees/rem2-cldma` | `patches/rem2-cldma.patch` (3 files, +306/−18) | `logs/rem2-cldma.md` | `ccci_cldma.o`, `ccci_ccif.o`, `ccci_core.o`: rc 0 | n/a |

Full integrated build (`vmlinux` + `Image` + all modules, `-j2`, `ARCH=arm64 LLVM=1`): **rc 0**,
0 `error:` / 0 `warning:` lines (`logs/rem2-integration-build.log`).

Integration tree: `$M2/worktrees/rem2-integration` (detached `046883c0c` + baseline + all four
patches + the coordinator frozen headers). No commit, no DTS edit, no worktree outside
`$M2/worktrees/rem2-*` touched.

## 2. Baseline verification

`$M/patches/parity-baseline-all.patch` (2 735 230 bytes, sha256 per `logs/rem-camera-coordinator.md`)
applies cleanly to `$M/src/nura-linux` HEAD `046883c0c2b956fd7436d3c1156f25512d1f6a90`:
`git apply --check` in a fresh detached worktree succeeded (247 files, +85795/−589), and each of
the five `rem2-*` worktrees is `HEAD + git apply <baseline>` with no rejects.

### Tree-hygiene finding (pre-existing, not introduced here)

The baseline itself adds dead patch artifacts to the tree:
`drivers/misc/mediatek/connectivity/wmt/wmt_api.c.rej`,
`drivers/misc/mediatek/connectivity/include/mtk_wcn_wmt.h.orig` and `.../mtk_wcn_wmt.h.rej`.
The content of the `.rej` is already present in `wmt_api.c` (`mtk_wcn_wmt_ic_info_get()` at
`wmt_api.c:226`, `mtk_wcn_wmt_co_clock_flag_get()` at `:220`), so they are inert. They should be
dropped from `parity-baseline-all.patch` by its owner; this round does not touch them.

## 3. Coordinator-owned frozen interfaces (pre-applied to every worktree)

Added before the leaves started, so the four patches never fight over ABI headers:

| File | Content |
|---|---|
| `include/linux/aee.h` (new) | `ae_defect_attr`, `ae_exp_class`, `DB_OPT_*` (aee.h:25-31,33-53,186-218), `aed_{md,md32,scp,combo}_exception_api()` (aee.h:246-253) inside `IS_ENABLED(CONFIG_MTK_AEE_FEATURE)` with documented no-op fallbacks; ROM symbol addresses in the file header |
| `include/linux/clkbuf.h` (new) | `clk_buf_id`, `clk_buf_status`, `clk_buf_swctrl_status`, `CLKBUF_NUM`, `clk_buf_ctrl/get_swctrl_status/init` (mt_clkbuf_ctl.h:23-51) with config-gated fallbacks |
| `include/linux/ccci.h` (appended block) | `ccci_cldma_stop_for_ee()`, `ccci_cldma_dhl_rx_alive()` with the downstream/ROM semantics in the comment (modem_cldma.c:113,1449-1520,1999-2035) |

No leaf needed a `SCAFFOLD CHANGE REQUIRED`.

## 4. Integration

Applied in this order; the only conflict is the two shared parent files.

```sh
cd $M2/worktrees/rem2-integration          # HEAD + baseline
git apply $M/patches/rem2-clkbuf.patch
git apply $M/patches/rem2-cldma.patch
git apply $M/patches/rem2-iopipe.patch
git apply --exclude=drivers/misc/mediatek/Kconfig \
          --exclude=drivers/misc/mediatek/Makefile $M/patches/rem2-aee.patch
git apply -C1 --include=drivers/misc/mediatek/Kconfig \
               --include=drivers/misc/mediatek/Makefile $M/patches/rem2-aee.patch
```

Resolution: clkbuf appends `source ".../clkbuf/Kconfig"` / `obj-$(CONFIG_MTK_CLKBUF) += clkbuf/`
at the end; aee appends its two lines in the same place. The merged result is the union, with
`aee/` before `clkbuf/`:

```
source "drivers/misc/mediatek/aee/Kconfig"
source "drivers/misc/mediatek/clkbuf/Kconfig"

obj-$(CONFIG_MTK_AEE_FEATURE) += aee/
obj-$(CONFIG_MTK_CLKBUF) += clkbuf/
```

File ownership made the rest conflict-free: iopipe touches only `tools/mediatek/camera/**`;
clkbuf owns `clkbuf/**`, `ccci_rpc.c`, `consys_mt6735.c`; aee owns `aee/**`,
`include/uapi/linux/aee.h`, `ccci_exception.c`, `tools/mediatek/aee/**`; cldma owns
`ccci_cldma/**` and `ccci_ccif.c`.

Integration configuration (worktree-local `.config`, copied from the known-good full-build
`.config` of `$M/worktrees/integration`; `$M/nura.config` untouched):

```sh
./scripts/config -e MTK_CLKBUF -e MTK_AEE_FEATURE
make ARCH=arm64 LLVM=1 olddefconfig      # rc 0, no prompts
# CONFIG_MTK_CCCI=y CONFIG_MTK_CLKBUF=y CONFIG_MTK_AEE_FEATURE=y
```

## 5. Build and test evidence

Toolchain: `PATH=$M/llvmpkg/usr/bin:$M/bin:$PATH`, `ARCH=arm64 LLVM=1`, `TMPDIR=$M2/tmp`,
`-j2` (the host has ~2.6 GB available RAM).

1. **Touched kernel objects (`W=1`)** — `logs/rem2-integration-build.log` prefix and the leaf logs:

```
CC drivers/misc/mediatek/aee/aed-main.o
CC drivers/misc/mediatek/aee/aed-process.o
CC drivers/misc/mediatek/ccci/ccci_exception.o
CC drivers/misc/mediatek/ccci/ccci_ccif.o
CC drivers/misc/mediatek/ccci/ccci_rpc.o
CC drivers/misc/mediatek/ccci_cldma/ccci_cldma.o
CC drivers/misc/mediatek/clkbuf/mt_clkbuf_ctl.o
AR drivers/misc/mediatek/clkbuf/built-in.a
CC [M] drivers/misc/mediatek/connectivity/wmt/consys_mt6735.o
```

rc 0. Only pre-existing kernel-doc warnings remain (`ccci_ccif.c:146 ccci_ccif_send`
parameters, `ccci_ringbuf.c:209`), each proven pre-existing by rebuilding the baseline blob.

2. **Link contract** (from `llvm-nm` on the integrated objects):
   - `ccci_ccif.o`: `U aed_md_exception_api` (emitted only with `CONFIG_MTK_AEE_FEATURE=y`).
   - `aed-main.o`: `T aed_md_exception_api` + `__export_symbol_aed_md_exception_api`;
     `U aee_dump_ccci_debug_info` (provided/exported by `ccci_core.c:1145`).
   - `mt_clkbuf_ctl.o`: `__export_symbol_clk_buf_{ctrl,get_swctrl_status,init}` — required
     because `consys_mt6735.o` is built into `mtk_wcn_wmt.ko` while clkbuf is built in.

3. **Userspace** (integrated tree):
   - `tools/mediatek/camera/iopipe`: `make` rc 0 (`-Wall -Wextra -Werror`), `make check` rc 0,
     `selftest: all checks passed` (7 portmap, 23 regmap, 15 tuning records re-read from the ELF;
     one BNR record independently cross-checked against `llvm-objdump`: `movw@0x66dd0 = 0x4004`,
     `bfi@0x66d9e = bit 5`).
   - `tools/mediatek/aee`: `make` rc 0, `make check` → `selftest: ... OK: 5 KAT groups,
     24-byte AE_Msg, 32-byte aee_ioctl, 0x404c7007/0x41287007 thread-reg ioctls`.
   - `tools/mediatek/camera` (parent `make all` / `make check`, incl. otp) passes with the new
     iopipe hook added to both targets.

4. **Full kernel build** (`make ARCH=arm64 LLVM=1 -j2`, vmlinux + all modules) in
   `$M2/worktrees/rem2-integration`: **rc 0** (`logs/rem2-integration-build.log`, last line
   `FULL_BUILD_RC=0`), no `error:` and no `warning:` lines. Artifacts:
   `vmlinux` (258 MB), `arch/arm64/boot/Image`, `drivers/media/platform/mediatek/isp/mtk-isp.ko`,
   `.../mtk_wcn_wmt.ko`, `.../mtk_wcn_wlan.ko`, … `Module.symvers` exports
   `clk_buf_ctrl`, `clk_buf_get_swctrl_status`, `clk_buf_init`, `aed_md_exception_api` from
   vmlinux, and `mtk_wcn_wmt.ko` carries `U clk_buf_ctrl`, i.e. the built-in/module boundary is
   correct. Not done (DTS is not ours): `make dtbs` with the §7 fragment.

## 6. What each gap now does

### 6.1 Camera IOPIPE/3A (userspace, no kernel change)

`tools/mediatek/camera/iopipe` (libiopipe.a/.so + `iopipe` CLI) extracts, at run time and with
size/address self-verification, from the shipped vendor ELFs:

- `libcam.iopipe.so` port/path maps: `mPortIdxMappingAlltoP2` @0x3000c (272 B, 34×8),
  `mPortIdxMappingP2toAll` @0x304fc (136 B, 17×8, verified inverse of the first),
  `mSwHwPathMapping` @0x3012c (860 B, 43×20), `mTransformMapping` @0x3049c (96 B, 8×12),
  `mPortTypeMapping` @0x30488 (16 B, 2×8), `nsTimeoutToWait` @0x30120 (8 B). Record widths are
  proved by code stride (`FeatureStream::init` 0x11686/0x11698, `mla #0x14`) or by content
  invariants asserted in the selftest.
- `libcam.hal3a.v3.so` manager catalogue: 25 `ISP_MGR_*` module tokens, 90 template instances
  (`ESensorDev_T` 1/2/4 × `ESensorTG_T` 1/2 × PCAMode), 123 methods, 24 `ISP_NVRAM_*` types.
- 19 machine-verified TPIPE enable-word records over 13 modules, e.g. OBC→`0x4004` bit 4,
  BNR→`0x4004` bit 5, LSC→`0x4004` bit 6/`0x4008` bit 7, PGN→`0x4018` bit 1,
  CCM→`0x4018` bit 5, GGM→`0x4018` bit 6, plus the P1_D mirrors at `0x4010/0x4014`. Every record
  stores the MOVW/BFI instruction words and the owning `CTL_EN_*` call site; the selftest
  re-reads and re-decodes them from the ELF.
- CQ0C word builder reusing the encodings of `tools/mediatek/camera/libcamdrv/camdrv_cq.c`.
- 15 allow-listed tuning symbols (9 file-backed, e.g. `ShadingATNTable` @0x50af18/504 B,
  `LomoFilterGGM` @0x44e520/11520 B, AWB log LUTs, GMA base), 6 reported as runtime `.bss`,
  and the per-sensor NVRAM default *generators* (functions, not blobs).

No kernel register was added because none is grounded: the enable words are written by the 3A
userspace; the kernel only reads `ISP_REG_EN1` for a debug line (`mtk-isp.c:2833`).

### 6.2 clk-buf (driver + RPC 0x4008 + CONSYS)

New `drivers/misc/mediatek/clkbuf/` (`CONFIG_MTK_CLKBUF`, bool): platform driver on
`mediatek,rf_clock_buffer`, SPM bank via the existing `scpsys` syscon (`mediatek,spm` phandle,
offsets 0x014/0x3b4/0x60c/0x610/0x614/0x620/0x624-0x634 from mt_spm.h), ported BSI command word
(`BSI_CW_CNT 30`, `BSI_CW_ADDR 252`, `BSI_CW_DEFAULT 0x01E8F`), `spm_ap_bsi_gen` and
`spm_ap_mdsrc_req` sequences, `/sys/power/clk_buf/clk_buf_ctrl`, late initcall. Deviations
(no `BUG()` on missing DT; bounded polls; sysfs under the driver lock) are listed in the leaf
report §6. `IPC_RPC_GET_RF_CLK_BUF_OP` now returns the downstream 13-byte payload (count 4 →
DT `mediatek,clkbuf-config` + SW shadow; count ≠ 4 → `0xFF`+zeros; driver unbound →
`FS_FUNC_FAIL`). CONSYS co-clock power-on/off drives `CLK_BUF_CONN`, matching
`CONSYS_CLOCK_BUF_CTRL 1`. ROM symbols confirm the port is the shipped driver
(`clk_buf_ctrl 0xc03492d0`, `clk_buf_get_swctrl_status 0xc03493f4`, `clk_buf_init 0xc0349424`,
`spm_ap_bsi_gen 0xc0367ffc`, `spm_ap_mdsrc_req 0xc036bf18`).

### 6.3 AEE DB client

New `drivers/misc/mediatek/aee/` (`CONFIG_MTK_AEE_FEATURE`, bool): `/dev/aed0` (EE) and
`/dev/aed1` (KE) misc devices named exactly as the ROM registers them
(`vmlinux.strings:45359-45360`), the 24-byte `AE_Msg` read/write/poll protocol, EE record
queue/worker with the 5-minute close handshake, the detail response including the
`== CCCI LOG ==` section via `aee_dump_ccci_debug_info()`, `/proc/aed/current-ee-coredump`, the
ioctls the shipped client actually uses (`AEEIOCTL_GET_THREAD_REG` in both the 296-byte
`0x41287007` and 76-byte `0x404c7007` encodings found in `libaed.so`, plus `GET_PROCESS_BT`,
`SET_AEE_MODE`, `CHECK_SUID_DUMPABLE`), and direct, GFP_ATOMIC-safe
`aed_{md,md32,scp,combo}_exception_api()` entry points (exported GPL). `ccci_ex_build()` now
hands its EX_LOG_T record, the 256-byte modem-image head and the `mdN:<reason>` detail to
`aed_md_exception_api()` with `DB_OPT_DEFAULT|DB_OPT_FTRACE`, guarded by the frozen header.
Client evidence: `/lib/libaed.so` in `system.raw.img` (exported `aee_aed_raise_exception`,
`aee_aed_dump_db_always`, `aee_ioctl_get_reg`, …; `/dev/aed0` literal at file offset 0xf0a6;
raises go over the local socket `com.mtk.aee.aed`), ROM `aed_md_exception_api 0xc0628298`,
`aed_ee_open 0xc062a328`, `aed_ke_open 0xc062a5a0`, `aed_ioctl 0xc062a614`.

### 6.4 CLDMA EE stop / DHL window

`ccci_cldma_stop_for_ee()` and `ccci_cldma_dhl_rx_alive()` implement the downstream
`cldma_stop_for_ee` sequence register-for-register: TX `UL_STOP_CMD=0xFF` + dummy read + poll
`UL_STATUS`; RX `SO_STOP_CMD=0xF0` + poll `SO_STATUS&0xF0` (queues 4..7 are the stop set —
the downstream "NONSTOP_QUEUE_MASK" comment is inverted relative to the mask; queues 0..3 stay
armed and are exactly the `.rxq_exp` queues of the port table: fs=1, ttyC1=2, log_ctrl=3);
latch clears/masks as in modem_cldma.c:1504-1518; bounded 1.6 M-poll timeout with the same
100 k report cadence. `ccci_ccif_exception()` now calls it at `HIF_EX_INIT` (instead of a full
stop), reports a timeout through `aed_md_exception_api()`, keeps the 2 s DHL window at
`HIF_EX_CLEARQ_DONE`, closes it with `ccci_cldma_dhl_rx_alive(false)`, then performs the normal
full stop/flush. Verified against the inlined ROM copy in `md_cd_ccif_work` (0xc0528558-0xc0528928:
`UL_STOP_CMD=0xFF` @0xc05285d8, `SO_STOP_CMD=0xF0` @0xc0528720, `SO_STATUS&0xF0` @0xc0528754,
`L2RIMSR0=0x00F000F0` @0xc05288c4, `msleep(2000)` @0xc0528a0c, `SO_CFG` bit-0 re-check
@0xc0528d04) and `md_cd_ccif_delayed_work` (0xc0527c80). `md_cd_ee_callback` (0xc052d648) in
this ROM only toggles the watchdog, so it is not the sequence carrier.

## 7. DTS fragment (not applied — owner must merge)

Only clkbuf needs one; AEE/CCDMA/iopipe need none. Nura already has the SPM bank as
`scpsys: syscon@10006000` (`mt6735.dtsi:182-185`), so the node is root-level with a phandle:

```dts
/ {
	rf_clock_buffer {
		compatible = "mediatek,rf_clock_buffer";
		mediatek,clkbuf-quantity = <4>;
		mediatek,clkbuf-config = <2 1 1 1>;
		mediatek,spm = <&scpsys>;	/* [INFERENCE] SPM bank 0x10006000 */
		status = "okay";
	};
};
```

Values are the ROM DTB (`rom_dtb_0.dts:4602-4607`). The `mediatek,spm` property is the only
inferred piece (downstream used a global `SPM_BASE`). Without the node the driver does not bind:
`clk_buf_ctrl()` returns false, CONSYS only logs, and RPC 0x4008 answers `FS_FUNC_FAIL` — all
deliberate, no `BUG()`, no fabricated status.

## 8. Deferred / blocked items (consolidated, exact missing input)

| Item | Status | Missing input |
|---|---|---|
| Camera per-module register **values** (BNR/LSC/CCM/GGM/… payload) | deferred | no static address→value table: `apply()` loops over runtime `(reg,val)` pairs built by `put()`; values come from per-sensor NVRAM/OTP at run time |
| Camera enable words for MIXER3, NR3D, MFB, G2C_SHADE | deferred | their `apply()` bodies contain no CAM-window immediate (machine-checked); addresses exist only as NVRAM data |
| FLK window / FKO DMA address registers | deferred | `ISP_MGR_FLK_CONFIG` methods are 2-byte aliases inside `Hal3AAdapter::doDataCb` (0x3d07b), no implementation in `libcam.hal3a.v3.so` |
| AE/AWB statistic config register lists | deferred | `config()` takes runtime structs; no register immediates |
| Any camera **DSP** tuning/pipeline | not present in any source | `grep -aic dsp` = 0 across all six vendor libs and downstream cameraisp; the post-processing stage is P2/DIP (MIXER3/PCA/RPG/NBC). Nothing invented |
| Per-sensor ISP NVRAM defaults | deferred | generated by functions (`RAWSensorInfo<537>/<24112>::impGetDefaultData`, 208 B each), not stored blobs |
| clk-buf NFC consumer | deferred | Nura has no NFC driver at all (downstream call sites `nfc/mt6735/mt6605.c:890,897`); API exported and ready |
| clk-buf `rf_clock_buffer` DT node | deferred to DTS owner | fragment in §7 |
| AEE userspace AE_IND raise path over `com.mtk.aee.aed` | deferred, userspace-only | the closed-source AED daemon/APK is absent from `system.raw.img` (only the `libaed.so` client is shipped); no kernel interface involved |
| AEE ipanic / mrdump / dram-console / red-screen / RT-monitor | deferred | out of scope for this gap; downstream sources exist (`aee/ipanic`, `aee/mrdump`) but need their own port |
| 296-byte `AEEIOCTL_GET_THREAD_REG64` payload layout | implemented, inner layout [INFERENCE] | only the command number and size (0x128) are observable; field split follows 3.18/4.x arm64 `user_pt_regs`+`orig_x0`+`syscallno` |
| CLDMA TX ring purge at `HIF_EX_INIT` (downstream `md_cd_clear_all_queue(OUT)`, modem_cldma.c:2007) | partial | still performed by the existing full stop at `HIF_EX_CLEARQ_DONE`; moving it earlier would need a verified per-queue TX purge that keeps the DHL RX rings armed — engine-level, not source-blocked, flagged for hardware validation |
| CLDMA L3 per-queue bit attribution | deferred | no source attributes L3 bits per queue; L3 uses the full-stop values when closing the window (documented in code and leaf report) |

## 9. Files delivered

```
patches/rem2-iopipe.patch     108 850 B
patches/rem2-clkbuf.patch      28 470 B
patches/rem2-aee.patch         69 248 B
patches/rem2-cldma.patch       16 344 B
logs/rem2-iopipe.md   logs/rem2-clkbuf.md   logs/rem2-aee.md   logs/rem2-cldma.md
logs/rem2-integration-build.log   (integrated full build, FULL_BUILD_RC=0, 0 warnings, 0 errors)
```

Each leaf patch applies to `HEAD + parity-baseline-all.patch`; the four together apply in the
order of §4 with the documented parent-Kconfig/Makefile resolution.
