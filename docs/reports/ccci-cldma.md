# CCCI CLDMA driver (MT6735/MT6737T MD1 data transport)

Leaf: `ccci-cldma` — worktree `$M/worktrees/ccci-cldma`
Patch: `$M/patches/ccci-cldma.patch`
Report: `$M/logs/ccci-cldma.md`

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Files changed

| File | Change |
|---|---|
| `include/linux/ccci.h` | New, **byte-identical** to `CCCI_CONTRACT.md` §5 (md5 `a44b077ce8fb37dd2c2d5c84f7d462e0`, verified with `diff` against lines 85-369 of the contract). |
| `drivers/misc/mediatek/ccci_cldma/ccci_cldma.c` | New, 1743 lines: the CLDMA engine. |
| `drivers/misc/mediatek/ccci_cldma/ccci_cldma.h` | New, 327 lines: private register map, descriptor layouts, ring/queue/engine structures. |
| `drivers/misc/mediatek/ccci_cldma/Kconfig` | New: `MTK_CCCI_CLDMA`, `bool`, no cross-leaf `depends on`. |
| `drivers/misc/mediatek/ccci_cldma/Makefile` | New: `obj-$(CONFIG_MTK_CCCI_CLDMA) += ccci_cldma.o`. |
| `drivers/misc/mediatek/Kconfig` | New: `menu "MediaTek device drivers"` + one `source` + `endmenu` (contract §3). |
| `drivers/misc/mediatek/Makefile` | New: `obj-y += ccci_cldma/` (contract §3). |
| `drivers/misc/Kconfig` | One line: `source "drivers/misc/mediatek/Kconfig"` before `endmenu`. |
| `drivers/misc/Makefile` | One line: `obj-y += mediatek/`. |

No DTS, no `nura.config`, no other leaf file touched. `git status --porcelain` after staging lists exactly the nine files above.

---

## 2. ROM evidence

### Symbols (from `$M/zip/vmlinux.elf`, `llvm-nm`)

| Symbol | Address | Notes |
|---|---|---|
| `cldma_isr` | `0xc051ead4` | present |
| `cldma_irq_work_cb` | `0xc051e3ac` | present |
| `cldma_irq_work` | `0xc051eab4` | present, but unreferenced by the ROM ISR |
| `cldma_rx_refill` | `0xc051ed10` | present |
| `cldma_rx_done` | `0xc051ef2c` | present |
| `cldma_tx_done` | `0xc051f764` | present |
| `cldma_gpd_rx_collect` | `0xc051f994` | present |
| `cldma_gpd_handle_tx_request` | `0xc052032c` | present |
| `md_cd_clear_all_queue` | `0xc0520484` | present |
| `cldma_gpd_rx_refill` | `0xc05207e0` | present |
| `cldma_enable_irq` | `0xc0527be0` | present |
| `cldma_gpd_tx_collect` | `0xc0527870` | present |
| `cldma_net_rx_push_thread` | `0xc0524c90` | present |
| `cldma_timeout_timer_func` | `0xc0524bb8` | present |
| `cldma_tx_ring_init` | `0xc0525078` | present |
| `cldma_rx_ring_init` | `0xc0525414` | present |
| `cldma_queue_switch_ring` | `0xc051c924` | present |
| `cldma_dump_gpd_queue` | `0xc051cb80` | present |
| `md_cd_send_request` | `0xc0526660` | present |
| `md_cldma_clear` | `0xc0529120` | present |
| `md_cldma_hw_reset` | `0xc0530a80` | present |

### Disassembly observations

- `md_cldma_hw_reset` (`0xc0530a80`): reads `infra_ao + 0x30` (`INFRA_RST0_REG`),
  sets bits `0x04000100` (bit 8 = `CLDMA_AO_RST_MASK`, bit 26 = `CLDMA_PD_RST_MASK`),
  writes back; then does the same on `infra_ao + 0x34` (`INFRA_RST1_REG`). This
  confirms `mt6735/cldma_reg.h:19-23` (`INFRA_RST0_REG 0x0030`,
  `INFRA_RST1_REG 0x0034`, `CLDMA_AO_RST_MASK 1<<8`, `CLDMA_PD_RST_MASK 1<<26`).
  The reset is **not** implemented in this leaf because `struct ccci_cldma_cfg`
  exposes only `AP_CLDMA_AO`/`AP_CLDMA_PDN`; the INFRA_AO block is not part of
  the frozen configuration (deferred, see §8).
