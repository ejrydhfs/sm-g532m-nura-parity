# CCCI modem subsystem — coordinator report

Subsystem: CCCI (Cross Chip Communication Interface) modem stack of the Samsung
SM-G532M (`grandpplte`, MT6737T, Android 6.0.1, ROM kernel `3.18.19-14373813`),
recreated on the Nura `v7.0.10-mt6735` arm64 tree.

- Coordinator worktree: `$M/worktrees/ccci-integration` (fresh, base `046883c0c`)
- Leaf worktrees: `$M/worktrees/ccci-{eccci,cldma,util,ccmni,c2k}` (fresh, same base)
- `$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
- Contract used by every leaf: `$M/CCCI_CONTRACT.md`
- No shared DTS was modified; no hardware was available.

## 1. Result

The subsystem is implemented as five new drivers plus one frozen public header,
merged into a single integrated patch that builds and links into `vmlinux`:

| # | Driver | Directory | Config | Status |
|---|---|---|---|---|
| 1 | CCCI core / modem loader | `drivers/misc/mediatek/ccci/` | `MTK_CCCI` | complete control plane; CLDMA data plane delegated to #2 and linked |
| 2 | CLDMA engine | `drivers/misc/mediatek/ccci_cldma/` | `MTK_CCCI_CLDMA` | complete GPD/BD ring engine; exception-mode remap deferred |
| 3 | CCCI util / dump / log | `drivers/misc/mediatek/ccci_util/` | `MTK_CCCI_UTIL` | complete log/dump/image-header services |
| 4 | CCMNI netdev | `drivers/net/ccmni/` | `MTK_CCMNI` | complete netdev/NAPI/ack-split path; iRAT cross-modem linking partial |
| 5 | C2K / dual modem (MD3) | `drivers/misc/mediatek/ccci_c2k/` | `MTK_CCCI_C2K` | complete MD3 CCIF instance, boot sequencing, channel map, IRAT, sysfs |
| — | Public API | `include/linux/ccci.h` | — | 298 lines, frozen contract + MD hardware-ops append |

Integrated patch: `$M/patches/ccci-integration.patch` — 41 files, +14644 lines,
applies cleanly to `$M/src/nura-linux` HEAD (`git apply --check` OK).

## 2. Leaf list and deliverables

| Leaf | Worktree | Patch | Report | Patch size | Leaf compile |
|---|---|---|---|---|---|
| ccci-core (`eccci` + CCIF + loader + ports) | `$M/worktrees/ccci-eccci` | `$M/patches/ccci-eccci.patch` | `$M/logs/ccci-eccci.md` | 17 files, 6493+ | 9/9 objects, rc=0, no warnings |
| ccci-cldma | `$M/worktrees/ccci-cldma` | `$M/patches/ccci-cldma.patch` | `$M/logs/ccci-cldma.md` | 9 files, 2377+ | 1/1 object, rc=0, `W=1` clean |
| ccci-util | `$M/worktrees/ccci-util` | `$M/patches/ccci-util.patch` | `$M/logs/ccci-util.md` | 12 files, 3069+ | 4/4 objects, rc=0, `W=1` clean |
| ccmni | `$M/worktrees/ccci-ccmni` | `$M/patches/ccmni.patch` | `$M/logs/ccmni.md` | 7 files, 1954+ | 1/1 object, rc=0, `W=1` clean |
| c2k (`c2k_ccci`/`dual_ccci` replacement) | `$M/worktrees/ccci-c2k` | `$M/patches/ccci-c2k.patch` | `$M/logs/ccci-c2k.md` | 12 files, 1923+ | 4/4 objects, rc=0, `W=1` clean |

Leaf build logs: `$M/logs/ccci-c2k_build.log` (c2k); the other leaves recorded the
command and result in their reports (all exit 0, `CONFIG_*=y`, no full kernel build).

### Leaf code volume

| Area | Files | Lines |
|---|---|---|
| core (`ccci/*.c`, `ccci_priv.h`) | 10 | 6151 |
| CLDMA | 2 | 2070 |
| util | 5 | 2742 |
| c2k | 5 | 1592 |
| ccmni | 2 | 1640 |

## 3. Integrated architecture

```
                 +-----------------------------+
  user space --- | /dev/ccci_* char ports      |  ccci_port_char.c
                 | /dev/ccci_log, /dev/ccci_dump|  ccci_util_log.c
                 | debugfs ccci/, sysfs mdN/    |  ccci_sysfs.c, ccci_util.c
                 +--------------+--------------+
                                |
  +-----------------------------v-------------------------------+
  | CCCI core: modem objects, state machine, port table/channel |
  | map (port_cfg.c verbatim), RX dispatch, TX queueing, seq     |  ccci_core.c
  | numbers, boot/loader, runtime-data handshake                 |  ccci_boot.c
  +------+--------------------------+---------------------------+
         | control                  | data
  +------v-------+           +-------v--------+         +----------------+
  | CCIF (512 B  |           | CLDMA GPD/BD   |         | CCMNI netdev   |
  | SRAM, ACK/   |           | rings, IRQ     |         | ccmni0..N, NAPI|
  | BUSY, WDT)   |           | (GIC SPI 145)  |         | ack fast path  |
  +------+-------+           +-------+--------+         +--------+-------+
         |                           |                           ^
  +------v---------------------------v---------------------------+
  | platform_driver "mediatek,mdcldma" (MD1)  | platform_driver   |
  | 6 reg windows, 3 IRQs, clock/power domain | "mediatek,ap2c2k_ccif" / mt6735-ccif-md3 (MD3)
  +--------------------------------------------+-------------------+
                                                 |
                                     c2k: MD3 boot sequencing, C2K channel map,
                                     IRAT, SIM-switch mode, /sys/kernel/c2k
```

Cross-driver interfaces are exactly the declarations in `include/linux/ccci.h`:
`ccmni_*` (core → netdev), `ccci_register_modem/unregister/get_state/set_irat`
(platform drivers → core), `ccci_register_md_hw_ops` (MD3 platform sequence →
core), `ccci_cldma_*` (core → engine), `ccci_util_*` + `ccci_util_dump_set_window`
(core → util).

## 4. Integration work (conflicts and resolutions)

Applied in this order: `ccci-util`, `ccci-cldma`, `ccci-c2k`, `ccci-eccci`,
`ccmni`, with `git apply --3way`.

| Conflict | Cause | Resolution |
|---|---|---|
| `drivers/misc/mediatek/Kconfig` (AA) | four leaves each created the file with only their own `source` line | merged to one menu with `ccci/`, `ccci_c2k/`, `ccci_cldma/`, `ccci_util/` |
| `drivers/misc/mediatek/Makefile` (AA) | same, `obj-y +=` lines | merged to four `obj-y +=` lines |
| `include/linux/ccci.h` (5× add) | every leaf carried the frozen header verbatim; core and c2k appended `struct ccci_md_hw_ops` with identical semantics but different comments/whitespace | kept one canonical copy (core's text, 298 lines, contract §5 + the approved hw-ops append); all other copies dropped |
| `drivers/misc/{Kconfig,Makefile}`, `drivers/net/{Kconfig,Makefile}` | identical one-line hooks | applied cleanly; verified exactly one hook line each (no duplicates) |

Coordinator-owned integration fixes (not leaf edits):

1. `MTK_CCCI` now `select`s `MTK_CCCI_CLDMA`, `MTK_CCCI_UTIL` **and `MTK_CCMNI`**.
   The core calls `ccmni_init/exit/rx_callback/md_state_callback` and
   `ccci_cldma_*`/`ccci_util_*` unconditionally, so those objects must be part of
   the build; this also mirrors the ROM, where `ccmni` is linked inside the eccci
   module (`drivers/misc/mediatek/ccmni/Makefile`: `obj-y := eccmni.o`).
   Verified: `scripts/config -d MTK_CCMNI` + `olddefconfig` still yields
   `CONFIG_MTK_CCMNI=y`.
2. `MTK_CCCI_C2K` and `MTK_CCMNI` now `depends on MTK_CCCI` (they consume the
   core API; neither can be built alone).
3. `ccci_register_modem()` returns `-EPROBE_DEFER` while `ccci_class` is not yet
   created. The MD3 platform driver is a separate `module_platform_driver()`; if
   it probes before the core initcall, `device_add()` would run without the CCCI
   class. Link order currently makes the core run first, but the guard removes the
   ordering assumption. Verified in the object: `mov w0, #-0x205` (‑517) on the
   uninitialised path.

No leaf driver logic was altered; all changes above are Kconfig wiring, the
single merged header, and the 4-line probe-defer guard.

## 5. Compile / test evidence

All commands run in `$M/worktrees/ccci-integration` with
`export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`.

### 5.1 Configuration

```
cp $M/nura.config .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K
make ARCH=arm64 LLVM=1 olddefconfig        -> rc=0
CONFIG_MTK_CCCI=y CONFIG_MTK_CCCI_CLDMA=y CONFIG_MTK_CCCI_UTIL=y
CONFIG_MTK_CCMNI=y CONFIG_MTK_CCCI_C2K=y
```
Log: `$M/logs/ccci-integration_olddefconfig.log`

### 5.2 Subsystem objects (19 objects)

```
make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/ccci/{ccci_core,ccci_platform,
  ccci_ccif,ccci_boot,ccci_port_char,ccci_port_net,ccci_port_kernel,ccci_ringbuf,
  ccci_sysfs}.o drivers/misc/mediatek/ccci_cldma/ccci_cldma.o
  drivers/misc/mediatek/ccci_util/{ccci_util,ccci_util_dump,ccci_util_img,
  ccci_util_log}.o drivers/misc/mediatek/ccci_c2k/{ccci_c2k,ccci_c2k_hw,
  ccci_c2k_ch,ccci_c2k_sysfs}.o drivers/net/ccmni/ccmni.o
```
Result: rc=0, 19/19 objects produced, no `warning:`/`error:` lines.
Log: `$M/logs/ccci-integration_objects.log`

Directory archives (`built-in.a`) also build: rc=0,
log `$M/logs/ccci-integration_dirs.log`.

### 5.3 Link test (full kernel)

```
make ARCH=arm64 LLVM=1 AR=ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump \
     READELF=llvm-readelf STRIP=llvm-strip -j8 vmlinux
```
- first run: rc=0 in 476 s — `$M/logs/ccci-integration_full_build.log`
- after the Kconfig/select fix: rc=0 — `$M/logs/ccci-integration_full_build2.log`
- after the probe-defer guard (forced `ccci_core.o` rebuild): rc=0, `vmlinux`,
  `System.map`, `modules.builtin` regenerated —
  `$M/logs/ccci-integration_full_build3.log`

Linked symbols verified in `vmlinux`: `ccci_mdcldma_probe`, `ccci_c2k_probe`,
`ccci_register_modem`, `ccci_cldma_create`, `ccci_util_log`,
`ccci_boot_load_firmware`, `ccmni_init`.

### 5.4 Symbol-consistency audit

Across the 19 objects: no unresolved `ccci_*`/`ccmni_*`/`cldma_*`/`c2k_*` symbol,
and no duplicate global definition. (Undefined cross-leaf symbols would only have
shown up at the vmlinux link; both checks pass.)

## 6. ROM / downstream evidence base

Full citations are in each leaf report; the load-bearing evidence:

- ROM symbols (`$M/zip/symbols.txt`): `ccci_modem_probe/remove/shutdown/suspend/
  resume`, `ccci_modem_pm_ops`, `ccci_subsys_bm_init`, `ccci_subsys_char_init`,
  `ccci_subsys_kernel_init`, `ccmni_init/exit/open/close/start_xmit/
  rx_callback/md_state_callback/napi_poll/select_queue/change_mtu/tx_timeout/
  ioctl/get_stats`, `ccci_get_ccmni_channel`, `ccmni_send_pkt`,
  `md_cd_ccif_isr`, `md_ccif_irq_tasklet`, `ccif_enable_irq`, `ccif_disable_irq`,
  `cldma_isr`, `cldma_tx_ring_init`, `cldma_rx_ring_init`,
  `cldma_gpd_handle_tx_request`, `cldma_gpd_tx_collect`, `cldma_gpd_rx_collect`,
  `cldma_gpd_rx_refill`, `cldma_net_rx_push_thread`, `ccci_util_fo_init`,
  `ccci_util_mem_dump`, `ccci_log_init`, `ccci_dump_write`,
  `ccci_load_firmware`, `ccci_get_md_check_hdr_inf`.
- ROM symbols **absent**: no `modem_sdio`, `cbp_sdio`, `rawbulk`; only one `c2k`
  symbol (`mtk_wdt_set_c2k_sysrst`). This is why `c2k_ccci`'s SDIO/CBP transport
  and the legacy `dual_ccci` `/dev/ccci_*` ABI are not recreated: they are not in
  this ROM build. MD3 is a second CCIF modem instance.
- ROM strings: `md1img`, `md3img`, `mediatek,ap2c2k_ccif`, `opt_eccci_c2k`,
  `[MTK_ECCCI_C2K]:1`, `C2K EE`, `c2k status ind`, and the eccci source names
  (`ccci_core.c`, `ccci_bm.c`, `ccci_support.c`, `port_char.c`, `port_net.c`,
  `port_kernel.c`, `port_ipc.c`, `ccci_private_log.c`).
- ROM DTB (`$M/zip/rom_dtb_0.dts`): `mdc2k@3a00b01c` (`reg` = C2K chip id +
  MD1/MD3 PCCIF windows, `SPI 229` C2K WDT, `scp-sys-md2-main` clock),
  `simswitch` node with `sim1_md3_sim2_md1`.
- Downstream: `drivers/misc/mediatek/eccci/*` (core/ports/CCIF/CLDMA/platform),
  `ccci_util/*`, `ccmni/*`, `mt6735/{ccci_platform.c,ccif_c2k_platform.c,
  modem_reg_base.h,cldma_reg.h}`, `include/mt-plat/mt_ccci_common.h`, and
  `arch/arm64/boot/dts/mt6735.dtsi` node `mdcldma@1000A000` (register windows,
  IRQs, capabilities).
- Real firmware header validation: `md1rom.img` from the ROM CP tarball carries a
  valid v3 `CHECK_HEADER` (`hdr 3, release, lwg, MT6735_S00, mem 0x04d00000,
  img 0x00e00000`); the util parser was validated against it
  (`$M/logs/ccci-util.md` §2).

## 7. Parity matrix (condensed; full matrices in the leaf reports)

| Driver | Done | Partial | Deferred |
|---|---|---|---|
| core (`eccci`) | modem object/state machine; MD1+MD3 port & channel tables verbatim; RX dispatch; TX queueing; CCIF 512 B SRAM ACK/BUSY handshake, threaded IRQ + tasklets, `ccif_{enable,disable}_irq`; MD watchdog IRQ; boot: `request_firmware(md1img/md3img)`, v1–v5 `CHECK_HEADER` validation, image copy into DT reserved region, boot vector/key/enable, runtime-data handshake, boot timeouts; char ports with ROM minors and ioctls; net port bridging to CCMNI; system/kernel channel threads (`MD_INIT_START_BOOT`, `MD_EX*`, `MD_GET_BATTERY_INFO`, `MD_WDT_MONITOR`, thermal/DVFS messages); `exec_ccci_kern_func_by_md_id`, `register_ccci_sys_call_back`, `ccci_get_md_boot_count`; per-MD sysfs + debugfs | — | RPC kernel services (no MTK GPIO/ADC/EINT/time providers in tree); global `/sys/kernel/ccci` attrs; MD security/MPU/RMPU programming (masp/EMI-MPU absent); PMIC VLTE/VSEL, SPM `md_src`, RF clock-buffer pinctrl, `spm_is_md1_sleep`; AEE/SBP/DHL dumps and MD sleep-history; LK/DA memory-table parsing (replaced by DT `memory-region`); IPC kernel handshake; header v6 (`-EOPNOTSUPP`) |
| CLDMA | GPD/BD ring alloc (`dma_pool`), TX request + collect, RX arm/refill/collect, normal + net push-thread delivery, threaded IRQ with L2/L3 ack/mask, start/stop/clear/config, 30 s queue-7 watchdog with dumps, seq-num ownership per channel (downstream rule), `-EBUSY` ownership semantics | normal-mode ring switch only | exception-mode 4K ring remap (no EX_INIT_DONE in the frozen API); RGU reset via INFRA_RST0/1 (INFRA_AO base not in `ccci_cldma_cfg`); SPM MD clock request (power-domain layer); CCIF peer wakeup / force-assert (core); NAPI hand-off (API has no NAPI); MD-side `UL_STATUS` check (no MD_CLDMA_PDN); traffic monitor / packet history / AEE hooks; blocking send / `write_room` |
| util | contract API (`ccci_util_init/exit`, `log_init/exit`, `log`, `dump_mem`); 4 KiB per-MD log ring with timestamps/overflow; 6 dump buffers per MD; `/dev/ccci_log`, `/dev/ccci_dump` (blocking/`O_NONBLOCK`/poll/raw-record write/SET_MD ioctl); hex/word/compact dumps; debugfs `ccci/mdN/{log,dump,log_stats}`, `ccci/dump`, `ccci/dump_window`; MD check-header v3/v4/v5 parser + info string; `ccci_event_log`; ROM `ccci_dump_write`/`ccci_log_write*`/`ccci_log_init` parity | smem dump window must be registered at runtime (`ccci_util_dump_set_window`) | block-partition file I/O (`ccci_util_fo_init`/raw size) replaced by core `request_firmware`; raw-image `ccci_load_firmware` path; secure-image authentication (`ccci_init_security`); meta/opt parsing; `/sys/kernel/ccci` globals; v1/v2/v6 headers |
| ccmni | `ccmni_init/exit/rx_callback/md_state_callback`; `alloc_netdev_mqs` (MQ when capable); open/stop/xmit/select_queue/change_mtu/tx_timeout/get_stats64/siocdevprivate; `hard_header_len` includes `struct ccci_header`; TCP-ACK fast path to the dl-ack channel; TX-busy backpressure; RX Ethernet frame build (IPv4/IPv6), seq counters, state-gated drop with stats; NAPI + poll timeout; wakeup_source instead of wake_lock; debugfs stats/dump/rx_status; iRAT netdev linking | TXBUSY_STOP queue-stop notifications (not expressible with the frozen state enum); iRAT `SIOCCCMNICFG` swaps routing instead of copying live locks/timers; WORLD_PHONE keeps per-modem netdevs; seq counters driver-local (on-wire header owned by core) | — |
| c2k | MD3 as second CCIF instance (`mediatek,ap2c2k_ccif` / `mediatek,mt6735-ccif-md3`); `ccci_md_cfg` fill (SYS3, md3img, boot-slave triples, RGU 0x30050000, 512 B SRAM, IRAT+world-phone caps); boot sequencing (C2K_CONFIG boot mode + ETS_SEL, C2K_SPM_CTRL srcclkena, SLEEP_CLK_CON, TOP_RGU SWSYSRST bit15/key 0x88000000, SPM wake pulse + C2K_STATUS[1] poll, chip-id, VTCXO_1 regulator); C2K channel map (control/status/audio/net1-3+dl_ack/mdlog/fs/ppp/AT/AT2/AT3/agps); IRAT via `ccci_md_set_irat`; SIM-switch mode store/get; `/sys/kernel/c2k` + debugfs | MD3 watchdog IRQ parsed/reported but not requested (`ccci_md_cfg` has one IRQ); PMIC VTCXO_1 uses the regulator instead of the exact 0xA12E register value; `CCCI_MD_MSG_CFG_UPDATE` monitor message not sent | `scp-sys-md2-main` clock (no SCP_SYS_MD2 provider in the Nura clock/SCP driver); ETS_SEL boot-mode source (DT bool instead); `CCCI_IOC_RESET_MD1_MD3_PCCIF` side effect; CBP/SDIO transport and legacy `dual_ccci` ABI (not in ROM) |

## 8. Unresolved gaps (aggregated)

Ordered by impact for real hardware:

1. **C2K/MD3 power and clock providers.** `scp-sys-md2-main` does not exist in the
   Nura MT6735 clock/SCP driver (only `POWER_DOMAIN_MD1`). MD3 power-on therefore
   relies on the C2K register sequence plus the optional clock; the DT fragment
   leaves the clock commented out. Needs shared clock/scpsys work.
2. **Modem memory map.** The modem image base and the AP↔MD shared-memory
   carve-outs are not in any available source; both leaves require DT
   `memory-region` and fail cleanly (`-ENODEV`) without them. The DTS fragments
   contain the node shape with `FILL` markers, no invented addresses.
3. **Security/MPU programming** (`ccci_init_security`, RMPU region permissions).
   Headers are parsed, permissions are not programmed: the masp/EMI-MPU APIs do
   not exist in this tree.
4. **CLDMA exception-mode ring remap** and **RGU reset**: both need either a
   callback in the frozen API or the INFRA_AO base, which the CLDMA API does not
   expose. Normal-mode operation is complete.
5. **Kernel RPC services** (GPIO/ADC/EINT/time), AEE/SBP/DHL dumps, MD
   sleep-history, traffic monitor/packet history: their users do not exist in this
   tree; messages are logged and dropped rather than answered with fabricated data.
6. **Global `/sys/kernel/ccci` attributes** (`boot`, `md_en`, `version`, `md_chn`,
   `dump_max`, `lk_md`, `debug`, `ft_info`): not implemented by any leaf (core owns
   only per-modem attributes, util declined them). This is a real parity gap for
   userspace that reads those files; it is the first item to add if needed.
7. **iRAT / WORLD_PHONE** linking is functional at the netdev level but the
   downstream in-place routing copy is not reproduced; both modems keep their own
   netdevs.
8. **Header v6 images** rejected with `-EOPNOTSUPP` (ROM supports up to v5).

## 9. Hardware-unverified

No device is attached; every register write, IRQ and timing below is
source-derived only.

- CCIF handshake timing (1 ms busy/ack timeout) and the 20 s/10 s boot/exception
  timeouts are downstream constants, not measured.
- The CLDMA ring sizes, queue-to-ring map and doorbell bits come from
  `mt6735/cldma_reg.h` / `modem_cldma.c`; no packet was ever transmitted.
- MD3 CCIF IRQ (`GIC_SPI 139`, level high) is `[INFERENCE]` from
  `mt6735m.dtsi`'s `AP_CCIF1@0x10218000`; the ROM DTB only proves the C2K
  watchdog interrupt (`SPI 229`, edge falling).
- MD3 boot-slave writes conflict with a downstream comment claiming MD3 has no
  boot slave (`ccif_c2k_platform.c:188-193`); the implementation follows the ROM
  memory map (`modem_reg_base.h`) and documents the conflict.
- C2K wake timeout (1 s) is a robustness choice, not a hardware constant.
- `[INFERENCE]` Firmware names `md1img`/`md3img` are the ROM strings, but the
  actual firmware partition/`request_firmware` search path for Android 6.0.1 is
  `/system/etc/firmware/` (`vmlinux.strings`); no blob is embedded.
- CCMNI ACK fast-path classification (`is_ack_skb`) adds bounds hardening absent
  downstream; behaviour on real TCP streams unverified.

## 10. DTS fragments

Not applied. Exact fragments live in:

- MD1 `mdcldma@1000A000` (regs, IRQs, `md_id`, `cldma_capability`,
  `md_smem_size`, `firmware-name`, power domain) and the reserved-memory
  `md1-image`/`md_smem` carve-outs: `$M/logs/ccci-eccci.md` §10 and
  `$M/logs/ccci-cldma.md` §9.
- MD3 `ccif_md3` (compatible `mediatek,ap2c2k_ccif` / `mediatek,mt6735-ccif-md3`,
  `ap-ccif`/`md1-pccif`/`md3-pccif`/chip-id/boot-vector*/`md-rgu` windows, CCIF and
  MD-WDT interrupts, `cell-index = <2>`, `ccif,capability`, `vtcxo1-supply`):
  `$M/logs/ccci-c2k.md` §9.
