// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735/MT6737T camera ISP character driver ("camera-isp").
 *
 * Port of the downstream GPL driver
 * drivers/misc/mediatek/cameraisp/src/mt6735/camera_isp_D1.c (and its header
 * inc/camera_isp_D1.h) to the upstream misc-device model.  The userspace
 * contract is include/uapi/linux/mtk_isp.h; this file implements it.
 *
 * What is implemented, and where it comes from:
 *
 *  - register window mapping and mmap() of the documented windows
 *    (camera_isp_D1.c:11263-11335, uapi MTK_ISP_MMAP_*);
 *  - the 64 KiB shared real-time buffer table mapped at page offset 16
 *    (RT_BUF_TBL_NPAGES, camera_isp_D1.c:11231-11261, 13042-13065);
 *  - ISP_READ_REGISTER / ISP_WRITE_REGISTER over the CAMINF address space
 *    (camera_isp_D1.c:3387-3455);
 *  - the hold-register write buffers, flushed at VD or EXPDONE
 *    (camera_isp_D1.c:3462-3736, 3805-3960, 3745-3800);
 *  - per-user interrupt status / wait / mark / clear with the v1 and v3
 *    flows and the user-key table (camera_isp_D1.c:7712-8640, 9748-10370);
 *  - the Pass-1 / Pass-1-D / CAMSV / CAMSV2 real-time DMA buffer rings with
 *    SOF-time base-address and RCNT_INC programming and DONE-time completion
 *    (camera_isp_D1.c:4115-6158, 6177-7084, 8680-9718);
 *  - the Pass-2 early-done buffer queue (camera_isp_D1.c:7084-7712);
 *  - reset, register scenario, burst queue number, DMA/int error counters,
 *    current SOF and debug-mask controls (camera_isp_D1.c:9748-10370).
 *
 * Completion is exclusively hardware-interrupt driven: a buffer is marked
 * filled from mtk_isp_irq_cam() / mtk_isp_irq_camsv_common() and nowhere
 * else.  There is no timer, no polled completion and no synthetic frame.
 *
 * Interrupt semantics, proven on the ROM image (zip/vmlinux.elf):
 *
 *  - ISP_Irq_CAM @0xc04ddce8 reads the seven status registers
 *    0x4c/0x54/0x5c/0x64/0x70/0x74/0x78 and never writes them (the whole
 *    0x2498-byte function contains seven matching ldr and no str), so they
 *    are read-to-clear.
 *  - It reads the four FBC registers 0xf0/0xf4/0xf8/0xfc, ORs in RCNT_INC
 *    (bit 11) and writes them back at 0xc04de558/0xc04de57c and
 *    0xc04deac8/0xc04deaec, each followed by dsb sy.
 *  - At SOF it reads the DMA base addresses 0x3300/0x3320/0x34d4/0x34f4,
 *    re-programs them when userspace queued a replacement, pulls RCNT_INC,
 *    and only then publishes the per-frame geometry.
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <asm/barrier.h>

#include <uapi/linux/mtk_isp.h>

#include "mtk-isp-regs.h"

/* ------------------------------------------------------------------ */
/* Limits (camera_isp_D1.c:700-800)                                    */
/* ------------------------------------------------------------------ */

#define MTK_ISP_IRQ_DOM_NUM		4	/* _IRQ_MAX */
#define MTK_ISP_IRQ_USERS		32	/* IRQ_USER_NUM_MAX */
#define MTK_ISP_IRQ_TYPES		ISP_IRQ_TYPE_AMOUNT
#define MTK_ISP_DMA_NUM			_rt_dma_max_
#define MTK_ISP_RT_BUF_SIZE		ISP_RT_BUF_SIZE	/* 16 */
#define MTK_ISP_RT_TBL_NPAGES		16	/* RT_BUF_TBL_NPAGES */
#define MTK_ISP_RT_TBL_ORDER		4	/* 16 pages */
#define MTK_ISP_HOLD_BUFS		6	/* ISP_BUF_WRITE_AMOUNT */
#define MTK_ISP_HOLD_BUF_SIZE		1024	/* ISP_BUF_SIZE_WRITE */
#define MTK_ISP_USERKEY_LEN		128	/* USERKEY_STR_LEN */
#define MTK_ISP_EISMETA_RINGSIZE	4
#define MTK_ISP_ED_RING			512	/* _MAX_SUPPORT_P2_FRAME_NUM_ */
#define MTK_ISP_ED_MGR			128	/* _MAX_SUPPORT_P2_PACKAGE_NUM_ */
#define MTK_ISP_ED_BURSTQ_MAX		4	/* _MAX_SUPPORT_P2_BURSTQ_NUM_ */
#define MTK_ISP_ED_WAIT_US		5000000
#define MTK_ISP_INVALID_FRM_CNT		0xffff
#define MTK_ISP_UNCERTAIN_MAGIC		0x40000000
#define MTK_ISP_DUMMY_MAGIC		0x20000000
#define MTK_ISP_SLOW_MOTION_FPS		100	/* SlowMotion */

/* Buffer states, union-free copies of ISP_RTBC_BUF_STATE_ENUM. */
#define MTK_ISP_BUF_EMPTY		ISP_RTBC_BUF_EMPTY
#define MTK_ISP_BUF_FILLED		ISP_RTBC_BUF_FILLED
#define MTK_ISP_BUF_LOCKED		ISP_RTBC_BUF_LOCKED

/* Hold buffer states (camera_isp_D1.c:786-791). */
enum mtk_isp_hold_state {
	MTK_ISP_HOLD_EMPTY = 0,
	MTK_ISP_HOLD_HOLD,
	MTK_ISP_HOLD_READY,
};

/* Interrupt domains (camera_isp_D1.c:894-903). */
enum mtk_isp_irq_dom {
	MTK_ISP_IRQ_CAM = 0,
	MTK_ISP_IRQ_CAM_D,
	MTK_ISP_IRQ_CAMSV,
	MTK_ISP_IRQ_CAMSV2,
};

/* Frame counters (camera_isp_D1.c:858-864). */
enum mtk_isp_channel {
	MTK_ISP_CH_P1 = 0,
	MTK_ISP_CH_P1_D,
	MTK_ISP_CH_CAMSV,
	MTK_ISP_CH_CAMSV_D,
	MTK_ISP_CH_MAX = 5,
};

struct mtk_isp_hold {
	u8 *data;
	size_t len;
	int state;
};

struct mtk_isp_userkey {
	char name[MTK_ISP_USERKEY_LEN];
	int key;
};

/* Per-ring RCNT_INC scheduling (FW_RCNT_CTRL, camera_isp_D1.c:4105-4112). */
struct mtk_isp_fw_rcnt {
	u32 inc[MTK_ISP_IRQ_DOM_NUM][MTK_ISP_RT_BUF_SIZE];
	u32 dma_idx[MTK_ISP_DMA_NUM];
	u32 rd_idx[MTK_ISP_IRQ_DOM_NUM];
	u32 cur_idx[MTK_ISP_IRQ_DOM_NUM];
	u32 load_base[MTK_ISP_IRQ_DOM_NUM];
};

/* SOF timestamp interpolation (T_STAMP_2_0, camera_isp_D1.c:1085-1100). */
struct mtk_isp_tstamp {
	s32 fps;
	u32 interval_us;
	u32 compensation_us;
	u64 t_ns;
	u32 fcnt;
};

struct mtk_isp_edbuf {
	u32 process_id;
	u32 caller_id;
	s32 dup_cq_idx;
	s32 state;
};

struct mtk_isp_edmgr {
	u32 process_id;
	u32 caller_id;
	s32 dup_cq_idx;
	s32 dequed_num;
};

struct mtk_isp_file {
	struct mtk_isp *isp;
	pid_t pid;
};

struct mtk_isp {
	struct device *dev;
	void __iomem *isp;		/* 0x15004000, 0x9000 */
	void __iomem *cam;		/* 0x15000000, 0x10000 */
	phys_addr_t isp_phys;
	phys_addr_t cam_phys;
	int irq_cam;
	int irq_camsv;
	int irq_camsv2;
	struct miscdevice misc;

	struct clk *clks[6];
	int clk_num;

	struct mutex power_lock;
	int open_count;

	spinlock_t rtbc_lock;		/* real-time buffer rings */
	spinlock_t reg_lock;
	spinlock_t irq_lock;
	spinlock_t hold_lock;
	spinlock_t userkey_lock;
	spinlock_t edbuf_lock;
	spinlock_t scen_lock;

	wait_queue_head_t waitq;
	wait_queue_head_t ed_wait_deque;
	wait_queue_head_t ed_wait_frame;

	struct wakeup_source *ws;
	bool ws_held;

	struct tasklet_struct vd_tasklet;
	struct tasklet_struct expdone_tasklet;

	u32 irq_mask[MTK_ISP_IRQ_TYPES];
	u32 irq_err_mask[MTK_ISP_IRQ_TYPES];
	u32 status[MTK_ISP_IRQ_USERS][MTK_ISP_IRQ_TYPES];
	u32 marked[MTK_ISP_IRQ_USERS][MTK_ISP_IRQ_TYPES];
	u32 marked_sec[MTK_ISP_IRQ_USERS][MTK_ISP_IRQ_TYPES][32];
	u32 marked_usec[MTK_ISP_IRQ_USERS][MTK_ISP_IRQ_TYPES][32];
	s32 passed_cnt[MTK_ISP_IRQ_USERS][MTK_ISP_IRQ_TYPES][32];
	u32 last_sec[MTK_ISP_IRQ_TYPES][32];
	u32 last_usec[MTK_ISP_IRQ_TYPES][32];
	ISP_EIS_META_STRUCT eismeta[ISP_IRQ_TYPE_INT_STATUSX]
					  [MTK_ISP_EISMETA_RINGSIZE];
	int eismeta_ridx;
	int eismeta_widx;
	int eismeta_in_sof;
	int eismeta_ridx_d;
	int eismeta_widx_d;
	int eismeta_in_sof_d;

	struct mtk_isp_userkey userkey[MTK_ISP_IRQ_USERS];
	int first_unused_key;
	u32 flush_v1[ISP_IRQ_USER_MAX];
	u32 flush_v3[MTK_ISP_IRQ_USERS];

	struct mtk_isp_hold hold[MTK_ISP_HOLD_BUFS];
	int hold_enable;
	ISP_HOLD_TIME_ENUM hold_time;

	/* Shared real time buffer table, mapped at page offset 16. */
	struct page *rtbuf_pages;
	ISP_RT_BUF_STRUCT *rtbuf;
	u32 sof_count[MTK_ISP_CH_MAX];
	u32 start_time[MTK_ISP_CH_MAX];
	u32 avg_frame_time[MTK_ISP_CH_MAX];
	u32 prv_addr[MTK_ISP_CH_MAX];
	u32 prv_tstamp_s[MTK_ISP_DMA_NUM];
	u32 prv_tstamp_us[MTK_ISP_DMA_NUM];
	s32 last_mnum[MTK_ISP_DMA_NUM];
	u32 lost_p1done_cnt;
	u32 lost_p1d_done_cnt;
	bool first_sof[MTK_ISP_IRQ_DOM_NUM];
	struct mtk_isp_fw_rcnt fw;
	u8 dma_en_rec[MTK_ISP_DMA_NUM][MTK_ISP_RT_BUF_SIZE];

	u32 dma_err[ISP_DMA_ERR_COUNT];
	u32 int_err[MTK_ISP_IRQ_DOM_NUM];
	bool camsv_drop[2];		/* gSof_camsvdone[2] */

	struct mtk_isp_edbuf ed_ring[MTK_ISP_ED_RING];
	struct mtk_isp_edmgr ed_mgr[MTK_ISP_ED_MGR];
	int ed_r_first, ed_r_cur, ed_r_last;
	int ed_m_first, ed_m_last;
	s32 ed_remain;

	u32 reg_scen;
	s32 burst_qnum;
	u32 debug_mask;
	struct mtk_isp_tstamp ts;
	atomic_t refcnt[ISP_REF_CNT_ID_MAX];
};

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

static void mtk_isp_dump_reg(struct mtk_isp *p, const char *why);

static inline u32 isp_rd(struct mtk_isp *p, u32 off)
{
	return readl(p->isp + off);
}

static inline void isp_wr(struct mtk_isp *p, u32 off, u32 val)
{
	writel(val, p->isp + off);
}

/* CAMINF addressing: offset relative to 0x15000000 (uapi ISP_REG_STRUCT). */
static inline u32 cam_rd(struct mtk_isp *p, u32 off)
{
	return readl(p->cam + off);
}

static inline void cam_wr(struct mtk_isp *p, u32 off, u32 val)
{
	writel(val, p->cam + off);
}

static bool mtk_isp_cam_addr_valid(u32 addr)
{
	return addr >= MTK_ISP_CAM_ADDR_MIN && addr < MTK_ISP_CAM_ADDR_MAX;
}

/* FBC bitfields, union _CQ_RTBC_FBC_ in camera_isp_D1.h. */
static inline u32 mtk_isp_fbc_cnt(u32 fbc)
{
	return fbc & 0xf;
}

static inline u32 mtk_isp_fbc_fbnum(u32 fbc)
{
	return (fbc >> 16) & 0xf;
}

static inline u32 mtk_isp_fbc_wcnt(u32 fbc)
{
	return (fbc >> 24) & 0xf;
}

static enum mtk_isp_irq_dom mtk_isp_dom_of_dma(unsigned int dma)
{
	switch (dma) {
	case _imgo_:
	case _rrzo_:
		return MTK_ISP_IRQ_CAM;
	case _imgo_d_:
	case _rrzo_d_:
		return MTK_ISP_IRQ_CAM_D;
	case _camsv_imgo_:
		return MTK_ISP_IRQ_CAMSV;
	case _camsv2_imgo_:
		return MTK_ISP_IRQ_CAMSV2;
	default:
		return MTK_ISP_IRQ_CAM;
	}
}

static enum mtk_isp_channel mtk_isp_channel_of_dma(unsigned int dma)
{
	switch (dma) {
	case _imgo_:
	case _rrzo_:
		return MTK_ISP_CH_P1;
	case _imgo_d_:
	case _rrzo_d_:
		return MTK_ISP_CH_P1_D;
	case _camsv_imgo_:
		return MTK_ISP_CH_CAMSV;
	case _camsv2_imgo_:
		return MTK_ISP_CH_CAMSV_D;
	default:
		return MTK_ISP_CH_P1;
	}
}

/* Index of the lowest set bit; my_get_pow_idx(), camera_isp_D1.c:867-880. */
static int mtk_isp_pow_idx(u32 value)
{
	int i;

	for (i = 0; i < 32; i++)
		if ((value >> i) & 0x1)
			return i;
	return 32;
}

/* ------------------------------------------------------------------ */
/* Reset (camera_isp_D1.c:3263-3387)                                   */
/* ------------------------------------------------------------------ */

static void mtk_isp_reset(struct mtk_isp *p, unsigned int path)
{
	unsigned long flags;
	u32 reg, set;

	/* The reset sequences are read-modify-write of CAM_SW_CTL and share the
	 * register file with the FBC write-backs of the interrupt handlers. */
	spin_lock_irqsave(&p->reg_lock, flags);
	switch (path) {
	case ISP_REG_SW_CTL_RST_CAM_P1:
		reg = isp_rd(p, ISP_REG_CAM_SW_CTL);
		set = (reg & ~ISP_SW_CTL_SW_RST_P1_MASK) |
		      (ISP_SW_CTL_SW_RST_TRIG & ISP_SW_CTL_SW_RST_P1_MASK);
		isp_wr(p, ISP_REG_CAM_SW_CTL, set);
		/* The ROM sequence re-reads the register once and then writes
		 * HW_RST followed by the cleared trigger; there is no status
		 * poll in the ROM build (see camera_isp_D1.c:3275-3295). */
		reg = isp_rd(p, ISP_REG_CAM_SW_CTL);
		isp_wr(p, ISP_REG_CAM_SW_CTL, ISP_SW_CTL_HW_RST);
		set = reg & ~ISP_SW_CTL_SW_RST_P1_MASK;
		isp_wr(p, ISP_REG_CAM_SW_CTL, set);
		break;
	case ISP_REG_SW_CTL_RST_CAM_P2:
		reg = isp_rd(p, ISP_REG_CAM_SW_CTL);
		set = (reg & ~ISP_SW_CTL_SW_RST_P2_MASK) |
		      (ISP_SW_CTL_SW_RST_P2_TRIG & ISP_SW_CTL_SW_RST_P2_MASK);
		isp_wr(p, ISP_REG_CAM_SW_CTL, set);
		reg = isp_rd(p, ISP_REG_CAM_SW_CTL);
		isp_wr(p, ISP_REG_CAM_SW_CTL, ISP_SW_CTL_HW_RST_P2);
		set = reg & ~ISP_SW_CTL_SW_RST_P2_MASK;
		isp_wr(p, ISP_REG_CAM_SW_CTL, set);
		break;
	case ISP_REG_SW_CTL_RST_CAMSV:
		isp_wr(p, ISP_REG_CAMSV_SW_CTL, ISP_SW_CTL_SW_RST_TRIG);
		isp_wr(p, ISP_REG_CAMSV_SW_CTL, 0);
		isp_rd(p, ISP_REG_CAMSV_SW_CTL);
		isp_wr(p, ISP_REG_CAMSV_SW_CTL, ISP_SW_CTL_HW_RST);
		isp_wr(p, ISP_REG_CAMSV_SW_CTL, 0);
		break;
	case ISP_REG_SW_CTL_RST_CAMSV2:
		isp_wr(p, ISP_REG_CAMSV2_SW_CTL, ISP_SW_CTL_SW_RST_TRIG);
		isp_wr(p, ISP_REG_CAMSV2_SW_CTL, 0);
		isp_rd(p, ISP_REG_CAMSV2_SW_CTL);
		isp_wr(p, ISP_REG_CAMSV2_SW_CTL, ISP_SW_CTL_HW_RST);
		isp_wr(p, ISP_REG_CAMSV2_SW_CTL, 0);
		break;
	default:
		dev_warn(p->dev, "unknown reset path %u\n", path);
		break;
	}
	spin_unlock_irqrestore(&p->reg_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Hold-register buffers (camera_isp_D1.c:3462-3960)                   */
/* ------------------------------------------------------------------ */

static void mtk_isp_hold_free(struct mtk_isp *p)
{
	int i;

	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		kfree(p->hold[i].data);
		p->hold[i].data = NULL;
		p->hold[i].len = 0;
		p->hold[i].state = MTK_ISP_HOLD_EMPTY;
	}
}

static int mtk_isp_hold_alloc(struct mtk_isp *p)
{
	int i;

	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		p->hold[i].data = kmalloc(MTK_ISP_HOLD_BUF_SIZE, GFP_KERNEL);
		if (!p->hold[i].data) {
			mtk_isp_hold_free(p);
			return -ENOMEM;
		}
		p->hold[i].len = 0;
		p->hold[i].state = MTK_ISP_HOLD_EMPTY;
	}
	return 0;
}

static void mtk_isp_hold_reset(struct mtk_isp *p)
{
	int i;

	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		p->hold[i].state = MTK_ISP_HOLD_EMPTY;
		p->hold[i].len = 0;
	}
	p->hold_enable = 0;
}

/*
 * Append a chunk of ISP_REG_STRUCT entries to the buffer that is still in
 * HOLD state, or start a new one (camera_isp_D1.c:3576-3640).
 */
