// SPDX-License-Identifier: GPL-2.0+
/*
 * Cadence GEM Ethernet MAC driver for the AMD Xilinx Zynq-7000
 *
 * Register description: UG585 chapter 16 and appendix B.18
 *
 * Polled operation only: one TX buffer descriptor in flight at a time and a
 * ring of single-buffer RX descriptors. Descriptors live in non-cached
 * memory because they are smaller than a cache line and the MAC writes them
 * back; packet buffers are cache line aligned and maintained explicitly.
 */

#include <clk.h>
#include <cpu_func.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <phy.h>
#include <wait_bit.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>

#define GEM_NWCTRL		0x000	/* Network Control */
#define GEM_NWCFG		0x004	/* Network Configuration */
#define GEM_NWSR		0x008	/* Network Status */
#define GEM_DMACR		0x010	/* DMA Configuration */
#define GEM_TXSR		0x014	/* Transmit Status */
#define GEM_RXQBASE		0x018	/* RX descriptor queue base */
#define GEM_TXQBASE		0x01c	/* TX descriptor queue base */
#define GEM_RXSR		0x020	/* Receive Status */
#define GEM_IDR			0x02c	/* Interrupt Disable */
#define GEM_PHYMNTNC		0x034	/* PHY Maintenance */
#define GEM_HASHL		0x080
#define GEM_HASHH		0x084
#define GEM_LADDR1L		0x088	/* Specific address 1, bytes 0..3 */
#define GEM_LADDR1H		0x08c	/* Specific address 1, bytes 4..5 */

#define NWCTRL_RXEN		BIT(2)
#define NWCTRL_TXEN		BIT(3)
#define NWCTRL_MDEN		BIT(4)
#define NWCTRL_CLRSTAT		BIT(5)
#define NWCTRL_STARTTX		BIT(9)

#define NWCFG_SPEED100		BIT(0)
#define NWCFG_FDEN		BIT(1)
#define NWCFG_1536RXEN		BIT(8)
#define NWCFG_GIGEN		BIT(10)
#define NWCFG_FCSREM		BIT(17)
#define NWCFG_MDCCLKDIV		GENMASK(20, 18)

#define NWSR_MDIO_IDLE		BIT(2)

#define DMACR_RXBUF		GENMASK(23, 16)	/* in 64 byte units */
#define DMACR_TXSIZE		BIT(10)		/* use full 4 KiB TX packet RAM */
#define DMACR_RXSIZE		GENMASK(9, 8)	/* 3: full 8 KiB RX packet RAM */
#define DMACR_BLENGTH		GENMASK(4, 0)
#define DMACR_BLENGTH_INCR16	0x10

#define TXSR_COL		BIT(1)
#define TXSR_RTRY		BIT(2)
#define TXSR_AHB_ERR		BIT(4)
#define TXSR_TXCOMPL		BIT(5)
#define TXSR_URUN		BIT(6)
#define TXSR_ERRORS		(TXSR_COL | TXSR_RTRY | TXSR_AHB_ERR | TXSR_URUN)
#define TXSR_CLEAR_ALL		GENMASK(8, 0)
#define RXSR_CLEAR_ALL		GENMASK(3, 0)

#define PHYMNTNC_CLAUSE22	FIELD_PREP(GENMASK(31, 30), 1)
#define PHYMNTNC_OP_WRITE	FIELD_PREP(GENMASK(29, 28), 1)
#define PHYMNTNC_OP_READ	FIELD_PREP(GENMASK(29, 28), 2)
#define PHYMNTNC_PHYADDR	GENMASK(27, 23)
#define PHYMNTNC_REGADDR	GENMASK(22, 18)
#define PHYMNTNC_MUST_10	FIELD_PREP(GENMASK(17, 16), 2)
#define PHYMNTNC_DATA		GENMASK(15, 0)

