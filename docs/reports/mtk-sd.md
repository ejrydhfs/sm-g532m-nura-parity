# mtk-sd parity report (MT6735/MT6737 eMMC + SD)

## 1. Scope

| Item | Value |
|---|---|
| Worktree | `$M/worktrees/mtk-sd` |
| Driver | `drivers/mmc/host/mtk-sd.c` |
| Files changed | `drivers/mmc/host/mtk-sd.c` only |
| Binding | `mediatek,mt6735-mmc` / `mt6735_compat` unchanged |
| Patch | `$M/patches/mtk-sd.patch` |

Nothing outside the driver was modified. Shared DTS files were not touched;
required fragments are in section 6.

`mt6735_compat` is intentionally unchanged:

```c
static const struct mtk_mmc_compatible mt6735_compat = {
	.clk_div_bits = 12, .recheck_sdio_irq = false, .hs400_tune = false,
	.pad_tune_reg = MSDC_PAD_TUNE0, .async_fifo = false, .data_tune = true,
	.busy_check = false, .stop_clk_fix = false, .enhance_rx = false,
	.support_64g = true,
};
```

All new devicetree properties are optional; with the current Nura DTS the
only behaviour changes are the HS400 command internal delay (the property is
already present and was previously dead), the HS400 data start bit, the SD
power-on clock sequence and 10 ms rail-settling delays.

## 2. Evidence

### 2.1 Downstream source (`$M/src/downstream-grandpplte`)

All paths below are relative to
`drivers/mmc/host/mediatek/mt6735/` unless stated otherwise.

| Downstream symbol / code | Lines | What it establishes |
|---|---|---|
| `msdc_set_smpl()` | `sd.c:1049-1147` | CMD response / read data / write CRC sampling edges, `R_D_SMPL_SEL`/`W_D_SMPL_SEL`=0, HS400 clears `EMMC50_CFG0.PADCMD_LATCHCK` and `CMD_RESP_SEL` |
| `msdc_set_startbit()` + `MSDC_CFG_START_BIT` | `sd.c:1039-1047`, `mt_sd.h:337` | `START_BIT` is `0x3 << 23`; HS400 uses `START_AT_RISING_AND_FALLING` (2), all other modes `START_AT_RISING` (0), `sd.c:1097-1104` |
| `msdc_set_mclk()` | `sd.c:2289-2398` | HS400 branch, `set_smpl()` for cmd/read/write on every clock change (`sd.c:2367-2379`) |
| `msdc_apply_ett_settings()` | `sd.c:7308-7370` | HS200 ETT table applied on HS200; on HS400 the HS200 fields are first zeroed, then the HS400 table is applied; HS400 saves DS DLY1/DLY3 |
| `msdc_ops_set_ios()` | `sd.c:7392-7450` | ETT application on timing change, SD clock run on `MMC_POWER_ON`, power mode handling |
| `msdc_sd_clock_run()` | `sd.c:7381-7388` | 400 kHz identification clock, clock stop, `mdelay(1)`, clock restart |
| `msdc_set_power_mode()` | `sd.c:2913-2955` | `mdelay(10)` after power on and after power off |
| `msdc_get_cd()` / `cd_level` | `sd.c:7479-7540` | GPIO card detect; `cd_level=0` means low = inserted |
| `emmc_hs400_backup()` / `emmc_hs400_restore()` | `sd.c:6014-6101` | Registers that differ between HS400 and the 4.5 backward modes |
| `emmc_hs400_tune_rw()` | `sd.c:6102-6160` | HS400 DLY1 re-tune on data errors |
| `msdc_tune_cmdrsp()` / `msdc_tune_rw_request()` | `sd.c:5714-5824`, `sd.c:5150-5230` | Vendor re-tune on command/data CRC errors |
| `msdc_pm()` / `msdc_drv_suspend()` / `msdc_drv_resume()` | `sd.c:3161-3290`, `sd.c:9337-9460` | System suspend: save, optional power off, pin pull-down; resume: reset, pull-up, power on, `mdelay(10)`, restore |
| `msdc_cache_ctrl()`, `msdc_set_cache_quirk()` | `sd.c:3297-3455` | eMMC cache control through CMD6; quirk table is empty in this tree |
| `emmc_rpmb.c` | `$M/src/downstream-grandpplte/drivers/mmc/host/mediatek/emmc_rpmb.c:234-360` | RPMB is implemented on top of generic `mmc_wait_for_req()` CMD23/CMD25/CMD18; no controller register sequences |

Downstream board DTS (`arch/arm/boot/dts/mt6737t_grandpplte_cis_ser_00.dts`):

