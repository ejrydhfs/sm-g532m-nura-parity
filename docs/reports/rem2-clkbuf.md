# rem2-clkbuf — MT6735 RF clock buffer (BSI) driver, CCCI RPC 0x4008 and WCN wiring

## 1. Gap, files changed, worktree, result

**Gap.** The Nura tree had no RF clock buffer driver at all. `IPC_RPC_GET_RF_CLK_BUF_OP`
(op `0x4008`) answered `FS_FUNC_FAIL` for every valid request because
`clk_buf_get_swctrl_status()` and the `mediatek,rf_clock_buffer` consumer did not exist, and
the WMT/CONSYS co-clock power sequence never touched the CONN clock buffer.

**Worktree / HEAD.** `$M2/worktrees/rem2-clkbuf`, detached `046883c0c` +
`$M/patches/parity-baseline-all.patch` + the coordinator's frozen headers
(`include/linux/clkbuf.h`, `include/linux/aee.h`, CLDMA block of `include/linux/ccci.h`).
No reset/checkout/stash/commit was performed. No DTS was edited. No other worktree was
touched.

**Result.** The downstream driver is ported as an arm64 7.x platform driver over the
`scpsys` syscon, the frozen `clkbuf.h` API is implemented and exported, CCCI op `0x4008`
now returns the downstream 13-byte payload, and CONSYS toggles `CLK_BUF_CONN` in co-clock
mode. All touched objects compile at `W=1`, rc 0.

**Diffstat** (`git diff T_BASE T_ALL`, see §8 for how the patch was produced):

```
 drivers/misc/mediatek/Kconfig                      |   1 +
 drivers/misc/mediatek/Makefile                     |   1 +
 drivers/misc/mediatek/ccci/ccci_rpc.c              |  96 +++-
 drivers/misc/mediatek/clkbuf/Kconfig               |  22 +
 drivers/misc/mediatek/clkbuf/Makefile              |   5 +
 drivers/misc/mediatek/clkbuf/mt_clkbuf_ctl.c       | 553 +++++++++++++++++++++
 drivers/misc/mediatek/clkbuf/mt_clkbuf_ctl.h       | 127 +++++
 .../misc/mediatek/connectivity/wmt/consys_mt6735.c |  26 +
 8 files changed, 814 insertions(+), 17 deletions(-)
```

## 2. Evidence

### 2.1 Downstream GPL source (`$M/src/downstream-grandpplte`)

Driver (`drivers/misc/mediatek/base/power/mt6735/mt_clkbuf_ctl.c`, 314 lines):

| Item | Citation (verified with `nl -ba`) |
|---|---|
| copyright/GPL header | `:1-11` |
| `DEFINE_MUTEX(clk_buf_ctrl_lock)` | `:35` |
| sysfs attr macros | `:37-56` |
| `clk_buf_swctrl[4] = {ENABLE, DISABLE, DISABLE, DISABLE}` | `:59-64` |
| BSI constants (`BSI_CW_CNT 30`, `BSI_CLK_MASK 0x7`, `BSI_CW_ADDR 252`, `BSI_CW_DEFAULT 0x01E8F`, data/read bit 0) | `:66-73` |
| `clk_buf_spm_cfg[5]`, `clk_buf_bit_ctrl` | `:75-83` |
| `clk_buf_CW()` | `:85-96` |
| `clk_buf_Send_BSI_CW()` | `:98-149` (calls `spm_ap_bsi_gen()` at `:147`) |
| `spm_clk_buf_ctrl()` | `:151-169` — `spm_ap_mdsrc_req(1)` `:156`, read+mask `SPM_SLEEP_MDBSI_CON & ~0x7` `:158`, `status[i] << (i-1)` `:160-161`, write `:163`, `udelay(2)` `:165`, `spm_ap_mdsrc_req(0)` `:167` |
| `SPM_PWR_STATUS_MD (1U << 0)` | `:170` |
| `clk_buf_ctrl()` | `:171-217` — DT read + `BUG()` `:177-187`, id/HW-control early returns `:189-198`, shadow update `:202`, PWR_STATUS/PWR_STATUS_2ND MD check `:203-208`, dispatch `:209-213` |
| `clk_buf_get_swctrl_status()` | `:219-225` |
| sysfs store/show, group, `DEFINE_ATTR_RW` | `:227-294` |
| `clk_buf_fs_init()` / `clk_buf_init()` (`power_kobj`) | `:296-313` |

