# rem-camera-dphy — MIPI D-PHY / CSI-2 lane and settle programming for the MT6735 SENINF receiver

Worktree: `$M/worktrees/rem-camera-dphy` (detached at `src/nura-linux` HEAD `046883c0c`,
`patches/parity-baseline-all.patch` applied; the two `drivers/misc` baseline fixups of the
22:00 regeneration are present in the worktree and therefore absent from the leaf patch).
`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

Deliverables:

| Item | Path |
|---|---|
| Patch (applies on top of the baseline, verified) | `$M/patches/rem-camera-dphy.patch` |
| This report | `$M/logs/rem-camera-dphy.md` |
| Kernel build log (2 passes, 0 warnings) | `$M/logs/rem-camera-dphy-build.log` |
| Full extractor output (evidence, 396 lines) | `$M/logs/rem-camera-dphy-extract.log` |

## 1. Status summary

| # | Item | Status |
|---|---|---|
| 1 | SENINF/MIPI-RX register program recovered from `libcam.halsensor.so` (windows, offsets, values, argument meaning, ordering, per-instance differences) | **closed** |
| 2 | CSI-2 receiver + MIPI-RX lane/settle programming implemented at stream-on in `mtk-seninf.c` | **closed** |
| 3 | Stream-off path (vendor disable branch, per instance) | **closed** |
| 4 | Settle-time formula (`settle_ns * 300 / 1000`, default 85 ns) | **closed** |
| 5 | SENINF `init()` register defaults (MUX_CTRL bit 31 ×4, CTRL bit 0 ×3) | **closed** |
| 6 | HS deskew calibration (`autoDeskewCalibrationSeninf1`) | **partial / deliberately not wired**: the mechanism and per-lane delay fields are recovered and documented, but its field semantics (bits 24:21 / 28:25, the 0x3b4/0x3b8/0x3bc measurement handshake) exist in no other source, and it is a separate API, not part of the vendor's CSI-2 stream-on sequence. No value is invented; nothing was wired. |
| 7 | SENINF mux/source selection (`SENINFn_MUX_CTRL`, `SENINFn_CTRL` sources, `SENINF_TOP_MUX_CTRL`) | **blocked by missing input**: field layouts recovered, but `SENINF_SOURCE_ENUM` / `PAD2CAM_DATA_ENUM` / `TG_FORMAT_ENUM` values are HAL policy from `cfg_setting_imgsensor`, absent from every binary and from the GPL drop (see §6). Only the two proven unconditional `init()` bits are applied. |
| 8 | MIPI-RX per-lane map (the vendor's `this[0x58]`/`this[0x5a]` nibbles, from an ioctl) | **blocked by missing input** (see §6); the vendor itself skips the block when the value is 0, which is what the driver does. |

## 2. Files changed

```
drivers/media/platform/mediatek/seninf/mtk-seninf.c        (+332/-18)
drivers/media/platform/mediatek/seninf/mtk-seninf-regs.h   (+238)
tools/mediatek/camera/seninf-dphy/Makefile                 (new, 59)
tools/mediatek/camera/seninf-dphy/seninf-dphy-extract.c    (new, 1569)
tools/mediatek/camera/seninf-dphy/.gitignore               (new, 1)
```

Nothing outside these five files. No DTS/DTSI, no `imgsys`, no `isp`, no sensor driver, no
`include/uapi/linux/mtk_isp.h`, no camera tools `Makefile`. The media device, async notifier,
MCLK provider, error decode and runtime-PM structure of the baseline driver are unchanged.

## 3. Evidence

### 3.1 Source of the programming

The MT6735 SENINF CSI-2 receiver and MIPI D-PHY have **no** kernel-side programming in this
product: the ROM kernel exports no SENINF symbol (`zip/symbols.txt`, zero matches) and the
downstream GPL drop only declares the windows (`camera_isp_D1.c:124-133`) and the error status
bits (`camera_isp_D1.h:267-298`). All of it is done by the userspace `seninf_drv`, shipped as:
`$M/zip/vendor-libs/libcam.halsensor.so`, `class SeninfDrvImp`, Thumb-2
(`readelf -A` → `Tag_THUMB_ISA_use: Thumb-2`; `Tag_CPU_arch: v7`).

Instruction addresses below are file offsets in that ELF (the `.text` virtual address equals
its file offset) and were all produced by the extractor, which refuses to print a value whose
proof address does not hold the expected instruction
(`$M/logs/rem-camera-dphy-extract.log`).

### 3.2 Mapped windows (mmap() in `SeninfDrvImp::init` @0x18078)

| Window | Base | Length | mmap call | Length proof | Base proof | Stored to |
|---|---|---|---|---|---|---|
| SENINF | `0x15008000` | `0x4000` | @0x181c0 (`blx mmap@plt`) | `mov.w r1,#0x4000` @0x181b0 | literal pool `0x18404` = 0x15008000 | `this[0x18]` @0x181c4 |
| MIPI_ANA | `0x10215000` | `0x3000` | @0x1824a | `mov.w r1,#0x3000` @0x1823e | literal pool `0x18408` = 0x10215000 | `this[0x28]` @0x18250 |
| ISP | `0x15000000` | `0xa000` | @0x1817c | `mov.w r1,#0xa000` @0x18176 | `mov.w r2,#0x15000000` @0x1816a | `this[0x14]` @0x18180 |
| PLL | `0x10000000` | `0x100` | @0x18206 | `mov.w r1,#0x100` @0x18200 | `mov.w r2,#0x10000000` @0x181f4 | `this[0x3c]` @0x1820a |
| GPIO | `0x10211000` | `0x1000` | @0x18294 | `mov.w r1,#0x1000` @0x18288 | literal pool `0x1840c` = 0x10211000 | `this[0x38]` @0x1829a |

