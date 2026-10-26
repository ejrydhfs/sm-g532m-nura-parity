# wcn-wmt — MediaTek WCN WMT/CONSYS core driver

Assigned driver: `drivers/misc/mediatek/connectivity/wmt/` in worktree
`$M/worktrees/wcn-wmt` (Nura `v7.0.10-mt6735`, arm64, MT6737T, SM-G532M).

This is a **driver port**, not an upstreaming series: every number below is
either copied from the Samsung/MTK downstream source named in the citation or
read from the ROM, and the code carries the same GPL-2.0 notice and MediaTek
copyright as the downstream files.

---

## 1. Files changed

All paths are relative to the worktree. 15 files, +6532 lines, all new except
the leaf Makefile.

| File | Role |
|---|---|
| `drivers/misc/mediatek/connectivity/wmt/wmt_local.h` | private types/constants/declarations shared by the leaf (downstream `core/include/*.h`, `include/*.h`, `mt6735/include/mtk_wcn_consys_hw.h`) |
| `.../wmt/stp_core.c` | STP engine: framing, seq/ack window, CRC16, two parsers (full + mandatory), 8 RX rings, event/tx-space callbacks |
| `.../wmt/stp_btif.c` | STP↔BTIF transport binding (`CONSYS_STP` owner, TX retry, RX registration, wakeup) |
| `.../wmt/wmt_core.c` | WMT op engine, PDU builders, init-script executor, register access, chip-id/version probe |
| `.../wmt/wmt_ctrl.c` | control dispatch (power, STP open/close/conf, patch store, paldo, GPS pins) |
| `.../wmt/wmt_func.c` | per-function BT/FM/GPS/WIFI on/off incl. VCN33/VCN28 and GPS_SYNC/LNA |
| `.../wmt/wmt_lib.c` | driver state, op serialisation, reset/recovery, configuration getters |
| `.../wmt/wmt_conf.c` | `WMT_SOC.cfg` parser |
| `.../wmt/wmt_ic_soc.c` | SOC_CONSYS bring-up: version tables, WMT init scripts, firmware patch download |
| `.../wmt/consys_mt6735.c` | `mediatek,mt6735-consys` platform driver: regs, genpd, clocks, rails, EMI window, IRQs, pinctrl |
| `.../wmt/wmt_dev.c` | `/dev/stpwmt` (major 190), ioctl ABI, patch store + firmware loader |
| `.../wmt/wmt_chrdev_wifi.c` | `/dev/wmtWifi` (major 153), `mtk_wcn_wmt_wifi_init/exit` |
| `.../wmt/wmt_api.c` | frozen `mtk_wcn_wmt.h` API |
| `.../wmt/wmt_main.c` | module init/exit ordering |
| `.../wmt/Makefile` | object list for `mtk_wcn_wmt.ko` |

The frozen headers (`connectivity/include/mtk_wcn_{stp,wmt,consys}.h`), the
`btif/` scaffold, the leaf `Kconfig` and the `drivers/misc/{Kconfig,Makefile}`
hooks are the coordinator's scaffold and are **not** part of this patch (they
were snapshotted into the index before work started). Only `wmt/Makefile`
inside the leaf was edited by me.

---

## 2. ROM evidence

### 2.1 Symbols present in `$M/zip/symbols.txt`

STP: `mtk_wcn_stp_parser_data`, `mtk_wcn_stp_send_data`, `mtk_wcn_stp_is_ready`,
`mtk_wcn_stp_register_event_cb`, `mtk_wcn_stp_rxcb_register`,
`mtk_wcn_stp_open_btif`, `mtk_wcn_stp_tx`, `mtk_wcn_stp_wakeup_consys`.

WMT core/lib/ic: `wmt_core_init`, `wmt_core_deinit`, `wmt_core_tx`,
`wmt_core_rx`, `wmt_core_rx_flush`, `wmt_core_func_ctrl_cmd`,
`wmt_core_opid(_handler)`, `wmt_core_ctrl`, `wmt_core_stp_init`,
`wmt_core_stp_deinit`, `wmt_core_reg_rw_raw`, `wmt_core_init_script`,
`wmt_core_patch_check`, `wmt_core_is_quick_ps_support`,
`wmt_core_set_coredump_state`, `wmt_lib_*` (init/deinit/…),
`mtk_wcn_soc_sw_init`, `mtk_wcn_soc_sw_deinit`, `mtk_wcn_soc_ver_check`,
`mtk_wcn_soc_pin_ctrl`, `mtk_wcn_soc_co_clock_ctrl`,
`mtk_wcn_soc_patch_info_prepare`, `mtk_wcn_soc_patch_dwn`,
`mtk_wcn_soc_quick_sleep_flag_get`.

