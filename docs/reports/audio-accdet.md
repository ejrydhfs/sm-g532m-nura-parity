# MT6328 PMIC ACCDET (accessory / headset jack detection) — parity report

Worktree: `$M/worktrees/audio-accdet` (nura-linux, git HEAD 046883c0c)

## 1. Assigned driver and files changed

Created:

| File | Lines | Purpose |
|---|---|---|
| `drivers/misc/mediatek/accdet/accdet.c` | 1056 | MT6328 ACCDET platform driver (MFD child) |
| `drivers/misc/mediatek/accdet/accdet-regs.h` | 135 | Register/bit definitions, every value cited |
| `drivers/misc/mediatek/accdet/Kconfig` | 13 | `CONFIG_MTK_ACCDET` |
| `drivers/misc/mediatek/accdet/Makefile` | 2 | `obj-$(CONFIG_MTK_ACCDET) += accdet.o` |
| `include/linux/mfd/mt6328/accdet.h` | 83 | shared client API (was untracked; reviewed and extended: `enum accdet_button`, `accdet_get_state()`, corrected `mic_adc` unit doc) |

Edited (minimal, additive):

| File | Change |
|---|---|
| `drivers/misc/Kconfig` | `source "drivers/misc/mediatek/accdet/Kconfig"` |
| `drivers/misc/Makefile` | `obj-$(CONFIG_MTK_ACCDET) += mediatek/accdet/` |
| `drivers/mfd/mt6397-core.c` | one MFD cell `mt6328-accdet` with `DEFINE_RES_IRQ_NAMED(MT6328_IRQ_STATUS_ACCDET, "accdet")`, `of_compatible = "mediatek,mt6328-accdet"` |

No DTS files, no `sound/**`, no `include/linux/mfd/mt6328/core.h` touched. `accdet.c`, `accdet-regs.h`, `Kconfig`, `Makefile` and the header were recorded with `git add -N` so `git diff --binary` contains them.

Nura before: no MT6328 ACCDET support at all (no `drivers/misc/mediatek/` directory, no PMIC ACCDET MFD cell, no jack detection on MT6737T boards).

## 2. ROM / downstream evidence

Downstream source (primary): `src/downstream-grandpplte/...`

- `drivers/misc/mediatek/accdet/mt6735/accdet.c` (2073 lines) — probe 1783, hw init 1368, disable 319, enable 308, irq handler 810, clear irq 881, `check_cable_type` 894, `check_pole_type` 356, ADC read 252, `multi_key_detection` 772, `key_check` 728, `headset_plug_out` 285, DTS parse 1298, efuse 1358, remove 1913, suspend 1931, resume 1942, restore_noirq 1980.
- `.../mt6735/reg_accdet.h` — register offsets/bits (quoted line by line in `accdet-regs.h`).
- `.../mt6735/accdet.h:24,79-103` — `ACCDET_TIME_OUT 0x61A80` (400 µs), `headset_mode_settings`, `accdet_status`.
- `drivers/misc/mediatek/accdet/accdet_drv.c` — platform driver split, PM ops, `of_match_table`.
- `drivers/misc/mediatek/accdet/Kconfig` — downstream tunables (`ACCDET_EINT`, `ACCDET_EINT_IRQ`, `FOUR_KEY_HEADSET`, `PIN_SWAP`, `PIN_RECOGNIZATION`, `TS3A225E`, `SEC_JACK_SYSFS`).
- `drivers/misc/mediatek/power/mt6735/pmic.c:5155-5195` — `pmic_Read_Efuse_HPOffset()` OTP sequence.
- `drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/upmu_hw.h:93,94,114,115,484,492,495-497,612,7106-7117` — OTP/clock register names, `MT6328_AUDENC_ANA_CON9` (0x0D00) mic-bias fields.

ROM configuration and behaviour confirmation:

