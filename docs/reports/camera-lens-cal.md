# camera-lens-cal: DW9807AF VCM (Samsung module) + IMX219 module EEPROM

Worktree: `$M/worktrees/camera-lens-cal` (nura-linux 7.0.10, HEAD 046883c0c)

## 1. Driver and files changed

| File | Change |
|---|---|
| `drivers/media/i2c/dw9807-vcm.c` | Added `samsung,dw9807-vcm` compatible (first entry, before the Dongwoon fallbacks) with OF match data `{ .ring_mode = true }`; `dw9807_probe()` reads it via `of_device_get_match_data()` into `struct dw9807_device.ring_mode`; `dw9807_vcm_resume()` writes `0x02 = 0x02` (ring control mode) when `ring_mode`, otherwise `0x02 = 0x00` exactly as upstream did, so `dongwoon,dw9807-vcm` / `dongwoon,dw9807` (and any non-OF match, where the match data is NULL) are unchanged. Suspend still writes `0x02 = 0x01` (`DW9807_CTL_PD`). Documented that the downstream 10-bit DAC programming already matches; added a `Port from Samsung/MTK downstream drivers/misc/mediatek/lens/common/dw9807af/DW9807AF.c` header stating that the downstream eflash read/write helpers are superseded by the EEPROM driver. No supplies, no GPIOs, no `V4L2_CID_FOCUS_AUTO` added (see §3). |
| `drivers/media/i2c/imx219-eeprom.c` | **New.** Read-only NVMEM provider over the module EEPROM i2c client (`reg_read` doing 16-bit-address byte reads under a mutex), plus sysfs attributes `mfr_info`, `module_id`, `af_macro_cal`, `af_pan_cal`, `afcal`. Lazy reads only (no i2c traffic at probe, because the EEPROM shares the module rails with the sensor). Blank/erased data is rejected with `-ENODATA`. |
| `drivers/media/i2c/Kconfig` | New `VIDEO_IMX219_EEPROM` tristate inside the `VIDEO_CAMERA_SENSOR` block (after `VIDEO_IMX219`), `depends on I2C && NVMEM`, module name `imx219-eeprom`. |
| `drivers/media/i2c/Makefile` | `obj-$(CONFIG_VIDEO_IMX219_EEPROM) += imx219-eeprom.o` (alphabetically after `imx219.o`). |

Untouched: `imx219.c`, `s5k5e3yx.c`, all DTS/DTSI, `media_device` ownership (seninf).

## 2. ROM/downstream evidence (symbols, strings, file:line)

Downstream source root: `$M/src/downstream-grandpplte/drivers/misc/mediatek`.

### VCM (DW9807AF)
| Fact | Evidence |
|---|---|
| i2c write address `0x18` → 7-bit `0x0c` | `lens/common/dw9807af/DW9807AF.c:29` (`AF_I2C_SLAVE_ADDR 0x18`), `:142-143`/`:154-155` (`client->addr = AF_I2C_SLAVE_ADDR >> 1`); ROM DT `camera_main_af@18` (`zip/rom_dtb_0.dts:3288`), board `mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:770` |
| 10-bit DAC, max 1023 | `DW9807AF.c:46` (`g_u4AF_MACRO = 1023`), range check `:196-205` |
| MSB reg `0x03` bits 1:0, LSB reg `0x04` | `DW9807AF.c:138-139` (`{0x03,(data>>8)&0x3}`, `{0x04,data&0xFF}`), read-back `:97-132` (`0x03` / `0x04` at `:100-102`, value `((msb&0x3)<<8)|lsb` at `:129`) |
| "ring control mode" `0x02 = 0x02` at lens open | `DW9807AF.c:190-194` (`initdrv()`), called from `moveAF()` `:209` |
| Power-down is done by the rails, not the chip | `DW9807AF.c` contains no `0x02` write other than `initdrv()`; rails `vcamaf` are owned by `kd_camera_hw1` (`rom_dtb_0.dts:3719-3730`, board `..._gpio_04.dtsi:681-710`, `&kd_camera_hw1`, `vcamaf-supply` at `:686`) |
| Gradual ramp on release | `DW9807AF.c:303-330` (steps of 100 above 300, then 10 down to 0, `usleep_range(1000,2000)`) |
| No regulator / GPIO handling in the VCM driver | whole `DW9807AF.c` (only i2c ops + `SetI2Cclient`); the AF DT node has no `pinctrl-*`, no `*-supply`; `cam0_pnd*`/`cam0_rst*` pinctrl belongs to `kd_camera_hw1` |
| Calibrated AF range is applied by userspace | `AFIOC_T_SETINFPOS`/`AFIOC_T_SETMACROPOS` → `setAFInf()`/`setAFMacro()` `DW9807AF.c:251-266`, `:281-287` |
| Eflash helpers use a *different* slave on the same adapter | `DW9807AF.c:50-95` (`s4DW9807AF_Eflash_ReadReg/_WriteReg`, page/addr packed into 2 bytes, `addr = slave_id >> 1`) |
| Downstream position read-back is broken | `DW9807AF.c:123` compares a 1-byte `i2c_master_recv()` result against `2`, so `s4DW9807AF_ReadReg()` always returns `-1` and `moveAF()` always seeds `g_u4CurrPosition = 0` (`:209-222`) |

