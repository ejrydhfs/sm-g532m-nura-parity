// SPDX-License-Identifier: GPL-2.0
/*
 * CCCI exception dump engine: collect the modem exception record and the
 * shared-memory debug windows, decode them and expose the result in the
 * kernel log, in the util dump ring, in debugfs files per modem and in a
 * kernel-retrievable buffer.
 *
 * Ported from the Samsung/MTK 3.18 downstream kernel
 * drivers/misc/mediatek/eccci/port_kernel.c (ccci_aed, ccci_md_exception,
 * ccci_ee_info_dump, md_ex_monitor_func, md_ex_monitor2_func,
 * md_bootup_timeout_func) and ccci_bm.c (ccci_mem_dump).
 * Original copyright: Copyright (C) 2015 MediaTek Inc. GPL-2.0.
 *
 * The ROM hands the collected buffers to the AEE client
 * (aed_md_exception_api, ROM 0xc0628298).  That client does not exist in this
 * kernel tree, so the dump is delivered to the destinations above instead of
 * pretending to call AEE.  The ROM ABI used by such a client,
 * aee_dump_ccci_debug_info(), is implemented in ccci_core.c and exported.
 *
 * Layout of the AP<->MD shared-memory exception window, which is the base of
 * the modem smem region (downstream ccci_core.c:262-293, ccci_core.h:580-619,
 * mt6735/ccci_config.h:70-83):
 *
 *   smem+0x0000  2048  CCCI debug record       (aee_dump_ccci_debug_info())
 *   smem+0x0800  2048  MDSS debug window       (CCCI_AED_DUMP_EX_MEM)
 *   smem+0x0BF0    16  CCIF SRAM header copy   (CCCI_AED_DUMP_CCIF_REG)
 *
 * The MDSS debug window and the exception record (CCCI_SMEM_OFFSET_EXREC) are
 * the same 2 KiB because on this platform both constants are 2048:
 * ccci_config.h:75 and :82.  The modem fills either the legacy EX_LOG_T that
 * arrives in the MD_EX_REC_OK packet or, after the boot handshake, a record
 * at CCCI_SMEM_OFFSET_EXREC (port_kernel.c:3125-3135); the build has no
 * MD_UMOLY_EE_SUPPORT, so the EX_LOG_T layout applies.
 */
#include <linux/aee.h>
#include <linux/ccci.h>
#include <linux/byteorder/generic.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ccci_priv.h"

/*
 * Text snapshot builder.  The buffer is allocated with kvmalloc() in
 * ccci_ex_build() and swapped into md->ex.dump under dump_lock, so a debugfs
 * reader always sees a complete snapshot.
 */
struct ccci_ex_text {
	char *buf;
	size_t size;
	size_t len;
};

static void ccci_ex_printf(struct ccci_ex_text *t, const char *fmt, ...)
	__printf(2, 3);
static void ccci_ex_printf(struct ccci_ex_text *t, const char *fmt, ...)
{
	va_list args;
	int n;

	if (t->len >= t->size)
		return;

	va_start(args, fmt);
	n = vscnprintf(t->buf + t->len, t->size - t->len, fmt, args);
	va_end(args);
	t->len += n;
}

static void ccci_ex_hex(struct ccci_ex_text *t, const void *data, size_t len)
{
	const u8 *p = data;
	size_t off;
	int i;

	for (off = 0; off < len; off += 16) {
		size_t n = min_t(size_t, 16, len - off);

		ccci_ex_printf(t, "  %08zx:", off);
		for (i = 0; i < n; i++)
			ccci_ex_printf(t, " %02x", p[off + i]);
		ccci_ex_printf(t, "\n");
	}
}

static const char *ccci_ex_reason_name(unsigned int reason)
{
	switch (reason) {
	case CCCI_EX_DUMP_MD_EX:
		return "MD_EX";
	case CCCI_EX_DUMP_MD_EX_REC_OK:
		return "MD_EX_REC_OK";
	case CCCI_EX_DUMP_WDT:
		return "watchdog/reset";
	case CCCI_EX_DUMP_NO_RESPONSE:
		return "no-response/boot-timeout";
	default:
		return "unknown";
	}
}

const char *ccci_exception_stage_name(unsigned int stage)
{
	switch (stage) {
	case CCCI_EX_NONE:
		return "none";
	case CCCI_EX_INIT:
		return "EX_INIT";
	case CCCI_EX_DHL_DL_RDY:
		return "EX_DHL_DL_RDY";
	case CCCI_EX_INIT_DONE:
		return "EX_INIT_DONE";
	case CCCI_EX_NO_RESPONSE:
		return "MD_NO_RESPONSE";
	case CCCI_EX_WDT:
		return "MD_WDT";
	default:
		return "unknown";
	}
}

static void ccci_ex_aed_flags(struct ccci_ex_text *t, unsigned int flags)
{
	ccci_ex_printf(t, "  AED selection: 0x%x%s%s%s%s\n", flags,
		       flags & CCCI_AED_DUMP_EX_MEM ? " EX_MEM" : "",
		       flags & CCCI_AED_DUMP_MD_IMG_MEM ? " MD_IMG_MEM" : "",
		       flags & CCCI_AED_DUMP_CCIF_REG ? " CCIF_REG" : "",
		       flags & CCCI_AED_DUMP_EX_PKT ? " EX_PKT" : "");
}