- `arch/arm/configs/grandpplte_00_defconfig:1402` `CONFIG_ACCDET_EINT=y`, `:1403` `# CONFIG_ACCDET_EINT_IRQ is not set`, `:1406` `CONFIG_FOUR_KEY_HEADSET=y`, `:1404,1405,1407,1408` pin swap / pin recognition / five-pole / TS3A225E off, `:1409` `SEC_JACK_SYSFS=y`.
- ROM strings `zip/vmlinux.strings`: `27055 "[CONFIG_ACCDET_EINT]dump_register:"`, `27112 "[Accdet]ACC EINT func :plug-in"`, `27116 "[Accdet]ACC EINT func:plug-out"`, `27071 "[accdet] pole check:%d mv, AB=%d"`, `27072 "[Accdet]adc cali_voltage1"`, `27133-27137` DT names `headset-mode-setting`, `accdet-mic-vol`, `accdet-plugout-debounce`, `accdet-mic-mode`, `headset-four-key-threshold`, `27166 "state_eint_as_int"`. There is no `headset-three-key-threshold` string (grep count 0), so the ROM parses only the four-key table.
- ROM symbols `zip/symbols.txt:16768-16814`: `accdet_probe`, `accdet_eint_func`, `check_pole_type`, `multi_key_detection`, `accdet_irq_handler`, `accdet_eint_int_handler`, `accdet_int_handler`, `accdet_get_dts_data`, `accdet_pmic_Read_Efuse_HPOffset`, `mt_accdet_probe`, `accdet_detect`, `accdet_get_cable_type`.
- ROM disassembly (`zip/vmlinux.elf`, `aarch64-linux-gnu-objdump -b elf32-littlearm -m arm`):
  - `check_pole_type` @ `0xc0391f38`: ADC read + `cust_vol_set` bound comparisons (`vol < max_4pole+1 && vol > min_4pole-1`, then `max_3pole+1`, else external) — same logic as downstream `accdet.c:356-381`.
  - `accdet_eint_work_callback` @ `0xc03930c8` contains `accdet_init()` inlined and writes `0x0F1A`/`0x0F1C` (PWM width/thresh from `cust_headset_settings[0]/[1]`), `0x0F18 = 0x07`, `0x0F1E` (delays from `[2]/[3]`), `0x029A` (`TOP_RST_ACCDET_SET`) and `0x024A` (`TOP_CKPDN_SET`, i.e. the clock is gated again at the end of init). That is the `CONFIG_ACCDET_EINT` init tail, confirming the ROM build is the EINT variant.
- ROM DTB `zip/rom_dtb_0.dts:4024-4043` (accdet node) and `:2158-2179` (`accdetdefault` empty state; `state_eint_as_int` muxes pins `0x100` = GPIO1 and `0x400` = GPIO4, `slew-rate 0`, `bias-disable`).

Hardware facts used: MT6328 PMIC, ACCDET registers in the parent PMIC regmap (16-bit reg/val, `mtk-pmic-wrap.c:2527-2534`), PMIC IRQ `MT6328_IRQ_STATUS_ACCDET = 10` (`include/linux/mfd/mt6328/core.h`), threaded nested IRQ (`mt6397-irq.c:120-127`).

## 3. Parity matrix

| Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|
| Binding / register access | `accdet_drv.c` platform driver, `pwrap_read/write` on PMIC addresses | none | platform driver on the `mt6328-accdet` MFD cell, `dev_get_regmap(dev->parent)` (16-bit reg/val), no direct pwrap | done |
| Probe DT parsing + defaults | `accdet_get_dts_data()` (`accdet.c:1298`) | none | `accdet_parse_dt()`: `headset-mode-setting`, `accdet-mic-vol`, `accdet-mic-mode`, `accdet-plugout-debounce`, `headset-vol-threshold`, `headset-four-key-threshold`, `headset-three-key-threshold`; defaults = ROM DTB values; bias >2500 forced to 2800 (`accdet.c:1330`) | done |
| HW bring-up order | `accdet_init()` (`accdet.c:1368`): clock ungated (`TOP_CKPDN_CLR` bit9) → `TOP_RST_ACCDET_SET/CLR` → PWM width/thresh (`value-1`) → `STATE_SWCTRL=0x07` → `EN_DELAY_NUM` → DEBOUNCE0/1/3/4 → IRQ clear + `INT_CON_ACCDET_SET` bit12 → `AUDMICBIASVREF` → `ACCDET_RSV=0x1290` → mic-mode branch → ADC 2.8 V test bit → AUXADC auto sample | none | `accdet_hw_init()`, same order/values, cached in `accdet-regs.h` with `accdet.c`/`upmu_hw.h` citations; tail uses the non-EINT `ACCDET_CTRL = ENABLE` (`accdet.c:1451-1453`) | done |
| Power-down sequence | `disable_accdet()` (`accdet.c:319`): mask IRQ enable, clear IRQ, wait status clear, `STATE_SWCTRL=0`, `CTRL=0`, clock gate | none | `accdet_disable()`, same sequence; called on remove, suspend (unplugged) and probe error | done |
| Plug / unplug detection | AP GPIO EINT 4 (level low, 256 ms debounce) → `accdet_eint_func()` + `accdet_eint_work_callback()` | none | PMIC ACCDET comparator interrupt: `ACCDET_STATE_RG[7:6] == 3` means plug-out (`accdet.c:35-38`), threaded handler clears `ACCDET_IRQ_STS` and schedules detection | done, documented substitution (see §5) |
| Mic ADC measurement | `Accdet_PMIC_IMM_GetOneChannelValue()` (`accdet.c:252`): request via `AUXADC_CTL_SET` bit5, 3 ms, poll `AUXADC_REG` bit15, `val*1800/4096`, minus efuse offset | none | `accdet_measure_mic()`: same registers/scale, `usleep_range(3 ms)` then `regmap_read_poll_timeout` (bounded, see §5) | done |
| 3-pole vs 4-pole vs external cable | `check_pole_type()` (`accdet.c:356`) with `headset-vol-threshold` | none | same comparisons on the measured mV, result feeds the downstream state machine (`PLUG_OUT`→`HOOK_SWITCH`/`MIC_BIAS`/`EXT_CABLE`) | done |
| 5-pole headset | `CONFIG_HEADSET_SUPPORT_FIVE_POLE` not set in the ROM; `TYPE_AB_10` is handled as external cable with `cable_type = NO_DEVICE` | none | external cable maps to `EXT_CABLE` + `ACCDET_CABLE_NONE`; `ACCDET_CABLE_HEADSET_5POLE` is never produced | deferred (matches ROM defconfig) |
| Headset buttons (four keys) | `key_check()` (`accdet.c:728`) + `multi_key_detection()` (`accdet.c:772`), reported as input key codes | none | four-key mapping from DT thresholds, indices 1..4 (`accdet.c` key_check port), reported with `ACCDET_EVENT_BUTTON`; three-key mapping used when only that table is described | done |
| State reporting | `switch_set_state()` + input device + `/dev/accdet` ioctl | none | blocking notifier: `ACCDET_EVENT_PLUG` on plug/cable change, `ACCDET_EVENT_BUTTON` on key change, payload `struct accdet_state` (cable, plugged, button, mic ADC mV); `accdet_get_state()` for the boot state; both exported GPL | done |
| Suspend / resume | `mt_accdet_suspend/resume()` only debug-print in the EINT build (`accdet.c:1931-1951`) | none | suspend: IRQ off, work drained, power down when unplugged; resume: replay full init, re-read comparator, notify on change | done (superset, see §5) |
| Hibernation restore | `mt_accdet_pm_restore_noirq()` (`accdet.c:1980`): clock, enable, `IDLE_EN`, read AB, restore cable, disable when no device | none | `accdet_restore_noirq()`: replays full init, re-reads comparator, notifies on change | done (superset, see §5) |
| PMIC OTP headset offset | `accdet_pmic_Read_Efuse_HPOffset()` + `pmic_Read_Efuse_HPOffset(0x10)` (`accdet.c:1358`, `pmic.c:5155`) | none | `accdet_read_efuse_offset()`: exact register sequence, `(efuse >> 8) & 0xff) / 2`; failure only warns, offset stays 0 | done |
| Mic bias reference | `pmic_set_register_value(AUDMICBIASVREF, accdet-mic-vol)` and `AUDMICBIAS1DCSWPEN` per `accdet-mic-mode` | none | `regmap_update_bits(0x0D00, …)` with the `upmu_hw.h` masks (shared register, see §5) | done |
| AP GPIO EINT plug path | `accdet_setup_eint()`: pinctrl, `irq_of_parse_and_map`, `request_irq` | none | not requested; pinctrl `state_eint_as_int` is selected (ROM pin muxing) | deferred (see §5) |
| `debounce` DT property | EINT pin + debounce time, consumed by `accdet_setup_eint()` | none | not parsed; belongs to the deferred EINT path | deferred |
| `/dev/accdet` + ioctls (`ACCDET_INIT`, `SET_CALL_STATE`, `GET_BUTTON_STATUS`), `mt_accdet_unlocked_ioctl` | HAL interface | none | not ported | deferred (excluded by task rules) |
| Samsung `SEC_JACK_SYSFS` jack factory (`jack_factory.c`) | factory test sysfs | none | not ported | deferred (excluded by task rules) |
| `TS3A225E` accessory switch | `ts3a225e.c` (ROM defconfig: off) | none | not ported | deferred |
| Pin swap / pin recognition (`ACCDET_PIN_SWAP`, `ACCDET_PIN_RECOGNIZATION`) | ROM defconfig: off | none | not ported | deferred |
| `send_key_event()` input device (`KEY_PLAYPAUSE`/`VOLUMEUP`/`VOLUMEDOWN`/`VOICECOMMAND`) | input subsystem injection | none | replaced by notifier button events (the audio machine driver maps them to `SND_JACK_BTN_*`) | deferred (by interface contract) |
| 6 s mic-bias power-down timer (`MICBIAS_DISABLE_TIMER`) | disables the unit 6 s after unplug in the EINT build | none | not ported: in this interrupt configuration disabling the unit would also lose plug detection | deferred, documented |
| Call state / `STAND_BY` state | set from the HAL ioctl only | none | not implemented (no consumer) | deferred |

