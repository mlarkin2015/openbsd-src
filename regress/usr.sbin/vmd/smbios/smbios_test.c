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
#include <dev/vmm/vmm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vmd.h"
#include "smbios.h"

#define CHECK(_cond) do {						\
	if (!(_cond)) {						\
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", 	\
		    __func__, __LINE__, #_cond);			\
		exit(1);						\
	}							\
} while (0)

struct record {
	const uint8_t	*data;
	size_t		 length;
};

static uint16_t
getle16(const uint8_t *p)
{
	uint16_t value;

	memcpy(&value, p, sizeof(value));
	return letoh16(value);
}

static uint32_t
getle32(const uint8_t *p)
{
	uint32_t value;

	memcpy(&value, p, sizeof(value));
	return letoh32(value);
}

static int
next_record(const uint8_t *tables, size_t tables_len, size_t *offset,
    struct record *record)
{
	size_t end, formatted;

	if (*offset == tables_len)
		return 0;
	CHECK(tables_len - *offset >= 4);
	formatted = tables[*offset + 1];
	CHECK(formatted >= 4);
	CHECK(formatted <= tables_len - *offset);
	end = *offset + formatted;
	while (end + 1 < tables_len &&
	    (tables[end] != 0 || tables[end + 1] != 0))
		end++;
	CHECK(end + 1 < tables_len);
	record->data = tables + *offset;
	record->length = end + 2 - *offset;
	*offset = end + 2;
	return 1;
}

static struct record
find_record(const uint8_t *tables, size_t tables_len, uint8_t type,
    size_t instance)
{
	struct record record;
	size_t offset = 0;

	while (next_record(tables, tables_len, &offset, &record)) {
		if (record.data[0] == type && instance-- == 0)
			return record;
	}
	memset(&record, 0, sizeof(record));
	return record;
}

static void
setup_vmc(struct vmop_create_params *vmc)
{
	memset(vmc, 0, sizeof(*vmc));
	strlcpy(vmc->vmc_name, "smbios-test", sizeof(vmc->vmc_name));
	vmc->vmc_ncpus = 4;
	vmc->vmc_firmware = VMFW_UEFI;
	vmc->vmc_nmemranges = 4;
	vmc->vmc_memranges[0].vmr_gpa = 0;
	vmc->vmc_memranges[0].vmr_size = 576 * 1024;
	vmc->vmc_memranges[0].vmr_type = VM_MEM_RAM;
	vmc->vmc_memranges[1].vmr_gpa = 576 * 1024;
	vmc->vmc_memranges[1].vmr_size = 448 * 1024;
	vmc->vmc_memranges[1].vmr_type = VM_MEM_RESERVED;
	vmc->vmc_memranges[2].vmr_gpa = 1024 * 1024;
	vmc->vmc_memranges[2].vmr_size = 2047ULL * 1024 * 1024;
	vmc->vmc_memranges[2].vmr_type = VM_MEM_RAM;
	vmc->vmc_memranges[3].vmr_gpa = 4ULL * 1024 * 1024 * 1024;
	vmc->vmc_memranges[3].vmr_size = 1024ULL * 1024 * 1024;
	vmc->vmc_memranges[3].vmr_type = VM_MEM_RAM;
}

static void
test_tables(void)
{
	struct vmop_create_params vmc;
	struct record record, type1, type4;
	uint8_t *tables, *anchor, *tables2, *anchor2;
	size_t tables_len, anchor_len, tables2_len, anchor2_len;
	size_t count[256] = { 0 }, offset = 0, records = 0;

	setup_vmc(&vmc);
	CHECK(smbios_build_tables(&vmc, &tables, &tables_len, &anchor,
	    &anchor_len) == 0);
	CHECK(anchor_len == 24);
	CHECK(memcmp(anchor, "_SM3_", 5) == 0);
	CHECK(anchor[6] == anchor_len);
	CHECK(anchor[7] == 3 && anchor[8] == 0);
	CHECK(getle32(anchor + 12) == tables_len);

	while (next_record(tables, tables_len, &offset, &record)) {
		count[record.data[0]]++;
		records++;
	}
	CHECK(offset == tables_len);
	CHECK(records == 15);
	CHECK(count[0] == 1 && count[1] == 1 && count[2] == 1);
	CHECK(count[3] == 1 && count[4] == 1);
	CHECK(count[16] == 1 && count[17] == 1);
	CHECK(count[19] == 3 && count[20] == 3);
	CHECK(count[32] == 1 && count[127] == 1);
	CHECK(record.data[0] == 127);

	type4 = find_record(tables, tables_len, 4, 0);
	CHECK(type4.data != NULL && type4.data[1] == 0x30);
	CHECK(type4.data[6] == 0xfe);
	CHECK(type4.data[0x23] == 4);
	CHECK(type4.data[0x24] == 4);
	CHECK(type4.data[0x25] == 4);
	CHECK(getle16(type4.data + 0x2a) == 4);
	CHECK(getle16(type4.data + 0x2c) == 4);
	CHECK(getle16(type4.data + 0x2e) == 4);
	CHECK(getle16(type4.data + 0x28) == 1);

	type1 = find_record(tables, tables_len, 1, 0);
	CHECK(type1.data != NULL && type1.data[1] == 0x1b);
	CHECK((type1.data[15] & 0xf0) == 0x50);
	CHECK((type1.data[16] & 0xc0) == 0x80);

	CHECK(smbios_build_tables(&vmc, &tables2, &tables2_len, &anchor2,
	    &anchor2_len) == 0);
	CHECK(tables2_len == tables_len && anchor2_len == anchor_len);
	CHECK(memcmp(tables2, tables, tables_len) == 0);
	free(tables2);
	free(anchor2);

	strlcpy(vmc.vmc_name, "smbios-test-2", sizeof(vmc.vmc_name));
	CHECK(smbios_build_tables(&vmc, &tables2, &tables2_len, &anchor2,
	    &anchor2_len) == 0);
	record = find_record(tables2, tables2_len, 1, 0);
	CHECK(record.data != NULL);
	CHECK(memcmp(type1.data + 8, record.data + 8, 16) != 0);
	free(tables2);
	free(anchor2);
	free(tables);
	free(anchor);
}

static void
test_extended_memory(void)
{
	struct vmop_create_params vmc;
	struct record type17;
	uint8_t *tables, *anchor;
	size_t tables_len, anchor_len;

	setup_vmc(&vmc);
	vmc.vmc_nmemranges = 1;
	vmc.vmc_memranges[0].vmr_gpa = 0;
	vmc.vmc_memranges[0].vmr_size = 64ULL * 1024 * 1024 * 1024;
	vmc.vmc_memranges[0].vmr_type = VM_MEM_RAM;
	CHECK(smbios_build_tables(&vmc, &tables, &tables_len, &anchor,
	    &anchor_len) == 0);
	type17 = find_record(tables, tables_len, 17, 0);
	CHECK(type17.data != NULL);
	CHECK(getle16(type17.data + 0x0c) == 0x7fff);
	CHECK(getle32(type17.data + 0x1c) == 65536);
	free(tables);
	free(anchor);
}

static void
test_invalid(void)
{
	struct vmop_create_params vmc;
	uint8_t *tables = (uint8_t *)1, *anchor = (uint8_t *)1;
	size_t tables_len = 1, anchor_len = 1;

	setup_vmc(&vmc);
	vmc.vmc_nmemranges = 0;
	CHECK(smbios_build_tables(&vmc, &tables, &tables_len, &anchor,
	    &anchor_len) == -1);
	CHECK(tables == NULL && anchor == NULL);
	CHECK(tables_len == 0 && anchor_len == 0);

	setup_vmc(&vmc);
	vmc.vmc_nmemranges = nitems(vmc.vmc_memranges) + 1;
	CHECK(smbios_build_tables(&vmc, &tables, &tables_len, &anchor,
	    &anchor_len) == -1);
	CHECK(tables == NULL && anchor == NULL);

	setup_vmc(&vmc);
	vmc.vmc_nmemranges = 1;
	vmc.vmc_memranges[0].vmr_gpa = UINT64_MAX - 1;
	vmc.vmc_memranges[0].vmr_size = 4;
	CHECK(smbios_build_tables(&vmc, &tables, &tables_len, &anchor,
	    &anchor_len) == -1);
	CHECK(tables == NULL && anchor == NULL);
}

int
main(void)
{
	test_tables();
	test_extended_memory();
	test_invalid();
	return 0;
}
