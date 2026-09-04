/*	$OpenBSD: vionet.c,v 1.32 2026/08/04 19:12:14 claudio Exp $	*/

/*
 * Copyright (c) 2023 Dave Voutila <dv@openbsd.org>
 * Copyright (c) 2015 Mike Larkin <mlarkin@openbsd.org>
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
#include <sys/ioctl.h>

#include <dev/pci/virtio_pcireg.h>
#include <dev/pv/virtioreg.h>

#include <net/if.h>
#include <net/if_tun.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "atomicio.h"
#include "virtio.h"
#include "vmd.h"

#define VIONET_DEBUG	0
#ifdef DPRINTF
#undef DPRINTF
#endif
#if VIONET_DEBUG
#define DPRINTF		log_debug
#else
#define DPRINTF(x...)	do {} while(0)
#endif	/* VIONET_DEBUG */

#define VIRTIO_NET_CONFIG_MAC		 0 /*  8 bit x 6 byte */
#define VIRTIO_NET_CONFIG_MAX_QUEUES	 8 /* 16 bit */

#define VIRTIO_NET_CTRL_MQ		 4
#define VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET	 0
#define VIRTIO_NET_OK			 0
#define VIRTIO_NET_ERR			 1

#define RXQ	VIONET_RXQ(0)

extern struct vmd_vm *current_vm;

struct packet {
	uint8_t	*buf;
	size_t	 len;
};

struct vionet_tx_worker {
	struct virtio_dev	*dev;
	struct vm_dev_pipe	 pipe;
	struct event_base	*event_base;
	struct iovec		 iov[VIRTIO_QUEUE_SIZE_MAX];
	pthread_t		 thread;
	uint16_t		 vq_idx;
	unsigned int		 pair;
};

struct vionet_tx_offload {
	struct tun_hdr	 tun_hdr;
	int		 tso;
};

static void *rx_run_loop(void *);
static void *tx_run_loop(void *);
static int vionet_rx(struct virtio_dev *, int);
static ssize_t vionet_rx_copy(struct vionet_dev *, int, const struct iovec *,
    int, size_t);
static ssize_t vionet_rx_zerocopy(struct vionet_dev *, int,
    const struct iovec *, int);
static void vionet_rx_event(int, short, void *);
static uint32_t vionet_read(struct virtio_dev *, struct viodev_msg *, int *);
static void vionet_write(struct virtio_dev *, struct viodev_msg *);
static uint32_t vionet_cfg_read(struct virtio_dev *, struct viodev_msg *);
static void vionet_cfg_write(struct virtio_dev *, struct viodev_msg *);

static int vionet_tx(struct vionet_tx_worker *);
static int vionet_tx_offload(struct virtio_dev *,
    const struct virtio_net_hdr *, size_t, struct vionet_tx_offload *);
static int vionet_ctrl(struct virtio_dev *);
static void vionet_handle_ctrl(struct virtio_dev *);
static uint16_t vionet_ctrlq(struct virtio_dev *);
static void vionet_notifyq(struct virtio_dev *, uint16_t);
static uint32_t vionet_dev_read(struct virtio_dev *, struct viodev_msg *);
static void dev_dispatch_vm(int, short, void *);
static void handle_sync_io(int, short, void *);
static void read_pipe_main(int, short, void *);
static void read_pipe_rx(int, short, void *);
static void read_pipe_tx(int, short, void *);
static void vionet_assert_irq(struct virtio_dev *, uint16_t);
static void vionet_deassert_pic_irq(struct virtio_dev *);
static void vionet_stats_report(int, short, void *);

/* Device Globals */
struct event ev_tap;
struct event ev_inject;
struct event_base *ev_base_main;
struct event_base *ev_base_rx;
pthread_t rx_thread;
struct vm_dev_pipe pipe_main;
struct vm_dev_pipe pipe_rx;
struct vionet_tx_worker tx_workers[VIONET_QUEUE_PAIRS];
CTASSERT(VIRTIO_RAISE_IRQ_TX3 - VIRTIO_RAISE_IRQ_TX + 1 ==
    VIONET_QUEUE_PAIRS);
int pipe_inject[2];
#define READ	0
#define WRITE	1
struct iovec iov_rx[VIRTIO_QUEUE_SIZE_MAX];
pthread_rwlock_t lock = NULL;		/* Guards device config state. */
int rx_enabled = 0;	/* 1: we expect to read the tap, 0: wait for notify. */

#define VIONET_STATS_INTERVAL	5

struct vionet_perf_stats {
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t rx_kicks;
	uint64_t tx_kicks;
	uint64_t rx_irqs;
	uint64_t tx_irqs;
	uint64_t tx_csum_packets;
	uint64_t tx_tso_packets;
	uint64_t ctrl_kicks;
	uint64_t ctrl_irqs;
	uint64_t config_irqs;
	struct {
		uint64_t rx_packets;
		uint64_t rx_bytes;
		uint64_t tx_packets;
		uint64_t tx_bytes;
		uint64_t rx_kicks;
		uint64_t tx_kicks;
		uint64_t rx_irqs;
		uint64_t tx_irqs;
	} queue[VIONET_QUEUE_PAIRS];
};

static struct vionet_perf_stats vionet_stats;
static struct vionet_perf_stats vionet_stats_prev;
static struct event vionet_stats_event;

static inline void
vionet_stats_add(uint64_t *counter, uint64_t value)
{
	if (log_getverbose() == 1)
		__atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static uint64_t
vionet_stats_delta(uint64_t current, uint64_t *previous)
{
	uint64_t delta = current - *previous;

	*previous = current;
	return (delta);
}

static void
vionet_stats_report(int fd, short event, void *arg)
{
	struct vionet_perf_stats current, delta;
	struct timeval tv = { VIONET_STATS_INTERVAL, 0 };
	uint64_t rx_per_irq = 0, tx_per_irq = 0;
	unsigned int i;

	(void)fd;
	(void)event;
	(void)arg;
#define VIONET_STATS_DELTA(_field) do { \
	current._field = __atomic_load_n(&vionet_stats._field, \
	    __ATOMIC_RELAXED); \
	delta._field = vionet_stats_delta(current._field, \
	    &vionet_stats_prev._field); \
} while (0)
	VIONET_STATS_DELTA(rx_packets);
	VIONET_STATS_DELTA(rx_bytes);
	VIONET_STATS_DELTA(tx_packets);
	VIONET_STATS_DELTA(tx_bytes);
	VIONET_STATS_DELTA(rx_kicks);
	VIONET_STATS_DELTA(tx_kicks);
	VIONET_STATS_DELTA(rx_irqs);
	VIONET_STATS_DELTA(tx_irqs);
	VIONET_STATS_DELTA(tx_csum_packets);
	VIONET_STATS_DELTA(tx_tso_packets);
	VIONET_STATS_DELTA(ctrl_kicks);
	VIONET_STATS_DELTA(ctrl_irqs);
	VIONET_STATS_DELTA(config_irqs);
#undef VIONET_STATS_DELTA
	for (i = 0; i < VIONET_QUEUE_PAIRS; i++) {
#define VIONET_QUEUE_STATS_DELTA(_field) do { \
	current.queue[i]._field = __atomic_load_n( \
	    &vionet_stats.queue[i]._field, __ATOMIC_RELAXED); \
	delta.queue[i]._field = vionet_stats_delta( \
	    current.queue[i]._field, &vionet_stats_prev.queue[i]._field); \
} while (0)
		VIONET_QUEUE_STATS_DELTA(rx_packets);
		VIONET_QUEUE_STATS_DELTA(rx_bytes);
		VIONET_QUEUE_STATS_DELTA(tx_packets);
		VIONET_QUEUE_STATS_DELTA(tx_bytes);
		VIONET_QUEUE_STATS_DELTA(rx_kicks);
		VIONET_QUEUE_STATS_DELTA(tx_kicks);
		VIONET_QUEUE_STATS_DELTA(rx_irqs);
		VIONET_QUEUE_STATS_DELTA(tx_irqs);
#undef VIONET_QUEUE_STATS_DELTA
	}
	if (delta.rx_irqs != 0)
		rx_per_irq = delta.rx_packets / delta.rx_irqs;
	if (delta.tx_irqs != 0)
		tx_per_irq = delta.tx_packets / delta.tx_irqs;
	if (log_getverbose() == 1) {
		log_info("stats %ds net-dev: rx-packets=%llu rx-bytes=%llu "
		    "rx-kicks=%llu rx-irqs=%llu rx-packets/irq=%llu "
		    "tx-packets=%llu tx-bytes=%llu tx-kicks=%llu "
		    "tx-irqs=%llu tx-packets/irq=%llu tx-csum=%llu "
		    "tx-tso=%llu kick-ctrl=%llu "
		    "irq-ctrl=%llu config-irqs=%llu",
		    VIONET_STATS_INTERVAL,
		    (unsigned long long)delta.rx_packets,
		    (unsigned long long)delta.rx_bytes,
		    (unsigned long long)delta.rx_kicks,
		    (unsigned long long)delta.rx_irqs,
		    (unsigned long long)rx_per_irq,
		    (unsigned long long)delta.tx_packets,
		    (unsigned long long)delta.tx_bytes,
		    (unsigned long long)delta.tx_kicks,
		    (unsigned long long)delta.tx_irqs,
		    (unsigned long long)tx_per_irq,
		    (unsigned long long)delta.tx_csum_packets,
		    (unsigned long long)delta.tx_tso_packets,
		    (unsigned long long)delta.ctrl_kicks,
		    (unsigned long long)delta.ctrl_irqs,
		    (unsigned long long)delta.config_irqs);
		for (i = 0; i < VIONET_QUEUE_PAIRS; i++) {
			log_info("stats %ds net-dev-q%u: rx-packets=%llu "
			    "rx-bytes=%llu rx-kicks=%llu rx-irqs=%llu "
			    "tx-packets=%llu tx-bytes=%llu tx-kicks=%llu "
			    "tx-irqs=%llu", VIONET_STATS_INTERVAL, i,
			    (unsigned long long)delta.queue[i].rx_packets,
			    (unsigned long long)delta.queue[i].rx_bytes,
			    (unsigned long long)delta.queue[i].rx_kicks,
			    (unsigned long long)delta.queue[i].rx_irqs,
			    (unsigned long long)delta.queue[i].tx_packets,
			    (unsigned long long)delta.queue[i].tx_bytes,
			    (unsigned long long)delta.queue[i].tx_kicks,
			    (unsigned long long)delta.queue[i].tx_irqs);
		}
	}
	if (evtimer_add(&vionet_stats_event, &tv) == -1)
		log_warnx("%s: could not reschedule stats timer", __func__);
}