/*
 * EX_HEADER_T.ex_type names, verbatim from the non-MD_UMOLY decoder of
 * downstream port_kernel.c:3128-3250 (the names the ROM prints), plus the
 * MD_EX_CC_* values of ccci_core.h:230-246.
 */
static const char *ccci_ex_type_name(unsigned int type)
{
	switch (type) {
	case CCCI_EX_TYPE_INVALID:
		return "INVALID";
	case CCCI_EX_TYPE_UNDEF:
		return "Fatal error (undefine)";
	case CCCI_EX_TYPE_SWI:
		return "Fatal error (swi)";
	case CCCI_EX_TYPE_PREF_ABT:
		return "Fatal error (prefetch abort)";
	case CCCI_EX_TYPE_DATA_ABT:
		return "Fatal error (data abort)";
	case CCCI_EX_TYPE_ASSERT:
		return "ASSERT";
	case CCCI_EX_TYPE_FATALERR_TASK:
		return "Fatal error (task)";
	case CCCI_EX_TYPE_FATALERR_BUF:
		return "Fatal error (buff)";
	case CCCI_EX_TYPE_LOCKUP:
		return "Lockup";
	case CCCI_EX_TYPE_ASSERT_DUMP:
		return "ASSERT DUMP";
	case CCCI_EX_TYPE_ASSERT_FAIL:
		return "ASSERT FAIL";
	case CCCI_EX_TYPE_DSP_ASSERT:
		return "MD DMD ASSERT";
	case CCCI_EX_TYPE_DSP_EXCEPTION:
		return "MD DMD Exception";
	case CCCI_EX_TYPE_DSP_FATAL_ERROR:
		return "MD DMD FATAL ERROR";
	case CCCI_EX_TYPE_CROSS_CORE:
		return "Fatal error (LTE_EXP)";
	case CCCI_EX_TYPE_EMI_MPU_VIOLATION:
		return "Fatal error (rmpu violation)";
	case CCCI_EX_TYPE_C2K_ERROR:
		return "Fatal error (C2K_EXP)";
	case CCCI_EX_TYPE_EMI_CHECK:
		return "EMI_CHK";
	default:
		return "UNKNOWN Exception";
	}
}

/*
 * Copy a bounded, printable C string out of the modem record.  The modem
 * writes raw bytes; anything outside printable ASCII is shown as '.'.
 */
static void ccci_ex_sanitize(char *dst, size_t dst_len, const u8 *src,
			     size_t src_len)
{
	size_t i, n;

	n = strnlen((const char *)src, src_len);
	if (n >= dst_len)
		n = dst_len - 1;
	for (i = 0; i < n; i++)
		dst[i] = (src[i] >= 0x20 && src[i] <= 0x7e) ? src[i] : '.';
	dst[i] = '\0';
}

static bool ccci_ex_type_is_assert(u8 type)
{
	/*
	 * port_kernel.c:3153-3175, 3200-3215: ASSERT, ASSERT DUMP and the
	 * DSP/MD32 ASSERT all carry EX_ASSERTFAIL_T in the content union.
	 */
	switch (type) {
	case CCCI_EX_TYPE_ASSERT:
	case CCCI_EX_TYPE_ASSERT_DUMP:
	case CCCI_EX_TYPE_ASSERT_FAIL:
	case CCCI_EX_TYPE_DSP_ASSERT:
		return true;
	default:
		return false;
	}
}

/*
 * Types whose content union is EX_FATALERR_T and therefore carries
 * error_code.code1/code2 (port_kernel.c:3130-3205).  INVALID, LOCKUP and the
 * EMI check have no error code in the ROM decoder.
 */
static bool ccci_ex_type_has_codes(u8 type)
{
	switch (type) {
	case CCCI_EX_TYPE_UNDEF:
	case CCCI_EX_TYPE_SWI:
	case CCCI_EX_TYPE_PREF_ABT:
	case CCCI_EX_TYPE_DATA_ABT:
	case CCCI_EX_TYPE_FATALERR_TASK:
	case CCCI_EX_TYPE_FATALERR_BUF:
	case CCCI_EX_TYPE_DSP_EXCEPTION:
	case CCCI_EX_TYPE_DSP_FATAL_ERROR:
	case CCCI_EX_TYPE_CROSS_CORE:
	case CCCI_EX_TYPE_EMI_MPU_VIOLATION:
	case CCCI_EX_TYPE_C2K_ERROR:
		return true;
	default:
		return false;
	}
}

/*
 * Decode one EX_LOG_T (downstream eccci/ccci_core.h:130-135).  The content
 * union is an assert record or a fatal-error record depending on ex_type;
 * the offender string sits at offset 288 of the record (port_kernel.h:29,
 * port_kernel.c:3160-3168).
 */
