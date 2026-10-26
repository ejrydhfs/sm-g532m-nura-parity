// SPDX-License-Identifier: GPL-2.0
/*
 * MT6735/MT6737T modem memory protection.
 *
 * Two independent pieces of the downstream CCCI platform code live here:
 *
 *  1. EMI MPU region programming.  On MT6735/MT6737T the MPU registers are
 *     only reachable from the secure world, so every region update is an
 *     ATF SMC (the same code path the shipping ROM uses):
 *
 *       mach/mt_secure_api.h:38   MTK_SIP_KERNEL_EMIMPU_SET = 0x82000209
 *       mach/mt_secure_api.h:90-91
 *             emi_mpu_smc_set(start, end, region_permission)
 *             -> mt_secure_call(id, start, end, region_permission)
 *       emi_reg_rw.c:74-82        mt_emi_mpu_set_region_protection()
 *       emi_mpu.c:1138-1149       emi_mpu_set_region_protection(): packs
 *             (permission & 0xFFFFFF) | ((region & 0xF) << 27)
 *
 *     ROM proof (ARM32 vmlinux.elf):
 *       emi_mpu_set_region_protection @ 0xc064e448:
 *         0xc064e470  and   r12, r12, #15        region &= 0xF
 *         0xc064e478  bic   r10, r10, #0xFF000000
 *         0xc064e480  orr   r10, r10, r12, lsl #27
 *         0xc064e4a0  bl    mt_emi_mpu_set_region_protection
 *       mt_emi_mpu_set_region_protection @ 0xc064c7c4:
 *         0xc064c7d0  mov   r1, r0                start (low word)
 *         0xc064c7d4  ldr   r3, [r11, #4]         permission (stack arg)
 *         0xc064c7d8  movw  r0, #0x0209
 *         0xc064c7dc  movt  r0, #0x8200           function id 0x82000209
 *         0xc064c7e0  bl    mt_secure_call
 *       mt_secure_call @ 0xc064c6f8: 0xc064c704  smc #0
 *
 *     Only the low 32 bits of start/end reach the monitor (the downstream
 *     helper is a 4-register function), so addresses above 4 GiB are
 *     rejected here instead of being silently truncated.
 *
 *  2. RMPU (memory remap) windows.  MD1 and MD2/MD3 see DRAM through BANK0
 *     and BANK4 remap slots in the always-on INFRA window; without them the
 *     modem cannot address its image or the shared memory at all.
 *     mt6735/ccci_platform.c:130-148 (register offsets)
 *     mt6735/ccci_platform.h:22    (KERN_EMI_BASE 0x40000000 = Bank4)
 *     ROM proof of the offsets/stores:
 *       set_md_smem_remap       @ 0xc053011c: str ..., [r3, #0x308/#0x30c]
 *                                            str ..., [r3, #0x318/#0x31c]
 *       set_md_rom_rw_mem_remap @ 0xc05303b4: str ..., [r3, #0x300/#0x304]
 *                                            str ..., [r3, #0x310/#0x314]
 *
 *  3. Image-security state reporting (ROM ccci_init_security).
 *
 * Port from Samsung/MTK downstream:
 *   drivers/misc/mediatek/eccci/mt6735/ccci_platform.c
 *   drivers/misc/mediatek/eccci/mt6735/ccci_platform.h
 *   drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/emi_mpu.h
 *   drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/mt_secure_api.h
 *   drivers/misc/mediatek/emi_mpu/emi_reg_rw.c
 *   drivers/misc/mediatek/emi_mpu/mt6735/emi_mpu.c
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c
 *
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#include <linux/arm-smccc.h>
#include <linux/ccci.h>
#include <linux/io.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/string.h>

#include "ccci_priv.h"
#include "ccci_mpu.h"

/* ATF SIP function id, mach/mt_secure_api.h:38 */
#define MTK_SIP_KERNEL_EMIMPU_SET	0x82000209

/*
 * Access permission encodings, mach/emi_mpu.h:81,85,86.
 * The SET_ACCESS_PERMISSON() packing is mach/emi_mpu.h:195-197.
 */
#define MPU_NO_PROTECTION	0
#define MPU_SEC_R_NSEC_R	4
#define MPU_FORBIDDEN		5

#define SET_ACCESS_PERMISSON(d7, d6, d5, d4, d3, d2, d1, d0) \
	(((d7) << 21) | ((d6) << 18) | ((d5) << 15) | \
	 ((d4) << 12) | ((d3) << 9) | ((d2) << 6) | ((d1) << 3) | (d0))

/*
 * Region ids for MT6735/MT6737T, i.e. the non-CONFIG_ARCH_MT6735M half of
 * mt6735/ccci_platform.c:81-98.  arch/arm/configs/grandpplte_00_defconfig:296
 * leaves CONFIG_ARCH_MT6735M unset.  (The two remaining ids of the table,
 * WIFI EMI FW 12 and WMT 13, are not used by the modem.)
 *
 * The D-domain order of SET_ACCESS_PERMISSON is
 * (D7, D6(MFG), D5(MD2), D4(MM), D3(MD32), D2(CONN), D1(MD1), D0(AP))
 * per ccci_platform.c:99-100, so MD2 is D5 and MD1 is D1.
 */
