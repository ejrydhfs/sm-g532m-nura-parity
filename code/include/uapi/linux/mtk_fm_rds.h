/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * MediaTek WCN FM radio - raw RDS record ABI.
 *
 * The driver exposes the raw RDS group stream of the MT6627 firmware through
 * read() on the V4L2 radio device (VFL_TYPE_RADIO, /dev/radio0).  This header
 * is the single source of truth for that byte layout; the driver uses the
 * same definitions, so the ABI cannot drift from the implementation.
 *
 * Record stream
 * -------------
 * Every read() returns one or more whole records of
 * sizeof(struct mtk_wcn_fm_rds_data), which is MTK_FM_RDS_RECORD_SIZE bytes.
 * The read size must therefore be at least one record and should be a
 * multiple of it:
 *
 *   - count < sizeof(struct mtk_wcn_fm_rds_data)	-> -EINVAL
 *   - count >= sizeof(struct mtk_wcn_fm_rds_data)	-> 0 < n*148 <= count
 *
 * A short read never happens for a partial record: the driver always copies
 * a whole number of records.  A read() on an empty FIFO blocks until an RDS
 * record arrives, unless the file was opened with O_NONBLOCK, in which case
 * it returns -EWOULDBLOCK.  poll() reports EPOLLIN | EPOLLRDNORM when at
 * least one record is queued; when the chip is powered down, blocked readers
 * wake up and read() returns -EIO (poll() sets EPOLLHUP).
 *
 * Record contents
 * ---------------
 * Each record starts with the two demodulator samples (sin, cos), which the
 * firmware emits for every RDS event and which carry no RDS data.  They are
 * followed by MTK_FM_RDS_GROUP_COUNT group slots filled from the firmware
 * event payload in order.  The event payload may hold fewer groups than the
 * fixed record size: the driver zero-fills the rest of the record, so unused
 * slots hold all-zero blocks with crc == 0.  Because a received group always
 * has at least its B block marked good (see below), a slot with crc == 0 can
 * be recognised as "no group here" and skipped.
 *
 * RDS data arrives with one record per firmware RDS_RX_DATA event; the
 * sequence number of the record inside the stream is not part of the ABI.
 *
 * Validity
 * --------
 * The firmware does not deliver a separate validity flag.  Instead the crc
 * word is a good-block bitmap, one bit per block, set when the block passed
 * the RDS checkword check (and, for corrected blocks, was repaired):
 *
 *	bit 3 (0x08)	block A (PI)
 *	bit 2 (0x04)	block B (group type, TP, PTY, ...)
 *	bit 1 (0x02)	block C
 *	bit 0 (0x01)	block D
 *
 * Only groups whose B block is marked good carry a usable group type; the
 * other blocks are optional depending on the group version.  The cbc word is
 * the firmware's count of corrected bits for the group.  These semantics are
 * the ones the downstream driver relies on (fm_rds_parser.c:122-143,
 * 209-243); this interface passes them through unchanged.
 *
 * Note that group 0B/1B/2B/... repeat the PI code in block C (offset C'),
 * which is an ordinary block C value on the wire.
 */

#ifndef _UAPI_LINUX_MTK_FM_RDS_H
#define _UAPI_LINUX_MTK_FM_RDS_H

#include <linux/types.h>

/** Number of RDS group slots in one record. */
#define MTK_FM_RDS_GROUP_COUNT		12

/** Size of one raw RDS record as returned by read(). */
#define MTK_FM_RDS_RECORD_SIZE		148

/** Good-block bits in &struct mtk_wcn_fm_rds_block.crc. */
#define MTK_FM_RDS_BLOCK_A_OK		0x0008
#define MTK_FM_RDS_BLOCK_B_OK		0x0004
#define MTK_FM_RDS_BLOCK_C_OK		0x0002
#define MTK_FM_RDS_BLOCK_D_OK		0x0001

/**
 * struct mtk_wcn_fm_rds_block - one RDS group as delivered by the firmware
 * @blk_a: block A, the programme identification (PI) code
 * @blk_b: block B, group type/version, TP, PTY and group specific bits
 * @blk_c: block C, group specific (PI in version B groups, offset C')
 * @blk_d: block D, group specific
 * @cbc: firmware count of corrected bits for the group
 * @crc: good-block bitmap, see MTK_FM_RDS_BLOCK_*_OK
 *
 * Mirrors downstream rds_packet_t (inc/fm_rds.h:32-39).
 */
struct mtk_wcn_fm_rds_block {
	__u16 blk_a;
	__u16 blk_b;
	__u16 blk_c;
	__u16 blk_d;
	__u16 cbc;
	__u16 crc;
};

/**
 * struct mtk_wcn_fm_rds_data - one raw RDS record returned by read()
 * @sin: demodulator sample, not RDS data
 * @cos: demodulator sample, not RDS data
 * @group: group slots, filled in firmware order and zero-filled when unused
 *
 * Mirrors downstream rds_rx_t (inc/fm_rds.h:41-45).  The record is exactly
 * MTK_FM_RDS_RECORD_SIZE (148) bytes.
 */
struct mtk_wcn_fm_rds_data {
	__u16 sin;
	__u16 cos;
	struct mtk_wcn_fm_rds_block group[MTK_FM_RDS_GROUP_COUNT];
};

#endif /* _UAPI_LINUX_MTK_FM_RDS_H */
