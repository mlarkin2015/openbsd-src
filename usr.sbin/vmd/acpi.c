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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <machine/i82093reg.h>
#include <machine/i82489reg.h>
#include <sys/types.h>

#include "acpi.h"
#include "fw_cfg.h"
#include "vmd.h"

struct acpi_pm1 {
	pthread_mutex_t	mtx;
	uint16_t	status;
	uint16_t	enable;
	uint16_t	control;
};

static struct acpi_pm1 acpi_pm1 = {
	.mtx = PTHREAD_MUTEX_INITIALIZER
};

#define VMD_PM1_ENABLE_MASK	(ACPI_PM1_TMR_EN | ACPI_PM1_GBL_EN | \
	ACPI_PM1_PWRBTN_EN | ACPI_PM1_SLPBTN_EN | ACPI_PM1_RTC_EN | \
	ACPI_PM1_PCIEXP_WAKE_DIS)
#define VMD_PM1_CONTROL_MASK	(ACPI_PM1_SCI_EN | ACPI_PM1_BM_RLD | \
	ACPI_PM1_GBL_RLS | ACPI_PM1_SLP_TYPX_MASK | ACPI_PM1_SLP_EN)

void
acpi_pm1_init(void)
{
	mutex_lock(&acpi_pm1.mtx);
	acpi_pm1.status = 0;
	acpi_pm1.enable = 0;
	/* There is no SMI command interface; firmware leaves ACPI mode on. */
	acpi_pm1.control = ACPI_PM1_SCI_EN;
	mutex_unlock(&acpi_pm1.mtx);
}

static uint8_t
acpi_pm1_read_byte(uint16_t port)
{
	if (port >= VMD_PM1A_EVT_BASE &&
	    port < VMD_PM1A_EVT_BASE + sizeof(acpi_pm1.status))
		return acpi_pm1.status >> ((port - VMD_PM1A_EVT_BASE) * 8);
	if (port >= VMD_PM1A_EVT_BASE + sizeof(acpi_pm1.status) &&
	    port < VMD_PM1A_EVT_BASE + VMD_PM1A_EVT_LEN)
		return acpi_pm1.enable >>
		    ((port - VMD_PM1A_EVT_BASE - sizeof(acpi_pm1.status)) * 8);
	if (port >= VMD_PM1A_CNT_BASE &&
	    port < VMD_PM1A_CNT_BASE + VMD_PM1A_CNT_LEN)
		return acpi_pm1.control >> ((port - VMD_PM1A_CNT_BASE) * 8);

	return 0xff;
}

static void
acpi_pm1_write_byte(uint16_t port, uint8_t data)
{
	uint16_t mask;

	if (port >= VMD_PM1A_EVT_BASE &&
	    port < VMD_PM1A_EVT_BASE + sizeof(acpi_pm1.status)) {
		mask = (uint16_t)data << ((port - VMD_PM1A_EVT_BASE) * 8);
		/* PM1 status bits are write-one-to-clear. */
		acpi_pm1.status &= ~(mask & ACPI_PM1_ALL_STS);
	} else if (port >= VMD_PM1A_EVT_BASE + sizeof(acpi_pm1.status) &&
	    port < VMD_PM1A_EVT_BASE + VMD_PM1A_EVT_LEN) {
		mask = 0xffU <<
		    ((port - VMD_PM1A_EVT_BASE - sizeof(acpi_pm1.status)) * 8);
		acpi_pm1.enable = (acpi_pm1.enable & ~mask) |
		    ((uint16_t)data <<
		    ((port - VMD_PM1A_EVT_BASE - sizeof(acpi_pm1.status)) * 8));
		acpi_pm1.enable &= VMD_PM1_ENABLE_MASK;
	} else if (port >= VMD_PM1A_CNT_BASE &&
	    port < VMD_PM1A_CNT_BASE + VMD_PM1A_CNT_LEN) {
		mask = 0xffU << ((port - VMD_PM1A_CNT_BASE) * 8);
		acpi_pm1.control = (acpi_pm1.control & ~mask) |
		    ((uint16_t)data << ((port - VMD_PM1A_CNT_BASE) * 8));
		acpi_pm1.control &= VMD_PM1_CONTROL_MASK;
	}
}