Derived per-instance pointers (`MIPI-RX digital block = MIPI_ANA + 0x800 + 0x400*n`):

| Member | Derivation | Proof |
|---|---|---|
| `this[0x24]` | `[this+0x28] + 0x800` → `0x10215800` (SENINF1) | `add.w r1,r0,#0x800` @0x1827e |
| `this[0x2c]` | `= this[0x24]` | `str r6,[r4,#0x2c]` @0x18348 |
| `this[0x30]` | `this[0x24] + 0x400` → `0x10215c00` (SENINF2) | `add.w r0,r6,#0x400` @0x1834a |
| `this[0x34]` | `this[0x24] + 0x800` → `0x10216000` (SENINF3) | `add.w r5,r6,#0x800` @0x18350 |

The same ranges are what the ROM device tree lists for the ispsys node
(`zip/rom_dtb_0.dts:3703`: `0x10215000 0x3000`, `0x10211000 0x1000`) and what
`camera_isp_D1.c:131-133` names `MIPIRX_ANALOG_ADDR` / `GPIO_BASE_ADDR`.

**Window size note:** the vendor maps `0x4000` for SENINF (`camera_isp_D1.c:126`) but never
touches anything at or above offset `0x1000` (highest offset used is `0xd20`, SENINF4
MUX_CTRL). `reg = <0x15008000 0x1000>` is therefore sufficient; the report's DTS fragment uses
that so the CAMSV bank at `0x15009000` stays free (cross-checked with RemImgsys).

### 3.3 Argument meaning of `setSeninf{1,2,3}NCSI2`

The vendor function takes ten `unsigned long` arguments. Its own trace formats name them
(rodata strings resolved by the extractor):

* @0x2863c `"[%s]TermDelay:%d SettleDelay:%d ClkTermDelay:%d Vsync:%d lane_num:%d
  NCSI2_en:%d HeaderOrder:%d SettleDelay(cnt):%d ISP_clk:%d dpcm:%d\n"`
* @0x285eb `"[%s]CSI-0 calibration start,MIPIType=%d,HSRXDE=%d\n"`
* @0x2861c `"[%s]CSI-0 calibration end !\n"`
* @0x286c1 `"[%s]Err(%5d):CSI-2 calibration failed!, NCSI2Config Reg 0x44=0x%x, 0x48=0x%x\n"`

Mapping to registers (extractor output at 0x16502/0x16436):

