// SPDX-License-Identifier: GPL-2.0+
/*
 * Clock controller driver for the AMD Xilinx Zynq-7000
 *
 * Register description: UG585 chapter 25 and appendix B.28 (SLCR).
 * Clock IDs follow the Linux "xlnx,ps7-clkc" binding (zynq-7000.txt).
 */

#include <clk-uclass.h>
#include <dm.h>
#include <errno.h>
#include <stdio.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/kernel.h>

/* SLCR register offsets */
#define SLCR_LOCK		0x004
#define SLCR_UNLOCK		0x008
#define ARM_PLL_CTRL		0x100
#define DDR_PLL_CTRL		0x104
#define IO_PLL_CTRL		0x108
#define ARM_CLK_CTRL		0x120
#define DDR_CLK_CTRL		0x124
#define DCI_CLK_CTRL		0x128
#define APER_CLK_CTRL		0x12c
#define GEM0_RCLK_CTRL		0x138
#define GEM1_RCLK_CTRL		0x13c
#define GEM0_CLK_CTRL		0x140
#define GEM1_CLK_CTRL		0x144
#define SMC_CLK_CTRL		0x148
#define LQSPI_CLK_CTRL		0x14c
#define SDIO_CLK_CTRL		0x150
#define UART_CLK_CTRL		0x154
#define SPI_CLK_CTRL		0x158
#define CAN_CLK_CTRL		0x15c
#define PCAP_CLK_CTRL		0x168
#define FPGA0_CLK_CTRL		0x170
#define FPGA1_CLK_CTRL		0x180
#define FPGA2_CLK_CTRL		0x190
#define FPGA3_CLK_CTRL		0x1a0
#define CLK_621_TRUE		0x1c4
#define BOOT_MODE		0x25c

#define SLCR_LOCK_KEY		0x767b
#define SLCR_UNLOCK_KEY		0xdf0d

#define PLL_FDIV		GENMASK(18, 12)
#define PLL_BYPASS_FORCE	BIT(4)
#define PLL_BYPASS_QUAL		BIT(3)
#define BOOT_MODE_PLL_BYPASS	BIT(4)

#define CLK_DIVISOR1		GENMASK(25, 20)
#define CLK_DIVISOR0		GENMASK(13, 8)
#define CLK_SRCSEL		GENMASK(5, 4)
#define CLK_SRCSEL_GEM		GENMASK(6, 4)
#define CLK_DIVISOR_MAX		63

#define DDR_2XCLK_DIVISOR	GENMASK(31, 26)
#define DDR_3XCLK_DIVISOR	GENMASK(25, 20)

#define RCLK_CLKACT		BIT(0)

enum amd_zynq_clk_id {
	armpll, ddrpll, iopll,
	cpu_6or4x, cpu_3or2x, cpu_2x, cpu_1x,
	ddr2x, ddr3x, dci,
	lqspi, smc, pcap, gem0, gem1,
	fclk0, fclk1, fclk2, fclk3,
	can0, can1, sdio0, sdio1, uart0, uart1, spi0, spi1,
	dma,
	usb0_aper, usb1_aper, gem0_aper, gem1_aper, sdio0_aper, sdio1_aper,
	spi0_aper, spi1_aper, can0_aper, can1_aper, i2c0_aper, i2c1_aper,
	uart0_aper, uart1_aper, gpio_aper, lqspi_aper, smc_aper,
	swdt, dbg_trc, dbg_apb,
	clk_max,
};

static const char * const amd_zynq_clk_names[clk_max] = {
	"armpll", "ddrpll", "iopll", "cpu_6or4x", "cpu_3or2x", "cpu_2x",
	"cpu_1x", "ddr2x", "ddr3x", "dci", "lqspi", "smc", "pcap", "gem0",
	"gem1", "fclk0", "fclk1", "fclk2", "fclk3", "can0", "can1", "sdio0",
	"sdio1", "uart0", "uart1", "spi0", "spi1", "dma", "usb0_aper",
	"usb1_aper", "gem0_aper", "gem1_aper", "sdio0_aper", "sdio1_aper",
	"spi0_aper", "spi1_aper", "can0_aper", "can1_aper", "i2c0_aper",
	"i2c1_aper", "uart0_aper", "uart1_aper", "gpio_aper", "lqspi_aper",
	"smc_aper", "swdt", "dbg_trc", "dbg_apb",
};

/* Peripheral clock generators: PLL -> DIVISOR0 [-> DIVISOR1] -> gate */
struct amd_zynq_periph_clk {
	u16 reg;
	bool gem_srcsel;	/* 3-bit SRCSEL with EMIO option */
	bool has_div1;
	s8 clkact;		/* gate bit in reg, -1 if not gated */
	u16 rclk_reg;		/* GEM RX clock control, 0 if none */
};

