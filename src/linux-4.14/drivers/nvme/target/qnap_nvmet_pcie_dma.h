/*
 * drivers/misc/al/nvme_bridge/al_nvme_dma.h
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

#ifndef __AL_NVME_DMA_H
#define __AL_NVME_DMA_H

#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/dmapool.h>
#include <linux/cache.h>
#include <linux/circ_buf.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#include "al_hal_ssm_raid.h"
//#include "al_hal_raid.h"
#include "qnap_nvmet_pcie_dma_api.h"

#define AL_NVME_DMA_VERSION  "0.01"

#define AL_NVME_DMA_IRQNAME_SIZE		40

#define AL_NVME_DMA_OP_MAX_BUFS		(2)
#define AL_NVME_DMA_MAX_SIZE_SHIFT_MEMCPY	16	/* 64KB */
#define AL_NVME_DMA_MAX_SIZE_MEMCPY		\
				(1 << AL_NVME_DMA_MAX_SIZE_SHIFT_MEMCPY)

/* No alignment requirements */
#define AL_NVME_DMA_ALIGN_SHIFT		0

#ifndef CONFIG_ALPINE_VP_WA
#define AL_NVME_DMA_RAID_TX_CDESC_SIZE	8
#define AL_NVME_DMA_RAID_RX_CDESC_SIZE	8
#else
/* Currently in VP it is always 16 bytes */
#define AL_NVME_DMA_RAID_TX_CDESC_SIZE	16
#define AL_NVME_DMA_RAID_RX_CDESC_SIZE	16
#endif

#define AL_NVME_DMA_MAX_CHANNELS	4

#define AL_NVME_DMA_SW_RING_MIN_ORDER	4
#define AL_NVME_DMA_SW_RING_MAX_ORDER	16

/**
 * Issue pending transaction upon sumbit:
 * 0 - no, issue when issue_pending is called
 * 1 - yes, and do nothing when issue_pending is called
 */
#define AL_NVME_DMA_ISSUE_PNDNG_UPON_SUBMIT	1

/**
 * struct al_nvme_dma_sw_desc - software descriptor
 */
struct al_nvme_dma_sw_desc {
	void (*callback) (void *param);
	void *callback_param;
};

#define to_dev(al_nvme_dma_chan) (&(al_nvme_dma_chan)->device->pdev->dev)

/* internal structure for AL Crypto IRQ
 */
struct al_nvme_dma_irq {
	char name[AL_NVME_DMA_IRQNAME_SIZE];
};

enum al_nvme_dma_irq_mode {
	AL_NVME_IRQ_MSIX_PER_Q,
	AL_NVME_IRQ_MSIX_PER_GROUP,
	AL_NVME_IRQ_LEGACY
};

extern struct nvme_dma_template **template;
/**
 * struct al_nvme_dma_device - internal representation of a DMA device
 */
struct al_nvme_dma_device {
	struct pci_dev			*pdev;
	u16				dev_id;
	u8				rev_id;

	//struct al_raid_dma_params	raid_dma_params;
	struct al_ssm_dma_params	raid_dma_params;
	void __iomem			*udma_regs_base;
	void __iomem			*app_regs_base;
	//struct al_raid_dma		hal_raid;
	struct al_ssm_dma		hal_raid;
	struct kset			*channels_kset;

	struct nvme_dma_template	common;

	struct msix_entry	msix_entries[AL_NVME_DMA_MAX_CHANNELS];
	struct al_nvme_dma_irq	irq_tbl[AL_NVME_DMA_MAX_CHANNELS];
	enum al_nvme_dma_irq_mode	irq_mode;
	struct al_nvme_dma_chan	*channels[AL_NVME_DMA_MAX_CHANNELS];
	int				max_channels;

	struct kmem_cache		*cache;
};

struct al_nvme_dma_xaction {
	struct al_raid_transaction hal_xaction;
	struct al_block src_block;
	struct al_block dst_block;
	struct al_buf src_bufs[AL_NVME_DMA_OP_MAX_BUFS];
	struct al_buf dst_bufs[AL_NVME_DMA_OP_MAX_BUFS];
};

/**
 * struct al_nvme_dma_chan - internal representation of a DMA channel
 */
struct al_nvme_dma_chan {
	/* Misc */
	/* struct dma_chan common		____cacheline_aligned; */
	//struct al_raid_dma *hal_raid;
	struct al_ssm_dma *hal_raid;
	int	idx;
	struct al_nvme_dma_device *device;
	cpumask_t affinity_mask;

	/* SW descriptors ring */
	struct al_nvme_dma_sw_desc **sw_ring;

	/* Tx UDMA hw ring */
	int tx_descs_num; /* number of descriptors in Tx queue */
	void *tx_dma_desc_virt; /* Tx descriptors ring */
	dma_addr_t tx_dma_desc;

	/* Rx UDMA hw ring */
	int rx_descs_num; /* number of descriptors in Rx queue */
	void *rx_dma_desc_virt; /* Rx descriptors ring */
	dma_addr_t rx_dma_desc;
	void *rx_dma_cdesc_virt; /* Rx completion descriptors ring */
	dma_addr_t rx_dma_cdesc;

	/* Channel allocation */
	u16 alloc_order;

	/* Preparation */
	spinlock_t prep_lock		____cacheline_aligned;
	u16 head;

	/* Completion */
	spinlock_t cleanup_lock		____cacheline_aligned_in_smp;
	u16 tail;
	bool abort;
	struct task_struct *poll_kthread;
	struct kobject kobj;

	struct al_nvme_dma_xaction xaction_data  ____cacheline_aligned_in_smp;
};

static inline u16 al_nvme_dma_ring_size(struct al_nvme_dma_chan *chan)
{
	return 1 << chan->alloc_order;
}

/* count of transactions in flight with the engine */
static inline u16 al_nvme_dma_ring_active(struct al_nvme_dma_chan *chan)
{
	return CIRC_CNT(chan->head, chan->tail, al_nvme_dma_ring_size(chan));
}

static inline u16 al_nvme_dma_ring_space(struct al_nvme_dma_chan *chan)
{
	return CIRC_SPACE(chan->head, chan->tail,
			  al_nvme_dma_ring_size(chan));
}

static inline struct al_nvme_dma_sw_desc  *
al_nvme_dma_get_ring_ent(struct al_nvme_dma_chan *chan, u16 idx)
{
	return chan->sw_ring[idx & (al_nvme_dma_ring_size(chan) - 1)];
}

/* wrapper around hardware descriptor format + additional software fields */

static inline struct al_nvme_dma_chan *
al_nvme_dma_chan_by_index(struct al_nvme_dma_device *device, int index)
{
	return device->channels[index];
}

void al_nvme_dma_cleanup_single(
	struct al_nvme_dma_chan	*chan,
	struct al_nvme_dma_sw_desc	*desc,
	uint32_t		comp_status,
	bool			abort);

int al_nvme_dma_core_init(
	struct al_nvme_dma_device	*device,
	void __iomem		*iobase_udma,
	void __iomem		*iobase_app,
	bool			dma_register);

int al_nvme_dma_core_terminate(
	struct al_nvme_dma_device	*device);

int al_nvme_dma_cleanup_fn(
	struct al_nvme_dma_chan	*chan,
	int			from_tasklet);

int al_nvme_dma_abort(
	struct al_nvme_dma_chan	*chan);

#endif /* __AL_NVME_DMA_H */
