/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * MediaTek MT6735 camera ISP userspace ABI.
 *
 * Ported from the Samsung/MediaTek downstream GPL kernel header
 * drivers/misc/mediatek/cameraisp/src/mt6735/inc/camera_isp_D1.h
 * (MTK proprietary notice: Copyright (C) 2015 MediaTek Inc., GPL-2.0).
 *
 * The ioctl command numbers, enumeration values, structure layouts and
 * semantics are kept identical to the downstream "libcamdrv" ABI so that a
 * ported camera userspace stack keeps working.  The only deliberate change
 * is that user-space pointers inside the structures are declared as __u64
 * instead of "pointer" types.  This makes the ABI independent of the
 * userspace pointer size: the kernel this header targets is arm64, while the
 * downstream ABI was compiled for 32-bit ARM.  A 32-bit client may still use
 * this header because the pointer fields keep their 8-byte layout.
 *
 * The kernel side of the ABI is delivered by
 * drivers/media/platform/mediatek/isp/mtk-isp.c, which registers the
 * misc character device documented by MTK_ISP_DEV_NAME below.
 */

#ifndef _UAPI_LINUX_MTK_ISP_H
#define _UAPI_LINUX_MTK_ISP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Downstream ISP_DEV_NAME, camera_isp_D1.c:116 */
#define MTK_ISP_DEV_NAME		"camera-isp"

/* Downstream ISP_MAGIC, camera_isp_D1.h */
#define MTK_ISP_MAGIC			'k'

/*
 * mmap(2): the page offset is the physical base address of one of the
 * register windows below.  This is the downstream ISP_mmap() contract
 * (camera_isp_D1.c:11263-11335).  Only these windows and their exact lengths
 * are accepted.
 */
#define MTK_ISP_MMAP_IMGSYS		0x15000000
#define MTK_ISP_MMAP_IMGSYS_SIZE	0x10000
#define MTK_ISP_MMAP_ISP		0x15004000
#define MTK_ISP_MMAP_ISP_SIZE		0x4000		/* ISP base 0x15004000..0x15007fff */
#define MTK_ISP_MMAP_SENINF		0x15008000
#define MTK_ISP_MMAP_SENINF_SIZE	0x4000
#define MTK_ISP_MMAP_MIPIRX_CFG		0x1500c000
#define MTK_ISP_MMAP_MIPIRX_CFG_SIZE	0x1000
#define MTK_ISP_MMAP_ISP_INNER		0x1500d000
#define MTK_ISP_MMAP_ISP_INNER_SIZE	0x1000
#define MTK_ISP_MMAP_PLL		0x10000000
#define MTK_ISP_MMAP_PLL_SIZE		0x1000
#define MTK_ISP_MMAP_GPIO		0x10211000
#define MTK_ISP_MMAP_GPIO_SIZE		0x1000
#define MTK_ISP_MMAP_MIPIRX_ANA		0x10215000
#define MTK_ISP_MMAP_MIPIRX_ANA_SIZE	0x3000

/* ------------------------------------------------------------------ */
/* Interrupt status bits (camera_isp_D1.h)                            */
/* ------------------------------------------------------------------ */

/* CAM_CTL_INT_P1_STATUS */
#define ISP_IRQ_P1_STATUS_VS1_INT_ST			(1U << 0)
#define ISP_IRQ_P1_STATUS_TG1_INT1_ST			(1U << 1)
#define ISP_IRQ_P1_STATUS_TG1_INT2_ST			(1U << 2)
#define ISP_IRQ_P1_STATUS_EXPDON1_ST			(1U << 3)
#define ISP_IRQ_P1_STATUS_TG1_ERR_ST			(1U << 4)
#define ISP_IRQ_P1_STATUS_TG1_GBERR			(1U << 5)
#define ISP_IRQ_P1_STATUS_CQ0_ERR			(1U << 6)
#define ISP_IRQ_P1_STATUS_CQ0_VS_ERR_ST			(1U << 7)
#define ISP_IRQ_P1_STATUS_IMGO_DROP_FRAME_ST		(1U << 8)
#define ISP_IRQ_P1_STATUS_RRZO_DROP_FRAME_ST		(1U << 9)
#define ISP_IRQ_P1_STATUS_PASS1_DON_ST			(1U << 10)
#define ISP_IRQ_P1_STATUS_SOF1_INT_ST			(1U << 12)
#define ISP_IRQ_P1_STATUS_PESUDO_P1_DON_ST		(1U << 15)
#define ISP_IRQ_P1_STATUS_AF_DON_ST			(1U << 16)
#define ISP_IRQ_P1_STATUS_FLK_DON_ST			(1U << 17)
#define ISP_IRQ_P1_STATUS_IMGO_ERR_ST			(1U << 20)
#define ISP_IRQ_P1_STATUS_AAO_ERR_ST			(1U << 21)
#define ISP_IRQ_P1_STATUS_LCSO_ERR_ST			(1U << 22)
#define ISP_IRQ_P1_STATUS_RRZO_ERR_ST			(1U << 23)
#define ISP_IRQ_P1_STATUS_ESFKO_ERR_ST			(1U << 24)
#define ISP_IRQ_P1_STATUS_FLK_ERR_ST			(1U << 25)
#define ISP_IRQ_P1_STATUS_LSC_ERR_ST			(1U << 26)
#define ISP_IRQ_P1_STATUS_FBC_RRZO_DON_ST		(1U << 27)
#define ISP_IRQ_P1_STATUS_FBC_IMGO_DON_ST		(1U << 28)
#define ISP_IRQ_P1_STATUS_DMA_ERR_ST			(1U << 30)

