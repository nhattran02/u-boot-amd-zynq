// SPDX-License-Identifier: GPL-2.0+
/*
 * Cortex-A9 MPCore global timer driver for the AMD Xilinx Zynq-7000
 *
 * Register description: ARM DDI 0407, "Global timer", and UG585 chapter 8.3
 */

#include <dm.h>
#include <errno.h>
#include <timer.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define GTIMER_COUNT_LO		0x00
#define GTIMER_COUNT_HI		0x04
#define GTIMER_CTRL		0x08

#define GTIMER_CTRL_EN		BIT(0)

struct amd_zynq_timer_priv {
	void __iomem *base;
};

static u64 amd_zynq_timer_get_count(struct udevice *dev)
{
	struct amd_zynq_timer_priv *priv = dev_get_priv(dev);
	u32 hi, lo;

	/* The two halves are not latched together: retry if HI rolled over */
	do {
		hi = readl(priv->base + GTIMER_COUNT_HI);
		lo = readl(priv->base + GTIMER_COUNT_LO);
	} while (hi != readl(priv->base + GTIMER_COUNT_HI));

	return ((u64)hi << 32) | lo;
}

static int amd_zynq_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct amd_zynq_timer_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	/* clock_rate is filled by the uclass from "clock-frequency" */
	if (!uc_priv->clock_rate)
		return -EINVAL;

	/* Prescaler 0, no compare/IRQ: just a free running 64-bit counter */
	if (!(readl(priv->base + GTIMER_CTRL) & GTIMER_CTRL_EN))
		writel(GTIMER_CTRL_EN, priv->base + GTIMER_CTRL);

	return 0;
}

static const struct timer_ops amd_zynq_timer_ops = {
	.get_count = amd_zynq_timer_get_count,
};

static const struct udevice_id amd_zynq_timer_ids[] = {
	{ .compatible = "arm,cortex-a9-global-timer" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_timer) = {
	.name = "amd_zynq_timer",
	.id = UCLASS_TIMER,
	.of_match = amd_zynq_timer_ids,
	.probe = amd_zynq_timer_probe,
	.priv_auto = sizeof(struct amd_zynq_timer_priv),
	.ops = &amd_zynq_timer_ops,
};
