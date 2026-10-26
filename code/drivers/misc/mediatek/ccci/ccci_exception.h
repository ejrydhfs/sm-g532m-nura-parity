/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CCCI exception / AEE dump engine: private state and the modem exception
 * record wire format.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/eccci/ccci_core.h        (EX_HEADER_T, EX_ENVINFO_T,
 *                                                   EX_LOG_T, MD_EX_TYPE_*)
 *   drivers/misc/mediatek/eccci/port_kernel.h      (CCCI_AED_DUMP_*,
 *                                                   AED_STR_LEN,
 *                                                   CCCI_EXREC_OFFSET_OFFENDER)
 *   drivers/misc/mediatek/eccci/port_kernel.c      (ccci_aed, EE dump flow)
 *
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#ifndef __CCCI_EXCEPTION_H__
#define __CCCI_EXCEPTION_H__

#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct ccci_md;

/* ------------------------------------------------------------------ */
/* Modem exception log, downstream eccci/ccci_core.h:60-135            */
/* ------------------------------------------------------------------ */

/* MODEM MAUI Exception header (4 bytes) */
struct ccci_ex_header {
	u8 ex_type;
	u8 ex_nvram;
	__le16 ex_serial_num;
} __packed;

/* MODEM MAUI Environment information (164 bytes) */
struct ccci_ex_env_info {
	u8 boot_mode;		/* offset: +0x10 */
	u8 reserved1[8];
	u8 execution_unit[8];
	u8 status;		/* offset: +0x21 */
	u8 elm_status;		/* offset: +0x22 */
	u8 reserved2[145];
} __packed;

/* MODEM MAUI fatal error code (8 bytes) */
struct ccci_ex_fatalerr_code {
	__le32 code1;
	__le32 code2;
} __packed;

/* MODEM MAUI fatal error (296 bytes) */
struct ccci_ex_fatalerr {
	struct ccci_ex_fatalerr_code error_code;
	u8 reserved1[288];
} __packed;

/* MODEM MAUI assert fail (296 bytes) */
struct ccci_ex_assert_fail {
	u8 filename[24];
	__le32 linenumber;
	__le32 parameters[3];
	u8 reserved1[256];
} __packed;

/* MODEM MAUI globally exported data structure (296 bytes) */
union ccci_ex_content {
	struct ccci_ex_fatalerr fatalerr;
	struct ccci_ex_assert_fail assert;
} __packed;

/* MODEM MAUI standard exception log (512 bytes) */
struct ccci_ex_log {
	struct ccci_ex_header header;
	u8 reserved1[12];
	struct ccci_ex_env_info envinfo;
	u8 reserved2[36];
	union ccci_ex_content content;
} __packed;

/* downstream port_kernel.h:29: fatal-error "offender" string in the record */
#define CCCI_EXREC_OFFSET_OFFENDER	288
/* downstream port_kernel.c:3160-3168: a 0xCC lead byte means "no offender" */
#define CCCI_EX_OFFENDER_NONE		0xCC

/*
 * EX_LOG_T.ex_type values.
 *
 * port_kernel.h:31-54 (the port_kernel.c:3152-3250 decoder uses exactly these)
 * plus EMI_MPU_VIOLATION from ccci_core.h:246 (0x30).  The latter lives in the
 * platform exception list because it is raised by the RMPU/EMI MPU, not by the
 * modem software; the non-MD_UMOLY decoder (port_kernel.c:2899-3010) has no
 * dedicated case for it and reports it as an unknown type, the
 * MD_UMOLY_EE_SUPPORT decoder (port_kernel.c:2752-2765) names it
 * "Fatal error (rmpu violation)".  This kernel has no MD_UMOLY build, so the
 * record layout is EX_LOG_T and the type is decoded here.
 */
#define CCCI_EX_TYPE_INVALID		0
#define CCCI_EX_TYPE_UNDEF		1
#define CCCI_EX_TYPE_SWI		2
#define CCCI_EX_TYPE_PREF_ABT		3
#define CCCI_EX_TYPE_DATA_ABT		4
#define CCCI_EX_TYPE_ASSERT		5
#define CCCI_EX_TYPE_FATALERR_TASK	6
#define CCCI_EX_TYPE_FATALERR_BUF	7
#define CCCI_EX_TYPE_LOCKUP		8
#define CCCI_EX_TYPE_ASSERT_DUMP	9
#define CCCI_EX_TYPE_ASSERT_FAIL	10
#define CCCI_EX_TYPE_DSP_ASSERT		11
#define CCCI_EX_TYPE_DSP_EXCEPTION	12
#define CCCI_EX_TYPE_DSP_FATAL_ERROR	13
#define CCCI_EX_TYPE_CROSS_CORE		15
#define CCCI_EX_TYPE_C2K_ERROR		0x25
#define CCCI_EX_TYPE_EMI_MPU_VIOLATION	0x30	/* ccci_core.h:246 */
#define CCCI_EX_TYPE_EMI_CHECK		99
/*
 * Not an ex_type: downstream port_kernel.h:58 MD_EX_C2K_FATAL_ERROR is the
 * err_code1 value that marks a C2K fatal error inside a C2K_ERROR record
 * (port_kernel.c:3100).
 */
