// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung/MTK IMX219 module EEPROM (camera calibration data) provider
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/imgsensor/src/mt6735/imx219_mipi_raw/imx219_eeprom.c
 *   drivers/misc/mediatek/cam_cal/src/mt6735/imx219_eeprom/imx219_eeprom.c
 *   drivers/misc/mediatek/cam_cal/inc/cam_cal.h
 * (Copyright (C) 2015 MediaTek Inc., GPL-2.0)
 *
 * Downstream exposes the calibration data in two ways:
 *   - a character device ("CAM_CAL_DRV") with the CAM_CALIOC_G_READ ioctl
 *     (cam_cal.h:40) which copies pre-parsed bytes to userspace, and
 *   - Samsung LED class attributes in drivers/leds/leds_s2mu005.c
 *     (rear_camfw/rear_afcal/rear_moduleid, lines 730-1080) backed by
 *     get_imx219_mfr_info(), get_imx219_af_macro_cal(),
 *     get_imx219_af_pan_cal() and get_imx219_moduleid().
 *
 * This driver keeps the data, drops the legacy ABI: the EEPROM is exported as
 * a read-only nvmem device (consumers/cells resolve through the DT layout)
 * plus sysfs attributes carrying the same fields the downstream getters
 * returned.  The CAM_CALIOC_G_READ -> nvmem mapping is documented in
 * $M/logs/camera-lens-cal.md.
 *
 * Layout, all offsets are EEPROM byte offsets:
 *   0x0020..0x002a  manufacturer information string, 11 bytes
 *                   (imx219_eeprom.c:413-416 get_imx219_mfr_info)
 *   0x0050..0x0059  Samsung module ID, 10 bytes
 *                   (imx219_eeprom.c:427-431, CONFIG_GET_REAR_MODULE_ID=y)
 *   0x010c..0x010f  AF macro calibration, little endian u32
 *                   (imx219_eeprom.c:366, 418-421)
 *   0x0110..0x0113  AF pan/infinity calibration, little endian u32
 *                   (imx219_eeprom.c:367, 422-426)
 *   0x0300..0x0301  LSC block length (little endian u16), payload follows
 *                   (imx219_eeprom.c:324-332)
 *
 * The legacy "mtk format" block read by read_imx219_eeprom_mtk_fmt() uses a
 * different slave address (0xa0..0xa6, i.e. the 24C16 page aliases) and is
 * only invoked by the mt6735m sensor variant
 * (imgsensor/src/mt6735m/imx219_mipi_raw/imx219mipiraw_Sensor.c:1238); it is
 * not registered here.
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/slab.h>
#include <linux/string.h>

/*
 * Downstream read address for the sysfs block: IMX219_EEPROM_DEVICE_ID is
 * 0xb0 (imx219_eeprom.h:40) and iReadRegI2C() stores "i2cId >> 1" into the
 * i2c client (kd_sensorlist.c:454-462), so the 7-bit address is 0x58.  The
 * DT node must use reg = <0x58>.
 *
 * read_cmos_sensor() always sends two address bytes, high byte first
 * (imx219_eeprom.c:151-157), i.e. 16-bit register addressing.  Confirmed in
 * the shipped ROM: read_imx219_eeprom() at 0xc04f4598 sends a 2-byte buffer
 * with a_sizeSendData = 2.  Hence no SMBus byte/word register accessor can be
 * used for the address phase; a raw 2-byte write plus a 1-byte read is
 * required.
 */
#define IMX219_EEPROM_ADDR_LEN		2

#define IMX219_EEPROM_MFR_INFO_OFFSET	0x0020
#define IMX219_EEPROM_MFR_INFO_LEN	11
#define IMX219_EEPROM_MODULE_ID_OFFSET	0x0050
#define IMX219_EEPROM_MODULE_ID_LEN	10
#define IMX219_EEPROM_AF_MACRO_OFFSET	0x010c
#define IMX219_EEPROM_AF_PAN_OFFSET	0x0110
#define IMX219_EEPROM_AF_CAL_LEN	4

/*
 * Downstream declares no total device size.  The highest fixed offset it
 * reads is the LSC length prefix at 0x300 (imx219_eeprom.c:324-332); 0x400
 * keeps every metadata field and the LSC header addressable without claiming
 * a part size.  [INFERENCE]
 */
#define IMX219_EEPROM_SIZE		0x400

/* Downstream sentinel: the calibration u32s start at -1 (imx219_eeprom.c:362). */
#define IMX219_EEPROM_CAL_INVALID	0xffffffff

