# CCCI subsystem contract (coordinator-owned, read-only for leaves)

Target: Samsung SM-G532M (grandpplte, MT6737T) CCCI modem subsystem recreated on the
Nura `v7.0.10-mt6735` arm64 tree. The ROM kernel is 3.18 and the Nura tree is 7.0.10,
so the downstream code is a *reference*, not a verbatim port: keep the protocol,
register layout, channel numbers and state machine, but use current kernel APIs.

`$M` = `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/g532m_work`

## 1. Fixed facts (do not re-derive)

- Nura base: `$M/src/nura-linux` @ `046883c0c` (detached HEAD).
- Downstream reference: `$M/src/downstream-grandpplte`.
- ROM evidence: `$M/zip/symbols.txt`, `$M/zip/vmlinux.strings`, `$M/zip/vmlinux.elf`
  (32-bit ARM; use `$M/llvmpkg/usr/bin/llvm-objdump -d --start-address=0x... --stop-address=0x...`).
- ROM symbols that define the shape of this subsystem (confirmed in `symbols.txt`):
  `ccci_modem_probe`, `ccci_modem_remove`, `ccci_modem_shutdown`, `ccci_modem_suspend`,
  `ccci_modem_resume`, `ccci_modem_pm_ops`, `ccci_subsys_bm_init`, `ccci_subsys_char_init`,
  `ccci_subsys_kernel_init`, `ccmni_init`, `ccmni_exit`, `ccmni_open`, `ccmni_close`,
  `ccmni_start_xmit`, `ccmni_rx_callback`, `ccmni_md_state_callback`, `ccmni_napi_poll`,
  `ccmni_select_queue`, `ccmni_change_mtu`, `ccmni_tx_timeout`, `ccmni_ioctl`,
  `ccmni_get_stats`, `ccci_get_ccmni_channel`, `ccmni_send_pkt`,
  `md_cd_ccif_isr`, `md_ccif_irq_tasklet`, `ccif_enable_irq`, `ccif_disable_irq`,
  `cldma_isr`, `cldma_tx_ring_init`, `cldma_rx_ring_init`, `cldma_gpd_handle_tx_request`,
  `cldma_gpd_tx_collect`, `cldma_gpd_rx_collect`, `cldma_gpd_rx_refill`,
  `cldma_rx_refill`, `cldma_tx_done`, `cldma_rx_done`, `cldma_net_rx_push_thread`,
  `ccci_util_fo_init`, `ccci_util_mem_dump`, `ccci_log_init`, `ccci_dump_write`,
  `ccci_load_firmware`, `ccci_init_security`, `ccci_get_md_check_hdr_inf`,
  `ccci_parse_meta_md_setting`, `ccci_md_mem_reserve`, `ccci_get_opt_val`.
- There is **no** C2K SDIO/CBP driver in the ROM build (only `mtk_wdt_set_c2k_sysrst`
  matches `c2k`); `c2k_ccci/{modem_sdio,cbp_sdio,rawbulk}*.c` is legacy for other
  platforms. MD3 (C2K) on MT6735 is a second CCIF/CLDMA instance
  (`drivers/misc/mediatek/eccci/mt6735/ccif_c2k_platform.c`, `modem_ccif_c2k.c`).
- MD1 transport on this platform is CLDMA (data) + CCIF (control), see downstream
  `arch/arm64/boot/dts/mt6735.dtsi` node `mdcldma@1000A000` (regs/IRQs below).

## 2. Worktrees and file ownership (never edit another leaf's files)

| Leaf | Worktree | Owned new files | Patch | Report |
|---|---|---|---|---|
| ccci-core | `$M/worktrees/ccci-eccci` | `drivers/misc/mediatek/ccci/*` | `$M/patches/ccci-eccci.patch` | `$M/logs/ccci-eccci.md` |
| ccci-cldma | `$M/worktrees/ccci-cldma` | `drivers/misc/mediatek/ccci_cldma/*` | `$M/patches/ccci-cldma.patch` | `$M/logs/ccci-cldma.md` |
| ccci-util | `$M/worktrees/ccci-util` | `drivers/misc/mediatek/ccci_util/*` | `$M/patches/ccci-util.patch` | `$M/logs/ccci-util.md` |
| ccmni | `$M/worktrees/ccci-ccmni` | `drivers/net/ccmni/*` | `$M/patches/ccmni.patch` | `$M/logs/ccmni.md` |
| c2k | `$M/worktrees/ccci-c2k` | `drivers/misc/mediatek/ccci_c2k/*` | `$M/patches/ccci-c2k.patch` | `$M/logs/ccci-c2k.md` |

