// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI kernel RPC engine: CCCI_RPC_RX request parser, service registry and
 * the built-in modem-firmware RPC services.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_kernel.c (ccci_rpc_work_helper() and
 * rpc_msg_handler()) and port_kernel.h.  Original copyright (C) 2015
 * MediaTek Inc., GPL-2.0.
 *
 * The wire format, op ids, argument layouts and FS_* result codes are the
 * modem firmware ABI and are copied verbatim from port_kernel.h:107-300.
 *
 * Handlers whose downstream backend has no counterpart in this tree answer
 * the downstream error code for that op and log the missing input; they never
 * return made-up data.  The deviations are listed in the leaf report and in
 * the comment above each handler.
 */
#include <linux/ccci.h>
#include <linux/clkbuf.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/refcount.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ccci_priv.h"
#include "ccci_rpc.h"

/* Each response argument owns one arena slot, so no two arguments alias. */
#define CCCI_RPC_ARG_SLOT	((CCCI_RPC_ARENA_SIZE / CCCI_RPC_MAX_ARGS) & ~3U)

/* ccci_rpc_init() asserts that every built-in response fits one slot. */

/* ------------------------------------------------------------------ */
/* service registry                                                    */
/* ------------------------------------------------------------------ */
struct ccci_rpc_service {
	struct list_head node;
	u32 op_id;
	char *name;
	ccci_rpc_handler_t handler;
	refcount_t refs;
};

static LIST_HEAD(ccci_rpc_services);
static DEFINE_MUTEX(ccci_rpc_lock);
static bool ccci_rpc_ready;

static struct ccci_rpc_service *ccci_rpc_lookup(u32 op_id)
{
	struct ccci_rpc_service *svc;

	mutex_lock(&ccci_rpc_lock);
	list_for_each_entry(svc, &ccci_rpc_services, node) {
		if (svc->op_id == op_id) {
			refcount_inc(&svc->refs);
			mutex_unlock(&ccci_rpc_lock);
			return svc;
		}
	}
	mutex_unlock(&ccci_rpc_lock);
	return NULL;
}

static void ccci_rpc_put(struct ccci_rpc_service *svc)
{
	if (refcount_dec_and_test(&svc->refs)) {
		kfree(svc->name);
		kfree(svc);
	}
}