| Arg | Name | Register use (SENINF1) |
|---|---|---|
| 1 | TermDelay | logged only (@0x164d6) |
| 2 | SettleDelay (ns) | → settle count (0x164c8…0x164da) → 0x3a8 / 0x364 |
| 3 | ClkTermDelay | logged only (@0x164d6) |
| 4 | Vsync | logged only (@0x16502 stack slot) |
| 5 | lane_num (0-based) | → lane mask `(1 << (lane_num+1)) - 1` → 0x3a0 / 0x360 |
| 6 | NCSI2_en | select enable vs disable path (@0x16282) |
| 7 | HeaderOrder (ECC order) | → 0x3a0 bits 31:16 (@0x165ca…0x165d8) |
| 8 | MIPIType | `== 1` → MIPI_OPHY_CSI2 register group, else NCSI2 group (@0x16532) |
| 9 | HSRXDE | logged only (@0x16436) |
| 10 | dpcm | index into an 8×u16 table for 42…49, else 0 → 0x3ac (@0x16506…0x1651e) |

Values reported by both sensors of this board (downstream GPL sensor drivers, the same struct
the HAL is fed through `KDIMGSENSORIOC_X_GET_SENSOR_INFO2`):

| Field | IMX219 | S5K5E3YX | Header |
|---|---|---|---|
| `MIPIsensorType` = `MIPI_OPHY_NCSI2` (0) | `imx219mipiraw_Sensor.c:156` | `s5k5e3yxmipiraw_Sensor.c:146` | enum `kd_imgsensor_define.h:286-289` |
| `SensorMIPILaneNumber` (`SENSOR_MIPI_4_LANE`=3 / `_2_LANE`=1) | `:160` | `:150` | enum `kd_imgsensor_define.h:270-275` |
| `MIPIDataLowPwr2HighSpeedSettleDelayCount` = 85 ns | `:78` | `:69` | — |
| `SensorPacketECCOrder` = 1 | `:1476` | `:1433` | — |
| `MIPIDataLowPwr2HighSpeedTermDelayCount` = 0, `MIPICLK…TermDelayCount` = 0 | `:1472-1473` | `:1429-1430` | — |
| `DPCM_INFO` | never assigned → 0 | never assigned → 0 | `kd_sensorlist.c:1879-1883` |

### 3.4 Recovered formulas

| Formula | Evidence |
|---|---|
| settle default `0x55` (85 ns) when the caller passes 0 | `cmp r7,#0` @0x164c8, `moveq r7,#0x55` @0x164cc |
| `settle_count = settle_ns * 300 / 1000` (SENINF clock 300 MHz) | `mov.w r5,#0x12c` @0x1647e, `mov.w r1,#0x3e8` @0x164ce, `mul r2,r5,r7` @0x164d2, `udiv r5,r2,r1` @0x164da |
| `lane_mask = (1 << (lane_num + 1)) - 1` | `movs r3,#0x1` @0x165bc, `lsls r3,r7` @0x165ce, `subs r7,r3,#1` @0x165d4, `orrs r7,r5` @0x165d6 |
| `0x3a8 = (u16)(settle_count << 8)` | `lsls r5,r5,#8` @0x165ac, `uxth r3,r5` @0x165b2, store @0x165b8 |
| `0x3a0 \|= BIT(26) \| BIT(4) \| (ecc_order << 16) \| lane_mask` | `orr r6,r0,#0x4000000` @0x165c4, `orr r1,r6,#0x10` @0x165ca, `orr.w r5,r1,r2,lsl#16` @0x165d0 |

### 3.5 Register write table (all pad0 addresses; pad1 = same instruction +0x400-exact
addresses in `rem-camera-dphy-extract.log`, pad2 in the same log)

Stream-on, in vendor order (`setSeninf1NCSI2`, `NCSI2_en == 1`, `MIPIType = MIPI_OPHY_NCSI2`):

