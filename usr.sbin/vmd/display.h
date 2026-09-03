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

#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include <sys/types.h>

#define VM_DISPLAY_DIR		"/var/run/vmd"

#define DISPLAY_SURFACE_MAGIC	0x564d4446U	/* VMDF */
#define DISPLAY_SURFACE_VERSION	1
#define DISPLAY_DEFAULT_WIDTH	800U
#define DISPLAY_DEFAULT_HEIGHT	600U
#define DISPLAY_MAX_WIDTH	4096U
#define DISPLAY_MAX_HEIGHT	2160U
#define DISPLAY_BPP		4U
#define DISPLAY_FORMAT_XRGB8888	0x34325258U

struct display_surface {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t sequence;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t generation;
	uint8_t pixels[];
};

struct display_frame {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t generation;
};

enum display_input_type {
	DISPLAY_INPUT_KEY = 1,
	DISPLAY_INPUT_POINTER
};

struct display_input {
	uint8_t type;
	uint8_t down;
	uint8_t buttons;
	uint8_t reserved;
	uint32_t value;
	uint32_t x;
	uint32_t y;
};

struct vmop_create_params;
struct vmd_vm;

int	display_path_default(const struct vmop_create_params *, char *, size_t);
int	display_path_validate(const char *);
int	display_socket_open(const char *, uid_t, int *, dev_t *, ino_t *);
void	display_socket_close(const char *, int *, dev_t, ino_t, int);
size_t	display_surface_size(void);
int	display_surface_open(int *);
int	display_surface_map(int, int, struct display_surface **);
void	display_surface_init(struct display_surface *);
int	display_surface_update(struct display_surface *, const void *, uint32_t,
	    uint32_t, uint32_t, uint32_t);
int	display_surface_snapshot(const struct display_surface *, void *, size_t,
	    struct display_frame *);

int	display_start(struct vmd_vm *);
void	display_stop(void);
void	display_main(int, int, int, const char *) __dead;
struct display_surface *display_get_surface(void);
#ifdef DISPLAY_RFB_TEST
int	display_rfb_test_client(int, int, const struct display_surface *);
#endif

#endif /* _DISPLAY_H_ */
