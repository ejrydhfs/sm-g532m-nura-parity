# C2K / dual-modem (MD3) — `drivers/misc/mediatek/ccci_c2k/`

Target worktree: `$M/worktrees/ccci-c2k` (base `046883c0c`, kernel 7.0.10, arm64)
Patch: `$M/patches/ccci-c2k.patch`
Build log: `$M/logs/ccci-c2k_build.log`

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Files changed

| File | Purpose |
|---|---|
| `include/linux/ccci.h` | frozen contract header, verbatim from contract §5 **plus one documented append** (`struct ccci_md_hw_ops` + 2 prototypes, see §9) |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k.c` | platform driver for the MD3/C2K node: DT parse, `struct ccci_md_cfg` fill, `ccci_register_modem()`, hardware-ops registration |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.c` | C2K hardware sequencing: boot slave, C2K boot mode/SPM/sleep clocks, C2K sysreset, wake handshake, chip id, register dumps |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k_ch.c` | `enum c2k_channel` ⇄ `enum ccci_ch` map, IRAT coordination, dual-SIM switch-mode storage |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k_sysfs.c` | `/sys/kernel/c2k/*` and `/sys/kernel/debug/ccci_c2k/*` |
| `drivers/misc/mediatek/ccci_c2k/ccci_c2k.h` | private header: register offsets/constants with citations, `struct ccci_c2k` |
| `drivers/misc/mediatek/ccci_c2k/Kconfig` | `MTK_CCCI_C2K` (bool, no cross-leaf `depends on`) |
| `drivers/misc/mediatek/ccci_c2k/Makefile` | four objects |
| `drivers/misc/mediatek/Kconfig` | created: `menu "MediaTek device drivers"` + single `source` + `endmenu` |
| `drivers/misc/mediatek/Makefile` | created: `obj-y += ccci_c2k/` |
| `drivers/misc/Kconfig` | one line: `source "drivers/misc/mediatek/Kconfig"` |
| `drivers/misc/Makefile` | one line: `obj-y += mediatek/` |

No DTS file was touched. No other leaf's directory was touched. `git status --porcelain`
in the worktree lists only the above.

---

## 2. ROM evidence (SM-G532M, `$M/zip/`)

### 2.1 The CBP/SDIO transport is *not* in the ROM build (confirmed)

```
$ grep -ci "modem_sdio\|cbp_sdio\|rawbulk" $M/zip/symbols.txt
0
$ grep -i "c2k" $M/zip/symbols.txt
28142:mtk_wdt_set_c2k_sysrst        # the only c2k symbol
```

The `c2k` matches in `vmlinux.strings` are pin-name tables (`C2K_TCK`, `C2K_TDI`,
`C2K_UART0_TXD`, …), one watchdog string (`C2K_WDT`, line 25711) and the eccci C2K
paths — no SDIO/bulk transport. Conclusion: MD3 is a **second CCIF modem instance**.

### 2.2 ROM strings proving the MD3 CCIF/C2K paths are built in

| `vmlinux.strings` line | String | Meaning |
|---|---|---|
| 37045 | `mediatek,ap_ccif1` | second AP CCIF compatible (eccci) |
| 37046 | `mediatek,ap2c2k_ccif` | **MD3 CCIF node compatible** |
| 37084 | `md3img` | MD3 firmware name |
| 37118 | `mediatek,md1md3-smem-size` | MD1↔MD3 share memory property |
| 37127–37129 | `opt_eccci_c2k`, `opt_c2k_support`, `opt_c2k_lte_mode` | LK/DT-granted C2K options |
| 37219 | `[MTK_ECCCI_C2K]:1` | `CONFIG_MTK_ECCCI_C2K` reported as enabled |
| 37863 | `[name:ccci&][md%d]C2K EE, No need trigger DB` | C2K exception path |
| 38069 | `[name:ccci&][md%d]c2k status ind 0x%02x` | C2K status indication |
| 38134 | `Fatal error (C2K_EXP)` | C2K exception string |

### 2.3 ROM device tree

`$M/zip/rom_dtb_0.dts:3869-3877` (identical in dtb 1–3):

```
mdc2k@3a00b01c {
        compatible = "mediatek,mdc2k";
        reg = <0x3a00b01c 0x10       /* C2K chip id           */
               0x1021c800 0x300      /* MD1 PCCIF window      */
               0x1021d800 0x300>;    /* MD3 PCCIF window      */
        interrupts = <0x00 0xe5 0x02>;   /* SPI 229, EDGE_FALLING (C2K WDT) */
        clocks = <0x14 0x02>;            /* SCP_SYS_MD2 */
        clock-names = "scp-sys-md2-main";
};
```