| # | Target (absolute) | Value | Proof |
|---|---|---|---|
| 1 | MIPI `0x1021584c` | `& 0xfefbefbe` (clears bits 24,18,12,7,0) | @0x16294 (pad1 0x1671c, pad2 0x16b56) |
| 2 | MIPI `0x10215850` | `& 0xfefbefbe` | @0x1629e (0x16726 / 0x16b62) |
| 3 | MIPI `0x10215800/04/08/0c/10` | `\|= 0x8` (lane pre-charge, 5 lanes: clk+4 data) | @0x162aa, 0x162b6, 0x162c2, 0x162ce, 0x162da |
| 4 | MIPI `0x10215824` | `\|= 0x1` then `usleep(30)` | @0x162e8, usleep @0x162ea |
| 5 | MIPI `0x10215820` | `\|= 0x3` then `usleep(1)` | @0x162f8, usleep @0x162fc |
| 6 | MIPI `0x10215800/04/08/0c/10` | `\|= 0x1` (lane enable) | @0x1630a…0x1633a |
| — | MIPI lane map | *skipped* (HAL ioctl value; vendor skips when 0) | `cmp r5,#0` @0x16340 |
| 7 | SENINF `0x150083d8` | `\|= 0x1f` (bits 4,0,1,2,3 set one by one) | @0x163ca, 0x163d6, 0x163e2, 0x163ee, 0x163fe |
| 8 | SENINF `0x15008338` | `\|= 0x1` (start) | @0x1640a (pad2 @0x16ca0) |
| 9 | SENINF `0x1500833c` | `= 0x1541` | @0x16412 (pad2 @0x16caa); **not written on pad1** |
| 10 | SENINF `0x15008338` | `\|= 0x4` (mode) | @0x16422 (pad1 @0x16894 is its only write, pad2 @0x16cb8) |
| 11 | — | `usleep(500)` | @0x1643e |
| 12 | SENINF `0x15008344` / `0x15008348` | read, test `& 0x10001` / `& 0x101` (completion, non-fatal) | @0x16442-0x1645a, error log @0x166b8/0x16f14 |
| 13 | SENINF `0x15008338` | `&= ~0x1` | @0x1646a (pad2 @0x16d00) |
| 14 | SENINF `0x150083d8` | `&= ~0x1f` (bits cleared one by one) | @0x1647a, 0x1648e, 0x1649c, 0x164ac, 0x164bc |
| 15 | SENINF `0x150083ac` | `= 0` (dpcm outside 42…49) | @0x1651e |
| 16 | SENINF `0x15008100` (CTRL) | `(\| & ~0xf000) \| 0x8000` | @0x165a4 (pad1 0x169ae, pad2 0x16dde) |
| 17 | SENINF `0x150083a0` | `&= ~0x80` | @0x165b4 (0x169be / 0x16dee) |
| 18 | SENINF `0x150083a8` | `(settle_count << 8) & 0xffff` | @0x165b8 (0x169c2 / 0x16df2) |
| 19 | SENINF `0x150083a0` | `(\| \| 0x4000000 \| 0x10 \| (ecc<<16)) \| lane_mask` | @0x165d8 (0x169e4 / 0x16e14) |
| 20 | SENINF `0x150083b0` | `= 0x80007fff` | @0x165dc (0x169e8 / 0x16e18) |
| 21 | SENINF `0x15008120` | `(\| 0x3)` then `(& ~0x3)` — back-to-back pulse | @0x165ec then @0x165f0 (0x169f8/fc, 0x16e28/2c) |

Stream-off (`NCSI2_en != 1`, `@0x165f6…0x166b4`):

| # | Target | Value | Proof |
|---|---|---|---|
| 1 | SENINF `0x150083a0` | `&= ~0x1f` | @0x165fe |
| 2 | SENINF `0x15008360` | `&= ~0x1f` | @0x1660a |
| 3 | MIPI `0x10215824` | `&= ~0x1` | @0x16618 |
| 4 | MIPI `0x10215820` | `&= ~0x1` | @0x16624 |
| 5 | MIPI lanes `+0x0…+0x10` | `&= ~0x1` | @0x16630, 0x1663c, 0x16648, 0x16654, 0x16662 |
| 6 | MIPI `0x1021584c` / `0x10215850` | `\|= 0x01041041` (bits 24,22,12,6,0) | @0x1666e / 0x16678 |
| 7 | MIPI lanes `+0x0…+0x10` | `&= ~0x8` | @0x16684, 0x16690, 0x1669c, 0x166a8, 0x166b4 |

Static defaults (`init()` @0x18078):

| Target | Value | Proof |
|---|---|---|
| SENINF `0x15008120`, `0x15008520`, `0x15008920`, `0x15008d20` (MUX_CTRL) | `\|= BIT(31)` | @0x182f2, 0x182fe, 0x1830a, 0x18316 |
| SENINF `0x15008100`, `0x15008500`, `0x15008900` (CTRL) | `\|= BIT(0)` | @0x18322, 0x1832e, 0x1833e |

