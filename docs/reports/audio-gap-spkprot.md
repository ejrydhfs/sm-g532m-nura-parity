# Speaker protection / SPKL interrupt / HP DC compensation (audio-gap-spkprot)

Worktree: `$M/worktrees/audio-gap-spkprot` (baseline audio-integration applied and staged)
Kernel: `$M/src/nura-linux` @ `046883c0c`, arm64, MT6737T / MT6328 "grandpplte"
Reference: `$M/src/downstream-grandpplte` (Samsung/MTK GPL kernel)

## 1. Files changed

| File | Change |
|---|---|
| `sound/soc/codecs/mt6331.c` | Class AB + receiver open/close, mode dispatch in the Speaker Amp DAPM path, vendor speaker-protection controls, SPKL threaded IRQs, software-backed Speaker PGA gain, DAC DC-offset apply/restore, HP trim-offset controls |
| `sound/soc/codecs/mt6331.h` | SPK_CON6/SPK_CON12, OC/ISENSE field bits, INT_STATUS2 + SPKL status bits, AFE_DL_DC_COMP_CFG0/1/2 + enable bit |
| `drivers/mfd/mt6397-core.c` | `mt6328_codec_resources[]` = IRQ names `spkl_d`/`spkl_ab`, attached to the existing `mt6328-codec` cell (accdet cell untouched) |
| `include/linux/mfd/mt6397/core.h` | `MT6397_IRQ_NR = 48` (explicit prerequisite, see §5). No existing enumerator value changes |

No new files. No DTS edits.

## 2. Evidence

### Downstream source (`$M/src/downstream-grandpplte`)

Speaker class sequences and open/close dispatch:
- `sound/soc/mediatek/mt_soc_audio_v3/mt_soc_codec_speaker_63xx.c:133-168` `Speaker_ClassAB_Open`
- `.../mt_soc_codec_speaker_63xx.c:170-177` `Speaker_ClassAB_close`
- `.../mt_soc_codec_speaker_63xx.c:179-190` `Speaker_ReveiverMode_Open`
- `.../mt_soc_codec_speaker_63xx.c:192-198` `Speaker_ReveiverMode_close`
- `.../mt_soc_codec_speaker_63xx.c:75-117` / `119-130` class D (baseline, unchanged)
- `.../mt_soc_codec_63xx.c:149-150` `Speaker_mode = AUDIO_SPEAKER_MODE_D`, `Speaker_pga_gain = SPK_D_TRIM_INDEX`
- `.../mt_soc_codec_63xx.c:2030-2126` `Speaker_Amp_Change`: open dispatch `2079-2084`, `Apply_Speaker_Gain()` `2086`, close dispatch `2091-2096`
- `.../mt_soc_analog_type.h:205-207` mode values D=0, AB=1, RECEIVER=2

Controls:
- `.../mt_soc_codec_63xx.c:2678-2685` enum text arrays (`"CALSSD"`, `"CLASSAB"`, `"RECEIVER"`, Off/On)
- `.../mt_soc_codec_63xx.c:2687-2700` `Audio_Speaker_Class_{Set,Get}`
- `.../mt_soc_codec_63xx.c:2703-2711`, `2728-2733` `Audio_Speaker_Pga_Gain_{Set,Get}` (software variable, writes `SPK_ANA_CON0` at `2709` with mask `0x7800`)
- `.../mt_soc_codec_63xx.c:2713-2718` `Audio_Speaker_OcFlag_Get` -> `GetSpeakerOcFlag()`
- `.../mt_soc_codec_63xx.c:2721-2726` `Audio_Speaker_OcFlag_Set` ("not support setting")
- `.../mt_soc_codec_63xx.c:2735-2751` `Audio_Speaker_Current_Sensing_{Set,Get}` (`SPK_CON12` `0x9300`/`0x1300`, mask `0xff00`, get bit 15)
- `.../mt_soc_codec_63xx.c:2754-2775` `Audio_Speaker_Current_Sensing_Peak_Detector_{Set,Get}` (`SPK_CON12` bit 14)
- `.../mt_soc_codec_63xx.c:2785-2796` control registration names
- `.../mt_soc_codec_speaker_63xx.c:200-215` `GetSpeakerOcFlag`: `TOP_CKPDN_CON2_CLR = 0x3` then `SPK_CON6` bits 14 (`SPK_D_OC_L_DEG`) / 15 (`SPK_AB_OC_L_DEG`)
- `.../mt_soc_pcm_routing.c:864-896` `Audio_HpSpkl_Offset_{Get,Set}`, `898-921` `Audio_HpSpkr_Offset_{Get,Set}`, kcontrol names `1023-1026` (`"Audio HPL_SPK Offset"`, `"Audio HPR_SPK Offset"`)