Gating: `ring_mode` is `true` only for `samsung,dw9807-vcm`; the `dongwoon,dw9807-vcm` and `dongwoon,dw9807` entries carry no match `.data`, so `dw9807_probe()` leaves `ring_mode = false` (same for any non-OF match, where `of_device_get_match_data()` returns NULL) and `dw9807_vcm_resume()` writes `0x02 = 0x00` exactly as upstream did — the generic path is byte-for-byte unchanged.

ROM strings/symbols: `DW9807AF_[...]` prefixes and the lens ioctl names are present; `$M/zip/symbols.txt` has `CAMERA_HW_probe`, `CAM_CAL_probe`, `read_imx219_eeprom*`, `get_imx219_*`, `imx219_update_sysfs_information`.

### Module EEPROM (cam_cal)
| Fact | Evidence |
|---|---|
| Linked implementation is the **imgsensor** variant | `zip/vmlinux.strings:36787` = `drivers/misc/mediatek/imgsensor/src/mt6735/imx219_mipi_raw/imx219_eeprom.c`; `llvm-nm` shows one `read_imx219_eeprom` @ `c04f4598` (2 address bytes) and two `read_cmos_sensor` (`c04f5584`, `c04f7a60`) |
| 16-bit register address, big endian | `imgsensor/.../imx219_eeprom.c:142-150` (`pu_send_cmd[2] = {addr>>8, addr&0xFF}`); ROM `read_imx219_eeprom` @ `c04f4598` builds the same 2-byte buffer and calls `iReadRegI2C(..., a_sizeSendData=2, a_sizeRecvData=1)` |
| 7-bit slave `0x58` | `imx219_eeprom.c:364` (`u8 slave_id = 0xB0`) with `kd_sensorlist.c:454-462` (`g_pstI2Cclient->addr = i2cId >> 1`) → `0xB0 >> 1 = 0x58`. ROM `imx219_update_sysfs_information` @ `c04f4c60` passes `mov r0, #176` (0xb0) to every read |
| MFR info: offset `0x20`, 11 bytes | `imx219_eeprom.c:365,371`; ROM `imx219_update_sysfs_information` (`r1 = #32`, `r3 = #11`) |
| Module ID: offset `0x50`, 10 bytes (CONFIG_GET_REAR_MODULE_ID=y) | `imx219_eeprom.c:369,387`; ROM (`r1 = #80`, `r3 = #10`); `grandpplte_00_defconfig:2957` |
| AF macro cal: offset `0x10c`, 4 bytes | `imx219_eeprom.c:366,375`; ROM (`r1 = #268`) |
| AF pan/infinity cal: offset `0x110`, 4 bytes | `imx219_eeprom.c:367,379`; ROM (`r1 = #272`) |
| Both `u32`s are little endian | read byte-by-byte into the `unsigned int` object (`imx219_eeprom.c:375-383`), ARM LE; sentinel `-1` at `:352-353` |
| Reads are lazy, at sensor open, with rails on | `imgsensor/src/mt6735/imx219_mipi_raw/imx219mipiraw_Sensor.c:1202` calls `imx219_update_sysfs_information()` after the chip-ID check in `open()` |
| LSC block at `0x300`: u16 LE length prefix, then payload | `imx219_eeprom.c:324-332` |
| CAM_CAL window whitelist `0x300`, `0x210`, `0x140`, `0x10c` | `imx219_eeprom.c:320-345` (`selective_read_region`) |
| Legacy "mtk format" block uses slave `0xa0..0xa6` and is only called by the mt6735m sensor | `imx219_eeprom.c:170-230`; caller `imgsensor/src/mt6735m/imx219_mipi_raw/imx219mipiraw_Sensor.c:1238` (no caller under `src/mt6735`) |
| Samsung sysfs consumers | `drivers/leds/leds_s2mu005.c:729-733` externs, `:736-743` `rear_camfw`, `:744-751` `rear_camfw_full`, `:752-798` `rear_checkfw_{factory,user}`, `:820-826` `rear_afcal` (`"1 %u %u\n"`), `:1042-1057` `rear_cam_moduleid` (`"%c%c%c%c%c%02X%02X%02X%02X%02X\n"`), `:1080` `DEVICE_ATTR(rear_moduleid, ...)` |
| Legacy ABI | `cam_cal/inc/cam_cal.h:40` `CAM_CALIOC_G_READ = _IOWR('i', 5, stCAM_CAL_INFO_STRUCT)`; `cam_cal_define.h:23-27` (`{u4Offset,u4Length,pu1Params}`); char device major 226 (`imx219_eeprom.h:37`) |