static void ccci_ex_decode_record(struct ccci_md *md, struct ccci_ex_text *t,
				  const char *source, const u8 *rec,
				  size_t len)
{
	const struct ccci_ex_log *log = (const struct ccci_ex_log *)rec;
	u32 code1, code2;
	bool is_assert, has_codes;
	char text[33];
	char file[33];

	ccci_ex_printf(t, "--- EX_LOG_T from %s (%zu of %zu bytes)\n", source,
		       len, sizeof(*log));
	if (len < sizeof(*log)) {
		ccci_ex_printf(t, "  record truncated\n");
		ccci_md_err(md, "exception record from %s truncated: %zu bytes\n",
			    source, len);
		return;
	}

	ccci_ex_printf(t,
		       "  header: ex_type %u (%s), nvram %u, serial %u\n",
		       log->header.ex_type,
		       ccci_ex_type_name(log->header.ex_type),
		       log->header.ex_nvram,
		       le16_to_cpu(log->header.ex_serial_num));
	ccci_ex_sanitize(text, sizeof(text), log->envinfo.execution_unit,
			 sizeof(log->envinfo.execution_unit));
	ccci_ex_printf(t,
		       "  envinfo: boot_mode %u, unit \"%s\", status 0x%02x, ELM_status 0x%02x\n",
		       log->envinfo.boot_mode, text, log->envinfo.status,
		       log->envinfo.elm_status);

	is_assert = ccci_ex_type_is_assert(log->header.ex_type);
	has_codes = ccci_ex_type_has_codes(log->header.ex_type);
	code1 = le32_to_cpu(log->content.fatalerr.error_code.code1);
	code2 = le32_to_cpu(log->content.fatalerr.error_code.code2);

	if (is_assert) {
		ccci_ex_sanitize(file, sizeof(file), log->content.assert.filename,
				 sizeof(log->content.assert.filename));
		ccci_ex_printf(t,
			       "  assert: file \"%s\", line %u, p1 0x%08x, p2 0x%08x, p3 0x%08x\n",
			       file,
			       le32_to_cpu(log->content.assert.linenumber),
			       le32_to_cpu(log->content.assert.parameters[0]),
			       le32_to_cpu(log->content.assert.parameters[1]),
			       le32_to_cpu(log->content.assert.parameters[2]));
	} else if (has_codes) {
		ccci_ex_printf(t,
			       "  fatal error: code1 0x%08x, code2 0x%08x\n",
			       code1, code2);
		/*
		 * EMI MPU / RMPU fault: port_kernel.c:2752-2765 names this case
		 * "Fatal error (rmpu violation)" and marks it as a fatal-error
		 * record.  EX_LOG_T only carries two error codes (EX_PL_LOG_T
		 * has a third); no other field is set by the modem for it.
		 */
		if (log->header.ex_type == CCCI_EX_TYPE_EMI_MPU_VIOLATION)
			ccci_ex_printf(t,
				       "  RMPU: the modem core violated an EMI MPU/RMPU region permission (ECT_SRC_RMPU, ccci_core.h:352)\n");
		if (log->header.ex_type == CCCI_EX_TYPE_C2K_ERROR &&
		    code1 == CCCI_EX_C2K_FATAL_ERR_CODE1)
			ccci_ex_printf(t,
				       "  C2K fatal error: no AEE DB trigger for this class (port_kernel.c:3100-3104)\n");
	} else {
		ccci_ex_printf(t, "  no error code for this exception type\n");
	}

	if (rec[CCCI_EXREC_OFFSET_OFFENDER] == CCCI_EX_OFFENDER_NONE) {
		ccci_ex_printf(t, "  offender: none (0x%02x marker)\n",
			       CCCI_EX_OFFENDER_NONE);
		text[0] = '\0';
	} else {
		ccci_ex_sanitize(text, sizeof(text),
				 rec + CCCI_EXREC_OFFSET_OFFENDER,
				 sizeof(*log) - CCCI_EXREC_OFFSET_OFFENDER);
		ccci_ex_printf(t, "  offender: \"%s\"\n", text);
	}

	if (is_assert)
		ccci_md_err(md,
			    "EX record from %s: type %u (%s), serial %u, boot_mode %u, status 0x%02x, assert file \"%s\" line %u, offender \"%s\"\n",
			    source, log->header.ex_type,
			    ccci_ex_type_name(log->header.ex_type),
			    le16_to_cpu(log->header.ex_serial_num),
			    log->envinfo.boot_mode, log->envinfo.status,
			    file, le32_to_cpu(log->content.assert.linenumber),
			    text);
	else if (has_codes)
		ccci_md_err(md,
			    "EX record from %s: type %u (%s), nvram %u, serial %u, boot_mode %u, status 0x%02x, ELM 0x%02x, code1 0x%08x, code2 0x%08x, offender \"%s\"\n",
			    source, log->header.ex_type,
			    ccci_ex_type_name(log->header.ex_type),
			    log->header.ex_nvram,
			    le16_to_cpu(log->header.ex_serial_num),
			    log->envinfo.boot_mode, log->envinfo.status,
			    log->envinfo.elm_status, code1, code2, text);
	else
		ccci_md_err(md,
			    "EX record from %s: type %u (%s), nvram %u, serial %u, boot_mode %u, status 0x%02x, ELM 0x%02x, offender \"%s\"\n",
			    source, log->header.ex_type,
			    ccci_ex_type_name(log->header.ex_type),
			    log->header.ex_nvram,
			    le16_to_cpu(log->header.ex_serial_num),
			    log->envinfo.boot_mode, log->envinfo.status,
			    log->envinfo.elm_status, text);
}

