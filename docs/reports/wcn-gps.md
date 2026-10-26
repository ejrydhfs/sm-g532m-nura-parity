# wcn-gps — MediaTek WCN GPS driver (GNSS over STP + WMT)

## 1. Assigned driver and files changed

Target: `drivers/misc/mediatek/connectivity/gps/` in `$M/worktrees/wcn-gps`
(worktree of `src/nura-linux`, HEAD `046883c0c`).

| File | Change |
|---|---|
| `drivers/misc/mediatek/connectivity/gps/gps.c` | **new**, 501 lines: platform driver `mt3326-gps`, GNSS char device (`CONFIG_GNSS` core, `GNSS_TYPE_MTK`), WMT GPS function control, STP GPS channel (`GPS_TASK_INDX`), rfkill, PM, sysfs `suspend` |
| `drivers/misc/mediatek/connectivity/gps/Makefile` | added `mtk_wcn_gps-y := gps.o` / `obj-$(CONFIG_MTK_WCN_GPS) += mtk_wcn_gps.o` |

`Kconfig` was **not** changed: the frozen scaffold already carries
`depends on MTK_WCN_WMT`, `depends on GNSS`, `select RFKILL` and the
`MTK_WCN_GPS` tristate. No shared header, DTS or other driver was touched.

Frozen API used (no modification, no weakening):
`mtk_wcn_stp_is_ready`, `mtk_wcn_stp_send_data`, `mtk_wcn_stp_receive_data`,
`mtk_wcn_stp_register_event_cb`, `mtk_wcn_wmt_func_on/off`,
`mtk_wcn_wmt_msgcb_reg/unreg`, `GPS_TASK_INDX`, `MTKSTP_BUFFER_SIZE`,
`WMTDRV_GPS`, `WMTDRV_WMT`, `WMTMSG_RESET`.

Nura GNSS API used as-is: `gnss_allocate_device()`, `gnss_set_drvdata()`,
`gnss_get_drvdata()`, `gnss_register_device()`, `gnss_deregister_device()`,
`gnss_put_device()`, `gnss_insert_raw()`, `struct gnss_operations{open,close,
write_raw}`. No invented GNSS calls.

## 2. ROM evidence

Symbols from `$M/zip/vmlinux.elf` (`llvm-nm`, ELF32 little-endian ARM):

| Symbol | Address |
|---|---|
| `mtk_wcn_stpgps_drv_init` | `0xc05fa8dc` (T) |
| `mtk_wcn_stpgps_drv_exit` | `0xc05faa1c` (T) |
| `gps_cdev_rst_cb` | `0xc05fa73c` (t) |
| `mt3326_gps_probe` | `0xc05fc8e4` (t) |
| `mt3326_gps_mod_init` | `0xc0db3360` (t) |

`mt3326_gps_mod_init` calls `__platform_driver_register` with the static
`mt3326_gps_driver` (disassembly at `0xc0db3360`), i.e. the ROM builds the
GPS module as a platform driver bound through DT. `mt3326_gps_mod_exit` is
absent from kallsyms (built-in `module_exit` discarded). `mt3326_gps_power`
has no standalone symbol: it is inlined into `mt3326_gps_probe`, whose
disassembly at `0xc05fc8e4+0x64..0xa8` loads `hw->ext_power_on`, tests it for
NULL, calls it with `0` and then with `1`, then runs a 120-iteration delay
loop and stores `rdelay = 50` — exactly the source path in
`gps.c:919-923 → mt3326_gps_power()`.

Strings (`$M/zip/vmlinux.strings`):

| String | Line | Meaning |
|---|---|---|
| `mtk_stp_GPS_chrdev` | 43664 | STP GPS char device name (`stp_chrdev_gps.c:35`) |
| `stpgps` | 43666 | class/device node name |
| `gpsdrv` / `mtkgps` | 43699 / 43701 | MT3326 char device class / node (`gps.c:57`, `:974`) |
| `mt3326-gps`, `mediatek,mt3326-gps` | 43695 / 6149 | platform driver name / OF compatible |
| `pwrctl`, `pwrsave`, `rdelay` | 43726 / 43725 / 43724 | sysfs attributes of `/dev/mtkgps` |
| `[GPS] [E]... target packet length...` etc. | 43638-43659 | STP GPS driver log strings |

