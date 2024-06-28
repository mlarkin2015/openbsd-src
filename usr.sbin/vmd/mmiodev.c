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

#include <sys/param.h> /* PAGE_SIZE */

#include <machine/i82093reg.h>
#include <machine/i82489reg.h>

#include <stdlib.h>

#include "vmd.h"
#include "mmiodev.h"
#include "i82489.h"
#include "i82093aa.h"
#include "proc.h"

SLIST_HEAD(mmio_dev_head, mmio_dev) mmio_devs;

int mmio_dev_launch(struct vmd_vm *, struct mmio_dev *);

int
mmio_dev_launch(struct vmd_vm *vm, struct mmio_dev *dev)
{
	return dev->mm_init();
}

int
mmio_io(uint64_t addr, uint64_t *data, size_t len, int dir)
{
	struct mmio_dev *dev;
	size_t i;

	log_warnx("%s: mmio request, addr=0x%llx, len=%zd, dir=%d", __func__,
	    addr, len, dir);

	SLIST_FOREACH(dev, &mmio_devs, dev_next) {
		for (i = 0; i < dev->mm_count; dev++) {
			log_warnx("%s: checking for 0x%llx in [0x%llx:0x%llx]",
			    __func__, addr, dev->mm_start[i], dev->mm_end[i]);
			if (dev->mm_start[i] <= addr &&
			    dev->mm_end[i] >= addr)
				return dev->mm_func[i](addr, data, len, dir);
		}
	}

	log_warnx("%s: no mmio device defined for addr 0x%llx", __func__,
	    addr);

	return 1;
}

void
mmio_init(struct vmd_vm *vm)
{
	struct mmio_dev *dev;

	SLIST_INIT(&mmio_devs);

	dev = calloc(1, sizeof(struct mmio_dev));
	if (!dev)
		fatalx("%s: calloc error", __func__);
	dev->mm_count = 1;
	dev->mm_start[0] = LAPIC_BASE;
	dev->mm_end[0] = LAPIC_BASE + PAGE_SIZE - 1;
	dev->mm_func[0] = i82489_mmio;
	dev->mm_init = i82489_init;
	SLIST_INSERT_HEAD(&mmio_devs, dev, dev_next);

	dev = calloc(1, sizeof(struct mmio_dev));
	if (!dev)
		fatalx("%s: calloc error", __func__);
	dev->mm_count = 1;
	dev->mm_start[0] = IOAPIC_BASE_DEFAULT;
	dev->mm_end[0] = IOAPIC_BASE_DEFAULT + PAGE_SIZE - 1;
	dev->mm_func[0] = i82093aa_mmio;
	dev->mm_init = i82093aa_init;
	SLIST_INSERT_HEAD(&mmio_devs, dev, dev_next);

	SLIST_FOREACH(dev, &mmio_devs, dev_next) {
		if (mmio_dev_launch(vm, dev))
			fatalx("failed to launch mmio device");
	}
}

