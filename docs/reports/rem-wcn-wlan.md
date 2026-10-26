# rem-wcn-wlan — MediaTek WLAN MAC / cfg80211 management

## 1. Assigned driver and files changed

Leaf: `rem-wcn-wlan` (WLAN MAC/management + gen2 firmware command channel) —
worktree `$M/worktrees/rem-wcn-wlan`, baseline `parity-baseline-all.patch`
(HEAD `d4336fc5e`, which additionally carries the coordinator's two baseline
fixups `drivers/misc/Kconfig` + `drivers/misc/Makefile`).

All paths under `drivers/misc/mediatek/connectivity/wlan/`:

| File | Role |
|---|---|
| `wlan_mac.c` (new) | wiphy, cfg80211 ops (scan/abort/connect/disconnect/get_station), RX dispatch (data/mgmt/event), firmware-up/down |
| `wlan_mac.h` (new) | HIF TX/RX header structs, TC/rate/RCPI definitions, command state, AP parameters, `struct mtk_wlan_mac` |
| `wlan_mgmt.c` (new) | software scan, probe/auth/assoc/deauth builders, auth/assoc state machine, firmware BSS/STA sync, quiesce |
| `wlan_netdev.c` (new) | Ethernet netdev + wireless_dev, `ndo_open/stop/start_xmit/set_rx_mode`, carrier, RX to stack |
| `wlan_txrx.c` (new) | HIF TX header fill, TC credits, 802.3 data TX (firmware builds the 802.11 header), 802.11→802.3 RX, TX worker |
| `wlan_cmd.c` (new) | gen2 command/event channel: PDU framing, sequence matching, timeout/abort; BSS activate, BSS info, STA record update/remove |
| `wlan_cmd.h` (new) | wire structs (WIFI_CMD_T, WIFI_EVENT_T, CMD_SET_BSS_INFO, CMD_UPDATE_STA_RECORD_T, …) and CID/event constants |
| `wlan_ahb.c` | RX buffer handed to `mtk_wlan_mac_rx()`, WTSR0/1 → TC credits, RX buffer slack |
| `wlan_hif.h` | MAC pointer, RX-drain/mac hooks, appended-DW length |
| `wlan_main.c` | wiphy attach/detach from probe/remove, reset/fw-down hooks, optional rfkill |
| `Kconfig` | exact supported-feature help text; `select RFKILL` removed (recursive dep, see §4) |
| `Makefile` | links the five new objects into `mtk_wcn_wlan.o` |

## 2. ROM / downstream evidence

### 2.1 Downstream source (`$M/src/downstream-grandpplte`, Android 6.0.1, 3.18)

WLAN gen2 root: `drivers/misc/mediatek/connectivity/wlan/gen2/`.

- cfg80211 glue / wiphy: `os/linux/gl_init.c:1569-1645` (wiphy_new, max_scan_ssids,
  `interface_modes`, bands, cipher suites, `wiphy_register`), `gl_init.c:790-865`
  (2.4 GHz rate/channel tables), `gl_init.c:1583-1690` (netdev), `gl_cfg80211.c`
  (`mtk_wlan_ops`).
- connect/scan status reporting: `os/linux/gl_kal.c:1660-1810`
  (`cfg80211_connect_result` 1730/1794, `cfg80211_disconnected` 1762,
  `cfg80211_scan_done` 1781, `netif_carrier_on/off`), `gl_init.c:2139` (abort scan).
- RX to stack: `os/linux/gl_kal.c:1509-1617` (`kalRxIndicatePkts`,
  `eth_type_trans` 1594, `netif_rx_ni`/`netif_rx` 1615/1617).
- TX: `os/linux/gl_init.c:1864-1998` (`wlanHardStartXmit`), `gl_init.c:2286-2292`
  (`wlan_netdev_ops` incl. `ndo_set_rx_mode`), `gl_init.c:1698-1770`
  (`wlanSetMulticastList` → `wlanoidSetCurrentPacketFilter`).
