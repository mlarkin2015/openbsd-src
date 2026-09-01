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

#include <machine/psl.h>
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
static int mmio_last_dir;
static uint64_t mem_write_addr;
static uint64_t mem_write_value;
static size_t mem_write_len;

static int
test_mmio(uint32_t vcpu_id, int dir, uint64_t addr, uint64_t *data)
{
	(void)vcpu_id;
	(void)addr;
	mmio_last_dir = dir;

	if (dir != MMIO_DIR_READ)
		return -1;
	*data = mmio_read_value;
	return 0;
}

static void
test_testl_lapic_icr_busy(void)
{
	/* testl $LAPIC_DLSTAT_BUSY, local_apic+LAPIC_ICRLO(%rip) */
	static const uint8_t bytes[] = {
		0xf7, 0x05, 0xf6, 0x02, 0xe0, 0x0e,
		0x00, 0x10, 0x00, 0x00
	};
	struct vm_exit exit;
	struct x86_insn insn;
	uint64_t expected_flags, old_rax;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xfffffffff0000000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RAX] = old_rax = 0xfeedfaceULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] =
	    PSL_MBO | PSL_I | PSL_C | PSL_AF | PSL_N | PSL_V;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode LAPIC ICR busy TEST");
	if (insn.insn_gva != 0xfffffffffee00300ULL)
		errx(1, "TEST decoded address 0x%llx",
		    (unsigned long long)insn.insn_gva);
	if (insn.insn_immediate_len != 4 ||
	    insn.insn_immediate != 0x1000)
		errx(1, "TEST decoded immediate 0x%llx/%u",
		    (unsigned long long)insn.insn_immediate,
		    insn.insn_immediate_len);

	mmio_read_value = 0;
	mmio_last_dir = -1;
	if (insn_emulate(&exit, &insn, 1) != 0)
		errx(1, "could not emulate LAPIC ICR busy TEST");
	if (mmio_last_dir != MMIO_DIR_READ)
		errx(1, "TEST performed MMIO direction %d", mmio_last_dir);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RAX] != old_rax)
		errx(1, "TEST modified RAX to 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RAX]);
	expected_flags = PSL_MBO | PSL_I | PSL_PF | PSL_Z;
	if (exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] != expected_flags)
		errx(1, "TEST produced RFLAGS 0x%llx, expected 0x%llx",
		    (unsigned long long)
		    exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS],
		    (unsigned long long)expected_flags);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RIP] !=
	    0xfffffffff0000000ULL + sizeof(bytes))
		errx(1, "TEST advanced RIP to 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RIP]);
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

static void
test_andl_mmio(void)
{
	static const uint8_t bytes[] = { 0x23, 0x08 };
	struct vm_exit exit;
	struct x86_insn insn;
	uint64_t expected_flags;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xffffffff80000000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RAX] = 0xfffffffff0001020ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RCX] = 0xfeedfaceffff00ffULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] =
	    PSL_MBO | PSL_I | PSL_C | PSL_PF | PSL_AF | PSL_Z | PSL_N | PSL_V;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode 32-bit MMIO AND");
	mmio_read_value = 0x00ff0f0f;
	if (insn_emulate(&exit, &insn, 0) != 0)
		errx(1, "could not emulate 32-bit MMIO AND");
	if (exit.vrs.vrs_gprs[VCPU_REGS_RCX] != 0x00ff000f)
		errx(1, "32-bit AND produced RCX 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RCX]);
	expected_flags = PSL_MBO | PSL_I | PSL_PF;
	if (exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] != expected_flags)
		errx(1, "32-bit AND produced RFLAGS 0x%llx, expected 0x%llx",
		    (unsigned long long)
		    exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS],
		    (unsigned long long)expected_flags);
}

