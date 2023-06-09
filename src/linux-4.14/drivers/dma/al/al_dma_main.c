/*
 * Annapurna Labs DMA Linux driver
 * Copyright(c) 2011 Annapurna Labs.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/msi.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>

#include "al_dma.h"
#include "al_dma_sysfs.h"
#include <soc/alpine/al_hal_udma_debug.h>

#define DRV_NAME "al_dma"

enum {
	/* BAR's are enumerated in terms of pci_resource_start() terms */
	AL_DMA_UDMA_BAR		= 0,
	AL_DMA_APP_BAR		= 4,
};

static int al_dma_pci_probe(
	struct pci_dev			*pdev,
	const struct pci_device_id	*id);

static void al_dma_pci_remove(
	struct pci_dev	*pdev);

static void al_dma_pci_shutdown(
	struct pci_dev	*pdev);

static DEFINE_PCI_DEVICE_TABLE(al_dma_pci_tbl) = {
    { .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
      .device = PCI_DEVICE_ID_AL_SSM,
      .subvendor = PCI_ANY_ID,
      .subdevice = PCI_ANY_ID,
      .class = PCI_CLASS_CRYPT_NETWORK << 8,
      .class_mask = PCI_ANY_ID,
    },
/*
    { .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
      .device = PCI_DEVICE_ID_AL_SSM_VF,
      .subvendor = PCI_ANY_ID,
      .subdevice = PCI_ANY_ID,
      .class = PCI_CLASS_CRYPT_NETWORK << 8,
      .class_mask = PCI_ANY_ID,
    },
*/
	{ .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
	  .device = PCI_DEVICE_ID_AL_SSM,
	  .subvendor = PCI_ANY_ID,
	  .subdevice = PCI_ANY_ID,
	  .class = PCI_CLASS_STORAGE_RAID << 8,
	  .class_mask = PCI_ANY_ID,
	},
	{ .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
	  .device = PCI_DEVICE_ID_AL_SSM_VF,
	  .subvendor = PCI_ANY_ID,
	  .subdevice = PCI_ANY_ID,
	  .class = PCI_CLASS_STORAGE_RAID << 8,
	  .class_mask = PCI_ANY_ID,
	},
	{ PCI_VDEVICE(ANNAPURNA_LABS, PCIE_DEVICE_ID_AL_RAID) },
	{ PCI_VDEVICE(ANNAPURNA_LABS, PCIE_DEVICE_ID_AL_RAID_VF) },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, al_dma_pci_tbl);

static struct pci_driver al_dma_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= al_dma_pci_tbl,
	.probe		= al_dma_pci_probe,
	.remove		= al_dma_pci_remove,
	.shutdown	= al_dma_pci_shutdown,
};

static struct proc_dir_entry *proc_qnap_pcie_ep_root = NULL;

struct al_dma_device *g_device1, *g_device2;
void __iomem * const *g_iomap1;
void __iomem * const *g_iomap2;
struct pci_dev *g_pdev1;
struct pci_dev *g_pdev2;
unsigned char *g_dma1 = NULL;
unsigned char *g_dma2 = NULL;

extern int al_udma_state_set(struct al_udma *udma, enum al_udma_state state);
extern int al_udma_q_reset_all(struct al_udma *udma);
//extern struct al_udma *q_udma1, q_udma2;

unsigned int tomyeh = 0;

