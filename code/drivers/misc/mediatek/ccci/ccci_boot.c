// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI modem boot: firmware validation/load into modem memory, boot vector
 * programming, runtime-data handshake and the boot watchdog.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel:
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c (header parsing,
 *       copy into the modem image region)
 *   drivers/misc/mediatek/ccci/modem_cldma.c (md_cd_power_on/let_md_go,
 *       md_cd_send_runtime_data_v2)
 *   drivers/misc/mediatek/ccci/modem_ccif.c (md_ccif_send_runtime_data,
 *       SRAM runtime block)
 *   drivers/misc/mediatek/ccci/port_kernel.c (prepare_runtime_data)
 *   drivers/misc/mediatek/eccci/mt6735/cldma_platform.c (clock + WDT setup)
 *   drivers/misc/mediatek/eccci/mt6735/modem_reg_base.h (boot vector values)
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#include <linux/ccci.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>
#include <linux/minmax.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/unaligned.h>

#include "ccci_priv.h"

#define CCCI_BOOT_TIMEOUT_CLDMA_S 20	/* modem_cldma.c:119 */
#define CCCI_BOOT_TIMEOUT_CCIF_S  10	/* modem_ccif.c:44  */
#define CCCI_IMG_COPY_CHUNK	  (1024 * 1024)
#define CCCI_IMG_MAX_HEADER	  1024
#define CCCI_IMAGE_ALIGN	  0x4000

#define MD1_LOGGER_FILE_PATH	"/data/mdlog/mdlog1_config"
#define MD2_LOGGER_FILE_PATH	"/data/mdlog/mdlog2_config"

/* check header versions, downstream ccci_util_lib_main.h */
struct md_check_header_v3 {
	u8 check_header[12];
	u32 header_verno;
	u32 product_ver;
	u32 image_type;
	u8 platform[16];
	u8 build_time[64];
	u8 build_ver[64];
	u8 bind_sys_id;
	u8 ext_attr;
	u8 reserved[2];
	u32 mem_size;
	u32 md_img_size;
	u32 rpc_sec_mem_addr;
	u32 dsp_img_offset;
	u32 dsp_img_size;
	u8 reserved2[88];
	u32 size;
} __packed;

struct md_check_header_v4 {
	u8 check_header[12];
	u32 header_verno;
	u32 product_ver;
	u32 image_type;
	u8 platform[16];
	u8 build_time[64];
	u8 build_ver[64];
	u8 bind_sys_id;
	u8 ext_attr;
	u8 reserved[2];
	u32 mem_size;
	u32 md_img_size;
	u32 rpc_sec_mem_addr;
	u32 dsp_img_offset;
	u32 dsp_img_size;
	u32 region_num;
	u32 region_info[8][2];
	u32 domain_attr[4];
	u8 reserved2[4];
	u32 size;
} __packed;

struct md_check_header_v5 {
	u8 check_header[12];
	u32 header_verno;
	u32 product_ver;
	u32 image_type;
	u8 platform[16];
	u8 build_time[64];
	u8 build_ver[64];
	u8 bind_sys_id;
	u8 ext_attr;
	u8 reserved[2];
	u32 mem_size;
	u32 md_img_size;
	u32 rpc_sec_mem_addr;
	u32 dsp_img_offset;
	u32 dsp_img_size;
	u32 region_num;
	u32 region_info[8][2];
	u32 domain_attr[4];
	u32 arm7_img_offset;
	u32 arm7_img_size;
	u8 reserved_1[56];
	u32 size;
} __packed;

/* v1/v2 header, downstream mt_ccci_common.h struct md_check_header */
struct md_check_header_v2 {
	u8 check_header[12];
	u32 header_verno;
	u32 product_ver;
	u32 image_type;
	u8 platform[16];
	u8 build_time[64];
	u8 build_ver[64];
	u8 bind_sys_id;
	u8 ext_attr;
	u8 reserved[2];
	u32 mem_size;
	u32 md_img_size;
	u32 reserved_info;
	u32 size;
} __packed;

/* common leading part of every check header */
struct md_check_header_common {
	u8 check_header[12];
	u32 header_verno;
	u32 product_ver;
	u32 image_type;
	u8 platform[16];
	u8 build_time[64];
	u8 build_ver[64];
	u8 bind_sys_id;
	u8 ext_attr;
	u8 reserved[2];
	u32 mem_size;
	u32 md_img_size;
} __packed;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static u32 ccci_boot_get_booting_start_id(struct ccci_md *md)
{
	struct file *filp;
	char path[32];
	loff_t pos = 0;
	int mdlog_flag = 0;	/* MODE_IDLE */
	ssize_t n;

	snprintf(path, sizeof(path), "%s",
		 md->id == CCCI_MD_SYS1 ? MD1_LOGGER_FILE_PATH :
		 MD2_LOGGER_FILE_PATH);
	filp = filp_open(path, O_RDONLY, 0);
	if (!IS_ERR(filp)) {
		n = kernel_read(filp, &mdlog_flag, sizeof(mdlog_flag), &pos);
		if (n != sizeof(mdlog_flag))
			mdlog_flag = 0;
		filp_close(filp, NULL);
	}

	if (md->md_boot_mode == 2)	/* MD_BOOT_MODE_META */
		return ((u8)mdlog_flag << 8) | CCCI_META_BOOT_ID;
	return ((u8)mdlog_flag << 8) | CCCI_NORMAL_BOOT_ID;
}

