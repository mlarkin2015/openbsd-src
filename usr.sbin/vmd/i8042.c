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
#include <sys/param.h>

#include <dev/ic/i8042reg.h>
#include <dev/pckbc/pckbdreg.h>
#include <dev/vmm/vmm.h>

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "i8042.h"
#include "vmd.h"

#define I8042_IRQ_KBD		1
#define I8042_FIFO_SIZE		256

#define I8042_CMD_READ_OUTPUT	0xd0
#define I8042_CMD_WRITE_OUTPUT	0xd1
#define I8042_CMD_WRITE_KBD_OUT	0xd2
#define I8042_OUTPUT_RESET	0x01
#define I8042_OUTPUT_A20		0x02

#define I8042_KBD_PARAM_NONE	0
#define I8042_KBD_PARAM_LEDS	1
#define I8042_KBD_PARAM_TYPEMATIC 2
#define I8042_KBD_PARAM_SCANSET	3

/* X11 keysyms used by the RFB KeyEvent message. */
#define XK_BACKSPACE		0xff08
#define XK_TAB			0xff09
#define XK_RETURN		0xff0d
#define XK_PAUSE		0xff13
#define XK_SCROLL_LOCK		0xff14
#define XK_ESCAPE		0xff1b
#define XK_HOME			0xff50
#define XK_LEFT			0xff51
#define XK_UP			0xff52
#define XK_RIGHT		0xff53
#define XK_DOWN			0xff54
#define XK_PAGE_UP		0xff55
#define XK_PAGE_DOWN		0xff56
#define XK_END			0xff57
#define XK_PRINT		0xff61
#define XK_INSERT		0xff63
#define XK_MENU			0xff67
#define XK_NUM_LOCK		0xff7f
#define XK_KP_ENTER		0xff8d
#define XK_KP_HOME		0xff95
#define XK_KP_LEFT		0xff96
#define XK_KP_UP		0xff97
#define XK_KP_RIGHT		0xff98
#define XK_KP_DOWN		0xff99
#define XK_KP_PAGE_UP		0xff9a
#define XK_KP_PAGE_DOWN		0xff9b
#define XK_KP_END		0xff9c
#define XK_KP_BEGIN		0xff9d
#define XK_KP_INSERT		0xff9e
#define XK_KP_DELETE		0xff9f
#define XK_KP_MULTIPLY		0xffaa
#define XK_KP_ADD		0xffab
#define XK_KP_SEPARATOR		0xffac
#define XK_KP_SUBTRACT		0xffad
#define XK_KP_DECIMAL		0xffae
#define XK_KP_DIVIDE		0xffaf
#define XK_KP_0			0xffb0
#define XK_KP_9			0xffb9
#define XK_F1			0xffbe
#define XK_F12			0xffc9
#define XK_SHIFT_L		0xffe1
#define XK_SHIFT_R		0xffe2
#define XK_CONTROL_L		0xffe3
#define XK_CONTROL_R		0xffe4
#define XK_CAPS_LOCK		0xffe5
#define XK_ALT_L		0xffe9
#define XK_ALT_R		0xffea
#define XK_SUPER_L		0xffeb
#define XK_SUPER_R		0xffec
#define XK_DELETE		0xffff

struct i8042_output {
	uint8_t data;
	uint8_t command;
};

struct i8042_keycode {
	uint8_t set1;
	uint8_t set2;
	uint8_t extended;
};

struct i8042_state {
	pthread_mutex_t mtx;
	uint32_t vmid;
	uint8_t command_byte;
	uint8_t output_port;
	uint8_t pending_command;
	uint8_t keyboard_parameter;
	uint8_t scan_set;
	uint8_t scanning;
	uint8_t last_write_command;
	uint8_t irq_asserted;
	uint16_t head;
	uint16_t count;
	struct i8042_output output[I8042_FIFO_SIZE];
};

static struct i8042_state i8042 = {
	.mtx = PTHREAD_MUTEX_INITIALIZER
};

