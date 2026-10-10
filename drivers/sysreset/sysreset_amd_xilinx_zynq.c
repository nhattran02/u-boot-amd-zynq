// SPDX-License-Identifier: GPL-2.0+
/*
 * System reset through the SLCR of the AMD Xilinx Zynq-7000
 *
 * Register description: UG585 chapter 26 and appendix B.28
 */

#include <dm.h>
#include <errno.h>
#include <sysreset.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define SLCR_UNLOCK		0x008
#define SLCR_PSS_RST_CTRL	0x200

#define SLCR_UNLOCK_KEY		0xdf0d
#define PSS_RST_CTRL_SOFT_RST	BIT(0)

struct amd_zynq_sysreset_priv {
	void __iomem *slcr;
};

static int amd_zynq_sysreset_request(struct udevice *dev, enum sysreset_t type)
{
	struct amd_zynq_sysreset_priv *priv = dev_get_priv(dev);

	switch (type) {
	case SYSRESET_WARM:
	case SYSRESET_COLD:
		break;
	default:
		return -EPROTONOSUPPORT;
	}

	/* SLCR registers are write protected until unlocked */
	writel(SLCR_UNLOCK_KEY, priv->slcr + SLCR_UNLOCK);
	writel(PSS_RST_CTRL_SOFT_RST, priv->slcr + SLCR_PSS_RST_CTRL);

	return -EINPROGRESS;
}

static int amd_zynq_sysreset_probe(struct udevice *dev)
{
	struct amd_zynq_sysreset_priv *priv = dev_get_priv(dev);

	priv->slcr = dev_read_addr_ptr(dev);
	if (!priv->slcr)
		return -EINVAL;

	return 0;
}

static const struct sysreset_ops amd_zynq_sysreset_ops = {
	.request = amd_zynq_sysreset_request,
};

static const struct udevice_id amd_zynq_sysreset_ids[] = {
	{ .compatible = "xlnx,zynq-slcr" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_sysreset) = {
	.name = "amd_zynq_sysreset",
	.id = UCLASS_SYSRESET,
	.of_match = amd_zynq_sysreset_ids,
	.probe = amd_zynq_sysreset_probe,
	.priv_auto = sizeof(struct amd_zynq_sysreset_priv),
	.ops = &amd_zynq_sysreset_ops,
};