Every leaf also creates `include/linux/ccci.h` **with the exact text in section 5**
(same bytes in every leaf; the coordinator de-duplicates at integration) and adds its
own one-line hook to the shared parents (`drivers/misc/Kconfig`, `drivers/misc/Makefile`,
`drivers/net/Kconfig`, `drivers/net/Makefile`, `drivers/misc/mediatek/{Kconfig,Makefile}`).
Textual conflicts in those parents are expected and are resolved by the coordinator.

Do **not** touch: `$M/worktrees/integration`, `$M/worktrees/ccci`, any other existing
worktree, `arch/arm64/boot/dts/**`, `$M/nura.config`, or other subsystems' drivers.

## 3. Kconfig / Makefile rules

- Kconfig symbol per leaf, `bool`, **no cross-leaf `depends on`** (the coordinator adds
  the final dependencies at integration):
  - `MTK_CCCI` (core), `MTK_CCCI_CLDMA`, `MTK_CCCI_UTIL`, `MTK_CCCI_C2K`, `MTK_CCMNI`.
- `drivers/misc/mediatek/Kconfig` (create if absent) contains `menu "MediaTek device drivers"`
  + your single `source "drivers/misc/mediatek/<your dir>/Kconfig"` + `endmenu`.
- `drivers/misc/mediatek/Makefile` (create if absent) contains your single `obj-y += <your dir>/`.
- Hook into `drivers/misc/{Kconfig,Makefile}` (mediatek leaves) or
  `drivers/net/{Kconfig,Makefile}` (ccmni leaf) with exactly one line each.

## 4. Build / compile test (must pass before yielding)

```sh
export PATH="$M/llvmpkg/usr/bin:$M/bin:$PATH"
cd "$M/worktrees/<your worktree>"
cp "$M/nura.config" .config
scripts/config -e <YOUR CONFIG SYMBOL>
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j2 <each changed .o target>
```

Also build every object you add (one `make` invocation may take all object paths).
Do not run a full kernel build. Do not modify kernel sources to work around tools.
Record the exact command and the result in the report.

## 5. Shared header — create verbatim as `include/linux/ccci.h`

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCCI (Cross Chip Communication Interface) public kernel API.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/include/mt-plat/mt_ccci_common.h
 *   drivers/misc/mediatek/eccci/ccci_core.h
 *   drivers/misc/mediatek/ccmni/ccmni.h
 *
 * This file is the frozen contract between the CCCI core, the CLDMA engine,
 * the CCMNI netdev driver, the CCCI util driver and the C2K/dual-modem driver.
 * Do not change existing declarations; append only if unavoidable.
 */
#ifndef __LINUX_CCCI_H
#define __LINUX_CCCI_H

#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/device.h>

/* Modem ids. Downstream md_id is 0-based (MD_SYS1 == 0). */
enum ccci_md_id {
	CCCI_MD_SYS1 = 0,
	CCCI_MD_SYS2,
	CCCI_MD_SYS3,
	CCCI_MD_SYS4,
	CCCI_MD_SYS5,
	CCCI_MAX_MD_NUM,
};

/* Modem states, mirror downstream MD_STATE (eccci/ccci_core.h). */
enum ccci_md_state {
	CCCI_MD_INVALID = 0,
	CCCI_MD_BOOTING,
	CCCI_MD_READY,
	CCCI_MD_EXCEPTION,
	CCCI_MD_STOPPED,
};

