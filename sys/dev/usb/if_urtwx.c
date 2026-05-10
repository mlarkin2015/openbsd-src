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

#include "bpfilter.h"

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#include <sys/timeout.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/malloc.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <net80211/ieee80211_var.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usbdevs.h>

#include <dev/usb/if_urtwxreg.h>
#include <dev/usb/if_urtwxtbl.h>

#define URTWX_CTL_READ			1
#define URTWX_CTL_WRITE			2

#define URTWX_ENDPT_RX			0
#define URTWX_ENDPT_TX			1
#define URTWX_ENDPT_INTR		2
#define URTWX_ENDPT_MAX			3
#define URTWX_MAX_OUT_PIPES		4

#define URTWX_MAX_FW_CHUNK		0x1000
#define URTWX_TXDESC_SIZE		48
#define URTWX_TXDESC_CSUM_IDX		14
#define URTWX_TXDESC_QSEL_BE		0
#define URTWX_TXDESC_QSEL_BEACON	16
#define URTWX_TXDESC_QSEL_HIGH		17
#define URTWX_TXDESC_QSEL_MGMT		18
#define URTWX_TXDESC_QSEL_H2C		19
#define URTWX_TX_TIMEOUT		1000
#define URTWX_SCAN_DWELL_TIME		200
#define URTWX_HOST_CMD_RING_COUNT	8
#define URTWX_HOST_CMD_DATA_LEN		256
#define URTWX_FORCE_PASSIVE_SCAN	0
#define URTWX_FORCE_DATA_MGMT_Q		0
#define URTWX_REFUSE_RX_ADDBA		0
#define URTWX_RX_BA_NO_AMSDU		1
#define URTWX_SCAN_LOG_MAX		64
#define URTWX_STATE_LOG_MAX		128
#define URTWX_TX_MGMT_LOG_MAX		32
#define URTWX_TX_DATA_LOG_MAX		16
#define URTWX_C2H_LOG_MAX		16
#define URTWX_C2H_TX_RPT_MAX		16
#define URTWX_TX_RPT_REQ_MAX		8
#define URTWX_8822C_FORCE_MGMT_OUT	-1
#define URTWX_8822C_FORCE_TRX_B_ONLY	0
#define URTWX_8822C_USBCTL_LOG_MAX	0
#define URTWX_8822C_RFIO_LOG_MAX	0
#define URTWX_8822C_RF_DELAY_US		1
#define URTWX_8822C_RF_SIPI_DELAY_US	13
#define URTWX_8822C_TBLWRITE_PACE_US	5000
#define URTWX_8822C_RF0_RETRY_MAX	50
#define URTWX_8822C_RF0_RETRY_DELAY_US	5000
#define URTWX_8822C_RFWRITE_RAW_MAX	64
#define URTWX_8822C_RFBLOCK_MAX		128
#define URTWX_8822C_RFBLOCK_SHIFT	8
#define URTWX_8822C_USBTRACE_MAX	64
#define URTWX_TXDESC_W0_BMC		0x01000000U
#define URTWX_TXDESC_W0_LS		0x04000000U
#define URTWX_TXDESC_W0_DISQSELSEQ	0x80000000U
#define URTWX_TXDESC_W1_MACID_MASK	0x000000ffU
#define URTWX_TXDESC_W1_QSEL_SHIFT	8
#define URTWX_TXDESC_W1_RATEID_SHIFT	16
#define URTWX_TXDESC_W1_SEC_TYPE_SHIFT	22
#define URTWX_TXDESC_W1_PKT_OFFSET_SHIFT 24
#define URTWX_TXDESC_W1_PKT_OFFSET_MASK	0x1f000000U
#define URTWX_TXDESC_W1_MORE_DATA	0x20000000U
#define URTWX_TXDESC_W2_AGG_EN		0x00001000U
#define URTWX_TXDESC_W2_SPE_RPT		0x00080000U
#define URTWX_TXDESC_W2_AMPDU_DEN_SHIFT	20
#define URTWX_TXDESC_W2_BT_NULL		0x00800000U
#define URTWX_TXDESC_W3_USE_RATE	0x00000100U
#define URTWX_TXDESC_W3_DISDATAFB	0x00000400U
#define URTWX_TXDESC_W3_MAX_AGGNUM_SHIFT 17
#define URTWX_TXDESC_W3_NAVUSEHDR	0x00008000U
#define URTWX_TXDESC_W5_DATA_SHORT	0x00000010U
#define URTWX_TXDESC_W5_DATA_LDPC	0x00000080U
#define URTWX_TXDESC_W6_SWDEF_MASK	0x00000fffU
#define URTWX_TXDESC_W8_EN_HWSEQ	0x00008000U
#define URTWX_TXDESC_W9_SWSEQ_SHIFT	12
#define URTWX_HTCAP_BASE		IEEE80211_HTCAP_SGI20
#define URTWX_RATEID_BGN_40M_2SS	0
#define URTWX_RATEID_BGN_40M_1SS	1
#define URTWX_RATEID_BGN_20M_2SS	2
#define URTWX_RATEID_BGN_20M_1SS	3
#define URTWX_RATEID_GN_N2SS		4
#define URTWX_RATEID_GN_N1SS		5
#define URTWX_RATEID_BG			6
#define URTWX_RATEID_G			7
#define URTWX_RATEID_B_20M		8
#define URTWX_RATEID_ARFR0_AC_2SS	9
#define URTWX_RATEID_ARFR1_AC_1SS	10
#define URTWX_RATEID_ARFR2_AC_2G_1SS	11
#define URTWX_RATEID_ARFR3_AC_2G_2SS	12
#define URTWX_CHANNEL_WIDTH_20		0
#define URTWX_CHANNEL_WIDTH_40		1
#define URTWX_CHANNEL_WIDTH_80		2
#define URTWX_SC_DONT_CARE		0
#define URTWX_SC_20_UPPER		1
#define URTWX_SC_20_LOWER		2
#define URTWX_SC_20_UPMOST		3
#define URTWX_SC_20_LOWEST		4
#define URTWX_SC_40_UPPER		9
#define URTWX_SC_40_LOWER		10
#define URTWX_DESC_RATE1M		0x00
#define URTWX_DESC_RATE11M		0x03
#define URTWX_DESC_RATE6M		0x04
#define URTWX_DESC_RATE54M		0x0b
#define URTWX_DESC_RATEMCS7		0x13
#define URTWX_DESC_RATEMCS15		0x1b
#define URTWX_DESC_RATEVHT1SS_MCS9	0x35
#define URTWX_DESC_RATEVHT2SS_MCS9	0x3f
#define URTWX_SEC_TYPE_WEP40		0x01
#define URTWX_SEC_TYPE_TKIP		0x01
#define URTWX_SEC_TYPE_WEP104		0x01
#define URTWX_SEC_TYPE_CCMP		0x03
#define URTWX_H2C_CMD_RSVD_PAGE		0x00
#define URTWX_H2C_CMD_MEDIA_STATUS_RPT	0x01
#define URTWX_H2C_CMD_DEFAULT_PORT	0x2c
#define URTWX_H2C_CMD_RA_INFO		0x40

#define URTWX_H2C_PKT_SIZE		32
#define URTWX_H2C_PKT_HDR_SIZE		8
#define URTWX_H2C_PKT_CATEGORY		0x01
#define URTWX_H2C_PKT_CMD_ID		0xff
#define URTWX_H2C_PKT_GENERAL_INFO	0x0d
#define URTWX_H2C_PKT_PHYDM_INFO	0x11

#define URTWX_FW_RF_2T2R		2
#define URTWX_FW_RF_1T1R		4
#define URTWX_BB_PATH_A			0x01
#define URTWX_BB_PATH_B			0x02
#define URTWX_BB_PATH_AB		(URTWX_BB_PATH_A | URTWX_BB_PATH_B)
#define URTWX_CAM_ENTRY_WORDS		8
#define URTWX_CAM_ENTRY_SHIFT		3
#define URTWX_CAM_PAIRWISE_ENTRY	4
#define URTWX_CAM_ALGO_CCMP		4
#define URTWX_SEC_CMD_WRITE_ENABLE	0x00010000U
#define URTWX_SEC_CMD_CLEAR		0x40000000U
#define URTWX_SEC_CMD_POLLING		0x80000000U
#define URTWX_C2H_CCX_TX_RPT		0x03
#define URTWX_C2H_RA_RPT		0x0c
#define URTWX_C2H_WLAN_INFO		0x27
#define URTWX_C2H_BCN_FILTER_NOTIFY	0x36
#define URTWX_C2H_SCAN_RESULT		0x38
#define URTWX_C2H_HW_FEATURE_REPORT	0x19
#define URTWX_C2H_HW_FEATURE_DUMP	0xfd
#define URTWX_C2H_HALMAC		0xff
#define URTWX_HW_FEATURE_LEN		13
#define URTWX_GET_CCX_RPT_SN_V0(p)	((p)[6] & 0xfc)
#define URTWX_GET_CCX_RPT_ST_V0(p)	((p)[0] & 0xc0)
#define URTWX_MACID_STA			0
#define URTWX_PORTID_STA		0
#define URTWX_RA_MASK_CCK_RATES		0x0000000fU
#define URTWX_RA_MASK_OFDM_RATES	0x00000ff0U
#define URTWX_RA_MASK_HT_RATES_1SS	0x000ff000U
#define URTWX_RA_MASK_HT_RATES_2SS	0x0ff00000U
#define URTWX_RA_MASK_VHT_RATES_1SS	0x3ff000U
#define URTWX_RA_MASK_VHT_RATES_2SS	0xffc00000U

#define URTWX_RX_LIST_COUNT		16
#define URTWX_RXBUFSZ			32768
#define URTWX_RX_DESC_SIZE		24
#define URTWX_RX_MGMT_LOG_MAX		64

#define URTWX_WAIT_COUNT		1000
#define URTWX_WAIT_DELAY_US		10
#define URTWX_PWR_POLL_COUNT		20000
#define URTWX_PWR_POLL_DELAY_US		50
#define URTWX_EFUSE_POLL_COUNT		100000
#define URTWX_EFUSE_POLL_DELAY_US	1

#define URTWX_PAGE_SIZE			128
#define URTWX_C2H_PKT_BUF		256
#define URTWX_PHY_STATUS_SIZE		4
#define URTWX_USB_TX_AGG_DESC_NUM	3
#define URTWX_RSVD_H2C_EXTRA_PG_NUM	24
#define URTWX_RSVD_H2C_STATIC_PG_NUM	8
#define URTWX_RSVD_H2CQ_PG_NUM		8
#define URTWX_RSVD_CPU_INSTR_PG_NUM	0
#define URTWX_RSVD_FW_TXBUF_PG_NUM	4
#define URTWX_RSVD_PAGE_MARGIN		(URTWX_PAGE_SIZE - URTWX_TXDESC_SIZE)
#define URTWX_DLFW_HIQ_PGNUM		0x200
#define URTWX_SYS_FUNC_EN_8821C		0xd8
#define URTWX_DMA_MAPPING_HIGH		3

#define URTWX_EFUSE_MAC_OFFSET		0x107
#define URTWX_EFUSE_8822C_USB_MODE	0x006
#define URTWX_EFUSE_8822C_TXPWR_IDX	0x010
#define URTWX_EFUSE_8822C_CHANNEL_PLAN	0x0b8
#define URTWX_EFUSE_8822C_XTAL_K	0x0b9
#define URTWX_EFUSE_8822C_RF_BOARD_OPT	0x0c1
#define URTWX_EFUSE_8822C_RF_BT_SETTING 0x0c3
#define URTWX_EFUSE_8822C_TX_BB_SWING_2G 0x0c6
#define URTWX_EFUSE_8822C_TX_BB_SWING_5G 0x0c7
#define URTWX_EFUSE_8822C_RFE_OPTION	0x0ca
#define URTWX_EFUSE_8822C_PPG_THERMAL_B	0x1b0
#define URTWX_EFUSE_8822C_PPG_2GH_TXAB	0x1d2
#define URTWX_EFUSE_8822C_PPG_2GL_TXAB	0x1d4
#define URTWX_EFUSE_8822C_PPG_PABIAS_2GB 0x1d5
#define URTWX_EFUSE_8822C_PPG_PABIAS_2GA 0x1d6
#define URTWX_EFUSE_8822C_PPG_PABIAS_5GB 0x1d7
#define URTWX_EFUSE_8822C_PPG_PABIAS_5GA 0x1d8
#define URTWX_EFUSE_8822C_PPG_5GH1_TXB	0x1db
#define URTWX_EFUSE_8822C_PPG_5GH1_TXA	0x1dc
#define URTWX_EFUSE_8822C_PPG_5GM2_TXB	0x1df
#define URTWX_EFUSE_8822C_PPG_5GM2_TXA	0x1e0
#define URTWX_EFUSE_8822C_PPG_5GM1_TXB	0x1e3
#define URTWX_EFUSE_8822C_PPG_5GM1_TXA	0x1e4
#define URTWX_EFUSE_8822C_PPG_5GL2_TXB	0x1e7
#define URTWX_EFUSE_8822C_PPG_5GL2_TXA	0x1e8
#define URTWX_EFUSE_8822C_PPG_5GL1_TXB	0x1eb
#define URTWX_EFUSE_8822C_PPG_5GL1_TXA	0x1ec
#define URTWX_EFUSE_8822C_PPG_2GM_TXAB	0x1ee
#define URTWX_EFUSE_8822C_PPG_THERMAL_A	0x1ef
#define URTWX_EFUSE_8822CU_MAC_OFFSET	0x157

#define URTWX_WLAN_SLOT_TIME		0x09
#define URTWX_WLAN_PIFS_TIME		0x19
#define URTWX_WLAN_SIFS_CFG		0x10100e0aU
#define URTWX_WLAN_VO_TXOP_LIMIT	0x0186
#define URTWX_WLAN_VI_TXOP_LIMIT	0x03bc
#define URTWX_WLAN_TBTT_TIME		0x00006404U
#define URTWX_WLAN_NAV_CFG		0x001b0005U
#define URTWX_WLAN_RX_TSF_CFG		0x3030
#define URTWX_WLAN_DRV_EARLY_INT	0x04
#define URTWX_WLAN_BCN_DMA_TIME		0x02
#define URTWX_WLAN_RX_FILTER0		0x0fffffffU
#define URTWX_WLAN_RX_FILTER2		0xffff
#define URTWX_WLAN_RCR_CFG		0xe400220eU
#define URTWX_WLAN_RXPKT_MAX_SZ_512	24
#define URTWX_WLAN_AMPDU_MAX_TIME	0x70
#define URTWX_WLAN_PRE_TXCNT		0x09e4
#define URTWX_WLAN_PROT_MODE_CTRL	0x202008ffU
#define URTWX_WLAN_BAR_MODE_CTRL	0x0801
#define URTWX_FAST_EDCA_TH		0x06
#define URTWX_WLAN_TX_FUNC_CFG1		0x30
#define URTWX_WLAN_TX_FUNC_CFG2		0x30
#define URTWX_WLAN_MAC_OPT_NORM_FUNC1	0x98
#define URTWX_WLAN_MAC_OPT_FUNC2	0xb0810041U
#define URTWX_8822C_REG_INT_MIG		0x0304
#define URTWX_8822C_REG_SPEC_SIFS	0x0428
#define URTWX_8822C_REG_DARFRC		0x0430
#define URTWX_8822C_REG_DARFRCH		0x0434
#define URTWX_8822C_REG_RARFRCH		0x043c
#define URTWX_8822C_REG_RRSR		0x0440
#define URTWX_8822C_REG_ARFR0		0x0444
#define URTWX_8822C_DACK_PATHS		2
#define URTWX_8822C_DACK_REGS		16
#define URTWX_8822C_DACK_RF_REGS	1
#define URTWX_8822C_DACK_MSBK_NUM	15
#define URTWX_8822C_DACK_DCK_NUM	2
#define URTWX_8822C_RF_TABLE_TRACE_MAX	8
#define URTWX_8822C_ABORT_AFTER_TABLES	1
#define URTWX_8822C_ABORT_AFTER_TRXMODE	0
#define URTWX_8822C_ABORT_BEFORE_DACK	0
#define URTWX_8822C_ABORT_AFTER_DACK_BB	0
#define URTWX_8822C_ABORT_AFTER_ADCK_PREP	0
#define URTWX_8822C_ABORT_AFTER_PHYINIT	0
#define URTWX_8822C_ABORT_AFTER_CONFIG_TRX	0
#define URTWX_8822C_ABORT_AFTER_PHYON	0
#define URTWX_8822C_DACK_VERBOSE	0
#define URTWX_8822C_DACK_SAMPLES	100
#define URTWX_8822C_DACK_TRIES		10
#define URTWX_8822C_REG_DCKA_I_0	0x18bc
#define URTWX_8822C_REG_DCKA_I_1	0x18c0
#define URTWX_8822C_REG_DCKA_Q_0	0x18d8
#define URTWX_8822C_REG_DCKA_Q_1	0x18dc
#define URTWX_8822C_REG_DCKB_I_0	0x41bc
#define URTWX_8822C_REG_DCKB_I_1	0x41c0
#define URTWX_8822C_REG_DCKB_Q_0	0x41d8
#define URTWX_8822C_REG_DCKB_Q_1	0x41dc
#define URTWX_8822C_REG_ARFRH0		0x0448
#define URTWX_8822C_REG_ARFR1_V1	0x044c
#define URTWX_8822C_REG_ARFRH1_V1	0x0450
#define URTWX_8822C_REG_ARFR4		0x049c
#define URTWX_8822C_REG_ARFRH4		0x04a0
#define URTWX_8822C_REG_ARFR5		0x04a4
#define URTWX_8822C_REG_ARFRH5		0x04a8
#define URTWX_8822C_REG_EDCA_BE_PARAM	0x0508
#define URTWX_8822C_REG_EDCA_BK_PARAM	0x050c
#define URTWX_8822C_REG_RD_CTRL		0x0524
#define URTWX_8822C_REG_BCN_CTRL_CLINT0	0x0551
#define URTWX_8822C_REG_BCN_MAX_ERR	0x055d
#define URTWX_8822C_REG_MISC_CTRL	0x0577
#define URTWX_8822C_REG_MAR		0x0620
#define URTWX_8822C_REG_RESP_SIFS_CCK	0x063c
#define URTWX_8822C_REG_RESP_SIFS_OFDM	0x063e
#define URTWX_8822C_REG_ACKTO		0x0640
#define URTWX_8822C_REG_EIFS		0x0642
#define URTWX_8822C_REG_NAV_CTRL	0x0650
#define URTWX_8822C_REG_BBPSF_CTRL	0x06dc
#define URTWX_8822C_REG_RXPSF_CTRL	0x1610
#define URTWX_8822C_REG_RXPSF_TYPE_CTRL	0x1614
#define URTWX_8822C_REG_GENERAL_OPTION	0x1664
#define URTWX_8822C_REG_ANTMAP0		0x0820
#define URTWX_8822C_REG_ANTMAP		0x0824
#define URTWX_8822C_REG_DIS_DPD		0x0a70
#define URTWX_8822C_REG_MRCM		0x0c38
#define URTWX_8822C_REG_AGCSWSH		0x0c44
#define URTWX_8822C_REG_ANTWTPD		0x0c54
#define URTWX_8822C_REG_ORITXCODE	0x1800
#define URTWX_8822C_REG_3WIRE		0x180c
#define URTWX_8822C_REG_ANAPAR_A	0x1830
#define URTWX_8822C_REG_RXCCKSEL	0x1a04
#define URTWX_8822C_REG_CCANRX		0x1a2c
#define URTWX_8822C_REG_ENCCK		0x1c3c
#define URTWX_8822C_REG_RSTB		0x1c90
#define URTWX_8822C_REG_RXFNCTL		0x1d30
#define URTWX_8822C_REG_RXIGI		0x1d70
#define URTWX_8822C_REG_TXLGMAP		0x1e2c
#define URTWX_8822C_REG_TXPWR_REF_A_CCK	0x18a0
#define URTWX_8822C_REG_TXPWR_REF_B_CCK	0x41a0
#define URTWX_8822C_REG_TXPWR_REF_A_OFDM 0x18e8
#define URTWX_8822C_REG_TXPWR_REF_B_OFDM 0x41e8
#define URTWX_8822C_REG_TXAGC_BASE	0x3a00
#define URTWX_8822C_REG_ORITXCODE2	0x4100
#define URTWX_8822C_REG_3WIRE2		0x410c
#define URTWX_8822C_REG_ANAPAR_B	0x4130
#define URTWX_8822C_WLAN_TXQ_RPT_EN	0x1f
#define URTWX_8822C_WLAN_PIFS_TIME	0x1c
#define URTWX_8822C_WLAN_SPEC_SIFS	0x100a
#define URTWX_8822C_WLAN_RESP_SIFS_CCK	0x0a0a
#define URTWX_8822C_WLAN_RESP_SIFS_OFDM 0x0e0e
#define URTWX_8822C_WLAN_DARFRC		0x01000000U
#define URTWX_8822C_WLAN_DARFRCH	0x08070504U
#define URTWX_8822C_WLAN_RARFRCH	0x08070504U
#define URTWX_8822C_WLAN_ARFR0		0xfe01f010U
#define URTWX_8822C_WLAN_ARFRH0		0x40000000U
#define URTWX_8822C_WLAN_ARFR1		0x003ff010U
#define URTWX_8822C_WLAN_ARFRH1		0x40000000U
#define URTWX_8822C_WLAN_ARFR4		0x0600f010U
#define URTWX_8822C_WLAN_ARFRH4		0x400003e0U
#define URTWX_8822C_WLAN_ARFR5		0x0600f015U
#define URTWX_8822C_WLAN_ARFRH5		0x000000e0U
#define URTWX_8822C_WLAN_PROT_MODE_CTRL	0x3f3f08ffU
#define URTWX_8822C_WLAN_EDCA_BE_PARAM	0x005ea42bU
#define URTWX_8822C_WLAN_EDCA_BK_PARAM	0x0000a44fU
#define URTWX_8822C_WLAN_BCN_CTRL_CLT0	0x10
#define URTWX_8822C_WLAN_BCN_MAX_ERR	0xff
#define URTWX_8822C_WLAN_MULTI_ADDR	0xffffffffU
#define URTWX_8822C_WLAN_RESP_TXRATE	0x84
#define URTWX_8822C_WLAN_ACKTO		0x21
#define URTWX_8822C_WLAN_ACKTO_CCK	0x6a
#define URTWX_8822C_WLAN_EIFS		0x40
#define URTWX_8822C_WLAN_NAV_MAX	0xc8
#define URTWX_8822C_WLAN_MAC_INT_MIG	0x33330000U
#define URTWX_8822C_BIT_BA_PARSER_EN	0x20
#define URTWX_8822C_BITS_RRSR_RSC	0x00600000U
#define URTWX_8822C_MISC_CTRL		0x0b
#define URTWX_8822C_RD_CTRL1		0x07
#define URTWX_8822C_BAR_ACK_TYPE	0x05
#define URTWX_8822C_RXPSF_CTRL2_MASK	0xf00fU
#define URTWX_8822C_RXPSF_CTRL2_VAL	0x15500000U
#define URTWX_8822C_RXPSF_CTRL_VAL	0x1f87U
#define URTWX_8822C_RXPSF_CTRL_MASK	0x0070U
#define URTWX_8822C_RXPSF_CTRL_FIXUP	0x0007U
#define URTWX_8822C_GENERAL_OPTION	0x00000200U
#define URTWX_8822C_GNT_HW_PTA		0x0
#define URTWX_8822C_GNT_SW_LOW		0x1
#define URTWX_8822C_GNT_SW_HIGH		0x3
#define URTWX_8822C_DIS_DPD_MASK	0x000003ffU
#define URTWX_8822C_DIS_DPD_RATEALL	0x000003ffU
#define URTWX_8822C_BIT_WLRF1_BBRF_EN	0x07000000U
#define URTWX_8822C_BITS_RFC_DIRECT	0xc0000000U
#define URTWX_8822C_RFC_DIRECT_OFF	0x0
#define URTWX_8822C_RFC_DIRECT_PI	0x2
#define URTWX_8822C_BIT_3WIRE_TX_EN	0x00000001U
#define URTWX_8822C_BIT_3WIRE_RX_EN	0x00000002U
#define URTWX_8822C_BIT_3WIRE_PI_ON	0x10000000U
#define URTWX_8822C_BIT_ANAPAR_UPDATE	0x20000000U
#define URTWX_8822C_BIT_CCK_OFDM_BLK_EN 0x00000003U
#define URTWX_8822C_BIT_RSTB_3WIRE	0x00000100U
#define URTWX_8822C_BIT_TXAGC_RSTB	0x00008000U
#define URTWX_8822C_BB_PATH_A		0x01
#define URTWX_8822C_BB_PATH_B		0x02
#define URTWX_8822C_BB_PATH_AB		0x03
#define URTWX_8822C_TXPWR_REF_DEFAULT	0x48
#define URTWX_RF_THEMAL_MASK		0x000f0000U
#define URTWX_RF_PABIAS_2G_MASK		0x0000f000U
#define URTWX_RF_PABIAS_5G_MASK		0x000f0000U
#define URTWX_8822C_PPG_2G_A_MASK	0x0f
#define URTWX_8822C_PPG_2G_B_MASK	0xf0
#define URTWX_8822C_PPG_5G_MASK		0x1f
#define URTWX_8822C_PPG_PABIAS_MASK	0x0f
#define URTWX_RF_PA			0x60

#define URTWX_FIXED_CHANNEL		1
#define URTWX_MAC_CLK_SPEED		80
#define URTWX_RX_AGG_SIZE		0x05
#define URTWX_RX_AGG_TIMEOUT		0x20
#define URTWX_RF_BASE_A			0x2800
#define URTWX_RF_BASE_B			0x2c00
#define URTWX_RF_SIPI_A			0x0c90
#define URTWX_RF_SIPI_B			0x0e90
#define URTWX_RFE_OPTION_BTG_MASK	0x1f
#define URTWX_RFE_OPTION_PKG_MASK	0x20
#define URTWX_XTAL_CAP_MASK		0x3f
#define URTWX_LNA_2G_WLG		0x7532
#define URTWX_LNA_2G_BTG		0xfc84
#define URTWX_CCA_2G_WLG		0x12
#define URTWX_CCA_2G_BTG		0x0e

#define URTWX_PHY_BRANCH_IF		0
#define URTWX_PHY_BRANCH_ELIF		1
#define URTWX_PHY_BRANCH_ELSE		2
#define URTWX_PHY_BRANCH_ENDIF		3
#define URTWX_PHY_COND_POS		0x80000000U
#define URTWX_PHY_COND_NEG		0x40000000U
#define URTWX_PHY_COND_BRANCH_MASK	0x30000000U
#define URTWX_PHY_COND_BRANCH_SHIFT	28
#define URTWX_PHY_COND_CUT_MASK		0x0f000000U
#define URTWX_PHY_COND_CUT_SHIFT	24
#define URTWX_PHY_COND_PKG_MASK		0x0000f000U
#define URTWX_PHY_COND_PKG_SHIFT	12
#define URTWX_PHY_COND_INTF_MASK	0x00000f00U
#define URTWX_PHY_COND_INTF_SHIFT	8
#define URTWX_PHY_COND_RFE_MASK		0x000000ffU
#define URTWX_PHY_COND_WILDCARD		0x0f
#define URTWX_PHY_COND_INTF_USB		0x02

#define URTWX_EFUSE_XTAL_K_OFFSET	0x0b9
#define URTWX_EFUSE_TX_BB_SWING_2G	0x0c6
#define URTWX_EFUSE_TX_BB_SWING_5G	0x0c7
#define URTWX_EFUSE_RFE_OPTION		0x0ca

#define URTWX_DEBUG 1

#ifdef URTWX_DEBUG
#define DPRINTF(x)	do { if (urtwxdebug) printf x; } while (0)
#define DPRINTFN(n, x)	do { if (urtwxdebug >= (n)) printf x; } while (0)
int	urtwxdebug = 0;
#else
#define DPRINTF(x)
#define DPRINTFN(n, x)
#endif

#define DEVNAME(_sc)			((_sc)->sc_dev.dv_xname)

struct urtwx_tx_desc {
	uint32_t	w0;
	uint32_t	w1;
	uint32_t	w2;
	uint32_t	w3;
	uint32_t	w4;
	uint32_t	w5;
	uint32_t	w6;
	uint32_t	w7;
	uint32_t	w8;
	uint32_t	w9;
	uint32_t	w10;
	uint32_t	w11;
} __packed;

struct urtwx_fw_dl_backup {
	uint8_t		txdma_pq_map1;
	uint8_t		cr;
	uint8_t		bcn_ctrl;
	uint16_t	fifopage_info1;
	uint32_t	rqpn_ctrl2;
};

struct urtwx_rx_desc {
	uint32_t	w0;
	uint32_t	w1;
	uint32_t	w2;
	uint32_t	w3;
	uint32_t	w4;
	uint32_t	w5;
} __packed;

struct urtwx_rx_data {
	struct urtwx_softc	*sc;
	struct usbd_xfer	*xfer;
	uint8_t			*buf;
};

struct urtwx_rx_stat {
	uint16_t	pktlen;
	uint16_t	drvinfosz;
	uint8_t		shift;
	uint8_t		enc_type;
	uint8_t		phy_status;
	uint8_t		swdec;
	uint8_t		decrypted;
	uint8_t		is_c2h;
	uint8_t		rate;
	uint8_t		rssi;
	uint8_t		crc_err;
	uint8_t		icv_err;
};

struct urtwx_c2h_cmd {
	uint8_t		id;
	uint8_t		seq;
	uint8_t		payload[];
} __packed;

struct urtwx_c2h_ra_rpt {
	uint8_t		rate_sgi;
	uint8_t		macid;
	uint8_t		byte2;
	uint8_t		status;
	uint8_t		byte4;
	uint8_t		ra_ratio;
	uint8_t		bw;
} __packed;

struct urtwx_8822c_2g_1s_pwr_idx_diff {
	uint8_t		val;
} __packed;

struct urtwx_8822c_2g_ns_pwr_idx_diff {
	uint16_t	val;
} __packed;

struct urtwx_8822c_2g_txpwr_idx {
	uint8_t		cck_base[6];
	uint8_t		bw40_base[5];
	struct urtwx_8822c_2g_1s_pwr_idx_diff ht_1s_diff;
	struct urtwx_8822c_2g_ns_pwr_idx_diff ht_2s_diff;
	struct urtwx_8822c_2g_ns_pwr_idx_diff ht_3s_diff;
	struct urtwx_8822c_2g_ns_pwr_idx_diff ht_4s_diff;
} __packed;

struct urtwx_8822c_5g_ht_1s_pwr_idx_diff {
	uint8_t		val;
} __packed;

struct urtwx_8822c_5g_ht_ns_pwr_idx_diff {
	uint8_t		val;
} __packed;

struct urtwx_8822c_5g_ofdm_ns_pwr_idx_diff {
	uint16_t	val;
} __packed;

struct urtwx_8822c_5g_vht_ns_pwr_idx_diff {
	uint8_t		val;
} __packed;

struct urtwx_8822c_5g_txpwr_idx {
	uint8_t		bw40_base[14];
	struct urtwx_8822c_5g_ht_1s_pwr_idx_diff ht_1s_diff;
	struct urtwx_8822c_5g_ht_ns_pwr_idx_diff ht_2s_diff;
	struct urtwx_8822c_5g_ht_ns_pwr_idx_diff ht_3s_diff;
	struct urtwx_8822c_5g_ht_ns_pwr_idx_diff ht_4s_diff;
	struct urtwx_8822c_5g_ofdm_ns_pwr_idx_diff ofdm_diff;
	struct urtwx_8822c_5g_vht_ns_pwr_idx_diff vht_1s_diff;
	struct urtwx_8822c_5g_vht_ns_pwr_idx_diff vht_2s_diff;
	struct urtwx_8822c_5g_vht_ns_pwr_idx_diff vht_3s_diff;
	struct urtwx_8822c_5g_vht_ns_pwr_idx_diff vht_4s_diff;
} __packed;

struct urtwx_8822c_txpwr_idx {
	struct urtwx_8822c_2g_txpwr_idx pwr_idx_2g;
	struct urtwx_8822c_5g_txpwr_idx pwr_idx_5g;
} __packed;

struct urtwx_8822c_bbpg {
	uint8_t		band;
	uint8_t		rf_path;
	uint8_t		tx_num;
	uint32_t	addr;
	uint32_t	bitmask;
	uint32_t	data;
};

struct urtwx_backup_info {
	uint8_t		len;
	uint32_t	reg;
	uint32_t	val;
};

struct urtwx_8822c_rfwrite_evt {
	uint8_t		path;
	uint16_t	idx;
	uint16_t	val;
	uint16_t	idxreg;
	uint32_t	tbl_addr;
	uint32_t	tbl_data;
	uint8_t		len;
	uint8_t		data[4];
};

struct urtwx_8822c_rfblock_evt {
	uint8_t		path;
	uint16_t	blk;
	uint16_t	count;
	uint32_t	hash;
};

struct urtwx_8822c_usbtrace_evt {
	uint16_t	val;
	uint16_t	idx;
	uint8_t		len;
	uint8_t		data[4];
};

struct urtwx_softc;
struct urtwx_chip_info;

struct urtwx_host_cmd {
	void	(*cb)(struct urtwx_softc *, void *);
	uint8_t	data[URTWX_HOST_CMD_DATA_LEN];
};

struct urtwx_cmd_newstate {
	enum ieee80211_state	state;
	enum ieee80211_state	ostate;
	int			arg;
	uint32_t		seq;
};

struct urtwx_cmd_key {
	struct ieee80211_key	key;
	struct ieee80211_node	*ni;
	int			set;
};

struct urtwx_host_cmd_ring {
	struct urtwx_host_cmd	cmd[URTWX_HOST_CMD_RING_COUNT];
	int			cur;
	int			next;
	int			queued;
};

struct urtwx_softc {
	struct device		 sc_dev;
	struct ieee80211com	 sc_ic;
	struct usbd_device	*sc_udev;
	struct usbd_interface	*sc_iface;
	struct usb_task		 sc_task;
	struct timeout		 sc_scan_to;

	int			 sc_ed[URTWX_ENDPT_MAX];
	uint8_t			 sc_out_ed[URTWX_MAX_OUT_PIPES];
	int			 sc_nbulkout;
	struct usbd_pipe	*sc_ep[URTWX_ENDPT_MAX];
	struct usbd_pipe	*sc_out_pipe[URTWX_MAX_OUT_PIPES];
	struct usbd_pipe	*sc_tx_data_pipe;
	int			(*sc_newstate)(struct ieee80211com *,
			    enum ieee80211_state, int);

	struct urtwx_host_cmd_ring sc_cmdq;
	struct urtwx_rx_data	 sc_rx_data[URTWX_RX_LIST_COUNT];
	struct usbd_xfer	*sc_tx_xfer;
	uint8_t			*sc_tx_buf;
	struct ieee80211_node	*sc_tx_ni;
	const struct urtwx_chip_info *sc_chip;
	uint32_t		 sc_chip_version;
	uint8_t			 sc_crystal_cap;
	uint8_t			 sc_macaddr[ETHER_ADDR_LEN];
	uint8_t			 sc_tx_bb_swing_2g;
	uint8_t			 sc_tx_bb_swing_5g;
	uint8_t			 sc_cut_version;
	uint8_t			 sc_pkg_type;
	uint8_t			 sc_rfe_option;
	uint8_t			 sc_rf_board_option;
	uint8_t			 sc_bt_setting;
	uint8_t			 sc_usb_mode_switch;
	uint8_t			 sc_8822c_have_txpwr_idx;
	uint8_t			 sc_8822c_have_bb_pg;
	uint8_t			 sc_8822c_have_power_trim;
	uint8_t			 sc_h2c_box;
	uint16_t		 sc_h2c_pkt_seq;
	int			 sc_have_macaddr;
	int			 sc_fw_loaded;
	int			 sc_rfe_btg;
	int			 sc_tx_active;
	int			 sc_tx_timer;
	uint8_t			 sc_tx_report_sn;
	uint32_t		 sc_state_seq;
	int			 sc_scan_log;
	int			 sc_state_log;
	int			 sc_tx_mgmt_log;
	int			 sc_rfio_log;
	int			 sc_usbctl_log;
	int			 sc_tx_data_log;
	int			 sc_tx_report_req;
	int			 sc_txeof_log;
	int			 sc_txeof_data_log;
	int			 sc_c2h_log;
	int			 sc_c2h_tx_rpt_log;
	int			 sc_rx_mgmt_log;
	unsigned int		 sc_rx_done;
	unsigned int		 sc_rx_c2h;
	unsigned int		 sc_rx_desc_log;
	unsigned int		 sc_rx_err;
	int			 sc_key_tasks;
	uint32_t		 sc_rf18_want;
	uint32_t		 sc_rf_cache[2][256];
	uint8_t			 sc_rf_cache_valid[2][256];
	int			 sc_rfset_log;
	struct urtwx_8822c_txpwr_idx sc_8822c_txpwr_idx[4];
	uint8_t			 sc_8822c_power_trim[2][8];
	uint8_t			 sc_8822c_thermal_trim[2];
	uint8_t			 sc_8822c_pa_bias_2g[2];
	uint8_t			 sc_8822c_pa_bias_5g[2];
	uint16_t		 sc_8822c_dack_msbk[2][2]
				    [URTWX_8822C_DACK_MSBK_NUM];
	uint8_t			 sc_8822c_dack_dck[2][2]
				    [URTWX_8822C_DACK_DCK_NUM];
	int8_t			 sc_8822c_txpwr_ofs_2g[2]
				    [URTWX_DESC_RATEVHT2SS_MCS9 + 1];
	int8_t			 sc_8822c_txpwr_ofs_5g[2]
				    [URTWX_DESC_RATEVHT2SS_MCS9 + 1];
	int			 sc_phy_cond_log;
	int			 sc_rfchan_log;
	int			 sc_rfinit_log;
	int			 sc_rfinit_prestage_log;
	int			 sc_rfwrite_active;
	uint8_t			 sc_rfwrite_path;
	uint16_t		 sc_rfwrite_idx;
	uint32_t		 sc_rfwrite_tbl_addr;
	uint32_t		 sc_rfwrite_tbl_data;
	int			 sc_rfwrite_raw_cnt;
	struct urtwx_8822c_rfwrite_evt
			    sc_rfwrite_raw[URTWX_8822C_RFWRITE_RAW_MAX];
	int			 sc_rfblock_cnt;
	struct urtwx_8822c_rfblock_evt
			    sc_rfblock[URTWX_8822C_RFBLOCK_MAX];
	int			 sc_usbtrace_active;
	int			 sc_usbtrace_sync_pending;
	int			 sc_usbtrace_cnt;
	struct urtwx_8822c_usbtrace_evt sc_usbtrace[URTWX_8822C_USBTRACE_MAX];
};

static const char *
urtwx_desc_rate_name(uint8_t rate, char *buf, size_t len)
{
	static const char * const legacy_rates[] = {
		[0x00] = "1M",
		[0x01] = "2M",
		[0x02] = "5.5M",
		[0x03] = "11M",
		[0x04] = "6M",
		[0x05] = "9M",
		[0x06] = "12M",
		[0x07] = "18M",
		[0x08] = "24M",
		[0x09] = "36M",
		[0x0a] = "48M",
		[0x0b] = "54M",
	};
	unsigned int mcs, nss;

	if (rate < nitems(legacy_rates) && legacy_rates[rate] != NULL)
		return legacy_rates[rate];

	if (rate >= 0x0c && rate <= 0x2b) {
		mcs = rate - 0x0c;
		snprintf(buf, len, "HT-MCS%u", mcs);
		return buf;
	}

	if (rate >= 0x2c && rate <= 0x53) {
		nss = 1 + ((rate - 0x2c) / 10);
		mcs = (rate - 0x2c) % 10;
		snprintf(buf, len, "VHT%uSS-MCS%u", nss, mcs);
		return buf;
	}

	snprintf(buf, len, "rate%#x", rate);
	return buf;
}

static const char *
urtwx_ra_bw_name(uint8_t bw)
{
	switch (bw) {
	case 0:
		return "20";
	case 1:
		return "40";
	case 2:
		return "80";
	case 3:
		return "160";
	default:
		return "?";
	}
}

static void
urtwx_format_mac(const uint8_t *addr, char *buf, size_t len)
{
	snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
	    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void
urtwx_hexbuf(const uint8_t *buf, size_t len, char *out, size_t outlen)
{
	size_t i, off;

	if (outlen == 0)
		return;
	out[0] = '\0';
	off = 0;
	for (i = 0; i < len && off + 3 < outlen; i++) {
		off += snprintf(out + off, outlen - off, "%02x%s",
		    buf[i], (i + 1 == len) ? "" : " ");
		if (off >= outlen)
			break;
	}
}

static int
urtwx_desc_rate2val(uint8_t rate)
{
	switch (rate) {
	case 0x00:
		return 2;
	case 0x01:
		return 4;
	case 0x02:
		return 11;
	case 0x03:
		return 22;
	case 0x04:
		return 12;
	case 0x05:
		return 18;
	case 0x06:
		return 24;
	case 0x07:
		return 36;
	case 0x08:
		return 48;
	case 0x09:
		return 72;
	case 0x0a:
		return 96;
	case 0x0b:
		return 108;
	default:
		return -1;
	}
}

static int
urtwx_legacy_rate_index(struct ieee80211_node *ni, uint8_t desc_rate)
{
	int i, val;

	val = urtwx_desc_rate2val(desc_rate);
	if (val < 0)
		return -1;

	for (i = 0; i < ni->ni_rates.rs_nrates; i++) {
		if ((ni->ni_rates.rs_rates[i] & IEEE80211_RATE_VAL) == val)
			return i;
	}

	return -1;
}

static int
urtwx_desc_rate_is_vht(uint8_t desc_rate)
{
	return (desc_rate >= 0x2c && desc_rate <= 0x53);
}

static int
urtwx_desc_rate_is_ht(uint8_t desc_rate)
{
	return (desc_rate >= 0x0c && desc_rate <= 0x2b);
}

static int
urtwx_desc_rate_vht_nss(uint8_t desc_rate)
{
	if (!urtwx_desc_rate_is_vht(desc_rate))
		return (0);
	return (1 + ((desc_rate - 0x2c) / 10));
}

static int
urtwx_desc_rate_vht_mcs(uint8_t desc_rate)
{
	if (!urtwx_desc_rate_is_vht(desc_rate))
		return (-1);
	return ((desc_rate - 0x2c) % 10);
}

static int
urtwx_desc_rate_ht_mcs(uint8_t desc_rate)
{
	if (!urtwx_desc_rate_is_ht(desc_rate))
		return (-1);
	return (desc_rate - 0x0c);
}

static uint16_t
urtwx_vht_mcs_map(uint8_t nss, uint8_t max_mcs)
{
	uint16_t map;
	uint16_t code;
	int ss;

	switch (max_mcs) {
	case 7:
		code = IEEE80211_VHT_MCS_0_7;
		break;
	case 8:
		code = IEEE80211_VHT_MCS_0_8;
		break;
	default:
		code = IEEE80211_VHT_MCS_0_9;
		break;
	}

	map = 0;
	for (ss = 1; ss <= 8; ss++) {
		map |= (uint16_t)((ss <= nss) ? code :
		    IEEE80211_VHT_MCS_SS_NOT_SUPP)
		    << IEEE80211_VHT_MCS_FOR_SS_SHIFT(ss);
	}

	return (map);
}

static void
urtwx_build_sup_mcs(uint8_t *sup_mcs, size_t sup_mcs_len, uint8_t nss,
    uint8_t max_mcs)
{
	int ss, mcs, bit;

	memset(sup_mcs, 0, sup_mcs_len);
	for (ss = 0; ss < nss; ss++) {
		for (mcs = 0; mcs <= max_mcs; mcs++) {
			bit = ss * 8 + mcs;
			if (bit / NBBY >= sup_mcs_len)
				return;
			sup_mcs[bit / NBBY] |= 1U << (bit % NBBY);
		}
	}
}

static uint8_t
urtwx_tx_ampdu_num(const struct ieee80211_node *ni)
{
	uint8_t exp;

	exp = ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_LE;
	return ((1U << (exp + 2)) - 1);
}

static uint8_t
urtwx_tx_ampdu_density(const struct ieee80211_node *ni)
{
	return ((ni->ni_ampdu_param & IEEE80211_AMPDU_PARAM_SS) >>
	    2);
}

static void
urtwx_update_ra_node(struct urtwx_softc *sc, uint8_t desc_rate)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	int idx, mcs, nss;

	if (ic->ic_state != IEEE80211_S_RUN || ni == NULL)
		return;

	if (urtwx_desc_rate_is_vht(desc_rate)) {
		nss = urtwx_desc_rate_vht_nss(desc_rate);
		mcs = urtwx_desc_rate_vht_mcs(desc_rate);
		ni->ni_txmcs = mcs;
		ni->ni_vht_ss = nss;
		return;
	}

	if (urtwx_desc_rate_is_ht(desc_rate)) {
		ni->ni_txmcs = urtwx_desc_rate_ht_mcs(desc_rate);
		return;
	}

	idx = urtwx_legacy_rate_index(ni, desc_rate);
	if (idx >= 0)
		ni->ni_txrate = idx;
}

static const char *const urtwx_fw_names_8821c[] = {
	"rtw8821c_fw.bin",
};

static const char *const urtwx_fw_names_8822c[] = {
	"rtw8822c_fw.bin",
};

static const int8_t urtwx_lna_gain_table_0[] = {
	22, 8, -6, -22, -31, -40, -46, -52
};

static const int8_t urtwx_lna_gain_table_1[] = {
	10, 6, 2, -2, -6, -10, -14, -17,
	-20, -24, -28, -31, -34, -37, -40, -44
};

enum urtwx_pq_mapping {
	URTWX_PQ_MAPPING_EXTRA		= 0,
	URTWX_PQ_MAPPING_LOW		= 1,
	URTWX_PQ_MAPPING_NORMAL		= 2,
	URTWX_PQ_MAPPING_HIGH		= 3
};

struct urtwx_page_table {
	uint16_t	hq_num;
	uint16_t	nq_num;
	uint16_t	lq_num;
	uint16_t	exq_num;
	uint16_t	gapq_num;
};

struct urtwx_phy_table {
	const uint32_t	*ut_data;
	size_t		 ut_size;
};

enum urtwx_rf_set {
	URTWX_SWITCH_TO_BTG,
	URTWX_SWITCH_TO_WLG,
	URTWX_SWITCH_TO_WLA
};

enum urtwx_phy_cfg {
	URTWX_PHY_CFG_MAC,
	URTWX_PHY_CFG_AGC,
	URTWX_PHY_CFG_BB,
	URTWX_PHY_CFG_RF
};

struct urtwx_rqpn {
	uint8_t		dma_map_vo;
	uint8_t		dma_map_vi;
	uint8_t		dma_map_be;
	uint8_t		dma_map_bk;
	uint8_t		dma_map_mg;
	uint8_t		dma_map_hi;
};

static int	urtwx_mac_init_8821c(struct urtwx_softc *);
static int	urtwx_mac_init_8822c(struct urtwx_softc *);
static int	urtwx_8822c_coex_cfg_init(struct urtwx_softc *);
static int	urtwx_parse_efuse_8821c(struct urtwx_softc *,
		    const uint8_t *, uint8_t *);
static int	urtwx_parse_efuse_8822cu(struct urtwx_softc *,
		    const uint8_t *, uint8_t *);
static uint8_t	urtwx_get_rx_rssi_8821c(struct urtwx_softc *,
		    const uint8_t *, const struct urtwx_rx_stat *);
static uint8_t	urtwx_get_rx_rssi_8822c(struct urtwx_softc *,
		    const uint8_t *, const struct urtwx_rx_stat *);

struct urtwx_chip_info {
	const char			*uc_name;
	const char *const		*uc_fw_names;
	size_t				 uc_fw_name_count;
	uint16_t			 uc_fw_signature;
	const struct urtwx_page_table	*uc_page_table;
	const struct urtwx_rqpn		*uc_rqpn;
	struct urtwx_phy_table		 uc_mac_tbl;
	struct urtwx_phy_table		 uc_agc_tbl;
	struct urtwx_phy_table		 uc_agc_btg_type2_tbl;
	struct urtwx_phy_table		 uc_bb_tbl;
	struct urtwx_phy_table		 uc_rfk_init_tbl;
	struct urtwx_phy_table		 uc_rf_tbl[2];
	uint16_t			 uc_rf_read_base[2];
	uint16_t			 uc_rf_write_base[2];
	uint16_t			 uc_rf_sipi[2];
	uint8_t				 uc_rf_path_count;
	uint8_t				 uc_sys_func_en;
	uint8_t				 uc_tx_nss;
	uint8_t				 uc_ht_mcs_max;
	uint8_t				 uc_vht_mcs_max;
	uint8_t				 uc_rateid_ht_20m;
	uint8_t				 uc_rateid_ht_40m;
	uint8_t				 uc_rateid_vht_5g;
	uint8_t				 uc_rateid_vht_2g;
	uint8_t				 uc_desc_rate_ht_max;
	uint8_t				 uc_desc_rate_vht_max;
	uint32_t			 uc_ra_mask_ht;
	uint32_t			 uc_ra_mask_vht;
	uint32_t			 uc_ic_caps;
	uint32_t			 uc_htcaps;
	uint32_t			 uc_vhtcaps;
	uint32_t			 uc_txff_size;
	uint32_t			 uc_rxff_size;
	uint8_t				 uc_rsvd_drv_pg_num;
	uint16_t			 uc_efuse_phy_size;
	uint16_t			 uc_efuse_log_size;
	uint16_t			 uc_efuse_protect_size;
	uint16_t			 uc_vht_rx_max_lgi_mbit_s;
	uint16_t			 uc_vht_tx_max_lgi_mbit_s;
	int				(*uc_parse_efuse)(struct urtwx_softc *,
					    const uint8_t *, uint8_t *);
	uint8_t				(*uc_get_rx_rssi)(struct urtwx_softc *,
					    const uint8_t *,
					    const struct urtwx_rx_stat *);
	int				(*uc_mac_init)(struct urtwx_softc *);
};

struct urtwx_devspec {
	struct usb_devno			 ud_dev;
	const struct urtwx_chip_info		*ud_chip;
};

static const struct urtwx_page_table urtwx_page_table_8821c[] = {
	{ 16, 16, 16, 14, 1 },
	{ 16, 16, 16, 14, 1 },
	{ 16, 16,  0,  0, 1 },
	{ 16, 16, 16,  0, 1 },
	{ 16, 16, 16, 14, 1 },
};

static const struct urtwx_page_table urtwx_page_table_8822c[] = {
	{ 64, 64, 64, 64, 1 },
	{ 64, 64, 64, 64, 1 },
	{ 64, 64,  0,  0, 1 },
	{ 64, 64, 64,  0, 1 },
	{ 64, 64, 64, 64, 1 },
};

static const struct urtwx_rqpn urtwx_rqpn_8821c[] = {
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_HIGH,
		URTWX_PQ_MAPPING_HIGH, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_HIGH, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
};

static const struct urtwx_rqpn urtwx_rqpn_8822c[] = {
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_HIGH,
		URTWX_PQ_MAPPING_HIGH, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_HIGH, URTWX_PQ_MAPPING_HIGH
	},
	{
		URTWX_PQ_MAPPING_NORMAL, URTWX_PQ_MAPPING_NORMAL,
		URTWX_PQ_MAPPING_LOW, URTWX_PQ_MAPPING_LOW,
		URTWX_PQ_MAPPING_EXTRA, URTWX_PQ_MAPPING_HIGH
	},
};

static const struct urtwx_chip_info urtwx_chip_8821c = {
	.uc_name = "rtl8821c",
	.uc_fw_names = urtwx_fw_names_8821c,
	.uc_fw_name_count = nitems(urtwx_fw_names_8821c),
	.uc_fw_signature = 0x8821,
	.uc_page_table = urtwx_page_table_8821c,
	.uc_rqpn = urtwx_rqpn_8821c,
	.uc_mac_tbl = { urtwx_mac, nitems(urtwx_mac) },
	.uc_agc_tbl = { urtwx_agc, nitems(urtwx_agc) },
	.uc_agc_btg_type2_tbl = { urtwx_agc_btg_type2,
	    nitems(urtwx_agc_btg_type2) },
	.uc_bb_tbl = { urtwx_bb, nitems(urtwx_bb) },
	.uc_rfk_init_tbl = { NULL, 0 },
	.uc_rf_tbl = {
		{ urtwx_rf_a, nitems(urtwx_rf_a) },
		{ NULL, 0 },
	},
	.uc_rf_read_base = { URTWX_RF_BASE_A, URTWX_RF_BASE_B },
	.uc_rf_write_base = { URTWX_RF_BASE_A, URTWX_RF_BASE_B },
	.uc_rf_sipi = { URTWX_RF_SIPI_A, URTWX_RF_SIPI_B },
	.uc_rf_path_count = 1,
	.uc_sys_func_en = URTWX_SYS_FUNC_EN_8821C,
	.uc_tx_nss = 1,
	.uc_ht_mcs_max = 7,
	.uc_vht_mcs_max = 9,
	.uc_rateid_ht_20m = URTWX_RATEID_BGN_20M_1SS,
	.uc_rateid_ht_40m = URTWX_RATEID_BGN_40M_1SS,
	.uc_rateid_vht_5g = URTWX_RATEID_ARFR1_AC_1SS,
	.uc_rateid_vht_2g = URTWX_RATEID_ARFR2_AC_2G_1SS,
	.uc_desc_rate_ht_max = URTWX_DESC_RATEMCS7,
	.uc_desc_rate_vht_max = URTWX_DESC_RATEVHT1SS_MCS9,
	.uc_ra_mask_ht = URTWX_RA_MASK_HT_RATES_1SS,
	.uc_ra_mask_vht = URTWX_RA_MASK_VHT_RATES_1SS,
	.uc_ic_caps = IEEE80211_C_QOS | IEEE80211_C_TX_AMPDU |
	    IEEE80211_C_MFP | IEEE80211_C_SHPREAMBLE |
	    IEEE80211_C_SHSLOT | IEEE80211_C_WEP | IEEE80211_C_RSN,
	.uc_htcaps = URTWX_HTCAP_BASE,
	.uc_vhtcaps = IEEE80211_VHTCAP_MAX_MPDU_LENGTH_3895 |
	    (IEEE80211_VHTCAP_MAX_AMPDU_LEN_64K <<
	    IEEE80211_VHTCAP_MAX_AMPDU_LEN_SHIFT) |
	    (IEEE80211_VHTCAP_CHAN_WIDTH_80 <<
	    IEEE80211_VHTCAP_CHAN_WIDTH_SHIFT) |
	    IEEE80211_VHTCAP_SGI80 |
	    IEEE80211_VHTCAP_RX_ANT_PATTERN |
	    IEEE80211_VHTCAP_TX_ANT_PATTERN,
	.uc_txff_size = 65536,
	.uc_rxff_size = 16384,
	.uc_rsvd_drv_pg_num = 8,
	.uc_efuse_phy_size = 512,
	.uc_efuse_log_size = 512,
	.uc_efuse_protect_size = 96,
	.uc_vht_rx_max_lgi_mbit_s = 390,
	.uc_vht_tx_max_lgi_mbit_s = 390,
	.uc_parse_efuse = urtwx_parse_efuse_8821c,
	.uc_get_rx_rssi = urtwx_get_rx_rssi_8821c,
	.uc_mac_init = urtwx_mac_init_8821c,
};

static const struct urtwx_chip_info urtwx_chip_8822c = {
	.uc_name = "rtl8822c",
	.uc_fw_names = urtwx_fw_names_8822c,
	.uc_fw_name_count = nitems(urtwx_fw_names_8822c),
	.uc_fw_signature = 0x8822,
	.uc_page_table = urtwx_page_table_8822c,
	.uc_rqpn = urtwx_rqpn_8822c,
	.uc_mac_tbl = { urtwx_8822c_mac, nitems(urtwx_8822c_mac) },
	.uc_agc_tbl = { urtwx_8822c_agc, nitems(urtwx_8822c_agc) },
	.uc_agc_btg_type2_tbl = { NULL, 0 },
	.uc_bb_tbl = { urtwx_8822c_bb, nitems(urtwx_8822c_bb) },
	.uc_rfk_init_tbl = {
		urtwx_8822c_rfk_init, nitems(urtwx_8822c_rfk_init)
	},
	.uc_rf_tbl = {
		{ urtwx_8822c_rf_a, nitems(urtwx_8822c_rf_a) },
		{ urtwx_8822c_rf_b, nitems(urtwx_8822c_rf_b) },
	},
	.uc_rf_read_base = { 0x3c00, 0x4c00 },
	.uc_rf_write_base = { 0x3c00, 0x4c00 },
	.uc_rf_sipi = { 0x1808, 0x4108 },
	.uc_rf_path_count = 2,
	.uc_sys_func_en = URTWX_SYS_FUNC_EN_8821C,
	.uc_tx_nss = 2,
	.uc_ht_mcs_max = 7,
	.uc_vht_mcs_max = 9,
	.uc_rateid_ht_20m = URTWX_RATEID_BGN_20M_2SS,
	.uc_rateid_ht_40m = URTWX_RATEID_BGN_40M_2SS,
	.uc_rateid_vht_5g = URTWX_RATEID_ARFR0_AC_2SS,
	.uc_rateid_vht_2g = URTWX_RATEID_ARFR3_AC_2G_2SS,
	.uc_desc_rate_ht_max = URTWX_DESC_RATEMCS15,
	.uc_desc_rate_vht_max = URTWX_DESC_RATEVHT2SS_MCS9,
	.uc_ra_mask_ht = URTWX_RA_MASK_HT_RATES_1SS |
	    URTWX_RA_MASK_HT_RATES_2SS,
	.uc_ra_mask_vht = URTWX_RA_MASK_VHT_RATES_1SS |
	    URTWX_RA_MASK_VHT_RATES_2SS,
	.uc_ic_caps = IEEE80211_C_QOS | IEEE80211_C_TX_AMPDU |
	    IEEE80211_C_MFP | IEEE80211_C_SHPREAMBLE |
	    IEEE80211_C_SHSLOT | IEEE80211_C_WEP | IEEE80211_C_RSN,
	.uc_htcaps = URTWX_HTCAP_BASE,
	.uc_vhtcaps = IEEE80211_VHTCAP_MAX_MPDU_LENGTH_3895 |
	    (IEEE80211_VHTCAP_MAX_AMPDU_LEN_64K <<
	    IEEE80211_VHTCAP_MAX_AMPDU_LEN_SHIFT) |
	    (IEEE80211_VHTCAP_CHAN_WIDTH_80 <<
	    IEEE80211_VHTCAP_CHAN_WIDTH_SHIFT) |
	    IEEE80211_VHTCAP_SGI80 |
	    IEEE80211_VHTCAP_RX_ANT_PATTERN |
	    IEEE80211_VHTCAP_TX_ANT_PATTERN,
	.uc_txff_size = 262144,
	.uc_rxff_size = 24576,
	.uc_rsvd_drv_pg_num = 16,
	.uc_efuse_phy_size = 512,
	.uc_efuse_log_size = 768,
	.uc_efuse_protect_size = 124,
	.uc_vht_rx_max_lgi_mbit_s = 780,
	.uc_vht_tx_max_lgi_mbit_s = 780,
	.uc_parse_efuse = urtwx_parse_efuse_8822cu,
	.uc_get_rx_rssi = urtwx_get_rx_rssi_8822c,
	.uc_mac_init = urtwx_mac_init_8822c,
};

static const struct urtwx_8822c_bbpg urtwx_8822c_bb_pg_type0[] = {
	{ 0, 0, 0, 0x00000c20, 0xffffffff, 0x484c5054 },
	{ 0, 0, 0, 0x00000c24, 0xffffffff, 0x54585858 },
	{ 0, 0, 0, 0x00000c28, 0xffffffff, 0x44484c50 },
	{ 0, 0, 0, 0x00000c2c, 0xffffffff, 0x50545858 },
	{ 0, 0, 0, 0x00000c30, 0xffffffff, 0x4044484c },
	{ 0, 0, 1, 0x00000c34, 0xffffffff, 0x50545858 },
	{ 0, 0, 1, 0x00000c38, 0xffffffff, 0x4044484c },
	{ 0, 0, 0, 0x00000c3c, 0xffffffff, 0x50545858 },
	{ 0, 0, 0, 0x00000c40, 0xffffffff, 0x4044484c },
	{ 0, 0, 0, 0x00000c44, 0xffffffff, 0x5858383c },
	{ 0, 0, 1, 0x00000c48, 0xffffffff, 0x484c5054 },
	{ 0, 0, 1, 0x00000c4c, 0xffffffff, 0x383c4044 },
	{ 0, 1, 0, 0x00000e20, 0xffffffff, 0x484c5054 },
	{ 0, 1, 0, 0x00000e24, 0xffffffff, 0x54585858 },
	{ 0, 1, 0, 0x00000e28, 0xffffffff, 0x44484c50 },
	{ 0, 1, 0, 0x00000e2c, 0xffffffff, 0x50545858 },
	{ 0, 1, 0, 0x00000e30, 0xffffffff, 0x4044484c },
	{ 0, 1, 1, 0x00000e34, 0xffffffff, 0x50545858 },
	{ 0, 1, 1, 0x00000e38, 0xffffffff, 0x4044484c },
	{ 0, 1, 0, 0x00000e3c, 0xffffffff, 0x50545858 },
	{ 0, 1, 0, 0x00000e40, 0xffffffff, 0x4044484c },
	{ 0, 1, 0, 0x00000e44, 0xffffffff, 0x5858383c },
	{ 0, 1, 1, 0x00000e48, 0xffffffff, 0x484c5054 },
	{ 0, 1, 1, 0x00000e4c, 0xffffffff, 0x383c4044 },
	{ 1, 0, 0, 0x00000c24, 0xffffffff, 0x54585858 },
	{ 1, 0, 0, 0x00000c28, 0xffffffff, 0x44484c50 },
	{ 1, 0, 0, 0x00000c2c, 0xffffffff, 0x50545858 },
	{ 1, 0, 0, 0x00000c30, 0xffffffff, 0x4044484c },
	{ 1, 0, 1, 0x00000c34, 0xffffffff, 0x50545858 },
	{ 1, 0, 1, 0x00000c38, 0xffffffff, 0x4044484c },
	{ 1, 0, 0, 0x00000c3c, 0xffffffff, 0x50545858 },
	{ 1, 0, 0, 0x00000c40, 0xffffffff, 0x4044484c },
	{ 1, 0, 0, 0x00000c44, 0xffffffff, 0x5858383c },
	{ 1, 0, 1, 0x00000c48, 0xffffffff, 0x484c5054 },
	{ 1, 0, 1, 0x00000c4c, 0xffffffff, 0x383c4044 },
	{ 1, 1, 0, 0x00000e24, 0xffffffff, 0x54585858 },
	{ 1, 1, 0, 0x00000e28, 0xffffffff, 0x44484c50 },
	{ 1, 1, 0, 0x00000e2c, 0xffffffff, 0x50545858 },
	{ 1, 1, 0, 0x00000e30, 0xffffffff, 0x4044484c },
	{ 1, 1, 1, 0x00000e34, 0xffffffff, 0x50545858 },
	{ 1, 1, 1, 0x00000e38, 0xffffffff, 0x4044484c },
	{ 1, 1, 0, 0x00000e3c, 0xffffffff, 0x50545858 },
	{ 1, 1, 0, 0x00000e40, 0xffffffff, 0x4044484c },
	{ 1, 1, 0, 0x00000e44, 0xffffffff, 0x5858383c },
	{ 1, 1, 1, 0x00000e48, 0xffffffff, 0x484c5054 },
	{ 1, 1, 1, 0x00000e4c, 0xffffffff, 0x383c4044 },
};

static inline uint32_t
urtwx_txff_size(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_txff_size);
}

static inline uint32_t
urtwx_txff_pg_num(const struct urtwx_softc *sc)
{
	return (urtwx_txff_size(sc) / URTWX_PAGE_SIZE);
}

static inline uint32_t
urtwx_rxff_size(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_rxff_size);
}

static inline uint8_t
urtwx_rsvd_drv_pg_num(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_rsvd_drv_pg_num);
}

static inline uint32_t
urtwx_rsvd_pg_num(const struct urtwx_softc *sc)
{
	return (urtwx_rsvd_drv_pg_num(sc) + URTWX_RSVD_H2C_EXTRA_PG_NUM +
	    URTWX_RSVD_H2C_STATIC_PG_NUM + URTWX_RSVD_H2CQ_PG_NUM +
	    URTWX_RSVD_CPU_INSTR_PG_NUM + URTWX_RSVD_FW_TXBUF_PG_NUM);
}

static inline uint16_t
urtwx_rsvd_boundary(const struct urtwx_softc *sc)
{
	return (urtwx_txff_pg_num(sc) - urtwx_rsvd_pg_num(sc));
}

static inline uint16_t
urtwx_rsvd_drv_addr(const struct urtwx_softc *sc)
{
	return (urtwx_rsvd_boundary(sc) - urtwx_rsvd_drv_pg_num(sc));
}

static inline uint16_t
urtwx_h2cq_pg_addr(const struct urtwx_softc *sc)
{
	return (urtwx_txff_pg_num(sc) - URTWX_RSVD_FW_TXBUF_PG_NUM -
	    URTWX_RSVD_CPU_INSTR_PG_NUM - URTWX_RSVD_H2CQ_PG_NUM);
}

static inline uint32_t
urtwx_h2cq_addr(const struct urtwx_softc *sc)
{
	return (urtwx_h2cq_pg_addr(sc) * URTWX_PAGE_SIZE);
}

static inline uint32_t
urtwx_h2cq_size(const struct urtwx_softc *sc)
{
	return (URTWX_RSVD_H2CQ_PG_NUM * URTWX_PAGE_SIZE);
}

static inline uint16_t
urtwx_efuse_phy_size(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_efuse_phy_size);
}

static inline uint16_t
urtwx_efuse_log_size(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_efuse_log_size);
}

static inline uint16_t
urtwx_efuse_protect_size(const struct urtwx_softc *sc)
{
	return (sc->sc_chip->uc_efuse_protect_size);
}

static const struct urtwx_devspec urtwx_devs[] = {
	{
		.ud_dev = { USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_1 },
		.ud_chip = &urtwx_chip_8821c,
	},
	{
		.ud_dev = { USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_2 },
		.ud_chip = &urtwx_chip_8821c,
	},
	{
		.ud_dev = { USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8822CU },
		.ud_chip = &urtwx_chip_8822c,
	},
};

static const struct urtwx_devspec *
urtwx_lookup_devspec(uint16_t vendor, uint16_t product)
{
	size_t i;

	for (i = 0; i < nitems(urtwx_devs); i++) {
		if (urtwx_devs[i].ud_dev.ud_vendor == vendor &&
		    urtwx_devs[i].ud_dev.ud_product == product)
			return (&urtwx_devs[i]);
	}

	return (NULL);
}

int	urtwx_match(struct device *, void *, void *);
void	urtwx_attach(struct device *, struct device *, void *);
int	urtwx_detach(struct device *, int);

static int	urtwx_alloc_rx_list(struct urtwx_softc *);
static int	urtwx_bulkout_num(struct urtwx_softc *);
static int	urtwx_bulk_tx(struct urtwx_softc *, uint8_t, int);
static int	urtwx_check_fw_checksum(struct urtwx_softc *, uint32_t);
static int	urtwx_check_firmware_size(const uint8_t *, size_t);
static void	urtwx_close_pipes(struct urtwx_softc *);
static int	urtwx_ctl(struct urtwx_softc *, uint8_t, uint8_t,
		    uint16_t, uint16_t, void *, int);
static int	urtwx_dma_mapping_to_out(uint8_t);
static int	urtwx_ddma_enable(struct urtwx_softc *, uint32_t,
		    uint32_t, uint32_t);
static int	urtwx_download_firmware_end_flow(struct urtwx_softc *);
static int	urtwx_download_firmware_to_mem(struct urtwx_softc *,
		    const uint8_t *, uint32_t, uint32_t);
static void	urtwx_download_firmware_reg_backup(struct urtwx_softc *,
		    struct urtwx_fw_dl_backup *);
static void	urtwx_download_firmware_reg_restore(struct urtwx_softc *,
		    const struct urtwx_fw_dl_backup *);
static void	urtwx_download_firmware_reset_platform(struct urtwx_softc *);
static int	urtwx_download_firmware_validate(struct urtwx_softc *);
static int	urtwx_dump_log_efuse_map(struct urtwx_softc *,
		    const uint8_t *, uint8_t *);
static int	urtwx_dump_phy_efuse_map(struct urtwx_softc *, uint8_t *);
static int	urtwx_fw_default_port(struct urtwx_softc *);
static int	urtwx_fw_download(struct urtwx_softc *);
static int	urtwx_fw_download_sta_rsvd_page(struct urtwx_softc *);
static int	urtwx_fw_media_status(struct urtwx_softc *, int);
static int	urtwx_fw_ra_info(struct urtwx_softc *,
		    struct ieee80211_node *);
static int	urtwx_fw_rsvd_page(struct urtwx_softc *, uint8_t, uint8_t,
		    uint8_t);
static int	urtwx_fw_send_h2c(struct urtwx_softc *, uint32_t, uint32_t);
static int	urtwx_fw_send_general_info(struct urtwx_softc *);
static int	urtwx_fw_send_h2c_pkt(struct urtwx_softc *, const uint8_t *,
		    size_t);
static int	urtwx_fw_send_phydm_info(struct urtwx_softc *);
static void	urtwx_free_rx_list(struct urtwx_softc *);
static int	urtwx_iddma_download_firmware(struct urtwx_softc *,
		    uint32_t, uint32_t, uint32_t, int);
static int	urtwx_init_h2c(struct urtwx_softc *);
static int	urtwx_init(struct ifnet *);
static int	urtwx_is_powered_on(struct urtwx_softc *);
static int	urtwx_ioctl(struct ifnet *, u_long, caddr_t);
static int	urtwx_8822c_ltecoex_read(struct urtwx_softc *, uint16_t,
		    uint32_t *);
static int	urtwx_8822c_ltecoex_write(struct urtwx_softc *, uint16_t,
		    uint32_t);
static int	urtwx_8822c_ltecoex_write_mask(struct urtwx_softc *,
		    uint16_t, uint32_t, uint32_t);
static int	urtwx_8822c_wifi_only_coex(struct urtwx_softc *);
static int	urtwx_8822c_rf_init(struct urtwx_softc *);
static int	urtwx_8822c_phy_init(struct urtwx_softc *);
static int	urtwx_mac_init_runtime(struct urtwx_softc *);
static int	urtwx_mac_init_system_cfg(struct urtwx_softc *);
static int	urtwx_mask2shift(uint32_t);
static int	urtwx_media_change(struct ifnet *);
static int	urtwx_newstate(struct ieee80211com *, enum ieee80211_state,
		    int);
static int	urtwx_ampdu_rx_start(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static void	urtwx_ampdu_rx_stop(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static void	urtwx_newstate_cb(struct urtwx_softc *, void *);
static int	urtwx_set_key(struct ieee80211com *, struct ieee80211_node *,
		    struct ieee80211_key *);
static void	urtwx_delete_key(struct ieee80211com *,
		    struct ieee80211_node *, struct ieee80211_key *);
static void	urtwx_set_key_cb(struct urtwx_softc *, void *);
static void	urtwx_delete_key_cb(struct urtwx_softc *, void *);
static void	urtwx_node_tx_info(struct ieee80211_node *, uint8_t *,
		    uint8_t *, uint8_t *, uint8_t *, uint32_t *);
static void	urtwx_parse_c2h(struct urtwx_softc *, const uint8_t *, int);
static void	urtwx_parse_rx_desc(const struct urtwx_rx_desc *,
		    struct urtwx_rx_stat *);
static int	urtwx_phy_cfg(struct urtwx_softc *, enum urtwx_phy_cfg,
		    uint8_t, uint32_t, uint32_t);
static void	urtwx_update_htcaps(struct urtwx_softc *,
		    struct ieee80211_channel *);
static void	urtwx_get_chan_params(struct urtwx_softc *,
		    struct ieee80211_channel *, uint8_t *, uint8_t *,
		    uint8_t *);
static int	urtwx_phy_init_runtime(struct urtwx_softc *);
static int	urtwx_phy_load_table(struct urtwx_softc *,
		    const uint32_t *, size_t, enum urtwx_phy_cfg, uint8_t);
static int	urtwx_phy_load_tables(struct urtwx_softc *);
static void	urtwx_8822c_tbltime_enter(struct urtwx_softc *,
		    const char *, struct timeval *);
static void	urtwx_8822c_tbltime_exit(struct urtwx_softc *,
		    const char *, const struct timeval *);
static int	urtwx_8822c_rf0_retry(struct urtwx_softc *, uint8_t, uint32_t);
static int	urtwx_phy_match_cond(struct urtwx_softc *, uint32_t,
		    uint32_t);
#if URTWX_8822C_ABORT_AFTER_TABLES
static void	urtwx_8822c_rfwrite_reset(struct urtwx_softc *);
static void	urtwx_8822c_rfwrite_start(struct urtwx_softc *);
static void	urtwx_8822c_rfwrite_stop(struct urtwx_softc *);
static void	urtwx_8822c_rfwrite_dump(struct urtwx_softc *);
static void	urtwx_8822c_usbtrace_reset(struct urtwx_softc *);
static void	urtwx_8822c_usbtrace_start(struct urtwx_softc *);
static void	urtwx_8822c_usbtrace_stop(struct urtwx_softc *);
static void	urtwx_8822c_usbtrace_dump(struct urtwx_softc *);
#endif
static void	urtwx_8822c_rfwrite_set_entry(struct urtwx_softc *, uint8_t,
		    uint16_t, uint32_t, uint32_t);
static void	urtwx_8822c_rfwrite_clear_entry(struct urtwx_softc *);
static void	urtwx_8822c_rfwrite_log(struct urtwx_softc *, uint16_t,
		    uint16_t, const void *, int);
static void	urtwx_8822c_usbtrace_log(struct urtwx_softc *, uint16_t,
		    uint16_t, const void *, int);
static void	urtwx_8822c_dump_phy_stage(struct urtwx_softc *, const char *);
static int	urtwx_8822c_set_tx_power_default(struct urtwx_softc *,
		    uint8_t, uint8_t);
static void	urtwx_8822c_parse_bb_pg(struct urtwx_softc *);
static void	urtwx_8822c_board_info_setup(struct urtwx_softc *);
static void	urtwx_8822c_dump_chan_regs(struct urtwx_softc *, uint8_t);
static void	urtwx_8822c_header_file_init(struct urtwx_softc *, int);
static void	urtwx_8822c_log_usb_mode(struct urtwx_softc *);
static void	urtwx_8822c_dac_cal_backup(struct urtwx_softc *);
static int	urtwx_8822c_dac_cal_restore(struct urtwx_softc *);
static int	urtwx_8822c_trace_ctl_reg(uint16_t);
static int	urtwx_8822c_thermal_trim(struct urtwx_softc *);
static int	urtwx_8822c_power_trim(struct urtwx_softc *);
static int	urtwx_8822c_pa_bias(struct urtwx_softc *);
static uint32_t	urtwx_read_rf_path(struct urtwx_softc *, uint8_t, uint8_t);
static uint32_t	urtwx_read_rf(struct urtwx_softc *, uint8_t);
static int	urtwx_power_on(struct urtwx_softc *);
static int	urtwx_priority_queue_cfg(struct urtwx_softc *);
static int	urtwx_sta_enter_run(struct urtwx_softc *);
static void	urtwx_sta_leave_run(struct urtwx_softc *);
static struct usbd_pipe *urtwx_qsel_to_pipe(struct urtwx_softc *, uint8_t);
static uint8_t	urtwx_tx_data_qsel(struct ieee80211_frame *);
static void	urtwx_8822c_dump_hw_feature(struct urtwx_softc *);
static int	urtwx_read_efuse(struct urtwx_softc *, uint8_t *);
static uint8_t	urtwx_read_1(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static uint16_t	urtwx_read_2(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static uint32_t	urtwx_read_4_mask(struct urtwx_softc *, uint16_t,
		    uint32_t);
static uint32_t	urtwx_read_4(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static int	urtwx_read_mem(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, void *, int);
static int	urtwx_reg_sec(struct urtwx_softc *, uint16_t, void *);
static int	urtwx_set_macaddr(struct urtwx_softc *, const uint8_t *);
static void	urtwx_rx_frame(struct urtwx_softc *, const uint8_t *, int,
		    const struct urtwx_rx_stat *, struct mbuf_list *);
static int	urtwx_rx_init(struct urtwx_softc *);
static void	urtwx_rxeof(struct usbd_xfer *, void *, usbd_status);
static void	urtwx_scan_to(void *);
static int	urtwx_set_chan(struct urtwx_softc *,
		    struct ieee80211_channel *);
static void	urtwx_start(struct ifnet *);
static void	urtwx_stop(struct ifnet *);
static void	urtwx_task(void *);
static int	urtwx_tx(struct urtwx_softc *, struct mbuf *,
		    struct ieee80211_node *, uint8_t, int);
static void	urtwx_txeof(struct usbd_xfer *, void *, usbd_status);
static int	urtwx_rx_set_dma(struct urtwx_softc *);
static int	urtwx_start_download_firmware(struct urtwx_softc *,
		    const uint8_t *, size_t);
static int	urtwx_txdma_queue_mapping(struct urtwx_softc *);
static void	urtwx_txdesc_checksum(struct urtwx_softc *,
		    struct urtwx_tx_desc *);
static int	urtwx_usb_interface_cfg(struct urtwx_softc *);
static int	urtwx_usb_rx_agg_cfg(struct urtwx_softc *);
static int	urtwx_sec_enable(struct urtwx_softc *);
static int	urtwx_sec_cam_write(struct urtwx_softc *, uint8_t, uint32_t);
static int	urtwx_hw_set_key(struct ieee80211com *, struct ieee80211_node *,
		    struct ieee80211_key *);
static void	urtwx_hw_delete_key(struct ieee80211com *,
		    struct ieee80211_node *, struct ieee80211_key *);
static void	urtwx_wait_async(struct urtwx_softc *);
static void	urtwx_watchdog(struct ifnet *);
static int	urtwx_valid_etheraddr(const uint8_t *);
static int	urtwx_wait_1(struct urtwx_softc *, uint16_t, uint8_t,
		    uint8_t, int, int);
static int	urtwx_wait_2(struct urtwx_softc *, uint16_t, uint16_t,
		    uint16_t, int, int);
static int	urtwx_wait_4(struct urtwx_softc *, uint16_t, uint32_t,
		    uint32_t, int, int);
static void	urtwx_wlan_cpu_enable(struct urtwx_softc *, int);
static uint32_t	urtwx_build_null_frame(struct urtwx_softc *, uint8_t *, int);
static uint32_t	urtwx_build_pspoll_frame(struct urtwx_softc *, uint8_t *);
static void	urtwx_fill_rsvd_page_desc(struct urtwx_softc *,
		    struct urtwx_tx_desc *, uint32_t, int, int);
static int	urtwx_write_rf_path(struct urtwx_softc *, uint8_t, uint8_t,
		    uint32_t, uint32_t);
static int	urtwx_write_1(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_1_mask(struct urtwx_softc *, uint16_t,
		    uint8_t, uint8_t);
static int	urtwx_write_2(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_2_mask(struct urtwx_softc *, uint16_t,
		    uint16_t, uint16_t);
static int	urtwx_write_4(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_4_field(struct urtwx_softc *, uint16_t,
		    uint32_t, uint32_t);
static int	urtwx_write_4_mask(struct urtwx_softc *, uint16_t,
		    uint32_t, uint32_t);
static int	urtwx_write_data_h2c(struct urtwx_softc *, const uint8_t *,
		    uint32_t);
static int	urtwx_write_data_rsvd_page(struct urtwx_softc *, uint16_t,
		    const uint8_t *, uint32_t);
static int	urtwx_write_mem(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, void *, int);
static int	urtwx_8822c_rstb_3wire(struct urtwx_softc *, int);
static int	urtwx_8822c_trace_reg(uint16_t);
static void	urtwx_do_async(struct urtwx_softc *,
		    void (*)(struct urtwx_softc *, void *), void *, int);

struct cfdriver urtwx_cd = {
	NULL, "urtwx", DV_IFNET
};

const struct cfattach urtwx_ca = {
	sizeof(struct urtwx_softc), urtwx_match, urtwx_attach, urtwx_detach
};

static int
urtwx_8822c_trace_reg(uint16_t reg)
{
	switch (reg) {
	case 0x1808:
	case 0x4108:
	case 0x3c00:
	case 0x4c00:
	case 0x1c90:
	case 0x1830:
	case 0x4130:
	case 0x180c:
	case 0x1810:
	case 0x410c:
	case 0x4110:
	case 0x1c3c:
	case 0x1c24:
	case 0x1868:
	case 0x4168:
		return (1);
	default:
		return (0);
	}
}

static int
urtwx_8822c_trace_ctl_reg(uint16_t reg)
{
	switch (reg) {
	case REG_SYS_FUNC_EN:
	case REG_RSV_CTRL:
	case REG_RF_CTRL:
	case REG_WLRF1:
	case REG_ANAPAR_XTAL_0:
	case 0x1808:
	case 0x4108:
		return (1);
	default:
		return (0);
	}
}

static int
urtwx_8822c_trace_ctl_write_reg(uint16_t reg)
{
	switch (reg) {
	case REG_SYS_FUNC_EN:
	case REG_RSV_CTRL:
	case REG_RF_CTRL:
	case REG_WLRF1:
	case REG_ANAPAR_XTAL_0:
	case 0x1808:
	case 0x4108:
		return (1);
	default:
		return (0);
	}
}

static int
urtwx_ctl(struct urtwx_softc *sc, uint8_t rw, uint8_t cmd, uint16_t val,
    uint16_t index, void *buf, int len)
{
	usb_device_request_t	req;
	usbd_status		err;
	uint8_t			bytes[4];

	if (usbd_is_dying(sc->sc_udev))
		return (EIO);

	req.bmRequestType = (rw == URTWX_CTL_WRITE) ?
	    UT_WRITE_VENDOR_DEVICE : UT_READ_VENDOR_DEVICE;
	req.bRequest = cmd;
	USETW(req.wValue, val);
	USETW(req.wIndex, index);
	USETW(req.wLength, len);

	err = usbd_do_request(sc->sc_udev, &req, buf);
	if (err) {
		DPRINTF(("%s: reg %#x failed: %s\n", DEVNAME(sc), val,
		    usbd_errstr(err)));
		return (EIO);
	}

	if (sc->sc_chip == &urtwx_chip_8822c && rw == URTWX_CTL_WRITE &&
	    cmd == RTW_USB_CMD_REQ) {
		urtwx_8822c_rfwrite_log(sc, val, index, buf, len);
		urtwx_8822c_usbtrace_log(sc, val, index, buf, len);
	}

	if (sc->sc_chip == &urtwx_chip_8822c && cmd == RTW_USB_CMD_REQ &&
	    ((rw == URTWX_CTL_READ && urtwx_8822c_trace_ctl_reg(val)) ||
	    (rw == URTWX_CTL_WRITE && urtwx_8822c_trace_ctl_write_reg(val))) &&
	    sc->sc_usbctl_log < URTWX_8822C_USBCTL_LOG_MAX &&
	    len > 0 && len <= (int)sizeof(bytes)) {
		memset(bytes, 0, sizeof(bytes));
		memcpy(bytes, buf, len);
		printf("%s: 8822c usb ctl %c req=%#x val=%#x idx=%#x len=%d "
		    "data=%02x %02x %02x %02x\n",
		    DEVNAME(sc), (rw == URTWX_CTL_WRITE) ? 'W' : 'R', cmd,
		    val, index, len, bytes[0], bytes[1], bytes[2], bytes[3]);
		sc->sc_usbctl_log++;
	}

	return (0);
}

static int
urtwx_reg_sec(struct urtwx_softc *sc, uint16_t addr, void *buf)
{
	if (!(addr <= 0x00ff || (addr >= 0x1000 && addr <= 0x10ff)))
		return (0);

	return (urtwx_ctl(sc, URTWX_CTL_WRITE, RTW_USB_CMD_REQ, 0x04e0, 0,
	    buf, 1));
}

static int
urtwx_read_mem(struct urtwx_softc *sc, uint8_t cmd, uint16_t addr,
    uint16_t index, void *buf, int len)
{
	int ret;

	ret = urtwx_ctl(sc, URTWX_CTL_READ, cmd, addr, index, buf, len);
	if (ret != 0)
		return (ret);

	return (urtwx_reg_sec(sc, addr, buf));
}

static int
urtwx_write_mem(struct urtwx_softc *sc, uint8_t cmd, uint16_t addr,
    uint16_t index, void *buf, int len)
{
	int ret;

	ret = urtwx_ctl(sc, URTWX_CTL_WRITE, cmd, addr, index, buf, len);
	if (ret != 0)
		return (ret);

	return (urtwx_reg_sec(sc, addr, buf));
}

static uint8_t
urtwx_read_1(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint8_t	val;

	val = 0xff;
	(void)urtwx_read_mem(sc, cmd, reg, index, &val, sizeof(val));
	return (val);
}

static uint16_t
urtwx_read_2(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint8_t	val[2];

	val[0] = 0xff;
	val[1] = 0xff;
	(void)urtwx_read_mem(sc, cmd, reg, index, val, sizeof(val));
	return (UGETW(val));
}

static uint32_t
urtwx_read_4(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index)
{
	uint8_t	val[4];
	uint32_t out;

	val[0] = 0xff;
	val[1] = 0xff;
	val[2] = 0xff;
	val[3] = 0xff;
	(void)urtwx_read_mem(sc, cmd, reg, index, val, sizeof(val));
	out = UGETDW(val);
	if (sc->sc_chip == &urtwx_chip_8822c && cmd == RTW_USB_CMD_REQ &&
	    urtwx_8822c_trace_reg(reg) &&
	    sc->sc_rfio_log < URTWX_8822C_RFIO_LOG_MAX) {
		printf("%s: 8822c usb rd reg %#x = %#x\n",
		    DEVNAME(sc), reg, out);
		sc->sc_rfio_log++;
	}
	return (out);
}

static int
urtwx_write_1(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint8_t	buf;

	buf = val & 0xff;
	return (urtwx_write_mem(sc, cmd, reg, index, &buf, sizeof(buf)));
}

static int
urtwx_write_2(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint8_t	buf[2];

	USETW(buf, val);
	return (urtwx_write_mem(sc, cmd, reg, index, buf, sizeof(buf)));
}

static int
urtwx_write_4(struct urtwx_softc *sc, uint8_t cmd, uint16_t reg, uint16_t index,
    uint32_t val)
{
	uint8_t	buf[4];

	USETDW(buf, val);
	if (sc->sc_chip == &urtwx_chip_8822c && cmd == RTW_USB_CMD_REQ &&
	    urtwx_8822c_trace_reg(reg) &&
	    sc->sc_rfio_log < URTWX_8822C_RFIO_LOG_MAX) {
		printf("%s: 8822c usb wr reg %#x = %#x\n",
		    DEVNAME(sc), reg, val);
		sc->sc_rfio_log++;
	}
	return (urtwx_write_mem(sc, cmd, reg, index, buf, sizeof(buf)));
}

static int
urtwx_write_1_mask(struct urtwx_softc *sc, uint16_t reg, uint8_t mask,
    uint8_t val)
{
	uint8_t regval;

	regval = urtwx_read_1(sc, RTW_USB_CMD_REQ, reg, 0);
	regval &= ~mask;
	regval |= val & mask;

	return (urtwx_write_1(sc, RTW_USB_CMD_REQ, reg, 0, regval));
}

static int
urtwx_write_2_mask(struct urtwx_softc *sc, uint16_t reg, uint16_t mask,
    uint16_t val)
{
	uint16_t regval;

	regval = urtwx_read_2(sc, RTW_USB_CMD_REQ, reg, 0);
	regval = (regval & ~mask) | (val & mask);

	return (urtwx_write_2(sc, RTW_USB_CMD_REQ, reg, 0, regval));
}

static int
urtwx_write_4_mask(struct urtwx_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t val)
{
	uint32_t regval;

	regval = urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0);
	regval &= ~mask;
	regval |= val & mask;

	return (urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, regval));
}

static int
urtwx_mask2shift(uint32_t mask)
{
	uint32_t shift;

	for (shift = 0; shift < 32; shift++) {
		if (mask & (1U << shift))
			return (shift);
	}

	return (0);
}

static uint32_t
urtwx_read_4_mask(struct urtwx_softc *sc, uint16_t reg, uint32_t mask)
{
	uint32_t regval;

	regval = urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0);
	return ((regval & mask) >> urtwx_mask2shift(mask));
}

static int
urtwx_write_4_field(struct urtwx_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t val)
{
	return (urtwx_write_4_mask(sc, reg, mask,
	    (val << urtwx_mask2shift(mask)) & mask));
}

static int
urtwx_check_hw_ready(struct urtwx_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t target)
{
	int cnt;

	for (cnt = 0; cnt < 1000; cnt++) {
		if (urtwx_read_4_mask(sc, reg, mask) == target)
			return (1);
		delay(10);
	}

	return (0);
}

static int
urtwx_restore_regs(struct urtwx_softc *sc, struct urtwx_backup_info *bckp,
    size_t num)
{
	size_t i;
	int ret;

	for (i = 0; i < num; i++, bckp++) {
		switch (bckp->len) {
		case 1:
			ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, bckp->reg, 0,
			    bckp->val);
			break;
		case 2:
			ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, bckp->reg, 0,
			    bckp->val);
			break;
		case 4:
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, bckp->reg, 0,
			    bckp->val);
			break;
		default:
			ret = EINVAL;
			break;
		}
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
urtwx_8822c_ltecoex_read(struct urtwx_softc *sc, uint16_t offset, uint32_t *val)
{
	int ret;

	ret = urtwx_wait_4(sc, LTECOEX_ACCESS_CTRL, LTECOEX_READY,
	    LTECOEX_READY, URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, LTECOEX_ACCESS_CTRL, 0,
	    0x800f0000U | offset);
	if (ret != 0)
		return (ret);
	*val = urtwx_read_4(sc, RTW_USB_CMD_REQ, LTECOEX_READ_DATA, 0);
	return (0);
}

static int
urtwx_8822c_ltecoex_write(struct urtwx_softc *sc, uint16_t offset,
    uint32_t val)
{
	int ret;

	ret = urtwx_wait_4(sc, LTECOEX_ACCESS_CTRL, LTECOEX_READY,
	    LTECOEX_READY, URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, LTECOEX_WRITE_DATA, 0, val);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4(sc, RTW_USB_CMD_REQ, LTECOEX_ACCESS_CTRL, 0,
	    0xc00f0000U | offset));
}

static int
urtwx_8822c_ltecoex_write_mask(struct urtwx_softc *sc, uint16_t offset,
    uint32_t mask, uint32_t val)
{
	uint32_t shift, tmp;
	int ret;

	ret = urtwx_8822c_ltecoex_read(sc, offset, &tmp);
	if (ret != 0)
		return (ret);
	shift = urtwx_mask2shift(mask);
	tmp = (tmp & ~mask) | ((val << shift) & mask);
	return (urtwx_8822c_ltecoex_write(sc, offset, tmp));
}

static int
urtwx_8822c_wifi_only_coex(struct urtwx_softc *sc)
{
	uint32_t ctrl;
	uint32_t wl_trx;
	uint32_t bt_trx;
	uint32_t rf_0x1;
	int share_ant;
	int ret;

	/*
	 * Follow Linux's Wi-Fi-only ownership/grant setup for 8822C:
	 * force Wi-Fi control of the shared mux, GNT_BT low, and GNT_WL high.
	 * This is a focused bring-up step, not full coexistence support.
	 */
	ret = urtwx_write_1_mask(sc, REG_SYS_SDIO_CTRL + 3,
	    BIT_LTE_MUX_CTRL_PATH >> 24, BIT_LTE_MUX_CTRL_PATH >> 24);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_COEX_CTRL,
	    BIT_LTE_COEX_EN, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_WL_TRX_CTRL, 0x0000ffffU,
	    0xffffU);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_BT_TRX_CTRL, 0x0000ffffU,
	    0xffffU);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_COEX_CTRL, 0xc000,
	    URTWX_8822C_GNT_SW_LOW);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_COEX_CTRL, 0x0c00,
	    URTWX_8822C_GNT_SW_LOW);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_COEX_CTRL, 0x3000,
	    URTWX_8822C_GNT_SW_HIGH);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_write_mask(sc, LTE_COEX_CTRL, 0x0300,
	    URTWX_8822C_GNT_SW_HIGH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_PAD_CTRL1 + 2,
	    BIT_BTGP_SPI_EN >> 16, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_PAD_CTRL1 + 3,
	    BIT_BTGP_JTAG_EN >> 24, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_GPIO_MUXCFG + 2,
	    BIT_FSPI_EN >> 16, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_PAD_CTRL1 + 1,
	    BIT_LED1DIS >> 8, 0);
	if (ret != 0)
		return (ret);
	share_ant = !!(sc->sc_bt_setting & 0x01);
	rf_0x1 = 0x40000;
	if (share_ant)
		rf_0x1 |= 1U << 13;
	ret = urtwx_write_rf_path(sc, 1, 0x01, RFREG_MASK, rf_0x1);
	if (ret != 0)
		return (ret);
	if (share_ant) {
		ret = urtwx_write_1_mask(sc, REG_ANAPAR + 2,
		    BIT_ANAPAR_BTPS >> 16, 0);
		if (ret != 0)
			return (ret);
	} else {
		ret = urtwx_write_1_mask(sc, REG_ANAPAR + 2,
		    BIT_ANAPAR_BTPS >> 16, BIT_ANAPAR_BTPS >> 16);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_RSTB_SEL + 1,
		    BIT_DAC_OFF_ENABLE, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_RSTB_SEL + 3,
		    BIT_DAC_OFF_ENABLE, BIT_DAC_OFF_ENABLE);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_1_mask(sc, REG_IGN_GNTBT4,
	    BIT_PI_IGNORE_GNT_BT, BIT_PI_IGNORE_GNT_BT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_IGN_GNT_BT1,
	    BIT_PI_IGNORE_GNT_BT, BIT_PI_IGNORE_GNT_BT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_NOMASK_TXBT,
	    BIT_NOMASK_TXBT_ENABLE, BIT_NOMASK_TXBT_ENABLE);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_read(sc, LTE_COEX_CTRL, &ctrl);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_read(sc, LTE_WL_TRX_CTRL, &wl_trx);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_ltecoex_read(sc, LTE_BT_TRX_CTRL, &bt_trx);
	if (ret != 0)
		return (ret);
	if (sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		printf("%s: 8822c ltecoex ctrl %#x wl_trx %#x bt_trx %#x "
		    "share_ant %d rf1 %#x ign1 %#x ign4 %#x nomask %#x\n",
		    DEVNAME(sc), ctrl, wl_trx, bt_trx, share_ant,
		    urtwx_read_rf_path(sc, 1, 0x01),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_IGN_GNT_BT1, 0),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_IGN_GNTBT4, 0),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_NOMASK_TXBT, 0));
		sc->sc_state_log++;
	}
	return (0);
}

static uint32_t
urtwx_read_rf_path(struct urtwx_softc *sc, uint8_t path, uint8_t addr)
{
	uint16_t reg;

	if (path >= sc->sc_chip->uc_rf_path_count)
		return (0xffffffffU);

	reg = sc->sc_chip->uc_rf_read_base[path] + ((uint16_t)addr << 2);
	return (urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0) & RFREG_MASK);
}

static int
urtwx_write_rf_path(struct urtwx_softc *sc, uint8_t path, uint8_t addr,
    uint32_t mask, uint32_t data)
{
	uint32_t old, shift, value;
	uint16_t reg;

	if (path >= sc->sc_chip->uc_rf_path_count)
		return (EINVAL);

	mask &= RFREG_MASK;
	data &= RFREG_MASK;

	if (mask != RFREG_MASK) {
		old = urtwx_read_rf_path(sc, path, addr);
		shift = urtwx_mask2shift(mask);
		data = (old & ~mask) | ((data << shift) & mask);
	}

	if (sc->sc_chip == &urtwx_chip_8822c && addr != 0x00) {
		reg = sc->sc_chip->uc_rf_write_base[path] +
		    ((uint16_t)addr << 2);
		/*
		 * rtw88/8822c writes full-width RF registers directly through
		 * the USB RF window.  Using a masked RMW here leaves stale
		 * upper bits behind in the write window, and RF18 has been
		 * observed to stay wedged at 0x1400 across all channels.
		 */
		if (mask == RFREG_MASK) {
			if (urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, data) != 0)
				return (EIO);
		} else {
			if (urtwx_write_4_mask(sc, reg, RFREG_MASK, data) != 0)
				return (EIO);
		}
		if (URTWX_8822C_RFIO_LOG_MAX > 0 &&
		    (addr == 0xee || addr == 0x33 || addr == 0x3f ||
		    addr == 0x18) &&
		    sc->sc_rfchan_log < URTWX_8822C_RFIO_LOG_MAX) {
			printf("%s: 8822c rf wr path %u addr %#x reg %#x "
			    "data %#x peek %#x\n", DEVNAME(sc), path, addr,
			    reg, data,
			    urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0) &
			    RFREG_MASK);
			sc->sc_rfchan_log++;
		}
		delay(URTWX_8822C_RF_DELAY_US);
		return (0);
	}

	value = (((uint32_t)addr << 20) | data) & 0x0fffffffU;
	if (urtwx_write_4(sc, RTW_USB_CMD_REQ, sc->sc_chip->uc_rf_sipi[path],
	    0, value) != 0)
		return (EIO);
	if (URTWX_8822C_RFIO_LOG_MAX > 0 &&
	    sc->sc_chip == &urtwx_chip_8822c && addr == 0x00 &&
	    sc->sc_rfchan_log < URTWX_8822C_RFIO_LOG_MAX) {
		printf("%s: 8822c rf sipi path %u reg %#x data %#x rf0 %#x/%#x\n",
		    DEVNAME(sc), path, sc->sc_chip->uc_rf_sipi[path], data,
		    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
		sc->sc_rfchan_log++;
	}
	delay(URTWX_8822C_RF_SIPI_DELAY_US);
	return (0);
}

static uint32_t
urtwx_read_rf(struct urtwx_softc *sc, uint8_t addr)
{
	return (urtwx_read_rf_path(sc, 0, addr));
}

static int
urtwx_wait_1(struct urtwx_softc *sc, uint16_t reg, uint8_t mask, uint8_t target,
    int count, int delay_us)
{
	int i;

	for (i = 0; i < count; i++) {
		if ((urtwx_read_1(sc, RTW_USB_CMD_REQ, reg, 0) & mask) ==
		    target)
			return (0);
		delay(delay_us);
	}

	return (ETIMEDOUT);
}

static int
urtwx_wait_2(struct urtwx_softc *sc, uint16_t reg, uint16_t mask,
    uint16_t target, int count, int delay_us)
{
	int i;

	for (i = 0; i < count; i++) {
		if ((urtwx_read_2(sc, RTW_USB_CMD_REQ, reg, 0) & mask) ==
		    target)
			return (0);
		delay(delay_us);
	}

	return (ETIMEDOUT);
}

static int
urtwx_wait_4(struct urtwx_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t target, int count, int delay_us)
{
	int i;

	for (i = 0; i < count; i++) {
		if ((urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0) & mask) ==
		    target)
			return (0);
		delay(delay_us);
	}

	return (ETIMEDOUT);
}

static int
urtwx_fw_send_h2c(struct urtwx_softc *sc, uint32_t w0, uint32_t w1)
{
	static const uint16_t box_reg[] = {
		REG_HMEBOX0,
		REG_HMEBOX1,
		REG_HMEBOX2,
		REG_HMEBOX3
	};
	static const uint16_t box_ex_reg[] = {
		REG_HMEBOX0_EX,
		REG_HMEBOX1_EX,
		REG_HMEBOX2_EX,
		REG_HMEBOX3_EX
	};
	uint8_t box;
	int ret;

	box = sc->sc_h2c_box & 0x3;
	ret = urtwx_wait_1(sc, REG_HMETFR, 1 << box, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0) {
		printf("%s: H2C box %u busy\n", DEVNAME(sc), box);
		return (ret);
	}
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, box_ex_reg[box], 0, w1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, box_reg[box], 0, w0);
	if (ret != 0)
		return (ret);

	sc->sc_h2c_box = (box + 1) & 0x3;
	return (0);
}

static int
urtwx_fw_default_port(struct urtwx_softc *sc)
{
	uint32_t w0;

	w0 = URTWX_H2C_CMD_DEFAULT_PORT |
	    (URTWX_PORTID_STA << 8) |
	    (URTWX_MACID_STA << 16);

	return (urtwx_fw_send_h2c(sc, w0, 0));
}

static int
urtwx_fw_media_status(struct urtwx_softc *sc, int connected)
{
	uint32_t w0;

	w0 = URTWX_H2C_CMD_MEDIA_STATUS_RPT |
	    (URTWX_MACID_STA << 16);
	if (connected)
		w0 |= 1 << 8;
	return (urtwx_fw_send_h2c(sc, w0, 0));
}

static int
urtwx_fw_ra_info(struct urtwx_softc *sc, struct ieee80211_node *ni)
{
	uint32_t ra_mask, w0, w1;
	uint8_t desc_rate, ldpc, rate_id, sgi;

	urtwx_node_tx_info(ni, &rate_id, &desc_rate, &sgi, &ldpc,
	    &ra_mask);

	w0 = URTWX_H2C_CMD_RA_INFO |
	    (URTWX_MACID_STA << 8) |
	    ((uint32_t)rate_id << 16) |
	    (1U << 21) |
	    ((uint32_t)sgi << 23) |
	    ((uint32_t)ldpc << 26) |
	    (1U << 30);
	w1 = ra_mask;

	return (urtwx_fw_send_h2c(sc, w0, w1));
}

static int
urtwx_fw_rsvd_page(struct urtwx_softc *sc, uint8_t pspoll, uint8_t null,
    uint8_t qos_null)
{
	uint32_t w0, w1;

	w0 = URTWX_H2C_CMD_RSVD_PAGE |
	    ((uint32_t)pspoll << 16) |
	    ((uint32_t)null << 24);
	w1 = qos_null;

	return (urtwx_fw_send_h2c(sc, w0, w1));
}

static int
urtwx_fw_send_h2c_pkt(struct urtwx_softc *sc, const uint8_t *pkt, size_t len)
{
	uint8_t buf[URTWX_H2C_PKT_SIZE];
	uint32_t seq;

	if (len > sizeof(buf))
		return (EFBIG);

	memset(buf, 0, sizeof(buf));
	memcpy(buf, pkt, len);

	seq = sc->sc_h2c_pkt_seq++;
	((uint32_t *)buf)[1] = htole32((letoh32(((uint32_t *)buf)[1]) &
	    0x0000ffffU) | (seq << 16));

	return (urtwx_write_data_h2c(sc, buf, sizeof(buf)));
}

static int
urtwx_fw_send_general_info(struct urtwx_softc *sc)
{
	uint32_t pkt[URTWX_H2C_PKT_SIZE / sizeof(uint32_t)];

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = htole32(URTWX_H2C_PKT_CATEGORY |
	    (URTWX_H2C_PKT_CMD_ID << 8) |
	    (URTWX_H2C_PKT_GENERAL_INFO << 16));
	pkt[1] = htole32(URTWX_H2C_PKT_HDR_SIZE + 4);
	pkt[2] = htole32((uint32_t)URTWX_RSVD_FW_TXBUF_PG_NUM << 16);

	return (urtwx_fw_send_h2c_pkt(sc, (const uint8_t *)pkt,
	    sizeof(pkt)));
}

static int
urtwx_fw_send_phydm_info(struct urtwx_softc *sc)
{
	uint32_t pkt[URTWX_H2C_PKT_SIZE / sizeof(uint32_t)];
	uint8_t fw_rf_type;
	uint8_t ant;

	memset(pkt, 0, sizeof(pkt));
	if (sc->sc_chip->uc_tx_nss >= 2) {
		fw_rf_type = URTWX_FW_RF_2T2R;
		ant = URTWX_BB_PATH_AB;
	} else {
		fw_rf_type = URTWX_FW_RF_1T1R;
		ant = URTWX_BB_PATH_A;
	}

	pkt[0] = htole32(URTWX_H2C_PKT_CATEGORY |
	    (URTWX_H2C_PKT_CMD_ID << 8) |
	    (URTWX_H2C_PKT_PHYDM_INFO << 16));
	pkt[1] = htole32(URTWX_H2C_PKT_HDR_SIZE + 8);
	pkt[2] = htole32((uint32_t)sc->sc_rfe_option |
	    ((uint32_t)fw_rf_type << 8) |
	    ((uint32_t)0 << 16) |
	    ((uint32_t)ant << 24) |
	    ((uint32_t)ant << 28));

	return (urtwx_fw_send_h2c_pkt(sc, (const uint8_t *)pkt,
	    sizeof(pkt)));
}

static uint32_t
urtwx_build_pspoll_frame(struct urtwx_softc *sc, uint8_t *buf)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct ieee80211_frame_pspoll *psp;
	uint16_t aid;

	psp = (struct ieee80211_frame_pspoll *)buf;
	memset(psp, 0, sizeof(*psp));
	psp->i_fc[0] = IEEE80211_FC0_TYPE_CTL |
	    IEEE80211_FC0_SUBTYPE_PS_POLL;
	aid = IEEE80211_AID(ni->ni_associd) | 0xc000;
	USETW(psp->i_aid, aid);
	memcpy(psp->i_bssid, ni->ni_bssid, IEEE80211_ADDR_LEN);
	memcpy(psp->i_ta, ic->ic_myaddr, IEEE80211_ADDR_LEN);

	return (sizeof(*psp));
}

static uint32_t
urtwx_build_null_frame(struct urtwx_softc *sc, uint8_t *buf, int qos)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	struct ieee80211_frame *wh;

	if (qos) {
		struct ieee80211_qosframe *qwh;

		qwh = (struct ieee80211_qosframe *)buf;
		memset(qwh, 0, sizeof(*qwh));
		qwh->i_fc[0] = IEEE80211_FC0_TYPE_DATA |
		    IEEE80211_FC0_SUBTYPE_NODATA |
		    IEEE80211_FC0_SUBTYPE_QOS;
		qwh->i_fc[1] = IEEE80211_FC1_DIR_TODS;
		memcpy(qwh->i_addr1, ni->ni_bssid, IEEE80211_ADDR_LEN);
		memcpy(qwh->i_addr2, ic->ic_myaddr, IEEE80211_ADDR_LEN);
		memcpy(qwh->i_addr3, ni->ni_bssid, IEEE80211_ADDR_LEN);
		USETW(qwh->i_qos, 7);
		return (sizeof(*qwh));
	}

	wh = (struct ieee80211_frame *)buf;
	memset(wh, 0, sizeof(*wh));
	wh->i_fc[0] = IEEE80211_FC0_TYPE_DATA |
	    IEEE80211_FC0_SUBTYPE_NODATA;
	wh->i_fc[1] = IEEE80211_FC1_DIR_TODS;
	memcpy(wh->i_addr1, ni->ni_bssid, IEEE80211_ADDR_LEN);
	memcpy(wh->i_addr2, ic->ic_myaddr, IEEE80211_ADDR_LEN);
	memcpy(wh->i_addr3, ni->ni_bssid, IEEE80211_ADDR_LEN);

	return (sizeof(*wh));
}

static void
urtwx_fill_rsvd_page_desc(struct urtwx_softc *sc, struct urtwx_tx_desc *txd,
    uint32_t len, int qos_null, int nav_use_hdr)
{
	memset(txd, 0, sizeof(*txd));

	txd->w0 = htole32((len & 0xffff) | (URTWX_TXDESC_SIZE << 16) |
	    URTWX_TXDESC_W0_LS |
	    (nav_use_hdr ? 0 : URTWX_TXDESC_W0_DISQSELSEQ));
	txd->w1 = htole32((URTWX_MACID_STA & URTWX_TXDESC_W1_MACID_MASK) |
	    ((uint32_t)URTWX_TXDESC_QSEL_MGMT <<
	    URTWX_TXDESC_W1_QSEL_SHIFT) |
	    ((uint32_t)URTWX_RATEID_B_20M <<
	    URTWX_TXDESC_W1_RATEID_SHIFT));
	if (qos_null)
		txd->w2 = htole32(URTWX_TXDESC_W2_BT_NULL);
	txd->w3 = htole32(URTWX_TXDESC_W3_USE_RATE |
	    URTWX_TXDESC_W3_DISDATAFB |
	    (nav_use_hdr ? URTWX_TXDESC_W3_NAVUSEHDR : 0));
	txd->w4 = htole32(URTWX_DESC_RATE1M);
	if (!nav_use_hdr)
		txd->w8 = htole32(URTWX_TXDESC_W8_EN_HWSEQ);

	urtwx_txdesc_checksum(sc, txd);
}

static int
urtwx_fw_download_sta_rsvd_page(struct urtwx_softc *sc)
{
	struct ieee80211_node *ni = sc->sc_ic.ic_bss;
	struct urtwx_tx_desc *txd;
	uint8_t frame[sizeof(struct ieee80211_qosframe)];
	uint8_t *buf;
	uint8_t null_loc, pspoll_loc, qos_null_loc;
	uint32_t len, off, size;
	uint8_t page;
	int qos, ret;

	buf = malloc(urtwx_rsvd_drv_pg_num(sc) * URTWX_PAGE_SIZE,
	    M_TEMP, M_WAITOK | M_ZERO);

	buf[0] = 0;
	page = 1;

	pspoll_loc = page;
	len = urtwx_build_pspoll_frame(sc, frame);
	off = URTWX_RSVD_PAGE_MARGIN + (page - 1) * URTWX_PAGE_SIZE;
	txd = (struct urtwx_tx_desc *)(buf + off);
	urtwx_fill_rsvd_page_desc(sc, txd, len, 0, 1);
	memcpy(buf + off + URTWX_TXDESC_SIZE, frame, len);
	page += howmany(URTWX_TXDESC_SIZE + len, URTWX_PAGE_SIZE);

	null_loc = page;
	len = urtwx_build_null_frame(sc, frame, 0);
	off = URTWX_RSVD_PAGE_MARGIN + (page - 1) * URTWX_PAGE_SIZE;
	txd = (struct urtwx_tx_desc *)(buf + off);
	urtwx_fill_rsvd_page_desc(sc, txd, len, 0, 0);
	memcpy(buf + off + URTWX_TXDESC_SIZE, frame, len);
	page += howmany(URTWX_TXDESC_SIZE + len, URTWX_PAGE_SIZE);

	qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	qos_null_loc = page;
	len = urtwx_build_null_frame(sc, frame, qos);
	off = URTWX_RSVD_PAGE_MARGIN + (page - 1) * URTWX_PAGE_SIZE;
	txd = (struct urtwx_tx_desc *)(buf + off);
	urtwx_fill_rsvd_page_desc(sc, txd, len, qos, 0);
	memcpy(buf + off + URTWX_TXDESC_SIZE, frame, len);
	page += howmany(URTWX_TXDESC_SIZE + len, URTWX_PAGE_SIZE);

	if (page > urtwx_rsvd_drv_pg_num(sc)) {
		free(buf, M_TEMP, urtwx_rsvd_drv_pg_num(sc) * URTWX_PAGE_SIZE);
		return (EFBIG);
	}

	size = URTWX_RSVD_PAGE_MARGIN + (page - 1) * URTWX_PAGE_SIZE;
	ret = urtwx_write_data_rsvd_page(sc, urtwx_rsvd_drv_addr(sc),
	    buf, size);
	if (ret != 0)
		goto out;
	ret = urtwx_write_data_rsvd_page(sc, urtwx_rsvd_drv_addr(sc),
	    buf, 1);
	if (ret != 0)
		goto out;
	if (sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		DPRINTF(("%s: rsvd page pspoll %u null %u qosnull %u size %u\n",
		    DEVNAME(sc), pspoll_loc, null_loc, qos_null_loc, size));
		sc->sc_state_log++;
	}

	ret = urtwx_fw_rsvd_page(sc, pspoll_loc, null_loc, qos_null_loc);

out:
	free(buf, M_TEMP, urtwx_rsvd_drv_pg_num(sc) * URTWX_PAGE_SIZE);
	return (ret);
}

static int
urtwx_is_powered_on(struct urtwx_softc *sc)
{
	if (urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR, 0) == 0xea)
		return (0);
	if (urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_STATUS1 + 1, 0) & 0x01)
		return (0);

	return (1);
}

static int
urtwx_mac_init_system_cfg(struct urtwx_softc *sc)
{
	uint32_t val32;
	uint8_t val8;
	int ret;

	val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_CPU_DMEM_CON, 0);
	val32 |= BIT_WL_PLATFORM_RST | BIT_DDMA_EN;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_CPU_DMEM_CON, 0, val32);
	if (ret != 0)
		return (ret);

	val8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN + 1, 0);
	val8 |= sc->sc_chip->uc_sys_func_en;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN + 1, 0,
	    val8);
	if (ret != 0)
		return (ret);

	val8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR_EXT + 3, 0);
	val8 = (val8 & 0xf0) | 0x0c;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR_EXT + 3, 0, val8);
	if (ret != 0)
		return (ret);

	val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0);
	if (val32 & BIT_BOOT_FSPI_EN) {
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0,
		    val32 & ~BIT_BOOT_FSPI_EN);
		if (ret != 0)
			return (ret);

		val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_GPIO_MUXCFG, 0);
		val32 &= ~BIT_FSPI_EN;
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_GPIO_MUXCFG, 0,
		    val32);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
urtwx_power_on(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_RSV_CTRL, 0, 0);
	if (ret != 0)
		return (ret);

	if (!urtwx_is_powered_on(sc)) {
		ret = urtwx_write_1_mask(sc, 0x004a, 0x01, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x0005, 0x98, 0);
		if (ret != 0)
			return (ret);

		ret = urtwx_write_1_mask(sc, 0x0020, 0x01, 0x01);
		if (ret != 0)
			return (ret);
		delay(1000);

		ret = urtwx_write_1_mask(sc, 0x0000, 0x20, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x0005, 0x1c, 0);
		if (ret != 0)
			return (ret);

		ret = urtwx_wait_1(sc, 0x0006, 0x02, 0x02,
		    URTWX_PWR_POLL_COUNT, URTWX_PWR_POLL_DELAY_US);
		if (ret != 0)
			return (ret);

		ret = urtwx_write_1_mask(sc, 0x0006, 0x01, 0x01);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x0005, 0x80, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x0005, 0x18, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x10c3, 0x01, 0x01);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x0005, 0x01, 0x01);
		if (ret != 0)
			return (ret);

		ret = urtwx_wait_1(sc, 0x0005, 0x01, 0,
		    URTWX_PWR_POLL_COUNT, URTWX_PWR_POLL_DELAY_US);
		if (ret != 0)
			return (ret);

		ret = urtwx_write_1_mask(sc, 0x0020, 0x08, 0x08);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, 0x007c, 0x02, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_SYS_STATUS1 + 1, 0x01, 0);
		if (ret != 0)
			return (ret);
	}

	return (urtwx_mac_init_system_cfg(sc));
}

static void
urtwx_wlan_cpu_enable(struct urtwx_softc *sc, int enable)
{
	if (enable) {
		(void)urtwx_write_1_mask(sc, REG_RSV_CTRL + 1,
		    BIT_WLMCU_IOIF, BIT_WLMCU_IOIF);
		(void)urtwx_write_1_mask(sc, REG_SYS_FUNC_EN + 1,
		    BIT_FEN_CPUEN, BIT_FEN_CPUEN);
	} else {
		(void)urtwx_write_1_mask(sc, REG_SYS_FUNC_EN + 1,
		    BIT_FEN_CPUEN, 0);
		(void)urtwx_write_1_mask(sc, REG_RSV_CTRL + 1,
		    BIT_WLMCU_IOIF, 0);
	}
}

static void
urtwx_download_firmware_reg_backup(struct urtwx_softc *sc,
    struct urtwx_fw_dl_backup *bckp)
{
	bckp->txdma_pq_map1 = urtwx_read_1(sc, RTW_USB_CMD_REQ,
	    REG_TXDMA_PQ_MAP + 1, 0);
	bckp->cr = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR, 0);
	bckp->fifopage_info1 = urtwx_read_2(sc, RTW_USB_CMD_REQ,
	    REG_FIFOPAGE_INFO_1, 0);
	bckp->rqpn_ctrl2 = urtwx_read_4(sc, RTW_USB_CMD_REQ,
	    REG_RQPN_CTRL_2, 0);
	bckp->bcn_ctrl = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0);

	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP + 1, 0,
	    URTWX_DMA_MAPPING_HIGH << 6);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR, 0,
	    BIT_HCI_TXDMA_EN | BIT_TXDMA_EN);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2CQ_CSR, 0,
	    BIT_H2CQ_FULL);
	(void)urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_1, 0,
	    URTWX_DLFW_HIQ_PGNUM);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RQPN_CTRL_2, 0,
	    bckp->rqpn_ctrl2 | BIT_LD_RQPN);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0,
	    (bckp->bcn_ctrl & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);
}

static void
urtwx_download_firmware_reg_restore(struct urtwx_softc *sc,
    const struct urtwx_fw_dl_backup *bckp)
{
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP + 1, 0,
	    bckp->txdma_pq_map1);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR, 0, bckp->cr);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2CQ_CSR, 0,
	    BIT_H2CQ_FULL);
	(void)urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_1, 0,
	    bckp->fifopage_info1);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RQPN_CTRL_2, 0,
	    bckp->rqpn_ctrl2);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0,
	    bckp->bcn_ctrl);
}

static void
urtwx_download_firmware_reset_platform(struct urtwx_softc *sc)
{
	(void)urtwx_write_1_mask(sc, REG_CPU_DMEM_CON + 2,
	    BIT_WL_PLATFORM_RST >> 16, 0);
	(void)urtwx_write_1_mask(sc, REG_SYS_CLK_CTRL + 1,
	    BIT_CPU_CLK_EN >> 8, 0);
	(void)urtwx_write_1_mask(sc, REG_CPU_DMEM_CON + 2,
	    BIT_WL_PLATFORM_RST >> 16, BIT_WL_PLATFORM_RST >> 16);
	(void)urtwx_write_1_mask(sc, REG_SYS_CLK_CTRL + 1,
	    BIT_CPU_CLK_EN >> 8, BIT_CPU_CLK_EN >> 8);
}

static void
urtwx_txdesc_checksum(struct urtwx_softc *sc, struct urtwx_tx_desc *txd)
{
	uint8_t		*buf;
	uint16_t	csum, word;
	uint32_t	w1;
	size_t		words;
	int		i;

	buf = (uint8_t *)txd;
	buf[URTWX_TXDESC_CSUM_IDX * 2] = 0;
	buf[URTWX_TXDESC_CSUM_IDX * 2 + 1] = 0;

	if (sc->sc_chip == &urtwx_chip_8822c) {
		w1 = letoh32(txd->w1);
		words = ((((w1 & URTWX_TXDESC_W1_PKT_OFFSET_MASK) >>
		    URTWX_TXDESC_W1_PKT_OFFSET_SHIFT) * 8) +
		    URTWX_TXDESC_SIZE) / 2;
	} else
		words = 16;

	csum = 0;
	for (i = 0; i < words; i++) {
		memcpy(&word, buf + i * 2, sizeof(word));
		csum ^= letoh16(word);
	}

	word = htole16(csum);
	memcpy(buf + URTWX_TXDESC_CSUM_IDX * 2, &word, sizeof(word));
}

static int
urtwx_bulk_tx(struct urtwx_softc *sc, uint8_t qsel, int len)
{
	struct usbd_pipe *pipe;
	usbd_status err;

	pipe = urtwx_qsel_to_pipe(sc, qsel);
	if (pipe == NULL)
		return (ENXIO);

	usbd_setup_xfer(sc->sc_tx_xfer, pipe, sc,
	    sc->sc_tx_buf, len, USBD_NO_COPY | USBD_SHORT_XFER_OK |
	    USBD_SYNCHRONOUS, URTWX_TX_TIMEOUT, NULL);
	err = usbd_transfer(sc->sc_tx_xfer);
	if (err != USBD_NORMAL_COMPLETION) {
		printf("%s: bulk tx failed: %s\n", DEVNAME(sc),
		    usbd_errstr(err));
		return (EIO);
	}

	return (0);
}

static void
urtwx_txeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct urtwx_softc *sc = priv;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int s;

	s = splnet();
	sc->sc_tx_active = 0;
	sc->sc_tx_timer = 0;
	ifp->if_timer = (sc->sc_ic.ic_mgt_timer != 0);

	if (__predict_false(status != USBD_NORMAL_COMPLETION)) {
		DPRINTF(("%s: TX status=%d\n", DEVNAME(sc), status));
		if (status == USBD_STALLED && sc->sc_tx_data_pipe != NULL)
			usbd_clear_endpoint_stall_async(sc->sc_tx_data_pipe);
		if (status != USBD_CANCELLED)
			ifp->if_oerrors++;
	}
	if (sc->sc_txeof_log < URTWX_TX_MGMT_LOG_MAX) {
		printf("%s: TX completion status %d\n", DEVNAME(sc), status);
		sc->sc_txeof_log++;
	}
	if (sc->sc_ic.ic_state == IEEE80211_S_RUN &&
	    sc->sc_txeof_data_log < URTWX_TX_DATA_LOG_MAX) {
		DPRINTFN(2, ("%s: TX data completion status %d\n",
		    DEVNAME(sc), status));
		sc->sc_txeof_data_log++;
	}

	if (sc->sc_tx_ni != NULL) {
		ieee80211_release_node(&sc->sc_ic, sc->sc_tx_ni);
		sc->sc_tx_ni = NULL;
	}
	sc->sc_tx_data_pipe = NULL;
	ifq_clr_oactive(&ifp->if_snd);
	urtwx_start(ifp);
	splx(s);
}

static int
urtwx_tx(struct urtwx_softc *sc, struct mbuf *m, struct ieee80211_node *ni,
    uint8_t qsel, int mgmt)
{
	struct urtwx_tx_desc *txd;
	struct ieee80211_frame *wh;
	struct ieee80211_key *k;
	const char *name;
	char sa[18], da[18], bssid[18];
	char hex[3 * 32];
	uint16_t seq;
	uint32_t len;
	usbd_status err;
	struct usbd_pipe *pipe;
	uint32_t ra_mask;
	uint8_t sec_type;
	uint8_t ldpc;
	uint8_t report_sn;
	uint8_t rate;
	uint8_t rate_id;
	uint8_t sgi;
	uint8_t tid;
	uint8_t ampdu_density;
	uint8_t ampdu_num;
	uint32_t txpktlen;
	int hdrlen;
	int ampdu;
	int outi;
	uint8_t *txbuf;

	if (sc->sc_tx_active)
		return (EBUSY);
	wh = mtod(m, struct ieee80211_frame *);
	name = NULL;
	sa[0] = da[0] = bssid[0] = '\0';
	hex[0] = '\0';
	seq = 0;
	report_sn = 0;
	rate_id = URTWX_RATEID_G;
	rate = URTWX_DESC_RATE6M;
	sgi = 0;
	ldpc = 0;
	ra_mask = 0;
	k = NULL;
	sec_type = 0;
	tid = 0;
	ampdu_density = 0;
	ampdu_num = 0;
	txpktlen = m->m_pkthdr.len;
	hdrlen = 0;
	ampdu = 0;
	outi = -1;
	txbuf = NULL;

	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
		k = ieee80211_get_txkey(&sc->sc_ic, wh, ni);
		if (k == NULL)
			return (EINVAL);
		/*
		 * net80211 still marks software-crypto keys explicitly.
		 * Use that slow path only when needed; otherwise let the
		 * 8821C security engine use the CAM entry we installed.
		 */
		if (k->k_flags & IEEE80211_KEY_SWCRYPTO) {
			if ((m = ieee80211_encrypt(&sc->sc_ic, m, k)) ==
			    NULL)
				return (ENOBUFS);
			wh = mtod(m, struct ieee80211_frame *);
			txpktlen = m->m_pkthdr.len;
		} else {
			switch (k->k_cipher) {
			case IEEE80211_CIPHER_WEP40:
				sec_type = URTWX_SEC_TYPE_WEP40;
				break;
			case IEEE80211_CIPHER_WEP104:
				sec_type = URTWX_SEC_TYPE_WEP104;
				break;
			case IEEE80211_CIPHER_TKIP:
				sec_type = URTWX_SEC_TYPE_TKIP;
				break;
			case IEEE80211_CIPHER_CCMP:
				sec_type = URTWX_SEC_TYPE_CCMP;
				break;
			default:
				break;
			}
			if (k->k_cipher == IEEE80211_CIPHER_CCMP)
				txpktlen += IEEE80211_CCMP_HDRLEN;
		}
	}

	len = URTWX_TXDESC_SIZE + m->m_pkthdr.len;
	if (sec_type == URTWX_SEC_TYPE_CCMP && k != NULL &&
	    (k->k_flags & IEEE80211_KEY_SWCRYPTO) == 0)
		len += IEEE80211_CCMP_HDRLEN;
	if (len > URTWX_TXDESC_SIZE + URTWX_MAX_FW_CHUNK)
		return (EFBIG);

#if URTWX_FORCE_DATA_MGMT_Q
	if (!mgmt)
		qsel = URTWX_TXDESC_QSEL_MGMT;
#endif

	memset(sc->sc_tx_buf, 0, len);
	txd = (struct urtwx_tx_desc *)sc->sc_tx_buf;
	txd->w0 = htole32(txpktlen | (URTWX_TXDESC_SIZE << 16) |
	    (IEEE80211_IS_MULTICAST(wh->i_addr1) ? URTWX_TXDESC_W0_BMC : 0) |
	    URTWX_TXDESC_W0_LS);
	txd->w1 = htole32((URTWX_MACID_STA & URTWX_TXDESC_W1_MACID_MASK) |
	    ((uint32_t)qsel << URTWX_TXDESC_W1_QSEL_SHIFT) |
	    ((uint32_t)sec_type << URTWX_TXDESC_W1_SEC_TYPE_SHIFT));
	if (qsel == URTWX_TXDESC_QSEL_HIGH)
		txd->w1 = htole32(letoh32(txd->w1) |
		    URTWX_TXDESC_W1_MORE_DATA);
	if (mgmt) {
		switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
		case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
			name = "probe request";
			break;
		case IEEE80211_FC0_SUBTYPE_AUTH:
			name = "auth";
			break;
		case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
			name = "assoc request";
			break;
		case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
			name = "reassoc request";
			break;
		default:
			name = "management";
			break;
		}
		if (sc->sc_tx_mgmt_log < URTWX_TX_MGMT_LOG_MAX) {
			urtwx_format_mac(wh->i_addr2, sa, sizeof(sa));
			urtwx_format_mac(wh->i_addr1, da, sizeof(da));
			urtwx_format_mac(wh->i_addr3, bssid, sizeof(bssid));
			urtwx_hexbuf(mtod(m, const uint8_t *),
			    MIN((size_t)m->m_pkthdr.len, (size_t)32),
			    hex, sizeof(hex));
			printf("%s: TX %s len %u chan %u "
			    "sa %s da %s bssid %s qsel %u\n",
			    DEVNAME(sc), name, m->m_pkthdr.len,
			    ieee80211_chan2ieee(&sc->sc_ic, ni->ni_chan),
			    sa, da, bssid, qsel);
			sc->sc_tx_mgmt_log++;
		}
		/*
		 * Follow the 8821C USB management-frame template from
		 * Linux: fixed basic rate, no fallback, and HW-managed
		 * sequence numbers.
		 */
		txd->w0 = htole32(letoh32(txd->w0) |
		    URTWX_TXDESC_W0_DISQSELSEQ);
		if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan)) {
			rate_id = URTWX_RATEID_B_20M;
			rate = URTWX_DESC_RATE1M;
		} else {
			rate_id = URTWX_RATEID_G;
			rate = URTWX_DESC_RATE6M;
		}
		txd->w1 = htole32(letoh32(txd->w1) |
		    ((uint32_t)rate_id << URTWX_TXDESC_W1_RATEID_SHIFT));
		txd->w3 = htole32(URTWX_TXDESC_W3_USE_RATE |
		    URTWX_TXDESC_W3_DISDATAFB);
		txd->w4 = htole32(rate);
		txd->w8 = htole32(URTWX_TXDESC_W8_EN_HWSEQ);
		if (sc->sc_chip == &urtwx_chip_8822c &&
		    (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) ==
		    IEEE80211_FC0_SUBTYPE_PROBE_REQ &&
		    sc->sc_tx_mgmt_log < URTWX_TX_MGMT_LOG_MAX) {
			printf("%s: TX probe desc w0=%#x w1=%#x w2=%#x w3=%#x "
			    "w4=%#x w5=%#x w6=%#x w7=%#x w8=%#x w9=%#x\n",
			    DEVNAME(sc), letoh32(txd->w0), letoh32(txd->w1),
			    letoh32(txd->w2), letoh32(txd->w3),
			    letoh32(txd->w4), letoh32(txd->w5),
			    letoh32(txd->w6), letoh32(txd->w7),
			    letoh32(txd->w8), letoh32(txd->w9));
			printf("%s: TX probe hdr %s\n", DEVNAME(sc), hex);
			sc->sc_tx_mgmt_log++;
		}
	} else {
		urtwx_node_tx_info(ni, &rate_id, &rate, &sgi, &ldpc,
		    &ra_mask);
		txd->w1 = htole32(letoh32(txd->w1) |
		    ((uint32_t)rate_id << URTWX_TXDESC_W1_RATEID_SHIFT));
		txd->w4 = htole32(rate);
		txd->w5 = htole32((sgi ? URTWX_TXDESC_W5_DATA_SHORT : 0) |
		    (ldpc ? URTWX_TXDESC_W5_DATA_LDPC : 0));
		if (!IEEE80211_IS_MULTICAST(wh->i_addr1) &&
		    ieee80211_has_qos(wh)) {
			tid = ieee80211_get_qos(wh) & IEEE80211_QOS_TID;
			if (ni->ni_tx_ba[tid].ba_state == IEEE80211_BA_AGREED) {
				ampdu = 1;
				ampdu_density = urtwx_tx_ampdu_density(ni);
				ampdu_num = urtwx_tx_ampdu_num(ni);
				txd->w2 = htole32(letoh32(txd->w2) |
				    URTWX_TXDESC_W2_AGG_EN |
				    ((uint32_t)ampdu_density <<
				    URTWX_TXDESC_W2_AMPDU_DEN_SHIFT));
				txd->w3 = htole32(letoh32(txd->w3) |
				    ((uint32_t)ampdu_num <<
				    URTWX_TXDESC_W3_MAX_AGGNUM_SHIFT));
			}
		}
		if (sc->sc_tx_report_req < URTWX_TX_RPT_REQ_MAX) {
			report_sn = ((++sc->sc_tx_report_sn) << 2) & 0xfc;
			txd->w2 = htole32(letoh32(txd->w2) |
			    URTWX_TXDESC_W2_SPE_RPT);
			txd->w6 = htole32(report_sn &
			    URTWX_TXDESC_W6_SWDEF_MASK);
			sc->sc_tx_report_req++;
		}
		memcpy(&seq, wh->i_seq, sizeof(seq));
		seq = (letoh16(seq) >> IEEE80211_SEQ_SEQ_SHIFT) & 0xfff;
		txd->w9 = htole32((uint32_t)seq <<
		    URTWX_TXDESC_W9_SWSEQ_SHIFT);
		if (sc->sc_tx_data_log < URTWX_TX_DATA_LOG_MAX) {
			DPRINTFN(2, ("%s: TX data len %u qsel %u seq %u chan %u"
			    " rateid %u rate %#x auto-rate rpt %#x agg %d"
			    " tid %u den %u num %u\n",
			    DEVNAME(sc), m->m_pkthdr.len, qsel, seq,
			    ieee80211_chan2ieee(&sc->sc_ic, ni->ni_chan),
			    rate_id, rate, report_sn, ampdu, tid,
			    ampdu_density, ampdu_num));
			sc->sc_tx_data_log++;
		}
	}
	urtwx_txdesc_checksum(sc, txd);

	txbuf = sc->sc_tx_buf + URTWX_TXDESC_SIZE;
	if (sec_type == URTWX_SEC_TYPE_CCMP && k != NULL &&
	    (k->k_flags & IEEE80211_KEY_SWCRYPTO) == 0) {
		hdrlen = ieee80211_get_hdrlen(wh);
		m_copydata(m, 0, hdrlen, txbuf);
		txbuf += hdrlen;

		k->k_tsc++;
		txbuf[0] = k->k_tsc;
		txbuf[1] = k->k_tsc >> 8;
		txbuf[2] = 0;
		txbuf[3] = k->k_id | IEEE80211_WEP_EXTIV;
		txbuf[4] = k->k_tsc >> 16;
		txbuf[5] = k->k_tsc >> 24;
		txbuf[6] = k->k_tsc >> 32;
		txbuf[7] = k->k_tsc >> 40;
		txbuf += IEEE80211_CCMP_HDRLEN;

		m_copydata(m, hdrlen, m->m_pkthdr.len - hdrlen, txbuf);
	} else {
		m_copydata(m, 0, m->m_pkthdr.len, txbuf);
	}
	m_freem(m);

	pipe = urtwx_qsel_to_pipe(sc, qsel);
	if (pipe == NULL)
		return (ENXIO);
	if (sc->sc_chip == &urtwx_chip_8822c && mgmt &&
	    URTWX_8822C_FORCE_MGMT_OUT >= 0 &&
	    URTWX_8822C_FORCE_MGMT_OUT < sc->sc_nbulkout &&
	    sc->sc_out_pipe[URTWX_8822C_FORCE_MGMT_OUT] != NULL)
		pipe = sc->sc_out_pipe[URTWX_8822C_FORCE_MGMT_OUT];
	if (sc->sc_chip == &urtwx_chip_8822c && mgmt &&
	    sc->sc_tx_mgmt_log < URTWX_TX_MGMT_LOG_MAX) {
		for (outi = 0; outi < sc->sc_nbulkout; outi++) {
			if (sc->sc_out_pipe[outi] == pipe)
				break;
		}
		printf("%s: TX mgmt pipe out=%d ed=%#x force=%d\n",
		    DEVNAME(sc),
		    outi < sc->sc_nbulkout ? outi : -1,
		    (outi >= 0 && outi < sc->sc_nbulkout) ?
		    sc->sc_out_ed[outi] : 0,
		    URTWX_8822C_FORCE_MGMT_OUT);
		sc->sc_tx_mgmt_log++;
	}

	sc->sc_tx_active = 1;
	sc->sc_tx_ni = ni;
	sc->sc_tx_data_pipe = pipe;
	usbd_setup_xfer(sc->sc_tx_xfer, pipe, sc,
	    sc->sc_tx_buf, len, USBD_FORCE_SHORT_XFER | USBD_NO_COPY,
	    URTWX_TX_TIMEOUT, urtwx_txeof);
	err = usbd_transfer(sc->sc_tx_xfer);
	if (err != USBD_IN_PROGRESS && err != USBD_NORMAL_COMPLETION) {
		sc->sc_tx_active = 0;
		sc->sc_tx_ni = NULL;
		sc->sc_tx_data_pipe = NULL;
		return (EIO);
	}

	return (0);
}

static int
urtwx_write_data_rsvd_page(struct urtwx_softc *sc, uint16_t pg_addr,
    const uint8_t *buf, uint32_t size)
{
	struct urtwx_tx_desc	*txd;
	uint16_t		 rsvd_pg_head;
	uint32_t		 total_len;
	uint32_t		 txpkt_size;
	uint8_t			 cr1;
	uint8_t			 bcn_ctrl;
	int			 ret;

	txpkt_size = size;
	if (((size + URTWX_TXDESC_SIZE) & (512 - 1)) == 0)
		txpkt_size++;

	total_len = URTWX_TXDESC_SIZE + txpkt_size;
	if (total_len > URTWX_TXDESC_SIZE + URTWX_MAX_FW_CHUNK + 1)
		return (EFBIG);

	cr1 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR + 1, 0);
	bcn_ctrl = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0);

	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2, 0,
	    (pg_addr & BIT_MASK_BCN_HEAD_1_V1) | BIT_BCN_VALID_V1);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR + 1, 0,
	    cr1 | (BIT_ENSWBCN >> 8));
	if (ret != 0)
		goto restore;

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0,
	    (bcn_ctrl & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);
	if (ret != 0)
		goto restore;

	memset(sc->sc_tx_buf, 0, total_len);
	memcpy(sc->sc_tx_buf + URTWX_TXDESC_SIZE, buf, size);

	txd = (struct urtwx_tx_desc *)sc->sc_tx_buf;
	txd->w0 = htole32((txpkt_size & 0xffff) | (URTWX_TXDESC_SIZE << 16) |
	    (1U << 26));
	txd->w1 = htole32(URTWX_TXDESC_QSEL_BEACON << 8);
	urtwx_txdesc_checksum(sc, txd);

	ret = urtwx_bulk_tx(sc, URTWX_TXDESC_QSEL_BEACON, total_len);
	if (ret != 0)
		goto restore;

	ret = urtwx_wait_2(sc, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1,
	    BIT_BCN_VALID_V1, URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);

restore:
	rsvd_pg_head = urtwx_rsvd_boundary(sc);
	(void)urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2, 0,
	    rsvd_pg_head | BIT_BCN_VALID_V1);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0, bcn_ctrl);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR + 1, 0, cr1);

	return (ret);
}

static int
urtwx_write_data_h2c(struct urtwx_softc *sc, const uint8_t *buf, uint32_t size)
{
	struct urtwx_tx_desc *txd;
	uint32_t total_len;
	int ret;

	total_len = URTWX_TXDESC_SIZE + size;
	if (total_len > URTWX_TXDESC_SIZE + URTWX_MAX_FW_CHUNK + 1)
		return (EFBIG);

	memset(sc->sc_tx_buf, 0, total_len);
	memcpy(sc->sc_tx_buf + URTWX_TXDESC_SIZE, buf, size);

	txd = (struct urtwx_tx_desc *)sc->sc_tx_buf;
	txd->w0 = htole32((size & 0xffff) | URTWX_TXDESC_W0_LS);
	txd->w1 = htole32((uint32_t)URTWX_TXDESC_QSEL_H2C <<
	    URTWX_TXDESC_W1_QSEL_SHIFT);
	urtwx_txdesc_checksum(sc, txd);

	ret = urtwx_bulk_tx(sc, URTWX_TXDESC_QSEL_H2C, total_len);
	if (ret == 0 && sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		DPRINTF(("%s: H2C pkt sent len %u first %#x %#x %#x %#x\n",
		    DEVNAME(sc), size, buf[0], buf[1], buf[2], buf[3]));
		sc->sc_state_log++;
	}

	return (ret);
}

static int
urtwx_ddma_enable(struct urtwx_softc *sc, uint32_t src, uint32_t dst,
    uint32_t ctrl)
{
	int ret;

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0SA, 0, src);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0DA, 0, dst);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0CTRL, 0, ctrl);
	if (ret != 0)
		return (ret);

	return (urtwx_wait_4(sc, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US));
}

static int
urtwx_iddma_download_firmware(struct urtwx_softc *sc, uint32_t src,
    uint32_t dst, uint32_t len, int first)
{
	uint32_t ch0_ctrl;
	int ret;

	ret = urtwx_wait_4(sc, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0)
		return (ret);

	ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;
	ch0_ctrl |= len & BIT_MASK_DDMACH0_DLEN;
	if (!first)
		ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;

	return (urtwx_ddma_enable(sc, src, dst, ch0_ctrl));
}

static int
urtwx_check_fw_checksum(struct urtwx_softc *sc, uint32_t addr)
{
	uint32_t ddma_ctrl;
	uint8_t	 fw_ctrl;

	fw_ctrl = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0);
	ddma_ctrl = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0CTRL, 0);
	if (ddma_ctrl & BIT_DDMACH0_CHKSUM_STS) {
		if (addr < OCPBASE_DMEM_88XX) {
			fw_ctrl |= BIT_IMEM_DW_OK;
			fw_ctrl &= ~BIT_IMEM_CHKSUM_OK;
		} else {
			fw_ctrl |= BIT_DMEM_DW_OK;
			fw_ctrl &= ~BIT_DMEM_CHKSUM_OK;
		}
		(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0,
		    fw_ctrl);
		return (EINVAL);
	}

	if (addr < OCPBASE_DMEM_88XX)
		fw_ctrl |= BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK;
	else
		fw_ctrl |= BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK;

	return (urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0, fw_ctrl));
}

static int
urtwx_download_firmware_to_mem(struct urtwx_softc *sc, const uint8_t *data,
    uint32_t src, uint32_t dst)
{
	uint32_t mem_offset;
	uint32_t pkt_size;
	uint32_t residue_size;
	uint32_t val32;
	int first_part;
	int ret;

	mem_offset = 0;
	residue_size = src;
	first_part = 1;

	val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0CTRL, 0);
	val32 |= BIT_DDMACH0_RESET_CHKSUM_STS;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_DDMA_CH0CTRL, 0, val32);
	if (ret != 0)
		return (ret);

	while (residue_size != 0) {
		pkt_size = min(residue_size, (uint32_t)URTWX_MAX_FW_CHUNK);

		ret = urtwx_write_data_rsvd_page(sc, 0, data + mem_offset,
		    pkt_size);
		if (ret != 0)
			return (ret);

		ret = urtwx_iddma_download_firmware(sc,
		    OCPBASE_TXBUF_88XX + URTWX_TXDESC_SIZE,
		    dst + mem_offset, pkt_size, first_part);
		if (ret != 0)
			return (ret);

		first_part = 0;
		mem_offset += pkt_size;
		residue_size -= pkt_size;
	}

	return (urtwx_check_fw_checksum(sc, dst));
}

static int
urtwx_check_firmware_size(const uint8_t *data, size_t size)
{
	const struct rtw_fw_hdr *hdr;
	uint32_t dmem_size;
	uint32_t imem_size;
	uint32_t emem_size;
	uint32_t real_size;

	hdr = (const struct rtw_fw_hdr *)data;

	dmem_size = letoh32(hdr->dmem_size) + FW_HDR_CHKSUM_SIZE;
	imem_size = letoh32(hdr->imem_size) + FW_HDR_CHKSUM_SIZE;
	emem_size = 0;
	if (hdr->mem_usage & 0x10)
		emem_size = letoh32(hdr->emem_size) + FW_HDR_CHKSUM_SIZE;

	real_size = FW_HDR_SIZE + dmem_size + imem_size + emem_size;
	return (real_size == size);
}

static int
urtwx_start_download_firmware(struct urtwx_softc *sc, const uint8_t *data,
    size_t size)
{
	const struct rtw_fw_hdr *hdr;
	const uint8_t *section;
	uint32_t dmem_size;
	uint32_t imem_size;
	uint32_t emem_size;
	uint32_t addr;
	uint16_t fw_ctrl;
	int ret;

	if (!urtwx_check_firmware_size(data, size))
		return (EINVAL);

	hdr = (const struct rtw_fw_hdr *)data;

	dmem_size = letoh32(hdr->dmem_size) + FW_HDR_CHKSUM_SIZE;
	imem_size = letoh32(hdr->imem_size) + FW_HDR_CHKSUM_SIZE;
	emem_size = 0;
	if (hdr->mem_usage & 0x10)
		emem_size = letoh32(hdr->emem_size) + FW_HDR_CHKSUM_SIZE;

	fw_ctrl = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0);
	fw_ctrl &= 0x3800;
	fw_ctrl |= BIT_MCUFWDL_EN;
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0, fw_ctrl);
	if (ret != 0)
		return (ret);

	section = data + FW_HDR_SIZE;
	addr = letoh32(hdr->dmem_addr) & ~0x80000000U;
	ret = urtwx_download_firmware_to_mem(sc, section, dmem_size, addr);
	if (ret != 0)
		return (ret);

	section += dmem_size;
	addr = letoh32(hdr->imem_addr) & ~0x80000000U;
	ret = urtwx_download_firmware_to_mem(sc, section, imem_size, addr);
	if (ret != 0)
		return (ret);

	if (emem_size != 0) {
		section += imem_size;
		addr = letoh32(hdr->emem_addr) & ~0x80000000U;
		ret = urtwx_download_firmware_to_mem(sc, section, emem_size,
		    addr);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
urtwx_download_firmware_end_flow(struct urtwx_softc *sc)
{
	uint16_t fw_ctrl;
	int ret;

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_TXDMA_STATUS, 0,
	    BTI_PAGE_OVF);
	if (ret != 0)
		return (ret);

	fw_ctrl = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0);
	if ((fw_ctrl & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
		return (EINVAL);

	fw_ctrl = (fw_ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN;
	return (urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_MCUFW_CTRL, 0, fw_ctrl));
}

static int
urtwx_download_firmware_validate(struct urtwx_softc *sc)
{
	uint32_t fw_key;
	int ret;

	ret = urtwx_wait_2(sc, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret == 0)
		return (0);

	fw_key = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_FW_DBG7, 0) &
	    FW_KEY_MASK;
	if (fw_key == ILLEGAL_KEY_GROUP)
		printf("%s: firmware rejected by key check\n", DEVNAME(sc));

	return (ret);
}

static int
urtwx_fw_download(struct urtwx_softc *sc)
{
	const struct rtw_fw_hdr	*hdr;
	struct urtwx_fw_dl_backup bckp;
	uint8_t			*fw;
	size_t			 fw_size;
	int			 i;
	int			 ret;
	int			 backed_up;

	fw = NULL;
	fw_size = 0;
	backed_up = 0;

	ret = ENOENT;
	for (i = 0; i < sc->sc_chip->uc_fw_name_count; i++) {
		ret = loadfirmware(sc->sc_chip->uc_fw_names[i], &fw, &fw_size);
		if (ret == 0)
			break;
	}
	if (ret != 0)
		return (ret);

	if (fw_size < sizeof(*hdr)) {
		ret = EINVAL;
		goto fail;
	}

	hdr = (const struct rtw_fw_hdr *)fw;
	if (letoh16(hdr->signature) != sc->sc_chip->uc_fw_signature) {
		printf("%s: unexpected firmware signature %#x\n", DEVNAME(sc),
		    letoh16(hdr->signature));
		ret = EINVAL;
		goto fail;
	}
	if (!urtwx_check_firmware_size(fw, fw_size)) {
		printf("%s: firmware size mismatch\n", DEVNAME(sc));
		ret = EINVAL;
		goto fail;
	}

	printf("%s: dmem %u@%#x imem %u@%#x emem %u@%#x\n", DEVNAME(sc),
	    letoh32(hdr->dmem_size), letoh32(hdr->dmem_addr) & ~0x80000000U,
	    letoh32(hdr->imem_size), letoh32(hdr->imem_addr) & ~0x80000000U,
	    (hdr->mem_usage & 0x10) ? letoh32(hdr->emem_size) : 0,
	    (hdr->mem_usage & 0x10) ?
	    (letoh32(hdr->emem_addr) & ~0x80000000U) : 0);

	ret = urtwx_power_on(sc);
	if (ret != 0) {
		printf("%s: power on failed\n", DEVNAME(sc));
		goto fail;
	}
	if (sc->sc_chip == &urtwx_chip_8822c) {
		ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_C2HEVT, 0,
		    URTWX_C2H_HW_FEATURE_DUMP);
		if (ret != 0)
			goto fail;
	}

	urtwx_wlan_cpu_enable(sc, 0);
	urtwx_download_firmware_reg_backup(sc, &bckp);
	backed_up = 1;
	urtwx_download_firmware_reset_platform(sc);

	ret = urtwx_start_download_firmware(sc, fw, fw_size);
	if (ret != 0)
		goto fail;

	urtwx_download_firmware_reg_restore(sc, &bckp);
	backed_up = 0;

	ret = urtwx_download_firmware_end_flow(sc);
	if (ret != 0)
		goto fail;

	urtwx_wlan_cpu_enable(sc, 1);

	ret = urtwx_download_firmware_validate(sc);
	if (ret != 0)
		goto fail;
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_dump_hw_feature(sc);

	free(fw, M_TEMP, fw_size);
	printf("%s: firmware running\n", DEVNAME(sc));
	return (0);

fail:
	if (backed_up)
		urtwx_download_firmware_reg_restore(sc, &bckp);
	(void)urtwx_write_1_mask(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN, 0);
	(void)urtwx_write_1_mask(sc, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN,
	    BIT_FEN_CPUEN);
	if (fw != NULL)
		free(fw, M_TEMP, fw_size);

	return (ret);
}

static int
urtwx_dump_phy_efuse_map(struct urtwx_softc *sc, uint8_t *phy_map)
{
	uint32_t efuse_ctl;
	uint16_t addr;
	int ret;

	ret = urtwx_write_4_mask(sc, REG_LDO_EFUSE_CTRL,
	    BIT_MASK_EFUSE_BANK_SEL, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_LDO_EFUSE_CTRL + 3, BIT_LDO25_EN, 0);
	if (ret != 0)
		return (ret);

	efuse_ctl = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_EFUSE_CTRL, 0);
	for (addr = 0; addr < urtwx_efuse_phy_size(sc); addr++) {
		efuse_ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
		efuse_ctl |= (addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;

		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_EFUSE_CTRL, 0,
		    efuse_ctl & ~BIT_EF_FLAG);
		if (ret != 0)
			return (ret);

		ret = urtwx_wait_4(sc, REG_EFUSE_CTRL, BIT_EF_FLAG,
		    BIT_EF_FLAG, URTWX_EFUSE_POLL_COUNT,
		    URTWX_EFUSE_POLL_DELAY_US);
		if (ret != 0)
			return (ret);

		efuse_ctl = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_EFUSE_CTRL,
		    0);
		phy_map[addr] = efuse_ctl & BIT_MASK_EF_DATA;
	}

	return (0);
}

static int
urtwx_dump_log_efuse_map(struct urtwx_softc *sc, const uint8_t *phy_map,
    uint8_t *log_map)
{
	uint32_t phy_idx;
	uint32_t log_idx;
	uint32_t limit;
	uint32_t log_size;
	uint8_t hdr1, hdr2;
	uint8_t blk_idx;
	uint8_t word_en;
	int i;

	limit = urtwx_efuse_phy_size(sc) - urtwx_efuse_protect_size(sc);
	log_size = urtwx_efuse_log_size(sc);
	for (phy_idx = 0; phy_idx < limit; ) {
		hdr1 = phy_map[phy_idx];
		if (hdr1 == 0xff)
			break;

		if ((hdr1 & 0x1f) == 0x0f) {
			if (phy_idx + 1 >= limit)
				return (EINVAL);
			hdr2 = phy_map[phy_idx + 1];
			if (hdr2 == 0xff)
				break;
			blk_idx = ((hdr2 & 0xf0) >> 1) |
			    ((hdr1 >> 5) & 0x07);
			word_en = hdr2 & 0x0f;
			phy_idx += 2;
		} else {
			blk_idx = (hdr1 & 0xf0) >> 4;
			word_en = hdr1 & 0x0f;
			phy_idx++;
		}

		for (i = 0; i < 4; i++) {
			if (word_en & (1U << i))
				continue;

			log_idx = (blk_idx << 3) + (i << 1);
			if (phy_idx + 1 >= limit ||
			    log_idx + 1 >= log_size)
				return (EINVAL);

			log_map[log_idx] = phy_map[phy_idx];
			log_map[log_idx + 1] = phy_map[phy_idx + 1];
			phy_idx += 2;
		}
	}

	return (0);
}

static int
urtwx_valid_etheraddr(const uint8_t *addr)
{
	static const uint8_t zero[ETHER_ADDR_LEN];
	static const uint8_t ff[ETHER_ADDR_LEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	if (memcmp(addr, zero, ETHER_ADDR_LEN) == 0)
		return (0);
	if (memcmp(addr, ff, ETHER_ADDR_LEN) == 0)
		return (0);
	if (ETHER_IS_MULTICAST(addr))
		return (0);

	return (1);
}

static int
urtwx_parse_efuse_8821c(struct urtwx_softc *sc, const uint8_t *log_map,
    uint8_t *mac_out)
{
	int ret;

	ret = 0;

	if (log_map[URTWX_EFUSE_XTAL_K_OFFSET] != 0xff)
		sc->sc_crystal_cap =
		    log_map[URTWX_EFUSE_XTAL_K_OFFSET] & URTWX_XTAL_CAP_MASK;
	if (log_map[URTWX_EFUSE_TX_BB_SWING_2G] != 0xff)
		sc->sc_tx_bb_swing_2g =
		    log_map[URTWX_EFUSE_TX_BB_SWING_2G];
	if (log_map[URTWX_EFUSE_TX_BB_SWING_5G] != 0xff)
		sc->sc_tx_bb_swing_5g =
		    log_map[URTWX_EFUSE_TX_BB_SWING_5G];
	if (log_map[URTWX_EFUSE_RFE_OPTION] != 0xff) {
		sc->sc_pkg_type =
		    (log_map[URTWX_EFUSE_RFE_OPTION] &
		    URTWX_RFE_OPTION_PKG_MASK) ? 1 : 0;
		sc->sc_rfe_option = log_map[URTWX_EFUSE_RFE_OPTION] &
		    URTWX_RFE_OPTION_BTG_MASK;
		switch (sc->sc_rfe_option) {
		case 0x02:
		case 0x04:
		case 0x07:
		case 0x0a:
		case 0x0c:
		case 0x0f:
			sc->sc_rfe_btg = 1;
			break;
		default:
			sc->sc_rfe_btg = 0;
			break;
		}
	}

	memcpy(mac_out, log_map + URTWX_EFUSE_MAC_OFFSET, ETHER_ADDR_LEN);
	if (!urtwx_valid_etheraddr(mac_out))
		ret = ENOENT;

	return (ret);
}

static int
urtwx_parse_efuse_8822cu(struct urtwx_softc *sc, const uint8_t *log_map,
    uint8_t *mac_out)
{
	static const uint8_t empty_idx[sizeof(sc->sc_8822c_txpwr_idx)] = { 0 };
	static const uint16_t pwr_5g_a[5] = {
		URTWX_EFUSE_8822C_PPG_5GL1_TXA,
		URTWX_EFUSE_8822C_PPG_5GL2_TXA,
		URTWX_EFUSE_8822C_PPG_5GM1_TXA,
		URTWX_EFUSE_8822C_PPG_5GM2_TXA,
		URTWX_EFUSE_8822C_PPG_5GH1_TXA
	};
	static const uint16_t pwr_5g_b[5] = {
		URTWX_EFUSE_8822C_PPG_5GL1_TXB,
		URTWX_EFUSE_8822C_PPG_5GL2_TXB,
		URTWX_EFUSE_8822C_PPG_5GM1_TXB,
		URTWX_EFUSE_8822C_PPG_5GM2_TXB,
		URTWX_EFUSE_8822C_PPG_5GH1_TXB
	};
	uint8_t raw;
	int i;
	int ret;

	ret = 0;
	sc->sc_pkg_type = 0;
	sc->sc_rfe_btg = 0;
	sc->sc_rf_board_option = 0;
	sc->sc_bt_setting = 0;
	sc->sc_usb_mode_switch = 0;
	sc->sc_8822c_have_txpwr_idx = 0;
	sc->sc_8822c_have_bb_pg = 0;
	sc->sc_8822c_have_power_trim = 0;
	memset(sc->sc_8822c_txpwr_idx, 0, sizeof(sc->sc_8822c_txpwr_idx));
	memset(sc->sc_8822c_power_trim, 0, sizeof(sc->sc_8822c_power_trim));
	memset(sc->sc_8822c_thermal_trim, 0xff,
	    sizeof(sc->sc_8822c_thermal_trim));
	memset(sc->sc_8822c_pa_bias_2g, 0xff,
	    sizeof(sc->sc_8822c_pa_bias_2g));
	memset(sc->sc_8822c_pa_bias_5g, 0xff,
	    sizeof(sc->sc_8822c_pa_bias_5g));
	memset(sc->sc_8822c_txpwr_ofs_2g, 0, sizeof(sc->sc_8822c_txpwr_ofs_2g));
	memset(sc->sc_8822c_txpwr_ofs_5g, 0, sizeof(sc->sc_8822c_txpwr_ofs_5g));

	if (log_map[URTWX_EFUSE_8822C_USB_MODE] != 0xff)
		sc->sc_usb_mode_switch =
		    (log_map[URTWX_EFUSE_8822C_USB_MODE] & 0x80) != 0;
	if (log_map[URTWX_EFUSE_8822C_XTAL_K] != 0xff)
		sc->sc_crystal_cap =
		    log_map[URTWX_EFUSE_8822C_XTAL_K] & URTWX_XTAL_CAP_MASK;
	if (log_map[URTWX_EFUSE_8822C_TX_BB_SWING_2G] != 0xff)
		sc->sc_tx_bb_swing_2g =
		    log_map[URTWX_EFUSE_8822C_TX_BB_SWING_2G];
	if (log_map[URTWX_EFUSE_8822C_TX_BB_SWING_5G] != 0xff)
		sc->sc_tx_bb_swing_5g =
		    log_map[URTWX_EFUSE_8822C_TX_BB_SWING_5G];
	if (log_map[URTWX_EFUSE_8822C_RFE_OPTION] != 0xff)
		sc->sc_rfe_option = log_map[URTWX_EFUSE_8822C_RFE_OPTION];
	else
		sc->sc_rfe_option = 0;
	if (log_map[URTWX_EFUSE_8822C_RF_BOARD_OPT] != 0xff)
		sc->sc_rf_board_option =
		    log_map[URTWX_EFUSE_8822C_RF_BOARD_OPT];
	if (log_map[URTWX_EFUSE_8822C_RF_BT_SETTING] != 0xff)
		sc->sc_bt_setting =
		    log_map[URTWX_EFUSE_8822C_RF_BT_SETTING];
	memcpy(sc->sc_8822c_txpwr_idx,
	    log_map + URTWX_EFUSE_8822C_TXPWR_IDX,
	    sizeof(sc->sc_8822c_txpwr_idx));
	if (memcmp(sc->sc_8822c_txpwr_idx, empty_idx,
	    sizeof(sc->sc_8822c_txpwr_idx)) != 0)
		sc->sc_8822c_have_txpwr_idx = 1;

	raw = log_map[URTWX_EFUSE_8822C_PPG_THERMAL_A];
	if (raw != 0xff)
		sc->sc_8822c_thermal_trim[0] = ((raw & 0x0e) >> 1) |
		    ((raw & 0x01) << 3);
	raw = log_map[URTWX_EFUSE_8822C_PPG_THERMAL_B];
	if (raw != 0xff)
		sc->sc_8822c_thermal_trim[1] = ((raw & 0x0e) >> 1) |
		    ((raw & 0x01) << 3);

	raw = log_map[URTWX_EFUSE_8822C_PPG_PABIAS_2GA];
	if (raw != 0xff)
		sc->sc_8822c_pa_bias_2g[0] = raw & URTWX_8822C_PPG_PABIAS_MASK;
	raw = log_map[URTWX_EFUSE_8822C_PPG_PABIAS_2GB];
	if (raw != 0xff)
		sc->sc_8822c_pa_bias_2g[1] = raw & URTWX_8822C_PPG_PABIAS_MASK;
	raw = log_map[URTWX_EFUSE_8822C_PPG_PABIAS_5GA];
	if (raw != 0xff)
		sc->sc_8822c_pa_bias_5g[0] = raw & URTWX_8822C_PPG_PABIAS_MASK;
	raw = log_map[URTWX_EFUSE_8822C_PPG_PABIAS_5GB];
	if (raw != 0xff)
		sc->sc_8822c_pa_bias_5g[1] = raw & URTWX_8822C_PPG_PABIAS_MASK;

	raw = log_map[URTWX_EFUSE_8822C_PPG_2GL_TXAB];
	if (raw != 0xff) {
		sc->sc_8822c_have_power_trim = 1;
		sc->sc_8822c_power_trim[0][0] = raw & URTWX_8822C_PPG_2G_A_MASK;
		sc->sc_8822c_power_trim[1][0] =
		    (raw & URTWX_8822C_PPG_2G_B_MASK) >> 4;
	}
	raw = log_map[URTWX_EFUSE_8822C_PPG_2GM_TXAB];
	if (raw != 0xff) {
		sc->sc_8822c_have_power_trim = 1;
		sc->sc_8822c_power_trim[0][1] = raw & URTWX_8822C_PPG_2G_A_MASK;
		sc->sc_8822c_power_trim[1][1] =
		    (raw & URTWX_8822C_PPG_2G_B_MASK) >> 4;
	}
	raw = log_map[URTWX_EFUSE_8822C_PPG_2GH_TXAB];
	if (raw != 0xff) {
		sc->sc_8822c_have_power_trim = 1;
		sc->sc_8822c_power_trim[0][2] = raw & URTWX_8822C_PPG_2G_A_MASK;
		sc->sc_8822c_power_trim[1][2] =
		    (raw & URTWX_8822C_PPG_2G_B_MASK) >> 4;
	}
	for (i = 0; i < nitems(pwr_5g_a); i++) {
		raw = log_map[pwr_5g_a[i]];
		if (raw != 0xff) {
			sc->sc_8822c_have_power_trim = 1;
			sc->sc_8822c_power_trim[0][i + 3] =
			    raw & URTWX_8822C_PPG_5G_MASK;
		}
		raw = log_map[pwr_5g_b[i]];
		if (raw != 0xff) {
			sc->sc_8822c_have_power_trim = 1;
			sc->sc_8822c_power_trim[1][i + 3] =
			    raw & URTWX_8822C_PPG_5G_MASK;
		}
	}

	(void)log_map[URTWX_EFUSE_8822C_CHANNEL_PLAN];

	memcpy(mac_out, log_map + URTWX_EFUSE_8822CU_MAC_OFFSET,
	    ETHER_ADDR_LEN);
	if (!urtwx_valid_etheraddr(mac_out))
		ret = ENOENT;

	return (ret);
}

static void
urtwx_8822c_bbpg_rates(uint32_t addr, uint8_t *rates, size_t *nrates)
{
	*nrates = 0;

	switch (addr) {
	case 0x0c20:
	case 0x0e20:
		rates[0] = URTWX_DESC_RATE1M;
		rates[1] = 0x01;
		rates[2] = 0x02;
		rates[3] = URTWX_DESC_RATE11M;
		*nrates = 4;
		break;
	case 0x0c24:
	case 0x0e24:
		rates[0] = URTWX_DESC_RATE6M;
		rates[1] = 0x05;
		rates[2] = 0x06;
		rates[3] = 0x07;
		*nrates = 4;
		break;
	case 0x0c28:
	case 0x0e28:
		rates[0] = 0x08;
		rates[1] = 0x09;
		rates[2] = 0x0a;
		rates[3] = URTWX_DESC_RATE54M;
		*nrates = 4;
		break;
	case 0x0c2c:
	case 0x0e2c:
		rates[0] = 0x0c;
		rates[1] = 0x0d;
		rates[2] = 0x0e;
		rates[3] = 0x0f;
		*nrates = 4;
		break;
	case 0x0c30:
	case 0x0e30:
		rates[0] = 0x10;
		rates[1] = 0x11;
		rates[2] = 0x12;
		rates[3] = URTWX_DESC_RATEMCS7;
		*nrates = 4;
		break;
	case 0x0c34:
	case 0x0e34:
		rates[0] = 0x14;
		rates[1] = 0x15;
		rates[2] = 0x16;
		rates[3] = 0x17;
		*nrates = 4;
		break;
	case 0x0c38:
	case 0x0e38:
		rates[0] = 0x18;
		rates[1] = 0x19;
		rates[2] = 0x1a;
		rates[3] = URTWX_DESC_RATEMCS15;
		*nrates = 4;
		break;
	case 0x0c3c:
	case 0x0e3c:
		rates[0] = 0x2c;
		rates[1] = 0x2d;
		rates[2] = 0x2e;
		rates[3] = 0x2f;
		*nrates = 4;
		break;
	case 0x0c40:
	case 0x0e40:
		rates[0] = 0x30;
		rates[1] = 0x31;
		rates[2] = 0x32;
		rates[3] = 0x33;
		*nrates = 4;
		break;
	case 0x0c44:
	case 0x0e44:
		rates[0] = 0x34;
		rates[1] = URTWX_DESC_RATEVHT1SS_MCS9;
		rates[2] = 0x36;
		rates[3] = 0x37;
		*nrates = 4;
		break;
	case 0x0c48:
	case 0x0e48:
		rates[0] = 0x38;
		rates[1] = 0x39;
		rates[2] = 0x3a;
		rates[3] = 0x3b;
		*nrates = 4;
		break;
	case 0x0c4c:
	case 0x0e4c:
		rates[0] = 0x3c;
		rates[1] = 0x3d;
		rates[2] = 0x3e;
		rates[3] = URTWX_DESC_RATEVHT2SS_MCS9;
		*nrates = 4;
		break;
	}
}

static void
urtwx_8822c_bbpg_normalize(int8_t *ofs, const uint8_t *rates, size_t nrates)
{
	int8_t base;
	size_t i;
	uint8_t base_idx;

	if (nrates == 0)
		return;

	if (nrates == 10)
		base_idx = rates[nrates - 3];
	else
		base_idx = rates[nrates - 1];
	base = ofs[base_idx];

	for (i = 0; i < nrates; i++)
		ofs[rates[i]] -= base;
}

static void
urtwx_8822c_parse_bb_pg(struct urtwx_softc *sc)
{
	static const uint8_t cck_rates[] = {
		URTWX_DESC_RATE1M, 0x01, 0x02, URTWX_DESC_RATE11M
	};
	static const uint8_t ofdm_rates[] = {
		URTWX_DESC_RATE6M, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, URTWX_DESC_RATE54M
	};
	static const uint8_t ht1_rates[] = {
		0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, URTWX_DESC_RATEMCS7
	};
	static const uint8_t ht2_rates[] = {
		0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, URTWX_DESC_RATEMCS15
	};
	static const uint8_t vht1_rates[] = {
		0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34,
		URTWX_DESC_RATEVHT1SS_MCS9
	};
	static const uint8_t vht2_rates[] = {
		0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
		URTWX_DESC_RATEVHT2SS_MCS9
	};
	uint8_t rates[4];
	size_t i, nrates;
	int have;

	have = 0;
	for (i = 0; i < nitems(urtwx_8822c_bb_pg_type0); i++) {
		const struct urtwx_8822c_bbpg *p = &urtwx_8822c_bb_pg_type0[i];
		int8_t *ofs;
		size_t j;

		if (p->rf_path >= nitems(sc->sc_8822c_txpwr_ofs_2g))
			continue;
		if (p->band > 1)
			continue;

		urtwx_8822c_bbpg_rates(p->addr, rates, &nrates);
		if (nrates == 0)
			continue;

		ofs = (p->band == 0) ? sc->sc_8822c_txpwr_ofs_2g[p->rf_path] :
		    sc->sc_8822c_txpwr_ofs_5g[p->rf_path];
		for (j = 0; j < nrates; j++)
			ofs[rates[j]] = (p->data >> (j * 8)) & 0xff;
		have = 1;
	}

	if (!have)
		return;

	for (i = 0; i < nitems(sc->sc_8822c_txpwr_ofs_2g); i++) {
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    cck_rates, nitems(cck_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    ofdm_rates, nitems(ofdm_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    ht1_rates, nitems(ht1_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    ht2_rates, nitems(ht2_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    vht1_rates, nitems(vht1_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_2g[i],
		    vht2_rates, nitems(vht2_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_5g[i],
		    ofdm_rates, nitems(ofdm_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_5g[i],
		    ht1_rates, nitems(ht1_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_5g[i],
		    ht2_rates, nitems(ht2_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_5g[i],
		    vht1_rates, nitems(vht1_rates));
		urtwx_8822c_bbpg_normalize(sc->sc_8822c_txpwr_ofs_5g[i],
		    vht2_rates, nitems(vht2_rates));
	}

	sc->sc_8822c_have_bb_pg = 1;
}

static void
urtwx_8822c_board_info_setup(struct urtwx_softc *sc)
{
	/*
	 * Linux's chip_board_info_setup() is a separate stage after efuse
	 * parse. The full tx-power-limit tables are still missing locally,
	 * but the phy_pg table we have should be applied from the same stage
	 * instead of being hidden behind one specific RFE value.
	 *
	 * All current 8822C RFE definitions use bb_pg_type0, including the
	 * user's rfe_def[3].
	 */
	sc->sc_8822c_have_bb_pg = 0;
	urtwx_8822c_parse_bb_pg(sc);
}

static int
urtwx_read_efuse(struct urtwx_softc *sc, uint8_t *mac_out)
{
	uint8_t	*phy_map;
	uint8_t	*log_map;
	uint16_t phy_size, log_size;
	int	 ret;

	phy_size = urtwx_efuse_phy_size(sc);
	log_size = urtwx_efuse_log_size(sc);
	phy_map = malloc(phy_size, M_TEMP, M_WAITOK);
	log_map = malloc(log_size, M_TEMP, M_WAITOK);
	memset(phy_map, 0, phy_size);
	memset(log_map, 0xff, log_size);

	ret = urtwx_dump_phy_efuse_map(sc, phy_map);
	if (ret != 0)
		goto out;

	ret = urtwx_dump_log_efuse_map(sc, phy_map, log_map);
	if (ret != 0)
		goto out;

	if (sc->sc_chip->uc_parse_efuse == NULL) {
		ret = ENODEV;
		goto out;
	}

	ret = sc->sc_chip->uc_parse_efuse(sc, log_map, mac_out);

out:
	free(log_map, M_TEMP, log_size);
	free(phy_map, M_TEMP, phy_size);
	return (ret);
}

static int
urtwx_bulkout_num(struct urtwx_softc *sc)
{
	if (sc->sc_nbulkout >= 2 && sc->sc_nbulkout <= 4)
		return (sc->sc_nbulkout);

	DPRINTF(("%s: unexpected bulk-out count %d, assuming 2\n",
	    DEVNAME(sc), sc->sc_nbulkout));
	return (2);
}

static int
urtwx_dma_mapping_to_out(uint8_t mapping)
{
	switch (mapping) {
	case URTWX_PQ_MAPPING_HIGH:
		return (0);
	case URTWX_PQ_MAPPING_NORMAL:
		return (1);
	case URTWX_PQ_MAPPING_LOW:
		return (2);
	case URTWX_PQ_MAPPING_EXTRA:
		return (3);
	default:
		return (-1);
	}
}

static struct usbd_pipe *
urtwx_qsel_to_pipe(struct urtwx_softc *sc, uint8_t qsel)
{
	const struct urtwx_rqpn *rqpn;
	uint8_t mapping;
	int out;

	rqpn = &sc->sc_chip->uc_rqpn[urtwx_bulkout_num(sc)];
	switch (qsel) {
	case URTWX_TXDESC_QSEL_BEACON:
	case URTWX_TXDESC_QSEL_HIGH:
	case URTWX_TXDESC_QSEL_H2C:
		mapping = rqpn->dma_map_hi;
		break;
	case URTWX_TXDESC_QSEL_MGMT:
		mapping = rqpn->dma_map_mg;
		break;
	case 0:
	case 3:
		mapping = rqpn->dma_map_be;
		break;
	case 1:
	case 2:
		mapping = rqpn->dma_map_bk;
		break;
	case 4:
	case 5:
		mapping = rqpn->dma_map_vi;
		break;
	case 6:
	case 7:
		mapping = rqpn->dma_map_vo;
		break;
	default:
		mapping = rqpn->dma_map_hi;
		break;
	}

	out = urtwx_dma_mapping_to_out(mapping);
	if (out < 0 || out >= sc->sc_nbulkout ||
	    sc->sc_out_pipe[out] == NULL) {
		if (sc->sc_nbulkout == 0)
			return (NULL);
		return (sc->sc_out_pipe[0]);
	}
	return (sc->sc_out_pipe[out]);
}

static uint8_t
urtwx_tx_data_qsel(struct ieee80211_frame *wh)
{
	uint16_t qos;

	if (IEEE80211_IS_MULTICAST(wh->i_addr1))
		return (URTWX_TXDESC_QSEL_HIGH);
	if (!ieee80211_has_qos(wh))
		return (URTWX_TXDESC_QSEL_BE);

	qos = ieee80211_get_qos(wh);
	return (letoh16(qos) & IEEE80211_QOS_TID);
}

static int
urtwx_phy_match_cond(struct urtwx_softc *sc, uint32_t cond, uint32_t cond2)
{
	uint8_t cut, intf, pkg, rfe;

	(void)cond2;

	cut = (cond & URTWX_PHY_COND_CUT_MASK) >> URTWX_PHY_COND_CUT_SHIFT;
	pkg = (cond & URTWX_PHY_COND_PKG_MASK) >> URTWX_PHY_COND_PKG_SHIFT;
	intf = (cond & URTWX_PHY_COND_INTF_MASK) >> URTWX_PHY_COND_INTF_SHIFT;
	rfe = cond & URTWX_PHY_COND_RFE_MASK;

	if (cut != 0 &&
	    cut != (sc->sc_cut_version != 0 ?
	    sc->sc_cut_version : URTWX_PHY_COND_WILDCARD))
		return (0);
	if (pkg != 0 &&
	    pkg != (sc->sc_pkg_type != 0 ?
	    sc->sc_pkg_type : URTWX_PHY_COND_WILDCARD))
		return (0);
	if (intf != 0 && intf != URTWX_PHY_COND_INTF_USB)
		return (0);
	if (rfe != sc->sc_rfe_option)
		return (0);

	return (1);
}

#if URTWX_8822C_ABORT_AFTER_TABLES
static void
urtwx_8822c_rfwrite_reset(struct urtwx_softc *sc)
{
	sc->sc_rfwrite_active = 0;
	sc->sc_rfwrite_path = 0xff;
	sc->sc_rfwrite_idx = 0xffff;
	sc->sc_rfwrite_tbl_addr = 0;
	sc->sc_rfwrite_tbl_data = 0;
	sc->sc_rfwrite_raw_cnt = 0;
	sc->sc_rfblock_cnt = 0;
}

static void
urtwx_8822c_rfwrite_start(struct urtwx_softc *sc)
{
	sc->sc_rfwrite_active = 1;
	sc->sc_rfwrite_path = 0xff;
	sc->sc_rfwrite_idx = 0xffff;
	sc->sc_rfwrite_tbl_addr = 0;
	sc->sc_rfwrite_tbl_data = 0;
}

static void
urtwx_8822c_rfwrite_stop(struct urtwx_softc *sc)
{
	sc->sc_rfwrite_active = 0;
	sc->sc_rfwrite_path = 0xff;
	sc->sc_rfwrite_idx = 0xffff;
	sc->sc_rfwrite_tbl_addr = 0;
	sc->sc_rfwrite_tbl_data = 0;
}
#endif

static void
urtwx_8822c_rfwrite_set_entry(struct urtwx_softc *sc, uint8_t path,
    uint16_t idx, uint32_t addr, uint32_t data)
{
	sc->sc_rfwrite_path = path;
	sc->sc_rfwrite_idx = idx;
	sc->sc_rfwrite_tbl_addr = addr;
	sc->sc_rfwrite_tbl_data = data;
}

static void
urtwx_8822c_rfwrite_clear_entry(struct urtwx_softc *sc)
{
	sc->sc_rfwrite_path = 0xff;
	sc->sc_rfwrite_idx = 0xffff;
	sc->sc_rfwrite_tbl_addr = 0;
	sc->sc_rfwrite_tbl_data = 0;
}

static void
urtwx_8822c_rfwrite_log(struct urtwx_softc *sc, uint16_t val, uint16_t idx,
    const void *buf, int len)
{
	struct urtwx_8822c_rfwrite_evt *evt;
	struct urtwx_8822c_rfblock_evt *blk;
	const uint8_t *bytes = buf;
	uint16_t blkno;
	uint32_t h;
	int i;

	if (!sc->sc_rfwrite_active || sc->sc_rfwrite_path == 0xff)
		return;
	if (len <= 0 || len > 4)
		return;

	if (sc->sc_rfwrite_idx < 32 &&
	    sc->sc_rfwrite_raw_cnt < URTWX_8822C_RFWRITE_RAW_MAX) {
		evt = &sc->sc_rfwrite_raw[sc->sc_rfwrite_raw_cnt++];
		evt->path = sc->sc_rfwrite_path;
		evt->idx = sc->sc_rfwrite_idx;
		evt->val = val;
		evt->idxreg = idx;
		evt->tbl_addr = sc->sc_rfwrite_tbl_addr;
		evt->tbl_data = sc->sc_rfwrite_tbl_data;
		evt->len = len;
		memset(evt->data, 0, sizeof(evt->data));
		memcpy(evt->data, buf, len);
	}

	blkno = sc->sc_rfwrite_idx >> URTWX_8822C_RFBLOCK_SHIFT;
	if (sc->sc_rfblock_cnt == 0 ||
	    sc->sc_rfblock[sc->sc_rfblock_cnt - 1].path != sc->sc_rfwrite_path ||
	    sc->sc_rfblock[sc->sc_rfblock_cnt - 1].blk != blkno) {
		if (sc->sc_rfblock_cnt >= URTWX_8822C_RFBLOCK_MAX)
			return;
		blk = &sc->sc_rfblock[sc->sc_rfblock_cnt++];
		blk->path = sc->sc_rfwrite_path;
		blk->blk = blkno;
		blk->count = 0;
		blk->hash = 2166136261U;
	}

	blk = &sc->sc_rfblock[sc->sc_rfblock_cnt - 1];
	h = blk->hash;
	h = (h ^ sc->sc_rfwrite_path) * 16777619U;
	h = (h ^ (sc->sc_rfwrite_idx & 0xff)) * 16777619U;
	h = (h ^ (sc->sc_rfwrite_idx >> 8)) * 16777619U;
	h = (h ^ (sc->sc_rfwrite_tbl_addr & 0xff)) * 16777619U;
	h = (h ^ ((sc->sc_rfwrite_tbl_addr >> 8) & 0xff)) * 16777619U;
	h = (h ^ (sc->sc_rfwrite_tbl_data & 0xff)) * 16777619U;
	h = (h ^ ((sc->sc_rfwrite_tbl_data >> 8) & 0xff)) * 16777619U;
	h = (h ^ ((sc->sc_rfwrite_tbl_data >> 16) & 0xff)) * 16777619U;
	h = (h ^ ((sc->sc_rfwrite_tbl_data >> 24) & 0xff)) * 16777619U;
	h = (h ^ (val & 0xff)) * 16777619U;
	h = (h ^ (val >> 8)) * 16777619U;
	for (i = 0; i < len; i++)
		h = (h ^ bytes[i]) * 16777619U;
	blk->hash = h;
	blk->count++;
}

#if URTWX_8822C_ABORT_AFTER_TABLES
static void
urtwx_8822c_usbtrace_reset(struct urtwx_softc *sc)
{
	sc->sc_usbtrace_active = 0;
	sc->sc_usbtrace_sync_pending = 0;
	sc->sc_usbtrace_cnt = 0;
}

static void
urtwx_8822c_usbtrace_start(struct urtwx_softc *sc)
{
	sc->sc_usbtrace_active = 1;
	sc->sc_usbtrace_sync_pending = 0;
}

static void
urtwx_8822c_usbtrace_stop(struct urtwx_softc *sc)
{
	sc->sc_usbtrace_active = 0;
	sc->sc_usbtrace_sync_pending = 0;
}
#endif

static void
urtwx_8822c_usbtrace_log(struct urtwx_softc *sc, uint16_t val, uint16_t idx,
    const void *buf, int len)
{
	struct urtwx_8822c_usbtrace_evt *evt;

	if (!sc->sc_usbtrace_active)
		return;
	if (val == 0x1808 || val == 0x4108)
		sc->sc_usbtrace_sync_pending = 1;
	else if (val == 0x04e0) {
		if (!sc->sc_usbtrace_sync_pending)
			return;
		sc->sc_usbtrace_sync_pending = 0;
	} else
		return;
	if (len <= 0 || len > 4)
		return;
	if (sc->sc_usbtrace_cnt >= URTWX_8822C_USBTRACE_MAX)
		return;

	evt = &sc->sc_usbtrace[sc->sc_usbtrace_cnt++];
	evt->val = val;
	evt->idx = idx;
	evt->len = len;
	memset(evt->data, 0, sizeof(evt->data));
	memcpy(evt->data, buf, len);
}

#if URTWX_8822C_ABORT_AFTER_TABLES
static void
urtwx_8822c_usbtrace_dump(struct urtwx_softc *sc)
{
	const struct urtwx_8822c_usbtrace_evt *evt;
	int i;

	for (i = 0; i < sc->sc_usbtrace_cnt; i++) {
		evt = &sc->sc_usbtrace[i];
		printf("%s: 8822c usbtrace W req=%#x val=%#x idx=%#x len=%u "
		    "data=%02x %02x %02x %02x\n",
		    DEVNAME(sc), RTW_USB_CMD_REQ, evt->val, evt->idx,
		    evt->len, evt->data[0], evt->data[1], evt->data[2],
		    evt->data[3]);
	}
}

static void
urtwx_8822c_rfwrite_dump(struct urtwx_softc *sc)
{
	const struct urtwx_8822c_rfwrite_evt *evt;
	const struct urtwx_8822c_rfblock_evt *blk;
	int i;

	for (i = 0; i < sc->sc_rfwrite_raw_cnt; i++) {
		evt = &sc->sc_rfwrite_raw[i];
		printf("%s: 8822c rfwrite path %u tblidx %u tbladdr %#x "
		    "tbldata %#x val=%#x idx=%#x len=%u data=%02x %02x "
		    "%02x %02x\n", DEVNAME(sc), evt->path, evt->idx,
		    evt->tbl_addr, evt->tbl_data, evt->val, evt->idxreg,
		    evt->len, evt->data[0], evt->data[1], evt->data[2],
		    evt->data[3]);
	}
	for (i = 0; i < sc->sc_rfblock_cnt; i++) {
		blk = &sc->sc_rfblock[i];
		printf("%s: 8822c rfblock path %u blk %u count %u hash %#x\n",
		    DEVNAME(sc), blk->path, blk->blk, blk->count, blk->hash);
	}
}
#endif

static void
urtwx_8822c_dump_phy_stage(struct urtwx_softc *sc, const char *tag)
{
	printf("%s: 8822c %s sys=%#x sys1=%#x rsv=%#x rfctrl=%#x "
	    "wlrf1=%#x 3wirea=%#x 3wireb=%#x rstb=%#x rf0=%#x/%#x "
	    "rfe=%#x rfboard=%#x bt=%#x pkg=%u\n",
	    DEVNAME(sc), tag,
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN + 1, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RSV_CTRL, 0),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_RF_CTRL, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_WLRF1, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE2, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RSTB, 0),
	    urtwx_read_rf_path(sc, 0, 0x0),
	    urtwx_read_rf_path(sc, 1, 0x0),
	    sc->sc_rfe_option, sc->sc_rf_board_option,
	    sc->sc_bt_setting, sc->sc_pkg_type);
}

static void
urtwx_8822c_tbltime_enter(struct urtwx_softc *sc, const char *tag,
    struct timeval *tv)
{
	if (sc->sc_chip != &urtwx_chip_8822c)
		return;

	microuptime(tv);
	printf("%s: 8822c tbltime enter %s t=%lld.%06ld\n", DEVNAME(sc),
	    tag, (long long)tv->tv_sec, (long)tv->tv_usec);
}

static void
urtwx_8822c_tbltime_exit(struct urtwx_softc *sc, const char *tag,
    const struct timeval *start)
{
	struct timeval tv;
	long long delta_ms;

	if (sc->sc_chip != &urtwx_chip_8822c)
		return;

	microuptime(&tv);
	delta_ms = (tv.tv_sec - start->tv_sec) * 1000LL;
	delta_ms += (tv.tv_usec - start->tv_usec) / 1000LL;
	printf("%s: 8822c tbltime exit %s t=%lld.%06ld dur=%lldms\n",
	    DEVNAME(sc), tag, (long long)tv.tv_sec, (long)tv.tv_usec,
	    delta_ms);
}

static int
urtwx_8822c_rf0_retry(struct urtwx_softc *sc, uint8_t path, uint32_t expected)
{
	uint32_t rf0;
	int attempt, ret;

	rf0 = urtwx_read_rf_path(sc, path, 0x0);
	for (attempt = 1; attempt < URTWX_8822C_RF0_RETRY_MAX &&
	    rf0 != expected; attempt++) {
		ret = urtwx_write_rf_path(sc, path, 0x0, RFREG_MASK, expected);
		if (ret != 0)
			return (ret);
		delay(URTWX_8822C_RF0_RETRY_DELAY_US);
		rf0 = urtwx_read_rf_path(sc, path, 0x0);
	}

	printf("%s: 8822c rf0 latch path %u attempts %d final %#x "
	    "expect %#x\n", DEVNAME(sc), path, attempt, rf0, expected);
	return (0);
}

static void
urtwx_8822c_tblwrite_pace(struct urtwx_softc *sc)
{
	if (sc->sc_chip == &urtwx_chip_8822c)
		delay(URTWX_8822C_TBLWRITE_PACE_US);
}

static int
urtwx_phy_cfg(struct urtwx_softc *sc, enum urtwx_phy_cfg cfg, uint8_t rf_path,
    uint32_t addr, uint32_t data)
{
	int ret;

	switch (cfg) {
	case URTWX_PHY_CFG_MAC:
		ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, addr, 0, data);
		if (ret == 0)
			urtwx_8822c_tblwrite_pace(sc);
		return (ret);
	case URTWX_PHY_CFG_AGC:
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, addr, 0, data);
		if (ret == 0)
			urtwx_8822c_tblwrite_pace(sc);
		return (ret);
	case URTWX_PHY_CFG_BB:
		switch (addr) {
		case 0xfe:
			delay(50000);
			return (0);
		case 0xfd:
			delay(5000);
			return (0);
		case 0xfc:
			delay(1000);
			return (0);
		case 0xfb:
			delay(50);
			return (0);
		case 0xfa:
			delay(5);
			return (0);
		case 0xf9:
			delay(URTWX_8822C_RF_DELAY_US);
			return (0);
		default:
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, addr, 0, data);
			if (ret == 0)
				urtwx_8822c_tblwrite_pace(sc);
			return (ret);
		}
	case URTWX_PHY_CFG_RF:
		switch (addr) {
		case 0xffe:
			delay(50000);
			return (0);
		case 0xfe:
			delay(100);
			return (0);
		default:
			if (urtwx_write_rf_path(sc, rf_path, addr, RFREG_MASK,
			    data) != 0)
				return (EIO);
			delay(URTWX_8822C_RF_DELAY_US);
			urtwx_8822c_tblwrite_pace(sc);
			return (0);
		}
	}

	return (EINVAL);
}

static int
urtwx_phy_load_table(struct urtwx_softc *sc, const uint32_t *tbl, size_t size,
    enum urtwx_phy_cfg cfg, uint8_t rf_path)
{
	uint32_t addr, cond, cond2, data;
	uint32_t cut, pkg, intf, rfe;
	int i, is_matched, is_skipped, ret;
	size_t applied, skipped;
	size_t off;

	cond = 0;
	cond2 = 0;
	is_matched = 1;
	is_skipped = 0;
	applied = 0;
	skipped = 0;
	if (size % 2 != 0)
		return (EINVAL);

	for (off = 0; off < size; off += 2) {
		addr = tbl[off];
		data = tbl[off + 1];

		if (addr & URTWX_PHY_COND_POS) {
			i = (addr & URTWX_PHY_COND_BRANCH_MASK) >>
			    URTWX_PHY_COND_BRANCH_SHIFT;
			switch (i) {
			case URTWX_PHY_BRANCH_ENDIF:
				is_matched = 1;
				is_skipped = 0;
				break;
			case URTWX_PHY_BRANCH_ELSE:
				is_matched = !is_skipped;
				break;
			case URTWX_PHY_BRANCH_ELIF:
			case URTWX_PHY_BRANCH_IF:
			default:
				cond = addr;
				cond2 = data;
				break;
			}
			continue;
		}

		if (addr & URTWX_PHY_COND_NEG) {
			if (!is_skipped) {
				if (urtwx_phy_match_cond(sc, cond, cond2)) {
					is_matched = 1;
					is_skipped = 1;
				} else {
					is_matched = 0;
					is_skipped = 0;
				}
			} else
				is_matched = 0;
			continue;
		}

		if (!is_matched)
		{
			skipped++;
			continue;
		}

		if (sc->sc_chip == &urtwx_chip_8822c && cfg == URTWX_PHY_CFG_RF)
			urtwx_8822c_rfwrite_set_entry(sc, rf_path, off / 2,
			    addr, data);
		ret = urtwx_phy_cfg(sc, cfg, rf_path, addr, data);
		if (sc->sc_chip == &urtwx_chip_8822c && cfg == URTWX_PHY_CFG_RF)
			urtwx_8822c_rfwrite_clear_entry(sc);
		if (ret != 0)
			return (ret);
		if (sc->sc_chip == &urtwx_chip_8822c &&
		    cfg == URTWX_PHY_CFG_RF && off == 0 && addr == 0x0 &&
		    data == 0x30000) {
			ret = urtwx_8822c_rf0_retry(sc, rf_path, data);
			if (ret != 0)
				return (ret);
		}
		applied++;
	}

	if (sc->sc_chip == &urtwx_chip_8822c &&
	    sc->sc_phy_cond_log < 8) {
		cut = (cond & URTWX_PHY_COND_CUT_MASK) >>
		    URTWX_PHY_COND_CUT_SHIFT;
		pkg = (cond & URTWX_PHY_COND_PKG_MASK) >>
		    URTWX_PHY_COND_PKG_SHIFT;
		intf = (cond & URTWX_PHY_COND_INTF_MASK) >>
		    URTWX_PHY_COND_INTF_SHIFT;
		rfe = cond & URTWX_PHY_COND_RFE_MASK;
		printf("%s: phy tbl cfg=%d path=%u applied=%zu skipped=%zu "
		    "lastcond cut=%#x pkg=%#x intf=%#x rfe=%#x drvcut=%u "
		    "drvpkg=%u drvrfe=%#x\n", DEVNAME(sc), cfg, rf_path,
		    applied, skipped, cut, pkg, intf, rfe,
		    sc->sc_cut_version != 0 ? sc->sc_cut_version :
		    URTWX_PHY_COND_WILDCARD, sc->sc_pkg_type,
		    sc->sc_rfe_option);
		sc->sc_phy_cond_log++;
	}

	return (0);
}

static int
urtwx_phy_load_tables(struct urtwx_softc *sc)
{
	const struct urtwx_chip_info *chip = sc->sc_chip;
	struct timeval stage_tv, total_tv;
	int path;
	int ret;

	urtwx_8822c_tbltime_enter(sc, "tables", &total_tv);

#if URTWX_8822C_ABORT_AFTER_TABLES
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_rfwrite_reset(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_usbtrace_reset(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		usbd_8822c_usbtrace_reset();
#endif

	urtwx_8822c_tbltime_enter(sc, "mac", &stage_tv);
	ret = urtwx_phy_load_table(sc, chip->uc_mac_tbl.ut_data,
	    chip->uc_mac_tbl.ut_size, URTWX_PHY_CFG_MAC, 0);
	if (ret != 0)
		return (ret);
	urtwx_8822c_tbltime_exit(sc, "mac", &stage_tv);

	urtwx_8822c_tbltime_enter(sc, "bb", &stage_tv);
	ret = urtwx_phy_load_table(sc, chip->uc_bb_tbl.ut_data,
	    chip->uc_bb_tbl.ut_size, URTWX_PHY_CFG_BB, 0);
	if (ret != 0)
		return (ret);
	urtwx_8822c_tbltime_exit(sc, "bb", &stage_tv);

	urtwx_8822c_tbltime_enter(sc, "agc", &stage_tv);
	ret = urtwx_phy_load_table(sc, chip->uc_agc_tbl.ut_data,
	    chip->uc_agc_tbl.ut_size, URTWX_PHY_CFG_AGC, 0);
	if (ret != 0)
		return (ret);
	urtwx_8822c_tbltime_exit(sc, "agc", &stage_tv);
	if (sc->sc_rfe_option == 0x02 || sc->sc_rfe_option == 0x04) {
		urtwx_8822c_tbltime_enter(sc, "agc_btg", &stage_tv);
		ret = urtwx_phy_load_table(sc, chip->uc_agc_btg_type2_tbl.ut_data,
		    chip->uc_agc_btg_type2_tbl.ut_size, URTWX_PHY_CFG_AGC, 0);
		if (ret != 0)
			return (ret);
		urtwx_8822c_tbltime_exit(sc, "agc_btg", &stage_tv);
	}
	if (chip->uc_rfk_init_tbl.ut_data != NULL &&
	    chip->uc_rfk_init_tbl.ut_size != 0) {
		ret = urtwx_write_4_mask(sc, 0x1e24, 1U << 17, 1U << 17);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, 0x1cd0, 1U << 28, 1U << 28);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, 0x1cd0, 1U << 29, 1U << 29);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, 0x1cd0, 1U << 30, 1U << 30);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, 0x1cd0, 1U << 31, 0);
		if (ret != 0)
			return (ret);
		urtwx_8822c_tbltime_enter(sc, "rfk", &stage_tv);
		ret = urtwx_phy_load_table(sc, chip->uc_rfk_init_tbl.ut_data,
		    chip->uc_rfk_init_tbl.ut_size, URTWX_PHY_CFG_BB, 0);
		if (ret != 0)
			return (ret);
		urtwx_8822c_tbltime_exit(sc, "rfk", &stage_tv);
	}

#if URTWX_8822C_ABORT_AFTER_TABLES
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_rfwrite_start(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_usbtrace_start(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		usbd_8822c_usbtrace_start();
#endif

	if (sc->sc_chip == &urtwx_chip_8822c) {
		/*
		 * Linux 8822C applies the RF tables in B-then-A order:
		 * chip->rf_tbl = { rf_b, rf_a }, with rf_path carried in the
		 * table metadata. Our port strips that metadata and passes the
		 * RF path separately, so preserve the effective load order here.
		 */
		for (path = chip->uc_rf_path_count - 1; path >= 0; path--) {
			if (chip->uc_rf_tbl[path].ut_data == NULL ||
			    chip->uc_rf_tbl[path].ut_size == 0)
				continue;
			urtwx_8822c_tbltime_enter(sc,
			    path == 0 ? "rf path 0" : "rf path 1", &stage_tv);
			ret = urtwx_phy_load_table(sc,
			    chip->uc_rf_tbl[path].ut_data,
			    chip->uc_rf_tbl[path].ut_size,
			    URTWX_PHY_CFG_RF, path);
			if (ret != 0)
				return (ret);
			urtwx_8822c_tbltime_exit(sc,
			    path == 0 ? "rf path 0" : "rf path 1", &stage_tv);
		}
	} else {
		for (path = 0; path < chip->uc_rf_path_count; path++) {
			if (chip->uc_rf_tbl[path].ut_data == NULL ||
			    chip->uc_rf_tbl[path].ut_size == 0)
				continue;
			urtwx_8822c_tbltime_enter(sc,
			    path == 0 ? "rf path 0" : "rf path 1", &stage_tv);
			ret = urtwx_phy_load_table(sc,
			    chip->uc_rf_tbl[path].ut_data,
			    chip->uc_rf_tbl[path].ut_size,
			    URTWX_PHY_CFG_RF, path);
			if (ret != 0)
				return (ret);
			urtwx_8822c_tbltime_exit(sc,
			    path == 0 ? "rf path 0" : "rf path 1", &stage_tv);
		}
	}

#if URTWX_8822C_ABORT_AFTER_TABLES
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_rfwrite_stop(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_usbtrace_stop(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		usbd_8822c_usbtrace_stop();
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_rfwrite_dump(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_usbtrace_dump(sc);
	if (sc->sc_chip == &urtwx_chip_8822c)
		usbd_8822c_usbtrace_dump();
#endif
	urtwx_8822c_tbltime_exit(sc, "tables", &total_tv);

	return (0);
}

static int
urtwx_phy_switch_rf_set(struct urtwx_softc *sc, enum urtwx_rf_set rf_set)
{
	uint32_t reg;
	int ret;

	/*
	 * The older RFECTL/LNA/CCA switching sequence below is 8821C-shaped.
	 * Keep it off the 8822C bring-up path until we have a confirmed
	 * 8822C-specific front-end switch model; it is a strong candidate for
	 * muting TX/RX despite otherwise-correct channel programming.
	 */
	if (sc->sc_chip == &urtwx_chip_8822c)
		return (0);

	ret = urtwx_write_4_mask(sc, REG_CPU_DMEM_CON, BIT_WL_PLATFORM_RST,
	    BIT_WL_PLATFORM_RST);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_SYS_CTRL, BIT_FEN_EN, BIT_FEN_EN);
	if (ret != 0)
		return (ret);

	reg = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RFECTL, 0);
	switch (rf_set) {
	case URTWX_SWITCH_TO_BTG:
		reg |= B_BTG_SWITCH;
		reg &= ~(B_CTRL_SWITCH | B_WL_SWITCH | B_WLG_SWITCH |
		    B_WLA_SWITCH);
		ret = urtwx_write_4_mask(sc, REG_ENRXCCA, 0x00ff0000U,
		    URTWX_CCA_2G_BTG << 16);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_ENTXCCK, 0x0000ffffU,
		    URTWX_LNA_2G_BTG);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_SWITCH_TO_WLG:
		reg |= B_WL_SWITCH | B_WLG_SWITCH;
		reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLA_SWITCH);
		ret = urtwx_write_4_mask(sc, REG_ENRXCCA, 0x00ff0000U,
		    URTWX_CCA_2G_WLG << 16);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_ENTXCCK, 0x0000ffffU,
		    URTWX_LNA_2G_WLG);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_SWITCH_TO_WLA:
		reg |= B_WL_SWITCH | B_WLA_SWITCH;
		reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLG_SWITCH);
		break;
	}

	return (urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RFECTL, 0, reg));
}

static int
urtwx_phy_enable_bb_rf(struct urtwx_softc *sc)
{
	uint8_t val8;
	int ret;

	if (sc->sc_chip == &urtwx_chip_8822c) {
		ret = urtwx_write_1_mask(sc, REG_SYS_FUNC_EN,
		    BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST,
		    BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_RF_CTRL,
		    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB,
		    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_WLRF1,
		    URTWX_8822C_BIT_WLRF1_BBRF_EN,
		    URTWX_8822C_BIT_WLRF1_BBRF_EN);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, REG_RSV_CTRL,
		    URTWX_8822C_BITS_RFC_DIRECT,
		    URTWX_8822C_RFC_DIRECT_PI);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, REG_WLRF1,
		    URTWX_8822C_BITS_RFC_DIRECT,
		    URTWX_8822C_RFC_DIRECT_PI);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_DIS_DPD,
		    URTWX_8822C_DIS_DPD_MASK,
		    URTWX_8822C_DIS_DPD_RATEALL);
		if (ret != 0)
			return (ret);
		urtwx_8822c_header_file_init(sc, 1);
		ret = urtwx_phy_load_tables(sc);
		if (ret != 0)
			return (ret);
		urtwx_8822c_dump_phy_stage(sc, "rf tables");
#if URTWX_8822C_ABORT_AFTER_TABLES
		printf("%s: 8822c rfinit stop after tables\n", DEVNAME(sc));
		return (EIO);
#endif
		sc->sc_usbctl_log = 0;
		ret = urtwx_write_4_field(sc, REG_ANAPAR_XTAL_0,
		    URTWX_8822C_BIT_XCAP_0,
		    (uint32_t)sc->sc_crystal_cap |
		    ((uint32_t)sc->sc_crystal_cap << 7));
		if (ret != 0)
			return (ret);
		urtwx_8822c_header_file_init(sc, 0);
		urtwx_8822c_dump_phy_stage(sc, "phy on");
#if URTWX_8822C_ABORT_AFTER_PHYON
	return (EIO);
#endif
	return (0);
}

	val8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
	val8 |= BIT_FEN_PCIEA;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0, val8);
	if (ret != 0)
		return (ret);

	val8 |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0, val8);
	if (ret != 0)
		return (ret);
	val8 &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0, val8);
	if (ret != 0)
		return (ret);
	val8 |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0, val8);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_RF_CTRL, 0,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	if (ret != 0)
		return (ret);
	delay(10);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_WLRF1 + 3, 0,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	if (ret != 0)
		return (ret);
	delay(10);

	if (sc->sc_chip == &urtwx_chip_8822c) {
		ret = urtwx_write_4_mask(sc, REG_WLRF1,
		    URTWX_8822C_BIT_WLRF1_BBRF_EN,
		    URTWX_8822C_BIT_WLRF1_BBRF_EN);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_DIS_DPD,
		    URTWX_8822C_DIS_DPD_MASK,
		    URTWX_8822C_DIS_DPD_RATEALL);
		if (ret != 0)
			return (ret);
		urtwx_8822c_header_file_init(sc, 1);
	} else {
		ret = urtwx_write_4_mask(sc, REG_RXPSEL, BIT_RX_PSEL_RST, 0);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_phy_load_tables(sc);
	if (ret != 0)
		return (ret);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		ret = urtwx_write_4_field(sc, REG_ANAPAR_XTAL_0,
		    0x00fffc00U,
		    sc->sc_crystal_cap | (sc->sc_crystal_cap << 7));
		if (ret != 0)
			return (ret);
		urtwx_8822c_header_file_init(sc, 0);
	} else {
		ret = urtwx_write_4_mask(sc, REG_AFE_XTAL_CTRL, 0x7e000000U,
		    (uint32_t)sc->sc_crystal_cap << 25);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_AFE_PLL_CTRL, 0x0000007eU,
		    (uint32_t)sc->sc_crystal_cap << 1);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_CCK0_FAREPORT,
		    (1U << 18) | (1U << 22), 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_RXPSEL, BIT_RX_PSEL_RST,
		    BIT_RX_PSEL_RST);
		if (ret != 0)
			return (ret);
	}

	if (sc->sc_chip == &urtwx_chip_8822c) {
		printf("%s: 8822c phy on sys=%#x sys1=%#x rfctrl=%#x "
		    "wlrf1=%#x 3wirea=%#x 3wireb=%#x rstb=%#x rfe=%#x "
		    "rfboard=%#x bt=%#x pkg=%u\n",
		    DEVNAME(sc),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN + 1, 0),
		    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_RF_CTRL, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_WLRF1, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE,
			0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE2,
			0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RSTB, 0),
		    sc->sc_rfe_option, sc->sc_rf_board_option,
		    sc->sc_bt_setting, sc->sc_pkg_type);
	}

	return (0);
}

static void
urtwx_8822c_header_file_init(struct urtwx_softc *sc, int pre)
{
	int ret;

	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_3WIRE,
	    URTWX_8822C_BIT_3WIRE_TX_EN | URTWX_8822C_BIT_3WIRE_RX_EN,
	    URTWX_8822C_BIT_3WIRE_TX_EN | URTWX_8822C_BIT_3WIRE_RX_EN);
	if (ret != 0)
		return;
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_3WIRE,
	    URTWX_8822C_BIT_3WIRE_PI_ON, URTWX_8822C_BIT_3WIRE_PI_ON);
	if (ret != 0)
		return;
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_3WIRE2,
	    URTWX_8822C_BIT_3WIRE_TX_EN | URTWX_8822C_BIT_3WIRE_RX_EN,
	    URTWX_8822C_BIT_3WIRE_TX_EN | URTWX_8822C_BIT_3WIRE_RX_EN);
	if (ret != 0)
		return;
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_3WIRE2,
	    URTWX_8822C_BIT_3WIRE_PI_ON, URTWX_8822C_BIT_3WIRE_PI_ON);
	if (ret != 0)
		return;
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_ENCCK,
	    URTWX_8822C_BIT_CCK_OFDM_BLK_EN,
	    pre ? 0 : URTWX_8822C_BIT_CCK_OFDM_BLK_EN);
}

static void
urtwx_8822c_bb_reset(struct urtwx_softc *sc)
{
	uint16_t sysfunc;

	if (sc->sc_chip == &urtwx_chip_8822c) {
		sysfunc = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
		printf("%s: 8822c bbreset pre sysfunc=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc), sysfunc,
		    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	}
	(void)urtwx_write_2_mask(sc, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB,
	    BIT_FEN_BB_RSTB);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		sysfunc = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
		printf("%s: 8822c bbreset set sysfunc=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc), sysfunc,
		    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	}
	(void)urtwx_write_2_mask(sc, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB, 0);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		sysfunc = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
		printf("%s: 8822c bbreset clr sysfunc=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc), sysfunc,
		    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	}
	(void)urtwx_write_2_mask(sc, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB,
	    BIT_FEN_BB_RSTB);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		sysfunc = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
		printf("%s: 8822c bbreset fin sysfunc=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc), sysfunc,
		    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	}
}

static void
urtwx_8822c_toggle_igi(struct urtwx_softc *sc)
{
	uint32_t igi;

	igi = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXIGI, 0) &
	    0x7f;
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_RXIGI, 0x7f, igi - 2);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_RXIGI, 0x7f00,
	    (igi - 2) << 8);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_RXIGI, 0x7f, igi);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_RXIGI, 0x7f00,
	    igi << 8);
}

static int
urtwx_8822c_rstb_3wire(struct urtwx_softc *sc, int enable)
{
	int ret;

	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
	    URTWX_8822C_BIT_RSTB_3WIRE,
	    enable ? URTWX_8822C_BIT_RSTB_3WIRE : 0);
	if (ret != 0)
		return (ret);
	if (!enable)
		return (0);
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANAPAR_A,
	    URTWX_8822C_BIT_ANAPAR_UPDATE,
	    URTWX_8822C_BIT_ANAPAR_UPDATE);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4_mask(sc, URTWX_8822C_REG_ANAPAR_B,
	    URTWX_8822C_BIT_ANAPAR_UPDATE,
	    URTWX_8822C_BIT_ANAPAR_UPDATE));
}

static uint16_t
urtwx_8822c_get_path_write_addr(uint8_t path)
{
	switch (path) {
	case 0:
		return (0x1800);
	case 1:
		return (0x4100);
	default:
		return (0);
	}
}

static uint16_t
urtwx_8822c_get_path_read_addr(uint8_t path)
{
	switch (path) {
	case 0:
		return (0x2800);
	case 1:
		return (0x4500);
	default:
		return (0);
	}
}

static int
urtwx_8822c_dac_backup_reg(struct urtwx_softc *sc,
    struct urtwx_backup_info *backup, uint32_t *backup_rf)
{
	static const uint16_t addrs[URTWX_8822C_DACK_REGS] = {
		0x180c, 0x1810, 0x410c, 0x4110,
		0x1c3c, 0x1c24, 0x1d70, 0x09b4,
		0x1a00, 0x1a14, 0x1d58, 0x1c38,
		0x1e24, 0x1e28, 0x1860, 0x4160
	};
	int i;
	int path;

	for (i = 0; i < URTWX_8822C_DACK_REGS; i++) {
		backup[i].len = 4;
		backup[i].reg = addrs[i];
		backup[i].val = urtwx_read_4(sc, RTW_USB_CMD_REQ, addrs[i], 0);
	}

	for (path = 0; path < URTWX_8822C_DACK_PATHS; path++)
		backup_rf[path] = urtwx_read_rf_path(sc, path, 0x8f);

	return (0);
}

static int
urtwx_8822c_dac_restore_reg(struct urtwx_softc *sc,
    struct urtwx_backup_info *backup, uint32_t *backup_rf)
{
	int ret;
	int path;

	ret = urtwx_restore_regs(sc, backup, URTWX_8822C_DACK_REGS);
	if (ret != 0)
		return (ret);

	for (path = 0; path < URTWX_8822C_DACK_PATHS; path++) {
		ret = urtwx_write_rf_path(sc, path, 0x8f, RFREG_MASK,
		    backup_rf[path]);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static void
urtwx_8822c_rf_minmax_cmp(uint32_t value, uint32_t *min, uint32_t *max)
{
	if (value >= 0x200) {
		if (*min >= 0x200) {
			if (*min > value)
				*min = value;
		} else {
			*min = value;
		}
		if (*max >= 0x200) {
			if (*max < value)
				*max = value;
		}
	} else {
		if (*min < 0x200) {
			if (*min > value)
				*min = value;
		}
		if (*max >= 0x200)
			*max = value;
		else if (*max < value)
			*max = value;
	}
}

static void
urtwx_8822c_dac_iq_sort_one(uint32_t *v1, uint32_t *v2)
{
	uint32_t tmp;

	if (*v1 >= 0x200 && *v2 >= 0x200) {
		if (*v1 > *v2) {
			tmp = *v1;
			*v1 = *v2;
			*v2 = tmp;
		}
	} else if (*v1 < 0x200 && *v2 < 0x200) {
		if (*v1 > *v2) {
			tmp = *v1;
			*v1 = *v2;
			*v2 = tmp;
		}
	} else if (*v1 < 0x200 && *v2 >= 0x200) {
		tmp = *v1;
		*v1 = *v2;
		*v2 = tmp;
	}
}

static void
urtwx_8822c_dac_iq_sort(uint32_t *iv, uint32_t *qv)
{
	int i, j;

	for (i = 0; i < URTWX_8822C_DACK_SAMPLES - 1; i++) {
		for (j = 0; j < URTWX_8822C_DACK_SAMPLES - 1 - i; j++) {
			urtwx_8822c_dac_iq_sort_one(&iv[j], &iv[j + 1]);
			urtwx_8822c_dac_iq_sort_one(&qv[j], &qv[j + 1]);
		}
	}
}

static void
urtwx_8822c_dac_iq_offset(uint32_t *vec, uint32_t *val)
{
	uint32_t p, m, t;
	int i;

	m = 0;
	p = 0;
	for (i = 10; i < URTWX_8822C_DACK_SAMPLES - 10; i++) {
		if (vec[i] > 0x200)
			m += 0x400 - vec[i];
		else
			p += vec[i];
	}

	if (p > m)
		t = (p - m) / (URTWX_8822C_DACK_SAMPLES - 20);
	else {
		t = (m - p) / (URTWX_8822C_DACK_SAMPLES - 20);
		if (t != 0)
			t = 0x400 - t;
	}

	*val = t;
}

static int
urtwx_8822c_dac_iq_check(uint32_t value)
{
	if ((value >= 0x200 && (0x400 - value) > 0x64) ||
	    (value < 0x200 && value > 0x64))
		return (0);
	return (1);
}

static int
urtwx_8822c_dac_cal_iq_sample(struct urtwx_softc *sc, uint32_t *iv,
    uint32_t *qv)
{
	uint32_t temp;
	uint32_t last_i, last_q;
	int cnt, i;
	int rawlog, badlog;
	int j;

	i = 0;
	rawlog = 0;
	badlog = 0;
	last_i = 0;
	last_q = 0;
	for (j = 0; j < URTWX_8822C_DACK_SAMPLES; j++) {
		iv[j] = 0;
		qv[j] = 0;
	}
	for (cnt = 0; i < URTWX_8822C_DACK_SAMPLES && cnt < 10000; cnt++) {
		temp = urtwx_read_4_mask(sc, 0x2dbc, 0x003fffff);
		iv[i] = (temp & 0x3ff000U) >> 12;
		qv[i] = temp & 0x3ffU;
		last_i = iv[i];
		last_q = qv[i];
		if (URTWX_8822C_DACK_VERBOSE && rawlog < 8) {
			printf("%s: 8822c dack sample raw %#x i %#x q %#x\n",
			    DEVNAME(sc), temp, iv[i], qv[i]);
			rawlog++;
		}
		if (urtwx_8822c_dac_iq_check(iv[i]) &&
		    urtwx_8822c_dac_iq_check(qv[i]))
			i++;
		else if (URTWX_8822C_DACK_VERBOSE && badlog < 8) {
			printf("%s: 8822c dack bad sample raw %#x i %#x q %#x\n",
			    DEVNAME(sc), temp, iv[i], qv[i]);
			badlog++;
		}
	}

	if (i != URTWX_8822C_DACK_SAMPLES) {
		printf("%s: 8822c dack iq sample short %d/%d\n", DEVNAME(sc),
		    i, URTWX_8822C_DACK_SAMPLES);
		for (j = i; j < URTWX_8822C_DACK_SAMPLES; j++) {
			iv[j] = last_i;
			qv[j] = last_q;
		}
	}
	return (0);
}

static void
urtwx_8822c_dac_cal_iq_search(struct urtwx_softc *sc, uint32_t *iv,
    uint32_t *qv, uint32_t *i_value, uint32_t *q_value)
{
	uint32_t i_max, q_max, i_min, q_min;
	uint32_t i_delta, q_delta, temp;
	int cnt, i;

	for (cnt = 0; cnt < 100; cnt++) {
		i_min = iv[0];
		i_max = iv[0];
		q_min = qv[0];
		q_max = qv[0];
		for (i = 0; i < URTWX_8822C_DACK_SAMPLES; i++) {
			urtwx_8822c_rf_minmax_cmp(iv[i], &i_min, &i_max);
			urtwx_8822c_rf_minmax_cmp(qv[i], &q_min, &q_max);
		}

		if (i_max < 0x200 && i_min < 0x200)
			i_delta = i_max - i_min;
		else if (i_max >= 0x200 && i_min >= 0x200)
			i_delta = i_max - i_min;
		else
			i_delta = i_max + (0x400 - i_min);

		if (q_max < 0x200 && q_min < 0x200)
			q_delta = q_max - q_min;
		else if (q_max >= 0x200 && q_min >= 0x200)
			q_delta = q_max - q_min;
		else
			q_delta = q_max + (0x400 - q_min);

		urtwx_8822c_dac_iq_sort(iv, qv);
		if (i_delta <= 5 && q_delta <= 5)
			break;

		temp = urtwx_read_4_mask(sc, 0x2dbc, 0x003fffff);
		iv[0] = (temp & 0x3ff000U) >> 12;
		qv[0] = temp & 0x3ffU;
		temp = urtwx_read_4_mask(sc, 0x2dbc, 0x003fffff);
		iv[URTWX_8822C_DACK_SAMPLES - 1] =
		    (temp & 0x3ff000U) >> 12;
		qv[URTWX_8822C_DACK_SAMPLES - 1] = temp & 0x3ffU;
	}

	urtwx_8822c_dac_iq_offset(iv, i_value);
	urtwx_8822c_dac_iq_offset(qv, q_value);
}

static int
urtwx_8822c_dac_cal_rf_mode(struct urtwx_softc *sc, uint32_t *i_value,
    uint32_t *q_value)
{
	uint32_t iv[URTWX_8822C_DACK_SAMPLES], qv[URTWX_8822C_DACK_SAMPLES];
	uint32_t rf_a, rf_b;
	int ret;

	rf_a = urtwx_read_rf_path(sc, 0, 0x0);
	rf_b = urtwx_read_rf_path(sc, 1, 0x0);
	printf("%s: 8822c dack rf path-a=%#x path-b=%#x\n", DEVNAME(sc),
	    rf_a, rf_b);

	ret = urtwx_8822c_dac_cal_iq_sample(sc, iv, qv);
	if (ret != 0)
		return (ret);
	urtwx_8822c_dac_cal_iq_search(sc, iv, qv, i_value, q_value);
	return (0);
}

static int
urtwx_8822c_dac_bb_setting(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_write_4_field(sc, 0x1d58, 0x00000ff8U, 0x1ff);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x1a00, 0x00000003U, 0x2);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x1a14, 0x00000300U, 0x3);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1d70, 0, 0x7e7e7e7e);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x180c, 0x00000003U, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x410c, 0x00000003U, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1b00, 0, 0x00000008);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, 0x1bcc, 0, 0x3f);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1b00, 0, 0x0000000a);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, 0x1bcc, 0, 0x3f);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x1e24, 1U << 31, 0x0);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4_field(sc, 0x1e28, 0x0000000fU, 0x3));
}

static int
urtwx_8822c_dac_cal_adc(struct urtwx_softc *sc, uint8_t path,
    uint32_t *adc_ic, uint32_t *adc_qc)
{
	uint16_t base_addr;
	uint32_t path_sel, ic, qc, rf_a, rf_b, temp;
	int i, ret;

	base_addr = urtwx_8822c_get_path_write_addr(path);
	if (base_addr == 0)
		return (EINVAL);
	path_sel = (path == 0) ? 0xa0000U : 0x80000U;

	ret = urtwx_write_4_field(sc, base_addr + 0x30, 1U << 30, 0x0);
	if (ret != 0)
		return (ret);
	if (path == 1) {
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x30, 0,
		    0x30db8041);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x60, 0,
	    0xf0040ff0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0xdff00220);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02dd08c4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0x10000260);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf_path(sc, 0, 0x0, RFREG_MASK, 0x10000);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf_path(sc, 1, 0x0, RFREG_MASK, 0x10000);
	if (ret != 0)
		return (ret);
	printf("%s: 8822c dack adc prep path %u r30=%#x r60=%#x r0c=%#x "
	    "r10=%#x\n", DEVNAME(sc), path,
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, base_addr + 0x30, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, base_addr + 0x60, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0));
	rf_a = urtwx_read_rf_path(sc, 0, 0x0);
	rf_b = urtwx_read_rf_path(sc, 1, 0x0);
	printf("%s: 8822c dack rf0 set path-a=%#x path-b=%#x\n",
	    DEVNAME(sc), rf_a, rf_b);
#if URTWX_8822C_ABORT_AFTER_ADCK_PREP
	return (EIO);
#endif

	for (i = 0; i < URTWX_8822C_DACK_TRIES; i++) {
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c3c, 0,
		    path_sel + 0x8003);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c24, 0,
		    0x00010002);
		if (ret != 0)
			return (ret);
		ret = urtwx_8822c_dac_cal_rf_mode(sc, &ic, &qc);
		if (ret != 0)
			return (ret);
		if (URTWX_8822C_DACK_VERBOSE) {
			printf("%s: 8822c dack adck path %u iter %d before i=%#x q=%#x\n",
			    DEVNAME(sc), path, i, ic, qc);
		}
		if (ic != 0)
			ic = 0x400 - ic;
		if (qc != 0)
			qc = 0x400 - qc;
		if (ic != 0)
			*adc_ic = ic;
		if (qc != 0)
			*adc_qc = qc;
		temp = (ic & 0x3ffU) | ((qc & 0x3ffU) << 10);
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x68, 0,
		    temp);
		if (ret != 0)
			return (ret);
		if (i == 0 && URTWX_8822C_DACK_VERBOSE) {
			printf("%s: 8822c dack adck ctl path %u r1c3c=%#x "
			    "r1c24=%#x r68=%#x\n", DEVNAME(sc), path,
			    urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x1c3c, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x1c24, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ, base_addr + 0x68,
				0));
		}
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c3c, 0,
		    path_sel + 0x8103);
		if (ret != 0)
			return (ret);
		ret = urtwx_8822c_dac_cal_rf_mode(sc, &ic, &qc);
		if (ret != 0)
			return (ret);
		if (URTWX_8822C_DACK_VERBOSE) {
			printf("%s: 8822c dack adck path %u iter %d after i=%#x q=%#x\n",
			    DEVNAME(sc), path, i, ic, qc);
		}
		if (ic >= 0x200)
			ic = 0x400 - ic;
		if (qc >= 0x200)
			qc = 0x400 - qc;
		if (ic < 5 && qc < 5)
			break;
	}

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c3c, 0, 0x00000003);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0x10000260);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c4);
	if (ret != 0)
		return (ret);
	return (urtwx_write_rf_path(sc, path, 0x8f, 1U << 13, 0x1));
}

static int
urtwx_8822c_dac_cal_step1(struct urtwx_softc *sc, uint8_t path, uint32_t adck)
{
	uint16_t base_addr, read_addr;
	int ret;

	base_addr = urtwx_8822c_get_path_write_addr(path);
	read_addr = urtwx_8822c_get_path_read_addr(path);
	if (base_addr == 0 || read_addr == 0)
		return (EINVAL);

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x68, 0, adck);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0xdff00220);
	if (ret != 0)
		return (ret);
	if (path == 0) {
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x60, 0,
		    0xf0040ff0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c38, 0,
		    0xffffffffU);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb66db00);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb0, 0,
	    0x0a11fb88);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xbc, 0,
	    0x0008ff81);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xc0, 0,
	    0x0003d208);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xcc, 0,
	    0x0a11fb88);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xd8, 0,
	    0x0008ff81);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xdc, 0,
	    0x0003d208);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x60000000);
	if (ret != 0)
		return (ret);
	delay(2000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xbc, 0,
	    0x000aff8d);
	if (ret != 0)
		return (ret);
	delay(2000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb0, 0,
	    0x0a11fb89);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xcc, 0,
	    0x0a11fb89);
	if (ret != 0)
		return (ret);
	delay(1000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x62000000);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xd4, 0,
	    0x62000000);
	if (ret != 0)
		return (ret);
	delay(20000);
	if (!urtwx_check_hw_ready(sc, read_addr + 0x08, 0x007fff80U, 0xffff) ||
	    !urtwx_check_hw_ready(sc, read_addr + 0x34, 0x007fff80U, 0xffff)) {
		printf("%s: 8822c dack step1 timeout path %u read %#x\n",
		    DEVNAME(sc), path, read_addr);
	}
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x02000000);
	if (ret != 0)
		return (ret);
	delay(1000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xbc, 0,
	    0x0008ff87);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb6db600);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xbc, 0,
	    0x0008ff87);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x60, 0,
	    0xf0000000));
}

static int
urtwx_8822c_dac_cal_step2(struct urtwx_softc *sc, uint8_t path,
    uint32_t *ic_out, uint32_t *qc_out)
{
	uint16_t base_addr;
	uint32_t ic, qc, ic_in, qc_in;
	int ret;

	base_addr = urtwx_8822c_get_path_write_addr(path);
	if (base_addr == 0)
		return (EINVAL);

	ret = urtwx_write_4_field(sc, base_addr + 0xbc, 0xf0000000U, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xc0, 0x0000000fU, 0x8);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xd8, 0xf0000000U, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xdc, 0x0000000fU, 0x8);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1b00, 0, 0x00000008);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, 0x1bcc, 0, 0x3f);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0xdff00220);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1c3c, 0, 0x00088103);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_dac_cal_rf_mode(sc, &ic_in, &qc_in);
	if (ret != 0)
		return (ret);
	ic = ic_in;
	qc = qc_in;
	if (ic != 0)
		ic = 0x400 - ic;
	if (qc != 0)
		qc = 0x400 - qc;
	if (ic < 0x300) {
		ic = ic * 2 * 6 / 5;
		ic += 0x80;
	} else {
		ic = (0x400 - ic) * 2 * 6 / 5;
		ic = 0x7f - ic;
	}
	if (qc < 0x300) {
		qc = qc * 2 * 6 / 5;
		qc += 0x80;
	} else {
		qc = (0x400 - qc) * 2 * 6 / 5;
		qc = 0x7f - qc;
	}
	if (ic > 0xff) {
		printf("%s: 8822c dack step2 clamp path %u ic %#x -> 0xff\n",
		    DEVNAME(sc), path, ic);
		ic = 0xff;
	}
	if (qc > 0xff) {
		printf("%s: 8822c dack step2 clamp path %u qc %#x -> 0xff\n",
		    DEVNAME(sc), path, qc);
		qc = 0xff;
	}

	*ic_out = ic;
	*qc_out = qc;
	if (URTWX_8822C_DACK_VERBOSE) {
		printf("%s: 8822c dack step2 path %u before i=%#x q=%#x after i=%#x q=%#x\n",
		    DEVNAME(sc), path, ic_in, qc_in, ic, qc);
	}
	return (0);
}

static int
urtwx_8822c_dac_cal_step3(struct urtwx_softc *sc, uint8_t path,
    uint32_t adc_ic, uint32_t adc_qc, uint32_t *ic_in, uint32_t *qc_in,
    uint32_t *i_out, uint32_t *q_out)
{
	uint16_t base_addr, read_addr;
	uint32_t ic, qc, temp;
	int ret;

	base_addr = urtwx_8822c_get_path_write_addr(path);
	read_addr = urtwx_8822c_get_path_read_addr(path);
	if (base_addr == 0 || read_addr == 0)
		return (EINVAL);
	ic = *ic_in;
	qc = *qc_in;

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x0c, 0,
	    0xdff00220);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb66db00);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb0, 0,
	    0x0a11fb88);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xbc, 0,
	    0xc008ff81);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xc0, 0,
	    0x0003d208);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xbc, 0xf0000000U,
	    ic & 0xf);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xc0, 0x0000000fU,
	    (ic & 0xf0) >> 4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xcc, 0,
	    0x0a11fb88);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xd8, 0,
	    0xe008ff81);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xdc, 0,
	    0x0003d208);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xd8, 0xf0000000U,
	    qc & 0xf);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xdc, 0x0000000fU,
	    (qc & 0xf0) >> 4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x60000000);
	if (ret != 0)
		return (ret);
	delay(2000);
	ret = urtwx_write_4_field(sc, base_addr + 0xbc, 0x0000000eU, 0x6);
	if (ret != 0)
		return (ret);
	delay(2000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb0, 0,
	    0x0a11fb89);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xcc, 0,
	    0x0a11fb89);
	if (ret != 0)
		return (ret);
	delay(1000);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x62000000);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xd4, 0,
	    0x62000000);
	if (ret != 0)
		return (ret);
	delay(20000);
	if (!urtwx_check_hw_ready(sc, read_addr + 0x24, 0x07f80000U, ic) ||
	    !urtwx_check_hw_ready(sc, read_addr + 0x50, 0x07f80000U, qc)) {
		printf("%s: 8822c dack step3 timeout path %u read %#x ic %#x "
		    "qc %#x got_i %#x got_q %#x raw_i %#x raw_q %#x\n",
		    DEVNAME(sc), path, read_addr, ic, qc,
		    urtwx_read_4_mask(sc, read_addr + 0x24, 0x07f80000U),
		    urtwx_read_4_mask(sc, read_addr + 0x50, 0x07f80000U),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, read_addr + 0x24, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, read_addr + 0x50, 0));
	}
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0xb8, 0,
	    0x02000000);
	if (ret != 0)
		return (ret);
	delay(1000);
	ret = urtwx_write_4_field(sc, base_addr + 0xbc, 0x0000000eU, 0x3);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb6db600);
	if (ret != 0)
		return (ret);

	temp = ((adc_ic + 0x10) & 0x3ffU) | (((adc_qc + 0x10) & 0x3ffU) << 10);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x68, 0, temp);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x60, 0,
	    0xf0000000);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_dac_cal_rf_mode(sc, &ic, &qc);
	if (ret != 0)
		return (ret);
	if (ic >= 0x10)
		ic -= 0x10;
	else
		ic = 0x400 - (0x10 - ic);
	if (qc >= 0x10)
		qc -= 0x10;
	else
		qc = 0x400 - (0x10 - qc);
	*i_out = ic;
	*q_out = qc;

	if (ic >= 0x200)
		ic = 0x400 - ic;
	if (qc >= 0x200)
		qc = 0x400 - qc;
	*ic_in = ic;
	*qc_in = qc;
	if (URTWX_8822C_DACK_VERBOSE) {
		printf("%s: 8822c dack step3 path %u after-dack i=%#x q=%#x rem_i=%#x rem_q=%#x\n",
		    DEVNAME(sc), path, *i_out, *q_out, *ic_in, *qc_in);
	}
	return (0);
}

static int
urtwx_8822c_dac_cal_step4(struct urtwx_softc *sc, uint8_t path)
{
	uint16_t base_addr;
	int ret;

	base_addr = urtwx_8822c_get_path_write_addr(path);
	if (base_addr == 0)
		return (EINVAL);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x68, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, base_addr + 0x10, 0,
	    0x02d508c4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, base_addr + 0xbc, 0x00000001U, 0x0);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4_field(sc, base_addr + 0x30, 1U << 30, 0x1));
}

static void
urtwx_8822c_dac_cal_backup_vec(struct urtwx_softc *sc, uint8_t path,
    uint8_t vec, uint16_t w_addr, uint16_t r_addr)
{
	uint32_t i;

	if (path >= URTWX_8822C_DACK_PATHS || vec >= 2)
		return;

	for (i = 0; i < URTWX_8822C_DACK_MSBK_NUM; i++) {
		(void)urtwx_write_4_field(sc, w_addr, 0xf0000000U, i);
		sc->sc_8822c_dack_msbk[path][vec][i] =
		    urtwx_read_4_mask(sc, r_addr, 0x07fc0000U);
	}
}

static void
urtwx_8822c_dac_cal_backup_path(struct urtwx_softc *sc, uint8_t path)
{
	uint16_t w_addr, r_addr;

	if (path >= URTWX_8822C_DACK_PATHS)
		return;

	w_addr = urtwx_8822c_get_path_write_addr(path) + 0xb0;
	r_addr = urtwx_8822c_get_path_read_addr(path) + 0x10;
	urtwx_8822c_dac_cal_backup_vec(sc, path, 0, w_addr, r_addr);
	urtwx_8822c_dac_cal_backup_vec(sc, path, 1, w_addr + 0x1c,
	    r_addr + 0x2c);
}

static void
urtwx_8822c_dac_cal_backup_dck(struct urtwx_softc *sc)
{
	sc->sc_8822c_dack_dck[0][0][0] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKA_I_0, 0xf0000000U);
	sc->sc_8822c_dack_dck[0][0][1] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKA_I_1, 0x0000000fU);
	sc->sc_8822c_dack_dck[0][1][0] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKA_Q_0, 0xf0000000U);
	sc->sc_8822c_dack_dck[0][1][1] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKA_Q_1, 0x0000000fU);
	sc->sc_8822c_dack_dck[1][0][0] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKB_I_0, 0xf0000000U);
	sc->sc_8822c_dack_dck[1][0][1] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKB_I_1, 0x0000000fU);
	sc->sc_8822c_dack_dck[1][1][0] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKB_Q_0, 0xf0000000U);
	sc->sc_8822c_dack_dck[1][1][1] =
	    urtwx_read_4_mask(sc, URTWX_8822C_REG_DCKB_Q_1, 0x0000000fU);
}

static void
urtwx_8822c_dac_cal_backup(struct urtwx_softc *sc)
{
	uint32_t temp[3];

	temp[0] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x1860, 0);
	temp[1] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x4160, 0);
	temp[2] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0);

	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb66db00);
	(void)urtwx_write_4_field(sc, 0x1830, 1U << 30, 0x0);
	(void)urtwx_write_4_field(sc, 0x1860, 0xfc000000U, 0x3c);
	urtwx_8822c_dac_cal_backup_path(sc, 0);
	(void)urtwx_write_4_field(sc, 0x4130, 1U << 30, 0x0);
	(void)urtwx_write_4_field(sc, 0x4160, 0xfc000000U, 0x3c);
	urtwx_8822c_dac_cal_backup_path(sc, 1);
	urtwx_8822c_dac_cal_backup_dck(sc);
	(void)urtwx_write_4_field(sc, 0x1830, 1U << 30, 0x1);
	(void)urtwx_write_4_field(sc, 0x4130, 1U << 30, 0x1);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1860, 0, temp[0]);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x4160, 0, temp[1]);
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, temp[2]);
}

static void
urtwx_8822c_dac_cal_restore_dck(struct urtwx_softc *sc)
{
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_DCKA_I_0, 1U << 19,
	    1U << 19);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKA_I_0, 0xf0000000U,
	    sc->sc_8822c_dack_dck[0][0][0]);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKA_I_1, 0x0000000fU,
	    sc->sc_8822c_dack_dck[0][0][1]);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_DCKA_Q_0, 1U << 19,
	    1U << 19);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKA_Q_0, 0xf0000000U,
	    sc->sc_8822c_dack_dck[0][1][0]);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKA_Q_1, 0x0000000fU,
	    sc->sc_8822c_dack_dck[0][1][1]);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_DCKB_I_0, 1U << 19,
	    1U << 19);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKB_I_0, 0xf0000000U,
	    sc->sc_8822c_dack_dck[1][0][0]);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKB_I_1, 0x0000000fU,
	    sc->sc_8822c_dack_dck[1][0][1]);
	(void)urtwx_write_4_mask(sc, URTWX_8822C_REG_DCKB_Q_0, 1U << 19,
	    1U << 19);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKB_Q_0, 0xf0000000U,
	    sc->sc_8822c_dack_dck[1][1][0]);
	(void)urtwx_write_4_field(sc, URTWX_8822C_REG_DCKB_Q_1, 0x0000000fU,
	    sc->sc_8822c_dack_dck[1][1][1]);
}

static void
urtwx_8822c_dac_cal_restore_prepare(struct urtwx_softc *sc)
{
	(void)urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, 0xdb66db00);
	(void)urtwx_write_4_mask(sc, 0x18b0, 1U << 27, 0);
	(void)urtwx_write_4_mask(sc, 0x18cc, 1U << 27, 0);
	(void)urtwx_write_4_mask(sc, 0x41b0, 1U << 27, 0);
	(void)urtwx_write_4_mask(sc, 0x41cc, 1U << 27, 0);
	(void)urtwx_write_4_field(sc, 0x1830, 1U << 30, 0x0);
	(void)urtwx_write_4_field(sc, 0x1860, 0xfc000000U, 0x3c);
	(void)urtwx_write_4_field(sc, 0x18b4, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x18d0, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x4130, 1U << 30, 0x0);
	(void)urtwx_write_4_field(sc, 0x4160, 0xfc000000U, 0x3c);
	(void)urtwx_write_4_field(sc, 0x41b4, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x41d0, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x18b0, 0x00000f00U, 0x0);
	(void)urtwx_write_4_mask(sc, 0x18c0, 1U << 14, 0);
	(void)urtwx_write_4_field(sc, 0x18cc, 0x00000f00U, 0x0);
	(void)urtwx_write_4_mask(sc, 0x18dc, 1U << 14, 0);
	(void)urtwx_write_4_field(sc, 0x18b0, 0x00000001U, 0x0);
	(void)urtwx_write_4_field(sc, 0x18cc, 0x00000001U, 0x0);
	(void)urtwx_write_4_field(sc, 0x18b0, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x18cc, 0x00000001U, 0x1);
	urtwx_8822c_dac_cal_restore_dck(sc);
	(void)urtwx_write_4_field(sc, 0x18c0, 0x00038000U, 0x7);
	(void)urtwx_write_4_field(sc, 0x18dc, 0x00038000U, 0x7);
	(void)urtwx_write_4_field(sc, 0x41c0, 0x00038000U, 0x7);
	(void)urtwx_write_4_field(sc, 0x41dc, 0x00038000U, 0x7);
	(void)urtwx_write_4_field(sc, 0x18b8, (1U << 26) | (1U << 25), 0x1);
	(void)urtwx_write_4_field(sc, 0x18d4, (1U << 26) | (1U << 25), 0x1);
	(void)urtwx_write_4_field(sc, 0x41b0, 0x00000f00U, 0x0);
	(void)urtwx_write_4_mask(sc, 0x41c0, 1U << 14, 0);
	(void)urtwx_write_4_field(sc, 0x41cc, 0x00000f00U, 0x0);
	(void)urtwx_write_4_mask(sc, 0x41dc, 1U << 14, 0);
	(void)urtwx_write_4_field(sc, 0x41b0, 0x00000001U, 0x0);
	(void)urtwx_write_4_field(sc, 0x41cc, 0x00000001U, 0x0);
	(void)urtwx_write_4_field(sc, 0x41b0, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x41cc, 0x00000001U, 0x1);
	(void)urtwx_write_4_field(sc, 0x41b8, (1U << 26) | (1U << 25), 0x1);
	(void)urtwx_write_4_field(sc, 0x41d4, (1U << 26) | (1U << 25), 0x1);
}

static int
urtwx_8822c_dac_cal_restore_wait(struct urtwx_softc *sc, uint16_t target_addr,
    uint16_t toggle_addr)
{
	int cnt;

	for (cnt = 0; cnt < 100; cnt++) {
		(void)urtwx_write_4_field(sc, toggle_addr,
		    (1U << 26) | (1U << 25), 0x0);
		(void)urtwx_write_4_field(sc, toggle_addr,
		    (1U << 26) | (1U << 25), 0x2);
		if (urtwx_read_4_mask(sc, target_addr, 0x0000000fU) == 0x6)
			return (1);
	}

	return (0);
}

static int
urtwx_8822c_dac_cal_restore_path(struct urtwx_softc *sc, uint8_t path)
{
	uint16_t w_i, r_i, w_q, r_q;
	uint32_t i, value;
	int ret;

	if (path >= URTWX_8822C_DACK_PATHS)
		return (EINVAL);

	w_i = urtwx_8822c_get_path_write_addr(path) + 0xb0;
	r_i = urtwx_8822c_get_path_read_addr(path) + 0x08;
	w_q = w_i + 0x1c;
	r_q = r_i + 0x2c;

	if (!urtwx_8822c_dac_cal_restore_wait(sc, r_i, w_i + 0x8))
		return (ETIMEDOUT);

	for (i = 0; i < URTWX_8822C_DACK_MSBK_NUM; i++) {
		ret = urtwx_write_4_field(sc, w_i + 0x4, 0x00000004U, 0x0);
		if (ret != 0)
			return (ret);
		value = sc->sc_8822c_dack_msbk[path][0][i];
		ret = urtwx_write_4_field(sc, w_i + 0x4, 0x00000ff8U, value);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, w_i, 0xf0000000U, i);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, w_i + 0x4, 0x00000004U, 0x1);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_4_field(sc, w_i + 0x4, 0x00000004U, 0x0);
	if (ret != 0)
		return (ret);

	if (!urtwx_8822c_dac_cal_restore_wait(sc, r_q, w_q + 0x8))
		return (ETIMEDOUT);

	for (i = 0; i < URTWX_8822C_DACK_MSBK_NUM; i++) {
		ret = urtwx_write_4_field(sc, w_q + 0x4, 0x00000004U, 0x0);
		if (ret != 0)
			return (ret);
		value = sc->sc_8822c_dack_msbk[path][1][i];
		ret = urtwx_write_4_field(sc, w_q + 0x4, 0x00000ff8U, value);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, w_q, 0xf0000000U, i);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_field(sc, w_q + 0x4, 0x00000004U, 0x1);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_4_field(sc, w_q + 0x4, 0x00000004U, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, w_i + 0x8,
	    (1U << 26) | (1U << 25), 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, w_q + 0x8,
	    (1U << 26) | (1U << 25), 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, w_i + 0x4, 0x00000001U, 0x0);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4_field(sc, w_q + 0x4, 0x00000001U, 0x0));
}

static int
urtwx_8822c_dac_cal_restore(struct urtwx_softc *sc)
{
	uint32_t temp[3];
	int ret;

	if (sc->sc_8822c_dack_msbk[0][0][0] == 0 &&
	    sc->sc_8822c_dack_msbk[0][1][0] == 0 &&
	    sc->sc_8822c_dack_msbk[1][0][0] == 0 &&
	    sc->sc_8822c_dack_msbk[1][1][0] == 0)
		return (ENOENT);

	temp[0] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x1860, 0);
	temp[1] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x4160, 0);
	temp[2] = urtwx_read_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0);

	urtwx_8822c_dac_cal_restore_prepare(sc);
	if (!urtwx_check_hw_ready(sc, 0x2808, 0x007fff80U, 0xffff) ||
	    !urtwx_check_hw_ready(sc, 0x2834, 0x007fff80U, 0xffff) ||
	    !urtwx_check_hw_ready(sc, 0x4508, 0x007fff80U, 0xffff) ||
	    !urtwx_check_hw_ready(sc, 0x4534, 0x007fff80U, 0xffff))
		return (ETIMEDOUT);

	ret = urtwx_8822c_dac_cal_restore_path(sc, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_dac_cal_restore_path(sc, 1);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_4_field(sc, 0x1830, 1U << 30, 0x1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x4130, 1U << 30, 0x1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1860, 0, temp[0]);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x4160, 0, temp[1]);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, 0x18b0, 1U << 27, 1U << 27);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, 0x18cc, 1U << 27, 1U << 27);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, 0x41b0, 1U << 27, 1U << 27);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, 0x41cc, 1U << 27, 1U << 27);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x09b4, 0, temp[2]));
}

static int
urtwx_8822c_rf_dac_cal(struct urtwx_softc *sc)
{
	struct urtwx_backup_info backup[URTWX_8822C_DACK_REGS];
	uint32_t backup_rf[URTWX_8822C_DACK_PATHS];
	uint32_t adc_ic_a, adc_qc_a, adc_ic_b, adc_qc_b;
	uint32_t ic_a, qc_a, ic_b, qc_b, ic, qc;
	uint32_t i_a, q_a, i_b, q_b;
	int i, ret;

	adc_ic_a = adc_qc_a = adc_ic_b = adc_qc_b = 0;
	ic_a = qc_a = ic_b = qc_b = 0;
	i_a = q_a = i_b = q_b = 0;
	sc->sc_usbctl_log = 0;
	printf("%s: 8822c dack begin rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
#if URTWX_8822C_ABORT_BEFORE_DACK
	urtwx_8822c_dump_dackstage(sc);
	return (EIO);
#endif

	ret = urtwx_8822c_dac_cal_restore(sc);
	if (ret == 0) {
		printf("%s: 8822c dack restore ok\n", DEVNAME(sc));
		return (0);
	}

	ret = urtwx_8822c_dac_backup_reg(sc, backup, backup_rf);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_dac_bb_setting(sc);
	if (ret != 0)
		return (ret);
#if URTWX_8822C_ABORT_AFTER_DACK_BB
	urtwx_8822c_dump_dackbbstage(sc);
	return (EIO);
#endif

	ret = urtwx_8822c_dac_cal_adc(sc, 0, &adc_ic_a, &adc_qc_a);
	if (ret != 0)
		return (ret);
	for (i = 0; i < URTWX_8822C_DACK_TRIES; i++) {
		ret = urtwx_8822c_dac_cal_step1(sc, 0,
		    (adc_ic_a & 0x3ffU) | ((adc_qc_a & 0x3ffU) << 10));
		if (ret != 0)
			return (ret);
		ret = urtwx_8822c_dac_cal_step2(sc, 0, &ic, &qc);
		if (ret != 0)
			return (ret);
		ic_a = ic;
		qc_a = qc;
		ret = urtwx_8822c_dac_cal_step3(sc, 0, adc_ic_a, adc_qc_a,
		    &ic, &qc, &i_a, &q_a);
		if (ret != 0)
			return (ret);
		if (ic < 5 && qc < 5)
			break;
	}
	ret = urtwx_8822c_dac_cal_step4(sc, 0);
	if (ret != 0)
		return (ret);

	ret = urtwx_8822c_dac_cal_adc(sc, 1, &adc_ic_b, &adc_qc_b);
	if (ret != 0)
		return (ret);
	for (i = 0; i < URTWX_8822C_DACK_TRIES; i++) {
		ret = urtwx_8822c_dac_cal_step1(sc, 1,
		    (adc_ic_b & 0x3ffU) | ((adc_qc_b & 0x3ffU) << 10));
		if (ret != 0)
			return (ret);
		ret = urtwx_8822c_dac_cal_step2(sc, 1, &ic, &qc);
		if (ret != 0)
			return (ret);
		ic_b = ic;
		qc_b = qc;
		ret = urtwx_8822c_dac_cal_step3(sc, 1, adc_ic_b, adc_qc_b,
		    &ic, &qc, &i_b, &q_b);
		if (ret != 0)
			return (ret);
		if (ic < 5 && qc < 5)
			break;
	}
	ret = urtwx_8822c_dac_cal_step4(sc, 1);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1b00, 0, 0x00000008);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_field(sc, 0x4130, 1U << 30, 0x1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, 0x1bcc, 0, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1b00, 0, 0x0000000a);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, 0x1bcc, 0, 0x0);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_dac_restore_reg(sc, backup, backup_rf);
	if (ret != 0)
		return (ret);
	urtwx_8822c_dac_cal_backup(sc);

	printf("%s: 8822c dack a(ic=%#x qc=%#x i=%#x q=%#x) "
	    "b(ic=%#x qc=%#x i=%#x q=%#x)\n", DEVNAME(sc), ic_a, qc_a,
	    i_a, q_a, ic_b, qc_b, i_b, q_b);
	return (0);
}

static int
urtwx_8822c_rf_x2_check(struct urtwx_softc *sc)
{
	uint32_t x2k_busy;
	int ret;

	delay(1000);
	x2k_busy = urtwx_read_rf_path(sc, 0, 0xb8) & (1U << 15);
	if (x2k_busy == 0)
		return (0);
	ret = urtwx_write_rf_path(sc, 0, 0xb8, RFREG_MASK, 0x0c4440);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf_path(sc, 0, 0xba, RFREG_MASK, 0x06840d);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf_path(sc, 0, 0xb8, RFREG_MASK, 0x080440);
	if (ret != 0)
		return (ret);
	delay(1000);
	return (0);
}

static int
urtwx_8822c_thermal_trim(struct urtwx_softc *sc)
{
	uint8_t path;
	int ret;

	for (path = 0; path < sc->sc_chip->uc_rf_path_count &&
	    path < nitems(sc->sc_8822c_thermal_trim); path++) {
		if (sc->sc_8822c_thermal_trim[path] == 0xff)
			continue;
		ret = urtwx_write_rf_path(sc, path, 0x43, URTWX_RF_THEMAL_MASK,
		    sc->sc_8822c_thermal_trim[path]);
		if (ret != 0)
			return (ret);
	}
	return (0);
}

static int
urtwx_8822c_power_trim(struct urtwx_softc *sc)
{
	static const uint8_t seq_to_idx[] = {
		0, 1, 2, 2, 3, 4, 5, 6, 7, 3, 4, 5, 6, 7, 7
	};
	uint8_t path, seq;
	int ret;

	if (!sc->sc_8822c_have_power_trim)
		return (urtwx_write_4_mask(sc, URTWX_8822C_REG_DIS_DPD,
		    URTWX_8822C_DIS_DPD_MASK, URTWX_8822C_DIS_DPD_RATEALL));

	for (path = 0; path < sc->sc_chip->uc_rf_path_count &&
	    path < nitems(sc->sc_8822c_power_trim); path++) {
		ret = urtwx_write_rf_path(sc, path, 0xee, 1U << 19, 0x1);
		if (ret != 0)
			return (ret);
		for (seq = 0; seq < nitems(seq_to_idx); seq++) {
			ret = urtwx_write_rf_path(sc, path, 0x33, RFREG_MASK,
			    seq);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_rf_path(sc, path, 0x3f, RFREG_MASK,
			    sc->sc_8822c_power_trim[path][seq_to_idx[seq]]);
			if (ret != 0)
				return (ret);
		}
		ret = urtwx_write_rf_path(sc, path, 0xee, 1U << 19, 0x0);
		if (ret != 0)
			return (ret);
	}
	return (urtwx_write_4_mask(sc, URTWX_8822C_REG_DIS_DPD,
	    URTWX_8822C_DIS_DPD_MASK, URTWX_8822C_DIS_DPD_RATEALL));
}

static int
urtwx_8822c_pa_bias(struct urtwx_softc *sc)
{
	uint8_t path;
	int ret;

	for (path = 0; path < sc->sc_chip->uc_rf_path_count &&
	    path < nitems(sc->sc_8822c_pa_bias_2g); path++) {
		if (sc->sc_8822c_pa_bias_2g[path] != 0xff) {
			ret = urtwx_write_rf_path(sc, path, URTWX_RF_PA,
			    URTWX_RF_PABIAS_2G_MASK,
			    sc->sc_8822c_pa_bias_2g[path]);
			if (ret != 0)
				return (ret);
		}
	}
	for (path = 0; path < sc->sc_chip->uc_rf_path_count &&
	    path < nitems(sc->sc_8822c_pa_bias_5g); path++) {
		if (sc->sc_8822c_pa_bias_5g[path] != 0xff) {
			ret = urtwx_write_rf_path(sc, path, URTWX_RF_PA,
			    URTWX_RF_PABIAS_5G_MASK,
			    sc->sc_8822c_pa_bias_5g[path]);
			if (ret != 0)
				return (ret);
		}
	}
	return (0);
}

static int
urtwx_8822c_rf_init(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_8822c_rf_dac_cal(sc);
	if (ret != 0) {
		printf("%s: 8822c dack failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_8822c_rf_x2_check(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_thermal_trim(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_power_trim(sc);
	if (ret != 0)
		return (ret);
	return (urtwx_8822c_pa_bias(sc));
}

static int
urtwx_8822c_phy_init(struct urtwx_softc *sc)
{
	int ret;

	/*
	 * Match the hardware-affecting subset of Linux's rtw_phy_init() /
	 * rtw8822c_adaptivity_init() before RF init. The software-only DM
	 * bookkeeping from that helper is not relevant to this bring-up path.
	 */
	ret = urtwx_write_4_mask(sc, REG_EDCCA_TH, 0x00ff0000U, 0x00ff0000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_EDCCA_TH, 0xff000000U, 0xff000000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_TX_PTCL_CTRL, BIT_DIS_EDCCA, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_RD_CTRL, BIT_EDCCA_MSK_CNTDOWN_EN,
	    BIT_EDCCA_MSK_CNTDOWN_EN);
	if (ret != 0)
		return (ret);
	return (urtwx_write_4_mask(sc, REG_EDCCA_DECISION,
	    URTWX_8822C_BIT_EDCCA_OPTION, 0));
}

static int
urtwx_8822c_config_trx_mode(struct urtwx_softc *sc, uint8_t tx_path,
    uint8_t rx_path)
{
	int ret;

	if ((tx_path | rx_path) & URTWX_8822C_BB_PATH_A)
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ORITXCODE,
		    0x000fffffU, 0x33312U);
	else
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ORITXCODE,
		    0x000fffffU, 0x11111U);
	if (ret != 0)
		return (ret);
	if ((tx_path | rx_path) & URTWX_8822C_BB_PATH_B)
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ORITXCODE2,
		    0x000fffffU, 0x33312U);
	else
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ORITXCODE2,
		    0x000fffffU, 0x11111U);
	if (ret != 0)
		return (ret);
	printf("%s: 8822c trx stage oritx rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));

	if (rx_path == URTWX_8822C_BB_PATH_A ||
	    rx_path == URTWX_8822C_BB_PATH_B) {
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_CCANRX,
		    0x00600000U, 0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_CCANRX,
		    0x00060000U, 0x0U);
		if (ret != 0)
			return (ret);
		if (rx_path == URTWX_8822C_BB_PATH_B)
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
			    0x0f000000U, 0x05000000U);
		else
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
			    0x0f000000U, 0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXFNCTL, 0x00000300U,
		    0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXFNCTL,
		    0x00600000U, 0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_AGCSWSH,
		    0x00020000U, 0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTWTPD,
		    0x00100000U, 0x0U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_MRCM, 0x01000000U,
		    0x0U);
		if (ret != 0)
			return (ret);
		if (rx_path == URTWX_8822C_BB_PATH_B) {
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
			    0x0f000000U, 0x02000000U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
			    0x000f0000U, 0x00020000U);
			if (ret != 0)
				return (ret);
		} else {
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
			    0x0f000000U, 0x01000000U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
			    0x000f0000U, 0x00010000U);
			if (ret != 0)
				return (ret);
		}
	} else if (rx_path == URTWX_8822C_BB_PATH_AB) {
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_CCANRX,
		    0x00600000U, 0x00200000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_CCANRX,
		    0x00060000U, 0x00020000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
		    0x0f000000U, 0x01000000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXFNCTL, 0x00000300U,
		    0x00000100U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXFNCTL,
		    0x00600000U, 0x00200000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_AGCSWSH,
		    0x00020000U, 0x00020000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTWTPD,
		    0x00100000U, 0x00100000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_MRCM, 0x01000000U,
		    0x01000000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
		    0x0f000000U, 0x03000000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP,
		    0x000f0000U, 0x00030000U);
		if (ret != 0)
			return (ret);
	}
	printf("%s: 8822c trx stage rx rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));

	switch (tx_path) {
	case URTWX_8822C_BB_PATH_A:
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
		    0xf0000000U, 0x80000000U);
		break;
	case URTWX_8822C_BB_PATH_B:
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
		    0xf0000000U, 0x40000000U);
		break;
	case URTWX_8822C_BB_PATH_AB:
	default:
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RXCCKSEL,
		    0xf0000000U, 0x80000000U);
		break;
	}
	if (ret != 0)
		return (ret);
	printf("%s: 8822c trx stage ccksel rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	urtwx_8822c_bb_reset(sc);
	printf("%s: 8822c trx stage ccktx rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));

	switch (tx_path) {
	case URTWX_8822C_BB_PATH_A:
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP0,
		    0x000000ffU, 0x11U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_TXLGMAP,
		    0x000000ffU, 0x00U);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_8822C_BB_PATH_B:
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP0,
		    0x000000ffU, 0x12U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_TXLGMAP,
		    0x000000ffU, 0x00U);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_8822C_BB_PATH_AB:
	default:
		/*
		 * Linux calls rtw8822c_config_tx_path(tx_path=AB,
		 * tx_path_sel_1ss=A, tx_path_cck=A, is_tx2_path=false)
		 * from rtw8822c_phy_set_param().  That selects the
		 * "1SS on path A" OFDM mapping even though TX is 2x2.
		 */
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_ANTMAP0, 0x000000ffU,
		    0x31U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_TXLGMAP, 0x0000ffffU,
		    0x0400U);
		if (ret != 0)
			return (ret);
		break;
	}
	urtwx_8822c_bb_reset(sc);
	urtwx_8822c_bb_reset(sc);
	printf("%s: 8822c trx stage ofdmtx rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));
	urtwx_8822c_toggle_igi(sc);
	printf("%s: 8822c trx stage igi rf0=%#x/%#x\n", DEVNAME(sc),
	    urtwx_read_rf_path(sc, 0, 0x0), urtwx_read_rf_path(sc, 1, 0x0));

	return (0);
}

static void
urtwx_update_htcaps(struct urtwx_softc *sc, struct ieee80211_channel *c)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint16_t htcaps;

	htcaps = URTWX_HTCAP_BASE;
	if (c != NULL && IEEE80211_CHAN_40MHZ_ALLOWED(c)) {
		htcaps |= IEEE80211_HTCAP_CBW20_40 |
		    IEEE80211_HTCAP_SGI40;
	}
	ic->ic_htcaps = htcaps;
}

static void
urtwx_get_chan_params(struct urtwx_softc *sc, struct ieee80211_channel *c,
    uint8_t *chanp, uint8_t *bwp, uint8_t *primaryp)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	uint8_t chan, sco, center;

	chan = ieee80211_chan2ieee(ic, c);
	*chanp = chan;
	*bwp = URTWX_CHANNEL_WIDTH_20;
	*primaryp = URTWX_SC_DONT_CARE;

	if (ic->ic_state == IEEE80211_S_SCAN || ni == NULL || ni->ni_chan != c)
		return;

	if (IEEE80211_CHAN_80MHZ_ALLOWED(c) &&
	    ieee80211_node_supports_vht_chan80(ni)) {
		center = ni->ni_vht_chan_center_freq_idx0;
		if (center != 0) {
			*chanp = center;
			*bwp = URTWX_CHANNEL_WIDTH_80;
			if (chan > center) {
				if (chan - center == 2)
					*primaryp = URTWX_SC_20_UPPER;
				else
					*primaryp = URTWX_SC_20_UPMOST;
			} else {
				if (center - chan == 2)
					*primaryp = URTWX_SC_20_LOWER;
				else
					*primaryp = URTWX_SC_20_LOWEST;
			}
			return;
		}
	}

	if (!IEEE80211_CHAN_40MHZ_ALLOWED(c) ||
	    !ieee80211_node_supports_ht_chan40(ni))
		return;

	sco = (ni->ni_htop0 & IEEE80211_HTOP0_SCO_MASK) >>
	    IEEE80211_HTOP0_SCO_SHIFT;
	switch (sco) {
	case IEEE80211_HTOP0_SCO_SCA:
		*chanp = chan + 2;
		*bwp = URTWX_CHANNEL_WIDTH_40;
		*primaryp = URTWX_SC_20_LOWER;
		break;
	case IEEE80211_HTOP0_SCO_SCB:
		if (chan > 2) {
			*chanp = chan - 2;
			*bwp = URTWX_CHANNEL_WIDTH_40;
			*primaryp = URTWX_SC_20_UPPER;
		}
		break;
	}
}

static int
urtwx_phy_set_channel_bb(struct urtwx_softc *sc, uint8_t channel, uint8_t bw,
    uint8_t primary_ch_idx)
{
	uint32_t val32;
	int ret;

	if (channel > 14 && channel < 36)
		return (EINVAL);

	if (sc->sc_chip == &urtwx_chip_8822c) {
		/* Follow Linux rtw8822c_set_channel_bb(). */
		if (channel <= 14) {
			ret = urtwx_write_4_mask(sc, 0x1a14, 0x00000300U, 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1a9c, 0x00100000U,
			    0x00100000U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_1_mask(sc, REG_CCK_CHECK,
			    BIT_CHECK_CCK_EN, 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1a80, 0x00040000U, 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x1c80, 0x3f000000U, 0x0f);
			if (ret != 0)
				return (ret);
			switch (bw) {
			case URTWX_CHANNEL_WIDTH_40:
				ret = urtwx_write_4_field(sc, 0x18ac,
				    0x0000f000U, 0x4);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x41ac,
				    0x0000f000U, 0x4);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x18ac,
				    0x000001f0U, 0x0);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x41ac,
				    0x000001f0U, 0x0);
				if (ret != 0)
					return (ret);
				break;
			case URTWX_CHANNEL_WIDTH_20:
			default:
				ret = urtwx_write_4_field(sc, 0x18ac,
				    0x0000f000U, 0x5);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x41ac,
				    0x0000f000U, 0x5);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x18ac,
				    0x000001f0U, 0x6);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x41ac,
				    0x000001f0U, 0x6);
				if (ret != 0)
					return (ret);
				break;
			}
			if (channel == 13 || channel == 14)
				val32 = 0x969;
			else if (channel == 11 || channel == 12)
				val32 = 0x96a;
			else
				val32 = 0x9aa;
			ret = urtwx_write_4_field(sc, 0x0c30, 0x00000fffU,
			    val32);
			if (ret != 0)
				return (ret);
			if (channel == 14) {
				ret = urtwx_write_4_field(sc, 0x1a20,
				    0xffff0000U, 0x3da0);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1a24,
				    0, 0x4962c931U);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a28,
				    0x0000ffffU, 0x6aa3);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a98,
				    0xffff0000U, 0xaa7b);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a9c,
				    0x0000ffffU, 0xf3d7);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1aa0,
				    0, 0x0);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1aac,
				    0, 0xff012455U);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1ab0,
				    0, 0x0000ffffU);
				if (ret != 0)
					return (ret);
			} else {
				ret = urtwx_write_4_field(sc, 0x1a20,
				    0xffff0000U, 0x5284);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1a24,
				    0, 0x3e18fec8U);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a28,
				    0x0000ffffU, 0x0a88);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a98,
				    0xffff0000U, 0xacc4);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4_field(sc, 0x1a9c,
				    0x0000ffffU, 0xc8b2);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1aa0,
				    0, 0x00faf0deU);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1aac,
				    0, 0x00122344U);
				if (ret != 0)
					return (ret);
				ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, 0x1ab0,
				    0, 0x0fffffffU);
				if (ret != 0)
					return (ret);
			}
			ret = urtwx_write_4_field(sc, 0x0808, 0x00000070U,
			    channel == 13 ? 0x3 : 0x1);
			if (ret != 0)
				return (ret);
		} else {
			ret = urtwx_write_4_mask(sc, 0x1a80, 0x00040000U,
			    0x00040000U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_1_mask(sc, REG_CCK_CHECK,
			    BIT_CHECK_CCK_EN, BIT_CHECK_CCK_EN);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1a14, 0x00000300U,
			    0x00000300U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1a9c, 0x00100000U, 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x1c80, 0x3f000000U, 0x22);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x0808, 0x00000070U, 0x3);
			if (ret != 0)
				return (ret);
			if ((channel >= 36 && channel <= 64)) {
				val32 = 0x1;
			} else if (channel >= 100 && channel <= 144) {
				val32 = 0x2;
			} else {
				val32 = 0x3;
			}
			ret = urtwx_write_4_field(sc, 0x18ac, 0x000001f0U,
			    val32);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x41ac, 0x000001f0U,
			    val32);
			if (ret != 0)
				return (ret);
			if (channel >= 36 && channel <= 51)
				val32 = 0x494;
			else if (channel >= 52 && channel <= 55)
				val32 = 0x493;
			else if (channel >= 56 && channel <= 111)
				val32 = 0x453;
			else if (channel >= 112 && channel <= 119)
				val32 = 0x452;
			else if (channel >= 120 && channel <= 172)
				val32 = 0x412;
			else
				val32 = 0x411;
			ret = urtwx_write_4_field(sc, 0x0c30, 0x00000fffU,
			    val32);
			if (ret != 0)
				return (ret);
		}

		switch (bw) {
		case URTWX_CHANNEL_WIDTH_40:
			ret = urtwx_write_4_mask(sc, 0x1a00, 0x10U,
			    primary_ch_idx == URTWX_SC_20_UPPER ? 0x10U : 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000000fU, 0x5);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x000000c0U, 0x0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000ff00U,
			    primary_ch_idx | (primary_ch_idx << 4));
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1abc, 0x40000000U,
			    0x40000000U);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x088c, 0x0000f000U, 0x1);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x0cbc, 0x00200000U,
			    0x00200000U);
			if (ret != 0)
				return (ret);
			break;
		case URTWX_CHANNEL_WIDTH_80:
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000000fU, 0xa);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x000000c0U, 0x0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000ff00U,
			    primary_ch_idx | (primary_ch_idx << 4));
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x088c, 0x0000f000U, 0x6);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x0cbc, 0x00200000U,
			    0x00200000U);
			if (ret != 0)
				return (ret);
			break;
		case URTWX_CHANNEL_WIDTH_20:
		default:
			ret = urtwx_write_4_field(sc, 0x0810, 0x00003ff0U,
			    0x19b);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000000fU, 0x0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b0, 0x0000ffc0U, 0x0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b4, 0x00000700U, 0x7);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x09b4, 0x00700000U, 0x6);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x1abc, 0x40000000U, 0);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_field(sc, 0x088c, 0x0000f000U, 0x1);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_4_mask(sc, 0x0cbc, 0x00200000U, 0);
			if (ret != 0)
				return (ret);
			break;
		}

		return (0);
	}

	if (channel <= 14) {
		ret = urtwx_write_4_mask(sc, REG_RXPSEL, 0x10000000U,
		    0x10000000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_CCK_CHECK,
		    BIT_CHECK_CCK_EN, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_ENTXCCK, 0x00040000U, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_RXCCAMSK, 0x0000fc00U,
		    15U << 10);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_TXSCALE_A, 0x00000f00U, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_CLKTRK, 0x1ffe0000U,
		    0x96aU << 17);
		if (ret != 0)
			return (ret);
	} else {
		ret = urtwx_write_4_mask(sc, REG_ENTXCCK, 0x00040000U,
		    0x00040000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_1_mask(sc, REG_CCK_CHECK,
		    BIT_CHECK_CCK_EN, BIT_CHECK_CCK_EN);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_RXPSEL, 0x10000000U, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_RXCCAMSK, 0x0000fc00U,
		    15U << 10);
		if (ret != 0)
			return (ret);

		if (channel >= 36 && channel <= 64)
			val32 = 1;
		else if (channel >= 100 && channel <= 144)
			val32 = 2;
		else
			val32 = 3;
		ret = urtwx_write_4_mask(sc, REG_TXSCALE_A, 0x00000f00U,
		    val32 << 8);
		if (ret != 0)
			return (ret);

		if (channel >= 36 && channel <= 48)
			val32 = 0x494U;
		else if (channel >= 52 && channel <= 64)
			val32 = 0x453U;
		else if (channel >= 100 && channel <= 116)
			val32 = 0x452U;
		else
			val32 = 0x412U;
		ret = urtwx_write_4_mask(sc, REG_CLKTRK, 0x1ffe0000U,
		    val32 << 17);
		if (ret != 0)
			return (ret);
	}

	switch (bw) {
	case URTWX_CHANNEL_WIDTH_80:
		val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0);
		val32 &= 0xfcffcf00U;
		val32 |= 0x40040000U |
		    ((primary_ch_idx & 0x0f) << 2) |
		    URTWX_CHANNEL_WIDTH_80;
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0, val32);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_CHANNEL_WIDTH_40:
		if (primary_ch_idx == URTWX_SC_20_UPPER)
			ret = urtwx_write_4_mask(sc, REG_RXSB, 0x00000010U,
			    0x00000010U);
		else
			ret = urtwx_write_4_mask(sc, REG_RXSB, 0x00000010U, 0);
		if (ret != 0)
			return (ret);

		val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0);
		val32 &= 0xff3ff300U;
		val32 |= 0x20020000U |
		    ((primary_ch_idx & 0x0f) << 2) |
		    URTWX_CHANNEL_WIDTH_40;
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0, val32);
		if (ret != 0)
			return (ret);
		break;
	case URTWX_CHANNEL_WIDTH_20:
	default:
		val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0);
		val32 &= 0xffcffc00U;
		val32 |= 0x10010000U;
		ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0, val32);
		if (ret != 0)
			return (ret);
		break;
	}

	return (urtwx_write_4_mask(sc, REG_ADC160, 0x40000000U,
	    0x40000000U));
}

static int
urtwx_phy_set_channel_mac(struct urtwx_softc *sc, uint8_t channel, uint8_t bw,
    uint8_t primary_ch_idx)
{
	uint8_t txsc40;
	uint32_t val32;
	int ret;

	txsc40 = 0;
	if (bw == URTWX_CHANNEL_WIDTH_80) {
		if (primary_ch_idx == URTWX_SC_20_UPPER ||
		    primary_ch_idx == URTWX_SC_20_UPMOST)
			txsc40 = URTWX_SC_40_UPPER;
		else
			txsc40 = URTWX_SC_40_LOWER;
	}
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_DATA_SC, 0,
	    BIT_TXSC_20M(primary_ch_idx) | BIT_TXSC_40M(txsc40));
	if (ret != 0)
		return (ret);
	if (bw == URTWX_CHANNEL_WIDTH_80)
		val32 = BIT_RFMOD_80M;
	else if (bw == URTWX_CHANNEL_WIDTH_40)
		val32 = BIT_RFMOD_40M;
	else
		val32 = 0;
	ret = urtwx_write_4_mask(sc, REG_WMAC_TRXPTCL_CTL, BIT_RFMOD, val32);
	if (ret != 0)
		return (ret);

	val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_AFE_CTRL1, 0);
	val32 &= ~BIT_MAC_CLK_SEL;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_AFE_CTRL1, 0, val32);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_USTIME_TSF, 0,
	    URTWX_MAC_CLK_SPEED);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_USTIME_EDCA, 0,
	    URTWX_MAC_CLK_SPEED);
	if (ret != 0)
		return (ret);

	if (channel <= 14)
		return (urtwx_write_1_mask(sc, REG_CCK_CHECK,
		    BIT_CHECK_CCK_EN, 0));

	return (urtwx_write_1_mask(sc, REG_CCK_CHECK, BIT_CHECK_CCK_EN,
	    BIT_CHECK_CCK_EN));
}

static int
urtwx_phy_set_channel_rf(struct urtwx_softc *sc, uint8_t channel, uint8_t bw)
{
	const struct urtwx_chip_info *chip = sc->sc_chip;
	uint32_t rf18;
	uint32_t rf_rxbb;
	uint8_t path;
	int ret;

	if (sc->sc_chip == &urtwx_chip_8822c)
		rf18 = 0;
	else
		rf18 = urtwx_read_rf_path(sc, 0, 0x18);
	rf18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
	    RF18_BW_MASK);
	rf18 |= (channel & RF18_CHANNEL_MASK);
	if (bw == URTWX_CHANNEL_WIDTH_80)
		rf18 |= RF18_BW_80M;
	else if (bw == URTWX_CHANNEL_WIDTH_40)
		rf18 |= RF18_BW_40M;
	else
		rf18 |= RF18_BW_20M;
	if (channel <= 14)
		rf18 |= RF18_BAND_2G;
	else
		rf18 |= RF18_BAND_5G;
	if (channel >= 100 && channel <= 140)
		rf18 |= RF18_RFSI_GE;
	else if (channel > 140)
		rf18 |= RF18_RFSI_GT;
	sc->sc_rf18_want = rf18;

	if (channel <= 14) {
		if (sc->sc_rfe_btg)
			ret = urtwx_phy_switch_rf_set(sc, URTWX_SWITCH_TO_BTG);
		else
			ret = urtwx_phy_switch_rf_set(sc, URTWX_SWITCH_TO_WLG);
	} else
		ret = urtwx_phy_switch_rf_set(sc, URTWX_SWITCH_TO_WLA);
	if (ret != 0)
		return (ret);

	if (sc->sc_chip == &urtwx_chip_8822c) {
		uint16_t reg;

		if (bw == URTWX_CHANNEL_WIDTH_80)
			rf_rxbb = 0x8;
		else if (bw == URTWX_CHANNEL_WIDTH_40)
			rf_rxbb = 0x10;
		else
			rf_rxbb = 0x18;

		if (sc->sc_rfset_log < 8) {
			printf("%s: 8822c setchan pre ch %u rstb=%#x 3wirea=%#x "
			    "3wireb=%#x\n", DEVNAME(sc), channel,
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_RSTB, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_3WIRE, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_3WIRE2, 0));
			sc->sc_rfset_log++;
		}
		ret = urtwx_8822c_rstb_3wire(sc, 0);
		if (ret != 0)
			return (ret);
		for (path = 0; path < chip->uc_rf_path_count; path++) {
			reg = sc->sc_chip->uc_rf_write_base[path] +
			    ((uint16_t)0xee << 2);
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, 0x4);
			if (ret != 0)
				return (ret);
			delay(URTWX_8822C_RF_DELAY_US);
				if (sc->sc_scan_log < 4)
					printf("%s: 8822c setchan ch %u path %u rf0xee=0x1 "
					    "peek %#x\n", DEVNAME(sc), channel, path,
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    sc->sc_chip->uc_rf_write_base[path] +
				    ((uint16_t)0xee << 2), 0) & RFREG_MASK);
			reg = sc->sc_chip->uc_rf_write_base[path] +
			    ((uint16_t)0x33 << 2);
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, 0x12);
			if (ret != 0)
				return (ret);
			delay(URTWX_8822C_RF_DELAY_US);
				if (sc->sc_scan_log < 4)
					printf("%s: 8822c setchan ch %u path %u rf0x33=0x12 "
					    "peek %#x\n", DEVNAME(sc), channel, path,
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    sc->sc_chip->uc_rf_write_base[path] +
				    ((uint16_t)0x33 << 2), 0) & RFREG_MASK);
			reg = sc->sc_chip->uc_rf_write_base[path] +
			    ((uint16_t)0x3f << 2);
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0,
			    rf_rxbb);
			if (ret != 0)
				return (ret);
			delay(URTWX_8822C_RF_DELAY_US);
				if (sc->sc_scan_log < 4)
					printf("%s: 8822c setchan ch %u path %u rf0x3f=%#x "
					    "peek %#x\n", DEVNAME(sc), channel, path, rf_rxbb,
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    sc->sc_chip->uc_rf_write_base[path] +
				    ((uint16_t)0x3f << 2), 0) & RFREG_MASK);
			reg = sc->sc_chip->uc_rf_write_base[path] +
			    ((uint16_t)0xee << 2);
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, 0);
			if (ret != 0)
				return (ret);
			delay(URTWX_8822C_RF_DELAY_US);
				if (sc->sc_scan_log < 4)
					printf("%s: 8822c setchan ch %u path %u rf0xee=0 "
					    "peek %#x\n", DEVNAME(sc), channel, path,
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    sc->sc_chip->uc_rf_write_base[path] +
				    ((uint16_t)0xee << 2), 0) & RFREG_MASK);
		}
		for (path = 0; path < chip->uc_rf_path_count; path++) {
			reg = sc->sc_chip->uc_rf_write_base[path] +
			    ((uint16_t)0x18 << 2);
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, reg, 0, rf18);
			if (ret != 0)
				return (ret);
			delay(1);
				if (sc->sc_scan_log < 4)
					printf("%s: 8822c setchan ch %u path %u rf0x18=%#x "
					    "peek %#x\n", DEVNAME(sc), channel, path, rf18,
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    sc->sc_chip->uc_rf_write_base[path] +
				    ((uint16_t)0x18 << 2), 0) & RFREG_MASK);
		}
		ret = urtwx_8822c_rstb_3wire(sc, 1);
		if (ret != 0)
			return (ret);
		if (sc->sc_rfset_log < 16) {
			printf("%s: 8822c setchan post ch %u rstb=%#x 3wirea=%#x "
			    "3wireb=%#x\n", DEVNAME(sc), channel,
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_RSTB, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_3WIRE, 0),
			    urtwx_read_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_3WIRE2, 0));
			sc->sc_rfset_log++;
		}
		return (0);
	}

	for (path = 0; path < chip->uc_rf_path_count; path++) {
		if (channel <= 14) {
			ret = urtwx_write_rf_path(sc, path, RF_LUTDBG, 0x40, 1);
			if (ret != 0)
				return (ret);
			ret = urtwx_write_rf_path(sc, path, 0x64, 0x0f, 0x0f);
			if (ret != 0)
				return (ret);
		} else {
			ret = urtwx_write_rf_path(sc, path, RF_LUTDBG, 0x40, 0);
			if (ret != 0)
				return (ret);
		}
		ret = urtwx_write_rf_path(sc, path, 0x18, RFREG_MASK, rf18);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_rf_path(sc, path, RF_XTALX2, 0x00080000U, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_rf_path(sc, path, RF_XTALX2, 0x00080000U, 1);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
urtwx_phy_set_channel_rxdfir(struct urtwx_softc *sc, uint8_t bw)
{
	int ret;

	ret = urtwx_write_4_mask(sc, REG_ACBB0, 0x30000000U, 0x20000000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_ACBBRXFIR, 0x30000000U,
	    0x20000000U);
	if (ret != 0)
		return (ret);
	if (bw == URTWX_CHANNEL_WIDTH_80) {
		ret = urtwx_write_4_mask(sc, REG_ACBBRXFIR, 0x30000000U,
		    0x10000000U);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, REG_TXDFIR, 0x80000000U, 0);
		if (ret != 0)
			return (ret);
		return (urtwx_write_4_mask(sc, REG_CHFIR, 0x80000000U,
		    0x80000000U));
	}
	if (bw == URTWX_CHANNEL_WIDTH_40) {
		ret = urtwx_write_4_mask(sc, REG_TXDFIR, 0x80000000U, 0);
		if (ret != 0)
			return (ret);
		return (urtwx_write_4_mask(sc, REG_CHFIR, 0x80000000U, 0));
	}

	ret = urtwx_write_4_mask(sc, REG_TXDFIR, 0x80000000U, 0x80000000U);
	if (ret != 0)
		return (ret);

	return (urtwx_write_4_mask(sc, REG_CHFIR, 0x80000000U, 0));
}

static int
urtwx_8822c_set_tx_power_default(struct urtwx_softc *sc, uint8_t channel,
    uint8_t bw)
{
	static const uint16_t cck_ref_reg[2] = {
		URTWX_8822C_REG_TXPWR_REF_A_CCK,
		URTWX_8822C_REG_TXPWR_REF_B_CCK
	};
	static const uint16_t ofdm_ref_reg[2] = {
		URTWX_8822C_REG_TXPWR_REF_A_OFDM,
		URTWX_8822C_REG_TXPWR_REF_B_OFDM
	};
	uint8_t pwr_ref_cck[2], pwr_ref_ofdm[2];
	int8_t diff_idx[4];
	uint8_t txpwr[2][URTWX_DESC_RATEVHT2SS_MCS9 + 1];
	uint32_t cck_ref, ofdm_ref, phy_pwr_idx;
	uint16_t ns;
	uint8_t group, pwr_idx[4];
	int8_t diff_a, diff_b;
	int path, rate, i;
	int ret;

	static const uint8_t cck_rates[] = {
		URTWX_DESC_RATE1M, 0x01, 0x02, URTWX_DESC_RATE11M
	};
	static const uint8_t ofdm_rates[] = {
		URTWX_DESC_RATE6M, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, URTWX_DESC_RATE54M
	};
	static const uint8_t ht1_rates[] = {
		0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, URTWX_DESC_RATEMCS7
	};
	static const uint8_t ht2_rates[] = {
		0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, URTWX_DESC_RATEMCS15
	};
	static const uint8_t vht1_rates[] = {
		0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34,
		URTWX_DESC_RATEVHT1SS_MCS9
	};
	static const uint8_t vht2_rates[] = {
		0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
		URTWX_DESC_RATEVHT2SS_MCS9
	};

#define URTWX_8822C_NIBBLE(_v, _shift) \
	((((int8_t)(((_v) >> (_shift)) & 0x0f)) & 0x08) ? \
	    (int8_t)((((_v) >> (_shift)) & 0x0f) - 16) : \
	    (int8_t)(((_v) >> (_shift)) & 0x0f))

	if (!sc->sc_8822c_have_txpwr_idx) {
		const uint32_t cck_ref_default =
		    ((uint32_t)URTWX_8822C_TXPWR_REF_DEFAULT << 16) &
		    0x007f0000U;
		const uint32_t ofdm_ref_default =
		    ((uint32_t)URTWX_8822C_TXPWR_REF_DEFAULT << 10) &
		    0x0001fc00U;

		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
		    URTWX_8822C_BIT_TXAGC_RSTB, 0);
		if (ret != 0)
			return (ret);
		for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
			ret = urtwx_write_4_mask(sc, cck_ref_reg[path],
			    0x007f0000U, cck_ref_default);
			if (ret != 0)
				return (ret);
		}
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
		    URTWX_8822C_BIT_TXAGC_RSTB, 0);
		if (ret != 0)
			return (ret);
		for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
			ret = urtwx_write_4_mask(sc, ofdm_ref_reg[path],
			    0x0001fc00U, ofdm_ref_default);
			if (ret != 0)
				return (ret);
		}
		for (rate = 0; rate <= URTWX_DESC_RATEVHT2SS_MCS9; rate += 4) {
			ret = urtwx_write_4(sc, RTW_USB_CMD_REQ,
			    URTWX_8822C_REG_TXAGC_BASE + rate, 0, 0);
			if (ret != 0)
				return (ret);
		}
		return (urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
		    URTWX_8822C_BIT_TXAGC_RSTB,
		    URTWX_8822C_BIT_TXAGC_RSTB));
	}

	group = 0;
	switch (channel) {
	case 1:
	case 2:
	case 36:
	case 38:
	case 40:
	case 42:
		group = 0;
		break;
	case 3:
	case 4:
	case 5:
	case 44:
	case 46:
	case 48:
	case 50:
		group = 1;
		break;
	case 6:
	case 7:
	case 8:
	case 52:
	case 54:
	case 56:
	case 58:
		group = 2;
		break;
	case 9:
	case 10:
	case 11:
	case 60:
	case 62:
	case 64:
		group = 3;
		break;
	case 12:
	case 13:
	case 100:
	case 102:
	case 104:
	case 106:
		group = 4;
		break;
	case 14:
		group = 4;
		break;
	case 108:
	case 110:
	case 112:
	case 114:
		group = 5;
		break;
	case 116:
	case 118:
	case 120:
	case 122:
		group = 6;
		break;
	case 124:
	case 126:
	case 128:
	case 130:
		group = 7;
		break;
	case 132:
	case 134:
	case 136:
	case 138:
		group = 8;
		break;
	case 140:
	case 142:
	case 144:
		group = 9;
		break;
	case 149:
	case 151:
	case 153:
	case 155:
		group = 10;
		break;
	case 157:
	case 159:
	case 161:
		group = 11;
		break;
	case 165:
	case 167:
	case 169:
	case 171:
		group = 12;
		break;
	case 173:
	case 175:
	case 177:
		group = 13;
		break;
	}

	memset(txpwr, 0, sizeof(txpwr));
	for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
		const struct urtwx_8822c_txpwr_idx *idx =
		    &sc->sc_8822c_txpwr_idx[path];
		int8_t ofdm1, bw20_1, bw20_2, bw40_2, bw80_1, bw80_2;
		uint8_t base_cck, base40, base80_lo, base80_hi;

		if (channel <= 14) {
			base_cck = idx->pwr_idx_2g.cck_base[
			    (channel == 14) ? 5 : group];
			base40 = idx->pwr_idx_2g.bw40_base[group];
			ofdm1 = URTWX_8822C_NIBBLE(idx->pwr_idx_2g.ht_1s_diff.val,
			    0);
			bw20_1 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_2g.ht_1s_diff.val, 4);
			ns = le16toh(idx->pwr_idx_2g.ht_2s_diff.val);
			bw20_2 = URTWX_8822C_NIBBLE(ns, 0);
			bw40_2 = URTWX_8822C_NIBBLE(ns, 4);

			for (i = 0; i < nitems(cck_rates); i++)
				txpwr[path][cck_rates[i]] = base_cck;
			for (i = 0; i < nitems(ofdm_rates); i++)
				txpwr[path][ofdm_rates[i]] = base40 + ofdm1 * 2;
			for (i = 0; i < nitems(ht1_rates); i++) {
				txpwr[path][ht1_rates[i]] = base40 +
				    ((bw == URTWX_CHANNEL_WIDTH_20) ?
				    bw20_1 * 2 : 0);
			}
			for (i = 0; i < nitems(ht2_rates); i++) {
				txpwr[path][ht2_rates[i]] = base40 +
				    ((bw == URTWX_CHANNEL_WIDTH_20) ?
				    (bw20_1 + bw20_2) * 2 : bw40_2 * 2);
			}
			for (i = 0; i < nitems(vht1_rates); i++)
				txpwr[path][vht1_rates[i]] =
				    txpwr[path][ht1_rates[i < nitems(ht1_rates) ?
				    i : nitems(ht1_rates) - 1]];
			for (i = 0; i < nitems(vht2_rates); i++)
				txpwr[path][vht2_rates[i]] =
				    txpwr[path][ht2_rates[i < nitems(ht2_rates) ?
				    i : nitems(ht2_rates) - 1]];
		} else {
			base40 = idx->pwr_idx_5g.bw40_base[group];
			base80_lo = idx->pwr_idx_5g.bw40_base[group];
			base80_hi = idx->pwr_idx_5g.bw40_base[
			    (group < 13) ? group + 1 : group];
			ofdm1 = URTWX_8822C_NIBBLE(idx->pwr_idx_5g.ht_1s_diff.val,
			    0);
			bw20_1 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_5g.ht_1s_diff.val, 4);
			bw20_2 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_5g.ht_2s_diff.val, 0);
			bw40_2 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_5g.ht_2s_diff.val, 4);
			bw80_1 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_5g.vht_1s_diff.val, 4);
			bw80_2 = URTWX_8822C_NIBBLE(
			    idx->pwr_idx_5g.vht_2s_diff.val, 4);

			for (i = 0; i < nitems(ofdm_rates); i++)
				txpwr[path][ofdm_rates[i]] = base40 + ofdm1 * 2;
			for (i = 0; i < nitems(ht1_rates); i++) {
				if (bw == URTWX_CHANNEL_WIDTH_80)
					txpwr[path][ht1_rates[i]] =
					    ((base80_lo + base80_hi) / 2) +
					    bw80_1 * 2;
				else
					txpwr[path][ht1_rates[i]] = base40 +
					    ((bw == URTWX_CHANNEL_WIDTH_20) ?
					    bw20_1 * 2 : 0);
			}
			for (i = 0; i < nitems(ht2_rates); i++) {
				if (bw == URTWX_CHANNEL_WIDTH_80)
					txpwr[path][ht2_rates[i]] =
					    ((base80_lo + base80_hi) / 2) +
					    (bw80_1 + bw80_2) * 2;
				else
					txpwr[path][ht2_rates[i]] = base40 +
					    ((bw == URTWX_CHANNEL_WIDTH_20) ?
					    (bw20_1 + bw20_2) * 2 :
					    bw40_2 * 2);
			}
			for (i = 0; i < nitems(vht1_rates); i++) {
				if (bw == URTWX_CHANNEL_WIDTH_80)
					txpwr[path][vht1_rates[i]] =
					    ((base80_lo + base80_hi) / 2) +
					    bw80_1 * 2;
				else
					txpwr[path][vht1_rates[i]] =
					    txpwr[path][ht1_rates[
					    i < nitems(ht1_rates) ? i :
					    nitems(ht1_rates) - 1]];
			}
			for (i = 0; i < nitems(vht2_rates); i++) {
				if (bw == URTWX_CHANNEL_WIDTH_80)
					txpwr[path][vht2_rates[i]] =
					    ((base80_lo + base80_hi) / 2) +
					    (bw80_1 + bw80_2) * 2;
				else
					txpwr[path][vht2_rates[i]] =
					    txpwr[path][ht2_rates[
					    i < nitems(ht2_rates) ? i :
					    nitems(ht2_rates) - 1]];
			}
		}

		pwr_ref_cck[path] = txpwr[path][URTWX_DESC_RATE11M];
		pwr_ref_ofdm[path] = txpwr[path][URTWX_DESC_RATEMCS7];
	}

	if (sc->sc_8822c_have_bb_pg) {
		for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
			const int8_t *ofs = (channel <= 14) ?
			    sc->sc_8822c_txpwr_ofs_2g[path] :
			    sc->sc_8822c_txpwr_ofs_5g[path];

			for (rate = 0; rate <= URTWX_DESC_RATEVHT2SS_MCS9;
			    rate++) {
				int pwr;

				pwr = (int)txpwr[path][rate] + ofs[rate];
				if (pwr < 0)
					pwr = 0;
				else if (pwr > 0x7f)
					pwr = 0x7f;
				txpwr[path][rate] = pwr;
			}

			pwr_ref_cck[path] = txpwr[path][URTWX_DESC_RATE11M];
			pwr_ref_ofdm[path] = txpwr[path][URTWX_DESC_RATEMCS7];
		}
	}

	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
	    URTWX_8822C_BIT_TXAGC_RSTB, 0);
	if (ret != 0)
		return (ret);
	for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
		cck_ref = ((uint32_t)pwr_ref_cck[path] << 16) & 0x007f0000U;
		ret = urtwx_write_4_mask(sc, cck_ref_reg[path], 0x007f0000U,
		    cck_ref);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
	    URTWX_8822C_BIT_TXAGC_RSTB, 0);
	if (ret != 0)
		return (ret);
	for (path = 0; path < sc->sc_chip->uc_rf_path_count; path++) {
		ofdm_ref = ((uint32_t)pwr_ref_ofdm[path] << 10) & 0x0001fc00U;
		ret = urtwx_write_4_mask(sc, ofdm_ref_reg[path], 0x0001fc00U,
		    ofdm_ref);
		if (ret != 0)
			return (ret);
	}

	for (rate = 0; rate <= URTWX_DESC_RATEVHT2SS_MCS9; rate += 4) {
		for (i = 0; i < 4; i++) {
			diff_a = (int8_t)txpwr[0][rate + i] - (int8_t)
			    ((rate + i) <= URTWX_DESC_RATE11M ?
			    pwr_ref_cck[0] : pwr_ref_ofdm[0]);
			diff_b = diff_a;
			if (sc->sc_chip->uc_rf_path_count > 1)
				diff_b = (int8_t)txpwr[1][rate + i] - (int8_t)
				    ((rate + i) <= URTWX_DESC_RATE11M ?
				    pwr_ref_cck[1] : pwr_ref_ofdm[1]);
			diff_idx[i] = min(diff_a, diff_b);
			pwr_idx[i] = (uint8_t)diff_idx[i] & 0x7f;
		}
		phy_pwr_idx = pwr_idx[0] |
		    ((uint32_t)pwr_idx[1] << 8) |
		    ((uint32_t)pwr_idx[2] << 16) |
		    ((uint32_t)pwr_idx[3] << 24);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
		    URTWX_8822C_BIT_TXAGC_RSTB, 0);
		if (ret != 0)
			return (ret);
		ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_TXAGC_BASE + rate,
		    0xffffffffU, phy_pwr_idx);
		if (ret != 0)
			return (ret);
	}

#undef URTWX_8822C_NIBBLE
	return (urtwx_write_4_mask(sc, URTWX_8822C_REG_RSTB,
	    URTWX_8822C_BIT_TXAGC_RSTB,
	    URTWX_8822C_BIT_TXAGC_RSTB));
}

static void
urtwx_8822c_dump_chan_regs(struct urtwx_softc *sc, uint8_t channel)
{
	uint32_t sys, rfctrl, wlrf1, wirea, wireb, rstb;
	uint32_t antmap0, antmap, mrcm, agcswsh, antwtpd;
	uint32_t rxccksel, ccanrx, encck, rxfnctl, rxigi, txlgmap;
	uint32_t refacck, refaofdm, refbcck, refbofdm;
	uint32_t agc0, agc1, agc2, agc3;

	sys = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_FUNC_EN, 0);
	rfctrl = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_RF_CTRL, 0);
	wlrf1 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_WLRF1, 0);
	wirea = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE, 0);
	wireb = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_3WIRE2, 0);
	rstb = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RSTB, 0);
	antmap0 = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ANTMAP0, 0);
	antmap = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ANTMAP, 0);
	mrcm = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_MRCM, 0);
	agcswsh = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_AGCSWSH, 0);
	antwtpd = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ANTWTPD, 0);
	rxccksel = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXCCKSEL, 0);
	ccanrx = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_CCANRX, 0);
	encck = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ENCCK, 0);
	rxfnctl = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXFNCTL, 0);
	rxigi = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXIGI, 0);
	txlgmap = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXLGMAP, 0);
	refacck = urtwx_read_4(sc, RTW_USB_CMD_REQ,
	    URTWX_8822C_REG_TXPWR_REF_A_CCK, 0);
	refaofdm = urtwx_read_4(sc, RTW_USB_CMD_REQ,
	    URTWX_8822C_REG_TXPWR_REF_A_OFDM, 0);
	refbcck = urtwx_read_4(sc, RTW_USB_CMD_REQ,
	    URTWX_8822C_REG_TXPWR_REF_B_CCK, 0);
	refbofdm = urtwx_read_4(sc, RTW_USB_CMD_REQ,
	    URTWX_8822C_REG_TXPWR_REF_B_OFDM, 0);
	agc0 = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXAGC_BASE + 0,
	    0);
	agc1 = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXAGC_BASE + 4,
	    0);
	agc2 = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXAGC_BASE + 8,
	    0);
	agc3 = urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXAGC_BASE + 12,
	    0);

	printf("%s: 8822c ch%u regs sys=%#x rfctrl=%#x wlrf1=%#x "
	    "3wirea=%#x 3wireb=%#x rstb=%#x\n",
	    DEVNAME(sc), channel, sys, rfctrl, wlrf1, wirea, wireb, rstb);
	printf("%s: 8822c ch%u ant=%#x/%#x mrcm=%#x agcswsh=%#x antwtpd=%#x "
	    "rxccksel=%#x ccanrx=%#x encck=%#x\n",
	    DEVNAME(sc), channel, antmap0, antmap, mrcm, agcswsh, antwtpd,
	    rxccksel, ccanrx, encck);
	printf("%s: 8822c ch%u rxfnctl=%#x rxigi=%#x txlgmap=%#x "
	    "pwrref a=%#x/%#x b=%#x/%#x agc=%#x/%#x/%#x/%#x\n",
	    DEVNAME(sc), channel, rxfnctl, rxigi, txlgmap, refacck, refaofdm,
	    refbcck, refbofdm, agc0, agc1, agc2, agc3);
}

static void
urtwx_8822c_log_usb_mode(struct urtwx_softc *sc)
{
	uint8_t syscfg2_3;
	uint32_t pad_ctrl2;
	int cur_u3, can_switch, already_u3;

	syscfg2_3 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_SYS_CFG2 + 3, 0);
	pad_ctrl2 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_PAD_CTRL2, 0);
	cur_u3 = (syscfg2_3 == 0x20);
	can_switch = (pad_ctrl2 & (BIT_MASK_USB23_SW_MODE_V1 |
	    BIT_USB3_USB2_TRANSITION)) != 0;
	already_u3 = ((pad_ctrl2 & BIT_MASK_USB23_SW_MODE_V1) >> 18) ==
	    BIT_USB_MODE_U3;

	printf("%s: 8822c usb syscfg2+3=%#x pad_ctrl2=%#x efuse_switch=%u "
	    "cur_u3=%d can_switch=%d already_u3=%d\n",
	    DEVNAME(sc), syscfg2_3, pad_ctrl2, sc->sc_usb_mode_switch,
	    cur_u3, can_switch, already_u3);
}

static void
urtwx_8822c_dump_hw_feature(struct urtwx_softc *sc)
{
	uint8_t id, feat[URTWX_HW_FEATURE_LEN];
	char hex[URTWX_HW_FEATURE_LEN * 3];
	int i;

	id = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_C2HEVT, 0);
	if (id != URTWX_C2H_HW_FEATURE_REPORT) {
		printf("%s: 8822c hw feature id %#x\n", DEVNAME(sc), id);
		return;
	}

	for (i = 0; i < URTWX_HW_FEATURE_LEN; i++)
		feat[i] = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_C2HEVT + 2 + i,
		    0);
	urtwx_hexbuf(feat, sizeof(feat), hex, sizeof(hex));
	printf("%s: 8822c hw feature %s\n", DEVNAME(sc), hex);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_C2HEVT, 0, 0);
}

static int
urtwx_phy_init_runtime(struct urtwx_softc *sc)
{
	int ret;

	memset(sc->sc_rf_cache_valid, 0, sizeof(sc->sc_rf_cache_valid));
	if (sc->sc_chip == &urtwx_chip_8822c)
		sc->sc_usbctl_log = 0;

	ret = urtwx_phy_enable_bb_rf(sc);
	if (ret != 0)
		return (ret);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		sc->sc_usbctl_log = 0;
#if URTWX_8822C_FORCE_TRX_B_ONLY
		printf("%s: 8822c diag forcing trx B/B\n", DEVNAME(sc));
		ret = urtwx_8822c_config_trx_mode(sc, URTWX_8822C_BB_PATH_B,
		    URTWX_8822C_BB_PATH_B);
#else
		ret = urtwx_8822c_config_trx_mode(sc, URTWX_8822C_BB_PATH_AB,
		    URTWX_8822C_BB_PATH_AB);
#endif
		if (ret != 0)
			return (ret);
#if URTWX_8822C_ABORT_AFTER_CONFIG_TRX
		printf("%s: 8822c trx init antmap0=%#x antmap=%#x "
		    "rxccksel=%#x ccanrx=%#x txlgmap=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ANTMAP0, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ANTMAP, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXCCKSEL, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_CCANRX, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_TXLGMAP, 0),
		    urtwx_read_rf_path(sc, 0, 0x0),
		    urtwx_read_rf_path(sc, 1, 0x0));
		return (EIO);
#endif
		sc->sc_usbctl_log = 0;
		ret = urtwx_8822c_phy_init(sc);
		if (ret != 0)
			return (ret);
#if URTWX_8822C_ABORT_AFTER_PHYINIT
		printf("%s: 8822c phy init edcca_th=%#x edcca_dec=%#x "
		    "rd_ctrl=%#x txptcl=%#x rf0=%#x/%#x\n",
		    DEVNAME(sc),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_EDCCA_TH, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_EDCCA_DECISION, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RD_CTRL, 0),
		    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_TX_PTCL_CTRL, 0),
		    urtwx_read_rf_path(sc, 0, 0x0),
		    urtwx_read_rf_path(sc, 1, 0x0));
		return (EIO);
#endif
#if URTWX_8822C_ABORT_AFTER_TRXMODE
		urtwx_8822c_dump_trxstage(sc);
		return (EIO);
#endif
		ret = urtwx_8822c_rf_init(sc);
		if (ret != 0)
			return (ret);
	}
	ret = urtwx_phy_set_channel_bb(sc, URTWX_FIXED_CHANNEL,
	    URTWX_CHANNEL_WIDTH_20, URTWX_SC_DONT_CARE);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_mac(sc, URTWX_FIXED_CHANNEL,
	    URTWX_CHANNEL_WIDTH_20, URTWX_SC_DONT_CARE);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_rf(sc, URTWX_FIXED_CHANNEL,
	    URTWX_CHANNEL_WIDTH_20);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_rxdfir(sc, URTWX_CHANNEL_WIDTH_20);
	if (ret != 0)
		return (ret);
	if (sc->sc_chip == &urtwx_chip_8822c)
		return (urtwx_8822c_set_tx_power_default(sc,
		    URTWX_FIXED_CHANNEL, URTWX_CHANNEL_WIDTH_20));
	return (0);
}

static int
urtwx_set_chan(struct urtwx_softc *sc, struct ieee80211_channel *c)
{
	uint8_t chan, bw, primary_ch_idx;
	int ret;

	if (c == NULL ||
	    (!IEEE80211_IS_CHAN_2GHZ(c) && !IEEE80211_IS_CHAN_5GHZ(c)))
		return (ENOTSUP);

	urtwx_update_htcaps(sc, c);
	urtwx_get_chan_params(sc, c, &chan, &bw, &primary_ch_idx);
	if (chan == 0 || (chan > 14 && chan < 36))
		return (EINVAL);

	ret = urtwx_phy_set_channel_bb(sc, chan, bw, primary_ch_idx);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_mac(sc, chan, bw, primary_ch_idx);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_rf(sc, chan, bw);
	if (ret != 0)
		return (ret);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		urtwx_8822c_toggle_igi(sc);
		return (0);
	}
	ret = urtwx_phy_set_channel_rxdfir(sc, bw);
	if (ret != 0)
		return (ret);
	return (0);
}

static int
urtwx_set_macaddr(struct urtwx_softc *sc, const uint8_t *addr)
{
	uint32_t addr0;
	uint16_t addr1;
	int ret;

	addr0 = addr[0] |
	    ((uint32_t)addr[1] << 8) |
	    ((uint32_t)addr[2] << 16) |
	    ((uint32_t)addr[3] << 24);
	addr1 = addr[4] | ((uint16_t)addr[5] << 8);

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_MACID, 0, addr0);
	if (ret != 0)
		return (ret);

	return (urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_MACID + 4, 0, addr1));
}

static void
urtwx_sta_leave_run(struct urtwx_softc *sc)
{
	(void)urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_ASSOCIATED_BFMER0_INFO,
	    0, 0);
	(void)urtwx_write_4_mask(sc, REG_CR, BIT_PORT0_NET_TYPE, 0);
	(void)urtwx_write_4_mask(sc, REG_RCR,
	    BIT_CBSSID_DATA | BIT_CBSSID_BCN, 0);
	(void)urtwx_write_1_mask(sc, REG_BCN_CTRL, BIT_DIS_TSF_UDT,
	    BIT_DIS_TSF_UDT);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_DUAL_TSF_RST, 0, 0x03);
}

static int
urtwx_sta_enter_run(struct urtwx_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni = ic->ic_bss;
	uint16_t aid;
	uint16_t bssid1, intval;
	uint32_t bssid0;
	uint32_t ra_mask;
	uint8_t rate;
	uint8_t rate_id, desc_rate, sgi, ldpc;
	int ret;

	bssid0 = ni->ni_bssid[0] |
	    ((uint32_t)ni->ni_bssid[1] << 8) |
	    ((uint32_t)ni->ni_bssid[2] << 16) |
	    ((uint32_t)ni->ni_bssid[3] << 24);
	bssid1 = ni->ni_bssid[4] |
	    ((uint16_t)ni->ni_bssid[5] << 8);
	intval = ni->ni_intval;
	urtwx_node_tx_info(ni, &rate_id, &desc_rate, &sgi, &ldpc, &ra_mask);
	if (urtwx_desc_rate_is_vht(desc_rate)) {
		ni->ni_vht_ss = urtwx_desc_rate_vht_nss(desc_rate);
		ni->ni_txmcs = urtwx_desc_rate_vht_mcs(desc_rate);
	} else if (urtwx_desc_rate_is_ht(desc_rate))
		ni->ni_txmcs = urtwx_desc_rate_ht_mcs(desc_rate);
	else if ((ret = urtwx_legacy_rate_index(ni, desc_rate)) >= 0)
		ni->ni_txrate = ret;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_BSSID, 0, bssid0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BSSID + 4, 0, bssid1);
	if (ret != 0)
		return (ret);
	aid = IEEE80211_AID(ni->ni_associd);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_ASSOCIATED_BFMER0_INFO,
	    0, aid);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_CR, BIT_PORT0_NET_TYPE,
	    0x00020000U);
	if (ret != 0)
		return (ret);

	rate = (ic->ic_curmode == IEEE80211_MODE_11B) ? 0 : 3;
	ret = urtwx_write_1_mask(sc, REG_INIRTS_RATE_SEL, 0x0f, rate);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_RXFLTMAP2, 0, 0xffff);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_TXPAUSE, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BCN_INTERVAL, 0, intval);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_BCN_CTRL, BIT_DIS_TSF_UDT, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_RCR,
	    BIT_CBSSID_DATA | BIT_CBSSID_BCN,
	    BIT_CBSSID_DATA | BIT_CBSSID_BCN);
	if (ret != 0)
		return (ret);
	ret = urtwx_fw_ra_info(sc, ni);
	if (ret != 0)
		return (ret);
	ret = urtwx_fw_download_sta_rsvd_page(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_fw_default_port(sc);
	if (ret != 0)
		return (ret);

	return (urtwx_fw_media_status(sc, 1));
}

static void
urtwx_node_tx_info(struct ieee80211_node *ni, uint8_t *rate_id,
    uint8_t *desc_rate, uint8_t *sgi, uint8_t *ldpc, uint32_t *ra_mask)
{
	struct urtwx_softc *sc = ni->ni_ic->ic_if.if_softc;
	const struct ieee80211_rateset *rs = &ni->ni_rates;
	int has_cck, has_ofdm;
	int i;

	has_cck = 0;
	has_ofdm = 0;
	for (i = 0; i < rs->rs_nrates; i++) {
		if ((rs->rs_rates[i] & IEEE80211_RATE_VAL) > 22)
			has_ofdm = 1;
		else
			has_cck = 1;
	}

	*rate_id = URTWX_RATEID_B_20M;
	*desc_rate = URTWX_DESC_RATE11M;
	*sgi = 0;
	*ldpc = 0;
	*ra_mask = URTWX_RA_MASK_CCK_RATES;

	if (has_cck && has_ofdm) {
		*rate_id = URTWX_RATEID_BG;
		*desc_rate = URTWX_DESC_RATE54M;
		*ra_mask = URTWX_RA_MASK_CCK_RATES |
		    URTWX_RA_MASK_OFDM_RATES;
	} else if (has_ofdm) {
		*rate_id = URTWX_RATEID_G;
		*desc_rate = URTWX_DESC_RATE54M;
		*ra_mask = URTWX_RA_MASK_OFDM_RATES;
	}

	if ((ni->ni_flags & IEEE80211_NODE_HT) != 0) {
		*rate_id = sc->sc_chip->uc_rateid_ht_20m;
		*desc_rate = sc->sc_chip->uc_desc_rate_ht_max;
		*ra_mask = URTWX_RA_MASK_CCK_RATES |
		    URTWX_RA_MASK_OFDM_RATES |
		    sc->sc_chip->uc_ra_mask_ht;
		if (IEEE80211_CHAN_40MHZ_ALLOWED(ni->ni_chan) &&
		    ieee80211_node_supports_ht_chan40(ni)) {
			*rate_id = sc->sc_chip->uc_rateid_ht_40m;
			if ((ni->ni_flags & IEEE80211_NODE_HT_SGI40) != 0)
				*sgi = 1;
		} else if ((ni->ni_flags & IEEE80211_NODE_HT_SGI20) != 0)
			*sgi = 1;
		if ((ni->ni_htcaps & IEEE80211_HTCAP_LDPC) != 0)
			*ldpc = 1;
	}
	if ((ni->ni_flags & IEEE80211_NODE_VHT) != 0) {
		if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
			*rate_id = sc->sc_chip->uc_rateid_vht_2g;
		else
			*rate_id = sc->sc_chip->uc_rateid_vht_5g;
		*desc_rate = sc->sc_chip->uc_desc_rate_vht_max;
		*sgi = 0;
		*ldpc = 0;
		*ra_mask = sc->sc_chip->uc_ra_mask_vht;
		if (ieee80211_node_supports_vht_sgi80(ni))
			*sgi = 1;
		if ((ni->ni_vhtcaps & IEEE80211_VHTCAP_RX_LDPC) != 0)
			*ldpc = 1;
	}
}

static void
urtwx_scan_to(void *arg)
{
	struct urtwx_softc *sc = arg;
	int s;

	if (usbd_is_dying(sc->sc_udev))
		return;

	usbd_ref_incr(sc->sc_udev);
	s = splnet();
	if (sc->sc_ic.ic_state == IEEE80211_S_SCAN)
		ieee80211_next_scan(&sc->sc_ic.ic_if);
	splx(s);
	usbd_ref_decr(sc->sc_udev);
}

static int
urtwx_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct urtwx_softc *sc = ic->ic_if.if_softc;
	struct urtwx_cmd_newstate cmd;

	if (nstate != IEEE80211_S_SCAN)
		timeout_del(&sc->sc_scan_to);

	cmd.state = nstate;
	cmd.ostate = ic->ic_state;
	cmd.arg = arg;
	cmd.seq = ++sc->sc_state_seq;
	if (!(cmd.state == IEEE80211_S_SCAN &&
	    cmd.ostate == IEEE80211_S_SCAN) &&
	    sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		printf("%s: newstate req #%u %s -> %s arg %#x\n",
		    DEVNAME(sc), cmd.seq, ieee80211_state_name[cmd.ostate],
		    ieee80211_state_name[cmd.state], cmd.arg);
		sc->sc_state_log++;
	}
	urtwx_do_async(sc, urtwx_newstate_cb, &cmd, sizeof(cmd));
	return (0);
}

static int
urtwx_ampdu_rx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
#if URTWX_REFUSE_RX_ADDBA
	/*
	 * Temporary diagnostic: refuse incoming ADDBA so we can isolate
	 * whether reverse-stream stalls are driven by RX BA/reorder.
	 */
	DPRINTFN(2, ("%s: refusing RX ADDBA from %s tid %u\n",
	    DEVNAME((struct urtwx_softc *)ic->ic_softc),
	    ether_sprintf(ni->ni_macaddr), tid));
	return (ENOTSUP);
#else
#if URTWX_RX_BA_NO_AMSDU
	/*
	 * Keep RX BA enabled, but do not advertise A-MSDU inside A-MPDU.
	 * On this USB path, allowing A-MSDU inside RX BA agreements causes
	 * severe reverse-direction throughput collapse under load. The most
	 * likely explanation is that firmware/hardware splits A-MSDU
	 * subframes before net80211 sees them, so multiple delivered MSDUs
	 * share a sequence number and collide in the software reorder buffer.
	 */
	ni->ni_rx_ba[tid].ba_params &= ~IEEE80211_ADDBA_AMSDU;
#endif
	return (0);
#endif
}

static void
urtwx_ampdu_rx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
}

static void
urtwx_newstate_cb(struct urtwx_softc *sc, void *arg)
{
	struct urtwx_cmd_newstate *cmd = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	enum ieee80211_state ostate;
	int ret;
	int s;

	s = splnet();
	ostate = ic->ic_state;
	if (!(cmd->state == IEEE80211_S_SCAN &&
	    cmd->ostate == IEEE80211_S_SCAN) &&
	    sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		printf("%s: newstate cb  #%u req %s -> %s cur %s arg %#x\n",
		    DEVNAME(sc), cmd->seq,
		    ieee80211_state_name[cmd->ostate],
		    ieee80211_state_name[cmd->state],
		    ieee80211_state_name[ostate], cmd->arg);
		sc->sc_state_log++;
	}
	if (cmd->ostate != ostate) {
		if (sc->sc_state_log < URTWX_STATE_LOG_MAX) {
			printf("%s: newstate stale#%u req %s -> %s cur %s "
			    "arg %#x\n",
			    DEVNAME(sc), cmd->seq,
			    ieee80211_state_name[cmd->ostate],
			    ieee80211_state_name[cmd->state],
			    ieee80211_state_name[ostate], cmd->arg);
			sc->sc_state_log++;
		}
		splx(s);
		return;
	}
	if (cmd->state == ostate &&
	    cmd->state != IEEE80211_S_SCAN &&
	    cmd->state != IEEE80211_S_AUTH) {
		if (sc->sc_state_log < URTWX_STATE_LOG_MAX) {
			printf("%s: newstate skip#%u cur %s arg %#x\n",
			    DEVNAME(sc), cmd->seq,
			    ieee80211_state_name[ostate], cmd->arg);
			sc->sc_state_log++;
		}
		splx(s);
		return;
	}
	if (ostate == IEEE80211_S_RUN && cmd->state != IEEE80211_S_RUN) {
		urtwx_sta_leave_run(sc);
	}
	switch (cmd->state) {
	case IEEE80211_S_INIT:
		timeout_del(&sc->sc_scan_to);
		break;
	case IEEE80211_S_SCAN:
#if URTWX_FORCE_PASSIVE_SCAN
		ic->ic_flags &= ~IEEE80211_F_ASCAN;
#endif
		if (ostate != IEEE80211_S_SCAN &&
		    ostate != IEEE80211_S_RUN)
			urtwx_sta_leave_run(sc);
		chan = ic->ic_bss->ni_chan;
		if (urtwx_set_chan(sc, chan) == 0 &&
		    !usbd_is_dying(sc->sc_udev)) {
			if (sc->sc_scan_log < URTWX_SCAN_LOG_MAX) {
				if (sc->sc_chip == &urtwx_chip_8822c) {
					uint16_t wreg_a, wreg_b;
					uint32_t read_a, read_b, write_a, write_b;
					uint8_t curchan;

					wreg_a = sc->sc_chip->uc_rf_write_base[0] +
					    ((uint16_t)0x18 << 2);
					wreg_b = sc->sc_chip->uc_rf_write_base[1] +
					    ((uint16_t)0x18 << 2);
					curchan = ieee80211_chan2ieee(ic, chan);
					read_a = urtwx_read_rf_path(sc, 0, 0x18);
					read_b = urtwx_read_rf_path(sc, 1, 0x18);
					write_a = urtwx_read_4(sc, RTW_USB_CMD_REQ,
					    wreg_a, 0) & RFREG_MASK;
					write_b = urtwx_read_4(sc, RTW_USB_CMD_REQ,
					    wreg_b, 0) & RFREG_MASK;
					printf("%s: scan channel %u rf18 want=%#x "
					    "reada=%#x readb=%#x writea=%#x "
					    "writeb=%#x\n", DEVNAME(sc),
					    curchan,
					    sc->sc_rf18_want, read_a, read_b,
					    write_a, write_b);
					if (sc->sc_scan_log < 4)
						urtwx_8822c_dump_chan_regs(sc,
						    curchan);
				} else {
					printf("%s: scan channel %u rf18=%#x\n",
					    DEVNAME(sc),
					    ieee80211_chan2ieee(ic, chan),
					    urtwx_read_rf(sc, 0x18));
				}
				sc->sc_scan_log++;
			}
			timeout_add_msec(&sc->sc_scan_to,
			    URTWX_SCAN_DWELL_TIME);
		}
		break;
	case IEEE80211_S_AUTH:
	case IEEE80211_S_ASSOC:
	case IEEE80211_S_RUN:
	{
		timeout_del(&sc->sc_scan_to);
		chan = (ic->ic_opmode == IEEE80211_M_MONITOR) ?
		    ic->ic_ibss_chan : ic->ic_bss->ni_chan;
		if (cmd->state == IEEE80211_S_AUTH &&
		    sc->sc_state_log < URTWX_STATE_LOG_MAX) {
			printf("%s: auth candidate bssid %s chan %u ess ",
			    DEVNAME(sc), ether_sprintf(ic->ic_bss->ni_bssid),
			    ieee80211_chan2ieee(ic, chan));
			ieee80211_print_essid(ic->ic_bss->ni_essid,
			    ic->ic_bss->ni_esslen);
			printf("\n");
			sc->sc_state_log++;
		}
		ret = urtwx_set_chan(sc, chan);
		if (ret != 0)
			printf("%s: could not set channel for state %s: %d\n",
			    DEVNAME(sc), ieee80211_state_name[cmd->state], ret);
		if (cmd->state == IEEE80211_S_RUN) {
			ret = urtwx_sta_enter_run(sc);
			if (ret != 0) {
				printf("%s: could not enter RUN state: %d\n",
				    DEVNAME(sc), ret);
			} else {
				DPRINTF(("%s: RUN bssid %s chan %u intval %u "
				    "aid %u cr=%#x\n",
				    DEVNAME(sc),
				    ether_sprintf(ic->ic_bss->ni_bssid),
				    ieee80211_chan2ieee(ic, chan),
				    ic->ic_bss->ni_intval,
				    IEEE80211_AID(ic->ic_bss->ni_associd),
				    urtwx_read_4(sc, RTW_USB_CMD_REQ,
				    REG_CR, 0)));
				sc->sc_tx_data_log = 0;
				sc->sc_txeof_data_log = 0;
			}
		}
		break;
	}
	}
	(void)sc->sc_newstate(ic, cmd->state, cmd->arg);
	if (cmd->state != IEEE80211_S_SCAN &&
	    sc->sc_state_log < URTWX_STATE_LOG_MAX) {
		printf("%s: newstate done#%u now %s link %u assoc_fail %#x "
		    "flags %#x\n",
		    DEVNAME(sc), cmd->seq, ieee80211_state_name[ic->ic_state],
		    ic->ic_if.if_link_state, ic->ic_bss->ni_assoc_fail,
		    ic->ic_flags);
		sc->sc_state_log++;
	}
	if (cmd->state == IEEE80211_S_RUN)
		urtwx_start(&ic->ic_if);
	splx(s);
}

static int
urtwx_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwx_softc *sc = ic->ic_softc;
	struct urtwx_cmd_key cmd;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP)
		return (ieee80211_set_key(ic, ni, k));
	if ((ic->ic_if.if_flags & (IFF_UP | IFF_RUNNING)) !=
	    (IFF_UP | IFF_RUNNING))
		return (0);

	memset(&cmd, 0, sizeof(cmd));
	cmd.key = *k;
	cmd.ni = ni;
	cmd.set = 1;
	urtwx_do_async(sc, urtwx_set_key_cb, &cmd, sizeof(cmd));
	sc->sc_key_tasks++;

	return (EBUSY);
}

static void
urtwx_set_key_cb(struct urtwx_softc *sc, void *arg)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct urtwx_cmd_key *cmd = arg;

	sc->sc_key_tasks--;
	if (urtwx_hw_set_key(ic, cmd->ni, &cmd->key) == 0) {
		if (sc->sc_key_tasks == 0 && cmd->ni != NULL) {
			cmd->ni->ni_port_valid = 1;
			ieee80211_set_link_state(ic, LINK_STATE_UP);
		}
	} else if (cmd->ni != NULL) {
		IEEE80211_SEND_MGMT(ic, cmd->ni, IEEE80211_FC0_SUBTYPE_DEAUTH,
		    IEEE80211_REASON_AUTH_LEAVE);
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	}
}

static void
urtwx_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwx_softc *sc = ic->ic_softc;
	struct urtwx_cmd_key cmd;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP) {
		ieee80211_delete_key(ic, ni, k);
		return;
	}
	if (!(ic->ic_if.if_flags & IFF_RUNNING) || ic->ic_state != IEEE80211_S_RUN)
		return;

	memset(&cmd, 0, sizeof(cmd));
	cmd.key = *k;
	cmd.ni = ni;
	urtwx_do_async(sc, urtwx_delete_key_cb, &cmd, sizeof(cmd));
}

static void
urtwx_delete_key_cb(struct urtwx_softc *sc, void *arg)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct urtwx_cmd_key *cmd = arg;

	urtwx_hw_delete_key(ic, cmd->ni, &cmd->key);
}

static int
urtwx_sec_cam_write(struct urtwx_softc *sc, uint8_t addr, uint32_t data)
{
	int ret;

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_CAMWRITE, 0, data);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_CAMCMD, 0,
	    URTWX_SEC_CMD_POLLING | URTWX_SEC_CMD_WRITE_ENABLE | addr);
	if (ret != 0)
		return (ret);

	return (urtwx_wait_4(sc, REG_CAMCMD, URTWX_SEC_CMD_POLLING, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US));
}

static int
urtwx_sec_enable(struct urtwx_softc *sc)
{
	uint16_t seccfg;
	int ret;

	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_CAMCMD, 0,
	    URTWX_SEC_CMD_POLLING | URTWX_SEC_CMD_CLEAR);
	if (ret != 0)
		return (ret);
	ret = urtwx_wait_4(sc, REG_CAMCMD, URTWX_SEC_CMD_POLLING, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_CR, BIT_MAC_SEC_EN,
	    BIT_MAC_SEC_EN);
	if (ret != 0)
		return (ret);

	seccfg = urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_SECCFG, 0);
	seccfg |= BIT_SECCFG_TXUCKEY_DEF | BIT_SECCFG_RXUCKEY_DEF |
	    BIT_SECCFG_TXENC_ENA | BIT_SECCFG_RXENC_ENA |
	    BIT_SECCFG_TXBCKEY_DEF | BIT_SECCFG_RXBCKEY_DEF;

	return (urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_SECCFG, 0, seccfg));
}

static int
urtwx_hw_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwx_softc *sc = ic->ic_softc;
	static const uint8_t etherbroadcastaddr[IEEE80211_ADDR_LEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	const uint8_t *macaddr;
	uint8_t algo, entry, keybuf[16];
	uint32_t content;
	int i, j, ret;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP)
		return (EINVAL);

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0) {
		macaddr = etherbroadcastaddr;
		entry = k->k_id;
	} else {
		if (ni == NULL)
			return (EINVAL);
		macaddr = ni->ni_macaddr;
		entry = URTWX_CAM_PAIRWISE_ENTRY;
	}
	algo = URTWX_CAM_ALGO_CCMP;

	memset(keybuf, 0, sizeof(keybuf));
	memcpy(keybuf, k->k_key, MIN(k->k_len, sizeof(keybuf)));
	for (i = URTWX_CAM_ENTRY_WORDS - 1; i >= 0; i--) {
		switch (i) {
		case 0:
			content = (k->k_id & 0x3) |
			    ((uint32_t)(algo & 0x7) << 2) |
			    (((k->k_flags & IEEE80211_KEY_GROUP) != 0) << 6) |
			    (1U << 15) |
			    ((uint32_t)macaddr[0] << 16) |
			    ((uint32_t)macaddr[1] << 24);
			break;
		case 1:
			content = macaddr[2] |
			    ((uint32_t)macaddr[3] << 8) |
			    ((uint32_t)macaddr[4] << 16) |
			    ((uint32_t)macaddr[5] << 24);
			break;
		case 6:
		case 7:
			content = 0;
			break;
		default:
			j = (i - 2) << 2;
			content = keybuf[j] |
			    ((uint32_t)keybuf[j + 1] << 8) |
			    ((uint32_t)keybuf[j + 2] << 16) |
			    ((uint32_t)keybuf[j + 3] << 24);
			break;
		}

		ret = urtwx_sec_cam_write(sc,
		    (entry << URTWX_CAM_ENTRY_SHIFT) + i, content);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static void
urtwx_hw_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwx_softc *sc = ic->ic_softc;
	uint8_t entry;
	int i;

	if (k->k_cipher != IEEE80211_CIPHER_CCMP)
		return;

	if ((k->k_flags & IEEE80211_KEY_GROUP) != 0)
		entry = k->k_id;
	else
		entry = URTWX_CAM_PAIRWISE_ENTRY;

	for (i = 0; i < URTWX_CAM_ENTRY_WORDS; i++) {
		if (urtwx_sec_cam_write(sc,
		    (entry << URTWX_CAM_ENTRY_SHIFT) + i, 0) != 0)
			break;
	}
}

static int
urtwx_alloc_rx_list(struct urtwx_softc *sc)
{
	struct urtwx_rx_data *data;
	int i, error;

	error = 0;
	for (i = 0; i < URTWX_RX_LIST_COUNT; i++) {
		data = &sc->sc_rx_data[i];
		data->sc = sc;

		data->xfer = usbd_alloc_xfer(sc->sc_udev);
		if (data->xfer == NULL) {
			error = ENOMEM;
			break;
		}
		data->buf = usbd_alloc_buffer(data->xfer, URTWX_RXBUFSZ);
		if (data->buf == NULL) {
			error = ENOMEM;
			break;
		}
	}
	if (error != 0)
		urtwx_free_rx_list(sc);

	return (error);
}

static void
urtwx_free_rx_list(struct urtwx_softc *sc)
{
	int i;

	for (i = 0; i < URTWX_RX_LIST_COUNT; i++) {
		if (sc->sc_rx_data[i].xfer != NULL)
			usbd_free_xfer(sc->sc_rx_data[i].xfer);
		sc->sc_rx_data[i].xfer = NULL;
		sc->sc_rx_data[i].buf = NULL;
	}
}

static void
urtwx_task(void *arg)
{
	struct urtwx_softc *sc = arg;
	struct urtwx_host_cmd_ring *ring = &sc->sc_cmdq;
	struct urtwx_host_cmd *cmd;
	int s;

	if (usbd_is_dying(sc->sc_udev))
		return;

	s = splusb();
	while (ring->next != ring->cur) {
		cmd = &ring->cmd[ring->next];
		splx(s);
		cmd->cb(sc, cmd->data);
		s = splusb();
		ring->queued--;
		ring->next = (ring->next + 1) %
		    URTWX_HOST_CMD_RING_COUNT;
	}
	splx(s);
}

static void
urtwx_do_async(struct urtwx_softc *sc,
    void (*cb)(struct urtwx_softc *, void *), void *arg, int len)
{
	struct urtwx_host_cmd_ring *ring = &sc->sc_cmdq;
	struct urtwx_host_cmd *cmd;
	int s;

	if (usbd_is_dying(sc->sc_udev))
		return;

	s = splusb();
	cmd = &ring->cmd[ring->cur];
	cmd->cb = cb;
	KASSERT(len <= sizeof(cmd->data));
	memcpy(cmd->data, arg, len);
	ring->cur = (ring->cur + 1) % URTWX_HOST_CMD_RING_COUNT;

	if (++ring->queued == 1)
		usb_add_task(sc->sc_udev, &sc->sc_task);
	splx(s);
}

static void
urtwx_wait_async(struct urtwx_softc *sc)
{
	int s;

	s = splusb();
	usb_wait_task(sc->sc_udev, &sc->sc_task);
	splx(s);
}

static void
urtwx_close_pipes(struct urtwx_softc *sc)
{
	int i;

	if (sc->sc_ep[URTWX_ENDPT_RX] != NULL) {
		usbd_close_pipe(sc->sc_ep[URTWX_ENDPT_RX]);
		sc->sc_ep[URTWX_ENDPT_RX] = NULL;
	}
	for (i = 0; i < URTWX_MAX_OUT_PIPES; i++) {
		if (sc->sc_out_pipe[i] != NULL) {
			usbd_close_pipe(sc->sc_out_pipe[i]);
			sc->sc_out_pipe[i] = NULL;
		}
	}
	if (sc->sc_ep[URTWX_ENDPT_INTR] != NULL) {
		usbd_close_pipe(sc->sc_ep[URTWX_ENDPT_INTR]);
		sc->sc_ep[URTWX_ENDPT_INTR] = NULL;
	}
}

static void
urtwx_parse_rx_desc(const struct urtwx_rx_desc *rxd, struct urtwx_rx_stat *stat)
{
	uint32_t w0, w2;

	memset(stat, 0, sizeof(*stat));

	w0 = letoh32(rxd->w0);
	w2 = letoh32(rxd->w2);

	stat->pktlen = w0 & 0x3fff;
	stat->crc_err = (w0 & 0x4000) != 0;
	stat->icv_err = (w0 & 0x8000) != 0;
	stat->drvinfosz = ((w0 >> 16) & 0xf) * 8;
	stat->enc_type = (w0 >> 20) & 0x7;
	stat->shift = (w0 >> 24) & 0x3;
	stat->phy_status = (w0 & 0x04000000U) != 0;
	stat->swdec = (w0 >> 27) & 0x1;
	stat->decrypted = !stat->swdec && stat->enc_type != 0;
	stat->is_c2h = (w2 & 0x10000000U) != 0;
	stat->rate = letoh32(rxd->w3) & 0x7f;
}

static void
urtwx_parse_c2h(struct urtwx_softc *sc, const uint8_t *buf, int len)
{
	const struct urtwx_c2h_cmd *c2h;
	const struct urtwx_c2h_ra_rpt *ra;
	const char *name;
	const char *rate_name;
	uint8_t status, sn, subid;
	uint8_t rate, sgi;
	char bytes[3 * 8];
	char ratebuf[16];
	int i, n, off;

	if (len < 2)
		return;

	c2h = (const struct urtwx_c2h_cmd *)buf;
	name = "unknown";
	switch (c2h->id) {
	case URTWX_C2H_CCX_TX_RPT:
		name = "ccx-tx-rpt";
		break;
	case URTWX_C2H_RA_RPT:
		name = "ra-rpt";
		break;
	case URTWX_C2H_WLAN_INFO:
		name = "wlan-info";
		break;
	case URTWX_C2H_BCN_FILTER_NOTIFY:
		name = "bcn-filter";
		break;
	case URTWX_C2H_SCAN_RESULT:
		name = "scan-result";
		break;
	case URTWX_C2H_HALMAC:
		name = "halmac";
		break;
	}

	if (sc->sc_c2h_log < URTWX_C2H_LOG_MAX) {
		n = MIN(len, 12);
		off = 0;
		memset(bytes, 0, sizeof(bytes));
		for (i = 0; i < n && off < sizeof(bytes); i++) {
			off += snprintf(bytes + off, sizeof(bytes) - off,
			    "%s%02x", (i == 0) ? "" : " ", buf[i]);
		}
		DPRINTFN(2, ("%s: C2H id %#x (%s) seq %#x len %d data %s\n",
		    DEVNAME(sc), c2h->id, name, c2h->seq, len, bytes));
		sc->sc_c2h_log++;
	}

	switch (c2h->id) {
	case URTWX_C2H_CCX_TX_RPT:
		if (len < 9)
			break;
		sn = URTWX_GET_CCX_RPT_SN_V0(c2h->payload);
		status = URTWX_GET_CCX_RPT_ST_V0(c2h->payload);
		if (sc->sc_c2h_tx_rpt_log < URTWX_C2H_TX_RPT_MAX) {
			DPRINTFN(2, ("%s: C2H tx report sn %#x status %#x "
			    "ack %s\n", DEVNAME(sc), sn, status,
			    (status == 0) ? "yes" : "no"));
			sc->sc_c2h_tx_rpt_log++;
		}
		break;
	case URTWX_C2H_RA_RPT:
		if (len < (int)(2 + sizeof(*ra)))
			break;
		ra = (const struct urtwx_c2h_ra_rpt *)c2h->payload;
		rate = ra->rate_sgi & 0x7f;
		sgi = (ra->rate_sgi & 0x80) != 0;
		if (ra->macid == URTWX_MACID_STA)
			urtwx_update_ra_node(sc, rate);
		rate_name = urtwx_desc_rate_name(rate, ratebuf, sizeof(ratebuf));
		if (sc->sc_c2h_tx_rpt_log < URTWX_C2H_TX_RPT_MAX) {
			DPRINTFN(2, ("%s: C2H RA report macid %u %s (%#x) "
			    "sgi %u bw %s status %#x ratio %u\n",
			    DEVNAME(sc), ra->macid, rate_name, rate, sgi,
			    urtwx_ra_bw_name(ra->bw), ra->status,
			    ra->ra_ratio));
			sc->sc_c2h_tx_rpt_log++;
		}
		break;
	case URTWX_C2H_HALMAC:
		if (len < 3)
			break;
		subid = c2h->payload[0];
		if (sc->sc_c2h_tx_rpt_log < URTWX_C2H_TX_RPT_MAX) {
			DPRINTFN(2, ("%s: C2H HALMAC subid %#x\n",
			    DEVNAME(sc), subid));
			sc->sc_c2h_tx_rpt_log++;
		}
		if (subid == 0x0f && len >= 12) {
			sn = c2h->payload[8] & 0xfc;
			status = c2h->payload[9] & 0xc0;
			if (sc->sc_c2h_tx_rpt_log < URTWX_C2H_TX_RPT_MAX) {
				DPRINTFN(2, ("%s: C2H HALMAC tx report sn %#x "
				    "status %#x ack %s\n", DEVNAME(sc), sn,
				    status, (status == 0) ? "yes" : "no"));
				sc->sc_c2h_tx_rpt_log++;
			}
		}
		break;
	}
}

static uint8_t
urtwx_rx_power_to_rssi(int8_t rx_power)
{
	if (rx_power <= -100)
		return (0);
	if (rx_power >= 0)
		return (100);
	return (100 + rx_power);
}

static int8_t
urtwx_cck_rx_power(struct urtwx_softc *sc, const uint8_t *phy_status)
{
	const int8_t *lna_gain_table;
	size_t lna_gain_table_size;
	uint32_t dw3;
	uint8_t lna_idx, vga_idx;

	if (sc->sc_rfe_option == 0) {
		lna_gain_table = urtwx_lna_gain_table_0;
		lna_gain_table_size = nitems(urtwx_lna_gain_table_0);
	} else {
		lna_gain_table = urtwx_lna_gain_table_1;
		lna_gain_table_size = nitems(urtwx_lna_gain_table_1);
	}

	dw3 = UGETDW(phy_status + 12);
	vga_idx = (dw3 >> 8) & 0x1f;
	lna_idx = ((dw3 >> 20) & 0x8) | ((dw3 >> 13) & 0x7);
	if (lna_idx >= lna_gain_table_size)
		return (-100);

	return (lna_gain_table[lna_idx] - (2 * vga_idx));
}

static void
urtwx_get_rx_rssi(struct urtwx_softc *sc, const uint8_t *phy_status,
    struct urtwx_rx_stat *stat)
{
	stat->rssi = 0;
	if (!stat->phy_status || stat->drvinfosz == 0 || phy_status == NULL)
		return;
	if (sc->sc_chip->uc_get_rx_rssi == NULL)
		return;

	stat->rssi = sc->sc_chip->uc_get_rx_rssi(sc, phy_status, stat);
}

static uint8_t
urtwx_get_rx_rssi_8821c(struct urtwx_softc *sc, const uint8_t *phy_status,
    const struct urtwx_rx_stat *stat)
{
	uint32_t dw0;
	int8_t rx_power;
	uint8_t page;

	page = phy_status[0] & 0x0f;
	if (page == 0 || stat->rate <= URTWX_DESC_RATE11M) {
		if (stat->drvinfosz < 16)
			return (0);
		rx_power = urtwx_cck_rx_power(sc, phy_status);
		return (urtwx_rx_power_to_rssi(rx_power));
	}

	if (stat->drvinfosz < 8)
		return (0);

	/*
	 * 8821C OFDM/HT/VHT PHY status uses page 1, where PWDB_A lives in
	 * dword 0 bits 15:8.  The old code was reading from dword 1, which
	 * left 5 GHz RSSI effectively at zero.
	 */
	dw0 = UGETDW(phy_status);
	rx_power = ((dw0 >> 8) & 0xff) - 110;
	return (urtwx_rx_power_to_rssi(rx_power));
}

static uint8_t
urtwx_get_rx_rssi_8822c(struct urtwx_softc *sc, const uint8_t *phy_status,
    const struct urtwx_rx_stat *stat)
{
	uint32_t dw0, dw1;
	uint8_t page, pwdb_a, pwdb_b, pwdb;
	int8_t rx_power;

	(void)sc;

	if (stat->drvinfosz < 8)
		return (0);

	page = phy_status[0] & 0x0f;
	dw0 = UGETDW(phy_status);
	dw1 = UGETDW(phy_status + 4);

	if (page == 0 || stat->rate <= URTWX_DESC_RATE11M) {
		pwdb_a = (dw0 >> 8) & 0xff;
		pwdb_b = dw1 & 0xff;
	} else {
		pwdb_a = (dw0 >> 8) & 0xff;
		pwdb_b = (dw0 >> 16) & 0xff;
	}

	if (pwdb_a == 0 && pwdb_b == 0)
		return (0);
	if (pwdb_b == 0)
		pwdb = pwdb_a;
	else if (pwdb_a == 0)
		pwdb = pwdb_b;
	else
		pwdb = (pwdb_a + pwdb_b) / 2;

	rx_power = pwdb - 110;
	return (urtwx_rx_power_to_rssi(rx_power));
}

static void
urtwx_rx_frame(struct urtwx_softc *sc, const uint8_t *buf, int len,
    const struct urtwx_rx_stat *stat, struct mbuf_list *ml)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	const struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct ieee80211_rxinfo rxi;
	struct mbuf *m;
	const uint8_t *frm, *efrm;
	const char *name;
	char ssid[IEEE80211_NWID_LEN + 1];
	uint8_t subtype, chan;
	uint16_t aid, alg, seq, status;
	int i, s, ssidlen;

	if (!(ifp->if_flags & IFF_RUNNING))
		return;
	if (len < sizeof(*wh))
		return;

	wh = (const struct ieee80211_frame *)buf;
	if (ic->ic_opmode == IEEE80211_M_STA &&
	    ic->ic_state != IEEE80211_S_SCAN &&
	    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_FROMDS &&
	    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
	    !IEEE80211_ADDR_EQ(wh->i_addr1, ic->ic_myaddr))
		return;
	if (len > MCLBYTES) {
		ifp->if_ierrors++;
		return;
	}

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		ifp->if_ierrors++;
		return;
	}
	if (len > MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			ifp->if_ierrors++;
			m_freem(m);
			return;
		}
	}
	memcpy(mtod(m, uint8_t *), buf, len);
	m->m_pkthdr.len = m->m_len = len;

	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT) {
		subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
		switch (subtype) {
		case IEEE80211_FC0_SUBTYPE_BEACON:
			name = "beacon";
			break;
		case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
			name = "probe response";
			break;
		case IEEE80211_FC0_SUBTYPE_AUTH:
			name = "auth";
			break;
		case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
			name = "assoc response";
			break;
		case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
			name = "reassoc response";
			break;
		default:
			name = NULL;
			break;
		}
		if (name != NULL) {
			if (subtype == IEEE80211_FC0_SUBTYPE_AUTH) {
				if (len < sizeof(*wh) + 6)
					goto done_log;
				memcpy(&alg, buf + sizeof(*wh), sizeof(alg));
				memcpy(&seq, buf + sizeof(*wh) + 2, sizeof(seq));
				memcpy(&status, buf + sizeof(*wh) + 4,
				    sizeof(status));
				alg = letoh16(alg);
				seq = letoh16(seq);
				status = letoh16(status);
				printf("%s: %s bssid %s alg %u seq %u "
				    "status %u\n",
				    DEVNAME(sc), name,
				    ether_sprintf((u_char *)wh->i_addr3), alg,
				    seq, status);
				goto done_log;
			}
			if (subtype == IEEE80211_FC0_SUBTYPE_ASSOC_RESP ||
			    subtype == IEEE80211_FC0_SUBTYPE_REASSOC_RESP) {
				if (len < sizeof(*wh) + 6)
					goto done_log;
				memcpy(&status, buf + sizeof(*wh) + 2,
				    sizeof(status));
				memcpy(&aid, buf + sizeof(*wh) + 4, sizeof(aid));
				status = letoh16(status);
				aid = letoh16(aid);
				printf("%s: %s bssid %s status %u aid %#x\n",
				    DEVNAME(sc), name,
				    ether_sprintf((u_char *)wh->i_addr3),
				    status, aid);
				goto done_log;
			}
			if (sc->sc_rx_mgmt_log >= URTWX_RX_MGMT_LOG_MAX)
				goto done_log;
			frm = buf + sizeof(*wh) + 12;
			efrm = buf + len;
			ssidlen = -1;
			chan = 0;

			while (frm + 2 <= efrm) {
				if (frm + 2 + frm[1] > efrm)
					break;

				switch (frm[0]) {
				case IEEE80211_ELEMID_SSID:
					ssidlen = MIN(frm[1], IEEE80211_NWID_LEN);
					if (ssidlen == 0) {
						strlcpy(ssid, "<hidden>",
						    sizeof(ssid));
						break;
					}
					for (i = 0; i < ssidlen; i++) {
						if (frm[2 + i] >= 0x20 &&
						    frm[2 + i] <= 0x7e)
							ssid[i] = frm[2 + i];
						else
							ssid[i] = '.';
					}
			ssid[ssidlen] = '\0';
					break;
				case IEEE80211_ELEMID_DSPARMS:
					if (frm[1] >= 1)
						chan = frm[2];
					break;
				}

				frm += 2 + frm[1];
			}
			if (ssidlen < 0)
				strlcpy(ssid, "<missing>", sizeof(ssid));
			if (chan != 0) {
				if (subtype == IEEE80211_FC0_SUBTYPE_BEACON ||
				    subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP) {
					DPRINTFN(2, ("%s: %s bssid %s chan %u "
					    "ssid \"%s\"\n", DEVNAME(sc), name,
					    ether_sprintf((u_char *)wh->i_addr3),
					    chan, ssid));
				} else {
					printf("%s: %s bssid %s chan %u ssid "
					    "\"%s\"\n", DEVNAME(sc), name,
					    ether_sprintf((u_char *)wh->i_addr3),
					    chan, ssid);
				}
			} else {
				if (subtype == IEEE80211_FC0_SUBTYPE_BEACON ||
				    subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP) {
					DPRINTFN(2, ("%s: %s bssid %s ssid "
					    "\"%s\"\n", DEVNAME(sc), name,
					    ether_sprintf((u_char *)wh->i_addr3),
					    ssid));
				} else {
					printf("%s: %s bssid %s ssid \"%s\"\n",
					    DEVNAME(sc), name,
					    ether_sprintf((u_char *)wh->i_addr3),
					    ssid);
				}
			}
			sc->sc_rx_mgmt_log++;
		}
	}
done_log:

	s = splnet();
	ni = ieee80211_find_rxnode(ic, mtod(m, struct ieee80211_frame *));
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = stat->rssi;
	if (stat->decrypted)
		rxi.rxi_flags |= IEEE80211_RXI_HWDEC;
	ieee80211_inputm(ifp, m, ni, &rxi, ml);
	ieee80211_release_node(ic, ni);
	splx(s);
}

static void
urtwx_rxeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct urtwx_rx_data *data = priv;
	struct urtwx_softc *sc = data->sc;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct urtwx_rx_desc *rxd;
	struct urtwx_rx_stat stat;
	const struct ieee80211_frame_min *wh;
	uint8_t *buf;
	int error, len, pktoff, pktlen, pkttot, protected;

	if (status != USBD_NORMAL_COMPLETION) {
		DPRINTF(("%s: RX status=%d\n", DEVNAME(sc), status));
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_ep[URTWX_ENDPT_RX]);
		if (status != USBD_CANCELLED)
			goto resubmit;
		return;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &len, NULL);
	buf = data->buf;
	sc->sc_rx_done++;
	if (sc->sc_rx_done <= 8) {
		printf("%s: RX completion %u len %d\n", DEVNAME(sc),
		    sc->sc_rx_done, len);
	}

	while (len >= URTWX_RX_DESC_SIZE) {
		rxd = (struct urtwx_rx_desc *)buf;
		urtwx_parse_rx_desc(rxd, &stat);

		if (stat.pktlen == 0)
			break;
		if (sc->sc_rx_desc_log < 16) {
			DPRINTFN(2, ("%s: RX desc pktlen %u info %u shift %u "
			    "c2h %u crc %u icv %u\n", DEVNAME(sc),
			    stat.pktlen, stat.drvinfosz, stat.shift,
			    stat.is_c2h, stat.crc_err, stat.icv_err));
			sc->sc_rx_desc_log++;
		}

		pktoff = URTWX_RX_DESC_SIZE + stat.drvinfosz + stat.shift;
		pktlen = stat.pktlen + pktoff;
		pkttot = roundup(pktlen, 8);

		if (pktoff > len || pktlen > len) {
			sc->sc_rx_err++;
			DPRINTF(("%s: RX packet overflow len=%d pkt=%d off=%d\n",
			    DEVNAME(sc), len, stat.pktlen, pktoff));
			printf("%s: RX packet overflow len %d pkt %u off %d "
			    "total %d\n", DEVNAME(sc), len, stat.pktlen,
			    pktoff, pkttot);
			break;
		}
		if (pkttot > len)
			pkttot = len;

		urtwx_get_rx_rssi(sc, buf + URTWX_RX_DESC_SIZE + stat.shift,
		    &stat);

		if (stat.is_c2h) {
			sc->sc_rx_c2h++;
			if (sc->sc_rx_c2h <= 8) {
				DPRINTFN(2, ("%s: RX C2H pktlen %u\n",
				    DEVNAME(sc), stat.pktlen));
			}
			urtwx_parse_c2h(sc, buf + pktoff, stat.pktlen);
		} else if (!stat.crc_err) {
			protected = 0;
			if (stat.pktlen >= sizeof(*wh)) {
				wh = (const struct ieee80211_frame_min *)
				    (buf + pktoff);
				protected =
				    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) != 0;
			}
			if (!stat.icv_err || protected)
				urtwx_rx_frame(sc, buf + pktoff, stat.pktlen,
				    &stat, &ml);
			else
				sc->sc_rx_err++;
		}

		buf += pkttot;
		len -= pkttot;
	}
	if_input(&sc->sc_ic.ic_if, &ml);

resubmit:
	usbd_setup_xfer(xfer, sc->sc_ep[URTWX_ENDPT_RX], data, data->buf,
	    URTWX_RXBUFSZ, USBD_SHORT_XFER_OK | USBD_NO_COPY,
	    USBD_NO_TIMEOUT, urtwx_rxeof);
	error = usbd_transfer(data->xfer);
	if (error != 0 && error != USBD_IN_PROGRESS) {
		DPRINTF(("%s: could not resubmit RX transfer: %d\n",
		    DEVNAME(sc), error));
	}
}

static int
urtwx_rx_init(struct urtwx_softc *sc)
{
	struct urtwx_rx_data *data;
	int i, error;

	sc->sc_rx_mgmt_log = 0;
	sc->sc_rx_done = 0;
	sc->sc_rx_c2h = 0;
	sc->sc_rx_desc_log = 0;
	sc->sc_rx_err = 0;
	sc->sc_tx_report_req = 0;
	sc->sc_c2h_log = 0;
	sc->sc_c2h_tx_rpt_log = 0;

	for (i = 0; i < URTWX_RX_LIST_COUNT; i++) {
		data = &sc->sc_rx_data[i];
		usbd_setup_xfer(data->xfer, sc->sc_ep[URTWX_ENDPT_RX], data,
		    data->buf, URTWX_RXBUFSZ,
		    USBD_SHORT_XFER_OK | USBD_NO_COPY, USBD_NO_TIMEOUT,
		    urtwx_rxeof);
		error = usbd_transfer(data->xfer);
		if (i < 8) {
			printf("%s: RX submit %d status %d pipe %p ed %#x\n",
			    DEVNAME(sc), i, error,
			    (void *)sc->sc_ep[URTWX_ENDPT_RX],
			    sc->sc_ed[URTWX_ENDPT_RX]);
		}
		if (error != 0 && error != USBD_IN_PROGRESS)
			return (EIO);
	}

	return (0);
}

static int
urtwx_media_change(struct ifnet *ifp)
{
	int error;

	error = ieee80211_media_change(ifp);
	if (error != ENETRESET)
		return (error);

	if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
	    (IFF_UP | IFF_RUNNING)) {
		urtwx_stop(ifp);
		error = urtwx_init(ifp);
	}
	return (error);
}

static int
urtwx_init(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error;

	printf("%s: init fw_loaded %d have_mac %d state %s\n",
	    DEVNAME(sc), sc->sc_fw_loaded, sc->sc_have_macaddr,
	    ieee80211_state_name[ic->ic_state]);
	printf("%s: init des_ess ", DEVNAME(sc));
	ieee80211_print_essid(ic->ic_des_essid, ic->ic_des_esslen);
	printf(" len %d autojoin %d rsn %d\n", ic->ic_des_esslen,
	    ISSET(ic->ic_flags, IEEE80211_F_AUTO_JOIN) != 0,
	    ISSET(ic->ic_flags, IEEE80211_F_RSNON) != 0);
	printf("%s: usb ed rx=%#x intr=%#x out=%d pipe_rx=%p pipe_intr=%p\n",
	    DEVNAME(sc), sc->sc_ed[URTWX_ENDPT_RX],
	    sc->sc_ed[URTWX_ENDPT_INTR], sc->sc_nbulkout,
	    (void *)sc->sc_ep[URTWX_ENDPT_RX],
	    (void *)sc->sc_ep[URTWX_ENDPT_INTR]);
	sc->sc_rfinit_log = 0;
	sc->sc_rfinit_prestage_log = 0;

	if (!sc->sc_fw_loaded) {
		error = urtwx_fw_download(sc);
		if (error != 0) {
			printf("%s: firmware download failed\n",
			    DEVNAME(sc));
			return (error);
		}

		if (!sc->sc_have_macaddr) {
			if (urtwx_read_efuse(sc, sc->sc_macaddr) != 0) {
				printf("%s: could not read efuse MAC address\n",
				    DEVNAME(sc));
				return (EIO);
			}
			if (sc->sc_chip == &urtwx_chip_8822c)
				urtwx_8822c_board_info_setup(sc);
			sc->sc_have_macaddr = 1;
			printf("%s: MAC %s\n", DEVNAME(sc),
			    ether_sprintf(sc->sc_macaddr));
		}

		error = if_setlladdr(ifp, sc->sc_macaddr);
		if (error != 0) {
			printf("%s: could not set MAC address %s: %d\n",
			    DEVNAME(sc), ether_sprintf(sc->sc_macaddr), error);
			return (error);
		}
		sc->sc_fw_loaded = 1;
	}

	error = urtwx_power_on(sc);
	if (error != 0) {
		printf("%s: power-on/system init failed\n", DEVNAME(sc));
		return (error);
	}
	if (sc->sc_chip == &urtwx_chip_8822c)
		urtwx_8822c_log_usb_mode(sc);

	if (urtwx_mac_init_runtime(sc) != 0) {
		printf("%s: could not initialize MAC/RX path\n",
		    DEVNAME(sc));
		return (EIO);
	}

	if (urtwx_rx_init(sc) != 0) {
		printf("%s: could not start RX path\n", DEVNAME(sc));
		return (EIO);
	}

	DPRINTF(("%s: runtime regs CR=%#x RCR=%#x RXDMA_MODE=%#x "
	    "TXDMA_PQ=%#x\n", DEVNAME(sc),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RCR, 0),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_RXDMA_MODE, 0),
	    urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP, 0)));
	DPRINTF(("%s: rxdma agg=%#x pktnum=%#x status=%#x dpr=%#x\n",
	    DEVNAME(sc),
	    urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_RXDMA_AGG_PG_TH, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXPKT_NUM, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXDMA_STATUS, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXDMA_DPR, 0)));
	DPRINTF(("%s: fixed channel %u rf18=%#x rfectl=%#x xtal=%#x "
	    "rfe=%#x\n", DEVNAME(sc), URTWX_FIXED_CHANNEL,
	    urtwx_read_rf(sc, 0x18),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RFECTL, 0),
	    sc->sc_crystal_cap, sc->sc_rfe_option));
	DPRINTF(("%s: RX ready, listening for management frames\n",
	    DEVNAME(sc)));

	IEEE80211_ADDR_COPY(ic->ic_myaddr,
	    ((struct arpcom *)ifp)->ac_enaddr);
	if (urtwx_set_macaddr(sc, ic->ic_myaddr) != 0)
		return (EIO);

	ic->ic_bss->ni_chan = ic->ic_ibss_chan;
	sc->sc_scan_log = 0;
	sc->sc_state_log = 0;
	sc->sc_tx_mgmt_log = 0;
	sc->sc_rfio_log = 0;
	sc->sc_usbctl_log = 0;
	sc->sc_tx_data_log = 0;
	sc->sc_txeof_log = 0;
	sc->sc_txeof_data_log = 0;
	sc->sc_rx_mgmt_log = 0;
	sc->sc_rx_done = 0;
	sc->sc_rx_c2h = 0;
	sc->sc_rx_desc_log = 0;
	sc->sc_rx_err = 0;
	sc->sc_tx_report_sn = 0;
	sc->sc_tx_report_req = 0;
	sc->sc_c2h_log = 0;
	sc->sc_c2h_tx_rpt_log = 0;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	printf("%s: init begin scan\n", DEVNAME(sc));
	ieee80211_begin_scan(ifp);
	return (0);
}

static void
urtwx_stop(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int i;
	int leave_mgt;
	int s;

	printf("%s: stop state %s fw_loaded %d\n", DEVNAME(sc),
	    ieee80211_state_name[ic->ic_state], sc->sc_fw_loaded);
	printf("%s: stop des_ess ", DEVNAME(sc));
	ieee80211_print_essid(ic->ic_des_essid, ic->ic_des_esslen);
	printf(" len %d autojoin %d rsn %d\n", ic->ic_des_esslen,
	    ISSET(ic->ic_flags, IEEE80211_F_AUTO_JOIN) != 0,
	    ISSET(ic->ic_flags, IEEE80211_F_RSNON) != 0);

	timeout_del(&sc->sc_scan_to);

	leave_mgt = (ic->ic_state >= IEEE80211_S_ASSOC) ? 0 : -1;
	s = splnet();
	ieee80211_new_state(ic, IEEE80211_S_INIT, leave_mgt);
	urtwx_wait_async(sc);
	splx(s);

	if (leave_mgt != -1) {
		for (i = 0; i < 200; i++) {
			if (!sc->sc_tx_active)
				break;
			delay(1000);
		}
	}

	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);

	if (sc->sc_ep[URTWX_ENDPT_RX] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_RX]);
	for (i = 0; i < sc->sc_nbulkout; i++) {
		if (sc->sc_out_pipe[i] != NULL)
			usbd_abort_pipe(sc->sc_out_pipe[i]);
	}

	sc->sc_tx_active = 0;
	sc->sc_tx_data_pipe = NULL;
	if (sc->sc_tx_ni != NULL) {
		ieee80211_release_node(ic, sc->sc_tx_ni);
		sc->sc_tx_ni = NULL;
	}
	sc->sc_key_tasks = 0;
}

static void
urtwx_start(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct mbuf *m;
	uint8_t qsel;

	if (!(ifp->if_flags & IFF_RUNNING) || sc->sc_tx_active) {
		if (sc->sc_tx_active)
			ifq_set_oactive(&ifp->if_snd);
		return;
	}

	for (;;) {
		if (sc->sc_tx_active) {
			ifq_set_oactive(&ifp->if_snd);
			break;
		}
		m = mq_dequeue(&ic->ic_mgtq);
		if (m != NULL) {
			ni = m->m_pkthdr.ph_cookie;
			if (ic->ic_state == IEEE80211_S_AUTH ||
			    ic->ic_state == IEEE80211_S_ASSOC) {
				if (urtwx_set_chan(sc, ic->ic_bss->ni_chan) != 0) {
					ieee80211_release_node(ic, ni);
					m_freem(m);
					ifp->if_oerrors++;
					continue;
				}
			}
			if (urtwx_tx(sc, m, ni, URTWX_TXDESC_QSEL_MGMT,
			    1) != 0) {
				ieee80211_release_node(ic, ni);
				ifp->if_oerrors++;
				continue;
			}
			sc->sc_tx_timer = 5;
			ifp->if_timer = 1;
			break;
		}
		if (ic->ic_state != IEEE80211_S_RUN ||
		    (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY) != 0)
			break;
		m = ifq_dequeue(&ifp->if_snd);
		if (m == NULL)
			break;
		if (m->m_len < sizeof(struct ether_header) &&
		    (m = m_pullup(m, sizeof(struct ether_header))) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
#if NBPFILTER > 0
		if (ifp->if_bpf != NULL)
			bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
#endif
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
#if NBPFILTER > 0
		if (ic->ic_rawbpf != NULL)
			bpf_mtap(ic->ic_rawbpf, m, BPF_DIRECTION_OUT);
#endif
		wh = mtod(m, struct ieee80211_frame *);
		qsel = urtwx_tx_data_qsel(wh);
		if (urtwx_tx(sc, m, ni, qsel, 0) != 0) {
			ieee80211_release_node(ic, ni);
			ifp->if_oerrors++;
			continue;
		}
		sc->sc_tx_timer = 5;
		ifp->if_timer = 1;
		break;
	}
}

static void
urtwx_watchdog(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;

	ifp->if_timer = 0;
	if (sc->sc_tx_timer > 0) {
		if (--sc->sc_tx_timer == 0) {
			printf("%s: device timeout\n", DEVNAME(sc));
			ifp->if_oerrors++;
			return;
		}
		ifp->if_timer = 1;
	}
	ieee80211_watchdog(ifp);
}

static int
urtwx_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct urtwx_softc *sc = ifp->if_softc;
	int s, error;

	if (usbd_is_dying(sc->sc_udev))
		return (ENXIO);

	usbd_ref_incr(sc->sc_udev);
	s = splnet();

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		/* FALLTHROUGH */
	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_flags & IFF_RUNNING))
				error = urtwx_init(ifp);
			else
				error = 0;
		} else {
			if (ifp->if_flags & IFF_RUNNING)
				urtwx_stop(ifp);
			error = 0;
		}
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
		if (error == ENETRESET) {
			if (ifp->if_flags & IFF_RUNNING) {
				/*
				 * Wireless-parameter ioctls commonly arrive
				 * in a burst around the initial up/join path.
				 * If we are not yet fully associated, tearing
				 * the device back down here just restarts scan
				 * underneath an in-flight state transition.
				 */
				if (sc->sc_ic.ic_state == IEEE80211_S_RUN) {
					urtwx_stop(ifp);
					error = urtwx_init(ifp);
				} else {
					error = 0;
				}
			} else
				error = 0;
		}
		break;
	}

	splx(s);
	usbd_ref_decr(sc->sc_udev);
	return (error);
}

static int
urtwx_txdma_queue_mapping(struct urtwx_softc *sc)
{
	const struct urtwx_rqpn *rqpn;
	uint16_t pq_map;
	int bulkout, ret;

	bulkout = urtwx_bulkout_num(sc);
	rqpn = &sc->sc_chip->uc_rqpn[bulkout];

	pq_map = BIT_TXDMA_HIQ_MAP(rqpn->dma_map_hi) |
	    BIT_TXDMA_MGQ_MAP(rqpn->dma_map_mg) |
	    BIT_TXDMA_BKQ_MAP(rqpn->dma_map_bk) |
	    BIT_TXDMA_BEQ_MAP(rqpn->dma_map_be) |
	    BIT_TXDMA_VIQ_MAP(rqpn->dma_map_vi) |
	    BIT_TXDMA_VOQ_MAP(rqpn->dma_map_vo);

	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP, 0, pq_map);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_CR, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_CR, 0,
	    MAC_TRX_ENABLE);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2CQ_CSR, 0,
	    BIT_H2CQ_FULL);
	if (ret != 0)
		return (ret);

	return (urtwx_write_1_mask(sc, REG_TXDMA_PQ_MAP, BIT_RXDMA_ARBBW_EN,
	    BIT_RXDMA_ARBBW_EN));
}

static int
urtwx_priority_queue_cfg(struct urtwx_softc *sc)
{
	const struct urtwx_page_table *pg;
	uint16_t pubq_num;
	int bulkout, ret;

	bulkout = urtwx_bulkout_num(sc);
	pg = &sc->sc_chip->uc_page_table[bulkout];
	pubq_num = urtwx_rsvd_boundary(sc) - pg->hq_num - pg->lq_num -
	    pg->nq_num - pg->exq_num - pg->gapq_num;

	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_1, 0,
	    pg->hq_num);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_2, 0,
	    pg->lq_num);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_3, 0,
	    pg->nq_num);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_4, 0,
	    pg->exq_num);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_INFO_5, 0,
	    pubq_num);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_RQPN_CTRL_2, BIT_LD_RQPN,
	    BIT_LD_RQPN);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2, 0,
	    urtwx_rsvd_boundary(sc));
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_FWHW_TXQ_CTRL + 2, 0x10, 0x10);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BCNQ_BDNY_V1, 0,
	    urtwx_rsvd_boundary(sc));
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2 + 2, 0,
	    urtwx_rsvd_boundary(sc));
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BCNQ1_BDNY_V1, 0,
	    urtwx_rsvd_boundary(sc));
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RXFF_BNDY, 0,
	    urtwx_rxff_size(sc) - URTWX_C2H_PKT_BUF - 1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_AUTO_LLT_V1, 0xf0,
	    URTWX_USB_TX_AGG_DESC_NUM << 4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_AUTO_LLT_V1 + 3, 0,
	    URTWX_USB_TX_AGG_DESC_NUM);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_TXDMA_OFFSET_CHK + 1, 0x02, 0x02);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1,
	    BIT_AUTO_INIT_LLT_V1);
	if (ret != 0)
		return (ret);
	ret = urtwx_wait_1(sc, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0,
	    URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);
	if (ret != 0)
		return (ret);

	return (urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR + 3, 0, 0));
}

static int
urtwx_init_h2c(struct urtwx_softc *sc)
{
	uint32_t value32, wp, rp, freeq;
	uint8_t value8;
	int ret;

	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_HEAD, 0);
	value32 = (value32 & 0xfffc0000U) | urtwx_h2cq_addr(sc);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2C_HEAD, 0, value32);
	if (ret != 0)
		return (ret);

	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_READ_ADDR, 0);
	value32 = (value32 & 0xfffc0000U) | urtwx_h2cq_addr(sc);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2C_READ_ADDR, 0,
	    value32);
	if (ret != 0)
		return (ret);

	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_TAIL, 0);
	value32 = (value32 & 0xfffc0000U) |
	    (urtwx_h2cq_addr(sc) + urtwx_h2cq_size(sc));
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2C_TAIL, 0, value32);
	if (ret != 0)
		return (ret);

	value8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_H2C_INFO, 0);
	value8 = (value8 & 0xfc) | 0x01;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_H2C_INFO, 0, value8);
	if (ret != 0)
		return (ret);

	value8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_H2C_INFO, 0);
	value8 = (value8 & 0xfb) | 0x04;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_H2C_INFO, 0, value8);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_1_mask(sc, REG_TXDMA_OFFSET_CHK + 1, 0x80, 0x80);
	if (ret != 0)
		return (ret);

	wp = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_PKT_WRITEADDR, 0) &
	    0x3ffff;
	rp = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_PKT_READADDR, 0) &
	    0x3ffff;
	if (wp >= rp)
		freeq = urtwx_h2cq_size(sc) - (wp - rp);
	else
		freeq = rp - wp;
	if (freeq != urtwx_h2cq_size(sc)) {
		DPRINTF(("%s: H2C queue mismatch wp=%#x rp=%#x free=%#x\n",
		    DEVNAME(sc), wp, rp, freeq));
	}
	sc->sc_h2c_box = 0;
	sc->sc_h2c_pkt_seq = 0;

	return (0);
}

static int
urtwx_mac_init_8821c(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_AMPDU_MAX_TIME_V1, 0,
	    URTWX_WLAN_AMPDU_MAX_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_TX_HANG_CTRL, BIT_EN_EOF_V1,
	    BIT_EN_EOF_V1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_PRECNT_CTRL, 0,
	    URTWX_WLAN_PRE_TXCNT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_PROT_MODE_CTRL, 0,
	    URTWX_WLAN_PROT_MODE_CTRL);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BAR_MODE_CTRL + 2, 0,
	    URTWX_WLAN_BAR_MODE_CTRL);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_FAST_EDCA_VOVI_SETTING,
	    0,
	    URTWX_FAST_EDCA_TH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ,
	    REG_FAST_EDCA_VOVI_SETTING + 2, 0,
	    URTWX_FAST_EDCA_TH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_FAST_EDCA_BEBK_SETTING,
	    0,
	    URTWX_FAST_EDCA_TH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ,
	    REG_FAST_EDCA_BEBK_SETTING + 2, 0,
	    URTWX_FAST_EDCA_TH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_INIRTS_RATE_SEL, 0x20, 0x20);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0,
	    0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_TXPAUSE, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_SLOT, 0,
	    URTWX_WLAN_SLOT_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_PIFS, 0,
	    URTWX_WLAN_PIFS_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_SIFS, 0,
	    URTWX_WLAN_SIFS_CFG);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_EDCA_VO_PARAM + 2, 0,
	    URTWX_WLAN_VO_TXOP_LIMIT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_EDCA_VI_PARAM + 2, 0,
	    URTWX_WLAN_VI_TXOP_LIMIT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RD_NAV_NXT, 0,
	    URTWX_WLAN_NAV_CFG);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_RXTSF_OFFSET_CCK, 0,
	    URTWX_WLAN_RX_TSF_CFG);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION,
	    BIT_EN_BCN_FUNCTION);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_TBTT_PROHIBIT, 0,
	    URTWX_WLAN_TBTT_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_DRVERLYINT, 0,
	    URTWX_WLAN_DRV_EARLY_INT);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCNDMATIM, 0,
	    URTWX_WLAN_BCN_DMA_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_TX_PTCL_CTRL + 1,
	    BIT_SIFS_BK_EN >> 8, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RXFLTMAP0, 0,
	    URTWX_WLAN_RX_FILTER0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_RXFLTMAP2, 0,
	    URTWX_WLAN_RX_FILTER2);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RCR, 0,
	    URTWX_WLAN_RCR_CFG);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_RX_PKT_LIMIT, 0,
	    URTWX_WLAN_RXPKT_MAX_SZ_512);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_TCR + 2, 0,
	    URTWX_WLAN_TX_FUNC_CFG2);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_TCR + 1, 0,
	    URTWX_WLAN_TX_FUNC_CFG1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_ACKTO_CCK, 0, 0x40);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_WMAC_TRXPTCL_CTL_H, 0x02, 0x02);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_SND_PTCL_CTRL,
	    BIT_DIS_CHK_VHTSIGB_CRC, BIT_DIS_CHK_VHTSIGB_CRC);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_WMAC_OPTION_FUNCTION_2,
	    0, URTWX_WLAN_MAC_OPT_FUNC2);
	if (ret != 0)
		return (ret);

	return (urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_WMAC_OPTION_FUNCTION_1,
	    0, URTWX_WLAN_MAC_OPT_NORM_FUNC1));
}

static int
urtwx_mac_init_8822c(struct urtwx_softc *sc)
{
	uint8_t value8;
	uint16_t value16;
	uint32_t value32;
	int ret;

	ret = urtwx_mac_init_8821c(sc);
	if (ret != 0)
		return (ret);

	value8 = urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_FWHW_TXQ_CTRL, 0);
	value8 |= 0x80;
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_FWHW_TXQ_CTRL, 0,
	    value8);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_FWHW_TXQ_CTRL + 1, 0,
	    URTWX_8822C_WLAN_TXQ_RPT_EN);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_SPEC_SIFS, 0,
	    URTWX_8822C_WLAN_SPEC_SIFS);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RESP_SIFS_CCK,
	    0, URTWX_8822C_WLAN_RESP_SIFS_CCK);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RESP_SIFS_OFDM,
	    0, URTWX_8822C_WLAN_RESP_SIFS_OFDM);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_DARFRC, 0,
	    URTWX_8822C_WLAN_DARFRC);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_DARFRCH, 0,
	    URTWX_8822C_WLAN_DARFRCH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RARFRCH, 0,
	    URTWX_8822C_WLAN_RARFRCH);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFR0, 0,
	    URTWX_8822C_WLAN_ARFR0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFRH0, 0,
	    URTWX_8822C_WLAN_ARFRH0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFR1_V1, 0,
	    URTWX_8822C_WLAN_ARFR1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFRH1_V1, 0,
	    URTWX_8822C_WLAN_ARFRH1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFR4, 0,
	    URTWX_8822C_WLAN_ARFR4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFRH4, 0,
	    URTWX_8822C_WLAN_ARFRH4);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFR5, 0,
	    URTWX_8822C_WLAN_ARFR5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ARFRH5, 0,
	    URTWX_8822C_WLAN_ARFRH5);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_PROT_MODE_CTRL, 0,
	    URTWX_8822C_WLAN_PROT_MODE_CTRL);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_LIFETIME_EN,
	    URTWX_8822C_BIT_BA_PARSER_EN, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_RRSR,
	    URTWX_8822C_BITS_RRSR_RSC, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_EDCA_BE_PARAM,
	    0, URTWX_8822C_WLAN_EDCA_BE_PARAM);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_EDCA_BK_PARAM,
	    0, URTWX_8822C_WLAN_EDCA_BK_PARAM);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_PIFS, 0,
	    URTWX_8822C_WLAN_PIFS_TIME);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_TX_PTCL_CTRL + 1,
	    BIT_SIFS_BK_EN >> 8, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, URTWX_8822C_REG_RD_CTRL + 1,
	    0x07, URTWX_8822C_RD_CTRL1);
	if (ret != 0)
		return (ret);
	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_AFE_CTRL1, 0);
	value32 &= ~BIT_MAC_CLK_SEL;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_AFE_CTRL1, 0, value32);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_USTIME_TSF, 0,
	    URTWX_MAC_CLK_SPEED);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_USTIME_EDCA, 0,
	    URTWX_MAC_CLK_SPEED);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, URTWX_8822C_REG_MISC_CTRL,
	    URTWX_8822C_MISC_CTRL, URTWX_8822C_MISC_CTRL);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_BCN_CTRL_CLINT0,
	    0, URTWX_8822C_WLAN_BCN_CTRL_CLT0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_BCN_MAX_ERR, 0,
	    URTWX_8822C_WLAN_BCN_MAX_ERR);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_MAR, 0,
	    URTWX_8822C_WLAN_MULTI_ADDR);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_MAR + 4, 0,
	    URTWX_8822C_WLAN_MULTI_ADDR);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_BBPSF_CTRL + 2,
	    0, URTWX_8822C_WLAN_RESP_TXRATE);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_ACKTO, 0,
	    URTWX_8822C_WLAN_ACKTO);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_ACKTO_CCK, 0,
	    URTWX_8822C_WLAN_ACKTO_CCK);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_EIFS, 0,
	    URTWX_8822C_WLAN_EIFS);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_NAV_CTRL + 2, 0,
	    URTWX_8822C_WLAN_NAV_MAX);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_WMAC_TRXPTCL_CTL_H + 2, 0,
	    URTWX_8822C_BAR_ACK_TYPE);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, URTWX_8822C_REG_GENERAL_OPTION,
	    URTWX_8822C_GENERAL_OPTION, URTWX_8822C_GENERAL_OPTION);
	if (ret != 0)
		return (ret);
	value16 = urtwx_read_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXPSF_CTRL + 2,
	    0);
	value16 &= URTWX_8822C_RXPSF_CTRL2_MASK;
	value16 |= (uint16_t)(URTWX_8822C_RXPSF_CTRL2_VAL >> 16);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXPSF_CTRL + 2,
	    0, value16);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXPSF_CTRL, 0,
	    URTWX_8822C_RXPSF_CTRL_VAL);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ,
	    URTWX_8822C_REG_RXPSF_TYPE_CTRL, 0, 0xffffffffU);
	if (ret != 0)
		return (ret);
	value16 = urtwx_read_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXPSF_CTRL, 0);
	value16 &= ~URTWX_8822C_RXPSF_CTRL_MASK;
	value16 |= URTWX_8822C_RXPSF_CTRL_FIXUP;
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_RXPSF_CTRL, 0,
	    value16);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_8822C_REG_INT_MIG, 0,
	    URTWX_8822C_WLAN_MAC_INT_MIG);
	if (ret != 0)
		return (ret);
	ret = urtwx_8822c_coex_cfg_init(sc);
	if (ret != 0)
		return (ret);

	return (urtwx_write_1_mask(sc, REG_SND_PTCL_CTRL,
	    BIT_DIS_CHK_VHTSIGB_CRC, BIT_DIS_CHK_VHTSIGB_CRC));
}

static int
urtwx_8822c_coex_cfg_init(struct urtwx_softc *sc)
{
	int ret;

	/*
	 * Minimal Wi-Fi-side front-end/PTA ownership setup from Linux
	 * rtw8822c_coex_cfg_init(). This is not full BT coexistence;
	 * it just puts the shared front-end into the expected WLAN state.
	 */
	ret = urtwx_write_1_mask(sc, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION,
	    BIT_EN_BCN_FUNCTION);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_BT_TDMA_TIME, BIT_MASK_SAMPLE_RATE,
	    0x05);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BT_STAT_CTRL, 0, 0x01);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_GPIO_MUXCFG, BIT_BT_PTA_EN,
	    BIT_BT_PTA_EN);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_GPIO_MUXCFG, BIT_PO_BT_PTA_PINS,
	    BIT_PO_BT_PTA_PINS);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_QUEUE_CTRL, BIT_PTA_WL_TX_EN,
	    BIT_PTA_WL_TX_EN);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_QUEUE_CTRL, BIT_PTA_EDCCA_EN, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2_mask(sc, REG_BT_COEX_V2, BIT_GNT_BT_POLARITY,
	    BIT_GNT_BT_POLARITY);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_DUMMY_PAGE4_V1, BIT_BTCCA_CTRL, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf_path(sc, 1, RF_MODOPT, RFREG_MASK, 0x40000);
	if (ret != 0)
		return (ret);

	return (urtwx_8822c_wifi_only_coex(sc));
}

static int
urtwx_rx_set_dma(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_RX_DRVINFO_SZ, 0,
	    URTWX_PHY_STATUS_SIZE);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_RCR, BIT_APP_PHYSTS,
	    BIT_APP_PHYSTS);
	if (ret != 0)
		return (ret);

	return (urtwx_write_4_mask(sc, REG_WMAC_OPTION_FUNCTION_1,
	    0x00000300U, 0));
}

static int
urtwx_usb_interface_cfg(struct urtwx_softc *sc)
{
	uint8_t rxdma;
	int ret;

	rxdma = BIT_DMA_MODE | BIT_DMA_BURST_CNT;
	switch (sc->sc_udev->speed) {
	case USB_SPEED_SUPER:
		rxdma |= BIT_DMA_BURST_SIZE_1024;
		break;
	case USB_SPEED_HIGH:
		rxdma |= BIT_DMA_BURST_SIZE_512;
		break;
	default:
		rxdma |= BIT_DMA_BURST_SIZE_64;
		break;
	}

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_RXDMA_MODE, 0, rxdma);
	if (ret != 0)
		return (ret);

	return (urtwx_write_2_mask(sc, REG_TXDMA_OFFSET_CHK,
	    BIT_DROP_DATA_EN, BIT_DROP_DATA_EN));
}

static int
urtwx_usb_rx_agg_cfg(struct urtwx_softc *sc)
{
	uint16_t val16;
	int ret;

	ret = urtwx_write_1_mask(sc, REG_TXDMA_PQ_MAP, BIT_RXDMA_AGG_EN,
	    BIT_RXDMA_AGG_EN);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_RXDMA_AGG_PG_TH + 3, 0x80, 0);
	if (ret != 0)
		return (ret);

	val16 = URTWX_RX_AGG_SIZE |
	    ((uint16_t)URTWX_RX_AGG_TIMEOUT << 8);
	return (urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_RXDMA_AGG_PG_TH, 0,
	    val16));
}

static int
urtwx_mac_init_runtime(struct urtwx_softc *sc)
{
	int ret;

	sc->sc_chip_version = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_SYS_CFG1, 0);
	sc->sc_cut_version = BIT_GET_CHIP_VER(sc->sc_chip_version);
	if (sc->sc_chip == &urtwx_chip_8822c) {
		printf("%s: chipcfg1=%#x cut=%u\n", DEVNAME(sc),
		    sc->sc_chip_version, sc->sc_cut_version);
	}

	ret = urtwx_txdma_queue_mapping(sc);
	if (ret != 0) {
		printf("%s: txdma queue mapping failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_priority_queue_cfg(sc);
	if (ret != 0) {
		printf("%s: priority queue cfg failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_init_h2c(sc);
	if (ret != 0) {
		printf("%s: h2c init failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = sc->sc_chip->uc_mac_init(sc);
	if (ret != 0) {
		printf("%s: chip mac init failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_phy_init_runtime(sc);
	if (ret != 0) {
		printf("%s: phy init runtime failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_rx_set_dma(sc);
	if (ret != 0) {
		printf("%s: rx dma setup failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	ret = urtwx_usb_interface_cfg(sc);
	if (ret != 0) {
		printf("%s: usb interface cfg failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}
	if (sc->sc_chip == &urtwx_chip_8822c) {
		ret = urtwx_fw_send_general_info(sc);
		if (ret != 0) {
			printf("%s: general info h2c failed %d\n", DEVNAME(sc),
			    ret);
			return (ret);
		}
		ret = urtwx_fw_send_phydm_info(sc);
		if (ret != 0) {
			printf("%s: phydm info h2c failed %d\n", DEVNAME(sc),
			    ret);
			return (ret);
		}
	}
	ret = urtwx_sec_enable(sc);
	if (ret != 0) {
		printf("%s: sec enable failed %d\n", DEVNAME(sc), ret);
		return (ret);
	}

	ret = urtwx_usb_rx_agg_cfg(sc);
	if (ret != 0)
		printf("%s: usb rx agg cfg failed %d\n", DEVNAME(sc), ret);
	return (ret);
}

int
urtwx_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa;

	uaa = aux;
	if (uaa->iface == NULL || uaa->configno != 1)
		return (UMATCH_NONE);

	if (urtwx_lookup_devspec(uaa->vendor, uaa->product) == NULL)
		return (UMATCH_NONE);

	return (UMATCH_VENDOR_PRODUCT_CONF_IFACE);
}

void
urtwx_attach(struct device *parent, struct device *self, void *aux)
{
	struct urtwx_softc		*sc;
	struct ieee80211com		*ic;
	struct ifnet			*ifp;
	struct usb_attach_arg		*uaa;
	const struct urtwx_devspec	*devspec;
	usb_interface_descriptor_t	*id;
	usb_endpoint_descriptor_t	*ed;
	usbd_status			 err;
	int				 i, ifp_attached;

	sc = (struct urtwx_softc *)self;
	uaa = aux;
	devspec = urtwx_lookup_devspec(uaa->vendor, uaa->product);
	ic = &sc->sc_ic;
	ifp = &ic->ic_if;
	ifp_attached = 0;

	if (devspec == NULL) {
		printf("%s: unsupported device %#x:%#x\n", DEVNAME(sc),
		    uaa->vendor, uaa->product);
		return;
	}

	sc->sc_udev = uaa->device;
	sc->sc_iface = uaa->iface;
	sc->sc_chip = devspec->ud_chip;
	sc->sc_nbulkout = 0;
	usb_init_task(&sc->sc_task, urtwx_task, sc, USB_TASK_TYPE_GENERIC);
	timeout_set(&sc->sc_scan_to, urtwx_scan_to, sc);
	sc->sc_cmdq.cur = 0;
	sc->sc_cmdq.next = 0;
	sc->sc_cmdq.queued = 0;
	sc->sc_have_macaddr = 0;
	sc->sc_fw_loaded = 0;

	id = usbd_get_interface_descriptor(sc->sc_iface);
	for (i = 0; i < id->bNumEndpoints; i++) {
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (ed == NULL) {
			printf("%s: missing endpoint descriptor %d\n",
			    DEVNAME(sc), i);
			return;
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			if (sc->sc_ed[URTWX_ENDPT_RX] == 0)
				sc->sc_ed[URTWX_ENDPT_RX] = ed->bEndpointAddress;
			else
				printf("%s: ignoring extra bulk-in endpoint %#x\n",
				    DEVNAME(sc), ed->bEndpointAddress);
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			if (sc->sc_nbulkout >= URTWX_MAX_OUT_PIPES) {
				printf("%s: too many bulk out endpoints\n",
				    DEVNAME(sc));
				return;
			}
			sc->sc_out_ed[sc->sc_nbulkout++] =
			    ed->bEndpointAddress;
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
			sc->sc_ed[URTWX_ENDPT_INTR] = ed->bEndpointAddress;
		}
	}

	if (sc->sc_ed[URTWX_ENDPT_RX] == 0 ||
	    sc->sc_nbulkout == 0 ||
	    sc->sc_ed[URTWX_ENDPT_INTR] == 0) {
		printf("%s: missing endpoints rx=%#x out=%d intr=%#x\n",
		    DEVNAME(sc), sc->sc_ed[URTWX_ENDPT_RX],
		    sc->sc_nbulkout, sc->sc_ed[URTWX_ENDPT_INTR]);
		return;
	}
	printf("%s: endpoints rx=%#x intr=%#x out=%d", DEVNAME(sc),
	    sc->sc_ed[URTWX_ENDPT_RX], sc->sc_ed[URTWX_ENDPT_INTR],
	    sc->sc_nbulkout);
	for (i = 0; i < sc->sc_nbulkout; i++)
		printf(" out%d=%#x", i, sc->sc_out_ed[i]);
	printf("\n");

	err = usbd_open_pipe(sc->sc_iface, sc->sc_ed[URTWX_ENDPT_RX],
	    USBD_EXCLUSIVE_USE, &sc->sc_ep[URTWX_ENDPT_RX]);
	if (err != USBD_NORMAL_COMPLETION) {
		printf("%s: could not open rx pipe: %s\n", DEVNAME(sc),
		    usbd_errstr(err));
		return;
	}

	for (i = 0; i < sc->sc_nbulkout; i++) {
		err = usbd_open_pipe(sc->sc_iface, sc->sc_out_ed[i],
		    USBD_EXCLUSIVE_USE, &sc->sc_out_pipe[i]);
		if (err != USBD_NORMAL_COMPLETION) {
			printf("%s: could not open tx pipe %d: %s\n",
			    DEVNAME(sc), i, usbd_errstr(err));
			goto fail;
		}
	}

	if (urtwx_alloc_rx_list(sc) != 0) {
		printf("%s: could not allocate rx buffers\n", DEVNAME(sc));
		goto fail;
	}

	sc->sc_tx_xfer = usbd_alloc_xfer(sc->sc_udev);
	if (sc->sc_tx_xfer == NULL) {
		printf("%s: could not allocate tx xfer\n", DEVNAME(sc));
		goto fail;
	}
	sc->sc_tx_buf = usbd_alloc_buffer(sc->sc_tx_xfer,
	    URTWX_TXDESC_SIZE + URTWX_MAX_FW_CHUNK + 1);
	if (sc->sc_tx_buf == NULL) {
		printf("%s: could not allocate tx buffer\n", DEVNAME(sc));
		goto fail;
	}

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_max_rssi = 100;
	ic->ic_htcaps = sc->sc_chip->uc_htcaps;
	urtwx_build_sup_mcs(ic->ic_sup_mcs, sizeof(ic->ic_sup_mcs),
	    sc->sc_chip->uc_tx_nss, sc->sc_chip->uc_ht_mcs_max);
	ic->ic_vhtcaps = sc->sc_chip->uc_vhtcaps;
	ic->ic_vht_rxmcs = urtwx_vht_mcs_map(sc->sc_chip->uc_tx_nss,
	    sc->sc_chip->uc_vht_mcs_max);
	ic->ic_vht_txmcs = urtwx_vht_mcs_map(sc->sc_chip->uc_tx_nss,
	    sc->sc_chip->uc_vht_mcs_max);
	ic->ic_vht_rx_max_lgi_mbit_s = sc->sc_chip->uc_vht_rx_max_lgi_mbit_s;
	ic->ic_vht_tx_max_lgi_mbit_s = sc->sc_chip->uc_vht_tx_max_lgi_mbit_s;
	ic->ic_caps = sc->sc_chip->uc_ic_caps;
	ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 1; i <= 14; i++) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ |
		    IEEE80211_CHAN_HT;
		if (i != 14)
			ic->ic_channels[i].ic_flags |= IEEE80211_CHAN_40MHZ;
	}
	for (i = 36; i <= 48; i += 4) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_A | IEEE80211_CHAN_HT |
		    IEEE80211_CHAN_VHT |
		    IEEE80211_CHAN_40MHZ;
		ic->ic_channels[i].ic_xflags |= IEEE80211_CHANX_80MHZ;
	}
	for (i = 149; i <= 161; i += 4) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_A | IEEE80211_CHAN_HT |
		    IEEE80211_CHAN_VHT |
		    IEEE80211_CHAN_40MHZ;
		ic->ic_channels[i].ic_xflags |= IEEE80211_CHANX_80MHZ;
	}
	ic->ic_channels[165].ic_freq =
	    ieee80211_ieee2mhz(165, IEEE80211_CHAN_5GHZ);
	ic->ic_channels[165].ic_flags =
	    IEEE80211_CHAN_A | IEEE80211_CHAN_HT;
	ic->ic_ibss_chan = &ic->ic_channels[1];

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = urtwx_ioctl;
	ifp->if_start = urtwx_start;
	ifp->if_watchdog = urtwx_watchdog;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);
	ether_fakeaddr(ifp);
	IEEE80211_ADDR_COPY(ic->ic_myaddr,
	    ((struct arpcom *)ifp)->ac_enaddr);

	if_attach(ifp);
	ifp_attached = 1;
	ieee80211_ifattach(ifp);
	ic->ic_set_key = urtwx_set_key;
	ic->ic_delete_key = urtwx_delete_key;
	ic->ic_ampdu_rx_start = urtwx_ampdu_rx_start;
	ic->ic_ampdu_rx_stop = urtwx_ampdu_rx_stop;
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = urtwx_newstate;
	ieee80211_media_init(ifp, urtwx_media_change,
	    ieee80211_media_status);
	return;

fail:
	if (ifp_attached) {
		ieee80211_ifdetach(ifp);
		if_detach(ifp);
		ifp->if_softc = NULL;
	}
	urtwx_free_rx_list(sc);
	if (sc->sc_tx_xfer != NULL) {
		usbd_free_xfer(sc->sc_tx_xfer);
		sc->sc_tx_xfer = NULL;
		sc->sc_tx_buf = NULL;
	}
	urtwx_close_pipes(sc);
}

int
urtwx_detach(struct device *self, int flags)
{
	struct urtwx_softc *sc;
	struct ifnet *ifp;
	int i, s;

	sc = (struct urtwx_softc *)self;
	ifp = &sc->sc_ic.ic_if;

	timeout_del(&sc->sc_scan_to);
	usb_rem_wait_task(sc->sc_udev, &sc->sc_task);

	if (sc->sc_ep[URTWX_ENDPT_RX] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_RX]);
	for (i = 0; i < sc->sc_nbulkout; i++) {
		if (sc->sc_out_pipe[i] != NULL)
			usbd_abort_pipe(sc->sc_out_pipe[i]);
	}

	s = splusb();
	usbd_ref_wait(sc->sc_udev);
	splx(s);

	if (ifp->if_softc != NULL) {
		ieee80211_ifdetach(ifp);
		if_detach(ifp);
		ifp->if_softc = NULL;
	}
	urtwx_free_rx_list(sc);
	if (sc->sc_tx_xfer != NULL) {
		usbd_free_xfer(sc->sc_tx_xfer);
		sc->sc_tx_xfer = NULL;
		sc->sc_tx_buf = NULL;
	}
	urtwx_close_pipes(sc);

	return (0);
}