static int mtk_isp_hold_add(struct mtk_isp *p, const void __user *src,
			    size_t size)
{
	int i;

	if (!size || size > MTK_ISP_HOLD_BUF_SIZE)
		return -EINVAL;

	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		if (p->hold[i].state != MTK_ISP_HOLD_HOLD)
			continue;
		if (p->hold[i].len + size > MTK_ISP_HOLD_BUF_SIZE)
			return -ENOSPC;
		if (copy_from_user(p->hold[i].data + p->hold[i].len, src, size))
			return -EFAULT;
		p->hold[i].len += size;
		return 0;
	}
	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		if (p->hold[i].state != MTK_ISP_HOLD_EMPTY)
			continue;
		if (copy_from_user(p->hold[i].data, src, size))
			return -EFAULT;
		p->hold[i].len = size;
		p->hold[i].state = MTK_ISP_HOLD_HOLD;
		return 0;
	}
	return -ENOSPC;
}

/* ISP_BufWrite_WriteToHw(): drain every READY buffer into the registers. */
static void mtk_isp_hold_flush(struct mtk_isp *p)
{
	unsigned long flags;
	ISP_REG_STRUCT *regs;
	u32 i, k, n;

	spin_lock_irqsave(&p->hold_lock, flags);
	for (i = 0; i < MTK_ISP_HOLD_BUFS; i++) {
		if (p->hold[i].state != MTK_ISP_HOLD_READY)
			continue;
		regs = (ISP_REG_STRUCT *)p->hold[i].data;
		n = p->hold[i].len / sizeof(ISP_REG_STRUCT);
		for (k = 0; k < n; k++) {
			if (!mtk_isp_cam_addr_valid(regs[k].Addr)) {
				dev_err_ratelimited(p->dev,
						    "hold: bad register 0x%08x\n",
						    regs[k].Addr);
				continue;
			}
			cam_wr(p, regs[k].Addr, regs[k].Val);
		}
		p->hold[i].state = MTK_ISP_HOLD_EMPTY;
		p->hold[i].len = 0;
	}
	spin_unlock_irqrestore(&p->hold_lock, flags);
}

/* ISP_EnableHoldReg(): on disable every collected write becomes READY. */
static int mtk_isp_hold_enable(struct mtk_isp *p, bool enable)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&p->hold_lock, flags);
	if (!enable) {
		for (i = 0; i < MTK_ISP_HOLD_BUFS; i++)
			if (p->hold[i].state == MTK_ISP_HOLD_HOLD)
				p->hold[i].state = MTK_ISP_HOLD_READY;
	}
	p->hold_enable = enable ? 1 : 0;
	spin_unlock_irqrestore(&p->hold_lock, flags);
	return 0;
}

static void mtk_isp_vd_tasklet(struct tasklet_struct *t)
{
	struct mtk_isp *p = from_tasklet(p, t, vd_tasklet);

	if (p->hold_time == ISP_HOLD_TIME_VD)
		mtk_isp_hold_flush(p);
}

static void mtk_isp_expdone_tasklet(struct tasklet_struct *t)
{
	struct mtk_isp *p = from_tasklet(p, t, expdone_tasklet);

	if (p->hold_time == ISP_HOLD_TIME_EXPDONE)
		mtk_isp_hold_flush(p);
}

/* ------------------------------------------------------------------ */
/* Register read/write ioctls (camera_isp_D1.c:3387-3455, 3805-3869)   */
/* ------------------------------------------------------------------ */

static int mtk_isp_reg_read(struct mtk_isp *p, const ISP_REG_IO_STRUCT *io)
{
	u32 __user *up = (u32 __user *)(uintptr_t)io->pData;
	u32 i, addr, val;

	if (!up)
		return -EFAULT;

	for (i = 0; i < io->Count; i++) {
		if (get_user(addr, up + 2 * i))
			return -EFAULT;
		if (!mtk_isp_cam_addr_valid(addr)) {
			dev_err_ratelimited(p->dev,
					    "read: address 0x%08x out of range\n",
					    addr);
			val = 0;
		} else {
			val = cam_rd(p, addr);
		}
		if (put_user(val, up + 2 * i + 1))
			return -EFAULT;
	}
	return 0;
}

static void mtk_isp_reg_write_hw(struct mtk_isp *p, const ISP_REG_STRUCT *regs,
				 u32 count)
{
	unsigned long flags;
	u32 i;

	spin_lock_irqsave(&p->reg_lock, flags);
	for (i = 0; i < count; i++) {
		if (!mtk_isp_cam_addr_valid(regs[i].Addr)) {
			dev_err_ratelimited(p->dev,
					    "write: address 0x%08x out of range\n",
					    regs[i].Addr);
			continue;
		}
		cam_wr(p, regs[i].Addr, regs[i].Val);
	}
	spin_unlock_irqrestore(&p->reg_lock, flags);
}

static int mtk_isp_reg_write(struct mtk_isp *p, const ISP_REG_IO_STRUCT *io)
{
	const void __user *up = (const void __user *)(uintptr_t)io->pData;
	ISP_REG_STRUCT *regs;
	size_t size;
	int ret;

	if (!up || !io->Count)
		return -EINVAL;
	size = io->Count * sizeof(ISP_REG_STRUCT);

	if (p->hold_enable) {
		spin_lock_bh(&p->hold_lock);
		ret = mtk_isp_hold_add(p, up, size);
		spin_unlock_bh(&p->hold_lock);
		if (ret)
			dev_err(p->dev, "hold: add of %zu bytes failed: %d\n",
				size, ret);
		return ret;
	}

	regs = memdup_user(up, size);
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	mtk_isp_reg_write_hw(p, regs, io->Count);
	kfree(regs);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Interrupt bookkeeping                                               */
/* ------------------------------------------------------------------ */

static inline u32 mtk_isp_irq_state(struct mtk_isp *p, unsigned int type,
				    unsigned int user, u32 status)
{
	unsigned long flags;
	u32 ret;

	spin_lock_irqsave(&p->irq_lock, flags);
	ret = p->status[user][type] & status;
	spin_unlock_irqrestore(&p->irq_lock, flags);
	return ret;
}

static void mtk_isp_irq_init_defaults(struct mtk_isp *p)
{
	p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST] = ISP_MASK_INT_P1_ST;
	p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST2] = ISP_MASK_INT_P1_ST2;
	p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST_D] = ISP_MASK_INT_P1_ST_D;
	p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST2_D] = ISP_MASK_INT_P1_ST2_D;
	p->irq_mask[ISP_IRQ_TYPE_INT_P2_ST] = ISP_MASK_INT_P2_ST;
	p->irq_mask[ISP_IRQ_TYPE_INT_STATUSX] = ISP_MASK_INT_STATUSX;
	p->irq_mask[ISP_IRQ_TYPE_INT_STATUS2X] = ISP_MASK_INT_STATUS2X;
	p->irq_mask[ISP_IRQ_TYPE_INT_STATUS3X] = ISP_MASK_INT_STATUS3X;
	p->irq_mask[ISP_IRQ_TYPE_INT_SENINF1] = ISP_MASK_INT_SENINF;
	p->irq_mask[ISP_IRQ_TYPE_INT_SENINF2] = ISP_MASK_INT_SENINF;
	p->irq_mask[ISP_IRQ_TYPE_INT_SENINF3] = ISP_MASK_INT_SENINF;
	p->irq_mask[ISP_IRQ_TYPE_INT_SENINF4] = ISP_MASK_INT_SENINF;
	p->irq_mask[ISP_IRQ_TYPE_INT_CAMSV] = ISP_MASK_CAMSV;
	p->irq_mask[ISP_IRQ_TYPE_INT_CAMSV2] = ISP_MASK_CAMSV2;

	p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST] = ISP_MASK_INT_P1_ST_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST2] = 0;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST_D] = ISP_MASK_INT_P1_ST_D_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST2_D] = 0;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_P2_ST] = ISP_MASK_INT_P2_ST_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUSX] = ISP_MASK_INT_STATUSX_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUS2X] = ISP_MASK_INT_STATUS2X_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUS3X] = ISP_MASK_INT_STATUS3X_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_SENINF1] = ISP_MASK_INT_SENINF_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_SENINF2] = ISP_MASK_INT_SENINF_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_SENINF3] = ISP_MASK_INT_SENINF_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_SENINF4] = ISP_MASK_INT_SENINF_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_CAMSV] = ISP_MASK_CAMSV_ERR;
	p->irq_err_mask[ISP_IRQ_TYPE_INT_CAMSV2] = ISP_MASK_CAMSV2_ERR;
}

/* ISP_open() state reset, camera_isp_D1.c:11035-11120. */
static void mtk_isp_session_reset(struct mtk_isp *p)
{
	int u, t, b;

	for (u = 0; u < MTK_ISP_IRQ_USERS; u++) {
		for (t = 0; t < MTK_ISP_IRQ_TYPES; t++) {
			p->status[u][t] = 0;
			p->marked[u][t] = 0;
			for (b = 0; b < 32; b++) {
				p->marked_sec[u][t][b] = 0;
				p->marked_usec[u][t][b] = 0;
				p->passed_cnt[u][t][b] = 0;
				p->last_sec[t][b] = 0;
				p->last_usec[t][b] = 0;
			}
		}
	}
	for (t = 0; t < ISP_IRQ_TYPE_INT_STATUSX; t++) {
		for (b = 0; b < MTK_ISP_EISMETA_RINGSIZE; b++) {
			p->eismeta[t][b].tLastSOF2P1done_sec = 0;
			p->eismeta[t][b].tLastSOF2P1done_usec = 0;
		}
	}
	p->eismeta_ridx = 0;
	p->eismeta_widx = 0;
	p->eismeta_in_sof = 0;
	p->eismeta_ridx_d = 0;
	p->eismeta_widx_d = 0;
	p->eismeta_in_sof_d = 0;

	for (u = 0; u < MTK_ISP_IRQ_USERS; u++) {
		memset(p->userkey[u].name, 0, MTK_ISP_USERKEY_LEN);
		p->userkey[u].key = -1;
		p->flush_v3[u] = 0;
	}
	p->first_unused_key = 1;
	for (u = 0; u < ISP_IRQ_USER_MAX; u++)
		p->flush_v1[u] = 0;
	p->flush_v1[ISP_IRQ_USER_3A] = ISP_IRQ_P1_VS1 | ISP_IRQ_P1_VS1 |
				      ISP_IRQ_P1_AF_DON | ISP_IRQ_P1_AF_DON;
	p->flush_v1[ISP_IRQ_USER_EIS] = ISP_IRQ_P1_VS1 | ISP_IRQ_P1_VS1 |
					ISP_IRQ_P1_PASS1_DON |
					ISP_IRQ_P1_PASS1_DON;
	p->flush_v1[ISP_IRQ_USER_VHDR] = p->flush_v1[ISP_IRQ_USER_EIS];

	p->reg_scen = 0xa5a5a5a5;
	p->burst_qnum = 1;
	p->ts.fps = 0;
	p->ts.interval_us = 0;
	p->ts.compensation_us = 0;
	p->ts.t_ns = 0;
	p->ts.fcnt = 0;

	for (u = 0; u < MTK_ISP_ED_RING; u++) {
		p->ed_ring[u].process_id = 0;
		p->ed_ring[u].caller_id = 0;
		p->ed_ring[u].dup_cq_idx = -1;
		p->ed_ring[u].state = ISP_ED_BUF_STATE_NONE;
	}
	p->ed_r_first = 0;
	p->ed_r_cur = 0;
	p->ed_r_last = -1;
	for (u = 0; u < MTK_ISP_ED_MGR; u++) {
		p->ed_mgr[u].process_id = 0;
		p->ed_mgr[u].caller_id = 0;
		p->ed_mgr[u].dup_cq_idx = -1;
		p->ed_mgr[u].dequed_num = 0;
	}
	p->ed_m_first = 0;
	p->ed_m_last = -1;
	p->ed_remain = 0;

	memset(p->dma_err, 0, sizeof(p->dma_err));
	memset(p->int_err, 0, sizeof(p->int_err));
	memset(p->sof_count, 0, sizeof(p->sof_count));
	memset(p->start_time, 0, sizeof(p->start_time));
	memset(p->avg_frame_time, 0, sizeof(p->avg_frame_time));
	memset(p->prv_addr, 0, sizeof(p->prv_addr));
	memset(p->prv_tstamp_s, 0, sizeof(p->prv_tstamp_s));
	memset(p->prv_tstamp_us, 0, sizeof(p->prv_tstamp_us));
	memset(p->last_mnum, 0, sizeof(p->last_mnum));
	memset(&p->fw, 0, sizeof(p->fw));
	memset(p->dma_en_rec, 0, sizeof(p->dma_en_rec));
	for (u = 0; u < MTK_ISP_IRQ_DOM_NUM; u++)
		p->first_sof[u] = true;

	if (p->rtbuf) {
		memset(p->rtbuf, 0, sizeof(*p->rtbuf));
		for (u = 0; u < _rt_dma_max_; u++) {
			int i;

			for (i = 0; i < MTK_ISP_RT_BUF_SIZE; i++)
				p->rtbuf->ring_buf[u].data[i].image.frm_cnt =
					MTK_ISP_INVALID_FRM_CNT;
		}
	}
}

/* Publish the sampled status words to every user slot
 * (camera_isp_D1.c:9636-9665). */
static void mtk_isp_irq_publish(struct mtk_isp *p, const u32 *st, u32 sec,
				u32 usec)
{
	unsigned long flags;
	int u, t, k, idx;

	spin_lock_irqsave(&p->irq_lock, flags);
	for (u = 0; u < MTK_ISP_IRQ_USERS; u++) {
		for (t = 0; t < MTK_ISP_IRQ_TYPES; t++) {
			p->status[u][t] |= st[t] & p->irq_mask[t];
			if (!(p->marked[u][t] & p->irq_mask[t]))
				continue;
			for (k = 0; k < 32; k++) {
				if (!((st[t] & p->irq_mask[t]) & (1 << k)))
					continue;
				idx = mtk_isp_pow_idx(1 << k);
				p->last_sec[t][idx] = sec;
				p->last_usec[t][idx] = usec;
				p->passed_cnt[u][t][k]++;
			}
		}
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);
}

/* ------------------------------------------------------------------ */
/* Timestamps                                                          */
/* ------------------------------------------------------------------ */

static void mtk_isp_now(struct mtk_isp *p, u32 *sec, u32 *usec)
{
	u64 ns = ktime_get_ns();

	/* T_STAMP_2_0: once userspace reported the frame rate the SOF
	 * timestamp is advanced by a fixed frame interval instead of being
	 * sampled, which compensates interrupt delivery jitter
	 * (camera_isp_D1.c:9385-9420). */
	if (p->ts.fps > MTK_ISP_SLOW_MOTION_FPS && p->ts.t_ns) {
		p->ts.t_ns += (u64)p->ts.interval_us * 1000;
		if (++p->ts.fcnt == (u32)p->ts.fps) {
			p->ts.fcnt = 0;
			p->ts.t_ns += (u64)p->ts.compensation_us * 1000;
		}
		ns = p->ts.t_ns;
	}
	*sec = div_u64(ns, 1000000000ULL);
	*usec = div_u64(ns % 1000000000ULL, 1000);
}

static void mtk_isp_frame_profile(struct mtk_isp *p, enum mtk_isp_channel ch,
				  u32 sec, u32 usec)
{
	u32 now_us = sec * 1000000U + usec;

	if (!p->start_time[ch]) {
		p->start_time[ch] = now_us;
	} else if (p->avg_frame_time[ch]) {
		p->avg_frame_time[ch] += now_us - p->avg_frame_time[ch];
		p->avg_frame_time[ch] >>= 1;
	} else {
		p->avg_frame_time[ch] = now_us - p->start_time[ch];
	}
}

/* ------------------------------------------------------------------ */
/* Pass-1 DONE: publish the completed buffer                           */
/* ------------------------------------------------------------------ */

static void mtk_isp_done_buf_time(struct mtk_isp *p, enum mtk_isp_irq_dom dom,
				  u32 fbc_imgo, u32 fbc_rrzo, u32 sec, u32 usec)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring;
	unsigned int ch_imgo, ch_rrzo, dma;
	enum mtk_isp_channel ch;
	u32 wcnt, fb_num, shift_t = 0, round;
	bool shift = false;
	int k;

	if (dom == MTK_ISP_IRQ_CAM) {
		ch_imgo = _imgo_;
		ch_rrzo = _rrzo_;
	} else {
		ch_imgo = _imgo_d_;
		ch_rrzo = _rrzo_d_;
	}

	if (p->rtbuf->ring_buf[ch_imgo].active) {
		dma = ch_imgo;
	} else if (p->rtbuf->ring_buf[ch_rrzo].active) {
		dma = ch_rrzo;
	} else {
		dev_err_ratelimited(p->dev, "done: no active DMA port\n");
		return;
	}

	/* ISR callback timing shift handling, camera_isp_D1.c:6880-6935. */
	fb_num = mtk_isp_fbc_fbnum(dma == ch_imgo ? fbc_imgo : fbc_rrzo);
	wcnt = mtk_isp_fbc_wcnt(dma == ch_imgo ? fbc_imgo : fbc_rrzo);
	if (wcnt && fb_num) {
		u32 start = p->rtbuf->ring_buf[dma].start;

		if (wcnt > start + 2) {
			shift_t = wcnt - start - 2;
		} else if (wcnt < start + 2) {
			shift_t = wcnt + fb_num - (start + 2);
			if (shift_t >= fb_num)
				shift_t = fb_num - 1;
		}
		shift = true;
	}

	ch = mtk_isp_channel_of_dma(dma);

	for (round = 0; round < (shift ? shift_t + 1 : 1); round++) {
		for (k = 0; k < 2; k++) {
			unsigned int idma = k ? ch_rrzo : ch_imgo;
			u32 curr;

			ring = &p->rtbuf->ring_buf[idma];
			if (!ring->empty_count || !ring->total_count)
				continue;
			curr = ring->start;
			if (curr >= MTK_ISP_RT_BUF_SIZE ||
			    curr >= ring->total_count)
				continue;
			if (ring->data[curr].bFilled != MTK_ISP_BUF_EMPTY)
				continue;
			ring->data[curr].bFilled = MTK_ISP_BUF_FILLED;
			ring->start = (curr + 1) % ring->total_count;
			ring->empty_count--;
			ring->img_cnt = p->sof_count[ch];
			ring->data[curr].timeStampS = sec;
			ring->data[curr].timeStampUs = usec;
		}
	}

	p->rtbuf->state = ISP_RTBC_STATE_DONE;
}

/* ------------------------------------------------------------------ */
/* Pass-1 SOF: buffer switch, RCNT_INC and per-frame metadata          */
/* ------------------------------------------------------------------ */