__dead void
vionet_main(int fd, int fd_vmm)
{
	struct virtio_dev	 dev;
	struct vionet_dev	*vionet = NULL;
	struct viodev_msg 	 msg;
	struct vmd_vm	 	 vm;
	char			 thread_name[16];
	ssize_t			 sz;
	unsigned int		 i;
	int			 ret;

	/*
	 * stdio - needed for read/write to disk fds and channels to the vm.
	 * vmm + proc - needed to create shared vm mappings.
	 */
	/* DSDT DEBUG: pledge disabled
	if (pledge("stdio vmm proc", NULL) == -1)
		fatal("pledge");
	*/

	/* Initialize iovec arrays. */
	memset(iov_rx, 0, sizeof(iov_rx));
	memset(tx_workers, 0, sizeof(tx_workers));

	/* Receive our vionet_dev, mostly preconfigured. */
	sz = atomicio(read, fd, &dev, sizeof(dev));
	if (sz != sizeof(dev)) {
		ret = errno;
		log_warn("failed to receive vionet");
		goto fail;
	}
	if (dev.dev_type != VMD_DEVTYPE_NET) {
		ret = EINVAL;
		log_warn("received invalid device type");
		goto fail;
	}
	dev.sync_fd = fd;
	vionet = &dev.vionet;

	log_debug("%s: got vionet dev. tap fd = %d, syncfd = %d, asyncfd = %d"
	    ", vmm fd = %d", __func__, vionet->data_fd, dev.sync_fd,
	    dev.async_fd, fd_vmm);

	/*
	 * Enable tap(4)'s per-packet offload header.  We do not advertise
	 * receive offloads to the host yet, so headers read from tap contain
	 * no checksum or segmentation requests.  Headers written to tap may
	 * still request the transmit offloads negotiated with the guest.
	 */
	{
		struct tun_capabilities cap = { 0 };

		if (ioctl(vionet->data_fd, TUNSCAP, &cap) == -1)
			fatal("%s: TUNSCAP", __func__);
	}

	/* Receive our vm information from the vm process. */
	memset(&vm, 0, sizeof(vm));
	sz = atomicio(read, dev.sync_fd, &vm, sizeof(vm));
	if (sz != sizeof(vm)) {
		ret = EIO;
		log_warnx("failed to receive vm details");
		goto fail;
	}
	current_vm = &vm;
	setproctitle("%s/vionet%d", vm.vm_params.vmc_name, vionet->idx);
	log_procinit("vm/%s/vionet%d", vm.vm_params.vmc_name, vionet->idx);

	/* Now that we have our vm information, we can remap memory. */
	ret = remap_guest_mem(&vm, fd_vmm);
	if (ret) {
		fatal("%s: failed to remap", __func__);
		goto fail;
	}

	/*
	 * We no longer need /dev/vmm access.
	 */
	close_fd(fd_vmm);
	if (pledge("stdio", NULL) == -1)
		fatal("pledge2");

	/* Initialize our packet injection pipe. */
	if (pipe2(pipe_inject, O_NONBLOCK) == -1) {
		log_warn("%s: injection pipe", __func__);
		goto fail;
	}

	/* Initialize inter-thread communication channels. */
	vm_pipe_init2(&pipe_main, read_pipe_main, &dev);
	vm_pipe_init2(&pipe_rx, read_pipe_rx, &dev);
	for (i = 0; i < VIONET_QUEUE_PAIRS; i++) {
		tx_workers[i].dev = &dev;
		tx_workers[i].pair = i;
		tx_workers[i].vq_idx = VIONET_TXQ(i);
		vm_pipe_init2(&tx_workers[i].pipe, read_pipe_tx,
		    &tx_workers[i]);
	}

	/* Initialize the rwlock before any worker can access device state. */
	ret = pthread_rwlock_init(&lock, NULL);
	if (ret) {
		errno = ret;
		log_warn("%s: failed to initialize rwlock", __func__);
		goto fail;
	}

	/* Initialize the RX thread and one TX worker per queue pair. */
	ret = pthread_create(&rx_thread, NULL, rx_run_loop, &dev);
	if (ret) {
		errno = ret;
		log_warn("%s: failed to initialize rx thread", __func__);
		goto fail;
	}
	pthread_set_name_np(rx_thread, "rx");
	for (i = 0; i < VIONET_QUEUE_PAIRS; i++) {
		ret = pthread_create(&tx_workers[i].thread, NULL, tx_run_loop,
		    &tx_workers[i]);
		if (ret) {
			errno = ret;
			log_warn("%s: failed to initialize tx%u thread",
			    __func__, i);
			goto fail;
		}
		snprintf(thread_name, sizeof(thread_name), "tx%u", i);
		pthread_set_name_np(tx_workers[i].thread, thread_name);
	}

	/* Initialize libevent so we can start wiring event handlers. */
	ev_base_main = event_base_new();
	evtimer_set(&vionet_stats_event, vionet_stats_report, NULL);
	event_base_set(ev_base_main, &vionet_stats_event);
	if (evtimer_add(&vionet_stats_event,
	    (&(struct timeval) { VIONET_STATS_INTERVAL, 0 })) == -1)
		log_warnx("%s: could not start stats timer", __func__);

	/* Add our handler for receiving messages from the RX/TX threads. */
	event_base_set(ev_base_main, &pipe_main.read_ev);
	event_add(&pipe_main.read_ev, NULL);

	/* Wire up an async imsg channel. */
	log_debug("%s: wiring in async vm event handler (fd=%d)", __func__,
		dev.async_fd);
	if (vm_device_pipe(&dev, dev_dispatch_vm, ev_base_main)) {
		ret = EIO;
		log_warnx("vm_device_pipe");
		goto fail;
	}

	/* Configure our sync channel event handler. */
	log_debug("%s: wiring in sync channel handler (fd=%d)", __func__,
		dev.sync_fd);
	if (imsgbuf_init(&dev.sync_iev.ibuf, dev.sync_fd) == -1) {
		log_warnx("imsgbuf_init");
		goto fail;
	}
	imsgbuf_allow_fdpass(&dev.sync_iev.ibuf);
	dev.sync_iev.handler = handle_sync_io;
	dev.sync_iev.data = &dev;
	dev.sync_iev.events = EV_READ;
	imsg_event_add2(&dev.sync_iev, ev_base_main);

	/* Send a ready message over the sync channel. */
	log_debug("%s: telling vm %s device is ready", __func__,
	    vm.vm_params.vmc_name);
	memset(&msg, 0, sizeof(msg));
	msg.type = VIODEV_MSG_READY;
	imsg_compose_event2(&dev.sync_iev, IMSG_DEVOP_MSG, 0, 0, -1, &msg,
	    sizeof(msg), ev_base_main);

	/* Send a ready message over the async channel. */
	log_debug("%s: sending async ready message", __func__);
	ret = imsg_compose_event2(&dev.async_iev, IMSG_DEVOP_MSG, 0, 0, -1,
	    &msg, sizeof(msg), ev_base_main);
	if (ret == -1) {
		log_warnx("%s: failed to send async ready message!", __func__);
		goto fail;
	}

	/* Engage the event loop! */
	ret = event_base_dispatch(ev_base_main);
	event_base_free(ev_base_main);

	/* Try stopping the rx and tx threads cleanly by messaging them. */
	vm_pipe_send(&pipe_rx, VIRTIO_THREAD_STOP);
	for (i = 0; i < VIONET_QUEUE_PAIRS; i++)
		vm_pipe_send(&tx_workers[i].pipe, VIRTIO_THREAD_STOP);

	/* Wait for threads to stop. */
	pthread_join(rx_thread, NULL);
	for (i = 0; i < VIONET_QUEUE_PAIRS; i++)
		pthread_join(tx_workers[i].thread, NULL);
	pthread_rwlock_destroy(&lock);

	/* Cleanup */
	if (ret == 0) {
		close_fd(dev.sync_fd);
		close_fd(dev.async_fd);
		close_fd(vionet->data_fd);
		close_fd(pipe_main.read);
		close_fd(pipe_main.write);
		close_fd(pipe_rx.write);
		for (i = 0; i < VIONET_QUEUE_PAIRS; i++)
			close_fd(tx_workers[i].pipe.write);
		close_fd(pipe_inject[READ]);
		close_fd(pipe_inject[WRITE]);
		_exit(ret);
		/* NOTREACHED */
	}
fail:
	/* Try firing off a message to the vm saying we're dying. */
	memset(&msg, 0, sizeof(msg));
	msg.type = VIODEV_MSG_ERROR;
	msg.data = ret;
	imsg_compose(&dev.sync_iev.ibuf, IMSG_DEVOP_MSG, 0, 0, -1, &msg,
	    sizeof(msg));
	imsgbuf_flush(&dev.sync_iev.ibuf);

	close_fd(dev.sync_fd);
	close_fd(dev.async_fd);
	close_fd(pipe_inject[READ]);
	close_fd(pipe_inject[WRITE]);
	if (vionet != NULL)
		close_fd(vionet->data_fd);
	if (lock != NULL)
		pthread_rwlock_destroy(&lock);
	_exit(ret);
}