- HIF TX header: `nic/nic_tx.c:1400-1480` (frame header fill; 802.3 branch
  1404-1416, `fgIs802_11` 1447-1448, ack/basic-rate 1461-1474),
  `nic/nic_tx.c:2000-2018` (`nicTxFillMsduInfo`: data frames use
  `fgIs802_11 = FALSE`, `ucMacHeaderLength = ETH_HLEN`),
  `nic/nic_tx.c:862-887` (TC0-3/TC5 → port 0, TC4 → port 1),
  `nic/nic_tx.c:559-571` (2-buffer TC4 watermark for management/802.1x),
  `nic/nic_tx.c:633-697` (resource polling), `nic/nic_tx.c:698-738` (release from
  WTSR).
- Command transport: `include/nic/cmd_buf.h:79-105` (CMD_INFO_T),
  `include/nic/hif_tx.h:98` (`CMD_HDR_SIZE = sizeof(WIFI_CMD_T)`),
  `common/wlan_lib.c:2037-2098` (`wlanSendCommand`: TC4, one credit, pending for
  queries), `nic/nic_tx.c:1597-1761` (`nicTxCmd`: port 1, header fields, DW
  padding), `nic/nic.c:1594-1609` (`nicIncreaseCmdSeqNum`),
  `nic/nic.c:1406-1437` (`nicGetPendingCmdInfo`, sequence match).
- Command completion: `include/nic_cmd_event.h:919-941` (WIFI_CMD_T,
  WIFI_EVENT_T; first DW of WIFI_EVENT_T shared with HIF_RX_HEADER_T,
  `nic_cmd_event.h:613`), `nic_cmd_event.h:660` (`EVENT_HDR_SIZE`),
  `nic_cmd_event.h:1175-1179` (EVENT_CMD_RESULT, status 0/1/2),
  `nic/nic_rx.c:1750-1830` (`nicRxProcessEventPacket`, `EVENT_ID_CMD_RESULT`
  matched by sequence), `os/linux/gl_kal.c:2445-2449` (5 s OID/command wait).
- Commands: `include/nic_cmd_event.h:678-702`
  (CID `BSS_ACTIVATE_CTRL` 0x15, `SET_BSS_INFO` 0x16, `UPDATE_STA_RECORD` 0x17,
  `REMOVE_STA_RECORD` 0x18), `nic_cmd_event.h:1414-1418`
  (CMD_BSS_ACTIVATE_CTRL), `1420-1460` (CMD_SET_BSS_RLM_PARAM_T + CMD_SET_BSS_INFO),
  `1462-1488` (CMD_UPDATE_STA_RECORD_T), `1490-1494` (CMD_REMOVE_STA_RECORD_T).
- Command producers: `nic/nic.c:1902-1958` (`nicActivateNetwork` /
  `nicDeactivateNetwork`), `nic/nic.c:1966-2103` (`nicUpdateBss`, incl.
  `eAuthMode - 1` and `ucStaRecIdxOfAP`), `mgmt/cnm_mem.c:1032-1056`
  (`cnmStaRecChangeState` → update command; state 1 and 3 are synced, state 2 is
  local), `mgmt/cnm_mem.c:1129-1215` (`cnmStaSendUpdateCmd` field fill),
  `mgmt/cnm_mem.c:1220-1238` (`cnmStaSendRemoveCmd`), `mgmt/cnm_mem.c:906-917` +
  `853-890` (free with/without chip sync), `mgmt/ais_fsm.c:1372-1384` (state 1
  before auth), `ais_fsm.c:1873/1885` (network activate/deactivate),
  `ais_fsm.c:3353-3358` (state 3 then `nicUpdateBss` then media indication),
  `ais_fsm.c:4072-4080` (disconnect: BSS info disconnected).
- AP parameters: `mgmt/rate.c:80-96` (`aucDataRate`), `rate.c:178-254`
  (`rateGetRateSetFromIEs`), `mgmt/bss.c:566-680`
  (`bssCreateStaRecFromBssDesc`: PHY type set, non-HT PHY, rate sets),
  `mgmt/rlm.c:1713-1745` (`rlmFillSyncCmdParam`), `rlm.c:1227-1314`
  (short preamble/ERP protection from the AP IEs), `rlm.c:1571/1681`
  (short slot time from the capability field).
