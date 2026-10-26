# wcn-bt: MTK BTIF + Bluetooth HCI over STP

Worktree: `$M/worktrees/wcn-bt`
Patch: `$M/patches/wcn-bt.patch`
Scope: (A) `drivers/misc/mediatek/btif/*.c` implementing the frozen
`include/mtk_btif.h`; (B) `drivers/misc/mediatek/connectivity/bt/` presenting
the STP Bluetooth channel as a kernel HCI device.

---

## 1. Assigned driver and files changed

### BTIF (owned leaf A)

| File | Status | Content |
|---|---|---|
| `drivers/misc/mediatek/btif/btif_regs.h` | new (119 l) | BTIF + APDMA register offsets and bit fields, each cited to downstream |
| `drivers/misc/mediatek/btif/btif_core.h` | new (195 l) | internal objects (controller, VFF channel, owner, staging ring, logs) |
| `drivers/misc/mediatek/btif/btif_hw.c` | new (330 l) | BTIF PIO ops: init sequence, IER/mode/loopback/wake, TX kfifo + TX-empty IRQ, RX FIFO drain, register dump |
| `drivers/misc/mediatek/btif/btif_dma.c` | new (393 l) | APDMA virtual-FIFO ops: warm reset, enable/stop/flush, TX ring publish, TX/RX IRQ handlers, dump |
| `drivers/misc/mediatek/btif/btif_core.c` | new (1427 l) | platform driver, owner/channel model, ON/DPIDLE/SUSPEND state machine, RX staging ring + `btif_rxd` thread, debug packet log, all exported API |
| `drivers/misc/mediatek/btif/Makefile` | edited (object lines only) | `obj-$(CONFIG_MTK_BTIF) += mtk_btif.o`, `mtk_btif-objs := btif_core.o btif_hw.o btif_dma.o` |

The frozen `btif/Kconfig` and `btif/include/mtk_btif.h` were **not** touched.

### Bluetooth (owned leaf B)

| File | Status | Content |
|---|---|---|
| `drivers/misc/mediatek/connectivity/bt/mtk_wcn_bt.c` | new (519 l) | HCI device, open/close = WMT func on/off + STP BT channel registration, H4 TX/RX, reset message handling, module init/exit |
| `drivers/misc/mediatek/connectivity/bt/Makefile` | edited | `obj-$(CONFIG_MTK_WCN_BT) += mtk_wcn_bt.o` |

The frozen `bt/Kconfig` and all frozen headers were **not** touched. No other
driver, DTS or shared file was modified.

### Exported GPL symbols (frozen API, complete)

`mtk_wcn_btif_open`, `_close`, `_write`, `_read`, `_dpidle_ctrl`,
`_rx_cb_register`, `_wakeup_consys`, `_loopback_ctrl`, `_dbg_ctrl`,
`_parser_wmt_evt` — all `EXPORT_SYMBOL_GPL`.

---

## 2. ROM evidence

`$M/zip/vmlinux.elf` is a 32-bit ARM ELF with `.symtab`; addresses below are
from `llvm-nm`/`llvm-objdump` on that file.