* `mmc0` (`:697-714`): `max-frequency = <200000000>`, `mmc-hs200-1_8v`,
  `mmc-hs400-1_8v`, `non-removable`, `clk_src = MSDC50_CLKSRC_400MHZ`.
* `mmc1` (`:716-736`): `max-frequency = <200000000>`, SD UHS modes,
  `cd-gpios = <&pio 5 GPIO_ACTIVE_LOW>`, `cd_level = MSDC_CD_LOW`.
* `mmc0@register_default` (`:759-797`): `cmd_edge/rdata_edge/wdata_edge =
  MSDC_SMPL_FALLING`, `ett-hs200-default` (12 entries),
  `ett-hs400-default` (8 entries).
* `mmc1@default` / `mmc1@sdr104` (`:800-854`): drive strengths 3 for the
  default state, 2/2/3 for cmd/dat/clk in SDR104/SDR50/DDR50.

### 2.2 ROM binary (`$M/zip/vmlinux.elf`, symbols from `$M/zip/symbols.txt`)

Vendor DTB extracted from `$M/zip/boot.img` (`/tmp/rom3.dtb`,
`model = "Samsung GrandPrimePlus LTE LTN OPEN rev04"`). It confirms the
downstream DTS values:

* `msdc0@11230000`: `max-frequency = <0xbebc200>` (200 MHz), `clk_src = [02]`,
  `mmc-hs200-1_8v`, `mmc-hs400-1_8v`, `emmc_power_ao`, `non-removable`.
* `msdc1@11240000`: `max-frequency = <0xbebc200>`, `sd-uhs-sdr104`,
  `cd-gpios = <&pio 5 0>`, `cd_level = [00]`.
* `mmc0@register_default`: `cmd_edge/rdata_edge/wdata_edge = [01]`,
  `ett-hs200-cells = <0x0c>`, `ett-hs400-cells = <0x08>`, table values
  identical to the downstream DTS (`PAD_TUNE0` CMDRDLY/CMDRRDLY/DATWRDLY/
  DATRRDLY = 7/11/11/9 for HS200, 6/6 for HS400, `PAD_DS_TUNE` DLY3 = 0x10).
* `mmc1@register_default`: all three edges = `[01]`.

ROM disassembly (`arm` ELF, Capstone used because the installed binutils has
no ARM target):

| Address | Symbol | Observation |
|---|---|---|
| `0xc074f6e8` | `msdc_set_smpl` | type 2 (read data) branch at `0xc074f7dc`: read `MSDC_CFG`, clear `0x1800000` (`START_BIT` mask `3<<23`), set `0x1000000` (value 2) when HS400; also writes IOCON `R_D_SMPL_SEL`/`R_D_SMPL`. Type 0 branch writes `RSPL`, type 1 writes `W_D_SMPL`/CRC status edge. |
| `0xc074f6c0` | `msdc_set_startbit` | `MSDC_CFG_START_BIT` field write helper |
| `0xc074fab4` | `msdc_set_mclk` | calls `msdc_set_smpl` three times at `0xc074fb90`, `0xc074fbac`, `0xc074fbc8` (HS400) and `0xc074fcb8`, `0xc074fcd4`, `0xc074fcf0` (other modes); does **not** write the pad-tune registers |
| `0xc074b8d8` | `msdc_apply_ett_settings` | mode 3/4 branches; loops over `(offset, mask, value)` triples and `sdr_set_field()`s them; reads back `PAD_TUNE0`, `DAT_RDDLY0/1`, `PATCH_BIT1` |
| `0xc0755074` | `msdc_ops_set_ios` | `msdc_set_mclk` at `0xc07554ac`, `msdc_apply_ett_settings` at `0xc07554c0` (after set_mclk, matching this patch's ordering because Nura's `set_mclk` rewrites the pad-tune registers) |
| `0xc0755018` | `msdc_sd_clock_run` | distinct function, called from set_ios |
| `0xc074d15c` | `msdc_set_power_mode` | power on/off helper (10 ms settle in source) |
| `0xc074ca2c` | `msdc_ops_get_cd` | GPIO card detect with `cd_level` polarity |
| `0xc07527f0` | `msdc_cache_ctrl` | CMD6 `CACHE_CTRL` |
| `0xc0753a5c` | `msdc_tune_cmdrsp` | vendor re-tune |
| `0xc0754a2c` | `msdc_tune_read` | vendor re-tune |
| `0xc0754e80` | `msdc_tune_write` | vendor re-tune |
| `0xc07548b0` | `emmc_hs400_tune_rw` | HS400 DLY1 re-tune |
| `0xc074e630` | `msdc_reset_hw` | reset helper used on command/data errors and timeouts |
| `0xc0a0e0ac` | `msdc_enable_emmc_cache` | cache enable |
| `0xc0a0d938` | `msdc_check_emmc_cache_status` | cache status |
| `0xc07649e0` | `emmc_rpmb_send_command` | RPMB via generic MMC requests |

