# CCCI gap — MD1 carve-out, modem node and EINT DTS recovery

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Worktree `$M/worktrees/rem-ccci-dts`, branch `rem-ccci-dts`, HEAD `509789971`
("ccci: gap-closure scaffold"), baseline `e4a4be64e` (integration) on Nura
`046883c0c`.

## 1. Gap, files changed, result

**Gap.** The modem bring-up had no usable device-tree description: the MD1
carve-out values were recorded as `FILL` markers in
`$M/logs/ccci-eccci.md` §10, the `mdcldma@1000a000` node existed only as
prose fragments, the EINT attribute nodes the kernel RPC service reads were
absent, and the boot code could not resolve a carve-out described by
`size`/`alignment`/`alloc-ranges` (the downstream shape) because
`ccci_boot_region_get()` used `of_address_to_resource()`, which requires a
`reg` property.

**Files changed (worktree):**

| File | Change |
|---|---|
| `drivers/misc/mediatek/ccci/ccci_boot.c` | `ccci_boot_region_get()` resolves `memory-region` through `of_reserved_mem_region_to_resource()` first, falling back to the old phandle + `reg` path. `#include <linux/of_reserved_mem.h>`. 21 added lines, no reformatting. |

No shared DTS was edited. The clock leaf's single-carve-out split
(`$M/patches/ccci-gap-clock.patch`, `ccci_boot.c` hunk; report §1.2) is **not
in this worktree** and was not re-implemented. It was reviewed: the split uses
`CCCI_IMAGE_ALIGN` = `0x4000` (`ccci_boot.c:44`), which is exactly
`get_md_smem_align()` (`ccci_util_lib_fo.c:948-951`), and the
`round_up(total - smem)` + `base + image_size` math matches
`ccci_util_lib_fo.c:1120-1122`. No bug found, so nothing is reported against it.

**Result.** Every recovered value is now in the fragment set with a source
citation, and the driver can bind the single-carve-out fragment. The patch is
one isolated helper-function change; the split logic itself is the clock leaf's.

## 2. ROM + downstream evidence

### 2.1 MD1 carve-out (downstream is authoritative)

| Value | Evidence |
|---|---|
| Node `reserve-memory-ccci_md1`, `compatible = "mediatek,reserve-memory-ccci_md1"` | downstream `arch/arm64/boot/dts/mt6735.dtsi:197-198`; consumer key `ccci_util_lib_fo.c:1226` |
| `no-map` | `mt6735.dtsi:199` |
| `size = <0 0x3830000>` (md_size + smem_size) | `mt6735.dtsi:200` |
| `alignment = <0 0x2000000>` (32 MiB) | `mt6735.dtsi:201`; 32 MiB is also `CCCI_MEM_ALIGN` (`SZ_32M`) at `ccci_util_lib_fo.c:54`, checked by `ccci_util_lib_fo.c:1127` |
| `alloc-ranges = <0 0x40000000 0 0xC0000000>` | `mt6735.dtsi:202` |
| MT6735M variant `size = <0 0x3810000>` | `mt6735m.dtsi:139-144` |
| MT6735M `mediatek,md_smem_size = <0x10000>` | `mt6735m.dtsi:739` |
| `mediatek,md_smem_size = <0x30000>` on `mdcldma` | downstream `mt6735.dtsi:399`; ROM DTB `$M/zip/rom_dtb_0.dts:456` |
| split alignment `0x4000` | `ccci_util_lib_fo.c:948-951` (`get_md_smem_align()` returns `0x4000`) |
| split math: `image = round_up(total - smem, align)`, `smem = base + image` | `ccci_util_lib_fo.c:1120-1122` |
| resolved region is the modem total (`rmem->base/size` → `md_resv_mem_list/size`) | `ccci_util_lib_fo.c:1231-1258` |
| reserved-memory keys `_md1`, `_md2`, `_md3_ccif`, `_ccci_share` | `ccci_util_lib_fo.c:1226-1229` |
| ROM strings prove the properties are parsed | `$M/zip/vmlinux.strings:37107` (`mediatek,md_smem_size`), `:37118` (`mediatek,md1md3-smem-size`), `:37019-37022` and `:63561-63564` (the four reserve-memory keys) |