static void mtk_isp_imgo_meta(struct mtk_isp *p, bool d, u32 idx)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring;
	ISP_RT_IMAGE_INFO_STRUCT *img;
	ISP_RT_DMAO_CROPPING_STRUCT *crop;
	ISP_RT_RRZ_INFO_STRUCT *rrz;
	unsigned int ch_imgo = d ? _imgo_d_ : _imgo_;
	unsigned int ch_rrzo = d ? _rrzo_d_ : _rrzo_;
	u32 tmp, sel, off;
	u32 imgo_xsize, imgo_ysize;

	if (!d) {
		sel = cam_rd(p, ISP_INNER_CAM_CTRL_SEL_P1);
		ring = &p->rtbuf->ring_buf[ch_imgo];
		img = &ring->data[idx].image;
		crop = &ring->data[idx].dmaoCrop;
		tmp = isp_rd(p, ISP_REG_TG_GRAB_W);
		img->xsize = cam_rd(p, ISP_INNER_IMGO_XSIZE) & 0x3fff;
		img->w = ((tmp >> 16) & 0x7fff) - (tmp & 0x7fff);
		tmp = isp_rd(p, ISP_REG_TG_GRAB_H);
		img->h = ((tmp >> 16) & 0x1fff) - (tmp & 0x1fff);
		off = cam_rd(p, ISP_INNER_IMGO_STRIDE);
		img->stride = off & 0x3fff;
		img->bus_size = (off >> 16) & 0x3;
		off = cam_rd(p, ISP_INNER_FMT_SEL_P1);
		img->fmt = (off & 0xf000) >> 12;
		img->pxl_id = off & 0x3;
		img->m_num_0 = cam_rd(p, ISP_REG_TG_MAGIC_0);
		img->frm_cnt = (isp_rd(p, ISP_REG_TG_INTER_ST) & 0x00ff0000) >> 16;
		off = cam_rd(p, ISP_INNER_IMGO_CROP);
		crop->x = off & 0x3fff;
		crop->y = (off >> 16) & 0x1fff;
		imgo_xsize = cam_rd(p, ISP_INNER_IMGO_XSIZE);
		imgo_ysize = cam_rd(p, ISP_INNER_IMGO_YSIZE);
		crop->w = (imgo_xsize & 0x3fff) + 1;
		crop->h = (imgo_ysize & 0x1fff) + 1;
		ring->data[idx].bProcessRaw =
			(sel & ISP_CAM_CTL_SEL_P1_IMGO) ?
			ISP_PURE_RAW : ISP_RROCESSED_RAW;

		/* RRZO mirrors the IMGO frame, camera_isp_D1.c:6388-6430. */
		ring = &p->rtbuf->ring_buf[ch_rrzo];
		img = &ring->data[idx].image;
		crop = &ring->data[idx].dmaoCrop;
		rrz = &ring->data[idx].rrzInfo;
		img->xsize = cam_rd(p, ISP_INNER_RRZO_XSIZE) & 0x3fff;
		img->w = p->rtbuf->ring_buf[ch_imgo].data[idx].image.w;
		img->h = p->rtbuf->ring_buf[ch_imgo].data[idx].image.h;
		off = cam_rd(p, ISP_INNER_RRZO_STRIDE);
		img->stride = off & 0x3fff;
		img->bus_size = (off >> 16) & 0x3;
		off = cam_rd(p, ISP_INNER_FMT_SEL_P1);
		img->fmt = (off & 0x30) >> 4;
		rrz->srcX = cam_rd(p, ISP_INNER_RRZ_HORI_INT_OFST) & 0x1fff;
		rrz->srcY = cam_rd(p, ISP_INNER_RRZ_VERT_INT_OFST) & 0x1fff;
		off = isp_rd(p, ISP_REG_TG_RRZ_CROP_IN);
		rrz->srcW = off & 0xffff;
		rrz->srcH = off >> 16;
		rrz->dstW = cam_rd(p, ISP_REG_RRZ_W);
		rrz->dstH = (cam_rd(p, ISP_INNER_RRZ_OUT_IMG) >> 16) & 0x1fff;
		off = cam_rd(p, ISP_INNER_RRZO_CROP);
		crop->x = off & 0x3fff;
		crop->y = (off >> 16) & 0x1fff;
		crop->w = (cam_rd(p, ISP_INNER_RRZO_XSIZE) & 0x3fff) + 1;
		crop->h = (cam_rd(p, ISP_INNER_RRZO_YSIZE) & 0x1fff) + 1;
	} else {
		sel = cam_rd(p, ISP_INNER_CAM_CTRL_SEL_P1_D);
		ring = &p->rtbuf->ring_buf[ch_imgo];
		img = &ring->data[idx].image;
		crop = &ring->data[idx].dmaoCrop;
		tmp = isp_rd(p, ISP_REG_TG2_GRAB_W);
		img->xsize = cam_rd(p, ISP_INNER_IMGO_D_XSIZE) & 0x3fff;
		img->w = ((tmp >> 16) & 0x7fff) - (tmp & 0x7fff);
		tmp = isp_rd(p, ISP_REG_TG2_GRAB_H);
		img->h = ((tmp >> 16) & 0x1fff) - (tmp & 0x1fff);
		off = cam_rd(p, ISP_INNER_IMGO_D_STRIDE);
		img->stride = off & 0x3fff;
		img->bus_size = (off >> 16) & 0x3;
		off = cam_rd(p, ISP_INNER_FMT_SEL_P1_D);
		img->fmt = (off & 0xf000) >> 12;
		img->pxl_id = off & 0x3;
		img->m_num_0 = cam_rd(p, ISP_REG_TG2_MAGIC_0);
		img->frm_cnt = (isp_rd(p, ISP_REG_TG2_INTER_ST) & 0x00ff0000) >> 16;
		off = cam_rd(p, ISP_INNER_IMGO_D_CROP);
		crop->x = off & 0x3fff;
		crop->y = (off >> 16) & 0x1fff;
		crop->w = (cam_rd(p, ISP_INNER_IMGO_D_XSIZE) & 0x3fff) + 1;
		crop->h = (cam_rd(p, ISP_INNER_IMGO_D_YSIZE) & 0x1fff) + 1;
		ring->data[idx].bProcessRaw =
			(sel & ISP_CAM_CTL_SEL_P1_D_IMGO) ?
			ISP_PURE_RAW : ISP_RROCESSED_RAW;

		ring = &p->rtbuf->ring_buf[ch_rrzo];
		img = &ring->data[idx].image;
		crop = &ring->data[idx].dmaoCrop;
		rrz = &ring->data[idx].rrzInfo;
		img->xsize = cam_rd(p, ISP_INNER_RRZO_D_XSIZE) & 0x3fff;
		img->w = p->rtbuf->ring_buf[ch_imgo].data[idx].image.w;
		img->h = p->rtbuf->ring_buf[ch_imgo].data[idx].image.h;
		off = cam_rd(p, ISP_INNER_RRZO_D_STRIDE);
		img->stride = off & 0x3fff;
		img->bus_size = (off >> 16) & 0x3;
		off = cam_rd(p, ISP_INNER_FMT_SEL_P1_D);
		img->fmt = (off & 0x30) >> 4;
		rrz->srcX = cam_rd(p, ISP_INNER_RRZ_D_HORI_INT_OFST) & 0x1fff;
		rrz->srcY = cam_rd(p, ISP_INNER_RRZ_D_VERT_INT_OFST) & 0x1fff;
		off = isp_rd(p, ISP_REG_TG_RRZ_CROP_IN_D);
		rrz->srcW = off & 0xffff;
		rrz->srcH = off >> 16;
		rrz->dstW = cam_rd(p, ISP_REG_RRZ_W_D);
		rrz->dstH = (cam_rd(p, ISP_INNER_RRZ_D_OUT_IMG) >> 16) & 0x1fff;
		off = cam_rd(p, ISP_INNER_RRZO_D_CROP);
		crop->x = off & 0x3fff;
		crop->y = (off >> 16) & 0x1fff;
		crop->w = (cam_rd(p, ISP_INNER_RRZO_D_XSIZE) & 0x3fff) + 1;
		crop->h = (cam_rd(p, ISP_INNER_RRZO_D_YSIZE) & 0x1fff) + 1;
	}

	ring = &p->rtbuf->ring_buf[ch_rrzo];
	img = &ring->data[idx].image;
	img->pxl_id = p->rtbuf->ring_buf[ch_imgo].data[idx].image.pxl_id;
	img->m_num_0 = p->rtbuf->ring_buf[ch_imgo].data[idx].image.m_num_0;
	img->frm_cnt = p->rtbuf->ring_buf[ch_imgo].data[idx].image.frm_cnt;
	ring->data[idx].bProcessRaw = ISP_RROCESSED_RAW;
}

/*
 * ISP_SOF_Buf_Get(): stamp the frame, fill the per-frame metadata and advance
 * the software ring (camera_isp_D1.c:6177-6608).
 */
static void mtk_isp_sof_buf_get(struct mtk_isp *p, enum mtk_isp_irq_dom dom,
				const u32 *fbc, u32 sec, u32 usec, bool drop)
{
	unsigned int ch_imgo = (dom == MTK_ISP_IRQ_CAM) ? _imgo_ : _imgo_d_;
	unsigned int ch_rrzo = (dom == MTK_ISP_IRQ_CAM) ? _rrzo_ : _rrzo_d_;
	enum mtk_isp_channel ch = (dom == MTK_ISP_IRQ_CAM) ?
				  MTK_ISP_CH_P1 : MTK_ISP_CH_P1_D;
	ISP_RT_RING_BUF_INFO_STRUCT *ring;
	u32 wcnt, idx;

	if (p->first_sof[dom]) {
		wcnt = mtk_isp_fbc_wcnt(dom == MTK_ISP_IRQ_CAM ? fbc[0] : fbc[2]);
		p->rtbuf->ring_buf[ch_imgo].start = wcnt ? wcnt - 1 : 0;
		wcnt = mtk_isp_fbc_wcnt(dom == MTK_ISP_IRQ_CAM ? fbc[1] : fbc[3]);
		p->rtbuf->ring_buf[ch_rrzo].start = wcnt ? wcnt - 1 : 0;
		p->first_sof[dom] = false;
		p->ts.t_ns = (u64)sec * 1000000000ULL + (u64)usec * 1000;
	}

	p->rtbuf->dropCnt = drop ? 1 : 0;
	if (p->rtbuf->dropCnt)
		return;

	if (!p->rtbuf->ring_buf[ch_imgo].active &&
	    !p->rtbuf->ring_buf[ch_rrzo].active) {
		dev_err_ratelimited(p->dev, "sof: no active DMA port\n");
		return;
	}

	ring = &p->rtbuf->ring_buf[ch_imgo];
	if (ring->total_count > MTK_ISP_RT_BUF_SIZE) {
		dev_err_ratelimited(p->dev, "sof: bad buffer count %u\n",
				    ring->total_count);
		ring->total_count = MTK_ISP_RT_BUF_SIZE;
	}
	ring = &p->rtbuf->ring_buf[ch_rrzo];
	if (ring->total_count > MTK_ISP_RT_BUF_SIZE)
		ring->total_count = MTK_ISP_RT_BUF_SIZE;

	idx = p->rtbuf->ring_buf[ch_imgo].start;
	if (idx < MTK_ISP_RT_BUF_SIZE &&
	    idx < p->rtbuf->ring_buf[ch_imgo].total_count) {
		p->rtbuf->ring_buf[ch_imgo].data[idx].timeStampS = sec;
		p->rtbuf->ring_buf[ch_imgo].data[idx].timeStampUs = usec;
	}
	idx = p->rtbuf->ring_buf[ch_rrzo].start;
	if (idx < MTK_ISP_RT_BUF_SIZE &&
	    idx < p->rtbuf->ring_buf[ch_rrzo].total_count) {
		p->rtbuf->ring_buf[ch_rrzo].data[idx].timeStampS = sec;
		p->rtbuf->ring_buf[ch_rrzo].data[idx].timeStampUs = usec;
	}

	idx = p->rtbuf->ring_buf[ch_imgo].start;
	if (idx < MTK_ISP_RT_BUF_SIZE &&
	    idx < p->rtbuf->ring_buf[ch_imgo].total_count &&
	    idx < p->rtbuf->ring_buf[ch_rrzo].total_count)
		mtk_isp_imgo_meta(p, dom == MTK_ISP_IRQ_CAM_D, idx);

	mtk_isp_frame_profile(p, ch, sec, usec);
	p->sof_count[ch]++;
	if (p->sof_count[ch] > 255)	/* match the 8-bit vsync counter */
		p->sof_count[ch] -= 256;
	p->rtbuf->state = ISP_RTBC_STATE_SOF;
}

/*
 * SOF-time hardware programming: reload a deferred base address and pull
 * RCNT_INC for every enqueue round that was queued since the last SOF
 * (camera_isp_D1.c:9345-9395 for P1, :9580-9610 for P1_D).
 */
static void mtk_isp_sof_program(struct mtk_isp *p, enum mtk_isp_irq_dom dom)
{
	unsigned int ch_imgo = (dom == MTK_ISP_IRQ_CAM) ? _imgo_ : _imgo_d_;
	unsigned int ch_rrzo = (dom == MTK_ISP_IRQ_CAM) ? _rrzo_ : _rrzo_d_;
	u32 base_reg[2], fbc_reg[2];
	u32 round;

	if (dom == MTK_ISP_IRQ_CAM) {
		fbc_reg[0] = ISP_REG_IMGO_FBC;
		fbc_reg[1] = ISP_REG_RRZO_FBC;
		base_reg[0] = ISP_REG_IMGO_BASE_ADDR;
		base_reg[1] = ISP_REG_RRZO_BASE_ADDR;
	} else {
		fbc_reg[0] = ISP_REG_IMGO_D_FBC;
		fbc_reg[1] = ISP_REG_RRZO_D_FBC;
		base_reg[0] = ISP_REG_IMGO_D_BASE_ADDR;
		base_reg[1] = ISP_REG_RRZO_D_BASE_ADDR;
	}

	if (p->fw.load_base[dom]) {
		ISP_RT_RING_BUF_INFO_STRUCT *r;

		r = &p->rtbuf->ring_buf[ch_imgo];
		if (r->active && r->start < MTK_ISP_RT_BUF_SIZE)
			isp_wr(p, base_reg[0], r->data[r->start].base_pAddr);
		r = &p->rtbuf->ring_buf[ch_rrzo];
		if (r->active && r->start < MTK_ISP_RT_BUF_SIZE)
			isp_wr(p, base_reg[1], r->data[r->start].base_pAddr);
		p->fw.load_base[dom] = 0;
	}

	for (round = 0; round < MTK_ISP_RT_BUF_SIZE; round++) {
		u32 v;
		int i;

		if (!p->fw.inc[dom][round])
			break;
		p->fw.inc[dom][round] = 0;
		for (i = 0; i < 2; i++) {
			unsigned int dma = i ? ch_rrzo : ch_imgo;

			if (!p->rtbuf->ring_buf[dma].active)
				continue;
			v = isp_rd(p, fbc_reg[i]);
			v |= ISP_FBC_RCNT_INC;
			isp_wr(p, fbc_reg[i], v);
			/* Matches the dsb sy that follows each FBC write in
			 * the ROM ISR (0xc04de55c, 0xc04de580). */
			mb();
		}
	}
	p->fw.cur_idx[dom] = 0;
}

/* ------------------------------------------------------------------ */
/* CAMSV DONE/SOF                                                      */
/* ------------------------------------------------------------------ */

static void mtk_isp_camsv_done_buf_time(struct mtk_isp *p, unsigned int dma,
					u32 fbc, u32 sec, u32 usec)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	enum mtk_isp_channel ch = mtk_isp_channel_of_dma(dma);
	u32 fb_num = mtk_isp_fbc_fbnum(fbc);
	u32 wcnt = mtk_isp_fbc_wcnt(fbc);
	u32 curr, next;

	if (!ring->empty_count || !ring->total_count)
		return;

	curr = ring->start;
	if (curr >= MTK_ISP_RT_BUF_SIZE || curr >= ring->total_count)
		return;
	if (ring->data[curr].bFilled != MTK_ISP_BUF_EMPTY)
		return;

	ring->data[curr].bFilled = MTK_ISP_BUF_FILLED;
	ring->start = (curr + 1) % ring->total_count;
	ring->empty_count--;
	ring->img_cnt = p->sof_count[ch];
	ring->data[curr].timeStampS = sec;
	ring->data[curr].timeStampUs = usec;

	/* CAMSV has no inner register window; the DMA destination size is the
	 * frame geometry (camera_isp_D1.c:6752-6772). */
	if (dma == _camsv_imgo_) {
		ring->data[curr].image.w = isp_rd(p, ISP_REG_IMGO_SV_XSIZE) & 0x3fff;
		ring->data[curr].image.h = isp_rd(p, ISP_REG_IMGO_SV_YSIZE) & 0x1fff;
		ring->data[curr].image.stride =
			isp_rd(p, ISP_REG_IMGO_SV_STRIDE) & 0x3fff;
		ring->data[curr].image.fmt =
			isp_rd(p, ISP_REG_CAMSV_FMT_SEL) & 0x30000;
	} else {
		ring->data[curr].image.w =
			isp_rd(p, ISP_REG_IMGO_SV_D_XSIZE) & 0x3fff;
		ring->data[curr].image.h =
			isp_rd(p, ISP_REG_IMGO_SV_D_YSIZE) & 0x1fff;
		ring->data[curr].image.stride =
			isp_rd(p, ISP_REG_IMGO_SV_D_STRIDE) & 0x3fff;
		ring->data[curr].image.fmt =
			isp_rd(p, ISP_REG_CAMSV2_FMT_SEL) & 0x30000;
	}

	/*
	 * The CAMSV DMA destination is programmed from the software ring index
	 * (camera_isp_D1.c:6683-6692).  The hardware write counter must have
	 * advanced by exactly one relative to the ring start; a mismatch means
	 * a DONE was lost and is reported rather than papered over.
	 */
	if (fb_num != 0 && (wcnt % fb_num) != ((ring->start + ring->total_count - 1) %
					       ring->total_count))
		dev_err_ratelimited(p->dev,
				    "camsv%u: WCNT %u does not follow ring start %u\n",
				    dma == _camsv_imgo_ ? 1 : 2, wcnt,
				    ring->start);

	next = ring->start;
	if (next < MTK_ISP_RT_BUF_SIZE && next < ring->total_count) {
		u32 pa = ring->data[next].base_pAddr;

		if (dma == _camsv_imgo_)
			isp_wr(p, ISP_REG_IMGO_SV_BASE_ADDR, pa);
		else
			isp_wr(p, ISP_REG_IMGO_SV_D_BASE_ADDR, pa);
	}
	p->rtbuf->state = ISP_RTBC_STATE_DONE;
}

static void mtk_isp_camsv_sof_buf_get(struct mtk_isp *p, unsigned int dma,
				      u32 fbc, u32 curr_pa, u32 sec, u32 usec,
				      bool drop)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	enum mtk_isp_irq_dom dom = mtk_isp_dom_of_dma(dma);
	enum mtk_isp_channel ch = mtk_isp_channel_of_dma(dma);
	u32 wcnt = mtk_isp_fbc_wcnt(fbc);
	u32 fbnum = mtk_isp_fbc_fbnum(fbc);
	u32 idx;

	if (p->first_sof[dom]) {
		ring->start = wcnt ? wcnt - 1 : 0;
		p->first_sof[dom] = false;
		p->ts.t_ns = (u64)sec * 1000000000ULL + (u64)usec * 1000;
	}

	p->rtbuf->dropCnt = drop ? 1 : 0;
	if (p->rtbuf->dropCnt)
		return;

	p->prv_addr[ch] = curr_pa;

	/* CAMSV SOF pulls RCNT_INC for every queued round; the hardware bug
	 * workaround writes the bit set and then clear
	 * (camera_isp_D1.c:8878-8890). */
	if (p->fw.load_base[dom]) {
		u32 reg = (dma == _camsv_imgo_) ? ISP_REG_CAMSV_IMGO_FBC :
						  ISP_REG_CAMSV2_IMGO_FBC;
		u32 v = isp_rd(p, reg);

		isp_wr(p, reg, v | ISP_FBC_RCNT_INC);
		isp_wr(p, reg, v & ~ISP_FBC_RCNT_INC);
		p->fw.load_base[dom] = 0;
	}

	if (fbnum) {
		idx = wcnt % fbnum;
		if (idx < MTK_ISP_RT_BUF_SIZE && idx < ring->total_count)
			ring->data[idx].bFilled = MTK_ISP_BUF_EMPTY;
	}

	mtk_isp_frame_profile(p, ch, sec, usec);
	p->sof_count[ch]++;
	if (p->sof_count[ch] > 255)
		p->sof_count[ch] -= 256;
	p->rtbuf->state = ISP_RTBC_STATE_SOF;
}

/* ------------------------------------------------------------------ */
/* Interrupt handlers                                                  */
/* ------------------------------------------------------------------ */

