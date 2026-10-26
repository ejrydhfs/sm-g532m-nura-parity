# Audio subsystem — coordinator report

Subsystem: MT6737T (Samsung SM-G532M “grandpplte”) audio — AFE/platform, MT6331-named PMIC codec, ACCDET, machine/jack.
Target kernel: Nura `v7.0.10-mt6735` (arm64), base commit `046883c0c`.
Downstream reference: `$M/src/downstream-grandpplte` (primary), ROM `$M/zip/vmlinux.elf` + `$M/zip/rom_dtb_0.dts` (confirmation).
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`.

Status: **4/4 leaf drivers delivered, integrated, and compile-tested**. No shared DTS was modified; the consolidated
fragment is in §7. Hardware validation is not available (see §8).

---

## 1. Leaf list

| # | Driver | Worktree (from `nura-linux` HEAD) | Patch | Report |
|---|---|---|---|---|
| 1 | MT6735/MT6737T ASoC AFE platform + FE DAIs (DL1/DL2/VUL) + I2S0 BE DAI | `worktrees/audio-mt-soc` | `patches/audio-mt-soc.patch` | `logs/audio-mt-soc.md` |
| 2 | MT6331-named MT6328 PMIC audio codec (component, DAPM, controls, 3 DAIs) | `worktrees/audio-codec` | `patches/audio-codec.patch` | `logs/audio-codec.md` |
| 3 | MT6328 ACCDET (plug/cable type/mic/buttons, notifier API) | `worktrees/audio-accdet` | `patches/audio-accdet.patch` | `logs/audio-accdet.md` |
| 4 | MT6735 machine card + headset/jack integration | `worktrees/audio-jack` | `patches/audio-jack.patch` | `logs/audio-jack.md` |

Each leaf patch applies cleanly to `src/nura-linux` HEAD on its own (`git apply --check`, all four verified).
Each leaf patch is byte-identical to the current state of its worktree (`git diff HEAD --binary`).
Each leaf ran its own compile test; results are re-verified at subsystem level in §5.

New source delivered by the subsystem (5 677 inserted lines, 23 files):

| Area | Files | Lines |
|---|---|---|
| AFE platform | `sound/soc/mediatek/mt6735/{mt6735-afe-pcm.c, mt6735-afe-clk.c/.h, mt6735-afe-common.h, mt6735-afe-regs.h}` | 1 155 |
| AFE DAIs | `mt6735-dai-pcm.c` (553), `mt6735-dai-i2s.c` (285) | 838 |
| Codec | `sound/soc/codecs/mt6331.c` (1 714), `mt6331.h` (179) | 1 893 |
| ACCDET | `drivers/misc/mediatek/accdet/{accdet.c (1 056), accdet-regs.h (135), Kconfig, Makefile}` + `include/linux/mfd/mt6328/accdet.h` (87) | 1 299 |
| Machine/jack | `sound/soc/mediatek/mt6735/mt6735-mt6331.c` | 436 |
| Plumbing | `sound/soc/mediatek/{Kconfig,Makefile,mt6735/Makefile}`, `sound/soc/codecs/{Kconfig,Makefile}`, `drivers/mfd/mt6397-core.c`, `drivers/misc/{Kconfig,Makefile}` | 56 |

---

## 2. Integration worktree and conflict resolution

Integration worktree: `worktrees/audio-integration` (fresh `git worktree add --detach HEAD`).
Leaf patches applied in order 1→2→3→4 with `git apply --3way`.

Conflicts (all mechanical, resolved in favour of keeping both slices’ intent):

| File | Conflict | Resolution |
|---|---|---|
| `drivers/mfd/mt6397-core.c` | codec cell vs accdet cell appended to `mt6328_devs[]` at the same position | both cells kept, codec first, accdet second; `mt6328_accdet_resources[]` (IRQ `MT6328_IRQ_STATUS_ACCDET`) kept |
| `sound/soc/mediatek/Kconfig` | `SND_SOC_MT6735` (AFE) and `SND_SOC_MT6735_MT6331` (machine) inserted at the same line | both symbols kept, AFE first; help-text continuation lines reattached to the machine symbol |
| `sound/soc/mediatek/Makefile` | two competing `mt6735/` descend lines | single `obj-$(CONFIG_SND_SOC_MT6735) += mt6735/` (the machine symbol depends on it, so the directory is always descended) |
| `sound/soc/mediatek/mt6735/Makefile` | both slices created the file | merged: SPDX + AFE objects + machine object |

Coordinator fixes applied on top of the leaf patches (cross-slice integration only, no driver logic rewritten):

1. **DL2 second codec DAI.** The machine slice initially wired `MultiMedia_DL2` to `mt-soc-codec-tx-dai` because the
   interface contract exposed only one playback DAI; the ROM uses a distinct DAI (`MT_SOC_CODEC_TXDAI2_NAME`,
   `AudDrv_Def.h:274`, `mt_soc_machine.c:1099-1106`). The codec slice added `mt-soc-codec-tx-dai2`; the machine link is
   now `COMP_CODEC("mt-soc-codec", "mt-soc-codec-tx-dai2")`, removing the stream-serialisation limitation both slices flagged.
2. **`accdet.h` reachability.** `#if IS_ENABLED(CONFIG_MTK_ACCDET)` → `#if IS_REACHABLE(...)`, so a built-in card that
   consumes a modular ACCDET driver gets the stubs instead of an unresolvable symbol (the ACCDET slice flagged this
   exact link hazard).