## 3. Parity matrix

| # | Behaviour | ROM / downstream implementation | Nura before | Nura after | Status |
|---|---|---|---|---|---|
| 1 | HS400 command response internal delay (`CMDRRDLY`) | ROM ETT HS400 `PAD_TUNE0 CMDRRDLY = 6`; downstream ETT `sd.c:7308-7370`; current DTS already sets `mediatek,hs400-cmd-int-delay = <6>` | property parsed but only applied when `dev_comp->hs400_tune` (false for mt6735) — dead | applied whenever timing changes to HS400 and the property is non-zero | done |
| 2 | HS400 data start bit | ROM `msdc_set_smpl` at `0xc074f7dc`; downstream `sd.c:1097-1104` | never programmed | `MSDC_CFG_START_BIT = 2` (both edges) in HS400, `0` otherwise, set in `msdc_set_mclk()` | done |
| 3 | HS200/HS400 ETT register tables (CMD/DAT delays, turnaround) | `msdc_apply_ett_settings()` `sd.c:7308-7370`; ROM DTB tables; ROM function `0xc074b8d8` | not supported | optional `ett-hs200-cells`/`ett-hs200-default`/`ett-hs200-customer` and HS400 equivalents parsed at probe and applied on HS200/HS400 entry; HS200 fields zeroed before HS400 table | done (DT-gated) |
| 4 | CMD response / read data / write CRC sampling edge | downstream `register_setting` `cmd_edge/rdata_edge/wdata_edge`; ROM values all `1` (falling) | always rising except where tuning overrides | optional `mediatek,cmd-edge`, `mediatek,rdata-edge`, `mediatek,wdata-edge`; applied in `msdc_set_mclk()` and re-applied after the ETT table | done (DT-gated) |
| 5 | HS400 DS delay | ROM ETT: DLY3 = 0x10 (16); DLY1 found by `execute_hs400_tuning()` | DLY1 tuned; DLY3 from DTS, currently `10` | driver untouched for DLY3 (uses DTS); fragment corrects it to 16; ETT table also writes it | partial (DTS value) |
| 6 | HS200 tuning | downstream `msdc_tune_cmdrsp/read/write`; Nura `msdc_tune_response`/`msdc_tune_data` | present, `data_tune=true`, tuning step 16 | unchanged | done (existing, not regressed) |
| 7 | HS400 tuning | downstream `emmc_hs400_tune_rw()` + DLY1 scan; Nura `execute_hs400_tuning()` scans DLY1 | present | unchanged; plus #1/#2/#3 | done (existing + additions) |
| 8 | SD power-on clock sequence | `msdc_sd_clock_run()` `sd.c:7381-7388`; ROM `0xc0755018` | absent | 400 kHz, `CKPDN` assert, `mdelay(1)`, deassert on `MMC_POWER_ON` for removable, SD-capable hosts | done |
| 9 | Power rail settling | `msdc_set_power_mode()` `sd.c:2913-2955` | none | `mdelay(10)` after power-on and after power-off | done |
| 10 | SD card power rails (external TFLASH LDO + PMIC VMC IO) | `msdc_sd_power()`/`msdc_sd_power_switch()` (`sd.c:1641-1670`, 1726-1770); board rev >= 2 uses external LDO (`sd.c:1161-1225`, `SDCARD_USE_EXTERNAL_LDO`) | `vmmc=vdd_tflash`, `vqmmc=vdd_tflash` (fixed 3.0 V, cannot switch to 1.8 V) | driver already supports both rails through regulators/pinctrl; DTS fragment changes `vqmmc` to `&mt6328_vmc_reg` | partial (DTS-only) |
| 11 | Card-detect polarity | `msdc_ops_get_cd()` + `cd_level = MSDC_CD_LOW`; ROM DTB `cd-gpios = <&pio 5 0>` | `cd-gpios = <&pio 5 GPIO_ACTIVE_LOW>` maps low to inserted, same as ROM; internal-CD path handles `MMC_CAP2_CD_ACTIVE_HIGH` | unchanged; verified equivalent | done (no change needed) |
| 12 | Suspend/resume | downstream saves/restores eMMC registers and re-runs `msdc_set_mclk()` (`sd.c:3080-3160`, 3161-3290) | `msdc_save_reg()`/`msdc_restore_reg()` + `pm_runtime_force_suspend`, core powers the card off/on through `set_ios` | unchanged; the added power delays apply on the core's power-off/on | done (existing) |
| 13 | eMMC sleep / DAT0 before power-off | downstream `msdc_emmc_power()` waits for DAT0 using `sa_timeout` | `mmc_sleep()` polls `card_busy()` (MSDC_PS DAT0) with `sa_timeout` because `MMC_CAP_WAIT_WHILE_BUSY` is not set | unchanged; behaviour already provided by the core | done (core) |
| 14 | eMMC cache enable + flush | downstream CMD6 cache control; quirk list empty | core enables cache when `ext_csd.cache_size > 0` and flushes on suspend (`mmc.c`, `_mmc_flush_cache()`); block layer flushes on barrier/FUA | unchanged | done (core) |
| 15 | RPMB | downstream `emmc_rpmb.c` is a TEE/DCI proxy over generic MMC requests | core RPMB char device (`block.c`, `rpmb` bus) | unchanged; no controller-level sequence exists in downstream | deferred (core feature, not driver) |
| 16 | Error recovery | downstream re-tunes on CRC error, lowers clock, power-cycles SD, marks bad card | core flags retune on `-EILSEQ` in `mmc_request_done()`; host resets HW on CMDTMO/DATTMO/CRC in `msdc_cmd_done()`/`msdc_data_xfer_done()`; CMD retries in core | unchanged; downstream's private re-tune/reinit loops not duplicated | partial (core + existing) |
| 17 | eMMC kept powered during suspend (`emmc_power_ao`) | downstream skips `power_control(0)` when the board sets `emmc_power_ao` | core always powers the card off on system suspend | unchanged | deferred (power policy, hardware-unverified) |
| 18 | HS400 fallback register restore (`emmc_hs400_restore()`) | downstream restores the 4.5-mode register set when leaving HS400 | only the pad-tune subset is restored by `msdc_set_mclk()`; `PATCH_BIT2.CFGCRCSTS`/`EMMC50_CFG3` are not | unchanged | deferred (no hardware to validate the fallback path) |

