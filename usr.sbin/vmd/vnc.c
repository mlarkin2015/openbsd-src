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

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atomicio.h"
#include "vmd.h"
#include "vnc.h"

#define VNC_PORT_START	5900
#define VNC_PORT_END	5999

#define VNC_PROTOCOL	"RFB 003.008\n"

static void vnc_accept(int, short, void *);
static void vnc_client_read(int, short, void *);
static void vnc_close_client(struct vnc_server *);
static int vnc_send(const struct vnc_server *, const void *, size_t);
static void vnc_send_framebuffer(struct vnc_server *, const struct vnc_rect *);
static void vnc_queue_request(struct vnc_server *);
static void vnc_handle_message(struct vnc_server *);
static size_t vnc_needed_size(uint8_t, const uint8_t *, size_t, int *);
static void vnc_handle_handshake(struct vnc_server *);
static void vnc_send_server_init(struct vnc_server *);

int
vnc_server_init(struct vnc_server *srv, uint32_t width, uint32_t height,
    const uint8_t *fb, uint32_t stride)
{
	struct sockaddr_in sin;
	int fd;
	int on = 1;
	unsigned int port;

	memset(srv, 0, sizeof(*srv));
	srv->listen_fd = -1;
	srv->client_fd = -1;
	srv->width = width;
	srv->height = height;
	srv->stride = stride;
	srv->fb = fb;
	srv->msg_type = 0;
	srv->have = 0;
	srv->want = 0;
	srv->state = 0;
	srv->state = 0;
	srv->update_pending = 0;

	for (port = VNC_PORT_START; port <= VNC_PORT_END; port++) {
		log_warnx("%s: trying port %d", __func__, port);
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (fd == -1)
			return errno;
		log_warnx("setsockopt");
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
			close(fd);
			return errno;
		}

		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_ANY);
		sin.sin_port = htons(port);
		log_warnx("bind");
		if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
			close(fd);
			if (errno == EADDRINUSE) {
				log_warnx("%s: port %d in use", __func__, port);
				continue;
			}
			return errno;
		}
		log_warnx("listen");
		if (listen(fd, NR_BACKLOG) == -1) {
			close(fd);
			return errno;
		}
		srv->listen_fd = fd;
		srv->port = port;
		break;
	}
	if (srv->listen_fd == -1)
		return EADDRINUSE;

	log_warnx("%s: configured vnc port %d", __func__, port);
	event_set(&srv->ev_listen, srv->listen_fd, EV_READ | EV_PERSIST,
	    vnc_accept, srv);
	log_warnx("adding ev_listen");
	event_add(&srv->ev_listen, NULL);

	return 0;
}

void
vnc_server_shutdown(struct vnc_server *srv)
{
	vnc_close_client(srv);
	if (srv->listen_fd != -1) {
		event_del(&srv->ev_listen);
		close(srv->listen_fd);
		srv->listen_fd = -1;
	}
}

void
vnc_server_mark_dirty(struct vnc_server *srv, const struct vnc_rect *rect)
{
	uint16_t x1, y1, x2, y2;

	if (!srv->ready) {
		srv->update_pending = 1;
		srv->dirty = *rect;
		srv->dirty_valid = 1;
		return;
	}

	if (!srv->dirty_valid) {
		srv->dirty = *rect;
		srv->dirty_valid = 1;
	} else {
		x1 = srv->dirty.x < rect->x ? srv->dirty.x : rect->x;
		y1 = srv->dirty.y < rect->y ? srv->dirty.y : rect->y;
		x2 = (srv->dirty.x + srv->dirty.width) >
		    (rect->x + rect->width)
		    ? (srv->dirty.x + srv->dirty.width)
		    : (rect->x + rect->width);
		y2 = (srv->dirty.y + srv->dirty.height) >
		    (rect->y + rect->height)
		    ? (srv->dirty.y + srv->dirty.height)
		    : (rect->y + rect->height);
		srv->dirty.x = x1;
		srv->dirty.y = y1;
		srv->dirty.width = x2 - x1;
		srv->dirty.height = y2 - y1;
	}

	srv->update_pending = 1;
	return;
}