static const struct amd_zynq_periph_clk amd_zynq_periph_clks[clk_max] = {
	[lqspi]	= { LQSPI_CLK_CTRL, false, false, 0 },
	[smc]	= { SMC_CLK_CTRL, false, false, 0 },
	[pcap]	= { PCAP_CLK_CTRL, false, false, 0 },
	[gem0]	= { GEM0_CLK_CTRL, true, true, 0, GEM0_RCLK_CTRL },
	[gem1]	= { GEM1_CLK_CTRL, true, true, 0, GEM1_RCLK_CTRL },
	[fclk0]	= { FPGA0_CLK_CTRL, false, true, -1 },
	[fclk1]	= { FPGA1_CLK_CTRL, false, true, -1 },
	[fclk2]	= { FPGA2_CLK_CTRL, false, true, -1 },
	[fclk3]	= { FPGA3_CLK_CTRL, false, true, -1 },
	[can0]	= { CAN_CLK_CTRL, false, true, 0 },
	[can1]	= { CAN_CLK_CTRL, false, true, 1 },
	[sdio0]	= { SDIO_CLK_CTRL, false, false, 0 },
	[sdio1]	= { SDIO_CLK_CTRL, false, false, 1 },
	[uart0]	= { UART_CLK_CTRL, false, false, 0 },
	[uart1]	= { UART_CLK_CTRL, false, false, 1 },
	[spi0]	= { SPI_CLK_CTRL, false, false, 0 },
	[spi1]	= { SPI_CLK_CTRL, false, false, 1 },
};

/* AMBA peripheral clock gates in APER_CLK_CTRL */
static const s8 amd_zynq_aper_bits[clk_max] = {
	[dma] = 0, [usb0_aper] = 2, [usb1_aper] = 3, [gem0_aper] = 6,
	[gem1_aper] = 7, [sdio0_aper] = 10, [sdio1_aper] = 11,
	[spi0_aper] = 14, [spi1_aper] = 15, [can0_aper] = 16,
	[can1_aper] = 17, [i2c0_aper] = 18, [i2c1_aper] = 19,
	[uart0_aper] = 20, [uart1_aper] = 21, [gpio_aper] = 22,
	[lqspi_aper] = 23, [smc_aper] = 24,
};

struct amd_zynq_clk_priv {
	void __iomem *slcr;
	ulong ps_clk;
};

static void amd_zynq_slcr_write(struct amd_zynq_clk_priv *priv, u32 reg,
				u32 val)
{
	writel(SLCR_UNLOCK_KEY, priv->slcr + SLCR_UNLOCK);
	writel(val, priv->slcr + reg);
	writel(SLCR_LOCK_KEY, priv->slcr + SLCR_LOCK);
}

static ulong amd_zynq_pll_rate(struct amd_zynq_clk_priv *priv,
			       enum amd_zynq_clk_id id)
{
	static const u16 regs[] = { ARM_PLL_CTRL, DDR_PLL_CTRL, IO_PLL_CTRL };
	u32 ctrl = readl(priv->slcr + regs[id - armpll]);
	bool bypass = ctrl & PLL_BYPASS_FORCE;

	/* Qualified bypass follows the PLL bypass boot strap */
	if (ctrl & PLL_BYPASS_QUAL)
		bypass |= !!(readl(priv->slcr + BOOT_MODE) & BOOT_MODE_PLL_BYPASS);

	if (bypass)
		return priv->ps_clk;

	return priv->ps_clk * FIELD_GET(PLL_FDIV, ctrl);
}

/* SRCSEL: 0x = IO PLL, 10 = ARM PLL, 11 = DDR PLL (GEM: 0xx, 100, 101, 11x EMIO) */
static ulong amd_zynq_src_rate(struct amd_zynq_clk_priv *priv, u32 ctrl,
			       bool gem_srcsel)
{
	u32 sel;

	if (gem_srcsel) {
		sel = FIELD_GET(CLK_SRCSEL_GEM, ctrl);
		if (sel < 4)
			return amd_zynq_pll_rate(priv, iopll);
		if (sel == 4)
			return amd_zynq_pll_rate(priv, armpll);
		if (sel == 5)
			return amd_zynq_pll_rate(priv, ddrpll);
		return -EOPNOTSUPP;
	}

	sel = FIELD_GET(CLK_SRCSEL, ctrl);
	if (sel < 2)
		return amd_zynq_pll_rate(priv, iopll);
	if (sel == 2)
		return amd_zynq_pll_rate(priv, armpll);
	return amd_zynq_pll_rate(priv, ddrpll);
}

