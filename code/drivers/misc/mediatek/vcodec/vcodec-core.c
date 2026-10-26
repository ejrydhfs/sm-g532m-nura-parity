// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6735 video codec hardware service core.
 *
 * Copyright (C) 2015 MediaTek Inc.
 *
 * The MT6735 VENC/VDEC register programming is done by the vendor codec
 * libraries in userspace through the register windows exported here.  This
 * driver only owns the clocks, the power domains, the engine interrupts,
 * the hardware ownership lock, the interrupt wait and the register-window
 * mappings.  It serves both engines as one character device.
 *
 * Port from Samsung/MediaTek downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c
 * (lock: 688-1119, unlock: 1120-1243, waitisr: 1244-1395, ioctl: 1397-1665,
 *  compat: 1667-2030, open/release: 2030-2158, mmap: 2158-2200,
 *  init/exit: 2350-2611).
 */
#define pr_fmt(fmt)	"vcodec: " fmt

#include <linux/compat.h>
#include <linux/cpufreq.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/threads.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <asm/barrier.h>

#include <uapi/linux/vcodec.h>

#include "vcodec.h"

/* Downstream VCODEC_DEVNAME, videocodec_kernel_driver_D1.c:266 */
#define VCODEC_DEVNAME			"Vcodec"

/*
 * Downstream waits 1000 ms for the hardware lock whenever the caller did not
 * ask for a specific timeout (videocodec_kernel_driver_D1.c:886, 915).
 */
#define VCODEC_HWLOCK_DEFAULT_TIMEOUT_MS	1000

/* Engine registry, protected by vcodec_engines_lock. */
static LIST_HEAD(vcodec_engines);
static DEFINE_MUTEX(vcodec_engines_lock);

/* Number of open file descriptors on /dev/Vcodec, see vcodec_release(). */
static DEFINE_MUTEX(vcodec_open_lock);
static unsigned int vcodec_open_count;

/* Per-codec EMI user counters, downstream gu4EncEMICounter/gu4DecEMICounter. */
static DEFINE_MUTEX(vcodec_emi_lock);
static u32 vcodec_enc_emi_users;
static u32 vcodec_dec_emi_users;

/* Per-engine power user counters, downstream gu4L2CCounter accounting. */
static DEFINE_MUTEX(vcodec_pwr_lock);
static u32 vcodec_power_users[VCODEC_ENGINE_MAX];

/* Per-CPU load sampling, downstream get_cpu_load() is not available here. */
static DEFINE_MUTEX(vcodec_cpu_load_lock);
static u64 vcodec_cpu_prev_busy[NR_CPUS];
static u64 vcodec_cpu_prev_total[NR_CPUS];
static bool vcodec_cpu_prev_valid[NR_CPUS];

static bool vcodec_driver_type_is_dec(u32 driver_type)
{
	switch (driver_type) {
	case VCODEC_DRV_TYPE_MP4_DEC:
	case VCODEC_DRV_TYPE_HEVC_DEC:
	case VCODEC_DRV_TYPE_H264_DEC:
	case VCODEC_DRV_TYPE_MP1_MP2_DEC:
	case VCODEC_DRV_TYPE_VC1_DEC:
	case VCODEC_DRV_TYPE_VC1_ADV_DEC:
	case VCODEC_DRV_TYPE_VP8_DEC:
		return true;
	default:
		return false;
	}
}

static bool vcodec_driver_type_is_enc(u32 driver_type)
{
	switch (driver_type) {
	case VCODEC_DRV_TYPE_H264_ENC:
	case VCODEC_DRV_TYPE_HEVC_ENC:
	case VCODEC_DRV_TYPE_JPEG_ENC:
		return true;
	default:
		return false;
	}
}

/*
 * Translate a user handle into a physical address used as the lock owner
 * identity.  Downstream walks the page tables (pmem_user_v2p_video(),
 * videocodec_kernel.c:42-90); pinning the page is the equivalent stable
 * identity here.  If the page cannot be pinned (for example the handle lives
 * in a device mapping) the raw value is used unchanged, which keeps the
 * owner comparison self-consistent.
 */
static phys_addr_t vcodec_user_v2p(u64 uaddr)
{
	struct page *page = NULL;
	phys_addr_t pa;
	int ret;

	if (!uaddr)
		return 0;
	if (!current->mm)
		return (phys_addr_t)uaddr;

	ret = pin_user_pages_fast(uaddr & PAGE_MASK, 1, FOLL_WRITE, &page);
	if (ret != 1)
		ret = pin_user_pages_fast(uaddr & PAGE_MASK, 1, 0, &page);
	if (ret != 1) {
		pr_debug("handle 0x%llx is not pinnable, using it as identity\n",
			 (unsigned long long)uaddr);
		return (phys_addr_t)uaddr;
	}

	pa = page_to_phys(page) | offset_in_page(uaddr);
	unpin_user_page(page);

	return pa;
}

/*
 * Look up the registered engine with @id.  Returns NULL when there is none.
 * The returned pointer stays valid because engines are only unregistered
 * from their platform driver's ->remove, which cannot race an ioctl on a
 * driver that is still in use.
 */
static struct vcodec_engine *vcodec_engine_by_id(enum vcodec_engine_id id)
{
	struct vcodec_engine *eng, *found = NULL;

	mutex_lock(&vcodec_engines_lock);
	list_for_each_entry(eng, &vcodec_engines, node) {
		if (eng->id == id && eng->registered) {
			found = eng;
			break;
		}
	}
	mutex_unlock(&vcodec_engines_lock);

	return found;
}

/*
 * Look up the engine serving @driver_type.  Returns an ERR_PTR() for a
 * driver type that no engine handles (-EINVAL) or an engine that is not
 * registered right now (-ENODEV).
 */
static struct vcodec_engine *vcodec_lookup_engine(u32 driver_type)
{
	enum vcodec_engine_id id;
	struct vcodec_engine *eng;

	if (vcodec_driver_type_is_dec(driver_type))
		id = VCODEC_ENGINE_DEC;
	else if (vcodec_driver_type_is_enc(driver_type))
		id = VCODEC_ENGINE_ENC;
	else
		return ERR_PTR(-EINVAL);

	eng = vcodec_engine_by_id(id);

	return eng ? eng : ERR_PTR(-ENODEV);
}