static void ccci_boot_timeout(struct timer_list *t)
{
	struct ccci_boot *boot = timer_container_of(boot, t, timer);
	struct ccci_md *md = boot->md;

	boot->timer_armed = false;
	ccci_md_err(md, "handshake%d timeout, boot failed\n",
		    md->boot_stage + 1);
	ccci_md_notify_ports(md, CCCI_EV_BOOT_FAIL);
	/*
	 * downstream md_bootup_timeout_func() (port_kernel.c:3421-3456):
	 * handshake 1 failure hands the CCIF registers and the MD image head
	 * to AEE, handshake 2 failure the exception shared memory and the CCIF
	 * registers; the image head and the memory layout are always dumped
	 * into the util ring.
	 */
	ccci_exception_trigger(md, CCCI_EX_DUMP_NO_RESPONSE,
			       md->boot_stage == CCCI_BOOT_STAGE_0 ?
			       (CCCI_AED_DUMP_CCIF_REG |
				CCCI_AED_DUMP_MD_IMG_MEM) :
			       (CCCI_AED_DUMP_CCIF_REG |
				CCCI_AED_DUMP_EX_MEM));
}

void ccci_boot_arm_timer(struct ccci_md *md)
{
	unsigned int sec = md->ring_mode ? CCCI_BOOT_TIMEOUT_CCIF_S :
			   CCCI_BOOT_TIMEOUT_CLDMA_S;

	mod_timer(&md->boot.timer, jiffies + sec * HZ);
	md->boot.timer_armed = true;
}

void ccci_boot_disarm_timer(struct ccci_md *md)
{
	if (md->boot.timer_armed) {
		timer_delete_sync(&md->boot.timer);
		md->boot.timer_armed = false;
	}
}

/* ------------------------------------------------------------------ */
/* shared memory                                                       */
/* ------------------------------------------------------------------ */
static int ccci_boot_region_get(struct device *dev, const char *prop,
				unsigned int index, phys_addr_t *phys,
				resource_size_t *size)
{
	struct device_node *np = dev->of_node;
	struct device_node *rn;
	struct resource res;
	int ret;

	/*
	 * A reserved-memory child may describe the carve-out with
	 * size/alignment/alloc-ranges instead of reg (downstream
	 * arch/arm64/boot/dts/mt6735.dtsi:197-203 reserve-memory-ccci_md1).
	 * Its base is picked by the early reserved-memory allocator and only
	 * recorded in the reserved_mem[] table (drivers/of/of_reserved_mem.c:
	 * 391-491, fdt_reserved_mem_save_node()), so of_address_to_resource()
	 * cannot resolve it. of_reserved_mem_region_to_resource() handles both
	 * the reg-based and the size-based form.
	 */
	if (!strcmp(prop, "memory-region")) {
		ret = of_reserved_mem_region_to_resource(np, index, &res);
		if (!ret) {
			*phys = res.start;
			*size = resource_size(&res);
			return 0;
		}
	}

	/* Fallback: a plain reg node referenced by the phandle property. */
	rn = of_parse_phandle(np, prop, index);
	if (!rn)
		return -ENOENT;

	ret = of_address_to_resource(rn, 0, &res);
	of_node_put(rn);
	if (ret)
		return ret;

	*phys = res.start;
	*size = resource_size(&res);
	return 0;
}

