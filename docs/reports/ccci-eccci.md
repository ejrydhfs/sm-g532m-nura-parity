# ccci-eccci — CCCI core + modem loader (eccci) port to the Nura v7.0.10-mt6735 arm64 tree

Worktree: `$M/worktrees/ccci-eccci`
Patch: `$M/patches/ccci-eccci.patch`
Report: `$M/logs/ccci-eccci.md`
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Files changed

New files (all owned by this leaf):

| File | Lines | Purpose |
|---|---|---|
| `include/linux/ccci.h` | 298 | Frozen shared header, byte-identical to contract §5 plus the required `struct ccci_md_hw_ops` addendum appended before `#endif` |
| `drivers/misc/mediatek/ccci/ccci_priv.h` | 782 | Private definitions: registers, channels, messages, ioctl numbers, MD/port/CCIF/boot/ring structures, internal API |
| `drivers/misc/mediatek/ccci/ccci_core.c` | 1265 | Modem object, registry, state machine, port table (MD1+MD3), Rx dispatch, Tx queueing, seq numbers, kernel-service helpers, module init |
| `drivers/misc/mediatek/ccci/ccci_platform.c` | 366 | `platform_driver` for `mediatek,mdcldma`, DT parse, regs/IRQs, clock/genpd, CLDMA engine creation, PM ops, remove/shutdown |
| `drivers/misc/mediatek/ccci/ccci_ccif.c` | 784 | CCIF control protocol, 512-byte SRAM window, busy/ack with timeout, threaded IRQ + tasklet Rx, CCIF-ring transport, MD WDT |
| `drivers/misc/mediatek/ccci/ccci_boot.c` | 918 | `md1img`/`md3img` request_firmware, `CHECK_HEADER` v1–v5 parse/validate/copy, boot vector/key/enable, runtime-data handshake, boot watchdog |
| `drivers/misc/mediatek/ccci/ccci_port_char.c` | 577 | Per-port char/misc device nodes, read/write/poll, ROM ioctl subset |
| `drivers/misc/mediatek/ccci/ccci_port_net.c` | 369 | CCMNI binding: `send_pkt`, `napi_poll`, `get_ccmni_ch`, Rx into `ccmni_rx_callback`, md-state callback |
| `drivers/misc/mediatek/ccci/ccci_port_kernel.c` | 434 | Kernel ports (control/system/status/RPC), `exec_ccci_kern_func_by_md_id`, `register_ccci_sys_call_back`, `ccci_get_md_boot_count` |
| `drivers/misc/mediatek/ccci/ccci_ringbuf.c` | 290 | Framed shared-memory ring buffer (downstream wire format), MMIO-safe |
| `drivers/misc/mediatek/ccci/ccci_sysfs.c` | 357 | Per-modem sysfs attributes + debugfs `ccci/mdN/` |
| `drivers/misc/mediatek/ccci/Kconfig`, `Makefile` | 22 + 15 | `MTK_CCCI` (bool), composite `ccci.o` |

Shared parents touched with exactly one hook line each (integration resolves conflicts):

* `drivers/misc/Kconfig` — `source "drivers/misc/mediatek/Kconfig"` before `endmenu`
* `drivers/misc/Makefile` — `obj-y += mediatek/`
* `drivers/misc/mediatek/Kconfig` (new) — `menu "MediaTek device drivers"` + one `source`
* `drivers/misc/mediatek/Makefile` (new) — `obj-y += ccci/`

`git status --porcelain` in the worktree lists only these files (`M drivers/misc/Kconfig`, `M drivers/misc/Makefile`, `?? drivers/misc/mediatek/`, `?? include/linux/ccci.h`). No DTS, no `nura.config`, no other leaf's files.

## 2. ROM evidence

ROM kernel: `$M/zip/vmlinux.elf` / `$M/zip/symbols.txt` / `$M/zip/vmlinux.strings`.

Symbols present (line numbers in `$M/zip/symbols.txt`), all matched by an implementation in this patch:

