/*	$OpenBSD$	*/

/*-
 * Register and firmware definitions for Realtek RTL8821AU/RTL8811AU.
 * Values are taken from Linux rtw88 (rtw8821a/reg/fw headers).
 */

#ifndef _IF_URTWAREG_H_
#define _IF_URTWAREG_H_

#ifndef BIT
#define BIT(n)				(1U << (n))
#endif

#define URTWA_USB_CMD_REQ		0x05
#define URTWA_USB_VENQT_IDX		0x0000

#define URTWA_FW_NAME			"rtw8821a_fw.bin"
#define URTWA_FW_SIGNATURE		0x2101

#define URTWA_FW_START_ADDR_LEGACY	0x1000
#define URTWA_DLFW_PAGE_SIZE_LEGACY	0x1000
#define URTWA_DLFW_PAGE_SIZE_SHIFT	12
#define URTWA_DLFW_BLK_SIZE_LEGACY	4
#define URTWA_LLT_LAST_ENTRY		255

#define REG_SYS_FUNC_EN			0x0002
#define REG_APS_FSMCO			0x0004
#define APS_FSMCO_MAC_ENABLE		BIT(8)
#define APS_FSMCO_MAC_OFF		BIT(9)
#define REG_SYS_CLKR			0x0008
#define BIT_ANA8M			BIT(1)
#define BIT_LOADER_CLK_EN		BIT(5)
#define REG_RSV_CTRL			0x001c
#define REG_RF_CTRL			0x001f
#define BIT_FEN_USBA			BIT(2)
#define BIT_FEN_BB_GLB_RST		BIT(1)
#define BIT_FEN_BB_RSTB			BIT(0)
#define BIT_RF_SDM_RSTB			BIT(2)
#define BIT_RF_RSTB			BIT(1)
#define BIT_RF_EN			BIT(0)
#define REG_AFE_CTRL1			0x0024
#define REG_AFE_CTRL3			0x002c
#define REG_EFUSE_CTRL			0x0030
#define BIT_EF_FLAG			BIT(31)
#define BIT_SHIFT_EF_ADDR		8
#define BIT_MASK_EF_ADDR		0x3ff
#define BIT_MASK_EF_DATA		0xff
#define BITS_EF_ADDR			(BIT_MASK_EF_ADDR << BIT_SHIFT_EF_ADDR)
#define REG_LDO_EFUSE_CTRL		0x0034
#define BIT_MASK_EFUSE_BANK_SEL		(BIT(8) | BIT(9))
#define REG_WL_BT_PWR_CTRL		0x0068
#define REG_RF_B_CTRL			0x0076
#define REG_SYS_SDIO_CTRL		0x0070
#define REG_LDO_SWR_CTRL		0x007c
#define REG_MCUFW_CTRL			0x0080
#define REG_SYS_CFG1			0x00f0
#define REG_LED_CFG			0x004c
#define BIT_DPDT_SEL_EN			BIT(23)
#define BIT_DPDT_WL_SEL			BIT(24)
#define REG_EFUSE_ACCESS		0x00cf
#define REG_CR				0x0100
#define BIT_MACRXEN			BIT(7)
#define BIT_MACTXEN			BIT(6)
#define REG_TXDMA_PQ_MAP		0x010c
#define BIT_SHIFT_TXDMA_VOQ_MAP		4
#define BIT_MASK_TXDMA_VOQ_MAP		0x3
#define BIT_TXDMA_VOQ_MAP(x)		(((x) & BIT_MASK_TXDMA_VOQ_MAP) << BIT_SHIFT_TXDMA_VOQ_MAP)
#define BIT_SHIFT_TXDMA_VIQ_MAP		6
#define BIT_MASK_TXDMA_VIQ_MAP		0x3
#define BIT_TXDMA_VIQ_MAP(x)		(((x) & BIT_MASK_TXDMA_VIQ_MAP) << BIT_SHIFT_TXDMA_VIQ_MAP)
#define BIT_SHIFT_TXDMA_BEQ_MAP		8
#define BIT_MASK_TXDMA_BEQ_MAP		0x3
#define BIT_TXDMA_BEQ_MAP(x)		(((x) & BIT_MASK_TXDMA_BEQ_MAP) << BIT_SHIFT_TXDMA_BEQ_MAP)
#define BIT_SHIFT_TXDMA_BKQ_MAP		10
#define BIT_MASK_TXDMA_BKQ_MAP		0x3
#define BIT_TXDMA_BKQ_MAP(x)		(((x) & BIT_MASK_TXDMA_BKQ_MAP) << BIT_SHIFT_TXDMA_BKQ_MAP)
#define BIT_SHIFT_TXDMA_MGQ_MAP		12
#define BIT_MASK_TXDMA_MGQ_MAP		0x3
#define BIT_TXDMA_MGQ_MAP(x)		(((x) & BIT_MASK_TXDMA_MGQ_MAP) << BIT_SHIFT_TXDMA_MGQ_MAP)
#define BIT_SHIFT_TXDMA_HIQ_MAP		14
#define BIT_MASK_TXDMA_HIQ_MAP		0x3
#define BIT_TXDMA_HIQ_MAP(x)		(((x) & BIT_MASK_TXDMA_HIQ_MAP) << BIT_SHIFT_TXDMA_HIQ_MAP)
#define REG_TRXFF_BNDY			0x0114
#define REG_HMETFR			0x01cc
#define REG_HMEBOX0			0x01d0
#define REG_HMEBOX1			0x01d4
#define REG_HMEBOX2			0x01d8
#define REG_HMEBOX3			0x01dc
#define REG_LLT_INIT			0x01e0
#define REG_HMEBOX0_EX			0x01f0
#define REG_HMEBOX1_EX			0x01f4
#define REG_HMEBOX2_EX			0x01f8
#define REG_HMEBOX3_EX			0x01fc
#define REG_RQPN			0x0200
#define BIT_MASK_HPQ			0xff
#define BIT_SHIFT_HPQ			0
#define BIT_RQPN_HPQ(x)			(((x) & BIT_MASK_HPQ) << BIT_SHIFT_HPQ)
#define BIT_MASK_LPQ			0xff
#define BIT_SHIFT_LPQ			8
#define BIT_RQPN_LPQ(x)			(((x) & BIT_MASK_LPQ) << BIT_SHIFT_LPQ)
#define BIT_MASK_PUBQ			0xff
#define BIT_SHIFT_PUBQ			16
#define BIT_RQPN_PUBQ(x)		(((x) & BIT_MASK_PUBQ) << BIT_SHIFT_PUBQ)
#define BIT_LD_RQPN			BIT(31)
#define BIT_RQPN_HLP(h, l, p)		(BIT_LD_RQPN | BIT_RQPN_HPQ(h) | BIT_RQPN_LPQ(l) | BIT_RQPN_PUBQ(p))
#define REG_TXDMA_OFFSET_CHK		0x020c
#define REG_DWBCN0_CTRL			0x0208
#define REG_RQPN_NPQ			0x0214
#define BIT_MASK_NPQ			0xff
#define BIT_SHIFT_NPQ			0
#define BIT_MASK_EPQ			0xff
#define BIT_SHIFT_EPQ			16
#define BIT_RQPN_NPQ(x)			(((x) & BIT_MASK_NPQ) << BIT_SHIFT_NPQ)
#define BIT_RQPN_EPQ(x)			(((x) & BIT_MASK_EPQ) << BIT_SHIFT_EPQ)
#define BIT_RQPN_NE(n, e)		(BIT_RQPN_NPQ(n) | BIT_RQPN_EPQ(e))
#define REG_DWBCN1_CTRL			0x0228
#define REG_EARLY_MODE_CONTROL		0x02bc
#define REG_ACLK_MON			0x003e
#define REG_RXDMA_STATUS		0x0288
#define REG_HIQ_NO_LMT_EN		0x05a7
#define REG_FPGA0_XCD_RF_PARA		0x08b4
#define REG_PIFS			0x0512
#define REG_RX_DRVINFO_SZ		0x060f
#define REG_RX_PKT_LIMIT		0x060c
#define REG_MAR				0x0620
#define REG_USTIME_EDCA			0x0638
#define REG_MAC_SPEC_SIFS		0x063a
#define REG_ACKTO			0x0640
#define REG_NAV_CTRL			0x0650
#define REG_WMAC_TRXPTCL_CTL		0x0668
#define BIT_RFMOD			(BIT(7) | BIT(8))
#define BIT_RFMOD_80M			BIT(8)
#define BIT_RFMOD_40M			BIT(7)
#define REG_RXFLTMAP0			0x06a0
#define REG_RXFLTMAP1			0x06a2
#define REG_RXFLTMAP2			0x06a4
#define REG_WMAC_OPTION_FUNCTION_1	0x07d4
#define REG_FPGA0_RFMOD			0x0800
#define BIT_CCKEN			BIT(24)
#define BIT_OFDMEN			BIT(25)
#define REG_CCK_RPT_FORMAT		0x0804
#define BIT_CCK_RPT_FORMAT		BIT(16)
#define REG_RXPSEL			0x0808
#define BIT_RX_PSEL_RST			(BIT(28) | BIT(29))
#define REG_TXPSEL			0x080c
#define REG_CCASEL			0x082c
#define REG_PDMFTH			0x0830
#define REG_BWINDICATION		0x0834
#define REG_CCA2ND			0x0838
#define REG_L1PKTH			0x0848
#define REG_CLKTRK			0x0860
#define REG_ADCCLK			0x08ac
#define REG_HSSI_READ			0x08b0
#define REG_ADC160			0x08c4
#define REG_RXSB			0x0a00
#define REG_CCK_RX			0x0a04
#define REG_DATA_SC			0x0483
#define BIT_TXSC_40M(x)			(((x) & 0xf) << 4)
#define BIT_TXSC_20M(x)			((x) & 0xf)
#define REG_TXSCALE_A			0x0c1c
#define REG_RFE_PINMUX_A		0x0cb0
#define REG_RFE_INV_A			0x0cb4
#define REG_3WIRE_SWA			0x0c00
#define REG_3WIRE_SWB			0x0e00
#define REG_LSSI_WRITE_A		0x0c90
#define REG_LSSI_WRITE_B		0x0e90
#define REG_PI_READ_A			0x0d04
#define REG_SI_READ_A			0x0d08
#define REG_PI_READ_B			0x0f04
#define REG_SI_READ_B			0x0f08
#define REG_CCK_CHECK			0x0454
#define BIT_CHECK_CCK_EN		BIT(7)
#define REG_AMPDU_MAX_TIME		0x0456
#define REG_AMPDU_MAX_LENGTH		0x0458
#define REG_FAST_EDCA_CTRL		0x0460
#define REG_QUEUE_CTRL			0x04c6
#define REG_SINGLE_AMPDU_CTRL		0x04c7
#define BIT_EN_SINGLE_APMDU		BIT(7)
#define REG_MAX_AGGR_NUM		0x04ca
#define REG_BAR_MODE_CTRL		0x04cc
#define REG_TX_RPT_TIME			0x04f0
#define REG_USB_HRPWM			0xfe58
#define REG_FWHW_TXQ_CTRL		0x0420
#define REG_HWSEQ_CTRL			0x0423
#define REG_BCNQ_BDNY			0x0424
#define REG_MGQ_BDNY			0x0425
#define REG_SPEC_SIFS			0x0428
#define REG_RETRY_LIMIT			0x042a
#define REG_RRSR			0x0440
#define REG_WMAC_LBK_BF_HD		0x045d
#define REG_EDCA_VO_PARAM		0x0500
#define REG_EDCA_VI_PARAM		0x0504
#define REG_EDCA_BE_PARAM		0x0508
#define REG_EDCA_BK_PARAM		0x050c
#define REG_BCNTCFG			0x0510
#define REG_SIFS			0x0514
#define REG_TBTT_PROHIBIT		0x0540
#define REG_BCN_CTRL			0x0550
#define REG_DRVERLYINT			0x0558
#define REG_BCNDMATIM			0x0559
#define REG_USTIME_TSF			0x055c
#define REG_BCN_MAX_ERR			0x055d
#define REG_MACID			0x0610
#define REG_BSSID			0x0618
#define REG_AID				0x06a8
#define REG_HIMR0			0x00b0
#define REG_HIMR1			0x00b8
#define REG_USB3_RXITV			0xf050
#define REG_USB_MOD			0xf008

