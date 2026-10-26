/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek AEE (Android Exception Engine) kernel API.
 *
 * Coordinator-owned frozen interface for the rem2 gap-closure work.  The
 * values and prototypes are copied from the Samsung/MTK downstream GPL tree
 * that matches the SM-G532M ROM:
 *
 *   drivers/misc/mediatek/include/mt-plat/aee.h
 *     - AE_DEFECT_ATTR   : aee.h:25-31
 *     - AE_EXP_CLASS     : aee.h:33-53
 *     - DB_OPT_*         : aee.h:186-218
 *     - aed_*_exception_api(): aee.h:246-253
 *
 * The same symbols exist in the ROM kernel:
 *   aed_md_exception_api     0xc0628298
 *   aed_md32_exception_api   0xc0628354
 *   aed_scp_exception_api    0xc0628410
 *   aed_combo_exception_api  0xc06284cc
 *   aed_ee_open              0xc062a328  (/dev/aed0, "ee")
 *   aed_ke_open              0xc062a5a0  (/dev/aed1, "ke")
 *   aed_ioctl                0xc062a614
 *   aed_proc_debug_init      0xc062e4a8
 * (addresses from `llvm-nm $M/zip/vmlinux.elf`).
 *
 * The implementation is delivered by the AEE gap-closure leaf.  Callers that
 * only need to report an external (modem/DSP/SCP) exception include this
 * header; aee_dump_ccci_debug_info() stays declared in <linux/ccci.h>.
 */
#ifndef __LINUX_AEE_H
#define __LINUX_AEE_H

#include <linux/kconfig.h>
#include <linux/types.h>

#define AEE_MODULE_NAME_LENGTH		64
#define AEE_PROCESS_NAME_LENGTH		256
#define AEE_BACKTRACE_LENGTH		3072

/* aee.h:25-31 */
enum ae_defect_attr {
	AE_DEFECT_FATAL,
	AE_DEFECT_EXCEPTION,
	AE_DEFECT_WARNING,
	AE_DEFECT_REMINDING,
	AE_DEFECT_ATTR_END
};

/* aee.h:33-53 */
enum ae_exp_class {
	AE_KE = 0,		/* Fatal Exception */
	AE_HWT,
	AE_REBOOT,
	AE_NE,
	AE_JE,
	AE_SWT,
	AE_EE,
	AE_EXP_ERR_END,
	AE_ANR,			/* Error or Warning or Defect */
	AE_RESMON,
	AE_MODEM_WARNING,
	AE_WRN_ERR_END,
	AE_MANUAL,		/* Manual Raise */
	AE_EXP_CLASS_END,

	AE_KERNEL_PROBLEM_REPORT = 1000,
	AE_SYSTEM_JAVA_DEFECT,
	AE_SYSTEM_NATIVE_DEFECT,
};

/* aee.h:186-218 -- dump ("DB") options passed to the AED client. */
#define DB_OPT_DEFAULT			(0)
#define DB_OPT_FTRACE			BIT(0)
#define DB_OPT_PRINTK_TOO_MUCH		BIT(1)
#define DB_OPT_NE_JBT_TRACES		BIT(2)
#define DB_OPT_SWT_JBT_TRACES		BIT(3)
#define DB_OPT_VM_TRACES		BIT(4)
#define DB_OPT_DUMPSYS_ACTIVITY		BIT(5)
#define DB_OPT_DUMPSYS_WINDOW		BIT(6)
#define DB_OPT_DUMPSYS_GFXINFO		BIT(7)
#define DB_OPT_DUMPSYS_SURFACEFLINGER	BIT(8)
#define DB_OPT_DISPLAY_HANG_DUMP	BIT(9)
#define DB_OPT_LOW_MEMORY_KILLER	BIT(10)
#define DB_OPT_PROC_MEM			BIT(11)
#define DB_OPT_FS_IO_LOG		BIT(12)
#define DB_OPT_PROCESS_COREDUMP		BIT(13)
#define DB_OPT_VM_HPROF			BIT(14)
#define DB_OPT_PROCMEM			BIT(15)
#define DB_OPT_DUMPSYS_INPUT		BIT(16)
#define DB_OPT_MMPROFILE_BUFFER		BIT(17)
#define DB_OPT_BINDER_INFO		BIT(18)
#define DB_OPT_WCN_ISSUE_INFO		BIT(19)
#define DB_OPT_DUMMY_DUMP		BIT(20)
#define DB_OPT_PID_MEMORY_INFO		BIT(21)
#define DB_OPT_VM_OOME_HPROF		BIT(22)
#define DB_OPT_PID_SMAPS		BIT(23)
#define DB_OPT_PROC_CMDQ_INFO		BIT(24)
#define DB_OPT_PROC_USKTRK		BIT(25)
#define DB_OPT_SF_RTT_DUMP		BIT(26)
#define DB_OPT_PAGETYPE_INFO		BIT(27)
#define DB_OPT_DUMPSYS_PROCSTATS	BIT(28)
#define DB_OPT_DUMP_DISPLAY		BIT(29)
#define DB_OPT_NATIVE_BACKTRACE		BIT(30)
#define DB_OPT_AARCH64			BIT(31)

#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)
/*
 * External processors hand their exception log through these entry points
 * (aee.h:246-253).  log/phy are kernel virtual/physical buffers that the AED
 * driver copies into the exception record; detail is a NUL-terminated text
 * header (the CCCI caller passes the "mdN:<module>" string built by
 * port_kernel.c:2290-2304).
 */
void aed_md_exception_api(const int *log, int log_size, const int *phy,
			  int phy_size, const char *detail, const int db_opt);
void aed_md32_exception_api(const int *log, int log_size, const int *phy,
			    int phy_size, const char *detail, const int db_opt);
void aed_scp_exception_api(const int *log, int log_size, const int *phy,
			   int phy_size, const char *detail, const int db_opt);
void aed_combo_exception_api(const int *log, int log_size, const int *phy,
			     int phy_size, const char *detail, const int db_opt);
#else /* !CONFIG_MTK_AEE_FEATURE */
/*
 * The AED client is compiled out (downstream guards the whole body the same
 * way, aee-common.c:224-300).  The exception still reaches the kernel log
 * through the caller; nothing is silently reported as dumped.
 */
static inline void aed_md_exception_api(const int *log, int log_size,
					const int *phy, int phy_size,
					const char *detail, const int db_opt)
{
}

static inline void aed_md32_exception_api(const int *log, int log_size,
					  const int *phy, int phy_size,
					  const char *detail, const int db_opt)
{
}

static inline void aed_scp_exception_api(const int *log, int log_size,
					 const int *phy, int phy_size,
					 const char *detail, const int db_opt)
{
}

static inline void aed_combo_exception_api(const int *log, int log_size,
					   const int *phy, int phy_size,
					   const char *detail, const int db_opt)
{
}
#endif /* CONFIG_MTK_AEE_FEATURE */

#endif /* __LINUX_AEE_H */
