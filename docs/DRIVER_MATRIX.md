# Nura / ROM driver parity matrix

Target: Samsung SM-G532M (`grandpplteub`), MT6737T, Android 6.0.1, ROM kernel `3.18.19-14373813`.
Nura baseline: `v7.0.10-mt6735`, arm64, config `nura.config`.
Downstream reference: Samsung/MTK GPL tree `src/downstream-grandpplte`.

Status: `wave1` = subagent running; `wave2` = queued; `audit` = compare-only, patch only if a concrete gap is found.

## Wave 1 — user-visible parity gaps

| Driver | ROM evidence | Downstream reference | Nura target | Parity gap | Status |
|---|---|---|---|---|---|
| MTK MSDC / eMMC+SD | `msdc_*`, `mt_msdc_init` | `drivers/mmc/host/mediatek/mt6735/{sd.c,mt_sd_misc.c,msdc_hw_ett.c,sdio_autok.c,emmc_otp.c,emmc_rpmb.c,mt_dump.c}` | `drivers/mmc/host/mtk-sd.c` | HS400/HS200 tuning, SD power/detect, cache, RPMB, resume | wave1 |
| MUSB + T-PHY | `musb_*`, `mtk_otg_vbus_event`, `usb_phy_*` | `drivers/misc/mediatek/usb20/mt6735/{usb20.c,usb20_otg_if.c,usb20_host.c,usb20_phy.c}` | `drivers/usb/musb/mediatek.c`, `drivers/phy/mediatek/phy-mtk-tphy.c` | OTG host role, ID/VBUS, PHY tuning, charger detect | wave1 |
| S2MU005 / SM5504 / SM5701 | `s2mu005_*`, `sm5504_*`, `SM5701_*` | `drivers/battery_v2/*`, `drivers/battery/*`, `drivers/muic/*`, `drivers/mfd/{s2mu005_core.c,sm5701_core.c}`, `drivers/leds/leds_s2mu005.c` | `drivers/power/supply/s2mu005-battery.c` | charger, MUIC, OTG boost, charge limits, LED | wave1 |
| MT6328 PMIC/RTC | `mt6328_*`, `rtc_*` | `drivers/misc/mediatek/rtc/mt6328/mtk_rtc_hal.c`, MT6328 PMIC headers | `drivers/regulator/mt6328-regulator.c`, `drivers/rtc/rtc-mt6397.c` | missing rails, RTC trigger/alarm, vibrator rail | wave1 |
| MT6735 pinctrl | `mt6735_pinctrl_probe`, `mt_pctrl_init` | `drivers/pinctrl/mediatek/{pinctrl-mt6735.c,pinctrl-mtk-mt6735.h}` | same | missing pins/functions/EINT | wave1 |
| S6D78A0 panel / DSI | `mtkfb_*`, `disp_*`, `mtk_panel_*`, `bt541` panel strings | `drivers/misc/mediatek/lcm/s6d78a0_qhd_dsi_vdo/*`, `lcm_common.c`, `mtk_gen_panel/*` | `drivers/gpu/drm/panel/panel-samsung-s6d78a0-gh9607501a.c`, `drivers/gpu/drm/mediatek/*` | init sequence, delays, brightness, sleep, backlight | wave1 |
| Zinitix BT541 | `bt541_ts_*` | `drivers/input/touchscreen/bt541/bt541_ts.c` | `drivers/input/touchscreen/zinitix.c` | firmware update, keys/gesture, power/reset, suspend | wave1 |
| BMA254 accelerometer | `bma2x2_*` | `drivers/sensors/{bma2x2.c,bma2x2_reg.h}` | `drivers/iio/accel/bmc150-accel-*` | chip ID, ODR/range, calibration, mount matrix, IRQ | wave1 |

## Wave 2 — remaining Nura drivers

