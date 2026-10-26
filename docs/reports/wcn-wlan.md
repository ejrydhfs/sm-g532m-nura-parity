# wcn-wlan — MediaTek WLAN (CONSYS gen2) AHB HIF + firmware bring-up

Worktree: `$M/worktrees/wcn-wlan`
Patch: `$M/patches/wcn-wlan.patch`
Report: `$M/logs/wcn-wlan.md`

## 1. Assigned driver and files changed

Host interface and firmware bring-up of the MediaTek WLAN function on the
MT6737T connectivity subsystem (CONSYS), ported from the Samsung/MTK
Android 6.0.1 downstream `connectivity/wlan/gen2` driver.

New files (all in `drivers/misc/mediatek/connectivity/wlan/`):

| File | Contents |
|---|---|
| `wlan_hif.h` | private HIF register map (mtreg.h), AP-DMA (PDMA) register map, `struct mtk_wlan_hif`, private prototypes |
| `wlan_fw.h` | gen2 firmware/section/init-command wire formats and constants |
| `wlan_ahb.c` | `mediatek,wifi` platform driver, PIO + PDMA data-port engine, HIF ISR/bottom half, driver-own, suspend/resume |
| `wlan_fw.c` | `request_firmware` + chip-suffixed name, MTKW header/CRC validation, divided + flat download, WIFI_START, ready polling |
| `wlan_main.c` | module init/exit, WMT WLAN callback registration, whole-chip reset notifier, rfkill |

Scaffold files edited only to build the driver:

* `wlan/Kconfig`: `select CRC32`, `select RFKILL`.
* `wlan/Makefile`: `obj-$(CONFIG_MTK_WCN_WLAN) += mtk_wcn_wlan.o` and
  `mtk_wcn_wlan-y := wlan_main.o wlan_ahb.o wlan_fw.o`.

No shared header, DTS, Kconfig or Makefile outside the WLAN subdirectory was
touched. The frozen ABI headers were not modified.

## 2. ROM evidence

ROM ELF: `$M/zip/vmlinux.elf` (symbols via `llvm-nm`), `$M/zip/symbols.txt`,
`$M/zip/vmlinux.strings`.

Symbols:

| Symbol | Address | Line in `symbols.txt` |
|---|---|---|
| `mtk_wcn_wmt_wlan_reg` | `0xc05578ac` | 21925 |
| `mtk_wcn_wmt_wlan_unreg` | `0xc05579ac` | 21926 |
| `mtk_wcn_wlan_gen2_init` | `0xc059729c` | — |
| `wlanProbe` | `0xc0595f20` | 22624 |
| `wlanRemove` | `0xc0594df8` | — |
| `HifAhbISR` | `0xc05b9acc` | 23014 |
| `HifAhbProbe` / `HifAhbRemove` | `0xc05b9be8` / `0xc05b9a74` | 23015 / 23013 |
| `HifAhbBusCntGet` / `HifAhbBusCntClr` | `0xc05b9960` / — | 23008 / 23009 |
| `HifPdmaInit` | `0xc05bbb9c` | 23043 |
| `kalDevPortRead` | `0xc05ba620` | 23030 |
| `glRegisterBus` | `0xc05b9cf4` | — |
| `glBusSetIrq` | `0xc05ba1a8` | — |
| `nicpmSetAcpiPowerD0` | `0xc058accc` | 22504 |
| `wlanImageSectionDownload` / `...Status` | `0xc055f918` / — | 22085 / 22084 |
| `mtk_wcn_consys_hw_wifi_paldo_ctrl` | — | 21941 |

Strings (`vmlinux.strings`):

| Line | String | Use |
|---|---|---|
| 41764 | `/etc/firmware/WIFI_RAM_CODE_` | chip-suffixed firmware path |
| 41767 | `WIFI_RAM_CODE` | firmware basename |
| 41713 | `WLAN AHB ISR` | IRQ name (used verbatim) |
| 5580, 42452 | `mediatek,WIFI` | OF compatible spelling in the ROM DT |
| 41347 | `/etc/firmware/wifi_fw.cfg` | 802.11-stack config file (deferred stack only) |
| 40926 / 40927 | `WLAN-GEN2 driver init` / `finish wlan module init` | gen2 module init path |

