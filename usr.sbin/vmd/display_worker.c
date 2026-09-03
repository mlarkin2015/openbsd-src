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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <endian.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmd.h"
#include "display.h"
#ifdef __amd64__
#include "i8042.h"
#include "ramfb.h"
#endif

#define DISPLAY_LISTEN_FD	3
#define DISPLAY_SURFACE_FD	4
#define DISPLAY_CONTROL_FD	5
#define DISPLAY_STATUS_FD	6
#define RFB_ENCODING_RAW	0
#define RFB_MAX_ENCODINGS	64
#define RFB_UPDATE_POLL_MS	33

struct rfb_pixel_format {
	uint8_t bits_per_pixel;
	uint8_t depth;
	uint8_t big_endian;
	uint8_t true_colour;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
	uint8_t padding[3];
} __packed;

static struct display_surface *display_surface;
static int display_control = -1;
static pid_t display_pid = -1;
static struct event display_control_ev;

extern struct vmd *env;

static void	display_input_drain(int, short, void *);
static int	rfb_wait(int, short, int, int);
static int	rfb_read(int, int, void *, size_t);
static int	rfb_write(int, int, const void *, size_t);
static int	rfb_handshake(int, int, const struct display_surface *,
	    uint8_t *, size_t, struct display_frame *);
static int	rfb_client(int, int, const struct display_surface *, uint8_t *,
	    size_t);
static int	rfb_send_update(int, int, const struct display_surface *,
	    uint8_t *, size_t, struct display_frame *, uint16_t, uint16_t,
	    uint16_t, uint16_t);
static void	rfb_input(int, const struct display_input *);

struct display_surface *
display_get_surface(void)
{
	return (display_surface);
}

static void
display_input_drain(int fd, short event, void *arg)
{
	struct display_input input;
	ssize_t n;

	while ((n = recv(fd, &input, sizeof(input), MSG_DONTWAIT)) > 0) {
		if (n != sizeof(input))
			continue;
#ifdef __amd64__
		if (input.type == DISPLAY_INPUT_KEY)
			i8042_key_event(input.value, input.down);
#endif
	}
}

int
display_start(struct vmd_vm *vm)
{
	char *argv[5];
	int control[2];
	int status[2], childfd[4], child_error, error_fd;
	ssize_t n;
	pid_t pid;

	if (!vm->vm_params.vmc_display)
		return (0);
	if (vm->vm_display == -1 || vm->vm_display_mem == -1) {
		errno = EINVAL;
		return (-1);
	}
	if (display_surface_map(vm->vm_display_mem, PROT_READ | PROT_WRITE,
	    &display_surface) == -1)
		return (-1);
	display_surface_init(display_surface);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
	    control) == -1)
		goto fail_map;
	if (pipe2(status, O_CLOEXEC) == -1)
		goto fail_control;

	pid = fork();
	if (pid == -1)
		goto fail_status;
	if (pid == 0) {
		close(status[0]);
		childfd[0] = fcntl(vm->vm_display, F_DUPFD_CLOEXEC, 10);
		childfd[1] = fcntl(vm->vm_display_mem, F_DUPFD_CLOEXEC, 10);
		childfd[2] = fcntl(control[1], F_DUPFD_CLOEXEC, 10);
		childfd[3] = fcntl(status[1], F_DUPFD_CLOEXEC, 10);
		error_fd = childfd[3] == -1 ? status[1] : childfd[3];
		if (childfd[0] == -1 || childfd[1] == -1 ||
		    childfd[2] == -1 || childfd[3] == -1 ||
		    dup2(childfd[0], DISPLAY_LISTEN_FD) == -1 ||
		    dup2(childfd[1], DISPLAY_SURFACE_FD) == -1 ||
		    dup2(childfd[2], DISPLAY_CONTROL_FD) == -1 ||
		    dup2(childfd[3], DISPLAY_STATUS_FD) == -1 ||
		    fcntl(DISPLAY_STATUS_FD, F_SETFD, FD_CLOEXEC) == -1)
			goto child_fail;
		error_fd = DISPLAY_STATUS_FD;
		closefrom(DISPLAY_STATUS_FD + 1);
		argv[0] = env->argv0;
		argv[1] = "-G";
		argv[2] = "-p";
		argv[3] = vm->vm_params.vmc_name;
		argv[4] = NULL;
		execv(argv[0], argv);
	child_fail:
		child_error = errno;
		(void)write(error_fd, &child_error,
		    sizeof(child_error));
		_exit(1);
	}

	close(status[1]);
	do {
		n = read(status[0], &child_error, sizeof(child_error));
	} while (n == -1 && errno == EINTR);
	close(status[0]);
	if (n != 0) {
		if (n == sizeof(child_error))
			errno = child_error;
		else if (n >= 0)
			errno = EIO;
		(void)waitpid(pid, NULL, 0);
		goto fail_control;
	}
	display_pid = pid;
	close(control[1]);
	display_control = control[0];
	close(vm->vm_display);
	vm->vm_display = -1;
	close(vm->vm_display_mem);
	vm->vm_display_mem = -1;
	event_set(&display_control_ev, display_control, EV_READ | EV_PERSIST,
	    display_input_drain, NULL);
	event_add(&display_control_ev, NULL);
	return (0);

 fail_status:
	close(status[0]);
	close(status[1]);
 fail_control:
	close(control[0]);
	close(control[1]);
 fail_map:
	munmap(display_surface, display_surface_size());
	display_surface = NULL;
	return (-1);
}

