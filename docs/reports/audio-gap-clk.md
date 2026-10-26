# Audio gap: MT6735 audiosys clock provider

Worktree: `$M/worktrees/audio-gap-clk` (audio baseline staged in the index).
Owned set only: `drivers/clk/mediatek/*`, `include/dt-bindings/clock/*`.

## 1. Files changed

| File | Change |
|---|---|
| `include/dt-bindings/clock/mediatek,mt6735-audiosys.h` | new; AUDIO_AFE..AUDIO_TML = 1..10, AUDIO_NR_CLK = 11 (downstream numbering) |
| `drivers/clk/mediatek/clk-mt6735-audiosys.c` | new; platform driver for `"mediatek,mt6735-audiosys"`, 10 no-setclr gates at 0x0 bits 2,6,8,9,18,19,24,25,26,27 |
| `drivers/clk/mediatek/Kconfig` | `CONFIG_COMMON_CLK_MT6735_AUDIOSYS` |
| `drivers/clk/mediatek/Makefile` | `obj-$(CONFIG_COMMON_CLK_MT6735_AUDIOSYS) += clk-mt6735-audiosys.o` |

No DTS/DTSI file was touched.

## 2. Evidence

### IP / ABI
- ROM DT clock provider: `$M/zip/rom_dtb_0.dts:3537-3543` — `audiosys@11220000`, `compatible = "mediatek,mt6735-audiosys"`, `reg = <0x11220000 0x10000>`, `#clock-cells = <1>`, `phandle = <0xf8>`.
- ROM DT consumer: `$M/zip/rom_dtb_0.dts:3545-3562` — the AFE/audio node consumes `<0xf8 0x01, 0x02, 0x08, 0x09, 0x07, 0x03, 0x04, 0x06, 0x05, 0x0a>` for `aud_afe_clk, aud_i2s_clk, aud_dac_clk, aud_dac_predis_clk, aud_adc_clk, aud_apll22m_clk, aud_apll24m_clk, aud_apll1_tuner_clk, aud_apll2_tuner_clk, aud_tml_clk`, then 11 clocks from other providers. The 0xf8 ids map 1:1 onto the header below.
- Downstream ids: `src/downstream-grandpplte/include/dt-bindings/clock/mt6735-clk.h:216-227` (`AUDIO_AFE 1 ... AUDIO_TML 10, AUDIO_NR_CLK 11`).
- Downstream driver: `src/downstream-grandpplte/drivers/clk/mediatek/clk-mt6735.c:1005-1027` (`aud_cg_regs` all offsets 0x0; `GATE(... CLK_GATE_NO_SETCLR_REG)` bits 2,6,8,9,18,19,24,25,26,27, parent `aud_intbus_sel`), `:1284-1307` (`mt_audiosys_init`, `CLK_OF_DECLARE(mtk_audiosys, "mediatek,mt6735-audiosys", ...)`).
- Gate semantics: `src/downstream-grandpplte/drivers/clk/mediatek/clk-gate-v1.c:52-73` (`cg_set_mask`/`cg_clr_mask` for `CLK_GATE_NO_SETCLR_REG`: read-modify-write of `sta_addr`), `:92-135` (enable = clear bit, disable = set bit), `:137-155` (`is_enabled` = bit clear for non-inverse). `CLK_GATE_NO_SETCLR_REG` defined at `clk-gate-v1.h:37`.
  Nura equivalent: `mtk_clk_gate_ops_no_setclr` (`drivers/clk/mediatek/clk-gate.c:117-133` enable/disable, `:189-194` ops table), whose functions are `mtk_cg_clr_bit_no_setclr`/`mtk_cg_set_bit_no_setclr` (read-modify-write of `sta_ofs`) and `is_enabled = mtk_cg_bit_is_cleared`. Exact match, no local reimplementation needed.
