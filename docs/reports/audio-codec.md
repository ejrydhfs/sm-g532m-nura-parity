# MT6328 PMIC audio codec (ROM driver name: mt6331)

## 1. Assigned driver and files changed

Deliverable: the MT6328 PMIC audio codec driver only. No machine driver, no AFE
platform driver, no ACCDET.

| File | Change |
|---|---|
| `sound/soc/codecs/mt6331.h` | **new** – MT6328 audio register map (PMIC offsets, fields, PMIC AFE digital block) |
| `sound/soc/codecs/mt6331.c` | **new** – platform driver, component, controls, DAPM, 3 DAIs, jack, suspend/resume |
| `sound/soc/codecs/Kconfig` | +9 lines: `config SND_SOC_MT6331` (`depends on MFD_MT6397`) |
| `sound/soc/codecs/Makefile` | +2 lines: `snd-soc-mt6331-y := mt6331.o`, `obj-$(CONFIG_SND_SOC_MT6331)` |
| `drivers/mfd/mt6397-core.c` | +3 lines: one cell appended to `mt6328_devs[]` (`mt6328-codec` / `mediatek,mt6328-codec`); array order otherwise untouched |

Component name `mt-soc-codec`, DAIs `mt-soc-codec-tx-dai`, `mt-soc-codec-tx-dai2`
(added per interface amendment), `mt-soc-codec-rx-dai`, DT compatible
`mediatek,mt6328-codec` — all as required by the interface contract.

**Naming fact (must not be “fixed”):** the vendor driver and its ROM symbols are
called *mt6331* (`mt6331_codec_probe`, `mt6331_read`, `mt6331_write`,
`mt6331_snd_controls`, `mt6331_dapm_widgets` in `zip/symbols.txt`), but the PMIC
soldered on MT6737T/grandpplte is an **MT6328** (`mediatek,mt6328` in
`zip/rom_dtb_0.dts`, `MT6328_CHIP_ID = 0x30` in `include/linux/mfd/mt6397/core.h`).
Only the MT6328 register map is implemented; there is no MT6331 register map in
this driver and no MT6331 support is claimed.

## 2. ROM / downstream evidence

Primary source: `src/downstream-grandpplte/sound/soc/mediatek/mt_soc_audio_v3/`.
Abbreviations below: `vendor:` = `mt_soc_codec_63xx.c`, `spk:` =
`mt_soc_codec_speaker_63xx.c`, `upmu:` =
`drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/upmu_hw.h`,
`ana.h` = `AudDrv_Ana.h`.

**Every register is a PMIC register over pwrap, not SoC MMIO.**
`AudDrv_Ana.c:67-104` (`Ana_Set_Reg`/`Ana_Get_Reg`) is a plain
`pwrap_read`/`pwrap_write` read-modify-write of a 16-bit offset with
`PMIC_REG_BASE = 0x0000` (`ana.h:69`). Accordingly the driver is a regmap client
of the parent PMIC (`mt6397_chip->regmap`, created by the pwrap driver with
`reg_bits = 16 / val_bits = 16 / max_register = 0xffff`,
`drivers/soc/mediatek/mtk-pmic-wrap.c:2528-2533`), and never touches
`0x1122xxxx`. Our `mt6331_ana_write(priv, reg, val, mask)` is exactly
`Ana_Set_Reg(reg, val, mask)`.

**Probe init sequence (ROM-confirmed).** `mt6331_codec_probe` is at
`0xc0864334`; the inlined `mt6331_codec_init_reg()` body is
`0xc0864460`-`0xc08644bc` (disassembled with capstone from
`zip/vmlinux.elf`; `llvm-objdump`/`objdump` reject the recovered image):

| ROM address | Instruction | `Ana_Set_Reg` call |
|---|---|---|
| `0xc0864460`-`6c` | `mov r1,r5; movw r0,#0x28a; mov r2,#1` | `TOP_CLKSQ, 0x0000, 0x0001` |
| `0xc0864470`-`7c` | `mov r1,#2; mov r2,r1; movw r0,#0xcec` | `AUDDEC_ANA_CON8, 0x0002, 0x0002` |
| `0xc0864480`-`8c` | `mov r1,#0x3800; mov r2,r1; movw r0,#0x23e` | `TOP_CKPDN_CON0_SET, 0x3800, 0x3800` |
| `0xc0864490`-`9c` | `mov r1,#0xe000; mov r2,r1; movw r0,#0xcdc` | `AUDDEC_ANA_CON0, 0xe000, 0xe000` |
| `0xc08644a0`-`ac` | `mov r1,r5; mov r0,#0xd00; mov r2,#0x10` | `AUDENC_ANA_CON9, 0x0000, 0x0010` |
| `0xc08644b0`-`bc` | `mov r1,#0x8000; mov r2,r1; movw r0,#0x203c` | `AFE_PMIC_NEWIF_CFG2, 0x8000, 0x8000` |
| `0xc08644c0` | `bl 0xc08642d4` | `InitCodecDefault()` |
| `0xc08644c4`-`cc` | `mov r3,#1; strb r3,[r4,#0x4c]` | `mInitCodec = true` |

