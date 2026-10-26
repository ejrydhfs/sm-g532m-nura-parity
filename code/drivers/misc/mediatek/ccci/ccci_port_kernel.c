// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI kernel ports: control, system, status and RPC channels handled inside
 * the kernel, plus the kernel service API used by other subsystems.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_kernel.c.  Original copyright
 * (C) 2015 MediaTek Inc., GPL-2.0.
 *
 * Deviations from the ROM, all because the corresponding provider does not
 * exist in this kernel tree (documented in the leaf report):
 *   - ID_GET_TXPOWER needs the thermal/DVFS clients; the service still
 *     forwards the system message, and unknown ids return -EOPNOTSUPP.
 *
 * Kernel RPC requests are handed to the RPC engine in ccci_rpc.c, the
 * counterpart of downstream port_kernel_thread() calling rpc_msg_handler()
 * (port_kernel.c:2108-2113).
 */
#include <linux/ccci.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "ccci_priv.h"
#include "ccci_rpc.h"

#define CCCI_MAX_KERN_API	35

struct ccci_md;

struct ccci_sys_cb {
	unsigned int id;
	int (*func)(int md_id, int data);
};

static struct ccci_sys_cb ccci_sys_cb_100[CCCI_MAX_MD_NUM][CCCI_MAX_KERN_API];
static struct ccci_sys_cb ccci_sys_cb_1000[CCCI_MAX_MD_NUM][CCCI_MAX_KERN_API];
static DEFINE_MUTEX(ccci_sys_cb_lock);

/* ------------------------------------------------------------------ */
/* system callback dispatch                                            */
/* ------------------------------------------------------------------ */
static void ccci_exec_sys_cb(struct ccci_md *md, unsigned int id, int data)
{
	struct ccci_sys_cb *slot = NULL;

	if (id >= 0x1000 && id - 0x1000 < CCCI_MAX_KERN_API)
		slot = &ccci_sys_cb_1000[md->id][id - 0x1000];
	else if (id >= 0x100 && id - 0x100 < CCCI_MAX_KERN_API)
		slot = &ccci_sys_cb_100[md->id][id - 0x100];
	if (!slot || !slot->func) {
		ccci_md_dbg(md, "no kernel callback for message 0x%x\n", id);
		return;
	}
	slot->func(md->id, data);
}

int register_ccci_sys_call_back(int md_id, unsigned int id,
				int (*func)(int md_id, int data))
{
	struct ccci_sys_cb *slot = NULL;
	int ret = 0;

	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM || !func)
		return -EINVAL;

	mutex_lock(&ccci_sys_cb_lock);
	if (id >= 0x1000 && id - 0x1000 < CCCI_MAX_KERN_API)
		slot = &ccci_sys_cb_1000[md_id][id - 0x1000];
	else if (id >= 0x100 && id - 0x100 < CCCI_MAX_KERN_API)
		slot = &ccci_sys_cb_100[md_id][id - 0x100];
	else
		ret = -EINVAL;

	if (!ret) {
		if (slot->func) {
			pr_err("ccci: sys callback 0x%x already registered\n", id);
			ret = -EBUSY;
		} else {
			slot->id = id;
			slot->func = func;
		}
	}
	mutex_unlock(&ccci_sys_cb_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(register_ccci_sys_call_back);

/* ------------------------------------------------------------------ */
/* boot count (throttling feature, ROM ccci_get_md_boot_count)         */
/* ------------------------------------------------------------------ */
unsigned long ccci_get_md_boot_count(int md_id)
{
	struct ccci_md *md;

	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM)
		return 0;
	md = ccci_md_get(md_id);
	return md ? md->boot_count : 0;
}
EXPORT_SYMBOL_GPL(ccci_get_md_boot_count);