## 3. Parity matrix

| Behavior | ROM/downstream implementation | Nura before | after | status |
|---|---|---|---|---|
| VCM binds on the Samsung module | DT `camera_main_af@18` (7-bit `0x0c`) | only `dongwoon,*` compatibles → never binds | `samsung,dw9807-vcm` added ahead of the fallbacks, with match data `{ .ring_mode = true }` | done |
| DAC programming (10-bit over `0x03`/`0x04`) | `DW9807AF.c:134-165` (two 2-byte writes, MSB `&0x3`) | 3-byte auto-increment write, same mask | unchanged + cross-reference comment | done (already matched) |
| DAC range 0..1023 | `DW9807AF.c:46,196-205` | `DW9807_MAX_FOCUS_POS 1023` | unchanged | done |
| Ring control mode on power-up (Samsung module only) | `DW9807AF.c:190-194,209` (`0x02=0x02`) | resume wrote `0x02=0x00` | resume writes `0x02=0x02` (`DW9807_CTL_RING`) **only** when the OF match data says `ring_mode` (i.e. `samsung,dw9807-vcm`); generic path still writes `0x02=0x00` (`DW9807_CTL_DIRECT`) | done |
| Power-down | downstream cuts the `vcamaf` rail only | `0x02=0x01` (upstream PD bit) | unchanged for every compatible: `DW9807_CTL_PD` in suspend | done (kept, superset) |
| Gradual lens ramp before power-down | `DW9807AF.c:303-330` (100/10 steps, 1-2 ms) | 16-step ramp, 1 ms | kept unchanged (task: keep existing ramp) | done (granularity differs by design) |
| Current-position read-back after init | `DW9807AF.c:97-132`, but the LSB read check at `:123` is wrong (1 vs 2) → always 0 | `current_val` starts at 0, tracks `s_ctrl` | not ported; downstream path is dead code, documented | done (documented) |
| `vdd` regulator / `powerdown-gpios` | none in the VCM driver; rails + pinctrl belong to `kd_camera_hw1` | none | omitted, documented | done |
| `V4L2_CID_FOCUS_AUTO` | not present downstream | not present | not added | done |
| EEPROM access from the VCM driver | `s4DW9807AF_Eflash_*` (`DW9807AF.c:50-95`) | n/a | superseded by `imx219-eeprom` | done |
| Module MFR info | `0x20`, 11 bytes (`imx219_eeprom.c:365,371`) | n/a | `mfr_info` sysfs + raw nvmem range | done |
| Samsung module ID | `0x50`, 10 bytes (`:369,387`) | n/a | `module_id` sysfs (downstream `%c…%02X` format) + raw nvmem range | done |
| AF macro calibration | `0x10c`, 4 bytes LE (`:366,375`) | n/a | `af_macro_cal` sysfs (`%u`) + nvmem cell | done |
| AF pan (infinity) calibration | `0x110`, 4 bytes LE (`:367,379`) | n/a | `af_pan_cal` sysfs (`%u`) + nvmem cell | done |
| Combined AF calibration string | `leds_s2mu005.c:820-826` `"1 %u %u\n"` | n/a | `afcal` attribute, identical format | done |
| Read timing | at sensor open, rails on (`imx219mipiraw_Sensor.c:1202`) | n/a | no probe i2c; every sysfs/nvmem read is a fresh i2c transaction | done |
| `CAM_CALIOC_G_READ` + `/dev/CAM_CAL_DRV` (major 226) | `cam_cal.h:40`, `cam_cal_define.h:23-27`, `imx219_eeprom.c:439-560` | n/a | not reproduced; mapped to raw nvmem reads (see below) | done (replaced) |
| `read_imx219_eeprom_mtk_fmt()` legacy block (`0xa0..0xa6`, 8-bit page aliases, 0x01/0x05/0x07/0x09/0x11/0x15/0x17 + 0xa2/0xa4/0xa6 pages) | `imx219_eeprom.c:170-230` | n/a | not registered: different slave, and the only caller is the mt6735m sensor variant (`mt6735m/.../imx219mipiraw_Sensor.c:1238`); this board's `src/mt6735` sensor never calls it | deferred |
| `rear_checkfw_factory` / `rear_checkfw_user` (string compare against `"W08KSIA"`) | `leds_s2mu005.c:752-798` | n/a | not implemented (Samsung LED-class policy, not calibration data); the raw `mfr_info` string is exposed so userspace can do the same check | deferred |
| Blank/erased module | downstream prints raw bytes / `0xffffffff` sentinel | n/a | `-ENODATA` for all-`0x00`/`0xff` blocks and for `0xffffffff` calibration | done (added) |
| LSC OTP window @ `0x300` | `imx219_eeprom.c:324-332` | n/a | readable through nvmem (offset < 0x400) | done |