static irqreturn_t mtk_isp_irq_cam(int irq, void *data)
{
	struct mtk_isp *p = data;
	u32 st[MTK_ISP_IRQ_TYPES] = { 0 };
	u32 fbc[4], sec, usec;
	bool drop;

	mtk_isp_now(p, &sec, &usec);

	/* Read-to-clear status registers; the ROM ISR never writes them. */
	st[ISP_IRQ_TYPE_INT_P1_ST] =
		isp_rd(p, ISP_REG_INT_P1_ST) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST]);
	st[ISP_IRQ_TYPE_INT_P1_ST2] =
		isp_rd(p, ISP_REG_INT_P1_ST2) &
		p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST2];
	st[ISP_IRQ_TYPE_INT_P1_ST_D] =
		isp_rd(p, ISP_REG_INT_P1_ST_D) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST_D] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST_D]);
	st[ISP_IRQ_TYPE_INT_P1_ST2_D] =
		isp_rd(p, ISP_REG_INT_P1_ST2_D) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_P1_ST2_D] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_P1_ST2_D]);
	st[ISP_IRQ_TYPE_INT_STATUSX] =
		isp_rd(p, ISP_REG_INT_STATUSX) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_STATUSX] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUSX]);
	st[ISP_IRQ_TYPE_INT_STATUS2X] =
		isp_rd(p, ISP_REG_INT_STATUS2X) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_STATUS2X] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUS2X]);
	st[ISP_IRQ_TYPE_INT_STATUS3X] =
		isp_rd(p, ISP_REG_INT_STATUS3X) &
		(p->irq_mask[ISP_IRQ_TYPE_INT_STATUS3X] |
		 p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUS3X]);
	/*
	 * ISP_IRQ_TYPE_INT_P2_ST is deliberately left at zero: the ROM ISR
	 * does not sample 0x6c (ISP_Irq_CAM has no load of that offset), so
	 * the Pass-2 done bits are reported through ISP_IRQ_TYPE_INT_STATUSX
	 * exactly as the ROM build does.
	 */

	fbc[0] = isp_rd(p, ISP_REG_IMGO_FBC);
	fbc[1] = isp_rd(p, ISP_REG_RRZO_FBC);
	fbc[2] = isp_rd(p, ISP_REG_IMGO_D_FBC);
	fbc[3] = isp_rd(p, ISP_REG_RRZO_D_FBC);

	/* Error accounting, camera_isp_D1.c:9130-9180.  The per-port DMA
	 * error counters (g_DmaErr_p1[]) are only ever zeroed by the ROM build
	 * -- DMAErrHandler() is compiled out behind _debug_dma_err_ -- so the
	 * driver keeps them at zero and reports the aggregate through
	 * ISP_GET_INT_ERR. */
	if (st[ISP_IRQ_TYPE_INT_STATUSX] &
	    p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUSX]) {
		if (st[ISP_IRQ_TYPE_INT_STATUSX] & ISP_IRQ_P1_DMA_ERR)
			mtk_isp_dump_reg(p, "p1 dma error");
		p->int_err[MTK_ISP_IRQ_CAM] |=
			st[ISP_IRQ_TYPE_INT_STATUSX] & ~ISP_STATUSX_WARNING;
		dev_err_ratelimited(p->dev, "ISP P1 int err 0x%08x\n",
				    st[ISP_IRQ_TYPE_INT_STATUSX]);
	}
	if (st[ISP_IRQ_TYPE_INT_STATUS2X] &
	    p->irq_err_mask[ISP_IRQ_TYPE_INT_STATUS2X]) {
		p->int_err[MTK_ISP_IRQ_CAM_D] |=
			st[ISP_IRQ_TYPE_INT_STATUS2X] & ~ISP_STATUSX_WARNING;
		dev_err_ratelimited(p->dev, "ISP P1_D int err 0x%08x\n",
				    st[ISP_IRQ_TYPE_INT_STATUS2X]);
	}

	spin_lock(&p->rtbc_lock);

	/* Pass-1 DONE: publish the completed buffer. */
	if (st[ISP_IRQ_TYPE_INT_P1_ST] & ISP_IRQ_P1_PASS1_DON) {
		p->last_usec[ISP_IRQ_TYPE_INT_P1_ST][10] = usec;
		p->last_sec[ISP_IRQ_TYPE_INT_P1_ST][10] = sec;
		p->eismeta_in_sof = 0;
		mtk_isp_done_buf_time(p, MTK_ISP_IRQ_CAM, fbc[0], fbc[1],
				      sec, usec);
		p->lost_p1done_cnt = 0;
	}

	/*
	 * SOF: program the hardware first (base-address reload, RCNT_INC),
	 * then publish the frame.  This is the ordering of the ROM ISR, where
	 * the FBC write-backs at 0xc04de558/0xc04de57c precede the call into
	 * ISP_SOF_Buf_Get.
	 */
	if (st[ISP_IRQ_TYPE_INT_P1_ST] & ISP_IRQ_P1_SOF1) {
		drop = (mtk_isp_fbc_fbnum(fbc[0]) == mtk_isp_fbc_cnt(fbc[0]));
		if (drop)
			p->lost_p1done_cnt++;
		mtk_isp_sof_program(p, MTK_ISP_IRQ_CAM);
		p->eismeta[ISP_IRQ_TYPE_INT_P1_ST][p->eismeta_widx]
			.tLastSOF2P1done_sec = sec;
		p->eismeta[ISP_IRQ_TYPE_INT_P1_ST][p->eismeta_widx]
			.tLastSOF2P1done_usec = usec;
		p->eismeta_in_sof = 1;
		p->eismeta_widx = (p->eismeta_widx + 1) %
				  MTK_ISP_EISMETA_RINGSIZE;
		mtk_isp_sof_buf_get(p, MTK_ISP_IRQ_CAM, fbc, sec, usec, drop);
	}

	if (st[ISP_IRQ_TYPE_INT_P1_ST_D] & ISP_IRQ_P1_PASS1_DON) {
		p->last_usec[ISP_IRQ_TYPE_INT_P1_ST_D][10] = usec;
		p->last_sec[ISP_IRQ_TYPE_INT_P1_ST_D][10] = sec;
		p->eismeta_in_sof_d = 0;
		mtk_isp_done_buf_time(p, MTK_ISP_IRQ_CAM_D, fbc[2], fbc[3],
				      sec, usec);
		p->lost_p1d_done_cnt = 0;
	}
	if (st[ISP_IRQ_TYPE_INT_P1_ST_D] & ISP_IRQ_P1_SOF1) {
		drop = (mtk_isp_fbc_fbnum(fbc[2]) == mtk_isp_fbc_cnt(fbc[2]));
		if (drop)
			p->lost_p1d_done_cnt++;
		mtk_isp_sof_program(p, MTK_ISP_IRQ_CAM_D);
		p->eismeta[ISP_IRQ_TYPE_INT_P1_ST_D][p->eismeta_widx_d]
			.tLastSOF2P1done_sec = sec;
		p->eismeta[ISP_IRQ_TYPE_INT_P1_ST_D][p->eismeta_widx_d]
			.tLastSOF2P1done_usec = usec;
		p->eismeta_in_sof_d = 1;
		p->eismeta_widx_d = (p->eismeta_widx_d + 1) %
				    MTK_ISP_EISMETA_RINGSIZE;
		mtk_isp_sof_buf_get(p, MTK_ISP_IRQ_CAM_D, fbc, sec, usec, drop);
	}

	spin_unlock(&p->rtbc_lock);

	mtk_isp_irq_publish(p, st, sec, usec);
	wake_up_interruptible_all(&p->waitq);

	/*
	 * The ROM ISR schedules the VD and EXPDONE tasklets when the
	 * corresponding status bits are set; the hold-register flush runs from
	 * them.  The ROM combines the two TGs with a bitwise AND, which can
	 * never be true for a single-sensor session; here either TG triggers.
	 */
	if ((st[ISP_IRQ_TYPE_INT_P1_ST] & ISP_IRQ_P1_VS1) ||
	    (st[ISP_IRQ_TYPE_INT_P1_ST_D] & ISP_IRQ_P1_VS1))
		tasklet_schedule(&p->vd_tasklet);
	if ((st[ISP_IRQ_TYPE_INT_P1_ST] & ISP_IRQ_P1_EXPDON1) ||
	    (st[ISP_IRQ_TYPE_INT_P1_ST_D] & ISP_IRQ_P1_EXPDON1))
		tasklet_schedule(&p->expdone_tasklet);

	return IRQ_HANDLED;
}

static irqreturn_t mtk_isp_irq_camsv_common(int irq, void *data,
					    unsigned int dma)
{
	struct mtk_isp *p = data;
	enum mtk_isp_irq_dom dom = mtk_isp_dom_of_dma(dma);
	u32 type = (dom == MTK_ISP_IRQ_CAMSV) ? ISP_IRQ_TYPE_INT_CAMSV :
						ISP_IRQ_TYPE_INT_CAMSV2;
	u32 reg_int = (dom == MTK_ISP_IRQ_CAMSV) ? ISP_REG_CAMSV_INT :
						   ISP_REG_CAMSV2_INT;
	u32 reg_fbc = (dom == MTK_ISP_IRQ_CAMSV) ? ISP_REG_CAMSV_IMGO_FBC :
						   ISP_REG_CAMSV2_IMGO_FBC;
	u32 reg_base = (dom == MTK_ISP_IRQ_CAMSV) ? ISP_REG_IMGO_SV_BASE_ADDR :
						    ISP_REG_IMGO_SV_D_BASE_ADDR;
	u32 st[MTK_ISP_IRQ_TYPES] = { 0 };
	u32 fbc, curr_pa, sec, usec;
	bool drop;

	mtk_isp_now(p, &sec, &usec);
	fbc = isp_rd(p, reg_fbc);
	curr_pa = isp_rd(p, reg_base);
	st[type] = isp_rd(p, reg_int) & (p->irq_mask[type] |
					 p->irq_err_mask[type]);

	if (st[type] & p->irq_err_mask[type]) {
		p->int_err[dom] |= st[type] & p->irq_err_mask[type];
		dev_err_ratelimited(p->dev, "CAMSV%d int err 0x%08x\n",
				    dma == _camsv_imgo_ ? 1 : 2, st[type]);
	}

	/* The CAMSV status register is read-to-clear, like the CAM one. */
	spin_lock(&p->rtbc_lock);
	if (st[type] & ISP_IRQ_CAMSV_PASS1_DON)
		mtk_isp_camsv_done_buf_time(p, dma, fbc, sec, usec);
	if (st[type] & ISP_IRQ_CAMSV_TG_SOF1) {
		int slot = (dma == _camsv_imgo_) ? 0 : 1;

		drop = (mtk_isp_fbc_fbnum(fbc) == mtk_isp_fbc_cnt(fbc));
		/* gSof_camsvdone[], camera_isp_D1.c:8836-8843 */
		p->camsv_drop[slot] = drop;
		mtk_isp_camsv_sof_buf_get(p, dma, fbc, curr_pa, sec, usec,
					  drop);
	}
	spin_unlock(&p->rtbc_lock);

	mtk_isp_irq_publish(p, st, sec, usec);
	wake_up_interruptible_all(&p->waitq);
	return IRQ_HANDLED;
}

static irqreturn_t mtk_isp_irq_camsv(int irq, void *data)
{
	return mtk_isp_irq_camsv_common(irq, data, _camsv_imgo_);
}

static irqreturn_t mtk_isp_irq_camsv2(int irq, void *data)
{
	return mtk_isp_irq_camsv_common(irq, data, _camsv2_imgo_);
}

/* ------------------------------------------------------------------ */
/* Wait / mark / clear interrupt (camera_isp_D1.c:7712-8640)           */
/* ------------------------------------------------------------------ */

static int mtk_isp_userkey_register(struct mtk_isp *p, const char __user *name,
				    int *out_key)
{
	char local[MTK_ISP_USERKEY_LEN];
	unsigned long flags;
	int len, i, key = -1;

	if (!name)
		return -EINVAL;

	len = strnlen_user(name, MTK_ISP_USERKEY_LEN);
	if (!len)
		return -EFAULT;
	if (len > MTK_ISP_USERKEY_LEN)
		len = MTK_ISP_USERKEY_LEN;
	if (copy_from_user(local, name, len))
		return -EFAULT;
	local[MTK_ISP_USERKEY_LEN - 1] = '\0';

	spin_lock_irqsave(&p->userkey_lock, flags);
	if (p->first_unused_key >= MTK_ISP_IRQ_USERS ||
	    p->first_unused_key < 0) {
		spin_unlock_irqrestore(&p->userkey_lock, flags);
		return -ENOSPC;
	}
	for (i = 1; i < p->first_unused_key; i++) {
		if (!strcmp(p->userkey[i].name, local)) {
			key = p->userkey[i].key;
			break;
		}
	}
	if (key <= 0) {
		i = p->first_unused_key;
		memset(p->userkey[i].name, 0, MTK_ISP_USERKEY_LEN);
		strscpy(p->userkey[i].name, local, MTK_ISP_USERKEY_LEN);
		p->userkey[i].key = i;
		key = i;
		p->first_unused_key++;
		/* Flush bit sets per well-known user name
		 * (camera_isp_D1.c:7770-7795). */
		if (!strcmp(p->userkey[i].name, "VSIrq"))
			p->flush_v3[key] = ISP_IRQ_P1_VS1;
		if (!strcmp(p->userkey[i].name, "AFIrq"))
			p->flush_v3[key] = ISP_IRQ_P1_AF_DON;
		if (!strcmp(p->userkey[i].name, "EIS") ||
		    !strcmp(p->userkey[i].name, "VHDR"))
			p->flush_v3[key] = ISP_IRQ_P1_VS1 |
					   ISP_IRQ_P1_PASS1_DON;
	}
	spin_unlock_irqrestore(&p->userkey_lock, flags);

	*out_key = key;
	return key > 0 ? 0 : -ENOSPC;
}

static int mtk_isp_mark_irq(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;
	int idx;

	if (irq->UserInfo.UserKey < 1 || irq->UserInfo.UserKey >= MTK_ISP_IRQ_USERS)
		return -EINVAL;
	if (irq->UserInfo.Type < 0 || irq->UserInfo.Type >= MTK_ISP_IRQ_TYPES)
		return -EINVAL;

	idx = mtk_isp_pow_idx(irq->UserInfo.Status);
	spin_lock_irqsave(&p->irq_lock, flags);
	p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type] |=
		irq->UserInfo.Status;
	p->marked_sec[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
	p->marked_usec[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
	p->passed_cnt[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
	spin_unlock_irqrestore(&p->irq_lock, flags);
	return 0;
}

static int mtk_isp_get_mark2query(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;
	u64 ns = ktime_get_ns();
	s64 ds, dus;
	int idx, ret = 0;

	if (irq->UserInfo.UserKey < 1 || irq->UserInfo.UserKey >= MTK_ISP_IRQ_USERS)
		return -EINVAL;
	if (irq->UserInfo.Type < 0 || irq->UserInfo.Type >= MTK_ISP_IRQ_TYPES)
		return -EINVAL;

	idx = mtk_isp_pow_idx(irq->UserInfo.Status);
	spin_lock_irqsave(&p->irq_lock, flags);
	if (irq->UserInfo.Status &
	    p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type]) {
		irq->TimeInfo.passedbySigcnt =
			p->passed_cnt[irq->UserInfo.UserKey]
				     [irq->UserInfo.Type][idx];
		ds = (s64)div_u64(ns, 1000000000ULL) -
		     p->marked_sec[irq->UserInfo.UserKey]
				  [irq->UserInfo.Type][idx];
		dus = (s64)div_u64(ns % 1000000000ULL, 1000) -
		      p->marked_usec[irq->UserInfo.UserKey]
				    [irq->UserInfo.Type][idx];
		if (dus < 0) {
			ds--;
			dus += 1000000;
		}
		if (ds < 0)
			ds = 0;
		irq->TimeInfo.tMark2WaitSig_sec = (u32)ds;
		irq->TimeInfo.tMark2WaitSig_usec = (u32)dus;
		if (irq->TimeInfo.passedbySigcnt > 0) {
			ds = (s64)div_u64(ns, 1000000000ULL) -
			     p->last_sec[irq->UserInfo.Type][idx];
			dus = (s64)div_u64(ns % 1000000000ULL, 1000) -
			      p->last_usec[irq->UserInfo.Type][idx];
			if (dus < 0) {
				ds--;
				dus += 1000000;
			}
			if (ds < 0)
				ds = 0;
			irq->TimeInfo.tLastSig2GetSig_sec = (u32)ds;
			irq->TimeInfo.tLastSig2GetSig_usec = (u32)dus;
		} else {
			irq->TimeInfo.tLastSig2GetSig_sec = 0;
			irq->TimeInfo.tLastSig2GetSig_usec = 0;
		}
	} else {
		ret = -EFAULT;
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);
	return ret;
}

/* EIS metadata selection, camera_isp_D1.c:8160-8215 and :8470-8520. */
static void mtk_isp_eis_meta_locked(struct mtk_isp *p,
				    ISP_WAIT_IRQ_STRUCT *irq)
{
	int ridx;

	if (irq->UserInfo.Type >= ISP_IRQ_TYPE_INT_STATUSX ||
	    irq->SpecUser != ISP_IRQ_WAITIRQ_SPEUSER_EIS)
		return;

	if (irq->UserInfo.Type == ISP_IRQ_TYPE_INT_P1_ST) {
		if (p->eismeta_widx == 0)
			ridx = p->eismeta_in_sof ? MTK_ISP_EISMETA_RINGSIZE - 2 :
						   MTK_ISP_EISMETA_RINGSIZE - 1;
		else if (p->eismeta_widx == 1)
			ridx = p->eismeta_in_sof ? MTK_ISP_EISMETA_RINGSIZE - 1 : 0;
		else
			ridx = p->eismeta_widx - p->eismeta_in_sof - 1;
		if (ridx < 0 || ridx >= MTK_ISP_EISMETA_RINGSIZE)
			ridx = 0;
		irq->EisMeta = p->eismeta[ISP_IRQ_TYPE_INT_P1_ST][ridx];
	} else if (irq->UserInfo.Type == ISP_IRQ_TYPE_INT_P1_ST_D) {
		if (p->eismeta_widx_d == 0)
			ridx = p->eismeta_in_sof_d ?
				MTK_ISP_EISMETA_RINGSIZE - 2 :
				MTK_ISP_EISMETA_RINGSIZE - 1;
		else if (p->eismeta_widx_d == 1)
			ridx = p->eismeta_in_sof_d ?
				MTK_ISP_EISMETA_RINGSIZE - 1 : 0;
		else
			ridx = p->eismeta_widx_d - p->eismeta_in_sof_d - 1;
		if (ridx < 0 || ridx >= MTK_ISP_EISMETA_RINGSIZE)
			ridx = 0;
		irq->EisMeta = p->eismeta[ISP_IRQ_TYPE_INT_P1_ST_D][ridx];
	}
}

static int mtk_isp_wait_irq_v1(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;
	long timeout;
	int idx;

	if (irq->Type < 0 || irq->Type >= MTK_ISP_IRQ_TYPES)
		return -EINVAL;

	spin_lock_irqsave(&p->irq_lock, flags);
	switch (irq->Clear) {
	case ISP_IRQ_CLEAR_WAIT:
	case ISP_IRQ_CLEAR_STATUS:
		p->status[irq->UserNumber][irq->Type] &= ~irq->Status;
		break;
	case ISP_IRQ_CLEAR_ALL:
		p->status[irq->UserNumber][irq->Type] = 0;
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);

	if (irq->Clear == ISP_IRQ_CLEAR_STATUS)
		return 0;

	timeout = wait_event_interruptible_timeout(
		p->waitq,
		mtk_isp_irq_state(p, irq->Type, irq->UserNumber, irq->Status),
		msecs_to_jiffies(irq->Timeout));
	if (timeout == 0) {
		dev_err_ratelimited(p->dev,
				    "wait-irq timeout type=%d status=0x%08x user=%d\n",
				    irq->Type, irq->Status, irq->UserNumber);
		return -EFAULT;
	}
	if (timeout < 0)
		return -ERESTARTSYS;

	idx = mtk_isp_pow_idx(irq->Status);
	spin_lock_irqsave(&p->irq_lock, flags);
	mtk_isp_eis_meta_locked(p, irq);
	irq->TimeInfo.tLastSig_sec = p->last_sec[irq->Type][idx];
	irq->TimeInfo.tLastSig_usec = p->last_usec[irq->Type][idx];
	p->status[irq->UserNumber][irq->Type] &= ~irq->Status;
	spin_unlock_irqrestore(&p->irq_lock, flags);
	return 0;
}

static int mtk_isp_wait_irq_v3(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;
	long timeout;
	u64 ns, get_ns;
	s64 ds, dus;
	int idx, ret = 0;
	bool marked, freeze = false;

	if (irq->UserInfo.Type < 0 || irq->UserInfo.Type >= MTK_ISP_IRQ_TYPES)
		return -EINVAL;
	if (irq->UserInfo.UserKey < 0 ||
	    irq->UserInfo.UserKey >= MTK_ISP_IRQ_USERS)
		return -EINVAL;

	idx = mtk_isp_pow_idx(irq->UserInfo.Status);
	get_ns = ktime_get_ns();

	if (irq->Clear == ISP_IRQ_CLEAR_STATUS) {
		spin_lock_irqsave(&p->irq_lock, flags);
		p->status[irq->UserInfo.UserKey][irq->UserInfo.Type] &=
			~irq->UserInfo.Status;
		spin_unlock_irqrestore(&p->irq_lock, flags);
		return 0;
	}

	spin_lock_irqsave(&p->irq_lock, flags);
	marked = !!(p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type] &
		    irq->UserInfo.Status);
	if (marked) {
		/* Freeze the passed-by counter when the wait request arrived
		 * before the signal (camera_isp_D1.c:8360-8395). */
		freeze = !(p->status[irq->UserInfo.UserKey]
					[irq->UserInfo.Type] &
			   irq->UserInfo.Status);
	} else {
		switch (irq->Clear) {
		case ISP_IRQ_CLEAR_WAIT:
			p->status[irq->UserInfo.UserKey][irq->UserInfo.Type] &=
				~irq->UserInfo.Status;
			break;
		case ISP_IRQ_CLEAR_ALL:
			p->status[irq->UserInfo.UserKey][irq->UserInfo.Type] = 0;
			break;
		default:
			break;
		}
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);

	timeout = wait_event_interruptible_timeout(
		p->waitq,
		mtk_isp_irq_state(p, irq->UserInfo.Type,
				  irq->UserInfo.UserKey,
				  irq->UserInfo.Status),
		msecs_to_jiffies(irq->Timeout));
	if (timeout == 0) {
		dev_err_ratelimited(p->dev,
				    "wait-irq v3 timeout type=%d status=0x%08x key=%d\n",
				    irq->UserInfo.Type, irq->UserInfo.Status,
				    irq->UserInfo.UserKey);
		ret = -EFAULT;
		goto out;
	}
	if (timeout < 0) {
		ret = -ERESTARTSYS;
		goto out;
	}

	ns = ktime_get_ns();
	spin_lock_irqsave(&p->irq_lock, flags);
	mtk_isp_eis_meta_locked(p, irq);
	if (irq->UserInfo.Status &
	    p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type]) {
		ds = (s64)div_u64(get_ns, 1000000000ULL) -
		     p->marked_sec[irq->UserInfo.UserKey]
				  [irq->UserInfo.Type][idx];
		dus = (s64)div_u64(get_ns % 1000000000ULL, 1000) -
		      p->marked_usec[irq->UserInfo.UserKey]
				    [irq->UserInfo.Type][idx];
		if (dus < 0) {
			ds--;
			dus += 1000000;
		}
		if (ds < 0)
			ds = 0;
		irq->TimeInfo.tMark2WaitSig_sec = (u32)ds;
		irq->TimeInfo.tMark2WaitSig_usec = (u32)dus;
		ds = (s64)div_u64(ns, 1000000000ULL) -
		     p->last_sec[irq->UserInfo.Type][idx];
		dus = (s64)div_u64(ns % 1000000000ULL, 1000) -
		      p->last_usec[irq->UserInfo.Type][idx];
		if (dus < 0) {
			ds--;
			dus += 1000000;
		}
		if (ds < 0)
			ds = 0;
		irq->TimeInfo.tLastSig2GetSig_sec = (u32)ds;
		irq->TimeInfo.tLastSig2GetSig_usec = (u32)dus;
		irq->TimeInfo.passedbySigcnt =
			p->passed_cnt[irq->UserInfo.UserKey]
				     [irq->UserInfo.Type][idx] -
			(freeze ? 1 : 0);
		if (irq->TimeInfo.passedbySigcnt < 0)
			irq->TimeInfo.passedbySigcnt = 0;
	}
	irq->TimeInfo.tLastSig_sec = p->last_sec[irq->UserInfo.Type][idx];
	irq->TimeInfo.tLastSig_usec = p->last_usec[irq->UserInfo.Type][idx];
	p->status[irq->UserInfo.UserKey][irq->UserInfo.Type] &=
		~irq->UserInfo.Status;
	spin_unlock_irqrestore(&p->irq_lock, flags);

