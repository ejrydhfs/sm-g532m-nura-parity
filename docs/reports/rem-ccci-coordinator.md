# CCCI remaining gaps — coordinator report

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

Scope: the four remaining CCCI gap areas (modem MPU/security, kernel RPC,
AEE/SBP/DHL dump + userspace tooling, CLDMA/carve-out/DTS recovery), on top of
the delivered CCCI subsystem and the already-finished CLDMA and clock gap
leaves.

Base chain:

```
Nura 046883c0c
  + $M/patches/ccci-integration.patch        (subsystem baseline)
  + $M/patches/ccci-gap-scaffold.patch       (frozen gap API + infra_ao mapping)
  = tree 96da16cf6 / scaffold commit 509789971   (every leaf worktree HEAD)
  + the six gap patches below = this report
```

Integration worktree: `$M/worktrees/ccci-gap-integration`, HEAD
`e640c6687 ccci: gap scaffold` (tree identical to `509789971`), left dirty,
not committed. No shared DTS was edited; no file under
`arch/arm64/boot/dts/` is modified by any patch.

## 1. Result

| Leaf | Worktree | Area | Status | Patch | Report |
|---|---|---|---|---|---|
| mpu | `worktrees/rem-ccci-mpu` | EMI MPU via ATF SMC, RMPU remap, DSP region, security state, AP re-protection, op `0x400B` backend | **done** | `patches/rem-ccci-mpu.patch` | `logs/rem-ccci-mpu.md` |
| rpc | `worktrees/rem-ccci-rpc` | RPC engine, wire format, RX/TX, registration API, built-in services | **partial** (3 services need absent providers) | `patches/rem-ccci-rpc.patch` | `logs/rem-ccci-rpc.md` |
| dump | `worktrees/rem-ccci-dump` | exception record, AEE ABI, DHL handshake/flush, SBP, dump engine, `tools/mediatek/ccci` | **partial** (DHL RX-alive window; AEE DB client; legacy SBP WM_ID) | `patches/rem-ccci-dump.patch` | `logs/rem-ccci-dump.md` |
| dts | `worktrees/rem-ccci-dts` | carve-out/DTS data recovery, reg-less reserved-memory resolution, fragments | **done** for recovered data; MD3/MD1MD3 sizes unrecoverable | `patches/rem-ccci-dts.patch` | `logs/rem-ccci-dts.md` |
| cldma | `worktrees/ccci-gap-cldma` | exception rings, RGU reset, start/stop serialisation (pre-delivered) | done | `patches/ccci-gap-cldma.patch` | `logs/ccci-gap-cldma.md` |
| clock | `worktrees/ccci-gap-clock` | MD2 power domain + single-carve-out split (pre-delivered) | done | `patches/ccci-gap-clock.patch` | `logs/ccci-gap-clock.md` |

Everything integrates, compiles and links into `vmlinux`. The remaining
`partial`s are external-provider / frozen-API / unrecoverable-data gaps, all
named in §6; none of them is a stub or a fabricated answer.

## 2. Deliverables

| File | Content | Diffstat |
|---|---|---|
| `patches/rem-ccci-mpu.patch` | `ccci_mpu.c/.h`, core/boot/priv wiring, Makefile | 6 files, +1024/−10 |
| `patches/rem-ccci-rpc.patch` | `ccci_rpc.c/.h`, Makefile, core demux/init, port kernel dispatch | 5 files, +1639/−6 |
| `patches/rem-ccci-dump.patch` | `ccci_exception.c/.h`, ccif handshake, boot/port/priv/core, Kconfig/Makefile, `tools/mediatek/ccci/*` | 13 files, +2295/−22 |
| `patches/rem-ccci-dts.patch` | `ccci_boot.c` reserved-memory resolution | 1 file, +21/−0 |
| `patches/ccci-gap-cldma.patch` | pre-delivered CLDMA leaf | 4 files, +336/−28 |
| `patches/ccci-gap-clock.patch` | pre-delivered clock/power leaf | 6 files, +118/−27 |
| **`patches/rem-ccci-integration.patch`** | all six leaf patches merged (relative to scaffold `509789971`) | **26 files, +5432/−93** |
| **`patches/rem-ccci-integration-full.patch`** | scaffold + all six leaf patches (relative to `ccci-integration.patch` state) | **27 files, +5541/−93** |

