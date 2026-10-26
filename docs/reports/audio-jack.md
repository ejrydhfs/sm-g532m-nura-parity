# audio-jack — MT6735 machine driver + headset/jack integration

Status: **done** (source parity for the implemented paths), compile-tested, hardware unverified.
Scope: Nura `v7.0.10-mt6735`, worktree `$M/worktrees/audio-jack`.

## 1. Files changed

| File | Change |
|---|---|
| `sound/soc/mediatek/mt6735/mt6735-mt6331.c` | **new** — MT6735/MT6331 machine driver, 4 FE links, ASoC jack + ACCDET notifier |
| `sound/soc/mediatek/mt6735/Makefile` | **new** — `snd-soc-mt6735-mt6331-y := mt6735-mt6331.o`, `obj-$(CONFIG_SND_SOC_MT6735_MT6331) += snd-soc-mt6735-mt6331.o` |
| `sound/soc/mediatek/Kconfig` | **+12** — `config SND_SOC_MT6735_MT6331`, `depends on SND_SOC_MT6735` |
| `sound/soc/mediatek/Makefile` | **+1** — `obj-$(CONFIG_SND_SOC_MT6735_MT6331) += mt6735/` |

Nothing else was touched. `include/linux/mfd/mt6328/accdet.h` stays untracked and is not in the patch.
`arch/arm64/boot/dts/**` untouched (fragment in §6).

## 2. Evidence

Downstream (primary):
- `sound/soc/mediatek/mt_soc_audio_v3/mt_soc_machine.c`
  - DAI link table `mt_soc_dai_common[]`: MultiMedia1 at 857 (DL1 CPU / codec TX DAI), MultiMedia2 at 867 (UL1 CPU / codec RX DAI), I2S0OUTPUT at 907 (I2S0 CPU / dummy codec), MultiMedia_DL2 at 1099 (DL2 CPU / codec TX DAI2).
  - Card: `.name = "mt-snd-card"` at 1139; `snd_soc_card_mt` at 1138.
  - `mt_soc_audio_init()` at 167 is empty except `pr_debug`; `mt_machine_audio_ops` at 136 has no-op `startup`/`prepare`. So the ROM sets **no** machine DAI format and **no** PCM constraints per link.
  - `mt_soc_controls` / `mt6735_set_lowjitter` (~1114-1134): "I2S low Jitter function" machine switch — deferred (Android tuning path, not required for playback).
