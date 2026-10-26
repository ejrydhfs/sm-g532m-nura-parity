# Audio gap: external speaker amplifier enable (AW8736 class, AP GPIO117)

Worktree: `$M/worktrees/audio-gap-extamp` (baseline `audio-integration.patch` applied and staged)
Tree: `src/nura-linux` @ 046883c0c, arm64, MT6737T Samsung SM-G532M "grandpplte"

Citations are relative to `src/downstream-grandpplte` unless stated otherwise:

* `vendor:LINE` = `sound/soc/mediatek/mt_soc_audio_v3/mt_soc_codec_63xx.c`
* `gpio:LINE`   = `sound/soc/mediatek/mt_soc_audio_v3/AudDrv_Gpio.c`
* `rom_dtb_0.dts:LINE` = `$M/zip/rom_dtb_0.dts` (decompiled ROM DT)
* `soc-dapm.c:LINE` = the target tree's `sound/soc/soc-dapm.c`

---

## 1. Files changed

| File | Change |
|---|---|
| `sound/soc/codecs/mt6331.c` | +148 lines: `ext_amp` GPIO descriptor + probe lookup, `mt6331_ext_amp_enable()/_disable()`, `mt6331_ext_amp_event()` DAPM handler, `"Ext Spk Amp"` endpoint widget + route, vendor-compatible `"Ext Speaker Amp"` control |
| `sound/soc/codecs/mt6331.h` | unchanged (no new register/bit; the amplifier has no register interface in any available source) |

No shared DTS/DTSI file was touched. No other file in the worktree was modified
(`git status` shows only `sound/soc/codecs/mt6331.c`).

---

## 2. Evidence

### 2.1 Pin and its idle state

| Fact | Source |
|---|---|
| Pin is `pin_extspkamp`, parsed from DT via `GetGPIO_Info()` | `vendor:143-145`, `vendor:2182` |
| GPIO117, mode 0 (GPIO), pull enable, direction out, output **low = disabled** | `vendor:2195-2198` (legacy branch), `vendor:2206` (pinctrl "extamp-pulllow") |
| Board DT property `extspkamp-gpio = <0x75 0x00>` (0x75 = 117, mode 0) | `rom_dtb_0.dts:3561` |
| Pinctrl states `extamp-pullhigh`/`extamp-pulllow` declared | `rom_dtb_0.dts:3566` |
| Those two ROM pinctrl states are **empty** (no `pins`/pinconf cells) | `rom_dtb_0.dts:2405-2413` |
| The ROM audio `default` state does not configure pin 117 either | `rom_dtb_0.dts:2343-2369` |
| MT6735 pinmux binding for the pin: mode 0 = GPIO | `include/dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h:759` |
| ROM has the amplifier control symbols | `zip/symbols.txt:33397` (`Ext_Speaker_Amp_Get`), `:33417` (`Ext_Speaker_Amp_Set`) |

Consequence: the pinctrl-state path (`AudDrv_GPIO_EXTAMP_Select()`,
`gpio:261-280`) selects empty states on this ROM and cannot change the pin, so the
shipped kernel must have used the `CONFIG_MTK_LEGACY` `mt_set_gpio_*` path —
marked **[INFERENCE]** because `arch/arm64/configs/defconfig` in the GPL drop does
not contain `CONFIG_MTK_LEGACY`, the option is `bool ... default n`
(`drivers/misc/mediatek/Kconfig:16-19`), and Samsung/MTK project configs are not
part of the drop. Both branches execute the same delays (see 2.3), so the
implemented behaviour is unaffected.

### 2.2 Enable/disable sequence

`Ext_Speaker_Amp_Change()` (`vendor:2175-2265`), enable:

```
vendor:2198 / 2206   pin low            (mt_set_gpio_out(0) / pinctrl "extamp-pulllow")
vendor:2211          usleep_range(1*1000, 20*1000)      /* 1..20 ms settle */
vendor:2220 / 2225   pin high           (mt_set_gpio_out(1) / pinctrl "extamp-pullhigh")
vendor:2228          msleep(SPK_WARM_UP_TIME)           /* SPK_WARM_UP_TIME = 25 ms, vendor:2177 */
```

