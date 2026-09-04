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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio.h"

#define CHECK(_expr) do {						\
	if (!(_expr)) {						\
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n",		\
		    __FILE__, __LINE__, #_expr);			\
		exit(1);						\
	}							\
} while (0)

static uint8_t guest_mem;
static unsigned int warning_count;

/* Stubs needed by the live virtio.c paths this focused regression exercises. */
void *
hvaddr_mem(paddr_t gpa, size_t len)
{
	(void)gpa;
	(void)len;
	return (&guest_mem);
}

int
log_getverbose(void)
{
	return (0);
}

void
log_debug(const char *fmt, ...)
{
	(void)fmt;
}

void
log_warnx(const char *fmt, ...)
{
	(void)fmt;
	warning_count++;
}

__dead void
fatalx(const char *fmt, ...)
{
	(void)fmt;
	fprintf(stderr, "unexpected fatalx()\n");
	exit(1);
}

static void
cfg_write_bytes(struct virtio_dev *dev, uint8_t reg, uint64_t value,
    size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		(void)virtio_io_cfg(dev, VEI_DIR_OUT, reg + i,
		    value >> (i * 8), 1);
}

static uint64_t
cfg_read_bytes(struct virtio_dev *dev, uint8_t reg, size_t len)
{
	uint64_t value = 0;
	size_t i;

	for (i = 0; i < len; i++)
		value |= (uint64_t)virtio_io_cfg(dev, VEI_DIR_IN, reg + i,
		    0, 1) << (i * 8);
	return (value);
}

static void
init_dev(struct virtio_dev *dev)
{
	size_t i;

	memset(dev, 0, sizeof(*dev));
	dev->device_feature = UINT64_MAX;
	dev->num_queues = 4;
	dev->queue_size = 128;
	dev->pci_cfg.config_msix_vector = VIRTIO_MSI_NO_VECTOR;
	dev->config_msix_vector_staged = VIRTIO_MSI_NO_VECTOR;
	for (i = 0; i < dev->num_queues; i++)
		virtio_vq_init(dev, i);
	virtio_update_qs(dev);
}

static void
test_partial_reads(struct virtio_dev *dev)
{
	const uint64_t device_features = UINT64_C(0x8877665544332211);
	const uint64_t desc = UINT64_C(0x1122334455667780);
	const uint64_t avail = UINT64_C(0x99aabbccddeeff82);
	const uint64_t used = UINT64_C(0x0123456789abcd84);
	unsigned int warnings;

	dev->device_feature = device_features;
	dev->pci_cfg.device_feature_select = 0;
	CHECK(cfg_read_bytes(dev, VIO1_PCI_DEVICE_FEATURE, 4) ==
	    (uint32_t)device_features);
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN,
	    VIO1_PCI_DEVICE_FEATURE + 1, 0, 2) == 0x3322);
	dev->pci_cfg.device_feature_select = 1;
	CHECK(cfg_read_bytes(dev, VIO1_PCI_DEVICE_FEATURE, 4) ==
	    (uint32_t)(device_features >> 32));
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_NUM_QUEUES + 1,
	    0, 1) == 0);

	dev->vq[2].qs = 0x100;
	dev->vq[2].q_gpa = desc;
	dev->vq[2].q_avail_gpa = avail;
	dev->vq[2].q_used_gpa = used;
	dev->vq[2].q_msix_vector = 3;
	dev->vq[2].vq_enabled = 1;
	dev->pci_cfg.queue_select = 2;
	virtio_update_qs(dev);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_SELECT, 2) == 2);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_SIZE, 2) == 0x100);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_DESC, 8) == desc);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_AVAIL, 8) == avail);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_USED, 8) == used);
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_QUEUE_DESC,
	    0, 4) == (uint32_t)desc);
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_QUEUE_DESC + 4,
	    0, 4) == (uint32_t)(desc >> 32));
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_QUEUE_DESC + 1,
	    0, 2) == 0x6677);

	/* An access may not straddle adjacent fields or 64-bit dword halves. */
	warnings = warning_count;
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_QUEUE_DESC + 3,
	    0, 2) == 0);
	CHECK(virtio_io_cfg(dev, VEI_DIR_IN, VIO1_PCI_QUEUE_SELECT + 1,
	    0, 2) == 0);
	CHECK(warning_count == warnings + 2);
}

