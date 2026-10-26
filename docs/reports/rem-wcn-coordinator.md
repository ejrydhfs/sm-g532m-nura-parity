# rem-wcn — WCN remaining-gap coordinator report

Subsystem: MediaTek WCN / CONSYS (WMT+STP, WLAN gen2, FM) for SM-G532M /
grandpplte, MT6737T, Nura `v7.0.10-mt6735` arm64.
Baseline: `$M/patches/parity-baseline-all.patch` on `$M/src/nura-linux` HEAD
`046883c0c` (chain HEAD in the integration tree is `fdb362e6b`, i.e. baseline
plus the two hook fixups described in §2).
Hardware: none attached. Everything below is source-level parity plus real
compile/link and userspace self-test runs.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Method

Four leaves (`deepseek-worker`), one fresh worktree each, baseline applied and
committed first. The aborted earlier run's partial implementations were
snapshotted and re-applied as working-tree state in the three WCN-side
worktrees, so no earlier progress was lost, and every leaf then finished,
validated and documented its slice on the current baseline:

| Leaf | Worktree | Patch | Report |
|---|---|---|---|
| WLAN MAC/management (`RemWcn.WlanMac`) | `$M/worktrees/rem-wcn-wlan` | `rem-wcn-wlan.patch` | `$M/logs/rem-wcn-wlan.md` |
| WMT PSM/coredump/coex (`RemWcn.WmtPsm`) | `$M/worktrees/rem-wcn-wmt` | `rem-wcn-wmt.patch` | `$M/logs/rem-wcn-wmt.md` |
| FM I2S, WCN side (`RemWcn.FmI2s`) | `$M/worktrees/rem-wcn-fm-i2s` | `rem-wcn-fm-i2s.patch` | `$M/logs/rem-wcn-fm-i2s.md` |
| FM I2S, AFE/machine side (`RemWcn.FmI2sAudio`) | `$M/worktrees/rem-wcn-fm-i2s-audio` | `rem-wcn-fm-i2s-audio.patch` | `$M/logs/rem-wcn-fm-i2s-audio.md` |
| integration (coordinator) | `$M/worktrees/rem-wcn-integration` | `rem-wcn-integration.patch` | this file |

Shared run context (frozen naming, rules, evidence index, build recipe):
`$M/REM_WCN_CONTEXT.md`. Per-slice acceptance criteria:
`$M/WCN_GAP_CONTRACT.md` §5.

No leaf committed. `$M/worktrees/integration` and every `wcn-gap-*` /
`audio-*` worktree were left untouched.

## 2. Baseline defect found and fixed (affects the whole program)

`parity-baseline-all.patch` lost two merge hooks that every subsystem patch
adds; without them the tree configures and builds nothing under
`drivers/misc/mediatek/`:

1. `drivers/misc/Kconfig` no longer sourced `drivers/misc/mediatek/Kconfig`
   (only the accdet source survived). Symptom: `make olddefconfig` silently
   drops every `CONFIG_MTK_*` symbol (WMT/WLAN/BT/GPS/FM, ccci, vcodec), and
   module targets are "No rule to make target".
2. `drivers/misc/Makefile` no longer had `obj-y += mediatek/` (only
   `obj-$(CONFIG_MTK_ACCDET) += mediatek/accdet/`). Symptom: the whole
   `drivers/misc/mediatek` subtree is never descended into.

Fix delivered as `$M/patches/rem-baseline-hooks.patch` (2 files, +2 lines,
identical to the hunks in `wcn-base.patch:9,19` etc.), committed as two
`baseline fixup:` commits at the bottom of every `rem-wcn-*` worktree and
broadcast to the other area coordinators. Recommendation: fold these two lines
into the shared baseline patch before the next run.

A second, non-baseline detail worth recording: a bare `<file>.o` make target
does not inherit the directory Makefile's `ccflags-y`, so
`make drivers/misc/.../stp_psm.o` fails on `mtk_wcn_consys.h`. Build the
composite module object (`…/connectivity/wmt/mtk_wcn_wmt.o`, etc.).