| Symbol / string | Address | What it confirms |
|---|---|---|
| `hal_btif_hw_init` | `0xc06055c0` | Exact BTIF init sequence (see below) |
| `hal_btif_raise_wak_sig` | `0xc0605ac8` | `WAK(0x64)` bit0 clear → `usleep_range(64,96)` → bit0 set |
| `hal_btif_dma_hw_init` | `0xc0603b24` | APDMA warm reset, `0x1c/0x24/0x2c/0x30/0x28` programming, INT_FLAG/INT_EN polarity per direction |
| `hal_btif_irq_handler` | `0xc060570c` | BTIF IIR-driven handler |
| `hal_dma_send_data` | `0xc0604290` | TX vFIFO copy + WPT publish path |
| `hal_rx_dma_irq_handler` | `0xc06044b4` | RX vFIFO drain path |
| `hal_tx_dma_irq_handler` | `0xc0603f7c` | TX vFIFO completion/flush path |
| `_btif_enter_dpidle_from_on` | `0xc05fd5d8` | `do_gettimeofday` + 5 s cap + `msleep(1)` TX-drain wait |
| `btif_rx_thread` | `0xc05fe148` | `msecs_to_jiffies(360)` + `wait_for_completion_interruptible(_timeout)` + `btif_rx_data_consummer` |
| `btif_open` / `btif_close` | `0xc0601104` / `0xc0600ed4` | Controller bring-up/teardown entry points |
| `mtk_wcn_btif_open` | `0xc060231c` | Single-owner list check, `vmalloc(52)` user object, id = object pointer |
| `mtk_wcn_btif_read` | `0xc0602860` | **ROM is a stub**: `mov r0,#0; bx lr` (matches downstream `mtk_btif_exp.c:279-283`) |
| `mtk_wcn_btif_write/_dpidle_ctrl/_rx_cb_register/_wakeup_consys/_loopback_ctrl/_dbg_ctrl/_parser_wmt_evt` | `0xc0602768/2874/28b0/28e0/2908/2940/2b08` | API surface of the ROM matches the frozen header |
| `mtk_wcn_stpbt_drv_init` / `_exit` | `0xc054d72c` / `0xc054d8ac` | ROM Bluetooth user interface is the `/dev/stpbt` character device |
| `BT_open` / `BT_write` / `BT_read` / `bt_cdev_rst_cb` | `0xc054d4c8` / `0xc054cda0` / `0xc054cf04` / `0xc054d624` | ROM BT lifecycle = func-on/off + STP channel CB + reset CB (no in-kernel HCI) |
| `mtk_wcn_consys_stp_btif_open` / `_tx` | `0xc054ed9c` / `0xc054eb2c` | STP is the BTIF owner (`"CONSYS_STP"`) |
| ROM string `/system/etc/firmware/` | `vmlinux.strings:39659` | WMT firmware prefix |
| ROM string `WMT_SOC.cfg` | `vmlinux.strings:39660` | WMT config file |

`hal_btif_hw_init` disassembly (abridged) confirms every step ported:

```
0x0c FAKELCR   = 0
0x6c HANDSHAKE |= 1
0x08 FIFOCTRL  RMW |= 2 ; &= ~2 ; |= 4 ; &= ~4      (RX clear, TX clear)
0x60 TRI_LVL   = (rx&7)<<4 | (tx&0xf) ; then &= ~0x80 (loopback off)
0x4c DMA_EN    &= ~2 ; &= ~1 ; |= 4                  (PIO TX/RX, auto reset)
0x04 IER       &= ~2 ; |= 1                          (TX irq off, RX irq on)
```

Note that the ROM performs the FIFO-flush write as a read-modify-write on
offset `0x08`; on this IP a read of `0x08` returns IIR and a write targets
FIFOCTRL. The port reproduces the same read-modify-write so the written value
is bit-for-bit what the ROM writes.

`mtk_wcn_btif_read` returning 0 in the ROM is the one intentional behavioural
deviation: the frozen header documents `read` as the RX path for owners that do
not register a callback, and the task forbids stub implementations, so a real
ring-drain implementation is provided (§4). No in-tree caller uses it; the STP
owner always registers a callback.

### Downstream source citations (primary reference)

BTIF registers/bits: `btif/common/plat_inc/btif_priv.h:32-101`,
`btif/common/plat_inc/btif_dma_priv.h:30-145`.
HAL sequences: `btif/common/btif_plat.c:476-522` (init), `:535-575` (IER),
`:708-722` (loopback), `:816-860` (PIO RX), `:874-925` (TX-empty IRQ),
`:938-979` (mode), `:993-1075` (PIO TX), `:1088-1112` (wake),
`:1204-1241` (tx complete), `:1125-1192` (dump),
`btif/common/btif_dma_plat.c:151-251` (resource defaults), `:263-395` (APDMA
clock), `:397-460` (hw init), `:502-564` (enable/stop), `:576-591` (VFF reset),
`:604-655` (IER), `:687-761` (TX IRQ), `:775-871` (send), `:883-930`
(complete/room), `:942-959` (tx allow), `:974-1079` (RX IRQ), `:1081-1254`
(dumps).
Core: `btif/common/mtk_btif.c:1438-1522` (controller setup/init),
`:1524-1588` (open/close), `:1590-1763` (dpidle state machine), `:1832-1907`
(vFIFO), `:2067-2123` (RX consumer), `:2296-2323` (RX thread), `:2560-2625`
(staging ring), `:2882-2969` (TX), `:2999-3167` (logs), `:3184-3325` (module
init), `btif/common/mtk_btif_exp.c:74-507` (exported API).
BT: `connectivity/common/conn_soc/linux/pub/stp_chrdev_bt.c:105` (HW error
event), `:111-147` (reset CB), `:149-173` (RX event CB), `:205-250` (write),
`:252-345` (read), `:401-437` (open), `:439-456` (close);
`connectivity/common/conn_soc/linux/pri/stp_btif.c:64-65` (TX retry 10 × 5 ms),
`:74-93` (owner name `"CONSYS_STP"`); `conn_soc/include/wmt_exp.h:185-190`
(reset stages 0/1/2).