### `CAM_CALIOC_G_READ` → nvmem mapping
* Downstream: `ioctl(fd, CAM_CALIOC_G_READ, {u4Offset,u4Length,pu1Params})` (`cam_cal_define.h:23-27`); the imgsensor implementation only answers four offsets — `0x300` (LSC, prefixed by its u16 length and only returned when `u4Length` equals that length, `imx219_eeprom.c:324-333`), `0x210`, `0x140`, `0x10c` (`:335-338`); anything else copies nothing and returns `0`.
* Nura: `read()`/`nvmem_cell_read()` on `/sys/bus/nvmem/devices/imx219-eeprom0/` maps offset→EEPROM byte 1:1 over `0..0x3ff`; the `0x300` length check is not enforced by the driver (it is a userspace protocol detail), consumers read `0x300`/`0x301` themselves. The char device and its ioctls are intentionally absent.

## 4. Compile-tested

```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-lens-cal"
cp "$M/nura.config" .config
./scripts/config -e MEDIA_SUPPORT -e MEDIA_CAMERA_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV \
  -e VIDEO_V4L2_SUBDEV_API -e V4L2_FWNODE -e V4L2_ASYNC -e I2C -e VIDEO_CAMERA_SENSOR \
  -e VIDEO_DW9807_VCM -e NVMEM -e VIDEO_IMX219_EEPROM
make ARCH=arm64 LLVM=1 olddefconfig
rm -f drivers/media/i2c/dw9807-vcm.o drivers/media/i2c/imx219-eeprom.o
make ARCH=arm64 LLVM=1 -j2 drivers/media/i2c/dw9807-vcm.o drivers/media/i2c/imx219-eeprom.o \
  2>&1 | tee "$M/logs/camera-lens-cal-build.log"
```

Result: `CC drivers/media/i2c/dw9807-vcm.o`, `CC drivers/media/i2c/imx219-eeprom.o`; `grep -cE "warning:|error:" build.log` → `0`. `olddefconfig` accepted the new Kconfig symbol (`CONFIG_VIDEO_IMX219_EEPROM=y`, `CONFIG_VIDEO_DW9807_VCM=y`).