static void
test_partial_writes(struct virtio_dev *dev)
{
	const uint64_t desc = UINT64_C(0x1122334455667780);
	const uint64_t avail = UINT64_C(0x99aabbccddeeff82);
	const uint64_t used = UINT64_C(0x0123456789abcd84);
	unsigned int warnings;

	dev->device_feature = UINT64_MAX;
	dev->pci_cfg.device_feature_select = 0;
	cfg_write_bytes(dev, VIO1_PCI_DEVICE_FEATURE_SELECT, 0x12345678, 4);
	CHECK(dev->pci_cfg.device_feature_select == 0x12345678);

	cfg_write_bytes(dev, VIO1_PCI_DRIVER_FEATURE_SELECT, 0, 4);
	cfg_write_bytes(dev, VIO1_PCI_DRIVER_FEATURE, 0x89abcdef, 4);
	cfg_write_bytes(dev, VIO1_PCI_DRIVER_FEATURE_SELECT, 1, 4);
	cfg_write_bytes(dev, VIO1_PCI_DRIVER_FEATURE, 0x01234567, 4);
	CHECK(dev->driver_feature == UINT64_C(0x0123456789abcdef));
	CHECK(cfg_read_bytes(dev, VIO1_PCI_DRIVER_FEATURE, 4) == 0x01234567);

	/* Select queue 2 using byte writes and configure it byte by byte. */
	dev->pci_cfg.queue_select = 0;
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_SELECT, 2, 2);
	CHECK(dev->pci_cfg.queue_select == 2);
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_SIZE, 0x100, 2);
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_DESC, desc, 8);
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_AVAIL, avail, 8);
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_USED, used, 8);
	cfg_write_bytes(dev, VIO1_PCI_QUEUE_ENABLE, 1, 2);
	CHECK(dev->pci_cfg.queue_size == 0x100);
	CHECK(dev->pci_cfg.queue_desc == desc);
	CHECK(dev->pci_cfg.queue_avail == avail);
	CHECK(dev->pci_cfg.queue_used == used);
	CHECK(dev->vq[2].qs == 0x100);
	CHECK(dev->vq[2].q_gpa == desc);
	CHECK(dev->vq[2].q_avail_gpa == avail);
	CHECK(dev->vq[2].q_used_gpa == used);
	CHECK(dev->vq[2].vq_enabled == 1);

	/* Read-only fields and invalid access sizes must remain unchanged. */
	warnings = warning_count;
	(void)virtio_io_cfg(dev, VEI_DIR_OUT, VIO1_PCI_NUM_QUEUES + 1,
	    0xff, 1);
	CHECK(dev->num_queues == 4);
	CHECK(virtio_io_cfg(dev, VEI_DIR_OUT, VIO1_PCI_QUEUE_DESC,
	    0, 3) == 0);
	CHECK(warning_count == warnings + 2);
}