Header: `mt6735/mt_clkbuf_ctl.h:23-51` (enums `clk_buf_id`, `clk_buf_status`,
`clk_buf_swctrl_status`, `CLKBUF_NUM 4`, prototypes) — mirrored verbatim by
`include/linux/clkbuf.h`.

SPM side:

| Item | Citation |
|---|---|
| register offsets `SPM_POWER_ON_VAL1 0x014`, `SPM_PCM_REG13_DATA 0x3b4`, `SPM_PWR_STATUS 0x60c`, `SPM_PWR_STATUS_2ND 0x610`, `SPM_SLEEP_MDBSI_CON 0x614`, `SPM_BSI_GEN 0x620`, `SPM_BSI_EN/CLK/D0/D1/D2_SR 0x624/0x628/0x62c/0x630/0x634` | `base/power/mt6735/mt_spm.h:87,156,176,177,179-185` |
| BSI pad index enum | `mt_spm.h:321-327` |
| `spm_ap_bsi_gen()`: 5 SR writes, `GEN |= SPM_BSI_START` (bit0), unbounded poll | `base/power/spm_v1/mt_spm.c:932,934-945` |
| `spm_ap_mdsrc_req()`: refcount `:2040-2046`, `POWER_ON_VAL1 |= 1<<17` `:2049`, 3 ms settle when `R13_MD1_APSRC_REQ` clear `:2054-2058`, ACK poll 10x1 ms `:2060-2071`, release `:2073-2096` | `base/power/spm_v1/mt_spm_sleep.c:2031-2096` |
| `R13_MD1_APSRC_REQ (1<<4)`, `R13_AP_MD1SRC_ACK (1<<5)` | `base/power/mt6735/mt_spm_internal.h:77-78` |
| SPM base = 0x10006000 (`sleep@10006000`) | `arch/arm/boot/dts/mt6735.dtsi:367-369` |

Consumers / integration:

| Item | Citation |
|---|---|
| RPC op handler, count check, DT read, swctrl fill, count!=4 → `0xFF`+zeros | `eccci/port_kernel.c:1650-1707`; op id enum `eccci/port_kernel.h:139` (`IPC_RPC_GET_RF_CLK_BUF_OP = 0x4008`) |
| `FEATURE_RF_CLK_BUF` | `eccci/mt6735/ccci_config.h:57` |
| CONSYS co-clock power-on `clk_buf_ctrl(CLK_BUF_CONN, 1)` | `connectivity/common/conn_soc/mt6735/mtk_wcn_consys_hw.c:353-358` |
| CONSYS co-clock power-off `clk_buf_ctrl(CLK_BUF_CONN, 0)` | `mtk_wcn_consys_hw.c:729-734` |
| `CONSYS_CLOCK_BUF_CTRL 1` | `.../mt6735/include/mtk_wcn_consys_hw.h:33` |
| `late_initcall` → `clk_buf_init()` | `base/power/mt6735/mt_pm_init.c:550-559` |
| NFC consumer (deferred, see §4) | `nfc/mt6735/mt6605.c:890` (enable), `:897` (disable) |
| GPS consumer is MT6580-only, **not** MT6735 behaviour | `connectivity/gps/stp_chrdev_gps.c:427-429,459-461` inside `#if defined(CONFIG_ARCH_MT6580)` |

ROM DTB node (`$M/zip/rom_dtb_0.dts:4602-4607`, root level):

```
rf_clock_buffer {
    compatible = "mediatek,rf_clock_buffer";
    mediatek,clkbuf-quantity = <0x04>;
    mediatek,clkbuf-config = <0x02 0x01 0x01 0x01>;
    status = "okay";
};
```

and `scpsys@10000000 { compatible = "mediatek,mt6735-scpsys"; reg = <0x10000000 0x1000
0x10006000 0x1000>; }` (`rom_dtb_0.dts:307-311`) — i.e. the vendor scpsys node carries the
SPM bank 0x10006000, which Nura already has as
`scpsys: syscon@10006000` (`arch/arm64/boot/dts/mediatek/mt6735.dtsi:182-185`,
`compatible = "mediatek,mt6735-scpsys", "syscon", "simple-mfd"`, `reg = <0 0x10006000 0 0x1000>`,
inside `&{/soc}`).