Hardware resources (no GPIO or regulator is used by BTIF itself; CONSYS
power/clock/GPIO sequencing belongs to the WMT leaf):
`arch/arm/boot/dts/mt6735.dtsi:689-699` (APDMA TX `0x11000880` IRQ 113, RX
`0x11000900` IRQ 114), `:861-867` (BTIF `0x1100c000` IRQ 90, clocks
`btifc`/`apdmac`); Nura clock gates `drivers/clk/mediatek/clk-mt6735-pericfg.c:42`
(`apdma`, bit 12) and `:52` (`btif`, bit 22) with
`include/dt-bindings/clock/mediatek,mt6735-pericfg.h:18,28`.

---

## 3. Parity matrix

### A. BTIF

| Behaviour | ROM / downstream | Before | After | Status |
|---|---|---|---|---|
| Platform probe/remove, 3 reg windows, 3 IRQs, `btifc`/`apdmac` clocks, 32-bit DMA mask | `mtk_btif.c:235-257,3184-3325`; DTS above | absent | done | done |
| Controller init (normal mode, handshake, FIFO flush, trigger levels 8/1, PIO default, auto-reset, RX IER on) | `btif_plat.c:476-522`; ROM `0xc06055c0` | absent | done | done |
| PIO TX via 4 KiB kfifo + TX-empty IRQ | `btif_plat.c:993-1075,874-925`; `plat_common.h:199` | absent | done | done |
| DMA TX via 8 KiB APDMA vFIFO, WPT publish, sub-8-byte flush | `btif_dma_plat.c:397-460,775-871,1256-1284`; ROM `0xc0603b24` | absent | done | done |
| PIO RX drain | `btif_plat.c:816-860` | absent | done | done |
| DMA RX vFIFO drain with wrap handling and RPT publish | `btif_dma_plat.c:974-1079` | absent | done | done |
| Owner/channel model: single owner, `id` = owner object pointer, `E_BTIF_ALREADY_OPEN` | `mtk_btif_exp.c:74-201`; ROM `0xc060231c` | absent | done | done |
| RX staging ring + `btif_rxd` thread (360 ms poll) + RX callback | `mtk_btif.c:2067-2123,2296-2323`; ROM `0xc05fe148` | absent | done | done |
| `mtk_wcn_btif_read` for callback-less owners | ROM stub `0xc0602860` | absent | real ring drain | done (deviation, §2) |
| `dpidle_ctrl` ON↔DPIDLE with 5 s TX drain and clock gating | `mtk_btif.c:1590-1763`; ROM `0xc05fd5d8` | absent | done | done |
| `wakeup_consys` (WAK pulse 64–96 µs, clock forced on) | `btif_plat.c:1088-1112`; ROM `0xc0605ac8` | absent | done | done |
| `loopback_ctrl` (TRI_LVL bit7) | `btif_plat.c:708-722` | absent | done | done |
| `dbg_ctrl`: logger enable/disable/dump/clear, real-time dump, BTIF+DMA register dump | `mtk_btif.c:2999-3167`; `mtk_btif_exp.c:429-491` | absent | done | done |
| `parser_wmt_evt` (search staged RX bytes, non-destructive) | `mtk_btif.c:1102-1166`; caller `wmt_ctrl.c:948-975` | absent | done | done |
| System suspend/resume (refuse when ON, re-init to DPIDLE) | `mtk_btif.c:259-435` | absent | done | done |
| `/dev/btif` debug character device | `mtk_btif.c:437-878` (`BTIF_CDEV_SUPPORT`) | absent | omitted | deferred (debug-only userspace ABI; task forbids procfs/char debug spam) |
| `restore_noirq` IRQ retype | `btif_plat.c:1312-1372` | absent | omitted | deferred: IRQ trigger comes from DT and survives resume; nothing to re-arm |
| WCP deep-idle notification | `hal_btif_pm_ops` returns 0 unconditionally (`btif_plat.c:1312-1372`) | absent | omitted | deferred (no WCP API in Nura) |
| Deep-idle stress, loopback data test, PIO-fallback soak | none in ROM source | — | — | deferred (needs hardware) |