static void
test_msix_byte_writes(struct virtio_dev *dev)
{
	dev->pci_cfg.config_msix_vector = VIRTIO_MSI_NO_VECTOR;
	dev->config_msix_vector_staged = VIRTIO_MSI_NO_VECTOR;
	dev->config_msix_vector_bytes = 0;
	(void)virtio_io_cfg(dev, VEI_DIR_OUT,
	    VIO1_PCI_CONFIG_MSIX_VECTOR, 1, 1);
	CHECK(dev->pci_cfg.config_msix_vector == VIRTIO_MSI_NO_VECTOR);
	CHECK(dev->config_msix_vector_staged == 0xff01);
	CHECK(dev->config_msix_vector_bytes == 1);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_CONFIG_MSIX_VECTOR, 2) == 0xff01);
	(void)virtio_io_cfg(dev, VEI_DIR_OUT,
	    VIO1_PCI_CONFIG_MSIX_VECTOR + 1, 0, 1);
	CHECK(dev->pci_cfg.config_msix_vector == 1);
	CHECK(dev->config_msix_vector_bytes == 0);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_CONFIG_MSIX_VECTOR, 2) == 1);

	dev->pci_cfg.queue_select = 2;
	dev->vq[2].q_msix_vector = VIRTIO_MSI_NO_VECTOR;
	dev->vq[2].q_msix_vector_staged = VIRTIO_MSI_NO_VECTOR;
	dev->vq[2].q_msix_vector_bytes = 0;
	virtio_update_qs(dev);
	(void)virtio_io_cfg(dev, VEI_DIR_OUT,
	    VIO1_PCI_QUEUE_MSIX_VECTOR, 2, 1);
	CHECK(dev->vq[2].q_msix_vector == VIRTIO_MSI_NO_VECTOR);
	CHECK(dev->vq[2].q_msix_vector_staged == 0xff02);
	CHECK(dev->vq[2].q_msix_vector_bytes == 1);
	(void)virtio_io_cfg(dev, VEI_DIR_OUT,
	    VIO1_PCI_QUEUE_MSIX_VECTOR + 1, 0, 1);
	CHECK(dev->vq[2].q_msix_vector == 2);
	CHECK(dev->pci_cfg.queue_msix_vector == 2);
	CHECK(dev->vq[2].q_msix_vector_bytes == 0);

	/* A completed, out-of-range vector reads back as NO_VECTOR. */
	(void)virtio_io_cfg(dev, VEI_DIR_OUT,
	    VIO1_PCI_QUEUE_MSIX_VECTOR, 9, 2);
	CHECK(dev->vq[2].q_msix_vector == VIRTIO_MSI_NO_VECTOR);
	CHECK(cfg_read_bytes(dev, VIO1_PCI_QUEUE_MSIX_VECTOR, 2) ==
	    VIRTIO_MSI_NO_VECTOR);
}

static void
test_status_reset(struct virtio_dev *dev)
{
	size_t i;

	dev->driver_feature = UINT64_C(0x123456789abcdef0);
	dev->isr = 3;
	dev->pci_cfg.config_msix_vector = 1;
	dev->config_msix_vector_staged = 1;
	dev->vq[2].q_msix_vector = 2;
	dev->vq[2].vq_enabled = 1;
	(void)virtio_io_cfg(dev, VEI_DIR_OUT, VIO1_PCI_DEVICE_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_ACK, 1);
	CHECK(dev->status == VIRTIO_CONFIG_DEVICE_STATUS_ACK);
	(void)virtio_io_cfg(dev, VEI_DIR_OUT, VIO1_PCI_DEVICE_STATUS, 0, 1);
	CHECK(dev->status == 0);
	CHECK(dev->driver_feature == 0);
	CHECK(dev->isr == 0);
	CHECK(dev->pci_cfg.config_msix_vector == VIRTIO_MSI_NO_VECTOR);
	CHECK(dev->pci_cfg.queue_select == 0);
	for (i = 0; i < dev->num_queues; i++) {
		CHECK(dev->vq[i].q_msix_vector == VIRTIO_MSI_NO_VECTOR);
		CHECK(dev->vq[i].vq_enabled == 0);
		CHECK(dev->vq[i].qs == dev->queue_size);
	}
}

int
main(void)
{
	struct virtio_dev dev;

	init_dev(&dev);
	test_partial_reads(&dev);
	init_dev(&dev);
	test_partial_writes(&dev);
	test_msix_byte_writes(&dev);
	test_status_reset(&dev);
	return (0);
}
