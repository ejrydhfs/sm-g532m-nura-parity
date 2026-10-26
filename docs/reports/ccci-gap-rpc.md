# CCCI gap — kernel RPC engine and built-in modem services

`M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Worktree `$M/worktrees/rem-ccci-rpc`, branch `rem-ccci-rpc`, HEAD `509789971`
("ccci: gap-closure scaffold", parent `e4a4be64e ccci: integration baseline`).
Continued from the aborted first run: the untracked 1333-line `ccci_rpc.c` and
204-line `ccci_rpc.h` draft were audited line by line against the downstream
sources, corrected, re-wired and finally compiled. Nothing was committed; the
worktree is left dirty at HEAD.

## 1. Gap, files changed, result

**Gap.** The baseline kernel port had no RPC engine at all: `ccci_port_kernel.c`
logged and dropped every `CCCI_RPC_RX` packet, the frozen
`ccci_rpc_register_service()` API had no implementation, and the RX dispatch
delivered RPC packets to the userspace char port `ccci_rpc` because the demux
rule of downstream `port_char_req_match()`/`port_kernel_req_match()` was not
reproduced. The modem's GPIO/ADC/EINT/DSP-MPU/queue-remap requests therefore
never got an answer.

**Files changed**

| File | Change |
|---|---|
| `drivers/misc/mediatek/ccci/ccci_rpc.c` | new, 1389 lines: wire parser, service registry, 14 op-id registrations over 11 built-in handlers, GPIO/ADC/EINT providers, response framing |
| `drivers/misc/mediatek/ccci/ccci_rpc.h` | new, 211 lines: op ids, wire structs, payload structs, FS codes, engine prototypes |
| `drivers/misc/mediatek/ccci/Makefile` | `+ ccci_rpc.o` in `ccci-y` |
| `drivers/misc/mediatek/ccci/ccci_core.c` | `#include "ccci_rpc.h"`; RPC demux in `ccci_rx_dispatch_qno()`; `ccci_rpc_init()`/`ccci_rpc_exit()` in module init/exit |
| `drivers/misc/mediatek/ccci/ccci_port_kernel.c` | `#include "ccci_rpc.h"`; `CCCI_RPC_RX` case calls `ccci_rpc_handle(port, skb)` |

**Result.** The engine parses the downstream wire format, dispatches by op id,
and every op id either reads real data from the tree (GPIO through the mt6735
pinctrl, ADC channel numbers through the `mediatek,adc_channel` node, EINT
attributes through the SIM hot-plug DT nodes), calls a cited cross-leaf API
(`0x400B` → frozen `ccci_md_set_dsp_mpu_protection()`), or returns the downstream
error code for that op. ADC *value* reads and the RF clock-buffer *status* half
cannot be produced with the providers in this tree and say so
(`FS_NO_FEATURE` / `FS_FUNC_FAIL`); the secure-algo transform needs the MASP/HACC
driver and returns `FS_NO_FEATURE`. Op `0x2002` is at exact parity because the
downstream asfv2 build itself answers "secro disabled".

## 2. ROM + downstream evidence

### 2.1 Firmware ABI

| Item | Evidence |
|---|---|
| Op ids `0x2001`, `0x2002`, `0x4001`-`0x400c`, `0x4321` | `eccci/port_kernel.h:107-146` (`RPC_OP_ID`); `0x400a` at `:141`, `0x400b` `:142`, `0x400c` `:143` |
| Wire request `{ccci_header, op_id, para_num, [len,buf]…}` | `port_kernel.h:148-159` (`rpc_pkt`, `rpc_buffer`) |
| Arg count limits `RPC_REQ_BUFFER_NUM 2`, `RPC_MAX_ARG_NUM 6`, `RPC_MAX_BUF_SIZE 2048`, `RPC_API_RESP_ID 0xFFFF0000` | `port_kernel.h:239-242` |
| `FS_*` result codes | `port_kernel.h:244-251` |
| GPIO/ADC payload structs (v1 3 pins, v2 10 pins), name lengths 34/33 | `port_kernel.h:168-218` |
| `ccci_rpc_dsp_emi_mpu_input`, `ccci_rpc_usim2nfs`, `ccci_rpc_queue_mapping`, `ccci_rpc_clkbuf_result` | `port_kernel.h:161-166`, `:219-237` |
| EINT query types/errors, `MD_SIM_MAX 16` | `port_kernel.h:259-288` |
| Response rewrite: `op_id \|= RPC_API_RESP_ID`, channel `CCCI_RPC_TX`, `data[1] = data_len` | `port_kernel.c:1963`, `:1994-1995` |
| Request sanity check and parse | `port_kernel.c:1942-1952` |
| Response is a blocking send | `port_kernel.c:2001` (`req->blocking = 1`) |

