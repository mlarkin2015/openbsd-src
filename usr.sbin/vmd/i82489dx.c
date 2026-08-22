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
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT,
 * OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <machine/i82489reg.h>

#include "i82489dx.h"
#include "i82093aa.h"
#include "mmio.h"
#include "vmd.h"

#define LAPIC_MAX_VCPUS		VMM_MAX_VCPUS_PER_VM

/* LVT entry indices */
#define LVT_TIMER	0
#define LVT_PCINT	1
#define LVT_LINT0	2
#define LVT_LINT1	3
#define LVT_ERROR	4
#define LVT_COUNT	5

struct i82489dx {
	uint64_t	base;
	uint32_t	ver;
	uint32_t	tpr;
	uint32_t	svr;
	uint32_t	id;
	uint32_t	ldr;
	uint32_t	dfr;
	uint32_t	esr;
	uint32_t	lvt[LVT_COUNT];

	uint32_t	isr[8];
	uint32_t	irr[8];

	/* timer state */
	uint32_t	icr_timer;	/* initial count */
	uint64_t	ccr_timer;	/* current count (computed) */
	uint32_t	dcr_timer;	/* divisor config */
	struct timespec	timer_start;	/* host time at ICR write */
	int		timer_running;
	int		timer_periodic;

	uint32_t	curvec;
};

/*
 * One LAPIC per vcpu. The mmio handler receives the accessing vcpu id and
 * indexes into this table. Interrupt delivery functions take a vcpu id for
 * the target lapic.
 */
static struct i82489dx	lapics[LAPIC_MAX_VCPUS];
static int		lapic_ncpus = 0;

static uint32_t	i82489dx_divisor(uint32_t);
static uint64_t	i82489dx_timer_ccr(struct i82489dx *);
static void	i82489dx_timer_reload(struct i82489dx *);

uint32_t
i82489dx_divisor(uint32_t dcr)
{
	switch (dcr & 0xb) {
	case LAPIC_DCRT_DIV1: return 1;
	case LAPIC_DCRT_DIV2: return 2;
	case LAPIC_DCRT_DIV4: return 4;
	case LAPIC_DCRT_DIV8: return 8;
	case LAPIC_DCRT_DIV16: return 16;
	case LAPIC_DCRT_DIV32: return 32;
	case LAPIC_DCRT_DIV64: return 64;
	case LAPIC_DCRT_DIV128: return 128;
	default: return 1;
	}
}

/*
 * Compute the current down-count from elapsed host time.
 * The LAPIC timer runs at a nominal 100 MHz bus clock divided by DCR.
 */
uint64_t
i82489dx_timer_ccr(struct i82489dx *lapic)
{
	struct timespec now, delta;
	uint64_t ns, ticks;

	if (!lapic->timer_running)
		return lapic->ccr_timer;

	clock_gettime(CLOCK_MONOTONIC, &now);
	delta.tv_sec = now.tv_sec - lapic->timer_start.tv_sec;
	delta.tv_nsec = now.tv_nsec - lapic->timer_start.tv_nsec;
	if (delta.tv_nsec < 0) {
		delta.tv_sec -= 1;
		delta.tv_nsec += 1000000000L;
	}
	ns = (uint64_t)delta.tv_sec * 1000000000ULL + (uint64_t)delta.tv_nsec;

	ticks = ns / (1000 / i82489dx_divisor(lapic->dcr_timer));
	if (ticks >= (uint64_t)lapic->icr_timer) {
		/* expired */
		lapic->timer_running = 0;
		return 0;
	}

	return (uint64_t)lapic->icr_timer - ticks;
}

static void
i82489dx_timer_reload(struct i82489dx *lapic)
{
	clock_gettime(CLOCK_MONOTONIC, &lapic->timer_start);
	lapic->timer_running = (lapic->icr_timer != 0);
}

void
i82489dx_init(uint32_t curcpu)
{
	struct i82489dx *lapic = &lapics[curcpu];

	memset(lapic, 0, sizeof(*lapic));
	lapic->ver = (1ULL << 31) | (6ULL << LAPIC_VERSION_LVT_SHIFT) | 0x10;
	lapic->base = LAPIC_BASE;
	lapic->id = (curcpu << LAPIC_ID_SHIFT);

	if ((int)curcpu >= lapic_ncpus)
		lapic_ncpus = curcpu + 1;

	/*
	 * Only register the MMIO range once - it dispatches per-vcpu via
	 * the vcpu_id argument.
	 */
	if (curcpu == 0)
		mmio_dev_add(LAPIC_BASE, LAPIC_BASE + 0xFFF,
		    (mmio_dev_fn_t)i82489dx_mmio);
}

