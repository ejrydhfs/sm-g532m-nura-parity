# rem-wcn-fm-i2s — MT6627 FM I2S, WCN side

Worktree: `$M/worktrees/rem-wcn-fm-i2s`
HEAD: `baseline: parity-baseline-all.patch` + two coordinator baseline fixups
(`drivers/misc/Kconfig` sources `drivers/misc/mediatek/Kconfig`,
`drivers/misc/Makefile` descends into `mediatek/`). Both fixups are in HEAD and
are **not** part of this patch.

## 1. Assigned driver and files changed

Driver: MediaTek WCN FM radio (`mtk_wcn_fm`), MT6627 I2S audio port, WCN side.
Nothing under `sound/` is touched.

| File | Change |
|---|---|
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_i2s.c` | new: ASoC codec component, DAI ops, route/power bookkeeping |
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_i2s.h` | new: glue interface + `SND_SOC`-less inline fallbacks |
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_chip.c` | `mtk_wcn_fm_chip_i2s_set()/_get()/_info()`; I2S state in power-up/power-down/tune |
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_v4l2.c` | `FM_IOCTL_I2S_SETTING`, `FM_IOCTL_GET_I2S_INFO`, compat dispatch, power/retune hooks |
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.h` | I2S ABI include, device state, chip-layer declarations |
| `drivers/misc/mediatek/connectivity/fm/Makefile` | build the glue only when it is linkable |
| `include/uapi/linux/mtk_fm_i2s.h` | new: shared UAPI of the two 12-byte ioctls |
| `tools/mediatek/wcn/fm_i2s/{Makefile,fm_i2s.c,selftest.c}` | new: host helper + offline ABI self-test |

`Kconfig` is intentionally unchanged: the driver must build and keep working
with `CONFIG_SND_SOC=n`, so it neither selects nor depends on `SND_SOC`.

## 2. Evidence

### ROM (`$M/zip/vmlinux.elf`, 32-bit ARM, kallsyms recovered)

| Symbol | Address | Use |
|---|---|---|
| `fm_i2s_set` | `0xc063a59c` | ioctl SET handler semantics |
| `fm_get_i2s_info` | `0xc0638e90` | ioctl GET handler semantics |
| `mt6627_host_write` | `0xc06442e8` | host-register write path |
| `mt6627fm_get_audio_info` | `0xc0643dc0` | audio config copy |
| `mt6627_I2s_Setting` | absent | confirms the chip hook is compiled out |

Disassembly of `fm_i2s_set` (`0xc063a59c`): it loads `fm_low_ops` and tests
`[fm_low_ops+0xa4]` (the `bi.i2s_set` hook); NULL returns `0xfffffc16`
(`-1002 = -FM_EPARA`) after a `printk`. When the hook exists it passes
`r0 = 0` (onoff forced to `FM_I2S_ON`), `r1 = mode`, `r2 = sample`. This
matches `core/fm_main.c:1551-1568` exactly, including the always-true
`if ((onoff != 0) || (onoff != 1)) onoff = 0;`. In the shipped ROM the hook is
NULL (`mt6627_I2s_Setting` is under `#if 0`, `mt6627_fm_lib.c:1825-1827`), so
the stock ioctl always failed with `-1002`.

`$M/zip/vmlinux.strings:45672,45704` contain `FM_IOCTL_GET_I2S_INFO` and
`FM_IOCTL_I2S_SETTING`.

### Downstream source (`$M/src/downstream-grandpplte`)

