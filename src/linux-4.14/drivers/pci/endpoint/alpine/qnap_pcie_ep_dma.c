#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>

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

#define SSM_XOR_UDMA	1
//#define CRYPTO_ENGINE_UDMA 1

int udma_fast_memcpy(int len, al_phys_addr_t src, al_phys_addr_t dst);
int ep_udma_fast_memcpy(int len, al_phys_addr_t src, al_phys_addr_t dst);

int qnap_udma_memcpy(int len, dma_addr_t source, dma_addr_t destination, int direction)
{
	int ret = 0;

	switch(direction) {
	case PCIE_EP_DMA_H2D:
#if defined(SSM_XOR_UDMA)
		ret = udma_fast_memcpy(len, source + HOST_MEM_BASE_0, destination);
#elif defined(CRYPTO_ENGINE_UDMA)
		ret = ep_udma_fast_memcpy(len, source + HOST_MEM_BASE_0, destination);
#endif
		if(ret < 0) {
			printk("AL UDMA function fail !\n");
			return -EIO;
		}
	case PCIE_EP_DMA_D2H:
#if defined(SSM_XOR_UDMA)
		ret = udma_fast_memcpy(len, source, destination + HOST_MEM_BASE_0);
#elif defined(CRYPTO_ENGINE_UDMA)
		ret = ep_udma_fast_memcpy(len, source, destination + HOST_MEM_BASE_0);
#endif
		if(ret < 0) {
			printk("AL UDMA function fail !\n");
			return -EIO;
		}
	}

	return 0;
}
EXPORT_SYMBOL(qnap_udma_memcpy);