/* CAM_CTL_INT_P1_STATUS2 */
#define ISP_IRQ_P1_STATUS2_IMGO_DONE_ST			(1U << 0)
#define ISP_IRQ_P1_STATUS2_UFEO_DONE_ST			(1U << 1)
#define ISP_IRQ_P1_STATUS2_RRZO_DONE_ST			(1U << 2)
#define ISP_IRQ_P1_STATUS2_ESFKO_DONE_ST		(1U << 3)
#define ISP_IRQ_P1_STATUS2_LCSO_DONE_ST			(1U << 4)
#define ISP_IRQ_P1_STATUS2_AAO_DONE_ST			(1U << 5)
#define ISP_IRQ_P1_STATUS2_BPCI_DONE_ST			(1U << 6)
#define ISP_IRQ_P1_STATUS2_LSCI_DONE_ST			(1U << 7)
#define ISP_IRQ_P1_STATUS2_AF_TAR_DONE_ST		(1U << 8)
#define ISP_IRQ_P1_STATUS2_AF_FLO1_DONE_ST		(1U << 9)
#define ISP_IRQ_P1_STATUS2_AF_FLO2_DONE_ST		(1U << 10)
#define ISP_IRQ_P1_STATUS2_AF_FLO3_DONE_ST		(1U << 11)

/* CAM_CTL_INT_P1_STATUS_D (Pass-1 "D"/second TG, same bit layout as P1) */
#define ISP_IRQ_P1_STATUS_D_VS1_INT_ST			(1U << 0)
#define ISP_IRQ_P1_STATUS_D_TG1_INT1_ST			(1U << 1)
#define ISP_IRQ_P1_STATUS_D_TG1_INT2_ST			(1U << 2)
#define ISP_IRQ_P1_STATUS_D_EXPDON1_ST			(1U << 3)
#define ISP_IRQ_P1_STATUS_D_TG1_ERR_ST			(1U << 4)
#define ISP_IRQ_P1_STATUS_D_TG1_GBERR			(1U << 5)
#define ISP_IRQ_P1_STATUS_D_CQ0_ERR			(1U << 6)
#define ISP_IRQ_P1_STATUS_D_CQ0_VS_ERR_ST		(1U << 7)
#define ISP_IRQ_P1_STATUS_D_IMGO_DROP_FRAME_ST		(1U << 8)
#define ISP_IRQ_P1_STATUS_D_RRZO_DROP_FRAME_ST		(1U << 9)
#define ISP_IRQ_P1_STATUS_D_PASS1_DON_ST		(1U << 10)
#define ISP_IRQ_P1_STATUS_D_SOF1_INT_ST			(1U << 12)
#define ISP_IRQ_P1_STATUS_D_PESUDO_P1_DON_ST		(1U << 15)
#define ISP_IRQ_P1_STATUS_D_AF_DON_ST			(1U << 16)
#define ISP_IRQ_P1_STATUS_D_IMGO_ERR_ST			(1U << 20)
#define ISP_IRQ_P1_STATUS_D_AAO_ERR_ST			(1U << 21)
#define ISP_IRQ_P1_STATUS_D_LCSO_ERR_ST			(1U << 22)
#define ISP_IRQ_P1_STATUS_D_RRZO_ERR_ST			(1U << 23)
#define ISP_IRQ_P1_STATUS_D_AFO_ERR_ST			(1U << 24)
#define ISP_IRQ_P1_STATUS_D_LSC_ERR_ST			(1U << 26)
#define ISP_IRQ_P1_STATUS_D_FBC_RRZO_DON_ST		(1U << 27)
#define ISP_IRQ_P1_STATUS_D_FBC_IMGO_DON_ST		(1U << 28)
#define ISP_IRQ_P1_STATUS_D_DMA_ERR_ST			(1U << 30)

/* CAM_CTL_INT_STATUSX (P1 and P2) */
#define ISP_IRQ_STATUSX_VS1_INT_ST			(1U << 0)
#define ISP_IRQ_STATUSX_TG1_INT1_ST			(1U << 1)
#define ISP_IRQ_STATUSX_TG1_INT2_ST			(1U << 2)
#define ISP_IRQ_STATUSX_EXPDON1_ST			(1U << 3)
#define ISP_IRQ_STATUSX_TG1_ERR_ST			(1U << 4)
#define ISP_IRQ_STATUSX_TG1_GBERR			(1U << 5)
#define ISP_IRQ_STATUSX_CQ0_ERR				(1U << 6)
#define ISP_IRQ_STATUSX_CQ0_VS_ERR_ST			(1U << 7)
#define ISP_IRQ_STATUSX_IMGO_DROP_FRAME_ST		(1U << 8)
#define ISP_IRQ_STATUSX_RRZO_DROP_FRAME_ST		(1U << 9)
#define ISP_IRQ_STATUSX_PASS1_DON_ST			(1U << 10)
#define ISP_IRQ_STATUSX_SOF1_INT_ST			(1U << 12)
#define ISP_IRQ_STATUSX_CQ_ERR_ST			(1U << 13)
#define ISP_IRQ_STATUSX_PASS2_DON_ST			(1U << 14)
#define ISP_IRQ_STATUSX_TILE_DON_ST			(1U << 15)
#define ISP_IRQ_STATUSX_AF_DON_ST			(1U << 16)
#define ISP_IRQ_STATUSX_FLK_DON_ST			(1U << 17)
#define ISP_IRQ_STATUSX_CQ_DON_ST			(1U << 19)
#define ISP_IRQ_STATUSX_IMGO_ERR_ST			(1U << 20)
#define ISP_IRQ_STATUSX_AAO_ERR_ST			(1U << 21)
#define ISP_IRQ_STATUSX_LCSO_ERR_ST			(1U << 22)
#define ISP_IRQ_STATUSX_RRZO_ERR_ST			(1U << 23)
#define ISP_IRQ_STATUSX_ESFKO_ERR_ST			(1U << 24)
#define ISP_IRQ_STATUSX_FLK_ERR_ST			(1U << 25)
#define ISP_IRQ_STATUSX_LSC_ERR_ST			(1U << 26)
#define ISP_IRQ_STATUSX_FBC_RRZO_DON_ST			(1U << 27)
#define ISP_IRQ_STATUSX_FBC_IMGO_DON_ST			(1U << 28)
#define ISP_IRQ_STATUSX_DMA_ERR_ST			(1U << 30)