void asetting_mode(uint cmd)
{
	int status = 0;
	struct device *dev = NULL;
	int bar_reg;
	u16 ven_id;
	u16 dev_id;
	u8 rev_id;

	switch(cmd) {
	case 1:
		printk("hello !!\n");
		break;
	case 2:
		dev = &g_pdev1->dev;
		pci_read_config_word(g_pdev1, PCI_VENDOR_ID, &ven_id);
		pci_read_config_word(g_pdev1, PCI_DEVICE_ID, &dev_id);
		pci_read_config_byte(g_pdev1, PCI_REVISION_ID, &rev_id);
		pr_info("a ven_id = 0x%x\n", ven_id);
		pr_info("a dev_id = 0x%x\n", dev_id);
		pr_info("a rev_id = 0x%x\n", rev_id);

		pr_info("b ven_id = 0x%x\n", g_device1->dev_id);
		pr_info("b dev_id = 0x%x\n", g_device1->rev_id);

		pci_set_master(g_pdev1);
		pci_set_drvdata(g_pdev1, g_device1);
		dev_set_drvdata(dev, g_device1);

		status = pci_set_dma_mask(g_pdev1, DMA_BIT_MASK(40));
		if (status)
			printk("pci_set_dma_mask fail \n");

		status = pci_set_consistent_dma_mask(g_pdev1, DMA_BIT_MASK(40));
		if (status)
			printk("pci_set_consistent_dma_mask fail \n");

#ifdef CONFIG_AL_DMA_PCI_IOV
		if (PCI_FUNC(g_pdev1->devfn) == 0) {
			status = pci_enable_sriov(g_pdev1, 1);
			if (status) {
				dev_err(dev, "%s: pci_enable_sriov failed, status %d\n",
					__func__, status);
			}
		}
#endif
		printk("reinit dma1 core\n");
		status = al_dma_core_init(
                        g_device1,
                        g_iomap1[AL_DMA_UDMA_BAR],
                        g_iomap1[AL_DMA_APP_BAR]);
                if (status) {
                        dev_err(dev, "%s: al_dma_core_init failed\n", __func__);
                }

		break;
#if 1
	case 3:
		dev = &g_pdev2->dev;
                pci_read_config_word(g_pdev2, PCI_VENDOR_ID, &ven_id);
                pci_read_config_word(g_pdev2, PCI_DEVICE_ID, &dev_id);
                pci_read_config_byte(g_pdev2, PCI_REVISION_ID, &rev_id);
                pr_info("a ven_id = 0x%x\n", ven_id);
                pr_info("a dev_id = 0x%x\n", dev_id);
                pr_info("a rev_id = 0x%x\n", rev_id);

                pr_info("b ven_id = 0x%x\n", g_device2->dev_id);
                pr_info("b dev_id = 0x%x\n", g_device2->rev_id);

                pci_set_master(g_pdev2);
                pci_set_drvdata(g_pdev2, g_device2);
                dev_set_drvdata(dev, g_device2);

                status = pci_set_dma_mask(g_pdev2, DMA_BIT_MASK(40));
                if (status)
                        printk("pci_set_dma_mask fail \n");

                status = pci_set_consistent_dma_mask(g_pdev2, DMA_BIT_MASK(40));
                if (status)
                        printk("pci_set_consistent_dma_mask fail \n");

#ifdef CONFIG_AL_DMA_PCI_IOV
                if (PCI_FUNC(g_pdev2->devfn) == 0) {
                        status = pci_enable_sriov(g_pdev2, 1);
                        if (status) {
                                dev_err(dev, "%s: pci_enable_sriov failed, status %d\n",
                                        __func__, status);
                        }
                }
#endif

		status = al_dma_fast_init(
				g_device2,
				g_iomap2[AL_DMA_UDMA_BAR]);
                if (status) {
                        dev_err(dev, "%s: al_dma_fast_init failed\n", __func__);
                }
		break;
#endif
	case 4:
		printk("al_udma_state_set..\n");
		//al_udma_state_set(q_udma1, UDMA_NORMAL);
		//al_udma_q_reset(q_tx_udma, );
		break;
	case 5:
		printk("al_udma_q_reset_all....\n");
		//al_udma_q_reset_all(q_udma1);
		break;
	case 6:
		printk("al_udma_regs_print........\n");
		//al_udma_regs_print(q_udma1, AL_UDMA_DEBUG_ALL);
		break;
	case 7:
		tomyeh = 1;
		printk("tomyeh = 1\n");
		break;
	case 8:
		tomyeh = 0;
		printk("tomyeh = 0\n");
		break;
	case 9:
		printk("g_iomap1 = 0x%lx, g_device1 = 0x%lx, g_pdev1 = 0x%lx\n", g_iomap1, g_device1, g_pdev1);
		printk("g_iomap2 = 0x%lx, g_device2 = 0x%lx, g_pdev2 = 0x%lx\n", g_iomap2, g_device2, g_pdev2);
		break;
	default:
		printk("QNAP DMA Control function error\n");
		break;
	}

	return;
};

