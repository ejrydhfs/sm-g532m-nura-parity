/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * MediaTek AEE/AED exception ("DB") client ABI.
 *
 * The values, message layout and ioctl numbers are copied from the
 * Samsung/MTK 3.18 downstream GPL tree that matches the SM-G532M ROM:
 *
 *   drivers/misc/mediatek/aee/aed/aed.h:39-167
 *     - AE_INVALID/AE_NOT_AVAILABLE/AE_DEFAULT : :39-41
 *     - enum AEE_MODE                          : :43-49
 *     - enum AE_PASS_METHOD                    : :58-61
 *     - enum AE_CMD_TYPE                       : :63
 *     - enum AE_CMD_ID                         : :65-108
 *     - struct AE_Msg                          : :110-125
 *     - struct aee_ioctl                       : :141-148
 *     - AEEIOCTL_*                             : :151-161
 *   drivers/misc/mediatek/aee/aed/aed.h:147-149 (AEE_NR_FRAME)
 *   drivers/misc/mediatek/include/mt-plat/aee.h:65-107 (aee_bt_frame,
 *     aee_process_bt, aee_thread_reg)
 *
 * Original copyright: Copyright (C) 2015 MediaTek Inc., GPL-2.0.
 *
 * The client library shipped in the ROM (/lib/libaed.so, ELF32 ARM) talks
 * to the AED daemon over the abstract socket "com.mtk.aee.aed" and uses
 * exactly one ioctl on /dev/aed0: AEEIOCTL_GET_THREAD_REG, requested with
 * both the 296-byte (64-bit pt_regs) and the 76-byte (32-bit pt_regs)
 * command encoding (libaed.so: aee_ioctl_get_reg, file offset 0x4674).
 */
#ifndef _UAPI_LINUX_AEE_H
#define _UAPI_LINUX_AEE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t __s32;
#endif

/* aed.h:39-41 */
#define AE_INVALID		0xAEEFF000
#define AE_NOT_AVAILABLE	0xAEE00000
#define AE_DEFAULT		0xAEE00001

/* aed.h:43-49 */
enum AEE_MODE {
	AEE_MODE_MTK_ENG = 1,
	AEE_MODE_MTK_USER,
	AEE_MODE_CUSTOMER_ENG,
	AEE_MODE_CUSTOMER_USER,
	AEE_MODE_NOT_INIT
};

/* aed.h:58-61 */
enum AE_PASS_METHOD {
	AE_PASS_BY_MEM,
	AE_PASS_BY_FILE,
	AE_PASS_METHOD_END
};

/* aed.h:63 */
enum AE_CMD_TYPE { AE_REQ, AE_RSP, AE_IND, AE_CMD_TYPE_END };

/*
 * aed.h:65-108.  The numeric values are the ABI; keep the downstream names
 * and order so the encoder used by the AED daemon/libaed stays compatible.
 */
enum AE_CMD_ID {
	AE_REQ_IDX,

	AE_REQ_CLASS,
	AE_REQ_TYPE,
	AE_REQ_PROCESS,
	AE_REQ_MODULE,
	AE_REQ_BACKTRACE,
	AE_REQ_DETAIL,		/* Content of response message rule:
				 *   if msg.arg1==AE_PASS_BY_FILE => msg->data=file path
				 */

	AE_REQ_ROOT_LOG_DIR,
	AE_REQ_CURR_LOG_DIR,
	AE_REQ_DFLT_LOG_DIR,
	AE_REQ_MAIN_LOG_FILE_PATH,

	AE_IND_FATAL_RAISED,	/* fatal event raised, indicate AED to notify users */
	AE_IND_EXP_RAISED,	/* exception event raised, indicate AED to notify users */
	AE_IND_WRN_RAISED,	/* warning event raised, indicate AED to notify users */
	AE_IND_REM_RAISED,	/* reminding event raised, indicate AED to notify users */

	AE_IND_LOG_STATUS,	/* arg = AE_ERR */
	AE_IND_LOG_CLOSE,	/* arg = AE_ERR */

	AE_REQ_SWITCH_DAL_BEEP,	/* arg: dal on|off, seq: beep on|off */
	AE_REQ_DB_COUNT,	/* arg: db count */
	AE_REQ_DB_FORCE_PATH,	/* arg: force db path yes\no */
	AE_REQ_SWITCH_EXP_LEVEL,
	AE_REQ_IS_AED_READY,	/* query if AED is ready for service */
	AE_REQ_COREDUMP,	/* msg->data=file path */
	AE_REQ_SET_READFLAG,	/* set read flag msg */
	AE_REQ_E2S_INIT,	/* Init notification of client side(application layer) of Exp2Server */
	AE_REQ_USERSPACEBACKTRACE = 40,
	AE_REQ_USER_REG,
	AE_REQ_USER_MAPS,
	AE_CMD_ID_END
};

