# Display / panel parity report — `panel-samsung-s6d78a0-gh9607501a`

## 1. Assigned driver and files changed

| File | Status |
|---|---|
| `drivers/gpu/drm/panel/panel-samsung-s6d78a0-gh9607501a.c` | rewritten for ROM parity |
| `drivers/gpu/drm/mediatek/mtk_dsi.c` | **not changed** — no MT6735-specific fix required |
| `drivers/gpu/drm/mediatek/mtk_drm_drv.c` | **not changed** |
| DTS | not changed; fragment in §6 |

All parity is expressed through the panel's `mode_flags`, `drm_display_mode` and
DCS/GPIO handling, all of which `mtk_dsi.c` already consumes. The old driver's
two host-level mistakes (`MIPI_DSI_CLOCK_NON_CONTINUOUS`, `MIPI_DSI_MODE_NO_EOT_PACKET`)
are panel-side flags, so no DRM core change was needed.

Worktree: `$M/worktrees/display`
Patch: `$M/patches/display.patch` (`git diff --binary`, 1 file, +504/−153)

## 2. ROM evidence

### 2.1 The ROM does not use the S6D78A0 LCM C driver

* `$M/zip/symbols.txt` contains `gen_panel_probe`, `gen_panel_start`,
  `gen_panel_set_status`, `gen_panel_set_power`, `gen_panel_set_pin_state`,
  `gen_panel_parse_dt_extpin_ctrl_seq`, `gen_panel_parse_dt_dcs_cmds`,
  `gen_panel_read_reg`, `ddp_dsi_set_lcm_utils`, `ddp_dsi_config`.
* `$M/zip/vmlinux.strings` contains 48 hits for `gen_panel`.
* No `s6d78a0` string or symbol exists in the ROM kernel image.
* Downstream config
  `arch/arm/configs/grandpplte_00_defconfig:1379`
  `CONFIG_CUSTOM_KERNEL_LCM="mtk_gen_panel"`, `:1384-1385`
  `CONFIG_LCM_HEIGHT="960"`, `CONFIG_LCM_WIDTH="540"`.

Conclusion: the ROM panel driver is `mtk_gen_panel`, and the real init
sequence, timing, panel ID and brightness calibration come from the **board
DTB**, not from `s6d78a0_qhd_dsi_vdo.c`. The panel names/sequences are therefore
*not* findable in `vmlinux.strings`.

### 2.2 ROM DTB extracted from `boot.img`

`$M/zip/boot.img` (ANDROID! v0, page size 2048) was parsed; the MTK dt image is
at offset 9472000 (0x908800), size 387072. Extracted artifacts created for this
task:

* `$M/zip/boot_dt.img`
* `$M/zip/rom_dtb_0.dtb` … `rom_dtb_3.dtb` (4 board variants, all identical for
  the panel nodes)
* `$M/zip/rom_dtb_0.dts` (decompiled)

`/panel/gen-panel` (rom_dtb_0.dts lines 5066-5161) contains **two** panels,
selected at runtime by ID:

`gen-panel-s6d78a0x02-grandpplte-286108-video`:
`gen-panel-id = <0x286108>`, `manu = "BOE"`, name `lcd_s6d78a0x02_286108_mipi`,
refresh 60, 540×960, 64×110 mm, work-mode 1 (video), suspend-mode 1 (sleep cmd),
lanes 2, phy-feq 230000, hfp/hbp/hsync 35/35/28, vfp/vbp/vsync 8/6/2,
`gen-panel-rst-high-in-sleep`, init-cmds 331 bytes, disable-cmds
`[05 00 00 32 00 01 28 | 05 00 00 32 00 01 10]`, read-id `DA/DB/DC`.

`gen-panel-s6d78a0-grandpplte-53b810-video`: same geometry/timing/lanes, ID
`0x53b810`, name `lcd_s6d78a0_53b810_mipi`, same disable-cmds, its own 331-byte
init sequence.

Both were decoded and written into the driver **byte for byte** (verified
programmatically against the DTB properties).

Backlight node (rom_dtb_0.dts lines 5052-5064):

