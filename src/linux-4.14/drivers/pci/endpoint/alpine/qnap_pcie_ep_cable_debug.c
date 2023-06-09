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
#include "qnap_pcie_ep_cpld.h"

static struct proc_dir_entry *proc_qnap_pcie_ep_cable_debug_root = NULL;
static uint g_mode = 0;

static void setting_mode(uint cmd)
{
	switch(cmd) {
	case 1:
		printk("hello word !!!\n");
		break;
		break;
	default:
		printk("QNAP PCIE Cable Debug EP Control function error\n");
		break;
	}

	return;
};

static ssize_t qnap_pcie_ep_cable_debug_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;

	return len;
};

static ssize_t qnap_pcie_ep_cable_debug_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	unsigned char value[100];
	unsigned int tmp;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	sscanf(value,"%u\n", &tmp);
	//printk("tmp=%d\n", tmp);
	setting_mode(tmp);
	g_mode = tmp;

	return count;
};

static const struct file_operations qnap_pcie_ep_cable_debug_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= qnap_pcie_ep_cable_debug_read_proc,
	.write			= qnap_pcie_ep_cable_debug_write_proc
};

static int qnap_ep_callback(struct qnap_ep_event *event)
{
	printk("qnap_ep_callback event\n");
	printk("event->port_num = 0x%x\n", event->port_num);
	printk("event->type = 0x%x\n", event->type);
	if(event->type == CPLD_EVENT_CABLE_USB_INSERT) {
		printk("Type-C USB Cable Insert\n");
	} else if(event->type == CPLD_EVENT_CABLE_USB_REMOVED) {
		printk("Type-C USB Cable Removed\n");
	} else if(event->type == CPLD_EVENT_CABLE_TBT_INSERT) {
		printk("Type-C TBT Cable Insert\n");
	} else if(event->type == CPLD_EVENT_CABLE_TBT_REMOVED) {
		printk("Type-C TBT Cable Removed\n");
	} else {
		printk("Unkonw type !!\n");
	}
	return 0;
};

static struct qnap_ep_fabrics_ops callback_ep_ops = {
	.owner			= THIS_MODULE,
	.type			= QNAP_EP_CALLBACK,
	.callback_response	= qnap_ep_callback,
};

static int qnap_pcie_ep_cable_debug_probe(struct platform_device *pdev)
{
	struct proc_dir_entry *mode;
	int ret;

	printk("init qnap_pcie_ep_cable_debug_probe\n");

	// procfs
	proc_qnap_pcie_ep_cable_debug_root = proc_mkdir("qnapepcabledebug", NULL);
	if(!proc_qnap_pcie_ep_cable_debug_root) {
		printk(KERN_ERR "Couldn't create qnapep folder in procfs\n");
		return -1;
	}

	// create file of folder in procfs
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_cable_debug_root, &qnap_pcie_ep_cable_debug_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnapep debug procfs node\n");
		return -1;
	}

	ret = qnap_ep_register(&callback_ep_ops);
	if (ret)
		return ret;

	return 0;
}

static int qnap_pcie_ep_cable_debug_remove(struct platform_device *pdev)
{
	qnap_ep_unregister(&callback_ep_ops);
	remove_proc_entry("mode", proc_qnap_pcie_ep_cable_debug_root);
	remove_proc_entry("qnapepcabledebug", NULL);

	return 0;
}

static struct platform_driver qnap_pcie_ep_cable_debug_plat_driver = {
	.driver = {
		.name		= "qnap-pcie-ep-cable-debug",
		.owner		= THIS_MODULE
	},
	.probe = qnap_pcie_ep_cable_debug_probe,
	.remove = qnap_pcie_ep_cable_debug_remove,
};

static int __init qnap_pcie_ep_cable_debug_init_module(void)
{
	int rv;

	rv = platform_driver_register(&qnap_pcie_ep_cable_debug_plat_driver);
	if(rv) {
		printk(KERN_ERR "Unable to register "
			"driver: %d\n", rv);
		return rv;
	}

	return rv;
}

static void __exit qnap_pcie_ep_cable_debug_cleanup_module(void)
{
	platform_driver_unregister(&qnap_pcie_ep_cable_debug_plat_driver);

	return;
}

module_init(qnap_pcie_ep_cable_debug_init_module);
module_exit(qnap_pcie_ep_cable_debug_cleanup_module);

MODULE_AUTHOR("Yong-Yu Yeh <tomyeh@qnap.com>");
MODULE_DESCRIPTION("EP cable debug");
MODULE_LICENSE("GPL");