/* aed.h:110-125 (24 bytes on both ABIs) */
struct AE_Msg {
	__u32 cmdType;		/* enum AE_CMD_TYPE */
	__u32 cmdId;		/* enum AE_CMD_ID */
	union {
		__u32 seq;	/* sequence number for error checking */
		__s32 pid;	/* process id */
	};
	union {
		__u32 arg;	/* simple argument */
		__u32 cls;	/* enum AE_EXP_CLASS (int-plat/aee.h:33-53) */
	};
	union {
		__u32 len;	/* dynamic length argument */
		__s32 id;	/* desired id */
	};
	__u32 dbOption;		/* DB dump option, int-plat/aee.h:186-218 */
};

/* aed.h:130-140 -- kernel ioctl argument, aed.h:141-148 */
struct aee_ioctl {
	__u32 pid;
	__u32 detail;
	__u32 size;
	__u32 pad;
	__u64 in;
	__u64 out;
};

/*
 * aed.h:87-90 (32-bit kernel ABI: pid_t is 4 bytes) and
 * include/mt-plat/aee.h:65-85 (struct aee_bt_frame is 256 bytes: two u64,
 * five u32 of padding and two symbol strings).
 */
#define AEE_NR_FRAME		32
#define AEE_SZ_SYMBOL_S		80
#define AEE_SZ_SYMBOL_L		140

struct aee_bt_frame {
	__u64 pc;
	__u64 lr;
	__u32 pad[5];
	char pc_symbol[AEE_SZ_SYMBOL_S];
	char lr_symbol[AEE_SZ_SYMBOL_L];
};

/*
 * include/mt-plat/aee.h:87-90: pid_t tid + struct pt_regs.  The client library
 * requests the 32-bit view as a 76-byte payload (_IOW('p', 0x07, 76), see
 * libaed.so aee_ioctl_get_reg) and the 64-bit view as a 296-byte payload
 * (_IOW('p', 0x07, 296); 4-byte tid + 4 bytes padding + a 288-byte 3.18/4.x
 * arm64 pt_regs: 34 u64 of user_pt_regs, orig_x0, syscallno).
 */
struct aee_thread_reg {
	__s32 tid;
	__u32 regs[18];		/* arm32 struct pt_regs: uregs[0..17] */
};

struct aee_thread_reg64 {
	__s32 tid;
	__u32 pad;
	__u64 regs[36];
};

/*
 * aed.h:151-161, expanded with the uapi struct sizes above.  The literal
 * values are asserted against _IOW()/_IOR() in the kernel driver
 * (drivers/misc/mediatek/aee/aed-main.c) because this header must not
 * depend on <linux/ioctl.h> when it is compiled for the host tool.
 */
#define AEEIOCTL_DAL_SHOW		0x44007001	/* _IOW('p', 0x01, struct aee_dal_show[1024]) */
#define AEEIOCTL_DAL_CLEAN		0x00007002	/* _IO('p', 0x02) */
#define AEEIOCTL_SETCOLOR		0x400c7003	/* _IOW('p', 0x03, struct aee_dal_setcolor[12]) */
#define AEEIOCTL_GET_PROCESS_BT		0x40207004	/* _IOW('p', 0x04, struct aee_ioctl[32]) */
#define AEEIOCTL_GET_SMP_INFO		0x80047005	/* _IOR('p', 0x05, int) */
#define AEEIOCTL_SET_AEE_MODE		0x80047006	/* _IOR('p', 0x06, int) */
#define AEEIOCTL_GET_THREAD_REG		0x404c7007	/* _IOW('p', 0x07, struct aee_thread_reg[76]) */
#define AEEIOCTL_GET_THREAD_REG64	0x41287007	/* _IOW('p', 0x07, struct aee_thread_reg64[296]) */
#define AEEIOCTL_CHECK_SUID_DUMPABLE	0x80047008	/* _IOR('p', 0x08, int) */
#define AEEIOCTL_WDT_KICK_POWERKEY	0x80047009	/* _IOR('p', 0x09, int) */
#define AEEIOCTL_RT_MON_Kick		0x8004700a	/* _IOR('p', 0x0A, int) */
#define AEEIOCTL_SET_FORECE_RED_SCREEN	0x8004700b	/* _IOR('p', 0x0B, int) */
#define AEEIOCTL_SET_SF_STATE		0x8008700c	/* _IOR('p', 0x0C, long long[8]) */
#define AEEIOCTL_GET_SF_STATE		0x4008700d	/* _IOW('p', 0x0D, long long[8]) */
#define AEEIOCTL_USER_IOCTL_TO_KERNEL_WANING 0x8004700e	/* _IOR('p', 0x0E, int) */

#endif /* _UAPI_LINUX_AEE_H */
