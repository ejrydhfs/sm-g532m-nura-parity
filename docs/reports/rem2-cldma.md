# rem2-cldma — exception-time CLDMA stop and the DHL RX window

Path legend used throughout (constants from `rem2-contract.md`):

* `$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
* `$M2` = `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work`
* `DS/` = `$M/src/downstream-grandpplte/drivers/misc/mediatek/eccci/`
* `NURA/` = `$M2/worktrees/rem2-cldma/drivers/misc/mediatek/` (files named `ccci/x.c`
  or `ccci_cldma/x.c`)
* `ROM` = `$M/zip/vmlinux.elf` (ARM32, symbols recovered)

## 1. Gap, files changed, worktree

Worktree `$M2/worktrees/rem2-cldma`, detached HEAD `046883c0c` (`arm64: dts: mediatek: Add
support for Doogee X5 Pro`) with `$M/patches/parity-baseline-all.patch` applied and the
coordinator-owned frozen headers (`include/linux/aee.h`, `include/linux/ccci.h`) in place.
Nothing was committed, reset, checked out or stashed; no other worktree was touched; no DTS was
edited.

Gap: the Nura CLDMA engine had no exception-time stop primitive. `ccci_ccif_exception()`
called the *full* engine stop at `HIF_EX_INIT` and carried a comment saying the downstream
primitive (`cldma_stop_for_ee()`) was missing, which made the two-second DHL window empty. The
frozen API declared `ccci_cldma_stop_for_ee()` and `ccci_cldma_dhl_rx_alive()` but nothing
implemented or called them.

```
 drivers/misc/mediatek/ccci/ccci_ccif.c        |  68 +++++---
 drivers/misc/mediatek/ccci_cldma/ccci_cldma.c | 214 ++++++++++++++++++++++++++
 drivers/misc/mediatek/ccci_cldma/ccci_cldma.h |  42 +++++
 3 files changed, 306 insertions(+), 18 deletions(-)
