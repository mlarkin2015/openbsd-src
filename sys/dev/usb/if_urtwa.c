/*	$OpenBSD$	*/

/*-
 * Copyright (c) 2026
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

/*
 * Driver scaffold for Realtek RTL8821AU/RTL8811AU.
 *
 * Design rules:
 *  - USB attach/resource structure may borrow from existing OpenBSD USB WLAN
 *    drivers.
 *  - Chip power-on, firmware, channel, RX, TX, scan, and association logic
 *    must follow Linux rtw8821au/rtw8821a/rtw88xxa.
 *  - Do not graft urtwx lifecycle/state-machine/register sequencing here.
 */

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/task.h>
#include <sys/device.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/if_llc.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <net80211/ieee80211.h>
#include <net80211/ieee80211_crypto.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdevs.h>

#include <dev/usb/if_urtwareg.h>
#include <dev/usb/if_urtwatbl.h>

#define URTWA_MAX_EPOUT	4
#define URTWA_RF_PATH_A	0
#define URTWA_RX_LIST_COUNT 4
#define URTWA_RXBUFSZ	32768
#define URTWA_RX_DESC_SIZE 24
#define URTWA_TX_DESC_SIZE 40
#define URTWA_TXBUFSZ	4096
#define URTWA_TX_TIMEOUT 5000
#define URTWA_RX_MGMT_LOG_MAX 256
#define URTWA_RX_TRACE_LOG_MAX 64
#define URTWA_TX_TRACE_LOG_MAX 32
#define URTWA_TX_START_LOG_MAX 64

#define URTWA_TX_DESC_W0_TXPKTSIZE	0x0000ffffU
#define URTWA_TX_DESC_W0_OFFSET		0x00ff0000U
#define URTWA_TX_DESC_W0_BMC		BIT(24)
#define URTWA_TX_DESC_W0_LS		BIT(26)
#define URTWA_TX_DESC_W0_DISQSELSEQ	BIT(31)
#define URTWA_TX_DESC_W1_MACID		0x000000ffU
#define URTWA_TX_DESC_W1_QSEL		0x00001f00U
#define URTWA_TX_DESC_W1_RATE_ID	0x001f0000U
#define URTWA_TX_DESC_W1_SEC_TYPE	0x00c00000U
#define URTWA_TX_DESC_W1_PKT_OFFSET	0x1f000000U
#define URTWA_TX_DESC_W2_SPE_RPT	BIT(19)
#define URTWA_TX_DESC_W3_HW_SSN_SEL	0x000000c0U
#define URTWA_TX_DESC_W3_USE_RATE	BIT(8)
#define URTWA_TX_DESC_W3_DISDATAFB	BIT(10)
#define URTWA_TX_DESC_W4_DATARATE	0x0000007fU
#define URTWA_TX_DESC_W4_DATARATE_FB_LIMIT 0x00001f00U
#define URTWA_TX_DESC_W7_TXDESC_CHECKSUM	0x0000ffffU
#define URTWA_TX_DESC_W8_EN_HWSEQ	BIT(15)

#define URTWA_TX_QSEL_HIGH	17
#define URTWA_TX_QSEL_MGMT	18
#define URTWA_TX_QSEL_H2C	19
#define URTWA_TX_QSEL_TID0	0
#define URTWA_TX_QSEL_TID1	1
#define URTWA_TX_QSEL_TID2	2
#define URTWA_TX_QSEL_TID3	3
#define URTWA_TX_QSEL_TID4	4
#define URTWA_TX_QSEL_TID5	5
#define URTWA_TX_QSEL_TID6	6
#define URTWA_TX_QSEL_TID7	7

#define URTWA_DMA_MAPPING_EXTRA		0
#define URTWA_DMA_MAPPING_LOW		1
#define URTWA_DMA_MAPPING_NORMAL	2
#define URTWA_DMA_MAPPING_HIGH		3

#define URTWA_RATEID_G			7
#define URTWA_RATEID_B_20M		8

#define URTWA_DESC_RATE1M		0x00
#define URTWA_DESC_RATE6M		0x04

#define URTWA_NET_NO_LINK		0
#define URTWA_NET_MGD_LINKED		2

#define URTWA_PORT0_NET_TYPE_MASK	0x00030000U

#define URTWA_H2C_CMDID_MASK		0x000000ffU
#define URTWA_H2C_DEFAULT_PORT_CMD	0x2c
#define URTWA_H2C_DEFAULT_PORT_PORTID	0x0000ff00U
#define URTWA_H2C_DEFAULT_PORT_MACID	0x00ff0000U
#define URTWA_H2C_MEDIA_STATUS_CMD	0x01
#define URTWA_H2C_MEDIA_STATUS_OPMODE	BIT(8)
#define URTWA_H2C_MEDIA_STATUS_MACID	0x00ff0000U

enum urtwa_table_kind {
	URTWA_TBL_MAC,
	URTWA_TBL_AGC,
	URTWA_TBL_BB,
	URTWA_TBL_RF,
};

struct urtwa_table {
	const uint32_t		*data;
	size_t			 size;
	enum urtwa_table_kind	 kind;
	uint8_t			 rf_path;
};

struct urtwa_rx_desc {
	uint32_t	w0;
	uint32_t	w1;
	uint32_t	w2;
	uint32_t	w3;
	uint32_t	w4;
	uint32_t	w5;
} __packed;

struct urtwa_tx_desc {
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
} __packed;

struct urtwa_rx_stat {
	uint16_t	pktlen;
	uint8_t		drvinfosz;
	uint8_t		shift;
	uint8_t		rate;
	uint8_t		bw;
	uint8_t		rssi;
	int8_t		rx_power;
	uint8_t		phy_status;
	uint8_t		crc_err;
	uint8_t		icv_err;
	uint8_t		decrypted;
	uint8_t		is_c2h;
};

struct urtwa_rx_data {
	struct urtwa_softc	*sc;
	struct usbd_xfer	*xfer;
	uint8_t			*buf;
};

struct urtwa_efuse_info {
	uint8_t		rf_board_option;
	uint8_t		channel_plan;
	uint8_t		crystal_cap;
	uint8_t		pa_type_2g;
	uint8_t		pa_type_5g;
	uint8_t		lna_type_2g;
	uint8_t		lna_type_5g;
	uint8_t		bt_setting;
	uint8_t		tx_bb_swing_setting_2g;
	uint8_t		tx_bb_swing_setting_5g;
	uint8_t		rfe_option;
	uint8_t		glna_type;
	uint8_t		alna_type;
	uint8_t		gpa_type;
	uint8_t		apa_type;
	uint8_t		ext_lna_2g;
	uint8_t		ext_lna_5g;
	uint8_t		ext_pa_2g;
	uint8_t		ext_pa_5g;
	uint8_t		btcoex;
};

struct urtwa_softc {
	struct device		 sc_dev;
	struct ieee80211com	 sc_ic;
	struct usbd_device	*sc_udev;
	struct usbd_interface	*sc_iface;
	struct usbd_pipe	*sc_rx_pipe;
	struct usbd_pipe	*sc_intr_pipe;
	struct usbd_pipe	*sc_tx_pipe[URTWA_MAX_EPOUT];
	struct usbd_xfer	*sc_tx_xfer;
	struct usb_task		 sc_task;
	uint16_t		 sc_vendor;
	uint16_t		 sc_product;
	uint8_t			 sc_rx_no;
	uint8_t			 sc_intr_no;
	uint8_t			 sc_tx_no[URTWA_MAX_EPOUT];
	uint8_t			 sc_ntx;
	uint16_t		 sc_page_size;
	uint16_t		 sc_txff_pg_num;
	uint16_t		 sc_rsvd_boundary;
	uint8_t			 sc_macaddr[6];
	uint8_t			*sc_tx_buf;
	uint8_t			 sc_if_attached;
	uint8_t			 sc_curchan;
	struct timeout		 sc_scan_to;
	struct urtwa_rx_data	 sc_rx_data[URTWA_RX_LIST_COUNT];
	int			(*sc_newstate)(struct ieee80211com *,
				    enum ieee80211_state, int);
	int			(*sc_send_mgmt)(struct ieee80211com *,
				    struct ieee80211_node *, int, int, int);
	int			(*sc_set_key)(struct ieee80211com *,
				    struct ieee80211_node *,
				    struct ieee80211_key *);
	void			(*sc_delete_key)(struct ieee80211com *,
				    struct ieee80211_node *,
				    struct ieee80211_key *);
	int			 sc_rx_done;
	int			 sc_rx_desc_log;
	int			 sc_rx_mgmt_log;
	int			 sc_tx_active;
	int			 sc_tx_ep;
	int			 sc_tx_log;
	int			 sc_tx_start_log;
	int			 sc_tx_mgmt_log;
	int			 sc_key_log;
	int			 sc_eapol_log;
	int			 sc_powered;
	int			 sc_fw_running;
	int			 sc_fw_deferred;
	int			 sc_efuse_valid;
	int			 sc_running;
	uint8_t			 sc_h2c_box;
	int			 sc_scan_pending;
	int			 sc_link_pending;
	int			 sc_link_connect;
	struct urtwa_efuse_info	 sc_efuse;
};

struct urtwa_8821au_efuse {
	uint8_t		res4[48];		/* 0x0d0 */
	uint8_t		vid[2];			/* 0x100 */
	uint8_t		pid[2];
	uint8_t		res8[3];
	uint8_t		mac_addr[6];		/* 0x107 */
	uint8_t		res9[243];
} __packed;

struct urtwa_efuse_map {
	uint16_t	rtl_id;
	uint8_t		res0[6];		/* 0x002 */
	uint8_t		usb_mode;		/* 0x008 */
	uint8_t		res1[7];		/* 0x009 */
	uint8_t		res_txpwr[0xa8];	/* 0x010 */
	uint8_t		channel_plan;		/* 0x0b8 */
	uint8_t		xtal_k;
	uint8_t		thermal_meter;
	uint8_t		iqk_lck;
	uint8_t		pa_type;		/* 0x0bc */
	uint8_t		lna_type_2g;		/* 0x0bd */
	uint8_t		res2;
	uint8_t		lna_type_5g;		/* 0x0bf */
	uint8_t		res3;
	uint8_t		rf_board_option;	/* 0x0c1 */
	uint8_t		rf_feature_option;
	uint8_t		rf_bt_setting;
	uint8_t		eeprom_version;
	uint8_t		eeprom_customer_id;	/* 0x0c5 */
	uint8_t		tx_bb_swing_setting_2g;
	uint8_t		tx_bb_swing_setting_5g;
	uint8_t		tx_pwr_calibrate_rate;
	uint8_t		rf_antenna_option;	/* 0x0c9 */
	uint8_t		rfe_option;
	uint8_t		country_code[2];
	uint8_t		res4[3];
	struct urtwa_8821au_efuse rtw8821au;
} __packed;

CTASSERT(sizeof(struct urtwa_efuse_map) == 512);

#if BYTE_ORDER == LITTLE_ENDIAN
struct urtwa_phy_cond {
	uint32_t	rfe:8;
	uint32_t	intf:4;
	uint32_t	pkg:4;
	uint32_t	plat:4;
	uint32_t	intf_rsvd:4;
	uint32_t	cut:4;
	uint32_t	branch:2;
	uint32_t	neg:1;
	uint32_t	pos:1;
};

struct urtwa_phy_cond2 {
	uint8_t		type_glna;
	uint8_t		type_gpa;
	uint8_t		type_alna;
	uint8_t		type_apa;
};
#else
#error "urtwa currently expects little-endian OpenBSD targets"
#endif

#define URTWA_INTF_USB			BIT(1)
#define URTWA_BRANCH_IF			0
#define URTWA_BRANCH_ELIF		1
#define URTWA_BRANCH_ELSE		2
#define URTWA_BRANCH_ENDIF		3

struct urtwa_phy_cfg_pair {
	uint32_t	addr;
	uint32_t	data;
};

union urtwa_phy_table_tile {
	struct {
		struct urtwa_phy_cond cond;
		struct urtwa_phy_cond2 cond2;
	} __packed;
	struct urtwa_phy_cfg_pair cfg;
};

static const struct urtwa_table urtwa_mac_tbl = {
	.data = rtw8821a_mac,
	.size = nitems(rtw8821a_mac),
	.kind = URTWA_TBL_MAC,
};

static const struct urtwa_table urtwa_agc_tbl = {
	.data = rtw8821a_agc,
	.size = nitems(rtw8821a_agc),
	.kind = URTWA_TBL_AGC,
};

static const struct urtwa_table urtwa_bb_tbl = {
	.data = rtw8821a_bb,
	.size = nitems(rtw8821a_bb),
	.kind = URTWA_TBL_BB,
};

static const struct urtwa_table urtwa_rf_a_tbl = {
	.data = rtw8821a_rf_a,
	.size = nitems(rtw8821a_rf_a),
	.kind = URTWA_TBL_RF,
	.rf_path = URTWA_RF_PATH_A,
};

static const struct usb_devno urtwa_devs[] = {
	{ USB_VENDOR_TPLINK, 0x011e },
	{ USB_VENDOR_TPLINK, 0x011f },
	{ USB_VENDOR_TPLINK, 0x0120 },
};

static const struct urtwa_pwr_seq_cmd urtwa_carddis_to_cardemu[] = {
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(3) | BIT(7), 0},
	{0x0086, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_SDIO, URTWA_PWR_CMD_WRITE, BIT(0), 0},
	{0x0086, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_SDIO, URTWA_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x004a, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_USB_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(3) | BIT(4), 0},
	{0x0023, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(4), 0},
	{0x0301, URTWA_PWR_CUT_ALL_MSK, 0,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, 0xff, 0},
	{0xffff, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    0, URTWA_PWR_CMD_END, 0, 0},
};

static const struct urtwa_pwr_seq_cmd urtwa_cardemu_to_act[] = {
	{0x0020, URTWA_PWR_CUT_ALL_MSK,
	    URTWA_PWR_INTF_USB_MSK | URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0067, URTWA_PWR_CUT_ALL_MSK,
	    URTWA_PWR_INTF_USB_MSK | URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(4), 0},
	{0x0001, URTWA_PWR_CUT_ALL_MSK,
	    URTWA_PWR_INTF_USB_MSK | URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_DELAY, 1, URTWA_PWR_DELAY_MS},
	{0x0000, URTWA_PWR_CUT_ALL_MSK,
	    URTWA_PWR_INTF_USB_MSK | URTWA_PWR_INTF_SDIO_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE,
	    BIT(4) | BIT(3) | BIT(2), 0},
	{0x0075, URTWA_PWR_CUT_ALL_MSK, 0,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0006, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0075, URTWA_PWR_CUT_ALL_MSK, 0,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), 0},
	{0x0006, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(7), 0},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(4) | BIT(3), 0},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_POLLING, BIT(0), 0},
	{0x004f, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0067, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(5) | BIT(4),
	    BIT(5) | BIT(4)},
	{0x0025, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(6), 0},
	{0x0049, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0063, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0062, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(1), 0},
	{0x0058, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x005a, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x002e, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, 0xff, 0x82},
	{0x0010, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    URTWA_PWR_ADDR_MAC, URTWA_PWR_CMD_WRITE, BIT(6), BIT(6)},
	{0xffff, URTWA_PWR_CUT_ALL_MSK, URTWA_PWR_INTF_ALL_MSK,
	    0, URTWA_PWR_CMD_END, 0, 0},
};

static const struct usb_devno	*urtwa_lookup(uint16_t, uint16_t);
static int			 urtwa_ctl(struct urtwa_softc *, uint8_t,
				     uint8_t, uint16_t, uint16_t, void *, int);
static int			 urtwa_read_mem(struct urtwa_softc *, uint16_t,
				     void *, int);
