# wcn-fm — MT6627 FM radio over WMT/STP (V4L2 radio device)

Worktree: `$M/worktrees/wcn-fm` (Nura `v7.0.10`, arm64, MT6737T / MT6735 family)
Patch: `$M/patches/wcn-fm.patch`
Downstream reference: `$M/src/downstream-grandpplte/drivers/misc/mediatek/fmradio`
ROM: `$M/zip/vmlinux.elf`, `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`, `$M/zip/boot.img`

---

## 1. Assigned driver and files changed

All files live in `drivers/misc/mediatek/connectivity/fm/` (owned subdirectory). The frozen shared
headers (`connectivity/include/*.h`, `btif/include/*.h`, the `Kconfig` skeletons and the
`drivers/misc` hooks) were **not** modified; `connectivity/fm/Kconfig` was already provided by the
coordinator and is unchanged.

| File | Lines | Content |
|---|---|---|
| `mtk_wcn_fm.h` | 408 | Protocol opcodes/event bits/timeouts, MT6627 register map and bit fields, band/seek/space constants, V4L2 private control IDs, `struct mtk_wcn_fm_dev`, RDS ABI records, cross-file prototypes. |
| `mtk_wcn_fm_link.c` | 371 | STP link layer (`fm_link.c` + `fm_eint.c` equivalent): command transmit with per-opcode completion bits, retry and timeouts, event packet parser, RDS record push, WMT message callback (`WMTDRV_FM`), link init/exit. |
| `mtk_wcn_fm_chip.c` | 1644 | MT6627 chip layer: firmware primitive builders, register access, power-up/power-down, DSP patch+coeff download, ROM/ECO/patch version readout, tune, seek, RSSI/stereo, volume, mute, RDS enable, antenna, de-emphasis, mono, soft-mute gain, blending, ramp-down. |
| `mtk_wcn_fm_v4l2.c` | 793 | `v4l2_device` + `video_device` (`/dev/radio0`), ioctls, V4L2 controls, `read()`/`poll()` RDS path, open/close power semantics, rfkill, reset worker, platform driver + module init/exit. |
| `Makefile` | 6 | `obj-$(CONFIG_MTK_WCN_FM) += mtk_wcn_fm.o`, composite object list. |

Module name is `mtk_wcn_fm` (matches `connectivity/fm/Kconfig`). `CONFIG_MTK_WCN_FM=m`.

---

## 2. ROM evidence and downstream citations

### 2.1 ROM symbols (`$M/zip/symbols.txt`, addresses from `llvm-nm` on `vmlinux.elf`)

| Symbol | Address | Meaning |
|---|---|---|
| `mtk_wcn_fm_init` | `0xc0636698` | Module init: `fm_env_setup()`, `platform_device_register()`, `__platform_driver_register()` |
| `mtk_wcn_fm_exit` | `0xc0636720` | `platform_driver_unregister()`, `fm_env_destroy()` |
| `mt_fm_probe` | `0xc0636340` | Platform probe (reads DT node, `alloc_chrdev_region`, `fm_dev_init`) |
| `mt_fm_remove` | `0xc0636270` | Platform remove |
| `do_fm_drv_init` | `0xc055cfa8` | Downstream helper |
| `fm_ops_open/read/ioctl/release/flush/lseek`, `fm_powerup`, `fm_powerdown`, `fm_tune`, `fm_rds_onoff`, `fm_get_hw_info`, `fm_rds_parser`, `fm_eint_handler`, `fm_wholechip_rst_cb`, `fm_subsys_reset_work_func` | lines 24761–24810 of `symbols.txt` | Downstream `core/fm_module.c` / `core/fm_main.c` interface set |
| `fm_i2s_set` | line 24839 | Audio-path hook (deferred, see §3) |
| `mtk_pcm_fm_i2s_open/close/hw_free/prepare/trigger/pointer/copy/silence`, `mtk_asoc_pcm_fm_i2s_new`, `mtk_afe_fm_i2s_probe`, `mtk_fm_i2s_soc_platform_init`, `Audio_fm_i2s_Volume_Set/Get`, `mtk_soc_pcm_fm_i2s_of_ids` | lines 33201–33230, 50552–50565, 54777 | SoC-side FM I2S PCM driver (deferred, see §3) |

`mtk_wcn_fm_init` disassembly (`0xc0636698`–`0xc063671c`) confirms the ROM is a *platform driver*
(`platform_device_register` for the static node + `platform_driver_register`), which is why the port
is a `platform_driver` matching `mediatek,mt_mt6627_fmradio`.

### 2.2 ROM strings (`$M/zip/vmlinux.strings`)

