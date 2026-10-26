// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI per-modem sysfs attributes and debugfs views.
 *
 * Attribute names follow the Samsung/MTK ROM service set
 * (md_enable/version/md_state/dump/boot_info/md_exception, see downstream
 * ccci_util_lib_sys.c for the global /sys/kernel/ccci variant).  Here they are
 * attached to the per-modem device /sys/class/ccci_node/mdN and duplicated in
 * debugfs ccci/mdN, which the ccci_util log driver also populates.
 */
#include <linux/ccci.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ccci_priv.h"

static struct dentry *ccci_debugfs_root;

/* ------------------------------------------------------------------ */
/* text helpers                                                        */
/* ------------------------------------------------------------------ */
static const char *ccci_state_name(enum ccci_md_state state)
{
	switch (state) {
	case CCCI_MD_BOOTING:
		return "boot";
	case CCCI_MD_READY:
		return "ready";
	case CCCI_MD_EXCEPTION:
		return "exception";
	case CCCI_MD_STOPPED:
		return "stop";
	case CCCI_MD_INVALID:
	default:
		return "invalid";
	}
}

static int ccci_show_version(struct ccci_md *md, char *buf, size_t len)
{
	return scnprintf(buf, len, "%u\n", CCCI_DRIVER_VER);
}

static int ccci_show_state(struct ccci_md *md, char *buf, size_t len)
{
	return scnprintf(buf, len, "%s, boot stage %u, ex stage %u\n",
			 ccci_state_name(md->state), md->boot_stage,
			 md->ex_stage);
}

static int ccci_show_boot_info(struct ccci_md *md, char *buf, size_t len)
{
	if (!md->boot_info[0])
		return scnprintf(buf, len, "not loaded\n");
	return scnprintf(buf, len, "%s", md->boot_info);
}

static int ccci_show_exception(struct ccci_md *md, char *buf, size_t len)
{
	return scnprintf(buf, len, "type %u, stage %u, state %s\n", md->ex_type,
			 md->ex_stage, ccci_state_name(md->state));
}

static int ccci_show_enable(struct ccci_md *md, char *buf, size_t len)
{
	bool up = md->state != CCCI_MD_STOPPED && md->state != CCCI_MD_INVALID;

	return scnprintf(buf, len, "%d\n", up);
}

static int ccci_store_enable(struct ccci_md *md, const char *buf, size_t len)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	if (enable)
		ret = ccci_md_start(md);
	else
		ret = ccci_md_stop(md, 0);
	return ret ? ret : len;
}

static int ccci_store_dump(struct ccci_md *md, const char *buf, size_t len)
{
	struct ccci_port *port;

	ccci_md_info(md, "state %s, boot %u, ex %u, fw %s\n",
		     ccci_state_name(md->state), md->boot_stage, md->ex_stage,
		     md->boot.file_name[0] ? md->boot.file_name : "none");
	ccci_ccif_dump_regs(md, NULL);
	list_for_each_entry(port, &md->ports_list, node)
		ccci_md_info(md, "port %-18s ch %3u/%-3u q %2u/%-3u rx %llu tx %llu drop %llu busy %llu\n",
			     port->name, port->tx_ch, port->rx_ch, port->txq,
			     port->rxq, port->rx_pkts, port->tx_pkts,
			     port->rx_drops, port->tx_busy_cnt);
	return len;
}

/* ------------------------------------------------------------------ */
/* sysfs                                                               */
/* ------------------------------------------------------------------ */
static ssize_t ccci_md_enable_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return ccci_show_enable(dev_get_drvdata(dev), buf, PAGE_SIZE);
}

static ssize_t ccci_md_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	return ccci_store_enable(dev_get_drvdata(dev), buf, count);
}
static DEVICE_ATTR_RW(ccci_md_enable);