/**
 * struct imx219_eeprom - module EEPROM state
 * @client: i2c client of the EEPROM
 * @lock: serialises the byte accesses issued on the shared adapter
 * @nvmem: registered nvmem device
 */
struct imx219_eeprom {
	struct i2c_client *client;
	struct mutex lock;
	struct nvmem_device *nvmem;
};

/*
 * Read one byte at @offset.  This is the same transfer the downstream helper
 * performs: address write (2 bytes, big endian) followed by a 1-byte read
 * (imx219_eeprom.c:151-157), expressed here as a single i2c_transfer so that
 * the address phase uses a repeated start.
 */
static int imx219_eeprom_read_byte(struct imx219_eeprom *eeprom, u16 offset,
				   u8 *val)
{
	struct i2c_client *client = eeprom->client;
	u8 addr[IMX219_EEPROM_ADDR_LEN] = { offset >> 8, offset & 0xff };
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(addr),
			.buf = addr,
		}, {
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = val,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static int imx219_eeprom_nvmem_read(void *priv, unsigned int offset,
				    void *val, size_t bytes)
{
	struct imx219_eeprom *eeprom = priv;
	u8 *buf = val;
	size_t i;
	int ret;

	if (offset > IMX219_EEPROM_SIZE || bytes > IMX219_EEPROM_SIZE - offset)
		return -EINVAL;

	mutex_lock(&eeprom->lock);
	for (i = 0; i < bytes; i++) {
		ret = imx219_eeprom_read_byte(eeprom, offset + i, &buf[i]);
		if (ret)
			goto out;
	}
	ret = 0;

out:
	mutex_unlock(&eeprom->lock);

	return ret;
}

static int imx219_eeprom_read_block(struct imx219_eeprom *eeprom,
				    u16 offset, void *buf, size_t len)
{
	return imx219_eeprom_nvmem_read(eeprom, offset, buf, len);
}

/*
 * Unprogrammed modules read back all-0x00 or all-0xff.  Downstream did not
 * check this and happily printed the raw bytes; returning -ENODATA keeps
 * erased data from being presented as calibration.  [INFERENCE]
 */
static bool imx219_eeprom_block_is_blank(const u8 *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] != 0x00 && buf[i] != 0xff)
			return false;

	return true;
}

/*
 * The downstream readers fill a u32 byte by byte from increasing offsets
 * (imx219_eeprom.c:370-380) on a little endian CPU, hence little endian.
 */
static int imx219_eeprom_read_cal(struct imx219_eeprom *eeprom, u16 offset,
				  u32 *val)
{
	u8 raw[IMX219_EEPROM_AF_CAL_LEN];
	int ret;

	ret = imx219_eeprom_read_block(eeprom, offset, raw, sizeof(raw));
	if (ret)
		return ret;

	*val = (u32)raw[0] | ((u32)raw[1] << 8) |
	       ((u32)raw[2] << 16) | ((u32)raw[3] << 24);

	return 0;
}

static ssize_t mfr_info_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct imx219_eeprom *eeprom = dev_get_drvdata(dev);
	char info[IMX219_EEPROM_MFR_INFO_LEN + 1] = { 0 };
	int ret;

	ret = imx219_eeprom_read_block(eeprom, IMX219_EEPROM_MFR_INFO_OFFSET,
				       info, IMX219_EEPROM_MFR_INFO_LEN);
	if (ret)
		return ret;

	if (imx219_eeprom_block_is_blank(info, IMX219_EEPROM_MFR_INFO_LEN))
		return -ENODATA;

	return sysfs_emit(buf, "%s\n", info);
}

static ssize_t module_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct imx219_eeprom *eeprom = dev_get_drvdata(dev);
	u8 id[IMX219_EEPROM_MODULE_ID_LEN];
	int ret;

	ret = imx219_eeprom_read_block(eeprom, IMX219_EEPROM_MODULE_ID_OFFSET,
				       id, sizeof(id));
	if (ret)
		return ret;

	if (imx219_eeprom_block_is_blank(id, sizeof(id)))
		return -ENODATA;

	/* Same formatting as leds_s2mu005.c:1042-1057. */
	return sysfs_emit(buf, "%c%c%c%c%c%02X%02X%02X%02X%02X\n",
			  id[0], id[1], id[2], id[3], id[4],
			  id[5], id[6], id[7], id[8], id[9]);
}