## 3. What each slice delivers

Stats are `git apply --stat` of the delivered patch; all patches are
conflict-free with each other (disjoint file sets, verified), contain 0 binary
hunks and none of the two hook files.

| Leaf | Files | +/- | Highlights |
|---|---|---|---|
| WMT | 20 | +4674/−30 | PSM state machine (ACT/ACT_INACT/INACT/INACT_ACT) with WMT sleep/wakeup/host-awake handshake, hold FIFO, idle monitor, quick-PS, TX/RX-density disable, forced wakeup, reset+reapply; firmware coredump/assert path with `[M]`-chunked EMI paged dump to the `STP_DBG` netlink daemon; BGW desense + LTE IDC protocol with `-ENODEV` modem-bridge degradation; userspace `tools/mediatek/wcn/stp_dbg` (real client + streaming parser + 10-case self-test) |
| WLAN | 12 | +3822/−26 | cfg80211 STA wiphy/netdev (2.4 GHz only, no key offload/AP/P2P/hw-scan); software scan with real probe requests over the HIF TX path; open auth/assoc connect with firmware `CMD_BSS_ACTIVATE_CTRL`/`UPDATE_STA_RECORD`/`SET_BSS_INFO` sequencing and sequence-matched `EVENT_ID_CMD_RESULT` completion; 802.3↔802.11 TX/RX conversion with per-AC credits; `get_station` from real RCPI; documented lifecycle/teardown |
| FM (WCN) | 10 | +1484/−6 | `mt6627-fm-i2s` codec component (no card), DAI `mt-soc-fm-i2s-dai-codec`, fixed 32 kHz/16-bit/stereo, chip I2S enable/disable (`0x9B=0xF9AB`, host `0x80101054=0x00003f35`, disable masks), `FM_IOCTL_I2S_SETTING`/`GET_I2S_INFO` 12-byte payload + compat, userspace `tools/mediatek/wcn/fm_i2s` (37-check self-test) |
| FM (audio) | 7 | +564/−13 | memif-less AFE DAI `mt-soc-fmi2S-driver` with the documented 2nd-I2S-in preparation (four AFE routes, digital gain 1, I2S_OUT_DAC, ASRC bypass bit 30, `AFE_I2S_CON` 0x0018 pad-from-CONNSYS/slave/I2S/16-bit/LRCK-normal/phase-shift-fix); one `FM_I2S_RX_Playback` link on the existing `mt-snd-card`, guarded by `IS_ENABLED(CONFIG_MTK_WCN_FM)`; `AFE_I2S_CON` 0x0018 FM↔I2S0 mutual exclusion with `-EBUSY` |

## 4. Integration and verification (run by me, in `rem-wcn-integration`)

Configuration (both hook fixups in HEAD, then):

```sh
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e RFKILL -e CRC32 -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM \
  -e SOUND -e SND -m SND_SOC_MT6735 -m SND_SOC_MT6735_MT6331 -m SND_SOC_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
```

(`nura.config` has no `CONFIG_MTK_*` and no sound symbols; `SOUND`/`SND` gate
all of `sound/soc`.)

Applied in this order: `rem-wcn-wmt` → `rem-wcn-fm-i2s` →
`rem-wcn-fm-i2s-audio` → `rem-wcn-wlan`. All applied with `git apply` cleanly,
no conflicts (the series is file-disjoint).

Clean rebuild of every touched directory (objects deleted first, `W=1`, `-j2`):

```sh
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  drivers/misc/mediatek/btif/mtk_btif.o \
  drivers/misc/mediatek/connectivity/{wmt/mtk_wcn_wmt,wlan/mtk_wcn_wlan,bt/mtk_wcn_bt,gps/mtk_wcn_gps,fm/mtk_wcn_fm}.o \
  sound/soc/mediatek/mt6735/snd-soc-mt6735-afe.o sound/soc/mediatek/mt6735/snd-soc-mt6735-mt6331.o
```

