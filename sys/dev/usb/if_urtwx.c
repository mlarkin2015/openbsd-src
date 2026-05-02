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
#define URTWX_TXDESC_CSUM_WORDS		16
#define URTWX_TXDESC_CSUM_IDX		14
#define URTWX_TXDESC_QSEL_BE		0
#define URTWX_TXDESC_QSEL_BEACON	16
#define URTWX_TXDESC_QSEL_HIGH		17
#define URTWX_TXDESC_QSEL_MGMT		18
#define URTWX_TXDESC_QSEL_H2C		19
#define URTWX_TX_TIMEOUT		1000
#define URTWX_SCAN_DWELL_TIME		200
#define URTWX_HOST_CMD_RING_COUNT	8
#define URTWX_FORCE_PASSIVE_SCAN	0
#define URTWX_SCAN_LOG_MAX		32
#define URTWX_TX_MGMT_LOG_MAX		16
#define URTWX_TXDESC_W0_BMC		0x01000000U
#define URTWX_TXDESC_W0_LS		0x04000000U
#define URTWX_TXDESC_W0_DISQSELSEQ	0x80000000U
#define URTWX_TXDESC_W1_QSEL_SHIFT	8
#define URTWX_TXDESC_W1_RATEID_SHIFT	16
#define URTWX_TXDESC_W3_USE_RATE	0x00000100U
#define URTWX_TXDESC_W3_DISDATAFB	0x00000400U
#define URTWX_TXDESC_W8_EN_HWSEQ	0x00008000U
#define URTWX_RATEID_G			7
#define URTWX_RATEID_B_20M		8
#define URTWX_DESC_RATE1M		0x00
#define URTWX_DESC_RATE6M		0x04

#define URTWX_RX_LIST_COUNT		4
#define URTWX_RXBUFSZ			32768
#define URTWX_RX_DESC_SIZE		24
#define URTWX_RX_MGMT_LOG_MAX		32

#define URTWX_WAIT_COUNT		1000
#define URTWX_WAIT_DELAY_US		10
#define URTWX_PWR_POLL_COUNT		20000
#define URTWX_PWR_POLL_DELAY_US		50
#define URTWX_EFUSE_POLL_COUNT		100000
#define URTWX_EFUSE_POLL_DELAY_US	1

#define URTWX_PAGE_SIZE			128
#define URTWX_TXFF_SIZE			65536
#define URTWX_TXFF_PG_NUM		(URTWX_TXFF_SIZE / URTWX_PAGE_SIZE)
#define URTWX_RXFF_SIZE			16384
#define URTWX_C2H_PKT_BUF		256
#define URTWX_PHY_STATUS_SIZE		4
#define URTWX_USB_TX_AGG_DESC_NUM	3
#define URTWX_RSVD_DRV_PG_NUM		8
#define URTWX_RSVD_H2C_EXTRA_PG_NUM	24
#define URTWX_RSVD_H2C_STATIC_PG_NUM	8
#define URTWX_RSVD_H2CQ_PG_NUM		8
#define URTWX_RSVD_CPU_INSTR_PG_NUM	0
#define URTWX_RSVD_FW_TXBUF_PG_NUM	4
#define URTWX_RSVD_PG_NUM		52
#define URTWX_RSVD_BOUNDARY		\
	((URTWX_TXFF_SIZE / URTWX_PAGE_SIZE) - URTWX_RSVD_PG_NUM)
#define URTWX_H2CQ_PG_ADDR		\
	(URTWX_TXFF_PG_NUM - URTWX_RSVD_FW_TXBUF_PG_NUM -	\
	 URTWX_RSVD_CPU_INSTR_PG_NUM - URTWX_RSVD_H2CQ_PG_NUM)
#define URTWX_H2CQ_ADDR			(URTWX_H2CQ_PG_ADDR * URTWX_PAGE_SIZE)
#define URTWX_H2CQ_SIZE			(URTWX_RSVD_H2CQ_PG_NUM * URTWX_PAGE_SIZE)
#define URTWX_DLFW_HIQ_PGNUM		0x200
#define URTWX_SYS_FUNC_EN_8821C		0xd8
#define URTWX_DMA_MAPPING_HIGH		3

#define URTWX_EFUSE_PHY_SIZE		512
#define URTWX_EFUSE_LOG_SIZE		512
#define URTWX_EFUSE_PROTECT_SIZE	96
#define URTWX_EFUSE_MAC_OFFSET		0x107

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

#define URTWX_FIXED_CHANNEL		1
#define URTWX_MAC_CLK_SPEED		80
#define URTWX_RX_AGG_SIZE		0x05
#define URTWX_RX_AGG_TIMEOUT		0x20
#define URTWX_RF_BASE_A			0x2800
#define URTWX_RF_SIPI_A			0x0c90
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
	uint8_t		is_c2h;
	uint8_t		crc_err;
	uint8_t		icv_err;
};