- `cldma_isr` (`0xc051ead4`): the ROM ISR calls `cldma_irq_work_cb` directly
  (`bl 0xc051e3ac`) and returns `1` (`IRQ_HANDLED`), i.e. the ROM build took the
  `ENABLE_CLDMA_AP_SIDE` path of `modem_cldma.c:1329-1344`. The Nura port uses a
  threaded IRQ (`IRQ_WAKE_THREAD` → `cldma_irq_thread`, `IRQF_ONESHOT`), which is
  the modern equivalent of the same synchronous drain: no separate `cldma_irq_work`
  work item is needed because the thread itself is the deferred context.

---

## 3. Downstream references

All paths are under
`$M/src/downstream-grandpplte/drivers/misc/mediatek/eccci/`.

| Behaviour | Reference |
|---|---|
| queue/ring geometry, skb sizes | `modem_cldma.c:82-101`; `modem_cldma.h:31-53`; `mt6735/ccci_config.h:93-94` (CCCI_MTU 3456, CCCI_NET_MTU 1500) |
| descriptor structs (16 B) | `modem_cldma.h:250-300` |
| ring/queue layout, producer/consumer cursors | `modem_cldma.h:78-183` |
| `cldma_ring_step_forward()` | `modem_cldma.h:95-104` |
| register map | `mt6735/cldma_reg.h:42-222`; masks `:238-258` |
| timeout timer | `modem_cldma.c:308-330` |
| RX refill | `modem_cldma.c:333-384`, `531-577` |
| RX collect (normal) | `modem_cldma.c:386-537` |
| RX collect (net) | `modem_cldma.c:539-639` |
| net RX push thread | `modem_cldma.c:641-690` |
| refill work | `modem_cldma.c:692-720` |
| RX done work | `modem_cldma.c:722-796` |
| BD TX collect | `modem_cldma.c:798-860` |
| GPD TX collect | `modem_cldma.c:862-939` |
| TX done work | `modem_cldma.c:941-995` |
| RX ring init | `modem_cldma.c:997-1031` |
| TX ring init (GPD + BD) | `modem_cldma.c:1033-1092` |
| queue→ring switch | `modem_cldma.c:1094-1133` |
| queue/workqueue setup | `modem_cldma.c:1135-1180` |
| IRQ enable/disable | `modem_cldma.c:1182-1205` |
| IRQ work callback | `modem_cldma.c:1207-1327` |
| ISR | `modem_cldma.c:1329-1344` |
| stop | `modem_cldma.c:1354-1447` |
| engine config/reset | `modem_cldma.c:1526-1567` |
| start | `modem_cldma.c:1569-1617` |
| clear all queues | `modem_cldma.c:1619-1693` |
| BD TX request fill | `modem_cldma.c:2698-2760` |
| GPD TX request fill | `modem_cldma.c:2762-2799` |
| send path / doorbell | `modem_cldma.c:2801-2989` |
| dma_pool creation and rings | `modem_cldma.c:3825-3931` |
| 32-bit DMA mask | `modem_cldma.c:3858-3859` |
| RGU reset | `mt6735/cldma_platform.c:55-72`; `mt6735/cldma_reg.h:19-23` |

### Register map implemented (all offsets from `mt6735/cldma_reg.h`)

`cfg.ap_base` = AP_CLDMA_AO (`0x1000A000`), `cfg.ap_pdn_base` = AP_CLDMA_PDN
(`0x1021A000`). Offsets used, with the defining line in `cldma_reg.h`:

