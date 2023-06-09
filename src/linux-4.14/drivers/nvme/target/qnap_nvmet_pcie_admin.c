/*
 * drivers/misc/al/nvme_bridge/al_nvme_admin.c
 *
 * Annapurna Labs NVMe Bridge Reference driver, NVMe Admin commands handling
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

#include <linux/slab.h>
#include <generated/utsrelease.h>
#include <soc/alpine/al_hal_unit_adapter_regs.h>
#include <soc/alpine/al_hal_plat_services.h>
#include "qnap_nvmet_pcie.h"
#include "qnap_nvmet_pcie_api.h"
#include "qnap_nvmet_pcie_task_manager.h"
#include "qnap_nvmet_pcie_dma_api.h"
#include "qnap_nvmet_pcie_dma.h"
#include "qnap_pcie_ep.h"
#include <linux/platform_device.h>
#include <linux/async_tx.h>
#include "nvmet.h"

#include <linux/rculist.h>
#include <asm/unaligned.h>

#define IDENTIFY_BUFFER_SIZE	SZ_4K

struct al_nvme_admin_data {
	struct al_dma_buffer ctrl_ident_data;
	struct al_dma_buffer ns_ident_data;
	struct al_dma_buffer nslist_ident_data;
	struct al_dma_buffer ns_id_desc_ident_data;
	struct al_dma_buffer log_fw_slot;
	struct al_dma_buffer log_cmd_effects;
};

static enum al_req_execution_status
			_nvme_admin_set_features(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;

	switch (cmd->features.fid) {
	case NVME_FEAT_NUM_QUEUES:
	{
		u32 q_count = le32_to_cpu(cmd->features.dword11); // Number of Queues(NCQR, NSQR)
		u32 new_q_count;
		u32 result;

		pr_info("NVMe cmd io queue count = 0x%x\n", q_count);
		/* -2 to account for admin queue */
		new_q_count = min_t(u16, q_count & 0xFFFF, AL_NVME_QUEUES_MAX-2); // NSQR???
		//new_q_count = min_t(u16, q_count & 0xFFFF, AL_NVME_QUEUES_MAX-1); // NSQR???
		new_q_count = min_t(u16, (q_count >> 16) & 0xFFFF, new_q_count);  // NCQR

		/* Dword 0 of command completion queue entry (NCQA, NSQA) */ 
		result = (new_q_count & 0xFFFF) | ((new_q_count & 0xFFFF) << 16);

		//ctrl->io_queues_num = new_q_count + 1;
		ctrl->feat_val.num_queues = new_q_count + 1;
		al_nvme_queues_num_updated(ctrl);
		req->result = result;
		req->status = NVME_SC_SUCCESS;
		break;
	}
	case NVME_FEAT_ARBITRATION:
	{
		u32 arbitration = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.arbitration = arbitration;
		break;
	}
	case NVME_FEAT_POWER_MGMT:
	{
		u32 power_mgmt = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.power_mgmt = power_mgmt;
		break;
	}
	case NVME_FEAT_LBA_RANGE:
		break;
	case NVME_FEAT_TEMP_THRESH:
	{
		u32 temp_thresh = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.temp_thresh = temp_thresh;
		break;
	}
	case NVME_FEAT_ERR_RECOVERY:
	{
		u32 err_rec = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.err_rec = err_rec;
		break;
	}
	case NVME_FEAT_VOLATILE_WC:
	{
		u32 volatile_wc = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.volatile_wc = volatile_wc;
		break;
	}
	case NVME_FEAT_IRQ_COALESCE:
	{
		u32 int_coalescing = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.int_coalescing = int_coalescing;
		break;
	}
	case NVME_FEAT_IRQ_CONFIG:
	{
		if ((le32_to_cpu(cmd->features.dword11) & 0xFFFF) > ctrl->feat_val.num_queues) {
			req->status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
			break;
		}
		ctrl->feat_val.int_vector_config[le32_to_cpu(cmd->features.dword11) & 0xFFFF]
				                       = le32_to_cpu(cmd->features.dword11) & 0x1FFFF;
		break;
	}
	case NVME_FEAT_WRITE_ATOMIC:
	{
		u32 write_atomicity = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.write_atomicity = write_atomicity;
		break;
	}
	case NVME_FEAT_ASYNC_EVENT:
	{
		u32 async_config = le32_to_cpu(cmd->features.dword11);
		ctrl->feat_val.async_config = async_config;
		break;
	}
	case NVME_FEAT_KATO:
		break;
	case NVME_FEAT_HOST_ID:
		break;
	default:
		req->status = NVME_SC_INVALID_FIELD;
		pr_err("%s: received an invalid feature %d\n",
			 __func__, cmd->features.fid);
		break;
	}

	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
			_nvme_admin_get_features(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;

	switch (cmd->features.fid) {
	case NVME_FEAT_NUM_QUEUES:
		req->result = cpu_to_le32((ctrl->feat_val.num_queues - 1)| 
									(ctrl->feat_val.num_queues - 1) << 16 );
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_ARBITRATION:
		req->result = cpu_to_le32(ctrl->feat_val.arbitration);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_POWER_MGMT:
		req->result = cpu_to_le32(ctrl->feat_val.power_mgmt);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_LBA_RANGE:
		break;
	case NVME_FEAT_TEMP_THRESH:
		req->result = cpu_to_le32(ctrl->feat_val.temp_thresh);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_ERR_RECOVERY:
		req->result = cpu_to_le32(ctrl->feat_val.err_rec);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_VOLATILE_WC:
		req->result = cpu_to_le32(ctrl->feat_val.volatile_wc);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_IRQ_COALESCE:
		req->result = cpu_to_le32(ctrl->feat_val.int_coalescing);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_IRQ_CONFIG:
		if ((le32_to_cpu(cmd->features.dword11) & 0xFFFF) > ctrl->feat_val.num_queues) {
			req->status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
			break;
		}
		req->result = cpu_to_le32(ctrl->feat_val.int_vector_config[le32_to_cpu(cmd->features.dword11) & 0xFFFF]);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_WRITE_ATOMIC:
		req->result = cpu_to_le32(ctrl->feat_val.write_atomicity);
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_ASYNC_EVENT:
		req->result = cpu_to_le32(ctrl->feat_val.async_config); 
		req->status = NVME_SC_SUCCESS;
		break;
	case NVME_FEAT_KATO:
		break;
	case NVME_FEAT_HOST_ID:
		break;
	default:
		req->status = NVME_SC_INVALID_FIELD;
		break;
	}

	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_create_cq(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;

	u16 cq_id = le16_to_cpu(cmd->create_cq.cqid);
	u16 int_vector = le16_to_cpu(cmd->create_cq.irq_vector);
	u16 queue_size = le16_to_cpu(cmd->create_cq.qsize);
	u16 qflags = le16_to_cpu(cmd->create_cq.cq_flags);
	u64 prp1 = le64_to_cpu(cmd->create_cq.prp1);
	bool int_enabled = (qflags >> 1) & 0x1;

	/************************************************ 
		* qflags:
		* bit0 -> Physically Contiguous (PC)
		* bit1 -> Interrupts Enabled (IEN)
	 *************************************************/
	/* TODO check that parameters are legit before creating the queue */
	/* QNAP */
	if (unlikely(!cq_id)) {
		req->status = NVME_SC_QID_INVALID | NVME_SC_DNR;
		goto error;
	}
	if (unlikely(!queue_size || queue_size > NVME_CAP_MQES(ctrl->target_ctrl->cap))) {
		req->status = NVME_SC_QUEUE_SIZE | NVME_SC_DNR;
		goto error;
	}
	if (unlikely(!prp1)) {
		req->status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		goto error;
	}
	/* */
	/* PC is 0- we don't support non-contiguous queues */
	if (!(qflags & (1 << 0))) {
		req->status = NVME_SC_INVALID_FIELD;
		goto error;
	}

	if (al_nvme_cq_init(ctrl, cq_id, queue_size + 1, int_vector, prp1,
			    int_enabled)) {
		req->status = NVME_SC_INVALID_FIELD;
		goto error;
	}

	req->status = NVME_SC_SUCCESS;

error:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_create_sq(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;
	uint32_t region_memory_base0;
	struct al_pcie_port *pcie_port = ctrl->pcie_data->pcie_port_config;
	struct al_pcie_regs *regs = pcie_port->regs;
	char cmb_szu = ctrl->params.cmb_szu;
	bool in_host_mem;

	u16 cq_id = le16_to_cpu(cmd->create_sq.cqid);
	u16 sq_id = le16_to_cpu(cmd->create_sq.sqid);
	u16 queue_size = le16_to_cpu(cmd->create_sq.qsize);
	u16 qflags = le16_to_cpu(cmd->create_sq.sq_flags);
	u64 prp1 = le64_to_cpu(cmd->create_sq.prp1);

	/* TODO check that parameters are legit before creating the queue */
	/* QNAP */

	if (unlikely(!cq_id)) {
		req->status = NVME_SC_QID_INVALID | NVME_SC_DNR;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}
	if (unlikely(!sq_id)) {
		req->status = NVME_SC_QID_INVALID | NVME_SC_DNR;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}
	if (unlikely(!queue_size || queue_size > NVME_CAP_MQES(ctrl->target_ctrl->cap))) {
		req->status = NVME_SC_QUEUE_SIZE | NVME_SC_DNR;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}
	if (unlikely(!prp1)) {
		req->status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}
	/* PC is 0- we don't support non-contiguous queues */
	if (!(qflags & (1 << 0))) {
		req->status = NVME_SC_INVALID_FIELD;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}

#if 0
	/* If prp1 is the same as the BAR0 address,
	 * the feature of controller memory buffer is enable.
	 */
	region_memory_base0 = al_reg_read32(&regs->core_space[0].config_header[AL_PCI_BASE_ADDRESS_0 >> 2]);
	region_memory_base0 = region_memory_base0 + QNAP_NVME_CMB_OFST_SZU * SZ_4K * (1 << (4 * cmb_szu)) +
							(sq_id - 1) * (queue_size + 1) * (1 << ctrl->params.sqe_size_2n);;
	if (QNAP_PCIE_MLBAR0_BA(prp1) == QNAP_PCIE_MLBAR0_BA(region_memory_base0)) {
		in_host_mem = false;
	} else {
		in_host_mem = true;
	}
#else
	in_host_mem = true;
#endif
	if (al_nvme_sq_init(ctrl, sq_id, cq_id, queue_size + 1, in_host_mem, prp1)) {
		req->status = NVME_SC_INVALID_FIELD;
		printk("%s: %d\n", __func__, __LINE__);
		goto error;
	}

	req->status = NVME_SC_SUCCESS;

error:
	return NVME_REQ_COMPLETE;
}

static void _send_identify_callback(void *param)
{
	struct al_nvme_req *req = param;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct nvmet_ns *ns = req->target_req.ns;
	struct al_nvme_tm_context *ctx = NULL;
	unsigned long flags;
	const unsigned int first_cpu = ctrl->first_cpu;

	ctx = &ctrl->tm->cpu_ctx[first_cpu];

	if (ns) {
		nvmet_put_namespace(ns);
	}

	/* if get smart information log page */
	if (req->log_page_smart.buf) {
		kfree(req->log_page_smart.buf);
		req->log_page_smart.buf = NULL;
	}

	req->status = NVME_SC_SUCCESS;

	/* sent to complete queue */
    spin_lock_irqsave(&ctx->comp_queue_lock, flags);
    al_nvme_tm_queue_push(req->task, req->task_queue, req);
    spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
}

static int _send_identify_buffer(struct al_nvme_req *req, 
								dma_addr_t ident_addr, 
								u64 host_addr,
								unsigned int len)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
    struct async_submit_ctl submit;
	enum async_tx_flags flags = ASYNC_TX_XOR_ZERO_DST | ASYNC_TX_ACK;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;

	init_async_submit(&submit, 
					flags, 
					NULL,
		            _send_identify_callback,
					req, 
					NULL);

	async_memcpy_phys_by_index(host_addr + host_mem_base,
				   ident_addr,
				   0, 0,
				   len,
				   &submit,
				   req->ctrl->ctrl_id);

	return 0;
}

static enum al_req_execution_status
				_nvme_admin_identify_ctrl(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;
	struct al_nvme_admin_data *data = ctrl->admin_data;
	struct nvme_id_ctrl *id = (struct nvme_id_ctrl *) data->ctrl_ident_data.buf;
	dma_addr_t identify_addr = data->ctrl_ident_data.dma_addr;
	const char model[] = "TR-540UT";

	u64 prp1 = le64_to_cpu(cmd->identify.dptr.prp1);

	memset(id, 0, NVME_IDENTIFY_DATA_SIZE);
	/* XXX: figure out how to assign real vendors IDs. */
	id->vid = 0x1baa;
	id->ssvid = 0x1baa;

	/* Get the controller serial number from configFS */
	if (ctrl->id_ctrl.sn) {
		memcpy(id->sn, ctrl->id_ctrl.sn, sizeof(id->sn));
	}
/*
	if (ctrl->target_ctrl->subsys) {
		bin2hex(id->sn, &ctrl->target_ctrl->subsys->serial,
		min(sizeof(ctrl->target_ctrl->subsys->serial), sizeof(id->sn) / 2));
	}

	memcpy_and_pad(id->mn, sizeof(id->mn), model, sizeof(model) - 1, ' ');
*/
	if (ctrl->id_ctrl.mn) {
		memcpy(id->mn, ctrl->id_ctrl.mn, sizeof(id->mn));
	}

	if (ctrl->target_ctrl->subsys) {
		memcpy(id->fr, ctrl->target_ctrl->subsys->fw_rev, sizeof(id->fr));
	} else {
		memcpy_and_pad(id->fr, sizeof(id->fr),
								UTS_RELEASE, strlen(UTS_RELEASE), ' ');
	}

	id->rab = 6;
	/*
	 * XXX: figure out how we can assign a IEEE OUI, but until then
	 * the safest is to leave it as zeroes.
	 */
	//QNAP OUI = 24:5E:BE
	id->ieee[0] = 0xBE;
	id->ieee[1] = 0x5E;
	id->ieee[2] = 0x24;

	/* we support multiple ports and multiples hosts: */
	id->cmic = (1 << 0) | (1 << 1);

	/* no limit on data transfer sizes for now */
	id->mdts = 8;
	id->cntlid = cpu_to_le16(ctrl->target_ctrl->cntlid);
	//id->ver = cpu_to_le32(ctrl->target_ctrl->subsys->ver);
	id->ver = 0x00010300; 

	/* XXX: figure out what to do about RTD3R/RTD3 */
	//id->oaes = cpu_to_le32(1 << 8);
	id->oaes = 0;
	//id->ctratt = cpu_to_le32(1 << 0);
	id->ctratt = 0;

	id->oacs = 0;

	/*
	 * We don't really have a practical limit on the number of abort
	 * comands.  But we don't do anything useful for abort either, so
	 * no point in allowing more abort commands than the spec requires.
	 */
	id->acl = 3;

	id->aerl = NVMET_ASYNC_EVENTS - 1;

	/* first slot is read-only, only one slot supported */
	id->frmw = (1 << 0) | (1 << 1);
	id->lpa = (1 << 0) | (1 << 1) | (1 << 2);
	id->elpe = 0;
	id->npss = 0;

	/* We support keep-alive timeout in granularity of seconds */
	//id->kas = cpu_to_le16(NVMET_KAS);
	id->kas = 0;

	id->sqes = (0x6 << 4) | 0x6;
	id->cqes = (0x4 << 4) | 0x4;

	/* no enforcement soft-limit for maxcmd - pick arbitrary high value */
	id->maxcmd = cpu_to_le16(NVMET_MAX_CMD);

	//id->nn = cpu_to_le32(ctrl->target_ctrl->subsys->max_nsid);
	id->nn = 8;
	id->oncs = cpu_to_le16(NVME_CTRL_ONCS_DSM);

	/* XXX: don't report vwc if the underlying device is write through */
	//id->vwc = NVME_CTRL_VWC_PRESENT;
	id->vwc = 0;

	/*
	 * We can't support atomic writes bigger than a LBA without support
	 * from the backend device.
	 */
	id->awun = 0;
	id->awupf = 0;
	id->wctemp = 0x0157;

	id->sgls = 0;	/* we always support SGLs */
	//id->sgls = cpu_to_le32(1 << 0);	/* we always support SGLs */
	/*
	if (ctrl->target_ctrl->ops->has_keyed_sgls)
		id->sgls |= cpu_to_le32(1 << 2);
	if (ctrl->target_ctrl->ops->sqe_inline_size)
		id->sgls |= cpu_to_le32(1 << 20);
	*/
	if (ctrl->target_ctrl->subsys)
		strcpy(id->subnqn, ctrl->target_ctrl->subsys->subsysnqn);

	/* Max command capsule size is sqe + single page of in-capsule data */
	//id->ioccsz = cpu_to_le32((sizeof(struct nvme_command) +
	//			  ctrl->target_ctrl->ops->sqe_inline_size) / 16);
	/* Max response capsule size is cqe */
	//id->iorcsz = cpu_to_le32(sizeof(struct nvme_completion) / 16);

	//id->msdbd = ctrl->target_ctrl->ops->msdbd;

	/*
	 * Meh, we don't really support any power state.  Fake up the same
	 * values that qemu does.
	 */
	id->psd[0].max_power = cpu_to_le16(0x9c4);
	id->psd[0].entry_lat = cpu_to_le32(0x10);
	id->psd[0].exit_lat = cpu_to_le32(0x4);

	/*vendor specific - for Asmedia 2362 to report in USB protocol*/
	/*      	USB Vendor ID	USB Product ID
	TR-104CT	0x1c04       	0x035c
	TR-106CT	0x1c04       	0x036c
	TR-108C2T	0x1c04       	0x037c     */
	*(__le16*)(id->vs) = 0x1c04;
	if (strstr(id->mn, "TR-104CT"))
		*(__le16*)(id->vs+2) = 0x035c;
	else if (strstr(id->mn, "TR-106CT"))
		*(__le16*)(id->vs+2) = 0x036c;
	else if (strstr(id->mn, "TR-108C2T"))
		*(__le16*)(id->vs+2) = 0x037c;

	/* DMA (device to host): identify_addr(Local), prp1(Host) */
	if (_send_identify_buffer(req, identify_addr, prp1, IDENTIFY_BUFFER_SIZE)) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto error;
	}

	return NVME_REQ_IN_PROGRESS;
error:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_identify_ns(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;
	struct al_nvme_admin_data *data = ctrl->admin_data;
	dma_addr_t identify_addr = data->ns_ident_data.dma_addr;
	struct nvme_id_ns *id = (struct nvme_id_ns *)data->ns_ident_data.buf;
	struct nvmet_ns *ns;

	u32 nsid = le32_to_cpu(cmd->identify.nsid);
	u64 prp1 = le64_to_cpu(cmd->identify.dptr.prp1);

	if (!ctrl->target_ctrl->subsys) {
		req->status = NVME_SC_INVALID_NS | NVME_SC_DNR;
		goto out;
	}

	ns = nvmet_find_namespace(ctrl->target_ctrl, nsid);
	if (!ns) {
		pr_err("%s: Could not find namespace\n", __func__);
		req->status = NVME_SC_INVALID_NS | NVME_SC_DNR;
		goto out;
	}

	memset(id, 0, NVME_IDENTIFY_DATA_SIZE);
	/*
	 * nuse = ncap = nsze isn't aways true, but we have no way to find
	 * that out from the underlying device.
	 */
	id->ncap = id->nuse = id->nsze =
		cpu_to_le64(ns->size >> ns->blksize_shift);

	/*
	 * We just provide a single LBA format that matches what the
	 * underlying device reports.
	 */
	id->nlbaf = 0;
	id->flbas = 0;

	/*
	 * Our namespace might always be shared.  Not just with other
	 * controllers, but also with any other user of the block device.
	 */
	id->nmic = (1 << 0);

	memcpy(&id->nguid, &ns->nguid, sizeof(uuid_le));
	memcpy(&id->eui64, &ns->nguid, NVME_NIDT_EUI64_LEN);

	id->lbaf[0].ds = ns->blksize_shift;

	nvmet_put_namespace(ns);

	if (_send_identify_buffer(req, identify_addr, prp1, IDENTIFY_BUFFER_SIZE)) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto out;
	}

	return NVME_REQ_IN_PROGRESS;
out:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_identify_nslist(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;
	struct nvmet_ns *ns;
	int i = 0;
	struct al_nvme_admin_data *data = ctrl->admin_data;
	dma_addr_t identify_addr = data->nslist_ident_data.dma_addr;
	__le32 *list = (__le32 *) data->nslist_ident_data.buf;

	u32 nsid = le32_to_cpu(cmd->identify.nsid);
	u64 prp1 = le64_to_cpu(cmd->identify.dptr.prp1);

	if (!ctrl->target_ctrl->subsys)
		goto out;

	memset(list, 0, IDENTIFY_BUFFER_SIZE);

	rcu_read_lock();
	list_for_each_entry_rcu(ns, &ctrl->target_ctrl->subsys->namespaces, dev_link) {
		if (ns->nsid <= nsid) {
			continue;
		}
		list[i++] = cpu_to_le32(ns->nsid);
		if (i == NVME_IDENTIFY_DATA_SIZE / sizeof(__le32))
			break;
	}
	rcu_read_unlock();

	if (_send_identify_buffer(req, identify_addr, prp1, IDENTIFY_BUFFER_SIZE)) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto out;
	}

	return NVME_REQ_IN_PROGRESS;
out:
	return NVME_REQ_COMPLETE;
}

