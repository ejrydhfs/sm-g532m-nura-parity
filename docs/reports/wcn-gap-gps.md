# WCN GPS vendor ABI gap (`/dev/stpgps`)

Worktree: `$M/worktrees/wcn-gap-gps` (baseline `6241bb92a baseline: wcn-subsystem.patch`)
Patch: `$M/patches/wcn-gap-gps.patch`

## 1. Files changed

| File | Change |
|---|---|
| `drivers/misc/mediatek/connectivity/gps/gps.c` | Shared GPS-core rework: owner arbitration (`GNSS` / vendor), one STP callbacks, reset handling, rfkill/PM on the shared state, GNSS device made optional (`CONFIG_GNSS=n`). |
| `drivers/misc/mediatek/connectivity/gps/stp_chrdev_gps.c` | New vendor char device `/dev/stpgps`: blocking `read`/`write`, downstream ioctl numbers 0/1/6/7/8/9/10/11, `compat_ioctl`, dynamic major + class + `device_create`. |
| `drivers/misc/mediatek/connectivity/gps/gps.h` | New internal interface shared by the two faces (`struct mtk_gps`, owner enum, channel/claim/release API). |
| `drivers/misc/mediatek/connectivity/gps/Makefile` | `mtk_wcn_gps-y := gps.o stp_chrdev_gps.o`. |
| `drivers/misc/mediatek/connectivity/gps/Kconfig` | Dropped `depends on GNSS`; the vendor chardev builds with `CONFIG_GNSS=n`. |

No DTS file was edited.

## 2. ROM / downstream evidence

Downstream (`$M/src/downstream-grandpplte`, 3.18):

- `drivers/misc/mediatek/connectivity/gps/stp_chrdev_gps.c:24` — `GPS_DEV_MAJOR 191`
- `stp_chrdev_gps.c:31-37` — ioctl numbers 6..11 (`COMBO_IOC_*`), plus raw commands 0/1
- `stp_chrdev_gps.c:88-99` — `rtc_GPS_low_power_detected()`
- `stp_chrdev_gps.c:103-175` — `GPS_write()` (STP send, `-ENOSPC` when no window)
- `stp_chrdev_gps.c:177-250` — `GPS_read()` (STP receive + wait queue)
- `stp_chrdev_gps.c:255-350` — `GPS_unlocked_ioctl()` switch
- `stp_chrdev_gps.c:257-271` — commands 0/1 return `-EINVAL`
- `stp_chrdev_gps.c:273-315` — commands 6/7/8 (`mtk_wcn_wmt_hwver_get()`, `mtk_wcn_wmt_ic_info_get(WMTCHIN_HWVER/FWVER)`)
- `stp_chrdev_gps.c:321-326` — command 11, `#if SOC_CO_CLOCK_FLAG` / `mtk_wcn_wmt_co_clock_flag_get()`
- `stp_chrdev_gps.c:330-343` — command 9, `ioremap(0x10206198)` and the word is *returned* as the ioctl result
- `stp_chrdev_gps.c:345-348` — unknown command returns `-EFAULT`
- `stp_chrdev_gps.c:354-387` — reset callback (`WMTRSTMSG_RESET_START/END/END_FAIL`)
- `stp_chrdev_gps.c:389-457` — open: reset gate `-EPERM`, WMT function on, STP event registration
- `stp_chrdev_gps.c:459-487` — close: unregister events, WMT function off
- `stp_chrdev_gps.c:521-560` — `register_chrdev_region`, `cdev_add`, `class_create("stpgps")`, `device_create(..., "stpgps")`
- `arch/arm/boot/dts/mt6735.dtsi:1637-1639` — `compatible = "mediatek,mt3326-gps"`
- `arch/arm/boot/dts/mt6735.dtsi:462-465` — `efusec@10206000`, `compatible = "mediatek,efusec"` (the 0x10206198 word is base + 0x198)

ROM (`$M/zip/vmlinux.elf` with `$M/llvmpkg/usr/bin/llvm-nm`, `$M/zip/vmlinux.strings`):

- `vmlinux.strings:43666` — device name `stpgps`
- `vmlinux.strings:43642-43651` — `GPS_ioctl(): cmd (%d)`, the disable-STP / raw-data / hw-version / fw-version / unknown-cmd strings, tagged `[name:stp_chrdev_gps&]`
- `GPS_unlocked_ioctl` = `c05fa03c`, `GPS_compat_ioctl` = `c05fa3e4`, `GPS_read` = `c05f9e38`, `GPS_write` = `c05f9d2c`, `GPS_open` = `c05fa5a4`, `GPS_close` = `c05fa480`, `rtc_GPS_low_power_detected` = `c05fa8b0`, `mtk_wcn_stpgps_drv_init` = `c05fa8dc`, `mtk_wcn_stpgps_drv_exit` = `c05faa1c`