static void	i8042_irq_deassert_locked(void);
static void	i8042_irq_assert_locked(void);
static void	i8042_queue_locked(uint8_t, int);
static uint8_t	i8042_read_data_locked(void);
static uint8_t	i8042_read_status_locked(void);
static void	i8042_controller_command_locked(uint8_t);
static void	i8042_data_write_locked(uint8_t);
static void	i8042_keyboard_command_locked(uint8_t);
static int	i8042_keycode(uint32_t, struct i8042_keycode *);
static void	i8042_emit_key_locked(const struct i8042_keycode *, int);

static void
i8042_irq_deassert_locked(void)
{
	if (!i8042.irq_asserted)
		return;
	i8042.irq_asserted = 0;
	vcpu_deassert_irq(i8042.vmid, 0, I8042_IRQ_KBD);
}

static void
i8042_irq_assert_locked(void)
{
	if (i8042.irq_asserted || i8042.count == 0 ||
	    !(i8042.command_byte & KC8_KENABLE) ||
	    (i8042.command_byte & KC8_KDISABLE))
		return;
	i8042.irq_asserted = 1;
	vcpu_assert_irq(i8042.vmid, 0, I8042_IRQ_KBD);
}

static void
i8042_queue_locked(uint8_t data, int command)
{
	uint16_t tail;

	if (i8042.count == I8042_FIFO_SIZE)
		return;
	tail = (i8042.head + i8042.count) % I8042_FIFO_SIZE;
	i8042.output[tail].data = data;
	i8042.output[tail].command = command != 0;
	i8042.count++;
	i8042_irq_assert_locked();
}

static uint8_t
i8042_read_data_locked(void)
{
	uint8_t data;

	if (i8042.count == 0)
		return (0);
	i8042_irq_deassert_locked();
	data = i8042.output[i8042.head].data;
	i8042.head = (i8042.head + 1) % I8042_FIFO_SIZE;
	i8042.count--;
	i8042_irq_assert_locked();
	return (data);
}

static uint8_t
i8042_read_status_locked(void)
{
	uint8_t status = KBS_NOSEC;

	if (i8042.count != 0) {
		status |= KBS_DIB;
		if (i8042.output[i8042.head].command)
			status |= KBS_OCMD;
	}
	if (i8042.command_byte & KC8_CPU)
		status |= KBS_WARM;
	if (i8042.last_write_command)
		status |= KBS_OCMD;
	return (status);
}

static void
i8042_controller_command_locked(uint8_t command)
{
	i8042.last_write_command = 1;
	i8042.pending_command = I8042_KBD_PARAM_NONE;

	if (command >= KBC_RAMREAD && command < KBC_RAMWRITE) {
		i8042_queue_locked(command == KBC_RAMREAD ?
		    i8042.command_byte : 0, 1);
		return;
	}
	if (command >= KBC_RAMWRITE && command < KBC_AUXDISABLE) {
		i8042.pending_command = command;
		return;
	}

	switch (command) {
	case KBC_AUXDISABLE:
		i8042.command_byte |= KC8_MDISABLE;
		break;
	case KBC_AUXENABLE:
		i8042.command_byte &= ~KC8_MDISABLE;
		break;
	case KBC_AUXTEST:
		/* No auxiliary device is implemented yet. */
		i8042_queue_locked(0x01, 1);
		break;
	case KBC_SELFTEST:
		i8042.command_byte |= KC8_CPU;
		i8042_queue_locked(0x55, 1);
		break;
	case KBC_KBDTEST:
		i8042_queue_locked(0x00, 1);
		break;
	case KBC_KBDDISABLE:
		i8042.command_byte |= KC8_KDISABLE;
		i8042_irq_deassert_locked();
		break;
	case KBC_KBDENABLE:
		i8042.command_byte &= ~KC8_KDISABLE;
		i8042_irq_assert_locked();
		break;
	case I8042_CMD_READ_OUTPUT:
		i8042_queue_locked(i8042.output_port, 1);
		break;
	case I8042_CMD_WRITE_OUTPUT:
	case KBC_KBDECHO:
	case KBC_AUXECHO:
	case KBC_AUXWRITE:
		i8042.pending_command = command;
		break;
	default:
		break;
	}
}

