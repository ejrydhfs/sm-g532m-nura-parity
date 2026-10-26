# Zinitix BT541 parity report

## 1. Assigned driver and files changed

- Driver: `drivers/input/touchscreen/zinitix.c` (Zinitix BT4xx/BT5xx mainline
  driver, ported to behave like the SM-G532M BT541 driver).
- Worktree: `$M/worktrees/zinitix`
- Files changed:
  - `drivers/input/touchscreen/zinitix.c` (971 insertions, 128 deletions total
    in the patch)
  - `Documentation/devicetree/bindings/input/touchscreen/zinitix,bt400.yaml`
    (`zinitix,mode` now documents and allows mode 0)
- No Kconfig/Makefile, shared DTS, or generated file was modified. No
  Android sysfs, misc-device, or ioctl interface was added (see matrix row 17).
- Patch: `$M/patches/zinitix.patch`

Reference sources:

- Downstream Samsung/MTK driver:
  `$M/src/downstream-grandpplte/drivers/input/touchscreen/bt541/bt541_ts.c`
  (cited as `bt541_ts.c:<line>` below) and `bt541_ts.h`.
- Downstream board DTS:
  `$M/src/downstream-grandpplte/arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_03.dts`
  (`bt541_ts@20` node lines 1120-1143, `tsp_int_active`/`tsp_int_suspend`
  lines 1147-1159).
- ROM kernel: `$M/zip/vmlinux.elf` (32-bit ARM, recovered kallsyms),
  `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`.

## 2. ROM evidence

### Symbols present in the ROM (the downstream driver, built into the ROM)

Addresses verified with `aarch64-linux-gnu-nm -n $M/zip/vmlinux.elf`:

```
c06f0da0 t ts_read_coord
c06f2b34 t bt541_power_control          (power-on sequence + IUM timeout)
c06f3230 t ts_upgrade_firmware
c06f42f8 T bt541_ts_shutdown
c06f43e8 t bt541_touch_work             (IRQ handler)
c06f56f0 T tsp_nvm_ium_lock
c06f57c0 T tsp_nvm_ium_unlock
c06f71c0 T bt541_fw_update_from_kernel
c06f73dc t init_touch
c06fa490 T bt541_pinctrl_configure
c06fa648 t bt541_ts_probe
c06fba10 t bt541_ts_close
c06fbc1c t bt541_ts_open
c0ad1134 t _fw_tsp_zinitix_bt541_GP_fw_bin
c0ad9134 t _fw_end                      (blob size 0x8000 = 32 KiB)
c0d106d0 t _fw_tsp_zinitix_bt541_GP_fw_name
```

`tsp_zinitix/bt541_GP.fw` is present in `$M/zip/vmlinux.strings`, matching the
firmware name the driver requests (`bt541_ts.c:6443`).

### Register-level confirmations from disassembly (objdump windows on vmlinux.elf)

- `ts_read_coord()` (`0xc06f0da0`) selects register `0x80` (`mov r3, #128` at
  `0xc06f0e74`), reads **16 bytes** (`mov r3, #16` at `0xc06f0e80`) and checks
  the payload against register `0x016c` (`mov r3, #364` at `0xc06f0ed4`).
  16 bytes = 4-byte header + 2 x 6-byte coordinates, i.e. touch mode 0 with
  two fingers — exactly `struct point_info` for `TOUCH_POINT_MODE 0`
  (`bt541_ts.c:474-493`, `bt541_ts.c:79-85`).
- `bt541_touch_work()` (`0xc06f43e8`) reads the icon status register `0x00aa`
  (`mov ip, #170` at `0xc06f4a88`, 2-byte read at `0xc06f4a8c`), confirming
  `BT541_ICON_STATUS_REG = 0x00aa` (`bt541_ts.c:260`).
- `bt541_power_control()` (`0xc06f2b34`) writes value `64` (`mov r3, #64` at
  `0xc06f2f40`) to register `0x002e` (`mov r3, #46` at `0xc06f2f50`), confirming
  the IUM timeout register/value (`bt541_ts.c:118`, `bt541_ts.c:1445`).
- Embedded firmware: `_fw_tsp_zinitix_bt541_GP_fw_bin` at `0xc0ad1134` to
  `_fw_end` at `0xc0ad9134` = 32 KiB, matching `cap->ic_fw_size = 32*1024`
  (`bt541_ts.c:2523`).

### Downstream configuration constants (source-verified)