This matches `vendor:4970-4982` exactly. `InitCodecDefault` (`0xc08642d4`, ROM)
stores `3` into `mAudio_Ana_Volume[MICAMP1..4]` (`str r1,[r3,#0x34/0x38/0x3c/0x40]`)
and `8` into `mAudio_Ana_Volume[HPOUTR]` (`str r0,[r3,#0xc]`), matching
`vendor:4975-4996`.

**Register map** (`upmu:` line numbers are cited in `mt6331.h`):
`AUDDEC_ANA_CON0..8` `0x0cdc..0x0cec` (`upmu:594-602`),
`AUDENC_ANA_CON0..10` `0x0cee..0x0d02` (`upmu:603-613`),
`AUDNCP_CLKDIV_CON0..4` `0x0d04..0x0d0c` (`upmu:614-618`),
`ZCD_CON0..4` `0x0800..0x0808` (`upmu:370-374`),
`SPK_CON*/SPK_ANA_CON*` `0x0a90..0x0ab6` (`upmu:464-483`),
`TOP_CKPDN_CON0/2` `0x023c/0x0248`, `TOP_CLKSQ` `0x028a` (`upmu:83-127`),
PMIC AFE digital block `0x2000..0x203c` (`ana.h:70-101`).

**Analog sequences used, with the downstream function they come from**
(all values transcribed; a scripted cross-check compared every numeric
`(reg, value, mask)` triple in `mt6331.c` (166 calls) against 307
`Ana_Set_Reg()` triples in the two vendor files: **all 166 matched**).

| Driver function | Downstream source |
|---|---|
| `mt6331_codec_init_reg` | `vendor:4970-4982` + ROM probe above |
| `mt6331_dl_power_up/down` | `vendor:1618-1671` `TurnOnDacPower`/`TurnOffDacPower` |
| `mt6331_dl_analog_up/down` | `vendor:1749-1778`, `vendor:1834-1849`, `vendor:1965-1976`, `vendor:2104-2119` |
| `mt6331_hp_power_up/down` | `vendor:1734-1826`, `vendor:1828-1862` `Audio_Amp_Change` |
| `mt6331_earpiece_power_up/down` | `vendor:1925-2008` `Voice_Amp_Change` |
| `mt6331_speaker_*`, `mt6331_speaker_classd_*` | `vendor:2030-2146` `Speaker_Amp_Change`, `spk:75-130` |
| `mt6331_speaker_gain_apply` | `vendor:569-604` `Apply_Speaker_Gain` |
| `mt6331_adc_supply_up/down` | `vendor:3256-3271`, `vendor:3469-3480` |
| `mt6331_adc_power_up/down` | `vendor:3359-3397`, `vendor:3472-3480` |
| `mt6331_pga_l_power_up/down` | `vendor:3273-3341`, `vendor:3404-3429` (main mic / headset mic) |
| `mt6331_pga_r_power_up/down` | `vendor:3295-3354`, `vendor:3444-3467` (reference mic) |
| `mt6331_micbias_set` | `vendor:3234-3239`, `3280-3288`, `3433-3439`, `1234-1267` |
| `mt6331_hp_volume_ramp` | `vendor:1673-1732` `HeadsetVoloumeRestore`/`Set` |
| `mt6331_dai_hw_params` | `vendor:1641-1646`, `vendor:3386-3389`, `1581-1616`, `1290-1306` |
| `mt6331_codec_suspend/resume` | `vendor:215-262` `SetAnalogSuspend` (extended, see §5) |
| controls | `vendor:2846-2859` (gain tables), `3927` (PGA table), `2635-2636`, `1234-1267` |
| DAI list | `vendor:1361-1580` `mtk_6331_dai_codecs`, `AudDrv_Def.h:273-295` |