/* Buffer descriptor bits */
#define RXBD_USED		BIT(0)	/* word 0: frame written by the MAC */
#define RXBD_WRAP		BIT(1)
#define RXBD_ADDR		GENMASK(31, 2)
#define RXBD_LEN		GENMASK(12, 0)
#define RXBD_SOF		BIT(14)
#define RXBD_EOF		BIT(15)
#define TXBD_LEN		GENMASK(13, 0)
#define TXBD_LAST		BIT(15)
#define TXBD_WRAP		BIT(30)
#define TXBD_USED		BIT(31)	/* owned by software */

#define GEM_RX_RING		32
#define GEM_RX_BUF_SIZE		1536
#define GEM_TX_RING		2	/* one frame + a terminating used descriptor */
#define GEM_TX_TIMEOUT_MS	1000
#define GEM_MDIO_TIMEOUT_MS	100

struct gem_bd {
	u32 addr;
	u32 ctrl;
};

struct amd_zynq_gem_priv {
	void __iomem *base;
	struct gem_bd *rx_bd;
	struct gem_bd *tx_bd;
	u8 *rx_buf;
	unsigned int rx_tail;

	struct clk tx_clk;
	ulong pclk_rate;

	struct mii_dev *bus;
	struct phy_device *phydev;
	phy_interface_t interface;
	int phy_addr;
	ofnode phy_node;
};

static int amd_zynq_gem_mdio_wait(struct amd_zynq_gem_priv *priv)
{
	return wait_for_bit_le32(priv->base + GEM_NWSR, NWSR_MDIO_IDLE, true,
				 GEM_MDIO_TIMEOUT_MS, false);
}

static int amd_zynq_gem_mdio_read(struct mii_dev *bus, int addr, int devad,
				  int reg)
{
	struct amd_zynq_gem_priv *priv = bus->priv;
	int ret;

	ret = amd_zynq_gem_mdio_wait(priv);
	if (ret)
		return ret;

	writel(PHYMNTNC_CLAUSE22 | PHYMNTNC_OP_READ | PHYMNTNC_MUST_10 |
	       FIELD_PREP(PHYMNTNC_PHYADDR, addr) |
	       FIELD_PREP(PHYMNTNC_REGADDR, reg), priv->base + GEM_PHYMNTNC);

	ret = amd_zynq_gem_mdio_wait(priv);
	if (ret)
		return ret;

	return FIELD_GET(PHYMNTNC_DATA, readl(priv->base + GEM_PHYMNTNC));
}

static int amd_zynq_gem_mdio_write(struct mii_dev *bus, int addr, int devad,
				   int reg, u16 val)
{
	struct amd_zynq_gem_priv *priv = bus->priv;
	int ret;

	ret = amd_zynq_gem_mdio_wait(priv);
	if (ret)
		return ret;

	writel(PHYMNTNC_CLAUSE22 | PHYMNTNC_OP_WRITE | PHYMNTNC_MUST_10 |
	       FIELD_PREP(PHYMNTNC_PHYADDR, addr) |
	       FIELD_PREP(PHYMNTNC_REGADDR, reg) |
	       FIELD_PREP(PHYMNTNC_DATA, val), priv->base + GEM_PHYMNTNC);

	return amd_zynq_gem_mdio_wait(priv);
}

/* MDC must stay below 2.5 MHz: pick the smallest pclk divider that does it */
static u32 amd_zynq_gem_mdc_div(ulong pclk)
{
	static const u16 dividers[] = { 8, 16, 32, 48, 64, 96, 128, 224 };
	u32 i;

	for (i = 0; i < ARRAY_SIZE(dividers) - 1; i++)
		if (pclk / dividers[i] <= 2500000)
			break;

	return i;
}

static int amd_zynq_gem_write_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	u8 *mac = pdata->enetaddr;

	/* Writing the low half disables the filter until the high half lands */
	writel(mac[0] | mac[1] << 8 | mac[2] << 16 | mac[3] << 24,
	       priv->base + GEM_LADDR1L);
	writel(mac[4] | mac[5] << 8, priv->base + GEM_LADDR1H);

	return 0;
}