### 3.6 HS deskew (recovered, not wired)

`autoDeskewCalibrationSeninf1` @0x16f34 (siblings `...Seninf2` @0x17424, `...Seninf3` @0x17914;
`nm -D` gives 0x16f35/0x17425/0x17915 and size 1264, i.e. contiguous):

- clears bit 24 of the four data-lane registers (@0x16f54…0x16f8a),
- `0x3b0 = 0x80007fff` (@0x16f92), `0x3b8 = 0x12` (@0x16f8e), polls `0x3b4 & 0xfb8` (@0x16f96),
  `0x3b4 = 0x7fff` (@0x16fd0),
- for each data lane (regs `+0x4/+0x8/+0xc/+0x10`) walks the delay field `[28:25]` (with bit 24
  as enable) from 0 to 15, waiting for `0x3bc` to change and testing `0x3b4 & 0xfb8` for a
  successful eye (@0x16fe0…0x1716a), keeping the delay of the first good position,
- for the clock lane (reg `+0x0`) uses bit 20 as enable and `[24:21]` as the delay field
  (@0x1728c…0x172ea),
- finally re-programs all five lanes with `(lane_result - min_result) & 0xf` (data) and
  `((clk_result - min_result) >> 1) & 0xf` (clock), guards the shift with `cmp r11,r6`
  (@0x17244…0x173c8).

It is not part of `setSeninf{1,2,3}NCSI2` and its field semantics appear in no other source;
the driver does not run it (see §5/§6).

## 4. Implementation

`drivers/media/platform/mediatek/seninf/mtk-seninf.c`:

* `mtk_seninf_get_link()` — replaces the old `check_link()`: returns lane count and link
  frequency of the pad's remote endpoint (same validation and log messages as before).
* `mtk_seninf_hw_init()` — the two proven `init()` defaults; called from
  `mtk_seninf_runtime_resume()` so they are applied on probe and after every power-domain
  resume.
* `mtk_seninf_mipi_rx_enable()/disable()` — the MIPI-RX digital sequence of §3.5 rows 1-7 and
  the disable mirror, on `mipi_ana + 0x800 + 0x400*pad`.
* `mtk_seninf_csi2_calibrate()` — rows 7-14 including the per-instance difference (SENINF2
  has no start bit, no sequence register, no stop-bit clear; it does run the same status test).
* `mtk_seninf_csi2_enable()` — rows 15-21, with `settle = 85 ns * 300 / 1000 = 25` and
  `lane_mask = (1u << lanes) - 1` from the endpoint.
* `mtk_seninf_csi2_disable()` — the stream-off table above.
* `s_stream()` now programs every sink pad that has a sensor bound (`-ENODEV` pads skipped),
  tracking them in `streaming_pads` so stream-off and the error unwind disable exactly what was
  enabled; `remove()` does the same before dropping the runtime-PM reference.
  `s_stream()` has no pad argument and the per-pad `enable_streams()` API is hard-disabled in
  this kernel (`drivers/media/v4l2-core/v4l2-subdev.c:59`, `v4l2_subdev_enable_streams_api`),
  so "the active pad" is the set of bound inputs; on this board that is one sensor per SENINF
  instance, i.e. exactly the streaming input.

Ordering is the vendor's exactly: MIPI-RX lane power/enable → CSI-2 calibration handshake →
DPCM value → CTRL CSI-2 mode → `0x3a0`/`0x3a8`/`0x3b0` lane/settle config → MUX_CTRL pulse.
`setSeninf{1,2,3}NCSI2` touches only the SENINF window (`this[0x18]`) and the MIPI-RX block
(`MIPI_ANA + 0x800 + 0x400*n`); no ISP-pipeline register (the `0x15004000` TG/CAM/CAMSV block
that `setTg1Cfg`/`setTg1GrabRange`/`setSeninf*VCE` program through `this[0x14]`) is written by
this driver.

## 5. Parity matrix

