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

#ifndef _DEV_USB_IF_URTWXREG_H_
#define _DEV_USB_IF_URTWXREG_H_

#include <sys/types.h>

#define RTW_USB_CMD_REQ			0x05

#define REG_SYS_FUNC_EN			0x0002
#define REG_SYS_CLK_CTRL		0x0008
#define REG_RSV_CTRL			0x001c
#define REG_EFUSE_CTRL			0x0030
#define REG_LDO_EFUSE_CTRL		0x0034
#define REG_GPIO_MUXCFG			0x0040
#define REG_MCUFW_CTRL			0x0080
#define REG_SYS_STATUS1			0x00f4
#define REG_CR				0x0100
#define REG_TXDMA_PQ_MAP		0x010c
#define REG_FIFOPAGE_CTRL_2		0x0204
#define REG_TXDMA_STATUS		0x0210
#define REG_RQPN_CTRL_2			0x022c
#define REG_FIFOPAGE_INFO_1		0x0230
#define REG_BCN_CTRL			0x0550
#define REG_CPU_DMEM_CON		0x1080
#define REG_FW_DBG7			0x10fc
#define REG_CR_EXT			0x1100
#define REG_DDMA_CH0SA			0x1200
#define REG_DDMA_CH0DA			0x1204
#define REG_DDMA_CH0CTRL		0x1208
#define REG_H2CQ_CSR			0x1330

#define BIT_FEN_CPUEN			0x04
#define BIT_CPU_CLK_EN			0x4000
#define BIT_WLMCU_IOIF			0x01
#define BIT_EF_FLAG			0x80000000U
#define BIT_MASK_EF_DATA		0x000000ffU
#define BIT_SHIFT_EF_ADDR		8
#define BIT_MASK_EF_ADDR		0x000003ffU
#define BITS_EF_ADDR			(BIT_MASK_EF_ADDR << BIT_SHIFT_EF_ADDR)
#define BIT_MASK_EFUSE_BANK_SEL		0x00000300U
#define BIT_LDO25_EN			0x80
#define BIT_FSPI_EN			0x00080000U
#define BIT_BOOT_FSPI_EN		0x00100000U
#define BIT_FW_INIT_RDY			0x8000
#define BIT_FW_DW_RDY			0x4000
#define BIT_CPU_CLK_SEL			0x3000
#define BIT_DMEM_CHKSUM_OK		0x0040
#define BIT_DMEM_DW_OK			0x0020
#define BIT_IMEM_CHKSUM_OK		0x0010
#define BIT_IMEM_DW_OK			0x0008
#define BIT_MCUFWDL_EN			0x0001
#define BIT_CHECK_SUM_OK		\
	(BIT_DMEM_CHKSUM_OK | BIT_IMEM_CHKSUM_OK)
#define FW_READY			\
	(BIT_FW_INIT_RDY | BIT_FW_DW_RDY | BIT_DMEM_DW_OK |	\
	 BIT_IMEM_DW_OK | BIT_CHECK_SUM_OK)
#define FW_READY_MASK			(0xffff & ~BIT_CPU_CLK_SEL)
#define BIT_ENSWBCN			0x00000100U
#define BIT_TXDMA_EN			0x04
#define BIT_HCI_TXDMA_EN		0x01
#define BIT_BCN_VALID_V1		0x8000
#define BIT_MASK_BCN_HEAD_1_V1		0x0fff
#define BIT_LD_RQPN			0x80000000U
#define BIT_DIS_TSF_UDT			0x10
#define BIT_EN_BCN_FUNCTION		0x08
#define BIT_H2CQ_FULL			0x80000000U
#define BIT_WL_PLATFORM_RST		0x00010000U
#define BIT_DDMA_EN			0x00000100U
#define BIT_DDMACH0_OWN			0x80000000U
#define BIT_DDMACH0_CHKSUM_EN		0x20000000U
#define BIT_DDMACH0_CHKSUM_STS		0x08000000U
#define BIT_DDMACH0_DDMA_MODE		0x04000000U
#define BIT_DDMACH0_RESET_CHKSUM_STS	0x02000000U
#define BIT_DDMACH0_CHKSUM_CONT		0x01000000U
#define BIT_MASK_DDMACH0_DLEN		0x0003ffffU
#define BTI_PAGE_OVF			0x80000000U
#define FW_KEY_MASK			0xffffff00U
#define ILLEGAL_KEY_GROUP		0xfaaaaa00U

#define OCPBASE_IMEM_88XX		0x00030000U
#define OCPBASE_EMEM_88XX		0x00100000U
#define OCPBASE_DMEM_88XX		0x00200000U
#define OCPBASE_TXBUF_88XX		0x18780000U

#define FW_HDR_SIZE			64
#define FW_HDR_CHKSUM_SIZE		8

struct rtw_fw_hdr {
	uint16_t	signature;
	uint8_t		category;
	uint8_t		function;
	uint16_t	version;
	uint8_t		subversion;
	uint8_t		subindex;
	uint32_t	rsvd0;
	uint32_t	feature;
	uint8_t		month;
	uint8_t		day;
	uint8_t		hour;
	uint8_t		minute;
	uint16_t	year;
	uint16_t	rsvd1;
	uint8_t		mem_usage;
	uint8_t		rsvd2[3];
	uint16_t	h2c_fmt_ver;
	uint16_t	rsvd3;
	uint32_t	dmem_addr;
	uint32_t	dmem_size;
	uint32_t	rsvd4;
	uint32_t	rsvd5;
	uint32_t	imem_size;
	uint32_t	emem_size;
	uint32_t	emem_addr;
	uint32_t	imem_addr;
} __packed;

#endif