| String | Line | Use |
|---|---|---|
| `/etc/firmware/mt6627/mt6627_fm_v1_patch.bin` … `v5_patch.bin`, `v1_coeff.bin` … `v5_coeff.bin` | 46226-46235 | Exact DSP blob names (see §5.3) |
| `mt6627_DspPatch` | 6807 | DSP download routine |
| `mediatek,mt_mt6627_fmradio` | 45750 | DT compatible |
| `fmradio_ant_sel`, `fm_ant_sel` | 45751–45752 | DT GPIO property and its `gpio_request()` label |
| `No fm_ant_sel control`, `Got fm_ant_sel %d` | 45753–45754 | Probe messages |
| `fm_ops_ioctl` trace strings, `set_fm_ant_sel_gpio from %#x` | 45659–45660 | Ioctl/antenna-switch behaviour |
| `etc/fmr/mt6627_fm_cust.cfg` | 46277 | Optional customer config (deferred) |
| `/etc/fmr/mt6627_fm_channel_param.dat` | 46166 | Optional channel-parameter override (deferred) |
| `/etc/fm_cust.cfg` | 45921 | Generic customer config (not used for MT6627) |
| `mediatek,mt_soc_pcm_fm_i2s`, `mediatek,mt_soc_pcm_fm_i2s_awb`, `mt_soc_pcm_fm_i2s_awb` | 8919, 8927, 89189 | SoC audio PCM nodes (deferred) |
| `sound/soc/mediatek/mt_soc_audio_v3/mt_soc_pcm_fm_i2s.c` | 59190 | Downstream audio source path |

`$M/zip/boot.img` contains the same DT compatible and the node name `mt_fmradio_mt6627`
(4 copies, one per board DTB), confirming the ROM DT node.

Downstream DT node (read-only reference, `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_03.dts:1299-1304`):

```
&soc{
	mt_fmradio_mt6627 {
		compatible = "mediatek,mt_mt6627_fmradio";
		fmradio_ant_sel = <&pio 140 0>;   /* GPIO 140 */
	};
};
```

### 2.3 Downstream source citations

Every constant in the port comes from one of these locations.

**Protocol / link layer**

| Item | Source |
|---|---|
| Command/event packet types `0x01`/`0x04`, opcode map, event-bit map | `inc/fm_link.h:29-97` |
| Command framing `[type][opcode][len:le16][payload]` | `core/fm_link.c:161-220`, `mt6627/pub/mt6627_fm_cmd.c:172-221` |
| Event parser state machine and per-opcode payload handling | `core/fm_link.c:222-420` |
| Retry count, wait bounds, per-command timeouts (3/6/15/3 s …) | `inc/fm_link.h:175-204` |
| RX/TX buffer sizes 256/1024 | `inc/fm_link.h:175-176` |
| EINT handling is only an STP event callback | `core/fm_eint.c:24-44` |
| WMT reset callback `WCNfm_wholechip_rst_cb` | `core/fm_link.c:34-63` |
| `WMTRSTMSG_RESET_START/END` values | downstream `connectivity/common/conn_soc/include/wmt_exp.h:185-191` |
| STP callback registration also clears the channel RX queue; NULL unregisters | downstream `connectivity/common/combo/core/stp_exp.c:173-186`, `gps/stp_chrdev_gps.c:449` |
| Reset recovery flow | `core/fm_main.c:2188-2265` |

**Chip register access primitives** (opcode, payload layout, wait bit)

| Function | Source |
|---|---|
| `FSPI_READ`/`FSPI_WRITE` (0x03/0x04) | `mt6627_fm_cmd.c:839-870` |
| `set_bits` (opcode `0x11` + modify bop) | `mt6627_fm_cmd.c:872-888` |
| `CSPI_READ`/`CSPI_WRITE` top registers (0x21/0x20, space `0x04`) | `mt6627_fm_cmd.c:891-929` |
| `FM_HOST_READ`/`FM_HOST_WRITE` (0x18/0x19) | `mt6627_fm_cmd.c:932-972` |
| Primitive builders (write/udelay/rd_until/modify/top_write/top_rd_until) | `mt6627_fm_cmd.c:30-164` |

**Power-up / power-down**