| ROM symbol | symbols.txt | Implemented as |
|---|---|---|
| `ccci_modem_probe` | 21256 | `ccci_mdcldma_probe()` (ccci_platform.c) |
| `ccci_modem_remove` / `_shutdown` / `_suspend` / `_resume` | 21330–21333 | `.remove/.shutdown/.driver.pm.suspend/.resume` |
| `ccci_modem_pm_ops` | 44652 | `ccci_modem_pm_ops` (dev_pm_ops, + `restore_noirq`) |
| `ccci_subsys_char_init` | 21140 | `ccci_port_char_init()` |
| `ccci_subsys_kernel_init` | 21173 | `ccci_kernel_port_init()` per kernel port |
| `ccci_register_modem` / `ccci_allocate_modem` | 21099 / 21098 | `ccci_register_modem()` / `ccci_md_alloc()` |
| `exec_ccci_kern_func_by_md_id` | 21107 | same name |
| `register_ccci_sys_call_back` | 21161 | same name |
| `ccci_get_md_boot_count` | 21092 | `ccci_get_md_boot_count()` |
| `ccci_get_ccmni_channel` / `ccmni_send_pkt` | 21144 / 21147 | `ccci_get_ccmni_channel()` / `ccci_ccmni_send_pkt()` |
| `md_cd_ccif_isr` | 21281 | `ccci_ccif_isr()` (+ threaded `ccci_ccif_thread()`) |
| `md_ccif_irq_tasklet` | 21273 | `ccci_ccif_irq_task()` (tasklet) |
| `ccif_enable_irq` / `ccif_disable_irq` | 21271 / 21272 | `ccif_enable_irq()` / `ccif_disable_irq()` |
| `md_cd_wdt_isr` | 21274 | `ccci_ccif_wdt_isr()` + `ccci_ccif_wdt_work()` |
| `md_cd_power_on` / `md_cd_let_md_go` / `md_cd_send_runtime_data` | 21324 / 21326 / 21247 | `ccci_boot_power_on()` / `ccci_boot_let_md_go()` / `ccci_boot_send_runtime_data()` |
| `ccci_load_firmware` | 21014 | `ccci_boot_load_firmware()` (file-static, no symbol clash with ccci-util) |
| `ccci_md_mem_reserve` | 21008 | replaced by DT reserved-memory parsing in `ccci_boot_smem_setup()` (documented) |

ROM strings: `md1img` and `md3img` at `$M/zip/vmlinux.strings:37080` and `:37084` — the default firmware names in this driver. `ccci_md_exception` (`:4129`) is the sysfs attribute name reproduced per-modem.

ROM absence proof: `grep -n c2k zip/symbols.txt` matches only `mtk_wdt_set_c2k_sysrst`; there is no SDIO/CBP C2K driver in the ROM build, and `md_cd_*` (MD1) plus the CCIF instance are the whole modem stack, consistent with `CCCI_CONTRACT.md` §1.

## 3. Downstream references (source of each behaviour)

Paths relative to `$M/src/downstream-grandpplte/drivers/misc/mediatek/`.

