# IMGSYS SMI / DMA-clock / reset integration audit (MT6737T grandpplte -> Nura 7.0.10 arm64)

Worktree: `$M/worktrees/camera-gap-imgsys` (fresh from `src/nura-linux` HEAD `046883c0c`,
baseline `patches/camera-subsystem.patch` + `patches/camera-gap-uapi.patch` applied).
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Driver and files changed

| File | Change |
|---|---|
| `drivers/memory/mtk-smi.c` | new `mtk_smi_common_mt6735` gen data + `mtk_smi_common_mt6735_init` register table (L1LEN / M4U_TH / FIFO_TH1), wired to `mediatek,mt6735-smi-common`; evidence comments on the MT6735 LARB path. No change to the LARB MMU-enable writer (see §3.1). |
| `drivers/clk/mediatek/clk-mt6735-imgsys.c` | `CLK_IMG_SEN_CAM` gate parent corrected `mm_sel` -> `camtg_sel`. |

Deliberately not changed (verified correct): `drivers/pmdomain/mediatek/mt6735-pm-domains.h`,
`include/dt-bindings/clock/mediatek,mt6735-imgsys.h` (no gate missing), the MT8167/MT8173
LARB gens, any DTS/DTSI, the ISP char driver, seninf, sensors.

## 2. Evidence

Downstream source (`src/downstream-grandpplte`), ROM and Nura references used:

- `drivers/misc/mediatek/m4u/mt6735/mt6735/m4u_reg.h:314-321` — the MT6735 LARB register bank:
  `SMI_LARB_MMU_EN (0xfc0)`, `F_SMI_MMU_EN(port,en) = en << port`, `SMI_LARB_SEC_EN (0xfc4)`,
  `SMI_LARB_DOMN_0..3 (0xfd0..0xfdc)` with 4-bit domains.
- `drivers/misc/mediatek/m4u/mt6735/mt6735m/m4u_reg.h:311` — same `0xfc0` for the MT6735M variant.
- `drivers/misc/mediatek/m4u/mt6735/m4u_hw.c:1214-1231` — `m4u_config_port()` read-modify-writes
  `SMI_LARB_MMU_EN` with `F_SMI_MMU_EN(larb_port, !!(virt))` and `SMI_LARB_SEC_EN` with
  `F_SMI_SEC_EN(larb_port, !!(sec))`; `:1367,1395,1420,1658,1685` backup/restore the whole bitmap.
- `drivers/misc/mediatek/video/mt6735/dispsys/mt6735/ddp_reg.h:1866` —
  `DISP_REG_SMI_LARB_MMU_EN = DISPSYS_SMI_LARB0_BASE + 0xfc0`.
- `drivers/misc/mediatek/smi/smi_reg.h:298-339` — platform port counts: MT6735 (SMI_D1, selected
  by `arch/arm/configs/grandpplte_00_defconfig:295 CONFIG_ARCH_MT6735=y` and
  `smi/Makefile:28-31 -DSMI_D1`) has LARB0..3 and `SMI_LARB2_PORT_NUM 21` (`:308`).
- `drivers/misc/mediatek/smi/smi_reg.h:388-392` — `SMI_LARB_MMU_EN 0xf00`,
  `REG_SMI_LARB_DOMN_OF_PORT` at `0xf08/0xf0c` with 2-bit domains, and
  `SMI_LARB_SEC_EN` aliased to `F_SMI_SEC_EN`. **These are only definitions: no file under
  `drivers/misc/mediatek/smi/` or `drivers/misc/mediatek/m4u/` uses them.** The register layout
  they describe (2-bit domains at 0xf08/0xf0c) contradicts the MT6735 bank above and belongs to a
  different SMI generation.
- `drivers/misc/mediatek/smi/smi_configuration.c:111-117, 127-134` — MT6735 ("init" profile)
  common and LARB2 register tables; `smi_configuration.c:1273-1282` — D1 runtime offset/value
  patching and the LARB2 `0x24` bit-27 clear; `smi_common.c:1249-1255` — applied at boot after
  `save_default_common_val()`.