/* CAM_CTL_INT_P2_STATUS */
#define ISP_IRQ_P2_STATUS_CQ_ERR_ST			(1U << 0)
#define ISP_IRQ_P2_STATUS_PASS2_DON_ST			(1U << 1)
#define ISP_IRQ_P2_STATUS_TILE_DON_ST			(1U << 2)
#define ISP_IRQ_P2_STATUS_CQ_DON_ST			(1U << 3)
#define ISP_IRQ_P2_STATUS_TDR_ERR_ST			(1U << 4)
#define ISP_IRQ_P2_STATUS_PASS2A_DON_ST			(1U << 5)
#define ISP_IRQ_P2_STATUS_PASS2B_DON_ST			(1U << 6)
#define ISP_IRQ_P2_STATUS_PASS2C_DON_ST			(1U << 7)
#define ISP_IRQ_P2_STATUS_CQ1_DONE_ST			(1U << 8)
#define ISP_IRQ_P2_STATUS_CQ2_DONE_ST			(1U << 9)
#define ISP_IRQ_P2_STATUS_CQ3_DONE_ST			(1U << 10)
#define ISP_IRQ_P2_STATUS_PASS2A_ERR_TRIG_ST		(1U << 11)
#define ISP_IRQ_P2_STATUS_PASS2B_ERR_TRIG_ST		(1U << 12)
#define ISP_IRQ_P2_STATUS_PASS2C_ERR_TRIG_ST		(1U << 13)
#define ISP_IRQ_P2_STATUS_IMGI_DONE_ST			(1U << 15)
#define ISP_IRQ_P2_STATUS_UFDI_DONE_ST			(1U << 16)
#define ISP_IRQ_P2_STATUS_VIPI_DONE_ST			(1U << 17)
#define ISP_IRQ_P2_STATUS_VIP2I_DONE_ST			(1U << 18)
#define ISP_IRQ_P2_STATUS_VIP3I_DONE_ST			(1U << 19)
#define ISP_IRQ_P2_STATUS_LCEI_DONE_ST			(1U << 20)
#define ISP_IRQ_P2_STATUS_MFBO_DONE_ST			(1U << 21)
#define ISP_IRQ_P2_STATUS_IMG2O_DONE_ST			(1U << 22)
#define ISP_IRQ_P2_STATUS_IMG3O_DONE_ST			(1U << 23)
#define ISP_IRQ_P2_STATUS_IMG3BO_DONE_ST		(1U << 24)
#define ISP_IRQ_P2_STATUS_IMG3CO_DONE_ST		(1U << 25)
#define ISP_IRQ_P2_STATUS_FEO_DONE_ST			(1U << 26)

/* CAM_CTL_INT_STATUS3X (all DMAs) */
#define ISP_IRQ_STATUS3X_IMGO_DONE_ST			(1U << 0)
#define ISP_IRQ_STATUS3X_UFEO_DONE_ST			(1U << 1)
#define ISP_IRQ_STATUS3X_RRZO_DONE_ST			(1U << 2)
#define ISP_IRQ_STATUS3X_ESFKO_DONE_ST			(1U << 3)
#define ISP_IRQ_STATUS3X_LCSO_DONE_ST			(1U << 4)
#define ISP_IRQ_STATUS3X_AAO_DONE_ST			(1U << 5)
#define ISP_IRQ_STATUS3X_BPCI_DONE_ST			(1U << 6)
#define ISP_IRQ_STATUS3X_LSCI_DONE_ST			(1U << 7)
#define ISP_IRQ_STATUS3X_IMGO_D_DONE_ST			(1U << 8)
#define ISP_IRQ_STATUS3X_RRZO_D_DONE_ST			(1U << 9)
#define ISP_IRQ_STATUS3X_AFO_D_DONE_ST			(1U << 10)
#define ISP_IRQ_STATUS3X_LCSO_D_DONE_ST			(1U << 11)
#define ISP_IRQ_STATUS3X_AAO_D_DONE_ST			(1U << 12)
#define ISP_IRQ_STATUS3X_BPCI_D_DONE_ST			(1U << 13)
#define ISP_IRQ_STATUS3X_LCSI_D_DONE_ST			(1U << 14)
#define ISP_IRQ_STATUS3X_IMGI_DONE_ST			(1U << 15)
#define ISP_IRQ_STATUS3X_UFDI_DONE_ST			(1U << 16)
#define ISP_IRQ_STATUS3X_VIPI_DONE_ST			(1U << 17)
#define ISP_IRQ_STATUS3X_VIP2I_DONE_ST			(1U << 18)
#define ISP_IRQ_STATUS3X_VIP3I_DONE_ST			(1U << 19)
#define ISP_IRQ_STATUS3X_LCEI_DONE_ST			(1U << 20)
#define ISP_IRQ_STATUS3X_MFBO_DONE_ST			(1U << 21)
#define ISP_IRQ_STATUS3X_IMG2O_DONE_ST			(1U << 22)
#define ISP_IRQ_STATUS3X_IMG3O_DONE_ST			(1U << 23)
#define ISP_IRQ_STATUS3X_IMG3BO_DONE_ST			(1U << 24)
#define ISP_IRQ_STATUS3X_IMG3CO_DONE_ST			(1U << 25)
#define ISP_IRQ_STATUS3X_FEO_DONE_ST			(1U << 26)