`$M/zip/rom_dtb_0.dts:4059-4068`: `simswitch { compatible = "mediatek,sim_switch";
pinctrl-names = ..., "sim1_md3_sim2_md1"; }` — the ROM supports SIM1 bound to MD3,
i.e. the C2K slot2 ("world phone") configuration.

Note: the shipped Samsung DTBs carry the `mdc2k` node but not an `ap2c2k_ccif` node,
even though the kernel string exists. The instance is therefore built but instantiated
only from a board DTB that ships the node; the node layout is documented by
`Documentation/devicetree/bindings/ccci/eccci/mediatek,ap2c2k_ccif.txt` (downstream).
This driver accepts both the ROM compatible (`mediatek,ap2c2k_ccif`) and the new-tree
name (`mediatek,mt6735-ccif-md3`).

### 2.4 ROM `mtk_wdt_set_c2k_sysrst` (register semantics confirmed from disassembly)

`llvm-objdump -d --start-address=0xc0715d60 --stop-address=0xc0715e00 vmlinux.elf`:

* reads the TOP_RGU base from the device tree (`of_find_compatible_node` /
  `of_iomap`, strings `mediatek,mt6735-rgu` at `vmlinux.strings:7555`),
* `ldr r2, [r3, #0x18]` → `MTK_WDT_SWSYSRST` at TOP_RGU + 0x18,
* `flag == 1`: `bic r2, r2, #32768` (clear bit 15) `orr r2, r2, #-2013265920`
  (= `0x88000000` key) `str r2, [r3, #0x18]`,
* otherwise: set bit 15 + same key.

This matches downstream `drivers/watchdog/mediatek/wdt/mt6735/mtk_wdt.c:472-504`
(`C2K_SYSRST_SHIFT 15`, `MTK_WDT_SWSYS_RST_KEY 0x88000000` from `mt_wdt.h:78`,
`MTK_WDT_SWSYSRST = base+0x18` from `mt_wdt.h:12`). Implemented 1:1 in
`ccci_c2k_hw.c:c2k_sysrst_set()`.

---

## 3. Downstream references (every register/sequence implemented)

| Implemented item | Downstream reference |
|---|---|
| MD3 = MD_SYS3 / index 2, `cell-index`, `ccif,capability` | `eccci/mt6735/ccif_c2k_platform.c:130-194` |
| CCIF SRAM size 512 | `eccci/mt6735/modem_reg_base.h:43`, `ccif_c2k_platform.c:48,174` |
| CCIF register offsets | `eccci/mt6735/ccif_c2k_platform.h:35-42` |
| `INFRA_AO_C2K_CONFIG 0x330`, `_STATUS 0x334`, `_SPM_CTRL 0x338`, `SLEEP_CLK_CON 0x400` | `eccci/mt6735/ccif_c2k_platform.h:44-52` |
| `ETS_SEL_BIT (1<<13)` | `eccci/mt6735/ccif_c2k_platform.h:54`, used at `ccif_c2k_platform.c:261-268` |
| MD3 boot slave 0x30190000 / key 0x3019379C / en 0x30195488, MD3 RGU 0x30050000 | `eccci/mt6735/modem_reg_base.h:64-71` |
| boot slave values `0x0` / `0x3567C766` / `0xA3B66175` | `eccci/mt6735/modem_reg_base.h:52-54` |
| boot slave write order key→vector→enable | `eccci/mt6735/ccif_c2k_platform.c:251-259` (MD_SYS2 branch) |
| C2K boot mode field `[10:8] = 5` | `eccci/mt6735/ccif_c2k_platform.c:269-275` |
| srcclkena mask `[5:4] = 2` | `eccci/mt6735/ccif_c2k_platform.c:279-288` |
| `SLEEP_CLK_CON` set bits 2,3,12,27 / clear bit 14 | `eccci/mt6735/ccif_c2k_platform.c:292-303` |
| C2K sysreset release | `eccci/mt6735/ccif_c2k_platform.c:310-319` → `mtk_wdt_set_c2k_sysrst(1)` |
| SPM wake pulse + `C2K_STATUS[1]` poll + drop wake | `eccci/mt6735/ccif_c2k_platform.c:326-345` |
| power on = clock `scp-sys-md2-main` | `eccci/mt6735/ccif_c2k_platform.c:376-389` |
| power off = clock gate | `eccci/mt6735/ccif_c2k_platform.c:412-418` |
| MD WD left alone for MD3 (`if (ret == 0 && md->index != MD_SYS3)`) | `eccci/mt6735/ccif_c2k_platform.c:392-396` |
| PMIC VTCXO_1 `pmic_config_interface(0x0A02, 0xA12E, ...)` | `eccci/mt6735/ccif_c2k_platform.c:308-309`; `0x0A02` == `MT6328_VTCXO_1_CON0` (`include/linux/mfd/mt6328/registers.h:383`) |
| C2K chip id `c2k_chip_id_base`, expected `0x020AC000` | `c2k_ccci/mt6735/c2k_platform.c:537-542` (inside `#if 0`), node parse at `:195-200` |
| C2K reset flow (assert → release → wake) | `c2k_ccci/mt6735/c2k_platform.c:599-650` |
| C2K channel map | `eccci/modem_ccif_c2k.c:117-140` |
| C2K↔CCCI translation incl. exception channel | `eccci/modem_ccif_c2k.c:215-267` |
| MD3 port list / names | `eccci/port_cfg.c:126-157`, dispatch `:172-177` |
| CCMNI ability: IRAT always, WORLD_PHONE under `CONFIG_MTK_C2K_SLOT2_SUPPORT`, `irat_md_id = MD_SYS1` | `eccci/port_net.c:279-293` |
| sim switch mode store/get | `eccci/ccci_support.c:45-59`, callers `ccci_core.c:517-527` |
| boot count increment on start | `eccci/modem_ccif_c2k.c:958` (`ccci_modem_boot_count[md->index]++`) |
| start / stop order (fw load → power on → BOOTING → let md go) | `eccci/modem_ccif_c2k.c:911-961`, `:963-981` |