| Step | Source | Registers / values |
|---|---|---|
| WMT FM function on/off | `mt6627_fm_lib.c:98-126` | `mtk_wcn_wmt_func_on/off(WMTDRV_FM)` |
| CSPI parameter | `mt6627_fm_lib.c:693` | host `0x8013000C = 0x0000801F` |
| Clock/DSP clock-on packet | `mt6627_fm_cmd.c:172-221` | top `0xA10=0xFFFFFFFF`, top `0x60`: `0x30`,`0x35`, poll[3:1]=0xA, `0x15`,`0x05`+10 µs,`0x45`; top `0x68=0x3F`; top `0x50`: `1`, 3 ms, `3`, `7`; top `0x00=0`; `0x70=0x0040`; `0x61` de-emphasis bit12 |
| Chip id / ECO | `mt6627_fm_lib.c:739-750`, `mt6627_fm_lib.c:752` | read `0x62`; accept `0x6627`/`0x6625`; `eco = mtk_wcn_wmt_hwver_get()+1` |
| DSP ROM version | `mt6627_fm_lib.c:447-485` | `0x61` b15/b1/b0; poll `0x84 == 1`; read `0x83[15:8]` |
| Patch path selection | `mt6627_fm_lib.c:487-513`, `775-784` | exact ROM version required |
| Coeff path selection | `mt6627_fm_lib.c:515-547`, `797-809` | fallback allowed; then coeff bytes 4/5 zeroed |
| DSP segment download | `mt6627_fm_lib.c:549-662` | 512-byte segments, `seg_num = len/512 + 1`, opcodes `0x12`/`0x13` |
| Patch/coeff tail | `mt6627_fm_lib.c:800-823` | `rom_ver+1`, `patch_ver = coeff[38]|coeff[39]<<8`, `0x92=0`, `0x90=0x40`, `0x90=0` |
| Digital init packet | `mt6627_fm_cmd.c:422-468` | `0x21=0xE000`, `0xD8=0x03F0`, `0xD9=0x3F04`, `0xDA=0x0014`, `0xDB=0x2A38`, `0x23=0x4000`, `0x6A=0x21`, `0x6B=0x21`, top `0x50=0xF`, `0x61` bits 1/0, 100 ms, poll `0x64[4:0]==2` |
| RF fine tune packet | `mt6627_fm_cmd.c:476-504` | top `0x50=7`, `0x01=0xBEE8`, `0x03=0xF6ED`, `0x15=0x0D80`, `0x16=0x0068`, `0x17=0x092A`, `0x34=0x807F`, `0x35=0x311E`, top `0x50=0xF` |
| FM 2-wire RX audio enable | `mt6627_fm_lib.c:850-851` | `0x9B=0xF9AB`, host `0x80101054=0x00003F35` |
| Power-down | `mt6627_fm_cmd.c:512-551`, `mt6627_fm_lib.c:865-940` | clear `0x69` bit0, host `0x80101054 &= 0xFFFF9FFF`, `0x9B[2:0]=0`, top `0x50=0x330F`, `0x61=1`, `0x6E` ×4, top `0x50` `0`/`0x4000`/`0`, top `0x60` `5`/`0x15`/`0x35`/`0x30`, poll `0x60[3:1]==0`, `0x6B=0` |
| Soft-mute/blend defaults after power-up | `mt6627_fm_lib.c:855-860` | SMG `0x15`, blend `0x164`/`0x19A`/`0x2` |

**Tune / seek / status**

| Behaviour | Source | Registers |
|---|---|---|
| Tune | `mt6627_fm_lib.c:953-1136`, `mt6627_fm_cmd.c:595-630` | top `0x50=7`; `0x0F` bits[10:8]=`0x455`; `0x30` TDD/FDD; top `0x50=0xF`; `0x65[9:0]=(f-6400)*2/10`, `0x65[11:0]=chan_para`; `0x9B=0xF9AB`; host `0x80101054=0x3F35`; `0x63[2:0]=TUNE` |
| Channel side parameter table | `mt6627_fm_lib.c:1903-1949`, index rule `2106-2128` | `0x65[11:0]` (861 entries, 6500–10800 kHz step 50 kHz) |
| TDD channel table/mask | `mt6627_fm_lib.c:1958-2007`, `2044-2073` | `0x30` |
| Post-tune SMG/blend re-apply | `mt6627_fm_lib.c:1073-1118` | coeff `0x31D8`, `0x3154/55/57` |
| Seek command | `mt6627_fm_cmd.c:696-746` | `0x66[10]` dir, `0x66[14:12]` space, `0x66[11]` wrap, `0x66[9:0]` max, `0x67[9:0]` min, `0x63[2:0]=SEEK` |
| Seek result + post-processing | `mt6627_fm_lib.c:1146-1179` (inside `#if 0`), event result in `core/fm_link.c:323-329` | ramp-down, mute/unmute |
| Ramp-down | `mt6627_fm_lib.c:384-445`, `mt6627_fm_cmd.c:559-586` | top `0x50` `7`/`0xF`, `0x0F[15:11]=0`, `0x6A=0`, `0x6B=0`, `0x63[3:0]=0`, ramp-down bit, poll `0x69` STC_DONE, clear, `0x6B=0x21`, `0x6A=0x21` |
| RSSI + stereo indicator | `mt6627_fm_lib.c:1277-1294`, `1364-1379` | read `0x6C`: `[9:0]` RSSI, bit12 stereo; dBm `(rs>511)?((rs-1024)*6>>4):(rs*6>>4)` |
| Volume table (16 entries) | `mt6627_fm_lib.c:1296-1300`, `1301-1349` | `0x7D` |
| Mute | `mt6627_fm_lib.c:333-351` | `0x9C[1:0] = 3` / `0` |
| Mono/stereo force | `mt6627_fm_lib.c:1380-1395` | top `0x50=7`, `0x75` bit3, top `0x50=0xF` |
| Antenna type | `mt6627_fm_lib.c:303-318` | `0x61` bit4 (0 long, 1 short) |
| De-emphasis | `mt6627_fm_cmd.c:213`, `mt6627_fm_lib.c:1740-1747` | `0x61` bit12 |
| Soft-mute gain control | `mt6627_fm_lib.c:1597-1628` | coeff `0x31D8` (`0x1` disables) |
| Blending control | `mt6627_fm_lib.c:1630-1718` | coeff `0x3154`/`0x3155`/`0x3157` |
| RDS enable/disable | `mt6627_fm_rds.c:45-93` | `0x80=6`, `0x63` bit4 |
| RDS data framing | `core/fm_link.c:383-403`, `inc/fm_rds.h:32-45` | event `0x0D`, payload = `rds_rx_t` (148 bytes) |
| RDS init/reset | `mt6627_fm_rds.c:231-240` | software state |