Downstream source is the primary reference; every register, descriptor field,
threshold and timing in the patch cites it.

### Register / field / timing citation table

| Item | Value | Downstream citation |
|---|---|---|
| HIF register block | `0x180f0000`, `0x5c` bytes | `hif/ahb/include/hif.h:217-218` |
| HIF register offsets | `WCIR 0x00 … HSTCR 0x58` | `include/nic/mtreg.h:81-143` |
| `WCIR_WLAN_READY` / `WCIR_CHIP_ID` / `WCIR_REVISION_ID` | `BIT(21)` / `BITS(0,15)` / `BITS(16,19)` | `mtreg.h:175-178` |
| `WHLPCR_INT_EN_SET/CLR`, `FW_OWN_REQ_SET/CLR` | `BIT0/BIT1/BIT8/BIT9` | `mtreg.h:187-190` |
| `WHCR_W_MAILBOX_RD_CLR_EN`, `WHCR_W_INT_CLR_CTRL` | `BIT2`, `BIT1` | `mtreg.h:204-205` |
| `WHISR_*` / `WHIER_DEFAULT` | `BIT0-3`, `GENMASK(31,8)` | `mtreg.h:213-225` |
| `WRPLR_RX0/RX1_PACKET_LENGTH` | `BITS(0,15)`, `BITS(16,31)` | `mtreg.h:243-244` |
| `HSTCR` fields (burst/target/count) | `BITS(24,25)`, `BITS(20,22)`, `BITS(2,19)` | `mtreg.h:246-260` |
| Chip-id remap `0x0321/0x0335/0x0337→0x6735`, `0x0326→0x6755` | — | `ahb.c:455-458` |
| Chip suffix for the firmware name | `%04x`, fallback `SOC` | `ahb.c:534-548` |
| `"wifi-dma"` clock lookup | — | `ahb.c:464` |
| IRQ name `"WLAN AHB ISR"`, trigger from DT | — | `ahb.c:651` |
| Driver-own loop: 2000 ms total, 1 ms step, clear every 256 iters | — | `nic_pwr_mgt.c:313-315,335-400` |
| Download with interrupts masked; `WHIER_DEFAULT` | — | `wlan_lib.c:1306-1307`, `nic.c:1251` |
| Init-command TX header / `INIT_CMD_DOWNLOAD_BUF` / `INIT_CMD_WIFI_START` | 8 B hdr, 16 B download cmd | `nic_init_cmd_event.h:89-117` |
| `DOWNLOAD_BUF_ACK_OPTION` / `ENCRYPTION_MODE` | `BIT(31)` / `BIT(0)` | `nic_init_cmd_event.h:103-105` |
| `CMD_PKT_SIZE_FOR_IMAGE` | 2048 | `include/nic/hif_tx.h:100` |
| Download chunk CRC32 | standard reflected CRC-32 | `wlan_lib.c:3288-3297,2920` |
| Divided header `'MTKW'` + CRC over bytes after `u4NumOfEntries` | offset 8 | `wlan_lib.h:598-611,733-737`, `wlan_lib.c:1318-1322` |
| `CFG_FW_LOAD_ADDRESS` / `CFG_FW_START_ADDRESS` (MT6628 build) | `0x00060000` | `config.h:1352-1355` |
| Start address = first section dest | — | `config.h:1355`, `wlan_lib.c:1335-1339` |
| Per-chunk ACK: EID + seq + status, 1000 ms | — | `wlan_lib.c:3059-3145`, `nic_rx.c:3619-3702`, `nic_rx.c:834` |
| Ready poll: 512 × 10 ms, then mailbox-0 id | — | `config.h:1204`, `wlan_lib.c:1428-1452` |
| Post-download HISR read, WTSR0/1, WHCR clear mode | — | `nic_pwr_mgt.c:598-602`, `wlan_lib.c:1649-1653` |
| `HAL_WRITE_TX_PORT` → TXD0 + zero dword terminator | — | `include/nic/hal.h:315-325`, `nic_tx.c:2124-2148` |
| PIO data-port loop / 32-bit word access | — | `ahb.c:1005-1012`, `ahb.c:1177-1186` |
| `HSTCR` burst/target programming before each data-port access | — | `ahb.c:877-883`, `ahb.c:1451-1468` |
| Save/restore `WHLPCR_INT_EN` around a data-port access | — | `ahb.c:886-890`, `ahb.c:1068-1072` |
| PDMA registers `INT_FLAG 0x00 … LEN 0x24`, `LENGTH 0x54` | — | `hif_pdma.h:61-106` |
| PDMA burst fixed 4/4, `FIX_EN=0`, `DIR` from transfer | — | `ahb_pdma.c:231-243` |
| PDMA completion poll 5 s, 100000 iterations for `EN` | — | `ahb.c:953,1042-1052,1128` |
| PDMA reset: warm reset, then hard reset + 1 ms | — | `ahb_pdma.c:568-592` |
| PDMA clock gating (`clk_prepare_enable`) per transfer | — | `ahb_pdma.c:393-413` |
| `HifTxCnt++` on every port write | — | `ahb.c:1059` |
| Bus counters reported to WMT | — | `ahb.c:1418-1438` |
| WMT WLAN callbacks registered from platform probe | — | `ahb.c:1546-1560` |
| ISR masks `WHLPCR`, wakes bottom half | — | `ahb.c:1215-1249` |
| IST reads `WHISR`, drains RX, re-enables | — | `wlan_lib.c:1836-1852`, `nic.c:1060-1113` |
| `CFG_IST_LOOP_COUNT` = 8 (RX drain bound) | — | `config.h:1296` |
| `CFG_RX_MAX_PKT_SIZE` = 2352 (RX bounce buffer) | — | `config.h:1179` |
| Reset messages `WMTRSTMSG_RESET_START/END/END_FAIL` | 0/1/2 | `os/linux/include/gl_rst.h:60-66` |
| Reset callback defers end-of-reset to a work item | — | `gl_rst.c:148-207` |
| Re-open after reset = `mtk_wcn_wmt_func_on(WIFI)` | — | `wmt_chrdev_wifi.c:197-268` |
| Chip assert reason code `0x40` on fatal HIF error | — | `gl_rst.h:80-89` |
| Module init/exit names `mtk_wcn_wlan_gen2_init/exit` | — | `gl_init.c:3673-3684` |