Hardware addresses used for the DT fragment were taken from the downstream platform
DTS (allowed by contract §7) and the ROM DTS:

* `mt6735m.dtsi:1000-1010`: `AP_CCIF1@0x10218000`, `interrupts = <0 139 0x4>`
  (SPI 139, LEVEL_HIGH); `MD_CCIF1@0x10219000`.
* `mt6735m.dtsi:656-659` `INFRACFG_AO@0x10000000`; `:695-700` `SLEEP@0x10006000`;
  `:503` `toprgu@10212000`.
* ROM DTS `mdc2k@3a00b01c` (§2.3) for the chip-id and PCCIF windows, and SPI 229
  for the C2K watchdog.

---

## 4. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| MD3 as a second CCIF modem (no CBP/SDIO) | `md_ccif_probe` + `md_ccif_ops`, `cell-index=2` | absent | platform driver, `CCCI_MD_SYS3`, `name="md3"`, `firmware_name="md3img"` | **done** |
| Modem registration / teardown | `ccci_register_modem(md)` / `md_ccif_remove` | absent | `ccci_register_modem()`; `ccci_unregister_modem()` in remove **and** shutdown | **done** |
| `struct ccci_md_cfg` content | `ccif,major/minor_base/capability`, boot slave, RGU | — | md_id 3, name, fw name, ccif phys/size/irq, boot-slave 3 phys, rgu phys, sram 512, `capability = IRAT \| WORLD_PHONE`, `irat_md_id = SYS1` | **done** |
| Boot slave (key/vector/enable) write | `MD2_*` branch of `md_ccif_let_md_go`; *skipped* for MD_SYS3 (`ccif_c2k_platform.c:188-193`) | — | written before C2K reset release, per contract §7 / assignment | **done** (see §6 [INFERENCE]) |
| C2K boot mode `[10:8]=5` | `ccif_c2k_platform.c:269-275` | — | `c2k_rmw()` in `let_md_go` | **done** |
| ETS_SEL (meta/factory boot) | `is_meta_mode() \|\| FACTORY_BOOT` → set bit 13 | — | DT boolean `mediatek,ets-sel` | **partial** (boot-mode source, see §6) |
| srcclkena `[5:4]=2` | `ccif_c2k_platform.c:279-288` | — | `c2k_rmw()` | **done** |
| `SLEEP_CLK_CON` bits | `ccif_c2k_platform.c:292-303` | — | `c2k_rmw()` set 2,3,12,27 / clear 14 | **done** |
| PMIC VTCXO_1 | `pmic_config_interface(0x0A02,0xA12E)` | — | `regulator_enable(vtcxo1)` in `power_on` (same register: `MT6328_VTCXO_1_CON0`) | **done** |
| C2K sysreset (`mtk_wdt_set_c2k_sysrst`) | TOP_RGU +0x18, bit 15, key 0x88000000 | — | `c2k_sysrst_set()`, release in `let_md_go`, assert+release in `reset` | **done** |
| Wake handshake | SPM_CTRL bit0, poll STATUS bit1, drop bit0 | — | `c2k_wake()` with a 1 s bounded poll | **done** (+ timeout instead of an infinite loop) |
| Clock gating of the second modem | `clk_prepare_enable("scp-sys-md2-main")` | — | optional `clk` `"scp-sys-md2-main"` in power_on/off | **partial** (no `SCP_SYS_MD2` id in the Nura mt6735 clock/SCP driver — see §6) |
| MD watchdog | not disabled for MD3 | — | MD RGU mapped and dumped, never written | **done (parity: no write)** |
| C2K chip id read | `c2k_chip_id_base` read, compare `0x020AC000` (poll loop `#if 0`) | — | `ccci_c2k_hw_read_chip_id()` at init and after wake | **done** |
| C2K channel map (control, audio, data-ppp, mdlog ctrl/data, fs, AT/AT2/AT3, agps, net1-3 + dl_ack, loopback, status) | `c2k_ports[]` | — | same table + names, both directions, exception-channel behaviour | **done** |
| IRAT coordination | `eccci_cc3mni_ops.irat_md_id = MD_SYS1` | — | `ccci_md_set_irat(SYS3, SYS1)` at probe, `irat_state` sysfs | **done** |
| Dual-SIM switch mode storage | `ccci_store/get_sim_switch_mode` | — | `ccci_c2k_sim_switch_mode_set/get()`, sysfs `c2k/sim_switch_mode` | **partial** (storage only; the `CCCI_MD_MSG_CFG_UPDATE` virtual message is core-owned, §6) |
| `c2k/` sysfs + debugfs | none (3.18 had only printk dumps) | — | `md3_state`, `boot_count`, `irat_state`, `sim_switch_mode`, `chip_id`, `md_wdt_irq`, `regs`, `c2k_wdt_status`; debugfs `state`, `regs`, `wdt`, `channels` | **done** |
| MD3 watchdog IRQ handling | `md_cd_wdt_isr` (IRQ 1 in the DT node) | — | `md_wdt_irq` parsed and reported; not requested | **deferred** (not representable in `struct ccci_md_cfg`; MD exception handling is core-owned) |
| `CCCI_IOC_RESET_MD1_MD3_PCCIF` | `port_char.c:981-987` → `reset_md1_md3_pccif()` | — | PCCIF windows mapped and dumped | **deferred** (userspace ioctl on the core char port; clearing MD1's window from MD3 reset would be a behaviour change) |
| CBP / SDIO modem transport | not in the ROM build (§2.1) | — | — | **deferred by design** |
| legacy `dual_ccci` `/dev/ccci_*` ABI | not used by 3.18 eccci | — | — | **deferred by design** |
| CLDMA for MD3 | MD3 uses CCIF only (512 B SRAM, ring buffers in smem are core's) | — | — | **core-owned** |

---

## 5. Compile test

Exact command (contract §4 / assignment), run in `$M/worktrees/ccci-c2k`:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI_C2K
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 \
  drivers/misc/mediatek/ccci_c2k/ccci_c2k.o \
  drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.o \
  drivers/misc/mediatek/ccci_c2k/ccci_c2k_ch.o \
  drivers/misc/mediatek/ccci_c2k/ccci_c2k_sysfs.o
```

Result: `rc=0`, `.config` contains `CONFIG_MTK_CCCI_C2K=y`, all four objects built,
**no warnings**:

```
  CC      drivers/misc/mediatek/ccci_c2k/ccci_c2k.o
  CC      drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.o
  CC      drivers/misc/mediatek/ccci_c2k/ccci_c2k_ch.o
  CC      drivers/misc/mediatek/ccci_c2k/ccci_c2k_sysfs.o
```

An additional `W=1` pass over the same objects is clean; the only finding was a
misplaced kernel-doc block, which was fixed.

The DTS fragment was syntax-checked with the tree's own `cpp` + `scripts/dtc/dtc`
against `mt6737t-samsung-grandpplte.dts`; it compiles and all phandles
(`infracfg`, `scpsys`, `toprgu`, `mt6328_vtcxo_1_reg`) resolve.

No full kernel build was run, so the two appended symbols have no in-tree definition
yet — see §9.

---

## 6. Inferred / hardware-unverified

1. **[INFERENCE] MD3 CCIF IRQ = GIC_SPI 139, LEVEL_HIGH.** `mediatek,ap2c2k_ccif`
   is not present in the shipped Samsung DTBs. The second CCIF instance is
   `AP_CCIF1@0x10218000` in `mt6735m.dtsi:1000-1004` with `<0 139 0x4>`; the ROM DTB
   only proves the C2K *watchdog* line (SPI 229, EDGE_FALLING, `rom_dtb_0.dts:3875`).
   Verify on hardware before enabling the node.
2. **[INFERENCE] MD3 boot slave writes.** Contract §7 and `modem_reg_base.h:64-70`
   describe the MD3 boot-vector block (same addresses/values as the MD2 block), but
   downstream eccci explicitly skips it for MD_SYS3 (`ccif_c2k_platform.c:188-193`,
   "no boot slave for md3"). The driver writes it (assignment + contract §7) *and*
   hands the same addresses to the core, so both writers use identical values; if
   hardware ignores the block this is a no-op.
3. **[INFERENCE] ETS_SEL source.** Downstream sets bit 13 for
   `is_meta_mode() || get_boot_mode() == FACTORY_BOOT`. The 7.0.10 tree has no boot-mode
   provider, so the driver takes a DT boolean `mediatek,ets-sel` (off by default) and
   applies it before the boot-mode write.
4. **Wake timeout.** Downstream spins forever on `C2K_STATUS[1]`; this driver gives up
   after 1 s (`C2K_WAKE_TIMEOUT_US`) and returns `-ETIMEDOUT`, which the core's start
   path sees as a boot failure. The 1 s value is a robustness choice, not a hardware
   constant.
5. **Clock gating is partial.** `SCP_SYS_MD2` exists in the downstream clock bindings
   (`clock-names = "scp-sys-md2-main"`, ROM DTS `mdc2k` node) but the Nura mt6735
   clock/SCP driver only exposes `MT6735_POWER_DOMAIN_MD1`
   (`include/dt-bindings/power/mediatek,mt6735-power-controller.h:6`) and has no
   `SCP_SYS_MD2` gate. The driver therefore takes an **optional** clock
   `"scp-sys-md2-main"`, warns when it is absent, and the fragment leaves the
   `clocks`/`clock-names` lines commented out. The C2K power state is then changed by
   the documented SPM/wake sequence only. Completing this needs a `SCP_SYS_MD2` gate in
   `drivers/soc/mediatek/mtk-scpsys.c` + clock bindings (shared files, out of scope for
   this leaf). The `vtcxo1-supply` is likewise optional (warn if absent): boards that
   enable VTCXO_1 elsewhere still boot.
6. **PMIC equivalence.** Downstream writes the whole 16-bit `0xA12E` into
   `VTCXO_1_CON0`. The regulator framework enables the same register's enable bit
   (`mt6328-regulator.c:335-336`) and applies the 2.8 V setting already present in
   `mt6328.dtsi:103-108`. Register identity is proven (`MT6328_VTCXO_1_CON0 0x0a02`);
   the full 16-bit value is not reproduced bit for bit.
7. **Not implemented (documented gaps, not stubs):**
   * MD3 watchdog IRQ: `struct ccci_md_cfg` carries a single `ccif_irq`; the second
     interrupt is parsed and exported via `c2k/md_wdt_irq` for the core, but no
     handler is installed here.
   * `CCCI_IOC_RESET_MD1_MD3_PCCIF` (`port_char.c:981-987`): the ioctl is part of the
     core char port. The two PCCIF windows are mapped and dumped
     (`c2k/regs`: `pccif: md1_BUSY=… md3_BUSY=…`), but the clearing loop is not run as
     a side effect of MD3 reset — that would also clear MD1's window.
   * `CCCI_MD_MSG_CFG_UPDATE` monitor message after a sim-mode change
     (`ccci_support.c:45-54`): virtual modem messages have no API in the frozen header.
   * CLDMA/ring buffers for MD3: the C2K instance uses only the 512-byte CCIF SRAM; the
     queue/ring engine is the core's.

---

## 7. `struct ccci_md_hw_ops` — appended header symbols (for the coordinator)

Approved by the coordinator. Appended at the end of `include/linux/ccci.h`: lines 1-284
are byte-identical to contract §5 and the closing `#endif /* __LINUX_CCCI_H */` is
unchanged (`diff` verified); only this block was inserted before it:

```c
struct ccci_md_hw_ops {
	int (*power_on)(unsigned int md_id);	/* release power/clocks */
	int (*let_md_go)(unsigned int md_id);	/* after boot vector write */
	int (*power_off)(unsigned int md_id);	/* modem stop */
	void (*reset)(unsigned int md_id);	/* exception recovery */
};

int ccci_register_md_hw_ops(unsigned int md_id, const struct ccci_md_hw_ops *ops);
void ccci_unregister_md_hw_ops(unsigned int md_id);
```

Required core behaviour (from downstream `md_ccif_op_start`/`op_stop`,
`modem_ccif_c2k.c:911-981`): call `power_on` after the firmware is staged and before
any MD register access, `let_md_go` after the boot-vector/key/enable writes and after
the `BOOTING` broadcast, `power_off` in the stop path, `reset` on exception recovery.
Members must be NULL checked; all `int` callbacks return 0 or a negative errno. The
callbacks run in sleepable context (`readl_poll_timeout` with `usleep_range`).

The C2K driver registers the ops in probe (`ccci_register_md_hw_ops(CCCI_MD_SYS3, …)`)
before `ccci_register_modem()`, and unregisters them in remove/shutdown. **It calls the
symbols unconditionally — there is intentionally no `__weak` or polling fallback.**

---

## 8. Channel map (as implemented)

`c2k_ch_map[]` in `ccci_c2k_ch.c` is `c2k_ports[]` of `modem_ccif_c2k.c:117-140` in
order, first match wins, with the MD3 port names from `port_cfg.c:127-155`:

| C2K ch | CCCI tx | CCCI rx | excp | port |
|---|---|---|---|---|
| 0 CTRL | CONTROL_TX | CONTROL_RX | CONTROL_TX | ccci3_ctrl |
| 0 CTRL | STATUS_TX | STATUS_RX | CTRL(0) | ccci3_poll |
| 1 AUDIO | PCM_TX | PCM_RX | AUDIO | ccci3_aud |
| 12 NET1 | CCMNI1_TX | CCMNI1_RX | NET1 | cc3mni0 |
| 12 NET1 | CCMNI1_DL_ACK | CCMNI1_DL_ACK | NET1 | cc3mni0 |
| 13/14 NET2/3 | CCMNI2/3_TX | CCMNI2/3_RX (+DL_ACK rows) | NET2/3 | cc3mni1/2 |
| 3 MDLOG_CTRL | UART1_TX | UART1_RX | — | ccci3_md_log_ctrl |
| 9 MDLOG | MD_LOG_TX | MD_LOG_RX | — | ccci3_tty1 |
| 4 FS | FS_TX | FS_RX | — | ccci3_fs |
| 2 DATA_PPP | C2K_PPP_DATA | C2K_PPP_DATA | — | ccci3_data |
| 5/7/8 AT/AT2/AT3 | C2K_AT/AT2/AT3 | same | — | ccci3_at/at2/at3 |
| 6 AGPS | IPC_UART_TX | IPC_UART_RX | — | ccci3_tty2 |
| 256 MD2AP_LOOPBACK | C2K_LB_DL | C2K_LB_DL | — | ccci3_lb_dl |
| 255 LOOPBACK | LB_IT_TX | LB_IT_RX | — | ccci3_lb_it |
| 11 STATUS | CONTROL_TX | CONTROL_RX | STATUS | ccci3_poll |

`LOOPBACK_C2K = 255` and `MD2AP_LOOPBACK_C2K = 256` are defined locally in
`ccci_c2k_ch.c` because the frozen enum ends at `NET5_CH_C2K` (downstream
`mt_ccci_common.h:502-503`). `ccci_c2k_ch_to_ccci()` / `ccci_c2k_ccci_to_ch()` are
exported (`EXPORT_SYMBOL_GPL`) for the core's MD3 port layer; the map is also visible
at `/sys/kernel/debug/ccci_c2k/channels`.

`skb` ownership: this leaf never touches skbs — it only translates channel ids and
sequences hardware, so the core/CCMNI skb contract is unaffected.

---

## 9. DTS-FRAGMENT

Place inside the existing `soc { … }` node of
`arch/arm64/boot/dts/mediatek/mt6735.dtsi` (or in the board DTS as an
`&{/soc} { … };` overlay). Not applied by this leaf.

```dts
		ccif_md3: ccif@10218000 {
			compatible = "mediatek,ap2c2k_ccif",
				     "mediatek,mt6735-ccif-md3";
			reg = <0 0x10218000 0 0x1000>,	/* AP CCIF, MD3 instance */
			      <0 0x1021c800 0 0x300>,	/* MD1 PCCIF window */
			      <0 0x1021d800 0 0x300>,	/* MD3 PCCIF window */
			      <0 0x3a00b01c 0 0x0010>,	/* C2K chip id */
			      <0 0x30190000 0 0x0004>,	/* MD3 boot vector */
			      <0 0x3019379c 0 0x0004>,	/* boot vector key */
			      <0 0x30195488 0 0x0004>,	/* boot vector enable */
			      <0 0x30050000 0 0x0040>;	/* MD3 RGU */
			reg-names = "ap-ccif", "md1-pccif", "md3-pccif",
				    "chip-id", "boot-vector", "boot-vector-key",
				    "boot-vector-en", "md-rgu";
			interrupts = <GIC_SPI 139 IRQ_TYPE_LEVEL_HIGH>,	/* C2K CCIF */
				     <GIC_SPI 229 IRQ_TYPE_EDGE_FALLING>; /* C2K WDT */
			interrupt-names = "ccif", "md-wdt";

			cell-index = <2>;
			ccif,capability = <0x00180000>;	/* IRAT | WORLD_PHONE */

			mediatek,infracfg = <&infracfg>;	/* C2K_CONFIG/STATUS/SPM_CTRL */
			mediatek,spm = <&scpsys>;	/* SLEEP_CLK_CON */
			mediatek,toprgu = <&toprgu>;	/* C2K sysreset */
			vtcxo1-supply = <&mt6328_vtcxo_1_reg>;

			/*
			 * Enable only for meta/factory boot (downstream
			 * is_meta_mode() || FACTORY_BOOT):
			 * mediatek,ets-sel;
			 *
			 * Enable once the mt6735 clock/SCP driver exposes
			 * SCP_SYS_MD2:
			 * clocks = <&scpsys SCP_SYS_MD2>;
			 * clock-names = "scp-sys-md2-main";
			 */

			status = "okay";
		};
```

Notes:
* `interrupts[0]` (SPI 139 LEVEL_HIGH) is the second CCIF line from
  `mt6735m.dtsi:1000-1004` — **[INFERENCE]**, see §6.1; `interrupts[1]` (SPI 229
  EDGE_FALLING) is the C2K watchdog line of the ROM `mdc2k` node.
* The MD3 boot slave/RGU `reg` entries are redundant with the driver's built-in
  defaults (`modem_reg_base.h:64-71`); keep them if a board moves the windows.
* `mediatek,md_smem_size`, `ccif,major` and `ccif,minor_base` of the downstream
  binding are deliberately absent: the share-memory layout and the char-device minor
  base belong to the core, whose `struct ccci_md_cfg` has no field for them.

---

## 10. Patch

```
$M/patches/ccci-c2k.patch        (staged with `git add -A && git diff --cached --binary`)
```

Worktree `git status --porcelain` (everything staged, nothing unstaged):

```
M  drivers/misc/Kconfig
M  drivers/misc/Makefile
A  drivers/misc/mediatek/Kconfig
A  drivers/misc/mediatek/Makefile
A  drivers/misc/mediatek/ccci_c2k/Kconfig
A  drivers/misc/mediatek/ccci_c2k/Makefile
A  drivers/misc/mediatek/ccci_c2k/ccci_c2k.c
A  drivers/misc/mediatek/ccci_c2k/ccci_c2k.h
A  drivers/misc/mediatek/ccci_c2k/ccci_c2k_ch.c
A  drivers/misc/mediatek/ccci_c2k/ccci_c2k_hw.c
A  drivers/misc/mediatek/ccci_c2k/ccci_c2k_sysfs.c
A  include/linux/ccci.h
```

`git apply --check --reverse $M/patches/ccci-c2k.patch` succeeds, i.e. the patch is
exactly the worktree content. Nothing was committed.
