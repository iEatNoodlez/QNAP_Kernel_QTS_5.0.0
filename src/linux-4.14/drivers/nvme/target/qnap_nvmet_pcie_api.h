 /*
 * Annapurna Labs NVMe Bridge Reference driver, NVMe Execution engine
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

#ifndef __QNAP_NVMET_PCIE_API_H
#define __QNAP_NVMET_PCIE_API_H

#include <linux/types.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/cache.h>
#include <linux/circ_buf.h>
#include <linux/cpumask.h>
#include <asm/barrier.h>

/**
 * Initialize a new requests ring buffer.
 *
 * @param	ring The requests ring buffer to be initialized.
 * @param	size The size of the ring buffer to be created.
 *
 * @return	0 on success
 */
int al_nvme_reqs_ring_init(struct al_reqs_ring *ring, unsigned int size);

/**
 * Free a requests ring buffer.
 *
 * @param	ring The requests ring buffer to be freed.
 *
 * @return	0 on success
 */
void al_nvme_reqs_ring_free(struct al_reqs_ring *ring);

/**
 * Alerts the NVMe controller that the number of I/O queues in the system has
 * changed.
 * This is normally called when ctrl->io_queues_num is updated.
 *
 * @param	ctrl NVMe Controller
 *
 * @return	0 on success
 */
void al_nvme_queues_num_updated(struct al_nvme_ctrl *ctrl);

/**
 * Initialize a new SQ
 *
 * @param	ctrl NVMe Controller
 * @param	sq_id SQ ID
 * @param	cq_id CQ ID
 * @param	queue_size Queue Size (1-based)
 * @param	in_host_mem True if this SQ resides in host memory
 * @param	base_addr Base Address of the queue (Host/Alpine memory)
 *
 * @return	0 on success
 */
int al_nvme_sq_init(struct al_nvme_ctrl *ctrl, u16 sq_id, u16 cq_id,
		    u32 queue_size, bool in_host_mem, u64 base_addr);

void _free_sq(struct al_nvme_ctrl *ctrl, int idx);
/**
 * Initialize a new CQ
 *
 * @param	ctrl NVMe Controller
 * @param	sq_id CQ ID
 * @param	queue_size Queue Size (1-based)
 * @param	int_vector Interrupt Vector
 * @param	host_addr Base Address of the queue in host memory
 * @param	int_enabled True if interrupts are enabled for this queue
 *
 * @return	0 on success
 */
int al_nvme_cq_init(struct al_nvme_ctrl *ctrl, u16 cq_id, u32 queue_size,
		    u16 int_vector, u64 host_addr, u8 int_enabled);

void _free_cq(struct al_nvme_ctrl *ctrl, int idx);
/**
 * Returns DMA engine template
 *
 * @param	ctrl NVMe Controller
 * @param	idx The DMA index to return
 *
 */
struct nvme_dma_template *al_nvme_dma_get(struct al_nvme_ctrl *ctrl, int idx);

/**
 * Returns the number of DMA engines registered with the given controller
 *
 * @param	ctrl NVMe Controller
 *
 * @return	Number of DMA engines
 */
int al_nvme_dma_get_count(struct al_nvme_ctrl *ctrl);

/**
 * Execute an Admin request
 * Called when a new Admin request is ready to be executed.
 *
 * @param	req The Admin request to be executed
 *
 * @return	NVME_REQ_COMPLETE if command execution has completed
 *              successfully.
 *              NVME_REQ_IN_PROGRESS if command execution has begun, and is
 *              currently in progress.
 */
enum al_req_execution_status 
            al_nvme_admin_request_execute(struct al_nvme_req *req);

/**
 * Initialize the Admin engine.
 * Called on initialization of the controller.
 * The admin engine may use ctrl->admin_data as a place holder
 * for private data.
 *
 * @param	ctrl NVMe Controller
 *
 * @return	0 on success
 */
int al_nvme_admin_init(struct al_nvme_ctrl *ctrl);

/**
 * Free the admin engine
 * Called on destruction of the controller.
 *
 * @param	ctrl NVMe Controller
 *
 */
void al_nvme_admin_free(struct al_nvme_ctrl *ctrl);

int print_debug_list(void);

#endif /* __QNAP_NVMET_PCIE_API_H */
