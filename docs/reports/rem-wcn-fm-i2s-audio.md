# FM I2S, AFE / machine side (`rem-wcn-fm-i2s-audio`)

Leaf: **audio + WCN baseline worktree, AFE back end and `mt-snd-card` link only**
Target: Nura `v7.0.10-mt6735`, arm64, MT6737T (SM-G532M "grandpplte")
Downstream reference: `$M/src/downstream-grandpplte` (Samsung/MTK GPL, Android 6.0.1, 3.18)
ROM evidence: `$M/zip/vmlinux.elf` (llvm-nm/llvm-objdump), `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`
No phone attached: acceptance below is source parity + clean compile + an explicit hardware-unknown list.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

Worktree: `$M/worktrees/rem-wcn-fm-i2s-audio`
HEAD = `9b66b7790` (`baseline: parity-baseline-all.patch` + the two shared
`baseline fixup:` commits that make `drivers/misc/{Kconfig,Makefile}` reach
`drivers/misc/mediatek/`; those two commits are **not** part of this patch).

---

## 1. Assigned driver and files changed

Assigned slice: the AFE-side FM I2S back end (CPU DAI) and the single machine
link that binds the WCN FM codec component to the existing card. Everything is
inside `sound/soc/mediatek/mt6735/`; no `drivers/` file is touched, no
`sound/soc/mediatek/Kconfig|Makefile` change was needed.

| File | Change |
|---|---|
| `sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.c` | **new** (395 lines): AFE CPU DAI `mt-soc-fmi2S-driver`, startup/shutdown/hw_params/prepare/trigger, FM teardown |
| `sound/soc/mediatek/mt6735/mt6735-afe-common.h` | `MT6735_AFE_DAI_FM_I2S` DAI id (after the contiguous voice/BT block), `MT6735_AFE_FM_I2S_DAI_NAME`, fixed 32 kHz/16-bit/stereo limits, `i2s_con_lock` + `i2s0_active`/`fm_i2s_active`/`fm_i2s_prepared`/`fm_i2s_on`/`fm_i2s_dac_owner`, external DAI declaration |
| `sound/soc/mediatek/mt6735/mt6735-afe-pcm.c` | count the FM path in `mt6735_afe_update_power()` (`EnableAfe`), register `dais[MT6735_AFE_DAI_FM_I2S]`, init the new mutex, extend the memif-less DAI comment |
| `sound/soc/mediatek/mt6735/mt6735-afe-regs.h` | `AFE_GAIN1_CON0/CON1/CUR/CONN/CONN2` (+ field masks), the four FM connection bits, `AFE_CONN4_ASRC_BYPASS`, `AFE_DAC_CON0_I2S_IN_2_EN` |
| `sound/soc/mediatek/mt6735/mt6735-dai-i2s.c` | symmetric `-EBUSY` against the FM path (I2S0 startup/shutdown), `i2s0_active` |
| `sound/soc/mediatek/mt6735/mt6735-mt6331.c` | one DAI link `FM_I2S_RX_Playback` / `FM_I2S_Playback` in the existing `mt-snd-card` table, compiled only when `CONFIG_MTK_WCN_FM` is enabled |
| `sound/soc/mediatek/mt6735/Makefile` | add `mt6735-dai-fm-i2s.o` to `snd-soc-mt6735-afe-y` |

Card name (`mt-snd-card`), platform driver (`mediatek,mt6735-sound`) and all
ten pre-existing links are unchanged; exactly one link was added, behind
`#if IS_ENABLED(CONFIG_MTK_WCN_FM)` (see §5.2) so an optional subsystem can
never defer the whole card.

---

## 2. ROM and downstream evidence

### 2.1 Names (frozen contract, ROM strings)

`$M/zip/vmlinux.strings`:

```
59307:mt-soc-fmi2S-driver
59308:FM_I2S_Playback
59559:mt-soc-fm-i2s-dai-codec
59951:FM_I2S_RX_Playback
```

Downstream `AudDrv_Def.h`:

```
230:#define MT_SOC_FM_I2S_NAME "mt-soc-fmi2S-driver"
307:#define MT_SOC_CODEC_FM_I2S_DAI_NAME "mt-soc-fm-i2s-dai-codec"
342:#define MT_SOC_FM_I2S_PLAYBACK_STREAM_NAME "FM_I2S_Playback"
343:#define MT_SOC_FM_I2S_CAPTURE_STREAM_NAME "FM_I2S_Capture"
```