- TCs and credits: `include/nic/nic_tx.h:340-364` (TC indices), `nic_tx.h:298-303`
  (`{1,36,1,1,4,1}` for MT6628 with `CFG_SRAM_SIZE_OPTION == 1`), `gen2/Makefile:84`
  selects option 1 for `CONFIG_ARCH_MT6735`; `include/nic/hal.h:511-521`
  (`HAL_READ_TX_RELEASED_COUNT`); `nic/que_mgt.c:1296-1300` (non-QoS STA → AC1/TC1).
- RX: `nic/nic_rx.c:2870-2895` (`ALIGN_4(pktLen) + 4` read), `nic_rx.c:1044-1085`
  (payload = header + 12 + header offset), `nic_rx.c:2734-2823` (packet-type
  dispatch), `include/nic/hif_rx.h:89-160/176/201` (header layout, channel macro),
  `include/nic/hif_tx.h:106-185` (TX header/flags/packet types).
- Management frames: `mgmt/auth.c:258-325` (auth compose), `auth.c:428-445`
  (`fgIs802_11`, `fgIsBasicRate`), `auth.c:932-1139` (deauth);
  `mgmt/assoc.c:505-575` (capability = ESS, no privacy), `assoc.c:587-731` (SSID,
  rates, ext rates), `assoc.c:747-823` + `925-934` (assoc request, 802.11 format,
  basic rate), `assoc.c:1183-1200` (AID masking and the Kingnet workaround).
- Scan: `mgmt/scan_fsm.c:377-455` (firmware `CMD_ID_SCAN_REQ`), `mgmt/scan.c`
  `scanAddScanResult()` 2074-2193, DS/HT channel preference 1975-1987.
- Constants: `include/config.h:1179` (`CFG_RX_MAX_PKT_SIZE` 28+2312+12),
  `include/nic/mac.h:227` (listen interval 10), `mac.h:234-247` (500 kb/s rate
  codes), `mac.h:250-251` (rate mask/basic bit), `mac.h:261-263`
  (STA_STATE_1/3), `mac.h:265-267/2533-2534` (RCPI→dBm, bounds 220/110),
  `include/nic/wlan_def.h:293-388` (PHY type bits/sets and RATE_SET_* bits),
  `wlan_def.h:547-555` (PHY indices), `wlan_def.h:563-571` (OP modes),
  `wlan_def.h:582-587` (bands), `wlan_def.h:769-783` (STA types),
  `include/wlan_oid.h:302-335` (AUTH_MODE_OPEN and encryption status),
  `include/nic/que_mgt.h:308-309` (BMCAST/NOT_FOUND indices).
- HIF register map: `include/nic/mtreg.h:81-260` (`MCR_*`, `WRPLR`, `WTSR0/1`,
  `HSTCR`), `os/linux/hif/ahb/include/hif.h:217-218` (HIF base/length),
  `hif_pdma.h:55/74` (AP-DMA base `0x11000080`, length `0x54`).
- Board DTS: `arch/arm/boot/dts/mt6735.dtsi:1477-1483` (`wifi@180f0000`,
  IRQ `GIC_SPI 226 LEVEL_LOW`, clock `PERI_APDMA`/`wifi-dma`).

### 2.2 ROM ELF symbols (`$M/zip/vmlinux.elf`, addresses from `llvm-nm`)

The ROM contains the same cfg80211/STA implementation and the same firmware
command path this slice ports:

