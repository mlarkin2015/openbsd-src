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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/errno.h>

#include <machine/bus.h>

#include <dev/pv/virtioreg.h>
#include <dev/pv/virtiovar.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsmousevar.h>

#define VIOINPUT_EVENTQ		0
#define VIOINPUT_STATUSQ	1
#define VIOINPUT_NVQS		2

/* VirtIO input configuration selectors. */
#define VIOINPUT_CFG_UNSET	0x00
#define VIOINPUT_CFG_ID_NAME	0x01
#define VIOINPUT_CFG_EV_BITS	0x11
#define VIOINPUT_CFG_ABS_INFO	0x12

#define VIOINPUT_CFG_SELECT	0
#define VIOINPUT_CFG_SUBSEL	1
#define VIOINPUT_CFG_SIZE	2
#define VIOINPUT_CFG_DATA	8
#define VIOINPUT_CFG_DATA_SIZE	128

/* Linux input-event ABI values used by VirtIO input. */
#define VIOINPUT_EV_SYN		0x00
#define VIOINPUT_EV_KEY		0x01
#define VIOINPUT_EV_REL		0x02
#define VIOINPUT_EV_ABS		0x03
#define VIOINPUT_SYN_REPORT	0x00
#define VIOINPUT_REL_WHEEL	0x08
#define VIOINPUT_ABS_X		0x00
#define VIOINPUT_ABS_Y		0x01
#define VIOINPUT_BTN_LEFT	0x110
#define VIOINPUT_BTN_RIGHT	0x111
#define VIOINPUT_BTN_MIDDLE	0x112

struct vioinput_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} __packed;

struct vioinput_absinfo {
	int32_t min;
	int32_t max;
	uint32_t fuzz;
	uint32_t flat;
	uint32_t res;
} __packed;

struct vioinput_scale {
	int minx;
	int maxx;
	int miny;
	int maxy;
	int swapxy;
	int resx;
	int resy;
};

struct vioinput_softc {
	struct device		 sc_dev;
	struct virtio_softc	*sc_virtio;
	struct virtqueue	 sc_vqs[VIOINPUT_NVQS];

	struct vioinput_event	*sc_events;
	size_t			 sc_events_size;
	bus_dma_segment_t	 sc_events_seg;
	bus_dmamap_t		 sc_events_map;
	int			 sc_events_nsegs;

	struct device		*sc_wsmousedev;
	struct vioinput_scale	 sc_scale;
	int			 sc_rawmode;
	int			 sc_enabled;
	int			 sc_x;
	int			 sc_y;
	int			 sc_dz;
	u_int			 sc_buttons;
	u_int			 sc_dirty;
};

#define VIOINPUT_DIRTY_X	(1U << 0)
#define VIOINPUT_DIRTY_Y	(1U << 1)
#define VIOINPUT_DIRTY_BUTTONS	(1U << 2)
#define VIOINPUT_DIRTY_WHEEL	(1U << 3)

int	vioinput_match(struct device *, void *, void *);
void	vioinput_attach(struct device *, struct device *, void *);
int	vioinput_vq_done(struct virtqueue *);

int	vioinput_enable(void *);
void	vioinput_disable(void *);
int	vioinput_ioctl(void *, u_long, caddr_t, int, struct proc *);

static int	vioinput_has_event(struct vioinput_softc *, uint8_t,
		    uint16_t);
static int	vioinput_read_absinfo(struct vioinput_softc *, uint8_t,
		    struct vioinput_absinfo *);
static void	vioinput_fill(struct vioinput_softc *);
static void	vioinput_event(struct vioinput_softc *, uint16_t, uint16_t,
		    int32_t);
static void	vioinput_sync(struct vioinput_softc *);

const struct cfattach vioinput_ca = {
	sizeof(struct vioinput_softc),
	vioinput_match,
	vioinput_attach,
	NULL
};

struct cfdriver vioinput_cd = {
	NULL, "vioinput", DV_DULL
};