ROM symbols confirming the driver shape: `mt6331_codec_probe`,
`mt6331_read`, `mt6331_write`, `mt6331_dapm_widgets`, `mt6331_snd_controls`,
`Speaker_ClassD_Open`, `Speaker_ClassD_close`, `AudDrv_GPIO_HPDEPOP_Select`,
`TurnOnADcPowerACC`, `InitCodecDefault` (`zip/symbols.txt`). Strings
`hpdepop-pullhigh`/`hpdepop-pulllow` (`zip/vmlinux.strings:58641-58642`) show
the de-pop switch is live in the shipped image.

**Third DAI (interface amendment).** `MT_SOC_CODEC_TXDAI2_NAME`
(`AudDrv_Def.h:274`) is bound by the ROM machine driver to the MultiMedia_DL2
link. It is implemented as an independent `snd_soc_dai_driver`
(`mt-soc-codec-tx-dai2`, stream `MultiMedia2_PLayback` →
`MT_SOC_DL2_STREAM_NAME`, `AudDrv_Def.h:313`) with its own AIF widget
(`AIF_RX2`) so ASoC does not serialise the two playback streams. The vendor
implementation is register-identical for both DAIs: the `TXDAI2` entry
(`vendor:1523-1531`) uses the same `mt6323_aif1_dai_ops`, the same
`mBlockSampleRate[AUDIO_ANALOG_DEVICE_OUT_DAC]` and therefore the same PMIC
downlink programming. No per-DAI register difference was invented; the two
DAIs share the single PMIC downlink voice (and the single `DAC` DAPM node),
which is what the hardware provides.

