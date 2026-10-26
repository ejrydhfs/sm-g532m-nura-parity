# audio-mt-soc — MT6735/MT6737T ASoC AFE platform driver

Worktree: `$M/worktrees/audio-mt-soc` (HEAD 046883c0c, nura-linux 7.0.10 arm64)
Scope: AFE + memif DAIs (DL1/DL2/UL1) + I2S0 BE DAI. No machine driver, no codec.

## 1. Assigned driver and files changed

New (all under `sound/soc/mediatek/mt6735/`):

| File | Content |
|---|---|
| `mt6735-afe-regs.h` | AFE register map + bit fields, every definition cited to downstream file:line |
| `mt6735-afe-common.h` | private state (`mtk_base_afe` + `mt6735_afe_priv`), rates/formats, DAI ids, prototypes |
| `mt6735-afe-clk.c` / `.h` | 21 `aud_*` clocks via `clk_bulk`, `AUDIO_TOP_CON0` power-on, audio pinctrl states |
| `mt6735-afe-pcm.c` | platform driver `mediatek,mt6735-afe`, regmap MMIO, memif tables, MCU IRQ handler, I2S0/DAC/ADC IRQ arbitration, component (pointer/pcm_construct), runtime PM + regcache |
| `mt6735-dai-pcm.c` | FE DAIs `mt-soc-dl1dai-driver`, `mt-soc-dl2dai-driver`, `mt-soc-ul1dai-driver` (startup/hw_params/prepare/trigger) + PMIC DAC/ADC path + AFE_CONN routing |
| `mt6735-dai-i2s.c` | BE DAI `mt-soc-i2s0dai-driver` (set_fmt/set_sysclk/hw_params/trigger, `AFE_I2S_CON`) |
| `Makefile` | `snd-soc-mt6735-afe.ko` |

Modified (only the permitted additions):
- `sound/soc/mediatek/Kconfig`: new `SND_SOC_MT6735` entry (selects `SND_SOC_MEDIATEK`).
- `sound/soc/mediatek/Makefile`: `obj-$(CONFIG_SND_SOC_MT6735) += mt6735/`.

No `arch/arm64/boot/dts/**` edit, no `sound/soc/codecs/**` edit, no machine driver,
no commit.

Interface-contract conformance: component name **`mtk-afe-pcm`** (the established MTK AFE
component name), CPU DAIs exactly `mt-soc-dl1dai-driver`, `mt-soc-dl2dai-driver`,
`mt-soc-ul1dai-driver`, `mt-soc-i2s0dai-driver`; DT compatible `mediatek,mt6735-afe`;
reg 0x11220000/0x10000; single platform IRQ.

## 2. ROM/downstream evidence

### 2.1 ROM disassembly (`$M/zip/vmlinux.elf`, 32-bit ARM, addresses verified with llvm-objdump)

| Symbol @ address | Observed register argument | Confirms |
|---|---|---|
| `AudDrv_IRQ_handler` @ 0xc084c08c | `Afe_Get_Reg(932/0x3A4)`, `(948/0x3B4)`, `(956/0x3BC)`, `(972/0x3CC)`; `Afe_Set_Reg(936/0x3A8)` | AFE_IRQ_MCU_STATUS/EN/CNT5/DEBUG, CLR |
| `Auddrv_DL1_Interrupt_Handler` @ 0xc084b7a8 | `Afe_Get_Reg(68/0x44)` | AFE_DL1_CUR |
| `mtk_pcm_pointer` @ 0xc0850238 | `Afe_Get_Reg(68/0x44)`, `Align64ByteSize`, `audio_bytes_to_frame` | pointer = CUR − BASE in frames |
| `SetIrqMcuCounter` @ 0xc0847224 | `Afe_Set_Reg(940/0x3AC)`, `(944/0x3B0)`, `(988/0x3DC)` | AFE_IRQ_MCU_CNT1/CNT2/CNT7 |
| `SetIrqEnable` @ 0xc0846f60 | `Afe_Set_Reg(928/0x3A0)`, `(948/0x3B4)` | AFE_IRQ_MCU_CON/EN |
| `EnableAfe` @ 0xc0847fac | `Afe_Set_Reg(16/0x10, mask 0x1)` | AFE_DAC_CON0 bit0 |
| `SetDLSrc2` @ 0xc0849194 | `Afe_Set_Reg(264/0x108)`, `(268/0x10C)` | AFE_ADDA_DL_SRC2_CON0/1 |
| `SetI2SDacOut` @ 0xc0849284 | `Afe_Set_Reg(52/0x34)` | AFE_I2S_CON1 (PMIC codec bus) |
| `SetMemIfFetchFormatPerSample` @ 0xc084a658 | `Afe_Set_Reg(984/0x3D8)` masks `0x30000`, `0xC0000`, `0xC00000` | DL1/DL2/VUL fetch format fields |
| `SetSampleRate` @ 0xc0848174 | `Afe_Set_Reg(20/0x14)` masks `0xf`,`0xf0`,`0xf00`,`0xf000`,`0xf0000` | AFE_DAC_CON1 per-memif FS fields |
| `Enable4pin_I2S0_I2S3` @ 0xc084ccfc | `Afe_Set_Reg(4/0x04)`, `(24/0x18)`, `(76/0x4C)` | AUDIO_TOP_CON1, AFE_I2S_CON, AFE_I2S_CON3 |

