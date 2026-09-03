/*	$OpenBSD$	*/

#include <sys/types.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fw_cfg.h"
#include "ramfb.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
		    __FILE__, __LINE__, #expr); \
		exit(1); \
	} \
} while (0)

struct wire_config {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} __packed;

/* ramfb_init() is not exercised by this parser regression. */
void
fw_cfg_add_file_callback(const char *name, const void *data, size_t len,
    fw_cfg_write_cb cb, void *arg)
{
}

static struct wire_config
valid_config(void)
{
	struct wire_config wire;

	wire.address = htobe64(0x12345000ULL);
	wire.fourcc = htobe32(RAMFB_FOURCC_XRGB8888);
	wire.flags = htobe32(0);
	wire.width = htobe32(800);
	wire.height = htobe32(600);
	wire.stride = htobe32(3200);
	return (wire);
}

int
main(void)
{
	struct wire_config wire;
	struct ramfb_config cfg;

	wire = valid_config();
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == 0);
	CHECK(cfg.address == 0x12345000ULL);
	CHECK(cfg.width == 800 && cfg.height == 600 && cfg.stride == 3200);

	errno = 0;
	CHECK(ramfb_parse_config(&wire, sizeof(wire) - 1, &cfg) == -1);
	CHECK(errno == EINVAL);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), NULL) == -1);

	wire = valid_config();
	wire.fourcc = htobe32(0);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);
	wire = valid_config();
	wire.flags = htobe32(1);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);
	wire = valid_config();
	wire.width = htobe32(0);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);
	wire = valid_config();
	wire.height = htobe32(RAMFB_MAX_HEIGHT + 1);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);
	wire = valid_config();
	wire.stride = htobe32(3199);
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);

	wire = valid_config();
	wire.address = htobe64(UINT64_MAX - 1024);
	errno = 0;
	CHECK(ramfb_parse_config(&wire, sizeof(wire), &cfg) == -1);
	CHECK(errno == EOVERFLOW);

	return (0);
}