static int			 urtwa_write_mem(struct urtwa_softc *, uint16_t,
				     void *, int);
static uint8_t			 urtwa_read_1(struct urtwa_softc *, uint16_t);
static uint16_t			 urtwa_read_2(struct urtwa_softc *, uint16_t);
static uint32_t			 urtwa_read_4(struct urtwa_softc *, uint16_t);
static int			 urtwa_write_1(struct urtwa_softc *, uint16_t,
				     uint8_t);
static int			 urtwa_write_2(struct urtwa_softc *, uint16_t,
				     uint16_t);
static int			 urtwa_write_4(struct urtwa_softc *, uint16_t,
				     uint32_t);
static int			 urtwa_write_addr(struct urtwa_softc *,
				     uint16_t, const uint8_t *);
static int			 urtwa_write_2_mask(struct urtwa_softc *,
				     uint16_t, uint16_t, uint16_t);
static int			 urtwa_write_1_mask(struct urtwa_softc *,
				     uint16_t, uint8_t, uint8_t);
static int			 urtwa_write_4_mask(struct urtwa_softc *,
				     uint16_t, uint32_t, uint32_t);
static int			 urtwa_wait_1(struct urtwa_softc *, uint16_t,
				     uint8_t, uint8_t, int);
static int			 urtwa_parse_endpoints(struct urtwa_softc *);
static int			 urtwa_open_pipes(struct urtwa_softc *);
static void			 urtwa_close_pipes(struct urtwa_softc *);
static int			 urtwa_alloc_tx_resources(struct urtwa_softc *);
static void			 urtwa_free_tx_resources(struct urtwa_softc *);
static int			 urtwa_alloc_rx_list(struct urtwa_softc *);
static void			 urtwa_free_rx_list(struct urtwa_softc *);
static int			 urtwa_pwr_seq(struct urtwa_softc *,
				     const struct urtwa_pwr_seq_cmd *);
static void			 urtwa_reset_8051(struct urtwa_softc *);
static void			 urtwa_hw_reset(struct urtwa_softc *);
static void			 urtwa_wlan_cpu_enable(struct urtwa_softc *,
				     int);
static int			 urtwa_llt_write(struct urtwa_softc *,
				     uint32_t, uint32_t);
static int			 urtwa_llt_init(struct urtwa_softc *);
static int			 urtwa_power_on(struct urtwa_softc *);
static void			 urtwa_power_off(struct urtwa_softc *);
static int			 urtwa_write_firmware_page(struct urtwa_softc *,
				     uint32_t, const uint8_t *, uint32_t);
static int			 urtwa_fw_download_legacy(struct urtwa_softc *);
static void			 urtwa_efuse_grant(struct urtwa_softc *, int);
static int			 urtwa_read_efuse(struct urtwa_softc *);
static int			 urtwa_ensure_fw(struct urtwa_softc *);
static int			 urtwa_attach_ifnet(struct urtwa_softc *);
static int			 urtwa_runtime_init(struct urtwa_softc *);
static int			 urtwa_h2c_send_register(struct urtwa_softc *,
				     uint32_t, uint32_t);
static int			 urtwa_h2c_send(struct urtwa_softc *,
				     uint32_t, uint32_t);
static int			 urtwa_fw_default_port(struct urtwa_softc *);
static int			 urtwa_fw_media_status(struct urtwa_softc *, int);
static int			 urtwa_sta_link_prep(struct urtwa_softc *,
				     const uint8_t *);
static void			 urtwa_sta_link_drop(struct urtwa_softc *);
static uint8_t			 urtwa_desired_channel(struct urtwa_softc *);
static void			 urtwa_task(void *);
static void			 urtwa_kick_state(struct urtwa_softc *);
static void			 urtwa_scan_to(void *);
static void			 urtwa_cancel_scan(struct urtwa_softc *);
static void			 urtwa_parse_rx_desc(struct urtwa_rx_desc *,
				     struct urtwa_rx_stat *);
static void			 urtwa_get_rx_rssi(struct urtwa_softc *,
				     uint8_t *, struct urtwa_rx_stat *);
static int			 urtwa_sta_rx_mgmt_ok(struct urtwa_softc *,
				     const struct ieee80211_frame *);
static void			 urtwa_rx_frame(struct urtwa_softc *, uint8_t *,
				     int, struct urtwa_rx_stat *,
				     struct mbuf_list *);
static void			 urtwa_rxeof(struct usbd_xfer *, void *,
				     usbd_status);
static int			 urtwa_rx_init(struct urtwa_softc *);
static void			 urtwa_txeof(struct usbd_xfer *, void *,
				     usbd_status);
static int			 urtwa_tx(struct urtwa_softc *, struct mbuf *,
				     struct ieee80211_node *, uint8_t, int);
static int			 urtwa_tx_mgmt(struct urtwa_softc *,
				     struct mbuf *, struct ieee80211_node *);
static uint32_t			 urtwa_read_rf(struct urtwa_softc *,
				     uint8_t, uint32_t, uint32_t);
static int			 urtwa_write_rf(struct urtwa_softc *,
				     uint8_t, uint32_t, uint32_t, uint32_t);
static void			 urtwa_setup_phy_cond(struct urtwa_softc *,
				     struct urtwa_phy_cond *,
				     struct urtwa_phy_cond2 *);
static int			 urtwa_load_table(struct urtwa_softc *,
				     const struct urtwa_table *);
static int			 urtwa_phy_init(struct urtwa_softc *);
static int			 urtwa_set_channel(struct urtwa_softc *, uint8_t);
static int			 urtwa_newstate(struct ieee80211com *,
				     enum ieee80211_state, int);
static int			 urtwa_send_mgmt(struct ieee80211com *,
				     struct ieee80211_node *, int, int, int);
static int			 urtwa_set_key(struct ieee80211com *,
				     struct ieee80211_node *,
				     struct ieee80211_key *);
static void			 urtwa_delete_key(struct ieee80211com *,
				     struct ieee80211_node *,
				     struct ieee80211_key *);
static int			 urtwa_init(struct ifnet *);
static void			 urtwa_stop(struct ifnet *, int);
static int			 urtwa_ioctl(struct ifnet *, u_long, caddr_t);
static void			 urtwa_start(struct ifnet *);
static void			 urtwa_scan_set_channel(struct urtwa_softc *);
int				 urtwa_match(struct device *, void *, void *);
void				 urtwa_attach(struct device *, struct device *,
				     void *);
int				 urtwa_detach(struct device *, int);

struct cfdriver urtwa_cd = {
	NULL, "urtwa", DV_IFNET
};

const struct cfattach urtwa_ca = {
	sizeof(struct urtwa_softc), urtwa_match, urtwa_attach, urtwa_detach
};

static const struct usb_devno *
urtwa_lookup(uint16_t vendor, uint16_t product)
{
	return usb_lookup(urtwa_devs, vendor, product);
}

static int
urtwa_ctl(struct urtwa_softc *sc, uint8_t rw, uint8_t cmd, uint16_t val,
    uint16_t index, void *buf, int len)
{
	usb_device_request_t req;
	usbd_status err;

	if (usbd_is_dying(sc->sc_udev))
		return (EIO);

	req.bmRequestType = rw ? UT_WRITE_VENDOR_DEVICE : UT_READ_VENDOR_DEVICE;
	req.bRequest = cmd;
	USETW(req.wValue, val);
	USETW(req.wIndex, index);
	USETW(req.wLength, len);

	err = usbd_do_request(sc->sc_udev, &req, buf);
	if (err) {
		printf("%s: ctl reg 0x%04x failed: %s\n", sc->sc_dev.dv_xname,
		    val, usbd_errstr(err));
		return (EIO);
	}

	return (0);
}

static int
urtwa_read_mem(struct urtwa_softc *sc, uint16_t addr, void *buf, int len)
{
	return (urtwa_ctl(sc, 0, URTWA_USB_CMD_REQ, addr, URTWA_USB_VENQT_IDX,
	    buf, len));
}

static int
urtwa_write_mem(struct urtwa_softc *sc, uint16_t addr, void *buf, int len)
{
	return (urtwa_ctl(sc, 1, URTWA_USB_CMD_REQ, addr, URTWA_USB_VENQT_IDX,
	    buf, len));
}

static uint8_t
urtwa_read_1(struct urtwa_softc *sc, uint16_t addr)
{
	uint8_t v = 0xff;
	(void)urtwa_read_mem(sc, addr, &v, sizeof(v));
	return (v);
}

static uint16_t
urtwa_read_2(struct urtwa_softc *sc, uint16_t addr)
{
	uint8_t v[2] = { 0xff, 0xff };
	(void)urtwa_read_mem(sc, addr, v, sizeof(v));
	return (UGETW(v));
}

static uint32_t
urtwa_read_4(struct urtwa_softc *sc, uint16_t addr)
{
	uint8_t v[4] = { 0xff, 0xff, 0xff, 0xff };
	(void)urtwa_read_mem(sc, addr, v, sizeof(v));
	return (UGETDW(v));
}

static int
urtwa_write_1(struct urtwa_softc *sc, uint16_t addr, uint8_t val)
{
	return (urtwa_write_mem(sc, addr, &val, sizeof(val)));
}

static int
urtwa_write_2(struct urtwa_softc *sc, uint16_t addr, uint16_t val)
{
	uint8_t buf[2];
	USETW(buf, val);
	return (urtwa_write_mem(sc, addr, buf, sizeof(buf)));
}

static int
urtwa_write_4(struct urtwa_softc *sc, uint16_t addr, uint32_t val)
{
	uint8_t buf[4];
	USETDW(buf, val);
	return (urtwa_write_mem(sc, addr, buf, sizeof(buf)));
}