ROM strings (`$M/zip/vmlinux.strings`): `mt-soc-dl1dai-driver`:59257,
`mt-soc-ul1dai-driver`:59259, `mt-soc-i2s0dai-driver`:59283,
`mt-soc-dl2dai-driver`:59313.

### 2.2 Downstream source (`$M/src/downstream-grandpplte/sound/soc/mediatek/mt_soc_audio_v3/`)

- `AudDrv_Afe.h`: base 0x11220000:361; AUDIO_TOP_CON0..3 416-426; AFE_DAC_CON0/1 427-428;
  AFE_I2S_CON 429; AFE_CONN0..3 432-435; AFE_I2S_CON1/2 438-439; DL1 443-445; DL2 454-456;
  AFE_CONN_24BIT 458; VUL 462-464; AFE_MEMIF_MSB 470; AFE_MEMIF_MON0..4 473-477;
  AFE_ADDA_* 481-491, 539-540; AFE_IRQ_MCU_* 584-593; AFE_MEMIF_PBUF_SIZE 611.
- `mt_soc_afe_control.c`: EnableAfe 949-1031 (DAC_CON0 bit0 at 981/1029, PMIC pin select 993/1017);
  SetSampleRate 1073-1128; SetChannels 1129-1160; SetI2SAdcIn 1346-1380; SetI2SAdcEnable 1553-1572;
  CleanPreDistortion 1583-1590; SetDLSrc2 1592-1635; SetI2SDacOut 1637-1655; SetMemoryPathEnable
  1968-2000; SetI2SDacEnable 2010-2034; SetIrqEnable 2099-2126; SetIrqMcuSampleRate 2128-2152;
  SetIrqMcuCounter 2154-2178; SetMemIfFetchFormatPerSample 2294-2369; SetoutputConnectionFormat
  2371-2377; AudDrv_Allocate_DL1_Buffer 2563-2620; AudDrv_IRQ_handler 574-642 (spurious recovery
  594-610, shared IRQ1 613-620, IRQ2 620-630); IRQ manager 3830-4153 (`IRQ_MAX_RATE` 260000 from
  `mt_soc_digital_type.h:865`); `AudioInterruptLimiter = 100` at 164; `Align64ByteSize` 3702.
- `mt_soc_pcm_dl1.c`: SetDL1Buffer 234-255; pointer 189-235; prepare 374-424; start 426-479;
  stop 168-186; close 347-358.
- `mt_soc_pcm_capture.c`: StopAudioCaptureHardware 101-116; StartAudioCaptureHardware 139-176.
- `mt_soc_pcm_dl2.c`: prepare 300-351; start 353-386; stop 100-125.
- `mt_soc_pcm_dl1_i2s0.c` / `mt_soc_pcm_dl1_i2s0Dl1.c`: I2S0 field layout 186-214, O00/O01 routing
  514-518.
- `AudDrv_Clk.c`: clock names 136-157; devm_clk_get 171; AUDIO_TOP_CON0 bit14 253/265;
  Clk_On (CCF) 368-476; Clk_Off 479-533 (IRQ clear 511).
- `AudDrv_Gpio.c`: pinctrl state list 79-101; PMIC select 215-236; I2S select 238-259.
- `mt_soc_afe_connection.c`: mConnectionbits 173-224; mConnectionReg 281-332;
  mConnectionState/SetConnectionState 380-500.
- `mt_soc_digital_type.h`: FS encoding 265-279; AFE_WLEN 507-509; IRQ_MAX_RATE 865.

## 3. Parity matrix

