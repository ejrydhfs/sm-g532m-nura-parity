# CCCI gap closure: MD2 power domain + single carve-out split

Target worktree: `$M/worktrees/ccci-gap-clock` (HEAD `a3e42ae96 ccci: gap scaffold`)
Patch: `$M/patches/ccci-gap-clock.patch`
Report: this file.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Result

Both gaps named in the ticket are closed in the kernel:

1. **MD2 (C2K) power domain** — `MT6735_POWER_DOMAIN_MD2` now exists in the MT6735
   scpsys driver, backed by `SPM_C2K_PWR_CON` (0x2d4), status `PWR_STATUS_MD2`
   (bit 22), SRAM power-down bit 8 and TOPAXI bus-protect bits 29..31. The C2K
   driver attaches the domain through the platform bus (`power-domains` in DT) and
   holds it on with runtime PM across `ccci_c2k_hw_power_on()`/`_power_off()`,
   exactly like the MD1 core holds `MT6735_POWER_DOMAIN_MD1`
   (`drivers/misc/mediatek/ccci/ccci_platform.c:208`,
   `drivers/misc/mediatek/ccci/ccci_boot.c:520`). The optional
   `scp-sys-md2-main` clock property is still honoured when a clock provider exists.

2. **Single modem carve-out split** — `ccci_boot_smem_setup()` now accepts one
   reserved-memory region plus `mediatek,md_smem_size` and splits it into the MD
   image half and the AP↔MD shared-memory half the way downstream does
   (`ccci_util_lib_fo.c:1113-1122`): `smem_size = md_smem_size`,
   `image_size = round_up(total - md_smem_size, 0x4000)`,
   `smem_phys = region_base + image_size`. The previous two-region DT shape
   (`memory-region[0]` image, `[1]` smem) keeps working unchanged.

No DTS file was touched. No declaration of `include/linux/ccci.h` was changed; no
new cross-leaf interface was needed.

---

## 2. Files changed