| Item | Value | Downstream reference |
|---|---|---|
| Touch point mode (SUPPORTED_PALM_TOUCH undefined) | 0 | `bt541_ts.c:79-83` |
| Finger count | 2 | `bt541_ts.c:85` |
| Touch keys, mapping | 2 keys, `{KEY_RECENT, KEY_BACK}` | `bt541_ts.c:49`, `77`, `93-96`, `659-663` |
| Icon status register | 0x00aa; down bits 0-7, up bits 8-15 | `bt541_ts.c:260`, `333-347`, key loops `3126-3165` |
| Power timings | off 50 ms, on 20 ms, firmware 60 ms | `bt541_ts.c:111-113` |
| IUM timeout | 0x002e = 64 (1.7 s) | `bt541_ts.c:118`, `1445`, `2756` |
| Power-on sequence | 0xc000=1, read 0xcc00, 0xc004, 0xc002=1, 0xc001=1, 60 ms, 0x002e=64, 3 retries | `bt541_ts.c:1406-1458` |
| Flash sector size | 8 bytes | `bt541_ts.c:87` |
| Calibration/IUM section | `DEF_IUM_ADDR = 64*2 = 0x80`, page size 64 | `bt541_ts.c:1529`, board DTS `bt541,page_size = <64>` line 1137 |
| Flash sequence | 0xc000, 0xcc00, size from fw[0x61..0x67], 0xc201=0x00be, 0xc004, 0xc002, 0xc003, 0xc104, INIT/WRITE/READ_FLASH, verify with calibration page excluded, 0x55aa check | `bt541_ts.c:2021-2258` (header size 2086-2088, clk 2092, IUM window 2134-2160, verify skip 2216-2218, checksum 2229-2238) |
| Firmware version rules | fw[0x34], fw[0x38], fw[0x3c]; update if cur > 0xff or cur < new (version, then minor, then regdata) | `bt541_ts.c:1966-2019` |
| Firmware name / size | `tsp_zinitix/bt541_GP.fw`, 32 KiB | `bt541_ts.c:6443`, `2523` |
| Init checksum | register 0x012c must read 0x55aa | `bt541_ts.c:2442-2452`, `2847-2853` |
| I2C payload checksum | 0x011e bit 15 enables; word count in 0x016a, sum (s16 words) vs 0x016c | `bt541_ts.c:1155-1178`, `1258-1285` |
| Resolution | `bt541,x_resolution`/`y_resolution` = 539/959 (DT) written to 0x00c0/0x00c1 | `bt541_ts.c:6418-6424`, `2707-2718` |
| Orientation | `bt541,orientation` bit0 V-flip, bit1 H-flip, bit2 XY-swap; board value 0 | `bt541_ts.c:501-503`, `3192-3210`, board DTS line 1136 |
| Suspend/resume | close: SLEEP_CMD then power off; open: power-on sequence + `mini_init_touch()` (incl. soft calibration) | `bt541_ts.c:3527-3580` (SLEEP at 3569), `3463-3522`, `2837-2950` (CALIBRATE_CMD at 2900-2902) |
| Shutdown | disable IRQ, power off | `bt541_ts.c:6880-6895` |
| Regulator | `regulator_get(dev, "vgp1")`, `regulator_set_voltage(3.0 V)` | `bt541_ts.c:6467-6483` |
| Gestures | `cap->gesture_support = 0`, no gesture code | `bt541_ts.c:2732` |

## 3. Parity matrix