static void amd_zynq_gem_init_rings(struct amd_zynq_gem_priv *priv)
{
	u32 addr;
	int i;

	for (i = 0; i < GEM_RX_RING; i++) {
		addr = (ulong)priv->rx_buf + i * GEM_RX_BUF_SIZE;
		if (i == GEM_RX_RING - 1)
			addr |= RXBD_WRAP;
		priv->rx_bd[i].addr = addr;
		priv->rx_bd[i].ctrl = 0;
	}
	priv->rx_tail = 0;

	priv->tx_bd[0].addr = 0;
	priv->tx_bd[0].ctrl = TXBD_USED;
	priv->tx_bd[1].addr = 0;
	priv->tx_bd[1].ctrl = TXBD_USED | TXBD_WRAP;

	/* Buffers will be written by DMA: drop any stale cache lines */
	invalidate_dcache_range((ulong)priv->rx_buf,
				(ulong)priv->rx_buf + GEM_RX_RING * GEM_RX_BUF_SIZE);
}

static int amd_zynq_gem_adjust_link(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct phy_device *phydev = priv->phydev;
	u32 cfg = readl(priv->base + GEM_NWCFG);
	ulong tx_rate, rate;

	cfg &= ~(NWCFG_SPEED100 | NWCFG_GIGEN | NWCFG_FDEN);

	switch (phydev->speed) {
	case SPEED_1000:
		cfg |= NWCFG_GIGEN;
		tx_rate = 125000000;
		break;
	case SPEED_100:
		cfg |= NWCFG_SPEED100;
		tx_rate = 25000000;
		break;
	default:
		tx_rate = 2500000;
		break;
	}

	if (phydev->duplex == DUPLEX_FULL)
		cfg |= NWCFG_FDEN;

	writel(cfg, priv->base + GEM_NWCFG);

	/* RGMII/GMII TX clock is generated by the SoC and follows link speed */
	rate = clk_set_rate(&priv->tx_clk, tx_rate);
	if (IS_ERR_VALUE(rate)) {
		dev_err(dev, "failed to set tx_clk to %lu: %ld\n", tx_rate,
			(long)rate);
		return (int)rate;
	}

	return clk_enable(&priv->tx_clk);
}

static int amd_zynq_gem_start(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	int ret;

	/* Quiesce the MAC, keep the management port alive for the PHY */
	writel(NWCTRL_MDEN, priv->base + GEM_NWCTRL);
	writel(NWCTRL_MDEN | NWCTRL_CLRSTAT, priv->base + GEM_NWCTRL);
	writel(TXSR_CLEAR_ALL, priv->base + GEM_TXSR);
	writel(RXSR_CLEAR_ALL, priv->base + GEM_RXSR);
	writel(~0U, priv->base + GEM_IDR);
	writel(0, priv->base + GEM_HASHL);
	writel(0, priv->base + GEM_HASHH);

	amd_zynq_gem_init_rings(priv);
	writel((ulong)priv->rx_bd, priv->base + GEM_RXQBASE);
	writel((ulong)priv->tx_bd, priv->base + GEM_TXQBASE);

	writel(FIELD_PREP(DMACR_RXBUF, GEM_RX_BUF_SIZE / 64) |
	       FIELD_PREP(DMACR_RXSIZE, 3) | DMACR_TXSIZE |
	       FIELD_PREP(DMACR_BLENGTH, DMACR_BLENGTH_INCR16),
	       priv->base + GEM_DMACR);

	amd_zynq_gem_write_hwaddr(dev);

	ret = phy_startup(priv->phydev);
	if (ret) {
		dev_err(dev, "PHY startup failed: %d\n", ret);
		return ret;
	}

	if (!priv->phydev->link) {
		dev_err(dev, "no link on PHY %d\n", priv->phy_addr);
		return -ENETDOWN;
	}

	ret = amd_zynq_gem_adjust_link(dev);
	if (ret)
		return ret;

	writel(NWCTRL_MDEN | NWCTRL_RXEN | NWCTRL_TXEN,
	       priv->base + GEM_NWCTRL);

	return 0;
}

