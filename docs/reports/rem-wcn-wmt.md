# rem-wcn-wmt — WMT PSM / coredump / coexistence gap closure

Worktree: `$M/worktrees/rem-wcn-wmt` (HEAD `6a5726ccb baseline fixup: descend into
drivers/misc/mediatek` on top of `2d78ac00f baseline: parity-baseline-all.patch`).
Baseline patches were applied before this run; nothing was committed, reset or
rebased here. The two `baseline fixup:` commits (drivers/misc/Kconfig source +
drivers/misc/Makefile descent) are HEAD and are not part of the delivered patch.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Assigned driver and files changed

MediaTek WCN/CONSYS WMT core: STP power-saving engine (PSM), firmware
coredump/assert collection with the `STP_DBG` generic-netlink ABI, and the
BGW-desense / LTE-IDC coexistence engine.

| File | Change |
|---|---|
| `drivers/misc/mediatek/connectivity/wmt/stp_psm.c` | PSM state machine, hold FIFO, idle monitor, quick-PS, density sampling, EIRQ, WMT handshake, reset/reapply |
| `.../wmt/stp_psm.h` | PSM internal interface (frozen names mapped from `psm_core.c`) |
| `.../wmt/stp_dbg.c` | `STP_DBG` generic-netlink family (BIND/RESET, `STP_DBG_ATTR_MSG`), bind registry, bounded chunk push |
| `.../wmt/stp_btm.c` | assert capture, paged-dump collector over the CONSYS EMI window, `[M]` chunk delivery, timeout and teardown |
| `.../wmt/wmt_coex.c` | COEXDBG command path, BGW desense netlink daemon, LTE IDC parse/forward + modem-bridge registration |
| `.../wmt/stp_core.c` | PSM hooks in the real TX/RX paths; assert path now starts the coredump collector; stale header comment fixed |
| `.../wmt/stp_btif.c` | `stp_btif_dpidle_ctrl()` (was declared but undefined → link blocker) |
| `.../wmt/wmt_core.c` | `opfunc_pwr_sv`, `opfunc_cmd_test` (COEXDBG), `opfunc_bgw_ds`, `opfunc_idc_msg_handling`; BGW-desense transition hook on the real FUNC_ON/OFF ops |
| `.../wmt/wmt_lib.c` | coex init/deinit wiring, `wmt_lib_ps_irq_cb()`, PSM reapply after a successful chip reset |
| `.../wmt/consys_mt6735.c` | `wmt_plat_eirq_ctrl()`, BGF IRQ → PSM HOST_AWAKE, EMI control-block accessors (`wmt_plat_get_dump_info`/`set_host_dump_state`/`update_host_sync_num`/`emi_virt`, all previously declared but undefined) |
| `.../wmt/wmt_local.h` | private declarations for the above |
| `.../wmt/wmt_api.c` | `mtk_wcn_wmt_ic_info_get()` / `mtk_wcn_wmt_co_clock_flag_get()` exports (GPS dependency) |
| `.../wmt/Makefile` | `stp_psm.o`, `stp_dbg.o`, `stp_btm.o`, `wmt_coex.o` in `mtk_wcn_wmt-y` |
| `drivers/misc/mediatek/connectivity/include/mtk_wcn_stp.h` | PSM/coredump public API, `COEX_TASK_INDX` |
| `drivers/misc/mediatek/connectivity/include/mtk_wcn_wmt.h` | coex/IDC/BGW API, `mtk_wcn_wmt_ic_info_get()` / `mtk_wcn_wmt_co_clock_flag_get()` (GPS dependency) |
| `tools/mediatek/wcn/stp_dbg/` | host userspace daemon: real `STP_DBG` netlink client, `[M]` chunk parser, `make check` self-test |

## 2. ROM / downstream evidence