DC-offset compensation (non-efuse path; `EFUSE_HP_TRIM` is not defined for this target):
- `.../mt_soc_codec_63xx.c:621-643` `mHplTrimOffset`/`mHprTrimOffset`/`mHplSpkTrimOffset`/`mHprSpkTrimOffset` defaults `2048`, valid range `1844..2252` (HP) / `1400..2696` (SPK), reset to `2048` on violation
- `.../mt_soc_codec_63xx.c:1071-1083` `SetHprOffset`; `1085-1123` `SetHplOffset` (`DCOFFSET_RAMP_STEP_HPL 3`, ramp only when `EnableRamp`; formula `(Trim * 10589 + 2048) / 4096` stored as `u16`)
- `.../mt_soc_codec_63xx.c:1125-1131` `EnableDcCompensation` -> `AFE_DL_DC_COMP_CFG2` bit 0
- `.../mt_soc_codec_63xx.c:1133-1163` `RestoreDcOffsetToZero` (`DCOFFSET_RAMP_STEP 3`, then CFG0 = CFG1 = 0)
- `.../mt_soc_codec_63xx.c:1166-1180` `SetHprOffsetTrim`/`SetHpLOffsetTrim` (offset = trim - 2048), `1200-1213` `SetDcCompenSation`
- `.../mt_soc_codec_63xx.c:1800` `SetDcCompenSation()` on HP power-up; `1838` `EnableDcCompensation(false)` on HP power-down
- `.../AudDrv_Ana.h:95-97` `AFE_DL_DC_COMP_CFG0 = 0x2032`, `CFG1 = 0x2034`, `CFG2 = 0x2036`

PMIC interrupts:
- `.../power/mt6735/pmic.c:3100-3125` `interrupt_status2[]`, SPKL entries at `3114-3115`, bound to `MT6328_INT_STATUS2`/`INT_CON2`
- `.../include/mt-plat/mt6735/include/mach/upmu_hw.h:2597-2602` INT_CON2 enable bits 13/14; `:2747-2752` INT_STATUS2 status bits 13/14
- `.../include/mt-plat/mt6735/include/mach/upmu_hw.h:5981-5987` SPK_CON6 bits 14/15; `:6107-6112` SPK_CON12 bits 14/15

Nura tree:
- `drivers/mfd/mt6397-irq.c:84-105` `mt6397_irq_handle_reg()` reads a status register, runs the nested handlers and then writes the status back (write-1-to-clear); `:114` dispatches INT_STATUS2 with hwirq base 32; `:219` creates the linear domain with `MT6397_IRQ_NR`
- `include/linux/mfd/mt6328/core.h:49-50` `MT6328_IRQ_STATUS_SPKL_D = 45`, `MT6328_IRQ_STATUS_SPKL_AB = 46` (32 + 13/14)
- `include/linux/mfd/mt6328/registers.h:135-137,143` INT_CON2 / INT_STATUS2 offsets
- `drivers/mfd/mfd-core.c:251-257` maps cell IRQ resources through `irq_create_mapping(domain, resource.start)`
- `kernel/irq/irqdomain.c:674-678` rejects hwirqs >= domain size

ROM confirmation (`$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`):
- symbols `Speaker_ClassAB_Open`, `Speaker_ClassAB_close`, `Speaker_ReveiverMode_Open`,
  `Speaker_ReveiverMode_close`, `GetSpeakerOcFlag`, `Audio_Speaker_OcFlag_Get/Set`,
  `Audio_Speaker_Current_Sensing_Get/Set`, `speaker_OC_function`, `speaker_CS_function`,
  `speaker_CSPeakDetecReset_function`
- strings `RG_INT_STATUS_SPKL_D`, `RG_INT_STATUS_SPKL_AB`, `Audio_Speaker_class_Switch`,
  `Audio_Speaker_OC_Falg`, `Audio_Speaker_CurrentSensing`