/* ------------------------------------------------------------------ */
/* kernel service API (ROM exec_ccci_kern_func_by_md_id)               */
/* ------------------------------------------------------------------ */
int exec_ccci_kern_func_by_md_id(int md_id, unsigned int id, char *buf,
				 unsigned int len)
{
	struct ccci_md *md = ccci_md_get(md_id);
	int val = 0;
	int ret = 0;

	if (!md)
		return -ENODEV;

	if (buf && len >= sizeof(int))
		memcpy(&val, buf, sizeof(int));

	switch (id) {
	case ID_GET_MD_WAKEUP_SRC:
		/* SPM asks who woke the AP; the CCIF Rx path marks it */
		md->wakeup_src = 1;
		break;
	case ID_GET_MD_STATE:
		ret = md->boot_stage;
		break;
	case ID_RESET_MD:
		ret = ccci_md_reset(md);
		if (!ret)
			ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
						       CCCI_MD_MSG_RESET, 0);
		break;
	case ID_PAUSE_LTE:
		/* DVFS: >0 means "modem not available", 0 delivered */
		if (md->state != CCCI_MD_READY) {
			ret = 1;
			break;
		}
		ret = ccci_send_msg_to_md(md, CCCI_SYSTEM_TX, MD_PAUSE_LTE,
					  val, true);
		if (ret == -CCCI_ERR_MD_NOT_READY ||
		    ret == -CCCI_ERR_HIF_NOT_POWER_ON)
			ret = 1;
		break;
	case ID_GET_TXPOWER:
		if (!buf)
			return -EINVAL;
		if (val == 0)
			ret = ccci_send_msg_to_md(md, CCCI_SYSTEM_TX,
						  MD_TX_POWER, 0, false);
		else if (val == 1)
			ret = ccci_send_msg_to_md(md, CCCI_SYSTEM_TX,
						  MD_RF_TEMPERATURE, 0, false);
		else if (val == 2)
			ret = ccci_send_msg_to_md(md, CCCI_SYSTEM_TX,
						  MD_RF_TEMPERATURE_3G, 0,
						  false);
		else
			ret = -EINVAL;
		break;
	case ID_STORE_SIM_SWITCH_MODE:
		/* no SIM switch driver in this tree: keep the value locally */
		md->sim_switch_mode = val;
		break;
	case ID_GET_SIM_SWITCH_MODE:
		if (!buf)
			return -EINVAL;
		memcpy(buf, &md->sim_switch_mode, sizeof(int));
		break;
	case ID_THROTTLING_CFG:
		/* no MD throughput throttling client in this tree */
		ret = -EOPNOTSUPP;
		break;
	case ID_DUMP_MD_SLEEP_MODE:
		if (md->smem_vir) {
			ccci_md_info(md, "MD sleep debug at smem+0x%x\n",
				     CCCI_SMEM_SIZE_EXCEPTION -
				     CCCI_SMEM_SLEEP_MODE_DBG_SIZE);
		}
		break;
	case ID_DUMP_MD_REG:
		ccci_ccif_dump_regs(md, NULL);
		break;
	case ID_UPDATE_MD_BOOT_MODE:
		if (val > 0 && val < 3)
			md->md_boot_mode = val;
		else
			ret = -EINVAL;
		break;
	case MD_THERMAL_LOW_POWER:
	case MD_THROTTLING:
		ret = ccci_send_msg_to_md(md, CCCI_SYSTEM_TX, id, val, false);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(exec_ccci_kern_func_by_md_id);

/* ------------------------------------------------------------------ */
/* message handlers                                                    */
/* ------------------------------------------------------------------ */
static void ccci_kernel_drop(struct ccci_md *md, const char *what,
			     struct sk_buff *skb)
{
	ccci_md_dbg(md, "%s: unsupported request, dropped (%u bytes)\n", what,
		    skb->len);
	dev_kfree_skb_any(skb);
}

static void ccci_control_msg_handler(struct ccci_port *port,
				     struct sk_buff *skb)
{
	struct ccci_md *md = port->md;
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
	u32 msg = ccci_h->data[1];
	u32 check = ccci_h->reserved;

	ccci_md_dbg(md, "control msg 0x%x/0x%x\n", msg, check);

	if (msg == MD_INIT_START_BOOT && check == MD_INIT_CHK_ID &&
	    md->boot_stage == CCCI_BOOT_STAGE_0) {
		ccci_boot_disarm_timer(md);
		md->boot_stage = CCCI_BOOT_STAGE_1;
		if (skb->len == CCCI_HEADER_SIZE + sizeof(struct md_query_ap_feature)) {
			if (ccci_boot_prepare_runtime_data(md, skb))
				ccci_md_err(md, "runtime data prep failed\n");
		} else if (skb->len == CCCI_HEADER_SIZE) {
			ccci_md_info(md, "legacy handshake message\n");
		} else {
			ccci_md_err(md, "invalid MD_QUERY_MSG length %u\n",
				    skb->len);
		}
		ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					 CCCI_MD_MSG_BOOT_UP, 0);
	} else if (msg == MD_NORMAL_BOOT && md->boot_stage == CCCI_BOOT_STAGE_1) {
		ccci_boot_disarm_timer(md);
		md->boot_stage = CCCI_BOOT_STAGE_2;
		ccci_md_set_state(md, CCCI_MD_READY);
		ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					 CCCI_MD_MSG_BOOT_READY, 0);
	} else if (msg == MD_EX) {
		if (check != MD_EX_CHK_ID) {
			ccci_md_err(md, "invalid MD_EX check id 0x%x\n", check);
		} else {
			ccci_boot_disarm_timer(md);
			md->ex_stage = CCCI_EX_INIT;
			ccci_md_set_state(md, CCCI_MD_EXCEPTION);
			ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
						 CCCI_MD_MSG_EXCEPTION, 0);
			/* downstream disables the MD watchdog on exception
			 * entry (port_kernel.c:562 ee_callback)
			 */
			ccif_wdt_irq(md, false);
			ccci_send_msg_to_md(md, CCCI_CONTROL_TX, MD_EX,
					    MD_EX_CHK_ID, true);
		}
	} else if (msg == MD_EX_REC_OK) {
		if (check != MD_EX_REC_OK_CHK_ID ||
		    skb->len < CCCI_HEADER_SIZE + 4) {
			ccci_md_err(md, "invalid MD_EX_REC_OK resv 0x%x len %u\n",
				    check, skb->len);
		} else {
			/* exception log: first byte is EX_HEADER_T.ex_type */
			md->ex_type = skb->data[CCCI_HEADER_SIZE];
			md->ex_stage = CCCI_EX_DHL_DL_RDY;
			ccci_md_err(md, "MD exception type %u, rec ok (%u bytes)\n",
				    md->ex_type, skb->len);
			ccci_md_set_state(md, CCCI_MD_EXCEPTION);
		}
	} else if (msg == MD_EX_PASS) {
		md->ex_stage = CCCI_EX_INIT_DONE;
		ccci_md_set_state(md, CCCI_MD_EXCEPTION);
	} else if (msg == MD_INIT_START_BOOT && check == MD_INIT_CHK_ID) {
		/* second boot attempt: userspace has to restart the modem */
		md->boot_stage = CCCI_BOOT_STAGE_0;
		ccci_md_err(md, "MD second boot detected\n");
		ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					 CCCI_MD_MSG_RESET, 0);
	} else if (msg == MD_EX_RESUME) {
		md->ex_type = 0;	/* MD_EX_TYPE_EMI_CHECK is 0 in ROM */
		ccci_md_err(md, "MD EMI check resume\n");
		ccci_md_set_state(md, CCCI_MD_EXCEPTION);
	} else if (msg == CCCI_DRV_VER_ERROR) {
		ccci_md_err(md, "AP/MD CCCI version mismatch\n");
	} else if (msg == C2K_HB_MSG && md->id == CCCI_MD_SYS3) {
		md->heartbeat++;
	} else if ((msg == C2K_STATUS_IND_MSG ||
		    msg == C2K_STATUS_QUERY_MSG) && md->id == CCCI_MD_SYS3) {
		/* struct c2k_ctrl_port_msg option byte lives in reserved[3] */
		md->dtr_state = !!(ccci_h->reserved & 0x80000000);
	} else {
		ccci_md_err(md, "unknown control message 0x%x\n", msg);
	}
	dev_kfree_skb_any(skb);
}