/*
 * Downstream videocodec_kernel_driver_D1.c:1291-1296 re-raises the decoder to
 * its default level and restarts the DVFS monitor whenever a new decoder
 * EMI user shows up.
 */
static void vcodec_dvfs_begin_dec(void)
{
	struct vcodec_engine *eng = vcodec_engine_by_id(VCODEC_ENGINE_DEC);

	if (!eng || !eng->ops->dvfs_begin)
		return;

	mutex_lock(&eng->lock);
	eng->ops->dvfs_begin(eng);
	mutex_unlock(&eng->lock);
}

static unsigned int vcodec_engine_inst_count(void)
{
	struct vcodec_engine *eng;
	unsigned int count = 0;

	mutex_lock(&vcodec_engines_lock);
	list_for_each_entry(eng, &vcodec_engines, node) {
		if (READ_ONCE(eng->hw_locked))
			count++;
	}
	mutex_unlock(&vcodec_engines_lock);

	return count;
}

int vcodec_engine_register(struct vcodec_engine *eng)
{
	struct vcodec_engine *iter;
	int ret = 0;

	if (!eng || !eng->name || !eng->dev || !eng->ops)
		return -EINVAL;
	if (!eng->ops->power_on || !eng->ops->power_off ||
	    !eng->ops->enable_irq || !eng->ops->disable_irq)
		return -EINVAL;
	if (eng->id >= VCODEC_ENGINE_MAX)
		return -EINVAL;
	if (eng->num_regions > VCODEC_MAX_MMAP_REGIONS)
		return -EINVAL;

	mutex_init(&eng->lock);
	init_waitqueue_head(&eng->hwlock_wq);
	init_waitqueue_head(&eng->isr_wq);
	spin_lock_init(&eng->irq_lock);
	eng->hw_locked = false;
	eng->powered = false;
	eng->irq_enabled = false;
	eng->owner = 0;
	eng->driver_type = VCODEC_DRV_TYPE_NONE;
	eng->isr_seq = 0;
	eng->irq_status = 0;

	mutex_lock(&vcodec_engines_lock);
	list_for_each_entry(iter, &vcodec_engines, node) {
		if (iter->id == eng->id) {
			ret = -EBUSY;
			break;
		}
	}
	if (!ret) {
		eng->registered = true;
		list_add_tail(&eng->node, &vcodec_engines);
	}
	mutex_unlock(&vcodec_engines_lock);

	if (ret)
		dev_err(eng->dev, "engine %s already registered\n", eng->name);
	else
		dev_dbg(eng->dev, "engine %s registered\n", eng->name);

	return ret;
}

void vcodec_engine_unregister(struct vcodec_engine *eng)
{
	if (!eng)
		return;

	mutex_lock(&vcodec_engines_lock);
	if (!eng->registered) {
		mutex_unlock(&vcodec_engines_lock);
		return;
	}

	mutex_lock(&eng->lock);
	if (eng->irq_enabled) {
		eng->ops->disable_irq(eng);
		eng->irq_enabled = false;
	}
	if (eng->powered) {
		eng->ops->power_off(eng);
		eng->powered = false;
	}
	eng->hw_locked = false;
	eng->owner = 0;
	eng->driver_type = VCODEC_DRV_TYPE_NONE;
	mutex_unlock(&eng->lock);

	list_del(&eng->node);
	eng->registered = false;
	mutex_unlock(&vcodec_engines_lock);

	dev_dbg(eng->dev, "engine %s unregistered\n", eng->name);
}

void vcodec_engine_irq(struct vcodec_engine *eng, u32 status)
{
	unsigned long flags;

	if (!eng)
		return;

	spin_lock_irqsave(&eng->irq_lock, flags);
	eng->irq_status = status;
	eng->isr_seq++;
	spin_unlock_irqrestore(&eng->irq_lock, flags);

	wake_up_interruptible_all(&eng->isr_wq);
}

bool vcodec_engine_is_locked(struct vcodec_engine *eng)
{
	return eng && READ_ONCE(eng->hw_locked);
}

u32 vcodec_engine_get_driver_type(struct vcodec_engine *eng)
{
	return eng ? READ_ONCE(eng->driver_type) : VCODEC_DRV_TYPE_NONE;
}

/*
 * Take the hardware ownership lock of the engine @driver_type serves.
 * Downstream: videocodec_kernel_driver_D1.c:688-1119.
 */