static int
urtwa_write_addr(struct urtwa_softc *sc, uint16_t addr, const uint8_t *ea)
{
	int i, error;

	for (i = 0; i < IEEE80211_ADDR_LEN; i++) {
		error = urtwa_write_1(sc, addr + i, ea[i]);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
urtwa_write_2_mask(struct urtwa_softc *sc, uint16_t addr, uint16_t mask,
    uint16_t val)
{
	uint8_t buf[2];
	uint16_t reg;

	reg = 0xffff;
	(void)urtwa_read_mem(sc, addr, buf, sizeof(buf));
	reg = UGETW(buf);
	reg &= ~mask;
	reg |= val & mask;
	return (urtwa_write_2(sc, addr, reg));
}

static int
urtwa_write_1_mask(struct urtwa_softc *sc, uint16_t addr, uint8_t mask,
    uint8_t val)
{
	uint8_t reg;

	reg = urtwa_read_1(sc, addr);
	reg &= ~mask;
	reg |= val & mask;
	return (urtwa_write_1(sc, addr, reg));
}

static int
urtwa_write_4_mask(struct urtwa_softc *sc, uint16_t addr, uint32_t mask,
    uint32_t val)
{
	uint32_t reg;

	reg = urtwa_read_4(sc, addr);
	reg &= ~mask;
	reg |= val & mask;
	return (urtwa_write_4(sc, addr, reg));
}

static uint32_t
urtwa_encode_bits(uint32_t value, uint32_t mask)
{
	uint32_t shift;

	if (mask == 0)
		return (0);
	for (shift = 0; (mask & 1) == 0; shift++)
		mask >>= 1;
	return ((value << shift) & (mask << shift));
}

static void
urtwa_txdesc_checksum(struct urtwa_tx_desc *txd)
{
	uint16_t *data;
	uint16_t csum;
	uint32_t w7;
	size_t i;

	w7 = letoh32(txd->w7);
	w7 &= ~URTWA_TX_DESC_W7_TXDESC_CHECKSUM;
	txd->w7 = htole32(w7);

	data = (uint16_t *)txd;
	csum = 0;
	for (i = 0; i < 16; i++)
		csum ^= letoh16(data[i]);

	w7 |= urtwa_encode_bits(csum, URTWA_TX_DESC_W7_TXDESC_CHECKSUM);
	txd->w7 = htole32(w7);
}

static int
urtwa_dma_mapping_to_ep(uint8_t mapping)
{
	switch (mapping) {
	case URTWA_DMA_MAPPING_HIGH:
		return (0);
	case URTWA_DMA_MAPPING_NORMAL:
		return (1);
	case URTWA_DMA_MAPPING_LOW:
		return (2);
	case URTWA_DMA_MAPPING_EXTRA:
		return (3);
	default:
		return (-1);
	}
}

static int
urtwa_qsel_to_ep(struct urtwa_softc *sc, uint8_t qsel)
{
	uint8_t mg_map, hi_map, vo_map, vi_map, be_map, bk_map;
	int ep;

	switch (sc->sc_ntx) {
	case 4:
		mg_map = URTWA_DMA_MAPPING_EXTRA;
		hi_map = URTWA_DMA_MAPPING_HIGH;
		vo_map = URTWA_DMA_MAPPING_NORMAL;
		vi_map = URTWA_DMA_MAPPING_NORMAL;
		be_map = URTWA_DMA_MAPPING_LOW;
		bk_map = URTWA_DMA_MAPPING_LOW;
		break;
	case 3:
	case 2:
		mg_map = URTWA_DMA_MAPPING_HIGH;
		hi_map = URTWA_DMA_MAPPING_HIGH;
		vo_map = URTWA_DMA_MAPPING_NORMAL;
		vi_map = URTWA_DMA_MAPPING_NORMAL;
		be_map = URTWA_DMA_MAPPING_HIGH;
		bk_map = URTWA_DMA_MAPPING_HIGH;
		break;
	case 1:
	default:
		mg_map = URTWA_DMA_MAPPING_EXTRA;
		hi_map = URTWA_DMA_MAPPING_HIGH;
		vo_map = URTWA_DMA_MAPPING_HIGH;
		vi_map = URTWA_DMA_MAPPING_HIGH;
		be_map = URTWA_DMA_MAPPING_HIGH;
		bk_map = URTWA_DMA_MAPPING_HIGH;
		break;
	}

	switch (qsel) {
	case URTWA_TX_QSEL_TID0:
	case URTWA_TX_QSEL_TID3:
		ep = urtwa_dma_mapping_to_ep(be_map);
		break;
	case URTWA_TX_QSEL_TID1:
	case URTWA_TX_QSEL_TID2:
		ep = urtwa_dma_mapping_to_ep(bk_map);
		break;
	case URTWA_TX_QSEL_TID4:
	case URTWA_TX_QSEL_TID5:
		ep = urtwa_dma_mapping_to_ep(vi_map);
		break;
	case URTWA_TX_QSEL_TID6:
	case URTWA_TX_QSEL_TID7:
		ep = urtwa_dma_mapping_to_ep(vo_map);
		break;
	case URTWA_TX_QSEL_MGMT:
		ep = urtwa_dma_mapping_to_ep(mg_map);
		break;
	case URTWA_TX_QSEL_HIGH:
	case URTWA_TX_QSEL_H2C:
	default:
		ep = urtwa_dma_mapping_to_ep(hi_map);
		break;
	}

	if (ep < 0 || ep >= sc->sc_ntx)
		ep = 0;
	return (ep);
}

static int
urtwa_wait_1(struct urtwa_softc *sc, uint16_t addr, uint8_t mask,
    uint8_t target, int delay_us)
{
	int n;
	uint8_t v;

	target &= mask;
	for (n = 0; n < URTWA_PWR_POLLING_CNT; n++) {
		v = urtwa_read_1(sc, addr);
		if ((v & mask) == target)
			return (0);
		if (delay_us > 0)
			DELAY(delay_us);
	}
	return (ETIMEDOUT);
}

int
urtwa_wait_4(struct urtwa_softc *sc, uint16_t addr, uint32_t mask,
    uint32_t target, int delay_us)
{
	int n;
	uint32_t v;

	target &= mask;
	for (n = 0; n < URTWA_PWR_POLLING_CNT; n++) {
		v = urtwa_read_4(sc, addr);
		if ((v & mask) == target)
			return (0);
		if (delay_us > 0)
			DELAY(delay_us);
	}
	return (ETIMEDOUT);
}

static int
urtwa_pwr_seq(struct urtwa_softc *sc, const struct urtwa_pwr_seq_cmd *seq)
{
	const struct urtwa_pwr_seq_cmd *cmd;
	uint8_t value;
	uint16_t offset;

	for (cmd = seq; cmd->cmd != URTWA_PWR_CMD_END; cmd++) {
		if (!(cmd->intf_mask & URTWA_PWR_INTF_USB_MSK))
			continue;

		offset = cmd->offset;
		switch (cmd->cmd) {
		case URTWA_PWR_CMD_WRITE:
			value = urtwa_read_1(sc, offset);
			value &= ~cmd->mask;
			value |= cmd->value & cmd->mask;
			if (urtwa_write_1(sc, offset, value) != 0)
				return (EIO);
			break;
		case URTWA_PWR_CMD_POLLING:
			if (urtwa_wait_1(sc, offset, cmd->mask, cmd->value, 50) != 0) {
				printf("%s: power-seq poll failed off=0x%04x mask=0x%02x val=0x%02x\n",
				    sc->sc_dev.dv_xname, offset, cmd->mask, cmd->value);
				return (ETIMEDOUT);
			}
			break;
		case URTWA_PWR_CMD_DELAY:
			if (cmd->value == URTWA_PWR_DELAY_US)
				DELAY(offset);
			else
				usbd_delay_ms(sc->sc_udev, offset);
			break;
		case URTWA_PWR_CMD_READ:
			(void)urtwa_read_1(sc, offset);
			break;
		default:
			return (EINVAL);
		}
	}

	return (0);
}

static void
urtwa_reset_8051(struct urtwa_softc *sc)
{
	uint8_t val8;

	(void)urtwa_write_1_mask(sc, REG_RSV_CTRL, BIT(1), 0);
	(void)urtwa_write_1_mask(sc, REG_RSV_CTRL + 1, BIT(0), 0);

	val8 = urtwa_read_1(sc, REG_SYS_FUNC_EN + 1);
	(void)urtwa_write_1(sc, REG_SYS_FUNC_EN + 1, val8 & ~BIT(2));

	(void)urtwa_write_1_mask(sc, REG_RSV_CTRL, BIT(1), 0);
	(void)urtwa_write_1_mask(sc, REG_RSV_CTRL + 1, BIT(0), BIT(0));
	(void)urtwa_write_1(sc, REG_SYS_FUNC_EN + 1, val8 | BIT(2));
}

static void
urtwa_hw_reset(struct urtwa_softc *sc)
{
	uint8_t val8;

	if (!(urtwa_read_1(sc, REG_MCUFW_CTRL) & BIT_RAM_DL_SEL))
		return;

	urtwa_reset_8051(sc);
	(void)urtwa_write_1(sc, REG_MCUFW_CTRL, 0x00);

	(void)urtwa_write_4_mask(sc, REG_FPGA0_XCD_RF_PARA, BIT(6), BIT(6));
	(void)urtwa_write_1_mask(sc, REG_SYS_FUNC_EN, BIT(0) | BIT(1), 0);
	(void)urtwa_write_1(sc, REG_RF_CTRL, 0x00);
	(void)urtwa_write_2(sc, REG_CR, 0x0000);

	(void)urtwa_write_1_mask(sc, REG_APS_FSMCO + 1,
	    APS_FSMCO_MAC_OFF >> 8, APS_FSMCO_MAC_OFF >> 8);
	if (urtwa_wait_1(sc, REG_APS_FSMCO + 1, APS_FSMCO_MAC_OFF >> 8, 0, 1) !=
	    0) {
		printf("%s: hw-reset MAC-off clear timeout\n",
		    sc->sc_dev.dv_xname);
	}

	val8 = urtwa_read_1(sc, REG_APS_FSMCO + 1);
	val8 |= APS_FSMCO_MAC_ENABLE >> 8;
	(void)urtwa_write_1(sc, REG_APS_FSMCO + 1, val8);

	(void)urtwa_write_1_mask(sc, REG_SYS_FUNC_EN + 1, BIT(4) | BIT(7), 0);
	(void)urtwa_write_1_mask(sc, REG_SYS_FUNC_EN + 1, BIT(4) | BIT(7),
	    BIT(4) | BIT(7));
}

static void
urtwa_wlan_cpu_enable(struct urtwa_softc *sc, int enable)
{
	if (enable) {
		(void)urtwa_write_1_mask(sc, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF,
		    BIT_WLMCU_IOIF);
		(void)urtwa_write_1_mask(sc, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN,
		    BIT_FEN_CPUEN);
	} else {
		(void)urtwa_write_1_mask(sc, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN, 0);
		(void)urtwa_write_1_mask(sc, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF, 0);
	}
}

static int
urtwa_llt_write(struct urtwa_softc *sc, uint32_t addr, uint32_t data)
{
	uint32_t value;
	int n;

	value = BIT_LLT_WRITE_ACCESS | (addr << 8) | data;
	if (urtwa_write_4(sc, REG_LLT_INIT, value) != 0)
		return (EIO);

	for (n = 0; n < 20; n++) {
		if ((urtwa_read_4(sc, REG_LLT_INIT) &
		    (BIT_LLT_WRITE_ACCESS | (1U << 31))) == 0)
			return (0);
	}

	printf("%s: LLT write timeout at %u\n", sc->sc_dev.dv_xname, addr);
	return (ETIMEDOUT);
}

static int
urtwa_llt_init(struct urtwa_softc *sc)
{
	uint32_t i;
	int error;

	for (i = 0; i < sc->sc_rsvd_boundary - 1; i++) {
		error = urtwa_llt_write(sc, i, i + 1);
		if (error != 0)
			return (error);
	}
	error = urtwa_llt_write(sc, sc->sc_rsvd_boundary - 1, 0xff);
	if (error != 0)
		return (error);
	for (i = sc->sc_rsvd_boundary; i < URTWA_LLT_LAST_ENTRY; i++) {
		error = urtwa_llt_write(sc, i, i + 1);
		if (error != 0)
			return (error);
	}
	return (urtwa_llt_write(sc, URTWA_LLT_LAST_ENTRY, sc->sc_rsvd_boundary));
}

static int
urtwa_power_on(struct urtwa_softc *sc)
{
	uint16_t val16;
	int error;

	if (sc->sc_powered)
		return (0);

	urtwa_hw_reset(sc);

	(void)urtwa_write_1(sc, REG_RSV_CTRL, 0);
	if (urtwa_read_4(sc, REG_SYS_CFG1) & BIT_LDO)
		(void)urtwa_write_1(sc, REG_LDO_SWR_CTRL, LDO_SEL);
	else
		(void)urtwa_write_1(sc, REG_LDO_SWR_CTRL, SPS_SEL);

	error = urtwa_pwr_seq(sc, urtwa_carddis_to_cardemu);
	if (error != 0)
		return (error);
	error = urtwa_pwr_seq(sc, urtwa_cardemu_to_act);
	if (error != 0)
		return (error);

	if (urtwa_write_2(sc, REG_CR, 0) != 0)
		return (EIO);
	val16 = URTWA_TRX_ENABLE;
	if (urtwa_write_2(sc, REG_CR, val16) != 0)
		return (EIO);

	if (urtwa_read_1(sc, REG_SYS_CFG1 + 3) & BIT(0))
		(void)urtwa_write_1_mask(sc, REG_LDO_SWR_CTRL, BIT(6), BIT(6));

	sc->sc_page_size = 256;
	sc->sc_txff_pg_num = 65536 / sc->sc_page_size;
	sc->sc_rsvd_boundary = sc->sc_txff_pg_num - 8;
	error = urtwa_llt_init(sc);
	if (error != 0)
		return (error);

	(void)urtwa_write_4_mask(sc, REG_TXDMA_OFFSET_CHK, BIT_DROP_DATA_EN,
	    BIT_DROP_DATA_EN);

	sc->sc_powered = 1;
	return (0);
}

static void
urtwa_power_off(struct urtwa_softc *sc)
{
	(void)urtwa_write_1_mask(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN, 0);
	urtwa_wlan_cpu_enable(sc, 0);
	sc->sc_fw_running = 0;
	sc->sc_powered = 0;
}

static int
urtwa_write_firmware_page(struct urtwa_softc *sc, uint32_t page,
    const uint8_t *data, uint32_t size)
{
	uint32_t addr, n, block_size;
	int error;

	block_size = 196;
	addr = URTWA_FW_START_ADDR_LEGACY;

	error = urtwa_write_4_mask(sc, REG_MCUFW_CTRL, BIT_ROM_PGE,
	    (page << BIT_SHIFT_ROM_PGE) & BIT_ROM_PGE);
	if (error != 0)
		return (error);

	while (size > 0) {
		if (size >= block_size)
			n = block_size;
		else if (size >= 8)
			n = 8;
		else
			n = 1;

		error = urtwa_write_mem(sc, addr, (void *)data, n);
		if (error != 0)
			return (error);

		addr += n;
		data += n;
		size -= n;
	}

	return (0);
}

static int
urtwa_fw_download_legacy(struct urtwa_softc *sc)
{
	struct urtwa_fw_hdr_legacy *hdr;
	uint8_t *fw;
	size_t fwlen;
	size_t payload_len;
	size_t last_page;
	uint8_t *data;
	uint32_t page, pages;
	uint32_t val32;
	int error, n;

	fw = NULL;
	fwlen = 0;
	error = loadfirmware(URTWA_FW_NAME, &fw, &fwlen);
	if (error != 0) {
		printf("%s: could not read firmware %s (error %d)\n",
		    sc->sc_dev.dv_xname, URTWA_FW_NAME, error);
		urtwa_power_off(sc);
		return (EAGAIN);
	}
	if (fwlen < sizeof(*hdr)) {
		printf("%s: short firmware image\n", sc->sc_dev.dv_xname);
		error = EINVAL;
		goto done;
	}

	hdr = (struct urtwa_fw_hdr_legacy *)fw;
	if (letoh16(hdr->signature) != URTWA_FW_SIGNATURE) {
		printf("%s: unexpected firmware signature %#x\n",
		    sc->sc_dev.dv_xname, letoh16(hdr->signature));
		error = EINVAL;
		goto done;
	}

	payload_len = fwlen - sizeof(*hdr);
	data = fw + sizeof(*hdr);
	pages = payload_len >> URTWA_DLFW_PAGE_SIZE_SHIFT;
	last_page = payload_len & (URTWA_DLFW_PAGE_SIZE_LEGACY - 1);

	urtwa_wlan_cpu_enable(sc, 0);
	urtwa_wlan_cpu_enable(sc, 1);

	(void)urtwa_write_1_mask(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN,
	    BIT_MCUFWDL_EN);
	for (n = 0; n < 10; n++) {
		if (urtwa_read_1(sc, REG_MCUFW_CTRL) & BIT_MCUFWDL_EN)
			break;
		(void)urtwa_write_1_mask(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN,
		    BIT_MCUFWDL_EN);
		usbd_delay_ms(sc->sc_udev, 20);
	}
	if (!(urtwa_read_1(sc, REG_MCUFW_CTRL) & BIT_MCUFWDL_EN)) {
		printf("%s: firmware download enable failed\n", sc->sc_dev.dv_xname);
		error = ETIMEDOUT;
		goto done;
	}
	(void)urtwa_write_4_mask(sc, REG_MCUFW_CTRL, BIT_ROM_DLEN, 0);
	(void)urtwa_write_1_mask(sc, REG_MCUFW_CTRL, BIT_FWDL_CHK_RPT,
	    BIT_FWDL_CHK_RPT);

	for (page = 0; page < pages; page++) {
		uint32_t offset = page * URTWA_DLFW_PAGE_SIZE_LEGACY;
		error = urtwa_write_firmware_page(sc, page, data + offset,
		    URTWA_DLFW_PAGE_SIZE_LEGACY);
		if (error != 0)
			goto done;
	}
	if (last_page != 0) {
		uint32_t offset = pages * URTWA_DLFW_PAGE_SIZE_LEGACY;
		error = urtwa_write_firmware_page(sc, page, data + offset,
		    last_page);
		if (error != 0)
			goto done;
	}
	if (urtwa_wait_1(sc, REG_MCUFW_CTRL, BIT_FWDL_CHK_RPT,
	    BIT_FWDL_CHK_RPT, 50) != 0) {
		printf("%s: firmware download report timeout\n", sc->sc_dev.dv_xname);
		error = ETIMEDOUT;
		goto done;
	}

	(void)urtwa_write_1_mask(sc, REG_MCUFW_CTRL, BIT_MCUFWDL_EN, 0);

	val32 = urtwa_read_4(sc, REG_MCUFW_CTRL);
	val32 |= BIT_MCUFWDL_RDY;
	val32 &= ~BIT_WINTINI_RDY;
	(void)urtwa_write_4(sc, REG_MCUFW_CTRL, val32);

	urtwa_wlan_cpu_enable(sc, 0);
	urtwa_wlan_cpu_enable(sc, 1);

	error = ETIMEDOUT;
	for (n = 0; n < 10; n++) {
		val32 = urtwa_read_4(sc, REG_MCUFW_CTRL);
		if ((val32 & FW_READY_LEGACY) == FW_READY_LEGACY) {
			error = 0;
			break;
		}
		usbd_delay_ms(sc->sc_udev, 20);
	}
	if (error == 0) {
		sc->sc_fw_running = 1;
		printf("%s: firmware running version %#x sub %#x.%#x\n",
		    sc->sc_dev.dv_xname, letoh16(hdr->version),
		    hdr->subversion1, hdr->subversion2);
	} else {
		printf("%s: firmware validate timeout fwctrl=0x%08x\n",
		    sc->sc_dev.dv_xname, urtwa_read_4(sc, REG_MCUFW_CTRL));
	}

done:
	if (error != 0)
		urtwa_power_off(sc);
	if (fw != NULL)
		free(fw, M_DEVBUF, fwlen);
	return (error);
}

#define URTWA_EFUSE_PHYS_SIZE		512
#define URTWA_EFUSE_LOG_SIZE		512

static void
urtwa_efuse_grant(struct urtwa_softc *sc, int on)
{
	if (on) {
		(void)urtwa_write_1(sc, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON);
		(void)urtwa_write_2_mask(sc, REG_SYS_FUNC_EN, BIT_FEN_ELDR,
		    BIT_FEN_ELDR);
		(void)urtwa_write_1_mask(sc, REG_SYS_CLKR,
		    BIT_LOADER_CLK_EN | BIT_ANA8M,
		    BIT_LOADER_CLK_EN | BIT_ANA8M);
	} else {
		(void)urtwa_write_1(sc, REG_EFUSE_ACCESS, EFUSE_ACCESS_OFF);
	}
}

static int
urtwa_read_efuse(struct urtwa_softc *sc)
{
	struct urtwa_efuse_map *map;
	uint8_t *phy_map, *log_map;
	uint32_t efuse_ctl, phy_idx, log_idx, cnt;
	uint8_t hdr1, hdr2, blk_idx, word_en;
	int i, error;

	phy_map = malloc(URTWA_EFUSE_PHYS_SIZE, M_DEVBUF, M_WAITOK);
	log_map = malloc(URTWA_EFUSE_LOG_SIZE, M_DEVBUF, M_WAITOK);
	if (phy_map == NULL || log_map == NULL) {
		error = ENOMEM;
		goto out;
	}

	urtwa_efuse_grant(sc, 1);
	(void)urtwa_write_4_mask(sc, REG_LDO_EFUSE_CTRL,
	    BIT_MASK_EFUSE_BANK_SEL, 0);

	efuse_ctl = urtwa_read_4(sc, REG_EFUSE_CTRL);
	for (phy_idx = 0; phy_idx < URTWA_EFUSE_PHYS_SIZE; phy_idx++) {
		efuse_ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
		efuse_ctl |= (phy_idx & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
		if (urtwa_write_4(sc, REG_EFUSE_CTRL, efuse_ctl & ~BIT_EF_FLAG) !=
		    0) {
			error = EIO;
			goto out;
		}

		cnt = 1000000;
		do {
			DELAY(1);
			efuse_ctl = urtwa_read_4(sc, REG_EFUSE_CTRL);
			if (--cnt == 0) {
				printf("%s: efuse read timeout at %#x\n",
				    sc->sc_dev.dv_xname, phy_idx);
				error = ETIMEDOUT;
				goto out;
			}
		} while (!(efuse_ctl & BIT_EF_FLAG));

		phy_map[phy_idx] = efuse_ctl & BIT_MASK_EF_DATA;
	}

	memset(log_map, 0xff, URTWA_EFUSE_LOG_SIZE);
	for (phy_idx = 0; phy_idx < URTWA_EFUSE_PHYS_SIZE;) {
		hdr1 = phy_map[phy_idx];
		hdr2 = (phy_idx + 1 < URTWA_EFUSE_PHYS_SIZE) ?
		    phy_map[phy_idx + 1] : 0xff;
		if (hdr1 == 0xff || (((hdr1 & 0x1f) == 0x0f) && hdr2 == 0xff))
			break;

		if ((hdr1 & 0x1f) == 0x0f) {
			blk_idx = ((hdr2 & 0xf0) >> 1) | ((hdr1 >> 5) & 0x07);
			word_en = hdr2 & 0x0f;
			phy_idx += 2;
		} else {
			blk_idx = (hdr1 & 0xf0) >> 4;
			word_en = hdr1 & 0x0f;
			phy_idx += 1;
		}

		for (i = 0; i < 4; i++) {
			if (word_en & BIT(i))
				continue;

			log_idx = (blk_idx << 3) + (i << 1);
			if (phy_idx + 1 >= URTWA_EFUSE_PHYS_SIZE ||
			    log_idx + 1 >= URTWA_EFUSE_LOG_SIZE) {
				error = EINVAL;
				goto out;
			}

			log_map[log_idx] = phy_map[phy_idx];
			log_map[log_idx + 1] = phy_map[phy_idx + 1];
			phy_idx += 2;
		}
	}

	map = (struct urtwa_efuse_map *)log_map;
	memcpy(sc->sc_macaddr, map->rtw8821au.mac_addr,
	    sizeof(sc->sc_macaddr));
	sc->sc_efuse.rf_board_option = map->rf_board_option == 0xff ? 0 :
	    map->rf_board_option;
	sc->sc_efuse.channel_plan = map->channel_plan == 0xff ? 0x7f :
	    map->channel_plan;
	sc->sc_efuse.crystal_cap = map->xtal_k == 0xff ? 0x20 : map->xtal_k;
	sc->sc_efuse.pa_type_2g = map->pa_type == 0xff ? 0 : map->pa_type;
	sc->sc_efuse.pa_type_5g = sc->sc_efuse.pa_type_2g;
	sc->sc_efuse.lna_type_2g = map->lna_type_2g == 0xff ? 0 :
	    map->lna_type_2g;
	sc->sc_efuse.lna_type_5g = map->lna_type_5g == 0xff ? 0 :
	    map->lna_type_5g;
	sc->sc_efuse.bt_setting = map->rf_bt_setting;
	sc->sc_efuse.tx_bb_swing_setting_2g =
	    map->tx_bb_swing_setting_2g == 0xff ? 0 :
	    map->tx_bb_swing_setting_2g;
	sc->sc_efuse.tx_bb_swing_setting_5g =
	    map->tx_bb_swing_setting_5g == 0xff ? 0 :
	    map->tx_bb_swing_setting_5g;
	sc->sc_efuse.rfe_option = map->rfe_option == 0xff ? 0 : map->rfe_option;
	sc->sc_efuse.btcoex =
	    (sc->sc_efuse.rf_board_option & 0xe0) == 0x20 ? 1 : 0;
	sc->sc_efuse.ext_pa_2g =
	    (sc->sc_efuse.pa_type_2g & BIT(4)) ? 1 : 0;
	sc->sc_efuse.ext_lna_2g =
	    (sc->sc_efuse.lna_type_2g & BIT(3)) ? 1 : 0;
	sc->sc_efuse.ext_pa_5g =
	    (sc->sc_efuse.pa_type_5g & BIT(0)) ? 1 : 0;
	sc->sc_efuse.ext_lna_5g =
	    (sc->sc_efuse.lna_type_5g & BIT(3)) ? 1 : 0;
	printf("%s: mac %02x:%02x:%02x:%02x:%02x:%02x\n",
	    sc->sc_dev.dv_xname,
	    sc->sc_macaddr[0], sc->sc_macaddr[1], sc->sc_macaddr[2],
	    sc->sc_macaddr[3], sc->sc_macaddr[4], sc->sc_macaddr[5]);
	error = 0;

out:
	urtwa_efuse_grant(sc, 0);
	if (phy_map != NULL)
		free(phy_map, M_DEVBUF, URTWA_EFUSE_PHYS_SIZE);
	if (log_map != NULL)
		free(log_map, M_DEVBUF, URTWA_EFUSE_LOG_SIZE);
	return (error);
}

static int
urtwa_ensure_fw(struct urtwa_softc *sc)
{
	int error;

	if (!sc->sc_powered) {
		error = urtwa_power_on(sc);
		if (error != 0)
			return (error);
	}
	if (!sc->sc_efuse_valid) {
		error = urtwa_read_efuse(sc);
		if (error != 0) {
			urtwa_power_off(sc);
			return (error);
		}
		sc->sc_efuse_valid = 1;
	}
	if (sc->sc_fw_running) {
		sc->sc_fw_deferred = 0;
		return (0);
	}
	error = urtwa_fw_download_legacy(sc);
	if (error != 0)
		return (error);
	sc->sc_fw_deferred = 0;
	return (0);
}

static uint32_t
urtwa_read_rf(struct urtwa_softc *sc, uint8_t rf_path, uint32_t addr,
    uint32_t mask)
{
	static const uint16_t pi_addr[2] = { REG_3WIRE_SWA, REG_3WIRE_SWB };
	static const uint16_t read_addr[2][2] = {
		{ REG_SI_READ_A, REG_SI_READ_B },
		{ REG_PI_READ_A, REG_PI_READ_B },
	};
	uint32_t val;
	uint32_t shift;
	int pi_mode;

	if (rf_path > 1)
		return (0xffffffff);

	addr &= 0xff;
	pi_mode = (urtwa_read_4(sc, pi_addr[rf_path]) & BIT(2)) ? 1 : 0;
	if (urtwa_write_4_mask(sc, REG_HSSI_READ, MASKBYTE0, addr) != 0)
		return (0xffffffff);
	DELAY(20);
	val = urtwa_read_4(sc, read_addr[pi_mode][rf_path]);
	if (mask == RFREG_MASK)
		return (val & mask);
	shift = ffs(mask);
	if (shift == 0)
		return (0);
	return ((val & mask) >> (shift - 1));
}

static int
urtwa_write_rf(struct urtwa_softc *sc, uint8_t rf_path, uint32_t addr,
    uint32_t mask, uint32_t data)
{
	static const uint16_t sipi_addr[2] = { REG_LSSI_WRITE_A, REG_LSSI_WRITE_B };
	uint32_t data_and_addr;
	uint32_t old_data;
	uint32_t shift;

	if (rf_path > 1)
		return (EINVAL);

	addr &= 0xff;
	mask &= RFREG_MASK;
	if (mask != RFREG_MASK) {
		old_data = urtwa_read_rf(sc, rf_path, addr, RFREG_MASK);
		if (old_data == 0xffffffff)
			return (EIO);
		shift = ffs(mask);
		if (shift == 0)
			return (EINVAL);
		data = (old_data & ~mask) | ((data << (shift - 1)) & mask);
	}

	data_and_addr = ((addr << 20) | (data & RFREG_MASK)) & 0x0fffffff;
	if (urtwa_write_4(sc, sipi_addr[rf_path], data_and_addr) != 0)
		return (EIO);
	DELAY(13);
	return (0);
}

static void
urtwa_setup_phy_cond(struct urtwa_softc *sc, struct urtwa_phy_cond *cond,
    struct urtwa_phy_cond2 *cond2)
{
	memset(cond, 0, sizeof(*cond));
	memset(cond2, 0, sizeof(*cond2));

	cond->cut = 15;
	cond->pkg = 15;
	cond->plat = 0x4;
	cond->intf = URTWA_INTF_USB;
	cond->rfe = sc->sc_efuse.ext_lna_2g;
	cond->rfe |= sc->sc_efuse.ext_pa_2g << 1;
	cond->rfe |= sc->sc_efuse.ext_lna_5g << 2;
	cond->rfe |= sc->sc_efuse.ext_pa_5g << 3;
	cond->rfe |= sc->sc_efuse.btcoex << 4;

	cond2->type_alna = sc->sc_efuse.alna_type;
	cond2->type_glna = sc->sc_efuse.glna_type;
	cond2->type_apa = sc->sc_efuse.apa_type;
	cond2->type_gpa = sc->sc_efuse.gpa_type;
}

static int
urtwa_check_positive(struct urtwa_softc *sc, struct urtwa_phy_cond cond,
    struct urtwa_phy_cond2 cond2)
{
	struct urtwa_phy_cond drv_cond;
	struct urtwa_phy_cond2 drv_cond2;

	urtwa_setup_phy_cond(sc, &drv_cond, &drv_cond2);

	if (cond.cut && cond.cut != drv_cond.cut)
		return (0);
	if (cond.pkg && cond.pkg != drv_cond.pkg)
		return (0);
	if (cond.intf && cond.intf != drv_cond.intf)
		return (0);
	if (!(cond.rfe & 0x0f))
		return (1);
	if ((cond.rfe & drv_cond.rfe) != cond.rfe)
		return (0);
	if ((cond.rfe & BIT(0)) && cond2.type_glna != drv_cond2.type_glna)
		return (0);
	if ((cond.rfe & BIT(1)) && cond2.type_gpa != drv_cond2.type_gpa)
		return (0);
	if ((cond.rfe & BIT(2)) && cond2.type_alna != drv_cond2.type_alna)
		return (0);
	if ((cond.rfe & BIT(3)) && cond2.type_apa != drv_cond2.type_apa)
		return (0);
	return (1);
}

static int
urtwa_apply_table_cfg(struct urtwa_softc *sc, const struct urtwa_table *tbl,
    uint32_t addr, uint32_t data)
{
	switch (tbl->kind) {
	case URTWA_TBL_MAC:
		return (urtwa_write_1(sc, addr, data));
	case URTWA_TBL_AGC:
		return (urtwa_write_4(sc, addr, data));
	case URTWA_TBL_BB:
		if (addr == 0xfe)
			usbd_delay_ms(sc->sc_udev, 50);
		else if (addr == 0xfd)
			DELAY(5000);
		else if (addr == 0xfc)
			DELAY(1000);
		else if (addr == 0xfb)
			DELAY(50);
		else if (addr == 0xfa)
			DELAY(5);
		else if (addr == 0xf9)
			DELAY(1);
		else if (urtwa_write_4(sc, addr, data) != 0)
			return (EIO);
		return (0);
	case URTWA_TBL_RF:
		if (addr == 0xffe)
			usbd_delay_ms(sc->sc_udev, 50);
		else if (addr == 0xfe)
			DELAY(100);
		else if (urtwa_write_rf(sc, tbl->rf_path, addr, RFREG_MASK,
		    data) != 0)
			return (EIO);
		else
			DELAY(1);
		return (0);
	}

	return (EINVAL);
}

static int
urtwa_load_table(struct urtwa_softc *sc, const struct urtwa_table *tbl)
{
	const union urtwa_phy_table_tile *p, *end;
	struct urtwa_phy_cond pos_cond = {0};
	struct urtwa_phy_cond2 pos_cond2 = {0};
	int is_matched = 1, is_skipped = 0;

	p = (const union urtwa_phy_table_tile *)tbl->data;
	end = p + tbl->size / 2;

	for (; p < end; p++) {
		if (p->cond.pos) {
			switch (p->cond.branch) {
			case URTWA_BRANCH_ENDIF:
				is_matched = 1;
				is_skipped = 0;
				break;
			case URTWA_BRANCH_ELSE:
				is_matched = is_skipped ? 0 : 1;
				break;
			case URTWA_BRANCH_IF:
			case URTWA_BRANCH_ELIF:
			default:
				pos_cond = p->cond;
				pos_cond2 = p->cond2;
				break;
			}
		} else if (p->cond.neg) {
			if (!is_skipped) {
				if (urtwa_check_positive(sc, pos_cond, pos_cond2)) {
					is_matched = 1;
					is_skipped = 1;
				} else {
					is_matched = 0;
					is_skipped = 0;
				}
			} else {
				is_matched = 0;
			}
		} else if (is_matched) {
			if (urtwa_apply_table_cfg(sc, tbl, p->cfg.addr,
			    p->cfg.data) != 0)
				return (EIO);
		}
	}

	return (0);
}

static int
urtwa_set_channel(struct urtwa_softc *sc, uint8_t channel)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t fc_area, rf_mod_ag;
	uint8_t txsc = 0;

	if (channel == 0)
		channel = 1;

	if (channel > 14) {
		printf("%s: phase3 set_channel only supports 2G for now "
		    "(requested %u)\n", sc->sc_dev.dv_xname, channel);
		return (EOPNOTSUPP);
	}

	if (urtwa_write_1_mask(sc, REG_CCK_CHECK, BIT_CHECK_CCK_EN, 0) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_RXPSEL, BIT_RX_PSEL_RST,
	    BIT_RX_PSEL_RST) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_TXPSEL, 0xf0, 0x10) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_CCK_RX, 0x0f000000, 0x01000000) != 0)
		return (EIO);

	if (urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0xF000, 0x7 << 12) != 0 ||
	    urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0xF0, 0x7 << 4) != 0)
		return (EIO);
	if (sc->sc_efuse.ext_lna_2g) {
		if (urtwa_write_4_mask(sc, REG_RFE_INV_A, BIT(20), BIT(20)) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_INV_A, BIT(22), 0) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0x7, 0x2) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0x700, 0x200) != 0)
			return (EIO);
	} else {
		if (urtwa_write_4_mask(sc, REG_RFE_INV_A, BIT(20), 0) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_INV_A, BIT(22), 0) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0x7, 0x7) != 0 ||
		    urtwa_write_4_mask(sc, REG_RFE_PINMUX_A, 0x700, 0x700) != 0)
			return (EIO);
	}
	if (urtwa_write_4_mask(sc, REG_TXSCALE_A, 0xf00, 0) != 0)
		return (EIO);

	switch (channel) {
	case 1 ... 14:
	default:
		fc_area = 0x96a;
		rf_mod_ag = 0x000;
		break;
	}
	if (urtwa_write_4_mask(sc, REG_CLKTRK, 0x1ffe0000,
	    fc_area << 17) != 0)
		return (EIO);
	if (urtwa_write_rf(sc, URTWA_RF_PATH_A, RF_CFGCH,
	    RF18_RFSI_MASK | RF18_BAND_MASK, rf_mod_ag) != 0)
		return (EIO);
	if (urtwa_write_rf(sc, URTWA_RF_PATH_A, RF_CFGCH,
	    RF18_CHANNEL_MASK, channel) != 0)
		return (EIO);
	if (urtwa_write_rf(sc, URTWA_RF_PATH_A, RF_CFGCH,
	    RF18_BW_MASK, 3) != 0)
		return (EIO);

	if (urtwa_write_2_mask(sc, REG_WMAC_TRXPTCL_CTL, BIT_RFMOD, 0) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_DATA_SC, BIT_TXSC_20M(txsc) |
	    BIT_TXSC_40M(0)) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_ADCCLK, 0x003003C3, 0x00300200) != 0 ||
	    urtwa_write_4_mask(sc, REG_ADC160, BIT(30), 0) != 0 ||
	    urtwa_write_4_mask(sc, REG_L1PKTH, 0x03c00000,
	    8 << 22) != 0)
		return (EIO);

	sc->sc_curchan = channel;
	ic->ic_ibss_chan = &ic->ic_channels[channel];
	printf("%s: phy init channel %u\n", sc->sc_dev.dv_xname, channel);
	return (0);
}

