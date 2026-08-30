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
static unsigned int avic_vector_count;
static unsigned int unhalt_count;
static unsigned int signal_count;
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

static void
write_x2apic(uint32_t vcpu, uint32_t reg, uint64_t value)
{
	assert(i82489dx_x2apic(vcpu, MMIO_DIR_WRITE,
	    MSR_X2APIC_BASE + reg, &value) == 0);
}

static uint64_t
read_x2apic(uint32_t vcpu, uint32_t reg)
{
	uint64_t value = 0;

	assert(i82489dx_x2apic(vcpu, MMIO_DIR_READ,
	    MSR_X2APIC_BASE + reg, &value) == 0);
	return (value);
}

int
main(void)
{
	struct i82489dx_stats stats;
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

	/* An unmatched logical destination must not misroute. */
	write_reg(0, LAPIC_ICRLO,
	    LAPIC_DSTMODE_LOG | LAPIC_DLMODE_FIXED | 0x47);
	assert(vector_count == 2);

	/* INIT resets software-enable and other mutable LAPIC state. */
	write_reg(2, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	assert(read_reg(2, LAPIC_SVR) == (LAPIC_SVR_ENABLE | 0xff));
	i82489dx_reset(2);
	assert(read_reg(2, LAPIC_SVR) == 0);

	/* Diagnostic counters cover the same MMIO and fixed-IPI operations. */
	i82489dx_stats_snapshot(&stats);
	assert(stats.mmio_reads == 4);
	assert(stats.mmio_writes == 9);
	assert(stats.icr_writes == 6);
	assert(stats.ipi_targets == 2);

	/* Flat-mode logical fixed delivery selects the matching LDR bit. */
	write_reg(1, LAPIC_LDR, 2U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRHI, 2U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRLO,
	    LAPIC_DSTMODE_LOG | LAPIC_DLMODE_FIXED | 0x50);
	assert(vector_count == 3);
	assert(vector_targets[2] == 1);
	assert(last_vector == 0x50);

	/* AVIC's direct-vector path enters vmm(4) and wakes the target. */
	test_vm.vm_avic = 1;
	write_reg(2, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	i82489dx_vector_irq(2, 0, 0x51, 0);
	assert(avic_vector_count == 1);
	assert(unhalt_count == 1);
	assert(signal_count == 1);
	test_vm.vm_avic = 0;

	/* Invalid-type AVIC IPIs fall back to the normal ICR emulation. */
	i82489dx_avic_ipi(0, 1U << LAPIC_ID_SHIFT,
	    LAPIC_DLMODE_FIXED | 0x52, I82489DX_AVIC_IPI_INVALID_TYPE, 1);
	assert(vector_count == 4);
	assert(vector_targets[3] == 1);
	assert(last_vector == 0x52);

	/* Hardware has queued a stopped-target IPI; userspace only wakes it. */
	i82489dx_avic_ipi(0, 3U << LAPIC_ID_SHIFT,
	    LAPIC_DLMODE_FIXED | 0x53,
	    I82489DX_AVIC_IPI_TARGET_NOT_RUNNING, 3);
	assert(unhalt_count == 2);
	assert(signal_count == 2);

	/* x2APIC exposes unshifted physical and derived logical IDs. */
	assert(read_x2apic(3, 0x02) == 3);
	assert(read_x2apic(3, 0x0d) == (1U << 3));
	write_x2apic(3, 0x08, 0x20);
	assert(read_x2apic(3, 0x08) == 0x20);

	/* Its 64-bit ICR supports physical and cluster-logical targets. */
	write_x2apic(0, 0x30, (3ULL << 32) | 0x54);
	assert(vector_count == 5);
	assert(vector_targets[4] == 3);
	assert(last_vector == 0x54);
	assert(read_x2apic(0, 0x30) == ((3ULL << 32) | 0x54));

	write_x2apic(0, 0x30,
	    (4ULL << 32) | LAPIC_DSTMODE_LOG | 0x55);
	assert(vector_count == 6);
	assert(vector_targets[5] == 2);
	assert(last_vector == 0x55);

	/* Self-IPI has no destination field and always targets the sender. */
	write_x2apic(1, 0x3f, 0x56);
	assert(vector_count == 7);
	assert(vector_targets[6] == 1);
	assert(last_vector == 0x56);

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

int
vcpu_intr_vector(uint32_t vmid, uint32_t target, uint8_t vector, int level)
{
	assert(vmid == test_vm.vm_vmmid);
	assert(target == 2);
	assert(vector == 0x51);
	assert(level == 0);
	avic_vector_count++;
	return (0);
}

void
vcpu_unhalt(uint32_t target)
{
	assert(target == 2 || target == 3);
	unhalt_count++;
}

void
vcpu_signal_run(uint32_t target)
{
	assert(target == 2 || target == 3);
	signal_count++;
}

void
log_debug(const char *fmt, ...)
{
	(void)fmt;
}

int
log_getverbose(void)
{
	return (1);
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
