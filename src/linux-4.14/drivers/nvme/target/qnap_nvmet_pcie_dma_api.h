/*
 * drivers/misc/al/nvme_bridge/al_nvme_dma_api.h
 *
 * Annapurna Labs NVMe DMA API
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
#ifndef __AL_NVME_DMA_API_H_
#define __AL_NVME_DMA_API_H_

#define PRP_LENGTH(prp, page_size) (page_size - (prp & (page_size-1)))

enum nvme_dma_direction {
	NVME_DMA_H2D, /* Host to device */
	NVME_DMA_D2H, /* Device to host */
};

struct nvme_dma_template {
	int num_channels;
	void *dma_device;
};

/**
 * Reads (And cleans up) new entries on the given DMA's queue's completion ring.
 *
 * @param	device DMA device
 * @param	chan DMA queue
 *
 * @return	Number of completion descriptors read.
 */
unsigned int nvme_dma_chan_read_cdesc(void *device,
				      unsigned int chan);

/**
 * Submits a new DMA transaction to the IO-DMA engine
 *
 * @param	device DMA device
 * @param	chan DMA queue
 * @param	dma_addr Address of buffer in local memory space
 * @param	host_addr Address of buffer in host memory space
 * @param	len Length of buffer
 * @param	dir Direction of transaction (H2D, D2H)
 * @param	callback Callback function to be called when transaction is
 *                       complete
 * @param	callback_param Callback parameter to be passed to the callback
 *                             function
 *
 * @return	0 on success
 */
int nvme_dma_submit_buf(void *device,
			unsigned int chan,
			dma_addr_t dma_addr,
			phys_addr_t host_addr,
			unsigned int len,
			enum nvme_dma_direction dir,
			void (*callback) (void *param),
			void *callback_param);

/**
 * Register a IO-DMA device with the NVMe subsystem
 *
 * @param	tmpl IO-DMA Device template
 * @param	chan DMA queue
 * @param	dma_addr Address of buffer in local memory space
 * @param	host_addr Address of buffer in host memory space
 * @param	len Length of buffer
 * @param	dir Direction of transaction (H2D, D2H)
 * @param	callback Callback function to be called when transaction is
 *                       complete
 * @param	callback_param Callback parameter to be passed to the callback
 *                             function
 *
 * @return	0 on success
 */
//int nvme_dma_register(struct nvme_dma_template *tmpl);

/* Deregister dma device with nvme */
//void nvme_dma_deregister(struct nvme_dma_template *tmpl);

#endif /* __AL_NVME_DMA_API_H_ */
