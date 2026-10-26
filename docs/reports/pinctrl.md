# MT6735/MT6737T pinctrl parity report

## 1. Assigned driver and files changed

- Driver: `drivers/pinctrl/mediatek/pinctrl-mt6735.c` (Nura `v7.0.10-mt6735`, worktree `$M/worktrees/pinctrl`)
- Files changed: `drivers/pinctrl/mediatek/pinctrl-mt6735.c` only.
- `pinctrl-mtk-mt6735.h`, `pinctrl-paris.c`, `mtk-eint.c`, Kconfig/Makefile: **not** touched. The mainline
  `mtk_pin_desc` / `mtk_pin_field_calc` model is preserved; only data was corrected.

The pin/function table itself needed **no additions**: every pin and mux value used by the ROM
(`mtk_pins_mt6735` at `0xc0a5d2f4`) already exists in the Nura header. The parity gaps were in the
register-range tables (`ies`, `smt`, `tdsel`, `rdsel`, `drv`, `r0/r1/pupd`), in `mt6735_pull_type[]`
and in the EINT count.

## 2. ROM evidence

### 2.1 ROM pin table (names / functions / EINT)

`readelf -sW zip/vmlinux.elf`:

```
mt6735_pinctrl_data  c0a5d214  OBJECT LOCAL
mtk_pins_mt6735      c0a5d2f4  OBJECT LOCAL
```

The ROM is built from an older `struct mtk_desc_pin` (24 bytes: `pinctrl_pin_desc`, `pad`, `chip`,
`eint{u8,u8}`, `functions`). Dumping the table shows exactly **204 entries (GPIO0..GPIO203)**, all with
`eint = {255,255}` (i.e. `NO_EINT_SUPPORT`) and function lists identical to
`$M/src/downstream-grandpplte/drivers/pinctrl/mediatek/pinctrl-mtk-mt6735.h` and to the Nura header.
Comparison result: 0 missing pins, 0 missing functions, 0 mux-value differences. Only difference in names:
ROM/downstream say `PCM1_CLK[0]` on GPIO61, Nura says `PCM1_CLK` (cosmetic; GPIO61 mux 1 is identical;
not used by any DTS and not changed).

The ROM has no pins 204-212; Nura's 9 extra entries are upstream placeholders with
`MTK_FUNCTION(0, NULL)` and `DRV_FIXED`.

### 2.2 ROM GPIO register arrays

The downstream `drivers/misc/mediatek/gpio/mt6735/gpio_cfg.h` "Denali1 & Denali3" branch (from
line 5050) is the register map the ROM actually runs. Each packed array was found byte-identical in
the ROM image (`zip/vmlinux.elf`, `.kernel` at vaddr `0xc0004000`, file offset `0x124`):

| array | ROM vaddr |
|---|---|
| `IES_addr` | `0xc0e79798` |
| `SMT_addr` | `0xc0e7939c` |
| `PULLEN_addr` | `0xc0e78c70` |
| `PULL_addr` (pull select) | `0xc0e79b94` |
| `pupd_addr` | `0xc0e78fa0` |
| `MODE_addr` | `0xc0e7a788` |
| `DATAOUT_addr` | `0xc0e79f90` |
| `DIN_addr` | `0xc0e7a38c` |
| `DIR_addr` | `0xc0e787a8` |
| `DRV_addr` | `0xc0e7ac50` |
| `DRV_offset` | `0xc0e7aab8` |
| `DRV_width` | `0xc0e7ab84` |
| `TDSEL_addr` | `0xc0e7b448` |
| `RDSEL_addr` | `0xc0e7b04c` |

`mt_set_gpio_driving_base` @ `0xc03edd54` (ARM disassembly) confirms the encoding: it rejects pin > 203,
reads `DRV_width[pin]` (1, 2 or 3 bits), `DRV_offset[pin]`, and writes
`((1 << width) - 1) << offset` at `DRV_addr[pin]`. This is the encoding reproduced in the tables below.

So for every register type the authoritative per-pin `(offset, bit, width)` was extracted from those
ROM arrays and compared against the expansion of the Nura `mtk_pin_field_calc` tables.

### 2.3 Special pull (R0/R1/PUPD)

