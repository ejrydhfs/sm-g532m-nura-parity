// SPDX-License-Identifier: GPL-2.0
/*
 * C2K (MD3) sysfs and debugfs interface.
 *
 * Port from Samsung/MTK downstream (SM-G532M, MT6737T, kernel 3.18):
 *   drivers/misc/mediatek/eccci/ccci_core.c (state, boot count)
 *   drivers/misc/mediatek/eccci/ccci_support.c (sim switch mode)
 *   drivers/misc/mediatek/eccci/port_net.c (IRAT peer)
 *   drivers/misc/mediatek/eccci/modem_ccif_c2k.c (register dump)
 *
 * Nothing in the 3.18 C2K path exposed these files; the values are the ones
 * the downstream debug prints and sysfs attributes of the CCCI core report.
 *
 * Copyright (C) 2015 MediaTek Inc.
 * Copyright (C) 2026 The Nura project
 */

#include <linux/ccci.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>

#include "ccci_c2k.h"

static const char *c2k_md_state_name(int state)
{
	switch (state) {
	case CCCI_MD_INVALID:
		return "invalid";
	case CCCI_MD_BOOTING:
		return "booting";
	case CCCI_MD_READY:
		return "ready";
	case CCCI_MD_EXCEPTION:
		return "exception";
	case CCCI_MD_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

static ssize_t md3_state_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return sysfs_emit(buf, "%s\n",
			  c2k_md_state_name(ccci_md_get_state(CCCI_MD_SYS3)));
}
static struct kobj_attribute c2k_md3_state_attr =
	__ATTR_RO(md3_state);

static ssize_t boot_count_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return sysfs_emit(buf, "%u\n", READ_ONCE(c2k->boot_count));
}
static struct kobj_attribute c2k_boot_count_attr =
	__ATTR_RO(boot_count);

static ssize_t irat_state_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return sysfs_emit(buf, "md3_irat_md_id=MD%u capability=0x%08x\n",
			  c2k->irat_md_id + 1, c2k->cfg.capability);
}
static struct kobj_attribute c2k_irat_state_attr =
	__ATTR_RO(irat_state);

static ssize_t sim_switch_mode_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	if (!ccci_c2k_get())
		return -ENODEV;

	return sysfs_emit(buf, "0x%x\n", ccci_c2k_sim_switch_mode_get());
}

static ssize_t sim_switch_mode_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int mode;
	int ret;

	if (!ccci_c2k_get())
		return -ENODEV;

	ret = kstrtoint(buf, 0, &mode);
	if (ret)
		return ret;

	ccci_c2k_sim_switch_mode_set(mode);
	return count;
}
static struct kobj_attribute c2k_sim_switch_mode_attr =
	__ATTR_RW(sim_switch_mode);

static ssize_t chip_id_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return sysfs_emit(buf, "0x%08x%s\n", ccci_c2k_hw_read_chip_id(c2k),
			  c2k->chip_id_valid ? "" : " (unexpected)");
}
static struct kobj_attribute c2k_chip_id_attr =
	__ATTR_RO(chip_id);

static ssize_t md_wdt_irq_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return sysfs_emit(buf, "%u\n", c2k->md_wdt_irq);
}
static struct kobj_attribute c2k_md_wdt_irq_attr =
	__ATTR_RO(md_wdt_irq);

static ssize_t regs_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return ccci_c2k_hw_format_regs(c2k, buf, PAGE_SIZE);
}
static struct kobj_attribute c2k_regs_attr =
	__ATTR_RO(regs);

static ssize_t c2k_wdt_status_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	struct ccci_c2k *c2k = ccci_c2k_get();

	if (!c2k)
		return -ENODEV;

	return ccci_c2k_hw_format_wdt(c2k, buf, PAGE_SIZE);
}
static struct kobj_attribute c2k_wdt_status_attr =
	__ATTR_RO(c2k_wdt_status);