| Behaviour | Reference |
|---|---|
| Port table, minors, queues, flags | `eccci/port_cfg.c:23-70` (MD1), `:127-155` (MD3) |
| Port/channel structure, seq helpers | `eccci/ccci_core.h:370-470`, `:890-935` |
| Rx dispatch by channel | `eccci/ccci_core.c:70-135` (`ccci_port_recv_request`) |
| CCIF register offsets | `eccci/mt6735/ccif_c2k_platform.h:46-52` |
| CCIF init/ACK/SRAM clear | `eccci/modem_ccif.c:1043-1065` (`md_ccif_hw_init`) |
| CCIF ISR / tasklet / SRAM msg | `eccci/modem_ccif.c:420-500`, `:152-200` (`md_ccif_sram_rx_work`) |
| CCIF channel numbers, ring base, SRAM ch | `eccci/modem_ccif.c:58-102` |
| CCIF ring sizes | `eccci/modem_ccif.c:105-112`, `eccci/modem_ccif_c2k.c:146-158` |
| Ring placement in smem | `eccci/modem_ccif.c:1119` (`CCIF_MD_SMEM_RESERVE`), `eccci/modem_ccif_c2k.c:1440` |
| Ring wire format | `eccci/ccci_ringbuf.c` (whole file), `ccci_ringbuf.h:22-43` |
| CLDMA exception state machine | `eccci/modem_cldma.c:1986-2088` (`md_cd_exception`, `polling_ready`, `md_cd_ccif_work`) |
| WDT handler | `eccci/modem_cldma.c:1838-1930` |
| `ccif_enable_irq`/`disable_irq` | `eccci/modem_cldma.c:1771-1796` |
| Tx seq ownership (CLDMA) | `eccci/modem_cldma.c:2862`; ring: `eccci/modem_ccif.c:671` |
| Boot vector values | `eccci/mt6735/modem_reg_base.h:26-31`, `eccci/mt6735/cldma_platform.c:328-340` |
| Power-on clock + WDT disable | `eccci/mt6735/cldma_platform.c:234-326` |
| Boot timeout 20 s (CLDMA) / 10 s (CCIF) | `eccci/modem_cldma.c:119`, `eccci/modem_ccif.c:58` |
| Firmware name/format, header parse/copy | `ccci_util/ccci_util_lib_load_img.c:651-905`, `:102-284`, `:508-650` |
| Check header structs v3/v4/v5 | `ccci_util/ccci_util_lib_main.h:15-95` |
| Runtime data (v2 + SRAM legacy) | `eccci/port_kernel.c:147-460` (`prepare_runtime_data`), `eccci/modem_cldma.c:3076-3250`, `eccci/modem_ccif.c:840-1010` |
| smem layout constants | `eccci/mt6735/ccci_config.h:69-96` |
| char ports, ioctl table | `eccci/port_char.c:178-560`, `:558-1100`, `:1177-1349` |
| net port / ccmni ops | `eccci/port_net.c:42-330`, `:570-869` |
| kernel ports, handlers | `eccci/port_kernel.c:501-770`, `:2073-2200` |
| ioctl numbers, message ids | `include/mt-plat/mt_ccci_common.h:198-257`, `:470-700` |
| sysfs attribute set | `ccci_util/ccci_util_lib_sys.c:142-232` (global `/sys/kernel/ccci`; see deferred) |

GPL notices: every file carries `SPDX-License-Identifier: GPL-2.0` plus a "Ported from the Samsung/MTK 3.18 downstream kernel … Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0" header.

## 4. Parity matrix

MD1 is CLDMA data + CCIF control; MD3 is CCIF rings. Downstream state names are mapped onto the frozen `enum ccci_md_state`.

