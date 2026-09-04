/*	$OpenBSD $	*/

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

#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <event.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "hpet.h"
#include "mmio.h"
#include "vmd.h"

#define HPET_CAPABILITIES	0x000
#define HPET_CONFIGURATION	0x010
#define HPET_INTERRUPT_STATUS	0x020
#define HPET_MAIN_COUNTER	0x0f0
#define HPET_TIMER_BASE		0x100
#define HPET_TIMER_STRIDE	0x020
#define HPET_TIMER_CONFIG	0x000
#define HPET_TIMER_COMPARATOR	0x008
#define HPET_TIMER_FSB_ROUTE	0x010

#define HPET_CFG_ENABLE		(1ULL << 0)

#define HPET_TN_LEVEL		(1ULL << 1)
#define HPET_TN_ENABLE		(1ULL << 2)
#define HPET_TN_PERIODIC		(1ULL << 3)
#define HPET_TN_PERIODIC_CAP	(1ULL << 4)
#define HPET_TN_SIZE_CAP	(1ULL << 5)
#define HPET_TN_SETVAL		(1ULL << 6)
#define HPET_TN_32BIT		(1ULL << 8)
#define HPET_TN_ROUTE_SHIFT	9
#define HPET_TN_ROUTE_MASK	(0x1fULL << HPET_TN_ROUTE_SHIFT)
#define HPET_TN_WRITE_MASK	(HPET_TN_LEVEL | HPET_TN_ENABLE | \
	HPET_TN_PERIODIC | HPET_TN_SETVAL | HPET_TN_32BIT | \
	HPET_TN_ROUTE_MASK)

/* IRQ2, IRQ8 and PCI-style IOAPIC inputs 16 through 23. */
#define HPET_ROUTE_CAP		0x00ff0104U

/* Do not ask libevent to hold a timer for an impractically distant match. */
#define HPET_MAX_EVENT_TICKS	(VMD_HPET_FREQUENCY * 86400ULL * 30)

struct hpet_timer {
	struct event	 event;
	uint64_t	 config;
	uint64_t	 comparator;
	uint64_t	 comparator64;
	uint64_t	 fsb_route;
	uint64_t	 period;
	uint8_t		 index;
	uint8_t		 asserted;
	uint8_t		 asserted_route;
};

struct hpet {
	pthread_mutex_t	 mtx;
	struct vm_dev_pipe pipe;
	struct hpet_timer timer[VMD_HPET_NUM_TIMERS];
	struct timespec	 epoch;
	uint64_t	 capability;
	uint64_t	 configuration;
	uint64_t	 isr;
	uint64_t	 counter;
	uint32_t	 vm_id;
};

static struct hpet hpet;

static uint64_t	hpet_counter_locked(void);
static void	hpet_pipe_dispatch(int, short, void *);
static void	hpet_reschedule_all(void);
static void	hpet_timer_fire(int, short, void *);

static uint64_t
hpet_counter_locked(void)
{
	struct timespec now;
	uint64_t sec, nsec;

	if ((hpet.configuration & HPET_CFG_ENABLE) == 0)
		return (hpet.counter);
	if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
		fatal("clock_gettime");
	sec = now.tv_sec - hpet.epoch.tv_sec;
	if (now.tv_nsec < hpet.epoch.tv_nsec) {
		sec--;
		nsec = 1000000000ULL + now.tv_nsec - hpet.epoch.tv_nsec;
	} else
		nsec = now.tv_nsec - hpet.epoch.tv_nsec;

	return (hpet.counter + sec * VMD_HPET_FREQUENCY +
	    nsec * VMD_HPET_FREQUENCY / 1000000000ULL);
}

static uint64_t
hpet_deposit(uint64_t old, uint64_t value, unsigned int shift,
    unsigned int bits)
{
	uint64_t mask;

	if (bits == 64)
		return (value);
	mask = ((1ULL << bits) - 1) << shift;
	return ((old & ~mask) | ((value << shift) & mask));
}

static uint8_t
hpet_timer_route(struct hpet_timer *timer)
{
	return ((timer->config & HPET_TN_ROUTE_MASK) >>
	    HPET_TN_ROUTE_SHIFT);
}