3. **Kconfig dependency.** `SND_SOC_MT6735_MT6331` now also `depends on SND_SOC_MT6331` (deliberately `depends`,
   not `select`: `SND_SOC_MT6331` depends on `MFD_MT6397`, and `select` would violate that dependency).

Interface contract verified end-to-end after integration:
AFE component `mtk-afe-pcm`; CPU DAIs `mt-soc-dl1dai-driver`, `mt-soc-dl2dai-driver`, `mt-soc-ul1dai-driver`,
`mt-soc-i2s0dai-driver`; codec component `mt-soc-codec` with DAIs `mt-soc-codec-tx-dai`, `mt-soc-codec-tx-dai2`,
`mt-soc-codec-rx-dai`; card `mt-snd-card`; ACCDET exports `accdet_register_notifier`, `accdet_unregister_notifier`,
`accdet_get_state` (`EXPORT_SYMBOL_GPL`) and the machine module’s undefined-symbol list matches exactly
(`llvm-nm` cross-check on the built objects).

---

## 3. What each driver implements

### 3.1 AFE platform (`audio-mt-soc`)
- Platform driver `mediatek,mt6735-afe`, regmap-MMIO `0x11220000`/`0x10000`, IRQ from DT (ROM: GIC SPI 144, level high),
  runtime PM with regmap cache and clock gating (`mt6735-afe-clk.c`).
- Memifs DL1/DL2/VUL with base/cur/end programming, `AFE_DAC_CON0` enable bits, `AFE_IRQ_MCU_CON/STATUS/CLR/CNT`,
  period elapsed from `snd_pcm_period_elapsed()`, spurious-IRQ limiter (`mt_soc_afe_control.c:594-610`).
- FE DAIs `mt-soc-dl1dai-driver` (playback), `mt-soc-dl2dai-driver` (playback), `mt-soc-ul1dai-driver` (capture):
  real `startup/hw_params/prepare/trigger/pointer`; 8–48 kHz, S16/S24, 1–2 ch; unsupported params rejected.
  FE prepare also programs the PMIC codec bus (`AFE_I2S_CON1`, `AFE_ADDA_DL_SRC2_CON0/1`, `AFE_UL_DL_CON0`) as downstream does.
- BE DAI `mt-soc-i2s0dai-driver` (bidirectional) over `AFE_I2S_CON`: `set_fmt` accepts I2S + CBP_CFP/CBC_CFC with NB_NF/NB_IF,
  `set_sysclk`, `hw_params`, `trigger`; pinctrl `audi2s1-mode0/1`.
- PMIC clock pin mux: `audpmicclk-mode1` while streaming, `mode0` when idle (`AudDrv_Gpio.c:215-236`; the ROM DTS
  confirms mode1 = `AUD_*` functions). The assignment text said the opposite; the leaf documents the disagreement in
  `logs/audio-mt-soc.md` §5, and the ROM evidence was followed.