### 2.2 ROM kernel ELF (`$M/zip/vmlinux.elf`, ARM32)

`llvm-readelf -sW` (section index 1 = `.text`, sizes are 0 in the recovered symtab):

```
c0348e20  LOCAL   clk_buf_ctrl_show
c034905c  LOCAL   spm_clk_buf_ctrl.constprop.0
c03490dc  LOCAL   clk_buf_Send_BSI_CW.constprop.1
c0349208  LOCAL   clk_buf_ctrl_store
c03492d0  GLOBAL  clk_buf_ctrl
c03493f4  GLOBAL  clk_buf_get_swctrl_status
c0349424  GLOBAL  clk_buf_init
c0367ffc  GLOBAL  spm_ap_bsi_gen
c036bf18  GLOBAL  spm_ap_mdsrc_req
```

`llvm-objdump -d --triple=armv7a-none-eabi --start-address=0xc03492d0
--stop-address=0xc03493f4 vmlinux.elf` (window used):

```
c03492f8  bl   of_find_compatible_node
c0349314  bl   of_property_read_u32_array
c0349318  cmp  r4, #3                  ; id >= CLK_BUF_INVALID
c0349330  cmp  r4, #0 / cmpeq r3, #2   ; config[0] == CLOCK_BUFFER_HW_CONTROL
c034933c  cmp  r4, #1 / cmpeq r2, #2
c0349350  cmp  r4, #2 / cmpeq r12, #2
c0349368  cmp  r4, #3 / cmpeq r5, #2
c0349380  bl   mutex_lock
c0349394  ldr  r1, [r2, #0x60c]        ; SPM_PWR_STATUS
c03493a4  ldr  r2, [r3, #0x610]        ; SPM_PWR_STATUS_2ND
c03493b0  bl   clk_buf_Send_BSI_CW.constprop.1
c03493d0  bl   spm_clk_buf_ctrl.constprop.0
```

`$M/zip/vmlinux.strings` contains `********** clock buffer state **********`,
`BSI  switch on/off: echo bsi en1 en2 en3 en4 > /sys/power/clk_buf/clk_buf_ctrl`,
`APBSI switch on/off: ...` and `mediatek,rf_clock_buffer`, i.e. the shipped ROM runs exactly
the downstream driver. The ROM disassembly was used to confirm the control flow and the
0x60c/0x610 offsets; every constant in the port is nevertheless taken from the cited
downstream source lines.

### 2.3 Vendor userspace ELFs

Not used for this gap: the RF clock buffer is a kernel-internal SPM facility with no
userspace ABI beyond the ROM's `/sys/power/clk_buf/clk_buf_ctrl` debug knob.

## 3. Implementation

### 3.1 New: `drivers/misc/mediatek/clkbuf/`

* `Kconfig` — `config MTK_CLKBUF`, `bool "MT6735 RF clock buffer control"`,
  `depends on ARCH_MEDIATEK || COMPILE_TEST`, `depends on PM` (uses `power_kobj`),
  `depends on MFD_SYSCON` (regmap of the `scpsys` syscon). Built-in only: `power_kobj` has
  no `EXPORT_SYMBOL` in 7.x (`kernel/power/main.c:737`, declaration
  `include/linux/kobject.h:210`, no export anywhere in the tree), so a `=m` driver could not
  link. The WMT consumer *is* a module, so the API symbols are `EXPORT_SYMBOL_GPL`.
* `Makefile` — `obj-$(CONFIG_MTK_CLKBUF) += mt_clkbuf_ctl.o`.
* `mt_clkbuf_ctl.h` — private hardware definitions: SPM offsets (mt_spm.h:87,156,176-185),
  `SPM_BSI_START` (mt_spm.c:932), `R13_MD1_APSRC_REQ`/`R13_AP_MD1SRC_ACK`/
  `SPMPWR_AP_MDSRC_REQ` (mt_spm_internal.h:77-78, mt_spm_sleep.c:2049),
  `SPM_PWR_STATUS_MD` (mt_clkbuf_ctl.c:170), `SPM_SLEEP_MDBSI_MASK 0x7` (:158), BSI pad
  enum (mt_spm.h:321-327), BSI CW constants (:66-73), `[INFERENCE] BSI_GEN_POLL_US 1000`,
  MDSRC timing constants, and `struct mt_clkbuf`.