**ROM DTB cross-check.** `$M/zip/rom_dtb_0.dts:167-220` is the whole
`reserved-memory` node. Its children are `atf-reserved-memory@43000000`,
`ram_console-reserved-memory@43f00000`, `pstore-reserved-memory@43f10000`,
`minirdump-reserved-memory@43ff0000`, `consys-reserve-memory`,
`pl-reserved-memory@43d00000`, `lk-reserved-memory@41e00000`,
`sec-log@0x43700000`, `sec-lastkmsg@0x43900000`, `sec-debugmagic@0x43030000`.
**There is no `ccci_md1` (or any ccci) reserved-memory node in the ROM DTB**
(`grep ccci` finds only the `ccci_off` clock node at `:4068-4072`). The
carve-out fragment therefore comes from the downstream DTS, not the ROM.

Arithmetic for 0x3830000 / 0x30000: image = `round_up(0x3800000, 0x4000)` =
`0x3800000`, smem at base + `0x3800000`, size `0x30000`, region ends at
base + `0x3830000`. For the MT6735M 0x3810000 / 0x10000 the image is also
`0x3800000`.

### 2.2 `mdcldma` node

| Value | Downstream | ROM DTB |
|---|---|---|
| node / compatible | `mt6735.dtsi:386-387` `mdcldma@1000A000`, `mediatek,mdcldma` | `rom_dtb_0.dts:450-451` |
| reg 0 AP_CLDMA_AO | `<0x1000A000 0x1000>` `mt6735.dtsi:388` | `:452` first pair (`0x1000a000`) |
| reg 1 MD_CLDMA_AO | `0x1000B000` `mt6735.dtsi:389` | `:452` second pair |
| reg 2 AP_CLDMA_PDN | `0x1021A000` `mt6735.dtsi:390` | `:452` third pair |
| reg 3 MD_CLDMA_PDN | `0x1021B000` `mt6735.dtsi:391` | `:452` fourth pair |
| reg 4 AP_CCIF_BASE | `0x1020A000` `mt6735.dtsi:392` | `:452` fifth pair |
| reg 5 MD_CCIF_BASE | `0x1020B000` `mt6735.dtsi:393` | `:452` sixth pair |
| IRQ 0 CLDMA: SPI 145, level high | `mt6735.dtsi:394` | `:453` `0x00 0x91 0x04` |
| IRQ 1 CCIF: SPI 140, level low | `mt6735.dtsi:395` | `:453` `0x00 0x8c 0x08` |
| IRQ 2 MDWDT: SPI 221, edge falling | `mt6735.dtsi:396` | `:453` `0x00 0xdd 0x02` |
| `mediatek,md_id = <0>` | `mt6735.dtsi:397` | `:454` |
| `mediatek,cldma_capability = <6>` | `mt6735.dtsi:398` | `:455` |
| `mediatek,md_smem_size = <0x30000>` | `mt6735.dtsi:399` | `:456` |
| MD1 SCP gate clock | `clocks = <&scpsys SCP_SYS_MD1>`, `clock-names = "scp-sys-md1-main"` `mt6735.dtsi:400-401`; binding `include/dt-bindings/clock/mt6735-clk.h:231` (`SCP_SYS_MD1 = 1`) | `:457-458` (`<0x14 0x01>` = SCP_SYS_MD1) plus a second consumer `ccci_off` `:4068-4072` |