static int
urtwa_phy_init(struct urtwa_softc *sc)
{
	uint8_t channel;
	uint8_t val8;

	val8 = urtwa_read_1(sc, REG_SYS_FUNC_EN);
	val8 |= BIT_FEN_USBA;
	if (urtwa_write_1(sc, REG_SYS_FUNC_EN, val8) != 0)
		return (EIO);
	val8 |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	if (urtwa_write_1(sc, REG_SYS_FUNC_EN, val8) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_RF_CTRL,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB) != 0 ||
	    urtwa_write_1(sc, REG_RF_B_CTRL,
	    BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB) != 0)
		return (EIO);

	if (urtwa_load_table(sc, &urtwa_bb_tbl) != 0 ||
	    urtwa_load_table(sc, &urtwa_agc_tbl) != 0)
		return (EIO);

	if (urtwa_write_4_mask(sc, REG_AFE_CTRL3, 0x00FFF000,
	    (((sc->sc_efuse.crystal_cap & 0x3f) |
	    ((sc->sc_efuse.crystal_cap & 0x3f) << 6)) << 12)) != 0)
		return (EIO);
	if (urtwa_load_table(sc, &urtwa_rf_a_tbl) != 0)
		return (EIO);

	channel = urtwa_desired_channel(sc);
	return (urtwa_set_channel(sc, channel));
}

