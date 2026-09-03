/*	$OpenBSD$	*/

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

#ifndef _RAMFB_H_
#define _RAMFB_H_

#include <sys/types.h>

#define RAMFB_FOURCC_XRGB8888	0x34325258U
#define RAMFB_MAX_WIDTH		4096U
#define RAMFB_MAX_HEIGHT	2160U
#define RAMFB_BYTES_PER_PIXEL	4U

struct ramfb_config {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
};

void	ramfb_init(void);
void	ramfb_stop(void);
int	ramfb_parse_config(const void *, size_t, struct ramfb_config *);
int	ramfb_get_config(struct ramfb_config *);

#endif /* _RAMFB_H_ */