| Driver | ROM evidence | Downstream reference | Nura target | Planned work | Status |
|---|---|---|---|---|---|
| Mali-T720 / GPU power | `kbase_*`, `mt_gpufreq_*`, `ged_*` | `drivers/misc/mediatek/gpu/{ged,hal,mt6735/mali-r7p0}` | `drivers/gpu/drm/panfrost/*` | T720 power/OPP/DVFS/thermal hooks, cooling integration | wave2 |
| MT6328 vibrator / haptics | `mt6328_*`, vibrator symbols | `drivers/misc/mediatek/vibrator/vibrator_drv.c` | `drivers/input/misc/regulator-haptic.c`, `mt6328-regulator.c` | rail + timed-output parity | wave2 |
| MTK I2C | `mt_i2c_*` | `drivers/i2c/busses/i2c-mt65xx.c`, `i2c-mtk*` | `drivers/i2c/busses/i2c-mt65xx.c` | MT6735 quirks, bus recovery | audit/wave2 |
| MTK thermal | `mtkts_*`, `mt_cpufreq_thermal_*` | `drivers/misc/mediatek/thermal/*` | `drivers/thermal/mediatek/*`, `CONFIG_MTK_THERMAL` | thermal zones/trips/cooling | wave2 |
| MTK watchdog | `mtk_wdt_*` | `drivers/watchdog/mediatek/wdt/mt6735/*` | `drivers/watchdog/mediatek/*` | MT6735 reset/RGU behavior | audit/wave2 |
| MTK IOMMU/SMI | `mtk_m4u_*`, `m4u_*`, SMI | `drivers/misc/mediatek/m4u`, `smi` | `drivers/iommu/mtk_iommu*`, `drivers/memory/mtk-smi.c` | MT6735 port tables, larb, fault handling | wave2 |
| MMSYS/SCPSYS/PMIC wrap | `mt_mmsys_init`, `mt_scpsys_init`, `mt_pwrap_init` | `drivers/misc/mediatek/{mmsys,scpsys,pmic_wrap}` | `drivers/soc/mediatek/*`, `drivers/mfd/mt6397-core.c` | power domains, pmic wrap | wave2 |
| DSI PHY / display PWM | `mtk_panel_*`, `disp_*` | `drivers/misc/mediatek/video/mt6735`, `leds` | `drivers/phy/mediatek/phy-mtk-mipi-dsi.c`, `drivers/pwm/pwm-mtk-disp.c` | PHY tuning, backlight PWM | wave2 |
| UART / timer / APDMA | `mtk_uart_*`, `mtk_gpt_*` | `drivers/tty/serial/8250/8250_mtk.c`, `drivers/clocksource/*` | same | MT6735 quirks | audit/wave2 |
| CPUfreq / thermal / GPU DVFS | `mt_cpufreq_*`, `mt_gpufreq_*` | `drivers/misc/mediatek/base/power/*` | `drivers/cpufreq/mediatek-cpufreq.c`, `drivers/opp/*` | OPP tables, thermal limits | wave2 |

## Hardware blocks with no Nura driver

These are not "Nura drivers" today; they need new drivers, not parity patches. They remain blocked/deferred unless explicitly added to scope:

- WCN/CONSYS/WMT: Wi-Fi, BT, GPS, FM (`mtk_wcn_*`, `mt3326_gps`, `mtk_btif_*`).
- CCCI modem (`ccci_*`).
- Camera ISP/IMGSYS/SENINF + `imx219`/`s5k5e3` MTK glue.
- Audio `mt_soc` + `mt6331_codec`.
- Video codec `venc`/`vdec`.
- STK3013 proximity.
- MTK flashlight / S4DW9807AF.
- Samsung SM5504/SM5701 charger/MUIC (covered inside the S2MU005 wave-1 task as board variants).
- Mali `kbase` (Panfrost is the upstream replacement; no kbase port planned).

## Integration rules

- Each worktree produces `patches/<driver>.patch` and `logs/<driver>.md`.
- Shared DTS edits are collected as fragments and applied by the integration owner only.
- Full arm64 build is run in `worktrees/integration` after applying the series.
- Hardware validation is not available; reports must mark every unverified register/sequence.
