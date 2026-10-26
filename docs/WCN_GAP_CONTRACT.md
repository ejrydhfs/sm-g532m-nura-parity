# WCN gap-closure contract (coordinator-owned, read-only for leaves)

Subsystem: MediaTek WCN / CONSYS (WMT core, WLAN, Bluetooth, GPS, FM) for
SM-G532M / MT6737T, Nura `v7.0.10-mt6735` arm64.
Read first: `$M/PARITY_CONTEXT.md`, `$M/logs/wcn-coordinator.md`,
`$M/logs/wcn-gap-gps.md`, `$M/logs/wcn-gap-fm-rds.md`.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Worktree state (already prepared — do not recreate, do not reset)

| Leaf | Worktree | HEAD baseline | Patch | Report |
|---|---|---|---|---|
| WLAN MAC/mgmt | `$M/worktrees/wcn-gap-wlan` | `baseline: wcn-subsystem.patch` | `$M/patches/wcn-gap-wlan.patch` | `$M/logs/wcn-gap-wlan.md` |
| WMT PSM/coredump/coex | `$M/worktrees/wcn-gap-wmt` | `baseline: wcn-subsystem.patch` | `$M/patches/wcn-gap-wmt.patch` | `$M/logs/wcn-gap-wmt.md` |
| FM I2S (driver side) | `$M/worktrees/wcn-gap-fm-i2s` | `baseline: wcn-subsystem.patch` | `$M/patches/wcn-gap-fm-i2s.patch` | `$M/logs/wcn-gap-fm-i2s.md` |
| FM I2S (machine/AFE side) | `$M/worktrees/wcn-gap-fm-i2s-audio` | `baseline: audio-integration.patch + wcn-subsystem.patch` | `$M/patches/wcn-gap-fm-i2s-audio.patch` | (same report) |
| GPS | `$M/worktrees/wcn-gap-gps` | `baseline: wcn-subsystem.patch` | `$M/patches/wcn-gap-gps.patch` | `$M/logs/wcn-gap-gps.md` (done) |
| FM RDS | `$M/worktrees/wcn-gap-fm-rds` | `baseline: wcn-subsystem.patch` | `$M/patches/wcn-gap-fm-rds.patch`, `wcn-gap-fm-rds-kernel.patch` | `$M/logs/wcn-gap-fm-rds.md` (done) |

Each of the three open worktrees already contains partial work from the aborted
first run. **Continue that work; do not restart from scratch.** Do not commit
in `wcn-gap-wlan` / `wcn-gap-wmt` / `wcn-gap-fm-i2s`. The only exception is
`wcn-gap-fm-i2s-audio`, where the driver-side patch is committed by the leaf so
that its own diff contains only the machine/AFE changes (see §5).

Never edit `$M/worktrees/integration`, `$M/worktrees/wcn-gap-integration`,
`$M/worktrees/audio-gap-integration`, any non-gap `wcn-*`/`audio-*` worktree,
another leaf's worktree, `$M/nura.config`, or any shared `.dts`/`.dtsi`.

## 2. Deliverables

Patch: `git add -N <new files> && git diff HEAD --binary > $M/patches/<leaf>.patch`.
The diff must contain only intended source changes: no `.o`, `.ko`, `.cmd`,
`modules.order`, `Module.symvers`, `.mod.c`, generated config, or baseline files.
No binary hunks, no firmware blobs.

Report: `$M/logs/<leaf>.md`, sections exactly as in `PARITY_CONTEXT.md`
(assigned driver + files, ROM/downstream evidence with file:line, parity matrix,
compile command + result, inferred/hardware-unverified list, DTS fragment,
patch path). The FM I2S report additionally carries the `AUDIO-FRAGMENT`
section (§5).

## 3. Rules

- No stubs, no fake success, no `TODO: implement`, no invented register values.
  Every register/value must come from `$M/src/downstream-grandpplte` (cite
  `file:line`) or the ROM ELF/strings (cite symbol + address); anything inferred
  is marked `[INFERENCE]`.
- A gap that needs data absent from every source: implement what is possible,
  then report the exact missing input and stop that part. Never fabricate it.
- No shared DTS edits. DTS fragments go in the report only.
- Reject unsupported userspace requests with a real errno instead of accepting
  and ignoring them.
- Keep the existing coding style of the WCN driver; `W=1` clean is required for
  touched objects.

## 4. Compile recipe

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/<worktree>
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e RFKILL -e CRC32 -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j4 \
  <every .o of the directory> drivers/misc/.../<module>.o
