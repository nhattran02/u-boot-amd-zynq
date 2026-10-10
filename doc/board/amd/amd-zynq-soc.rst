.. SPDX-License-Identifier: GPL-2.0+

AMD Zynq-7000 ZC702 (amd-zynq-soc)
==================================

A from-scratch port of U-Boot to the AMD Xilinx Zynq-7000 SoC, targeting the
ZC702 evaluation board. It is independent of the ``ARCH_ZYNQ`` support and of
``board/xilinx``: every SoC specific file was written for this port.

Supported hardware
------------------

====================  ==============================  ===============================
Block                 Driver                          Notes
====================  ==============================  ===============================
Cadence UART          serial_amd_xilinx_zynq.c        console and debug UART
Cortex-A9 global      amd_xilinx_zynq_timer.c         64-bit free running counter
timer
SLCR clocks           clk_amd_xilinx_zynq.c           Linux ``xlnx,ps7-clkc`` IDs
SLCR reset            sysreset_amd_xilinx_zynq.c      ``reset`` command
Arasan SDHCI          amd_xilinx_zynq_sdhci.c         SD 2.0, ADMA2
Cadence GEM           amd_xilinx_zynq_gem.c           RGMII, 10/100/1000
GPIO                  amd_xilinx_zynq_gpio.c          54 MIO + 64 EMIO
====================  ==============================  ===============================

Not supported yet: SPL (DDR, PLL and MIO initialisation), QSPI, USB, I2C,
FPGA programming. Until SPL exists the PS has to be initialised by
something else before U-Boot runs (``ps7_init`` over JTAG, or an FSBL).

Building
--------

GCC 10 or newer is required.

.. code-block:: bash

   export CROSS_COMPILE=arm-none-linux-gnueabihf-
   make amd_zynq_soc_defconfig
   make -j$(nproc)                                       # real ZC702
   make -j$(nproc) DEVICE_TREE=amd-zynq-soc-zc702-qemu   # QEMU

Outputs:

* ``u-boot-dtb.bin``: raw image with the device tree appended, linked at
  ``0x04000000``
* ``u-boot.elf``: the same image as an ELF, for JTAG or ``bootgen``

Running on QEMU
---------------

QEMU's ``xilinx-zynq-a9`` machine models the ZC702 peripherals. The QEMU
device tree only differs in the global timer rate (QEMU always ticks at
100 MHz), the PHY address and the missing GPIO block.

.. code-block:: bash

   qemu-system-arm -M xilinx-zynq-a9 -m 1G -nographic \
       -serial null -serial mon:stdio \
       -device loader,file=u-boot-dtb.bin,addr=0x4000000,cpu-num=0 \
       -drive file=sd.img,if=sd,format=raw \
       -nic user,tftp=/path/to/tftp

``-serial null`` routes the second UART (UART1, the ZC702 console) to the
terminal.

Running on the ZC702
--------------------

Initialise the PS with the ``ps7_init.tcl`` exported by Vivado, then load and
start U-Boot over JTAG:

.. code-block:: none

   xsct% connect
   xsct% targets -set -filter {name =~ "ARM*#0"}
   xsct% source ps7_init.tcl
   xsct% ps7_init
   xsct% ps7_post_config
   xsct% dow u-boot.elf
   xsct% con

The board device tree assumes the default ZC702 clocking: 33.333 MHz PS_CLK,
CPU at 666 MHz and a 50 MHz UART reference clock (``DEBUG_UART_CLOCK``).

Booting Linux
-------------

The default environment provides the standard boot variables:

================  ==============  ===============================
Variable          Value           Use
================  ==============  ===============================
kernel_addr_r     0x02000000      zImage
fdt_addr_r        0x01f00000      device tree
ramdisk_addr_r    0x04000000      initramfs
scriptaddr        0x01e00000      boot.scr
pxefile_addr_r    0x01d00000      PXE configuration
fdtfile           zynq-zc702.dtb
bootm_size        0x20000000      keeps DT and initrd in lowmem
================  ==============  ===============================

``bootcmd`` runs standard boot, which scans the SD card for
``extlinux/extlinux.conf`` or ``boot.scr`` and then tries DHCP/PXE. A
minimal ``extlinux.conf`` on the first FAT partition:

.. code-block:: none

   label linux
       kernel /zImage
       fdt /zynq-zc702.dtb
       initrd /initramfs.cpio.gz
       append console=ttyPS0,115200n8 earlycon

Or by hand:

.. code-block:: none

   => load mmc 0:1 ${kernel_addr_r} zImage
   => load mmc 0:1 ${fdt_addr_r} ${fdtfile}
   => bootz ${kernel_addr_r} - ${fdt_addr_r}

The environment is stored in ``uboot.env`` on the first FAT partition of the
SD card (``saveenv``).