static u16 nvmet_copy_ns_desclist(struct page *p, u8 type, u8 len,
				    void *id, int *off)
{
	struct nvme_ns_id_desc desc = {
		.nidt = type,
		.nidl = len,
	};

	memcpy(page_address(p) + *off, &desc, sizeof(desc));
	*off += sizeof(desc);

	memcpy(page_address(p) + *off, id, len);
	*off += len;

	return 0;
}

static enum al_req_execution_status
				_nvme_admin_identify_desclist(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;
	struct al_nvme_admin_data *data = ctrl->admin_data;
	struct nvmet_ns *ns;
	struct nvme_ns_id_desc *id_desc = 
				(struct nvme_ns_id_desc *) data->ns_id_desc_ident_data.buf;
	dma_addr_t identify_addr = data->ns_id_desc_ident_data.dma_addr;

	u32 nsid = le32_to_cpu(cmd->identify.nsid);
	u64 prp1 = le64_to_cpu(cmd->identify.dptr.prp1);

	struct page *page = virt_to_page(id_desc);
	int offset = 0;

	if (!ctrl->target_ctrl->subsys) {
		req->status = NVME_SC_INVALID_NS | NVME_SC_DNR;
		goto out;
	}

	ns = nvmet_find_namespace(ctrl->target_ctrl, nsid);
	if (!ns) {
		req->status = NVME_SC_INVALID_NS | NVME_SC_DNR;
		goto out;
	}

	if (memchr_inv(&ns->nguid, 0, sizeof(ns->nguid))) {
		nvmet_copy_ns_desclist(page, NVME_NIDT_EUI64,
									NVME_NIDT_EUI64_LEN,
									&ns->nguid, &offset);
	}
	if (memchr_inv(&ns->nguid, 0, sizeof(ns->nguid))) {
		nvmet_copy_ns_desclist(page, NVME_NIDT_NGUID,
									NVME_NIDT_NGUID_LEN,
									&ns->nguid, &offset);
	}
	if (memchr_inv(&ns->uuid, 0, sizeof(ns->uuid))) {
		nvmet_copy_ns_desclist(page, NVME_NIDT_UUID,
									NVME_NIDT_UUID_LEN,
									&ns->uuid, &offset);
	}

	nvmet_put_namespace(ns);

	if (_send_identify_buffer(req, identify_addr, prp1, IDENTIFY_BUFFER_SIZE)) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto out;
	}

	return NVME_REQ_IN_PROGRESS;

out:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_delete_cq(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;

	/* TODO implement me */
	/* QNAP */
	u16 qid = le16_to_cpu(cmd->delete_queue.qid);

	if (unlikely(!qid || !ctrl->cq[qid] || qid >=AL_NVME_QUEUES_MAX))
	{
		req->status = NVME_SC_QID_INVALID;
		return NVME_REQ_COMPLETE;
	}

	printk("%s: Dele CQ id %d\n", __func__, qid);
	_free_cq(ctrl, qid);
	req->status = NVME_SC_SUCCESS;

	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_delete_sq(struct al_nvme_req *req)
{
	/* QNAP */
	struct al_nvme_ctrl *ctrl = req->ctrl;
	const struct nvme_command *cmd = req->cmd;

	/* TODO implement me */

	/* QNAP */
	u16 qid = le16_to_cpu(cmd->delete_queue.qid);

	if (unlikely(!qid || !ctrl->sq[qid] || qid >=AL_NVME_QUEUES_MAX))
	{
		req->status = NVME_SC_QID_INVALID;
		return NVME_REQ_COMPLETE;
	}

	printk("%s: Dele SQ id %d\n", __func__, qid);
	_free_sq(ctrl, qid);

	req->status = NVME_SC_SUCCESS;

	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_abort(struct al_nvme_req *req)
{
	/* QNAP */
	//req->status = NVME_SC_SUCCESS;
	struct nvme_command *cmd = req->cmd;

	pr_info("%s: abort: cmd_id=%d, sq_id=%d\n", __func__, cmd->abort.cid, cmd->abort.sqid);
	print_debug_list();

	req->status = NVME_SC_SUCCESS;

	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvme_admin_async_event(struct al_nvme_req *req)
{
	struct nvmet_ctrl *ctrl = req->ctrl->target_ctrl;
	struct nvmet_req *target_req = &req->target_req;

	mutex_lock(&ctrl->lock);
	if (ctrl->nr_async_event_cmds >= NVMET_ASYNC_EVENTS) {
		mutex_unlock(&ctrl->lock);
		req->status = NVME_SC_ASYNC_LIMIT | NVME_SC_DNR;
		return NVME_REQ_COMPLETE;
	}

	/* nvmet_req init */
	target_req->cq = &req->cq->nvmet_cq;
	target_req->sq = &req->sq->nvmet_sq;
	target_req->ops = ctrl->ops;
	target_req->rsp->status = 0;
	target_req->cmd = req->cmd;
/*
	if (unlikely(!percpu_ref_tryget_live(&target_req->sq->ref))) {
		req->status = NVME_SC_INVALID_FIELD | NVME_SC_DNR;
		pr_info("%s: sq ref not live\n", __func__);
		return NVME_REQ_COMPLETE;
	}
*/
	ctrl->async_event_cmds[ctrl->nr_async_event_cmds++] = target_req;
	mutex_unlock(&ctrl->lock);

	schedule_work(&ctrl->async_event_work);

	return NVME_REQ_IN_PROGRESS;
}

static u16 nvmet_get_smart_log_nsid(struct al_nvme_req *req,
		struct nvme_smart_log *slog)
{
	u16 status;
	struct nvmet_ns *ns;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	u64 host_reads, host_writes, data_units_read, data_units_written;

	status = NVME_SC_SUCCESS;

    if (!ctrl->target_ctrl->subsys) {
		req->status = NVME_SC_INTERNAL;
		status = NVME_SC_INTERNAL;
		pr_err("nvmet : Could not find subsys\n");
		goto out;
	}

	ns = nvmet_find_namespace(ctrl->target_ctrl, req->cmd->get_log_page.nsid);
	if (!ns) {
		req->status = NVME_SC_INVALID_NS;
		status = NVME_SC_INVALID_NS;
		pr_err("nvmet : Could not find namespace id : %d\n",
		le32_to_cpu(req->cmd->get_log_page.nsid));
		goto out;
	}

	host_reads = part_stat_read(ns->bdev->bd_part, ios[READ]);
	data_units_read = part_stat_read(ns->bdev->bd_part, sectors[READ]);
	host_writes = part_stat_read(ns->bdev->bd_part, ios[WRITE]);
	data_units_written = part_stat_read(ns->bdev->bd_part, sectors[WRITE]);

	put_unaligned_le64(host_reads, &slog->host_reads[0]);
	put_unaligned_le64(data_units_read, &slog->data_units_read[0]);
	put_unaligned_le64(host_writes, &slog->host_writes[0]);
	put_unaligned_le64(data_units_written, &slog->data_units_written[0]);
 
    slog->temperature[0] = ctrl->feat_val.temp_thresh & 0XFF;
    slog->temperature[1] = (ctrl->feat_val.temp_thresh >> 8) & 0XFF; 


	nvmet_put_namespace(ns);
out:
	return status;
}

static u16 nvmet_get_smart_log_all(struct al_nvme_req *req,
		struct nvme_smart_log *slog)
{
	u16 status;
	u64 host_reads = 0, host_writes = 0;
	u64 data_units_read = 0, data_units_written = 0;
	struct nvmet_ns *ns;
	struct nvmet_ctrl *ctrl;

	status = NVME_SC_SUCCESS;
	ctrl = req->ctrl->target_ctrl;
    
    if (!ctrl->subsys) {
        req->status = NVME_SC_INTERNAL;
        status = NVME_SC_INTERNAL;
        pr_err("nvmet : Could not find subsys\n");
        return -1;
    }
	rcu_read_lock();
	list_for_each_entry_rcu(ns, &ctrl->subsys->namespaces, dev_link) {
		host_reads += part_stat_read(ns->bdev->bd_part, ios[READ]);
		data_units_read +=
			part_stat_read(ns->bdev->bd_part, sectors[READ]);
		host_writes += part_stat_read(ns->bdev->bd_part, ios[WRITE]);
		data_units_written +=
			part_stat_read(ns->bdev->bd_part, sectors[WRITE]);

	}
	rcu_read_unlock();

	put_unaligned_le64(host_reads, &slog->host_reads[0]);
	put_unaligned_le64(data_units_read, &slog->data_units_read[0]);
	put_unaligned_le64(host_writes, &slog->host_writes[0]);
	put_unaligned_le64(data_units_written, &slog->data_units_written[0]);

	slog->temperature[0] = req->ctrl->feat_val.temp_thresh & 0XFF;
	slog->temperature[1] = (req->ctrl->feat_val.temp_thresh >> 8) & 0XFF; 

	return status;
}

static u16 nvmet_get_smart_log(struct al_nvme_req *req,
		struct nvme_smart_log *slog)
{
	u16 status;

	WARN_ON(req == NULL || slog == NULL);
	if (req->cmd->get_log_page.nsid == cpu_to_le32(NVME_NSID_ALL)) {
		status = nvmet_get_smart_log_all(req, slog);
	} else {
		status = nvmet_get_smart_log_nsid(req, slog);
	}
	return status;
}

static enum al_req_execution_status
				_nvmet_execute_get_log_page_error(struct al_nvme_req *req)
{
	pr_info("%s: not support\n", __func__);
	req->status = NVME_SC_INVALID_OPCODE;
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvmet_execute_get_log_page_smart(struct al_nvme_req *req)
{
	struct nvme_smart_log *smart_log;
	size_t data_len = nvmet_get_log_page_len(req->cmd);
	dma_addr_t identify_addr;
	u64 prp1 = le64_to_cpu(req->cmd->identify.dptr.prp1);
	u16 status = 0;

	req->log_page_smart.buf = kzalloc(data_len, GFP_KERNEL);
	if (!req->log_page_smart.buf) {
		printk("%s: allocate buffer log_page_smart failed\n", __func__);
		goto out;
	}
	req->log_page_smart.dma_addr = virt_to_phys(req->log_page_smart.buf);

	if (data_len != sizeof(*smart_log)) {
		req->status = NVME_SC_INTERNAL;
		goto err;
	}
	smart_log = req->log_page_smart.buf;
	status = nvmet_get_smart_log(req, smart_log);

	if (status) {
		pr_info("%s: get smart log error\n", __func__);
		memset(req->log_page_smart.buf, '\0', data_len);
		goto err;
	}

	identify_addr = req->log_page_smart.dma_addr;

	if (_send_identify_buffer(req, identify_addr, prp1, data_len)) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto err;
	}

	return NVME_REQ_IN_PROGRESS;
err:
	kfree(req->log_page_smart.buf);
	req->log_page_smart.buf = NULL;
out:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvmet_execute_get_log_page_fw_slot(struct al_nvme_req *req)
{
	u64 prp1 = le64_to_cpu(req->cmd->identify.dptr.prp1);
	struct al_nvme_admin_data *data = req->ctrl->admin_data;
	struct nvme_fw_slot_info_log *fw_slot_log =
								(struct nvme_fw_slot_info_log *) data->log_fw_slot.buf;
	dma_addr_t identify_addr = data->log_fw_slot.dma_addr;

	memset(fw_slot_log, 0, sizeof(struct nvme_fw_slot_info_log));

	fw_slot_log->afi = 0x1;

	if (req->ctrl->id_ctrl.fr) {
		memcpy(&fw_slot_log->frs[0], req->ctrl->id_ctrl.fr, sizeof(fw_slot_log->frs[0]));
	} else {
		memcpy_and_pad(&fw_slot_log->frs[0], sizeof(fw_slot_log->frs[0]),
								UTS_RELEASE, strlen(UTS_RELEASE), ' ');
	}

	if (_send_identify_buffer(req, identify_addr, prp1, sizeof(struct nvme_fw_slot_info_log))) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto out;
	}

	return NVME_REQ_IN_PROGRESS;
out:
	return NVME_REQ_COMPLETE;
}

static enum al_req_execution_status
				_nvmet_execute_get_log_page_cmd_effects(struct al_nvme_req *req)
{
	u64 prp1 = le64_to_cpu(req->cmd->identify.dptr.prp1);
	struct al_nvme_admin_data *data = req->ctrl->admin_data;
	struct nvme_effects_log *cmd_effects_log =
								(struct nvme_effects_log *) data->log_cmd_effects.buf;
	dma_addr_t identify_addr = data->log_cmd_effects.dma_addr;

	memset(cmd_effects_log, 0, sizeof(struct nvme_effects_log));

	cmd_effects_log->acs[nvme_admin_create_cq]      = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_delete_cq]      = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_create_sq]      = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_delete_sq]      = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_get_log_page]   = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_identify]       = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_abort_cmd]      = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_set_features]   = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_get_features]   = cpu_to_le32(1 << 0);
	cmd_effects_log->acs[nvme_admin_async_event]    = cpu_to_le32(1 << 0) |
														cpu_to_le32(1 << 2) |
														cpu_to_le32(1 << 3);

	cmd_effects_log->iocs[nvme_cmd_read]        = cpu_to_le32(1 << 0);
	cmd_effects_log->iocs[nvme_cmd_write]       = cpu_to_le32(1 << 0);
	cmd_effects_log->iocs[nvme_cmd_flush]       = cpu_to_le32(1 << 0);
	cmd_effects_log->iocs[nvme_cmd_dsm]         = cpu_to_le32(1 << 0);

	if (_send_identify_buffer(req, identify_addr, prp1, sizeof(struct nvme_effects_log))) {
		req->status = NVME_SC_DATA_XFER_ERROR;
		goto out;
	}

	return NVME_REQ_IN_PROGRESS;
