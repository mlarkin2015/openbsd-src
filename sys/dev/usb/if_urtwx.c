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
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/malloc.h>

#include <machine/bus.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usbdevs.h>

#include <dev/usb/if_urtwxreg.h>

#define URTWX_CTL_READ			1
#define URTWX_CTL_WRITE			2

#define URTWX_ENDPT_RX			0
#define URTWX_ENDPT_TX			1
#define URTWX_ENDPT_INTR		2
#define URTWX_ENDPT_MAX			3

#define URTWX_MAX_FW_CHUNK		0x1000
#define URTWX_TXDESC_SIZE		48
#define URTWX_TXDESC_CSUM_WORDS		16
#define URTWX_TXDESC_CSUM_IDX		14
#define URTWX_TXDESC_QSEL_BEACON	16
#define URTWX_TX_TIMEOUT		1000

#define URTWX_WAIT_COUNT		1000
#define URTWX_WAIT_DELAY_US		10
#define URTWX_PWR_POLL_COUNT		20000
#define URTWX_PWR_POLL_DELAY_US		50
#define URTWX_EFUSE_POLL_COUNT		100000
#define URTWX_EFUSE_POLL_DELAY_US	1

#define URTWX_PAGE_SIZE			128
#define URTWX_TXFF_SIZE			65536
#define URTWX_RSVD_PG_NUM		52
#define URTWX_RSVD_BOUNDARY		\
	((URTWX_TXFF_SIZE / URTWX_PAGE_SIZE) - URTWX_RSVD_PG_NUM)
#define URTWX_DLFW_HIQ_PGNUM		0x200
#define URTWX_SYS_FUNC_EN_8821C		0xd8
#define URTWX_DMA_MAPPING_HIGH		3

#define URTWX_EFUSE_PHY_SIZE		512
#define URTWX_EFUSE_LOG_SIZE		512
#define URTWX_EFUSE_PROTECT_SIZE	96
#define URTWX_EFUSE_MAC_OFFSET		0x107

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

struct urtwx_softc {
	struct device		 sc_dev;
	struct usbd_device	*sc_udev;
	struct usbd_interface	*sc_iface;

	int			 sc_ed[URTWX_ENDPT_MAX];
	struct usbd_pipe	*sc_ep[URTWX_ENDPT_MAX];

	struct usbd_xfer	*sc_tx_xfer;
	uint8_t			*sc_tx_buf;
};

static const struct usb_devno urtwx_devs[] = {
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_1 },
	{ USB_VENDOR_REALTEK, USB_PRODUCT_REALTEK_RTL8821CU_2 },
};

static const char *urtwx_fw_names[] = {
	"rtw8821c_fw.bin",
};

int	urtwx_match(struct device *, void *, void *);
void	urtwx_attach(struct device *, struct device *, void *);
int	urtwx_detach(struct device *, int);

static int	urtwx_bulk_tx(struct urtwx_softc *, int);
static int	urtwx_check_fw_checksum(struct urtwx_softc *, uint32_t);
static int	urtwx_check_firmware_size(const uint8_t *, size_t);
static int	urtwx_ctl(struct urtwx_softc *, uint8_t, uint8_t,
		    uint16_t, uint16_t, void *, int);
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
static int	urtwx_iddma_download_firmware(struct urtwx_softc *,
		    uint32_t, uint32_t, uint32_t, int);
static int	urtwx_is_powered_on(struct urtwx_softc *);
static int	urtwx_mac_init_system_cfg(struct urtwx_softc *);
static int	urtwx_power_on(struct urtwx_softc *);
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
static int	urtwx_start_download_firmware(struct urtwx_softc *,
		    const uint8_t *, size_t);
static void	urtwx_txdesc_checksum(struct urtwx_tx_desc *);
static int	urtwx_valid_etheraddr(const uint8_t *);
static int	urtwx_wait_1(struct urtwx_softc *, uint16_t, uint8_t,
		    uint8_t, int, int);
static int	urtwx_wait_2(struct urtwx_softc *, uint16_t, uint16_t,
		    uint16_t, int, int);
static int	urtwx_wait_4(struct urtwx_softc *, uint16_t, uint32_t,
		    uint32_t, int, int);