static void
hpet_timer_deassert_locked(struct hpet_timer *timer)
{
	if (!timer->asserted)
		return;
	vcpu_deassert_irq(hpet.vm_id, 0, timer->asserted_route);
	timer->asserted = 0;
}

static void
hpet_latch_timer_locked(struct hpet_timer *timer)
{
	uint64_t current, target;

	current = hpet_counter_locked();
	if (timer->config & HPET_TN_32BIT) {
		target = (current & ~0xffffffffULL) |
		    (uint32_t)timer->comparator;
		if (target < current)
			target += 0x100000000ULL;
	} else
		target = timer->comparator;
	timer->comparator64 = target;
}

static void
hpet_arm_timer_locked(struct hpet_timer *timer)
{
	struct timeval tv;
	uint64_t current, delta, usec;

	evtimer_del(&timer->event);
	if ((hpet.configuration & HPET_CFG_ENABLE) == 0)
		return;

	current = hpet_counter_locked();
	if (timer->comparator64 <= current)
		delta = 1;
	else
		delta = timer->comparator64 - current;
	if (delta > HPET_MAX_EVENT_TICKS)
		return;

	/* Round up to libevent's microsecond resolution. */
	usec = (delta + (VMD_HPET_FREQUENCY / 1000000ULL) - 1) /
	    (VMD_HPET_FREQUENCY / 1000000ULL);
	if (usec == 0)
		usec = 1;
	tv.tv_sec = usec / 1000000ULL;
	tv.tv_usec = usec % 1000000ULL;
	evtimer_add(&timer->event, &tv);
}

static void
hpet_timer_fire(int fd, short event, void *arg)
{
	struct hpet_timer *timer = arg;
	uint64_t current, next;
	uint8_t route;
	int enabled, level;

	(void)fd;
	(void)event;

	mutex_lock(&hpet.mtx);
	if ((hpet.configuration & HPET_CFG_ENABLE) == 0) {
		mutex_unlock(&hpet.mtx);
		return;
	}

	current = hpet_counter_locked();
	if ((timer->config & HPET_TN_PERIODIC) && timer->period != 0) {
		next = timer->comparator64;
		do {
			if (UINT64_MAX - next < timer->period) {
				next = UINT64_MAX;
				break;
			}
			next += timer->period;
		} while (next <= current);
		timer->comparator64 = next;
		timer->comparator = timer->config & HPET_TN_32BIT ?
		    (uint32_t)next : next;
		hpet_arm_timer_locked(timer);
	}

	route = hpet_timer_route(timer);
	enabled = (timer->config & HPET_TN_ENABLE) != 0;
	level = (timer->config & HPET_TN_LEVEL) != 0;
	if (level)
		hpet.isr |= 1ULL << timer->index;
	if (enabled && level && !timer->asserted) {
		timer->asserted = 1;
		timer->asserted_route = route;
	}
	mutex_unlock(&hpet.mtx);

	if (!enabled)
		return;
	vcpu_assert_irq(hpet.vm_id, 0, route);
	if (!level)
		vcpu_deassert_irq(hpet.vm_id, 0, route);
}

static void
hpet_reschedule_all(void)
{
	size_t i;

	mutex_lock(&hpet.mtx);
	for (i = 0; i < VMD_HPET_NUM_TIMERS; i++)
		hpet_arm_timer_locked(&hpet.timer[i]);
	mutex_unlock(&hpet.mtx);
}

static void
hpet_pipe_dispatch(int fd, short event, void *arg)
{
	enum pipe_msg_type msg;

	(void)fd;
	(void)event;
	(void)arg;
	msg = vm_pipe_recv(&hpet.pipe);
	if (msg != HPET_RESCHEDULE)
		fatalx("%s: unexpected pipe message %d", __func__, msg);
	hpet_reschedule_all();
}

