// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CCCI MD image check-header parsing and validation.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c
 *     - check_md_header_v3()      (line 102)
 *     - md_check_header_parser()  (line 285)
 *     - md_check_header()         (line 508)
 *     - ccci_get_md_info_str()    (line 646)
 *     - ccci_get_md_check_hdr_inf (line 1059)
 *
 * Only the image-header part of the downstream loader is reproduced here.
 * Downstream also opened the modem image through block-device file I/O and
 * verified it with the secure boot library; on this tree the firmware blob is
 * supplied by the CCCI core through request_firmware() and secure-image
 * authentication is a separate hook, so those paths are not duplicated.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/ccci.h>

#include "ccci_util.h"

/* Downstream type_str[], ccci_util_lib_load_img.c:54-75. */
static const char * const md_type_str[] = {
	[0]  = "invalid",
	[1]  = "2g",
	[2]  = "3g",
	[3]  = "wg",
	[4]  = "tg",
	[5]  = "lwg",
	[6]  = "ltg",
	[7]  = "sglte",
	[8]  = "ultg",
	[9]  = "ulwg",
	[10] = "ulwtg",
	[11] = "ulwcg",
	[12] = "ulwctg",
	[13] = "ulttg",
	[14] = "ulfwg",
	[15] = "ulfwcg",
	[16] = "ulctg",
	[17] = "ultctg",
	[18] = "ultwg",
	[19] = "ultwcg",
	[20] = "ulftg",
	[21] = "ulfctg",
};

static_assert(ARRAY_SIZE(md_type_str) == CCCI_MD_MAX_IMG_TYPE + 1,
	       "md_type_str must cover MAX_IMG_NUM");

/*
 * Field offsets of the common check-header prefix.  They are identical in
 * v3, v4 and v5 because v4 only reuses v3's reserved2[] area and v5 only
 * appends fields after domain_attr[] (ccci_util_lib_main.h:16-96).
 */
#define MD_HDR_OFF_VERNO	12
#define MD_HDR_OFF_PRODUCT_VER	16
#define MD_HDR_OFF_IMAGE_TYPE	20
#define MD_HDR_OFF_PLATFORM	24
#define MD_HDR_OFF_BUILD_TIME	40
#define MD_HDR_OFF_BUILD_VER	104
#define MD_HDR_OFF_BIND_SYS_ID	168
#define MD_HDR_OFF_EXT_ATTR	169
#define MD_HDR_OFF_MEM_SIZE	172
#define MD_HDR_OFF_MD_IMG_SIZE	176
#define MD_HDR_OFF_RPC_SEC	180
#define MD_HDR_OFF_DSP_OFFSET	184
#define MD_HDR_OFF_DSP_SIZE	188
#define MD_HDR_OFF_REGION_NUM	192
#define MD_HDR_OFF_REGION_INFO	196
#define MD_HDR_OFF_DOMAIN_ATTR	260
#define MD_HDR_OFF_ARM7_OFFSET	276
#define MD_HDR_OFF_ARM7_SIZE	280

/* DSP not loaded into EMI, ccci_util_lib_load_img.c:243. */
#define MD_DSP_ON_EMI_DISABLED	0xCDCDCDAA

static_assert(sizeof(struct md_check_header_v3) == 284, "v3 layout");
static_assert(sizeof(struct md_check_header_v4) == 284, "v4 layout");
static_assert(sizeof(struct md_check_header_v5) == 344, "v5 layout");

static char md_img_info_str[CCCI_MAX_MD_NUM][MD_INFO_STR_LEN];
static DEFINE_MUTEX(md_img_info_lock);

const char *ccci_util_md_type_str(unsigned int image_type)
{
	if (image_type >= ARRAY_SIZE(md_type_str))
		return "invalid";

	return md_type_str[image_type];
}
EXPORT_SYMBOL_GPL(ccci_util_md_type_str);

/* Downstream product_str[], ccci_util_lib_load_img.c:84-88. */
const char *ccci_util_md_product_str(unsigned int product_ver)
{
	switch (product_ver) {
	case 1:
		return MD_DEBUG_STR;
	case 2:
		return MD_RELEASE_STR;
	default:
		return MD_INVALID_STR;
	}
}
EXPORT_SYMBOL_GPL(ccci_util_md_product_str);