disable:

```
vendor:2245-2246 / 2252  pin low
vendor:2255              udelay(500)                    /* 500 us settle */
```

`AW8736_MODE3` / `AW8736_MODE4` (the 2 us triple-toggle enable signature,
`vendor:2160-2171`, `vendor:2273-2321`) are gated by `CONFIG_MTK_LEGACY` **and**
`AW8736_MODE_CTRL`, which is not defined anywhere in the tree; the plain
`mt_set_gpio_out(pin, 1)` path is the compiled one (`vendor:2217-2221`).

### 2.3 Delays are vendor delays, not invented

All three delays (`usleep_range(1000, 20000)` at `vendor:2211`, `msleep(25)` at
`vendor:2228`, `udelay(500)` at `vendor:2255`) sit **outside** the
`#if defined(CONFIG_MTK_LEGACY)` blocks, so they are compiled in both the legacy
and the pinctrl build. The driver reproduces exactly these three values and adds
no delay of its own.

### 2.4 Is the pin a userspace control in the vendor kernel?

Yes. `Ext_Speaker_Amp_Change()` has exactly **one** call site: the control
handler `Ext_Speaker_Amp_Set()` (`vendor:2373-2387`). The control is exported in
`mt6331_snd_controls[]` as `"Ext_Speaker_Amp_Switch"` (`vendor:3165-3166`,
enum `Audio_DL_Enum[11]` = `amp_function`, `vendor:3136-3150`, text table
`vendor:2842-2843`). Nothing in the vendor kernel touches the pin from
`Speaker_Amp_Change()` (`vendor:2030-2126`) or `SetAnalogSuspend()`
(`vendor:215-272`), so the *ordering* of the external amp relative to
`"Speaker_Amp_Switch"` (`vendor:3156`) is decided by the closed-source audio HAL.
This is stated as unverifiable below.

`Ext_Speaker_Amp_Get()` returns the driver's bookkeeping bit
`mAudio_Ana_DevicePower[AUDIO_ANALOG_DEVICE_OUT_EXTSPKAMP]` (`vendor:2365-2371`,
enum `mt_soc_analog_type.h:114`), not the pin level.

### 2.5 DAPM ordering facts used to place the widget

* `dapm_up_seq[]`: `snd_soc_dapm_supply` = 3, `snd_soc_dapm_dac` = 8,
  `snd_soc_dapm_out_drv` = 12, `snd_soc_dapm_spk` = 13 — `soc-dapm.c:74-113`
* `dapm_down_seq[]`: `snd_soc_dapm_spk` = 4, `snd_soc_dapm_out_drv` = 5,
  `snd_soc_dapm_dac` = 8, `snd_soc_dapm_supply` = 14 — `soc-dapm.c:114-153`
* Widget ids in the existing driver: `"DAC"` = `snd_soc_dapm_dac`,
  `"Speaker Amp"` = `snd_soc_dapm_out_drv` (`mt6331.c`, DAPM widget table).

---

## 3. What was implemented and why

Widget added (endpoint, `snd_soc_dapm_spk`, same power slots as a speaker output):

```c
SND_SOC_DAPM_SPK("Ext Spk Amp", mt6331_ext_amp_event),
...
{ "Ext Spk Amp", NULL, "Speaker Amp" },     /* parallel with the existing
                                               { "SPEAKER", NULL, "Speaker Amp" } */
```

`mt6331_ext_amp_event()` runs `mt6331_ext_amp_enable()` on `POST_PMU` and
`mt6331_ext_amp_disable()` on `PRE_PMD` (`SND_SOC_DAPM_SPK()` supplies exactly
those two flags, `include/sound/soc-dapm.h:78-82`).

