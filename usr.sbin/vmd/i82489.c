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

#include <sys/types.h>

#include "vmd.h"
#include "i82489.h"
#include "mmiodev.h"
#include "proc.h"

int
i82489_init(void)
{
	log_warnx("%s: i82489 init", __func__);

	return 0;
}

int
i82489_mmio(uint64_t addr, uint64_t *data, size_t len, int dir)
{
	if (dir == MMIO_W)
		log_warnx("%s: i82489 mmio write, addr=0x%llx, data=0x%llx, "
		    "len=%zd", __func__, addr, *data, len);
	else if (dir == MMIO_R)
		log_warnx("%s: i82489 mmio read, addr=0x%llx, "
		    "len=%zd", __func__, addr, len);
	else
		fatalx("%s: unsupported mmio dir %d", __func__, dir);

	return 0;
}