int
i82489dx_enabled(int vcpu_id)
{
	if (vcpu_id < 0 || vcpu_id >= lapic_ncpus)
		return 0;
	return (lapics[vcpu_id].svr & LAPIC_SVR_ENABLE) != 0;
}

int
i82489dx_mmio(uint32_t vcpu_id, int dir, paddr_t addr, uint64_t *data)
{
	struct i82489dx *lapic;
	uint16_t reg;
	uint32_t d;

	if (vcpu_id >= LAPIC_MAX_VCPUS) {
		log_warnx("%s: invalid vcpu id %u", __func__, vcpu_id);
		return 1;
	}
	lapic = &lapics[vcpu_id];

	reg = addr - lapic->base;
	if (reg > 0xFFF) {
		log_warnx("%s: invalid i82489 register 0x%x", __func__, reg);
		return 1;
	}

	switch (reg) {
	case LAPIC_ID:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->id;
		break;
	case LAPIC_VERS:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->ver;
		break;
	case LAPIC_TPRI:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    (lapic->tpr & LAPIC_TPRI_MASK);
		else
			lapic->tpr = (uint32_t)*data & LAPIC_TPRI_MASK;
		break;
	case LAPIC_APRI:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    (i82489dx_get_highest_isr(vcpu_id) == 0xFFFF ? 0 :
			    (i82489dx_get_highest_isr(vcpu_id) >> 4));
		break;
	case LAPIC_PPRI:
		if (dir == MMIO_DIR_READ) {
			/*
			 * PPR = class(TPR) | max(subclass(TPR), highest ISR
			 * subclass); simplified to TPR when no ISR active,
			 * which is correct enough for guests that only gate
			 * by class.
			 */
			int isr = i82489dx_get_highest_isr(vcpu_id);
			uint32_t ppr = lapic->tpr & LAPIC_TPRI_MASK;
			if (isr != 0xFFFF && (uint32_t)(isr >> 4) > (ppr >> 4))
				ppr = (isr & 0xF0) | (lapic->tpr & 0x0F);
			*data = (*data & 0xFFFFFFFF00000000ULL) | ppr;
		}
		break;
	case LAPIC_EOI:
		if (dir == MMIO_DIR_WRITE)
			i82489dx_eoi(vcpu_id);
		break;
	case LAPIC_RRR:
		if (dir == MMIO_DIR_READ)
			*data &= 0xFFFFFFFF00000000ULL;
		break;
	case LAPIC_LDR:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->ldr;
		else
			lapic->ldr = (uint32_t)*data;
		break;
	case LAPIC_DFR:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->dfr;
		else
			lapic->dfr = (uint32_t)*data & 0xF0000000;
		break;
	case LAPIC_SVR:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->svr;
		else {
			d = (uint32_t)*data;
			lapic->svr = d & (LAPIC_SVR_VECTOR_MASK |
			    LAPIC_SVR_ENABLE | LAPIC_SVR_FOCUS);
			log_debug("%s: vcpu %u svr=0x%x", __func__, vcpu_id,
			    lapic->svr);
		}
		break;
	case LAPIC_ESR:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) | lapic->esr;
		else
			lapic->esr &= ~(uint32_t)*data;	/* write-1-to-clear */
		break;
	case LAPIC_ICRLO:
		if (dir == MMIO_DIR_WRITE) {
			d = (uint32_t)*data;
			log_debug("%s: vcpu %u ICRLO write 0x%x "
			    "(IPIs not yet implemented)", __func__, vcpu_id,
			    d);
			/* IPI handling lands in a subsequent change */
		}
		break;
	case LAPIC_ICRHI:
		if (dir == MMIO_DIR_WRITE)
			log_debug("%s: vcpu %u ICRHI write", __func__,
			    vcpu_id);
		break;
	case LAPIC_ICR_TIMER:
		if (dir == MMIO_DIR_WRITE) {
			lapic->icr_timer = (uint32_t)*data;
			lapic->timer_periodic =
			    (lapic->lvt[LVT_TIMER] & LAPIC_LVTT_TM) ==
			    LAPIC_LVTT_TM_PERIODIC;
			i82489dx_timer_reload(lapic);
			log_debug("%s: vcpu %u lapic timer icr=%u div=%u",
			    __func__, vcpu_id, lapic->icr_timer,
			    i82489dx_divisor(lapic->dcr_timer));
		}
		break;
	case LAPIC_CCR_TIMER:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    (uint32_t)i82489dx_timer_ccr(lapic);
		break;
	case LAPIC_DCR_TIMER:
		if (dir == MMIO_DIR_WRITE)
			lapic->dcr_timer = (uint32_t)*data & 0x0F;
		break;
	case LAPIC_LVTT:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    lapic->lvt[LVT_TIMER];
		else {
			d = (uint32_t)*data;
			/* TSC deadline mode unsupported: report as masked */
			if ((d & LAPIC_LVTT_TM) == LAPIC_LVTT_TM_TSCDL) {
				log_warnx("%s: vcpu %u tsc-deadline timer "
				    "requested but unsupported", __func__,
				    vcpu_id);
			}
			lapic->lvt[LVT_TIMER] = d;
			lapic->timer_periodic =
			    (d & LAPIC_LVTT_TM) == LAPIC_LVTT_TM_PERIODIC;
		}
		break;
	case LAPIC_PCINT:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    lapic->lvt[LVT_PCINT];
		else
			lapic->lvt[LVT_PCINT] = (uint32_t)*data |
			    LAPIC_LVT_MASKED;	/* always masked */
		break;
	case LAPIC_LVINT0:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    lapic->lvt[LVT_LINT0];
		else
			lapic->lvt[LVT_LINT0] = (uint32_t)*data;
		break;
	case LAPIC_LVINT1:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    lapic->lvt[LVT_LINT1];
		else
			lapic->lvt[LVT_LINT1] = (uint32_t)*data;
		break;
	case LAPIC_LVERR:
		if (dir == MMIO_DIR_READ)
			*data = (*data & 0xFFFFFFFF00000000ULL) |
			    lapic->lvt[LVT_ERROR];
		else
			lapic->lvt[LVT_ERROR] = (uint32_t)*data;
		break;
	default:
		if (dir == MMIO_DIR_READ) {
			log_debug("%s: unhandled read of i82489 reg 0x%x",
			    __func__, reg);
			*data = 0xFFFFFFFFFFFFFFFF;
		} else {
			log_debug("%s: discarding write to reg 0x%x",
			    __func__, reg);
		}
	}

	return 0;
}