Convenience aliases for the CCCI gap contract names:
`patches/ccci-gap-{mpu,rpc,dump,dts}.patch` and
`logs/ccci-gap-{mpu,rpc,dump,dts}.md` are symlinks to the `rem-ccci-*`
deliverables.

Applicability (verified, not assumed):

```sh
# leaf combined patch on the scaffold tree
cd $M/worktrees/ccci-gap-base && git apply --check $M/patches/rem-ccci-integration.patch   # OK
# full patch on the integration-baseline tree
cd $M/worktrees/ccci-integration && git apply --check $M/patches/rem-ccci-integration-full.patch  # OK
```

## 3. Integration and conflict resolution

Applied in this order: `ccci-gap-clock`, `ccci-gap-cldma`, `rem-ccci-dts`,
`rem-ccci-mpu`, `rem-ccci-rpc`, `rem-ccci-dump` (the last with `git apply
--3way`, because three leaves edited the same shared headers).

| Conflict | Cause | Resolution |
|---|---|---|
| `drivers/misc/mediatek/ccci/ccci_priv.h` include block | mpu added `#include "ccci_mpu.h"`, dump added `"ccci_exception.h"` at the same line | both kept (`ccci_exception.h` then `ccci_mpu.h`) |
| `ccci_priv.h` `struct ccci_md` tail | mpu's MPU state fields vs dump's `struct ccci_exception ex` | both kept, MPU block first, exception block last |
| `ccci/Makefile` `ccci-y` list | three leaves appended their object | one list: `ccci_mpu.o`, `ccci_rpc.o`, `ccci_exception.o` each exactly once (12 members in `built-in.a`) |
| `ccci_core.c` init/exit | mpu inserted boot calls, rpc inserted module init/exit, dump inserted per-md exception init/exit | 3-way merge kept all; order verified (§3.1) |
| `ccci_boot.c` | dts helper, clock split, mpu header/AP-translation/DSP slot, dump SBP/runtime changes | all four layers present; helper precedes the split, split precedes mapping (§3.2) |

### 3.1 Core ordering (verified in the merged file)

- `ccci_init()`: class → sysfs → util → `ccci_rpc_init()` → platform register;
  unwind labels call `ccci_rpc_exit()`/`ccci_util_exit()`/`ccci_sysfs_exit()`.
- `ccci_md_register()`: per-md device → `ccci_md_map_infra_ao()` →
  `ccci_mpu_init()` → boot init → smem setup → ports → sysfs →
  `ccci_exception_init()`; unwind calls `ccci_exception_exit()` first.
- `ccci_md_start()`: `ccci_init_security()` → `ccci_mpu_clear_md_protection()`
  → load firmware → `ccci_mpu_set_mem_remap()` → `ccci_mpu_set_md_protection()`
  → power on; `ccci_md_stop()`/`ccci_md_reset()` clear the protection.
- RPC RX has exactly one owner: `ccci_rx_dispatch_qno()` skips non-kernel
  ports on `CCCI_RPC_RX`, and `ccci_port_kernel.c` calls
  `ccci_rpc_handle()`; the handler frees the request skb on every path
  (success, malformed, overflow).
- `aee_dump_ccci_debug_info()` has exactly one definition (in `ccci_core.c`)
  and one export; `llvm-nm vmlinux` shows one `T` symbol.

### 3.2 Boot/smem layering (verified in the merged file)

`ccci_boot_smem_setup()` now: resolves `memory-region` through
`of_reserved_mem_region_to_resource()` first (dts leaf, handles the reg-less
downstream carve-out) → falls back to phandle+`reg`; takes
`memory-region[1]` if present, else splits one carve-out with
`mediatek,md_smem_size` at `round_up(total − smem, 0x4000)` (clock leaf);
then maps the image half. MPU adds the DSP slot parse
(`dsp_img_offset/dsp_img_size`, `0xCDCDCDAA` disabled marker), publishes
`loaded_size`, and translates the published share-memory addresses with
`smem_offset_ap_to_md`. The SBP/runtime-data changes from the dump leaf sit
after the regions are established. No duplicate `dt_smem_size` read or
double mapping was introduced by the merge (single read at the top of the
`np` block).

## 4. Compile / test evidence

Environment: `export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`.
Config (note: `nura.config` does not carry the CCCI symbols; they are enabled
explicitly):

```sh
cp $M/nura.config .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K -e MTK_CCCI_UTIL -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig          # rc=0
grep '^CONFIG_MTK_CCCI' .config               # all =y
```

Objects (`W=1`, log `logs/rem-ccci-integration_objects.log`):

