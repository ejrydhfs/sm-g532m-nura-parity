# SM-G532M ROM → Nura driver recreation report

Target: Samsung SM-G532M (`grandpplteub`), MT6737T, Android 6.0.1,
ROM kernel `3.18.19-14373813`.
Nura baseline: `v7.0.10-mt6735`, arm64.

## Result

- **14 core patches** for existing Nura drivers.
- **5 subsystem series / 22 leaf drivers**: WCN, CCCI, camera, audio, vcodec.
- **19 gap-closure patches.**
- **17 reverse-engineering patches** for proprietary tables/firmware ABIs.
- **4 final internals patches**: IOPIPE/TPIPE, clk-buf, AEE DB, CLDMA DHL.
- **Full arm64 build**: `rc=0`, 0 warnings/errors.

## Final internals closures

- **Camera IOPIPE/TPIPE:** 6 `libcam.iopipe.so` port/path tables, 25-module/
  90-instance/24-NVRAM-type catalogue, 19 TPIPE enable-word records over 13
  modules, CQ0C word builder, 15 tuning symbols and per-sensor generators.
  Implemented as userspace `tools/mediatek/camera/iopipe`.
- **clk-buf:** new platform driver `drivers/misc/mediatek/clkbuf` on
  `mediatek,rf_clock_buffer` + scpsys, BSI words, `spm_ap_bsi_gen`,
  `/sys/power/clk_buf`, `clk_buf_ctrl` API, CCCI RPC 0x4008 and CONSYS
  co-clock wiring.
- **AEE DB:** `/dev/aed0` (EE) and `/dev/aed1` (KE), 24-byte AE_Msg protocol,
  EE queue/worker, `libaed.so` ioctls, exported `aed_*_exception_api`,
  CCCI dump wiring and `aee-db` tool.
- **CLDMA DHL:** `ccci_cldma_stop_for_ee()` and
  `ccci_cldma_dhl_rx_alive()` implemented register-for-register; HIF EX
  INIT/CLEARQ handshake rewired with the 2 s RX-alive window.

## Previous closures

- Camera: D-PHY lane/settle, ISP char device/DMA/IRQ, OTP parser/LSC,
  IMGSYS resets/SMI.
- WCN: WLAN cfg80211 station, WMT PSM/coredump/coex, FM I2S/ASoC.
- CCCI: MPU/RMPU, RPC, AEE/SBP/DHL, DTS carve-outs.
- Audio: audiosys clocks, voice/BT PCM, speaker protection, HP impedance,
  external amp.
- Vcodec: DVFS, hibernation, CPU OPP, command dispatch.

## Remaining exact blockers

Only data/behaviour absent from every available image remains:

- Camera per-module register **values** for BNR/LSC/CCM/GGM and MIXER3/NR3D/
  MFB/G2C_SHADE are runtime NVRAM/OTP data, not static tables; per-sensor
  NVRAM defaults are generated, not stored.
- Camera FLK/FKO and AE/AWB statistic register lists have no register
  immediates in any vendor ELF.
- No camera **DSP** stage exists in any source; post-processing is P2/DIP.
- clk-buf NFC consumer needs an NFC driver (not present in Nura).
- AEE userspace `AE_IND` raise path needs the closed `com.mtk.aee.aed`
  daemon/APK, absent from `system.raw.img`.
- AEE ipanic/mrdump/dram-console/red-screen are separate downstream ports.
- CLDMA TX ring purge ordering and L3 per-queue bit attribution need
  hardware-level validation.
- WCN scan retune/WoWLAN/filter firmware paths and FM ASRC table remain.
- CCCI MASP/HACC key material, ADC EFUSE calibration and AEE DB client
  external providers remain.

## Build

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
export TMPDIR="$M/tmp"
cd "$M/worktrees/integration"
make ARCH=arm64 LLVM=1 -j8
```

Result: `vmlinux` 244 MB, `Image.gz` 10 MB, board DTB 22 KB,
0 warnings/errors. Modules include WCN, MTK-ISP, IMGSYS, SENINF,
audiosys, audio, vcodec, STK3013, flashlight; CCCI, AEE and clk-buf are
built in.

## Deliverables

```text
deliverables/
 0001..0014-*.patch
 9998-full-baseline.patch
 9999-combined.patch
 subsystems/patches/        # 5 series + 22 leaves
 subsystems/gaps/patches/   # 19 gap patches
 subsystems/rem/patches/    # 17 RE patches
 subsystems/rem2/patches/   # 4 final internals patches
 artifacts/                 # Image.gz, DTB, System.map, .config, modules/
```

No hardware was attached; register programming is source/ROM/vendor-ELF
derived and compile/link-tested only.
