/*	$OpenBSD$	*/

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmd.h"
#include "display.h"

#define CHECK(_cond) do { \
	if (!(_cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed: %s\n", \
		    __func__, __LINE__, #_cond, strerror(errno)); \
		exit(1); \
	} \
} while (0)

struct vmd *env;

/* The RFB unit test reads the typed input channel directly. */
void
i8042_key_event(uint32_t keysym, int down)
{
	(void)keysym;
	(void)down;
}

void
log_procinit(const char *fmt, ...)
{
}

void
ramfb_stop(void)
{
}

__dead void
fatal(const char *fmt, ...)
{
	_exit(127);
}

__dead void
fatalx(const char *fmt, ...)
{
	_exit(127);
}

static void
read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	ssize_t n;

	while (len != 0) {
		n = read(fd, p, len);
		CHECK(n > 0);
		p += n;
		len -= n;
	}
}

static void
write_full(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	ssize_t n;

	while (len != 0) {
		n = write(fd, p, len);
		CHECK(n > 0);
		p += n;
		len -= n;
	}
}

static pid_t
start_server(struct display_surface *surface, int *client, int *control)
{
	int rfb[2], ctl[2];
	pid_t pid;

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, rfb) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctl) == 0);
	CHECK((pid = fork()) != -1);
	if (pid == 0) {
		close(rfb[0]);
		close(ctl[0]);
		(void)display_rfb_test_client(rfb[1], ctl[1], surface);
		_exit(0);
	}
	close(rfb[1]);
	close(ctl[1]);
	*client = rfb[0];
	*control = ctl[0];
	return (pid);
}

static void
handshake(int fd, uint16_t *width, uint16_t *height)
{
	uint8_t buf[24];
	uint32_t name_len;

	read_full(fd, buf, 12);
	CHECK(memcmp(buf, "RFB 003.008\n", 12) == 0);
	write_full(fd, buf, 12);
	read_full(fd, buf, 2);
	CHECK(buf[0] == 1 && buf[1] == 1);
	buf[0] = 1;
	write_full(fd, buf, 1);
	read_full(fd, buf, 4);
	CHECK(memcmp(buf, "\0\0\0\0", 4) == 0);
	buf[0] = 1;
	write_full(fd, buf, 1);
	read_full(fd, buf, sizeof(buf));
	memcpy(width, &buf[0], sizeof(*width));
	memcpy(height, &buf[2], sizeof(*height));
	*width = be16toh(*width);
	*height = be16toh(*height);
	memcpy(&name_len, &buf[20], sizeof(name_len));
	name_len = be32toh(name_len);
	CHECK(name_len < sizeof(buf));
	read_full(fd, buf, name_len);
	CHECK(name_len == strlen("OpenBSD vmd"));
	CHECK(memcmp(buf, "OpenBSD vmd", name_len) == 0);
}

static void
test_protocol(struct display_surface *surface)
{
	struct display_input input;
	struct pollfd pfd;
	uint8_t msg[10], reply[16], pixels[16];
	uint16_t width, height, value16;
	uint32_t value32;
	int client, control, status;
	pid_t pid;

	pid = start_server(surface, &client, &control);
	handshake(client, &width, &height);
	CHECK(width == 2 && height == 2);

	memset(msg, 0, sizeof(msg));
	msg[0] = 3;
	value16 = htobe16(2);
	memcpy(&msg[6], &value16, sizeof(value16));
	memcpy(&msg[8], &value16, sizeof(value16));
	write_full(client, msg, sizeof(msg));
	read_full(client, reply, sizeof(reply));
	CHECK(reply[0] == 0 && reply[3] == 1);
	memcpy(&value32, &reply[12], sizeof(value32));
	CHECK(be32toh(value32) == 0);
	read_full(client, pixels, sizeof(pixels));
	CHECK(memcmp(pixels, surface->pixels, sizeof(pixels)) == 0);

	/* An unchanged incremental request must remain pending. */
	memset(msg, 0, sizeof(msg));
	msg[0] = 3;
	msg[1] = 1;
	value16 = htobe16(2);
	memcpy(&msg[6], &value16, sizeof(value16));
	memcpy(&msg[8], &value16, sizeof(value16));
	write_full(client, msg, sizeof(msg));
	pfd.fd = client;
	pfd.events = POLLIN;
	CHECK(poll(&pfd, 1, 100) == 0);

	/* Input must still be handled while that update is pending. */
	memset(msg, 0, 8);
	msg[0] = 4;
	msg[1] = 1;
	value32 = htobe32('A');
	memcpy(&msg[4], &value32, sizeof(value32));
	write_full(client, msg, 8);
	read_full(control, &input, sizeof(input));
	CHECK(input.type == DISPLAY_INPUT_KEY && input.down == 1);
	CHECK(input.value == 'A');

	pixels[0] ^= 0xff;
	CHECK(display_surface_update(surface, pixels, 2, 2, 8,
	    DISPLAY_FORMAT_XRGB8888) == 0);
	read_full(client, reply, sizeof(reply));
	CHECK(reply[0] == 0 && reply[3] == 1);
	read_full(client, pixels, sizeof(pixels));
	CHECK(memcmp(pixels, surface->pixels, sizeof(pixels)) == 0);

	memset(msg, 0, 6);
	msg[0] = 5;
	msg[1] = 1;
	value16 = htobe16(1);
	memcpy(&msg[2], &value16, sizeof(value16));
	memcpy(&msg[4], &value16, sizeof(value16));
	write_full(client, msg, 6);
	read_full(control, &input, sizeof(input));
	CHECK(input.type == DISPLAY_INPUT_POINTER && input.buttons == 1);
	CHECK(input.x == 1 && input.y == 1);

	close(client);
	close(control);
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
}

static void
test_encoding_limit(struct display_surface *surface)
{
	uint8_t msg[4];
	uint16_t count, width, height;
	int client, control, status;
	pid_t pid;

	pid = start_server(surface, &client, &control);
	handshake(client, &width, &height);
	msg[0] = 2;
	msg[1] = 0;
	count = htobe16(65);
	memcpy(&msg[2], &count, sizeof(count));
	write_full(client, msg, sizeof(msg));
	CHECK(read(client, msg, 1) == 0);
	close(client);
	close(control);
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
}

int
main(void)
{
	struct display_surface *surface;
	uint8_t pixels[16];
	int i;

	CHECK((surface = mmap(NULL, display_surface_size(),
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0)) != MAP_FAILED);
	display_surface_init(surface);
	for (i = 0; i < (int)sizeof(pixels); i++)
		pixels[i] = i;
	CHECK(display_surface_update(surface, pixels, 2, 2, 8,
	    DISPLAY_FORMAT_XRGB8888) == 0);
	test_protocol(surface);
	test_encoding_limit(surface);
	CHECK(munmap(surface, display_surface_size()) == 0);
	return (0);
}
