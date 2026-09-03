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

struct vmop_create_params;

int	display_path_default(const struct vmop_create_params *, char *, size_t);
int	display_path_validate(const char *);
int	display_socket_open(const char *, uid_t, int *, dev_t *, ino_t *);
void	display_socket_close(const char *, int *, dev_t, ino_t, int);

#endif /* _DISPLAY_H_ */