### PSM (downstream `core/psm_core.c`, ROM `stp_psm_*`)
- four states ACT/ACT_INACT/INACT/INACT_ACT and the flag indices:
  `core/include/psm_core.h:90-113`, `_stp_psm_set_state()` `psm_core.c:542-565`
  (raises `BLOCK_DATA_EN` on leaving ACT).
- hold FIFO `type|u32 len|data|0xbb 0xbb`, 8 KiB, reset when full:
  `psm_core.c:596-629`; release loop max 20 packets: `psm_core.c:636-676`.
- idle monitor and `_stp_psm_stp_is_idle()`: `psm_core.c:567-594,1141-1156`.
- WMT action state machine `_stp_psm_notify_wmt()`: `psm_core.c:1037-1135`.
- event completion `_stp_psm_notify_stp()`: `psm_core.c:901-1035`;
  event processing `_stp_psm_wait_wmt_event_wq()`: `psm_core.c:834-899`
  (deep idle + wake unlock on SLEEP_EN; release data + ACT on WAKEUP_EN;
  rollback to ACT/INACT; RESET_EN).
- forced wakeup `_stp_psm_do_wakeup()` (10 retries, monitor stop, queue drain):
  `psm_core.c:1234-1287`; enable/disable `psm_core.c:1289-1334`.
- TX filter (`stp_psm_thread_lock_aquire`, hold when `BLOCK_DATA_EN`,
  release pending data, `start_monitor`, density sampling, post-TX sleep):
  `core/stp_core.c:2428-2510`, `2610-2640`.
- RX density: `stp_core.c:2768-2772`; density window/threshold
  (`PSM_USE_COUNT_PACKAGE=0`): `psm_core.h:141-148`, `psm_core.c:1432-1470`.
- quick PS: `wmt_lib.c:791-797` (`g_quick_sleep_ctrl` + early-suspend state),
  `wmt_core.c:1928-1936` (`p->ic_ops->is_quick_sleep`),
  `wmt_ic_soc.c:1497-1500` (SOC returns true).
- EIRQ: `wmt_lib.c:698` arms / `:728` masks the BGF EINT;
  `wmt_plat_alps.c:225-239` IRQ callback, `:460-520` `wmt_plat_eirq_ctrl()`;
  PSM EIRQ → HOST_AWAKE `psm_core.c:901-910`.
- ctx save/restore around a dump: `stp_core.c:3300-3322`.
- WMT power-saving PDUs and handshake: `wmt_core.c:142-150`
  (`WMT_SLEEP_CMD/EVT`, `WMT_HOST_AWAKE_CMD/EVT`, `WMT_WAKEUP_EVT`),
  `opfunc_pwr_sv()` `wmt_core.c:1144-1340`.
- ROM symbols in `$M/zip/symbols.txt`: `stp_psm_set_state`,
  `stp_psm_do_wakeup`, `stp_psm_notify_wmt_wakeup`, `stp_psm_notify_wmt_sleep`,
  `stp_psm_start_monitor`, `stp_psm_is_disable`, `stp_psm_has_pending_data`,
  `stp_psm_hold_data`, `stp_psm_release_data`, `stp_psm_is_to_block_traffic`,
  `stp_psm_disable_by_tx_rx_density`, `_stp_psm_*`.

### Coredump / netlink (downstream `core/btm_core.c`, `linux/pri/stp_dbg.c`)
- family/ABI: `stp_dbg.c:83-160` (name `STP_DBG`, `STP_DBG_ATTR_MSG=1`,
  `STP_DBG_COMMAND_BIND=1`, `STP_DBG_COMMAND_RESET=2`,
  `MAX_BIND_PROCESS=4`), `stp_dbg_nl_bind()` `:1240-1266`,
  `stp_dbg_nl_reset()` `:1267-1272`, `stp_dbg_nl_send()` `:1274-1320`.
- chunk framing `'[' 'M' ']' | u16 len | data`, 1024-byte chunks, command 2:
  `btm_core.c:186-242`.
