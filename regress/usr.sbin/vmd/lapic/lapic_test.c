/*	$OpenBSD$ */

#include <sys/types.h>

#include <machine/i82093reg.h>
#include <machine/i82489reg.h>

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "i82093aa.h"
#include "i82489dx.h"
#include "mmio.h"
#include "vmd.h"

struct vmd_vm test_vm;
struct vmd_vm *current_vm = &test_vm;

static unsigned int vector_count;
static unsigned int init_count;
static unsigned int sipi_count;
static unsigned int avic_vector_count;
static unsigned int intr_count;
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

	assert(i82489dx_mmio(vcpu, MMIO_DIR_WRITE, LAPIC_BASE + reg, 4,
	    &data) == 0);
}

static uint32_t
read_reg(uint32_t vcpu, uint16_t reg)
{
	uint64_t data = 0;

	assert(i82489dx_mmio(vcpu, MMIO_DIR_READ, LAPIC_BASE + reg, 4,
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

static void
write_ioapic(uint32_t reg, uint32_t value)
{
	uint64_t data = reg;

	assert(i82093aa_mmio(0, MMIO_DIR_WRITE,
	    IOAPIC_BASE_DEFAULT + IOAPIC_REG, 4, &data) == 0);
	data = value;
	assert(i82093aa_mmio(0, MMIO_DIR_WRITE,
	    IOAPIC_BASE_DEFAULT + IOAPIC_DATA, 4, &data) == 0);
}

static void
program_ioapic(uint8_t pin, uint8_t dest, uint32_t low)
{
	write_ioapic(IOAPIC_REDHI(pin), dest << IOAPIC_REDHI_DEST_SHIFT);
	write_ioapic(IOAPIC_REDLO(pin), low);
}

static void
eoi(uint32_t vcpu)
{
	write_reg(vcpu, LAPIC_EOI, 0);
}

int
main(void)
{
	struct i82489dx_stats stats;
	uint32_t lapic_state[VMM_LAPIC_NREGS];
	unsigned int i;

	test_vm.vm_vmmid = 7;
	test_vm.vm_params.vmc_ncpus = 4;
	for (i = 0; i < test_vm.vm_params.vmc_ncpus; i++)
		i82489dx_init(i);
	i82093aa_init(test_vm.vm_params.vmc_ncpus);

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

	/* Vectors 0x10-0x1f are valid; only exception vectors are reserved. */
	write_reg(0, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	i82489dx_vector_irq(0, 0, 0x1f, 0);
	assert(read_reg(0, LAPIC_IRR) == (1U << 31));
	assert(i82489dx_ack(0) == 0x1f);
	eoi(0);
	i82489dx_vector_irq(0, 0, 0x0f, 0);
	assert(read_reg(0, LAPIC_IRR) == 0);

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
	assert(stats.mmio_reads == 6);
	assert(stats.mmio_writes == 11);
	assert(stats.icr_writes == 6);
	assert(stats.ipi_targets == 2);

	/* Linux/i386 MSI-X uses flat logical destination bit 0 for the BSP. */
	write_reg(0, LAPIC_LDR, 1U << LAPIC_ID_SHIFT);
	assert(i82489dx_targets(0x01, 1) == 0x01);
	write_reg(0, LAPIC_LDR, 0);

	/* Flat-mode logical fixed delivery selects the matching LDR bit. */
	write_reg(1, LAPIC_LDR, 2U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRHI, 2U << LAPIC_ID_SHIFT);
	write_reg(0, LAPIC_ICRLO,
	    LAPIC_DSTMODE_LOG | LAPIC_DLMODE_FIXED | 0x50);
	assert(vector_count == 3);
	assert(vector_targets[2] == 1);
	assert(last_vector == 0x50);

	/* AVIC's direct-vector path enters vmm(4) and wakes the target. */
	write_reg(2, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	assert(i82489dx_avic_activate(2, VMM_AVIC_XAPIC, 0,
	    lapic_state) == 0);
	assert(lapic_state[LAPIC_ID >> 4] ==
	    (2U << LAPIC_ID_SHIFT));
	i = unhalt_count;
	i82489dx_vector_irq(2, 0, 0x51, 0);
	assert(avic_vector_count == 1);
	assert(unhalt_count == i + 1);
	assert(signal_count == i + 1);
	assert(i82489dx_avic_deactivate(2, VMM_AVIC_XAPIC,
	    lapic_state) == 0);

	/* Invalid-type AVIC IPIs fall back to the normal ICR emulation. */
	i82489dx_avic_ipi(0, 1U << LAPIC_ID_SHIFT,
	    LAPIC_DLMODE_FIXED | 0x52, I82489DX_AVIC_IPI_INVALID_TYPE, 1,
	    0);
	assert(vector_count == 4);
	assert(vector_targets[3] == 1);
	assert(last_vector == 0x52);

	/* Hardware has queued a stopped-target IPI; userspace only wakes it. */
	i = unhalt_count;
	i82489dx_avic_ipi(0, 3U << LAPIC_ID_SHIFT,
	    LAPIC_DLMODE_FIXED | 0x53,
	    I82489DX_AVIC_IPI_TARGET_NOT_RUNNING, 3, 0);
	assert(unhalt_count == i + 1);
	assert(signal_count == i + 1);

	/* x2APIC exposes unshifted physical and derived logical IDs. */
	assert(read_x2apic(3, 0x02) == 3);
	assert(read_x2apic(3, 0x03) ==
	    ((6U << LAPIC_VERSION_LVT_SHIFT) | 0x10));
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

	/* x2AVIC state uses unshifted IDs and full-width ICR destinations. */
	write_reg(3, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	assert(i82489dx_avic_activate(3, VMM_AVIC_X2APIC, 0,
	    lapic_state) == 0);
	assert(lapic_state[LAPIC_ID >> 4] == 3);
	assert(lapic_state[LAPIC_LDR >> 4] == (1U << 3));
	i82489dx_avic_ipi(3, 2, LAPIC_DLMODE_FIXED | 0x57,
	    I82489DX_AVIC_IPI_INVALID_TYPE, 2, 1);
	assert(vector_count == 8);
	assert(vector_targets[7] == 2);
	assert(last_vector == 0x57);
	assert(i82489dx_avic_deactivate(3, VMM_AVIC_X2APIC,
	    lapic_state) == 0);

	/* IOAPIC fixed logical delivery multicasts in flat mode. */
	write_reg(1, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	write_reg(2, LAPIC_SVR, LAPIC_SVR_ENABLE | 0xff);
	write_reg(1, LAPIC_DFR, 0xffffffff);
	write_reg(2, LAPIC_DFR, 0xffffffff);
	write_reg(1, LAPIC_LDR, 2U << LAPIC_ID_SHIFT);
	write_reg(2, LAPIC_LDR, 4U << LAPIC_ID_SHIFT);
	program_ioapic(16, 0x06, IOAPIC_REDLO_DSTMOD | 0x60);
	i82093aa_assert_pin(16);
	assert(i82489dx_ack(1) == 0x60);
	assert(i82489dx_ack(2) == 0x60);
	eoi(1);
	eoi(2);
	i82093aa_deassert_pin(16);

	/* Lowest-priority delivery rotates ties between matching LAPICs. */
	program_ioapic(17, 0x06, IOAPIC_REDLO_DSTMOD |
	    (IOAPIC_REDLO_DEL_LOPRI << IOAPIC_REDLO_DEL_SHIFT) | 0x61);
	i82093aa_assert_pin(17);
	assert(i82489dx_ack(1) == 0x61);
	assert(i82489dx_ack(2) == 0xffff);
	eoi(1);
	i82093aa_deassert_pin(17);
	i82093aa_assert_pin(17);
	assert(i82489dx_ack(1) == 0xffff);
	assert(i82489dx_ack(2) == 0x61);
	eoi(2);
	i82093aa_deassert_pin(17);

	/* Processor priority wins over the round-robin tie breaker. */
	write_reg(1, LAPIC_TPRI, 0x40);
	write_reg(2, LAPIC_TPRI, 0x20);
	i82093aa_assert_pin(17);
	assert(i82489dx_ack(1) == 0xffff);
	assert(i82489dx_ack(2) == 0x61);
	eoi(2);
	i82093aa_deassert_pin(17);
	write_reg(1, LAPIC_TPRI, 0);
	write_reg(2, LAPIC_TPRI, 0);

	/* CR8 exposes only the task-priority class. */
	write_reg(1, LAPIC_TPRI, 0x2b);
	assert(i82489dx_get_cr8(1) == 2);
	i82489dx_set_cr8(1, 4);
	assert(read_reg(1, LAPIC_TPRI) == 0x40);

	/* A masked IRR vector supplies the exact CR8 unmask threshold. */
	i = intr_count;
	i82489dx_vector_irq(1, 0, 0x35, 0);
	assert(intr_count == i + 1);
	assert(i82489dx_cr8_threshold(1) == 3);
	i82489dx_set_cr8(1, 2);
	assert(i82489dx_cr8_threshold(1) == 0);
	assert(i82489dx_ack(1) == 0x35);
	eoi(1);
	write_reg(1, LAPIC_TPRI, 0);

	/* Cluster logical mode matches both cluster and local-ID mask. */
	write_reg(1, LAPIC_DFR, 0);
	write_reg(2, LAPIC_DFR, 0);
	write_reg(1, LAPIC_LDR, 0x11U << LAPIC_ID_SHIFT);
	write_reg(2, LAPIC_LDR, 0x12U << LAPIC_ID_SHIFT);
	program_ioapic(18, 0x12, IOAPIC_REDLO_DSTMOD | 0x62);
	i82093aa_assert_pin(18);
	assert(i82489dx_ack(1) == 0xffff);
	assert(i82489dx_ack(2) == 0x62);
	eoi(2);
	i82093aa_deassert_pin(18);

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

int
intr_pending(int vcpu_id)
{
	(void)vcpu_id;
	return (0);
}

int
vcpu_intr(uint32_t vmid, uint32_t target, uint8_t level)
{
	(void)vmid;
	(void)target;
	(void)level;
	intr_count++;
	return (0);
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
	assert(target < test_vm.vm_params.vmc_ncpus);
	unhalt_count++;
}

void
vcpu_signal_run(uint32_t target)
{
	assert(target < test_vm.vm_params.vmc_ncpus);
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
