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

#include <dev/pv/virtioreg.h>

#include <endian.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viinput.h"
#include "virtio.h"

#define CHECK(_expr) do {						\
	if (!(_expr)) {						\
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n",		\
		    __FILE__, __LINE__, #_expr);			\
		exit(1);						\
	}							\
} while (0)

#define MEM_SIZE	0x10000
#define EVENT_DESC	0x1000
#define EVENT_AVAIL	0x2000
#define EVENT_USED	0x3000
#define EVENT_DATA	0x4000
#define STATUS_DESC	0x5000
#define STATUS_AVAIL	0x6000
#define STATUS_USED	0x7000
#define STATUS_DATA	0x8000

#define CFG_ID_NAME	0x01
#define CFG_ID_DEVIDS	0x03
#define CFG_PROP_BITS	0x10
#define CFG_EV_BITS	0x11
#define CFG_ABS_INFO	0x12
#define EV_KEY		0x01
#define EV_ABS		0x03
#define ABS_X		0x00
#define BTN_LEFT	0x110

struct test_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} __packed;

static uint8_t mem[MEM_SIZE];
static unsigned int irq_count;
static uint16_t last_irq_queue;
static unsigned int warning_count;

void *
hvaddr_mem(paddr_t gpa, size_t len)
{
	if (gpa > MEM_SIZE || len > MEM_SIZE - gpa)
		return (NULL);
	return (&mem[gpa]);
}

int
write_mem(paddr_t gpa, const void *buf, size_t len)
{
	void *dst = hvaddr_mem(gpa, len);

	if (dst == NULL)
		return (-1);
	memcpy(dst, buf, len);
	return (0);
}

int
read_mem(paddr_t gpa, void *buf, size_t len)
{
	void *src = hvaddr_mem(gpa, len);

	if (src == NULL)
		return (-1);
	memcpy(buf, src, len);
	return (0);
}

void
mutex_lock(pthread_mutex_t *mtx)
{
	CHECK(pthread_mutex_lock(mtx) == 0);
}

void
mutex_unlock(pthread_mutex_t *mtx)
{
	CHECK(pthread_mutex_unlock(mtx) == 0);
}

void
virtio_inject_irq(struct virtio_dev *dev, uint16_t queue)
{
	CHECK(dev == &viinput);
	irq_count++;
	last_irq_queue = queue;
}

void
log_warnx(const char *fmt, ...)
{
	(void)fmt;
	warning_count++;
}

static void
select_config(uint8_t select, uint8_t subsel)
{
	(void)viinput_cfg_io(&viinput, VEI_DIR_OUT, 0, select, 1);
	(void)viinput_cfg_io(&viinput, VEI_DIR_OUT, 1, subsel, 1);
}

static uint32_t
read_config(uint16_t reg, uint8_t sz)
{
	return (viinput_cfg_io(&viinput, VEI_DIR_IN, reg, 0, sz));
}

static void
setup_queue(struct virtio_vq_info *vq, paddr_t desc_gpa,
    paddr_t avail_gpa, paddr_t used_gpa)
{
	memset(vq, 0, sizeof(*vq));
	vq->q_gpa = desc_gpa;
	vq->q_avail_gpa = avail_gpa;
	vq->q_used_gpa = used_gpa;
	vq->q_hva = &mem[desc_gpa];
	vq->q_avail_hva = &mem[avail_gpa];
	vq->q_used_hva = &mem[used_gpa];
	vq->qs = VIINPUT_QUEUE_SIZE;
	vq->mask = VIINPUT_QUEUE_SIZE - 1;
	vq->vq_enabled = 1;
}

static void
post_event_buffers(uint16_t first, uint16_t count)
{
	struct virtio_vq_info *vq = &viinput.vq[VIINPUT_EVENTQ];
	struct vring_desc *desc = vq->q_hva;
	struct vring_avail *avail = vq->q_avail_hva;
	uint16_t i, slot;

	for (i = 0; i < count; i++) {
		slot = first + i;
		desc[slot].addr = htole64(EVENT_DATA + slot * 8);
		desc[slot].len = htole32(8);
		desc[slot].flags = htole16(VRING_DESC_F_WRITE);
		avail->ring[slot & vq->mask] = htole16(slot);
	}
	avail->idx = htole16(first + count);
}

static struct test_event *
event_at(uint16_t slot)
{
	return ((struct test_event *)&mem[EVENT_DATA + slot * 8]);
}

static void
test_config(void)
{
	static const char name[] = "OpenBSD VMM VirtIO Tablet";
	size_t i;

	select_config(CFG_ID_NAME, 0);
	CHECK(read_config(0, 1) == CFG_ID_NAME);
	CHECK(read_config(2, 1) == sizeof(name) - 1);
	for (i = 0; i < sizeof(name) - 1; i++)
		CHECK(read_config(8 + i, 1) == (uint8_t)name[i]);

	select_config(CFG_ID_DEVIDS, 0);
	CHECK(read_config(2, 1) == 8);
	CHECK(read_config(8, 2) == 6);

	select_config(CFG_EV_BITS, EV_ABS);
	CHECK(read_config(2, 1) == 1);
	CHECK((read_config(8, 1) & 0x03) == 0x03);
	select_config(CFG_EV_BITS, EV_KEY);
	CHECK(read_config(2, 1) == BTN_LEFT / 8 + 1);
	CHECK(read_config(8 + BTN_LEFT / 8, 1) & (1 << (BTN_LEFT % 8)));

	select_config(CFG_ABS_INFO, ABS_X);
	CHECK(read_config(2, 1) == 20);
	CHECK(read_config(8, 4) == 0);
	CHECK(read_config(12, 4) == 0x7fff);

	/* Unsupported selector/subselector combinations read as all zeroes. */
	select_config(CFG_PROP_BITS, 0);
	CHECK(read_config(0, 4) == 0);
	select_config(CFG_ID_NAME, 1);
	CHECK(read_config(0, 4) == 0);
	select_config(0xff, 0xff);
	CHECK(read_config(0, 4) == 0);

	/* The payload and size bytes are read-only. */
	select_config(CFG_ID_NAME, 0);
	(void)viinput_cfg_io(&viinput, VEI_DIR_OUT, 2, 0xff, 1);
	CHECK(read_config(2, 1) == sizeof(name) - 1);
	CHECK(viinput_cfg_io(&viinput, VEI_DIR_IN, 135, 0, 2) == UINT32_MAX);
}