No GPIO is driven by this driver. No voltage rail is touched: the VCN33_WIFI
rail and the VCN18/VCN28 rails belong to the WMT/CONSYS power sequence and
are described by the canonical `consys` DT node.

## 3. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| `mediatek,wifi` platform device, reg 0x180f0000, IRQ 226, clk `wifi-dma` | `ahb.c:304-327,429-467,651` | no driver | `wlan_ahb.c` probe, DT resources only | done |
| HIF register map / PIO port read/write | `ahb.c:858-1212` | — | `wlan_hif_port_read/write` | done |
| `HSTCR` burst/target programming | `ahb.c:1451-1468` | — | `mtk_wlan_hif_dma_enhance` | done |
| AP-DMA (PDMA) config/start/poll/ack/stop/reset/clock | `ahb_pdma.c:225-592` | — | `mtk_wlan_pdma_*` | done |
| PDMA timeout → 5 s, register dump, chip assert | `ahb.c:953,1039-1041` | — | dump + `mtk_wcn_wmt_assert(WIFI,0x40)` via deferred work | done |
| HIF ISR mask + bottom half | `ahb.c:1215-1249`, `wlan_lib.c:1836-1852` | — | ISR + `irq_work` | done |
| ISR RX dispatch to 802.11 | `nic.c:1060-1157`, `nic_rx.c` | — | drains RX ports, counts `rx_dropped` | **deferred** (no stack) |
| TX status / abnormal status handling | `nic.c:1292-1298`, `nic.c:1023-1043` | — | logged, WTSR read | done (log only) |
| Driver-own acquisition | `nic_pwr_mgt.c:311-406` | — | `mtk_wlan_hif_claim_driver_own` | done |
| Firmware naming `WIFI_RAM_CODE_<chip>` | `gl_kal.c:804-890`, `ahb.c:534-548` | — | `mtk_wlan_fw_name` | done |
| Firmware open/size/load via VFS `/etc/firmware` | `gl_kal.c:804-1046` | — | `request_firmware` (kernel firmware path) | done, different KAPI |
| Divided firmware header validation (`MTKW`+CRC32) | `wlan_lib.c:1318-1322` | — | `mtk_wlan_fw_download` | done |
| Divided section download + per-chunk CRC/ACK | `wlan_lib.c:2865-2965` | — | `mtk_wlan_fw_download_section` | done |
| Flat image fallback at 0x60000 | `wlan_lib.c:1388-1400`, `config.h:1352` | — | same function | done |
| WIFI_START with section-0 address override | `wlan_lib.c:3156-3221`, `nic_pwr_mgt.c:569-574` | — | `mtk_wlan_fw_kick` | done |
| Wait `WCIR.WLAN_READY` 512×10 ms | `wlan_lib.c:1428-1452` | — | `mtk_wlan_fw_wait_ready` | done |
| Interrupt status reset + `WHCR` default + INT_EN | `nic_pwr_mgt.c:598-608`, `wlan_lib.c:1649-1653` | — | `mtk_wlan_fw_clear_state` | done |
| WMT WLAN registration (probe/remove/bus counters) | `ahb.c:1546-1560`, `wmt_exp.c:440-458` | — | `mtk_wlan_main_attach` | done |
| WMT reset message callback + recovery | `gl_rst.c:104-207` | — | `mtk_wlan_reset_cb` + work | done |
| rfkill | none in ROM | — | `mtk-wlan` rfkill gating WMT func on/off | done (new) |
| Device link to `consys` | none | — | `device_link_add()` when the node exists | done (new) |
| suspend/resume | no-ops (`ahb.c:1595-1611`) | — | mask/restore HIF interrupt | partial, `[INFERENCE]` |
| Firmware power-off handshake (`CMD_NIC_POWER_CTRL`, FW-own) | `nic_pwr_mgt.c:639-676`, `wlan_lib.c` | — | none; WMT powers the subsystem off | **deferred** (needs command path) |
| 802.11 MAC/mgmt engine, cfg80211/wiphy/netdev | `gen2/nic`, `gen2/mgmt`, `gl_cfg80211.c`, `gl_wext.c`, `gl_init.c` | — | not registered | **deferred** |
| EMI MPU region 12 for the 512 KiB EMI window | `ahb_pdma.c:190-201` | no `emi_mpu` API in Nura | none | **deferred** |
| `mtk_wcn_consys_hw_wifi_paldo_ctrl(1)` in probe | `ahb.c:1327-1332`, `mtk_wcn_consys_hw.c:946` | frozen CONSYS API has no such call | WMT owns it (canonical `vcn33_wifi-supply`) | **deferred** to WMT |
| GDMA engine (`hif_gdma.h`) | not used by MT6735 | — | not ported | deferred (PDMA is the downstream path) |
| `HIF_DEV_MISC` chardev, loopback test | disabled downstream (`CONF_HIF_DEV_MISC 0`, `CONF_HIF_LOOPBACK_AUTO 0`) | — | not ported | deferred (dev-only) |
| Debug dumps (full PDMA/CONNSYS register dump, procfs) | `ahb_pdma.c:424-557` | — | condensed `dev_err` dump on timeout | partial, intentional |