/*
 * Check the vcpu's LAPIC timer for expiry. On expiry, deliver the timer
 * LVT vector (if unmasked) into the IRR and reload in periodic mode.
 *
 * Returns 1 if an interrupt became pending, 0 otherwise.
 */
int
i82489dx_timer_check(uint32_t vcpu_id)
{
	struct i82489dx *lapic;
	uint8_t vector;

	if (vcpu_id >= LAPIC_MAX_VCPUS || vcpu_id >= (uint32_t)lapic_ncpus)
		return 0;

	lapic = &lapics[vcpu_id];
	if (!lapic->timer_running || !i82489dx_enabled(vcpu_id))
		return 0;

	if (i82489dx_timer_ccr(lapic) != 0)
		return 0;

	/* expired */
	vector = lapic->lvt[LVT_TIMER] & LAPIC_LVTT_VEC_MASK;

	if (lapic->timer_periodic) {
		i82489dx_timer_reload(lapic);
	} else {
		lapic->timer_running = 0;
		lapic->ccr_timer = 0;
	}

	if (lapic->lvt[LVT_TIMER] & LAPIC_LVT_MASKED)
		return 0;

	if (vector < 32) {
		log_debug("%s: vcpu %u timer vector %u too low, dropped",
		    __func__, vcpu_id, vector);
		return 0;
	}

	log_debug("%s: vcpu %u lapic timer expired, vector %u", __func__,
	    vcpu_id, vector);
	i82489dx_set_irr(vcpu_id, vector);
	return 1;
}

