/*	$OpenBSD$ */

#include <sys/types.h>

#include <machine/i82489reg.h>

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "i82489dx.h"
#include "mmio.h"
#include "vmd.h"

struct vmd_vm test_vm;
struct vmd_vm *current_vm = &test_vm;

static unsigned int vector_count;
static unsigned int init_count;
static unsigned int sipi_count;
static uint32_t vector_targets[VMM_MAX_VCPUS_PER_VM];
static uint32_t init_targets[VMM_MAX_VCPUS_PER_VM];
static uint32_t sipi_targets[VMM_MAX_VCPUS_PER_VM];
static uint8_t last_vector;
static uint8_t last_sipi;

static void
write_reg(uint32_t vcpu, uint16_t reg, uint32_t value)
{
	uint64_t data = value;

	assert(i82489dx_mmio(vcpu, MMIO_DIR_WRITE, LAPIC_BASE + reg,
	    &data) == 0);
}

static uint32_t
read_reg(uint32_t vcpu, uint16_t reg)
{
	uint64_t data = 0;

	assert(i82489dx_mmio(vcpu, MMIO_DIR_READ, LAPIC_BASE + reg,
	    &data) == 0);
	return ((uint32_t)data);
}

int
main(void)
{
	unsigned int i;

	test_vm.vm_vmmid = 7;
	test_vm.vm_params.vmc_ncpus = 4;
	for (i = 0; i < test_vm.vm_params.vmc_ncpus; i++)
		i82489dx_init(i);

	/* Physical destination and ICR readback. */
	write_reg(0, LAPIC_ICRHI, 2U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRLO, LAPIC_DLMODE_FIXED | 0x45);
	assert(vector_count == 1);
	assert(vector_targets[0] == 2);
	assert(last_vector == 0x45);
	assert(read_reg(0, LAPIC_ICRHI) == (2U << LAPIC_ID_SHIFT));
	assert(read_reg(0, LAPIC_ICRLO) == 0x45);

	/* Destination shorthand: self. */
	write_reg(0, LAPIC_ICRLO,
	    LAPIC_DEST_SELF | LAPIC_DLMODE_FIXED | 0x46);
	assert(vector_count == 2);
	assert(vector_targets[1] == 0);
	assert(last_vector == 0x46);

	/* INIT all-excluding-self, followed by the architectural deassert. */
	write_reg(0, LAPIC_ICRLO, LAPIC_DEST_ALLEXCL |
	    LAPIC_DLMODE_INIT | LAPIC_LVL_TRIG | LAPIC_LVL_ASSERT);
	assert(init_count == 3);
	assert(init_targets[0] == 1);
	assert(init_targets[1] == 2);
	assert(init_targets[2] == 3);
	write_reg(0, LAPIC_ICRLO, LAPIC_DEST_ALLEXCL |
	    LAPIC_DLMODE_INIT | LAPIC_LVL_TRIG);
	assert(init_count == 3);

	/* A physical startup IPI preserves the complete eight-bit vector. */
	write_reg(0, LAPIC_ICRHI, 3U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRLO, LAPIC_DLMODE_STARTUP | 0x8f);
	assert(sipi_count == 1);
	assert(sipi_targets[0] == 3);
	assert(last_sipi == 0x8f);

	/* Logical destination mode remains unsupported and must not misroute. */
	write_reg(0, LAPIC_ICRLO,
	    LAPIC_DSTMODE_LOG | LAPIC_DLMODE_FIXED | 0x47);
	assert(vector_count == 2);

	/* INIT resets software-enable and other mutable LAPIC state. */
	write_reg(2, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	assert(read_reg(2, LAPIC_SVR) == (LAPIC_SVR_ENABLE | 0xff));
	i82489dx_reset(2);
	assert(read_reg(2, LAPIC_SVR) == 0);

	return (0);
}

int
mmio_dev_add(paddr_t start, paddr_t end, mmio_dev_fn_t fn)
{
	(void)start;
	(void)end;
	(void)fn;
	return (0);
}

void
i82093aa_eoi(int vector)
{
	(void)vector;
}

void
vcpu_assert_vector(uint32_t vmid, uint32_t target, uint8_t vector)
{
	assert(vmid == test_vm.vm_vmmid);
	assert(vector_count < VMM_MAX_VCPUS_PER_VM);
	vector_targets[vector_count++] = target;
	last_vector = vector;
}

void
vcpu_assert_init(uint32_t target)
{
	assert(init_count < VMM_MAX_VCPUS_PER_VM);
	init_targets[init_count++] = target;
}

void
vcpu_start_sipi(uint32_t target, uint8_t vector)
{
	assert(sipi_count < VMM_MAX_VCPUS_PER_VM);
	sipi_targets[sipi_count++] = target;
	last_sipi = vector;
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
}

__dead void
fatalx(const char *fmt, ...)
{
	(void)fmt;
	abort();
}