```
c05aac98 T mtk_cfg80211_scan          c05aaeb0 T mtk_cfg80211_connect
c05ab9dc T mtk_cfg80211_disconnect    c05aa3bc T mtk_cfg80211_get_station
c0594064 T wlanHardStartXmit          c05945a0 t wlanSetMulticastList
c0594d38 t wlanNetUnregister          c059858c T kalIndicateStatusAndComplete
c05840c0 T nicTxMsduQueue             c0588a28 T nicRxProcessRFBs
c05c47d0 T authSendAuthFrame          c05c51fc T authSendDeauthFrame
c05c2388 T assocSendReAssocReqFrame   c05c2250 T assocBuildCapabilityInfo
c05bffac T aisFsmRunEventJoinComplete c05bf77c T aisFsmDisconnect
c05dc314 T scnSendScanReq             c055e118 T wlanIST
c055e238 T wlanSendCommand            c055e5fc T wlanProcessCommandQueue
c057fa68 T nicUpdateBss               c057f840 T nicActivateNetwork
c057f94c T nicDeactivateNetwork       c0583698 T nicTxCmd
c058b6dc T cmdBufAllocateCmdInfo      c058b8e8 T cmdBufFreeCmdInfo
c057ee5c T nicIncreaseCmdSeqNum       c057e864 T nicGetPendingCmdInfo
c05879dc T nicRxProcessEventPacket    c0582a84 T nicTxAcquireResource
c0582dfc T nicTxReleaseResource       c05cdcc0 T rlmFillSyncCmdParam
c05c9644 T cnmStaRecAlloc             c05c9c74 T cnmStaRecChangeState
c05c9a1c T cnmStaFreeAllStaByNetType  c05cbb44 T rateGetRateSetFromIEs
c055ef44 T wlanQueryInformation       c056768c T wlanoidSetAuthMode
c056d4ac T wlanoidQueryCurrentAddr    c056bc50 T wlanoidSetCurrentAddr
c05639e4 T wlanoidQueryStaStatistics
```

`cnmStaSendUpdateCmd`/`cnmStaSendRemoveCmd` are static in the source and not in
the recovered kallsyms, but their callers (`cnmStaRecChangeState`,
`cnmStaFreeAllStaByNetType`, `cnmStaRecAlloc`) and the command transport
(`wlanSendCommand`, `nicTxCmd`, `nicUpdateBss`, `nicActivateNetwork`) are.

## 3. Parity matrix

Legend: ROM implementation = downstream + ROM evidence; Nura before = baseline tree.