const struct wsmouse_accessops vioinput_accessops = {
	vioinput_enable,
	vioinput_ioctl,
	vioinput_disable
};

int
vioinput_match(struct device *parent, void *match, void *aux)
{
	struct virtio_attach_args *va = aux;

	return (va->va_devid == PCI_PRODUCT_VIRTIO_INPUT);
}

static void
vioinput_select(struct vioinput_softc *sc, uint8_t select, uint8_t subsel)
{
	struct virtio_softc *vsc = sc->sc_virtio;

	virtio_write_device_config_1(vsc, VIOINPUT_CFG_SELECT, select);
	virtio_write_device_config_1(vsc, VIOINPUT_CFG_SUBSEL, subsel);
}

static int
vioinput_has_event(struct vioinput_softc *sc, uint8_t type, uint16_t code)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	uint8_t size;

	vioinput_select(sc, VIOINPUT_CFG_EV_BITS, type);
	size = virtio_read_device_config_1(vsc, VIOINPUT_CFG_SIZE);
	if (code / NBBY >= size || code / NBBY >= VIOINPUT_CFG_DATA_SIZE)
		return (0);

	return ((virtio_read_device_config_1(vsc,
	    VIOINPUT_CFG_DATA + code / NBBY) & (1U << (code % NBBY))) != 0);
}

static int
vioinput_read_absinfo(struct vioinput_softc *sc, uint8_t axis,
    struct vioinput_absinfo *abs)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	uint8_t size;

	vioinput_select(sc, VIOINPUT_CFG_ABS_INFO, axis);
	size = virtio_read_device_config_1(vsc, VIOINPUT_CFG_SIZE);
	if (size < sizeof(*abs))
		return (ENXIO);

	abs->min = (int32_t)letoh32(virtio_read_device_config_4(vsc,
	    VIOINPUT_CFG_DATA + offsetof(struct vioinput_absinfo, min)));
	abs->max = (int32_t)letoh32(virtio_read_device_config_4(vsc,
	    VIOINPUT_CFG_DATA + offsetof(struct vioinput_absinfo, max)));
	abs->fuzz = letoh32(virtio_read_device_config_4(vsc,
	    VIOINPUT_CFG_DATA + offsetof(struct vioinput_absinfo, fuzz)));
	abs->flat = letoh32(virtio_read_device_config_4(vsc,
	    VIOINPUT_CFG_DATA + offsetof(struct vioinput_absinfo, flat)));
	abs->res = letoh32(virtio_read_device_config_4(vsc,
	    VIOINPUT_CFG_DATA + offsetof(struct vioinput_absinfo, res)));
	if (abs->min >= abs->max)
		return (EINVAL);

	return (0);
}

