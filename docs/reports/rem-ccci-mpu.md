# CCCI gap — modem MPU / RMPU / security programming

`M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
Worktree `$M/worktrees/rem-ccci-mpu`, branch `rem-ccci-mpu`, HEAD
`509789971 ccci: gap-closure scaffold` (baseline `e4a4be64e ccci: integration
baseline` on Nura `046883c0c`). Continued from the untracked draft
(`ccci_mpu.c`/`ccci_mpu.h` + four touched shared files) after a line-by-line
audit against the ROM binary and the downstream source; the draft was rewritten
where it disagreed with the sources and its uncited values were deleted.

## 1. Gap, files changed, result

**Gap.** The Nura CCCI core loaded and booted the modem but programmed no
memory protection at all: no EMI MPU regions (so the AP could not re-load an
image after a protected boot, and the modem could read the whole AP DRAM after
the runtime handshake), no RMPU remap windows (so the modem could not address
either its own image or the shared memory), no DSP sub-image region, and
`ccci_md_set_dsp_mpu_protection()` — the frozen API for RPC op `0x400B` — had
no implementation. Baseline `ccci_core.c` also had no security-state step in
the boot sequence.

**Files changed**

| File | Change |
|---|---|
| `drivers/misc/mediatek/ccci/ccci_mpu.c` | new: EMI MPU via ATF SMC, RMPU windows, DSP region, AP region, security state, `ccci_mpu_ap_to_md()` |
| `drivers/misc/mediatek/ccci/ccci_mpu.h` | new: private API used by the core wiring |
| `drivers/misc/mediatek/ccci/Makefile` | add `ccci_mpu.o` to `ccci-y` |
| `drivers/misc/mediatek/ccci/ccci_core.c` | additive: MPU init in `ccci_md_register()`, security/clear/remap/protect/DSP calls in `ccci_md_start()`, clears in `ccci_md_stop()`/`ccci_md_reset()` |
| `drivers/misc/mediatek/ccci/ccci_boot.c` | additive: publish `boot.loaded_size`, parse the v3 DSP slot, second-stage AP protection before runtime data, AP→MD translation of the published share-memory addresses |
| `drivers/misc/mediatek/ccci/ccci_priv.h` | additive: `boot.loaded_size/dsp_offset/dsp_size`, `md.smem_offset_ap_to_md/dsp_phy/dsp_size/mpu_smc`, include `ccci_mpu.h` |

**Result.** Full parity with `ccci_platform.c` for the region table, permission
words, region maths, RMPU packing/offsets, DSP region and the AP
re-protection; the MPU is programmed through the same ATF SMC
(`MTK_SIP_KERNEL_EMIMPU_SET = 0x82000209`) the ROM uses, with no invented id,
and every constant is cited below. With no SMC conduit (or no
`mediatek,infracfg-ao`) the code returns the downstream failure path as
`-ENODEV` and logs it rather than pretending success.

## 2. ROM + downstream evidence

`vmlinux.elf` is an **ELF32 ARM** (ROM kernel `3.18.19-14373813`); symbol
addresses from `llvm-nm -n zip/vmlinux.elf`, instructions from
`llvm-objdump -d --start-address=… --stop-address=… zip/vmlinux.elf`.
`movw`/`movt` print as `<unknown>` in this llvm-objdump build; the encodings
were decoded by hand (`movw rd,#imm12|imm4`, `movt` keeps the low half).

### 2.1 EMI MPU backend and SMC arguments

