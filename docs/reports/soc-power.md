# MT6737 SoC power / MMSYS / PMIC-wrap / DSI-PWM parity audit

Status: audit-only, no patch in this run.

## Evidence checked
- Nura: `drivers/soc/mediatek/*`, `drivers/mfd/mt6397-core.c`,
  `drivers/phy/mediatek/phy-mtk-mipi-dsi.c`, `drivers/pwm/pwm-mtk-disp.c`,
  `CONFIG_MTK_MMSYS/SCPSYS/PMIC_WRAP/MFG_PM_DOMAIN`.
- Downstream: `drivers/misc/mediatek/base/power`, `pmic_wrap`, `mmsys`,
  `scpsys`, `video/mt6735`, `leds`.
- ROM symbols: `mt_mmsys_init`, `mt_scpsys_init`, `mt_pwrap_init`,
  `disp_*`, `mtk_panel_*`.

## Finding
The upstream MTK clock/power/PMIC-wrap/DSI/PWM drivers already cover the MT6735
blocks, and the Nura DTS enables the display and GPU power domains. Downstream
adds vendor DVFS and display policy; no specific missing register sequence was
established in this audit.

## Deferred work
- Validate SCPSYS/MFG domain transitions and MMSYS clock/reset ordering.
- Compare DSI PHY tuning and PWM backlight values against the ROM DTB on hardware.

No patch is delivered because no concrete gap was proven.
