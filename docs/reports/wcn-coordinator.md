# WCN subsystem — coordinator report

Subsystem: MediaTek WCN / CONSYS (WMT core + Wi-Fi, Bluetooth, GPS, FM)
Target kernel: Nura `v7.0.10-mt6735`, arm64, MT6737T (SM-G532M "grandpplte")
Base commit: `$M/src/nura-linux` HEAD `046883c0c`
Downstream reference: `$M/src/downstream-grandpplte` (Samsung/MTK GPL, Android 6.0.1, 3.18)
ROM evidence: `$M/zip/vmlinux.elf`, `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`, `$M/zip/system.raw.img`
Hardware: none attached — everything below is source-level parity plus compile validation.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

---

## 1. Method

The Nura tree had **no** WCN driver at all (`drivers/misc/mediatek/` did not exist), so this was a
new-driver port rather than a parity patch. Work was split into a coordinator-owned scaffold plus
five independent drivers, each in its own worktree created from HEAD:

| Worktree | Owner | Directory |
|---|---|---|
| `$M/worktrees/wcn-base` | coordinator | scaffold (Kconfig/Makefile hooks, frozen ABI headers) |
| `$M/worktrees/wcn-wmt` | leaf | `connectivity/wmt/` |
| `$M/worktrees/wcn-wlan` | leaf | `connectivity/wlan/` |
| `$M/worktrees/wcn-bt` | leaf | `btif/` + `connectivity/bt/` |
| `$M/worktrees/wcn-gps` | leaf | `connectivity/gps/` |
| `$M/worktrees/wcn-fm` | leaf | `connectivity/fm/` |
| `$M/worktrees/wcn-integration` | coordinator | integration (base + all five leaves) |
| `$M/worktrees/wcn-verify` | coordinator | pristine HEAD + consolidated patch (end-to-end check) |

No worktree is shared between two workers; the pre-existing `$M/worktrees/wcn` and
`$M/worktrees/integration` were not touched.

### Frozen cross-driver ABI (coordinator-authored, `wcn-base.patch`)

Because four drivers consume the fifth, the inter-driver API was frozen before fan-out and is
byte-identical in every worktree (verified with `diff` after integration):

- `drivers/misc/mediatek/connectivity/include/mtk_wcn_stp.h` — STP channels, tx/rx/event API
- `drivers/misc/mediatek/connectivity/include/mtk_wcn_wmt.h` — functions, msgcb, WLAN registration
- `drivers/misc/mediatek/connectivity/include/mtk_wcn_consys.h` — CONSYS power/EMI/chip-id API
- `drivers/misc/mediatek/btif/include/mtk_btif.h` — BTIF open/close/write/read/rx-cb/wakeup API
- `drivers/misc/Kconfig`, `drivers/misc/Makefile` hooks, `drivers/misc/mediatek/{Kconfig,Makefile}`,
  `connectivity/{Kconfig,Makefile}` and one skeleton `Kconfig`/`Makefile` per driver directory.

**ABI audit (integration tree):** every function prototype in the four frozen headers has a real
implementation and an export — `grep -oE 'mtk_wcn_[a-z0-9_]+|mtk_btif_[a-z0-9_]+'` over the headers
yields 0 missing symbols; 41 × `EXPORT_SYMBOL_GPL` (WMT/STP/CONSYS/BTIF) plus the two WLAN module
entry points (`mtk_wcn_wlan_gen2_init/exit`, plain `EXPORT_SYMBOL`, see §8).

---

## 2. Leaf deliverables

| Leaf | Patch | Report | Insertions |
|---|---|---|---|
| scaffold | `$M/patches/wcn-base.patch` | (this report) | 537 (22 files) |
| WMT/CONSYS core | `$M/patches/wcn-wmt.patch` | `$M/logs/wcn-wmt.md` | 6 532 (15 files) |
| WLAN | `$M/patches/wcn-wlan.patch` | `$M/logs/wcn-wlan.md` | 1 809 (7 files) |
| Bluetooth (BTIF + HCI) | `$M/patches/wcn-bt.patch` | `$M/logs/wcn-bt.md` | 2 988 (8 files) |
| GPS | `$M/patches/wcn-gps.patch` | `$M/logs/wcn-gps.md` | 505 (2 files) |
| FM | `$M/patches/wcn-fm.patch` | `$M/logs/wcn-fm.md` | 3 222 (5 files) |
| **consolidated** | **`$M/patches/wcn-subsystem.patch`** | — | **15 592 (52 files)** |