| Item | Evidence |
|---|---|
| SIP id `MTK_SIP_KERNEL_EMIMPU_SET = 0x82000209` | downstream `.../mach/mt_secure_api.h:38` |
| `emi_mpu_smc_set(start,end,perm) -> mt_secure_call(id,start,end,perm)` | `mt_secure_api.h:90-91` |
| `mt_secure_call` is a 4-register SMC, args truncate to 32 bits | `mt_secure_api.h:44-58` (`register u64 reg0 asm("x0") … smc #0`); ARM32 variant `:62-72` |
| only accessor used on MT6735 | `emi_mpu/emi_reg_rw.c:74-82` → `emi_mpu_smc_set()`; compiled for `CONFIG_ARM_PSCI‖CONFIG_MTK_PSCI`, otherwise `return 0` without touching hardware (`:81`) |
| region packed into permission bits 27..30 | `emi_mpu/mt6735/emi_mpu.c:1138-1149`: `access_permission & 0xFFFFFF`, `\| ((region & 0xF) << 27)`, then `mt_emi_mpu_set_region_protection(start,end,perm)` at `:1147` |
| permission encodings `NO_PROTECTION 0`, `SEC_R_NSEC_R 4`, `FORBIDDEN 5`; `SET_ACCESS_PERMISSON(d7..d0)` shifts 21/18/15/12/9/6/3/0; 64 KiB granule | `.../mach/emi_mpu.h:81,85,86`, `:195-197`, `:97` |
| ROM `emi_mpu_set_region_protection` @ `0xc064e448` | `0xc064e470 and r12,r12,#15`; `0xc064e478 bic r10,r10,#0xff000000`; `0xc064e480 orr r10,r10,r12,lsl #27`; `0xc064e4a0 bl mt_emi_mpu_set_region_protection` |
| ROM `mt_emi_mpu_set_region_protection` @ `0xc064c7c4` | `0xc064c7d0 mov r1,r0` (start); `0xc064c7d4 ldr r3,[r11,#4]` (permission); `0xc064c7d8 movw r0,#0x209` / `0xc064c7dc movt r0,#0x8200` → `0x82000209`; `0xc064c7e0 bl mt_secure_call` |
| ROM `mt_secure_call` @ `0xc064c6f8` | `0xc064c704 e1600070` = `smc #0` |
| EMI MPU MMIO is *not* used by this build | `emi_mpu.c:2335-2341` searches `"mediatek,EMI"`; the ROM DTB node is `emi@10203000 { compatible = "mediatek,emi"; }` (`zip/rom_dtb_0.dts:490-494`) — case mismatch, so `EMI_BASE_ADDR` stays NULL; region programming still works because it goes through SMC |

### 2.2 Region ids and permission words

`eccci/mt6735/ccci_platform.c:81-98` (non-`CONFIG_ARCH_MT6735M` half;
`arch/arm/configs/grandpplte_00_defconfig:296` has `# CONFIG_ARCH_MT6735M is
not set`): SEC_OS 0, ATF 1, MD1_SEC_SMEM 3, MD2_SEC_SMEM 4, MD1_ROM 5,
MD1_DSP 6, MD2_ROM 7, MD2_RW 8, MD1_SMEM 9, MD2_LG3G 10, MD3_ROM 7,
MD3_RW 8, MD3_SMEM 10, MD1_RW 14, AP 15 (WIFI EMI FW 12 / WMT 13 unused).

Macros `ccci_platform.c:101-123`; D-domain order (D7..D0 = MFG, MD2, MM,
MD32, CONN, MD1, AP) at `:99-100`. Values recomputed from the macro and
confirmed against the ROM's `movw/movt` constants:

| Macro | Domains D7..D0 | Value | ROM proof |
|---|---|---|---|
| `MPU_ACCESS_PERMISSON_CLEAR` (`:101-102`) | 0,0,0,0,0,0,0,0 | 0x0000000 | `emi_mpu_set_region_protection(0,0,id,0)`, ROM `0xc052f478..0xc052f4d4` |
| `MPU_ACCESS_PERMISSON_AP_MD1_RO_ATTR` (`:103-104`) | 0,0,0,0,0,0,4,0 | 0x00000020 | `ccci_set_mem_access_protection` `0xc052fc70 mov lr,#32`, `0xc052fc84 bl` |
| `MPU_ACCESS_PERMISSON_AP_ATTR` (`:105-106`) | 5,0,5,0,5,5,5,0 | 0x00A28B68 | `ccci_set_ap_region_protection` `0xc052f830 movw r12,#0x8b68` / `0xc052f834 movt r12,#0xa2` |
| `MPU_ACCESS_PERMISSON_MD1_ROM_ATTR` (`:107-108`) | 5,5,5,5,5,5,4,4 | 0x00B6DB64 | `ccci_set_mem_access_protection` `0xc052fcb0 movw r10,#0xdb64` / `0xc052fcc8 movt r10,#0xb6` |
| `MPU_ACCESS_PERMISSON_MD1_RW_ATTR` (`:109-110`) | 5,5,5,5,5,5,0,5 | 0x00B6DB45 | same block `movw r7,#0xdb45` / `movt r7,#0xb6` |
| `MPU_ACCESS_PERMISSON_MD1_SMEM_ATTR` (`:111-112`) | 5,5,5,5,5,5,0,0 | 0x00B6DB40 | same block `movw r8,#0xdb40` / `movt r8,#0xb6` |
| `MPU_ACCESS_PERMISSON_MD1_DSP_ATTR` (`:113-114`) | 5,5,5,5,5,5,4,4 | 0x00B6DB64 | `ccci_set_dsp_region_protection` `0xc052f938 movw r0,#0xdb64` / `0xc052f950 movt r0,#0xb6` |
| `MPU_ACCESS_PERMISSON_MD1_DSP_CL_ATTR` (`:115-116`) | 5,5,5,5,5,5,0,5 | 0x00B6DB45 | same function `0xc052f940 movw r3,#0xdb45` / `0xc052f954 movt r3,#0xb6` |
| `MPU_ACCESS_PERMISSON_MD3_ROM_ATTR` (`:118-119`) | 5,5,0,5,5,5,5,4 | 0x00B45B6C | `ccci_set_mem_access_protection` `0xc052fabc movw r7,#0x5b6c` / `0xc052fac4 movt r7,#0xb4` |
| `MPU_ACCESS_PERMISSON_MD3_RW_ATTR` (`:120-121`) | 5,5,0,5,5,5,5,4 | 0x00B45B6C | same |
| `MPU_ACCESS_PERMISSON_MD3_SMEM_ATTR` (`:122-123`) | 5,5,0,5,5,5,5,0 | 0x00B45B68 | same block `movw r8,#0x5b68` / `movt r8,#0xb4` |

Region ids the ROM actually loads into the argument registers:
`ccci_set_mem_access_protection` `0xc052fcb8 mov r1,#14` (MD1 RW),
`0xc052fcbc mov r2,#5` (MD1 ROM), `0xc052fcc0 mov lr,#9` (MD1 SMEM);
`0xc052fadc mov r1,#8` (MD3 RW), `0xc052fae0 mov r2,#7` (MD3 ROM),
`0xc052fae4 mov lr,#10` (MD3 SMEM).

### 2.3 Region maths

| Item | Evidence |
|---|---|
| `rom_end = ((md_region_phy + img_size + 0xFFFF) & ~0xFFFF) - 1` | `ccci_platform.c:390-391`; ROM `0xc052fb34 add r2,r9,r2` … `0xc052fb44 add r2,r2,#255` … `0xc052fb4c lsr r2,#16` / `lsl r2,#16` / `0xc052fb5c sub r2,r2,#1` |
| `rw_start = rom_end + 1` | `:392`; ROM `0xc052fb58 str` of the aligned value to `[r11,#-0x44]`, used as `r0` at `0xc052fbe8` |
| `rw_end = md_region_phy + md_region_size - 1` (ROM) vs `rw_start + md_region_size - 1` (C source `:392-393`) | ROM `0xc052fb84 add lr,r9,r12` (r9 = `[md+0x7f4]` md_region_phy, r12 = `[md+0x7f8]` md_region_size), `0xc052fb90 sub r9,lr,#1`, used as the end (`r2`) at `0xc052fbec`. **The shipped binary is followed; the C source overstates the region by the aligned image size.** |
| `smem_end = ((smem_phy + smem_size + 0xFFFF) & ~0xFFFF) - 1` | `:397-398`; ROM `0xc052fbd0 add r3,r1,r2` (`[md+0x80c]` smem_phy, `[md+0x810]` smem_size) … `0xc052fc08 sub r7,r7,#1` |
| AP window `[phys_offset, phys_offset + max_dram - 1]`, region 15, perm 0x20 | `ccci_platform.c:373-381`, `:426-430`; ROM `0xc052f7e0 bl get_phys_offset`, `0xc052f7e8 bl get_max_DRAM_size`, `0xc052f81c mov lr,#15`, `0xc052f82c/838` perm 32 |
| second stage AP window, region 15, perm `0x0A28B68` | `ccci_platform.c:221-249`; ROM `0xc052f7cc` (perm at `0xc052f830/34`) |
| order ROM, RW, SMEM, then AP (AP always) | `ccci_platform.c:400-430` (`by_pass_setting` guards only the three MD regions) |
| clear: ROM then RW, `start = end = 0`, permission 0 | `ccci_platform.c:181-190`; ROM `0xc052f478..0xc052f494` and `0xc052f4b8..0xc052f4d4` |
| clear DSP: MD1 only, region 6, `start = end = 0`, permission 0 | `ccci_platform.c:194-219`; ROM `0xc052f6d0 mov r12,#6`, `0xc052f6e8 bl` |
| DSP region `start = mem_layout.dsp_region_phy`, `end = align(start + dsp_region_size) - 1`, region 6 | `ccci_platform.c:270-277`; ROM `0xc052f924 ldr r4,[r0,#0x800]`, `0xc052f92c ldr r2,[r0,#0x804]`, `0xc052f98c mov r12,#6` |