int ccci_boot_smem_setup(struct ccci_md *md, struct device *dev)
{
	struct device *dma_dev = md->dev->parent ? md->dev->parent : md->dev;
	struct device_node *np = dev ? dev->of_node : NULL;
	phys_addr_t img_phys = 0, smem_phys = 0;
	resource_size_t img_size = 0, smem_size = 0;
	u32 dt_smem_size = 0;
	dma_addr_t dma_addr;
	void *dma_virt;
	int ret;

	md->img_present = false;

	/* image region: memory-region index 0 (reserved-memory, no-map) */
	if (np) {
		ret = ccci_boot_region_get(dev, "memory-region", 0, &img_phys,
					   &img_size);
		if (ret)
			ccci_md_warn(md,
				     "no memory-region[0] (md image), boot will fail\n");
	}

	/* shared memory: memory-region index 1; when absent, a single carve-out
	 * (memory-region[0]) is split exactly like downstream
	 * ccci_util_lib_fo.c:1113-1122: md_smem_size bytes are taken from the
	 * end of the region and the image part is rounded up to 16 KiB
	 * (get_md_smem_align() at ccci_util_lib_fo.c:948-951).
	 */
	if (np) {
		of_property_read_u32(np, "mediatek,md_smem_size",
				     &dt_smem_size);
		ret = ccci_boot_region_get(dev, "memory-region", 1, &smem_phys,
					   &smem_size);
		if (!ret) {
			md->smem_phy = smem_phys;
			md->smem_size = smem_size;
			md->smem_vir = ioremap(smem_phys, smem_size);
			if (!md->smem_vir) {
				ccci_md_err(md, "cannot map smem %pa+0x%zx\n",
					    &smem_phys, (size_t)smem_size);
				return -ENOMEM;
			}
			md->smem_coherent = false;
		} else if (img_size && dt_smem_size) {
			resource_size_t image_size;

			if (dt_smem_size >= img_size) {
				ccci_md_err(md,
					    "md_smem_size 0x%x exceeds carve-out 0x%zx\n",
					    dt_smem_size, (size_t)img_size);
				return -EINVAL;
			}
			image_size = round_up(img_size - dt_smem_size,
					      CCCI_IMAGE_ALIGN);
			if (image_size + dt_smem_size > img_size) {
				ccci_md_err(md,
					    "carve-out 0x%zx cannot hold image 0x%zx + smem 0x%x\n",
					    (size_t)img_size,
					    (size_t)image_size, dt_smem_size);
				return -EINVAL;
			}
			smem_phys = img_phys + image_size;
			smem_size = dt_smem_size;
			img_size = image_size;
			md->smem_phy = smem_phys;
			md->smem_size = smem_size;
			md->smem_vir = ioremap(smem_phys, smem_size);
			if (!md->smem_vir) {
				ccci_md_err(md, "cannot map smem %pa+0x%zx\n",
					    &smem_phys, (size_t)smem_size);
				return -ENOMEM;
			}
			md->smem_coherent = false;
			ccci_md_info(md,
				     "carve-out 0x%zx split: image 0x%zx smem 0x%zx\n",
				     (size_t)(image_size + dt_smem_size),
				     (size_t)image_size, (size_t)smem_size);
		}
	}

	if (img_size) {
		md->img_phy = img_phys;
		md->img_size = img_size;
		md->img_vir = ioremap(img_phys, img_size);
		if (!md->img_vir) {
			ccci_md_err(md, "cannot map md image region %pa+0x%zx\n",
				    &img_phys, (size_t)img_size);
			return -ENOMEM;
		}
		md->img_present = true;
		ccci_md_info(md, "md image region %pa size 0x%zx\n",
			     &md->img_phy, md->img_size);
	}

	if (!md->smem_vir) {
		if (!smem_size) {
			if (dt_smem_size) {
				smem_size = dt_smem_size;
			} else {
				/* worst case: exception + runtime + CCIF rings */
				smem_size = CCCI_SMEM_SIZE_EXCEPTION +
					    CCCI_SMEM_SIZE_RUNTIME +
					    ccci_ccif_ring_total_size();
				smem_size = max_t(size_t, smem_size, 0x30000);
			}
		}
		smem_size = round_up(smem_size, PAGE_SIZE);
		dma_virt = dma_alloc_coherent(dma_dev, smem_size, &dma_addr,
					      GFP_KERNEL);
		if (!dma_virt) {
			ccci_md_err(md, "dma_alloc_coherent(0x%zx) failed\n",
				    (size_t)smem_size);
			return -ENOMEM;
		}
		md->smem_phy = dma_addr;
		md->smem_size = smem_size;
		md->smem_vir = (void __iomem *)dma_virt;
		md->smem_coherent = true;
		/* [INFERENCE] a coherent allocation is only usable if the modem
		 * can address that physical range; provide a reserved-memory
		 * region on real hardware.
		 */
		ccci_md_warn(md,
			     "smem from dma_alloc_coherent %pa+0x%zx, modem visibility unverified\n",
			     &md->smem_phy, md->smem_size);
	}

	memset_io(md->smem_vir, 0, md->smem_size);
	/* let the util debugfs dump file read the modem shared memory */
	ccci_util_dump_set_window(md->id, md->smem_phy, md->smem_size);
	ccci_md_info(md, "smem %pa size 0x%zx%s\n", &md->smem_phy,
		     md->smem_size, md->smem_coherent ? " (coherent)" : "");
	return 0;
}

/* ------------------------------------------------------------------ */
/* firmware                                                            */
/* ------------------------------------------------------------------ */
static void ccci_boot_release_fw(struct ccci_md *md)
{
	if (md->boot.fw) {
		release_firmware(md->boot.fw);
		md->boot.fw = NULL;
	}
	md->boot.fw_valid = false;
}

