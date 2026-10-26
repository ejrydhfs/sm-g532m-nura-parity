// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI character ports (/dev/ccci_monitor, /dev/ttyC0, /dev/ccci_fs, ...).
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_char.c (read/write/poll/ioctl and the
 * critical-user bookkeeping) and port_cfg.c (port table, see ccci_core.c).
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#include <linux/cdev.h>
#include <linux/ccci.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ccci_priv.h"

/*
 * Downstream keeps four "critical user" flags so the modem is only reset from
 * userspace when no important client is attached (port_char.c:50-89).
 */
static void ccci_char_open_check(struct ccci_port *port)
{
	struct ccci_md *md = port->md;

	if (port->rx_ch == CCCI_FS_RX)
		md->critical_user[0] = 1;
	if (port->rx_ch == CCCI_UART2_RX)
		md->critical_user[1] = 1;
	if (port->rx_ch == CCCI_MD_LOG_RX)
		md->critical_user[2] = 0;
	if (port->rx_ch == CCCI_UART1_RX)
		md->critical_user[3] = 0;
}

static int ccci_char_close_check(struct ccci_port *port)
{
	struct ccci_md *md = port->md;

	if (port->rx_ch == CCCI_FS_RX && !atomic_read(&port->open_count))
		md->critical_user[0] = 0;
	if (port->rx_ch == CCCI_UART2_RX && !atomic_read(&port->open_count))
		md->critical_user[1] = 0;
	if (port->rx_ch == CCCI_MD_LOG_RX && !atomic_read(&port->open_count))
		md->critical_user[2] = 0;
	if (port->rx_ch == CCCI_UART1_RX && !atomic_read(&port->open_count))
		md->critical_user[3] = 0;

	if (md->critical_user[0] || md->critical_user[1])
		return 1;
	/*
	 * downstream dev_char_close_check() (port_char.c:65-96): in META mode
	 * only the DHL control port (CCCI_UART1_RX, /dev/ccci_md_log_ctrl)
	 * must be closed before the modem may be reset; the MD logger port is
	 * not part of a META session.
	 */
	if (md->md_boot_mode == CCCI_MD_BOOT_MODE_META) {
		if (md->critical_user[3]) {
			ccci_md_err(md, "DHL ctrl is still open in META mode\n");
			return 1;
		}
		ccci_md_info(md, "ready to reset MD in META mode\n");
		return 0;
	}
	if (md->critical_user[2] || md->critical_user[3])
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* open / release                                                      */
/* ------------------------------------------------------------------ */
static int ccci_char_open(struct inode *inode, struct file *file)
{
	struct ccci_port *port = ccci_get_port_for_node(inode->i_rdev);

	if (!port)
		return -ENODEV;

	if (atomic_inc_return(&port->open_count) > 1) {
		atomic_dec(&port->open_count);
		ccci_md_err(port->md, "port %s already open\n", port->name);
		return -EBUSY;
	}

	file->private_data = port;
	nonseekable_open(inode, file);
	ccci_char_open_check(port);
	ccci_md_dbg(port->md, "port %s opened by %s\n", port->name,
		    current->comm);
	return 0;
}

static int ccci_char_release(struct inode *inode, struct file *file)
{
	struct ccci_port *port = file->private_data;
	unsigned long flags;
	struct sk_buff *skb;

	atomic_dec(&port->open_count);

	spin_lock_irqsave(&port->rx_lock, flags);
	while ((skb = __skb_dequeue(&port->rx_q)) != NULL) {
		if (port->rx_len)
			port->rx_len--;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		dev_kfree_skb_any(skb);
		spin_lock_irqsave(&port->rx_lock, flags);
	}
	if (port->rx_current) {
		dev_kfree_skb_any(port->rx_current);
		port->rx_current = NULL;
	}
	port->rx_len = 0;
	port->rx_full = false;
	spin_unlock_irqrestore(&port->rx_lock, flags);

	/* let a pending modem reset proceed if no critical user is left */
	if (ccci_char_close_check(port) == 0 &&
	    port->md->state == CCCI_MD_STOPPED)
		ccci_send_virtual_md_msg(port->md, CCCI_MONITOR_CH,
					 CCCI_MD_MSG_READY_TO_RESET, 0);

	ccci_port_ask_more(port);
	ccci_md_dbg(port->md, "port %s closed\n", port->name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */
static ssize_t ccci_char_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	struct sk_buff *skb;
	struct ccci_header *ccci_h;
	size_t read_len;
	unsigned long flags;
	int ret;

	if (!count)
		return 0;

	if (!port->rx_current) {
		if (skb_queue_empty(&port->rx_q)) {
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			ret = wait_event_interruptible(port->rx_wq,
						       !skb_queue_empty(&port->rx_q));
			if (ret)
				return -ERESTARTSYS;
		}

		spin_lock_irqsave(&port->rx_lock, flags);
		skb = __skb_dequeue(&port->rx_q);
		if (skb && port->rx_len)
			port->rx_len--;
		spin_unlock_irqrestore(&port->rx_lock, flags);
		if (!skb)
			return -EAGAIN;

		port->rx_current = skb;
		port->rx_current_user_header = false;

		if (port->flags & PORT_F_USER_HEADER) {
			/* monitor channel: hand the header to md_init and tag it
			 * with the legacy CCCI_MONITOR_CH_ID (port_char.c:205)
			 */
			ccci_h = (struct ccci_header *)skb->data;
			if (ccci_h->data[0] == CCCI_MAGIC_NUM &&
			    ccci_h->channel == CCCI_MONITOR_CH) {
				*(((u32 *)ccci_h) + 2) = CCCI_MONITOR_CH_ID;
				port->rx_current_user_header = true;
			}
		} else {
			skb_pull(skb, CCCI_HEADER_SIZE);
		}
	}

	skb = port->rx_current;
	read_len = min(count, (size_t)skb->len);
	if (copy_to_user(buf, skb->data, read_len)) {
		ccci_md_err(port->md, "read on %s: copy_to_user failed\n",
			    port->name);
		return -EFAULT;
	}
	skb_pull(skb, read_len);
	if (!skb->len) {
		dev_kfree_skb_any(skb);
		port->rx_current = NULL;
		ccci_port_ask_more(port);
	}
	return read_len;
}

/* ------------------------------------------------------------------ */
/* write                                                               */
/* ------------------------------------------------------------------ */
static ssize_t ccci_char_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct ccci_port *port = file->private_data;
	struct ccci_md *md = port->md;
	struct ccci_header *ccci_h;
	struct sk_buff *skb;
	size_t limit, actual;
	bool user_header = port->flags & PORT_F_USER_HEADER;
	unsigned int qno, retries = 0;
	int ret;

	if (port->tx_ch == CCCI_MONITOR_CH)
		return -EPERM;

	if (md->state == CCCI_MD_BOOTING && port->tx_ch != CCCI_FS_TX &&
	    port->tx_ch != CCCI_RPC_TX)
		return -ENODEV;
	if (md->state == CCCI_MD_EXCEPTION && port->tx_ch != CCCI_MD_LOG_TX &&
	    port->tx_ch != CCCI_UART1_TX && port->tx_ch != CCCI_FS_TX)
		return -ETXTBSY;
	if (md->state == CCCI_MD_STOPPED || md->state == CCCI_MD_INVALID)
		return -ENODEV;

	limit = user_header ? CCCI_MTU + CCCI_HEADER_SIZE : CCCI_MTU;
	if (!count)
		return -EINVAL;
	actual = min(count, limit);

	qno = ccci_port_txq(port);
	if (qno >= CCCI_MAX_QUEUES)
		return -ENODEV;

	skb = alloc_skb(CCCI_HEADER_SIZE + actual, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	if (!user_header) {
		ccci_h = (struct ccci_header *)skb_put(skb, CCCI_HEADER_SIZE);
		memset(ccci_h, 0, CCCI_HEADER_SIZE);
		ccci_h->data[0] = 0;
		ccci_h->data[1] = actual + CCCI_HEADER_SIZE;
		ccci_h->channel = port->tx_ch;
		ccci_h->reserved = 0;
	}

	if (copy_from_user(skb_put(skb, actual), buf, actual)) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	if (user_header) {
		/* the CCCI header came from the user, only patch what the ROM
		 * patches (port_char.c:492-502)
		 */
		ccci_h = (struct ccci_header *)skb->data;
		if (actual == CCCI_HEADER_SIZE)
			ccci_h->data[0] = CCCI_MAGIC_NUM;
		else
			ccci_h->data[1] = actual;
		ccci_h->channel = port->tx_ch;
	}

	port->tx_pkts++;
	for (;;) {
		ret = ccci_md_send_skb(md, qno, skb);
		if (!ret)
			return actual;
		if (ret != -EBUSY)
			break;
		port->tx_busy_cnt++;
		/* a blocking writer waits for room, like downstream's blocking
		 * request allocation
		 */
		if ((file->f_flags & O_NONBLOCK) || ++retries > 2000)
			break;
		usleep_range(1000, 2000);
	}
	dev_kfree_skb_any(skb);
	return ret == -EBUSY ? -EAGAIN : ret;
}

/* ------------------------------------------------------------------ */
/* poll                                                                */
/* ------------------------------------------------------------------ */
static __poll_t ccci_char_poll(struct file *file, poll_table *wait)
{
	struct ccci_port *port = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &port->rx_wq, wait);
	if (port->rx_current || !skb_queue_empty(&port->rx_q))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ccci_md_write_room(port->md, ccci_port_txq(port)) > 0)
		mask |= EPOLLOUT | EPOLLWRNORM;
	/* mdlogger polls to learn that the modem is going away */
	if (port->rx_ch == CCCI_UART1_RX &&
	    port->md->state != CCCI_MD_READY &&
	    port->md->state != CCCI_MD_EXCEPTION)
		mask |= EPOLLERR;
	return mask;
}