- `AudDrv_Def.h`: CPU DAI names 204-219, codec DAI names 273-275, codec name 295, stream names 311/313/321/329, dummy codec name 296.
- `mt_soc_pcm_dl1_i2s0.c:183-193`: AFE writes `AFE_I2S_CON`/`AFE_I2S_CON3` with `Soc_Aud_I2S_FORMAT_I2S`, `Soc_Aud_INV_LRCK_NO_INVERSE`, `Soc_Aud_LR_SWAP_NO_SWAP`, `Soc_Aud_I2S_WLEN_WLEN_32BITS`, clock master.
- `mt_soc_digital_type.h:236-262`: `Soc_Aud_LR_SWAP_NO_SWAP = 0`, `Soc_Aud_INV_LRCK_NO_INVERSE = 0`, `Soc_Aud_I2S_FORMAT_I2S = 1`, `Soc_Aud_I2S_SRC_MASTER_MODE = 0` → SND_SOC_DAIFMT_I2S | NB_NF | CBC_CFC.
- ACCDET driver interface `drivers/misc/mediatek/accdet/mt6735/accdet.c:712-745` four-key bands ordered by increasing mic ADC voltage; `746-769` keycodes `KEY_PLAYPAUSE`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`, `KEY_VOICECOMMAND`; `941-975` cable types `HEADSET_NO_MIC` / `HEADSET_MIC`; `1835-1855` its own input device (the duplicate reporting path deliberately not reproduced).
- Device-tree: `$M/zip/rom_dtb_0.dts:3987` codec node, `4024-4042` accdet node (`mediatek,mt6735-accdet`, 3/4-key thresholds). No sound-card node in the ROM DTB: downstream created the card as `platform_device_alloc("soc-audio")`.

Nura/ASoC:
- Link API: `SND_SOC_DAILINK_DEFS` + `SND_SOC_DAILINK_REG` + `COMP_CPU/COMP_CODEC/COMP_EMPTY` (`include/sound/soc.h:912-940`), pattern checked against `sound/soc/mediatek/mt8189/mt8189-nau8825.c`.
- `sound/soc/soc-jack.c:38-88` `snd_soc_jack_report()` semantics; `sound/soc/soc-core.c:1459-1510` `snd_soc_runtime_set_dai_fmt()` (skips on `dai_fmt == 0`, tolerates `-ENOTSUPP`); `soc-dapm.c:2900-2940` pin lookup.
- Codec jack/keys pattern: `sound/soc/codecs/mt6359-accdet.c:909-912`.
- ACCDET client API: `include/linux/mfd/mt6328/accdet.h` (final version, untracked, delivered by the accdet patch): `accdet_register_notifier()`, `accdet_unregister_notifier()`, `accdet_get_state()`, `enum accdet_cable_type`, `enum accdet_button` (MEDIA=1, VOICE=2, VOLUP=3, VOLDOWN=4, ordered by increasing mic-ADC voltage).

## 3. Parity matrix

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| Card name | `mt-snd-card` (mt_soc_machine.c:1139) | absent | `mt-snd-card` | done |
| Card registration | `platform_device_alloc("soc-audio")` + `platform_set_drvdata` | absent | platform_driver, of_match `mediatek,mt6735-sound` | done |
| Link MultiMedia1 | DL1 CPU ↔ codec TX DAI, playback | absent | names per contract, `playback_only` | done |
| Link MultiMedia2 | UL1 CPU ↔ codec RX DAI, capture | absent | names per contract, `capture_only` | done |
| Link MultiMedia_DL2 | DL2 CPU ↔ codec TX DAI2, playback | absent | DL2 CPU ↔ contract's `mt-soc-codec-tx-dai`, `playback_only` | done, DAI reuse noted |
| Link I2S0OUTPUT | I2S0 CPU ↔ dummy codec | absent | I2S0 CPU ↔ `snd-soc-dummy`/`snd-soc-dummy-dai`, bidirectional | done |
| Link stream names | `MultiMedia1_PLayback`, `MultiMedia1_Capture`, `MultiMedia2_PLayback`, `I2S0_PLayback` | — | same strings | done |
| DAI formats | none for MM links; I2S programmed inside AFE | — | none for MM links; `I2S|NB_NF|CBC_CFC` on I2S0 | done |
| ASoC jack | none in machine driver; ACCDET had its own input device | none | `snd_soc_card_jack_new_pins` "Headset Jack", pins `Headphone`/`Headset Mic`, BTN_0..3, `snd_jack_set_key` | done |
| Codec jack hook | n/a | none | `snd_soc_component_set_jack(rtd0 codec, jack, NULL)`; `-ENOTSUPP` treated as "codec has no jack", card still works | done |
| Plug reporting | cable type 1/2 (4-pole/3-pole) | none | `ACCDET_CABLE_HEADPHONE → SND_JACK_HEADPHONE`, `HEADSET`/`HEADSET_5POLE → SND_JACK_HEADSET`, plug-out clears headset+buttons | done |
| Key reporting | own input device, hold semantics via `cur_key` | none | press: `snd_soc_jack_report(BTN_x, BTN_MASK)`; release (`button == 0`): clears the remembered pressed bit; single input device | done |
| Boot-time jack state | ACCDET detects at probe and skips the first notification | none | `accdet_get_state()` seeded in card probe and retried in MultiMedia1 init, replayed to the jack | done |
| Duplicate input device | yes (accdet `kpd_accdet_dev`) | n/a | deliberately not registered; ASoC jack supplies the input device | done |
| MultiMedia1 init fmt/constraints | empty init, no constraints | — | no fmt/constraints added (ROM does none); init used only for jack wiring | done |
| Machine DAPM | none | — | only two jack-anchor pins (`SND_SOC_DAPM_HP("Headphone")`, `SND_SOC_DAPM_MIC("Headset Mic")`), **no routes**, no codec routing duplicated | done |
| I2S low-jitter switch | `SOC_ENUM_EXT` machine control | — | not ported | deferred |
| Voice_MD1/MD2 (+BT), HDMI_OUT, FM_I2S/FM_MRGTX, TDM, AWB (MRGRX/DL1/I2S0), ULDLOOPBACK, OFFLOAD_GDMA, HP_IMPEDANCE, PLATOFRM_CONTROL, MultiMedia3 | full link set in ROM | — | not present | deferred |
| Debugfs/proc/ioctl HAL-facing controls | `mtksocaudio`, `mtksocanaaudio`, param parser | — | not ported (no HAL in scope) | deferred |

Notes:
- ROM uses a second codec playback DAI (`mt-soc-codec-tx-dai2`) for DL2. The fixed interface contract exposes only `mt-soc-codec-tx-dai`, so DL2 shares it. Consequence: MM1 and DL2 cannot stream simultaneously through the codec DAI (ASoC serialises DAIs); documented as an interface limitation, not a bug.
- Playback/capture `_only` flags are set from the codec DAI direction; the ROM had no flags because each FE link was opened in one direction by the HAL.

## 4. Compile test

Toolchain: `$M/llvmpkg/usr/bin` + `$M/bin` prepended, clang 23.

Commands (exactly as assigned):

```sh
cd $M/worktrees/audio-jack
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
./scripts/config --enable SOUND --enable SND --module SND_SOC --module SND_SOC_MT6735_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 sound/soc/mediatek/mt6735/mt6735-mt6331.o
```

Result: **success**, `CC sound/soc/mediatek/mt6735/mt6735-mt6331.o`, exit 0, object produced (233 KB).
Clean rebuild with `W=1` (`rm` object first): exit 0, **no warnings, no errors**.

Both ACCDET header branches are compiled cleanly:
- `CONFIG_MTK_ACCDET` unset (this tree): the inline stubs are used, object has no external ACCDET references.
- `CONFIG_MTK_ACCDET=1` forced via `KCFLAGS` (the accdet patch's real config, not present here): exit 0, no warnings, and `llvm-nm` shows `U accdet_get_state`, `U accdet_register_notifier`, `U accdet_unregister_notifier`.

Caveat recorded for the integration owner: `CONFIG_SND_SOC_MT6735` does not exist in this worktree yet (it belongs to the AFE patch), so `olddefconfig` drops `CONFIG_SND_SOC_MT6735_MT6331` from `.config`. kbuild's single-target path still descends into `mt6735/` and compiles the object, which is why the command above passes. I additionally verified the module wiring with the symbol forced on the make command line (`CONFIG_SND_SOC_MT6735_MT6331=m`, compiled as `CC [M]`); once the AFE patch adds `SND_SOC_MT6735`, the normal config path enables the module with no further changes.

## 5. Inferred / unverified

- **Button mapping** uses the contract names from the final `accdet.h` (`ACCDET_BUTTON_MEDIA/VOICE/VOLUP/VOLDOWN` → BTN_0..BTN_3 → `KEY_PLAYPAUSE`, `KEY_VOICECOMMAND`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`), confirmed with the ACCDET driver owner and aligned with `accdet.c:712-769`. Physical band-to-key behaviour remains hardware-unverified.
- **5-pole cable → `SND_JACK_HEADSET`** is `[INFERENCE]` (electrically a 4-pole cable with an extra ground ring); the ROM only ever distinguishes 3-pole vs 4-pole.
- **`ACCDET_CABLE_UNKNOWN` with `plugged = true` reports nothing** (defensive); not exercised by downstream and not verified.
- **Codec `set_jack`**: the codec DAI is implemented by another engineer; if it has no `set_jack` hook the driver warns and still reports through the ASoC jack (ACCDET-driven). Runtime path unverified.
- **Jack DAPM pins** are card-level anchors for `snd_soc_jack_report()`; if the codec also defines widgets with the same names, ASoC resolves the card pin first (same-context preference). ALSA control naming/behaviour unverified without hardware.
- **Boot-time jack state** uses `accdet_get_state()`, which returns `-ENODEV` until the ACCDET driver has probed (and when `CONFIG_MTK_ACCDET=n`). The query is retried in `mm1_init()`; if ACCDET still is not up at card-bind time, the state is learned from the first notification.
- **I2S word length**: downstream programs 32-bit WLEN in `AFE_I2S_CON3`; no machine-level constraint is added, the AFE DAI's `hw_params` owns it. Link-level fmt only carries I2S/NB_NF/CBC_CFC.
- All register programming, clocking, jack-detect timing and key debounce are **hardware-unverified** (no phone attached).

## 6. DTS-FRAGMENT

Names-only links suffice: the codec (`mt-soc-codec`), the AFE CPU DAIs and the dummy codec are matched by component/DAI name, and ACCDET is a separate platform device. Only the card's own device node is required.

```dts
/ {
	sound {
		compatible = "mediatek,mt6735-sound";
		status = "okay";
	};
};
```

Applied by the integration owner (I did not edit shared DTS). The codec and ACCDET nodes come from their own patches; ROM reference for ACCDET is `rom_dtb_0.dts:4024-4042` (`mediatek,mt6735-accdet`, headset 3/4-key thresholds, mic bias/debounce properties) in case the integration DTS needs those values.

## 7. Patch

`$M/patches/audio-jack.patch` (`git diff --binary`, 4 files, 485 lines; new files via `git add -N`, nothing committed, `accdet.h` excluded).