/* Logical channel ids, verbatim from downstream mt_ccci_common.h. */
enum ccci_ch {
	CCCI_CONTROL_RX = 0,
	CCCI_CONTROL_TX = 1,
	CCCI_SYSTEM_RX = 2,
	CCCI_SYSTEM_TX = 3,
	CCCI_PCM_RX = 4,
	CCCI_PCM_TX = 5,
	CCCI_UART1_RX = 6,
	CCCI_UART1_RX_ACK = 7,
	CCCI_UART1_TX = 8,
	CCCI_UART1_TX_ACK = 9,
	CCCI_UART2_RX = 10,
	CCCI_UART2_RX_ACK = 11,
	CCCI_UART2_TX = 12,
	CCCI_UART2_TX_ACK = 13,
	CCCI_FS_RX = 14,
	CCCI_FS_TX = 15,
	CCCI_PMIC_RX = 16,
	CCCI_PMIC_TX = 17,
	CCCI_UEM_RX = 18,
	CCCI_UEM_TX = 19,
	CCCI_CCMNI1_RX = 20,
	CCCI_CCMNI1_RX_ACK = 21,
	CCCI_CCMNI1_TX = 22,
	CCCI_CCMNI1_TX_ACK = 23,
	CCCI_CCMNI2_RX = 24,
	CCCI_CCMNI2_RX_ACK = 25,
	CCCI_CCMNI2_TX = 26,
	CCCI_CCMNI2_TX_ACK = 27,
	CCCI_CCMNI3_RX = 28,
	CCCI_CCMNI3_RX_ACK = 29,
	CCCI_CCMNI3_TX = 30,
	CCCI_CCMNI3_TX_ACK = 31,
	CCCI_RPC_RX = 32,
	CCCI_RPC_TX = 33,
	CCCI_IPC_RX = 34,
	CCCI_IPC_RX_ACK = 35,
	CCCI_IPC_TX = 36,
	CCCI_IPC_TX_ACK = 37,
	CCCI_IPC_UART_RX = 38,
	CCCI_IPC_UART_RX_ACK = 39,
	CCCI_IPC_UART_TX = 40,
	CCCI_IPC_UART_TX_ACK = 41,
	CCCI_MD_LOG_RX = 42,
	CCCI_MD_LOG_TX = 43,
	CCCI_IT_RX = 50,
	CCCI_IT_TX = 51,
	CCCI_IMSV_UL = 52,
	CCCI_IMSV_DL = 53,
	CCCI_IMSC_UL = 54,
	CCCI_IMSC_DL = 55,
	CCCI_IMSA_UL = 56,
	CCCI_IMSA_DL = 57,
	CCCI_IMSDC_UL = 58,
	CCCI_IMSDC_DL = 59,
	CCCI_ICUSB_RX = 60,
	CCCI_ICUSB_TX = 61,
	CCCI_LB_IT_RX = 62,
	CCCI_LB_IT_TX = 63,
	CCCI_CCMNI1_DL_ACK = 64,
	CCCI_CCMNI2_DL_ACK = 65,
	CCCI_CCMNI3_DL_ACK = 66,
	CCCI_STATUS_RX = 67,
	CCCI_STATUS_TX = 68,
	CCCI_CCMNI4_RX = 69,
	CCCI_CCMNI4_RX_ACK = 70,
	CCCI_CCMNI4_TX = 71,
	CCCI_CCMNI4_TX_ACK = 72,
	CCCI_CCMNI4_DLACK_RX = 73,
	CCCI_CCMNI5_RX = 74,
	CCCI_CCMNI5_RX_ACK = 75,
	CCCI_CCMNI5_TX = 76,
	CCCI_CCMNI5_TX_ACK = 77,
	CCCI_CCMNI5_DLACK_RX = 78,
	CCCI_CCMNI6_RX = 79,
	CCCI_CCMNI6_RX_ACK = 80,
	CCCI_CCMNI6_TX = 81,
	CCCI_CCMNI6_TX_ACK = 82,
	CCCI_CCMNI6_DLACK_RX = 83,
	CCCI_CCMNI7_RX = 84,
	CCCI_CCMNI7_RX_ACK = 85,
	CCCI_CCMNI7_TX = 86,
	CCCI_CCMNI7_TX_ACK = 87,
	CCCI_CCMNI7_DLACK_RX = 88,
	CCCI_CCMNI8_RX = 89,
	CCCI_CCMNI8_RX_ACK = 90,
	CCCI_CCMNI8_TX = 91,
	CCCI_CCMNI8_TX_ACK = 92,
	CCCI_CCMNI8_DLACK_RX = 93,
	CCCI_MDL_MONITOR_DL = 94,
	CCCI_MDL_MONITOR_UL = 95,
	CCCI_RIL_IPC0_RX = 148,
	CCCI_RIL_IPC0_TX = 149,
	CCCI_RIL_IPC1_RX = 150,
	CCCI_RIL_IPC1_TX = 151,
	CCCI_VT_CTL_RX = 152,
	CCCI_VT_CTL_TX = 153,
	CCCI_C2K_PPP_DATA,
	CCCI_C2K_AT,
	CCCI_C2K_AT2,
	CCCI_C2K_AT3,
	CCCI_C2K_LB_DL,
	CCCI_MONITOR_CH,
	CCCI_DUMMY_CH,
	CCCI_MAX_CH_NUM,
	CCCI_MONITOR_CH_ID = 0xf0000000,
	CCCI_FORCE_ASSERT_CH = 20090215,
	CCCI_INVALID_CH_ID = 0xffffffff,
};

