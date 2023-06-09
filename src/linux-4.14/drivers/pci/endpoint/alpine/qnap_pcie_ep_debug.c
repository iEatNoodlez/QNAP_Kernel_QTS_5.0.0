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

unsigned char *dma1 = NULL;
unsigned char *dma2 = NULL;

int udma_fast_memcpy(int len, al_phys_addr_t src, al_phys_addr_t dst);
//int ep_udma_fast_memcpy(int len, al_phys_addr_t src, al_phys_addr_t dst);

static struct proc_dir_entry *proc_qnap_pcie_ep_debug_root = NULL;
static uint g_mode = 0;

static unsigned long dma_ch0_count = 0;
static unsigned long dma_ch1_count = 0;
static unsigned long dma_ch2_count = 0;
static unsigned long dma_ch3_count = 0;
static unsigned long other_count = 0;

static char *mode_name[] =
{
	"NULL",
};

void debug_dma_add_number(unsigned char a)
{
	if(a == '0')
		dma_ch0_count++;
	else if(a == '1')
		dma_ch1_count++;
	else if(a == '2')
		dma_ch2_count++;
	else if(a == '3')
		dma_ch3_count++;
	else
		other_count++;

	return;
}
EXPORT_SYMBOL(debug_dma_add_number);

static void setting_mode(uint cmd)
{
	switch(cmd) {
	case 1:
		printk("hello word !!!\n");
		break;
	case 2:
		printk("rest ch0 dma num to zero\n");
		printk("rest ch1 dma num to zero\n");
		printk("rest ch2 dma num to zero\n");
		printk("rest ch3 dma num to zero\n");
		dma_ch0_count = 0;
		dma_ch1_count = 0;
		dma_ch2_count = 0;
		dma_ch3_count = 0;
		other_count = 0;
		break;
	case 3:
		printk("ch0 dma num = %ld\n", dma_ch0_count);
		printk("ch1 dma num = %ld\n", dma_ch1_count);
		printk("ch2 dma num = %ld\n", dma_ch2_count);
		printk("ch3 dma num = %ld\n", dma_ch3_count);
		printk("others num = %ld\n", other_count);
		break;
	case 4:
		debug_dma_add_number('0');
		debug_dma_add_number('1');
		debug_dma_add_number('2');
		debug_dma_add_number('3');
		break;
	default:
		printk("QNAP PCIE Debug EP Control function error\n");
		break;
	}

	return;
};

static ssize_t qnap_pcie_ep_debug_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;

	printk("mode__name[%d] = %s\n", g_mode, mode_name[g_mode]);

	return len;
};

static ssize_t qnap_pcie_ep_debug_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
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

static const struct file_operations qnap_pcie_ep_debug_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= qnap_pcie_ep_debug_read_proc,
	.write			= qnap_pcie_ep_debug_write_proc
};

static ssize_t dump_read_reg(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	return 0;
};