static void
vnc_accept(int fd, short event, void *arg)
{
	struct vnc_server *srv = arg;
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	int client;
	uint8_t sec_type = 1;
	uint32_t sec_result = htonl(0);
	uint8_t shared = 1;

	log_warnx("%s: accepted connection", __func__);

	client = accept(fd, (struct sockaddr *)&ss, &slen);
	if (client == -1) {
		log_warnx("%s: accept error %d", __func__, errno);
		return;
	}

	if (fcntl(client, F_SETFL, O_NONBLOCK) == -1) {
		log_warnx("%s: fcntl error %d", __func__, errno);
		close(client);
		return;
	}
	if (srv->client_fd != -1) {
		log_warnx("%s: client_fd != -1 error", __func__);
		close(client);
		return;
	}

	srv->client_fd = client;
	if (!vnc_send(srv, VNC_PROTOCOL, strlen(VNC_PROTOCOL))) {
		log_warnx("%s: vnc_send error", __func__);
		close(client);
		return;
	}

	srv->ready = 0;
	srv->want = 0;
	srv->have = 0;
	srv->state = 1;
	srv->msg_type = 0;
	srv->buf[0] = 0;
	srv->buf[1] = 0;
	srv->buf[2] = 0;
	srv->buf[3] = 0;
	(void)sec_type;
	(void)sec_result;
	(void)shared;
	event_set(&srv->ev_client, srv->client_fd, EV_READ | EV_PERSIST,
	    vnc_client_read, srv);
	event_add(&srv->ev_client, NULL);
}

static void
vnc_client_read(int fd, short event, void *arg)
{
	struct vnc_server *srv = arg;
	ssize_t n;
	uint8_t *dst;

	if (srv->want == 0) {
		if (srv->state != 0) {
			srv->want = 12;
			srv->have = 0;
		} else {
			vnc_queue_request(srv);
		}
	}
	if (srv->want == 0) {
		vnc_close_client(srv);
		return;
	}

	while (srv->want > srv->have) {
		dst = srv->buf + srv->have;
		n = read(fd, dst, srv->want - srv->have);
		if (n == 0) {
			vnc_close_client(srv);
			return;
		}
		if (n == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				break;
			vnc_close_client(srv);
			return;
		}
		srv->have += (size_t)n;
		if (srv->state == 0 && (srv->msg_type == 2 || srv->msg_type == 6)) {
			size_t newwant;
			int state = 0;

			newwant = vnc_needed_size(srv->msg_type, srv->buf, srv->have,
			    &state);
			if (newwant == 0) {
				vnc_close_client(srv);
				return;
			}
			if (newwant > srv->want)
				srv->want = newwant;
		}
	}

	if (srv->have == srv->want) {
		if (srv->state != 0) {
			log_warnx("%s: handling handshake", __func__);
			vnc_handle_handshake(srv);
		} else {
			log_warnx("%s: handling message", __func__);
			vnc_handle_message(srv);
			if (srv->want == 0) {
				log_warnx("%s: queuing request", __func__);
				vnc_queue_request(srv);
			} else {
				log_warnx("%s: handled message but want=%zd?", __func__, srv->want);
			}
		}
	}
}

static void
vnc_send_framebuffer(struct vnc_server *srv, const struct vnc_rect *rect)
{
	uint8_t hdr[4];
	uint8_t recthdr[12];
	uint16_t x = htons(rect->x);
	uint16_t y = htons(rect->y);
	uint16_t w = htons(rect->width);
	uint16_t h = htons(rect->height);
	uint32_t enc = htonl(0);
	uint32_t row;
	const uint8_t *src;
	uint32_t row_bytes;

	if (srv->client_fd == -1 || !srv->ready)
		return;
	if (rect->width == 0 || rect->height == 0)
		return;

	hdr[0] = 0;
	hdr[1] = 0;
	hdr[2] = 0;
	hdr[3] = 1;
	memcpy(recthdr, &x, sizeof(x));
	memcpy(recthdr + 2, &y, sizeof(y));
	memcpy(recthdr + 4, &w, sizeof(w));
	memcpy(recthdr + 6, &h, sizeof(h));
	memcpy(recthdr + 8, &enc, sizeof(enc));

	log_warnx("%s: sending hdr and recthdr", __func__);

	if (!vnc_send(srv, hdr, sizeof(hdr)) ||
	    !vnc_send(srv, recthdr, sizeof(recthdr))) {
		vnc_close_client(srv);
		return;
	}

	row_bytes = rect->width * 4;
	src = srv->fb + (uint32_t)rect->y * srv->stride + rect->x * 4;
	for (row = 0; row < rect->height; row++) {
		log_warnx("%s: sending row %d", __func__, row);
		if (!vnc_send(srv, src, row_bytes)) {
			vnc_close_client(srv);
			return;
		}
		src += srv->stride;
	}
}

