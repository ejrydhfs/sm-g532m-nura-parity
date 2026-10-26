# rem-camera-otp — OTP / lens-shading / 3A tables into a userspace parser-provider

Worktree: `$M/worktrees/rem-camera-otp` (detached at `src/nura-linux` HEAD `046883c0c`,
`parity-baseline-all.patch` applied, plus the two `drivers/misc` baseline fixups)
Deliverables: `$M/patches/rem-camera-otp.patch`, this report,
`$M/logs/rem-camera-otp-build.log`, `$M/logs/rem-camera-otp-olddefconfig.log`

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Status summary

| Item | Status | Result |
|---|---|---|
| (a) IMX219 MTK OTP image parser via the nvmem provider | **closed** | All fields decoded with downstream file:line per offset; 0x3bb-byte image assembled exactly as `read_imx219_eeprom_mtk_fmt()` does. |
| (b) S5K5E3YX OTP sysfs attribute + userspace page parser | **closed** | Real page-select + burst read on `drivers/media/i2c/s5k5e3yx.c`; driver object builds with `W=1`, 0 warnings. |
| (c) Lens shading: which code/data is real, OTP→gain table conversion | **closed for the real parts** | Conversion ported instruction-for-instruction (block header, channel permutation, pedestal, per-channel max, distance polynomial, 16.16 packing). The NVRAM-only coefficient sets are named as missing (section 6), not substituted. |
| (d) 3A static tables accessor/dump | **closed for the statically present tables** | 7 tables verified in `lib3a.so`, `libcamalgo.so`, `libcameracustom.so` with symbol/size/decode; BSS singletons reported as runtime-only. |

No DTS change is needed for this slice. No stub, placeholder, `TODO` or synthetic table is
present; the only non-device bytes the tool can emit are read out of a vendor library at a
symbol whose ELF size is re-checked on every access.

## 2. Files changed

```
 drivers/media/i2c/s5k5e3yx.c                | 156 ++++++++++++  (OTP sysfs attribute only)
 tools/mediatek/camera/otp/Makefile          |  72 +++
 tools/mediatek/camera/otp/camotp.h          | 326 +++++++++++++
 tools/mediatek/camera/otp/camotp.c          | 693 +++++++++++++  CLI
 tools/mediatek/camera/otp/camotp_elf.c      | 307 ++++++++++++  ELF32 reader
 tools/mediatek/camera/otp/camotp_imx219.c   | 110 ++++         MTK OTP image parser
 tools/mediatek/camera/otp/camotp_io.c       | 203 ++++++       read-only I/O + discovery
 tools/mediatek/camera/otp/camotp_lsc.c      | 301 ++++++++++++  LSC conversion
 tools/mediatek/camera/otp/camotp_s5k5e3yx.c | 188 +++++++      OTP page parser
 tools/mediatek/camera/otp/camotp_selftest.c | 633 +++++++++++   fixtures + negative tests
 tools/mediatek/camera/otp/camotp_tables.c   | 148 ++++++       table registry
 11 files changed, 3138 insertions(+), 5 deletions(-)
```

Nothing else is touched: no libcamdrv/camdrv/seninf/imgsys/isp file, no DTS, no commit.

### 2.1 Parent build wiring (applied at integration, now part of the patch)

`tools/mediatek/camera/Makefile`:

```make
all: static shared cli service ; @$(MAKE) -C otp
```

and both `check:` variants gained `@$(MAKE) -C otp check`, so the parent `make all` and
`make check` build and test the OTP provider. The line is a normal rule with prerequisites plus a
recipe, verified with GNU make. `build/` under `otp/` is covered by the parent
`tools/mediatek/camera/.gitignore`.

## 3. Evidence

### 3.1 IMX219 MTK OTP image (`imgsensor/src/mt6735/imx219_mipi_raw/imx219_eeprom.c`)

The image is the union byte array downstream fills, *not* the C struct: `OTP_MTK_TYPE`
would place `CaliVer` at offset 4 through natural alignment, while every read uses explicit
byte offsets in the union (`imx219_eeprom_data.Data[...]`).