static bool ccci_ex_smem_range(struct ccci_md *md, size_t off, size_t len)
{
	return md->smem_vir && off <= md->smem_size &&
	       len <= md->smem_size - off;
}

/*
 * Dump one AP-visible shared-memory window.  The full window goes to the
 * snapshot and to the util dump ring; the kernel log is bounded to @klog_len
 * because it is the slowest destination.
 */
static void ccci_ex_dump_region(struct ccci_md *md, struct ccci_ex_text *t,
				const char *name, size_t off, size_t len,
				size_t klog_len)
{
	char prefix[64];
	u8 *buf;

	if (!ccci_ex_smem_range(md, off, len)) {
		ccci_ex_printf(t,
			       "--- %s: smem+0x%zx, 0x%zx bytes: unavailable (smem %s, size 0x%zx)\n",
			       name, off, len,
			       md->smem_vir ? "mapped" : "not mapped",
			       md->smem_size);
		ccci_md_warn(md, "dump %s unavailable (smem+0x%zx)\n", name,
			     off);
		return;
	}

	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf) {
		ccci_ex_printf(t, "--- %s: kvmalloc(0x%zx) failed\n", name,
			       len);
		return;
	}
	memcpy_fromio(buf, md->smem_vir + off, len);

	ccci_ex_printf(t, "--- %s: smem+0x%zx, 0x%zx bytes%s\n", name, off,
		       len, len > klog_len ? " (kernel log truncated)" : "");
	ccci_ex_hex(t, buf, len);

	snprintf(prefix, sizeof(prefix), "ccci: md%u %s: ", md->id + 1, name);
	print_hex_dump(KERN_ERR, prefix, DUMP_PREFIX_OFFSET, 16, 1, buf,
		       min_t(size_t, len, klog_len), false);

	/* persistent per-MD dump ring, downstream ccci_mem_dump() */
	ccci_util_dump_mem(md->id, buf, len);
	kvfree(buf);
}

/* MD image head, downstream MD_IMG_DUMP_SIZE == 256 (ccci_core.h:640) */
static void ccci_ex_dump_image(struct ccci_md *md, struct ccci_ex_text *t)
{
	u8 buf[CCCI_MD_IMG_DUMP_SIZE];

	if (!md->img_vir || md->img_size < sizeof(buf)) {
		ccci_ex_printf(t,
			       "--- MD image head: unavailable (image %s, size 0x%zx)\n",
			       md->img_vir ? "mapped" : "not mapped",
			       md->img_size);
		return;
	}

	memcpy_fromio(buf, md->img_vir, sizeof(buf));
	ccci_ex_printf(t, "--- MD image head: MD_IMG_DUMP_SIZE 0x%zx bytes\n",
		       sizeof(buf));
	ccci_ex_hex(t, buf, sizeof(buf));
	print_hex_dump(KERN_ERR, "ccci: md image head: ", DUMP_PREFIX_OFFSET,
		       16, 1, buf, sizeof(buf), false);
	ccci_util_dump_mem(md->id, buf, sizeof(buf));
}

static bool ccci_ex_dhl_ctrl_open(struct ccci_md *md)
{
	struct ccci_port *port;

	list_for_each_entry(port, &md->ports_list, node) {
		if (port->rx_ch == CCCI_UART1_RX)
			return atomic_read(&port->open_count) > 0;
	}
	return false;
}