| Behaviour | ROM/downstream implementation | Nura before | After | Status |
|---|---|---|---|---|
| AFE device | `audio@11220000`, SPI 144 level high; ioremap 0x11220000 (AudDrv_Afe.c) | no node, no driver | `mediatek,mt6735-afe` platform driver, regmap MMIO 32-bit, max 0xffc | done |
| Clocking | aud_* clocks via CCF (AudDrv_Clk.c:136-171, 437-470); AUDIO_TOP_CON0 bit14 (265); IRQ clear before gating (511) | none | `clk_bulk` 21 named clocks (optional), bit14 set on resume, CLR written on suspend | done (coarse bulk enable, see §5) |
| PMIC pin mux | audpmicclk-mode0 = GPIOs, mode1 = AUD_CLK_MOSI/MISO/MOSI; EnableAfe selects mode1 when active (AudDrv_Gpio.c:215-236) | none | default at probe/remove, mode1 while codec path active (prepare), mode0 when idle (shutdown) | done |
| DL1 memif | 0x40/0x44/0x48; DAC_CON1 FS[3:0]/mono b21; PBUF[17:16]; DAC_CON0 b1 enable | none | hw_params writes base/end/FS/mono/PBUF, trigger sets enable bit | done |
| DL2 memif | 0x50/0x54/0x58; FS[7:4]/mono b22; PBUF[19:18]; DAC_CON0 b2 | none | same | done (mono shift corrected, §5) |
| UL1/VUL memif | 0x80/0x88/0x8C; FS[19:16]/mono b27; PBUF[23:22]; DAC_CON0 b3 | none | same | done |
| Period IRQ | IRQ1 shared DL1/DL2, IRQ2 VUL; CNT1/CNT2 = period; FS in CON[7:4]/[11:8]; enable bit0/bit1; CLR = status | none | single threaded-less handler, min-period arbitration at 260 kHz reference (downstream get_tgt_count), `snd_pcm_period_elapsed` | done |
| Spurious IRQ | status==0 → clear all, stop after 100 (mt_soc_afe_control.c:594-610) | none | same limiter | done |
| Pointer | CUR − BASE in frames (mtk_pcm_pointer) | none | component `.pointer`, bounds-checked (returns 0 for the I2S0 link) | done |
| Codec DAC bus | AFE_I2S_CON1 + ADDA_DL_SRC2_CON0/1 + PREDIS clear + UL_DL_CON0 (SetI2SDacOut/SetDLSrc2/SetI2SDacEnable) | none | implemented in FE prepare / trigger stop | done |
| Codec ADC bus | ADDA_TOP_CON0, UL_SRC_CON0 voice mode, NEWIF_CFG0/1, UL_DL_CON0, UL_SRC_CON0 enable | none | implemented in FE prepare / trigger stop | done |
| AFE_CONN routing | DL1→O03/O04, DL2→O03/O04, ADC→O09/O10, 24-bit bits | none | four routes + CONN_24BIT | done (only these routes) |
| I2S0 BE | AFE_I2S_CON layout (3754-3761); Enable4pin soft-reset/gating sequence; audi2s1 pin states | none | set_fmt/set_sysclk/hw_params/trigger, DL1→O00/O01 when DL1 active | done, except MCLK/BCLK dividers (§5) |
| Runtime PM / suspend | per-block clock counters, register backup | none | runtime PM + `regcache_cache_only`/`regcache_sync`, system sleep via `pm_runtime_force_*` | done |
| SRAM playback path | AudDrv_Allocate_DL1_Buffer uses internal SRAM when free | none | only DRAM managed buffers | deferred |
| Voice MD1/MD2 (+BT), HDMI, FM I2S/MRG, TDM capture, AWB/ULDL loopback, BT CVSD (DAIBT), offload/GDMA, HP impedance, MRG | present downstream | none | not registered (no stubs) | deferred (out of scope) |
| Other memifs (AWB, DAI/mod-PCM, DL1_D2, VUL_D2, HDMI), side tone, sine gen, HW gain, ASRC, 2nd I2S, modem PCM | present downstream | none | not implemented | deferred |

## 4. Compile-tested

```
cd $M/worktrees/audio-mt-soc
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
./scripts/config --enable SOUND --enable SND --module SND_SOC --module SND_SOC_MT6735
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
  sound/soc/mediatek/mt6735/mt6735-afe-pcm.o \
  sound/soc/mediatek/mt6735/mt6735-afe-clk.o \
  sound/soc/mediatek/mt6735/mt6735-dai-pcm.o \
  sound/soc/mediatek/mt6735/mt6735-dai-i2s.o
```

Result (after `rm` of the four objects, full rebuild, exit status 0, no warnings):