/* C2K channel ids, verbatim from downstream mt_ccci_common.h. */
enum c2k_channel {
	CTRL_CH_C2K = 0,
	AUDIO_CH_C2K = 1,
	DATA_PPP_CH_C2K = 2,
	MDLOG_CTRL_CH_C2K = 3,
	FS_CH_C2K = 4,
	AT_CH_C2K = 5,
	AGPS_CH_C2K = 6,
	AT2_CH_C2K = 7,
	AT3_CH_C2K = 8,
	MDLOG_CH_C2K = 9,
	STATUS_CH_C2K = 11,
	NET1_CH_C2K = 12,
	NET2_CH_C2K = 13,
	NET3_CH_C2K = 14,
	NET4_CH_C2K = 15,
	NET5_CH_C2K = 16,
	C2K_MAX_CH_NUM,
};

/* Downstream struct ccci_header (16 bytes, little endian). */
struct ccci_header {
	__le32 data[2];
	u16 channel;
	u16 seq_num:15;
	u16 assert_bit:1;
	__le32 reserved;
} __packed;

/* Modem capabilities, verbatim downstream MODEM_CAP_*. */
#define CCCI_MD_CAP_NAPI		(1 << 0)
#define CCCI_MD_CAP_TXBUSY_STOP		(1 << 1)
#define CCCI_MD_CAP_SGIO		(1 << 2)
#define CCCI_MD_CAP_CCMNI_DISABLE	(1 << 16)
#define CCCI_MD_CAP_DATA_ACK_DVD	(1 << 17)
#define CCCI_MD_CAP_CCMNI_SEQNO		(1 << 18)
#define CCCI_MD_CAP_CCMNI_IRAT		(1 << 19)
#define CCCI_MD_CAP_WORLD_PHONE		(1 << 20)
#define CCCI_MD_CAP_CCMNI_MQ		(1 << 21)

/* ------------------------------------------------------------------ */
/* CCMNI <-> CCCI core binding (implemented by ccmni, used by core)    */
/* ------------------------------------------------------------------ */
struct ccmni_ch {
	int rx;
	int rx_ack;
	int tx;
	int tx_ack;
	int dl_ack;
	int multiq;
};

#define CCMNI_DRV_V0	0
#define CCMNI_DRV_V1	1
#define CCMNI_DRV_V2	2

struct ccmni_ccci_ops {
	int ccmni_ver;
	int ccmni_num;
	unsigned char name[16];
	unsigned int md_ability;
	unsigned int irat_md_id;
	unsigned int napi_poll_weigh;
	int (*send_pkt)(int md_id, int tx_ch, void *data);
	int (*napi_poll)(int md_id, int rx_ch, struct napi_struct *napi, int weight);
	int (*get_ccmni_ch)(int md_id, int ccmni_idx, struct ccmni_ch *channel);
};