static int ccci_boot_parse_header(struct ccci_md *md, const u8 *data, size_t size)
{
	struct ccci_boot *boot = &md->boot;
	const struct md_check_header_common *h;
	u32 hdr_size, parse_size;

	if (size < sizeof(struct md_check_header_common) + 4) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "image too small (%zu)\n", size);
		return -EINVAL;
	}

	/* the header size word is the last u32 of the image, downstream
	 * check_md_header(): header_size = *(((u32 *)parse_addr) - 1)
	 */
	hdr_size = get_unaligned_le32(data + size - 4);
	if (hdr_size > CCCI_IMG_MAX_HEADER || hdr_size > size)
		hdr_size = sizeof(struct md_check_header_v2);

	if (hdr_size == sizeof(struct md_check_header_v5))
		parse_size = sizeof(struct md_check_header_v5);
	else if (hdr_size == sizeof(struct md_check_header_v4))
		parse_size = sizeof(struct md_check_header_v4);
	else if (hdr_size == sizeof(struct md_check_header_v3))
		parse_size = sizeof(struct md_check_header_v3);
	else
		parse_size = sizeof(struct md_check_header_v2);

	h = (const struct md_check_header_common *)(data + size - parse_size);

	if (memcmp(h->check_header, CCCI_MD_HEADER_MAGIC, 12)) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "bad magic, not a modem image\n");
		return -EINVAL;
	}

	if (h->bind_sys_id != md->id + 1) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "image is for md sys%d, not md sys%u\n",
			 h->bind_sys_id, md->id + 1);
		return -EINVAL;
	}

	/* header_verno > 5 (v6) is not supported by this driver */
	if (h->header_verno > 5) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "unsupported header version %u\n", h->header_verno);
		return -EOPNOTSUPP;
	}

	boot->hdr_verno = h->header_verno;
	boot->product_ver = h->product_ver;
	boot->image_type = h->image_type;
	boot->mem_size = h->mem_size;
	boot->md_img_size = h->md_img_size;
	boot->bind_sys_id = h->bind_sys_id;
	memcpy(boot->md_platform, h->platform, 16);
	boot->md_platform[16] = 0;
	memcpy(boot->build_ver, h->build_ver, 64);
	boot->build_ver[64] = 0;
	memcpy(boot->build_time, h->build_time, 64);
	boot->build_time[64] = 0;

	/*
	 * DSP sub-image slot: check header v3 and later carry
	 * dsp_img_offset/dsp_img_size (ccci_util_lib_load_img.c:238-253).
	 * 0xCDCDCDAA means "no DSP on EMI" and dsp_offset + dsp_size has to fit
	 * into the image, otherwise the downstream loader rejects the image
	 * with CCCI_ERR_LOAD_IMG_MD_CHECK.
	 */
	if (boot->hdr_verno >= 3 && parse_size >= sizeof(struct md_check_header_v3)) {
		const struct md_check_header_v3 *hv3 = (const void *)h;

		boot->dsp_offset = hv3->dsp_img_offset;
		boot->dsp_size = hv3->dsp_img_size;
		if (boot->dsp_offset == 0xCDCDCDAA) {
			ccci_md_info(md, "DSP on EMI disabled\n");
			boot->dsp_offset = 0;
			boot->dsp_size = 0;
		} else if (boot->dsp_size &&
			   boot->dsp_offset + boot->dsp_size > size) {
			snprintf(boot->err_str, sizeof(boot->err_str),
				 "DSP image 0x%x+0x%x outside image 0x%zx\n",
				 boot->dsp_offset, boot->dsp_size, size);
			return -EINVAL;
		} else {
			ccci_md_info(md, "DSP image offset 0x%x size 0x%x\n",
				     boot->dsp_offset, boot->dsp_size);
		}
	}

	if (boot->md_img_size && boot->md_img_size > size) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "md_img_size 0x%x > firmware size 0x%zx\n",
			 boot->md_img_size, size);
		return -EINVAL;
	}

	return 0;
}

int ccci_boot_load_firmware(struct ccci_md *md)
{
	struct ccci_boot *boot = &md->boot;
	const u8 *src;
	size_t total, copy_len, done = 0;
	u32 chunk;
	int ret;

	if (boot->fw_valid)
		return 0;

	ccci_boot_release_fw(md);

	if (!md->img_present || !md->img_vir) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "no modem image memory region in DT\n");
		return -ENODEV;
	}

	ret = request_firmware(&boot->fw, md->fw_name, md->dev);
	if (ret) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "Modem image '%s' not found (%d)\n", md->fw_name, ret);
		boot->fw = NULL;
		return -ENOENT;
	}

	total = boot->fw->size;
	ret = ccci_boot_parse_header(md, boot->fw->data, total);
	if (ret) {
		ccci_md_err(md, "%s", boot->err_str);
		ccci_boot_release_fw(md);
		return ret;
	}

	copy_len = boot->md_img_size ? min_t(size_t, boot->md_img_size, total) :
		   total;
	if (copy_len > md->img_size) {
		snprintf(boot->err_str, sizeof(boot->err_str),
			 "image 0x%zx larger than md region 0x%zx\n", copy_len,
			 md->img_size);
		ccci_boot_release_fw(md);
		return -ENOMEM;
	}

	src = boot->fw->data;
	strscpy(boot->file_name, md->fw_name, sizeof(boot->file_name));
	while (done < copy_len) {
		chunk = min_t(size_t, CCCI_IMG_COPY_CHUNK, copy_len - done);

		memcpy_toio(md->img_vir + done, src + done, chunk);
		done += chunk;
	}
	/* make sure the image is in memory before the modem is released */
	wmb();
	boot->loaded_size = done;

	/* the modem keeps its own copy in DRAM, do not pin the firmware */
	release_firmware(boot->fw);
	boot->fw = NULL;
	boot->fw_valid = true;

	snprintf(md->boot_info, sizeof(md->boot_info),
		 "MD:%s*%s*%s*%s*%s\nAP:%s drv 0x%08x (MD)mem 0x%08x smem 0x%08x\n",
		 md->boot.image_type == 1 ? "2G" :
		 (md->boot.image_type == 2 ? "3G" : "INVALID"),
		 md->boot.md_platform, md->boot.build_ver,
		 md->boot.build_time,
		 md->boot.product_ver == 1 ? "Debug" :
		 (md->boot.product_ver == 2 ? "Release" : "INVALID"),
		 AP_PLATFORM_INFO, CCCI_DRIVER_VER, boot->mem_size,
		 (u32)md->smem_size);

	ccci_md_info(md, "loaded %s: 0x%zx bytes, hdr v%u, md build %s\n",
		     boot->file_name, done, boot->hdr_verno, boot->build_ver);
	return 0;
}