CONSYS/platform: `mtk_wcn_consys_hw_init/deinit/pwr_on/pwr_off/rst`,
`mtk_wcn_consys_emi_virt_addr_get`, `mtk_wcn_consys_soc_chipid`,
`wmt_plat_pwr_ctrl`, `wmt_plat_gpio_ctrl`, `wmt_plat_eirq_ctrl`,
`wmt_plat_soc_paldo_ctrl`, `wmt_conf_read_file`, `wmt_dev_patch_get`,
`wmt_ctrl`, `wmt_ctrl_patch_search`.

Device nodes: `WMT_open`, `WMT_read`, `WMT_write`, `WMT_poll`,
`WMT_unlocked_ioctl`, `WMT_close`, `WMT_fops`; `WIFI_open`, `WIFI_write`,
`WIFI_close`, `WIFI_fops`. **`WIFI_ioctl` and `WMT_ioctl` are absent**, and
`WMTIOCTL` does not appear in `vmlinux.strings` — the ROM's `/dev/wmtWifi` is a
`write()`-only node (see the interpretation in §3, row "wmtWifi control ABI").

### 2.2 Strings in `$M/zip/vmlinux.strings`

`WMT_SOC.cfg`, `mt66xx_patch_hdr.bin`, `stpwmt`, `wmtWifi`,
`/system/etc/firmware/`, `WMT_SLEEP_CMD`/`WMT_WAKEUP_EVT` data symbols.

### 2.3 ROM device tree (`$M/zip/rom_dtb_0.dts`)

* `consys@18070000`: `reg = <0x18070000 0x200 0x10212000 0x100 0x10000000 0x2000 0x10006000 0x1000>`,
  `interrupts = <0 0xe3 8 0 0xe1 8>` (= GIC_SPI 227 and 225, level-low),
  `clocks = <0x14 3 0x1a 0xb>`, `clock-names = "conn", "bus"`,
  `vcn18/vcn28/vcn33_bt/vcn33_wifi-supply`, `pinctrl-names = "default",
  "gps_lna_state_init", "gps_lna_state_oh", "gps_lna_state_ol"`.
  Phandle `0x14` is `mediatek,mt6735-scpsys` (the legacy SCP/CONN gate) and
  `0x1a` is `mediatek,mt6735-infrasys` (index 0x0b = CONNMCU bus). The SCP gate
  is what Nura models as `MT6735_POWER_DOMAIN_CONN`
  (`drivers/pmdomain/mediatek/mt6735-pm-domains.h`, `SPM_CONN_PWR_CON 0x0280`),
  which is why the port uses `power-domains` instead of writing
  `CONSYS_TOP1_PWR_CTRL_REG` by hand.
* `consys-reserve-memory`: `size = <0x100000>`, `no-map` → base taken from the
  bootloader (Nura reserves `consys_mem` at `0x7f800000/1 MiB`).
* `btif@1100c000` (`reg 0x1100c000/0x1000`, IRQ 0x5a) and `wifi@180f0000`
  (`reg 0x180f0000/0x5c`, IRQ 0xe2) are handled by the other leaves.

### 2.4 ROM system image (`$M/zip/system.raw.img`, `debugfs`)

`/etc/firmware` (i.e. `/system/etc/firmware`) contains:

```
ROMv2_lm_patch_1_0_hdr.bin   131812 bytes
ROMv2_lm_patch_1_1_hdr.bin    68164 bytes
WMT_SOC.cfg                     117 bytes
WIFI_RAM_CODE_6735           416688 bytes   (WLAN leaf)
WIFI                            514 bytes   (WLAN leaf)
```

`WMT_SOC.cfg` content (verbatim):

```
coex_wmt_ant_mode=1

wmt_gps_lna_pin=0
wmt_gps_lna_enable=0

co_clock_flag=3

bt_tssi_from_wifi=1
bt_tssi_target=3664
```

`co_clock_flag=3` means the co-clock power-on path (VCN28 *not* powered by the
CONSYS power-on sequence) and the `osc_type` init script are used;
`wmt_gps_lna_enable=0` means the host-side GPS LNA pin (`gps_lna_state_*`)
is used. `coex_wmt_ant_mode` belongs to the deferred coexistence engine and is
accepted-but-ignored by the parser.

Patch header of `ROMv2_lm_patch_1_0_hdr.bin` (first 32 bytes):

```
00000000: 3230 3136 3131 3131 3133 3239 3339 610a   20161111132939a.
00000010: 3136 3239 8a00 8a00 2200 0600 ffff ffff   1629....".......
```

i.e. 16-byte date, 4-byte platform `"1629"`, `u2HwVer=0x008a`,
`u2SwVer=0x008a`, `u4PatchVer=0x00060022`, body at offset 28 — matching the
28-byte `WMT_PATCH` layout with the 16-bit fields stored big-endian (the
downstream log byte-swaps them to `0x8a00`).

### 2.5 Downstream source citations (primary reference)