/* SENINF1..4_IRQ_INTSTA */
#define ISP_IRQ_SENINF_OVERRUN_STA			(1U << 0)
#define ISP_IRQ_SENINF_CRCERR_STA			(1U << 1)
#define ISP_IRQ_SENINF_FSMERR_STA			(1U << 2)
#define ISP_IRQ_SENINF_VSIZEERR_STA			(1U << 3)
#define ISP_IRQ_SENINF_HSIZEERR_STA			(1U << 4)
#define ISP_IRQ_SENINF_SENSOR_VSIZEERR_STA		(1U << 5)
#define ISP_IRQ_SENINF_SENSOR_HSIZEERR_STA		(1U << 6)

/* REG_CAMSV_INT_STATUS / REG_ISP_IRQ_CAMSV2_STATUS */
#define ISP_IRQ_CAMSV_STATUS_VS1_ST			(1U << 0)
#define ISP_IRQ_CAMSV_STATUS_TG_ST1			(1U << 1)
#define ISP_IRQ_CAMSV_STATUS_TG_ST2			(1U << 2)
#define ISP_IRQ_CAMSV_STATUS_EXPDON1_ST			(1U << 3)
#define ISP_IRQ_CAMSV_STATUS_TG_ERR_ST			(1U << 4)
#define ISP_IRQ_CAMSV_STATUS_TG_GBERR_ST		(1U << 5)
#define ISP_IRQ_CAMSV_STATUS_TG_DROP_ST			(1U << 6)
#define ISP_IRQ_CAMSV_STATUS_TG_SOF1_ST			(1U << 7)
#define ISP_IRQ_CAMSV_STATUS_PASS1_DON_ST		(1U << 10)
#define ISP_IRQ_CAMSV_STATUS_IMGO_ERR_ST		(1U << 16)
#define ISP_IRQ_CAMSV_STATUS_IMGO_OVERR_ST		(1U << 17)
#define ISP_IRQ_CAMSV_STATUS_IMGO_DROP_ST		(1U << 19)

/* ------------------------------------------------------------------ */
/* Enumerations                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
	ISP_IRQ_CLEAR_NONE,
	ISP_IRQ_CLEAR_WAIT,
	ISP_IRQ_CLEAR_STATUS,
	ISP_IRQ_CLEAR_ALL
} ISP_IRQ_CLEAR_ENUM;

typedef enum {
	ISP_IRQ_TYPE_INT_P1_ST,		/* P1 */
	ISP_IRQ_TYPE_INT_P1_ST2,	/* P1 DMA */
	ISP_IRQ_TYPE_INT_P1_ST_D,	/* P1_D */
	ISP_IRQ_TYPE_INT_P1_ST2_D,	/* P1 DMA_D */
	ISP_IRQ_TYPE_INT_P2_ST,		/* P2 */
	ISP_IRQ_TYPE_INT_STATUSX,	/* STATUSX, P1 and P2 */
	ISP_IRQ_TYPE_INT_STATUS2X,	/* STATUS2X, P1_D */
	ISP_IRQ_TYPE_INT_STATUS3X,	/* STATUS3X, all DMA */
	ISP_IRQ_TYPE_ISP_AMOUNT,
	ISP_IRQ_TYPE_INT_SENINF1 = ISP_IRQ_TYPE_ISP_AMOUNT,
	ISP_IRQ_TYPE_INT_SENINF2,
	ISP_IRQ_TYPE_INT_SENINF3,
	ISP_IRQ_TYPE_INT_SENINF4,
	ISP_IRQ_TYPE_SENINF_AMOUNT,
	ISP_IRQ_TYPE_INT_CAMSV = ISP_IRQ_TYPE_SENINF_AMOUNT,
	ISP_IRQ_TYPE_INT_CAMSV2,
	ISP_IRQ_TYPE_AMOUNT
} ISP_IRQ_TYPE_ENUM;

typedef enum {
	ISP_IRQ_USER_ISPDRV = 0,
	ISP_IRQ_USER_MW = 1,
	ISP_IRQ_USER_3A = 2,
	ISP_IRQ_USER_HWSYNC = 3,
	ISP_IRQ_USER_ACDK = 4,
	ISP_IRQ_USER_EIS = 5,
	ISP_IRQ_USER_VHDR = 6,
	ISP_IRQ_USER_MAX
} ISP_IRQ_USER_ENUM;

typedef enum {			/* special user for specific operation */
	ISP_IRQ_WAITIRQ_SPEUSER_NONE = 0,
	ISP_IRQ_WAITIRQ_SPEUSER_EIS = 1,
	ISP_IRQ_WAITIRQ_SPEUSER_NUM
} ISP_IRQ_WAITIRQ_SPEUSER_ENUM;

typedef enum {
	ISP_HOLD_TIME_VD,
	ISP_HOLD_TIME_EXPDONE
} ISP_HOLD_TIME_ENUM;