```sh
make ARCH=arm64 LLVM=1 -j8 W=1 drivers/misc/mediatek/ccci/built-in.a
# rc=0; 12 members: core, platform, ccif, boot, mpu, port_char, port_net,
# port_kernel, rpc, ringbuf, sysfs, exception
```

Warnings in that log: one pre-existing `ccci_ringbuf.c:209` unused-but-set
variable and two kbuild "overriding recipe" notices caused by naming
`built-in.a` explicitly. No warning or error from any new code.

Userspace (dump leaf):

```sh
make -C tools/mediatek/ccci            # rc=0, cc -O2 -Wall -Wextra -std=gnu11
make -C tools/mediatek/ccci check      # rc=0: "help, input validation and error paths OK"
./tools/mediatek/ccci/build/bin/ccci-dump -h    # usage, rc=0
```

Full kernel (`logs/rem-ccci-integration_build.log`):

```sh
make ARCH=arm64 LLVM=1 -j8 vmlinux     # rc=0 in 688 s, 2118 CC lines, 0 errors
# vmlinux 250,598,016 bytes, System.map 4,158,065 bytes
```

Linked symbols in `vmlinux` (`llvm-nm`, one `T` each):
`ccci_md_set_dsp_mpu_protection`, `ccci_rpc_handle`,
`ccci_rpc_register_service`, `aee_dump_ccci_debug_info`,
`ccci_exception_trigger`, `ccci_cldma_set_exception_mode`,
`ccci_cldma_hw_reset`, `ccci_mpu_set_mem_remap`, `ccci_init_security`.

Hygiene: `git diff --cached --check` clean; no `TODO`/`FIXME`/placeholder
strings in the new sources; no DTS file modified.

## 5. Parity summary

Full matrices and every citation are in the leaf reports. Condensed:

### 5.1 MPU / RMPU / security (mpu leaf, done)

| Behaviour | Status |
|---|---|
| EMI MPU region ids/permission words (MT6735 non-M table) | done, `ccci_platform.c:50-120` cited per constant |
| EMI MPU write through ATF SMC `MTK_SIP_KERNEL_EMIMPU_SET = 0x82000209` (downstream `mt_secure_api.h:38`, ROM `mt_secure_call` `0xc064c6f8`) | done; no SMC conduit → `-ENODEV` + log, no fake success |
| RMPU BANK0/BANK4 windows, offsets `0x300..0x31c`, 32 MiB slot stride | done, ROM `set_md_rom_rw_mem_remap @ 0xc05303b4` cited |
| MD ROM/RW/SMEM region programming + clear, AP read-only region, AP re-protection before runtime data | done |
| DSP region from check-header v3 slot; `ccci_md_set_dsp_mpu_protection(md->id)` (0-based) | done; returns 0 with no DSP region (MT6735/MT6737T) |
| `ccci_init_security()` state report | done (reports the image-security state it can observe; no fabricated attestation) |

### 5.2 Kernel RPC (rpc leaf, partial)

| Op | Status |
|---|---|
| wire parse/framing/response, registry API (`EXPORT_SYMBOL_GPL`) | done |
| 0x2002 SECRO (`{FS_NO_FEATURE,0}`), 0x4001 TDD EINT, 0x4004 EMI clk type, 0x400a USIM2NFC, 0x4321 IT | done |
| 0x4002 GPIO num, 0x4006 GPIO val | done (DT `mediatek,gpio_usage_mapping`, gpiolib) |
| 0x4003 ADC num, 0x4005 EINT attr, 0x400c queue mapping | done (DT properties/nodes) |
| 0x4009 GPIO+ADC block | GPIO half done, ADC half returns `FS_NO_FEATURE` |
| 0x400b DSP EMI MPU | done (calls mpu leaf, 0-based id) |
| 0x2001 SECURE_ALGO | deferred: no MASP/asfv2/HACC driver in tree; validates args, returns `FS_NO_FEATURE`, logs |
| 0x4007 ADC value | deferred: in-tree auxadc exposes mV only; raw needs a provider change |
| 0x4008 RF clk buffer, count==4 | deferred: no `clk_buf_get_swctrl_status()`/clk-buf driver |
| "time" | already answered by `ccci_boot.c` `CCCI_RT_MISC_INFO_CLIB_TIME`; verified, not duplicated |

### 5.3 AEE / SBP / DHL dump (dump leaf, partial)

