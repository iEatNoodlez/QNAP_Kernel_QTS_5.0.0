#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <plat/devs.h>

static int __init alpine2_init(void)
{
	alpine2_add_all_devices();
	return 0;
};
arch_initcall(alpine2_init)