static ssize_t qnap_pcie_ep_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;

	//printk("mode_name[%d] = %s\n", g_mode, mode_name[g_mode]);

	return len;
};

static ssize_t qnap_pcie_ep_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	unsigned char value[100];
	unsigned int tmp;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	sscanf(value,"%u\n", &tmp);
	asetting_mode(tmp);

	return count;
};

static const struct file_operations qnap_pcie_ep_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= qnap_pcie_ep_read_proc,
	.write			= qnap_pcie_ep_write_proc
};

unsigned char *d_dma1 = NULL;
unsigned char *d_dma2 = NULL;

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
	} else if(cmd == 'D' || cmd == 'd') {
		if(cmd2 == 'R' || cmd2 == 'r') {
			memset(d_dma1, 0xff, 64 * 1024);
			udma_fast_memcpy(4, tmp_base, virt_to_phys(d_dma1));
			printk("DMA Read : Base = 0x%0lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) d_dma1);
		} else if(cmd2 == 'W' || cmd2 == 'w') {
			memset(d_dma1, 0xff, 64 * 1024);
			tmp_value = (unsigned int) wval;
			ptr_tmp_value = (unsigned int *)g_dma1;
			*ptr_tmp_value = tmp_value;
			printk("DMA Write : Base = 0x%lx, Val = 0x%08x\n", tmp_base, wval);
			udma_fast_memcpy(8, virt_to_phys(d_dma1), tmp_base);
			memset(d_dma2, 0xff, 64 * 1024);
			udma_fast_memcpy(4, tmp_base, virt_to_phys(d_dma2));
			printk("DMA Read : Base = 0x%lx, Val = 0x%08x\n", tmp_base, *(unsigned int *) d_dma2);
		} else if(cmd2 == 'C' || cmd2 == 'c') {
			udma_fast_memcpy(4, base1, base2);
			printk("Copy DMA : Base1 = 0x%lx to Base2 = 0x%lx\n", base1, base2);
		} else {
			printk("DMA Commmand Error !!\n");
		}
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


/******************************************************************************
 *****************************************************************************/
static int al_dma_pci_probe(
	struct pci_dev			*pdev,
	const struct pci_device_id	*id)
{
	int status = 0;

	void __iomem * const *iomap;
	struct device *dev = &pdev->dev;
	struct al_dma_device *device;
	int bar_reg;
	u16 ven_id;
	u16 dev_id;
	u8 rev_id;
	struct proc_dir_entry *mode;


	//dev_dbg
	dev_err(dev, "%s(%p, %p)\n", __func__, pdev, id);

	pci_read_config_word(pdev, PCI_VENDOR_ID, &ven_id);
	pci_read_config_word(pdev, PCI_DEVICE_ID, &dev_id);
	pci_read_config_byte(pdev, PCI_REVISION_ID, &rev_id);

	//pr_info("ven_id = 0x%x\n", ven_id);
	//pr_info("dev_id = 0x%x\n", dev_id);
	//pr_info("rev_id = 0x%x\n", rev_id);

	status = pcim_enable_device(pdev);
	if (status) {
		printk("%s: pcim_enable_device failed!\n", __func__);
		goto done;
	}

	bar_reg = pdev->is_physfn ?
		(1 << AL_DMA_UDMA_BAR) | (1 << AL_DMA_APP_BAR) :
		(1 << AL_DMA_UDMA_BAR);

	status = pcim_iomap_regions(
		pdev,
		bar_reg,
		DRV_NAME);
	if (status) {
		printk("%s: pcim_iomap_regions failed!\n", __func__);
		goto done;
	}

	iomap = pcim_iomap_table(pdev);
	if (!iomap) {
		status = -ENOMEM;
		goto done;
	}

	status = pci_set_dma_mask(pdev, DMA_BIT_MASK(40));
	if (status)
		goto done;

	status = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(40));
	if (status)
		goto done;

	device = devm_kzalloc(dev, sizeof(struct al_dma_device), GFP_KERNEL);
	if (!device) {
		status = -ENOMEM;
		goto done;
	}

	device->pdev = pdev;
	device->dev_id = dev_id;
	device->rev_id = rev_id;
	device->class_id = id->class;

	pci_set_master(pdev);
	pci_set_drvdata(pdev, device);
	dev_set_drvdata(dev, device);

	device->common.dev = &pdev->dev;

#ifdef CONFIG_AL_DMA_PCI_IOV
	if (PCI_FUNC(pdev->devfn) == 0) {
		status = pci_enable_sriov(pdev, 1);
		if (status) {
			dev_err(dev, "%s: pci_enable_sriov failed, status %d\n",
					__func__, status);
		}
	}
#endif

if (pdev->is_physfn && id->class == (PCI_CLASS_STORAGE_RAID << 8)) {
	// procfs
	proc_qnap_pcie_ep_root = proc_mkdir("qnapdma", NULL);
	if(!proc_qnap_pcie_ep_root) {
		printk(KERN_ERR "Couldn't create qnapep folder in procfs\n");
		return -EINVAL;
	}

	// create file of folder in procfs
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_root, &qnap_pcie_ep_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnapep procfs node\n");
		return -EINVAL;
	}

	mode = proc_create("reg", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_root, &dump_reg_fileops);
	if (!mode) {
		printk(KERN_ERR "Couldn't create qnapep debug dump procfs node\n");
		return -1;
	}
}

	if (pdev->is_physfn) {
		g_iomap1 = iomap;
		g_device1 = device;
		g_pdev1 = pdev;
		printk("g_iomap1 = 0x%lx, g_device1 = 0x%lx, g_pdev1 = 0x%lx\n", g_iomap1, g_device1, g_pdev1);

		g_dma1 = kzalloc(64 * 1024, GFP_KERNEL);
		g_dma2 = kzalloc(64 * 1024, GFP_KERNEL);
		printk("al_dma g_dma1 virt = 0x%lx, phy = 0x%lx\n", g_dma1, virt_to_phys(g_dma1));
		printk("al_dma g_dma2 virt = 0x%lx, phy = 0x%lx\n", g_dma2, virt_to_phys(g_dma2));

		d_dma1 = kzalloc(64 * 1024, GFP_KERNEL);
		if (!d_dma1) {
			printk("kzalloc d_dma 1 fail !!\n");
			return -ENOMEM;
		} else {
			printk("dma 1 vir addr = 0x%lx, phy addr = 0x%lx\n", d_dma1, virt_to_phys(d_dma1));
		}

		d_dma2 = kzalloc(64 * 1024, GFP_KERNEL);
		if (!d_dma2) {
			printk("kzalloc d_dma 2 fail !!\n");
			return -ENOMEM;
		} else {
			printk("dma 2 vir addr = 0x%lx, phy addr = 0x%lx\n", d_dma2, virt_to_phys(d_dma2));
		}

		status = al_dma_core_init(
			device,
			iomap[AL_DMA_UDMA_BAR],
			iomap[AL_DMA_APP_BAR]);
		if (status) {
			dev_err(dev, "%s: al_dma_core_init failed\n", __func__);
			goto done;
		}

		status = al_dma_sysfs_init(dev);
		if (status) {
			dev_err(dev, "%s: al_dma_sysfs_init failed\n", __func__);
			goto err_sysfs_init;
		}
	}
	else {
		g_iomap2 = iomap;
		g_device2 = device;
		g_pdev2 = pdev;
		printk("g_iomap2 = 0x%lx, g_device2 = 0x%lx, g_pdev2 = 0x%lx\n", g_iomap2, g_device2, g_pdev2);

		status = al_dma_fast_init(
				device,
				iomap[AL_DMA_UDMA_BAR]);
		if (status) {
			dev_err(dev, "%s: al_dma_fast_init failed\n", __func__);
			goto done;
		}
	}

	goto done;

err_sysfs_init:
	al_dma_core_terminate(device);

done:
	return status;
}