/*
 * vionet_rx
 *
 * Pull packet from the provided fd and fill the receive-side virtqueue. We
 * selectively use zero-copy approaches when possible.
 *
 * Returns 1 if guest notification is needed. Otherwise, returns -1 on failure
 * or 0 if no notification is needed.
 */
static int
vionet_rx(struct virtio_dev *dev, int fd)
{
	uint16_t idx, hdr_idx;
	char *vr = NULL;
	size_t chain_len = 0, iov_cnt;
	struct vionet_dev *vionet = &dev->vionet;
	struct vring_desc *desc, *table;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_net_hdr *hdr = NULL;
	struct virtio_vq_info *vq_info;
	struct iovec *iov;
	int notify = 0, stats_enabled;
	ssize_t sz;
	uint64_t stats_bytes = 0, stats_packets = 0;
	uint8_t status = 0;

	status = dev->status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	stats_enabled = log_getverbose() == 1;
	if (status != VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) {
		log_warnx("%s: driver not ready", __func__);
		return (0);
	}

	vq_info = &dev->vq[RXQ];
	idx = vq_info->last_avail;
	vr = vq_info->q_hva;
	if (vr == NULL || vq_info->q_avail_hva == NULL ||
	    vq_info->q_used_hva == NULL)
		fatalx("%s: vr == NULL", __func__);

	/* Locate the independently mapped split virtqueue areas. */
	table = (struct vring_desc *)(vr);
	avail = vq_info->q_avail_hva;
	used = vq_info->q_used_hva;
	used->flags |= VRING_USED_F_NO_NOTIFY;

	while (idx != avail->idx) {
		hdr_idx = avail->ring[idx & vq_info->mask];
		desc = &table[hdr_idx & vq_info->mask];
		if (!DESC_WRITABLE(desc)) {
			log_warnx("%s: invalid descriptor state", __func__);
			goto reset;
		}

		iov = &iov_rx[0];
		iov_cnt = 1;

		/*
		 * First descriptor should be at least as large as the
		 * virtio_net_hdr. It's not technically required, but in
		 * legacy devices it should be safe to assume.
		 */
		iov->iov_len = desc->len;
		if (iov->iov_len < sizeof(struct virtio_net_hdr)) {
			log_warnx("%s: invalid descriptor length", __func__);
			goto reset;
		}

		/*
		 * Insert the virtio_net_hdr and adjust len/base. We do the
		 * pointer math here before it's a void*.
		 */
		iov->iov_base = hvaddr_mem(desc->addr, iov->iov_len);
		if (iov->iov_base == NULL)
			goto reset;
		hdr = iov->iov_base;
		memset(hdr, 0, sizeof(struct virtio_net_hdr));

		/* Tweak the iovec to account for the virtio_net_hdr. */
		iov->iov_len -= sizeof(struct virtio_net_hdr);
		iov->iov_base = hvaddr_mem(desc->addr +
		    sizeof(struct virtio_net_hdr), iov->iov_len);
		if (iov->iov_base == NULL)
			goto reset;
		chain_len = iov->iov_len;

		/*
		 * Walk the remaining chain and collect remaining addresses
		 * and lengths.
		 */
		while (desc->flags & VRING_DESC_F_NEXT) {
			desc = &table[desc->next & vq_info->mask];
			if (!DESC_WRITABLE(desc)) {
				log_warnx("%s: invalid descriptor state",
				    __func__);
				goto reset;
			}

			/* Collect our IO information. Translate gpa's. */
			iov = &iov_rx[iov_cnt];
			iov->iov_len = desc->len;
			iov->iov_base = hvaddr_mem(desc->addr, iov->iov_len);
			if (iov->iov_base == NULL)
				goto reset;
			chain_len += iov->iov_len;

			/* Guard against infinitely looping chains. */
			if (++iov_cnt >= nitems(iov_rx)) {
				log_warnx("%s: infinite chain detected",
				    __func__);
				goto reset;
			}
		}

		/* Make sure the driver gave us the bare minimum buffers. */
		if (chain_len < VIONET_MIN_TXLEN) {
			log_warnx("%s: insufficient buffers provided",
			    __func__);
			goto reset;
		}

		hdr->num_buffers = iov_cnt;

		/*
		 * If we're enforcing hardware address or handling an injected
		 * packet, we need to use a copy-based approach.
		 */
		if (vionet->lockedmac || fd != vionet->data_fd)
			sz = vionet_rx_copy(vionet, fd, iov_rx, iov_cnt,
			    chain_len);
		else
			sz = vionet_rx_zerocopy(vionet, fd, iov_rx, iov_cnt);
		if (sz == -1)
			goto reset;
		if (sz == 0)	/* No packets, so bail out for now. */
			break;

		/*
		 * Account for the prefixed header since it wasn't included
		 * in the copy or zerocopy operations.
		 */
		sz += sizeof(struct virtio_net_hdr);
		stats_packets++;
		stats_bytes += sz - sizeof(struct virtio_net_hdr);

		/* Mark our buffers as used. */
		used->ring[used->idx & vq_info->mask].id = hdr_idx;
		used->ring[used->idx & vq_info->mask].len = sz;
		__sync_synchronize();
		used->idx++;
		idx++;
	}

	if (idx != vq_info->last_avail &&
	    !(avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
		notify = 1;
	}

	vq_info->last_avail = idx;
	if (stats_enabled) {
		vionet_stats_add(&vionet_stats.rx_packets, stats_packets);
		vionet_stats_add(&vionet_stats.rx_bytes, stats_bytes);
		vionet_stats_add(&vionet_stats.queue[0].rx_packets,
		    stats_packets);
		vionet_stats_add(&vionet_stats.queue[0].rx_bytes, stats_bytes);
	}
	return (notify);
reset:
	if (stats_enabled) {
		vionet_stats_add(&vionet_stats.rx_packets, stats_packets);
		vionet_stats_add(&vionet_stats.rx_bytes, stats_bytes);
		vionet_stats_add(&vionet_stats.queue[0].rx_packets,
		    stats_packets);
		vionet_stats_add(&vionet_stats.queue[0].rx_bytes, stats_bytes);
	}
	return (-1);
}

/*
 * vionet_rx_copy
 *
 * Read a packet off the provided file descriptor, validating packet
 * characteristics, and copy into the provided buffers in the iovec array.
 *
 * It's assumed that the provided iovec array contains validated host virtual
 * address translations and not guest physical addreses.
 *
 * Returns number of bytes copied on success, 0 if packet is dropped, and
 * -1 on an error.
 */
ssize_t
vionet_rx_copy(struct vionet_dev *dev, int fd, const struct iovec *iov,
    int iov_cnt, size_t chain_len)
{
	static uint8_t		 buf[VIONET_HARD_MTU];
	struct packet		*pkt = NULL;
	struct ether_header	*eh = NULL;
	struct tun_hdr		 tun_hdr;
	struct iovec		 tap_iov[2];
	uint8_t			*payload = buf;
	size_t			 i, chunk, nbytes, copied = 0;
	ssize_t			 sz;

	if (fd == dev->data_fd) {
		/* Try to right-size the packet portion of the tap(4) read. */
		nbytes = MIN(chain_len, VIONET_HARD_MTU);
		tap_iov[0].iov_base = &tun_hdr;
		tap_iov[0].iov_len = sizeof(tun_hdr);
		tap_iov[1].iov_base = buf;
		tap_iov[1].iov_len = nbytes;
		sz = readv(fd, tap_iov, nitems(tap_iov));
	} else if (fd == pipe_inject[READ]) {
		nbytes = sizeof(struct packet);
		sz = read(fd, buf, nbytes);
	} else {
		log_warnx("%s: invalid fd: %d", __func__, fd);
		return (-1);
	}

	/*
	 * Try to pull a packet. The fd should be non-blocking and we don't
	 * care if we under-read (i.e. sz != nbytes) as we may not have a
	 * packet large enough to fill the buffer.
	 */
	if (sz == -1) {
		if (errno != EAGAIN) {
			log_warn("%s: error reading packet", __func__);
			return (-1);
		}
		return (0);
	} else if (fd == dev->data_fd &&
	    sz < (ssize_t)(sizeof(tun_hdr) + VIONET_MIN_TXLEN)) {
		/* A tap(4) read must contain its offload and ethernet headers. */
		log_warnx("%s: invalid packet size", __func__);
		return (0);
	} else if (fd == pipe_inject[READ] && sz != sizeof(struct packet)) {
		log_warnx("%s: invalid injected packet object (sz=%ld)",
		    __func__, sz);
		return (0);
	}
	if (fd == dev->data_fd) {
		if (tun_hdr.th_flags & ~TUN_H_PRIO_MASK) {
			log_warnx("%s: unexpected receive offload flags 0x%x",
			    __func__, tun_hdr.th_flags);
			return (0);
		}
		sz -= sizeof(tun_hdr);
	}

	/* Decompose an injected packet, if that's what we're working with. */
	if (fd == pipe_inject[READ]) {
		pkt = (struct packet *)buf;
		if (pkt->buf == NULL) {
			log_warnx("%s: invalid injected packet, no buffer",
			    __func__);
			return (0);
		}
		if (sz < VIONET_MIN_TXLEN || sz > VIONET_MAX_TXLEN) {
			log_warnx("%s: invalid injected packet size", __func__);
			goto drop;
		}
		payload = pkt->buf;
		sz = (ssize_t)pkt->len;
	}

	/* Validate the ethernet header, if required. */
	if (dev->lockedmac) {
		eh = (struct ether_header *)(payload);
		if (!ETHER_IS_MULTICAST(eh->ether_dhost) &&
		    memcmp(eh->ether_dhost, dev->mac,
		    sizeof(eh->ether_dhost)) != 0)
			goto drop;
	}

	/* Truncate one last time to the chain length, if shorter. */
	sz = MIN(chain_len, (size_t)sz);

	/*
	 * Copy the packet into the provided buffers. We can use memcpy(3)
	 * here as the gpa was validated and translated to an hva previously.
	 */
	for (i = 0; (int)i < iov_cnt && (size_t)sz > copied; i++) {
		chunk = MIN(iov[i].iov_len, (size_t)(sz - copied));
		memcpy(iov[i].iov_base, payload + copied, chunk);
		copied += chunk;
	}

drop:
	/* Free any injected packet buffer. */
	if (pkt != NULL)
		free(pkt->buf);

	return (copied);
}

/*
 * vionet_rx_zerocopy
 *
 * Perform a vectorized read from the given fd into the guest physical memory
 * pointed to by iovecs.
 *
 * Returns number of bytes read on success, -1 on error, or 0 if EAGAIN was
 * returned by readv.
 *
 */
static ssize_t
vionet_rx_zerocopy(struct vionet_dev *dev, int fd, const struct iovec *iov,
    int iov_cnt)
{
	struct tun_hdr	 tun_hdr;
	struct iovec	 tap_iov[VIRTIO_QUEUE_SIZE_MAX + 1];
	ssize_t		 sz;

	if (dev->lockedmac) {
		log_warnx("%s: zerocopy not available for locked lladdr",
		    __func__);
		return (-1);
	}

	if (iov_cnt >= VIRTIO_QUEUE_SIZE_MAX)
		return (-1);
	tap_iov[0].iov_base = &tun_hdr;
	tap_iov[0].iov_len = sizeof(tun_hdr);
	memcpy(&tap_iov[1], iov, iov_cnt * sizeof(*iov));

	sz = readv(fd, tap_iov, iov_cnt + 1);
	if (sz == -1 && errno == EAGAIN)
		return (0);
	if (sz == -1)
		return (-1);
	if (sz < (ssize_t)(sizeof(tun_hdr) + VIONET_MIN_TXLEN)) {
		log_warnx("%s: invalid packet size", __func__);
		return (0);
	}
	if (tun_hdr.th_flags & ~TUN_H_PRIO_MASK) {
		log_warnx("%s: unexpected receive offload flags 0x%x", __func__,
		    tun_hdr.th_flags);
		return (0);
	}
	return (sz - sizeof(tun_hdr));
}


/*
 * vionet_rx_event
 *
 * Called when new data can be received on the tap fd of a vionet device.
 */
static void
vionet_rx_event(int fd, short event, void *arg)
{
	struct virtio_dev	*dev = (struct virtio_dev *)arg;
	struct vionet_dev	*vionet = (struct vionet_dev *)&dev->vionet;
	int			 raise_irq = 0, ret = 0;
	uint32_t		 generation = 0;

	if (!(event & EV_READ))
		fatalx("%s: invalid event type", __func__);

	pthread_rwlock_rdlock(&lock);
	generation = vionet->reset_generation;
	ret = vionet_rx(dev, fd);
	pthread_rwlock_unlock(&lock);

	if (ret == 0) {
		/* Nothing to do. */
		return;
	}

	pthread_rwlock_wrlock(&lock);
	if (generation == vionet->reset_generation) {
		if (ret == 1) {
			/* Notify the driver. */
			dev->isr |= 1;
		} else {
			/* Need a reset. Something went wrong. */
			log_warnx("%s: requesting device reset", __func__);
			dev->status |= DEVICE_NEEDS_RESET;
			dev->isr |= VIRTIO_CONFIG_ISR_CONFIG_CHANGE;
		}
		raise_irq = 1;
	}
	pthread_rwlock_unlock(&lock);

	if (raise_irq)
		vm_pipe_send(&pipe_main, ret == 1 ? VIRTIO_RAISE_IRQ_RX :
		    VIRTIO_RAISE_IRQ_CONFIG);
}

static void
vionet_notifyq(struct virtio_dev *dev, uint16_t vq_idx)
{
	struct vionet_dev *vionet = &dev->vionet;
	unsigned int pair;
	uint16_t ctrlq = vionet_ctrlq(dev);

	if (vq_idx >= dev->num_queues) {
		log_debug("%s: invalid queue ID %u", __func__, vq_idx);
		return;
	}

	if (vq_idx == RXQ) {
		vionet_stats_add(&vionet_stats.rx_kicks, 1);
		vionet_stats_add(&vionet_stats.queue[0].rx_kicks, 1);
		rx_enabled = 1;
		vm_pipe_send(&pipe_rx, VIRTIO_NOTIFY);
		return;
	}
	if (vq_idx == ctrlq) {
		vionet_stats_add(&vionet_stats.ctrl_kicks, 1);
		vionet_handle_ctrl(dev);
		return;
	}
	if (vq_idx < VIONET_CTRLQ_MQ && (vq_idx & 1) == 0) {
		/* RX remains deliberately pinned to queue pair zero. */
		pair = vq_idx / 2;
		vionet_stats_add(&vionet_stats.rx_kicks, 1);
		vionet_stats_add(&vionet_stats.queue[pair].rx_kicks, 1);
		return;
	}
	if (vq_idx < VIONET_CTRLQ_MQ && (vq_idx & 1) != 0) {
		pair = vq_idx / 2;
		vionet_stats_add(&vionet_stats.tx_kicks, 1);
		vionet_stats_add(&vionet_stats.queue[pair].tx_kicks, 1);
		if (pair < vionet->active_queue_pairs)
			vm_pipe_send(&tx_workers[pair].pipe, VIRTIO_NOTIFY);
		return;
	}
}

static uint16_t
vionet_ctrlq(struct virtio_dev *dev)
{
	return (VIONET_CTRLQ(dev->driver_feature));
}

/*
 * Process the VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET command.  Other control
 * classes are not advertised and receive the standard VIRTIO_NET_ERR status.
 */
static int
vionet_ctrl(struct virtio_dev *dev)
{
	struct vionet_dev *vionet = &dev->vionet;
	struct virtio_vq_info *vq_info = &dev->vq[vionet_ctrlq(dev)];
	struct vring_desc *desc, *table;
	struct vring_avail *avail;
	struct vring_used *used;
	void *data;
	uint8_t *cmd, *status;
	uint16_t idx, hdr_idx, pairs;
	char *vr;
	int notify = 0;

	if ((dev->status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) == 0)
		return (0);
	vr = vq_info->q_hva;
	if (vr == NULL || vq_info->q_avail_hva == NULL ||
	    vq_info->q_used_hva == NULL)
		return (-1);

	table = (struct vring_desc *)vr;
	avail = vq_info->q_avail_hva;
	used = vq_info->q_used_hva;
	idx = vq_info->last_avail;

	while (idx != avail->idx) {
		hdr_idx = avail->ring[idx & vq_info->mask];
		desc = &table[hdr_idx & vq_info->mask];
		if (DESC_WRITABLE(desc) || desc->len < 2 ||
		    (desc->flags & VRING_DESC_F_NEXT) == 0)
			return (-1);
		cmd = hvaddr_mem(desc->addr, 2);
		if (cmd == NULL)
			return (-1);

		desc = &table[desc->next & vq_info->mask];
		if (DESC_WRITABLE(desc) || desc->len < sizeof(pairs) ||
		    (desc->flags & VRING_DESC_F_NEXT) == 0)
			return (-1);
		data = hvaddr_mem(desc->addr, sizeof(pairs));
		if (data == NULL)
			return (-1);
		memcpy(&pairs, data, sizeof(pairs));

		desc = &table[desc->next & vq_info->mask];
		if (!DESC_WRITABLE(desc) || desc->len < 1)
			return (-1);
		status = hvaddr_mem(desc->addr, 1);
		if (status == NULL)
			return (-1);
		*status = VIRTIO_NET_ERR;
		if (cmd[0] == VIRTIO_NET_CTRL_MQ &&
		    cmd[1] == VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET &&
		    pairs >= 1 && pairs <= VIONET_QUEUE_PAIRS) {
			vionet->active_queue_pairs = pairs;
			*status = VIRTIO_NET_OK;
		}

		used->ring[used->idx & vq_info->mask].id = hdr_idx;
		used->ring[used->idx & vq_info->mask].len = 1;
		__sync_synchronize();
		used->idx++;
		idx++;
	}

	if (idx != vq_info->last_avail &&
	    !(avail->flags & VRING_AVAIL_F_NO_INTERRUPT))
		notify = 1;
	vq_info->last_avail = idx;

	return (notify);
}

static void
vionet_handle_ctrl(struct virtio_dev *dev)
{
	struct vionet_dev *vionet = &dev->vionet;
	uint32_t generation;
	int ret;

	pthread_rwlock_wrlock(&lock);
	generation = vionet->reset_generation;
	ret = vionet_ctrl(dev);
	if (ret != 0 && generation == vionet->reset_generation) {
		if (ret == 1)
			dev->isr |= 1;
		else {
			log_warnx("%s: requesting device reset", __func__);
			dev->status |= DEVICE_NEEDS_RESET;
			dev->isr |= VIRTIO_CONFIG_ISR_CONFIG_CHANGE;
		}
	}
	pthread_rwlock_unlock(&lock);

	if (ret == 1)
		vionet_assert_irq(dev, vionet_ctrlq(dev));
	else if (ret == -1)
		vionet_assert_irq(dev, VIODEV_QUEUE_CONFIG);
}

/*
 * Translate the virtio checksum and segmentation metadata supplied by the
 * guest into the native tap(4) offload header.  tap(4) does not expose the
 * arbitrary checksum offset operation from the virtio protocol, but the TCP
 * and UDP offsets used by the network drivers are directly representable.
 */
static int
vionet_tx_offload(struct virtio_dev *dev, const struct virtio_net_hdr *hdr,
    size_t packet_len, struct vionet_tx_offload *offload)
{
	uint8_t gso_type;

	memset(offload, 0, sizeof(*offload));
	if (hdr->flags & ~VIRTIO_NET_HDR_F_NEEDS_CSUM)
		return (-1);

	if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
		if ((dev->driver_feature & VIRTIO_NET_F_CSUM) == 0)
			return (-1);
		if (hdr->csum_start > packet_len ||
		    hdr->csum_offset > packet_len - hdr->csum_start ||
		    sizeof(uint16_t) >
		    packet_len - hdr->csum_start - hdr->csum_offset)
			return (-1);

		switch (hdr->csum_offset) {
		case offsetof(struct tcphdr, th_sum):
			offload->tun_hdr.th_flags |= TUN_H_TCP_CSUM;
			break;
		case offsetof(struct udphdr, uh_sum):
			offload->tun_hdr.th_flags |= TUN_H_UDP_CSUM;
			break;
		default:
			return (-1);
		}
	}

	gso_type = hdr->gso_type;
	if (gso_type & VIRTIO_NET_HDR_GSO_ECN)
		return (-1);
	switch (gso_type) {
	case VIRTIO_NET_HDR_GSO_NONE:
		break;
	case VIRTIO_NET_HDR_GSO_TCPV4:
		if ((dev->driver_feature & VIRTIO_NET_F_HOST_TSO4) == 0)
			return (-1);
		goto tso;
	case VIRTIO_NET_HDR_GSO_TCPV6:
		if ((dev->driver_feature & VIRTIO_NET_F_HOST_TSO6) == 0)
			return (-1);
tso:
		if ((hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) == 0 ||
		    (offload->tun_hdr.th_flags & TUN_H_TCP_CSUM) == 0 ||
		    hdr->gso_size == 0 || hdr->hdr_len == 0 ||
		    hdr->hdr_len > packet_len)
			return (-1);
		offload->tun_hdr.th_flags |= TUN_H_TCP_MSS;
		offload->tun_hdr.th_mss = hdr->gso_size;
		offload->tso = 1;
		break;
	default:
		return (-1);
	}

	return (0);
}