/* ------------------------------------------------------------------ */
/* ioctl                                                               */
/* ------------------------------------------------------------------ */
static long ccci_char_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct ccci_port *port = file->private_data;
	struct ccci_md *md = port->md;
	unsigned int val;
	long ret = 0;

	switch (cmd) {
	case CCCI_IOC_GET_MD_STATE:
		ret = put_user((unsigned int)md->boot_stage,
			       (unsigned int __user *)arg);
		break;

	case CCCI_IOC_MD_RESET:
		ccci_md_info(md, "MD reset requested by %s\n", current->comm);
		ret = ccci_md_reset(md);
		if (!ret)
			ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
						       CCCI_MD_MSG_RESET, 0);
		break;

	case CCCI_IOC_DO_MD_RST:
		ret = ccci_md_reset(md);
		break;

	/* downstream CCCI_IOC_GET_MD_INFO returns the image version, this is
	 * the closest ROM ioctl to a "get boot info" request
	 */
	case CCCI_IOC_GET_MD_INFO:
		ret = put_user(md->boot.product_ver, (unsigned int __user *)arg);
		break;

	case CCCI_IOC_GET_MD_EX_TYPE:
		ret = put_user(md->ex_type, (unsigned int __user *)arg);
		break;

	case CCCI_IOC_FORCE_MD_ASSERT:
		ccci_md_info(md, "force MD assert by %s\n", current->comm);
		ret = ccci_md_force_assert(md,
					   md->id == CCCI_MD_SYS3 ? 1 : 0);
		break;

	case CCCI_IOC_SEND_RUN_TIME_DATA:
		if (port->rx_ch != CCCI_MONITOR_CH) {
			ret = -EPERM;
			break;
		}
		ret = ccci_boot_send_runtime_data(md, md->sbp_code);
		break;

	case CCCI_IOC_ENTER_DEEP_FLIGHT:
		ccci_md_info(md, "enter flight mode by %s\n", current->comm);
		ret = ccci_md_reset(md);
		if (!ret) {
			ccci_md_stop(md, 1000);
			ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
						       CCCI_MD_MSG_ENTER_FLIGHT_MODE,
						       0);
		}
		break;

	case CCCI_IOC_LEAVE_DEEP_FLIGHT:
		ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					       CCCI_MD_MSG_LEAVE_FLIGHT_MODE,
					       0);
		break;

	case CCCI_IOC_DO_START_MD:
		ret = ccci_md_start(md);
		break;

	case CCCI_IOC_DO_STOP_MD:
		ret = ccci_md_stop(md, 0);
		break;

	case CCCI_IOC_SEND_STOP_MD_REQUEST:
		ret = ccci_md_reset(md);
		if (!ret) {
			ccci_md_stop(md, 0);
			ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
						       CCCI_MD_MSG_STOP_MD_REQUEST,
						       0);
		}
		break;

	case CCCI_IOC_SEND_START_MD_REQUEST:
		ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					       CCCI_MD_MSG_START_MD_REQUEST, 0);
		break;

	case CCCI_IOC_POWER_ON_MD_REQUEST:
		ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					       CCCI_MD_MSG_POWER_ON_REQUEST, 0);
		break;

	case CCCI_IOC_POWER_OFF_MD_REQUEST:
		ret = ccci_send_virtual_md_msg(md, CCCI_MONITOR_CH,
					       CCCI_MD_MSG_POWER_OFF_REQUEST, 0);
		break;

	case CCCI_IOC_POWER_ON_MD:
	case CCCI_IOC_POWER_OFF_MD:
		/* downstream marks these abandoned but returns -1 */
		ret = -EPERM;
		break;

	case CCCI_IOC_GET_MD_PROTOCOL_TYPE:
		/* { "DHL", 0 } as char[16] buffer, downstream port_char.c:574 */
		ret = copy_to_user((void __user *)arg, "DHL", 4) ? -EFAULT : 0;
		break;

	case CCCI_IOC_SEND_BATTERY_INFO:
		/* needs the MT6328 battery driver, absent from this tree */
		ccci_md_info(md, "battery info request without battery driver\n");
		ret = -EOPNOTSUPP;
		break;

	case CCCI_IOC_SET_MD_BOOT_MODE:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		if (val > 0 && val < 3)	/* NORMAL / META */
			md->md_boot_mode = val;
		else
			ret = -EINVAL;
		break;

	case CCCI_IOC_GET_MD_BOOT_MODE:
		ret = put_user(md->md_boot_mode, (unsigned int __user *)arg);
		break;

	case CCCI_IOC_GET_MD_SBP_CFG:
		/* default resolved from CONFIG_MTK_MD[_2]_SBP_CUSTOM_VALUE at
		 * modem creation, downstream port_char.c:914-947
		 */
		ret = put_user(md->sbp_code_default,
			       (unsigned int __user *)arg);
		break;

	case CCCI_IOC_SET_MD_SBP_CFG:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		md->sbp_code = val;
		ccci_md_info(md, "SBP code set to 0x%x by %s\n", val,
			     current->comm);
		break;

	case CCCI_IOC_GET_MD_MEM_SIZE:
		ret = put_user((unsigned int)md->img_size,
			       (unsigned int __user *)arg);
		break;

	case CCCI_IOC_PCM_BASE_ADDR:
	case CCCI_IOC_PCM_LEN:
	case CCCI_IOC_ALLOC_MD_LOG_MEM:
		/* deprecated shared-memory operations, downstream ignores them */
		break;

	default:
		ccci_md_dbg(md, "unsupported ioctl 0x%x on %s\n", cmd,
			    port->name);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long ccci_char_compat_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	return ccci_char_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations ccci_char_fops = {
	.owner = THIS_MODULE,
	.open = ccci_char_open,
	.release = ccci_char_release,
	.read = ccci_char_read,
	.write = ccci_char_write,
	.poll = ccci_char_poll,
	.unlocked_ioctl = ccci_char_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ccci_char_compat_ioctl,
#endif
};