static int
urtwa_attach_ifnet(struct urtwa_softc *sc)
{
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int i;

	ic = &sc->sc_ic;
	ifp = &ic->ic_if;

	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps = IEEE80211_C_SCANALL | IEEE80211_C_MONITOR |
	    IEEE80211_C_QOS | IEEE80211_C_TX_AMPDU |
	    IEEE80211_C_WEP | IEEE80211_C_RSN |
	    IEEE80211_C_SHSLOT | IEEE80211_C_SHPREAMBLE;
	ic->ic_max_rssi = 100;
	ic->ic_lintval = 5;
	ic->ic_htcaps = IEEE80211_HTCAP_SGI20 |
	    IEEE80211_HTCAP_SGI40 |
	    IEEE80211_HTCAP_CBW20_40;
	ic->ic_ampdu_params = IEEE80211_AMPDU_PARAM_SS_4 | 0x3;
	ic->ic_sup_mcs[0] = 0xff;

	ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;

	for (i = 1; i <= 13; i++) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ |
		    IEEE80211_CHAN_HT | IEEE80211_CHAN_40MHZ;
	}
	ic->ic_channels[14].ic_freq =
	    ieee80211_ieee2mhz(14, IEEE80211_CHAN_2GHZ);
	ic->ic_channels[14].ic_flags =
	    IEEE80211_CHAN_CCK | IEEE80211_CHAN_2GHZ;

	for (i = 36; i <= 64; i += 4) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A |
		    IEEE80211_CHAN_HT | IEEE80211_CHAN_40MHZ;
	}
	for (i = 100; i <= 144; i += 4) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A |
		    IEEE80211_CHAN_HT | IEEE80211_CHAN_40MHZ;
	}
	for (i = 149; i <= 165; i += 4) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_5GHZ);
		ic->ic_channels[i].ic_flags = IEEE80211_CHAN_A |
		    IEEE80211_CHAN_HT | IEEE80211_CHAN_40MHZ;
	}
	ic->ic_ibss_chan = &ic->ic_channels[1];
	IEEE80211_ADDR_COPY(ic->ic_myaddr, sc->sc_macaddr);

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = urtwa_ioctl;
	ifp->if_start = urtwa_start;
	strlcpy(ifp->if_xname, sc->sc_dev.dv_xname, IFNAMSIZ);

	if_attach(ifp);
	sc->sc_if_attached = 1;
	ieee80211_ifattach(ifp);
	timeout_set(&sc->sc_scan_to, urtwa_scan_to, sc);
	usb_init_task(&sc->sc_task, urtwa_task, sc, USB_TASK_TYPE_GENERIC);
	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = urtwa_newstate;
	sc->sc_send_mgmt = ic->ic_send_mgmt;
	ic->ic_send_mgmt = urtwa_send_mgmt;
	sc->sc_set_key = ic->ic_set_key;
	ic->ic_set_key = urtwa_set_key;
	sc->sc_delete_key = ic->ic_delete_key;
	ic->ic_delete_key = urtwa_delete_key;
	ieee80211_media_init(ifp, ieee80211_media_change,
	    ieee80211_media_status);

	printf("%s: ifnet attached, passive RX phase next\n",
	    sc->sc_dev.dv_xname);
	return (0);
}

static int
urtwa_newstate(struct ieee80211com *ic, enum ieee80211_state nstate, int arg)
{
	struct urtwa_softc *sc = ic->ic_if.if_softc;
	enum ieee80211_state ostate;
	int error;

	ostate = ic->ic_state;
	if (sc->sc_running && nstate == IEEE80211_S_SCAN)
		urtwa_scan_set_channel(sc);
	if (sc->sc_tx_mgmt_log < URTWA_TX_TRACE_LOG_MAX) {
		printf("%s: newstate %d->%d ascan=%d arg=%d\n",
		    sc->sc_dev.dv_xname, ic->ic_state, nstate,
		    (ic->ic_flags & IEEE80211_F_ASCAN) != 0, arg);
		sc->sc_tx_mgmt_log++;
	}
	error = (*sc->sc_newstate)(ic, nstate, arg);
	if (error == 0 && ic->ic_state != IEEE80211_S_SCAN)
		ic->ic_flags &= ~IEEE80211_F_ASCAN;
	if (error == 0 &&
	    (ic->ic_state == IEEE80211_S_INIT || ic->ic_state == IEEE80211_S_SCAN))
		urtwa_sta_link_drop(sc);
	if (error == 0 && sc->sc_running && ic->ic_state != ostate) {
		if (ic->ic_state == IEEE80211_S_RUN) {
			sc->sc_link_pending = 1;
			sc->sc_link_connect = 1;
			usb_add_task(sc->sc_udev, &sc->sc_task);
		} else if (ostate == IEEE80211_S_RUN) {
			sc->sc_link_pending = 1;
			sc->sc_link_connect = 0;
			usb_add_task(sc->sc_udev, &sc->sc_task);
		}
	}
	if (nstate == IEEE80211_S_SCAN && !usbd_is_dying(sc->sc_udev))
		timeout_add_msec(&sc->sc_scan_to, 200);
	else
		urtwa_cancel_scan(sc);
	return (error);
}

static int
urtwa_send_mgmt(struct ieee80211com *ic, struct ieee80211_node *ni, int type,
    int arg1, int arg2)
{
	struct urtwa_softc *sc = ic->ic_if.if_softc;
	const char *name;
	int subtype;

	subtype = type & IEEE80211_FC0_SUBTYPE_MASK;
	switch (subtype) {
	case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
		name = "probe request";
		break;
	case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
		name = "probe response";
		break;
	case IEEE80211_FC0_SUBTYPE_AUTH:
		name = "auth";
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
		name = "assoc request";
		break;
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
		name = "assoc response";
		break;
	default:
		name = NULL;
		break;
	}
	if (name != NULL && sc->sc_tx_mgmt_log < URTWA_TX_TRACE_LOG_MAX) {
		printf("%s: send_mgmt %s state=%d ascan=%d mgtq=%d\n",
		    sc->sc_dev.dv_xname, name, ic->ic_state,
		    (ic->ic_flags & IEEE80211_F_ASCAN) != 0, mq_len(&ic->ic_mgtq));
		sc->sc_tx_mgmt_log++;
	}
	if (ni != NULL && subtype == IEEE80211_FC0_SUBTYPE_AUTH) {
		(void)urtwa_sta_link_prep(sc, ni->ni_bssid);
	}
	return ((*sc->sc_send_mgmt)(ic, ni, type, arg1, arg2));
}

static const char *
urtwa_cipher_name(const struct ieee80211_key *k)
{
	switch (k->k_cipher) {
	case IEEE80211_CIPHER_USEGROUP:
		return ("usegroup");
	case IEEE80211_CIPHER_WEP40:
		return ("wep40");
	case IEEE80211_CIPHER_TKIP:
		return ("tkip");
	case IEEE80211_CIPHER_CCMP:
		return ("ccmp");
	case IEEE80211_CIPHER_WEP104:
		return ("wep104");
	case IEEE80211_CIPHER_BIP:
		return ("bip");
	default:
		return ("unknown");
	}
}

static int
urtwa_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwa_softc *sc = ic->ic_if.if_softc;
	int error;

	error = (*sc->sc_set_key)(ic, ni, k);
	if (sc->sc_key_log < URTWA_TX_TRACE_LOG_MAX) {
		printf("%s: set_key cipher=%s id=%u flags=%#x len=%u ni=%p error=%d\n",
		    sc->sc_dev.dv_xname, urtwa_cipher_name(k), k->k_id,
		    k->k_flags, k->k_len, ni, error);
		sc->sc_key_log++;
	}
	return (error);
}

static void
urtwa_delete_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	struct urtwa_softc *sc = ic->ic_if.if_softc;

	if (sc->sc_key_log < URTWA_TX_TRACE_LOG_MAX) {
		printf("%s: delete_key cipher=%s id=%u flags=%#x ni=%p\n",
		    sc->sc_dev.dv_xname, urtwa_cipher_name(k), k->k_id,
		    k->k_flags, ni);
		sc->sc_key_log++;
	}
	(*sc->sc_delete_key)(ic, ni, k);
}

static uint8_t
urtwa_desired_channel(struct urtwa_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	uint8_t ieee;

	chan = ic->ic_des_chan;
	if (chan == IEEE80211_CHAN_ANYC || chan == NULL)
		chan = ic->ic_ibss_chan;
	if (chan == NULL)
		return (1);
	ieee = ieee80211_chan2ieee(ic, chan);
	if (ieee == 0)
		return (1);
	return (ieee);
}

static void
urtwa_scan_set_channel(struct urtwa_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;
	uint8_t ieee;

	chan = ic->ic_bss->ni_chan;
	if (chan == IEEE80211_CHAN_ANYC || chan == NULL)
		return;
	ieee = ieee80211_chan2ieee(ic, chan);
	if (ieee == 0 || ieee == sc->sc_curchan)
		return;
	(void)urtwa_set_channel(sc, ieee);
}

static void
urtwa_task(void *arg)
{
	struct urtwa_softc *sc = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	int connect;
	int s;

	if (usbd_is_dying(sc->sc_udev))
		return;
	if (!sc->sc_scan_pending && !sc->sc_link_pending)
		return;

	if (sc->sc_scan_pending) {
		sc->sc_scan_pending = 0;

		s = splnet();
		if (ic->ic_state == IEEE80211_S_SCAN) {
			if (ic->ic_des_chan != IEEE80211_CHAN_ANYC)
				ieee80211_end_scan(ifp);
			else
				ieee80211_next_scan(ifp);
		}
		splx(s);
		if (sc->sc_running && ic->ic_state == IEEE80211_S_SCAN)
			urtwa_scan_set_channel(sc);
	}

	if (sc->sc_link_pending && sc->sc_fw_running) {
		sc->sc_link_pending = 0;
		connect = sc->sc_link_connect;
		if (connect) {
			(void)urtwa_fw_default_port(sc);
			(void)urtwa_fw_media_status(sc, 1);
		} else
			(void)urtwa_fw_media_status(sc, 0);
	}
}

static void
urtwa_kick_state(struct urtwa_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *chan;

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		ieee80211_new_state(ic, IEEE80211_S_RUN, -1);
		return;
	}

	chan = ic->ic_des_chan;
	if (chan == IEEE80211_CHAN_ANYC || chan == NULL)
		chan = ic->ic_ibss_chan;
	if (ic->ic_bss != NULL && chan != IEEE80211_CHAN_ANYC && chan != NULL)
		ic->ic_bss->ni_chan = chan;

	if (ic->ic_des_esslen != 0 ||
	    (ic->ic_flags & IEEE80211_F_DESBSSID) != 0 ||
	    (ic->ic_flags & IEEE80211_F_AUTO_JOIN) != 0) {
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	} else if (ic->ic_state == IEEE80211_S_INIT &&
	    ic->ic_des_chan == IEEE80211_CHAN_ANYC) {
		ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	}
}

static void
urtwa_scan_to(void *arg)
{
	struct urtwa_softc *sc = arg;

	if (usbd_is_dying(sc->sc_udev))
		return;
	sc->sc_scan_pending = 1;
	usb_add_task(sc->sc_udev, &sc->sc_task);
}

static void
urtwa_cancel_scan(struct urtwa_softc *sc)
{
	if (timeout_initialized(&sc->sc_scan_to))
		timeout_del(&sc->sc_scan_to);
	sc->sc_scan_pending = 0;
}

