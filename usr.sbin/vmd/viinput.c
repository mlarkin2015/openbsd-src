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
#include <dev/pci/pcidevs.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "pci.h"
#include "viinput.h"
#include "virtio.h"
#include "vmd.h"

/* VirtIO input configuration selectors. */
#define VIINPUT_CFG_UNSET	0x00
#define VIINPUT_CFG_ID_NAME	0x01
#define VIINPUT_CFG_ID_DEVIDS	0x03
#define VIINPUT_CFG_PROP_BITS	0x10
#define VIINPUT_CFG_EV_BITS	0x11
#define VIINPUT_CFG_ABS_INFO	0x12

/* Linux input-event ABI values used by VirtIO input. */
#define VIINPUT_EV_SYN		0x00
#define VIINPUT_EV_KEY		0x01
#define VIINPUT_EV_REL		0x02
#define VIINPUT_EV_ABS		0x03
#define VIINPUT_SYN_REPORT	0x00
#define VIINPUT_REL_WHEEL	0x08
#define VIINPUT_ABS_X		0x00
#define VIINPUT_ABS_Y		0x01
#define VIINPUT_BTN_LEFT	0x110
#define VIINPUT_BTN_RIGHT	0x111
#define VIINPUT_BTN_MIDDLE	0x112

#define VIINPUT_BUS_VIRTUAL	0x06
#define VIINPUT_ABS_MAX		0x7fff
#define VIINPUT_REPORT_MAX	8
#define VIINPUT_ISR_QUEUE	0x01

struct viinput_absinfo {
	uint32_t min;
	uint32_t max;
	uint32_t fuzz;
	uint32_t flat;
	uint32_t res;
} __packed;

struct viinput_devids {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
} __packed;

struct viinput_config {
	uint8_t select;
	uint8_t subsel;
	uint8_t size;
	uint8_t reserved[5];
	union {
		char string[128];
		uint8_t bitmap[128];
		struct viinput_absinfo abs;
		struct viinput_devids ids;
	} u;
} __packed;

struct viinput_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} __packed;

CTASSERT(sizeof(struct viinput_config) == VIINPUT_CONFIG_SIZE);
CTASSERT(sizeof(struct viinput_event) == 8);

struct virtio_dev viinput;

static void	viinput_config(struct virtio_dev *, struct viinput_config *);
static int	viinput_buffer_valid(struct virtio_vq_info *, uint16_t, int);
static int	viinput_buffer_io(struct virtio_vq_info *, uint16_t, void *,
		    int);
static int	viinput_send_report(struct virtio_dev *,
		    const struct viinput_event *, size_t);
static int	viinput_drain_status(struct virtio_dev *);
static uint32_t	viinput_scale(uint32_t, uint32_t);

int
viinput_init(struct virtio_dev *dev)
{
	int ret;

	ret = pthread_mutex_init(&dev->viinput.mutex, NULL);
	if (ret != 0) {
		errno = ret;
		return (-1);
	}
	dev->viinput.initialized = 1;
	return (0);
}

void
viinput_reset(struct virtio_dev *dev)
{
	dev->viinput.select = VIINPUT_CFG_UNSET;
	dev->viinput.subsel = 0;
	dev->viinput.buttons = 0;
}

static void
viinput_set_bit(uint8_t *bitmap, size_t bitmap_size, unsigned int bit)
{
	if (bit / 8 < bitmap_size)
		bitmap[bit / 8] |= 1U << (bit % 8);
}

