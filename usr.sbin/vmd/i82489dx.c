/*	$OpenBSD */

/*
 * Copyright (c) 2025 Mike Larkin <mlarkin@openbsd.org>
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

#include <strings.h>

#include <machine/i82489reg.h>

#include "i82489dx.h"
#include "i82093aa.h"
#include "mmio.h"
#include "vmd.h"

struct i82489dx {
	uint64_t	base;
	uint32_t	ver;
	uint32_t	tpr;
	uint32_t	svr;

	uint32_t	isr[8];
	uint32_t	irr[8];

	uint32_t	curvec;
};

/* XXX will need one per cpu, which entails a per-vcpu init loop in x86_vm.c
 *     for cases like this. The vcpu in use will also need to be passed
 *     through to all io functions.
 */
struct i82489dx		lapic;

uint32_t i82489dx_get_version(void);
uint32_t i82489dx_get_tpr(void);
void i82489dx_set_tpr(uint32_t);

uint32_t
i82489dx_get_version(void)
{
	log_warnx("%s: returning 0x%x", __func__, lapic.ver);

	return lapic.ver;
}

uint32_t
i82489dx_get_tpr(void)
{
	log_warnx("%s: returning 0x%x", __func__, lapic.tpr);

	return lapic.tpr;
}

void
i82489dx_set_tpr(uint32_t tpr)
{
	log_warnx("%s: setting tpr=0x%x", __func__, tpr);

	lapic.tpr = tpr;
}

int
i82489dx_mmio(int dir, paddr_t addr, uint64_t *data)
{
	uint16_t reg;
	uint32_t d;

	log_warnx("%s: dir=%d addr=0x%lx data=0x%llx", __func__, dir, addr,
	    *data);

	reg = addr - lapic.base;
	if (reg > 0xFFF) {
		log_warnx("%s: invalid i82489 register 0x%x", __func__, reg);
		return 1;
	}

	switch (reg) {
	case LAPIC_VERS:
		if (dir == MMIO_DIR_READ) {
			*data &= 0xFFFFFFFF00000000;
			*data |= i82489dx_get_version();
			log_warnx("%s: read LAPIC_VERS: return 0x%llx",
			    __func__, *data);
		} else {
			log_warnx("%s: write to reg 0x%x discarded", __func__,
			    reg);
		}
		break;
	case LAPIC_TPRI:
		if (dir == MMIO_DIR_READ) {
			*data &= 0xFFFFFFFF00000000;
			*data |= i82489dx_get_tpr();
			log_warnx("%s: read LAPIC_TPR: return 0x%llx",
			    __func__, *data);
		} else {
			d = (uint32_t)(*data);

			log_warnx("%s: setting LAPIC_TPR=0x%x",
			    __func__, d);
			i82489dx_set_tpr(d);
		}
		break;
	case LAPIC_SVR:
		if (dir == MMIO_DIR_READ) {
			*data &= 0xFFFFFFFF00000000;
			*data |= lapic.svr;
			log_warnx("%s: read LAPIC_SVR: return 0x%llx",
			    __func__, *data);
		} else {
			d = (uint32_t)(*data);

			log_warnx("%s: setting LAPIC_SVR=0x%x",
			    __func__, d);
			lapic.svr = d;
		}
	default:
		if (dir == MMIO_DIR_READ) {
			log_warnx("%s: unsupported i/o on i82489 reg 0x%x. "
			    "returning 0xFFFFFFFFFFFFFFFF", __func__, reg);
			*data = 0xFFFFFFFFFFFFFFFF;
		} else {
			log_warnx("%s: discarding write to reg 0x%x", __func__,
			    reg);
		}
	}

	return 0;
}

void
i82489dx_init(void)
{
	lapic.ver = (1ULL << 31) | (6ULL << LAPIC_VERSION_LVT_SHIFT) | 0x10;
	lapic.base = LAPIC_BASE;

	mmio_dev_add(LAPIC_BASE, LAPIC_BASE + 0xFFF,
	    (mmio_dev_fn_t)i82489dx_mmio);
}

int
i82489dx_enabled(void)
{
	return (lapic.svr & LAPIC_SVR_ENABLE) != 0;
}

void
i82489dx_vector_irq(int apic_id, int destmode, uint8_t vector, int level)
{
	if (!i82489dx_enabled()) {
		log_warnx("%s: vector irq %d but lapic is disabled",
		    __func__, vector);
		return;
	}

	log_warnx("%s: received irq from ioapic: vec=%d",
	    __func__, vector);
	i82489dx_set_irr(vector);
}

/* return highest prio vector, 0xFFFF if nothing pending */
int
i82489dx_get_highest_irr(void)
{
	int vector = 0xFFFF, i, base, j;

	for (i = 7; i >=0; i--) {
		base = 32 * i;
		j = ffsl(lapic.irr[i]);
		if (j) {
			vector = (j - 1) + base;
			log_warnx("%s: found highest IRR=%d", __func__, vector);
			return vector;
		}
	}
	log_warnx("%s: no bits set in IRR", __func__);

	return vector;
}

/* return highest isr, 0xFFFF if nothing inservice */
int
i82489dx_get_highest_isr(void)
{
	int vector = 0xFFFF, i, base, j;

	for (i = 7; i >=0; i--) {
		base = 32 * i;
		j = ffsl(lapic.isr[i]);
		if (j) {
			vector = (j - 1) + base;
			log_warnx("%s: found highest ISR=%d", __func__, vector);
			return vector;
		}
	}
	log_warnx("%s: no bits set in ISR", __func__);

	return vector;
}

/* return 1 if anything pending, 0 otherwise */
int
i82489dx_is_pending(int vcpu_id)
{
	if (!i82489dx_enabled())
		return 0;

	return (i82489dx_get_highest_irr() != 0xFFFF);
}

void
i82489dx_clear_isr(int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	log_warnx("%s: clearing ISR vector %d (base=%d bit=%d)",
	    __func__, vector, base, ofs);

	lapic.isr[base] &= ~(1ULL << ofs);
}

void
i82489dx_set_isr(int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	log_warnx("%s: setting ISR vector %d (base=%d bit=%d)",
	    __func__, vector, base, ofs);

	lapic.isr[base] |= ~(1ULL << ofs);
}

void
i82489dx_set_irr(int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	log_warnx("%s: setting IRR vector %d (base=%d bit=%d)",
	    __func__, vector, base, ofs);

	lapic.irr[base] |= ~(1ULL << ofs);
}

void
i82489dx_clear_irr(int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	log_warnx("%s: clearing IRR vector %d (base=%d bit=%d)",
	    __func__, vector, base, ofs);

	lapic.irr[base] &= ~(1ULL << ofs);
}

void
i82489dx_eoi(void)
{
	int vector;

	if (!i82489dx_enabled()) {
		log_warnx("%s: eoi but lapic disabled", __func__);
		return;
	}

	log_warnx("%s: finding highest ISR", __func__);

	vector = i82489dx_get_highest_isr();
	if (vector == 0xFFFF) {
		log_warnx("%s: EOI without any active ISR?", __func__);
		return;
	}

	i82489dx_clear_isr(vector);
	i82093aa_eoi(vector);
}

int
i82489dx_ack(int vcpu_id)
{
	int vector;

	log_warnx("%s: irq ack but lapic disabled", __func__);

	vector = i82489dx_get_highest_irr();
	if (vector == 0xFFFF) {
		log_warnx("%s: ack called but no IRR bits set?", __func__);
		return vector;
	}

	i82489dx_set_isr(vector);
	i82489dx_clear_irr(vector);

	return vector;
}