Machine link: `mt_soc_machine.c:1068-1076`
(`.name = "FM_I2S_RX_Playback"`, `.stream_name = MT_SOC_FM_I2S_PLAYBACK_STREAM_NAME`,
`.cpu_dai_name = MT_SOC_FM_I2S_NAME`, `.codec_dai_name = MT_SOC_CODEC_FM_I2S_DAI_NAME`).

### 2.2 ROM symbols (addresses from `llvm-nm -n $M/zip/vmlinux.elf`)

| Symbol | Address | Used for |
|---|---|---|
| `Set2ndI2SIn` | `0xc084a324` | AFE_I2S_CON 0x0018 programming |
| `Set2ndI2SInEnable` | `0xc084a450` | bit 0 of AFE_I2S_CON |
| `SetI2SDacOut` | `0xc0849284` | AFE_I2S_CON1 (PMIC I2S DAC) |
| `SetI2SDacEnable` | `0xc084a0c8` | I2S_OUT_DAC / ADDA enables |
| `SetMemoryPathEnable` | `0xc0849f78` | AFE_DAC_CON0 bit (block+1) |
| `SetHwDigitalGainMode` / `Enable` / `Gain` | `0xc08492ec` / `0xc0849350` / `0xc08493ec` | gain block 1 |
| `SetI2SASRCConfig` / `SetI2SASRCEnable` | `0xc084a4a0` / `0xc084a604` | ASRC (bypassed here) |
| `Enable4pin_I2S0_I2S3` | `0xc084ccfc` | I2S0 also writes AFE_I2S_CON |
| `SetConnectionState` | `0xc084d8cc` | connection table semantics |
| `mtk_pcm_fm_i2s_prepare` | `0xc085a850` | the prepare sequence |
| `fm_i2s_set` | `0xc063a59c` | chip-side I2S (peer leaf) |

ROM disassembly confirms the downstream source bit-for-bit:

* `Set2ndI2SIn` (`0xc084a37c-0xc084a3ac`) composes
  `INV_LRCK << 5 | FMT << 3 | (1 << 31) | WLEN << 1 | PAD_SEL << 28 | SLAVE << 2`
  and issues `Afe_Set_Reg(0x18, value, 0xfffffffe)` (register 0x0018 = `AFE_I2S_CON`,
  mask keeps only the enable bit).
* `mtk_pcm_fm_i2s_prepare` (`0xc085a850-0xc085a9e8`):
  `SetConnection(connect=1, I00→O13)`, `(1, I01→O14)`, `(1, I10→O03)`,
  `(1, I11→O04)` (`0xc085a898-0xc085a8d4`);
  `SetHwDigitalGainMode(GAIN1, runtime->rate, 64)` (`0xc085a8d8`),
  `SetHwDigitalGainEnable(GAIN1, 1)`, `SetHwDigitalGain(mfm_i2s_Volume, GAIN1)`;
  `GetMemoryPathEnable(12)`/`SetI2SDacOut`/`SetMemoryPathEnable(12,1)`/`SetI2SDacEnable(1)`
  (`0xc085a9c0-0xc085a9e0`); `GetMemoryPathEnable(17)`/`SetMemoryPathEnable(17,1)`,
  `Set2ndI2SIn` with sample rate 32000, slave = 1, format = I2S, WLEN = 16 bit,
  pad select = 0 (`0xc085a924-0xc085a978`); ASRC config + enable
  (`0xc085a97c-0xc085a998`); `Set2ndI2SInEnable(1)`; `EnableAfe(1)`.

### 2.3 Downstream source lines used (all values cited in the code)

* `mt_soc_pcm_fm_i2s.c` — volume `:73`, hardware `:134-150`, open/clk `:190-200`,
  rate list `:204`, integer periods `:206`, close `:227-264`, prepare `:267-329`
  (connections `:278-285`, gain `:288-291`, DAC `:294-299`, I2S_IN_2 `:302`,
  2nd-I2S attributes `:305-314`, ASRC `:316-321`, enable `:322`, `EnableAfe` `:326`).
