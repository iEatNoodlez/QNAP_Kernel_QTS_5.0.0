#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/platform_device.h>

#include <plat/devs.h>

typedef void (init_fnc_t) (void);

init_fnc_t __initdata *init_all_device[] = {
	qnap_add_device_ep,
	qnap_add_device_ep_debug,
	qnap_add_device_ep_cable_debug,
	NULL,
};

int __init alpine2_add_all_devices(void)
{
	init_fnc_t **init_fnc_ptr;

	for (init_fnc_ptr = init_all_device; *init_fnc_ptr; ++init_fnc_ptr) {
		(*init_fnc_ptr)();
	}

	return 0;
}