static void ccci_ex_build(struct ccci_md *md, unsigned int reason,
			  unsigned int flags)
{
	struct ccci_ex_text t;
	struct ccci_exception *ex = &md->ex;
	unsigned long lflags;
	u8 packet[CCCI_EX_PACKET_MAX];
	u8 smem_record[sizeof(struct ccci_ex_log)];
	const u8 *record = NULL;
	const char *record_src = NULL;
	size_t record_len = 0;
	size_t packet_len = 0;
	bool packet_valid = false;
	bool dhl_open;
	char *buf;
	char *old;
	u32 count;

	buf = kvmalloc(CCCI_EX_DUMP_MAX, GFP_KERNEL);
	if (!buf) {
		ccci_md_err(md, "exception dump: cannot allocate snapshot\n");
		return;
	}

	t.buf = buf;
	t.size = CCCI_EX_DUMP_MAX;
	t.len = 0;
	t.buf[0] = '\0';

	spin_lock_irqsave(&ex->lock, lflags);
	if (ex->packet_valid && ex->packet_len <= sizeof(packet)) {
		packet_len = ex->packet_len;
		memcpy(packet, ex->packet, packet_len);
		packet_valid = true;
	}
	count = ex->dump_count + 1;
	spin_unlock_irqrestore(&ex->lock, lflags);

	dhl_open = ccci_ex_dhl_ctrl_open(md);

	ccci_ex_printf(&t, "CCCI exception dump md%u #%u\n", md->id + 1,
		       count);
	ccci_ex_printf(&t, " reason: %s\n",
		       ccci_ex_reason_name(reason));
	ccci_ex_aed_flags(&t, flags);
	ccci_ex_printf(&t,
		       " state %d, boot stage %u, exception stage %s (%u), exception type %u (%s)\n",
		       READ_ONCE(md->state), md->boot_stage,
		       ccci_exception_stage_name(md->ex_stage), md->ex_stage,
		       md->ex_type, ccci_ex_type_name(md->ex_type));
	ccci_ex_printf(&t,
		       " smem %pa+0x%zx, image %pa+0x%zx, transport %s\n",
		       &md->smem_phy, md->smem_size, &md->img_phy,
		       md->img_size,
		       md->cldma ? "CLDMA" : "CCIF-ring");

	/*
	 * The modem stamps 0xBAEBAE10 at smem+0xC64 when it took the software
	 * watchdog path; the ROM watchdog worker tests this word before it
	 * decides to reset (modem_cldma.c:1862-1872).
	 */
	if (ccci_ex_smem_range(md, CCCI_SMEM_OFFSET_EPON, sizeof(u32))) {
		u32 epon;

		memcpy_fromio(&epon, md->smem_vir + CCCI_SMEM_OFFSET_EPON,
			      sizeof(epon));
		ccci_ex_printf(&t,
			       " SW watchdog marker (smem+0x%x): 0x%08x%s\n",
			       CCCI_SMEM_OFFSET_EPON, epon,
			       epon == CCCI_SMEM_EPON_SW_WDT ?
			       " (MD reported SW WDT)" : "");
	}

	/*
	 * DHL: the ROM build leaves FEATURE_DHL_LOG_EN undefined (there is no
	 * definition in the downstream tree), so port_kernel.c:215-219
	 * publishes DHL_SHARE_MEMORY as CCCI_FEATURE_NOT_SUPPORT and no
	 * shared-memory carve-out exists.  What does exist is the protocol-type
	 * ioctl (port_char.c:571-582) and the META-mode close gate
	 * (port_char.c:79-90).
	 */
	ccci_ex_printf(&t,
		       " DHL: protocol \"DHL\", control port %s, md_boot_mode %u (%s), close gate %s\n",
		       dhl_open ? "open" : "closed", md->md_boot_mode,
		       md->md_boot_mode == CCCI_MD_BOOT_MODE_META ? "META" :
		       "normal",
		       (dhl_open && md->md_boot_mode == CCCI_MD_BOOT_MODE_META) ?
		       "blocks modem reset" : "allows modem reset");
	ccci_ex_printf(&t,
		       " DHL shared memory: NOT_SUPPORT (FEATURE_DHL_LOG_EN undefined in the ROM build)\n");

	/*
	 * Record source, downstream ccci_md_exception() (port_kernel.c:
	 * 3125-3135): the packet copy is used for MD3 and before the boot
	 * handshake, the shared-memory record afterwards.
	 */
	if ((md->id == CCCI_MD_SYS3 || md->boot_stage == CCCI_BOOT_STAGE_0) &&
	    packet_valid && packet_len > CCCI_HEADER_SIZE) {
		record = packet + CCCI_HEADER_SIZE;
		record_len = packet_len - CCCI_HEADER_SIZE;
		record_src = "MD_EX_REC_OK packet";
	} else if (ccci_ex_smem_range(md, CCCI_SMEM_OFFSET_EXREC,
				      sizeof(smem_record))) {
		memcpy_fromio(smem_record,
			      md->smem_vir + CCCI_SMEM_OFFSET_EXREC,
			      sizeof(smem_record));
		record = smem_record;
		record_len = sizeof(smem_record);
		record_src = "shared memory EXREC (smem+0x800)";
	} else if (packet_valid && packet_len > CCCI_HEADER_SIZE) {
		record = packet + CCCI_HEADER_SIZE;
		record_len = packet_len - CCCI_HEADER_SIZE;
		record_src = "MD_EX_REC_OK packet (no smem window)";
	}

	if (packet_valid) {
		ccci_ex_printf(&t,
			       "--- MD_EX_REC_OK packet (ccci_header + EX_LOG_T), %zu bytes\n",
			       packet_len);
		ccci_ex_hex(&t, packet, packet_len);
		print_hex_dump(KERN_ERR, "ccci: ex packet: ",
			       DUMP_PREFIX_OFFSET, 16, 1, packet,
			       min_t(size_t, packet_len, CCCI_EX_KLOG_BYTES),
			       false);
		ccci_util_dump_mem(md->id, packet, packet_len);
	} else {
		ccci_ex_printf(&t,
			       "--- MD_EX_REC_OK packet: not received\n");
	}

	/*
	 * Fixed-offset windows of the exception region, ccci_config.h:70-83.
	 * The CCCI debug record and the MDSS debug window together are the
	 * 4 KiB CCCI_SMEM_DUMP_SIZE window that md_ex_monitor_func() dumps
	 * (port_kernel.c:3411-3413); they are printed separately because they
	 * have different consumers.
	 */
	ccci_ex_dump_region(md, &t, "CCCI debug record (CCCI_SMEM_OFFSET_CCCI_DEBUG)",
			    CCCI_SMEM_OFFSET_CCCI_DEBUG,
			    CCCI_SMEM_CCCI_DEBUG_SIZE, CCCI_EX_KLOG_BYTES);
	ccci_ex_dump_region(md, &t, "MDSS debug / exception record (CCCI_SMEM_OFFSET_EXREC)",
			    CCCI_SMEM_OFFSET_MDSS_DEBUG,
			    CCCI_SMEM_MDSS_DEBUG_SIZE, CCCI_EX_KLOG_BYTES);

	/* CCIF SRAM header copy, only refreshed for the CCIF_REG selection */
	if (flags & CCCI_AED_DUMP_CCIF_REG)
		ccci_ex_dump_region(md, &t, "CCIF SRAM snapshot",
				    CCCI_SMEM_OFFSET_CCIF_SRAM,
				    CCCI_SMEM_CCIF_SRAM_SIZE,
				    CCCI_SMEM_CCIF_SRAM_SIZE);

	ccci_ex_dump_image(md, &t);

	if (record)
		ccci_ex_decode_record(md, &t, record_src, record, record_len);
	else
		ccci_ex_printf(&t,
			       "--- EX_LOG_T: unavailable (no packet, no smem window)\n");

	/* CCIF registers, existing core helper (prints to the kernel log) */
	ccci_ex_printf(&t,
		       "--- CCIF registers: dumped to the kernel log by md%u\n",
		       md->id + 1);
	ccci_ccif_dump_regs(md, NULL);

	if (t.len >= t.size)
		ccci_md_warn(md, "exception snapshot truncated at %zu bytes\n",
			     t.size);

	mutex_lock(&ex->dump_lock);
	old = ex->dump;
	ex->dump = buf;
	ex->dump_len = t.len;
	ex->dump_count = count;
	ex->dump_reason = reason;
	ex->dump_flags = flags;
	ex->dump_ex_stage = md->ex_stage;
	ex->dump_ex_type = md->ex_type;
	mutex_unlock(&ex->dump_lock);
	kvfree(old);

#if IS_ENABLED(CONFIG_MTK_AEE_FEATURE)
	/*
	 * Hand the exception record to the AEE client, exactly where downstream
	 * ccci_aed() does (port_kernel.c:2335-2336):
	 *
	 *  - the EX_LOG_T record selected above (MD_EX_REC_OK packet or the
	 *    EXREC window), port_kernel.c:2318-2333;
	 *  - the first MD_IMG_DUMP_SIZE bytes of the modem image,
	 *    port_kernel.c:2329-2333 (CCCI_MD_IMG_DUMP_SIZE == 256,
	 *    ccci_priv.h:852);
	 *  - the "mdN:<module>" detail string, port_kernel.c:2304.  The image
	 *    information suffix that downstream appends through
	 *    ccci_get_md_info_str() (port_kernel.c:2295) has no equivalent in
	 *    this tree, so the reason name is used as the module text;
	 *  - DB_OPT_DEFAULT | DB_OPT_FTRACE, port_kernel.c:2347.
	 */
	{
		u8 img[CCCI_MD_IMG_DUMP_SIZE];
		const int *img_p = NULL;
		int img_len = 0;
		char detail[64];

		if (md->img_vir && md->img_size >= sizeof(img)) {
			memcpy_fromio(img, md->img_vir, sizeof(img));
			img_p = (const int *)img;
			img_len = sizeof(img);
		}

		snprintf(detail, sizeof(detail), "md%u:%s", md->id + 1,
			 ccci_ex_reason_name(reason));

		aed_md_exception_api((const int *)record, (int)record_len,
				     img_p, img_len, detail,
				     DB_OPT_DEFAULT | DB_OPT_FTRACE);
	}
#endif

	ccci_md_err(md,
		    "exception dump #%u ready (%zu bytes, reason %s), full hex in ccci_dump/debugfs\n",
		    count, t.len, ccci_ex_reason_name(reason));
}