out:
	/* Clear the mark flag / marked time / passed-by count
	 * (camera_isp_D1.c:8590-8610). */
	spin_lock_irqsave(&p->irq_lock, flags);
	if (irq->UserInfo.Status &
	    p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type]) {
		p->marked[irq->UserInfo.UserKey][irq->UserInfo.Type] &=
			~irq->UserInfo.Status;
		p->marked_sec[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
		p->marked_usec[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
		p->passed_cnt[irq->UserInfo.UserKey][irq->UserInfo.Type][idx] = 0;
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);
	return ret;
}

static int mtk_isp_flush_irq(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_lock, flags);
	p->status[irq->UserInfo.UserKey][irq->UserInfo.Type] |=
		irq->UserInfo.Status;
	spin_unlock_irqrestore(&p->irq_lock, flags);
	wake_up_interruptible_all(&p->waitq);
	return 0;
}

static int mtk_isp_flush_irq_v1(struct mtk_isp *p, ISP_WAIT_IRQ_STRUCT *irq)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_lock, flags);
	p->status[irq->UserNumber][irq->Type] |= irq->Status;
	spin_unlock_irqrestore(&p->irq_lock, flags);
	wake_up_interruptible_all(&p->waitq);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pass-2 early-done buffer queue (camera_isp_D1.c:7084-7712)           */
/* ------------------------------------------------------------------ */

/* Advance the ring global pointer (ISP_ED_BufQue_Update_GPtr). */
static void mtk_isp_ed_update_gptr(struct mtk_isp *p)
{
	ISP_ED_BUF_STATE_ENUM st;
	int cnt, i = 0, tmp;
	bool stop = false;

	st = p->ed_ring[p->ed_r_cur].state;
	if (p->ed_r_first <= p->ed_r_last)
		cnt = p->ed_r_last - p->ed_r_first;
	else
		cnt = MTK_ISP_ED_RING - p->ed_r_first + p->ed_r_last;

	tmp = p->ed_r_cur;
	switch (st) {
	case ISP_ED_BUF_STATE_ENQUE:
		p->ed_ring[p->ed_r_cur].state = ISP_ED_BUF_STATE_RUNNING;
		break;
	case ISP_ED_BUF_STATE_WAIT_DEQUE_FAIL:
	case ISP_ED_BUF_STATE_DEQUE_SUCCESS:
	case ISP_ED_BUF_STATE_DEQUE_FAIL:
		do {
			tmp = (tmp + 1) % MTK_ISP_ED_RING;
			if (p->ed_ring[tmp].state == ISP_ED_BUF_STATE_ENQUE ||
			    p->ed_ring[tmp].state == ISP_ED_BUF_STATE_RUNNING) {
				p->ed_ring[tmp].state = ISP_ED_BUF_STATE_RUNNING;
				p->ed_r_cur = tmp;
				stop = true;
			}
			i++;
		} while (i < cnt && !stop);
		if (!stop && i == cnt)
			p->ed_r_cur = p->ed_r_last;
		break;
	default:
		break;
	}
}

/* First matching node (ISP_ED_BufQue_Get_FirstMatBuf). */
static int mtk_isp_ed_first_ring(struct mtk_isp *p, u32 pid, u32 caller)
{
	int i;

	for (i = 0; i < MTK_ISP_ED_RING; i++)
		if (p->ed_ring[i].process_id == pid &&
		    p->ed_ring[i].caller_id == caller &&
		    p->ed_ring[i].state != ISP_ED_BUF_STATE_NONE)
			return i;
	return -1;
}

static int mtk_isp_ed_first_mgr(struct mtk_isp *p, u32 pid, u32 caller,
				s32 dup, bool check_dup)
{
	int i;

	for (i = 0; i < MTK_ISP_ED_MGR; i++) {
		if (p->ed_mgr[i].process_id != pid ||
		    p->ed_mgr[i].caller_id != caller ||
		    p->ed_mgr[i].dup_cq_idx == -1)
			continue;
		if (!check_dup)
			return i;
		if (p->ed_mgr[i].dup_cq_idx == dup &&
		    p->ed_mgr[i].dequed_num < p->burst_qnum)
			return i;
	}
	return -1;
}

static void mtk_isp_ed_erase_ring(struct mtk_isp *p, int idx)
{
	int cnt, i = 0, tmp;
	bool stop = false;

	p->ed_ring[idx].process_id = 0;
	p->ed_ring[idx].caller_id = 0;
	p->ed_ring[idx].dup_cq_idx = -1;
	p->ed_ring[idx].state = ISP_ED_BUF_STATE_NONE;
	if (p->ed_remain)
		p->ed_remain--;

	if (p->ed_ring[p->ed_r_first].state != ISP_ED_BUF_STATE_NONE)
		return;

	if (p->ed_r_first <= p->ed_r_last)
		cnt = p->ed_r_last - p->ed_r_first;
	else
		cnt = MTK_ISP_ED_RING - p->ed_r_first + p->ed_r_last;
	tmp = p->ed_r_first;
	do {
		tmp = (tmp + 1) % MTK_ISP_ED_RING;
		if (p->ed_ring[tmp].state != ISP_ED_BUF_STATE_NONE) {
			stop = true;
			p->ed_r_first = tmp;
		}
		i++;
	} while (i < cnt && !stop);
	if (!stop && i == cnt)
		p->ed_r_first = p->ed_r_last;
}

static void mtk_isp_ed_erase_mgr(struct mtk_isp *p, int idx)
{
	int cnt, i = 0, tmp;
	bool stop = false;

	p->ed_mgr[idx].process_id = 0;
	p->ed_mgr[idx].caller_id = 0;
	p->ed_mgr[idx].dup_cq_idx = -1;
	p->ed_mgr[idx].dequed_num = 0;

	if (p->ed_mgr[p->ed_m_first].dup_cq_idx != -1)
		return;

	if (p->ed_m_first <= p->ed_m_last)
		cnt = p->ed_m_last - p->ed_m_first;
	else
		cnt = MTK_ISP_ED_MGR - p->ed_m_first + p->ed_m_last;
	tmp = p->ed_m_first;
	do {
		tmp = (tmp + 1) % MTK_ISP_ED_MGR;
		if (p->ed_mgr[tmp].dup_cq_idx != -1) {
			stop = true;
			p->ed_m_first = tmp;
		}
		i++;
	} while (i < cnt && !stop);
	if (!stop && i == cnt)
		p->ed_m_first = p->ed_m_last;
}

static bool mtk_isp_ed_deque_ready(struct mtk_isp *p, int idx)
{
	unsigned long flags;
	bool ret;

	if (idx < 0 || idx >= MTK_ISP_ED_RING)
		return true;
	spin_lock_irqsave(&p->edbuf_lock, flags);
	ret = p->ed_ring[idx].state == ISP_ED_BUF_STATE_RUNNING;
	spin_unlock_irqrestore(&p->edbuf_lock, flags);
	return ret;
}

static bool mtk_isp_ed_frame_ready(struct mtk_isp *p, int idx)
{
	unsigned long flags;
	bool ret;

	if (idx < 0 || idx >= MTK_ISP_ED_MGR)
		return true;
	spin_lock_irqsave(&p->edbuf_lock, flags);
	ret = p->ed_mgr[idx].dequed_num == p->burst_qnum;
	spin_unlock_irqrestore(&p->edbuf_lock, flags);
	return ret;
}