static ssize_t ccci_version_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return ccci_show_version(dev_get_drvdata(dev), buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(ccci_version);

static ssize_t ccci_md_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return ccci_show_state(dev_get_drvdata(dev), buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(ccci_md_state);

static ssize_t ccci_boot_info_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return ccci_show_boot_info(dev_get_drvdata(dev), buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(ccci_boot_info);

static ssize_t ccci_md_exception_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	return ccci_show_exception(dev_get_drvdata(dev), buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(ccci_md_exception);

static ssize_t ccci_dump_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	return ccci_store_dump(dev_get_drvdata(dev), buf, count);
}
static DEVICE_ATTR_WO(ccci_dump);

static struct attribute *ccci_md_attrs[] = {
	&dev_attr_ccci_md_enable.attr,
	&dev_attr_ccci_version.attr,
	&dev_attr_ccci_md_state.attr,
	&dev_attr_ccci_dump.attr,
	&dev_attr_ccci_boot_info.attr,
	&dev_attr_ccci_md_exception.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ccci_md);

/* ------------------------------------------------------------------ */
/* debugfs                                                             */
/* ------------------------------------------------------------------ */
static int ccci_dbg_version_show(struct seq_file *s, void *unused)
{
	char buf[32];

	ccci_show_version(s->private, buf, sizeof(buf));
	seq_puts(s, buf);
	return 0;
}

static int ccci_dbg_state_show(struct seq_file *s, void *unused)
{
	struct ccci_md *md = s->private;
	struct ccci_port *port;

	seq_printf(s, "md%u: %s\n", md->id + 1, ccci_state_name(md->state));
	seq_printf(s, "transport: %s\n",
		   md->cldma ? "cldma" : (md->ring_mode ? "ccif-ring" : "none"));
	seq_printf(s, "fw: %s (%u bytes)\n",
		   md->boot.file_name[0] ? md->boot.file_name : "none",
		   md->boot.md_img_size);
	seq_printf(s, "smem: %pa + 0x%zx\n", &md->smem_phy, md->smem_size);
	list_for_each_entry(port, &md->ports_list, node)
		seq_printf(s, "  %-18s ch %3u/%-3u q %2u/%-3u rx %llu tx %llu\n",
			   port->name, port->tx_ch, port->rx_ch, port->txq,
			   port->rxq, port->rx_pkts, port->tx_pkts);
	return 0;
}

static int ccci_dbg_ccif_show(struct seq_file *s, void *unused)
{
	ccci_ccif_dump_regs(s->private, s);
	return 0;
}

static int ccci_dbg_boot_show(struct seq_file *s, void *unused)
{
	char buf[256];

	ccci_show_boot_info(s->private, buf, sizeof(buf));
	seq_puts(s, buf);
	return 0;
}

static int ccci_dbg_exception_show(struct seq_file *s, void *unused)
{
	char buf[128];

	ccci_show_exception(s->private, buf, sizeof(buf));
	seq_puts(s, buf);
	return 0;
}

static int ccci_dbg_enable_show(struct seq_file *s, void *unused)
{
	char buf[16];

	ccci_show_enable(s->private, buf, sizeof(buf));
	seq_puts(s, buf);
	return 0;
}

static ssize_t ccci_dbg_enable_write(struct file *file,
				     const char __user *ubuf, size_t len,
				     loff_t *ppos)
{
	struct ccci_md *md = file_inode(file)->i_private;
	char buf[16];
	size_t n = min(len, sizeof(buf) - 1);
	int ret;

	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	ret = ccci_store_enable(md, buf, n);
	return ret < 0 ? ret : n;
}

static int ccci_dbg_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, ccci_dbg_enable_show, inode->i_private);
}

static const struct file_operations ccci_dbg_enable_fops = {
	.owner = THIS_MODULE,
	.open = ccci_dbg_enable_open,
	.read = seq_read,
	.write = ccci_dbg_enable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

#define CCCI_DBG_SEQ_FOPS(_name, _showfn)				\
static int _name##_open(struct inode *inode, struct file *file)		\
{									\
	return single_open(file, _showfn, inode->i_private);		\
}									\
static const struct file_operations _name##_fops = {			\
	.owner = THIS_MODULE,						\
	.open = _name##_open,						\
	.read = seq_read,						\
	.llseek = seq_lseek,						\
	.release = single_release,					\
}

CCCI_DBG_SEQ_FOPS(ccci_dbg_version, ccci_dbg_version_show);
CCCI_DBG_SEQ_FOPS(ccci_dbg_state, ccci_dbg_state_show);
CCCI_DBG_SEQ_FOPS(ccci_dbg_ccif, ccci_dbg_ccif_show);
CCCI_DBG_SEQ_FOPS(ccci_dbg_boot, ccci_dbg_boot_show);
CCCI_DBG_SEQ_FOPS(ccci_dbg_exception, ccci_dbg_exception_show);

static struct dentry *ccci_dbg_dir_get(struct ccci_md *md)
{
	struct dentry *dir;
	char name[16];

	if (!ccci_debugfs_root)
		return NULL;

	snprintf(name, sizeof(name), "md%u", md->id + 1);
	dir = debugfs_lookup(name, ccci_debugfs_root);
	if (IS_ERR_OR_NULL(dir)) {
		dir = debugfs_create_dir(name, ccci_debugfs_root);
		if (IS_ERR(dir))
			return NULL;
	}
	return dir;
}

int ccci_sysfs_md_init(struct ccci_md *md)
{
	int ret;

	ret = sysfs_create_groups(&md->dev->kobj, ccci_md_groups);
	if (ret)
		return ret;

	md->dbg_dir = ccci_dbg_dir_get(md);
	if (!md->dbg_dir)
		return 0;	/* debugfs is optional */

	debugfs_create_file("ccci_version", 0444, md->dbg_dir, md,
			    &ccci_dbg_version_fops);
	debugfs_create_file("ccci_md_state", 0444, md->dbg_dir, md,
			    &ccci_dbg_state_fops);
	debugfs_create_file("ccci_md_enable", 0644, md->dbg_dir, md,
			    &ccci_dbg_enable_fops);
	debugfs_create_file("ccci_boot_info", 0444, md->dbg_dir, md,
			    &ccci_dbg_boot_fops);
	debugfs_create_file("ccci_md_exception", 0444, md->dbg_dir, md,
			    &ccci_dbg_exception_fops);
	debugfs_create_file("ccif", 0444, md->dbg_dir, md, &ccci_dbg_ccif_fops);
	return 0;
}

void ccci_sysfs_md_exit(struct ccci_md *md)
{
	if (!md->dev)
		return;
	sysfs_remove_groups(&md->dev->kobj, ccci_md_groups);
	md->dbg_dir = NULL;	/* the util log driver owns the directory */
}

int ccci_sysfs_init(void)
{
	ccci_debugfs_root = debugfs_create_dir("ccci", NULL);
	if (IS_ERR(ccci_debugfs_root)) {
		pr_warn("ccci: debugfs unavailable (%ld)\n",
			PTR_ERR(ccci_debugfs_root));
		ccci_debugfs_root = NULL;
	}
	return 0;
}

void ccci_sysfs_exit(void)
{
	if (ccci_debugfs_root)
		debugfs_remove(ccci_debugfs_root);
	ccci_debugfs_root = NULL;
}