Note: `MEDIA_CAMERA_SUPPORT` had to be enabled on top of the listed options because `menuconfig VIDEO_CAMERA_SENSOR` depends on it; without it both objects are dropped from `.config` silently. `scripts/checkpatch.pl --no-tree -f drivers/media/i2c/imx219-eeprom.c` and `... -f drivers/media/i2c/dw9807-vcm.c`: 0 errors, 0 warnings.

## 5. Inferred / hardware-unverified items

* **EEPROM size 0x400** `[INFERENCE]`: downstream declares no part size; 0x400 covers every documented field and the LSC header at 0x300 (`imx219_eeprom.c:324`). Reads beyond the physical device simply fail at the i2c layer (`-EREMOTEIO`) — no fabricated bytes.
* **Slave address 0x58 / 16-bit addressing** is derived, but from executed ROM code (disassembly of `read_imx219_eeprom` @ `c04f4598` and `imx219_update_sysfs_information` @ `c04f4c60`) plus `kd_sensorlist.c:454-462`, not from a datasheet. The exact EEPROM part number is unknown; the downstream comment "GT24C16" (`imx219_eeprom.c:193`) contradicts 16-bit addressing and is treated as a leftover from the imx135 driver.
* **Repeated start vs. STOP+START**: the driver issues one `i2c_transfer()` (write address, repeated start, read byte); downstream does `i2c_master_send()` + `i2c_master_recv()`. Both are valid EEPROM random-read sequences; only the former was implemented. Not exercised on hardware.
* **Blank-module policy** `[INFERENCE]`: the all-`0x00`/all-`0xff` check and the `0xffffffff` calibration sentinel (`imx219_eeprom.c:352-353`) are driver-side additions; downstream presented both as data.
* **Ring control mode on every resume (Samsung match only)** `[INFERENCE]`: downstream writes `0x02 = 0x02` once per lens open (`DW9807AF.c:190-194`, `:209`); mapping that to runtime resume is an equivalence assumption, not a literal port. The write is gated by OF match data, so no other compatible changes behaviour.
* **No regulator/GPIO in the VCM driver**: downstream really has none, but the rails (`vcamaf`) and module reset/powerdown are then owned by the sensor's power sequence. AF will not work until the imx219 driver's `power_on` raises `vcamaf` (cross-driver note).
* No hardware testing of any kind was performed; all register values come from downstream text or ROM disassembly.

## 6. ## DTS-FRAGMENT

```dts
/*
 * camera-lens-cal: DW9807AF VCM (7-bit 0x0c, downstream "camera_main_af@18")
 * and the IMX219 module EEPROM (7-bit 0x58, downstream slave id 0xB0).
 * Both live on i2c0, matching the downstream board files
 * (mt6737t_grandpplte_ltn_dtv_gpio_04.dtsi:765-780, rom_dtb_0.dts:3282-3299).
 */
&i2c0 {
	#address-cells = <1>;
	#size-cells = <0>;

	camera_main_af: lens@c {
		compatible = "samsung,dw9807-vcm", "dongwoon,dw9807-vcm";
		reg = <0x0c>;
		/*
		 * No supplies and no GPIOs: the DW9807AF driver downstream
		 * (dw9807af/DW9807AF.c) drives neither; the VCM is powered from
		 * the camera module rails sequenced by the sensor driver.
		 */
	};

	imx219_eeprom: eeprom@58 {
		compatible = "samsung,imx219-eeprom";
		reg = <0x58>;

		#address-cells = <1>;
		#size-cells = <1>;

		nvmem-layout {
			compatible = "fixed-layout";
			#address-cells = <1>;
			#size-cells = <1>;

			mfr_info: mfr-info@20 {
				reg = <0x20 0xb>;
			};

			module_id: module-id@50 {
				reg = <0x50 0xa>;
			};

			af_macro_cal: af-macro-cal@10c {
				reg = <0x10c 0x4>;
			};

			af_pan_cal: af-pan-cal@110 {
				reg = <0x110 0x4>;
			};
		};
	};
};
```

The nvmem device name (`imx219-eeprom`) comes from `nvmem_config.name` in the driver, not from DT; the cells above are for `nvmem-cells` consumers and mirror the sysfs attributes.

## 7. Patch path

`$M/patches/camera-lens-cal.patch` (4 files: Kconfig, Makefile, dw9807-vcm.c, new imx219-eeprom.c; no `.config`, no build artifacts).