/* ------------------------------------------------------------------ */
/* power on / let go / power off                                       */
/* ------------------------------------------------------------------ */
int ccci_boot_power_on(struct ccci_md *md)
{
	int ret = 0;
	struct ccci_ccif *ccif = md->ccif;

	if (md->boot.clk) {
		ret = clk_prepare_enable(md->boot.clk);
		if (ret) {
			ccci_md_err(md, "clk_prepare_enable failed %d\n", ret);
			return ret;
		}
	}

	if (md->dev->parent) {
		ret = pm_runtime_resume_and_get(md->dev->parent);
		if (ret) {
			ccci_md_err(md, "pm_runtime_resume_and_get failed %d\n",
				    ret);
			if (md->boot.clk)
				clk_disable_unprepare(md->boot.clk);
			return ret;
		}
	}

	/* downstream md_cd_power_on(): disable the MD watchdog before the
	 * modem starts executing (WDT_MD_MODE_KEY)
	 */
	if (ccif && ccif->rgu_base)
		writel(WDT_MD_MODE_KEY, ccif->rgu_base + WDT_MD_MODE);

	ccci_md_info(md, "power on done\n");
	return 0;
}

int ccci_boot_let_md_go(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;

	if (!ccif || !ccif->boot_key || !ccif->boot_vector || !ccif->boot_en) {
		ccci_md_err(md, "boot slave registers not mapped\n");
		return -ENODEV;
	}

	/* downstream md_cd_let_md_go(): make the boot vector programmable,
	 * program it, then let it take effect. Values from
	 * mt6735/modem_reg_base.h MD2_BOOT_VECTOR_*_VALUE.
	 */
	writel(MD_BOOT_VECTOR_KEY_VALUE, ccif->boot_key);
	writel(MD_BOOT_VECTOR_VALUE, ccif->boot_vector);
	wmb();
	writel(MD_BOOT_VECTOR_EN_VALUE, ccif->boot_en);

	ccci_md_info(md, "boot vector programmed, modem released\n");
	return 0;
}

void ccci_boot_power_off(struct ccci_md *md)
{
	/* downstream polls spm_is_md1_sleep() before power off; SPM API is not
	 * available in this tree, so drop the clock/domain directly.
	 */
	if (md->dev->parent)
		pm_runtime_put_sync(md->dev->parent);
	if (md->boot.clk)
		clk_disable_unprepare(md->boot.clk);
	ccci_md_info(md, "power off done\n");
}

/* ------------------------------------------------------------------ */
/* runtime data handshake                                              */
/* ------------------------------------------------------------------ */
struct ccci_rt_builder {
	void __iomem *base;
	size_t size;
	size_t off;
	bool overflow;
};

static void ccci_rt_append(struct ccci_rt_builder *b, u8 id, u8 mask, u8 ver,
			   const void *data, u32 len)
{
	struct ccci_runtime_feature hdr = {
		.feature_id = id,
		.data_len = len,
	};

	hdr.support_info.support_mask = mask;
	hdr.support_info.version = ver;

	if (b->off + sizeof(hdr) + len > b->size) {
		b->overflow = true;
		return;
	}
	memcpy_toio(b->base + b->off, &hdr, sizeof(hdr));
	b->off += sizeof(hdr);
	if (len) {
		memcpy_toio(b->base + b->off, data, len);
		b->off += len;
	}
}