**INFRA_AO.** Downstream/ROM `infracfg_ao@10000000`: `rom_dtb_0.dts:315-318`
(`compatible = "mediatek,infracfg_ao"`, `reg = <0x10000000 0x1000>`). In the
Nura tree the same block is `infracfg: clock-controller@10000000`,
`compatible = "mediatek,mt6735-infracfg", "syscon"`,
`reg = <0 0x10000000 0 0x1000>` (`arch/arm64/boot/dts/mediatek/mt6735.dtsi:
145-150`). `ccci_md_map_infra_ao()` (`ccci_core.c:722-753`) resolves the
`mediatek,infracfg-ao` phandle and maps `res.start..+0x1000`; offsets
`INFRA_RST0 0x30` / `INFRA_RST1 0x34` (`eccci/mt6735/cldma_reg.h:18-19`) are
inside that window.

**Nura clock binding does not exist.** There is no `clk-mt6735-pg.c` and no
`SCP_SYS_*` macro anywhere in the tree (`include/dt-bindings/clock/` has only
topckgen/apmixedsys/imgsys/mfgcfg/mmsys/pericfg/vdecsys/vencsys), and the Nura
`scpsys` node has no `#clock-cells` (`mt6735.dtsi:182-187`). `clocks` is
therefore **not** emitted; the MD1 gate is the existing
`MT6735_POWER_DOMAIN_MD1` (`include/dt-bindings/power/mt6735-power.h:6`,
`mt6735.dtsi:192-196`), which the core holds through runtime PM
(`ccci_platform.c:208`), exactly as the already-delivered fragments state.

### 2.3 EINT attribute nodes

The downstream RPC service finds the nodes by **name**, independent of their
parent: `of_find_node_by_name(NULL, name)` at
`drivers/misc/mediatek/eccci/port_kernel.c:983`, names from
`md_eint_node[]` (`:894-907`), attributes from `md_eint_struct[]` (`:882-893`,
`interrupts[0]`, `debounce[1]`, `interrupts[1]` for polarity+sensitivity,
`sockettype[1]`, `dedicated[1]`, `src_pin[1]`), read by `get_eint_attr_val()`
(`:920-970`), debounce converted µs→ms by `/1000` (`:924`, `:957`).

| Value | Source |
|---|---|
| EINT controller: `eintc@10005000`, `compatible = "mediatek,mt-eic"`, `reg = <0x10005000 0x1000>`, `interrupts = <0 0x99 0x04>` (SPI 153, level high), `#interrupt-cells = <2>`, `interrupt-controller`, `mediatek,max_eint_num = <0xd5>` | ROM `rom_dtb_0.dts:403-412` |
| Nura EINT controller: `pio: pin-controller@10211000`, `compatible = "mediatek,mt6735-pinctrl"`, 8th reg window `<0 0x10005000 0 0x1000>` with `reg-names = "... eint"`, `interrupts = <GIC_SPI 153 IRQ_TYPE_LEVEL_HIGH>`, `interrupt-controller`, `#interrupt-cells = <2>` | `arch/arm64/boot/dts/mediatek/mt6735.dtsi:276-300` |
| Note: the Nura tree has **no separate `eintc` node/label**; the EINT block is part of `pio` (mainline merged pinctrl+EINT). The assignment's "`mt6735.dtsi:353`" does not hold in this tree — line 353 is an MMC pin group. The correct anchor is `pio` (line 275), whose EINT window, IRQ and 2 interrupt cells match `rom_dtb_0.dts:403-408` exactly. | — |
| `MD1_SIM1_HOT_PLUG_EINT@0`: `compatible = "mediatek,MD1_SIM1_HOT_PLUG_EINT-eint"`, `interrupts = <0 4>`, `debounce = <0 0x186a0>`, `dedicated = <0 0>`, `src_pin = <0 1>`, `sockettype = <0 0>`, `status = "okay"` | ROM `rom_dtb_0.dts:423-431` |
| `MD1_SIM2_HOT_PLUG_EINT@1`: `compatible = "mediatek,MD1_SIM2_HOT_PLUG_EINT-eint"`, `interrupts = <1 4>`, `debounce = <1 0x186a0>`, `dedicated = <1 0>`, `src_pin = <1 2>`, `sockettype = <1 0>`, `status = "okay"` | ROM `rom_dtb_0.dts:433-441` |