void
vioinput_attach(struct device *parent, struct device *self, void *aux)
{
	struct vioinput_softc *sc = (struct vioinput_softc *)self;
	struct virtio_softc *vsc = (struct virtio_softc *)parent;
	struct virtio_attach_args *va = aux;
	struct vioinput_absinfo absx, absy;
	struct wsmousedev_attach_args waa;
	char name[VIOINPUT_CFG_DATA_SIZE + 1];
	caddr_t kva = NULL;
	uint8_t namelen;
	int i;

	if (vsc->sc_child != NULL) {
		printf(": child already attached\n");
		return;
	}

	sc->sc_virtio = vsc;
	vsc->sc_child = self;
	vsc->sc_ipl = IPL_TTY;
	vsc->sc_vqs = sc->sc_vqs;
	vsc->sc_nvqs = 0;
	vsc->sc_driver_features = 0;

	if (virtio_negotiate_features(vsc, NULL) != 0)
		goto err;
	if (!vsc->sc_version_1) {
		printf(": requires virtio version 1\n");
		goto err;
	}
	if (!vioinput_has_event(sc, VIOINPUT_EV_ABS, VIOINPUT_ABS_X) ||
	    !vioinput_has_event(sc, VIOINPUT_EV_ABS, VIOINPUT_ABS_Y) ||
	    vioinput_read_absinfo(sc, VIOINPUT_ABS_X, &absx) != 0 ||
	    vioinput_read_absinfo(sc, VIOINPUT_ABS_Y, &absy) != 0) {
		printf(": unsupported input device\n");
		goto err;
	}

	vioinput_select(sc, VIOINPUT_CFG_ID_NAME, 0);
	namelen = virtio_read_device_config_1(vsc, VIOINPUT_CFG_SIZE);
	namelen = MIN(namelen, VIOINPUT_CFG_DATA_SIZE);
	for (i = 0; i < namelen; i++)
		name[i] = virtio_read_device_config_1(vsc,
		    VIOINPUT_CFG_DATA + i);
	name[namelen] = '\0';

	for (i = 0; i < VIOINPUT_NVQS; i++) {
		if (virtio_alloc_vq(vsc, &sc->sc_vqs[i], i, 1,
		    i == VIOINPUT_EVENTQ ? "input event" : "input status") != 0)
			goto err;
		vsc->sc_nvqs++;
	}

	sc->sc_events_size = sc->sc_vqs[VIOINPUT_EVENTQ].vq_num *
	    sizeof(*sc->sc_events);
	if (bus_dmamap_create(vsc->sc_dmat, sc->sc_events_size, 1,
	    sc->sc_events_size, 0, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW,
	    &sc->sc_events_map) != 0)
		goto err;
	if (bus_dmamem_alloc(vsc->sc_dmat, sc->sc_events_size,
	    sizeof(uint64_t), 0, &sc->sc_events_seg, 1,
	    &sc->sc_events_nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO) != 0)
		goto err;
	if (bus_dmamem_map(vsc->sc_dmat, &sc->sc_events_seg,
	    sc->sc_events_nsegs, sc->sc_events_size, &kva,
	    BUS_DMA_NOWAIT) != 0)
		goto err;
	sc->sc_events = (struct vioinput_event *)kva;
	if (bus_dmamap_load(vsc->sc_dmat, sc->sc_events_map,
	    sc->sc_events, sc->sc_events_size, NULL, BUS_DMA_NOWAIT) != 0)
		goto err;

	sc->sc_vqs[VIOINPUT_EVENTQ].vq_done = vioinput_vq_done;
	virtio_start_vq_intr(vsc, &sc->sc_vqs[VIOINPUT_EVENTQ]);
	printf(": %s\n", name[0] != '\0' ? name : "absolute tablet");
	if (virtio_attach_finish(vsc, va) != 0)
		goto err;

	sc->sc_scale.minx = absx.min;
	sc->sc_scale.maxx = absx.max;
	sc->sc_scale.miny = absy.min;
	sc->sc_scale.maxy = absy.max;
	sc->sc_scale.resx = absx.max - absx.min;
	sc->sc_scale.resy = absy.max - absy.min;
	sc->sc_rawmode = 1;

	vioinput_fill(sc);

	waa.accessops = &vioinput_accessops;
	waa.accesscookie = sc;
	sc->sc_wsmousedev = config_found(self, &waa, wsmousedevprint);
	return;

err:
	if (sc->sc_events_map != NULL && sc->sc_events != NULL)
		bus_dmamap_unload(vsc->sc_dmat, sc->sc_events_map);
	if (sc->sc_events != NULL)
		bus_dmamem_unmap(vsc->sc_dmat, (caddr_t)sc->sc_events,
		    sc->sc_events_size);
	if (sc->sc_events_nsegs != 0)
		bus_dmamem_free(vsc->sc_dmat, &sc->sc_events_seg,
		    sc->sc_events_nsegs);
	if (sc->sc_events_map != NULL)
		bus_dmamap_destroy(vsc->sc_dmat, sc->sc_events_map);
	for (i = 0; i < vsc->sc_nvqs; i++)
		virtio_free_vq(vsc, &sc->sc_vqs[i]);
	vsc->sc_nvqs = 0;
	vsc->sc_child = VIRTIO_CHILD_ERROR;
}