**Board / power**

| Item | Source |
|---|---|
| GPIO `fmradio_ant_sel` (GPIO 140), request + direction out low, high on power-up, low on power-down | `core/fm_module.c:1382-1393`, `151-152`, `164-165` |
| No clocks, regulators or voltages are touched by the FM driver. FM lives in CONSYS; its rails (`vcn18`, `vcn28`, `vcn33_bt`, `vcn33_wifi`) and the `CLK_INFRA_CONNMCU_BUS` clock are owned by the frozen CONSYS/WMT node and are powered by `mtk_wcn_wmt_func_on(WMTDRV_FM)` (`mt6627_fm_lib.c:98-115`). | — |
| Multi-open: only the first opener powers the chip | `core/fm_main.c:254-330` |

---

## 3. Parity matrix

Legend: **done** = implemented and compile-tested against downstream semantics; **partial** = implemented
but a downstream gap or hardware-only unknown remains (see §5); **deferred** = deliberately not
implemented, reason given.

| # | Behaviour | ROM / downstream | Nura before | After (this patch) | Status |
|---|---|---|---|---|---|
| 1 | STP FM channel framing, command TX | `core/fm_link.c:161-220` | none | `mtk_wcn_fm_cmd_tx()` sends on `FM_TASK_INDX` with the downstream `[01][op][len][payload]` frame | done |
| 2 | Event parser + completion bits | `core/fm_link.c:222-420` | none | state machine in `mtk_wcn_fm_link.c`, per-opcode bits incl. `FM_EVT_TUNE_DONE`/`SEEK_DONE`/`CSPI_READ`/`HOST_READ` | done |
| 3 | Retry (1×) and per-command timeouts | `inc/fm_link.h:175-204` | none | `FM_SW_RETRY_CNT`, 3/6/15 s timeouts | done |
| 4 | WMT FM function on/off | `mt6627_fm_lib.c:98-126` | none | `mtk_wcn_wmt_func_on/off(WMTDRV_FM)` around the chip sequence, rollback on failure | done |
| 5 | WMT message callback (`WMTDRV_FM`) | `core/fm_link.c:34-63` | none | `mtk_wcn_wmt_msgcb_reg()`; RESET_START/END; END reschedules power-up + state restore | partial (whole-chip variant only) |
| 6 | MT6627 power-up sequence | `mt6627_fm_lib.c:664-863` | none | full sequence, all registers/values | done |
| 7 | MT6627 power-down sequence | `mt6627_fm_lib.c:865-940` | none | full sequence + `func_off` | done |
| 8 | Chip id / ECO readout | `mt6627_fm_lib.c:739-750`, `752` | none | accepts 0x6627/0x6625, rejects others with `-EIO` | done |
| 9 | DSP ROM version readout | `mt6627_fm_lib.c:447-485` | none | poll `0x84`, read `0x83>>8`, 10 s safety bound | done |
| 10 | DSP patch + coefficient download | `mt6627_fm_lib.c:487-662` | none | `request_firmware()` + 512-byte segmented download, patch-version word, patch-disable fallback | done (blobs not shipped, §5.3) |
| 11 | Tune | `mt6627_fm_lib.c:953-1136` | none | `VIDIOC_S_FREQUENCY` → full tune sequence | done |
| 12 | Seek | `mt6627_fm_lib.c:1146-1179` (`#if 0`), `mt6627_fm_cmd.c:696-746` | none | `VIDIOC_S_HW_FREQ_SEEK` → seek packet, result from event, ramp-down/unmute | partial (downstream had it disabled; hardware-unverified) |
| 13 | RSSI | `mt6627_fm_lib.c:1277-1294` | none | `VIDIOC_G_TUNER.signal` from `0x6C` | done (V4L2 scaling is [INFERENCE]) |
| 14 | Mono/stereo indicator + force | `mt6627_fm_lib.c:1277-1294`, `1364-1395` | none | `G_TUNER.rxsubchans`/`audmode`, `S_TUNER.audmode` → `0x75` bit3 | done |
| 15 | Volume 0–15 | `mt6627_fm_lib.c:1296-1349` | none | `V4L2_CID_AUDIO_VOLUME` → `0x7D` table | done |
| 16 | Mute | `mt6627_fm_lib.c:333-351` | none | `V4L2_CID_AUDIO_MUTE` → `0x9C` | done |
| 17 | RDS on/off | `mt6627_fm_rds.c:45-93` | none | `V4L2_CID_RDS_RECEPTION` → `0x80=6` + `0x63` bit4 | done |
| 18 | RDS data delivery to userspace | `core/fm_link.c:383-403`, `core/fm_main.c:392-426` | none | event `0x0D` payload stored as 148-byte records in a kfifo, `read()`/`poll()` | done (raw records, not decoded) |
| 19 | RDS group decoding (PI/PTY/PS/RT/AF/ECC/CT) | `core/fm_rds_parser.c` (2140 lines) | none | — | **deferred** (see below) |
| 20 | RDS block counters, group counters, BLER | `mt6627_fm_rds.c:95-153` | none | — | **deferred** (belongs to the RDS decoder/log path) |
| 21 | Scan, CQI, soft-mute tune, search thresholds | `mt6627_fm_cmd.c:642-837`, `mt6627_fm_lib.c:1182-1269`, `1517-1590`, `1740-1747` | none | — | **deferred** (no V4L2 radio API; would be a private ioctl set) |
| 22 | FM TX | none for MT6627 (`ops->bi.tx_*` never set) | none | — | **deferred** (chip driver has no TX implementation) |
| 23 | Antenna switch (long/short) | `mt6627_fm_lib.c:303-318` | none | `V4L2_CID_MTK_WCN_FM_ANTENNA` → `0x61` bit4 | done |
| 24 | De-emphasis 50/75 µs | `mt6627_fm_cmd.c:213`, `mt6627_fm_lib.c:1740-1747` | none | `V4L2_CID_TUNE_DEEMPHASIS` → `0x61` bit12 | done (runtime write is [INFERENCE]) |
| 25 | Soft-mute gain / blending | `mt6627_fm_lib.c:1597-1718` | none | `V4L2_CID_MTK_WCN_FM_SOFTMUTE_GAIN`, `..._BLEND` → coeff memory | done |
| 26 | Channel side parameter (`0x65[11:0]`) | `mt6627_fm_lib.c:1903-1949`, `2106-2128` | none | built-in 861-entry table, compile-time size assert | done |
| 27 | Channel-parameter override file | `mt6627_fm_lib.c:2101-2128` (`/etc/fmr/mt6627_fm_channel_param.dat`) | none | — | **deferred** (optional board tuning file; built-in table used) |
| 28 | Customer config file defaults | `mt6627_fm_config.c:155-178` (`etc/fmr/mt6627_fm_cust.cfg`) | none | compiled-in defaults from `mt6627_fm_cust_cfg.h` | **deferred** (file override; defaults in use) |
| 29 | Antenna switch GPIO 140 | `core/fm_module.c:1382-1393` | none | `of_get_named_gpio("fmradio_ant_sel")`, high on power-up / low on power-down | done |
| 30 | `/dev/fm` char device + the FM ioctl set (`inc/fm_ioctl.h`) | `core/fm_module.c` | none | replaced by `/dev/radio0` V4L2 radio device | done (interface change, by design) |
| 31 | `/proc/fm` status file | `core/fm_module.c:1168-1208` | none | — | **deferred** (debug-only, no procfs in the port) |
| 32 | FM over BT (`FM_IOCTL_OVER_BT_ENABLE`) | `core/fm_main.c:1791-1815` | none | — | **deferred** (needs BT controller A2DP glue) |
| 33 | GPS RTC drift compensation | `core/fm_main.c:2088-2140` | none | — | **deferred** (needs GPS RTC owner) |
| 34 | FM audio out to the AP (I2S/ASoC) | §3.1 | none | chip-side audio enable only | **deferred** (no MT6735 AFE/codec driver in Nura) |
| 35 | rfkill switch | none downstream | none | `RFKILL_TYPE_FM` switch: block powers down, unblock restores | done (new; no downstream counterpart) |
| 36 | Reset recovery | `core/fm_main.c:2188-2265` | none | power-up + retune + `v4l2_ctrl_handler_setup()` | partial (hardware-unverified) |