**Why not `SND_SOC_DAPM_SUPPLY`, as the task suggested.** A supply is
`up_seq 3` / `down_seq 14` (`soc-dapm.c:74-153`), i.e. its `PRE_PMU` runs
*before* the DAC (`up_seq 8`) and before the internal Speaker Amp
(`out_drv`, `up_seq 12`), and its `POST_PMD` runs *after* the internal amplifier
has already been powered down. That is the inverse of what the task asks for
("assert the amp enable only when the speaker output is actually powered") and
it is the wrong anti-pop order: the external amplifier would be live while the
PMIC speaker driver ramps. `subseq` cannot fix it — `SND_SOC_DAPM_SUPPLY_S()`
only sorts within the supply group (`soc-dapm.c:1781-1804`). Using
`snd_soc_dapm_spk` (up 13 / down 4) gives:

* power-up: `DAC` (8) → area `Speaker Amp` out_drv (12) → `Ext Spk Amp` spk (13):
  `POST_PMU` = pin high **after** the internal amp sequence ran;
* power-down: `Ext Spk Amp` spk (4) runs before `Speaker Amp` out_drv (5):
  `PRE_PMD` = pin low **before** the internal amp is shut down, i.e. the
  amplifier is muted first and only then is the PMIC output ramped away.

**[INFERENCE]** "enable last, disable first" is the safe order for this pin; it
matches the intent of the vendor state machine (pin low during the wake-up
settle, `vendor:2206-2228`). The vendor's actual HAL ordering is not observable
(§2.4).

The widget is a separate endpoint fed from `"Speaker Amp"` rather than an
in-line element so that the existing `"SPEAKER"` route is untouched and the
amplifier is gated on exactly the same condition as the internal speaker
amplifier (endpoint `connected = 1` by default, `soc-dapm.c:3886`; powered only
when the input path from `"DAC"` is complete). There is no output mux in this
codec, so every downlink stream powers the speaker chain — a pre-existing
property of the baseline driver (the internal `"Speaker Amp"` already behaves
this way), not something this patch introduces.

### Suspend / resume

No new handler was added. `snd_soc_suspend()` suspends the PCMs first
(`sound/soc/soc-core.c:682`) and then synchronises DAPM
(`soc-core.c:690-694`), so the widget is powered down and `PRE_PMD` deasserts the
pin *before* `mt6331_codec_suspend()` runs. This matches the vendor driver,
which also leaves `AUDIO_ANALOG_DEVICE_OUT_EXTSPKAMP` alone in
`SetAnalogSuspend()` (`vendor:215-272`). Resume likewise re-asserts the pin from
DAPM (stream restart), not from `mt6331_codec_resume()`.

---

## 4. Parity matrix