| Field | Image offset | Size | Downstream line |
|---|---|---|---|
| flag byte | 0x000 | 1 | read is `#if 0`, `:187-189` — provider keeps the byte, does not decode it |
| calibration version (LE u32) | 0x001 | 4 | `:192` |
| serial number (LE u16) | 0x005 | 2 | `:198` |
| version | 0x007 | 1 | `:201` (first byte of the 2-byte read) |
| AWB/AF info | 0x008 | 1 | `:201` (second byte) |
| unit AWB R/Gr/Gb/B | 0x009 | 4 | `:204` (first four of the 8-byte read) |
| golden AWB R/Gr/Gb/B | 0x00d | 4 | `:204` (last four) |
| AF infinity (LE u16) | 0x011 | 2 | `:207` |
| AF macro (LE u16) | 0x013 | 2 | `:207` |
| LSC payload length (LE u16) | 0x015 | 2 | `:210` |
| LSC payload | 0x017 | length | `:219` (page 0 tail), `:221-224` (pages 0xa2/0xa4), `:225` (page 0xa6) |
| manufacturer information | 0x020 | 11 | `:365`, `:371` |
| module id | 0x050 | 10 | `:369`, `:387` (`CONFIG_GET_REAR_MODULE_ID`) |

Populated image size: 0x01..0xff (255) + 0x100..0x2ff (512) + 0x300..0x3ba (187) = 0x3bb
(955) bytes; the LSC payload capacity is 0x3bb − 0x017 = 932 bytes. A payload longer than
that is rejected with `-ERANGE` instead of reading past the assembled image.

### 3.2 S5K5E3YX OTP pages (`s5k5e3yxmipiraw_Sensor.c`)

`get_otps_data()` `:392-414`:

```
0x0a00 = 0x04   ; :395  initial state
0x0a02 = page   ; :396  page select, 0..3
0x0a00 = 0x01   ; :397  read mode
delay 1 ms      ; :400  "to wait Tmin = 47us"
0x0a04..0x0a08  ; :402-406  five bytes of the page buffer
0x0a00 = 0x04   ; :408
0x0a00 = 0x00   ; :409
```

Byte roles from the log strings `:409-413`: `[0]` calibration flag, `[1]` lens-shading table
index, `[2..4]` R/G/B calibration. Downstream left the four calls commented out in `open()`
(`:1161-1166`), so the attribute is the first real consumer.

Burst read: `devm_cci_regmap_init_i2c(client, 16)` builds a `reg_bits=16, val_bits=8` map
(`drivers/media/v4l2-core/v4l2-cci.c:186-196`). For `val_bits == 8` regmap sets
`format.parse_inplace = regmap_parse_inplace_noop` and `map->read = bus->read`
(`drivers/base/regmap/regmap.c:862, 977-981`), so `regmap_bulk_read()` takes the raw path
(`regmap.c:3215-3218`) and issues one register-address write plus one five-byte read — a
single I2C transfer pair with a 2-byte big-endian register address.

### 3.3 Lens shading: what is real in `libcameracustom.so`

Symbols (dynsym sizes):

| Symbol | Address | Size | Section | Verdict |
|---|---|---|---|---|
| `ShadingATNTable` | 0x0050af18 | 0x1f8 | `.data` (file 0x4f2f18) | **real static table**: 21 × 6 × s32, `1e6 == 1.0` |
| `channel_lsc_data` | 0x007b00a0 | 0x38e | `.bss` | runtime only (NVRAM) |
| `channel_lsc_data_2800k` | 0x007b042e | 0x38e | `.bss` | runtime only (NVRAM) |
| `channel_lsc_data_4100k` | 0x007b07bc | 0x38e | `.bss` | runtime only (NVRAM) |
| `g_lsc_cali_info` | 0x007afe74 | 0x40 | `.bss` | runtime only |
| `g_lsc_param` | 0x007aff74 | 0x64 | `.bss` | runtime only |
| `g_lsc_result` | 0x007aff40 | 0x30 | `.bss` | runtime only |
| `g_rLSCCaliINIParam` | 0x007b0060 | 0x40 | `.bss` | runtime only |