### 2.4 RMPU (BANKn_MAPx) windows

| Item | Evidence |
|---|---|
| MD1 BANK0 `0x300/0x304`, BANK4 `0x308/0x30c`; MD2/MD3 BANK0 `0x310/0x314`, BANK4 `0x318/0x31c`; base = `infra_ao_base` | `ccci_platform.c:130-148` |
| `infra_ao_base` = `of_iomap(of_find_compatible_node("mediatek,INFRACFG_AO"))` | `ccci_platform.c:748-754` |
| `KERN_EMI_BASE 0x40000000` (Bank4) | `ccci_platform.h:22` |
| slot packing: `((addr>>24)\|1)&0xFF`, `((addr>>16)\|1<<8)&0xFF00`, `((addr>>8)\|1<<16)&0xFF0000`, `((addr\|1<<24))&0xFF000000`; stride `0x2000000` | `ccci_platform.c:497-504` (MD1), `:512-519` (MD2/3 smem), `:583-590` (MD2/3 rom) |
| `des -= KERN_EMI_BASE` (non-4G) | `ccci_platform.c:488-492`, `:556-563`; 4G path needs `CONFIG_MTK_LM_MODE` (`ccci_config.h:49-51`), unset in `grandpplte_00_defconfig:1261` |
| `invalid = ALIGN(md_region_phy + md_region_size + smem_region_size, 0x2000000) - KERN_EMI_BASE` | `ccci_core.c:311-313` (caller passes `ALIGN(...)`, callee subtracts, `ccci_platform.c:626-630`) |
| `smem_offset = smem_phy - md_region_phy`; `remainder = smem_offset % 0x2000000`; `smem_offset_AP_to_MD = smem_phy - (remainder + 0x40000000)` | `ccci_core.c:311`, `ccci_platform.c:642-643`; ROM `0xc05306e4 bic r2,r1,#0xfe000000`, `0xc05306f0 add r12,r12,#-0x40000000`, `0xc05306f8 rsb r12,r2,r12`, `0xc0530700 str r12,[r0,#0x814]` |
| `set_md_smem_remap(md, 0x40000000, md_region_phy + smem_offset - remainder, invalid)` / `set_md_rom_rw_mem_remap(md, 0, md_region_phy, invalid)` | `ccci_platform.c:644,648`; ROM `0xc05306fc rsb r2,r2,r1`, `0xc0530704 mov r1,#0x40000000`, `0xc053070c bl`, `0xc0530740 ldr r2,[r4,#0x7f4]`, `0xc0530744 mov r1,#0`, `0xc0530748 bl` |
| MD1 window = 8 consecutive slots; MD2/3 smem = slot 0 + 7 invalid; MD2/3 rom = 3 slots + 9 invalid | `ccci_platform.c:496-509`, `511-539`, `582-618` |
| ROM stores: MD1 `0x308/0x30c` (smem) and `0x300/0x304` (rom); MD2/3 `0x318/0x31c` and `0x310/0x314` | `0xc0530314 str r6,[r3,#0x308]`, `0xc0530328 str r12,[r3,#0x30c]`, `0xc053064c str r4,[r3,#0x300]`, `0xc0530660 str r12,[r3,#0x304]`, `0xc05301d4 str r6,[r3,#0x318]`, `0xc05301e8 str r12,[r3,#0x31c]`, `0xc05304c0 str r4,[r3,#0x310]`, `0xc05304d4 str r12,[r3,#0x314]` |
| `modem_run_env_ready()` bypass in clear / ROM-RW remap | `ccci_platform.c:155-159`, `:340-343`, `:556-560`; implemented `ccci_util/ccci_util_lib_fo.c:908-911` |
| AP→MD consumers | `modem_cldma.c:3201` (`ap_runtime_data_addr`), `:3205` (`set_md_mpu_start_addr`), `:3319-3327` (`ExceShareMemBase`, `TotalShareMemBase`), `:3410` (sleep-log base) |