static struct attribute *c2k_attrs[] = {
	&c2k_md3_state_attr.attr,
	&c2k_boot_count_attr.attr,
	&c2k_irat_state_attr.attr,
	&c2k_sim_switch_mode_attr.attr,
	&c2k_chip_id_attr.attr,
	&c2k_md_wdt_irq_attr.attr,
	&c2k_regs_attr.attr,
	&c2k_wdt_status_attr.attr,
	NULL,
};

static const struct attribute_group c2k_attr_group = {
	.attrs = c2k_attrs,
};

static int c2k_debugfs_regs_show(struct seq_file *m, void *unused)
{
	struct ccci_c2k *c2k = m->private;
	char buf[512];
	int len;

	if (!c2k) {
		seq_puts(m, "MD3 not probed\n");
		return 0;
	}

	len = ccci_c2k_hw_format_regs(c2k, buf, sizeof(buf));
	seq_write(m, buf, len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(c2k_debugfs_regs);

static int c2k_debugfs_wdt_show(struct seq_file *m, void *unused)
{
	struct ccci_c2k *c2k = m->private;
	char buf[256];
	int len;

	if (!c2k) {
		seq_puts(m, "MD3 not probed\n");
		return 0;
	}

	len = ccci_c2k_hw_format_wdt(c2k, buf, sizeof(buf));
	seq_write(m, buf, len);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(c2k_debugfs_wdt);

static int c2k_debugfs_channels_show(struct seq_file *m, void *unused)
{
	return ccci_c2k_ch_dump(m);
}
DEFINE_SHOW_ATTRIBUTE(c2k_debugfs_channels);

static int c2k_debugfs_state_show(struct seq_file *m, void *unused)
{
	struct ccci_c2k *c2k = m->private;

	if (!c2k) {
		seq_puts(m, "MD3 not probed\n");
		return 0;
	}

	seq_printf(m, "state=%s boot_count=%u irat_md_id=MD%u\n",
		   c2k_md_state_name(ccci_md_get_state(CCCI_MD_SYS3)),
		   c2k->boot_count, c2k->irat_md_id + 1);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(c2k_debugfs_state);

/**
 * ccci_c2k_sysfs_init() - create /sys/kernel/c2k and the debugfs directory
 * @c2k: C2K instance
 */
int ccci_c2k_sysfs_init(struct ccci_c2k *c2k)
{
	int ret;

	c2k->kobj = kobject_create_and_add("c2k", kernel_kobj);
	if (!c2k->kobj)
		return -ENOMEM;

	ret = sysfs_create_group(c2k->kobj, &c2k_attr_group);
	if (ret) {
		kobject_put(c2k->kobj);
		c2k->kobj = NULL;
		return ret;
	}

	c2k->debugfs_dir = debugfs_create_dir("ccci_c2k", NULL);
	debugfs_create_file("state", 0444, c2k->debugfs_dir, c2k,
			    &c2k_debugfs_state_fops);
	debugfs_create_file("regs", 0444, c2k->debugfs_dir, c2k,
			    &c2k_debugfs_regs_fops);
	debugfs_create_file("wdt", 0444, c2k->debugfs_dir, c2k,
			    &c2k_debugfs_wdt_fops);
	debugfs_create_file("channels", 0444, c2k->debugfs_dir, c2k,
			    &c2k_debugfs_channels_fops);

	return 0;
}

/**
 * ccci_c2k_sysfs_exit() - remove the sysfs and debugfs interface
 * @c2k: C2K instance
 */
void ccci_c2k_sysfs_exit(struct ccci_c2k *c2k)
{
	debugfs_remove_recursive(c2k->debugfs_dir);
	c2k->debugfs_dir = NULL;

	if (c2k->kobj) {
		sysfs_remove_group(c2k->kobj, &c2k_attr_group);
		kobject_put(c2k->kobj);
		c2k->kobj = NULL;
	}
}