static void
vnc_close_client(struct vnc_server *srv)
{
	if (srv->client_fd == -1)
		return;
	event_del(&srv->ev_client);
	close(srv->client_fd);
	srv->client_fd = -1;
	srv->ready = 0;
	srv->want = 0;
	srv->have = 0;
	srv->update_pending = 0;
}

static int
vnc_send(const struct vnc_server *srv, const void *data, size_t len)
{
	ssize_t n;
	const uint8_t *ptr = data;
	size_t left = len;

	if (srv->client_fd == -1) {
		log_warnx("%s: client_fd == -1 , exiting", __func__);
		return 0;
	}

	log_warnx("%s: writing %zd bytes", __func__, len);
	while (left > 0) {
		n = write(srv->client_fd, ptr, left);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				continue;
			return 0;
		}
		if (n == 0)
			return 0;
		ptr += (size_t)n;
		left -= (size_t)n;
	}

	log_warnx("%s: returning 1", __func__);
	return 1;
}

static void
vnc_queue_request(struct vnc_server *srv)
{
	ssize_t n;

	srv->have = 0;
	srv->want = 1;

	n = read(srv->client_fd, &srv->msg_type, 1);
	if (n == 0) {
		log_warnx("%s: short read, closing client", __func__);
		vnc_close_client(srv);
		return;
	}
	if (n == -1) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		log_warnx("%s: error in read, closing client", __func__);
		vnc_close_client(srv);
		return;
	}

	log_warnx("%s: read msg type %d", __func__, srv->msg_type);
	if (srv->state != 0) {
		log_warnx("%s: srv->state !=0, returning", __func__);
		srv->want = 12;
		srv->have = 0;
		return;
	}
	srv->state = 0;
	srv->want = vnc_needed_size(srv->msg_type, srv->buf, srv->have,
	    &srv->state);
}

static void
vnc_handle_message(struct vnc_server *srv)
{

	log_warnx("%s: handling message type %d", __func__, srv->msg_type);
	switch (srv->msg_type) {
	case 0:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
		if (srv->msg_type == 3 && srv->have >= 9) {
			struct vnc_rect rect;
			rect.x = ((uint16_t)srv->buf[1] << 8) | srv->buf[2];
			rect.y = ((uint16_t)srv->buf[3] << 8) | srv->buf[4];
			rect.width = ((uint16_t)srv->buf[5] << 8) | srv->buf[6];
			rect.height = ((uint16_t)srv->buf[7] << 8) | srv->buf[8];
			if (rect.width == 0 || rect.height == 0 ||
			    rect.x + rect.width > srv->width ||
			    rect.y + rect.height > srv->height) {
				log_warnx("%s: invalid rect request (rect.x=%d rect.y=%d, rect.width=%d, rect.height=%d, srv.width=%d, srv.height=%d",
				    __func__, rect.x, rect.y, rect.width, rect.height, srv->width, srv->height);
				vnc_close_client(srv);
				return;
			}
			log_warnx("%s: sending fb (rect.x=%d rect.y=%d, rect.width=%d, rect.height=%d, srv.width=%d, srv.height=%d",
			    __func__, rect.x, rect.y, rect.width, rect.height, srv->width, srv->height);
			vnc_send_framebuffer(srv, &rect);
		}
		srv->want = 0;
		srv->have = 0;
		return;
	default:
		log_warnx("%s: unknown srv msg type %d", __func__, srv->msg_type);
		vnc_close_client(srv);
		return;
	}
}

