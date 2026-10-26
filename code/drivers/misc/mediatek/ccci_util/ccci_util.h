/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * CCCI utility driver: private interfaces shared between ccci_util.c,
 * ccci_util_dump.c, ccci_util_img.c and ccci_util_log.c.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_main.h
 *   drivers/misc/mediatek/ccci_util/ccci_util_log.h
 *   drivers/misc/mediatek/ccci_util/ccci_private_log.c
 *   drivers/misc/mediatek/include/mt-plat/mt_ccci_common.h
 *
 * The public (core-facing) API lives in <linux/ccci.h>.
 */
#ifndef __CCCI_UTIL_H__
#define __CCCI_UTIL_H__

#include <linux/types.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ccci.h>

/* Downstream CCCI_LOG_BUF_SIZE / CCCI_LOG_MAX_WRITE (ccci_private_log.c:36-37). */
#define CCCI_UTIL_LOG_SIZE		4096	/* must be a power of two */
#define CCCI_UTIL_MAX_WRITE		512

/* Downstream dump buffer sizes, ccci_private_log.c:248-253. */
#define CCCI_UTIL_INIT_SETTING_BUF	(4096 * 2)
#define CCCI_UTIL_BOOT_UP_BUF		(4096 * 4)
#define CCCI_UTIL_NORMAL_BUF		(4096 * 2)
#define CCCI_UTIL_REPEAT_BUF		(4096 * 32)
#define CCCI_UTIL_REG_DUMP_BUF		(4096 * 64)
#define CCCI_UTIL_HISTORY_BUF		64

/* Dump buffer selectors, verbatim downstream mt_ccci_common.h:900-906. */
enum ccci_dump_type {
	CCCI_DUMP_INIT = 0,
	CCCI_DUMP_BOOTUP,
	CCCI_DUMP_NORMAL,
	CCCI_DUMP_REPEAT,
	CCCI_DUMP_MEM_DUMP,
	CCCI_DUMP_HISTORY,
	CCCI_DUMP_MAX,
};

/* Dump flags, verbatim downstream mt_ccci_common.h:896-898. */
#define CCCI_DUMP_TIME_FLAG		(1 << 0)
#define CCCI_DUMP_CLR_BUF_FLAG		(1 << 1)
#define CCCI_DUMP_CURR_FLAG		(1 << 2)

/* Image errors, verbatim downstream mt_ccci_common.h:344-358. */
#define CCCI_ERR_LOAD_IMG_START_ID	400
#define CCCI_ERR_LOAD_IMG_CHECK_HEAD	(CCCI_ERR_LOAD_IMG_START_ID + 8)
#define CCCI_ERR_LOAD_IMG_MD_CHECK	(CCCI_ERR_LOAD_IMG_START_ID + 11)

/* Image header constants, downstream mt_ccci_common.h:89-93. */
#define MD_HEADER_MAGIC_NO		"CHECK_HEADER"
#define MD_HEADER_MAGIC_LEN		12
#define MD_DEBUG_STR			"Debug"
#define MD_RELEASE_STR			"Release"
#define MD_INVALID_STR			"INVALID"

#define IMG_POSTFIX_LEN			16
#define MD_INFO_STR_LEN			256
#define MD_PLATFORM_STR_LEN		16
#define MD_BUILD_TIME_STR_LEN		64
#define MD_BUILD_VER_STR_LEN		64

#define MD_MAX_REGION_NUM		8
#define MD_MAX_DOMAIN_NUM		4
/* Downstream MAX_IMG_NUM == modem_ulfctg == 21 (mt_ccci_common.h:65). */
#define CCCI_MD_MAX_IMG_TYPE		21
/* Downstream mt_ccci_common.h:164, MPU_REGION_INFO_ID_LAST. */
#define MPU_REGION_INFO_ID_LAST		4
/* Downstream mt_ccci_common.h:190, MPU_DOMAIN_INFO_ID_TOTAL_NUM. */
#define MPU_DOMAIN_INFO_ID_TOTAL_NUM	3

/*
 * Downstream mt_ccci_common.h:136-139, referenced by the check header v4/v5
 * region tables. Name and layout kept verbatim.
 */
struct _md_regin_info {
	unsigned int region_offset;
	unsigned int region_size;
};