static int amd_zynq_gem_send(struct udevice *dev, void *packet, int length)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	ulong start = (ulong)packet;
	u32 status;
	int ret;

	flush_dcache_range(rounddown(start, ARCH_DMA_MINALIGN),
			   roundup(start + length, ARCH_DMA_MINALIGN));

	priv->tx_bd[0].addr = start;
	priv->tx_bd[0].ctrl = FIELD_PREP(TXBD_LEN, length) | TXBD_LAST;
	priv->tx_bd[1].ctrl = TXBD_USED | TXBD_WRAP;

	/* TX is idle between frames, so the queue can be rewound every time */
	writel((ulong)priv->tx_bd, priv->base + GEM_TXQBASE);
	setbits_le32(priv->base + GEM_NWCTRL, NWCTRL_STARTTX);

	ret = wait_for_bit_le32(priv->base + GEM_TXSR, TXSR_TXCOMPL, true,
				GEM_TX_TIMEOUT_MS, false);
	status = readl(priv->base + GEM_TXSR);
	writel(TXSR_CLEAR_ALL, priv->base + GEM_TXSR);

	if (ret) {
		dev_err(dev, "TX timeout, TXSR 0x%08x\n", status);
		return ret;
	}
	if (status & TXSR_ERRORS) {
		dev_err(dev, "TX error, TXSR 0x%08x\n", status);
		return -EIO;
	}

	return 0;
}

static int amd_zynq_gem_free_pkt(struct udevice *dev, uchar *packet,
				 int length)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct gem_bd *bd = &priv->rx_bd[priv->rx_tail];

	invalidate_dcache_range((ulong)priv->rx_buf +
				priv->rx_tail * GEM_RX_BUF_SIZE,
				(ulong)priv->rx_buf +
				(priv->rx_tail + 1) * GEM_RX_BUF_SIZE);

	bd->ctrl = 0;
	bd->addr &= ~RXBD_USED;		/* hand the descriptor back to the MAC */
	priv->rx_tail = (priv->rx_tail + 1) % GEM_RX_RING;

	return 0;
}

static int amd_zynq_gem_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct gem_bd *bd = &priv->rx_bd[priv->rx_tail];
	u32 ctrl;

	if (!(bd->addr & RXBD_USED))
		return -EAGAIN;

	writel(RXSR_CLEAR_ALL, priv->base + GEM_RXSR);
	ctrl = bd->ctrl;

	/* Frames never span buffers with a 1536 byte buffer size */
	if ((ctrl & (RXBD_SOF | RXBD_EOF)) != (RXBD_SOF | RXBD_EOF)) {
		amd_zynq_gem_free_pkt(dev, NULL, 0);
		return -EAGAIN;
	}

	*packetp = priv->rx_buf + priv->rx_tail * GEM_RX_BUF_SIZE;

	/* The A9 may have speculatively fetched the buffer before the DMA */
	invalidate_dcache_range((ulong)*packetp,
				(ulong)*packetp + GEM_RX_BUF_SIZE);

	return FIELD_GET(RXBD_LEN, ctrl);
}

static void amd_zynq_gem_stop(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);

	writel(NWCTRL_MDEN, priv->base + GEM_NWCTRL);
	phy_shutdown(priv->phydev);
}

static int amd_zynq_gem_mdio_init(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct mii_dev *bus;

	bus = mdio_alloc();
	if (!bus)
		return -ENOMEM;

	bus->read = amd_zynq_gem_mdio_read;
	bus->write = amd_zynq_gem_mdio_write;
	bus->priv = priv;
	snprintf(bus->name, sizeof(bus->name), "%s", dev->name);
	priv->bus = bus;

	return mdio_register(bus);
}

static int amd_zynq_gem_phy_init(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct phy_device *phydev;

	/* Without a phy-handle, scan the whole MDIO bus (addr -1) */
	phydev = phy_connect(priv->bus, priv->phy_addr, dev, priv->interface);
	if (!phydev) {
		dev_err(dev, "no PHY found\n");
		return -ENODEV;
	}

	if (ofnode_valid(priv->phy_node))
		phydev->node = priv->phy_node;

	phydev->supported &= PHY_GBIT_FEATURES;
	phydev->advertising = phydev->supported;
	priv->phydev = phydev;
	priv->phy_addr = phydev->addr;

	return phy_config(phydev);
}