- Gate flags: downstream `clk-gate-v1.c:172` sets `init.flags = CLK_IGNORE_UNUSED`; reproduced via `GATE_MTK_FLAGS(..., CLK_IGNORE_UNUSED)` so bootloader-enabled audio gates are not killed by `clk_disable_unused()`.
- Gate register is the AFE's `AUDIO_TOP_CON0`: `sound/soc/mediatek/mt6735/mt6735-afe-regs.h:24` (`#define AUDIO_TOP_CON0 0x0000`).

### Parent clock
- Downstream parent `aud_intbus_sel` = `GATE()` macro argument, `clk-mt6735.c:1011-1020`.
- Nura registers that name: `drivers/clk/mediatek/clk-mt6735-topckgen.c:352`
  `MUX_GATE_CLR_SET_UPD(CLK_TOP_AUDINTBUS_SEL, "aud_intbus_sel", ...)`;
  id `CLK_TOP_AUDINTBUS_SEL 58` at `include/dt-bindings/clock/mediatek,mt6735-topckgen.h:66`.

### MMIO coexistence
- AFE maps the same 0x11220000/0x10000 with `devm_platform_ioremap_resource(pdev, 0)` at `sound/soc/mediatek/mt6735/mt6735-afe-pcm.c:475`, i.e. it takes the `request_mem_region()` claim.
- The clock driver deliberately uses `mtk_clk_simple_probe()` with a gate-only `mtk_clk_desc`. `__mtk_clk_simple_probe()` only ioremaps when composite/divider clocks exist (`clk-mtk.c:487-496`), and `mtk_clk_register_gates()` obtains its regmap via `device_node_to_regmap()` (`clk-gate.c:267`). That path (`drivers/mfd/syscon.c:255-258` -> `:168-194` -> `of_syscon_register()` `:61`) creates the regmap over `of_iomap(np, 0)`, which does not call `request_mem_region()`. Therefore neither driver can cause `-EBUSY` for the other, regardless of probe order.
- Why it is safe at the register level: the gate ops perform a single-bit read-modify-write of AUDIO_TOP_CON0 (regmap `set_bits`/`clear_bits`), and the AFE only read-modify-writes its own bits of the same register through its separate regmap (`mt6735-afe-pcm.c:480`). This is the same access pattern downstream used (AFE and clock driver both RW the same word without a shared lock); the only theoretical hazard is a lost-update window between the two drivers' RMW sequences, identical to downstream and not made worse by this port.

## 3. Parity matrix

| Behaviour | ROM / downstream implementation | Before | After | Status |
|---|---|---|---|---|
| audiosys clock provider for `mediatek,mt6735-audiosys`, #clock-cells=1 | `mt_audiosys_init` + `of_clk_add_provider`, `clk-mt6735.c:1284-1307`; ROM DT `:3537-3543` | no provider; audio node's phandle unresolvable | platform driver + gate table registered by `mtk_clk_simple_probe` | done |
| AUDIO_AFE gate, bit 2, 0x0 | `clk-mt6735.c:1010` | absent | `audio_afe` gate, `aud_intbus_sel` parent | done |
| AUDIO_I2S gate, bit 6 | `:1011` | absent | `audio_i2s` | done |
| AUDIO_22M gate, bit 8 | `:1012` | absent | `audio_22m` | done |
| AUDIO_24M gate, bit 9 | `:1013` | absent | `audio_24m` | done |
| AUDIO_APLL2_TUNER gate, bit 18 | `:1014` | absent | `audio_apll2_tuner` | done |
| AUDIO_APLL_TUNER gate, bit 19 | `:1015` | absent | `audio_apll_tuner` | done |
| AUDIO_ADC gate, bit 24 | `:1016` | absent | `audio_adc` | done |
| AUDIO_DAC gate, bit 25 | `:1017` | absent | `audio_dac` | done |
| AUDIO_DAC_PREDIS gate, bit 26 | `:1018` | absent | `audio_dac_predis` | done |
| AUDIO_TML gate, bit 27 | `:1019` | absent | `audio_tml` | done |
| Gate polarity | enable clears bit / disable sets bit (`clk-gate-v1.c:52-73,92-135`) | n/a | `mtk_clk_gate_ops_no_setclr` | done |
| Unused-clock safety | `CLK_IGNORE_UNUSED` (`clk-gate-v1.c:172`) | n/a | same flag | done |
| Own the I/O region exclusively | downstream `get_reg()` used `of_iomap` semantics (no request_region) | n/a | `of_iomap` via `device_node_to_regmap`, no claim | done |
| Runtime PM / clock framework enable counts | plain gates in downstream | n/a | plain gates (`CLK_SET_RATE_PARENT` added by `mtk_clk_register_gate`) | done |
| DT provider node in board DTS | ROM DT `:3537-3543` | absent | fragment below (not applied, owned by board-DTS slice) | deferred (by design) |