static size_t
vnc_needed_size(uint8_t type, const uint8_t *buf, size_t have, int *state)
{
	uint16_t count;
	uint32_t len;

	switch (type) {
	case 0:
		log_warnx("%s: type 0, returning size 19", __func__);
		return 19;
	case 2:
		if (have < 3) {
			log_warnx("%s: type 2, returning size 3", __func__);
			return 3;
		}
		count = (uint16_t)buf[1] << 8 | buf[2];
		if (count > 256) {
			*state = 1;
			log_warnx("%s: type 2, returning size 0 (state=1)", __func__);
			return 0;
		}
		log_warnx("%s: type 2, returning size %zd", __func__, 3 + (size_t)count * 4);
		return 3 + (size_t)count * 4;
	case 3:
		log_warnx("%s: type 3, returning size 9", __func__);
		return 9;
	case 4:
		log_warnx("%s: type 4, returning size 7", __func__);
		return 7;
	case 5:
		log_warnx("%s: type 5, returning size 5", __func__);
		return 5;
	case 6:
		if (have < 7) {
			log_warnx("%s: type 6, returning size 7 (have=%zd)", __func__, have);
			return 7;
		}
		len = (uint32_t)buf[3] << 24 | (uint32_t)buf[4] << 16 |
		    (uint32_t)buf[5] << 8 | (uint32_t)buf[6];
		if (len > (1024 * 1024)) {
			*state = 1;
			log_warnx("%s: type 6, returning size 0 (state=1)", __func__);
			return 0;
		}
		log_warnx("%s: type 6, returning size %d", __func__, 7 + len);
		return 7 + len;
	default:
		log_warnx("%s: type unknown (%d), returning size 0", __func__, type);
		return 0;
	}
}

static void
vnc_handle_handshake(struct vnc_server *srv)
{
	uint8_t sec_types[2] = { 1, 1 };
	uint32_t result = htonl(0);

	switch (srv->state) {
	case 1:
		if (srv->want != 12)
			return;
		if (strncmp((char *)srv->buf, VNC_PROTOCOL, 12) != 0) {
			vnc_close_client(srv);
			return;
		}
		if (!vnc_send(srv, sec_types, sizeof(sec_types))) {
			vnc_close_client(srv);
			return;
		}
		srv->state = 2;
		srv->have = 0;
		srv->want = 1;
		break;
	case 2:
		if (srv->buf[0] != 1) {
			vnc_close_client(srv);
			return;
		}
		if (!vnc_send(srv, &result, sizeof(result))) {
			vnc_close_client(srv);
			return;
		}
		srv->state = 3;
		srv->have = 0;
		srv->want = 1;
		break;
	case 3:
		log_warnx("%s: sending server init", __func__);
		vnc_send_server_init(srv);
		srv->state = 0;
		srv->have = 0;
		srv->want = 0;
		srv->ready = 1;
		if (srv->update_pending && srv->dirty_valid) {
			vnc_send_framebuffer(srv, &srv->dirty);
			srv->dirty_valid = 0;
			srv->update_pending = 0;
		}
		break;
	default:
		log_warnx("%s: unknown server state %d, closing client", __func__, srv->state);
		vnc_close_client(srv);
		return;
	}
}

static void
vnc_send_server_init(struct vnc_server *srv)
{
	uint16_t width = htons(srv->width);
	uint16_t height = htons(srv->height);
	uint32_t name_len;
	uint8_t pixfmt[16];
	const char *name = "OpenBSD vmd";

	memset(pixfmt, 0, sizeof(pixfmt));
	pixfmt[0] = 32;
	pixfmt[1] = 24;
	pixfmt[2] = 0;
	pixfmt[3] = 1;
	pixfmt[4] = 0;
	pixfmt[5] = 255;
	pixfmt[6] = 0;
	pixfmt[7] = 255;
	pixfmt[8] = 0;
	pixfmt[9] = 255;
	pixfmt[10] = 16;
	pixfmt[11] = 8;
	pixfmt[12] = 0;

	name_len = htonl((uint32_t)strlen(name));
	if (!vnc_send(srv, &width, sizeof(width)) ||
	    !vnc_send(srv, &height, sizeof(height)) ||
	    !vnc_send(srv, pixfmt, sizeof(pixfmt)) ||
	    !vnc_send(srv, &name_len, sizeof(name_len)) ||
	    !vnc_send(srv, name, strlen(name))) {
		vnc_close_client(srv);
		return;
	}
}