typedef enum {
	/* Work queue. Interruptible, sleeping is allowed. */
	ISP_CALLBACK_WORKQUEUE_VD,
	ISP_CALLBACK_WORKQUEUE_EXPDONE,
	/* Tasklet. Not interruptible, sleeping is NOT allowed. */
	ISP_CALLBACK_TASKLET_VD,
	ISP_CALLBACK_TASKLET_EXPDONE,
	ISP_CALLBACK_AMOUNT
} ISP_CALLBACK_ENUM;

typedef enum {
	_cam_tg_ = 0,
	_cam_tg2_,
	_camsv_tg_,
	_camsv2_tg_,
	_cam_tg_max_
} _isp_tg_enum_;

typedef enum {
	_imgi_ = 0,
	_vipi_,
	_vip2i_,
	_vip3i_,
	_imgo_,
	_ufdi_,
	_lcei_,
	_ufeo_,
	_rrzo_,
	_imgo_d_,
	_rrzo_d_,
	_img2o_,
	_img3o_,
	_img3bo_,
	_img3co_,
	_camsv_imgo_,
	_camsv2_imgo_,
	_mfbo_,
	_feo_,
	_wrot_,
	_wdma_,
	_jpeg_,
	_rt_dma_max_
} _isp_dma_enum_;

typedef enum {
	ISP_RROCESSED_RAW,	/* 0 */
	ISP_PURE_RAW,		/* 1 */
} ISP_RAW_TYPE_ENUM;

typedef enum {
	ISP_RT_BUF_CTRL_ENQUE,		/* 0 */
	ISP_RT_BUF_CTRL_ENQUE_IMD,	/* 1, directly enqueue */
	ISP_RT_BUF_CTRL_EXCHANGE_ENQUE,	/* 2 */
	ISP_RT_BUF_CTRL_DEQUE,		/* 3 */
	ISP_RT_BUF_CTRL_IS_RDY,		/* 4 */
	ISP_RT_BUF_CTRL_DMA_EN,		/* 5 */
	ISP_RT_BUF_CTRL_GET_SIZE,	/* 6 */
	ISP_RT_BUF_CTRL_CLEAR,		/* 7 */
	ISP_RT_BUF_CTRL_CUR_STATUS,	/* 8 */
	ISP_RT_BUF_CTRL_MAX
} ISP_RT_BUF_CTRL_ENUM;

typedef enum {
	ISP_RTBC_STATE_INIT,	/* 0 */
	ISP_RTBC_STATE_SOF,
	ISP_RTBC_STATE_DONE,
	ISP_RTBC_STATE_MAX
} ISP_RTBC_STATE_ENUM;

typedef enum {
	ISP_RTBC_BUF_EMPTY,	/* 0 */
	ISP_RTBC_BUF_FILLED,	/* 1 */
	ISP_RTBC_BUF_LOCKED,	/* 2 */
} ISP_RTBC_BUF_STATE_ENUM;

typedef enum {
	ISP_REF_CNT_GET,	/* 0 */
	ISP_REF_CNT_INC,	/* 1 */
	ISP_REF_CNT_DEC,	/* 2 */
	ISP_REF_CNT_DEC_AND_RESET_P1_P2_IF_LAST_ONE,	/* 3 */
	ISP_REF_CNT_DEC_AND_RESET_P1_IF_LAST_ONE,	/* 4 */
	ISP_REF_CNT_DEC_AND_RESET_P2_IF_LAST_ONE,	/* 5 */
	ISP_REF_CNT_MAX
} ISP_REF_CNT_CTRL_ENUM;

typedef enum {
	ISP_REF_CNT_ID_IMEM,	/* 0 */
	ISP_REF_CNT_ID_ISP_FUNC,	/* 1 */
	ISP_REF_CNT_ID_GLOBAL_PIPE,	/* 2 */
	ISP_REF_CNT_ID_P1_PIPE,	/* 3 */
	ISP_REF_CNT_ID_P2_PIPE,	/* 4 */
	ISP_REF_CNT_ID_MAX,
} ISP_REF_CNT_ID_ENUM;

typedef enum {
	ISP_ED_BUFQUE_CTRL_ENQUE_FRAME = 0,
	ISP_ED_BUFQUE_CTRL_WAIT_DEQUE,		/* 1 */
	ISP_ED_BUFQUE_CTRL_DEQUE_SUCCESS,	/* 2 */
	ISP_ED_BUFQUE_CTRL_DEQUE_FAIL,		/* 3 */
	ISP_ED_BUFQUE_CTRL_WAIT_FRAME,		/* 4 */
	ISP_ED_BUFQUE_CTRL_WAKE_WAITFRAME,	/* 5 */
	ISP_ED_BUFQUE_CTRL_CLAER_ALL,		/* 6 */
	ISP_ED_BUFQUE_CTRL_MAX
} ISP_ED_BUFQUE_CTRL_ENUM;

typedef enum {
	ISP_ED_BUF_STATE_NONE = -1,
	ISP_ED_BUF_STATE_ENQUE = 0,
	ISP_ED_BUF_STATE_RUNNING,
	ISP_ED_BUF_STATE_WAIT_DEQUE_FAIL,
	ISP_ED_BUF_STATE_DEQUE_SUCCESS,
	ISP_ED_BUF_STATE_DEQUE_FAIL
} ISP_ED_BUF_STATE_ENUM;