## 4. 21-clock resolution matrix (AFE node clock-names)

Provider ids are Nura ids (the values used in a board DTS), with the corresponding downstream/ROM id in parentheses. `audiosys` = the new provider.

| # | clock-name | Nura resolution | Evidence / note |
|---|---|---|---|
| 1 | aud_afe_clk | `<&audiosys AUDIO_AFE>` (1) | downstream id 1 (`mt6735-clk.h:217`), ROM `0xf8 0x01` |
| 2 | aud_i2s_clk | `<&audiosys AUDIO_I2S>` (2) | id 2 (`:218`), ROM `0xf8 0x02` |
| 3 | aud_dac_clk | `<&audiosys AUDIO_DAC>` (8) | id 8 (`:224`), ROM `0xf8 0x08` |
| 4 | aud_dac_predis_clk | `<&audiosys AUDIO_DAC_PREDIS>` (9) | id 9 (`:225`), ROM `0xf8 0x09` |
| 5 | aud_adc_clk | `<&audiosys AUDIO_ADC>` (7) | id 7 (`:223`), ROM `0xf8 0x07` |
| 6 | aud_apll22m_clk | `<&audiosys AUDIO_22M>` (3) | id 3 (`:219`), ROM `0xf8 0x03` |
| 7 | aud_apll24m_clk | `<&audiosys AUDIO_24M>` (4) | id 4 (`:220`), ROM `0xf8 0x04` |
| 8 | aud_apll1_tuner_clk | `<&audiosys AUDIO_APLL_TUNER>` (6) | id 6 (`:222`), ROM `0xf8 0x06` |
| 9 | aud_apll2_tuner_clk | `<&audiosys AUDIO_APLL2_TUNER>` (5) | id 5 (`:221`), ROM `0xf8 0x05` |
| 10 | aud_tml_clk | `<&audiosys AUDIO_TML>` (10) | id 10 (`:226`), ROM `0xf8 0x0a` |
| 11 | aud_infra_clk | `<&infracfg CLK_INFRA_AUDIO>` (5) | `mediatek,mt6735-infracfg.h:11`; driver gate `clk-mt6735-infracfg.c:34` ("audio", parent `aud_intbus_sel`); downstream `INFRA_AUDIO`, ROM `0x1a 0x06` |
| 12 | aud_mux1_clk | `<&topckgen CLK_TOP_AUD1_SEL>` (65) | `mediatek,mt6735-topckgen.h:73`; mux `"aud_1_sel"` `clk-mt6735-topckgen.c:359`; downstream `TOP_MUX_AUD1`=25, ROM `0x03 0x19` |
| 13 | aud_mux2_clk | `<&topckgen CLK_TOP_AUD2_SEL>` (66) | header `:74`; mux `"aud_2_sel"` driver `:360`; downstream `TOP_MUX_AUD2`=26, ROM `0x03 0x1a` |
| 14 | top_ad_apll1_clk | **unavailable** | downstream `TOP_AD_APLL1_CK`=33 (`mt6735-clk.h:51`, ROM `0x03 0x21`). Nura `mediatek,mt6735-topckgen.h` has no AD_APLL1 id and `clk-mt6735-topckgen.c` registers no such clock/factor (no `ad_apll*` symbol). Must not be invented; AFE treats the name as optional. |
| 15 | top_whpll_audio_clk | `<&topckgen CLK_TOP_WHPLL_AUDIO_CK>` (4) | header `:10`; `FIXED_CLK(CLK_TOP_WHPLL_AUDIO_CK, "whpll_audio_ck", ...)` `clk-mt6735-topckgen.c:68`; downstream `TOP_WHPLL_AUDIO_CK`=75, ROM `0x03 0x4b` |
| 16 | top_mux_audio | `<&topckgen CLK_TOP_AUDIO_SEL>` (57) | header `:65`; mux `"audio_sel"` driver `:353`; downstream `TOP_MUX_AUDIO`=17, ROM `0x03 0x11` |
| 17 | top_mux_audio_int | `<&topckgen CLK_TOP_AUDINTBUS_SEL>` (58) | header `:66`; mux `"aud_intbus_sel"` driver `:352`; downstream `TOP_MUX_AUDINTBUS`=18, ROM `0x03 0x12`. Also the parent of all 10 audiosys gates and of infra `audio`. |
| 18 | top_sys_pll1_d4 | `<&topckgen CLK_TOP_SYSPLL1_D4>` (9) | header `:16`; downstream `TOP_SYSPLL1_D4`=51, ROM `0x03 0x33` |
| 19 | apmixed_apll1_clk | `<&apmixedsys CLK_APMIXED_APLL1>` (7) | `mediatek,mt6735-apmixedsys.h:13`; PLL `"apll1"` `clk-mt6735-apmixedsys.c:76`; downstream `APMIXED_APLL1`, ROM `0xf9 0x08` |
| 20 | apmixed_apll2_clk | `<&apmixedsys CLK_APMIXED_APLL2>` (8) | header `:14`; PLL `"apll2"` driver `:77`; downstream `APMIXED_APLL2`, ROM `0xf9 0x09` |
| 21 | top_clk26m_clk | `<&clk26m>` (0 cells) | fixed-clock `clk26m` in `arch/arm64/boot/dts/mediatek/mt6735.dtsi:106-110`, 26 MHz; ROM `0xfa` |