Function verdicts (Thumb, verified by disassembly from the dynsym value):

| Symbol | Entry | Size | Verdict |
|---|---|---|---|
| `vLSC_PARAM_INIT` | 0xf3e84 | 0x108 | real: derives grid steps/normaliser, zero-tests the polynomial coefficients |
| `vLSC_Calibration_INIT` | 0xf3f8c | 0x94 | real: `malloc(n_x*n_y*16)`, builds the column/row coordinate tables |
| `vLSC_AvgRaw_Load` | 0xf4150 | 0xfa | real: four planar byte blocks → floats |
| `vLSC_AvgRaw_Process` | 0xf4020 | 0x130 | real: bayer permutation + pedestal subtraction |
| `mrLSC_AvgToGainTbl` | 0xf4250 | 0x34c | real: per-channel max, distance ratio, gain, packing |
| `LSC_AVGRAW_to_Gaintable` | 0xf45a0 | 0x154 | real orchestrator |
| `mrLSC_POLY_RatioFloat_Get` | 0xf3e1a | 0x6a | real polynomial |
| `mrLSC_POLY_Ratio_Get` | 0xf3da8 | 0x72 | real (int variant) |
| `u4LSC_Integer_SQRT` | 0xf3d48 | 0x60 | real integer sqrt |
| `DoISPDynamicShadingLoad` | 0xf6254 | 0x18 | **stub**: log + `return 0` |
| `DoISPFixShadingLoad` | 0xf626c | 0x18 | **stub**: log + `return 0` |
| `DoISPSensorShadingLoad` | 0xf6284 | 0x18 | **stub**: log + `return 0x1000` |
| `DoISPSlimShadingLoad` | 0xf6440 | 0x38 | thin wrapper: reads `[arg+0xb8]`, conditionally calls 0xf6430, returns `[[global]+8]` |
| `GetCameraDefaultPara` | 0xf18e8 | 0xb8 | dispatcher: looks up a 0x38-byte per-sensor record and calls its generator |

Constant/data evidence used by the port:

| What | Evidence |
|---|---|
| block header offsets/roles | `LSC_AVGRAW_to_Gaintable` 0xf45be-0xf45e0 (bytes 0,1,2@+2, u16@+4, u16@+6, byte@+8), 0xf4650-0xf4666 (bytes +0x0a..+0x0d) |
| plane base and stride | `vLSC_AvgRaw_Load` reads at base+0x2ae / +0x1ce / +0xee / +0x0e with a fixed 0xe0 stride (0xf4182, 0xf41c0, 0xf41fe, 0xf4230); total 0x0e + 4·0xe0 = 0x38e |
| 14-byte header (not 16) | `LSC_AVGRAW_to_Gaintable` copies 0x37e bytes from struct+0x10 into its argument area (0xf468e-0xf46a0); the callee reads plane 0 at its saved-register base + 0x0e, which is 2 bytes before that copy — consistent only with planes starting at +0x0e |
| channel permutation | `vLSC_AvgRaw_Process` 0xf403e-0xf4084: order 0 → (0,1,2,3), 1 → (1,0,3,2), 2 → (2,3,0,1), 3 → (3,2,1,0) |
| pedestal clamp-subtract | `vLSC_AvgRaw_Process` 0xf40a6-0xf4134 (`if v > ped then v-ped else 0`) |
| per-channel maximum | `mrLSC_AvgToGainTbl` 0xf42c0-0xf42ea |
| reference distance | `mrLSC_AvgToGainTbl` 0xf4306-0xf4346: `isqrt((((w>>1)^2 + (h>>1)^2) >> 2) << 12)`, `w`/`h` = u16 at +0x04/+0x06 |
| coordinate tables | `vLSC_Calibration_INIT` 0xf3fbe-0xf3ffc: first 0, last `extent-1`, interior `i*step`, `step = extent/(n-1)`; extents `w>>1`, `h>>1` (`vLSC_PARAM_INIT` 0xf3f24-0xf3f3e) |
| polynomial | `mrLSC_POLY_RatioFloat_Get` 0xf3e66-0xf3e74: `acc = c5; p = r; for k=4..0 { acc += c[k]*p; p *= r }` |
| coefficient order | call site 0xf43d2-0xf43ee passes `param[0x48], param[0x4c]` in registers and `param[0x50..0x60]` on the stack; the callee's memory array is `{arg3, arg2, arg1, arg0, w1, w0}` (0xf3e42-0xf3e60) |
| ratio values | `LSC_AVGRAW_to_Gaintable` 0xf45e4-0xf4648: `param[0x24+i] = atn[i]/atn[5]` for i=0..4, `param[0x38] = 1.0` |
| polynomial enable | `vLSC_PARAM_INIT` 0xf3e94-0xf3f22 stores 1 iff any of the five coefficients is non-zero |
| gain | `mrLSC_AvgToGainTbl` 0xf441c-0xf4460: `gain = max[ch] * ratio / avg`, `avg == 0 → 32768`, clamp |
| clamp value | `vLSC_PARAM_INIT` 0xf3ec8 / 0xf3f2a region `param[0x44] = 4`; `mrLSC_AvgToGainTbl` 0xf4316-0xf4338 `min(4<<13, 0xffff) = 32768` |
| packing | `mrLSC_AvgToGainTbl` 0xf4462-0xf4542: `scaled = gain * 8192.0f`, `+0.5` in double, `< 65535.0`, truncate to s32, `out[2k] = v0 | v1<<16`, `out[2k+1] = v2 | v3<<16` |
| literal pool | 0xf4578 = double 65535.0, 0xf4580 = float 8192.0, 0xf4584 = float 65535.0 (all four `vldr` sites resolve here) |

### 3.4 3A tables verified in the ROM libraries

| Library | Symbol | Address | Size | Section | Decode |
|---|---|---|---|---|---|
| libcameracustom.so | `ShadingATNTable` | 0x0050af18 | 0x1f8 | `.data` | 21 × 6 s32; words 0..4 divided by word 5 (`1e6`); entry 0 all-zero |
| lib3a.so | `_ZN7AwbAlgo14m_i4AWBLogLUT1E` | 0x0006a6e8 | 0x400 | `.rodata` | 256 s32, entry[i] ≈ 256·log2(i), entry[255] = 2047 |
| lib3a.so | `_ZN7AwbAlgo14m_i4AWBLogLUT2E` | 0x00058fe8 | 0x2000 | `.rodata` | 2048 s32, monotonic, entry[512] = 0, entry[1024] = 4932, entry[2047] = 9861 |
| lib3a.so | `LumLog2x1000` | 0x00071028 | 0x804 | `.data` | 513 s32, entry[i] ≈ 1000·log2(i), entry[512] = 9000 |
| lib3a.so | `g_i4PLweight` | 0x000719a0 | 0x80 | `.data` | 32 u32 monotonic weight ramp |
| libcamalgo.so | `_ZN7GmaCore15m_i4DefGmaBase0E` | 0x0067ad40 | 0x244 | `.rodata` | 145 u32 packed GMA base |
| libcamalgo.so | `_ZN7GmaCore14m_i4GmaSectionE` | 0x0067af98 | 0x244 | `.rodata` | 145 u32 packed GMA section |

Every size was re-read from the shipped ELF with `readelf -sW` and confirmed by dumping
through the tool (`camotp tables --lib ... --dump ...`); the "≈" decodes are labelled as such
because the tables are fixed-point approximations, not exact float formulas (checked
entry-by-entry: 256·log2(i) matches only within ±1 LSB).

Unrelated but supporting: `GetCameraDefaultPara` receives the NVRAM struct sizes
0x28000 (ISP), 0x7c04 (3A), 0x10081c (shading) and 0x512b4 (AE pline table) as immediate
arguments (0xf1934, 0xf1950, 0xf1960-0xf1962, 0xf1974-0xf1976) — they are produced by
generator functions, not stored as blobs.