## 4. Compile test

Toolchain per `PARITY_CONTEXT.md` / assignment. `.config` derived from
`$M/nura.config` with:

```
./scripts/config -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -e CRC32
make ARCH=arm64 LLVM=1 olddefconfig
```

`CONFIG_MTK_WCN_WLAN=m`; `MTK_WCN_WLAN` selects `CRC32` and `RFKILL`
(`RFKILL=m` in the resulting `.config`).

Commands and results (all from `$M/worktrees/wcn-wlan`):

```
$ make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
    OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
    READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
    drivers/misc/mediatek/connectivity/wlan/wlan_ahb.o
  CC [M]  drivers/misc/mediatek/connectivity/wlan/wlan_ahb.o      (no warnings)

$ … drivers/misc/mediatek/connectivity/wlan/wlan_fw.o
  CC [M]  drivers/misc/mediatek/connectivity/wlan/wlan_fw.o       (no warnings)

$ … drivers/misc/mediatek/connectivity/wlan/wlan_main.o
  CC [M]  drivers/misc/mediatek/connectivity/wlan/wlan_main.o     (no warnings)
```

Also compiled with `CONFIG_RFKILL` unset (stub path) — clean — before the
`select RFKILL` was added, and again after `olddefconfig` enabled it — clean.

Full-module link: `make … drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.ko`
reaches `LD [M] …/mtk_wcn_wlan.o` and then modpost stops because `vmlinux.o`
is absent in a partial build (expected). The only non-core unresolved symbols
are exactly the frozen WMT API, verified with `llvm-nm -u`:

```
U mtk_wcn_wmt_assert   U mtk_wcn_wmt_func_off  U mtk_wcn_wmt_func_on
U mtk_wcn_wmt_msgcb_reg U mtk_wcn_wmt_msgcb_unreg
U mtk_wcn_wmt_wlan_reg  U mtk_wcn_wmt_wlan_unreg
```

Exported by the module: `mtk_wcn_wlan_gen2_init`, `mtk_wcn_wlan_gen2_exit`
only (plus the automatic `init_module`/`cleanup_module`).

`scripts/checkpatch.pl --no-tree --strict -f` on all five files:
`0 errors, 0 warnings, 0 checks`.

## 5. Inferred or hardware-unverified

* `[INFERENCE]` Driver-own success condition: `WHLPCR` bit 8 read back set.
  `mtreg.h:187-188` names the same bit `WHLPCR_FW_OWN_REQ_SET` and
  `WHLPCR_IS_DRIVER_OWN`; the code follows `nicpmSetDriverOwn()`
  (`nic_pwr_mgt.c:335-341`), which treats the bit as "host owns".
* `[INFERENCE]` The AP-DMA window (`0x11000080`, channel 1) is taken from a
  second DT `reg` entry instead of the downstream hard-coded
  `AP_DMA_HIF_BASE` (`hif_pdma.h:53-59`). It is optional: without it the
  driver logs a warning and uses PIO.
* `[INFERENCE]` Suspend/resume: downstream handlers are empty
  (`ahb.c:1595-1611`); masking `WHLPCR` interrupt enable across suspend is
  added so the EINT cannot storm while the AP is suspended without the
  deferred 802.11 D3 handshake.
* `[INFERENCE]` The chip assert after a fatal HIF error is deferred by
  100 ms so the bottom half that detected the error has returned before WMT
  can tear the interface down. `mtk_wcn_wmt_assert()` is assumed not to call
  the WLAN remove callback (true for the downstream reset path:
  `wmt_lib_hw_rst()` → `wmt_ctrl_hw_rst()` never calls `func_off`).
* `[UNVERIFIED on hardware]` `DOWNLOAD_BUF_ENCRYPTION_MODE` is set because
  `CFG_ENABLE_FW_ENCRYPTION` is 1 in this build (`config.h:1329-1330`,
  `wlan_lib.c:2914-2927`); whether the shipped `WIFI_RAM_CODE_6735` blob is
  plaintext or pre-encrypted is not verifiable here.
