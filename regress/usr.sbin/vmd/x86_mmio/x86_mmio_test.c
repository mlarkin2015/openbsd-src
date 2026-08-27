/*	$OpenBSD$ */

/*
 * Copyright (c) 2026 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <machine/specialreg.h>
#include <machine/vmmvar.h>

#include <err.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "mmio.h"
#include "vmd.h"
#include "x86_mmio.h"

static uint64_t mmio_read_value;

static int
test_mmio(uint32_t vcpu_id, int dir, uint64_t addr, uint64_t *data)
{
	(void)vcpu_id;
	(void)addr;

	if (dir != MMIO_DIR_READ)
		return -1;
	*data = mmio_read_value;
	return 0;
}

static void
test_lapic_eoi_absolute_sib(void)
{
	static const uint8_t bytes[] = {
		0xc7, 0x04, 0x25, 0xb0, 0x40, 0xb1, 0x82,
		0x00, 0x00, 0x00, 0x00
	};
	struct vm_exit exit;
	struct x86_insn insn;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xffffffff819b6406ULL;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode absolute SIB LAPIC EOI");
	if (insn.insn_bytes_len != sizeof(bytes))
		errx(1, "decoded length %u, expected %zu",
		    insn.insn_bytes_len, sizeof(bytes));
	if (insn.insn_gva != 0xffffffff82b140b0ULL)
		errx(1, "decoded address 0x%llx",
		    (unsigned long long)insn.insn_gva);
	if (insn.insn_immediate_len != 4 || insn.insn_immediate != 0)
		errx(1, "decoded immediate 0x%llx/%u",
		    (unsigned long long)insn.insn_immediate,
		    insn.insn_immediate_len);
}

static void
test_movl_zero_extends(void)
{
	static const uint8_t bytes[] = { 0x8b, 0x00 };
	struct vm_exit exit;
	struct x86_insn insn;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xffffffff80000000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RAX] = 0xfffffffff0001000ULL;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode 32-bit MMIO MOV");
	mmio_read_value = 0xe031;
	if (insn_emulate(&exit, &insn, 0) != 0)
		errx(1, "could not emulate 32-bit MMIO MOV");
	if (exit.vrs.vrs_gprs[VCPU_REGS_RAX] != mmio_read_value)
		errx(1, "32-bit MOV produced RAX 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RAX]);
}

int
main(void)
{
	test_lapic_eoi_absolute_sib();
	test_movl_zero_extends();
	return 0;
}

/* Unused by decoder-only tests, but referenced by x86_mmio.c. */
int
translate_gva(struct vm_exit *exit, uint64_t gva, uint64_t *gpa, int prot)
{
	(void)exit;
	(void)prot;
	*gpa = gva & 0xffffffff;
	return 0;
}

mmio_dev_fn_t
mmio_find_dev(paddr_t gpa)
{
	(void)gpa;
	return test_mmio;
}

void
log_info(const char *fmt, ...)
{
	(void)fmt;
}

void
log_warnx(const char *fmt, ...)
{
	(void)fmt;
}

__dead void
fatalx(const char *fmt, ...)
{
	(void)fmt;
	abort();
}