## 3. Parity matrix

| Behaviour | Downstream | ROM | Before | After | Status |
|---|---|---|---|---|---|
| Vendor node `/dev/stpgps` | class+`device_create`, major 191/dynamic (`stp_chrdev_gps.c:521-560`) | name present (`vmlinux.strings:43666`) | absent | `alloc_chrdev_region` (dynamic; `stpgps_major` param for a fixed major), `class_create("stpgps")`, `device_create` | closed |
| Raw byte `read()` | dequeue STP GPS channel, block on event (`:177-250`) | `GPS_read` @ `c05f9e38` | absent | blocking `read`, bounded by `MTKSTP_BUFFER_SIZE`, interrupts/reset/channel-off return real errors | closed |
| Raw byte `write()` | copy ≤ `MTKSTP_BUFFER_SIZE`, return accepted bytes, `-ENOSPC` if 0 (`:103-175`) | `GPS_write` @ `c05f9d2c` | absent | same, plus `-EIO` during reset and `-ENODEV` when the link is down | closed |
| Single GPS link | downstream has only the vendor fd | HAL uses it | GNSS-only | exclusive owner: opening GNSS while vendor open (or vice versa) = `-EBUSY`; one STP callback branches on owner | closed |
| ioctl 0 (enable/disable STP) | `-EINVAL` (`:257-266`) | `GPS_unlocked_ioctl` | n/a | `-EINVAL` with explanatory comment | closed |
| ioctl 1 (raw data) | `-EINVAL` (`:268-271`) | same switch | n/a | `-EINVAL` | closed |
| ioctl 6 `COMBO_IOC_GPS_HWVER` | copy 4-byte `ENUM_WMTHWVER_TYPE_T` (`:273-283`) | same switch | n/a | copy `enum mtk_wcn_wmt_hwver` (4 bytes) | closed |
| ioctl 7 `..._IC_HW_VERSION` | `mtk_wcn_wmt_ic_info_get(WMTCHIN_HWVER)` (`:285-294`) | same switch | n/a | `mtk_wcn_wmt_ic_info_get(WMT_ICINFO_HWVER)`, u32 copy | closed in code; depends on WmtGap additive export (see §8) |
| ioctl 8 `..._IC_FW_VERSION` | `WMTCHIN_FWVER` (`:296-305`) | same switch | n/a | `mtk_wcn_wmt_ic_info_get(WMT_ICINFO_FWVER)` | same dependency |
| ioctl 9 `COMBO_IOC_D1_EFUSE_GET` | `ioremap(0x10206198)`, return word as ioctl result (`:330-343`) | same switch | n/a | `nvmem_cell_read("gps-efuse")`, word returned as ioctl result; no cell → `-EOPNOTSUPP` (never fabricated) | code closed; **hardware path blocked on missing nvmem provider** (§6) |
| ioctl 10 `COMBO_IOC_RTC_FLAG` | `rtc_GPS_low_power_detected()` = 0 in both branches (`:88-99`, `:307-312`) | `rtc_GPS_low_power_detected` @ `c05fa8b0` | n/a | returns 0 | closed |
| ioctl 11 `COMBO_IOC_CO_CLOCK_FLAG` | compile-time 0 with `SOC_CO_CLOCK_FLAG` disabled (`:321-326`) | same switch | n/a | live `mtk_wcn_wmt_co_clock_flag_get()` (the WMT core does carry `gen_conf.co_clock_flag`), 0/1 | closed in code; same WMT dependency |
| Unknown ioctl | `-EFAULT` (`:345-348`) | same | n/a | `-EFAULT` | closed |
| Reset notification | `retflag` gate, open `-EPERM` during reset (`:354-387`, `:397-400`) | reset callback symbols in ROM WMT | callback only toggled a flag unused by I/O | blocked `read` wakes and returns `-EIO`; `write` returns `-EIO`; open during reset `-EPERM`; retry after reset end | closed (vendor); GNSS has no error-injection API (see §8) |
| GNSS device | none | n/a | GNSS-only | unchanged behaviour, still `suspend` sysfs notify, now shares core and is optional at build time | closed |
| rfkill / PM / WMT power | function on before STP, off on close | n/a | GNSS-only | owner-based: rfkill block tears the channel down for either face; unblock restores it when an owner exists; PM notify retained | closed |
| Build with `CONFIG_GNSS=n` | vendor chardev independent of GNSS | n/a | impossible (`depends on GNSS`, unguarded `gnss_*`) | `gps.o` + `stp_chrdev_gps.o` compile and link with GNSS off | closed |