* `mt_clkbuf_ctl.c` — the port proper:
  * `mt_clkbuf_cw()` (:85-96) — command word from `status[1..3]`.
  * `mt_clkbuf_bsi_gen()` (spm_v1/mt_spm.c:934-945) — five SR writes, `GEN |= BIT(0)`,
    poll until clear (`[INFERENCE]` bounded to 1000 x 1 us instead of unbounded).
  * `mt_clkbuf_send_bsi_cw()` (:98-149) — bit-banged 30-bit word, operation-for-operation.
  * `mt_clkbuf_mdsrc_req()` (mt_spm_sleep.c:2031-2096) — reference-counted
    `SPM_POWER_ON_VAL1` bit 17 with the 3 ms settle and 10x1 ms ACK poll.
  * `mt_clkbuf_spm_ctrl()` (:151-169) — `SPM_SLEEP_MDBSI_CON` bits[2:0] from
    `status[1..3]`, `udelay(2)` while the MDSRC request is held.
  * `clk_buf_ctrl()` (:170-217) — id/HW-mode early returns, shadow update, MD-power
    dispatch on `SPM_PWR_STATUS`/`SPM_PWR_STATUS_2ND` bit 0, all under `clkbuf.lock`.
  * `clk_buf_get_swctrl_status()` (:219-225) — software shadow.
  * sysfs `/sys/power/clk_buf/clk_buf_ctrl` (:227-282) with the downstream text and
    `bsi`/`apbsi` commands (8-bit hex values, `-EPERM` on a malformed line, `-EINVAL` on an
    unknown command **after** the shadow update, exactly like downstream).
  * `clk_buf_init()` (:296-313) — `sysfs_create_group(power_kobj, ...)`, idempotent;
    `late_initcall(mt_clkbuf_late_init)` (:471-472) mirrors `mt_pm_init.c:550-559`.
  * `platform_driver` on `mediatek,rf_clock_buffer`: required `mediatek,clkbuf-config`
    (4 x u32) and required `mediatek,spm` phandle → `device_node_to_regmap()` of the
    `scpsys` syscon. Missing property/phandle/regmap fails the probe with a `dev_err`.

API surface (exactly the frozen `include/linux/clkbuf.h`, unchanged):

```c
bool clk_buf_ctrl(enum clk_buf_id id, bool onoff);       /* false: not ready/HW/invalid/err */
void clk_buf_get_swctrl_status(enum clk_buf_swctrl_status *status);
bool clk_buf_init(void);                                 /* true when ready && sysfs added  */
```

`clk_buf_ctrl()` and `clk_buf_get_swctrl_status()` are `EXPORT_SYMBOL_GPL` (the WMT
consumer is `mtk_wcn_wmt.ko`); `clk_buf_init()` is exported for symmetry and future
in-kernel consumers. `llvm-nm` on the object shows `__export_symbol_clk_buf_ctrl`,
`__export_symbol_clk_buf_get_swctrl_status`, `__export_symbol_clk_buf_init`.

No scaffold change was required: `include/linux/clkbuf.h` is sufficient as frozen.

### 3.2 `drivers/misc/mediatek/ccci/ccci_rpc.c`

`ccci_rpc_get_rf_clk_buf()` rewritten to the downstream response
(`port_kernel.c:1650-1707`), nothing else in the file changed:

* `arg_count != 1` / short argument → two `FS_PARAM_ERROR` words (unchanged, :1656-1666).
* `FS_NO_ERROR` in word 0, then the 13-byte `struct ccci_rpc_clkbuf_result`
  (`ccci_rpc.h:148-153`: `u16 CLKBuf_Count; u8 CLKBuf_Status[4]; u8 CLKBuf_SWCtrl_Status[4]`,
  `__packed`) in word 1.
* `count != CLKBUF_MAX_COUNT` → `CLKBuf_Count = 0xFF` and an all-zero array
  (`port_kernel.c:1675-1679`).
* driver not built in / not bound to `mediatek,rf_clock_buffer` → log + `FS_FUNC_FAIL` in
  both words (documented deviation, §6).
* otherwise `count = 4`, `CLKBuf_Status[]` from `mediatek,clkbuf-config`
  (`of_find_compatible_node` + 4 x u32, `port_kernel.c:1681-1694`),
  `CLKBuf_SWCtrl_Status[]` from `clk_buf_get_swctrl_status()`.