static long vcodec_ioc_lockhw(u64 handle, u32 timeout_ms, u32 driver_type,
			      bool secure)
{
	struct vcodec_engine *eng;
	phys_addr_t owner;
	unsigned long timeout, left;
	int ret;

	eng = vcodec_lookup_engine(driver_type);
	if (IS_ERR(eng))
		return PTR_ERR(eng);

	owner = vcodec_user_v2p(handle);
	if (!owner)
		return -EINVAL;

	if (!timeout_ms)
		timeout_ms = VCODEC_HWLOCK_DEFAULT_TIMEOUT_MS;
	timeout = msecs_to_jiffies(timeout_ms);

	for (;;) {
		mutex_lock(&eng->lock);
		if (!eng->hw_locked)
			break;
		mutex_unlock(&eng->lock);

		left = wait_event_interruptible_timeout(eng->hwlock_wq,
							!READ_ONCE(eng->hw_locked),
							timeout);
		if (!left) {
			dev_dbg(eng->dev, "hw lock timeout after %u ms\n",
				timeout_ms);
			return -ETIMEDOUT;
		}
		if (left < 0) {
			dev_dbg(eng->dev, "hw lock interrupted\n");
			return -ERESTARTSYS;
		}

		/* Spurious wakeup: keep the remaining part of the budget. */
		timeout = left;
	}

	/*
	 * Downstream acquires the lock for the JPEG encoder too but guards the
	 * power on and the interrupt unmask with H264_ENC/HEVC_ENC only
	 * (videocodec_kernel_driver_D1.c:953-957 and 971-977); enc_isr() then
	 * reports a JPEG holder as an invalid lock holder (lines 664-667).
	 */
	if (driver_type == VCODEC_DRV_TYPE_JPEG_ENC) {
		eng->owner = owner;
		eng->driver_type = driver_type;
		/* Pairs with the READ_ONCE() in the wait condition above. */
		smp_store_release(&eng->hw_locked, true);
		mutex_unlock(&eng->lock);

		dev_dbg(eng->dev, "jpeg encoder lock recorded\n");

		return 0;
	}

	ret = eng->ops->power_on(eng);
	if (ret) {
		mutex_unlock(&eng->lock);
		dev_err(eng->dev, "power on failed: %d\n", ret);
		return ret;
	}
	eng->powered = true;

	eng->owner = owner;
	eng->driver_type = driver_type;
	/*
	 * Publish the owner before the line is unmasked: a level-low
	 * completion may already be pending, and the engine ISR must
	 * attribute it to this instance instead of acking it as spurious.
	 * Waiters only look at hw_locked, so publishing this early is safe.
	 */
	smp_store_release(&eng->hw_locked, true);

	if (!secure) {
		ret = eng->ops->enable_irq(eng);
		if (ret) {
			/* Withdraw the publication, the lock was not taken. */
			smp_store_release(&eng->hw_locked, false);
			eng->owner = 0;
			eng->driver_type = VCODEC_DRV_TYPE_NONE;
			eng->ops->power_off(eng);
			eng->powered = false;
			mutex_unlock(&eng->lock);
			wake_up_interruptible_all(&eng->hwlock_wq);
			dev_err(eng->dev, "enabling the irq failed: %d\n", ret);
			return ret;
		}
		eng->irq_enabled = true;
	}

	/*
	 * Downstream videocodec_kernel_driver_D1.c:858-860 starts the VDEC
	 * DVFS monitor here, after the power-on and the interrupt unmask, for
	 * secure and non-secure instances alike.
	 */
	if (eng->ops->dvfs_monitor_start)
		eng->ops->dvfs_monitor_start(eng);

	mutex_unlock(&eng->lock);

	dev_dbg(eng->dev, "engine locked by type %u (secure %d)\n",
		driver_type, secure);

	return 0;
}

/*
 * Release the hardware ownership lock.  Only the recorded owner may do so.
 * Downstream: videocodec_kernel_driver_D1.c:1120-1243.
 */
static long vcodec_ioc_unlockhw(u64 handle, u32 driver_type)
{
	struct vcodec_engine *eng;
	phys_addr_t owner;

	eng = vcodec_lookup_engine(driver_type);
	if (IS_ERR(eng))
		return PTR_ERR(eng);

	owner = vcodec_user_v2p(handle);
	if (!owner)
		return -EINVAL;

	mutex_lock(&eng->lock);
	if (!eng->hw_locked || eng->owner != owner) {
		mutex_unlock(&eng->lock);
		dev_warn(eng->dev, "handle 0x%llx is not the engine owner\n",
			 (unsigned long long)handle);
		return -EPERM;
	}

	if (eng->irq_enabled) {
		eng->ops->disable_irq(eng);
		eng->irq_enabled = false;
	}
	if (eng->powered) {
		eng->ops->power_off(eng);
		eng->powered = false;
	}

	/*
	 * Downstream videocodec_kernel_driver_D1.c:1094-1096 evaluates the
	 * DVFS monitor window once the device is powered off and before the
	 * lock is released.
	 */
	if (eng->ops->dvfs_adjustment)
		eng->ops->dvfs_adjustment(eng);

	/* Publish the release after the engine is quiescent. */
	smp_store_release(&eng->hw_locked, false);
	eng->owner = 0;
	eng->driver_type = VCODEC_DRV_TYPE_NONE;
	eng->irq_status = 0;
	mutex_unlock(&eng->lock);

	wake_up_interruptible_all(&eng->hwlock_wq);

	dev_dbg(eng->dev, "engine unlocked\n");

	return 0;
}

/*
 * Wait for the engine interrupt.  Downstream: videocodec_kernel_driver_D1.c:
 * 1244-1395.  A timeout is reported as -EFAULT because downstream's
 * dispatcher turns every failure of vcodec_waitisr() into -EFAULT before it
 * reaches userspace (lines 1443-1448), so the vendor libraries never see the
 * internal -2.  @status is only filled in for the encoder, matching
 * downstream which only returns status words for VENC (lines 1183-1197).
 */
static long vcodec_ioc_waitisr(u64 handle, u32 driver_type, u32 timeout_ms,
			       u32 irq_status_num, u32 *status)
{
	struct vcodec_engine *eng;
	phys_addr_t owner;
	unsigned long seq;
	long ret;

	/*
	 * Downstream's WAITISR knows the decode types and H264_ENC/HEVC_ENC
	 * only; the JPEG encoder falls into its "Unknown instance" branch
	 * (videocodec_kernel_driver_D1.c:1233-1236).  Report that as -EINVAL
	 * rather than downstream's -EFAULT.
	 */
	if (driver_type == VCODEC_DRV_TYPE_JPEG_ENC) {
		pr_debug("waitisr does not apply to the jpeg encoder\n");
		return -EINVAL;
	}

	eng = vcodec_lookup_engine(driver_type);
	if (IS_ERR(eng))
		return PTR_ERR(eng);

	owner = vcodec_user_v2p(handle);

	mutex_lock(&eng->lock);
	if (!eng->hw_locked || eng->owner != owner) {
		mutex_unlock(&eng->lock);
		dev_warn(eng->dev, "waitisr without holding the engine\n");
		return -EPERM;
	}
	seq = READ_ONCE(eng->isr_seq);
	mutex_unlock(&eng->lock);

	ret = wait_event_interruptible_timeout(eng->isr_wq,
					       READ_ONCE(eng->isr_seq) != seq,
					       msecs_to_jiffies(timeout_ms));
	if (!ret) {
		dev_dbg(eng->dev, "isr timeout after %u ms\n", timeout_ms);
		return -EFAULT;
	}
	if (ret < 0) {
		dev_dbg(eng->dev, "isr wait interrupted\n");
		return -ERESTARTSYS;
	}

	if (status && irq_status_num &&
	    eng->id == VCODEC_ENGINE_ENC)
		*status = READ_ONCE(eng->irq_status);

	return 0;
}

