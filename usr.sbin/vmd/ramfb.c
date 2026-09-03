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

#include <sys/types.h>

#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "fw_cfg.h"
#include "ramfb.h"

struct ramfb_wire_config {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} __packed;

_Static_assert(sizeof(struct ramfb_wire_config) == 28,
    "invalid QEMU ramfb configuration size");

static pthread_mutex_t ramfb_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct ramfb_config ramfb_cfg;
static int ramfb_configured;

int
ramfb_parse_config(const void *data, size_t len, struct ramfb_config *cfg)
{
	const struct ramfb_wire_config *wire = data;
	uint64_t size;

	if (len != sizeof(*wire) || cfg == NULL) {
		errno = EINVAL;
		return (-1);
	}

	cfg->address = be64toh(wire->address);
	cfg->fourcc = be32toh(wire->fourcc);
	cfg->flags = be32toh(wire->flags);
	cfg->width = be32toh(wire->width);
	cfg->height = be32toh(wire->height);
	cfg->stride = be32toh(wire->stride);

	if (cfg->fourcc != RAMFB_FOURCC_XRGB8888 || cfg->flags != 0 ||
	    cfg->width == 0 || cfg->width > RAMFB_MAX_WIDTH ||
	    cfg->height == 0 || cfg->height > RAMFB_MAX_HEIGHT ||
	    cfg->stride < cfg->width * RAMFB_BYTES_PER_PIXEL ||
	    cfg->stride > RAMFB_MAX_WIDTH * RAMFB_BYTES_PER_PIXEL) {
		errno = EINVAL;
		return (-1);
	}

	size = (uint64_t)cfg->stride * cfg->height;
	if (cfg->address + size < cfg->address) {
		errno = EOVERFLOW;
		return (-1);
	}

	return (0);
}

static int
ramfb_config_write(const void *data, size_t len, void *arg)
{
	struct ramfb_config cfg;

	if (ramfb_parse_config(data, len, &cfg) == -1)
		return (-1);

	pthread_mutex_lock(&ramfb_mtx);
	ramfb_cfg = cfg;
	ramfb_configured = 1;
	pthread_mutex_unlock(&ramfb_mtx);
	return (0);
}

void
ramfb_init(void)
{
	struct ramfb_wire_config wire;

	memset(&wire, 0, sizeof(wire));
	pthread_mutex_lock(&ramfb_mtx);
	memset(&ramfb_cfg, 0, sizeof(ramfb_cfg));
	ramfb_configured = 0;
	pthread_mutex_unlock(&ramfb_mtx);
	fw_cfg_add_file_callback("etc/ramfb", &wire, sizeof(wire),
	    ramfb_config_write, NULL);
}

int
ramfb_get_config(struct ramfb_config *cfg)
{
	int configured;

	pthread_mutex_lock(&ramfb_mtx);
	configured = ramfb_configured;
	if (configured)
		*cfg = ramfb_cfg;
	pthread_mutex_unlock(&ramfb_mtx);
	return (configured);
}