## 4. Implementation description

### 4.1 `libcamotp` (userspace, host gcc)

* `camotp_elf.c` — bounded ELF32 LSB/EM_ARM reader: validates class/data/machine, phdrs,
  shdrs, `.dynsym`/`.dynstr`, maps vaddr→offset through `PT_LOAD` only, every offset checked
  against the file size. Used for both the table dumps and the `ShadingATNTable` input.
* `camotp_io.c` — `pread`-based read-only file access (`O_RDONLY|O_CLOEXEC|O_NONBLOCK`),
  sysfs text read, and discovery of the two sources. The nvmem device keeps exactly
  `nvmem_config.name` because the provider uses `NVMEM_DEVID_NONE`
  (`drivers/nvmem/core.c:961-963`), so the directory name under
  `/sys/bus/nvmem/devices` is `imx219-eeprom` (this kernel exposes no `name` attribute on
  nvmem devices, only `type`, `force_ro` and the binary `nvmem` file). The sensor attribute
  is found by the `driver` symlink of the i2c client pointing at `s5k5e3yx`, which proves
  the binding instead of relying on the client name.
* `camotp_imx219.c` — the MTK image parser (table in 3.1) with LE decoding, the 0x3bb bound
  check and blank/validity flags. Returns `-ERANGE` when the LSC length exceeds the
  populated image.
* `camotp_s5k5e3yx.c` — raw 20-byte parser and parser for the driver's text format
  (`pageN: 0a04=VV ... 0a08=VV`); rejects wrong register order, out-of-range page/value,
  duplicated or missing pages (`-EINVAL`/`-ERANGE`).