| # | Behaviour | ROM / downstream implementation | Nura before | After (this patch) | Status |
|---|---|---|---|---|---|
| 1 | cfg80211 wiphy + one netdev + one wireless_dev, `NL80211_IFTYPE_STATION` | `gl_init.c:1569-1645`; ROM `mtk_cfg80211_*` | none | `wlan_mac.c:mtk_wlan_mac_attach()` creates/registers the wiphy; `wlan_netdev.c` creates one netdev whose priv is the `wireless_dev` with `iftype = STATION`; `wiphy->interface_modes = BIT(STATION)` | done |
| 1a | Bands/channels/rates limited to real support | 2.4 GHz table + 12 rates, `gl_init.c:790-865`; MT6737T is 2.4 GHz only | none | only `NL80211_BAND_2GHZ` (ch 1-14, ch14 `NO_IR`/`NO_OFDM`), 12 legacy rates 1-54 Mb/s; no 5 GHz pointer; no HT cap | done |
| 1b | No hardware scan, no key offload | ROM has `mtk_cfg80211_scan` (driver scan op) and cipher suites | none | only `.scan`/`.abort_scan` (software scan); no `.hw_scan`/`.sched_scan*`/`.remain_on_channel`; `n_cipher_suites = 0`, `cipher_suites = NULL` | done |
| 2 | Software scan: real probe-request frames through the HIF TX path | firmware scan via `CMD_ID_SCAN_REQ` (`scan_fsm.c:377-455`) | none | `wlan_mgmt.c` builds probe requests (SSID, rates, DS, ext rates + `req->ie`) and sends them via `mtk_wlan_mgmt_tx()` (TC4/HIF port 1); passive scans (`n_ssids == 0`) and `NO_IR` channels transmit nothing | done |
| 2a | Scan results parsed from HIF RX and reported | `scanAddScanResult()` `scan.c:2074-2193`, reported via `cfg80211_inform_bss` (`gl_kal.c:1708`) | none | beacons/probe responses in `mtk_wlan_rx_mgmt()`; channel from DS parameter IE else HIF RX channel (`scan.c:1975-1987`), `cfg80211_inform_bss_frame_data()` with RCPI in mBm | done |
| 2b | Abort scan, exactly-once completion, no `scan_req` leak, fw-down/unload | `gl_init.c:2139` aborts with `cfg80211_scan_done(…, TRUE)` | none | `scan_aborted` polled every 10 ms while dwelling; `mtk_wlan_scan_complete()` is the only `cfg80211_scan_done()` caller; `mtk_wlan_mgmt_quiesce()` flushes the worker and reports a never-run request | done |
| 2c | Per-channel RF tuning during scan | firmware scans and switches channels (`CMD_ID_SCAN_REQ`) | none | probe requests carry each requested channel number, but the host cannot retune the firmware; results depend on what the firmware receives on its current channel | deferred (scan command not ported) |
| 3 | Open (no-encryption) auth/assoc, result/timeout, abort, deauth | `auth.c:258-445`, `assoc.c:505-934`, `ais_fsm.c`, `gl_kal.c:1727-1795` | none | auth (trans 1) → assoc req; responses matched by subtype + SA; success `cfg80211_connect_bss()` with IEs and the BSS reference; rejection → status; timeout → `-1` + reason; local disconnect sends deauth and reports; AP deauth/disassoc → `cfg80211_disconnected()` | done |
| 3a | Encryption material must be rejected | ROM has key offload (`add_key` …) | none | `-EOPNOTSUPP` for `privacy`, `key`, `key_len`, `mfp`, WPA versions/ciphers/AKMs, RSN IE, WPA vendor IE, and for a BSS with the privacy bit | done |
| 3b | Firmware command/event channel | `wlanSendCommand`/`nicTxCmd`/`nicRxProcessEventPacket`; ROM `c055e238`/`c0583698`/`c05879dc` | none | `wlan_cmd.c`: WIFI_CMD_T + payload on TC4/port 1, length DW-padded, one command in flight under a mutex, result matched by `EVENT_ID_CMD_RESULT` + sequence, 5 s timeout, aborted on fw-down and cancelled on local disconnect; no polling in atomic context | done |
| 3c | Network activation | `nicActivateNetwork`/`nicDeactivateNetwork` (`nic.c:1902-1958`), sent when AIS starts (`ais_fsm.c:1873/1885`) | none | `CMD_BSS_ACTIVATE_CTRL` (0x15) active=1 at join start, active=0 on teardown | done |
| 3d | AP peer record | `CMD_UPDATE_STA_RECORD_T` (`nic_cmd_event.h:1462-1488`) filled by `cnmStaSendUpdateCmd()` (`cnm_mem.c:1129-1215`), state 1 before auth (`ais_fsm.c:1372-1384`) and state 3 + AID/RCPI after assoc (`cnm_mem.c:1032-1056`) | none | index 0 (`cnmStaRecAlloc` first free slot), type `STA_TYPE_LEGACY_AP`, BSSID, AID (masked with the `assoc.c:1183-1200` workaround), listen interval 10, desired PHY/rate sets from the AP IEs, state 1 then state 3 (state 3 waits for the result event) | done |
| 3e | BSS state | `CMD_SET_BSS_INFO` (`nic_cmd_event.h:1437-1460`) from `nicUpdateBss()` (`nic.c:1966-2090`) with `rlmFillSyncCmdParam()`; connected after assoc (`ais_fsm.c:3353-3358`), disconnected on teardown (`ais_fsm.c:4072-4080`) | none | connected/disconnected BSS info with SSID/BSSID, connected state, operational/basic rate sets, PHY type set, non-HT basic PHY, auth mode 0, encryption disabled, own MAC, STA index, and the full RLM block (2.4 GHz, primary channel, SCN, ERP protection, short preamble/slot, check id 0x72) | done |
| 3f | Teardown so a reconnect starts clean | AIS disconnect sends BSS info disconnected and frees the record without chip sync (`cnmStaRecFree(..., FALSE)`, `ais_fsm.c:4975`); `CMD_REMOVE_STA_RECORD` is used by P2P/TDLS (`p2p_func.c:1103`, `tdls.c:4331`) | none | `mtk_wlan_cmd_teardown()`: `CMD_SET_BSS_INFO` disconnected → `CMD_REMOVE_STA_RECORD` (0x18, documented payload) → `CMD_BSS_ACTIVATE_CTRL` active=0. The remove command is an intentional extension over AIS (see §5) | done (remove command is an extension, documented) |
| 4 | TX: data frames, TX header, per-AC credits, netdev ops, carrier | `nic_tx.c:1400-1480` + `2000-2018` (802.3 data, firmware builds the header), `que_mgt.c:1296-1300`, `nic_tx.h:298-303`, `gl_init.c:1864-1992` | none | data frames are copied verbatim behind the 16-byte HIF header in 802.3 format (`is_80211 = false`, `wlan_header_length = ETH_HLEN`, EtherType offset 14) carrying the real STA record index; TC1 (36 credits), mgmt/commands on TC4 (4 credits, 2-buffer watermark for mgmt, 1 for commands); credits replenished from WTSR0/1; `ndo_open/stop/start_xmit`, carrier follows association, queue stops at depth | done |
| 4a | Multicast filter | `wlanSetMulticastList()` → `wlanoidSetCurrentPacketFilter` (`gl_init.c:1698-1770`), needs a firmware OID | none | `ndo_set_rx_mode` accepts the list but there is no filter register in this HIF and no OID path; documented no-op | partial (no programming path) |
| 4b | RX 802.11 → 802.3, `eth_type_trans`, `netif_rx`, bounds checks | `nic_rx.c:2734-2823`, `gl_kal.c:1509-1617` | HIF drained and counted only | 802.3 buffers pass through; 802.11 FromDS frames converted (QoS/A4 lengths, A-MSDU drop, LLC/SNAP check); lengths validated against WRPLR, `packet_len`, header offset and the HW-appended DW; `eth_type_trans()` + `netif_rx()` | done |
| 4c | Firmware event buffers consumed | `nicRxProcessEventPacket()` (`nic_rx.c:1750`) | counted as drops | `HIF_RX_PKT_TYPE_EVENT` is dispatched to `mtk_wlan_cmd_event()`; the event header at offset 0 and the body at `EVENT_HDR_SIZE = 8` (shared first DW with HIF_RX_HEADER_T); command results are matched, other events ignored | done |
| 5 | `get_station` real RCPI, no invented values | `mtk_cfg80211_get_station` (ROM `c05aa3bc`), `wlanoidQueryStaStatistics` (ROM `c05639e4`) | none | fills `NL80211_STA_INFO_SIGNAL` from the HIF RX-header RCPI (`mac.h:2533`) only when valid, plus `CONNECTED_TIME`; rate/counters omitted because the HIF does not provide them | done |
| 6 | MAC address | NVRAM `WIFI` section `aucMacAddress` (`gl_init.c:1119-1150`) → `wlanoidSetCurrentAddr` | none | `local-mac-address` from DT if present, else `eth_hw_addr_random()` (addr_assign_type random) with an explicit log; no hard-coded OUI; no firmware getter exists in this port | done |
| 7 | Lifecycle: fw-down/reset disconnect + stop netdev; unload without UAF | `gl_rst.c` reset callback | none | `mtk_wlan_mac_fw_down()` → quiesce (command aborted, scan aborted, connect timeout, disconnect reported) → `link_work` unregisters the netdev; detach flushes all work before `wiphy_unregister()`/`wiphy_free()`; IRQ freed before MAC teardown | done |
| 7a | PM suspend/resume consistent with the HIF | ROM suspend/resume + WoWLAN | HIF masks its interrupt on suspend | unchanged HIF suspend/resume (interrupt masked/restored); MAC keeps its state, no WoWLAN advertised | partial (no WoWLAN; hardware behaviour unverified) |
| 8 | Kconfig describes exactly the supported set | – | help only described the HIF | help text names station-only, 2.4 GHz, open networks, software scan, no AP/IBSS/P2P/hw-scan/WoWLAN; notes cfg80211-only userspace and optional RFKILL | done |
| 9 | Userspace ABI | cfg80211 (`iw`, `wpa_supplicant`); ROM also exposes `/dev/wmtWifi` for power | cfg80211 only | no custom WLAN control tool and no private char device/ioctl is added; the ABI is cfg80211/nl80211 (stated in Kconfig help too) | done |

