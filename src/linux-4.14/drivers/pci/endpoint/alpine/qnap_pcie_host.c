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

#include <soc/alpine/al_hal_pcie.h>
#include <soc/alpine/al_hal_addr_map.h>
#include <soc/alpine/al_hal_pbs_utils.h>
#include <soc/alpine/al_hal_unit_adapter_regs.h>

unsigned char *test = NULL;

static int qnap_pcie_host_probe(struct platform_device *pdev)
{
	uint64_t cc = 0;
	test = kzalloc(16 * 1024, GFP_KERNEL);

        memset(test,0x11,1);
        memset(test+1,0x22,1);
        memset(test+2,0x33,1);
        memset(test+4,0x44,1);
        printk("test[0] = 0x%x\n", test[0]);
        printk("test[1] = 0x%x\n", test[1]);
        printk("test[2] = 0x%x\n", test[2]);
        printk("test[3] = 0x%x\n", test[3]);

	cc = virt_to_phys(test);
	printk("phys = 0x%llx, vir = 0x%llx\n", cc, test);

	return 0;
}

static int qnap_pcie_host_remove(struct platform_device *pdev)
{

	return 0;
}

static struct platform_driver qnap_pcie_host_plat_driver = {
	.driver = {
		.name		= "qnap-pcie-host",
		.owner		= THIS_MODULE
	},
	.probe = qnap_pcie_host_probe,
	.remove = qnap_pcie_host_remove,
};

static int __init qnap_pcie_host_init_module(void)
{
	int rv;

	rv = platform_driver_register(&qnap_pcie_host_plat_driver);
	if(rv) {
		printk(KERN_ERR "Unable to register "
			"driver: %d\n", rv);
		return rv;
	}

	return rv;
}

static void __exit qnap_pcie_host_cleanup_module(void)
{
	platform_driver_unregister(&qnap_pcie_host_plat_driver);

	return;
}

module_init(qnap_pcie_host_init_module);
module_exit(qnap_pcie_host_cleanup_module);

MODULE_AUTHOR("Yong-Yu Yeh <tomyeh@qnap.com>");
MODULE_DESCRIPTION("SOFTUART driver");
MODULE_LICENSE("GPL");
