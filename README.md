# SM-G532M → Nura Kernel Driver Parity

Documentation, patches, driver source and userspace tools from a
reverse-engineering effort to bring Samsung **SM-G532M** (`grandpplteub`,
MediaTek **MT6737T**) hardware support toward parity with the
**Nura** (formerly postmarketOS) mainline-based kernel.

This repository contains **no ROM images, firmware, vendor ELFs, or other
proprietary blobs**. It contains source patches, original/reworked driver code,
documentation, build artifacts, and a sanitized session transcript.

## Target

| | |
|---|---|
| Device | Samsung Galaxy J2 Prime / Grand Prime+ |
| Model | SM-G532M (`grandpplteub`) |
| SoC | MediaTek MT6737T |
| ROM | Android 6.0.1, kernel `3.18.19-14373813`, 32-bit ARM |
| Downstream source | Samsung/MTK GPL drop for `grandpplte` |
| Target kernel | Nura `v7.0.10-mt6735`, commit `046883c0c`, arm64 |

## Status

- Full arm64 kernel build: **`rc=0`, 0 warnings/errors**
- `Image.gz`: 11 MB
- board DTB: 23 KB
- vmlinux: 244 MB
- CCCI, AEE, clk-buf built in
- WCN, camera ISP/IMGSYS/SENINF, audio, vcodec, STK3013 and flashlight modules
- Camera IOPIPE/TPIPE tables and userspace camera service included
- No hardware validation; all register programming is source/ROM/vendor-ELF
  derived and compile/link-tested only

See [`docs/PARITY_REPORT.md`](docs/PARITY_REPORT.md) for the full status and
remaining data blockers.

## Repository layout

```text
docs/
  SESSION_HANDOFF.md        complete agent handoff
  PARITY_REPORT.md          final parity status
  DRIVER_MATRIX.md          ROM/upstream/Nura driver matrix
  PARITY_CONTEXT.md         work rules and build environment
  CCCI_CONTRACT.md
  CCCI_GAP_CONTRACT.md
  WCN_GAP_CONTRACT.md
  REM_WCN_CONTEXT.md
  reports/                  per-driver and coordinator reports

patches/
  core/                     core driver series 0001-0014
  subsystems/               5 subsystem series + 22 leaf patches
  gaps/                     19 gap-closure patches
  rem/                      17 reverse-engineered patches
  rem2/                     4 final internals patches
  9998-full-baseline.patch  complete source snapshot
  9999-combined.patch       earlier core-only combined diff

code/
  drivers/                  new/reworked kernel driver source
  include/                  UAPI/kernel headers added by the series
  tools/mediatek/           userspace camera, AEE, CCCI, WCN, FM RDS tools

session/
  omp-session-public-sanitized.zip
  README.md

LICENSE                     GPL-2.0
SHA256SUMS
```

## Apply and build

Baseline: Nura `v7.0.10-mt6735`
(`https://gitlab.com/mt6735-mainline/linux`).

```sh
git clone --depth 1 --branch v7.0.10-mt6735 \
  https://gitlab.com/mt6735-mainline/linux.git linux
cd linux
git apply /path/to/9998-full-baseline.patch
cp /path/to/nura.config .config
# merge patches/core/0012-required-config.txt into .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j8
```

Use real LLVM binutils. GNU host `objcopy` cannot rewrite this kernel image.

### Required config highlights

```text
CONFIG_CHARGER_S2MU005=m
CONFIG_USB_MUSB_DUAL_ROLE=y
CONFIG_STK3013=m
CONFIG_LEDS_CLASS_FLASH=m
CONFIG_LEDS_S2MU005_FLASH=m
CONFIG_MTK_WCN_WMT=m
CONFIG_MTK_WCN_WLAN=m
CONFIG_MTK_WCN_BT=m
CONFIG_MTK_WCN_GPS=m
CONFIG_MTK_WCN_FM=m
CONFIG_MTK_BTIF=m
CONFIG_MTK_CCCI=y
CONFIG_MTK_CCMNI=y
CONFIG_MTK_CCCI_C2K=y
CONFIG_MTK_CCCI_CLDMA=y
CONFIG_MTK_CCCI_UTIL=y
CONFIG_VIDEO_MTK_IMGSYS=m
CONFIG_VIDEO_MTK_SENINF=m
CONFIG_VIDEO_MTK_ISP=m
CONFIG_SND_SOC_MT6735=m
CONFIG_SND_SOC_MT6735_MT6331=m
CONFIG_SND_SOC_MT6331=m
CONFIG_MTK_ACCDET=m
CONFIG_COMMON_CLK_MT6735_AUDIOSYS=m
CONFIG_MTK_VCODEC_MT6735=m
CONFIG_MTK_CLKBUF=y
CONFIG_MTK_AEE_FEATURE=y
```

The complete fragment is `patches/core/0012-required-config.txt`.

## Critical integration notes

1. `drivers/misc/Kconfig` must `source "drivers/misc/mediatek/Kconfig"`.
2. `drivers/misc/Makefile` must contain `obj-y += mediatek/`.
3. Build composite module objects, not bare `foo.o`, so directory `ccflags-y`
   applies.
4. `/tmp` may be too small; set `TMPDIR` to a large disk.

## Session archive

`session/omp-session-public-sanitized.zip` was produced with
[`alosslessdev/omp-archive`](https://github.com/alosslessdev/omp-archive-main).
It contains the main transcript, agent metadata, job index and standalone HTML
viewer. It is the **no-artifacts** variant: untruncated tool logs and binary
artifact trees are excluded. One Google API key present in the raw transcript
was redacted. The full sanitized archive is published as a release asset.

**Full sanitized archive (release asset):**
<https://github.com/ejrydhfs/sm-g532m-nura-parity/releases/tag/session-archive-2026-10-26>

See [`session/README.md`](session/README.md).

## Remaining exact blockers

- Camera per-module tuning values (BNR/LSC/CCM/GGM, MIXER3/NR3D/MFB/G2C) are
  runtime NVRAM/OTP data; the NVRAM image is absent.
- FLK/FKO and AE/AWB statistic register lists have no register immediates in
  any available vendor ELF.
- No camera DSP stage exists; post-processing is P2/DIP.
- AEE `AE_IND` userspace path needs the closed `com.mtk.aee.aed` daemon/APK.
- AEE ipanic/mrdump/dram-console are separate downstream ports.
- CLDMA TX purge ordering and L3 per-queue bit attribution need hardware.
- WCN scan retune/WoWLAN/filter and FM ASRC tables remain.
- CCCI MASP/HACC key material, ADC EFUSE calibration and AEE DB client remain.
- Vcodec MMDVFS SMI-bandwidth/VCORE vote remains.
- No phone attached; hardware validation is pending.

## Licensing and provenance

Kernel code and patches are licensed **GPL-2.0-only**; see `LICENSE`.
They incorporate work derived from the Linux kernel, Nura/pmaports, and the
Samsung/MTK GPL downstream kernel. Copyright notices are retained in the
patches.

This is an independent community effort and is not affiliated with Samsung,
MediaTek, Nura, or postmarketOS.

No proprietary firmware or ROM content is distributed here. Users must obtain
firmware legally for their own device.