| Evidence | File:line |
|---|---|
| ioctl numbers | `fmradio/inc/fm_ioctl.h:70,81` (`_IOWR(magic,33,...*)`, `_IOWR(magic,41,...*)`), magic `:19-20` |
| 12-byte payload, enum values | `fmradio/inc/fm_interface.h:59-90` |
| ioctl handlers | `fmradio/core/fm_module.c:294-315` (GET), `:735-757` (SET) |
| `fm_i2s_set()` / `fm_get_i2s_info()` | `fmradio/core/fm_main.c:1551-1568`, `:935-942` |
| i2s_get state source | `mt6627/pub/mt6627_fm_lib.c:1451-1471` |
| fixed audio config | `mt6627/pub/mt6627_fm_config.c:115-119` (`FM_I2S_MASTER`, `FM_I2S_32K`, `FM_I2S_OFF`) |
| enable writes (power up) | `mt6627/pub/mt6627_fm_lib.c:849-851` — `mt6627_write(0x9B,0xF9AB)`, `mt6627_host_write(0x80101054,0x00003f35)` |
| enable writes (every retune) | `mt6627/pub/mt6627_fm_lib.c:1022-1024` |
| enable writes (search restore) | `mt6627/pub/mt6627_fm_lib.c:1508-1513` |
| disable (power down host) | `mt6627/pub/mt6627_fm_lib.c:892-894` — `host &= 0xFFFF9FFF` |
| disable (power down chip) | `mt6627/pub/mt6627_fm_cmd.c:521-523` — `fm_bop_modify(0x9B,0xFFF8,0x0000)` |
| no programmable rate/mode | `mt6627/pub/mt6627_fm_lib.c:88-90,1825-1827`; no `mt6627_I2s_Setting` body |
| DAI/stream names | `sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Def.h:230,306-307,342-343` |
| machine link | `.../mt_soc_machine.c:1068-1086` |
| AP-side 2nd I2S-in | `.../mt_soc_pcm_fm_i2s.c:267-330`, `.../mt_soc_afe_control.c:2214-2248` |

## 3. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| Codec component name | `MT_SOC_CODEC_NAME` machine-side codec | none | `"mt6627-fm-i2s"` | done |
| Codec DAI name | `MT_SOC_CODEC_FM_I2S_DAI_NAME "mt-soc-fm-i2s-dai-codec"` (`AudDrv_Def.h:307`) | partial code used the component name | DAI `.name = "mt-soc-fm-i2s-dai-codec"` | done |
| Stream names | `FM_I2S_Playback` / `FM_I2S_Capture` (`AudDrv_Def.h:342-343`) | correct in partial code | unchanged | done |
| Sound card ownership | single machine card `mt-snd-card` | no card | no card; component only | done |
| Fixed format | config `FM_I2S_MASTER`, `FM_I2S_32K`; AP side slave/32000/16-bit (`mt_soc_pcm_fm_i2s.c:305-320`) | partial enforced rate/ch/format | `hw_params`/`set_fmt` reject rate != 32000, ch != 2, fmt != S16_LE, DAI fmt != I2S/NB_NF/CBC_CFC with `-EINVAL`/`-EOPNOTSUPP` | done |
| DAPM | no codec-side switch; path lives in AFE | none | no widgets/controls | done |
| Unbound component | n/a | `route(ON)` succeeded with no link | `-ENODEV` on enable when no card bound; disable still works | done |
| Chip enable | `0x9B=0xF9AB`, host `0x80101054=0x00003f35` | absent | exact writes, `-EAGAIN` when unpowered | done |
| Chip disable | host `&=0xFFFF9FFF`; `0x9B &= ~0xFFF8` | absent | exact writes | done |
| Read back | no downstream read-back | absent | `0x9B & 0xFFF8 != 0` and host bits 13:14 == `0x2000` | done, `[INFERENCE]` |
| GET info | config copy (`fm_lib.c:1451-1471`) | absent | status = real port state (`FM_I2S_OFF` when unpowered), mode master, rate 32K | done |
| SET ioctl | 12-byte copy; ROM always `-1002` because hook is NULL | absent | 12-byte copy, onoff validated, mode/rate other than master/32K → `-EOPNOTSUPP`, unpowered → `-EAGAIN` | done |
| `compat_ioctl` | 32-bit ROM command word `_IOC_SIZE == 4` | absent | `.compat_ioctl32` uses the same handler, pointer-free payload | done |
| Power up | enables port (`fm_lib.c:849-851`) | port on, glue unaware | glue mirrors `route=on`, `chip_on=true` after `power_up` | done |
| Power down | clears host bits (`:892-894`), then `pwrdown` clears `0x9B` | glue unaware | glue clears state **before** `chip_power_down` | done |
| Retune | re-enables port on every `SetFreq` (`:1022-1024`) | state would go stale | `mtk_wcn_fm_i2s_resync()` after `s_frequency` and after the band-change retune; restores the requested route (off stays off) | done |
| Build with `SND_SOC=n/m/y` | n/a | no glue | all three compile; glue object only when linkable | done |
| Userspace control | downstream app used the ioctls | none | `tools/mediatek/wcn/fm_i2s` + offline self-test | done |
| AFE 2nd I2S input / card link | `Set2ndI2SIn`, `mt_soc_machine.c:1068-1086` | absent | out of scope (audio leaf) | deferred → `AUDIO-FRAGMENT` |