* "probed" is decided without new symbols: `of_find_device_by_node()` +
  `device_is_bound()` on the `rf_clock_buffer` node (device must exist and be bound).
* Added includes: `<linux/clkbuf.h>`, `<linux/device.h>`, `<linux/of_platform.h>`.

### 3.3 `drivers/misc/mediatek/connectivity/wmt/consys_mt6735.c`

* Power-on step 2 (`consys_reg_ctrl(true)`): the existing
  `if (!wmt_lib_co_clock())` VCN28 branch gained its `else`, which calls
  `clk_buf_ctrl(CLK_BUF_CONN, 1)` (mtk_wcn_consys_hw.c:353-358) and records
  `consys.clk_buf_on`; a `false` return only warns (downstream ignores it too).
* Power-off: a new `if (consys.clk_buf_on)` block releases the buffer
  (mtk_wcn_consys_hw.c:729-734) before the VCN28/VCN18 teardown.
* Error unwind: `err_vcn28:` releases the buffer if the power-on failed after step 2
  (downstream has no unwind; see §6).
* No other line in the file changed; the buffer stays inside the existing
  `wmt_lib_co_clock()` condition.

## 4. Parity matrix

| Behaviour | ROM/downstream reference | Nura before | After | Status |
|---|---|---|---|---|
| DT node `mediatek,rf_clock_buffer` bound to a driver | mt_clkbuf_ctl.c:171-187; rom_dtb_0.dts:4602-4607 | no node, no consumer | platform_driver probes, caches `mediatek,clkbuf-config` | done |
| `mediatek,clkbuf-config` read (4 u32) | :177-183, port_kernel.c:1685-1687 | — | probe (required) + rpc handler | done |
| HW-control early returns (config==2) | :189-198 | — | implemented | done |
| Software shadow defaults `{1,0,0,0}` | :59-64 | — | `struct mt_clkbuf.swctrl` initializer | done |
| MD BSI path on `SPM_SLEEP_MDBSI_CON` bits[2:0] | :151-169 | — | `mt_clkbuf_spm_ctrl()` | done |
| MDSRC request/refcount/settle/ACK | mt_spm_sleep.c:2031-2096 | — | `mt_clkbuf_mdsrc_req()` | done |
| AP BSI command word to SPM generator | :85-149, mt_spm.c:934-945 | — | `mt_clkbuf_cw()` + `mt_clkbuf_send_bsi_cw()` + `mt_clkbuf_bsi_gen()` | done |
| MD power gate (`PWR_STATUS`&`PWR_STATUS_2ND` bit0) | :170,203-208 | — | implemented | done |
| `clk_buf_get_swctrl_status()` shadow readback | :219-225 | absent (header stub) | implemented + `EXPORT_SYMBOL_GPL` | done |
| `/sys/power/clk_buf/clk_buf_ctrl` show/store (`bsi`, `apbsi`) | :227-294 | absent | sysfs group below `power_kobj` | done |
| `late_initcall` → `clk_buf_init()` | :296-313, mt_pm_init.c:550-559 | — | `late_initcall(mt_clkbuf_late_init)` | done |
| CCCI op `0x4008` full payload (count 4 + DT config + swctrl) | port_kernel.c:1650-1707 | `FS_FUNC_FAIL` for count==4 | downstream payload | done |
| CCCI op `0x4008` wrong count → `0xFF` + zeros | port_kernel.c:1675-1679 | already matched | unchanged, still matches | done |
| CCCI payload byte layout | `struct ccci_rpc_clkbuf_result` ccci_rpc.h:148-153 | matched | matched (`__packed`, 13 bytes) | done |
| WCN co-clock CONN buffer on/off | mtk_wcn_consys_hw.c:353-358,729-734 | absent | wired in `consys_reg_ctrl()` | done |
| AUDIO buffer from GPS open/close | stp_chrdev_gps.c:427-429,459-461 — **inside `CONFIG_ARCH_MT6580`** | n/a | intentionally not added (not MT6735 behaviour) | done (not applicable) |
| NFC buffer on `MTK_NFC_IOCTL_CMD_CLOCK_BUF_ENABLE/DISABLE` | mt6605.c:890,897 | Nura has no NFC driver/ioctl at all | API exported, no caller | **deferred** — missing input: an NFC driver port (`drivers/misc/mediatek/nfc/mt6735/`, MT6605) with those two ioctl commands; nothing in Nura implements them today |
| Root-level `rf_clock_buffer` DT node incl. `mediatek,spm` | rom_dtb_0.dts:4602-4607; phandle is [INFERENCE] | node absent | not edited (shared DTS) | **deferred** — fragment in §7 for the coordinator |