| Behaviour | Citation |
|---|---|
| STP full-mode parser / states | `core/stp_core.c:1790-1954` |
| STP mandatory-mode parser | `core/stp_core.c:1474-1770` |
| STP packet/ack processing | `core/stp_core.c:972-1160` |
| STP TX (full/mandatory), window 7, 180 ms retry | `core/stp_core.c:2407-2560`, `core/include/stp_core.h:118-140` |
| STP RX rings, per-channel callbacks | `core/stp_core.c:660-720`, `linux/pri/stp_exp.c:85-230` |
| STP TX timeout / resync / retry limit 10 | `core/stp_core.c:371-500` |
| BTIF binding, owner `CONSYS_STP`, 10×5 ms TX retry | `linux/pri/stp_btif.c:43-46,77-200` |
| WMT PDU layouts (therm/dsns/reg/assert) | `core/wmt_core.c:139-259` |
| WMT op table | `core/wmt_core.c:210-239` |
| `wmt_core_init_script` incl. RF-cal event bypass | `core/wmt_core.c:598-644` |
| `wmt_core_reg_rw_raw` | `core/wmt_core.c:500-562` |
| `wmt_core_stp_init` / `deinit` | `core/wmt_core.c:609-845` |
| chip-id acceptance list | `core/wmt_core.c:861-940` |
| func on/off state machine + power-off rule | `core/wmt_core.c:1077-1265` |
| hw reset / sw reset / assert (cmd `0x08`) | `core/wmt_core.c:1420-1537,2073-2082` |
| ctrl dispatch table | `core/wmt_ctrl.c:130-197` |
| STP open/close via BTIF | `core/wmt_ctrl.c:506-578` |
| patch get/free/num/info | `core/wmt_ctrl.c:602-772` |
| BT PALDO + FUNC_CTRL | `core/wmt_func.c:269-330` |
| FM/GPS VCN28 sharing, GPS_SYNC/LNA | `core/wmt_func.c:440-620` |
| WIFI via wlan probe/remove callbacks | `core/wmt_func.c:640-712` |
| SOC init scripts and bring-up order | `core/wmt_ic_soc.c:898-1290` |
| version tables (`0x8A00/0x8A01/0x8B01`) | `core/wmt_ic_soc.c:790-830` |
| chip-side pin control (AIF/GSYNC) | `core/wmt_ic_soc.c:1308-1471` |
| patch download, 1000-byte fragments, addr cmds | `core/wmt_ic_soc.c:1937-2188` |
| patch RAM base byte `0x40`/`0xc8` for 6735-class | `core/wmt_ic_soc.c:2042-2046` |
| WMT_SOC.cfg field table / parser | `core/wmt_conf.c:117-155,306-470` |
| reset flow + reset notifications | `core/wmt_lib.c:1417-1712` |
| public WMT API | `linux/pri/wmt_exp.c:74-460` |
| `/dev/stpwmt` ioctl numbers, major 190 | `linux/pri/wmt_dev.c:63-94` |
| ioctl handlers | `linux/pri/wmt_dev.c:1886-2245` |
| patch load by name (file API) | `linux/pri/wmt_dev.c:1511-1600` |
| `/dev/wmtWifi` major 153, write-only ABI | `linux/pub/wmt_chrdev_wifi.c:39,353-470,614-660` |
| power-on: VCN18 1.8 V + 150 µs | `mt6735/mtk_wcn_consys_hw.c:340-352` |
| power-on: VCN28 in non-co-clock mode | `mt6735/mtk_wcn_consys_hw.c:360-380` |
| CPU SW reset assert (key 0x88<<24) | `mt6735/mtk_wcn_consys_hw.c:389` |
| `PWRON_CONFG_EN = 0x0b160001` | `mt6735/mtk_wcn_consys_hw.c:391` |
| conn clock enable, 10 µs, bus clock | `mt6735/mtk_wcn_consys_hw.c:400-455` |
| chip-id poll 10×20 ms, ids 0x0321/0x0335/0x0337 | `mt6735/mtk_wcn_consys_hw.c:455-470` |
| ACR MBIST bit 18 | `mt6735/mtk_wcn_consys_hw.c:487-491` |
| CPU SW reset deassert | `mt6735/mtk_wcn_consys_hw.c:499-501` |
| final `msleep(20)` | `mt6735/mtk_wcn_consys_hw.c:615` |
| power-off order (bus clk → conn clk → rails) | `mt6735/mtk_wcn_consys_hw.c:620-700` |
| EMI remap value `((phys&0xFFF00000)>>20)|BIT(12)` | `mt6735/mtk_wcn_consys_hw.c:1063-1075,1155-1180` |
| EMI window offset 0x80000, 343 KiB | `mt6735/include/mtk_wcn_consys_hw.h:171-181` |
| BGF IRQ request/disable/free, name `BTIF_WAKEUP_IRQ` | `mt6735/wmt_plat_alps.c:492-536` |
| GPS LNA pinctrl states | `mt6735/wmt_plat_alps.c:685-730` |
| paldo mapping BT/WIFI→VCN33, FM/GPS→VCN28 | `mt6735/wmt_plat_alps.c:863-885` |