```

Result: both frozen functions are implemented from the downstream sequence (confirmed by the
inlined register writes in the ROM), exported with `EXPORT_SYMBOL_GPL`, and the exception flow
now runs `stop_for_ee` → `H2D_EXCEPTION_ACK` at `HIF_EX_INIT` and
`msleep(2000)` → `dhl_rx_alive(false)` → full stop → dump → `H2D_EXCEPTION_CLEARQ_ACK` at
`HIF_EX_CLEARQ_DONE`. Both touched objects and both directory archives build with `W=1` and no
new warning.

## 2. Evidence

### 2.1 Downstream source

| What | Reference |
|---|---|
| DHL / non-stop queue mask `0xF0`, 32-bit form `0xF0F0F0F0` | `DS/modem_cldma.c:113-114` |
| `cldma_stop_for_ee()`: TX stop loop, RX stop loop, latch clear, mask | `DS/modem_cldma.c:1449-1520` |
| TX partial: `txq_active &= ~ALL_QUEUE`, `UL_STOP_CMD = 0xFF`, dummy read, `UL_STATUS` | `DS/modem_cldma.c:1456-1479` |
| dump every 100000 polls (`(++count) % 100000`), AEE at `count >= 1600000` | `DS/modem_cldma.c:1464-1476` (TX), `1489-1501` (RX) |
| RX partial: `rxq_active &= ~(ALL_QUEUE & NONSTOP_QUEUE_MASK)`, `SO_STOP_CMD = 0xF0`, dummy read, `SO_STATUS & 0xF0` | `DS/modem_cldma.c:1481-1501` |
| latch clear: `L2TISAR0/1` = ALL, `L2RISAR0/1` = `ALL & 0xF0F0F0F0`, `L3TISAR0/1` = ALL, `L3RISAR0/1` = `ALL & 0xF0F0F0F0` | `DS/modem_cldma.c:1503-1511` |
| mask: `L2TIMSR0/1` = ALL, `L2RIMSR0` = `(DONE|ERROR) & 0xF0F0F0F0`, `L2RIMSR1` = ALL, `L3TIMSR0/1` = ALL, `L3RIMSR0/1` = `ALL & 0xF0F0F0F0` | `DS/modem_cldma.c:1512-1521` |
| per-queue RX stop (mask `L2RIMSR0` with the queue bit, clear `rxq_active` bit, `SO_STOP_CMD` = bit, dummy read, poll `SO_STATUS & bit`) | `DS/modem_cldma.c:1694-1730` |
| per-queue RX start (`SO_START_CMD` = bit, `L2RIMCR0` = bit, dummy read, set `rxq_active` bit) | `DS/modem_cldma.c:1746-1770` |
| full stop clears/masks with `CLDMA_BM_INT_ALL` | `DS/modem_cldma.c:1417-1435` (latches), `1426-1434` (`L2RIMSR0`), plus `1400-1420` |
| `md_cd_ccif_delayed_work()`: `cldma_stop()`, flush TX/RX/refill work, `H2D_EXCEPTION_CLEARQ_ACK` | `DS/modem_cldma.c:1966-1983` |
| `HIF_EX_INIT`: notify `EX_INIT` → `cldma_stop_for_ee()` → `md_cd_clear_all_queue(md, OUT)` → `H2D_EXCEPTION_ACK` | `DS/modem_cldma.c:1999-2010` |
| `HIF_EX_INIT_DONE`: notify `EX_DHL_DL_RDY` | `DS/modem_cldma.c:2011-2013` |
| `HIF_EX_CLEARQ_DONE`: `msleep(2000)`, `md_cd_ccif_delayed_work()` | `DS/modem_cldma.c:2014-2017` |
| `HIF_EX_ALLQ_RESET`: `cldma_reset()`, notify, `cldma_start()`, re-check `SO_CFG` bit 0 | `DS/modem_cldma.c:2018-2035` |
| queue *q* is bit *q* in `L2RISAR0`/`L2RIMSR0` (`BIT(index)`), STATUS bitmask semantics | `DS/modem_cldma.c:1296-1312`, `1714-1718` |
| MT6735 CLDMA clock-source lock is a no-op (`spm_ap_mdsrc_req` commented out) | `DS/mt6735/cldma_platform.c:176-179` |

Queue-mask direction. `NONSTOP_QUEUE_MASK` names the set that is **stopped**, not the set that
is kept: `modem_cldma.c:1481` clears bits `0xF0` (queues 4..7) from `rxq_active` and
`modem_cldma.c:1484` writes `SO_STOP_CMD = 0xF0`, while the comment at `modem_cldma.c:113`
says "queue 0,1,2,3 are non-stop". So RX queues **0..3 (0x0F)** stay armed and unmasked, and
the DHL payload is delivered over them. That is independently corroborated by the Nura port
table (ported from downstream `port_cfg.c`): the log-control port uses `.rxq_exp = 3`
(`NURA/ccci/ccci_core.c:1283-1285`), `ccci_fs` `.rxq_exp = 1` (`1289-1291`), `ttyC1`
`.rxq_exp = 2` (`1298-1300`), and every network port has `.rxq_exp = 0xFF`
(`1251-1273`). See §6 for the naming caveat in the frozen header.

### 2.2 ROM (`$M/zip/vmlinux.elf`)

Symbols (`llvm-nm`): `md_cd_ccif_work 0xc052843c`, `md_cd_ccif_delayed_work 0xc0527c80`,
`md_cd_ee_callback 0xc052d648`, `md_cd_clear_all_queue 0xc0520484`,
`md_cd_ccif_send 0xc051ba50`, `md_cd_dump_debug_register 0xc0531320`,
`cldma_dump_register 0xc05320d4`, `aed_md_exception_api 0xc0628298`. `$M/zip/symbols.txt` has
no `cldma_stop_for_ee` (it was inlined), and `md_cd_ee_callback` in *this* ROM build only
toggles the watchdog (`0xc052d654-0xc052d680`: `tst r1,#1` → `wdt_enable_irq`/`wdt_disable_irq`),
so the exception sequence is fully inlined in the ccif worker. Windows below are from
`llvm-objdump -d --triple=armv7a-none-eabi`; the `md_cd_ctrl` field offsets visible in the code
are PD base `+0x1634`, AO base `+0x162c`, `txq_active` (`u16`) `+0x1108`, `rxq_active` (`u16`)
`+0x110a`.

`md_cd_ccif_work` — `HIF_EX_INIT` inlined `cldma_stop_for_ee()` (window `0xc0528558-0xc0528928`):