static void
i8042_keyboard_command_locked(uint8_t data)
{
	if (i8042.keyboard_parameter != I8042_KBD_PARAM_NONE) {
		switch (i8042.keyboard_parameter) {
		case I8042_KBD_PARAM_LEDS:
		case I8042_KBD_PARAM_TYPEMATIC:
			i8042_queue_locked(KBR_ACK, 0);
			break;
		case I8042_KBD_PARAM_SCANSET:
			if (data == 0) {
				i8042_queue_locked(KBR_ACK, 0);
				i8042_queue_locked(i8042.scan_set, 0);
			} else if (data == 1 || data == 2) {
				i8042.scan_set = data;
				i8042_queue_locked(KBR_ACK, 0);
			} else {
				i8042_queue_locked(KBR_RESEND, 0);
			}
			break;
		}
		i8042.keyboard_parameter = I8042_KBD_PARAM_NONE;
		return;
	}

	switch (data) {
	case KBC_MODEIND:
		i8042_queue_locked(KBR_ACK, 0);
		i8042.keyboard_parameter = I8042_KBD_PARAM_LEDS;
		break;
	case KBC_ECHO:
		i8042_queue_locked(KBR_ECHO, 0);
		break;
	case KBC_SETTABLE:
		i8042_queue_locked(KBR_ACK, 0);
		i8042.keyboard_parameter = I8042_KBD_PARAM_SCANSET;
		break;
	case KBC_GETID:
		i8042_queue_locked(KBR_ACK, 0);
		i8042_queue_locked(KCID_KBD1, 0);
		i8042_queue_locked(KCID_KBD2, 0);
		break;
	case KBC_TYPEMATIC:
		i8042_queue_locked(KBR_ACK, 0);
		i8042.keyboard_parameter = I8042_KBD_PARAM_TYPEMATIC;
		break;
	case KBC_ENABLE:
		i8042.scanning = 1;
		i8042_queue_locked(KBR_ACK, 0);
		break;
	case KBC_DISABLE:
		i8042.scanning = 0;
		i8042.scan_set = 2;
		i8042_queue_locked(KBR_ACK, 0);
		break;
	case KBC_SETDEFAULT:
		i8042.scan_set = 2;
		i8042_queue_locked(KBR_ACK, 0);
		break;
	case KBC_RESEND:
		i8042_queue_locked(KBR_RESEND, 0);
		break;
	case KBC_RESET:
		i8042.scanning = 0;
		i8042.scan_set = 2;
		i8042_queue_locked(KBR_ACK, 0);
		i8042_queue_locked(KBR_RSTDONE, 0);
		break;
	default:
		i8042_queue_locked(KBR_RESEND, 0);
		break;
	}
}

static void
i8042_data_write_locked(uint8_t data)
{
	uint8_t command;

	i8042.last_write_command = 0;
	command = i8042.pending_command;
	i8042.pending_command = I8042_KBD_PARAM_NONE;

	if (command >= KBC_RAMWRITE && command < KBC_AUXDISABLE) {
		if (command == KBC_RAMWRITE) {
			i8042_irq_deassert_locked();
			i8042.command_byte = data;
			i8042_irq_assert_locked();
		}
		return;
	}

	switch (command) {
	case I8042_CMD_WRITE_OUTPUT:
		i8042.output_port = data;
		break;
	case KBC_KBDECHO:
		i8042_queue_locked(data, 0);
		break;
	case KBC_AUXECHO:
		/* Leave the auxiliary output buffer empty: no mouse exists. */
		break;
	case KBC_AUXWRITE:
		break;
	default:
		i8042_keyboard_command_locked(data);
		break;
	}
}