* `[UNVERIFIED on hardware]` `WCIR` chip id on MT6737T is expected to read
  `0x6735` (or one of the remapped Denali ids). If it does not, the firmware
  file name falls back to `WIFI_RAM_CODE_SOC`, exactly like `glGetChipInfo()`.
* `[UNVERIFIED on hardware]` The download ACK event is expected on RX port 0
  with EID 1 and the matching sequence number; the check is strict, as
  downstream (`wlan_lib.c:3107-3138`).
* `[UNVERIFIED on hardware]` PDMA register access is wrapped in
  `clk_prepare_enable("wifi-dma")`; downstream does this for the transfers
  (`ahb_pdma.c:405-411`) but not for `HifPdmaReset()`, which is called from
  the chip-reset path. Wrapping the reset is a correctness deviation, not a
  register change.
* `[UNVERIFIED on hardware]` The PDMA source/destination for the HIF side is
  `hif_phys + port` from DT (`0x180f0030` for RXD0), not a hard-coded
  `HIF_DRV_BASE` (downstream `ahb.c:917,1080`).
* `[INFERENCE]` The whole-chip reset notifier clears `up` at RESET_START
  because the subsystem is about to be reset; downstream keeps its state and
  re-probes unconditionally.

## 6. DTS-FRAGMENT

The canonical fragment is used as-is except for one added `reg` entry: the
AP-DMA channel used by the PDMA engine. The downstream DT only describes the
HIF window (`arch/arm/boot/dts/mt6735.dtsi:1477-1483`) because the downstream
driver hard-codes `AP_DMA_HIF_BASE`; this driver takes the window from DT
instead of hard-coding it.

```dts
/* needs:
 *   #include <dt-bindings/clock/mediatek,mt6735-infracfg.h>
 *   #include <dt-bindings/clock/mediatek,mt6735-pericfg.h>
 *   #include <dt-bindings/power/mediatek,mt6735-power-controller.h>
 */
&soc {
	wifi: wifi@180f0000 {
		compatible = "mediatek,wifi";
		reg = <0x180f0000 0x0100>,	/* HIF registers (hif.h:217-218) */
		      <0x11000080 0x0080>;	/* AP-DMA channel 1 (hif_pdma.h:53-59) */
		interrupts = <GIC_SPI 226 IRQ_TYPE_LEVEL_LOW>;	/* BGF_EINT */
		clocks = <&pericfg CLK_PERI_APDMA>;
		clock-names = "wifi-dma";
		status = "okay";
	};
};
```

Notes:

* Without the second `reg` the driver still probes and runs the whole
  firmware download over PIO, logging
  `no AP-DMA window in DT, using PIO`.
* `compatible = "mediatek,wifi"` is the downstream DTS spelling; the driver
  also matches `"mediatek,WIFI"` because the ROM carries that string
  (`vmlinux.strings:5580,42452`).
* No GPIO, regulator or reset property is needed by this driver. The
  `consys` node from the canonical fragment supplies power/clock/reset for
  the subsystem and is linked to this device when present.

## 7. Runtime firmware blobs (userspace must supply; none are added)

| Path in the ROM | Name the kernel requests | Notes |
|---|---|---|
| `/etc/firmware/WIFI_RAM_CODE_6735` | `WIFI_RAM_CODE_6735` | requested with `request_firmware()`, i.e. resolved through the kernel firmware search path (`/lib/firmware` by default); mirror or symlink the ROM file there. This is the exact chip-suffixed name produced by `kalFirmwareOpen()` + `glGetChipInfo()` (`gl_kal.c:851-856`, `ahb.c:534-548`). If the HIF reports an unlisted chip id the name becomes `WIFI_RAM_CODE_SOC`. Declared with `MODULE_FIRMWARE("WIFI_RAM_CODE_6735")`. |
| `/etc/firmware/wifi_fw.cfg` | — | Only read by the deferred 802.11 stack (`CFG_SUPPORT_CFG_FILE`, `include/fwcfg.h:34`); this pass does not open it. |

