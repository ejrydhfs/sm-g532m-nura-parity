# CCCI gap-closure contract (coordinator-owned, read-only for leaves)

Subsystem: CCCI modem stack, SM-G532M / MT6737T ROM (`3.18.19-14373813`) recreated on
the Nura `v7.0.10-mt6735` arm64 tree.
Read first: `$M/PARITY_CONTEXT.md`, `$M/CCCI_CONTRACT.md`,
`$M/logs/ccci-coordinator.md`, `$M/patches/ccci-gap-scaffold.patch`.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Worktree state (already prepared — do not recreate, do not reset)

Every gap worktree already contains two coordinator commits on top of Nura
`046883c0c`:

1. `ccci: integration baseline` — `$M/patches/ccci-integration.patch` applied
   (5 CCCI drivers + `include/linux/ccci.h`).
2. `ccci: gap scaffold` — coordinator-owned additions:
   - `include/linux/ccci.h`: the frozen gap API (search for `Gap-closure API`),
   - `ccci_priv.h`: `md->infra_ao`, `infra_ao_phys`, `infra_ao_size`,
   - `ccci_core.c`: `ccci_md_map_infra_ao()` resolving the optional
     `mediatek,infracfg-ao` phandle into `md->infra_ao`.

Each worktree also contains partial work from the aborted first run (see the
per-leaf task). **Continue that work; do not restart from scratch.** Do not
commit. Do not `git checkout` / `git reset` / `git stash`.

| Leaf | Worktree | Patch | Report |
|---|---|---|---|
| mpu | `$M/worktrees/ccci-gap-mpu` | `$M/patches/ccci-gap-mpu.patch` | `$M/logs/ccci-gap-mpu.md` |
| rpc | `$M/worktrees/ccci-gap-rpc` | `$M/patches/ccci-gap-rpc.patch` | `$M/logs/ccci-gap-rpc.md` |
| dump | `$M/worktrees/ccci-gap-dump` | `$M/patches/ccci-gap-dump.patch` | `$M/logs/ccci-gap-dump.md` |
| cldma | `$M/worktrees/ccci-gap-cldma` | `$M/patches/ccci-gap-cldma.patch` | `$M/logs/ccci-gap-cldma.md` |
| clock | `$M/worktrees/ccci-gap-clock` | `$M/patches/ccci-gap-clock.patch` | `$M/logs/ccci-gap-clock.md` |

Never edit `$M/worktrees/integration`, `$M/worktrees/ccci*` (non-gap), another
leaf's gap worktree, `$M/nura.config`, or shared DTS files.

## 2. Deliverables

Patch (diff against the worktree HEAD, i.e. baseline+scaffold only):

```sh
cd "$M/worktrees/ccci-gap-<leaf>"
git add -A
git diff --cached --binary > "$M/patches/ccci-gap-<leaf>.patch"
```

`git status --porcelain` must list only files belonging to your gap. Report
`$M/logs/ccci-gap-<leaf>.md` per §7.