```
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-afe-clk.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-pcm.o
  CC [M]  sound/soc/mediatek/mt6735/mt6735-dai-i2s.o
```

`olddefconfig` resolved `CONFIG_SND_SOC_MT6735=m` (and selected `CONFIG_SND_SOC_MEDIATEK=m`).
A module link (`make ... snd-soc-mt6735-afe.ko`) also passes `LD [M]`; modpost then reports
unresolved core symbols only because no `vmlinux.o` is built in this single-module build
(`WARNING: vmlinux.o is missing`), not because of a driver problem. The `.o` targets above are
the acceptance criteria.

Note: the `.config`/`scripts/config` lines are local build enablement, not part of the patch.

## 5. Inferred or unverified on hardware

1. **PMIC pin state for "codec active"**: the assignment says select `audpmicclk-mode0` while the
   codec path is active. Downstream contradicts this: `AudDrv_GPIO_PMIC_Select(1)` selects
   *mode1* (`AudDrv_Gpio.c:219-234`), and in the board DTS `audpmicclk-mode0` is the idle GPIO
   setting while `audpmicclk-mode1` muxes GPIO143/144/145 to AUD_CLK_MOSI/AUD_DAT_MISO/
   AUD_DAT_MOSI. This driver therefore selects **mode1 while the codec path is active** and
   mode0 when it is idle / default at probe-remove. Documented as a deliberate deviation from the
   assignment wording, following downstream + the hardware description.
2. **DL2 mono bit**: downstream writes value `bMono << 21` with mask `1 << 22`
   (`mt_soc_afe_control.c:1139`), so the field can never be set. The mask identifies bit 22; this
   driver uses shift 22 (commented in the memif table).
3. **PCM constraints**: `period_bytes_min = 64`, `periods_min = 2`, `periods_max = 256` and a
   64-byte `BUFFER_BYTES` step are inferred from `Align64ByteSize()` / the 32-byte sample mask;
   downstream's `.periods_max` value is `36*1024` (a byte count) and unusable.
4. **Capture voice-mode field**: downstream leaves `dVoiceModeSelect = 0` for any rate other than
   8/16/32/48 kHz (`mt_soc_afe_control.c:1352-1366`). That is reproduced as-is; 11.025/22.05/
   44.1 kHz capture is therefore unverified, possibly wrong (hardware-only question).
5. **Clock model**: the 21 `aud_*` clocks are fetched with `devm_clk_bulk_get_optional()` and
   enabled as one group. Downstream keeps per-block counters (ADC, I2S, APLL, EMI, ...); the AFE
   register block state is identical but power gating is coarser than downstream. There is no
   audiosys clock provider (`clk-mt6735-audiosys.c` / `mediatek,mt6735-audiosys`) in this kernel
   yet, so the clocks are optional: without them AFE register access depends on the boot state of
   the audio bus clock and is unverified on hardware. [INFERENCE + missing provider]
6. **MCLK/BCLK**: the I2S0 dividers live in the audiosys clock controller (`AUDIO_CLK_AUDDIV_1`,
   0x1021xxxx, `SetCLkMclk()`/`SetCLkBclk()`), outside the AFE. `set_sysclk` validates and stores
   the request; `hw_params` checks it is an integer multiple of the rate and warns when below the
   default 128×fs. No divider is programmed here.
7. **IRQ trigger type**: DT says level-high (SPI 144); the downstream driver requested
   `IRQF_TRIGGER_LOW` on a hardcoded line. This driver passes flags 0 and honours DT.
8. **DMA address width**: AFE memifs use 32-bit addresses; a 33-bit address is rejected
   (`-EINVAL`) because `AFE_MEMIF_MSB` is never programmed downstream. `dma_set_mask_and_coherent(32)`
   is applied.
9. **AFE_IRQ_MCU_EN**: only used by downstream for IRQ3/5/7; IRQ1/2 are enabled via
   `AFE_IRQ_MCU_CON` bits 0/1, as downstream does.
10. **Register writes in trigger**: `regmap_update_bits` on an MMIO map is atomic-safe; pinctrl
    selection was moved to prepare/shutdown because trigger context may not sleep.
11. **Register values**: every value written is a transcription of a downstream constant cited in
    the source (e.g. `0xf74f0000`, `0x03f87201`, `(0x3<<24)|(0x3<<11)|(0x1<<1)`); no value was
    invented. ROM confirmation exists for all register offsets; 0xf74f0000/0x03f87201 are
    downstream-only (not visible in disassembly).
12. **No hardware verification of anything**: no phone attached. All streaming, IRQ, DAC/ADC and
    pin behaviour is source-parity only.

