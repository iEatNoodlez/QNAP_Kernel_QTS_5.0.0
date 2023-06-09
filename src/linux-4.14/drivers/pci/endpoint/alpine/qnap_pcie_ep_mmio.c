#include <linux/io.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/serial.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/bootmem.h>
#include <linux/vmalloc.h>
#include <linux/tty_flip.h>
#include <linux/of_device.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/serial_core.h>
#include <linux/gpio/driver.h>
#include <linux/hwmon-sysfs.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/irqchip/chained_irq.h>

#include <linux/types.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/signal.h>
#include <asm/uaccess.h>

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/mbus.h>
#include <linux/msi.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>

#include <soc/alpine/al_hal_udma.h>
#include <soc/alpine/al_hal_pcie.h>
#include <soc/alpine/al_hal_ssm.h>
#include <soc/alpine/al_hal_m2m_udma.h>
#include <soc/alpine/al_hal_addr_map.h>
#include <soc/alpine/al_hal_pbs_utils.h>
#include <soc/alpine/al_hal_udma_config.h>
#include <soc/alpine/al_hal_plat_services.h>
#include <soc/alpine/al_hal_unit_adapter_regs.h>

#include <soc/alpine/al_hal_udma_fast.h>
#include <soc/alpine/al_hal_ssm_crc_memcpy.h>

#include "qnap_pcie_ep.h"

unsigned int pcie_ep_mmio_read(u32 base_reg, int direction)
{
	register u64 tmp = 0;

	switch(direction) {
	case PCIE_EP_MMIO_HOST:
		tmp = readq(base_reg + HOST_MEM_BASE_0);
		break;
	default:
		tmp = readl(base_reg);
		break;
	}

	return (u32) tmp;
}
EXPORT_SYMBOL(pcie_ep_mmio_read);

void pcie_ep_mmio_write(u32 base_reg, u32 val, int direction)
{
	switch(direction) {
	case PCIE_EP_MMIO_HOST:
		writeq(val, base_reg + HOST_MEM_BASE_0);
		break;
	default:
		writel(val, base_reg);
		break;
	}
}
EXPORT_SYMBOL(pcie_ep_mmio_write);