### B. Bluetooth HCI

| Behaviour | ROM / downstream | Before | After | Status |
|---|---|---|---|---|
| HCI device registration (`hci_alloc_dev`/`hci_register_dev`, open/close/send) | ROM has no in-kernel HCI (`/dev/stpbt` only) | absent | done | done |
| open = `mtk_wcn_wmt_func_on(WMTDRV_BT)` → `mtk_wcn_stp_is_ready()` → STP event CB → WMT msg CB | `stp_chrdev_bt.c:401-437` | absent | done | done |
| close = msg CB unreg → STP event CB NULL → `func_off` | `stp_chrdev_bt.c:439-456` | absent | done | done |
| TX: H4 type byte + payload → `mtk_wcn_stp_send_data(..., BT_TASK_INDX)`, 10 × 5 ms retry | `stp_chrdev_bt.c:205-250`; `stp_btif.c:64-65` | absent | done | done |
| RX: STP event CB → `mtk_wcn_stp_receive_data` → H4 demux → `hci_recv_frame` | `stp_chrdev_bt.c:149-173,252-345` | absent | done | done |
| Whole-chip reset: refuse TX, flush parser, inject HCI Hardware Error `04 10 01 00` on reset end | `stp_chrdev_bt.c:105,111-147`; `wmt_exp.h:185-190` | absent | done | done |
| `rfkill` for the Bluetooth radio | ROM: userspace rfkill over `/dev/stpbt` | absent | provided by `hci_register_dev()` (`net/bluetooth/hci_core.c:2628-2639`); no driver rfkill object | done (core-provided) |
| `/dev/stpbt` char device | `stp_chrdev_bt.c:458-563`; ROM `0xc054d72c` | absent | intentionally not created | deferred by design (assignment: upstream interface is HCI) |
| `mtk_wcn_stp_set_bluez(0)` | `stp_chrdev_bt.c:419` | absent | omitted | deferred: no such call in the frozen STP API |

---

## 4. Compile test (exact commands and results)

```
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/wcn-bt
cp $M/nura.config .config
./scripts/config -m CONFIG_MTK_BTIF -m CONFIG_MTK_WCN_WMT -e CONFIG_BT -m CONFIG_MTK_WCN_BT
make ARCH=arm64 LLVM=1 olddefconfig
# .config: CONFIG_BT=y CONFIG_MTK_BTIF=m CONFIG_MTK_WCN_WMT=m CONFIG_MTK_WCN_BT=m

make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
  OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
  READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
  drivers/misc/mediatek/btif/btif_core.o \
  drivers/misc/mediatek/btif/btif_hw.o \
  drivers/misc/mediatek/btif/btif_dma.o \
  drivers/misc/mediatek/connectivity/bt/mtk_wcn_bt.o
```

Result (after deleting the four objects, full rebuild of the changed set):

```
  CC [M]  drivers/misc/mediatek/btif/btif_core.o
  CC [M]  drivers/misc/mediatek/btif/btif_hw.o
  CC [M]  drivers/misc/mediatek/btif/btif_dma.o
  CC [M]  drivers/misc/mediatek/connectivity/bt/mtk_wcn_bt.o
```

* zero warnings at default and at `W=1`.
* `scripts/checkpatch.pl --no-tree -f` on all six new files: **0 errors,
  0 warnings**.