## 6. DTS-FRAGMENT

Board-level pin groups (adapted from the downstream grandpplte DTS; the macros already exist in
`include/dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h`). `audi2s1-mode0/1` are empty groups on
grandpplte (no pins), so they are intentionally not defined — the driver tolerates missing states.

```dts
#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/interrupt-controller/irq.h>
#include <dt-bindings/pinctrl/mediatek,mt6735-pinctrl.h>

&pio {
	aud_pins_default: auddefault {
		pins_cmd0_dat {
			pinmux = <PINMUX_GPIO80__FUNC_GPIO80>;
			slew-rate = <1>;
			bias-disable;
			output-low;
		};
		pins_cmd1_dat {
			pinmux = <PINMUX_GPIO185__FUNC_F2W_CK>;
			slew-rate = <0>;
			bias-disable;
		};
	};

	aud_pins_pmicclk_mode0: pmicclkmode0 {
		pins_cmd0_dat {
			pinmux = <PINMUX_GPIO143__FUNC_GPIO143>;
		};
		pins_cmd1_dat {
			pinmux = <PINMUX_GPIO144__FUNC_GPIO144>;
		};
		pins_cmd2_dat {
			pinmux = <PINMUX_GPIO145__FUNC_GPIO145>;
		};
	};

	aud_pins_pmicclk_mode1: pmicclkmode1 {
		pins_cmd0_dat {
			pinmux = <PINMUX_GPIO143__FUNC_AUD_CLK_MOSI>;
		};
		pins_cmd1_dat {
			pinmux = <PINMUX_GPIO144__FUNC_AUD_DAT_MISO>;
		};
		pins_cmd2_dat {
			pinmux = <PINMUX_GPIO145__FUNC_AUD_DAT_MOSI>;
		};
	};
};

/* Place in mt6735.dtsi (or keep in the board DTS). */
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
```

Prerequisite for the clock properties — **not present in this kernel today**: an audiosys clock
provider node/driver (`compatible = "mediatek,mt6735-audiosys"`, `reg = <0 0x11220000 0 0x10000>`,
`#clock-cells = <1>`, the gate/mux sets from downstream `clk-mt6735.c`). Once it exists, add
(clock-names exactly as in the ROM DTS, and the matching `clocks` phandles):

```dts
	clock-names = "aud_afe_clk", "aud_i2s_clk", "aud_dac_clk",
		      "aud_dac_predis_clk", "aud_adc_clk", "aud_tml_clk",
		      "aud_apll22m_clk", "aud_apll24m_clk",
		      "aud_apll1_tuner_clk", "aud_apll2_tuner_clk",
		      "aud_infra_clk", "aud_mux1_clk", "aud_mux2_clk",
		      "top_ad_apll1_clk", "top_whpll_audio_clk",
		      "top_mux_audio", "top_mux_audio_int", "top_sys_pll1_d4",
		      "apmixed_apll1_clk", "apmixed_apll2_clk",
		      "top_clk26m_clk";
	clocks = <&audiosys 1>, <&audiosys 2>, <&audiosys 8>,
		 <&audiosys 9>, <&audiosys 7>, <&audiosys 3>,
		 <&audiosys 4>, <&audiosys 6>, <&audiosys 5>,
		 <&audiosys 10>, <&infracfg ...>, <&topckgen ...>, ...
		 /* IDs to be taken from the downstream audiosys gate table */
```

(Clock IDs are deliberately left as placeholders: the ROM DTS uses `<0xf8 0x01> ...` against the
audiosys phandle, but the mainline ID numbering of that provider does not exist yet, and inventing
IDs would be wrong. The AFE driver binds and runs without the `clocks` property.)

## 7. Patch path

`$M/patches/audio-mt-soc.patch` (`git diff --binary`, 10 files, +2015 lines).

Key integration notes for the machine-driver/audio owner:
- AFE component name is **`mtk-afe-pcm`**; `dai_link.platforms` can be omitted (CPU component is
  used as platform).
- The AFE already programs the PMIC codec bus (`AFE_I2S_CON1`, `AFE_ADDA_*`) in the DL1/DL2 FE
  prepare and the ADC path in the UL1 FE prepare; the codec driver only needs the PMIC-side
  registers. `mt-soc-codec-tx-dai` must accept 16/24-bit, 1–2 ch, 8–48 kHz.
- The I2S0 BE expects `set_fmt` with `SND_SOC_DAIFMT_I2S` and either `CBP_CFP` or `CBC_CFC`;
  other formats return `-EINVAL`.