```
c0528574: e3c220ff  bic  r2, r2, #255        ; txq_active &= ~0xFF   (r3 = 0x1108)
c052857c: e18420b3  strh r2, [r4, r3]
c05285d0: e3a020ff  mov  r2, #255
c05285d8: e583205c  str  r2, [r3, #0x5c]     ; CLDMA_AP_UL_STOP_CMD = 0xFF (PD)
c05285e8: e593205c  ldr  r2, [r3, #0x5c]     ; dummy read of the same register
c05285f8: e5938050  ldr  r8, [r3, #0x50]     ; CLDMA_AP_UL_STATUS
c0528604: e30836a0  movw r3, #0x86a0
c0528608: e3403001  movt r3, #0x1           ; 100000
c0528618: e0636293  mls  r3, r3, r2, r6      ; count % 100000
c0528550: e306a9ff  movw r10, #0x69ff
c0528568: e340a018  movt r10, #0x18         ; 1599999
c05285bc: e156000a  cmp  r6, r10
c0528678: (count > 1599999) ... c0528698: bl 0xc0628298 <aed_md_exception_api>
c05286b8: e3c220f0  bic  r2, r2, #240       ; rxq_active &= ~0xF0  (r3 = 0x110a)
c05286c4: e18420b3  strh r2, [r4, r3]
c0528718: e3a020f0  mov  r2, #240
c0528720: e58323c4  str  r2, [r3, #0x3c4]    ; CLDMA_AP_SO_STOP_CMD = 0xF0 (PD)
c0528730: e59323c4  ldr  r2, [r3, #0x3c4]    ; dummy read
c0528740: e593cab8  ldr  r12, [r3, #0xab8]   ; CLDMA_AP_SO_STATUS (AO)
c0528754: e20c80f0  and  r8, r12, #240       ; status & 0xF0
c05287c4..c05287e4: (count > 1599999) -> aed_md_exception_api  (RX message)
c05287f4: e5832410  str  r2, [r3, #0x410]    ; L2TISAR0 = 0xFFFFFFFF
c0528804: e5832414  str  r2, [r3, #0x414]    ; L2TISAR1 = 0xFFFFFFFF
c0528810: e30f30f0  movw r3, #0xf0f0
c0528818: e7df3813  bfi  r3, r3, #16, #16   ; 0xF0F0F0F0
c052881c: e5823450  str  r3, [r2, #0x450]    ; L2RISAR0 = 0xF0F0F0F0
c0528834: e5823454  str  r3, [r2, #0x454]    ; L2RISAR1 = 0xF0F0F0F0
c0528848: e5832430  str  r2, [r3, #0x430]    ; L3TISAR0 = 0xFFFFFFFF
c0528858: e5832434  str  r2, [r3, #0x434]    ; L3TISAR1 = 0xFFFFFFFF
c0528870: e5823470  str  r3, [r2, #0x470]    ; L3RISAR0 = 0xF0F0F0F0
c0528888: e5823474  str  r3, [r2, #0x474]    ; L3RISAR1 = 0xF0F0F0F0
c052889c: e5832428  str  r2, [r3, #0x428]    ; L2TIMSR0 = 0xFFFFFFFF
c05288ac: e583242c  str  r2, [r3, #0x42c]    ; L2TIMSR1 = 0xFFFFFFFF
c05288b8: e3a030f0  mov  r3, #240
c05288c0: e7df3813  bfi  r3, r3, #16, #16   ; (DONE|ERROR) & 0xF0F0F0F0 = 0x00F000F0
c05288c4: e5823c68  str  r3, [r2, #0xc68]    ; L2RIMSR0 = 0x00F000F0 (AO)
c05288d8: e5832c6c  str  r2, [r3, #0xc6c]    ; L2RIMSR1 = 0xFFFFFFFF
c05288e8: e5832448  str  r2, [r3, #0x448]    ; L3TIMSR0 = 0xFFFFFFFF
c05288f8: e583244c  str  r2, [r3, #0x44c]    ; L3TIMSR1 = 0xFFFFFFFF
c0528910: e5823488  str  r3, [r2, #0x488]    ; L3RIMSR0 = 0xF0F0F0F0
c0528928: e582348c  str  r3, [r2, #0x48c]    ; L3RIMSR1 = 0xF0F0F0F0
c0528938: bl _raw_spin_unlock_irqrestore
c0528940: bl 0xc0520484 <md_cd_clear_all_queue>   ; r1 = 1 (OUT)
c0528950: bl 0xc051ba50 <md_cd_ccif_send>          ; r1 = 1 = H2D_EXCEPTION_ACK
```

Follow-on stages in the same function: `0xc052896c polling_ready(md, 2)` (D2H_EXCEPTION_INIT_DONE),
`0xc05289b0 ccci_md_exception_notify(md, 2)` (`EX_DHL_DL_RDY`), `0xc05289d0 polling_ready(md, 3)`,
`0xc0528a0c msleep(2000)`, `0xc0528a14 bl md_cd_ccif_delayed_work`, `0xc0528a34 polling_ready(md, 4)`,
then the restart. The `SO_CFG` re-check after the restart is at `0xc0528d04-0xc0528d1c`
(`ldr r2, [r3, #0xa04]`, `tst r3, #1`, `beq` to the "Enable AP OUTCLDMA failed" path).