## 4. Compile verification

Environment:

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export TMPDIR=$M/tmp-wlan-build          # /tmp was full on this host (tmpfs)
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/rem-wcn-wlan
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e CRC32 -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT \
  -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.o
```

**(a) `CONFIG_RFKILL=y`** (`./scripts/config -e RFKILL -e RFKILL_INPUT` plus the
configuration above, then `olddefconfig` and the same make command).

Result: `CC [M]` for `wlan_main.o wlan_ahb.o wlan_fw.o wlan_mac.o wlan_netdev.o
wlan_mgmt.o wlan_txrx.o wlan_cmd.o` and
`LD [M] drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.o`; zero warnings,
zero errors. `.config`: `CONFIG_RFKILL=y`, `CONFIG_CFG80211=y`,
`CONFIG_MTK_WCN_WMT=m`, `CONFIG_MTK_WCN_WLAN=m`.

**(b) `CONFIG_RFKILL` off** (`./scripts/config -d RFKILL`, plus `-d MTK_WCN_GPS
-d MTK_WCN_FM` because those Kconfigs `select RFKILL`, then `olddefconfig` and
the same make command).

Result: same eight `CC [M]` lines plus `LD [M] mtk_wcn_wlan.o`; zero warnings,
zero errors. `.config`: `# CONFIG_RFKILL is not set`, `CONFIG_CFG80211=y`,
`CONFIG_MTK_WCN_WLAN=m`.

