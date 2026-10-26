# WCN FM RDS: raw-record ABI + userspace group decoder

Worktree: `$M/worktrees/wcn-gap-fm-rds` (baseline `46d91e872 baseline: wcn-subsystem.patch`)
Status: complete; RDS group decoding is closed in userspace. No kernel driver
behaviour was changed and no additional kernel hook was required.

## 1. Files changed

| File | Change |
|---|---|
| `include/uapi/linux/mtk_fm_rds.h` | NEW. Formal raw-record ABI (148-byte record, good-block bitmap, read/poll semantics). |
| `drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.h` | Additive: include the UAPI header, drop the duplicate struct definitions, add `static_assert` on the 12-byte block and 148-byte record. No read/poll code touched. |
| `tools/mediatek/wcn/fm_rds/rds_decode.h` | NEW. Reusable decoder API. |
| `tools/mediatek/wcn/fm_rds/rds_decode.c` | NEW. Group decoding/assembly state machine. |
| `tools/mediatek/wcn/fm_rds/fm_rds_decode.c` | NEW. CLI: `/dev/radio0` or file input, text/JSON output. |
| `tools/mediatek/wcn/fm_rds/selftest.c` | NEW. Synthetic-record self-test (`--selftest`, `make check`). |
| `tools/mediatek/wcn/fm_rds/Makefile` | NEW. Host build, `-O2 -Wall -Wextra -Werror`. |

`mtk_wcn_fm_chip.c`, `mtk_wcn_fm_link.c`, `mtk_wcn_fm_v4l2.c` and all audio files
are untouched. The header hunk is in the include block (lines 22-31) and in the
type-definition region (line 290), both outside the read/poll path owned by
`FmI2s`.

## 2. Evidence

### 2.1 Downstream (Samsung/MTK 3.18)

Path prefix `src/downstream-grandpplte/drivers/misc/mediatek/fmradio/`:

* `inc/fm_rds.h:19-22` - good-block bits in the `crc` word
  (`A=0x08, B=0x04, C=0x02, D=0x01`).
* `inc/fm_rds.h:32-39` - `rds_packet_t` = `{blkA, blkB, blkC, blkD, cbc, crc}`.
* `inc/fm_rds.h:41-45` - `rds_rx_t` = `{sin, cos, data[12]}`; `inc/fm_rds.h:124-128`
  `rds_raw_t.data[148]` confirms the record size.
* `inc/fm_rds.h:54-57` - version A/B encoding (`B0` bit).
* `core/fm_rds_parser.c:67-88` `rds_cnt_get`, `:90-120` `rds_grp_get`,
  `:122-143` `rds_checksum_check`, `:209-243` `rds_grp_type_get`,
  `:359-396` PI, `:398-435` PTY, `:437-474` TP, `:476-504` TA, `:506-534` M/S,
  `:536-563` PS address/DI, `:565-681` PS assembly, `:683-782` RT segment fetch,
  `:784-892` RT two-copy assembly/terminator, `:894-1097` AF list,
  `:1099-1161` DI, `:1296-1347` 1A ECC/language + PIN, `:1374-1606` 2A/2B,
  `:1608-1666` 3A ODA, `:1668-1750` RT+ ODA, `:1752-1791` 4A CT,
  `:1793-1976` 14A/B EON, `:1978-2139` `rds_parser` dispatch.
* `core/fm_link.c:222-408` - firmware event parser; RDS_RX_DATA payload is
  `rds_rx_t` verbatim.
* `mt6627/pub/mt6627_fm_rds.c:276-292` - downstream `rds_parser` /
  `rds_log_get` wrappers.
* `core/fm_module.c:703-725` - downstream read() ABI: userspace reads the raw
  `rds_raw_t` (`data[148]`).

### 2.2 ROM (shipped SM-G532M kernel, `$M/zip/vmlinux.elf`)

`llvm-nm -S`:
```
c0636870 t fm_rds_parser
c0637de0 T fm_rds_read
c0647cc8 T MT6627fm_rds_ops_register
c063a2dc T fm_rds_group_cnt_get
```
The shipped kernel contains the same downstream parser and its read path, so
the record semantics ported here are the ones the device actually used.

### 2.3 Current kernel implementation

* `mtk_wcn_fm_link.c:114-132` - one event payload is zero-padded to
  `sizeof(struct mtk_wcn_fm_rds_data)` and queued.