int ccmni_init(int md_id, struct ccmni_ccci_ops *ccci_info);
void ccmni_exit(int md_id);
int ccmni_rx_callback(int md_id, int ccmni_idx, struct sk_buff *skb);
void ccmni_md_state_callback(int md_id, int ccmni_idx, enum ccci_md_state state);

/* ------------------------------------------------------------------ */
/* Modem instantiation (implemented by the core, used by platform/C2K) */
/* ------------------------------------------------------------------ */
struct ccci_md_cfg {
	unsigned int md_id;			/* enum ccci_md_id */
	const char *name;			/* "md1", "md3" */
	const char *firmware_name;		/* request_firmware() name */
	phys_addr_t ccif_phys;			/* AP CCIF register block */
	resource_size_t ccif_size;
	unsigned int ccif_irq;
	phys_addr_t md_boot_vector_phys;	/* 0 if unused */
	phys_addr_t md_boot_vector_key_phys;
	phys_addr_t md_boot_vector_en_phys;
	phys_addr_t rgu_phys;
	unsigned int sram_size;			/* CCIF SRAM bytes */
	unsigned int capability;		/* CCCI_MD_CAP_* */
	unsigned int irat_md_id;		/* CCCI_MAX_MD_NUM if none */
};

int ccci_register_modem(const struct ccci_md_cfg *cfg);
void ccci_unregister_modem(unsigned int md_id);
int ccci_md_get_state(unsigned int md_id);
int ccci_md_set_irat(unsigned int md_id, unsigned int irat_md_id);

/* ------------------------------------------------------------------ */
/* CLDMA engine (implemented by ccci_cldma, used by the core)          */
/* ------------------------------------------------------------------ */
#define CCCI_CLDMA_Q_BASIC	0
#define CCCI_CLDMA_Q_NET	1

struct ccci_cldma;
struct ccci_cldma_cfg {
	struct device *dev;
	void __iomem *ap_base;		/* AP_CLDMA_AO */
	void __iomem *ap_pdn_base;	/* AP_CLDMA_PDN */
	unsigned int irq;
	phys_addr_t smem_phys;
	size_t smem_size;
};

struct ccci_cldma *ccci_cldma_create(const struct ccci_cldma_cfg *cfg);
void ccci_cldma_destroy(struct ccci_cldma *cd);
int ccci_cldma_start(struct ccci_cldma *cd);
void ccci_cldma_stop(struct ccci_cldma *cd);
int ccci_cldma_send(struct ccci_cldma *cd, unsigned int qno, struct sk_buff *skb);
void ccci_cldma_set_rx_cb(struct ccci_cldma *cd,
			  void (*cb)(void *ctx, unsigned int qno, struct sk_buff *skb),
			  void *ctx);

/* ------------------------------------------------------------------ */
/* CCCI util (implemented by ccci_util, used by core)                  */
/* ------------------------------------------------------------------ */
int ccci_util_init(void);
void ccci_util_exit(void);
int ccci_util_log_init(unsigned int md_id, struct device *dev);
void ccci_util_log_exit(unsigned int md_id);
void ccci_util_log(unsigned int md_id, const char *fmt, ...) __printf(2, 3);
void ccci_util_dump_mem(unsigned int md_id, const void *buf, size_t len);