Notes:

- The acceptance build uses the composite target
  `drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.o`: it compiles all
  objects and performs the `LD [M]` step. Individual object targets were also run
  (`wlan_mac.o` and `wlan_ahb.o`) and compile clean with `W=1`, but they do not
  link the module, so the composite target is the one used for acceptance.
- The tree needs the coordinator's two baseline fixups (already in HEAD, not part
  of this patch): `drivers/misc/Kconfig` sourcing `drivers/misc/mediatek/Kconfig`,
  and `drivers/misc/Makefile` descending into `mediatek/`.
- `MTK_WCN_WLAN` no longer `select RFKILL`: that select created a Kconfig
  recursive dependency (`CFG80211 depends on RFKILL || !RFKILL`) and made
  `olddefconfig` fail outright. rfkill is optional and its callbacks are compiled
  under `IS_ENABLED(CONFIG_RFKILL)`.
- `grep -nE 'TODO|FIXME|XXX|stub|not implemented'` over the touched files → no
  matches.
- `grep -c 'GIT binary patch' rem-wcn-wlan.patch` → `0`; the patch contains only
  the WLAN source files (no build artifacts, no config, no generated files).

## 5. Inferred / unverified on hardware

No phone is attached. Everything below is source-level parity only.

1. **Firmware acceptance of the command PDUs.** The frame layout, CIDs, payload
   layouts, sequence matching and TC4/port-1 transport are copied from the
   downstream driver and the ROM contains the same functions
   (`wlanSendCommand`, `nicTxCmd`, `nicRxProcessEventPacket`, `nicUpdateBss`,
   `nicActivateNetwork`, `cnmStaRecChangeState`).  Whether this particular
   ROM firmware revision accepts the exact field values derived here (rate
   sets, PHY type, RLM block, empty HT fields) is only verifiable on hardware.