static long vcodec_ioc_emi_user(bool enc, bool increment, u32 __user *uval)
{
	u32 value;

	mutex_lock(&vcodec_emi_lock);
	if (enc) {
		if (increment)
			vcodec_enc_emi_users++;
		else if (vcodec_enc_emi_users)
			vcodec_enc_emi_users--;
		value = vcodec_enc_emi_users;
	} else {
		if (increment)
			vcodec_dec_emi_users++;
		else if (vcodec_dec_emi_users)
			vcodec_dec_emi_users--;
		value = vcodec_dec_emi_users;
	}
	mutex_unlock(&vcodec_emi_lock);

	if (put_user(value, uval))
		return -EFAULT;

	return 0;
}

static long vcodec_ioc_pwr_user(u32 driver_type, bool increment)
{
	enum vcodec_engine_id id;

	if (vcodec_driver_type_is_dec(driver_type))
		id = VCODEC_ENGINE_DEC;
	else if (vcodec_driver_type_is_enc(driver_type))
		id = VCODEC_ENGINE_ENC;
	else
		return -EINVAL;

	mutex_lock(&vcodec_pwr_lock);
	if (increment)
		vcodec_power_users[id]++;
	else if (vcodec_power_users[id])
		vcodec_power_users[id]--;
	pr_debug("engine %d power users: %u\n", id, vcodec_power_users[id]);
	mutex_unlock(&vcodec_pwr_lock);

	return 0;
}

static long vcodec_ioc_core_number(s32 __user *unr)
{
	s32 nr = nr_cpu_ids;

	if (put_user(nr, unr))
		return -EFAULT;

	return 0;
}

/*
 * Per-CPU load in percent over the interval since the previous query of the
 * same CPU.  Downstream called get_cpu_load() which does not exist here;
 * the same numbers are derived from the per-CPU kcpustat counters.
 */
static void vcodec_cpu_sample(int cpu, u64 *busy, u64 *total)
{
	struct kernel_cpustat kcs;
	u64 sum = 0, idle;
	int i;

	kcpustat_cpu_fetch(&kcs, cpu);
	for (i = 0; i < NR_STATS; i++)
		sum += kcpustat_field(&kcs, i, cpu);

	idle = kcpustat_field(&kcs, CPUTIME_IDLE, cpu) +
	       kcpustat_field(&kcs, CPUTIME_IOWAIT, cpu);

	*total = sum;
	*busy = sum > idle ? sum - idle : 0;
}

static long vcodec_ioc_core_loading(struct vcodec_core_loading *loading)
{
	u64 busy, total;
	s32 cpu = loading->cpu_id;

	/* nr_cpu_ids never exceeds NR_CPUS, so this bounds the arrays too. */
	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	vcodec_cpu_sample(cpu, &busy, &total);

	mutex_lock(&vcodec_cpu_load_lock);
	if (!vcodec_cpu_prev_valid[cpu]) {
		/* First sample for this CPU only provides the baseline. */
		loading->loading = 0;
		vcodec_cpu_prev_valid[cpu] = true;
	} else if (total > vcodec_cpu_prev_total[cpu] &&
		   busy >= vcodec_cpu_prev_busy[cpu]) {
		u64 d_total = total - vcodec_cpu_prev_total[cpu];
		u64 d_busy = busy - vcodec_cpu_prev_busy[cpu];

		loading->loading = d_busy >= d_total ? 100 :
				   (s32)div_u64(100 * d_busy, d_total);
	} else {
		loading->loading = 0;
	}
	vcodec_cpu_prev_busy[cpu] = busy;
	vcodec_cpu_prev_total[cpu] = total;
	mutex_unlock(&vcodec_cpu_load_lock);

	return 0;
}

static void vcodec_ioc_cpu_loading_info(struct vcodec_cpu_loading_info *info)
{
	/*
	 * Downstream copied an uninitialised stack struct here
	 * (videocodec_kernel_driver_D1.c:1595-1620); only the fields the
	 * kernel can actually provide are filled in.
	 */
	memset(info, 0, sizeof(*info));
	info->sched_clock = sched_clock();
	info->inst_count = vcodec_engine_inst_count();
}

static long vcodec_ioc_thread_id(const struct vcodec_thread_id *tid)
{
	if (tid->thread_num > VCODEC_THREAD_MAX_NUM)
		return -EINVAL;

	/* Downstream only logs the ids, there is no hardware action. */
	pr_debug("thread ids %u/%u, count %u\n", tid->tid1, tid->tid2,
		 tid->thread_num);

	return 0;
}

/*
 * VCODEC_SET_CPU_OPP_LIMIT.
 *
 * Port from Samsung/MTK downstream
 * drivers/misc/mediatek/videocodec/mt6735/videocodec_kernel_driver_D1.c:
 * 1533-1555, which copies VAL_VCODEC_CPU_OPP_LIMIT_T and then calls
 * cpu_opp_limit(EVENT_VIDEO, limited_freq, limited_cpu, enable).  That helper
 * exists neither in the downstream tree nor in the ROM, so the call is
 * commented out there.  The semantics below follow the argument names and
 * the downstream call site:
 *
 *   limited_freq  frequency ceiling in kHz
 *   limited_cpu   bitmask of CPUs; 0 means every CPU
 *   enable        1 applies the ceiling, 0 removes it
 *
 * They are [INFERENCE]: no available source defines cpu_opp_limit().  The
 * ceiling is expressed with the frequency QoS API on each policy's maximum
 * frequency constraint, which is what cpufreq-dt (CONFIG_CPUFREQ_DT, the
 * provider Nura uses) honours for every CPU sharing the policy.
 *
 * Unlike downstream's fire-and-forget call, every active request is dropped
 * when the last user closes /dev/Vcodec and at module exit, so a crashed
 * caller cannot leave the CPU capped.  Enabling again replaces the previous
 * ceiling set: policies the new mask does not select are released.  Those
 * are deliberate deviations.
 */
#if IS_ENABLED(CONFIG_CPU_FREQ)
struct vcodec_cpu_opp {
	struct cpufreq_policy *policy;
	struct freq_qos_request max_freq_req;
	bool visited;
};

/*
 * One entry per policy.  There can never be more policies than CPUs, so
 * NR_CPUS bounds the array; nr_cpu_ids bounds the CPUs we look at below.
 */
static DEFINE_MUTEX(vcodec_cpu_opp_lock);
static struct vcodec_cpu_opp vcodec_cpu_opps[NR_CPUS];