### 3.1 Deferred: FM audio routing to the SoC (documented gap)

The chip-side audio path *is* programmed exactly as downstream: `0x9B = 0xF9AB` and host
`0x80101054 = 0x00003F35` on power-up/tune (`mt6627_fm_lib.c:850-851`, `1023-1024`), and the
audio path is shut down with `0x9B[2:0] = 0`, host `0x80101054 &= 0xFFFF9FFF` on power-down
(`mt6627_fm_cmd.c:522-523`, `mt6627_fm_lib.c:892-894`). What is missing on the Nura side is the
AP audio backend; the following is the exact interface that has to exist for FM audio:

* PCM platform driver name **`mt-soc-fm-i2s-pcm`** (`MT_SOC_FM_I2S_PCM`,
  `sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Def.h:267`), device-tree node **`mt_soc_fm_i2s_pcm`**
  with `compatible = "mediatek,mt_soc_pcm_fm_i2s"` (`mt6735.dtsi:1529-1531`, driver
  `sound/soc/mediatek/mt_soc_audio_v3/mt_soc_pcm_fm_i2s.c`, ROM symbols `mtk_pcm_fm_i2s_*`,
  `mtk_asoc_pcm_fm_i2s_new`, `mtk_afe_fm_i2s_probe`).
* Capture (AWB) path: PCM name **`mt-soc-fm-i2s-awb-pcm`** (`AudDrv_Def.h:268`), node
  **`mt_soc_fm_i2s_awb_pcm`**, `compatible = "mediatek,mt_soc_pcm_fm_i2s_awb"`
  (`mt6735.dtsi:1533-1535`).