Notes on the two deliberate divergences from the literal downstream code:

* `fm_i2s_set()` in downstream/ROM forces `onoff = FM_I2S_ON` because its
  condition is always true, and the chip hook is compiled out, so the stock
  ioctl always returns `-FM_EPARA`. The Nura driver implements what the
  interface promises (real on/off) instead of reproducing the dead path; the
  wire ABI is unchanged.
* The downstream `i2s_get` returns the cached config, which is `FM_I2S_OFF`
  until someone calls the dead setter. The Nura driver reports the real
  register state when powered and `FM_I2S_OFF` when not, which cannot claim
  "on" for a powered-down chip.

## 4. Compile / test

Toolchain: `PATH=$M/llvmpkg/usr/bin:$M/bin:$PATH`, `ARCH=arm64 LLVM=1`,
`W=1`, `-j2`. The host `/tmp` tmpfs was full from concurrent builds, so the
kernel builds ran with `TMPDIR=$M/tmp/remwcn/tmp` (environmental only; it does
not change the command). The userspace Makefile keeps compiler temporaries in
`tools/mediatek/wcn/fm_i2s/.tmp` for the same reason.

Config base (`$M/nura.config` has no sound symbols):

```sh
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV \
  -e BT -e CFG80211 -e RFKILL -e CRC32 -e SOUND -e SND -e SND_SOC -e NVMEM \
  -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
```

Build target (composite module object, so the directory `ccflags-y` applies):

```sh
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o
```

Results (all exit 0, no `warning`, no `error` in the logs):

```text
SND_SOC=y (MTK_WCN_FM=m)
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_link.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_chip.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_v4l2.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_i2s.o
  LD [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o

SND_SOC=m (MTK_WCN_FM=m)
  CC [M]  .../mtk_wcn_fm_link.o
  CC [M]  .../mtk_wcn_fm_chip.o
  CC [M]  .../mtk_wcn_fm_v4l2.o
  CC [M]  .../mtk_wcn_fm_i2s.o
  LD [M]  .../mtk_wcn_fm.o

SND_SOC=n (MTK_WCN_FM=m)
  CC [M]  .../mtk_wcn_fm_link.o
  CC [M]  .../mtk_wcn_fm_chip.o
  CC [M]  .../mtk_wcn_fm_v4l2.o
  LD [M]  .../mtk_wcn_fm.o          (no mtk_wcn_fm_i2s.o: inline fallbacks)
```

Build logs: `$M/tmp/remwcn/fm-i2s-build-final-sndsoc-{y,m,n}.log`.

Userspace (host `cc`, `-Wall -Wextra -Werror -std=gnu11`):

```text
$ make -C tools/mediatek/wcn/fm_i2s check
mkdir -p .../tools/mediatek/wcn/fm_i2s/.tmp
cc -idirafter .../include/uapi -O2 -g -Wall -Wextra -Werror -std=gnu11 -o fm_i2s fm_i2s.c selftest.c
./fm_i2s --selftest
fm_i2s selftest: PASS (37 checks)
```

The self-test checks size/offsets (12 bytes; 0/4/8), all enum values, and the
exact command words `0xc004f521` (0xf5,33,size 4, RW) and `0xc004f529`
(0xf5,41,size 4, RW), plus the three-word wire encoding.

`grep -rnE 'TODO|FIXME|XXX|stub|not implemented'` over every touched source
returns nothing. No full kernel build and no tree-wide modpost was run; a
standalone `mtk_wcn_fm.ko` modpost is the documented environment limitation
(unresolved kernel symbols), so only the `LD [M] mtk_wcn_fm.o` composite link
is claimed.

## 5. Inferred / not verified on hardware

* `[INFERENCE]` `mtk_wcn_fm_chip_i2s_get()`: downstream never reads the port
  back. "on" is derived from the exact write/clear masks (`0x9B` bits 15:3
  non-zero, host bits 13:14 == `0x2000`).
* `[INFERENCE]` The MT6627 port itself is fixed master/32 kHz: only the
  downstream *config* says so (`mt6627_fm_config.c:115-119`) and the AP side
  programs slave/32000 16-bit (`mt_soc_pcm_fm_i2s.c:305-320`); the chip setter
  is compiled out. Rejecting slave/44.1/48 kHz is therefore a policy derived
  from that config, not from a documented chip register.