static struct vcodec_cpu_opp *vcodec_cpu_opp_find(struct cpufreq_policy *policy)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vcodec_cpu_opps); i++)
		if (vcodec_cpu_opps[i].policy == policy)
			return &vcodec_cpu_opps[i];

	return NULL;
}

static struct vcodec_cpu_opp *vcodec_cpu_opp_free(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vcodec_cpu_opps); i++)
		if (!vcodec_cpu_opps[i].policy)
			return &vcodec_cpu_opps[i];

	return NULL;
}

/* Drop one ceiling.  Caller holds vcodec_cpu_opp_lock. */
static void __vcodec_cpu_opp_release(struct vcodec_cpu_opp *opp)
{
	freq_qos_update_request(&opp->max_freq_req, FREQ_QOS_MAX_DEFAULT_VALUE);
	freq_qos_remove_request(&opp->max_freq_req);
	cpufreq_cpu_put(opp->policy);
	opp->policy = NULL;
	opp->visited = false;
}

/* Drop every active ceiling.  Caller holds vcodec_cpu_opp_lock. */
static void __vcodec_cpu_opp_release_all(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vcodec_cpu_opps); i++)
		if (vcodec_cpu_opps[i].policy)
			__vcodec_cpu_opp_release(&vcodec_cpu_opps[i]);
}

static void vcodec_cpu_opp_release_all(void)
{
	mutex_lock(&vcodec_cpu_opp_lock);
	__vcodec_cpu_opp_release_all();
	mutex_unlock(&vcodec_cpu_opp_lock);
}

static long vcodec_cpu_opp_enable(const struct vcodec_cpu_opp_limit *limit)
{
	u32 mask = (u32)limit->limited_cpu;
	int matched = 0;
	int applied = 0;
	int err = 0;
	int cpu, i;

	mutex_lock(&vcodec_cpu_opp_lock);

	for (i = 0; i < ARRAY_SIZE(vcodec_cpu_opps); i++)
		vcodec_cpu_opps[i].visited = false;

	for_each_possible_cpu(cpu) {
		struct vcodec_cpu_opp *opp;
		struct cpufreq_policy *policy;
		int ret;

		/*
		 * limited_cpu == 0 applies to every CPU.  It is the 32-bit ABI
		 * field, so CPUs at or above bit 31 can never be selected.
		 */
		if (mask && (cpu >= 32 || !((mask >> cpu) & 1)))
			continue;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		matched++;

		/*
		 * Several CPUs share one policy and its constraint set, so
		 * only one request per policy is added; later selections of
		 * the same policy reuse (and update) that request.
		 */
		opp = vcodec_cpu_opp_find(policy);
		if (opp) {
			opp->visited = true;
			ret = freq_qos_update_request(&opp->max_freq_req,
						      limit->limited_freq);
			cpufreq_cpu_put(policy);
			if (ret < 0) {
				pr_warn("cannot update cpu opp limit for cpu %d: %d\n",
					cpu, ret);
				if (!err)
					err = ret;
				continue;
			}
			applied++;
			continue;
		}

		opp = vcodec_cpu_opp_free();
		if (!opp) {
			/* Cannot happen: at most NR_CPUS policies. */
			pr_warn("cpu opp limit table is full\n");
			cpufreq_cpu_put(policy);
			if (!err)
				err = -ENOSPC;
			continue;
		}

		ret = freq_qos_add_request(&policy->constraints,
					   &opp->max_freq_req, FREQ_QOS_MAX,
					   limit->limited_freq);
		if (ret < 0) {
			pr_warn("cannot add cpu opp limit for cpu %d: %d\n",
				cpu, ret);
			cpufreq_cpu_put(policy);
			if (!err)
				err = ret;
			continue;
		}

		opp->policy = policy;
		opp->visited = true;
		applied++;

		pr_debug("cpu %d limited to %d kHz\n", cpu,
			 limit->limited_freq);
	}

	/*
	 * A re-issue replaces the previous ceiling set: policies that this
	 * call did not select must not stay capped.
	 */
	for (i = 0; i < ARRAY_SIZE(vcodec_cpu_opps); i++) {
		struct vcodec_cpu_opp *opp = &vcodec_cpu_opps[i];

		if (opp->policy && !opp->visited)
			__vcodec_cpu_opp_release(opp);
	}

	mutex_unlock(&vcodec_cpu_opp_lock);

	if (!matched)
		return -ENODEV;

	if (!applied)
		return err;

	return 0;
}

static long vcodec_cpu_opp_disable(void)
{
	mutex_lock(&vcodec_cpu_opp_lock);
	__vcodec_cpu_opp_release_all();
	mutex_unlock(&vcodec_cpu_opp_lock);

	return 0;
}

static long vcodec_ioc_cpu_opp_limit(const struct vcodec_cpu_opp_limit *limit)
{
	if (limit->enable != 0 && limit->enable != 1)
		return -EINVAL;

	if (limit->limited_cpu < 0)
		return -EINVAL;

	if (limit->enable && limit->limited_freq <= 0)
		return -EINVAL;

	if (limit->enable)
		return vcodec_cpu_opp_enable(limit);

	/*
	 * Removing the ceiling drops every request this driver owns, whatever
	 * the mask says: a stale cap is worse than an over-broad release, and
	 * it is the same operation the last-close path performs.
	 */
	return vcodec_cpu_opp_disable();
}
#else /* !CONFIG_CPU_FREQ */
static long vcodec_ioc_cpu_opp_limit(const struct vcodec_cpu_opp_limit *limit)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_CPU_FREQ */

static int vcodec_open(struct inode *inode, struct file *file)
{
	mutex_lock(&vcodec_open_lock);
	vcodec_open_count++;
	pr_debug("open by %s, %u open now\n", current->comm,
		 vcodec_open_count);
	mutex_unlock(&vcodec_open_lock);

	return 0;
}