#define MPU_REGION_ID_SEC_OS		0	/* ccci_platform.c:81 */
#define MPU_REGION_ID_ATF		1	/* ccci_platform.c:82 */
#define MPU_REGION_ID_MD1_SEC_SMEM	3	/* ccci_platform.c:84 */
#define MPU_REGION_ID_MD2_SEC_SMEM	4	/* ccci_platform.c:85 */
#define MPU_REGION_ID_MD1_ROM		5	/* ccci_platform.c:86 */
#define MPU_REGION_ID_MD1_DSP		6	/* ccci_platform.c:87 */
#define MPU_REGION_ID_MD2_ROM		7	/* ccci_platform.c:88 */
#define MPU_REGION_ID_MD2_RW		8	/* ccci_platform.c:89 */
#define MPU_REGION_ID_MD1_SMEM		9	/* ccci_platform.c:90 */
/* MD2 and MD3 share ids and BANK registers (ccci_platform.c:91-94,143-148) */
#define MPU_REGION_ID_MD3_ROM		7	/* ccci_platform.c:92 */
#define MPU_REGION_ID_MD3_RW		8	/* ccci_platform.c:93 */
#define MPU_REGION_ID_MD3_SMEM		10	/* ccci_platform.c:94 */
#define MPU_REGION_ID_MD1_RW		14	/* ccci_platform.c:97 */
#define MPU_REGION_ID_AP		15	/* ccci_platform.c:98 */

/* mt6735/ccci_platform.c:101-102 */
#define MPU_PERM_CLEAR \
	SET_ACCESS_PERMISSON(MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION)

/* mt6735/ccci_platform.c:103-104: MD1 may read the AP region while booting.
 * Value 0x20; ROM ccci_set_mem_access_protection @ 0xc052fc70: mov lr, #32.
 */
#define MPU_PERM_AP_MD1_RO \
	SET_ACCESS_PERMISSON(MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION, \
			     MPU_SEC_R_NSEC_R, MPU_NO_PROTECTION)

/* mt6735/ccci_platform.c:105-106.  Value 0x0A28B68;
 * ROM ccci_set_ap_region_protection @ 0xc052f830/0xc052f834:
 * movw r12, #0x8b68 / movt r12, #0xa2.
 */
#define MPU_PERM_AP \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_NO_PROTECTION, \
			     MPU_FORBIDDEN, MPU_NO_PROTECTION, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_NO_PROTECTION)

/* mt6735/ccci_platform.c:107-116.  MD1_ROM is 0xB6DB64 and MD1_RW 0xB6DB45
 * in the ROM (ccci_set_mem_access_protection @ 0xc052fcb0..0xc052fcd0:
 * movw/movt r10 = 0xdb64/0xb6, r7 = 0xdb45/0xb6, r8 = 0xdb40/0xb6).
 */
#define MPU_PERM_MD1_ROM \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_SEC_R_NSEC_R, MPU_SEC_R_NSEC_R)
#define MPU_PERM_MD1_RW \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_FORBIDDEN)
#define MPU_PERM_MD1_SMEM \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_NO_PROTECTION)
/* ccci_platform.c:113-114; also 0xB6DB64, ROM ccci_set_dsp_region_protection
 * @ 0xc052f938/0xc052f950 (movw r0, #0xdb64 / movt r0, #0xb6)
 */
#define MPU_PERM_MD1_DSP \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_SEC_R_NSEC_R, MPU_SEC_R_NSEC_R)
/* ccci_platform.c:115-116; 0xB6DB45, ROM 0xc052f940/0xc052f954
 * (movw r3, #0xdb45 / movt r3, #0xb6)
 */
#define MPU_PERM_MD1_DSP_CL \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_FORBIDDEN)

/* mt6735/ccci_platform.c:118-123; 0xB45B6C / 0xB45B6C / 0xB45B68 in the ROM
 * (ccci_set_mem_access_protection @ 0xc052fabc..0xc052fad0:
 * movw/movt r7 = 0x5b6c/0xb4, r8 = 0x5b68/0xb4).
 */
#define MPU_PERM_MD3_ROM \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_SEC_R_NSEC_R)
#define MPU_PERM_MD3_RW \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_SEC_R_NSEC_R)
#define MPU_PERM_MD3_SMEM \
	SET_ACCESS_PERMISSON(MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_NO_PROTECTION, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_FORBIDDEN, \
			     MPU_FORBIDDEN, MPU_NO_PROTECTION)

/* RMPU windows, mt6735/ccci_platform.c:130-148 (offsets into INFRA_AO) */
#define MD1_BANK0_MAP0		0x300
#define MD1_BANK0_MAP1		0x304
#define MD1_BANK4_MAP0		0x308
#define MD1_BANK4_MAP1		0x30C
#define MD2_BANK0_MAP0		0x310
#define MD2_BANK0_MAP1		0x314
#define MD2_BANK4_MAP0		0x318
#define MD2_BANK4_MAP1		0x31C
/* MD3 uses the same registers as MD2 (ccci_platform.c:143-148) */
#define MD3_BANK0_MAP0		MD2_BANK0_MAP0
#define MD3_BANK0_MAP1		MD2_BANK0_MAP1
#define MD3_BANK4_MAP0		MD2_BANK4_MAP0
#define MD3_BANK4_MAP1		MD2_BANK4_MAP1

