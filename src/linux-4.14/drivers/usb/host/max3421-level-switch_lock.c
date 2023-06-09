#include <linux/slab.h>

static DEFINE_MUTEX(max3421_oe_lock);

void max3421_oe_level_shift_lock(void)
{
//	printk("max3421_oe_level_shift_lock....\n");
	mutex_lock(&max3421_oe_lock);
}
EXPORT_SYMBOL(max3421_oe_level_shift_lock);

void max3421_oe_level_shift_unlock(void)
{
//	printk("max3421_oe_level_shift_unlock....\n");
	mutex_unlock(&max3421_oe_lock);
}
EXPORT_SYMBOL(max3421_oe_level_shift_unlock);
