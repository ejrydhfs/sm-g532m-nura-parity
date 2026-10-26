# SM-G532M Nura driver patch series

Base tree: Nura `v7.0.10-mt6735`
(`https://gitlab.com/mt6735-mainline/linux`, tag `v7.0.10-mt6735`).

## Apply

```sh
cd linux
git checkout v7.0.10-mt6735
git am /path/to/deliverables/000*.patch
```

Or apply the combined diff:

```sh
git apply /path/to/deliverables/9999-combined.patch
```

The series is ordered:

1. `0001` pinctrl
2. `0002` display/panel
3. `0003` BMA254
4. `0004` haptics
5. `0005` MT6328 PMIC/RTC
6. `0006` I2C
7. `0007` Zinitix touch
8. `0008` MTK MSDC
9. `0009` MUSB/T-PHY
10. `0010` S2MU005 charger/MUIC/fuel gauge
11. `0011` board DTS
12. `0013` STK3013 proximity
13. `0014` S2MU005 flashlight

`0012-required-config.txt` is not a patch; merge it into the kernel config.

## Subsystem series

New driver sets are in `subsystems/patches/` with coordinator reports in
`subsystems/reports/`:

- `wcn-subsystem.patch` — WMT/CONSYS, WLAN, BT, GPS, FM
- `ccci-integration.patch` — CCCI core/loader, CLDMA, util, CCMNI, C2K
- `camera-subsystem.patch` — IMGSYS, SENINF, IMX219, S5K5E3YX, lens/cal
- `audio-integration.patch` — MT-SoC AFE, MT6328/MT6331 codec, ACCDET, machine
- `vcodec-integration.patch` — VENC, VDEC and `/dev/Vcodec` core

Per-driver patches are alongside the consolidated series.

## Required config

```text
CONFIG_CHARGER_S2MU005=m
CONFIG_USB_MUSB_DUAL_ROLE=y
CONFIG_STK3013=m
CONFIG_LEDS_CLASS_FLASH=m
CONFIG_LEDS_S2MU005_FLASH=m
```

## Build

The reference build used clang/lld. GNU host `objcopy` cannot rewrite this
kernel image; use LLVM binutils.

```sh
export PATH="/path/to/llvm/bin:$PATH"
make ARCH=arm64 LLVM=1 -j8
```

Verified result for the series:

```text
rc=0
warning/error count: 0
Image.gz: 9.7 MB
vmlinux: 237 MB
```

## Artifacts

`artifacts/` contains:

- `Image.gz`
- `mt6737t-samsung-grandpplte.dtb`
- `System.map`
- `.config`
- `stk3013.ko`
- `leds-s2mu005.ko`
- `led-class-flash.ko`
- `s2mu005-charger.ko`
- `s2mu005-battery.ko`
- `zinitix.ko`
- `bmc150-accel-i2c.ko`

## Status

Compile-tested only. No phone was attached. See `PARITY_REPORT.md` and the
per-driver reports in `../logs/` for hardware-unverified paths.