| Behaviour | Status |
|---|---|
| `EX_LOG_T`/`EX_ENVINFO`/`EX_HEADER` layout, offender at 288, `EMI_MPU_VIOLATION 0x30` | done (`BUILD_BUG_ON`s) |
| `aee_dump_ccci_debug_info()` 1-based, smem debug window, `-316` | done (ROM `0xc0508b54`) |
| exception packet capture/validate, record build (packet vs smem rule) | done |
| decode (assert file/line/params, fatal codes, RMPU/C2K notes) | done, kernel + userspace |
| stages `EX_INIT`, `EX_DHL_DL_RDY`, `CLEARQ_DONE` (2 s flush + stop + purge + sync dump + ACK), `ALLQ_RESET`, `MD_WDT`, `MD_NO_RESPONSE` | done |
| DHL control-port META gate, `CCCI_IOC_GET_MD_PROTOCOL_TYPE = "DHL"`, `GET/SET_MD_SBP_CFG` | done |
| SBP handshake v2 and legacy SRAM feature indexes | done |
| DHL RX ring alive during the 2 s window (`cldma_stop_for_ee`) | deferred, frozen API (proposal in dump report §7) |
| AEE database client (`aed_md_exception_api`) | deferred: not present in tree/ROM sources; ROM ABI provided for it |
| legacy SBP `WM_ID` (`feature_4_val[1]`) | deferred: modem load-type data absent |
| userspace `tools/mediatek/ccci/ccci-dump` (`/dev/ccci_dump`, debugfs `ccci/mdN/exception_*`, `-D` decoder) | done, builds and self-checks |

### 5.4 CLDMA / carve-out / DTS (dts leaf done + pre-delivered cldma/clock)

| Behaviour | Status |
|---|---|
| reg-less `reserved-memory` carve-out resolution (`of_reserved_mem_region_to_resource`) | done (dts leaf) |
| single carve-out split (image `round_up(total−smem,0x4000)`, smem at base+image) | done (clock leaf), reviewed by dts leaf |
| MD1 carve-out values `0x3830000 / 0x2000000 / 0x40000000..0xC0000000`, smem `0x30000` | recovered, fragment §7 |
| mdcldma node (6 windows, 3 IRQs, md_id, capability, smem size, infracfg-ao) | recovered, fragment §7; matches downstream and ROM DTB |
| EINT attribute nodes (SIM hot-plug) | recovered from ROM DTB `:423-434`, fragment §7 |
| MD2/C2K power domain + `SCP_SYS_MD2` | done (clock leaf) |
| MD3 and MD1↔MD3 carve-out sizes | **unrecoverable**, see §6 |
| CLDMA exception rings/RGU reset | done (pre-delivered cldma leaf) |

## 6. Remaining gaps and exact missing inputs

1. **MASP/HACC secure-algo (RPC 0x2001).** Missing: MediaTek MASP asfv2 driver
   (`masp_hal_secure_algo_init()`, `masp_hal_secure_algo()`,
   `drivers/misc/mediatek/masp/asfv2/**`, HACC key material). Not in the Nura
   tree, not in the downstream kernel tree, not in `vmlinux.elf` symbols.
2. **ADC raw value (RPC 0x4007, second half of 0x4009).** Missing inputs:
   `mediatek,mt6735-auxadc` compatible + `IIO_CHAN_INFO_RAW` in
   `drivers/iio/adc/mt6577_auxadc.c` (the driver's `mt6577_auxadc_read()`
   already returns the 12-bit count, but only `PROCESSED` is exposed), and the
   MT6735 offset calibration source `ADC_CALI_EN_A_REG = EFUSEC_BASE + 0x548`
   (downstream `auxadc/mt6735/mt_auxadc_hw.h:21`, subtracted at
   `mt_auxadc.c:315`) — no EFUSE window/nvmem consumer exists in this tree.
   The recovered DTS fragment includes the controller and channel map.
3. **RF clock buffer (RPC 0x4008, count==4).** Missing: `clk_buf_get_swctrl_status()`
   and its MTK clk-buffer driver (`mediatek,rf_clock_buffer` consumer). The
   count≠4 branch is exact.
4. **DHL RX-alive window.** Missing frozen-API addition
   `void ccci_cldma_stop_for_ee(struct ccci_cldma *cd, u16 stop_mask)` with
   `NONSTOP_QUEUE_MASK 0xF0` (downstream `modem_cldma.c:1449-1520`). Proposal
   is in `logs/rem-ccci-dump.md` §7; the header was not changed (frozen).