| Behaviour | ROM / downstream | Nura before | After (this patch) | Status |
|---|---|---|---|---|
| Modem probe/remove/shutdown, PM | `ccci_modem_probe` 21256, `modem_cldma.c:3750+` | absent | `ccci_platform.c` probe/remove/shutdown + dev_pm_ops (suspend/resume/restore_noirq) | done |
| Modem object, registry | `ccci_allocate_modem` 21098 / `ccci_register_modem` 21099 | absent | `ccci_md_register()`/`ccci_register_modem()`, per-MD `struct ccci_md`, class device `mdN` | done |
| State machine | `MD_STATE` incl. GATED/RESET/BOOT_FAIL (`mt_ccci_common.h:649-660`) | absent | `CCCI_MD_INVALID/BOOTING/READY/EXCEPTION/STOPPED`; GATED/RESET → STOPPED, TX/RX pseudo events as `CCCI_EV_*` | done (collapsed by frozen header, documented) |
| Port table + channel map + minor numbers | `port_cfg.c` MD1/MD3 | absent | verbatim tables in `ccci_core.c`, `rx_ch_ports[]` hash | done |
| Rx dispatch | `ccci_core.c:70-135` | absent | `ccci_rx_dispatch_qno()`: channel bound check, power gate, port match, drop accounting | done |
| Tx queueing, per-port seq | `FEATURE_SEQ_CHECK_EN` on (`mt6735/ccci_config.h:32`) | absent | ring path increments under tx lock after room check; CLDMA increments in engine (confirmed with cldma leaf) | done |
| CCIF registers/handshake | `modem_ccif.c:1043`, `modem_cldma.c:1954` | absent | `ccci_ccif.c`: CON=0x01, ACK=0xFFFF, SRAM clear, BUSY→TCHNUM send with 1 ms bounded timeout | done |
| CCIF 512-byte SRAM window | `CCIF_SRAM_SIZE 512` (`modem_reg_base.h:44`) | absent | `struct ccci_ccif_sram` over `APCCIF_CHDATA`, D2H SRAM header dispatch | done |
| CCIF interrupt handling | `md_cd_ccif_isr` 21281, `md_ccif_irq_tasklet` 21273 | absent | hardirq acks RCHNUM → threaded handler (exception policy) + per-queue Rx tasklets | done |
| `ccif_enable_irq` / `ccif_disable_irq` | `modem_cldma.c:1771/1782` | absent | same names, refcounted, WDT included | done |
| MD watchdog | `md_cd_wdt_isr` 21274 / `md_cd_wdt_work` | absent | `ccci_ccif_wdt_isr` → work → reg dump, `ccci_md_reset()`, `CCCI_MD_MSG_RESET` to md_init | done |
| CLDMA data path (MD1) | `modem_cldma.c` (4k lines) | absent | delegated through `ccci_cldma_create/start/send/set_rx_cb` (ccci-cldma leaf), Rx via `ccci_md_rx_cb()` backlog | deferred (owned by cldma leaf) |
| CCIF-ring data path (MD3) | `modem_ccif.c` ring queues | absent | `ccci_ccif_ring_send/write_room/rx_poll`, per-queue tasklet + retry work, ring reset on exception | done |
| Ring buffer wire format | `ccci_ringbuf.c` | absent | `ccci_ringbuf.c`: 0xEE0000EE/0xFF0000FF framing, 0xAABBAABB + len, 8-byte align, 0xCCDDEEFF footer; MMIO accessors | done |
| Firmware load/validate/copy | `ccci_load_firmware` 21014 | absent | `request_firmware(md1img/md3img)`; `CHECK_HEADER` magic, `bind_sys_id == md_id+1`, `md_img_size`/`mem_size`, header v1–v5 at image tail; 1 MB chunked `memcpy_toio`; firmware released after copy | done; v6 headers rejected with `-EOPNOTSUPP` |
| Missing firmware behaviour | `Modem image not exist` | absent | clean `-ENOENT`, message kept in `boot.err_str`, boot aborts, state stays STOPPED | done |
| Boot vector/key/enable | `md_cd_let_md_go` 21326 | absent | `ccci_boot_let_md_go()`: 0x3567C766 → 0x0 → 0xA3B66175 | done |
| Power-on sequence | `md_cd_power_on` 21324 | absent | clock `scp-sys-md1-main` (optional) + runtime-PM/genpd + `WDT_MD_MODE = 0x220E` | partial: PMIC VLTE/VSEL, SPM `srcclkena`, RF clock-buffer pinctrl, `kicker_pbm_by_md` have no providers in this tree |
| Boot handshake (control msgs) | `port_kernel.c:501-630` | absent | MD_INIT_START_BOOT/MD_NORMAL_BOOT/MD_EX/MD_EX_REC_OK/MD_EX_PASS/MD_EX_RESUME/CCCI_DRV_VER_ERROR/C2K msgs | done |
| Runtime data handshake | `prepare_runtime_data`, `md_cd_send_runtime_data_v2`, SRAM variant | absent | `ccci_boot_prepare_runtime_data()` writes `ccci_runtime_feature` records into AP rt region; `ccci_boot_send_runtime_data()` sends `ap_query_md_feature` on control q0 (CLDMA) or the legacy `modem_runtime` in CCIF SRAM (ring mode) | done |
| Boot watchdog | `md_bootup_timeout_func` | absent | 20 s (CLDMA) / 10 s (CCIF) timer, BOOT_FAIL notify + image-head dump; AEE hand-off omitted (no AEE client) | done / AEE deferred |
| char ports & devices | `port_char.c` | absent | one cdev + `/dev/<name>` per char port, minors from the ROM table (0…23, IPC 100+n), `ccci_get_port_for_node()` | done |
| char read/write/poll | `port_char.c:178-560`, `:1130` | absent | header stripping, monitor `CCCI_MONITOR_CH_ID` tag, partial reads, state gating, non-blocking/blocking write with retry, `POLLIN/POLLOUT/POLLERR` | done |
| char ioctls | `port_char.c:558-1100` | absent | GET_MD_STATE, MD_RESET, DO_MD_RST, GET_MD_INFO, GET_MD_EX_TYPE, FORCE_MD_ASSERT, SEND_RUN_TIME_DATA, DO_START/STOP_MD(+requests), ENTER/LEAVE_DEEP_FLIGHT, POWER_ON/OFF requests, GET_MD_PROTOCOL_TYPE, SET/GET_MD_BOOT_MODE, GET_MD_MEM_SIZE, LOG_LVL ignored, PCM_*/ALLOC_MD_LOG_MEM no-op | done, see §8 for the two requested names that do not exist in the ROM |
| net port / CCMNI | `port_net.c` | absent | `struct ccmni_ccci_ops` filled (8/3 devices, "ccmni"/"cc3mni", `MODEM_CAP_*`), `send_pkt` returns 0/-1/-2/-3, ack-channel queue mapping, Rx pull + `ccmni_rx_callback`, md-state callback | done |
| kernel ports / system API | `port_kernel.c` | absent | control/system/status threads; `exec_ccci_kern_func_by_md_id()` (wakeup-src, state, reset, pause-lte, txpower, sim-switch store/get, dump, throttle/thermal), `register_ccci_sys_call_back()`, `ccci_get_md_boot_count()` | done; kernel RPC services deferred |
| sysfs / debugfs | `ccci_util_lib_sys.c` | absent | per-modem `/sys/class/ccci_node/mdN/{ccci_md_enable,ccci_version,ccci_md_state,ccci_dump,ccci_boot_info,ccci_md_exception}` + same names and `ccif` in debugfs `ccci/mdN/` | partial: the global `/sys/kernel/ccci` set is deferred (ccci-util also declines it) |
| `struct ccci_md_hw_ops` addendum | contract addendum | absent | registered per MD, NULL-checked; `power_on`/`let_md_go` failures abort the boot with a clean error; `power_off` on stop, `reset` on exception recovery | done |