static u8 ccci_ap_feature_mask(struct ccci_md *md, unsigned int id)
{
	switch (id) {
	case CCCI_RT_BOOT_INFO:
	case CCCI_RT_EXCEPTION_SHARE_MEMORY:
	case CCCI_RT_MISC_INFO_HIF_DMA_REMAP:
	case CCCI_RT_MISC_INFO_RANDOM_SEED_NUM:
	case CCCI_RT_MISC_INFO_GPS_COCLOCK:
	case CCCI_RT_MISC_INFO_SBP_ID:
	case CCCI_RT_MISC_INFO_CCCI:
	case CCCI_RT_MISC_INFO_CLIB_TIME:
		return CCCI_FEATURE_MUST_SUPPORT;
	case CCCI_RT_CCIF_SHARE_MEMORY:
		return md->id == CCCI_MD_SYS1 ? CCCI_FEATURE_NOT_SUPPORT :
		       CCCI_FEATURE_MUST_SUPPORT;
	case CCCI_RT_DHL_SHARE_MEMORY:
	case CCCI_RT_MD1MD3_SHARE_MEMORY:
	case CCCI_RT_MISC_INFO_RTC_32K_LESS:
	case CCCI_RT_MISC_INFO_C2K:
	default:
		/* features whose providers (DHL logging, MD1<->MD3 smem,
		 * 32K-less crystal, C2K always-on) do not exist in this tree
		 */
		return CCCI_FEATURE_NOT_SUPPORT;
	}
}

/**
 * ccci_boot_prepare_runtime_data - answer the modem's feature query
 *
 * The modem sends struct md_query_ap_feature on the control channel during
 * handshake 1; the AP publishes its answers into the runtime region of the
 * shared memory (downstream prepare_runtime_data()).
 */
int ccci_boot_prepare_runtime_data(struct ccci_md *md, struct sk_buff *skb)
{
	const struct md_query_ap_feature *query;
	struct ccci_rt_builder b;
	struct ccci_runtime_share_memory shm;
	struct ccci_misc_info_element el;
	struct ccci_runtime_boot_info bi;
	u32 seed = 0;
	u8 i;

	if (!md->smem_vir || md->smem_size <= CCCI_SMEM_SIZE_EXCEPTION) {
		ccci_md_err(md, "no runtime smem region\n");
		return -ENODEV;
	}
	if (skb->len < CCCI_HEADER_SIZE + sizeof(*query)) {
		ccci_md_err(md, "md feature query too short (%u)\n", skb->len);
		return -EINVAL;
	}
	query = (const struct md_query_ap_feature *)(skb->data +
						     CCCI_HEADER_SIZE);
	if (query->head_pattern != CCCI_MD_FEATURE_QUERY_PATTERN ||
	    query->tail_pattern != CCCI_MD_FEATURE_QUERY_PATTERN) {
		ccci_md_err(md, "bad feature query pattern %08x/%08x\n",
			    query->head_pattern, query->tail_pattern);
		return -EINVAL;
	}

	md->runtime_version = AP_MD_HS_V2;
	b.base = md->smem_vir + CCCI_SMEM_SIZE_EXCEPTION;
	b.size = CCCI_SMEM_SIZE_RUNTIME_AP;
	b.off = 0;
	b.overflow = false;

	for (i = 0; i < CCCI_FEATURE_COUNT; i++) {
		u8 md_mask = query->feature_set[i].support_mask;
		u8 ap_mask = ccci_ap_feature_mask(md, i);
		u8 ver = query->feature_set[i].version;

		if (md_mask == CCCI_FEATURE_MUST_SUPPORT &&
		    ap_mask < CCCI_FEATURE_MUST_SUPPORT) {
			ccci_md_err(md, "md requires feature %u not supported\n",
				    i);
			return -EOPNOTSUPP;
		}

		if (md_mask == CCCI_FEATURE_NOT_EXIST ||
		    md_mask == CCCI_FEATURE_MUST_SUPPORT) {
			/* keep the modem's view */
		} else if (md_mask == CCCI_FEATURE_OPTIONAL_SUPPORT) {
			if (ap_mask >= CCCI_FEATURE_MUST_SUPPORT)
				md_mask = CCCI_FEATURE_MUST_SUPPORT;
			else
				md_mask = CCCI_FEATURE_NOT_SUPPORT;
			ver = query->feature_set[i].version;
		} else {	/* CCCI_FEATURE_SUPPORT_BACKWARD_COMPAT */
			u8 ap_ver = 0;	/* every AP feature is version 0 */

			if (ver >= ap_ver)
				md_mask = CCCI_FEATURE_MUST_SUPPORT;
			else
				md_mask = CCCI_FEATURE_NOT_SUPPORT;
			ver = ap_ver;
		}

		if (md_mask != CCCI_FEATURE_MUST_SUPPORT) {
			ccci_rt_append(&b, i, md_mask, ver, NULL, 0);
			continue;
		}

		switch (i) {
		case CCCI_RT_BOOT_INFO:
			memset(&bi, 0, sizeof(bi));
			bi.boot_channel = CCCI_CONTROL_RX;
			bi.booting_start_id = ccci_boot_get_booting_start_id(md);
			ccci_rt_append(&b, i, md_mask, ver, &bi, sizeof(bi));
			break;
		case CCCI_RT_EXCEPTION_SHARE_MEMORY:
			shm.addr = ccci_mpu_ap_to_md(md, md->smem_phy);
			shm.size = CCCI_SMEM_SIZE_EXCEPTION;
			ccci_rt_append(&b, i, md_mask, ver, &shm, sizeof(shm));
			break;
		case CCCI_RT_CCIF_SHARE_MEMORY:
			shm.addr = ccci_mpu_ap_to_md(md,
						     md->smem_phy +
						     CCCI_SMEM_SIZE_EXCEPTION +
						     CCCI_SMEM_SIZE_RUNTIME);
			shm.size = md->ccif ? md->ccif->total_smem_size : 0;
			ccci_rt_append(&b, i, md_mask, ver, &shm, sizeof(shm));
			break;
		case CCCI_RT_MISC_INFO_RANDOM_SEED_NUM:
			memset(&el, 0, sizeof(el));
			get_random_bytes(&seed, sizeof(seed));
			el.feature[0] = seed;
			ccci_rt_append(&b, i, md_mask, ver, &el, sizeof(el));
			break;
		case CCCI_RT_MISC_INFO_SBP_ID:
			memset(&el, 0, sizeof(el));
			el.feature[0] = md->sbp_code;
			ccci_rt_append(&b, i, md_mask, ver, &el, sizeof(el));
			break;
		case CCCI_RT_MISC_INFO_CCCI:
			memset(&el, 0, sizeof(el));
			el.feature[0] |= BIT(0);	/* sequence check */
			ccci_rt_append(&b, i, md_mask, ver, &el, sizeof(el));
			break;
		case CCCI_RT_MISC_INFO_CLIB_TIME:
			memset(&el, 0, sizeof(el));
			el.feature[0] = (u32)ktime_get_real_seconds();
			el.feature[2] = sys_tz.tz_minuteswest;
			el.feature[3] = sys_tz.tz_dsttime;
			ccci_rt_append(&b, i, md_mask, ver, &el, sizeof(el));
			break;
		default:
			memset(&el, 0, sizeof(el));
			ccci_rt_append(&b, i, md_mask, ver, &el, sizeof(el));
			break;
		}
	}

	if (b.overflow) {
		ccci_md_err(md, "runtime feature block exceeds 0x%x bytes\n",
			    CCCI_SMEM_SIZE_RUNTIME_AP);
		return -ENOSPC;
	}
	wmb();
	ccci_md_info(md, "runtime data published, %zu bytes\n", b.off);
	return 0;
}