### ioctl -> implementation -> status

| cmd | Name | Implementation (this tree) | Status |
|---|---|---|---|
| 0 | STP enable/disable | `-EINVAL` (WMT owns STP) | implemented as downstream |
| 1 | send raw data | `-EINVAL` (raw mode disabled) | implemented as downstream |
| 6 | `COMBO_IOC_GPS_HWVER` | `mtk_wcn_wmt_hwver_get()` + `copy_to_user` | implemented |
| 7 | `COMBO_IOC_GPS_IC_HW_VERSION` | `mtk_wcn_wmt_ic_info_get(WMT_ICINFO_HWVER)` | implemented; needs WMT export |
| 8 | `COMBO_IOC_GPS_IC_FW_VERSION` | `mtk_wcn_wmt_ic_info_get(WMT_ICINFO_FWVER)` | implemented; needs WMT export |
| 9 | `COMBO_IOC_D1_EFUSE_GET` | `nvmem_cell_read()`, value returned as ioctl result | implemented; `-EOPNOTSUPP` without DT cell |
| 10 | `COMBO_IOC_RTC_FLAG` | returns 0 | implemented |
| 11 | `COMBO_IOC_CO_CLOCK_FLAG` | `mtk_wcn_wmt_co_clock_flag_get()` | implemented; needs WMT export |
| other | — | `-EFAULT` | implemented |

## 4. Compile verification

Toolchain: `clang` from `/usr/bin`, LLVM binutils from `$M/llvmpkg/usr/bin`.

Config setup exactly as instructed:
```
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV -e BT \
  -e CFG80211 -e RFKILL -e CRC32 -e SND_SOC -m MTK_BTIF -m MTK_WCN_WMT \
  -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
```

`CONFIG_GNSS=y` (objects + built-in object link):
```
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j4 \
  drivers/misc/mediatek/connectivity/gps/
  CC [M]  drivers/misc/mediatek/connectivity/gps/gps.o
  CC [M]  drivers/misc/mediatek/connectivity/gps/stp_chrdev_gps.o
  LD [M]  drivers/misc/mediatek/connectivity/gps/mtk_wcn_gps.o
```
Result: **0 errors, 0 warnings**.

`CONFIG_GNSS=n` (`./scripts/config -d GNSS`, `olddefconfig`, same make command):
```
  CC [M]  drivers/misc/mediatek/connectivity/gps/gps.o
  CC [M]  drivers/misc/mediatek/connectivity/gps/stp_chrdev_gps.o
  LD [M]  drivers/misc/mediatek/connectivity/gps/mtk_wcn_gps.o
```
Result: **0 errors, 0 warnings**.

Module final link (attempted):
```
make ... KBUILD_MODPOST_WARN=1 M=drivers/misc/mediatek/connectivity/gps modules
  LD [M]  mtk_wcn_gps.ko
  Skipping BTF generation for mtk_wcn_gps.ko due to unavailability of vmlinux
```
`mtk_wcn_gps.ko` (427064 bytes) was produced. modpost emitted "undefined symbol" warnings for kernel/WMT symbols because this worktree has no tree-wide `Module.symvers` (environment limitation: a full kernel build was not run). Among them are the two new WMT accessors, which are provided by the `wcn-gap-wmt` patch (see §8).

Note: `mtk_wcn_wmt.h` is owned by peer `WcnGap.WmtGap`, who confirmed the additive API
`u32 mtk_wcn_wmt_ic_info_get(enum mtk_wcn_wmt_icinfo)` / `bool mtk_wcn_wmt_co_clock_flag_get(void)` and
`enum mtk_wcn_wmt_icinfo { WMT_ICINFO_CHIPID=0, WMT_ICINFO_HWVER, WMT_ICINFO_MAPPINGHWVER, WMT_ICINFO_FWVER }`.
For the compile checks above an identical declaration block was inserted temporarily into the local header and reverted; it is **not** part of this patch. At integration the WMT patch must precede this one.