#endif /* __LINUX_CCCI_H */
```

If you need an extra symbol, append it at the end of the file (never change the above);
say so in your report so the coordinator can fold it into the final header.

## 6. Cross-leaf behaviour contract

- Core owns MD state; it calls `ccmni_init()`/`ccmni_exit()` and
  `ccmni_md_state_callback()` on MD ready/exception/stop, and hands received packets
  to `ccmni_rx_callback()`. `ccmni_ccci_ops` (send_pkt/napi_poll/get_ccmni_ch) is filled
  by the core net port and passed into `ccmni_init()`.
- Core calls `ccci_util_log_init()` once per modem and `ccci_util_log()` for MD log
  records; util must never call back into core.
- Core creates the CLDMA engine with `ccci_cldma_create()` and routes RX skbs to the
  port layer through `ccci_cldma_set_rx_cb()`.
- C2K leaf only fills `struct ccci_md_cfg` for MD3 and calls `ccci_register_modem()`;
  it must not poke core internals.
- `skb` ownership: the callee consumes the skb on success and on error returns it to
  the caller (document which in the report).

## 7. Hardware facts (from downstream arm64 DTS, do not invent others)

`arch/arm64/boot/dts/mt6735.dtsi` node `mdcldma@1000A000`, compatible
`"mediatek,mdcldma"`:

- reg: `<0x1000A000 0x1000>` AP_CLDMA_AO, `<0x1000B000 0x1000>` MD_CLDMA_AO,
  `<0x1021A000 0x1000>` AP_CLDMA_PDN, `<0x1021B000 0x1000>` MD_CLDMA_PDN,
  `<0x1020A000 0x1000>` AP_CCIF, `<0x1020B000 0x1000>` MD_CCIF
- interrupts: `GIC_SPI 145 IRQ_TYPE_LEVEL_HIGH` (CLDMA), `GIC_SPI 140 IRQ_TYPE_LEVEL_LOW`
  (CCIF), `GIC_SPI 221 IRQ_TYPE_EDGE_FALLING` (MDWDT)
- `mediatek,md_id = <0>`, `mediatek,cldma_capability = <6>`,
  `mediatek,md_smem_size = <0x30000>`, `clocks = <&scpsys SCP_SYS_MD1>`,
  `clock-names = "scp-sys-md1-main"`
- CCIF register offsets (downstream `mt6735/ccif_c2k_platform.h`): `APCCIF_CON 0x00`,
  `APCCIF_BUSY 0x04`, `APCCIF_START 0x08`, `APCCIF_TCHNUM 0x0C`,
  `APCCIF_RCHNUM 0x10`, `APCCIF_ACK 0x14`, `APCCIF_CHDATA 0x100`; SRAM 512 bytes.
- MD1 boot: `MD_BOOT_VECTOR 0x20190000`, `MD_BOOT_VECTOR_KEY 0x2019379C`,
  `MD_BOOT_VECTOR_EN 0x20195488`, `MD_RGU_BASE 0x20050000`
  (`mt6735/modem_reg_base.h`). MD3 equivalents: `MD2_BOOT_VECTOR 0x30190000`,
  `MD2_BOOT_VECTOR_KEY 0x3019379C`, `MD2_BOOT_VECTOR_EN 0x30195488`, `MD2_RGU_BASE 0x30050000`.
- Put the DT fragment in your report under a `DTS-FRAGMENT` heading; do not edit DTS.

## 8. Deliverables per leaf

1. Working driver code (no stubs, no `TODO: implement`, no fake probe/success paths,
   no invented register values, no embedded firmware). Modem firmware is external:
   load it with `request_firmware()` and document the missing blob (`md1img`/`md3img`).
2. `$M/patches/<name>.patch`:
   `cd "$M/worktrees/<wt>" && git add -A && git diff --cached --binary > "$M/patches/<name>.patch"`
   (staging is required so new files appear in the diff; do not commit).
3. `$M/logs/<name>.md` with: files changed; ROM evidence (symbols/strings/addresses);
   downstream references (file:line); parity matrix (behaviour | ROM/downstream | Nura
   before | after | status done/partial/deferred); exact compile command + result;
   what is inferred or unverified on hardware; `DTS-FRAGMENT`; patch path.
4. Only your own files modified (`git status --porcelain` in your worktree must list
   only your files, `include/linux/ccci.h` and the shared Kconfig/Makefile hooks).

## 9. Integration (coordinator)

After all leaves yield, the coordinator applies the five patches to
`$M/worktrees/ccci-integration`, resolves Kconfig/Makefile/header duplicates, adds the
final Kconfig dependencies (`MTK_CCMNI`, `MTK_CCCI_C2K` depend on `MTK_CCCI`;
`MTK_CCCI` selects `MTK_CCCI_UTIL` and `MTK_CCCI_CLDMA`), compiles every object and
writes `$M/logs/ccci-coordinator.md`.