- AO: `SO_CFG 0x0A04` (42), `SO_CHECKSUM_CHANNEL_ENABLE 0x0A74` (45),
  `SO_START_ADDR_0 0x0A78` (46), `SO_CURRENT_ADDR_0 0x0A98` (54),
  `SO_STATUS 0x0AB8` (62), `L2RIMR0/1 0x0C58/0x0C5C` (65/66),
  `L2RIMCR0/1 0x0C60/0x0C64` (67/68), `L2RIMSR0/1 0x0C68/0x0C6C` (69/70),
  `BUS_CFG 0x0C90` (71).
- PDN: `UL_START_ADDR_0 0x0004` (80), `UL_CURRENT_ADDR_0 0x0028` (88),
  `UL_STATUS 0x0050` (96), `UL_START_CMD 0x0054` (97),
  `UL_RESUME_CMD 0x0058` (98), `UL_STOP_CMD 0x005C` (99), `UL_CFG 0x0074` (101),
  `HPQR 0x0118` (140), `TCR0 0x011C` (141),
  `UL_CHECKSUM_CHANNEL_ENABLE 0x0140` (150), `SO_START_CMD 0x03BC` (179),
  `SO_RESUME_CMD 0x03C0` (180), `SO_STOP_CMD 0x03C4` (181),
  `CLDMA_CODA_VERSION 0x0400` (183), `L2TISAR0/1 0x0410/0x0414` (184/185),
  `L2TIMR0/1 0x0418/0x041C` (186/187), `L2TIMCR0/1 0x0420/0x0424` (188/189),
  `L2TIMSR0/1 0x0428/0x042C` (190/191), `L3TISAR0/1 0x0430/0x0434` (192/193),
  `L3TIMR0/1 0x0438/0x043C` (194/195), `L3TIMCR0/1 0x0440/0x0444` (196/197),
  `L3TIMSR0/1 0x0448/0x044C` (198/199), `L2RISAR0/1 0x0450/0x0454` (200/201),
  `L3RISAR0/1 0x0470/0x0474` (202/203), `L3RIMR0/1 0x0478/0x047C` (204/205),
  `L3RIMCR0/1 0x0480/0x0484` (206/207), `L3RIMSR0/1 0x0488/0x048C` (208/209),
  `CLDMA_IP_BUSY 0x04B4` (216).
- Per-queue helpers `TQSAR(i)/TQCPR(i)/RQSAR(i)/RQCPR(i)` = `cldma_reg.h:219-222`.

No register value or offset was invented: every write in `ccci_cldma.c` is either
one of the constants above or a value derived from the ring/skb addresses.

---

## 4. Implementation notes

- **API.** Exactly the frozen header: `ccci_cldma_create/destroy/start/stop/send/
  set_rx_cb`. `CCCI_CLDMA_Q_BASIC`/`CCCI_CLDMA_Q_NET` are not used by this engine;
  the queue number is the hardware TX queue (0-7) and the network queues are
  3/4/5 per downstream tables.
- **Rings.** Six ring objects, built exactly like downstream
  (`modem_cldma.c:3883-3931`): `net_tx[3]`, `net_rx[3]`, `normal_tx[6]`,
  `normal_rx[6]`. Net TX rings are `RING_GPD_BD` (SGIO, `CLDMA_NET_TX_BD`,
  `modem_cldma.h:53`), all other rings are `RING_GPD`. Queue→ring tables are
  verbatim from `modem_cldma.c:94-101`.
- **Descriptors.** Allocated from one `dma_pool` of `sizeof(struct cldma_tgpd)`
  (16 B), created like `modem_cldma.c:3882`; `dma_pool` itself allocates from
  `dma_alloc_coherent`. RX payloads are skbs allocated with
  `__netdev_alloc_skb(NULL, pkt_size, GFP_KERNEL)` and `dma_map_single()`d
  (`DMA_FROM_DEVICE`); TX payloads are `dma_map_single()`d per skb
  (`DMA_TO_DEVICE`) or per head/fragment for BD rings.