| # | Behavior | ROM / downstream implementation | Nura before | After | Status |
|---|---|---|---|---|---|
| 1 | Firmware update path | `request_firmware("tsp_zinitix/bt541_GP.fw")` at probe (`bt541_ts.c:2260-2313`), version rules (`1966-2019`), 8-byte flash writes with write-protect window at 0x80 (`2021-2258`), read-back verify, 0x55aa check, 3 retries | absent | `zinitix_update_firmware()` / `zinitix_flash_firmware()` implementing the same register sequence and checks, gated on `zinitix,bt541`; optional blob, on-chip image kept when absent; runs at first input open instead of probe (the driver powers the chip from input open/close) | done (compile-tested; flashing hardware-unverified) |
| 2 | Firmware integrity check | 0x012c must read 0x55aa (`bt541_ts.c:2442-2452`); bad checksum fails init and triggers forced update (`2795-2835`) | absent | `zinitix_init_touch()` checks 0x012c (5 retries), forces an update on bad checksum, fails init if no blob is available, mirroring downstream fail_init | done (hardware-unverified) |
| 3 | Touch keys (recent/back) | icon reg 0x00aa, down bits 0..7 then up bits 8..15, `KEY_RECENT`/`KEY_BACK` (`bt541_ts.c:3126-3165`, `659-663`), release all on power down (`2989-3042`) | down bits only (`icon_events & BIT(i)`), no up-event handling, no release on suspend | `zinitix_report_keys()` tracks state and processes down bits then up bits in the same frame (fast tap can raise both); `zinitix_release_all()` on stop; `linux,keycodes` supplies the mapping | done (mapping needs the DTS fragment) |
| 4 | Gestures | not compiled (`SUPPORTED_PALM_TOUCH` undefined, `cap->gesture_support = 0` at `bt541_ts.c:2732`); no gesture code in downstream | absent | not implemented; the ROM exposes no gesture feature | deferred (not present in ROM) |
| 5 | Touch report layout (point mode / fingers) | mode 0, 2 fingers, 6-byte coords, 16-byte point report (ROM disassembly of `ts_read_coord`, `bt541_ts.c:79-85`, `474-493`) | mode 2, 5 fingers, 8-byte coords (mainline default) | BT541 runs mode 0 with 2 fingers and a 6-byte coordinate stride, exactly the ROM configuration; other chips keep mode 2 / 5 fingers; `zinitix,mode = <0\|2>` overrides | done (ROM-exact; hardware-unverified) |
| 6 | Touch report fields | `w = max(width, 1)`, `ABS_MT_TOUCH_MAJOR = w`, `ABS_MT_WIDTH_MAJOR = w`, positions through orientation transform (`bt541_ts.c:3220-3255`) | `ABS_MT_TOUCH_MAJOR = width`, no width clamp | same as downstream: width clamped to 1, both TOUCH_MAJOR and WIDTH_MAJOR reported; `ABS_MT_PRESSURE` not ported — in the ROM build the axis is only registered in `CONFIG_SEC_FACTORY` (`bt541_ts.c:6700-6702`) so the input core discards those events anyway | done |
| 7 | Power sequencing | vgp1 regulator 3.0 V (`bt541_ts.c:6467-6483`), 20/50/60 ms timings, 0xc000/0xcc00/0xc004/0xc002/0xc001 with IUM timeout and 3 retries (`1406-1458`) | regulator enable + 15 ms, no chip-code read, no retries, no IUM timeout | 3.0 V set for BT541, downstream timings, chip-code read, IUM timeout (0x002e = 64), 3 sequence retries, SLEEP_CMD before power off (`bt541_ts.c:3569`) | done |
| 8 | Reset GPIO | none: no reset line in downstream driver or board DTS; the board resets by power cycling | none | optional `reset-gpios` (binding-conformant, active low) deasserted after regulator-on, asserted on power-off; no-op without the property | done (optional; timing `[INFERENCE]`) |
| 9 | pinctrl | `tsp_int_active`/`tsp_int_suspend`, GPIO62, bias-disable (identical states, board DTS 1147-1159); `bt541_pinctrl_configure()` switches them | node has no pinctrl | no driver change needed (single default state is enough); DTS fragment adds `tsp_int_default` for GPIO62 | partial (DTS fragment) |
| 10 | Suspend/resume | close: disable IRQ, clear reports, SLEEP_CMD, power off (`bt541_ts.c:3527-3580`); open: power-on sequence, `mini_init_touch()` incl. soft calibration (`2837-2950`), enable IRQ | stop only disabled regulators; no SLEEP, no contact release | `zinitix_stop()` disables IRQ, releases contacts/keys, SLEEP then power off; `zinitix_start()` powers on, sequences, runs init incl. the downstream soft calibration (`BT541_CALIBRATE_CMD`, `bt541_ts.c:2900-2902`) and the firmware fallback | done |
| 11 | Shutdown | disable IRQ, power off (`bt541_ts.c:6880-6895`) | absent | `.shutdown` callback added | done |
| 12 | Resolution registers | writes `bt541,x_resolution`/`y_resolution` (539/959) to 0x00c0/0x00c1; abs range 0..539/0..959 (`bt541_ts.c:2707-2718`, `6679-6696`) | wrote `prop.max_x` = 538/958 because the shared DTS uses `touchscreen-size-x = <539>` (size = max + 1 per the binding) | unchanged register logic; DTS fragment sets `touchscreen-size-x/y = <540>/<960>` so `prop.max_x/y` = 539/959 exactly as downstream | done via DTS fragment |
| 13 | Orientation | `bt541,orientation` V-flip/H-flip/XY-swap around MaxX/MaxY (`bt541_ts.c:3192-3210`) | generic `touchscreen-inverted-x/y`, `touchscreen-swapped-x-y` via `touchscreen_report_pos()` | unchanged: the generic properties express the same transforms with standard APIs; board orientation is 0 (identity) | done |
| 14 | I2C payload checksum | enabled when 0x011e bit 15 set; WCNT = report words (8 for the 16-byte report), s16 word sum vs 0x016c (`bt541_ts.c:1155-1178`, `1258-1285`) | absent | implemented for BT541; WCNT = `event_size / 2` (8 words in mode 0, exactly the ROM value); mismatch retries the read | done (hardware-unverified) |
| 15 | Palm events | compiled out (`SUPPORTED_PALM_TOUCH` undefined) | absent | not enabled; matches ROM | done (not applicable) |
| 16 | Optional setting 0x0116 | written by `bt541_set_optional_mode()`; bit 3 is the `dead_zone` sec_cmd knob (`bt541_ts.c:1042-1052`, `5767-5788`); value stays 0 on this product | absent | not ported (Android service knob, no hardware behavior on this board) | deferred |
| 17 | PAT/NVM calibration, factory sysfs, misc ioctl, raw data | downstream Android production/factory features (`ts_hw_calibration`, sec_cmd tables, `ts_misc_fops_ioctl`, `ts_get_raw_data`) | absent | not ported per assignment; the delivered patch adds **no** sysfs/misc/ioctl nodes | deferred |
| 18 | ESD timer | periodical interrupt + reset work (`bt541_ts.c:1355-1402`, `2837-2950`) every `CHECK_ESD_TIMER` seconds | absent | not ported; it is a watchdog around the same power-cycle + mini-init sequence and not required for input-path parity | deferred (documented follow-up) |