static void
viinput_config(struct virtio_dev *dev, struct viinput_config *cfg)
{
	static const char name[] = "OpenBSD VMM VirtIO Tablet";
	uint8_t select = dev->viinput.select;
	uint8_t subsel = dev->viinput.subsel;

	memset(cfg, 0, sizeof(*cfg));
	switch (select) {
	case VIINPUT_CFG_ID_NAME:
		if (subsel != 0)
			break;
		cfg->select = select;
		cfg->size = sizeof(name) - 1;
		memcpy(cfg->u.string, name, sizeof(name) - 1);
		break;
	case VIINPUT_CFG_ID_DEVIDS:
		if (subsel != 0)
			break;
		cfg->select = select;
		cfg->size = sizeof(cfg->u.ids);
		cfg->u.ids.bustype = htole16(VIINPUT_BUS_VIRTUAL);
		cfg->u.ids.vendor = htole16(PCI_VENDOR_OPENBSD);
		cfg->u.ids.product = htole16(3);
		cfg->u.ids.version = htole16(1);
		break;
	case VIINPUT_CFG_PROP_BITS:
		/* No INPUT_PROP_DIRECT: this is a tablet, not a touchscreen. */
		break;
	case VIINPUT_CFG_EV_BITS:
		switch (subsel) {
		case VIINPUT_EV_KEY:
			cfg->select = select;
			cfg->subsel = subsel;
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_BTN_LEFT);
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_BTN_RIGHT);
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_BTN_MIDDLE);
			cfg->size = VIINPUT_BTN_MIDDLE / 8 + 1;
			break;
		case VIINPUT_EV_REL:
			cfg->select = select;
			cfg->subsel = subsel;
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_REL_WHEEL);
			cfg->size = VIINPUT_REL_WHEEL / 8 + 1;
			break;
		case VIINPUT_EV_ABS:
			cfg->select = select;
			cfg->subsel = subsel;
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_ABS_X);
			viinput_set_bit(cfg->u.bitmap, sizeof(cfg->u.bitmap),
			    VIINPUT_ABS_Y);
			cfg->size = 1;
			break;
		}
		break;
	case VIINPUT_CFG_ABS_INFO:
		if (subsel == VIINPUT_ABS_X || subsel == VIINPUT_ABS_Y) {
			cfg->select = select;
			cfg->subsel = subsel;
			cfg->size = sizeof(cfg->u.abs);
			cfg->u.abs.min = htole32(0);
			cfg->u.abs.max = htole32(VIINPUT_ABS_MAX);
		}
		break;
	}
}

uint32_t
viinput_cfg_io(struct virtio_dev *dev, int dir, uint16_t reg, uint32_t data,
    uint8_t sz)
{
	struct viinput_config cfg;
	uint8_t *bytes = (uint8_t *)&cfg;
	uint32_t value = 0;
	uint16_t end;
	size_t i;

	if (sz != 1 && sz != 2 && sz != 4)
		return (UINT32_MAX);
	end = reg + sz;
	if (end > sizeof(cfg))
		return (UINT32_MAX);

	if (dir == VEI_DIR_OUT) {
		/* Only select and subsel are driver-writable. */
		for (i = 0; i < sz; i++) {
			if (reg + i == 0)
				dev->viinput.select = (data >> (i * 8)) & 0xff;
			else if (reg + i == 1)
				dev->viinput.subsel = (data >> (i * 8)) & 0xff;
		}
		return (0);
	}

	viinput_config(dev, &cfg);
	memcpy(&value, bytes + reg, sz);
	return (le32toh(value));
}

/*
 * Validate one event/status buffer.  VirtIO permits a buffer to be split
 * over a descriptor chain, so accept any finite chain with at least one
 * complete input event in the expected direction.
 */
static int
viinput_buffer_valid(struct virtio_vq_info *vq, uint16_t head, int writable)
{
	struct vring_desc *table = vq->q_hva;
	struct vring_desc *desc;
	uint64_t addr;
	uint32_t len;
	uint16_t flags, next;
	size_t left = sizeof(struct viinput_event), ndesc;

	if (head >= vq->qs)
		return (0);
	for (ndesc = 0; ndesc < vq->qs; ndesc++) {
		desc = &table[head];
		flags = le16toh(desc->flags);
		if ((flags & VRING_DESC_F_INDIRECT) != 0 ||
		    !!(flags & VRING_DESC_F_WRITE) != writable)
			return (0);
		addr = le64toh(desc->addr);
		len = le32toh(desc->len);
		if (len > left)
			len = left;
		if (len != 0 && hvaddr_mem(addr, len) == NULL)
			return (0);
		left -= len;
		if (left == 0)
			return (1);
		if ((flags & VRING_DESC_F_NEXT) == 0)
			return (0);
		next = le16toh(desc->next);
		if (next >= vq->qs)
			return (0);
		head = next;
	}
	return (0);
}