* `mt_soc_afe_control.c` — `SetI2SDacOut` `:1637-1651`, gain mode `:1663,1667`,
  gain enable `:1686,1688`, gain value `:1711`, `SetMemoryPathEnable` `:1968-2000`
  (bit = block+1 at `:1995-1997`), `SetI2SDacEnable` `:2010-2034`,
  `Set2ndI2SIn` `:2214-2239` (bit wiring `:2224-2229`, `Afe_Set_Reg(AFE_I2S_CON,…)`
  `:2231`), `Set2ndI2SInEnable` `:2241-2249`, `SetI2SASRCConfig` `:2251-2278`
  (ASRC path `:2257`, bypass `:2274`), `Enable4pin_I2S0_I2S3` `:3740-3786`.
* `mt_soc_afe_connection.c` — `mConnectionReg[]` `:281-306`,
  `mConnectionbits[]` `:173-200`; the four routes programmed by
  `mtk_pcm_fm_i2s_prepare()` (`:278-285`) are cited individually in
  `mt6735-afe-regs.h` (`[I00][O13]` 0x448/bit 2, `[I01][O14]` 0x448/bit 16,
  `[I10][O03]` 0x420/bit 8, `[I11][O04]` 0x420/bit 10); the table also has
  possible `[I00][O14]` (bit 15) and `[I01][O13]` (bit 3) entries that
  downstream never connects, so they are deliberately not programmed.
* `AudDrv_Afe.h` — `AFE_I2S_CON` `:429`, `AFE_CONN4` `:436`, `AFE_GAIN1_CON0/CON1/
  CONN/CUR` `:618,619,622,623`, `AFE_GAIN1_CONN2` `:632`.
* `mt_soc_digital_type.h` — block numbers `:53-82` (`I2S_OUT_DAC` = 12 at `:68`,
  `I2S_IN_2` = 17 at `:74`), `Soc_Aud_I2S_SAMPLERATE_I2S_32K` = 8 at `:272`.
* `mt_soc_pcm_common.h:210-212` — `soc_fm_supported_sample_rates[] = { 32000, 44100, 48000 }`.

---

## 3. Parity matrix