- 21 `aud_*` clocks are fetched with `devm_clk_bulk_get_optional()` because this kernel has no `mt6735-audiosys`
  clock provider yet — see §8.

### 3.2 Codec (`audio-codec`)
- `sound/soc/codecs/mt6331.c`, component `.name = "mt-soc-codec"`, platform driver `mediatek,mt6328-codec`, regmap is
  the parent PMIC regmap (16-bit/16-bit) exactly like downstream `Ana_Set_Reg()` (a pwrap read-modify-write); no raw pwrap access.
- Naming documented: ROM/downstream symbols say `mt6331`, the silicon is MT6328; only the MT6328 register map is implemented,
  and the report says so explicitly (no invented MT6331 map).
- Init sequence, analog power-up/down and suspend/resume replay taken from downstream; every one of the 166 (reg, value, mask)
  triples was machine-compared against the 307 `Ana_Set_Reg()` triples in `mt_soc_codec_63xx.c`/`mt_soc_codec_speaker_63xx.c`
  (all matched), and the init sequence was cross-checked against ROM `mt6331_codec_probe` (`0xc0864334`, inlined init at
  `0xc0864460-0xc08644bc`).
- 8 controls, all mapped to documented `upmu_hw.h` fields: headset volume (stereo TLV, ZCD), earpiece volume (TLV, ZCD),
  speaker PGA gain (vendor enum), mic amp 1/2 (TLV), mic bias voltage, MICBIAS1/2 mode.
- DAPM: `AIF_RX`/`AIF_RX2` → DAC → headphone/earpiece/speaker amps → `HEADSET`/`EARPIECE`/`SPEAKER`;
  `MICBIAS1/2` → `MIC1`/`MIC2`/`Headset Mic` → PGA L/R → ADC → `AIF1TX`. `.set_jack` (via `snd_soc_jack_notifier_register`)
  switches the headset micbias and AIN1 path.
- DAIs `mt-soc-codec-tx-dai` (playback), `mt-soc-codec-tx-dai2` (playback), `mt-soc-codec-rx-dai` (capture);
  8–48 kHz, S16/S24, 1–2 ch.

### 3.3 ACCDET (`audio-accdet`)
- `drivers/misc/mediatek/accdet/accdet.c`, platform driver `mediatek,mt6328-accdet`, bound through a new MFD cell with
  IRQ resource `MT6328_IRQ_STATUS_ACCDET` (hwirq 10 in the mt6397 irq domain); regmap from the parent PMIC.
- Hardware bring-up in downstream order: clock gate (TOP_CKPDN), reset release, PWM width/threshold, debounce
  programming, ACCDET enable; power-down on remove/suspend with register-state save/restore (`accdet_hw_init`,
  `accdet_disable`, `accdet_suspend/resume`).
- Detection: threaded PMIC IRQ → work item → `accdet_check_cable_type` (3-pole / 4-pole via AUXADC mic measurement with
  downstream polling/timeout), `accdet_key_check` / `accdet_multi_key_detection` against the DT 3-/4-key thresholds,
  `accdet_plug_out`.
- DT parsing of the ROM properties (`debounce`, `headset-mode-setting`, `accdet-mic-vol`, `accdet-mic-mode`,
  `accdet-plugout-debounce`, `headset-vol-threshold`, `headset-three-key-threshold`, `headset-four-key-threshold`), with
  the ROM DTS values as defaults.
- Clients via `accdet_register_notifier()` (blocking notifier) with `ACCDET_EVENT_PLUG` / `ACCDET_EVENT_BUTTON` and
  `accdet_get_state()` for boot-time seeding. No second input device; the ASoC jack owns the input device.

### 3.4 Machine / jack (`audio-jack`)
- Card `mt-snd-card` (`snd_soc_card`), platform driver `mediatek,mt6735-sound`, four non-DPCM links:
  `MultiMedia1` (DL1→`mt-soc-codec-tx-dai`, playback), `MultiMedia2` (UL1→`mt-soc-codec-rx-dai`, capture),
  `MultiMedia_DL2` (DL2→`mt-soc-codec-tx-dai2`, playback), `I2S0OUTPUT` (I2S0↔`snd-soc-dummy`, bidirectional,
  `I2S|NB_NF|CBC_CFC` from `AFE_I2S_CON`/`I2S_CON3` programming).
