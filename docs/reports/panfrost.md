# Panfrost / Mali-T720 parity audit

Status: audit-only, no patch in this run.

## Evidence checked
- Nura: `drivers/gpu/drm/panfrost/*`, `CONFIG_DRM_PANFROST=m`, GPU node in
  `arch/arm64/boot/dts/mediatek/mt6735.dtsi` (`mediatek,mt6735-mali`,
  `arm,mali-t720`, MFG power domain, `mt6328_vcore1_reg`).
- ROM: `kbase_*`, `mt_gpufreq_*`, `ged_*`; downstream
  `drivers/misc/mediatek/gpu/{ged,hal,mt6735/mali-r7p0}`.

## Finding
Panfrost already binds the T720 node and owns clock, reset, power domain and
regulator. The remaining downstream behavior is MTK `ged`/`mt_gpufreq` DVFS and
thermal policy, which has no direct Panfrost equivalent and is board policy, not
a missing register sequence. Adding an unverified OPP table or thermal governor
without hardware would be speculative.

## Deferred work
- Add a reviewed GPU OPP table from `mt_gpufreq.c` (table selection depends on
  binning/fuse values).
- Wire GPU cooling to the MTK thermal zones.
- Validate idle/suspend and regulator sequencing on hardware.

No patch is delivered because no defensible code gap was established.
