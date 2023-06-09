/*
 * drivers/misc/al/nvme_bridge/al_nvme_subsystem.c
 *
 * Annapurna Labs NVMe Bridge Reference driver, NVMe subsystem layer
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

#include <linux/module.h>
#include <linux/idr.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/unistd.h>
#include <linux/kthread.h>
#include <asm/cacheflush.h>
#include <linux/pci_regs.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/nvme.h>
#include <linux/err.h>
#include <linux/posix-clock.h>
#include <linux/proc_fs.h>
#include <linux/async_tx.h>

#include <soc/alpine/al_hal_pcie.h>
#include <soc/alpine/al_hal_unit_adapter_regs.h>
#include <soc/alpine/al_hal_plat_services.h>
#include "qnap_nvmet_pcie.h"
#include "qnap_nvmet_pcie_api.h"
#include "qnap_nvmet_pcie_task_manager.h"
#include "qnap_nvmet_pcie_dma_api.h"
#include "qnap_nvmet_pcie_dma.h"

#include "../../pci/endpoint/alpine/qnap_pcie_ep.h"
#include "../../pci/endpoint/alpine/qnap_pcie_ep_cpld.h"
#include "nvmet.h"

static struct nvmet_fabrics_ops nvmet_pcie_ops;
static struct al_nvme_ctrl **nvme_ctrl;
/************************************************************************/
struct debug_list {
	unsigned int task_id;
	int cmd_cnt;
	struct timespec s_dma;
	struct timespec e_dma;
	struct nvmet_req *req;
	struct al_nvme_req *al_req;
	struct list_head list;
};
struct debug_list *debug_1 = {0}, *debug_2 = {0};
/************************************************************************/
static struct proc_dir_entry *proc_qnap_nvme_ep_root = NULL;
static uint g_mode = 0;
static bool g_rdy = 0;
int debug_flag = 0;
int debugDB_flag = 0;
int debug_sleep_cnt = 0;
int debug_schedule_cnt = 0;
bool ep0_error_handling = false;
bool ep1_error_handling = false;

int nvmet_ctrl_ready(void) {
	int i, ep_port_counts = 0;
	struct al_nvme_ctrl *ctrl;
	struct NvmeBar *ctrl_regs;

	ep_port_counts = qnap_ep_get_port_count_fun();

	if (ep_port_counts <= 0) {
		printk("NVME EP port counts : %d\n", ep_port_counts);
		return -1;
	}
	g_rdy = 1;
	for (i = 0;i < ep_port_counts;i++) {
		ctrl = nvme_ctrl[i];
		ctrl_regs = (struct NvmeBar *) ctrl->pcie_data->inbound_base_addr_bar0;
		if (QNAP_NVME_CC_EN(ctrl_regs->cc)) {
			ctrl_regs->csts |= QNAP_NVME_CSTS_READY;
			ctrl->target_ctrl->csts = ctrl_regs->csts;
			printk("%s: NVMe Controller %d Started!\n", __func__, i);
		} else {
			printk("%s: NVMe Controller %d Host NOT Enable\n", __func__, i);
		}
	}
	return 0;
}

void setting_nvme_mode(uint cmd)
{
	switch(cmd) {
	case 0:
		ep0_error_handling = true;
		break;
	case 1:
		ep1_error_handling = true;
		break;
	case 2:
		printk("hello word !!!\n");
		break;
	case 3:

		break;
	case 4:
		nvmet_ctrl_ready();
		break;
	case 5:
		debug_flag = 1;
		break;
	case 6:
		debug_flag = 0;
		break;
	case 7:
		break;
	case 8:
		debugDB_flag = 1;
		break;
	case 9:
		debugDB_flag = 0;
		break;
	default:
		printk("QNAP PCIE EP Control function error\n");
		break;
	}

	return;
};

static ssize_t qnap_nvme_ep_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;
	struct debug_list *debug_tmp;

	if(list_empty(&debug_1->list)) {
		printk("bio debug list is empty\n");
	} else {
		printk("bio debug list\n");
		list_for_each_entry(debug_tmp, &debug_1->list, list) {
			printk("cmd_op=%d\n", debug_tmp->al_req->cmd->rw.opcode);
			printk("cmd_id=%d\n", debug_tmp->al_req->cmd->rw.command_id);
			printk("sq_id=%d\n", debug_tmp->al_req->sq_id);
			printk("prp1=0x%llx\n", debug_tmp->al_req->cmd->rw.dptr.prp1);
		}
	}

	return len;
};

static ssize_t qnap_nvme_ep_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	unsigned char value[100];
	unsigned int tmp;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	sscanf(value,"%u\n", &tmp);
	setting_nvme_mode(tmp);
	g_mode = tmp;

	return count;
};

static const struct file_operations qnap_nvme_ep_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= qnap_nvme_ep_read_proc,
	.write			= qnap_nvme_ep_write_proc
};

static int qnap_nvme_rdy_cmd_line(struct seq_file *m, void *v)
{
        seq_printf(m, "%d\n", g_rdy);
        return 0;
};

static int qnap_nvme_ep_rdy_open_proc(struct inode *inode, struct file *file)
{
	return single_open(file, qnap_nvme_rdy_cmd_line, NULL);
};

static const struct file_operations qnap_nvme_ep_rdy_proc_fileops = {
	.owner			= THIS_MODULE,
	.open			= qnap_nvme_ep_rdy_open_proc,
	.read			= seq_read,
	.llseek			= seq_lseek,
	.release		= seq_release,
};

int print_debug_list(void) {
	int len = 0;
	struct debug_list *debug_tmp;

	if(list_empty(&debug_1->list)) {
		printk("BIO debug list is empty\n");
		printk("NO command lose from BIO\n");
	} else {
		printk("BIO debug list: \n");
		list_for_each_entry(debug_tmp, &debug_1->list, list) {
			printk("cmd_op=%d\n", debug_tmp->al_req->cmd->rw.opcode);
			printk("cmd_id=%d\n", debug_tmp->al_req->cmd->rw.command_id);
			printk("sq_id=%d, cq_id=%d\n", debug_tmp->al_req->sq_id, debug_tmp->al_req->cq_id);
			printk("status=%d\n", debug_tmp->al_req->status);
			printk("time=%lu\n", debug_tmp->s_dma.tv_sec);
		}
	}
	return len;
}