### 2.2 ROM proof (disassembly / strings / symbols)

| Item | Evidence |
|---|---|
| Symbol `ccci_rpc_work_helper` | `llvm-nm zip/vmlinux.elf` → `c0514bbc t ccci_rpc_work_helper` (`symbols.txt:21165`) |
| Op dispatch in the ROM | `llvm-objdump -d --mcpu=cortex-a7 --start-address=0xc0514c18`: `movw r3, #0x4007` (`:0xc0514c18`), `movw r3, #0x4006` (`:0xc0514c2c`, `beq 0xc0515234`), `movw r10, #0x4003` (`:0xc0514c38`), `movw r3, #0x2001` (`:0xc0514c4c`, `beq 0xc051504c`), `add r3, r3, #1` → `0x2002` (`:0xc0514c58`), `movw r3, #0x400b` (`:0xc0514ce8`), `movw r3, #0x400c` (`:0xc0514cf8`), `movw r3, #0x4321` (`:0xc0514d04`) |
| Secro handler compiled in and calls the MASP predicate | `bl 0xc04d1fb4 <masp_secro_en>` at `0xc0514c74` |
| `get_dtsi_eint_node` / `get_eint_attr_DTSVal` in the ROM | `llvm-nm`: `c0514744 T get_dtsi_eint_node`, `c0514abc T get_eint_attr_DTSVal` |
| RPC demux string | `vmlinux.strings:37794` `[name:ccci&][md%d]kernelspace rpc msg 0x%x on %s` |
| Secro-disabled string | `vmlinux.strings:37975` `RPC_GET_SECRO_OP: secro disable` |
| GPIO/ADC handler strings | `vmlinux.strings:37998` `IPC_RPC_GET_GPIO_ADC_OP request=%x`, `:38001` `…reqMask=%x`, `:37892` `FEATURE_GET_MD_ADC_VAL,ret=%d, val=%d`, `:37889` `IMM_get_adc_channel_num, %d` |
| DT property names compiled in | `vmlinux.strings:26244` `mediatek,adc_channel`, `:37870` `mediatek,gpio_usage_mapping` |

### 2.3 Build-feature facts for this ROM

| Item | Evidence |
|---|---|
| GPIO num/val, ADC num/val, EINT attr (+DTS) enabled | `eccci/mt6735/ccci_config.h:20-26` |
| TDD EINT num and DRAM type disabled | `eccci/mt6735/ccci_config.h:34-37` (`#if 0 /*DEPRECATED*/`) |
| RF clock buffer enabled | `eccci/mt6735/ccci_config.h:57` |
| Security features enabled | `eccci/Makefile:25-31` (`-DENABLE_MD_IMG_SECURITY_FEATURE`, `-DMTK_SEC_MODEM_NVRAM_ANTI_CLONE`), `arch/arm/configs/grandpplte_00_defconfig:1410-1411` |
| MASP variant = asfv2 | `masp/Makefile:24` (`asf_link := asfv2`) |
| `masp_secro_en()` and `masp_secro_md_len()` are constant 0 in asfv2 | `masp/asfv2/core/sec_legacy.c:26-29`, `:42-45` |
| `masp_secure_algo*()` are real HACC calls | `masp/asfv2/core/sec_ccci.c:23-43` → `mach/hacc_hk.c` |
| `FEATURE_INFORM_NFC_VSIM_CHANGE` undefined | no definition anywhere in the tree (`grep` over `drivers/misc/mediatek/eccci`) |
| `CONFIG_MTK_TC1_FEATURE` not in this build | `grandpplte_00_defconfig:1275` has `CONFIG_MTK_TC10_FEATURE` only; no `CONFIG_MTK_TC1_FEATURE` entry |

