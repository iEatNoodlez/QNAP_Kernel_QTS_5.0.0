#ifndef __ASM_PLAT_ALPINE2_H
#define __ASM_PLAT_ALPINE2_H

extern int __init alpine2_add_all_devices(void);

extern void __init qnap_add_device_ep(void);
extern void __init qnap_add_device_ep_debug(void);
extern void __init qnap_add_device_ep_cable_debug(void);

#endif