void
display_stop(void)
{
#ifdef __amd64__
	ramfb_stop();
#endif
	if (display_control != -1) {
		event_del(&display_control_ev);
		close(display_control);
		display_control = -1;
	}
	if (display_pid > 0) {
		while (waitpid(display_pid, NULL, 0) == -1 && errno == EINTR)
			;
		display_pid = -1;
	}
}

static int
rfb_wait(int fd, short events, int control, int timeout)
{
	struct pollfd pfd[2];
	int n;

	pfd[0].fd = fd;
	pfd[0].events = events;
	pfd[1].fd = control;
	pfd[1].events = POLLIN;
	for (;;) {
		n = poll(pfd, 2, timeout);
		if (n == -1 && errno == EINTR)
			continue;
		if (n == 0)
			return (1);
		if (n == -1 || (pfd[1].revents &
		    (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
			return (-1);
		if (pfd[0].revents & (events | POLLHUP | POLLERR | POLLNVAL))
			return (pfd[0].revents & events ? 0 : -1);
	}
}

static int
rfb_read(int fd, int control, void *buf, size_t len)
{
	uint8_t *p = buf;
	ssize_t n;

	while (len != 0) {
		if (rfb_wait(fd, POLLIN, control, -1) == -1)
			return (-1);
		n = read(fd, p, len);
		if (n == -1 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			return (-1);
		p += n;
		len -= n;
	}
	return (0);
}

static int
rfb_write(int fd, int control, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	ssize_t n;

	while (len != 0) {
		if (rfb_wait(fd, POLLOUT, control, -1) == -1)
			return (-1);
		n = write(fd, p, len);
		if (n == -1 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			return (-1);
		p += n;
		len -= n;
	}
	return (0);
}

static int
rfb_handshake(int fd, int control, const struct display_surface *surface,
    uint8_t *pixels, size_t pixels_len, struct display_frame *frame)
{
	static const char version[] = "RFB 003.008\n";
	static const char name[] = "OpenBSD vmd";
	struct rfb_pixel_format format;
	uint8_t client_version[12], security[2] = { 1, 1 }, selected, shared;
	uint8_t init[24];
	uint32_t value;
	uint16_t short_value;
	unsigned int minor;

	if (rfb_write(fd, control, version, sizeof(version) - 1) == -1 ||
	    rfb_read(fd, control, client_version, sizeof(client_version)) == -1)
		return (-1);
	if (memcmp(client_version, "RFB 003.", 8) != 0 ||
	    client_version[11] != '\n' ||
	    client_version[8] < '0' || client_version[8] > '9' ||
	    client_version[9] < '0' || client_version[9] > '9' ||
	    client_version[10] < '0' || client_version[10] > '9')
		return (-1);
	minor = (client_version[8] - '0') * 100 +
	    (client_version[9] - '0') * 10 + client_version[10] - '0';
	if (minor != 3 && minor != 7 && minor != 8)
		return (-1);

	if (minor == 3) {
		value = htobe32(1);
		if (rfb_write(fd, control, &value, sizeof(value)) == -1)
			return (-1);
	} else {
		if (rfb_write(fd, control, security, sizeof(security)) == -1 ||
		    rfb_read(fd, control, &selected, sizeof(selected)) == -1 ||
		    selected != 1)
			return (-1);
		value = 0;
		if (rfb_write(fd, control, &value, sizeof(value)) == -1)
			return (-1);
	}
	if (rfb_read(fd, control, &shared, sizeof(shared)) == -1 ||
	    display_surface_snapshot(surface, pixels, pixels_len, frame) == -1)
		return (-1);

	memset(&format, 0, sizeof(format));
	format.bits_per_pixel = 32;
	format.depth = 24;
	format.true_colour = 1;
	format.red_max = htobe16(255);
	format.green_max = htobe16(255);
	format.blue_max = htobe16(255);
	format.red_shift = 16;
	format.green_shift = 8;
	memset(init, 0, sizeof(init));
	short_value = htobe16(frame->width);
	memcpy(&init[0], &short_value, sizeof(short_value));
	short_value = htobe16(frame->height);
	memcpy(&init[2], &short_value, sizeof(short_value));
	memcpy(&init[4], &format, sizeof(format));
	value = htobe32(sizeof(name) - 1);
	memcpy(&init[20], &value, sizeof(value));
	if (rfb_write(fd, control, init, sizeof(init)) == -1 ||
	    rfb_write(fd, control, name, sizeof(name) - 1) == -1)
		return (-1);
	return (0);
}

static int
rfb_send_update(int fd, int control, const struct display_surface *surface,
    uint8_t *pixels, size_t pixels_len, struct display_frame *frame,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
	uint8_t header[16];
	uint32_t encoding = htobe32(RFB_ENCODING_RAW);
	uint32_t row;
	uint16_t value;

	if (display_surface_snapshot(surface, pixels, pixels_len, frame) == -1)
		return (-1);
	if (x >= frame->width || y >= frame->height)
		width = height = 0;
	else {
		if (width > frame->width - x)
			width = frame->width - x;
		if (height > frame->height - y)
			height = frame->height - y;
	}
	memset(header, 0, sizeof(header));
	header[1] = 0;
	value = htobe16(width != 0 && height != 0);
	memcpy(&header[2], &value, sizeof(value));
	if (width != 0 && height != 0) {
		value = htobe16(x);
		memcpy(&header[4], &value, sizeof(value));
		value = htobe16(y);
		memcpy(&header[6], &value, sizeof(value));
		value = htobe16(width);
		memcpy(&header[8], &value, sizeof(value));
		value = htobe16(height);
		memcpy(&header[10], &value, sizeof(value));
		memcpy(&header[12], &encoding, sizeof(encoding));
	}
	if (rfb_write(fd, control, header,
	    width != 0 && height != 0 ? sizeof(header) : 4) == -1)
		return (-1);
	for (row = 0; row < height && width != 0; row++) {
		if (rfb_write(fd, control,
		    pixels + (size_t)(y + row) * frame->stride +
		    (size_t)x * DISPLAY_BPP,
		    (size_t)width * DISPLAY_BPP) == -1)
			return (-1);
	}
	return (0);
}

static void
rfb_input(int control, const struct display_input *input)
{
	(void)send(control, input, sizeof(*input), MSG_DONTWAIT);
}

static int
rfb_client(int fd, int control, const struct display_surface *surface,
    uint8_t *pixels, size_t pixels_len)
{
	struct display_frame frame, next;
	struct display_input input;
	struct rfb_pixel_format format;
	uint8_t type, buf[9], update_pending = 0;
	uint16_t count, x, y, width, height;
	uint16_t update_x = 0, update_y = 0;
	uint16_t update_width = 0, update_height = 0;
	uint32_t i, value;
	int ret;

	if (rfb_handshake(fd, control, surface, pixels, pixels_len,
	    &frame) == -1)
		return (-1);
	for (;;) {
		if (update_pending) {
			if (__atomic_load_n(&surface->generation,
			    __ATOMIC_ACQUIRE) != frame.generation) {
				if (display_surface_snapshot(surface, pixels,
				    pixels_len, &next) == -1 ||
				    next.width != frame.width ||
				    next.height != frame.height ||
				    rfb_send_update(fd, control, surface, pixels,
				    pixels_len, &frame, update_x, update_y,
				    update_width, update_height) == -1)
					return (-1);
				update_pending = 0;
				continue;
			}
			ret = rfb_wait(fd, POLLIN, control,
			    RFB_UPDATE_POLL_MS);
			if (ret == -1)
				return (-1);
			if (ret == 1)
				continue;
		}
		if (rfb_read(fd, control, &type, 1) == -1)
			return (-1);
		switch (type) {
		case 0: /* SetPixelFormat */
			if (rfb_read(fd, control, buf, 3) == -1 ||
			    rfb_read(fd, control, &format, sizeof(format)) == -1)
				return (-1);
			if (format.bits_per_pixel != 32 || format.depth != 24 ||
			    format.big_endian != 0 || format.true_colour != 1 ||
			    be16toh(format.red_max) != 255 ||
			    be16toh(format.green_max) != 255 ||
			    be16toh(format.blue_max) != 255 ||
			    format.red_shift != 16 || format.green_shift != 8 ||
			    format.blue_shift != 0)
				return (-1);
			break;
		case 2: /* SetEncodings */
			if (rfb_read(fd, control, buf, 1) == -1 ||
			    rfb_read(fd, control, &count, sizeof(count)) == -1)
				return (-1);
			count = be16toh(count);
			if (count > RFB_MAX_ENCODINGS)
				return (-1);
			for (i = 0; i < count; i++)
				if (rfb_read(fd, control, &value,
				    sizeof(value)) == -1)
					return (-1);
			break;
		case 3: /* FramebufferUpdateRequest */
			if (rfb_read(fd, control, buf, sizeof(buf)) == -1)
				return (-1);
			memcpy(&x, &buf[1], sizeof(x));
			memcpy(&y, &buf[3], sizeof(y));
			memcpy(&width, &buf[5], sizeof(width));
			memcpy(&height, &buf[7], sizeof(height));
			x = be16toh(x);
			y = be16toh(y);
			width = be16toh(width);
			height = be16toh(height);
			if (display_surface_snapshot(surface, pixels, pixels_len,
			    &next) == -1 || next.width != frame.width ||
			    next.height != frame.height)
				return (-1);
			if (buf[0] == 0 || next.generation != frame.generation) {
				if (rfb_send_update(fd, control, surface, pixels,
				    pixels_len, &frame, x, y, width,
				    height) == -1)
					return (-1);
			} else {
				update_x = x;
				update_y = y;
				update_width = width;
				update_height = height;
				update_pending = 1;
			}
			break;
		case 4: /* KeyEvent */
			if (rfb_read(fd, control, buf, 7) == -1)
				return (-1);
			memset(&input, 0, sizeof(input));
			input.type = DISPLAY_INPUT_KEY;
			input.down = buf[0] != 0;
			memcpy(&value, &buf[3], sizeof(value));
			input.value = be32toh(value);
			rfb_input(control, &input);
			break;
		case 5: /* PointerEvent */
			if (rfb_read(fd, control, buf, 5) == -1)
				return (-1);
			memset(&input, 0, sizeof(input));
			input.type = DISPLAY_INPUT_POINTER;
			input.buttons = buf[0];
			memcpy(&x, &buf[1], sizeof(x));
			memcpy(&y, &buf[3], sizeof(y));
			input.x = be16toh(x);
			input.y = be16toh(y);
			rfb_input(control, &input);
			break;
		case 6: /* ClientCutText: clipboard is intentionally disabled. */
		default:
			return (-1);
		}
	}
}

#ifdef DISPLAY_RFB_TEST
int
display_rfb_test_client(int fd, int control,
    const struct display_surface *surface)
{
	uint8_t *pixels;
	size_t pixels_len;
	int ret;

	pixels_len = (size_t)DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT *
	    DISPLAY_BPP;
	if ((pixels = malloc(pixels_len)) == NULL)
		return (-1);
	ret = rfb_client(fd, control, surface, pixels, pixels_len);
	free(pixels);
	return (ret);
}
#endif

void
display_main(int listener, int surface_fd, int control, const char *title)
{
	struct display_surface *surface;
	struct stat st;
	struct pollfd pfd[2];
	uint8_t *pixels;
	size_t pixels_len;
	int client, n;

	log_procinit("vm/%s/display", title == NULL ? "?" : title);
	setproctitle("%s/display", title == NULL ? "?" : title);
	if (fstat(listener, &st) == -1 || !S_ISSOCK(st.st_mode) ||
	    fstat(surface_fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size != (off_t)display_surface_size() ||
	    fstat(control, &st) == -1 || !S_ISSOCK(st.st_mode))
		fatalx("invalid display worker file descriptors");
	if (display_surface_map(surface_fd, PROT_READ, &surface) == -1)
		fatal("cannot map display surface");
	close(surface_fd);
	if (pledge("stdio unix", NULL) == -1)
		fatal("pledge");
	pixels_len = (size_t)DISPLAY_MAX_WIDTH * DISPLAY_MAX_HEIGHT *
	    DISPLAY_BPP;
	if ((pixels = malloc(pixels_len)) == NULL)
		fatal("malloc");

	for (;;) {
		pfd[0].fd = listener;
		pfd[0].events = POLLIN;
		pfd[1].fd = control;
		pfd[1].events = POLLIN;
		n = poll(pfd, 2, -1);
		if (n == -1 && errno == EINTR)
			continue;
		if (n == -1 || (pfd[1].revents &
		    (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
			break;
		if (!(pfd[0].revents & POLLIN))
			continue;
		client = accept4(listener, NULL, NULL,
		    SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client == -1)
			continue;
		(void)rfb_client(client, control, surface, pixels, pixels_len);
		close(client);
	}
	free(pixels);
	munmap(surface, display_surface_size());
	_exit(0);
}
