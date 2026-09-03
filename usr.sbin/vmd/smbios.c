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
#include <sys/types.h>
#include <dev/vmm/vmm.h>

#include <errno.h>
#include <limits.h>
#include <sha1.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmd.h"
#include "smbios.h"

#define SMBIOS_TYPE_BIOS		0
#define SMBIOS_TYPE_SYSTEM		1
#define SMBIOS_TYPE_BASEBOARD		2
#define SMBIOS_TYPE_ENCLOSURE		3
#define SMBIOS_TYPE_PROCESSOR		4
#define SMBIOS_TYPE_PHYSMEM		16
#define SMBIOS_TYPE_MEMDEV		17
#define SMBIOS_TYPE_MEMARRAYMAP		19
#define SMBIOS_TYPE_MEMDEVMAP		20
#define SMBIOS_TYPE_BOOTINFO		32
#define SMBIOS_TYPE_END			127

#define SMBIOS_HANDLE_BIOS		0x0000
#define SMBIOS_HANDLE_SYSTEM		0x0100
#define SMBIOS_HANDLE_BASEBOARD		0x0200
#define SMBIOS_HANDLE_ENCLOSURE		0x0300
#define SMBIOS_HANDLE_PROCESSOR		0x0400
#define SMBIOS_HANDLE_PHYSMEM		0x1000
#define SMBIOS_HANDLE_MEMDEV		0x1100
#define SMBIOS_HANDLE_MEMARRAYMAP	0x1300
#define SMBIOS_HANDLE_MEMDEVMAP		0x1400
#define SMBIOS_HANDLE_BOOTINFO		0x2000
#define SMBIOS_HANDLE_END		0x7f00

#define SMBIOS_INVALID_HANDLE		0xffff
#define SMBIOS_NO_ERROR_HANDLE		0xfffe

struct smbios_header {
	uint8_t		type;
	uint8_t		length;
	uint16_t	handle;
} __packed;

/* SeaBIOS fills in the structure-table address and entry-point checksum. */
struct smbios_30_entry_point {
	uint8_t		anchor[5];
	uint8_t		checksum;
	uint8_t		length;
	uint8_t		major;
	uint8_t		minor;
	uint8_t		docrev;
	uint8_t		revision;
	uint8_t		reserved;
	uint32_t	table_max_size;
	uint64_t	table_address;
} __packed;

struct smbios_type0 {
	struct smbios_header	hdr;
	uint8_t		vendor;
	uint8_t		version;
	uint16_t	segment;
	uint8_t		release_date;
	uint8_t		rom_size;
	uint64_t	characteristics;
	uint8_t		characteristics_ext[2];
	uint8_t		bios_major;
	uint8_t		bios_minor;
	uint8_t		ec_major;
	uint8_t		ec_minor;
} __packed;

struct smbios_type1 {
	struct smbios_header	hdr;
	uint8_t		manufacturer;
	uint8_t		product;
	uint8_t		version;
	uint8_t		serial;
	uint8_t		uuid[16];
	uint8_t		wakeup;
	uint8_t		sku;
	uint8_t		family;
} __packed;

struct smbios_type2 {
	struct smbios_header	hdr;
	uint8_t		manufacturer;
	uint8_t		product;
	uint8_t		version;
	uint8_t		serial;
	uint8_t		asset;
	uint8_t		features;
	uint8_t		location;
	uint16_t	chassis_handle;
	uint8_t		board_type;
	uint8_t		contained_count;
} __packed;

struct smbios_type3 {
	struct smbios_header	hdr;
	uint8_t		manufacturer;
	uint8_t		chassis_type;
	uint8_t		version;
	uint8_t		serial;
	uint8_t		asset;
	uint8_t		boot_state;
	uint8_t		power_state;
	uint8_t		thermal_state;
	uint8_t		security_status;
	uint32_t	oem_defined;
	uint8_t		height;
	uint8_t		power_cords;
	uint8_t		contained_count;
	uint8_t		contained_length;
} __packed;