* Module link (`... mtk_btif.ko`) reaches `LD [M]` and then stops in modpost
  with `vmlinux.o is missing` / unresolved core-kernel symbols. That is
  expected in a per-object build tree and is not a source defect; the
  integration build resolves it.

---

## 5. Inferred or unverified on hardware

* `[INFERENCE]` BTIF interrupt unmasking: downstream unmasks the BTIF IRQ only
  in the RX PIO setup (`mtk_btif.c:1248-1255`). The port unmasks it whenever
  *either* direction is PIO, otherwise the TX-empty interrupt that drives the
  PIO TX kfifo would never be delivered.
* `[INFERENCE]` 32-bit DMA mask for the APDMA vFIFO: downstream allocates it
  with `GFP_DMA32` (`mtk_btif.c:1862-1864`) and programs a 32-bit base register
  (`btif_dma_priv.h:66`), so `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))`
  is used before allocation.
* `[INFERENCE]` RX-callback return convention: the frozen callback documents
  "number of bytes consumed", while the downstream STP parser always returns 0
  meaning "handled". A return of 1..len consumes that many bytes; 0, negative
  or > len consumes the whole chunk (preserves downstream behaviour).
* `[INFERENCE]` H4 framing: the STP Bluetooth channel carries type-prefixed HCI
  frames. Downstream forwards the channel byte stream verbatim to `/dev/stpbt`
  (`stp_chrdev_bt.c:252-345`) and MTK's userspace stack speaks H4 over it; the
  kernel source does not state this explicitly, hence the marker.
* Integration requirement (not enforceable through the frozen STP API): the STP
  layer must stay in MTK (non-bluez) mode so BT payloads land in the
  `BT_TASK_INDX` queue. Downstream does this explicitly with
  `mtk_wcn_stp_set_bluez(0)` (`stp_chrdev_bt.c:419`); the alternative "bluez"
  path (`mtk_wcn_sys_if_rx`) is a stub in this tree
  (`conn_soc/linux/pri/stp_exp.c:85`). The frozen STP header exposes no
  `set_bluez`, so the default (0) must be preserved by the WMT leaf.
* `[INFERENCE]` DMA RX drain iteration cap (64) and BTIF ISR iteration cap
  (64): downstream loops are unbounded; the caps only guarantee forward
  progress on a wedged bus.
* `[INFERENCE]` IRQF_NO_AUTOEN + explicit `enable_irq()` replaces the
  downstream request/disable pair; the effect on the line is identical.
* Unverified on hardware: DMA vFIFO coherency/ordering on MT6737T, the exact
  APDMA flush latency, the 64 µs wake pulse timing, deep-idle entry with a
  live Bluetooth link, and the H4 stream boundary behaviour of the shipped
  firmware.
* The ROM `mtk_wcn_btif_read` stub is intentionally not reproduced; see §2.

---

## 6. BTIF register map citations

| Offset | Register | Downstream citation | Used for |
|---|---|---|---|
| `0x00` | RBR / THR | `btif_priv.h:32-33` | PIO RX byte read / PIO TX byte write |
| `0x04` | IER | `btif_priv.h:34,48-49` | TXEEN bit1, RXFEN bit0 |
| `0x08` | IIR (r) / FIFOCTRL (w) | `btif_priv.h:35-36,52-55,63-64` | IRQ identification; RX/TX FIFO clear |
| `0x0c` | FAKELCR | `btif_priv.h:37,67` | write 0 (normal mode) |
| `0x14` | LSR | `btif_priv.h:38,58-60` | DR bit0, THRE bit5, TEMT bit6 |
| `0x48` | SLEEP_EN | `btif_priv.h:39,70-71` | not programmed (downstream comment at `btif_plat.c:490`) |
| `0x4c` | DMA_EN | `btif_priv.h:40,74-78` | RX bit0, TX bit1, auto-reset bit2 |
| `0x54` | RTOCNT | `btif_priv.h:41` | not programmed (downstream comment `btif_plat.c:491`) |
| `0x60` | TRI_LVL | `btif_priv.h:42,81-88` | TX level 8, RX level 1, loopback bit7 |
| `0x64` | WAK | `btif_priv.h:43,91` | wake pulse bit0 |
| `0x68` | WAT_TIME | `btif_priv.h:44` | not programmed (downstream comment `btif_plat.c:492`) |
| `0x6c` | HANDSHAKE | `btif_priv.h:45,94-95` | new handshake bit0 |
| APDMA `0x00/0x04/0x08/0x0c/0x10/0x14` | INT_FLAG, INT_EN, EN, RST, STOP, FLUSH | `btif_dma_priv.h:44-49,98-104` | channel control |
| APDMA `0x1c/0x24/0x28/0x2c/0x30` | VFF_ADDR/LEN/THRE/WPT/RPT | `btif_dma_priv.h:51-56,66-70,87-91` | vFIFO |
| APDMA `0x38/0x3c/0x40/0x50` | INT_BUF_SIZE, VALID, LEFT, DBG | `btif_dma_priv.h:57-58,73-78,93-96` | completion/room/dump |