static void vcodec_reset_counters(void)
{
	mutex_lock(&vcodec_emi_lock);
	vcodec_enc_emi_users = 0;
	vcodec_dec_emi_users = 0;
	mutex_unlock(&vcodec_emi_lock);

	mutex_lock(&vcodec_pwr_lock);
	memset(vcodec_power_users, 0, sizeof(vcodec_power_users));
	mutex_unlock(&vcodec_pwr_lock);

	mutex_lock(&vcodec_cpu_load_lock);
	memset(vcodec_cpu_prev_busy, 0, sizeof(vcodec_cpu_prev_busy));
	memset(vcodec_cpu_prev_total, 0, sizeof(vcodec_cpu_prev_total));
	memset(vcodec_cpu_prev_valid, 0, sizeof(vcodec_cpu_prev_valid));
	mutex_unlock(&vcodec_cpu_load_lock);
}

/*
 * Force both engines back to their idle state.  Downstream does the same
 * bookkeeping when the last file descriptor is closed
 * (videocodec_kernel_driver_D1.c:2042-2158).
 */
static void vcodec_cleanup_engines(void)
{
	struct vcodec_engine *eng;

	mutex_lock(&vcodec_engines_lock);
	list_for_each_entry(eng, &vcodec_engines, node) {
		mutex_lock(&eng->lock);
		if (eng->irq_enabled) {
			eng->ops->disable_irq(eng);
			eng->irq_enabled = false;
		}
		if (eng->powered) {
			eng->ops->power_off(eng);
			eng->powered = false;
		}
		eng->hw_locked = false;
		eng->owner = 0;
		eng->driver_type = VCODEC_DRV_TYPE_NONE;
		eng->irq_status = 0;
		/*
		 * Downstream videocodec_kernel_driver_D1.c:2121-2137 drops the
		 * decoder back to its lowest level and clears the monitor
		 * bookkeeping once the last user is gone.
		 */
		if (eng->ops->dvfs_idle)
			eng->ops->dvfs_idle(eng);
		mutex_unlock(&eng->lock);
		wake_up_interruptible_all(&eng->hwlock_wq);
	}
	mutex_unlock(&vcodec_engines_lock);

	vcodec_reset_counters();
}

static int vcodec_release(struct inode *inode, struct file *file)
{
	bool last = false;

	mutex_lock(&vcodec_open_lock);
	if (vcodec_open_count)
		vcodec_open_count--;
	if (!vcodec_open_count)
		last = true;
	pr_debug("release by %s, %u open left\n", current->comm,
		 vcodec_open_count);
	mutex_unlock(&vcodec_open_lock);

	if (last) {
		pr_debug("last user gone, forcing engines idle\n");
		vcodec_cleanup_engines();
#if IS_ENABLED(CONFIG_CPU_FREQ)
		/*
		 * The caller that set a CPU ceiling may be gone without
		 * clearing it; drop it with the rest of the session state.
		 */
		vcodec_cpu_opp_release_all();
#endif
	}

	return 0;
}

/*
 * Map a codec register window.  Only ranges that lie completely inside one
 * of the regions an engine registered are accepted.
 *
 * Downstream additionally allowed any address inside four hardcoded
 * physical windows (videocodec_kernel_driver_D1.c:2178-2205), which lets
 * userspace map unrelated MMIO.  That is not reproduced here.
 */
static int vcodec_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long length = vma->vm_end - vma->vm_start;
	phys_addr_t start = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	phys_addr_t end;
	struct vcodec_engine *eng;
	bool allowed = false;

	if (!length || !PAGE_ALIGNED(vma->vm_start) || !PAGE_ALIGNED(length))
		return -EINVAL;

	end = start + length;
	if (end < start)
		return -EINVAL;

	mutex_lock(&vcodec_engines_lock);
	list_for_each_entry(eng, &vcodec_engines, node) {
		unsigned int i;

		for (i = 0; i < eng->num_regions; i++) {
			phys_addr_t rstart = eng->regions[i].phys;
			phys_addr_t rend = rstart + eng->regions[i].size;

			if (start >= rstart && end <= rend) {
				allowed = true;
				break;
			}
		}
		if (allowed)
			break;
	}
	mutex_unlock(&vcodec_engines_lock);

	if (!allowed) {
		pr_debug("rejecting mmap of 0x%llx+0x%lx\n",
			 (u64)start, length);
		return -EINVAL;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, length,
			    vma->vm_page_prot))
		return -EAGAIN;

	pr_debug("mapped 0x%llx+0x%lx\n", (u64)start, length);

	return 0;
}

/*
 * Commands whose downstream handlers never did anything.  They are refused
 * instead of silently succeeding, so userspace can tell that the service is
 * not available on this kernel.
 */
static const unsigned int vcodec_unsupported_cmds[] = {
	MFV_TEST_CMD,
	MFV_INIT_CMD,
	MFV_DEINIT_CMD,
	MFV_SET_CMD_CMD,
	MFV_SET_PWR_CMD,
	MFV_SET_ISR_CMD,
	MFV_ALLOC_MEM_CMD,
	MFV_FREE_MEM_CMD,
	MFV_MAKE_PMEM_TO_NONCACHED,
	MFV_ALLOC_INT_MEM_CMD,
	MFV_FREE_INT_MEM_CMD,
	VCODEC_PMEM_FLUSH,
	VCODEC_PMEM_CLEAN,
	VCODEC_INC_SYSRAM_USER,
	VCODEC_DEC_SYSRAM_USER,
	VCODEC_SET_SYSRAM_INFO,
	VCODEC_GET_SYSRAM_INFO,
	VCODEC_ALLOC_NON_CACHE_BUFFER,
	VCODEC_FREE_NON_CACHE_BUFFER,
};

static bool vcodec_cmd_is_unsupported(unsigned int cmd)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vcodec_unsupported_cmds); i++)
		if (vcodec_unsupported_cmds[i] == cmd)
			return true;

	return false;
}

