# MT6737 IOMMU / SMI parity audit

Status: audit-only, no patch in this run.

## Evidence checked
- Nura: `drivers/iommu/mtk_iommu.c`, `drivers/memory/mtk-smi.c`,
  `CONFIG_MTK_IOMMU=y`, `CONFIG_MTK_SMI=y`.
- Downstream: `drivers/misc/mediatek/m4u/{2.0,mt6735}` and
  `drivers/misc/mediatek/smi/*`.
- ROM symbols: `mtk_m4u_*`, `m4u_*`, SMI/larb symbols.

## Finding
The upstream MTK IOMMU/SMI drivers already describe the MT6735/MT6737 LARB and
port model. The remaining downstream code is the older M4U API and multimedia
power/DVFS policy, not a missing upstream register block. Changing LARB tables
without the board's multimedia mappings would be unsafe.

## Deferred work
- Compare every downstream LARB/port entry against the upstream `mt6735.dtsi`
  `iommus`/`mediatek,larb` phandles.
- Validate display/camera/VDEC translation and fault recovery on hardware.

No patch is delivered because no concrete gap was proven.