Summary: 20 of 21 resolvable; `top_ad_apll1_clk` is the only missing input (no id in the Nura topckgen binding and no clock registered by its driver). `aud_infra_clk`, `aud_mux1_clk`, `aud_mux2_clk`, `top_mux_audio`, `top_mux_audio_int`, `top_sys_pll1_d4`, `top_whpll_audio_clk` all exist in Nura with different numeric ids than the downstream/ROM numbering (Nura renumbered topckgen), so they must be referenced through the Nura headers/labels, not by raw ROM ids.

## 5. Exact compile commands + result

```
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/audio-gap-clk
cp $M/nura.config .config
./scripts/config --enable SOUND --module SND --module SND_SOC --module SND_SOC_MEDIATEK \
                 --enable COMMON_CLK_MT6735 --module COMMON_CLK_MT6735_AUDIOSYS --module COMMON_CLK_MT6735
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/clk/mediatek/clk-mt6735-audiosys.o \
                          drivers/clk/mediatek/clk-mt6735-topckgen.o
```

Result: both objects compiled, 0 warnings:

```
  CC [M]  drivers/clk/mediatek/clk-mt6735-audiosys.o
  CC [M]  drivers/clk/mediatek/clk-mt6735-topckgen.o
```

`.config` confirmation: `CONFIG_COMMON_CLK_MT6735=m`, `CONFIG_COMMON_CLK_MT6735_AUDIOSYS=m`.
Patch check: extracted `drivers/clk/mediatek/{Kconfig,Makefile}` at HEAD into a scratch dir and ran
`git apply --check $M/patches/audio-gap-clk.patch` -> `PATCH_OK`. (The baseline `audio-integration.patch`
does not touch `drivers/clk/mediatek`, so HEAD context equals the staged baseline context for these files.)

## 6. Inferred / unverified items