static void ccci_system_msg_handler(struct ccci_port *port,
				    struct sk_buff *skb)
{
	struct ccci_md *md = port->md;
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;

	switch (ccci_h->data[1]) {
	case MD_GET_BATTERY_INFO:
		ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					 CCCI_MD_MSG_SEND_BATTERY_INFO, 0);
		break;
	case MD_WDT_MONITOR:
		/* heartbeat, the WDT IRQ is the real watchdog */
		md->heartbeat++;
		break;
	case MD_SIM_TYPE:
		md->sim_type = ccci_h->reserved;
		break;
	case MD_TX_POWER:
	case MD_RF_TEMPERATURE:
	case MD_RF_TEMPERATURE_3G:
	case MD_THROTTLING:
	case MD_THERMAL_LOW_POWER:
		ccci_exec_sys_cb(md, ccci_h->data[1], ccci_h->reserved);
		break;
	default:
		ccci_md_dbg(md, "unhandled system message 0x%x\n",
			    ccci_h->data[1]);
		break;
	}
	dev_kfree_skb_any(skb);
}

static void ccci_status_msg_handler(struct ccci_port *port,
				    struct sk_buff *skb)
{
	struct ccci_md *md = port->md;

	md->heartbeat++;
	ccci_md_dbg(md, "status message 0x%x\n",
		    ((struct ccci_header *)skb->data)->data[1]);
	dev_kfree_skb_any(skb);
}