struct smbios_type4 {
	struct smbios_header	hdr;
	uint8_t		socket;
	uint8_t		processor_type;
	uint8_t		processor_family;
	uint8_t		manufacturer;
	uint32_t	processor_id_eax;
	uint32_t	processor_id_edx;
	uint8_t		processor_version;
	uint8_t		voltage;
	uint16_t	external_clock;
	uint16_t	max_speed;
	uint16_t	current_speed;
	uint8_t		status;
	uint8_t		upgrade;
	uint16_t	l1_handle;
	uint16_t	l2_handle;
	uint16_t	l3_handle;
	uint8_t		serial;
	uint8_t		asset;
	uint8_t		part;
	uint8_t		core_count;
	uint8_t		core_enabled;
	uint8_t		thread_count;
	uint16_t	characteristics;
	uint16_t	processor_family2;
	uint16_t	core_count2;
	uint16_t	core_enabled2;
	uint16_t	thread_count2;
} __packed;

struct smbios_type16 {
	struct smbios_header	hdr;
	uint8_t		location;
	uint8_t		use;
	uint8_t		error_correction;
	uint32_t	maximum_capacity;
	uint16_t	error_handle;
	uint16_t	device_count;
	uint64_t	extended_capacity;
} __packed;

struct smbios_type17 {
	struct smbios_header	hdr;
	uint16_t	array_handle;
	uint16_t	error_handle;
	uint16_t	total_width;
	uint16_t	data_width;
	uint16_t	size;
	uint8_t		form_factor;
	uint8_t		device_set;
	uint8_t		device_locator;
	uint8_t		bank_locator;
	uint8_t		memory_type;
	uint16_t	type_detail;
	uint16_t	speed;
	uint8_t		manufacturer;
	uint8_t		serial;
	uint8_t		asset;
	uint8_t		part;
	uint8_t		attributes;
	uint32_t	extended_size;
	uint16_t	configured_speed;
	uint16_t	minimum_voltage;
	uint16_t	maximum_voltage;
	uint16_t	configured_voltage;
} __packed;

struct smbios_type19 {
	struct smbios_header	hdr;
	uint32_t	start_kb;
	uint32_t	end_kb;
	uint16_t	array_handle;
	uint8_t		partition_width;
	uint64_t	extended_start;
	uint64_t	extended_end;
} __packed;

struct smbios_type20 {
	struct smbios_header	hdr;
	uint32_t	start_kb;
	uint32_t	end_kb;
	uint16_t	device_handle;
	uint16_t	array_map_handle;
	uint8_t		row_position;
	uint8_t		interleave_position;
	uint8_t		interleave_depth;
	uint64_t	extended_start;
	uint64_t	extended_end;
} __packed;

struct smbios_type32 {
	struct smbios_header	hdr;
	uint8_t		reserved[6];
	uint8_t		boot_status;
} __packed;

struct smbios_builder {
	uint8_t		*data;
	size_t		 len;
	size_t		 capacity;
};

static int	smbios_append(struct smbios_builder *, const void *, size_t);
static int	smbios_add_structure(struct smbios_builder *, const void *,
	    size_t, const char *const *, size_t);
static void	smbios_header_init(struct smbios_header *, uint8_t, size_t,
	    uint16_t);
static void	smbios_uuid(const struct vmop_create_params *, uint8_t *);
static void	smbios_mapped_address(uint64_t, uint64_t, uint32_t *,
	    uint32_t *, uint64_t *, uint64_t *);

_Static_assert(sizeof(struct smbios_30_entry_point) == 24,
    "invalid SMBIOS 3 entry point size");
_Static_assert(sizeof(struct smbios_type0) == 0x18,
    "invalid SMBIOS type 0 size");
_Static_assert(sizeof(struct smbios_type1) == 0x1b,
    "invalid SMBIOS type 1 size");
_Static_assert(sizeof(struct smbios_type2) == 0x0f,
    "invalid SMBIOS type 2 size");
_Static_assert(sizeof(struct smbios_type3) == 0x15,
    "invalid SMBIOS type 3 size");
_Static_assert(sizeof(struct smbios_type4) == 0x30,
    "invalid SMBIOS type 4 size");
_Static_assert(sizeof(struct smbios_type16) == 0x17,
    "invalid SMBIOS type 16 size");
_Static_assert(sizeof(struct smbios_type17) == 0x28,
    "invalid SMBIOS type 17 size");
_Static_assert(sizeof(struct smbios_type19) == 0x1f,
    "invalid SMBIOS type 19 size");
_Static_assert(sizeof(struct smbios_type20) == 0x23,
    "invalid SMBIOS type 20 size");