New C/header code: 15 390 lines in 52 files, no binary hunks (`grep -c "GIT binary patch"` → 0),
no firmware blobs. No `TODO`/`FIXME`/`XXX`/placeholder markers and no `#if 0` dead code in the
delivered sources (the two grep hits are explanatory comments).

### What each leaf ported (evidence cited in the leaf reports)

**WMT/CONSYS core** — `stp_core.c` (framing `0x80|seq<<3|ack | type<<4|len_hi | len_lo | checksum`,
CRC16, 8 RX rings of 16 KiB, window 7, retry 10, timeout 180 — `stp_core.h:116-123`,
`stp_core.c:604-652,762-802`), `stp_btif.c` (`conn_soc/linux/pri/stp_btif.c`), `wmt_core.c`/
`wmt_ctrl.c`/`wmt_func.c`/`wmt_lib.c` with the exact opid numbering of
`wmt_core.h:122-153` (HIF_CONF 0, PWR_ON 1, PWR_OFF 2, FUNC_ON 3, FUNC_OFF 4, REG_RW 5, DSNS 8,
HW_RST 11, SW_RST 12, STP_RST 14, THERM 15, GPIO_CTRL 17, GPIO_STATE 19, ASSERT 30),
`consys_mt6735.c` (`CONSYS_PWRON_CONFG_EN_VALUE 0x0b160001`, `CPU_SW_RST_BIT BIT(12)`,
key `0x88<<24`, MBIST `BIT(18)`, EMI remap `0x320`, EMI window `+0x80000`/343 KiB, chip-id
`0x0321/0x0335/0x0337` — `mtk_wcn_consys_hw.h:68-173`), patch download incl. the 28-byte
`WMT_PATCH` header (`wmt_ic_soc.c:1988-2020`), `/dev/stpwmt` with the launcher ioctl ABI
`wmt_dev.c:72-88,1886+`, `/dev/wmtWifi`, `WMT_SOC.cfg` parser (`request_firmware`, DT fallback).

**WLAN** — AHB HIF at `0x180f0000` (`MCR_WCIR/WHLPCR/WHCR/WHISR/WHIER/WASR/WTSR/WTDR/WRDR/WRPLR/HSTCR`
= `0x00..0x58` per `gen2/include/nic/mtreg.h:81-142`; `WCIR_WLAN_READY BIT(21)`,
`WHLPCR_FW_OWN_REQ_SET BIT(8)` per `mtreg.h:175-187`), AP-DMA/PDMA descriptor rings, IRQ 226,
firmware bring-up (`request_firmware` of `WIFI_RAM_CODE_6735`, gen2 header + CRC32 validation,
sectioned/flat download, `INIT_CMD_WIFI_START`, WLAN_READY poll 512×10 ms), WMT WLAN
registration/reset handling, rfkill, suspend/resume.

**Bluetooth** — BTIF controller (`RBR/THR 0x0`, `IER 0x4`, `IIR/FCR 0x8`, `FAKELCR 0xc`,
`LSR 0x14`, `SLEEP_EN 0x48`, `DMA_EN 0x4c`, `RTOCNT 0x54`, `TRI_LVL 0x60`, `WAK 0x64`,
`WAT_TIME 0x68`, `HANDSHAKE 0x6c` — `btif/common/plat_inc/btif_priv.h:32-43`), PIO + APDMA rings,
ON/DPIDLE/SUSPEND state machine, plus an HCI device whose open/close powers
`WMTDRV_BT` and whose TX/RX run over STP channel `BT_TASK_INDX`.

