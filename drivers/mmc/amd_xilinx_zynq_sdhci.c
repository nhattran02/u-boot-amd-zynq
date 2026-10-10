// SPDX-License-Identifier: GPL-2.0+
/*
 * Arasan SD/SDIO controller glue for the AMD Xilinx Zynq-7000
 *
 * The controller is a standard SD Host Controller 2.0 (UG585 chapter 13),
 * so all the work is done by the generic sdhci core.
 */

#include <clk.h>
#include <dm.h>
#include <mmc.h>
#include <sdhci.h>
#include <dm/device_compat.h>
#include <linux/err.h>

/* SD 2.0 high speed is the fastest mode the Zynq-7000 IOs support */
#define AMD_ZYNQ_SDHCI_MAX_FREQ		52000000

struct amd_zynq_sdhci_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

static int amd_zynq_sdhci_probe(struct udevice *dev)
{
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct amd_zynq_sdhci_plat *plat = dev_get_plat(dev);
	struct sdhci_host *host = dev_get_priv(dev);
	struct clk clk_xin, clk_ahb;
	ulong rate;
	int ret;

	ret = clk_get_by_name(dev, "clk_ahb", &clk_ahb);
	if (!ret)
		ret = clk_enable(&clk_ahb);
	if (ret) {
		dev_err(dev, "failed to enable clk_ahb: %d\n", ret);
		return ret;
	}

	ret = clk_get_by_name(dev, "clk_xin", &clk_xin);
	if (!ret)
		ret = clk_enable(&clk_xin);
	if (ret) {
		dev_err(dev, "failed to enable clk_xin: %d\n", ret);
		return ret;
	}

	rate = clk_get_rate(&clk_xin);
	if (IS_ERR_VALUE(rate) || !rate) {
		dev_err(dev, "invalid clk_xin rate\n");
		return -EINVAL;
	}

	host->name = dev->name;
	host->ioaddr = dev_read_addr_ptr(dev);
	if (!host->ioaddr)
		return -EINVAL;

	host->max_clk = rate;
	if (dev_read_bool(dev, "no-1-8-v"))
		host->quirks |= SDHCI_QUIRK_NO_1_8_V;

	host->mmc = &plat->mmc;
	host->mmc->dev = dev;
	host->mmc->priv = host;
	upriv->mmc = host->mmc;

	ret = sdhci_setup_cfg(&plat->cfg, host, AMD_ZYNQ_SDHCI_MAX_FREQ, 0);
	if (ret)
		return ret;

	/* bus-width, max-frequency, non-removable, cap-sd-highspeed... */
	ret = mmc_of_parse(dev, &plat->cfg);
	if (ret)
		return ret;

	return sdhci_probe(dev);
}

static int amd_zynq_sdhci_bind(struct udevice *dev)
{
	struct amd_zynq_sdhci_plat *plat = dev_get_plat(dev);

	return sdhci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id amd_zynq_sdhci_ids[] = {
	{ .compatible = "arasan,sdhci-8.9a" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_sdhci) = {
	.name = "amd_zynq_sdhci",
	.id = UCLASS_MMC,
	.of_match = amd_zynq_sdhci_ids,
	.ops = &sdhci_ops,
	.bind = amd_zynq_sdhci_bind,
	.probe = amd_zynq_sdhci_probe,
	.priv_auto = sizeof(struct sdhci_host),
	.plat_auto = sizeof(struct amd_zynq_sdhci_plat),
};
