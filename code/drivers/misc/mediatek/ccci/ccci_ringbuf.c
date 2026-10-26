// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI shared-memory ring buffer.
 *
 * The AP and the modem exchange framed packets through a ring carved out of
 * the modem shared memory.  Wire format (downstream ccci_ringbuf.c):
 *
 *   ring  := 2 * CCCI_RBF_HEADER
 *            rx_control { read, write, length }
 *            tx_control { read, write, length }
 *            rx payload [length]
 *            tx payload [length]
 *            2 * CCCI_RBF_FOOTER
 *
 *   packet := CCIF_PKG_HEADER, len
 *             payload (len bytes)
 *             padding to 8 byte alignment
 *             CCIF_PKG_FOOTER, CCIF_PKG_FOOTER
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/ccci_ringbuf.c.
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 *
 * Unlike the downstream version the ring is accessed through the MMIO
 * accessors, because it lives in UNCACHED shared memory that the modem also
 * writes: plain pointer dereferences would be torn/reordered on arm64.
 */
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ccci_priv.h"

#define CTL_RX_READ	0
#define CTL_RX_WRITE	4
#define CTL_RX_LEN	8
#define CTL_TX_READ	12
#define CTL_TX_WRITE	16
#define CTL_TX_LEN	20
#define CTL_SIZE	24

struct ccci_ringbuf {
	void __iomem *base;
	void __iomem *ctl;
	void __iomem *rx_buf;
	void __iomem *tx_buf;
	u32 rx_len;
	u32 tx_len;
	u32 size;
};

static inline u32 ctl_rd(struct ccci_ringbuf *rb, u32 off)
{
	return readl(rb->ctl + off);
}

static inline void ctl_wr(struct ccci_ringbuf *rb, u32 off, u32 val)
{
	writel(val, rb->ctl + off);
}

/* copy out of a ring region, handling wrap-around */
static void ring_read(struct ccci_ringbuf *rb, void __iomem *region, u32 pos,
		      void *dst, u32 len, u32 region_len)
{
	u32 first = min(len, region_len - pos);

	memcpy_fromio(dst, region + pos, first);
	if (first < len)
		memcpy_fromio((u8 *)dst + first, region, len - first);
}

static void ring_write(struct ccci_ringbuf *rb, void __iomem *region, u32 pos,
		       const void *src, u32 len, u32 region_len)
{
	u32 first = min(len, region_len - pos);

	memcpy_toio(region + pos, src, first);
	if (first < len)
		memcpy_toio(region, (const u8 *)src + first, len - first);
}

static inline u32 ring_align(u32 len)
{
	return (len + 7) & ~7u;
}

struct ccci_ringbuf *ccci_ringbuf_create(void __iomem *mem, unsigned int mem_size,
					 unsigned int rx_size, unsigned int tx_size)
{
	struct ccci_ringbuf *rb;
	u32 total = CCCI_RINGBUF_CTL_LEN + rx_size + tx_size;

	if (!mem || !rx_size || !tx_size || mem_size < total)
		return NULL;

	rb = kzalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return NULL;

	rb->base = mem;
	rb->ctl = mem + 8;
	rb->rx_buf = mem + 8 + CTL_SIZE;
	rb->tx_buf = rb->rx_buf + rx_size;
	rb->rx_len = rx_size;
	rb->tx_len = tx_size;
	rb->size = total;

	/* header, control block and footer guards */
	writel(CCCI_RBF_HEADER, mem + 0);
	writel(CCCI_RBF_HEADER, mem + 4);
	ctl_wr(rb, CTL_RX_READ, 0);
	ctl_wr(rb, CTL_RX_WRITE, 0);
	ctl_wr(rb, CTL_RX_LEN, rx_size);
	ctl_wr(rb, CTL_TX_READ, 0);
	ctl_wr(rb, CTL_TX_WRITE, 0);
	ctl_wr(rb, CTL_TX_LEN, tx_size);
	writel(CCCI_RBF_FOOTER, mem + total - 8);
	writel(CCCI_RBF_FOOTER, mem + total - 4);
	wmb();

	return rb;
}

/**
 * ccci_ringbuf_readable - length of the next complete packet
 * @rb: ring buffer
 *
 * Returns the payload length (without framing) of the packet at the read
 * pointer, or a negative CCCI_RINGBUF_* error.
 */