## 4. Compile test

From `$M/worktrees/mtk-sd`, with `$M/bin` first in `PATH`:

```sh
export PATH="$M/bin:$PATH"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy \
  OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf \
  STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/mmc/host/mtk-sd.o
```

Result:

```
  CC      drivers/mmc/host/mtk-sd.o
```

No warnings or errors. `checkpatch.pl --no-tree` on the diff: `0 errors, 0
warnings, 300 lines checked`.

Only `drivers/mmc/host/mtk-sd.c` is modified (`git status --short` shows one
entry).

## 5. Inferred / hardware-unverified

* `[INFERENCE]` The ETT ordering: the ROM calls `apply_ett` after `set_mclk`
  in `msdc_ops_set_ios()` (`0xc07554ac` then `0xc07554c0`). Nura's
  `msdc_set_mclk()` rewrites the pad-tune registers from
  `def_tune_para`/`saved_tune_para`, so the table is applied after
  `msdc_set_mclk()` here. The ROM does not rewrite those registers in
  `set_mclk`, which is why the vendor source can apply the table earlier.
* `[INFERENCE]` `msdc_set_sample_edges()` is re-run after the ETT table so
  that the DTS edge values win, matching ROM `set_mclk -> set_smpl` ordering
  for clock changes. Without the new DTS properties this is a no-op.
* The exact `PAD_DS_TUNE` DLY3 effect on this board cannot be measured
  without hardware; the ROM value is 16 and the current DTS says 10.
* The HS400 fallback path (leaving HS400 for HS200/HS after repeated errors)
  is not exercised; see matrix row 18.
* `emmc_power_ao` is not implemented. On this ROM the eMMC rail is described
  as always-on during suspend; Nura's core powers the card off. This is a
  power-policy difference, not a data-path difference.
* RPMB and cache are core features in this kernel tree; no
  `mtk-sd.c` register programming is required by the downstream code.
* SD UHS depends on the `vqmmc` rail change in section 6, which is not part
  of this patch. With the current fixed 3.0 V `vqmmc` the card will stay at
  3.3 V signalling and UHS modes will not be enabled.
* eMMC performance still depends on `max-frequency`; the current Nura DTS
  caps `mmc0` at 20 MHz, so HS200/HS400 are entered but run at 20 MHz. The
  ROM value is 200 MHz (fragment below). Verify the 400 MHz MSDC source
  clock (`MSDC50_CLKSRC_400MHZ`, `clk_src = 2`) before enabling 200 MHz.