* `camotp_lsc.c` — the conversion described in 3.3, plus `camotp_isqrt()` and
  `camotp_lsc_poly()`. Rejections: `n_x < 2 || n_y < 2` (downstream divides by `n-1`),
  `n_x*n_y > 224` (fixed 0xe0 plane stride), `bayer > 3`, `width/height < 2`,
  `ref_distance == 0`, blank planes (`-ENODATA`), zero ATN normaliser (`-ERANGE`).
  A block whose ATN index is above 20 is accepted with the polynomial disabled, which is
  what a freshly loaded process computes (the vendor's `param[0x24..0x38]` are BSS zeros).
* `camotp_tables.c` — the registry from 3.4 with `camotp_table_read()`, which refuses to
  return bytes when the symbol size differs from the documented size.
* `camotp.c` — CLI:
  `imx219`, `s5k5e3yx`, `lsc` (`--region samsung` = EEPROM 0x300 length + 0x302 payload,
  `--region mtk` = OTP image payload at 0x17, `--block` = raw file, `--lib` = the library
  that supplies `ShadingATNTable`), `tables` (`--list`, `--dump SYMBOL`, `--prefix STR`).
* `camotp_selftest.c` — fixtures are explicit parser inputs, never fallbacks. It covers:
  image field decode, truncation, LSC-length overrun, blank flags; OTP page text/raw parsing
  and four malformed variants; `isqrt` edge values; polynomial values; the gain table for a
  2×2 block (8192/16384/32768/65535), pedestal subtraction (8192/18432/49152/65535), the
  bayer permutation for bayer 0 and 1, the distance polynomial doubling at r = 1, and eight
  rejection paths; plus an ELF32 fixture that exercises symbol lookup, vaddr translation and
  three malformed-ELF paths.

### 4.2 Kernel: S5K5E3YX OTP attribute

`drivers/media/i2c/s5k5e3yx.c` (addition only):

* `S5K5E3YX_REG_NVM_{CTRL,PAGE,DATA}` (0x0a00/0x0a02/0x0a04), control values 0x04/0x01/0x00.
* `s5k5e3yx_read_otp_page()` (line 1124) issues the downstream sequence and reads
  0x0a04..0x0a08 as one burst via `regmap_bulk_read()`; the two controller-reset writes run
  on the error path as well, mirroring `:408-409`.
* `otp_show()` (line 1157) takes a runtime-PM reference, then the new `otp_lock`, then
  refuses with `-EBUSY` if the mode register says streaming; it emits at most 256 bytes
  (`pageN:` + five `0aXX=VV` pairs per page) so the sysfs buffer bound is explicit.
* `otp_lock` is also taken around the register programming in `enable_streams()` and around
  the stop write in `disable_streams()`, so an OTP read can never interleave with a stream
  start/stop. Lock order is runtime-PM then `otp_lock` in all three callers.
* The attribute group is added in probe (line 1409) and removed in both the registration
  error path (1431) and `remove()` (1454).

## 5. Parity matrix

| Item | State | Reason |
|---|---|---|
| IMX219 calibration version | done | offset/size/endianness from `:192`; expected value 0xff000b01 |
| IMX219 serial, version, AWB/AF info | done | `:198`, `:201` |
| IMX219 unit + golden AWB | done | `:204` |
| IMX219 AF infinity/macro | done | `:207` |
| IMX219 LSC payload + length | done | `:210`, `:219-225` |
| IMX219 module id, manufacturer info | done | `:365`, `:369`, `:371`, `:387` |
| S5K5E3YX OTP page select + read | done | driver attribute, real I2C |
| S5K5E3YX OTP page decode | done | `:400-413` |
| LSC block layout | done | disassembly, section 3.3 |
| LSC bayer permutation + pedestal | done | disassembly |
| LSC per-channel max + distance polynomial | done | disassembly |
| LSC gain/16.16 packing | done | disassembly, exact constants |
| LSC coefficient source | done (static part) | `ShadingATNTable` extracted from the library at runtime |
| LSC NVRAM coefficient sets (`channel_lsc_data*`, `g_lsc_*`) | **blocked** | see section 6 |
| ISP-side shading loaders | not implemented by the vendor either | `DoISP*ShadingLoad` are stubs (section 3.3) |
| 3A static tables | done (7 tables) | section 3.4 |
| 3A NS3A/AWB/AE/AF/Pd/Flash singletons | **blocked** | `.bss`, filled at runtime |
| NVRAM default structs | **blocked** | generated procedurally, no blob in any image |

## 6. Irrecoverable items

1. **NVRAM camera calibration (shading coefficient sets).** `channel_lsc_data`,
   `channel_lsc_data_2800k`, `channel_lsc_data_4100k` (0x38e bytes each), `g_lsc_cali_info`
   (0x40), `g_lsc_param` (0x64), `g_lsc_result` (0x30) and `g_rLSCCaliINIParam` (0x40) are
   `.bss` in `libcameracustom.so`: the file contains no initialised bytes for them and the
   loader is NVRAM-only. The coordinator verified with `debugfs` on `zip/system.raw.img`
   that `/data` contains only `misc/`, `/cameradata` only XML + YUV patterns and `/etc` no
   camera tuning files, so the NVRAM image (normally `NVRAM/NVD_CAMERA_*` in the `nvram` or
   `protect_f` partition of the device) is present in none of the available images. Nothing
   is substituted; the conversion uses only the module OTP block plus `ShadingATNTable`.
2. **NVRAM default structs for 3A.** `GetCameraDefaultPara()` (0xf18e8) resolves a
   per-sensor record and calls its generator with struct sizes 0x28000 (ISP), 0x7c04 (3A),
   0x10081c (shading) and 0x512b4 (AE pline). The contents are produced by code, not stored;
   extracting them would require executing the vendor library, which is out of scope and
   would not be a "static table" any more.
3. **3A runtime singletons** in `lib3a.so` / `libcam.hal3a.v3.so`
   (`AwbAlgoDev<...>::singleton` 0xcb68, `AeAlgoDev<...>::singleton` 0x23f8,
   `AfAlgoDev<...>::singleton` 0xf708, `PdAlgoDev<...>::singleton` 0x2960,
   `AwbMgrDev<...>` 0x11f04, `AeMgrDev<...>` 0x6290, `AfMgrDev<...>` 0x4a88, …) are `.bss`
   and only exist after the algorithm objects initialise at runtime.
4. **Remaining unknowns, explicitly out of scope:** the semantic meaning of the block header
   words `+0x04`/`+0x06` beyond "half extents used for the coordinate tables" (they behave
   exactly as `width`/`height` in the distance math, and are named that way in the code and
   report); the unused byte `+0x09`; the unused ATN words are not touched.

## 7. Build evidence

Log: `$M/logs/rem-camera-otp-build.log` (0 occurrences of `warning:` or `error:`),
olddefconfig log: `$M/logs/rem-camera-otp-olddefconfig.log`.

Kernel (PATH `$M/llvmpkg/usr/bin:$M/bin`, `ARCH=arm64 LLVM=1`, `TMPDIR=$M/tmp/build-otp`):

```
cp $M/nura.config .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT … -e VIDEO_S5K5E3YX -e VIDEO_IMX219_EEPROM
make ARCH=arm64 LLVM=1 olddefconfig                     # rc=0; CONFIG_VIDEO_S5K5E3YX=y, CONFIG_VIDEO_IMX219_EEPROM=y
make ARCH=arm64 LLVM=1 -j2 drivers/media/i2c/s5k5e3yx.o drivers/media/i2c/imx219-eeprom.o
  CC      drivers/media/i2c/s5k5e3yx.o
  CC      drivers/media/i2c/imx219-eeprom.o
  rc=0
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/media/i2c/s5k5e3yx.o drivers/media/i2c/imx219-eeprom.o
  CC      drivers/media/i2c/s5k5e3yx.o
  CC      drivers/media/i2c/imx219-eeprom.o
  rc=0
```

Userspace (`gcc (GCC) 16.2.1`, `-Wall -Wextra -std=gnu11`):

```
make -C tools/mediatek/camera/otp clean all     # rc=0, no warnings
make -C tools/mediatek/camera/otp check         # "camotp selftest / PASS", rc=0
```

Read-only CLI evidence against the real ROM libraries (no device needed):

```
camotp tables --list
camotp tables --lib $M/zip/vendor-libs/libcameracustom.so --dump ShadingATNTable   # 0x50af18, 504 bytes
camotp tables --lib $M/zip/vendor-libs/lib3a.so --dump LumLog2x1000                # 0x71028, 2052 bytes
camotp tables --lib $M/zip/vendor-libs/libcamalgo.so --dump _ZN7GmaCore15m_i4DefGmaBase0E  # 0x67ad40, 580 bytes
```

Patch: `$M/patches/rem-camera-otp.patch`, 92 KB, generated with two temporary index trees
(`BT` = HEAD + `parity-baseline-all.patch`, `LT` = worktree). Verified:
`git apply --cached --check` against a baseline tree and `git apply --check -R` against the
worktree both succeed.

## 8. DTS fragment

**No device-tree change is required for this slice.**

* The OTP attribute is a device attribute of the i2c client, so it appears
  automatically once the sensor node from the S5K5E3YX slice is bound:
  `/sys/bus/i2c/devices/<bus>-<addr>/otp`.
* The nvmem node already comes from the lens-cal slice
  (`compatible = "samsung,imx219-eeprom"`, `reg = <0x58>`); no nvmem cell is needed,
  the tool reads the raw `nvmem` attribute. Adding fixed-layout cells is optional and
  does not change any offset used here.

The two paths the CLI resolves on its own (both overridable with `--nvmem`/`--sysfs`):

```
/sys/bus/nvmem/devices/imx219-eeprom/nvmem
/sys/bus/i2c/devices/<bus>-<addr>/otp
```

The `<addr>` is 0x10 or 0x36 depending on which of the two S5K5E3YX aliases the probe
matched (S5K5E3YX_I2C_ADDR_1/_2 in `drivers/media/i2c/s5k5e3yx.c`), and 0x58 for the
module EEPROM; discovery does not depend on either value.