* `mtk_wcn_fm_v4l2.c:529-561` - `read()` rejects `count < 148`, always returns a
  whole number of records, blocks unless `O_NONBLOCK`, returns `-EIO` after
  power down.
* `mtk_wcn_fm_v4l2.c:564-580` - `poll()` reports `EPOLLIN|EPOLLRDNORM`, and
  `EPOLLHUP` when the chip is powered down.
* `mtk_wcn_fm_chip.c:178` - existing `static_assert(sizeof(...) == 148)`.

## 3. Raw record ABI (exactly as delivered)

Little-endian, native struct copy from the kernel; one record:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `sin` - demodulator sample, not RDS data |
| 2 | 2 | `cos` - demodulator sample, not RDS data |
| 4 | 12 | `group[0]`: `blk_a` @0, `blk_b` @2, `blk_c` @4, `blk_d` @6, `cbc` @8, `crc` @10 |
| ... | ... | `group[1]` .. `group[11]` |
| 148 | | end of record |

* `crc` low nibble is the firmware good-block bitmap: `A=0x08, B=0x04,
  C=0x02, D=0x01` (downstream masks). There is **no separate validity field**;
  this bitmap is the validity representation and is passed through unchanged.
* `cbc` is the firmware correct-bit count. Downstream drops group 2 when it is
  non-zero; the decoder mirrors that and reports `rt_dropped_cbc`.
* The event payload may be shorter than 12 groups; the driver zero-fills the
  remainder, so unused slots are all-zero with `crc == 0`. A received group
  always has at least block B marked good, so `crc == 0` identifies padding.
* `read()`: `count` must be >= 148; return value is `n*148`, `0 < n*148 <= count`.
  Blocking read waits for a record; `O_NONBLOCK` gives `-EWOULDBLOCK`; power
  down wakes readers with `-EIO`. `poll()`: `EPOLLIN|EPOLLRDNORM` when queued,
  `EPOLLHUP` after power down.