| # | Behaviour | ROM / downstream | Nura before | Nura after | Status |
|---|---|---|---|---|---|
| 1 | AFE CPU DAI name | `MT_SOC_FM_I2S_NAME` (`AudDrv_Def.h:230`) | absent | `mt-soc-fmi2S-driver`, `.id = MT6735_AFE_DAI_FM_I2S`, registered on the existing `mtk-afe-pcm` component | done |
| 2 | Memif-less back end | no AP memory path (`mtk_pcm_fm_i2s_copy()` only advances its own counter) | – | `mt6735_afe_dai_memif_id()` returns -1, `pointer()` returns 0, 36 KiB managed ring only | done |
| 3 | `startup` | `AudDrv_Clk_On()`/`AudDrv_I2S_Clk_On()` `:196-197`, hw `:200`, rate list `:204`, integer periods `:206` | – | `pm_runtime_get_sync`, `snd_soc_set_runtime_hwparams`, constraints {32000}, channels 2, S16_LE, integer periods | done |
| 4 | `hw_params` | no-op downstream (`:167-174`) | – | validates 32000 Hz / 2 ch / S16_LE, `-EINVAL` otherwise | done |
| 5 | Connections | I00→O13, I01→O14, I10→O03, I11→O04 (`:278-285`; four routes, no cross-connections) | – | `AFE_GAIN1_CONN2` bits 2/16 + `AFE_GAIN1_CONN` bits 8/10 | done |
| 6 | Digital gain 1 | mode `:288-289`, enable `:290`, gain `0x10000` `:291` | – | `AFE_GAIN1_CON0` 0xfff0 field, `AFE_GAIN1_CUR`=0 then enable, `AFE_GAIN1_CON1`=`0x10000` | done |
| 7 | `SetI2SDacOut` + I2S_OUT_DAC | `:294-297`, `SetI2SDacOut()` `:1637-1651` | `mt6735_afe_dac_prepare()` existed for DL1 | reuses `mt6735_afe_dac_prepare(afe, 32000, 16-bit)` (CleanPreDistortion + SetDLSrc2 + AFE_I2S_CON1 + SetI2SDacEnable) and sets `AFE_DAC_CON0` bit 13; skipped when another stream owns the PMIC DAC | done |
| 8 | `I2S_IN_2` memory path | `SetMemoryPathEnable(17, true)` `:302` | – | `AFE_DAC_CON0_I2S_IN_2_EN` (bit 18) | done |
| 9 | `Set2ndI2SIn()` on `AFE_I2S_CON` 0x0018 | pad select CONNSYS (bit28=0), slave (bit2=1), I2S (bit3=1), 16-bit (bit1=0), LRCK not inverted (bit5=0), phase-shift fix (bit31=1), mask 0xfffffffe (`:2224-2231`) | I2S0 owned the register | same value, bit 0 left to the trigger | done |
| 10 | `Set2ndI2SInEnable` | bit 0 (`:2241-2249`) | – | trigger START/RESUME/PAUSE_RELEASE sets it, STOP/SUSPEND/PAUSE_PUSH clears it | done |
| 11 | `EnableAfe` | `EnableAfe(true)` `:326`, `EnableAfe(false)` `:257` | AFE ON bit counted memifs + voice | `fm_i2s_on` OR-ed into `mt6735_afe_update_power()`; PMIC pin mux selected in prepare, released on shutdown when no other user | done |
| 12 | Teardown | `mtk_pcm_fm_i2s_close()` `:227-264`: I2S_IN_2 off, ASRC off, bit0 off, I2S_OUT_DAC off, `SetI2SDacEnable(false)`, disconnect, `EnableAfe(false)`, clocks off | – | `mt6735_afe_fm_i2s_teardown()` (idempotent) + `pm_runtime_put()`; DAC released only if this DAI owns it and no multimedia/voice user remains | done |
| 13 | ASRC 32 kHz → 44.1/48 kHz | `SetI2SASRCConfig(true, 44100/48000)` `:316-321` | – | **not engaged**: the frozen hw_params is 32 kHz, so input and DAC-out rates are equal; the documented bypass (`AFE_CONN4` bit 30 = 1, `:2274`) is programmed instead. Register/sequence availability is discussed in §5.3 | deferred (by design, see §5.3) |
| 14 | I2S0 / FM register ownership | `Enable4pin_I2S0_I2S3()` writes `AFE_I2S_CON` (`:3759-3761`) | I2S0 wrote it unconditionally | mutual exclusion: FM startup fails `-EBUSY` while I2S0 is open, I2S0 startup fails `-EBUSY` while FM is open, both release in shutdown under `i2s_con_lock` | done |
| 15 | FM ↔ voice PMIC DAC | shared `AFE_I2S_CON1`, voice reprograms it at 8/16 kHz | unguarded | FM startup also fails `-EBUSY` while `voice_afe_users > 0`; teardown never disables the voice DAC | done |
| 16 | Machine link | `mt_soc_machine.c:1068-1076` | absent | one link `FM_I2S_RX_Playback` / `FM_I2S_Playback`, `playback_only = 1`, `I2S \| NB_NF \| CBC_CFC`, compiled only with `CONFIG_MTK_WCN_FM` | done |
| 16b | Optional-subsystem coupling | downstream always lists the link because its FM codec is in the always-built codec driver (`mt_soc_machine.c:1068-1076`) | – | link and its `DAILINK_DEFS` wrapped in `#if IS_ENABLED(CONFIG_MTK_WCN_FM)`; with `=n` the card binds with its ten base links and no FM PCM, with `=m` the link is present | done |
| 17 | FM capture link (`FM_I2S_RX_Capture`, dummy codec) | `mt_soc_machine.c:1077-1086` | – | not added: the assignment requires exactly one link | deferred (out of slice) |
| 18 | `Audio FM I2S Volume` kcontrol | `mt_soc_pcm_fm_i2s.c:71-94` | – | fixed gain `0x10000`; no new card-level kcontrol (out of the assigned op set, and the FM codec exposes its own volume path) | deferred (out of slice) |
| 19 | Buffer size | 64 KiB (`FM_I2S_MAX_BUFFER_SIZE`, `mt_soc_pcm_common.h:163`) | – | the AFE component's managed 36 KiB ring; the FM path has no memory interface, so this is only the ALSA-side ring | partial (documented) |

No `TODO`/`FIXME`/`XXX`/`stub`/`not implemented`/`#if 0` and no invented
register values in any touched file.

---

## 4. Compile test (commands and results)