### 2.4 GPIO

| Item | Evidence |
|---|---|
| `get_md_gpio_info()`: name table → DT property, else raw property, node `mediatek,gpio_usage_mapping` | `port_kernel.c:832-864`; table `:81-92` |
| `get_md_gpio_val()` | `port_kernel.c:771-782` |
| Base DTS node | downstream `arch/arm64/boot/dts/mt6735.dtsi:528-530`; ROM `zip/rom_dtb_0.dts:3004-3015` |
| Runtime values (SIM1=8, SIM2=9, CMDAT1=43, SIM2 SCLK/SRST/SIO=160/161/162, SIM1=163/164/165) | `zip/rom_dtb_0.dts:3006-3014` |
| FDD band-support pins on the three other DTB variants (2nd=59, 1st=106, 3rd=107) | `zip/rom_dtb_1.dts:3033-3035` (identical in `rom_dtb_2.dts`, `rom_dtb_3.dts`) |
| Board DTSI source of the same numbers | `arch/arm/boot/dts/mt6737t_grandpplte_ltn_open_gpio_01.dtsi:586-596` |
| Nura gpiochip: compatible, 203 lines, dynamic base | `drivers/pinctrl/mediatek/pinctrl-mt6735.c:862`, `pinctrl-moore.c:575-576`, `arch/arm64/boot/dts/mediatek/mt6735.dtsi:295` (`gpio-ranges = <&pio 0 0 203>`) |
| Nura get op | `pinctrl-moore.c:482-495` (`mtk_gpio_get`), `:571` |
| Lookup APIs used | `include/linux/gpio/driver.h:628` (`gpio_device_find_by_fwnode`), `:806-807` (`gpio_device_get_desc`), `:808` (`gpio_device_get_chip`) |

### 2.5 ADC

| Item | Evidence |
|---|---|
| `get_md_adc_info()` → `IMM_get_adc_channel_num()`; `get_md_adc_val()` → `IMM_GetOneChannelValue()` | `port_kernel.c:816-829`, `:784-805` |
| Channel-name list built from the `mediatek,adc_channel` node properties | `auxadc/mt_auxadc.c:1645-1718` |
| Names used by the modem side | `auxadc/mt_auxadc.c:1650-1718`; board file `arch/arm64/boot/dts/cust_adc.dtsi:12-20` |
| Runtime values (temperature0=0, temperature1=1, fdd_rf=12) | `zip/rom_dtb_0.dts:3161-3168` |
| Auxadc controller node | `zip/rom_dtb_0.dts:3158-3165`, downstream `arch/arm64/boot/dts/mt6735.dtsi:677-683` |
| In-tree IIO provider: PROCESSED only, no mt6735 compatible | `drivers/iio/adc/mt6577_auxadc.c:64-69`, `:184-210`, `:313-320` |

### 2.6 EINT attributes

| Item | Evidence |
|---|---|
| Property/​cell-index table `md_eint_struct[]` | `port_kernel.c:882-892` |
| Node table `md_eint_node[]` (MD1 SIM1-4) | `port_kernel.c:894-909` |
| `get_eint_attr_val()`: debounce µs→ms, polarity/sensitivity from one cell | `port_kernel.c:920-968` |
| `get_dtsi_eint_node()` (called once at subsystem init) | `port_kernel.c:972-998`, `ccci_subsys_kernel_init()` `:3459-3463` |
| `get_eint_attr_DTSVal()` | `port_kernel.c:1000-1026` |
| Runtime values SIM1 `interrupts=<0 4>`, `debounce=<0 0x186a0>`, `dedicated=<0 0>`, `src_pin=<0 1>`, `sockettype=<0 0>` | `zip/rom_dtb_0.dts:423-434` |
| Board source | `mt6737t_grandpplte_ltn_open_gpio_01.dtsi:599-617` |

