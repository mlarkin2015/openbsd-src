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

#ifndef _I82489DX_H_
#define _I82489DX_H_

#include <sys/types.h>

struct i82489dx_stats {
	uint64_t mmio_reads;
	uint64_t mmio_writes;
	uint64_t tpr_writes;
	uint64_t eois;
	uint64_t icr_writes;
	uint64_t ipi_targets;
	uint64_t timer_irqs;
	uint64_t vectors;
	uint64_t acks;
};

/* AMD AVIC incomplete-IPI failure causes passed through by vmm(4). */
enum i82489dx_avic_ipi_failure {
	I82489DX_AVIC_IPI_INVALID_TYPE = 0,
	I82489DX_AVIC_IPI_TARGET_NOT_RUNNING,
	I82489DX_AVIC_IPI_INVALID_TARGET,
	I82489DX_AVIC_IPI_INVALID_BACKING,
	I82489DX_AVIC_IPI_INVALID_VECTOR
};

void i82489dx_init(uint32_t);
void i82489dx_reset(uint32_t);
int i82489dx_mmio(uint32_t, int, paddr_t, uint8_t, uint64_t *);
int i82489dx_x2apic(uint32_t, int, uint32_t, uint64_t *);
int i82489dx_vector_irq(uint32_t, int, uint8_t, int);
int i82489dx_avic_write(uint32_t, uint16_t, uint32_t, uint32_t);
void i82489dx_avic_ipi(uint32_t, uint32_t, uint32_t, uint8_t, uint8_t,
    int);
int i82489dx_avic_activate(uint32_t, uint8_t, uint8_t, uint32_t *);
int i82489dx_avic_deactivate(uint32_t, uint8_t, uint32_t *);
int i82489dx_hw_accel(int);
uint8_t i82489dx_get_cr8(uint32_t);
void i82489dx_set_cr8(uint32_t, uint8_t);
int i82489dx_is_pending(int);
int i82489dx_ack(int);
int i82489dx_enabled(int);
int i82489dx_extint_enabled(int);
uint64_t i82489dx_targets(uint8_t, int);
int i82489dx_lowest_priority(uint64_t, uint32_t);
int i82489dx_timer_check(uint32_t);
void i82489dx_stats_snapshot(struct i82489dx_stats *);

#endif /* !_I82489DX_H_ */