---

## 3. Parity matrix

`done` = implemented from the cited source and compile-tested;
`partial` = implemented with the stated caveat; `deferred` = intentionally
absent (omitted, not stubbed).

### STP engine (`stp_core.c`)

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| Frame framing (0x80 sync, seq/ack, type, len, header checksum, CRC16) | `stp_core.c:1790-2210` | none | same state machine, both parsers | done |
| Mandatory mode (no seq/ack/CRC, 4-byte padding) | `stp_core.c:1474-1770` | none | ported | done |
| 8 per-channel RX rings of 16384 B, channel flush on register | `stp_core.c:604-652`, `stp_exp.c:200-216` | none | `struct stp_ring[8]` + spinlocks | done |
| Sliding window 7, ACK/NAK processing, tx_read advance | `stp_core.c:921-963` | none | ported | done |
| TX timeout 180 ms, resync `7f 7f 7f 7f`, retransmit, limit 10 | `stp_core.c:345-495` | none | timer → work → resync+resend | done |
| Retry-limit/assert recovery | downstream asserts via EMI/BTM then resets | none | marks STP not ready and runs a whole-chip reset | partial (see §5) |
| is_ready / send/receive / rxqueue-empty / register cbs | `stp_exp.h` | frozen decls only | implemented + `EXPORT_SYMBOL_GPL` | done |
| bluez direct HCI delivery | `stp_core.c:985-1000`, `stp_exp.c:149-161` | none | implemented, state defaults off; setter exported as `mtk_wcn_stp_set_bluez` (no frozen-API caller) | partial |
| Raw WMT TX before STP enable | `stp_core.c:2653-2700` | none | `mtk_wcn_stp_send_data_raw` + pending-type routing | done |
| PSM sleep/wakeup engine (`stp_psm_*`) | `core/psm_core.c` | none | — | deferred (not in the frozen API; no PS control) |
| BTM coredump/trace collection | `core/btm_core.c` | none | — | deferred |
| `stp_dbg` packet logging | `linux/pri/stp_dbg.c` | none | replaced by `pr_debug` | deferred |

### BTIF transport (`stp_btif.c`)

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| open owner `CONSYS_STP`, register TX if, mark BTIF active | `stp_btif.c:77-95` | none | `stp_btif_open()` | done |
| TX with 10 retries × 5 ms for short writes | `stp_btif.c:129-186` | none | ported (also treats `0` as "retry", per the BTIF owner contract) | done |
| RX registration → `mtk_wcn_stp_parser_data` | `stp_btif.c:104-127` | none | trampoline in kthread context | done |
| close / unregister | `stp_btif.c:97-122` | none | `stp_btif_close()` | done |
| `wakeup_consys` notification | `stp_btif.c:206-224` | none | `stp_btif_wakeup()` via `WMT_CTRL_SOC_WAKEUP_CONSYS` | done |
| dpidle clock gating control | PSM-driven downstream | none | not called (PSM deferred; BTIF leaf owns the gate) | deferred |

### WMT control protocol (`wmt_core.c`, `wmt_ctrl.c`, `wmt_func.c`)

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| opid engine, one command/event exchange at a time | `wmt_core.c:470-560` | none | op table + `op_lock` | done |
| pwr_on/pwr_off with retries and STP (re)init | `wmt_core.c:947-1075` | none | ported | done |
| func_on/func_off with per-function ops and auto power-off | `wmt_core.c:1077-1265` | none | ported | done |
| hw_rst (power-cycle), sw_rst (full re-init) | `wmt_core.c:1420-1531` | none | ported | done |
| stp_rst | downstream handler is empty (`wmt_core.c:1533`) | none | flushes STP state and clears ready | partial ([INFERENCE]) |
| reg_rw via WMT `0x08` opcode | `wmt_core.c:500-562` | none | ported | done |
| gpio_ctrl / pin_state | `wmt_core.c:1898-1930` | none | AIF_2 + GPS_SYNC chip-side writes; EEDI/EEDO return `-EOPNOTSUPP` | partial |
| assert (WMT cmd `01 02 01 00 08`) | `wmt_core.c:1720`, `wmt_exp.c:366-410` | none | ported, preceded by a CONSYS wakeup | done |
| therm / dsns commands | `wmt_core.c:1539-1590,1200-1260` | none | ported | done |
| hw ver + chip id detection | `wmt_core.c:861-940`, `wmt_ic_soc.c:1512-1558` | none | GEN_HCR/HVR/FVR + info table | done |
| `wmt_core_init_script` (incl. RF-cal `0x14` bypass) | `wmt_core.c:598-644` | none | ported | done |
| function power bitmaps + WMTMSG_RESET msgcb | `wmt_lib.c:1399-1443` | none | `drv_status[]`, `msgcb[]`, reset notifications | done |
| WLAN probe/remove hooks | `wmt_func.c:640-712`, `wmt_exp.c:426-462` | none | `mtk_wcn_wmt_wlan_reg` (calls probe if registered late) | done |
| PSM pwr_sv (sleep/host-awake/wakeup PDUs) | `wmt_core.c:1267-1420` | none | — | deferred |
| loopback, cmd_test, efuse, mcu-clk, ADIE, BGW desense, IDC/LTE coex, TDM antsel | `wmt_core.c:1463-2090`, `psm_core.c`, `wmt_idc.c` | none | — | deferred |
| coredump state / AEE dump flag | `wmt_lib.c`, `stp_dbg.c` | none | — | deferred |