static int mtk_isp_ed_ctrl(struct mtk_isp *p, ISP_ED_BUFQUE_STRUCT *req,
			   u32 pid)
{
	unsigned long flags;
	int i, idx = -1, idx2 = -1;
	long rest;

	switch (req->ctrl) {
	case ISP_ED_BUFQUE_CTRL_ENQUE_FRAME:
		spin_lock_irqsave(&p->edbuf_lock, flags);
		if (p->ed_m_last != -1 &&
		    (p->ed_m_last + 1) % MTK_ISP_ED_MGR == p->ed_m_first) {
			spin_unlock_irqrestore(&p->edbuf_lock, flags);
			return -EFAULT;
		}
		if (p->ed_r_first == p->ed_r_last &&
		    p->ed_ring[p->ed_r_first].state == ISP_ED_BUF_STATE_NONE) {
			p->ed_r_last = (p->ed_r_last + 1) % MTK_ISP_ED_RING;
			p->ed_r_first = p->ed_r_last;
			p->ed_r_cur = p->ed_r_last;
		} else if (p->ed_r_cur == p->ed_r_last &&
			   p->ed_ring[p->ed_r_cur].state ==
				   ISP_ED_BUF_STATE_NONE) {
			p->ed_r_last = (p->ed_r_last + 1) % MTK_ISP_ED_RING;
			p->ed_r_cur = p->ed_r_last;
		} else {
			p->ed_r_last = (p->ed_r_last + 1) % MTK_ISP_ED_RING;
		}
		p->ed_ring[p->ed_r_last].process_id = pid;
		p->ed_ring[p->ed_r_last].caller_id = req->callerID;
		p->ed_ring[p->ed_r_last].dup_cq_idx = req->p2dupCQIdx;
		p->ed_ring[p->ed_r_last].state = ISP_ED_BUF_STATE_ENQUE;
		p->ed_remain++;

		if (req->p2burstQIdx == 0) {
			if (p->ed_m_first == p->ed_m_last &&
			    p->ed_mgr[p->ed_m_first].dup_cq_idx == -1) {
				p->ed_m_last = (p->ed_m_last + 1) %
					       MTK_ISP_ED_MGR;
				p->ed_m_first = p->ed_m_last;
			} else {
				p->ed_m_last = (p->ed_m_last + 1) %
					       MTK_ISP_ED_MGR;
			}
			p->ed_mgr[p->ed_m_last].process_id = pid;
			p->ed_mgr[p->ed_m_last].caller_id = req->callerID;
			p->ed_mgr[p->ed_m_last].dup_cq_idx = req->p2dupCQIdx;
			p->ed_mgr[p->ed_m_last].dequed_num = 0;
		}
		mtk_isp_ed_update_gptr(p);
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		wake_up_interruptible_all(&p->ed_wait_deque);
		return 0;

	case ISP_ED_BUFQUE_CTRL_WAIT_DEQUE:
		spin_lock_irqsave(&p->edbuf_lock, flags);
		idx = mtk_isp_ed_first_ring(p, pid, req->callerID);
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		if (idx < 0)
			return -EFAULT;
		rest = wait_event_interruptible_timeout(
			p->ed_wait_deque, mtk_isp_ed_deque_ready(p, idx),
			usecs_to_jiffies(MTK_ISP_ED_WAIT_US));
		if (rest == 0)
			return -EFAULT;
		if (rest < 0)
			return -ERESTARTSYS;
		return 0;

	case ISP_ED_BUFQUE_CTRL_DEQUE_SUCCESS:
	case ISP_ED_BUFQUE_CTRL_DEQUE_FAIL:
		spin_lock_irqsave(&p->edbuf_lock, flags);
		idx2 = mtk_isp_ed_first_ring(p, pid, req->callerID);
		idx = mtk_isp_ed_first_mgr(p, pid, req->callerID,
					   req->p2dupCQIdx, true);
		if (idx2 < 0 || idx < 0) {
			spin_unlock_irqrestore(&p->edbuf_lock, flags);
			return -EFAULT;
		}
		p->ed_ring[idx2].state =
			(req->ctrl == ISP_ED_BUFQUE_CTRL_DEQUE_SUCCESS) ?
			ISP_ED_BUF_STATE_DEQUE_SUCCESS :
			ISP_ED_BUF_STATE_DEQUE_FAIL;
		p->ed_mgr[idx].dequed_num++;
		mtk_isp_ed_update_gptr(p);
		mtk_isp_ed_erase_ring(p, idx2);
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		wake_up_interruptible_all(&p->ed_wait_frame);
		wake_up_interruptible_all(&p->ed_wait_deque);
		return 0;

	case ISP_ED_BUFQUE_CTRL_WAIT_FRAME:
		spin_lock_irqsave(&p->edbuf_lock, flags);
		idx = mtk_isp_ed_first_mgr(p, pid, req->callerID, -1, false);
		if (idx < 0) {
			spin_unlock_irqrestore(&p->edbuf_lock, flags);
			return -EFAULT;
		}
		if (p->ed_mgr[idx].dequed_num == p->burst_qnum) {
			mtk_isp_ed_erase_mgr(p, idx);
			spin_unlock_irqrestore(&p->edbuf_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		rest = wait_event_interruptible_timeout(
			p->ed_wait_frame, mtk_isp_ed_frame_ready(p, idx),
			usecs_to_jiffies(req->timeoutUs));
		if (rest == 0)
			return -EFAULT;
		if (rest < 0)
			return -ERESTARTSYS;
		spin_lock_irqsave(&p->edbuf_lock, flags);
		idx = mtk_isp_ed_first_mgr(p, pid, req->callerID, -1, false);
		if (idx >= 0)
			mtk_isp_ed_erase_mgr(p, idx);
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		return 0;

	case ISP_ED_BUFQUE_CTRL_WAKE_WAITFRAME:
		wake_up_interruptible_all(&p->ed_wait_frame);
		return 0;

	case ISP_ED_BUFQUE_CTRL_CLAER_ALL:
		spin_lock_irqsave(&p->edbuf_lock, flags);
		for (i = 0; i < MTK_ISP_ED_RING; i++) {
			p->ed_ring[i].process_id = 0;
			p->ed_ring[i].caller_id = 0;
			p->ed_ring[i].dup_cq_idx = -1;
			p->ed_ring[i].state = ISP_ED_BUF_STATE_NONE;
		}
		p->ed_r_first = 0;
		p->ed_r_cur = 0;
		p->ed_r_last = -1;
		for (i = 0; i < MTK_ISP_ED_MGR; i++) {
			p->ed_mgr[i].process_id = 0;
			p->ed_mgr[i].caller_id = 0;
			p->ed_mgr[i].dup_cq_idx = -1;
			p->ed_mgr[i].dequed_num = 0;
		}
		p->ed_m_first = 0;
		p->ed_m_last = -1;
		p->ed_remain = 0;
		spin_unlock_irqrestore(&p->edbuf_lock, flags);
		return 0;

	case ISP_ED_BUFQUE_CTRL_MAX:
		return 0;
	}
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Real-time buffer control (camera_isp_D1.c:4115-6158)                */
/* ------------------------------------------------------------------ */

static void mtk_isp_dma_default_regs(unsigned int dma, u32 *fbc, u32 *base)
{
	switch (dma) {
	case _imgo_:
		*fbc = ISP_REG_IMGO_FBC;
		*base = ISP_REG_IMGO_BASE_ADDR;
		break;
	case _rrzo_:
		*fbc = ISP_REG_RRZO_FBC;
		*base = ISP_REG_RRZO_BASE_ADDR;
		break;
	case _imgo_d_:
		*fbc = ISP_REG_IMGO_D_FBC;
		*base = ISP_REG_IMGO_D_BASE_ADDR;
		break;
	case _rrzo_d_:
		*fbc = ISP_REG_RRZO_D_FBC;
		*base = ISP_REG_RRZO_D_BASE_ADDR;
		break;
	case _camsv_imgo_:
		*fbc = ISP_REG_CAMSV_IMGO_FBC;
		*base = ISP_REG_IMGO_SV_BASE_ADDR;
		break;
	case _camsv2_imgo_:
		*fbc = ISP_REG_CAMSV2_IMGO_FBC;
		*base = ISP_REG_IMGO_SV_D_BASE_ADDR;
		break;
	default:
		*fbc = 0;
		*base = 0;
		break;
	}
}

/* ISP_RTBC_ENQUE(): add a buffer to a stopped ring and arm its base address. */
static int mtk_isp_rtbc_enque(struct mtk_isp *p, unsigned int dma,
			      const ISP_RT_BUF_INFO_STRUCT *info)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	unsigned int i, index = 0;
	u32 base_reg, fbc_reg;
	bool found = false;

	if (ring->total_count == MTK_ISP_RT_BUF_SIZE)
		return -EFAULT;

	for (i = 0; i < MTK_ISP_RT_BUF_SIZE; i++) {
		if (ring->data[i].base_pAddr == info->base_pAddr) {
			found = true;
			break;
		}
		if (!ring->data[i].base_pAddr)
			break;
	}

	if (found) {
		if (ring->data[i].bFilled != MTK_ISP_BUF_EMPTY) {
			ring->data[i].bFilled = MTK_ISP_BUF_EMPTY;
			ring->empty_count++;
			index = i;
		}
	} else {
		index = ring->total_count % MTK_ISP_RT_BUF_SIZE;
		ring->data[index].memID = info->memID;
		ring->data[index].size = info->size;
		ring->data[index].base_vAddr = info->base_vAddr;
		ring->data[index].base_pAddr = info->base_pAddr;
		ring->data[index].bFilled = MTK_ISP_BUF_EMPTY;
		ring->data[index].bufIdx = info->bufIdx;
		ring->total_count++;
		ring->empty_count++;
	}

	if (ring->empty_count == 1) {
		mtk_isp_dma_default_regs(dma, &fbc_reg, &base_reg);
		/* Set the base address before VF is enabled
		 * (camera_isp_D1.c:4243-4258). */
		isp_wr(p, base_reg, ring->data[index].base_pAddr);
		ring->pre_empty_count = ring->empty_count;
	}
	return 0;
}

/* Exchange one buffer in place; used while the port is streaming. */
static int mtk_isp_rtbc_exchange(struct mtk_isp *p, unsigned int dma,
				 const ISP_RT_BUF_INFO_STRUCT *old,
				 const ISP_RT_BUF_INFO_STRUCT *new)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	unsigned int i;

	for (i = 0; i < MTK_ISP_RT_BUF_SIZE; i++) {
		if (new->bufIdx != 0xffff) {
			if (ring->data[i].bufIdx == new->bufIdx)
				break;
		} else if (ring->data[i].base_pAddr == old->base_pAddr) {
			break;
		}
	}
	if (i == MTK_ISP_RT_BUF_SIZE)
		return -EFAULT;

	ring->data[i].memID = new->memID;
	ring->data[i].size = new->size;
	ring->data[i].base_pAddr = new->base_pAddr;
	ring->data[i].base_vAddr = new->base_vAddr;
	ring->data[i].bFilled = MTK_ISP_BUF_EMPTY;
	ring->data[i].image.frm_cnt = MTK_ISP_INVALID_FRM_CNT;
	if (ring->empty_count < ring->total_count)
		ring->empty_count++;
	else
		return -EFAULT;

	/* A replacement queued while the FBC window is full needs the base
	 * address reloaded at the next SOF (camera_isp_D1.c:5120-5140). */
	{
		u32 fbc_reg, base_reg;
		u32 fbc;

		mtk_isp_dma_default_regs(dma, &fbc_reg, &base_reg);
		fbc = isp_rd(p, fbc_reg);
		if (dma == _camsv_imgo_ || dma == _camsv2_imgo_) {
			/* CAMSV workaround: pulse RCNT_INC and clear it again. */
			isp_wr(p, fbc_reg, fbc | ISP_FBC_RCNT_INC);
			isp_wr(p, fbc_reg, fbc & ~ISP_FBC_RCNT_INC);
		} else {
			if (mtk_isp_fbc_fbnum(fbc) == mtk_isp_fbc_cnt(fbc) ||
			    mtk_isp_fbc_fbnum(fbc) - 1 ==
				    mtk_isp_fbc_cnt(fbc))
				p->fw.load_base[mtk_isp_dom_of_dma(dma)] = 1;
			isp_wr(p, fbc_reg, fbc | ISP_FBC_RCNT_INC);
		}
	}
	return 0;
}

/*
 * Queue one RCNT_INC round for a domain.  The queue holds at most
 * ISP_RT_BUF_SIZE rounds, which is also the ring depth; a round that arrives
 * with the queue full is dropped with a message instead of overflowing the
 * array (the downstream code indexes INC[irqT][curIdx++] without a bound).
 */
static void mtk_isp_rcnt_push(struct mtk_isp *p, enum mtk_isp_irq_dom dom)
{
	if (p->fw.cur_idx[dom] >= MTK_ISP_RT_BUF_SIZE) {
		dev_err_ratelimited(p->dev,
				    "rtbc: RCNT_INC queue full on domain %d\n",
				    dom);
		return;
	}
	p->fw.inc[dom][p->fw.cur_idx[dom]++] = 1;
}

/*
 * ISP_RTBC_ENQUE / ENQUE_IMD while streaming: re-arm the slot that carries
 * this address and queue one RCNT_INC round when every active port of the
 * domain has a buffer for it (camera_isp_D1.c:5160-5330).
 */
static void mtk_isp_rtbc_rcnt_queue(struct mtk_isp *p, unsigned int dma)
{
	enum mtk_isp_irq_dom dom = mtk_isp_dom_of_dma(dma);
	unsigned int ch_imgo = (dom == MTK_ISP_IRQ_CAM) ? _imgo_ :
			       (dom == MTK_ISP_IRQ_CAM_D) ? _imgo_d_ :
			       (dom == MTK_ISP_IRQ_CAMSV) ? _camsv_imgo_ :
							    _camsv2_imgo_;
	unsigned int ch_rrzo = (dom == MTK_ISP_IRQ_CAM) ? _rrzo_ :
			       (dom == MTK_ISP_IRQ_CAM_D) ? _rrzo_d_ : ch_imgo;
	u32 z;

	p->dma_en_rec[dma][p->fw.dma_idx[dma]] = 1;
	p->fw.dma_idx[dma] = (p->fw.dma_idx[dma] + 1) % MTK_ISP_RT_BUF_SIZE;

	if (dom == MTK_ISP_IRQ_CAMSV || dom == MTK_ISP_IRQ_CAMSV2) {
		if (!p->rtbuf->ring_buf[dma].active)
			return;
		if (p->dma_en_rec[dma][p->fw.rd_idx[dom]]) {
			mtk_isp_rcnt_push(p, dom);
			p->dma_en_rec[dma][p->fw.rd_idx[dom]] = 0;
			p->fw.rd_idx[dom] = (p->fw.rd_idx[dom] + 1) %
					    MTK_ISP_RT_BUF_SIZE;
		}
		return;
	}

	if (p->rtbuf->ring_buf[ch_imgo].active &&
	    p->rtbuf->ring_buf[ch_rrzo].active) {
		for (z = 0; z < MTK_ISP_RT_BUF_SIZE; z++) {
			if (p->dma_en_rec[ch_imgo][p->fw.rd_idx[dom]] &&
			    p->dma_en_rec[ch_rrzo][p->fw.rd_idx[dom]]) {
				mtk_isp_rcnt_push(p, dom);
				p->dma_en_rec[ch_imgo][p->fw.rd_idx[dom]] = 0;
				p->dma_en_rec[ch_rrzo][p->fw.rd_idx[dom]] = 0;
				p->fw.rd_idx[dom] = (p->fw.rd_idx[dom] + 1) %
						    MTK_ISP_RT_BUF_SIZE;
			} else {
				break;
			}
		}
		return;
	}

	/* Single active port: one round per enqueued buffer. */
	for (z = 0; z < MTK_ISP_RT_BUF_SIZE; z++) {
		if (p->dma_en_rec[dma][p->fw.rd_idx[dom]]) {
			mtk_isp_rcnt_push(p, dom);
			p->dma_en_rec[dma][p->fw.rd_idx[dom]] = 0;
			p->fw.rd_idx[dom] = (p->fw.rd_idx[dom] + 1) %
					    MTK_ISP_RT_BUF_SIZE;
		} else {
			break;
		}
	}
}

/* ISP_RTBC_DEQUE(): hand a filled buffer to userspace. */
static int mtk_isp_rtbc_deque(struct mtk_isp *p, unsigned int dma,
			      ISP_DEQUE_BUF_INFO_STRUCT *out)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	enum mtk_isp_channel ch = mtk_isp_channel_of_dma(dma);
	unsigned int ibuf;
	u32 magic;

	if (!ring->total_count)
		return -EFAULT;

	ibuf = ring->read_idx;
	if (ibuf >= MTK_ISP_RT_BUF_SIZE)
		ibuf = 0;
	if (ring->data[ibuf].bFilled != MTK_ISP_BUF_FILLED) {
		/* Nothing completed yet: report it instead of handing out an
		 * empty slot as if it were a frame. */
		return -EAGAIN;
	}
	ring->read_idx = (ibuf + 1) % ring->total_count;

	out->count = ISP_P1_DEQUE_CNT;
	out->data[0] = ring->data[ibuf];

	magic = out->data[0].image.m_num_0;
	if (magic & MTK_ISP_DUMMY_MAGIC)
		magic &= ~MTK_ISP_DUMMY_MAGIC;
	if (out->data[0].image.frm_cnt == MTK_ISP_INVALID_FRM_CNT ||
	    p->last_mnum[dma] > magic) {
		if (!(out->data[0].image.m_num_0 & MTK_ISP_DUMMY_MAGIC))
			out->data[0].image.m_num_0 |= MTK_ISP_UNCERTAIN_MAGIC;
	} else {
		p->last_mnum[dma] = magic;
	}

	ring->data[ibuf].bFilled = MTK_ISP_BUF_LOCKED;
	out->sof_cnt = p->sof_count[ch];
	out->img_cnt = ring->img_cnt;

	if (p->prv_tstamp_s[dma] > out->data[0].timeStampS ||
	    (p->prv_tstamp_s[dma] == out->data[0].timeStampS &&
	     p->prv_tstamp_us[dma] >= out->data[0].timeStampUs))
		dev_err_ratelimited(p->dev,
				    "rtbc: dma %u timestamp rollback %u.%06u -> %u.%06u\n",
				    dma, p->prv_tstamp_s[dma],
				    p->prv_tstamp_us[dma],
				    out->data[0].timeStampS,
				    out->data[0].timeStampUs);
	p->prv_tstamp_s[dma] = out->data[0].timeStampS;
	p->prv_tstamp_us[dma] = out->data[0].timeStampUs;

	return 0;
}

/* TG VF (viewfinder/streaming) enable state of one domain. */
static bool mtk_isp_vf_up(struct mtk_isp *p, enum mtk_isp_irq_dom dom)
{
	switch (dom) {
	case MTK_ISP_IRQ_CAM:
		return !!(isp_rd(p, ISP_REG_TG_VF_CON) & 0x1);
	case MTK_ISP_IRQ_CAM_D:
		return !!(isp_rd(p, ISP_REG_TG2_VF_CON) & 0x1);
	case MTK_ISP_IRQ_CAMSV:
		return !!(isp_rd(p, ISP_REG_CAMSV_TG_VF_CON) & 0x1);
	case MTK_ISP_IRQ_CAMSV2:
		return !!(isp_rd(p, ISP_REG_CAMSV_TG2_VF_CON) & 0x1);
	}
	return false;
}

/* ISP_RT_BUF_CTRL_CLEAR, camera_isp_D1.c:5934-6090. */
static void mtk_isp_rtbc_clear(struct mtk_isp *p, unsigned int dma)
{
	ISP_RT_RING_BUF_INFO_STRUCT *ring = &p->rtbuf->ring_buf[dma];
	enum mtk_isp_irq_dom dom = mtk_isp_dom_of_dma(dma);
	int i;

	ring->active = 0;
	memset(ring, 0, sizeof(*ring));
	for (i = 0; i < MTK_ISP_RT_BUF_SIZE; i++)
		ring->data[i].image.frm_cnt = MTK_ISP_INVALID_FRM_CNT;
	p->prv_tstamp_s[dma] = 0;
	p->prv_tstamp_us[dma] = 0;
	p->last_mnum[dma] = 0;
	memset(p->dma_en_rec[dma], 0, MTK_ISP_RT_BUF_SIZE);
	p->fw.dma_idx[dma] = 0;

	/* Reset the domain bookkeeping once no port of the domain is active. */
	if ((dom == MTK_ISP_IRQ_CAM &&
	     !p->rtbuf->ring_buf[_imgo_].active &&
	     !p->rtbuf->ring_buf[_rrzo_].active) ||
	    (dom == MTK_ISP_IRQ_CAM_D &&
	     !p->rtbuf->ring_buf[_imgo_d_].active &&
	     !p->rtbuf->ring_buf[_rrzo_d_].active) ||
	    (dom == MTK_ISP_IRQ_CAMSV &&
	     !p->rtbuf->ring_buf[_camsv_imgo_].active) ||
	    (dom == MTK_ISP_IRQ_CAMSV2 &&
	     !p->rtbuf->ring_buf[_camsv2_imgo_].active)) {
		enum mtk_isp_channel ch = mtk_isp_channel_of_dma(dma);

		p->sof_count[ch] = 0;
		p->start_time[ch] = 0;
		p->avg_frame_time[ch] = 0;
		p->first_sof[dom] = true;
		p->prv_addr[ch] = 0;
		p->int_err[dom] = 0;
		p->fw.load_base[dom] = 0;
		p->fw.cur_idx[dom] = 0;
		memset(p->fw.inc[dom], 0, sizeof(p->fw.inc[dom]));
		p->fw.rd_idx[dom] = 0;
		if (dom == MTK_ISP_IRQ_CAM) {
			memset(&p->ts, 0, sizeof(p->ts));
			memset(&p->eismeta[ISP_IRQ_TYPE_INT_P1_ST][0], 0,
			       sizeof(p->eismeta[0]));
			p->eismeta_ridx = 0;
			p->eismeta_widx = 0;
			p->eismeta_in_sof = 0;
			memset(p->dma_err, 0, sizeof(p->dma_err));
			p->lost_p1done_cnt = 0;
		}
		if (dom == MTK_ISP_IRQ_CAM_D) {
			memset(&p->eismeta[ISP_IRQ_TYPE_INT_P1_ST_D][0], 0,
			       sizeof(p->eismeta[0]));
			p->eismeta_ridx_d = 0;
			p->eismeta_widx_d = 0;
			p->eismeta_in_sof_d = 0;
			p->lost_p1d_done_cnt = 0;
		}
	}

	for (i = 0; i < MTK_ISP_DMA_NUM; i++)
		if (p->rtbuf->ring_buf[i].active)
			return;
	p->rtbuf->dropCnt = 0;
	p->rtbuf->state = ISP_RTBC_STATE_INIT;
}