static long vcodec_unlocked_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct vcodec_hw_lock hwlock;
	struct vcodec_power power;
	struct vcodec_isr isr;
	struct vcodec_thread_id thread_id;
	struct vcodec_cpu_opp_limit cpu_opp;
	struct vcodec_core_loading loading;
	struct vcodec_cpu_loading_info info;
	u32 status = 0;
	long ret;

	switch (cmd) {
	case VCODEC_LOCKHW:
		if (copy_from_user(&hwlock, uarg, sizeof(hwlock)))
			return -EFAULT;
		return vcodec_ioc_lockhw(hwlock.pv_handle, hwlock.timeout_ms,
					 hwlock.driver_type,
					 !!hwlock.secure_inst);
	case VCODEC_UNLOCKHW:
		if (copy_from_user(&hwlock, uarg, sizeof(hwlock)))
			return -EFAULT;
		return vcodec_ioc_unlockhw(hwlock.pv_handle,
					   hwlock.driver_type);
	case VCODEC_WAITISR:
		if (copy_from_user(&isr, uarg, sizeof(isr)))
			return -EFAULT;
		ret = vcodec_ioc_waitisr(isr.pv_handle, isr.driver_type,
					 isr.timeout_ms, isr.irq_status_num,
					 &status);
		if (ret)
			return ret;
		if (isr.irq_status_num &&
		    vcodec_driver_type_is_enc(isr.driver_type)) {
			isr.irq_status[0] = status;
			if (copy_to_user(uarg, &isr, sizeof(isr)))
				return -EFAULT;
		}
		return 0;
	case VCODEC_INC_ENC_EMI_USER:
		return vcodec_ioc_emi_user(true, true, uarg);
	case VCODEC_DEC_ENC_EMI_USER:
		return vcodec_ioc_emi_user(true, false, uarg);
	case VCODEC_INC_DEC_EMI_USER:
		ret = vcodec_ioc_emi_user(false, true, uarg);
		if (!ret)
			vcodec_dvfs_begin_dec();
		return ret;
	case VCODEC_DEC_DEC_EMI_USER:
		return vcodec_ioc_emi_user(false, false, uarg);
	case VCODEC_INC_PWR_USER:
	case VCODEC_DEC_PWR_USER:
		if (copy_from_user(&power, uarg, sizeof(power)))
			return -EFAULT;
		return vcodec_ioc_pwr_user(power.driver_type,
					   cmd == VCODEC_INC_PWR_USER);
	case VCODEC_GET_CORE_NUMBER:
		return vcodec_ioc_core_number(uarg);
	case VCODEC_GET_CORE_LOADING:
		if (copy_from_user(&loading, uarg, sizeof(loading)))
			return -EFAULT;
		ret = vcodec_ioc_core_loading(&loading);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &loading, sizeof(loading)))
			return -EFAULT;
		return 0;
	case VCODEC_GET_CPU_LOADING_INFO:
		vcodec_ioc_cpu_loading_info(&info);
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	case VCODEC_SET_THREAD_ID:
		if (copy_from_user(&thread_id, uarg, sizeof(thread_id)))
			return -EFAULT;
		return vcodec_ioc_thread_id(&thread_id);
	case VCODEC_SET_CPU_OPP_LIMIT:
		if (copy_from_user(&cpu_opp, uarg, sizeof(cpu_opp)))
			return -EFAULT;
		return vcodec_ioc_cpu_opp_limit(&cpu_opp);
	case VCODEC_INITHWLOCK:
	case VCODEC_DEINITHWLOCK:
		/* Downstream only logs these, the lock is owned by this file. */
		pr_debug("cmd 0x%x accepted\n", cmd);
		return 0;
	case VCODEC_MB:
		/* Downstream: full barrier, no other effect. */
		mb();
		return 0;
	default:
		if (vcodec_cmd_is_unsupported(cmd)) {
			pr_debug("cmd 0x%x is not supported\n", cmd);
			return -EOPNOTSUPP;
		}
		pr_debug("unknown cmd 0x%x\n", cmd);
		return -ENOTTY;
	}
}

#if IS_ENABLED(CONFIG_COMPAT)
/*
 * 32-bit layouts, mirroring COMPAT_VAL_HW_LOCK_T, COMPAT_VAL_POWER_T and
 * COMPAT_VAL_ISR_T of videocodec_kernel_driver_D1.c:1672-1740.  They are
 * read field by field so no padding assumptions are made.
 */
struct vcodec_compat_hw_lock {
	__u32 pv_handle;
	__u32 handle_size;
	__u32 pv_lock;
	__u32 timeout_ms;
	__u32 pv_reserved;
	__u32 reserved_size;
	__u32 driver_type;
	__u8 secure_inst;
};

struct vcodec_compat_power {
	__u32 pv_handle;
	__u32 handle_size;
	__u32 driver_type;
	__u8 enable;
	__u32 pv_reserved;
	__u32 reserved_size;
};

struct vcodec_compat_isr {
	__u32 pv_handle;
	__u32 handle_size;
	__u32 driver_type;
	__u32 pv_isr_function;
	__u32 pv_reserved;
	__u32 reserved_size;
	__u32 timeout_ms;
	__u32 irq_status_num;
	__u32 irq_status[VCODEC_IRQ_STATUS_MAX_NUM];
};

/*
 * VCODEC_GET_CPU_LOADING_INFO payload of a 32-bit caller: three u64 followed
 * by one u32, with no trailing field of our own.  Kept separate from the
 * native struct so nothing beyond @inst_count is ever written.
 */
struct vcodec_compat_cpu_loading_info {
	__u64 cpu_idle_time;
	__u64 thread_cpu_time;
	__u64 sched_clock;
	__u32 inst_count;
};

static long vcodec_compat_lockhw(struct vcodec_compat_hw_lock __user *ucompat)
{
	u32 handle, timeout_ms, driver_type;
	u8 secure;

	if (get_user(handle, &ucompat->pv_handle) ||
	    get_user(timeout_ms, &ucompat->timeout_ms) ||
	    get_user(driver_type, &ucompat->driver_type) ||
	    get_user(secure, &ucompat->secure_inst))
		return -EFAULT;

	return vcodec_ioc_lockhw(handle, timeout_ms, driver_type, !!secure);
}

static long vcodec_compat_unlockhw(struct vcodec_compat_hw_lock __user *ucompat)
{
	u32 handle, driver_type;

	if (get_user(handle, &ucompat->pv_handle) ||
	    get_user(driver_type, &ucompat->driver_type))
		return -EFAULT;

	return vcodec_ioc_unlockhw(handle, driver_type);
}