### 2.7 Queue remap, DSP MPU, secure services

| Item | Evidence |
|---|---|
| `ccci_rpc_remap_ch_queue()`: `txq_index = q`, DL-ACK `txq_exp_index = q | 0xF0` | `port_kernel.c:1166-1205` |
| Queue bound `MAX_TXQ_NUM 8` | `eccci/ccci_core.h:32` |
| DL-ACK channel source in this tree | `ccci_get_ccmni_channel()`, `ccci/ccci_port_net.c:104-136` |
| `0x400B`: `if (dsp_region_phy) ccci_set_dsp_region_protection(md, 1)`, answer `{0, request=0}` | `port_kernel.c:1797-1826` |
| `0x2001` handler and its HACC calls | `port_kernel.c:1221-1350`, calls at `:1297-1308`, `BUG_ON` on init failure `:1298-1300` |
| `0x2002` handler; "secro disable" branch answers `{FS_NO_FEATURE, 0}` | `port_kernel.c:1352-1472`, branch `:1455-1466` |
| `0x4001`/`0x4004` answer `FS_FUNC_FAIL` because the features are off | `port_kernel.c:807-813`, `:871-877`, branches `:1474-1541`, `:1542-1583` |
| `0x4008` count-mismatch branch (no clkbuf call) | `port_kernel.c:1679-1685` |

### 2.8 Port routing / "time"

| Item | Evidence |
|---|---|
| Two ports on channel `CCCI_RPC_RX`: char minor 22 and kernel port minor 0 | `eccci/port_cfg.c:58`, `:61`; Nura `ccci/ccci_core.c:1097-1106` |
| Char port matches RPC only when `port_kernel_req_match() == 0`, i.e. LGE op ids (TC1) | `port_char.c:1295-1306` |
| Kernel match: everything else | `port_kernel.c:2166-2197` |
| `CCCI_RT_MISC_INFO_CLIB_TIME` is the runtime feature ("time"), answered with seconds + timezone | `ccci/ccci_boot.c:617-618`, `:745-751` (no RPC op id; downstream switch has no `time` case) |

## 3. Parity matrix