out:
	return NVME_REQ_COMPLETE;
}

enum al_req_execution_status
			al_nvme_admin_request_execute(struct al_nvme_req *req)
{
	const struct nvme_command *cmd = req->cmd;

	switch (cmd->common.opcode) {
	case nvme_admin_set_features:
		return _nvme_admin_set_features(req);
	case nvme_admin_get_features:
		return _nvme_admin_get_features(req);
	case nvme_admin_create_cq:
		return _nvme_admin_create_cq(req);
	case nvme_admin_delete_cq:
		return _nvme_admin_delete_cq(req);
	case nvme_admin_create_sq:
		return _nvme_admin_create_sq(req);
	case nvme_admin_delete_sq:
		return _nvme_admin_delete_sq(req);
	case nvme_admin_abort_cmd:
		return _nvme_admin_abort(req);
	case nvme_admin_identify:
		switch (cmd->identify.cns) {
		case NVME_ID_CNS_NS:
			return _nvme_admin_identify_ns(req);
		case NVME_ID_CNS_CTRL:
			return _nvme_admin_identify_ctrl(req);
		case NVME_ID_CNS_NS_ACTIVE_LIST:
			return _nvme_admin_identify_nslist(req);
		case NVME_ID_CNS_NS_DESC_LIST:
			return _nvme_admin_identify_desclist(req);
	}
	case nvme_admin_async_event:
		return _nvme_admin_async_event(req);
	case nvme_admin_get_log_page:
/*
        pr_info("%s: Get Log Page: cmd_op=%d, cmd_id=0x%x, "
        "nsid=0x%x, lid=%d, numdl=0x%x, numdu=0x%x, "
        "lpol=0x%x, lpou=0x%x\n", 
        __func__, cmd->get_log_page.opcode, cmd->get_log_page.command_id, 
        cmd->get_log_page.nsid, cmd->get_log_page.lid, cmd->get_log_page.numdl,
        cmd->get_log_page.numdu, cmd->get_log_page.lpol, cmd->get_log_page.lpou);
*/
		switch (cmd->get_log_page.lid) {
		case NVME_LOG_ERROR:
			return _nvmet_execute_get_log_page_error(req);
		case NVME_LOG_SMART:
			return _nvmet_execute_get_log_page_smart(req);
		case NVME_LOG_FW_SLOT:
			return _nvmet_execute_get_log_page_fw_slot(req);
		case NVME_LOG_CMD_EFFECTS:
			return _nvmet_execute_get_log_page_cmd_effects(req);
		}
		pr_info("%s: Log Page Identifiers %d\n", __func__, cmd->get_log_page.lid);
		break;
	default:
		break;
	}

	pr_err("%s: unknown opcode (%d)\n", __func__, cmd->common.opcode);
	req->status = NVME_SC_INVALID_OPCODE;
	return NVME_REQ_COMPLETE;
}