## 5. Build / compile test

Exactly the contract command, from the worktree, with all nine objects (fresh objects, `.o` and `.*.cmd` removed first):

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-eccci"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
  drivers/misc/mediatek/ccci/ccci_core.o \
  drivers/misc/mediatek/ccci/ccci_platform.o \
  drivers/misc/mediatek/ccci/ccci_ccif.o \
  drivers/misc/mediatek/ccci/ccci_boot.o \
  drivers/misc/mediatek/ccci/ccci_port_char.o \
  drivers/misc/mediatek/ccci/ccci_port_net.o \
  drivers/misc/mediatek/ccci/ccci_port_kernel.o \
  drivers/misc/mediatek/ccci/ccci_ringbuf.o \
  drivers/misc/mediatek/ccci/ccci_sysfs.o
```

Result (`2026-10-24`):

```
  CC      drivers/misc/mediatek/ccci/ccci_core.o
  CC      drivers/misc/mediatek/ccci/ccci_platform.o
  CC      drivers/misc/mediatek/ccci/ccci_ccif.o
  CC      drivers/misc/mediatek/ccci/ccci_boot.o
  CC      drivers/misc/mediatek/ccci/ccci_port_char.o
  CC      drivers/misc/mediatek/ccci/ccci_port_net.o
  CC      drivers/misc/mediatek/ccci/ccci_port_kernel.o
  CC      drivers/misc/mediatek/ccci/ccci_ringbuf.o
  CC      drivers/misc/mediatek/ccci/ccci_sysfs.o