Result: **43 × `CC [M]`, 7 × `LD [M]`** (`mtk_btif`, `mtk_wcn_wmt`,
`mtk_wcn_wlan`, `mtk_wcn_fm`, `mtk_wcn_gps`, `snd-soc-mt6735-afe`,
`snd-soc-mt6735-mt6331`; `mtk_wcn_bt` is a single-object module, no LD step),
**0 warnings, 0 errors**. Log: `$M/logs/rem-wcn-integration_final.log`.

The `-Wimplicit-function-declaration` errors in `gps/stp_chrdev_gps.c`
(undeclared `mtk_wcn_wmt_ic_info_get()` /
`mtk_wcn_wmt_co_clock_flag_get()` / `WMT_ICINFO_*`) that the pristine baseline
shows are gone once `rem-wcn-wmt` is applied — that patch is what provides
them, as the GPS leaf's report requires.

Extra integration checks:

- **FM link guard** (`mt6331-mt6735` machine card): with `MTK_WCN_FM=m` the
  object contains `FM_I2S_RX_Playback`, `mt-soc-fmi2S-driver`,
  `mt6627-fm-i2s`; with `MTK_WCN_FM` unset the object contains none of them and
  keeps `MultiMedia1`/I2S0/voice strings. Both configurations compile `W=1`
  clean (`$M/logs/rem-wcn-integration_fm_n.log`). Without the guard the card
  would defer forever in an FM-less build.
- **Userspace self-tests** in the integrated tree: `fm_i2s --selftest` PASS
  (37 checks), `stp_dbg --selftest` all 10 checks, `fm_rds_decode --selftest`
  (pre-existing) all checks. Artifacts cleaned afterwards.
- Tree hygiene: 49 changed/new files = exactly the four patches' file sets;
  no `.o`/`.ko`/`.cmd`/generated files; `rem-wcn-integration.patch` has 0 binary
  hunks.

## 5. Independent cross-checks I performed (beyond the leaf reports)

- **Connection routes** (audio leaf): bits re-derived from downstream
  `mt_soc_afe_connection.c` — [I00][O13]=AFE_GAIN1_CONN2 bit 2,
  [I01][O14]=bit 16, [I10][O03]=AFE_GAIN1_CONN bit 8, [I11][O04]=bit 10, and
  exactly those four are programmed. I rejected an earlier revision that added
  invented I00→O14 / I01→O13 cross-connections; the leaf removed them.
- **ASRC polarity**: bypass = `AFE_CONN4` bit 30 set, per
  `mt_soc_afe_control.c:2273-2274`; matches the delivered code.
- **FM I2S ROM semantics**: `fm_i2s_set` at `0xc063a59c` is a `low_ops`
  dispatcher (NULL hook → `0xfffffc16`), `fm_get_i2s_info` at `0xc0638e90`;
  this matches the leaf's note that the stock ROM setter is dead code and the
  port implements the documented ABI instead.
- **PSM/coex/coredump ROM symbols**: `stp_psm_set_state`, `stp_psm_do_wakeup`,
  `stp_psm_notify_wmt_sleep/wakeup`, `stp_psm_hold_data/release_data`,
  `stp_dbg_nl_bind/reset/send`, `wmt_core_set_coredump_state`,
  `stp_btm_*`, `wmt_ctrl_bgw_desense_ctrl`, `wmt_lib_idc_lock_aquire/release`,
  `wmt_dev_bgw_desense_init/deinit` are all present in `$M/zip/symbols.txt`,
  and ROM strings `PSM_SLEEP`, `PSM_HOST_AWAKE`, `PSM_MONITOR`, `PSM_EXIT`,
  `STP_DBG`, `BGW_DESENSE_EVT` confirm the implemented state/ABI names.
- **STP_DBG netlink numbering**: family `STP_DBG`, `ATTR_MSG=1`, `BIND=1`,
  `RESET=2`, `MAX_BIND_PROCESS=4` — identical in
  `conn_soc/linux/pri/stp_dbg.c:84-100` and the ported `stp_dbg.c`.