#define BIT_FEN_CPUEN			(1U << 2)
#define BIT_FEN_ELDR			(1U << 12)
#define BIT_WLMCU_IOIF			(1U << 0)
#define BIT_LDO				(1U << 24)

#define BIT_HCI_TXDMA_EN		(1U << 0)
#define BIT_HCI_RXDMA_EN		(1U << 1)
#define BIT_TXDMA_EN			(1U << 2)
#define BIT_RXDMA_EN			(1U << 3)
#define BIT_PROTOCOL_EN			(1U << 4)
#define BIT_SCHEDULE_EN			(1U << 5)
#define BIT_MAC_SEC_EN			(1U << 9)
#define BIT_32K_CAL_TMR_EN		(1U << 10)

#define BIT_ROM_DLEN			(1U << 19)
#define BIT_ROM_PGE			(0x7U << 16)
#define BIT_SHIFT_ROM_PGE		16
#define BIT_RAM_DL_SEL			(1U << 7)
#define BIT_WINTINI_RDY			(1U << 6)
#define BIT_FWDL_CHK_RPT		(1U << 2)
#define BIT_MCUFWDL_RDY			(1U << 1)
#define BIT_MCUFWDL_EN			(1U << 0)

#define BIT_LLT_WRITE_ACCESS		(1U << 30)
#define BIT_DROP_DATA_EN		(1U << 9)
#define BIT_DIS_TSF_UDT			(1U << 4)
#define BIT_EN_BCN_FUNCTION		(1U << 3)