int ccci_ringbuf_readable(struct ccci_ringbuf *rb)
{
	u32 read, write, length, size, hdr[2], ftr[2], pkg_len, total;
	u32 footer_pos;

	if (!rb)
		return -CCCI_ERR_INVALID_PARAM;

	read = ctl_rd(rb, CTL_RX_READ);
	write = ctl_rd(rb, CTL_RX_WRITE);
	length = ctl_rd(rb, CTL_RX_LEN);

	size = (write + length - read) % length;
	if (size < CCIF_PKG_HDR_LEN + CCIF_PKG_FTR_LEN + CCCI_HEADER_SIZE)
		return -7;	/* CCCI_RINGBUF_EMPTY */

	ring_read(rb, rb->rx_buf, read, hdr, sizeof(hdr), length);
	if (hdr[0] != CCIF_PKG_HEADER) {
		pr_err("ccci: ring header 0x%08x != 0x%08x (r=%u w=%u len=%u)\n",
		       hdr[0], CCIF_PKG_HEADER, read, write, length);
		return -3;	/* CCCI_RINGBUF_BAD_HEADER */
	}
	pkg_len = hdr[1];
	total = ring_align(CCIF_PKG_HDR_LEN + pkg_len + CCIF_PKG_FTR_LEN);
	if (total > size)
		return -6;	/* CCCI_RINGBUF_NOT_COMPLETE */

	footer_pos = read + total - CCIF_PKG_FTR_LEN;
	if (footer_pos >= length)
		footer_pos -= length;
	ring_read(rb, rb->rx_buf, footer_pos, ftr, sizeof(ftr), length);
	if (ftr[0] != CCIF_PKG_FOOTER || ftr[1] != CCIF_PKG_FOOTER) {
		pr_err("ccci: ring footer %08x %08x != %08x (pos=%u len=%u)\n",
		       ftr[0], ftr[1], CCIF_PKG_FOOTER, footer_pos, length);
		return -4;	/* CCCI_RINGBUF_BAD_FOOTER */
	}
	if (pkg_len > rb->rx_len)
		return -CCCI_ERR_INVALID_PARAM;

	return pkg_len;
}

/**
 * ccci_ringbuf_writeable - space left for a @size byte payload
 *
 * Positive: number of bytes consumed in the ring (header+payload+footer,
 * 8 byte aligned).  Negative: the number of bytes that are missing.
 * Zero payload is rejected, matching downstream behaviour.
 */
int ccci_ringbuf_writeable(struct ccci_ringbuf *rb, unsigned int size)
{
	u32 read, write, length, avail, need;

	if (!rb)
		return -CCCI_ERR_INVALID_PARAM;

	read = ctl_rd(rb, CTL_TX_READ);
	write = ctl_rd(rb, CTL_TX_WRITE);
	length = ctl_rd(rb, CTL_TX_LEN);

	if (size > length)
		return -CCCI_ERR_INVALID_PARAM;

	need = ring_align(size + CCIF_PKG_HDR_LEN + CCIF_PKG_FTR_LEN);

	if (read == write)
		avail = length - 1;
	else if (read < write)
		avail = length - write - 1 + read;
	else
		avail = read - write - 1;

	return need < avail ? (int)need : -(int)(need - avail);
}

int ccci_ringbuf_write(struct ccci_ringbuf *rb, const void *src, unsigned int len)
{
	u32 write, read, length;
	const u32 hdr[2] = { CCIF_PKG_HEADER, 0 };
	u32 hdr_len = len;
	const u32 ftr[2] = { CCIF_PKG_FOOTER, CCIF_PKG_FOOTER };
	u32 pos;

	if (!rb || !src || !len)
		return -CCCI_ERR_INVALID_PARAM;
	if (ccci_ringbuf_writeable(rb, len) <= 0)
		return -2;	/* CCCI_RINGBUF_NOT_ENOUGH */

	read = ctl_rd(rb, CTL_TX_READ);
	write = ctl_rd(rb, CTL_TX_WRITE);
	length = ctl_rd(rb, CTL_TX_LEN);

	pos = write;
	ring_write(rb, rb->tx_buf, pos, &hdr[0], 4, length);
	pos = (pos + 4) % length;
	ring_write(rb, rb->tx_buf, pos, &hdr_len, 4, length);
	pos = (pos + 4) % length;
	ring_write(rb, rb->tx_buf, pos, src, len, length);
	pos = (pos + ring_align(len)) % length;
	ring_write(rb, rb->tx_buf, pos, &ftr[0], 4, length);
	pos = (pos + 4) % length;
	ring_write(rb, rb->tx_buf, pos, &ftr[1], 4, length);
	pos = (pos + 4) % length;

	/* publish only after the payload is visible to the modem */
	wmb();
	ctl_wr(rb, CTL_TX_WRITE, pos);

	return len;
}

int ccci_ringbuf_read(struct ccci_ringbuf *rb, void *dst, unsigned int len)
{
	u32 read, length;

	if (!rb || !dst || !len)
		return -CCCI_ERR_INVALID_PARAM;

	read = ctl_rd(rb, CTL_RX_READ);
	length = ctl_rd(rb, CTL_RX_LEN);
	if (len > length)
		return -CCCI_ERR_INVALID_PARAM;

	ring_read(rb, rb->rx_buf, (read + CCIF_PKG_HDR_LEN) % length, dst, len,
		  length);

	return len;
}

void ccci_ringbuf_move_rpointer(struct ccci_ringbuf *rb, unsigned int len)
{
	u32 read, length, pos;

	if (!rb)
		return;

	read = ctl_rd(rb, CTL_RX_READ);
	length = ctl_rd(rb, CTL_RX_LEN);
	pos = (read + ring_align(len + CCIF_PKG_HDR_LEN + CCIF_PKG_FTR_LEN)) %
	      length;
	/* make sure the payload was consumed before the modem may reuse it */
	mb();
	ctl_wr(rb, CTL_RX_READ, pos);
}

void ccci_ringbuf_reset(struct ccci_ringbuf *rb, bool tx)
{
	if (!rb)
		return;

	if (tx) {
		ctl_wr(rb, CTL_TX_READ, 0);
		ctl_wr(rb, CTL_TX_WRITE, 0);
	} else {
		ctl_wr(rb, CTL_RX_READ, 0);
		ctl_wr(rb, CTL_RX_WRITE, 0);
	}
	wmb();
}