### 2.5 DSP and security

| Item | Evidence |
|---|---|
| v3 header carries `dsp_img_offset/dsp_img_size`; `0xCDCDCDAA` = none; `offset+size` must fit | `ccci_util/ccci_util_lib_load_img.c:238-253` and `:454-468` |
| `dsp_region_phy = img.address + dsp_offset`, `dsp_region_size = loaded DSP size` | `modem_cldma.c:2270-2285` |
| DSP protect `loaded=0` only when a DSP region exists | `modem_cldma.c:2320-2321` |
| DSP re-protect `loaded=1` from RPC op `0x400B` | `eccci/port_kernel.h:142`, `port_kernel.c:1797-1826` (call at `:1823`) |
| `ccci_init_security()` = one message + `return 0` | `ccci_util/ccci_util_lib_load_img.c:934-942`; ROM `0xc0501e54`: `0xc0501e60/64 movw/movt r0,0xc0c80664`, `0xc0501e68 bl printk`, `0xc0501e6c mov r0,#0`; string `vmlinux.strings:37195 "[ccci0/util]security is on!"` |
| boot order: security → clear MD → clear DSP → load → set MD/AP regions → DSP → power on | `modem_cldma.c:2256`, `2261-2263`, `2319-2321`, then `md_cd_power_on()` |

## 3. Parity matrix