static int
urtwa_runtime_init(struct urtwa_softc *sc)
{
	uint16_t txdma_pq_map;
	uint16_t pubq_num;
	uint32_t val32;
	uint8_t boundary;

	/*
	 * Linux 8821A USB, 4 bulk-out endpoints:
	 * page_table[4] = { hq 8, nq 0, lq 8, exq 4, gapq 1 }
	 * rqpn_table[4] = { vo normal, vi normal, be low,
	 *                   bk low, mg extra, hi high }
	 */
	boundary = sc->sc_rsvd_boundary;
	pubq_num = sc->sc_txff_pg_num - 8 - 8 - 0 - 4 - 1 - 8;

	if (urtwa_write_1(sc, REG_HMETFR, 0x0f) != 0)
		return (EIO);

	val32 = BIT_RQPN_NE(0, 4);
	if (urtwa_write_4(sc, REG_RQPN_NPQ, val32) != 0)
		return (EIO);
	val32 = BIT_RQPN_HLP(8, 8, pubq_num);
	if (urtwa_write_4(sc, REG_RQPN, val32) != 0)
		return (EIO);

	if (urtwa_write_1(sc, REG_BCNQ_BDNY, boundary) != 0 ||
	    urtwa_write_1(sc, REG_MGQ_BDNY, boundary) != 0 ||
	    urtwa_write_1(sc, REG_WMAC_LBK_BF_HD, boundary) != 0 ||
	    urtwa_write_1(sc, REG_TRXFF_BNDY, boundary) != 0 ||
	    urtwa_write_1(sc, REG_DWBCN0_CTRL + 1, boundary) != 0)
		return (EIO);
	if (urtwa_write_2(sc, REG_TRXFF_BNDY + 2, 16128 - 128 - 1) != 0)
		return (EIO);

	txdma_pq_map = urtwa_read_2(sc, REG_TXDMA_PQ_MAP) & 0x7;
	txdma_pq_map |= BIT_TXDMA_HIQ_MAP(3);
	txdma_pq_map |= BIT_TXDMA_MGQ_MAP(0);
	txdma_pq_map |= BIT_TXDMA_BKQ_MAP(1);
	txdma_pq_map |= BIT_TXDMA_BEQ_MAP(1);
	txdma_pq_map |= BIT_TXDMA_VIQ_MAP(2);
	txdma_pq_map |= BIT_TXDMA_VOQ_MAP(2);
	if (urtwa_write_2(sc, REG_TXDMA_PQ_MAP, txdma_pq_map) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_HIQ_NO_LMT_EN, 0xff) != 0)
		return (EIO);

	if (urtwa_write_1(sc, REG_RX_DRVINFO_SZ, 4) != 0)
		return (EIO);
	if (urtwa_write_4(sc, REG_HIMR0, 0) != 0 ||
	    urtwa_write_4(sc, REG_HIMR1, 0) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_CR, URTWA_PORT0_NET_TYPE_MASK,
	    urtwa_encode_bits(URTWA_NET_NO_LINK, URTWA_PORT0_NET_TYPE_MASK)) != 0)
		return (EIO);

	if (urtwa_write_2(sc, REG_RXFLTMAP0, 0xffff) != 0 ||
	    urtwa_write_2(sc, REG_RXFLTMAP1, 0x0400) != 0 ||
	    urtwa_write_2(sc, REG_RXFLTMAP2, 0xffff) != 0)
		return (EIO);
	if (urtwa_write_4(sc, REG_MAR, 0xffffffff) != 0 ||
	    urtwa_write_4(sc, REG_MAR + 4, 0xffffffff) != 0)
		return (EIO);

	if (urtwa_write_4_mask(sc, REG_RRSR, 0xfffff, 0xffff1) != 0)
		return (EIO);
	if (urtwa_write_2(sc, REG_RETRY_LIMIT, 0x3030) != 0)
		return (EIO);

	if (urtwa_write_2(sc, REG_SPEC_SIFS, 0x100a) != 0 ||
	    urtwa_write_2(sc, REG_MAC_SPEC_SIFS, 0x100a) != 0 ||
	    urtwa_write_2(sc, REG_SIFS, 0x100a) != 0 ||
	    urtwa_write_2(sc, REG_SIFS + 2, 0x100a) != 0)
		return (EIO);

	if (urtwa_write_4(sc, REG_EDCA_BE_PARAM, 0x005ea42b) != 0 ||
	    urtwa_write_4(sc, REG_EDCA_BK_PARAM, 0x0000a44f) != 0 ||
	    urtwa_write_4(sc, REG_EDCA_VI_PARAM, 0x005ea324) != 0 ||
	    urtwa_write_4(sc, REG_EDCA_VO_PARAM, 0x002fa226) != 0)
		return (EIO);

	if (urtwa_write_1(sc, REG_USTIME_TSF, 0x50) != 0 ||
	    urtwa_write_1(sc, REG_USTIME_EDCA, 0x50) != 0)
		return (EIO);
	if (urtwa_write_1_mask(sc, REG_FWHW_TXQ_CTRL, BIT(7), BIT(7)) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_ACKTO, 0x80) != 0)
		return (EIO);

	if (urtwa_write_4_mask(sc, REG_DWBCN0_CTRL, 0xf0, 6 << 4) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_DWBCN1_CTRL, 6 << 1) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_BCN_MAX_ERR, 0xff) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_HWSEQ_CTRL, 0xff) != 0)
		return (EIO);

	if (urtwa_write_1(sc, REG_USB3_RXITV, 0x01) != 0)
		return (EIO);
	if (urtwa_write_2(sc, REG_RXDMA_STATUS, 0x7400) != 0 ||
	    urtwa_write_1(sc, REG_RXDMA_STATUS + 1, 0xf5) != 0)
		return (EIO);

	printf("%s: runtime init qpages=%u boundary=%u pubq=%u\n",
	    sc->sc_dev.dv_xname, sc->sc_txff_pg_num, boundary, pubq_num);
	return (0);
}

static int
urtwa_h2c_send_register(struct urtwa_softc *sc, uint32_t w0, uint32_t w1)
{
	static const uint16_t box_reg[] = {
		REG_HMEBOX0, REG_HMEBOX1, REG_HMEBOX2, REG_HMEBOX3
	};
	static const uint16_t box_ex_reg[] = {
		REG_HMEBOX0_EX, REG_HMEBOX1_EX, REG_HMEBOX2_EX, REG_HMEBOX3_EX
	};
	uint8_t box, state;
	int n;

	box = sc->sc_h2c_box & 0x3;
	for (n = 0; n < 30; n++) {
		state = urtwa_read_1(sc, REG_HMETFR);
		if (((state >> box) & 0x1) == 0)
			break;
		DELAY(100);
	}
	if (n == 30)
		return (ETIMEDOUT);
	if (urtwa_write_4(sc, box_ex_reg[box], w1) != 0 ||
	    urtwa_write_4(sc, box_reg[box], w0) != 0)
		return (EIO);
	sc->sc_h2c_box = (box + 1) & 0x3;
	return (0);
}

static int
urtwa_h2c_send(struct urtwa_softc *sc, uint32_t w0, uint32_t w1)
{
	return (urtwa_h2c_send_register(sc, w0, w1));
}

static int
urtwa_fw_default_port(struct urtwa_softc *sc)
{
	uint32_t w0;

	w0 = urtwa_encode_bits(URTWA_H2C_DEFAULT_PORT_CMD,
	    URTWA_H2C_CMDID_MASK) |
	    urtwa_encode_bits(0, URTWA_H2C_DEFAULT_PORT_PORTID) |
	    urtwa_encode_bits(0, URTWA_H2C_DEFAULT_PORT_MACID);
	return (urtwa_h2c_send_register(sc, w0, 0));
}

static int
urtwa_fw_media_status(struct urtwa_softc *sc, int connect)
{
	uint32_t w0;

	w0 = urtwa_encode_bits(URTWA_H2C_MEDIA_STATUS_CMD,
	    URTWA_H2C_CMDID_MASK) |
	    (connect ? URTWA_H2C_MEDIA_STATUS_OPMODE : 0) |
	    urtwa_encode_bits(0, URTWA_H2C_MEDIA_STATUS_MACID);
	return (urtwa_h2c_send(sc, w0, 0));
}

static int
urtwa_sta_link_prep(struct urtwa_softc *sc, const uint8_t *bssid)
{
	if (bssid == NULL || IEEE80211_ADDR_EQ(bssid, etherbroadcastaddr))
		return (EINVAL);
	if (urtwa_write_addr(sc, REG_MACID, sc->sc_macaddr) != 0)
		return (EIO);
	if (urtwa_write_addr(sc, REG_BSSID, bssid) != 0)
		return (EIO);
	if (urtwa_write_4_mask(sc, REG_CR, URTWA_PORT0_NET_TYPE_MASK,
	    urtwa_encode_bits(URTWA_NET_MGD_LINKED,
	    URTWA_PORT0_NET_TYPE_MASK)) != 0)
		return (EIO);
	return (0);
}

static void
urtwa_sta_link_drop(struct urtwa_softc *sc)
{
	(void)urtwa_write_4_mask(sc, REG_CR, URTWA_PORT0_NET_TYPE_MASK,
	    urtwa_encode_bits(URTWA_NET_NO_LINK, URTWA_PORT0_NET_TYPE_MASK));
}

static void
urtwa_parse_rx_desc(struct urtwa_rx_desc *rxd, struct urtwa_rx_stat *stat)
{
	uint32_t w0, w2, w3, w4;
	uint32_t enc_type, swdec;

	memset(stat, 0, sizeof(*stat));

	w0 = letoh32(rxd->w0);
	w2 = letoh32(rxd->w2);
	w3 = letoh32(rxd->w3);
	w4 = letoh32(rxd->w4);

	stat->pktlen = w0 & 0x3fff;
	stat->crc_err = (w0 & BIT(14)) != 0;
	stat->icv_err = (w0 & BIT(15)) != 0;
	stat->drvinfosz = ((w0 >> 16) & 0xf) * 8;
	enc_type = (w0 >> 20) & 0x7;
	stat->shift = (w0 >> 24) & 0x3;
	stat->phy_status = (w0 & BIT(26)) != 0;
	swdec = (w0 & BIT(27)) != 0;
	stat->decrypted = !swdec && enc_type != 0;
	stat->is_c2h = (w2 & BIT(28)) != 0;
	stat->rate = w3 & 0x7f;
	stat->bw = (w4 >> 4) & 0x3;
}

static uint8_t
urtwa_rx_power_to_rssi(int8_t rx_power)
{
	if (rx_power <= -100 || rx_power >= 20)
		return (0);
	if (rx_power >= 0)
		return (100);

	return (100 + rx_power);
}

static int8_t
urtwa_cck_rx_pwr(uint8_t lna_idx, uint8_t vga_idx)
{
	static const int8_t lna_gain_table[] = { 15, -1, -17, 0, -30, -38 };
	int8_t rx_pwr = 0;

	switch (lna_idx) {
	case 0:
	case 1:
	case 2:
	case 4:
	case 5:
		rx_pwr = lna_gain_table[lna_idx] - (2 * vga_idx);
		break;
	default:
		break;
	}

	return (rx_pwr);
}

static void
urtwa_get_rx_rssi(struct urtwa_softc *sc, uint8_t *phy_status,
    struct urtwa_rx_stat *stat)
{
	uint32_t w0, w1;
	uint8_t gain, lna_idx, vga_idx;
	int8_t rx_power, rx_power_gain;

	/*
	 * On 8821AU we are seeing valid-looking 32-byte Jaguar PHY blocks on
	 * beacon/probe frames even when the RX descriptor PHYST bit is clear.
	 * Use the drvinfo payload whenever it is present, and reserve the
	 * PHYST bit only as a hint for debugging.
	 */
	if (stat->is_c2h || stat->drvinfosz < 8)
		return;

	w0 = letoh32(((struct urtwa_rx_desc *)phy_status)->w0);
	w1 = letoh32(((struct urtwa_rx_desc *)phy_status)->w1);
	gain = w0 & 0x7f;
	rx_power_gain = gain - 110;

	if (stat->rate <= 3) {
		lna_idx = (w1 >> 13) & 0x7;
		vga_idx = (w1 >> 8) & 0x1f;
		rx_power = urtwa_cck_rx_pwr(lna_idx, vga_idx);

		/*
		 * With PHYST clear, some 8821AU CCK drvinfo blocks still carry
		 * plausible AGC state but occasionally decode to unrealistically
		 * strong values. Prefer the weaker estimate when both the CCK
		 * AGC path and the OFDM-style gain path look sane.
		 */
		if (rx_power > 0 || rx_power <= -100)
			rx_power = rx_power_gain;
		else if (rx_power_gain < 0 && rx_power_gain > -100 &&
		    rx_power_gain < rx_power)
			rx_power = rx_power_gain;
	} else {
		rx_power = rx_power_gain;
	}

	stat->rx_power = rx_power;
	stat->rssi = urtwa_rx_power_to_rssi(rx_power);
	(void)sc;
}

static int
urtwa_sta_rx_mgmt_ok(struct urtwa_softc *sc, const struct ieee80211_frame *wh)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t subtype;

	if (ic->ic_opmode != IEEE80211_M_STA)
		return (1);

	subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	switch (subtype) {
	case IEEE80211_FC0_SUBTYPE_AUTH:
	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
		if (IEEE80211_IS_MULTICAST(wh->i_addr1) ||
		    !IEEE80211_ADDR_EQ(wh->i_addr1, ic->ic_myaddr))
			return (0);
		if (!IEEE80211_ADDR_EQ(wh->i_addr2, ic->ic_bss->ni_bssid) ||
		    !IEEE80211_ADDR_EQ(wh->i_addr3, ic->ic_bss->ni_bssid))
			return (0);
		break;
	}

	return (1);
}

static void
urtwa_rx_frame(struct urtwa_softc *sc, uint8_t *buf, int len,
    struct urtwa_rx_stat *stat, struct mbuf_list *ml)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct ieee80211_rxinfo rxi;
	struct mbuf *m;
	struct llc *llc;
	const char *name;
	uint8_t *frm, *efrm;
	char ssid[IEEE80211_NWID_LEN + 1];
	uint8_t chan;
	uint16_t etype;
	u_int hdrlen;
	int i, ssidlen, s;

	if (len < sizeof(*wh) || len > MCLBYTES) {
		ifp->if_ierrors++;
		return;
	}

	wh = (struct ieee80211_frame *)buf;
	if (ic->ic_opmode == IEEE80211_M_STA &&
	    ic->ic_state != IEEE80211_S_SCAN) {
		if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) ==
		    IEEE80211_FC0_TYPE_DATA &&
		    (wh->i_fc[1] & IEEE80211_FC1_DIR_MASK) ==
		    IEEE80211_FC1_DIR_FROMDS &&
		    !IEEE80211_IS_MULTICAST(wh->i_addr1) &&
		    !IEEE80211_ADDR_EQ(wh->i_addr1, ic->ic_myaddr))
			return;
		if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) ==
		    IEEE80211_FC0_TYPE_MGT &&
		    !urtwa_sta_rx_mgmt_ok(sc, wh))
			return;
	}
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT &&
	    sc->sc_rx_mgmt_log < URTWA_RX_MGMT_LOG_MAX) {
		switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
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
			printf("%s: %s bssid %s chan %u rssi %u ssid \"%s\"\n",
			    sc->sc_dev.dv_xname, name,
			    ether_sprintf((u_char *)wh->i_addr3), chan,
			    stat->rssi, ssid);
			sc->sc_rx_mgmt_log++;
		}
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

	s = splnet();
	ni = ieee80211_find_rxnode(ic, wh);
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA &&
	    sc->sc_eapol_log < URTWA_TX_TRACE_LOG_MAX) {
		hdrlen = ieee80211_get_hdrlen(wh);
		if (len >= hdrlen + LLC_SNAPFRAMELEN) {
			llc = (struct llc *)(buf + hdrlen);
			if (llc->llc_dsap == LLC_SNAP_LSAP &&
			    llc->llc_ssap == LLC_SNAP_LSAP &&
			    llc->llc_control == LLC_UI &&
			    llc->llc_snap.org_code[0] == 0 &&
			    llc->llc_snap.org_code[1] == 0 &&
			    llc->llc_snap.org_code[2] == 0) {
				etype = ntohs(llc->llc_snap.ether_type);
				if (etype == ETHERTYPE_EAPOL) {
					printf("%s: RX EAPOL port_valid=%d protected=%d sa %02x:%02x:%02x:%02x:%02x:%02x da %02x:%02x:%02x:%02x:%02x:%02x\n",
					    sc->sc_dev.dv_xname,
					    ni != NULL ? ni->ni_port_valid : -1,
					    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) != 0,
					    wh->i_addr2[0], wh->i_addr2[1], wh->i_addr2[2],
					    wh->i_addr2[3], wh->i_addr2[4], wh->i_addr2[5],
					    wh->i_addr1[0], wh->i_addr1[1], wh->i_addr1[2],
					    wh->i_addr1[3], wh->i_addr1[4], wh->i_addr1[5]);
					sc->sc_eapol_log++;
				}
			}
		}
	}
	memset(&rxi, 0, sizeof(rxi));
	rxi.rxi_rssi = stat->rssi;
	rxi.rxi_chan = sc->sc_curchan;
	if (stat->decrypted &&
	    (ic->ic_flags & (IEEE80211_F_RSNON | IEEE80211_F_WEPON)) == 0)
		rxi.rxi_flags |= IEEE80211_RXI_HWDEC;
	ieee80211_inputm(ifp, m, ni, &rxi, ml);
	ieee80211_release_node(ic, ni);
	splx(s);
}

