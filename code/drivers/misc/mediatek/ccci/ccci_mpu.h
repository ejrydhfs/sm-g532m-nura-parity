/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6735/MT6737T modem memory protection: EMI MPU (programmed through ATF
 * SMC), RMPU (MD memory remap windows) and the modem image-security state.
 *
 * The public entry point used by other CCCI users, ccci_md_set_dsp_mpu_protection(),
 * is declared in include/linux/ccci.h (frozen gap API); everything else here is
 * private to drivers/misc/mediatek/ccci.
 *
 * Port from Samsung/MTK downstream
 *   drivers/misc/mediatek/eccci/mt6735/ccci_platform.c
 *   drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/emi_mpu.h
 *   drivers/misc/mediatek/include/mt-plat/mt6735/include/mach/mt_secure_api.h
 *   drivers/misc/mediatek/ccci_util/ccci_util_lib_load_img.c
 *
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 */
#ifndef __CCCI_MPU_H__
#define __CCCI_MPU_H__

#include <linux/types.h>

struct ccci_md;

/* Resolve the MPU backend (EL3/ATF SMC conduit) and reset the DSP region.
 * Called once from ccci_md_register(), i.e. before the INFRA_AO-dependent
 * paths can run and before any ccci_md_start().
 */
int ccci_mpu_init(struct ccci_md *md);

/* Clear the MD ROM/RW regions and, for MD1, the DSP region: downstream
 * ccci_clear_md_region_protection() + ccci_clear_dsp_region_protection().
 */
void ccci_mpu_clear_md_protection(struct ccci_md *md);

/* Program the MD ROM/RW/SMEM regions plus the AP region as MD-read-only.
 * Must run after the image is in DRAM and before the modem is released
 * (downstream md_cd_start() step 3).
 */
int ccci_mpu_set_md_protection(struct ccci_md *md);

/* Program the MD1_DSP region from the image header's DSP slot; @loaded
 * selects the post-DSP-load attribute.  Returns 0 when the image has no DSP
 * sub-image (the MT6735/MT6737T case).
 */
int ccci_mpu_dsp_protection(struct ccci_md *md, bool loaded);

/* Re-protect the whole AP DRAM window as MD-forbidden; called right before
 * runtime data is sent (downstream ccci_set_ap_region_protection()).
 */
int ccci_mpu_set_ap_region_protection(struct ccci_md *md);

/* Program the RMPU BANK0/BANK4 windows and publish the AP->MD shared-memory
 * offset in md->smem_offset_ap_to_md (0 when nothing was programmed).
 */
int ccci_mpu_set_mem_remap(struct ccci_md *md);

/* Convert an AP-view address to the modem's view using
 * md->smem_offset_ap_to_md (no-op before the remap windows are programmed).
 */
u32 ccci_mpu_ap_to_md(struct ccci_md *md, phys_addr_t addr);

/* Report the modem image-security state (ROM ccci_init_security()).
 * Downstream returns 0 unconditionally; the kernel performs no image
 * authentication, LK/ATF/the MD boot ROM do.
 */
int ccci_init_security(struct ccci_md *md);

#endif /* __CCCI_MPU_H__ */