```
ktd3102-bl {
	compatible = "kinetic,backlight-ktd3102";
	bl-ctrl = <&pio 7 0>;			/* pin_ctrl  = GPIO7  */
	bl-pwm  = <&pio 69 0>;			/* pin_pwm   = GPIO69 */
	brt_table@0 {
		backlight-brt-range = <0 0>, <5 2>, <5 2>, <0x90 0x10>,
				      <0xff 0x1b>, <0x132 0x20>;
		gen-panel-bl-hbm-en = <0x132 0x20>;
	};
};
```

ROM pinmux states (rom_dtb_0.dts lines 2190-2275) confirm the panel control
pins: `lcm_rst` pin 146, `lcd_bias_enp` pin 61, `blic_ctl` pin 69,
`blic_en` pin 7 — all as plain GPIO. GPIO184 is **not** used for the LCD
anywhere in the ROM DTB.

### 2.3 Downstream source citations

* `drivers/misc/mediatek/lcm/mtk_gen_panel/mtk_gen_panel.c`
  * `:144-199` `mtk_panel_get_params()`: 540×960, 64×110 mm, 2 lanes,
    RGB888, `SYNC_PULSE_VDO_MODE`, v 2/6/8, h 28/35/35, `PLL_CLOCK = 230`,
    `ssc_disable = 1`, `clk_lp_per_line_enable = 0`, `cont_clock = 1`.
  * `:338-347` driver struct: `init`, `suspend`, `resume`,
    `.resume_power` commented out, `set_backlight`.
  * `:425-426` (and `:457-458`) the DCS command delay is applied **after**
    each command: `if (desc[i].delay) MDELAY(desc[i].delay);`.
  * `:217-234` `mtk_panel_resume_power()`: `set_gpio_lcd_enp(1)`, 60 ms,
    `set_gpio_lcd_rst(1)`, 30 ms.
  * `:250-268` `mtk_panel_suspend()`: backlight off + disable cmds via
    `gen_panel_start(OFF)`, then `set_gpio_lcd_rst(0)`, `set_gpio_lcd_enp(0)`.
* `arch/arm/boot/dts/mtk-dsi-panel_s6d78a0x02_grandpplte_286108_video.dtsi`
  `:5, :8, :11-12, :17-18, :25-30, :42-74 (init), :75-78 (disable)` and the
  same lines in `mtk-dsi-panel_s6d78a0_granpplte_53b810_video.dtsi`.
* `drivers/video/backlight/gen_panel/gen-panel.c`
  * `:575-597` `set_panel_id()`: read 0xDA, 0xDB, 0xDC and build
    `DA << 16 | DB << 8 | DC`.
  * `:1525-1563` `gen_panel_set_status()`: ON writes `PANEL_INIT_CMD` then
    `PANEL_ENABLE_CMD`; OFF disables backlight, then writes `PANEL_DISABLE_CMD`.
  * `:1565-1607` `gen_panel_start()`: backlight on for ON/REDUCED_ON,
    backlight off for OFF.
  * `:1827-1930` DCS DT parsing; header is `dtype u8, txmode u8, wait u16,
    dlen u16`, so `05 00 00 32 00 01 28` means "DCS 0x28, no params, 50 ms
    delay after".
* `drivers/video/backlight/ktd3102_bl.c`
  * `:39-79` `ktd3102_get_tune_level()`: interpolation between the 6 table
    points.
  * `:82-127` `ktd3102_set_bl_level()`: 5-bit single-wire dimming; code sent
    as `prev - level (+32 if < 0)` pulses, 2 µs low / 2 µs high; power off is
    EN low + CTRL low + 3 ms; power on is EN high, CTRL high, 100 µs.
  * `:130-152` `ktd3102_backlight_set_brightness()`; `:336` backlight
    `max_brightness` = HBM entry (306); `:341` `prev_tune_level = 0`.
* `include/linux/platform_data/gen-panel-bl.h:22,32-38`: `IS_HBM(level>=6)`,
  `BRT_VALUE_OFF/MIN/DIM/DEF/MAX/HBM`.
