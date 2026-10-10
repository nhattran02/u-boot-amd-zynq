// SPDX-License-Identifier: GPL-2.0+
/*
 * Cadence UART driver for the AMD Xilinx Zynq-7000
 *
 * Register description: UG585, chapter 19 and appendix B.33
 */

#include <debug_uart.h>
#include <dm.h>
#include <errno.h>
#include <serial.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/kernel.h>

#define UART_CR			0x00	/* Control */
#define UART_MR			0x04	/* Mode */
#define UART_IDR		0x0c	/* Interrupt Disable */
#define UART_BAUDGEN		0x18	/* Baud Rate Generator */
#define UART_SR			0x2c	/* Channel Status */
#define UART_FIFO		0x30	/* TX/RX FIFO */
#define UART_BAUDDIV		0x34	/* Baud Rate Divider */

#define UART_CR_RXRST		BIT(0)
#define UART_CR_TXRST		BIT(1)
#define UART_CR_RXEN		BIT(2)
#define UART_CR_RXDIS		BIT(3)
#define UART_CR_TXEN		BIT(4)
#define UART_CR_TXDIS		BIT(5)

#define UART_MR_PAR_NONE	(0x4 << 3)	/* 8 data bits, 1 stop bit */

#define UART_SR_RXEMPTY		BIT(1)
#define UART_SR_TXEMPTY		BIT(3)
#define UART_SR_TXFULL		BIT(4)

#define UART_BDIV_MIN		4
#define UART_BDIV_MAX		255
#define UART_CD_MAX		65535

struct amd_zynq_uart_plat {
	void __iomem *base;
	ulong clock;
};

/*
 * baud = clock / (CD * (BDIV + 1)), search the pair with the smallest error
 */
static void amd_zynq_uart_set_baud(void __iomem *base, ulong clock, int baud)
{
	unsigned int best_cd = 0, best_bdiv = 0, best_err = ~0U;
	unsigned int bdiv, cd, err;

	for (bdiv = UART_BDIV_MIN; bdiv <= UART_BDIV_MAX; bdiv++) {
		cd = DIV_ROUND_CLOSEST(clock, baud * (bdiv + 1));
		if (cd < 1 || cd > UART_CD_MAX)
			continue;

		err = abs((int)(clock / (cd * (bdiv + 1))) - baud);
		if (err < best_err) {
			best_err = err;
			best_cd = cd;
			best_bdiv = bdiv;
		}
	}

	writel(best_cd, base + UART_BAUDGEN);
	writel(best_bdiv, base + UART_BAUDDIV);
}

/* UG585 19.3.2: disable, program mode and baud, reset FIFOs, enable */
static void amd_zynq_uart_init(void __iomem *base, ulong clock, int baud)
{
	/* Let the debug UART drain before reprogramming the channel */
	while (!(readl(base + UART_SR) & UART_SR_TXEMPTY))
		;

	writel(UART_CR_TXDIS | UART_CR_RXDIS, base + UART_CR);
	writel(~0U, base + UART_IDR);
	writel(UART_MR_PAR_NONE, base + UART_MR);
	if (clock)
		amd_zynq_uart_set_baud(base, clock, baud);
	writel(UART_CR_TXRST | UART_CR_RXRST, base + UART_CR);
	writel(UART_CR_TXEN | UART_CR_RXEN, base + UART_CR);
}

static int amd_zynq_uart_setbrg(struct udevice *dev, int baudrate)
{
	struct amd_zynq_uart_plat *plat = dev_get_plat(dev);

	amd_zynq_uart_init(plat->base, plat->clock, baudrate);

	return 0;
}

static int amd_zynq_uart_putc(struct udevice *dev, const char ch)
{
	struct amd_zynq_uart_plat *plat = dev_get_plat(dev);

	if (readl(plat->base + UART_SR) & UART_SR_TXFULL)
		return -EAGAIN;

	writel(ch, plat->base + UART_FIFO);

	return 0;
}

static int amd_zynq_uart_getc(struct udevice *dev)
{
	struct amd_zynq_uart_plat *plat = dev_get_plat(dev);

	if (readl(plat->base + UART_SR) & UART_SR_RXEMPTY)
		return -EAGAIN;

	return readl(plat->base + UART_FIFO) & 0xff;
}

static int amd_zynq_uart_pending(struct udevice *dev, bool input)
{
	struct amd_zynq_uart_plat *plat = dev_get_plat(dev);
	u32 sr = readl(plat->base + UART_SR);

	if (input)
		return !(sr & UART_SR_RXEMPTY);

	return !(sr & UART_SR_TXEMPTY);
}

static int amd_zynq_uart_of_to_plat(struct udevice *dev)
{
	struct amd_zynq_uart_plat *plat = dev_get_plat(dev);

	plat->base = dev_read_addr_ptr(dev);
	if (!plat->base)
		return -EINVAL;

	/* No clock driver yet: the reference clock comes straight from DT */
	plat->clock = dev_read_u32_default(dev, "clock-frequency", 0);

	return 0;
}

static const struct dm_serial_ops amd_zynq_uart_ops = {
	.putc = amd_zynq_uart_putc,
	.getc = amd_zynq_uart_getc,
	.pending = amd_zynq_uart_pending,
	.setbrg = amd_zynq_uart_setbrg,
};

static const struct udevice_id amd_zynq_uart_ids[] = {
	{ .compatible = "cdns,uart-r1p8" },
	{ }
};

U_BOOT_DRIVER(amd_zynq_uart) = {
	.name = "amd_zynq_uart",
	.id = UCLASS_SERIAL,
	.of_match = amd_zynq_uart_ids,
	.of_to_plat = amd_zynq_uart_of_to_plat,
	.plat_auto = sizeof(struct amd_zynq_uart_plat),
	.ops = &amd_zynq_uart_ops,
};

#ifdef CONFIG_DEBUG_UART_AMD_XILINX_ZYNQ

static inline void _debug_uart_init(void)
{
	amd_zynq_uart_init((void __iomem *)CONFIG_VAL(DEBUG_UART_BASE),
			   CONFIG_DEBUG_UART_CLOCK, CONFIG_BAUDRATE);
}

static inline void _debug_uart_putc(int ch)
{
	void __iomem *base = (void __iomem *)CONFIG_VAL(DEBUG_UART_BASE);

	while (readl(base + UART_SR) & UART_SR_TXFULL)
		;

	writel(ch, base + UART_FIFO);
}

DEBUG_UART_FUNCS

#endif