`mt6735_spec_pupd` @ `0xc0a5cecc` (60 entries, matching
`downstream .../pinctrl-mt6735.c:54-96`). It gives, for the special pins, the absolute register offset
and the `pupd_bit` / `r1_bit` / `r0_bit`. Nura's `r0`/`r1`/`pupd` tables matched for 45, 46, 81-86 and
166-183 but were off for 160-165 (SIM1/SIM2), as shown in the matrix below.

### 2.4 EINT

Downstream `arch/arm64/boot/dts/mt6735.dtsi:361-362` (and `mt6735m.dtsi:690-691`):

```
mediatek,max_eint_num = <213>;
mediatek,mapping_table_entry = <0>;
```

EINT number == GPIO number on this SoC (mapping table empty). `irqchip/irq-mt-eic.c:1848` defaults
`MAX_HW_DEBOUNCE_CNT` to 16 when the DT property is absent, matching `.db_cnt = 16`.
The EINT register offsets in `include/linux/irqchip/mt-eic.h:20-40` are identical to the generic
`mtk_eint_regs` used by `mtk-eint.c`, so no EINT register work was required.

## 3. Parity matrix (before -> after)

Legend: ROM source = `gpio_cfg.h` / `mt6735_spec_pupd` / downstream DTS as above.

| # | Behaviour | ROM implementation | Nura before | After | Status |
|---|---|---|---|---|---|
| 1 | IES enable for GPIO42-44 (CMDAT0/CMDAT1/CMPCLK) | `IES_addr=0x900`, bit 0 | missing range -> `-EINVAL` | `PINS_FIELD_BASE(42,44,2,GPIO_IES,16,0,0,1)` | done |
| 2 | SMT for GPIO42-44 | `SMT_addr=0x910`, bit 0 | missing | added | done |
| 3 | IES/SMT for GPIO163-165 (SIM1) | iocfg4 `0xC00`, bit 1 | iocfg3 `0xB00`, bit 1 (wrong block) | i_base 5 (iocfg4) | done |
| 4 | TDSEL for GPIO42-44 | `0x920`, bit 0, 4 bits | missing | added | done |
| 5 | RDSEL for GPIO42-44 | `0x928`, bit 0, 2 bits | missing | added | done |
| 6 | RDSEL for GPIO137-149 | `0xB28` bits 2,4,6,8,10,2 | bits 4,6,8,10,12,4 (+2) | corrected | done |
| 7 | RDSEL for GPIO186-189 (WB SPI) | `0xD2C`, bit 0 | `0xD28`, bit 0 | `x_addrs=0x04` (0xD2C) | done |
| 8 | Drive strength, all pins | bit offsets from `DRV_offset[]`, widths 1/2/3 | every field one bit too high (`s_bit` = ROM bit + 1) | full table regenerated from ROM arrays | done |
| 9 | Drive strength GPIO47-54 (I2C0-3) | 1-bit fields at bits 8/12/16/20 | no range -> unsupported | added | done |
| 10 | Drive strength GPIO103 | `DRV_width = -1` (unsupported) | inside 87-103 range (writable) | range ends at 102 | done |
| 11 | Drive strength GPIO42-44 | `DRV_width = -1` | unsupported | unsupported (unchanged) | done |
| 12 | R0/R1/PUPD for GPIO160-165 | `spec_pupd`: r0 bits 1,5,9,17,21,25; r1 +1; pupd +2 | r0 0,4,12,16,20,24; r1 +1; pupd +2 | corrected to ROM bits | done |
| 13 | `mt6735_pull_type[]` indexing | 204 pins classified by `PULLEN_addr`/`pupd_addr` | 203 entries; GPIO59 entry missing so every index >= 59 was shifted (GPIO80 wrongly R1R0, GPIO202/203 OOB) | entry 59 added; entries 204-212 added; 213 entries | done |
| 14 | EINT channel count | `max_eint_num = 213` | `ap_num = 224` (EINT 213-223 would index past `mtk_pins_mt6735`) | `ap_num = 213` | done |
| 15 | Pin/function table | 204 pins, all mux values | 204 pins + 9 placeholders, all mux values matching | unchanged (already matching) | done |
| 16 | EINT-mux per pin | ROM pin table stores `NO_EINT_SUPPORT`; EINT==GPIO number | `MTK_EINT_FUNCTION(0, pin)` for all pins | unchanged (equivalent model, matches `mapping_table_entry = 0`) | done |
| 17 | `gpio-ranges` in `mt6735.dtsi` | 204 GPIOs in ROM pin table | `<&pio 0 0 203>` (GPIO203 inaccessible) | not changed (shared DTS) | deferred, fragment below |