## 4. Compile-tested

```
cd $M/worktrees/audio-accdet
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
./scripts/config --enable SOUND --enable SND --module SND_SOC --module MTK_ACCDET
make ARCH=arm64 LLVM=1 olddefconfig

make ARCH=arm64 LLVM=1 -j2 drivers/misc/mediatek/accdet/accdet.o drivers/mfd/mt6397-core.o
  CC      drivers/mfd/mt6397-core.o
  CC [M]  drivers/misc/mediatek/accdet/accdet.o
  (exit 0)
```

Result: both objects compile with no errors. Re-run with `W=1` after every edit: no warnings from either object (`CONFIG_MTK_ACCDET=m`, `CONFIG_MFD_MT6397=y` confirmed in `.config`).

`accdet.ko` linking was also attempted (`make … drivers/misc/mediatek/accdet/accdet.ko`); modpost cannot resolve kernel symbols because this worktree has no `Module.symvers` (no full kernel build by instruction), so this is expected and not a code problem. Only the `.o` targets were required.

## 5. Inferred or unverified on hardware

1. **Plug/unplug source.** The ROM uses AP GPIO EINT 4 (`rom_dtb_0.dts:4026-4028`, `grandpplte_00_defconfig:1402`, `accdet_setup_eint`). This kernel's MT6735 EINT has no programmable debounce (`drivers/pinctrl/mediatek/pinctrl-mt6735.c:817-822` `mt6735_eint_hw` has no `.db_time`, so `mtk_eint_set_debounce()` returns `-EOPNOTSUPP`, `mtk-eint.c:452-453`), i.e. the ROM's 256 ms plug debounce cannot be reproduced. The driver therefore keeps the ACCDET unit enabled and uses the PMIC ACCDET comparator interrupt for plug/unplug (`STATE_RG[7:6] == 3`), which is the downstream non-EINT configuration. Consequence: the unit is not clock-gated while idle, unlike the ROM. If hardware shows comparator bounce, the fix is to add the deferred GPIO EINT with a software debounce.
2. **ADC polling timeout.** Downstream busy-waits on `ACCDET_AUXADC_REG` bit 15 without a bound (`accdet.c:288-289`). This driver uses `regmap_read_poll_timeout` with 3 ms settle + 100 ms timeout (`ACCDET_ADC_TIMEOUT_US`). The timeout value is `[INFERENCE]`.
3. **OTP ready timeout** 20 ms (`ACCDET_OTP_READY_TIMEOUT_US`) is `[INFERENCE]`; downstream loops without a bound.
4. **ADC offset clamp.** Downstream subtracts the signed efuse offset from an unsigned value (possible wrap); this driver clamps at 0. `[INFERENCE]`, no behaviour change for sane offsets.
5. **Resume/restore replay the whole init sequence.** Downstream `mt_accdet_pm_restore_noirq()` only re-enables the clock/unit (registers survive its IPO-H flow). Replaying `accdet_hw_init()` additionally covers a real PMIC reset across hibernation; all programmed values are the same. `[INFERENCE]` about which registers the PMIC loses.
6. **Suspend power-down** when unplugged is not in the downstream EINT build (it only debug-prints, relying on the EINT to wake). Required by the task; behaviour when a headset is plugged during suspend is preserved by keeping the unit running. Hardware-unverified.
7. **Boot-time state** is read from the comparator right after init (`accdet_sync_state_from_ab()`, the same mapping as downstream `restore_noirq`). Downstream does not run an initial detection at probe, so whether the comparator has settled at that instant is `[INFERENCE]`; if wrong, the first interrupt corrects it.
8. **Mic bias register is shared** with the audio codec: `accdet-mic-vol`/`accdet-mic-mode` write `MT6328_AUDENC_ANA_CON9` (0x0D00), which the codec driver also owns (mic-bias). Writes use masked `regmap_update_bits`, but ordering against codec setup is unverified on hardware.
9. **`AUDMICBIAS1DCSWPEN` cleared with `regmap_update_bits(…, 0)`** assumes the field is writable and not read-only; downstream writes it via `pmic_set_register_value`. Unverified.
10. **`ACCDET_RSV = 0x1290`** is copied verbatim from downstream with its own `TODO: need confirm pull low, 6328 bit[12]=1` comment; not validated.
11. **Key release notification carries `button == 0`** (contract), so the consumer must remember the pressed index; documented in the header.
12. **EFUSE/OTP block access at probe** assumes no other driver uses the OTP engine concurrently (downstream guards with a private mutex; this driver reads once at probe).
13. No hardware is attached; no part of this driver was exercised on a device.

