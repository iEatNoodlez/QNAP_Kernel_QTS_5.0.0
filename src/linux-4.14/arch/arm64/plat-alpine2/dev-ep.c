#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#if defined(CONFIG_PCI_ALPINE_ENDPOINT)
static struct platform_device qnap_device_ep = {
	.name		= "qnap-pcie-ep",
	.id		= -1,
};

void __init qnap_add_device_ep(void)
{
	platform_device_register(&qnap_device_ep);

	return;
};
#else
void __init qnap_add_device_ep(void)
{
	return;
};
#endif
