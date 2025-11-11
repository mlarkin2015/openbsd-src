/*	$OpenBSD */

/*
 * Copyright (c) 2024 Mike Larkin <mlarkin@openbsd.org>
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

#include <pthread.h>
#include <machine/i82093reg.h>

#include "i82093aa.h"
#include "i82489dx.h"
#include "mmio.h"
#include "vmd.h"
#include "x86_mmio.h"

struct i82093aa {
	uint32_t	reg;
	uint32_t	win;

	uint32_t	id;
	uint32_t	redtbl[I82093AA_NUM_PINS * 2];
    	int		pin_level[I82093AA_NUM_PINS];
    	int		last_level[I82093AA_NUM_PINS];
	pthread_mutex_t mtx;
};

struct i82093aa		ioapic;

void
i82093aa_decode_redent(uint32_t reg)
{
	uint8_t pin, destmode, masked, triggermode, rirr, polarity, delivs,
	    delmod, vector;
	uint32_t lo, hi;

	if (reg < I82093AA_REDTBL0_LO || reg > I82093AA_REDTBL23_HI)
		return;

	pin = (I82093AA_REDTBL23_HI - reg) / 2;
	lo = ioapic.redtbl[pin];
	hi = ioapic.redtbl[pin + 1];
	destmode = lo & I82093AA_REDLO_DESTMODE;
	masked = lo & I82093AA_REDLO_MASKED;
	triggermode = lo & I82093AA_REDLO_TYPE;
	rirr = lo & I82093AA_REDLO_RIRR;
	polarity = lo & I82093AA_REDLO_POLARITY;
	delivs = lo & I82093AA_REDLO_DELIVS;
	delmod = (lo & I82093AA_REDLO_DELMODE_MASK) >> 8;
	vector = lo & 0xFF;

	log_warnx("%s: write to redirection entry %d (%s byte)",
	    __func__,
	    pin,
	    reg & 1 ? "high" : "low");
	log_warnx("%s:  -> destination mode: %s",
	    __func__,
	    destmode ? "logical" : "physical");
	if (destmode) {
		log_warnx("%s:  -> destination: logical 0x%x",
		    __func__,
		    (hi & 0xFF) >> 24);
	} else {
		log_warnx("%s:  -> apic id 0x%x",
		    __func__,
		    (hi & 0x0F) >> 24);
	}

	log_warnx("%s:  -> masked: %s",
	    __func__,
	    masked ? "true" : "false");

	log_warnx("%s:  -> trigger mode: %s",
	    __func__,
	    triggermode ? "level" : "edge");

	log_warnx("%s:  -> rirr: %s",
	    __func__,
	    rirr ? "asserted" : "deasserted");

	log_warnx("%s:  -> polarity: %s",
	    __func__,
	    polarity ? "low" : "high");

	log_warnx("%s:  -> delivery status: %s",
	    __func__,
	    delivs ? "idle" : "send pending");

	switch (delmod) {
	case 0x0: log_warnx("%s:  -> delivery mode: fixed", __func__); break;
	case 0x1: log_warnx("%s:  -> delivery mode: lo pri", __func__); break;
	case 0x2: log_warnx("%s:  -> delivery mode: smi", __func__); break;
	case 0x3: log_warnx("%s:  -> delivery mode: reserved", __func__); break;
	case 0x4: log_warnx("%s:  -> delivery mode: nmi", __func__); break;
	case 0x5: log_warnx("%s:  -> delivery mode: init", __func__); break;
	case 0x6: log_warnx("%s:  -> delivery mode: reserved", __func__); break;
	case 0x7: log_warnx("%s:  -> delivery mode: extint", __func__); break;
	}

	log_warnx("%s:  -> vector: 0x%x (%d)",
	    __func__,
	    vector, vector);
}

void
i82093aa_winop(int dir, uint64_t *data)
{
	uint32_t d;

	pthread_mutex_lock(&ioapic.mtx);

	if (dir == MMIO_DIR_READ) {
		log_warnx("%s: requested read from register 0x%x", __func__,
		    ioapic.reg);

		d = 0;

		switch (ioapic.reg) {
		case IOAPIC_ID:
			d = ioapic.id;
			break;
		case IOAPIC_VER:
			d = I82093AA_VERSION |
			    (I82093AA_NUM_PINS << IOAPIC_MAX_SHIFT);
			break;
		case I82093AA_REDTBL0_LO ... I82093AA_REDTBL23_HI:
			d = ioapic.redtbl[ioapic.reg];
			break;
		default:
			log_warnx("%s: unknown register id 0x%x", __func__,
			    ioapic.reg);
		}

		*data &= 0xFFFFFFFF00000000ULL;
		*data |= d;
	} else if (dir == MMIO_DIR_WRITE) {
		log_warnx("%s: requested write to register 0x%x, data=0x%x",
		    __func__, ioapic.reg, (uint32_t)*data);
		switch (ioapic.reg) {
		case I82093AA_REDTBL0_LO ... I82093AA_REDTBL23_HI:
			ioapic.redtbl[ioapic.reg] = (uint32_t)*data;
			i82093aa_decode_redent(ioapic.reg);
			i82093aa_evaluate_pin(ioapic.reg);
			break;
		default:
			log_warnx("%s: unknown register id 0x%x", __func__,
			    ioapic.reg);
		}
	} else {
		log_warnx("%s: impossible direction %d", __func__, dir);
	}

	pthread_mutex_unlock(&ioapic.mtx);
}

void
i82093aa_regsel(int dir, uint64_t *data)
{
	uint64_t d;

	pthread_mutex_lock(&ioapic.mtx);

	log_warnx("%s: mmio op to register select (dir=%d)", __func__, dir);
	if (dir == MMIO_DIR_READ) {
		d = *data & 0xFFFFFFFF00000000ULL;
		log_warnx("%s: returning 0x%x", __func__, ioapic.reg);
		d |= ioapic.reg;
		*data = d;
	} else if (dir == MMIO_DIR_WRITE) {
		log_warnx("%s: setting regwin = 0x%x", __func__,
		    (uint32_t)*data);
		ioapic.reg = (uint32_t)*data;
	} else {
		log_warnx("%s: impossible direction %d", __func__, dir);
	}

	pthread_mutex_unlock(&ioapic.mtx);
}

int
i82093aa_mmio(int dir, paddr_t addr, uint64_t *data)
{
	log_warnx("%s: dir=%d addr=0x%lx data=0x%llx", __func__, dir, addr,
	    *data);

	if (addr == (IOAPIC_BASE_DEFAULT + IOAPIC_REG)) {
		i82093aa_regsel(dir, data);
	} else if (addr == (IOAPIC_BASE_DEFAULT + IOAPIC_DATA)) {
		i82093aa_winop(dir, data);
	} else {
		log_warnx("%s: invalid i82093aa register @ 0x%lx", __func__,
		    addr);
	}

	return 0;
}

void
i82093aa_init(void)
{
	ioapic.mtx = PTHREAD_MUTEX_INITIALIZER;
	mmio_dev_add(IOAPIC_BASE_DEFAULT, IOAPIC_BASE_DEFAULT + 0xFFFF,
	    (mmio_dev_fn_t)i82093aa_mmio);
}

void
i82093aa_assert_pin(uint8_t pin)
{
    pthread_mutex_lock(&ioapic.mtx);
    ioapic.pin_level[pin] = 1;
    i82093aa_evaluate_pin(pin);
    pthread_mutex_unlock(&ioapic.mtx);
}

void
i82093aa_deassert_pin(uint8_t pin)
{
    pthread_mutex_lock(&ioapic.mtx);
    ioapic.pin_level[pin] = 0;
    i82093aa_evaluate_pin(pin);
    pthread_mutex_unlock(&ioapic.mtx);
}

void
i82093aa_evaluate_pin(uint8_t pin)
{
	uint64_t ent;
	uint8_t delivery_mode, vector, dest;
	int masked, level, polarity, active, rising;

	ent = ioapic.redtbl[pin] | ((uint64_t)(ioapic.redtbl[pin + 1]) << 32);
	masked = ent & I82093AA_REDLO_MASKED;
	level = ent & I82093AA_REDLO_TYPE;
	polarity = ent & I82093AA_REDLO_POLARITY;
	active = ioapic.pin_level[pin] ^ polarity;
	rising = active && !ioapic.last_level[pin];

	delivery_mode = (ent & I82093AA_REDLO_DEL) >> 8;
	vector = ent & 0xFF;
   	dest = (ent >> I82093AA_REDIR_SHIFT) & 0xFF;

	ioapic.last_level[pin] = active;

	if (masked)
		return;

	if (level) {
		if (active && !(ent & I82093AA_REDLO_RIRR)) {
			i82093aa_deliver(dest, delivery_mode, vector, 1);
			ioapic.redtbl[pin] |= I82093AA_REDLO_RIRR;
		}
	} else {
		if (rising) {
			i82093aa_deliver(dest, delivery_mode, vector, 0);
        }
    }
}

void
i82093aa_deliver(uint8_t dest, int destmode, uint8_t vector, int level)
{
	int vcpu_id = dest;

	i82489dx_vector_irq(vcpu_id, destmode, vector, level);
}

void
i82093aa_eoi(uint8_t vector)
{
	uint8_t pin;
	uint64_t ent;

	pthread_mutex_lock(&ioapic.mtx);

	for (pin = 0; pin < I82093AA_NUM_PINS; pin++) {
		ent = ioapic.redtbl[pin] |
		    ((uint64_t)(ioapic.redtbl[pin + 1]) << 32);
		if ((ent & 0xFF) != vector)
			continue;

		if ((ent & I82093AA_REDLO_TYPE) && (ent & IOAPIC_REDLO_RIRR)) {
			ioapic.redtbl[pin] &= ~IOAPIC_REDLO_RIRR;
			i82093aa_evaluate_pin(pin);
		}
	}

	pthread_mutex_unlock(&ioapic.mtx);
}