static void
test_subl_mmio(void)
{
	static const uint8_t bytes[] = { 0x2b, 0x08 };
	struct vm_exit exit;
	struct x86_insn insn;
	uint64_t expected_flags;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_D;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xd0100000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RAX] = 0xfec00390ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RCX] = 0xfeedface00000001ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] =
	    PSL_MBO | PSL_I | PSL_Z | PSL_V;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode 32-bit MMIO SUB");
	mmio_read_value = 2;
	mmio_last_dir = -1;
	if (insn_emulate(&exit, &insn, 0) != 0)
		errx(1, "could not emulate 32-bit MMIO SUB");
	if (mmio_last_dir != MMIO_DIR_READ)
		errx(1, "SUB performed MMIO direction %d", mmio_last_dir);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RCX] != 0xffffffffULL)
		errx(1, "32-bit SUB produced RCX 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RCX]);
	expected_flags = PSL_MBO | PSL_I | PSL_C | PSL_PF | PSL_AF | PSL_N;
	if (exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] != expected_flags)
		errx(1, "SUB produced RFLAGS 0x%llx, expected 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS],
		    (unsigned long long)expected_flags);
}

static void
test_cmpl_mmio(void)
{
	static const uint8_t bytes[] = {
		0x3b, 0x1d, 0x80, 0xd0, 0xf3, 0xd0
	};
	struct vm_exit exit;
	struct x86_insn insn;
	uint64_t expected_flags, old_rbx;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_D;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xd08bac0fULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RBX] = old_rbx =
	    0xfeedface00000001ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] =
	    PSL_MBO | PSL_I | PSL_Z | PSL_V;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode 32-bit MMIO CMP");
	if (insn.insn_gva != 0xd0f3d080ULL)
		errx(1, "CMP decoded address 0x%llx",
		    (unsigned long long)insn.insn_gva);

	mmio_read_value = 2;
	mmio_last_dir = -1;
	if (insn_emulate(&exit, &insn, 0) != 0)
		errx(1, "could not emulate 32-bit MMIO CMP");
	if (mmio_last_dir != MMIO_DIR_READ)
		errx(1, "CMP performed MMIO direction %d", mmio_last_dir);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RBX] != old_rbx)
		errx(1, "CMP modified RBX to 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RBX]);
	expected_flags = PSL_MBO | PSL_I | PSL_C | PSL_PF | PSL_AF | PSL_N;
	if (exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS] != expected_flags)
		errx(1, "CMP produced RFLAGS 0x%llx, expected 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RFLAGS],
		    (unsigned long long)expected_flags);
}

static void
test_pushl_mmio(void)
{
	static const uint8_t bytes[] = {
		0xff, 0x35, 0x80, 0xd0, 0xf3, 0xd0
	};
	struct vm_exit exit;
	struct x86_insn insn;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_D;
	exit.vrs.vrs_sregs[VCPU_REGS_SS].vsi_base = 0;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xd08bd428ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RSP] = 0xfeedfacec0101000ULL;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode 32-bit MMIO PUSH");
	if (insn.insn_gva != 0xd0f3d080ULL)
		errx(1, "PUSH decoded address 0x%llx",
		    (unsigned long long)insn.insn_gva);

	mmio_read_value = 0x89abcdefULL;
	mmio_last_dir = -1;
	mem_write_addr = 0;
	mem_write_value = 0;
	mem_write_len = 0;
	if (insn_emulate(&exit, &insn, 0) != 0)
		errx(1, "could not emulate 32-bit MMIO PUSH");
	if (mmio_last_dir != MMIO_DIR_READ)
		errx(1, "PUSH performed MMIO direction %d", mmio_last_dir);
	if (mem_write_addr != 0xc0100ffcULL || mem_write_len != 4 ||
	    mem_write_value != 0x89abcdefULL)
		errx(1, "PUSH wrote 0x%llx/%zu at 0x%llx",
		    (unsigned long long)mem_write_value, mem_write_len,
		    (unsigned long long)mem_write_addr);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RSP] !=
	    0xfeedfacec0100ffcULL)
		errx(1, "PUSH produced RSP 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RSP]);
	if (exit.vrs.vrs_gprs[VCPU_REGS_RIP] !=
	    0xd08bd428ULL + sizeof(bytes))
		errx(1, "PUSH advanced RIP to 0x%llx",
		    (unsigned long long)exit.vrs.vrs_gprs[VCPU_REGS_RIP]);
}