void
hpet_init(uint32_t vm_id)
{
	struct hpet_timer *timer;
	size_t i;

	memset(&hpet, 0, sizeof(hpet));
	if (pthread_mutex_init(&hpet.mtx, NULL) != 0)
		fatalx("%s: could not initialize HPET mutex", __func__);
	hpet.vm_id = vm_id;
	hpet.capability = ((uint64_t)VMD_HPET_PERIOD_FS << 32) |
	    VMD_HPET_CAP_ID;
	if (clock_gettime(CLOCK_MONOTONIC, &hpet.epoch) == -1)
		fatal("clock_gettime");

	for (i = 0; i < VMD_HPET_NUM_TIMERS; i++) {
		timer = &hpet.timer[i];
		timer->index = i;
		timer->config = ((uint64_t)HPET_ROUTE_CAP << 32) |
		    HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;
		timer->comparator = UINT64_MAX;
		timer->comparator64 = UINT64_MAX;
		evtimer_set(&timer->event, hpet_timer_fire, timer);
	}

	vm_pipe_init(&hpet.pipe, hpet_pipe_dispatch);
	event_add(&hpet.pipe.read_ev, NULL);
	if (mmio_dev_add(VMD_HPET_BASE, VMD_HPET_BASE + VMD_HPET_SIZE - 1,
	    hpet_mmio) != 0)
		fatalx("%s: cannot register HPET MMIO window", __func__);
}

void
hpet_stop(void)
{
	size_t i;

	for (i = 0; i < VMD_HPET_NUM_TIMERS; i++)
		evtimer_del(&hpet.timer[i].event);
	event_del(&hpet.pipe.read_ev);
}

void
hpet_start(void)
{
	event_add(&hpet.pipe.read_ev, NULL);
	hpet_reschedule_all();
}