static ssize_t dump_write_reg(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len=count;
	unsigned char value[60], cmd, cmd2;
	unsigned int rval, wval;
	unsigned long base, rvaq, wvaq, base1, base2, tmp_base;
	static void __iomem *vbase1;
	static unsigned long *vbase2;
	unsigned int *ptr_tmp_value = NULL, tmp_value = 0;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	sscanf(value, "%c %lx %x\n", &cmd, &base, &wval);
	if(cmd == 'd' || cmd == 'D' || cmd == 'E' || cmd == 'e') {
		sscanf(value, "%c %c %lx %lx %x\n", &cmd, &cmd2, &base1, &base2, &wval);
		if(cmd2 != 'C' || cmd2 != 'c')
			tmp_base = base1 | base2;
		printk("=============\n");
		printk("cmd = %c\n", cmd);
		printk("cmd2 = %c\n", cmd2);
		printk("base1 = 0x%lx\n", base1);
		printk("base2 = 0x%lx\n", base2);
		if(cmd2 != 'C' || cmd2 != 'c')
			printk("base1 | base2 (tmp_base) = 0x%lx\n", tmp_base);
		if(cmd2 == 'W' || cmd2 == 'w')
			printk("wval = 0x%x\n", wval);
		printk("=============\n\n");
	} else if(cmd != 'd' || cmd != 'D' || cmd != 'E' || cmd != 'e') {
		printk("=============\n");
		printk("cmd = %c\n", cmd);
		printk("base = 0x%lx\n", base);
		if(cmd == 'W' || cmd == 'w' || cmd == 'O' || cmd == 'o')
			printk("wval = 0x%x\n", wval);
		printk("=============\n\n");
	}

	if(cmd == 'R' || cmd == 'r') {
		if(base > 0xffffffff) {
			printk("long address ...\n");
			vbase1 = ioremap(base, 16);
			if(vbase1 == NULL)
				printk("mmio map address 0x%lx fail\n", base);

			rvaq = readq(vbase1);
			printk("MMIO Read : Base = 0x%08lx, VBase = 0x%08lx, Val = 0x%08x\n", base, vbase1, rvaq);
			iounmap(vbase1);
		} else {
			vbase2 = phys_to_virt(base);
			if(vbase2 == NULL)
				printk("map address 0x%lx fail\n", base);

			rval = readl(vbase2);
			printk("MMIO Read : Base = 0x%08x, VBase = 0x%08x, Val = 0x%08x\n", base, vbase2, rval);
		}
	} else if(cmd == 'W' || cmd == 'w') {
		if(base > 0xffffffff) {
			printk("long address\n");
			vbase1 = ioremap(base, 16);
			if(vbase1 == NULL)
				printk("mmio map address 0x%lx fail\n", base);

			wvaq = wval;
			printk("MMIO Write : Base = 0x%08lx, VBase = 0x%08x, Val = 0x%08x\n", base, vbase1, wvaq);
			writeq(wvaq, vbase1);
			rvaq = readq(vbase1);
			printk("MMIO Read : Base = 0x%08lx, VBase = 0x%08x, Val = 0x%08x\n", base, vbase1, rvaq);
			iounmap(vbase1);
		} else {
			vbase2 = phys_to_virt(base);
			if(vbase2 == NULL)
				printk("mmio map address 0x%lx fail\n", base);

			printk("MMIO Write : Base = 0x%08lx, VBase = 0x%08x, Val = 0x%08x\n", base, vbase2, wvaq);
			writel(wval, vbase2);
			rval = readl(vbase2);
			printk("MMIO Read : Base = 0x%08lx, VBase = 0x%08x, Val = 0x%08x\n", base, vbase2, rval);
		}
#if defined(CONFIG_AL_DMA)
	} else if(cmd == 'D' || cmd == 'd') {
		if(cmd2 == 'R' || cmd2 == 'r') {
			memset(dma1, 0xff, 64 * 1024);
			udma_fast_memcpy(4, tmp_base, virt_to_phys(dma1));
			printk("DMA Read : Base = 0x%0lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) dma1);
		} else if(cmd2 == 'W' || cmd2 == 'w') {
			memset(dma1, 0xff, 64 * 1024);
			tmp_value = (unsigned int) wval;
			ptr_tmp_value = (unsigned int *)dma1;
			*ptr_tmp_value = tmp_value;
			printk("DMA Write : Base = 0x%lx, Val = 0x%08x\n", tmp_base, wval);
			udma_fast_memcpy(8, virt_to_phys(dma1), tmp_base);
			memset(dma2, 0xff, 64 * 1024);
			udma_fast_memcpy(4, tmp_base, virt_to_phys(dma2));
			printk("DMA Read : Base = 0x%lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) dma2);
		} else if(cmd2 == 'O' || cmd2 == 'o') {
			memset(dma1, 0xff, 64 * 1024);
			tmp_value = (unsigned int) wval;
			printk("tmp_value = 0x%x, wval = 0x%lx\n", tmp_value, wval);
			ptr_tmp_value = (unsigned int *)dma1;
			*ptr_tmp_value = tmp_value;
			printk("DMA Write : Base = 0x%lx, Val = 0x%08x\n", tmp_base, wval);
			udma_fast_memcpy(4, virt_to_phys(dma1), tmp_base);
		} else if(cmd2 == 'C' || cmd2 == 'c') {
			udma_fast_memcpy(4, base1, base2);
			printk("Copy DMA : Base1 = 0x%lx to Base2 = 0x%lx\n", base1, base2);
		} else {
			printk("DMA Commmand Error !!\n");
		}
#endif
#if 0
	} else if(cmd == 'E' || cmd == 'e') {
		if(cmd2 == 'R' || cmd2 == 'r') {
			memset(dma1, 0xff, 64 * 1024);
			ep_udma_fast_memcpy(4, tmp_base, virt_to_phys(dma1));
			printk("EP DMA Read : Base = 0x%0lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) dma1);
		} else if(cmd2 == 'W' || cmd2 == 'w') {
			memset(dma1, 0xff, 64 * 1024);
			tmp_value = (unsigned int) wval;
			ptr_tmp_value = (unsigned int *)dma1;
			*ptr_tmp_value = tmp_value;
			printk("EP DMA Write : Base = 0x%lx, Val = 0x%08x\n", tmp_base, wval);
			ep_udma_fast_memcpy(8, virt_to_phys(dma1), tmp_base);
			memset(dma2, 0xff, 64 * 1024);
			udma_fast_memcpy(4, tmp_base, virt_to_phys(dma2));
			printk("EP DMA Read : Base = 0x%lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) dma2);
		} else if(cmd2 == 'O' || cmd2 == 'o') {
			memset(dma1, 0xff, 64 * 1024);
			tmp_value = (unsigned int) wval;
			printk("tmp_value = 0x%x, wval = 0x%lx\n", tmp_value, wval);
			ptr_tmp_value = (unsigned int *)dma1;
			*ptr_tmp_value = tmp_value;
			printk("EP DMA Write : Base = 0x%lx, Val = 0x%08x\n", tmp_base, wval);
			ep_udma_fast_memcpy(4, virt_to_phys(dma1), tmp_base);
		} else if(cmd2 == 'C' || cmd2 == 'c') {
			ep_udma_fast_memcpy(4, base1, base2);
			printk("Copy DMA : Base1 = 0x%lx to Base2 = 0x%lx\n", base1, base2);
		} else {
			printk("DMA Commmand Error !!\n");
		}
#endif
	} else {
		printk("MMIO or DMA Commmand Error !!\n");
	}

	return count;
};

static const struct file_operations dump_reg_fileops = {
	.owner		= THIS_MODULE,
	.read		= dump_read_reg,
	.write		= dump_write_reg
};

static int qnap_pcie_ep_debug_probe(struct platform_device *pdev)
{
	struct proc_dir_entry *mode;

	// procfs
	proc_qnap_pcie_ep_debug_root = proc_mkdir("qnapepdebug", NULL);
	if(!proc_qnap_pcie_ep_debug_root) {
		printk(KERN_ERR "Couldn't create qnapep folder in procfs\n");
		return -1;
	}

	// create file of folder in procfs
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_debug_root, &qnap_pcie_ep_debug_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnapep debug procfs node\n");
		return -1;
	}

	mode = proc_create("reg", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_debug_root, &dump_reg_fileops);
	if (!mode) {
		printk(KERN_ERR "Couldn't create qnapep debug dump procfs node\n");
		return -1;
	}

	dma1 = kzalloc(64 * 1024, GFP_KERNEL);
	if (!dma1) {
		printk("kzalloc dma 1 fail !!\n");
		return -ENOMEM;
	} else {
		printk("dma 1 vir addr = 0x%lx, phy addr = 0x%lx\n", dma1, virt_to_phys(dma1));
	}

	dma2 = kzalloc(64 * 1024, GFP_KERNEL);
	if (!dma2) {
		printk("kzalloc dma 2 fail !!\n");
		return -ENOMEM;
	} else {
		printk("dma 2 vir addr = 0x%lx, phy addr = 0x%lx\n", dma2, virt_to_phys(dma2));
	}


	return 0;
}

static int qnap_pcie_ep_debug_remove(struct platform_device *pdev)
{
	remove_proc_entry("mode", proc_qnap_pcie_ep_debug_root);
	remove_proc_entry("reg", proc_qnap_pcie_ep_debug_root);
	remove_proc_entry("qnapepbug", NULL);

	return 0;
}

static struct platform_driver qnap_pcie_ep_debug_plat_driver = {
	.driver = {
		.name		= "qnap-pcie-ep-debug",
		.owner		= THIS_MODULE
	},
	.probe = qnap_pcie_ep_debug_probe,
	.remove = qnap_pcie_ep_debug_remove,
};

static int __init qnap_pcie_ep_debug_init_module(void)
{
	int rv;

	rv = platform_driver_register(&qnap_pcie_ep_debug_plat_driver);
	if(rv) {
		printk(KERN_ERR "Unable to register "
			"driver: %d\n", rv);
		return rv;
	}

	return rv;
}

static void __exit qnap_pcie_ep_debug_cleanup_module(void)
{
	platform_driver_unregister(&qnap_pcie_ep_debug_plat_driver);

	return;
}

module_init(qnap_pcie_ep_debug_init_module);
module_exit(qnap_pcie_ep_debug_cleanup_module);

MODULE_AUTHOR("Yong-Yu Yeh <tomyeh@qnap.com>");
MODULE_DESCRIPTION("EP debug driver");
MODULE_LICENSE("GPL");