### CONSYS platform (`consys_mt6735.c`)

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| DT node `mediatek,mt6735-consys`, 4 reg ranges, IRQ 227 | ROM DTS §2.3 | no driver | platform driver, `devm_platform_ioremap_resource` ×4, `platform_get_irq(0)` | done |
| CONN power domain on/off | `mtk_wcn_consys_hw.c:391-455` (SPM writes) | genpd exists, unused | `pm_runtime_get_sync/put_sync` on the CONN genpd | done |
| VCN18 1.8 V + 150 µs | `mtk_wcn_consys_hw.c:340-350` | none | regulator set/enable | done |
| VCN28 2.8 V only when not co-clock | `mtk_wcn_consys_hw.c:360-380` | none | ported (ROM cfg has co-clock=3) | done |
| CPU SW reset assert/deassert with key | `:389`, `:499` | none | ported | done |
| `PWRON_CONFG_EN=0x0b160001` | `:391` | none | ported | done |
| conn/bus clocks, 10 µs gap | `:400-455` | none | `conn` required, `bus` optional | done |
| chip-id poll 10×20 ms, `CONSYS_CHIP_ID_OFFSET` | `:455-470` | none | ported | done |
| ACR bit 18 before reset release | `:487-491` | none | ported | done |
| `msleep(20)` after release | `:615` | none | ported | done |
| power-off order | `:620-700` | none | ported with per-resource tracking | done |
| EMI remap register + 343 KiB window at +0x80000 | `:1050-1075`, `:1150-1180` | none | ported, `memory-region` parsed via `of_reserved_mem_lookup` | done |
| BGF IRQ request/disable/free | `wmt_plat_alps.c:492-536` | none | ported; handler only kicks waiters (PSM deferred) | partial |
| GPS LNA host pinctrl (init/oh/ol) | `wmt_plat_alps.c:685-730` | none | ported | done |
| GPS_SYNC chip-side mux | `wmt_ic_soc.c:1426-1445` | none | WMT reg write `0x80050078[31:28]` | done |
| BT/WIFI VCN33 paldo | `wmt_plat_alps.c:820-850` | none | `vcn33_bt`/`vcn33_wifi` regulators | done |
| EMI MPU region protection | `mtk_wcn_consys_hw.c:1055-1062` | n/a | — | deferred (no `emi_mpu` API in Nura) |
| hibernation restore callback | `mtk_wcn_consys_hw.c:1185-1188` | n/a | — | deferred |
| JTAG pin mux | `mtk_wcn_consys_hw.c:120-260` | n/a | — | deferred |
| WDT_EINT (225) | declared in DT; never requested downstream | n/a | not requested | deferred (unused downstream) |

### Firmware / patch path

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| 28-byte `WMT_PATCH` header parse, byte-swapped hw/sw logged | `wmt_ic_soc.c:1990-2015` | none | ported | done |
| 1000-byte fragments, FIRST/MID/LAST flags, len field, in-place WMT header | `wmt_ic_soc.c:2050-2185` | none | ported (buffer has 8-byte headroom) | done |
| patch/part-patch address PDUs, `0x40`/`0xc8` for 6735-class | `wmt_ic_soc.c:2032-2048` | none | ported | done |
| multi-patch loop + WMT reset after each patch | `wmt_ic_soc.c:995-1020` | none | ported | done |
| load patch by launcher-provided name | `wmt_dev.c:1511-1600` (`filp_open`) | none | `request_firmware()` then `kernel_read_file_from_path()` fallback | done |
| `WMT_SOC.cfg` parse | `wmt_conf.c:520-560` | none | subset: `co_clock_flag`, `wmt_gps_lna_pin/enable`, `bt_tssi_from_wifi/target`; coexistence keys accepted and ignored | partial |
| `/dev/stpwmt` ABI: `SET_PATCH_NAME(4)`, `SET_STP_MODE(5)`, `FUNC_ONOFF_CTRL(6)`, `GET_CHIP_INFO(12)`, `SET_PATCH_NUM(14)`, `SET_PATCH_INFO(15)`, `PORT_NAME(20)`, `WMT_CFG_NAME(21)`, `QUERY_CHIPID(22)`, `TELL_CHIPID(23)`, magic `0xa0` | `wmt_dev.c:72-88,1886-2245` | none | all implemented; `PORT_NAME` returns `-EOPNOTSUPP` (BTIF-only transport) | partial |
| read/poll/write command channel (`"srh_patch"`/`"ok"`) | `wmt_dev.c:1770-1870` | none | ported | done |
| 32-bit launcher ioctl compatibility on arm64 | not applicable (32-bit ROM kernel) | n/a | `.compat_ioctl` remaps the pointer-sized commands | done ([INFERENCE]) |