#define MASKBYTE0			0x000000ffU
#define MASKDWORD			0xffffffffU
#define RFREG_MASK			0x000fffffU

#define FW_READY_LEGACY \
	(BIT_MCUFWDL_RDY | BIT_FWDL_CHK_RPT | BIT_WINTINI_RDY | BIT_RAM_DL_SEL)

#define EFUSE_ACCESS_ON			0x69
#define EFUSE_ACCESS_OFF		0x00

#define LDO_SEL				0xc3
#define SPS_SEL				0x83

#define URTWA_TRX_ENABLE \
	(BIT_HCI_TXDMA_EN | BIT_HCI_RXDMA_EN | BIT_TXDMA_EN | BIT_RXDMA_EN | \
	 BIT_PROTOCOL_EN | BIT_SCHEDULE_EN | BIT_MAC_SEC_EN | \
	 BIT_32K_CAL_TMR_EN)

#define URTWA_PWR_INTF_USB_MSK		(1U << 1)
#define URTWA_PWR_INTF_SDIO_MSK		(1U << 0)
#define URTWA_PWR_INTF_ALL_MSK		0xff
#define URTWA_PWR_CUT_ALL_MSK		0xff

#define URTWA_PWR_CMD_READ		0x00
#define URTWA_PWR_CMD_WRITE		0x01
#define URTWA_PWR_CMD_POLLING		0x02
#define URTWA_PWR_CMD_DELAY		0x03
#define URTWA_PWR_CMD_END		0x04