- ccmni and util need no DT node.

Tree-specific constraints found while validating the fragments: the Nura
`mt6735.dtsi` `soc` node has **no `soc:` label**, so an overlay must use
`&{/soc} { … }` (`&soc` fails with "Label or path soc not found"), and the node is
`#address-cells = <2>`/`#size-cells = <2>`, so every `reg` entry needs the
four-cell `<0 ADDR 0 SIZE>` form, with a lowercase unit address
(`mdcldma@1000a000`) so `simple_bus_reg` matches the first `reg` entry. Both MD1
and MD3 fragments were compiled into DTBs with the tree's `scripts/dtc/dtc`
(exit 0; the only warning is the pre-existing `/soc/interrupt-controller@10220000`
one from `mt6735.dtsi`, no warning from the added nodes; `SPI 139`/`SPI 229`
decode as expected). The `ccci-eccci.md` and `ccci-cldma.md` fragments were
corrected to `&{/soc}` and the lowercase name accordingly; the c2k fragment
already used the path form.

Board files (`arch/arm64/boot/dts/mediatek/mt6735.dtsi`,
`mt6737t-samsung-grandpplte.dts`) were not modified by any leaf or by integration.

## 11. Enabling

```
CONFIG_MTK_CCCI=y        # core; selects CLDMA, UTIL and CCMNI
CONFIG_MTK_CCCI_C2K=y    # optional MD3/dual-modem instance
```
`MTK_CCMNI` and `MTK_CCCI_C2K` depend on `MTK_CCCI`; `MTK_CCCI` depends on
`ARCH_MEDIATEK || COMPILE_TEST`, `OF`, `FW_LOADER`.