static ulong amd_zynq_cpu_rate(struct amd_zynq_clk_priv *priv,
			       enum amd_zynq_clk_id id)
{
	u32 ctrl = readl(priv->slcr + ARM_CLK_CTRL);
	bool ratio_621 = readl(priv->slcr + CLK_621_TRUE) & BIT(0);
	u32 div = FIELD_GET(CLK_DIVISOR0, ctrl);
	ulong rate;

	switch (FIELD_GET(CLK_SRCSEL, ctrl)) {
	case 2:
		rate = amd_zynq_pll_rate(priv, ddrpll);
		break;
	case 3:
		rate = amd_zynq_pll_rate(priv, iopll);
		break;
	default:
		rate = amd_zynq_pll_rate(priv, armpll);
		break;
	}

	if (!div)
		return -EINVAL;
	rate /= div;

	switch (id) {
	case cpu_6or4x:
		return rate;
	case cpu_3or2x:
		return rate / 2;
	case cpu_2x:
		return rate / (ratio_621 ? 3 : 2);
	default:	/* cpu_1x */
		return rate / (ratio_621 ? 6 : 4);
	}
}

static ulong amd_zynq_periph_rate(struct amd_zynq_clk_priv *priv,
				  const struct amd_zynq_periph_clk *pc)
{
	u32 ctrl = readl(priv->slcr + pc->reg);
	u32 div0 = FIELD_GET(CLK_DIVISOR0, ctrl);
	u32 div1 = pc->has_div1 ? FIELD_GET(CLK_DIVISOR1, ctrl) : 1;
	ulong rate = amd_zynq_src_rate(priv, ctrl, pc->gem_srcsel);

	if (IS_ERR_VALUE(rate))
		return rate;
	if (!div0 || !div1)
		return -EINVAL;

	return rate / div0 / div1;
}

static ulong amd_zynq_clk_get_rate(struct clk *clk)
{
	struct amd_zynq_clk_priv *priv = dev_get_priv(clk->dev);
	enum amd_zynq_clk_id id = clk->id;
	u32 ctrl, div0, div1;

	switch (id) {
	case armpll:
	case ddrpll:
	case iopll:
		return amd_zynq_pll_rate(priv, id);
	case cpu_6or4x:
	case cpu_3or2x:
	case cpu_2x:
	case cpu_1x:
		return amd_zynq_cpu_rate(priv, id);
	case ddr2x:
	case ddr3x:
		ctrl = readl(priv->slcr + DDR_CLK_CTRL);
		div0 = id == ddr2x ? FIELD_GET(DDR_2XCLK_DIVISOR, ctrl) :
				     FIELD_GET(DDR_3XCLK_DIVISOR, ctrl);
		if (!div0)
			return -EINVAL;
		return amd_zynq_pll_rate(priv, ddrpll) / div0;
	case dci:
		ctrl = readl(priv->slcr + DCI_CLK_CTRL);
		div0 = FIELD_GET(CLK_DIVISOR0, ctrl);
		div1 = FIELD_GET(CLK_DIVISOR1, ctrl);
		if (!div0 || !div1)
			return -EINVAL;
		return amd_zynq_pll_rate(priv, ddrpll) / div0 / div1;
	case lqspi ... spi1:
		return amd_zynq_periph_rate(priv, &amd_zynq_periph_clks[id]);
	case dma:
		return amd_zynq_cpu_rate(priv, cpu_2x);
	case usb0_aper ... smc_aper:
	case swdt:
	case dbg_apb:
		return amd_zynq_cpu_rate(priv, cpu_1x);
	default:
		return -EOPNOTSUPP;
	}
}

