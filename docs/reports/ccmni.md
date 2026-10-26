# CCMNI driver recreation — SM-G532M (MT6737T) / Nura v7.0.10-mt6735

Driver: MediaTek CCMNI (Cross Chip Modem Network Interface), version 0
(`CCMNI_DRV_V0`, the eccci variant). Worktree: `$M/worktrees/ccci-ccmni`.

## 1. Files changed

| File | Change |
|---|---|
| `include/linux/ccci.h` | new, byte-identical to contract §5 (`sha256 bf40af03a917122eb59cd675075e576471ef57925d69c19053e8b7f6fe802df5`, 285 lines; `diff` against an awk extraction of contract lines 85–369 is empty) |
| `drivers/net/ccmni/ccmni.c` | new, 1493 lines — the driver |
| `drivers/net/ccmni/ccmni.h` | new, private header (downstream constants/structures/enums) |
| `drivers/net/ccmni/Kconfig` | new, `config MTK_CCMNI` (bool, no cross-leaf `depends on`) |
| `drivers/net/ccmni/Makefile` | new, `obj-$(CONFIG_MTK_CCMNI) += ccmni.o` |
| `drivers/net/Kconfig` | one line: `source "drivers/net/ccmni/Kconfig"` (inside `if NETDEVICES`) |
| `drivers/net/Makefile` | one line: `obj-$(CONFIG_MTK_CCMNI) += ccmni/` |

`git status --porcelain` in the worktree lists exactly those seven entries; no
other file was touched and there are no build artefacts in the diff.

Patch: `$M/patches/ccmni.patch` (staged `git diff --cached --binary`, 2001 lines).

## 2. ROM evidence

Symbols from `$M/zip/vmlinux.elf` (`llvm-nm`, 32-bit ARM):

| Symbol | Address | Note |
|---|---|---|
| `ccmni_init` | `0xc0507124` | calls `alloc_etherdev_mqs` `0xc05072b8`, `register_netdev` `0xc0507258`, `get_random_bytes` `0xc0507364`, `sprintf` `0xc0507390`, `init_timer_key` `0xc0507400`, `netif_napi_add` `0xc050742c` |
| `ccmni_exit` | `0xc0505774` | |
| `ccmni_open` / `ccmni_close` | `0xc0506484` / `0xc0506250` | |
| `ccmni_start_xmit` | `0xc0506a64` | calls `ipv6_skip_exthdr` `0xc0506cd8`; `consume_skb` `0xc0506dd4`; **no** `netif_stop_queue`/`netif_tx_stop_queue` call anywhere in the function |
| `ccmni_rx_callback` | `0xc0506838` | NAPI path `netif_receive_skb` `0xc050693c`; non-NAPI `netif_rx_ni` `0xc0506a50` / `netif_rx` `0xc0506a58`; `__pm_wakeup_event` `0xc0506968` (after `jiffies_to_msecs` `0xc050695c`); `consume_skb` `0xc05069d0` on invalid ctl |
| `ccmni_md_state_callback` | `0xc0505870` | `mod_timer` `0xc0505910`, `__pm_wakeup_event` `0xc0505934`, `netif_carrier_on` `0xc05059c0`, `netif_carrier_off` `0xc0505a60`, `__napi_schedule` `0xc0505a98`, `netif_tx_wake_queue` `0xc0505aa0` |
| `ccmni_napi_poll` | `0xc0506688` | driver instance |
| `ccmni_napi_poll` | `0xc0510508` | second copy from `eccci/port_net.c`; that one is `return 0` (see downstream `port_net.c:258`) |
| `ccmni_napi_poll_timeout` | `0xc05056ec` | |
| `ccmni_select_queue` | `0xc0505f88` | |
| `ccmni_change_mtu` | `0xc05067d8` | |
| `ccmni_tx_timeout` | `0xc05066ec` | |
| `ccmni_ioctl` | `0xc0505b18` | |
| `ccmni_get_stats` | `0xc0510de8` | from `port_net.c` (3.18 `ndo_get_stats` returning `&dev->stats`) |
| `ccmni_dump` / `ccmni_dump_rx_status` | `0xc05055f0` / `0xc0505730` | |
| `ccmni_debug_file_init` | `0xc0506ff4` | |
| `ccmni_netdev_ops` | `0xc0a8fd50` | single object, referenced by `ccmni_init` → the v0 `ccmni.c` netdev path is the live one |
| `ccmni_dbg_skb_header` | `0xc0a0c214` | plus `ccmni_dbg_tcp_header.part.6` `0xc0a0c16c` |
| `ccmni_send_pkt` / `ccci_get_ccmni_channel` | `0xc0510b68` / `0xc0510748` | core side (`eccci/port_net.c`) |