| # | Behaviour | ROM + downstream | Nura before | After | Status |
|---|---|---|---|---|---|
| — | Wire parse: header check, `reserved<=2`, `para_num<=6`, 4-byte-aligned args, `RPC_MAX_BUF_SIZE` bound | `port_kernel.c:1942-1952` | none (dropped) | `ccci_rpc_handle()`, plus overflow/short-packet checks | done |
| — | Response framing: copy header, `op_id\|RPC_API_RESP_ID`, ch `CCCI_RPC_TX`, `data[1]=len`, args with length prefixes | `port_kernel.c:1963-1995` | none | `ccci_rpc_send()`, fresh skb on queue `txq`/`txq_exp` | done |
| — | Blocking response send | `port_kernel.c:2001` | none | bounded retry (1000 × 1-2 ms) on `-EBUSY` | done ([INFERENCE] bound) |
| — | Unknown op → `FS_NO_OP`, one argument | `port_kernel.c:1914-1920` | none | same | done |
| — | Service registry API | none (ROM has no registration) | declaration only | list + refcount, `EXPORT_SYMBOL_GPL` | done |
| — | RX demux: non-LGE RPC op → kernel port | `port_char.c:1295-1306`, `port_kernel.c:2166-2197` | char port took every RPC packet, kernel port never ran | dispatch skips non-kernel ports on `CCCI_RPC_RX` | done |
| 0x2001 | SECURE_ALGO | `port_kernel.c:1221-1350` → `masp_secure_algo_init()/algo()`, `BUG_ON` on failure (no error code) | none | validates 4 ≤ n < 6, answers `FS_NO_FEATURE`, logs missing MASP | **deferred** (MASP/HACC driver absent) |
| 0x2002 | GET_SECRO | asfv2 `masp_secro_en()==0` → `{FS_NO_FEATURE, 0}` (`sec_legacy.c:26-29`, `port_kernel.c:1455-1466`) | none | `{FS_NO_FEATURE, 0}` | done |
| 0x4001 | TDD EINT num | feature off → `FS_FUNC_FAIL` (`port_kernel.c:807-813`) | none | `FS_FUNC_FAIL` ×2 | done |
| 0x4002 | GPIO num | DT `mediatek,gpio_usage_mapping` (`port_kernel.c:832-864`) | none | DT lookup, name table → property, raw-name fallback | done |
| 0x4003 | ADC num | DT `mediatek,adc_channel` (`mt_auxadc.c:1650-1718`) | none | DT lookup by the same 9 properties | done (needs `DTS-FRAGMENT`) |
| 0x4004 | EMI clk type | feature off → `FS_FUNC_FAIL` ×3 | none | same | done |
| 0x4005 | EINT attr | DT nodes + µs→ms + polarity/sensitivity decode | none | same tables/logic, parsed once at init | done (needs `DTS-FRAGMENT`) |
| 0x4006 | GPIO val | `get_md_gpio_val()` | none | gpiochip desc by controller offset, raw level | done |
| 0x4007 | ADC val | `IMM_GetOneChannelValue()` returns raw counts | none | `FS_NO_FEATURE` — provider exposes mV only | **partial** |
| 0x4008 | RF clk buffer | DT config + `clk_buf_get_swctrl_status()`; count≠4 → `{0, count=0xFF, zeros}` | none | count≠4 branch exact; count=4 → `FS_FUNC_FAIL` | **partial** |
| 0x4009 | GPIO+ADC block | v1/v2 by payload size, `0xF` fill, per-item fill | none | v1/v2 exact, GPIO half live, ADC half → `FS_NO_FEATURE` status | **partial** |
| 0x400a | USIM2NFC | compiled out (`FEATURE_INFORM_NFC_VSIM_CHANGE` off) → default `FS_NO_OP` | none | not registered → `FS_NO_OP`, one arg | done |
| 0x400b | DSP EMI MPU | `ccci_set_dsp_region_protection(md, 1)`, answer `{0, request=0}` | none | calls frozen `ccci_md_set_dsp_mpu_protection(md->id)` (0-based, cross-leaf) | done |
| 0x400c | Queue mapping | `port_kernel.c:1166-1205` | none | same, DL-ACK via ccmni channel map, answer `0/0` | done |
| 0x4321 | IT | `tmp_data[0]=1`, `tmp_data[1]=0xA5A5` | none | same | done |
| — | "time" RPC | does not exist; it is `CCCI_RT_MISC_INFO_CLIB_TIME` | already answered by `ccci_boot.c:745-751` | unchanged (verified, not duplicated) | done (other owner) |

## 4. Compile-tested

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/rem-ccci-rpc"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K -e MTK_CCCI_UTIL -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig                       # exit 0
grep -E '^CONFIG_MTK_CCCI|^CONFIG_MTK_CCMNI' .config
#   CONFIG_MTK_CCCI=y  CONFIG_MTK_CCCI_C2K=y  CONFIG_MTK_CCCI_CLDMA=y
#   CONFIG_MTK_CCCI_UTIL=y  CONFIG_MTK_CCMNI=y

rm -f drivers/misc/mediatek/ccci/{ccci_rpc,ccci_core,ccci_port_kernel}.o \
      drivers/misc/mediatek/ccci/built-in.a
make ARCH=arm64 LLVM=1 -j2 W=1 \
     drivers/misc/mediatek/ccci/ccci_rpc.o \
     drivers/misc/mediatek/ccci/ccci_core.o \
     drivers/misc/mediatek/ccci/ccci_port_kernel.o          # exit 0
