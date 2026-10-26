/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * MediaTek MT6735 video codec hardware service ABI.
 *
 * The vendor codec userspace libraries program the VENC/VDEC register
 * windows themselves; this ABI only covers the services the kernel does
 * provide: hardware ownership (lock/unlock), power/user accounting,
 * interrupt wait and the register-window mappings.
 *
 * The command numbers, their encodings and the driver-type enumeration
 * are ROM compatible and must never be renumbered.  Port from
 * Samsung/MediaTek downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver.h:19-52
 * and drivers/misc/mediatek/videocodec/include/val_types_public.h:135-170.
 */

#ifndef _UAPI_LINUX_VCODEC_H
#define _UAPI_LINUX_VCODEC_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MFV_IOC_MAGIC		'M'

/* Control messages; payload pointers are passed as 32-bit integers. */
#define MFV_TEST_CMD			_IO(MFV_IOC_MAGIC, 0x00)
#define MFV_INIT_CMD			_IO(MFV_IOC_MAGIC, 0x01)
#define MFV_DEINIT_CMD			_IO(MFV_IOC_MAGIC, 0x02)
#define MFV_SET_CMD_CMD			_IOW(MFV_IOC_MAGIC, 0x03, __u32)
#define MFV_SET_PWR_CMD			_IOW(MFV_IOC_MAGIC, 0x04, __u32)
#define MFV_SET_ISR_CMD			_IOW(MFV_IOC_MAGIC, 0x05, __u32)
#define MFV_ALLOC_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x06, __u32)
#define MFV_FREE_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x07, __u32)
#define MFV_MAKE_PMEM_TO_NONCACHED	_IOW(MFV_IOC_MAGIC, 0x08, __u32)
#define MFV_ALLOC_INT_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x09, __u32)
#define MFV_FREE_INT_MEM_CMD		_IOW(MFV_IOC_MAGIC, 0x0a, __u32)
#define VCODEC_WAITISR			_IOW(MFV_IOC_MAGIC, 0x0b, __u32)
#define VCODEC_LOCKHW			_IOW(MFV_IOC_MAGIC, 0x0d, __u32)
#define VCODEC_PMEM_FLUSH		_IOW(MFV_IOC_MAGIC, 0x10, __u32)
#define VCODEC_PMEM_CLEAN		_IOW(MFV_IOC_MAGIC, 0x11, __u32)
#define VCODEC_INC_SYSRAM_USER		_IOW(MFV_IOC_MAGIC, 0x13, __u32)
#define VCODEC_DEC_SYSRAM_USER		_IOW(MFV_IOC_MAGIC, 0x14, __u32)
#define VCODEC_INC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x15, __u32)
#define VCODEC_DEC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x16, __u32)
#define VCODEC_INC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x17, __u32)
#define VCODEC_DEC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x18, __u32)
#define VCODEC_INITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x20, __u32)
#define VCODEC_DEINITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x21, __u32)
#define VCODEC_ALLOC_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x22, __u32)
#define VCODEC_FREE_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x23, __u32)
#define VCODEC_SET_THREAD_ID		_IOW(MFV_IOC_MAGIC, 0x24, __u32)
#define VCODEC_SET_SYSRAM_INFO		_IOW(MFV_IOC_MAGIC, 0x25, __u32)
#define VCODEC_GET_SYSRAM_INFO		_IOW(MFV_IOC_MAGIC, 0x26, __u32)
#define VCODEC_INC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x27, __u32)
#define VCODEC_DEC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x28, __u32)
#define VCODEC_GET_CPU_LOADING_INFO	_IOW(MFV_IOC_MAGIC, 0x29, __u32)
#define VCODEC_GET_CORE_LOADING		_IOW(MFV_IOC_MAGIC, 0x30, __u32)
#define VCODEC_GET_CORE_NUMBER		_IOW(MFV_IOC_MAGIC, 0x31, __u32)
#define VCODEC_SET_CPU_OPP_LIMIT	_IOW(MFV_IOC_MAGIC, 0x32, __u32)
#define VCODEC_UNLOCKHW			_IOW(MFV_IOC_MAGIC, 0x33, __u32)
#define VCODEC_MB			_IOW(MFV_IOC_MAGIC, 0x34, __u32)

/*
 * Driver types.  Order and values are the downstream ones, see
 * VAL_DRIVER_TYPE_T; the vendor libraries pass these numbers verbatim.
 */
enum vcodec_driver_type {
	VCODEC_DRV_TYPE_NONE = 0,
	VCODEC_DRV_TYPE_MP4_ENC,
	VCODEC_DRV_TYPE_MP4_DEC,
	VCODEC_DRV_TYPE_H263_ENC,
	VCODEC_DRV_TYPE_H263_DEC,
	VCODEC_DRV_TYPE_H264_ENC = 5,
	VCODEC_DRV_TYPE_H264_DEC = 6,
	VCODEC_DRV_TYPE_SORENSON_SPARK_DEC,
	VCODEC_DRV_TYPE_VC1_SP_DEC,
	VCODEC_DRV_TYPE_RV9_DEC,
	VCODEC_DRV_TYPE_MP1_MP2_DEC,
	VCODEC_DRV_TYPE_XVID_DEC,
	VCODEC_DRV_TYPE_DIVX4_DIVX5_DEC,
	VCODEC_DRV_TYPE_VC1_MP_WMV9_DEC,
	VCODEC_DRV_TYPE_RV8_DEC,
	VCODEC_DRV_TYPE_WMV7_DEC,
	VCODEC_DRV_TYPE_WMV8_DEC,
	VCODEC_DRV_TYPE_AVS_DEC,
	VCODEC_DRV_TYPE_DIVX_3_11_DEC,
	VCODEC_DRV_TYPE_H264_DEC_MAIN,
	VCODEC_DRV_TYPE_H264_DEC_MAIN_CABAC,
	VCODEC_DRV_TYPE_VP8_DEC,
	VCODEC_DRV_TYPE_MP2_DEC,
	VCODEC_DRV_TYPE_VP9_DEC,
	VCODEC_DRV_TYPE_VP8_ENC,
	VCODEC_DRV_TYPE_VC1_ADV_DEC,
	VCODEC_DRV_TYPE_VC1_DEC,
	VCODEC_DRV_TYPE_JPEG_ENC,
	VCODEC_DRV_TYPE_HEVC_ENC = 28,
	VCODEC_DRV_TYPE_HEVC_DEC,
	VCODEC_DRV_TYPE_H264_ENC_LIVEPHOTO,
	VCODEC_DRV_TYPE_MMDVFS,
	VCODEC_DRV_TYPE_MAX = 0xffffffff,
};