make exit: 0
```

Zero warnings, zero errors. `CONFIG_MTK_CCCI=y` in the resulting `.config`. `make drivers/misc/mediatek/ccci/built-in.a` also succeeds. Symbol audit of the nine objects: no duplicate globals; the only unresolved symbols are the intended cross-leaf/kernel ones — `ccci_cldma_{create,destroy,start,stop,send,set_rx_cb}`, `ccci_util_{init,exit,log_init,log_exit,log}` (plus `ccci_util_dump_set_window`), `ccmni_{init,exit,rx_callback,md_state_callback}` — all declared in `include/linux/ccci.h` or agreed with the owning leaf.

No full kernel build was run.

## 6. skb ownership (documented cross-leaf contract)

* `ccci_md_send_skb()`: on success the skb is consumed (the transport frees it); on `-EBUSY`/`-EINVAL`/`-EIO` the skb is untouched and the caller frees it. The skb must already contain the 16-byte `ccci_header` at `skb->data` and `skb->len >= 16`.
* CLDMA path: `ccci_cldma_send()` consumes and frees on 0; returns `-EBUSY` before any header/seq change; the engine owns `seq_num`/`assert_bit` (confirmed with the cldma leaf; the core only pre-sets `assert_bit` for `CCCI_RPC_TX`/`CCCI_FS_TX` as 1 iff `boot_stage == 1`). Rx from CLDMA arrives via `ccci_cldma_set_rx_cb(cd, ccci_md_rx_cb, md)`; the callback owns the skb and either dispatches it or queues it in the per-modem backlog.
* CCIF-ring path: `ccci_ccif_ring_send()` frees the skb after a successful ring write.
* Net port → `ccmni_rx_callback()`: core pulls the 16-byte header first (L3 at `skb->data`, 16 bytes headroom); CCMNI consumes the skb in all paths.
* `ccmni_send_pkt()`: skb arrives at L3 with ≥16 bytes headroom; core pushes the header. Returns 0 (consumed), `-1` (busy, skb restored), `-2` (not ready), `-3` (invalid).
* Char ports: the port RX queue owns skbs; read() consumes them; partially read packets stay in `port->rx_current` until drained or the node is closed.

## 7. Inferred / unverified on hardware

* Everything above is source-level parity only. No MT6737T device was attached; no register was written on real silicon.
* `[INFERENCE]` When the DT provides no `memory-region`, the shared memory comes from `dma_alloc_coherent()` and its physical range is only usable if the modem can address it. The log warns "modem visibility unverified". Real hardware requires reserved-memory carve-outs (see DTS fragment).
* `[INFERENCE]` The modem-image region must be the address the MD boot ROM maps (downstream gets it from the LK memory table via `md_mem_info_parsing()`, which this tree has no equivalent for). The driver therefore *requires* `memory-region = <&md_image>, <&md_smem>` for a real boot and fails cleanly (`-ENODEV`, "no modem image memory region in DT") without it.
* `[INFERENCE]` Ring sizes for the CCIF transport are the downstream constants; they are only exercised for a ring-mode modem (MD3) since MD1 uses CLDMA here.
* Timer/IRQ timing (1 ms CCIF busy timeout, 20 ms/10 s exception polling, 20 s/10 s boot timeout) is copied from downstream and not measured on hardware.
* `ccci_util_dump_set_window()` is called with the AP view of the modem smem, per the ccci-util leaf's published signature; integration must confirm symbol compatibility.
* The header `channel` field is 16 bits; `CCCI_FORCE_ASSERT_CH` (20090215) is deliberately truncated like the ROM does (`(u16)` cast with a comment).

## 8. Requested but non-existent in this ROM (documented mapping)

* `CCCI_IOC_GET_MD_BOOT_INFO`: no such ioctl exists in the 3.18 downstream `mt_ccci_common.h` (the ioctl list is lines 198–257) nor in the ROM. Implemented as `CCCI_IOC_GET_MD_INFO` (`_IOR('C', 8)`, the ROM's image-version query) and `ccci_boot_info` sysfs/debugfs for the human-readable boot record. No new ioctl number was invented.
* `CCCI_IOC_GET_MD_WAKEUP_SRC`: not an ioctl in this ROM. It is the *kernel API* `exec_ccci_kern_func_by_md_id(md_id, ID_GET_MD_WAKEUP_SRC, ...)` (`ccci_core.c:492` downstream), which this driver implements (sets the wakeup-source flag consumed by the CCIF Rx path).

## 9. Deferred (and why)

| Item | Why |
|---|---|
| CLDMA engine internals (GPD rings, ISR, Tx collect) | Owned by the `ccci-cldma` leaf; this core only calls the frozen `ccci_cldma_*` API |
| Kernel RPC services (GPIO/ADC/EINT/time RPC) | The ROM handlers sit on MTK `IMM_GetOneChannelValue`, `mt_get_gpio_in`, EINT and time-update APIs that do not exist in this tree; the `ccci_rpc_k` port logs and drops rather than faking replies |
| Global `/sys/kernel/ccci` attributes (`boot`, `md_en`, `version`, `md_chn`, `dump_max`, `lk_md`, `debug`, `ft_info`) | Downstream implements them in `ccci_util`; the ccci-util leaf declines them and this leaf owns only per-modem attributes |
| MD security/MPU/RMPU programming (`ccci_init_security`, `ccci_set_mem_access_protection`, check-header v4/v5 region tables) | Requires masp/EMI-MPU APIs absent from the tree; headers are parsed but region permissions are not programmed |
| PMIC VLTE/VSEL, SPM `md_src` request, RF clock-buffer pinctrl, `kicker_pbm_by_md`, `spm_is_md1_sleep()` | No regulators/PBM/SPM-md-src provider in this tree; power-on is clock+genpd+WDT only |
| AEE exception dump / SBP / DHL logging / MD sleep-history log | No AEE or SBP/DHL consumers in the tree |
| LK/DA modem memory table parsing (`ccci_parse_meta_md_setting`, `md_mem_info_parsing`) | Replaced by DT `memory-region` |
| MD1↔MD3 shared memory, C2K SDIO/CBP, rawbulk/USB bypass | Not part of the ROM build (no C2K SDIO symbols) and/or owned by the `ccci-c2k` leaf |
| IPC port kernel handshake (ID check, ack, garbage filter) | `port_ipc.c` is a separate subsystem with no userspace client recreated here; the IPC char nodes and channel mapping exist, the kernel-side ID filtering is not implemented |
| Header v6 images | Rejected with `-EOPNOTSUPP` (the ROM itself supports up to v5 in this build) |

## 10. DTS-FRAGMENT

Do not edit shared DTS. For the board DTS (`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`), the node below uses only the facts from contract §7 (register windows, interrupts, `md_id`, `cldma_capability`, `md_smem_size`, clock name).

The Nura `mt6735.dtsi` `soc` node carries **no `soc:` label**, so the overlay must
use the path form `&{/soc}` (a plain `&soc { }` fails with "Label or path soc not
found"). The `soc` node is `#address-cells = <2>` / `#size-cells = <2>`, hence the
four-cell `reg` entries. Validated by compiling the board DTS plus this fragment
with the tree's `scripts/dtc/dtc` (exit 0). The node name is lowercase
(`mdcldma@1000a000`) so `simple_bus_reg` matches the first `reg` entry and the node
produces no warning; the only remaining warning is the pre-existing
`/soc/interrupt-controller@10220000` one from `mt6735.dtsi` itself.

