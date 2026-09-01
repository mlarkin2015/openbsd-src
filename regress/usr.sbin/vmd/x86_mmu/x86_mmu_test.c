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

#include <sys/mman.h>
#include <sys/types.h>

#include <machine/pte.h>
#include <machine/specialreg.h>
#include <machine/vmmvar.h>

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vmd.h"
#include "x86_vm.h"

#define	TEST_MEM_SIZE	0x5000
#define	PGDIR_GPA	0x1000
#define	PD_GPA		0x2000
#define	PT_GPA		0x3000

static uint8_t test_mem[TEST_MEM_SIZE];

static void
store32(paddr_t gpa, uint32_t value)
{
	if (gpa > sizeof(test_mem) - sizeof(value))
		err(1, "store32 outside test memory");
	memcpy(&test_mem[gpa], &value, sizeof(value));
}

static uint32_t
load32(paddr_t gpa)
{
	uint32_t value;

	if (gpa > sizeof(test_mem) - sizeof(value))
		err(1, "load32 outside test memory");
	memcpy(&value, &test_mem[gpa], sizeof(value));
	return (value);
}

static void
store64(paddr_t gpa, uint64_t value)
{
	if (gpa > sizeof(test_mem) - sizeof(value))
		err(1, "store64 outside test memory");
	memcpy(&test_mem[gpa], &value, sizeof(value));
}

static void
init_exit(struct vm_exit *exit, uint64_t cr3, uint64_t cr4)
{
	memset(exit, 0, sizeof(*exit));
	exit->vrs.vrs_crs[VCPU_REGS_CR0] = CR0_PE | CR0_PG;
	exit->vrs.vrs_crs[VCPU_REGS_CR3] = cr3;
	exit->vrs.vrs_crs[VCPU_REGS_CR4] = cr4;
}

static void
test_i386_4k_high_frame(void)
{
	const uint64_t va = 0xd0001004ULL;
	const uint64_t expected = 0xfec00004ULL;
	const uint32_t pdidx = (va >> 22) & 0x3ff;
	const uint32_t ptidx = (va >> 12) & 0x3ff;
	const paddr_t pde_gpa = PGDIR_GPA + pdidx * sizeof(uint32_t);
	const paddr_t pte_gpa = PT_GPA + ptidx * sizeof(uint32_t);
	struct vm_exit exit;
	uint64_t gpa = 0;
	uint32_t pde, pte;

	memset(test_mem, 0, sizeof(test_mem));
	store32(pde_gpa, PT_GPA | PG_V | PG_RW);
	store32(pte_gpa, 0xfec00000U | PG_V | PG_RW);
	init_exit(&exit, PGDIR_GPA | PG_WT, 0);

	if (translate_gva(&exit, va, &gpa, PROT_WRITE) != 0)
		errx(1, "i386 4K high-frame translation failed");
	if (gpa != expected)
		errx(1, "i386 4K translated 0x%llx to 0x%llx, expected 0x%llx",
		    (unsigned long long)va, (unsigned long long)gpa,
		    (unsigned long long)expected);

	pde = load32(pde_gpa);
	pte = load32(pte_gpa);
	if (!(pde & PG_U) || !(pte & PG_U) || !(pte & PG_M))
		errx(1, "i386 4K did not update page-table state");
}

static void
test_i386_4m_high_frame(void)
{
	const uint64_t va = 0xd0123456ULL;
	const uint64_t expected = 0xf8123456ULL;
	const uint32_t pdidx = (va >> 22) & 0x3ff;
	const paddr_t pde_gpa = PGDIR_GPA + pdidx * sizeof(uint32_t);
	struct vm_exit exit;
	uint64_t gpa = 0;

	memset(test_mem, 0, sizeof(test_mem));
	store32(pde_gpa, 0xf8000000U | PG_V | PG_RW | PG_PS);
	init_exit(&exit, PGDIR_GPA | PG_WT, CR4_PSE);

	if (translate_gva(&exit, va, &gpa, PROT_READ) != 0)
		errx(1, "i386 4M high-frame translation failed");
	if (gpa != expected)
		errx(1, "i386 4M translated 0x%llx to 0x%llx, expected 0x%llx",
		    (unsigned long long)va, (unsigned long long)gpa,
		    (unsigned long long)expected);
}

static void
test_i386_pae_high_frame(void)
{
	const uint64_t va = 0xc0001004ULL;
	const uint64_t expected = 0xfec00004ULL;
	const uint32_t pdptidx = (va >> 30) & 0x3;
	const uint32_t pdidx = (va >> 21) & 0x1ff;
	const uint32_t ptidx = (va >> 12) & 0x1ff;
	struct vm_exit exit;
	uint64_t gpa = 0, pdpte;

	memset(test_mem, 0, sizeof(test_mem));
	pdpte = PD_GPA | PG_V | PG_WT;
	store64(PGDIR_GPA + pdptidx * sizeof(uint64_t), pdpte);
	store64(PD_GPA + pdidx * sizeof(uint64_t), PT_GPA | PG_V | PG_RW);
	store64(PT_GPA + ptidx * sizeof(uint64_t),
	    0xfec00000ULL | PG_V | PG_RW);
	init_exit(&exit, PGDIR_GPA | PG_WT, CR4_PAE);

	if (translate_gva(&exit, va, &gpa, PROT_WRITE) != 0)
		errx(1, "i386 PAE high-frame translation failed");
	if (gpa != expected)
		errx(1, "i386 PAE translated 0x%llx to 0x%llx, expected 0x%llx",
		    (unsigned long long)va, (unsigned long long)gpa,
		    (unsigned long long)expected);
	if (memcmp(&test_mem[PGDIR_GPA + pdptidx * sizeof(uint64_t)],
	    &pdpte, sizeof(pdpte)) != 0)
		errx(1, "i386 PAE modified reserved PDPTE state");
}

int
main(void)
{
	test_i386_4k_high_frame();
	test_i386_4m_high_frame();
	test_i386_pae_high_frame();
	return (0);
}

int
read_mem(paddr_t src, void *buf, size_t len)
{
	if (len > sizeof(test_mem) || src > sizeof(test_mem) - len)
		return (EINVAL);
	memcpy(buf, &test_mem[src], len);
	return (0);
}

int
write_mem(paddr_t dst, const void *buf, size_t len)
{
	if (len > sizeof(test_mem) || dst > sizeof(test_mem) - len)
		return (EINVAL);
	memcpy(&test_mem[dst], buf, len);
	return (0);
}

void
log_debug(const char *fmt, ...)
{
	(void)fmt;
}

void
log_warn(const char *fmt, ...)
{
	(void)fmt;
}