static int
vionet_tx(struct vionet_tx_worker *worker)
{
	struct virtio_dev *dev = worker->dev;
	uint16_t idx, hdr_idx;
	size_t chain_len, iov_cnt;
	ssize_t dhcpsz = 0, sz;
	int notify = 0, stats_enabled;
	char *vr = NULL, *dhcppkt = NULL;
	struct vionet_dev *vionet = &dev->vionet;
	struct vring_desc *desc, *table;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_vq_info *vq_info;
	struct virtio_net_hdr net_hdr, *guest_hdr;
	struct vionet_tx_offload offload;
	struct ether_header *eh;
	struct iovec *iov;
	struct packet pkt;
	uint8_t status = 0;
	uint64_t stats_bytes = 0, stats_packets = 0;
	uint64_t stats_csum_packets = 0, stats_tso_packets = 0;

	status = dev->status & VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	stats_enabled = log_getverbose() == 1;
	if (status != VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) {
		log_warnx("%s: driver not ready", __func__);
		return (0);
	}

	vq_info = &dev->vq[worker->vq_idx];
	idx = vq_info->last_avail;
	vr = vq_info->q_hva;
	if (vr == NULL || vq_info->q_avail_hva == NULL ||
	    vq_info->q_used_hva == NULL)
		fatalx("%s: vr == NULL", __func__);

	/* Locate the independently mapped split virtqueue areas. */
	table = (struct vring_desc *)(vr);
	avail = vq_info->q_avail_hva;
	used = vq_info->q_used_hva;

	while (idx != avail->idx) {
		dhcpsz = 0;
		dhcppkt = NULL;
		hdr_idx = avail->ring[idx & vq_info->mask];
		desc = &table[hdr_idx & vq_info->mask];
		if (DESC_WRITABLE(desc)) {
			log_warnx("%s: invalid descriptor state", __func__);
			goto reset;
		}

		iov_cnt = 1;	/* Reserve slot 0 for struct tun_hdr. */
		chain_len = 0;

		/*
		 * We do not negotiate VIRTIO_NET_F_HASH_REPORT so we
		 * assume the header length is fixed.
		 */
		if (desc->len < sizeof(struct virtio_net_hdr)) {
			log_warnx("%s: invalid descriptor length", __func__);
			goto reset;
		}
		guest_hdr = hvaddr_mem(desc->addr, sizeof(*guest_hdr));
		if (guest_hdr == NULL)
			goto reset;
		memcpy(&net_hdr, guest_hdr, sizeof(net_hdr));

		if (desc->len > sizeof(struct virtio_net_hdr)) {
			/* Chop off the virtio header, leaving packet data. */
			iov = &worker->iov[iov_cnt++];
			iov->iov_len = desc->len - sizeof(struct virtio_net_hdr);
			iov->iov_base = hvaddr_mem(desc->addr +
			    sizeof(struct virtio_net_hdr), iov->iov_len);
			if (iov->iov_base == NULL)
				goto reset;

			chain_len += iov->iov_len;
		}

		/*
		 * Walk the chain and collect remaining addresses and lengths.
		 */
		while (desc->flags & VRING_DESC_F_NEXT) {
			desc = &table[desc->next & vq_info->mask];
			if (DESC_WRITABLE(desc)) {
				log_warnx("%s: invalid descriptor state",
				    __func__);
				goto reset;
			}

			if (iov_cnt >= nitems(worker->iov)) {
				log_warnx("%s: infinite chain detected",
				    __func__);
				goto reset;
			}

			/* Collect our IO information, translating gpa's. */
			iov = &worker->iov[iov_cnt++];
			iov->iov_len = desc->len;
			iov->iov_base = hvaddr_mem(desc->addr, iov->iov_len);
			if (iov->iov_base == NULL)
				goto reset;
			chain_len += iov->iov_len;

		}

		/* Check if we've got a viable ethernet frame or TSO packet. */
		if (chain_len < VIONET_MIN_TXLEN ||
		    chain_len > VIONET_MAX_TXLEN)
			goto drop;
		if (vionet_tx_offload(dev, &net_hdr, chain_len, &offload) == -1) {
			log_warnx("%s: invalid transmit offload header", __func__);
			goto drop;
		}
		worker->iov[0].iov_base = &offload.tun_hdr;
		worker->iov[0].iov_len = sizeof(offload.tun_hdr);

		/*
		 * Packet inspection for ethernet header (if using a "local"
		 * interface) for possibility of a DHCP packet or (if using
		 * locked lladdr) for validating ethernet header.
		 *
		 * To help preserve zero-copy semantics, we require the first
		 * descriptor with packet data contains a large enough buffer
		 * for this inspection.
		 */
		iov = &worker->iov[1];
		if (vionet->lockedmac) {
			if (iov->iov_len < ETHER_HDR_LEN) {
				log_warnx("%s: insufficient header data",
				    __func__);
				goto drop;
			}
			eh = (struct ether_header *)iov->iov_base;
			if (memcmp(eh->ether_shost, vionet->mac,
			    sizeof(eh->ether_shost)) != 0) {
				log_warnx("%s: bad source address %s",
				    __func__, ether_ntoa((struct ether_addr *)
					eh->ether_shost));
				goto drop;
			}
		}
		if (vionet->local) {
			dhcpsz = dhcp_request(dev, iov->iov_base, iov->iov_len,
			    &dhcppkt);
			if (dhcpsz > 0) {
				log_debug("%s: detected dhcp request of %zu bytes",
				    __func__, dhcpsz);
				goto drop;
			}
		}

		/* Write our packet to the tap(4). */
		sz = writev(vionet->data_fd, worker->iov, iov_cnt);
		if (sz == -1 && errno != ENOBUFS) {
			log_warn("%s", __func__);
			goto reset;
		}
		if (sz >= (ssize_t)sizeof(offload.tun_hdr)) {
			stats_packets++;
			stats_bytes += sz - sizeof(offload.tun_hdr);
			if (offload.tun_hdr.th_flags &
			    (TUN_H_TCP_CSUM | TUN_H_UDP_CSUM))
				stats_csum_packets++;
			if (offload.tso)
				stats_tso_packets++;
		}
		chain_len += sizeof(struct virtio_net_hdr);
drop:
		used->ring[used->idx & vq_info->mask].id = hdr_idx;
		used->ring[used->idx & vq_info->mask].len = chain_len;
		__sync_synchronize();
		used->idx++;
		idx++;

		/* Facilitate DHCP reply injection, if needed. */
		if (dhcpsz > 0) {
			pkt.buf = dhcppkt;
			pkt.len = dhcpsz;
			sz = write(pipe_inject[WRITE], &pkt, sizeof(pkt));
			if (sz == -1 && errno != EAGAIN) {
				log_warn("%s: packet injection", __func__);
				free(pkt.buf);
			} else if (sz == -1 && errno == EAGAIN) {
				log_debug("%s: dropping dhcp reply", __func__);
				free(pkt.buf);
			} else if (sz != sizeof(pkt)) {
				log_warnx("%s: failed packet injection",
				    __func__);
				free(pkt.buf);
			}
		}
	}

	if (idx != vq_info->last_avail &&
	    !(avail->flags & VRING_AVAIL_F_NO_INTERRUPT))
		notify = 1;

	vq_info->last_avail = idx;
	if (stats_enabled) {
		vionet_stats_add(&vionet_stats.tx_packets, stats_packets);
		vionet_stats_add(&vionet_stats.tx_bytes, stats_bytes);
		vionet_stats_add(&vionet_stats.tx_csum_packets,
		    stats_csum_packets);
		vionet_stats_add(&vionet_stats.tx_tso_packets,
		    stats_tso_packets);
		vionet_stats_add(&vionet_stats.queue[worker->pair].tx_packets,
		    stats_packets);
		vionet_stats_add(&vionet_stats.queue[worker->pair].tx_bytes,
		    stats_bytes);
	}
	return (notify);
reset:
	if (stats_enabled) {
		vionet_stats_add(&vionet_stats.tx_packets, stats_packets);
		vionet_stats_add(&vionet_stats.tx_bytes, stats_bytes);
		vionet_stats_add(&vionet_stats.tx_csum_packets,
		    stats_csum_packets);
		vionet_stats_add(&vionet_stats.tx_tso_packets,
		    stats_tso_packets);
		vionet_stats_add(&vionet_stats.queue[worker->pair].tx_packets,
		    stats_packets);
		vionet_stats_add(&vionet_stats.queue[worker->pair].tx_bytes,
		    stats_bytes);
	}
	return (-1);
}