ROM strings (confirm the source tree is the same as
`$M/src/downstream-grandpplte`): `ccmni_init: ccmni_num=%d, ...`,
`ccmni_exit: unregister ccmni%d dev`, `[IRAT]CCMNI%d=...`, `register netdev name: %s`,
`ccmni/md%d/debug_level fail: %d`, `SIOCSTXQSTATE:`, `SIOCCCMNICFG: %s iRAT MD%d`,
`[TX]CCMNI%d head_len=%d len=%d ack=%d tx_ch=%d`, `[RX]CCMNI%d recv data_len=%d`,
`ccmni%d_tx_timeout: usage_...`, `%s(%d,%d), irat_md=MD%d, r...`.

Downstream references (`$M/src/downstream-grandpplte/drivers/misc/mediatek/`):

* `ccmni/ccmni.c:51-53` globals; `:55-65` `ccmni_make_etherframe()`; `:67-121`
  `is_ack_skb()`; `:124-201` debug header printers; `:204-234`
  `ccmni_debug_file_init()`; `:237-246` `ccmni_select_queue()`; `:248-285`
  `ccmni_open()`; `:287-315` `ccmni_close()`; `:317-389` `ccmni_start_xmit()`
  (TX-busy block `:380-388`); `:391-398` `ccmni_change_mtu()`; `:400-415`
  `ccmni_tx_timeout()`; `:417-534` `ccmni_ioctl()` (SIOCSTXQSTATE / SIOCCCMNICFG);
  `:536-544` `ccmni_netdev_ops`; `:546-558` `ccmni_napi_poll()`; `:560-565`
  NAPI timeout; `:569-592` `ccmni_inst_init()`; `:594-781` `ccmni_init()`
  (netdev creation condition `:634-636`, netdev fields `:649-668`, iRAT block
  `:692-755`, source-instance memcpy `:750-751`, wakelock `:757-758`); `:783-812`
  `ccmni_exit()`; `:814-882` `ccmni_rx_callback()` (Ethernet frame `:833`, NAPI
  delivery `:862-869`); `:884-956` `ccmni_md_state_callback()` (READY `:904-907`,
  EXCEPTION/RESET `:910-913`, TX_IRQ/TX_FULL `:921-955`); `:958-985` `ccmni_dump()`;
  `:987-990` `ccmni_dump_rx_status()`.
* `ccmni/ccmni.h:36-38` MTU/queue/WDT constants; `:40-41` IP versions; `:43-44`
  private ioctls; `:48-55` `struct ccmni_ch`; `:57-73` `struct ccmni_instance`;
  `:87-93` `struct ccmni_ctl_block`; `:113-118` `CCMNI_CH`; `:120-125`
  `CCMNI_ERRNO`; `:127-134` debug levels; `:136-141` TX queues; `:153-160` log macros.
* `eccci/port_net.c:56` `ccmni_get_default_channel()` (channel map);
  `:117` `ccci_get_ccmni_channel()`; `:172` `ccmni_send_pkt()` (CCCI header push,
  `reserved = 0`); `:258` `ccmni_napi_poll()` = `return 0`; `:263-277`
  `eccci_ccmni_ops` (v0, 8 instances, `DATA_ACK_DVD`); `:279-298`
  `eccci_cc3mni_ops` (iRAT + TXBUSY_STOP, 3 instances); `:690-700`
  `port_net_recv_skb()` CCMNI_U path (`skb_pull(ccci_header)` then
  `ccmni_ops.rx_callback`); `:786-806` `port_net_md_state_notice()` mapping
  TX_IRQ/TX_FULL/READY/EXCEPTION/RESET.

