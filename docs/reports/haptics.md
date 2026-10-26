# Haptics parity report — SM-G532M ROM vs Nura `regulator-haptic` (worktree `rtc-haptic`)

## 1. Assigned driver and files changed

Assigned: timed-output/vibrator parity for the MT6328 `vibr` PMIC LDO + haptic motor
(`drivers/input/misc/regulator-haptic.c`, `drivers/regulator/mt6328-regulator.c`).

Changed (worktree `$M/worktrees/rtc-haptic`):

- `drivers/input/misc/regulator-haptic.c` — added a maximum continuous run-time cap
  (port of the ROM driver's `sec_vib,max_timeout` clamp) with an `hrtimer` watchdog,
  new `remove` callback, and an optional `max-timeout-ms` DT property.
- `Documentation/devicetree/bindings/input/regulator-haptic.yaml` — documents
  `max-timeout-ms`.

Audited, **no change required** (already at parity, evidence in §3):

- `drivers/regulator/mt6328-regulator.c` (VIBR rail register programming).
- `arch/arm64/boot/dts/mediatek/mt6328.dtsi` (`mt6328_vibr_reg` node; shared file,
  byte-equal to the ROM DTB node, not touched).
- `arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts` (`haptics` node;
  shared file — see DTS-FRAGMENT in §6).

No Android-only glue exists in the Nura tree (`timed_output`, `vibr_on` sysfs, etc.
are absent from `drivers/` and `include/`), so there was nothing to remove; the
standard regulator-haptic / input FF_RUMBLE API is kept.

## 2. ROM evidence

### 2.1 The ROM ships Samsung `sec_vib`, not the MTK `vibrator_drv.c`

ROM `vmlinux.elf` symbols (`$M/zip/symbols.txt`, addresses from `llvm-nm`):

| symbol | address |
|---|---|
| `sec_vib_enable` | `0xc07c8810` |
| `sec_vib_get_time` | `0xc07c8900` |
| `sec_vib_timer_func` | `0xc07c87e4` |
| `sec_vib_probe` | `0xc07c8964` |
| `sec_vib_remove` | `0xc07c87a0` |
| `sec_vib_vdd_en` | `0xc07c8c34` |
| `sec_vib_work` | `0xc07c8cb0` |
| `sec_vib_init` | `0xc0dc2ff0` |
| `timed_output_dev_register` / `timed_output_init` / `create_timed_output_class` | present |

There are **no** `vibr_Enable_HW`, `vibr_power_set`, `vib_mod_init`, `mtk_vibrator`
symbols — the MTK driver `downstream-grandpplte/drivers/misc/mediatek/vibrator/vibrator_drv.c`
(and its HAL `mt6735/vibrator.c`) is **not built into the ROM**. The function set
matches `downstream-grandpplte/drivers/motor/sec_vib.c` exactly, which is therefore
the primary behavioral reference; `vibrator_drv.c` is kept as a secondary reference
as assigned.

ROM strings (`$M/zip/vmlinux.strings`): `sec_vib,max_timeout`, `sec_vib,ldo_type`,
`sec_vib,regulator`, `sec_vib,ldo_en`, `sec_vib`, `vibrator` — the `sec_vib.c` DT
property strings.

ROM disassembly of `sec_vib_enable` (`aarch64-linux-gnu-objdump -d --disassemble=sec_vib_enable
$M/zip/vmlinux.elf`), matching `sec_vib.c:105-131`:

```
c07c8848: bl   c00a50f0 <hrtimer_cancel>
c07c884c: cmp  r5, #0            ; value
c07c8850: ble  c07c88e0          ; value <= 0 -> timeout = 0, queue_work (disable)
c07c8854: ldr  r2, [r4, #104]    ; ddata->max_timeout
c07c8864: cmp  r5, r2
c07c8868: movge r5, r2           ; value = min(value, max_timeout)
c07c8870: str  r5, [r4, #108]    ; ddata->timeout = value
c07c8874: bl   c003a840 <queue_work_on>
c07c88c8: bl   c00a4fa0 <hrtimer_start>
```

### 2.2 ROM device tree (extracted, `$M/zip/rom_dtb_0.dts`)

```
4677  sec_vib {
4678      compatible = "sec_vib";
4680      sec_vib_pdata {
4682          sec_vib,max_timeout = <0x2710>;   /* 10000 ms */
4683          sec_vib,ldo_type = <0x00>;        /* INTERNAL_LDO: PMIC regulator */
4684          sec_vib,regulator = "vibr";       /* MT6328 VIBR LDO */
4324  ldo_vibr {
4325      regulator-name = "vibr";
4326      regulator-min-microvolt = <0x325aa0>; /* 3300000 */
4327      regulator-max-microvolt = <0x325aa0>; /* 3300000 */
4328      regulator-enable-ramp-delay = <0x2c>; /* 44 us */
4329      regulator-default-on = <0x00>;
```

### 2.3 Downstream source citations

`drivers/motor/sec_vib.c` (the shipped driver):

- `sec_vib_enable()` (lines 105-131): `hrtimer_cancel`, `if (value > ddata->max_timeout)
  value = ddata->max_timeout` (114-115), start `hrtimer` for `value` ms (121-124);
  `value <= 0` → `timeout = 0` + `queue_work` (133-136). No minimum pulse.
- `sec_vib_work()` (133-152): caches `running`, calls `sec_vib_vdd_en()` only on
  off→on / on→off transitions (142-148).
- `sec_vib_vdd_en()` (57-81): `regulator_enable()`/`regulator_disable()` for
  `INTERNAL_LDO` (64, 72). Never touches voltage.
- DT parse of `sec_vib,max_timeout` (181); `platform_driver` has only
  `.probe`/`.remove` (309-320) — **no suspend and no shutdown callback**.

Secondary reference `drivers/misc/mediatek/vibrator/vibrator_drv.c` (not in ROM):

- min-pulse bump `value > hw->vib_limit && value < hw->vib_timer` → `vib_timer`
  (146-150; DTS: `vib_timer = <25>`, `vib_limit = <9>` —
  `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_02.dts:68-72`), hard cap 15000 ms (153).
- `vibr_power_set()` at module init (263) writes `PMIC_RG_VIBR_VOSEL` to `vib_vol`
  (DTS `<5>`) — `mt6735/vibrator.c:110-118`.
- `vibr_Enable_HW`/`vibr_Disable_HW` → `pmic_set_register_value(PMIC_RG_VIBR_EN, 1/0)`
  (`mt6735/vibrator.c:33-40`).
- `.shutdown = vib_shutdown` disables the rail (188-199, 211).

PMIC register map (`include/mt-plat/mt6735/include/mach/upmu_hw.h`):

- `MT6328_VIBR_CON0 = base+0x0A38` (420): `RG_VIBR_EN` mask 0x1 shift 1 (5342-5344),
  `RG_VIBR_MODE_SET` shift 0 (5339-5341), `QI_VIBR_EN` shift 15 (5369-5371).
- `RG_VIBR_VOSEL`: `MT6328_DLDO_ANA_CON2 = base+0x0A6E` (447), mask 0x7 shift 4 (5708-5710).
- Voltage table `mt6328_VCAM_AF_voltages` = {1.2, 1.3, 1.5, 1.8, 2.0, 2.8, 3.0, 3.3} V
  (`power/mt6735/pmic.c:1047-1056`), used for `VIBR` (pmic.c:1145).

## 3. Parity matrix

| Behavior | ROM implementation | Nura before | Nura after | Status |
|---|---|---|---|---|
| Userspace interface | timed_output `"vibrator"`, `enable(ms)` (Android only) | input FF_RUMBLE (ff-memless) | unchanged | done (per assignment: keep standard input API) |
| Enable/disable sequencing | work + `running` cache; `regulator_enable/disable` only on transitions (`sec_vib_work` 142-148) | work + `active` cache in `regulator_haptic_toggle`; voltage before enable | unchanged | done (already at parity) |
| Voltage | rail fixed at 3.3 V (ROM DTB `ldo_vibr` min=max=3300000); `sec_vib` never writes VOSEL | DT `min/max-microvolt = 3300000` → constant 3.3 V per play | unchanged | done (already at parity) |
| VOSEL / rail register programming | via `regulator` core on the "vibr" LDO | `mt6328-regulator.c:375-377`: EN `VIBR_CON0` BIT(1), VOSEL `DLDO_ANA_CON2` 0x70, QI BIT(15), MODE_SET BIT(0); table `ldo_volt_table9` (mt6328-regulator.c:177-179) == downstream `mt6328_VCAM_AF_voltages` | unchanged | done (verified match, no patch) |
| Ramp delay | `regulator-enable-ramp-delay = <44>` (ROM DTB 0x2c) | `mt6328.dtsi:226-231` = 44 µs, node identical to ROM DTB | unchanged | done |
| Timeouts | every `enable()` clamped to `sec_vib,max_timeout` = 10000 ms (ROM DTB 0x2710; `sec_vib_enable` 114-115 + disassembly `movge r5, r2`) | no limit; an effect with `replay.length = 0` could hold the motor on indefinitely | continuous regulator-on time capped by an `hrtimer`; value from DT `max-timeout-ms` (10000 in the DTS fragment); motor held off until the effect stream goes quiet | done (see divergence note below) |
| Suspend | none (`sec_vib` has no PM ops) — motor keeps running through suspend until the timer fires after resume | suspend: rail off; resume: restore if magnitude non-zero | unchanged | partial (deliberate deviation, see §5) |
| Shutdown / unload | none (`sec_vib`); MTK driver disables in `.shutdown` | none | `.remove` cancels the watchdog, drains work, disables the rail | done |
| Min pulse width | none (`sec_vib`); MTK driver bumps 10–24 ms to 25 ms (`vibrator_drv.c:146-150`) | none | none | deferred (see §5) |

Timeout-cap divergence note: the ROM clamp is per `enable(ms)` request. The
ff-memless core never passes the effect duration to the driver (it hands
`play_effect` a synthesized combo with `replay.*` zeroed — `drivers/input/ff-memless.c`,
`ml_play_effects()`/`ml_get_combo_effect()`), so the limit is implemented as a cap
on *continuous* regulator-on time: armed on each off→on transition; at expiry the
rail is switched off and held off until the FF stream reports zero magnitude. For
single-effect playback (the only shape the ROM's timed_output API can express) the
observable behavior is identical. Corner case: with *overlapping* FF effects, a new
effect started while the cap is latched stays suppressed until all effects end,
whereas timed_output would grant a fresh 10 s window per `enable()` call.

## 4. Compile test

From `$M/worktrees/rtc-haptic`:

```sh
export PATH="$M/bin:$PATH"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/input/misc/regulator-haptic.o
```

Result:

```
CC [M]  drivers/input/misc/regulator-haptic.o
```

Clean, no warnings. `drivers/regulator/mt6328-regulator.c` is unchanged and was not
rebuilt. (First build attempt failed with `hrtimer_init` undeclared — this tree uses
the `hrtimer_setup()` API; the code was fixed to `hrtimer_setup()` and rebuilt.)

## 5. Inferred / hardware-unverified

- `[INFERENCE]` The ROM runs the `vibr` rail at 3.3 V: `sec_vib` never writes
  `RG_VIBR_VOSEL`, so the effective voltage is the PMIC reset/boot default unless
  the downstream regulator core applies the DT constraints (min=max=3300000) at
  registration. Nura programs 3.3 V explicitly via `regulator_set_voltage()`, which
  matches the ROM DTB constraint; the actual ROM register value is unverified.
- The MTK HAL's `vib_vol = <5>` (2.8 V) is **not** what ships — that driver is not
  in the ROM. If hardware testing shows the motor is over/under-driven at 3.3 V,
  the fix is in the shared DTS (`min/max-microvolt`) and/or the MT6328 VOSEL table,
  not in this patch.
- VOSEL table entries 0..4 for VIBR conflict between the two downstream tables
  (`mt6735/vibrator.c:42-51` comment: 1.3/1.5/1.8/2.0/2.5 V vs
  `pmic.c:1047-1056`: 1.2/1.3/1.5/1.8/2.0 V). Only code 7 (3.3 V) is used on this
  board; unverified for other voltages.
- Timeout cap: hrtimer accuracy + workqueue latency on real hardware unverified.
  The latched-hold-off semantics for overlapping effects (see §3) are unverified
  against any real workload; the ROM API cannot express that case.
- Suspend/resume: ROM keeps the motor state across suspend; Nura switches the rail
  off at suspend and restarts playback (with a fresh cap window) at resume. This is
  the standard regulator-haptic behavior and avoids an unattended motor during
  suspend, but it is a deliberate deviation from the ROM; unverified on hardware.
- Min pulse width: the MTK driver bumps 10–24 ms pulses to 25 ms; the ROM-shipped
  `sec_vib` driver does not. Not implemented (ROM parity wins). If short taps prove
  inaudible on hardware, add a minimum on-time in `regulator-haptic.c` as a follow-up.

## 6. DTS-FRAGMENT

Shared DTS was not modified. To enable the timeout cap on this board, add one
property to the existing `haptics` node in
`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts` (10000 ms = ROM DTB
`sec_vib,max_timeout = <0x2710>`):

```dts
	haptics {
		compatible = "regulator-haptic";
		haptic-supply = <&mt6328_vibr_reg>;
		max-microvolt = <3300000>;
		min-microvolt = <3300000>;
		max-timeout-ms = <10000>;
	};
```

Only `max-timeout-ms = <10000>;` is new; the rest matches the current node exactly.
Without the property the driver applies no time limit (unchanged behavior for the
other boards using `regulator-haptic` in this tree). No change is needed for
`mt6328.dtsi` — its `mt6328_vibr_reg` node (min=max=3300000, ramp 44 µs) is already
byte-identical to the ROM DTB `ldo_vibr` node.

## 7. Patch

`$M/patches/haptics.patch` (`git diff --binary` from `$M/worktrees/rtc-haptic`).