Notes on two intentional, documented differences:

- **Firmware update timing**: downstream updates from `bt541_ts_probe()`
  (`bt541_ts.c:6649`); this driver powers the controller from input
  open/close, so the update runs at the first open. The update decision,
  register sequence and checks are identical.
- **Soft calibration at first init**: downstream sends `BT541_CALIBRATE_CMD`
  only from `mini_init_touch()` (resume/recovery), not from the probe-time
  `init_touch()`. This driver runs one init path, so it calibrates on every
  power-up — one extra baseline calibration at boot compared to downstream.

## 4. Compile test

```
cd $M/worktrees/zinitix
export PATH="$M/bin:$PATH"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig                       # exit 0
make ARCH=arm64 LLVM=1 W=1 \
  AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy \
  OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf \
  STRIP=aarch64-linux-gnu-strip \
  -j2 drivers/input/touchscreen/zinitix.o
```

Result: `CC [M] drivers/input/touchscreen/zinitix.o`, exit 0, no warnings
(with `W=1`).

`scripts/checkpatch.pl -f drivers/input/touchscreen/zinitix.c`:
`0 errors, 0 warnings, 1596 lines checked`. The binding YAML also passes
checkpatch (`0 errors, 0 warnings, 129 lines checked`). `git diff --check` is
clean. No full kernel build was run, per the program rules.

## 5. Inferred / hardware-unverified

- **Flash sequence is hardware-unverified**: no phone is attached and no
  firmware blob is shipped. The patch implements the downstream register
  sequence (0xc201 = 0x00be clock, 0xc003 VPP, 0xc104 write protect, flash
  init/write/read, 8-byte sectors, 100 us per sector, 30 ms per page, the
  0x80 calibration window, read-back verify, 0x55aa check). Only install the
  exact 32 KiB `tsp_zinitix/bt541_GP.fw`; there is no blob in the patch.
- **Mode 0 / 2 fingers / 6-byte coordinates**: proven for the ROM build by
  the `ts_read_coord` disassembly (16-byte read) and the downstream structs,
  but the panel response is unverified on hardware. If testing shows the
  firmware serves mode 2 instead, set `zinitix,mode = <2>` in the DTS node —
  that path is implemented and keeps the mainline 5-finger behaviour only for
  non-BT541 chips.
- **Reset GPIO timing is `[INFERENCE]`**: the board has no reset line in the
  downstream DTS or driver, so the optional sequence (assert while powered
  down, release after regulator-on + 20 ms) follows the binding and the
  downstream power-on delay; it is a no-op without `reset-gpios`.