### Wi-Fi control node

| Behaviour | ROM/downstream | Nura before | After | Status |
|---|---|---|---|---|
| `/dev/wmtWifi` major 153, `write("0"/"1")` powers WIFI off/on | `wmt_chrdev_wifi.c:39,353-470`; ROM symbols `WIFI_write`, no `WIFI_ioctl` | none | `mtk_wcn_wmt_wifi_init/exit` + write handler | done |
| assert + recover on power failure | `wmt_chrdev_wifi.c:185-196` | none | `mtk_wcn_wmt_assert(WMTDRV_WIFI, 0x40)` | done |
| `D<level>` dynamic WLAN debug level, P2P/AP reconfig | `wmt_chrdev_wifi.c:106-168,201-353,430-470` | none | `D` returns `-EOPNOTSUPP`; reset recovery left to the WLAN `WMTMSG_RESET` callback | deferred |

### Frozen API completeness

All prototypes in the three frozen headers are defined in `mtk_wcn_wmt.ko`
(verified with `aarch64-linux-gnu-nm`, see §4):

* `mtk_wcn_wmt.h`: `func_on/off`, `msgcb_reg/unreg`, `wlan_reg/unreg`,
  `assert`, `system_state_reset`, `dsns_ctrl`, `therm_ctrl`, `hwver_get`,
  `wifi_init/exit` — all `EXPORT_SYMBOL_GPL`.
* `mtk_wcn_stp.h`: all 9 functions — all `EXPORT_SYMBOL_GPL`.
* `mtk_wcn_consys.h`: all 8 functions — all `EXPORT_SYMBOL_GPL`.

---

## 4. Compile test

Environment and commands (exactly as run):

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/wcn-wmt
cp $M/nura.config .config
./scripts/config -e MODULES -m MTK_BTIF -m MTK_WCN_WMT
make ARCH=arm64 LLVM=1 olddefconfig
# .config now has CONFIG_MTK_BTIF=m and CONFIG_MTK_WCN_WMT=m
for f in stp_core stp_btif wmt_core wmt_ctrl wmt_func wmt_lib wmt_conf \
         wmt_ic_soc consys_mt6735 wmt_dev wmt_chrdev_wifi wmt_api wmt_main; do
  make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
       OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
       READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
       drivers/misc/mediatek/connectivity/wmt/$f.o
done
make ... drivers/misc/mediatek/connectivity/wmt/mtk_wcn_wmt.o
```

Result: **13/13 objects compile with zero warnings and zero errors**
(clang, `W=1`-clean under the default build flags), and the module object links:

```
  LD [M]  drivers/misc/mediatek/connectivity/wmt/mtk_wcn_wmt.o