/* mt6735/ccci_platform.h:22 */
#define KERN_EMI_BASE		0x40000000UL	/* Bank4 */
/*
 * RMPU slot stride.  Every slot of a BANKn_MAPx word covers one 32 MiB
 * window; the constant is the literal 0x2000000 used by
 * ccci_platform.c:497-504 (MD1), 512-519 (MD2 smem), 583-590 (MD2 rom).
 */
#define MD_REMAP_SLOT_SIZE	0x02000000UL

/*
 * Program one MPU region.  The permission word carries the region id in
 * bits 27..30 exactly like emi_mpu_set_region_protection() (emi_mpu.c:1144).
 * Returns 0 or the SIP status code (SIP_SVC_E_*, mt_secure_api.h:22-26).
 *
 * On MT6735/MT6737T the MPU registers live in the secure EMI block: the only
 * kernel-side accessor downstream is mt_emi_mpu_set_region_protection() ->
 * mt_secure_call(MTK_SIP_KERNEL_EMIMPU_SET) (emi_reg_rw.c:74-82), compiled
 * for CONFIG_ARM_PSCI/CONFIG_MTK_PSCI; without such a conduit downstream
 * returns 0 without touching hardware (emi_reg_rw.c:81).  There is no
 * non-secure register path for the MT6735 MPU regions, so we do not invent
 * one: a missing conduit is reported as an error instead of faking success.
 */
static int ccci_mpu_set_region(struct ccci_md *md, phys_addr_t start,
			       phys_addr_t end, unsigned int region,
			       u32 permission)
{
	struct arm_smccc_res res;
	u32 perm;

	if (!md->mpu_smc) {
		ccci_md_err(md,
			    "no EL3/ATF SMC conduit, EMI MPU region %u not programmed\n",
			    region);
		return -ENODEV;
	}
	if ((start >> 32) || (end >> 32)) {
		ccci_md_err(md, "MPU region %u outside 32-bit space (%pa-%pa)\n",
			    region, &start, &end);
		return -ERANGE;
	}

	perm = (permission & 0xFFFFFF) | ((region & 0xF) << 27);
	memset(&res, 0, sizeof(res));
	arm_smccc_smc(MTK_SIP_KERNEL_EMIMPU_SET, (u32)start, (u32)end, perm,
		      0, 0, 0, 0, &res);
	return (int)res.a0;
}

static int ccci_mpu_program(struct ccci_md *md, const char *name,
			    phys_addr_t start, phys_addr_t end,
			    unsigned int region, u32 permission)
{
	int ret;

	ret = ccci_mpu_set_region(md, start, end, region, permission);
	if (ret)
		ccci_md_err(md,
			    "MPU %s region %u [0x%08llx-0x%08llx] perm 0x%08x failed: %d\n",
			    name, region, (unsigned long long)start,
			    (unsigned long long)end, permission, ret);
	else
		ccci_md_info(md,
			     "MPU %s region %u [0x%08llx-0x%08llx] perm 0x%08x\n",
			     name, region, (unsigned long long)start,
			     (unsigned long long)end, permission);
	return ret;
}

/*
 * End address rounding used by the whole downstream CCCI MPU code:
 *   end = ((start + size + 0xFFFF) & ~0xFFFF) - 1
 * (ccci_platform.c:272, 391-392, 398; EMI MPU granule is 64 KiB,
 * mach/emi_mpu.h:96-97.  ROM: ccci_set_mem_access_protection @ 0xc052fb34
 * ..0xc052fb5c and ccci_set_dsp_region_protection @ 0xc052f93c..0xc052f96c.)
 */
static phys_addr_t ccci_mpu_region_end(phys_addr_t start, size_t size)
{
	return ((start + size + 0xFFFF) & ~(phys_addr_t)0xFFFF) - 1;
}

/* ------------------------------------------------------------------ */
/* image security state                                                */
/* ------------------------------------------------------------------ */
/*
 * ROM ccci_init_security() (0xc0501e54) calls printk() once and returns 0
 * ("mov r0, #0" at 0xc0501e6c); the downstream source
 * (ccci_util/ccci_util_lib_load_img.c:934-942) does the same.
 * ENABLE_MD_IMG_SECURITY_FEATURE only selects the text, and the ROM build
 * had it on: vmlinux.strings:37195 "[ccci0/util]security is on!".
 *
 * No signature, hash or cipher verification runs in the kernel: the ROM
 * ccci_load_firmware() only does request_firmware(), the check-header parse
 * and the image copy.  The modem image is authenticated by LK/ATF/the MD
 * boot ROM, and the kernel holds no data that would let it claim otherwise.
 * Report the parsed header state and return the downstream status code (0)
 * instead of fabricating a check.
 */