- paged-dump protocol (host/chip sync states, page address/length, page count,
  end flag, host sync counter): `btm_core.c:480-700`;
  control-block offsets `mt6735/include/mtk_wcn_consys_hw.h:218-233`.
- firmware assert path: `stp_core.c:2035-2180` (FW_MSG, ctx save, BTM notify,
  coredump flag); `mtk_wcn_stp_coredump_start_ctrl/flag` `stp_core.c:2342-2375`.
- ROM symbols: `stp_dbg_nl_bind`, `stp_dbg_nl_reset`, `stp_dbg_nl_send`,
  `stp_dbg_dmp_out`, `stp_dbg_pkt_log`, `wmt_core_set_coredump_state`,
  `wmt_ctrl_set_stp_dbg_info`, `stp_btm_*`, `wmt_dev_bgw_desense_init/deinit`.

### Coexistence (downstream `wmt_core.c`, `wmt_idc.c`, `bgw_desense.c`, `wmt_func.c`)
- COEXDBG command `01 10 02 00 08 <idx>` with payload size by index, and the
  index range 0x0..0xf: `wmt_core.c:1545-1695`.
- BGW desense forward `01 0e 0f 00 02 <14 B>` / event `02 0e 01 00 00`:
  `wmt_core.c:1962-2025`; daemon netlink protocol 17, `BGW` bind + ACK, ON=1,
  OFF=0, ACK=2: `linux/include/bgw_desense.h:22-59`,
  `linux/pub/bgw_desense.c:24-145`; control from `wmt_ctrl.c:899-910` and
  transitions in `wmt_func.c:302-340,510-545,668-710`.
- LTE IDC TX frame `01 10 <u16 len> 00 <opcode> <flag+payload>` and RX parse
  (`dir|opcode`, `u16 len`, `len-1`, flag/opcode byte, payload, `+5` per frame):
  `wmt_core.c:2170-2240`, `wmt_idc.c:118-200`; opcode tables
  `linux/include/wmt_idc.h:60-90`.
- ROM symbols: `wmt_ctrl_bgw_desense_ctrl`, `wmt_lib_idc_lock_aquire/release`,
  `wmt_dev_bgw_desense_init/deinit`.

## 3. Parity matrix

| # | Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|---|
| 1 | PSM active/sleep 4-state machine, per-state request handling | `psm_core.c:1037-1135` | partial (state struct + partial notify) | `psm_notify_wmt()` maps ACT/INACT exactly; `psm_notify_action_done()` sets event bits only in ACT_INACT/INACT_ACT; event handler converges | done |
| 2 | WMT SLEEP/WAKEUP/HOST_AWAKE handshake through the real WMT op thread | `wmt_core.c:1144-1340`, `wmt_lib.c:646-797` | `wmt_core_psm_action()` added; failure handling only SLEEP/other | full per-action PDU exchange, failure maps to downstream assert reasons 33/34/35, ROLLBACK on failure | done |
| 3 | AP-sleep interaction: wake lock, BTIF deep idle, EIRQ arm/mask | `psm_core.c:867-890`, `wmt_lib.c:698/728`, `wmt_plat_alps.c:460-520` | `stp_btif_dpidle_ctrl()` declared only; EIRQ absent | implemented both sides; BGF IRQ masks itself and queues HOST_AWAKE; PSM arms before sleep/masks on wake/reset/unload | done |
| 4 | TX blocking `stp_psm_tx_begin/end`, hold/release, monitor re-arm | `stp_core.c:2428-2510,2610-2640` | hooks present, monitor armed in quick mode too | downstream ordering; quick mode does not re-arm monitor, queues sleep before unlock | done |
| 5 | `stp_psm_rx_done` density accounting | `stp_core.c:2768-2772`, `psm_core.c:1432-1470` | present, started monitor on every sample | 1 s window / 50 000 byte threshold, 30 ms ↔ 1000 ms idle, monitor restart only on high density | done |
| 6 | Timeout / retry (`_stp_psm_do_wait`, 10 × handshake) | `psm_core.c:1208-1287` | 10 × 20 ms sleep loop, no error propagation | `stp_psm_do_wakeup()` public, propagates `-EIO`, enable/disable return the real result (`-ETIMEDOUT`/WMT error) | done |
| 7 | Quick-PS | `wmt_lib.c:791-797`, `wmt_ic_soc.c:1497` | SOC flag + PM suspend, but monitor handling wrong | gated on SOC capability + `PM_SUSPEND_PREPARE`; post-TX sleep; monitor skipped | done (suspend trigger `[INFERENCE]`) |
| 8 | PSM reset/reapply across chip reset | `psm_core.c:769-820`, `stp_core.c:2280-2290` | reset added, reapply dead code | reset masks EIRQ, leaves BTIF deep idle, restores sleep-enable; `stp_psm_reapply()` called after `wmt_lib_cmb_rst()` success | done |
| 9 | Firmware assert → coredump, TX blocked | `stp_core.c:2035-2180,3300-3322` | FW_MSG only warned and reset | sets `f_coredump`, `psm_set_sleep_disable()`, collects, resets after (or immediately when collection disabled/unavailable) | done |
| 10 | `STP_DBG` netlink family, BIND/RESET, 4 binds, safe teardown | `stp_dbg.c:83-160,1240-1320` | family existed; bind/send unsynchronised | mutex-protected bind registry/send, real errno on overflow, unregister on teardown, RESET is kernel→user only | done |
| 11 | EMI paged dump → `[M]` chunks to bound daemon | `btm_core.c:189-242,480-700` | collector present but platform accessors undefined | bounded page copy (address/length validated against the 343 KiB map), 1024-byte `[M]` records, assert text pushed first, 5 min timeout | done |
| 12 | COEXDBG command path | `wmt_core.c:1545-1695` | fine | index range/expected payload size per index, errno on bad input | done |
| 13 | BGW desense: daemon ON/OFF at GPS∧(BT∨WiFi) transitions, 14-byte forward | `bgw_desense.c:24-145`, `wmt_ctrl.c:899-910`, `wmt_func.c:302-710` | socket existed but `wmt_coex_init()` never called, no transitions | init wired into `wmt_lib_init()`, invariant-driven ON/OFF on FUNC_ON/OFF, `mtk_wcn_wmt_bgw_ds_send()` forwards via `opfunc_bgw_ds` | done |
| 14 | LTE IDC RX parse/forward, modem bridge, `-ENODEV` without bridge | `wmt_idc.c:118-200`, `wmt_core.c:2170-2240` | parse present, registration unlocked | coex-ready/serialised registration, COEX channel only enabled with a bridge, both directions `-ENODEV` otherwise, no fabricated data | done |
| 15 | GPS additive exports preserved | `wmt_lib.c:239-253`, `wmt_exp.c:424-429` | absent from baseline header (GPS build blocker) | `u32 mtk_wcn_wmt_ic_info_get(enum mtk_wcn_wmt_icinfo)` / `bool mtk_wcn_wmt_co_clock_flag_get(void)` + `EXPORT_SYMBOL_GPL` | done |
| 16 | Userspace coredump daemon | ABI above | absent | `tools/mediatek/wcn/stp_dbg/` real netlink client + streaming parser + self-test | done |

### PSM state transition table (cross-checked against ROM `stp_psm_*` / `psm_core.c`)