## 3. Parity matrix

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| Driver binding | 3.18 platform driver, `mediatek,mt_soc_codec_63xx` | none | platform driver, `mediatek,mt6328-codec`, MFD cell child of mt6328 | done |
| Register access | `Ana_Set_Reg` → pwrap RMW, 16/16 | none | `regmap_update_bits` on parent PMIC regmap, same offsets/masks | done |
| Probe init sequence (6 writes) | `mt6331_codec_init_reg` (`vendor:4970`, ROM `0xc0864460`) | none | `mt6331_codec_init_reg()` identical values/masks | done |
| Default volumes (MICAMP=3, HPOUT=8) | `InitCodecDefault` RAM copy only (`vendor:4975`, ROM `0xc08642d4`) | none | programmed once at probe into `AUDENC_ANA_CON10` / `ZCD_CON2` (see §5) | done |
| Component/DAIs | `snd_soc_register_codec`, 16 DAIs | none | `snd_soc_component_driver` + 3 DAIs (`tx`, `tx2`, `rx`) | done (DAI subset, see below) |
| Downlink digital (AFE, rate regs, AFE enable) | `TurnOnDacPower`/`TurnOffDacPower` | none | `mt6331_dl_power_up/down`, rate from `hw_params` | done |
| Headphone analog + de-pop | `Audio_Amp_Change`, `HP_Ana_Switch_to_On/Release` | none | `mt6331_hp_power_up/down`, stepwise ZCD volume ramp, optional de-pop GPIO | done |
| Earpiece analog | `Voice_Amp_Change` | none | `mt6331_earpiece_power_up/down` | done |
| Internal speaker (class D) | `Speaker_Amp_Change` + `Speaker_ClassD_Open/close` | none | `mt6331_speaker_power_up/down` + class-D trim sequence + gain ramp | done |
| Shared DL LDO/NCP/NVREG, last-user teardown | `GetDLStatus()` guard in each amp path | none | `mt6331_dl_analog_up/down`, `MT6331_ST_OUTPUTS` guard | done |
| Capture analog (LDO/CLKGEN/LCLDO_ENC) | `TurnOnADcPowerACC` head | none | `mt6331_adc_supply_up/down` | done |
| Capture per-channel (AIN0/AIN1, AIN2, PGA, ADC bits) | `TurnOnADcPowerACC` branches | none | `mt6331_pga_l/r_power_up/down` | done |
| Capture digital (AFE UL enable, UL rate) | `TurnOnADcPowerACC` tail | none | `mt6331_adc_power_up/down` + UL rate in `hw_params` | done |
| Microphone bias 0/1 + reference save/restore | `vendor:3280-3288`, `3433-3439`, `MicbiasRef` | none | `MICBIAS1`/`MICBIAS2` DAPM supplies, `mt6331_micbias_set`, ref save/restore | done |
| HP/eartpiece volume, mic amp gain, speaker PGA gain, micbias vref/mode | vendor enum controls | none | 8 ALSA controls with real fields, TLV where a table exists | done |
| DAPM graph | 3 output widgets only; routing done from userspace via vendor switches | none | outputs EARPIECE/HEADSET/SPEAKER, inputs MIC1/MIC2/Headset Mic, MICBIAS1/2, PGA L/R, ADC Supply, DAC, ADC, AIFs | done |
| Rate programming | `GetDLNewIFFrequency`, `ULSampleRateTransform` | none | `mt6331_dai_hw_params` (8k-48k incl. 11.025/22.05k for DL) | done |
| Jack: headset mic bias + AIN1 path | `Audio_MicSource1_Setting` + accdet | none | `set_jack` + jack notifier switches bias 1 and AIN1 | done |
| Suspend/resume | `SetAnalogSuspend` saves `mAudio_Ana_DevicePower[]`, powers amps down, restores | none | full save/power-down/restore of every powered block | done (extended, §5) |
| HP de-pop switch | `AudDrv_GPIO_HPDEPOP_Select` via pinctrl states | none | optional `hp-depop-gpios` GPIO, toggled in the HP/speaker sequences | done (needs DTS, §6) |
| Voice call DAIs (MD1/MD2), TDM | `MT_SOC_CODEC_VOICE_MD*` | none | — | deferred (task) |
| VOW (analog + digital) | `_VOW_ENABLE` blocks, `AFE_VOW_CFG*` | none | — | deferred (task) |
| HP impedance detection | `OpenHeadPhoneImpedanceSetting` | none | — | deferred (task) |
| FM paths (I2S2 / MRG / FM_I2S) | `MT_SOC_CODEC_FM*` | none | — | deferred (task) |
| Speaker protection / SPKL IRQ, current sensing | `Audio_Speaker_CurrentSensing*`, `SPK_CON12` | none | — | deferred (task) |
| OTP/efuse HP offset trim | `Auddrv_Read_Efuse_HPOffset`, `SetDcCompenSation`, `AFE_DL_DC_COMP_CFG0/1` | none | — | deferred: efuse read is ROM/nvmem-only, offset values unavailable; no trim/DC-comp registers written |
| Audio DAC/ADC digital volume | none with a documented scale (`AFE_DL_SDM_CON1` is a fixed `0x001e` attenuation) | none | fixed write kept, no control invented | deferred (no register/scale evidence) |
| External speaker amp (AW8736) | `Ext_Speaker_Amp_Change`, `AW8736_MODE*`, `extspkamp-gpio` (GPIO117 in ROM DTS) | none | — | deferred (board-level GPIO policy; belongs to machine/board layer, see §5) |
| Class-AB / receiver speaker modes, 2-in-1 headset-speaker, lineout, DMIC | `Speaker_ClassAB_Open`, `Speaker_ReveiverMode_Open`, `Headset_Speaker_Amp_Change`, `Lineout_*`, `TurnOnADcPowerDmic` | none | — | deferred: selected by vendor-private userspace controls not used by this board; implementing them would add register blocks nothing selects |
| Audio clock buffer (`CLK_BUF_AUDIO`) | `audckbufEnable` → `clk_buf_ctrl()` | Nura has no clk-buffer driver | calls omitted | deferred: no counterpart in this tree; owned by the clock/AFE slice |
| AuxADC/loopback/sine-table test controls, `Audio_VOW_*` test controls | `mt6331_pmic_Test_controls` etc. | none | — | deferred (test/debug interfaces, not ported) |

## 4. Compile-tested

Worktree `$M/worktrees/audio-codec` (HEAD `046883c0c`), config built from
`$M/nura.config` with `SOUND`, `SND`, `SND_SOC=m`, `SND_SOC_MT6331=m`
(`olddefconfig` confirmed `CONFIG_SND=y`, `CONFIG_SND_SOC=m`,
`CONFIG_SND_SOC_MT6331=m`).

```
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
./scripts/config --enable SOUND --enable SND --module SND_SOC --module SND_SOC_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 sound/soc/codecs/mt6331.o drivers/mfd/mt6397-core.o
```

Result (after touching both sources, exit status 0):

```
  CC      drivers/mfd/mt6397-core.o
  CC [M]  sound/soc/codecs/mt6331.o
```