static void
dev_dispatch_vm(int fd, short event, void *arg)
{
	struct virtio_dev	*dev = arg;
	struct vionet_dev	*vionet = &dev->vionet;
	struct imsgev		*iev = &dev->async_iev;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg	 	 imsg;
	int			 n, verbose;
	uint32_t		 type;

	if (dev == NULL)
		fatalx("%s: missing vionet pointer", __func__);

	if (event & EV_READ) {
		if ((n = imsgbuf_read(ibuf)) == -1)
			fatal("%s: imsgbuf_read", __func__);
		if (n == 0) {
			/* this pipe is dead, so remove the event handler */
			log_debug("%s: pipe dead (EV_READ)", __func__);
			event_del(&iev->ev);
			event_base_loopexit(ev_base_main, NULL);
			return;
		}
	}

	if (event & EV_WRITE) {
		if (imsgbuf_write(ibuf) == -1) {
			if (errno == EPIPE) {
				/* this pipe is dead, remove the handler */
				log_debug("%s: pipe dead (EV_WRITE)", __func__);
				event_del(&iev->ev);
				event_loopexit(NULL);
				return;
			}
			fatal("%s: imsgbuf_write", __func__);
		}
	}

	for (;;) {
		if ((n = imsgbuf_get(ibuf, &imsg)) == -1)
			fatal("%s: imsgbuf_get", __func__);
		if (n == 0)
			break;

		type = imsg_get_type(&imsg);
		switch (type) {
		case IMSG_DEVOP_HOSTMAC:
			vionet_hostmac_read(&imsg, vionet);
			log_debug("%s: set hostmac", __func__);
			break;
		case IMSG_VMDOP_PAUSE_VM:
			log_debug("%s: pausing", __func__);
			vm_pipe_send(&pipe_rx, VIRTIO_THREAD_PAUSE);
			break;
		case IMSG_VMDOP_UNPAUSE_VM:
			log_debug("%s: unpausing", __func__);
			if (rx_enabled)
				vm_pipe_send(&pipe_rx, VIRTIO_THREAD_START);
			break;
		case IMSG_CTL_VERBOSE:
			if (imsg_get_data(&imsg, &verbose, sizeof(verbose)))
				fatal("%s", __func__);
			log_setverbose(verbose);
			break;
		}
		imsg_free(&imsg);
	}
	imsg_event_add2(iev, ev_base_main);
}

