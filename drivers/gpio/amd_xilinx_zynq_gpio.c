// SPDX-License-Identifier: GPL-2.0+
/*
 * GPIO controller driver for the AMD Xilinx Zynq-7000
 *
 * Register description: UG585 chapter 14 and appendix B.19
 *
 * 118 GPIOs in four banks: bank 0 = MIO 0..31, bank 1 = MIO 32..53,
 * banks 2 and 3 = EMIO 54..117 (routed to the programmable logic).
 */

#include <dm.h>
#include <errno.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define GPIO_MASK_DATA_LSW(b)	(0x000 + 8 * (b))	/* pins 0..15 */
#define GPIO_MASK_DATA_MSW(b)	(0x004 + 8 * (b))	/* pins 16..31 */
#define GPIO_DATA_RO(b)		(0x060 + 4 * (b))
#define GPIO_DIRM(b)		(0x204 + 0x40 * (b))
#define GPIO_OEN(b)		(0x208 + 0x40 * (b))

#define GPIO_NR			118

static const u8 amd_zynq_gpio_bank_size[] = { 32, 22, 32, 32 };

struct amd_zynq_gpio_priv {
	void __iomem *base;
};

static int amd_zynq_gpio_to_bank(unsigned int offset, unsigned int *bank,
				 unsigned int *pin)
{
	unsigned int b;

	for (b = 0; b < ARRAY_SIZE(amd_zynq_gpio_bank_size); b++) {
		if (offset < amd_zynq_gpio_bank_size[b]) {
			*bank = b;
			*pin = offset;
			return 0;
		}
		offset -= amd_zynq_gpio_bank_size[b];
	}

	return -EINVAL;
}

static int amd_zynq_gpio_get_value(struct udevice *dev, unsigned int offset)
{
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bank, pin;

	if (amd_zynq_gpio_to_bank(offset, &bank, &pin))
		return -EINVAL;

	return !!(readl(priv->base + GPIO_DATA_RO(bank)) & BIT(pin));
}

static int amd_zynq_gpio_set_value(struct udevice *dev, unsigned int offset,
				   int value)
{
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bank, pin, reg;

	if (amd_zynq_gpio_to_bank(offset, &bank, &pin))
		return -EINVAL;

	/* Masked write: upper half selects bits to keep (1) or change (0) */
	reg = pin < 16 ? GPIO_MASK_DATA_LSW(bank) : GPIO_MASK_DATA_MSW(bank);
	pin %= 16;
	writel((~BIT(pin) & 0xffff) << 16 | (value ? BIT(pin) : 0),
	       priv->base + reg);

	return 0;
}

static int amd_zynq_gpio_direction_input(struct udevice *dev,
					 unsigned int offset)
{
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bank, pin;

	if (amd_zynq_gpio_to_bank(offset, &bank, &pin))
		return -EINVAL;

	/* MIO 7 and 8 can only drive outputs (UG585 14.2.1) */
	if (bank == 0 && (pin == 7 || pin == 8))
		return -EINVAL;

	clrbits_le32(priv->base + GPIO_DIRM(bank), BIT(pin));
	clrbits_le32(priv->base + GPIO_OEN(bank), BIT(pin));

	return 0;
}

static int amd_zynq_gpio_direction_output(struct udevice *dev,
					  unsigned int offset, int value)
{
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bank, pin;

	if (amd_zynq_gpio_to_bank(offset, &bank, &pin))
		return -EINVAL;

	amd_zynq_gpio_set_value(dev, offset, value);
	setbits_le32(priv->base + GPIO_DIRM(bank), BIT(pin));
	setbits_le32(priv->base + GPIO_OEN(bank), BIT(pin));

	return 0;
}

static int amd_zynq_gpio_get_function(struct udevice *dev, unsigned int offset)
{
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);
	unsigned int bank, pin;

	if (amd_zynq_gpio_to_bank(offset, &bank, &pin))
		return -EINVAL;

	if (readl(priv->base + GPIO_DIRM(bank)) & BIT(pin))
		return GPIOF_OUTPUT;

	return GPIOF_INPUT;
}

static int amd_zynq_gpio_probe(struct udevice *dev)
{
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct amd_zynq_gpio_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	uc_priv->bank_name = dev_read_string(dev, "gpio-bank-name");
	if (!uc_priv->bank_name)
		uc_priv->bank_name = "gpio";
	uc_priv->gpio_count = GPIO_NR;

	return 0;
}

static const struct dm_gpio_ops amd_zynq_gpio_ops = {
	.direction_input = amd_zynq_gpio_direction_input,
	.direction_output = amd_zynq_gpio_direction_output,
	.get_value = amd_zynq_gpio_get_value,
	.set_value = amd_zynq_gpio_set_value,
	.get_function = amd_zynq_gpio_get_function,
};

static const struct udevice_id amd_zynq_gpio_ids[] = {
	{ .compatible = "xlnx,zynq-gpio-1.0" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_gpio) = {
	.name = "amd_zynq_gpio",
	.id = UCLASS_GPIO,
	.of_match = amd_zynq_gpio_ids,
	.probe = amd_zynq_gpio_probe,
	.priv_auto = sizeof(struct amd_zynq_gpio_priv),
	.ops = &amd_zynq_gpio_ops,
};