## 3. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| init / netdev creation | `ccmni_init` (`ccmni.c:594-781`); 8 instances for MD1, 3 for MD3; `alloc_etherdev[_mqs]`, name `%s%d`, `random_ether_addr`, `mtu=1500`, `tx_queue_len=1000`, `watchdog_timeo=1s`, flags `IFF_NOARP`, `NETIF_F_VLAN_CHALLENGED` (+`SG` if `MODEM_CAP_SGIO`), `header_ops=NULL`, `hard_header_len += sizeof(ccci_header)`, `destructor=free_netdev` | absent | `alloc_netdev_mqs()` + `ether_setup()` setup callback, same fields, `needs_free_netdev=true`, `eth_hw_addr_random()`, `hard_header_len = 14+16 = 30`, `CAP_CCMNI_DISABLE` short-circuits to success | done |
| MQ netdev | 2 tx queues / 1 rx queue when `MODEM_CAP_CCMNI_MQ` | absent | `CCMNI_TXQ_NUM`=2 tx queues / 1 rx queue; `netif_tx_{start,wake,stop}_all_queues` / `netif_tx_disable` used on that path | done |
| open | `ccmni.c:248-285`: refresh `get_ccmni_ch`, start queue(s), NAPI enable+schedule, usage++ (also peer instance) | absent | same, plus error propagation from `get_ccmni_ch`; returns `-ENODEV` if MD not initialised | done |
| close | `ccmni.c:287-315`: usage--, stop/disable queue(s), NAPI disable | absent | same | done |
| xmit pre-checks | `ccmni.c:317-389`: len>mtu → drop(+tx_dropped) `NETDEV_TX_OK`; headroom<`sizeof(ccci_header)` → drop | absent | same checks, counters in per-instance atomic64 stat | done |
| TCP-ACK fast path | `is_ack_skb()` `ccmni.c:67-121`: IPv4/IPv6, len ≤ 128−16, TCP, no SYN/FIN/RST, exact header length; `start_xmit` uses `ch.dl_ack` for ACK when `MODEM_CAP_DATA_ACK_DVD` | absent | same logic, same channels; bounds checks added before reading IP/TCP headers (hardening, behaviourally identical for valid packets) | done |
| select_queue | `ccmni.c:237-246` returns FAST(1) for ACK skbs | absent | returns FAST only when `MODEM_CAP_CCMNI_MQ` (single-queue devices return 0 so the index is always valid) | done |
| TX result mapping | `send_pkt` ret: `MD_NO_READY`/`TX_INVAL` → drop, `tx_dropped`, `tx_busy_cnt=0`, `NETDEV_TX_OK`; `TX_BUSY` → `NETDEV_TX_BUSY`, retry counter (log every 100 without `TXBUSY_STOP`); success → `tx_packets/tx_bytes` | absent | identical; skb ownership: consumed on success, untouched on `TX_BUSY`, freed by driver on drop | done |
| TXBUSY_STOP queue-stop | downstream stops the queue on the core's **TX_FULL** state notification (`ccmni.c:944-953`), not in `start_xmit` | absent | `NETDEV_TX_BUSY` backpressure only. The frozen `enum ccci_md_state` has no TX_FULL/TX_IRQ value, so there is no event to stop/wake a queue on ring-full | partial (see §5) |
| RX frame | `ccmni.c:814-882`: writes `ethhdr` at `skb->data − ETH_HLEN` (dest=dev_addr, src=0, proto from first nibble), `skb_set_mac_header(-ETH_HLEN)`, `protocol`, `ip_summed=CHECKSUM_NONE`, then deliver; `wake_lock_timeout(HZ)`; `dev->stats.rx_*` | absent | same; adds headroom check + `pskb_expand_head(ETH_HLEN)` when a caller provides less than 14 bytes; per-device atomic64 rx stats; `__pm_wakeup_event(ws, jiffies_to_msecs(HZ))` | done |
| RX delivery | `MODEM_CAP_NAPI` → `netif_receive_skb()` (ROM `0xc050693c`), else `netif_rx_ni`/`netif_rx` by context | absent | `netif_receive_skb()` with NAPI capability; otherwise `netif_rx()` (`netif_rx_ni` was removed from the kernel; `netif_rx` is safe in both contexts). NAPI instance is still registered/scheduled and `ops->napi_poll` is called from our poll | done (deliberate ROM parity, see §5) |
| RX state gate | none in `ccmni_rx_callback` (the core gates) | absent | drops (+`rx_dropped`) when the core has reported a state other than `CCCI_MD_READY`; before the first report packets are passed, exactly like downstream | done |
| RX/TX sequence numbers | instance keeps `rx_seq_num`, `tx_seq_num[2]`; only reset on READY (`ccmni.c:904-907`); the wire seq (`ccci_header.reserved`) is written by `ccmni_send_pkt()` as 0 (`port_net.c:229-233`) | absent | counters maintained (reset on READY, ++ per packet) when `CCCI_MD_CAP_CCMNI_SEQNO`; exposed in debugfs. The on-wire `reserved` field remains owned by the core and is not modified by ccmni (same as downstream) | partial (documented) |
| NAPI | `netif_napi_add(dev, &napi, ccmni_napi_poll, napi_poll_weigh)`, `napi_enable/schedule` on open, `napi_disable` on close, per-instance 1 s "lost NAPI polling" timer deleted on every poll | absent | `netif_napi_add_weight()`; weight from `ops->napi_poll_weigh`, falling back to `NAPI_POLL_WEIGHT` when 0 (downstream passes 0 for cc3mni); `timer_setup()` + `timer_delete()`; poll calls `ops->napi_poll(md_id, ch.rx, napi, budget)` | done |
| MD state | `ccmni_md_state_callback()`: READY → carrier on, reset seq; EXCEPTION/RESET → carrier off; RX_IRQ → timer+napi_schedule+wakeup; TX_IRQ/TX_FULL → per-queue stop/wake | absent | READY → carrier on, reset seq/busy, wake queues, napi_schedule (if NAPI registered and running); EXCEPTION/STOPPED/BOOTING/INVALID → carrier off + `netif_tx_stop_all_queues`; NAPI stays enabled (owned by `ndo_stop`, which cannot sleep from the callback context) and RX is gated by state | done (states TX_IRQ/TX_FULL not in contract enum → see §5) |
| ioctl `SIOCSTXQSTATE` | `(SIOCDEVPRIVATE+0)`; `ifru_ivalue[3:0]` start/stop, `[31:16]` watchdog seconds; stop: usage--, stop queue(s), watchdog 60 s or requested; start: usage<=0 → wake if running, watchdog 1 s, usage++ | absent | same, in `ndo_siocdevprivate()` (the kernel no longer routes SIOCDEVPRIVATE through `ndo_do_ioctl`), MQ paths included, peer/sibling instance usage synced | done |
| ioctl `SIOCCCMNICFG` | `(SIOCDEVPRIVATE+1)`; remap this netdev's TX routing to another MD for iRAT | absent | validates md id (downstream's `&&` test is fixed to `||`), target ctlb/num/peer instance; exchanges routing state (md_id, ctlb, channels, usage, tx_busy) with the peer instance instead of memcpy'ing live locks/timers/NAPI | partial (see §5) |
| change_mtu | sets `dev->mtu`, no validation | absent | rejects `< ETH_MIN_MTU`, otherwise sets and logs; upper bound is the core's `dev->max_mtu` (1500 from `ether_setup`) | done |
| tx_timeout | `dev->stats.tx_errors++`; if usage>0 wake queue(s) | absent | `tx_errors++`; same wake logic; logs queue index | done |
| stats | 3.18 `dev->stats` (`ndo_get_stats` in `port_net.c:532`) | absent | `ndo_get_stats64()` from per-instance atomic64 counters populated by the RX/TX paths | done |
| dump / rx_status | `ccmni_dump()` line + `ccmni_dump_rx_status()` copying the 4 `net_rx_delay[]` slots (`ccmni.c:958-990`) | absent | same line (device names, usage, iRAT md, rx/tx packets+bytes, qdisc qlen, tx_busy, running/carrier/queue-stopped) rendered into debugfs; `ccmni_dump_rx_status()` keeps the downstream signature and the 4-slot array is maintained in RX | done |
| debugfs | `ccmni/mdN/debug_level` (u32, 0600) only | absent | `ccmni/mdN/debug_level` plus `ccmni/mdN/ccmniN/{stats,dump,rx_status}`; dentries released in `ccmni_exit()`; root dir created lazily and reused | done |
| wakelock | `wake_lock_init(WAKE_LOCK_SUSPEND, "ccmni_md%d")`, `wake_lock_timeout(HZ)` | absent | `wakeup_source_register(NULL, "ccmni_md%d")` / `wakeup_source_unregister()`, `__pm_wakeup_event(ws, jiffies_to_msecs(HZ))` (exactly what the ROM does at `0xc0506968`) | done |
| iRAT init | `ccmni.c:692-755`: validate irat md, source ctlb, equal/lesser instance count; allocate peer instances that share the source netdevices; duplicate the source's list entries | absent | same validation and shared-device instances (heap, `owns_dev=false`) and duplicated source list entries; `SIOCCCMNICFG`/`SIOCSTXQSTATE` use them for independent per-MD routing | partial (`WORLD_PHONE` variant not cross-linked, see §5) |
| `CCMNI_MD_CAP_WORLD_PHONE` | both modems create netdevs; iRAT mirrors onto the source device | absent | both modems create their own netdevs; instances are not cross-linked (iRAT validation still performed) | partial |
| `CCMNI_MD_CAP_CCMNI_DISABLE` | return 0 without allocating | absent | same | done |
| teardown | `ccmni_exit()` unregisters devices, frees ops/ctl | absent | unregisters/frees, removes debugfs, unregisters wakeup source; additionally clears iRAT peers' device pointers before unregistering a shared device (avoids a use-after-free the downstream code has) | done |
| DTS | none (no register/IRQ/clock use) | none | none | n/a |