make ARCH=arm64 LLVM=1 -j2 W=1 \
     drivers/misc/mediatek/ccci/built-in.a                  # exit 0
```

Log: `$M/logs/rem-ccci-rpc_build.log` (`OBJ_RC=0`, `AR_RC=0`).

- All three objects were rebuilt from scratch (`CC` lines present for each).
- `ccci_rpc.o` and `ccci_port_kernel.o`: no warnings at `W=1`.
- `ccci_core.c` emits six pre-existing kernel-doc `Warning:` lines for
  `ccci_md_send_skb()` parameters (`ccci_core.c:384`, the same block the cldma
  leaf reported at `ccci_core.c:404` before my 13-line insertion shifted it).
  Not introduced by this leaf.
- The `make` warnings about an "overriding recipe" for
  `drivers/misc/mediatek/ccci/built-in.a` come from asking kbuild for the
  archive target explicitly; they appear independently of these changes.
- `ar t drivers/misc/mediatek/ccci/built-in.a` lists `ccci_rpc.o` among the ten
  objects. `llvm-nm ccci_rpc.o` shows `T ccci_rpc_register_service`,
  `T ccci_rpc_unregister_service`, `T ccci_rpc_init/exit/handle`, and the
  expected cross-leaf `U ccci_md_set_dsp_mpu_protection` (MPU leaf),
  `U ccci_get_ccmni_channel`/`U ccci_net_port_ops` (same module).

## 5. `[INFERENCE]` and hardware-unverified

- `[INFERENCE]` **GPIO numbering.** The DT values are mt6735 pin-controller
  offsets. Downstream can hand them to `__gpio_get_value()` because its pinctrl
  chip has a stable base; the mainline driver registers with `base = -1`, so the
  engine resolves the controller by compatible and uses the number as the
  *offset* (`gpio_device_get_desc()`). It reports that same offset in
  `gpioPinNum`, so a following `0x4006` round-trip is consistent. On hardware
  where another gpiochip claims a lower base, the reported number differs from
  the downstream global number, but not the pin it designates. Unverified.
- `[INFERENCE]` EINT attributes are parsed once from `ccci_rpc_init()` (module
  init) instead of downstream `ccci_subsys_kernel_init()` (modem probe). The
  device tree is already unflattened at module init and the nodes are static, so
  the values are identical; the ordering difference is not observable.
- `[INFERENCE]` Handler `FS_*` deviation for missing providers: downstream fills
  the GPIO/ADC output with `0xF` and answers `FS_NO_ERROR` even when the ADC
  provider fails; this port answers `FS_FUNC_FAIL` (unknown ADC name) or
  `FS_NO_FEATURE` (raw read unavailable) instead of reporting success with
  unknown values. Wire-compatible (same codes) but semantically stricter.
- `[INFERENCE]` 64-byte cap on modem-supplied names (downstream allocates
  `pkt[0].len` bytes). Real names are ≤ 34 bytes; longer names get
  `FS_PARAM_ERROR`.
- `[INFERENCE]` Response-send retry bound (1000 × 1-2 ms) on `-EBUSY`; the
  downstream blocking request has no explicit timeout.
- `[INFERENCE]` `ccci_rpc_errno_to_fs()` mapping for externally registered
  services (the built-ins return the wire code themselves).
- Hardware-unverified: every GPIO level, ADC count and EINT attribute; no
  device was available. Nothing was simulated and no value is fabricated.
- Hardware-unverified: `0x400b` effect depends on the MPU leaf's implementation
  (cross-leaf, compile-only here).

## 6. DTS-FRAGMENT

Required for `0x4002`/`0x4006` (GPIO), `0x4003` (ADC channel numbers),
`0x4005` (EINT attributes) and — after the IIO driver gains the compatible —
`0x4007`/`0x4009` ADC reads. Not applied. Validated with the tree's dtc:
`cpp -nostdinc -I include -I arch/arm64/boot/dts/mediatek -I
arch/arm64/boot/dts/include -undef -D__DTS__` + `scripts/dtc/dtc -I dts -O dtb`
(exit 0; only the two pre-existing `mt6735.dtsi` warnings for
`/soc/interrupt-controller@10220000` and `/soc/dsi@14011000`; the decompiled DTB
shows `debounce = <0x00 0x186a0>` etc. as in the ROM).

```dts
/ {
	/*
	 * Downstream mt6735.dtsi:528-530 / ROM rom_dtb_0.dts:3004-3015.
	 * Values from rom_dtb_0.dts:3006-3014; the FDD band-support pins at
	 * the end exist only on the rom_dtb_1..3 variants
	 * (rom_dtb_1.dts:3033-3035, board source
	 * mt6737t_grandpplte_ltn_open_gpio_01.dtsi:586-596).
	 */
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
		/* rom_dtb_1..3 only:
		GPIO_FDD_BAND_SUPPORT_DETECT_2ND_PIN = <59>;
		GPIO_FDD_BAND_SUPPORT_DETECT_1ST_PIN = <106>;
		GPIO_FDD_BAND_SUPPORT_DETECT_3RD_PIN = <107>;
		*/
	};
};

