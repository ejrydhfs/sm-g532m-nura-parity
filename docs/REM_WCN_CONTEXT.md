# REM-WCN run context (coordinator-owned, read-only for leaves)

Remaining-gap closure for the MediaTek WCN / CONSYS subsystem of the SM-G532M
(grandpplte, MT6737T) Nura `v7.0.10-mt6735` arm64 kernel.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

Read first, in this order:

1. `$M/PARITY_CONTEXT.md` — sources of truth, editing rules, required report format.
2. `$M/WCN_GAP_CONTRACT.md` §3 (rules), §4 (compile recipe), §5 (slice acceptance
   criteria, per leaf). Everything in §5 applies to this run except the worktree
   names/paths and the FM DAI naming, which this file supersedes.
3. `$M/logs/wcn-coordinator.md` — what the WCN base port already implements.
4. `$M/logs/wcn-gap-gps.md`, `$M/logs/wcn-gap-fm-rds.md` — the two gap leaves
   already closed; do not regress their interfaces.

## 1. Worktree, baseline, deliverables

Every worktree was created from `$M/src/nura-linux` HEAD `046883c0c` with
`$M/patches/parity-baseline-all.patch` applied and committed as
`baseline: parity-baseline-all.patch`. That baseline already contains the WCN
subsystem, the camera/audio/ccci/vcodec subsystems and the applied gap patches
(GPS, FM RDS). **Do not recreate, reset, rebase or re-apply the baseline.**

Two baseline fixup commits sit on top of it in every worktree (they are in HEAD,
so they never appear in a leaf patch — do not touch those two files):

- `baseline fixup: source drivers/misc/mediatek/Kconfig` — the distributed
  baseline patch had lost `source "drivers/misc/mediatek/Kconfig"`, so
  `make olddefconfig` silently dropped every `CONFIG_MTK_*` symbol under
  `drivers/misc/mediatek/`.
- `baseline fixup: descend into drivers/misc/mediatek` — it had also lost
  `obj-y += mediatek/` in `drivers/misc/Makefile`, so the whole subtree was
  unreachable and module targets reported "No rule to make target".

Consequence for the build: build the **composite module object**, never a bare
`<file>.o` target (a singled-out `.o` does not inherit the directory Makefile's
`ccflags-y` include paths and fails on `mtk_wcn_consys.h`):

```sh
make ... drivers/misc/mediatek/connectivity/wmt/mtk_wcn_wmt.o
make ... drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.o
make ... drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o
make ... sound/soc/mediatek/mt6735/snd-soc-mt6735-afe.o
make ... sound/soc/mediatek/mt6735/snd-soc-mt6735-mt6331.o
```

| Leaf | Worktree | Patch | Report |
|---|---|---|---|
| WLAN MAC/management | `$M/worktrees/rem-wcn-wlan` | `$M/patches/rem-wcn-wlan.patch` | `$M/logs/rem-wcn-wlan.md` |
| WMT PSM/coredump/coex | `$M/worktrees/rem-wcn-wmt` | `$M/patches/rem-wcn-wmt.patch` | `$M/logs/rem-wcn-wmt.md` |
| FM I2S, WCN side | `$M/worktrees/rem-wcn-fm-i2s` | `$M/patches/rem-wcn-fm-i2s.patch` | `$M/logs/rem-wcn-fm-i2s.md` |
| FM I2S, AFE/machine side | `$M/worktrees/rem-wcn-fm-i2s-audio` | `$M/patches/rem-wcn-fm-i2s-audio.patch` | `$M/logs/rem-wcn-fm-i2s-audio.md` |

The three WCN-side worktrees (`wlan`, `wmt`, `fm-i2s`) already contain the
aborted first run's partial implementation, staged in the index. **Continue and
finish that code; do not restart from scratch.** The audio worktree is clean
baseline.

Every leaf MUST NOT commit. Produce the patch with:

```sh
cd $M/worktrees/<worktree>
git add -N <any new files>            # already staged files stay staged
git diff HEAD --binary > $M/patches/<leaf>.patch
```