static void
urtwa_rxeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct urtwa_rx_data *data = priv;
	struct urtwa_softc *sc = data->sc;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct urtwa_rx_desc *rxd;
	struct urtwa_rx_stat stat;
	uint8_t *buf;
	int error, len, pktoff, pktlen, pkttot;

	if (status != USBD_NORMAL_COMPLETION) {
		if (status == USBD_STALLED)
			usbd_clear_endpoint_stall_async(sc->sc_rx_pipe);
		if (status != USBD_CANCELLED)
			goto resubmit;
		return;
	}

	usbd_get_xfer_status(xfer, NULL, NULL, &len, NULL);
	buf = data->buf;
	sc->sc_rx_done++;
	if (sc->sc_rx_done <= 8) {
		printf("%s: RX completion %d len %d\n", sc->sc_dev.dv_xname,
		    sc->sc_rx_done, len);
	}

	while (len >= URTWA_RX_DESC_SIZE) {
		rxd = (struct urtwa_rx_desc *)buf;
		urtwa_parse_rx_desc(rxd, &stat);
		if (stat.pktlen == 0)
			break;

		urtwa_get_rx_rssi(sc, buf + URTWA_RX_DESC_SIZE + stat.shift,
		    &stat);

		if (sc->sc_rx_desc_log < URTWA_RX_TRACE_LOG_MAX) {
			uint32_t phy_w0 = 0, phy_w1 = 0;
			uint8_t gain = 0, lna_idx = 0, vga_idx = 0;

			if (stat.drvinfosz >= 8) {
				phy_w0 = letoh32(*(uint32_t *)(buf +
				    URTWA_RX_DESC_SIZE + stat.shift));
				phy_w1 = letoh32(*(uint32_t *)(buf +
				    URTWA_RX_DESC_SIZE + stat.shift + 4));
				gain = phy_w0 & 0x7f;
				lna_idx = (phy_w1 >> 13) & 0x7;
				vga_idx = (phy_w1 >> 8) & 0x1f;
			}
			printf("%s: RX desc pktlen %u info %u shift %u physt %u "
			    "c2h %u crc %u icv %u rate %u pwr %d rssi %u "
			    "gain %u lna %u vga %u\n", sc->sc_dev.dv_xname,
			    stat.pktlen, stat.drvinfosz, stat.shift,
			    stat.phy_status, stat.is_c2h, stat.crc_err,
			    stat.icv_err, stat.rate, stat.rx_power,
			    stat.rssi, gain, lna_idx, vga_idx);
			sc->sc_rx_desc_log++;
		}

		pktoff = URTWA_RX_DESC_SIZE + stat.drvinfosz + stat.shift;
		pktlen = stat.pktlen + pktoff;
		pkttot = roundup(pktlen, 8);
		if (pktoff > len || pktlen > len)
			break;
		if (pkttot > len)
			pkttot = len;

		if (stat.is_c2h) {
			if (sc->sc_rx_done <= 8)
				printf("%s: RX C2H len %u\n",
				    sc->sc_dev.dv_xname, stat.pktlen);
		} else if (!stat.crc_err && !stat.icv_err) {
			urtwa_rx_frame(sc, buf + pktoff, stat.pktlen, &stat,
			    &ml);
		}

		buf += pkttot;
		len -= pkttot;
	}
	if_input(&sc->sc_ic.ic_if, &ml);

resubmit:
	usbd_setup_xfer(xfer, sc->sc_rx_pipe, data, data->buf, URTWA_RXBUFSZ,
	    USBD_SHORT_XFER_OK | USBD_NO_COPY, USBD_NO_TIMEOUT, urtwa_rxeof);
	error = usbd_transfer(data->xfer);
	if (error != 0 && error != USBD_IN_PROGRESS) {
		printf("%s: could not resubmit RX transfer: %d\n",
		    sc->sc_dev.dv_xname, error);
	}
}

static int
urtwa_rx_init(struct urtwa_softc *sc)
{
	struct urtwa_rx_data *data;
	int i, error;

	sc->sc_rx_done = 0;
	sc->sc_rx_desc_log = 0;
	sc->sc_rx_mgmt_log = 0;

	for (i = 0; i < URTWA_RX_LIST_COUNT; i++) {
		data = &sc->sc_rx_data[i];
		usbd_setup_xfer(data->xfer, sc->sc_rx_pipe, data, data->buf,
		    URTWA_RXBUFSZ, USBD_SHORT_XFER_OK | USBD_NO_COPY,
		    USBD_NO_TIMEOUT, urtwa_rxeof);
		error = usbd_transfer(data->xfer);
		if (error != 0 && error != USBD_IN_PROGRESS)
			return (EIO);
	}

	return (0);
}

static void
urtwa_txeof(struct usbd_xfer *xfer, void *priv, usbd_status status)
{
	struct urtwa_softc *sc = priv;
	struct ifnet *ifp = &sc->sc_ic.ic_if;
	int s;

	s = splnet();
	sc->sc_tx_active = 0;

	if (__predict_false(status != USBD_NORMAL_COMPLETION)) {
		if (status == USBD_STALLED) {
			if (sc->sc_tx_ep >= 0 && sc->sc_tx_ep < sc->sc_ntx &&
			    sc->sc_tx_pipe[sc->sc_tx_ep] != NULL)
				usbd_clear_endpoint_stall_async(
				    sc->sc_tx_pipe[sc->sc_tx_ep]);
		}
		if (status != USBD_CANCELLED)
			ifp->if_oerrors++;
	}

	ifq_clr_oactive(&ifp->if_snd);
	urtwa_start(ifp);
	splx(s);
	(void)xfer;
}

static int
urtwa_tx(struct urtwa_softc *sc, struct mbuf *m, struct ieee80211_node *ni,
    uint8_t qsel, int is_mgmt)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &ic->ic_if;
	struct ieee80211_key *k;
	struct llc *llc;
	struct ieee80211_frame whbuf;
	struct ieee80211_frame *wh;
	struct urtwa_tx_desc *txd;
	uint16_t len;
	uint16_t etype;
	u_int hdrlen;
	uint32_t txpktlen;
	uint8_t sec_type;
	int use_hwseq;
	uint8_t rateid, rate;
	int ep, error;

	if (m == NULL)
		return (EINVAL);
	if (sc->sc_tx_active)
		return (EBUSY);

	wh = mtod(m, struct ieee80211_frame *);
	k = NULL;
	sec_type = 0;
	use_hwseq = 1;
	txpktlen = m->m_pkthdr.len;

	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) && ni != NULL) {
		k = ieee80211_get_txkey(ic, wh, ni);
		if (k == NULL) {
			m_freem(m);
			ieee80211_release_node(ic, ni);
			return (EINVAL);
		}
		if (k->k_flags & IEEE80211_KEY_SWCRYPTO) {
			if ((m = ieee80211_encrypt(ic, m, k)) == NULL) {
				ieee80211_release_node(ic, ni);
				return (ENOBUFS);
			}
			wh = mtod(m, struct ieee80211_frame *);
			txpktlen = m->m_pkthdr.len;
			use_hwseq = 0;
		}
	}

	len = m->m_pkthdr.len;
	if (len < sizeof(whbuf) || len + URTWA_TX_DESC_SIZE > URTWA_TXBUFSZ) {
		m_freem(m);
		if (ni != NULL)
			ieee80211_release_node(ic, ni);
		return (EMSGSIZE);
	}

	m_copydata(m, 0, sizeof(whbuf), (caddr_t)&whbuf);
	wh = &whbuf;

	if (sc->sc_curchan >= 36) {
		rateid = URTWA_RATEID_G;
		rate = URTWA_DESC_RATE6M;
	} else {
		rateid = URTWA_RATEID_B_20M;
		rate = URTWA_DESC_RATE1M;
	}
	ep = urtwa_qsel_to_ep(sc, qsel);
	txd = (struct urtwa_tx_desc *)sc->sc_tx_buf;
	memset(txd, 0, URTWA_TX_DESC_SIZE);
	txd->w0 = htole32(
	    urtwa_encode_bits(txpktlen, URTWA_TX_DESC_W0_TXPKTSIZE) |
	    urtwa_encode_bits(URTWA_TX_DESC_SIZE, URTWA_TX_DESC_W0_OFFSET) |
	    (IEEE80211_IS_MULTICAST(wh->i_addr1) ? URTWA_TX_DESC_W0_BMC : 0) |
	    URTWA_TX_DESC_W0_LS |
	    (use_hwseq ? URTWA_TX_DESC_W0_DISQSELSEQ : 0));
	txd->w1 = htole32(
	    urtwa_encode_bits(0, URTWA_TX_DESC_W1_MACID) |
	    urtwa_encode_bits(qsel, URTWA_TX_DESC_W1_QSEL) |
	    urtwa_encode_bits(rateid, URTWA_TX_DESC_W1_RATE_ID) |
	    urtwa_encode_bits(sec_type, URTWA_TX_DESC_W1_SEC_TYPE) |
	    urtwa_encode_bits(0, URTWA_TX_DESC_W1_PKT_OFFSET));
	txd->w2 = htole32(is_mgmt ? URTWA_TX_DESC_W2_SPE_RPT : 0);
	txd->w3 = htole32(
	    urtwa_encode_bits(0, URTWA_TX_DESC_W3_HW_SSN_SEL) |
	    URTWA_TX_DESC_W3_USE_RATE |
	    URTWA_TX_DESC_W3_DISDATAFB);
	txd->w4 = htole32(
	    urtwa_encode_bits(rate, URTWA_TX_DESC_W4_DATARATE) |
	    urtwa_encode_bits(0x1f, URTWA_TX_DESC_W4_DATARATE_FB_LIMIT));
	txd->w8 = htole32(use_hwseq ? URTWA_TX_DESC_W8_EN_HWSEQ : 0);

	m_copydata(m, 0, len, sc->sc_tx_buf + URTWA_TX_DESC_SIZE);
	urtwa_txdesc_checksum(txd);

	if (sc->sc_tx_log < URTWA_TX_TRACE_LOG_MAX) {
		uint8_t subtype;

		subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
		if (is_mgmt && (subtype == IEEE80211_FC0_SUBTYPE_PROBE_REQ ||
		    subtype == IEEE80211_FC0_SUBTYPE_AUTH ||
		    subtype == IEEE80211_FC0_SUBTYPE_ASSOC_REQ)) {
			const char *name;

			switch (subtype) {
			case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
				name = "probe request";
				break;
			case IEEE80211_FC0_SUBTYPE_AUTH:
				name = "auth";
				break;
			default:
				name = "assoc request";
				break;
			}
			printf("%s: TX %s ep %d rateid %u rate %#x da %02x:%02x:%02x:%02x:%02x:%02x sa %02x:%02x:%02x:%02x:%02x:%02x\n",
			    sc->sc_dev.dv_xname, name, ep, rateid, rate,
			    wh->i_addr1[0], wh->i_addr1[1], wh->i_addr1[2],
			    wh->i_addr1[3], wh->i_addr1[4], wh->i_addr1[5],
			    wh->i_addr2[0], wh->i_addr2[1], wh->i_addr2[2],
			    wh->i_addr2[3], wh->i_addr2[4], wh->i_addr2[5]);
			sc->sc_tx_log++;
		} else if (!is_mgmt && sc->sc_tx_log < URTWA_TX_TRACE_LOG_MAX) {
			printf("%s: TX data ep %d qsel %u rateid %u rate %#x da %02x:%02x:%02x:%02x:%02x:%02x sa %02x:%02x:%02x:%02x:%02x:%02x\n",
			    sc->sc_dev.dv_xname, ep, qsel, rateid, rate,
			    wh->i_addr1[0], wh->i_addr1[1], wh->i_addr1[2],
			    wh->i_addr1[3], wh->i_addr1[4], wh->i_addr1[5],
			    wh->i_addr2[0], wh->i_addr2[1], wh->i_addr2[2],
			    wh->i_addr2[3], wh->i_addr2[4], wh->i_addr2[5]);
			sc->sc_tx_log++;
		}
	}
	if (!is_mgmt && sc->sc_eapol_log < URTWA_TX_TRACE_LOG_MAX) {
		hdrlen = ieee80211_get_hdrlen(wh);
		if (len >= hdrlen + LLC_SNAPFRAMELEN) {
			llc = (struct llc *)((uint8_t *)wh + hdrlen);
			if (llc->llc_dsap == LLC_SNAP_LSAP &&
			    llc->llc_ssap == LLC_SNAP_LSAP &&
			    llc->llc_control == LLC_UI &&
			    llc->llc_snap.org_code[0] == 0 &&
			    llc->llc_snap.org_code[1] == 0 &&
			    llc->llc_snap.org_code[2] == 0) {
				etype = ntohs(llc->llc_snap.ether_type);
				if (etype == ETHERTYPE_EAPOL) {
					printf("%s: TX EAPOL ep %d qsel %u port_valid=%d protected=%d da %02x:%02x:%02x:%02x:%02x:%02x sa %02x:%02x:%02x:%02x:%02x:%02x\n",
					    sc->sc_dev.dv_xname, ep, qsel,
					    ni != NULL ? ni->ni_port_valid : -1,
					    (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) != 0,
					    wh->i_addr1[0], wh->i_addr1[1], wh->i_addr1[2],
					    wh->i_addr1[3], wh->i_addr1[4], wh->i_addr1[5],
					    wh->i_addr2[0], wh->i_addr2[1], wh->i_addr2[2],
					    wh->i_addr2[3], wh->i_addr2[4], wh->i_addr2[5]);
					sc->sc_eapol_log++;
				}
			}
		}
	}

	m_freem(m);
	if (ni != NULL)
		ieee80211_release_node(ic, ni);

	sc->sc_tx_ep = ep;
	sc->sc_tx_active = 1;
	usbd_setup_xfer(sc->sc_tx_xfer, sc->sc_tx_pipe[ep], sc, sc->sc_tx_buf,
	    URTWA_TX_DESC_SIZE + len, USBD_FORCE_SHORT_XFER | USBD_NO_COPY,
	    URTWA_TX_TIMEOUT, urtwa_txeof);
	error = usbd_transfer(sc->sc_tx_xfer);
	if (error != 0 && error != USBD_IN_PROGRESS) {
		printf("%s: tx submit failed: %s\n", sc->sc_dev.dv_xname,
		    usbd_errstr(error));
		sc->sc_tx_active = 0;
		ifp->if_oerrors++;
		return (EIO);
	}

	return (0);
}

static int
urtwa_tx_mgmt(struct urtwa_softc *sc, struct mbuf *m, struct ieee80211_node *ni)
{
	return (urtwa_tx(sc, m, ni, URTWA_TX_QSEL_MGMT, 1));
}

static uint8_t
urtwa_tx_data_qsel(struct ieee80211_frame *wh)
{
	if (IEEE80211_IS_MULTICAST(wh->i_addr1))
		return (URTWA_TX_QSEL_HIGH);
	return (URTWA_TX_QSEL_TID0);
}