* Not hardware-tested (no phone): actual audio on the connectivity I2S pads,
  register read-back values, the port surviving retune/reset sequences on
  silicon, and the ASoC card binding (requires the audio leaf's AFE DAI).
* `FM_IOCTL_I2S_SETTING(ON)` returns `-ENODEV` until a machine link binds
  `mt6627-fm-i2s` to the card. That is intentional: enabling the port with no
  PCM consumer would silently produce no audio.
* The `CONFIG_MTK_WCN_FM=y` + `CONFIG_SND_SOC=m` combination is handled by the
  same fallback rule (Makefile + header guard), but could not be built here
  because `MTK_WCN_FM` is capped at `=m` by its `MTK_WCN_WMT` dependency in
  this tree. The tested matrix is the required `SND_SOC=n/m/y` with the FM
  driver as a module.

## 6. DTS fragment

None. The FM node (`mediatek,mt_mt6627_fmradio`) is unchanged; the codec
component is not represented in DT (the machine link is a C-level DAI link).

## 7. Patch

`$M/patches/rem-wcn-fm-i2s.patch` (`git diff HEAD --binary`, 10 files,
0 binary hunks, no generated files).

## AUDIO-FRAGMENT

The audio side is a **separate series** (`rem-wcn-fm-i2s-audio.patch`), applied
after the audio integration/gap patches; it must touch only
`sound/soc/mediatek/` and its Kconfig/Makefile. This WCN patch touches nothing
under `sound/`.

Exact names the audio side must provide/reference:

```c
/* CPU DAI, owned by the AFE (downstream MT_SOC_FM_I2S_NAME,
 * AudDrv_Def.h:230; used as cpu_dai_name in mt_soc_machine.c:1070,1080) */
#define MT6735_AFE_FM_I2S_DAI_NAME  "mt-soc-fmi2S-driver"

/* Codec side provided by this patch (rem-wcn-fm-i2s) */
#define MTK_WCN_FM_I2S_CODEC_NAME   "mt6627-fm-i2s"
#define MTK_WCN_FM_I2S_DAI_NAME     "mt-soc-fm-i2s-dai-codec" /* AudDrv_Def.h:307 */
#define MTK_WCN_FM_I2S_PB_STREAM    "FM_I2S_Playback"         /* AudDrv_Def.h:342 */
#define MTK_WCN_FM_I2S_CAP_STREAM   "FM_I2S_Capture"          /* AudDrv_Def.h:343 */
```

Machine link in the existing `mt-snd-card` (`mt6735-mt6331.c`):

```c
SND_SOC_DAILINK_DEFS(fm_i2s,
	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-fmi2S-driver")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6627-fm-i2s",
				      "mt-soc-fm-i2s-dai-codec")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
/* .name = "FM_I2S_RX_Playback", .stream_name = "FM_I2S_Playback",
 * .playback_only = 1,
 * .dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC
 */
```

Binding rules:

* ASoC binds a component to exactly one card. `soc_probe_component()`
  (`sound/soc/soc-core.c:1602-1622`) returns `-ENODEV` when
  `component->card` is already a different card. This driver therefore
  registers **no** `snd_soc_register_card()`; a second card would be a hard
  failure. The board `mt-snd-card` and its platform name
  `mediatek,mt6735-sound` stay unchanged.
* The FM DAI name must be `mt-soc-fm-i2s-dai-codec` and the CPU DAI name
  `mt-soc-fmi2S-driver`; any other spelling makes the link fail to bind.

AFE constraint: downstream programs the 2nd I2S input through
`AFE_I2S_CON` (`AFE_BASE + 0x0018`, `AudDrv_Afe.h:429`) in `Set2ndI2SIn()`
(`mt_soc_afe_control.c:2231`), and the MT6735 4-pin I2S0 path programs and
enables the **same** register in `Enable4pin_I2S0_I2S3()`
(`mt_soc_afe_control.c:3761,3781`). The two users are mutually exclusive: the
AFE leaf must not silently overwrite the register when I2S0 is active; it must
fail (e.g. `-EBUSY`) and report it. Downstream values for the FM path:
`mI2S_IN_PAD_SEL = false` (pads from CONNSYS), slave, 32000 Hz, 16-bit words,
I2S format, LRCK not inverted, phase-shift-fix bit 31 set
(`mt_soc_pcm_fm_i2s.c:305-320`, `mt_soc_afe_control.c:2223-2231`).