- **Feature reachability** (WMT): `stp_psm_init/deinit` and
  `stp_btm_init/deinit` are called from `stp_core.c:1257-1282`;
  `stp_psm_tx_begin/tx_end/rx_done` from the real STP TX/RX paths
  (`stp_core.c:1023,1031,1125`); coredump from the FW-assert path
  (`stp_core.c:791-803`); `wmt_coex_init()` from `wmt_lib.c:170`.
  Nothing gap-related is dead code.

## 6. Closed vs deferred

Closed (implemented, compile/link verified, evidence-cited):
WLAN cfg80211 station + software scan + open connect/disconnect with real
firmware BSS/STA-record programming and 802.3 data path; WMT PSM, coredump
netlink, coex/IDC; FM I2S chip+codec+ioctl; FM I2S AFE DAI + single machine
link with I2S0 exclusivity; userspace helpers `stp_dbg` and `fm_i2s`.

Deferred with the exact missing input (documented in the leaf reports, not
stubbed, no fabricated values):

1. **WLAN per-channel RF retune for scan** — needs the firmware scan command
   (`CMD_ID_SCAN_REQ`, downstream `mgmt/scan_fsm.c:377-455`) plus the scan
   FSM/command result flow. Probe requests are still emitted per requested
   channel, but the RF stays where the firmware put it.
2. **WLAN multicast/promiscuous filter** — no filter register in this HIF; the
   packet-filter OID requires the firmware command path for OIDs (only CMD_IDs
   are wired). `ndo_set_rx_mode` is a documented no-op.
3. **WLAN rate report in `get_station`** — the HIF RX header has no rate field;
   downstream obtains rates through `wlanoidQueryStaStatistics` (firmware
   statistics OID), which is not ported.
4. **WLAN WoWLAN/MAC suspend** — suspend masks the HIF interrupt only; no
   WoWLAN firmware interface is implemented anywhere in this port.
5. **FM ASRC 32k→44.1/48k** — deliberately not engaged (fixed-32 kHz path);
   enabling it needs the documented `AFE_ASRC_CON13/14/15/16/17/20` writes
   (`AudDrv_Afe.h:675-682`), which the audio report lists.
6. **FM vs voice arbitration** — the AFE refuses FM while a voice call is
   active, but a voice call can still start during FM and reprogram the shared
   PMIC DAC (AFE_I2S_CON1). Userspace must stop FM first; documented.

No leaf was blocked by missing proprietary data: every register, PDU field and
ABI element used comes from `$M/src/downstream-grandpplte` or the ROM ELF, and
each report carries `file:line` / symbol citations. Remaining unknowns are
hardware-only (see each report's "inferred/unverified" section).

## 7. Deliverables and ordering

```
$M/patches/rem-baseline-hooks.patch      (2 lines, fold into the shared baseline)
$M/patches/rem-wcn-wmt.patch
$M/patches/rem-wcn-wlan.patch
$M/patches/rem-wcn-fm-i2s.patch
$M/patches/rem-wcn-fm-i2s-audio.patch    (audio-series fragment, applies after audio-integration.patch)
$M/patches/rem-wcn-integration.patch     (all four combined; 49 files, +10544/−75)
$M/logs/rem-wcn-{wmt,wlan,fm-i2s,fm-i2s-audio}.md
$M/logs/rem-wcn-coordinator.md           (this file)
```

Ordering: `parity-baseline-all.patch` (+ `rem-baseline-hooks.patch`) →
`rem-wcn-wmt.patch` → `rem-wcn-wlan.patch` → `rem-wcn-fm-i2s.patch` →
`rem-wcn-fm-i2s-audio.patch`. The last is the audio-side fragment: it touches
only `sound/soc/mediatek/mt6735/` and must be listed separately from the pure
WCN series. The FM WCN patch and the audio patch are mutually dependent at
runtime (codec component ↔ machine link) but independent as patches.

## 8. DTS

None. No shared `.dts`/`.dtsi` was modified by any leaf; the FM link needs no
new node because the codec component is registered by the FM driver and the CPU
DAI lives on the existing MT6735 AFE platform device. The missing audio pinctrl
state for the I2S pads is a hardware/board item noted by the audio leaf, not a
driver gap in this run.