5. **AEE database client.** Missing: `aed_md_exception_api()` and the aee/aed
   daemon interface (ROM `0xc0628298`); dumps are currently delivered to the
   kernel log, `/dev/ccci_dump` ring and debugfs, and the ROM ABI
   `aee_dump_ccci_debug_info()` is exported for the client.
6. **Legacy SBP `WM_ID` byte.** Missing: modem load-type source
   (`md->config.load_type`, `get_md_wm_id_map()`, `get_modem_support_cap()`,
   downstream `modem_cldma.c:3357-3360`). No such data in the tree or the
   frozen `struct ccci_md_cfg`.
7. **MD3 / MD1↔MD3 carve-outs.** Missing sizes for
   `mediatek,reserve-memory-ccci_md3_ccif` and `..._ccci_share`
   (keys `ccci_util_lib_fo.c:1228-1229`): absent from downstream DTS (only
   `reserve-memory-ccci_md1` exists), absent from the ROM DTB reserved-memory
   node, and no `md3-smem-size`/`md1md3-smem-size` value exists. The ROM string
   `vmlinux.strings:37118` proves the property is parsed but not its value.
   The MD1↔MD3 window has no consumer in the delivered Nura code, so no dead
   fields were added.
8. **`mediatek,infracfg-ao` DT property** is required by both RMPU programming
   and CLDMA reset; it is in the fragments (§7) but not applied.

## 7. DTS fragments (not applied — coordinator-owned shared DTS)

Validated in the leaf reports: dts leaf §6 (MD1 carve-out + mdcldma + EINT) and
rpc leaf §6 (gpio usage mapping + auxadc + adc channels + EINT) were compiled
with the tree's `cpp`+`scripts/dtc` against a scratch board overlay; exit 0,
only the pre-existing `mt6735.dtsi` warnings. Consolidated, conflict-free set:

```dts
/ {
	reserved-memory {
		/* downstream mt6735.dtsi:197-203; ROM DTB has no ccci node */
		md1_carveout: reserve-memory-ccci_md1 {
			compatible = "mediatek,reserve-memory-ccci_md1";
			no-map;
			size = <0 0x3830000>;		/* md_size + 0x30000 smem */
			alignment = <0 0x2000000>;
			alloc-ranges = <0 0x40000000 0 0xC0000000>;
		};
	};

	/* downstream mt6735.dtsi:528-530, ROM rom_dtb_0.dts:3006-3014 */
	gpio_usage_mapping: gpio-usage-mapping {
		compatible = "mediatek,gpio_usage_mapping";
		GPIO_SIM1_HOT_PLUG = <8>;
		GPIO_SIM2_HOT_PLUG = <9>;
		GPIO_CMDAT1 = <43>;
		GPIO_SIM2_SCLK = <160>;
		GPIO_SIM2_SRST = <161>;
		GPIO_SIM2_SIO = <162>;
		GPIO_SIM1_SCLK = <163>;
		GPIO_SIM1_SRST = <164>;
		GPIO_SIM1_SIO = <165>;
	};
};

&{/soc} {
	/* downstream mt6735.dtsi:386-400 / ROM rom_dtb_0.dts:450-463 */
	mdcldma: mdcldma@1000a000 {
		compatible = "mediatek,mdcldma";
		reg = <0 0x1000a000 0 0x1000>,	/* AP_CLDMA_AO  */
		      <0 0x1000b000 0 0x1000>,	/* MD_CLDMA_AO  */
		      <0 0x1021a000 0 0x1000>,	/* AP_CLDMA_PDN */
		      <0 0x1021b000 0 0x1000>,	/* MD_CLDMA_PDN */
		      <0 0x1020a000 0 0x1000>,	/* AP_CCIF_BASE */
		      <0 0x1020b000 0 0x1000>;	/* MD_CCIF_BASE */
		interrupts = <GIC_SPI 145 IRQ_TYPE_LEVEL_HIGH>,
			     <GIC_SPI 140 IRQ_TYPE_LEVEL_LOW>,
			     <GIC_SPI 221 IRQ_TYPE_EDGE_FALLING>;
		mediatek,md_id = <0>;
		mediatek,cldma_capability = <6>;
		mediatek,md_smem_size = <0x30000>;
		mediatek,infracfg-ao = <&infracfg>;	/* RST0/1 0x30/0x34, RMPU 0x300..0x31c */
		memory-region = <&md1_carveout>;
		firmware-name = "md1img";
		power-domains = <&spm MT6735_POWER_DOMAIN_MD1>;
		status = "okay";
	};

	/* ROM rom_dtb_0.dts:3158-3168; needs the compatible added to
	 * drivers/iio/adc/mt6577_auxadc.c (see §6.2) before it binds.
	 */
	auxadc: adc@11001000 {
		compatible = "mediatek,mt6735-auxadc";
		reg = <0 0x11001000 0 0x1000>;
		interrupts = <GIC_SPI 76 IRQ_TYPE_EDGE_FALLING>;
		clocks = <&pericfg CLK_PERI_AUXADC>;
		clock-names = "main";

		adc_channel: adc-channel {
			compatible = "mediatek,adc_channel";
			mediatek,temperature0 = <0>;
			mediatek,temperature1 = <1>;
			mediatek,adc_fdd_rf_params_dynamic_custom_ch = <12>;
			status = "okay";
		};
	};
};

/* ROM rom_dtb_0.dts:423-434; read by the RPC 0x4005 service */
&pio {
	MD1_SIM1_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM1_HOT_PLUG_EINT-eint";
		interrupts = <0 IRQ_TYPE_LEVEL_HIGH>;
		debounce = <0 100000>;
		dedicated = <0 0>;
		src_pin = <0 1>;
		sockettype = <0 0>;
		status = "okay";
	};

	MD1_SIM2_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM2_HOT_PLUG_EINT-eint";
		interrupts = <1 IRQ_TYPE_LEVEL_HIGH>;
		debounce = <1 100000>;
		dedicated = <1 0>;
		src_pin = <1 2>;
		sockettype = <1 0>;
		status = "okay";
	};
};
```