/*
 * Synchronous IO handler.
 *
 */
static void
handle_sync_io(int fd, short event, void *arg)
{
	struct virtio_dev *dev = (struct virtio_dev *)arg;
	struct imsgev *iev = &dev->sync_iev;
	struct imsgbuf *ibuf = &iev->ibuf;
	struct viodev_msg msg;
	struct imsg imsg;
	int n, deassert = 0;

	if (event & EV_READ) {
		if ((n = imsgbuf_read(ibuf)) == -1)
			fatal("%s: imsgbuf_read", __func__);
		if (n == 0) {
			/* this pipe is dead, so remove the event handler */
			log_debug("%s: pipe dead (EV_READ)", __func__);
			event_del(&iev->ev);
			event_base_loopexit(ev_base_main, NULL);
			return;
		}
	}

	if (event & EV_WRITE) {
		if (imsgbuf_write(ibuf) == -1) {
			if (errno == EPIPE) {
				/* this pipe is dead, remove the handler */
				log_debug("%s: pipe dead (EV_WRITE)", __func__);
				event_del(&iev->ev);
				event_loopexit(NULL);
				return;
			}
			fatal("%s: imsgbuf_write", __func__);
		}
	}

	for (;;) {
		if ((n = imsgbuf_get(ibuf, &imsg)) == -1)
			fatal("%s: imsgbuf_get", __func__);
		if (n == 0)
			break;

		/* Unpack our message. They ALL should be dev messeges! */
		viodev_msg_read(&imsg, &msg);
		imsg_free(&imsg);

		switch (msg.type) {
		case VIODEV_MSG_IO_READ:
			/* Read IO: make sure to send a reply */
			msg.data = vionet_read(dev, &msg, &deassert);
			msg.data_valid = 1;
			if (deassert)
				msg.state = INTR_STATE_DEASSERT;
			imsg_compose_event2(iev, IMSG_DEVOP_MSG, 0, 0, -1, &msg,
			    sizeof(msg), ev_base_main);
			break;
		case VIODEV_MSG_IO_WRITE:
			/* Write IO: no reply needed */
			vionet_write(dev, &msg);
			break;
		case VIODEV_MSG_SHUTDOWN:
			event_del(&dev->sync_iev.ev);
			event_base_loopbreak(ev_base_main);
			return;
		default:
			fatalx("%s: invalid msg type %d", __func__, msg.type);
		}
	}
	imsg_event_add2(iev, ev_base_main);
}