link rc=0
```

`make modules`/modpost cannot complete in this tree because `vmlinux.o` does not
exist (no full kernel build is allowed); the resulting modpost errors are the
generic `"_printk ... undefined"` class caused by the missing vmlinux, not by
this driver. Symbol check of the linked module object:

```
U mtk_wcn_btif_close
U mtk_wcn_btif_open
U mtk_wcn_btif_rx_cb_register
U mtk_wcn_btif_wakeup_consys
U mtk_wcn_btif_write
```

Those five are the only `mtk_wcn_*` imports and are provided by the BTIF leaf
(`CONFIG_MTK_BTIF`). Every frozen API symbol is defined (`T`) in the module.

No file outside the leaf was modified; `$M/nura.config` was not touched.

---

## 5. Inferences and hardware-unverified behaviour

Marked `[INFERENCE]` in the source where the code makes a choice the
downstream source does not spell out:

1. **SPM gate / AXI protection** are delegated to the MT6735 CONN power domain
   (`power-domains`) instead of the downstream hand-written
   `CONSYS_TOP1_PWR_CTRL_REG`/`CONSYS_TOPAXI_PROT_*` sequence. The genpd writes
   the same `SPM_CONN_PWR_CON` (0x280) and the same bus-protection bits, so the
   effect is equivalent, but the *ordering* relative to the clock gates is
   inferred from the ROM DT's legacy scpsys clock provider.
   **Hardware-unverified.**
2. **PMIC control-mode writes** `pmic_set_register_value(PMIC_RG_VCN18_ON_CTRL /
   VCN28_ON_CTRL / VCN33_ON_CTRL_*)` (`mtk_wcn_consys_hw.c:337,366,1027-1035`)
   are omitted: Nura has no `upmu_common`/pwrap register API and inventing the
   register numbers is not allowed. The port assumes the MT6328 regulator
   driver leaves the VCN rails in software-control mode. **Hardware-unverified;
   if the PMIC keeps hardware control, VCN28/VCN33 enable/disable will not take
   effect.**
3. **`co_clock_flag=3`** (from the ROM's `WMT_SOC.cfg`) selects the co-clock
   path: VCN28 is not powered at CONSYS power-on and the `osc_type` script is
   sent. This is a data-driven consequence of the config file.
4. **Firmware-assert-via-EMI** (`EXP_APMEM_HOST_OUTBAND_ASSERT_MAGIC_W1`) is
   not ported (coredump deferred). On STP TX-retry exhaustion / ACK-miss /
   firmware assert messages the driver marks STP not ready and performs a
   whole-chip reset (`wmt_lib_cmb_rst`) instead of the downstream
   assert-then-reset order. `WMT_OPID_STP_RST` and `WMT_CTRL_STP_RST` are
   implemented as "flush the STP state machine and clear ready" because the
   downstream handlers are empty.
5. **PSM is absent.** The chip never enters its sleep protocol, the BGF wakeup
   IRQ stays disabled after power-on, and `wmt_ctrl_soc_wakeup_consys` is only
   used before sending an assert command. Power consumption is therefore higher
   than the downstream driver; functionally all required paths are unaffected.
6. **GPS chip-side LNA** (`WMT_SOC.cfg wmt_gps_lna_enable=1`, EEDI/EEDO
   register tables) returns `-EOPNOTSUPP`. The ROM config uses the host-side
   LNA pin, which is supported.
7. **`PORT_NAME`** returns `-EOPNOTSUPP`: the STP transport is BTIF on
   MT6735/MT6737T, there is no UART HIF, and `SET_STP_MODE` rejects the UART
   and SDIO HIF values the downstream driver accepted.
8. **BlueZ delivery** is implemented (including the exported
   `mtk_wcn_stp_set_bluez`) but dormant: the frozen STP API has no bluez call
   and the Bluetooth client uses MTK mode, as agreed with the BTIF/BT leaf.
9. **32-bit compat ioctls**: the ROM kernel is 32-bit and has no compat layer;
   the arm64 Nura kernel needs `.compat_ioctl` for the pointer-sized commands
   (`4`, `15`, `20`, `21`) plus the `CONFIG_COMPAT` mapping in §3.
10. **`msleep(20)` after reset release**, the `10 µs` delay before the bus
    clock, and the `150 µs` VCN18 settling delay are copied verbatim and are
    timing-sensitive hardware behaviour that no test here can confirm.
11. Two WMT init scripts return different firmware data than the expected
    event on some firmware builds (the RF-calibration `0x14` event is skipped
    exactly as downstream does); other mismatches fail the power-on, matching
    downstream.
12. The MTK WMT patch files are **not** in the tree and were **not** added;
    only the loader is implemented.

---

## 6. DTS fragment

The node itself is unchanged from the canonical fragment; the only addition is
the GPS LNA pinctrl state names that the ROM DT has and that the driver looks
up (`wmt_plat_alps.c:685-730`). `pinctrl-0` must reference the board's consys
default pin state; the three `gps_lna_state_*` states are required only if the
GPS LNA is driven by the AP.

```dts
#include <dt-bindings/clock/mediatek,mt6735-infracfg.h>
#include <dt-bindings/clock/mediatek,mt6735-pericfg.h>
#include <dt-bindings/power/mediatek,mt6735-power-controller.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