`md_cd_ccif_delayed_work` (`0xc0527c80`) — inlined full `cldma_stop()` (window
`0xc0527d80-0xc052809c`): `UL_STOP_CMD = 0xFF` + dummy read (`0xc0527d8c`, `0xc0527d9c`),
`rxq_active &= ~0xFF` (`0xc0527e68`/`0xc0527e70`, field `+0x110a`), `SO_STOP_CMD = 0xFF` +
dummy read (`0xc0527e9c`, `0xc0527eac`), unmasked `SO_STATUS` (`0xc0527ebc`), then every latch
`= 0xFFFFFFFF` (`0xc0527fd0`…`0xc0528034`) and `L2TIMSR0/1`, `L2RIMSR0/1`, `L3TIMSR0/1`,
`L3RIMSR0/1` all `= 0xFFFFFFFF` (`0xc0528050`…`0xc052809c`).

No vendor userspace ELF was needed for this gap.

## 3. Implementation

### `NURA/ccci_cldma/ccci_cldma.h` (+42)

New private constants, all derived from cited sources:

* `CLDMA_NONSTOP_QUEUE_MASK 0xF0` / `CLDMA_NONSTOP_QUEUE_MASK_32 0xF0F0F0F0`
  (`DS/modem_cldma.c:113-114`).
* `CLDMA_EE_ALIVE_RXQ_MASK = CLDMA_BM_ALL_QUEUE & ~CLDMA_NONSTOP_QUEUE_MASK` = `0x0F`: the
  RX queues the EE stop leaves armed (queues 0..3), justified by `DS/modem_cldma.c:1481-1487`
  and the port table in §2.1.
* `CLDMA_EE_ALIVE_RXQ_INT = (DONE & 0x0F) | (ERROR & (0x0F << 16))` = `0x000F000F`: the same
  queues' DONE/ERROR positions in the L2 RX status/ready/mask registers (queue *q* = bit *q*
  for DONE, bit 16+*q* for ERROR; `DS/modem_cldma.c:1296-1312`). It is the 0..3 counterpart of
  downstream's `0xF0F0F0F0` for queues 4..7.
* `CLDMA_EE_STOP_DUMP_INTERVAL 100000`, `CLDMA_EE_STOP_POLL_MAX 1600000`
  (`DS/modem_cldma.c:1464-1476,1489-1501`; ROM `0xc0528604-0xc0528618`, `0xc0528550-0xc05285bc`).

### `NURA/ccci_cldma/ccci_cldma.c` (+214)

`static int cldma_ee_stop_poll(cd, cmd_base, cmd_off, status_base, status_off, mask, what)`
(line 1372): the shared downstream stop loop — `write32(cmd)`, `read32(cmd)` dummy read,
`read32(status) & mask`, repeat while non-zero; progress dump (`dev_info` + the existing
`cldma_dump_register()`) every 100000 iterations; `-ETIMEDOUT` when the masked status is still
non-zero at 1600000 polls. Masking the status read with `mask` means only the queues we asked
to stop are waited for, exactly as in the ROM (`and r8, r12, #240`).

`int ccci_cldma_stop_for_ee(struct ccci_cldma *cd)` (line 1413, `EXPORT_SYMBOL_GPL` at 1484):

1. `mutex_lock(&cd->start_lock)` — serialises against `ccci_cldma_start()`/`_stop()` and the
   ring map; `timeout_lock` is then only taken around the active-mask updates and the
   latch/mask programming, never across the poll (the poll may run 1.6M MMIO iterations, so no
   sleeping and no long IRQ-off window; downstream holds its spinlock across the whole loop).
2. `cd->txq_active &= ~CLDMA_BM_ALL_QUEUE` under `timeout_lock`, then TX poll:
   `UL_STOP_CMD = 0xFF` on `ap_pdn_base`, dummy read, `UL_STATUS = 0` expected.
3. `cd->rxq_active &= ~(CLDMA_BM_ALL_QUEUE & CLDMA_NONSTOP_QUEUE_MASK)`, then RX poll:
   `SO_STOP_CMD = 0xF0` on `ap_pdn_base`, dummy read, `SO_STATUS & 0xF0 = 0` expected on
   `ap_base`.
4. Latch clear: `L2TISAR0/1 = 0xFFFFFFFF`, `L2RISAR0/1 = 0xF0F0F0F0`, `L3TISAR0/1 =
   0xFFFFFFFF`, `L3RISAR0/1 = 0xF0F0F0F0`.