| State | Request/event | WMT exchange | Next state | Side effects |
|---|---|---|---|---|
| ACT | SLEEP (monitor enabled) | `SLEEP cmd 01 03 01 00 01` → `02 03 02 00 00 01` | ACT_INACT, then INACT on ack; ACT on rollback | release hold FIFO first; on INACT: `eirq on`, BTIF dpidle, wake unlock |
| ACT | SLEEP (monitor disabled) | none | ACT | request dropped (downstream error path) |
| ACT | WAKEUP / HOST_AWAKE | none | ACT | release hold FIFO |
| ACT | EIRQ | none | ACT | (HOST_AWAKE request handled as above) |
| INACT | WAKEUP / HOST_AWAKE / EIRQ | `WAKEUP` = BTIF wake + `02 03 02 00 00 03`; `HOST_AWAKE cmd 01 03 01 00 02` → `…00 02` | INACT_ACT, then ACT on ack; INACT on rollback | wake lock, `eirq off`, BTIF dpidle off |
| INACT | SLEEP | none | INACT | already asleep |
| ACT_INACT / INACT_ACT | any PSM request | — | unchanged | a handshake is in flight; next request re-runs after the event |
| any | reset / link down | — | ACT, monitor disabled | EIRQ masked, BTIF dpidle off, hold FIFO dropped, sleep-enable restored |
| any | firmware assert | — | ACT (TX blocked) | `f_coredump=1`, sleep disabled, collector runs, whole-chip reset at the end |

## 4. Compile evidence

Environment: `PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, `ARCH=arm64 LLVM=1`.

Config (as in `WCN_GAP_CONTRACT.md` §4, plus the two baseline fixups already in HEAD):

```sh
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e RFKILL -e CRC32 -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
```

Module (`CONFIG_MTK_WCN_WMT=m`), all objects + `LD [M]`, `W=1`:

```sh
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  drivers/misc/mediatek/connectivity/wmt/
```

Result (full forced rebuild of every source in the directory):

```
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_main.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/stp_core.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/stp_btif.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_core.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_ctrl.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_func.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_lib.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_conf.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_ic_soc.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/consys_mt6735.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/stp_psm.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/stp_dbg.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/stp_btm.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_coex.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_dev.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_chrdev_wifi.o
  CC [M]  drivers/misc/mediatek/connectivity/wmt/wmt_api.o
  LD [M]  drivers/misc/mediatek/connectivity/wmt/mtk_wcn_wmt.o
```

No warnings (W=1 clean). No unresolved symbols at `LD [M]`; every `wmt_plat_*`
helper the new code calls is now defined in `consys_mt6735.c` /
`stp_btif.c`.

Built-in (`CONFIG_MTK_WCN_WMT=y`; `MTK_BTIF` must also be `y`, the tristate
dependency forbids `MTK_WCN_WMT=y` with `MTK_BTIF=m`):

```
CONFIG_MTK_BTIF=y
CONFIG_MTK_WCN_WMT=y
  CC      .../wmt/wmt_main.o … .../wmt/wmt_api.o      (all 17 objects)
  AR      drivers/misc/mediatek/connectivity/wmt/built-in.a