static void md_hdr_copy_str(char *dst, size_t dst_size, const u8 *src,
			    size_t src_size)
{
	size_t len = strnlen((const char *)src, src_size);

	len = min(len, dst_size - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void md_hdr_build_info_str(int md_id, const char *ap_platform,
				  unsigned int ap_mem_size,
				  const struct ccci_util_md_img_info *info)
{
	char *str = md_img_info_str[md_id];

	snprintf(str, MD_INFO_STR_LEN,
		 "MD:%s*%s*%s*%s*%s\nAP:%s*%s*%08x (MD)%08x\n",
		 info->image_type_str, info->platform, info->build_ver,
		 info->build_time, info->product_ver_str,
		 info->image_type_str, ap_platform ?: "unknown",
		 ap_mem_size, info->mem_size);
}

char *ccci_get_md_info_str(int md_id)
{
	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM)
		return NULL;

	return md_img_info_str[md_id];
}
EXPORT_SYMBOL_GPL(ccci_get_md_info_str);

/**
 * ccci_util_md_check_header() - parse and validate an MD image check header
 * @md_id:	modem index (0 == MD_SYS1); bind_sys_id must equal md_id + 1
 * @hdr:	start of the check header ("CHECK_HEADER")
 * @hdr_len:	bytes available at @hdr
 * @md_mem_size: AP reserved MD memory size, 0 if unknown (downstream md_size)
 * @ap_img_size: AP reserved image size, 0 if unknown (downstream image->size)
 * @ap_platform: AP platform string for the info line, may be NULL
 * @info:	decoded header
 *
 * Mirrors check_md_header_v3()/md_check_header_parser() from the downstream
 * loader.  Returns 0 on success, -EOPNOTSUPP for header versions this driver
 * does not implement (v1/v2 and v6), -EINVAL for malformed input, or the
 * downstream CCCI_ERR_LOAD_IMG_* error codes.
 */
int ccci_util_md_check_header(int md_id, const void *hdr, size_t hdr_len,
			      unsigned int md_mem_size,
			      unsigned int ap_img_size,
			      const char *ap_platform,
			      struct ccci_util_md_img_info *info)
{
	const u8 *p = hdr;
	unsigned int verno, layout_size, size_field;
	unsigned int image_type, product_ver, mem_size, md_img_size;
	unsigned int dsp_offset, dsp_size;
	unsigned int i;
	int ret = 0;

	if (!hdr || !info)
		return -EINVAL;
	if (md_id < 0 || md_id >= CCCI_MAX_MD_NUM)
		return -EINVAL;
	if (hdr_len < MD_HDR_OFF_VERNO + sizeof(u32))
		return -EINVAL;

	memset(info, 0, sizeof(*info));

	if (memcmp(p, MD_HEADER_MAGIC_NO, MD_HEADER_MAGIC_LEN)) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md check header not exist!\n");
		return -CCCI_ERR_LOAD_IMG_CHECK_HEAD;
	}

	verno = get_unaligned_le32(p + MD_HDR_OFF_VERNO);
	switch (verno) {
	case 3:
	case 4:
		layout_size = sizeof(struct md_check_header_v4);
		break;
	case 5:
		layout_size = sizeof(struct md_check_header_v5);
		break;
	default:
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md check header version %u not supported\n",
				     verno);
		return -EOPNOTSUPP;
	}

	if (hdr_len < layout_size) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md check header truncated: %zu < %u\n",
				     hdr_len, layout_size);
		return -EINVAL;
	}

	/* The last word of the header is its own size (downstream dispatch). */
	size_field = get_unaligned_le32(p + layout_size - sizeof(u32));
	if (size_field != layout_size) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md check header size mismatch: field %u, layout %u\n",
				     size_field, layout_size);
		return -CCCI_ERR_LOAD_IMG_CHECK_HEAD;
	}

	if (p[MD_HDR_OFF_BIND_SYS_ID] != (u8)(md_id + 1)) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md image is for MD SYS%u, not MD SYS%d!\n",
				     p[MD_HDR_OFF_BIND_SYS_ID], md_id + 1);
		return -CCCI_ERR_LOAD_IMG_MD_CHECK;
	}

	image_type = get_unaligned_le32(p + MD_HDR_OFF_IMAGE_TYPE);
	if (image_type > CCCI_MD_MAX_IMG_TYPE) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md image type %u out of range\n", image_type);
		return -CCCI_ERR_LOAD_IMG_MD_CHECK;
	}

	product_ver = get_unaligned_le32(p + MD_HDR_OFF_PRODUCT_VER);
	mem_size = get_unaligned_le32(p + MD_HDR_OFF_MEM_SIZE);
	md_img_size = get_unaligned_le32(p + MD_HDR_OFF_MD_IMG_SIZE);

	/* Downstream ENABLE_MEM_SIZE_CHECK block, ccci_util_lib_load_img.c:143. */
	if (md_mem_size) {
		if (mem_size > md_mem_size) {
			ccci_util_dump_write(md_id, CCCI_DUMP_INIT,
					     CCCI_DUMP_TIME_FLAG,
					     "md mem size 0x%x > AP reserve 0x%x\n",
					     mem_size, md_mem_size);
			return -CCCI_ERR_LOAD_IMG_MD_CHECK;
		}
		if (mem_size < md_mem_size)
			ccci_util_dump_write(md_id, CCCI_DUMP_INIT,
					     CCCI_DUMP_TIME_FLAG,
					     "[Warning]md size in md header isn't sync to DFO setting: (%08x, %08x)\n",
					     mem_size, md_mem_size);
	}

	/* Downstream compares head->md_img_size with the reserved image size. */
	if (ap_img_size && md_img_size && md_img_size > ap_img_size) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "md image size 0x%x > AP reserve 0x%x\n",
				     md_img_size, ap_img_size);
		return -CCCI_ERR_LOAD_IMG_MD_CHECK;
	}

	/* DSP image sanity, ccci_util_lib_load_img.c:225-251. */
	dsp_offset = get_unaligned_le32(p + MD_HDR_OFF_DSP_OFFSET);
	dsp_size = get_unaligned_le32(p + MD_HDR_OFF_DSP_SIZE);
	if (dsp_offset == MD_DSP_ON_EMI_DISABLED) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "DSP on EMI disabled\n");
	} else if (verno == 3 && (dsp_offset & 0xFFFF)) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "DSP image offset not 64KB align\n");
		return -CCCI_ERR_LOAD_IMG_MD_CHECK;
	} else if (md_mem_size &&
		   (u64)dsp_offset + dsp_size > md_mem_size) {
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "DSP image size too large %x\n", md_mem_size);
		return -CCCI_ERR_LOAD_IMG_MD_CHECK;
	}

	/* RMPU region table, only meaningful from check header v4 on. */
	if (verno >= 4) {
		unsigned int region_num =
			get_unaligned_le32(p + MD_HDR_OFF_REGION_NUM);
		unsigned int region_cnt;

		if (region_num > MD_MAX_REGION_NUM) {
			ccci_util_dump_write(md_id, CCCI_DUMP_INIT,
					     CCCI_DUMP_TIME_FLAG,
					     "md region number %u out of range\n",
					     region_num);
			return -CCCI_ERR_LOAD_IMG_MD_CHECK;
		}

		/*
		 * Downstream always consumes MPU_REGION_INFO_ID_LAST + 1
		 * entries regardless of region_num.
		 */
		region_cnt = clamp_t(unsigned int, region_num,
				     MPU_REGION_INFO_ID_LAST + 1,
				     MD_MAX_REGION_NUM);
		info->region_num = region_cnt;

		for (i = 0; i < region_cnt; i++) {
			const u8 *r = p + MD_HDR_OFF_REGION_INFO + i * 8;

			info->region_info[i].region_offset =
				get_unaligned_le32(r);
			info->region_info[i].region_size =
				get_unaligned_le32(r + 4);
			ccci_util_dump_write(md_id, CCCI_DUMP_INIT,
					     CCCI_DUMP_TIME_FLAG,
					     "[CCCI_UTIL] load_image: check_header_v%u, region(%u): size = %x , offset = %x\n",
					     verno, i,
					     info->region_info[i].region_size,
					     info->region_info[i].region_offset);
		}

		for (i = 0; i < MPU_DOMAIN_INFO_ID_TOTAL_NUM; i++) {
			info->domain_attr[i] = get_unaligned_le32(p +
				MD_HDR_OFF_DOMAIN_ATTR + i * 4);
			ccci_util_dump_write(md_id, CCCI_DUMP_INIT,
					     CCCI_DUMP_TIME_FLAG,
					     "[CCCI_UTIL] load_image: check_header_v%u, domain(%u): attr = %x\n",
					     verno, i, info->domain_attr[i]);
		}
	}

	if (verno >= 5) {
		info->arm7_img_offset =
			get_unaligned_le32(p + MD_HDR_OFF_ARM7_OFFSET);
		info->arm7_img_size =
			get_unaligned_le32(p + MD_HDR_OFF_ARM7_SIZE);
		ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
				     "[CCCI_UTIL] load_image: check_header_v5, arm7_offset = 0x%08X, arm_size = 0x%08X\n",
				     info->arm7_img_offset, info->arm7_img_size);
	}

	info->header_size = layout_size;
	info->header_verno = verno;
	info->product_ver = product_ver;
	info->image_type = image_type;
	info->bind_sys_id = p[MD_HDR_OFF_BIND_SYS_ID];
	info->ext_attr = p[MD_HDR_OFF_EXT_ATTR];
	info->mem_size = mem_size;
	info->md_img_size = md_img_size;
	info->rpc_sec_mem_addr = get_unaligned_le32(p + MD_HDR_OFF_RPC_SEC);
	info->dsp_img_offset = dsp_offset;
	info->dsp_img_size = dsp_size;
	info->image_type_str = ccci_util_md_type_str(image_type);
	info->product_ver_str = ccci_util_md_product_str(product_ver);

	md_hdr_copy_str(info->platform, sizeof(info->platform),
			p + MD_HDR_OFF_PLATFORM, 16);
	md_hdr_copy_str(info->build_time, sizeof(info->build_time),
			p + MD_HDR_OFF_BUILD_TIME, 64);
	md_hdr_copy_str(info->build_ver, sizeof(info->build_ver),
			p + MD_HDR_OFF_BUILD_VER, 64);

	mutex_lock(&md_img_info_lock);
	md_hdr_build_info_str(md_id, ap_platform, md_mem_size, info);
	mutex_unlock(&md_img_info_lock);

	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "Modem header check OK! v%u size=%zu\n",
			     verno, hdr_len);
	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "(MD)[type]=%s, (AP)[type]=%s\n",
			     info->image_type_str, info->image_type_str);
	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "(MD)[size]=%x, (AP)[size]=%x\n",
			     mem_size, md_mem_size);
	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "(MD)[img_size]=%x, (AP)[img_size]=%x\n",
			     md_img_size, ap_img_size);
	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "(MD)[build_ver]=%s, [build_time]=%s\n",
			     info->build_ver, info->build_time);
	ccci_util_dump_write(md_id, CCCI_DUMP_INIT, CCCI_DUMP_TIME_FLAG,
			     "(MD)[product_ver]=%s\n", info->product_ver_str);

	return ret;
}
EXPORT_SYMBOL_GPL(ccci_util_md_check_header);