static int
urtwa_init(struct ifnet *ifp)
{
	struct urtwa_softc *sc = ifp->if_softc;
	uint8_t channel;

	channel = urtwa_desired_channel(sc);
	if (sc->sc_running) {
		if (urtwa_set_channel(sc, channel) != 0)
			return (EIO);
		sc->sc_tx_log = 0;
		sc->sc_tx_start_log = 0;
		sc->sc_tx_mgmt_log = 0;
		sc->sc_key_log = 0;
		sc->sc_eapol_log = 0;
		urtwa_kick_state(sc);
		return (0);
	}
	if (!sc->sc_fw_running) {
		if (urtwa_ensure_fw(sc) != 0)
			return (EIO);
	}

	if (urtwa_load_table(sc, &urtwa_mac_tbl) != 0)
		return (EIO);
	if (urtwa_runtime_init(sc) != 0)
		return (EIO);
	if (urtwa_write_1_mask(sc, REG_CR, BIT_MACTXEN | BIT_MACRXEN,
	    BIT_MACTXEN | BIT_MACRXEN) != 0)
		return (EIO);
	if (urtwa_write_1(sc, REG_AMPDU_MAX_TIME, 0x5e) != 0 ||
	    urtwa_write_4(sc, REG_AMPDU_MAX_LENGTH, 0xffffffff) != 0 ||
	    urtwa_write_1_mask(sc, REG_SINGLE_AMPDU_CTRL, BIT_EN_SINGLE_APMDU,
	    BIT_EN_SINGLE_APMDU) != 0 ||
	    urtwa_write_1(sc, REG_RX_PKT_LIMIT, 0x18) != 0 ||
	    urtwa_write_1(sc, REG_PIFS, 0x00) != 0 ||
	    urtwa_write_2(sc, REG_MAX_AGGR_NUM, 0x1f1f) != 0 ||
	    urtwa_write_1(sc, REG_FWHW_TXQ_CTRL, 0x80) != 0 ||
	    urtwa_write_4(sc, REG_FAST_EDCA_CTRL, 0x03087777) != 0 ||
	    urtwa_write_1_mask(sc, REG_RSV_CTRL, BIT(5) | BIT(6),
	    BIT(5) | BIT(6)) != 0)
		return (EIO);
	if (urtwa_phy_init(sc) != 0)
		return (EIO);
	if (urtwa_rx_init(sc) != 0)
		return (EIO);

	sc->sc_running = 1;
	sc->sc_h2c_box = 0;
	sc->sc_link_pending = 0;
	sc->sc_link_connect = 0;
	sc->sc_tx_log = 0;
	sc->sc_tx_start_log = 0;
	sc->sc_tx_mgmt_log = 0;
	sc->sc_key_log = 0;
	sc->sc_eapol_log = 0;
	ifp->if_flags |= IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	urtwa_kick_state(sc);
	return (0);
}

static void
urtwa_stop(struct ifnet *ifp, int disable)
{
	struct urtwa_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;

	sc->sc_running = 0;
	urtwa_cancel_scan(sc);
	sc->sc_link_pending = 0;
	if (sc->sc_udev != NULL)
		usb_rem_wait_task(sc->sc_udev, &sc->sc_task);
	ifp->if_flags &= ~IFF_RUNNING;
	ifq_clr_oactive(&ifp->if_snd);
	if (sc->sc_rx_pipe != NULL)
		usbd_abort_pipe(sc->sc_rx_pipe);
	for (int i = 0; i < sc->sc_ntx; i++) {
		if (sc->sc_tx_pipe[i] != NULL)
			usbd_abort_pipe(sc->sc_tx_pipe[i]);
	}
	sc->sc_tx_active = 0;
	sc->sc_tx_ep = -1;
	sc->sc_h2c_box = 0;
	urtwa_sta_link_drop(sc);
	if (disable)
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
}

static int
urtwa_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct urtwa_softc *sc = ifp->if_softc;
	int s, error = 0;

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
				error = urtwa_init(ifp);
		} else if (ifp->if_flags & IFF_RUNNING)
			urtwa_stop(ifp, 1);
		break;
	default:
		error = ieee80211_ioctl(ifp, cmd, data);
		break;
	}

	if (error == ENETRESET) {
		if ((ifp->if_flags & (IFF_UP | IFF_RUNNING)) ==
		    (IFF_UP | IFF_RUNNING))
			error = urtwa_init(ifp);
		else
			error = 0;
	}

	splx(s);
	usbd_ref_decr(sc->sc_udev);
	return (error);
}

static void
urtwa_start(struct ifnet *ifp)
{
	struct urtwa_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct mbuf *m;
	uint8_t qsel;

	if (sc->sc_tx_start_log < URTWA_TX_START_LOG_MAX) {
		printf("%s: start state=%d ascan=%d running=%d tx_active=%d mgtq=%d ifq_empty=%d\n",
		    sc->sc_dev.dv_xname, ic->ic_state,
		    (ic->ic_flags & IEEE80211_F_ASCAN) != 0, sc->sc_running,
		    sc->sc_tx_active, mq_len(&ic->ic_mgtq), ifq_empty(&ifp->if_snd));
		sc->sc_tx_start_log++;
	}

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
			if (urtwa_tx_mgmt(sc, m, ni) != 0)
				ifp->if_oerrors++;
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
		if ((m = ieee80211_encap(ifp, m, &ni)) == NULL) {
			ifp->if_oerrors++;
			continue;
		}
		wh = mtod(m, struct ieee80211_frame *);
		qsel = urtwa_tx_data_qsel(wh);
		if (urtwa_tx(sc, m, ni, qsel, 0) != 0) {
			ieee80211_release_node(ic, ni);
			ifp->if_oerrors++;
			continue;
		}
		break;
		if (!ifq_empty(&ifp->if_snd))
			ifq_set_oactive(&ifp->if_snd);
		break;
	}
}

static int
urtwa_parse_endpoints(struct urtwa_softc *sc)
{
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	int i;

	sc->sc_rx_no = 0;
	sc->sc_intr_no = 0;
	sc->sc_ntx = 0;

	id = usbd_get_interface_descriptor(sc->sc_iface);
	if (id == NULL) {
		printf("%s: missing interface descriptor\n", sc->sc_dev.dv_xname);
		return (EIO);
	}

	for (i = 0; i < id->bNumEndpoints; i++) {
		ed = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
		if (ed == NULL)
			continue;

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			if (sc->sc_rx_no != 0) {
				printf("%s: multiple bulk-in endpoints\n",
				    sc->sc_dev.dv_xname);
				return (EIO);
			}
			sc->sc_rx_no = ed->bEndpointAddress;
			continue;
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
			if (sc->sc_intr_no != 0) {
				printf("%s: multiple interrupt-in endpoints\n",
				    sc->sc_dev.dv_xname);
				return (EIO);
			}
			sc->sc_intr_no = ed->bEndpointAddress;
			continue;
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			if (sc->sc_ntx >= URTWA_MAX_EPOUT) {
				printf("%s: too many bulk-out endpoints\n",
				    sc->sc_dev.dv_xname);
				return (EIO);
			}
			sc->sc_tx_no[sc->sc_ntx++] = ed->bEndpointAddress;
		}
	}

	if (sc->sc_rx_no == 0) {
		printf("%s: no bulk-in endpoint found\n", sc->sc_dev.dv_xname);
		return (EIO);
	}
	if (sc->sc_ntx < 1 || sc->sc_ntx > URTWA_MAX_EPOUT) {
		printf("%s: invalid number of bulk-out endpoints %u\n",
		    sc->sc_dev.dv_xname, sc->sc_ntx);
		return (EIO);
	}

	return (0);
}

static int
urtwa_open_pipes(struct urtwa_softc *sc)
{
	int i, error;

	error = usbd_open_pipe(sc->sc_iface, sc->sc_rx_no, 0, &sc->sc_rx_pipe);
	if (error != 0) {
		printf("%s: could not open Rx bulk pipe 0x%02x\n",
		    sc->sc_dev.dv_xname, sc->sc_rx_no);
		goto fail;
	}

	if (sc->sc_intr_no != 0) {
		error = usbd_open_pipe(sc->sc_iface, sc->sc_intr_no, 0,
		    &sc->sc_intr_pipe);
		if (error != 0) {
			printf("%s: could not open interrupt pipe 0x%02x\n",
			    sc->sc_dev.dv_xname, sc->sc_intr_no);
			goto fail;
		}
	}

	for (i = 0; i < sc->sc_ntx; i++) {
		error = usbd_open_pipe(sc->sc_iface, sc->sc_tx_no[i], 0,
		    &sc->sc_tx_pipe[i]);
		if (error != 0) {
			printf("%s: could not open Tx bulk pipe 0x%02x\n",
			    sc->sc_dev.dv_xname, sc->sc_tx_no[i]);
			goto fail;
		}
	}

	return (0);

fail:
	urtwa_close_pipes(sc);
	return (error);
}

static void
urtwa_close_pipes(struct urtwa_softc *sc)
{
	int i;

	if (sc->sc_rx_pipe != NULL) {
		usbd_close_pipe(sc->sc_rx_pipe);
		sc->sc_rx_pipe = NULL;
	}
	if (sc->sc_intr_pipe != NULL) {
		usbd_close_pipe(sc->sc_intr_pipe);
		sc->sc_intr_pipe = NULL;
	}
	for (i = 0; i < URTWA_MAX_EPOUT; i++) {
		if (sc->sc_tx_pipe[i] == NULL)
			continue;
		usbd_close_pipe(sc->sc_tx_pipe[i]);
		sc->sc_tx_pipe[i] = NULL;
	}
}

static int
urtwa_alloc_rx_list(struct urtwa_softc *sc)
{
	struct urtwa_rx_data *data;
	int i;

	for (i = 0; i < URTWA_RX_LIST_COUNT; i++) {
		data = &sc->sc_rx_data[i];
		data->sc = sc;
		data->xfer = usbd_alloc_xfer(sc->sc_udev);
		if (data->xfer == NULL) {
			printf("%s: could not allocate RX xfer\n",
			    sc->sc_dev.dv_xname);
			goto fail;
		}
		data->buf = usbd_alloc_buffer(data->xfer, URTWA_RXBUFSZ);
		if (data->buf == NULL) {
			printf("%s: could not allocate RX buffer\n",
			    sc->sc_dev.dv_xname);
			goto fail;
		}
	}
	return (0);

fail:
	urtwa_free_rx_list(sc);
	return (ENOMEM);
}

static int
urtwa_alloc_tx_resources(struct urtwa_softc *sc)
{
	sc->sc_tx_ep = -1;
	sc->sc_tx_xfer = usbd_alloc_xfer(sc->sc_udev);
	if (sc->sc_tx_xfer == NULL) {
		printf("%s: could not allocate TX xfer\n", sc->sc_dev.dv_xname);
		return (ENOMEM);
	}
	sc->sc_tx_buf = usbd_alloc_buffer(sc->sc_tx_xfer, URTWA_TXBUFSZ);
	if (sc->sc_tx_buf == NULL) {
		printf("%s: could not allocate TX buffer\n", sc->sc_dev.dv_xname);
		urtwa_free_tx_resources(sc);
		return (ENOMEM);
	}
	return (0);
}

static void
urtwa_free_tx_resources(struct urtwa_softc *sc)
{
	if (sc->sc_tx_xfer != NULL)
		usbd_free_xfer(sc->sc_tx_xfer);
	sc->sc_tx_xfer = NULL;
	sc->sc_tx_buf = NULL;
	sc->sc_tx_active = 0;
	sc->sc_tx_ep = -1;
}

static void
urtwa_free_rx_list(struct urtwa_softc *sc)
{
	int i;

	for (i = 0; i < URTWA_RX_LIST_COUNT; i++) {
		if (sc->sc_rx_data[i].xfer != NULL)
			usbd_free_xfer(sc->sc_rx_data[i].xfer);
		sc->sc_rx_data[i].xfer = NULL;
		sc->sc_rx_data[i].buf = NULL;
	}
}

int
urtwa_match(struct device *parent, void *match, void *aux)
{
	struct usb_attach_arg *uaa = aux;

	if (uaa->iface == NULL || uaa->configno != 1)
		return (UMATCH_NONE);

	return ((urtwa_lookup(uaa->vendor, uaa->product) != NULL) ?
	    UMATCH_VENDOR_PRODUCT_CONF_IFACE : UMATCH_NONE);
}

void
urtwa_attach(struct device *parent, struct device *self, void *aux)
{
	struct urtwa_softc *sc = (struct urtwa_softc *)self;
	struct usb_attach_arg *uaa = aux;
	int error;

	sc->sc_udev = uaa->device;
	sc->sc_iface = uaa->iface;
	sc->sc_vendor = uaa->vendor;
	sc->sc_product = uaa->product;

	printf(": vendor 0x%04x product 0x%04x\n",
	    sc->sc_vendor, sc->sc_product);
	printf("%s: RTL8821AU/RTL8811AU scaffold attached\n",
	    sc->sc_dev.dv_xname);
	printf("%s: Linux behavior reference is rtw8821a_hw_spec, firmware "
	    "target rtw8821a_fw.bin\n", sc->sc_dev.dv_xname);

	if (urtwa_parse_endpoints(sc) != 0)
		return;
	if (urtwa_open_pipes(sc) != 0)
		return;
	if (urtwa_alloc_tx_resources(sc) != 0) {
		urtwa_close_pipes(sc);
		return;
	}
	if (urtwa_alloc_rx_list(sc) != 0) {
		urtwa_free_tx_resources(sc);
		urtwa_close_pipes(sc);
		return;
	}

	printf("%s: endpoints rx=0x%02x intr=0x%02x tx=%u [",
	    sc->sc_dev.dv_xname, sc->sc_rx_no, sc->sc_intr_no, sc->sc_ntx);
	for (int i = 0; i < sc->sc_ntx; i++)
		printf("%s0x%02x", i == 0 ? "" : " ", sc->sc_tx_no[i]);
	printf("]\n");

	error = urtwa_power_on(sc);
	if (error != 0) {
		printf("%s: phase2 power-on failed\n", sc->sc_dev.dv_xname);
		urtwa_free_rx_list(sc);
		urtwa_free_tx_resources(sc);
		urtwa_close_pipes(sc);
		return;
	}
	error = urtwa_read_efuse(sc);
	if (error != 0) {
		printf("%s: efuse read failed\n", sc->sc_dev.dv_xname);
		urtwa_power_off(sc);
		urtwa_free_rx_list(sc);
		urtwa_free_tx_resources(sc);
		urtwa_close_pipes(sc);
		return;
	}
	sc->sc_efuse_valid = 1;
	error = urtwa_fw_download_legacy(sc);
	if (error == EAGAIN) {
		sc->sc_fw_deferred = 1;
		printf("%s: firmware load deferred until interface bring-up\n",
		    sc->sc_dev.dv_xname);
	} else if (error != 0) {
		printf("%s: phase2 firmware download failed\n",
		    sc->sc_dev.dv_xname);
		urtwa_free_rx_list(sc);
		urtwa_free_tx_resources(sc);
		urtwa_close_pipes(sc);
		return;
	}
	if (urtwa_attach_ifnet(sc) != 0) {
		printf("%s: ifnet attach failed\n", sc->sc_dev.dv_xname);
		if (sc->sc_powered)
			urtwa_power_off(sc);
		urtwa_free_rx_list(sc);
		urtwa_free_tx_resources(sc);
		urtwa_close_pipes(sc);
		return;
	}
}

int
urtwa_detach(struct device *self, int flags)
{
	struct urtwa_softc *sc = (struct urtwa_softc *)self;
	int i, s;

	urtwa_cancel_scan(sc);
	if (sc->sc_udev != NULL)
		usb_rem_wait_task(sc->sc_udev, &sc->sc_task);
	if (sc->sc_rx_pipe != NULL)
		usbd_abort_pipe(sc->sc_rx_pipe);
	for (i = 0; i < sc->sc_ntx; i++) {
		if (sc->sc_tx_pipe[i] != NULL)
			usbd_abort_pipe(sc->sc_tx_pipe[i]);
	}
	s = splusb();
	usbd_ref_wait(sc->sc_udev);
	splx(s);
	if (sc->sc_if_attached) {
		ieee80211_ifdetach(&sc->sc_ic.ic_if);
		if_detach(&sc->sc_ic.ic_if);
		sc->sc_ic.ic_if.if_softc = NULL;
		sc->sc_if_attached = 0;
	}
	urtwa_power_off(sc);
	urtwa_free_rx_list(sc);
	urtwa_free_tx_resources(sc);
	urtwa_close_pipes(sc);
	return (0);
}