* Codec DAI names used by that backend: **`mt-soc-fm-i2s-dai-codec`** and
  **`mt-soc-fm-i2s-dummy-dai-codec`** (`AudDrv_Def.h:306-307`).
* The downstream customer config declares the FM audio path as `FM_AUD_I2S`, I2S **master**, 32 kHz
  (`mt6627_fm_config.c:115-119`: `FM_AUD_I2S`, `FM_I2S_OFF`, `FM_I2S_MASTER`, `FM_I2S_32K`). Note that
  `ops->bi.i2s_set` is itself inside `#if 0` (`mt6627_fm_lib.c:1816-1827`), so in the shipped driver
  only the `0x9B` / host `0x80101054` writes actually program the chip.

Nura has no MT6735 AFE/`mt_soc_audio` backend, so this PCM/DAI and the mixer routing are a gap that
belongs to the audio-driver work, not to the FM driver. Nothing in this patch pretends otherwise: no
audio control is exposed and no audio state is reported. `mtk_wcn_fm_chip_power_up()` still programs
the chip-side registers so that audio starts working as soon as the AP backend exists.

---

## 4. Compile test

### 4.1 Local `.config`

The base `$M/nura.config` has `MEDIA_SUPPORT`, `VIDEO_DEV` and `RFKILL` disabled, so they were enabled
in the worktree `.config` only (never in `$M/nura.config`):

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/wcn-fm
cp $M/nura.config .config
./scripts/config -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
                 -e RFKILL -e MTK_BTIF -e MTK_WCN_WMT -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
