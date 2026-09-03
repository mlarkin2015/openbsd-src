/*	$OpenBSD$	*/

#include <sys/types.h>

#include <dev/ic/i8042reg.h>
#include <dev/pckbc/pckbdreg.h>
#include <dev/vmm/vmm.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "i8042.h"
#include "vmd.h"

#define XK_LEFT		0xff51

static struct vm_exit test_exit;
static struct vm_run_params test_vrp;
static unsigned int assert_count;
static unsigned int deassert_count;

void
vcpu_assert_irq(uint32_t vmid, uint32_t vcpu, int irq)
{
	assert(vmid == 42);
	assert(vcpu == 0);
	assert(irq == 1);
	assert_count++;
}

void
vcpu_deassert_irq(uint32_t vmid, uint32_t vcpu, int irq)
{
	assert(vmid == 42);
	assert(vcpu == 0);
	assert(irq == 1);
	deassert_count++;
}

void
set_return_data(struct vm_exit *vei, uint32_t data)
{
	vei->vei.vei_data = data;
}

static uint8_t
io(uint16_t port, uint8_t direction, uint8_t data)
{
	memset(&test_exit, 0, sizeof(test_exit));
	test_exit.vei.vei_port = port;
	test_exit.vei.vei_dir = direction;
	test_exit.vei.vei_size = 1;
	test_exit.vei.vei_data = data;
	assert(vcpu_exit_i8042(&test_vrp) == 0xff);
	return (test_exit.vei.vei_data);
}

static uint8_t
read_data(void)
{
	return (io(I8042_DATA_PORT, VEI_DIR_IN, 0));
}

static uint8_t
read_status(void)
{
	return (io(I8042_COMMAND_PORT, VEI_DIR_IN, 0));
}

static void
write_data(uint8_t data)
{
	(void)io(I8042_DATA_PORT, VEI_DIR_OUT, data);
}

static void
write_command(uint8_t data)
{
	(void)io(I8042_COMMAND_PORT, VEI_DIR_OUT, data);
}

static void
drain_ack(void)
{
	assert(read_status() & KBS_DIB);
	assert(read_data() == KBR_ACK);
}

static void
test_controller(void)
{
	uint8_t status;

	i8042_init(42);
	status = read_status();
	assert(!(status & KBS_DIB));
	assert(status & KBS_WARM);
	assert(status & KBS_NOSEC);

	write_command(KBC_SELFTEST);
	assert(read_status() & KBS_DIB);
	assert(read_data() == 0x55);
	write_command(KBC_KBDTEST);
	assert(read_data() == 0x00);
	write_command(KBC_AUXTEST);
	assert(read_data() == 0x01);

	write_command(KBC_RAMWRITE);
	write_data(KC8_TRANS | KC8_CPU | KC8_MDISABLE | KC8_KENABLE);
	write_command(KBC_RAMREAD);
	assert(assert_count == 1);
	assert(read_data() ==
	    (KC8_TRANS | KC8_CPU | KC8_MDISABLE | KC8_KENABLE));
	assert(deassert_count == 1);
}

static void
test_keyboard_commands(void)
{
	write_data(KBC_RESET);
	assert(read_data() == KBR_ACK);
	assert(read_data() == KBR_RSTDONE);
	write_data(KBC_ENABLE);
	drain_ack();
	write_data(KBC_GETID);
	assert(read_data() == KBR_ACK);
	assert(read_data() == KCID_KBD1);
	assert(read_data() == KCID_KBD2);
	write_data(KBC_MODEIND);
	drain_ack();
	write_data(7);
	drain_ack();
	write_data(KBC_TYPEMATIC);
	drain_ack();
	write_data(0x20);
	drain_ack();
}

static void
test_translated_scancodes(void)
{
	i8042_key_event('a', 1);
	assert(read_data() == 0x1e);
	i8042_key_event('A', 0);
	assert(read_data() == 0x9e);

	i8042_key_event(XK_LEFT, 1);
	assert(read_data() == KBR_EXTENDED0);
	assert(read_data() == 0x4b);
	i8042_key_event(XK_LEFT, 0);
	assert(read_data() == KBR_EXTENDED0);
	assert(read_data() == 0xcb);
}

static void
test_set2_scancodes(void)
{
	write_command(KBC_RAMWRITE);
	write_data(KC8_CPU | KC8_MDISABLE | KC8_KENABLE);
	write_data(KBC_SETTABLE);
	drain_ack();
	write_data(2);
	drain_ack();

	i8042_key_event('a', 1);
	assert(read_data() == 0x1c);
	i8042_key_event('a', 0);
	assert(read_data() == KBR_BREAK);
	assert(read_data() == 0x1c);

	write_data(KBC_SETTABLE);
	drain_ack();
	write_data(0);
	drain_ack();
	assert(read_data() == 2);
}

static void
test_disable_and_reset(void)
{
	struct vm_exit reset_exit;
	struct vm_run_params reset_vrp;

	write_command(KBC_KBDDISABLE);
	i8042_key_event('a', 1);
	assert(!(read_status() & KBS_DIB));
	write_command(KBC_KBDENABLE);

	memset(&reset_exit, 0, sizeof(reset_exit));
	memset(&reset_vrp, 0, sizeof(reset_vrp));
	reset_vrp.vrp_exit = &reset_exit;
	reset_exit.vei.vei_port = I8042_COMMAND_PORT;
	reset_exit.vei.vei_dir = VEI_DIR_OUT;
	reset_exit.vei.vei_size = 1;
	reset_exit.vei.vei_data = KBC_PULSE0;
	assert(i8042_reset_request(&reset_vrp));

	write_command(0xd1);
	reset_exit.vei.vei_port = I8042_DATA_PORT;
	reset_exit.vei.vei_data = 0x02;
	assert(i8042_reset_request(&reset_vrp));
	reset_exit.vei.vei_data = 0x03;
	assert(!i8042_reset_request(&reset_vrp));
}

int
main(void)
{
	memset(&test_vrp, 0, sizeof(test_vrp));
	test_vrp.vrp_vm_id = 42;
	test_vrp.vrp_exit = &test_exit;

	test_controller();
	test_keyboard_commands();
	test_translated_scancodes();
	test_set2_scancodes();
	test_disable_and_reset();

	assert(assert_count == deassert_count);
	return (0);
}