uint8_t
vcpu_exit_acpi_pm1(struct vm_run_params *vrp)
{
	struct vm_exit *vei = vrp->vrp_exit;
	uint32_t data = 0;
	uint16_t port;
	uint8_t i;

	mutex_lock(&acpi_pm1.mtx);
	if (vei->vei.vei_dir == VEI_DIR_IN) {
		for (i = 0; i < vei->vei.vei_size; i++)
			data |= (uint32_t)acpi_pm1_read_byte(vei->vei.vei_port + i)
			    << (i * 8);
		set_return_data(vei, data);
	} else {
		port = vei->vei.vei_port;
		for (i = 0; i < vei->vei.vei_size; i++)
			acpi_pm1_write_byte(port + i,
			    (uint8_t)(vei->vei.vei_data >> (i * 8)));
	}
	mutex_unlock(&acpi_pm1.mtx);

	return 0xff;
}

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

static void
acpi_verify_checksum(uint8_t *tbl, size_t len)
{
	size_t i;
	uint8_t cksum = 0;

	for (i = 0 ; i < len ; i++)
		cksum += tbl[i];

	if (cksum)
		log_warnx("%s: ACPI table checksum error, got %d", __func__,
		    cksum);
}

/*
 * acpi_load_table
 *
 * Load an ACPI table from the specified file path into memory at 'pa'.
 * Returns the size of the loaded table on success, or -1 on failure.
 */
static ssize_t
acpi_load_table(const char *path, paddr_t pa)
{
	int fd;
	ssize_t n;
	size_t total = 0;
	struct stat sb;
	void *buf = NULL;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		log_warn("%s: cannot open %s", __func__, path);
		return (-1);
	}

	if (fstat(fd, &sb) == -1) {
		log_warn("%s: cannot stat %s", __func__, path);
		close(fd);
		return (-1);
	}

	if (sb.st_size == 0 || sb.st_size > 1024 * 1024) {
		/* Reject empty or unreasonably large tables */
		log_warnx("%s: unreasonable table size %lld", __func__,
		    (long long)sb.st_size);
		close(fd);
		return (-1);
	}

	buf = malloc(sb.st_size);
	if (buf == NULL) {
		log_warn("%s: malloc", __func__);
		close(fd);
		return (-1);
	}

	n = read(fd, buf, sb.st_size);
	close(fd);

	if (n != sb.st_size) {
		log_warn("%s: short read from %s", __func__, path);
		free(buf);
		return (-1);
	}

	/* Write to guest memory */
	if (write_mem(pa, buf, n)) {
		log_warnx("%s: could not write table to %lx", __func__, pa);
		free(buf);
		return (-1);
	}

	total = n;
	free(buf);

	log_warnx("%s: loaded %s (%zd bytes) to %lx", __func__, path, total, pa);
	return (total);
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
 * acpi_create_fadt
 *
 * create FADT (Fixed ACPI Description Table) at 'pa' pointing to DSDT at 'dsdt_pa'
 */