## 4. Compile test

Worktree `$M/worktrees/ccci-ccmni`, exact contract command:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
scripts/config -e MTK_CCMNI
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/net/ccmni/ccmni.o
```

Result: `CONFIG_MTK_CCMNI=y` in `.config`; final line `CC drivers/net/ccmni/ccmni.o`,
exit status 0, **no warnings** (the same object also builds clean with `W=1`).

Produced symbols (`llvm-nm drivers/net/ccmni/ccmni.o`), matching the contract API:
`T ccmni_init`, `T ccmni_exit`, `T ccmni_rx_callback`, `T ccmni_md_state_callback`;
local: `ccmni_open`, `ccmni_close`, `ccmni_start_xmit`, `ccmni_select_queue`,
`ccmni_change_mtu`, `ccmni_tx_timeout`, `ccmni_siocdevprivate`, `ccmni_get_stats64`,
`ccmni_napi_poll`, `ccmni_napi_poll_timeout`, `ccmni_dump`, `ccmni_dump_rx_status`,
`ccmni_dbg_skb_header`, `ccmni_debug_file_init`, `ccmni_netdev_ops`.

No full kernel build was run.

## 5. Inferred / unverified / deviation notes

1. **RX skb layout (contract assumption, confirmed with the core leaf):**
   `ccmni_rx_callback()` assumes the core already called
   `skb_pull(sizeof(struct ccci_header))` (downstream `port_net_recv_skb()`,
   `port_net.c:690-700`), i.e. `skb->data` is the IP header and ≥ ETH_HLEN of
   headroom exists. If a caller violates this, the frame would be built over the
   CCCI header. A `pskb_expand_head()` fallback covers the < 14-byte case.
2. **NAPI delivery:** the assignment text says "queue the skb to NAPI"; the ROM at
   `0xc050693c` calls `netif_receive_skb()` for the NAPI-capable path, and the core
   leaf's `ops->napi_poll` returns 0 (`port_net.c:258`). The implementation follows
   the ROM: NAPI instance is registered/enabled/scheduled and polled (calling
   `ops->napi_poll`), while the skb itself goes straight to
   `netif_receive_skb()`. `netif_rx_ni()` no longer exists in this kernel, so the
   non-NAPI path uses `netif_rx()` from either context.
3. **`TXBUSY_STOP`:** the queue-stop half depends on the TX_FULL/TX_IRQ
   notifications, which the frozen `enum ccci_md_state` does not carry. On
   `CCMNI_ERR_TX_BUSY` the driver therefore returns `NETDEV_TX_BUSY` (qdisc
   requeues) and keeps the downstream retry accounting; no queue is stopped in
   `start_xmit`, matching `ccmni.c:380-388`. Queue stop/wake from the core state
   machine is limited to the states the contract defines.
4. **iRAT:** linking, channel lookup and the `SIOCCCMNICFG` routing switch are
   implemented, but the downstream `memcpy(netdev_priv(dev), peer, sizeof)` is
   replaced by exchanging routing fields, because a literal copy duplicates live
   spinlocks, timers and the NAPI context. Observable routing is preserved; the
   exact dual-modem sequencing cannot be verified without a C2K/iRAT-capable
   device. `WORLD_PHONE` + iRAT (two netdev sets mirrored onto one) is not
   cross-linked — each modem keeps its own devices. `SIOCCCMNICFG` is a no-op when
   the peer instance is missing.
5. **Sequence numbers:** `CCCI_MD_CAP_CCMNI_SEQNO` only affects the driver-local
   counters. The CCCI header `reserved` field (the on-wire sequence) is written by
   the core's `send_pkt` and is never touched by ccmni, matching `port_net.c:229-233`.
6. **`is_ack_skb()` hardening:** downstream reads the IP/TCP headers without length
   checks; this version validates `skb->len`, `tot_len`, `ihl`/`doff` and the IPv6
   L4 offset before dereferencing. The ACK predicate itself is unchanged.
7. **NAPI weight 0:** downstream passes `napi_poll_weigh = 0` for `cc3mni`;
   `netif_napi_add_weight()` requires ≥ 1, so 0 falls back to `NAPI_POLL_WEIGHT`.
8. **`ndo_siocdevprivate`:** current kernels dispatch `SIOCDEVPRIVATE..+15`
   through `ndo_siocdevprivate()` (`net/core/dev_ioctl.c:622`), not
   `ndo_do_ioctl()`; the ioctl argument is still taken from
   `ifr->ifr_ifru.ifru_ivalue`, so RILD/thermal clients are unaffected.
9. **Stats:** `dev->stats` (3.18) is replaced by atomic64 counters exported via
   `ndo_get_stats64()`; debugfs shows the same quantities.
10. **No hardware verification of any path** (no phone attached). Behaviour is
    source-level parity only. Register-free driver: no MMIO, IRQ, clock or GPIO.

## 6. DTS-FRAGMENT

None required. CCMNI is a pure network-framing library: it owns no DT node, no
registers, no interrupts, no clocks and no GPIOs. The data path is created at
runtime when the CCCI core calls `ccmni_init(md_id, &ops)`; all hardware
description (CCIF/CLDMA register blocks, IRQs, `md_smem_size`) belongs to the
`mdcldma@1000A000` node consumed by the core/CLDMA leaves. No `arch/arm64/boot/dts`
file was modified.

## 7. Patch

`$M/patches/ccmni.patch` (staged `git add -A && git diff --cached --binary`; no commit).