Also clean with `W=1`:

```
make ARCH=arm64 LLVM=1 -j2 W=1 sound/soc/codecs/mt6331.o
  CC [M]  sound/soc/codecs/mt6331.o
```

No errors, no warnings. Objects produced: `sound/soc/codecs/mt6331.o` (335216
bytes), `drivers/mfd/mt6397-core.o` (187864 bytes, contains the strings
`mt6328-codec` / `mediatek,mt6328-codec`).

Not run (out of scope): full kernel build, module link/`modpost` (needs
`vmlinux`/`Module.symvers`), any runtime or hardware test. No phone attached.

## 5. Inferred or unverified-on-hardware items

Everything below is either marked `[INFERENCE]` or is a deliberate deviation
from the vendor driver, with the reason.

1. **`[INFERENCE]` Microphone-bias masks.** The vendor enables a bias with a
   very broad mask (`AUDENC_ANA_CON9, 0x0701, 0xff19` / `0x0710, 0xff10`),
   which also clears the *other* bias enable and reserved bits 11-15. This
   driver writes only the enable bit and the reference field
   (`RG_AUDMICBIASVREF_MASK | BIT(0|4)`) so that the two biases can be
   independent DAPM supplies. Same intent, safer mask; not hardware verified.
2. **`[INFERENCE]` Microphone-bias reference default.** When the saved
   reference field reads back as 0 (the probe sequence only clears bit 4, so
   the reset value of bits 8-10 is unknown) the driver programs the vendor
   value 7 (`vendor:3280`). The "if 0 use 7" rule is inferred.
3. **`[INFERENCE]` Earpiece volume restoration.** `Voice_Amp_Change(true)`
   unconditionally ends with `ZCD_CON3 = 0x0000` (+8 dB) (`vendor:1997`),
   discarding the user's "Handset_PGA_GAIN" setting. To make the exported
   "Earpiece Volume" control effective, the driver saves `RG_AUDHSGAIN` before
   the sequence and re-applies it afterwards. Same technique the vendor itself
   uses for the headphone volume.
4. **`[INFERENCE]` HP/mic gain restoration.** The vendor keeps the user
   volumes in RAM (`mAudio_Ana_Volume[]`) and re-applies them on power-up
   (`SetMicPGAGain`, `HeadsetVoloumeSet`). Here the current register values are
   read back before the de-pop / power-down sequences clobber them and are
   restored afterwards. A userspace change made while the path is powered down
   is overwritten by that cache (same behaviour as the vendor software copy).
5. **`[INFERENCE]` Default-volume write at probe.** `InitCodecDefault` only
   fills a RAM struct in the ROM (no pwrap write in `0xc08642d4`). This driver
   writes the same two default fields to hardware at probe so that the ALSA
   controls start at the documented defaults. Note the ROM/downstream quirk
   `mAudio_Ana_Volume[HPOUTR] = 8` twice (HPOUTL stays 0); both channels are
   set to 8 here, which is what every HP power-up applies anyway
   (`vendor:1820-1822`).