/******************************************************************************
 *****************************************************************************/
static void al_dma_pci_remove(struct pci_dev *pdev)
{
    struct al_dma_device *device = pci_get_drvdata(pdev);
    struct device *dev = &pdev->dev;
    struct msi_desc *entry;
    struct al_dma_chan *chan;
    struct msix_entry *msix;
    int i, j = 0, mask = 0;
    int err = -EINVAL;

    if (!device)
        return;

    dev_dbg(&pdev->dev, "Removing dma\n");
    printk("%s: Removing DMA\n", __func__);

    if (pdev->is_physfn) {
        printk("%s: SysFs terminate\n", __func__);
        al_dma_sysfs_terminate(dev);

        //if (device->class_id == (PCI_CLASS_STORAGE_RAID << 8))
        //    async_dmaengine_put();

        for (j = 0; j < 4; j++) {
            chan = al_dma_chan_by_index(device, j);
            dma_release_channel(&chan->common);
        }

        printk("%s: Async tx unregister\n", __func__);
        dma_async_device_unregister(&device->common);

        for (j = 0; j < 4; j++) {
            msix = &device->msix_entries[j];
            chan = al_dma_chan_by_index(device, j);
            err = irq_set_affinity_hint(msix->vector, NULL);
            if (err) {
                dev_err(dev, "irq_set_affinity_hint failed!\n");
                return err;
            }
            devm_free_irq(dev, msix->vector, chan);
        }

        for_each_pci_msi_entry(entry, pdev) {
            if (entry->irq != 0) {
                irq_dispose_mapping(entry->irq);
                entry->irq = 0;
            }
        }
        printk("%s: Cache Destory\n", __func__);
        al_dma_core_terminate(device);

        printk("%s: Disable MSIX\n", __func__);
        pci_disable_msix(pdev);

        printk("%s: Disable SRIOV\n", __func__);
        pci_disable_sriov(pdev);

        printk("%s: Remove Debug Proc\n", __func__);
        if (d_dma1) {
            kfree(d_dma1);
            d_dma1 = NULL;
        }
        if (d_dma2) {
            kfree(d_dma2);
            d_dma2 = NULL;
        }
        if (g_dma1) {
            kfree(g_dma1);
            g_dma1 = NULL;
        }
        if (g_dma2) {
            kfree(g_dma2);
            g_dma2 = NULL;
        }
        if (device->class_id == (PCI_CLASS_STORAGE_RAID << 8)) {
            remove_proc_entry("mode", proc_qnap_pcie_ep_root);
            remove_proc_entry("reg", proc_qnap_pcie_ep_root);
            remove_proc_entry("qnapdma", NULL);
        }

        printk("%s: Release Device memory\n", __func__);
        devm_kfree(dev, device);

        mask = pdev->is_physfn ?
            (1 << AL_DMA_UDMA_BAR) | (1 << AL_DMA_APP_BAR) :
            (1 << AL_DMA_UDMA_BAR);

        for (i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
            if (!(mask & (1 << i)))
                continue;
            printk("%s: BAR %d region release\n", __func__, i);
            pci_release_region(pdev, i);
        }

        printk("%s: Disable PCI Device\n", __func__);
        pci_disable_device(pdev);
    } else {
        printk("%s: Fast Terminate\n", __func__);
        al_dma_fast_terminate(device);
    }
}

static void al_dma_pci_shutdown(struct pci_dev *pdev)
{
	/* Don't call for physfn as its removal is not fully implement yet */
	if (!pdev->is_physfn)
		al_dma_pci_remove(pdev);
}

/******************************************************************************
 *****************************************************************************/
static int __init al_dma_init_module(void)
{
	int err;

	pr_info(
		"%s: Annapurna Labs DMA Driver %s\n",
		DRV_NAME,
		AL_DMA_VERSION);

	err = pci_register_driver(&al_dma_pci_driver);

	return err;
}
module_init(al_dma_init_module);

/******************************************************************************
 *****************************************************************************/
static void __exit al_dma_exit_module(void)
{
	pci_unregister_driver(&al_dma_pci_driver);
}
module_exit(al_dma_exit_module);

MODULE_VERSION(AL_DMA_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Annapurna Labs");
MODULE_DESCRIPTION("AL DMA driver");