static uint32_t
vionet_cfg_read(struct virtio_dev *dev, struct viodev_msg *msg)
{
	uint32_t data;
	uint8_t reg = msg->reg & 0xff;

	pthread_rwlock_rdlock(&lock);
	data = virtio_io_cfg(dev, VEI_DIR_IN, reg, 0, msg->io_sz);
	pthread_rwlock_unlock(&lock);

	return (data);
}

static void
vionet_cfg_write(struct virtio_dev *dev, struct viodev_msg *msg)
{
	struct vionet_dev *vionet = (struct vionet_dev *)&dev->vionet;
	unsigned int i;
	uint8_t reg = msg->reg & 0xff;
	int pausing;

	pausing = reg == VIO1_PCI_DEVICE_STATUS && msg->io_sz == 1 &&
	    msg->data == 0;

	pthread_rwlock_wrlock(&lock);
	(void)virtio_io_cfg(dev, VEI_DIR_OUT, reg, msg->data, msg->io_sz);
	if (pausing) {
		vionet->reset_generation++;
		if (vionet->reset_generation == 0)
			vionet->reset_generation = 1;
		vionet->active_queue_pairs = 1;
	}
	pthread_rwlock_unlock(&lock);

	if (pausing) {
		/* Pause tx and rx processing. */
		rx_enabled = 0;
		vionet_deassert_pic_irq(dev);
		vm_pipe_send(&pipe_rx, VIRTIO_THREAD_PAUSE);
		for (i = 0; i < VIONET_QUEUE_PAIRS; i++)
			vm_pipe_send(&tx_workers[i].pipe, VIRTIO_THREAD_PAUSE);
	}
}

static uint32_t
vionet_read(struct virtio_dev *dev, struct viodev_msg *msg, int *deassert)
{
	uint32_t data = 0;
	uint16_t reg = msg->reg;

	switch (reg & 0xFF00) {
	case VIO1_CFG_BAR_OFFSET:
		data = vionet_cfg_read(dev, msg);
		break;
	case VIO1_DEV_BAR_OFFSET:
		data = vionet_dev_read(dev, msg);
		break;
	case VIO1_NOTIFY_BAR_OFFSET:
		/* Reads of notify register return all 1's. */
		data = (uint32_t)(-1);
		break;
	case VIO1_ISR_BAR_OFFSET:
		pthread_rwlock_wrlock(&lock);
		data = dev->isr;
		dev->isr = 0;
		*deassert = 1;
		pthread_rwlock_unlock(&lock);
		break;
	default:
		log_debug("%s: no handler for reg 0x%04x", __func__, reg);
	}

	return (data);
}

static void
vionet_write(struct virtio_dev *dev, struct viodev_msg *msg)
{
	uint16_t reg = msg->reg;

	switch (reg & 0xFF00) {
	case VIO1_CFG_BAR_OFFSET:
		(void)vionet_cfg_write(dev, msg);
		break;
	case VIO1_DEV_BAR_OFFSET:
		/* Ignore all writes to device configuration registers. */
		break;
	case VIO1_NOTIFY_BAR_OFFSET:
		vionet_notifyq(dev, (uint16_t)(msg->data));
		break;
	case VIO1_ISR_BAR_OFFSET:
		/* ignore writes to ISR. */
		break;
	default:
		log_debug("%s: no handler for reg 0x%04x", __func__, reg);
	}
}

static uint32_t
vionet_dev_read(struct virtio_dev *dev, struct viodev_msg *msg)
{
	struct vionet_dev *vionet = (struct vionet_dev *)&dev->vionet;
	uint8_t cfg[VIRTIO_NET_CONFIG_MAX_QUEUES + sizeof(uint16_t)] = { 0 };
	uint32_t data = 0;
	uint16_t reg = msg->reg & 0xFF;
	uint8_t sz = msg->io_sz;

	memcpy(cfg + VIRTIO_NET_CONFIG_MAC, vionet->mac,
	    sizeof(vionet->mac));
	cfg[VIRTIO_NET_CONFIG_MAX_QUEUES] = VIONET_QUEUE_PAIRS & 0xff;
	cfg[VIRTIO_NET_CONFIG_MAX_QUEUES + 1] = VIONET_QUEUE_PAIRS >> 8;

	if ((sz != 1 && sz != 2 && sz != 4) ||
	    (size_t)reg + sz > sizeof(cfg)) {
		log_warnx("%s: invalid access reg 0x%04x size %u", __func__,
		    reg, sz);
	} else {
		memcpy(&data, cfg + reg, sz);
	}

	return (data);
}

/*
 * Handle the rx side processing, communicating to the main thread via pipe.
 */