```

No warnings, and no `LD [M]` is attempted for a built-in (as expected).

Userspace (`make -C tools/mediatek/wcn/stp_dbg check`):

```
cc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -c -o stp_dbg.o stp_dbg.c
cc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -c -o stp_dbg_parse.o stp_dbg_parse.c
cc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -c -o selftest.o selftest.c
cc -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L  -o stp_dbg stp_dbg.o stp_dbg_parse.o selftest.o
./stp_dbg --selftest
selftest: single frame ok
selftest: two frames ok
selftest: split frame ok
selftest: max chunk byte-at-a-time ok
selftest: bad magic ok
selftest: oversized length ok
selftest: truncated stream ok
selftest: zero-length chunk ok
selftest: many small frames ok
selftest: callback abort ok
selftest: all 10 checks passed
```

Host smoke test of the real client path (kernel here has no `STP_DBG` family,
so the family lookup must fail cleanly):

```
$ ./stp_dbg -t 100
stp_dbg: family "STP_DBG" not available: No such file or directory
$ echo $?
1
```

Build artifacts (`*.o`, the `stp_dbg` binary, `.config`, `include/config/`)
were removed/not tracked before the patch was generated.

## 5. Inferred or hardware-unverified

Everything below is either `[INFERENCE]` or only provable on a device; no value
was invented.

- `[INFERENCE]` Quick-PS trigger: downstream keys quick sleep off the
  early-suspend/fb-blank state (`wmt_dev_get_early_suspend_state()`); this port
  uses `PM_SUSPEND_PREPARE`/`PM_POST_SUSPEND` because the WMT driver has no fb
  client. Consequence: quick PS is active only for whole-system suspend, not
  screen-off. The normal idle monitor still sleeps the MCU in both cases.
- `[INFERENCE]` EIRQ ordering: the wakeup line is armed when the SLEEP ack is
  processed (downstream enables it after `wmt_lib_ps_do_sleep()` returns and
  before the PSM thread consumes the event; the two are equivalent because the
  PSM event is already set inside the WMT op). Masking happens on the
  WAKEUP/HOST_AWAKE path, on reset and on unload; the IRQ handler additionally
  masks itself because the line is level-triggered.
- `[INFERENCE]` A failed WMT power-saving exchange is reported to PSM as
  ROLLBACK (upstream `opfunc_pwr_sv()` calls its PSM callback even on failure,
  relying on the whole-chip assert recovery). The assert request with reasons
  33/34/35 is still issued exactly as downstream.
- `[INFERENCE]` `wmt_core_bgw_desense_update()` sends ON/OFF from the invariant
  "GPS ∧ (BT ∨ WiFi)" at each FUNC_ON/OFF transition instead of repeating the
  downstream per-function `if` conditions; the emitted message sequence is the
  same (duplicates included) but this is not byte-for-byte the downstream call
  graph.
- `[INFERENCE]` PSM request-result propagation (`-ETIMEDOUT` from the request
  wait, WMT error from the enable/disable op) — downstream APIs also return the
  op result, but the 5 s wait bound is this port's choice.
- Hardware-only unknowns, not verifiable without a phone: the actual CONSYS
  page-dump register behaviour (offsets come from
  `mt6735/include/mtk_wcn_consys_hw.h:218-233`), the BGF IRQ trigger type
  supplied by DT, EMI remap values, the firmware page count, and whether the
  ROM firmware accepts the same SLEEP/WAKEUP/HOST_AWAKE PDUs through BTIF —
  the PDUs themselves are copied from `wmt_core.c:142-150` and the ROM symbol
  list, so they are not guesses.
- The page-dump collector's 5-minute timeout, 10×10 ms state polling,
  100 ms settle time and default 35 pages are copied from
  `btm_core.c:480-700` / `stp_dbg.h:127`; their interaction with the ROM
  firmware is unverified.
- No `dev_coredump` is used: the WMT core has no `struct device` of its own and
  downstream also delivered through its own userspace ABI. The netlink daemon
  is the only sink; AEE/Ramoops consumers are out of scope.

## 6. DTS-FRAGMENT

None. The change uses only resources the existing CONSYS platform node already
provides (`interrupts` for the BTIF wakeup line, the `memory-region` reserved
carve-out for the EMI window, regulators and clocks). No shared `.dts`/`.dtsi`
was modified.

## 7. Patch

`$M/patches/rem-wcn-wmt.patch` (20 files, 4674 insertions / 30 deletions,
`grep -c 'GIT binary patch'` = 0, no `.o`/`.ko`/`.cmd`/`modules.order`/
`Module.symvers`/`.mod.c`/`.config`/`include/config/` hunks; the two baseline
fixup commits are in HEAD and not part of the diff).

Also note for integration: this patch supplies
`mtk_wcn_wmt_ic_info_get()` / `mtk_wcn_wmt_co_clock_flag_get()` and the
`WMT_ICINFO_*` enumerators that the GPS leaf's `gps/stp_chrdev_gps.c` needs, so
it must be applied before/with `wcn-gap-gps.patch` as the coordinator's order
already states.