void
i8042_init(uint32_t vmid)
{
	pthread_mutex_lock(&i8042.mtx);
	i8042.vmid = vmid;
	i8042.command_byte = KC8_TRANS | KC8_CPU | KC8_MDISABLE;
	i8042.output_port = I8042_OUTPUT_RESET | I8042_OUTPUT_A20;
	i8042.pending_command = I8042_KBD_PARAM_NONE;
	i8042.keyboard_parameter = I8042_KBD_PARAM_NONE;
	i8042.scan_set = 2;
	i8042.scanning = 1;
	i8042.last_write_command = 0;
	i8042.irq_asserted = 0;
	i8042.head = 0;
	i8042.count = 0;
	pthread_mutex_unlock(&i8042.mtx);
}

int
i8042_reset_request(struct vm_run_params *vrp)
{
	struct vm_exit_inout *vei = &vrp->vrp_exit->vei;
	int reset = 0;
	uint8_t data;

	if (vei->vei_dir != VEI_DIR_OUT || vei->vei_string || vei->vei_size != 1)
		return (0);
	data = vei->vei_data;
	if (vei->vei_port == I8042_COMMAND_PORT)
		return (data == KBC_PULSE0);
	if (vei->vei_port != I8042_DATA_PORT)
		return (0);

	pthread_mutex_lock(&i8042.mtx);
	if (i8042.pending_command == I8042_CMD_WRITE_OUTPUT &&
	    !(data & I8042_OUTPUT_RESET))
		reset = 1;
	pthread_mutex_unlock(&i8042.mtx);
	return (reset);
}

uint8_t
vcpu_exit_i8042(struct vm_run_params *vrp)
{
	struct vm_exit_inout *vei = &vrp->vrp_exit->vei;
	uint32_t data;

	if (vei->vei_string || vei->vei_size != 1) {
		if (vei->vei_dir == VEI_DIR_IN)
			set_return_data(vrp->vrp_exit, UINT32_MAX);
		return (0xff);
	}

	pthread_mutex_lock(&i8042.mtx);
	if (vei->vei_dir == VEI_DIR_IN) {
		if (vei->vei_port == I8042_DATA_PORT)
			data = i8042_read_data_locked();
		else
			data = i8042_read_status_locked();
		set_return_data(vrp->vrp_exit, data);
	} else if (vei->vei_port == I8042_COMMAND_PORT) {
		i8042_controller_command_locked(vei->vei_data);
	} else {
		i8042_data_write_locked(vei->vei_data);
	}
	pthread_mutex_unlock(&i8042.mtx);
	return (0xff);
}