static void *
rx_run_loop(void *arg)
{
	struct virtio_dev	*dev = (struct virtio_dev *)arg;
	struct vionet_dev	*vionet = &dev->vionet;
	int			 ret;

	ev_base_rx = event_base_new();

	/* Wire up event handling for the tap fd. */
	event_set(&ev_tap, vionet->data_fd, EV_READ | EV_PERSIST,
	    vionet_rx_event, dev);
	event_base_set(ev_base_rx, &ev_tap);

	/* Wire up event handling for the packet injection pipe. */
	event_set(&ev_inject, pipe_inject[READ], EV_READ | EV_PERSIST,
	    vionet_rx_event, dev);
	event_base_set(ev_base_rx, &ev_inject);

	/* Wire up event handling for our inter-thread communication channel. */
	event_base_set(ev_base_rx, &pipe_rx.read_ev);
	event_add(&pipe_rx.read_ev, NULL);

	/* Begin our event loop with our channel event active. */
	ret = event_base_dispatch(ev_base_rx);
	event_base_free(ev_base_rx);

	log_debug("%s: exiting (%d)", __func__, ret);

	close_fd(pipe_rx.read);
	close_fd(pipe_inject[READ]);

	return (NULL);
}

/*
 * Handle the tx side processing, communicating to the main thread via pipe.
 */
static void *
tx_run_loop(void *arg)
{
	struct vionet_tx_worker *worker = arg;
	int			 ret;

	worker->event_base = event_base_new();

	/* Wire up event handling for our inter-thread communication channel. */
	event_base_set(worker->event_base, &worker->pipe.read_ev);
	event_add(&worker->pipe.read_ev, NULL);

	/* Begin our event loop with our channel event active. */
	ret = event_base_dispatch(worker->event_base);
	event_base_free(worker->event_base);
	worker->event_base = NULL;

	log_debug("%s: tx%u exiting (%d)", __func__, worker->pair, ret);

	close_fd(worker->pipe.read);

	return (NULL);
}

/*
 * Read events sent by the main thread to the rx thread.
 */
static void
read_pipe_rx(int fd, short event, void *arg)
{
	enum pipe_msg_type	msg;

	if (!(event & EV_READ))
		fatalx("%s: invalid event type", __func__);

	msg = vm_pipe_recv(&pipe_rx);

	switch (msg) {
	case VIRTIO_NOTIFY:
	case VIRTIO_THREAD_START:
		event_add(&ev_tap, NULL);
		event_add(&ev_inject, NULL);
		break;
	case VIRTIO_THREAD_PAUSE:
		event_del(&ev_tap);
		event_del(&ev_inject);
		break;
	case VIRTIO_THREAD_STOP:
		event_del(&ev_tap);
		event_del(&ev_inject);
		event_base_loopexit(ev_base_rx, NULL);
		break;
	default:
		fatalx("%s: invalid channel message: %d", __func__, msg);
	}
}

/*
 * Read events sent by the main thread to the tx thread.
 */
static void
read_pipe_tx(int fd, short event, void *arg)
{
	struct vionet_tx_worker	*worker = arg;
	struct virtio_dev	*dev = worker->dev;
	struct vionet_dev	*vionet = (struct vionet_dev*)&dev->vionet;
	enum pipe_msg_type	 msg;
	int			 raise_irq = 0, ret = 0;
	uint32_t		 generation = 0;

	if (!(event & EV_READ))
		fatalx("%s: invalid event type", __func__);

	msg = vm_pipe_recv(&worker->pipe);

	switch (msg) {
	case VIRTIO_NOTIFY:
		pthread_rwlock_rdlock(&lock);
		generation = vionet->reset_generation;
		ret = vionet_tx(worker);
		pthread_rwlock_unlock(&lock);
		break;
	case VIRTIO_THREAD_START:
		/* Ignore Start messages. */
		break;
	case VIRTIO_THREAD_PAUSE:
		/* Nothing to do when pausing on the tx side. */
		break;
	case VIRTIO_THREAD_STOP:
		event_base_loopexit(worker->event_base, NULL);
		break;
	default:
		fatalx("%s: invalid channel message: %d", __func__, msg);
	}

	if (ret == 0) {
		/* No notification needed. Return early. */
		return;
	}

	pthread_rwlock_wrlock(&lock);
	if (generation == vionet->reset_generation) {
		if (ret == 1) {
			/* Notify the driver. */
			dev->isr |= 1;
		} else {
			/* Need a reset. Something went wrong. */
			log_warnx("%s: requesting device reset", __func__);
			dev->status |= DEVICE_NEEDS_RESET;
			dev->isr |= VIRTIO_CONFIG_ISR_CONFIG_CHANGE;
		}
		raise_irq = 1;
	}
	pthread_rwlock_unlock(&lock);

	if (raise_irq) {
		if (ret == 1)
			vm_pipe_send(&pipe_main, VIRTIO_RAISE_IRQ_TX +
			    worker->pair);
		else
			vm_pipe_send(&pipe_main, VIRTIO_RAISE_IRQ_CONFIG);
	}
}

/*
 * Read events sent by the rx/tx threads to the main thread.
 */
static void
read_pipe_main(int fd, short event, void *arg)
{
	struct virtio_dev	*dev = (struct virtio_dev*)arg;
	enum pipe_msg_type	 msg;

	if (!(event & EV_READ))
		fatalx("%s: invalid event type", __func__);

	msg = vm_pipe_recv(&pipe_main);
	switch (msg) {
	case VIRTIO_RAISE_IRQ_RX:
		vionet_assert_irq(dev, RXQ);
		break;
	case VIRTIO_RAISE_IRQ_TX:
	case VIRTIO_RAISE_IRQ_TX1:
	case VIRTIO_RAISE_IRQ_TX2:
	case VIRTIO_RAISE_IRQ_TX3:
		vionet_assert_irq(dev, VIONET_TXQ(msg -
		    VIRTIO_RAISE_IRQ_TX));
		break;
	case VIRTIO_RAISE_IRQ_CONFIG:
		vionet_assert_irq(dev, VIODEV_QUEUE_CONFIG);
		break;
	default:
		fatalx("%s: invalid channel msg: %d", __func__, msg);
	}
}

/*
 * Message the vm process asking to raise the irq. Must be called from the main
 * thread.
 */
static void
vionet_assert_irq(struct virtio_dev *dev, uint16_t vq_idx)
{
	struct viodev_msg	msg;
	unsigned int		pair;
	int			ret;

	memset(&msg, 0, sizeof(msg));
	if (vq_idx == vionet_ctrlq(dev))
		vionet_stats_add(&vionet_stats.ctrl_irqs, 1);
	else if (vq_idx < VIONET_CTRLQ_MQ) {
		pair = vq_idx / 2;
		if ((vq_idx & 1) == 0) {
			vionet_stats_add(&vionet_stats.rx_irqs, 1);
			vionet_stats_add(&vionet_stats.queue[pair].rx_irqs, 1);
		} else {
			vionet_stats_add(&vionet_stats.tx_irqs, 1);
			vionet_stats_add(&vionet_stats.queue[pair].tx_irqs, 1);
		}
	} else if (vq_idx == VIODEV_QUEUE_CONFIG)
		vionet_stats_add(&vionet_stats.config_irqs, 1);
	msg.irq = dev->irq;
	msg.vcpu = 0; /* XXX: smp */
	msg.vq_idx = vq_idx;
	msg.type = VIODEV_MSG_KICK;
	msg.state = INTR_STATE_ASSERT;

	ret = imsg_compose_event2(&dev->async_iev, IMSG_DEVOP_MSG, 0, 0, -1,
	    &msg, sizeof(msg), ev_base_main);
	if (ret == -1)
		log_warnx("%s: failed to assert irq %d", __func__, dev->irq);
}

/*
 * Message the vm process asking to lower the irq. Must be called from the main
 * thread.
 */
static void
vionet_deassert_pic_irq(struct virtio_dev *dev)
{
	struct viodev_msg	msg;
	int			ret;

	memset(&msg, 0, sizeof(msg));
	msg.irq = dev->irq;
	msg.vcpu = 0; /* XXX: smp */
	msg.vq_idx = VIODEV_QUEUE_CONFIG;
	msg.type = VIODEV_MSG_KICK;
	msg.state = INTR_STATE_DEASSERT;

	ret = imsg_compose_event2(&dev->async_iev, IMSG_DEVOP_MSG, 0, 0, -1,
	    &msg, sizeof(msg), ev_base_main);
	if (ret == -1)
		log_warnx("%s: failed to assert irq %d", __func__, dev->irq);
}