## 5. Inferred / hardware-unverified

- `[INFERENCE]` The ROM HAL calls the ioctl numbers and reads results as described by the downstream driver; the ROM kernel image contains the same switch strings and symbols, but no HAL binary was disassembled to re-confirm each argument direction.
- The efuse path is untested (no hardware, no provider). The exact missing input is listed in §6.
- `[INFERENCE]` `RTClow-power` flag is always 0 on this platform because the RTC call is commented out downstream; no `rtc_low_power_detected()` exists in this tree.
- STP blocking-read wakeup semantics are implemented with a sequence counter instead of downstream's boolean `flag`; this removes a lost-wakeup race and does not change the userspace-visible behaviour.
- Closing the char device does not wait for an in-flight STP callback beyond the callback's own spinlock; the callback only touches the spinlock-protected fields and wakes the wait queue.
- `read()` with `count == 0` returns 0 (standard kernel semantics); downstream would block. `write()` with `count == 0` keeps downstream's `-EFAULT`.

## 6. DTS fragment

The Nura `mt6735.dtsi` has no GPS node. Required fragment (inside the existing `soc` node):

```dts
/ {
	soc {
		gps: gps {
			compatible = "mediatek,mt3326-gps";
		};
	};
};
```

Downstream places the same node at `arch/arm/boot/dts/mt6735.dtsi:1637-1639`.

### Exact missing input for `COMBO_IOC_D1_EFUSE_GET`

The downstream word is `0x10206198` = efuse base `0x10206000` + `0x198`
(`arch/arm/boot/dts/mt6735.dtsi:462-465`). This tree has **no MT6735 efuse provider**:
`drivers/nvmem/mtk-efuse.c:112-118` only matches `mediatek,mt8173-efuse`,
`mediatek,mt8186-efuse` and generic `mediatek,efuse`, all with the MT8173/MT8186
register layout — not the downstream `mediatek,efusec` block.

To make ioctl 9 return a real value, the board tree needs:

1. an nvmem provider for `efusec@10206000` (compatible such as
   `mediatek,mt6735-efuse`) with the MT6735 efuse register layout; and
2. a 4-byte cell at offset `0x198`, e.g.

```dts
&efuse {
	gps_efuse: gps-efuse@198 {
		reg = <0x198 0x4>;
	};
};
```

3. and on the GPS node:

```dts
&gps {
	nvmem-cells = <&gps_efuse>;
	nvmem-cell-names = "gps-efuse";
};
```

Until then the driver returns `-EOPNOTSUPP` — never a fabricated word. The driver reads
the cell through `devm_nvmem_cell_get(dev, "gps-efuse")`; with `CONFIG_NVMEM=n` the
kernel's consumer stubs make probe succeed and the ioctl return `-EOPNOTSUPP`.

## 7. Patch

`$M/patches/wcn-gap-gps.patch`

## 8. What is closed / still blocked

Closed:
- `/dev/stpgps` vendor ABI over the single STP GPS channel: blocking raw read/write, all downstream ioctl numbers with the downstream return conventions, `compat_ioctl`, dynamic major/class/device.
- Exclusive-owner arbitration between the GNSS device and the vendor device, shared RX callback and shared suspend-notify/power state, rfkill/PM/WMT paths valid from either face.
- Reset handling: blocked vendor reads wake with `-EIO`, writes fail with `-EIO`, open during reset `-EPERM`.
- `CONFIG_GNSS=n` build of the vendor face.

Still blocked / external:
- **ioctl 9 hardware path**: no MT6735 nvmem provider or `gps-efuse` cell; exact DT/provider input is in §6. Returns `-EOPNOTSUPP` in the meantime.
- **ioctls 7/8/11 runtime**: depend on the `wcn-gap-wmt` patch's additive
  `mtk_wcn_wmt_ic_info_get()` / `mtk_wcn_wmt_co_clock_flag_get()` exports. The values exist in the WMT core (`wmt_lib_get_icinfo()`, `wmt_lib_co_clock()`); the exports are intentionally left in the WMT-owned files (peer agreement), so this patch must be applied after the WMT patch.
- **GNSS read error injection**: `struct gnss_operations` has no read-error callback; on reset/write the GNSS face reports `-EIO` where the API allows it, and buffered GNSS bytes are not discarded.
- No hardware/runtime validation was performed (no device boot), and the module link's unresolved-symbol warnings are the documented missing-tree-`Module.symvers` limitation.
