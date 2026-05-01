/*	$OpenBSD */

/*
 * Copyright (c) 2024 Mike Larkin <mlarkin@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>

#define REG_MCUFW_CTRL				0x0080
#define BIT_MCUFWDL_EN				(1U << 0)
#define BIT_FW_DW_RDY				(1U << 7)
#define BIT_CHECK_SUM_OK			(1U << 8)

// Firmware validation
#define REG_SYS_FUNC_EN				0x0002
#define BIT_FEN_CPUEN				(1U << 7)

// DMA engine
#define REG_DDMA_CH0SA				0x1200
#define REG_DDMA_CH0DA				0x1204
#define REG_DDMA_CH0CTRL			0x1208
#define BIT_DDMACH0_OWN				(1U << 31)
#define BIT_DDMACH0_CHKSUM_EN			(1U << 16)
#define BIT_DDMACH0_RESET_CHKSUM_STS		(1U << 19)

// TX DMA status (page overflow clear)
#define REG_TXDMA_STATUS			0x0210
#define BTI_PAGE_OVF				(1U << 31)

// Firmware header (64 bytes — defined in fw.h)
struct rtw_fw_hdr {
	uint16_t signature;
	uint8_t  category;
	uint8_t  function;
	uint16_t version;      // offset 0x04
	uint8_t  subversion;
	uint8_t  subindex;
	uint32_t rsvd;        // offset 0x08
	uint32_t feature;     // offset 0x0C
	uint8_t  month;        // offset 0x10
	uint8_t  day;
	uint8_t  hour;
	uint8_t  min;
	uint16_t year;        // offset 0x14
	uint16_t rsvd3;
	uint8_t  mem_usage;   // offset 0x18 — BIT(4) = has EMEM
	uint8_t  rsvd4[3];
	uint16_t h2c_fmt_ver; // offset 0x1C
	uint32_t dmem_addr;   // offset 0x20
	uint32_t dmem_size;   // offset 0x24
	uint32_t rsvd6;
	uint32_t rsvd7;
	uint32_t imem_size;   // offset 0x30
	uint32_t emem_size;    // offset 0x34
	uint32_t emem_addr;   // offset 0x38
	uint32_t imem_addr;   // offset 0x3C
} __packed;

// Firmware block size — RTL8822BU/RTL8821CU use 196 bytes per block
// (RTL8723D uses 254)
#define FW_DL_BLOCK_SIZE	196

// Firmware starting address in chip memory
#define FW_START_ADDR		0x1000

// USB control request for firmware download
// (already in your code as the default cmd = 0x05)
#define RTW_USB_CMD_READ	0xc0
#define RTW_USB_CMD_WRITE	0x40
#define RTW_USB_CMD_REQ		0x05
