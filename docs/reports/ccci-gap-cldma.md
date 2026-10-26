# CCCI gap — CLDMA exception rings + RGU reset

`M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Worktree `$M/worktrees/ccci-gap-cldma`, HEAD `19003f335 ccci: gap scaffold`
(baseline `3539276c4 ccci: integration baseline`). Continued from the aborted
first run (251-line partial diff), not restarted.

## 1. Gap, files changed, result

**Gap.** The Nura CLDMA engine had normal-mode rings only: it could not move a
network queue onto its 4K exception ring, it never programmed the block RGU
reset, and its start/stop sequence was unsynchronised (a `ccci_md_start()` /
exception restart could interleave with a stop or with an exception-mode latch).
The tree's exception handshake (`ccci_ccif_exception()`) triggers both, so the
gap is a live-transfer hazard, not only a missing feature.

**Files changed**

| File | Change |
|---|---|
| `drivers/misc/mediatek/ccci_cldma/ccci_cldma.h` | `CLDMA_NORMAL_QUEUE_MASK 0xCF`, INFRA_RST0/1 + masks, `infra_ao`, `exception_mode`, `start_lock` |
| `drivers/misc/mediatek/ccci_cldma/ccci_cldma.c` | ring map switching (`cldma_queue_switch_ring()`, `cldma_apply_ring_map()`), RGU reset (`ccci_cldma_hw_reset()`), `ccci_cldma_set_infra_ao()`, `ccci_cldma_set_exception_mode()`, start/stop serialisation + reset gating, SO_CFG readback |
| `drivers/misc/mediatek/ccci/ccci_core.c` | additive: ring-set hook in `ccci_md_set_state()`; force normal map before the boot `ccci_cldma_start()` |
| `drivers/misc/mediatek/ccci/ccci_platform.c` | additive: hand `md->infra_ao` to the engine after `ccci_cldma_create()` |

**Result.** Ring switching, RGU reset and exception-mode latching are at parity
with `modem_cldma.c`; the RGU reset is no longer asserted on the exception
restart (downstream does not reset the block there); start/stop are serialised
under a new `start_lock` with the publish/rollback ordering fixed. One
downstream behaviour remains partial (non-stop RX queues during the exception
handshake) and is called out in §3/§7 — it needs a frozen-header change.

## 2. ROM + downstream evidence

### 2.1 Ring geometry and mapping

| Item | Evidence |
|---|---|
| 8 queues/direction, 3 net rings, 6 normal rings | `eccci/modem_cldma.h:31-36` |
| Net queues 3,4,5 | `NET_TX/RX_QUEUE_MASK 0x38`, `modem_cldma.c:108-109` |
| Normal ring set = queues 0,1,2,3,6,7 | `NORMAL_TX/RX_QUEUE_MASK 0xCF`, `modem_cldma.c:113-114` |
| net queue→ring `{-1,-1,-1,0,1,2,-1,-1}` | `modem_cldma.c:94-95` |
| normal queue→ring `{0,1,2,3,-1,-1,4,5}` (4/5 = −1) | `modem_cldma.c:96-97` |
| ring→queue `{0,1,2,3,6,7}` | `modem_cldma.c:98-101` |
| net RX buffer size `{0,0,0,SKB_1_5K×3,0,0}` | `modem_cldma.c:84` |
| normal RX buffer size `{SKB_4K×4,0,0,SKB_4K,SKB_4K}` | `modem_cldma.c:85` |
| net buffer count `{0,0,0,256,256,64,0,0}` | `modem_cldma.c:90-91` |
| normal buffer count `{16,16,16,16,0,0,16,16}` | `modem_cldma.c:92-93` |
| `SKB_4K = CCCI_MTU+128 = 3584`, `SKB_1_5K = CCCI_NET_MTU+16 = 1516` | `eccci/ccci_bm.h:28-29`, `eccci/mt6735/ccci_config.h:93-94` |
| switch decision `ex_stage != EX_INIT_DONE` → net ring, else normal if bit in `NORMAL_*_QUEUE_MASK`, 4/5 keep previous ring (no `else`) | `modem_cldma.c:1094-1133` |
| switch called at queue init and at every `cldma_start()` | `modem_cldma.c:1178,1206` (init), `1583,1592` (start) |
| exception map applies at `HIF_EX_ALLQ_RESET`: `cldma_reset()` → `EX_INIT_DONE` notify → `cldma_start()` | `modem_cldma.c:2021-2024` |

### 2.2 RGU / block reset

| Item | Evidence |
|---|---|
| `INFRA_RST0_REG 0x0030` (reset), `INFRA_RST1_REG 0x0034` (release) | `eccci/mt6735/cldma_reg.h:18-19` |
| `CLDMA_AO_RST_MASK (1<<8)`, `CLDMA_PD_RST_MASK (1<<26)` | `eccci/mt6735/cldma_reg.h:20-21` |
| Sequence: RMW `RST0 |= AO|PD`, then RMW `RST1 |= AO|PD` | `eccci/mt6735/cldma_platform.c:55-71` |
| Call sites: `md_cd_start()` (before `md_cd_power_on()`) and end of `md_cldma_clear()` | `modem_cldma.c:2340` / `2341`, `modem_cldma.c:2468` |
| Guard `ENABLE_CLDMA_AP_SIDE` | `eccci/mt6735/ccci_config.h:44` |
| ROM `md_cldma_hw_reset` = `0xc0530a80`: `ldr r3,[r2,#0x30]` → `orr #67108864` (bit 26) → `orr #256` (bit 8) → `str [r2,#0x30]`; same on `[r2,#0x34]` | ROM `vmlinux.elf` disassembly, `llvm-objdump -d --start-address=0xc0530a80` |
| Exception restart does **not** reset the block: `cldma_reset()` + `cldma_start()` | `modem_cldma.c:2020-2024` |

### 2.3 MD RGU watchdog — not a CLDMA gap

| Item | Evidence |
|---|---|
| `MD_RGU_BASE 0x20050000` (`MD2/MD3 0x30050000`) | `eccci/mt6735/modem_reg_base.h:28,55,71` |
| `WDT_MD_MODE 0x00`, `WDT_MD_STA 0x0C`, `WDT_MD_MODE_KEY 0x0000220E` | `eccci/mt6735/cldma_platform.h:20-25` (identical in `ccif_c2k_platform.h:28-33`) |
| WDT disable write in power-on path | `eccci/mt6735/cldma_platform.c:319` |
| `WDT_MD_STA` read / `WDT_MD_MODE` write in the MD WDT ISR | `modem_cldma.c:1917-1923` |
| Tree ownership: `md-rgu` window mapped in the platform probe and handed to the CCIF object | `ccci/ccci_platform.c:223-227` |
| Tree: `WDT_MD_MODE_KEY` programmed on boot | `ccci/ccci_boot.c:531-534` |
| Tree: MD WDT IRQ handler + recovery work | `ccci/ccci_ccif.c:555-572` |
| Tree constants | `ccci/ccci_priv.h:91-96` |

Conclusion: the RGU watchdog is **MD-side RGU**, not the CLDMA block; it is
already implemented by the CCIF/core code in this tree (mapped, key written at
boot, IRQ + recovery work). No CLDMA action; nothing added here.

### 2.4 Exception handshake

| Stage | Downstream | Tree |
|---|---|---|
| `HIF_EX_INIT` | `cldma_stop_for_ee()` (stops TX + RX 4-7, keeps RX 0-3), `md_cd_clear_all_queue(OUT)`, `H2D_EXCEPTION_ACK` (`modem_cldma.c:1994-2008`; masks `modem_cldma.c:113-114,1481-1489`) | `ccci_md_set_state(EXCEPTION)` → latch, full `ccci_cldma_stop()`, ACK (`ccci/ccci_ccif.c:364-371`) |
| `HIF_EX_INIT_DONE` | `EX_DHL_DL_RDY` notify (`modem_cldma.c:2011-2013`) | state re-set (`ccci_ccif.c:373-375`) |
| `HIF_EX_CLEARQ_DONE` | `msleep(2000)` DHL flush, then delayed RX purge (`modem_cldma.c:2015-2019`) | CCIF ringbuf reset + ACK (`ccci_ccif.c:376-386`) |
| `HIF_EX_ALLQ_RESET` | `cldma_reset()`, `EX_INIT_DONE`, `cldma_start()`, SO_CFG readback (`modem_cldma.c:2020-2033`) | `ccci_cldma_start()` (`ccci_ccif.c:387-390`) |

## 3. Parity matrix

| Behaviour | ROM + downstream | Nura before | After | Status |
|---|---|---|---|---|
| Queue→ring map, net mode | `modem_cldma.c:1094-1133` | net rings only, queue 3 normal ring deliberately built with `length = 0` | full tables, queue 3 normal ring built with 16×4K and only selected when `exception_mode` | done |
| `CLDMA_NORMAL_QUEUE_MASK 0xCF` | `modem_cldma.c:113` | absent | `ccci_cldma.h`, bit 3 selects the 4K ring in exception mode | done |
| Queues 4/5 have no normal ring | `-1` entries, `modem_cldma.c:96-97`; no `else` in switch | n/a | exception branch keeps the previously armed net ring (same as downstream), with a NULL-safe fallback on first arm | done |
| Re-point only at (re)arm | switch at queue init + `cldma_start()` | setter absent | `ccci_cldma_set_exception_mode()` latches only while `started`; `ccci_cldma_start()` applies under `timeout_lock` before arming; `cldma_hw_start()` re-applies under each `ring_lock` | done |
| RGU reset offsets/masks | `cldma_reg.h:18-21`, `cldma_platform.c:55-71` | absent | verbatim RMW sequence, `INFRA_RST0 0x30` / `INFRA_RST1 0x34`, mask `0x04000100` | done |
| RGU reset call sites | `md_cd_start()` before power-on, end of `md_cldma_clear()` (`modem_cldma.c:2340,2468`) | absent | `ccci_cldma_start()` (cold start) and end of `ccci_cldma_stop()` | done |
| No block reset on the exception restart | `modem_cldma.c:2020-2024` | (would have reset — start unconditionally reset) | reset skipped while `exception_mode` is latched, in both start and stop | done |
| SO_CFG accepted check | `modem_cldma.c:2026-2033` | absent | readback after every arm, re-write `\|0x05` on failure | done (superset: every arm, downstream only at ALLQ_RESET) |
| `started` publish ordering / rollback | no equivalent (downstream start is void) | `started = true` after applying the map, no serialisation | `started` published under `timeout_lock` before any ring/hardware touch; `-EBUSY` on double start; `started` rolled back if `ccci_cldma_hw_reset()` fails | done |
| Start/stop serialisation | `cldma_timeout_lock` inside `cldma_start`; `md_cldma_clear` only when stopped | none (stop could interleave with start) | new `start_lock` mutex around the whole arm/stop sequence | done |
| Ring re-point vs. concurrent TX/IRQ worker | `cldma_start` holds `cldma_timeout_lock`; queues drained | `cldma_hw_start()` re-pointed cursors without `ring_lock` | `ring_lock` taken per queue around `cldma_queue_switch_ring()` + `TQSAR/RQSAR` write | done |
| Exception stop keeps RX 0-3 alive | `cldma_stop_for_ee()`, `SO_STOP_CMD = 0xF0`, RX 0-3 stay active/unmasked (`modem_cldma.c:1449-1520`) | full stop | full stop | **partial** — frozen API has no exception stop; see §7 |
| 2000 ms DHL flush at `CLEARQ_DONE` | `modem_cldma.c:2015-2019` | absent | absent | **deferred** — lives in `ccci_ccif.c` (core/exception owner), not in the CLDMA engine; noted below |
| RX queue purge after the flush | `md_cd_ccif_delayed_work()` (`modem_cldma.c:2017-2018`) | CCIF ringbuf reset only | CCIF ringbuf reset only | **deferred** — same owner as above |
| MD RGU watchdog | MD RGU, `modem_cldma.c:1917-1923` | implemented by CCIF/core | unchanged | done (other owner) |
| MD3/C2K | n/a | untouched | untouched (`ccci_cldma` change has no MD index dependency) | done |

Notes on the two deferred items: they are steps of the exception **handshake
state machine** that lives in `ccci_ccif.c`, which this leaf does not own (the
dump leaf owns the exception smem/AEE side). The CLDMA engine provides every
primitive they need except the non-stop stop (§7).

## 4. Compile-tested

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-gap-cldma"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig          # exit 0
rm -f drivers/misc/mediatek/ccci_cldma/ccci_cldma.o \
      drivers/misc/mediatek/ccci/ccci_core.o \
      drivers/misc/mediatek/ccci/ccci_platform.o
make ARCH=arm64 LLVM=1 -j2 W=1 \
     drivers/misc/mediatek/ccci_cldma/ccci_cldma.o \
     drivers/misc/mediatek/ccci/ccci_core.o \
     drivers/misc/mediatek/ccci/ccci_platform.o   # exit 0
```

All three objects were rebuilt from scratch (`CC` lines emitted for each).
`W=1` warnings: only six pre-existing kernel-doc warnings in
`ccci_core.c:404` (`ccci_md_send_skb()` parameter descriptions) from the
integration baseline, outside this leaf's hunks (lines 118-150 and 625-641).
`ccci_cldma.o` and `ccci_platform.o` are warning-free. No new file, so no
`built-in.a` target was needed.

Cross-leaf symbols (`ccci_md_set_dsp_mpu_protection()`, `ccci_rpc_*`,
`aee_dump_ccci_debug_info()`) are not referenced here; no stubs were added.

## 5. `[INFERENCE]` and hardware-unverified

- `[INFERENCE]` For queues 4/5 in exception mode the downstream `else if` has
  no `else`, so `queue->tr_ring` keeps its previous value. The port mirrors
  that by keeping the current ring and falling back to the net ring only when
  no ring was armed yet (first arm). Observable behaviour is identical because
  queues 4/5 are always on their net ring.
- `[INFERENCE]` Downstream resets the block *before* `md_cd_power_on()`
  (`modem_cldma.c:2340-2341`); this tree calls `ccci_cldma_start()` after
  power-on (`ccci_core.c:604-635`). The RGU masks touch only the AP-side
  CLDMA AO/PD block, all start addresses are reprogrammed immediately after,
  and the MD is not released until later, so the ordering is treated as
  equivalent. Not hardware-verified.
- `[INFERENCE]` `cldma_hw_config()` is executed on the exception restart as
  well as on a cold start; downstream's `cldma_reset()` does the same register
  set at `HIF_EX_ALLQ_RESET`, so the shared path is intentional.
- `[INFERENCE]` The `SO_CFG` readback is applied to every arm rather than only
  the exception restart; the check and the recovery write are verbatim.
- Hardware-unverified: the actual RGU reset effect, the queue-3 4K ring under
  a real exception, and the `msleep(2000)` flush timing (not implemented here).
  No hardware was available; nothing was simulated.

## 6. DTS-FRAGMENT

Required only for the RGU reset. Without it `md->infra_ao` stays NULL,
`ccci_cldma_hw_reset()` returns `-ENODEV` and the engine runs normally (reset
skipped, logged at debug level). The property goes on the `mdcldma` node from
`$M/logs/ccci-cldma.md` §9:

```dts
&{/soc} {
	mdcldma: mdcldma@1000a000 {
		/* ... existing properties from ccci-cldma.md §9 ... */
		mediatek,infracfg-ao = <&infracfg>;	/* INFRA_RST0/1 */
	};
};
```

`&infracfg` is the Nura `clock-controller@10000000`
(`arch/arm64/boot/dts/mediatek/mt6735.dtsi:145-150`:
`reg = <0 0x10000000 0 0x1000>`), the same block as the downstream/ROM
`infracfg_ao@10000000` (`arch/arm64/boot/dts/mt6735.dtsi:304-307`,
`$M/zip/rom_dtb_0.dts:315-318`, `reg = <0x10000000 0x1000>`), so offsets
`0x30`/`0x34` are covered.

Validated by compiling `mt6735.dtsi` + this node/property with
`cpp -nostdinc -undef -x assembler-with-cpp … | scripts/dtc/dtc -I dts -O dtb`
(exit 0; the only warnings are the pre-existing
`/soc/interrupt-controller@10220000` and `/soc/dsi@14011000` ones from
`mt6735.dtsi`). The decompiled DTB shows
`mediatek,infracfg-ao = <0x03>` resolving to the infracfg node.

## 7. SCAFFOLD CHANGE REQUIRED

One declaration is genuinely insufficient: the exception handshake needs a stop
that matches `cldma_stop_for_ee()` (`modem_cldma.c:1449-1520`), i.e. stop TX and
RX queues 4-7 while RX queues 0-3 keep running with their interrupts unmasked,
and without a block reset.

```c
/*
 * CLDMA exception stop, downstream cldma_stop_for_ee():
 * stop all TX queues and the RX queues in @stop_mask (SO_STOP_CMD), leaving
 * the others running and unmasked. modem_cldma.c:1449-1520 uses
 * NONSTOP_QUEUE_MASK 0xF0, i.e. RX 0-3 stay alive. Must not reset the block.
 */
void ccci_cldma_stop_for_ee(struct ccci_cldma *cd, u16 stop_mask);
```

Callers that would then change (not in this leaf's files):
`ccci_ccif.c:369` `ccci_cldma_stop(md->cldma)` →
`ccci_cldma_stop_for_ee(md->cldma, 0xF0)`; and the `msleep(2000)` +
delayed RX purge at `HIF_EX_CLEARQ_DONE` (`ccci_ccif.c:376-386`) belongs to the
same handshake owner. Everything the CLDMA side needs for it (per-queue stop
masks, `txq_active`/`rxq_active` bookkeeping, interrupt mask registers) is
already implemented.

## 8. Patch and status

Patch: `$M/patches/ccci-gap-cldma.patch` (550 lines, diff against HEAD
`19003f335`, i.e. baseline+scaffold).

`git status --porcelain`:

```
M  drivers/misc/mediatek/ccci/ccci_core.c
M  drivers/misc/mediatek/ccci/ccci_platform.c
M  drivers/misc/mediatek/ccci_cldma/ccci_cldma.c
M  drivers/misc/mediatek/ccci_cldma/ccci_cldma.h
```

Diffstat: 4 files, +336/−28. Shared-file hunks for the coordinator:
`ccci_core.c` has two additive blocks (in `ccci_md_set_state()` right after the
`md->lock` release, and immediately before the `ccci_cldma_start()` call in
`ccci_md_start()`); `ccci_platform.c` has one two-line block after
`ccci_cldma_set_rx_cb()` in the probe. No shared DTS, no `.config`, no other
leaf's worktree touched.
