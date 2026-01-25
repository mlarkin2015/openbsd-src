/*	$OpenBSD$ */
/*
 * Copyright (c) 2026
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
#include <sys/queue.h>

#include <dev/pci/virtio_pcireg.h>
#include <dev/pv/viogpu.h>
#include <dev/pv/virtioreg.h>

#include <sys/endian.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "vmd.h"
#include "virtio.h"
#include "viogpu.h"

#define VIOGPU_DEBUG 0
#ifdef DPRINTF
#undef DPRINTF
#endif
#if VIOGPU_DEBUG
#define DPRINTF		log_debug
#else
#define DPRINTF(x...) do {} while (0)
#endif

#define VIOGPU_MAX_RESOURCES	1024

static uint32_t
viogpu_resp_type(uint32_t type)
{
	switch (type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
	case VIRTIO_GPU_CMD_RESOURCE_UNREF:
	case VIRTIO_GPU_CMD_SET_SCANOUT:
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
		return VIRTIO_GPU_RESP_OK_NODATA;
	default:
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	}
}

static int
viogpu_rect_valid(const struct viogpu_dev *gpu, const struct virtio_gpu_rect *r)
{
	uint64_t x, y, w, h;

	x = r->x;
	y = r->y;
	w = r->width;
	h = r->height;

	if (w == 0 || h == 0)
		return 0;
	if (x > UINT32_MAX - w || y > UINT32_MAX - h)
		return 0;
	if (x + w > gpu->width || y + h > gpu->height)
		return 0;
	return 1;
}

static struct viogpu_resource *
viogpu_resource_lookup(struct viogpu_dev *gpu, uint32_t id)
{
	size_t i;

	if (id == 0)
		return NULL;

	for (i = 0; i < gpu->nresources; i++) {
		if (gpu->resources[i].id == id)
			return &gpu->resources[i];
	}

	return NULL;
}

static int
viogpu_resource_add(struct viogpu_dev *gpu, struct viogpu_resource *res)
{
	struct viogpu_resource *newres;
	uint64_t size;
	uint64_t stride;

	if (gpu->nresources >= VIOGPU_MAX_RESOURCES)
		return E2BIG;
	if (res->width == 0 || res->height == 0)
		return EINVAL;
	if (res->width > VM_MAX_DISPLAY_WIDTH || res->height > VM_MAX_DISPLAY_HEIGHT)
		return EINVAL;
	if (res->format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM)
		return EINVAL;

	stride = (uint64_t)res->width * 4;
	if (stride > UINT32_MAX)
		return EINVAL;
	if (res->height > UINT64_MAX / stride)
		return EINVAL;
	size = stride * res->height;
	if (size == 0 || size > gpu->fb_size)
		return EINVAL;

	newres = reallocarray(gpu->resources, gpu->nresources + 1,
	    sizeof(*newres));
	if (newres == NULL)
		return ENOMEM;
	res->buf = calloc(1, size);
	if (res->buf == NULL) {
		free(newres);
		return ENOMEM;
	}
	res->size = size;
	res->nr_entries = 0;
	res->entries = NULL;
	res->backing_size = 0;

	newres[gpu->nresources] = *res;
	gpu->resources = newres;
	gpu->nresources++;
	return 0;
}

static void
viogpu_resource_remove(struct viogpu_dev *gpu, uint32_t id)
{
	size_t i;

	for (i = 0; i < gpu->nresources; i++) {
		if (gpu->resources[i].id != id)
			continue;
		free(gpu->resources[i].entries);
		free(gpu->resources[i].buf);
		if (i + 1 < gpu->nresources) {
			memmove(&gpu->resources[i], &gpu->resources[i + 1],
			    (gpu->nresources - i - 1) * sizeof(*gpu->resources));
		}
		gpu->nresources--;
		if (gpu->nresources == 0) {
			free(gpu->resources);
			gpu->resources = NULL;
		}
		if (gpu->scanout_resource_id == id)
			gpu->scanout_resource_id = 0;
		return;
	}
}

static int
viogpu_attach_backing(struct viogpu_resource *res,
    const struct virtio_gpu_resource_attach_backing *req,
    uint64_t data_addr, uint32_t data_len)
{
	struct virtio_gpu_mem_entry *entries;
	uint64_t total;
	uint32_t i;

	if (req->nr_entries == 0 || req->nr_entries > VIOGPU_MAX_RESOURCES)
		return EINVAL;
	if (data_len < req->nr_entries * sizeof(*entries))
		return EINVAL;

	entries = calloc(req->nr_entries, sizeof(*entries));
	if (entries == NULL)
		return ENOMEM;
	if (read_mem(data_addr, entries, req->nr_entries * sizeof(*entries))) {
		free(entries);
		return EINVAL;
	}

	for (i = 0; i < req->nr_entries; i++) {
		entries[i].addr = le64toh(entries[i].addr);
		entries[i].length = le32toh(entries[i].length);
		if (entries[i].length == 0) {
			free(entries);
			return EINVAL;
		}
		if (entries[i].length > UINT32_MAX - entries[i].addr) {
			free(entries);
			return EINVAL;
		}
	}

	total = 0;
	for (i = 0; i < req->nr_entries; i++) {
		if (total > UINT64_MAX - entries[i].length) {
			free(entries);
			return EINVAL;
		}
		total += entries[i].length;
	}
	if (total < res->size) {
		free(entries);
		return EINVAL;
	}

	free(res->entries);
	res->entries = entries;
	res->nr_entries = req->nr_entries;
	res->backing_size = total;
	return 0;
}

static int
viogpu_copy_from_backing(struct viogpu_resource *res, uint64_t offset,
    const struct virtio_gpu_rect *r)
{
	uint64_t stride;
	uint64_t row_bytes;
	uint64_t start;
	uint32_t i;
	uint64_t pos;
	uint64_t left;
	uint8_t *dst;

	stride = (uint64_t)res->width * 4;
	row_bytes = (uint64_t)r->width * 4;
	if (row_bytes == 0 || row_bytes > stride)
		return EINVAL;
	if (r->height > UINT64_MAX / stride)
		return EINVAL;
	start = offset + (uint64_t)r->y * stride + (uint64_t)r->x * 4;
	if (start > UINT64_MAX - (r->height - 1) * stride - row_bytes)
		return EINVAL;
	if (start + (r->height - 1) * stride + row_bytes > res->backing_size)
		return EINVAL;

	pos = start;
	dst = res->buf + (uint64_t)r->y * stride + (uint64_t)r->x * 4;
	for (i = 0; i < r->height; i++) {
		left = row_bytes;
		while (left > 0) {
			uint32_t j;
			uint64_t cur = 0;
			for (j = 0; j < res->nr_entries; j++) {
				uint64_t len = res->entries[j].length;
				if (pos < cur + len) {
					uint64_t chunk = len - (pos - cur);
					uint64_t copy_len = chunk < left ? chunk : left;
					if (read_mem(res->entries[j].addr + (pos - cur),
					    dst, copy_len))
						return EINVAL;
					pos += copy_len;
					dst += copy_len;
					left -= copy_len;
					break;
				}
				cur += len;
			}
			if (left > 0)
				return EINVAL;
		}
		dst += stride - row_bytes;
		pos = start + (uint64_t)(i + 1) * stride;
	}

	return 0;
}

static void
viogpu_set_resp(struct virtio_gpu_ctrl_hdr *resp,
    const struct virtio_gpu_ctrl_hdr *req, uint32_t type)
{
	memset(resp, 0, sizeof(*resp));
	resp->type = htole32(type);
	resp->flags = htole32(req->flags);
	resp->fence_id = htole64(req->fence_id);
	resp->ctx_id = htole32(req->ctx_id);
}

static int
viogpu_send_response(struct virtio_dev *dev, struct virtio_vq_info *vq_info,
    struct virtio_vq_acct *acct, const void *resp, size_t resp_len)
{
	struct vring_desc *desc = acct->resp_desc;
	uint16_t uidx;

	if (desc == NULL)
		return 0;
	if (!(desc->flags & VRING_DESC_F_WRITE))
		return 0;
	if (resp_len > desc->len)
		return 0;
	if (write_mem(desc->addr, resp, resp_len))
		return 0;

	uidx = acct->used->idx & vq_info->mask;
	acct->used->ring[uidx].id = acct->req_idx;
	acct->used->ring[uidx].len = resp_len;
	__sync_synchronize();
	acct->used->idx++;
	dev->isr = 1;
	return 1;
}

static int
viogpu_handle_get_display_info(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_resp_display_info resp;

	memset(&resp, 0, sizeof(resp));
	viogpu_set_resp(&resp.hdr, req_hdr, VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
	resp.pmodes[0].r.x = htole32(0);
	resp.pmodes[0].r.y = htole32(0);
	resp.pmodes[0].r.width = htole32(gpu->width);
	resp.pmodes[0].r.height = htole32(gpu->height);
	resp.pmodes[0].enabled = htole32(1);

	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_resource_create_2d(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_resource_create_2d *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct viogpu_resource res;
	struct virtio_gpu_ctrl_hdr resp;
	int error;

	if (viogpu_resource_lookup(gpu, le32toh(req->resource_id)) != NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	memset(&res, 0, sizeof(res));
	res.id = le32toh(req->resource_id);
	res.width = le32toh(req->width);
	res.height = le32toh(req->height);
	res.format = le32toh(req->format);

	error = viogpu_resource_add(gpu, &res);
	if (error != 0)
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
	else
		viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);

	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_resource_unref(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_resource_unref *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;

	viogpu_resource_remove(gpu, le32toh(req->resource_id));
	viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_set_scanout(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_set_scanout *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;
	struct viogpu_resource *res;
	struct virtio_gpu_rect r;

	if (le32toh(req->scanout_id) != 0) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	if (le32toh(req->resource_id) == 0) {
		gpu->scanout_resource_id = 0;
		viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	res = viogpu_resource_lookup(gpu, le32toh(req->resource_id));
	if (res == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	r.x = le32toh(req->r.x);
	r.y = le32toh(req->r.y);
	r.width = le32toh(req->r.width);
	r.height = le32toh(req->r.height);
	if (!viogpu_rect_valid(gpu, &r)) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	gpu->scanout_resource_id = le32toh(req->resource_id);
	viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_attach_backing(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_resource_attach_backing *req,
    uint64_t data_addr, uint32_t data_len)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;
	struct viogpu_resource *res;
	int error;
	struct virtio_gpu_resource_attach_backing host_req;

	memset(&host_req, 0, sizeof(host_req));
	host_req.resource_id = le32toh(req->resource_id);
	host_req.nr_entries = le32toh(req->nr_entries);

	res = viogpu_resource_lookup(gpu, host_req.resource_id);
	if (res == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	error = viogpu_attach_backing(res, &host_req, data_addr, data_len);
	if (error != 0)
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
	else
		viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_detach_backing(struct virtio_dev *dev,
    struct virtio_vq_info *vq_info, struct virtio_vq_acct *acct,
    const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_resource_detach_backing *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;
	struct viogpu_resource *res;

	res = viogpu_resource_lookup(gpu, le32toh(req->resource_id));
	if (res == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	free(res->entries);
	res->entries = NULL;
	res->nr_entries = 0;
	res->backing_size = 0;
	viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_transfer(struct virtio_dev *dev, struct virtio_vq_info *vq_info,
    struct virtio_vq_acct *acct, const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_transfer_to_host_2d *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;
	struct viogpu_resource *res;
	struct virtio_gpu_rect r;
	int error;

	res = viogpu_resource_lookup(gpu, le32toh(req->resource_id));
	if (res == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	if (res->nr_entries == 0 || res->entries == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	r.x = le32toh(req->r.x);
	r.y = le32toh(req->r.y);
	r.width = le32toh(req->r.width);
	r.height = le32toh(req->r.height);
	if (!viogpu_rect_valid(gpu, &r)) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	error = viogpu_copy_from_backing(res, le64toh(req->offset), &r);
	if (error != 0)
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
	else
		viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);

	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_flush(struct virtio_dev *dev, struct virtio_vq_info *vq_info,
    struct virtio_vq_acct *acct, const struct virtio_gpu_ctrl_hdr *req_hdr,
    const struct virtio_gpu_resource_flush *req)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	struct virtio_gpu_ctrl_hdr resp;
	struct viogpu_resource *res;
	struct vnc_rect dirty;
	struct virtio_gpu_rect r;
	uint64_t stride;
	uint8_t *src;
	uint8_t *dst;
	uint32_t i;
	uint64_t row_bytes;

	res = viogpu_resource_lookup(gpu, le32toh(req->resource_id));
	if (res == NULL) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}
	r.x = le32toh(req->r.x);
	r.y = le32toh(req->r.y);
	r.width = le32toh(req->r.width);
	r.height = le32toh(req->r.height);
	if (!viogpu_rect_valid(gpu, &r)) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	stride = (uint64_t)gpu->width * 4;
	row_bytes = (uint64_t)r.width * 4;
	if (r.height > UINT64_MAX / stride) {
		viogpu_set_resp(&resp, req_hdr,
		    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
		return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	}

	src = res->buf + (uint64_t)r.y * stride + (uint64_t)r.x * 4;
	dst = gpu->fb + (uint64_t)r.y * stride + (uint64_t)r.x * 4;
	for (i = 0; i < r.height; i++) {
		memcpy(dst, src, row_bytes);
		src += stride;
		dst += stride;
	}

	dirty.x = r.x;
	dirty.y = r.y;
	dirty.width = r.width;
	dirty.height = r.height;
	vnc_server_mark_dirty(&gpu->vnc, &dirty);

	viogpu_set_resp(&resp, req_hdr, VIRTIO_GPU_RESP_OK_NODATA);
	return viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
}

static int
viogpu_handle_cmd(struct virtio_dev *dev, struct virtio_vq_info *vq_info,
    struct virtio_vq_acct *acct)
{
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_ctrl_hdr resp;
	struct vring_desc *desc;
	uint64_t data_addr = 0;
	uint32_t data_len = 0;
	uint16_t idx;
	int ret;
	size_t chain;

	desc = acct->req_desc;
	if (desc == NULL || (desc->flags & VRING_DESC_F_WRITE))
		return 0;
	if (desc->len < sizeof(hdr))
		return 0;
	if (read_mem(desc->addr, &hdr, sizeof(hdr)))
		return 0;
	hdr.type = le32toh(hdr.type);
	hdr.flags = le32toh(hdr.flags);
	hdr.fence_id = le64toh(hdr.fence_id);
	hdr.ctx_id = le32toh(hdr.ctx_id);

	acct->resp_desc = NULL;
	if (desc->flags & VRING_DESC_F_WRITE)
		return 0;
	idx = acct->req_idx;
	for (chain = 0; chain < vq_info->qs; chain++) {
		if (desc->flags & VRING_DESC_F_WRITE) {
			acct->resp_desc = desc;
			break;
		}
		if (desc != acct->req_desc && data_addr == 0 && desc->len > 0) {
			data_addr = desc->addr;
			data_len = desc->len;
		}
		if (!(desc->flags & VRING_DESC_F_NEXT))
			break;
		idx = desc->next & vq_info->mask;
		desc = &acct->desc[idx];
	}

	switch (hdr.type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		return viogpu_handle_get_display_info(dev, vq_info, acct, &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: {
		struct virtio_gpu_resource_create_2d req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_resource_create_2d(dev, vq_info, acct, &hdr,
		    &req);
	}
	case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
		struct virtio_gpu_resource_unref req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_resource_unref(dev, vq_info, acct, &hdr, &req);
	}
	case VIRTIO_GPU_CMD_SET_SCANOUT: {
		struct virtio_gpu_set_scanout req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_set_scanout(dev, vq_info, acct, &hdr, &req);
	}
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
		struct virtio_gpu_resource_attach_backing req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		if (data_addr == 0 && acct->req_desc->len > sizeof(req)) {
			data_addr = acct->req_desc->addr + sizeof(req);
			data_len = acct->req_desc->len - sizeof(req);
		}
		return viogpu_handle_attach_backing(dev, vq_info, acct, &hdr, &req,
		    data_addr, data_len);
	}
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
		struct virtio_gpu_resource_detach_backing req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_detach_backing(dev, vq_info, acct, &hdr, &req);
	}
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: {
		struct virtio_gpu_transfer_to_host_2d req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_transfer(dev, vq_info, acct, &hdr, &req);
	}
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH: {
		struct virtio_gpu_resource_flush req;
		if (acct->req_desc->len < sizeof(req))
			break;
		if (read_mem(acct->req_desc->addr, &req, sizeof(req)))
			break;
		return viogpu_handle_flush(dev, vq_info, acct, &hdr, &req);
	}
	default:
		break;
	}

	viogpu_set_resp(&resp, &hdr, viogpu_resp_type(hdr.type));
	ret = viogpu_send_response(dev, vq_info, acct, &resp, sizeof(resp));
	return ret;
}

int
viogpu_notifyq(struct virtio_dev *dev, uint16_t vq_idx)
{
	struct virtio_vq_info *vq_info;
	struct virtio_vq_acct acct;
	char *vr;
	size_t cnt;
	int ret = 0;

	if (vq_idx >= dev->num_queues) {
		log_warnx("%s: invalid virtqueue index %u", __func__, vq_idx);
		return 0;
	}
	if (!dev->vq[vq_idx].vq_enabled) {
		log_warnx("%s: virtqueue not enabled", __func__);
		return 0;
	}

	vq_info = &dev->vq[vq_idx];
	vr = vq_info->q_hva;
	if (vr == NULL)
		fatalx("%s: null vring", __func__);

	memset(&acct, 0, sizeof(acct));
	acct.desc = (struct vring_desc *)vr;
	acct.avail = (struct vring_avail *)(vr + vq_info->vq_availoffset);
	acct.used = (struct vring_used *)(vr + vq_info->vq_usedoffset);
	acct.idx = vq_info->last_avail & vq_info->mask;

	if ((acct.avail->idx & vq_info->mask) == acct.idx)
		return 0;

	cnt = 0;
	while (acct.idx != (acct.avail->idx & vq_info->mask)) {
		if (++cnt >= vq_info->qs) {
			log_warnx("%s: invalid descriptor table", __func__);
			break;
		}

		acct.req_idx = acct.avail->ring[acct.idx] & vq_info->mask;
		acct.req_desc = &acct.desc[acct.req_idx];

		ret |= viogpu_handle_cmd(dev, vq_info, &acct);
		vq_info->last_avail = acct.avail->idx & vq_info->mask;
		acct.idx = vq_info->last_avail;
	}

	return ret;
}

int
viogpu_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr, void *arg,
    uint8_t sz)
{
	struct virtio_dev *dev = arg;
	struct viogpu_dev *gpu = &dev->viogpu;

	*intr = 0xFF;

	if (dir == VEI_DIR_IN) {
		switch (reg) {
		case offsetof(struct virtio_gpu_config, events_read):
			if (sz == 4)
				*data = le32toh(gpu->cfg.events_read);
			break;
		case offsetof(struct virtio_gpu_config, events_clear):
			if (sz == 4)
				*data = le32toh(gpu->cfg.events_clear);
			break;
		case offsetof(struct virtio_gpu_config, num_scanouts):
			if (sz == 4)
				*data = le32toh(gpu->cfg.num_scanouts);
			break;
		case offsetof(struct virtio_gpu_config, num_capsets):
			if (sz == 4)
				*data = le32toh(gpu->cfg.num_capsets);
			break;
		default:
			log_warnx("%s: invalid register 0x%04x", __func__, reg);
			break;
		}
	} else {
		switch (reg) {
		case offsetof(struct virtio_gpu_config, events_clear):
			if (sz == 4)
				gpu->cfg.events_clear = htole32(*data);
			break;
		default:
			log_warnx("%s: invalid register 0x%04x", __func__, reg);
			break;
		}
	}

	return 0;
}

int
viogpu_init(struct virtio_dev *dev, struct vmd_vm *vm)
{
	struct viogpu_dev *gpu = &dev->viogpu;
	uint64_t size;
	uint64_t stride;
	int ret;

	memset(gpu, 0, sizeof(*gpu));
	memset(&gpu->cfg, 0, sizeof(gpu->cfg));
	gpu->cfg.num_scanouts = 1;
	gpu->cfg.num_capsets = 0;
	gpu->width = vm->vm_params.vmc_gfx_width;
	gpu->height = vm->vm_params.vmc_gfx_height;

	stride = (uint64_t)gpu->width * 4;
	if (stride == 0 || stride > UINT32_MAX)
		return EINVAL;
	if (gpu->height > UINT64_MAX / stride)
		return EINVAL;
	size = stride * gpu->height;
	if (size == 0 || size > MB(64))
		return EINVAL;

	gpu->fb = calloc(1, size);
	if (gpu->fb == NULL)
		return ENOMEM;
	gpu->fb_size = size;

	ret = vnc_server_init(&gpu->vnc, gpu->width, gpu->height, gpu->fb,
	    (uint32_t)stride);
	if (ret != 0) {
		free(gpu->fb);
		gpu->fb = NULL;
		return ret;
	}

	return 0;
}

void
viogpu_reset(struct virtio_dev *dev)
{
	struct viogpu_dev *gpu = &dev->viogpu;

	while (gpu->nresources > 0)
		viogpu_resource_remove(gpu, gpu->resources[0].id);
	gpu->scanout_resource_id = 0;
}

void
viogpu_shutdown(struct virtio_dev *dev)
{
	struct viogpu_dev *gpu = &dev->viogpu;

	viogpu_reset(dev);
	vnc_server_shutdown(&gpu->vnc);
	free(gpu->fb);
	gpu->fb = NULL;
}