static long vcodec_compat_waitisr(struct vcodec_compat_isr __user *ucompat)
{
	u32 handle, driver_type, timeout_ms, irq_status_num;
	u32 status = 0;
	long ret;

	if (get_user(handle, &ucompat->pv_handle) ||
	    get_user(driver_type, &ucompat->driver_type) ||
	    get_user(timeout_ms, &ucompat->timeout_ms) ||
	    get_user(irq_status_num, &ucompat->irq_status_num))
		return -EFAULT;

	ret = vcodec_ioc_waitisr(handle, driver_type, timeout_ms,
				 irq_status_num, &status);
	if (ret)
		return ret;

	if (irq_status_num && vcodec_driver_type_is_enc(driver_type)) {
		if (put_user(status, &ucompat->irq_status[0]))
			return -EFAULT;
	}

	return 0;
}

static long vcodec_compat_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	void __user *uarg = compat_ptr(arg);
	struct vcodec_cpu_opp_limit cpu_opp;
	struct vcodec_core_loading loading;
	struct vcodec_cpu_loading_info info;
	struct vcodec_thread_id thread_id;
	long ret;

	switch (cmd) {
	case VCODEC_LOCKHW:
		return vcodec_compat_lockhw(uarg);
	case VCODEC_UNLOCKHW:
		return vcodec_compat_unlockhw(uarg);
	case VCODEC_WAITISR:
		return vcodec_compat_waitisr(uarg);
	case VCODEC_INC_PWR_USER:
	case VCODEC_DEC_PWR_USER: {
		u32 driver_type;

		if (get_user(driver_type,
			     &((struct vcodec_compat_power __user *)uarg)->driver_type))
			return -EFAULT;
		return vcodec_ioc_pwr_user(driver_type,
					   cmd == VCODEC_INC_PWR_USER);
	}
	case VCODEC_INC_ENC_EMI_USER:
		return vcodec_ioc_emi_user(true, true, uarg);
	case VCODEC_DEC_ENC_EMI_USER:
		return vcodec_ioc_emi_user(true, false, uarg);
	case VCODEC_INC_DEC_EMI_USER:
		ret = vcodec_ioc_emi_user(false, true, uarg);
		if (!ret)
			vcodec_dvfs_begin_dec();
		return ret;
	case VCODEC_DEC_DEC_EMI_USER:
		return vcodec_ioc_emi_user(false, false, uarg);
	case VCODEC_GET_CORE_NUMBER:
		return vcodec_ioc_core_number(uarg);
	case VCODEC_GET_CORE_LOADING:
		/* Two s32 fields, identical on both word sizes. */
		if (copy_from_user(&loading, uarg, sizeof(loading)))
			return -EFAULT;
		ret = vcodec_ioc_core_loading(&loading);
		if (ret)
			return ret;
		if (copy_to_user(uarg, &loading, sizeof(loading)))
			return -EFAULT;
		return 0;
	case VCODEC_GET_CPU_LOADING_INFO: {
		struct vcodec_compat_cpu_loading_info __user *uinfo = uarg;

		vcodec_ioc_cpu_loading_info(&info);
		/*
		 * Field by field: the fields sit at the same offsets on both
		 * word sizes, but the tail padding after @inst_count belongs
		 * to the caller's struct and must not be written from here.
		 */
		if (put_user(info.cpu_idle_time, &uinfo->cpu_idle_time) ||
		    put_user(info.thread_cpu_time, &uinfo->thread_cpu_time) ||
		    put_user(info.sched_clock, &uinfo->sched_clock) ||
		    put_user(info.inst_count, &uinfo->inst_count))
			return -EFAULT;
		return 0;
	}
	case VCODEC_SET_THREAD_ID:
		/* u32 only, identical on both word sizes. */
		if (copy_from_user(&thread_id, uarg, sizeof(thread_id)))
			return -EFAULT;
		return vcodec_ioc_thread_id(&thread_id);
	case VCODEC_SET_CPU_OPP_LIMIT:
		/* Three s32 fields, identical on both word sizes. */
		if (copy_from_user(&cpu_opp, uarg, sizeof(cpu_opp)))
			return -EFAULT;
		return vcodec_ioc_cpu_opp_limit(&cpu_opp);
	case VCODEC_MB:
		/* Downstream: full barrier, no other effect. */
		mb();
		return 0;
	default:
		/*
		 * Same distinction as the native dispatcher: a command that
		 * exists in the ABI but has no implementation is refused with
		 * -EOPNOTSUPP, anything else is not our ioctl at all.  Without
		 * this a 32-bit caller would get -ENOTTY for the very commands
		 * a 64-bit caller sees as -EOPNOTSUPP.
		 */
		if (vcodec_cmd_is_unsupported(cmd))
			return -EOPNOTSUPP;
		return -ENOTTY;
	}
}
#endif /* CONFIG_COMPAT */

static const struct file_operations vcodec_fops = {
	.owner		= THIS_MODULE,
	.open		= vcodec_open,
	.release	= vcodec_release,
	.unlocked_ioctl	= vcodec_unlocked_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.compat_ioctl	= vcodec_compat_ioctl,
#endif
	.mmap		= vcodec_mmap,
};

static struct miscdevice vcodec_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= VCODEC_DEVNAME,
	.fops	= &vcodec_fops,
	.mode	= 0660,
};

static int __init vcodec_core_init(void)
{
	int ret;

	vcodec_reset_counters();

	ret = misc_register(&vcodec_miscdev);
	if (ret) {
		pr_err("cannot register /dev/%s: %d\n", VCODEC_DEVNAME, ret);
		return ret;
	}

	ret = vcodec_vdec_register();
	if (ret) {
		pr_err("cannot register the decoder engine: %d\n", ret);
		goto err_misc;
	}

	ret = vcodec_venc_register();
	if (ret) {
		pr_err("cannot register the encoder engine: %d\n", ret);
		goto err_vdec;
	}

	pr_info("/dev/%s ready\n", VCODEC_DEVNAME);

	return 0;

err_vdec:
	vcodec_vdec_unregister();
err_misc:
	misc_deregister(&vcodec_miscdev);

	return ret;
}

static void __exit vcodec_core_exit(void)
{
#if IS_ENABLED(CONFIG_CPU_FREQ)
	vcodec_cpu_opp_release_all();
#endif
	vcodec_venc_unregister();
	vcodec_vdec_unregister();
	misc_deregister(&vcodec_miscdev);
}

module_init(vcodec_core_init);
module_exit(vcodec_core_exit);

MODULE_AUTHOR("Legis, Lu <legis.lu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek MT6735 video codec hardware service");
MODULE_LICENSE("GPL");