## 3. Parity matrix

| Behaviour | ROM implementation | Before | After | Status |
|---|---|---|---|---|
| Speaker class selection (D / AB / receiver) | `Speaker_mode` + `Speaker_Amp_Change` dispatch (`mt_soc_codec_63xx.c:2079-2096`) | Class D only, hardcoded | `Audio_Speaker_class_Switch` SOC_ENUM_EXT (`CALSSD`/`CLASSAB`/`RECEIVER`), invalid values -> `-EINVAL`; Speaker Amp DAPM PRE_PMU/PRE_PMD dispatch the matching open/close sequence | done |
| Class AB open/close | `spk:133-168` / `170-177` | absent | exact register/value sequence, 10 ms trim wait | done |
| Receiver mode open/close | `spk:179-190` / `192-198` | absent | exact sequence incl. 2 ms `udelay` | done |
| Speaker PGA gain | `Speaker_pga_gain` software var + `Apply_Speaker_Gain` (`vendor:149-150, 2086`) | register-backed enum; value lost whenever an open/close sequence rewrote `SPK_ANA_CON0` | existing `Speaker PGA Gain` control kept (same name/16 items), now software-backed like the vendor and immediately written at `vendor:2709`; open sequences apply it afterwards | done |
| OC flag read | `GetSpeakerOcFlag` (`spk:200-215`) | absent | `Audio_Speaker_OC_Falg` (read-only): enables `TOP_CKPDN_CON2_CLR` `RG_SPK_CK_GRP` first, returns SPK_CON6 bit14\|bit15; set returns 0 like vendor | done |
| Current sensing | `SPK_CON12` bit15, `0x9300`/`0x1300` mask `0xff00` | absent | `Audio_Speaker_CurrentSensing` get/set | done |
| Current peak detector | `SPK_CON12` bit14 | absent | `Audio_Speaker_CurrentPeakDetector` get/set | done |
| SPKL_D / SPKL_AB interrupt | `pmic.c:3114-3115` handlers on INT_STATUS2 bits 13/14 | absent | MFD IRQ resources 45/46 named `spkl_d`/`spkl_ab`; `platform_get_irq_byname_optional()` + threaded handler reads INT_STATUS2, logs, latches `spk_d_oc`/`spk_ab_oc` and clears only its own W1C bit | done |
| Probe without SPKL IRQ resource | n/a | n/a | `-ENXIO`/`-EINVAL`/request failure -> warning + continue; only `-EPROBE_DEFER` propagates | done |
| HP DC offset apply at power-up | `SetDcCompenSation()` non-efuse (`vendor:1800, 1200-1213`) | absent | `SetHprOffset(trim-2048)`, `SetHplOffset(trim-2048, ramp=false)`, `EnableDcCompensation(true)` between DAC gain and `AUDDEC_ANA_CON0=0xf49f` | done |
| HP DC offset restore at power-down | `RestoreDcOffsetToZero` + `EnableDcCompensation(false)` (`vendor:1133-1163, 1838`) | absent | signed ramp in steps of 3 on CFG0, CFG0/CFG1 cleared, compensation disabled | done |
| HP trim offset controls | `SetHplTrimOffset`/`SetHprTrimOffset` (`vendor:626-643`), routing controls `mt_soc_pcm_routing.c:864-921` | absent | `Audio HPL_SPK Offset` / `Audio HPR_SPK Offset` (`SOC_SINGLE_EXT`, max `0x20000` as vendor); 1844..2252 accepted, anything else resets to 2048 with `dev_err` | done |
| Automatic OC restart/recovery | none downstream (only `GetSpeakerOcFlag` polling) | none | none (explicitly not invented) | deferred by design |
| Plain headphone trim controls (`Audio HPL/HPR Offset`) | `mt_soc_pcm_routing.c:926-1000` | absent | not requested by this task | out of scope |

Note on control naming: the task names the handlers (`Audio_HpSpkl_Offset_Set/Get`); the kcontrol names are the vendor userspace names `Audio HPL_SPK Offset` / `Audio HPR_SPK Offset` used by `mt_soc_pcm_routing.c:1023-1026`, so the Android HAL keeps working.

## 4. Compile commands and result

