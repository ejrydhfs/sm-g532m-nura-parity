/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6735 video codec shared hardware service core.
 *
 * The MT6735 VENC/VDEC engines are register-programmed by the vendor
 * userspace codec libraries; the kernel side owns power, clocks, the
 * engine interrupts and the hardware ownership lock.  This header is the
 * contract between the shared core (vcodec-core.c) and the two engine
 * platform drivers (vcodec-vdec.c, vcodec-venc.c).
 *
 * Port from Samsung/MediaTek downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c
 */

#ifndef _MTK_VCODEC_MT6735_H_
#define _MTK_VCODEC_MT6735_H_

#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

/* Register windows a single engine may expose through mmap(2). */
#define VCODEC_MAX_MMAP_REGIONS		4

/*
 * Values of the driver-type argument of the ioctl ABI.  The full enum
 * (all codecs) lives in include/uapi/linux/vcodec.h; the engines only need
 * the two encoder types they have to recognise in their interrupt path.
 */
#define VCODEC_DRIVER_TYPE_H264_ENC	5
#define VCODEC_DRIVER_TYPE_HEVC_ENC	28

enum vcodec_engine_id {
	VCODEC_ENGINE_DEC = 0,
	VCODEC_ENGINE_ENC,
	VCODEC_ENGINE_MAX,
};

struct vcodec_engine;

/**
 * struct vcodec_engine_ops - hardware operations of one codec engine
 * @power_on: enable the engine clock tree and power domain.  Called with
 *            the core engine mutex held, in process context.
 * @power_off: undo @power_on.
 * @enable_irq: unmask the engine interrupt.
 * @disable_irq: mask the engine interrupt and wait for a running handler.
 * @dvfs_begin: optional.  Start (or restart) the DVFS load monitor.  Called
 *              from the INC_DEC_EMI_USER ioctl once the user count is
 *              published, which is when downstream raises the decoder to
 *              its default level and arms the monitor.
 * @dvfs_monitor_start: optional.  Called after a successful LOCKHW, i.e.
 *              once the engine is powered and (unless the instance is
 *              secure) its interrupt is unmasked.
 * @dvfs_adjustment: optional.  Called from UNLOCKHW after the engine has
 *              been powered off; closes the current measurement window and
 *              re-evaluates the operating point.
 * @dvfs_idle: optional.  Called for the last close of /dev/Vcodec, after
 *              the engine has been forced idle.
 *
 * The first four callbacks are mandatory.  They must be idempotent-safe in
 * the sense that the core only calls them on a state transition (see the
 * core's ->powered/->irq_enabled bookkeeping).
 *
 * The @dvfs_* callbacks are all invoked in process context with the core
 * engine mutex (@eng->lock) already held.  They must not take @eng->lock
 * again; state that needs serialisation belongs behind a driver-private
 * mutex.  They must also be safe to call when the engine has no DVFS
 * capability at all, in which case they simply do nothing.
 */
struct vcodec_engine_ops {
	int (*power_on)(struct vcodec_engine *eng);
	void (*power_off)(struct vcodec_engine *eng);
	int (*enable_irq)(struct vcodec_engine *eng);
	void (*disable_irq)(struct vcodec_engine *eng);
	void (*dvfs_begin)(struct vcodec_engine *eng);
	void (*dvfs_monitor_start)(struct vcodec_engine *eng);
	void (*dvfs_adjustment)(struct vcodec_engine *eng);
	void (*dvfs_idle)(struct vcodec_engine *eng);
};

/**
 * struct vcodec_mmap_region - physical register window exposed to userspace
 * @phys: physical base address
 * @size: window size in bytes
 */
struct vcodec_mmap_region {
	phys_addr_t phys;
	resource_size_t size;
};

/**
 * struct vcodec_engine - one MT6735 codec engine
 *
 * The fields above the "core private" marker are filled in by the engine
 * platform driver before vcodec_engine_register().  Everything below the
 * marker is owned by vcodec-core.c.
 */
struct vcodec_engine {
	const char *name;
	enum vcodec_engine_id id;
	struct device *dev;
	const struct vcodec_engine_ops *ops;
	void *priv;

	struct vcodec_mmap_region regions[VCODEC_MAX_MMAP_REGIONS];
	unsigned int num_regions;

	/* core private */
	struct list_head node;
	/* protects the fields below and serialises engine power/lock state */
	struct mutex lock;
	wait_queue_head_t hwlock_wq;
	wait_queue_head_t isr_wq;
	/* protects the interrupt completion counters below (hardirq) */
	spinlock_t irq_lock;
	bool registered;
	bool hw_locked;
	bool powered;
	bool irq_enabled;
	phys_addr_t owner;
	u32 driver_type;
	unsigned long isr_seq;
	u32 irq_status;
};

int vcodec_engine_register(struct vcodec_engine *eng);
void vcodec_engine_unregister(struct vcodec_engine *eng);

/**
 * vcodec_engine_irq() - report a completed hardware job
 * @eng: engine that raised the interrupt
 * @status: raw interrupt status word captured by the engine ISR
 *
 * Called from the engine hardirq handler after it has validated and
 * acknowledged the hardware interrupt.  Wakes VCODEC_WAITISR waiters.
 */
void vcodec_engine_irq(struct vcodec_engine *eng, u32 status);

bool vcodec_engine_is_locked(struct vcodec_engine *eng);
u32 vcodec_engine_get_driver_type(struct vcodec_engine *eng);

/* Implemented by vcodec-vdec.c / vcodec-venc.c, called by the core. */
int vcodec_vdec_register(void);
void vcodec_vdec_unregister(void);
int vcodec_venc_register(void);
void vcodec_venc_unregister(void);

#endif /* _MTK_VCODEC_MT6735_H_ */