The image may be either the divided (`MTKW` header) or the flat form; both
paths are implemented.

## 8. Deferred work (explicitly not stubbed)

1. **802.11 MAC/management engine** (`gen2/nic/`, `gen2/mgmt/`,
   `gl_cfg80211.c`, `gl_wext.c`, `gl_vendor.c`, `gl_p2p*`, ~130k lines).
   Not ported, and no `wiphy`/`net_device` is registered: without scan, AIS,
   TX/RX and key handling such an interface would be non-functional and
   misleading. Consequently:
   * RX frames from the firmware are drained and counted
     (`hif->rx_dropped`), not delivered;
   * only the firmware-download command subset is sent; the generic command
     queue, TX resource accounting (WTSR-based), OID handling, NVRAM and
     P2P/AP setup do not exist;
   * `register_set_dbg_level_handler()` / `register_set_p2p_mode_handler()`
     exports expected by `wmt_chrdev_wifi.c` are not provided.
   Next step: port `nic/` + `mgmt/` + `gl_cfg80211.c`/`gl_wext.c`, then
   register a wiphy and replace the RX drain with delivery.
2. **Firmware power-off handshake** (`wlanAdapterStop`: `CMD_NIC_POWER_CTRL`,
   FW-own, pending-frame flush, `nic_pwr_mgt.c:639-676`). Requires the
   command path above; until then WMT simply powers the subsystem down after
   the WLAN remove callback returns.
3. **EMI MPU protection** of the 512 KiB CONSYS EMI window
   (`ahb_pdma.c:190-201`). Nura has no `emi_mpu` driver; this configures the
   firmware's own memory protection and is not needed by the host download
   path. The WMT/CONSYS core owns the EMI window (`mtk_wcn_consys_emi_*`).
4. **`mtk_wcn_consys_hw_wifi_paldo_ctrl()`** (downstream AHB probe,
   `ahb.c:1327-1332`): the frozen CONSYS API has no equivalent, and the
   canonical `consys` node already models `vcn33_bt`/`vcn33_wifi`, so rail
   handling stays with WMT.
5. **GDMA** (`hif_gdma.h`) and the `CONF_HIF_DMA_INT` interrupt-driven PDMA
   mode: not used by the MT6735 downstream build (`CONF_HIF_DMA_INT 0`,
   `hif.h:79`), which polls like this driver.
6. **Development-only paths**: `CONF_HIF_DEV_MISC` chardev, automatic
   loopback test, full PDMA/CONNSYS register dump, procfs debug spam.
7. **`glDoChipReset()` user-space guard** (`current->comm` check in
   `gl_rst.h:80-89`): the assert is issued from a work item, so the
   "called from mtk_wmtd" guard is not applicable.

## 9. Patch

`$M/patches/wcn-wlan.patch` (`git diff --binary`):

```
 drivers/misc/mediatek/connectivity/wlan/Kconfig    |    2
 drivers/misc/mediatek/connectivity/wlan/Makefile   |    4
 drivers/misc/mediatek/connectivity/wlan/wlan_ahb.c |  758 ++++++++++++++++++++
 drivers/misc/mediatek/connectivity/wlan/wlan_fw.c  |  390 ++++++++++
 drivers/misc/mediatek/connectivity/wlan/wlan_fw.h  |  124 +++
 drivers/misc/mediatek/connectivity/wlan/wlan_hif.h |  248 +++++++
 .../misc/mediatek/connectivity/wlan/wlan_main.c    |  283 +++++++
 7 files changed, 1809 insertions(+)
```

The patch is relative to the coordinator scaffold (scaffold snapshotted in the
index before the work); it contains only this leaf's changes.
