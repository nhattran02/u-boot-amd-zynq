// SPDX-License-Identifier: GPL-2.0+
/*
 * AMD Xilinx Zynq-7000 SoC support
 */

#include <cpu_func.h>
#include <fdtdec.h>
#include <init.h>
#include <stdio.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>

/* UG585 appendix B.28, System Level Control Registers */
#define SLCR_BASE			0xf8000000
#define SLCR_PSS_IDCODE			(SLCR_BASE + 0x530)

#define PSS_IDCODE_REVISION		GENMASK(31, 28)
#define PSS_IDCODE_DEVICE		GENMASK(16, 12)
#define PSS_IDCODE_MANUFACTURER		GENMASK(11, 1)

#define XILINX_JEDEC_ID			0x49

static const struct {
	u8 code;
	const char *name;
} amd_zynq_devices[] = {
	{ 0x02, "7z010" },
	{ 0x03, "7z007s" },
	{ 0x07, "7z020" },
	{ 0x08, "7z014s" },
	{ 0x0c, "7z030" },
	{ 0x11, "7z045" },
	{ 0x12, "7z035" },
	{ 0x16, "7z100" },
	{ 0x1b, "7z015" },
	{ 0x1c, "7z012s" },
};

int print_cpuinfo(void)
{
	static const char * const revisions[] = { "1.0", "2.0", "3.0", "3.1" };
	u32 idcode = readl(SLCR_PSS_IDCODE);
	unsigned int device = FIELD_GET(PSS_IDCODE_DEVICE, idcode);
	unsigned int rev = FIELD_GET(PSS_IDCODE_REVISION, idcode);
	const char *name = "unknown";
	int i;

	/* Some emulators (e.g. QEMU) do not implement PSS_IDCODE */
	if (FIELD_GET(PSS_IDCODE_MANUFACTURER, idcode) != XILINX_JEDEC_ID) {
		printf("CPU:   AMD Xilinx Zynq-7000 (IDCODE 0x%08x)\n", idcode);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(amd_zynq_devices); i++) {
		if (amd_zynq_devices[i].code == device) {
			name = amd_zynq_devices[i].name;
			break;
		}
	}

	printf("CPU:   AMD Xilinx Zynq %s, silicon %s (IDCODE 0x%08x)\n",
	       name, rev < ARRAY_SIZE(revisions) ? revisions[rev] : "?",
	       idcode);

	return 0;
}

/* DRAM size and base come from the /memory node of the device tree */
int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

/* DRAM is mapped cacheable, everything else strongly ordered (cache-cp15.c) */
void enable_caches(void)
{
	icache_enable();
	dcache_enable();
}