2. **AIS command order.** Downstream order is reproduced: network activate →
   STA record state 1 → auth → assoc → STA record state 3 (waits for the result)
   → BSS info connected.  `CMD_REMOVE_STA_RECORD` on teardown is an extension:
   downstream AIS frees the record without chip sync (`cnmStaRecFree(..., FALSE)`,
   `ais_fsm.c:4975`) and uses the remove command only for P2P/TDLS
   (`p2p_func.c:1103`, `tdls.c:4331`).  It is a documented firmware command with
   the documented payload and is sent after the BSS is marked disconnected; if
   the firmware rejects it, the result event is ignored and the BSS-disconnect
   command still provides downstream's teardown semantics.
3. **Data TX mode.** Data frames now go out as 802.3 with
   `fgIs802_11 = FALSE`, `ucMacHeaderLength = ETH_HLEN` and EtherType offset 14,
   exactly `nic_tx.c:2000-2018`/`1404-1416`; the firmware builds the 802.11
   header.  The HIF header carries the real STA record index from
   `CMD_UPDATE_STA_RECORD`.  Whether the firmware forwards those frames without
   a prior `CMD_ID_SET_BSS_RLM_PARAM`/WMM setup is unverified; the embedded RLM
   block of `CMD_SET_BSS_INFO` carries the channel/band/protection values.
4. **Scan coverage.** The host cannot retune the RF (the scan command is not
   ported); probe requests are emitted per requested channel number, but
   responses are only those the firmware receives on its current channel.
5. **RCPI values.** `RCPI → dBm` uses the documented `(rcpi >> 1) - 110` with
   the 220/110 bounds; the accuracy of the firmware-reported RCPI is unverified.
6. **RX format.** The 802.11→802.3 converter only runs if the firmware sets the
   `HIF_RX_HDR_80211_HEADER_FORMAT` bit; the normal 802.3 path is the one
   downstream uses.  Both paths are bounds-checked.
7. **Rate reporting.** `get_station` reports no rate because the HIF RX header has
   no rate field; downstream derives rates from firmware statistics
   (`wlanoidQueryStaStatistics`), which this port does not issue.
8. **Multicast/promiscuous filtering.** No programming path (see row 4a).
9. **PM.** Suspend masks the HIF interrupt; there is no WoWLAN and no MAC-level
   suspend handling, so behaviour with an active association across suspend is
   unverified.
10. **Firmware blob.** The bring-up path (base port) loads `WIFI_RAM_CODE_6735`;
    the blob itself was not inspected or changed here.

## 6. DTS-FRAGMENT

Not committed (shared DTS is out of scope). The downstream node is
`arch/arm/boot/dts/mt6735.dtsi:1477-1483`; the address/IRQ/clock values come from
there, the AP-DMA window from `hif_pdma.h:55/74`, and the clock provider from the
Nura tree's own `pericfg` (`include/dt-bindings/clock/mediatek,mt6735-pericfg.h:18`,
`CLK_PERI_APDMA`).  The `soc` node in `mt6735.dtsi` has no label, hence the path
reference.

```dts
&{/soc} {
	/*
	 * WLAN host interface (MT6627 gen2 on CONSYS).
	 * reg[0] = HIF registers, 0x5c bytes           (hif.h:217-218)
	 * reg[1] = AP-DMA channel window, 0x54 bytes    (hif_pdma.h:55,74)
	 * IRQ    = BGF_EINT, GIC SPI 226, level low     (mt6735.dtsi:1480)
	 */
	wifi@180f0000 {
		compatible = "mediatek,wifi";
		reg = <0x180f0000 0x5c>, <0x11000080 0x54>;
		interrupts = <GIC_SPI 226 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&pericfg CLK_PERI_APDMA>;
		clock-names = "wifi-dma";

		/*
		 * Optional: without it the driver uses a random
		 * locally-administered address (reported at probe).
		 * local-mac-address = [00 00 00 00 00 00];
		 */
	};
};
```

If the integrator prefers PIO, drop the second `reg` entry; the driver then logs
"no AP-DMA window in DT, using PIO" and works without it.

## 7. Patch

`$M/patches/rem-wcn-wlan.patch` (generated with `git add -N` for the new files
and `git diff HEAD --binary`; WLAN source files only, no binary hunks).
