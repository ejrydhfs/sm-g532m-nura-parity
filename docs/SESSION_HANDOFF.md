# SM-G532M → Nura Kernel Driver Program — Session Handoff

## Objective

Bring Samsung SM-G532M (`grandpplteub`, MT6737T) hardware support toward parity
with the Nura (formerly postmarketOS) Linux kernel, using the Android ROM,
Samsung/MTK downstream GPL source, and reverse engineering of vendor ELFs.

## Inputs

- ROM: `SAMFW.COM_SM-G532M_PBS_G532MUMU1ASD1_fac.zip`
- ROM kernel: `3.18.19-14373813`, 32-bit ARM, Android 6.0.1
- Nura baseline: `v7.0.10-mt6735`, commit `046883c0c`
- Downstream Samsung/MTK tree: `src/downstream-grandpplte`
- ROM artifacts:
  - `zip/vmlinux.elf` — symbol-recovered kernel
  - `zip/symbols.txt`
  - `zip/vmlinux.strings`
  - `zip/system.raw.img` — vendor libs/firmware
  - `zip/boot.img`
- Build config: `nura.config`
- Analysis reports: `DRIVER_MATRIX.md`, `PARITY_REPORT.md`, all `logs/`

## What was produced

| Category | Count | Location |
|---|---|---|
| Core driver patches | 14 | `deliverables/0001..0014-*.patch` |
| Subsystem series | 5 integrated + 22 leaf patches | `subsystems/patches/` |
| Gap-closure patches | 19 | `subsystems/gaps/patches/` |
| Reverse-engineered patches | 17 | `subsystems/rem/patches/` |
| Final internals patches | 4 | `subsystems/rem2/patches/` |
| Full source baseline | 1 | `9998-full-baseline.patch` (2.9 MB) |
| Core-only combined patch | 1 | `9999-combined.patch` |
| Build artifacts | Image/DTB/System.map/.config/modules | `artifacts/` |
| Handoff/report | 2 | `SESSION_HANDOFF.md`, `PARITY_REPORT.md` |

### Driver groups covered

- Core: pinctrl, S6D78A0 panel/backlight, BMA254, haptics, MT6328 PMIC/RTC,
  I2C, Zinitix BT541, MTK MSDC, MUSB/T-PHY, S2MU005 charger/MUIC/fuel gauge,
  STK3013 proximity, S2MU005 flashlight.
- WCN: WMT/CONSYS, WLAN HIF + cfg80211 station, BTIF/HCI, GNSS, FM/RDS/I2S.
- CCCI: CCIF/core, CLDMA, modem loader, CCMNI, C2K/MD3, MPU, RPC, AEE dumps,
  DHL exception handshake, clk-buf.
- Camera: IMGSYS, SENINF, D-PHY, IMX219, S5K5E3YX, DW9807 VCM, EEPROM/OTP,
  ISP char pipeline, IOPIPE/TPIPE userspace tables.
- Audio: MT6735 AFE/DAIs, MT6328/MT6331 codec, ACCDET, machine/jack,
  audiosys clocks, voice/BT PCM, speaker protection, HP impedance, ext amp.
- Vcodec: VENC, VDEC, `/dev/Vcodec` ABI, DVFS, hibernation, CPU OPP.

## Build state

Full arm64 kernel build succeeds:

```text
rc=0
warnings/errors: 0
vmlinux: 244 MB
Image.gz: 11 MB
board DTB: 23 KB
```

Build recipe:

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
export TMPDIR="$M/tmp"          # /tmp is small/full
cd "$M/worktrees/integration"
make ARCH=arm64 LLVM=1 -j8
```

Required toolchain quirk: GNU host `objcopy` cannot rewrite this kernel image.
Use LLVM binutils. The extracted LLVM tools live at `$M/llvmpkg/usr/bin`.

## How to reproduce from a clean tree

```sh
git clone --depth 1 --branch v7.0.10-mt6735 \
  https://gitlab.com/mt6735-mainline/linux.git linux