```
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp $M/nura.config .config
./scripts/config --enable SOUND --module SND --module SND_SOC \
                 --module SND_SOC_MEDIATEK --module SND_SOC_MT6331
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 sound/soc/codecs/mt6331.o \
                           drivers/mfd/mt6397-core.o \
                           drivers/mfd/mt6397-irq.o
```

Result after a clean rebuild (objects removed first):

```
  CC      drivers/mfd/mt6397-core.o
  CC [M]  sound/soc/codecs/mt6331.o
  CC      drivers/mfd/mt6397-irq.o
EXIT=0
```

(`CONFIG_SOUND=y`, `CONFIG_SND=m`, `CONFIG_SND_SOC=m`, `CONFIG_SND_SOC_MT6331=m`, `CONFIG_MFD_MT6397=y`.)
Two intermediate failures were fixed before this result: this tree has no
`snd_soc_kcontrol_component()` helper (used `snd_kcontrol_chip()` as other codecs in the
tree do) and `platform_get_irq_byname_optional()` takes `struct platform_device *`.
No warnings remain. Only the listed objects were built; no full kernel build was requested.

## 5. Inferred / unverified items and cross-file dependency

1. **IRQ domain size (prerequisite, included in the patch).**
   `mt6397-irq.c:114` dispatches INT_STATUS2 at hwirq base 32 and
   `mt6328/core.h` places SPKL_D/SPKL_AB at 45/46, but `MT6397_IRQ_NR` was 32, so
   `irq_create_mapping()` rejected both hwirqs (`kernel/irq/irqdomain.c:674`) and
   mfd-core handed the codec cell IRQ resources with `start = 0`. The patch raises
   `MT6397_IRQ_NR` to 48. No enumerator with a value is renumbered (the new value is
   only the terminator), and `MT6397_IRQ_NR` is used only at
   `drivers/mfd/mt6397-irq.c:219`. The domain is shared by MT6323/MT6328/MT6331/MT6397;
   the extra 16 hwirqs are unused on the other chips (they only expose INT_STATUS0/1)
   and both MT6397 users of the domain are name-based (`accdet`, keys, RTC), so no
   existing IRQ index changes meaning. `mt6397-core.o` and `mt6397-irq.o` were
   recompiled with the change. This was approved by the coordinator before inclusion.
2. `0x9300` / `0x1300` for `SPK_CON12` and the masks (`0xff00`, bit 14) are taken
   verbatim from the vendor driver; the individual REFSEL/GAINSEL field values they
   encode were not independently decoded (not needed for parity).
3. `MT6328_INT_STATUS2 = 0x02c8` is defined locally in `mt6331.h` (value matches
   `include/linux/mfd/mt6328/registers.h:143`) instead of including the MT6328 register
   header, to avoid duplicating/renaming the addresses already local to this driver.
4. Interrupt clearing semantics verified by reading `mt6397_irq_handle_reg()`: the nested
   handler runs before the PMIC thread writes the status snapshot back, so the handler
   sees the set bits; it clears only its own bit (W1C) so other pending INT_STATUS2
   sources are unaffected.
5. The DAC DC-offset trim formula and ramp constants are ported exactly, including the
   `u16` wraparound for negative offsets; the HP power-down restore is the task-mandated
   combination of `RestoreDcOffsetToZero()` (vendor speaker-close path) and
   `EnableDcCompensation(false)` (vendor HP power-down path).
6. Register/bits derived rather than read: none. Every write in the new code is a
   literal from the downstream files cited in §2; enum text order is taken from the
   vendor tables.
7. No automatic over-current recovery is implemented; downstream only exposes the flag.

## 6. DTS-FRAGMENT

None required. The SPKL interrupts are provided by the `mt6328-codec` MFD cell
(`DEFINE_RES_IRQ_NAMED(... "spkl_d"/"spkl_ab")`) through the PMIC's own irq domain, and
the codec obtains them with `platform_get_irq_byname_optional()`; no DT property is read
for speaker protection, class selection, current sensing or DC compensation. The driver
continues to probe when those resources are absent. No shared `.dtsi`/`.dts` was touched.

## 7. Patch

`$M/patches/audio-gap-spkprot.patch` (844 lines, 4 files; generated with
`git diff --binary` against the staged baseline and verified with
`git apply --check -R`).