5. Mask: `L2TIMSR0/1 = 0xFFFFFFFF`, `L2RIMSR0 = 0x00F000F0`
   (`(DONE|ERROR) & 0xF0F0F0F0`), `L2RIMSR1 = 0xFFFFFFFF`, `L3TIMSR0/1 = 0xFFFFFFFF`,
   `L3RIMSR0/1 = 0xF0F0F0F0`. The DONE|ERROR bits of queues 0..3 are untouched in
   `L2RIMSR0`, i.e. the DHL queues stay unmasked.
6. Returns 0, `-EINVAL` on a bad `cd`, or `-ETIMEDOUT` (TX result takes precedence over RX). The
   latch/mask programming is applied even on timeout, as downstream does after its AEE call.
   `cd->started` is deliberately left `true` (the engine must be re-armed by
   `ccci_cldma_start()` at `HIF_EX_ALLQ_RESET`), and no INFRA/RGU reset is performed.

`int ccci_cldma_dhl_rx_alive(struct ccci_cldma *cd, bool alive)` (line 1508,
`EXPORT_SYMBOL_GPL` at 1562):

* `alive == true` opens the window (per-queue sequence of `DS/modem_cldma.c:1746-1770`
  generalised to the 0..3 set): set the `rxq_active` bits, `SO_START_CMD = 0x0F` + dummy read,
  clear the stale `L2RISAR0` bits `0x000F000F`, then unmask them through `L2RIMCR0`.
* `alive == false` closes it (per-queue sequence of `DS/modem_cldma.c:1694-1730` plus the latch
  handling of the full stop): clear the `rxq_active` bits, mask `L2RIMSR0` with `0x000F000F`,
  `SO_STOP_CMD = 0x0F` + dummy read + poll `SO_STATUS & 0x0F` through `cldma_ee_stop_poll()`,
  then clear `L2RISAR0` bits `0x000F000F` and clear/mask `L3RISAR0/1`, `L3RIMSR0/1` with
  `CLDMA_BM_INT_ALL` (L3 has no per-queue attribution in any source; the full stop uses these
  values, `DS/modem_cldma.c:1417-1435`, and the close is always followed by the full stop).
* Returns 0, `-EINVAL`, or `-ETIMEDOUT` from the close poll.

Also `#include <linux/export.h>` (line 31) for the two exports. No prototype is duplicated
outside the frozen `include/linux/ccci.h`.

### `NURA/ccci/ccci_ccif.c` (modified)

* `#include <linux/aee.h>` (line 25) and two locals (`char detail[96]`, `int err`) in
  `ccci_ccif_exception()`.
* `HIF_EX_INIT` (371): after `ccci_md_set_state(md, CCCI_MD_EXCEPTION)` (which latches the
  exception ring map, `NURA/ccci/ccci_core.c:139-148`) it calls
  `ccci_cldma_stop_for_ee(md->cldma)` instead of `ccci_cldma_stop()`. On `-ETIMEDOUT` it logs
  `stop CLDMA for EE failed: %d` and reports through
  `aed_md_exception_api(NULL, 0, NULL, 0, "mdN:\nUNKNOWN Exception\nstop Tx/Rx CLDMA for EE
  failed.\n", DB_OPT_DEFAULT)` (`N` = `md->id + 1`), then continues the handshake. The stale
  "primitive is missing / SCAFFOLD CHANGE REQUIRED" comment is gone. Order preserved:
  stop → `ccci_exception_trigger(CCCI_EX_DUMP_MD_EX)` (407) → `H2D_EXCEPTION_ACK` (412).
* `HIF_EX_CLEARQ_DONE` (421): `msleep(2000)` (`DS/modem_cldma.c:2015`) →
  `ccci_cldma_dhl_rx_alive(md->cldma, false)` (error logged, handshake continues) →
  `ccci_cldma_stop(md->cldma)` (full stop, mask-all, purge + flush of the TX/RX/refill work) →
  `ccci_exception_trigger_sync(CCCI_EX_DUMP_MD_EX)` (460) → `H2D_EXCEPTION_CLEARQ_ACK` (462).
* `HIF_EX_ALLQ_RESET` (464): unchanged call to `ccci_cldma_start()`, comment extended to state
  that it re-arms the queues, re-applies the latched ring map, re-unmasks L2/L3
  (`cldma_hw_start()`, `NURA/ccci_cldma/ccci_cldma.c:1261-1270`) and re-checks `SO_CFG` bit 0
  (`ccci_cldma.c:1941-1951`), mirroring `DS/modem_cldma.c:2026-2033`.