static ssize_t af_macro_cal_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct imx219_eeprom *eeprom = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = imx219_eeprom_read_cal(eeprom, IMX219_EEPROM_AF_MACRO_OFFSET,
				     &val);
	if (ret)
		return ret;
	if (val == IMX219_EEPROM_CAL_INVALID)
		return -ENODATA;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t af_pan_cal_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct imx219_eeprom *eeprom = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = imx219_eeprom_read_cal(eeprom, IMX219_EEPROM_AF_PAN_OFFSET, &val);
	if (ret)
		return ret;
	if (val == IMX219_EEPROM_CAL_INVALID)
		return -ENODATA;

	return sysfs_emit(buf, "%u\n", val);
}

/*
 * Combined AF calibration, byte-for-byte compatible with the downstream
 * "rear_afcal" attribute (leds_s2mu005.c:820-827): "1 <macro> <pan>".
 */
static ssize_t afcal_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct imx219_eeprom *eeprom = dev_get_drvdata(dev);
	u32 macro, pan;
	int ret;

	ret = imx219_eeprom_read_cal(eeprom, IMX219_EEPROM_AF_MACRO_OFFSET,
				     &macro);
	if (ret)
		return ret;
	ret = imx219_eeprom_read_cal(eeprom, IMX219_EEPROM_AF_PAN_OFFSET, &pan);
	if (ret)
		return ret;
	if (macro == IMX219_EEPROM_CAL_INVALID || pan == IMX219_EEPROM_CAL_INVALID)
		return -ENODATA;

	return sysfs_emit(buf, "1 %u %u\n", macro, pan);
}

static DEVICE_ATTR_RO(mfr_info);
static DEVICE_ATTR_RO(module_id);
static DEVICE_ATTR_RO(af_macro_cal);
static DEVICE_ATTR_RO(af_pan_cal);
static DEVICE_ATTR_RO(afcal);

static struct attribute *imx219_eeprom_attrs[] = {
	&dev_attr_mfr_info.attr,
	&dev_attr_module_id.attr,
	&dev_attr_af_macro_cal.attr,
	&dev_attr_af_pan_cal.attr,
	&dev_attr_afcal.attr,
	NULL
};
ATTRIBUTE_GROUPS(imx219_eeprom);

static int imx219_eeprom_probe(struct i2c_client *client)
{
	struct imx219_eeprom *eeprom;
	struct nvmem_config config = {
		.name = "imx219-eeprom",
		.id = NVMEM_DEVID_NONE,
		.type = NVMEM_TYPE_EEPROM,
		.read_only = true,
		.size = IMX219_EEPROM_SIZE,
		.word_size = 1,
		.stride = 1,
	};
	int ret;

	eeprom = devm_kzalloc(&client->dev, sizeof(*eeprom), GFP_KERNEL);
	if (!eeprom)
		return -ENOMEM;

	mutex_init(&eeprom->lock);
	eeprom->client = client;
	dev_set_drvdata(&client->dev, eeprom);

	config.dev = &client->dev;
	config.of_node = client->dev.of_node;
	config.priv = eeprom;
	config.reg_read = imx219_eeprom_nvmem_read;

	/*
	 * No register access here: the EEPROM shares the module rails with the
	 * sensor (vcamio/vcama/vcamaf, brought up by the sensor power
	 * sequence), so it is not readable before the camera is powered.
	 * Downstream behaves the same way: imx219_update_sysfs_information()
	 * runs from imx219_open() after the module is powered
	 * (imx219mipiraw_Sensor.c:1202).
	 */
	eeprom->nvmem = devm_nvmem_register(&client->dev, &config);
	if (IS_ERR(eeprom->nvmem)) {
		dev_err(&client->dev, "failed to register nvmem device\n");
		return PTR_ERR(eeprom->nvmem);
	}

	ret = device_add_groups(&client->dev, imx219_eeprom_groups);
	if (ret) {
		dev_err(&client->dev, "failed to add sysfs attributes\n");
		return ret;
	}

	return 0;
}

static void imx219_eeprom_remove(struct i2c_client *client)
{
	device_remove_groups(&client->dev, imx219_eeprom_groups);
}

static const struct of_device_id imx219_eeprom_of_table[] = {
	{ .compatible = "samsung,imx219-eeprom" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx219_eeprom_of_table);

static const struct i2c_device_id imx219_eeprom_id_table[] = {
	{ "imx219-eeprom", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, imx219_eeprom_id_table);

static struct i2c_driver imx219_eeprom_driver = {
	.driver = {
		.name = "imx219-eeprom",
		.of_match_table = imx219_eeprom_of_table,
	},
	.probe = imx219_eeprom_probe,
	.remove = imx219_eeprom_remove,
	.id_table = imx219_eeprom_id_table,
};

module_i2c_driver(imx219_eeprom_driver);

MODULE_DESCRIPTION("Samsung/MTK IMX219 module EEPROM calibration provider");
MODULE_LICENSE("GPL");