```dts
/* CCCI / MD1: CLDMA (data) + CCIF (control).
 * reg/interrupt values are the MT6735 mdcldma block (downstream
 * arch/arm64/boot/dts/mt6735.dtsi, node mdcldma@1000A000).
 */
&{/soc} {
	mdcldma: mdcldma@1000a000 {
		compatible = "mediatek,mdcldma";
		reg = <0 0x1000A000 0 0x1000>,	/* AP_CLDMA_AO  */
		      <0 0x1000B000 0 0x1000>,	/* MD_CLDMA_AO  */
		      <0 0x1021A000 0 0x1000>,	/* AP_CLDMA_PDN */
		      <0 0x1021B000 0 0x1000>,	/* MD_CLDMA_PDN */
		      <0 0x1020A000 0 0x1000>,	/* AP_CCIF      */
		      <0 0x1020B000 0 0x1000>;	/* MD_CCIF      */
		interrupts = <GIC_SPI 145 IRQ_TYPE_LEVEL_HIGH>,	 /* CLDMA */
			     <GIC_SPI 140 IRQ_TYPE_LEVEL_LOW>,	 /* CCIF  */
			     <GIC_SPI 221 IRQ_TYPE_EDGE_FALLING>;/* MDWDT */

		mediatek,md_id = <0>;
		mediatek,cldma_capability = <6>;
		mediatek,md_smem_size = <0x30000>;
		firmware-name = "md1img";

		/* mainline mt6735.dtsi has no scpsys clock provider; the MD1
		 * power domain gates the modem instead. If a clock provider
		 * exists, add:
		 *   clocks = <&scpsys SCP_SYS_MD1>;
		 *   clock-names = "scp-sys-md1-main";
		 */
		power-domains = <&spm MT6735_POWER_DOMAIN_MD1>;
		status = "okay";
	};
};
```