| Behaviour | ROM + downstream | Nura before | After | Status |
|---|---|---|---|---|
| EMI MPU backend | ATF SMC `0x82000209`, args `(start,end,perm)`; MMIO only in the unused `emi_base`/debugfs path | absent | `arm_smccc_smc(0x82000209, start, end, perm, …)`, PT/4 GiB guard, status returned | done |
| Region id packing | `(perm & 0xFFFFFF) \| ((region & 0xF) << 27)` (`emi_mpu.c:1144-1145`) | absent | identical (`ccci_mpu_set_region()`) | done |
| Region ids MT6735 | `ccci_platform.c:81-98` | absent | verbatim table incl. MD3/AP 7/8/10/15 | done |
| Permission words | 10 macros, `ccci_platform.c:101-123` | absent | verbatim, values match ROM constants (§2.2) | done |
| ROM/RW/SMEM region maths | `ccci_platform.c:390-398`; ROM uses `md_region_phy + md_region_size - 1` for RW end | absent | ROM maths, incl. the ROM-vs-source RW-end difference (documented in code) | done |
| 64 KiB end rounding | `((x+0xFFFF)&~0xFFFF)-1` | absent | same helper for ROM/DSP/SMEM | done |
| AP region stage 1 (MD1 RO, 0x20) | `ccci_platform.c:373-383, 426-430` | absent | programmed with ROM, RW and SMEM | done |
| AP region stage 2 (0x0A28B68) before runtime data | `ccci_platform.c:221-249`; called at `modem_cldma.c:3226,3275` | absent | `ccci_mpu_set_ap_region_protection()` at the top of `ccci_boot_send_runtime_data()` | done |
| Clear MD ROM/RW (+DSP for MD1) before load | `ccci_platform.c:150-219`; `modem_cldma.c:2262-2263` | absent | `ccci_mpu_clear_md_protection()` (both clears folded) in `ccci_md_start()` before the load | done |
| DSP region from image header, `loaded=0/1` | `ccci_platform.c:251-325`; `modem_cldma.c:2320-2321`; `port_kernel.c:1823` | absent | header `dsp_img_offset/dsp_img_size`; `ccci_mpu_dsp_protection(md, false)` at start, `ccci_md_set_dsp_mpu_protection()` (loaded=1) for op `0x400B`; returns 0 when no DSP | done (size basis: see §5) |
| RMPU MD1/MD2/MD3 windows | `ccci_platform.c:485-620` | absent | byte-identical packing and offsets `0x300..0x31c` | done |
| `smem_offset_AP_to_MD` publication | `ccci_platform.c:643`; stored at `md+0x814` in ROM | absent | `md->smem_offset_ap_to_md`, set to 0 until the windows are written | done |
| AP→MD address translation in consumers | `modem_cldma.c:3201,3205,3319-3327,3410` | published raw AP addresses | all six sites go through `ccci_mpu_ap_to_md()` (underflow-guarded) | done |
| Security state | `ccci_init_security()` = message + 0 | absent | same, plus the parsed header summary; no fake verification | done |
| Boot wiring order | `md_cd_start()` (`modem_cldma.c:2256-2321`) | `load → power_on → …` | security → clear → load → remap → MD/AP protect → DSP → power_on | done |
| Clear on stop/reset | **not done downstream** (clear happens only at start) | absent | also cleared in `ccci_md_stop()`/`ccci_md_reset()` (defensive, §5) | done (superset) |
| `modem_run_env_ready()` bypass | skips clear/protect when LK booted the modem | n/a | not reproduced: this driver always loads via `request_firmware()` | done (equivalent) |
| No-SMC fallback | downstream compiles the SMC path for `CONFIG_ARM_PSCI`; with no conduit it returns 0 and programs nothing (`emi_reg_rw.c:81`) | n/a | returns `-ENODEV` and logs (never fakes success). No direct-MMIO path: MT6735 has no non-secure MPU accessor and the Nura DT has no EMI node | deferred (see §5) |
| MD2/MD4/MD5 EMI MPU | `ccci_set_mem_access_protection()` handles only MD_SYS1/MD_SYS3 | absent | `-EINVAL` with the downstream "invalid when MPU protect" error | done |

## 4. Compile-tested

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/rem-ccci-mpu"
cp "$M/nura.config" .config
scripts/config -e MTK_CCCI -e MTK_CCMNI -e MTK_CCCI_C2K -e MTK_CCCI_UTIL -e MTK_CCCI_CLDMA
make ARCH=arm64 LLVM=1 olddefconfig                       # exit 0
grep -E '^CONFIG_MTK_CCCI' .config                        # CONFIG_MTK_CCCI=y
grep -E '^CONFIG_MTK_CCMNI' .config                       # CONFIG_MTK_CCMNI=y
# … C2K/CLDMA/UTIL also =y
rm -f drivers/misc/mediatek/ccci/{ccci_mpu,ccci_core,ccci_boot}.o \
      drivers/misc/mediatek/ccci/built-in.a
make ARCH=arm64 LLVM=1 -j2 W=1 \
     drivers/misc/mediatek/ccci/ccci_mpu.o \
     drivers/misc/mediatek/ccci/ccci_core.o \
     drivers/misc/mediatek/ccci/ccci_boot.o               # exit 0