#define CCCI_EX_C2K_FATAL_ERR_CODE1	0x3000

/* ------------------------------------------------------------------ */
/* AED dump selectors, downstream eccci/port_kernel.h:19-22            */
/* ------------------------------------------------------------------ */
#define CCCI_AED_DUMP_EX_MEM		BIT(0)
#define CCCI_AED_DUMP_MD_IMG_MEM	BIT(1)
#define CCCI_AED_DUMP_CCIF_REG		BIT(2)
#define CCCI_AED_DUMP_EX_PKT		BIT(3)

/* ------------------------------------------------------------------ */
/* Dump engine limits and state                                        */
/* ------------------------------------------------------------------ */
/* ccci_header + the full exception log, as received on MD_EX_REC_OK */
#define CCCI_EX_PACKET_MAX	(16 + sizeof(struct ccci_ex_log))
/* text snapshot: header, hex of every region and the decoded record */
#define CCCI_EX_DUMP_MAX	32768
/* every region is also printed to the kernel log, bounded per region */
#define CCCI_EX_KLOG_BYTES	512

/* One entry of the dump, used by the engine internally */
enum ccci_ex_dump_reason {
	CCCI_EX_DUMP_MD_EX = 0,
	CCCI_EX_DUMP_MD_EX_REC_OK,
	CCCI_EX_DUMP_WDT,
	CCCI_EX_DUMP_NO_RESPONSE,
};

/*
 * Exception shared-memory window (downstream ccci_core.c:262-293):
 *
 *   exp_vir/exp_size               CCCI_SMEM_SIZE_EXCEPTION bytes at the base
 *                                  of the reserved AP<->MD shared memory
 *   ccci_debug_vir/ccci_debug_size record handed to AEE at
 *                                  exp_vir + CCCI_SMEM_OFFSET_CCCI_DEBUG
 *
 * Downstream derives these from the smem region returned by the CCCI
 * "smem_layout" tag (ccci_util_lib_fo.c:468-511) plus the MT6735 layout
 * constants (mt6735/ccci_config.h:71-83).  The modem image CHECK_HEADER does
 * not carry the exception window: v3/v4/v5 have no smem field at all and v6
 * only adds the total AP<->MD smem size (ccci_util_lib_main.h:198-201), which
 * downstream never uses for the exception window.
 */
struct ccci_exception {
	/* packet capture and dump scheduling, taken from Rx/IRQ contexts */
	spinlock_t lock;
	bool ready;
	bool pending;
	bool worker_active;
	unsigned int pending_reason;
	unsigned int pending_flags;

	void __iomem *exp_vir;
	size_t exp_size;
	void __iomem *ccci_debug_vir;
	size_t ccci_debug_size;
	bool window_resolved;

	u8 packet[CCCI_EX_PACKET_MAX];
	size_t packet_len;
	bool packet_valid;

	struct work_struct work;

	/* text snapshot handed to debugfs and in-kernel consumers */
	struct mutex dump_lock;
	char *dump;
	size_t dump_len;
	u32 dump_count;
	unsigned int dump_reason;
	unsigned int dump_flags;
	unsigned int dump_ex_stage;
	unsigned int dump_ex_type;

	struct dentry *dbg_dump;
	struct dentry *dbg_packet;
	struct dentry *dbg_stage;
};

/* ------------------------------------------------------------------ */
/* API (implemented in ccci_exception.c, called from ccci_core.c)      */
/* ------------------------------------------------------------------ */
int ccci_exception_init(struct ccci_md *md);
void ccci_exception_exit(struct ccci_md *md);

/* store the MD_EX_REC_OK packet (ccci_header + EX_LOG_T) for the dump */
void ccci_exception_capture_packet(struct ccci_md *md, const void *data,
				   size_t len);

/*
 * AED selection the ROM uses for an exception dump: the MD_EX_REC_OK packet
 * before the first boot handshake completes, the shared-memory window after
 * it (downstream ccci_ee_info_dump(), port_kernel.c:3125-3135).
 */
unsigned int ccci_exception_dump_flags(struct ccci_md *md);

/* queue one dump; @reason is enum ccci_ex_dump_reason, @flags CCCI_AED_DUMP_* */
void ccci_exception_trigger(struct ccci_md *md, unsigned int reason,
			    unsigned int flags);

/*
 * Same, but wait until the snapshot is built.  Only for process context and
 * only where the dump must precede a modem reset (the MD watchdog path,
 * downstream modem_cldma.c:1838-1897 dumps the RGU/smem before resetting).
 */
void ccci_exception_trigger_sync(struct ccci_md *md, unsigned int reason,
				 unsigned int flags);

/*
 * CCCI debug record inside the exception window, for the ROM AEE ABI
 * aee_dump_ccci_debug_info() (downstream ccci_core.c:580-599).  Returns 0 and
 * fills @vir/@size, or -ENODEV when the modem has no shared-memory window.
 */
int ccci_exception_ccci_debug_window(struct ccci_md *md, void __iomem **vir,
				     size_t *size);
/* CCCI_EX_* stage name, also used by the debugfs stage file */
const char *ccci_exception_stage_name(unsigned int stage);

#endif /* __CCCI_EXCEPTION_H__ */