Downstream files/lines (primary behavioural reference):

* `connectivity/gps/stp_chrdev_gps.c`
  * `:35-36` device name / major 191; `:46-51` combo ioctl numbers 6-11
  * `:108-170` `GPS_write()` → `mtk_wcn_stp_send_data(..., GPS_TASK_INDX)`,
    cap `MTKSTP_BUFFER_SIZE`, `-ENOSPC` when 0 bytes accepted
  * `:174-252` `GPS_read()` → `mtk_wcn_stp_receive_data()`, wait queue
  * `:254-343` `GPS_unlocked_ioctl()` (see matrix)
  * `:354-387` `gps_cdev_rst_cb()`: `WMTMSG_TYPE_RESET` payload is
    `WMTRSTMSG_RESET_START/END/END_FAIL`
  * `:389-435` `GPS_open()`: reset-in-progress → `-EPERM`; `mtk_wcn_wmt_func_on(GPS)`
    **before** `mtk_wcn_stp_is_ready()`; `mtk_wcn_wmt_msgcb_reg(GPS, ...)`;
    `mtk_wcn_stp_register_event_cb(GPS_TASK_INDX, GPS_event_cb)`
  * `:438-461` `GPS_close()`: unregister event cb (flushes the channel queue),
    unregister msgcb, `mtk_wcn_wmt_func_off(GPS)`
  * `:475-479` `GPS_event_cb()`: wake-up only
  * `:491-560` module init/exit: static major 191 + class `stpgps`
* `connectivity/gps/gps.c`
  * `:155-167` `struct mt3326_gps_hardware` and `mt3326_gps_hw` with
    `.ext_power_on = NULL, .ext_power_off = NULL`
  * `:200-252` `mt3326_gps_power()`: only calls the callbacks when non-NULL;
    `static unsigned int power_on = 1` makes the first "on" a no-op
  * `:254-260` `mt3326_gps_reset()`: 3× `power()` + `mdelay(delay)`
  * `:264-283` `mt3326_gps_set_suspend()` + `sysfs_notify(kobj, NULL, "suspend")`
  * `:369-396` `mt3326_gps_dev_suspend/resume()`
  * `:706-726` sysfs attributes `pwrctl, suspend, status, state, pwrsave, rdelay`
  * `:760-764` `mt3326_gps_unlocked_ioctl()` → `-ENOIOCTLCMD`
  * `:798-848` `mt3326_gps_read()` and `:850-885` `mt3326_gps_write()`:
    **loopback** buffer (write data is returned by read; no STP)
  * `:919-928` `hw_init/hw_exit` → `mt3326_gps_power()`
  * `:931-1000` `mt3326_gps_probe()` (uses `&mt3326_gps_hw`; `rdelay = 50`)
  * `:1061-1092` `mt3326_gps_suspend/resume()`; `:1096-1120` OF ids/driver
* `common/conn_soc/include/wmt_exp.h:150-157` message types,
  `:186-192` `WMTRSTMSG_RESET_START=0x0, _END=0x1, _END_FAIL=0x2`
* `common/conn_soc/core/wmt_lib.c:1417-1438` reset payload is
  `sizeof(ENUM_WMTRSTMSG_TYPE_T)` (native 4-byte enum)
* `common/conn_soc/linux/pri/stp_exp.c:200-216`
  `mtk_wcn_stp_register_event_cb()` installs the callback **and flushes the
  channel receive queue**
* `arch/arm/boot/dts/mt6735.dtsi:1637-1639` `gps { compatible = "mediatek,mt3326-gps"; }`

### Why the MT3326 power/clock/GPIO/reset sequence is not ported