No hardware was available; all items are source/ROM validated and compile-tested, not run on a phone.

## 4. DTS validation

`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts` was parsed and every reference resolved
against the driver header / binding header:

| DTS reference | pin | mux | found |
|---|---|---|---|
| `PINMUX_GPIO3__FUNC_GPIO3` (accelerometer INT1) | 3 | 0 | yes |
| `PINMUX_GPIO7__FUNC_GPIO7` (backlight enable) | 7 | 0 | yes |
| `PINMUX_GPIO10__FUNC_SDA3` / `GPIO11__FUNC_SCL3` | 10/11 | 6 | yes |
| `PINMUX_GPIO19__FUNC_GPIO19` (tflash CE) | 19 | 0 | yes |
| `PINMUX_GPIO69__FUNC_DISP_PWM` | 69 | 1 | yes |
| `PINMUX_GPIO76__FUNC_GPIO76` (fuel gauge INT) | 76 | 0 | yes |
| `PINMUX_GPIO85__FUNC_GPIO85` / `GPIO86__FUNC_GPIO86` | 85/86 | 0 | yes |
| `PINMUX_GPIO146__FUNC_GPIO146` (panel reset) | 146 | 0 | yes |

Direct `&pio` users: GPIO5 (SD card detect), 7, 19, 85, 86, 146, 184; EINT users: 3, 62, 76 — all
present in `mtk_pins_mt6735`. No new function names are required, so no DTS change is needed for this
board.

## 5. Compile test

```sh
cd $M/worktrees/pinctrl
export PATH="$M/bin:$PATH"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy \
  OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf \
  STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/pinctrl/mediatek/pinctrl-mt6735.o
```

Result: `CC drivers/pinctrl/mediatek/pinctrl-mt6735.o` — success, no warnings/errors.
(Only this object was built; no full kernel build was run.)

## 6. Inferred / unverified

- `[INFERENCE]` `ap_num = 213` is taken from the vendor DTS `max_eint_num`. The ROM pin table has
  `NO_EINT_SUPPORT` for every pin, so the exact EINT/GPIO relationship is derived from the vendor EIC
  driver (`mt_gpio_to_irq` = `EINT_IRQ_BASE + gpio` with an empty mapping table), not from a ROM
  symbol. 213 also matches the number of EINTs declared for both `mt6735.dtsi` and `mt6735m.dtsi`.
- The nine pins 204-212 exist only in the Nura header (placeholders). They have no ROM register data;
  their pull type is set to "none" and they remain outside `gpio-ranges`.
- Drive-strength scaling (`mtk_drive[]` / `DRV_GRP*` in the pin descriptors) is unchanged; the ROM's
  legacy driver has no equivalent per-pin group table, so only the field position/width could be
  validated, not the mA scaling.
- `MTK_PULL_*` classification was derived from the ROM array validity (`PULLEN_addr != -1` vs
  `pupd_addr != -1`); this matches the vendor `mt_set_gpio_pull_enable_base` branching.
- The EINT debounce register programming and `db_cnt = 16` are unchanged and match the vendor default.
- Hardware-unverified: none of these paths were exercised on a device.

## 7. DTS fragment (not applied; shared DTS)

`arch/arm64/boot/dts/mediatek/mt6735.dtsi` currently maps only 203 GPIOs while the ROM pin table and the
driver expose 204 real pins. Suggested fragment (shared file, not edited here):

```dts
&pio {
	gpio-ranges = <&pio 0 0 204>;
};
```

This is not required for the grandpplte board (max used GPIO is 184), but it makes GPIO203
(MSDC2_DAT3) reachable and matches the 204-entry ROM pin table. Pins 204-212 are upstream placeholders
with no register ranges and should stay outside the mapping.

## 8. Patch

`$M/patches/pinctrl.patch` (`git diff --binary`, single file:
`drivers/pinctrl/mediatek/pinctrl-mt6735.c`, +78/-59 lines).
