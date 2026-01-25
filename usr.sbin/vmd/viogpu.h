/*	$OpenBSD$ */
/*
 * Copyright (c) 2026
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

#ifndef VIOGPU_H
#define VIOGPU_H

#include <dev/pv/viogpu.h>
#include <stdint.h>

#include "vnc.h"

struct viogpu_resource {
	uint32_t id;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint64_t size;
	uint8_t *buf;

	uint32_t nr_entries;
	struct virtio_gpu_mem_entry *entries;
	uint64_t backing_size;
};

struct viogpu_dev {
	struct virtio_gpu_config cfg;
	uint32_t width;
	uint32_t height;
	uint32_t scanout_resource_id;

	uint8_t *fb;
	uint64_t fb_size;

	struct viogpu_resource *resources;
	size_t nresources;

	struct vnc_server vnc;
};

struct virtio_dev;
struct vmd_vm;

int viogpu_init(struct virtio_dev *, struct vmd_vm *);
void viogpu_reset(struct virtio_dev *);
void viogpu_shutdown(struct virtio_dev *);
int viogpu_io(int, uint16_t, uint32_t *, uint8_t *, void *, uint8_t);
int viogpu_notifyq(struct virtio_dev *, uint16_t);

#endif /* VIOGPU_H */