## 3. Build

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/ccci-gap-<leaf>"
cp "$M/nura.config" .config
make ARCH=arm64 LLVM=1 olddefconfig
rm -f <each changed .o>            # force a real rebuild
make ARCH=arm64 LLVM=1 -j2 <each changed .o>
```

Compile every object you touch, plus the directory `built-in.a` target if you
added files. A full `vmlinux` build is the coordinator's job; do not run one.
Record the exact command and result in the report. `W=1` on your own objects is
expected to be clean (pre-existing warnings excluded, name them).

## 4. Shared files and the frozen API

- `include/linux/ccci.h` is **frozen**: implement the declared signatures
  exactly. If a declaration is genuinely insufficient, implement to it and add a
  `SCAFFOLD CHANGE REQUIRED: <exact proposal>` section to the report. Never
  silently change an existing declaration. Only the clock leaf may add new
  binding headers (`include/dt-bindings/...`).
- `ccci_core.c`, `ccci_priv.h`, `ccci_boot.c`,
  `drivers/misc/mediatek/ccci/{Kconfig,Makefile}` are touched by several
  leaves. Keep edits additive, minimal, and in one clearly-commented block; do
  not reformat or reorder existing code. Conflict resolution at integration is
  the coordinator's job.
- Do not modify `include/linux/ccci.h`'s scaffold block either (the `md->infra_ao`
  mapping stays as is). Consumers use `md->infra_ao` / `md->infra_ao_phys`.
- No shared DTS edits. Put exact fragments under `DTS-FRAGMENT` in the report;
  fragments must be valid against the Nura tree (see `ccci-coordinator.md` §10:
  `&{/soc}`, `<0 ADDR 0 SIZE>` four-cell `reg`, lowercase unit address).

## 5. Cross-leaf symbols (link-time contract, do not duplicate)

| Symbol | Implemented by | Consumed by |
|---|---|---|
| `ccci_md_set_dsp_mpu_protection()` | mpu leaf (`ccci_mpu.c`) | rpc leaf (op `0x400B`) |
| `ccci_mpu_*`, `ccci_init_security()` | mpu leaf (private header) | mpu leaf core wiring |
| `ccci_rpc_register_service()` / `ccci_rpc_unregister_service()` | rpc leaf | kernel users / rpc leaf built-ins |
| `aee_dump_ccci_debug_info()` | dump leaf | AEE consumers |
| `ccci_cldma_set_infra_ao/_hw_reset/_set_exception_mode` | cldma leaf | cldma leaf + core exception flow |

A leaf may reference another leaf's symbol; object compilation does not need it.
Never provide a stub definition for a symbol another leaf owns.

## 6. Evidence rules

- Sources: downstream `$M/src/downstream-grandpplte` (primary), ROM
  `$M/zip/symbols.txt` / `vmlinux.strings` / `vmlinux.elf` (disassembly:
  `$M/llvmpkg/usr/bin/llvm-objdump -d --start-address=0x… --stop-address=0x…`),
  then Nura tree.
- Cite `file:line` for every imported constant, op id, register offset, bit and
  timing. Anything not directly cited is `[INFERENCE]`.
- **Never invent** register values, op ids, GPIO numbers, ADC channels or
  timing. **Never** deliver stubs, `TODO: implement`, placeholder probes, fake
  success, or a handler that returns made-up data.
- Hardware-only unknowns are documented, not simulated.
- Keep GPL notices; add `Port from Samsung/MTK downstream …` comments.

If a sub-feature needs data absent from every available source: implement the
parts that are supported, return the downstream failure code for the rest
(log it), and state the exact missing input in the report.

## 7. Required report structure

`$M/logs/ccci-gap-<leaf>.md`:

1. Gap, files changed, worktree/HEAD, one-line result.
2. ROM + downstream evidence table with exact citations (symbol/address,
   file:line).
3. Parity matrix: behaviour / ROM+downstream implementation / Nura before /
   after / status (`done`, `partial`, `deferred`).
4. What is compile-tested: exact commands, objects, exit codes, warnings.
5. `[INFERENCE]` and hardware-unverified items.
6. `DTS-FRAGMENT` (exact text, not applied) or "none".
7. `SCAFFOLD CHANGE REQUIRED` if any.
8. Patch path and `git status --porcelain` file list.

## 8. Coordinator evidence map (verified; reuse, do not re-derive)

### 8.1 MPU / RMPU / security
- `drivers/misc/mediatek/eccci/mt6735/ccci_platform.c:50-120` — region ids and
  access attributes (`SET_ACCESS_PERMISSON`), MT6735 (non-`CONFIG_ARCH_MT6735M`)
  variant: SEC_OS 0, ATF 1, MD1_SEC_SMEM 3, MD2_SEC_SMEM 4, MD1_ROM 5,
  MD1_DSP 6, MD2_ROM 7, MD2_RW 8, MD1_SMEM 9, …; read the file for the rest.
- `ccci_platform.c:327-...` — `ccci_set_mem_access_protection()` (ROM/RW/SMEM
  regions per MD), `ccci_clear_md_region_protection()`,
  `ccci_set_dsp_region_protection()`, `ccci_set_ap_region_protection()`,
  `ccci_set_mem_remap()` (RMPU `BANKn_MAPx`).
- `drivers/misc/mediatek/emi_mpu/mt6735/emi_mpu.c:1138`
  `emi_mpu_set_region_protection()` — real register/API shape;
  `include/mt-plat/mt6735/include/mach/emi_mpu.h` for the API and
  `SET_ACCESS_PERMISSON` encoding; `mt-plat/.../mach/mt_secure_api.h` +
  `mtk_secure_api.c` for the EL3 SMC path (search `MTK_SIP_KERNEL_EMIMPU_SET`
  and `EMI_MPU_SET_PROTECTION`).
- `drivers/misc/mediatek/eccci/ccci_util/…`/`ccci_util_lib_load_img.c` —
  `ccci_init_security()`/secure-image checks (search `ccci_init_security`,
  `masp_ccci_version_info`, `secro`).
- `ccci_util_lib_main.c`/`ccci_util_lib_fo.c` for `ccci_set_mem_access_protection`
  call sequencing. Baseline Nura has no MPU code; the ROM has
  `emi_mpu_mod_init`/`mpu_violation_irq` strings and `masp_ccci_*` symbols.
- SMC convention for MT6735 (downstream `mt_secure_api.c`):
  `MTK_SIP_KERNEL_EMIMPU_SET` request id; verify the exact id/args from that
  file before using. If the tree has no `arm,psci`/`arm,smc` conduit, use
  `arm_smccc_smc()` only when the DT provides a conduit; otherwise program the
  EMI MPU registers directly from the DT node and document the choice.

### 8.2 Kernel RPC
- Downstream op ids: `eccci/port_kernel.h:107-146` (`RPC_OP_ID`), wire format
  `port_kernel.h:147-186`, payload structs `port_kernel.h:187-300`.
- Handlers: `eccci/port_kernel.c:1211-1900` (`ccci_rpc_work_helper()` and
  `ccci_rpc_get_gpio_adc[_v2]` at 1042/1104, `ccci_rpc_remap_ch_queue` at 1166,
  EINT-attr DT reader at 860-980, `get_dtsi_eint_node()`).
- Userspace RPC engine reference (for buffer/workqueue shape only; the ROM
  build has no `/dev/ccci_rpc`): `dual_ccci/ccci_rpc_main.c` (695 lines) and
  `dual_ccci/include/ccci_rpc.h`.
- ROM proof: symbol `ccci_rpc_work_helper` (symbols.txt:21165) and strings
  `IPC_RPC_GET_GPIO_ADC_OP`, `IPC_RPC_GET_RF_CLK_BUF`,
  `IPC_RPC_DSP_EMI_MPU_SETTING`, `RPC_GET_SECRO_OP`, `RPC_SECURE_ALGO_OP`,
  `kernelspace rpc msg 0x%x on %s`, `ccci_rpc_remap_ch_queue`,
  `ccci_rpc_get_gpio_adc`.
- Nura providers: `drivers/pinctrl/mediatek/pinctrl-mt6735.c` (gpiochip),
  `drivers/iio/adc/mt6577_auxadc.c` (IIO auxadc; no DT node in
  `mt6735.dtsi` yet), no EINT provider (EINT-attr service must come from DT,
  as downstream does). GPIO pin name → number mapping downstream comes from DT
  (`mediatek,gpio_...` names, see `get_md_gpio_info()`); reproduce with
  `of_property_read_u32` on the modem node or `gpio_get_named`, never with a
  hardcoded table. "time" = the `MISC_INFO_CLIB_TIME` runtime feature; baseline
  `ccci_boot.c:745` already answers it — verify, don't duplicate.
- Baseline Nura drops RPC: `ccci_port_kernel.c:381-385`.

### 8.3 AEE / SBP / DHL
- `aee_dump_ccci_debug_info()` — `eccci/ccci_core.c:580` (md_id is 1-based,
  returns `ccci_exp_smem_ccci_debug_vir/size`).
- Exception notify/stage: `modem_cldma.c:1986-2030` (`md_cd_exception`,
  `HIF_EX_*`, `EX_DHL_DL_RDY`), `modem_ccif.c:405/584`, `port_char.c:85,577`.
- DHL: `port_char.c:577` (`"DHL"` protocol open), `modem_cldma.c:2015`
  (flush delay); SBP: `modem_cldma.c:3210-3360` (`md_cd_send_runtime_data*`,
  `MISC_MD_SBP_SETTING`, `feature_4_val[0]`), `port_char.c:648,914`.
- Fatal debug record: `eccci/port_kernel.c:2752` (`EMI_MPU_VIOLATION` offset
  `0x30`), `ccci_core.h:246,352` (`EMI_MPU_VIOLATION`, `ECT_SRC_RMPU`).
- ROM symbols: `aee_dump_ccci_debug_info`, `aee_Handle`, `aed_md_exception_api`
  (strings `Modem exception dump start…`).
- Baseline Nura: `ccci_port_kernel.c:246-290` sets `md->ex_stage` and
  `CCCI_EX_DHL_DL_RDY`; `ccci_boot.c` already sends SBP id. Dump leaf owns the
  exception smem layout + AEE export + dump engine.

### 8.4 CLDMA exception mode + RGU
- Hardware reset: `eccci/mt6735/cldma_platform.c:55-71` —
  `INFRA_RST0_REG 0x0030` set mask `CLDMA_AO_RST_MASK|CLDMA_PD_RST_MASK`, then
  `INFRA_RST1_REG 0x0034` same mask (clear). Masks in
  `eccci/mt6735/cldma_reg.h` — read them, do not guess.
- Ring switch: `modem_cldma.c:1094-1133` (`cldma_queue_switch_ring`), masks at
  `modem_cldma.c:120-130`; normal/exception ring arrays in `modem_cldma.h`.
- Exception flow: `modem_cldma.c:1986-2030` (`md_cd_exception()` stages
  `HIF_EX_INIT` → `HIF_EX_INIT_DONE` → `HIF_EX_CLEARQ_DONE` →
  `HIF_EX_ALLQ_RESET`), boot exception handshake `modem_cldma.c:2040-2110`.
- RGU: `eccci/mt6735/modem_reg_base.h:28` `MD_RGU_BASE 0x20050000`
  (`MD2/MD3_RGU_BASE 0x30050000`), watchdog ack `WDT_MD_STA`/`WDT_MD_MODE`/
  `WDT_MD_MODE_KEY` in `modem_ccif.h`/`cldma_reg.h`; reset entry point
  `modem_cldma.c:2472` `md_cd_reset()`, dump `modem_cldma.c:1847-1856`.
- Baseline Nura CLDMA has normal-mode rings only; the partial edit in the
  worktree adds `CLDMA_NORMAL_QUEUE_MASK 0xCF` with the citation already.

### 8.5 MD3 clock + DTS carve-outs
- ROM DTB `$M/zip/rom_dtb_0.dts:307-313`: `scpsys@10000000` compatible
  `mediatek,mt6735-scpsys`, two reg windows (`0x10000000`, `0x10006000`),
  `#clock-cells = <1>`, phandle 0x14. Line 3873: `mdc2k clocks = <0x14 0x02>`
  (`SCP_SYS_MD2`); line 458: `mdcldma clocks = <0x14 0x01>` (`SCP_SYS_MD1`).