**GPS** — GNSS-subsystem device (`gnss_allocate_device`/`gnss_insert_raw`) on STP channel
`GPS_TASK_INDX`, WMT GPS function control, reset-notification gating, rfkill, PM, and the
downstream `/sys` suspend-notification semantics.

**FM** — V4L2 radio (`v4l2_device` + `video_device`, `/dev/radio0`) with tuner/frequency/seek/audio
ioctls, private controls, RDS raw record path; STP `FM_TASK_INDX` link with the downstream
completion-bit event parser; MT6627 chip layer (power up/down, tune, seek, RSSI, RDS, DSP
patch/coeff download, 861-entry channel-parameter table), `0x9B=0xF9AB` and host
`0x80101054=0x00003f35` from `fmradio/mt6627/pub/mt6627_fm_lib.c:849-850`.

### Per-leaf compile results (as reported, commands verbatim in each report)

| Leaf | Command target | Result |
|---|---|---|
| WMT | 13 objects + `LD [M] mtk_wcn_wmt.o` | clean, 0 warnings |
| WLAN | `wlan_ahb.o wlan_fw.o wlan_main.o` | clean, twice (with/without `CONFIG_RFKILL`) |
| BT | `btif_core.o btif_hw.o btif_dma.o mtk_wcn_bt.o` | clean, `W=1` clean |
| GPS | `gps.o` | clean, `W=1` clean, checkpatch 0/0 |
| FM | `mtk_wcn_fm_{link,chip,v4l2}.o` + `LD [M] mtk_wcn_fm.o` | clean, 0 warnings |

---

## 3. Integration result

**Conflicts: none.** The five patches touch disjoint paths (`connectivity/wmt/`, `wlan/`, `bt/` +
`btif/*.c`, `gps/`, `fm/`) plus their own `Kconfig`/`Makefile`; all five applied to
`wcn-base.patch` with `git apply` on the first attempt. The only shared-file edits in the series are
the scaffold's `drivers/misc/Kconfig` + `drivers/misc/Makefile` lines.

Shared-state audit of the integration tree:

- `git status --porcelain` outside `drivers/misc/mediatek/` → only the two scaffold hook lines.
- Frozen headers/composite Kconfig/Makefile → byte-identical to `wcn-base` (`diff -q` clean).
- Leaf-only Kconfig deltas: WLAN adds `select CRC32` (firmware CRC validation) and
  `select RFKILL`; WMT/BT/GPS/FM Kconfig unchanged (RFKILL already selected by the scaffold).
- No `.dts`/`.dtsi` file is modified by any patch.

### Symbol integration audit (all six modules linked)

```
LD [M] drivers/misc/mediatek/btif/mtk_btif.o
LD [M] drivers/misc/mediatek/connectivity/wmt/mtk_wcn_wmt.o
LD [M] drivers/misc/mediatek/connectivity/wlan/mtk_wcn_wlan.o
LD [M] drivers/misc/mediatek/connectivity/bt/mtk_wcn_bt.o
LD [M] drivers/misc/mediatek/connectivity/gps/mtk_wcn_gps.o
LD [M] drivers/misc/mediatek/connectivity/fm/mtk_wcn_fm.o
```

- Duplicate global symbols across leaves: only `init_module`/`cleanup_module` (expected).
- Undefined `mtk_wcn_*`/`mtk_btif_*`/`wmt_*`/`stp_*` symbols after linking all six modules: **0**
  (cross-module references all resolve: 43 WMT/STP/CONSYS exports, 10 BTIF exports).
- Modules that would still need a full-kernel `modpost` are noted in §7; a per-object build cannot
  produce `Module.symvers` for the whole tree.

### Coordinator clean-tree build (authoritative)