The diff must contain only intended source changes: no `.o`, `.ko`, `.cmd`,
`modules.order`, `Module.symvers`, `.mod.c`, `.config`, `include/config/`,
generated files, no binary hunks, no firmware blobs. Verify with
`grep -c 'GIT binary patch' $M/patches/<leaf>.patch` → 0 and inspect
`git diff HEAD --stat` before delivering.

## 2. Rules

- One leaf per worktree; never edit `$M/worktrees/integration`,
  `$M/worktrees/wcn-gap-integration`, `$M/worktrees/audio-gap-integration`, any
  other worktree, `$M/nura.config`, or any shared `.dts`/`.dtsi`.
- No stubs, fake success, `TODO: implement`, placeholder probes, invented
  register values. Every register/value cited from downstream source
  (`$M/src/downstream-grandpplte`, `file:line`) or ROM ELF/strings (symbol +
  address); anything inferred is marked `[INFERENCE]`.
- If a gap needs data absent from every source: implement what is possible, then
  report the exact missing input and stop that part. Never fabricate it.
- Reject unsupported userspace requests with a real errno instead of accepting
  and ignoring them.
- Keep the existing coding style; `W=1` clean for every touched object.
- Do not reorder or "fix" a peer's hunks in shared files (`mtk_wcn_fm.h` is
  shared between the FM RDS leaf's already-applied patch and the FM I2S leaf;
  the FM I2S leaf adds only its own declarations).
- No full kernel build. Compile the touched objects/module and the userspace
  tools only; that is the acceptance bar. Tree-wide modpost failures for a
  standalone module link are an environment limitation — report, do not stub.

## 3. Compile recipe (kernel)

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/<worktree>
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e RFKILL -e CRC32 -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM \
  -e SOUND -e SND -m SND_SOC_MT6735 -m SND_SOC_MT6735_MT6331 -m SND_SOC_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j4 \
  <objects...>
