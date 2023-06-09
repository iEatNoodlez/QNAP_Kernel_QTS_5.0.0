/*
 * drivers/misc/al/nvme_bridge/al_nvme_dma.c
 *
 * Annapurna Labs NVMe DMA
 *
 * Copyright (C) 2013 Annapurna Labs Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>

#include "al_hal_udma_iofic.h"
#include "qnap_nvmet_pcie_dma.h"
#include "qnap_nvmet_pcie.h"
#include "qnap_pcie_ep.h"

MODULE_VERSION(AL_NVME_DMA_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Annapurna Labs");

#define DRV_NAME "al_nvme_dma"
/* TODO: verify this */
#define MAX_SIZE SZ_32K
#define MAX_DMAS 2

static int ring_alloc_order = 14;
module_param(ring_alloc_order, int, 0644);
MODULE_PARM_DESC(
	ring_alloc_order,
	 "allocate 2^n descriptors per channel"
	 " (default: 13 max: 16)");

static int tx_descs_order = 16;
module_param(tx_descs_order, int, 0644);
MODULE_PARM_DESC(
	tx_descs_order,
	"allocate 2^n of descriptors in Tx queue (default: 13)");

static int rx_descs_order = 16;
module_param(rx_descs_order, int, 0644);
MODULE_PARM_DESC(
	rx_descs_order,
	"allocate 2^n of descriptors in Rx queue (default: 13)");

enum {
	/* BAR's are enumerated in terms of pci_resource_start() terms */
	AL_DMA_UDMA_BAR		= 0,
	AL_DMA_APP_BAR		= 4,
};

static int al_nvme_dma_pci_probe(
	struct pci_dev			*pdev,
	const struct pci_device_id	*id);

static void al_nvme_dma_pci_remove(
	struct pci_dev	*pdev);

#if 0
static DEFINE_PCI_DEVICE_TABLE(al_nvme_dma_pci_tbl) = {
	{ .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
	  .device = PCI_DEVICE_ID_AL_SSM,
	  .subvendor = PCI_ANY_ID,
	  .subdevice = PCI_ANY_ID,
	  .class = PCI_CLASS_CRYPT_NETWORK << 8,
	  .class_mask = PCI_ANY_ID,
	},
	{ .vendor = PCI_VENDOR_ID_ANNAPURNA_LABS,
	  .device = PCI_DEVICE_ID_AL_SSM_VF,
	  .subvendor = PCI_ANY_ID,
	  .subdevice = PCI_ANY_ID,
	  .class = PCI_CLASS_CRYPT_NETWORK << 8,
	  .class_mask = PCI_ANY_ID,
	},
	{ 0, }
};
#endif

struct nvme_dma_template **template = NULL;

static DEFINE_PCI_DEVICE_TABLE(al_nvme_dma_pci_tbl) = {
	{ PCI_VDEVICE(ANNAPURNA_LABS, PCI_DEVICE_ID_AL_RAID_DMA) },
	{ PCI_VDEVICE(ANNAPURNA_LABS, PCI_DEVICE_ID_AL_RAID_DMA_VF) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, al_nvme_dma_pci_tbl);

static struct pci_driver al_nvme_dma_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= al_nvme_dma_pci_tbl,
	.probe		= al_nvme_dma_pci_probe,
	.remove		= al_nvme_dma_pci_remove,
};

static void al_nvme_dma_free_chan_resources(
	struct al_nvme_dma_chan *chan);

static int al_nvme_dma_alloc_chan_resources(
	struct al_nvme_dma_chan *chan);

static irqreturn_t al_nvme_dma_do_interrupt(
	int	irq,
	void	*data);

static irqreturn_t al_nvme_dma_do_interrupt_msix(
	int	irq,
	void	*data);

static int al_nvme_dma_setup_interrupts(
	struct al_nvme_dma_device *device);

static int al_nvme_dma_init_channels(
	struct al_nvme_dma_device	*device,
	int				max_channels);

static void al_nvme_dma_init_channel(
	struct al_nvme_dma_device	*device,
	struct al_nvme_dma_chan		*chan,
	int				idx);

static int al_nvme_dma_alloc_channels(
	struct al_nvme_dma_device *device);

static void al_nvme_dma_free_channels(
	struct al_nvme_dma_device *device);

static struct al_nvme_dma_sw_desc **al_nvme_dma_alloc_sw_ring(
	struct al_nvme_dma_chan	*chan,
	int			order,
	gfp_t			flags);

static void al_nvme_dma_free_sw_ring(
	struct al_nvme_dma_chan	*chan,
	int			order);

static struct al_nvme_dma_sw_desc *al_nvme_dma_alloc_ring_ent(
	struct al_nvme_dma_chan	*chan,
	gfp_t			flags);