void
i82489dx_vector_irq(uint32_t dest_vcpu, int destmode, uint8_t vector,
    int level)
{
	if (dest_vcpu >= LAPIC_MAX_VCPUS) {
		log_warnx("%s: invalid destination vcpu %u", __func__,
		    dest_vcpu);
		return;
	}

	if (!i82489dx_enabled(dest_vcpu)) {
		log_debug("%s: vector irq %d but vcpu %u lapic disabled",
		    __func__, vector, dest_vcpu);
		return;
	}

	/* XXX : early IRQs causing vector 0 and junk ... */
	if (vector < 32) {
		log_debug("%s: skipping low vector %d", __func__, vector);
		return;
	}

	log_debug("%s: delivering vec=%d level=%d to vcpu %u (%s dest)",
	    __func__, vector, level, dest_vcpu,
	    destmode ? "logical" : "physical");
	i82489dx_set_irr(dest_vcpu, vector);
}

/*
 * Return highest priority pending vector within a class-scan order.
 * Within a priority class, higher vectors are delivered first, so scan
 * bits from MSB downward. Returns 0xFFFF if nothing pending in the map.
 */
static int
i82489dx_highest_in_map(const uint32_t *map)
{
	int base, bit;

	for (base = 7; base >= 0; base--) {
		for (bit = 31; bit >= 0; bit--) {
			if (map[base] & (1U << bit))
				return base * 32 + bit;
		}
	}

	return 0xFFFF;
}

int
i82489dx_get_highest_irr(int vcpu_id)
{
	if (vcpu_id < 0 || vcpu_id >= lapic_ncpus)
		return 0xFFFF;
	return i82489dx_highest_in_map(lapics[vcpu_id].irr);
}

int
i82489dx_get_highest_isr(int vcpu_id)
{
	if (vcpu_id < 0 || vcpu_id >= lapic_ncpus)
		return 0xFFFF;
	return i82489dx_highest_in_map(lapics[vcpu_id].isr);
}

int
i82489dx_is_pending(int vcpu_id)
{
	if (!i82489dx_enabled(vcpu_id))
		return 0;

	return (i82489dx_get_highest_irr(vcpu_id) != 0xFFFF);
}

void
i82489dx_clear_isr(int vcpu_id, int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	lapics[vcpu_id].isr[base] &= ~(1U << ofs);
}

void
i82489dx_set_isr(int vcpu_id, int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	lapics[vcpu_id].isr[base] |= (1U << ofs);
}

void
i82489dx_set_irr(int vcpu_id, int vector)
{
	int base, ofs;

	if (vcpu_id < 0 || vcpu_id >= lapic_ncpus)
		return;

	base = vector / 32;
	ofs = vector % 32;

	lapics[vcpu_id].irr[base] |= (1U << ofs);
}

void
i82489dx_clear_irr(int vcpu_id, int vector)
{
	int base, ofs;

	base = vector / 32;
	ofs = vector % 32;

	lapics[vcpu_id].irr[base] &= ~(1U << ofs);
}

void
i82489dx_eoi(int vcpu_id)
{
	int vector;

	if (!i82489dx_enabled(vcpu_id)) {
		log_debug("%s: eoi but lapic disabled", __func__);
		return;
	}

	vector = i82489dx_get_highest_isr(vcpu_id);
	if (vector == 0xFFFF) {
		log_debug("%s: EOI without any active ISR?", __func__);
		return;
	}

	i82489dx_clear_isr(vcpu_id, vector);
	i82093aa_eoi(vector);
}

int
i82489dx_ack(int vcpu_id)
{
	int vector;

	if (!i82489dx_enabled(vcpu_id)) {
		log_debug("%s: irq ack but lapic disabled", __func__);
		return 0xFFFF;
	}

	vector = i82489dx_get_highest_irr(vcpu_id);
	if (vector == 0xFFFF)
		return vector;

	/*
	 * Task priority gating: vectors of equal or lower priority class
	 * than TPR stay pending in the IRR until the guest lowers TPR.
	 */
	if ((vector & LAPIC_TPRI_INT_MASK) <=
	    (lapics[vcpu_id].tpr & LAPIC_TPRI_INT_MASK)) {
		log_debug("%s: vector %d gated by tpr 0x%x", __func__, vector,
		    lapics[vcpu_id].tpr);
		return 0xFFFF;
	}

	i82489dx_set_isr(vcpu_id, vector);
	i82489dx_clear_irr(vcpu_id, vector);

	return vector;
}