static void ccci_ex_work(struct work_struct *work)
{
	struct ccci_md *md = container_of(work, struct ccci_md, ex.work);
	struct ccci_exception *ex = &md->ex;
	unsigned long lflags;
	unsigned int reason, flags;

	for (;;) {
		spin_lock_irqsave(&ex->lock, lflags);
		if (!ex->ready || !ex->pending) {
			ex->worker_active = false;
			spin_unlock_irqrestore(&ex->lock, lflags);
			break;
		}
		reason = ex->pending_reason;
		flags = ex->pending_flags;
		ex->pending = false;
		spin_unlock_irqrestore(&ex->lock, lflags);

		ccci_ex_build(md, reason, flags);
	}
}

void ccci_exception_capture_packet(struct ccci_md *md, const void *data,
				   size_t len)
{
	struct ccci_exception *ex;
	unsigned long lflags;

	if (!md || !data || !len)
		return;

	ex = &md->ex;
	spin_lock_irqsave(&ex->lock, lflags);
	if (ex->ready) {
		ex->packet_len = min_t(size_t, len, sizeof(ex->packet));
		memcpy(ex->packet, data, ex->packet_len);
		ex->packet_valid = true;
	}
	spin_unlock_irqrestore(&ex->lock, lflags);
}

unsigned int ccci_exception_dump_flags(struct ccci_md *md)
{
	/*
	 * Downstream ccci_ee_info_dump() prefers the MD_EX_REC_OK packet while
	 * the modem is still in boot stage 0 and the shared-memory record
	 * afterwards (port_kernel.c:3125-3135, ccci_aed() selection at
	 * port_kernel.c:3079-3088).
	 */
	if (!md)
		return 0;
	return md->boot_stage == CCCI_BOOT_STAGE_0 ? CCCI_AED_DUMP_EX_PKT :
						     CCCI_AED_DUMP_EX_MEM;
}