## 5. Build evidence

Environment:

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export M2=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work
export TMPDIR=$M2/tmp
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M2/worktrees/rem2-clkbuf
cp "$M/nura.config" .config
./scripts/config -e MTK_CLKBUF -e MTK_CCCI -e MTK_CCCI_CLDMA -e MTK_CCCI_UTIL \
                 -m MTK_WCN_WMT -m MTK_BTIF
make ARCH=arm64 LLVM=1 olddefconfig          # rc 0
```

Resulting relevant config (worktree `.config`, `$M/nura.config` untouched): `CONFIG_MTK_CLKBUF=y`,
`CONFIG_MTK_CCCI=y`, `CONFIG_MTK_CCCI_CLDMA=y`, `CONFIG_MTK_CCCI_UTIL=y`,
`CONFIG_MTK_BTIF=m`, `CONFIG_MTK_WCN_WMT=m` (the module/built-in split the coordinator
reported: WMT is `mtk_wcn_wmt.ko`, CCCI is built-in).

Build commands and results (`-j2`, `W=1`; full log kept at `$M2/logs/rem2-clkbuf-build.log`):

```
rm -f drivers/misc/mediatek/clkbuf/{mt_clkbuf_ctl.o,built-in.a} \
      drivers/misc/mediatek/ccci/ccci_rpc.o \
      drivers/misc/mediatek/connectivity/wmt/consys_mt6735.o \
      drivers/misc/mediatek/built-in.a