- **TX.** `ccci_cldma_send()` reserves `queue->tx_xmit`, calls the ring's
  `handle_tx_request` (GPD or BD variant), advances the producer, then kicks the
  queue with `CLDMA_AP_UL_RESUME_CMD` (`modem_cldma.c:2908-2915`). Returns 0 and
  transfers skb ownership to the engine; returns `-EBUSY` when the reserved slot
  is still owned (`tx_xmit->skb != NULL`); returns `-EINVAL`/`-EIO` and keeps the
  skb with the caller on bad queue/length/DMA-map failure.
- **RX.** Refill arms `gpd_flags = 0x81` (IOC|HWO) after `wmb()`; collect
  waits for HWO=0, unmaps, `skb_put(data_buff_len)`, then:
  - queues 0/1/2/6/7: callback directly from the per-queue RX worker
    (`cldma_gpd_rx_collect`, `modem_cldma.c:386`);
  - queues 3/4/5: skb is queued and `cldma_net_rx_push_thread` delivers it
    (`modem_cldma.c:641`), matching downstream's non-NAPI path. The CCCI header
    stays at `skb->data`; the callback receives the hardware queue number.
  - Invalid RGPD lengths are dropped with a log instead of overflowing the skb.
- **Interrupts.** `request_threaded_irq(..., IRQF_ONESHOT, "CLDMA_AP")`; the hard
  handler returns `IRQ_WAKE_THREAD`, the thread performs downstream's
  `cldma_irq_work_cb` sequence: L2/L3 status & mask reads, L2 ack, TX_DONE mask
  + per-queue delayed work, RX_DONE mask + per-queue work, `CLDMA_IP_BUSY`
  writeback. IRQ line = `cfg.irq` (GIC SPI 145 in DT).
- **Contexts/locks.** `ring_lock` protects one ring's cursors and skb slots;
  `timeout_lock` protects `txq_active`/`rxq_active` and the HWO publish/resume
  sequence (same division as downstream). Work is per-queue on ordered
  (`max_active = 1`), unbound, mem-reclaim workqueues; queue 4 RX is the
  low-priority logger queue without `WQ_HIGHPRI`.
- **Reset/clear.** `ccci_cldma_start()` → `cldma_hw_config()` (SO_CFG|0x5,
  BUS_CFG|0x02, HPQR=0, checksum off), clear both queue directions, then
  `cldma_hw_start()` (TQSAR/RQSAR, UL/SO start, L2/L3 unmask).
  `ccci_cldma_stop()` issues UL/SO stop, polls `UL_STATUS`/`SO_STATUS` with a
  bounded wait, clears/masks all L2/L3 status, deletes timers, disables the IRQ,
  flushes work, and clears both queue directions.
- **Skb ownership.** TX: on `0` the engine owns the skb and frees it in the
  TX-completion worker; on any negative return (`-EINVAL`, `-EBUSY`, `-EIO`)
  the caller keeps ownership and must free it. RX: the callback receives
  ownership of the skb; if no callback is registered the skb is freed.
- **TX sequence numbers.** `FEATURE_SEQ_CHECK_EN` is enabled on mt6735
  (`mt6735/ccci_config.h:32`), and downstream increments the per-channel TX
  sequence number from the CLDMA send path under the hardware queue lock
  (`modem_cldma.c:2862` -> `ccci_core.h:895-910`). This engine owns
  `tx_seq_num[CCCI_MAX_CH_NUM]`, resets it in `ccci_cldma_start()`
  (`modem_cldma.c:2316`) and assigns `seq_num` while holding `ring_lock`,
  immediately before the skb is mapped, so wire order matches sequence order.
  `assert_bit` is forced to 1 for every channel except `CCCI_RPC_TX` /
  `CCCI_FS_TX`; for those two the core must pre-set the bit because the rule
  depends on the modem boot stage (`ccci_core.h:907-908`), which only the core
  knows. Out-of-range channels (force-assert/monitor) are skipped like
  downstream. Agreed with the core leaf: `ccci_md_send_skb()` leaves `seq_num`
  alone on the CLDMA path and only pre-sets `assert_bit` for RPC/FS. `ccci_cldma_send()` requires `skb->len >= sizeof(struct
  ccci_header)` and returns `-EINVAL` otherwise. RX sequence checking
  (`ccci_core.h:913-933`) stays in the core's RX callback: it needs the IN
  counters, `dump_info()` and `force_assert()`, all of which the frozen CLDMA
  API does not expose. The core leaf confirmed it performs that check in
  `ccci_md_rx_cb()` using the queue number delivered here.
