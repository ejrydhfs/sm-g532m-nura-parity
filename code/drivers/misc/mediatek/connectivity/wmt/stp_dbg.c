// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN STP debug / coredump generic-netlink ABI.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/linux/pri/stp_dbg.c
 *     (STP_DBG family, stp_dbg_nl_bind/reset/send, stp_dbg.c:83-160,
 *      1240-1320)
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * Userspace contract (unchanged from downstream):
 *   family name  : "STP_DBG"
 *   commands     : 1 = STP_DBG_COMMAND_BIND, 2 = STP_DBG_COMMAND_RESET
 *   attributes   : 1 = STP_DBG_ATTR_MSG
 *
 * A vendor daemon binds by sending STP_DBG_COMMAND_BIND; the kernel records
 * its netlink port id.  Coredump data is then pushed with
 * STP_DBG_COMMAND_RESET and the payload in STP_DBG_ATTR_MSG, framed as
 *
 *	'[' 'M' ']' | u16 chunk_len | chunk_len bytes
 *
 * which is exactly what _stp_btm_put_emi_dump_to_nl() produced downstream
 * (btm_core.c:189-242).
 */

#include <linux/module.h>
#include <linux/netlink.h>
#include <net/genetlink.h>

#include "wmt_local.h"

#define STP_DBG_PFX	"STPDbg: "

#define stp_dbg_err(fmt, ...)	pr_err(STP_DBG_PFX fmt, ##__VA_ARGS__)
#define stp_dbg_warn(fmt, ...)	pr_warn(STP_DBG_PFX fmt, ##__VA_ARGS__)
#define stp_dbg_info(fmt, ...)	pr_info(STP_DBG_PFX fmt, ##__VA_ARGS__)
#define stp_dbg_dbg(fmt, ...)	pr_debug(STP_DBG_PFX fmt, ##__VA_ARGS__)

#define STP_DBG_FAMILY_NAME	"STP_DBG"
#define STP_DBG_FAMILY_VERSION	1
#define STP_DBG_MAX_BIND	4

enum {
	__STP_DBG_ATTR_INVALID,
	STP_DBG_ATTR_MSG,
	__STP_DBG_ATTR_MAX,
};
#define STP_DBG_ATTR_MAX	(__STP_DBG_ATTR_MAX - 1)

enum {
	__STP_DBG_COMMAND_INVALID,
	STP_DBG_COMMAND_BIND,
	STP_DBG_COMMAND_RESET,
	__STP_DBG_COMMAND_MAX,
};

static int stp_dbg_bind_pid[STP_DBG_MAX_BIND];
static int stp_dbg_bind_num;
static u32 stp_dbg_seqnum;
static DEFINE_MUTEX(stp_dbg_nl_lock);

static int stp_dbg_nl_bind(struct sk_buff *skb, struct genl_info *info)
{
	int i;
	int ret = 0;

	if (!info)
		return 0;

	mutex_lock(&stp_dbg_nl_lock);

	for (i = 0; i < stp_dbg_bind_num; i++) {
		if (stp_dbg_bind_pid[i] == info->snd_portid) {
			stp_dbg_info("pid %d already bound\n", info->snd_portid);
			goto out;
		}
	}

	if (stp_dbg_bind_num >= STP_DBG_MAX_BIND) {
		stp_dbg_warn("exceeding binding limit %d\n", STP_DBG_MAX_BIND);
		ret = -ENOSPC;
		goto out;
	}

	stp_dbg_bind_pid[stp_dbg_bind_num++] = info->snd_portid;
	stp_dbg_info("pid %d bound\n", info->snd_portid);
out:
	mutex_unlock(&stp_dbg_nl_lock);
	return ret;
}

static int stp_dbg_nl_reset(struct sk_buff *skb, struct genl_info *info)
{
	/* Downstream stp_dbg.c:1267-1271: the command is reserved for the
	 * kernel-to-daemon direction and must not be invoked.
	 */
	stp_dbg_warn("STP_DBG_COMMAND_RESET is kernel-to-userspace only\n");
	return 0;
}

static const struct nla_policy stp_dbg_genl_policy[STP_DBG_ATTR_MAX + 1] = {
	[STP_DBG_ATTR_MSG] = { .type = NLA_NUL_STRING },
};

static const struct genl_ops stp_dbg_gnl_ops[] = {
	{
		.cmd = STP_DBG_COMMAND_BIND,
		.doit = stp_dbg_nl_bind,
		.policy = stp_dbg_genl_policy,
	},
	{
		.cmd = STP_DBG_COMMAND_RESET,
		.doit = stp_dbg_nl_reset,
		.policy = stp_dbg_genl_policy,
	},
};

static struct genl_family stp_dbg_gnl_family = {
	.name = STP_DBG_FAMILY_NAME,
	.version = STP_DBG_FAMILY_VERSION,
	.maxattr = STP_DBG_ATTR_MAX,
	.module = THIS_MODULE,
	.ops = stp_dbg_gnl_ops,
	.n_ops = ARRAY_SIZE(stp_dbg_gnl_ops),
};

int stp_dbg_nl_send(const u8 *data, u8 cmd, u32 len)
{
	struct sk_buff *skb;
	void *msg_head;
	int i, rc;

	if (!len || !data)
		return -EINVAL;

	mutex_lock(&stp_dbg_nl_lock);

	if (!stp_dbg_bind_num) {
		mutex_unlock(&stp_dbg_nl_lock);
		stp_dbg_dbg("no bound process, %u bytes dropped\n", len);
		return 0;
	}

	for (i = 0; i < stp_dbg_bind_num; i++) {
		skb = genlmsg_new(nla_total_size(len) + GENL_HDRLEN,
				  GFP_KERNEL);
		if (!skb) {
			mutex_unlock(&stp_dbg_nl_lock);
			return -ENOMEM;
		}

		msg_head = genlmsg_put(skb, 0, stp_dbg_seqnum++,
				       &stp_dbg_gnl_family, 0, cmd);
		if (!msg_head) {
			nlmsg_free(skb);
			mutex_unlock(&stp_dbg_nl_lock);
			return -EMSGSIZE;
		}

		rc = nla_put(skb, STP_DBG_ATTR_MSG, len, data);
		if (rc) {
			nlmsg_free(skb);
			mutex_unlock(&stp_dbg_nl_lock);
			return rc;
		}

		genlmsg_end(skb, msg_head);

		rc = genlmsg_unicast(&init_net, skb, stp_dbg_bind_pid[i]);
		if (rc) {
			/* A process that died between bind and now: drop it. */
			stp_dbg_warn("unicast to %d failed (%d)\n",
				     stp_dbg_bind_pid[i], rc);
			stp_dbg_bind_pid[i] = stp_dbg_bind_pid[--stp_dbg_bind_num];
			i--;
		}
	}

	mutex_unlock(&stp_dbg_nl_lock);
	return 0;
}

int stp_dbg_nl_init(void)
{
	int ret;

	ret = genl_register_family(&stp_dbg_gnl_family);
	if (ret) {
		stp_dbg_err("generic netlink family registration failed (%d)\n",
			    ret);
		return ret;
	}

	stp_dbg_info("family %s registered\n", STP_DBG_FAMILY_NAME);
	return 0;
}

void stp_dbg_nl_deinit(void)
{
	genl_unregister_family(&stp_dbg_gnl_family);

	mutex_lock(&stp_dbg_nl_lock);
	stp_dbg_bind_num = 0;
	mutex_unlock(&stp_dbg_nl_lock);
}
