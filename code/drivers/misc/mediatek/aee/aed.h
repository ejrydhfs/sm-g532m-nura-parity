/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek AED (Android Exception Device) driver private definitions.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/aee/aed/aed-main.c:155-200
 *     (struct aed_eerec, struct aed_kerec, struct aed_dev)
 *   drivers/misc/mediatek/include/mt-plat/aee.h:80-84 (struct aee_process_bt)
 *
 * Original copyright: Copyright (C) 2010/2015 MediaTek Inc. GPL-2.0.
 */
#ifndef __MTK_AED_H
#define __MTK_AED_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <uapi/linux/aee.h>

/*
 * aed-main.c:155-167.  exp_linenum/fatal1/fatal2 of the downstream record are
 * only used by the legacy aed_md_exception1()/aed_md32_exception1() entry
 * points, which this tree does not have; the remaining fields are the ones the
 * AE_Msg response generators and the /proc coredump reader use.
 */
struct aed_eerec {		/* external exception record */
	struct list_head list;
	char assert_type[32];
	char exp_filename[512];

	int *ee_log;
	int ee_log_size;
	int *ee_phy;
	int ee_phy_size;
	char *msg;
	int db_opt;
};

/* include/mt-plat/aee.h:80-84 */
struct aee_process_bt {
	__u32 pid;
	__u32 nr_entries;
	struct aee_bt_frame *entries;
};

int aed_get_process_bt(struct aee_process_bt *bt);

/* look up a process by pid in the caller's pid namespace, with a reference */
static inline struct task_struct *aed_find_task(u32 vpid)
{
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_vpid((pid_t)vpid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}

#endif /* __MTK_AED_H */