Config (the shared recipe's `SOUND`/`SND` step is required — `nura.config` has
no sound symbols at all, and `SND_SOC_MT6735_MT6331` needs `SND_SOC_MT6331`):

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/rem-wcn-fm-i2s-audio
cp $M/nura.config .config
./scripts/config -e SOUND -e SND -e SND_SOC -e NVMEM \
  -m SND_SOC_MT6735 -m SND_SOC_MT6735_MT6331 -m SND_SOC_MT6331 \
  -m MTK_BTIF -m MTK_WCN_WMT -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT \
  -e VIDEO_DEV -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
```

`olddefconfig` exit 0; resulting symbols:
`CONFIG_SOUND=y CONFIG_SND=y CONFIG_SND_SOC=y CONFIG_SND_SOC_MT6735=m
CONFIG_SND_SOC_MT6735_MT6331=m CONFIG_SND_SOC_MT6331=m CONFIG_MTK_BTIF=m
CONFIG_MTK_WCN_WMT=m CONFIG_MTK_WCN_FM=m`
(log `$M/logs/rem-wcn-fm-i2s-audio-olddefconfig.log`). The canonical build
below is the `CONFIG_MTK_WCN_FM=m` one, i.e. with the FM link compiled in; the
link-out configuration is verified separately in §4.1.

Module build, `W=1` (log `$M/logs/rem-wcn-fm-i2s-audio-build.log`, exit 0):

```sh
make ARCH=arm64 LLVM=1 W=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  sound/soc/mediatek/mt6735/snd-soc-mt6735-afe.o \
  sound/soc/mediatek/mt6735/snd-soc-mt6735-mt6331.o
```

```
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-clk.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-hpimp.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-voice.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-i2s.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-mt6331.o
  LD [M]  sound/soc/mediatek/mt6735/snd-soc-mt6735-afe.o
  LD [M]  sound/soc/mediatek/mt6735/snd-soc-mt6735-mt6331.o
```

0 errors, **0 warnings** (`grep -c warning` → 0).

The acceptance list's per-object form was also run explicitly, `W=1`
(log `$M/logs/rem-wcn-fm-i2s-audio-objects.log`, exit 0):

```sh
make ARCH=arm64 LLVM=1 W=1 … -j2 \
  sound/soc/mediatek/mt6735/mt6735-afe-pcm.o \
  sound/soc/mediatek/mt6735/mt6735-afe-clk.o \
  sound/soc/mediatek/mt6735/mt6735-dai-pcm.o \
  sound/soc/mediatek/mt6735/mt6735-dai-i2s.o \
  sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.o \
  sound/soc/mediatek/mt6735/mt6735-dai-hpimp.o \
  sound/soc/mediatek/mt6735/mt6735-dai-voice.o \
  sound/soc/mediatek/mt6735/mt6735-mt6331.o
```

→ all eight `CC [M]` lines, 0 warnings, then both `LD [M]` lines.

`scripts/checkpatch.pl --no-tree -f mt6735-dai-fm-i2s.c`:
`total: 0 errors, 0 warnings, 395 lines checked`.

### 4.1 Conditional FM link (`CONFIG_MTK_WCN_FM`)

The FM link is compiled only when the FM driver is enabled (§5.2). Both
configurations were built and the presence/absence of the link verified on the
machine object and on the preprocessed source. `MTK_WCN_FM` needs
`MTK_BTIF`, `MTK_WCN_WMT`, `MEDIA_SUPPORT`, `VIDEO_DEV` to stay selected.

`CONFIG_MTK_WCN_FM=m` (`$M/logs/rem-wcn-fm-i2s-audio-build-fm_m.log`,
`…-preproc-fm_m.log`):

```sh
./scripts/config -m MTK_BTIF -m MTK_WCN_WMT -e MEDIA_SUPPORT \
  -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig          # CONFIG_MTK_WCN_FM=m