| File | Change |
|---|---|
| `drivers/pmdomain/mediatek/mtk-pm-domains.h` | `SPM_C2K_PWR_CON` (0x02d4), `PWR_STATUS_MD2` (BIT(22)) |
| `drivers/pmdomain/mediatek/mt6735-pm-domains.h` | `scpsys_domain_data_mt6735[MT6735_POWER_DOMAIN_MD2]`, name `"md2"` |
| `include/dt-bindings/power/mediatek,mt6735-power-controller.h` | `MT6735_POWER_DOMAIN_MD2` = 7 (appended, existing ids unchanged) |
| `include/linux/soc/mediatek/infracfg.h` | `MT6735_TOP_AXI_PROT_EN_MD2` = BIT(29)|BIT(30)|BIT(31) |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.c` | `devm_pm_runtime_enable()` in `ccci_c2k_hw_init()`; `pm_runtime_resume_and_get()`/`pm_runtime_put_sync()` in power on/off; warn text updated |
| `drivers/misc/mediatek/ccci/ccci_boot.c` | single `memory-region[0]` + `mediatek,md_smem_size` split logic |

`drivers/misc/mediatek/ccci_c2k/ccci_c2k.h` needed no change (runtime PM state lives
in the device, not in `struct ccci_c2k`).

`git status --porcelain` in the worktree lists exactly the six files above.

---

## 3. ROM evidence (SM-G532M `$M/zip/`)

### 3.1 Symbols (`$M/zip/symbols.txt`, addresses from `llvm-nm vmlinux.elf`)

| Line | Symbol | Address | Meaning |
|---|---|---|---|
| 15505 | `spm_mtcmos_ctrl_mdsys2` | `0xc0353124` | MD2/C2K SPM power sequence |
| 15504 | `spm_mtcmos_ctrl_mdsys1` | `0xc0352df0` | MD1 equivalent |
| 16019 | `spm_is_md2_sleep` | `0xc036bd84` | MD2 sleep status reader |
| 15369/15370 | `md2_sys_disable_op` / `md2_sys_enable_op` | `0xc034ba98` / `0xc034bab0` | C2K sysreset ops |
| 21000 | `get_md1_md3_resv_smem_info` | `0xc04ff308` | MD1↔MD3 share-memory lookup |
| 20999 | `get_md_resv_mem_info` | — | image + smem layout lookup |
| 20992 | `ccci_reserve_mem_of_init` | — | reserved-memory carve-out parser |

### 3.2 `spm_mtcmos_ctrl_mdsys2` disassembly (`llvm-objdump`, 0xc0353124-0xc0353300)

| Address | Instruction | Register/bit |
|---|---|---|
| `0xc035315c` | `ldr r2,[r3,#0x220]` / `orr r2,r2,#-536870912` / `str` | TOPAXI_PROT_EN, mask 0xE0000000 = bits 29..31 |
| `0xc035318c` | `ldr r0,[r3,#0x228]` | TOPAXI_PROT_STA1 poll |
| `0xc03531a0` | `ldr r2,[r3,#0x2d4]` / `orr #256` | SPM_C2K_PWR_CON, SRAM_PDN bit 8 |
| `0xc03531d4` | `orr r2,r2,#2` | PWR_ISO bit 1 |
| `0xc03531fc` | `bic r2,r2,#17` / `orr r2,r2,#16` | clear PWR_RST_B bit 0, set PWR_CLK_DIS bit 4 |
| `0xc0353228` | `bic r2,r2,#12` | clear PWR_ON bit 2 / PWR_ON_2ND bit 3 |
| `0xc035323c`-`0xc0353250` | `ldr [#0x60c]`, `tst #4194304`; `ldr [#0x610]`, `tst #4194304` | PWR_STATUS / PWR_STATUS_2ND, mask BIT(22) |
| `0xc0353270`/`0xc0353298` | `orr #4` / `orr #8` | PWR_ON / PWR_ON_2ND |
| `0xc03532d8` | `bic r2,r2,#16` | clear PWR_CLK_DIS |

The ROM constants match downstream source one to one, so the mainline domain data
uses source values, not guessed ones.

### 3.3 Strings (`$M/zip/vmlinux.strings`)

| Line | String |
|---|---|
| 37107 | `mediatek,md_smem_size` |
| 37118 | `mediatek,md1md3-smem-size` |
| 37044 | `[ccci0/util]ccci_md_mem_reserve phased out.` |

`mediatek,md_smem_size` being a live ROM string proves the DT property is parsed by
the shipped kernel, i.e. the carve-out split is real ROM behaviour, not dead
downstream code.

---

## 4. Downstream references

| Reference | Content |
|---|---|
| `drivers/misc/mediatek/base/power/mt6735/mt_spm.h:127` | `#define SPM_C2K_PWR_CON (SPM_BASE + 0x2d4) /* mt6735 */` |
| `drivers/misc/mediatek/base/power/mt6735/mt_spm_mtcmos.c:1133` | `#define MD2_PWR_STA_MASK (0x1 << 22)` |
| `mt_spm_mtcmos.c:1142` | `#define MD1_PWR_STA_MASK (0x1 << 0)` (existing mainline `PWR_STATUS_MD1`) |
| `mt_spm_mtcmos.c:1154` | `#define MD_SRAM_PDN (0x1 << 8) /* MD1, C2K */` |
| `mt_spm_mtcmos.c:1166` | `#define MD2_PROT_MASK ((0x1<<29)|(0x1<<30)|(0x1<<31))` |
| `mt_spm_mtcmos.c:1767-1830` | `spm_mtcmos_ctrl_mdsys2()` — full MD2 power up/down sequence |
| `arch/arm64/boot/dts/mt6735.dtsi:1556-1564` | `mdc2k@3a00b01c`, `clocks = <&scpsys SCP_SYS_MD2>`, `clock-names = "scp-sys-md2-main"` |
| `include/dt-bindings/clock/mt6735-clk.h:231` | `#define SCP_SYS_MD2 2` |
| `drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.c:179-182` | MD3 looks up `"scp-sys-md2-main"` and fails hard if missing (`CONFIG_MTK_LEGACY` off) |
| `arch/arm64/boot/dts/mt6735.dtsi:399` | `mediatek,md_smem_size = <0x30000>;` on the MD1 node |
| `drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:948-951` | `get_md_smem_align()` returns 0x4000 |
| `ccci_util_lib_fo.c:1093-1105` | per-MD compatible (`mediatek,mdcldma` / `ap_ccif1` / `ap2c2k_ccif`) then `of_property_read_u32(node, "mediatek,md_smem_size", …)` |
| `ccci_util_lib_fo.c:1113-1122` | `img_size = round_up(total - smem, 0x4000)`, `smem = base + img_size` |
| `ccci_util_lib_fo.c:1231-1258` | `ccci_reserve_mem_of_init()` records the single reserved region as the modem total |

Note on the substitution: downstream controls `SCP_SYS_MD2` as a **clock gate**
(clk-scpsys) and separately runs `spm_mtcmos_ctrl_mdsys2()` for the SPM power
domain. Mainline has no mt6735 SCP_SYS clock provider, and the Nura tree already
models the sibling gate `SCP_SYS_MD1` as `MT6735_POWER_DOMAIN_MD1`; this patch
follows that established model for MD2. The optional clock lookup is left in place,
so if a clock provider is ever added the exact downstream sequence still works.

---

## 5. Parity matrix

| Behaviour | ROM / downstream | Before (Nura) | After | Status |
|---|---|---|---|---|
| MD2 (C2K) power gate exists | `SCP_SYS_MD2` clock (mt6735.dtsi:1563) + `spm_mtcmos_ctrl_mdsys2()` | no MD2 clock and no MD2 power domain anywhere in the tree | `MT6735_POWER_DOMAIN_MD2` genpd, name `md2`, DT id 7 | done (needs DT fragment) |
| MD2 power-control register | `SPM_C2K_PWR_CON` = 0x2d4 (`mt_spm.h:127`, ROM `str [r3,#0x2d4]`) | absent | `SPM_C2K_PWR_CON` 0x02d4 | done |
| MD2 power status | `MD2_PWR_STA_MASK` BIT(22) (`mt_spm_mtcmos.c:1133`, ROM `tst #0x400000` on 0x60c/0x610) | `PWR_STATUS_MFG_2D` BIT(22) only (unused on MT6735) | `PWR_STATUS_MD2` BIT(22) alias, used as `sta_mask` | done |
| MD2 SRAM power-down | `MD_SRAM_PDN` BIT(8) (`mt_spm_mtcmos.c:1154`) | absent | `sram_pdn_bits = GENMASK(8,8)`, `sram_pdn_ack_bits = 0` (no ack bit downstream either) | done |
| MD2 AXI bus protection | `MD2_PROT_MASK` bits 29..31 on TOPAXI_PROT_EN/STA1 (`mt_spm_mtcmos.c:1166,1779-1781`) | absent | `MT6735_TOP_AXI_PROT_EN_MD2` + `BUS_PROT_INFRA_UPDATE_TOPAXI()` | done |
| MD2 power sequence order | PROT_EN → SRAM_PDN → ISO → CLK_DIS/RST_B↓ → ON/ON_2ND↓ → poll status; reverse on power up | absent | handled by the generic `scpsys_power_on/off()` direct-control path (same path that serves MD1) | done |
| MD3 holds the gate while running | `clk_prepare_enable(clk_scp_sys_md2_main)` (`ccif_c2k_platform.c:376-389`) | optional clock only; no domain | optional clock + `pm_runtime_resume_and_get()` in `ccci_c2k_hw_power_on()`, `pm_runtime_put_sync()` in `_power_off()`; `devm_pm_runtime_enable()` at init | done |
| Optional `scp-sys-md2-main` clock | required downstream | `devm_clk_get_optional()`, warn | same, warn text now "relying on power domain" | done |
| Modem memory: single carve-out | one reserved region + `mediatek,md_smem_size` split at 16 KiB (`ccci_util_lib_fo.c:1113-1122`) | only `memory-region[0]` (image) + `[1]` (smem); with one region `[1]` was missing and smem fell back to `dma_alloc_coherent` | split of `memory-region[0]` when `[1]` is absent and `mediatek,md_smem_size` is present, with bounds validation | done |
| Modem memory: two-region DT | not used downstream | supported | still supported, unchanged | done |
| `mediatek,md_smem_size` without region | used as smem size | sized the coherent fallback allocation | same | done |
| Coherent fallback smem | n/a | `dma_alloc_coherent()` sized for exception+runtime+rings | unchanged when neither region nor property exists | done |

Deferred / not applicable to this leaf: nothing in the two named gaps. The SCP clock
gate itself has no mainline clock provider (see §7); MD2 stays powered for the
lifetime of the C2K device, and the domain is powered down only on driver detach,
which matches the always-on nature of the modem domain.

---

## 6. Compile

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-gap-clock"
make ARCH=arm64 LLVM=1 -j2 \
    drivers/pmdomain/mediatek/mtk-pm-domains.o \
    drivers/misc/mediatek/ccci/ccci_boot.o \
    drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.o
```

Result: **exit 0**, all three objects produced (`CC drivers/…`), no warnings.

Forced rebuild with `W=1` after deleting the three objects: exit 0, no warnings
from the changed code. The only `W=1` output is two pre-existing kernel-doc
warnings for `ccci_boot_prepare_runtime_data()` (`ccci_boot.c:680`), a function
this patch does not touch (it is absent from the diff).

`.config`: the existing `CONFIG_MTK_CCCI=y`, `CONFIG_MTK_CCCI_C2K=y`,
`CONFIG_MTK_SCPSYS_PM_DOMAINS=y` configuration; `olddefconfig` was not re-run
because no Kconfig symbol changed.

Objects verifying the header-only changes are compiled into
`drivers/pmdomain/mediatek/mtk-pm-domains.o` (includes
`mt6735-pm-domains.h` → `mtk-pm-domains.h` → `infracfg.h`) and the two CCCI
objects; the DT binding header is consumed by DTS builds, not by C.

---

## 7. Inferred / unverified on hardware

* The `SCP_SYS_MD2` gate in the vendor kernel is a *clock* (`clk-scpsys`); mainline
  models the same gate family as PM domains. Mapping it to
  `MT6735_POWER_DOMAIN_MD2` follows the tree's existing MD1 model and the ticket
  contract, but the vendor's exact `SCP_SYS` clock-register programming is **not**
  reproduced (there is no such provider in this tree). [INFERENCE]
* `PWR_STATUS_MD2` and `PWR_STATUS_MFG_2D` share bit 22 in the shared header. MT6735
  defines no MFG_2D domain, so no collision occurs, but the alias is only valid on
  SoCs whose SPM layout matches the downstream MT6735 map. [INFERENCE from
  `mt_spm_mtcmos.c:1133` vs. the generic header]
* The split reserves the *rounded-up* image size, so an unaligned carve-out size
  would leave the tail of the region unused; the code rejects the split
  (`-EINVAL`) rather than mapping outside the reserved region. Downstream silently
  rounds; no DT in this tree uses an unaligned size.
* No hardware is attached: no MD2 power-up, no register readback, no modem boot
  was exercised. Everything above is source/ROM-derived.
* `pm_runtime_resume_and_get()` on a device without `power-domains` succeeds and
  changes nothing, so the driver still works on a DT without the MD2 domain (the
  old loader-powered path).

---

## 8. DTS-FRAGMENT

Not applied. Required for the MD2 power-domain part; the carve-out split only needs
a DT change if the board wants the downstream single-region layout.

### 8.1 MD2 power domain (new child of the existing `spm` node)

```dts
&spm {
	power-domain@MT6735_POWER_DOMAIN_MD2 {
		reg = <MT6735_POWER_DOMAIN_MD2>;
		#power-domain-cells = <0>;
		mediatek,infracfg = <&infracfg>;
	};
};
```

The `spm` label exists in `arch/arm64/boot/dts/mediatek/mt6735.dtsi:186`; the
`mediatek,infracfg` phandle is what `scpsys_add_one_domain()` needs to drive the
TOPAXI protection bits (same property as `MT6735_POWER_DOMAIN_MD1`).

### 8.2 C2K node

Add to the MD3 node from `$M/logs/ccci-c2k.md` §9:

```dts
			power-domains = <&spm MT6735_POWER_DOMAIN_MD2>;
```

The commented-out `clocks = <&scpsys SCP_SYS_MD2>;` lines in that fragment stay
commented: there is no `SCP_SYS_MD2` clock provider in the tree, and the driver
treats the clock as optional.

### 8.3 Single modem carve-out (MD1, downstream shape)

Either keep the existing two-region shape (`memory-region = <&md1_image>, <&md1_smem>;`)
or use the downstream single-region shape:

```dts
/ {
	reserved-memory {
		md1_carveout: md1-carveout@0 {
			reg = <0 0x0 0 0x0>;	/* FILL: hw memory map.
						 * Total size = modem image +
						 * 0x30000 smem, 16 KiB aligned.
						 */
			no-map;
		};
	};
};

&{/soc} {
	mdcldma: mdcldma@1000a000 {
		/* … existing properties … */
		memory-region = <&md1_carveout>;
		mediatek,md_smem_size = <0x30000>;	/* downstream mt6735.dtsi:399 */
	};
};
```

With this shape the driver maps `[base, base + round_up(total - 0x30000, 0x4000))`
as the firmware image and `[base + image_size, base + total)` as AP↔MD shared
memory. The base address and total size must come from the device's memory map;
they are not present in any available source, so no value is invented.

---

## 9. Patch

`$M/patches/ccci-gap-clock.patch` (six modified files, 267 lines, generated with
`git add -A && git diff --cached --binary`; nothing committed).