Notes: `0x186a0 == 100000`; `4 == IRQ_TYPE_LEVEL_HIGH`. The dts leaf drops the
ROM `@0/@1` unit suffixes because the nodes have no `reg`; the RPC lookup
matches the base name. The Nura tree has no `eintc` node — the 0x10005000 EINT
block is part of `pio`. `clocks = <&scpsys SCP_SYS_MD1>` is deliberately not
emitted (no SCP_SYS binding/clock provider); MD1 is held through
`MT6735_POWER_DOMAIN_MD1`. MT6735M alternative: `size = <0 0x3810000>`,
`mediatek,md_smem_size = <0x10000>` (`mt6735m.dtsi:139-144,739`).

## 8. Patch chain / enabling

```sh
cd $M/src/nura-linux            # HEAD 046883c0c
git apply $M/patches/ccci-integration.patch
git apply $M/patches/ccci-gap-scaffold.patch
git apply $M/patches/rem-ccci-integration-full.patch   # == the six gap patches
```

or, on the scaffold tree:

```sh
git apply $M/patches/rem-ccci-integration.patch        # six leaf patches only
```

Kernel config: `CONFIG_MTK_CCCI=y` (selects CLDMA/UTIL/CCMNI);
`CONFIG_MTK_CCCI_C2K=y` optional. Modem firmware `md1img`/`md3img` remains
external.

## 9. Hardware-unverified

No device was attached. Every register write, IRQ and timing below is
source-derived only:

- EMI MPU SMC actually accepted by the EL3 monitor on this unit; region read
  back; RMPU window read back; modem boot with the remapped image/smem.
- RPC round-trip with real modem firmware (response timing, `-EBUSY` retry
  bound, GPIO/ADC/EINT values on hardware).
- Exception record contents written by the modem, the 2000 ms DHL flush
  timing, the RMPU offender offset, and the AEE consumer ABI.
- Reserved-memory base placement/32 MiB alignment and modem visibility of the
  carve-out; EINT polarity/debounce on hardware.
- MD2/C2K power-domain sequencing is from the clock leaf's ROM disassembly.

## 10. Report index

- `logs/rem-ccci-mpu.md`, `logs/rem-ccci-rpc.md`, `logs/rem-ccci-dump.md`,
  `logs/rem-ccci-dts.md`
- pre-delivered: `logs/ccci-gap-cldma.md`, `logs/ccci-gap-clock.md`
- evidence maps: `CCCI_GAP_CONTRACT.md`, `logs/ccci-coordinator.md`
- build logs: `logs/rem-ccci-integration_olddefconfig.log`,
  `logs/rem-ccci-integration_objects.log`,
  `logs/rem-ccci-integration_build.log`
- leaf compile log: `logs/rem-ccci-rpc_build.log`