&{/soc} {
	/* ROM rom_dtb_0.dts:3158-3168; needs "mediatek,mt6735-auxadc" added
	 * to mt6577_auxadc_of_match[] (mt6577_auxadc.c:313-320) before it binds.
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

/*
 * SIM hot-plug EINT attributes, ROM rom_dtb_0.dts:423-434, i.e. the
 * children downstream hangs off &eintc.  The pio node is the mt6735
 * interrupt controller with #interrupt-cells = <2>
 * (mt6735.dtsi:300-301), so the two-cell specifiers below are inherited
 * correctly and the pinctrl driver does not parse unrelated children
 * (pinctrl-moore.c:601-639).
 */
&pio {
	MD1_SIM1_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM1_HOT_PLUG_EINT-eint";
		interrupts = <0 4>;
		debounce = <0 100000>;
		dedicated = <0 0>;
		src_pin = <0 1>;
		sockettype = <0 0>;
		status = "okay";
	};

	MD1_SIM2_HOT_PLUG_EINT {
		compatible = "mediatek,MD1_SIM2_HOT_PLUG_EINT-eint";
		interrupts = <1 4>;
		debounce = <1 100000>;
		dedicated = <1 0>;
		src_pin = <1 2>;
		sockettype = <1 0>;
		status = "okay";
	};
};
```

## 7. SCAFFOLD CHANGE REQUIRED

none

The frozen declarations in `include/linux/ccci.h` were implemented exactly:
`ccci_rpc_register_service(u32, const char *, ccci_rpc_handler_t)` and
`ccci_rpc_unregister_service(u32)` are `EXPORT_SYMBOL_GPL`, handlers receive
`const struct ccci_rpc_msg *` / `struct ccci_rpc_msg *`, and `0x400b` calls
`ccci_md_set_dsp_mpu_protection()` without defining it.

## 8. Patch and status

Patch: `$M/patches/rem-ccci-rpc.patch` (1733 lines, diff against HEAD
`509789971`).

`git status --porcelain` (index staged, worktree otherwise clean):

```
M  drivers/misc/mediatek/ccci/Makefile
M  drivers/misc/mediatek/ccci/ccci_core.c
M  drivers/misc/mediatek/ccci/ccci_port_kernel.c
A  drivers/misc/mediatek/ccci/ccci_rpc.c
A  drivers/misc/mediatek/ccci/ccci_rpc.h
```

Diffstat: 5 files, +1639/−6. Shared-file hunks for the coordinator:
`Makefile` one line in `ccci-y`; `ccci_core.c` includes at the top, a 13-line
demux block inside `ccci_rx_dispatch_qno()`, and the init/exit block;
`ccci_port_kernel.c` one include plus the `CCCI_RPC_RX` case. No shared DTS, no
`.config`, no other leaf's worktree touched.