Decoded: EINT numbers 0 and 1; both level-high (`IRQ_TYPE_LEVEL_HIGH` = 4,
`value & 0x5`→polarity 1, `value & 0x3`→sensitivity 0 = level,
`port_kernel.c:931-943`); debounce 100000 µs = 100 ms; dedicated 0; source
pins 1 and 2. These are the only `*-eint` MD nodes in the ROM DTB; the other
`-eint` compatibles (`pmic-eint` `:416-421`, `msdc1_ins-eint` `:96`,
`als-eint` `:4668`, `gse_1-eint` `:4689`, `ext_buck_oc-eint` `:4695`) are not
modem attributes.

### 2.4 Driver-side resolution of the recovered carve-out

| Item | Evidence |
|---|---|
| Reserved-memory children with `size`/`alignment`/`alloc-ranges` are allocated at boot and only stored in `reserved_mem[]`; no `reg` is written back | `drivers/of/of_reserved_mem.c:391-491` (`__reserved_mem_alloc_size()` → `fdt_reserved_mem_save_node()`) |
| `of_address_to_resource()` needs a `reg`, so it fails on that shape | Nura `ccci_boot_region_get()` before this patch (`ccci_boot.c:222-243`) |
| Correct resolver: `of_reserved_mem_region_to_resource(dev_node, index, &res)` returns `rmem->base/size` for either shape | `drivers/of/of_reserved_mem.c:756-777`; `of_reserved_mem_lookup()` by node basename at `:727-742`; declared in `include/linux/of_reserved_mem.h`; `OF_RESERVED_MEM` is `def_bool OF_EARLY_FLATTREE` (`drivers/of/Kconfig:99-100`) |

## 3. Parity matrix

| Behaviour | ROM + downstream implementation | Nura before | After | Status |
|---|---|---|---|---|
| MD1 carve-out size/alignment/alloc-ranges | `mt6735.dtsi:197-203` (`0x3830000`, 32 MiB, 0x40000000+0xC0000000) | `FILL` markers (`ccci-eccci.md` §10) | exact fragment, §6.1 | **done** |
| MT6735M carve-out variant | `mt6735m.dtsi:139-144` `0x3810000`, smem `0x10000` (`:739`) | absent | documented in §6.1 note; not used by the MT6737T/grandpplte tree | **done** (documented) |
| Single carve-out split in `ccci_boot_smem_setup()` | `ccci_util_lib_fo.c:1120-1122`, align `0x4000` (`:948-951`) | only region[0]=image + region[1]=smem | delivered by the **clock leaf** (`ccci-gap-clock.patch`), not re-implemented here; reviewed consistent | **done (other leaf)** |
| Resolution of a `size`-only `memory-region` node | downstream uses `RESERVEDMEM_OF_DECLARE` + `rmem->base` (`fo.c:1231-1258`) | `of_address_to_resource()` → no base/size → split never triggers, silent `dma_alloc_coherent` fallback | `of_reserved_mem_region_to_resource()` first, `reg` fallback preserved | **done (this patch)** |
| Two-region `memory-region` DT shape | not used downstream | supported | unchanged, still supported | **done** |
| `mdcldma` node (6 windows, 3 IRQs, md_id, capability, smem size, INFRA_AO) | `mt6735.dtsi:386-401`, ROM `:450-458` | prose snippets (`ccci-eccci.md` §10, `ccci-cldma.md` §9) | exact fragment §6.2 | **done** |
| `clocks = <&scpsys SCP_SYS_MD1>` | `mt6735.dtsi:400-401`, ROM `:457-458` | no binding | not emitted; MD1 power domain used instead (`mt6735.dtsi:192-196`) | **deferred** (no Nura clock binding) |
| EINT SIM1/SIM2 attribute nodes | ROM `:423-441`, reader `port_kernel.c:882-990` | absent | exact fragment §6.3 | **done** |
| `mediatek,md1-smem-size` / `md3-smem-size` / `md1md3-smem-size` | parsed by `cal_md_settings_v2()` `fo.c:1157-1175`; ROM string `vmlinux.strings:37118` | no consumer anywhere in the Nura tree (no IRAT/MD3 smem window; `ccci_core.c` has no md1↔md3 region) | **not parsed** — recovered-but-unused, `deferred` | **deferred** (no consumer, no size) |
| MD3 (`_md3_ccif`) carve-out | key `fo.c:1228`, parsed generically `:1243-1244` | n/a | **no size in any source** → no fragment | **blocked** (missing size) |
| MD1↔MD3 (`_ccci_share`) carve-out | key `fo.c:1229`, used by `get_md1_md3_resv_smem_info()` (`fo.c:934-946`) and mapped/zeroed at `eccci/ccci_core.c:297-301` | n/a | **no size in any source** → no fragment | **blocked** (missing size) |