/* Maximum number of status words returned for one completed hardware job. */
#define VCODEC_IRQ_STATUS_MAX_NUM	16
/* Maximum number of cooperating threads an instance may register. */
#define VCODEC_THREAD_MAX_NUM		16

/**
 * struct vcodec_hw_lock - argument of VCODEC_LOCKHW / VCODEC_UNLOCKHW
 * @pv_handle: handle of the codec instance; identifies the lock owner
 * @handle_size: size of the handle structure
 * @pv_lock: lock descriptor, unused by the kernel
 * @timeout_ms: how long to wait for the engine, 0 means driver default
 * @pv_reserved: reserved, unused
 * @reserved_size: size of the reserved structure
 * @driver_type: one of &enum vcodec_driver_type, selects the engine
 * @secure_inst: secure instance, do not expose its interrupt to Linux
 */
struct vcodec_hw_lock {
	__u64 pv_handle;
	__u32 handle_size;
	__u64 pv_lock;
	__u32 timeout_ms;
	__u64 pv_reserved;
	__u32 reserved_size;
	__u32 driver_type;
	__u8 secure_inst;
	__u8 pad[7];
};

/**
 * struct vcodec_power - argument of VCODEC_INC_PWR_USER / VCODEC_DEC_PWR_USER
 * @pv_handle: handle of the codec instance
 * @handle_size: size of the handle structure
 * @driver_type: one of &enum vcodec_driver_type
 * @enable: power user count enable flag
 * @pv_reserved: reserved, unused
 * @reserved_size: size of the reserved structure
 */
struct vcodec_power {
	__u64 pv_handle;
	__u32 handle_size;
	__u32 driver_type;
	__u8 enable;
	__u8 pad[7];
	__u64 pv_reserved;
	__u32 reserved_size;
};

/**
 * struct vcodec_isr - argument of VCODEC_WAITISR
 * @pv_handle: handle of the codec instance, must currently own the engine
 * @handle_size: size of the handle structure
 * @driver_type: one of &enum vcodec_driver_type
 * @pv_isr_function: userspace ISR callback, unused by the kernel
 * @pv_reserved: reserved, unused
 * @reserved_size: size of the reserved structure
 * @timeout_ms: how long to wait for the interrupt, 0 fails immediately
 * @irq_status_num: number of status words the caller can accept
 * @irq_status: status words read from the hardware by the kernel ISR
 */
struct vcodec_isr {
	__u64 pv_handle;
	__u32 handle_size;
	__u32 driver_type;
	__u64 pv_isr_function;
	__u64 pv_reserved;
	__u32 reserved_size;
	__u32 timeout_ms;
	__u32 irq_status_num;
	__u32 irq_status[VCODEC_IRQ_STATUS_MAX_NUM];
};

/**
 * struct vcodec_core_loading - argument of VCODEC_GET_CORE_LOADING
 * @cpu_id: CPU to query, 0 .. nr_cpu_ids - 1
 * @loading: CPU load in percent since the previous query
 */
struct vcodec_core_loading {
	__s32 cpu_id;
	__s32 loading;
};

/**
 * struct vcodec_cpu_opp_limit - argument of VCODEC_SET_CPU_OPP_LIMIT
 * @limited_freq: frequency ceiling in kHz
 * @limited_cpu: CPU mask the limit applies to
 * @enable: 0 disables the limit, 1 enables it
 */
struct vcodec_cpu_opp_limit {
	__s32 limited_freq;
	__s32 limited_cpu;
	__s32 enable;
};

/**
 * struct vcodec_cpu_loading_info - argument of VCODEC_GET_CPU_LOADING_INFO
 * @cpu_idle_time: unused, always 0
 * @thread_cpu_time: unused, always 0
 * @sched_clock: value of the kernel scheduler clock
 * @inst_count: number of engines currently owned by userspace
 */
struct vcodec_cpu_loading_info {
	__u64 cpu_idle_time;
	__u64 thread_cpu_time;
	__u64 sched_clock;
	__u32 inst_count;
	__u32 pad;
};

/**
 * struct vcodec_thread_id - argument of VCODEC_SET_THREAD_ID
 * @tid1: first thread id
 * @tid2: second thread id
 * @thread_num: number of valid entries in @thread_id
 * @thread_id: cooperating thread ids
 */
struct vcodec_thread_id {
	__u32 tid1;
	__u32 tid2;
	__u32 thread_num;
	__u32 thread_id[VCODEC_THREAD_MAX_NUM];
};

#endif /* _UAPI_LINUX_VCODEC_H */
