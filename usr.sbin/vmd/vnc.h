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

#ifndef VNC_H
#define VNC_H

#include <sys/types.h>
#include <sys/socket.h>

#include <event.h>
#include <stdint.h>

struct vnc_rect {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
};

struct vnc_server {
	int listen_fd;
	int client_fd;
	unsigned int port;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	const uint8_t *fb;

	uint8_t msg_type;
	uint8_t buf[4096];
	size_t have;
	size_t want;
	int state;

	struct vnc_rect dirty;
	int dirty_valid;
	int ready;
	int update_pending;

	struct event ev_listen;
	struct event ev_client;
};

int vnc_server_init(struct vnc_server *, uint32_t, uint32_t, const uint8_t *,
    uint32_t);
void vnc_server_shutdown(struct vnc_server *);
void vnc_server_mark_dirty(struct vnc_server *, const struct vnc_rect *);

#endif /* VNC_H */