static void	urtwx_wlan_cpu_enable(struct urtwx_softc *, int);
static int	urtwx_write_1(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_1_mask(struct urtwx_softc *, uint16_t,
		    uint8_t, uint8_t);
static int	urtwx_write_2(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_4(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, uint32_t);
static int	urtwx_write_4_mask(struct urtwx_softc *, uint16_t,
		    uint32_t, uint32_t);
static int	urtwx_write_data_rsvd_page(struct urtwx_softc *, uint16_t,
		    const uint8_t *, uint32_t);
static int	urtwx_write_mem(struct urtwx_softc *, uint8_t, uint16_t,
		    uint16_t, void *, int);

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
urtwx_bulk_tx(struct urtwx_softc *sc, int len)
{
	usbd_status err;

	usbd_setup_xfer(sc->sc_tx_xfer, sc->sc_ep[URTWX_ENDPT_TX], sc,
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

	ret = urtwx_bulk_tx(sc, total_len);
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

	memcpy(mac_out, log_map + URTWX_EFUSE_MAC_OFFSET, ETHER_ADDR_LEN);
	if (!urtwx_valid_etheraddr(mac_out))
		ret = ENOENT;

out:
	free(log_map, M_TEMP, URTWX_EFUSE_LOG_SIZE);
	free(phy_map, M_TEMP, URTWX_EFUSE_PHY_SIZE);
	return (ret);
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
	struct usb_attach_arg		*uaa;
	usb_interface_descriptor_t	*id;
	usb_endpoint_descriptor_t	*ed;
	usbd_status			 err;
	uint8_t				 mac[ETHER_ADDR_LEN];
	int				 i;

	sc = (struct urtwx_softc *)self;
	uaa = aux;

	sc->sc_udev = uaa->device;
	sc->sc_iface = uaa->iface;

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
			if (sc->sc_ed[URTWX_ENDPT_TX] == 0)
				sc->sc_ed[URTWX_ENDPT_TX] =
				    ed->bEndpointAddress;
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
			sc->sc_ed[URTWX_ENDPT_INTR] = ed->bEndpointAddress;
		}
	}

	if (sc->sc_ed[URTWX_ENDPT_RX] == 0 ||
	    sc->sc_ed[URTWX_ENDPT_TX] == 0 ||
	    sc->sc_ed[URTWX_ENDPT_INTR] == 0) {
		printf("%s: missing endpoints rx=%#x tx=%#x intr=%#x\n",
		    DEVNAME(sc), sc->sc_ed[URTWX_ENDPT_RX],
		    sc->sc_ed[URTWX_ENDPT_TX], sc->sc_ed[URTWX_ENDPT_INTR]);
		return;
	}

	err = usbd_open_pipe(sc->sc_iface, sc->sc_ed[URTWX_ENDPT_TX],
	    USBD_EXCLUSIVE_USE, &sc->sc_ep[URTWX_ENDPT_TX]);
	if (err != USBD_NORMAL_COMPLETION) {
		printf("%s: could not open tx pipe: %s\n", DEVNAME(sc),
		    usbd_errstr(err));
		return;
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

	printf("%s: MAC %s\n", DEVNAME(sc), ether_sprintf(mac));
	return;

fail:
	if (sc->sc_tx_xfer != NULL) {
		usbd_free_xfer(sc->sc_tx_xfer);
		sc->sc_tx_xfer = NULL;
		sc->sc_tx_buf = NULL;
	}
	if (sc->sc_ep[URTWX_ENDPT_TX] != NULL) {
		usbd_close_pipe(sc->sc_ep[URTWX_ENDPT_TX]);
		sc->sc_ep[URTWX_ENDPT_TX] = NULL;
	}
}

int
urtwx_detach(struct device *self, int flags)
{
	struct urtwx_softc *sc;
	int s;

	sc = (struct urtwx_softc *)self;

	if (sc->sc_ep[URTWX_ENDPT_TX] != NULL)
		usbd_abort_pipe(sc->sc_ep[URTWX_ENDPT_TX]);

	s = splusb();
	usbd_ref_wait(sc->sc_udev);
	splx(s);

	if (sc->sc_tx_xfer != NULL) {
		usbd_free_xfer(sc->sc_tx_xfer);
		sc->sc_tx_xfer = NULL;
		sc->sc_tx_buf = NULL;
	}
	if (sc->sc_ep[URTWX_ENDPT_TX] != NULL) {
		usbd_close_pipe(sc->sc_ep[URTWX_ENDPT_TX]);
		sc->sc_ep[URTWX_ENDPT_TX] = NULL;
	}

	return (0);
}