static void al_nvme_dma_free_ring_ent(
	struct al_nvme_dma_sw_desc	*desc,
	struct al_nvme_dma_chan	*chan);

/******************************************************************************
 *****************************************************************************/
static int al_nvme_dma_pci_probe(
	struct pci_dev			*pdev,
	const struct pci_device_id	*id)
{
	int status = 0;

	void __iomem * const *iomap;
	struct device *dev = &pdev->dev;
	struct al_nvme_dma_device *device;
	int bars_mask;
	void __iomem *iobase_app;
	u16 dev_id;
	u8 rev_id;

	dev_dbg(dev, "%s(%p, %p)\n", __func__, pdev, id);

	pci_read_config_word(pdev, PCI_DEVICE_ID, &dev_id);
	pci_read_config_byte(pdev, PCI_REVISION_ID, &rev_id);

	status = pcim_enable_device(pdev);

	if (status) {
		pr_err("%s: pcim_enable_device failed!\n", __func__);
		goto done;
	}

	if (pdev->is_physfn)
		bars_mask = (1 << AL_DMA_UDMA_BAR) | (1 << AL_DMA_APP_BAR);
	else
		bars_mask = (1 << AL_DMA_UDMA_BAR);

	status = pcim_iomap_regions(
		pdev,
		bars_mask,
		DRV_NAME);

	if (status) {
		pr_err("%s: pcim_iomap_regions failed!\n", __func__);
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

	device = devm_kzalloc(dev, sizeof(struct al_nvme_dma_device),
			GFP_KERNEL);
	if (!device) {
		status = -ENOMEM;
		goto done;
	}

	device->pdev = pdev;
	device->dev_id = dev_id;
	device->rev_id = rev_id;

	pci_set_master(pdev);
	pci_set_drvdata(pdev, device);
	dev_set_drvdata(dev, device);

	if (pdev->is_physfn) {
        pr_info("%s: %d\n", __func__, __LINE__);
		status = pci_enable_sriov(pdev, 1);
		if (status) {
			dev_err(dev, "%s: pci_enable_sriov failed, status %d\n",
				__func__, status);
        }
	}

	if (pdev->is_physfn) {
        pr_info("%s: %d\n", __func__, __LINE__);
		iobase_app = iomap[AL_DMA_APP_BAR];
    }
	else
    {
        pr_info("%s: %d\n", __func__, __LINE__);
		iobase_app = NULL;
    }

	status = al_nvme_dma_core_init(
		device,
		iomap[AL_DMA_UDMA_BAR],
        iobase_app,
		true);

    if (status) {
		dev_err(dev, "%s: al_nvme_dma_core_init failed\n", __func__);
		goto done;
	}

	goto done;

done:
	return status;
}

/******************************************************************************
 *****************************************************************************/
static void al_nvme_dma_pci_remove(struct pci_dev *pdev)
{
	struct al_nvme_dma_device *device = pci_get_drvdata(pdev);
	int idx;

	if (!device)
		return;

	dev_dbg(&pdev->dev, "Removing dma\n");

	for (idx = 0; idx < MAX_DMAS; idx++) {
		if (template[idx]) {
			kfree(template[idx]);
			template[idx] = NULL;
		}
	}

	if (template) {
		kfree(template);
		template = NULL;
	}

	//nvme_dma_deregister(&device->common);

	al_nvme_dma_core_terminate(device);
}

/******************************************************************************
 *****************************************************************************/
static int __init al_nvme_dma_init_module(void)
{
	int err;

	pr_info(
		"%s: Annapurna Labs NVMe DMA Driver %s\n",
		DRV_NAME,
		AL_NVME_DMA_VERSION);

	err = pci_register_driver(&al_nvme_dma_pci_driver);

	return err;
}
module_init(al_nvme_dma_init_module);

/******************************************************************************
 *****************************************************************************/
static void __exit al_nvme_dma_exit_module(void)
{
	pci_unregister_driver(&al_nvme_dma_pci_driver);
}
module_exit(al_nvme_dma_exit_module);

/******************************************************************************
 *****************************************************************************/
int al_nvme_dma_core_init(
	struct al_nvme_dma_device	*device,
	void __iomem			*iobase_udma,
	void __iomem			*iobase_app,
	bool					dma_register)
{
	int32_t rc;
	struct nvme_dma_template *dma = &device->common;
	struct device *dev = &device->pdev->dev;
	int err, idx;

	dev_dbg(
		dev,
		"%s(%p, %p, %p)\n",
		__func__,
		device,
		iobase_udma,
		iobase_app);

	device->cache = kmem_cache_create(
		"al_nvme_dma",
		sizeof(struct al_nvme_dma_sw_desc),
		0,
		SLAB_HWCACHE_ALIGN,
		NULL);
	if (!device->cache)
		return -ENOMEM;

	device->max_channels = AL_NVME_DMA_MAX_CHANNELS;

	device->udma_regs_base = iobase_udma;
	device->app_regs_base = iobase_app;

	//memset(&device->raid_dma_params, 0, sizeof(struct al_raid_dma_params));
	memset(&device->raid_dma_params, 0, sizeof(struct al_ssm_dma_params));
	//device->raid_dma_params.dev_id = device->dev_id;
	device->raid_dma_params.rev_id = device->rev_id;
	device->raid_dma_params.udma_regs_base = device->udma_regs_base;
	//device->raid_dma_params.app_regs_base = device->app_regs_base;

	device->raid_dma_params.name =
		kmalloc(strlen(dev_name(dev)) + 1, GFP_KERNEL);
	if (device->raid_dma_params.name == NULL) {
		dev_err(dev, "kmalloc failed\n");
		return -1;
	}

	memcpy(
		device->raid_dma_params.name,
		dev_name(dev),
		strlen(dev_name(dev)) + 1);

	device->raid_dma_params.num_of_queues = AL_NVME_DMA_MAX_CHANNELS;
	//device->raid_dma_params.init_gf_tables = 0;

	//rc = al_raid_dma_init(&device->hal_raid, &device->raid_dma_params);
	rc = al_ssm_dma_init(&device->hal_raid, &device->raid_dma_params);
	if (rc) {
		dev_err(dev, "al_raid_dma_init failed\n");
		return rc;
	}

    if (device->app_regs_base == NULL) {
        pr_info("%s: device->app_regs_base is null\n", __func__);
    }
    //al_raid_init(&device->hal_raid, device->app_regs_base);

	/* enumerate and initialize channels (queues) */
	err = al_nvme_dma_init_channels(device, AL_NVME_DMA_MAX_CHANNELS);

	err = al_nvme_dma_alloc_channels(device);
	if (err) {
		dev_err(&device->pdev->dev,
			"failed to alloc channel resources\n");
		return err;
	}

	/* enable RAID DMA engine */
	//rc = al_raid_dma_state_set(&device->hal_raid, UDMA_NORMAL);
	rc = al_ssm_dma_state_set(&device->hal_raid, UDMA_NORMAL);

	dma->dma_device = (void *)device;
	err = al_nvme_dma_setup_interrupts(device);

	if (err) {
		dev_err(dev, "failed to setup interrupts\n");
		return err;
	}

	if (!template) {
		template = kcalloc(MAX_DMAS, sizeof(struct nvme_dma_template *), GFP_KERNEL);
		if (!template) {
			pr_info("%s: failed to allocate template\n", __func__);
			return err;
		}
    }

    for (idx = 0; idx < MAX_DMAS; idx++) {
        if (!template[idx]) {
            break;
        }
    }

	template[idx] = kmalloc(sizeof(struct nvme_dma_template), GFP_KERNEL);
    memcpy(template[idx], dma, sizeof(struct nvme_dma_template));

#if 0
	if (dma_register) {
		err = nvme_dma_register(&device->common);
		if (err)
			dev_err(dev, "failed to register dma device\n");
	}
#endif
	return err;
}

/******************************************************************************
 *****************************************************************************/
int al_nvme_dma_core_terminate(
	struct al_nvme_dma_device	*device)
{
	int status = 0;

	struct device *dev = &device->pdev->dev;

	dev_dbg(
		dev,
		"%s(%p)\n",
		__func__,
		device);

	al_nvme_dma_free_channels(device);

	kfree(device->raid_dma_params.name);

	kmem_cache_destroy(device->cache);

	return status;
}

/******************************************************************************
 *****************************************************************************/
static int al_nvme_dma_init_channels(
		struct al_nvme_dma_device *device, int max_channels)
{
	int i;
	struct al_nvme_dma_chan *chan;
	struct nvme_dma_template *dma = &device->common;

	for (i = 0; i < max_channels; i++) {
		chan = kzalloc(sizeof(*chan), GFP_KERNEL);
		if (!chan)
			break;

		al_nvme_dma_init_channel(device, chan, i);
	}
	dma->num_channels = i;
	return i;
}

/******************************************************************************
 *****************************************************************************/
static void al_nvme_dma_init_channel(struct al_nvme_dma_device *device,
			 struct al_nvme_dma_chan *chan, int idx)
{
	struct device *dev = &device->pdev->dev;

	dev_dbg(
		dev,
		"%s(%p, %p, %d)\n",
		__func__,
		device,
		chan,
		idx);

	chan->device = device;
	chan->idx = idx;
	chan->hal_raid = &device->hal_raid;

	spin_lock_init(&chan->prep_lock);
	spin_lock_init(&chan->cleanup_lock);
	device->channels[idx] = chan;
}

/******************************************************************************
 *****************************************************************************/
static int al_nvme_dma_setup_interrupts(struct al_nvme_dma_device *device)
{
	struct al_nvme_dma_chan *chan;
	struct pci_dev *pdev = device->pdev;
	struct device *dev = &pdev->dev;
	struct msix_entry *msix;
	int i, j, msixcnt;
	int err = -EINVAL;

	/* The number of MSI-X vectors should equal the number of channels */
	msixcnt = device->common.num_channels;

	for (i = 0; i < msixcnt; i++)
		device->msix_entries[i].entry = 3 + i;

	//err = pci_enable_msix(pdev, device->msix_entries, msixcnt);
    err = pci_enable_msix_exact(pdev, device->msix_entries, msixcnt);

	if (err < 0) {
		dev_err(dev, "pci_enable_msix failed! using intx instead.\n");
		goto intx;
	}

	if (err > 0) {
		dev_err(dev, "pci_enable_msix failed! msix_single_vector.\n");
		goto msix_single_vector;
	}

	for (i = 0; i < msixcnt; i++) {
		msix = &device->msix_entries[i];

		chan = al_nvme_dma_chan_by_index(device, i);

		dev_dbg(dev, "%s: requesting irq %d\n", __func__, msix->vector);

		snprintf(device->irq_tbl[i].name, AL_NVME_DMA_IRQNAME_SIZE,
			 "al-nvme-dma-comp-%d@pci:%s", i,
			 pci_name(pdev));

		err = devm_request_irq(
			dev,
			msix->vector,
			al_nvme_dma_do_interrupt_msix,
			0,
			device->irq_tbl[i].name,
			chan);

		if (err) {
			dev_err(dev, "devm_request_irq failed!.\n");

			for (j = 0; j < i; j++) {
				msix = &device->msix_entries[j];
				chan = al_nvme_dma_chan_by_index(device, j);
				devm_free_irq(dev, msix->vector, chan);
			}

			/* goto msix_single_vector; */
			return -EIO;
		}

#if 0
		/* setup interrupt affinity */
		if (cpu_online(chan->idx))
			cpumask_set_cpu(chan->idx, &chan->affinity_mask);
		else
			cpumask_copy(&chan->affinity_mask, cpu_online_mask);

		dev_dbg(
			dev,
			"Setting affinity of channel %d to %lx\n",
			chan->idx,
			chan->affinity_mask.bits[0]);

		err = irq_set_affinity_hint(msix->vector, &chan->affinity_mask);
		if (err) {
			dev_err(dev, "irq_set_affinity_hint failed!\n");
			return err;
		}

		err = irq_set_affinity(msix->vector, &chan->affinity_mask);
		if (err) {
			dev_err(dev, "irq_set_affinity failed!\n");
			return err;
		}
#endif
	}

	err = al_udma_iofic_config(
		(struct unit_regs *)device->udma_regs_base,
		AL_IOFIC_MODE_MSIX_PER_Q, 0x480, 0x480, 0x1E0, 0x1E0);
	//err = al_udma_iofic_config(
	//	(struct unit_regs *)device->udma_regs_base,
	//	AL_IOFIC_MODE_MSIX_PER_Q, 0x480, 0x1E0);
	if (err) {
		dev_err(dev, "al_udma_iofic_config failed!.\n");
		return err;
	}

	al_udma_iofic_unmask(
		(struct unit_regs *)device->udma_regs_base,
        AL_UDMA_IOFIC_LEVEL_PRIMARY,
		AL_INT_GROUP_B,
		((1 << (device->common.num_channels + 1)) - 1));

	device->irq_mode = AL_NVME_IRQ_MSIX_PER_Q;

	goto done;

msix_single_vector:
	msix = &device->msix_entries[0];

	msix->entry = 0;

	//err = pci_enable_msix(pdev, device->msix_entries, 1);
	err = pci_enable_msix_exact(pdev, device->msix_entries, 1);

	if (err)
		goto intx;

	snprintf(device->irq_tbl[0].name, AL_NVME_DMA_IRQNAME_SIZE,
		 "al-nvme-dma-msix-all@pci:%s", pci_name(pdev));

	err = devm_request_irq(
		dev,
		msix->vector,
		al_nvme_dma_do_interrupt,
		IRQF_TRIGGER_RISING,
		device->irq_tbl[0].name, device);

	if (err) {
		pci_disable_msix(pdev);
		goto intx;
	}

	device->irq_mode = AL_NVME_IRQ_MSIX_PER_GROUP;

	goto done;

intx:
	snprintf(device->irq_tbl[0].name, AL_NVME_DMA_IRQNAME_SIZE,
		 "al-nvme-dma-intx-all@pci:%s", pci_name(pdev));

	err = devm_request_irq(dev, pdev->irq, al_nvme_dma_do_interrupt,
			       IRQF_SHARED, device->irq_tbl[0].name, device);
	if (err)
		goto err_no_irq;

	device->irq_mode = AL_NVME_IRQ_LEGACY;

done:
	return 0;

err_no_irq:
	/* Disable all interrupt generation */

	dev_err(dev, "no usable interrupts\n");
	return err;
}

/******************************************************************************
 *****************************************************************************/
/* al_nvme_dma_alloc_chan_resources - allocate/initialize tx and rx descriptor
 * rings
 */
static int al_nvme_dma_alloc_chan_resources(struct al_nvme_dma_chan *chan)
{
	struct device *dev = to_dev(chan);
	struct al_nvme_dma_sw_desc **sw_ring;
	struct al_udma_q_params tx_params;
	struct al_udma_q_params rx_params;
	uint32_t rc = 0;

	dev_dbg(dev, "al_nvme_dma_alloc_chan_resources: channel %d\n",
		chan->idx);

	/* have we already been set up? */
	if (chan->sw_ring)
		return 1 << chan->alloc_order;

	chan->tx_descs_num = 1 << tx_descs_order;
	chan->rx_descs_num = 1 << rx_descs_order;

	/* allocate coherent memory for Tx submission descriptors */
	chan->tx_dma_desc_virt = dma_alloc_coherent(dev,
						    chan->tx_descs_num *
						    sizeof(union al_udma_desc),
						    &chan->tx_dma_desc,
						    GFP_KERNEL);

	if (chan->tx_dma_desc_virt == NULL) {
		dev_err(dev, "failed to allocate %d bytes of coherent memory for Tx submission descriptors\n",
			chan->tx_descs_num * sizeof(union al_udma_desc));
		return -ENOMEM;
	}
	dev_dbg(dev, "allocted tx descriptor ring: virt 0x%p phys 0x%llx\n",
		chan->tx_dma_desc_virt, (u64)chan->tx_dma_desc);

	/* allocate coherent memory for Rx submission descriptors */
	chan->rx_dma_desc_virt = dma_alloc_coherent(dev,
						    chan->rx_descs_num *
						    sizeof(union al_udma_desc),
						    &chan->rx_dma_desc,
						    GFP_KERNEL);

	if (chan->rx_dma_desc_virt == NULL) {
		dev_err(dev, "failed to allocate %d bytes of coherent memory for Rx submission descriptors\n",
			chan->rx_descs_num * sizeof(union al_udma_desc));

		al_nvme_dma_free_chan_resources(chan);
		return -ENOMEM;
	}
	dev_dbg(dev, "allocated rx descriptor ring: virt 0x%p phys 0x%llx\n",
		chan->rx_dma_desc_virt, (u64)chan->rx_dma_desc);

	/* allocate coherent memory for Rx completion descriptors */
	chan->rx_dma_cdesc_virt =
			dma_alloc_coherent(dev,
					   chan->rx_descs_num *
					   AL_NVME_DMA_RAID_RX_CDESC_SIZE,
					   &chan->rx_dma_cdesc,
					   GFP_KERNEL);
	if (chan->rx_dma_cdesc_virt == NULL) {
		dev_err(dev, "failed to allocate %d bytes of coherent memory for Rx completion descriptors\n",
			chan->rx_descs_num * AL_NVME_DMA_RAID_RX_CDESC_SIZE);

		al_nvme_dma_free_chan_resources(chan);
		return -ENOMEM;
	}

	/* clear the Rx completion descriptors to avoid false positive */
	memset(
		chan->rx_dma_cdesc_virt,
		0,
		chan->rx_descs_num * AL_NVME_DMA_RAID_RX_CDESC_SIZE);

	dev_dbg(
		dev,
		"allocted rx completion desc ring: virt 0x%p phys 0x%llx\n",
		chan->rx_dma_cdesc_virt, (u64)chan->rx_dma_cdesc);

	tx_params.size = chan->tx_descs_num;
	tx_params.desc_base = chan->tx_dma_desc_virt;
	tx_params.desc_phy_base = chan->tx_dma_desc;
	tx_params.cdesc_base = NULL; /* don't use Tx completion ring */
	tx_params.cdesc_phy_base = 0;
	tx_params.cdesc_size = AL_NVME_DMA_RAID_TX_CDESC_SIZE;
							/* size is needed */

	rx_params.size = chan->rx_descs_num;
	rx_params.desc_base = chan->rx_dma_desc_virt;
	rx_params.desc_phy_base = chan->rx_dma_desc;
	rx_params.cdesc_base = chan->rx_dma_cdesc_virt;
	rx_params.cdesc_phy_base = chan->rx_dma_cdesc;
	rx_params.cdesc_size = AL_NVME_DMA_RAID_RX_CDESC_SIZE;

	/* alloc sw descriptors */
	if (ring_alloc_order < AL_NVME_DMA_SW_RING_MIN_ORDER) {
		dev_err(
			dev,
			"%s: ring_alloc_order = %d < %d!\n",
			__func__,
			ring_alloc_order,
			AL_NVME_DMA_SW_RING_MIN_ORDER);

		al_nvme_dma_free_chan_resources(chan);
		return -EINVAL;
	} else if (ring_alloc_order > AL_NVME_DMA_SW_RING_MAX_ORDER) {
		dev_err(
			dev,
			"%s: ring_alloc_order = %d > %d!\n",
			__func__,
			ring_alloc_order,
			AL_NVME_DMA_SW_RING_MAX_ORDER);

		al_nvme_dma_free_chan_resources(chan);
		return -EINVAL;
	} else if (ring_alloc_order > rx_descs_order) {
		dev_warn(
			dev,
			"%s: ring_alloc_order > rx_descs_order (%d>%d)!\n",
			__func__,
			ring_alloc_order,
			rx_descs_order);
	}

	sw_ring =
		al_nvme_dma_alloc_sw_ring(chan, ring_alloc_order, GFP_KERNEL);
	if (!sw_ring)
		return -ENOMEM;

	spin_lock_bh(&chan->cleanup_lock);
	spin_lock_bh(&chan->prep_lock);
	chan->sw_ring = sw_ring;
	chan->head = 0;
	chan->tail = 0;
	chan->alloc_order = ring_alloc_order;
	spin_unlock_bh(&chan->prep_lock);
	spin_unlock_bh(&chan->cleanup_lock);

	//rc = al_raid_dma_q_init(&chan->device->hal_raid, chan->idx,
	rc = al_ssm_dma_q_init(&chan->device->hal_raid, chan->idx,
				&tx_params, &rx_params, AL_MEM_CRC_MEMCPY_Q);
	if (rc) {
		dev_err(dev, "failed to initialize hal q %d. rc %d\n",
			chan->idx, rc);
		al_nvme_dma_free_chan_resources(chan);
		return rc;
	}

	/* should we return less ?*/
	return  1 << chan->alloc_order;
}

/******************************************************************************
 *****************************************************************************/
/* al_nvme_dma_free_chan_resources - free tx and rx descriptor rings
 * @chan: channel to be free
 */
static void al_nvme_dma_free_chan_resources(struct al_nvme_dma_chan *chan)
{
	struct device *dev = to_dev(chan);

	dev_dbg(dev, "%s: %p\n", __func__, chan);

	al_nvme_dma_free_sw_ring(chan, ring_alloc_order);

	if (chan->tx_dma_desc_virt != NULL) {
		dma_free_coherent(
			dev,
			chan->tx_descs_num * sizeof(union al_udma_desc),
			chan->tx_dma_desc_virt, chan->tx_dma_desc);
		chan->tx_dma_desc_virt = NULL;
	}

	if (chan->rx_dma_desc_virt != NULL) {
		dma_free_coherent(
			dev,
			chan->rx_descs_num * sizeof(union al_udma_desc),
			chan->rx_dma_desc_virt,
			chan->rx_dma_desc);
		chan->rx_dma_desc_virt = NULL;
	}

	if (chan->rx_dma_cdesc_virt != NULL) {
		dma_free_coherent(dev, chan->rx_descs_num *
				  AL_NVME_DMA_RAID_RX_CDESC_SIZE,
				  chan->rx_dma_cdesc_virt, chan->rx_dma_cdesc);
		chan->rx_dma_desc_virt = NULL;
	}

	return;
}

/******************************************************************************
 *****************************************************************************/
/* Allocate/initialize tx and rx descriptor rings for all channels
 */
static int al_nvme_dma_alloc_channels(struct al_nvme_dma_device *device)
{
	int i, j;
	int err = -EINVAL;

	for (i = 0; i < device->common.num_channels; i++) {
		err = al_nvme_dma_alloc_chan_resources(device->channels[i]);

		if (err < 0) {
			dev_err(
				&device->pdev->dev,
				"failed to alloc resources for channel %d\n",
				i);

			for (j = 0; j < i; j++) {
				al_nvme_dma_free_chan_resources(
						device->channels[j]);
			}
			return err;
		}
	}

	return 0;
}

/******************************************************************************
 *****************************************************************************/
/* Free tx and rx descriptor rings for all channels
 */
static void al_nvme_dma_free_channels(struct al_nvme_dma_device *device)
{
	int i;

	for (i = 0; i < device->common.num_channels; i++)
		al_nvme_dma_free_chan_resources(device->channels[i]);
}

/******************************************************************************
 *****************************************************************************/
static struct al_nvme_dma_sw_desc **al_nvme_dma_alloc_sw_ring(
	struct al_nvme_dma_chan	*chan,
	int			order,
	gfp_t			flags)
{
	struct al_nvme_dma_sw_desc **ring;
	int descs = 1 << order;
	int i;

	/* allocate the array to hold the software ring */
	ring = kcalloc(descs, sizeof(*ring), flags);
	if (!ring)
		return NULL;
	for (i = 0; i < descs; i++) {
		ring[i] = al_nvme_dma_alloc_ring_ent(chan, flags);
		if (!ring[i]) {
			while (i--)
				al_nvme_dma_free_ring_ent(ring[i], chan);
			kfree(ring);
			return NULL;
		}
	}

	return ring;
}

/******************************************************************************
 *****************************************************************************/
void al_nvme_dma_free_sw_ring(
	struct al_nvme_dma_chan	*chan,
	int			order)
{
	int i;
	int descs = 1 << order;

	/* allocate the array to hold the software ring */
	for (i = 0; i < descs; i++)
		al_nvme_dma_free_ring_ent(chan->sw_ring[i], chan);

	kfree(chan->sw_ring);
}

/******************************************************************************
 *****************************************************************************/
static struct al_nvme_dma_sw_desc *al_nvme_dma_alloc_ring_ent(
	struct al_nvme_dma_chan	*chan,
	gfp_t			flags)
{
	struct al_nvme_dma_sw_desc *desc;

	desc = kmem_cache_zalloc(chan->device->cache, flags);
	if (!desc)
		return NULL;

	return desc;
}

/******************************************************************************
 *****************************************************************************/
static void al_nvme_dma_free_ring_ent(
	struct al_nvme_dma_sw_desc	*desc,
	struct al_nvme_dma_chan	*chan)
{
	kmem_cache_free(chan->device->cache, desc);
}

/******************************************************************************
 *****************************************************************************/
/**
 * al_nvme_dma_get_sw_desc - get sw desc
 * @chan: dma channel to operate on
 * @num: the number of required sw descriptos
 */
int al_nvme_dma_get_sw_desc(struct al_nvme_dma_chan *chan, int num)
{
	spin_lock_bh(&chan->prep_lock);
	/* never allow the last descriptor to be consumed, we need at
	 * least one free at all times to allow for on-the-fly ring
	 * resizing.
	 */
	if (likely(al_nvme_dma_ring_space(chan) >= num)) {
		dev_dbg(to_dev(chan), "%s: (%x:%x)\n",
			__func__, chan->head, chan->tail);
		return 0;  /* with chan->prep_lock held */
	}

	spin_unlock_bh(&chan->prep_lock);
	pr_info("%s: fail (%x:%x)\n",
			__func__, chan->head, chan->tail);

	return -ENOMEM;
}

/******************************************************************************
 *****************************************************************************/
/**
 * al_nvme_dma_do_interrupt - handler used for single vector interrupt mode
 * @irq: interrupt id
 * @data: interrupt data
 */
static irqreturn_t al_nvme_dma_do_interrupt(int irq, void *data)
{
	pr_debug("%s(%d, %p)\n", __func__, irq, data);

	/* TODO: handle interrupt registers */

	return IRQ_HANDLED;
}

/******************************************************************************
 *****************************************************************************/
/**
 * al_nvme_dma_do_interrupt_msix - handler used for vector-per-channel
 * interrupt mode
 * @irq: interrupt id
 * @data: interrupt data
 */
static irqreturn_t al_nvme_dma_do_interrupt_msix(int irq, void *data)
{
	pr_debug("%s(%d, %p)\n", __func__, irq, data);

	return IRQ_HANDLED;
}

/******************************************************************************
 *****************************************************************************/
static inline void al_nvme_xaction_add_dst(struct al_nvme_dma_xaction *xaction_data,
					   int idx,
					   unsigned int len, al_phys_addr_t dst)
{
	struct al_raid_transaction *xaction = &xaction_data->hal_xaction;
	xaction_data->dst_bufs[idx].addr = dst;
	xaction_data->dst_bufs[idx].len = len;
	xaction_data->dst_block.num = idx + 1;
	xaction->dsts_blocks = &xaction_data->dst_block;
	xaction->num_of_dsts = 1;
	xaction->total_dst_bufs = idx + 1;
}

static inline void al_nvme_xaction_add_src(struct al_nvme_dma_xaction *xaction_data,
					   int idx,
					   unsigned int len, al_phys_addr_t src)
{
	struct al_raid_transaction *xaction = &xaction_data->hal_xaction;

	xaction_data->src_bufs[idx].addr = src;
	xaction_data->src_bufs[idx].len = len;
	xaction_data->src_block.num = idx + 1;
	xaction->srcs_blocks = &xaction_data->src_block;
	xaction->num_of_srcs = 1;
	xaction->total_src_bufs = idx + 1;
}

/******************************************************************************
 *****************************************************************************/
static inline void al_nvme_desc_xaction_init(struct al_nvme_dma_xaction *xaction_data)
{
	struct al_raid_transaction *xaction = &xaction_data->hal_xaction;
	xaction->op = AL_RAID_OP_MEM_CPY;
    xaction->flags = AL_SSM_BARRIER;
    //xaction->flags = AL_SSM_BARRIER | AL_SSM_INTERRUPT;
	xaction_data->src_block.bufs = &xaction_data->src_bufs[0];
	xaction_data->dst_block.bufs = &xaction_data->dst_bufs[0];
}

/******************************************************************************
 *****************************************************************************/
static int al_nvme_get_sw_desc(struct al_nvme_dma_chan *chan,
		struct al_nvme_dma_sw_desc **desc)
{
	if (unlikely(al_nvme_dma_get_sw_desc(chan, 1))) {
		dev_err(
			to_dev(chan),
			"%s: al_nvme_dma_get_sw_desc failed!\n",
			__func__);
		return -ENOSPC;
	}
	*desc = al_nvme_dma_get_ring_ent(chan, chan->head);

	return 0;
}

unsigned int nvme_dma_chan_read_cdesc(void *device,
				      unsigned int chan)
{
	struct al_nvme_dma_device *dma_device = device;
	struct al_nvme_dma_chan *dma_chan = dma_device->channels[chan];
	u32 comp_status;
	struct al_nvme_dma_sw_desc *desc;
	int idx = dma_chan->tail;
	unsigned int descs_read = 0;

	spin_lock_bh(&dma_chan->cleanup_lock);
	if (!al_nvme_dma_ring_active(dma_chan)) {
		spin_unlock_bh(&dma_chan->cleanup_lock);
		return 0;
	}

	if (al_raid_dma_completion(dma_chan->hal_raid, dma_chan->idx,
				   &comp_status)) {
		AL_NVME_MB_READ_DEPENDS();
		desc = al_nvme_dma_get_ring_ent(dma_chan, idx);

		if (desc->callback)
			desc->callback(desc->callback_param);

		AL_NVME_MB();
		dma_chan->tail = dma_chan->tail + 1;
		descs_read++;
	}

	spin_unlock_bh(&dma_chan->cleanup_lock);

	return descs_read;
}
EXPORT_SYMBOL_GPL(nvme_dma_chan_read_cdesc);

int nvme_dma_submit_buf(void *device,
			unsigned int chan,
			dma_addr_t dma_addr,
			phys_addr_t host_addr,
			unsigned int len,
			enum nvme_dma_direction dir,
			void (*callback) (void *param),
			void *callback_param)
{
	struct al_nvme_dma_device *dev = device;
	struct al_nvme_dma_chan *dma_chan;
	struct al_nvme_dma_xaction *dma_xaction;
	struct al_nvme_dma_sw_desc *desc;
	int err = 0;

	dma_chan = dev->channels[chan];
	dma_xaction = &dma_chan->xaction_data;

	err = al_nvme_get_sw_desc(dma_chan, &desc);
	if (unlikely(err)) {
		/* TODO: error handling */
		goto done;
	}

	al_nvme_desc_xaction_init(dma_xaction);

	if (unlikely(dir == NVME_DMA_D2H)) {
		al_nvme_xaction_add_src(dma_xaction, 0,
					len,
					dma_addr);

		al_nvme_xaction_add_dst(dma_xaction, 0,
					len,
					host_addr);
	} else {
		al_nvme_xaction_add_src(dma_xaction, 0,
					len,
					host_addr);

		al_nvme_xaction_add_dst(dma_xaction, 0,
					len,
					dma_addr);
	}

	desc->callback = callback;
	desc->callback_param = callback_param;

	/* prepare and submit xaction */
	err = al_raid_dma_submit(dma_chan->hal_raid, dma_chan->idx,
				&dma_xaction->hal_xaction);
	if (unlikely(err)) {
		/* TODO: error handling */
		pr_err("al_raid_dma_prepare failed\n");
		spin_unlock_bh(&dma_chan->prep_lock);
		goto done;
	}

    dma_chan->head += 1;
	spin_unlock_bh(&dma_chan->prep_lock);

done:
	return err;
}
EXPORT_SYMBOL_GPL(nvme_dma_submit_buf);