* `drivers/misc/mediatek/video/mt6735/dispsys/ddp_dsi.c`
  * `:1261-1262` `dis_eotp_en = false; hstx_cklp_en = false;`
    → EOT **enabled**, clock lane stays HS for this panel.
  * `:3722` `clk_lp_per_line_enable` only enables per-line clock LP when set;
    `mtk_gen_panel` sets it to 0.
  * `:3057-3060` `lcm_set_reset_pin()` writes `MMSYS_LCM_RST_B` (+0x150);
    `ddp_reg.h:1710` names it `DISP_REG_CONFIG_MMSYS_LCM_RST_B`
    (0 = reset asserted/low, 1 = released/high).
* `drivers/misc/mediatek/lcm/s6d78a0_qhd_dsi_vdo/s6d78a0_qhd_dsi_vdo.c`
  (not the ROM driver, used only as a cross-check)
  * `:89-166` init table, `:168-175` sleep-in (15 ms/150 ms — differs from the
    ROM DTB, DTB wins), `:297-325` `brt_table_ktd` (255→28; differs from the
    ROM DTB ktd3102 table, DTB wins), `:428-442` `lcm_init_power()`
    (reset low 5 ms, bias off 5 ms, bias on 5 ms, reset high 10 ms,
    reset low 10 ms, reset high 30 ms), `:444-457` `lcm_resume_power()`
    (identical), `:459-473` `lcm_suspend()`.

## 3. Parity matrix

| Behavior | ROM implementation (evidence) | Nura before | After | Status |
|---|---|---|---|---|
| Panel selection | `set_panel_id()` reads 0xDA/0xDB/0xDC; matches 0x286108 or 0x53b810 (gen-panel.c:575; ROM DTB panel nodes) | none, one hardcoded sequence | runtime ID read, two verbatim tables, 53b810 fallback with warning | done (DSI read hardware-unverified) |
| Init sequence | ROM DTB `gen-panel-init-cmds`, 331 bytes per variant | 53b810-like but `0xf2` = `19 04 08 08 08 14 14 00` (ROM `… 3f 23 00`) and `0xf6` = `93 23 15 07 07 0c` (ROM `63 23 15 00 00 0c`); 286108 variant unsupported | both tables byte-identical to the DTB, 20 commands each | done |
| Init delays | per-command `wait` applied after write (mtk_gen_panel.c:425) | 120 ms after 0x11, 20 ms after 0x29 | same values, now table-driven and applied after each command | done |
| Sleep-out / display-on | `05 00 00 78 00 01 11` (120 ms), `05 00 00 14 00 01 29` (20 ms) | `mipi_dsi_dcs_exit_sleep_mode` + 120 ms, `set_display_on` + 20 ms | same commands/delays via the init table | done |
| Display-off / sleep-in | `05 00 00 32 00 01 28`, `05 00 00 32 00 01 10` (50 ms each) | 0x28 + 40 ms, 0x10 + 100 ms | 0x28 + 50 ms, 0x10 + 50 ms | done |
| Reset / power-on | `lcm_init_power()`: reset low 5 ms, bias off/on 5+5 ms, release 10 ms, assert 10 ms, release 30 ms; `mtk_panel_resume_power()` final release 30 ms | reset pulse 10/5/30 ms starting from an unexpected level, no bias control | reset already asserted at probe; regulators on; 15 ms asserted (5 ms + 10 ms bias window), release 10 ms, assert 10 ms, release 30 ms | done, bias folding `[INFERENCE]` |
| Suspend | backlight off, 0x28/0x10 (50/50 ms), reset low, bias off (mtk_gen_panel.c:250) | 0x28/0x10 (40/100 ms), reset high/regulators off | backlight off, exact disable seq, reset asserted, regulators off | done |
| Resume | bias on, 60 ms, reset released, 30 ms, then init cmds | full reset pulse + init | full power-on sequence + init + ID read | done; extra reset pulses `[INFERENCE]` |
| Brightness mapping | ktd3102 table `(0,0)(5,2)(5,2)(144,16)(255,27)` + HBM `(306,32)`, linear interpolation (ktd3102_bl.c:39) | DCS `set_display_brightness` 0..255, DC default 255 | identical table/interpolation, range 0..255, default 144; HBM not exposed | done (HBM deferred) |
| Backlight enable | KTD3102 single-wire pulses on GPIO7, enable on GPIO69; off = both low + 3 ms; on = EN high, CTRL high, 100 µs then N pulses (ktd3102_bl.c:82) | none in panel driver (pwm-backlight in DTS) | optional `backlight-ctrl-gpios`/`backlight-en-gpios`, panel-owned backlight device, exact pulse protocol | done; DTS fragment required |
| Backlight on/off ordering | ROM enables backlight after init, disables before display-off | backlight core only | `.enable`/`.disable` + force-off in prepare | done |
| DSI lanes / format | 2 lanes, RGB888 (mtk_gen_panel.c:151-165) | 2 lanes, RGB888 | unchanged | done |
| Video mode | `SYNC_PULSE_VDO_MODE` | `VIDEO \| VIDEO_SYNC_PULSE` | unchanged | done |
| Clock lane | continuous (`cont_clock=1`, `clk_lp_per_line_enable=0`, `hstx_cklp_en=false`) | `MIPI_DSI_CLOCK_NON_CONTINUOUS` (per-line LP) | flag removed → continuous HS clock | done |
| EOT packets | `DIS_EOT = 0` (EOT enabled) | `MIPI_DSI_MODE_NO_EOT_PACKET` (EOT disabled) | flag removed | done |
| Command LP/HS | host default HS (`force_update=1`) | toggled `MIPI_DSI_MODE_LPM` around init/brightness (no effect in mtk_dsi) | flag juggling removed | done |
| Timing | hfp 35, hsync 28, hbp 35; vfp 8, vsync 2, vbp 6; 60 Hz | h 10/14/10, v 8/2/6, 33,613 kHz | h 35/28/35, v 8/2/6, 638×976×60 → 37,361 kHz | done |
| Physical size | 64 × 110 mm | 62 × 110 mm | 64 × 110 mm | done |
| DSI bit rate | `PLL_CLOCK = 230` MTK step, DTB `phy-feq 230000` (comment: ×2 = 460 Mbps) | derived 33,613×24/2 = 403 Mbps | 37,361×24/2 = 448.3 Mbps (host PLL rounds; ROM step yields ≈61.4 Hz) | partial / host-side, see §5 |
| ESD check | `customization_esd_check_enable = 1`, `esd_check_enable = 0` (disabled) | none | none | done (no action needed) |
| TE / DBI params | video mode; TE settings unused | none | none | done (N/A) |
| FBDEV stack, MDNIE/CABC, HBM sysfs | present downstream | absent | not ported | deferred (out of scope) |