`mt3326_gps_hw.ext_power_on/ext_power_off` are initialised to `NULL`
(gps.c:164-167) and **no file in the downstream tree ever assigns them**
(verified by `grep -rn ext_power_ on the whole source tree: only the struct
definition, the two NULL-checked calls and the initialiser). `mt3326_gps_power()`
therefore does nothing but update the `power_on` flag; the ROM disassembly of
`mt3326_gps_probe` confirms the ROM image has the same NULL-checked indirect
calls. All register-level power, clock, GPIO and reset work for the GPS engine
on MT6737T is done by WMT/CONSYS (`wmt_func_gps_on/off`, `wmt_plat_gps_sync_ctrl`,
`wmt_plat_gps_lna_ctrl` in the ROM symbol list), which this driver drives via
`mtk_wcn_wmt_func_on/off(WMTDRV_GPS)`. Porting `pwrctl`/`rdelay`/reset or the
`/dev/mtkgps` loopback buffer would either be a fake control surface or would
synthesise data, so both are deferred (see matrix).

## 3. Parity matrix

| Behaviour | ROM / downstream | Nura before | After | Status |
|---|---|---|---|---|
| GPS data path | `/dev/stpgps` read/write on `GPS_TASK_INDX` (stp_chrdev_gps.c:108-252) | none | `/dev/gnss0` read/write; RX: STP event cb drains `mtk_wcn_stp_receive_data()` into `gnss_insert_raw()`; TX: `.write_raw` → `mtk_wcn_stp_send_data(..., GPS_TASK_INDX)` | **done** |
| WMT GPS function on/off | `mtk_wcn_wmt_func_on/off(WMTDRV_TYPE_GPS)` in open/close (:402, :452) | none | same, in `mtk_gps_channel_on/off()` | **done** |
| STP readiness gate | `mtk_wcn_stp_is_ready()` after func_on (:412) | none | same order, with full unwind on failure | **done** |
| STP reset notification | `gps_cdev_rst_cb()` sets `retflag`; open returns `-EPERM` while resetting (:354-400) | none | `WMTMSG_RESET` payload decoded, `resetting` gates `mtk_gps_channel_on()` → `-EPERM` | **done** |
| Event callback registration | `register_event_cb(GPS_TASK_INDX, cb)` on open, `NULL` on close (:420, :449) | none | same; registration also flushes the queue (stp_exp.c:200-216) | **done** |
| Blocking read / poll | wait queue signalled by `GPS_event_cb()` | none | GNSS core kfifo + wait queue + poll; cb feeds the kfifo | **done** |
| Short/failed TX | `-ENOSPC` when `send_data` accepts 0 bytes (:152-166) | none | `-ENOSPC` | **done** |
| TX size cap | `min(count, MTKSTP_BUFFER_SIZE)` | none | same | **done** |
| Writes with radio off | n/a (single open path) | n/a | `-ENETDOWN` while the STP channel is torn down (rfkill blocked) | **done** |
| rfkill | not present downstream | none | `RFKILL_TYPE_GPS` switch; block → channel off, unblock → channel on if open; open returns `-ERFKILL` | **done** (new, required) |
| PM suspend/resume | `mt3326_gps_suspend/resume` → set flag + `sysfs_notify("suspend")`; pwrctl side is a hardware no-op (gps.c:200-252, 369-396, 1061-1092) | none | `SYSTEM_SLEEP_PM_OPS` sets the same flag and notifies; sysfs `suspend` RW attr (0/1) on the GNSS device allows the daemon to set/detect it | **done** (no RF power change — proven no-op) |
| Module init/exit | platform driver register (ROM `mt3326_gps_mod_init`) | none | `module_platform_driver()`, OF match `mediatek,mt3326-gps`, driver name `mt3326-gps` | **done** |
| Teardown ordering | cdev del / class destroy / hw_exit | none | `device_remove_file` → `gnss_deregister_device` (closes channel if open) → callback barrier → clear singleton → rfkill unregister/destroy → `gnss_put_device` | **done** |
| ioctl 0/1 (STP enable/raw) | hard `-EINVAL` (:264-278) | n/a | GNSS ABI has no ioctl | **deferred** — downstream returns an error for both |
| ioctl 6 `COMBO_IOC_GPS_HWVER` | `mtk_wcn_wmt_hwver_get()` | n/a | — | **deferred** — GNSS ABI has no ioctl; WMT owns/exposes the hardware version through its own control channel |
| ioctl 7/8 IC HW/FW version | `mtk_wcn_wmt_ic_info_get(WMTCHIN_{HWVER,FWVER})` | n/a | — | **deferred** — `mtk_wcn_wmt_ic_info_get()` is not part of the frozen WMT header; also no GNSS ioctl |
| ioctl 9 `D1_EFUSE_GET` | `ioremap(0x10206198)` raw efuse read (:321-333) | n/a | — | **deferred** — SoC efuse access unrelated to GPS; belongs to an efuse/nvmem driver; no GNSS ioctl |
| ioctl 10 `RTC_FLAG` | `rtc_GPS_low_power_detected()` always returns 0 (:96-107) | n/a | — | **deferred** — constant return, nothing to port |
| ioctl 11 `CO_CLOCK_FLAG` | `mtk_wcn_wmt_co_clock_flag_get()` | n/a | — | **deferred** — not in the frozen WMT header; no GNSS ioctl |
| `/dev/mtkgps` echo device | `gps.c:798-885`: write is stored in a buffer and returned by read | none | — | **deferred** — synthesises data (would violate “all data from STP”) and has no hardware behind it |
| `pwrctl`, `rdelay`, `state`, `pwrsave`, `status` sysfs | `gps.c:706-726` software bookkeeping; power callbacks NULL | none | — | **deferred** — no hardware effect on CONSYS (see §2); would be a fake control knob. `status` is mnld restart logging only |
| NMEA/MTK frame parsing in kernel | none downstream (raw passthrough) | none | raw passthrough, `gdev->type = GNSS_TYPE_MTK` | **done** (no parsing by design) |

Deliberate divergences from downstream, all error-path fixes:

1. `GPS_open()` leaks the WMT function + msgcb when `mtk_wcn_stp_is_ready()`
   fails (:412-425). `mtk_gps_channel_on()` unwinds every step.
2. `GPS_close()` returns early while a chip reset is in progress (:443-447),
   leaving the WMT GPS reference on. Our `.close()` cannot return an error and
   the GNSS core has already dropped the file, so it always tears the channel
   down.
3. `current->pid == 1` bypass (:391-392) is not ported: it existed for an
   internal opener of the downstream chardev; a GNSS cdev cannot be opened that
   way.
4. `sema_init()` read/write semaphores (:431-433) are replaced by the GNSS
   core's own `read_mutex`/`write_mutex` and by our `lock`/`rx_lock`.

## 4. Compile test

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/wcn-gps
cp $M/nura.config .config
./scripts/config -e GNSS -m MTK_WCN_GPS -m MTK_WCN_WMT -m MTK_BTIF
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
     OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
     READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip \
     -j2 drivers/misc/mediatek/connectivity/gps/gps.o
```

Result (`.config`: `CONFIG_GNSS=y`, `CONFIG_RFKILL=m` (selected),
`CONFIG_MTK_BTIF=m`, `CONFIG_MTK_WCN_WMT=m`, `CONFIG_MTK_WCN_GPS=m`):

```
  CC [M]  drivers/misc/mediatek/connectivity/gps/gps.o
```

Clean, no warnings. Re-run with `W=1` after `rm gps.o`: also clean.
`./scripts/checkpatch.pl --no-tree --strict -f .../gps.c`:
`total: 0 errors, 0 warnings, 0 checks, 501 lines checked`.

The module link itself (`mtk_wcn_gps.ko`) needs the WMT/BTIF leaves and is
therefore left to the integration build; `llvm-nm -u gps.o` shows only frozen
WMT/STP symbols, GNSS core symbols, rfkill, sysfs/kstrtox and platform
helpers — no undefined symbol outside those APIs.

Patch sanity: `git apply --stat $M/patches/wcn-gps.patch` →
`gps/Makefile | 5 +-`, `gps/gps.c | 501 +`.

## 5. Inferred / unverified on hardware

* `[INFERENCE]` **Callback context.** `mtk_wcn_stp_event_cb` takes no argument
  and the frozen header does not state its context. The drain therefore runs
  entirely under `spin_lock_irqsave()` and uses only non-blocking calls
  (`mtk_wcn_stp_receive_data`, `gnss_insert_raw`). This is correct whether STP
  invokes it from the transport IRQ/softirq or from a thread.
* `[INFERENCE]` **Single GPS channel.** The frozen callback type cannot carry a
  device pointer, so the driver keeps a single `mtk_gps_singleton`; a second
  `mediatek,mt3326-gps` node is rejected with `-EEXIST`. This matches the
  hardware (one GPS function per CONSYS).
* `[INFERENCE]` **Reset payload size.** Downstream accepts `sz <= sizeof(enum)`
  and `memcpy`s it (reading past short buffers); we require
  `len >= sizeof(u32)` and copy exactly 4 bytes, following
  `wmt_lib.c:1423-1435`, which always sends `sizeof(ENUM_WMTRSTMSG_TYPE_T)`.
* `[INFERENCE]` **Reset vs. unregister ordering.** The WMT callback may race
  with probe/remove. The callback only performs `WRITE_ONCE()` on a flag and
  is a no-op once the singleton is cleared.
* **Unverified:** that the WMT/STP implementation guarantees no callback after
  `mtk_wcn_stp_register_event_cb(idx, NULL)` returns. `mtk_gps_remove()`
  clears `rx_enabled` under `rx_lock` *after* deregistration as a barrier
  (same residual window as downstream).
* **Unverified:** the real-world read-FIFO overrun behaviour. The GNSS core
  FIFO is 4 KiB while the STP per-channel queue is 16 KiB; a stalled reader
  loses bytes and the loss is reported with `dev_warn_ratelimited()`. On real
  data this must be checked with the positioning daemon.
* **Unverified:** whether `mtk_wcn_wmt_func_on(WMTDRV_GPS)` brings STP up
  before `mtk_wcn_stp_is_ready()` returns true with the Nura WMT implementation
  (the order is copied from downstream, see stp_chrdev_gps.c:398-412).
* **Unverified:** the rfkill-blocked/unblocked transition while a client holds
  `/dev/gnss0` open. Blocking tears the channel down and unblocking re-creates
  it; whether the MTK binary protocol survives that without a daemon-side
  restart is a hardware/userspace question.
* **Unverified:** suspend/resume notification semantics expected by the future
  positioning daemon (`/sys/class/gnss/gnss0/suspend`, values 0/1, `poll()`
  wakes through `sysfs_notify()`).

## 6. DTS-FRAGMENT

No `.dts`/`.dtsi` file was modified. The CONSYS node is used unchanged; only a
child-less platform node for the GPS function is required (identical to
downstream `arch/arm/boot/dts/mt6735.dtsi:1637-1639`):

```dts
&soc {
	gps: gps {
		compatible = "mediatek,mt3326-gps";
		status = "okay";
	};
};
```

No `reg`, `interrupts`, `clocks` or supplies: the GPS engine is powered,
clocked and reset by WMT/CONSYS; the STP transport is owned by the WMT/BTIF
drivers. It should be applied to `arch/arm64/boot/dts/mediatek/mt6735.dtsi`
(or the board DTS) by the integration owner.

## 7. Runtime requirements (no blobs added)

This driver loads no firmware and adds no blob. It depends on:

* `mtk_wcn_wmt` (module) having downloaded the MT6735 CONSYS ROM patch before
  `mtk_wcn_wmt_func_on(WMTDRV_GPS)` can succeed. The patch name and options
  come from the WMT configuration file
  **`/system/etc/firmware/WMT_SOC.cfg`** (downstream
  `common/conn_soc/core/include/wmt_conf.h:33-34` and `wmt_conf.c:478-479`;
  WMT-owned, not this leaf).
* The userspace positioning daemon opening **`/dev/gnss0`** (GNSS class
  `gnss`, type reported as `MTK`) and supplying whatever MTK GPS/NV data it
  needs over that raw stream; the kernel passes frames through untouched.
* `CONFIG_GNSS`, `CONFIG_RFKILL` (selected) and the built `mtk_wcn_gps.ko`.

## 8. Patch

`$M/patches/wcn-gps.patch` (`git diff --binary`, two files, +505/-1).