void ccci_exception_trigger(struct ccci_md *md, unsigned int reason,
			    unsigned int flags)
{
	struct ccci_exception *ex;
	unsigned long lflags;

	if (!md)
		return;

	ex = &md->ex;
	spin_lock_irqsave(&ex->lock, lflags);
	if (ex->ready) {
		ex->pending = true;
		ex->pending_reason = reason;
		ex->pending_flags = flags;
		if (!ex->worker_active) {
			ex->worker_active = true;
			/* queued under the lock so exit's cancel_work_sync()
			 * is ordered after any in-flight trigger
			 */
			schedule_work(&ex->work);
		}
	}
	spin_unlock_irqrestore(&ex->lock, lflags);
}

void ccci_exception_trigger_sync(struct ccci_md *md, unsigned int reason,
				 unsigned int flags)
{
	if (!md)
		return;

	ccci_exception_trigger(md, reason, flags);
	/*
	 * The worker loops over all pending requests, so one flush covers the
	 * dump queued above.  No-op when the engine was never initialised.
	 */
	if (READ_ONCE(md->ex.ready))
		flush_work(&md->ex.work);
}

static size_t ccci_exception_packet_copy(struct ccci_md *md, void *dst,
					 size_t len)
{
	unsigned long lflags;
	size_t n;

	if (!md || !dst || !len)
		return 0;

	spin_lock_irqsave(&md->ex.lock, lflags);
	if (!md->ex.packet_valid) {
		n = 0;
	} else {
		n = min(len, md->ex.packet_len);
		memcpy(dst, md->ex.packet, n);
	}
	spin_unlock_irqrestore(&md->ex.lock, lflags);
	return n;
}

int ccci_exception_ccci_debug_window(struct ccci_md *md, void __iomem **vir,
				     size_t *size)
{
	/*
	 * downstream ccci_core.c:271-273 sets
	 * ccci_exp_smem_ccci_debug_vir = exp_smem_base_vir +
	 * CCCI_SMEM_OFFSET_CCCI_DEBUG (0) and
	 * ccci_exp_smem_ccci_debug_size = CCCI_SMEM_CCCI_DEBUG_SIZE (2048);
	 * aee_dump_ccci_debug_info() returns exactly those two values.
	 */
	if (!md || !vir || !size)
		return -EINVAL;
	if (!ccci_ex_smem_range(md, CCCI_SMEM_OFFSET_CCCI_DEBUG,
				CCCI_SMEM_CCCI_DEBUG_SIZE))
		return -ENODEV;

	*vir = md->smem_vir + CCCI_SMEM_OFFSET_CCCI_DEBUG;
	*size = CCCI_SMEM_CCCI_DEBUG_SIZE;
	return 0;
}

/* ------------------------------------------------------------------ */
/* debugfs per modem                                                   */
/* ------------------------------------------------------------------ */
static ssize_t ccci_ex_dbg_dump_read(struct file *file, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct ccci_md *md = file->private_data;
	ssize_t ret;

	mutex_lock(&md->ex.dump_lock);
	if (!md->ex.dump)
		ret = 0;
	else
		ret = simple_read_from_buffer(buf, count, ppos, md->ex.dump,
					      md->ex.dump_len);
	mutex_unlock(&md->ex.dump_lock);
	return ret;
}

static ssize_t ccci_ex_dbg_packet_read(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct ccci_md *md = file->private_data;
	u8 packet[CCCI_EX_PACKET_MAX];
	size_t len;

	len = ccci_exception_packet_copy(md, packet, sizeof(packet));
	return simple_read_from_buffer(buf, count, ppos, packet, len);
}