make … -W=1 snd-soc-mt6735-afe.o snd-soc-mt6735-mt6331.o
```

→ exit 0, 8× `CC [M]` + 2× `LD [M]`, **0 warnings**;

```
$ strings mt6735-mt6331.o | grep -E 'FM_I2S_RX_Playback|FM_I2S_Playback|mt6627-fm-i2s|mt-soc-fm-i2s-dai-codec|mt-soc-fmi2S-driver'
FM_I2S_RX_Playback
FM_I2S_Playback
mt-soc-fmi2S-driver
mt6627-fm-i2s
mt-soc-fm-i2s-dai-codec
```

`CONFIG_MTK_WCN_FM` unset (`…-build-fm_n.log`, `…-preproc-fm_n.log`):

```sh
./scripts/config -d MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig          # # CONFIG_MTK_WCN_FM is not set
make … -W=1 snd-soc-mt6735-afe.o snd-soc-mt6735-mt6331.o
```

→ exit 0, 8× `CC [M]` + 2× `LD [M]`, **0 warnings**;

```
$ strings mt6331.o | grep -E 'FM_I2S_RX_Playback|mt6627-fm-i2s|mt-soc-fmi2S-driver'
NONE (link compiled out)
$ strings mt6331.o | grep -E 'MultiMedia1|I2S0_PLayback|Voice_MD1|HP_IMPEDANCE'
MultiMedia1 / MultiMedia1_PLayback / MultiMedia1_Capture / I2S0_PLayback
HP_IMPEDANCE / HP_IMPEDANCE_Playback / Voice_MD1 / Voice_MD1_PLayback …
```

Preprocessed source (`make … mt6735-mt6331.i`, then removed):

| config | `grep -c FM_I2S_RX_Playback` | `grep -c mt-soc-fmi2S-driver` |
|---|---|---|
| `=m` | 1 | 1 |
| not set | 0 | 0 |

`mt6735_mt6331_dai_links[]` still exists in both; only the guarded entry and
its `SND_SOC_DAILINK_DEFS` disappear, so `ARRAY_SIZE()`/`num_links` stay
consistent.


These are object/module builds only, as required; no full kernel build and no
tree-wide modpost were run. No unresolved-symbol link failure occurred, because
the AFE/machine composites only reference in-tree ASoC/PMIC symbols that the
object build itself does not have to resolve (modpost runs only in the full
`modules` target).

---

## 5. Name binding, single-card rule, inferred and hardware-unverified items

### 5.1 Binding greps (verbatim)

```sh
$ grep -n 'MT6735_AFE_FM_I2S_DAI_NAME' sound/soc/mediatek/mt6735/mt6735-afe-common.h \
      sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.c
sound/soc/mediatek/mt6735/mt6735-afe-common.h:97:#define MT6735_AFE_FM_I2S_DAI_NAME	"mt-soc-fmi2S-driver"
sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.c:383:	.name = MT6735_AFE_FM_I2S_DAI_NAME,

$ grep -n 'COMP_CPU("mt-soc-fmi2S-driver")\|COMP_CODEC("mt6627-fm-i2s"\|"mt-soc-fm-i2s-dai-codec"\|\.name = "FM_I2S_RX_Playback"\|\.stream_name = "FM_I2S_Playback"' \
      sound/soc/mediatek/mt6735/mt6735-mt6331.c