| Vendor behaviour | Status | Reason |
|---|---|---|
| MIPI-RX per-lane power/enable, `+0x20/+0x24` handshake, `+0x4c/+0x50` masks | done | §3.5 rows 1-6, disable table |
| `SENINFn_CTRL` CSI-2 mode bit (15) | done | @0x165a4 |
| `0x3a0` bit 7 clear, bit 26, bit 4, ECC order, lane mask | done | @0x165b4…0x165d8 |
| Settle count register `0x3a8` and the `*300/1000` formula | done | @0x165b8, @0x164d2/0x164da |
| `0x3b0 = 0x80007fff` | done | @0x165dc |
| `0x3ac` DPCM value | done (0) | @0x1651e; both sensors report DPCM_INFO 0 |
| `SENINFn_MUX_CTRL` start/clear pulse | done | @0x165ec/0x165f0 |
| CSI-2 calibration handshake + status check | done | §3.5 rows 7-14 |
| Stream-off (lanes, shared masks, CSI-2 bits) | done | disable table |
| `init()` MUX_CTRL bit 31 / CTRL bit 0 | done | @0x182f2…0x1833e |
| `SENINFn_MUX_CTRL` source fields, `SENINFn_CTRL` source/pad2cam, `SENINF_TOP_MUX_CTRL` | blocked | enum values are HAL policy, absent everywhere (§6.1) |
| MIPI-RX lane map (`this[0x58]`/`this[0x5a]`) | blocked | ioctl-provided HAL data (§6.2); vendor skips when 0 |
| MIPI_OPHY_CSI2 register group (`0x360/0x364/0x368`) | not programmed | recovered (@0x16534…0x1655a) but no DT/kernel input selects the PHY type; both sensors are NCSI2 (§3.3) |
| HS deskew calibration | not wired | field semantics unprovable, separate API (§3.6, §6.3) |
| Per-mode settle time | N/A | the only value either sensor reports is 85 ns (§3.3) |
| MCLK / GPIO / PLL windows of the vendor `init()` | out of slice | handled by the clock provider in the baseline driver |
| `setTg1Cfg` / `setTg1GrabRange` / `setSeninf*VCE` | out of slice | ISP (TG) and VCE registers, owned by the imgsys/ISP slice |

## 6. Irrecoverable items

1. **`SENINF_SOURCE_ENUM` / `PAD2CAM_DATA_ENUM` / `TG_FORMAT_ENUM` values.**
   `setSeninf1Ctrl` (@0x1508a) and `setSeninf1MuxCtrl` (@0x14d5e) program them into
   `SENINFn_CTRL[15:12]/[30:28]` and `SENINFn_MUX_CTRL[15:12]`, but the numbers come from the
   vendor HAL's per-sensor `cfg_setting_imgsensor` table plus `sensorStaticInfo` fields; the
   call sites in `HalSensor::seninfControl` (@0xac08) load them from HAL-internal structures
   (`[r5+0xac]`, `[r5+0xb0]`, the scenario index) whose initialisers are not in any provided
   binary (libcameracustom/libcamdrv contain no such table; no GPL source either). Only the
   field layouts are recovered. The kernel therefore applies only the two values the vendor
   writes unconditionally (`init()`), and would need the per-sensor source/pad numbers to
   complete the mux programming.
2. **MIPI-RX lane map.** `setSeninf1NCSI2` reads `this[0x58]` (SENINF1 lanes 1-4 in nibbles
   0-3, SENINF2 lanes 1-4 in nibbles 4-7 plus `this[0x5a]` bits 15:12) from an ioctl on the
   vendor ISP driver (`ioctl(fd, 0x80046401)` in `init()` @0x18392). The value is
   board/sensor-specific hardware data served by a kernel driver this tree does not have. The
   vendor skips the four writes when it is 0 (`cmp r5,#0` @0x16340), which is what the driver
   does; the register default lane order is used.
3. **HS deskew field semantics.** The procedure, the enable bits (24 data / 20 clock), the
   delay fields (28:25 / 24:21) and the averaging formula are recovered bit-exactly (§3.6), but
   what the delay actually delays is documented nowhere, and the measurement handshake
   (`0x3b4` status mask `0xfb8`, `0x3b8 = 0x12`, `0x3bc` free-running counter) is only
   observable as a working sequence. Wiring it would mean running an unbounded hardware scan
   loop over registers whose meaning is unproven, so it is documented and left out.
