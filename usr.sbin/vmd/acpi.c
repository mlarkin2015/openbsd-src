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

#include <stdlib.h>
#include <string.h>

#include <machine/i82093reg.h>
#include <machine/i82489reg.h>
#include <sys/types.h>

#include "acpi.h"
#include "vmd.h"

/*
 * acpi_calculate_checksum
 *
 * calculates a checksum value for the table at 'table' with size 'len'.
 */
uint8_t
acpi_calculate_checksum(uint8_t *tbl, size_t len)
{
	uint8_t cksum;
	size_t i;

	cksum = 0;
	for (i = 0; i < len; i++)
		cksum += tbl[i];

	return (256 - cksum);
}

/*
 * acpi_populate_header
 *
 * populates the table header in 'hdr' with default values. The OEM table
 * id is provided in 'oemtableid'.
 *
 * Caller is responsible for later populating the header length, checksum,
 * and signature fields
 */
void
acpi_populate_header(struct acpi_table_header *hdr, uint8_t *oemtableid)
{
	/* Header */
	hdr->revision = 1;
	memcpy(hdr->oemid, VMD_OEMID, 6);
	memcpy(hdr->oemtableid, oemtableid, 8);
	hdr->oemrevision = 1;
	memcpy(hdr->aslcompilerid, VMD_ASLCOMPILER_ID, 4);
	hdr->aslcompilerrevision = 1;
}

/*
 * acpi_create_madt
 *
 * create MADT table at 'pa' based on the supplied parameters
 */
void
acpi_create_madt(paddr_t pa, size_t numcpu)
{
	struct acpi_madt *madt;
	struct acpi_madt_lapic *lapic;
	struct acpi_madt_ioapic *ioapic;
	uint8_t *tbl, *b;
	size_t i, sz;

	log_warnx("%s: creating MADT", __func__);
	sz = sizeof(*madt) + sizeof(*ioapic) + sizeof(*lapic) * numcpu;
	log_warnx("%s: MADT size %zd", __func__, sz);
	tbl = (uint8_t *)malloc(sz);
	if (tbl == NULL)
		fatal("malloc");

	b = tbl;
	madt = (struct acpi_madt *)b;

	b += sizeof(*madt);
	ioapic = (struct acpi_madt_ioapic *)b;

	b += sizeof(*ioapic);
	lapic = (struct acpi_madt_lapic *)b;

	acpi_populate_header(&madt->hdr, VMD_MADT_OEM_TABLEID);
	madt->local_apic_address = LAPIC_BASE;
	madt->flags = ACPI_APIC_PCAT_COMPAT;

	for (i = 0; i < numcpu; i++) {
		lapic[i].apic_type = ACPI_MADT_LAPIC;
		lapic[i].length = sizeof(lapic[i]);
		lapic[i].acpi_proc_id = i;
		lapic[i].apic_id = i;
		lapic[i].flags = ACPI_PROC_ENABLE;
	}

	ioapic->apic_type = ACPI_MADT_IOAPIC;
	ioapic->length = sizeof(*ioapic);
	ioapic->reserved = 0;
	ioapic->address = IOAPIC_BASE_DEFAULT;
	ioapic->global_int_base = 0;

	memcpy(madt->hdr_signature, MADT_SIG, 4);
	madt->hdr.length = sz;
	madt->hdr.checksum = acpi_calculate_checksum(tbl, sz);

	log_warnx("%s: writing MADT to %lx", __func__, pa);

	if (write_mem(pa, tbl, sz))
		log_warnx("%s: could not write MADT table", __func__);
	free(tbl);

	log_warnx("%s: MADT creation complete", __func__);
}

/*
 * acpi_create_xsdt
 *
 * create XSDT table at 'pa' with pointers to the tables provided
 * in 'tables'. The 'numtables' parameter represents the number of
 * tables to include.
 */
void
acpi_create_xsdt(paddr_t pa, paddr_t *tables, size_t numtables)
{
	struct acpi_xsdt *xsdt;
	size_t i, sz;

	log_warnx("%s: creating XSDT", __func__);
	sz = sizeof(*xsdt) + ((numtables - 1) * sizeof(paddr_t));
	log_warnx("%s: XSDT size %zd", __func__, sz);

	xsdt = (struct acpi_xsdt *)malloc(sz);

	/* XXX unwind properly dont call fatal */
	if (xsdt == NULL)
		fatal("malloc");

	memset(xsdt, 0, sizeof(*xsdt));

	acpi_populate_header(&xsdt->hdr, VMD_XSDT_OEM_TABLEID);
	for (i = 0; i < numtables; i++)
		xsdt->table_offsets[i] = tables[i];

	memcpy(xsdt->hdr_signature, XSDT_SIG, 4);
	xsdt->hdr.length = sz;
	xsdt->hdr.checksum = acpi_calculate_checksum((uint8_t *)xsdt, sz);

	log_warnx("%s: writing XSDT to %lx", __func__, pa);
	if (write_mem(pa, xsdt, sizeof(xsdt)))
		log_warnx("%s: could not write XSDT table", __func__);

	log_warnx("%s: XSDT creation complete", __func__);
	free(xsdt);
}

/*
 * acpi_create_rsdp
 *
 * create RSDP table at 'pa' with a pointer to the XSDT located at 'xsdt_pa'.
 */
void
acpi_create_rsdp(paddr_t pa, paddr_t xsdt_pa)
{
	struct acpi_rsdp rsdp;

	log_warnx("%s: creating RSDP", __func__);
	memset(&rsdp, 0, sizeof(rsdp));

	/* RSDP v1 fields */
	memcpy(&rsdp.rsdp_signature, RSDP_SIG, 8);
	memcpy(&rsdp.rsdp_oemid, VMD_OEMID, 6);
	rsdp.rsdp_revision = 2;
	rsdp.rsdp_rsdt = 0;

	/* RSDP v2 fields */
	rsdp.rsdp_length = sizeof(rsdp);
	rsdp.rsdp_xsdt = VMD_XSDT_PADDR;

	/* Checksums */
	rsdp.rsdp_checksum = acpi_calculate_checksum((uint8_t *)&rsdp.rsdp1,
	    sizeof(rsdp.rsdp1));

	rsdp.rsdp_extchecksum = acpi_calculate_checksum((uint8_t *)&rsdp,
	    sizeof(rsdp));

	log_warnx("%s: writing RSDP to %lx", __func__, pa);
	if (write_mem(pa, &rsdp, sizeof(rsdp)))
		log_warnx("%s: could not write RSDP table", __func__);

	log_warnx("%s: RSDP creation complete", __func__);
}

void
acpi_init(size_t numcpu)
{
	paddr_t tables[1];
	size_t numtables;
	uint16_t rsdp_ptr_real;

	log_warnx("%s: initializing acpi tables", __func__);
	rsdp_ptr_real = VMD_RSDP_PADDR >> 4;

	numtables = 0;

	acpi_create_madt(VMD_MADT_PADDR, numcpu);
	tables[0] = VMD_MADT_PADDR;
	numtables++;

	acpi_create_xsdt(VMD_XSDT_PADDR, tables, numtables);
	acpi_create_rsdp(VMD_RSDP_PADDR, VMD_XSDT_PADDR);

	/* EBDA pointer */
	if (write_mem(VMD_ACPI_EBDA_PTR , &rsdp_ptr_real, sizeof(rsdp_ptr_real)))
		log_warnx("%s: could not write RSDP pointer to BDA", __func__);

}