/************************************************************************/
static void _async_memcpy_callback(void *param)
{
	struct al_nvme_req *req = param;
	struct nvme_command *cmd = req->cmd;
    struct al_nvme_ctrl *ctrl = req->ctrl;
	unsigned long flags;
	struct al_nvme_tm_task *_task = req->task;
	struct al_nvme_tm_context *ctx = NULL;
	__u16 id = cmd->rw.command_id;
	__u16 qid = req->sq->sq_id;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (_task->task_id == ctrl->bridge_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->bridge_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	spin_lock_irqsave(&ctx->comp_queue_lock, flags);
	if (ctrl->cmd_prp_cnt[qid-1][id] > 0) {
		ctrl->cmd_prp_cnt[qid-1][id]--;
	}

	if (ctrl->cmd_prp_cnt[qid-1][id] == 0) {
		/* send to complete queue */
		al_nvme_tm_queue_push(req->task, 0, req);
	}
	spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
}

static void _async_xor_callback(void *context)
{
	struct al_nvme_req *req = context;
    struct al_nvme_ctrl *ctrl = req->ctrl;
	unsigned long flags;
	struct al_nvme_tm_task *_task = req->task;
	struct al_nvme_tm_context *ctx = NULL;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (_task->task_id == ctrl->bridge_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->bridge_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	if (req->src_sgl) {
		kfree(req->src_sgl);
		req->src_sgl = NULL;
	}

	/* send to complete queue */
	spin_lock_irqsave(&ctx->comp_queue_lock, flags);
	al_nvme_tm_queue_push(req->task, 0, req);
	spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
}

static int nvmet_async_memcpy_sg(struct al_nvme_req *req, uint64_t length)
{
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	uint32_t k;
	uint64_t data_len = 0, page_len = 0;
	uint64_t prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	uint64_t prp2 = le64_to_cpu(cmd->rw.dptr.prp2);
	unsigned int nents = req->sg_nents;
	uint64_t *prps = req->prp_list.buf;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;
	struct dma_async_tx_descriptor *tx;
	int ret = 0;
	struct scatterlist *dst_sgl = ctrl->sg;

	data_len = min(PRP_LENGTH(prp1, ctrl->params.page_size), length);
	length -= data_len;

	if (!dst_sgl) {
		printk("%s: dst sgl is null pointer\n", __func__);
		return -ENOMEM;
	}

	req->src_sgl = kzalloc(sizeof(struct scatterlist) * nents, GFP_KERNEL);
	if (!req->src_sgl) {
		printk("%s: async_memcpy_sg failed\n", __func__);
		return -ENOMEM;
	}

	sg_init_table(req->src_sgl, nents);

	sg_dma_address(&req->src_sgl[0]) = (dma_addr_t) (prp1 + host_mem_base);
	sg_dma_len(&req->src_sgl[0]) = data_len;

	if (nents > 1) {
		if (nents == 2) {
			sg_dma_address(&req->src_sgl[1]) = (dma_addr_t) (prp2 + host_mem_base);
			sg_dma_len(&req->src_sgl[1]) = length;
		} else {
			uint16_t i = 0;
			if (!prps) {
				printk("%s: prp list is null\n", __func__);
				req->status = NVME_SC_INTERNAL;
				return -1;
			}
			while (length != 0) {
				page_len = min(PRP_LENGTH(prps[i], ctrl->params.page_size), length);
				sg_dma_address(&req->src_sgl[i+1]) = (dma_addr_t) (prps[i] + host_mem_base);
				sg_dma_len(&req->src_sgl[i+1]) = page_len;
				length -= page_len;
				i++;
			}
		}
	}

	if (cmd->rw.opcode == nvme_cmd_write)
		ret = async_memcpy_sg(req->src_sgl, nents, dst_sgl, nents, _async_xor_callback, req);
	else if (cmd->rw.opcode == nvme_cmd_read)
		ret = async_memcpy_sg(dst_sgl, nents, req->src_sgl, nents, _async_xor_callback, req);

	if (ret) {
		printk("%s: async_memcpy_sg failed\n", __func__);
		_async_xor_callback(req);
	}

	return 0;
}

static int nvmet_async_xor_write(struct al_nvme_req *req, uint64_t length)
{
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	uint32_t k;
	uint64_t data_len = 0;
	uint64_t prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	uint64_t prp2 = le64_to_cpu(cmd->rw.dptr.prp2);
	unsigned int nents = req->sg_nents;
	uint64_t prps_all[512] = { 0 };
	uint64_t *prps = req->prp_list.buf;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;

	struct dma_async_tx_descriptor *tx;
	struct async_submit_ctl submit_xor;
	struct async_submit_ctl submit;
	enum async_tx_flags flags = ASYNC_TX_XOR_ZERO_DST | ASYNC_TX_ACK;

	data_len = min(PRP_LENGTH(prp1, ctrl->params.page_size), length);
	length -= data_len;

	init_async_submit(&submit_xor,
					  flags,
					  NULL,
		              _async_xor_callback,
					  req,
					  NULL);

	init_async_submit(&submit,
					  flags,
					  NULL,
		              _async_memcpy_callback,
					  req,
					  NULL);
	if (nents == 1) {
		async_memcpy_phys(virt_to_phys(ctrl->data_buffer[0]),
									prp1 + host_mem_base,
									0, 0,
									data_len,
									&submit);
	} else if (nents == 2) {
		async_memcpy_phys(virt_to_phys(ctrl->data_buffer[0]),
						prp1 + host_mem_base,
						0, 0,
						data_len,
						&submit);
		async_memcpy_phys(virt_to_phys(ctrl->data_buffer[0]),
						prp2 + host_mem_base,
						0, 0,
						length,
						&submit);
	} else {
		for (k = 0;k < nents;k++) {
			if (k == 0)
				prps_all[k] = prp1 + host_mem_base;
			else
				prps_all[k] = prps[k-1] + host_mem_base;
		}
		tx = async_xor_phys(virt_to_phys(ctrl->data_buffer[0]),
							prps_all,
							0, nents,
							4096,
							&submit_xor);
	}

	return 0;
}

static int nvmet_async_memcpy_read(struct al_nvme_req *req, uint64_t length)
{
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	uint64_t data_len = 0, page_len = 0;
	uint64_t prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	uint64_t prp2 = le64_to_cpu(cmd->rw.dptr.prp2);
	unsigned int nents = req->sg_nents;
	uint64_t *prps = req->prp_list.buf;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;
	struct async_submit_ctl submit;
	enum async_tx_flags flags = ASYNC_TX_XOR_ZERO_DST | ASYNC_TX_ACK;

	data_len = min(PRP_LENGTH(prp1, ctrl->params.page_size), length);
	length -= data_len;

	init_async_submit(&submit,
					  flags,
					  NULL,
		              _async_memcpy_callback,
					  req,
					  NULL);

	async_memcpy_phys(prp1 + host_mem_base,
					virt_to_phys(ctrl->data_buffer[0]),
					0, 0,
					data_len,
					&submit);
	if (nents > 1) {
		if (nents == 2) {
			async_memcpy_phys(prp2 + host_mem_base,
						virt_to_phys(ctrl->data_buffer[0]),
						0, 0,
						length,
						&submit);
		} else {
			uint16_t i = 0;
			if (!prps) {
				printk("%s: prp list is null\n", __func__);
				req->status = NVME_SC_INTERNAL;
				return -1;
			}
			while (length != 0) {
				page_len = min(PRP_LENGTH(prps[i], ctrl->params.page_size), length);
				async_memcpy_phys(prps[i] + host_mem_base,
							virt_to_phys(ctrl->data_buffer[0]),
							0, 0,
							page_len,
							&submit);
				length -= page_len;
				i++;
			}
		}
	}
	return 0;
}

static int nvmet_get_dsm_data(struct al_nvme_req *req, struct scatterlist **sgl, unsigned int *nent)
{
	struct nvme_command *cmd = req->cmd;
	uint64_t data_len = 0;
	uint64_t prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	struct scatterlist *sg = NULL;
	unsigned int nents = 1;
	phys_addr_t host_mem_base = (phys_addr_t) req->ctrl->pcie_data->outbound_base_addr;

	data_len = NVME_DSM_MAX_RANGES * sizeof(struct nvme_dsm_range);

	sg = kmalloc(sizeof(struct scatterlist), GFP_KERNEL);
	if (!sg) {
		req->status = NVME_SC_INTERNAL;
		return -1;
	}

	sg_init_table(sg, nents);

	req->pages = alloc_page(GFP_KERNEL);
	if (!req->pages) {
		printk("%s: pages is null pointer!\n", __func__);
		req->status = NVME_SC_BAD_ATTRIBUTES;
		goto out;
	}
	req->vbase1[0] = ioremap_nocache(prp1 + host_mem_base, data_len);

	if (!req->vbase1[0]) {
		printk("%s: 1 ioremap failed\n", __func__);
		req->status = NVME_SC_INTERNAL;
		goto free_pages;
	}

	memcpy(page_address(req->pages), req->vbase1[0], data_len);
	sg_set_page(sg, req->pages, data_len, 0);

	*sgl = sg;
	*nent = nents;

	return 0;

free_pages:
	__free_page(req->pages);
	kfree(req->pages);
out:
	return -1;
}

static int nvmet_pcie_alloc_sgl(struct al_nvme_req *req, struct scatterlist **sgl,
                                    unsigned int *nent, uint64_t length)
{
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_ctrl *ctrl = req->ctrl;

	uint64_t data_len = 0, page_len = 0;
	uint64_t prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	uint64_t prp2 = le64_to_cpu(cmd->rw.dptr.prp2);
	struct scatterlist *sg;
	unsigned int nents = req->sg_nents;
	uint64_t *prps = req->prp_list.buf;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;

	data_len = min(PRP_LENGTH(prp1, ctrl->params.page_size), length);

	length -= data_len;

	sg = kmalloc_array(nents, sizeof(struct scatterlist), GFP_KERNEL); 
	if (!sg) {
		printk("%s: sg is null pointer!\n", __func__);
		req->status = NVME_SC_INTERNAL;
		return -1;
	}

	sg_init_table(sg, nents);

#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
    req->vbase1[0] = prp1 + host_mem_base;
#else
	req->vbase1[0] = ioremap_nocache(prp1 + host_mem_base, data_len);
#endif

	if (!req->vbase1[0]) {
		printk("%s: 1 ioremap failed\n", __func__);
		req->status = NVME_SC_INTERNAL;
		return -1;
	}
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
	sg_set_page(&sg[0],
				req->vbase1[0],
				data_len,
				0);
#else
	sg_set_page(&sg[0],
				vmalloc_to_page(req->vbase1[0]),
				data_len,
				offset_in_page(req->vbase1[0]));
#endif

	if (nents > 1) {
		if (nents == 2) {
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
            req->vbase1[1] = prp2 + host_mem_base;
#else
			req->vbase1[1] = ioremap_nocache(prp2 + host_mem_base, length);
#endif
			if (!req->vbase1[1]) {
				printk("%s: 2 ioremap failed\n", __func__);

				req->status = NVME_SC_INTERNAL;
				return -1;
			}
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
			sg_set_page(&sg[1],
						req->vbase1[1],
						length,
						0);
#else
			sg_set_page(&sg[1],
						vmalloc_to_page(req->vbase1[1]),
						length,
						offset_in_page(req->vbase1[1]));
#endif
		} else {
			uint16_t i = 0;
			if (!prps) {
				printk("%s: prp list is null\n", __func__);
				req->status = NVME_SC_INTERNAL;
				return -1;
			}

			while (length != 0) {
				page_len = min(PRP_LENGTH(prps[i], ctrl->params.page_size), length);
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
                req->vbase1[i+1] = prps[i] + host_mem_base;
#else
				req->vbase1[i+1] = ioremap_nocache(prps[i] + host_mem_base, page_len);
#endif
				if (!req->vbase1[i+1]) {
					printk("%s: 3 ioremap failed\n", __func__);

					req->status = NVME_SC_INTERNAL;
					return -1;
				}
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
				sg_set_page(&sg[i+1],
							req->vbase1[i+1],
							page_len,
							0);
#else
				sg_set_page(&sg[i+1],
							vmalloc_to_page(req->vbase1[i+1]),
							page_len,
							offset_in_page(req->vbase1[i+1]));
#endif
				length -= page_len;
				i++;
			}
		}
	}
	*sgl = sg;
	*nent = nents;

	return 0;
}

static u16 nvmet_pcie_map_sgl(struct al_nvme_req *req)
{
	struct nvme_command *cmd = req->cmd;
	uint64_t data_size;
	uint64_t slba = le64_to_cpu(cmd->rw.slba);
	uint32_t nlb  = le32_to_cpu(cmd->rw.length);
	uint16_t status;
	struct nvmet_ns *ns = req->target_req.ns;

	data_size = (nlb + 1) << ns->blksize_shift;

	if ((slba + nlb) >= (ns->size >> ns->blksize_shift)) {
		printk("%s: LBA out of range", __func__);

		req->status = NVME_SC_LBA_RANGE;
		return -1;
	}

	status = nvmet_pcie_alloc_sgl(req,
				      &req->target_req.sg,
				      &req->target_req.sg_cnt,
				      data_size);

	if (status) {
		printk("%s: without DMA alloc sgl failed\n", __func__);
		return status;
	}

	return 0;
}

static u16 nvmet_pcie_data_to_ram(struct al_nvme_req *req)
{
	struct nvmet_ns *ns = req->target_req.ns;
	struct nvme_command *cmd = req->cmd;
	uint64_t data_size;
	uint64_t slba = le64_to_cpu(cmd->rw.slba);
	uint32_t nlb  = le32_to_cpu(cmd->rw.length);
	uint16_t status;

	data_size = (nlb + 1) << ns->blksize_shift;

	if ((slba + nlb) >= (ns->size >> ns->blksize_shift)) {
		printk("%s: LBA out of range", __func__);

		req->status = NVME_SC_LBA_RANGE;
		return -1;
	}

	/* issue async memcpy sg */
	status = nvmet_async_memcpy_sg(req, data_size);
	if (status) {
		printk("%s: Async memcpy SG failed\n", __func__);
		return status;
	}
#if 0 // do async_xor for RAM test
	if (cmd->rw.opcode == nvme_cmd_write) {
		/* issue async XOR */
		status = nvmet_async_xor_write(req, data_size);
		if (status) {
			printk("%s: Async xor write failed\n", __func__);
			return status;
		}
	} else if (cmd->rw.opcode == nvme_cmd_read) {
		/* issue async memcpy */
		status = nvmet_async_memcpy_read(req, data_size);
		if (status) {
			printk("%s: Async memcpy read failed\n", __func__);
			return status;
		}
	}
#endif
	return 0;
}

static void nvmet_pcie_queue_response(struct nvmet_req *req)
{
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
	struct al_nvme_req *al_req = ((req->sq->qid != 0) &&
				      (req->cmd->common.opcode == nvme_cmd_read ||
				       req->cmd->common.opcode == nvme_cmd_write) ) ?
		container_of(req, struct al_nvme_req, rwbuf_treq) :
		container_of(req, struct al_nvme_req, target_req);

#else
	struct al_nvme_req *al_req =
		container_of(req, struct al_nvme_req, target_req);
#endif
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_tm_task *_task = al_req->task;
	struct al_nvme_ctrl *ctrl = al_req->ctrl;
	struct al_nvme_tm_context *ctx = NULL;
	unsigned long flags;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;
	bool qnap_error_handling = false;

	if (req->sq->qid != 0) {
		if (_task->task_id == ctrl->bridge_task_id[first_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[first_cpu];
		} else if (_task->task_id == ctrl->bridge_task_id[last_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[last_cpu];
		}
	} else {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	}

	if (debug_flag) {
		printk("%s: I/O cmd_op=%d, cmd_id=%d, sq_id=%d, "
				"nlba=%d, slba=0x%llx, prp1=0x%llx, prp2=0x%llx, "
				"control=0x%x, dsmgmt=0x%x, status=%s\n", __func__, cmd->common.opcode,
				cmd->common.command_id, req->sq->qid, cmd->rw.length,
				cmd->rw.slba, cmd->rw.dptr.prp1, cmd->rw.dptr.prp2,
				cmd->rw.control, cmd->rw.dsmgmt, req->rsp->status);
	}

	if (ctrl->ctrl_id == 0) {
		qnap_error_handling = ep0_error_handling;
	}
	if (ctrl->ctrl_id == 1) {
		qnap_error_handling = ep1_error_handling;
	}

	if (qnap_error_handling == false) {
		al_req->status = req->rsp->status;
		al_req->result = req->rsp->result.u32;
	} else {
		al_req->status = NVME_SC_DATA_XFER_ERROR;
		printk("%s: QNAP EH mode: st = 0x%x\n", __func__, al_req->status);
	}

	if (cmd->rw.opcode == nvme_cmd_dsm) {
		if (al_req->pages) {
			__free_page(al_req->pages);
			al_req->pages = NULL;
		}
	}

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF

	if (!qnap_error_handling  && req->cmd->rw.opcode == nvme_cmd_read) {
		spin_lock_irqsave(&ctx->cpy_queue_lock, flags);
		al_nvme_tm_queue_push(al_req->task, 1, al_req);
		spin_unlock_irqrestore(&ctx->cpy_queue_lock, flags);

	} else if (!qnap_error_handling && cmd->rw.opcode == nvme_cmd_write) {
		spin_lock_irqsave(&ctx->comp_queue_lock, flags);
		al_nvme_tm_queue_push(al_req->task, 0, al_req);
		spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
	} else {
		// for admin cmd, such as async cmd
		spin_lock_irqsave(&ctx->comp_queue_lock, flags);
		al_nvme_tm_queue_push(al_req->task, al_req->task_queue, al_req);
		spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
	}
#else

	spin_lock_irqsave(&ctx->comp_queue_lock, flags);
	al_nvme_tm_queue_push(al_req->task, al_req->task_queue, al_req);
	spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
#endif

}

static uint8_t nvme_cq_full(struct al_nvme_cq *cq)
{
	return (cq->tail + 1) % cq->queue_size == cq->head;
}

static inline struct al_shared_buf *get_shared_buf(struct al_bufs_lifo *lifo,
						   unsigned int count)
{
	BUG_ON(unlikely(lifo->size == lifo->head));

	lifo->bufs[lifo->head]->count += count;

	return lifo->bufs[lifo->head++];
}

static inline void put_shared_buf(struct al_bufs_lifo *lifo,
				  struct al_shared_buf *buf)
{
	if (--buf->count == 0)
		lifo->bufs[--lifo->head] = buf;
}

static inline void _nvme_ctrl_unmap(void *mapped)
{
	iounmap(mapped);
}

void al_nvme_bufs_lifo_free(struct al_bufs_lifo *lifo)
{
	if (!lifo)
		return;

	al_nvme_free(lifo->bufs);
	lifo->bufs = NULL;
}

static inline unsigned int get_avail_reqs(struct al_reqs_lifo *lifo,
					  struct al_nvme_req **reqs,
					  unsigned int num)
{
	unsigned int i;
	num = min_t(unsigned int, num, lifo->size - lifo->head);

	for (i = 0; i < num; ++i)
		reqs[i] = lifo->reqs[lifo->head + i];

	lifo->head += num;

	return num;
}

static inline void put_avail_req(struct al_reqs_lifo *lifo,
				 struct al_nvme_req *req)
{
	lifo->reqs[--lifo->head] = req;
}

static int al_nvme_reqs_lifo_init(struct al_reqs_lifo *lifo, unsigned int size)
{
	if (!lifo)
		return -EINVAL;
	lifo->head = 0;
	lifo->size = size;
	lifo->reqs = al_nvme_calloc(size, sizeof(*lifo->reqs));
	if (!lifo->reqs)
		return -ENOMEM;

	return 0;
}

static int al_nvme_bufs_lifo_init(struct al_bufs_lifo *lifo, unsigned int size)
{
	if (!lifo)
		return -EINVAL;
	lifo->head = 0;
	lifo->size = size;
	lifo->bufs = al_nvme_calloc(size, sizeof(*lifo->bufs));
	if (!lifo->bufs)
		return -ENOMEM;

	return 0;
}

void al_nvme_reqs_lifo_free(struct al_reqs_lifo *lifo)
{
	if (!lifo)
		return;

	al_nvme_free(lifo->reqs);
	lifo->reqs = NULL;
}

int al_nvme_reqs_ring_init(struct al_reqs_ring *ring, unsigned int size)
{
	if (!ring)
		return -EINVAL;
	ring->head = 0;
	ring->tail = 0;
	ring->size = roundup_pow_of_two(size);
	ring->size_mask = ring->size - 1;
	ring->reqs = al_nvme_calloc(size, sizeof(*ring->reqs));
	if (!ring->reqs)
		return -ENOMEM;

	return 0;
}

void al_nvme_reqs_ring_free(struct al_reqs_ring *ring)
{
	if (!ring)
		return;

	al_nvme_free(ring->reqs);
	ring->reqs = NULL;
}

void _free_queues(struct al_nvme_ctrl *ctrl)
{
	al_nvme_free(ctrl->sq);
	ctrl->sq = NULL;

	al_nvme_free(ctrl->cq);
	ctrl->cq = NULL;

	kfree(ctrl->target_ctrl->sqs);
	ctrl->target_ctrl->sqs = NULL;
	kfree(ctrl->target_ctrl->cqs);
	ctrl->target_ctrl->cqs = NULL;
}

static void pcie_async_events_free(struct nvmet_ctrl *ctrl)
{
	struct nvmet_req *req;

	while (1) {
		mutex_lock(&ctrl->lock);
		if (!ctrl->nr_async_event_cmds) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		mutex_unlock(&ctrl->lock);
		nvmet_req_complete(req, NVME_SC_INTERNAL);
	}
}

static void pcie_confirm_sq(struct percpu_ref *ref)
{
	struct al_nvme_sq *sq = container_of(ref, struct al_nvme_sq, ref);

	complete(&sq->confirm_done);
}

void _free_sq(struct al_nvme_ctrl *ctrl, int idx)
{
	struct al_nvme_sq *sq = ctrl->sq[idx];
	unsigned int i;
	struct al_nvme_req *req;

	if (!sq)
		return;

	/* TODO we should fail here if CIRC_CNT is not 0 (meaning
	 * some commands that belong to this sq are still executing)
	 */
	if (idx == 0) {
		pcie_async_events_free(sq->ctrl->target_ctrl);
	}

	percpu_ref_kill_and_confirm(&sq->ref, pcie_confirm_sq);
	wait_for_completion(&sq->confirm_done);
	wait_for_completion(&sq->free_done);
	percpu_ref_exit(&sq->ref);

	if (sq->avail_reqs.reqs) {
		for (i = 0; i < sq->avail_reqs.size; ++i) {
			req = sq->avail_reqs.reqs[i];
			if (req->prp_list.buf) {
				al_nvme_free(req->prp_list.buf);
				req->prp_list.buf = NULL;
			}
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
			if (req->rw_buf) {
				kfree(req->rw_buf);
				req->rw_buf = NULL;
			}
#endif
			if (sq->avail_reqs.reqs[i]->target_req.rsp) {
				al_nvme_free(sq->avail_reqs.reqs[i]->target_req.rsp);
				sq->avail_reqs.reqs[i]->target_req.rsp = NULL;
			}
			if (sq->avail_reqs.reqs[i]) {
				al_nvme_cache_free(ctrl->reqs_cache, sq->avail_reqs.reqs[i]);
				sq->avail_reqs.reqs[i] = NULL;
			}
		}
		al_nvme_reqs_lifo_free(&sq->avail_reqs);
	}

	if (sq->avail_cmds.bufs) {
		for (i = 0; i < sq->avail_cmds.size; ++i) {
			if (sq->avail_cmds.bufs[i]->buf) {
				al_nvme_free(sq->avail_cmds.bufs[i]->buf);
				sq->avail_cmds.bufs[i]->buf = NULL;
			}
			if (sq->avail_cmds.bufs[i]) {
				al_nvme_free(sq->avail_cmds.bufs[i]);
				sq->avail_cmds.bufs[i] = NULL;
			}
		}
		al_nvme_bufs_lifo_free(&sq->avail_cmds);
	}

	al_nvme_free(sq);
	ctrl->sq[idx] = NULL;
}

void _free_cq(struct al_nvme_ctrl *ctrl, int idx)
{
	struct al_nvme_cq *cq = ctrl->cq[idx];
	unsigned int i;

	if (!cq)
		return;

	if (cq->avail_comps.bufs) {
		for (i = 0; i < cq->avail_comps.size; ++i) {
			if (cq->avail_comps.bufs[i]->buf) {
				al_nvme_free(cq->avail_comps.bufs[i]->buf);
				cq->avail_comps.bufs[i]->buf = NULL;
			}
			if (cq->avail_comps.bufs[i]) {
				al_nvme_free(cq->avail_comps.bufs[i]);
				cq->avail_comps.bufs[i] = NULL;
			}
		}
		al_nvme_bufs_lifo_free(&cq->avail_comps);
	}

	_nvme_ctrl_unmap(cq->mapped_host_addr);
	if (cq->mapped_msix_host_addr)
		_nvme_ctrl_unmap(cq->mapped_msix_host_addr);
	al_nvme_free(cq);
	ctrl->cq[idx] = NULL;
}

void nvme_ctrl_free(struct al_nvme_ctrl **ctrl)
{
	int i;
	struct al_nvme_ctrl *_ctrl = *ctrl;

	if (debug_1) {
		al_nvme_free(debug_1);
		debug_1 = NULL;
	}

	if (_ctrl->bar_kthread)
		kthread_stop(_ctrl->bar_kthread);

	al_nvme_tm_stop(_ctrl->tm);
	al_nvme_tm_destroy(_ctrl->tm);

	if (_ctrl->reqs_cache)
		al_nvme_cache_destroy(_ctrl->reqs_cache);

	al_nvme_admin_free(_ctrl);
	//_free_virt_namespaces(ctrl);

	for (i = 0; i < AL_NVME_QUEUES_MAX; ++i) {
		if (_ctrl->sq[i])
			_free_sq(_ctrl, i);
		if (_ctrl->cq[i])
			_free_cq(_ctrl, i);
	}
	_free_queues(_ctrl);

	if (_ctrl->ctrl_wq)
		destroy_workqueue(_ctrl->ctrl_wq);

	al_nvme_free(_ctrl->target_ctrl);
	_ctrl->target_ctrl = NULL;

	if (_ctrl->pcie_data) {
		kfree(_ctrl->pcie_data);
		_ctrl->pcie_data = NULL;
	}

	for (i = 0; i < AL_NVME_MAX_DMAS; i++) {
		if (_ctrl->dma[i]) {
			kfree(_ctrl->dma[i]);
			_ctrl->dma[i] = NULL;
		}
	}

	al_nvme_free(_ctrl);
	_ctrl = NULL;
}

static u32 pcie_async_event_result(struct nvmet_async_event *aen)
{
	return aen->event_type | (aen->event_info << 8) | (aen->log_page << 16);
}

static void pcie_async_event_work(struct work_struct *work)
{
	struct nvmet_ctrl *ctrl =
		container_of(work, struct nvmet_ctrl, async_event_work);
	struct nvmet_async_event *aen;
	struct nvmet_req *req;

	while (1) {
		mutex_lock(&ctrl->lock);
		aen = list_first_entry_or_null(&ctrl->async_events,
				struct nvmet_async_event, entry);

		if (!aen || !ctrl->nr_async_event_cmds) {
			mutex_unlock(&ctrl->lock);
			return;
		}

		req = ctrl->async_event_cmds[--ctrl->nr_async_event_cmds];
		nvmet_set_result(req, pcie_async_event_result(aen));

		list_del(&aen->entry);
		kfree(aen);
		aen = NULL;

		mutex_unlock(&ctrl->lock);
		nvmet_req_complete(req, 0);
	}
}

static int allocate_ctrl(struct al_nvme_ctrl *ctrl)
{
	struct NvmeBar *ctrl_regs = (struct NvmeBar *)ctrl->pcie_data->inbound_base_addr_bar0;

	ctrl->target_ctrl = al_nvme_zalloc(sizeof(struct nvmet_ctrl));
	if (!ctrl->target_ctrl)
		return -ENOMEM;

	ctrl->target_ctrl->cntlid = ctrl->ctrl_id;
	ctrl->target_ctrl->cap = ctrl_regs->cap;
	ctrl->target_ctrl->ops = &nvmet_pcie_ops;

	INIT_WORK(&ctrl->target_ctrl->async_event_work, pcie_async_event_work);
	INIT_LIST_HEAD(&ctrl->target_ctrl->async_events);

	return 0;
}

static int allocate_queues(struct al_nvme_ctrl *ctrl, int sq_num, int cq_num)
{
	ctrl->sq = al_nvme_calloc(sq_num, sizeof(struct al_nvme_sq *));
	if (!ctrl->sq)
		goto err;

	ctrl->cq = al_nvme_calloc(cq_num, sizeof(struct al_nvme_cq *));
	if (!ctrl->cq)
		goto err;

	ctrl->target_ctrl->sqs = al_nvme_calloc(sq_num, sizeof(struct nvmet_sq *));
	if (!ctrl->target_ctrl->sqs)
		goto err;

	ctrl->target_ctrl->cqs = al_nvme_calloc(cq_num, sizeof(struct nvmet_cq *));
	if (!ctrl->target_ctrl->cqs)
		goto err;
	return 0;

err:
	printk("%s: %d\n", __func__, __LINE__);
	_free_queues(ctrl);
	return -ENOMEM;
}

static int allocate_data_buffer(struct al_nvme_ctrl *ctrl)
{
	unsigned int nents = AL_NVME_CTRL_MAX_PRP_ENTS;
	int i = 0;

	ctrl->sg = kzalloc(sizeof(struct scatterlist) * nents, GFP_KERNEL);
	if (!ctrl->sg) {
		printk("%s: scatterlist is null pointer!\n", __func__);
		return -ENOMEM;
	}

	sg_init_table(ctrl->sg, nents);

	ctrl->data_buffer = kmalloc_array(nents, sizeof(unsigned char *), GFP_KERNEL); 
	if (!ctrl->data_buffer) {
		printk("%s: data_buffer is a NULL pointer\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < nents; i++) {
		ctrl->data_buffer[i] = kzalloc(4096, GFP_KERNEL);
		if (!ctrl->data_buffer[i]) {
			printk("%s: data_buffer[%d] is a NULL pointer\n", __func__, i);
			return -ENOMEM;
		}
		sg_dma_address(&ctrl->sg[i]) = virt_to_phys(ctrl->data_buffer[i]);
		sg_dma_len(&ctrl->sg[i]) = 4096;
	}

	return 0;
}

static void _fetch_prp_complete(void *param)
{
	struct al_nvme_req *req = param;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct al_nvme_tm_task *_task = req->task;
	struct al_nvme_tm_context *ctx = NULL;
	unsigned long flags;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (_task->task_id == ctrl->fetchprp_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->fetchprp_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF

	if (req->cmd->rw.opcode == nvme_cmd_read) {
		spin_lock_irqsave(&ctx->bridge_queue_lock, flags);
		al_nvme_tm_queue_push(req->task, 0, req);
		spin_unlock_irqrestore(&ctx->bridge_queue_lock, flags);
	} else if (req->cmd->rw.opcode == nvme_cmd_write) {
		spin_lock_irqsave(&ctx->cpy_queue_lock, flags);
		al_nvme_tm_queue_push(req->task, 1, req);
		spin_unlock_irqrestore(&ctx->cpy_queue_lock, flags);
	}
#else
	/* send to bridge queue */
	spin_lock_irqsave(&ctx->bridge_queue_lock, flags);
	al_nvme_tm_queue_push(req->task, req->task_queue, req);
	spin_unlock_irqrestore(&ctx->bridge_queue_lock, flags);

#endif

}

static void al_nvme_io_fetch_prp_list(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct nvme_command *cmd;
	unsigned int prp_nents;
	u64 length, prp1, prp2;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;
    struct async_submit_ctl submit;
	enum async_tx_flags flags = ASYNC_TX_XOR_ZERO_DST | ASYNC_TX_ACK;

	cmd = req->cmd;
	prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
	prp2 = le64_to_cpu(cmd->rw.dptr.prp2);

	/* TODO do not assume LBA shift- get it from backend */
	length = (u64)(le16_to_cpu(cmd->rw.length) + 1)
					<< 9;
	length -= min(PRP_LENGTH(prp1, ctrl->params.page_size), length);

	prp_nents = (length + ctrl->params.page_size - 1)
					>> ctrl->params.page_bits;

	BUG_ON(unlikely(prp_nents > AL_NVME_CTRL_MAX_PRP_ENTS - 1));
	req->prp_nents = prp_nents;
	req->sg_nents = prp_nents + 1;
	/* Record the prp counts of each R/W command */
	ctrl->cmd_prp_cnt[req->sq->sq_id-1][cmd->rw.command_id] = req->sg_nents; 

	init_async_submit(&submit,
					flags,
					NULL,
		            _fetch_prp_complete,
					req,
					NULL);

	async_memcpy_phys_by_index(req->prp_list.dma_addr,
			  prp2 + host_mem_base,
			  0, 0,
			  prp_nents * sizeof(u64),
			  &submit,
			  req->ctrl->ctrl_id);

	return;
}

static bool al_nvme_prp_list_required(struct al_nvme_req *req)
{
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_ctrl *ctrl = req->ctrl;

	/* Return true iff:
	 * - This is a read/write command.
	 * - The command's fields PRP1 and PRP2 are not enough to represent
	 *   the command's data length. Meaning- PRP2 points to a list of PRPs.
	 */
	if ((cmd->common.opcode == nvme_cmd_read) ||
	    (cmd->common.opcode == nvme_cmd_write)) {
		/* TODO do not assume LBA shift- get it from backend */
		u64 length = (u64)(le16_to_cpu(cmd->rw.length) + 1)
						<< 9;
		u64 prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
		length -= min(PRP_LENGTH(prp1, ctrl->params.page_size), length);

		if (length > ctrl->params.page_size) {
			return true;
		}

		if (length == 0) {
			req->sg_nents = 1;
		} else if (length > 0 && length <= ctrl->params.page_size) {
			req->sg_nents = 2;
		}
		/* Record the prp counts of each R/W command */
		ctrl->cmd_prp_cnt[req->sq->sq_id-1][cmd->rw.command_id] = req->sg_nents;
	}

	return false;
}

static inline void _nvme_ctrl_commands_complete(struct al_nvme_req *req)
{
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct al_nvme_tm_task *_task = req->task;
	struct al_nvme_tm_context *ctx = NULL;
	unsigned long flags;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (_task->task_id == ctrl->comp_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->comp_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	spin_lock_irqsave(&ctx->comp_cb_lock, flags);
	put_shared_buf(&req->cq->avail_comps, req->shared_comp_buf);

	do {
		struct al_nvme_req *next = req->next_req;
		percpu_ref_put(&req->sq->ref);
		put_shared_buf(req->cmds_lifo, req->shared_cmd_buf);
		put_avail_req(req->reqs_lifo, req);
		req->next_req = NULL;
		req = next;
	} while (req);
	spin_unlock_irqrestore(&ctx->comp_cb_lock, flags);
}


static void _fetch_req_callback(void *param)
{
	struct al_nvme_req *req = param;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct al_nvme_tm_task *_task = req->task;
	struct al_nvme_tm_context *ctx = NULL;
	unsigned long flags;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (_task->task_id == ctrl->fetch_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->fetch_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	spin_lock_irqsave(&ctx->exec_queue_lock, flags);
	/* TODO error handling */
	do {
		struct al_nvme_req *next = req->next_req;

		if(unlikely(!percpu_ref_tryget_live(&req->sq->ref))) {
			printk("%s: ref failed\n", __func__);
			break;
		}

		al_nvme_tm_queue_push(req->task, 0, req);

		req->next_req = NULL;
		req = next;
	} while (req);
	spin_unlock_irqrestore(&ctx->exec_queue_lock, flags);
}

static int process_command(struct al_nvme_ctrl *ctrl, u32 qid, int cnt)
{
	const unsigned int cpus_num =
			AL_NVME_1_RTC_LAST_CPU - AL_NVME_1_RTC_FIRST_CPU + 1;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	unsigned int queues_per_cpu =
			(ctrl->feat_val.num_queues + 1 + cpus_num - 1) / cpus_num;
	unsigned int first_queue = 0, first_cpu_last_q;
	struct al_nvme_tm_context *ctx;

	first_cpu_last_q = first_queue + queues_per_cpu - 1;
	first_cpu_last_q = min(first_cpu_last_q, ctrl->feat_val.num_queues);

	if (qid <= first_cpu_last_q) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	if (atomic_add_negative(cnt, &ctx->nvmet_cmd_cnt)) {
		printk("%s: qid=%d, cmd_cnt=%d, %d\n", __func__, qid, cnt, atomic_read(&ctx->nvmet_cmd_cnt));
	}
	wake_up_interruptible(&ctx->wq);

	return 0;
}

/* Fetch requests task- in charge of fetching commands (descriptors) from one
 * or more submission queues.
 * Parameters:
 *  - dma_chan_id: DMA Channel to use for fetching requests.
 *  - sq_first: First SQ (ID) to monitor for new entries.
 *  - sq_last: Last SQ (ID) to monitor for new entries.
 * This task can fetch requests in batches, whenever possible.
 */
int task_command_fetch(void *priv, void *task)
{
	const unsigned int sq_first =
		al_nvme_tm_task_param_get(task, TASK_FETCH_PARAM_SQ_FIRST);
	const unsigned int sq_last =
		al_nvme_tm_task_param_get(task, TASK_FETCH_PARAM_SQ_LAST);
	struct al_nvme_ctrl *ctrl = priv;
	struct al_nvme_sq *sq;
	struct al_nvme_cq *cq;
	struct al_nvme_req *req[AL_NVME_CTRL_BATCH_SIZE];
	struct al_shared_buf *cmds_buf;
	u64 entry_base_addr;
	unsigned int sq_id;
	unsigned int i;
	unsigned int tail;
	unsigned int pending_cmds;
	unsigned long flags;
	u32 next_head;
	struct async_submit_ctl submit;
	enum async_tx_flags flags_tx = ASYNC_TX_XOR_ZERO_DST | ASYNC_TX_ACK;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;
	struct al_nvme_tm_context *ctx = NULL;
	struct al_nvme_tm_task *_task = task;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;


	if (_task->task_id == ctrl->fetch_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->fetch_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	for (sq_id = sq_first; sq_id <= sq_last; ++sq_id) {  // number of submission queue this cpu
		if (!ctrl->sq[sq_id])
			break;

		sq = ctrl->sq[sq_id];
		if (unlikely(!sq))
			break;

		spin_lock_irqsave(&ctrl->bar_regs_lock, flags);
		tail = sq->tail;
		spin_unlock_irqrestore(&ctrl->bar_regs_lock, flags);

		while (likely(tail != sq->head)) {
			AL_NVME_MB_READ_DEPENDS();

			/* only enable batches for I/O queues */
			if (likely(sq_id)) {
				if (tail > sq->head)
					pending_cmds = tail - sq->head;
				else
					pending_cmds = sq->queue_size - sq->head;
			} else {
				pending_cmds = 1;
			}

			pending_cmds = min_t(unsigned int,
					     AL_NVME_CTRL_BATCH_SIZE,   // 32
					     pending_cmds);

			spin_lock_irqsave(&ctx->comp_cb_lock, flags);
			pending_cmds =
				get_avail_reqs(&sq->avail_reqs, req, pending_cmds);
			/* no free requests are available- we break and try
			 * again later
			 */
			if (pending_cmds == 0) {
					break;
			}
			cmds_buf = get_shared_buf(&sq->avail_cmds, pending_cmds);
			spin_unlock_irqrestore(&ctx->comp_cb_lock, flags);

			next_head = sq->head + pending_cmds;
			if (next_head == sq->queue_size)
				next_head = 0;

			if (sq->in_host_mem == false) {
				entry_base_addr = sq->base_addr + (sq->head << req[0]->sqe_size_2n);
			} else {
				entry_base_addr = host_mem_base + (sq->base_addr + (sq->head << req[0]->sqe_size_2n));
			}

			for (i = 0; i < pending_cmds; ++i) {
				req[i]->task = task;
				/* TODO remove this and supply head from SQ */
				req[i]->sq_head = next_head;

				req[i]->shared_cmd_buf = cmds_buf;
				req[i]->batch_size = pending_cmds;
				req[i]->cmd = cmds_buf->buf + i*sizeof(struct nvme_command);

				if (i < pending_cmds-1)
					req[i]->next_req = req[i+1];
				else
					req[i]->next_req = NULL;
			}

			/* If SQs are locate in local memory, it is more efficient with CPU. */
			if (sq->in_host_mem == false) {
				memcpy(cmds_buf->buf, phys_to_virt(entry_base_addr), sizeof(struct nvme_command) * pending_cmds);
				_fetch_req_callback(req[0]);
			} else {
				init_async_submit(&submit,
						  flags_tx,
						  NULL,
						  _fetch_req_callback,
						  req[0],
						  NULL);

				async_memcpy_phys_by_index(virt_to_phys(cmds_buf->buf),
							   entry_base_addr,
							   0, 0,
							   sizeof(struct nvme_command) * pending_cmds,
							   &submit,
							   req[0]->ctrl->ctrl_id);
			}

			sq->head = next_head;
		}
	}
	return 0;
}

/* gets the MSI-X vector data for the given CQ's vector. also handles
 * mapping the MSI-X host address for the first time, if needed
 */
static inline bool _get_msix_vector(struct al_nvme_ctrl *ctrl,
				    struct al_nvme_cq *cq,
				    void **msix_mapped_addr,
				    void **msix_phys_addr,
				    uint32_t *msix_data)
{
	u16 vector = cq->int_vector;
	u64 host_address = 0, entry_upper_address = 0;
	void __iomem *ptr = (void *)ctrl->pcie_data->inbound_base_addr_bar0;
	void __iomem *ptr_offset = ptr + MSIX_TABLE_OFFSET;
	struct qnap_pcie_ep_msix_entry *qnap_msix_entries =
                (struct qnap_pcie_ep_msix_entry *) ptr_offset;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;

	if (unlikely(vector >= AL_NVME_MSIX_TABLE_SIZE)) {
		pr_err("%s: Vector %u is larger than table size!\n",
		       __func__, vector);
		return false;
	}

	/* TODO support int disabled per SQ */

	/* TODO in order to properly support legacy ints, we need to:
	 *  - figure out at this stage if MSI-X is disabled.
	 *  - if so: mark the first (last) request in the current batch
	 *           and raise a legacy int just about when we want to retire
	 *           the requests back.
	 *  - when raising legacy ints- locking is needed, maybe do it with a
	 *    dedicated task.
	 */

	/* TODO add MSI-X masked vectors support (PBA etc.) */

	entry_upper_address = qnap_msix_entries[vector].msix_entry_upper_address;
	host_address = (entry_upper_address << 32 | qnap_msix_entries[vector].msix_entry_address);
	if (likely(host_address)) {
		/* we may need to map the msi-x host address */
		if (unlikely(cq->msix_host_addr != host_address)) {
			if (cq->mapped_msix_host_addr) {
				_nvme_ctrl_unmap(cq->mapped_msix_host_addr);
			}

			cq->mapped_msix_host_addr =
				ioremap_nocache(host_address + host_mem_base, sizeof(uint32_t));
			cq->msix_host_addr = host_address;
		}

		*msix_mapped_addr = cq->mapped_msix_host_addr;
		*msix_phys_addr = host_address + host_mem_base;
		*msix_data = qnap_msix_entries[vector].msix_entry_data;

		return true;
	}

	return false;
}

static void _msix_callback(void *param)
{
	uint32_t *msix_data = param;
	kfree(msix_data);
}

static void _comp_req_callback(void *param)
{
	struct al_nvme_req *req = param;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct al_nvme_cq *cq = req->cq;
	void *msix_mapped_addr;
 	void *msix_phys_addr;
	uint32_t *msix_data;
	unsigned int async_cmd_free = 0;
	struct async_submit_ctl submit;
	enum async_tx_flags flags_tx = ASYNC_TX_ACK;

	if (req->comp_cb_ctx.async_cmd_free == 0 && req->comp_cb_ctx.msix_addr != NULL) {
		init_async_submit(&submit,
				  flags_tx,
				  NULL,
				  _msix_callback,
				  req->comp_cb_ctx.msix_data,
				  NULL);
		async_memcpy_phys_by_index(req->comp_cb_ctx.msix_addr,
					   req->comp_cb_ctx.msix_data_phy_addr,
					   0,
					   0,
					   sizeof(uint32_t),
					   &submit,
					   req->ctrl->ctrl_id);
	}
	_nvme_ctrl_commands_complete(req);
}


/* Command Completion task- in charge of reporting completion for commands
 * that have completed execution. Also in charge of sending an MSI-X interrupt back to the host.
 * Parameters:
 *  - dma_chan_id: DMA channel to use for command execution.
 *
 * This task can send completions in batches, whenever possible.
 */
int task_command_complete(void *priv, void *task)
{
	struct al_nvme_ctrl *ctrl = priv;
	struct al_nvme_cq *cq;
	struct al_nvme_req *req[AL_NVME_CTRL_BATCH_SIZE];
	struct nvme_command *cmd;
	struct nvme_completion *comp;
	unsigned int entry_offset;
	unsigned int i, j, num, n, async_cmd_free = 0;
	/* we'll do a greedy (and simple) way to batch completions- try to
	 * find as many commands sequentially that belong to the same CQ
	 */
	unsigned int next_req = 0, last_req = 0, pending_reqs = 0;
	struct al_nvme_tm_context *ctx = NULL;
	struct al_shared_buf *comps_buf;
	void *msix_mapped_addr;
	uint32_t *msix_data;
	uint32_t msix_data_2;
	struct al_nvme_tm_task *_task = task;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;
	struct async_submit_ctl submit;
	enum async_tx_flags flags_tx = ASYNC_TX_ACK;
	void *msix_phys_addr;
	unsigned long flags;
	bool qnap_error_handling = false;

	if (_task->task_id == ctrl->comp_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (_task->task_id == ctrl->comp_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	num = al_nvme_tm_queue_get(task, 0, req, AL_NVME_CTRL_BATCH_SIZE);

	while (num) {
		next_req = 0;

		while (next_req < num) {
			pending_reqs = 1;
			/* find out how many share the same cq as last */
			for (i = next_req+1; i < num; ++i) {
				if (req[next_req]->cq_id != req[i]->cq_id)
					{
						break;
					}
				pending_reqs++;
			}

			if (!req[next_req]->cq)
				{
					printk("%s: cq is null\n", __func__);
					return -1;
				}
			/* TODO need to handle when CQ is full */
			cq = req[next_req]->cq;

			if (nvme_cq_full(cq)) {
				printk("%s: completion queue is full \n", __func__);
				continue;
			}

			/* adjust pending_reqs according to CQ */
			pending_reqs = min_t(unsigned int, pending_reqs,
					     cq->queue_size - cq->tail);

			last_req = next_req + pending_reqs - 1;
			/* no need to keep track of counts here */

			spin_lock_irqsave(&ctx->comp_cb_lock, flags);
			comps_buf = get_shared_buf(&cq->avail_comps, 1);
			spin_unlock_irqrestore(&ctx->comp_cb_lock, flags);

			if (!comps_buf) {
					printk("%s: comps_buf is a null pointer\n", __func__);
					break;
			}

			entry_offset = cq->tail << req[next_req]->cqe_size_2n;

			req[next_req]->shared_comp_buf = comps_buf;
			req[next_req]->batch_size = pending_reqs;

			req[next_req]->task = _task;

			j = 0;
			async_cmd_free = 0;

			for (i = next_req; i <= last_req; ++i) {
				for (n = 0; n < req[i]->target_req.sg_cnt; n++) {
					if (req[i]->vbase1[n] != NULL) {
#if !defined(CONFIG_MACH_QNAPTS) || !defined(TRXX)
						_nvme_ctrl_unmap(req[i]->vbase1[n]);
#endif
						req[i]->vbase1[n] = NULL;
					}
				}
				if (req[i]->target_req.sg != NULL) {
					kfree(req[i]->target_req.sg);
					req[i]->target_req.sg = NULL;
					req[i]->target_req.sg_cnt = 0;
				}

				cmd = req[i]->cmd;
				comp = comps_buf->buf +
					j*sizeof(struct nvme_completion);

				/* link requests together */
				if (i < next_req+pending_reqs-1)
					req[i]->next_req = req[i+1];
				else
					req[i]->next_req = NULL;

				comp->result.u32 = cpu_to_le32(req[i]->result);
				comp->sq_head = cpu_to_le16(req[i]->sq_head);
				comp->sq_id = cpu_to_le16(req[i]->sq_id);
				comp->command_id = cpu_to_le16(cmd->common.command_id);
				comp->status = cpu_to_le16((req[i]->status << 1) | cq->phase);

				if (cmd->common.opcode != nvme_admin_async_event) {
					if (atomic_sub_return(1, &ctx->nvmet_cmd_cnt) < 0) {
						printk("%s: qid=%d, pending_reqs=%d, "
						       "cmd_cnt=%d, next_req=%d, last_req=%d\n", __func__, cq->cq_id,
						       pending_reqs, atomic_read(&ctx->nvmet_cmd_cnt), next_req, last_req);
						atomic_add(1, &ctx->nvmet_cmd_cnt);
					}
				}
				if (cmd->common.opcode == nvme_admin_async_event) {
					if (req[i]->status == NVME_SC_SUCCESS) {
						async_cmd_free = 0;
					} else {
						async_cmd_free = 1;
					}
				}
				j++;
			}

			msix_data = al_nvme_calloc(1, sizeof(uint32_t));
			req[next_req]->comp_cb_ctx.async_cmd_free = (u8) async_cmd_free;
			req[next_req]->comp_cb_ctx.pending_reqs = pending_reqs;

			if (likely(_get_msix_vector(ctrl, cq, &msix_mapped_addr, &msix_phys_addr,
						    msix_data)) ) {
				req[next_req]->comp_cb_ctx.msix_addr =(phys_addr_t) msix_phys_addr;			       
				req[next_req]->comp_cb_ctx.msix_data_phy_addr = virt_to_phys(msix_data);
				req[next_req]->comp_cb_ctx.msix_data = msix_data;
			} else {
				req[next_req]->comp_cb_ctx.msix_addr = NULL;
				printk("%s, %d: get msix vector error\n", __func__, __LINE__);

			}

			if (req[next_req]->ctrl->ctrl_id == 0) {
				qnap_error_handling = ep0_error_handling;
			}
			if (req[next_req]->ctrl->ctrl_id == 1) {
				qnap_error_handling = ep1_error_handling;
			}

			if (qnap_error_handling == false) {
				init_async_submit(&submit,
						  flags_tx,
						  NULL,
						  _comp_req_callback,
						  req[next_req],
						  NULL);

				async_memcpy_phys_by_index(cq->host_addr_phys + entry_offset,
							   virt_to_phys(comps_buf->buf),
							   0, 0,
							   sizeof(struct nvme_completion) * pending_reqs,
							   &submit,
							   req[next_req]->ctrl->ctrl_id);
			} else {
				kfree(msix_data);
				_nvme_ctrl_commands_complete(req[next_req]);
			}

			next_req = last_req + 1;
			cq->tail += pending_reqs;
			if (cq->tail == cq->queue_size) {
				cq->tail = 0;
				cq->phase = !cq->phase;
			}

		}

		num = al_nvme_tm_queue_get(task, 0, req, AL_NVME_CTRL_BATCH_SIZE);
	}
	return 0;
}

int task_command_bridge(void *priv, void *task)
{
	struct al_nvme_req *req;
	struct al_nvme_ctrl *ctrl = priv;
	u16 status = 0;
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
	uint64_t data_size = 0;
	struct nvmet_ns *ns = NULL;
	uint32_t nlb  = 0;
#endif

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF_RAM
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;
	unsigned long flags;
	struct al_nvme_tm_task *_task = task;
#endif
	struct al_nvme_tm_context *ctx = NULL;
	bool qnap_error_handling = false;
	unsigned long flags;

	while (al_nvme_tm_queue_get(task, 0, &req, 1)) {
		/* send to execution */
		req->task = task;
		req->task_queue = 0;
		req->target_req.cmd = req->cmd;

		if (!ctrl->target_ctrl->subsys) {
			al_nvme_tm_queue_push(task, 0, req);
			return 0;
		}

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		//
		// For RW-Buf scheme, nvmet_req init for write cmd is handled in task_cmd_cpy.
		//
		if (req->cmd->common.opcode != nvme_cmd_write) {
			if (!nvmet_req_init(&req->target_req, &req->cq->nvmet_cq,
					    &req->sq->nvmet_sq, &nvmet_pcie_ops)) {
				return 0;
			}
		}

#else

		if (!nvmet_req_init(&req->target_req, &req->cq->nvmet_cq,
							&req->sq->nvmet_sq, &nvmet_pcie_ops)) {
			return 0;
		}
#endif

		if (req->cmd->common.opcode == nvme_cmd_read ||
			req->cmd->common.opcode == nvme_cmd_write) {
#ifdef CONFIG_NVME_RAM_TEST
			status = nvmet_pcie_data_to_ram(req);
#else
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF

			memcpy(&req->rwbuf_treq, &req->target_req, sizeof(struct nvmet_req));
			nlb  = le32_to_cpu(req->cmd->rw.length);
			data_size = (nlb + 1) << req->target_req.ns->blksize_shift;
			//set real data size
			req->rw_sgl.length = data_size;
			req->rwbuf_treq.sg = &req->rw_sgl;
			req->rwbuf_treq.sg_cnt = 1;
#else
			status = nvmet_pcie_map_sgl(req);
#if defined(CONFIG_MACH_QNAPTS) && defined(TRXX)
			req->target_req.flags = (1 << NVME_CTRL_DMA_ADDR);
#endif
#endif
#endif
			if (status) {
				printk("%s: pcie map sgl failed, status=%d\n", __func__, status);
				al_nvme_tm_queue_push(task, 0, req);
				return 0;
			}
		}
		if (req->cmd->common.opcode == nvme_cmd_dsm) {
			if (debug_flag) {
				printk("%s: DSM: op=%d, nr=%d, attr=%d, "
					"prp1=0x%llx, prp2=0x%llx\n", __func__,
					req->cmd->dsm.opcode,
					req->cmd->dsm.nr,
					req->cmd->dsm.attributes,
					req->cmd->dsm.dptr.prp1,
					req->cmd->dsm.dptr.prp2);
			}
			status = nvmet_get_dsm_data(req, &req->target_req.sg, &req->target_req.sg_cnt);
			if (status) {
				printk("%s: get DSM data failed\n", __func__);
				al_nvme_tm_queue_push(task, 0, req);
				return 0;
			}
		}

		if (req->ctrl->ctrl_id == 0) {
			qnap_error_handling = ep0_error_handling;
		}
		if (req->ctrl->ctrl_id == 1) {
			qnap_error_handling = ep1_error_handling;
		}

		/* execute r/w and submit BIO, when BIO done,
		 * push the req to complete queue.
		 *
		 */
#ifndef CONFIG_NVME_RAM_TEST
		if (qnap_error_handling == false) {
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF

#ifndef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF_RAM

			req->target_req.execute(&req->rwbuf_treq);
#else

			if (_task->task_id == ctrl->bridge_task_id[first_cpu]) {
				ctx = &ctrl->tm->cpu_ctx[first_cpu];
			} else if (_task->task_id == ctrl->bridge_task_id[last_cpu]) {
				ctx = &ctrl->tm->cpu_ctx[last_cpu];
			}

			if (req->cmd->common.opcode == nvme_cmd_read) {
				spin_lock_irqsave(&ctx->cpy_queue_lock, flags);
				al_nvme_tm_queue_push(task, 1, req);
				spin_unlock_irqrestore(&ctx->cpy_queue_lock, flags);
			} else if (req->cmd->common.opcode == nvme_cmd_write) {
				spin_lock_irqsave(&ctx->comp_queue_lock, flags);
				al_nvme_tm_queue_push(task, 0, req);
				spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
			} else {
				printk("%s, %d: unknow error op-code %d, sqid %d . \n",
				       __func__,
				       __LINE__,
				       req->cmd->common.opcode,
				       req->sq_id);
			}

#endif

#else

			req->target_req.execute(&req->target_req);
#endif
		} else {
			req->status = NVME_SC_DATA_XFER_ERROR;
			printk("%s: QNAP EH mode: Push the req to completion queue\n", __func__);
			spin_lock_irqsave(&ctx->comp_queue_lock, flags);
			al_nvme_tm_queue_push(task, 0, req);
			spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
		}
#endif
	}

	return 0;
}

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
static void _cpy_req_callback(void *param)
{
	struct al_nvme_req *req = param;
	struct nvme_command *cmd = req->cmd;
	struct al_nvme_tm_task *task = req->task;
	unsigned long flags;
	struct al_nvme_ctrl *ctrl = req->ctrl;
	struct al_nvme_tm_context *ctx = NULL;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	if (task->task_id == ctrl->cpy_task_id[first_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[first_cpu];
	} else if (task->task_id == ctrl->cpy_task_id[last_cpu]) {
		ctx = &ctrl->tm->cpu_ctx[last_cpu];
	}

	if (cmd->rw.opcode == nvme_cmd_read) {
		spin_lock_irqsave(&ctx->comp_queue_lock, flags);
		al_nvme_tm_queue_push(task, 1, req);
		spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);

	} else if (cmd->rw.opcode == nvme_cmd_write) {
		spin_lock_irqsave(&ctx->bridge_queue_lock, flags);
		al_nvme_tm_queue_push(task, 0, req);
		spin_unlock_irqrestore(&ctx->bridge_queue_lock, flags);
	}
}

int task_command_cpy(void *priv, void *task)
{
	struct al_nvme_req *req;
	uint16_t status = 0;
	struct al_nvme_tm_context *ctx;
	struct al_nvme_ctrl *ctrl = priv;
	struct al_nvme_tm_task *_task = task;
	unsigned int first_cpu = ctrl->first_cpu;
	unsigned int last_cpu = ctrl->last_cpu;
	unsigned long flags;
	bool qnap_error_handling = false;


	while (al_nvme_tm_queue_get(task, 0, &req, 1)) {
		req->task = task;
		if (_task->task_id == ctrl->cpy_task_id[first_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[first_cpu];
		} else if (_task->task_id == ctrl->cpy_task_id[last_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[last_cpu];
		}

		if (req->ctrl->ctrl_id == 0) {
			qnap_error_handling = ep0_error_handling;
		}
		if (req->ctrl->ctrl_id == 1) {
			qnap_error_handling = ep1_error_handling;
		}

		if (qnap_error_handling == false) {
			req->target_req.cmd = req->cmd;
			if (req->cmd->rw.opcode == nvme_cmd_write) {
				if (!nvmet_req_init(&req->target_req, &req->cq->nvmet_cq,
						    &req->sq->nvmet_sq, &nvmet_pcie_ops)) {

					printk("%s, %d: req init failed in cpy phase\n",
					       __func__,
					       __LINE__);
					return 0;
				}
			}

			status = nvmet_pcie_map_sgl(req);

			if (status) {
				printk("%s, %d: pcie map sgl failed, status=%d\n",
				       __func__,
				       __LINE__,
				       status);
				spin_lock_irqsave(&ctx->comp_queue_lock, flags);
				al_nvme_tm_queue_push(task, 1, req);
				spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
				return 0;
			}
			if (req->cmd->rw.opcode == nvme_cmd_read) {

				status = async_single_buf_to_sg_by_index(req->target_req.sg,
									 req->target_req.sg_cnt,
									 req->rw_buf,
									 _cpy_req_callback,
									 req,
									 req->ctrl->ctrl_id);
				if (status) {
					printk("%s, %d: async_single_buf_to_sg failed, status=%d\n",
					       __func__,
					       __LINE__,
					       status);

				}

			} else if (req->cmd->rw.opcode == nvme_cmd_write) {

				status = async_sg_to_single_buf_by_index(req->target_req.sg,
									 req->target_req.sg_cnt,
									 req->rw_buf,
									 _cpy_req_callback,
									 req,
									 req->ctrl->ctrl_id);
				if (status) {
					printk("%s, %d: async_sg_to_single_buf failed, status=%d\n",
					       __func__,
					       __LINE__,
					       status);

				}
			}

		} else {
			req->status = NVME_SC_DATA_XFER_ERROR;
			printk("%s, %d: QNAP EH mode: Push the req to completion queue\n",
			       __func__,
			       __LINE__);
			spin_lock_irqsave(&ctx->comp_queue_lock, flags);
			al_nvme_tm_queue_push(task, 1, req);
			spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
		}
	}

	return 0;
}
#endif


/* Fetch PRP List task- in charge of fetching a request's PRP list
 */
int task_command_fetch_prp(void *priv, void *task)
{
	struct al_nvme_req *req;

	/* TODO add batching support */
	while (al_nvme_tm_queue_get(task, 0, &req, 1)) {
		/* TODO set channel here? */
		req->task = task;
		req->task_queue = 0;
		al_nvme_io_fetch_prp_list(req);
	}

	return 0;
}

/* Command Execution task- in charge of execution commands.
 * Parameters:
 *  - dma_chan_id: DMA channel to use for command execution.
 */
int task_command_execute(void *priv, void *task)
{
	struct al_nvme_ctrl *ctrl = priv;
	struct al_nvme_req *req;
	enum al_req_execution_status st;
	struct al_nvme_tm_context *ctx;
	struct al_nvme_tm_task *_task = task;
	unsigned long flags;
	bool qnap_error_handling = false;

	while (al_nvme_tm_queue_get(task, 0, &req, 1)) {
		struct nvme_command *cmd = req->cmd;
		struct al_nvme_ctrl *ctrl = req->ctrl;
		unsigned int first_cpu = ctrl->first_cpu;
		unsigned int last_cpu = ctrl->last_cpu;

		if (_task->task_id == ctrl->exec_task_id[first_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[first_cpu];
		} else if (_task->task_id == ctrl->exec_task_id[last_cpu]) {
			ctx = &ctrl->tm->cpu_ctx[last_cpu];
		}

		req->task = task;
		req->task_queue = 0;

		if (req->ctrl->ctrl_id == 0) {
			qnap_error_handling = ep0_error_handling;
		}
		if (req->ctrl->ctrl_id == 1) {
			qnap_error_handling = ep1_error_handling;
		}

		/* I/O queues handle prp */
		if (qnap_error_handling == false) {
			if (likely(req->sq_id != 0)) {
				if (debug_flag) {
					printk("%s: I/O cmd_op=%d, cmd_id=%d, sq_id=%d, "
					"nlba=%d, slba=0x%llx, prp1=0x%llx, prp2=0x%llx\n",
					 __func__, cmd->common.opcode, cmd->common.command_id,
					req->sq->sq_id, cmd->rw.length, cmd->rw.slba, cmd->rw.dptr.prp1, cmd->rw.dptr.prp2);
				}
				if (al_nvme_prp_list_required(req)) {
					/* send to fetch prp queue */
					al_nvme_tm_queue_push(task, 1, req);
				} else {
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
					if (req->cmd->rw.opcode == nvme_cmd_read) {
						spin_lock_irqsave(&ctx->bridge_queue_lock, flags);
						al_nvme_tm_queue_push(task, 2, req);
						spin_unlock_irqrestore(&ctx->bridge_queue_lock, flags);
					} else if (cmd->rw.opcode == nvme_cmd_write) {
						spin_lock_irqsave(&ctx->cpy_queue_lock, flags);
						al_nvme_tm_queue_push(task, 3, req);
						spin_unlock_irqrestore(&ctx->cpy_queue_lock, flags);
					}
#else
					/* send to bridge queue */
					spin_lock_irqsave(&ctx->bridge_queue_lock, flags);
					al_nvme_tm_queue_push(task, 2, req);
					spin_unlock_irqrestore(&ctx->bridge_queue_lock, flags);
#endif
				}
			} else {
				if (debug_flag) {
					printk("%s: ADMIN cmd_op=%d, "
					"cmd_id=%d, sq_id=%d\n", __func__, cmd->common.opcode,
					cmd->common.command_id, req->sq->sq_id);
				}

				if (cmd->common.opcode == nvme_admin_async_event) {
					if (atomic_sub_return(1, &ctx->nvmet_cmd_cnt) < 0) {
						atomic_add(1, &ctx->nvmet_cmd_cnt);
						printk("%s: %d\n", __func__, atomic_read(&ctx->nvmet_cmd_cnt));
					}
				}

				st = al_nvme_admin_request_execute(req);
				if (st == NVME_REQ_COMPLETE) {
					/* sent to complete queue */
					spin_lock_irqsave(&ctx->comp_queue_lock, flags);
					al_nvme_tm_queue_push(task, 0, req);
					spin_unlock_irqrestore(&ctx->comp_queue_lock, flags);
				}
			}
		} else {
			req->status = NVME_SC_DATA_XFER_ERROR;
			printk("%s: QNAP EH mode: Push the req to completion queue\n", __func__);
			al_nvme_tm_queue_push(task, 0, req);
		}
	}

	return 0;
}

void al_nvme_queues_num_updated(struct al_nvme_ctrl *ctrl)
{
	const unsigned int cpus_num =
			      AL_NVME_1_RTC_LAST_CPU - AL_NVME_1_RTC_FIRST_CPU + 1;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	unsigned int queues_per_cpu =
			(ctrl->feat_val.num_queues + 1 + cpus_num - 1) / cpus_num;
	unsigned int queue_size, current_queue = 0, first_queue, last_queue;
	unsigned int i;

	/* Redistribute queues among all RTC (Run To Completion) CPUs
	 * and resize task queues
	 */
	for (i = first_cpu; i <= last_cpu; ++i) {
		first_queue = current_queue;
		last_queue = first_queue + queues_per_cpu - 1;
		last_queue = min(last_queue, ctrl->feat_val.num_queues);

		al_nvme_tm_task_param_set(ctrl->tm, ctrl->fetch_task_id[i],
					  TASK_FETCH_PARAM_SQ_FIRST,
					  first_queue);

		al_nvme_tm_task_param_set(ctrl->tm, ctrl->fetch_task_id[i],
					  TASK_FETCH_PARAM_SQ_LAST,
					  last_queue);
		current_queue = last_queue + 1;

		/* TODO enable queue resize at some point */
		/* also set task queue sizes for this cpu */
		queue_size = (last_queue - first_queue + 1) *
						AL_NVME_AVAIL_REQS_PER_SQ;     /* queue number * queue depth */
		queue_size = max_t(unsigned int, queue_size, 1);

		al_nvme_tm_queue_resize(ctrl->tm, ctrl->exec_queue_id[i],
					queue_size);
		al_nvme_tm_queue_resize(ctrl->tm, ctrl->bridge_queue_id[i],
					queue_size);
		al_nvme_tm_queue_resize(ctrl->tm, ctrl->comp_queue_id[i],
					queue_size);
		al_nvme_tm_queue_resize(ctrl->tm, ctrl->fetchprp_queue_id[i],
					queue_size);

		printk("CPU%u: first queue %u; last queue %u; queue size %u\n", i,
                                        first_queue, last_queue, queue_size);
	}

	printk("Redistributed queues over %u cpus (First cpu: %u)\n",
		cpus_num, first_cpu);

	return;
}

static int initialize_task_manager(struct al_nvme_ctrl *ctrl)
{
	int task_id;
	int fetch_id, fetchprp_id, exec_id, comp_id, bridge_id;
	int exec_qid, comp_qid, fetchprp_qid, bridge_qid;
	unsigned int i;
	struct cpumask used_cpus;
	struct al_nvme_task_manager *tm;
	unsigned int first_rtc_cpu;
	unsigned int last_rtc_cpu;
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
	int cpy_id;
	int cpy_qid;
#endif

	/* our setup below requires all 4 cpus to be available, sort of */
	if (num_possible_cpus() < 4)
		return -EINVAL;

	cpumask_copy(&used_cpus, cpu_online_mask);
	switch (ctrl->ctrl_id) {
	case 0:
		/* We use cpu0 and cpu1 if NVMe controller 0 */
        first_rtc_cpu = AL_NVME_1_RTC_FIRST_CPU;
        last_rtc_cpu = AL_NVME_1_RTC_LAST_CPU;
        cpumask_clear_cpu(AL_NVME_2_RTC_FIRST_CPU, &used_cpus);
        cpumask_clear_cpu(AL_NVME_2_RTC_LAST_CPU, &used_cpus);
		break;
	case 1:
		/* We use cpu2 and cpu3 if NVMe controller 1 */
        first_rtc_cpu = AL_NVME_2_RTC_FIRST_CPU;
        last_rtc_cpu = AL_NVME_2_RTC_LAST_CPU;
        cpumask_clear_cpu(AL_NVME_1_RTC_FIRST_CPU, &used_cpus);
        cpumask_clear_cpu(AL_NVME_1_RTC_LAST_CPU, &used_cpus);
		break;
	default:
		printk("NVMe Controller %d is invalid\n", ctrl->ctrl_id);
		return -EINVAL;
	}

	ctrl->first_cpu = first_rtc_cpu;
	ctrl->last_cpu = last_rtc_cpu;

	ctrl->tm = al_nvme_tm_create(&used_cpus);
	if (IS_ERR(ctrl->tm))
		return -ENOMEM;

	tm = ctrl->tm;
	/* create tasks */
	/* setup tasks for all other cpus */
	for (i = first_rtc_cpu; i <= last_rtc_cpu; ++i) {
		unsigned int first_sq, last_sq;

		/* for now- let cpu 2 handle the admin task. i/o queues will be
		 * distributed among cpus later
		 */
		if (i == first_rtc_cpu) {
			first_sq = 0;
			last_sq = 0;
		} else {
			first_sq = 1;
			last_sq = 1;
		}
		/* ******************* CPU i ******************* */

		/*
		 * Create command fetch task.
		 */
		task_id = al_nvme_tm_task_create(tm, i, task_command_fetch, ctrl);
		al_nvme_tm_task_param_set(tm, task_id,
					  TASK_FETCH_PARAM_SQ_FIRST, first_sq);
		al_nvme_tm_task_param_set(tm, task_id,
					  TASK_FETCH_PARAM_SQ_LAST, last_sq);
		fetch_id = task_id;
		ctrl->fetch_task_id[i] = fetch_id;

		/*
		 * Create command execute task.
		 */
		task_id = al_nvme_tm_task_create(tm, i, task_command_execute, ctrl);
		exec_id = task_id;
		ctrl->exec_task_id[i] = exec_id;


		/*
		 * create command cpy task
		 */
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		task_id = al_nvme_tm_task_create(tm, i, task_command_cpy, ctrl);
		cpy_id = task_id;
		ctrl->cpy_task_id[i] = cpy_id;
#endif

		/*
		 * Create command bridge task.
		 */
		task_id = al_nvme_tm_task_create(tm, i, task_command_bridge, ctrl);
		bridge_id = task_id;
		ctrl->bridge_task_id[i] = bridge_id;

		/*
		 * Create command complete task.
		 */
		task_id = al_nvme_tm_task_create(tm, i, task_command_complete, ctrl);
		comp_id = task_id;
		ctrl->comp_task_id[i] = comp_id;

		/*
		 * Create command fetch prp task.
		 */
		task_id = al_nvme_tm_task_create(tm, i, task_command_fetch_prp, ctrl);
		fetchprp_id = task_id;
		ctrl->fetchprp_task_id[i] = fetchprp_id;

		/* create task queues */
		/* TODO use dynamic queue sizes, not hardcoded */
		/* alloc al_nvme_tm_queue , nvme request */
		exec_qid = al_nvme_tm_queue_create(tm, 1024);       /* 0, 4 */
		bridge_qid = al_nvme_tm_queue_create(tm, 1024);     /* 1, 5 */
		comp_qid = al_nvme_tm_queue_create(tm, 1024);       /* 2, 6 */
		fetchprp_qid = al_nvme_tm_queue_create(tm, 1024);   /* 3, 7 */
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		cpy_qid = al_nvme_tm_queue_create(tm, 1024);
#endif

		/* Save the queue IDs aside, we'll need them if we ever need
		 * to resize the queues
		 */
		ctrl->exec_queue_id[i] = exec_qid;
		ctrl->bridge_queue_id[i] = bridge_qid;
		ctrl->comp_queue_id[i] = comp_qid;
		ctrl->fetchprp_queue_id[i] = fetchprp_qid;
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		ctrl->cpy_queue_id[i] = cpy_qid;
#endif

		/* Set input queues */
		al_nvme_tm_tasks_queue_in_set(tm, exec_id, 0, exec_qid);
		al_nvme_tm_tasks_queue_in_set(tm, bridge_id, 0, bridge_qid);
		al_nvme_tm_tasks_queue_in_set(tm, comp_id, 0, comp_qid);
		al_nvme_tm_tasks_queue_in_set(tm, fetchprp_id, 0, fetchprp_qid);
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		al_nvme_tm_tasks_queue_in_set(tm, cpy_id, 0, cpy_qid);
#endif

		/* Set output queues */
		al_nvme_tm_tasks_queue_out_set(tm, fetch_id, 0, exec_qid);
		al_nvme_tm_tasks_queue_out_set(tm, exec_id, 0, comp_qid);
		al_nvme_tm_tasks_queue_out_set(tm, exec_id, 1, fetchprp_qid);
		al_nvme_tm_tasks_queue_out_set(tm, exec_id, 2, bridge_qid);
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		al_nvme_tm_tasks_queue_out_set(tm, exec_id, 3, cpy_qid);
#endif
		al_nvme_tm_tasks_queue_out_set(tm, bridge_id, 0, comp_qid);
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		al_nvme_tm_tasks_queue_out_set(tm, bridge_id, 1, cpy_qid);
#endif

		al_nvme_tm_tasks_queue_out_set(tm, fetchprp_id, 0, bridge_qid);
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		al_nvme_tm_tasks_queue_out_set(tm, fetchprp_id, 1, cpy_qid);
		al_nvme_tm_tasks_queue_out_set(tm, cpy_id, 0, bridge_qid);
		al_nvme_tm_tasks_queue_out_set(tm, cpy_id, 1, comp_qid);


#endif


#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		printk("NVMe cpu %d, fetch id=%d, exec id=%d, cpy id=%d, "
				"bridge id=%d, comp id=%d, fetchprp id=%d\n",
		       i, ctrl->fetch_task_id[i], ctrl->exec_task_id[i], ctrl->cpy_task_id[i],
		       ctrl->bridge_task_id[i], ctrl->comp_task_id[i],
		       ctrl->fetchprp_task_id[i]);

#else
		printk("NVMe cpu %d, fetch id=%d, exec id=%d, "
				"bridge id=%d, comp id=%d, fetchprp id=%d\n",
				i, ctrl->fetch_task_id[i], ctrl->exec_task_id[i],
				ctrl->bridge_task_id[i], ctrl->comp_task_id[i],
				ctrl->fetchprp_task_id[i]);
#endif
	}

	return 0;
}

static int nvme_update_db(void *data)
{
	struct al_nvme_ctrl *ctrl = (struct al_nvme_ctrl *)data;
	void __iomem *ptr = (void *) ctrl->pcie_data->inbound_base_addr_bar0;
	void __iomem *dbs = ptr + NVME_REG_DBS;
	unsigned long size = 0, i = 0, flags;
	unsigned int stride = QNAP_NVME_CAP_DSTRD(ctrl->target_ctrl->cap);
	unsigned int qid = 0;
	int cmd_cnt = 0;
	struct al_nvme_cq *cq;
	struct al_nvme_sq *sq;
	u32 new_head = 0, new_tail = 0;

	size = NVME_REG_DBS + ((2 * (AL_NVME_QUEUES_MAX - 1) + 1) * (4 << stride));

    while (!kthread_should_stop()) {
        for (i = NVME_REG_DBS; i <= size; i = i + (1 << (2 + stride)))
        {
            /* CQ */
            if (((i - NVME_REG_DBS) >> (2 + stride)) & 1) {
                qid = (i - (NVME_REG_DBS + (1 << (2 + stride)))) >> (3 + stride);
                if (!ctrl->cq[qid] || !ctrl->sq[qid])
                    continue;

                cq = ctrl->cq[qid];

                spin_lock_irqsave(&ctrl->bar_regs_lock, flags);
				new_head = *((u32 *) (dbs + (i - NVME_REG_DBS)));
                cq->head = new_head;
	            spin_unlock_irqrestore(&ctrl->bar_regs_lock, flags);
            } else { /* SQ */
                cmd_cnt = 0;
                qid = (i - NVME_REG_DBS) >> (3 + stride);
                if (!ctrl->cq[qid] || !ctrl->sq[qid])
                    continue;

                sq = ctrl->sq[qid];

                spin_lock_irqsave(&ctrl->bar_regs_lock, flags);
				new_tail = *((u32 *) (dbs + (i - NVME_REG_DBS)));
                cmd_cnt = new_tail - sq->tail;

                if (cmd_cnt < 0)
                    cmd_cnt = sq->queue_size - sq->tail + new_tail;

                //if (debugDB_flag)
                //    pr_info("%s: cmd_cnt=%d, qid=%d, new_tail=%d, sq->tail=%d\n", __func__, cmd_cnt, qid, new_tail, sq->tail);
                sq->tail = new_tail;

                ctrl->cmd_cnt[qid] = cmd_cnt;
                if (cmd_cnt > 0)
                    process_command(ctrl, qid, cmd_cnt);
	            spin_unlock_irqrestore(&ctrl->bar_regs_lock, flags);
            }
        }
        //usleep_range(100, 200);
        //msleep(5);
		schedule();
#if 0
        //cnt++;
        for (n = 0; n < AL_NVME_QUEUES_MAX; n++) {
            if (ctrl->cmd_cnt[n] != 0) {
                flag = 1;
                break;
            } else
                flag = 0;
        }

        //if (flag || cnt < 3000) {
        if (flag) {
            //debug_schedule_cnt++;
            schedule();
        //} else if (flag == 0 && cnt > 3000) {
        } else {
            //cnt = 0;
            //debug_sleep_cnt++;
            msleep(1);
        }
#endif
    }

    return 0;
}

int nvme_db_thread_create(struct al_nvme_ctrl *ctrl)
{
	const unsigned int first_cpu = ctrl->first_cpu;

	ctrl->db_kthread = kthread_create(nvme_update_db, ctrl, "ctrl %d DB update", ctrl->ctrl_id);
	if (IS_ERR(ctrl->db_kthread)) {
		pr_err("%s: Could not create kthread\n", __func__);
		goto error;
	}

	//kthread_bind(ctrl->db_kthread, first_cpu);

	wake_up_process(ctrl->db_kthread);

	return 0;

error:
	kthread_stop(ctrl->db_kthread);
	return -1;
}

static int nvme_update_bar(void *data)
{
	struct al_nvme_ctrl *ctrl = (struct al_nvme_ctrl *) data;
	struct NvmeBar *ctrl_regs = (struct NvmeBar *)ctrl->pcie_data->inbound_base_addr_bar0;

    while (!kthread_should_stop()) {
		/*
		 * Contoller Configuration (0x14)
		 */
        if (!QNAP_NVME_CC_EN(ctrl_regs->cc) && !QNAP_NVME_CC_EN(ctrl->target_ctrl->cc) &&
            !QNAP_NVME_CC_SHN(ctrl_regs->cc) && !QNAP_NVME_CC_SHN(ctrl->target_ctrl->cc)) {
            ctrl->target_ctrl->cc = ctrl_regs->cc;
        }
        /* Enable */
        if (QNAP_NVME_CC_EN(ctrl_regs->cc) != QNAP_NVME_CC_EN(ctrl->target_ctrl->cc)) {
            printk("%s: Controller %x CC_EN %x to %x \n", __func__, ctrl->ctrl_id, QNAP_NVME_CC_EN(ctrl->target_ctrl->cc), QNAP_NVME_CC_EN(ctrl_regs->cc));
            ctrl->target_ctrl->cc = ctrl_regs->cc;
            if (QNAP_NVME_CC_EN(ctrl_regs->cc)) {
		        queue_work(ctrl->ctrl_wq, &ctrl->start_ctrl_work);
            } else {
		        queue_work(ctrl->ctrl_wq, &ctrl->stop_ctrl_work);
            }
        }
        /* Shutdown Notification */
        if (QNAP_NVME_CC_SHN(ctrl_regs->cc) != QNAP_NVME_CC_SHN(ctrl->target_ctrl->cc)) {
            ctrl->target_ctrl->cc = ctrl_regs->cc;
            if (QNAP_NVME_CC_SHN(ctrl_regs->cc)) {
                printk("%s: Controller %x shutdown\n", __func__, ctrl->ctrl_id);
                queue_work(ctrl->ctrl_wq, &ctrl->stop_ctrl_work);
                //pr_info("%s: shutdown\n", __func__);
                //ctrl_regs->csts |= QNAP_NVME_CSTS_SHST_COMPLETE;
                //ctrl->target_ctrl->csts = ctrl_regs->csts;
            } else {
                printk("%s: No Effect!!\n", __func__);
                ctrl_regs->csts &= ~QNAP_NVME_CSTS_SHST_COMPLETE;
                ctrl->target_ctrl->csts = ctrl_regs->csts;
                ctrl->target_ctrl->cc = ctrl_regs->cc;
            }
        }
        msleep(10);
    }

    return 0;
}

int nvme_bar_thread_create(struct al_nvme_ctrl *ctrl)
{
	ctrl->bar_kthread = kthread_create(nvme_update_bar,
										ctrl,
										"ctrl %d BAR update",
										ctrl->ctrl_id);
	if (IS_ERR(ctrl->bar_kthread)) {
		pr_err("%s: Could not create kthread\n", __func__);
		goto error;
	}

	wake_up_process(ctrl->bar_kthread);

	return 0;

error:
	kthread_stop(ctrl->bar_kthread);
	return -1;
}

static void pcie_sq_free(struct percpu_ref *ref)
{
	struct al_nvme_sq *sq = container_of(ref, struct al_nvme_sq, ref);

	complete(&sq->free_done);
}

int al_nvme_sq_init(struct al_nvme_ctrl *ctrl, u16 sq_id, u16 cq_id,
			u32 queue_size, bool in_host_mem, u64 base_addr)
{
	struct al_nvme_sq *sq;
	struct al_nvme_req *req;
	struct al_shared_buf *buf;
	unsigned int i;
	unsigned int reqs_num;
	int ret;
	void __iomem *ptr = (void *) ctrl->pcie_data->inbound_base_addr_bar0;
	void __iomem *cmb_off;
	char cmb_szu = ctrl->params.cmb_szu;

	/* Queue depth (size) of this queue */
	ctrl->sq[sq_id] = al_nvme_zalloc(sizeof(struct al_nvme_sq));
	if (!ctrl->sq[sq_id])
	{
		printk("ctrl->sq[%d] is a NULL pointer\n", sq_id);
		return -1;
	}

	if (in_host_mem == false) {
		cmb_off = (ptr + QNAP_NVME_CMB_OFST_SZU * SZ_4K * (1 << (4 * cmb_szu))) +
					(sq_id - 1) * queue_size * (1 << ctrl->params.sqe_size_2n);
	}

	sq = ctrl->sq[sq_id];
	sq->ctrl = ctrl;
	sq->sq_id = sq_id;
	sq->cq_id = cq_id;
	sq->queue_size = queue_size;
	sq->in_host_mem = in_host_mem;

	if (sq->in_host_mem == false) {
		sq->base_addr = virt_to_phys(cmb_off);
	} else {
		sq->base_addr = base_addr;
	}

	sq->tail = 0;
	sq->head = 0;

	if (sq_id != cq_id) {
		pr_err("%s: NVMe Controller implementation assumes each SQ has"
				" a dedicated CQ- this doesn't appear to be the case! ("
				"SQ %u is mapped to CQ %u)\n", __func__, sq_id, cq_id);
	}

	ret = percpu_ref_init(&sq->ref, pcie_sq_free, 0, GFP_KERNEL);
	if (ret) {
		pr_err("percpu_ref init failed!\n");
		return ret;
	}
	init_completion(&sq->free_done);
	init_completion(&sq->confirm_done);

	nvmet_sq_init(&sq->nvmet_sq);
	nvmet_sq_setup(ctrl->target_ctrl, &sq->nvmet_sq, sq_id, queue_size);
	sq->nvmet_sq.ctrl = ctrl->target_ctrl;

	/* allocate available requests */
	reqs_num = min_t(unsigned int, queue_size, 1024);
	if (al_nvme_reqs_lifo_init(&sq->avail_reqs, reqs_num))
	{
		printk("%s: al_nvme_reqs_lifo_init alloc failed\n", __func__);
		goto nomem_err;
	}

	/* allocate avail commands */
	/* TODO size should be smaller? */
	if (al_nvme_bufs_lifo_init(&sq->avail_cmds, reqs_num))
	{
		printk("%s: al_nvme_bufs_lifo_init alloc failed\n", __func__);
		goto nomem_err;
	}

	for (i = 0; i < sq->avail_cmds.size; ++i) {
		/* TODO use cache for these buffers */
		sq->avail_cmds.bufs[i] =
				al_nvme_zalloc(sizeof(struct al_shared_buf));

		if (!sq->avail_cmds.bufs[i])
		{
			printk("%s: avail_cmds.bufs alloc failed\n", __func__);
			goto nomem_err;
		}
		buf = sq->avail_cmds.bufs[i];

		buf->buf = al_nvme_zalloc(sizeof(struct nvme_command) *
					  AL_NVME_CTRL_BATCH_SIZE);
		if (!buf->buf)
		{
			printk("%s: buf->buf alloc failed\n", __func__);
			goto nomem_err;
		}
	}

	/* preallocate requests in the ring */
	for (i = 0; i < sq->avail_reqs.size; ++i) {
		sq->avail_reqs.reqs[i] = al_nvme_cache_alloc(ctrl->reqs_cache);

		if (!sq->avail_reqs.reqs[i])
		{
			printk("%s: sq->avail_reqs.reqs failed\n", __func__);
			goto nomem_err;
		}
		req = sq->avail_reqs.reqs[i];
		memset(req, 0, sizeof(*req));

		req->sq = sq;
		req->sq_id = sq_id;
		req->cq_id = cq_id;
		req->reqs_lifo = &sq->avail_reqs;
		req->cmds_lifo = &sq->avail_cmds;
		req->ctrl = ctrl;
		req->cq = ctrl->cq[cq_id];
		req->sqe_size_2n = ctrl->params.sqe_size_2n;
		req->cqe_size_2n = ctrl->params.cqe_size_2n;
		req->in_host_mem = in_host_mem;
		req->prp_list.buf = kzalloc(4096, GFP_KERNEL);
		if (!req->prp_list.buf)
			goto nomem_err;
		req->prp_list.dma_addr = virt_to_phys(req->prp_list.buf);

		req->target_req.rsp = al_nvme_zalloc(sizeof(struct nvme_completion));
		if (!req->target_req.rsp)
		{
			printk("req->target_req.rsp is a NULL pointer\n");
			return -1;
		}

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		if (sq_id != 0) {
			req->rw_buf = kmalloc(1 << 20, GFP_KERNEL);
			if (!req->rw_buf) {
				printk("%s,%d: kmalloc err\n", __func__, __LINE__);
				goto nomem_err;
			}
			memset(&req->rw_sgl, 0, sizeof(struct scatterlist));
			sg_init_table(&req->rw_sgl, 1);
			sg_set_buf(&req->rw_sgl, req->rw_buf, 1 << 20);
		}
#endif

	}

	printk("NVMe controller %d: created new sq (%d): cqid %d, qs %d, in_host_mem %d, "
		"ring size %d, host 0x%llx\n",
		ctrl->ctrl_id, sq_id, cq_id, queue_size, in_host_mem, sq->avail_reqs.size,
		sq->base_addr);

	return 0;

nomem_err:
	_free_sq(ctrl, sq_id);
	return -ENOMEM;
}

int al_nvme_cq_init(struct al_nvme_ctrl *ctrl, u16 cq_id, u32 queue_size,
			u16 int_vector, u64 host_addr, u8 int_enabled)
{
	const int idx = cq_id;
	struct al_nvme_cq *cq;
	unsigned int i;
	struct al_shared_buf *buf;
	phys_addr_t host_mem_base = (phys_addr_t) ctrl->pcie_data->outbound_base_addr;

	/* Queue depth (size) of this queue */
	ctrl->cq[idx] = al_nvme_zalloc(sizeof(struct al_nvme_cq));

	if (!ctrl->cq[idx])
	{
		printk("%s: ctrl->cq[%d] is a NULL pointer\n", __func__, idx);
		return -1;
	}

	cq = ctrl->cq[idx];
	cq->ctrl = ctrl;
	cq->cq_id = cq_id;
	cq->queue_size = queue_size;
	cq->int_vector = int_vector;
	cq->host_addr = host_addr;
	cq->host_addr_phys = host_addr + host_mem_base;
	cq->mapped_host_addr =
				ioremap_nocache(host_addr + host_mem_base,
				queue_size * sizeof(struct nvme_completion));
	cq->int_enabled = int_enabled;
	cq->phase = 1;

	nvmet_cq_setup(ctrl->target_ctrl, &cq->nvmet_cq, cq_id, queue_size);

	/* allocate avail comps */
	/* TODO choose a more suited size */
	if (al_nvme_bufs_lifo_init(&cq->avail_comps, 1024))
	{
		printk("%s, al_nvme_bufs_lifo_init alloc failed\n", __func__);
		goto nomem_err;
	}
	for (i = 0; i < cq->avail_comps.size; ++i) {
		/* TODO use cache for these buffers */
		cq->avail_comps.bufs[i] =
				al_nvme_zalloc(sizeof(struct al_shared_buf));

		if (!cq->avail_comps.bufs[i])
		{
			printk("%s, avail_comps.bufs alloc failed\n", __func__);
			goto nomem_err;
		}
		buf = cq->avail_comps.bufs[i];
		buf->buf = al_nvme_zalloc(sizeof(struct nvme_completion) *
					 AL_NVME_CTRL_BATCH_SIZE);
		//buf->buf = al_nvme_cache_alloc(ctrl->comps_cache);

		if (!buf->buf)
		{
			printk("%s, buf->buf alloc failed\n", __func__);
			goto nomem_err;
		}
	}

	printk("NVMe controller %d: created new cq (%d): qs %d, int_enabled %d, vec 0x%x, host 0x%llx\n",
		ctrl->ctrl_id, cq_id, queue_size, int_enabled, int_vector, cq->host_addr);

	return 0;

nomem_err:
	_free_cq(ctrl, cq_id);
	return -ENOMEM;
}

static void _nvme_start_ctrl(struct work_struct *work)
{
	struct al_nvme_ctrl *ctrl =
		container_of(work, struct al_nvme_ctrl, start_ctrl_work);

	struct NvmeBar *ctrl_regs = (struct NvmeBar *)ctrl->pcie_data->inbound_base_addr_bar0;
	u32 page_bits, page_size, sq_queue_size, cq_queue_size;
	unsigned long flags;

	if (ctrl->ctrl_id == 0)
		ep0_error_handling = false;

	if (ctrl->ctrl_id == 1)
		ep1_error_handling = false;

	/* update the controller register and doorbell register */
	spin_lock_irqsave(&ctrl->ctrl_regs_lock, flags);
	ctrl_regs->csts = 0;
	ctrl->target_ctrl->csts = 0;

	page_bits = QNAP_NVME_CC_MPS(ctrl_regs->cc) + 12; // 0+12=12
	page_size = 1 << page_bits;                       // memory page size = 4K
	sq_queue_size = QNAP_NVME_AQA_ASQS(ctrl_regs->aqa) + 1;
	cq_queue_size = QNAP_NVME_AQA_ACQS(ctrl_regs->aqa) + 1;

	/* TODO lots of checks should be done here-
	 * check all parameters, make sure they are legit.
	 */
	if(unlikely(ctrl->cq[0])) {
		printk("%s: admin cq is NOT NULL!!\n", __func__);
		goto error;
	}
	if(unlikely(ctrl->sq[0])) {
		printk("%s: admin sq is NOT NULL!!\n", __func__);
		goto error;
	}
	if(unlikely(!ctrl_regs->asq)) {
		printk("%s: asq no address!!\n", __func__);
		goto error;
	}
	if(unlikely(!ctrl_regs->acq)) {
		printk("%s: acq no addrss!!\n", __func__);
		goto error;
	}
	if(unlikely(ctrl_regs->asq & (page_size - 1))) {
		printk("%s: asq!!\n", __func__);
		goto error;
	}
	if(unlikely(ctrl_regs->acq & (page_size - 1))) {
		printk("%s: acq!!\n", __func__);
		goto error;
	}
	if(unlikely(QNAP_NVME_CC_MPS(ctrl_regs->cc) < QNAP_NVME_CAP_MPSMIN(ctrl_regs->cap))) {
		printk("%s: MPS < MPSMIN!!\n", __func__);
		goto error;
	}
	if(unlikely(QNAP_NVME_CC_MPS(ctrl_regs->cc) > QNAP_NVME_CAP_MPSMAX(ctrl_regs->cap))) {
		printk("%s: MPS > MPSMAX!!\n", __func__);
		goto error;
	}
	if(unlikely(!QNAP_NVME_AQA_ASQS(ctrl_regs->aqa) || QNAP_NVME_AQA_ASQS(ctrl_regs->aqa) > 4095)) {
		printk("%s: ASQS is zero or larger than 4095!!\n", __func__);
		goto error;
	}
	if(unlikely(!QNAP_NVME_AQA_ACQS(ctrl_regs->aqa) || QNAP_NVME_AQA_ACQS(ctrl_regs->aqa) > 4095)) {
		printk("%s: ACQS is zero or larger than 4095!!\n", __func__);
		goto error;
	}

	ctrl->params.page_bits = page_bits;
	ctrl->params.page_size = page_size;

	/* TODO this is weird.. IOSQES and IOCQES cannot be set before IDENTIFY
	 * is called (in IDENTIFY the controller declares required/max values)
	 * BUT- the Linux driver does set it before ever calling
	 * IDENTIFY... look into it.
	 */
	//ctrl->params.sqe_size_2n = QNAP_NVME_CC_IOSQES(ctrl_regs->cc);    // 6 -> 2^6=64 Bytes
	//ctrl->params.cqe_size_2n = QNAP_NVME_CC_IOCQES(ctrl_regs->cc);    // 4 -> 2^4=16 Bytes

	/* Fix the NVMe command size here
	 * Windows driver may not config this value sometimes.
	 */
	ctrl->params.sqe_size_2n = 6;
	ctrl->params.cqe_size_2n = 4;

	printk("%s: page_size = %d, ASQS = %d, ACQS = %d, sqe = %d, cqe = %d\n",
				__func__, page_size, sq_queue_size, cq_queue_size, ctrl->params.sqe_size_2n, ctrl->params.cqe_size_2n);

	ctrl->params.cmb_szu = QNAP_NVME_CMBSZ_SZU(ctrl_regs->cmbsz);

	if (al_nvme_cq_init(ctrl, 0, cq_queue_size, 0, ctrl_regs->acq, 1) ||
		al_nvme_sq_init(ctrl, 0, 0, sq_queue_size, true, ctrl_regs->asq)) {
		pr_err("%s: Could not create Admin SQ/CQ\n", __func__);
		ctrl_regs->csts = QNAP_NVME_CSTS_FAILED;
		goto error;
	}
	spin_unlock_irqrestore(&ctrl->ctrl_regs_lock, flags);

	/*
	 * create doorbell polling thread.
	 */
	nvme_db_thread_create(ctrl);

	spin_lock_irqsave(&ctrl->ctrl_regs_lock, flags);
	/*
	 * Set Controller Status Ready.
	 */
	if (ctrl->target_ctrl->subsys) {
		ctrl_regs->csts |= QNAP_NVME_CSTS_READY;
		ctrl->target_ctrl->csts = ctrl_regs->csts;

		printk("%s: NVMe Controller %d Started!\n", __func__, ctrl->ctrl_id);
	}

error:
	spin_unlock_irqrestore(&ctrl->ctrl_regs_lock, flags);
}

static void _nvme_stop_ctrl(struct work_struct *work)
{
	struct al_nvme_ctrl *ctrl =
		container_of(work, struct al_nvme_ctrl, stop_ctrl_work);
	struct NvmeBar *ctrl_regs = (struct NvmeBar *)ctrl->pcie_data->inbound_base_addr_bar0;
	void __iomem *ptr = (void *) ctrl->pcie_data->inbound_base_addr_bar0;
	void __iomem *dbs = ptr + NVME_REG_DBS;
	unsigned long flags;
	int i = 0;
	struct al_nvme_tm_context *ctx;
	const unsigned int first_cpu = ctrl->first_cpu;
	const unsigned int last_cpu = ctrl->last_cpu;

	/* TODO implement me */
	/* QNAP */
	for (i = ctrl->feat_val.num_queues; i >= 1; i--) {
		if (ctrl->sq[i] != NULL) {
			printk("%s: NVMe Controller %d: Dele SQ id %d\n", __func__, ctrl->ctrl_id, i);
			_free_sq(ctrl, i);
		}
	}
	for (i = ctrl->feat_val.num_queues; i >= 1; i--) {
		if (ctrl->cq[i] != NULL) {
			printk("%s: NVMe Controller %d: Dele CQ id %d\n", __func__, ctrl->ctrl_id, i);
			_free_cq(ctrl, i);
		}
	}

	if (ctrl->sq[0] != NULL) {
		printk("%s: NVMe Controller %d: Dele SQ id 0\n", __func__, ctrl->ctrl_id);
		_free_sq(ctrl, i);
	}
	if (ctrl->cq[0] != NULL) {
		printk("%s: NVMe Controller %d: Dele CQ id 0\n", __func__, ctrl->ctrl_id);
		_free_cq(ctrl, i);
	}

	if (ctrl->db_kthread) {
		kthread_stop(ctrl->db_kthread);
		ctrl->db_kthread = NULL;
	}

	for (i = first_cpu; i <= last_cpu; i++) {
		ctx = &ctrl->tm->cpu_ctx[i];
		atomic_set(&ctx->nvmet_cmd_cnt, 0);
	}

	for (i = 0; i < AL_NVME_QUEUES_MAX; i++) {
		memset(ctrl->cmd_prp_cnt[i], 0, sizeof(int) * 1024);
	}

	spin_lock_irqsave(&ctrl->ctrl_regs_lock, flags);

	memset(dbs, 0, sizeof(unsigned int) * 2 * AL_NVME_QUEUES_MAX);
	ctrl_regs->csts &= ~QNAP_NVME_CSTS_READY;
	ctrl->target_ctrl->csts = ctrl_regs->csts;

	printk("%s: NVMe Controller %d Stopped!\n", __func__, ctrl->ctrl_id);

	if (QNAP_NVME_CC_SHN(ctrl_regs->cc)) {
		ctrl_regs->csts |= QNAP_NVME_CSTS_SHST_COMPLETE;
		ctrl->target_ctrl->csts = ctrl_regs->csts;
		printk("%s: NVMe Controller %d shutdown complete\n", __func__, ctrl->ctrl_id);
	}
	ctrl_regs->cc = 0;
	ctrl->target_ctrl->cc = 0;

	spin_unlock_irqrestore(&ctrl->ctrl_regs_lock, flags);
}

int nvme_ctrl_init(struct al_nvme_ctrl **ctrl, u8 ctrl_id)
{
	int ret = 0, idx;
	struct NvmeBar *ctrl_regs;
	struct al_nvme_ctrl *_ctrl = al_nvme_zalloc(sizeof(struct al_nvme_ctrl));
	struct qnap_ep_device ep;

	if (!_ctrl) {
		ret = -ENOMEM;
		goto error;
	}

	_ctrl->pcie_data = al_nvme_malloc(sizeof(struct qnap_ep_device));

	qnap_ep_pcie_port_list_parser_fun(ctrl_id, &ep);
	memcpy(_ctrl->pcie_data, &ep, sizeof(struct qnap_ep_device));

	printk("NVMe Controller %d: Bar0 Base Addr=0x%llx, Host Base Addr=0x%llx\n",
				ctrl_id,
				virt_to_phys(_ctrl->pcie_data->inbound_base_addr_bar0),
				_ctrl->pcie_data->outbound_base_addr);

	ctrl_regs = (struct NvmeBar *)_ctrl->pcie_data->inbound_base_addr_bar0;

	/* we only support one namespace for now */
	_ctrl->ns_num = 2;
	_ctrl->feat_val.temp_thresh = AL_NVME_TEMPERATURE + 10;
	_ctrl->feat_val.async_config = 0x100;
	_ctrl->ctrl_id = ctrl_id;

	spin_lock_init(&_ctrl->ctrl_regs_lock);
	spin_lock_init(&_ctrl->bar_regs_lock);

	_ctrl->ctrl_wq = alloc_ordered_workqueue("al_nvme%02d_wq", 0, ctrl_id);

	if (!_ctrl->ctrl_wq) {
		ret = -ENOMEM;
		goto error;
	}

	INIT_WORK(&_ctrl->start_ctrl_work, _nvme_start_ctrl);
	INIT_WORK(&_ctrl->stop_ctrl_work, _nvme_stop_ctrl);

	if(qnap_ep_uboot_init_fun(ctrl_id) == AL_FALSE) {
		/* Controller Capabilities */
		ctrl_regs->cap = 0;
		/* Maximum Queue Entries Supported (I/O)  */
		QNAP_NVME_CAP_SET_MQES(ctrl_regs->cap, 16383);
		/* Contiguous Queues Required             */
		QNAP_NVME_CAP_SET_CQR(ctrl_regs->cap, 1);
		/* Arbitration Mechanism Supported        */
		QNAP_NVME_CAP_SET_AMS(ctrl_regs->cap, 0);
		/* Timeout 500 ms/unit                    */
		QNAP_NVME_CAP_SET_TO(ctrl_regs->cap, 0xFF);
		/* Doorbell Stride                        */
		QNAP_NVME_CAP_SET_DSTRD(ctrl_regs->cap, 0);
		/* NVM Subsystem Reset Supported          */
		QNAP_NVME_CAP_SET_NSSRS(ctrl_regs->cap, 0);
		/* Command Sets Supported                 */
		QNAP_NVME_CAP_SET_CSS(ctrl_regs->cap, 1);
		/* Memory Page Size Minimum 2^(12+MPSMAX) */
		QNAP_NVME_CAP_SET_MPSMIN(ctrl_regs->cap, 0);
		/* Memory Page Size Maximum 2^(12+MPSMAX) */
		QNAP_NVME_CAP_SET_MPSMAX(ctrl_regs->cap, 0);

		/* Version */
		ctrl_regs->vs = 0x00010300;

#ifdef CONFIG_NVME_CMB_SUPPORT
		/* Controller Memory Buffer Location      */
		/* Base Indicator Register                */
		QNAP_NVME_CMBLOC_SET_BIR(ctrl_regs->cmbloc, QNAP_NVME_CMB_BAR);
		/* Offset                                 */
		QNAP_NVME_CMBLOC_SET_OFST(ctrl_regs->cmbloc, QNAP_NVME_CMB_OFST_SZU);

		/* Controller Memory Buffer Size          */
		/* The SZ is in multiples of the SZU      */
		QNAP_NVME_CMBSZ_SET_SZ(ctrl_regs->cmbsz, QNAP_NVME_CMB_MULTI_SZU);
		/* Size Units: 0x0 -> 4k                  */
		QNAP_NVME_CMBSZ_SET_SZU(ctrl_regs->cmbsz, QNAP_NVME_CMB_SZU_4K);
		/* Submission Queue Support               */
		QNAP_NVME_CMBSZ_SET_SQS(ctrl_regs->cmbsz, 1);
		/* Completion Queue Support               */
		QNAP_NVME_CMBSZ_SET_CQS(ctrl_regs->cmbsz, 0);
		/* PRP SGL List Support                   */
		QNAP_NVME_CMBSZ_SET_LISTS(ctrl_regs->cmbsz, 0);
		/* Read Data Support                      */
		QNAP_NVME_CMBSZ_SET_RDS(ctrl_regs->cmbsz, 0);
		/* Write Data Support                     */
		QNAP_NVME_CMBSZ_SET_WDS(ctrl_regs->cmbsz, 0);
#endif
	}

	printk("%s: initialize task manager\n", __func__);
	ret = initialize_task_manager(_ctrl);
	if (ret)
		goto error;

	printk("%s: allocate target controller\n", __func__);
	ret = allocate_ctrl(_ctrl);
	if (ret)
		goto error;

	printk("%s: allocate sqs and cqs\n", __func__);
	ret = allocate_queues(_ctrl, AL_NVME_QUEUES_MAX, AL_NVME_QUEUES_MAX);
	if (ret)
		goto error;

	printk("%s: initialize identify structure\n", __func__);
	ret = al_nvme_admin_init(_ctrl);
	if (ret)
		goto error;

	printk("%s: allocate requests cache\n", __func__);
	_ctrl->reqs_cache = al_nvme_cache_create("al_nvme_reqs", sizeof(struct al_nvme_req));
	if (!_ctrl->reqs_cache) {
		ret = -ENOMEM;
		goto error;
	}
#ifdef CONFIG_NVME_RAM_TEST
	printk("%s: allocate data buffer\n", __func__);
	ret = allocate_data_buffer(_ctrl);
	if (ret)
		goto error;
#endif

	printk("%s: task manager start\n", __func__);
	ret = al_nvme_tm_start(_ctrl->tm, _ctrl->ctrl_id);
	if (ret)
		goto error;

	printk("%s: NVMe controller register polling start\n", __func__);
	ret = nvme_bar_thread_create(_ctrl);
	if (ret)
		goto error;

	debug_1 = al_nvme_zalloc(sizeof(*debug_1));
	if (IS_ERR(debug_1))
		goto error;
	INIT_LIST_HEAD(&debug_1->list);

	if(qnap_ep_uboot_init_fun(ctrl_id) == AL_FALSE) {
		printk("ep %d port : off crs\n", ctrl_id);
		qnap_ep_rcs_fun(ctrl_id, AL_FALSE);
	}

	*ctrl = _ctrl;

	return 0;
error:
	nvme_ctrl_free(&_ctrl);
	return ret;
}

static void nvmet_pcie_remove_port(struct nvmet_port *port)
{
	struct al_nvme_ctrl *ctrl = port->priv;

	list_del(&ctrl->target_ctrl->subsys_entry);
	ctrl->pcie_port = NULL;

	printk("%s: Removing PCIe Port %d!\n", __func__, port->disc_addr.portid);
}

static int nvmet_pcie_add_port(struct nvmet_port *port)
{
	struct nvmet_subsys_link *p;
	__le16 port_id = port->disc_addr.portid;

	nvme_ctrl[port_id-1]->pcie_port = port;

	if (nvme_ctrl[port_id-1]->pcie_port->disc_addr.trtype != NVMF_TRTYPE_PCIE) {
		printk("%s: trtype=%d is NOT PCIe!\n", __func__, port->disc_addr.trtype);
	}

	p = list_first_entry(&nvme_ctrl[port_id-1]->pcie_port->subsystems, typeof(*p), entry);
	nvme_ctrl[port_id-1]->target_ctrl->subsys = p->subsys;

	list_add_tail(&nvme_ctrl[port_id-1]->target_ctrl->subsys_entry, &p->subsys->ctrls);

	port->priv = nvme_ctrl[port_id-1];
	printk("%s: Enabling PCIe Port %d !\n", __func__, port->disc_addr.portid);

	return 0;
}

static int qnap_ep_cable_event(struct qnap_ep_event *event)
{
	int ep_port_counts = 0;
	struct al_nvme_ctrl *ctrl = NULL;
	struct NvmeBar *ctrl_regs = NULL;;
	unsigned int port = event->port_num;

	ep_port_counts = qnap_ep_get_port_count_fun();

	if (port > ep_port_counts - 1) {
		printk("port: %d, ep port counts: %d NOT match !!\n", port, ep_port_counts);
		return -1;
	}

	ctrl = nvme_ctrl[port];
	ctrl_regs = (struct NvmeBar *) ctrl->pcie_data->inbound_base_addr_bar0;

	printk("%s: Port %d: Event Type 0x%x\n", __func__, port, event->type);

	switch (event->type) {
	case CPLD_EVENT_CABLE_USB_INSERT:
//		qnap_ep_ready_set_low(port);
		printk("%s: USB Insert!\n", __func__);
		break;
	case CPLD_EVENT_CABLE_USB_REMOVED:
		printk("%s: USB Removed!\n", __func__);
		queue_work(ctrl->ctrl_wq, &ctrl->stop_ctrl_work);
		break;
	case CPLD_EVENT_CABLE_TBT_INSERT:
		printk("%s: TBT Insert!\n", __func__);
		break;
	case CPLD_EVENT_CABLE_TBT_REMOVED:
		printk("%s: TBT Removed!\n", __func__);
		queue_work(ctrl->ctrl_wq, &ctrl->stop_ctrl_work);
		break;
	default:
		printk("Unkonw event type !!\n");
		return -1;
	}

	return 0;
}

static struct nvmet_fabrics_ops nvmet_pcie_ops = {
	.owner		= THIS_MODULE,
	.type		= NVMF_TRTYPE_PCIE,
	.add_port	= nvmet_pcie_add_port,
	.remove_port	= nvmet_pcie_remove_port,
	.queue_response = nvmet_pcie_queue_response,
};

static struct qnap_ep_fabrics_ops qnap_ep_ops = {
	.owner			= THIS_MODULE,
	.type			= QNAP_EP_CALLBACK,
	.callback_response	= qnap_ep_cable_event,
};

static void _nvme_subsystem_free(void)
{
	int i, ep_port_counts = 0;

	ep_port_counts = qnap_ep_get_port_count_fun();
	for (i = 0; i < ep_port_counts; i++) {
		if (nvme_ctrl && &nvme_ctrl[i])
			nvme_ctrl_free(&nvme_ctrl[i]);
	}
	kfree(nvme_ctrl);
}

static int __init nvmet_pcie_init(void)
{
	int ret = 0, i;
	/**********************************************************
	 * Debug
	 */
	struct proc_dir_entry *mode, *rdy;
	int ep_port_counts = 0;

	proc_qnap_nvme_ep_root = proc_mkdir("qnapepnvme", NULL);
	if(!proc_qnap_nvme_ep_root) {
		printk(KERN_ERR "Couldn't create qnapep folder in procfs\n");
		return -1;
	}
	/* create file of folder in procfs
	 */
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_nvme_ep_root, &qnap_nvme_ep_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnapepnvme procfs node\n");
		return -1;
	}
	rdy = proc_create("rdy", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_nvme_ep_root, &qnap_nvme_ep_rdy_proc_fileops);
	if(!rdy) {
		printk(KERN_ERR "Couldn't create qnapepnvme procfs rdy node\n");
		return -1;
	}

	/**********************************************************/
	ret = nvmet_register_transport(&nvmet_pcie_ops);
	if (ret)
		goto done;

	ret = qnap_ep_register(&qnap_ep_ops);
	if (ret) {
		printk("%s: QNAP EP register failed\n", __func__);
		goto done;
	}

	ep_port_counts = qnap_ep_get_port_count_fun();

	if (ep_port_counts <= 0) {
		printk("NVME EP port counts : %d\n", ep_port_counts);
		goto done;
	}

	printk("NVMe support EP port counts : %d\n", ep_port_counts);
	nvme_ctrl = al_nvme_calloc(ep_port_counts, sizeof(struct al_nvme_ctrl *));
	if (!nvme_ctrl)
		goto done;

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		printk("++++ rw_buf enable +++\n");
#else
		printk("++++ rw_buf disable +++\n");
#endif

	for (i = 0; i < ep_port_counts; i++) {
		ret = nvme_ctrl_init(&nvme_ctrl[i], i);

		if (ret) {
			pr_err("%s: failed to initialize controller(%d) (%d)\n",
					__func__, i, ret);
			goto err;
		}
	}

	goto done;
err:
	_nvme_subsystem_free();
done:
	return ret;
}

static void __exit nvmet_pcie_exit(void)
{
	qnap_ep_unregister(&qnap_ep_ops);
	nvmet_unregister_transport(&nvmet_pcie_ops);
	_nvme_subsystem_free();

	return;
}

module_init(nvmet_pcie_init);
module_exit(nvmet_pcie_exit);
MODULE_LICENSE("GPL");