```

Notes: `$M/nura.config` carries **no** `CONFIG_MTK_*` and no sound symbols, so
the `scripts/config` lines above are mandatory: `SOUND`/`SND` gate all of
`sound/soc`, and without them `SND_SOC*` and the AFE objects are invisible
("No rule to make target"). `MTK_WCN_FM` is a tristate and it selects/needs
`SND_SOC`. For the FM leaf also build with
`CONFIG_SND_SOC=n` (`./scripts/config -d SND_SOC` + olddefconfig) and with
`SND_SOC=m`; all three combinations must compile. For the WMT leaf also compile
with `CONFIG_MTK_WCN_WMT=y`. Userspace tools: host `cc` with
`-Wall -Wextra -Werror`, and run their self-test.

## 4. Evidence index

Downstream root: `$M/src/downstream-grandpplte` (Android 6.0.1, 3.18).
ROM: kernel `$M/zip/vmlinux.elf` (32-bit ARM, kallsyms recovered),
`$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`, `/system` in
`$M/zip/system.raw.img` (debugfs), vendor libs under `/system/lib`.

### WLAN (`rem-wcn-wlan`)
- Downstream: `drivers/misc/mediatek/connectivity/wlan/gen2/` —
  `mgmt/{ais_fsm,auth,assoc,bss,cnm,rsn,aaa_fsm,hem_mbox,hs20,mib}.c`,
  `nic/{nic,nic_tx,nic_rx,que,cmd_buf,rxm,hal_dm}.c`,
  `include/nic/mtreg.h` (AHB register map, `WCIR/WHLPCR/WHISR/WTSR/WRPLR`),
  `include/nic_cmd_event.h`, `include/nic_init_cmd_event.h`,
  `os/linux/gl_*.c` (cfg80211/wext glue, scan/connect entry points),
  `include/mgmt/aaa_fsm.h` (auth/assoc state machine), `include/mgmt/ais_fsm.h`.
- ROM: 337 `wlan*`-containing symbols in `$M/zip/symbols.txt`, e.g.
  `wlanSendCommand`, `wlanQueryInformation`, `wlanSetInformation`,
  `wlanProcessCommandQueue`, `wlanImageSectionDownload`, `wlanIST`,
  `wlanoidSetAuthMode`, `wlanoidSetConnInfra`. Use them to confirm the
  command/event opcode names the HIF must carry.
- The existing Nura HIF is `wlan_hif.h` + `wlan_ahb.c` (base port). Management
  frames MUST go through that TX/RX path; do not add a second HIF.

### WMT (`rem-wcn-wmt`)
- Downstream: `drivers/misc/mediatek/connectivity/common/conn_soc/core/`
  (`stp_core.c`, `psm_core.c`, `wmt_lib.c`, `include/psm_core.h`,
  `include/stp_core.h`) — PSM state machine, op codes, `stp_psm_*` semantics
  (this 3.18 tree calls the file `psm_core.c`; the ROM uses `stp_psm.c`, so map
  `psm_core.c` function-for-function).
- Downstream coredump/netlink: `common/conn_soc/linux/pri/stp_dbg.c` +
  `include/stp_dbg.h` (generic-netlink family `STP_DBG`, BIND/RESET commands,
  `[M]` chunk framing, `stp_dbg_dmp_out`).
- Downstream coexistence: `common/conn_soc/linux/pub/{wmt_idc.c,bgw_desense.c}`
  + `include/{wmt_idc.h,bgw_desense.h}`, and `common/conn_soc/core/wmt_lib.c`
  (`wmt_lib_idc_lock_aquire/release`, BGW desense control).
- ROM symbols confirming the target ABI (all present in `$M/zip/symbols.txt`):
  PSM — `stp_psm_set_state`, `stp_psm_do_wakeup`, `stp_psm_notify_wmt_wakeup`,
  `stp_psm_notify_wmt_sleep`, `stp_psm_start_monitor`, `stp_psm_is_disable`,
  `stp_psm_has_pending_data`, `stp_psm_hold_data`, `stp_psm_release_data`,
  `stp_psm_is_to_block_traffic`, `stp_psm_disable_by_tx_rx_density`,
  `mtk_wcn_stp_psm_dbg_level`, plus the internal `_stp_psm_*` helpers;
  coredump — `stp_dbg_nl_bind`, `stp_dbg_nl_reset`, `stp_dbg_nl_send`,
  `stp_dbg_dmp_out`, `stp_dbg_pkt_log`, `wmt_core_set_coredump_state`,
  `wmt_ctrl_set_stp_dbg_info`, and BTM (`stp_btm_*`) helpers;
  coex — `wmt_ctrl_bgw_desense_ctrl`, `wmt_lib_idc_lock_aquire/release`,
  `wmt_dev_bgw_desense_init/deinit`.
- The GPS leaf's `mtk_wcn_wmt_ic_info_get()` /
  `mtk_wcn_wmt_co_clock_flag_get()` exports exist in the baseline header; keep
  their signatures byte-identical.

### FM I2S (`rem-wcn-fm-i2s`, `rem-wcn-fm-i2s-audio`)
- Downstream chip side: `drivers/misc/mediatek/connectivity/fmradio/mt6627/`
  (`pub/mt6627_fm_lib.c` — `fm_i2s_set()`/`fm_get_i2s_info()` logic,
  `FM_REG_AUDIO_CTRL0 = 0x9b` write `0xf9ab`, host `0x80101054 = 0x00003f35`).
- Downstream AP side: `sound/soc/mediatek/mt_soc_audio_v3/`
  `mt_soc_pcm_fm_i2s.c` (`mtk_pcm_fm_i2s_prepare()` at 267-330),
  `mt_soc_afe_control.c` (`Set2ndI2SIn()` 2214-2248, `Set2ndI2SInEnable()`,
  `SetI2SASRCConfig()`, `SetI2SDacOut()`, `Enable4pin_I2S0_I2S3()` 3754-3781),
  `AudDrv_Def.h` (names 230-231, 267-268, 306-307, 325-327, 342-343),
  `mt_soc_machine.c:1068-1086` (the FM DAI links).
- ROM symbols: `fm_i2s_set`, `fm_get_i2s_info`, `mt6627_get_chipid`,
  `mt6627_host_write`, `mt6627_get_audio_info`, `MT6627fm_low_power_wa_default`
  in `$M/zip/symbols.txt`.

## 5. Frozen FM I2S naming contract (both FM leaves MUST use these exact strings)

```c
/* WCN FM driver component (rem-wcn-fm-i2s) */
#define MTK_WCN_FM_I2S_CODEC_NAME   "mt6627-fm-i2s"            /* snd_soc_component name */
#define MTK_WCN_FM_I2S_DAI_NAME     "mt-soc-fm-i2s-dai-codec"  /* downstream MT_SOC_CODEC_FM_I2S_DAI_NAME, AudDrv_Def.h:307 */
#define MTK_WCN_FM_I2S_PB_STREAM    "FM_I2S_Playback"          /* AudDrv_Def.h:342 */
#define MTK_WCN_FM_I2S_CAP_STREAM   "FM_I2S_Capture"           /* AudDrv_Def.h:343 */