133:	DAILINK_COMP_ARRAY(COMP_CPU("mt-soc-fmi2S-driver")),
134:	DAILINK_COMP_ARRAY(COMP_CODEC("mt6627-fm-i2s",
135:				      "mt-soc-fm-i2s-dai-codec")),
259:		.name = "FM_I2S_RX_Playback",
260:		.stream_name = "FM_I2S_Playback",
```

Peer (`$M/patches/rem-wcn-fm-i2s.patch`, read-only; the patch exists now):

```sh
$ grep -n 'MTK_WCN_FM_I2S_CODEC_NAME\|MTK_WCN_FM_I2S_DAI_NAME' $M/patches/rem-wcn-fm-i2s.patch
332:+#define MTK_WCN_FM_I2S_CODEC_NAME	"mt6627-fm-i2s"
333:+#define MTK_WCN_FM_I2S_DAI_NAME		"mt-soc-fm-i2s-dai-codec"
634:+	.name = MTK_WCN_FM_I2S_DAI_NAME,
699:+	.name = MTK_WCN_FM_I2S_CODEC_NAME,
```

So `dai_name` (`COMP_CPU`) == the AFE DAI's `.name` == `MT6735_AFE_FM_I2S_DAI_NAME`
== `"mt-soc-fmi2S-driver"`, and `codec_name`/`codec_dai_name` == the peer
component's `MTK_WCN_FM_I2S_CODEC_NAME`/`MTK_WCN_FM_I2S_DAI_NAME` — both strings
match `$M/REM_WCN_CONTEXT.md` §5 and the ROM strings (§2.1).

### 5.2 Single-card rule

ASoC binds a component to exactly one card: `soc_probe_component()`
(`sound/soc/soc-core.c:1602-1622`) returns 0 for the card the component is
already bound to and `-EBUSY` ("Trying to bind component ... already bound to
card ...") for any other card. Therefore the FM driver registers **no** card
(peer patch: `snd_soc_register_component()` only, no `snd_soc_register_card()`),
and the single `mt-snd-card` from `mt6735-mt6331.c` is the only card; its new
link is the only binding of the FM codec component. A second card sharing the
AFE could never bind.

Integration consequence and the guard: the card's FM link depends on the codec
component, and `soc_bind_dai_link()` returns `-EPROBE_DEFER`
(`soc-core.c:1036-1038`) while `mt6627-fm-i2s` is not registered. Because the
FM component is an optional module (`CONFIG_MTK_WCN_FM`, which itself depends on
`MTK_WCN_WMT`/`MTK_BTIF`/`MEDIA_SUPPORT`/`VIDEO_DEV`), an unconditional link
would defer `mt-snd-card` forever in a `CONFIG_MTK_WCN_FM=n` build and take the
base audio links down with it. Downstream can always list the link because its
FM codec is part of the always-built codec driver (`mt_soc_machine.c:1068-1076`);
here both the `SND_SOC_DAILINK_DEFS(fm_i2s, …)` block and the entry in
`mt6735_mt6331_dai_links[]` are therefore wrapped in
`#if IS_ENABLED(CONFIG_MTK_WCN_FM)` … `#endif` (the AFE DAI is not guarded; an
unused back-end DAI is harmless):

* `CONFIG_MTK_WCN_FM=n` → the card has its ten base links only and binds
  normally; no FM PCM.
* `CONFIG_MTK_WCN_FM=m` and the module loaded → the FM link is present and is
  the codec component's only binding.

`ARRAY_SIZE(mt6735_mt6331_dai_links)` and `card.num_links` follow the guard
automatically.

### 5.3 ASRC decision and exact missing declarations

`mtk_pcm_fm_i2s_prepare()` only uses the ASRC to convert the fixed 32 kHz 2nd
I2S input to the ALSA stream rate (44.1/48 kHz); with `hw_params` fixed at
32 kHz the DAC-out rate (programmed by `mt6735_afe_dac_prepare()`) is also
32 kHz, so a conversion would be a no-op. `SetI2SASRCConfig(false, 0)` — the
same call the downstream `close()` uses (`mt_soc_pcm_fm_i2s.c:238`) — is
programmed explicitly (`AFE_CONN4` bit 30 = 1), so the ASRC block is out of the
path rather than left in an undefined state.

The 32 kHz→44.1/48 kHz sequence is fully documented and would only need these
additional register declarations in `mt6735-afe-regs.h` if the 32 kHz
restriction is ever lifted: `AFE_ASRC_CON13` 0x550, `AFE_ASRC_CON14` 0x554,
`AFE_ASRC_CON15` 0x558, `AFE_ASRC_CON16` 0x55c, `AFE_ASRC_CON17` 0x560,
`AFE_ASRC_CON20` 0x56c (`AudDrv_Afe.h:675-682`), with the coefficient writes of
`mt_soc_afe_control.c:2257-2271` and the enable of `:2281-2289`. **They are not
missing from the hardware description** — they are simply not needed at 32 kHz,
so no value was guessed and none was added.

### 5.4 Inferred / hardware-unverified

* `[INFERENCE]` The ASRC-bypass path at equal sample rates (32 kHz in, 32 kHz
  out) is correct: the ASRC is a rate converter and `AFE_CONN4[30] = 1` is the
  downstream bypass. Not exercised on silicon.
* `[INFERENCE]` `AFE_GAIN1_CONN`/`AFE_GAIN1_CONN2` are the registers behind
  `mConnectionReg[I00/I01/I10/I11][…]`; the offsets 0x420/0x448 come from
  `AudDrv_Afe.h:622,632`, the bits from `mConnectionbits[]`. Never read back on
  hardware.
* `[INFERENCE]` The FM path's audio actually leaves the SoC through the PMIC
  codec DAC (`I2S_OUT_DAC` → `AFE_I2S_CON1`), i.e. FM audio is audible on the
  headset/speaker, because that is the only consumer of the gain-1 outputs that
  this port programs (`O03/O04` are the PMIC DAC inputs,
  `mConnectionReg[I05][O03]`).