- ASoC jack (`Headphone`, `Headset Mic`, BTN_0..3 mapped to `KEY_PLAYPAUSE`/`KEY_VOICECOMMAND`/`KEY_VOLUMEUP`/`KEY_VOLUMEDOWN`)
  created on the MultiMedia1 runtime, `snd_soc_component_set_jack()` on the codec (`-ENOTSUPP` tolerated),
  ACCDET notifier registered in probe and removed on unbind; jack state and key state translated with
  remembered-press release semantics and boot-state seeding via `accdet_get_state()`.

---

## 4. Leaf-level evidence highlights (full detail in each leaf report)

| Claim | Evidence |
|---|---|
| AFE base/size/IRQ | `rom_dtb_0.dts:3537-3547` (`audio@11220000`, `reg = <0x11220000 0x10000>`, `interrupts = <0 0x90 8>`) |
| AFE register offsets | `AudDrv_Afe.h` (memif, IRQ, I2S), cross-checked in ROM disassembly: `AudDrv_IRQ_handler@0xc084c08c` (0x3A4/0x3B4/0x3BC/0x3CC/0x3A8), `SetIrqMcuCounter@0xc0847224` (0x3AC/0x3B0), `SetIrqEnable@0xc0846f60` (0x3A0/0x3B4), `EnableAfe@0xc0847fac` (0x10) |
| Codec = PMIC registers | `AudDrv_Ana.h:69-80` (`PMIC_REG_BASE 0x0`, `AFE_UL_DL_CON0 = 0x2000`), `mt_soc_codec_63xx.c:5078-5097` (`Ana_Get/Set_Reg`), ROM `mt6331_codec_probe@0xc0864334` adds 3 DAPM widgets, 1 route and control sets of 16/33/0/6/5/1 (same shape as the downstream arrays) |
| Codec analog fields | `.../mt6735/include/mach/upmu_hw.h` `MT6328_AUDDEC_ANA_CON*`/`MT6328_AUDENC_ANA_CON*` + `MT6328_PMIC_RG_AUD*` field macros |
| ACCDET registers | downstream `reg_accdet.h` (e.g. `ACCDET_CTRL 0x0F16`) == `upmu_hw.h MT6328_ACCDET_CON1 0x0F16` |
| ACCDET DT values | `rom_dtb_0.dts:4024-4040` |
| Card / link names | `mt_soc_machine.c:854-1106`, `.name = "mt-snd-card"` at 1139, `AudDrv_Def.h:204-296` |
| Headset key bands | `accdet.c:712-769` (thresholds + keycodes) |

---

## 5. Compile results (subsystem integration)

Worktree `worktrees/audio-integration`, clang 23 (`/usr/bin/clang`), LLVM shims `$M/bin` + `$M/llvmpkg/usr/bin`:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
./scripts/config --enable SOUND --enable SND --module SND_SOC --module SND_SOC_MEDIATEK \
    --module SND_SOC_MT6735 --module SND_SOC_MT6735_MT6331 --module SND_SOC_MT6331 --module MTK_ACCDET
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
    sound/soc/mediatek/mt6735/mt6735-afe-pcm.o sound/soc/mediatek/mt6735/mt6735-afe-clk.o \
    sound/soc/mediatek/mt6735/mt6735-dai-pcm.o sound/soc/mediatek/mt6735/mt6735-dai-i2s.o \
    sound/soc/mediatek/mt6735/mt6735-mt6331.o \
    sound/soc/codecs/mt6331.o drivers/misc/mediatek/accdet/accdet.o drivers/mfd/mt6397-core.o
```

`olddefconfig` resolves `CONFIG_SOUND=y`, `CONFIG_SND_SOC_MT6735=m`, `CONFIG_SND_SOC_MT6735_MT6331=m`,
`CONFIG_SND_SOC_MT6331=m`, `CONFIG_MTK_ACCDET=m`, `CONFIG_MFD_MT6397=y`.

Result: **PASS, exit 0, no warnings**. Objects produced:

```
  CC      drivers/mfd/mt6397-core.o
  CC [M]  sound/soc/codecs/mt6331.o
  CC [M]  drivers/misc/mediatek/accdet/accdet.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-clk.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-i2s.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-mt6331.o