static void
test_event_queue(void)
{
	struct virtio_vq_info *vq = &viinput.vq[VIINPUT_EVENTQ];
	struct vring_avail *avail = vq->q_avail_hva;
	struct vring_used *used = vq->q_used_hva;
	struct test_event *event;
	unsigned int old_irqs;

	post_event_buffers(0, 3);
	viinput_pointer_event(0, 50, 25, 101, 51);
	CHECK(le16toh(used->idx) == 3);
	CHECK(vq->last_avail == 3);
	CHECK(irq_count == 1 && last_irq_queue == VIINPUT_EVENTQ);
	event = event_at(0);
	CHECK(le16toh(event->type) == EV_ABS &&
	    le16toh(event->code) == ABS_X && le32toh(event->value) == 16383);
	event = event_at(1);
	CHECK(le16toh(event->type) == EV_ABS &&
	    le16toh(event->code) == 1 && le32toh(event->value) == 16383);
	event = event_at(2);
	CHECK(le16toh(event->type) == 0 && le16toh(event->code) == 0);

	/* A complete report is dropped rather than partially consumed. */
	post_event_buffers(3, 2);
	viinput_pointer_event(0, 20, 20, 101, 51);
	CHECK(le16toh(used->idx) == 3 && vq->last_avail == 3);
	post_event_buffers(3, 3);
	viinput_pointer_event(0, 20, 20, 101, 51);
	CHECK(le16toh(used->idx) == 6 && vq->last_avail == 6);

	/* Button changes and axes remain in a single synchronized report. */
	post_event_buffers(6, 4);
	viinput_pointer_event(1, 20, 20, 101, 51);
	CHECK(le16toh(used->idx) == 10);
	event = event_at(6);
	CHECK(le16toh(event->type) == EV_KEY &&
	    le16toh(event->code) == BTN_LEFT && le32toh(event->value) == 1);

	/* Honor the driver's interrupt-suppression flag. */
	old_irqs = irq_count;
	avail->flags = htole16(VRING_AVAIL_F_NO_INTERRUPT);
	post_event_buffers(10, 3);
	viinput_pointer_event(1, 100, 50, 101, 51);
	CHECK(le16toh(used->idx) == 13);
	CHECK(irq_count == old_irqs);
}

static void
test_status_queue(void)
{
	struct virtio_vq_info *vq = &viinput.vq[VIINPUT_STATUSQ];
	struct vring_desc *desc = vq->q_hva;
	struct vring_avail *avail = vq->q_avail_hva;
	struct vring_used *used = vq->q_used_hva;
	struct test_event event = { htole16(0x11), htole16(1), htole32(1) };
	unsigned int old_irqs = irq_count;

	memcpy(&mem[STATUS_DATA], &event, sizeof(event));
	desc[0].addr = htole64(STATUS_DATA);
	desc[0].len = htole32(sizeof(event));
	desc[0].flags = 0;
	avail->ring[0] = htole16(0);
	avail->idx = htole16(1);
	CHECK(viinput_notifyq(&viinput, VIINPUT_STATUSQ) == 1);
	CHECK(le16toh(used->idx) == 1 && vq->last_avail == 1);
	CHECK(le32toh(used->ring[0].id) == 0);
	CHECK(le32toh(used->ring[0].len) == sizeof(event));
	CHECK(viinput.isr & 1);
	virtio_inject_irq(&viinput, VIINPUT_STATUSQ);
	CHECK(irq_count == old_irqs + 1 && last_irq_queue == VIINPUT_STATUSQ);
}

int
main(void)
{
	memset(&viinput, 0, sizeof(viinput));
	CHECK(viinput_init(&viinput) == 0);
	viinput.device_id = PCI_PRODUCT_VIRTIO_INPUT;
	viinput.status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	viinput.num_queues = VIINPUT_QUEUES;
	setup_queue(&viinput.vq[VIINPUT_EVENTQ], EVENT_DESC, EVENT_AVAIL,
	    EVENT_USED);
	setup_queue(&viinput.vq[VIINPUT_STATUSQ], STATUS_DESC, STATUS_AVAIL,
	    STATUS_USED);

	test_config();
	test_event_queue();
	test_status_queue();
	CHECK(warning_count == 0);

	viinput.viinput.select = 1;
	viinput.viinput.subsel = 2;
	viinput.viinput.buttons = 7;
	viinput_reset(&viinput);
	CHECK(viinput.viinput.select == 0 && viinput.viinput.subsel == 0 &&
	    viinput.viinput.buttons == 0);

	puts("viinput: ok");
	return (0);
}