Other differences found and fixed: the old driver's `0xef` command already
matched the 53b810 ROM data; `0xf7`, `0xfa`, `0xfb`, `0x36` and the trailing
`0xf0/0xf1/0xfc` also matched. Only `0xf2` and `0xf6` were wrong, and the whole
286108 variant was missing.

## 4. Compile test

```
export PATH="$M/bin:$PATH"
cd $M/worktrees/display
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy \
  OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf \
  STRIP=aarch64-linux-gnu-strip \
  -j2 W=1 drivers/gpu/drm/panel/panel-samsung-s6d78a0-gh9607501a.o
```

Result: `CC [M] drivers/gpu/drm/panel/panel-samsung-s6d78a0-gh9607501a.o`,
exit 0, no warnings with `W=1`. No other object changed.

`scripts/checkpatch.pl --no-tree --strict -f` on the driver:
`0 errors, 0 warnings, 0 checks`.

Additional verification: both generated init tables and the disable table were
compared programmatically against the ROM DTB properties (`gen-panel-init-cmds`,
`gen-panel-disable-cmds`) — exact match (20 commands each, delays included).

## 5. Inferred / unverified on hardware

* `[INFERENCE]` Panel ID read (0xDA/0xDB/0xDC) over DSI in `prepare()`. The
  transfer is the same one `gen_panel_set_id()` performs and `mtk_dsi.c`
  supports reads, but no hardware was available to confirm read timing after
  reset. If it fails the driver warns and uses the 53b810 sequence.
* `[INFERENCE]` The bias rail timing: the LCM toggles `LCD_ENP` inside the reset
  pulse; with the mainline regulator model the rail is enabled once before the
  pulse and its 10 ms off/on window is folded into the 15 ms reset-assert time.
