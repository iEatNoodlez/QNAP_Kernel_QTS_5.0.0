#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#if defined(CONFIG_PCI_ALPINE_ENDPOINT_DEBUG)
static struct platform_device qnap_device_ep_debug = {
	.name		= "qnap-pcie-ep-debug",
	.id		= -1,
};

void __init qnap_add_device_ep_debug(void)
{
	platform_device_register(&qnap_device_ep_debug);

	return;
};
#else
void __init qnap_add_device_ep_debug(void)
{
	return;
};
#endif