6. **`[INFERENCE]` Split of the vendor sequences across DAPM widgets.** The
   vendor runs one linear sequence per analog output; the driver maps it onto
   DAPM widgets (`ADC Supply`, `MICBIAS1/2`, `PGA L/R`, `ADC`, `DAC`,
   `Headphone/Earpiece/Speaker Amp`) and relies on ASoC's widget-type ordering
   (`sound/soc/soc-dapm.c:74-109`, `dapm_down_seq`). All four directions
   reproduce `mt_soc_codec_63xx.c`:
   playback up `DAC(8)` → amps`(12)` = `TurnOnDacPower` then analog;
   playback down amps`(5)` → `DAC(8)` = amp teardown then `TurnOffDacPower`;
   capture up supply`(3)` → `PGA(10)` → `ADC(11)` = LDO/CLKGEN, per-channel,
   then uplink digital; capture down `ADC(3)` → `PGA(6)` → `MICBIAS(10)` →
   supply`(14)` = uplink off, per-channel off, then LCLDO_ENC/ADC clock
   generator and finally Topck/Clsq/Nvreg (that tail is in the `ADC Supply`
   widget's `POST_PMD`). The only ordering DAPM does not define is between the
   two supply-type widgets (`ADC Supply` and `MICBIAS1/2`, same sequence
   number), i.e. a microphone bias may be enabled before the shared encoder
   LDO; that is unverified but both are before the PGAs.
7. **`[INFERENCE]` Suspend/resume extension.** `SetAnalogSuspend` only powers
   the three output amplifiers down and restores them. Because this driver
   clocks and powers the capture side itself, suspend/resume additionally
   tears down and restores the uplink blocks in reverse power-up order
   (state bitmask `MT6331_ST_*`). The vendor has no equivalent path in the
   codec driver.
8. **De-pop GPIO (optional, unverified).** The ROM drives the "hpdepop"
   pinctrl output (package pin `0x5000` = GPIO80, function 0) low while the
   headphone amplifier settles (`vendor:383-440`). Nura has no pinctrl state
   for it in the audio node, so the driver takes an optional
   `hp-depop-gpios` descriptor (active high = released, low = grounded) and
   silently skips the toggling when the property is absent. Pin assignment and
   polarity are `[INFERENCE]` from the ROM pinctrl data; **unverified on
   hardware**.
9. **Not verified on hardware:** all timing (`udelay(250)`, `udelay(50)`,
   `msleep(10)` speaker trim, 200 µs volume ramp steps, 10 ms de-pop settle)
   is copied from the vendor source, not measured. The speaker class-D trim
   result is only logged (`dev_dbg`), as in the vendor driver, and its offset
   value is not used.
10. **`[INFERENCE]` Clock-buffer:** `audckbufEnable()`/`clk_buf_ctrl(CLK_BUF_AUDIO)`
    has no target in this tree, so those calls are absent. If the audio clock
    buffer must be enabled for playback on real hardware, this is a **missing
    prerequisite in the clock/AFE slice**, not something this driver can fake.
11. **Deferred register blocks that exist in the vendor driver and are
    deliberately not implemented** (each would be dead code without a
    selector): Class-AB open/close (`spk:133-...`), receiver-mode speaker
    (`spk:...`), 2-in-1 headset/speaker (`vendor:2467-2660`), external AW8736
    amplifier (`vendor:2175-2380`), lineout (`ZCD_CON1`), DMIC
    (`vendor:3440-...`), HP impedance (`vendor:880-990`), VOW, voice MD1/MD2,
    FM, speaker protection/current sensing (`SPK_CON12`), AUXADC/loopback/sine
    test paths.
12. **Sub-DAI count parity:** the vendor codec registers 16 DAIs; this driver
    registers the three required ones. The other 13 (voice, FM, TDM, stub,
    loopback, merge, HP impedance) are deferred with item 11.

## 6. DTS-FRAGMENT

Add to the **board** file `arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`
(not to the shared `mt6328.dtsi`). `GPIO_ACTIVE_HIGH` means "pin released";
the driver drives it low while the headphone amplifier settles. If the
integration owner decides the de-pop pin belongs to the AFE/machine slice
instead, drop the `pinctrl-*`, `hp-depop-gpios` lines and the second hunk — the
codec works without them (only the de-pop toggle is skipped).

```dts
&mt6328 {
	mt6328_codec: codec {
		compatible = "mediatek,mt6328-codec";

		pinctrl-names = "default";
		pinctrl-0 = <&aud_hp_depop_default>;

		hp-depop-gpios = <&pio 80 GPIO_ACTIVE_HIGH>;
	};
};

&pio {
	aud_hp_depop_default: aud-hp-depop-default-pins {
		pins-depop {
			pinmux = <PINMUX_GPIO80__FUNC_GPIO80>;
			bias-disable;
			output-high;
		};
	};
};
```

Notes for the integrator:
* `hp-depop-gpios` must be spelled exactly like that: gpiolib only looks up
  `<con_id>-gpios`, vendor prefixes are not supported for con_id lookups.
* `PINMUX_GPIO80__FUNC_GPIO80` already comes from
  `include/dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h`, which the board DTS
  includes; `GPIO_ACTIVE_HIGH` comes from
  `include/dt-bindings/gpio/gpio.h`, also already included.
* The MFD cell is created by `drivers/mfd/mt6397-core.c` and needs this child
  node to be matched via `mediatek,mt6328-codec`; without the node the child is
  still created and binds by platform name `mt6328-codec`.

## 7. Patch path

`$M/patches/mt6331-codec.patch` (`git diff --binary`, 1959 lines, 5 files,
1907 insertions; new files added with `git add -N`, nothing committed).
Driver sources: `$M/worktrees/audio-codec/sound/soc/codecs/mt6331.{c,h}`.