static int
viinput_buffer_io(struct virtio_vq_info *vq, uint16_t head, void *buf,
    int do_write)
{
	struct vring_desc *table = vq->q_hva;
	struct vring_desc *desc;
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	size_t left = sizeof(struct viinput_event), offset = 0, ndesc;

	for (ndesc = 0; ndesc < vq->qs && left != 0; ndesc++) {
		desc = &table[head];
		flags = le16toh(desc->flags);
		addr = le64toh(desc->addr);
		len = le32toh(desc->len);
		if (len > left)
			len = left;
		if (len != 0 && (do_write ?
		    write_mem(addr, (uint8_t *)buf + offset, len) :
		    read_mem(addr, (uint8_t *)buf + offset, len)) != 0)
			return (-1);
		offset += len;
		left -= len;
		if (left == 0)
			break;
		if ((flags & VRING_DESC_F_NEXT) == 0)
			return (-1);
		head = le16toh(desc->next);
	}
	return (left == 0 ? 0 : -1);
}

static int
viinput_send_report(struct virtio_dev *dev, const struct viinput_event *events,
    size_t nevents)
{
	struct virtio_vq_info *vq = &dev->vq[VIINPUT_EVENTQ];
	struct vring_avail *avail;
	struct vring_used *used;
	struct viinput_event event;
	uint16_t avail_idx, head, pending, used_idx;
	size_t i;

	if ((dev->status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) == 0 ||
	    !vq->vq_enabled || vq->q_hva == NULL ||
	    vq->q_avail_hva == NULL || vq->q_used_hva == NULL)
		return (0);

	avail = vq->q_avail_hva;
	used = vq->q_used_hva;
	avail_idx = le16toh(avail->idx);
	pending = (uint16_t)(avail_idx - vq->last_avail);
	if (pending > vq->qs || pending < nevents)
		return (0);

	/* Keep a report atomic: first validate every destination buffer. */
	for (i = 0; i < nevents; i++) {
		head = le16toh(avail->ring[(vq->last_avail + i) & vq->mask]);
		if (!viinput_buffer_valid(vq, head, 1)) {
			log_warnx("viinput: invalid eventq descriptor");
			return (0);
		}
	}

	used_idx = le16toh(used->idx);
	for (i = 0; i < nevents; i++) {
		head = le16toh(avail->ring[(vq->last_avail + i) & vq->mask]);
		event = events[i];
		if (viinput_buffer_io(vq, head, &event, 1) != 0) {
			log_warnx("viinput: failed to write eventq descriptor");
			return (0);
		}
		used->ring[(used_idx + i) & vq->mask].id = htole32(head);
		used->ring[(used_idx + i) & vq->mask].len =
		    htole32(sizeof(events[i]));
	}
	__sync_synchronize();
	used->idx = htole16(used_idx + nevents);
	vq->last_avail += nevents;

	if ((le16toh(avail->flags) & VRING_AVAIL_F_NO_INTERRUPT) != 0)
		return (0);
	dev->isr |= VIINPUT_ISR_QUEUE;
	return (1);
}