```

Resulting symbols: `CONFIG_MEDIA_SUPPORT=y`, `CONFIG_MEDIA_RADIO_SUPPORT=y`, `CONFIG_VIDEO_DEV=y`,
`CONFIG_RFKILL=y`, `CONFIG_MTK_BTIF=y`, `CONFIG_MTK_WCN_WMT=y`, `CONFIG_MTK_WCN_FM=m`.
(`MEDIA_RADIO_SUPPORT=y` is required: with `MEDIA_SUPPORT_FILTER=y`, `VIDEO_DEV` lives in a menu that
is invisible unless one of the media device types is enabled, so a bare `-e VIDEO_DEV` is dropped by
`olddefconfig`.)

### 4.2 Object build (exact command and result)

```sh
cd $M/worktrees/wcn-fm
make ARCH=arm64 LLVM=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/misc/mediatek/connectivity/fm/
```

```
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_link.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_chip.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_v4l2.o
  LD [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o
```

* exit status 0, **no compiler warnings** (`-Wall` is on by default for the kernel).
* Each object was also built individually with
  `make … drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_{link,chip,v4l2}.o` — same result.
* `LD [M] mtk_wcn_fm.o` (the composite module object) links with no duplicate/undefined local symbols.

### 4.3 Full module link (`modpost`) — environment limitation, not a driver defect

```sh
make … drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.ko
```

fails in `MODPOST` with

```
WARNING: vmlinux.o is missing.
ERROR: modpost: "_raw_spin_lock" [drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.ko] undefined!
ERROR: modpost: "mtk_wcn_stp_send_data" [...] undefined!
...
```

because no `vmlinux` exists in the worktree (no full kernel build was run, per the task rules), so
*every* symbol — including core kernel ones — is unresolved. modpost must be re-run by the
integration owner after a full build; the frozen STP/WMT symbols used by this driver are:
`mtk_wcn_stp_send_data`, `mtk_wcn_stp_receive_data`, `mtk_wcn_stp_register_event_cb`,
`mtk_wcn_stp_is_ready`, `mtk_wcn_wmt_func_on`, `mtk_wcn_wmt_func_off`, `mtk_wcn_wmt_msgcb_reg`,
`mtk_wcn_wmt_msgcb_unreg`, `mtk_wcn_wmt_hwver_get`. All are declared in the frozen headers; no
frozen header was changed.

### 4.4 checkpatch

```sh
git diff -- drivers/misc/mediatek/connectivity/fm/ | ./scripts/checkpatch.pl --no-tree --no-signoff -
```

`total: 0 errors, 1 warnings, 3225 lines checked` — the only warning is
`added, moved or deleted file(s), does MAINTAINERS need updating?` (expected for new files).

---

## 5. Inferred / hardware-unverified behaviour

All register values and timings above are downstream values. The items below are **not** directly
stated by downstream source or ROM and are marked `[INFERENCE]` here and in the code comments.

1. **[INFERENCE] Seek result handling.** Downstream contains `mt6627_Seek()` but it is inside `#if 0`
   (`mt6627_fm_lib.c:1146-1179`) and `ops->bi.seek` is never assigned (`mt6627_fm_lib.c:1816-1827`), and `fm_module.c` has no
   `FM_IOCTL_SEEK` case. The seek packet builder (`mt6627_fm_cmd.c:696-746`) and the
   `FLAG_SEEK_DONE` result path (`core/fm_link.c:323-329`) are live code, so `VIDIOC_S_HW_FREQ_SEEK`
   was built from them: ramp-down → read `0x63` → mute → seek packet → result from the event payload
   → ramp-down → restore mute. **Unverified on hardware.**
2. **[INFERENCE] Seek wrap bit.** Downstream seek always sets `0x66[11]` (`mt6627_fm_cmd.c:728`) and
   the scan builder always clears it (`mt6627_fm_cmd.c:795`). The V4L2 `wrap_around` flag selects
   between those two downstream values.
3. **[INFERENCE] Seek spacing units.** V4L2 documents `v4l2_hw_freq_seek.spacing` in Hz
   (`Documentation/userspace-api/media/v4l/vidioc-s-hw-freq-seek.rst:90-93`); 50 k/100 k/200 kHz map
   to the downstream register encodings `1/2/4` (`mt6627_fm_cmd.c:691`), anything else is rejected.
   Spacing 0 uses the downstream default 100 kHz (`inc/fm_config.h:48-52`).
4. **[INFERENCE] De-emphasis at run time.** Downstream writes `0x61[12]` only during power-up
   (`mt6627_fm_cmd.c:213`) and stores the value through the search-threshold ioctl
   (`mt6627_fm_lib.c:1740-1747`). The V4L2 control issues the same modify operation at run time.
5. **[INFERENCE] `G_TUNER.signal` scaling.** The chip returns dBm
   (`mt6627_fm_lib.c:1277-1294`); V4L2 wants 0..65535, mapped as `(clamp(dbm,-128,127)+128)*257`
   (same convention as `radio-si476x.c`).
6. **[INFERENCE] ROM-ready poll bound.** Downstream loops `do { 1 ms; read 0x84 } while (tmp != 1);`
   forever (`mt6627_fm_lib.c:462-471`). The port bounds it with a 10 s deadline and returns
   `-ETIMEDOUT`; the bound is a safety guard, not a hardware constant.
7. **[INFERENCE] RDS record ABI.** The firmware event payload is copied into downstream
   `struct rds_rx_t` (`core/fm_link.c:394`, `inc/fm_rds.h:41-45`), i.e. 148 bytes
   (`sin, cos, 12 × {blkA..blkD, cbc, crc}`). `struct mtk_wcn_fm_rds_data` mirrors that layout and a
   compile-time assert checks `sizeof == 148`. Short payloads are zero-padded, oversized ones dropped.
   Downstream additionally *decodes* these groups into `rds_t` (`core/fm_rds_parser.c`, 2140 lines)
   and `read()` returns that decoded structure; here `read()` returns the raw groups. Consumers must
   decode PS/RT/AF themselves until the decoder is ported (deferred, §3 row 19).
8. **[INFERENCE] `rxsubchans`.** Reported as `MONO` always, `STEREO` when `0x6C` bit12 is set, and
   `RDS` while RDS reception is enabled. Downstream has no such aggregate; this is a V4L2-side
   convention built from the chip's mono/stereo indicator.
9. **[INFERENCE] Reset recovery.** The WMT callback is only invoked for whole-chip resets (that is the
   only `WMTDRV_WMT`-sourced reset message), which downstream treats as "FM already switched off by
   WMT" (`core/fm_main.c:2198-2208`), so the recovery path skips the explicit power-down and simply
   re-runs `func_on` + power-up + tune, then re-applies every control. If an ioctl is in flight when
   the reset happens, recovery waits for the command timeout. Unverified on hardware.
10. **[INFERENCE] rfkill semantics.** No downstream counterpart. Chosen behaviour: soft-block while
    the device is open powers the chip down and fails the current session with `-ERFKILL` on open;
    unblocking powers it up again and re-applies the controls.
11. **[INFERENCE] Band control.** Downstream takes the band per tune/seek call
    (`struct fm_tune_parm.band`, `inc/fm_main.h:77-93`); the V4L2 control stores it and, if the current
    frequency falls outside the new band, retunes to the band's lower edge. `FM_BAND_JAPAN` uses
    76.0–90.0 MHz per `inc/fm_config.h:24-28` (the file only defines `FM_JP_FREQ_MAX = 10800` for the
    wideband variant).
12. **[INFERENCE] GPIO 140 muxing.** Downstream only requests the GPIO and sets the direction; it does
    not mux the pin. The port does the same (`devm_gpio_request_one(..., GPIOF_OUT_INIT_LOW)`), so the
    pin must be left as GPIO by the bootloader/pinctrl default. `mt6735.dtsi` has `#gpio-cells = <2>`,
    which matches the downstream `<&pio 140 0>` specifier.
13. **Audio register writes.** `0x9B = 0xF9AB` / host `0x80101054 = 0x00003F35` are copied verbatim
    from downstream; their bit meaning is not documented in the source (only "enable connsys FM
    2 wire RX", `mt6627_fm_lib.c:849`).
14. **Zero-length final DSP segment.** Downstream computes `seg_num = len/512 + 1` and
    `seg_len = len % 512` for the last segment (`mt6627_fm_lib.c:559-570`), so an image whose size is
    a multiple of 512 produces a final zero-length segment. Kept for parity.
15. **Downstream quirk kept: reported ROM version.** Downstream increments the ROM version after the
    coefficient download (`mt6627_fm_lib.c:800`) and reports that value in `fm_hw_info`. Kept and
    documented; the raw value is what selects the firmware file.

Nothing else is inferred: every register, bit, delay, threshold, table entry, GPIO and firmware path
in the code is a downstream value with the citation listed in §2.3.

### 5.3 Firmware blobs userspace must supply (no blobs are added by this patch)

`request_firmware()` names (kernel firmware search path, normally `/lib/firmware`):

```
/lib/firmware/mt6627/mt6627_fm_v1_patch.bin      /lib/firmware/mt6627/mt6627_fm_v1_coeff.bin
/lib/firmware/mt6627/mt6627_fm_v2_patch.bin      /lib/firmware/mt6627/mt6627_fm_v2_coeff.bin
/lib/firmware/mt6627/mt6627_fm_v3_patch.bin      /lib/firmware/mt6627/mt6627_fm_v3_coeff.bin
/lib/firmware/mt6627/mt6627_fm_v4_patch.bin      /lib/firmware/mt6627/mt6627_fm_v4_coeff.bin
/lib/firmware/mt6627/mt6627_fm_v5_patch.bin      /lib/firmware/mt6627/mt6627_fm_v5_coeff.bin
```

These are exactly the downstream paths `/etc/firmware/mt6627/mt6627_fm_vN_{patch,coeff}.bin`
(`mt6627_fm_lib.c:46-57`, ROM strings §2.2). Two ways to satisfy them:

* install the Android files as `/lib/firmware/mt6627/…`, or
* build with `CONFIG_EXTRA_FIRMWARE_DIR="/etc/firmware"`, which makes the downstream path work
  unchanged (like the ROM did).

The file is chosen from the DSP ROM version read out of the chip: `vN` where `N = 0x83[15:8]`. If no
`vN` patch exists, power-up fails (downstream behaviour, `mt6627_fm_lib.c:775-784`); if no `vN`
coefficient exists, the newest file is used and coefficient bytes 4/5 are zeroed to disable the
patch (`mt6627_fm_lib.c:797-809`).

---

## 6. DTS-FRAGMENT

The canonical fragment from `PARITY_CONTEXT.md`, unchanged, plus the FM node this driver binds to
(adapted from downstream `mt6737t_grandpplte_ltn_dtv_03.dts:1299-1304`, GPIO number kept):

```dts
#include <dt-bindings/clock/mediatek,mt6735-infracfg.h>
#include <dt-bindings/clock/mediatek,mt6735-pericfg.h>
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/power/mediatek,mt6735-power-controller.h>

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

	/*
	 * MT6627 FM radio.  Only the antenna switch GPIO is board specific;
	 * the radio itself is a function of the CONSYS node above and is
	 * driven over STP, so this node has no reg/interrupt of its own.
	 */
	mt_fmradio_mt6627: mt_fmradio_mt6627 {
		compatible = "mediatek,mt_mt6627_fmradio";
		fmradio_ant_sel = <&pio 140 GPIO_ACTIVE_HIGH>;
		status = "okay";
	};
};
```

Notes for the integration owner:

* The node name/compatible come from the ROM (`mediatek,mt_mt6627_fmradio`, node
  `mt_fmradio_mt6627` in `boot.img`) and from downstream (`mt6737t_grandpplte_ltn_dtv_03.dts:1299-1304`).
* `fmradio_ant_sel = <&pio 140 …>` keeps the downstream property name and pin number; the second cell
  is the standard GPIO flag cell (`#gpio-cells = <2>` in `mt6735.dtsi`). If the board does not route
  this switch, the property may be omitted — the driver then skips the GPIO and only logs at debug
  level (`core/fm_module.c:1388-1393` behaviour).
* No additional clocks, regulators or interrupts are needed: FM power and clocks are owned by the
  CONSYS/WMT node, and FM has no dedicated interrupt line in the downstream DT.

---

## 7. Patch

```
$M/patches/wcn-fm.patch
```

`git apply --stat`:

```
 drivers/misc/mediatek/connectivity/fm/Makefile     |    6
 drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.h |  408 +++++
 .../mediatek/connectivity/fm/mtk_wcn_fm_chip.c     | 1644 ++++++++++++++++++++
 .../mediatek/connectivity/fm/mtk_wcn_fm_link.c     |  371 +++++
 .../mediatek/connectivity/fm/mtk_wcn_fm_v4l2.c     |  793 ++++++++++
 5 files changed, 3222 insertions(+)
```

Apply order: needs `MTK_WCN_WMT` (STP/WMT symbols) and `MEDIA_SUPPORT`/`VIDEO_DEV`/`RFKILL` from the
integration `.config`. Probe order: the WMT driver must have registered the STP transport before the
FM device is opened; the FM driver itself probes without touching the hardware.