_Static_assert(sizeof(struct smbios_type32) == 0x0b,
    "invalid SMBIOS type 32 size");

static int
smbios_append(struct smbios_builder *b, const void *data, size_t len)
{
	uint8_t *p;
	size_t capacity, need;

	if (len > SIZE_MAX - b->len) {
		errno = EOVERFLOW;
		return -1;
	}
	need = b->len + len;
	if (need > b->capacity) {
		capacity = b->capacity == 0 ? 512 : b->capacity;
		while (capacity < need) {
			if (capacity > SIZE_MAX / 2) {
				capacity = need;
				break;
			}
			capacity *= 2;
		}
		if ((p = realloc(b->data, capacity)) == NULL)
			return -1;
		b->data = p;
		b->capacity = capacity;
	}
	memcpy(b->data + b->len, data, len);
	b->len = need;
	return 0;
}

static int
smbios_add_structure(struct smbios_builder *b, const void *formatted,
    size_t formatted_len, const char *const *strings, size_t nstrings)
{
	static const uint8_t nul[2];
	size_t i, len;

	if (smbios_append(b, formatted, formatted_len) == -1)
		return -1;
	for (i = 0; i < nstrings; i++) {
		len = strlen(strings[i]);
		if (len == 0 || len > 64) {
			errno = EINVAL;
			return -1;
		}
		if (smbios_append(b, strings[i], len + 1) == -1)
			return -1;
	}
	return smbios_append(b, nul, nstrings == 0 ? 2 : 1);
}

static void
smbios_header_init(struct smbios_header *hdr, uint8_t type, size_t length,
    uint16_t handle)
{
	hdr->type = type;
	hdr->length = (uint8_t)length;
	hdr->handle = htole16(handle);
}

/*
 * Derive a stable RFC 4122 version-5 UUID from the VM and instance names.
 * The first three fields use the little-endian representation required by
 * SMBIOS 2.6 and later.
 */
static void
smbios_uuid(const struct vmop_create_params *vmc, uint8_t *uuid)
{
	static const uint8_t dns_namespace[16] = {
		0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
		0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
	};
	static const uint8_t prefix[] = "openbsd.org/vmd/";
	SHA1_CTX ctx;
	uint8_t digest[SHA1_DIGEST_LENGTH];

	SHA1Init(&ctx);
	SHA1Update(&ctx, dns_namespace, sizeof(dns_namespace));
	SHA1Update(&ctx, prefix, sizeof(prefix) - 1);
	SHA1Update(&ctx, (const uint8_t *)vmc->vmc_name,
	    strnlen(vmc->vmc_name, sizeof(vmc->vmc_name)));
	SHA1Update(&ctx, (const uint8_t *)"/", 1);
	SHA1Update(&ctx, (const uint8_t *)vmc->vmc_instance,
	    strnlen(vmc->vmc_instance, sizeof(vmc->vmc_instance)));
	SHA1Final(digest, &ctx);

	digest[6] = (digest[6] & 0x0f) | 0x50;
	digest[8] = (digest[8] & 0x3f) | 0x80;
	uuid[0] = digest[3];
	uuid[1] = digest[2];
	uuid[2] = digest[1];
	uuid[3] = digest[0];
	uuid[4] = digest[5];
	uuid[5] = digest[4];
	uuid[6] = digest[7];
	uuid[7] = digest[6];
	memcpy(uuid + 8, digest + 8, 8);
	explicit_bzero(digest, sizeof(digest));
}

static void
smbios_mapped_address(uint64_t start, uint64_t size, uint32_t *start_kb,
    uint32_t *end_kb, uint64_t *extended_start, uint64_t *extended_end)
{
	uint64_t end = start + size - 1;

	if (start / 1024 < UINT32_MAX && end / 1024 < UINT32_MAX) {
		*start_kb = htole32(start / 1024);
		*end_kb = htole32(end / 1024);
		*extended_start = 0;
		*extended_end = 0;
	} else {
		*start_kb = htole32(UINT32_MAX);
		*end_kb = htole32(UINT32_MAX);
		*extended_start = htole64(start);
		*extended_end = htole64(end);
	}
}