* Unverified at runtime: the ALSA pointer for this memif-less stream never
  advances (downstream behaves the same, `mtk_pcm_fm_i2s_pointer()` returns a
  private counter); userspace must not rely on `snd_pcm_avail()` for pacing.
* Unverified at runtime: PMIC pin mux (`audpmicclk-mode0/mode1`) and I2S clock
  gate (`aud_i2s_clk`) being sufficient for the 2nd I2S input; the DTS in this
  tree does not yet provide the audio pinctrl states (the AFE treats pinctrl as
  optional, `mt6735-afe-clk.c:119-137`).
* Unverified at runtime: the `-EBUSY` matrix (I2S0 ↔ FM, voice ↔ FM) and the
  shared-DAC ownership logic; no concurrent streams were exercised.
* Not verified on hardware: whether the MT6627 really drives BCLK/LRCK (peer
  claim from `mt6627_fm_config.c:116-118`) matches `AFE_I2S_CON` slave mode; the
  link `dai_fmt` is the frozen integration value and this DAI derives no
  register from it (it has no `set_fmt`; every format bit is fixed by
  `Set2ndI2SIn()`), so the register side is exactly the documented slave setup.
* Known limitation (outside this patch's file scope): the voice DAIs can still
  start during an active FM stream and would reprogram the shared PMIC DAC
  (`mt6735-dai-voice.c`, not in this leaf's file list). Userspace must stop the
  FM stream before a call, which is what the vendor FM HAL does.

---

## 6. DTS fragment

**None.** No `.dts`/`.dtsi` was modified. The AFE node
(`mediatek,mt6735-afe`) and the machine node (`mediatek,mt6735-sound`) already
exist, and the new DAI/link need no new properties. The FM codec provider is the
WCN FM platform device (`mediatek,mt_mt6627_fmradio`) and its DT node belongs to
the WCN series/coordinator fragment, not here.

```
DTS-FRAGMENT: none
```

---

## 7. AUDIO-FRAGMENT (ordering and split)

This patch is the **audio-side fragment**, not part of the pure WCN series:

* It touches only `sound/soc/mediatek/mt6735/` and nothing under `drivers/`.
* Apply order: `audio-integration.patch` (+ audio gap patches) →
  `rem-wcn-fm-i2s-audio.patch`; at source level it also needs
  `rem-wcn-fm-i2s.patch` (codec component/Dai names) to be present for the card
  to bind, but the two patches do not overlap paths (WCN patch: `drivers/misc/
  mediatek/connectivity/fm/`, `include/uapi/linux/mtk_fm_i2s.h`,
  `tools/mediatek/wcn/fm_i2s/`; this patch: `sound/soc/mediatek/mt6735/`).
* Coordinator ordering: `parity-baseline-all` → `rem-wcn-wmt` →
  `rem-wcn-wlan` → `rem-wcn-fm-i2s` → **`rem-wcn-fm-i2s-audio`**, and the audio
  series lists this file under `AUDIO-FRAGMENT`.

---

## 8. Patch

`$M/patches/rem-wcn-fm-i2s-audio.patch`

```
 sound/soc/mediatek/mt6735/Makefile            |   3 +-
 sound/soc/mediatek/mt6735/mt6735-afe-common.h |  41 +++
 sound/soc/mediatek/mt6735/mt6735-afe-pcm.c    |  22 +-
 sound/soc/mediatek/mt6735/mt6735-afe-regs.h   |  45 ++-
 sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.c | 395 ++++++++++++++++++++++++++
 sound/soc/mediatek/mt6735/mt6735-dai-i2s.c    |  34 ++-
 sound/soc/mediatek/mt6735/mt6735-mt6331.c     |  37 ++
 7 files changed, 564 insertions(+), 13 deletions(-)
```

`grep -c 'GIT binary patch'` → **0**; no `.o`/`.ko`/`.cmd`/`modules.order`/
`Module.symvers`/`.mod.c`/`.config`/`include/config/` hunk; no DTS, no
`drivers/`, no Kconfig change. Generated with
`git add -N sound/soc/mediatek/mt6735/mt6735-dai-fm-i2s.c && git diff HEAD --binary`.