Failure codes exposed by the new API: `0` success, `-EINVAL` invalid engine pointer,
`-ETIMEDOUT` engine did not report the stopped queues idle. No stub returns and no invented
register values; nothing calls AEE from inside a spinlock.

## 4. Parity matrix

| Behaviour | ROM / downstream reference | Nura before | after | status |
|---|---|---|---|---|
| TX complete stop at EE (`UL_STOP_CMD = 0xFF`, dummy read, poll `UL_STATUS`) | `DS/modem_cldma.c:1456-1479`; ROM `0xc05285d0-0xc05285f8` | full stop only (`cldma_hw_stop`, `ccci_cldma_stop`) | `ccci_cldma_stop_for_ee()` step 2 | done |
| RX partial stop (`SO_STOP_CMD = 0xF0`, dummy read, poll `SO_STATUS & 0xF0`) | `DS/modem_cldma.c:1481-1501`; ROM `0xc05286b8-0xc0528754` | whole engine stopped (all 8 RX) | RX 4..7 stopped, RX 0..3 stay armed | done |
| Active-mask bookkeeping (`txq_active &= ~0xFF`, `rxq_active &= ~0xF0`) | `DS/modem_cldma.c:1458,1481`; ROM `0xc0528574`, `0xc05286b8` | `hw_stop` cleared both fully | exact masks under `timeout_lock` | done |
| Progress dump every 100000 polls | `DS/modem_cldma.c:1464-1472`; ROM `0xc0528604-0xc0528618` | none | `dev_info` + `cldma_dump_register()` | done (engine registers only; see §6) |
| 1600000-iteration bound | `DS/modem_cldma.c:1473-1476`; ROM `0xc0528550-0xc05285bc` | none | `CLDMA_EE_STOP_POLL_MAX` | done |
| AEE notification on stop timeout | `DS/modem_cldma.c:1468-1476,1495-1503`; ROM `0xc0528698`, `0xc05287e4` | n/a | `aed_md_exception_api(..., DB_OPT_DEFAULT)` from the caller, not from the spinlock | done |
| L2/L3 latch clear of the stopped queues | `DS/modem_cldma.c:1503-1511`; ROM `0xc05287f4-0xc0528888` | mask-all only | exact values incl. `0xF0F0F0F0` | done |
| L2/L3 mask, DHL DONE\|ERROR preserved | `DS/modem_cldma.c:1512-1521`; ROM `0xc052889c-0xc0528928` | mask-all | `L2RIMSR0 = 0x00F000F0`, rest as cited | done |
| DHL window opens on the EE stop | `DS/modem_cldma.c:1481-1521` (RX 0..3 left running/unmasked) | empty (engine fully stopped) | open; `ccci_cldma_dhl_rx_alive(cd, true)` available to re-arm | done |
| DHL window closes before the clear-queue ACK | `DS/modem_cldma.c:1966-1983` (full `cldma_stop`) + `1694-1730` | full stop at `HIF_EX_CLEARQ_DONE` (no explicit close) | stop RX 0..3 + mask/latch clear, then full stop | done |
| 2 s flush window | `DS/modem_cldma.c:2015` | present | preserved (`msleep(2000)`) | done |
| `H2D_EXCEPTION_ACK` after the EE stop | `DS/modem_cldma.c:2005-2009`; ROM `0xc0528940-0xc0528950` | ACK after full stop | ACK after `stop_for_ee` | done |
| TX ring purge at `HIF_EX_INIT` | `DS/modem_cldma.c:2007` (`md_cd_clear_all_queue(md, OUT)`) | implicit in full stop at INIT | happens at `HIF_EX_CLEARQ_DONE` via `ccci_cldma_stop()` | partial — the frozen API (only `stop_for_ee`/`dhl_rx_alive`) exposes no TX-only ring purge; DMA is already stopped so no frame reaches the modem |
| Restart at `HIF_EX_ALLQ_RESET` + `SO_CFG` bit 0 re-check | `DS/modem_cldma.c:2024-2033`; ROM `0xc0528d04-0xc0528d1c` | present | unchanged, cited in the comment | done |
| No RGU reset during the exception flow | `DS/modem_cldma.c:2024-2027` (reset+start only) | `exception_mode` guard in `ccci_cldma_start()` | unchanged; mode is latched before the EE stop by `ccci_md_set_state()` (`ccci_core.c:139-148`) | done |
| Lock discipline (no AEE/sleep under a spinlock) | downstream violates this (`DS/modem_cldma.c:1455-1522`) | `hw_stop` polls outside the lock | same as `hw_stop`; `start_lock` held across the sequence | done (deviation, §6) |
| MD clock-source lock around the queue stop | `DS/modem_cldma.c:1704`, helper is a no-op on MT6735 (`DS/mt6735/cldma_platform.c:176-179`) | n/a | not needed; nothing to port | done |