* Version-B groups repeat PI in block C (C'); the decoder treats block C as an
  opaque C' value for 0B/1B/2B/5B/15B as downstream does.

The ABI is sufficient: the crc bitmap provides per-block validity and
zero-filled slots are distinguishable, so **no driver hook was added**. The only
kernel change is moving the layout into a UAPI header and asserting it at
compile time.

## 4. Parity matrix

Legend: DS = downstream `fm_rds_parser.c`; ROM = symbol present in shipped
vmlinux (all decoding below ran inside `fm_rds_parser`, `c0636870`).

| Behaviour | Downstream | ROM | Before | After | Status |
|---|---|---|---|---|---|
| Raw record ABI | `inc/fm_rds.h:32-45`, `core/fm_module.c:711-716` | `fm_rds_read` c0637de0 | structs only in driver-internal header | UAPI header + compile-time asserts | closed |
| Group type/version from B (version A vs B) | `:209-243` | yes | none | `rds_decode_group()` | closed |
| Block validity from crc bitmap | `:122-143` | yes | raw bytes only | `good` mask, per-block checks | closed |
| PI, PTY, TP from every group | `:359-474` | yes | none | common decoder fields | closed |
| 0A/0B: TA, M/S, DI, PS segment, PS assembly | `:476-681`, `:1163-1294` | yes | none | flags, 8-char PS | closed (PS emitted once per changed complete collection instead of DS's two-identical-buffer check) |
| 0A AF list (method A, count marker, filler, LF/MF skip) | `:894-1097` | yes | none | 25-entry list, deduped | closed (method B not classified; no tuned frequency input) |
| 0B PI in C' | `:1274-1294` | yes | none | version-B handling | closed |
| 1A slow labelling variant 0 ECC, variant 3 language | `:1296-1347` | yes | none | ECC, language, raw SLC data | closed (variants 1/2 TMC/paging IDs exposed only as `slc_data`) |
| 1A/1B PIN (day/hour/minute) | `:1322-1347` | yes | none | `pin_*` | closed |
| 2A/2B RadioText, 4/2 chars per segment, A/B flag | `:1374-1606` | yes | none | 64/32-char assembly, CR terminator | closed (cbc!=0 groups dropped, as DS) |
| 3A ODA identification (group, message, AID) | `:1608-1666`, `:1978-2139` | yes | none | group, message, AID | closed |
| 4A clock-time (MJD, UTC, local offset) | `:1752-1791` | yes | none | date/time/offset | closed |
| 5A/5B transparent data channels | not implemented (falls through) | yes (dispatch) | none | address + 4/2 data bytes | closed from EN 50067 3.1.5.4 / 3.1.5.5; not hardware-verified |
| 13A enhanced radio paging | routed to ODA only | yes (dispatch) | none | STY + 25 notification bits | closed from EN 50067 3.1.5.17 + annex M figures M.11a/b; CS/IT/S1/S2 not decoded |
| 14A EON: PS(ON), AF(ON), mapped freq, linkage, PTY/TA, PIN, broadcaster | `:1793-1976` | yes | none | all variants 0-15 needed by the tool | closed |
| 14B EON fast TA(ON) | `:1793-1976` (TA_ON bit 3) | yes | none | TA/TP | closed |
| 15B fast switching flags (block B and block D halves) | routed to ODA only | yes (dispatch) | none | both halves decoded | closed from EN 50067 3.1.5.21 / figure 39 |
| 15A | routed to ODA only | yes (dispatch) | none | reported as `15A_UNDEFINED`, raw blocks only | not portable: EN 50067:1998 3.1.5.20 declares 15A unavailable (RBDS-only) |
| Unknown/in-house/ODA groups 6-12, 13B | raw dispatch | yes | raw bytes | raw blocks preserved (text/JSON) | closed as pass-through |
| Block errors | crc mask checks | yes | none | bad-B reported; partial-good marked | closed |

## 5. Build and test

Environment: `M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`,
`PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"`, `clang` from `/usr/bin`.

Kernel (config: `cp $M/nura.config .config`, `./scripts/config -e ... -m MTK_WCN_FM`,
`make ARCH=arm64 LLVM=1 olddefconfig`):

```
$ make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm OBJCOPY=aarch64-linux-gnu-objcopy \
    OBJDUMP=aarch64-linux-gnu-objdump READELF=aarch64-linux-gnu-readelf \
    STRIP=aarch64-linux-gnu-strip -j4 drivers/misc/mediatek/connectivity/fm/
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_link.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_chip.o
  CC [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm_v4l2.o
  LD [M]  drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o
```

Module link attempt (required): `make ... drivers/.../mtk_wcn_fm.ko` failed at
modpost only because the full tree has no `vmlinux.o`/`Module.symvers`
(`ERROR: modpost: "_raw_spin_lock" ... undefined`). With
`KBUILD_MODPOST_WARN=1 M=drivers/misc/mediatek/connectivity/fm` the final link
completed:

```
  LD [M]  mtk_wcn_fm.ko
  BTF [M] mtk_wcn_fm.ko
Skipping BTF generation for mtk_wcn_fm.ko due to unavailability of vmlinux
-rw-r--r-- ... drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.ko
```
The unresolved-symbol warnings are the expected environment limitation (no full
kernel build); no symbol originates from this change.

Userspace (host gcc 16.2.1 and clang):

```
$ cd tools/mediatek/wcn/fm_rds
$ make CC=clang
  clang -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -c -o fm_rds_decode.o fm_rds_decode.c
  clang ... -c -o rds_decode.o rds_decode.c
  clang ... -c -o selftest.o selftest.c
  clang ... -o fm_rds_decode fm_rds_decode.o rds_decode.o selftest.o
$ make check
./fm_rds_decode --selftest
selftest: all checks passed
```
Also built with host `cc` (gcc 16.2.1); both clean under `-Werror`.
`synthetic /tmp/test.rds` (generated in the shell only, not shipped) was decoded
in text and JSON modes; JSON validated with `python3 -c 'json.loads'`; a
149-byte file correctly reported
`ends with a partial record (2 of 148 bytes)` and exited 2.

Self-test coverage (generated in code, no fixtures): 0A PS/AF/PI/PTY/DI, 0B PS,
2A RT + A/B restart, 2B RT, 2A cbc drop, 1A ECC/language/PIN, 1B PIN, 3A ODA,
4A CT round trip (2026-10-24 19:57, +02:00), 5A/5B TDC, 13A STY 0/1 notification
bits, 14A EON PS/AF/PTY-TA, 14B TA, 15B both flag halves, bad block B, zero
padding, text sanitising.

## 6. Inferred / hardware-unverified

* `[INFERENCE]` Firmware good-block bitmap semantics are taken from the
  downstream masks and its `rds_checksum_check()`; they were not verified by
  disassembling the ROM RDS producer or by hardware capture.
* EN 50067-derived layouts (5A/5B, 13A STY/notification bits, 15B flag halves)
  are not exercised by downstream code and were not observed from hardware.
* 13A: block B `CS/IT/S1/S2` and block C bits 15..9 are not decoded. The
  standard's own figures M.11a/b are ambiguous about those bit positions; only
  STY (B bits 2..0) and the 25 notification bits (C bits 8..0 + D) are decoded.
* 13A STY 1/2 `paging_first_bit` uses 25 (high half of the 50-bit map); the
  notification value is the same 25-bit payload as STY 0.
* AF method A only; method B classification needs the tuned frequency, which the
  decoder does not receive. AF list is reported in received order (deduped),
  LF/MF code 250 is skipped.
* EN 50067 Annex E character set: only the printable ASCII subset is rendered;
  other bytes become `.` (raw bytes remain available via `--raw`/JSON `blocks`).
* PS emission is once per changed complete 8-character collection; downstream
  additionally requires two identical collections before display. This is a
  display-policy difference, not a decoding difference.
* 1A variants 1/2 (TMC ID, paging ID), 6 (broadcaster), 7 (EWS) are exposed as
  `slc_variant` + `slc_data`; no named fields (downstream only special-cases
  variant 0/3 too).
* Real hardware was not available in this environment; the `--device` path
  (VIDIOC_QUERYCAP, VIDIOC_S_CTRL `V4L2_CID_RDS_RECEPTION`, poll/read loop) is
  compile-tested only.

## 7. DTS fragment

No DTS change is required. The FM node already exists from the WCN baseline and
the decoder is pure userspace on the existing `/dev/radio0` interface; nothing
new is bound.

```dts
/* none */
```

## 8. What is closed / still blocked

Closed:
* Raw RDS ABI is formal and compile-time-checked (`include/uapi/linux/mtk_fm_rds.h`);
  driver read()/poll() semantics unchanged.
* Userspace decoding for 0A/0B, 1A/1B, 2A/2B, 3A, 4A, 5A/5B, 13A, 14A/14B,
  15B, with PI/PTY/TP/TA/MS/DI/PS/AF/ECC/PIN/RT/ODA/CT/TDC/EON/paging/fast
  switching, version-B C' handling, good-block error detection.
* CLI with device and file input, RDS enable via V4L2 control, stable text and
  NDJSON modes, `make check` self-test; host gcc and clang clean.

Still blocked / deliberately out of scope:
* RDS-TMC (group 8A, ALERT-C) decoding: needs ENV 12313-1/ISO 14819, which is not
  in the downstream tree; downstream only routes it as ODA. Raw ODA blocks are
  passed through.
* RT+ ODA tag decoding (AID 0x4BD7): downstream `rds_retrieve_goda()` exists but
  the assignment scoped 3A to identification only; the tool reports the AID and
  raw ODA blocks.
* 15A: EN 50067:1998 deliberately leaves it unspecified (RBDS fast PS / RDS2
  Long PS in later IEC editions); no portable definition in the available
  sources, so it is reported raw.
* 13A CS/IT/sorting fields: bit positions not unambiguous in the available
  standard figures.
* Full EN 50067 Annex E character set.
* AF method B and any tuner-dependent behaviour.

## 9. Patches

* Userspace: `$M/patches/wcn-gap-fm-rds.patch`
  (`tools/mediatek/wcn/fm_rds/{Makefile,rds_decode.h,rds_decode.c,fm_rds_decode.c,selftest.c}`)
* Kernel: `$M/patches/wcn-gap-fm-rds-kernel.patch`
  (`include/uapi/linux/mtk_fm_rds.h`, `mtk_wcn_fm.h`)

Both were generated with `git add -N` + `git diff --binary`, contain no binary
hunks, no `.o`/`.ko`/`.cmd`/`Module.symvers`/`.mod.c` hunks and no baseline
files, and reverse-apply cleanly. They are independent and can be applied in
either order. For the `FmI2s` peer: the `mtk_wcn_fm.h` hunk touches only the
include block near line 22-31 and the type-definition region at line 290; the
read()/poll() path and all other FM code are unchanged.