```

Also compile the module (`LD [M]`) and, for userspace tools, build them with the
host compiler and run their self-test. A full kernel build / tree-wide modpost is
not expected; unresolved kernel symbols at a standalone module link are an
environment limitation and must be reported as such, not "fixed" with stubs.

## 5. Slice acceptance criteria

### WLAN MAC/management (`wcn-gap-wlan`)
Existing partial files: `wlan_mac.c/.h`, `wlan_mgmt.c`, `wlan_netdev.c`,
`wlan_txrx.c`, plus changes to `wlan_ahb.c`, `wlan_main.c`, `wlan_hif.h`,
`Kconfig`, `Makefile`. **Finish and validate this slice; it must be complete and
compileable, not a skeleton.**

Required end-to-end behaviour, all backed by downstream code:
- cfg80211 wiphy registered by `wlan_main.c` / `wlan_mac.c`, one netdev, one
  wireless_dev, `NL80211_IFTYPE_STATION`; wiphy bands/channels/rates limited to
  what the firmware path really supports (2.4 GHz, no 5 GHz unless the HIF/firmware
  evidence supports it).
- `scan`: software scan built from real 802.11 probe-request frames transmitted
  through the HIF TX path, probe responses/beacons parsed from HIF RX, results
  reported with `cfg80211_inform_bss_frame_data()`; abort support; scan state
  machine that cannot double-report or leak `scan_req`.
- `connect` / `disconnect`: open (no-encryption) infrastructure association via
  auth/assoc request frames, assoc response parsing, `cfg80211_connect_result()` /
  `cfg80211_disconnected()`, timeout/abort paths, deauth from the AP.
  If cfg80211 supplies WPA/RSN IEs, fail the connect with a real error (no key
  offload exists in this port); do not silently connect unencrypted.
- TX: Ethernet → 802.11 data frame conversion (802.3 → LLC/SNAP + header),
  per-AC queues/credits that match the HIF API actually implemented; RX: 802.11 →
  802.3 conversion, `eth_type_trans()`, `netif_rx()`, per-frame length/bounds
  checks; `ndo_start_xmit`, `ndo_open/stop`, multicast filter, carrier state.
- `get_station`: real RCPI/rate from the HIF RX header where available; no
  invented values.
- MAC address: if the firmware/HIF provides one, use it; otherwise
  `eth_hw_addr_random()` and say so in the report (never a hard-coded Samsung OUI).
- Lifecycle: `fw_down`/reset must disconnect + stop the netdev cleanly; module
  unload must unregister wiphy/netdev with no use-after-free; PM suspend/resume
  consistent with the HIF.
- Kconfig help text must describe exactly the supported feature set (no
  advertising of AP/IBSS/P2P/hardware scan/WPA).

### WMT PSM / coredump / coexistence (`wcn-gap-wmt`)
Existing partial files: `stp_psm.c/.h`, `stp_dbg.c`, `stp_btm.c`, `wmt_coex.c`
plus changes to `stp_core.c`, `wmt_core.c`, `wmt_api.c`, `wmt_local.h`, the two
frozen headers, and `Makefile`.
- Verify every gap source is listed in `mtk_wcn_wmt-y` in
  `drivers/misc/mediatek/connectivity/wmt/Makefile` (currently yes) and that
  nothing gap-related is dead code: PSM must be entered/left from the real STP
  TX/RX paths, coredump must be reachable from the real assert/reset path,
  coexistence from the real WMT command paths.
- PSM: downstream `stp_psm.c` semantics — active/sleep state machine, wakeup
  handshake, AP-sleep interactions, `stp_psm_tx_begin/end` blocking and
  `stp_psm_rx_done`, timeout handling; no busy-wait in atomic context.
- Coredump: firmware coredump/assert collection and the `STP_DBG` generic-netlink
  ABI (family `STP_DBG`, BIND/RESET, `[M]` chunk framing) with the EMI/register
  dump pushed to the bound daemon; bounded buffers, no user-triggerable overrun.
- Coex: BGW desense + LTE IDC protocol per downstream `wmt_coex.c`/`wmt_idc.c`/
  `bgw_desense.c`; the modem bridge API must degrade to `-ENODEV` when no bridge
  is registered, never fabricate modem data.
- The GPS leaf’s `wcn-gap-gps.patch` depends on the additive exports
  `mtk_wcn_wmt_ic_info_get()` / `mtk_wcn_wmt_co_clock_flag_get()`; keep those
  signatures exactly as declared in the current header.
- Compile: every object in the directory plus `LD [M] mtk_wcn_wmt.o`, `W=1`
  clean; also compile with `CONFIG_MTK_WCN_WMT=y`.

### FM I2S routing (`wcn-gap-fm-i2s`, two worktrees)
**Binding requirement:** the FM driver must not register a sound card. It
registers a codec component that the **existing** `mt-snd-card` machine driver
(`sound/soc/mediatek/mt6735/mt6735-mt6331.c`, from
`$M/patches/audio-integration.patch`) binds in an FM DAI link, and the CPU side
of that link is an AFE DAI owned by the existing MT6735 AFE
(`sound/soc/mediatek/mt6735/`). A second `snd_soc_register_card()` is a failure.

Worktree A (`wcn-gap-fm-i2s`, WCN baseline) — driver side only:
- `mtk_wcn_fm_i2s.c/.h`: codec component `mt6627-fm-i2s` (DAI name and stream
  names per downstream `AudDrv_Def.h`), fixed 32 kHz/16-bit/stereo I2S,
  `set_fmt`/`hw_params`/`startup`/`shutdown`/`trigger`/`mute_stream`, no DAPM
  widgets for hardware that does not exist.
- `mtk_wcn_fm_chip.c`: `mtk_wcn_fm_chip_i2s_set()/get()/info()` with the exact
  downstream register writes (`FM_REG_AUDIO_CTRL0 = 0xf9ab`, host
  `0x80101054 = 0x00003f35`, disable masks) and the power-up/power-down/retune
  interaction.
- `mtk_wcn_fm_v4l2.c`: `FM_IOCTL_I2S_SETTING` (0xf5, 33) and
  `FM_IOCTL_GET_I2S_INFO` (0xf5, 41) with downstream 12-byte payload layout and
  errno conventions, `compat_ioctl` where the uapi needs it.
- Kconfig/Makefile: build with `CONFIG_SND_SOC=n` (inline fallbacks) **and** with
  `CONFIG_SND_SOC=y/m`; all combinations must compile.

Worktree B (`wcn-gap-fm-i2s-audio`, audio+WCN baseline) — machine/AFE side:
- Add the AFE-side FM I2S DAI in `sound/soc/mediatek/mt6735/` implementing the
  documented 2nd-I2S-in path from downstream `Set2ndI2SIn()`
  (`mt_soc_afe_control.c:2214-2245`: `AFE_I2S_CON` with pad select from CONNSYS,
  slave, I2S framing, 16-bit, LRCK not inverted, phase-shift fix) and the
  documented FM I2S prepare sequence of `mtk_pcm_fm_i2s_prepare()`
  (`mt_soc_pcm_fm_i2s.c:279-323`) — including the AFE connections, digital gain
  and I2S-out enable it performs, only with values that exist in the downstream
  source. If a needed register/value is not documented anywhere, stop and report
  it; do not guess.
- Register that DAI on the existing AFE component and add one DAI link to the
  existing `mt-snd-card` link table referencing CPU DAI = the new AFE DAI and
  codec = `mt6627-fm-i2s`. Keep every existing link intact; the card name and
  platform driver name stay `mt-snd-card` / `mediatek,mt6735-sound`.
- Compile the AFE, machine card and the FM module in this tree, and prove the
  name binding: the DAI link's `codec_dai_name` equals the DAI registered by the
  FM component, and `dai_name` equals the AFE DAI, both greppable from the
  sources. State the ASoC binding rule that makes this a single card.
- Report ordering: `audio-integration.patch` (+ audio gap patches) →
  `wcn-gap-fm-i2s-audio.patch`; the audio-side patch is *not* part of the pure
  WCN series and must be listed under `AUDIO-FRAGMENT`/ordering in the report.

## 6. Coordinator integration order (informational)

`wcn-subsystem.patch` → `wcn-gap-wmt.patch` → `wcn-gap-wlan.patch` →
`wcn-gap-gps.patch` → `wcn-gap-fm-rds-kernel.patch` → `wcn-gap-fm-i2s.patch`
→ (userspace: `wcn-gap-fm-rds.patch`) → (audio tree:
`wcn-gap-fm-i2s-audio.patch`). The FM header (`mtk_wcn_fm.h`) is touched by both
fm-rds and fm-i2s; leaves must not reorder or "fix" the peer's hunks.