- Binding: downstream `include/dt-bindings/clock/mt6735-clk.h:229-237`
  (`SCP_SYS_MD1` 1 … `SCP_SYS_VEN` 8). Downstream gate table:
  `drivers/clk/mediatek/clk-mt6735-pg.c:955-956` (`PGATE(SCP_SYS_MD1, pg_md1, …,
  SYS_MD1)`, same for MD2), SPM status `0x060c/0x0610`, power bits at lines
  60-120, subsystem table at 181-200.
- Nura has no `clk-mt6735-pg.c` and no SCP_SYS binding; `spm` node is a
  power-domain provider (`mt6735.dtsi:186`). Existing delivered leaf patch adds
  `MT6735_POWER_DOMAIN_MD2` and holds it via runtime PM.
- Carve-outs **are** in the sources: downstream `arch/arm64/boot/dts/mt6735.dtsi:197-203`
  `reserve-memory-ccci_md1`: `no-map; size = <0 0x3830000>;
  alignment = <0 0x2000000>; alloc-ranges = <0 0x40000000 0 0xC0000000>;`
  and `mt6735m.dtsi:139-144` `size = <0 0x3810000>`, `smem 0x10000` at
  `mt6735m.dtsi:739`; `mt6735.dtsi:399` `mediatek,md_smem_size = <0x30000>`.
  ROM strings `mediatek,md_smem_size` / `mediatek,md1md3-smem-size` prove both
  properties are parsed. MD3/C2K ROM node `rom_dtb_0.dts:3869-3875`.
  The Nura tree reserves memory via `reserved-memory` + `memory-region`; the
  carve-out fragment must use the downstream size/alignment/alloc-ranges, not
  invented addresses.