Modem firmware (`md1img`, `md3img`) is external and must be present in the
firmware search path; nothing is embedded.

## 12. Patch set

| Patch | Content | State |
|---|---|---|
| `$M/patches/ccci-eccci.patch` | leaf: core, CCIF, boot/loader, ports, sysfs | as delivered by leaf |
| `$M/patches/ccci-cldma.patch` | leaf: CLDMA engine | as delivered by leaf |
| `$M/patches/ccci-util.patch` | leaf: util/log/dump/image header | as delivered by leaf |
| `$M/patches/ccmni.patch` (identical alias: `$M/patches/ccci-ccmni.patch`) | leaf: CCMNI netdev | as delivered by leaf |
| `$M/patches/ccci-c2k.patch` | leaf: MD3/dual modem | as delivered by leaf |
| **`$M/patches/ccci-integration.patch`** | **merged, conflict-resolved, Kconfig-wired, build-verified** | **deliverable of record** |

The five leaf patches are the reviewable units (each applies to pristine HEAD on
its own); the integration patch is the version that compiles and links with the
whole subsystem enabled.

## 13. Report index

Leaf reports: `$M/logs/ccci-eccci.md`, `ccci-cldma.md`, `ccci-util.md`,
`ccmni.md`, `ccci-c2k.md`.
Integration logs: `ccci-integration_olddefconfig.log`,
`ccci-integration_objects.log`, `ccci-integration_dirs.log`,
`ccci-integration_full_build.log`, `ccci-integration_full_build2.log`,
`ccci-integration_full_build3.log`.
