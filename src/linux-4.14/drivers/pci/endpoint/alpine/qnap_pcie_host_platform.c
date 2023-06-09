#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static struct platform_device qnap_device_pci_host = {
	.name		= "qnap-pcie-host",
	.id		= -1,
};

static int qnap_add_device_pci_host_start(void)
{
	platform_device_register(&qnap_device_pci_host);

	return 0;
};

static void qnap_remove_device_pci_host_exit(void)
{
	platform_device_unregister(&qnap_device_pci_host);

	return;
};

module_init(qnap_add_device_pci_host_start);
module_exit(qnap_remove_device_pci_host_exit);

MODULE_AUTHOR("Tom Yeh");
MODULE_DESCRIPTION("PCIE HOST Platform driver");
MODULE_LICENSE("GPL");