typedef enum {
	ISP_CMD_RESET_CAM_P1,		/* Reset */
	ISP_CMD_RESET_CAM_P2,
	ISP_CMD_RESET_CAMSV,
	ISP_CMD_RESET_CAMSV2,
	ISP_CMD_RESET_BUF,
	ISP_CMD_READ_REG,		/* Read register from driver */
	ISP_CMD_WRITE_REG,		/* Write register to driver */
	ISP_CMD_HOLD_TIME,
	ISP_CMD_HOLD_REG,		/* Hold reg write to hw, on/off */
	ISP_CMD_WAIT_IRQ,		/* Wait IRQ */
	ISP_CMD_READ_IRQ,		/* Read IRQ */
	ISP_CMD_CLEAR_IRQ,		/* Clear IRQ */
	ISP_CMD_DUMP_REG,		/* Dump ISP registers, debug */
	ISP_CMD_SET_USER_PID,		/* for signal */
	ISP_CMD_RT_BUF_CTRL,		/* pass1 buffer control */
	ISP_CMD_REF_CNT,		/* get imem reference count */
	ISP_CMD_DEBUG_FLAG,		/* dump message level */
	ISP_CMD_REGISTER_IRQ,		/* register a specific irq */
	ISP_CMD_UNREGISTER_IRQ,
	ISP_CMD_ED_QUEBUF_CTRL,
	ISP_CMD_UPDATE_REGSCEN,
	ISP_CMD_QUERY_REGSCEN,
	ISP_CMD_UPDATE_BURSTQNUM,
	ISP_CMD_QUERY_BURSTQNUM,
	ISP_CMD_DUMP_ISR_LOG,
	ISP_CMD_GET_CUR_SOF,
	ISP_CMD_GET_DMA_ERR,
	ISP_CMD_GET_INT_ERR,
	ISP_CMD_GET_DROP_FRAME,
	ISP_CMD_SET_FPS,
	ISP_CMD_WAKELOCK_CTRL,
	ISP_CMD_REGISTER_IRQ_USER_KEY,
	ISP_CMD_MARK_IRQ_REQUEST,
	ISP_CMD_GET_MARK2QUERY_TIME,
	ISP_CMD_FLUSH_IRQ_REQUEST,
} ISP_CMD_ENUM;

/* ------------------------------------------------------------------ */
/* Structures                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
	ISP_IRQ_TYPE_ENUM Type;
	__u32 Status;
	__s32 UserKey;		/* user key for interrupt operations */
} ISP_IRQ_USER_STRUCT;

typedef struct {
	__u32 tLastSig_sec;
	__u32 tLastSig_usec;
	__u32 tMark2WaitSig_sec;
	__u32 tMark2WaitSig_usec;
	__u32 tLastSig2GetSig_sec;
	__u32 tLastSig2GetSig_usec;
	__s32 passedbySigcnt;
} ISP_IRQ_TIME_STRUCT;

typedef struct {
	__u32 tLastSOF2P1done_sec;
	__u32 tLastSOF2P1done_usec;
} ISP_EIS_META_STRUCT;

typedef struct {
	ISP_IRQ_CLEAR_ENUM Clear;	/* v1 and v3 */
	ISP_IRQ_TYPE_ENUM Type;		/* v1 only */
	__u32 Status;			/* v1 only */
	__s32 UserNumber;		/* v1 only */
	__u32 Timeout;			/* v1 and v3 */
	__u64 UserName;			/* char __user *, unused */
	__u32 irq_TStamp;		/* v1 and v3 */
	__u32 bDumpReg;			/* v1 and v3 */
	ISP_IRQ_USER_STRUCT UserInfo;	/* v3 only */
	ISP_IRQ_TIME_STRUCT TimeInfo;	/* v3 only */
	ISP_EIS_META_STRUCT EisMeta;	/* v1 and v3 */
	ISP_IRQ_WAITIRQ_SPEUSER_ENUM SpecUser;
} ISP_WAIT_IRQ_STRUCT;

typedef struct {
	__s32 userKey;
	__u64 userName;		/* char __user * */
} ISP_REGISTER_USERKEY_STRUCT;

typedef struct {
	ISP_IRQ_TYPE_ENUM Type;
	ISP_IRQ_USER_ENUM UserNumber;
	__u32 Status;
} ISP_READ_IRQ_STRUCT;

typedef struct {
	ISP_IRQ_TYPE_ENUM Type;
	ISP_IRQ_USER_ENUM UserNumber;
	__u32 Status;
} ISP_CLEAR_IRQ_STRUCT;

typedef struct {
	__u32 Addr;	/* register address (physical) */
	__u32 Val;	/* register value */
} ISP_REG_STRUCT;

typedef struct {
	__u64 pData;	/* ISP_REG_STRUCT __user * */
	__u32 Count;
} ISP_REG_IO_STRUCT;

typedef struct {
	__u32 w;	/* TG size */
	__u32 h;
	__u32 xsize;	/* DMAO xsize */
	__u32 stride;
	__u32 fmt;
	__u32 pxl_id;
	__u32 wbn;
	__u32 ob;
	__u32 lsc;
	__u32 rpg;
	__u32 m_num_0;
	__u32 frm_cnt;
	__u32 bus_size;
} ISP_RT_IMAGE_INFO_STRUCT;

typedef struct {
	__u32 srcX;	/* crop window start point */
	__u32 srcY;
	__u32 srcW;	/* crop window size */
	__u32 srcH;
	__u32 dstW;	/* rrz out size */
	__u32 dstH;
} ISP_RT_RRZ_INFO_STRUCT;

typedef struct {
	__u32 x;	/* in pixels */
	__u32 y;
	__u32 w;	/* in bytes */
	__u32 h;	/* in bytes */
} ISP_RT_DMAO_CROPPING_STRUCT;