- ROM: `/run/media/.../zip/vmlinux.elf` `m4u_config_port` @ `0xc03814d8` writes `larb_base+0xfc0`
  (`ldr r0,[r2,#0xfc0]` @ `0xc03816a8`, `str r0,[r2,#0xfc0]` @ `0xc03816c8`) and `+0xfc4`
  (`0xc03816d0..0xc03816ec`) — identical to `m4u_reg.h`. The ROM `.rodata` at file offset
  `0xe808b0` contains the MT6735 common init table verbatim:
  `{0,0},{0,0x1000},{0,0x1000},{0,0x1000},{0x100,0x1b},{0x234,0xf7e19085},{0x230,0x0e9f}`.
- ROM DTB `zip/rom_dtb_0.dts:3678-3694, 3713-3716, 3781-3784, 3808-3811` — SMI/larb nodes; LARB2
  at `0x15001000` (window `0x1000`, so `0xfc0/0xfc4` are inside the mapped resource).
- `drivers/clk/mediatek/clk-mt6735.c:885-900` (downstream clock table) and `:1189-1211`
  (`mt_imgsys_init`, the `mt_imgsys_init` ROM symbol).
- `drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c:185-190, 3037-3060, 11543-11549`
  — the ISP clock list consumed by the camera driver.
- `drivers/misc/mediatek/base/power/mt6735/mt_spm_mtcmos.c:1137-1170, 1355-1391`
  — ISP/DIS/… power status masks, `SRAM_PDN`, `ISP_SRAM_ACK`, `DIS_SRAM_ACK`, `DISP_PROT_MASK`.
- Nura: `arch/arm64/boot/dts/mediatek/mt6735.dtsi:249-261` (m4u), `:806-825` (common, larb0),
  `:852-862` (larb2), `:885-901` (larb3); `include/dt-bindings/memory/mediatek,mt6735-m4u.h`;
  `drivers/iommu/mtk_iommu.c:607-625` (per-port bitmap), `:1581-1590` (`mt6735_data`, one bank),
  `:1186-1205` (`mediatek,larbs` / optional `mediatek,larb-id`), `:782` (attach calls config).

## 3. Parity matrix

### 3.1 LARB MMU-enable bitmap — the reported "real gap" does not exist