make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/clkbuf/mt_clkbuf_ctl.o            # rc 0
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/clkbuf/built-in.a                 # rc 0
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci/ccci_rpc.o \
                               drivers/misc/mediatek/connectivity/wmt/consys_mt6735.o   # rc 0
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/built-in.a                        # rc 0
```

Compiler diagnostics: **0 warnings, 0 errors**. The only warnings in the log are four
make-level messages emitted by `scripts/Makefile.build:541/462` when an archive that is also
a subdirectory target is requested explicitly (`overriding recipe for target
'drivers/misc/mediatek{,/clkbuf}/built-in.a'`); they are an artefact of the target form, not
of any source change. The `CC [M] consys_mt6735.o` / `LD [M] mtk_wcn_wmt.o` lines confirm the
module context that requires the `EXPORT_SYMBOL_GPL` API.

Config-directed compile check: with `./scripts/config -d MTK_CLKBUF` + `olddefconfig`,
`ccci_rpc.o` still compiles at `W=1` (rc 0) — the header's inline stubs and the
`IS_ENABLED(CONFIG_MTK_CLKBUF)` guard keep op `0x4008` on `FS_FUNC_FAIL`. The config was then
restored to `=y` and both objects rebuilt clean.

No userspace tool is part of this gap, so there is no `make check` target to run.
No full `vmlinux` build was run.

## 6. Hardware-unverified items and deliberate deviations

All runtime behaviour is hardware-unverified (no MT6735 device on this host). Static
confirmation: the ROM `clk_buf_ctrl@0xc03492d0` disassembly matches the ported flow and uses
the same `0x60c`/`0x610` SPM offsets (§2.2). Items that only a device can settle:
whether `SPM_BSI_START` clears within the 1 ms bound, the 3 ms/10x1 ms MDSRC timings, and the
actual effect of `CLK_BUF_CONN` on the CONN subsystem's RF clock.

Deliberate deviations from downstream, each intentional:

1. **No `BUG()` on a missing DT node** (downstream `mt_clkbuf_ctl.c:185-186`): the probe
   logs and fails; `clk_buf_ctrl()` then returns `false` and
   `clk_buf_get_swctrl_status()` returns the software shadow. Required by the contract.
2. **Bounded `SPM_BSI_START` poll**: downstream busy-waits forever (`mt_spm.c:943-944`);
   this port polls 1000 x 1 us and returns `-ETIMEDOUT`. The bound is `[INFERENCE]` — the
   vendor source gives no timeout value.
3. **regmap errors are propagated**: downstream ignores `spm_read`/`spm_write` results
   (platform accessors, no error path). Here a failed access makes `clk_buf_ctrl()` return
   `false` and log once, instead of pretending success.
4. **Failed `mediatek,clkbuf-config` read fails the probe**: downstream leaves `vals[]`
   uninitialised and derives the buffer modes from stack garbage
   (`mt_clkbuf_ctl.c:174-187`).
5. **The sysfs store takes the driver mutex**: downstream calls
   `spm_clk_buf_ctrl()`/`clk_buf_Send_BSI_CW()` without `clk_buf_ctrl_lock`
   (`mt_clkbuf_ctl.c:227-247`), racing `clk_buf_ctrl()`. The lock serialises both.
6. **`sysfs_emit_at()` instead of `sprintf()`** in show: same bytes, no overflow risk. The
   show path also prints the cached `mediatek,clkbuf-config` read at probe instead of
   re-reading the DT each time (identical bytes for a static DT; before a successful probe it
   prints zeroes).
7. **`clk_buf_get_swctrl_status()` is not locked**, matching downstream and keeping the
   frozen API callable from the CCCI work context.
8. **sysfs group is registered unconditionally at `late_initcall`** (as downstream does at
   `mt_pm_init.c:554`); if the probe failed, `store` answers `-ENODEV` and `show` prints the
   zero configuration.
9. **CCCI "not probed" answer**: downstream has no presence check; here a missing/unbound
   driver answers `FS_FUNC_FAIL` (with a log) instead of the downstream garbage-derived
   values.
10. **CONSYS tracks `clk_buf_on`** and releases the buffer on the power-on error path;
    downstream has neither the state nor the unwind (`mtk_wcn_consys_hw.c:353-358`,
    `:729-734`).
11. **`EXPORT_SYMBOL_GPL`** on the API: downstream linked everything into one image; here
    `mtk_wcn_wmt.ko` calls `clk_buf_ctrl()` while `CONFIG_MTK_CLKBUF` is built-in (bool,
    because `power_kobj` is not exported).
12. `mediatek,clkbuf-quantity` is present in the ROM DTB but, exactly as in downstream, is
    not read; `CLKBUF_NUM`/`CLKBUF_MAX_COUNT` (4) is the compile-time count.

## 7. DTS-FRAGMENT

Root level (the sysfs node is a root-node device, not part of `&{/soc}`), values from
`rom_dtb_0.dts:4602-4607`; the `mediatek,spm` property is **[INFERENCE]** — downstream used a
global SPM base, Nura needs a phandle to the existing `scpsys` syscon at 0x10006000
(`mt6735.dtsi:182-185`, ROM DTB `:307-311`).

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

No DTS file was edited by this leaf.

## 8. Patch

`$M/patches/rem2-clkbuf.patch` (28470 bytes, 8 files).

Generation note: in this worktree the parity baseline is applied to the working tree but not
committed, so a literal `git add -A && git diff --cached` would emit the whole baseline
(`drivers/misc/mediatek/` is untracked from `046883c0c`). To deliver a leaf-only delta the
patch was produced from staged tree objects:

```sh
git add -A && T_ALL=$(git write-tree)
# temp index = T_ALL, then for the four baseline files re-point the entries at the
# pristine baseline blobs (taken from the unchanged coordinator worktree copy) and drop the
# four new clkbuf files -> T_BASE
git diff --binary $T_BASE $T_ALL > $M/patches/rem2-clkbuf.patch
git apply -R --check $M/patches/rem2-clkbuf.patch     # rc 0
```

`git apply -R --check` succeeding proves the forward patch applies cleanly on top of the
unchanged baseline. Files in the patch: `drivers/misc/mediatek/{Kconfig,Makefile}`,
`drivers/misc/mediatek/ccci/ccci_rpc.c`,
`drivers/misc/mediatek/connectivity/wmt/consys_mt6735.c`, and the four new
`drivers/misc/mediatek/clkbuf/` files. No other worktree file is modified by this leaf and no
frozen header, DTS or `$M/nura.config` was touched.

Because the whole `drivers/misc/mediatek/` tree (and `include/linux/clkbuf.h`) is untracked
relative to `046883c0c`, `git status --porcelain` lists those paths as `A` once staged; the
authoritative "only my files" statement is the `T_BASE..T_ALL` file list above (8 files),
which excludes every other baseline path.