## 4. Compile-tested

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/rem-ccci-dts"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K -e MTK_CCCI_UTIL -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig          # exit 0
grep -E '^CONFIG_MTK_CCCI' .config
# CONFIG_MTK_CCCI=y
# CONFIG_MTK_CCCI_C2K=y
# CONFIG_MTK_CCCI_CLDMA=y
# CONFIG_MTK_CCCI_UTIL=y
```

Objects:

```sh
rm -f drivers/misc/mediatek/ccci/ccci_boot.o drivers/misc/mediatek/ccci/built-in.a
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci/ccci_boot.o    # exit 0, 0 warnings
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci/built-in.a    # exit 0
#   CC drivers/misc/mediatek/ccci/ccci_boot.o   (rebuilt from scratch)
#   AR drivers/misc/mediatek/ccci/built-in.a
```

`llvm-ar t drivers/misc/mediatek/ccci/built-in.a` → 9 members:
`ccci_core.o ccci_platform.o ccci_ccif.o ccci_boot.o ccci_port_char.o
ccci_port_net.o ccci_port_kernel.o ccci_ringbuf.o ccci_sysfs.o`.
Logs: `$M/logs/rem-ccci-dts_ccci_boot_o.log`,
`$M/logs/rem-ccci-dts_built_in_a.log`.

`W=1` warnings (all pre-existing, none from `ccci_boot.c`):
- `ccci_core.c:370` kernel-doc "parameter 'md'/'qno'/'skb' not described in
  'ccci_md_send_skb'" (6 lines, duplicated by the doc checker);
- `ccci_ccif.c:145` kernel-doc "parameter 'md'/'channel' not described in
  'ccci_ccif_send'" (4 lines);
- `ccci_ringbuf.c:181` kernel-doc "parameter 'rb'/'size' not described" (4 lines);
- `ccci_ringbuf.c:209` `-Wunused-but-set-variable` (`read`).
- Two `scripts/Makefile.build:462/541: warning: ... recipe for target
  'drivers/misc/mediatek/ccci/built-in.a'` lines. These are emitted by Kbuild
  whenever a subdirectory archive target is named directly on the command line
  (the recursion also wants it as a stop target); they are not code warnings.

`make ... drivers/misc/mediatek/ccci/ccci.o` exits 2 with
`No rule to make target 'drivers/misc/mediatek/ccci/ccci.o'` — a Kbuild
single-target limitation for a composite object whose `ccci-y` list lives one
directory below the top-level target; it is not a compile failure. The
`built-in.a` build above compiles every member with the directory `ccflags-y`,
so it is the authoritative check.

### 4.1 DTS compile check (scratch, outside the worktree)

```
$M/scratch/ccci-dts-check/base.dts : #include "mt6737t-samsung-grandpplte.dts"
$M/scratch/ccci-dts-check/frag.dts : same include + the §6 fragments
```

```sh
W=$M/worktrees/rem-ccci-dts; S=$M/scratch/ccci-dts-check; cd "$S"
cpp -nostdinc -undef -x assembler-with-cpp \
    -I "$W/include" -I "$W/arch/arm64/boot/dts/mediatek" -I "$W/arch/arm64/boot/dts" \
    frag.dts -o frag.pp.dts                     # exit 0