/* ------------------------------------------------------------------ */
/* port ops                                                            */
/* ------------------------------------------------------------------ */
static int ccci_char_port_recv_skb(struct ccci_port *port, struct sk_buff *skb)
{
	struct ccci_md *md = port->md;

	/* nobody is listening: keep the modem alive, count the drop */
	if (!atomic_read(&port->open_count) && port->rx_ch != CCCI_UART2_RX &&
	    port->rx_ch != CCCI_PCM_RX && port->rx_ch != CCCI_FS_RX &&
	    port->rx_ch != CCCI_RPC_RX) {
		port->rx_drops++;
		dev_kfree_skb_any(skb);
		return 0;
	}

	md->wakeup_src = 0;
	return ccci_port_recv_skb(port, skb);
}

static void ccci_char_md_state_notice(struct ccci_port *port, int event)
{
	if (port->rx_ch == CCCI_UART1_RX && event == CCCI_MD_STOPPED)
		wake_up_interruptible(&port->rx_wq);
}

const struct ccci_port_ops ccci_char_port_ops = {
	.recv_skb = ccci_char_port_recv_skb,
	.md_state_notice = ccci_char_md_state_notice,
};

/* ------------------------------------------------------------------ */
/* device node management                                              */
/* ------------------------------------------------------------------ */
static int ccci_char_add_port(struct ccci_port *port)
{
	struct ccci_md *md = port->md;
	dev_t devt = MKDEV(md->major, port->minor);
	int ret;

	cdev_init(&port->cdev, &ccci_char_fops);
	port->cdev.owner = THIS_MODULE;
	ret = cdev_add(&port->cdev, devt, 1);
	if (ret) {
		ccci_md_err(md, "cdev_add(%s, %u:%u) failed %d\n", port->name,
			    md->major, port->minor, ret);
		return ret;
	}
	port->cdev_added = true;

	port->dev = device_create(ccci_class, md->dev, devt, port, "%s",
				  port->name);
	if (IS_ERR(port->dev)) {
		ret = PTR_ERR(port->dev);
		port->dev = NULL;
		cdev_del(&port->cdev);
		port->cdev_added = false;
		ccci_md_err(md, "device_create(%s) failed %d\n", port->name,
			    ret);
		return ret;
	}
	ccci_md_info(md, "port %s at %u:%u\n", port->name, md->major,
		     port->minor);
	return 0;
}

int ccci_port_char_init(struct ccci_md *md)
{
	struct ccci_port *port;
	int ret;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->ops != &ccci_char_port_ops)
			continue;
		ret = ccci_char_add_port(port);
		if (ret)
			return ret;
	}
	return 0;
}

void ccci_port_char_exit(struct ccci_md *md)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (!port->cdev_added)
			continue;
		if (port->dev) {
			device_destroy(ccci_class,
				       MKDEV(md->major, port->minor));
			port->dev = NULL;
		}
		cdev_del(&port->cdev);
		port->cdev_added = false;
		skb_queue_purge(&port->rx_q);
		if (port->rx_current) {
			dev_kfree_skb_any(port->rx_current);
			port->rx_current = NULL;
		}
		port->rx_len = 0;
	}
}
