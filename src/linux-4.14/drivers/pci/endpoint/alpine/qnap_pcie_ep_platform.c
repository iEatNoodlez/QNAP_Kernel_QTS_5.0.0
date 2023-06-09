#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static struct platform_device qnap_device_pci_ep = {
	.name		= "qnap-pcie-ep",
	.id		= -1,
};

static int __init qnap_add_device_pci_ep_start(void)
{
	printk("added platform qnap_add_device_pci_ep_start .....\n");
	platform_device_register(&qnap_device_pci_ep);

	return 0;
};

static void qnap_remove_device_pci_ep_exit(void)
{
	platform_device_unregister(&qnap_device_pci_ep);

	return;
};

module_init(qnap_add_device_pci_ep_start);
module_exit(qnap_remove_device_pci_ep_exit);

MODULE_AUTHOR("Tom Yeh");
MODULE_DESCRIPTION("PCIE EP Platform driver");
MODULE_LICENSE("GPL");