## 5. Build evidence

Environment exactly as `rem2-contract.md` §0 (`TMPDIR=$M2/tmp`, `PATH=$M/llvmpkg/usr/bin:$M/bin`,
`-j2`). `nura.config` has no CCCI option, so `CONFIG_MTK_CCCI` defaults to `n` and the objects
are not part of the default config; for the object builds the worktree-local `.config` was
switched on with `./scripts/config -e MTK_CCCI && make olddefconfig`
(`CONFIG_MTK_CCCI=y`, `CONFIG_MTK_CCCI_CLDMA=y`, `CONFIG_MTK_CCCI_UTIL=y`). `$M/nura.config`
was not modified. No `vmlinux` build was run.

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export TMPDIR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work/tmp
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work/worktrees/rem2-cldma

make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci_cldma/ccci_cldma.o \
     drivers/misc/mediatek/ccci/ccci_ccif.o drivers/misc/mediatek/ccci/ccci_core.o
# rc=0. Warnings: 10, all pre-existing kernel-doc in untouched functions:
#   ccci_ccif.c:146 'md'/'channel' not described in 'ccci_ccif_send'   (4 lines, 2 params x2)
#   ccci_core.c:494 'md'/'qno'/'skb' not described in 'ccci_md_send_skb' (6 lines)
# Proof they are pre-existing: building the baseline blob of ccci_ccif.c from
# parity-baseline-all.patch yields the same 4 'ccci_ccif_send' kernel-doc warnings.
# No warning from ccci_cldma.c.

make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci_cldma/ drivers/misc/mediatek/ccci/
# rc=0; thin archives created:
#   drivers/misc/mediatek/ccci_cldma/built-in.a  (ccci_cldma.o)
#   drivers/misc/mediatek/ccci/built-in.a        (ccci_core.o, ccci_platform.o,
#                                                 ccci_ccif.o, ccci_boot.o, ...)
# When every object in ccci/ is compiled from scratch the same build also reports
# ccci_boot.c:730 kernel-doc and ccci_ringbuf.c:209 unused-but-set-variable - both in files
# this patch does not touch.