cd linux
git apply /path/to/deliverables/9998-full-baseline.patch
cp /path/to/nura.config .config
# merge 0012-required-config.txt into .config
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j8
```

`9998-full-baseline.patch` contains all source changes, including new files.
`9999-combined.patch` is the earlier core-only series, kept for review.

## Critical integration notes

1. `drivers/misc/Kconfig` must contain:
   `source "drivers/misc/mediatek/Kconfig"`
2. `drivers/misc/Makefile` must contain:
   `obj-y += mediatek/`
   Without these two, every `CONFIG_MTK_*` symbol disappears after
   `olddefconfig` and module targets become “No rule to make target”.
3. Build composite module objects, not bare `foo.o`, so directory `ccflags-y`
   applies.
4. `CONFIG_MTK_CCCI` and friends are `=y`; WCN/audio/vcodec/camera are modules.
5. `CONFIG_VIDEO_MTK_ISP=m` is required for the ISP char driver.
6. `drivers/misc/mediatek/clkbuf` and `aee` are gated by
   `CONFIG_MTK_CLKBUF` and `CONFIG_MTK_AEE_FEATURE`.

## Known remaining blockers

These are data/firmware blockers, not missing source files:

- Camera per-module register **values** (BNR/LSC/CCM/GGM, MIXER3/NR3D/MFB/G2C)
  are runtime NVRAM/OTP data; the NVRAM image is absent.
- Camera FLK/FKO and AE/AWB statistic register lists have no register
  immediates in any vendor ELF.
- No camera DSP stage exists; post-processing is P2/DIP.
- clk-buf NFC consumer needs an NFC driver (Nura has none).
- AEE `AE_IND` userspace path needs the closed `com.mtk.aee.aed` daemon/APK,
  absent from `system.raw.img`.
- AEE ipanic/mrdump/dram-console/red-screen are separate downstream ports.
- CLDMA TX purge ordering and L3 per-queue bit attribution need hardware.
- WCN scan retune/WoWLAN/filter firmware paths and FM ASRC table.
- CCCI MASP/HACC key material, ADC EFUSE calibration, AEE DB client.
- Vcodec MMDVFS SMI-bandwidth/VCORE vote (no MMDVFS framework).
- Everything is compile/link-tested only; no phone attached.

## Agent handoff

### Agents used

- `mimo-pro-worker` / `mimo-pro` — initial attempts; later failed with 402.
- `opencode-coordinator` / `opencode-worker` on
  `opencode-go/deepseek-v4.1-flash` — subsystem fan-out.
- `deepseek-coordinator` / `deepseek-worker` on `deepseek/deepseek-flash` —
  gap and reverse-engineering rounds.
- Agent definitions are in `/home/asdf/.omp/agent/agents/`.

### To continue with subagents

1. Ensure provider balance is topped up.
2. Verify `9998-full-baseline.patch` applies to the Nura tag.
3. Create one worktree per remaining item under
   `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work/worktrees/`
   (143 GB disk; primary disk is nearly full).
4. Use `deepseek-coordinator` for an area and `deepseek-worker` for leaves;
   max 5 leaves, one worktree per leaf, no shared integration tree edits.
5. Give each leaf the exact missing input from the reports in
   `subsystems/rem2/reports/` and `subsystems/rem/reports/`.
6. Require `git diff --binary` + report + compile test. Do not accept stubs or
   invented tables.
7. Integrate leaves in a fresh area worktree, then merge into
   `worktrees/integration` and run the full build.

### Do not redo

- Core patches `0001..0014`
- The five subsystem series
- The 19 gap patches
- The 17 RE patches
- The 4 final internals patches
- Kconfig/Makefile hooks and the WMT/GPS API fixes

Use `9998-full-baseline.patch` as the starting point.

## Paths

Primary (nearly full, 15 GB free):

```text
/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work/
```

Mirror (131 GB free):

```text
/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work/deliverables/
```