make ARCH=arm64 LLVM=1 -j2 W=1 drivers/misc/mediatek/ccci/  # exit 0, AR built-in.a
```

- Objects rebuilt for real (three `CC` lines, `ccci_boot.o` included) and
  `drivers/misc/mediatek/ccci/built-in.a` produced with all ten members
  (`ccci_core.o`, `ccci_platform.o`, `ccci_ccif.o`, `ccci_boot.o`,
  `ccci_mpu.o`, `ccci_port_char.o`, `ccci_port_net.o`, `ccci_port_kernel.o`,
  `ccci_ringbuf.o`, `ccci_sysfs.o`).
- `make …/ccci/ccci.o` is not a valid target in this tree (v7.0 archives the
  `ccci-y` members directly into `built-in.a`; observed
  `No rule to make target 'drivers/misc/mediatek/ccci/ccci.o'`). `built-in.a`
  is the composite artifact, and `ccci.o` is never produced as a file.
- `W=1` warnings, all pre-existing and outside this leaf's hunks:
  - `drivers/misc/mediatek/ccci/ccci_ringbuf.c:209:13: warning: variable 'read' set but not used [-Wunused-but-set-variable]` (file untouched);
  - kernel-doc "function parameter … not described" in
    `ccci_core.c:370` (`ccci_md_send_skb`) and `ccci_boot.c:670`
    (`ccci_boot_prepare_runtime_data`) — printed only when those sources are
    compiled fresh with `W=1`; both doc blocks come from the integration
    baseline and are not part of this leaf's hunks.
  - `ccci_mpu.c` and `ccci_mpu.h`: no warnings, no errors.
- Symbols in `ccci_mpu.o` (`llvm-nm --defined-only`): `ccci_init_security`,
  `ccci_md_set_dsp_mpu_protection`, `ccci_mpu_ap_to_md`,
  `ccci_mpu_clear_md_protection`, `ccci_mpu_dsp_protection`, `ccci_mpu_init`,
  `ccci_mpu_set_ap_region_protection`, `ccci_mpu_set_md_protection`,
  `ccci_mpu_set_mem_remap` (plus statics). Undefined: `ccci_md_get`,
  `ccci_util_log` (core), `arm_smccc_smc`. No `ccci_rpc_*`,
  `aee_dump_ccci_debug_info`, `ccci_cldma_*` definition, so no cross-leaf
  symbol is shadowed. `MTK_CCCI` is `bool`, so no `EXPORT_SYMBOL` is needed
  for the frozen API (all users are built into `vmlinux`).

## 5. `[INFERENCE]` and hardware-unverified

- `[INFERENCE]` `memblock_start_of_DRAM()` / `memblock_end_of_DRAM() - start`
  are used for the AP window in place of downstream
  `get_phys_offset()`/`get_max_DRAM_size()` (`ccci_platform.c:373-381`). On
  arm64 both return the DRAM bank base/size; no reserved-memory subtraction is
  applied, which matches `get_max_DRAM_size()`.
- `[INFERENCE]` **DSP region size basis.** Downstream sets
  `dsp_region_size` to the size of the separately `request_firmware()`-loaded
  DSP sub-image (`modem_cldma.c:2272,2285`), while this driver uses the header
  field `dsp_img_size`, which the loader bounds to the same slot and which is
  the upper bound of the loaded size. The protected range is therefore ≥ the
  downstream one. This tree does not load a separate DSP file: the sub-image
  already lives inside the copied modem image at `dsp_img_offset`.
- `[INFERENCE]` `modem_run_env_ready()` cannot be true here: this driver always
  loads the image itself (`ccci_boot_load_firmware()`), so the LK-already-done
  bypass in the downstream clear/remap functions is not reproduced.
- `[INFERENCE]` The RMPU windows are programmed on every `ccci_md_start()`,
  downstream does it once from the layout init (`ccci_core.c:311-313`). The
  register writes are idempotent and the published offset is recomputed, so
  behaviour is identical; doing it per start keeps the windows correct after an
  exception restart that never went through the layout init.
- `[INFERENCE]` Clearing protection in `ccci_md_stop()`/`ccci_md_reset()` is not
  a downstream call site (downstream only clears at the next start). It is
  harmless (loosens AP access only) and keeps the state consistent if the
  modem is not restarted; it is kept deliberately.
- `[INFERENCE]` `writel()` + `mb()` replaces downstream
  `mt_reg_sync_writel()` = `__raw_writel()` + `mb()` (`sync_write.h:28-33`).
  Equivalent on little-endian arm64.
- `[INFERENCE]` The SMC conduit test accepts a `psci` node with
  `method = "smc"` (the tree's `mt6735.dtsi:34-36`), which is exactly the
  downstream compile-time condition (`CONFIG_ARM_PSCI` in `emi_reg_rw.c:78`).
  It does **not** prove the EL3 monitor implements MediaTek's SIP id; a
  non-MediaTek monitor returns an error which is logged and returned.
- **No direct-MMIO EMI MPU fallback was written.** The MT6735 downstream has no
  non-secure accessor for the MPU regions (every `mt_emi_reg_write()` to an
  offset in `EMI_MPU_START..END` goes through SMC when PSCI is enabled,
  `emi_reg_rw.c:48-70`), and neither the Nura tree nor the ROM DTB exposes an
  EMI node the Nura DT could map. Fabricating a register path would be a guess.
- Hardware-unverified: no device was attached. No SMC was executed, no MPU
  register was read back, the modem was never booted with these regions, and
  the DSP sub-image (`dsp_img_size == 0` for this modem image) was never
  exercised.
- Observation (scaffold, not changed): the `ccci_priv.h` scaffold comment calls
  the INFRA_AO window "MT6735 0x10001000"; the ROM DTB
  (`rom_dtb_0.dts:315-318`), the downstream DTS (`mt6735.dtsi:304-307`) and
  the Nura node (`mt6735.dtsi:145-150`) all put `infracfg_ao` at
  `0x10000000`/`0x1000` (0x10001000 is PWRAP). Only the comment is wrong; the
  code maps the phandle's `reg`, so the RMPU offsets are unaffected.

## 6. DTS-FRAGMENT

The EMI MPU needs no DT node (SMC only). The RMPU needs the INFRA_AO window on
the modem node; without it `md->infra_ao` stays NULL,
`ccci_mpu_set_mem_remap()` returns `-ENODEV`, logs it and leaves
`smem_offset_ap_to_md = 0`, and the modem cannot see its own image. This is the
same node/property already required by the CLDMA leaf
(`$M/logs/ccci-gap-cldma.md` §6), on the `mdcldma` node of
`$M/logs/ccci-cldma.md` §9:

```dts
&{/soc} {
	mdcldma: mdcldma@1000a000 {
		/* ... existing properties from logs/ccci-cldma.md §9 ... */
		mediatek,infracfg-ao = <&infracfg>;	/* RMPU BANKn_MAPx 0x300..0x31c */
	};
};
```

`&infracfg` is `clock-controller@10000000`
(`arch/arm64/boot/dts/mediatek/mt6735.dtsi:145-150`, `reg = <0 0x10000000 0
0x1000>`), the same block as the downstream/ROM `infracfg_ao@10000000`
(`arch/arm64/boot/dts/mt6735.dtsi:304-307`, `rom_dtb_0.dts:315-318`), so
offsets `0x300..0x31c` are covered. Not applied (shared DTS is
coordinator-owned).

## 7. SCAFFOLD CHANGE REQUIRED

none. `int ccci_md_set_dsp_mpu_protection(unsigned int md_id)` is implemented
exactly as declared; `@md_id` follows the core's 0-based convention
(`include/linux/ccci.h:23`), which the RPC leaf must use (downstream calls
`ccci_set_dsp_region_protection(md, 1)` on the modem that received op `0x400B`,
`port_kernel.c:1823`).

## 8. Patch and status

Patch: `$M/patches/rem-ccci-mpu.patch` (diff against HEAD `509789971`).

`git status --porcelain` in `$M/worktrees/rem-ccci-mpu`:

```
M  drivers/misc/mediatek/ccci/Makefile
M  drivers/misc/mediatek/ccci/ccci_boot.c
M  drivers/misc/mediatek/ccci/ccci_core.c
A  drivers/misc/mediatek/ccci/ccci_mpu.c
A  drivers/misc/mediatek/ccci/ccci_mpu.h
M  drivers/misc/mediatek/ccci/ccci_priv.h
```

Diffstat: 6 files, +1024/−10. No DTS, no `.config`, no other leaf's files.
Shared-file hunks for the coordinator: `ccci_core.c` has three additive blocks
(two calls in `ccci_md_start()`, one pair in `ccci_md_stop()`/
`ccci_md_reset()`, one call in `ccci_md_register()`); `ccci_boot.c` has four
additive blocks (DSP parse in `ccci_boot_parse_header()`, `loaded_size` in
`ccci_boot_load_firmware()`, address translation in
`ccci_boot_prepare_runtime_data()`/`ccci_boot_send_runtime_sram()`, AP
re-protection + translation in `ccci_boot_send_runtime_data()`);
`ccci_priv.h` adds one include, three `ccci_boot` fields and four `ccci_md`
fields.