void
acpi_create_fadt(paddr_t pa, paddr_t dsdt_pa)
{
	struct acpi_fadt fadt;

	log_warnx("%s: creating FADT", __func__);
	memset(&fadt, 0, sizeof(fadt));

	acpi_populate_header(&fadt.hdr, VMD_FADT_OEM_TABLEID);
	fadt.hdr.revision = 6;
	fadt.fadt_minor = 5;

	/* Header */
	memcpy(fadt.hdr_signature, FADT_SIG, 4);
	fadt.hdr.length = sizeof(fadt);

	/* DSDT pointer - use both 32-bit and 64-bit fields */
	fadt.dsdt = (uint32_t)dsdt_pa;  /* ACPI 1.0 compatibility */
	fadt.x_dsdt = dsdt_pa;          /* ACPI 2.0+ 64-bit address */

	/* System configuration */
	fadt.pm_profile = FADT_PM_DESKTOP;

	/* SCI interrupt - typically uses IRQ 9 for ACPI */
	fadt.sci_int = 9;

	/* PM register lengths */
	fadt.pm1_evt_len = VMD_PM1A_EVT_LEN;
	fadt.pm1_cnt_len = VMD_PM1A_CNT_LEN;
	fadt.pm_tmr_len = 0;
	fadt.gpe0_blk_len = 0;
	fadt.p_lvl2_lat = 100;
	fadt.p_lvl3_lat = 1000;

	/*
	 * PM1A is retained for non-hardware-reduced ACPI.  Do not advertise
	 * optional register blocks which vmd does not implement.
	 */
	fadt.pm1a_evt_blk = VMD_PM1A_EVT_BASE;
	fadt.pm1b_evt_blk = 0;
	fadt.pm1a_cnt_blk = VMD_PM1A_CNT_BASE;
	fadt.pm1b_cnt_blk = 0;
	fadt.pm_tmr_blk = 0;
	fadt.gpe0_blk = 0;

	/* IAPC Boot Architecture Flags */
	fadt.iapc_boot_arch = FADT_LEGACY_DEVICES;

	/* FADT flags */
	fadt.flags = 0;

	/* Checksum */
	fadt.hdr.checksum = acpi_calculate_checksum((uint8_t *)&fadt, sizeof(fadt));

	log_warnx("%s: FADT size %zu", __func__, sizeof(fadt));
	log_warnx("%s: DSDT pointer 0x%x / 0x%llx", __func__, fadt.dsdt,
	    (unsigned long long)fadt.x_dsdt);

	acpi_verify_checksum((uint8_t *)&fadt, sizeof(fadt));

	log_warnx("%s: writing FADT to %lx", __func__, pa);
	if (write_mem(pa, &fadt, sizeof(fadt)))
		log_warnx("%s: could not write FADT table", __func__);

	log_warnx("%s: FADT creation complete", __func__);
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
	sz = sizeof(*madt) + sizeof(*ioapic) + (sizeof(*lapic) * numcpu);
	log_warnx("%s: MADT size %zd", __func__, sz);
	tbl = (uint8_t *)malloc(sz);
	if (tbl == NULL)
		fatal("malloc");

	memset(tbl, 0, sz);

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
	ioapic->acpi_ioapic_id = numcpu + 1;

	memcpy(madt->hdr_signature, MADT_SIG, 4);
	madt->hdr.length = sz;
	madt->hdr.checksum = acpi_calculate_checksum(tbl, sz);
	log_warnx("%s: computed MADT checksum 0x%x", __func__, madt->hdr.checksum);

	log_warnx("%s: writing MADT to %lx", __func__, pa);
	acpi_verify_checksum((uint8_t *)tbl, sz);

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

	acpi_verify_checksum((uint8_t *)xsdt, sz);

	log_warnx("%s: writing XSDT to %lx", __func__, pa);
	if (write_mem(pa, xsdt, sz))
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

	acpi_verify_checksum((uint8_t *)&rsdp, sizeof(rsdp));
	fw_cfg_add_acpi_rsdp(&rsdp, sizeof(rsdp));

	log_warnx("%s: writing RSDP to %lx", __func__, pa);
	if (write_mem(pa, &rsdp, sizeof(rsdp)))
		log_warnx("%s: could not write RSDP table", __func__);

	log_warnx("%s: RSDP creation complete", __func__);
}

void
acpi_init(size_t numcpu)
{
	paddr_t tables[4];
	size_t numtables;
	ssize_t dsdt_size;
	uint16_t rsdp_ptr_real;
	int have_dsdt = 0;

	log_warnx("%s: initializing acpi tables", __func__);
	rsdp_ptr_real = VMD_RSDP_PADDR >> 4;

	numtables = 0;

	/* Load DSDT from file - must be loaded before FADT creation */
	dsdt_size = acpi_load_table("/etc/firmware/vmm.dsdt", VMD_DSDT_PADDR);
	if (dsdt_size != -1) {
		have_dsdt = 1;
		log_warnx("%s: DSDT loaded successfully (%zd bytes)", __func__,
		    dsdt_size);
	} else {
		log_warnx("%s: DSDT not loaded (optional)", __func__);
	}

	/* Create FADT - points to DSDT if available, required for ACPI */
	if (have_dsdt) {
		acpi_create_fadt(VMD_FADT_PADDR, VMD_DSDT_PADDR);
		tables[numtables++] = VMD_FADT_PADDR;
		log_warnx("%s: FADT created pointing to DSDT", __func__);
	}

	acpi_create_madt(VMD_MADT_PADDR, numcpu);
	tables[numtables++] = VMD_MADT_PADDR;

	acpi_create_xsdt(VMD_XSDT_PADDR, tables, numtables);
	acpi_create_rsdp(VMD_RSDP_PADDR, VMD_XSDT_PADDR);

	/* EBDA pointer */
	log_warnx("%s: writing RSDP pointer 0x%x -> 0x%x", __func__,
	    rsdp_ptr_real, VMD_ACPI_EBDA_PTR);

	if (write_mem(VMD_ACPI_EBDA_PTR , &rsdp_ptr_real, sizeof(rsdp_ptr_real)))
		log_warnx("%s: could not write RSDP pointer to BDA", __func__);

}