`$M/worktrees/wcn-verify` = pristine HEAD + `$M/patches/wcn-subsystem.patch` only:

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd $M/worktrees/wcn-verify
git apply --check $M/patches/wcn-subsystem.patch && git apply $M/patches/wcn-subsystem.patch
cp $M/nura.config .config
./scripts/config -e GNSS -e MEDIA_SUPPORT -e MEDIA_RADIO_SUPPORT -e VIDEO_DEV -e BT -e CRC32 \
                 -m MTK_BTIF -m MTK_WCN_WMT -m MTK_WCN_WLAN -m MTK_WCN_BT -m MTK_WCN_GPS -m MTK_WCN_FM
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 AR=ar NM=aarch64-linux-gnu-nm \
     OBJCOPY=aarch64-linux-gnu-objcopy OBJDUMP=aarch64-linux-gnu-objdump \
     READELF=aarch64-linux-gnu-readelf STRIP=aarch64-linux-gnu-strip -j2 \
     $(ls drivers/misc/mediatek/connectivity/*/*.c drivers/misc/mediatek/btif/*.c | sed 's/\.c$/.o/')
```

Result: patch applies clean to pristine HEAD; `olddefconfig` exit 0; **24/24 objects compiled,
0 errors, 0 warnings** (log: `$M/logs/wcn-build-objects.log`, integration tree; the run in
`wcn-verify` produced the identical 24 `CC [M]` lines and exit 0).

---

## 4. Device-tree fragment (validated by building the DTB)

**Two corrections were required and are applied below** — the leaf reports contain the downstream
1-cell/`&soc` spelling, which does not work in this tree:

1. `mt6735.dtsi` has **no `soc` label**, so the fragment must use the path reference `&{/soc}`.
2. `soc` uses `#address-cells = <2>; #size-cells = <2>;`, so every `reg` entry needs two address
   cells and two size cells (`<0 ADDR 0 SIZE>`).

Validated by appending the fragment to `mt6737t-samsung-grandpplte.dts` in the throwaway
`wcn-verify` worktree and building `mediatek/mt6737t-samsung-grandpplte.dtb`: clean DTC, no
warnings, and `dtc -I dtb -O dts` shows IRQ `0xe3/0xe1/0xe2/0x5a/0x71/0x72` = 227/225/226/90/113/114,
GPIO `0x8c` = 140. The scratch DTS edit was reverted; **no DTS is part of the delivered series.**

```dts
/* add to the board DTS (e.g. mt6737t-samsung-grandpplte.dts), before the last node block */
#include <dt-bindings/clock/mediatek,mt6735-infracfg.h>
#include <dt-bindings/clock/mediatek,mt6735-pericfg.h>
#include <dt-bindings/power/mediatek,mt6735-power-controller.h>

&{/soc} {
	consys: consys@18070000 {
		compatible = "mediatek,mt6735-consys";
		reg = <0 0x18070000 0 0x0200>,	/* CONN_MCU_CONFIG_BASE */
		      <0 0x10212000 0 0x0100>,	/* AP_RGU_BASE */
		      <0 0x10000000 0 0x2000>,	/* TOPCKGEN_BASE */
		      <0 0x10006000 0 0x1000>;	/* SPM_BASE */
		interrupts = <GIC_SPI 227 IRQ_TYPE_LEVEL_LOW>,	/* BGF_EINT */
			     <GIC_SPI 225 IRQ_TYPE_LEVEL_LOW>;	/* WDT_EINT */
		clocks = <&infracfg CLK_INFRA_CONNMCU_BUS>;
		clock-names = "conn";
		power-domains = <&spm MT6735_POWER_DOMAIN_CONN>;
		memory-region = <&consys_mem>;
		vcn18-supply = <&mt6328_vcn18_reg>;
		vcn28-supply = <&mt6328_vcn28_reg>;
		vcn33_bt-supply = <&mt6328_vcn33_bt_reg>;
		vcn33_wifi-supply = <&mt6328_vcn33_wifi_reg>;
		status = "okay";
	};

	wifi: wifi@180f0000 {
		compatible = "mediatek,wifi";
		reg = <0 0x180f0000 0 0x0100>,	/* HIF registers */
		      <0 0x11000080 0 0x0080>;	/* AP-DMA channel 1 */
		interrupts = <GIC_SPI 226 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&pericfg CLK_PERI_APDMA>;
		clock-names = "wifi-dma";
		status = "okay";
	};

	btif: btif@1100c000 {
		compatible = "mediatek,btif";
		reg = <0 0x1100c000 0 0x1000>,	/* BTIF */
		      <0 0x11000880 0 0x0080>,	/* APDMA TX */
		      <0 0x11000900 0 0x0080>;	/* APDMA RX */
		interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_LOW>,
			     <GIC_SPI 113 IRQ_TYPE_LEVEL_LOW>,
			     <GIC_SPI 114 IRQ_TYPE_LEVEL_LOW>;
		clocks = <&pericfg CLK_PERI_BTIF>, <&pericfg CLK_PERI_APDMA>;
		clock-names = "btifc", "apdmac";
		status = "okay";
	};

	gps: gps {
		compatible = "mediatek,mt3326-gps";
		status = "okay";
	};

	mt_fmradio_mt6627: mt_fmradio_mt6627 {
		compatible = "mediatek,mt_mt6627_fmradio";
		fmradio_ant_sel = <&pio 140 GPIO_ACTIVE_HIGH>;
		status = "okay";
	};
};
```

Fragment provenance:

- consys resources: downstream `arch/arm64/boot/dts/mt6735.dtsi:1534-1546` and the binding
  `Documentation/devicetree/bindings/consys/mediatek-consys.txt`; IRQ/GIC numbers, clock gates,
  PM domain and `vcn*` rails all exist in Nura already
  (`CLK_INFRA_CONNMCU_BUS` = `mediatek,mt6735-infracfg.h:16`, `MT6735_POWER_DOMAIN_CONN` =
  `mediatek,mt6735-power-controller.h`, rails in `mt6328.dtsi:118-141,263-266`).
- `memory-region = <&consys_mem>` reuses the 1 MiB `no-map` reservation that
  `mt6735.dtsi:99-102` already declares at `0x7f800000` (downstream instead allocates the same
  1 MiB/2 MiB-aligned region dynamically: `arch/arm/boot/dts/mt6735.dtsi:205-210`).
- wifi: downstream `mt6735.dtsi:1549-1555`; the second `reg` (`0x11000080`) is the AP-DMA channel
  the downstream driver hard-codes; without it the driver falls back to PIO.
- btif: downstream `mt6735.dtsi:696-705` (APDMA TX/RX) and `:931-937` (BTIF controller).
- gps: downstream `arch/boot/dts/mt6735.dtsi` node with `compatible = "mediatek,mt3326-gps"`
  (ROM string `mediatek,mt3326-gps`).
- FM: downstream `arch/arm/boot/dts/mt6737t_grandpplte_ltn_dtv_03.dts:1299-1304`
  (`fmradio_ant_sel = <&pio 140 0>`); ROM string `mediatek,mt_mt6627_fmradio`.
- **Optional** consys pinctrl: if the board drives the GPS LNA from the AP, add
  `pinctrl-names = "default","gps_lna_state_init","gps_lna_state_oh","gps_lna_state_ol"` plus
  `pinctrl-0..3` pointing at board pin states. The driver treats pinctrl as optional
  (`devm_pinctrl_get` failure and missing states are tolerated, `consys_mt6735.c:644-681,794-801`
  following `wmt_plat_alps.c:685-730`); the fragment above deliberately omits it because Nura has
  no `consys_pins_default`/`consys_gps_lna_*` nodes yet.

### Required config fragment

The new symbols are `default n` and are absent from `$M/nura.config` (which was not modified).
Append to the board config:

```
CONFIG_BT=m                     # HCI core for the Bluetooth driver
CONFIG_GNSS=y                   # gnss subsystem for the GPS driver
CONFIG_MEDIA_SUPPORT=y
CONFIG_MEDIA_RADIO_SUPPORT=y
CONFIG_VIDEO_DEV=y              # V4L2 radio for the FM driver
CONFIG_MTK_BTIF=m
CONFIG_MTK_WCN_WMT=m
CONFIG_MTK_WCN_WLAN=m
CONFIG_MTK_WCN_BT=m
CONFIG_MTK_WCN_GPS=m
CONFIG_MTK_WCN_FM=m
```

`CONFIG_RFKILL` and `CONFIG_CRC32` are selected by the drivers
(`RFKILL` also by `CONFIG_BT`); `olddefconfig` resolves them.

---

## 5. Status per driver: complete vs deferred

| Driver | Complete | Deferred (in code: omitted, documented in leaf report) |
|---|---|---|
| WMT/CONSYS | STP engine (both parsers, seq/ack window, CRC16, 8 rings, event/tx callbacks), BTIF transport, WMT op/ctrl/function engines, CONSYS power-on/off/reset/EMI/pin sequences, chip-id/hwver, patch download, `/dev/stpwmt` launcher ABI, `/dev/wmtWifi`, `WMT_SOC.cfg` | PSM sleep/wakeup engine, coredump/AEE/stp_dbg, IDC/LTE coexistence, loopback, cmd-test, efuse, MCU-clk op, ADIE, BGW desense, EMI MPU region programming, `PORT_NAME` ioctl (`-EOPNOTSUPP`), coexistence keys of `WMT_SOC.cfg` (parsed, ignored) |
| WLAN | AHB HIF + PDMA rings, IRQ/tx/rx, firmware download and ready handshake, WMT registration/reset, rfkill, PM | **entire 802.11 MAC/mgmt engine** (gen2 `nic/` + `mgmt/`, ~130 k lines): no cfg80211, no netdev (a wiphy without the mgmt path would be fake); firmware power-off handshake, GDMA engine, EMI MPU for the WLAN window, `HIF_DEV_MISC` chardev/loopback |
| Bluetooth | BTIF (PIO + APDMA, dpidle/wakeup/loopback/dbg, WMT-event hook), HCI-over-STP device with WMT power and reset handling | `/dev/btif` debug chardev, IRQ retype on resume, WCP deep-idle notification, ROM `/dev/stpbt` ABI (replaced by HCI by design), hardware soak |
| GPS | GNSS device over STP, WMT power, reset gating, rfkill, PM, suspend notification | downstream chardev ioctls 0/1/6/7/8/9/10/11 (no GNSS ABI; efuse read belongs to nvmem; RTC flag is a constant 0), `/dev/mtkgps` echo device, `pwrctl/rdelay/state/pwrsave/status` sysfs (proven hardware no-ops on CONSYS) |
| FM | V4L2 radio ioctls + controls + RDS raw path, STP link with completion-bit parsing, MT6627 power/tune/seek/RSSI/RDS, DSP patch+coeff download, rfkill | RDS group decoder (PS/RT/AF/PI/PTY/ECC/CT) — raw 148-byte records delivered instead; scan/CQI/soft-mute-tune; FM TX; FM-over-BT; `/proc/fm`; **FM audio routing to SoC I2S/ASoC** (no MT6735 AFE/codec in Nura) — chip-side registers are programmed, DAI/PCM names documented |

### Hardware-only unknowns (must be validated on a device)

- CONSYS reset/de-assert timing (150 µs VCN18, 10 µs before bus clock, 20 ms after reset), the
  decision to let the MT6735 CONN genpd do the SPM gating instead of hand-written SPM writes, and
  whether the EMI remap write takes effect.
- Patch/firmware download against real silicon (`ROMv2_lm_patch_1_0_hdr.bin`/`_1_1_hdr.bin`,
  `WIFI_RAM_CODE_6735`), the STA/AP/`WMT_SOC.cfg` values, and the WLAN `WCIR` chip-id/ACK sequence.
- GPS LNA pinctrl states and GPS reset-notification timing.
- FM patch/coeff blobs, seek behaviour, RDS ABI zero-padding, rfkill transitions while a client
  holds the device open.
- BTIF DMA under sustained load, deep-idle entry/exit, and the ROM's 360 ms RX poll interval.
- Every `[INFERENCE]` marker listed in the five leaf reports (§5 of each).

### Runtime files userspace must supply (no blobs added)

- WMT: `/system/etc/firmware/WMT_SOC.cfg` (optional; DT/`request_firmware` fallback), and the
  CONSYS patch binaries whose names/addresses the launcher passes with
  `WMT_IOCTL_SET_PATCH_INFO` (ROM image contains `ROMv2_lm_patch_1_0_hdr.bin` 131 812 B,
  `ROMv2_lm_patch_1_1_hdr.bin` 68 164 B under `/system/etc/firmware/`).
- WLAN: `WIFI_RAM_CODE_6735` (`/etc/firmware/WIFI_RAM_CODE_6735` in the ROM; the kernel firmware
  search path must resolve it, e.g. `/lib/firmware/WIFI_RAM_CODE_6735`).
- FM: `mt6627_fm_v{1..5}_patch.bin` and `mt6627_fm_v{1..5}_coeff.bin`
  (`/etc/firmware/mt6627/` in the ROM, `request_firmware` name `mt6627/...`).
- BT/GPS: no blob of their own; both are downloaded as CONSYS functions through the WMT patch path.

---

## 6. Review notes / residual risks

1. **WLAN is transport-only.** This is the one deliberate scope reduction: 135 k downstream lines of
   802.11 stack cannot be ported in one pass, and a cfg80211 device without it would advertise
   capabilities it cannot honour. The report states the boundary explicitly; the next step is a
   second pass over `gen2/nic` + `gen2/mgmt` on top of this HIF.
2. `EXPORT_SYMBOL` (non-GPL) is used for `mtk_wcn_wlan_gen2_init/exit` to mirror the ROM symbol
   names; every cross-driver API is `EXPORT_SYMBOL_GPL`. Left as-is deliberately (would otherwise
   differ from the ROM symbol contract).
3. FM RDS is delivered as raw records, not decoded groups — a userspace/next-pass item.
4. WMT power-save (PSM) is not implemented, so the link stays awake; functionally correct, higher
   power than the ROM.
5. Both the leaf reports and this report mark a large `[INFERENCE]`/unverified set; nothing in this
   series has run on hardware.

---

## 7. Reproduction / next steps for the integration owner

```sh
# 1. apply the series
cd $M/worktrees/wcn-integration          # HEAD + wcn-base + 5 leaf patches (already applied)
#   or, from a clean tree:
git apply $M/patches/wcn-subsystem.patch

# 2. config fragment (§4) + DTS fragment (§4), then a full build
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j4

# 3. after a full build, re-check modpost for the six modules
#    (per-object builds cannot produce Module.symvers, so .ko modpost was not run here)
```

Checks still outstanding and owned by the integration/main stage: full arm64 build (which also
resolves `modpost` for `mtk_wcn_wmt.ko`, `mtk_btif.ko`, `mtk_wcn_wlan.ko`, `mtk_wcn_bt.ko`,
`mtk_wcn_gps.ko`, `mtk_wcn_fm.ko`), DTS fragment application to the board file, and any
`checkpatch` gate the program runs on the applied series.

---

## 8. Artefacts

- `$M/patches/wcn-subsystem.patch` — consolidated series (scaffold + 5 leaves), applies clean to
  `046883c0c`.
- `$M/patches/wcn-base.patch`, `wcn-wmt.patch`, `wcn-wlan.patch`, `wcn-bt.patch`,
  `wcn-gps.patch`, `wcn-fm.patch` — per-leaf patches.
- `$M/logs/wcn-wmt.md`, `wcn-wlan.md`, `wcn-bt.md`, `wcn-gps.md`, `wcn-fm.md` — leaf reports.
- `$M/logs/wcn-build-objects.log` — coordinator 24-object rebuild log.
- `$M/worktrees/wcn-integration` — integrated tree (base + all leaves, uncommitted).
- `$M/worktrees/wcn-verify` — pristine HEAD + consolidated patch (end-to-end compile/DTB check).
