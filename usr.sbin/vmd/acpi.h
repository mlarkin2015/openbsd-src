/*	$OpenBSD */

/*
 * Copyright (c) 2025 Mike Larkin <mlarkin@openbsd.org>
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

#ifndef _ACPI_H_
#define _ACPI_H_

#include <dev/acpi/acpireg.h>

#define VMD_ACPI_EBDA_PTR	0x040E

#define VMD_OEMID		"VMD   "
#define VMD_XSDT_OEM_TABLEID	"VMD XSDT"
#define VMD_MADT_OEM_TABLEID	"VMD MADT"
#define VMD_ASLCOMPILER_ID	"VMD "

#define VMD_RSDP_PADDR		0x9D000
#define VMD_XSDT_PADDR		0x9E000
#define VMD_MADT_PADDR		0x9F000

uint8_t acpi_calculate_checksum(uint8_t *, size_t);
void acpi_populate_header(struct acpi_table_header *, uint8_t *);
void acpi_create_madt(paddr_t, size_t);
void acpi_create_xsdt(paddr_t, paddr_t *, size_t);
void acpi_create_rsdp(paddr_t, paddr_t);
void acpi_init(size_t);

#endif /* !_ACPI_H_ */