static int mtk_isp_rtbuf_ctrl(struct mtk_isp *p, unsigned long arg)
{
	ISP_BUFFER_CTRL_STRUCT ctrl;
	ISP_DEQUE_BUF_INFO_STRUCT deque;
	ISP_RT_BUF_INFO_STRUCT info;
	unsigned int dma;
	u32 wait = 1, size = 0, reg_val;
	u8 array[MTK_ISP_DMA_NUM];
	unsigned long flags;
	bool vf_up;
	int ret = 0;

	if (copy_from_user(&ctrl, (void __user *)arg, sizeof(ctrl)))
		return -EFAULT;

	dma = ctrl.buf_id;
	if (dma >= MTK_ISP_DMA_NUM && ctrl.ctrl != ISP_RT_BUF_CTRL_DMA_EN)
		return -EFAULT;

	switch (ctrl.ctrl) {
	case ISP_RT_BUF_CTRL_EXCHANGE_ENQUE:
		/* No-op in the downstream kernel, camera_isp_D1.c:4797-4799. */
		return 0;

	case ISP_RT_BUF_CTRL_ENQUE:
	case ISP_RT_BUF_CTRL_ENQUE_IMD:
		if (!ctrl.data_ptr)
			return -EFAULT;
		if (copy_from_user(&info, (void __user *)ctrl.data_ptr,
				   sizeof(info)))
			return -EFAULT;

		vf_up = mtk_isp_vf_up(p, mtk_isp_dom_of_dma(dma));
		if (!vf_up) {
			spin_lock_irqsave(&p->rtbc_lock, flags);
			ret = mtk_isp_rtbc_enque(p, dma, &info);
			spin_unlock_irqrestore(&p->rtbc_lock, flags);
			return ret;
		}

		if (!ctrl.ex_data_ptr) {
			/* Re-arm a buffer that is already in the ring. */
			ISP_RT_RING_BUF_INFO_STRUCT *ring =
				&p->rtbuf->ring_buf[dma];
			unsigned int i;

			for (i = 0; i < MTK_ISP_RT_BUF_SIZE; i++) {
				if (ring->data[i].base_pAddr == info.base_pAddr)
					break;
			}
			if (i == MTK_ISP_RT_BUF_SIZE) {
				dev_err_ratelimited(p->dev,
						    "rtbc: unknown buffer 0x%08x on dma %u\n",
						    info.base_pAddr, dma);
				return -EFAULT;
			}
			if (ring->data[i].bFilled != MTK_ISP_BUF_EMPTY) {
				ring->data[i].bFilled = MTK_ISP_BUF_EMPTY;
				ring->data[i].image.frm_cnt =
					MTK_ISP_INVALID_FRM_CNT;
				if (ring->empty_count < ring->total_count)
					ring->empty_count++;
			}
			mtk_isp_rtbc_rcnt_queue(p, dma);
			return 0;
		}

		if (!p->rtbuf->ring_buf[dma].active) {
			dev_err_ratelimited(p->dev,
					    "rtbc: dma %u is not active\n", dma);
			return -EFAULT;
		}
		{
			ISP_RT_BUF_INFO_STRUCT newbuf;

			if (copy_from_user(&newbuf,
					   (void __user *)ctrl.ex_data_ptr,
					   sizeof(newbuf)))
				return -EFAULT;
			spin_lock_irqsave(&p->rtbc_lock, flags);
			ret = mtk_isp_rtbc_exchange(p, dma, &info, &newbuf);
			if (!ret)
				mtk_isp_rtbc_rcnt_queue(p, dma);
			spin_unlock_irqrestore(&p->rtbc_lock, flags);
		}
		return ret;

	case ISP_RT_BUF_CTRL_DEQUE:
		if (!ctrl.pExtend)
			return -EFAULT;
		memset(&deque, 0, sizeof(deque));

		if (!(mtk_isp_vf_up(p, MTK_ISP_IRQ_CAM) ||
		      mtk_isp_vf_up(p, MTK_ISP_IRQ_CAM_D) ||
		      mtk_isp_vf_up(p, MTK_ISP_IRQ_CAMSV) ||
		      mtk_isp_vf_up(p, MTK_ISP_IRQ_CAMSV2))) {
			/* A stopped port has no filled buffer to hand out. */
			return -EFAULT;
		}

		spin_lock_irqsave(&p->rtbc_lock, flags);
		ret = mtk_isp_rtbc_deque(p, dma, &deque);
		spin_unlock_irqrestore(&p->rtbc_lock, flags);
		if (ret)
			return ret;
		if (!deque.count)
			return -EFAULT;
		if (copy_to_user((void __user *)ctrl.pExtend, &deque,
				 sizeof(deque)))
			return -EFAULT;
		return 0;

	case ISP_RT_BUF_CTRL_IS_RDY:
		if (!ctrl.pExtend)
			return -EFAULT;
		{
			ISP_RT_RING_BUF_INFO_STRUCT *ring =
				&p->rtbuf->ring_buf[dma];
			u32 idx;

			spin_lock_irqsave(&p->rtbc_lock, flags);
			idx = ring->read_idx;
			if (idx < MTK_ISP_RT_BUF_SIZE &&
			    idx < ring->total_count &&
			    ring->data[idx].bFilled == MTK_ISP_BUF_FILLED)
				wait = 0;
			spin_unlock_irqrestore(&p->rtbc_lock, flags);
		}
		if (copy_to_user((void __user *)ctrl.pExtend, &wait,
				 sizeof(wait)))
			return -EFAULT;
		return 0;

	case ISP_RT_BUF_CTRL_DMA_EN:
		if (!ctrl.pExtend)
			return -EFAULT;
		if (copy_from_user(array, (void __user *)ctrl.pExtend,
				   sizeof(array)))
			return -EFAULT;
		spin_lock_irqsave(&p->rtbc_lock, flags);
		for (dma = 0; dma < MTK_ISP_DMA_NUM; dma++) {
			p->rtbuf->ring_buf[dma].active = array[dma] ? 1 : 0;
			if (array[dma])
				dev_info(p->dev, "rtbc: DMA %u enabled\n", dma);
		}
		spin_unlock_irqrestore(&p->rtbc_lock, flags);
		return 0;

	case ISP_RT_BUF_CTRL_GET_SIZE:
		if (!ctrl.pExtend)
			return -EFAULT;
		spin_lock_irqsave(&p->rtbc_lock, flags);
		size = p->rtbuf->ring_buf[dma].total_count;
		spin_unlock_irqrestore(&p->rtbc_lock, flags);
		if (copy_to_user((void __user *)ctrl.pExtend, &size,
				 sizeof(size)))
			return -EFAULT;
		return 0;

	case ISP_RT_BUF_CTRL_CLEAR:
		switch (dma) {
		case _imgo_:
		case _rrzo_:
		case _imgo_d_:
		case _rrzo_d_:
		case _camsv_imgo_:
		case _camsv2_imgo_:
			break;
		default:
			return -EFAULT;
		}
		spin_lock_irqsave(&p->rtbc_lock, flags);
		mtk_isp_rtbc_clear(p, dma);
		spin_unlock_irqrestore(&p->rtbc_lock, flags);
		return 0;

	case ISP_RT_BUF_CTRL_CUR_STATUS:
		/* Only dumps the state to the kernel log
		 * (camera_isp_D1.c:5855-5864). */
		reg_val = mtk_isp_vf_up(p, MTK_ISP_IRQ_CAM);
		spin_lock_irqsave(&p->rtbc_lock, flags);
		dev_info(p->dev,
			 "rtbc status: p1_vf=%u p1d_vf=%u sv1_vf=%u sv2_vf=%u dma=%u start=%u total=%u empty=%u\n",
			 reg_val, mtk_isp_vf_up(p, MTK_ISP_IRQ_CAM_D),
			 mtk_isp_vf_up(p, MTK_ISP_IRQ_CAMSV),
			 mtk_isp_vf_up(p, MTK_ISP_IRQ_CAMSV2), dma,
			 p->rtbuf->ring_buf[dma].start,
			 p->rtbuf->ring_buf[dma].total_count,
			 p->rtbuf->ring_buf[dma].empty_count);
		spin_unlock_irqrestore(&p->rtbc_lock, flags);
		return 0;

	case ISP_RT_BUF_CTRL_MAX:
		return 0;
	}
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Reference counts (ISP_REF_CNT_CTRL_FUNC, camera_isp_D1.c:3961-4110) */
/* ------------------------------------------------------------------ */

static int mtk_isp_ref_cnt(struct mtk_isp *p, ISP_REF_CNT_CTRL_STRUCT *ref,
			   int32_t *value)
{
	ISP_REF_CNT_ID_ENUM id = ref->id;
	int32_t cnt;

	if (id < 0 || id >= ISP_REF_CNT_ID_MAX)
		return -EFAULT;

	switch (ref->ctrl) {
	case ISP_REF_CNT_GET:
		break;
	case ISP_REF_CNT_INC:
		atomic_inc(&p->refcnt[id]);
		break;
	case ISP_REF_CNT_DEC:
	case ISP_REF_CNT_DEC_AND_RESET_P1_P2_IF_LAST_ONE:
	case ISP_REF_CNT_DEC_AND_RESET_P1_IF_LAST_ONE:
	case ISP_REF_CNT_DEC_AND_RESET_P2_IF_LAST_ONE:
		atomic_dec(&p->refcnt[id]);
		break;
	case ISP_REF_CNT_MAX:
	default:
		return -EINVAL;
	}

	cnt = atomic_read(&p->refcnt[id]);
	if (cnt == 0) {
		if (ref->ctrl == ISP_REF_CNT_DEC_AND_RESET_P1_P2_IF_LAST_ONE ||
		    ref->ctrl == ISP_REF_CNT_DEC_AND_RESET_P1_IF_LAST_ONE)
			mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAM_P1);
		if (ref->ctrl == ISP_REF_CNT_DEC_AND_RESET_P1_P2_IF_LAST_ONE ||
		    ref->ctrl == ISP_REF_CNT_DEC_AND_RESET_P2_IF_LAST_ONE)
			mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAM_P2);
	}
	*value = cnt;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Register / state dump (ISP_DumpReg, camera_isp_D1.c:2540-3010)      */
/* ------------------------------------------------------------------ */

static void mtk_isp_dump_reg(struct mtk_isp *p, const char *why)
{
	/*
	 * The ISP status registers are read-to-clear and there is no
	 * write-to-clear register, so a diagnostic dump must never sample
	 * them (it would eat a pending interrupt).  The cached software
	 * status is printed instead.
	 */
	unsigned long flags;
	u32 sw[MTK_ISP_IRQ_TYPES];
	int i;

	spin_lock_irqsave(&p->irq_lock, flags);
	for (i = 0; i < MTK_ISP_IRQ_TYPES; i++)
		sw[i] = p->status[ISP_IRQ_USER_ISPDRV][i];
	spin_unlock_irqrestore(&p->irq_lock, flags);

	dev_info(p->dev, "--- ISP state dump (%s) ---\n", why);
	dev_info(p->dev, "CAM_SW_CTL   0x%08x  EN1        0x%08x\n",
		 isp_rd(p, ISP_REG_CAM_SW_CTL), isp_rd(p, ISP_REG_EN1));
	dev_info(p->dev, "sw status: p1 0x%08x p1d 0x%08x stx 0x%08x sv 0x%08x sv2 0x%08x\n",
		 sw[ISP_IRQ_TYPE_INT_P1_ST], sw[ISP_IRQ_TYPE_INT_P1_ST_D],
		 sw[ISP_IRQ_TYPE_INT_STATUSX], sw[ISP_IRQ_TYPE_INT_CAMSV],
		 sw[ISP_IRQ_TYPE_INT_CAMSV2]);
	dev_info(p->dev, "TG_VF_CON    0x%08x  TG_INTER_ST 0x%08x\n",
		 isp_rd(p, ISP_REG_TG_VF_CON), isp_rd(p, ISP_REG_TG_INTER_ST));
	dev_info(p->dev, "TG2_VF_CON   0x%08x  TG2_INTER_ST 0x%08x\n",
		 isp_rd(p, ISP_REG_TG2_VF_CON), isp_rd(p, ISP_REG_TG2_INTER_ST));
	dev_info(p->dev, "IMGO_FBC     0x%08x  RRZO_FBC  0x%08x\n",
		 isp_rd(p, ISP_REG_IMGO_FBC), isp_rd(p, ISP_REG_RRZO_FBC));
	dev_info(p->dev, "IMGO_D_FBC   0x%08x  RRZO_D_FBC 0x%08x\n",
		 isp_rd(p, ISP_REG_IMGO_D_FBC), isp_rd(p, ISP_REG_RRZO_D_FBC));
	dev_info(p->dev, "IMGO_BASE    0x%08x  RRZO_BASE 0x%08x\n",
		 isp_rd(p, ISP_REG_IMGO_BASE_ADDR),
		 isp_rd(p, ISP_REG_RRZO_BASE_ADDR));
	dev_info(p->dev, "IMGO_D_BASE  0x%08x  RRZO_D_BASE 0x%08x\n",
		 isp_rd(p, ISP_REG_IMGO_D_BASE_ADDR),
		 isp_rd(p, ISP_REG_RRZO_D_BASE_ADDR));
	dev_info(p->dev, "IMGO_SV_BASE 0x%08x  IMGO_SV_D  0x%08x\n",
		 isp_rd(p, ISP_REG_IMGO_SV_BASE_ADDR),
		 isp_rd(p, ISP_REG_IMGO_SV_D_BASE_ADDR));
}

/* ------------------------------------------------------------------ */
/* ioctl (camera_isp_D1.c:9748-10370)                                  */
/* ------------------------------------------------------------------ */

static long mtk_isp_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct mtk_isp_file *ctx = file->private_data;
	struct mtk_isp *p;
	ISP_REG_IO_STRUCT reg_io;
	ISP_WAIT_IRQ_STRUCT irq;
	ISP_READ_IRQ_STRUCT read_irq;
	ISP_CLEAR_IRQ_STRUCT clear_irq;
	ISP_ED_BUFQUE_STRUCT ed;
	ISP_REGISTER_USERKEY_STRUCT key_req;
	ISP_REF_CNT_CTRL_STRUCT ref;
	ISP_HOLD_TIME_ENUM hold_time;
	u32 value = 0, values[2] = { 0, 0 };
	s32 svalue = 0;
	int32_t refcnt;
	u8 hold_en;
	long ret = 0;

	if (!ctx || !ctx->isp)
		return -EFAULT;
	p = ctx->isp;

	switch (cmd) {
	case ISP_RESET_CAM_P1:
		mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAM_P1);
		break;
	case ISP_RESET_CAM_P2:
		mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAM_P2);
		break;
	case ISP_RESET_CAMSV:
		mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAMSV);
		break;
	case ISP_RESET_CAMSV2:
		mtk_isp_reset(p, ISP_REG_SW_CTL_RST_CAMSV2);
		break;
	case ISP_RESET_BUF:
		spin_lock_bh(&p->hold_lock);
		mtk_isp_hold_reset(p);
		spin_unlock_bh(&p->hold_lock);
		break;

	case ISP_READ_REGISTER:
		if (copy_from_user(&reg_io, (void __user *)arg, sizeof(reg_io)))
			return -EFAULT;
		ret = mtk_isp_reg_read(p, &reg_io);
		break;
	case ISP_WRITE_REGISTER:
		if (copy_from_user(&reg_io, (void __user *)arg, sizeof(reg_io)))
			return -EFAULT;
		ret = mtk_isp_reg_write(p, &reg_io);
		break;

	case ISP_HOLD_REG_TIME:
		if (copy_from_user(&hold_time, (void __user *)arg,
				   sizeof(hold_time)))
			return -EFAULT;
		if (hold_time != ISP_HOLD_TIME_VD &&
		    hold_time != ISP_HOLD_TIME_EXPDONE)
			return -EINVAL;
		p->hold_time = hold_time;
		break;
	case ISP_HOLD_REG:
		if (copy_from_user(&hold_en, (void __user *)arg, sizeof(hold_en)))
			return -EFAULT;
		ret = mtk_isp_hold_enable(p, hold_en != 0);
		break;

	case ISP_WAIT_IRQ:
		if (copy_from_user(&irq, (void __user *)arg, sizeof(irq)))
			return -EFAULT;
		if (irq.Type < 0 || irq.Type >= MTK_ISP_IRQ_TYPES)
			return -EFAULT;
		if (irq.UserNumber < 0 || irq.UserNumber >= ISP_IRQ_USER_MAX)
			return -EFAULT;
		if (irq.UserNumber > 0) {
			ret = mtk_isp_wait_irq_v1(p, &irq);
		} else {
			int user_key = 0;

			if (irq.UserInfo.Type < 0 ||
			    irq.UserInfo.Type >= MTK_ISP_IRQ_TYPES)
				return -EFAULT;
			if (irq.UserInfo.UserKey > 0 &&
			    irq.UserInfo.UserKey < MTK_ISP_IRQ_USERS)
				user_key = irq.UserInfo.UserKey;
			irq.UserInfo.UserKey = user_key;
			ret = mtk_isp_wait_irq_v3(p, &irq);
		}
		if (copy_to_user((void __user *)arg, &irq, sizeof(irq)) &&
		    !ret)
			ret = -EFAULT;
		break;

	case ISP_READ_IRQ:
		if (copy_from_user(&read_irq, (void __user *)arg,
				   sizeof(read_irq)))
			return -EFAULT;
		if (read_irq.Type < 0 || read_irq.Type >= MTK_ISP_IRQ_TYPES)
			return -EFAULT;
		if (read_irq.UserNumber < 0 ||
		    read_irq.UserNumber >= ISP_IRQ_USER_MAX)
			return -EFAULT;
		{
			unsigned long flags;

			spin_lock_irqsave(&p->irq_lock, flags);
			read_irq.Status =
				p->status[read_irq.UserNumber][read_irq.Type];
			spin_unlock_irqrestore(&p->irq_lock, flags);
		}
		if (copy_to_user((void __user *)arg, &read_irq,
				 sizeof(read_irq)))
			return -EFAULT;
		break;

	case ISP_CLEAR_IRQ:
		if (copy_from_user(&clear_irq, (void __user *)arg,
				   sizeof(clear_irq)))
			return -EFAULT;
		if (clear_irq.Type < 0 || clear_irq.Type >= MTK_ISP_IRQ_TYPES)
			return -EFAULT;
		if (clear_irq.UserNumber < 0 ||
		    clear_irq.UserNumber >= ISP_IRQ_USER_MAX)
			return -EFAULT;
		{
			unsigned long flags;

			spin_lock_irqsave(&p->irq_lock, flags);
			p->status[clear_irq.UserNumber][clear_irq.Type] &=
				~clear_irq.Status;
			spin_unlock_irqrestore(&p->irq_lock, flags);
		}
		break;

	case ISP_REGISTER_IRQ:
	case ISP_UNREGISTER_IRQ:
		/*
		 * The downstream ISP_ioctl() has no case for either command,
		 * so both fall into its default branch and return -EPERM
		 * (camera_isp_D1.c:10360-10366).  The v1/v3 register/wait
		 * split is served by ISP_WAIT_IRQ and
		 * ISP_REGISTER_IRQ_USER_KEY.
		 */
		ret = -EPERM;
		break;

	case ISP_REGISTER_IRQ_USER_KEY:
		if (copy_from_user(&key_req, (void __user *)arg,
				   sizeof(key_req)))
			return -EFAULT;
		ret = mtk_isp_userkey_register(
			p, (const char __user *)(uintptr_t)key_req.userName,
			&key_req.userKey);
		if (copy_to_user((void __user *)arg, &key_req,
				 sizeof(key_req)) && !ret)
			ret = -EFAULT;
		if (!ret)
			ctx->pid = current->pid;
		break;

	case ISP_MARK_IRQ_REQUEST:
		if (copy_from_user(&irq, (void __user *)arg, sizeof(irq)))
			return -EFAULT;
		ret = mtk_isp_mark_irq(p, &irq);
		break;

	case ISP_GET_MARK2QUERY_TIME:
		if (copy_from_user(&irq, (void __user *)arg, sizeof(irq)))
			return -EFAULT;
		ret = mtk_isp_get_mark2query(p, &irq);
		if (copy_to_user((void __user *)arg, &irq, sizeof(irq)) &&
		    !ret)
			ret = -EFAULT;
		break;

	case ISP_FLUSH_IRQ_REQUEST:
		if (copy_from_user(&irq, (void __user *)arg, sizeof(irq)))
			return -EFAULT;
		if (irq.UserNumber > 0) {
			if (irq.UserNumber >= ISP_IRQ_USER_MAX ||
			    irq.Type < 0 || irq.Type >= MTK_ISP_IRQ_TYPES)
				return -EFAULT;
			ret = mtk_isp_flush_irq_v1(p, &irq);
		} else {
			if (irq.UserInfo.UserKey < 1 ||
			    irq.UserInfo.UserKey >= MTK_ISP_IRQ_USERS ||
			    irq.UserInfo.Type < 0 ||
			    irq.UserInfo.Type >= MTK_ISP_IRQ_TYPES)
				return -EFAULT;
			ret = mtk_isp_flush_irq(p, &irq);
		}
		break;

	case ISP_ED_QUEBUF_CTRL:
		if (copy_from_user(&ed, (void __user *)arg, sizeof(ed)))
			return -EFAULT;
		ret = mtk_isp_ed_ctrl(p, &ed, ctx->pid);
		break;

	case ISP_UPDATE_REGSCEN:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		spin_lock(&p->scen_lock);
		p->reg_scen = value;
		spin_unlock(&p->scen_lock);
		break;
	case ISP_QUERY_REGSCEN:
		spin_lock(&p->scen_lock);
		value = p->reg_scen;
		spin_unlock(&p->scen_lock);
		if (copy_to_user((void __user *)arg, &value, sizeof(value)))
			return -EFAULT;
		break;

	case ISP_UPDATE_BURSTQNUM:
		if (copy_from_user(&svalue, (void __user *)arg, sizeof(svalue)))
			return -EFAULT;
		if (svalue < 1 || svalue > MTK_ISP_ED_BURSTQ_MAX)
			return -EINVAL;
		spin_lock(&p->scen_lock);
		p->burst_qnum = svalue;
		spin_unlock(&p->scen_lock);
		break;
	case ISP_QUERY_BURSTQNUM:
		spin_lock(&p->scen_lock);
		svalue = p->burst_qnum;
		spin_unlock(&p->scen_lock);
		if (copy_to_user((void __user *)arg, &svalue, sizeof(svalue)))
			return -EFAULT;
		break;

	case ISP_DUMP_REG:
		mtk_isp_dump_reg(p, "ioctl");
		break;

	case ISP_DUMP_ISR_LOG:
		/*
		 * The downstream build defines IRQ_LOG_KEEPER() as pr_warn()
		 * (camera_isp_D1.c:984-991), so what the log printer can show
		 * is the live interrupt state.  The TG index is still taken
		 * from the argument (camera_isp_D1.c:10311-10324).
		 */
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (value > MTK_ISP_IRQ_DOM_NUM)
			return -EINVAL;
		mtk_isp_dump_reg(p, "isr-log");
		break;

	case ISP_GET_CUR_SOF:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		switch (value) {
		case MTK_ISP_IRQ_CAM:
			value = p->sof_count[MTK_ISP_CH_P1];
			break;
		case MTK_ISP_IRQ_CAM_D:
			value = p->sof_count[MTK_ISP_CH_P1_D];
			break;
		case MTK_ISP_IRQ_CAMSV:
			value = p->sof_count[MTK_ISP_CH_CAMSV];
			break;
		case MTK_ISP_IRQ_CAMSV2:
			value = p->sof_count[MTK_ISP_CH_CAMSV_D];
			break;
		default:
			return -EFAULT;
		}
		if (copy_to_user((void __user *)arg, &value, sizeof(value)))
			return -EFAULT;
		break;

	case ISP_GET_DROP_FRAME:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		switch (value) {
		case MTK_ISP_IRQ_CAM:
			value = p->lost_p1done_cnt ? 1 : 0;
			break;
		case MTK_ISP_IRQ_CAM_D:
			value = p->lost_p1d_done_cnt ? 1 : 0;
			break;
		case MTK_ISP_IRQ_CAMSV:
			value = p->camsv_drop[0] ? 1 : 0;
			break;
		case MTK_ISP_IRQ_CAMSV2:
			value = p->camsv_drop[1] ? 1 : 0;
			break;
		default:
			return -EFAULT;
		}
		if (copy_to_user((void __user *)arg, &value, sizeof(value)))
			return -EFAULT;
		break;

	case ISP_GET_DMA_ERR:
		if (!arg)
			return -EFAULT;
		if (copy_to_user((void __user *)arg, p->dma_err,
				 sizeof(p->dma_err)))
			return -EFAULT;
		break;

	case ISP_GET_INT_ERR:
		if (!arg)
			return -EFAULT;
		if (copy_to_user((void __user *)arg, p->int_err,
				 sizeof(p->int_err)))
			return -EFAULT;
		break;

	case ISP_SET_FPS:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (!value)
			return -EINVAL;
		/* One-shot, as in camera_isp_D1.c:10340-10360: the frame rate
		 * and the interpolation step are latched on the first call. */
		if (!p->ts.fps) {
			p->ts.fps = value;
			p->ts.interval_us = 1000000 / value;
			p->ts.compensation_us =
				1000000 - p->ts.interval_us * value;
		}
		break;

	case ISP_WAKELOCK_CTRL:
		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (value) {
			if (!p->ws_held) {
				__pm_stay_awake(p->ws);
				p->ws_held = true;
			}
		} else if (p->ws_held) {
			__pm_relax(p->ws);
			p->ws_held = false;
		}
		break;

	case ISP_DEBUG_FLAG:
		if (copy_from_user(values, (void __user *)arg, sizeof(values)))
			return -EFAULT;
		p->debug_mask = values[0];
		break;

	case ISP_SET_USER_PID:
		/*
		 * Only compiled in downstream behind
		 * ISP_KERNEL_MOTIFY_SINGAL_TEST (camera_isp_D1.c:10300-10310);
		 * it is not part of the shipped driver, so it is reported as
		 * unsupported instead of pretending to signal a process.
		 */
		ret = -EOPNOTSUPP;
		break;

	case ISP_BUFFER_CTRL:
		ret = mtk_isp_rtbuf_ctrl(p, arg);
		break;

	case ISP_REF_CNT_CTRL:
		if (copy_from_user(&ref, (void __user *)arg, sizeof(ref)))
			return -EFAULT;
		ret = mtk_isp_ref_cnt(p, &ref, &refcnt);
		if (!ret && ref.data_ptr &&
		    copy_to_user((void __user *)(uintptr_t)ref.data_ptr,
				 &refcnt, sizeof(refcnt)))
			ret = -EFAULT;
		break;

	default:
		dev_err(p->dev, "unknown ioctl 0x%08x\n", cmd);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* mmap (camera_isp_D1.c:11231-11373)                                  */
