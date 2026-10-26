# audio-gap-hpimp — PMIC efuse HP offset + HP impedance / AUXADC + SRAM playback

Worktree: `$M/worktrees/audio-gap-hpimp` (baseline `audio-integration.patch`
applied and staged; the diff below is only this task's changes).

## 1. Files changed

| File | Change |
|---|---|
| `sound/soc/codecs/mt6331.c` | efuse HP-offset read/decode/log (`mt6331_efuse_hp_offset_read`), called at component probe and retried from the first DL power-up; HP impedance analog sequence (`mt6331_open_analog_trim_hw`, trim mux/gain, trim buffer, HP gain, SDM level, `mt6331_open_hp_impedance_setting`), one-shot PMIC AUXADC read (`mt6331_auxadc_read`), offset calibration, and the three controls `Audio HP ImpeDance Setting` / `Audio_HP_ImpeDance` / `Audio AUXADC Data`. Adds the codec DAI `mt-soc-codec-hp-impedance-dai` (downstream `MT_SOC_CODEC_HP_IMPEDANCE_NAME`) so the HP impedance link programs `priv->dl_rate`/`MT6328_AFE_PMIC_NEWIF_CFG0` before the analog sequence powers the downlink. |
| `sound/soc/codecs/mt6331.h` | New register/bit constants: `AUDDEC_ANA_CON2/3`, OTP controller + row/trigger/ready bits, AUXADC ADC/RQST/CON0/CON2/CON12 + bits, trim mux/gain/buffer bits, `AUDDEC_ANA_CON4` trim-buffer bit, SDM levels, trim mux enum values, AUXADC channel/poll constants. |
| `sound/soc/mediatek/mt6735/mt6735-dai-hpimp.c` | **New.** Playback-only HP impedance FE DAI (`mt-soc-hpimpedancedai-driver`, stream `HP_IMPEDANCE_Playback`). `hw_params` rebases the DL1 memory interface onto the AFE internal SRAM (`0x11221000..0x11228fff`); `prepare` applies the vendor fetch format / output connection width / I2S DAC / DL1 rate+channels / I05→O03, I06→O04; `trigger` enables and tears down the DL1 path. Refuses DL1 when another stream holds it. |
| `sound/soc/mediatek/mt6735/mt6735-dai-pcm.c` | The six register helpers used by the new DAI are now non-static (`mt6735_afe_playback_enabled`, `mt6735_afe_memif_enable`, `mt6735_afe_connect`, `mt6735_afe_set_conn_width`, `mt6735_afe_dac_prepare`, `mt6735_afe_dac_disable`). Normal DL1 startup reserves `memif[DL1].substream` under `hp_imp_lock` and returns `-EBUSY` while the HP impedance stream holds DL1; shutdown releases it under the same lock. |
| `sound/soc/mediatek/mt6735/mt6735-afe-common.h` | `MT6735_AFE_DAI_HPIMP`; HP-impedance state (`hp_imp_lock`, `hp_imp_open`) in `struct mt6735_afe_priv`; helper prototypes matching the definitions in `mt6735-dai-pcm.c`; `mt6735_afe_hp_imp_dai`. |
| `sound/soc/mediatek/mt6735/mt6735-afe-pcm.c` | Component control `HP Impedance DC Value` that fills the AFE internal SRAM at `base + 0x1000` with the 16-bit word, now only while the HP impedance FE stream is open (otherwise `-EBUSY`); mutex init; registers the HP impedance DAI. |
| `sound/soc/mediatek/mt6735/mt6735-afe-regs.h` | `MT6735_AFE_INTERNAL_SRAM_OFFSET` (0x1000) and `MT6735_AFE_INTERNAL_SRAM_SIZE` (0x8000). |
| `sound/soc/mediatek/mt6735/mt6735-mt6331.c` | Machine link `HP_IMPEDANCE` / `HP_IMPEDANCE_Playback`, playback-only, CPU DAI `mt-soc-hpimpedancedai-driver`, codec DAI `mt-soc-codec-hp-impedance-dai`. |
| `sound/soc/mediatek/mt6735/Makefile` | Build `mt6735-dai-hpimp.o` into `snd-soc-mt6735-afe`. |

No shared DTS/DTSI file was touched (see §6).

## 2. Evidence

All downstream paths below are relative to
`$M/src/downstream-grandpplte/`.

### 2.1 EFUSE_HP_TRIM finding (explicit)

```
$ grep -rn EFUSE_HP_TRIM src/downstream-grandpplte
sound/soc/mediatek/mt_soc_audio_v3/mt_soc_pcm_common.h:106:/* #define EFUSE_HP_TRIM */
sound/.../mt_soc_codec_63xx.c:1127,1202,1217 (all #ifndef EFUSE_HP_TRIM)
sound/.../mt_soc_pcm_routing.c:867,888,901,922,935,956,969,990 (#ifndef)
$ grep -rn -- -DEFUSE_HP_TRIM src/downstream-grandpplte   -> no matches
```

The symbol is **never defined**: the only definition is commented out in
`mt_soc_pcm_common.h:106` and no Makefile/Kconfig adds `-DEFUSE_HP_TRIM`.
Consequently `SetDcCompenSation()` (`mt_soc_codec_63xx.c:1200-1213`) always
compiles the software-ramp branch (`:1202-1204`, `SetHprOffsetTrim` /
`SetHpLOffsetTrim` / `EnableDcCompensation`) and the decoded efuse fields are
supplied only to the `pr_warn` lines (`:553-561`); `AUDDEC_ANA_CON2` is never
programmed from them. This port keeps that parity: the eight fields are read,
decoded, stored in `mt6331_priv.hp_trim` and logged, and are **not** written to
`AUDDEC_ANA_CON2`.

### 2.2 Efuse read sequence

Downstream `mt_soc_codec_63xx.c:484-566` `Auddrv_Read_Efuse_HPOffset()`:

| Step | Downstream line | Register write (PMIC) | New code |
|---|---|---|---|
| enable efuse ctrl clock | 495-497 | `0x027c = 0x0040` (`TOP_CKHWEN_CON0_CLR`), `0x0252 = 0x0004` (`TOP_CKPDN_CON3_CLR`) | `regmap_write()` ×2 |
| enable engine | 500 | `0x0c16` bit0 (`OTP_CON11`) | `regmap_update_bits()` |
| row select, rows 0xa..0xc | 510-515 | `0x0c00` bits[5:1] = row | `regmap_update_bits()` |
| toggle read trigger | 517-525 | read/toggle `0x0c10` bit0 (`OTP_CON8`) | `regmap_read()` + update |
| wait ready | 526-531 | poll `0x0c1a` bit0 == 0 (`OTP_CON13`) | `regmap_read_poll_timeout()`, 1 ms period, 20 ms timeout (downstream is unbounded) |
| >=1 ms settle | 533 | `udelay(1000)` | `usleep_range(1000, 1100)` |
| read word | 535 | `upmu_get_reg_value(0x0c18)` (`OTP_CON12`) | `regmap_read()` |
| disable clock | 541-543 | `0x0250 = 0x0004`, `0x027a = 0x0040` | `regmap_write()` ×2 |
| decode | 545-552 | bit fields, see below | identical expressions |

Decode (verbatim from `:545-552`):

```
hpl      = (efuse[0] >> 9)  & 0xf
hpr      = ((efuse[0] >> 13) & 0x7) | ((efuse[1] & 0x1) << 3)
hpl_fine = (efuse[1] >> 2)  & 0x3
hpr_fine = (efuse[1] >> 4)  & 0x3
hpl_spkhp      = (efuse[1] >> 6)  & 0xf
hpr_spkhp      = (efuse[1] >> 10) & 0xf
hpl_fine_spkhp = (efuse[1] >> 14) & 0x3
hpr_fine_spkhp =  efuse[2]        & 0x3
```

Cross-check: the same sequence appears in
`drivers/misc/mediatek/power/mt6735/pmic.c:5155-5195`
(`pmic_Read_Efuse_HPOffset`) and the identical register names/bits are already
used by the baseline accdet driver for row 0x10
(`drivers/misc/mediatek/accdet/accdet-regs.h:40-52` region,
`accdet.c:1360-1370`).

ROM symbols (`$M/zip/vmlinux.elf`): `Auddrv_Read_Efuse_HPOffset @ 0xc0860b78`.

Call site parity: downstream `InitAfeControl()` calls the read on the first
audio open — `mt_soc_afe_control.c:457-470`, call at `:468`.
Port deviation (documented): the worktree performs the read once at codec
component probe so the values can be logged once with `dev_info`, and
`mt6331_dl_power_up()` calls the same function again (no-op when already
valid) to keep the downstream first-DL-power-up call site structurally
present and to retry a failed probe read.

### 2.3 HP impedance analog sequence

| Vendor function | Lines | Registers (all via `Ana_Set_Reg`) |
|---|---|---|
| `OpenAnalogTrimHardware(true)` | `mt_soc_codec_63xx.c:748-805` | `TurnOnDacPower()`; `AUDNCP_CLKDIV_CON1/2/0/4/3` = 1/0x2b/1/0/0; `AUDDEC_ANA_CON6` = 0x0a41→0x2ac1 (mask 0xfeeb); `CON7` bit15; `ZCD_CON0` = 0x0400; `CON0` = 0xe080; `CON6` = 0x2ac0; `CON4` = 0x1f01; `CON5` = 0x5490; `ZCD_CON2` = 0x0408; `ZCD_CON3` = 0x001f; `CON1` = 0x0480, 0x1480, 0x14a0; `CON0` = 0xe090; `CON6` = 0x2ac2; `CON0` = 0x0009 then 0x0006 |
| `OpenAnalogTrimHardware(false)` | `:807-818` | `CON4`=0, `CON0`=0, `CON6`=0x2ac0, `CON7` bit15=0, `CON6`=0x0001, `TurnOffDacPower()` |
| `setOffsetTrimMux` | `:610-614` | `AUDDEC_ANA_CON3` bits[4:1] = mux |
| `setOffsetTrimBufferGain` | `:616-619` | `AUDDEC_ANA_CON3` bits[6:5] = gain |
| `EnableTrimbuffer` | `:666-679` | `AUDDEC_ANA_CON4` bit0, `AUDDEC_ANA_CON3` bit0 |
| `OpenHeadPhoneImpedanceSetting(true)` | `:887-958` | `AUXADC_CON2` bit9; `AUDENC_ANA_CON9` bit4; NCP CON4=3/CON1=1/CON2=0x2b/CON0=1/CON3=0; `CON6` 0x0841→0x28c1→0x2ac1; `CON7` bit15; `ZCD_CON0`=0; `CON0`=0xe080; `CON1` bit10=0, bit7=0; `CON3` bit7; `CON6` bit0=0, bit1=1; `CON0`=0xe089; `CON3` bits[11:8]=0x9 |
| `OpenHeadPhoneImpedanceSetting(false)` | `:961-984` | inverse of the above, then `TurnOffDacPower()` |
| `setHpGain(8)` | `:1059-1064` → `HeadsetVoloumeRestore` `:1673-1703` | `ZCD_CON2` bit-by-bit ramp (mask 0xf9f), `udelay(200)` per step |
| `SetSdmLevel` | `:1065-1068` | `AFE_DL_SDM_CON1` (PMIC 0x2008) = level; 0x1e = normal, 0 = mute (`mt_soc_analog_type.h:216-218`) |

Trim mux enum values: `mt_soc_analog_type.h:221-231` (`GROUND` = 9, `HPR` = 2).

`TurnOnDacPower()` (`:1622-1648`) programs the PMIC downlink interface from
`mBlockSampleRate[AUDIO_ANALOG_DEVICE_OUT_DAC]`: `AFE_PMIC_NEWIF_CFG0` and
`AFE_DL_SRC2_CON0_H` at `:1641-1644`. That variable is written by the codec DAI
startup/prepare (`mt63xx_codec_startup`/`mt63xx_codec_prepare`, `:1308-1347`),
which is why the HP impedance link must bind a real codec DAI (§2.8).

**Discrepancy (explicit):** the body of `OpenHeadPhoneImpedanceSetting()`
(`:880-989`) is enclosed in `#if 0` in the vendor source, so the shipped
function only logs/returns `true`; the sequence above is the intended body and
is what this port implements. Its `GetDLStatus()` early return (`:884-886`)
has no equivalent in this split driver; userspace must not enable/disable the
analog path while a normal DL stream is active (reported as a limitation, §5).

ROM symbols: `OpenAnalogTrimHardware @ 0xc086262c`,
`OpenHeadPhoneImpedanceSetting @ 0xc0860ffc`, `setHpGain @ 0xc08611fc`,
`SetSdmLevel @ 0xc086122c`, `Audio_HP_ImpeDance_Set @ 0xc0866260`,
`Audio_HP_ImpeDance_Get @ 0xc0866558`,
`Audio_AuxAdcData_Get @ 0xc0862f7c`,
`Audio_AuxAdcData_Get_ext @ 0xc0862f3c`,
`Audio_AuxAdcData_Set @ 0xc085dc80`.

Vendor control names (ROM strings `$M/zip/vmlinux.strings`):
`Audio HP ImpeDance Setting` @ 59622, `Audio AUXADC Data` @ 59545,
`Audio_HP_ImpeDance_Set` @ 9105. The control registration in the vendor is
`mt_soc_pcm_hp_impedance.c:607-610` and `mt_soc_codec_63xx.c:2836-2839`.

### 2.4 AUXADC channel 9

Same PMIC AUXADC block as the baseline accdet driver — verified by register
identity, not assumed:

* accdet uses `0x0e8a` (RQST0_SET), `0x0e0a` (ADC5), `0x0eca`
  (`accdet-regs.h:40-43`); channel 5 is the ACCDET channel
  (`pmic_auxadc.c:184-200`).
* channel 9 uses the same request register and the next channel output
  register (`upmu_hw.h:628, 688, 693, 707`).

Downstream `drivers/misc/mediatek/power/mt6735/pmic_auxadc.c:173-455`
`PMIC_IMM_GetOneChannelValue()`:

| Step | Line | Register |
|---|---|---|
| channel range check | 207 | `> 15` → error |
| AUXADC clock on | 212 | `AUXADC_CON0` (0x0e94) bit15 (`upmu_hw.h:7694-7696`) |
| input buffer on | 213 | `AUXADC_CON12` (0x0eb0) bit7 (`upmu_hw.h:7838-7840`) |
| AP-mode 26 MHz | 214 | `TOP_CLKSQ` (0x028a) bit8 (`upmu_hw.h:2285-2287`) |
| request one conversion | 222 | `AUXADC_RQST0_SET` (0x0e8a) = `BIT(channel)` |
| poll ready (ch9) | 337-347 | `ADC9` (0x0e12) bit15 (`upmu_hw.h:7202-7204`) |
| extract code (ch9) | 347, 434-436 | `ADC9` bits[11:0]; ch9 returns the **raw 12-bit code**, not millivolts |

ROM: `PMIC_IMM_GetOneChannelValue @ 0xc03a28cc`,
`Accdet_PMIC_IMM_GetOneChannelValue @ 0xc0391e6c`.

The new helper mirrors this sequence. The downstream poll is bounded by
`count_time_out = 15` iterations of 1 ms (`pmic_auxadc.c:88, 338-345`); the
port uses `regmap_read_poll_timeout(..., 1000 us, 20000 us)`.

### 2.5 AFE internal SRAM

* `AudDrv_Afe.h:375-377`: `AFE_INTERNAL_SRAM_PHY_BASE 0x11221000`,
  `AFE_INTERNAL_SRAM_SIZE 0x8000`.
* `AudDrv_Afe.c:118-129`: `Get_Afe_Sram_Phys_Addr()` returns
  `0x11221000`, `Get_Afe_SramBase_Pointer()` returns
  `ioremap_nocache(0x11221000, 0x8000)` (`:99`).
* `mt_soc_pcm_hp_impedance.c:361-362` and `:377-378` fill it with the
  userspace-supplied value.
* The AFE driver's `devm_platform_ioremap_resource()` covers
  `0x11220000..0x1122ffff` (`MT6735_AFE_SIZE 0x10000`), so the SRAM is
  `base + 0x1000`.

### 2.6 Which buffer is played, and the DL1 SRAM path

This was re-checked from source because the two downstream buffers are not the
same object:

* `AudDrv_Def.h:45` defines **`AUDIO_MEMORY_SRAM`** and `:46`
  `AUDIO_MEM_IOREMAP` in the shipped build.
* Under those macros `AudDrv_Allocate_DL1_Buffer()`
  (`mt_soc_afe_control.c:2563-2620`) does **not** allocate DMA memory: it sets
  `pblock->pucPhysBufAddr = AFE_INTERNAL_SRAM_PHY_BASE` (`:2584`),
  `pblock->pucVirtBufAddr = Get_Afe_SramBase_Pointer()` (`:2588-2589`), then
  programs `AFE_DL1_BASE` / `AFE_DL1_END` with that address (`:2617-2618`) and
  zeroes it (`:2619`).  The `dma_alloc_coherent()` path (`:2591-2595`) is the
  `#else` branch and is not compiled.
* `mtk_pcm_dl1_params()` switches to exactly that buffer when
  `mPlaybackSramState == SRAM_STATE_PLAYBACKFULL`
  (`mt_soc_pcm_dl1.c:265-273`): `runtime->dma_area =
  Get_Afe_SramBase_Pointer()`, `runtime->dma_addr =
  AFE_INTERNAL_SRAM_PHY_BASE`, then `AudDrv_Allocate_DL1_Buffer()`.
* The vendor HP impedance PCM, in contrast, allocates its ALSA buffer through
  `AudDrv_Allocate_mem_Buffer()` → `dma_alloc_coherent()`
  (`mt_soc_afe_control.c:2628-2652`) and installs it in
  `mtk_pcm_hp_impedance_params()` (`mt_soc_pcm_hp_impedance.c:163-180`), i.e.
  the PCM as shipped plays **DRAM**.  Its `Audio_HP_ImpeDance_Set()` however
  only ever writes the **SRAM** (`:361-362`, `:377-378`).

Conclusion, stated plainly: the control's SRAM write can only be clocked out
when the DL1 memory interface is rebased to the SRAM, which is what the vendor's
DL1 SRAM state does and what this port's HP impedance DAI does. Playing the
ALSA ring buffer (the vendor HP PCM's literal params) would make the SRAM fill
a no-op. The port therefore uses the SRAM, which is also the design the task
specifies.

The port also keeps the ALSA managed buffer allocated (36 KiB,
`Dl1_MAX_BUFFER_SIZE`, `mt_soc_pcm_common.h:123`), because ALSA requires one;
the hardware simply never reads it for this stream.

### 2.7 Vendor HP impedance prepare/close and the port mapping

`mtk_pcm_hp_impedance_prepare()` (`mt_soc_pcm_hp_impedance.c:230-278`):

| Vendor | Lines | Port (`mt6735-dai-hpimp.c`) |
|---|---|---|
| fetch format DL1 (+DL2) 24/16-bit | 237-255 | DL1 fetch format in `prepare` (`AFE_MEMIF_PBUF_SIZE`, mask/shift from `mt6735-afe-pcm.c`); DL2 intentionally untouched (see §5) |
| `SetoutputConnectionFormat(O03/O04)` | 243-253 | `mt6735_afe_set_conn_width(DL1, wlen24)` → `AFE_CONN_24BIT` bits 3/4 |
| `SetSampleRate(MEM_I2S, rate)` | 257 | inside `mt6735_afe_dac_prepare()` (`AFE_DAC_CON1` bits [11:8]) |
| `SetMemoryPathEnable(I2S_OUT_DAC, true)` + `SetI2SDacOut()`/`SetI2SDacEnable()` | 258-264 | `priv->dac_path_on` covers the user-count-only path (`SetMemoryPathEnable()` returns before the register write for blocks above `NUM_OF_MEM_INTERFACE`, `mt_soc_afe_control.c:1991-1994`); `mt6735_afe_dac_prepare()` does `CleanPreDistortion`, `SetDLSrc2`, `AFE_I2S_CON1`, `AFE_ADDA_UL_DL_CON0`, `AFE_ADDA_DL_SRC2_CON0`, `AFE_I2S_CON1` enable |
| `SetConnection(I05→O03)`, `(I06→O04)` | 265-268 | `mt6735_afe_connect(DL1)`: `AFE_CONN1` bit 21 (`mConnectionbits[I05][O03]`, `mt_soc_afe_connection.c:187`, register `mConnectionReg[I05][O03]`, `:293`), `AFE_CONN2` bit 6 (`[I06][O04]`, `:189`, `:295`) |
| `SetSampleRate(MEM_DL1)` / `SetChannels(MEM_DL1)` | 270-271 | `AFE_DAC_CON1` bits [3:0] / bit 21 |
| `SetMemoryPathEnable(MEM_DL1, true)` + `EnableAfe(true)` | 272-274 | `trigger` START → `mt6735_afe_memif_enable(DL1, true)` (sets `AFE_DAC_CON0` bit 1 and `AFE_DAC_CON0` bit 0 via `mt6735_afe_update_power()`; `mt_soc_afe_control.c:981,1029,1994-1998`) |
| `mtk_soc_pcm_hp_impedance_close()` | 281-290 | `trigger` STOP → disable DL1, disconnect, `mt6735_afe_dac_disable()` when no other playback/capture remains |

The vendor's own trigger is a no-op that ends in `return -EINVAL`
(`:296-308`) and its pointer always returns 0 (`:131-134`); the port relies on
the AFE component pointer, which returns 0 for this DAI because its id (4) is
outside `memif_size` (3). No IRQ user is registered for the HP stream, so a
concurrent DL2 stream keeps IRQ1 exactly as before.

### 2.8 Machine link, CPU DAI and codec DAI names

* `AudDrv_Def.h:229` `MT_SOC_HP_IMPEDANCE_NAME "mt-soc-hpimpedancedai-driver"`
  → CPU DAI name.
* `AudDrv_Def.h:336` `MT_SOC_HP_IMPEDANCE_STREAM_NAME "HP_IMPEDANCE_Playback"`.
* `AudDrv_Def.h:287` `MT_SOC_CODEC_HP_IMPEDANCE_NAME
  "mt-soc-codec-hp-impedance-dai"`.
* `mt_soc_machine.c:1058-1066`: link name `HP_IMPEDANCE`, playback-only in
  effect (no capture DAI), `codec_name = MT_SOC_CODEC_NAME`.
* The vendor codec DAI entry (`mt_soc_codec_63xx.c:1538-1548`) is
  playback-only and uses the common `mt6323_aif1_dai_ops`; those ops only
  record the stream rate (`:1308-1347`) for `TurnOnDacPower()`'s PMIC downlink
  programming (`:1641-1646`). This port's `mt6331_dai_hw_params()` is the
  equivalent (it writes `priv->dl_rate` and `MT6328_AFE_PMIC_NEWIF_CFG0` /
  `AFE_DL_SRC2_CON0_H`), so the link binds a real fourth codec DAI rather than
  `snd-soc-dummy`. It is not a fake DAI: without it the analog HP impedance
  sequence would power the downlink with a stale rate.

## 3. Parity matrix

| Behaviour | ROM / downstream implementation | Before | After | Status |
|---|---|---|---|---|
| Efuse HP offset read | `Auddrv_Read_Efuse_HPOffset`, `mt_soc_codec_63xx.c:484-566`, called from `InitAfeControl` `mt_soc_afe_control.c:468` | absent | `mt6331_efuse_hp_offset_read()`: same clock enable/row/toggle/poll/read/disable sequence, bounded poll, `usleep_range(1000,1100)`; called at probe and retried from `mt6331_dl_power_up()` | done |
| Efuse HP offset apply | `#ifdef EFUSE_HP_TRIM` branch would write `AUDDEC_ANA_CON2` (`:1206-1212`), but macro is undefined | absent | values decoded into `priv->hp_trim`, logged with `dev_info`, **never** written to `AUDDEC_ANA_CON2` | done, parity with `#ifndef` branch |
| Analog trim hardware | `OpenAnalogTrimHardware` `:748-820`, trim mux/gain `:610-619`, `EnableTrimbuffer` `:666-679` | absent | ported 1:1 in `mt6331_open_analog_trim_hw`, `mt6331_set_offset_trim_mux`, `mt6331_set_offset_trim_buffer_gain`, `mt6331_enable_trimbuffer` | done |
| HP impedance analog sequence | `OpenHeadPhoneImpedanceSetting` body `:887-984` (inside `#if 0`), `setHpGain` `:1059`, `SetSdmLevel` `:1065` | absent | `mt6331_open_hp_impedance_setting()`, `mt6331_set_hp_gain()`, `mt6331_set_sdm_level()`; exposed as `Audio HP ImpeDance Setting` + alias `Audio_HP_ImpeDance`; 1 = start, 0 = stop, other = store DC value | done (analog); DC sweep stays in userspace |
| AUXADC read | `PMIC_IMM_GetOneChannelValue(MT6328_AUX_CH9, 20, 0)`, `pmic_auxadc.c:173-455` | absent | `mt6331_auxadc_read()`; `Audio AUXADC Data` get performs a one-shot read of `priv->auxadc_channel` (default 9), set stores the channel | done |
| AFE SRAM DC excitation | `Audio_HP_ImpeDance_Set()` `memset(Get_Afe_SramBase_Pointer(), value, 0x8000)` `mt_soc_pcm_hp_impedance.c:361,377` | absent | AFE component control `HP Impedance DC Value` writes 0x8000/2 16-bit words at `base+0x1000` under runtime PM, only while the HP impedance stream is open (`-EBUSY` otherwise) | done |
| SRAM playback to the codec | `AUDIO_MEMORY_SRAM` (`AudDrv_Def.h:45`); `AudDrv_Allocate_DL1_Buffer()` and `mtk_pcm_dl1_params()` point DL1 at `0x11221000..0x11228fff` (`mt_soc_afe_control.c:2563-2619`, `mt_soc_pcm_dl1.c:265-273`); vendor prepare enables DL1/I2S DAC and I05/I06 routes (`mt_soc_pcm_hp_impedance.c:230-278`) | absent | `mt6735-dai-hpimp.c` rebases DL1 to the SRAM in `hw_params`, programs the vendor format/rate/channel/routes/DAC in `prepare`, enables DL1+AFE at START and tears it down at STOP | **done** |
| HP impedance FE DAI | `MT_SOC_HP_IMPEDANCE_NAME` (`AudDrv_Def.h:229`), stream `HP_IMPEDANCE_Playback` (`:336`) | absent | playback-only `mt-soc-hpimpedancedai-driver`, id `MT6735_AFE_DAI_HPIMP`, registered by the AFE component | done |
| Machine link | `mt_soc_machine.c:1058-1066` | absent | link `HP_IMPEDANCE`, stream `HP_IMPEDANCE_Playback`, playback-only | done |
| Codec DAI | `MT_SOC_CODEC_HP_IMPEDANCE_NAME` (`AudDrv_Def.h:287`), vendor `mt_soc_codec_63xx.c:1538-1548` with rate-tracking ops (`:1308-1347`) | absent | `mt-soc-codec-hp-impedance-dai`, playback-only, `mt6331_dai_ops` (sets `dl_rate` + PMIC downlink interface) | done |
| DL1 vs HP impedance exclusion | vendor uses a single memif without an explicit guard (user counts only, `mt_soc_afe_control.c:1968-2000`) | absent | mutual `-EBUSY` under `hp_imp_lock`: HP start rejects an open/running DL1, DL1 startup rejects a held HP stream, control fill rejects a non-HP stream | done |
| First-call AUXADC offset | `mAuxAdc_Offset` calibration, `mt_soc_pcm_hp_impedance.c:351-370` | absent | `mt6331_hp_impedance_calibrate()` runs once before the start/stop/value branches | done (SRAM fill + 5 s wait dropped, §5) |
| Userspace sweep | `ApplyDctoDl` / `Calculate_HP_Impedance` `:466-572`; Set "other" loop `:387-418` | absent | userspace: fill DC value, start, wait, read AUXADC; resistance formula stays in userspace | done by design |

### Userspace sequence (HAL contract, end to end)

The DC fill is accepted only while the HP impedance PCM is open, and the
memory interface cannot be taken while normal DL1 is streaming:

```sh
# 0. no MultiMedia1/DL1 stream may be open: the HP stream takes DL1 over

# 1. open + configure + prepare the HP impedance PCM (playback-only FE)
#    e.g. aplay -D hw:... (44.1/48 kHz, S16_LE or S24_LE), kept open
#    ALSA calls startup -> hp_imp_open, hw_params -> DL1 BASE/END = SRAM,
#    prepare -> AFE format/rate/channels/routes/DAC + PMIC pins

# 2. load the DC level into the AFE internal SRAM
amixer -c 0 cset name='HP Impedance DC Value' <dc16>

# 3. start the stream: memory path + AFE on, SRAM is clocked to the codec
#    (snd_pcm_start / SNDRV_PCM_IOCTL_START; no data needs to be written)

# 4. enable the analog HP impedance path (first call also measures the
#    AUXADC/trim offset)
amixer -c 0 cset name='Audio_HP_ImpeDance' 1   # or 'Audio HP ImpeDance Setting'

# 5. settle, then read the raw 12-bit AUXADC code
amixer -c 0 cget name='Audio AUXADC Data'

# 6. repeat steps 2 and 5 for each DC step

# 7. restore: analog path off, stream stop + close
amixer -c 0 cset name='Audio_HP_ImpeDance' 0
```

Calling `cset 'HP Impedance DC Value'` without an open HP impedance stream now
fails with `-EBUSY`, and an HP stream cannot start while DL1 is in use.

## 4. Compile commands and result

```
$ export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
$ cd $M/worktrees/audio-gap-hpimp
$ export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
$ cp $M/nura.config .config
$ ./scripts/config --enable SOUND --enable SND --enable SND_SOC \
      --enable SND_SOC_MEDIATEK --enable SND_SOC_MT6735 \
      --enable SND_SOC_MT6331 --enable SND_SOC_MT6735_MT6331 \
      --enable MTK_ACCDET
$ make ARCH=arm64 LLVM=1 olddefconfig            # exit 0
$ make ARCH=arm64 LLVM=1 -j2 \
      sound/soc/mediatek/mt6735/mt6735-afe-pcm.o \
      sound/soc/mediatek/mt6735/mt6735-dai-pcm.o \
      sound/soc/mediatek/mt6735/mt6735-dai-hpimp.o \
      sound/soc/mediatek/mt6735/mt6735-mt6331.o \
      sound/soc/codecs/mt6331.o
  CC      sound/soc/codecs/mt6331.o
  CC      sound/soc/mediatek/mt6735/mt6735-afe-pcm.o
  CC      sound/soc/mediatek/mt6735/mt6735-dai-pcm.o
  CC      sound/soc/mediatek/mt6735/mt6735-dai-hpimp.o
  CC      sound/soc/mediatek/mt6735/mt6735-mt6331.o
# exit 0, no warnings
$ make ARCH=arm64 LLVM=1 -j2 sound/soc/mediatek/mt6735/
  CC      sound/soc/mediatek/mt6735/mt6735-afe-clk.o
  CC      sound/soc/mediatek/mt6735/mt6735-dai-i2s.o
  AR      sound/soc/mediatek/mt6735/built-in.a
# exit 0, no warnings
```

All objects compile with clang (`clang version 23.1.1`, `LLVM=1`). The
worktree's `snd_soc_kcontrol_component()`-avoiding `snd_kcontrol_chip()` use
from the earlier revision is retained.

## 5. Inferred / unverified / deviations

1. **EFUSE_HP_TRIM not defined** in the downstream build (verified by grep,
   §2.1). The decoded trim is logged/stored only; `AUDDEC_ANA_CON2` is not
   programmed. This intentionally matches the vendor `#ifndef` path.
2. **Call-site deviation**: downstream reads the efuse in `InitAfeControl()` on
   the first open (`mt_soc_afe_control.c:468`). This port reads at component
   probe (so the `dev_info` values exist once at boot) and retries from
   `mt6331_dl_power_up()`. Functionally equivalent, one read per boot.
3. **Unbounded downstream loops replaced by timeouts**
   (`regmap_read_poll_timeout`): OTP ready 1 ms × 20, AUXADC ready 1 ms × 20.
   The timeout values are a safety bound, not a vendor constant. [INFERENCE]
4. **`OpenHeadPhoneImpedanceSetting()` body is dead code in the vendor**
   (`#if 0`, `:880-989`); the port implements the intended body. The vendor's
   `GetDLStatus()` early return is not reproduced because the codec component
   cannot query the AFE component's stream state; userspace must not run the
   analog sequence while normal playback is active.
5. **`memset()` vs 16-bit words**: the vendor `Set` branch uses
   `memset(ptr, value, 0x8000)`, which repeats the *low byte*, while the
   `Get`/`ApplyDctoDl` path (`FillDatatoDlmemory`, `:453-464`) writes 16-bit
   samples. The AFE control broadcasts the 16-bit value (`writew`), matching
   the sample-based path.
6. **SRAM vs DMA in the vendor HP PCM**: §2.6. The vendor HP PCM's own
   `hw_params` installs a DRAM DMA buffer, while the control fills the SRAM;
   the port resolves this by rebasing DL1 to the SRAM, which is the vendor's
   DL1 SRAM mechanism and the only way the control's value can be clocked.
7. **Prepare/trigger split**: the vendor enables the DL1 memory path and
   `EnableAfe(true)` in `prepare` (`mt_soc_pcm_hp_impedance.c:272-274`); the
   port does it at `SNDRV_PCM_TRIGGER_START` and disables at STOP, matching
   the rest of this ASoC driver.
8. **DL2 fetch format not touched**: the vendor prepare also writes the DL2
   fetch-format field (`:241`, `:250`), which would corrupt an active DL2
   stream. The port touches DL1 only; DL2 is independent and can run
   concurrently with the HP impedance stream (the HP stream stops disabling
   the DAC while DL2 is active).
9. **Rate/format range**: the vendor advertises 8 kHz..192 kHz and 12 formats
   (`SOC_HIGH_USE_RATE`, `SND_SOC_ADV_MT_FMTS`); the port's AFE encoding table
   and DAI caps are 8..48 kHz, S16_LE/S24_LE, matching the rest of the port.
   Impedance measurements use 44.1/48 kHz.
10. **No IRQ/pointer for the HP stream**: vendor pointer returns 0
    (`:131-134`) and trigger is a no-op returning `-EINVAL` (`:296-308`). The
    port leaves the component pointer to return 0 (DAI id outside
    `memif_size`) and registers no IRQ user, so no MCU interrupt is needed
    while the AFE free-runs over the SRAM. Userspace may start the stream
    without writing data. [INFERENCE: ALSA start without prefill keeps the
    stream in RUNNING because no period/xrun source exists.]
11. **Vendor's 1 s `msleep()` after storing the DC value**
    (`mt_soc_pcm_hp_impedance.c:349`) and the 5 s wait in the calibration
    branch (`:363-365`) are not executed in the kcontrol; the userspace "wait"
    step covers them.
12. **`Get` semantics differ**: vendor `Audio_HP_ImpeDance_Get`
    (`:579-606`) runs the sweep and returns a resistance. This port's get
    returns the last value written (the DC value/start/stop command), because
    the sweep is userspace-driven.
13. **Alias control**: both `Audio HP ImpeDance Setting` (exact downstream/ROM
    string) and `Audio_HP_ImpeDance` (name requested by the assignment) are
    registered over the same state.
14. **AUXADC deCount (20) is unused** in the downstream implementation
    (`deCount` never referenced inside `PMIC_IMM_GetOneChannelValue`), so the
    port has no averaging parameter.
15. The baseline accdet AUXADC path confirmed the same PMIC block
    (`0x0e88/0x0e8a/0x0e0a/0x0e94/0x0eb0`); channel 9's ready bit is
    `ADC9[15]` and its value `ADC9[11:0]`.
16. The return value of `regmap_write()` on the OTP clock-gate registers is
    checked on enable; the disable writes are best-effort, like the vendor.
17. **ALSA buffer still allocated**: `pcm_construct` gives the HP PCM a 36 KiB
    managed buffer that the hardware ignores (the AFE reads the SRAM). This is
    required by ALSA and costs only 36 KiB of DRAM.

## 6. DTS-FRAGMENT

None. The codec binds to the existing PMIC child node
(`mediatek,mt6328-codec`, created by `drivers/mfd/mt6397-core.c`) and the AFE
uses the existing `mediatek,mt6735-afe` node/ioremap. The SRAM window is inside
the already-mapped AFE range (`0x11220000 + 0x10000`). No DT input is required;
the optional `hp-depop` GPIO already existed in the baseline.

## 7. Patch

`$M/patches/audio-gap-hpimp.patch`
(`git diff --binary` with the baseline staged; 9 files, +1169/-9 lines).
`git apply --check -R` succeeds in the worktree.