/* Verbatim downstream ccci_util_lib_main.h:15-37 (check header v3). */
struct md_check_header_v3 {
	unsigned char check_header[12];  /* magic number is "CHECK_HEADER"*/
	unsigned int  header_verno;	  /* header structure version number */
	unsigned int  product_ver;	   /* 0x0:invalid; 0x1:debug version; 0x2:release version */
	unsigned int  image_type;		/* 0x0:invalid; 0x1:2G modem; 0x2: 3G modem */
	unsigned char platform[16];	  /* MT6573_S01 or MT6573_S02 */
	unsigned char build_time[64];	/* build time string */
	unsigned char build_ver[64];	 /* project version, ex:11A_MD.W11.28 */

	unsigned char bind_sys_id;	   /* bind to md sys id, MD SYS1: 1, MD SYS2: 2, MD SYS5: 5 */
	unsigned char ext_attr;		  /* no shrink: 0, shrink: 1 */
	unsigned char reserved[2];	   /* for reserved */

	unsigned int  mem_size;		  /* md ROM/RAM image size requested by md */
	unsigned int  md_img_size;	   /* md image size, exclude head size */
	unsigned int  rpc_sec_mem_addr;  /* RPC secure memory address */

	unsigned int  dsp_img_offset;
	unsigned int  dsp_img_size;
	unsigned char reserved2[88];

	unsigned int  size;			  /* the size of this structure */
} __packed;

/* Verbatim downstream ccci_util_lib_main.h:39-66 (check header v4). */
struct md_check_header_v4 {
	unsigned char check_header[12]; /* magic number is "CHECK_HEADER"*/
	unsigned int header_verno; /* header structure version number */
	unsigned int product_ver; /* 0x0:invalid; 0x1:debug version; 0x2:release version */
	unsigned int image_type; /* 0x0:invalid; 0x1:2G modem; 0x2: 3G modem */
	unsigned char platform[16]; /* MT6573_S01 or MT6573_S02 */
	unsigned char build_time[64]; /* build time string */
	unsigned char build_ver[64]; /* project version, ex:11A_MD.W11.28 */

	unsigned char bind_sys_id; /* bind to md sys id, MD SYS1: 1, MD SYS2: 2, MD SYS5: 5 */
	unsigned char ext_attr; /* no shrink: 0, shrink: 1 */
	unsigned char reserved[2]; /* for reserved */

	unsigned int mem_size; /* md ROM/RAM image size requested by md */
	unsigned int md_img_size; /* md image size, exclude head size */
	unsigned int rpc_sec_mem_addr; /* RPC secure memory address */

	unsigned int dsp_img_offset;
	unsigned int dsp_img_size;

	unsigned int region_num; /* total region number */
	struct _md_regin_info region_info[8]; /* max support 8 regions */
	unsigned int domain_attr[4]; /* max support 4 domain settings, each region has 4 control bits*/

	unsigned char reserved2[4];

	unsigned int size; /* the size of this structure */
} __packed;

/* Verbatim downstream ccci_util_lib_main.h:68-99 (check header v5). */
struct md_check_header_v5 {
	unsigned char check_header[12]; /* magic number is "CHECK_HEADER"*/
	unsigned int  header_verno; /* header structure version number */
	unsigned int  product_ver; /* 0x0:invalid; 0x1:debug version; 0x2:release version */
	unsigned int  image_type; /* 0x0:invalid; 0x1:2G modem; 0x2: 3G modem */
	unsigned char platform[16]; /* MT6573_S01 or MT6573_S02 */
	unsigned char build_time[64]; /* build time string */
	unsigned char build_ver[64]; /* project version, ex:11A_MD.W11.28 */

	unsigned char bind_sys_id; /* bind to md sys id, MD SYS1: 1, MD SYS2: 2, MD SYS5: 5 */
	unsigned char ext_attr; /* no shrink: 0, shrink: 1 */
	unsigned char reserved[2]; /* for reserved */

	unsigned int  mem_size; /* md ROM/RAM image size requested by md */
	unsigned int  md_img_size; /* md image size, exclude head size */
	unsigned int  rpc_sec_mem_addr; /* RPC secure memory address */

	unsigned int  dsp_img_offset;
	unsigned int  dsp_img_size;

	unsigned int  region_num; /* total region number */
	struct _md_regin_info region_info[8]; /* max support 8 regions */
	unsigned int  domain_attr[4]; /* max support 4 domain settings, each region has 4 control bits*/