static int amd_zynq_gem_probe(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct clk pclk, hclk;
	int ret;

	ret = clk_get_by_name(dev, "pclk", &pclk);
	if (!ret)
		ret = clk_enable(&pclk);
	if (!ret)
		ret = clk_get_by_name(dev, "hclk", &hclk);
	if (!ret)
		ret = clk_enable(&hclk);
	if (!ret)
		ret = clk_get_by_name(dev, "tx_clk", &priv->tx_clk);
	if (ret) {
		dev_err(dev, "failed to get clocks: %d\n", ret);
		return ret;
	}

	priv->pclk_rate = clk_get_rate(&pclk);
	if (IS_ERR_VALUE(priv->pclk_rate) || !priv->pclk_rate)
		return -EINVAL;

	priv->rx_bd = (struct gem_bd *)noncached_alloc(GEM_RX_RING *
						       sizeof(struct gem_bd),
						       ARCH_DMA_MINALIGN);
	priv->tx_bd = (struct gem_bd *)noncached_alloc(GEM_TX_RING *
						       sizeof(struct gem_bd),
						       ARCH_DMA_MINALIGN);
	priv->rx_buf = memalign(ARCH_DMA_MINALIGN,
				GEM_RX_RING * GEM_RX_BUF_SIZE);
	if (!priv->rx_bd || !priv->tx_bd || !priv->rx_buf)
		return -ENOMEM;

	/* Management port first: the PHY is probed over MDIO right away */
	writel(NWCFG_FCSREM | NWCFG_1536RXEN |
	       FIELD_PREP(NWCFG_MDCCLKDIV, amd_zynq_gem_mdc_div(priv->pclk_rate)),
	       priv->base + GEM_NWCFG);
	writel(NWCTRL_MDEN, priv->base + GEM_NWCTRL);

	ret = amd_zynq_gem_mdio_init(dev);
	if (ret)
		return ret;

	ret = amd_zynq_gem_phy_init(dev);
	if (ret) {
		mdio_unregister(priv->bus);
		mdio_free(priv->bus);
	}

	return ret;
}

static int amd_zynq_gem_remove(struct udevice *dev)
{
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);

	free(priv->phydev);
	mdio_unregister(priv->bus);
	mdio_free(priv->bus);

	return 0;
}

static int amd_zynq_gem_of_to_plat(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct amd_zynq_gem_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args phandle;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	pdata->iobase = (ulong)priv->base;

	priv->interface = dev_read_phy_mode(dev);
	if (priv->interface == PHY_INTERFACE_MODE_NA)
		priv->interface = PHY_INTERFACE_MODE_RGMII_ID;
	pdata->phy_interface = priv->interface;

	priv->phy_addr = -1;
	priv->phy_node = ofnode_null();
	if (!dev_read_phandle_with_args(dev, "phy-handle", NULL, 0, 0,
					&phandle)) {
		priv->phy_node = phandle.node;
		priv->phy_addr = ofnode_read_u32_default(phandle.node, "reg",
							 -1);
	}

	return 0;
}

static const struct eth_ops amd_zynq_gem_ops = {
	.start = amd_zynq_gem_start,
	.send = amd_zynq_gem_send,
	.recv = amd_zynq_gem_recv,
	.free_pkt = amd_zynq_gem_free_pkt,
	.stop = amd_zynq_gem_stop,
	.write_hwaddr = amd_zynq_gem_write_hwaddr,
};

static const struct udevice_id amd_zynq_gem_ids[] = {
	{ .compatible = "xlnx,zynq-gem" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_gem) = {
	.name = "amd_zynq_gem",
	.id = UCLASS_ETH,
	.of_match = amd_zynq_gem_ids,
	.of_to_plat = amd_zynq_gem_of_to_plat,
	.probe = amd_zynq_gem_probe,
	.remove = amd_zynq_gem_remove,
	.ops = &amd_zynq_gem_ops,
	.priv_auto = sizeof(struct amd_zynq_gem_priv),
	.plat_auto = sizeof(struct eth_pdata),
};
