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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmd.h"
#include "display.h"

size_t
display_surface_size(void)
{
	return (sizeof(struct display_surface) +
	    (size_t)DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT * DISPLAY_BPP);
}

int
display_surface_open(int *fdp)
{
	char name[] = "/vmd-display.XXXXXXXXXX";
	int fd;

	*fdp = -1;
	if ((fd = shm_mkstemp(name)) == -1)
		return (-1);
	if (shm_unlink(name) == -1 ||
	    ftruncate(fd, display_surface_size()) == -1) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return (-1);
	}
	*fdp = fd;
	return (0);
}

int
display_surface_map(int fd, int prot, struct display_surface **surfacep)
{
	void *p;

	p = mmap(NULL, display_surface_size(), prot, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return (-1);
	*surfacep = p;
	return (0);
}

void
display_surface_init(struct display_surface *surface)
{
	surface->magic = DISPLAY_SURFACE_MAGIC;
	surface->version = DISPLAY_SURFACE_VERSION;
	surface->sequence = 0;
	surface->width = DISPLAY_DEFAULT_WIDTH;
	surface->height = DISPLAY_DEFAULT_HEIGHT;
	surface->stride = DISPLAY_DEFAULT_WIDTH * DISPLAY_BPP;
	surface->format = DISPLAY_FORMAT_XRGB8888;
	surface->generation = 1;
	memset(surface->pixels, 0,
	    (size_t)surface->stride * surface->height);
}

int
display_surface_update(struct display_surface *surface, const void *pixels,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t format)
{
	const uint8_t *src = pixels;
	uint32_t rowbytes, y;

	if (surface == NULL || pixels == NULL || width == 0 ||
	    width > DISPLAY_MAX_WIDTH || height == 0 ||
	    height > DISPLAY_MAX_HEIGHT || format != DISPLAY_FORMAT_XRGB8888 ||
	    width > UINT32_MAX / DISPLAY_BPP) {
		errno = EINVAL;
		return (-1);
	}
	rowbytes = width * DISPLAY_BPP;
	if (stride < rowbytes) {
		errno = EINVAL;
		return (-1);
	}
	if (surface->width == width && surface->height == height &&
	    surface->stride == rowbytes && surface->format == format) {
		for (y = 0; y < height; y++) {
			if (memcmp(surface->pixels + (size_t)y * rowbytes,
			    src + (size_t)y * stride, rowbytes) != 0)
				break;
		}
		if (y == height)
			return (0);
	}

	(void)__atomic_add_fetch(&surface->sequence, 1, __ATOMIC_SEQ_CST);
	surface->width = width;
	surface->height = height;
	surface->stride = rowbytes;
	surface->format = format;
	for (y = 0; y < height; y++)
		memcpy(surface->pixels + (size_t)y * rowbytes,
		    src + (size_t)y * stride, rowbytes);
	surface->generation++;
	(void)__atomic_add_fetch(&surface->sequence, 1, __ATOMIC_SEQ_CST);
	return (0);
}

int
display_surface_snapshot(const struct display_surface *surface, void *pixels,
    size_t pixels_len, struct display_frame *frame)
{
	uint32_t before, after;
	size_t len;
	unsigned int tries;

	if (surface == NULL || pixels == NULL || frame == NULL) {
		errno = EINVAL;
		return (-1);
	}
	for (tries = 0; tries < 100; tries++) {
		before = __atomic_load_n(&surface->sequence, __ATOMIC_ACQUIRE);
		if (before & 1) {
			usleep(100);
			continue;
		}
		if (surface->magic != DISPLAY_SURFACE_MAGIC ||
		    surface->version != DISPLAY_SURFACE_VERSION ||
		    surface->width == 0 || surface->width > DISPLAY_MAX_WIDTH ||
		    surface->height == 0 ||
		    surface->height > DISPLAY_MAX_HEIGHT ||
		    surface->stride != surface->width * DISPLAY_BPP ||
		    surface->format != DISPLAY_FORMAT_XRGB8888) {
			errno = EINVAL;
			return (-1);
		}
		len = (size_t)surface->stride * surface->height;
		if (len > pixels_len) {
			errno = ENOSPC;
			return (-1);
		}
		frame->width = surface->width;
		frame->height = surface->height;
		frame->stride = surface->stride;
		frame->format = surface->format;
		frame->generation = surface->generation;
		memcpy(pixels, surface->pixels, len);
		after = __atomic_load_n(&surface->sequence, __ATOMIC_ACQUIRE);
		if (before == after && !(after & 1))
			return (0);
		usleep(100);
	}
	errno = EAGAIN;
	return (-1);
}

int
display_path_default(const struct vmop_create_params *vmc, char *path,
    size_t path_len)
{
	int n;

	n = snprintf(path, path_len, "%s/%s.vnc", VM_DISPLAY_DIR,
	    vmc->vmc_name);
	if (n < 0 || (size_t)n >= path_len ||
	    (size_t)n >= SUN_PATH_LEN) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	return (0);
}

int
display_path_validate(const char *path)
{
	const char *base;

	if (path == NULL || path[0] != '/') {
		errno = EINVAL;
		return (-1);
	}
	if (strnlen(path, SUN_PATH_LEN) == SUN_PATH_LEN) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	base = strrchr(path, '/');
	if (base == NULL || base[1] == '\0') {
		errno = EINVAL;
		return (-1);
	}
	return (0);
}

static int
display_parent_validate(const char *path)
{
	struct stat st;
	const char *slash;
	char parent[PATH_MAX];
	size_t len;

	slash = strrchr(path, '/');
	if (slash == path)
		strlcpy(parent, "/", sizeof(parent));
	else {
		len = (size_t)(slash - path);
		if (len >= sizeof(parent)) {
			errno = ENAMETOOLONG;
			return (-1);
		}
		memcpy(parent, path, len);
		parent[len] = '\0';
	}
	if (lstat(parent, &st) == -1 && errno == ENOENT &&
	    strcmp(parent, VM_DISPLAY_DIR) == 0) {
		if (mkdir(parent, 0755) == -1 && errno != EEXIST)
			return (-1);
	}
	if (lstat(parent, &st) == -1)
		return (-1);
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		errno = EPERM;
		return (-1);
	}
	return (0);
}

