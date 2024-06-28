/* $OpenBSD */

/*
 * Copyright (c) 2023 Mike Larkin <mlarkin@openbsd.org>
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

#include <sys/queue.h>
#include <sys/types.h>

#ifndef _MMIODEV_H_
#define _MMIODEV_H_

#define MMIO_DEV_MAX_RANGES	16

#define MMIO_R	0
#define MMIO_W	1

typedef int (*mmio_fn_t)(uint64_t, uint64_t *, size_t, int);
typedef int (*mmio_init_fn_t)(void);

struct mmio_dev {
	size_t		mm_count;
	uint64_t	mm_start[MMIO_DEV_MAX_RANGES];
	uint64_t	mm_end[MMIO_DEV_MAX_RANGES];
	mmio_fn_t	mm_func[MMIO_DEV_MAX_RANGES];
	mmio_init_fn_t	mm_init;

	SLIST_ENTRY(mmio_dev) dev_next;
};

int mmio_io(uint64_t, uint64_t *, size_t, int);
void mmio_init(struct vmd_vm *);

#endif /* _MMIODEV_H_ */