/* legacy SRAM runtime block, downstream md_ccif_op_send_runtime_data() */
static int ccci_boot_send_runtime_sram(struct ccci_md *md)
{
	struct ccci_ccif *ccif = md->ccif;
	struct modem_runtime rt;
	struct ccci_header *ccci_h;
	u32 packet_size = sizeof(struct ccci_header) + sizeof(rt);
	u32 seed = 0;
	u32 mask = 0;
	u32 i;

	if (!ccif || !ccif->sram)
		return -ENODEV;
	if (sizeof(rt) > sizeof(ccif->sram->runtime))
		return -ENOSPC;

	ccci_h = &ccif->sram->up_header;

	writel(0x00, &ccci_h->data[0]);
	writel(packet_size, &ccci_h->data[1]);
	writel(MD_INIT_CHK_ID, &ccci_h->reserved);
	writel(CCCI_CONTROL_TX, ((u32 __iomem *)ccci_h) + 2);

	memset(&rt, 0, sizeof(rt));
	rt.Prefix = 0x46494343;		/* "CCIF" */
	rt.Postfix = 0x46494343;	/* "CCIF" */
	memcpy(&rt.Platform_L, AP_PLATFORM_INFO,
	       min_t(size_t, 8, strlen(AP_PLATFORM_INFO)));
	rt.BootChannel = CCCI_CONTROL_RX;
	rt.DriverVersion = CCCI_DRIVER_VER;
	rt.BootingStartID = ccci_boot_get_booting_start_id(md);

	rt.ExceShareMemBase = ccci_mpu_ap_to_md(md, md->smem_phy);
	rt.ExceShareMemSize = CCCI_SMEM_SIZE_EXCEPTION;
	rt.CCIFShareMemBase = ccci_mpu_ap_to_md(md, md->smem_phy +
						CCCI_SMEM_SIZE_EXCEPTION +
						CCCI_SMEM_SIZE_RUNTIME);
	rt.CCIFShareMemSize = ccif->total_smem_size;
	rt.TotalShareMemBase = ccci_mpu_ap_to_md(md, md->smem_phy);
	rt.TotalShareMemSize = (u32)md->smem_size;

	rt.misc_prefix = 0x4353494D;	/* "MISC" */
	rt.misc_postfix = 0x4353494D;

	/*
	 * Legacy block slot numbers, downstream MISC_FEATURE_ID: RAND_SEED is
	 * feature_2_val[], MD_SBP_SETTING is feature_4_val[] and
	 * MD_SEQ_CHECK is feature_5_val[] (modem_ccif.c:930-958).  The SBP
	 * feature is only advertised when a code was configured, exactly like
	 * md_cd_send_runtime_data() does for the CLDMA block
	 * (modem_cldma.c:3353-3360).
	 */
	mask |= CCCI_FEATURE_NOT_SUPPORT << (CCCI_MISC_32K_LESS * 2);
	mask |= CCCI_FEATURE_SUPPORT << (CCCI_MISC_RAND_SEED * 2);
	if (md->sbp_code > 0)
		mask |= CCCI_FEATURE_SUPPORT << (CCCI_MISC_MD_SBP_SETTING * 2);
	mask |= CCCI_FEATURE_SUPPORT << (CCCI_MISC_MD_SEQ_CHECK * 2);
	rt.support_mask = mask;

	get_random_bytes(&seed, sizeof(seed));
	rt.feature_val[CCCI_MISC_RAND_SEED][0] = seed;
	if (md->sbp_code > 0)
		rt.feature_val[CCCI_MISC_MD_SBP_SETTING][0] = md->sbp_code;
	rt.feature_val[CCCI_MISC_MD_SEQ_CHECK][0] = BIT(0);

	memcpy_toio(ccif->sram->runtime, &rt, sizeof(rt));
	wmb();
	(void)i;

	return ccci_ccif_send(md, H2D_SRAM);
}