* `[INFERENCE]` Extra reset pulses on resume. The ROM resume only releases reset
  (bias on → 60 ms → reset high → 30 ms); the driver runs the full
  `lcm_init_power()` pulse train so a cold boot and a resume behave identically.
* `[INFERENCE]` GPIO61 is the panel bias/3.0 V enable (`lcd_bias_enp` in the ROM
  DTB). The Nura DTS currently models `vdd_lcd_3p0` on GPIO184, which the ROM
  never uses for the display; see §6.
* KTD3102 pulse timing (2 µs/2 µs, 100 µs power-on, 3 ms off) is copied from
  `ktd3102_bl.c`; it is a single-wire protocol, not PWM, and is untested.
* HBM (brightness 306 → code 32) is not exposed: the ROM only reaches it
  through Samsung's `auto_brightness` sysfs, which is not ported.
* DSI PLL: the driver requests 37,361 kHz pixel clock → 448.3 Mbps. The ROM uses
  the `PLL_CLOCK = 230` MTK step (460 Mbps, ≈61.4 Hz at these totals). The
  MT6735 MIPI PLL rounds the requested rate; exact pxclk/refresh is
  hardware-unverified.
* `ssc_disable = 1` and `intermediat_buffer_num`, `packet_size`, `DSI_WMEM_CONTI`,
  `PS` are host/IP-block settings with no panel-side mainline control; not
  changed.
* `MIPI_DSI_MODE_NO_EOT_PACKET` was removed based on `dsi_eotp_en = false`. If
  the physical panel needs EOT suppression this would show as CRC/ECC errors;
  hardware-unverified.

## 6. DTS fragment (not applied)

The board DTS is shared and was not modified. To activate the ROM backlight and
correct the panel power rail, apply the following to
`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`:

```dts
/* 1. Remove the pwm-backlight node: GPIO69 is the KTD3102 EN line, not a
 *    PWM input, and GPIO7 is the single-wire CTRL line (ROM DTB pinctrl
 *    "blic_ctl_enp*" = pin 69, "blic_en_enp*" = pin 7).
 */
/ {
	/delete-node/ backlight;

	vdd_lcd_3p0: regulator-vdd-lcd-3p0 {
		compatible = "regulator-fixed";
		regulator-name = "vdd_lcd_3p0";
		regulator-min-microvolt = <3000000>;
		regulator-max-microvolt = <3000000>;
		startup-delay-us = <80>;
		gpio = <&pio 61 0>;	/* was 184; ROM "lcd_bias_enp" */
		enable-active-high;
	};
};

&pio {
	panel_default: panel-default-pins {
		pins-reset {
			pinmux = <PINMUX_GPIO146__FUNC_GPIO146>;
		};
		pins-bl-ctrl {
			pinmux = <PINMUX_GPIO7__FUNC_GPIO7>;
		};
		pins-bl-en {
			pinmux = <PINMUX_GPIO69__FUNC_GPIO69>;
		};
	};
};

&pio {
	/delete-node/ backlight_default;
};

&dsi {
	panel: panel@0 {
		/* was: backlight = <&backlight>; */
		backlight-ctrl-gpios = <&pio 7 GPIO_ACTIVE_HIGH>;
		backlight-en-gpios = <&pio 69 GPIO_ACTIVE_HIGH>;
	};
};
```

`/delete-node/` on `/backlight` and `backlight_default` is shown for clarity; in
a real patch the nodes can simply be removed. If the fixed regulator is kept on
GPIO184 (not recommended by ROM evidence), the panel will have no bias/enable
rail and the panel will not come up; the driver itself does not care which pin
the regulator uses.

Notes:
* If the panel node still has a `backlight = <...>` phandle, the driver uses
  that backlight and skips its own KTD3102 device (`drm_panel_of_backlight()`
  takes precedence). The fragment above removes the phandle so the ROM
  backlight path is used.
* GPIOs are requested with `GPIOD_OUT_*` by the driver, so the pinctrl fragment
  only needs the `pinmux` entries; no `output-low` needed.
* `GPIO_ACTIVE_HIGH` is already available in that DTS.
* No change is needed to `&dsi` other than the panel child; `mtk_dsi.c` reads
  lanes/format/mode flags from the panel device.

## 7. Patch

`$M/patches/display.patch`