## 6. DTS fragment (not applied; shared DTS untouched)

### 6.1 eMMC (`&mmc0`)

The ROM DTB (`rev04`) and the downstream `register_setting` node give these
values. `ett-hsX-cells` is the number of `(offset, mask, value)` entries
(the driver reads `cells * 3` u32 cells), matching downstream
`sd.c:8569-8600`.

```dts
&mmc0 {
	/* ROM DTB: 0xbebc200 = 200 MHz.  Verify the 400 MHz source first. */
	max-frequency = <200000000>;

	/* ROM DTB register_setting: all edges = 1 (falling) */
	mediatek,cmd-edge = <1>;
	mediatek,rdata-edge = <1>;
	mediatek,wdata-edge = <1>;

	/* ROM DTB: EMMC50_PAD_DS_TUNE DLY3 = 0x10; current DTS has 10 */
	mediatek,hs400-ds-dly3 = <16>;

	/* ROM DTB: ett-hs200-default, 12 entries */
	ett-hs200-cells = <12>;
	ett-hs200-default = <
		0x0b0 0x00000380 0x00
		0x0b0 0x00007c00 0x00
		0x0b4 0x00000038 0x01
		0x004 0x00000002 0x00
		0x0f0 0x001f0000 0x07
		0x0f0 0x07c00000 0x0b
		0x0b4 0x00000007 0x01
		0x0f0 0x0000001f 0x0b
		0x004 0x00000400 0x00
		0x0f8 0x1f000000 0x07
		0x0f0 0x00001f00 0x09
		0x004 0x00000004 0x00>;

	/* ROM DTB: ett-hs400-default, 8 entries */
	ett-hs400-cells = <8>;
	ett-hs400-default = <
		0x0b0 0x00000380 0x00
		0x0b0 0x00007c00 0x00
		0x188 0x0000007c 0x02
		0x188 0x0001f000 0x10
		0x0b4 0x00000038 0x01
		0x004 0x00000002 0x00
		0x0f0 0x001f0000 0x06
		0x0f0 0x07c00000 0x06>;
};
```

`&mmc0 { mediatek,hs400-cmd-resp-sel-rising; }` and the existing
`mediatek,hs200-cmd-int-delay = <11>` / `mediatek,hs400-cmd-int-delay = <6>`
stay as-is; they match the ROM ETT values.

Optional drive-strength correction for the eMMC UHS state (the ROM uses
drive strength 2 for all eMMC pins in both states, Nura's `state_uhs` uses
4):

```dts
&mmc0_pins_uhs {
	pins-cmd-clk-dat {
		drive-strength = <2>;
	};
};
```

### 6.2 SD (`&mmc1`)

```dts
&mmc1 {
	/* ROM DTB: 0xbebc200 = 200 MHz (MSDC30_CLKSRC_200MHZ) */
	max-frequency = <200000000>;

	/* ROM DTB register_setting: all edges = 1 (falling) */
	mediatek,cmd-edge = <1>;
	mediatek,rdata-edge = <1>;
	mediatek,wdata-edge = <1>;

	/*
	 * Card power stays on the external TFLASH LDO (GPIO19).  The IO rail
	 * must be PMIC VMC (1.8 V / 3.3 V) so UHS voltage switching works;
	 * downstream msdc_sd_power()/msdc_sd_power_switch() use VMC for IO.
	 */
	vqmmc-supply = <&mt6328_vmc_reg>;
};
```

The existing `cd-gpios = <&pio 5 GPIO_ACTIVE_LOW>` matches ROM
`cd-gpios = <&pio 5 0>` with `cd_level = 0` (low = inserted); no change.

Drive strengths from the ROM DTB (`mmc1@default` 3/3/3,
`mmc1@sdr104`/`sdr50`/`ddr50` cmd=2, dat=2, clk=3). The Nura pinctrl nodes
in `mt6735.dtsi` use one group per state (`pins-cmd-clk-dat`), so the exact
per-pin split requires changing that shared dtsi. If the split is done, use:

```dts
&mmc1_pins_default {
	pins-cmd-clk-dat {
		drive-strength = <3>;
	};
};
```

For the UHS state the exact ROM values need three separate pin groups
(cmd 2, dat 2, clk 3); this cannot be expressed by overriding the existing
single `pins-cmd-clk-dat` group and is left to the DTS integration owner.

## 7. Patch

`$M/patches/mtk-sd.patch` (`git diff --binary`, 315 lines, one file:
`drivers/mmc/host/mtk-sd.c`).