int
display_socket_open(const char *path, uid_t owner, int *fdp, dev_t *devp,
    ino_t *inop)
{
	struct sockaddr_un sun;
	struct stat st;
	mode_t old_umask;
	int fd = -1, saved_errno;

	*fdp = -1;
	*devp = 0;
	*inop = 0;
	if (display_path_validate(path) == -1 ||
	    display_parent_validate(path) == -1)
		return (-1);
	if (lstat(path, &st) == 0) {
		errno = EEXIST;
		return (-1);
	}
	if (errno != ENOENT)
		return (-1);

	if ((fd = socket(AF_UNIX,
	    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) == -1)
		return (-1);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	if (strlcpy(sun.sun_path, path, sizeof(sun.sun_path)) >=
	    sizeof(sun.sun_path)) {
		errno = ENAMETOOLONG;
		goto fail;
	}

	old_umask = umask(077);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		saved_errno = errno;
		(void)umask(old_umask);
		errno = saved_errno;
		goto fail;
	}
	(void)umask(old_umask);

	if (lstat(path, &st) == -1 || !S_ISSOCK(st.st_mode))
		goto fail_unlink;
	*devp = st.st_dev;
	*inop = st.st_ino;
	if (lchown(path, owner, 0) == -1 || chmod(path, 0600) == -1 ||
	    listen(fd, 1) == -1)
		goto fail_unlink;
	if (lstat(path, &st) == -1 || !S_ISSOCK(st.st_mode) ||
	    st.st_dev != *devp || st.st_ino != *inop) {
		errno = ESTALE;
		goto fail_unlink;
	}

	*fdp = fd;
	return (0);

fail_unlink:
	saved_errno = errno;
	display_socket_close(path, &fd, *devp, *inop, 1);
	errno = saved_errno;
	return (-1);

fail:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return (-1);
}

void
display_socket_close(const char *path, int *fdp, dev_t dev, ino_t ino,
    int remove)
{
	struct stat st;

	if (*fdp != -1) {
		close(*fdp);
		*fdp = -1;
	}
	if (!remove || dev == 0 || ino == 0)
		return;
	if (lstat(path, &st) == -1 || !S_ISSOCK(st.st_mode) ||
	    st.st_dev != dev || st.st_ino != ino)
		return;
	(void)unlink(path);
}