static int
viinput_drain_status(struct virtio_dev *dev)
{
	struct virtio_vq_info *vq = &dev->vq[VIINPUT_STATUSQ];
	struct viinput_event event;
	struct vring_avail *avail;
	struct vring_used *used;
	uint16_t avail_idx, head, pending, used_idx;
	int consumed = 0;

	if (!vq->vq_enabled || vq->q_hva == NULL ||
	    vq->q_avail_hva == NULL || vq->q_used_hva == NULL)
		return (0);
	avail = vq->q_avail_hva;
	used = vq->q_used_hva;
	avail_idx = le16toh(avail->idx);
	pending = (uint16_t)(avail_idx - vq->last_avail);
	if (pending > vq->qs) {
		log_warnx("viinput: invalid statusq producer index");
		return (0);
	}
	used_idx = le16toh(used->idx);

	while (vq->last_avail != avail_idx) {
		head = le16toh(avail->ring[vq->last_avail & vq->mask]);
		memset(&event, 0, sizeof(event));
		if (!viinput_buffer_valid(vq, head, 0) ||
		    viinput_buffer_io(vq, head, &event, 0) != 0) {
			log_warnx("viinput: invalid statusq descriptor");
			used->ring[used_idx & vq->mask].len = htole32(0);
		} else {
			/* Tablet status events have no device-side action. */
			used->ring[used_idx & vq->mask].len =
			    htole32(sizeof(event));
		}
		used->ring[used_idx & vq->mask].id = htole32(head);
		vq->last_avail++;
		used_idx++;
		consumed = 1;
	}
	if (!consumed)
		return (0);
	__sync_synchronize();
	used->idx = htole16(used_idx);
	if ((le16toh(avail->flags) & VRING_AVAIL_F_NO_INTERRUPT) != 0)
		return (0);
	dev->isr |= VIINPUT_ISR_QUEUE;
	return (1);
}

int
viinput_notifyq(struct virtio_dev *dev, uint16_t idx)
{
	if (idx == VIINPUT_EVENTQ)
		return (0);
	if (idx == VIINPUT_STATUSQ)
		return (viinput_drain_status(dev));
	return (0);
}

static uint32_t
viinput_scale(uint32_t value, uint32_t limit)
{
	if (limit <= 1)
		return (0);
	if (value >= limit)
		value = limit - 1;
	return ((uint64_t)value * VIINPUT_ABS_MAX / (limit - 1));
}

void
viinput_pointer_event(uint8_t buttons, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
	struct viinput_event events[VIINPUT_REPORT_MAX];
	struct viinput_dev *input = &viinput.viinput;
	uint8_t changed;
	size_t n = 0;
	int notify;

	if (!input->initialized)
		return;
	mutex_lock(&input->mutex);
	if ((viinput.status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) == 0) {
		mutex_unlock(&input->mutex);
		return;
	}

	changed = buttons ^ input->buttons;
#define VIINPUT_BUTTON(_mask, _code) do {				\
	if (changed & (_mask)) {					\
		events[n].type = htole16(VIINPUT_EV_KEY);		\
		events[n].code = htole16((_code));			\
		events[n].value = htole32(!!(buttons & (_mask)));	\
		n++;						\
	}							\
} while (0)
	VIINPUT_BUTTON(1U << 0, VIINPUT_BTN_LEFT);
	VIINPUT_BUTTON(1U << 1, VIINPUT_BTN_MIDDLE);
	VIINPUT_BUTTON(1U << 2, VIINPUT_BTN_RIGHT);
#undef VIINPUT_BUTTON
	/* RFB wheel buttons are momentary; report only their press edge. */
	if ((changed & buttons & (1U << 3)) != 0) {
		events[n].type = htole16(VIINPUT_EV_REL);
		events[n].code = htole16(VIINPUT_REL_WHEEL);
		events[n++].value = htole32(1);
	}
	if ((changed & buttons & (1U << 4)) != 0) {
		events[n].type = htole16(VIINPUT_EV_REL);
		events[n].code = htole16(VIINPUT_REL_WHEEL);
		events[n++].value = htole32((uint32_t)-1);
	}
	events[n].type = htole16(VIINPUT_EV_ABS);
	events[n].code = htole16(VIINPUT_ABS_X);
	events[n++].value = htole32(viinput_scale(x, width));
	events[n].type = htole16(VIINPUT_EV_ABS);
	events[n].code = htole16(VIINPUT_ABS_Y);
	events[n++].value = htole32(viinput_scale(y, height));
	events[n].type = htole16(VIINPUT_EV_SYN);
	events[n].code = htole16(VIINPUT_SYN_REPORT);
	events[n++].value = htole32(0);

	input->buttons = buttons;
	notify = viinput_send_report(&viinput, events, n);
	if (notify)
		virtio_inject_irq(&viinput, VIINPUT_EVENTQ);
	mutex_unlock(&input->mutex);
}