- **Stopped engine.** `ccci_cldma_send()` on a stopped engine still consumes
  the skb into the ring and returns `0` (no resume command), exactly like
  downstream (`modem_cldma.c:2928-2937`); the next `ccci_cldma_stop()` /
  `ccci_cldma_start()` clears and frees it. The core leaf frees the skb itself
  on `-EBUSY`/`-EINVAL`/`-EIO`, matching the ownership rule above.
- **Timeout.** Per-TX-queue timer, armed only for queue 7 (VT), 30 s
  (`modem_cldma.c:117`, `IS_VT_QUE`); expiry logs, dumps the GPD queue and the
  CLDMA register set. Deleted on the queue-7 TX_DONE interrupt.

---

## 5. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| GPD/BD descriptor layout (16 B, flags, links) | `modem_cldma.h:250-300`; ROM `cldma_tx_ring_init@0xc0525078`, `cldma_rx_ring_init@0xc0525414` | absent | `struct cldma_tgpd/rgpd/tbd/rbd/rspd` + init | **done** |
| TX ring GPD, 8 queues | `modem_cldma.c:1033-1092` | absent | `cldma_tx_ring_init()` | **done** |
| TX ring BD (SGIO) queues 3/4/5 | `modem_cldma.c:1059-1090`, `2698-2760` | absent | `cldma_gpd_bd_handle_tx_request()` | **done** |
| TX request fill + doorbell | `modem_cldma.c:2762-2799`, `2908-2915` | absent | `ccci_cldma_send()` + `UL_RESUME_CMD` | **done** |
| TX per-channel seq_num under queue lock | `modem_cldma.c:2862`, `ccci_core.h:895-910`, `mt6735/ccci_config.h:32` | absent | `cldma_tx_seq_inc()` inside `ring_lock`; reset at start | **done** |
| TX assert_bit rule (RPC/FS boot-stage dependent) | `ccci_core.h:902-908` | absent | forced 1 except RPC/FS, which the core pre-sets | **done (core split)** |
| TX completion collect (GPD + BD) | `modem_cldma.c:798-939`; ROM `cldma_gpd_tx_collect@0xc0527870` | absent | `cldma_gpd_tx_collect()`, `cldma_gpd_bd_tx_collect()` | **done** |
| TX full → `-EBUSY`, skb returned | `modem_cldma.c:2941-2990` | absent | `ccci_cldma_send()` | **done** |
| TX while engine stopped (accepted, dropped by next stop) | `modem_cldma.c:2928-2937` | absent | `ccci_cldma_send()` | **done** |
| TX DMA unmap + skb free | `modem_cldma.c:817-819`, `878-905` | absent | `cldma_unmap_tx_request()` + `dev_kfree_skb_any()` | **done** |
| RX ring arm/refill | `modem_cldma.c:333-384`, `531-577`; ROM `cldma_gpd_rx_refill@0xc05207e0` | absent | `cldma_gpd_rx_refill()` | **done** |
| RX collect, normal queues | `modem_cldma.c:386-537` | absent | `cldma_gpd_rx_collect()` | **done** |
| RX collect, net queues | `modem_cldma.c:539-639` | absent | `cldma_gpd_net_rx_collect()` | **done** |
| Net RX push thread | `modem_cldma.c:641-690`; ROM `cldma_net_rx_push_thread@0xc0524c90` | absent | `cldma_net_rx_push_thread()` | **done** |
| RX refill/done workers | `modem_cldma.c:692-796` | absent | `cldma_rx_refill()`, `cldma_rx_done()` | **done** |
| CCCI header intact + queue number to callback | port layer contract | absent | `ccci_cldma_set_rx_cb()` delivery | **done** |
| L2/L3 IRQ ack, mask, dispatch | `modem_cldma.c:1207-1327`; ROM `cldma_irq_work_cb@0xc051e3ac` | absent | `cldma_irq_thread()` | **done** |
| Threaded IRQ on GIC SPI 145 | ROM `cldma_isr@0xc051ead4` | absent | `request_threaded_irq(cfg->irq, ...)` | **done** |
| Queue→ring mapping (normal mode) | `modem_cldma.c:1094-1133`; ROM `cldma_queue_switch_ring@0xc051c924` | absent | `cldma_queue_switch_ring()` | **done** |
| Start sequence | `modem_cldma.c:1569-1617` | absent | `cldma_hw_start()` | **done** |
| Stop + status wait + mask | `modem_cldma.c:1354-1447` | absent | `cldma_hw_stop()` | **done** |
| Engine config (SO_CFG/BUS_CFG/HPQR/checksum) | `modem_cldma.c:1526-1567` | absent | `cldma_hw_config()` | **done** |
| Clear all queues | `modem_cldma.c:1619-1693`; ROM `md_cd_clear_all_queue@0xc0520484` | absent | `cldma_clear_tx_queue()/cldma_clear_rx_queue()` | **done** |
| TX watchdog (queue 7, 30 s) | `modem_cldma.c:308-330`; ROM `cldma_timeout_timer_func@0xc0524bb8` | absent | `cldma_timeout_timer_func()` | **done** |
| GPD queue + register dump | `modem_cldma.c:131-163`; ROM `cldma_dump_gpd_queue@0xc051cb80` | absent | `cldma_dump_gpd_queue()`, `cldma_dump_register()` | **done** |
| 32-bit DMA mask | `modem_cldma.c:3858-3859` | absent | `dma_set_mask_and_coherent(..., DMA_BIT_MASK(32))` | **done** |
| Queue3/4/5 exception-mode ring remap (4K) | `modem_cldma.c:1101-1122` | absent | not reachable through frozen API | **deferred** |
| RGU reset via INFRA_RST0/1 | `mt6735/cldma_platform.c:55-72`; ROM `md_cldma_hw_reset@0xc0530a80` | absent | `cfg` has no INFRA_AO base | **deferred** |
| SPM MD clock request around register access | `modem_cldma.c` `md_cd_lock_cldma_clock_src()` | absent | platform PM, not CLDMA | **deferred** |
| CCIF peer-wakeup doorbell after TX | `modem_cldma.c:2924` `md_cd_ccif_send(AP_MD_PEER_WAKEUP)` | absent | core owns CCIF | **deferred** |
| Force-assert on watchdog expiry | `modem_cldma.c:326-328` (commented out in ROM source) | absent | needs CCIF/port layer | **deferred** |
| Traffic monitor / packet history | `modem_cldma.c:175-274` | absent | diagnostics owned by util/core | **deferred** |
| NAPI hand-off for net RX | `modem_cldma.c:539-639` | absent | frozen API has no NAPI; push thread used | **deferred** |
| MD-side `UL_STATUS` check in RX done | `modem_cldma.c:755-760` | absent | `cfg` has no MD_CLDMA_PDN base | **deferred** |
| RX sequence check (`seq_nums[IN]`, force-assert) | `ccci_core.h:913-933`, `modem_cldma.c:475/577` | absent | core's RX callback (owns IN state + CCIF) | **deferred (core)** |
| AEE / `dump_info` hooks | `modem_cldma.c:1390-1420`, `1986-2038` | absent | no AEE in this tree | **deferred** |
| Blocking send / write_room | `modem_cldma.c:2688-2698` | absent | API is non-blocking; `-EBUSY` instead | **deferred** |
| IoC override from port layer | `modem_cldma.c:2765-2776` | absent | API has no override argument | **deferred** |
| GPD checksum (12/16 B) | `modem_cldma.h:48` (`CHECKSUM_SIZE 0`) | absent | compiled out, same config | **done (disabled)** |
| Secure-boot / firmware auth | core (`ccci_init_security`) | absent | not a CLDMA feature | **n/a** |