struct urtwx_softc;

struct urtwx_host_cmd {
	void	(*cb)(struct urtwx_softc *, void *);
	uint8_t	data[16];
};

struct urtwx_cmd_newstate {
	enum ieee80211_state	state;
	int			arg;
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
	uint8_t			 sc_crystal_cap;
	uint8_t			 sc_macaddr[ETHER_ADDR_LEN];
	uint8_t			 sc_tx_bb_swing_2g;
	uint8_t			 sc_tx_bb_swing_5g;
	uint8_t			 sc_pkg_type;
	uint8_t			 sc_rfe_option;
	int			 sc_rfe_btg;
	int			 sc_tx_active;
	int			 sc_tx_timer;
	int			 sc_scan_log;
	int			 sc_tx_mgmt_log;
	int			 sc_txeof_log;
	int			 sc_rx_mgmt_log;
	unsigned int		 sc_rx_done;
	unsigned int		 sc_rx_c2h;
	unsigned int		 sc_rx_desc_log;
	unsigned int		 sc_rx_err;
};

static const struct usb_devno urtwx_devs[] = {
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_1 },
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_2 },
};

static const char *urtwx_fw_names[] = {
	"rtw8821c_fw.bin",
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

static const struct urtwx_page_table urtwx_page_table_8821c[] = {
	{ 16, 16, 16, 14, 1 },
	{ 16, 16, 16, 14, 1 },
	{ 16, 16,  0,  0, 1 },
	{ 16, 16, 16,  0, 1 },
	{ 16, 16, 16, 14, 1 },
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
static int	urtwx_dump_log_efuse_map(const uint8_t *, uint8_t *);
static int	urtwx_dump_phy_efuse_map(struct urtwx_softc *, uint8_t *);
static int	urtwx_fw_download(struct urtwx_softc *);
static void	urtwx_free_rx_list(struct urtwx_softc *);
static int	urtwx_iddma_download_firmware(struct urtwx_softc *,
		    uint32_t, uint32_t, uint32_t, int);
static int	urtwx_init_h2c(struct urtwx_softc *);
static int	urtwx_init(struct ifnet *);
static int	urtwx_is_powered_on(struct urtwx_softc *);
static int	urtwx_ioctl(struct ifnet *, u_long, caddr_t);
static int	urtwx_mac_init_8821c(struct urtwx_softc *);
static int	urtwx_mac_init_runtime(struct urtwx_softc *);
static int	urtwx_mac_init_system_cfg(struct urtwx_softc *);
static int	urtwx_mask2shift(uint32_t);
static int	urtwx_media_change(struct ifnet *);
static int	urtwx_newstate(struct ieee80211com *, enum ieee80211_state,
		    int);
static void	urtwx_newstate_cb(struct urtwx_softc *, void *);
static void	urtwx_parse_rx_desc(const struct urtwx_rx_desc *,
		    struct urtwx_rx_stat *);
static int	urtwx_phy_cfg(struct urtwx_softc *, enum urtwx_phy_cfg,
		    uint32_t, uint32_t);
static int	urtwx_phy_init_runtime(struct urtwx_softc *);
static int	urtwx_phy_load_table(struct urtwx_softc *,
		    const uint32_t *, size_t, enum urtwx_phy_cfg);
static int	urtwx_phy_load_tables(struct urtwx_softc *);
static int	urtwx_phy_match_cond(struct urtwx_softc *, uint32_t,
		    uint32_t);
static uint32_t	urtwx_read_rf(struct urtwx_softc *, uint8_t);
static int	urtwx_power_on(struct urtwx_softc *);
static int	urtwx_priority_queue_cfg(struct urtwx_softc *);
static struct usbd_pipe *urtwx_qsel_to_pipe(struct urtwx_softc *, uint8_t);
static int	urtwx_read_efuse(struct urtwx_softc *, uint8_t *);
static uint8_t	urtwx_read_1(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static uint16_t	urtwx_read_2(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static uint32_t	urtwx_read_4(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t);
static int	urtwx_read_mem(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, void *, int);
static int	urtwx_reg_sec(struct urtwx_softc *, uint16_t, void *);
static void	urtwx_rx_frame(struct urtwx_softc *, const uint8_t *, int,
		    struct mbuf_list *);
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
static void	urtwx_txdesc_checksum(struct urtwx_tx_desc *);
static int	urtwx_usb_interface_cfg(struct urtwx_softc *);
static int	urtwx_usb_rx_agg_cfg(struct urtwx_softc *);
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
static int	urtwx_write_rf(struct urtwx_softc *, uint8_t, uint32_t,
		    uint32_t);
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
static int	urtwx_write_4_mask(struct urtwx_softc *, uint16_t,
		    uint32_t, uint32_t);
static int	urtwx_write_data_rsvd_page(struct urtwx_softc *, uint16_t,
		    const uint8_t *, uint32_t);
static int	urtwx_write_mem(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, void *, int);
static void	urtwx_do_async(struct urtwx_softc *,
		    void (*)(struct urtwx_softc *, void *), void *, int);

struct cfdriver urtwx_cd = {
	NULL, "urtwx", DV_IFNET
};

const struct cfattach urtwx_ca = {
	sizeof(struct urtwx_softc), urtwx_match, urtwx_attach, urtwx_detach
};

static int
urtwx_ctl(struct urtwx_softc *sc, uint8_t rw, uint8_t cmd, uint16_t val,
    uint16_t index, void *buf, int len)
{
	usb_device_request_t	req;
	usbd_status		err;

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

	val[0] = 0xff;
	val[1] = 0xff;
	val[2] = 0xff;
	val[3] = 0xff;
	(void)urtwx_read_mem(sc, cmd, reg, index, val, sizeof(val));
	return (UGETDW(val));
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
urtwx_read_rf(struct urtwx_softc *sc, uint8_t addr)
{
	uint16_t reg;

	reg = URTWX_RF_BASE_A + ((uint16_t)addr << 2);
	return (urtwx_read_4(sc, RTW_USB_CMD_REQ, reg, 0) & RFREG_MASK);
}

static int
urtwx_write_rf(struct urtwx_softc *sc, uint8_t addr, uint32_t mask,
    uint32_t data)
{
	uint32_t old, shift, value;

	mask &= RFREG_MASK;
	data &= RFREG_MASK;

	if (mask != RFREG_MASK) {
		old = urtwx_read_rf(sc, addr);
		shift = urtwx_mask2shift(mask);
		data = (old & ~mask) | ((data << shift) & mask);
	}

	value = (((uint32_t)addr << 20) | data) & 0x0fffffffU;
	if (urtwx_write_4(sc, RTW_USB_CMD_REQ, URTWX_RF_SIPI_A, 0, value) != 0)
		return (EIO);
	delay(13);
	return (0);
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
	val8 |= URTWX_SYS_FUNC_EN_8821C;
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
urtwx_txdesc_checksum(struct urtwx_tx_desc *txd)
{
	uint8_t		*buf;
	uint16_t	csum, word;
	int		i;

	buf = (uint8_t *)txd;
	buf[URTWX_TXDESC_CSUM_IDX * 2] = 0;
	buf[URTWX_TXDESC_CSUM_IDX * 2 + 1] = 0;

	csum = 0;
	for (i = 0; i < URTWX_TXDESC_CSUM_WORDS; i++) {
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
	ifp->if_timer = 0;

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
	const char *name;
	uint32_t len;
	usbd_status err;
	struct usbd_pipe *pipe;

	if (sc->sc_tx_active)
		return (EBUSY);

	len = URTWX_TXDESC_SIZE + m->m_pkthdr.len;
	if (len > URTWX_TXDESC_SIZE + URTWX_MAX_FW_CHUNK)
		return (EFBIG);

	memset(sc->sc_tx_buf, 0, len);
	txd = (struct urtwx_tx_desc *)sc->sc_tx_buf;
	wh = mtod(m, struct ieee80211_frame *);
	name = NULL;

	txd->w0 = htole32(m->m_pkthdr.len | (URTWX_TXDESC_SIZE << 16) |
	    (IEEE80211_IS_MULTICAST(wh->i_addr1) ? URTWX_TXDESC_W0_BMC : 0) |
	    URTWX_TXDESC_W0_LS);
	txd->w1 = htole32((uint32_t)qsel << URTWX_TXDESC_W1_QSEL_SHIFT);
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
			printf("%s: TX %s len %u chan %u\n", DEVNAME(sc), name,
			    m->m_pkthdr.len,
			    ieee80211_chan2ieee(&sc->sc_ic, ni->ni_chan));
			sc->sc_tx_mgmt_log++;
		}
		/*
		 * Follow the 8821C USB management-frame template from
		 * Linux: fixed basic rate, no fallback, and HW-managed
		 * sequence numbers.
		 */
		txd->w0 = htole32(letoh32(txd->w0) |
		    URTWX_TXDESC_W0_DISQSELSEQ);
		txd->w1 = htole32(letoh32(txd->w1) |
		    ((uint32_t)URTWX_RATEID_B_20M <<
		    URTWX_TXDESC_W1_RATEID_SHIFT));
		txd->w3 = htole32(URTWX_TXDESC_W3_USE_RATE |
		    URTWX_TXDESC_W3_DISDATAFB);
		txd->w4 = htole32(URTWX_DESC_RATE1M);
		txd->w8 = htole32(URTWX_TXDESC_W8_EN_HWSEQ);
	}
	urtwx_txdesc_checksum(txd);

	m_copydata(m, 0, m->m_pkthdr.len, sc->sc_tx_buf + URTWX_TXDESC_SIZE);
	m_freem(m);

	pipe = urtwx_qsel_to_pipe(sc, qsel);
	if (pipe == NULL)
		return (ENXIO);

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
	urtwx_txdesc_checksum(txd);

	ret = urtwx_bulk_tx(sc, URTWX_TXDESC_QSEL_BEACON, total_len);
	if (ret != 0)
		goto restore;

	ret = urtwx_wait_2(sc, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1,
	    BIT_BCN_VALID_V1, URTWX_WAIT_COUNT, URTWX_WAIT_DELAY_US);

restore:
	rsvd_pg_head = URTWX_RSVD_BOUNDARY;
	(void)urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2, 0,
	    rsvd_pg_head | BIT_BCN_VALID_V1);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_BCN_CTRL, 0, bcn_ctrl);
	(void)urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR + 1, 0, cr1);

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
	for (i = 0; i < nitems(urtwx_fw_names); i++) {
		ret = loadfirmware(urtwx_fw_names[i], &fw, &fw_size);
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
	if (letoh16(hdr->signature) != 0x8821) {
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
	for (addr = 0; addr < URTWX_EFUSE_PHY_SIZE; addr++) {
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
urtwx_dump_log_efuse_map(const uint8_t *phy_map, uint8_t *log_map)
{
	uint32_t phy_idx;
	uint32_t log_idx;
	uint32_t limit;
	uint8_t hdr1, hdr2;
	uint8_t blk_idx;
	uint8_t word_en;
	int i;

	limit = URTWX_EFUSE_PHY_SIZE - URTWX_EFUSE_PROTECT_SIZE;
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
			    log_idx + 1 >= URTWX_EFUSE_LOG_SIZE)
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
urtwx_read_efuse(struct urtwx_softc *sc, uint8_t *mac_out)
{
	uint8_t	*phy_map;
	uint8_t	*log_map;
	int	 ret;

	phy_map = malloc(URTWX_EFUSE_PHY_SIZE, M_TEMP, M_WAITOK);
	log_map = malloc(URTWX_EFUSE_LOG_SIZE, M_TEMP, M_WAITOK);
	memset(phy_map, 0, URTWX_EFUSE_PHY_SIZE);
	memset(log_map, 0xff, URTWX_EFUSE_LOG_SIZE);

	ret = urtwx_dump_phy_efuse_map(sc, phy_map);
	if (ret != 0)
		goto out;

	ret = urtwx_dump_log_efuse_map(phy_map, log_map);
	if (ret != 0)
		goto out;

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

out:
	free(log_map, M_TEMP, URTWX_EFUSE_LOG_SIZE);
	free(phy_map, M_TEMP, URTWX_EFUSE_PHY_SIZE);
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

	rqpn = &urtwx_rqpn_8821c[urtwx_bulkout_num(sc)];
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

static int
urtwx_phy_match_cond(struct urtwx_softc *sc, uint32_t cond, uint32_t cond2)
{
	uint8_t cut, intf, pkg, rfe;

	(void)cond2;

	cut = (cond & URTWX_PHY_COND_CUT_MASK) >> URTWX_PHY_COND_CUT_SHIFT;
	pkg = (cond & URTWX_PHY_COND_PKG_MASK) >> URTWX_PHY_COND_PKG_SHIFT;
	intf = (cond & URTWX_PHY_COND_INTF_MASK) >> URTWX_PHY_COND_INTF_SHIFT;
	rfe = cond & URTWX_PHY_COND_RFE_MASK;

	if (cut != 0 && cut != URTWX_PHY_COND_WILDCARD)
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

static int
urtwx_phy_cfg(struct urtwx_softc *sc, enum urtwx_phy_cfg cfg, uint32_t addr,
    uint32_t data)
{
	switch (cfg) {
	case URTWX_PHY_CFG_MAC:
		return (urtwx_write_1(sc, RTW_USB_CMD_REQ, addr, 0, data));
	case URTWX_PHY_CFG_AGC:
		return (urtwx_write_4(sc, RTW_USB_CMD_REQ, addr, 0, data));
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
			delay(1);
			return (0);
		default:
			return (urtwx_write_4(sc, RTW_USB_CMD_REQ, addr, 0,
			    data));
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
			if (urtwx_write_rf(sc, addr, RFREG_MASK, data) != 0)
				return (EIO);
			delay(1);
			return (0);
		}
	}

	return (EINVAL);
}

static int
urtwx_phy_load_table(struct urtwx_softc *sc, const uint32_t *tbl, size_t size,
    enum urtwx_phy_cfg cfg)
{
	uint32_t addr, cond, cond2, data;
	int i, is_matched, is_skipped, ret;
	size_t off;

	cond = 0;
	cond2 = 0;
	is_matched = 1;
	is_skipped = 0;

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
			continue;

		ret = urtwx_phy_cfg(sc, cfg, addr, data);
		if (ret != 0)
			return (ret);
	}

	return (0);
}

static int
urtwx_phy_load_tables(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_phy_load_table(sc, urtwx_mac, nitems(urtwx_mac),
	    URTWX_PHY_CFG_MAC);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_load_table(sc, urtwx_bb, nitems(urtwx_bb),
	    URTWX_PHY_CFG_BB);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_load_table(sc, urtwx_agc, nitems(urtwx_agc),
	    URTWX_PHY_CFG_AGC);
	if (ret != 0)
		return (ret);
	if (sc->sc_rfe_option == 0x02 || sc->sc_rfe_option == 0x04) {
		ret = urtwx_phy_load_table(sc, urtwx_agc_btg_type2,
		    nitems(urtwx_agc_btg_type2), URTWX_PHY_CFG_AGC);
		if (ret != 0)
			return (ret);
	}

	return (urtwx_phy_load_table(sc, urtwx_rf_a, nitems(urtwx_rf_a),
	    URTWX_PHY_CFG_RF));
}

static int
urtwx_phy_switch_rf_set(struct urtwx_softc *sc, enum urtwx_rf_set rf_set)
{
	uint32_t reg;
	int ret;

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

	ret = urtwx_write_4_mask(sc, REG_RXPSEL, BIT_RX_PSEL_RST, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_load_tables(sc);
	if (ret != 0)
		return (ret);
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

	return (urtwx_write_4_mask(sc, REG_RXPSEL, BIT_RX_PSEL_RST,
	    BIT_RX_PSEL_RST));
}

static int
urtwx_phy_set_channel_bb(struct urtwx_softc *sc, uint8_t channel)
{
	uint32_t val32;
	int ret;

	if (channel > 14)
		return (EINVAL);

	ret = urtwx_write_4_mask(sc, REG_RXPSEL, 0x10000000U, 0x10000000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_CCK_CHECK, BIT_CHECK_CCK_EN, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_ENTXCCK, 0x00040000U, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_RXCCAMSK, 0x0000fc00U, 15U << 10);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_TXSCALE_A, 0x00000f00U, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_CLKTRK, 0x1ffe0000U,
	    0x96aU << 17);
	if (ret != 0)
		return (ret);

	val32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0);
	val32 &= 0xffcffc00U;
	val32 |= 0x10010000U;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_ADCCLK, 0, val32);
	if (ret != 0)
		return (ret);

	return (urtwx_write_4_mask(sc, REG_ADC160, 0x40000000U,
	    0x40000000U));
}

static int
urtwx_phy_set_channel_mac(struct urtwx_softc *sc, uint8_t channel)
{
	uint32_t val32;
	int ret;

	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_DATA_SC, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_WMAC_TRXPTCL_CTL, BIT_RFMOD, 0);
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
urtwx_phy_set_channel_rf(struct urtwx_softc *sc, uint8_t channel)
{
	uint32_t rf18;
	int ret;

	rf18 = urtwx_read_rf(sc, 0x18);
	rf18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
	    RF18_BW_MASK);
	rf18 |= RF18_BAND_2G | (channel & RF18_CHANNEL_MASK) |
	    RF18_BW_20M;

	if (sc->sc_rfe_btg)
		ret = urtwx_phy_switch_rf_set(sc, URTWX_SWITCH_TO_BTG);
	else
		ret = urtwx_phy_switch_rf_set(sc, URTWX_SWITCH_TO_WLG);
	if (ret != 0)
		return (ret);

	ret = urtwx_write_rf(sc, RF_LUTDBG, 0x40, 1);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf(sc, 0x64, 0x0f, 0x0f);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf(sc, 0x18, RFREG_MASK, rf18);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_rf(sc, RF_XTALX2, 0x00080000U, 0);
	if (ret != 0)
		return (ret);

	return (urtwx_write_rf(sc, RF_XTALX2, 0x00080000U, 1));
}

static int
urtwx_phy_set_channel_rxdfir(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_write_4_mask(sc, REG_ACBB0, 0x30000000U, 0x20000000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_ACBBRXFIR, 0x30000000U,
	    0x20000000U);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4_mask(sc, REG_TXDFIR, 0x80000000U, 0x80000000U);
	if (ret != 0)
		return (ret);

	return (urtwx_write_4_mask(sc, REG_CHFIR, 0x80000000U, 0));
}

static int
urtwx_phy_init_runtime(struct urtwx_softc *sc)
{
	int ret;

	ret = urtwx_phy_enable_bb_rf(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_bb(sc, URTWX_FIXED_CHANNEL);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_mac(sc, URTWX_FIXED_CHANNEL);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_rf(sc, URTWX_FIXED_CHANNEL);
	if (ret != 0)
		return (ret);

	return (urtwx_phy_set_channel_rxdfir(sc));
}

static int
urtwx_set_chan(struct urtwx_softc *sc, struct ieee80211_channel *c)
{
	uint8_t chan;
	int ret;

	if (c == NULL || !IEEE80211_IS_CHAN_2GHZ(c))
		return (ENOTSUP);

	chan = ieee80211_chan2ieee(&sc->sc_ic, c);
	if (chan == 0 || chan > 14)
		return (EINVAL);

	ret = urtwx_phy_set_channel_bb(sc, chan);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_mac(sc, chan);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_set_channel_rf(sc, chan);
	if (ret != 0)
		return (ret);

	return (urtwx_phy_set_channel_rxdfir(sc));
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

	cmd.state = nstate;
	cmd.arg = arg;
	urtwx_do_async(sc, urtwx_newstate_cb, &cmd, sizeof(cmd));
	return (0);
}

static void
urtwx_newstate_cb(struct urtwx_softc *sc, void *arg)
{
	struct urtwx_cmd_newstate *cmd = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	int s;

	s = splnet();
	switch (cmd->state) {
	case IEEE80211_S_INIT:
		timeout_del(&sc->sc_scan_to);
		break;
	case IEEE80211_S_SCAN:
#if URTWX_FORCE_PASSIVE_SCAN
		ic->ic_flags &= ~IEEE80211_F_ASCAN;
#endif
		chan = ic->ic_bss->ni_chan;
		if (urtwx_set_chan(sc, chan) == 0 &&
		    !usbd_is_dying(sc->sc_udev)) {
			if (sc->sc_scan_log < URTWX_SCAN_LOG_MAX) {
				printf("%s: scan channel %u rf18=%#x\n",
				    DEVNAME(sc), ieee80211_chan2ieee(ic, chan),
				    urtwx_read_rf(sc, 0x18));
				sc->sc_scan_log++;
			}
			timeout_add_msec(&sc->sc_scan_to,
			    URTWX_SCAN_DWELL_TIME);
		}
		break;
	case IEEE80211_S_AUTH:
	case IEEE80211_S_ASSOC:
	case IEEE80211_S_RUN:
		timeout_del(&sc->sc_scan_to);
		chan = (ic->ic_opmode == IEEE80211_M_MONITOR) ?
		    ic->ic_ibss_chan : ic->ic_bss->ni_chan;
		(void)urtwx_set_chan(sc, chan);
		break;
	}
	(void)sc->sc_newstate(ic, cmd->state, cmd->arg);
	splx(s);
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
	sc->sc_tx_data_pipe = NULL;
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
	stat->shift = (w0 >> 24) & 0x3;
	stat->is_c2h = (w2 & 0x10000000U) != 0;
}

static void
urtwx_rx_frame(struct urtwx_softc *sc, const uint8_t *buf, int len,
    struct mbuf_list *ml)
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
	int i, s, ssidlen;

	if (!(ifp->if_flags & IFF_RUNNING))
		return;
	if (len < sizeof(*wh) + 12)
		return;

	wh = (const struct ieee80211_frame *)buf;
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

	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT &&
	    sc->sc_rx_mgmt_log < URTWX_RX_MGMT_LOG_MAX) {
		subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
		switch (subtype) {
		case IEEE80211_FC0_SUBTYPE_BEACON:
			name = "beacon";
			break;
		case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
			name = "probe response";
			break;
		default:
			name = NULL;
			break;
		}
		if (name != NULL) {
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
				printf("%s: %s bssid %s chan %u ssid \"%s\"\n",
				    DEVNAME(sc), name,
				    ether_sprintf((u_char *)wh->i_addr3),
				    chan, ssid);
			} else {
				printf("%s: %s bssid %s ssid \"%s\"\n",
				    DEVNAME(sc), name,
				    ether_sprintf((u_char *)wh->i_addr3),
				    ssid);
			}
			sc->sc_rx_mgmt_log++;
		}
	}

	s = splnet();
	ni = ieee80211_find_rxnode(ic, (const struct ieee80211_frame *)buf);
	memset(&rxi, 0, sizeof(rxi));
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
	uint8_t *buf;
	int error, len, pktoff, pktlen, pkttot;

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
			printf("%s: RX desc pktlen %u info %u shift %u c2h %u "
			    "crc %u icv %u\n", DEVNAME(sc), stat.pktlen,
			    stat.drvinfosz, stat.shift, stat.is_c2h,
			    stat.crc_err, stat.icv_err);
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

		if (stat.is_c2h) {
			sc->sc_rx_c2h++;
			if (sc->sc_rx_c2h <= 8) {
				printf("%s: RX C2H pktlen %u\n", DEVNAME(sc),
				    stat.pktlen);
			}
		} else if (!stat.crc_err && !stat.icv_err) {
			urtwx_rx_frame(sc, buf + pktoff, stat.pktlen, &ml);
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

	for (i = 0; i < URTWX_RX_LIST_COUNT; i++) {
		data = &sc->sc_rx_data[i];
		usbd_setup_xfer(data->xfer, sc->sc_ep[URTWX_ENDPT_RX], data,
		    data->buf, URTWX_RXBUFSZ,
		    USBD_SHORT_XFER_OK | USBD_NO_COPY, USBD_NO_TIMEOUT,
		    urtwx_rxeof);
		error = usbd_transfer(data->xfer);
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

	ic->ic_bss->ni_chan = ic->ic_ibss_chan;
	sc->sc_scan_log = 0;
	sc->sc_tx_mgmt_log = 0;
	sc->sc_txeof_log = 0;
	sc->sc_rx_mgmt_log = 0;
	sc->sc_rx_done = 0;
	sc->sc_rx_c2h = 0;
	sc->sc_rx_desc_log = 0;
	sc->sc_rx_err = 0;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	ieee80211_begin_scan(ifp);
	return (0);
}

static void
urtwx_stop(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int s;

	sc->sc_tx_timer = 0;
	ifp->if_timer = 0;
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	timeout_del(&sc->sc_scan_to);

	s = splnet();
	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	urtwx_wait_async(sc);
	splx(s);
}

static void
urtwx_start(struct ifnet *ifp)
{
	struct urtwx_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct mbuf *m;

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
		if (ic->ic_state != IEEE80211_S_RUN)
			break;
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
				urtwx_stop(ifp);
				error = urtwx_init(ifp);
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
	rqpn = &urtwx_rqpn_8821c[bulkout];

	pq_map = BIT_TXDMA_HIQ_MAP(rqpn->dma_map_hi) |
	    BIT_TXDMA_MGQ_MAP(rqpn->dma_map_mg) |
	    BIT_TXDMA_BKQ_MAP(rqpn->dma_map_bk) |
	    BIT_TXDMA_BEQ_MAP(rqpn->dma_map_be) |
	    BIT_TXDMA_VIQ_MAP(rqpn->dma_map_vi) |
	    BIT_TXDMA_VOQ_MAP(rqpn->dma_map_vo);

	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP, 0, pq_map);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR, 0, 0);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1(sc, RTW_USB_CMD_REQ, REG_CR, 0, MAC_TRX_ENABLE);
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
	pg = &urtwx_page_table_8821c[bulkout];
	pubq_num = URTWX_RSVD_BOUNDARY - pg->hq_num - pg->lq_num -
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
	    URTWX_RSVD_BOUNDARY);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_1_mask(sc, REG_FWHW_TXQ_CTRL + 2, 0x10, 0x10);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BCNQ_BDNY_V1, 0,
	    URTWX_RSVD_BOUNDARY);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_FIFOPAGE_CTRL_2 + 2, 0,
	    URTWX_RSVD_BOUNDARY);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_2(sc, RTW_USB_CMD_REQ, REG_BCNQ1_BDNY_V1, 0,
	    URTWX_RSVD_BOUNDARY);
	if (ret != 0)
		return (ret);
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_RXFF_BNDY, 0,
	    URTWX_RXFF_SIZE - URTWX_C2H_PKT_BUF - 1);
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
	value32 = (value32 & 0xfffc0000U) | URTWX_H2CQ_ADDR;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2C_HEAD, 0, value32);
	if (ret != 0)
		return (ret);

	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_READ_ADDR, 0);
	value32 = (value32 & 0xfffc0000U) | URTWX_H2CQ_ADDR;
	ret = urtwx_write_4(sc, RTW_USB_CMD_REQ, REG_H2C_READ_ADDR, 0,
	    value32);
	if (ret != 0)
		return (ret);

	value32 = urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_H2C_TAIL, 0);
	value32 = (value32 & 0xfffc0000U) |
	    (URTWX_H2CQ_ADDR + URTWX_H2CQ_SIZE);
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
		freeq = URTWX_H2CQ_SIZE - (wp - rp);
	else
		freeq = rp - wp;
	if (freeq != URTWX_H2CQ_SIZE) {
		DPRINTF(("%s: H2C queue mismatch wp=%#x rp=%#x free=%#x\n",
		    DEVNAME(sc), wp, rp, freeq));
	}

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

	ret = urtwx_txdma_queue_mapping(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_priority_queue_cfg(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_init_h2c(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_mac_init_8821c(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_phy_init_runtime(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_rx_set_dma(sc);
	if (ret != 0)
		return (ret);
	ret = urtwx_usb_interface_cfg(sc);
	if (ret != 0)
		return (ret);

	return (urtwx_usb_rx_agg_cfg(sc));
}

int
urtwx_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa;

	uaa = aux;
	if (uaa->iface == NULL || uaa->configno != 1)
		return (UMATCH_NONE);

	if (usb_lookup(urtwx_devs, uaa->vendor, uaa->product) == NULL)
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
	usb_interface_descriptor_t	*id;
	usb_endpoint_descriptor_t	*ed;
	usbd_status			 err;
	uint8_t				 mac[ETHER_ADDR_LEN];
	int				 i, ifp_attached;

	sc = (struct urtwx_softc *)self;
	uaa = aux;
	ic = &sc->sc_ic;
	ifp = &ic->ic_if;
	ifp_attached = 0;

	sc->sc_udev = uaa->device;
	sc->sc_iface = uaa->iface;
	sc->sc_nbulkout = 0;
	usb_init_task(&sc->sc_task, urtwx_task, sc, USB_TASK_TYPE_GENERIC);
	timeout_set(&sc->sc_scan_to, urtwx_scan_to, sc);
	sc->sc_cmdq.cur = 0;
	sc->sc_cmdq.next = 0;
	sc->sc_cmdq.queued = 0;

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
			sc->sc_ed[URTWX_ENDPT_RX] = ed->bEndpointAddress;
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

	if (urtwx_fw_download(sc) != 0) {
		printf("%s: firmware download failed\n", DEVNAME(sc));
		goto fail;
	}

	if (urtwx_read_efuse(sc, mac) != 0) {
		printf("%s: could not read efuse MAC address\n", DEVNAME(sc));
		goto fail;
	}
	memcpy(sc->sc_macaddr, mac, sizeof(sc->sc_macaddr));

	printf("%s: MAC %s\n", DEVNAME(sc), ether_sprintf(mac));

	if (urtwx_mac_init_runtime(sc) != 0) {
		printf("%s: could not initialize MAC/RX path\n", DEVNAME(sc));
		goto fail;
	}

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_SHPREAMBLE | IEEE80211_C_SHSLOT |
	    IEEE80211_C_WEP | IEEE80211_C_RSN;
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	for (i = 1; i <= 14; i++) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}
	ic->ic_ibss_chan = &ic->ic_channels[1];

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = urtwx_ioctl;
	ifp->if_start = urtwx_start;
	ifp->if_watchdog = urtwx_watchdog;
	memcpy(ifp->if_xname, DEVNAME(sc), IFNAMSIZ);
	IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_macaddr);

	if_attach(ifp);
	ifp_attached = 1;
	ieee80211_ifattach(ifp);
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = urtwx_newstate;
	ieee80211_media_init(ifp, urtwx_media_change,
	    ieee80211_media_status);

	if (urtwx_rx_init(sc) != 0) {
		printf("%s: could not start RX path\n", DEVNAME(sc));
		goto fail;
	}

	printf("%s: runtime regs CR=%#x RCR=%#x RXDMA_MODE=%#x TXDMA_PQ=%#x\n",
	    DEVNAME(sc),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_CR, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RCR, 0),
	    urtwx_read_1(sc, RTW_USB_CMD_REQ, REG_RXDMA_MODE, 0),
	    urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_TXDMA_PQ_MAP, 0));
	printf("%s: rxdma agg=%#x pktnum=%#x status=%#x dpr=%#x\n",
	    DEVNAME(sc),
	    urtwx_read_2(sc, RTW_USB_CMD_REQ, REG_RXDMA_AGG_PG_TH, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXPKT_NUM, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXDMA_STATUS, 0),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RXDMA_DPR, 0));
	printf("%s: fixed channel %u rf18=%#x rfectl=%#x xtal=%#x rfe=%#x\n",
	    DEVNAME(sc), URTWX_FIXED_CHANNEL, urtwx_read_rf(sc, 0x18),
	    urtwx_read_4(sc, RTW_USB_CMD_REQ, REG_RFECTL, 0),
	    sc->sc_crystal_cap, sc->sc_rfe_option);
	printf("%s: RX ready, listening for management frames\n",
	    DEVNAME(sc));
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