	unsigned int  arm7_img_offset;
	unsigned int  arm7_img_size;

	unsigned char reserved_1[56];


	unsigned int  size; /* the size of this structure */
} __packed;

/*
 * Decoded MD image check header. Only the fields the AP actually consumes are
 * kept; strings are copied out so the caller can release the firmware blob.
 */
struct ccci_util_md_img_info {
	unsigned int header_size;
	unsigned int header_verno;
	unsigned int product_ver;
	unsigned int image_type;
	char platform[MD_PLATFORM_STR_LEN + 1];
	char build_time[MD_BUILD_TIME_STR_LEN + 1];
	char build_ver[MD_BUILD_VER_STR_LEN + 1];

	u8 bind_sys_id;
	u8 ext_attr;

	unsigned int mem_size;
	unsigned int md_img_size;
	unsigned int rpc_sec_mem_addr;

	unsigned int dsp_img_offset;
	unsigned int dsp_img_size;

	unsigned int region_num;
	struct _md_regin_info region_info[MD_MAX_REGION_NUM];
	unsigned int domain_attr[MD_MAX_DOMAIN_NUM];

	unsigned int arm7_img_offset;
	unsigned int arm7_img_size;

	const char *image_type_str;
	const char *product_ver_str;
};

/* ------------------------------------------------------------------ */
/* ccci_util_img.c                                                     */
/* ------------------------------------------------------------------ */
int ccci_util_md_check_header(int md_id, const void *hdr, size_t hdr_len,
			      unsigned int md_mem_size,
			      unsigned int ap_img_size,
			      const char *ap_platform,
			      struct ccci_util_md_img_info *info);
char *ccci_get_md_info_str(int md_id);
int ccci_util_md_region_info(const struct ccci_util_md_img_info *info,
			     unsigned int *region_num,
			     struct _md_regin_info *regions,
			     unsigned int max_regions,
			     unsigned int *domain_attr,
			     unsigned int max_domains);
const char *ccci_util_md_type_str(unsigned int image_type);
const char *ccci_util_md_product_str(unsigned int product_ver);

/* ------------------------------------------------------------------ */
/* ccci_util_dump.c                                                    */
/* ------------------------------------------------------------------ */
void ccci_util_dump_hex(int md_id, int buf_type, const void *buf, size_t len);
void ccci_util_mem_dump(int md_id, int buf_type, void *start_addr, int len);
void ccci_util_cmpt_mem_dump(int md_id, int buf_type, void *start_addr,
			     int len);
int get_dump_buf_usage(char buf[], int size);
int ccci_util_dump_set_window(unsigned int md_id, phys_addr_t phys,
			      size_t size);
void ccci_util_dump_clear_window(unsigned int md_id);
int ccci_util_dump_debugfs_init(struct dentry *root);
void ccci_util_dump_debugfs_exit(void);

/* ------------------------------------------------------------------ */
/* ccci_util.c: per-MD dump/log rings                                  */
/* ------------------------------------------------------------------ */
int ccci_util_dump_write(int md_id, int buf_type, unsigned int flag,
			 const char *fmt, ...) __printf(4, 5);
int ccci_dump_write(int md_id, int buf_type, unsigned int flag,
		    const char *fmt, ...) __printf(4, 5);
int ccci_util_dump_append_raw(unsigned int md_id, unsigned int buf_type,
			      const void *data, size_t len);
bool ccci_util_dump_ring_ready(unsigned int md_id, unsigned int buf_type);
/* Both return the snapshot length in bytes (or a negative errno). */
int ccci_util_log_snapshot(int md_id, char **out_buf);
int ccci_util_dump_snapshot(int md_id, char **out_buf);
void ccci_util_snapshot_free(char *buf);

/* ------------------------------------------------------------------ */
/* ccci_util_log.c: /dev/ccci_log, /dev/ccci_dump, event log           */
/* ------------------------------------------------------------------ */
int ccci_util_logdev_init(void);
void ccci_util_logdev_exit(void);
void ccci_log_init(void);
int ccci_log_write(const char *fmt, ...) __printf(1, 2);
int ccci_log_write_raw(unsigned int set_flags, const char *fmt, ...)
	__printf(2, 3);
int ccci_event_log(const char *fmt, ...) __printf(1, 2);
int ccci_event_log_cpy(char buf[], int size);

#endif /* __CCCI_UTIL_H__ */