---

## 6. Compile test

Exact command (per contract §4 / assignment):

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-cldma"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/ccci_cldma/ccci_cldma.o
```

Result (exit 0):

```
#
# configuration written to .config
#
  SYNC    include/config/auto.conf
  ...
  CC      drivers/misc/mediatek/ccci_cldma/ccci_cldma.o
```

`CONFIG_MTK_CCCI_CLDMA=y` confirmed in `.config`. Additional clean build with
`W=1` produced no warnings. `scripts/checkpatch.pl --no-tree --terse -f` on the
new `.c`/`.h`: 0 errors, 0 warnings. No full kernel build was run.

---

## 7. Inferred / hardware-unverified

- `[INFERENCE]` The ROM ISR runs `cldma_irq_work_cb` synchronously
  (`cldma_isr@0xc051ead4` → `bl cldma_irq_work_cb`, return 1). The threaded IRQ
  reproduces the same "drain in deferred context with the line masked" model but
  the exact register-access context differs from the ROM.
- `[INFERENCE]` `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))` is correct
  because every descriptor address is programmed through a `u32`
  (`lower_32_bits()`), matching downstream's
  `dma_mask = DMA_BIT_MASK(sizeof(unsigned long) * 8)` on 32-bit ARM.
- All register sequences (start/stop/config/IRQ ack) are **hardware-unverified**:
  there is no MT6737T device attached. They are transcribed from the cited
  downstream lines; no timing or value was invented.
- `[INFERENCE]` RX GPD `data_allow_len` is the ring's `pkt_size` (downstream
  passes the skb pool size, `modem_cldma.c:1015`); with
  `__netdev_alloc_skb(pkt_size)` the usable buffer after `NET_SKB_PAD` reservation
  is exactly `pkt_size`, so the modem cannot overrun the mapping.
- The stop poll is bounded (10 000 × 20 µs ≈ 200 ms) instead of downstream's
  effectively unbounded spin with an AEE abort at 1 600 000 iterations. This is
  a deliberate, documented deviation to avoid a multi-second IRQ-off stall; the
  stop sequence itself (commands, status, masks) is identical.
- The refill allocator retries 100 × 20 ms instead of looping forever
  (`modem_cldma.c:362-371`), so a persistent OOM cannot wedge a workqueue.

---

## 8. Deferred downstream features and reasons

1. **Exception-mode ring remap** (`modem_cldma.c:1101-1122`, reached from
   `md_cd_exception()` `HIF_EX_ALLQ_RESET` when `ex_stage == EX_INIT_DONE`).
   The frozen API (`include/linux/ccci.h` §5) has no way to notify the engine
   that the modem entered exception mode, and §5 must stay byte-identical. The
   normal-mode mapping is fully implemented; `normal_*_ring[3]` is left empty
   (queue 3 uses the net ring outside exception mode).
2. **RGU hardware reset** (`md_cldma_hw_reset`, `mt6735/cldma_platform.c:55-72`,
   ROM `0xc0530a80`). Requires the INFRA_AO base (`INFRA_RST0 0x0030`,
   `INFRA_RST1 0x0034`), which is not part of `struct ccci_cldma_cfg`. Using a
   hardcoded physical address would violate the "no hardcoded physical
   addresses" rule. The engine-level reset that downstream performs immediately
   after the RGU reset (`SO_CFG`, `BUS_CFG`, `HPQR`, checksum, start addresses)
   is implemented.
3. **SPM MD clock request** (`md_cd_lock_cldma_clock_src`). That is the
   platform/SCPSYS power-domain layer, not CLDMA; the core/PM driver must hold
   the MD1 clock while `ccci_cldma_start/stop/send` run.
4. **CCIF peer wakeup** (`md_cd_ccif_send(md, AP_MD_PEER_WAKEUP)`,
   `modem_cldma.c:2924`) and **force assert** on watchdog expiry. Both drive the
   CCIF control path, which belongs to the CCCI core leaf.
5. **NAPI RX hand-off.** The frozen CLDMA API has no NAPI/port object, so the
   network queues always use the `cldma_net_rx_push_thread` delivery path
   (downstream's non-NAPI path).
6. **MD-side `UL_STATUS` check in `cldma_rx_done`** (`modem_cldma.c:755-760`).
   Needs `MD_CLDMA_PDN`, which `struct ccci_cldma_cfg` does not expose. The
   AP-side `L2RISAR0` retry is implemented.
7. **Traffic monitor, packet history, dump_info/AEE hooks.** Diagnostics that
   depend on the port layer, `ccci_dump_log`, and AEE, none of which exist in
   this module's API. Basic GPD/register dumps are implemented.
8. **Blocking send / `write_room` and IoC override.** Not part of the frozen
   API; `ccci_cldma_send()` returns `-EBUSY` and always uses the ring's
   configured IOC bit.
9. **`cfg.smem_phys/smem_size`** are stored by the core for the CCIF shared
   memory path; CLDMA itself has no shared-memory window on this SoC (the
   upstream driver never touches it), so the engine does not map them.

---

## 9. DTS-FRAGMENT

Not applied. `arch/arm64/boot/dts/mediatek/mt6735.dtsi` has no `mdcldma` node in
the Nura tree. Suggested fragment for the core's platform driver (matching the
downstream node, adapted to the Nura 2-cell `soc` address space and power-domain
model; the core maps reg 0 as `cfg.ap_base`, reg 2 as `cfg.ap_pdn_base`, and
interrupt 0 as `cfg.irq`). The Nura `soc` node has no `soc:` label, so the overlay
must use `&{/soc}`; `reg` needs the four-cell `<0 ADDR 0 SIZE>` form. Validated by
compiling the board DTS plus this fragment with `scripts/dtc/dtc` (exit 0, no
warning from this node; the unit address is lowercase so `simple_bus_reg` matches
the first `reg` entry).

```dts
&{/soc} {
	mdcldma: mdcldma@1000a000 {
		compatible = "mediatek,mdcldma";
		reg = <0 0x1000A000 0 0x1000>,	/* AP_CLDMA_AO  */
		      <0 0x1000B000 0 0x1000>,	/* MD_CLDMA_AO  */
		      <0 0x1021A000 0 0x1000>,	/* AP_CLDMA_PDN */
		      <0 0x1021B000 0 0x1000>,	/* MD_CLDMA_PDN */
		      <0 0x1020A000 0 0x1000>,	/* AP_CCIF      */
		      <0 0x1020B000 0 0x1000>;	/* MD_CCIF      */
		interrupts = <GIC_SPI 145 IRQ_TYPE_LEVEL_HIGH>,	/* CLDMA */
			     <GIC_SPI 140 IRQ_TYPE_LEVEL_LOW>,	/* CCIF  */
			     <GIC_SPI 221 IRQ_TYPE_EDGE_FALLING>; /* MDWDT */
		mediatek,md_id = <0>;
		mediatek,cldma_capability = <6>;
		mediatek,md_smem_size = <0x30000>;
		power-domains = <&spm MT6735_POWER_DOMAIN_MD1>;
		status = "disabled";
	};
};
```

Notes:
- Downstream used `clocks = <&scpsys SCP_SYS_MD1>`; the Nura mt6735.dtsi models
  MD1 as `power-domain@MT6735_POWER_DOMAIN_MD1` under `&spm`, hence
  `power-domains`.
- `status = "disabled"` because the node is only useful once the CCCI core
  probes; the board DTS can enable it.
- The downstream `&mdcldma` pinctrl states (`vsram_*`, `RFIC0_0*_mode`) are not
  reproduced here; they belong to the core/platform leaf and are not used by
  the CLDMA engine.

---

## 10. Patch

`$M/patches/ccci-cldma.patch`, produced with:

```sh
cd "$M/worktrees/ccci-cldma"
git add -A
git diff --cached --binary > "$M/patches/ccci-cldma.patch"
```

Not committed. Staged file list: the nine files in §1.