/**
 * ccci_util_md_region_info() - extract the RMPU region/domain table
 * @info:	decoded header from ccci_util_md_check_header()
 * @region_num:	output, number of valid entries in @info->region_info
 * @regions:	output array for the region table (may be NULL)
 * @max_regions: size of @regions
 * @domain_attr: output array for the domain attributes (may be NULL)
 * @max_domains: size of @domain_attr
 *
 * v4 and v5 headers carry the region table; v3 does not.  Mirrors the
 * "RMPU only available after check header v4" loop in
 * ccci_util_lib_load_img.c:254-274.
 */
int ccci_util_md_region_info(const struct ccci_util_md_img_info *info,
			     unsigned int *region_num,
			     struct _md_regin_info *regions,
			     unsigned int max_regions,
			     unsigned int *domain_attr,
			     unsigned int max_domains)
{
	unsigned int num;

	if (!info)
		return -EINVAL;
	if (info->header_verno < 4)
		return -EOPNOTSUPP;

	num = min(info->region_num, (unsigned int)MD_MAX_REGION_NUM);

	if (regions) {
		if (max_regions < num)
			return -EINVAL;
		memcpy(regions, info->region_info, num * sizeof(*regions));
	}
	if (region_num)
		*region_num = num;

	if (domain_attr) {
		if (max_domains < MPU_DOMAIN_INFO_ID_TOTAL_NUM)
			return -EINVAL;
		memcpy(domain_attr, info->domain_attr,
		       MPU_DOMAIN_INFO_ID_TOTAL_NUM * sizeof(*domain_attr));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ccci_util_md_region_info);

MODULE_DESCRIPTION("MediaTek CCCI MD image header helpers");
MODULE_LICENSE("GPL");