```

Directory-level build (exercises the merged Kconfig/Makefile wiring):

```sh
make ARCH=arm64 LLVM=1 -j2 sound/soc/mediatek/ sound/soc/codecs/ drivers/misc/mediatek/accdet/
```
Result: **PASS, exit 0** (`LD [M] sound/soc/mediatek/common/snd-soc-mtk-common.o`, both `mt6735` module objects,
`mt6331` and `accdet` objects; no errors).

Module final link (`*.ko`) cannot be completed in this worktree: `modpost` requires `vmlinux.o`/`Module.symvers` from a
full kernel build, which the assignment forbids outside the integration owner’s full build. Consequence: the object-level
and `LD [M]` (partial link) results are verified; **symbol-level cross-module resolution was verified separately** with
`llvm-nm` (ACCDET exports `accdet_register_notifier`/`accdet_unregister_notifier`/`accdet_get_state`; the machine object’s
undefined list is exactly those three names; no duplicate definitions between modules). A full `make modules` in the
integration worktree remains the last mechanical check.

---

## 6. Parity matrix (subsystem summary)

| Behaviour | ROM | Nura before | After | Status |
|---|---|---|---|---|
| AFE MMIO/IRQ, memif DL1/DL2/VUL, period IRQ | `mt_soc_afe_control.c`, `AudDrv_Afe.h` | absent | implemented, register-cited | done |
| AFE I2S0 BE (format/clock/trigger) | `mt_soc_pcm_dl1_i2s0.c` | absent | implemented | done |
| Codec analog power/volume/micbias/DAPM | `mt_soc_codec_63xx.c` | absent | implemented (166/166 triples matched) | done |
| Codec jack (micbias + AIN1 switch) | downstream `set_jack`-less; analog path | absent | `.set_jack` via jack notifier | done |
| ACCDET plug / cable type / mic / keys | `accdet.c` (2 073 lines) | absent | 1 056-line regmap driver + notifier | done |
| Machine card + 4 PCM links | `mt_soc_machine.c` | absent | implemented | done |
| Headset jack reporting | downstream: ACCDET private input device | absent | single ASoC jack (no duplicate input device) | done (design change, documented) |
| Voice MD1/MD2 (+BT), MRG, TDM | full link set | absent | not present | deferred (needs modem audio, out of scope) |
| HDMI, FM I2S/MRG, AWB, ULDL loopback, offload/compress, HP impedance | full link set | absent | not present | deferred |
| BT CVSD (DAIBT) | `mt_soc_pcm_bt_dai.c` | absent | not present | deferred (needs BT firmware interface) |
| AFE internal-SRAM buffers, side tone, HW gain, ASRC, 2nd I2S | present | absent | not present | deferred |
| VOW, speaker protection/SPKL IRQ, OTP/efuse HP trim, DC compensation | present | absent | not present | deferred (OTP read is ROM/nvmem-only; no safe mainline source) |
| `/dev/accdet` ioctls, Samsung jack sysfs, debugfs/proc controls | present | absent | not ported | deferred (HAL/factory interfaces, explicitly out of scope) |
| External speaker amp (AW8736, GPIO117) | downstream board glue | absent | not present | deferred (board-level) |

---

## 7. DTS fragment (consolidated, NOT applied)

For `arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts` (board file). `mt6328.dtsi`, `mt6735.dtsi` and all other
shared DTS stay untouched. `mt6328` label exists in `mt6328.dtsi`; `mt6735-pinctrl.h`, `gpio.h` and `irq.h` are already
included by the board/dtsi chain.

```dts
/* --- AFE platform (ROM: audio@11220000, rom_dtb_0.dts:3537-3547) --- */
&pio {
	aud_pins_default: auddefault {
		pins_cmd0_dat {
			pinmux = <PINMUX_GPIO80__FUNC_GPIO80>;	/* ROM 0x5000: depop/amp line, low */
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		pins_cmd1_dat {
			pinmux = <PINMUX_GPIO185__FUNC_F2W_CK>;	/* ROM 0xb901 */
			slew-rate = <0>;
			bias-disable;
		};
	};

	aud_pins_pmicclk_mode0: pmicclkmode0 {
		pins_cmd0_dat { pinmux = <PINMUX_GPIO143__FUNC_GPIO143>; };
		pins_cmd1_dat { pinmux = <PINMUX_GPIO144__FUNC_GPIO144>; };
		pins_cmd2_dat { pinmux = <PINMUX_GPIO145__FUNC_GPIO145>; };
	};

	aud_pins_pmicclk_mode1: pmicclkmode1 {
		pins_cmd0_dat { pinmux = <PINMUX_GPIO143__FUNC_AUD_CLK_MOSI>; };
		pins_cmd1_dat { pinmux = <PINMUX_GPIO144__FUNC_AUD_DAT_MISO>; };
		pins_cmd2_dat { pinmux = <PINMUX_GPIO145__FUNC_AUD_DAT_MOSI>; };
	};

	accdet_pins_default: accdet-default-pins {
		pins {
			pinmux = <PINMUX_GPIO1__FUNC_GPIO1>,	/* ROM 0x100 */
				 <PINMUX_GPIO4__FUNC_GPIO4>;	/* ROM 0x400 */
			slew-rate = <0>;
			bias-disable;
		};
	};

	accdet_pins_eint_as_int: accdet-eint-pins {
		pins {
			pinmux = <PINMUX_GPIO1__FUNC_GPIO1>,
				 <PINMUX_GPIO4__FUNC_GPIO4>;
			slew-rate = <0>;
			bias-disable;
		};
	};
};

afe: audio@11220000 {
	compatible = "mediatek,mt6735-afe";
	reg = <0 0x11220000 0 0x10000>;
	interrupts = <GIC_SPI 144 IRQ_TYPE_LEVEL_HIGH>;

	pinctrl-names = "default", "audpmicclk-mode0", "audpmicclk-mode1",
			"audi2s1-mode0", "audi2s1-mode1";
	pinctrl-0 = <&aud_pins_default>;
	pinctrl-1 = <&aud_pins_pmicclk_mode0>;
	pinctrl-2 = <&aud_pins_pmicclk_mode1>;

	status = "okay";
};

/* --- codec child of the MT6328 PMIC --- */
&mt6328 {
	mt6328_codec: codec {
		compatible = "mediatek,mt6328-codec";

		pinctrl-names = "default";
		pinctrl-0 = <&aud_pins_default>;

		/* Headphone/amp de-pop line (ROM audhpdepophigh/audhpdepoplow, pin 0x5000).
		 * The AFE default pinctrl state muxes the pad; this GPIO drives its level.
		 */
		hp-depop-gpios = <&pio 80 GPIO_ACTIVE_HIGH>;
	};

	mt6328_accdet: accdet {
		compatible = "mediatek,mt6328-accdet";
		status = "okay";

		pinctrl-names = "default", "state_eint_as_int";
		pinctrl-0 = <&accdet_pins_default>;
		pinctrl-1 = <&accdet_pins_eint_as_int>;

		/* ROM headset-detect EINT description (AP GPIO EINT 4); the driver uses the
		 * PMIC ACCDET interrupt from the MFD cell. Kept as hardware documentation.
		 */
		interrupt-parent = <&pio>;
		interrupts = <4 IRQ_TYPE_LEVEL_LOW>;
		debounce = <4 0x3e800>;

		accdet-mic-vol = <7>;
		headset-mode-setting = <0x500 0x500 0x1 0x1f0 0x800 0x800 0x20>;
		accdet-plugout-debounce = <20>;
		accdet-mic-mode = <1>;
		headset-vol-threshold = <0 0x252 0x253 0x270f 0xaf0>;
		headset-three-key-threshold = <0 0x7a 0xd4 0x1e8>;
		headset-four-key-threshold = <0 0x55 0x7a 0xd4 0x1e8>;
	};
};

/* --- machine card (names-only links; no phandles needed) --- */
/ {
	sound {
		compatible = "mediatek,mt6735-sound";
		status = "okay";
	};
};
```

Notes for the DTS owner:
* `audi2s1-mode0/1` are empty groups on grandpplte (ROM `rom_dtb_0.dts:2395-2402`); the AFE driver tolerates the
  pinctrl states being absent, so they are listed in `pinctrl-names` without phandles.
* `aud_pins_default` is shared by the AFE (pad mux + initial low) and the codec's `hp-depop-gpios` (level control);
  this mirrors the ROM, which has both `audiodefault` (0x5000 output-low) and `audhpdepophigh/low` groups.
* The AFE node should live in the board DTS rather than `mt6735.dtsi` until the audiosys clock provider exists (§8).

---

## 8. Unresolved gaps / prerequisites

1. **`mediatek,mt6735-audiosys` clock provider is missing in this kernel.** The downstream/ROM DTS models the audio
   subsystem as a clock controller (`audiosys@11220000`, `#clock-cells = <1>`) feeding `aud_afe_clk`, `aud_i2s_clk`,
   APLL tuners, muxes and MCLK/BCLK dividers. Nura only has the topckgen/infracfg audio gates
   (`CLK_TOP_AUDIO_SEL`, `CLK_TOP_AUDINTBUS_SEL`, `CLK_INFRA_AUDIO`). The AFE driver therefore fetches its 21 `aud_*`
   clocks with `devm_clk_bulk_get_optional()` and still binds; **register access works, but bit-clock/MCLK rates for
   I2S0 and the PMIC codec bus are not proven** until that provider exists. This is the single largest blocker to
   end-to-end audio bring-up.
2. **No hardware validation.** Nothing in this subsystem has run on a device. Every register sequence is source-derived
   (downstream match) or ROM-disassembly-confirmed, but none is silicon-verified: codec power sequencing, ACCDET
   thresholds/debounce, jack detection timing, I2S0 clocking, DAPM ordering (pop/click behaviour).
3. **ACCDET AP-GPIO EINT path not used.** The ROM used AP EINT 4 for low-power plug detection; the MT6735 `mtk_eint_hw`
   in this kernel has no `.db_time`, so the ROM's 256 ms plug debounce cannot be programmed. The driver uses the PMIC
   ACCDET comparator interrupt instead, keeps the unit enabled while idle, and documents the deviation. The board keeps the
   EINT property as hardware documentation.
4. **Deferred feature blocks** (intentional, not stubs): voice/modem PCMs, MRG, HDMI, FM, TDM, AWB, loopback, BT CVSD,
   offload/compress, HP impedance, VOW, speaker protection, OTP/efuse HP trimming, DC compensation, external speaker amp.
5. **HAL-facing interfaces deliberately not ported:** `/dev/accdet` ioctls, Samsung `SEC_JACK_SYSFS` jack factory test,
   debugfs/proc audio controls, wakelocks.
6. **Module link / modpost** needs a full kernel build in `worktrees/audio-integration` (or the shared integration tree)
   to produce `vmlinux.o` and `Module.symvers`. Cross-module symbols were verified statically; the final `.ko` link was
   not executed here.
7. **Naming caveat to carry forward:** the audio codec driver is named `mt6331` (ROM/downstream naming) but binds
   `mediatek,mt6328-codec` and implements only the MT6328 register map. `mt6331.dtsi` (a different PMIC) must not be
   wired to it.

---

## 9. Deliverables

| Artifact | Path |
|---|---|
| Leaf patch — AFE platform | `$M/patches/audio-mt-soc.patch` |
| Leaf patch — codec | `$M/patches/audio-codec.patch` |
| Leaf patch — ACCDET | `$M/patches/audio-accdet.patch` |
| Leaf patch — machine/jack | `$M/patches/audio-jack.patch` |
| **Integrated subsystem patch (23 files, +5 677)** | `$M/patches/audio-integration.patch` |
| Leaf reports | `$M/logs/audio-{mt-soc,codec,accdet,jack}.md` |
| This report | `$M/logs/audio-coordinator.md` |
| Worktrees | `$M/worktrees/audio-{mt-soc,codec,accdet,jack,integration}` |

The integrated patch is `git diff HEAD --binary` of `worktrees/audio-integration`; it contains no DTS, no build artifacts,
and no generated files. Applied order for replay: mt-soc → codec → accdet → jack, then the four coordinator fixes in §2.