"$W/scripts/dtc/dtc" -I dts -O dtb -o frag.dtb frag.pp.dts   # exit 0
```

Result: **exit 0**, and the warning set is byte-identical to the unmodified
board DTS (one pre-existing warning only):

```
mt6735.dtsi:424.38-433.5: Warning (simple_bus_reg):
  /soc/interrupt-controller@10220000: simple-bus unit address format error,
  expected "10221000"
```

No warning comes from the added nodes: the carve-out has no unit address and
no `reg`; `mdcldma@1000a000` is lowercase and matches its first `reg`; the EINT
nodes carry no unit suffix (see §5).
Decompiled checks (in `frag.decompiled.dts`):
- `memory-region = <0x30>` → the carve-out node; `mediatek,infracfg-ao = <0x03>`
  → `clock-controller@10000000` (`mediatek,mt6735-infracfg`, 0x10000000+0x1000);
  `power-domains = <0x13 0x00>` → the `mt6735-power-controller` phandle with
  `MT6735_POWER_DOMAIN_MD1` (0).
- Both EINT nodes are children of `pin-controller@10211000` (`pio`), whose
  `interrupts = <0x00 0x99 0x04>` and 8th reg window (`0x10005000`, name
  `"eint"`) match the ROM `eintc`.
- Carve-out decodes to `size = <0 0x3830000>`,
  `alignment = <0 0x2000000>`, `alloc-ranges = <0 0x40000000 0 0xc0000000>`, no `reg`.
Logs: `$M/logs/rem-ccci-dts_dtc_base.log`, `$M/logs/rem-ccci-dts_dtc_frag.log`,
test input `$M/logs/rem-ccci-dts_fragment_test.dts`.

## 5. `[INFERENCE]` and hardware-unverified

- The EINT attribute nodes are placed under `&pio` because that is the only
  EINT interrupt controller in the Nura tree (ROM `eintc`). `[INFERENCE]` that
  no other consumer depends on the parent node name: the RPC lookup is
  `of_find_node_by_name()` over the whole tree
  (`port_kernel.c:983`) and `of_node_name_eq()` matches the name before `@`.
- The ROM child node names end in `@0`/`@1`. The fragment drops those unit
  suffixes: the nodes have no `reg`/`ranges`, so modern dtc emits
  `unit_address_vs_reg` warnings for them, and `of_find_node_by_name()` matches
  the base name either way. No property value changes.
- `[INFERENCE]` The Nura pinctrl driver never walks `pio`'s children at probe
  (`pinctrl-paris.c:546` only iterates a *referenced* pin-config node), so the
  extra children cannot break pinctrl.
- `[INFERENCE]` `mediatek,cldma_capability = <6>` describes 6 CLDMA queues;
  the value is copied verbatim and its interpretation is the CLDMA engine's.
- Hardware-unverified: nothing was run on a device. The reserved-memory
  allocator's actual chosen base (32 MiB-aligned in
  0x40000000..0x100000000), modem visibility of that base, EINT polarity and
  debounce as seen by the MD, and the MD1 power-domain attach are source-only.
- `[INFERENCE]` The ROM `ccci_off` clock node (`:4068-4072`) confirms
  `SCP_SYS_MD1` is a gate around this block, but the Nura tree models it as the
  MD1 power domain; no clock binding exists to emit.

## 6. DTS-FRAGMENT

Not applied. Exact text, validated as described in §4.1. It is written for the
board DTS (`arch/arm64/boot/dts/mediatek/mt6737t-samsung-grandpplte.dts`) and
uses the Nura forms: `&{/soc}` (the `soc` node has no label), four-cell
`<0 ADDR 0 SIZE>` `reg`, lowercase `mdcldma@1000a000`.

### 6.1 Carve-out + modem node

```dts
/*
 * CCCI / MD1 carve-out, single-region form.  Values from downstream
 * arch/arm64/boot/dts/mt6735.dtsi:197-203 reserve-memory-ccci_md1
 * (SM-G532M tree; the ROM DTB has no ccci reserved-memory node).  The base
 * is chosen by the reserved-memory allocator, so the node has size/
 * alignment/alloc-ranges and no reg.
 *
 * ccci_boot_smem_setup() splits it using mediatek,md_smem_size:
 *   image = round_up(0x3830000 - 0x30000, 0x4000) = 0x3800000 at the base
 *   smem  = base + 0x3800000, size 0x30000
 * (downstream ccci_util/ccci_util_lib_fo.c:948-951, :1120-1122).
 */