## 6. DTS-FRAGMENT

Apply to the board DTS (e.g. `arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`); the board already includes `<dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h>` and `mt6328.dtsi`. The PMIC side needs no `interrupts` property: the interrupt comes from the `mt6328-accdet` MFD cell (`MT6328_IRQ_STATUS_ACCDET`). `interrupt-parent`/`interrupts`/`debounce` are kept as the ROM hardware description of the (deferred) AP GPIO EINT path and are not parsed by this driver.

```dts
&mt6328 {
	mt6328_accdet: accdet {
		compatible = "mediatek,mt6328-accdet";
		status = "okay";

		pinctrl-names = "default", "state_eint_as_int";
		pinctrl-0 = <&accdet_pins_default>;
		pinctrl-1 = <&accdet_pins_eint_as_int>;

		/* ROM headset-detect EINT (AP GPIO EINT 4); unused by the
		 * PMIC-interrupt configuration of the driver.
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

&pio {
	accdet_pins_default: accdet-default-pins {
		pins {
			pinmux = <PINMUX_GPIO1__FUNC_GPIO1>,
				 <PINMUX_GPIO4__FUNC_GPIO4>;
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
```

Pin set from `rom_dtb_0.dts:2163-2179` (`0x100` = GPIO1, `0x400` = GPIO4, `slew-rate 0`, `bias-disable`). `IRQ_TYPE_LEVEL_LOW` matches ROM `interrupts = <4 8>` (`0x08` = level low).

## 7. Patch path

`$M/patches/audio-accdet.patch` (1365 lines, `git diff --binary`, 8 files, 1300 insertions).