| Behaviour | ROM implementation | before this patch | after this patch | state |
|---|---|---|---|---|
| Pin configuration at boot | GPIO117, mode 0, pull enable, output, **low** = disabled (`vendor:2195-2198`; `rom_dtb_0.dts:3561`) | pin never requested; nobody drives it | `devm_gpiod_get_optional(dev, "ext-amp", GPIOD_OUT_LOW)` in probe → requested as output, driven **low** before the codec registers; `"Ext Spk Amp"` control reads 0 | done |
| Board without the amplifier | not applicable (vendor always parses the DT pin, `vendor:2182-2186`) | — | property missing → `priv->ext_amp == NULL`, probe still succeeds, DAPM event is a no-op, control reports `0` and `put(1)` returns `-ENODEV` (no fake success) | done |
| Amplifier ON sequence | low → 1..20 ms → high → 25 ms (`vendor:2198/2206,2211,2220/2225,2228`) | never enabled | `POST_PMU`: `gpiod = 0`, `usleep_range(1000,20000)`, `gpiod = 1`, `msleep(25)` | done |
| Amplifier OFF sequence | low → 500 us (`vendor:2245/2252,2255`) | never disabled | `PRE_PMD`: `gpiod = 0`, `udelay(500)` | done |
| Ordering vs. internal speaker amp | not in kernel; HAL writes `Ext_Speaker_Amp_Switch` (`vendor:3165`) and `Speaker_Amp_Switch` (`vendor:3156`) | — | automatic: enable after `"Speaker Amp"` `PRE_PMU` (spk up_seq 13 > out_drv 12), disable before `"Speaker Amp"` `PRE_PMD` (spk down_seq 4 < out_drv 5) | done (ordering per §3, HAL order unverifiable) |
| `Ext_Speaker_Amp_Switch` userspace control | `SOC_ENUM_EXT` "Ext_Speaker_Amp_Switch" (`vendor:3165`), get = bookkeeping bit, set = pin (`vendor:2365-2387`) | absent | `SOC_SINGLE_BOOL_EXT("Ext Speaker Amp")`, get = **actual pin level**, set = same enable/disable sequence incl. delays | done (name follows this driver's existing control naming, e.g. `"Speaker PGA Gain"` vs vendor `"Speaker_PGA_GAIN"`) |
| Amplifier state across system suspend | untouched by `SetAnalogSuspend()` (`vendor:215-272`) | — | DAPM powers the endpoint down before `.suspend` (`soc-core.c:682,690-694`), so the pin is low during suspend | done |
| AW8736 MODE3/MODE4 2 us triple-toggle | `vendor:2160-2171,2273-2321`, behind `CONFIG_MTK_LEGACY` + undefined `AW8736_MODE_CTRL` | — | not implemented; those macros are not compiled in the vendor build either | deferred (not compiled in any source) |
| Second ext-amp pin (`pin_extspkamp_2`) | `GetGPIO_Info(10, …)`, `NULL_PIN_DEFINITION` when absent (`vendor:2193-2203,2243-2250`) | — | not implemented: the vendor DT has no second `extspkamp2-gpio` property, so the pin is always `-1` and every access is skipped | deferred (dead on this board) |

---

## 5. Userspace routing helper: NOT required

Proof of the DAPM path (all inside the codec component, no card/machine changes
needed):

```
card DAI link (mt6735-mt6331.c)  →  codec DAI "mt-soc-codec-tx-dai"
   stream "MultiMedia1_PLayback"
        |
   AIF_RX  (snd_soc_dapm_aif_in)      ← becomes active on PCM start
                                        (AIF_RX2 for the "MultiMedia_DL2" link,
                                         same "DAC" input)
        |  { "DAC", NULL, "AIF_RX" }
   DAC     (snd_soc_dapm_dac,  up 8)   PRE_PMU: mt6331_dl_power_up()
        |  { "Speaker Amp", NULL, "DAC" }
   Speaker Amp (out_drv,      up 12)   PRE_PMU: mt6331_speaker_power_up()
        |  { "Ext Spk Amp", NULL, "Speaker Amp" }
   Ext Spk Amp (spk,          up 13)   POST_PMU: mt6331_ext_amp_enable()  → GPIO117 high
        |  (also) { "SPEAKER", NULL, "Speaker Amp" }  ← unchanged
   SPEAKER  (snd_soc_dapm_output)
```

Starting a downlink PCM activates `AIF_RX`, which powers the whole chain from
the DAC to the endpoint; stopping it tears the chain down in reverse, so the
amplifier is enabled and disabled purely from the DAPM graph plus the
`ext-amp-gpios` property in the board DT. No `tinymix`, AudioParam or other
routing helper is needed, and no helper was added under `tools/mediatek/audio/`.

The `"Ext Speaker Amp"` control exists only for vendor-HAL compatibility
(§2.4). It is not part of the routing path, and userspace does not have to write
it: a write of 1 is idempotent with the DAPM enable, and a write of 0 only
forces the pin low until DAPM next powers the widget (documented behaviour, same
unsynchronised ownership the vendor driver itself has between the HAL switch and
its power bookkeeping).

---

## 6. Exact compile commands and result

```
cd $M/worktrees/audio-gap-extamp
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp $M/nura.config .config
./scripts/config --enable SOUND --module SND --module SND_SOC \
                 --module SND_SOC_MEDIATEK --module SND_SOC_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 sound/soc/codecs/mt6331.o
```

Result:

```
  CC [M]  sound/soc/codecs/mt6331.o
```

(clean, no warnings). `CONFIG_SND=m`, `CONFIG_SND_SOC=m`,
`CONFIG_SND_SOC_MT6331=m` confirmed in the resulting `.config`.

Also attempted `make ARCH=arm64 LLVM=1 -j2 sound/soc/codecs/snd-soc-mt6331.ko`:
modpost reports the **whole kernel's** exports as missing
(`__platform_driver_register`, `devm_gpiod_get_optional`, `mutex_lock`, …)
because this is a single-module build without a built `vmlinux`/`Module.symvers`;
it is not a symbol used by this patch going unresolved (`devm_gpiod_get_optional`
is a standard exported symbol, and the same modpost failure appears without the
patch). Compile-only acceptance is the `.o`.

---

## 7. Inferred / unverified items

1. **[INFERENCE]** The shipped vendor kernel used `CONFIG_MTK_LEGACY` for this
   pin (the ROM's `extamp-pullhigh/pulllow` pinctrl states are empty,
   `rom_dtb_0.dts:2405-2413`, so the non-legacy path cannot switch it). The
   GPL defconfig does not carry the option.
2. **[INFERENCE]** "enable last, disable first" is the intended order; the vendor
   kernel only exposes the pin as a HAL-written switch (`vendor:3165`), so its
   order relative to `Speaker_Amp_Switch` is not observable in any source here.
3. **[UNVERIFIED]** The amplifier part number. The task calls it "AW8736 class";
   the vendor source only names AW8736 inside `AW8736_MODE3/4`
   (`vendor:2160-2171,2273-2321`), which are not compiled. No register or I2C
   interface for the part exists in any available source, so only the enable
   pin is modelled (nothing invented).
4. **[UNVERIFIED]** The pull *direction* the vendor enables: `vendor:2196`
   programs only "pull enable", never the pull select. The DTS fragment below
   uses `bias-pull-up` so that an undriven pin keeps the amplifier disabled; the
   pin is push-pull output during operation, so this does not affect levels.
5. **[UNVERIFIED]** Whether the external amplifier also has to be muted around
   jack detection/accessory switches (`Receiver_Speaker_Switch`,
   `vendor:2389-2434`). Those paths were not ported and no evidence ties the
   external amp to them in the vendor kernel.
6. Delays are **not** inferred: they are copied verbatim from `vendor:2211`,
   `vendor:2228`, `vendor:2255`.

---

## DTS-FRAGMENT

Board DT (`mt6737t-samsung-grandpplte.dts`), not applied by this patch. The
codec node does not exist yet in `mt6328.dtsi`, so the whole node is shown. The
pin is already described by the MT6735 pinctrl binding
(`include/dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h:759`); the ROM's own
`extamp-*` pinctrl states are empty and are not reused.

```
&pio {
	/* GPIO117 = external speaker amplifier enable (ROM "extspkamp-gpio",
	 * rom_dtb_0.dts:3561).  Initial level low = amplifier disabled,
	 * vendor:2198.
	 */
	extamp_default: extamp-default-pins {
		pins-en {
			pinmux = <PINMUX_GPIO117__FUNC_GPIO117>;
			bias-pull-up;
			output-low;
		};
	};
};

&mt6328 {
	mt6328_codec: codec {
		compatible = "mediatek,mt6328-codec";
		pinctrl-names = "default";
		pinctrl-0 = <&extamp_default>;
		ext-amp-gpios = <&pio 117 GPIO_ACTIVE_HIGH>;
		status = "okay";
	};
};
```

If a board does not populate the amplifier, only `ext-amp-gpios` and the
pinctrl group are removed; the driver then reports the control as disabled and
probe still succeeds.

---

## 8. Patch

`$M/patches/audio-gap-extamp.patch` (generated with `git diff --binary` against
the staged baseline; no new files, so no `git add -N` was needed).