static void
vioinput_fill(struct vioinput_softc *sc)
{
	struct virtio_softc *vsc = sc->sc_virtio;
	struct virtqueue *vq = &sc->sc_vqs[VIOINPUT_EVENTQ];
	size_t off;
	int nfilled = 0, slot;

	while (virtio_enqueue_prep(vq, &slot) == 0) {
		if (virtio_enqueue_reserve(vq, slot, 1) != 0)
			break;
		off = slot * sizeof(*sc->sc_events);
		bus_dmamap_sync(vsc->sc_dmat, sc->sc_events_map, off,
		    sizeof(*sc->sc_events), BUS_DMASYNC_PREREAD);
		virtio_enqueue_p(vq, slot, sc->sc_events_map, off,
		    sizeof(*sc->sc_events), 0);
		virtio_enqueue_commit(vsc, vq, slot, 0);
		nfilled++;
	}
	if (nfilled != 0)
		virtio_notify(vsc, vq);
}

int
vioinput_vq_done(struct virtqueue *vq)
{
	struct virtio_softc *vsc = vq->vq_owner;
	struct vioinput_softc *sc = (struct vioinput_softc *)vsc->sc_child;
	struct vioinput_event *event;
	size_t off;
	uint16_t type, code;
	int32_t value;
	int handled = 0, len, slot;

	while (virtio_dequeue(vsc, vq, &slot, &len) == 0) {
		off = slot * sizeof(*sc->sc_events);
		bus_dmamap_sync(vsc->sc_dmat, sc->sc_events_map, off,
		    sizeof(*sc->sc_events), BUS_DMASYNC_POSTREAD);
		if (len == sizeof(*event)) {
			event = &sc->sc_events[slot];
			type = letoh16(event->type);
			code = letoh16(event->code);
			value = (int32_t)letoh32(event->value);
			vioinput_event(sc, type, code, value);
		}
		virtio_dequeue_commit(vq, slot);
		handled = 1;
	}

	vioinput_fill(sc);
	return (handled);
}

static void
vioinput_event(struct vioinput_softc *sc, uint16_t type, uint16_t code,
    int32_t value)
{
	u_int mask;

	switch (type) {
	case VIOINPUT_EV_SYN:
		if (code == VIOINPUT_SYN_REPORT)
			vioinput_sync(sc);
		break;
	case VIOINPUT_EV_KEY:
		switch (code) {
		case VIOINPUT_BTN_LEFT:
			mask = 1U << 0;
			break;
		case VIOINPUT_BTN_MIDDLE:
			mask = 1U << 1;
			break;
		case VIOINPUT_BTN_RIGHT:
			mask = 1U << 2;
			break;
		default:
			return;
		}
		if (value != 0)
			sc->sc_buttons |= mask;
		else
			sc->sc_buttons &= ~mask;
		sc->sc_dirty |= VIOINPUT_DIRTY_BUTTONS;
		break;
	case VIOINPUT_EV_REL:
		if (code == VIOINPUT_REL_WHEEL) {
			sc->sc_dz += value;
			sc->sc_dirty |= VIOINPUT_DIRTY_WHEEL;
		}
		break;
	case VIOINPUT_EV_ABS:
		if (code == VIOINPUT_ABS_X) {
			sc->sc_x = value;
			sc->sc_dirty |= VIOINPUT_DIRTY_X;
		} else if (code == VIOINPUT_ABS_Y) {
			sc->sc_y = value;
			sc->sc_dirty |= VIOINPUT_DIRTY_Y;
		}
		break;
	}
}