static int
i8042_keycode(uint32_t keysym, struct i8042_keycode *key)
{
	static const struct {
		uint32_t keysym;
		uint8_t set1;
		uint8_t set2;
		uint8_t extended;
	} map[] = {
		{ XK_BACKSPACE, 0x0e, 0x66, 0 },
		{ XK_TAB, 0x0f, 0x0d, 0 },
		{ XK_RETURN, 0x1c, 0x5a, 0 },
		{ XK_ESCAPE, 0x01, 0x76, 0 },
		{ XK_HOME, 0x47, 0x6c, 1 },
		{ XK_LEFT, 0x4b, 0x6b, 1 },
		{ XK_UP, 0x48, 0x75, 1 },
		{ XK_RIGHT, 0x4d, 0x74, 1 },
		{ XK_DOWN, 0x50, 0x72, 1 },
		{ XK_PAGE_UP, 0x49, 0x7d, 1 },
		{ XK_PAGE_DOWN, 0x51, 0x7a, 1 },
		{ XK_END, 0x4f, 0x69, 1 },
		{ XK_PRINT, 0x37, 0x7c, 1 },
		{ XK_INSERT, 0x52, 0x70, 1 },
		{ XK_MENU, 0x5d, 0x2f, 1 },
		{ XK_NUM_LOCK, 0x45, 0x77, 0 },
		{ XK_KP_ENTER, 0x1c, 0x5a, 1 },
		{ XK_KP_HOME, 0x47, 0x6c, 0 },
		{ XK_KP_LEFT, 0x4b, 0x6b, 0 },
		{ XK_KP_UP, 0x48, 0x75, 0 },
		{ XK_KP_RIGHT, 0x4d, 0x74, 0 },
		{ XK_KP_DOWN, 0x50, 0x72, 0 },
		{ XK_KP_PAGE_UP, 0x49, 0x7d, 0 },
		{ XK_KP_PAGE_DOWN, 0x51, 0x7a, 0 },
		{ XK_KP_END, 0x4f, 0x69, 0 },
		{ XK_KP_BEGIN, 0x4c, 0x73, 0 },
		{ XK_KP_INSERT, 0x52, 0x70, 0 },
		{ XK_KP_DELETE, 0x53, 0x71, 0 },
		{ XK_KP_MULTIPLY, 0x37, 0x7c, 0 },
		{ XK_KP_ADD, 0x4e, 0x79, 0 },
		{ XK_KP_SEPARATOR, 0x53, 0x71, 0 },
		{ XK_KP_SUBTRACT, 0x4a, 0x7b, 0 },
		{ XK_KP_DECIMAL, 0x53, 0x71, 0 },
		{ XK_KP_DIVIDE, 0x35, 0x4a, 1 },
		{ XK_SHIFT_L, 0x2a, 0x12, 0 },
		{ XK_SHIFT_R, 0x36, 0x59, 0 },
		{ XK_CONTROL_L, 0x1d, 0x14, 0 },
		{ XK_CONTROL_R, 0x1d, 0x14, 1 },
		{ XK_CAPS_LOCK, 0x3a, 0x58, 0 },
		{ XK_ALT_L, 0x38, 0x11, 0 },
		{ XK_ALT_R, 0x38, 0x11, 1 },
		{ XK_SUPER_L, 0x5b, 0x1f, 1 },
		{ XK_SUPER_R, 0x5c, 0x27, 1 },
		{ XK_DELETE, 0x53, 0x71, 1 },
	};
	static const uint8_t f_set1[] = {
		0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
		0x41, 0x42, 0x43, 0x44, 0x57, 0x58
	};
	static const uint8_t f_set2[] = {
		0x05, 0x06, 0x04, 0x0c, 0x03, 0x0b,
		0x83, 0x0a, 0x01, 0x09, 0x78, 0x07
	};
	static const uint8_t kp_set1[] = {
		0x52, 0x4f, 0x50, 0x51, 0x4b,
		0x4c, 0x4d, 0x47, 0x48, 0x49
	};
	static const uint8_t kp_set2[] = {
		0x70, 0x69, 0x72, 0x7a, 0x6b,
		0x73, 0x74, 0x6c, 0x75, 0x7d
	};
	static const struct {
		char ascii;
		uint8_t set1;
		uint8_t set2;
	} ascii_map[] = {
		{ '`', 0x29, 0x0e }, { '1', 0x02, 0x16 },
		{ '2', 0x03, 0x1e }, { '3', 0x04, 0x26 },
		{ '4', 0x05, 0x25 }, { '5', 0x06, 0x2e },
		{ '6', 0x07, 0x36 }, { '7', 0x08, 0x3d },
		{ '8', 0x09, 0x3e }, { '9', 0x0a, 0x46 },
		{ '0', 0x0b, 0x45 }, { '-', 0x0c, 0x4e },
		{ '=', 0x0d, 0x55 }, { 'q', 0x10, 0x15 },
		{ 'w', 0x11, 0x1d }, { 'e', 0x12, 0x24 },
		{ 'r', 0x13, 0x2d }, { 't', 0x14, 0x2c },
		{ 'y', 0x15, 0x35 }, { 'u', 0x16, 0x3c },
		{ 'i', 0x17, 0x43 }, { 'o', 0x18, 0x44 },
		{ 'p', 0x19, 0x4d }, { '[', 0x1a, 0x54 },
		{ ']', 0x1b, 0x5b }, { 'a', 0x1e, 0x1c },
		{ 's', 0x1f, 0x1b }, { 'd', 0x20, 0x23 },
		{ 'f', 0x21, 0x2b }, { 'g', 0x22, 0x34 },
		{ 'h', 0x23, 0x33 }, { 'j', 0x24, 0x3b },
		{ 'k', 0x25, 0x42 }, { 'l', 0x26, 0x4b },
		{ ';', 0x27, 0x4c }, { '\'', 0x28, 0x52 },
		{ '\\', 0x2b, 0x5d }, { 'z', 0x2c, 0x1a },
		{ 'x', 0x2d, 0x22 }, { 'c', 0x2e, 0x21 },
		{ 'v', 0x2f, 0x2a }, { 'b', 0x30, 0x32 },
		{ 'n', 0x31, 0x31 }, { 'm', 0x32, 0x3a },
		{ ',', 0x33, 0x41 }, { '.', 0x34, 0x49 },
		{ '/', 0x35, 0x4a }, { ' ', 0x39, 0x29 }
	};
	size_t i;

	memset(key, 0, sizeof(*key));
	if (keysym >= 'A' && keysym <= 'Z')
		keysym += 'a' - 'A';
	switch (keysym) {
	case '~': keysym = '`'; break;
	case '!': keysym = '1'; break;
	case '@': keysym = '2'; break;
	case '#': keysym = '3'; break;
	case '$': keysym = '4'; break;
	case '%': keysym = '5'; break;
	case '^': keysym = '6'; break;
	case '&': keysym = '7'; break;
	case '*': keysym = '8'; break;
	case '(': keysym = '9'; break;
	case ')': keysym = '0'; break;
	case '_': keysym = '-'; break;
	case '+': keysym = '='; break;
	case '{': keysym = '['; break;
	case '}': keysym = ']'; break;
	case ':': keysym = ';'; break;
	case '"': keysym = '\''; break;
	case '|': keysym = '\\'; break;
	case '<': keysym = ','; break;
	case '>': keysym = '.'; break;
	case '?': keysym = '/'; break;
	}

	for (i = 0; i < nitems(ascii_map); i++) {
		if (keysym == (uint32_t)ascii_map[i].ascii) {
			key->set1 = ascii_map[i].set1;
			key->set2 = ascii_map[i].set2;
			return (0);
		}
	}
	for (i = 0; i < nitems(map); i++) {
		if (keysym == map[i].keysym) {
			key->set1 = map[i].set1;
			key->set2 = map[i].set2;
			key->extended = map[i].extended;
			return (0);
		}
	}
	if (keysym >= XK_F1 && keysym <= XK_F12) {
		i = keysym - XK_F1;
		key->set1 = f_set1[i];
		key->set2 = f_set2[i];
		return (0);
	}
	if (keysym >= XK_KP_0 && keysym <= XK_KP_9) {
		i = keysym - XK_KP_0;
		key->set1 = kp_set1[i];
		key->set2 = kp_set2[i];
		return (0);
	}
	if (keysym == XK_SCROLL_LOCK) {
		key->set1 = 0x46;
		key->set2 = 0x7e;
		return (0);
	}
	if (keysym == XK_PAUSE)
		return (-1);
	return (-1);
}

static void
i8042_emit_key_locked(const struct i8042_keycode *key, int down)
{
	if (i8042.command_byte & KC8_TRANS || i8042.scan_set == 1) {
		if (key->extended)
			i8042_queue_locked(KBR_EXTENDED0, 0);
		i8042_queue_locked(down ? key->set1 : key->set1 | 0x80, 0);
	} else {
		if (key->extended)
			i8042_queue_locked(KBR_EXTENDED0, 0);
		if (!down)
			i8042_queue_locked(KBR_BREAK, 0);
		i8042_queue_locked(key->set2, 0);
	}
}

void
i8042_key_event(uint32_t keysym, int down)
{
	struct i8042_keycode key;

	if (i8042_keycode(keysym, &key) != 0)
		return;
	pthread_mutex_lock(&i8042.mtx);
	if (i8042.scanning && !(i8042.command_byte & KC8_KDISABLE))
		i8042_emit_key_locked(&key, down != 0);
	pthread_mutex_unlock(&i8042.mtx);
}