| Behaviour | Downstream / ROM | Nura before | After | Status |
|---|---|---|---|---|
| MT6735 LARB `SMI_LARB_MMU_EN` offset | `0xfc0` (`m4u_reg.h:314`, `ddp_reg.h:1866`, ROM `m4u_config_port` writes `+0xfc0`) | `mediatek,mt6735-smi-larb` -> `mtk_smi_larb_mt8167` writes `MT8167_SMI_LARB_MMU_EN = 0xfc0` (`mtk-smi.c:75,227,551`) | unchanged (verified correct); rationale documented in code | done |
| `smi/smi_reg.h:388` `0xf00` | defined but never used; its sibling `DOMN` layout (`0xf08/0xf0c`, 2-bit) contradicts the MT6735 bank (`0xfd0..0xfdc`, 4-bit) | not used | not used | done (rejected as evidence) |
| Bitmap layout | per-port `BIT(port)`, whole `u32` written (`F_SMI_MMU_EN(port,en)=en<<port`; `m4u_hw.c:1214-1218`) | `writel(*larb->mmu, base + 0xfc0)` — same | same | done |
| Bitmap source | consumer IOMMU: `larb_mmu->mmu \|= BIT(portid)` (`mtk_iommu.c:607-625`) | same | same | done |
| No IOMMU consumer | bitmap stays 0 => all ports physical (bypass) | same (kzalloc'd `larb_imu`, `mtk_iommu_config()` is only reached from `attach_dev`/`identity_attach`) | same, now explicitly documented | done |
| LARB2 ports | 21: IMGO0 RRZO1 AAO2 LCSO3 ESFKO4 IMGO_S5 LSCI6 LSCI_D7 BPCI8 BPCI_D9 UFDI10 IMGI11 IMG2O12 IMG3O13 VIPI14 VIP2I15 VIP3I16 LCEI17 RB18 RP19 WR20 (`smi_reg.h:308`; Nura `mediatek,mt6735-m4u.h` uses the same numbering) | same IDs, bitmap not limited | same | done |
| Per-port `NONSEC_CON` scheme | not used by MT6735; downstream uses the `0xfc0` whole-bitmap writer | mt8167 writer does not use it | unchanged | done |
| `SMI_LARB_SEC_EN 0xfc4` | written as `!!(sec)` per port; all ports non-secure in a normal boot, reset value 0 | not written | not written (would write 0 anyway) | done |
| DOMN `0xfd0..0xfdc` | downstream writes are commented out ("multimedia engines should set domain as 3") | not written | not written | done |
| LARB2 OSTDL `0x200+4*port` | downstream init profile programs `1` for all 21 ports (`smi_configuration.c:127-134`) | not programmed | not programmed — QoS tuning, not required for routing/bypass; value semantics undocumented | deferred |
| LARB2 `0x24` bit 27 | downstream clears it once at boot for D1 (`smi_configuration.c:1276`) | not programmed | not programmed — undocumented workaround bit | deferred |

Conclusion: the mapping of `mediatek,mt6735-smi-larb` to the mt8167 (0xfc0 bitmap) writer is the
behaviour the downstream M4U and the ROM implement. Writing `0xf00` instead (as the ticket
hypothesised) would **break** IOMMU address translation on this SoC. The only change made here is
documentation so the next reader does not "fix" it into `0xf00` again.

### 3.2 SMI common

| Behaviour | Downstream / ROM | Nura before | After | Status |
|---|---|---|---|---|
| Common init values after (re)power | `L1LEN 0x100 = 0x1b`, `M4U_TH 0x234 = 0xf7e19085`, `FIFO_TH1 0x230 = 0x0e9f` (`smi_configuration.c:111-117`, patched at `:1273-1282`; table verbatim in ROM `.rodata` `0xe808b0`) | `mtk_smi_common_gen2`: no init table; only `SMI_BUS_SEL = 0` on resume | `mtk_smi_common_mt6735` with the three values | done |
| Gen2 features MT6735 lacks | no gals clocks (downstream `smi_common` DT clock list is smi-common/larb/img/vdec/venc, `rom_dtb_0.dts:3687`), no `sw_flag`/`thrt` (those are LARB `0x40`/`0x24` for later SoCs and are not applied because MT6735 uses the plain bitmap writer) | `has_gals = false`, 2 clocks required — matches the Nura DT (`apb`, `smi`) | unchanged | done |
| `SMI_BUS_SEL 0x220` | never written downstream; MT6735 m4u has a single bank (`mtk_iommu.c:1581-1590`) so "all to MMU0" is the only consistent value | written `0` on resume | unchanged | done (`[INFERENCE]` on the 0x220 name for MT6735) |
| Per-LARB L1 arbitration `0x104-0x114` | `0x104` restored from the boot value, `0x108/0x10c` = `0x1000`; the D1 loop runs `i < SMI_LARB_NR (4)` over a 3-entry `smi_common_l1arb_offset` array, i.e. one out-of-bounds write | not programmed | not programmed — undocumented field layout and a downstream OOB access; values are arbitration hints, not required for functional DMA | deferred |

### 3.3 IMGSYS clocks

| Behaviour | Downstream | Nura before | After | Status |
|---|---|---|---|---|
| Gate set / bits | `img_clks[]`: LARB2 b0, CAM_SMI b5, CAM_CAM b6, SEN_TG b7, SEN_CAM b8, CAM_SV b9, SUFOD b10, FD b11, set 0x4 / clr 0x8 / sta 0x0 (`clk-mt6735.c:885-899`) | identical set/bits, `mtk_clk_gate_ops_setclr` | unchanged | done |
| `SEN_CAM` parent | `camtg_sel` (`clk-mt6735.c:896`) | `mm_sel` | `camtg_sel` | done (fixed) |
| Other parents | `mm_sel` | `mm_sel` | unchanged | done |
| Missing gate | none — downstream has exactly these 8 gates (`IMG_NR_CLK`) | 8 gates in the DT binding | unchanged; no binding change needed | done |
| ISP clock list | `camera_isp_D1.c:11543-11549` gets `CG_IMAGE_CAM_SMI`, `CG_IMAGE_SEN_TG`, `CG_IMAGE_LARB2_SMI` (+ CAM_CAM/SEN_CAM/CAM_SV) from the ispsys node | ispsys fragment supplies all six | no change to bindings | done |

`camtg_sel` is registered by `clk-mt6735-topckgen.c:342` (`CLK_TOP_CAMTG_SEL`), so the gate parent
resolves through CCF exactly like the `mm_sel` gates; sensor MCLK users already depend on
`camtg`.

### 3.4 Power domains / reset

| Behaviour | Downstream | Nura before | After | Status |
|---|---|---|---|---|
| ISP domain status / SRAM | `ISP_PWR_STA_MASK 1<<5`, `SRAM_PDN 0xf<<8`, `ISP_SRAM_ACK 0x3<<12` (`mt_spm_mtcmos.c:1137,1152,1159`) | `sta_mask = PWR_STATUS_ISP (BIT(5))`, `sram_pdn_bits = GENMASK(11,8)`, `sram_pdn_ack_bits = GENMASK(13,12)` | unchanged | done (matches) |
| ISP bus protection | none programmed in `spm_mtcmos_ctrl_isp()` (`:1355-1391`) | no `bp_cfg` on the ISP domain | unchanged | done (matches) |
| DIS bus protection | `DISP_PROT_MASK = 1<<1` of TOPAXI (`:1168,1424-1425`) | `BUS_PROT_INFRA_UPDATE_TOPAXI(MT8173_TOP_AXI_PROT_EN_MM_M0)` with `BIT(1)` (`include/linux/soc/mediatek/infracfg.h:400`) | unchanged | done (matches) |
| ISP <-> DIS dependency | none: ISP is an independent MTCMOS domain (no parent programming in `spm_mtcmos_ctrl_isp`) | ISP has no parent, `smi_common` is in DIS, `smi_larb2` in ISP (Nura DT) | unchanged | done (verified correct) |
| LARB2 power on/off register backup | downstream backs up/restores LARB registers on MTCMOS off/on (`smi_common.c:858-879`, `m4u_hw.c:1658,1685`) | mainline re-programs `*larb->mmu` on resume, which is the authoritative state; other registers are left to HW reset | unchanged | done (functionally equivalent for the MMU bitmap) |
| Pass-2/CAMSV resets | outside the windows owned by this leaf / ISP leaf | handled by the ISP driver leaf | n/a | deferred |

### 3.5 DMA / IOMMU wiring

| Behaviour | Result |
|---|---|
| Camera M4U port IDs | LARB2 ports 0..20 as listed in §3.1; all present in `include/dt-bindings/memory/mediatek,mt6735-m4u.h` (IMGO/RRZO/AAO/LCSO/ESFKO/IMGO_S/LSCI/LSCI_D/BPCI/BPCI_D/UFDI/IMGI/IMG2O/IMG3O/VIPI/VIP2I/VIP3I/LCEI/RB/RP/WR). |
| `mediatek,larbs` on the m4u node | already correct in Nura DT; lists larb0..3 in order, so `mediatek,larb-id` is optional and ids resolve to 0..3 (`mtk_iommu.c:1200-1203`). |
| `mediatek,smi` on each larb | already correct for larb2 (`mtk6735.dtsi:862`); the smi driver links larb -> common at probe. |
| `iommus` on the ISP node | **must stay absent** for the delivered char-driver model: the ABI (`include/uapi/linux/mtk_isp.h`) carries physical buffer addresses as downstream does. Adding `iommus` would make the device an IOMMU consumer, set the LARB2 port bits, and translate those physical addresses as IOVAs -> faults. |
| IOMMU bypass claim | verified in code: `larb_mmu->mmu` is only OR-ed in `mtk_iommu_config()` from `attach_dev`/`identity_attach` (`mtk_iommu.c:607-625,722-792`). No camera consumer => LARB2 bitmap stays 0 => `mtk_smi_larb_config_port_mt8167()` writes 0 to `0xfc0` => physical addressing. |

## 4. Compile verification

Commands (exact):

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/camera-gap-imgsys"
cp "$M/nura.config" .config
./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT -e MEDIA_CAMERA_SUPPORT \
  -e MEDIA_PLATFORM_SUPPORT -e MEDIA_CONTROLLER -e VIDEO_DEV -e VIDEO_V4L2_SUBDEV_API \
  -e V4L2_FWNODE -e V4L2_ASYNC -e VIDEO_CAMERA_SENSOR -e I2C -e REGULATOR -e COMMON_CLK \
  -e NVMEM -e DEBUG_FS -e MTK_SMI -e MTK_IOMMU -e COMMON_CLK_MT6735_IMGSYS
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 drivers/memory/mtk-smi.o \
                              drivers/clk/mediatek/clk-mt6735-imgsys.o
make ARCH=arm64 LLVM=1 W=1 -j2 drivers/memory/mtk-smi.o \
                                  drivers/clk/mediatek/clk-mt6735-imgsys.o \
                                  drivers/iommu/mtk_iommu.o
```

Result (`logs/camera-gap-imgsys-build.log`), config symbols confirmed as
`CONFIG_MTK_SMI=y`, `CONFIG_MTK_IOMMU=y`, `CONFIG_COMMON_CLK_MT6735_IMGSYS=y`:

```
=== pass 1: ARCH=arm64 LLVM=1 ===
  CC      drivers/memory/mtk-smi.o
  CC      drivers/clk/mediatek/clk-mt6735-imgsys.o
exit=0
=== pass 2: W=1 ===
  CC      drivers/iommu/mtk_iommu.o
  CC      drivers/clk/mediatek/clk-mt6735-imgsys.o
  CC      drivers/memory/mtk-smi.o
exit=0
```

0 warnings, 0 errors in both passes (`mtk_iommu.o` was built as a consumer sanity check; it is
unchanged and not part of the patch). No full-kernel build was run.

No userspace component: the optional `tools/mediatek/camera/imgsys-check/` was not created. The
only observable SMI state is the register file, which mainline `mtk-smi` does not export to
debugfs; a `/dev/mem` based reader would be blocked by `CONFIG_STRICT_DEVMEM` on this arm64
config and would be an unverifiable placeholder, so it is deliberately omitted rather than
shipped untested.

## 5. Inferred / unverified (no hardware)

- `SMI_BUS_SEL = 0` at common `+0x220`: the offset/name comes from the shared gen2 layout
  (mainline `mtk-smi.c:27`); MT6735 downstream only lists `0x220` in its register-dump table and
  never writes it. With a single M4U bank the value is necessarily MMU0, so the existing write is
  kept. `[INFERENCE]`
- The exact power-on reset value of the MT6735 SMI common QoS registers is unknown; the added
  init table is justified by the downstream/ROM table, not by observing a difference.
- LARB2 OSTDL and the `0x24` bit-27 clear are recorded but not ported (see §3.1); their field
  semantics are not documented anywhere in the downstream tree.
- Whether ISP MTCMOS power-off actually clears the SMI common (DIS domain) registers on this SoC
  is a hardware observation; the init table makes the driver correct in either case.
- LARB IRQs (`rom_dtb_0.dts:3713-3716`, SPI 0xb2) are not implemented by mainline `mtk-smi`;
  no error-reporting consumer exists in the camera path, so nothing was added.

Nothing in `include/uapi/linux/mtk_isp.h` was needed for this work; no missing field was
identified for the SMI/DMA side of the ABI.

## 6. DTS fragment

No DTS/DTSI file was modified. The existing SoC nodes already match the driver after this change
(shown for verification; these are present in `arch/arm64/boot/dts/mediatek/mt6735.dtsi`):

```dts
/* mt6735.dtsi:249 — IOMMU with all four larbs in probe order */
m4u: iommu@10205000 {
	compatible = "mediatek,mt6735-m4u";
	reg = <0 0x10205000 0 0x1000>;
	interrupts = <GIC_SPI 146 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&infracfg CLK_INFRA_M4U>;
	clock-names = "bclk";
	mediatek,larbs = <&smi_larb0>, <&smi_larb1>, <&smi_larb2>, <&smi_larb3>;
	#iommu-cells = <1>;
};

/* mt6735.dtsi:819 — SMI common, two clocks (no gals), DIS domain */
smi_common: memory-controller@14016000 {
	compatible = "mediatek,mt6735-smi-common";
	reg = <0 0x14016000 0 0x1000>;
	clocks = <&clk26m>, <&mmsys CLK_MM_SMI_COMMON>;
	clock-names = "apb", "smi";
	power-domains = <&spm MT6735_POWER_DOMAIN_DIS>;
};

/* mt6735.dtsi:852 — camera LARB, ISP domain, MMU bitmap at 0xfc0 */
smi_larb2: memory-controller@15001000 {
	compatible = "mediatek,mt6735-smi-larb";
	reg = <0 0x15001000 0 0x1000>;
	clocks = <&clk26m>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "apb", "smi";
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	mediatek,smi = <&smi_common>;
};
```

Camera ISP node (from the coordinator fragment): keep it **without** `iommus` for the char-driver
(physical-DMA) model:

```dts
ispsys: isp@15004000 {
	compatible = "mediatek,mt6735-ispsys";
	reg = <0 0x15004000 0 0x4000>, <0 0x1500d000 0 0x1000>;
	interrupts = <GIC_SPI 183 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 184 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 185 IRQ_TYPE_LEVEL_LOW>, <GIC_SPI 206 IRQ_TYPE_LEVEL_LOW>,
		     <GIC_SPI 207 IRQ_TYPE_LEVEL_LOW>;
	clocks = <&imgsys CLK_IMG_CAM_SMI>, <&imgsys CLK_IMG_CAM_CAM>,
		 <&imgsys CLK_IMG_SEN_TG>, <&imgsys CLK_IMG_SEN_CAM>,
		 <&imgsys CLK_IMG_CAM_SV>, <&imgsys CLK_IMG_SMI_LARB2>;
	clock-names = "CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
		      "CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI";
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	/* no iommus: the ISP ABI passes physical buffer addresses (IOMMU bypass) */
	status = "disabled";	/* board DTS enables it */
};
```

If a future V4L2 capture path maps buffers through the IOMMU, the *only* change needed is to add
the consumed ports to the device node (mt6735.dtsi already includes
`<dt-bindings/memory/mediatek,mt6735-m4u.h>` at line 19):

```dts
	iommus = <&m4u M4U_PORT_IMGO>, <&m4u M4U_PORT_RRZO>,
		 <&m4u M4U_PORT_AAO>,  <&m4u M4U_PORT_LCSO>,
		 <&m4u M4U_PORT_ESFKO>, <&m4u M4U_PORT_IMGO_S>,
		 <&m4u M4U_PORT_LSCI>, <&m4u M4U_PORT_LSCI_D>,
		 <&m4u M4U_PORT_BPCI>, <&m4u M4U_PORT_BPCI_D>,
		 <&m4u M4U_PORT_UFDI>, <&m4u M4U_PORT_IMGI>,
		 <&m4u M4U_PORT_IMG2O>, <&m4u M4U_PORT_IMG3O>,
		 <&m4u M4U_PORT_VIPI>, <&m4u M4U_PORT_VIP2I>,
		 <&m4u M4U_PORT_VIP3I>, <&m4u M4U_PORT_LCEI>,
		 <&m4u M4U_PORT_RB>,   <&m4u M4U_PORT_RP>,
		 <&m4u M4U_PORT_WR>;
```

That must land together with `dma_map_*`-based buffer handling in the driver; on its own it
enables translation and breaks physical-address descriptors. `mediatek,larb` is a downstream
4.4 property and does not exist in this kernel — the larb association is `mediatek,larbs` on the
m4u node plus `mediatek,smi` on each larb (both already present).

## 7. Patch

`$M/patches/camera-gap-imgsys.patch` (102 lines, 2 files, +49/-3). Verified with
`git apply --check` against a clean `$M/src/nura-linux` HEAD; contains only the two owned files.