static void
vioinput_sync(struct vioinput_softc *sc)
{
	int dz, s, x, y;

	if (sc->sc_dirty == 0)
		return;
	dz = sc->sc_dz;
	sc->sc_dz = 0;
	if (!sc->sc_enabled || sc->sc_wsmousedev == NULL) {
		sc->sc_dirty = 0;
		return;
	}

	x = sc->sc_x;
	y = sc->sc_y;
	if (!sc->sc_rawmode) {
		if (sc->sc_scale.swapxy) {
			int tmp = x;
			x = y;
			y = tmp;
		}
		if (sc->sc_scale.maxx != sc->sc_scale.minx &&
		    sc->sc_scale.maxy != sc->sc_scale.miny) {
			x = ((x - sc->sc_scale.minx) * sc->sc_scale.resx) /
			    (sc->sc_scale.maxx - sc->sc_scale.minx);
			y = ((y - sc->sc_scale.miny) * sc->sc_scale.resy) /
			    (sc->sc_scale.maxy - sc->sc_scale.miny);
		}
	}

	s = spltty();
	if (sc->sc_dirty & (VIOINPUT_DIRTY_X | VIOINPUT_DIRTY_Y))
		wsmouse_position(sc->sc_wsmousedev, x, y);
	wsmouse_buttons(sc->sc_wsmousedev, sc->sc_buttons);
	if (dz != 0)
		wsmouse_motion(sc->sc_wsmousedev, 0, 0, dz, 0);
	wsmouse_input_sync(sc->sc_wsmousedev);
	splx(s);
	sc->sc_dirty = 0;
}

int
vioinput_enable(void *arg)
{
	struct vioinput_softc *sc = arg;

	if (sc->sc_enabled)
		return (EBUSY);
	sc->sc_enabled = 1;
	sc->sc_dz = 0;
	sc->sc_dirty = 0;
	return (0);
}

void
vioinput_disable(void *arg)
{
	struct vioinput_softc *sc = arg;

	sc->sc_enabled = 0;
}

int
vioinput_ioctl(void *arg, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct vioinput_softc *sc = arg;
	struct wsmouse_calibcoords *wsmc =
	    (struct wsmouse_calibcoords *)data;

	switch (cmd) {
	case WSMOUSEIO_GTYPE:
		*(u_int *)data = WSMOUSE_TYPE_TPANEL;
		return (0);
	case WSMOUSEIO_SCALIBCOORDS:
		if (wsmc->minx < -32768 || wsmc->minx >= 32768 ||
		    wsmc->maxx < -32768 || wsmc->maxx >= 32768 ||
		    wsmc->miny < -32768 || wsmc->miny >= 32768 ||
		    wsmc->maxy < -32768 || wsmc->maxy >= 32768 ||
		    wsmc->minx == wsmc->maxx || wsmc->miny == wsmc->maxy ||
		    wsmc->resx < 0 || wsmc->resx >= 32768 ||
		    wsmc->resy < 0 || wsmc->resy >= 32768 ||
		    wsmc->swapxy < 0 || wsmc->swapxy > 1 ||
		    wsmc->samplelen < 0 || wsmc->samplelen > 1)
			return (EINVAL);
		sc->sc_scale.minx = wsmc->minx;
		sc->sc_scale.maxx = wsmc->maxx;
		sc->sc_scale.miny = wsmc->miny;
		sc->sc_scale.maxy = wsmc->maxy;
		sc->sc_scale.swapxy = wsmc->swapxy;
		sc->sc_scale.resx = wsmc->resx;
		sc->sc_scale.resy = wsmc->resy;
		sc->sc_rawmode = wsmc->samplelen;
		return (0);
	case WSMOUSEIO_GCALIBCOORDS:
		wsmc->minx = sc->sc_scale.minx;
		wsmc->maxx = sc->sc_scale.maxx;
		wsmc->miny = sc->sc_scale.miny;
		wsmc->maxy = sc->sc_scale.maxy;
		wsmc->swapxy = sc->sc_scale.swapxy;
		wsmc->resx = sc->sc_scale.resx;
		wsmc->resy = sc->sc_scale.resy;
		wsmc->samplelen = sc->sc_rawmode;
		return (0);
	default:
		return (-1);
	}
}