/ {
	reserved-memory {
		md1_carveout: reserve-memory-ccci_md1 {
			compatible = "mediatek,reserve-memory-ccci_md1";
			no-map;
			size = <0 0x3830000>;
			alignment = <0 0x2000000>;
			alloc-ranges = <0 0x40000000 0 0xC0000000>;
		};
	};
};

&{/soc} {
	mdcldma: mdcldma@1000a000 {
		compatible = "mediatek,mdcldma";
		reg = <0 0x1000a000 0 0x1000>,	/* AP_CLDMA_AO  */
		      <0 0x1000b000 0 0x1000>,	/* MD_CLDMA_AO  */
		      <0 0x1021a000 0 0x1000>,	/* AP_CLDMA_PDN */
		      <0 0x1021b000 0 0x1000>,	/* MD_CLDMA_PDN */
		      <0 0x1020a000 0 0x1000>,	/* AP_CCIF_BASE */
		      <0 0x1020b000 0 0x1000>;	/* MD_CCIF_BASE */
		interrupts = <GIC_SPI 145 IRQ_TYPE_LEVEL_HIGH>,	/* IRQ_CLDMA */
			     <GIC_SPI 140 IRQ_TYPE_LEVEL_LOW>,	/* IRQ_CCIF  */
			     <GIC_SPI 221 IRQ_TYPE_EDGE_FALLING>; /* IRQ_MDWDT */
		mediatek,md_id = <0>;
		mediatek,cldma_capability = <6>;
		mediatek,md_smem_size = <0x30000>;
		mediatek,infracfg-ao = <&infracfg>;	/* INFRA_RST0/1 0x30/0x34 */
		memory-region = <&md1_carveout>;
		firmware-name = "md1img";
		/* no clocks: the tree has no SCP_SYS_MD1 clock provider; MD1 is
		 * the MT6735_POWER_DOMAIN_MD1 genpd.
		 */
		power-domains = <&spm MT6735_POWER_DOMAIN_MD1>;
		status = "okay";
	};
};
```

Notes / variants:

- **MT6735M** (not used by this board): `size = <0 0x3810000>` and
  `mediatek,md_smem_size = <0x10000>` (`mt6735m.dtsi:139-144`, `:739`); the
  split is the same `0x3800000` image.
- **Two-region form** (still supported by the driver, kept from
  `ccci-eccci.md` §10): `memory-region = <&md1_image>, <&md1_smem>` with two
  reserved-memory nodes carrying explicit `reg`. It cannot be emitted from the
  available sources because no base address exists for either half; the
  single-region form above is the recovered downstream layout.
- **MD3 (`mediatek,reserve-memory-ccci_md3_ccif`) and MD1↔MD3
  (`mediatek,reserve-memory-ccci_share`)**: no `size` exists in any source —
  the downstream DTS has only `reserve-memory-ccci_md1`
  (`grep` over `src/downstream-grandpplte/arch/arm64/boot/dts/`), the ROM DTB
  reserved-memory has no ccci node (`rom_dtb_0.dts:167-220`), and no
  `ap2c2k_ccif`/`md3-smem-size`/`md1md3-smem-size` property is present
  anywhere. **No fragment is emitted; the missing input is the MD3 carve-out
  size and the MD1↔MD3 share size (downstream keys `fo.c:1228-1229`).**
- `mediatek,md1-smem-size` etc. are parsed by downstream `cal_md_settings_v2()`
  (`fo.c:1157-1175`) but have no consumer in this tree (no MD3 IRAT smem
  window); they are **recovered-but-unused**, not emitted, not parsed by the
  driver.

### 6.2 EINT attribute nodes

```dts
/*
 * SIM hot-plug EINT attributes read by the kernel RPC service
 * (downstream eccci/port_kernel.c:882-990 via of_find_node_by_name()).
 * Values are the ROM DTB children of eintc (rom_dtb_0.dts:423-441); in the
 * Nura tree the EINT block is part of pio (mt6735.dtsi:276-300, reg-name
 * "eint", GIC_SPI 153), so the children live here.  The ROM node names carry
 * @0/@1; the suffix is dropped because the nodes have no reg/ranges and the
 * lookup matches the base name.
 * interrupts = <eint_nr IRQ_TYPE_LEVEL_HIGH>; debounce is in microseconds
 * (downstream divides by 1000 for the MD).
 */