#define URTWA_PWR_ADDR_MAC		0x00
#define URTWA_PWR_ADDR_USB		0x01
#define URTWA_PWR_ADDR_PCIE		0x02
#define URTWA_PWR_ADDR_SDIO		0x03

#define URTWA_PWR_DELAY_US		0
#define URTWA_PWR_DELAY_MS		1

#define URTWA_PWR_POLLING_CNT		20000

#define RF_CFGCH			0x18
#define RF18_RFSI_MASK			(BIT(18) | BIT(17))
#define RF18_BAND_MASK			(BIT(16) | BIT(9) | BIT(8))
#define RF18_CHANNEL_MASK		MASKBYTE0
#define RF18_BW_MASK			(BIT(11) | BIT(10))

struct urtwa_pwr_seq_cmd {
	uint16_t	offset;
	uint8_t		cut_mask;
	uint8_t		intf_mask;
	uint8_t		base:4;
	uint8_t		cmd:4;
	uint8_t		mask;
	uint8_t		value;
} __packed;

struct urtwa_fw_hdr_legacy {
	uint16_t	signature;
	uint8_t		category;
	uint8_t		function;
	uint16_t	version;
	uint8_t		subversion1;
	uint8_t		subversion2;
	uint8_t		month;
	uint8_t		day;
	uint8_t		hour;
	uint8_t		minute;
	uint16_t	size;
	uint16_t	rsvd2;
	uint32_t	idx;
	uint32_t	rsvd3;
	uint32_t	rsvd4;
	uint32_t	rsvd5;
} __packed;

#endif
