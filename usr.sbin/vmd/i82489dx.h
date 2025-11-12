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

#ifndef _I82489DX_H_
#define _I82489DX_H_

#include <sys/types.h>

void i82489dx_init(void);
int i82489dx_mmio(int, paddr_t, uint64_t *);
void i82489dx_vector_irq(int, int, uint8_t, int);
int i82489dx_is_pending(int);
void i82489dx_eoi(void);
int i82489dx_ack(int);
int i82489dx_get_highest_irr(void);
int i82489dx_get_highest_isr(void);
void i82489dx_clear_isr(int);

#endif /* !_I82489DX_H_ */