/* caller holds ccci_rpc_lock */
static int __ccci_rpc_register(u32 op_id, const char *name,
			       ccci_rpc_handler_t handler)
{
	struct ccci_rpc_service *svc;

	list_for_each_entry(svc, &ccci_rpc_services, node) {
		if (svc->op_id == op_id)
			return -EBUSY;
	}

	svc = kzalloc(sizeof(*svc), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;
	svc->name = kstrdup(name ? name : "rpc", GFP_KERNEL);
	if (!svc->name) {
		kfree(svc);
		return -ENOMEM;
	}
	svc->op_id = op_id;
	svc->handler = handler;
	refcount_set(&svc->refs, 1);
	list_add_tail(&svc->node, &ccci_rpc_services);
	return 0;
}

int ccci_rpc_register_service(u32 op_id, const char *name,
			      ccci_rpc_handler_t handler)
{
	int ret;

	if (!handler)
		return -EINVAL;

	mutex_lock(&ccci_rpc_lock);
	ret = __ccci_rpc_register(op_id, name, handler);
	mutex_unlock(&ccci_rpc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ccci_rpc_register_service);

int ccci_rpc_unregister_service(u32 op_id)
{
	struct ccci_rpc_service *svc;
	int ret = -ENOENT;

	mutex_lock(&ccci_rpc_lock);
	list_for_each_entry(svc, &ccci_rpc_services, node) {
		if (svc->op_id == op_id) {
			list_del(&svc->node);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&ccci_rpc_lock);

	if (!ret)
		ccci_rpc_put(svc);
	return ret;
}
EXPORT_SYMBOL_GPL(ccci_rpc_unregister_service);

/* ------------------------------------------------------------------ */
/* response helpers                                                    */
/* ------------------------------------------------------------------ */
static void ccci_rpc_rsp_u32(struct ccci_rpc_msg *rsp, unsigned int index,
			     u32 value)
{
	if (index >= CCCI_RPC_MAX_ARGS)
		return;
	memcpy(rsp->args[index].buf, &value, sizeof(value));
	rsp->args[index].len = sizeof(value);
	if (rsp->arg_count < index + 1)
		rsp->arg_count = index + 1;
}

static int ccci_rpc_rsp_data(struct ccci_rpc_msg *rsp, unsigned int index,
			     const void *data, unsigned int len)
{
	if (index >= CCCI_RPC_MAX_ARGS || len > CCCI_RPC_ARG_SLOT)
		return -EMSGSIZE;
	if (len)
		memcpy(rsp->args[index].buf, data, len);
	rsp->args[index].len = len;
	if (rsp->arg_count < index + 1)
		rsp->arg_count = index + 1;
	return 0;
}

/*
 * Result code of a handler that returned a negative errno.  Only handlers
 * registered through ccci_rpc_register_service() can return negative, the
 * built-ins return 0 and fill the FS_* code themselves.
 * [INFERENCE] the mapping itself; the FS_* codes are port_kernel.h:213-221.
 */
static int ccci_rpc_errno_to_fs(int err)
{
	switch (err) {
	case -EOPNOTSUPP:
	case -ENODEV:
		return FS_NO_FEATURE;
	case -EINVAL:
		return FS_PARAM_ERROR;
	case -ENOENT:
		return FS_NO_OP;
	case -ENOMEM:
		return FS_ERROR_RESERVED;
	case -EMSGSIZE:
		return FS_MEM_OVERFLOW;
	default:
		return FS_FUNC_FAIL;
	}
}

/* ------------------------------------------------------------------ */
/* GPIO provider                                                       */
/* ------------------------------------------------------------------ */
/*
 * Downstream get_md_gpio_info() (port_kernel.c:822-864) maps the modem's
 * GPIO name through gpio_mapping_table[] (port_kernel.c:81-92) onto a
 * property of the "mediatek,gpio_usage_mapping" node (mt6735.dtsi:528-530,
 * ROM rom_dtb_0.dts:3004-3015), and falls back to a property that carries
 * the modem-side name directly.  Only the *names* are a table; every pin
 * number comes from device tree.
 */
static const struct {
	const char *md_name;
	const char *dts_name;
} ccci_rpc_gpio_names[] = {
	{ "GPIO_FDD_Band_Support_Detection_1",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_1ST_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_2",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_2ND_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_3",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_3RD_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_4",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_4TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_5",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_5TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_6",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_6TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_7",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_7TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_8",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_8TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_9",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_9TH_PIN" },
	{ "GPIO_FDD_Band_Support_Detection_A",
	  "GPIO_FDD_BAND_SUPPORT_DETECT_ATH_PIN" },
};

/*
 * The returned number is the line number inside the mt6735 pin controller
 * (the value stored in DT).  Downstream can return it as a global GPIO
 * number because the 3.18 MTK pinctrl chip gets a stable base; the mainline
 * driver registers with base = -1 (pinctrl-moore.c:575), so the controller
 * node is resolved by compatible and the number is used as the offset
 * (pio: gpio-ranges = <&pio 0 0 203>, mt6735.dtsi:295).
 */
static int ccci_rpc_gpio_num(const char *name, unsigned int len)
{
	struct device_node *node;
	u32 gpio = 0;
	unsigned int i;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "mediatek,gpio_usage_mapping");
	if (!node) {
		pr_info("ccci: no gpio_usage_mapping node for GPIO %s\n", name);
		return -ENOENT;
	}

	ret = -ENOENT;
	for (i = 0; i < ARRAY_SIZE(ccci_rpc_gpio_names); i++) {
		if (strncmp(name, ccci_rpc_gpio_names[i].md_name, len))
			continue;
		if (!of_property_read_u32(node,
					  ccci_rpc_gpio_names[i].dts_name,
					  &gpio))
			ret = 0;
		break;
	}

	/* downstream retries with the modem name when the first read failed */
	if (ret && !of_property_read_u32(node, name, &gpio))
		ret = 0;
	of_node_put(node);

	if (ret)
		return -ENOENT;
	return gpio;
}

static struct gpio_desc *ccci_rpc_gpio_desc(unsigned int num,
					    struct gpio_device **gdev_out)
{
	struct device_node *np;
	struct gpio_device *gdev;
	struct gpio_chip *gc;
	struct gpio_desc *desc;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6735-pinctrl");
	if (!np)
		return ERR_PTR(-ENODEV);
	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(np));
	of_node_put(np);
	if (!gdev)
		return ERR_PTR(-ENODEV);

	gc = gpio_device_get_chip(gdev);
	if (!gc || num >= gc->ngpio) {
		gpio_device_put(gdev);
		return ERR_PTR(-EINVAL);
	}
	desc = gpio_device_get_desc(gdev, num);
	if (IS_ERR(desc)) {
		gpio_device_put(gdev);
		return desc;
	}
	*gdev_out = gdev;
	return desc;
}

/* downstream get_md_gpio_val() (port_kernel.c:772-782) */
static int ccci_rpc_gpio_get(unsigned int num, u32 *value)
{
	struct gpio_device *gdev = NULL;
	struct gpio_desc *desc;
	int ret;

	desc = ccci_rpc_gpio_desc(num, &gdev);
	if (IS_ERR(desc))
		return PTR_ERR(desc);
	ret = gpiod_get_value_cansleep(desc);
	gpio_device_put(gdev);
	if (ret < 0)
		return ret;
	*value = ret;
	return 0;
}

/* ------------------------------------------------------------------ */
/* ADC provider                                                        */
/* ------------------------------------------------------------------ */
/*
 * Downstream get_md_adc_info() (port_kernel.c:801-817) calls
 * IMM_get_adc_channel_num() (mt_auxadc.c:514-525), whose name->channel list
 * is built from the "mediatek,adc_channel" node properties
 * (mt_auxadc.c:1645-1718, cust_adc.dtsi:12-20, ROM rom_dtb_0.dts:3161-3168).
 * The names are ABI, the channel numbers are device tree data.
 */
static const struct {
	const char *name;
	const char *prop;
} ccci_rpc_adc_names[] = {
	{ "ADC_RFTMP", "mediatek,temperature0" },
	{ "ADC_APTMP", "mediatek,temperature1" },
	{ "ADC_FDD_Rf_Params_Dynamic_Custom",
	  "mediatek,adc_fdd_rf_params_dynamic_custom_ch" },
	{ "ADC_MIC", "mediatek,hf_mic" },
	{ "ADC_LCM_VOLTAGE", "mediatek,lcm_voltage" },
	{ "ADC_BATTERY_VOLTAGE", "mediatek,battery_voltage" },
	{ "ADC_CHARGER_VOLTAGE", "mediatek,charger_voltage" },
	{ "ADC_UTMS", "mediatek,utms" },
	{ "ADC_REF_CURRENT", "mediatek,ref_current" },
};

static int ccci_rpc_adc_num(const char *name, unsigned int len)
{
	struct device_node *node;
	unsigned int i;
	u32 channel;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "mediatek,adc_channel");
	if (!node)
		return -ENOENT;

	ret = -ENOENT;
	for (i = 0; i < ARRAY_SIZE(ccci_rpc_adc_names); i++) {
		if (strncmp(name, ccci_rpc_adc_names[i].name, len))
			continue;
		if (!of_property_read_u32(node, ccci_rpc_adc_names[i].prop,
					  &channel))
			ret = (int)channel;
		break;
	}

	of_node_put(node);
	return ret;
}

/* IIO device of the auxadc, reference held; see the note in adc_raw() */
static struct iio_dev *ccci_rpc_auxadc(void)
{
	struct device_node *node;
	struct device *dev;

	/* the only auxadc binding of this SoC, ROM rom_dtb_0.dts:3159 */
	node = of_find_compatible_node(NULL, NULL, "mediatek,mt6735-auxadc");
	if (!node)
		return NULL;
	dev = bus_find_device_by_of_node(&iio_bus_type, node);
	of_node_put(node);
	return dev ? dev_to_iio_dev(dev) : NULL;
}

/*
 * Downstream get_md_adc_val() -> IMM_GetOneChannelValue()
 * (mt_auxadc.c:527-530) returns the raw 12-bit AUXADC count.
 *
 * The in-tree provider (drivers/iio/adc/mt6577_auxadc.c) exposes only
 * IIO_CHAN_INFO_PROCESSED (mt6577_auxadc.c:64-69,184-210), whose value is the
 * raw count scaled to millivolts with integer division, and it does not match
 * "mediatek,mt6735-auxadc" (mt6577_auxadc.c:313-320).  A raw count therefore
 * cannot be produced: reconstructing it from the millivolt value would be
 * made-up data, so the IIO read fails and the caller answers FS_NO_FEATURE.
 */
static int ccci_rpc_adc_raw(unsigned int num, int *raw)
{
	struct iio_dev *indio_dev;
	const struct iio_chan_spec *spec = NULL;
	struct iio_channel channel;
	unsigned int i;
	int ret;

	indio_dev = ccci_rpc_auxadc();
	if (!indio_dev)
		return -ENODEV;

	for (i = 0; i < indio_dev->num_channels; i++) {
		if (indio_dev->channels[i].channel == num) {
			spec = &indio_dev->channels[i];
			break;
		}
	}
	if (!spec) {
		iio_device_put(indio_dev);
		return -EINVAL;
	}

	memset(&channel, 0, sizeof(channel));
	channel.indio_dev = indio_dev;
	channel.channel = spec;
	ret = iio_read_channel_raw(&channel, raw);
	iio_device_put(indio_dev);
	return ret;
}

/* ------------------------------------------------------------------ */
/* SIM hot plug EINT attributes                                        */
/* ------------------------------------------------------------------ */
/*
 * Downstream get_dtsi_eint_node()/get_eint_attr_val()/get_eint_attr_DTSVal()
 * (port_kernel.c:880-1027) read the attributes once, at subsystem init, from
 * the per-SIM EINT nodes of the device tree.  Values below are the property
 * name and cell index of md_eint_struct[] (port_kernel.c:882-892).
 */
#define CCCI_RPC_EINT_NODES	4

struct ccci_rpc_eint_node {
	const char *node_name;
	bool exist;
	int value[SIM_HOT_PLUG_EINT_MAX];
};

/* node names from md_eint_node[] (port_kernel.c:894-909) */
static struct ccci_rpc_eint_node ccci_rpc_eint[CCCI_RPC_EINT_NODES] = {
	{ .node_name = "MD1_SIM1_HOT_PLUG_EINT" },
	{ .node_name = "MD1_SIM2_HOT_PLUG_EINT" },
	{ .node_name = "MD1_SIM3_HOT_PLUG_EINT" },
	{ .node_name = "MD1_SIM4_HOT_PLUG_EINT" },
};

static const struct {
	const char *property;
	int index;
} ccci_rpc_eint_props[SIM_HOT_PLUG_EINT_MAX] = {
	[SIM_HOT_PLUG_EINT_NUMBER] = { "interrupts", 0 },
	[SIM_HOT_PLUG_EINT_DEBOUNCETIME] = { "debounce", 1 },
	[SIM_HOT_PLUG_EINT_POLARITY] = { "interrupts", 1 },
	[SIM_HOT_PLUG_EINT_SENSITIVITY] = { "interrupts", 1 },
	[SIM_HOT_PLUG_EINT_SOCKETTYPE] = { "sockettype", 1 },
	[SIM_HOT_PLUG_EINT_DEDICATEDEN] = { "dedicated", 1 },
	[SIM_HOT_PLUG_EINT_SRCPIN] = { "src_pin", 1 },
};

/* downstream get_eint_attr_val(), port_kernel.c:920-968 */
static int ccci_rpc_eint_read(struct device_node *node,
			      struct ccci_rpc_eint_node *eint)
{
	int type, value, ret;

	for (type = 0; type < SIM_HOT_PLUG_EINT_MAX; type++) {
		ret = of_property_read_u32_index(node,
						 ccci_rpc_eint_props[type].property,
						 ccci_rpc_eint_props[type].index,
						 &value);
		if (ret) {
			eint->value[type] = ERR_SIM_HOT_PLUG_QUERY_TYPE;
			pr_info("ccci: EINT %s: property %s not found\n",
				eint->node_name,
				ccci_rpc_eint_props[type].property);
			return ERR_SIM_HOT_PLUG_QUERY_TYPE;
		}

		if (type == SIM_HOT_PLUG_EINT_POLARITY) {
			/* polarity and sensitivity share the "interrupts" cell */
			switch (value) {
			case IRQ_TYPE_EDGE_RISING:
			case IRQ_TYPE_EDGE_FALLING:
			case IRQ_TYPE_LEVEL_HIGH:
			case IRQ_TYPE_LEVEL_LOW:
				eint->value[SIM_HOT_PLUG_EINT_POLARITY] =
					(value & 0x5) ? 1 : 0;
				eint->value[SIM_HOT_PLUG_EINT_SENSITIVITY] =
					(value & 0x3) ? 1 : 0;
				break;
			default:
				eint->value[SIM_HOT_PLUG_EINT_POLARITY] = -1;
				eint->value[SIM_HOT_PLUG_EINT_SENSITIVITY] = -1;
				pr_err("ccci: EINT %s: invalid trigger type %d\n",
				       eint->node_name, value);
				break;
			}
			type++;	/* sensitivity was filled above */
		} else if (type == SIM_HOT_PLUG_EINT_DEBOUNCETIME) {
			/* AP unit is us, MD unit is ms (port_kernel.c:935) */
			eint->value[type] = value / 1000;
		} else {
			eint->value[type] = value;
		}
	}
	return 0;
}

/* downstream get_dtsi_eint_node(), port_kernel.c:972-1000 */
static void ccci_rpc_eint_load(void)
{
	struct device_node *node;
	unsigned int i;

	for (i = 0; i < CCCI_RPC_EINT_NODES; i++) {
		node = of_find_node_by_name(NULL, ccci_rpc_eint[i].node_name);
		if (!node) {
			pr_info("ccci: EINT node %s not found\n",
				ccci_rpc_eint[i].node_name);
			continue;
		}
		ccci_rpc_eint[i].exist = true;
		ccci_rpc_eint_read(node, &ccci_rpc_eint[i]);
		of_node_put(node);
	}
}

/* downstream get_eint_attr_DTSVal(), port_kernel.c:1002-1027 */
static int ccci_rpc_eint_get(const char *name, unsigned int name_len,
			     unsigned int type, int *value)
{
	unsigned int i;

	if (!name || !value)
		return ERR_SIM_HOT_PLUG_NULL_POINTER;
	if (type >= SIM_HOT_PLUG_EINT_MAX)
		return ERR_SIM_HOT_PLUG_QUERY_TYPE;

	for (i = 0; i < CCCI_RPC_EINT_NODES; i++) {
		if (!ccci_rpc_eint[i].exist)
			continue;
		if (!strncmp(name, ccci_rpc_eint[i].node_name, name_len)) {
			*value = ccci_rpc_eint[i].value[type];
			return 0;
		}
	}
	return ERR_SIM_HOT_PLUG_QUERY_STRING;
}

/* ------------------------------------------------------------------ */
/* channel/queue remap                                                 */
/* ------------------------------------------------------------------ */
/*
 * Downstream ccci_rpc_remap_ch_queue() (port_kernel.c:1166-1205) rewrites
 * port->txq_index, and for a DL-ACK channel port->txq_exp_index = q | 0xF0.
 * This tree's ccci_port has no dl_ack_ch field, so the DL-ACK channel is
 * resolved through the port's ccmni channel map (ccci_get_ccmni_channel(),
 * ccci_port_net.c:104-136).
 */
static int ccci_rpc_remap_ch_queue(struct ccci_md *md,
				   const struct ccci_rpc_queue_mapping *map)
{
	struct ccci_port *port;
	struct ccmni_ch channel;
	unsigned int i;
	u32 ch, queue;

	if (map->ch_cnt > MAX_CHANNEL_QUEUE_MAPPING)
		return -EINVAL;

	for (i = 0; i < map->ch_cnt; i++) {
		ch = map->ch_queue_mapping[i].channel;
		queue = map->ch_queue_mapping[i].queue_type;

		if (ch >= CCCI_MAX_CH_NUM || queue >= CCCI_MAX_QUEUES)
			return -EINVAL;

		list_for_each_entry(port, &md->ports_list, node) {
			if (port->tx_ch == ch) {
				port->txq = queue;
				break;
			}
			if (port->ops != &ccci_net_port_ops)
				continue;
			if (ccci_get_ccmni_channel(md->id,
						   (port->minor & 0xF) - 1,
						   &channel))
				continue;
			if (channel.dl_ack == ch) {
				port->txq_exp = queue | 0xF0;
				break;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* GPIO / ADC payload filling                                          */
/* ------------------------------------------------------------------ */
static void ccci_rpc_gpio_fill(unsigned int count, u32 req_mask, u32 valid_mask,
			       const char names[][GPIO_PIN_NAME_STR_MAX_LEN],
			       const u32 *pin_num_in, u32 *pin_num_out,
			       u32 *pin_val_out)
{
	unsigned int i;
	int num;

	if ((req_mask & (RPC_REQ_GPIO_PIN | RPC_REQ_GPIO_VALUE)) ==
	    (RPC_REQ_GPIO_PIN | RPC_REQ_GPIO_VALUE)) {
		for (i = 0; i < count; i++) {
			if (!(valid_mask & BIT(i)))
				continue;
			num = ccci_rpc_gpio_num(names[i],
						strnlen(names[i],
							GPIO_PIN_NAME_STR_MAX_LEN));
			if (num < 0)
				continue;
			pin_num_out[i] = num;
			if (ccci_rpc_gpio_get(num, &pin_val_out[i]))
				pin_val_out[i] = (u32)-1;
		}
		return;
	}

	if (req_mask & RPC_REQ_GPIO_PIN) {
		for (i = 0; i < count; i++) {
			if (!(valid_mask & BIT(i)))
				continue;
			num = ccci_rpc_gpio_num(names[i],
						strnlen(names[i],
							GPIO_PIN_NAME_STR_MAX_LEN));
			if (num >= 0)
				pin_num_out[i] = num;
		}
	}
	if (req_mask & RPC_REQ_GPIO_VALUE) {
		for (i = 0; i < count; i++) {
			if (!(valid_mask & BIT(i)))
				continue;
			if (ccci_rpc_gpio_get(pin_num_in[i], &pin_val_out[i]))
				pin_val_out[i] = (u32)-1;
		}
	}
}

/*
 * Returns an FS_* code.  Downstream ignores provider failures here and always
 * answers FS_NO_ERROR with a 0xF-filled block; this port reports the failure
 * instead of returning fabricated "unknown" values as success.
 */
static int ccci_rpc_adc_fill(const char *name, unsigned int len, u32 req_mask,
			     u32 meas_count, u32 req_ch, u32 *adc_num,
			     u32 *adc_sum)
{
	bool want_pin = req_mask & RPC_REQ_ADC_PIN;
	bool want_val = req_mask & RPC_REQ_ADC_VALUE;
	int num = -1, raw;
	u32 i;

	if (want_pin) {
		num = ccci_rpc_adc_num(name, strnlen(name, ADC_CH_NAME_STR_MAX_LEN));
		if (num < 0)
			return FS_FUNC_FAIL;
		*adc_num = num;
	}
	if (!want_val)
		return FS_NO_ERROR;

	if (!want_pin)
		num = req_ch;	/* downstream samples the requested channel */
	*adc_sum = 0;
	for (i = 0; i < meas_count; i++) {
		if (ccci_rpc_adc_raw(num, &raw))
			return FS_NO_FEATURE;
		*adc_sum += raw;
	}
	return FS_NO_ERROR;
}

static void ccci_rpc_gpio_adc_v1(const struct ccci_rpc_gpio_adc_input *in,
				 struct ccci_rpc_gpio_adc_output *out,
				 int *status)
{
	u32 pin_num[GPIO_MAX_COUNT], pin_val[GPIO_MAX_COUNT];
	u32 adc_num = 0xFFFFFFFF, adc_sum = 0xFFFFFFFF;
	int adc_status;

	memset(pin_num, 0xF, sizeof(pin_num));
	memset(pin_val, 0xF, sizeof(pin_val));
	ccci_rpc_gpio_fill(GPIO_MAX_COUNT, in->reqMask, in->gpioValidPinMask,
			   in->gpioPinName, in->gpioPinNum, pin_num, pin_val);
	if (in->reqMask & (RPC_REQ_ADC_PIN | RPC_REQ_ADC_VALUE)) {
		adc_status = ccci_rpc_adc_fill(in->adcChName,
					       ADC_CH_NAME_STR_MAX_LEN,
					       in->reqMask, in->adcChMeasCount,
					       in->adcChNum,
					       &adc_num, &adc_sum);
		if (adc_status)
			*status = adc_status;
	}

	memcpy(out->gpioPinNum, pin_num, sizeof(pin_num));
	memcpy(out->gpioPinValue, pin_val, sizeof(pin_val));
	out->adcChNum = adc_num;
	out->adcChMeasSum = adc_sum;
}

static void ccci_rpc_gpio_adc_v2(const struct ccci_rpc_gpio_adc_input_v2 *in,
				 struct ccci_rpc_gpio_adc_output_v2 *out,
				 int *status)
{
	u32 pin_num[GPIO_MAX_COUNT_V2], pin_val[GPIO_MAX_COUNT_V2];
	u32 adc_num = 0xFFFFFFFF, adc_sum = 0xFFFFFFFF;
	int adc_status;

	memset(pin_num, 0xF, sizeof(pin_num));
	memset(pin_val, 0xF, sizeof(pin_val));
	ccci_rpc_gpio_fill(GPIO_MAX_COUNT_V2, in->reqMask, in->gpioValidPinMask,
			   in->gpioPinName, in->gpioPinNum, pin_num, pin_val);
	if (in->reqMask & (RPC_REQ_ADC_PIN | RPC_REQ_ADC_VALUE)) {
		adc_status = ccci_rpc_adc_fill(in->adcChName,
					       ADC_CH_NAME_STR_MAX_LEN,
					       in->reqMask, in->adcChMeasCount,
					       in->adcChNum,
					       &adc_num, &adc_sum);
		if (adc_status)
			*status = adc_status;
	}

	memcpy(out->gpioPinNum, pin_num, sizeof(pin_num));
	memcpy(out->gpioPinValue, pin_val, sizeof(pin_val));
	out->adcChNum = adc_num;
	out->adcChMeasSum = adc_sum;
}

/* ------------------------------------------------------------------ */
/* request argument helpers                                            */
/* ------------------------------------------------------------------ */
static void ccci_rpc_reply_error(struct ccci_rpc_msg *rsp, int code);

static int ccci_rpc_name_arg(const struct ccci_rpc_msg *req, char *name,
			     unsigned int size, unsigned int *len)
{
	if (req->arg_count < 1 || !req->args[0].buf || !req->args[0].len)
		return -EINVAL;
	if (req->args[0].len >= size)
		return -EINVAL;
	memcpy(name, req->args[0].buf, req->args[0].len);
	name[req->args[0].len] = '\0';
	*len = req->args[0].len;
	return 0;
}

/* ------------------------------------------------------------------ */
/* built-in handlers                                                   */
/* ------------------------------------------------------------------ */

/* IPC_RPC_GET_TDD_EINT_NUM_OP / _GPIO_NUM_OP / _ADC_NUM_OP (1474-1541) */
static int ccci_rpc_get_num(int md_id, const struct ccci_rpc_msg *req,
			    struct ccci_rpc_msg *rsp)
{
	char name[CCCI_RPC_NAME_MAX];
	unsigned int len;
	int num;

	if (req->arg_count < 2 || req->arg_count > RPC_MAX_ARG_NUM ||
	    ccci_rpc_name_arg(req, name, sizeof(name), &len)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}

	switch (req->op_id) {
	case IPC_RPC_GET_GPIO_NUM_OP:
		num = ccci_rpc_gpio_num(name, len);
		break;
	case IPC_RPC_GET_ADC_NUM_OP:
		num = ccci_rpc_adc_num(name, len);
		break;
	default:
		/* FEATURE_GET_TD_EINT_NUM is disabled in this build
		 * (mt6735/ccci_config.h:34-37, "#if 0" DEPRECATED block), so
		 * get_td_eint_info() answers -1 (port_kernel.c:792-798).
		 */
		num = -1;
		break;
	}

	if (num < 0)
		num = FS_FUNC_FAIL;

	ccci_rpc_rsp_u32(rsp, 0, num);
	ccci_rpc_rsp_u32(rsp, 1, num);
	return 0;
}

/* IPC_RPC_GET_EMI_CLK_TYPE_OP (1542-1583) */
static int ccci_rpc_get_emi_clk_type(int md_id,
				     const struct ccci_rpc_msg *req,
				     struct ccci_rpc_msg *rsp)
{
	unsigned int code = FS_FUNC_FAIL;

	/* FEATURE_GET_DRAM_TYPE_CLK is disabled in this build
	 * (mt6735/ccci_config.h:35-37), get_dram_type_clk() answers -1 and
	 * port_kernel.c:1555-1560 fills all three arguments with FS_FUNC_FAIL.
	 */
	if (req->arg_count != 0)
		code = FS_PARAM_ERROR;

	ccci_rpc_rsp_u32(rsp, 0, code);
	ccci_rpc_rsp_u32(rsp, 1, code);
	ccci_rpc_rsp_u32(rsp, 2, code);
	return 0;
}

/* IPC_RPC_GET_EINT_ATTR_OP (1586-1648) */
static int ccci_rpc_get_eint_attr(int md_id,
				  const struct ccci_rpc_msg *req,
				  struct ccci_rpc_msg *rsp)
{
	char name[CCCI_RPC_NAME_MAX];
	unsigned int len, type;
	int ret, value;

	if (req->arg_count < 3 || req->arg_count > RPC_MAX_ARG_NUM ||
	    ccci_rpc_name_arg(req, name, sizeof(name), &len) ||
	    !req->args[2].buf || req->args[2].len < sizeof(type)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}
	memcpy(&type, req->args[2].buf, sizeof(type));

	ret = ccci_rpc_eint_get(name, len, type, &value);
	if (ret) {
		ccci_rpc_rsp_u32(rsp, 0, ret);
		ccci_rpc_rsp_u32(rsp, 1, ret);
		return 0;
	}
	ccci_rpc_rsp_u32(rsp, 0, 0);
	ccci_rpc_rsp_u32(rsp, 1, (u32)value);
	return 0;
}

/*
 * IPC_RPC_GET_RF_CLK_BUF_OP, op 0x4008 (port_kernel.c:1650-1707).
 *
 * Downstream (FEATURE_RF_CLK_BUF, mt6735/ccci_config.h:57) answers with
 * CLKBuf_Status = "mediatek,clkbuf-config" of the "mediatek,rf_clock_buffer"
 * node and CLKBuf_SWCtrl_Status = clk_buf_get_swctrl_status().  A request
 * whose count is not CLKBUF_MAX_COUNT is answered with CLKBuf_Count 0xFF and
 * an all-zero status array.
 *
 * The clkbuf driver must be built in and bound to its DT node; otherwise the
 * answer is FS_FUNC_FAIL rather than a made-up status array.
 */
static bool ccci_rpc_clkbuf_ready(void)
{
	struct platform_device *pdev;
	struct device_node *node;
	bool bound;

	node = of_find_compatible_node(NULL, NULL, "mediatek,rf_clock_buffer");
	if (!node)
		return false;

	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return false;

	bound = device_is_bound(&pdev->dev);
	put_device(&pdev->dev);

	return bound;
}

static int ccci_rpc_get_rf_clk_buf(int md_id,
				   const struct ccci_rpc_msg *req,
				   struct ccci_rpc_msg *rsp)
{
	struct ccci_rpc_clkbuf_result result;
	enum clk_buf_swctrl_status swctrl[CLKBUF_MAX_COUNT];
	struct device_node *node;
	u32 vals[CLKBUF_MAX_COUNT];
	u16 count;
	int i, ret;

	if (req->arg_count != 1 || !req->args[0].buf ||
	    req->args[0].len < sizeof(count)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}
	memcpy(&count, req->args[0].buf, sizeof(count));

	memset(&result, 0, sizeof(result));
	ccci_rpc_rsp_u32(rsp, 0, FS_NO_ERROR);

	/* port_kernel.c:1675-1679 */
	if (count != CLKBUF_MAX_COUNT) {
		pr_warn("ccci: md%d %s: invalid clkbuf count %u/%d\n",
			md_id + 1, __func__, count, CLKBUF_MAX_COUNT);
		result.CLKBuf_Count = 0xFF;
		ccci_rpc_rsp_data(rsp, 1, &result, sizeof(result));
		return 0;
	}

	if (!IS_ENABLED(CONFIG_MTK_CLKBUF) || !ccci_rpc_clkbuf_ready()) {
		pr_err("ccci: md%d %s: clock buffer driver not probed\n",
		       md_id + 1, __func__);
		ccci_rpc_rsp_u32(rsp, 0, FS_FUNC_FAIL);
		ccci_rpc_rsp_u32(rsp, 1, FS_FUNC_FAIL);
		return 0;
	}

	/* port_kernel.c:1681-1694 */
	node = of_find_compatible_node(NULL, NULL, "mediatek,rf_clock_buffer");
	if (node)
		ret = of_property_read_u32_array(node, "mediatek,clkbuf-config",
						 vals, CLKBUF_MAX_COUNT);
	else
		ret = -ENODEV;
	of_node_put(node);
	if (ret) {
		pr_err("ccci: md%d %s: no \"mediatek,clkbuf-config\" (%d)\n",
		       md_id + 1, __func__, ret);
		ccci_rpc_rsp_u32(rsp, 0, FS_FUNC_FAIL);
		ccci_rpc_rsp_u32(rsp, 1, FS_FUNC_FAIL);
		return 0;
	}

	result.CLKBuf_Count = CLKBUF_MAX_COUNT;
	for (i = 0; i < CLKBUF_MAX_COUNT; i++)
		result.CLKBuf_Status[i] = vals[i];

	clk_buf_get_swctrl_status(swctrl);
	for (i = 0; i < CLKBUF_MAX_COUNT; i++)
		result.CLKBuf_SWCtrl_Status[i] = swctrl[i];

	pr_debug("ccci: md%d %s: count=%#x\n", md_id + 1, __func__,
		 result.CLKBuf_Count);

	ccci_rpc_rsp_data(rsp, 1, &result, sizeof(result));
	return 0;
}

/* IPC_RPC_GET_GPIO_VAL_OP / _GET_ADC_VAL_OP (1709-1737) */
static int ccci_rpc_get_value(int md_id, const struct ccci_rpc_msg *req,
			      struct ccci_rpc_msg *rsp)
{
	u32 num, val = (u32)-1;

	if (req->arg_count != 1 || !req->args[0].buf ||
	    req->args[0].len < sizeof(num)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}
	memcpy(&num, req->args[0].buf, sizeof(num));

	if (req->op_id == IPC_RPC_GET_GPIO_VAL_OP) {
		ccci_rpc_gpio_get(num, &val);	/* -1 on failure, as downstream */
	} else {
		int raw;

		if (ccci_rpc_adc_raw(num, &raw))
			val = FS_NO_FEATURE;
		else
			val = raw;
	}

	ccci_rpc_rsp_u32(rsp, 0, val);
	ccci_rpc_rsp_u32(rsp, 1, val);
	return 0;
}

/* IPC_RPC_GET_GPIO_ADC_OP (1739-1795) */
static int ccci_rpc_get_gpio_adc(int md_id, const struct ccci_rpc_msg *req,
				 struct ccci_rpc_msg *rsp)
{
	int status = FS_NO_ERROR;
	int ret;

	if (req->arg_count != 1 || !req->args[0].buf) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}

	if (req->args[0].len == sizeof(struct ccci_rpc_gpio_adc_input)) {
		struct ccci_rpc_gpio_adc_input in;
		struct ccci_rpc_gpio_adc_output out;

		memcpy(&in, req->args[0].buf, sizeof(in));
		memset(&out, 0xF, sizeof(out));	/* 0xF: unknown, downstream */
		ccci_rpc_gpio_adc_v1(&in, &out, &status);
		ccci_rpc_rsp_u32(rsp, 0, status);
		ret = ccci_rpc_rsp_data(rsp, 1, &out, sizeof(out));
	} else if (req->args[0].len ==
		   sizeof(struct ccci_rpc_gpio_adc_input_v2)) {
		struct ccci_rpc_gpio_adc_input_v2 in;
		struct ccci_rpc_gpio_adc_output_v2 out;

		memcpy(&in, req->args[0].buf, sizeof(in));
		memset(&out, 0xF, sizeof(out));
		ccci_rpc_gpio_adc_v2(&in, &out, &status);
		ccci_rpc_rsp_u32(rsp, 0, status);
		ret = ccci_rpc_rsp_data(rsp, 1, &out, sizeof(out));
	} else {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}

	if (ret) {
		ccci_rpc_reply_error(rsp, FS_MEM_OVERFLOW);
		return 0;
	}
	return 0;
}

/*
 * IPC_RPC_CPSVC_SECURE_ALGO_OP (1221-1350).
 *
 * This ROM build enables both ENABLE_MD_IMG_SECURITY_FEATURE and
 * MTK_SEC_MODEM_NVRAM_ANTI_CLONE (eccci/Makefile:27-32,
 * grandpplte_00_defconfig:1410-1411), i.e. the compiled-in path is
 * masp_secure_algo_init() -> masp_secure_algo() (HACC hardware crypto with
 * the MASP key material).  Neither the MASP/HACC driver nor its key material
 * exists in this tree, so the transform cannot be performed.  Downstream has
 * no error return for this case (it BUG_ON()s when the engine fails to
 * initialise, port_kernel.c:1297-1300); FS_NO_FEATURE is the closest ABI code.
 */
static int ccci_rpc_secure_algo(int md_id,
				const struct ccci_rpc_msg *req,
				struct ccci_rpc_msg *rsp)
{
	if (req->arg_count < 4 || req->arg_count >= RPC_MAX_ARG_NUM) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		return 0;
	}

	pr_err("ccci: md%d secure algo RPC needs the MASP/HACC driver\n",
	       md_id + 1);
	ccci_rpc_rsp_u32(rsp, 0, FS_NO_FEATURE);
	return 0;
}

/*
 * IPC_RPC_GET_SECRO_OP (1352-1472).
 *
 * Downstream asfv2 (the variant 3.18 builds, masp/Makefile:29) has
 * masp_secro_en() == 0 unconditionally
 * (masp/asfv2/core/sec_legacy.c:26-29), so the
 * handler always takes the "secro disable" branch: FS_NO_FEATURE plus an
 * image length of 0 (port_kernel.c:1455-1466).  The ROM disassembly calls
 * masp_secro_en() at 0xc0514c74, and the string "RPC_GET_SECRO_OP: secro
 * disable" is present in vmlinux.strings:37975.  That answer is reproduced
 * here exactly.
 */
static int ccci_rpc_get_secro(int md_id, const struct ccci_rpc_msg *req,
			      struct ccci_rpc_msg *rsp)
{
	if (req->arg_count != 1 || !req->args[0].buf ||
	    req->args[0].len < sizeof(u32)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}

	ccci_rpc_rsp_u32(rsp, 0, FS_NO_FEATURE);
	ccci_rpc_rsp_u32(rsp, 1, 0);
	return 0;
}

/* IPC_RPC_DSP_EMI_MPU_SETTING (1797-1826) */
static int ccci_rpc_dsp_mpu(int md_id, const struct ccci_rpc_msg *req,
			    struct ccci_rpc_msg *rsp)
{
	struct ccci_rpc_dsp_emi_mpu_input in, out;
	int ret;

	if (req->arg_count != 1 || !req->args[0].buf ||
	    req->args[0].len < sizeof(in)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}
	memcpy(&in, req->args[0].buf, sizeof(in));

	/* cross-leaf symbol, implemented by the MPU leaf (ccci_mpu.c) */
	ret = ccci_md_set_dsp_mpu_protection(md_id);
	out.request = 0;
	ccci_rpc_rsp_u32(rsp, 0, ret ? FS_FUNC_FAIL : FS_NO_ERROR);
	ccci_rpc_rsp_data(rsp, 1, &out, sizeof(out));
	return 0;
}

/* IPC_RPC_CCCI_QUEUE_MAPPING (1858-1887) */
static int ccci_rpc_queue_mapping(int md_id,
				  const struct ccci_rpc_msg *req,
				  struct ccci_rpc_msg *rsp)
{
	struct ccci_rpc_queue_mapping map;
	struct ccci_md *md;

	if (req->arg_count != 1 || !req->args[0].buf ||
	    req->args[0].len < sizeof(map)) {
		ccci_rpc_rsp_u32(rsp, 0, FS_PARAM_ERROR);
		ccci_rpc_rsp_u32(rsp, 1, FS_PARAM_ERROR);
		return 0;
	}
	memcpy(&map, req->args[0].buf, sizeof(map));

	md = ccci_md_get(md_id);
	if (md)
		ccci_rpc_remap_ch_queue(md, &map);

	/* downstream ignores the remap result and answers 0/0 */
	ccci_rpc_rsp_u32(rsp, 0, 0);
	ccci_rpc_rsp_u32(rsp, 1, 0);
	return 0;
}

/* IPC_RPC_IT_OP (1889-1912) */
static int ccci_rpc_it(int md_id, const struct ccci_rpc_msg *req,
		       struct ccci_rpc_msg *rsp)
{
	ccci_rpc_rsp_u32(rsp, 0, 1);
	ccci_rpc_rsp_u32(rsp, 1, 0xA5A5);
	return 0;
}

/*
 * IPC_RPC_USIM2NFC_OP (0x400a) is deliberately absent: it only exists under
 * FEATURE_INFORM_NFC_VSIM_CHANGE (port_kernel.c:1832), which this build does
 * not define, so the downstream switch falls through to the default case and
 * answers FS_NO_OP with one argument (port_kernel.c:1900-1907).
 */
static const struct {
	u32 op_id;
	const char *name;
	ccci_rpc_handler_t handler;
} ccci_rpc_builtins[] = {
	{ IPC_RPC_CPSVC_SECURE_ALGO_OP, "secure-algo", ccci_rpc_secure_algo },
	{ IPC_RPC_GET_SECRO_OP, "get-secro", ccci_rpc_get_secro },
	{ IPC_RPC_GET_TDD_EINT_NUM_OP, "tdd-eint-num", ccci_rpc_get_num },
	{ IPC_RPC_GET_GPIO_NUM_OP, "gpio-num", ccci_rpc_get_num },
	{ IPC_RPC_GET_ADC_NUM_OP, "adc-num", ccci_rpc_get_num },
	{ IPC_RPC_GET_EMI_CLK_TYPE_OP, "emi-clk-type",
	  ccci_rpc_get_emi_clk_type },
	{ IPC_RPC_GET_EINT_ATTR_OP, "eint-attr", ccci_rpc_get_eint_attr },
	{ IPC_RPC_GET_GPIO_VAL_OP, "gpio-val", ccci_rpc_get_value },
	{ IPC_RPC_GET_ADC_VAL_OP, "adc-val", ccci_rpc_get_value },
	{ IPC_RPC_GET_RF_CLK_BUF_OP, "rf-clk-buf", ccci_rpc_get_rf_clk_buf },
	{ IPC_RPC_GET_GPIO_ADC_OP, "gpio-adc", ccci_rpc_get_gpio_adc },
	{ IPC_RPC_DSP_EMI_MPU_SETTING, "dsp-emi-mpu", ccci_rpc_dsp_mpu },
	{ IPC_RPC_CCCI_QUEUE_MAPPING, "queue-mapping",
	  ccci_rpc_queue_mapping },
	{ IPC_RPC_IT_OP, "it", ccci_rpc_it },
};

/* ------------------------------------------------------------------ */
/* request/response framing: downstream rpc_msg_handler() 1927-2008    */
/* ------------------------------------------------------------------ */
static void ccci_rpc_reply_error(struct ccci_rpc_msg *rsp, int code)
{
	rsp->arg_count = 0;
	ccci_rpc_rsp_u32(rsp, 0, code);
}

/*
 * Build the response in a fresh skb: the request header is copied (op id,
 * channel map, reserved request index are echoed), op_id gets the
 * RPC_API_RESP_ID marker, channel becomes CCCI_RPC_TX and data[1] carries the
 * new packet length, exactly as rpc_msg_handler() rewrites the request skb
 * (port_kernel.c:1962-1995).
 */
static int ccci_rpc_send(struct ccci_port *port, struct sk_buff *req_skb,
			 const struct ccci_rpc_msg *rsp)
{
	struct ccci_md *md = port->md;
	struct ccci_header *req_h = (struct ccci_header *)req_skb->data;
	struct ccci_header *h;
	struct sk_buff *skb;
	unsigned int i, payload = 2 * sizeof(u32), total;
	u32 op_id, count, len;
	unsigned int retries;
	u8 *ptr;
	int ret;

	for (i = 0; i < rsp->arg_count; i++)
		payload += sizeof(u32) + ALIGN(rsp->args[i].len, 4);
	if (payload > RPC_MAX_BUF_SIZE)
		return -EMSGSIZE;

	total = CCCI_HEADER_SIZE + payload;
	skb = ccci_alloc_skb(total, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	h = skb_put(skb, CCCI_HEADER_SIZE);
	memcpy(h, req_h, CCCI_HEADER_SIZE);
	h->channel = CCCI_RPC_TX;
	h->data[1] = total;

	ptr = skb_put(skb, payload);
	/* downstream ORs the marker into the request op id (port_kernel.c:1964) */
	op_id = ((struct rpc_buffer *)req_skb->data)->op_id | RPC_API_RESP_ID;
	count = rsp->arg_count;
	memcpy(ptr, &op_id, sizeof(op_id));
	ptr += sizeof(op_id);
	memcpy(ptr, &count, sizeof(count));
	ptr += sizeof(count);

	for (i = 0; i < rsp->arg_count; i++) {
		len = rsp->args[i].len;
		memcpy(ptr, &len, sizeof(len));
		ptr += sizeof(len);
		if (len) {
			memcpy(ptr, rsp->args[i].buf, len);
			memset(ptr + len, 0, ALIGN(len, 4) - len);
		}
		ptr += ALIGN(len, 4);
	}

	port->tx_pkts++;
	ret = ccci_md_send_skb(md, ccci_port_txq(port), skb);
	if (ret == -EBUSY) {
		/*
		 * Downstream sends the response as a blocking request
		 * (rpc_msg_handler() sets req->blocking = 1,
		 * port_kernel.c:1998).  The kernel port thread may sleep, so
		 * wait for room exactly as the char port does
		 * (ccci_port_char.c:256-267).
		 */
		for (retries = 0; ret == -EBUSY && retries < 1000; retries++) {
			usleep_range(1000, 2000);
			ret = ccci_md_send_skb(md, ccci_port_txq(port), skb);
		}
	}
	if (ret) {
		port->tx_pkts--;
		dev_kfree_skb_any(skb);
	}
	return ret;
}

void ccci_rpc_handle(struct ccci_port *port, struct sk_buff *skb)
{
	struct ccci_md *md;
	struct rpc_buffer *rpc;
	struct rpc_pkt pkt[RPC_MAX_ARG_NUM];
	struct ccci_rpc_msg req, rsp;
	struct ccci_rpc_service *svc;
	unsigned int i, para_num, payload;
	u32 op_id, len;
	u8 *ptr, *end, *arena;
	int ret;

	if (!skb)
		return;
	if (!port || !port->md) {
		dev_kfree_skb_any(skb);
		return;
	}
	md = port->md;

	if (skb->len < sizeof(struct rpc_buffer)) {
		dev_kfree_skb_any(skb);
		return;
	}

	rpc = (struct rpc_buffer *)skb->data;
	if (rpc->header.channel != CCCI_RPC_RX) {
		ccci_md_err(md, "RPC packet on channel %u\n",
			    rpc->header.channel);
		dev_kfree_skb_any(skb);
		return;
	}

	op_id = rpc->op_id;
	para_num = rpc->para_num;
	/* downstream sanity check (port_kernel.c:1942-1948) */
	if (rpc->header.reserved > RPC_REQ_BUFFER_NUM ||
	    para_num > RPC_MAX_ARG_NUM) {
		ccci_md_err(md, "invalid RPC index %u/%u\n",
			    rpc->header.reserved, para_num);
		dev_kfree_skb_any(skb);
		return;
	}

	ptr = rpc->buffer;
	end = skb->data + skb->len;
	for (i = 0; i < para_num; i++) {
		if ((size_t)(end - ptr) < sizeof(u32))
			goto malformed;
		memcpy(&len, ptr, sizeof(len));
		ptr += sizeof(len);
		if (len > RPC_MAX_BUF_SIZE || (u32)(end - ptr) < len)
			goto malformed;
		pkt[i].len = len;
		pkt[i].buf = ptr;
		ptr += ALIGN(len, 4);
		if (ptr > end)
			goto malformed;
	}
	if (ptr - rpc->buffer > RPC_MAX_BUF_SIZE)
		goto malformed;

	arena = kmalloc(CCCI_RPC_ARENA_SIZE, GFP_KERNEL);
	if (!arena) {
		ccci_md_err(md, "RPC arena allocation failed\n");
		dev_kfree_skb_any(skb);
		return;
	}

	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.op_id = op_id;
	req.arg_count = para_num;
	for (i = 0; i < para_num; i++) {
		req.args[i].buf = pkt[i].buf;
		req.args[i].len = pkt[i].len;
	}
	rsp.op_id = op_id;
	for (i = 0; i < CCCI_RPC_MAX_ARGS; i++)
		rsp.args[i].buf = arena + i * CCCI_RPC_ARG_SLOT;

	svc = ccci_rpc_lookup(op_id);
	if (svc) {
		ret = svc->handler(md->id, &req, &rsp);
		ccci_rpc_put(svc);
		if (ret < 0)
			ccci_rpc_reply_error(&rsp, ccci_rpc_errno_to_fs(ret));
	} else {
		ccci_md_dbg(md, "unknown RPC op 0x%08x\n", op_id);
		ccci_rpc_reply_error(&rsp, FS_NO_OP);
	}

	if (rsp.arg_count > RPC_MAX_ARG_NUM) {
		ccci_md_err(md, "invalid RPC response for 0x%08x\n", op_id);
		ccci_rpc_reply_error(&rsp, FS_ERROR_RESERVED);
	}

	payload = 2 * sizeof(u32);
	for (i = 0; i < rsp.arg_count; i++) {
		if (rsp.args[i].len > RPC_MAX_BUF_SIZE ||
		    (rsp.args[i].len && !rsp.args[i].buf))
			break;
		payload += sizeof(u32) + ALIGN(rsp.args[i].len, 4);
	}
	if (i != rsp.arg_count || payload > RPC_MAX_BUF_SIZE) {
		ccci_md_err(md, "RPC response 0x%08x overflow\n", op_id);
		ccci_rpc_reply_error(&rsp, FS_MEM_OVERFLOW);
	}

	ret = ccci_rpc_send(port, skb, &rsp);
	if (ret)
		ccci_md_err(md, "RPC response 0x%08x send failed %d\n",
			    op_id, ret);

	kfree(arena);
	dev_kfree_skb_any(skb);
	return;

malformed:
	ccci_md_err(md, "malformed RPC request 0x%08x\n", op_id);
	dev_kfree_skb_any(skb);
}

/* ------------------------------------------------------------------ */
/* init/exit                                                           */
/* ------------------------------------------------------------------ */
int ccci_rpc_init(void)
{
	unsigned int i;
	int ret;

	/* the largest built-in response is the 10-pin GPIO/ADC block */
	BUILD_BUG_ON(sizeof(struct ccci_rpc_gpio_adc_output_v2) >
		     CCCI_RPC_ARG_SLOT);

	mutex_lock(&ccci_rpc_lock);
	if (ccci_rpc_ready) {
		mutex_unlock(&ccci_rpc_lock);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(ccci_rpc_builtins); i++) {
		ret = __ccci_rpc_register(ccci_rpc_builtins[i].op_id,
					  ccci_rpc_builtins[i].name,
					  ccci_rpc_builtins[i].handler);
		if (ret)
			goto err;
	}
	ccci_rpc_ready = true;
	mutex_unlock(&ccci_rpc_lock);

	/* downstream ccci_subsys_kernel_init() (port_kernel.c:2255-2259) */
	ccci_rpc_eint_load();
	return 0;

err:
	while (!list_empty(&ccci_rpc_services)) {
		struct ccci_rpc_service *svc;

		svc = list_first_entry(&ccci_rpc_services,
				       struct ccci_rpc_service, node);
		list_del(&svc->node);
		kfree(svc->name);
		kfree(svc);
	}
	mutex_unlock(&ccci_rpc_lock);
	return ret;
}

void ccci_rpc_exit(void)
{
	struct ccci_rpc_service *svc, *tmp;

	mutex_lock(&ccci_rpc_lock);
	if (!ccci_rpc_ready) {
		mutex_unlock(&ccci_rpc_lock);
		return;
	}
	ccci_rpc_ready = false;
	list_for_each_entry_safe(svc, tmp, &ccci_rpc_services, node) {
		list_del(&svc->node);
		ccci_rpc_put(svc);
	}
	mutex_unlock(&ccci_rpc_lock);
}
