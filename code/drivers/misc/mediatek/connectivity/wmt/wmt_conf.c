// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN WMT_SOC.cfg parser.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/connectivity/common/conn_soc/core/wmt_conf.c
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 *
 * The configuration file is a plain "key=value" list in
 * /system/etc/firmware/WMT_SOC.cfg.  Only the keys used by the ported
 * functionality are consumed; the coexistence keys of the downstream parser
 * are accepted and ignored because the coexistence engine is not ported.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "wmt_local.h"

static int wmt_conf_parse_u8(const char *val, u8 *dst)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(val, 0, &tmp);
	if (ret || tmp > 0xff)
		return -EINVAL;

	*dst = tmp;
	return 0;
}

static int wmt_conf_parse_u16(const char *val, u16 *dst)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(val, 0, &tmp);
	if (ret || tmp > 0xffff)
		return -EINVAL;

	*dst = tmp;
	return 0;
}

/* wmt_conf.c:520-560 */
static int wmt_conf_parse_pair(const char *key, const char *val)
{
	struct wmt_gen_conf *conf = &g_wmt.gen_conf;
	int ret;

	if (!strcmp(key, "co_clock_flag"))
		ret = wmt_conf_parse_u8(val, &conf->co_clock_flag);
	else if (!strcmp(key, "wmt_gps_lna_pin"))
		ret = wmt_conf_parse_u8(val, &conf->wmt_gps_lna_pin);
	else if (!strcmp(key, "wmt_gps_lna_enable"))
		ret = wmt_conf_parse_u8(val, &conf->wmt_gps_lna_enable);
	else if (!strcmp(key, "bt_tssi_from_wifi"))
		ret = wmt_conf_parse_u8(val, &conf->bt_tssi_from_wifi);
	else if (!strcmp(key, "bt_tssi_target"))
		ret = wmt_conf_parse_u16(val, &conf->bt_tssi_target);
	else
		return -ENOENT;	/* coexistence/debug key, not ported */

	if (ret)
		wmt_warn("invalid value '%s' for %s\n", val, key);

	return ret;
}

/* wmt_conf.c:330-470 */
static int wmt_conf_parse(const u8 *buf, size_t size)
{
	char *line, *next, *eq, *key, *val;
	char *copy;
	int ret = 0;

	copy = kmemdup_nul((const char *)buf, size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	for (line = copy; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		line = strim(line);
		if (!*line)
			continue;

		eq = strchr(line, '=');
		if (!eq) {
			wmt_dbg("malformed config line '%s'\n", line);
			continue;
		}

		*eq = '\0';
		key = strim(line);
		val = strim(eq + 1);

		if (wmt_conf_parse_pair(key, val) == -EINVAL)
			ret = -EINVAL;
	}

	kfree(copy);
	return ret;
}

int wmt_conf_read_file(void)
{
	struct wmt_gen_conf *conf = &g_wmt.gen_conf;
	u8 *buf = NULL;
	size_t size = 0;
	int ret;

	memset(conf, 0, sizeof(*conf));

	ret = wmt_dev_read_file(g_wmt.cfg_name, &buf, &size);
	if (ret) {
		wmt_dbg("cannot read %s (%d)\n", g_wmt.cfg_name, ret);
		return ret;
	}

	ret = wmt_conf_parse(buf, size);
	kfree(buf);
	if (ret) {
		wmt_err("failed to parse %s\n", g_wmt.cfg_name);
		return ret;
	}

	conf->co_clock_flag &= 0x0f;
	conf->cfg_exist = true;

	wmt_info("%s: co-clock %u, gps lna pin %u enable %u, bt tssi %u/%u\n",
		 g_wmt.cfg_name, conf->co_clock_flag, conf->wmt_gps_lna_pin,
		 conf->wmt_gps_lna_enable, conf->bt_tssi_from_wifi,
		 conf->bt_tssi_target);
	return 0;
}

int wmt_conf_set_cfg_file(const char *name)
{
	if (!name || !name[0])
		return -EINVAL;

	strscpy(g_wmt.cfg_name, name, sizeof(g_wmt.cfg_name));
	wmt_info("WMT config file set to %s\n", g_wmt.cfg_name);
	return 0;
}

const struct wmt_gen_conf *wmt_conf_get(void)
{
	return g_wmt.gen_conf.cfg_exist ? &g_wmt.gen_conf : NULL;
}