&pio {
	MD1_SIM1_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM1_HOT_PLUG_EINT-eint";
		interrupts = <0 IRQ_TYPE_LEVEL_HIGH>;
		debounce = <0 0x186a0>;
		dedicated = <0 0>;
		src_pin = <0 1>;
		sockettype = <0 0>;
		status = "okay";
	};

	MD1_SIM2_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM2_HOT_PLUG_EINT-eint";
		interrupts = <1 IRQ_TYPE_LEVEL_HIGH>;
		debounce = <1 0x186a0>;
		dedicated = <1 0>;
		src_pin = <1 2>;
		sockettype = <1 0>;
		status = "okay";
	};
};
```

### 6.3 Difference from the already-delivered fragments

| Item | `ccci-eccci.md` §10 / `ccci-cldma.md` §9 | This report |
|---|---|---|
| carve-out | two `FILL` regions (`md1-image@0`, `md1-smem@0`, `reg = <0 0 0 0>`) | single region with downstream size/alignment/alloc-ranges + `compatible` key; two-region form retained as an alternative |
| `memory-region` | `<&md1_image>, <&md1_smem>` | `<&md1_carveout>` |
| `mdcldma` regs/IRQs | identical | identical (re-verified against both sources) |
| `mediatek,infracfg-ao` | only in `ccci-cldma.md` §9 as an add-on block | included in the node |
| `firmware-name` | `ccci-eccci.md` has it; `ccci-cldma.md` does not | included |
| status | eccci `"okay"`, cldma `"disabled"` | `"okay"` (the node is the recovered hardware description; disable in the board if the modem is unused) |
| clocks | commented out | not emitted, with the binding-absence reason stated |
| EINT nodes | not delivered | delivered |

## 7. SCAFFOLD CHANGE REQUIRED

none.

Integration dependency (not a header change): this leaf supplies the
`memory-region` resolution that makes the clock leaf's single-carve-out split
reachable. If `ccci-gap-clock.patch` is dropped at integration, §6.1 falls back
to `dma_alloc_coherent` for smem; if this patch is dropped but the clock patch
stays, the split silently never triggers for the recovered (`reg`-less) node.
Both hunks are independent (helper at `ccci_boot.c:222-243` vs. the split in
`ccci_boot_smem_setup()` at `:259+`), so they apply in either order. Verified:

```sh
cd "$M/worktrees/ccci-gap-clock"          # already contains the clock split
git apply --check "$M/patches/rem-ccci-dts.patch"
# exit 0  → "apply-check on ccci-gap-clock: OK"
```


## 8. Patch and status

Patch: `$M/patches/rem-ccci-dts.patch` (39 lines, 1 file, +21/−0), produced by
`git add -A && git diff --cached --binary` at HEAD `509789971`.

`git status --porcelain`:

```
M  drivers/misc/mediatek/ccci/ccci_boot.c
```

No other file is staged or modified; the worktree is left dirty at
`509789971` (not committed). No shared DTS, no `.config`, no other leaf's
worktree touched.