int
hpet_mmio(uint32_t vcpu_id, int dir, paddr_t addr, uint8_t size,
    uint64_t *data)
{
	struct hpet_timer *timer;
	uint64_t old, reg, value;
	uint64_t off, timer_off;
	unsigned int bits, shift;
	uint8_t old_route;
	int reschedule = 0;
	size_t i;

	(void)vcpu_id;
	if ((size != 4 && size != 8) || (addr & (size - 1)) != 0)
		return (EINVAL);
	off = addr - VMD_HPET_BASE;
	shift = (off & 4) * 8;
	bits = size * 8;
	if (shift + bits > 64)
		return (EINVAL);
	off &= ~4ULL;

	mutex_lock(&hpet.mtx);
	if (dir == MMIO_DIR_READ) {
		if (off == HPET_CAPABILITIES)
			reg = hpet.capability;
		else if (off == HPET_CONFIGURATION)
			reg = hpet.configuration;
		else if (off == HPET_INTERRUPT_STATUS)
			reg = hpet.isr;
		else if (off == HPET_MAIN_COUNTER)
			reg = hpet_counter_locked();
		else if (off >= HPET_TIMER_BASE) {
			i = (off - HPET_TIMER_BASE) / HPET_TIMER_STRIDE;
			if (i >= VMD_HPET_NUM_TIMERS) {
				reg = 0;
				goto read_done;
			}
			timer = &hpet.timer[i];
			timer_off = (off - HPET_TIMER_BASE) %
			    HPET_TIMER_STRIDE;
			switch (timer_off) {
			case HPET_TIMER_CONFIG:
				reg = timer->config;
				break;
			case HPET_TIMER_COMPARATOR:
				reg = timer->comparator;
				break;
			case HPET_TIMER_FSB_ROUTE:
				reg = timer->fsb_route;
				break;
			default:
				reg = 0;
				break;
			}
		} else
			reg = 0;
read_done:
		*data = reg >> shift;
		mutex_unlock(&hpet.mtx);
		return (0);
	}
	if (dir != MMIO_DIR_WRITE) {
		mutex_unlock(&hpet.mtx);
		return (EINVAL);
	}

	if (off == HPET_CAPABILITIES) {
		/* Read-only. */
	} else if (off == HPET_CONFIGURATION) {
		old = hpet.configuration;
		value = hpet_deposit(old, *data, shift, bits) & HPET_CFG_ENABLE;
		if ((old & HPET_CFG_ENABLE) && !(value & HPET_CFG_ENABLE))
			hpet.counter = hpet_counter_locked();
		else if (!(old & HPET_CFG_ENABLE) &&
		    (value & HPET_CFG_ENABLE) &&
		    clock_gettime(CLOCK_MONOTONIC, &hpet.epoch) == -1)
			fatal("clock_gettime");
		hpet.configuration = value;
		if (!(old & HPET_CFG_ENABLE) && (value & HPET_CFG_ENABLE)) {
			for (i = 0; i < VMD_HPET_NUM_TIMERS; i++)
				hpet_latch_timer_locked(&hpet.timer[i]);
		}
		if (!(value & HPET_CFG_ENABLE)) {
			for (i = 0; i < VMD_HPET_NUM_TIMERS; i++)
				hpet_timer_deassert_locked(&hpet.timer[i]);
		}
		reschedule = 1;
	} else if (off == HPET_INTERRUPT_STATUS) {
		value = (*data << shift) & hpet.isr;
		hpet.isr &= ~value;
		for (i = 0; i < VMD_HPET_NUM_TIMERS; i++) {
			if (value & (1ULL << i))
				hpet_timer_deassert_locked(&hpet.timer[i]);
		}
	} else if (off == HPET_MAIN_COUNTER) {
		old = hpet_counter_locked();
		hpet.counter = hpet_deposit(old, *data, shift, bits);
		if ((hpet.configuration & HPET_CFG_ENABLE) &&
		    clock_gettime(CLOCK_MONOTONIC, &hpet.epoch) == -1)
			fatal("clock_gettime");
		if (hpet.configuration & HPET_CFG_ENABLE) {
			for (i = 0; i < VMD_HPET_NUM_TIMERS; i++)
				hpet_latch_timer_locked(&hpet.timer[i]);
		}
		reschedule = 1;
	} else if (off >= HPET_TIMER_BASE) {
		i = (off - HPET_TIMER_BASE) / HPET_TIMER_STRIDE;
		if (i < VMD_HPET_NUM_TIMERS) {
			timer = &hpet.timer[i];
			timer_off = (off - HPET_TIMER_BASE) %
			    HPET_TIMER_STRIDE;
			switch (timer_off) {
			case HPET_TIMER_CONFIG:
				old = timer->config;
				old_route = hpet_timer_route(timer);
				value = hpet_deposit(old, *data, shift, bits);
				timer->config = (old & ~HPET_TN_WRITE_MASK) |
				    (value & HPET_TN_WRITE_MASK);
				if (timer->config & HPET_TN_32BIT) {
					timer->comparator =
					    (uint32_t)timer->comparator;
					timer->period = (uint32_t)timer->period;
				}
				if (timer->asserted &&
				    (old_route != hpet_timer_route(timer) ||
				    !(timer->config & HPET_TN_LEVEL) ||
				    !(timer->config & HPET_TN_ENABLE))) {
					hpet_timer_deassert_locked(timer);
				}
				if (hpet.configuration & HPET_CFG_ENABLE)
					hpet_latch_timer_locked(timer);
				reschedule = 1;
				break;
			case HPET_TIMER_COMPARATOR:
				if ((timer->config & HPET_TN_32BIT) && shift)
					break;
				if (timer->config & HPET_TN_32BIT) {
					bits = 64;
					shift = 0;
					*data = (uint32_t)*data;
				}
				if (!(timer->config & HPET_TN_PERIODIC) ||
				    (timer->config & HPET_TN_SETVAL))
					timer->comparator = hpet_deposit(
					    timer->comparator, *data, shift, bits);
				if (timer->config & HPET_TN_PERIODIC)
					timer->period = hpet_deposit(timer->period,
					    *data, shift, bits);
				timer->config &= ~HPET_TN_SETVAL;
				if (hpet.configuration & HPET_CFG_ENABLE)
					hpet_latch_timer_locked(timer);
				reschedule = 1;
				break;
			case HPET_TIMER_FSB_ROUTE:
				timer->fsb_route = hpet_deposit(
				    timer->fsb_route, *data, shift, bits);
				break;
			default:
				break;
			}
		}
	}
	mutex_unlock(&hpet.mtx);

	if (reschedule)
		vm_pipe_send(&hpet.pipe, HPET_RESCHEDULE);
	return (0);
}