/* Namespace data should be READY by the time this function is called! */
int al_nvme_admin_init(struct al_nvme_ctrl *ctrl)
{
	struct al_nvme_admin_data *data;

	data = al_nvme_zalloc(sizeof(*data));
	if (!data)
		goto fail;

	/* Allocate NVMe identify controller buffer */
	data->ctrl_ident_data.buf = kzalloc(IDENTIFY_BUFFER_SIZE, GFP_KERNEL);
	if (!data->ctrl_ident_data.buf) {
		printk("%s: allocate ctrl_ident_data buffer failed\n", __func__);
		goto fail_buf;
	}
	data->ctrl_ident_data.dma_addr = virt_to_phys(data->ctrl_ident_data.buf);

	/* Allocate NVMe identify namespace buffer */
	data->ns_ident_data.buf = kzalloc(IDENTIFY_BUFFER_SIZE, GFP_KERNEL);
	if (!data->ns_ident_data.buf) {
		printk("%s: allocate ns_ident_data buffer failed\n", __func__);
		goto fail_buf;
	}
	data->ns_ident_data.dma_addr = virt_to_phys(data->ns_ident_data.buf);

	/* Allocate NVMe identify namespace list buffer */
	data->nslist_ident_data.buf = kzalloc(IDENTIFY_BUFFER_SIZE, GFP_KERNEL);
	if (!data->nslist_ident_data.buf) {
		printk("%s: allocate nslist_ident_data buffer failed\n", __func__);
		goto fail_buf;
	}
	data->nslist_ident_data.dma_addr = virt_to_phys(data->nslist_ident_data.buf);

	/* Allocate NVMe identify descriptor buffer */
	data->ns_id_desc_ident_data.buf = kzalloc(IDENTIFY_BUFFER_SIZE, GFP_KERNEL);
	if (!data->ns_id_desc_ident_data.buf) {
		printk("%s: allocate ns_id_desc_ident_data buffer failed\n", __func__);
		goto fail_buf;
	}
	data->ns_id_desc_ident_data.dma_addr = virt_to_phys(data->ns_id_desc_ident_data.buf);

	/* Allocate NVMe firmware slot log page buffer */
	data->log_fw_slot.buf = kzalloc(sizeof(struct nvme_fw_slot_info_log), GFP_KERNEL);
	if (!data->log_fw_slot.buf) {
		printk("%s: allocate buffer log_fw_slot failed\n", __func__);
		goto fail_buf;
	}
	data->log_fw_slot.dma_addr = virt_to_phys(data->log_fw_slot.buf);

	/* Allocate NVMe commands supported and effects log page buffer */
	data->log_cmd_effects.buf = kzalloc(sizeof(struct nvme_effects_log), GFP_KERNEL);
	if (!data->log_cmd_effects.buf) {
		printk("%s: allocate buffer log_cmd_effects failed\n", __func__);
		goto fail_buf;
	}
	data->log_cmd_effects.dma_addr = virt_to_phys(data->log_cmd_effects.buf);

	ctrl->admin_data = data;

	return 0;
fail_buf:
	al_nvme_admin_free(ctrl);
fail:
	return -ENOMEM;
}

void al_nvme_admin_free(struct al_nvme_ctrl *ctrl)
{
	struct al_nvme_admin_data *data = ctrl->admin_data;

	if (data) {
		if (data->ctrl_ident_data.buf) {
			kfree(data->ctrl_ident_data.buf);
			data->ctrl_ident_data.buf = NULL;
		}
		if (data->ns_ident_data.buf) {
			kfree(data->ns_ident_data.buf);
			data->ns_ident_data.buf = NULL;
		}
		if (data->nslist_ident_data.buf) {
			kfree(data->nslist_ident_data.buf);
			data->nslist_ident_data.buf = NULL;
		}
		if (data->ns_id_desc_ident_data.buf) {
			kfree(data->ns_id_desc_ident_data.buf);
			data->ns_id_desc_ident_data.buf = NULL;
        }
		if (data->log_fw_slot.buf) {
			kfree(data->log_fw_slot.buf);
			data->log_fw_slot.buf = NULL;
        }
		if (data->log_cmd_effects.buf) {
			kfree(data->log_cmd_effects.buf);
			data->log_cmd_effects.buf = NULL;
        }
		al_nvme_free(data);
		data = NULL;
	}
}