int ccci_boot_send_runtime_data(struct ccci_md *md, u32 sbp_code)
{
	struct ap_query_md_feature *ap;
	struct ccci_header *ccci_h;
	struct sk_buff *skb;
	u32 packet_size = sizeof(struct ccci_header) + sizeof(*ap);
	unsigned int qno = 0;
	int ret;

	if (md->state != CCCI_MD_BOOTING && md->state != CCCI_MD_READY)
		return -CCCI_ERR_MD_NOT_READY;

	/* downstr+ md_cd_send_runtime_data(): the modem no longer needs to read
	 * AP memory at this point, so re-protect the AP region.
	 */
	ccci_mpu_set_ap_region_protection(md);

	md->sbp_code = sbp_code ? sbp_code : md->sbp_code_default;

	if (md->ring_mode)
		return ccci_boot_send_runtime_sram(md);

	skb = ccci_alloc_skb(packet_size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	ap = (struct ap_query_md_feature *)skb_put(skb, sizeof(*ap));
	memset(ap, 0, sizeof(*ap));
	ap->head_pattern = CCCI_AP_FEATURE_QUERY_PATTERN;
	ap->tail_pattern = CCCI_AP_FEATURE_QUERY_PATTERN;
	ap->share_memory_support = 1;
	ap->ap_runtime_data_addr =
		ccci_mpu_ap_to_md(md, md->smem_phy + CCCI_SMEM_SIZE_EXCEPTION);
	ap->ap_runtime_data_size = CCCI_SMEM_SIZE_RUNTIME_AP;
	ap->md_runtime_data_addr = ap->ap_runtime_data_addr +
				   CCCI_SMEM_SIZE_RUNTIME_AP;
	ap->md_runtime_data_size = CCCI_SMEM_SIZE_RUNTIME_MD;
	ap->set_md_mpu_start_addr = ccci_mpu_ap_to_md(md, md->smem_phy);
	ap->set_md_mpu_total_size = (u32)md->smem_size;

	ccci_h = (struct ccci_header *)skb_push(skb, CCCI_HEADER_SIZE);
	memset(ccci_h, 0, CCCI_HEADER_SIZE);
	ccci_h->data[0] = 0;
	ccci_h->data[1] = packet_size;
	ccci_h->channel = CCCI_CONTROL_TX;
	ccci_h->reserved = MD_INIT_CHK_ID;

	ret = ccci_md_send_skb(md, qno, skb);
	if (ret)
		dev_kfree_skb_any(skb);
	return ret;
}

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */
int ccci_boot_init(struct ccci_md *md)
{
	struct ccci_boot *boot = &md->boot;

	boot->md = md;
	boot->fw = NULL;
	boot->fw_valid = false;
	boot->timer_armed = false;
	snprintf(boot->post_fix, sizeof(boot->post_fix), "%u", md->id + 1);
	timer_setup(&boot->timer, ccci_boot_timeout, 0);
	return 0;
}

void ccci_boot_exit(struct ccci_md *md)
{
	struct ccci_boot *boot = &md->boot;

	timer_delete_sync(&boot->timer);
	ccci_boot_release_fw(md);
	if (md->img_vir) {
		iounmap(md->img_vir);
		md->img_vir = NULL;
	}
	if (md->smem_vir) {
		if (md->smem_coherent) {
			struct device *dma_dev = md->dev &&
						 md->dev->parent ?
						 md->dev->parent : md->dev;

			if (dma_dev)
				dma_free_coherent(dma_dev, md->smem_size,
						  (void *)md->smem_vir,
						  md->smem_phy);
		} else {
			iounmap(md->smem_vir);
		}
		md->smem_vir = NULL;
	}
}