make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci_cldma/    # after the last header edit
# rc=0, 0 warnings.
```

No userspace tool is part of this gap.

## 6. Hardware-unverified items and deliberate deviations

1. **Poll outside the spinlock.** Downstream holds `cldma_timeout_lock` across both stop loops
   and up to 1600000 iterations; this implementation takes the same lock only for the
   `txq_active`/`rxq_active` updates and for the latch/mask programming, and runs the poll
   under `start_lock` (mutex) instead. Reason: the loops can spin for ~10^6 MMIO accesses and
   the driver already polls outside the lock in `cldma_hw_stop()`. The dummy-read pattern,
   masks, register offsets and bounds are unchanged, and `start_lock` still excludes
   `ccci_cldma_start()`/`ccci_cldma_stop()`, so no other writer can touch the stop/start
   command registers concurrently.
2. **AEE is raised by the caller, not inside the loop.** Downstream calls
   `aed_md_exception_api()` from inside the spinning loop; here `stop_for_ee()` returns
   `-ETIMEDOUT` and `ccci_ccif_exception()` reports it (same detail string form, `DB_OPT_DEFAULT`),
   then continues the handshake. Reason: the frozen prototype has no AEE argument and the
   contract forbids cross-layer calls from inside the spinlock.
3. **Progress dump content.** Downstream's 100000-iteration dump also calls
   `ccci_mem_dump()` (MD exception smem) and `md_cd_dump_debug_register()` (MD bus status / PC
   monitor). The CLDMA object owns neither the smem window nor the MD register map; a dump of
   the engine registers (`cldma_dump_register()`, already used by the ISR timeout path) is what
   this layer can produce. The core still snapshots the full MD state per stage
   (`ccci_exception_trigger()` at `HIF_EX_INIT`, `ccci_exception_trigger_sync()` at
   `HIF_EX_CLEARQ_DONE`), so the exceptional-memory dump is not lost - it is not interleaved
   with the poll.
4. **DHL set is RX 0..3, not 4..7.** The frozen `include/linux/ccci.h` comment calls
   `NONSTOP_QUEUE_MASK 0xF0` "the DHL set". The mask in `SO_STOP_CMD` and in the `rxq_active`
   clear is the set that is **stopped** (`DS/modem_cldma.c:1481-1487`, ROM
   `0xc05286b8`/`0xc0528718`); the queues that stay alive, unmasked and carrying the exception
   traffic are `0x0F` (queues 0..3), as corroborated by `DS/modem_cldma.c:113` and the port
   table (`rxq_exp` = 1/2/3 for the log ports). The implementation follows the register writes,
   not the wording. The frozen declarations themselves are sufficient; **no scaffold change is
   required**, only the comment in `include/linux/ccci.h` could be reworded.
5. **L3 masks in the close direction.** `ccci_cldma_dhl_rx_alive(cd, false)` clears/masks
   `L3RISAR0/1` and `L3RIMSR0/1` with `CLDMA_BM_INT_ALL` because no available source attributes
   L3 error bits to individual RX queues (downstream only ever writes `0xF0F0F0F0` for the
   stopped set or `0xFFFFFFFF` for the full stop). The close is always followed by the full
   stop, so this is a superset that cannot lose an interrupt source the caller still needs.
6. **No TX-only ring purge at `HIF_EX_INIT`** (parity row "partial"): the frozen API exposes
   only the two functions. TX DMA is stopped before the ACK, so no descriptor is handed to a
   modem that is resetting; the TX rings are cleared by `ccci_cldma_stop()` at
   `HIF_EX_CLEARQ_DONE` (and by any later `ccci_md_stop()` path). Missing input: a frozen-API
   entry point for a TX-only purge (e.g. `int ccci_cldma_purge_tx(struct ccci_cldma *)`).
7. **`stopped`/`started` interplay.** `stop_for_ee()` leaves `cd->started == true` so the
   `HIF_EX_ALLQ_RESET` restart works and `ccci_cldma_stop()` at `HIF_EX_CLEARQ_DONE` performs
   the complete stop/purge. If the handshake aborts before `HIF_EX_CLEARQ_DONE` (CCIF poll
   timeout), the engine stays in "TX stopped, RX 0..3 alive" state until the core's normal
   `ccci_md_stop()` path runs `ccci_cldma_stop()`; that matches downstream, whose window also
   only closes in its delayed work.
8. **Everything is hardware-unverified.** All timing is the downstream/ROM iteration count
   (100000/1600000), not a measured duration; the actual poll duration on MT6735 is unknown
   here. The `SO_STATUS`/`UL_STATUS` bit semantics, the `0x00F000F0` `L2RIMSR0` value and the
   DONE|ERROR positions are taken from the ROM writes and the downstream ISR, not from a
   register manual (none is available in any source tree).
9. **`EXPORT_SYMBOL_GPL`** was added for both functions even though `CONFIG_MTK_CCCI_CLDMA` is
   `bool` today and no other `ccci_cldma_*` entry point is exported: the frozen declarations
   live in `include/linux/ccci.h` and the exports keep the ABI usable if the CLDMA option is
   ever made tristate. Cost is one ksymtab entry each.

## 7. DTS-FRAGMENT

None. This gap changes no device tree property: the CLDMA register windows, IRQ and the
optional INFRA_AO window already come from the existing modem/CLDMA nodes, and no new
compatible, clock, reset or interrupt is introduced.

## 8. Patch

`$M/patches/rem2-cldma.patch` — 16344 bytes, sha256
`684bdc22f2c657a0c0a45265d76529e75fb42b8ddff1eac5919d16410d9ae086`.

The baseline patch is applied but not committed in this worktree, so `git add -A && git diff
--cached` would have captured the whole parity baseline (the three files are new files in
`parity-baseline-all.patch`). The delta was therefore produced in a temporary git repository
(removed after verification) seeded with the baseline blobs of those three files, extracted
from `$M/patches/parity-baseline-all.patch` (`@@ -0,0 +1,N @@` bodies):

```sh
# seed repo with the baseline blobs, commit, overlay the worktree files, then
git diff > $M/patches/rem2-cldma.patch
# verify against the baseline state
git checkout -- .                                        # back to the baseline blobs
git apply --check -v $M/patches/rem2-cldma.patch         # OK
git apply $M/patches/rem2-cldma.patch
cmp <each of the three files> <worktree copy>            # identical
```

`git status --porcelain` in the worktree lists the parity baseline (all of
`drivers/misc/mediatek/` is untracked in this baseline) plus the local `.config`; the patch
itself contains only the three files of this gap listed in §1.
