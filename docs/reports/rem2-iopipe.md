# rem2-iopipe — camera TPIPE / Pass-1 / Pass-2 tuning and pipeline programming

Worktree: `$M2/worktrees/rem2-iopipe`, detached at `src/nura-linux` HEAD
`046883c0c2b956fd7436d3c1156f25512d1f6a90`, `parity-baseline-all.patch` applied
(the parity payload is *uncommitted* in this worktree, see §6.5).
Deliverables: `$M/patches/rem2-iopipe.patch`, this report,
`$M/logs/rem2-iopipe-build.log`, `$M/logs/rem2-iopipe-check.log`.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`
`$M2` = `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/g532m_work`

**Result in one line:** the six `libcam.iopipe.so` port/path maps, the 25-token /
24-NVRAM-type `ISP_MGR_*` manager catalogue, 19 machine-verified TPIPE/P1/P1_D/P2
enable-word programming records for 13 modules, the CQ0C instruction builder and the
9 file-backed tuning tables are recovered into a host tool whose selftest re-reads every
table address, size and instruction word from the shipped ELFs; no kernel change is
warranted and no DSP pipeline exists in any available source.

## 1. Gap, files changed, result

Gap: the camera pipeline reconstruction stopped at `camdrv`/`otp`; nothing decoded the
ISP *pipe* (port/path maps), the 3A tuning manager register programming, the per-module
enable words, or the static tuning tables. This leaf closes that by extracting the data
from the vendor libraries into userspace, and only marks kernel-side constants where the
kernel itself must program them (here: nowhere, §4).

```
 tools/mediatek/camera/Makefile                 |  3 +   (3 recipe lines, see §6.5)
 tools/mediatek/camera/iopipe/Makefile          |  87 ++++
 tools/mediatek/camera/iopipe/iopipe_elf.c      | 590 +
 tools/mediatek/camera/iopipe/iopipe_elf.h      | 114 +
 tools/mediatek/camera/iopipe/iopipe_portmap.c  | 496 +
 tools/mediatek/camera/iopipe/iopipe_portmap.h  |  84 +
 tools/mediatek/camera/iopipe/iopipe_mgr.c      | 415 +
 tools/mediatek/camera/iopipe/iopipe_mgr.h      |  44 +
 tools/mediatek/camera/iopipe/iopipe_regmap.c   | 575 +
 tools/mediatek/camera/iopipe/iopipe_regmap.h   |  87 +
 tools/mediatek/camera/iopipe/iopipe_cq.c       |  83 +
 tools/mediatek/camera/iopipe/iopipe_cq.h       |  73 +
 tools/mediatek/camera/iopipe/iopipe_tuning.c   | 316 +
 tools/mediatek/camera/iopipe/iopipe_tuning.h   |  43 +
 tools/mediatek/camera/iopipe/main.c            | 282 +
 tools/mediatek/camera/iopipe/iopipe_selftest.c | 284 +
 15 files changed, 3573 insertions(+)