/* ------------------------------------------------------------------ */

struct mtk_isp_mmap_win {
	phys_addr_t base;
	unsigned long size;
};

static const struct mtk_isp_mmap_win mtk_isp_mmap_windows[] = {
	{ MTK_ISP_PHYS_IMGSYS,		MTK_ISP_SIZE_IMGSYS },
	{ MTK_ISP_PHYS_ISP,		MTK_ISP_SIZE_ISP },
	{ MTK_ISP_PHYS_SENINF,		MTK_ISP_SIZE_SENINF },
	{ MTK_ISP_PHYS_MIPIRX_CFG,	MTK_ISP_SIZE_MIPIRX_CFG },
	{ MTK_ISP_PHYS_ISP_INNER,	MTK_ISP_SIZE_ISP_INNER },
	{ MTK_ISP_PHYS_PLL,		MTK_ISP_SIZE_PLL },
	{ MTK_ISP_PHYS_GPIO,		MTK_ISP_SIZE_GPIO },
	{ MTK_ISP_PHYS_MIPI_ANA,	MTK_ISP_SIZE_MIPI_ANA },
};

static int mtk_isp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mtk_isp_file *ctx = file->private_data;
	struct mtk_isp *p = ctx->isp;
	unsigned long len = vma->vm_end - vma->vm_start;
	phys_addr_t pa;
	int i, ret;

	if (!p || p->open_count <= 0)
		return -ENODEV;
	if (len & ~PAGE_MASK)
		return -EINVAL;

	/* Page offset RT_BUF_TBL_NPAGES maps the shared ring table. */
	if (vma->vm_pgoff == MTK_ISP_RT_TBL_NPAGES) {
		if (len > MTK_ISP_RT_TBL_NPAGES * PAGE_SIZE)
			return -EINVAL;
		ret = remap_pfn_range(vma, vma->vm_start,
				      page_to_pfn(p->rtbuf_pages), len,
				      vma->vm_page_prot);
		if (ret)
			return ret;
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		return 0;
	}

	pa = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	for (i = 0; i < ARRAY_SIZE(mtk_isp_mmap_windows); i++) {
		if (pa != mtk_isp_mmap_windows[i].base)
			continue;
		if (len > mtk_isp_mmap_windows[i].size)
			return -EAGAIN;
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		ret = remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, len,
				      vma->vm_page_prot);
		if (ret)
			return -EAGAIN;
		return 0;
	}
	dev_err(p->dev, "mmap: illegal physical base 0x%pa\n", &pa);
	return -EAGAIN;
}

/* ------------------------------------------------------------------ */
/* Power (clocks + ISP power domain)                                   */
/* ------------------------------------------------------------------ */

static const char *const mtk_isp_clk_names[] = {
	"CG_IMAGE_CAM_SMI", "CG_IMAGE_CAM_CAM", "CG_IMAGE_SEN_TG",
	"CG_IMAGE_SEN_CAM", "CG_IMAGE_CAM_SV", "CG_IMAGE_LARB2_SMI",
};

static int mtk_isp_power_on(struct mtk_isp *p)
{
	int i, ret;

	ret = pm_runtime_resume_and_get(p->dev);
	if (ret) {
		dev_err(p->dev, "failed to resume ISP block: %d\n", ret);
		return ret;
	}
	for (i = 0; i < p->clk_num; i++) {
		ret = clk_prepare_enable(p->clks[i]);
		if (ret) {
			dev_err(p->dev, "failed to enable %s: %d\n",
				mtk_isp_clk_names[i], ret);
			while (--i >= 0)
				clk_disable_unprepare(p->clks[i]);
			pm_runtime_put(p->dev);
			return ret;
		}
	}
	return 0;
}

static void mtk_isp_power_off(struct mtk_isp *p)
{
	int i;

	for (i = p->clk_num - 1; i >= 0; i--)
		clk_disable_unprepare(p->clks[i]);
	pm_runtime_put(p->dev);
}

/*
 * The IRQs stay requested for the lifetime of the device but are disabled
 * while no file is open, so the handler can never touch registers of a
 * powered-down ISP block.  disable_irq() also waits for a running handler.
 */
static void mtk_isp_irqs_enable(struct mtk_isp *p)
{
	if (p->irq_cam > 0)
		enable_irq(p->irq_cam);
	if (p->irq_camsv > 0)
		enable_irq(p->irq_camsv);
	if (p->irq_camsv2 > 0)
		enable_irq(p->irq_camsv2);
}

static void mtk_isp_irqs_disable(struct mtk_isp *p)
{
	if (p->irq_cam > 0)
		disable_irq(p->irq_cam);
	if (p->irq_camsv > 0)
		disable_irq(p->irq_camsv);
	if (p->irq_camsv2 > 0)
		disable_irq(p->irq_camsv2);
}

/*
 * Teardown drain: the status registers are read-to-clear, so reading each of
 * them once after the last stream has been switched off drops whatever event
 * was still latched before the clocks are gated.  This is the only place
 * outside the interrupt handlers that samples them, and it exists exactly
 * because they are read-to-clear.
 */
static void mtk_isp_irq_drain(struct mtk_isp *p)
{
	isp_rd(p, ISP_REG_INT_P1_ST);
	isp_rd(p, ISP_REG_INT_P1_ST2);
	isp_rd(p, ISP_REG_INT_P1_ST_D);
	isp_rd(p, ISP_REG_INT_P1_ST2_D);
	isp_rd(p, ISP_REG_INT_STATUSX);
	isp_rd(p, ISP_REG_INT_STATUS2X);
	isp_rd(p, ISP_REG_INT_STATUS3X);
	isp_rd(p, ISP_REG_CAMSV_INT);
	isp_rd(p, ISP_REG_CAMSV2_INT);
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int mtk_isp_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct mtk_isp *p = container_of(misc, struct mtk_isp, misc);
	struct mtk_isp_file *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->isp = p;
	ctx->pid = current->pid;

	mutex_lock(&p->power_lock);
	if (p->open_count++ == 0) {
		ret = mtk_isp_power_on(p);
		if (ret) {
			p->open_count = 0;
			mutex_unlock(&p->power_lock);
			kfree(ctx);
			return ret;
		}
		/* Reset the session state before the handlers can run again. */
		mtk_isp_session_reset(p);
		mtk_isp_irqs_enable(p);
		dev_dbg(p->dev, "first user (pid %d)\n", current->pid);
	}
	mutex_unlock(&p->power_lock);

	file->private_data = ctx;
	return 0;
}

static int mtk_isp_release(struct inode *inode, struct file *file)
{
	struct mtk_isp_file *ctx = file->private_data;
	struct mtk_isp *p;

	if (!ctx)
		return 0;
	p = ctx->isp;

	mutex_lock(&p->power_lock);
	if (p->open_count == 1) {
		/* Closing the viewfinder makes sure a camera session recovers
		 * after an abnormal exit (camera_isp_D1.c:11170-11180). */
		isp_wr(p, ISP_REG_TG_VF_CON,
		       isp_rd(p, ISP_REG_TG_VF_CON) & ~0x1);
		isp_wr(p, ISP_REG_TG2_VF_CON,
		       isp_rd(p, ISP_REG_TG2_VF_CON) & ~0x1);
		if (p->ws_held) {
			__pm_relax(p->ws);
			p->ws_held = false;
		}
		/* Release the per-camera MCLK enables
		 * (camera_isp_D1.c:11200-11210 writes ISP_ADDR+0x4200,
		 * +0x4600 and +0x4a00 to 1). */
		isp_wr(p, ISP_REG_MCLK1_EN, 0x1);
		isp_wr(p, ISP_REG_MCLK2_EN, 0x1);
		isp_wr(p, ISP_REG_MCLK3_EN, 0x1);
		mtk_isp_irqs_disable(p);
		mtk_isp_irq_drain(p);
		mtk_isp_hold_reset(p);
		mtk_isp_power_off(p);
	}
	if (p->open_count > 0)
		p->open_count--;
	mutex_unlock(&p->power_lock);

	file->private_data = NULL;
	kfree(ctx);
	return 0;
}

static const struct file_operations mtk_isp_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_isp_open,
	.release	= mtk_isp_release,
	.unlocked_ioctl	= mtk_isp_ioctl,
	.compat_ioctl	= mtk_isp_ioctl,
	.mmap		= mtk_isp_mmap,
};

/* ------------------------------------------------------------------ */
/* Platform driver                                                     */
/* ------------------------------------------------------------------ */

static int mtk_isp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_isp *p;
	struct resource *res;
	int i, ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;

	/*
	 * The register windows follow the ROM ispsys node
	 * (zip/rom_dtb_0.dts:3705-3717):
	 *   0: ISP core + CAMSV banks   0x15004000 + 0x9000
	 *   1: ISP inner (TPIPE)        0x1500d000 + 0x1000
	 *   2: CAMINF / IMGSYS          0x15000000 + 0x10000
	 *   3: MIPI RX analog           0x10215000 + 0x3000
	 *   4: GPIO                     0x10211000 + 0x1000
	 *
	 * devm_ioremap() (not devm_ioremap_resource()) is used on purpose:
	 * the ispsys platform driver owns the same physical ranges and
	 * reserves them, and two reservations of one range would make
	 * whichever probes second fail with -EBUSY.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != MTK_ISP_PHYS_ISP)
		return dev_err_probe(dev, -ENODEV,
				     "missing ISP window at 0x%08x\n",
				     MTK_ISP_PHYS_ISP);
	if (resource_size(res) < MTK_ISP_SIZE_ISP)
		return dev_err_probe(dev, -EINVAL,
				     "ISP window must cover 0x%x bytes (CAMSV banks)\n",
				     MTK_ISP_SIZE_ISP);
	p->isp_phys = res->start;
	p->isp = devm_ioremap(dev, res->start, resource_size(res));
	if (!p->isp)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res || res->start != MTK_ISP_PHYS_IMGSYS)
		return dev_err_probe(dev, -ENODEV,
				     "missing CAMINF window at 0x%08x\n",
				     MTK_ISP_PHYS_IMGSYS);
	if (resource_size(res) < MTK_ISP_SIZE_IMGSYS)
		return dev_err_probe(dev, -EINVAL,
				     "CAMINF window must cover 0x%x bytes\n",
				     MTK_ISP_SIZE_IMGSYS);
	p->cam_phys = res->start;
	p->cam = devm_ioremap(dev, res->start, resource_size(res));
	if (!p->cam)
		return -ENOMEM;

	p->clk_num = ARRAY_SIZE(mtk_isp_clk_names);
	for (i = 0; i < p->clk_num; i++) {
		p->clks[i] = devm_clk_get(dev, mtk_isp_clk_names[i]);
		if (IS_ERR(p->clks[i]))
			return dev_err_probe(dev, PTR_ERR(p->clks[i]),
					     "failed to get clock %s\n",
					     mtk_isp_clk_names[i]);
	}

	p->irq_cam = platform_get_irq(pdev, 0);
	if (p->irq_cam < 0)
		return dev_err_probe(dev, p->irq_cam, "missing CAM IRQ\n");
	p->irq_camsv = platform_get_irq_optional(pdev, 3);
	p->irq_camsv2 = platform_get_irq_optional(pdev, 4);

	mutex_init(&p->power_lock);
	spin_lock_init(&p->rtbc_lock);
	spin_lock_init(&p->reg_lock);
	spin_lock_init(&p->irq_lock);
	spin_lock_init(&p->hold_lock);
	spin_lock_init(&p->userkey_lock);
	spin_lock_init(&p->edbuf_lock);
	spin_lock_init(&p->scen_lock);
	init_waitqueue_head(&p->waitq);
	init_waitqueue_head(&p->ed_wait_deque);
	init_waitqueue_head(&p->ed_wait_frame);

	p->hold_time = ISP_HOLD_TIME_EXPDONE;
	mtk_isp_irq_init_defaults(p);

	/*
	 * 64 KiB physically contiguous table, mapped to userspace at page
	 * offset RT_BUF_TBL_NPAGES (camera_isp_D1.c:13042-13065).
	 */
	p->rtbuf_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				     MTK_ISP_RT_TBL_ORDER);
	if (!p->rtbuf_pages) {
		ret = -ENOMEM;
		goto err_unwind;
	}
	p->rtbuf = page_address(p->rtbuf_pages);
	if (sizeof(*p->rtbuf) > MTK_ISP_RT_TBL_NPAGES * PAGE_SIZE) {
		dev_err(dev, "rt buffer table does not fit (%zu > %lu)\n",
			sizeof(*p->rtbuf),
			(unsigned long)MTK_ISP_RT_TBL_NPAGES * PAGE_SIZE);
		ret = -EINVAL;
		goto err_free_pages;
	}

	ret = mtk_isp_hold_alloc(p);
	if (ret)
		goto err_free_pages;

	p->ws = wakeup_source_register(dev, "camera-isp");
	if (!p->ws) {
		ret = -ENOMEM;
		goto err_free_hold;
	}

	tasklet_setup(&p->vd_tasklet, mtk_isp_vd_tasklet);
	tasklet_setup(&p->expdone_tasklet, mtk_isp_expdone_tasklet);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_ws;

	ret = devm_request_irq(dev, p->irq_cam, mtk_isp_irq_cam, 0,
			       "mtk-isp-cam", p);
	if (ret) {
		dev_err(dev, "failed to request CAM irq %d: %d\n",
			p->irq_cam, ret);
		goto err_ws;
	}
	if (p->irq_camsv > 0) {
		ret = devm_request_irq(dev, p->irq_camsv, mtk_isp_irq_camsv, 0,
				       "mtk-isp-camsv", p);
		if (ret)
			goto err_ws;
	}
	if (p->irq_camsv2 > 0) {
		ret = devm_request_irq(dev, p->irq_camsv2,
				       mtk_isp_irq_camsv2, 0,
				       "mtk-isp-camsv2", p);
		if (ret)
			goto err_ws;
	}

	mtk_isp_irqs_disable(p);

	p->misc.minor = MISC_DYNAMIC_MINOR;
	p->misc.name = MTK_ISP_DEV_NAME;
	p->misc.fops = &mtk_isp_fops;
	p->misc.parent = dev;
	ret = misc_register(&p->misc);
	if (ret) {
		dev_err(dev, "failed to register %s: %d\n", MTK_ISP_DEV_NAME,
			ret);
		goto err_ws;
	}

	platform_set_drvdata(pdev, p);
	dev_info(dev, "MT6735 camera ISP registered as /dev/%s (cam irq %d, camsv %d/%d)\n",
		 MTK_ISP_DEV_NAME, p->irq_cam, p->irq_camsv, p->irq_camsv2);
	return 0;

err_ws:
	tasklet_kill(&p->vd_tasklet);
	tasklet_kill(&p->expdone_tasklet);
	wakeup_source_unregister(p->ws);
err_free_hold:
	mtk_isp_hold_free(p);
err_free_pages:
	__free_pages(p->rtbuf_pages, MTK_ISP_RT_TBL_ORDER);
err_unwind:
	return ret;
}

static void mtk_isp_remove(struct platform_device *pdev)
{
	struct mtk_isp *p = platform_get_drvdata(pdev);

	misc_deregister(&p->misc);
	tasklet_kill(&p->vd_tasklet);
	tasklet_kill(&p->expdone_tasklet);
	wakeup_source_unregister(p->ws);
	mtk_isp_hold_free(p);
	__free_pages(p->rtbuf_pages, MTK_ISP_RT_TBL_ORDER);
}

static const struct of_device_id mtk_isp_of_match[] = {
	{ .compatible = "mediatek,mt6735-camera-isp" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_isp_of_match);

static struct platform_driver mtk_isp_driver = {
	.probe = mtk_isp_probe,
	.remove = mtk_isp_remove,
	.driver = {
		.name = "mtk-camera-isp",
		.of_match_table = mtk_isp_of_match,
	},
};
module_platform_driver(mtk_isp_driver);

MODULE_AUTHOR("MediaTek Co., Ltd.");
MODULE_DESCRIPTION("MediaTek MT6735 camera ISP character driver");
MODULE_LICENSE("GPL");