/* ------------------------------------------------------------------ */
/* kernel port thread                                                  */
/* ------------------------------------------------------------------ */
static int ccci_kernel_port_thread(void *arg)
{
	struct ccci_port *port = arg;
	struct ccci_md *md = port->md;
	struct sk_buff *skb;
	unsigned long flags;

	while (!kthread_should_stop()) {
		wait_event_interruptible(port->rx_wq,
					 !skb_queue_empty(&port->rx_q) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&port->rx_lock, flags);
		skb = __skb_dequeue(&port->rx_q);
		if (skb && port->rx_len)
			port->rx_len--;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		if (!skb)
			continue;

		switch (((struct ccci_header *)skb->data)->channel) {
		case CCCI_CONTROL_RX:
			ccci_control_msg_handler(port, skb);
			break;
		case CCCI_SYSTEM_RX:
			ccci_system_msg_handler(port, skb);
			break;
		case CCCI_STATUS_RX:
			ccci_status_msg_handler(port, skb);
			break;
		case CCCI_RPC_RX:
			/* downstream rpc_msg_handler() (port_kernel.c:2108),
			 * always consumes the skb
			 */
			ccci_rpc_handle(port, skb);
			break;
		default:
			ccci_kernel_drop(md, "kernel port", skb);
			break;
		}
		ccci_port_ask_more(port);
	}
	return 0;
}

static int ccci_kernel_port_init(struct ccci_port *port)
{
	struct task_struct *task;

	port->rx_len_th = CCCI_PORT_RX_LEN_TH;
	task = kthread_run(ccci_kernel_port_thread, port, "%s", port->name);
	if (IS_ERR(task)) {
		ccci_md_err(port->md, "cannot start thread for %s\n",
			    port->name);
		return PTR_ERR(task);
	}
	port->priv = task;
	return 0;
}

static int ccci_kernel_port_recv_skb(struct ccci_port *port,
				     struct sk_buff *skb)
{
	return ccci_port_recv_skb(port, skb);
}

const struct ccci_port_ops ccci_kernel_port_ops = {
	.init = ccci_kernel_port_init,
	.recv_skb = ccci_kernel_port_recv_skb,
};

void ccci_port_kernel_exit(struct ccci_md *md)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->ops != &ccci_kernel_port_ops || !port->priv)
			continue;
		wake_up_interruptible(&port->rx_wq);
		kthread_stop(port->priv);
		port->priv = NULL;
		skb_queue_purge(&port->rx_q);
	}
}