- `[INFERENCE]` The AFE's `devm_clk_bulk_get_optional()` defers probe (not skip) when a listed phandle's
  provider is not yet registered (`-EPROBE_DEFER` is not treated as "optional"), so probe order between
  `clk-mt6735-audiosys` and the AFE is self-resolving. Standard CCF behaviour, not runtime-tested here.
- `[INFERENCE]` `CLK_IGNORE_UNUSED` on the gates is desirable but not strictly required once the AFE has
  claimed and enabled the clocks; it is included to match downstream exactly.
- Not verified at runtime (no device boot in this task): actual gate enable/disable and the shared
  AUDIO_TOP_CON0 read-modify-write were only compile-tested. No hardware/register-level readback was done.
- `top_ad_apll1_clk` remains unresolved: adding `CLK_TOP_AD_APLL1_CK` would require editing the topckgen
  binding/driver, which is outside this task's owned set.

## 7. DTS-FRAGMENT

Not applied. Board DTS is owned by another slice. Two-cell address/size cells to match the `simple-bus`
in `mt6735.dtsi`; interrupt number from ROM DT `rom_dtb_0.dts:3546-3548` (SPI 144, level low).

```dts
	audiosys: clock-controller@11220000 {
		compatible = "mediatek,mt6735-audiosys";
		reg = <0 0x11220000 0 0x10000>;
		#clock-cells = <1>;
	};

	afe: afe@11220000 {
		compatible = "mediatek,mt6735-afe";
		reg = <0 0x11220000 0 0x10000>;
		interrupts = <GIC_SPI 144 IRQ_TYPE_LEVEL_LOW>;

		clocks = <&audiosys AUDIO_AFE>,
			 <&audiosys AUDIO_I2S>,
			 <&audiosys AUDIO_DAC>,
			 <&audiosys AUDIO_DAC_PREDIS>,
			 <&audiosys AUDIO_ADC>,
			 <&audiosys AUDIO_22M>,
			 <&audiosys AUDIO_24M>,
			 <&audiosys AUDIO_APLL_TUNER>,
			 <&audiosys AUDIO_APLL2_TUNER>,
			 <&audiosys AUDIO_TML>,
			 <&infracfg CLK_INFRA_AUDIO>,
			 <&topckgen CLK_TOP_AUD1_SEL>,
			 <&topckgen CLK_TOP_AUD2_SEL>,
			 <&topckgen CLK_TOP_WHPLL_AUDIO_CK>,
			 <&topckgen CLK_TOP_AUDIO_SEL>,
			 <&topckgen CLK_TOP_AUDINTBUS_SEL>,
			 <&topckgen CLK_TOP_SYSPLL1_D4>,
			 <&apmixedsys CLK_APMIXED_APLL1>,
			 <&apmixedsys CLK_APMIXED_APLL2>,
			 <&clk26m>;
		clock-names = "aud_afe_clk",
			      "aud_i2s_clk",
			      "aud_dac_clk",
			      "aud_dac_predis_clk",
			      "aud_adc_clk",
			      "aud_apll22m_clk",
			      "aud_apll24m_clk",
			      "aud_apll1_tuner_clk",
			      "aud_apll2_tuner_clk",
			      "aud_tml_clk",
			      "aud_infra_clk",
			      "aud_mux1_clk",
			      "aud_mux2_clk",
			      "top_whpll_audio_clk",
			      "top_mux_audio",
			      "top_mux_audio_int",
			      "top_sys_pll1_d4",
			      "apmixed_apll1_clk",
			      "apmixed_apll2_clk",
			      "top_clk26m_clk";
	};
```

`top_ad_apll1_clk` is intentionally absent from both properties because it does not exist in the Nura
topckgen binding; `devm_clk_bulk_get_optional()` leaves that slot NULL. If the topckgen driver later gains
the clock, add `<&topckgen CLK_TOP_AD_APLL1_CK>` / `"top_ad_apll1_clk"` in the corresponding positions.
Include the new header in the board DTS:
`#include <dt-bindings/clock/mediatek,mt6735-audiosys.h>`.

## 8. Patch path

`$M/patches/audio-gap-clk.patch` (4 files, +143 lines).