int
smbios_build_tables(const struct vmop_create_params *vmc, uint8_t **tables,
    size_t *tables_len, uint8_t **anchor, size_t *anchor_len)
{
	struct smbios_builder b = { 0 };
	struct smbios_30_entry_point *ep = NULL;
	struct smbios_type0 t0 = { 0 };
	struct smbios_type1 t1 = { 0 };
	struct smbios_type2 t2 = { 0 };
	struct smbios_type3 t3 = { 0 };
	struct smbios_type4 t4 = { 0 };
	struct smbios_type16 t16 = { 0 };
	struct smbios_type17 t17 = { 0 };
	struct smbios_type19 t19;
	struct smbios_type20 t20;
	struct smbios_type32 t32 = { 0 };
	struct smbios_header end = { 0 };
	const char *type0_strings[] = {
		"OpenBSD", "vmd virtual firmware", "01/01/2026"
	};
	const char *type1_strings[6], *type2_strings[6];
	const char *type3_strings[4], *type17_strings[6];
	const char *type4_strings[] = {
		"CPU 0", "OpenBSD", "vmm virtual CPU", "None", "None",
		"Virtual CPU"
	};
	char identity[65];
	uint64_t total_memory = 0, memory_mb, memory_kb;
	size_t i, ram_ranges = 0;
	uint8_t count;

	*tables = NULL;
	*tables_len = 0;
	*anchor = NULL;
	*anchor_len = 0;
	if (vmc->vmc_ncpus == 0 || vmc->vmc_ncpus > UINT16_MAX) {
		errno = EINVAL;
		return -1;
	}
	if (vmc->vmc_nmemranges > nitems(vmc->vmc_memranges)) {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < vmc->vmc_nmemranges; i++) {
		if (vmc->vmc_memranges[i].vmr_type != VM_MEM_RAM ||
		    vmc->vmc_memranges[i].vmr_size == 0)
			continue;
		if (vmc->vmc_memranges[i].vmr_gpa > UINT64_MAX -
		    (vmc->vmc_memranges[i].vmr_size - 1)) {
			errno = EOVERFLOW;
			goto fail;
		}
		if (vmc->vmc_memranges[i].vmr_size >
		    UINT64_MAX - total_memory) {
			errno = EOVERFLOW;
			goto fail;
		}
		total_memory += vmc->vmc_memranges[i].vmr_size;
		ram_ranges++;
	}
	if (total_memory == 0 || ram_ranges > UINT16_MAX) {
		errno = EINVAL;
		goto fail;
	}

	if (vmc->vmc_instance[0] != '\0')
		strlcpy(identity, vmc->vmc_instance, sizeof(identity));
	else
		strlcpy(identity, vmc->vmc_name, sizeof(identity));
	if (identity[0] == '\0')
		strlcpy(identity, "ephemeral", sizeof(identity));

	smbios_header_init(&t0.hdr, SMBIOS_TYPE_BIOS, sizeof(t0),
	    SMBIOS_HANDLE_BIOS);
	t0.vendor = 1;
	t0.version = 2;
	t0.segment = htole16(0xe800);
	t0.release_date = 3;
	t0.characteristics = htole64(1ULL << 3);
	t0.characteristics_ext[1] = vmc->vmc_firmware == VMFW_UEFI ?
	    0x1c : 0x14;
	t0.ec_major = 0xff;
	t0.ec_minor = 0xff;
	if (smbios_add_structure(&b, &t0, sizeof(t0), type0_strings,
	    nitems(type0_strings)) == -1)
		goto fail;

	smbios_header_init(&t1.hdr, SMBIOS_TYPE_SYSTEM, sizeof(t1),
	    SMBIOS_HANDLE_SYSTEM);
	t1.manufacturer = 1;
	t1.product = 2;
	t1.version = 3;
	t1.serial = 4;
	smbios_uuid(vmc, t1.uuid);
	t1.wakeup = 0x06;
	t1.sku = 5;
	t1.family = 6;
	type1_strings[0] = "OpenBSD";
	type1_strings[1] = "vmm virtual machine";
	type1_strings[2] = "1.0";
	type1_strings[3] = identity;
	type1_strings[4] = "Virtual Machine";
	type1_strings[5] = "Virtual Machine";
	if (smbios_add_structure(&b, &t1, sizeof(t1), type1_strings,
	    nitems(type1_strings)) == -1)
		goto fail;

	smbios_header_init(&t2.hdr, SMBIOS_TYPE_BASEBOARD, sizeof(t2),
	    SMBIOS_HANDLE_BASEBOARD);
	t2.manufacturer = 1;
	t2.product = 2;
	t2.version = 3;
	t2.serial = 4;
	t2.asset = 5;
	t2.features = 0x01;
	t2.location = 6;
	t2.chassis_handle = htole16(SMBIOS_HANDLE_ENCLOSURE);
	t2.board_type = 0x0a;
	type2_strings[0] = "OpenBSD";
	type2_strings[1] = "vmm virtual board";
	type2_strings[2] = "1.0";
	type2_strings[3] = identity;
	type2_strings[4] = "None";
	type2_strings[5] = "Virtual Machine";
	if (smbios_add_structure(&b, &t2, sizeof(t2), type2_strings,
	    nitems(type2_strings)) == -1)
		goto fail;

	smbios_header_init(&t3.hdr, SMBIOS_TYPE_ENCLOSURE, sizeof(t3),
	    SMBIOS_HANDLE_ENCLOSURE);
	t3.manufacturer = 1;
	t3.chassis_type = 0x03;
	t3.version = 2;
	t3.serial = 3;
	t3.asset = 4;
	t3.boot_state = 0x03;
	t3.power_state = 0x03;
	t3.thermal_state = 0x03;
	t3.security_status = 0x02;
	type3_strings[0] = "OpenBSD";
	type3_strings[1] = "vmm";
	type3_strings[2] = identity;
	type3_strings[3] = "None";
	if (smbios_add_structure(&b, &t3, sizeof(t3), type3_strings,
	    nitems(type3_strings)) == -1)
		goto fail;

	smbios_header_init(&t4.hdr, SMBIOS_TYPE_PROCESSOR, sizeof(t4),
	    SMBIOS_HANDLE_PROCESSOR);
	t4.socket = 1;
	t4.processor_type = 0x03;
	t4.processor_family = 0xfe;
	t4.manufacturer = 2;
	t4.processor_version = 3;
	t4.max_speed = htole16(2000);
	t4.current_speed = htole16(2000);
	t4.status = 0x41;
	t4.upgrade = 0x01;
	t4.l1_handle = htole16(SMBIOS_INVALID_HANDLE);
	t4.l2_handle = htole16(SMBIOS_INVALID_HANDLE);
	t4.l3_handle = htole16(SMBIOS_INVALID_HANDLE);
	t4.serial = 4;
	t4.asset = 5;
	t4.part = 6;
	count = vmc->vmc_ncpus > UINT8_MAX ? UINT8_MAX :
	    (uint8_t)vmc->vmc_ncpus;
	t4.core_count = count;
	t4.core_enabled = count;
	t4.thread_count = count;
	t4.characteristics = htole16(0x0002);
	t4.processor_family2 = htole16(0x0001);
	t4.core_count2 = htole16((uint16_t)vmc->vmc_ncpus);
	t4.core_enabled2 = htole16((uint16_t)vmc->vmc_ncpus);
	t4.thread_count2 = htole16((uint16_t)vmc->vmc_ncpus);
	if (smbios_add_structure(&b, &t4, sizeof(t4), type4_strings,
	    nitems(type4_strings)) == -1)
		goto fail;

	memory_kb = total_memory / 1024 + (total_memory % 1024 != 0);
	smbios_header_init(&t16.hdr, SMBIOS_TYPE_PHYSMEM, sizeof(t16),
	    SMBIOS_HANDLE_PHYSMEM);
	t16.location = 0x01;
	t16.use = 0x03;
	/* Match the virtual-system profile expected by Microsoft guests. */
	t16.error_correction = 0x06;
	if (memory_kb < 0x80000000ULL)
		t16.maximum_capacity = htole32((uint32_t)memory_kb);
	else {
		t16.maximum_capacity = htole32(0x80000000U);
		t16.extended_capacity = htole64(total_memory);
	}
	t16.error_handle = htole16(SMBIOS_NO_ERROR_HANDLE);
	t16.device_count = htole16(1);
	if (smbios_add_structure(&b, &t16, sizeof(t16), NULL, 0) == -1)
		goto fail;

	memory_mb = total_memory / (1024 * 1024) +
	    (total_memory % (1024 * 1024) != 0);
	smbios_header_init(&t17.hdr, SMBIOS_TYPE_MEMDEV, sizeof(t17),
	    SMBIOS_HANDLE_MEMDEV);
	t17.array_handle = htole16(SMBIOS_HANDLE_PHYSMEM);
	t17.error_handle = htole16(SMBIOS_NO_ERROR_HANDLE);
	t17.total_width = htole16(UINT16_MAX);
	t17.data_width = htole16(UINT16_MAX);
	if (memory_mb < 0x7fff)
		t17.size = htole16((uint16_t)memory_mb);
	else {
		t17.size = htole16(0x7fff);
		t17.extended_size = htole32((uint32_t)memory_mb);
	}
	t17.form_factor = 0x09;
	t17.device_locator = 1;
	t17.bank_locator = 2;
	t17.memory_type = 0x07;
	t17.type_detail = htole16(0x0002);
	t17.manufacturer = 3;
	t17.serial = 4;
	t17.asset = 5;
	t17.part = 6;
	type17_strings[0] = "DIMM 0";
	type17_strings[1] = "BANK 0";
	type17_strings[2] = "OpenBSD";
	type17_strings[3] = identity;
	type17_strings[4] = "None";
	type17_strings[5] = "Virtual RAM";
	if (smbios_add_structure(&b, &t17, sizeof(t17), type17_strings,
	    nitems(type17_strings)) == -1)
		goto fail;

	for (i = 0, ram_ranges = 0; i < vmc->vmc_nmemranges; i++) {
		const struct vm_mem_range *range = &vmc->vmc_memranges[i];

		if (range->vmr_type != VM_MEM_RAM || range->vmr_size == 0)
			continue;
		memset(&t19, 0, sizeof(t19));
		smbios_header_init(&t19.hdr, SMBIOS_TYPE_MEMARRAYMAP,
		    sizeof(t19), SMBIOS_HANDLE_MEMARRAYMAP + (uint16_t)ram_ranges);
		smbios_mapped_address(range->vmr_gpa, range->vmr_size,
		    &t19.start_kb, &t19.end_kb, &t19.extended_start,
		    &t19.extended_end);
		t19.array_handle = htole16(SMBIOS_HANDLE_PHYSMEM);
		t19.partition_width = 1;
		if (smbios_add_structure(&b, &t19, sizeof(t19), NULL, 0) == -1)
			goto fail;

		memset(&t20, 0, sizeof(t20));
		smbios_header_init(&t20.hdr, SMBIOS_TYPE_MEMDEVMAP,
		    sizeof(t20), SMBIOS_HANDLE_MEMDEVMAP + (uint16_t)ram_ranges);
		smbios_mapped_address(range->vmr_gpa, range->vmr_size,
		    &t20.start_kb, &t20.end_kb, &t20.extended_start,
		    &t20.extended_end);
		t20.device_handle = htole16(SMBIOS_HANDLE_MEMDEV);
		t20.array_map_handle = htole16(SMBIOS_HANDLE_MEMARRAYMAP +
		    (uint16_t)ram_ranges);
		t20.row_position = 1;
		if (smbios_add_structure(&b, &t20, sizeof(t20), NULL, 0) == -1)
			goto fail;
		ram_ranges++;
	}

	smbios_header_init(&t32.hdr, SMBIOS_TYPE_BOOTINFO, sizeof(t32),
	    SMBIOS_HANDLE_BOOTINFO);
	if (smbios_add_structure(&b, &t32, sizeof(t32), NULL, 0) == -1)
		goto fail;

	smbios_header_init(&end, SMBIOS_TYPE_END, sizeof(end),
	    SMBIOS_HANDLE_END);
	if (smbios_add_structure(&b, &end, sizeof(end), NULL, 0) == -1)
		goto fail;

	if (b.len > UINT32_MAX) {
		errno = EOVERFLOW;
		goto fail;
	}
	if ((ep = calloc(1, sizeof(*ep))) == NULL)
		goto fail;
	memcpy(ep->anchor, "_SM3_", sizeof(ep->anchor));
	ep->length = sizeof(*ep);
	ep->major = 3;
	ep->minor = 0;
	ep->revision = 1;
	ep->table_max_size = htole32((uint32_t)b.len);

	*tables = b.data;
	*tables_len = b.len;
	*anchor = (uint8_t *)ep;
	*anchor_len = sizeof(*ep);
	return 0;

fail:
	free(b.data);
	free(ep);
	return -1;
}