Boot additionally requires the two carve-outs, whose **base addresses and sizes must come from the device's own memory map** (they are what the MD boot ROM and the LK/DA memory table use; they are not present in any source available here, so no value is invented):

```dts
/ {
	reserved-memory {
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;

		/* modem image: copied by ccci_boot_load_firmware(); base must
		 * equal the address the MD boot vector maps.
		 * <FILL base>, size = modem image region (>= md_img_size).
		 */
		md1_image: md1-image@0 {
			compatible = "mediatek,md-image";
			reg = <0 0x0 0 0x0>;	/* FILL: hw memory map */
			no-map;
		};

		/* AP<->MD1 shared memory: exception + runtime + rings (CLDMA
		 * descriptors live here too). Keep the size from
		 * mediatek,md_smem_size (0x30000) or larger if the CLDMA leaf
		 * requires more descriptors.
		 */
		md1_smem: md1-smem@0 {
			compatible = "mediatek,md-smem";
			reg = <0 0x0 0 0x0>;	/* FILL: hw memory map */
			no-map;
		};
	};
};

&mdcldma {
	memory-region = <&md1_image>, <&md1_smem>;
};
```

Without the `memory-region` property the driver still probes, exposes the ports and the CCIF/CLDMA plumbing, but `CCCI_IOC_DO_START_MD`/`ccci_md_enable` fail with `-ENODEV` ("no modem image memory region in DT") instead of pretending to boot.

## 11. Cross-leaf coordination performed

* `ccci-util`: symbol split agreed — this leaf defines none of `ccci_dump_write`, `ccci_log_write*`, `ccci_event_log`, `ccci_util_mem_dump`, `ccci_util_cmpt_mem_dump`, `ccci_load_firmware`, `ccci_get_md_check_hdr_inf`, `ccci_init_security`, `ccci_md_mem_reserve`, `ccci_sysfs_add_modem`, `update_ccci_port_ver`, `trigger_md_boot`; it calls `ccci_util_init/exit/log_init/log_exit/log` and `ccci_util_dump_set_window`. debugfs `ccci/mdN` is shared safely via `debugfs_lookup`.
* `ccmni`: interface confirmed (L3 skb + headroom on Rx, error codes 0/-1/-2/-3 on Tx, ops struct is copied so the core keeps ownership, `napi_poll` may return 0).
* `ccci-cldma`: Tx `seq_num`/`assert_bit` ownership confirmed (engine), core only pre-sets the RPC/FS assert bit; Rx seq check runs in `ccci_md_rx_cb()` with the hardware queue number and raises `AP_MD_SEQ_ERROR`.

## 12. Patch

`$M/patches/ccci-eccci.patch` — `git add -A && git diff --cached --binary` (17 files, 6493 insertions, no commit).