static void
test_movl_immediate_rex_b(void)
{
	static const uint8_t bytes[] = {
		0x41, 0xc7, 0x06, 0x01, 0x00, 0x00, 0x00
	};
	struct vm_exit exit;
	struct x86_insn insn;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xffffffff8115e540ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RSI] = 0xffffffff827d6e50ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_R14] = 0xfffff800fec00000ULL;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode REX.B MMIO MOV immediate");
	if (insn.insn_gva != exit.vrs.vrs_gprs[VCPU_REGS_R14])
		errx(1, "REX.B MOV decoded address 0x%llx, expected R14",
		    (unsigned long long)insn.insn_gva);
	if (insn.insn_immediate_len != 4 || insn.insn_immediate != 1)
		errx(1, "REX.B MOV decoded immediate 0x%llx/%u",
		    (unsigned long long)insn.insn_immediate,
		    insn.insn_immediate_len);
}

static void
test_movl_rex_sib(void)
{
	static const uint8_t bytes[] = { 0x43, 0x8b, 0x04, 0x8c };
	struct vm_exit exit;
	struct x86_insn insn;
	uint64_t expected;

	memset(&exit, 0, sizeof(exit));
	exit.vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit.vrs.vrs_crs[VCPU_REGS_CR4] = CR4_PAE;
	exit.vrs.vrs_msrs[VCPU_REGS_EFER] = EFER_LME;
	exit.vrs.vrs_sregs[VCPU_REGS_CS].vsi_ar = CS_L;
	exit.vrs.vrs_gprs[VCPU_REGS_RIP] = 0xffffffff80000000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_RSP] = 0x11111111;
	exit.vrs.vrs_gprs[VCPU_REGS_RCX] = 0x22222222;
	exit.vrs.vrs_gprs[VCPU_REGS_R12] = 0xfffff800fec00000ULL;
	exit.vrs.vrs_gprs[VCPU_REGS_R9] = 4;
	exit.vee.vee_insn_len = sizeof(bytes);
	memcpy(exit.vee.vee_insn_bytes, bytes, sizeof(bytes));

	if (insn_decode(&exit, &insn) != 0)
		errx(1, "could not decode REX.B/REX.X SIB MMIO MOV");
	expected = exit.vrs.vrs_gprs[VCPU_REGS_R12] +
	    exit.vrs.vrs_gprs[VCPU_REGS_R9] * 4;
	if (insn.insn_gva != expected)
		errx(1, "REX SIB MOV decoded address 0x%llx, expected 0x%llx",
		    (unsigned long long)insn.insn_gva,
		    (unsigned long long)expected);
}

int
main(void)
{
	test_testl_lapic_icr_busy();
	test_lapic_eoi_absolute_sib();
	test_movl_zero_extends();
	test_andl_mmio();
	test_subl_mmio();
	test_cmpl_mmio();
	test_pushl_mmio();
	test_movl_immediate_rex_b();
	test_movl_rex_sib();
	return 0;
}

/* Unused by decoder-only tests, but referenced by x86_mmio.c. */
int
translate_gva(struct vm_exit *exit, uint64_t gva, uint64_t *gpa, int prot)
{
	(void)exit;
	(void)prot;
	if ((gva & 0xfffff000ULL) == 0xd0f3d000ULL)
		*gpa = 0xfee00000ULL | (gva & 0xfff);
	else
		*gpa = gva & 0xffffffff;
	return 0;
}

int
write_mem(paddr_t gpa, const void *buf, size_t len)
{
	mem_write_addr = gpa;
	mem_write_value = 0;
	memcpy(&mem_write_value, buf, len);
	mem_write_len = len;
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
