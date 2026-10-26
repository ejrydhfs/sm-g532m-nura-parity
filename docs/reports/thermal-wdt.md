# MT6735 thermal / watchdog parity audit

Status: audit-only, no patch in this run.

## Evidence checked
- Nura: `drivers/thermal/mediatek/auxadc_thermal.c`, `drivers/watchdog/mtk_wdt.c`,
  `CONFIG_MTK_THERMAL=y`, `CONFIG_MEDIATEK_WATCHDOG=y`.
- Downstream: `drivers/misc/mediatek/thermal/*` (zones/coolers),
  `drivers/watchdog/mediatek/wdt/mt6735/*`.
- ROM symbols: `mtkts_*`, `mtk_cpufreq_thermal_*`, `mtk_wdt_*`.

## Finding
The upstream MTK watchdog and auxadc thermal drivers already implement the
MT6735 register blocks. Downstream adds vendor cooling governors and board
trip tables, which are policy data. No register-level divergence was established
without hardware or a full downstream table comparison.

## Deferred work
- Port the board thermal trip/cooling table into `thermal-zones`.
- Validate watchdog reset/RGU and suspend behavior on hardware.

No patch is delivered because no concrete gap was proven.