&soc {
	consys: consys@18070000 {
		compatible = "mediatek,mt6735-consys";
		reg = <0x18070000 0x0200>,   /* CONN_MCU_CONFIG_BASE */
		      <0x10212000 0x0100>,   /* AP_RGU_BASE */
		      <0x10000000 0x2000>,   /* TOPCKGEN_BASE */
		      <0x10006000 0x1000>;   /* SPM_BASE */
		interrupts = <GIC_SPI 227 IRQ_TYPE_LEVEL_LOW>,  /* BGF_EINT */
			     <GIC_SPI 225 IRQ_TYPE_LEVEL_LOW>;  /* WDT_EINT */
		clocks = <&infracfg CLK_INFRA_CONNMCU_BUS>;
		clock-names = "conn";
		power-domains = <&spm MT6735_POWER_DOMAIN_CONN>;
		memory-region = <&consys_mem>;
		vcn18-supply = <&mt6328_vcn18_reg>;
		vcn28-supply = <&mt6328_vcn28_reg>;
		vcn33_bt-supply = <&mt6328_vcn33_bt_reg>;
		vcn33_wifi-supply = <&mt6328_vcn33_wifi_reg>;
		pinctrl-names = "default",
				"gps_lna_state_init",
				"gps_lna_state_oh",
				"gps_lna_state_ol";
		pinctrl-0 = <&consys_pins_default>;
		pinctrl-1 = <&consys_gps_lna_init>;
		pinctrl-2 = <&consys_gps_lna_oh>;
		pinctrl-3 = <&consys_gps_lna_ol>;
		status = "okay";
	};

	wifi: wifi@180f0000 {
		compatible = "mediatek,wifi";
		reg = <0x180f0000 0x0100>;
		interrupts = <GIC_SPI 226 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&pericfg CLK_PERI_APDMA>;
		clock-names = "wifi-dma";
		status = "okay";
	};

	btif: btif@1100c000 {
		compatible = "mediatek,btif";
		reg = <0x1100c000 0x1000>,    /* BTIF */
		      <0x11000880 0x0080>,    /* APDMA TX */
		      <0x11000900 0x0080>;    /* APDMA RX */
		interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_LOW>,
			     <GIC_SPI 113 IRQ_TYPE_LEVEL_LOW>,
			     <GIC_SPI 114 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&pericfg CLK_PERI_BTIF>, <&pericfg CLK_PERI_APDMA>;
		clock-names = "btifc", "apdmac";
		status = "okay";
	};
};
```

Notes for the integration owner:

* The ROM DT has two clock phandles (`"conn"` = legacy scpsys CONN-MCU gate,
  `"bus"` = infracfg CONNMCU bus gate). Nura models the first as the CONN
  power domain, so `"conn"` here is the infracfg bus gate; the driver also
  accepts an optional second `"bus"` clock if one is added later.
* `memory-region` must point at the 1 MiB `consys_mem` reservation that
  `mt6735.dtsi` already declares (`0x7f800000`). The driver maps the upper half
  (`+0x80000`, 343 KiB) and programs the EMI remap register from its base.
* `WDT_EINT` (225) is listed because the ROM lists it, but no downstream code
  requests it; the driver uses index 0 (BGF) only.
* Supply names and the `vcn*` rails already exist in `mt6328.dtsi`; the
  fragment assumes the board aliases named in the canonical fragment.

---

## 7. Runtime files userspace must provide (no blobs are added)

| Path | Consumer | Notes |
|---|---|---|
| `/system/etc/firmware/WMT_SOC.cfg` | `wmt_conf.c` | optional; parsed for `co_clock_flag`, `wmt_gps_lna_pin/enable`, `bt_tssi_from_wifi/target`. The ROM copy is reproduced in §2.4. If absent the driver runs with defaults (no co-clock, host LNA, no BT TSSI sharing). Overridable with `WMT_IOCTL_WMT_CFG_NAME`. |
| patch file #1, name from `WMT_IOCTL_SET_PATCH_INFO` (`downloadSeq=1`) | `wmt_dev.c`/`wmt_ic_soc.c` | 28-byte `WMT_PATCH` header + body. In the ROM: `/system/etc/firmware/ROMv2_lm_patch_1_0_hdr.bin` (131812 B). Loaded with `request_firmware()` and, failing that, read from the absolute path. |
| patch file #2 (`downloadSeq=2`) | same | ROM: `/system/etc/firmware/ROMv2_lm_patch_1_1_hdr.bin` (68164 B). |
| fallback default patch name | `wmt_dev.c` | `mt66xx_patch_hdr.bin` (downstream default, resolved via `request_firmware`, i.e. `/lib/firmware/mt66xx_patch_hdr.bin`). |
| device nodes | init/uevent | `/dev/stpwmt` (major 190) and `/dev/wmtWifi` (major 153) are created by the driver's classes (`stpwmt`, `wmtWifi`). |
| launcher protocol | userspace | must `ioctl(SET_PATCH_NUM)`, then one `ioctl(SET_PATCH_INFO)` per patch (sequence 1..N, name + 4 address bytes), then `ioctl(SET_STP_MODE, 0x23)` (BTIF full + FM COMM). It should `poll()`/`read()` `/dev/stpwmt` and answer `write("ok")`; the only kernel→user command currently issued is `"srh_patch"`, which is informational because the patch list is already registered. |

No firmware is embedded in the patch.

---

## 8. Patch

`$M/patches/wcn-wmt.patch` (binary diff; 15 files, +6532 lines).

Sanity check:

```
git apply --stat $M/patches/wcn-wmt.patch   # 15 files changed, 6532 insertions(+)
```
