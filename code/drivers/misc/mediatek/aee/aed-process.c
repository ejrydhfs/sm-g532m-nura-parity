// SPDX-License-Identifier: GPL-2.0
/*
 * Per-process backtrace for the AEEIOCTL_GET_PROCESS_BT dump ioctl.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/aee/aed/aed-process.c:35-204 (aed_get_process_bt,
 * aed_get_bt, aed_save_trace and the cross-CPU barrier).  Original
 * copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 *
 * The result is the aee_process_bt frame array of
 * include/mt-plat/aee.h:65-84: for every PC a 64-bit address plus a %pS
 * symbol.  The downstream ARM32 code also fills frame->lr; its arm64 build
 * leaves lr/lr_symbol zero (aed-process.c:88-91), which is what this port
 * does as well.
 *
 * DEVIATION: the upstream walker (walk_stackframe() with a task stackframe)
 * is replaced by stack_trace_save_tsk(), the architected arm64 unwinder, and
 * the cross-CPU "stop the other CPUs" barrier of aed-process.c:139-166 is not
 * reproduced: it only made the dump more atomic on a 32-bit kernel and needs
 * the downstream arch/stacktrace.h internals.
 */
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/string.h>

#include "aed.h"

int aed_get_process_bt(struct aee_process_bt *bt)
{
	struct aee_bt_frame *frames;
	struct task_struct *task;
	unsigned long *pcs;
	bool allowed;
	unsigned int nr, i;
	int ret = 0;

	if (!bt || !bt->entries || (int)bt->pid <= 0)
		return -EINVAL;

	frames = bt->entries;
	memset(frames, 0, sizeof(*frames) * AEE_NR_FRAME);
	bt->nr_entries = 0;

	task = aed_find_task(bt->pid);
	if (!task)
		return -EINVAL;

	pcs = kcalloc(AEE_NR_FRAME, sizeof(*pcs), GFP_KERNEL);
	if (!pcs) {
		ret = -ENOMEM;
		goto out_put_task;
	}

	/* aed-process.c:181-190 */
	ret = mutex_lock_killable(&task->signal->cred_guard_mutex);
	if (ret)
		goto out_free;

	allowed = ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS);
	mutex_unlock(&task->signal->cred_guard_mutex);
	if (!allowed) {
		ret = -EPERM;
		goto out_free;
	}

	nr = stack_trace_save_tsk(task, pcs, AEE_NR_FRAME, 0);
	for (i = 0; i < nr; i++) {
		frames[i].pc = pcs[i];
		snprintf(frames[i].pc_symbol, AEE_SZ_SYMBOL_S, "%pS",
			 (void *)pcs[i]);
	}
	bt->nr_entries = nr;

out_free:
	kfree(pcs);
out_put_task:
	put_task_struct(task);
	return ret;
}