typedef struct {
	__u32 memID;
	__u32 size;
	__s64 base_vAddr;
	__u32 base_pAddr;
	__u32 timeStampS;
	__u32 timeStampUs;
	__u32 bFilled;
	__u32 bProcessRaw;
	ISP_RT_IMAGE_INFO_STRUCT image;
	ISP_RT_RRZ_INFO_STRUCT rrzInfo;
	ISP_RT_DMAO_CROPPING_STRUCT dmaoCrop;	/* imgo */
	__u32 bDequeued;
	__s32 bufIdx;		/* used for buffer replacement */
} ISP_RT_BUF_INFO_STRUCT;

/* P1_DEQUE_CNT: one image per dequeue on this platform */
#define ISP_P1_DEQUE_CNT	1

typedef struct {
	__u32 count;
	__u32 sof_cnt;
	__u32 img_cnt;
	ISP_RT_BUF_INFO_STRUCT data[ISP_P1_DEQUE_CNT];
} ISP_DEQUE_BUF_INFO_STRUCT;

#define ISP_RT_BUF_SIZE		16
#define ISP_RT_CQ0C_BUF_SIZE	ISP_RT_BUF_SIZE

typedef struct {
	__u32 start;		/* current DMA accessing buffer */
	__u32 total_count;	/* filled + empty */
	__u32 empty_count;	/* empty buffers incl. current */
	__u32 pre_empty_count;
	__u32 active;
	__u32 read_idx;
	__u32 img_cnt;
	ISP_RT_BUF_INFO_STRUCT data[ISP_RT_BUF_SIZE];
} ISP_RT_RING_BUF_INFO_STRUCT;

typedef struct {
	ISP_RTBC_STATE_ENUM state;
	__u64 dropCnt;
	ISP_RT_RING_BUF_INFO_STRUCT ring_buf[_rt_dma_max_];
} ISP_RT_BUF_STRUCT;

typedef struct {
	ISP_RT_BUF_CTRL_ENUM ctrl;
	_isp_dma_enum_ buf_id;
	__u64 data_ptr;		/* ISP_RT_BUF_INFO_STRUCT __user * */
	__u64 ex_data_ptr;	/* ISP_RT_BUF_INFO_STRUCT __user *, exchange */
	__u64 pExtend;		/* extra in/out struct, see each ctrl */
} ISP_BUFFER_CTRL_STRUCT;

typedef struct {
	ISP_REF_CNT_CTRL_ENUM ctrl;
	ISP_REF_CNT_ID_ENUM id;
	__u64 data_ptr;		/* signed int __user * */
} ISP_REF_CNT_CTRL_STRUCT;

typedef struct {
	ISP_ED_BUFQUE_CTRL_ENUM ctrl;
	__u32 processID;
	__u32 callerID;
	__s32 p2burstQIdx;
	__s32 p2dupCQIdx;
	__u32 timeoutUs;
} ISP_ED_BUFQUE_STRUCT;

/* ------------------------------------------------------------------ */
/* CQ0C real-time ring buffer descriptors (userspace builder)         */
/* ------------------------------------------------------------------ */

typedef struct {
	__u32 inst;
	__u32 data_ptr_pa;
} ISP_CQ_CMD_ST;

typedef struct {
	ISP_CQ_CMD_ST imgo;
	ISP_CQ_CMD_ST rrzo;
	ISP_CQ_CMD_ST next_cq0ci;
	ISP_CQ_CMD_ST end;
	__u32 imgo_base_pAddr;
	__u32 rrzo_base_pAddr;
	__s32 imgo_buf_idx;
	__s32 rrzo_buf_idx;
} ISP_CQ_INFO_RTBC_ST;

typedef struct _isp_cq_ring_cmd_st_ {
	ISP_CQ_INFO_RTBC_ST cq_rtbc;
	__u64 next_pa;
	struct _isp_cq_ring_cmd_st_ *pNext;
} ISP_CQ_RING_CMD_ST;

typedef struct {
	ISP_CQ_RING_CMD_ST rtbc_ring[ISP_RT_CQ0C_BUF_SIZE];
	__u32 imgo_ring_size;
	__u32 rrzo_ring_size;
} ISP_CQ_RTBC_RING_ST;

typedef union {
	struct {
		__u32 FBC_CNT:4;
		__u32 rsv_4:7;
		__u32 RCNT_INC:1;
		__u32 rsv_12:2;
		__u32 FBC_EN:1;
		__u32 LOCK_EN:1;
		__u32 FB_NUM:4;
		__u32 RCNT:4;
		__u32 WCNT:4;
		__u32 DROP_CNT:4;
	} Bits;
	__u32 Reg_val;
} ISP_CQ_RTBC_FBC;

/* ------------------------------------------------------------------ */
/* ioctl commands (keep the downstream names and numbers)             */
/* ------------------------------------------------------------------ */