int ccci_init_security(struct ccci_md *md)
{
	struct ccci_boot *boot = &md->boot;

	if (boot->fw_valid)
		ccci_md_info(md,
			     "security is on! (hdr v%u, image type %u, product %u, platform %s, build %s)\n",
			     boot->hdr_verno, boot->image_type,
			     boot->product_ver, boot->md_platform,
			     boot->build_ver);
	else
		ccci_md_info(md,
			     "security is on! (modem image header not parsed yet)\n");
	ccci_md_info(md,
		     "security: image authentication is not performed by the kernel (LK/ATF/MD boot ROM)\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* DSP region (optional)                                               */
/* ------------------------------------------------------------------ */
/*
 * The DSP sub-image lives inside the modem image at dsp_img_offset for
 * dsp_img_size bytes (check header v3+, ccci_util_lib_load_img.c:238-253 and
 * 454-468: dsp_offset+dsp_size must fit into the image, 0xCDCDCDAA means "no
 * DSP on EMI").  downstream md_cd_start() (modem_cldma.c:2270-2286) then
 * points mem_layout.dsp_region_phy at img.address + dsp_offset.
 *
 * The region only exists after a successful image load, which is why
 * fw_valid is required (downstream tests mem_layout.dsp_region_phy != 0).
 */
static bool ccci_mpu_dsp_region(struct ccci_md *md, phys_addr_t *phy,
				size_t *size)
{
	u32 off = md->boot.dsp_offset;
	u32 len = md->boot.dsp_size;

	if (!md->boot.fw_valid || !md->img_present || !off || !len)
		return false;
	if (off >= md->img_size || len > md->img_size - off) {
		ccci_md_err(md, "DSP image 0x%x+0x%x outside MD region 0x%zx\n",
			    off, len, md->img_size);
		return false;
	}
	*phy = md->img_phy + off;
	*size = len;
	return true;
}

/*
 * Resolve the MPU backend.  Downstream enables the EMI MPU SMC path for
 * CONFIG_ARM_PSCI (emi_reg_rw.c:78-79), so the requirement is an EL3 monitor
 * reachable over SMC.  The MT6735 DTS declares it as
 *   psci { compatible = "arm,psci"; method = "smc"; };
 * (arch/arm64/boot/dts/mediatek/mt6735.dtsi:34-40); a PSCI node whose method
 * is "hvc" (a guest) has no SMC conduit and is rejected.
 *
 * The RMPU side (ccci_mpu_set_mem_remap) needs no monitor: its registers are
 * in the non-secure INFRA_AO window and are written directly, exactly like
 * downstream (ccci_plat_common_init() of_iomap("mediatek,INFRACFG_AO"),
 * ccci_platform.c:748-757).
 */
static bool ccci_mpu_has_smc_conduit(void)
{
	static const char *const compat[] = {
		"arm,psci", "arm,psci-0.2", "arm,psci-1.0",
	};
	struct device_node *np = NULL;
	const char *method;
	bool ok = false;
	int i;

	if (!IS_ENABLED(CONFIG_HAVE_ARM_SMCCC))
		return false;

	for (i = 0; i < ARRAY_SIZE(compat) && !np; i++)
		np = of_find_compatible_node(NULL, NULL, compat[i]);
	if (!np)
		return false;
	if (!of_property_read_string(np, "method", &method))
		ok = !strcmp(method, "smc");
	of_node_put(np);
	return ok;
}

int ccci_mpu_init(struct ccci_md *md)
{
	md->mpu_smc = ccci_mpu_has_smc_conduit();
	md->dsp_phy = 0;
	md->dsp_size = 0;
	ccci_md_info(md, "EMI MPU backend: %s\n",
		     md->mpu_smc ? "ATF SMC (PSCI/EL3)" :
				   "none (no EL3 conduit)");
	return 0;
}

/*
 * Program the MD1_DSP region; @loaded selects the DSP-image attribute
 * (downstream ccci_set_dsp_region_protection(), ccci_platform.c:251-325).
 * The region comes from the dsp_img_offset/dsp_img_size fields of the parsed
 * image header, which is what downstream derives mem_layout.dsp_region_phy
 * from.  Returns 0 when the image carries no DSP sub-image (the
 * MT6735/MT6737T case, and the frozen RPC contract for op 0x400B).
 */
int ccci_mpu_dsp_protection(struct ccci_md *md, bool loaded)
{
	phys_addr_t phy;
	size_t size;

	if (md->id != CCCI_MD_SYS1) {
		/* downstream ccci_set_dsp_region_protection() default case,
		 * ccci_platform.c:266-268
		 */
		ccci_md_err(md, "DSP MPU protection is MD1-only\n");
		return -EINVAL;
	}
	if (!ccci_mpu_dsp_region(md, &phy, &size)) {
		/* The downstream caller only invokes this when
		 * mem_layout.dsp_region_phy != 0 (modem_cldma.c:2320-2321),
		 * so a missing DSP slot is a no-op, not an error.
		 */
		md->dsp_phy = 0;
		md->dsp_size = 0;
		ccci_md_info(md, "no DSP region, skip DSP MPU protection\n");
		return 0;
	}

	md->dsp_phy = phy;
	md->dsp_size = size;
	return ccci_mpu_program(md, loaded ? "MD1_DSP(loaded)" : "MD1_DSP",
				phy, ccci_mpu_region_end(phy, size),
				MPU_REGION_ID_MD1_DSP,
				loaded ? MPU_PERM_MD1_DSP_CL : MPU_PERM_MD1_DSP);
}

/*
 * Frozen gap API (include/linux/ccci.h:313): called from the kernel RPC
 * service for IPC_RPC_DSP_EMI_MPU_SETTING (op 0x400B, port_kernel.h:142;
 * downstream handler port_kernel.c:1797-1826 calls
 * ccci_set_dsp_region_protection(md, 1) when a DSP region exists).
 * @md_id is the core's 0-based modem id (include/linux/ccci.h:23).
 * Returns 0 when there is no DSP region.
 */
int ccci_md_set_dsp_mpu_protection(unsigned int md_id)
{
	struct ccci_md *md = ccci_md_get(md_id);

	if (!md)
		return -ENODEV;
	return ccci_mpu_dsp_protection(md, true);
}

/* ------------------------------------------------------------------ */
/* MD / AP EMI MPU regions                                             */
/* ------------------------------------------------------------------ */
/*
 * Clear the MD ROM/RW regions and, for MD1, the DSP region.
 * Downstream splits this over ccci_clear_md_region_protection()
 * (ccci_platform.c:150-192, ROM 0xc052f3c4) and
 * ccci_clear_dsp_region_protection() (ccci_platform.c:194-219, ROM
 * 0xc052f684); md_cd_start() calls both back to back before loading the
 * image (modem_cldma.c:2261-2263), so they are folded here.
 *
 * Both downstream functions start with a "modem_run_env_ready()" bypass for
 * an image that LK already loaded and protected.  This driver always loads
 * the image itself over request_firmware() (ccci_boot.c), so that state can
 * never be true and the bypass is not reproduced.
 */
void ccci_mpu_clear_md_protection(struct ccci_md *md)
{
	unsigned int rom_id, rw_id;

	switch (md->id) {
	case CCCI_MD_SYS1:
		rom_id = MPU_REGION_ID_MD1_ROM;
		rw_id = MPU_REGION_ID_MD1_RW;
		break;
	case CCCI_MD_SYS2:
		rom_id = MPU_REGION_ID_MD2_ROM;
		rw_id = MPU_REGION_ID_MD2_RW;
		break;
	case CCCI_MD_SYS3:
		rom_id = MPU_REGION_ID_MD3_ROM;
		rw_id = MPU_REGION_ID_MD3_RW;
		break;
	default:
		/* downstream ccci_clear_md_region_protection() default,
		 * ccci_platform.c:175-179
		 */
		ccci_md_err(md, "invalid MD id for MPU clear\n");
		return;
	}

	/* ccci_platform.c:181-190: start = end = 0 clears the region;
	 * ROM 0xc052f478..0xc052f4d4 does the two calls with permission 0.
	 */
	ccci_mpu_program(md, "clear ROM", 0, 0, rom_id, MPU_PERM_CLEAR);
	ccci_mpu_program(md, "clear RW", 0, 0, rw_id, MPU_PERM_CLEAR);

	if (md->id == CCCI_MD_SYS1)
		ccci_mpu_program(md, "clear DSP", 0, 0,
				 MPU_REGION_ID_MD1_DSP, MPU_PERM_CLEAR);
}

/*
 * Program the MD ROM/RW/SMEM regions plus the AP region as MD-read-only.
 * Downstream ccci_set_mem_access_protection() (ccci_platform.c:327-432),
 * ROM 0xc052fa48.  Region maths as in the ROM:
 *
 *   rom_end = ((md_region_phy + img_size + 0xFFFF) & ~0xFFFF) - 1
 *   rw_start = rom_end + 1
 *   rw_end   = md_region_phy + md_region_size - 1
 *   smem_end = ((smem_phy + smem_size + 0xFFFF) & ~0xFFFF) - 1
 *
 * The rw_end expression comes from the ROM (0xc052fb84: "add lr, r9, r12"
 * with r9 = md_region_phy and r12 = md_region_size, then 0xc052fbd0 uses
 * that value as the region end).  The C source (ccci_platform.c:392-393)
 * writes "rw_start + md_region_size - 1" instead; the shipped binary is
 * authoritative and is followed here.
 */
int ccci_mpu_set_md_protection(struct ccci_md *md)
{
	phys_addr_t rom_end, rw_start, rw_end, ap_start;
	size_t ap_size, img_size;
	u32 rom_id, rw_id, shr_id;
	u32 rom_perm, rw_perm, shr_perm;
	int ret, first = 0;

	if (!md->img_present || !md->img_vir) {
		ccci_md_err(md, "no MD image region, skip MPU protection\n");
		return -ENODEV;
	}
	if (!md->smem_vir) {
		ccci_md_err(md, "no MD share memory, skip MPU protection\n");
		return -ENODEV;
	}

	switch (md->id) {
	case CCCI_MD_SYS1:
		rom_id = MPU_REGION_ID_MD1_ROM;
		rw_id = MPU_REGION_ID_MD1_RW;
		shr_id = MPU_REGION_ID_MD1_SMEM;
		rom_perm = MPU_PERM_MD1_ROM;
		rw_perm = MPU_PERM_MD1_RW;
		shr_perm = MPU_PERM_MD1_SMEM;
		break;
	case CCCI_MD_SYS3:
		rom_id = MPU_REGION_ID_MD3_ROM;
		rw_id = MPU_REGION_ID_MD3_RW;
		shr_id = MPU_REGION_ID_MD3_SMEM;
		rom_perm = MPU_PERM_MD3_ROM;
		rw_perm = MPU_PERM_MD3_RW;
		shr_perm = MPU_PERM_MD3_SMEM;
		break;
	default:
		/* downstream ccci_set_mem_access_protection() only handles
		 * MD_SYS1 and MD_SYS3 (ccci_platform.c:345-371); do not guess
		 * region ids for MD2/MD4/MD5.
		 */
		ccci_md_err(md, "invalid MD id for MPU protection\n");
		return -EINVAL;
	}

	/* img_info[IMG_MD].size in downstream terms: what was actually copied
	 * into the reserved image region.
	 */
	img_size = md->boot.loaded_size;
	if (!img_size)
		img_size = md->boot.md_img_size;
	if (!img_size)
		img_size = md->img_size;
	if (img_size > md->img_size) {
		ccci_md_err(md, "image size 0x%zx exceeds region 0x%zx\n",
			    img_size, md->img_size);
		return -EINVAL;
	}
	rom_end = ccci_mpu_region_end(md->img_phy, img_size);
	rw_start = rom_end + 1;
	rw_end = md->img_phy + md->img_size - 1;
	if (rw_end <= rom_end) {
		ccci_md_err(md, "MD region 0x%zx too small for image 0x%zx\n",
			    md->img_size, img_size);
		return -ERANGE;
	}

	/*
	 * While booting the modem may read AP memory, so the AP region is only
	 * MD-read-only here (ccci_platform.c:373-381, 426-430); it is
	 * re-protected by ccci_mpu_set_ap_region_protection() right before the
	 * runtime handshake.  The AP window is [PHYS_OFFSET, PHYS_OFFSET + DRAM
	 * size) downstream (get_phys_offset()/get_max_DRAM_size(),
	 * ccci_platform.c:373-381); memblock mirrors both on this tree.
	 */
	ap_start = memblock_start_of_DRAM();
	ap_size = memblock_end_of_DRAM() - ap_start;
	if (!ap_size) {
		ccci_md_err(md, "no DRAM, skip AP region protection\n");
		return -ENODEV;
	}

	/* Downstream ignores the SMC status and programs all four regions;
	 * keep that behaviour but report the first failure.
	 */
	ret = ccci_mpu_program(md, "MD ROM", md->img_phy, rom_end, rom_id,
			       rom_perm);
	if (ret && !first)
		first = ret;
	ret = ccci_mpu_program(md, "MD RW", rw_start, rw_end, rw_id, rw_perm);
	if (ret && !first)
		first = ret;
	ret = ccci_mpu_program(md, "MD SMEM", md->smem_phy,
			       ccci_mpu_region_end(md->smem_phy,
						   md->smem_size),
			       shr_id, shr_perm);
	if (ret && !first)
		first = ret;
	ret = ccci_mpu_program(md, "AP", ap_start, ap_start + ap_size - 1,
			       MPU_REGION_ID_AP, MPU_PERM_AP_MD1_RO);
	if (ret && !first)
		first = ret;

	return first;
}

/*
 * Re-protect the whole AP DRAM window as MD-forbidden.
 * Downstream ccci_set_ap_region_protection() (ccci_platform.c:221-249, ROM
 * 0xc052f7cc): [kernel_base, kernel_base + dram_size - 1], region 15,
 * permission MPU_ACCESS_PERMISSON_AP_ATTR (0x0A28B68).  Called right before
 * the runtime data is sent (modem_cldma.c:3226 and 3275), when the modem no
 * longer needs to read AP memory.
 */
int ccci_mpu_set_ap_region_protection(struct ccci_md *md)
{
	phys_addr_t start = memblock_start_of_DRAM();
	size_t size = memblock_end_of_DRAM() - start;

	if (!size) {
		ccci_md_err(md, "no DRAM, skip AP region protection\n");
		return -ENODEV;
	}
	return ccci_mpu_program(md, "AP", start, start + size - 1,
				MPU_REGION_ID_AP, MPU_PERM_AP);
}

/* ------------------------------------------------------------------ */
/* RMPU memory remap windows                                           */
/* ------------------------------------------------------------------ */
/*
 * Each 32-bit BANKn_MAPx register holds four remap slots; a slot is one byte
 * carrying bits 31..24 of the 32 MiB-aligned target address, with bit 0 of
 * the byte as the "slot valid" flag.  The packing below is a literal
 * transcription of ccci_platform.c:497-504 (and of the ROM, e.g.
 * 0xc0530160-0xc05301c4 for the MD2/MD3 share-memory window).
 */
static u32 ccci_mpu_pack0(u32 addr) /* byte 0: addr[31:24] */
{
	return ((addr >> 24) | 0x1) & 0xFF;
}

static u32 ccci_mpu_pack1(u32 addr) /* byte 1: addr[31:24] << 8 */
{
	return (((addr >> 16) | (1 << 8)) & 0xFF00);
}

static u32 ccci_mpu_pack2(u32 addr) /* byte 2: addr[31:24] << 16 */
{
	return (((addr >> 8) | (1 << 16)) & 0xFF0000);
}

static u32 ccci_mpu_pack3(u32 addr) /* byte 3: addr[31:24] << 24 */
{
	return (((addr >> 0) | (1 << 24)) & 0xFF000000);
}

/* MD1 share-memory/ROM window: 8 consecutive 32 MiB slots from @des
 * (ccci_platform.c:496-509, 567-580; ROM 0xc053028c-0xc0530330 for BANK4 and
 * 0xc05305c4-0xc0530660 for BANK0).
 */
static void ccci_mpu_md1_words(u32 des, u32 *w0, u32 *w1)
{
	*w0 = ccci_mpu_pack0(des) +
	      ccci_mpu_pack1(des + MD_REMAP_SLOT_SIZE * 1) +
	      ccci_mpu_pack2(des + MD_REMAP_SLOT_SIZE * 2) +
	      ccci_mpu_pack3(des + MD_REMAP_SLOT_SIZE * 3);
	*w1 = ccci_mpu_pack0(des + MD_REMAP_SLOT_SIZE * 4) +
	      ccci_mpu_pack1(des + MD_REMAP_SLOT_SIZE * 5) +
	      ccci_mpu_pack2(des + MD_REMAP_SLOT_SIZE * 6) +
	      ccci_mpu_pack3(des + MD_REMAP_SLOT_SIZE * 7);
}

/* MD2/MD3 share-memory window: slot 0 is the smem, the rest is invalid
 * memory (ccci_platform.c:511-539).
 */
static void ccci_mpu_md2md3_smem_words(u32 des, u32 invalid, u32 *w0, u32 *w1)
{
	*w0 = ccci_mpu_pack0(des) +
	      ccci_mpu_pack1(invalid + MD_REMAP_SLOT_SIZE * 0) +
	      ccci_mpu_pack2(invalid + MD_REMAP_SLOT_SIZE * 1) +
	      ccci_mpu_pack3(invalid + MD_REMAP_SLOT_SIZE * 2);
	*w1 = ccci_mpu_pack0(invalid + MD_REMAP_SLOT_SIZE * 3) +
	      ccci_mpu_pack1(invalid + MD_REMAP_SLOT_SIZE * 4) +
	      ccci_mpu_pack2(invalid + MD_REMAP_SLOT_SIZE * 5) +
	      ccci_mpu_pack3(invalid + MD_REMAP_SLOT_SIZE * 6);
}

/* MD2/MD3 image window: 3 image slots, the rest is invalid memory
 * (ccci_platform.c:582-618).
 */
static void ccci_mpu_md2md3_rom_words(u32 des, u32 invalid, u32 *w0, u32 *w1)
{
	*w0 = ccci_mpu_pack0(des) +
	      ccci_mpu_pack1(des + MD_REMAP_SLOT_SIZE * 1) +
	      ccci_mpu_pack2(des + MD_REMAP_SLOT_SIZE * 2) +
	      ccci_mpu_pack3(invalid + MD_REMAP_SLOT_SIZE * 7);
	*w1 = ccci_mpu_pack0(invalid + MD_REMAP_SLOT_SIZE * 8) +
	      ccci_mpu_pack1(invalid + MD_REMAP_SLOT_SIZE * 9) +
	      ccci_mpu_pack2(invalid + MD_REMAP_SLOT_SIZE * 10) +
	      ccci_mpu_pack3(invalid + MD_REMAP_SLOT_SIZE * 11);
}

static void ccci_mpu_write_window(struct ccci_md *md, u32 reg, u32 val)
{
	/* downstream mt_reg_sync_writel(): write + mb() (sync_write.h) */
	writel(val, md->infra_ao + reg);
	mb();
}

/*
 * Program the MD1/MD2/MD3 BANK0 and BANK4 remap windows and publish the
 * AP->MD share-memory offset, as downstream ccci_set_mem_remap()
 * (ccci_platform.c:622-649, ROM 0xc05306cc) does:
 *
 *   remainder   = smem_offset % 32 MiB
 *   smem_offset_AP_to_MD = smem_region_phy - (remainder + KERN_EMI_BASE)
 *   des_smem    = md_region_phy + smem_offset - remainder (- KERN_EMI_BASE)
 *   des_rom     = md_region_phy                             (- KERN_EMI_BASE)
 *   invalid     = ALIGN(md_region_phy + md_region_size + smem_region_size,
 *                       32 MiB) - KERN_EMI_BASE
 *
 * with smem_offset = smem_region_phy - md_region_phy (downstream caller
 * ccci_core.c:311).  The "- KERN_EMI_BASE" branches are the non-4G path:
 * downstream only takes the other one with FEATURE_USING_4G_MEMORY_API,
 * which requires CONFIG_MTK_LM_MODE (ccci_config.h:49-51) and that is off in
 * arch/arm/configs/grandpplte_00_defconfig:1261.
 *
 * ROM proof of the published offset and of the argument order:
 *   0xc05306e4  bic   r2, r1, #0xFE000000      r2 = remainder
 *   0xc05306e8  add   r1, r1, r3               r1 = smem_offset + md_phy
 *   0xc05306f0  add   r12, r12, #-0x40000000   smem_phy - KERN_EMI_BASE
 *   0xc05306f8  rsb   r12, r2, r12             - remainder
 *   0xc0530700  str   r12, [r0, #0x814]        smem_offset_AP_to_MD
 *   0xc05306fc  rsb   r2, r2, r1               des_smem
 *   0xc0530704  mov   r1, #0x40000000          src
 *   0xc053070c  bl    set_md_smem_remap
 *   0xc0530740  ldr   r2, [r4, #0x7f4]         des_rom = md_region_phy
 *   0xc0530744  mov   r1, #0               0    src
 *   0xc0530748  bl    set_md_rom_rw_mem_remap
 */
int ccci_mpu_set_mem_remap(struct ccci_md *md)
{
	phys_addr_t layout_end, off;
	unsigned long smem_offset, remainder;
	u32 invalid, smem_des, img_des, w0, w1;

	/* publish "no remap" until the windows are actually written */
	md->smem_offset_ap_to_md = 0;
	if (!md->infra_ao) {
		ccci_md_err(md, "no INFRA_AO window, skip memory remap\n");
		return -ENODEV;
	}
	if (!md->img_present || !md->smem_vir) {
		ccci_md_err(md, "no image/smem region, skip memory remap\n");
		return -ENODEV;
	}
	if (md->smem_phy < md->img_phy) {
		ccci_md_err(md, "smem %pa below image %pa\n", &md->smem_phy,
			    &md->img_phy);
		return -EINVAL;
	}
	if (md->img_phy < KERN_EMI_BASE) {
		ccci_md_err(md, "image %pa below KERN_EMI_BASE\n", &md->img_phy);
		return -EINVAL;
	}

	smem_offset = md->smem_phy - md->img_phy;
	remainder = smem_offset % MD_REMAP_SLOT_SIZE;
	layout_end = md->img_phy + md->img_size + md->smem_size;
	off = md->smem_phy - remainder - KERN_EMI_BASE;
	if ((layout_end >> 32) ||
	    ALIGN(layout_end, MD_REMAP_SLOT_SIZE) <= KERN_EMI_BASE ||
	    (off >> 32)) {
		ccci_md_err(md, "cannot build MD view for layout 0x%llx\n",
			    (unsigned long long)layout_end);
		return -ERANGE;
	}

	invalid = (u32)(ALIGN(layout_end, MD_REMAP_SLOT_SIZE) - KERN_EMI_BASE);
	smem_des = (u32)off;
	img_des = (u32)(md->img_phy - KERN_EMI_BASE);

	switch (md->id) {
	case CCCI_MD_SYS1:
		ccci_mpu_md1_words(smem_des, &w0, &w1);
		ccci_mpu_write_window(md, MD1_BANK4_MAP0, w0);
		ccci_mpu_write_window(md, MD1_BANK4_MAP1, w1);

		ccci_mpu_md1_words(img_des, &w0, &w1);
		ccci_mpu_write_window(md, MD1_BANK0_MAP0, w0);
		ccci_mpu_write_window(md, MD1_BANK0_MAP1, w1);
		break;
	case CCCI_MD_SYS2:
		ccci_mpu_md2md3_smem_words(smem_des, invalid, &w0, &w1);
		ccci_mpu_write_window(md, MD2_BANK4_MAP0, w0);
		ccci_mpu_write_window(md, MD2_BANK4_MAP1, w1);

		ccci_mpu_md2md3_rom_words(img_des, invalid, &w0, &w1);
		ccci_mpu_write_window(md, MD2_BANK0_MAP0, w0);
		ccci_mpu_write_window(md, MD2_BANK0_MAP1, w1);
		break;
	case CCCI_MD_SYS3:
		ccci_mpu_md2md3_smem_words(smem_des, invalid, &w0, &w1);
		ccci_mpu_write_window(md, MD3_BANK4_MAP0, w0);
		ccci_mpu_write_window(md, MD3_BANK4_MAP1, w1);

		ccci_mpu_md2md3_rom_words(img_des, invalid, &w0, &w1);
		ccci_mpu_write_window(md, MD3_BANK0_MAP0, w0);
		ccci_mpu_write_window(md, MD3_BANK0_MAP1, w1);
		break;
	default:
		ccci_md_err(md, "invalid MD id for memory remap\n");
		return -EINVAL;
	}
	mb();

	md->smem_offset_ap_to_md = (u32)off;
	ccci_md_info(md,
		     "remap smem_offset 0x%lx remainder 0x%lx AP->MD 0x%x invalid 0x%08x\n",
		     smem_offset, remainder, md->smem_offset_ap_to_md, invalid);
	return 0;
}

/*
 * Convert an AP-view physical address into the modem's view, i.e. subtract
 * mem_layout.smem_offset_AP_to_MD (downstream
 * modem_cldma.c:3201, 3205, 3319-3327, 3410).  Returns @addr unchanged when
 * no remap was programmed or when the offset would underflow, so a caller
 * never publishes a wrapped address.
 */
u32 ccci_mpu_ap_to_md(struct ccci_md *md, phys_addr_t addr)
{
	phys_addr_t off = md->smem_offset_ap_to_md;

	if (!off)
		return (u32)addr;
	if (off > addr) {
		ccci_md_err(md, "AP->MD offset 0x%llx above 0x%llx, keep AP view\n",
			    (unsigned long long)off,
			    (unsigned long long)addr);
		return (u32)addr;
	}
	return (u32)(addr - off);
}