4. **Register field names.** Outside the fields enumerated above (source, pad2cam, lane mask,
   settle, ECC order, DPCM, and the `MIPI_OPHY_CSI2` group) the CSI-2 block has no names in
   any source: `0x338/0x33c/0x344/0x348/0x3d8` and `0x3a0[26,7,4]`, `0x3a8`, `0x3b0`,
   `0x4c/0x50` bits are reproduced bit-exactly with the vendor's own log names ("CSI-0
   calibration", "NCSI2Config Reg 0x44/0x48"). No datasheet names are invented.
5. **`0x2a6c0` DPCM table.** The 8×u16 table indexed by `dpcm - 42` (@0x16510) is not needed
   for this board (`DPCM_INFO` is 0 in both sensor drivers) and its dump at that address is not
   a plausible rate table, so it was not used or quoted.

## 7. Build evidence

Full output: `$M/logs/rem-camera-dphy-build.log`.

```sh
export M=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
export TMPDIR=$M/tmp          # /tmp is a full 12G tmpfs on this box
cd "$M/worktrees/rem-camera-dphy"
cp "$M/nura.config" .config && ./scripts/config -d MEDIA_SUPPORT_FILTER -e MEDIA_SUPPORT ... \
  -e VIDEO_MTK_SENINF ...            # exact list in the log
make ARCH=arm64 LLVM=1 olddefconfig  # CONFIG_MEDIA_SUPPORT=y, CONFIG_VIDEO_DEV=y,
                                     # CONFIG_VIDEO_MTK_SENINF=y
make ARCH=arm64 LLVM=1    -j2 drivers/media/platform/mediatek/seninf/   # exit=0
make ARCH=arm64 LLVM=1 W=1 -j2 drivers/media/platform/mediatek/seninf/  # exit=0
```

`mtk-seninf.o` compiled in both passes, **0 warnings, 0 errors** (`-Werror` was not needed).
`built-in.a` was archived as well.

Userspace helper (host gcc, `-Wall -Wextra -std=gnu11`):

```sh
cd tools/mediatek/camera/seninf-dphy
make                                  # exit 0, no warning
make check OBJDUMP="$M/llvmpkg/usr/bin/llvm-objdump --triple=thumbv7-none-eabi"
                                      # exit 0: "all proof addresses verified"
```

`make check` regenerates the disassembly into `build/` (git-ignored), runs the extractor and
fails if any documented proof address no longer holds its instruction; the generated log has
0 `PROOF MISMATCH` lines. The tool never runs at kernel runtime and does not touch hardware.

## 8. DTS fragment (text only; no DTS file was modified)

Only the SENINF node changes, and only its first `reg` entry (0x4000 → 0x1000, see §3.2); the
window list otherwise already matches what the driver maps and what the vendor uses. The
`ports`/clock/pinctrl content is unchanged from the baseline fragment.

```dts
seninf: seninf@15008000 {
	compatible = "mediatek,mt6735-seninf";
	reg = <0 0x15008000 0 0x1000>,	/* SENINF: 4 instances, max offset 0xd20 */
	      <0 0x1500c000 0 0x1000>,	/* MIPIRX config window */
	      <0 0x10215000 0 0x3000>;	/* MIPI RX analog: +0x800 per instance */
	clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
	clock-names = "camtg";
	assigned-clocks = <&topckgen CLK_TOP_CAMTG_SEL>;
	assigned-clock-parents = <&topckgen CLK_TOP_UNIVPLL_D26>;
	power-domains = <&spm MT6735_POWER_DOMAIN_ISP>;
	pinctrl-names = "default";
	pinctrl-0 = <&camera_pins_default>;
	#clock-cells = <1>;
	clock-output-names = "mclk0", "mclk1", "mclk2";
	status = "okay";
	ports {
		#address-cells = <1>;
		#size-cells = <0>;
		port@0 { reg = <0>; seninf_in0: endpoint { remote-endpoint = <&imx219_out>; }; };
		port@1 { reg = <1>; seninf_in1: endpoint { remote-endpoint = <&s5k5e3yx_out>; }; };
	};
};
```

No new property is introduced: the driver takes the data-lane count and link frequency from the
connected sensor endpoint (`data-lanes`, `link-frequencies`), which the baseline sensor nodes
already provide (IMX219 `<1 2 3 4>`, S5K5E3YX `<1 2>`), and the settle time is the 85 ns both
sensors report (§3.3).