- **I2C checksum word count** is 8 words in mode 0, matching the ROM
  (`ts_read_coord` sums 16 bytes against 0x016c). The mode 2 value (22 words)
  is derived from the report size, not observed on hardware.
- **Regulator wiring**: the downstream board DTS has the supply commented out
  (`//avdd-supply = <&mt_pmic_vgp1_ldo_reg>;`, line 1126), so downstream power
  control was in fact a no-op on this board and the rail was always on. The
  fragment wires `mt6328_vgp1_reg` (3.0 V) properly so the driver can
  power-cycle the panel as `bt541_power()` intends; whether VGP1 feeds only
  the touch panel needs a board check (no other consumer exists in the DTS).
- **Key mapping** is board data: the driver reports `linux,keycodes` in icon
  order. The ROM maps icon 0 to Samsung `KEY_RECENT` (254) and icon 1 to
  `KEY_BACK`. Mainline has no 254; the fragment uses `KEY_APPSELECT`, the
  mainline equivalent used by other Samsung boards.
- The 0x012c checksum/forced-update behaviour, SLEEP_CMD ordering, and
  suspend/resume behaviour need a device to confirm.
- `touchscreen-size-x/y` defaults of 539/959 when the properties are missing
  mirror the downstream hard defaults (`bt541_ts.c:6420-6424`); conformant
  DTS must still supply them (binding `required`).

Firmware blob dependency: `tsp_zinitix/bt541_GP.fw` must be installed under
`/lib/firmware` (or built in via `CONFIG_EXTRA_FIRMWARE`) for updates. Without
it the driver logs that the firmware is unavailable and keeps the on-chip
image; a bad on-chip checksum without a blob fails probe-time init exactly as
downstream `fail_init` does.

## 6. DTS fragment (DTS-FRAGMENT)

The shared DTS already contains a `touchscreen@20` node
(`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts:226-233`) and an
always-on `mt6328_vgp1_reg` block. The fragment below overrides those nodes;
it is intentionally **not** applied to the shared files. Requires
`#include <dt-bindings/input/input.h>` for the `KEY_*` macros (already
included by the file for `mt6738_keys`).

```
&i2c3 {
	touchscreen@20 {
		compatible = "zinitix,bt541";
		reg = <0x20>;
		interrupt-parent = <&pio>;
		interrupts = <62 IRQ_TYPE_EDGE_FALLING>;

		pinctrl-names = "default";
		pinctrl-0 = <&tsp_int_default>;

		vcca-supply = <&mt6328_vgp1_reg>;
		vdd-supply = <&mt6328_vgp1_reg>;

		linux,keycodes = <KEY_APPSELECT KEY_BACK>;

		touchscreen-size-x = <540>;
		touchscreen-size-y = <960>;
	};
};

&pio {
	tsp_int_default: tsp-int-default-pins {
		pins-cmd-dat {
			pinmux = <PINMUX_GPIO62__FUNC_GPIO62>;
			bias-disable;
		};
	};
};

&mt6328_vgp1_reg {
	/delete-property/ regulator-always-on;
};
```

Rationale:

- `vcca-supply`/`vdd-supply` both point at the PMIC VGP1 rail: the downstream
  driver powers the panel from the single `vgp1` regulator at 3.0 V
  (`bt541_ts.c:6467-6483`) and the driver bulk-gets both supply names
  (binding `zinitix,bt400.yaml`). Removing `regulator-always-on` lets the
  driver power-cycle the panel (VGP1 has no other consumer in the DTS).
- `touchscreen-size-x/y` must be the pixel count (540x960): the binding treats
  the size as max + 1, so the driver writes `prop.max_x/y` = 539/959 to the
  controller's resolution registers — the exact values downstream writes from
  `bt541,x_resolution = <539>` / `bt541,y_resolution = <959>`.
- `linux,keycodes` enables the two capacitive keys in ROM order (recent then
  back). `KEY_APPSELECT` is the mainline stand-in for Samsung `KEY_RECENT`.
- `tsp_int_default` reproduces the downstream `tsp_int_active`/
  `tsp_int_suspend` configuration (GPIO62, bias-disable; both downstream
  states are identical, so one default state suffices and no driver pinctrl
  code is needed).
- No `zinitix,mode` property: the driver defaults BT541 to mode 0 (the ROM
  configuration). No `reset-gpios`: neither the downstream DTS nor the driver
  uses a reset line on this board.

## 7. Patch

`$M/patches/zinitix.patch` (`git diff --binary` from `$M/worktrees/zinitix`,
regenerated from the final tree).