static ulong amd_zynq_clk_set_rate(struct clk *clk, ulong rate)
{
	struct amd_zynq_clk_priv *priv = dev_get_priv(clk->dev);
	const struct amd_zynq_periph_clk *pc;
	ulong parent, best = 0, cur;
	u32 ctrl, div0, div1, max1, best0 = 1, best1 = 1;

	if (clk->id < lqspi || clk->id > spi1 || !rate)
		return -EOPNOTSUPP;

	pc = &amd_zynq_periph_clks[clk->id];
	ctrl = readl(priv->slcr + pc->reg);
	parent = amd_zynq_src_rate(priv, ctrl, pc->gem_srcsel);
	if (IS_ERR_VALUE(parent))
		return parent;

	/* Closest rate that does not exceed the request */
	max1 = pc->has_div1 ? CLK_DIVISOR_MAX : 1;
	for (div0 = 1; div0 <= CLK_DIVISOR_MAX; div0++) {
		for (div1 = 1; div1 <= max1; div1++) {
			cur = parent / div0 / div1;
			if (cur <= rate && cur > best) {
				best = cur;
				best0 = div0;
				best1 = div1;
			}
		}
	}

	if (!best)
		return -EINVAL;

	ctrl &= ~CLK_DIVISOR0;
	ctrl |= FIELD_PREP(CLK_DIVISOR0, best0);
	if (pc->has_div1) {
		ctrl &= ~CLK_DIVISOR1;
		ctrl |= FIELD_PREP(CLK_DIVISOR1, best1);
	}
	amd_zynq_slcr_write(priv, pc->reg, ctrl);

	return best;
}

static int amd_zynq_clk_gate(struct clk *clk, bool enable)
{
	struct amd_zynq_clk_priv *priv = dev_get_priv(clk->dev);
	const struct amd_zynq_periph_clk *pc;
	u32 reg, bit, ctrl;

	if (clk->id >= lqspi && clk->id <= spi1) {
		pc = &amd_zynq_periph_clks[clk->id];
		if (pc->clkact < 0)
			return 0;
		reg = pc->reg;
		bit = BIT(pc->clkact);
	} else if (clk->id == dma ||
		   (clk->id >= usb0_aper && clk->id <= smc_aper)) {
		pc = NULL;
		reg = APER_CLK_CTRL;
		bit = BIT(amd_zynq_aper_bits[clk->id]);
	} else {
		/* PLLs, CPU and DDR clocks are not gated from here */
		return 0;
	}

	ctrl = readl(priv->slcr + reg);
	ctrl = enable ? ctrl | bit : ctrl & ~bit;
	amd_zynq_slcr_write(priv, reg, ctrl);

	/* GEM also needs its RX clock running (sourced from MIO) */
	if (pc && pc->rclk_reg) {
		ctrl = readl(priv->slcr + pc->rclk_reg);
		ctrl = enable ? ctrl | RCLK_CLKACT : ctrl & ~RCLK_CLKACT;
		amd_zynq_slcr_write(priv, pc->rclk_reg, ctrl);
	}

	return 0;
}

static int amd_zynq_clk_enable(struct clk *clk)
{
	return amd_zynq_clk_gate(clk, true);
}

static int amd_zynq_clk_disable(struct clk *clk)
{
	return amd_zynq_clk_gate(clk, false);
}

static int amd_zynq_clk_request(struct clk *clk)
{
	return clk->id < clk_max ? 0 : -EINVAL;
}

static void amd_zynq_clk_dump(struct udevice *dev)
{
	struct clk clk = { .dev = dev };
	ulong rate;

	printf(" %-12s %14s\n", "Clock", "Rate (Hz)");
	printf("-----------------------------\n");
	for (clk.id = 0; clk.id < clk_max; clk.id++) {
		rate = amd_zynq_clk_get_rate(&clk);
		if (IS_ERR_VALUE(rate))
			printf(" %-12s %14s\n", amd_zynq_clk_names[clk.id], "n/a");
		else
			printf(" %-12s %14lu\n", amd_zynq_clk_names[clk.id], rate);
	}
}

static int amd_zynq_clk_probe(struct udevice *dev)
{
	struct amd_zynq_clk_priv *priv = dev_get_priv(dev);

	/* The clkc node sits inside the SLCR, registers are SLCR relative */
	priv->slcr = dev_read_addr_ptr(dev_get_parent(dev));
	if (!priv->slcr)
		return -EINVAL;

	priv->ps_clk = dev_read_u32_default(dev, "ps-clk-frequency", 33333333);

	return 0;
}

static const struct clk_ops amd_zynq_clk_ops = {
	.request = amd_zynq_clk_request,
	.get_rate = amd_zynq_clk_get_rate,
	.set_rate = amd_zynq_clk_set_rate,
	.enable = amd_zynq_clk_enable,
	.disable = amd_zynq_clk_disable,
	.dump = amd_zynq_clk_dump,
};

static const struct udevice_id amd_zynq_clk_ids[] = {
	{ .compatible = "xlnx,ps7-clkc" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_clk) = {
	.name = "amd_zynq_clk",
	.id = UCLASS_CLK,
	.of_match = amd_zynq_clk_ids,
	.probe = amd_zynq_clk_probe,
	.priv_auto = sizeof(struct amd_zynq_clk_priv),
	.ops = &amd_zynq_clk_ops,
};
