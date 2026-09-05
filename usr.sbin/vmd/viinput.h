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

#ifndef _VIINPUT_H_
#define _VIINPUT_H_

#include <sys/types.h>

#include <pthread.h>

#define VIINPUT_CONFIG_SIZE	136
#define VIINPUT_QUEUE_SIZE	64
#define VIINPUT_QUEUES		2
#define VIINPUT_EVENTQ		0
#define VIINPUT_STATUSQ		1

struct viinput_dev {
	pthread_mutex_t	 mutex;
	uint8_t		 select;
	uint8_t		 subsel;
	uint8_t		 buttons;
	uint8_t		 initialized;
};

struct virtio_dev;

int	viinput_init(struct virtio_dev *);
uint32_t viinput_cfg_io(struct virtio_dev *, int, uint16_t, uint32_t,
	    uint8_t);
int	viinput_notifyq(struct virtio_dev *, uint16_t);
void	viinput_reset(struct virtio_dev *);
void	viinput_pointer_event(uint8_t, uint32_t, uint32_t, uint32_t,
	    uint32_t);

#endif /* _VIINPUT_H_ */