DMA thresholds: TX `size-7`, RX `size*3/4` (`btif_dma_priv.h:33-34`); vFIFO
size 8 KiB each (`:30-31`); staging ring 64 KiB (`mtk_btif.h:48`); PIO TX kfifo
4 KiB (`plat_common.h:199`).

---

## 7. Firmware / userspace requirements (no blobs added)

* This driver loads **no** firmware. BT controller firmware (the WMT
  ROM-patch) is downloaded by the WMT/CONSYS core during
  `mtk_wcn_wmt_func_on(WMTDRV_BT)`, which this HCI driver calls from
  `hdev->open()`. If the patch download fails, `func_on` returns false and the
  HCI open fails with `-EIO`.
* The patch path is supplied by userspace through the WMT launcher ABI:
  downstream `WMT_IOCTL_SET_PATCH_NAME` (`wmt_dev.c:73,1886-1894`) →
  `wmt_lib_set_patch_name()` (`wmt_lib.c:387`), with the default WMT config
  file `/system/etc/firmware/WMT_SOC.cfg`
  (`conn_soc/core/include/wmt_conf.h:33-34`; also present in the ROM at
  `vmlinux.strings:39659-39660`) and firmware prefix `/system/etc/firmware/`.
  No BT blob name is hard-coded in this kernel driver.
* Userspace must provide: the WMT patch/config under `/system/etc/firmware/`,
  the WMT control channel userspace daemon/launcher, and BlueZ (or any HCI
  consumer). The kernel now exposes a normal `hci0` device; `/dev/stpbt` is not
  created and no H4 relay process is needed.

---

## DTS-FRAGMENT

No shared DTS file was modified. The BTIF node is the canonical fragment,
verbatim (it already matches `mediatek,btif` resources used by the driver:
`reg[0]`=BTIF, `reg[1]`=APDMA TX, `reg[2]`=APDMA RX; `interrupts` 0/1/2 =
BTIF / TX DMA / RX DMA; clocks `btifc`, `apdmac`):

```dts
#include <dt-bindings/clock/mediatek,mt6735-infracfg.h>
#include <dt-bindings/clock/mediatek,mt6735-pericfg.h>
#include <dt-bindings/power/mediatek,mt6735-power-controller.h>

&soc {
	btif: btif@1100c000 {
		compatible = "mediatek,btif";
		reg = <0x1100c000 0x1000>,	/* BTIF */
		      <0x11000880 0x0080>,	/* APDMA TX */
		      <0x11000900 0x0080>;	/* APDMA RX */
		interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_LOW>,	/* BTIF */
			     <GIC_SPI 113 IRQ_TYPE_LEVEL_LOW>,	/* APDMA TX */
			     <GIC_SPI 114 IRQ_TYPE_LEVEL_LOW>;	/* APDMA RX */
		clocks = <&pericfg CLK_PERI_BTIF>, <&pericfg CLK_PERI_APDMA>;
		clock-names = "btifc", "apdmac";
		status = "okay";
	};
};
```

The `consys`, `wifi` nodes and the `consys_mem` reservation are owned by the
other WCN leaves and are unchanged here.

---

## 8. Patch

`$M/patches/wcn-bt.patch` (`git diff --binary`, 8 files, 2988 insertions;
`git apply --stat` and reverse-check both pass). Apply on top of the
coordinator scaffold in this worktree series.