/* AFE CPU DAI (rem-wcn-fm-i2s-audio) */
#define MT6735_AFE_FM_I2S_DAI_NAME  "mt-soc-fmi2S-driver"      /* downstream MT_SOC_FM_I2S_NAME, AudDrv_Def.h:230 */
```

Machine link in `mt6735-mt6331.c` (audio leaf):

```c
SND_SOC_DAILINK_DEFS(fm_i2s,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-fmi2S-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6627-fm-i2s",
				      "mt-soc-fm-i2s-dai-codec")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
/* link: .name = "FM_I2S_RX_Playback", .stream_name = "FM_I2S_Playback",
 * .playback_only = 1,
 * .dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC
 */
```

ASoC binds a component to exactly one card (`soc_probe_component()`); the FM
driver therefore registers **no card**, only that component, and the existing
`mt-snd-card` / `mediatek,mt6735-sound` machine driver references it. A second
`snd_soc_register_card()` is a hard failure.

Hardware constraint to honour and document: downstream programs the 2nd I2S
input through **`AFE_I2S_CON` (0x0018)** (`mt_soc_afe_control.c:2231`), which is
the same register the MT6735 I2S0 4-pin path uses
(`Enable4pin_I2S0_I2S3()`, `mt_soc_afe_control.c:3761/3781`) — the two users are
mutually exclusive. The AFE leaf must enforce that (e.g. `-EBUSY` when I2S0 is
active) instead of silently overwriting the register, and say so in the report.

## 6. Integration order (coordinator)

`parity-baseline-all.patch` →
`rem-wcn-wmt.patch` → `rem-wcn-wlan.patch` → `rem-wcn-fm-i2s.patch` →
`rem-wcn-fm-i2s-audio.patch` (audio fragment: applied after the audio series,
listed separately in the coordinator report).

`rem-wcn-fm-i2s-audio.patch` touches **only** `sound/soc/mediatek/mt6735/` and
`sound/soc/mediatek/Kconfig|Makefile`; every other path belongs to the WCN-side
patch. State that split explicitly in the report.

## 7. Userspace helpers (only where a helper is genuinely required)

- WLAN: cfg80211 is the ABI; `iw` / `wpa_supplicant` are the userspace. Do not
  write a custom WLAN control tool. Say so in the report.
- WMT: the coredump daemon is required (the kernel only pushes to a bound
  netlink peer). Deliver `tools/mediatek/wcn/stp_dbg/` (host-buildable,
  `-Wall -Wextra -Werror`, `make check` self-test that exercises the real
  chunk-parser) matching the kernel's family/attribute numbering exactly.
- FM: `tools/mediatek/wcn/fm_i2s/` exercising `FM_IOCTL_I2S_SETTING` /
  `FM_IOCTL_GET_I2S_INFO` through the UAPI header, with an offline self-test
  (struct size/layout and request encoding) that does not need a device.