```

No file outside `tools/mediatek/` is touched: no DTS, no `nura.config`, no
`drivers/media/platform/mediatek/isp/*`, no `libcamdrv/`, no `otp/`, no commit.

## 2. Evidence

### 2.1 Environment / how the ELFs were read

All vendor files are ELF32 ARM, little-endian, Thumb-2, stripped, dynsym retained:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
llvm-readelf -h -lW -SW -sW -rW $M/zip/vendor-libs/libcam.iopipe.so
llvm-readelf -h -lW -SW -sW -rW $M/zip/vendor-libs/libcam.hal3a.v3.so
llvm-objdump -d --triple=thumbv7a-none-eabi \
    --start-address=0x… --stop-address=0x… $M/zip/vendor-libs/<lib>.so
```

Two facts the tool depends on:

* `libcam.iopipe.so`: first `PT_LOAD` vaddr `0x0`/file `0x0` filesz `0x2da1c`;
  second `PT_LOAD` vaddr `0x2ee60`/file `0x2de60` filesz `0x172c`, memsz `0x1be9`.
  `.data` is vaddr `0x30000` at file `0x2f000` — so every table below is reached through
  the *second* segment (the tool prints `PT_LOAD[3]`).
* `libcam.hal3a.v3.so`: first `PT_LOAD` vaddr `0x2000`/file `0x0` filesz `0xb9c18`;
  `.text` is vaddr `0x3ccc0` size `0x607e8`, `.got` vaddr `0xbfea0`.
* Symbol values of Thumb functions carry the low bit (`_ZN13NSIspTuningv311ISP_MGR_BNR5applyEN11NSIspTuning13EIspProfile_TE`
  is `0x66d4d`, size 360 → function `0x66d4c`..`0x66eb3`). The tool masks it and the
  selftest re-checks each recorded instruction against that range; disassembling from the
  odd address yields garbage, which is why every window below starts at the even address.

### 2.2 `libcam.iopipe.so` objects used (dynsym, `SHN`/section 23 = `.data`)

| symbol | st_value | st_size | section | records (proved divisor) |
|---|---|---|---|---|
| `mPortIdxMappingAlltoP2` | 0x0003000c | 272 | `.data` | 34 × 8 B (2 u32) |
| `mPortIdxMappingP2toAll` | 0x000304fc | 136 | `.data` | 17 × 8 B (2 u32) |
| `mSwHwPathMapping` | 0x0003012c | 860 | `.data` | 43 × 20 B (5 u32) |
| `mTransformMapping` | 0x0003049c | 96 | `.data` | 8 × 12 B (3 u32) |
| `mPortTypeMapping` | 0x00030488 | 16 | `.data` | 2 × 8 B (2 u32) |
| `nsTimeoutToWait` | 0x00030120 | 8 | `.data` | 1 × u64 ns |

GOT slots the code loads them through (`llvm-readelf -rW`, all `R_ARM_GLOB_DAT`):

| slot | symbol |
|---|---|
| 0x2f8e0 | `mSwHwPathMapping` |
| 0x2f910 | `mPortIdxMappingAlltoP2` |
| 0x2f914 | `mPortTypeMapping` |
| 0x2f918 | `mTransformMapping` |
| 0x2f920 | `nsTimeoutToWait` |
| 0x2f928 | `mPortIdxMappingP2toAll` |

Disassembly windows that **prove** the record widths:

```
; libcam.iopipe.so FeatureStream::init, record stride 0x14 over mSwHwPathMapping
   11682: f8df a0e8  ldr.w r10,[pc,#0xe8]      @ literal 0x1176c
   11686: 2214       movs  r2,#0x14
   11688: 6ba3       ldr   r3,[r4,#0x38]
   1168e: f857 700a  ldr.w r7,[r7,r10]          @ r7 = &mSwHwPathMapping (slot 0x2f8e0)
   11698: fb02 7e03  mla   lr,r2,r3,r7          @ record = base + 0x14 * scenario
   116a2: f8de 1008  ldr.w r1,[lr,#0x8]         @ field +0x8 passed as an argument
; literal 0x1176c = 0xffffff44 (anchor 0x2f99c -> 0x2f8e0), cmp r1,#0x2b at 0x10bbe /
; 0x1106a / 0x11634 bounds the scenario to 43 entries.
```

```
; libcam.iopipe.so HalPipeWrapper::waitDequeVSSBufAgain — 64-bit ns timeout
   15d48: 447b       add   r3,pc
   15d4a: 681b       ldr   r3,[r3]             @ r3 = &nsTimeoutToWait (slot 0x2f920)
   15d4c: e9d3 6700  ldrd  r6,r7,[r3]          @ 64-bit value 3000000 ns = 3 ms
   15d50: a30d       adr   r3,#52   -> 0x15d88 @ 64-bit divisor 0x3b9aca00 = 1e9
   15d5a: blx   __aeabi_ldivmod  -> tv_sec  = value / 1e9   (str r0,[sp])
   15d64: blx   __aeabi_ldivmod  -> tv_nsec = value % 1e9   (str r2,[sp,#4])
   15d78: blx   pthread_cond_timedwait_relative_np
; bytes at 0x15d88: 00 ca 9a 3b 00 00 00 00  -> 0x3b9aca00, 0
```

`mPortIdxMappingAlltoP2` / `mPortIdxMappingP2toAll` / `mTransformMapping` widths are
proved by content: the first field is a dense ascending index (0..33, 0..16, 0..7) and
only the stated divisor makes that true. All three checks run in the selftest, and the two
port maps are additionally checked to be each other's inverse.

### 2.3 `libcam.hal3a.v3.so` — manager symbols used (section 13 = `.text`)

| symbol (mangled) | st_value | st_size | notes |
|---|---|---|---|
| `…11ISP_MGR_OBC5applyEN11NSIspTuning13EIspProfile_TE` | 0x00066125 | 692 | P1 + P1_D enable |
| `…11ISP_MGR_BNR5applyEN11NSIspTuning13EIspProfile_TE` | 0x00066d4d | 360 | P1 + P1_D enable |
| `…11ISP_MGR_LSC5applyEN11NSIspTuning13EIspProfile_TE` | 0x00067675 | 608 | P1/P1_D, two words each |
| `…11ISP_MGR_RPG5applyEN11NSIspTuning13EIspProfile_TE` | 0x00068141 | 404 | P1 + P1_D |
| `…11ISP_MGR_PGN5applyEN11NSIspTuning13EIspProfile_TE` | 0x00068a61 | 264 | P2 |
| `…11ISP_MGR_SL25applyEN11NSIspTuning13EIspProfile_TE` | 0x00068f69 | 676 | P2 |
| `…11ISP_MGR_CFA5applyEN11NSIspTuning13EIspProfile_TE` | 0x000699a9 | 204 | P2, always-on ORR |
| `…11ISP_MGR_CCM5applyEN11NSIspTuning13EIspProfile_TE` | 0x00069e51 | 348 | P2 |
| `…11ISP_MGR_GGM5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006a395 | 236 | P2 |
| `…11ISP_MGR_G2C5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006a9d5 | 372 | P2 |
| `…17ISP_MGR_G2C_SHADE5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006acd1 | 92 | **no enable word** |
| `…11ISP_MGR_NBC5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006b639 | 280 | P2 |
| `…11ISP_MGR_PCA5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006baf9 | 280 | P2 |
| `…12ISP_MGR_SEEE5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006c495 | 212 | P2 (class name is `ISP_MGR_SEEE`) |
| `…12ISP_MGR_NR3D5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006ca35 | 100 | **no enable word** |
| `…11ISP_MGR_MFB5applyEN11NSIspTuning13EIspProfile_TE` | 0x0006ce11 | 64 | **no enable word** |
| `…14ISP_MGR_MIXER35applyEN11NSIspTuning13EIspProfile_TE` | 0x0006d165 | 64 | **no enable word** |
| `…17ISP_MGR_CTL_EN_P111getInstanceEN11NSIspTuning12ESensorDev_TE` | 0x00065a11 | 236 | owner of 0x4004 |
| `…19ISP_MGR_CTL_EN_P1_D11getInstanceEN11NSIspTuning12ESensorDev_TE` | 0x00065b59 | 236 | owner of 0x4010 |
| `…17ISP_MGR_CTL_EN_P211getInstanceEN11NSIspTuning12ESensorDev_TE` | 0x00065ca1 | 236 | owner of 0x4018 |

Every module also has an `apply(EIspProfile_T, isp_reg_t*)` overload; it is a generic
runtime loop, not static programming (2.5).

### 2.4 The 19 verified enable-word records

Recorded as `{module, symbol, MOVW addr+word, BFI/ORR addr+word, bit, form, owner-call addr+word, owner}`.
The selftest re-reads all three words from the ELF, re-decodes the MOVW immediate and the
BFI/ORR bit, checks the call word is a `BLX`, and checks the owning `CTL_EN_*` module
exists in the catalogue.

| module | reg (CAM) | owner manager | bit | form | MOVW @ | BFI/ORR @ |
|---|---|---|---|---|---|---|
| OBC | 0x4004 | CTL_EN_P1 | 4 | bfi | 0x6621a = `0904f244` | 0x66264 = `1204f366` |
| OBC | 0x4010 | CTL_EN_P1_D | 4 | bfi | 0x66310 = `0910f244` | 0x66362 = `1204f366` |
| BNR | 0x4004 | CTL_EN_P1 | 5 | bfi | 0x66dd0 = `0704f244` | 0x66d9e = `1245f36a` |
| BNR | 0x4010 | CTL_EN_P1_D | 5 | bfi | 0x66e16 = `0710f244` | 0x66df4 = `1245f36a` |
| LSC | 0x4004 | CTL_EN_P1 | 6 | bfi | 0x676da = `0904f244` | 0x6771c = `1286f361` |
| LSC | 0x4008 | CTL_EN_P1 | 7 | bfi | 0x67744 = `0908f244` | 0x67780 = `17c7f362` |
| LSC | 0x4010 | CTL_EN_P1_D | 6 | bfi | 0x677be = `0910f244` | 0x677fe = `1786f362` |
| LSC | 0x4014 | CTL_EN_P1_D | 7 | bfi | 0x67826 = `0914f244` | 0x67862 = `17c7f361` |
| RPG | 0x4004 | CTL_EN_P1 | 7 | bfi | 0x6818e = `0804f244` | 0x681ce = `12c7f361` |
| RPG | 0x4010 | CTL_EN_P1_D | 7 | bfi | 0x6820e = `0810f244` | 0x68250 = `12c7f361` |
| PGN | 0x4018 | CTL_EN_P2 | 1 | bfi | 0x68a9c = `0818f244` | 0x68ae6 = `0241f361` |
| SL2 | 0x4018 | CTL_EN_P2 | 2 | bfi | 0x69042 = `0518f244` | 0x6908c = `0182f369` |
| CFA | 0x4018 | CTL_EN_P2 | 3 | orr-imm `#0x8` | 0x699d6 = `0418f244` | 0x69a08 = `0108f040` |
| CCM | 0x4018 | CTL_EN_P2 | 5 | bfi | 0x69ed0 = `0818f244` | 0x69ee6 = `1c45f362` |
| GGM | 0x4018 | CTL_EN_P2 | 6 | bfi | 0x6a3a2 = `0518f244` | 0x6a3ec = `1286f368` |
| G2C | 0x4018 | CTL_EN_P2 | 1 | bfi | 0x6aa7a = `0818f244` | 0x6aa8e = `0341f366` |
| NBC | 0x4018 | CTL_EN_P2 | 0 | bfi | 0x6b68a = `0518f244` | 0x6b682 = `0800f365` |
| PCA | 0x4018 | CTL_EN_P2 | 4 | bfi | 0x6bb06 = `0818f244` | 0x6bb18 = `1304f369` |
| SEEE | 0x4018 | CTL_EN_P2 | 5 | bfi | 0x6c4b6 = `0418f244` | 0x6c4ca = `1145f369` |

Owner call sites (`BLX …getInstance@plt` inside the same function), recorded per record:
OBC 0x66298/0x66396, BNR 0x66d92/0x66de8, LSC 0x677ae/0x67890, RPG 0x68200/0x68282,
PGN 0x68b18, SL2 0x68fd4, CFA 0x69a36, CCM 0x69ed6, GGM 0x6a41a, G2C 0x6aa7e,
NBC 0x6b69a, PCA 0x6bb0a, SEEE 0x6c4be.

Instruction-form evidence for one record (BNR, P1 branch), from
`--start-address=0x66d4c --stop-address=0x66eb4`:

```
   66d56: f894 70e4  ldrb.w r7,[r4,#0xe4]      @ module enable flag
   66d92: f7d4 e884  blx   ISP_MGR_CTL_EN_P1::getInstance@plt   (0x3ae9c)
   66d96: f890 20d0  ldrb.w r2,[r0,#0xd0]      @ cached CTL_EN_P1 byte
   66d9e: f36a 1245  bfi   r2,r10,#5,#1         @ bit 5 <- enable flag
   66dd0: f244 0704  movw  r7,#0x4004           @ CAM_CTL_EN_P1 (0x15004004)
   66e30: …          TuningMgr readReg/writeReg with r2 = 0x4004
   66e16: f244 0710  movw  r7,#0x4010           @ the P1_D twin (else branch,
   66de8: f7d4 e85e  blx   ISP_MGR_CTL_EN_P1_D::getInstance@plt
   66df4: f36a 1245  bfi   r2,r10,#5,#1
```

`0x4004` is independently named in the downstream GPL tree:
`drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c:1402`
(`MUINT32 cam_ctrl_en_p1; /*4004 */`, read at `:1437` as `ISP_RD32(ISP_ADDR + 0x4)` with
`ISP_ADDR = 0x15004000`, `camera_isp_D1.c:367`), i.e. CAM address 0x4004 = ISP offset
0x0004 — the same register the Nura header already carries as `ISP_REG_EN1` (0x0004).

### 2.5 Why `MIXER3`/`NR3D`/`MFB`/`G2C_SHADE` have no record

Their `apply(EIspProfile_T)` bodies carry no register immediate, and their
`apply(EIspProfile_T, isp_reg_t*)` overloads write through a *runtime* pair list:

```
; MFB apply(profile, isp_reg_t*) 0x6ce50 — MIXER3 0x6d1a4 is identical
   6ce52: 6883       ldr   r3,[r0,#0x8]     @ pair array (addr,value) built by put()
   6ce54: 68c4       ldr   r4,[r0,#0xc]     @ count
   6ce5a: 42a1       cmp   r1,r4
   6ce62: e913 0060  ldmdb r3,{r5,r6}       @ r5 = register, r6 = value
   6ce68: 5156       str   r6,[r2,r5]       @ isp_reg_t[register] = value
```

So the register addresses exist only as NVRAM data at run time — there is nothing static
to recover, and the tool says exactly that instead of inventing it
(`iopipe_regmap_unproven()` re-scans each body for a CAM-window MOVW with a real Thumb-2
length walk and fails if one is found in an uncatalogued module).

`ISP_MGR_FLK_CONFIG`'s programming methods are not present as code in this library:

```
llvm-readelf -sW libcam.hal3a.v3.so | grep FLK_CONFIG
  …9SetFLKWinEiiii     0x0003d07b  size 2
  …9enableFlkEi        0x0003d07b  size 2
  …15SetFKO_DMA_AddrEii 0x0003d07b size 2
  …5applyEv            0x000431cd  size 4
; 0x3d07b lies inside _ZThn4_N6NS3Av313Hal3AAdapter38doDataCbEiPvj (0x3d07c: sub.w r0,r0,#4)
```

### 2.6 Tuning symbols (allow-list, size re-checked at run time)

| library | symbol | vaddr | size | section | decode |
|---|---|---|---|---|---|
| libcameracustom.so | `ShadingATNTable` | 0x0050af18 | 504 | `.data` | 21×6 s32, `v[0..4]/v[5]`, `v[5]=1e6` (`rem-camera-otp.md` §3.3/§3.4) |
| libcameracustom.so | `LomoFilterG2C` | 0x00453f70 | 160 | `.data` | raw |
| libcameracustom.so | `LomoFilterG2CDefault` | 0x0044e508 | 20 | `.data` | raw |
| libcameracustom.so | `LomoFilterGGM` | 0x0044e520 | 11520 | `.data` | raw |
| libcameracustom.so | `LomoFilterGGMJni` | 0x00451220 | 11520 | `.data` | raw |
| libcameracustom.so | `SensorList` | 0x0050b138 | 168 | `.data` | raw |
| libcameracustom.so | `channel_lsc_data` | 0x007b00a0 | 910 | `.bss` | runtime only |
| libcameracustom.so | `channel_lsc_data_2800k` | 0x007b042e | 910 | `.bss` | runtime only |
| libcameracustom.so | `channel_lsc_data_4100k` | 0x007b07bc | 910 | `.bss` | runtime only |
| libcameracustom.so | `g_lsc_param` | 0x007aff74 | 100 | `.bss` | runtime only |
| libcameracustom.so | `g_rLSCCaliINIParam` | 0x007b0060 | 64 | `.bss` | runtime only |
| lib3a.so | `_ZN7AwbAlgo14m_i4AWBLogLUT1E` | 0x0006a6e8 | 1024 | `.rodata` | 256 s32 |
| lib3a.so | `_ZN7AwbAlgo14m_i4AWBLogLUT2E` | 0x00058fe8 | 8192 | `.rodata` | 2048 s32 |
| libcamalgo.so | `_ZN7GmaCore15m_i4DefGmaBase0E` | 0x0067ad40 | 580 | `.rodata` | 145 u32 |
| libcamalgo.so | `g_lsc_param` | 0x00902220 | 104 | `.bss` | runtime only |

Per-sensor ISP NVRAM defaults are **generators, not blobs** (machine-listed by
`iopipe tuning --accessors`):

```
  _ZNK9NSFeature13RAWSensorInfoILj537EE17impGetDefaultDataE21CAMERA_DATA_TYPE_ENUMPvj   @0xf371c size 208
  _ZNK9NSFeature13RAWSensorInfoILj24112EE17impGetDefaultDataE21CAMERA_DATA_TYPE_ENUMPvj @0xf3c78 size 208
```

208-byte functions cannot hold a 0x28000-byte ISP NVRAM image; they fill it at run time.

### 2.7 DSP: no evidence anywhere (negative result, reproduced)

```sh
$ for l in libcam.iopipe.so libcam.hal3a.v3.so libcamalgo.so lib3a.so \
           libcameracustom.so libcamdrv.so; do
      printf "%-24s %s\n" "$l" "$(grep -aic dsp $M/zip/vendor-libs/$l)"; done
libcam.iopipe.so         0
libcam.hal3a.v3.so       0
libcamalgo.so            0
lib3a.so                 0
libcameracustom.so       0
libcamdrv.so             0
$ grep -ril dsp $M/src/downstream-grandpplte/drivers/misc/mediatek/cameraisp/ | wc -l
0
$ grep -ic dsp $M/zip/vmlinux.strings
45
$ grep -i dsp $M/zip/vmlinux.strings | grep -ic 'cam\|isp\|imgsys'
0
```

The 45 ROM hits are connectivity/audio (`ccci_set_dsp_region_protection`,
`mt6627_DspPatch`, `snd_pcm_oss.adsp_map`, `CONN_DSP_JTAG`). The camera post-processing
stage evidenced by the symbols above is P2/DIP: `MIXER3`, `PCA`, `RPG`, `NBC`, `SEEE`,
`G2C`, plus the P1 raw-domain blocks `OBC`, `BNR`, `LSC`, `RPG`, `CFA`. **There is no DSP
tuning data or DSP pipeline to recover — that is the missing input, and none was invented.**

### 2.8 Tooling used

* `llvm-readelf -h -lW -SW -sW -rW` on both libraries (headers, segments, sections,
  dynsym, relocations) — all addresses above.
* `llvm-objdump -d --triple=thumbv7a-none-eabi` over the windows quoted in §2.2/§2.4/§2.5
  plus a full `.text` dump of `libcam.iopipe.so` used to locate the GOT-slot accesses.
* Ghidra 12.1.2 headless:
  `analyzeHeadless $M2/tmp/iopipe/ghproj iopipe -import …/libcam.iopipe.so
   -scriptPath …/ghidra_scripts -postScript IopipeRefs.java -deleteProject`
  (script printed xrefs to the six data symbols and decompiled the referencing
  functions). Its decompiler folds neither the PC-relative GOT literals nor the
  `add rN, pc` pairs (`DAT_0002ffd0 + 0x2fd52`), so the instruction-accurate work in §2.2
  was done with `llvm-objdump` instead. Only one xref survived resolution
  (0x2fe9c → `mPortIdxMappingAlltoP2`), which was not the access path the code uses.

## 3. Implementation

### 3.1 `iopipe_elf.[ch]` — bounded ELF32 LSB ARM reader

`iopipe_elf_open/close`, `iopipe_elf_sym_count/sym_at/sym_find/sym_count_matching`
(dynsym), `iopipe_elf_vaddr_to_off` (PT_LOAD only, returns the segment index),
`iopipe_elf_read_vaddr`, `iopipe_elf_section_of/by_name/section_info`,
`iopipe_elf_str_at`, `iopipe_elf_fits`, plus `iopipe_libset_init/get/free` (a 6-entry
cache so `--lib-dir` is resolved once). Validates ELF class/data/machine/type, program
and section header sizes against the file size, and every table read against the file.
Failure codes: `-ELIBBAD` malformed ELF, `-ENOENT` symbol/section/address not found,
`-ENODATA` mapped but no file bytes (`.bss`), `-ERANGE` past `p_filesz` or bad size,
`-ENOMEM`, `-EIO`.

### 3.2 `iopipe_portmap.[ch]` — the six pipe tables

`iopipe_pm_tables[]` (symbol, expected size, proved record width, field labels, the
divisor evidence string), `iopipe_pm_resolve`, `iopipe_pm_dump`, `iopipe_pm_verify`,
`iopipe_pm_check_all`, `iopipe_pm_by_name`. Verification per table: symbol is
`STT_OBJECT`, `st_size` matches, the whole object is file-backed by one `PT_LOAD`,
`size % rec_size == 0`, and the content invariant (§2.2). `check_all` additionally proves
`mPortIdxMappingAlltoP2` ⟷ `mPortIdxMappingP2toAll` are inverses. Output: symbol, vaddr,
size, section, segment index, record count, decoded fields, raw words, and the
`mSwHwPathMapping` name strings read from `.rodata`.

### 3.3 `iopipe_mgr.[ch]` — `ISP_MGR_*` catalogue

Parses Itanium-mangled names with their length prefixes (so `SL2`, `MIXER3`, `NR3D`,
`G2C_SHADE`, `CTL_EN_P1_D` are never split wrongly), decodes `ESensorDev_T` /
`ESensorTG_T` / `EPCAMode_T` values and the `ISP_NVRAM_<T>_T` template argument of
`put()`/`get()`, dedupes, sorts and prints text / CSV / JSON. Live output: **213 rows,
25 module tokens, 90 template-instance rows, 123 method rows, 24 distinct NVRAM types**
(BPC, BPC_RMM, NR1, ANR, CCR, CCM, CFA, CTL_EN_P1, CTL_EN_P1_D, CTL_EN_P2, EE, SE, G2C,
G2C_SHADE, GGM, LSC, MFB, MIXER3, NR3D, OBC, PCA, PGN, RPG, SL2). `iopipe_mgr_expect()`
is the selftest hook. Note: the SEE module's class is spelled `ISP_MGR_SEEE` in the
library, hence the token `SEEE`.

### 3.4 `iopipe_regmap.[ch]` — enable-word programming

The 19-record static table of §2.4 plus decoders for the three forms actually used
(`MOVW` T3, `BFI` T1, `ORR`-immediate T1) and a Thumb-2 length function. 
`iopipe_regmap_verify` re-reads and re-decodes all of it; `iopipe_regmap_unproven`
proves the negative for the remaining modules; `iopipe_regmap_reg_label` names the word.

### 3.5 `iopipe_cq.[ch]` — CQ0C builder

Reuses, verbatim, the encodings documented in
`tools/mediatek/camera/libcamdrv/camdrv_cq.c:64-72` (definitions) and cited there from
`libcamdrv.so` (`camdrv_cq.c:20-45`), which follow
`drivers/misc/mediatek/cameraisp/src/mt6735/inc/camera_isp_D1.h:735-758`:
`IOPIPE_CQ_INST_IMGO_BASE 0x7300`, `RRZO 0x7320`, `IMGO_D 0x74d4`, `RRZO_D 0x74f4`,
`NEXT_CQ0C 0x40b4`, `NEXT_CQ0C2 0x40cc`, `TABLE_END 0x4184`, `END 0xfc000000`.
`iopipe_cq_build(out, max, writes, n)` emits `n` `{inst, data_ptr_pa}` pairs plus the end
marker and returns `n+1`; `iopipe_cq_reg_valid` accepts only the userspace CAM window
`[0x4000, 0x10000)` documented in
`drivers/media/platform/mediatek/isp/mtk-isp-regs.h` (`MTK_ISP_CAM_ADDR_MIN/MAX`).
Failure codes `-EINVAL` (bad register / null or misaligned operand / empty list),
`-ENOSPC` (output too small).

### 3.6 `iopipe_tuning.[ch]` — static tuning tables

Allow-list of 15 symbols (§2.6) with expected sizes; decodes `ShadingATNTable`
(21×6 s32 plus the `v/v[5]` ratios) and the AWB-log/GMA arrays (count + first entries),
prints annotated hex where no layout is cited, and reports `.bss` objects as
"runtime only: no file bytes" via the `-ENODATA` path. `iopipe_tuning_accessors()`
machine-lists the per-sensor default generators.

### 3.7 `main.c` — CLI

`iopipe <portmap|modules|regmap|cq|tuning|selftest>` with `--lib-dir DIR`,
`--table NAME`, `--csv`, `--json`, `--write REG:PA` (repeatable), `--accessors`,
`-h`. Library directory precedence: `--lib-dir`, `$IOPIPE_LIBDIR`, then
`IOPIPE_DEFAULT_LIBDIR` (set by the Makefile from `LIBDIR`, overridable with
`make LIBDIR=…`). Exit status 0/1/2.

### 3.8 `iopipe_selftest.c` — `iopipe selftest`

41 explicit checks plus 26 per-object verification lines, all against the real ELFs:
6 ELF-reader paths (including `-ENOENT` for an unmapped vaddr and `-ENODATA` for `.bss`),
the 6 port maps + inverse property (7 lines), 10 catalogue expectations (including the
negative "wrong NVRAM type"), 3 regmap groups — 19 records re-read and re-decoded
(19 lines), complete unproven accounting (4 lines), 6 decoder KATs — 11 CQ0C KATs
(encodings, window, ENOSPC/EINVAL paths) and 2 tuning groups (15 symbol lines).

## 4. Parity matrix

| behaviour | reference | Nura before | after | status |
|---|---|---|---|---|
| All↔P2 port index maps (34/17 entries) | `mPortIdxMappingAlltoP2` 0x3000c/272, `mPortIdxMappingP2toAll` 0x304fc/136 | not decoded | decoded, inverse-checked | done |
| Software scenario → hw path table (43 entries + names) | `mSwHwPathMapping` 0x3012c/860; stride 0x14 @0x11686/0x11698 | not decoded | decoded with names | done |
| Port type map (2 entries) | `mPortTypeMapping` 0x30488/16 | not decoded | decoded (pair semantics [INFERENCE]) | done |
| Transform map (8 entries) | `mTransformMapping` 0x3049c/96 | not decoded | decoded (3-field factorisation content-proved) | done |
| Pipe timeout | `nsTimeoutToWait` 0x30120/8 = 3e6 ns | not decoded | decoded | done |
| Module catalogue (token/instance/NVRAM type) | 213 dynsym rows in `libcam.hal3a.v3.so` | not decoded | text/CSV/JSON | done |
| P1 / P1_D / P2 module-enable words | 19 records, §2.4 | not decoded | decoded + machine-verified | done |
| Always-on module (CFA) enable | `orr r1,r0,#0x8` @0x69a08 | not decoded | decoded | done |
| Per-module *value* programming (BNR/LSC/… register payloads) | `apply(profile, isp_reg_t*)` loops over runtime pairs (0x6ce50, 0x6d1a4) | n/a | reported as runtime data | deferred — missing input: a static address→value table; none exists in the ELF |
| MIXER3 / NR3D / MFB / G2C_SHADE enable programming | their `apply` bodies contain no register immediate | not decoded | enumerated as unproven with the reason | deferred — missing input: the addresses come from NVRAM at run time |
| FLK window / FKO DMA address | `ISP_MGR_FLK_CONFIG` methods are 2/4-byte aliases into `_ZThn4_…doDataCb` | n/a | enumerated as unproven | deferred — missing input: no implementation in this library |
| AE/AWB stat config registers | `config(int, AE_STAT_PARAM_T&)` / `(AWB_STAT_CONFIG_T&, int, int)` take runtime structs | n/a | catalogued only | deferred — missing input: the register list is not in the ELF |
| DSP tuning / pipeline | no `dsp` in any of the six libraries, the downstream cameraisp tree or the ROM camera strings (§2.7) | absent | documented as absent | deferred — missing input: no camera DSP exists in any available source |
| Per-sensor ISP NVRAM defaults | 208-byte `impGetDefaultData` generators (§2.6) | n/a | machine-listed, not reversed | deferred — missing input: data is generated at run time, not stored |
| CQ0C instruction words | `camdrv_cq.c:64-72` | ring builder only | generic writer + KATs | done |
| Static tuning tables | 15 symbols, 9 file-backed | not decoded | decoded/dumped | done |
| `.bss` tuning objects | 6 symbols | n/a | reported as runtime-only | done |
| Kernel TPIPE register constants | kernel reads `ISP_REG_EN1` for a dump only (`mtk-isp.c:2833`); CAM 0x4004 = ISP 0x0004 already defined | n/a | no change needed | done (no-op, §6.4) |

## 5. Build evidence

Host tool (system gcc, `-O2 -Wall -Wextra -Werror`):

```sh
cd $M2/worktrees/rem2-iopipe/tools/mediatek/camera/iopipe
rm -rf build
make -j2            # rc=0, 0 warnings, 0 errors
make check          # rc=0, 41 x "selftest: ok", 0 FAIL
```

Logs: `$M/logs/rem2-iopipe-build.log` (12 lines: 8 compiles, 1 archive, 2 links, 1 blank; no diagnostics) and
`$M/logs/rem2-iopipe-check.log` (full selftest + the shell assertions).

Parent wiring:

```sh
cd $M2/worktrees/rem2-iopipe/tools/mediatek/camera
make all    # rc=0 — libcamdrv.a/.so, camdrv, camservice, otp, then iopipe
make check  # rc=0 — nodev-check (ENODEV), compat32 abi32, otp check, iopipe check
```

Kernel: **not built and not modified** — `drivers/media/platform/mediatek/isp/mtk-isp-regs.h`
was deliberately left untouched (§4 last row, §6.4), so the object-compile step of the
contract does not apply.

Patch application check (the parity baseline is uncommitted, see §6.5):

```sh
mkdir -p /tmp/pt/tools/mediatek/camera && cp <baseline Makefile> /tmp/pt/tools/mediatek/camera/Makefile
cd /tmp/pt && git apply --whitespace=error-all --check $M/patches/rem2-iopipe.patch   # rc=0
```

## 6. Hardware-unverified items and deliberate deviations

1. **Field names in the pipe tables are `[INFERENCE]`.** The offsets, sizes, record
   counts and raw words are machine-verified; the labels `all_port/p2_port`,
   `sw_path/name_ptr/f2..f4`, `transform/f1/f2`, `port/type` are my reading of the
   content. `mSwHwPathMapping` fields 2..4 are printed raw because the only code that
   reads them (0x116a2, `ldr.w r1,[lr,#0x8]`) passes them to a virtual call whose
   semantics are not recoverable from the ELF.
2. **`mTransformMapping`/`mPortTypeMapping` widths are content-proved, not code-proved.**
   No GOT-slot access to these two objects survived in the disassembly (only
   `mPortIdxMappingP2toAll` at 0x10de0 and `nsTimeoutToWait` at 0x15d48 could be located
   through their slots). For `mTransformMapping` only the 8×3 factorisation yields a
   dense ascending index, and for `mPortTypeMapping` only 2×2 yields an involution; both
   properties are asserted at run time, so a wrong divisor fails the selftest rather
   than silently mis-reporting.
3. **"BFI source is the module enable flag" is `[INFERENCE]`.** What is proven is the
   instruction, the bit position, the register address and the owning `CTL_EN_*` manager
   called in the same function. The downstream GPL tree has no TPIPE register map
   (`grep -rn CTL_EN_P1 drivers/` finds only `camera_isp_D1.c:1402`'s comment), so the
   *meaning* of each bit rests on the pattern being identical across all 15 modules.
4. **No kernel change, deliberately.** The evidence yields CAM 0x4004/0x4008/0x4010/
   0x4014/0x4018 and their bits, all of which the *userspace* 3A library writes through
   TuningMgr; the kernel driver only ever reads `ISP_REG_EN1` for a debug line
   (`mtk-isp.c:2833`) and never writes a module-enable word. Adding the constants would
   create unused, unexercised kernel API, and the P1 word is already present as
   `ISP_REG_EN1` (0x0004 in the ISP window = CAM 0x4004). Nothing was invented into the
   header.
5. **Patch composition deviation.** The baseline payload is applied to the working tree
   but not committed, and `tools/mediatek/camera/` is entirely untracked in this worktree,
   so a literal `git add -A && git diff --cached --binary` would emit the whole parity
   baseline (≈ 100 k lines) instead of this leaf. I staged only
   `tools/mediatek/camera/iopipe/` (`git add -A -- tools/mediatek/camera/iopipe`) and
   prepended a minimal two-hunk diff of `tools/mediatek/camera/Makefile` (the same file
   the `otp` leaf wired in), so the patch applies cleanly on top of the baseline and does
   not duplicate it. Everything is byte-identical to what is in the worktree
   (`git status --porcelain` shows only the 15 `iopipe/` files plus the pre-existing
   baseline entries).
6. **`LIBDIR` is a build-time default.** The Makefile passes
   `-DIOPIPE_DEFAULT_LIBDIR="$(LIBDIR)"` (default
   `$M/zip/vendor-libs`), so no absolute path is baked into the sources and
   `make LIBDIR=…` points it elsewhere; at run time `--lib-dir`/`$IOPIPE_LIBDIR` win.
7. **Never executed on hardware.** Nothing here has been run against a device: the tool
   only reads ELF files, and the register records describe what the *vendor library*
   does, not something this tree programs. The one behavioural claim that would need a
   device to confirm is that writing these enable bits is sufficient to bring a module
   out of reset; the reset sequencing lives in the vendor HAL and is out of scope.

## 7. DTS-FRAGMENT

None required. This leaf adds only a host tool that reads the shipped vendor ELFs; it
adds no device tree node or property, and makes no kernel change.

## 8. Patch

`$M/patches/rem2-iopipe.patch` (108,850 bytes; 16 file diffs: 1 modified
`tools/mediatek/camera/Makefile` hunk pair + 15 new `tools/mediatek/camera/iopipe/`
files).