#define ISP_RESET_CAM_P1	_IO(MTK_ISP_MAGIC, ISP_CMD_RESET_CAM_P1)
#define ISP_RESET_CAM_P2	_IO(MTK_ISP_MAGIC, ISP_CMD_RESET_CAM_P2)
#define ISP_RESET_CAMSV		_IO(MTK_ISP_MAGIC, ISP_CMD_RESET_CAMSV)
#define ISP_RESET_CAMSV2	_IO(MTK_ISP_MAGIC, ISP_CMD_RESET_CAMSV2)
#define ISP_RESET_BUF		_IO(MTK_ISP_MAGIC, ISP_CMD_RESET_BUF)
#define ISP_READ_REGISTER	_IOWR(MTK_ISP_MAGIC, ISP_CMD_READ_REG, ISP_REG_IO_STRUCT)
#define ISP_WRITE_REGISTER	_IOWR(MTK_ISP_MAGIC, ISP_CMD_WRITE_REG, ISP_REG_IO_STRUCT)
#define ISP_HOLD_REG_TIME	_IOW(MTK_ISP_MAGIC, ISP_CMD_HOLD_TIME, ISP_HOLD_TIME_ENUM)
#define ISP_HOLD_REG		_IOW(MTK_ISP_MAGIC, ISP_CMD_HOLD_REG, __u8)
#define ISP_WAIT_IRQ		_IOW(MTK_ISP_MAGIC, ISP_CMD_WAIT_IRQ, ISP_WAIT_IRQ_STRUCT)
#define ISP_READ_IRQ		_IOR(MTK_ISP_MAGIC, ISP_CMD_READ_IRQ, ISP_READ_IRQ_STRUCT)
#define ISP_CLEAR_IRQ		_IOW(MTK_ISP_MAGIC, ISP_CMD_CLEAR_IRQ, ISP_CLEAR_IRQ_STRUCT)
#define ISP_DUMP_REG		_IO(MTK_ISP_MAGIC, ISP_CMD_DUMP_REG)
#define ISP_SET_USER_PID	_IOW(MTK_ISP_MAGIC, ISP_CMD_SET_USER_PID, __u32)
#define ISP_BUFFER_CTRL		_IOWR(MTK_ISP_MAGIC, ISP_CMD_RT_BUF_CTRL, ISP_BUFFER_CTRL_STRUCT)
#define ISP_REF_CNT_CTRL	_IOWR(MTK_ISP_MAGIC, ISP_CMD_REF_CNT, ISP_REF_CNT_CTRL_STRUCT)
#define ISP_DEBUG_FLAG		_IOW(MTK_ISP_MAGIC, ISP_CMD_DEBUG_FLAG, __u64)
#define ISP_REGISTER_IRQ	_IOW(MTK_ISP_MAGIC, ISP_CMD_REGISTER_IRQ, ISP_WAIT_IRQ_STRUCT)
#define ISP_UNREGISTER_IRQ	_IOW(MTK_ISP_MAGIC, ISP_CMD_UNREGISTER_IRQ, ISP_WAIT_IRQ_STRUCT)
#define ISP_ED_QUEBUF_CTRL	_IOWR(MTK_ISP_MAGIC, ISP_CMD_ED_QUEBUF_CTRL, ISP_ED_BUFQUE_STRUCT)
#define ISP_UPDATE_REGSCEN	_IOWR(MTK_ISP_MAGIC, ISP_CMD_UPDATE_REGSCEN, __u32)
#define ISP_QUERY_REGSCEN	_IOR(MTK_ISP_MAGIC, ISP_CMD_QUERY_REGSCEN, __u32)
#define ISP_UPDATE_BURSTQNUM	_IOW(MTK_ISP_MAGIC, ISP_CMD_UPDATE_BURSTQNUM, __s32)
#define ISP_QUERY_BURSTQNUM	_IOR(MTK_ISP_MAGIC, ISP_CMD_QUERY_BURSTQNUM, __s32)
#define ISP_DUMP_ISR_LOG	_IO(MTK_ISP_MAGIC, ISP_CMD_DUMP_ISR_LOG)
#define ISP_GET_CUR_SOF		_IOR(MTK_ISP_MAGIC, ISP_CMD_GET_CUR_SOF, __u32)
#define ISP_GET_DMA_ERR		_IOWR(MTK_ISP_MAGIC, ISP_CMD_GET_DMA_ERR, __u64)
#define ISP_GET_INT_ERR		_IOR(MTK_ISP_MAGIC, ISP_CMD_GET_INT_ERR, __u64)
#define ISP_GET_DROP_FRAME	_IOWR(MTK_ISP_MAGIC, ISP_CMD_GET_DROP_FRAME, __u32)
#define ISP_SET_FPS		_IOW(MTK_ISP_MAGIC, ISP_CMD_SET_FPS, __u32)
#define ISP_WAKELOCK_CTRL	_IOWR(MTK_ISP_MAGIC, ISP_CMD_WAKELOCK_CTRL, __u32)
#define ISP_REGISTER_IRQ_USER_KEY \
	_IOR(MTK_ISP_MAGIC, ISP_CMD_REGISTER_IRQ_USER_KEY, ISP_REGISTER_USERKEY_STRUCT)
#define ISP_MARK_IRQ_REQUEST	_IOWR(MTK_ISP_MAGIC, ISP_CMD_MARK_IRQ_REQUEST, ISP_WAIT_IRQ_STRUCT)
#define ISP_GET_MARK2QUERY_TIME	_IOWR(MTK_ISP_MAGIC, ISP_CMD_GET_MARK2QUERY_TIME, ISP_WAIT_IRQ_STRUCT)
#define ISP_FLUSH_IRQ_REQUEST	_IOW(MTK_ISP_MAGIC, ISP_CMD_FLUSH_IRQ_REQUEST, ISP_WAIT_IRQ_STRUCT)

/* Number of DMA error counters returned by ISP_GET_DMA_ERR */
#define ISP_DMA_ERR_COUNT	18	/* nDMA_ERR_P1(11) + nDMA_ERR_P1_D(7) */
/* Size of the ISP interrupt error array returned by ISP_GET_INT_ERR */
#define ISP_INT_ERR_COUNT	4	/* _IRQ, _IRQ_D, _CAMSV_IRQ, _CAMSV_D_IRQ */

#endif /* _UAPI_LINUX_MTK_ISP_H */