static ssize_t ccci_ex_dbg_stage_read(struct file *file, char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct ccci_md *md = file->private_data;
	char text[640];
	bool dhl_open = ccci_ex_dhl_ctrl_open(md);
	bool meta = md->md_boot_mode == CCCI_MD_BOOT_MODE_META;
	int len;

	len = scnprintf(text, sizeof(text),
			"state %d, boot stage %u\n"
			"exception stage %u (%s), exception type %u (%s)\n"
			"md_boot_mode %u (%s), DHL control port %s\n"
			"DHL META close gate: %s\n"
			"dumps %u, last reason %s, AED flags 0x%x\n"
			"last stage %s (%u), last type %u, packet %s\n",
			READ_ONCE(md->state), md->boot_stage,
			md->ex_stage, ccci_exception_stage_name(md->ex_stage),
			md->ex_type, ccci_ex_type_name(md->ex_type),
			md->md_boot_mode, meta ? "META" : "normal",
			dhl_open ? "open" : "closed",
			(dhl_open && meta) ?
			"refuses modem reset" : "idle",
			md->ex.dump_count,
			ccci_ex_reason_name(md->ex.dump_reason),
			md->ex.dump_flags,
			ccci_exception_stage_name(md->ex.dump_ex_stage),
			md->ex.dump_ex_stage, md->ex.dump_ex_type,
			READ_ONCE(md->ex.packet_valid) ? "captured" : "absent");

	return simple_read_from_buffer(buf, count, ppos, text, len);
}

static const struct file_operations ccci_ex_dbg_dump_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ccci_ex_dbg_dump_read,
	.llseek = default_llseek,
};

static const struct file_operations ccci_ex_dbg_packet_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ccci_ex_dbg_packet_read,
	.llseek = default_llseek,
};

static const struct file_operations ccci_ex_dbg_stage_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ccci_ex_dbg_stage_read,
	.llseek = default_llseek,
};

static struct dentry *ccci_ex_dbg_create(struct ccci_md *md, const char *name,
					 const struct file_operations *fops)
{
	struct dentry *d;

	d = debugfs_create_file(name, 0444, md->dbg_dir, md, fops);
	if (IS_ERR_OR_NULL(d)) {
		ccci_md_dbg(md, "cannot create debugfs %s (%ld)\n", name,
			    IS_ERR(d) ? PTR_ERR(d) : 0);
		return NULL;
	}
	return d;
}

int ccci_exception_init(struct ccci_md *md)
{
	struct ccci_exception *ex = &md->ex;

	/*
	 * The wire format must stay byte-identical to the modem's EX_LOG_T;
	 * every offset above depends on it (ccci_core.h:130-135).
	 */
	BUILD_BUG_ON(sizeof(struct ccci_ex_log) != 512);
	BUILD_BUG_ON(sizeof(struct ccci_ex_header) != 4);
	BUILD_BUG_ON(sizeof(struct ccci_ex_env_info) != 164);
	BUILD_BUG_ON(sizeof(union ccci_ex_content) != 296);
	BUILD_BUG_ON(CCCI_EXREC_OFFSET_OFFENDER >= sizeof(struct ccci_ex_log));
	/* downstream CCCI_SMEM_DUMP_SIZE (0x1000) covers both dump windows,
	 * mt6735/ccci_config.h:70-83
	 */
	BUILD_BUG_ON(CCCI_SMEM_OFFSET_MDSS_DEBUG + CCCI_SMEM_MDSS_DEBUG_SIZE >
		     CCCI_SMEM_DUMP_SIZE);

	spin_lock_init(&ex->lock);
	mutex_init(&ex->dump_lock);
	INIT_WORK(&ex->work, ccci_ex_work);

	ex->dump = kvmalloc(CCCI_EX_DUMP_MAX, GFP_KERNEL);
	if (!ex->dump) {
		mutex_destroy(&ex->dump_lock);
		return -ENOMEM;
	}
	ex->dump[0] = '\0';
	ex->dump_len = 0;

	if (md->dbg_dir) {
		ex->dbg_dump = ccci_ex_dbg_create(md, "exception_dump",
						  &ccci_ex_dbg_dump_fops);
		ex->dbg_packet = ccci_ex_dbg_create(md, "exception_packet",
						    &ccci_ex_dbg_packet_fops);
		ex->dbg_stage = ccci_ex_dbg_create(md, "exception_stage",
						   &ccci_ex_dbg_stage_fops);
	} else {
		ccci_md_dbg(md, "debugfs unavailable, exception snapshot only in ccci_dump\n");
	}

	spin_lock_irq(&ex->lock);
	ex->ready = true;
	spin_unlock_irq(&ex->lock);
	return 0;
}

void ccci_exception_exit(struct ccci_md *md)
{
	struct ccci_exception *ex = &md->ex;
	unsigned long lflags;

	if (!ex->ready && !ex->dump)
		return;

	spin_lock_irqsave(&ex->lock, lflags);
	ex->ready = false;
	ex->pending = false;
	spin_unlock_irqrestore(&ex->lock, lflags);

	cancel_work_sync(&ex->work);

	debugfs_remove(ex->dbg_dump);
	debugfs_remove(ex->dbg_packet);
	debugfs_remove(ex->dbg_stage);
	ex->dbg_dump = NULL;
	ex->dbg_packet = NULL;
	ex->dbg_stage = NULL;

	mutex_lock(&ex->dump_lock);
	kvfree(ex->dump);
	ex->dump = NULL;
	ex->dump_len = 0;
	mutex_unlock(&ex->dump_lock);
	mutex_destroy(&ex->dump_lock);
}
