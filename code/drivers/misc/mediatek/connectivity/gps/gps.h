/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek WCN GPS driver: internal interface shared by the GNSS device and
 * the vendor /dev/stpgps character device.
 *
 * Port from Samsung/MTK downstream connectivity/gps (stp_chrdev_gps.c and
 * gps.c) of the SM-G532M Android 6.0.1 kernel.
 *
 * Copyright (C) 2011-2014 MediaTek Inc.
 */

#ifndef _MTK_WCN_GPS_H_
#define _MTK_WCN_GPS_H_

#include <linux/device.h>
#include <linux/kconfig.h>
#include <linux/mutex.h>
#include <linux/rfkill.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#if IS_ENABLED(CONFIG_GNSS)
#include <linux/gnss.h>
#endif

struct nvmem_cell;

/*
 * Owner of the single STP GPS channel.  The GNSS device and the vendor
 * /dev/stpgps node are two faces of the same link, so only one of them may
 * be open at a time (the other open fails with -EBUSY).
 */
enum mtk_gps_owner {
	MTK_GPS_OWNER_NONE = 0,
	MTK_GPS_OWNER_GNSS,
	MTK_GPS_OWNER_VENDOR,
};

/**
 * struct mtk_gps - GPS driver state
 * @dev: platform device (parent of the GNSS and vendor devices)
 * @gdev: GNSS character device, NULL when CONFIG_GNSS is disabled
 * @rfkill: GPS radio switch
 * @efuse_cell: optional "gps-efuse" nvmem cell, see stpgps_ioctl()
 * @lock: serialises open/close, ioctl power transitions, rfkill and PM
 * @rx_lock: protects @rx_enabled, @rx_seq and @rx_buf against the STP callback
 * @rx_enabled: the STP callback may feed the GNSS receive FIFO
 * @rx_seq: incremented for every STP receive event, wakes blocking readers
 * @rx_buf: GNSS RX drain scratch buffer
 * @rx_wait: /dev/stpgps blocking read wait queue
 * @rd_mtx: serialises /dev/stpgps read() (downstream rd_mtx semaphore)
 * @wr_mtx: serialises /dev/stpgps write() (downstream wr_mtx semaphore)
 * @rd_buf: /dev/stpgps read() staging buffer
 * @wr_buf: /dev/stpgps write() staging buffer
 * @owner: which face of the link is currently open
 * @channel_on: the WMT GPS function and the STP channel are up
 * @resetting: the connectivity SoC is being reset
 * @suspended: system suspend notification state
 */
struct mtk_gps {
	struct device *dev;
#if IS_ENABLED(CONFIG_GNSS)
	struct gnss_device *gdev;
#endif
	struct rfkill *rfkill;
	struct nvmem_cell *efuse_cell;

	struct mutex lock;	/* open/close, rfkill and PM transitions */
	spinlock_t rx_lock;	/* STP callback state */
	bool rx_enabled;
	u32 rx_seq;
	u8 *rx_buf;

	wait_queue_head_t rx_wait;
	struct mutex rd_mtx;
	struct mutex wr_mtx;
	u8 *rd_buf;
	u8 *wr_buf;

	enum mtk_gps_owner owner;
	bool channel_on;
	bool resetting;
	bool suspended;
};

/* Channel/power state machine (gps.c). */
int mtk_gps_channel_on(struct mtk_gps *gps);
void mtk_gps_channel_off(struct mtk_gps *gps);
int mtk_gps_claim(struct mtk_gps *gps, enum mtk_gps_owner owner);
void mtk_gps_release(struct mtk_gps *gps, enum mtk_gps_owner owner);

/* Vendor character device (stp_chrdev_gps.c). */
int mtk_gps_vendor_init(struct mtk_gps *gps);
void mtk_gps_vendor_exit(struct mtk_gps *gps);

#endif /* _MTK_WCN_GPS_H_ */
