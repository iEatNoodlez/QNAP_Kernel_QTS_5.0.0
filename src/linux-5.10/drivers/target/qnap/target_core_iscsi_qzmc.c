/*******************************************************************************
 * Filename:  
 * @file        target_core_iscsi_qzmc.c
 * @brief       
 *
 * @author      Adam Hsu
 * @date        2018/XX/YY
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
 ****************************************************************************/

#include <linux/net.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/in.h>
#include <linux/cdrom.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/vmalloc.h>
#include <linux/device-mapper.h>
#include <linux/parser.h>
#include <asm/unaligned.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>

#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_backend.h>

#include "../target_core_iblock.h"
#include "../target_core_file.h"

#include "../iscsi/qnap/iscsi_target_tcp_qzmc.h"
#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qzmc.h"
#include "target_core_iscsi_qzmc.h"

/**/
static int __qnap_iscsit_tcp_zmc_backend_ats_callback(
	struct se_cmd *cmd, struct scatterlist *sgl, u32 sgl_nents,
	bool success, int *post_ret,
	int (*backend_ats_cb_write)(struct se_cmd *cmd, 
			loff_t write_pos, u8 *write_buf, int write_len));

static int __qnap_iscsit_tcp_zmc_kmap_data_sg(struct se_cmd *cmd, u32 len, 
	void **vmap);

static int __qnap_iscsit_tcp_zmc_kunmap_data_sg(struct se_cmd *cmd);

/**/
#if 0
static u8 hex[] = {
	'0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};


static void zmc_dump_mem(
	u8 *buf,
	size_t dump_size
	)
{
	u8 *Data, Val[50], Str[20], c;
	size_t Size, Index;

	Data = buf;

	while (dump_size) {
		Size = 16;
		if (Size > dump_size)
			Size = dump_size;

		for (Index = 0; Index < Size; Index += 1) {
			c = Data[Index];
			Val[Index * 3 + 0] = hex[c >> 4];
			Val[Index * 3 + 1] = hex[c & 0xF];
			Val[Index * 3 + 2] = (u8) ((Index == 7) ? '-' : ' ');
			Str[Index] = (u8) ((c < ' ' || c > 'z') ? '.' : c);
		}

		Val[Index * 3] = 0;
		Str[Index] = 0;
		pr_info("addr-0x%p: %s *%s*\n",Data, Val, Str);
		Data += Size;
		dump_size -= Size;
	}
	return;
}

void qnap_target_zmc_dump_sg_list(
	struct scatterlist *start_sg,
	int sg_nents,
	int sg_total_check_len,
	int data_len
	)
{
	struct scatterlist *sg;
	int i, len = 0;

	pr_info("start\n");
	for_each_sg(start_sg, sg, sg_nents, i) {
		ISCSI_ZMC_DEBUG("sg:%p, page:%p, len:0x%x, off:0x%x\n", 
			sg, sg_page(sg), sg->length, sg->offset);
		pr_info("sg:%p, page:%p, len:0x%x, off:0x%x\n", 
			sg, sg_page(sg), sg->length, sg->offset);

		len += sg->length;
	}
	pr_info("end\n");

}
EXPORT_SYMBOL(qnap_target_zmc_dump_sg_list);
#endif

/**/
static void *__qnap_iscsit_tcp_zmc_core_kmap_data_sg(
	struct scatterlist *sgl,
	int sgl_nents,
	int len
	)
{
	/* refer from transport_kmap_data_sg() and se_cmd->zmc_wsgl MUST/NEED
	 * to be prepread already when to use this call
	 */
	struct scatterlist *sg;
	int i, copied;
	u8 *src = NULL, *dest = NULL, *buf = NULL;

	if (!sgl || !sgl_nents)
		return NULL;

	if (sgl_nents == 1)
		return kmap(sg_page(sgl)) + sgl->offset;

	/* >1 page. can NOT use vmap due to our pages in sgl may not
	 * be full size (PAGE_SIZE) we need to do copy one by one ...
	 */
	dest = buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return NULL;

	for_each_sg(sgl, sg, sgl_nents, i) {
		src = kmap(sg_page(sg)) + sg->offset;
		if (!src) 
			goto fail;
		/* the len may be smaller than your sg length ... */
		copied = min_t(int, len, sg->length);
		memcpy(dest, src, copied);
		kunmap(sg_page(sg));
		dest += copied;
		len -= copied;
		if (!len)
			break;
	}
	return buf;
fail:
	kfree(buf);
	return NULL;

}

static void __qnap_iscsit_tcp_zmc_core_kunmap_data_sg(
	struct scatterlist *sgl,
	int sgl_nents,
	void *data_vmap
	)
{
	if (!sgl || !sgl_nents)
		return;

	if (sgl_nents == 1) {
		kunmap(sg_page(sgl));
		return;
	}

	kfree(data_vmap);

}

static int __qnap_ib_ats_cb_write(
	struct se_cmd *cmd,
	loff_t write_pos,
	u8 *write_buf,
	int write_len
	)
{
	struct __rw_task task;
	struct scatterlist page_sg;
	void *page_buf = NULL;
	struct page *page = NULL;
	int ret, done_blks = 0, tcm_rc;	

	memset(&task, 0, sizeof(struct __rw_task));

	ret = qnap_transport_create_devinfo(cmd, &task.dev_info);
	if (ret != 0) {
		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		ret = -EINVAL;
		goto fail;
	}

	/* currently, linux supports one sector for ATS command .. so ... */
	WARN_ON(write_len != (1 << task.dev_info.bs_order));

	ret = -ENOMEM;
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		goto fail;
	}

	page_buf = kmap(page);
	if (!page_buf) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		goto fail;
	}

	memcpy(page_buf, write_buf, write_len);
	kunmap(page);

	sg_init_table(&page_sg, 1);
	sg_set_page(&page_sg, page, write_len, 0);

	/* prepare write task */
	task.sg_list = &page_sg;
	task.sg_nents = 1;
	task.nr_bytes = write_len;
	task.lba = (write_pos >> 9);
	task.bs_order = task.dev_info.bs_order;
	task.dir = DMA_TO_DEVICE;
	task.ret = 0;
	task.async_io = false;

	done_blks = qlib_blockio_rw(&task);
	ret = task.ret;

	WARN_ON((done_blks != (task.nr_bytes >> task.bs_order)));

	if (!ret)
		tcm_rc = TCM_NO_SENSE;
	else if (ret == -ENOSPC)
		tcm_rc = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
	else
		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

fail:
	if (page)
		__free_page(page);

	cmd->cmd_dr.work_io_err = tcm_rc;
	return ret;

}


static int __qnap_fio_ats_cb_write(
	struct se_cmd *cmd,
	loff_t write_pos, 
	u8 *write_buf, 
	int write_len
	)
{
	struct se_device *dev = cmd->se_dev;
	struct fd_dev *fd_dev = dev->transport->get_dev(dev);
	struct iov_iter iter;
	struct kvec k_iov;
	loff_t old_pos = write_pos;	
	int sync_ret;	
	int ret, tmp_ret, tcm_rc;	
	bool is_thin_dev = qlib_thin_lun(&dev->dev_dr);

	if(!fd_dev->fd_file) {
		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;		
		ret = -ENODEV;
		goto out;
	}

	k_iov.iov_base = write_buf;
	k_iov.iov_len = write_len;
	iov_iter_kvec(&iter, WRITE, &k_iov, 1, write_len);

	ret = vfs_iter_write(fd_dev->fd_file, &iter, &write_pos, 0);

	if (is_thin_dev && (ret > 0)) {

		/* we did write for write back behavior, so to go this way if it
		 * is thin lun, write operatin to thin lun will be sync i/o if
		 * occupied data size hits the sync i/o threshold of pool
		 */
		tmp_ret = ret;
		qnap_transport_fd_upadte_write_result(cmd, &tmp_ret);
		if (tmp_ret != ret) {
			ret = tmp_ret;
			if (ret == -ENOSPC) {
				ISCSI_TCP_ZMC_WARN("%s: space was full already\n", __func__);
				tcm_rc = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
			} else {
				tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
				ret = -EINVAL;
			}
			goto out;			
		} else {
			if (!qnap_transport_check_cmd_hit_thin_threshold(cmd)) {
				tcm_rc = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
				ret = -EINVAL;
				goto out;
			}
		}
	}

	if (ret < 0 || (ret != write_len)) {
		ISCSI_TCP_ZMC_ERR("vfs_iter_write() returned %zd for "
				"ATS callback\n", ret);

		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		ret = -EINVAL;
	} else {
		tcm_rc = TCM_NO_SENSE;
		ret = 0;
	}
out:
	cmd->cmd_dr.work_io_err = tcm_rc;
	return ret;

}

static int __qnap_iscsit_tcp_zmc_backend_ats_callback(
	struct se_cmd *cmd, 
	struct scatterlist *sgl,
	u32 sgl_nents,
	bool success,
	int *post_ret,
	int (*backend_ats_cb_write)(struct se_cmd *cmd, 
			loff_t write_pos, u8 *write_buf, int write_len)
	)
{
	/* refer from compare_and_write_callback() */
	struct se_device *dev = cmd->se_dev;
	struct scatterlist *sg;
	unsigned char *buf = NULL, *addr;
	unsigned int offset = 0, len;
	unsigned int nlbas = cmd->t_task_nolb;
	unsigned int block_size = dev->dev_attrib.block_size;
	unsigned int compare_len, write_len;
	loff_t pos;
	int i, tcm_rc = TCM_NO_SENSE, ret = 0;
	
	compare_len = write_len = (nlbas * block_size);

	/* Handle early failure in transport_generic_request_failure(),
	 * which will not have taken ->caw_sem yet..
	 */
	if (!success && (!sgl || !sgl_nents || !cmd->t_bidi_data_sg))
		goto exit;

	/* Handle special case for zero-length COMPARE_AND_WRITE */
	if (!cmd->data_length)
		goto out;

	/* Immediately exit + release dev->caw_sem if command has already
	 * been failed with a non-zero SCSI status.
	 */
	if (cmd->scsi_status) {
		ISCSI_TCP_ZMC_ERR("compare_and_write_callback: "
			"non zero scsi_status: 0x%02x\n", cmd->scsi_status);
		*post_ret = 1;
		if (cmd->scsi_status == SAM_STAT_CHECK_CONDITION)
			tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

		ret = -EINVAL;
		goto out;
	}

	/* step 1: compare the read data
	 * we map buf len to (2 * compare_len), one is for read-verification
	 * and the other is for written data
	 */
	ret = __qnap_iscsit_tcp_zmc_kmap_data_sg(cmd, (compare_len << 1), 
					(void **)&buf);
	if (ret || !buf) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto out;
	}
	 
#warning "todo, need to modify"
	/* Compare against SCSI READ payload against verify payload, 
	 * we can NOT use transport_kmap_data_sg() here
	 */
	for_each_sg(cmd->t_bidi_data_sg, sg, cmd->t_bidi_data_nents, i) {
		addr = (unsigned char *)kmap_atomic(sg_page(sg));
		if (!addr) {
			cmd->cmd_dr.work_io_err = TCM_OUT_OF_RESOURCES;
			ret = -ENOMEM;
			goto out;
		}

		len = min(sg->length, compare_len);

		if (memcmp(addr, buf + offset, len)) {
			ISCSI_TCP_ZMC_WARN("Detected MISCOMPARE at lba:0x%llx, "
				"for addr: %p, buf: %p\n",
				(unsigned long long)cmd->t_task_lba,
				addr, buf + offset);

			kunmap_atomic(addr);
			goto miscompare;
		}
		kunmap_atomic(addr);

		offset += len;
		compare_len -= len;
		if (!compare_len)
			break;
	}

	/* step 2:(TODO)
	 * Tricky method to use vfs write since we set max ATS len to 1 sector
	 * please refer spc_emulate_evpd_b0()
	 */
	pos = cmd->t_task_lba * block_size;

	ret = backend_ats_cb_write(cmd, pos, (buf + write_len), write_len);
	if (!ret)
		*post_ret = 1;

	goto out;

miscompare:
	ISCSI_TCP_ZMC_WARN("Target/%s: Send MISCOMPARE check condition and sense\n",
			dev->transport->name);

	tcm_rc = TCM_MISCOMPARE_VERIFY;
	ret = -EFAULT;
out:
	/*
	 * In the MISCOMPARE or failure case, unlock ->caw_sem obtained in
	 * sbc_compare_and_write() before the original READ I/O submission.
	 */
	up(&dev->caw_sem);

	if (buf)
		__qnap_iscsit_tcp_zmc_kunmap_data_sg(cmd);
exit:
	cmd->cmd_dr.work_io_err = tcm_rc;
	return ret;
}


static int __qnap_iscsit_tcp_zmc_sbc_ib_ats_callback(
	struct se_cmd *cmd, 
	bool success,
	int *post_ret
	)
{
	struct tcp_zmc_cmd_priv *zmc_data = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	WARN_ON(!zmc_data->sgl_in_used);

	return __qnap_iscsit_tcp_zmc_backend_ats_callback(cmd, zmc_data->sgl, 
			zmc_data->sgl_nents, success, post_ret, 
			__qnap_ib_ats_cb_write);
}

static int __qnap_iscsit_tcp_zmc_sbc_fio_ats_callback(
	struct se_cmd *cmd, 
	bool success,
	int *post_ret
	)
{
	struct tcp_zmc_cmd_priv *zmc_data = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	WARN_ON(!zmc_data->sgl_in_used);

	return __qnap_iscsit_tcp_zmc_backend_ats_callback(cmd, zmc_data->sgl,
			zmc_data->sgl_nents, success, post_ret, 
			__qnap_fio_ats_cb_write);
}

static int __qnap_iscsit_tcp_zmc_ib_write_same(
	struct se_cmd *cmd,
	struct scatterlist *sgl,
	u32 sgl_nents,
	void *data
	)
{
	/* refer from iblock_execute_write_same() */
	struct iblock_op_set *op_set = (struct iblock_op_set *)data;
	struct se_device *dev = cmd->se_dev;
	struct iblock_dev *ib_dev = dev->transport->get_dev(dev);
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct page *page = NULL;	
	struct iblock_req *ibr;
	struct bio *bio;
	struct bio_list list;
	struct scatterlist *_sg;
	struct scatterlist work_sg;
	u8 *buf = NULL, *page_buf = NULL;
	u32 work_len = 0, len = 0;	
	int page_size, copy_size, op_flags = 0, i, ret, tcm_rc;
	sector_t sectors = sbc_get_write_same_sectors(cmd);
	sector_t block_lba, new_sectors, min_sectors;

	if (cmd->prot_op) {
		pr_err("WRITE_SAME: Protection information with IBLOCK"
		       " backends not supported for zmc-enabled\n");
		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		ret = -EINVAL;
		goto exit;
	}

	qnap_zmc_iblock_get_write_op_flags(cmd, q, &op_flags);

	block_lba = (cmd->t_task_lba << ilog2(dev->dev_attrib.block_size) >> 9);
	new_sectors = (sectors << ilog2(dev->dev_attrib.block_size) >> 9);

	for_each_sg(sgl, _sg, sgl_nents, i)
		len += _sg->length;

	WARN_ON(len != cmd->se_dev->dev_attrib.block_size);
	ISCSI_TCP_ZMC_DEBUG("ws: sgl_nents:0x%x, len:0x%x\n", sgl_nents, len);

	ret = __qnap_iscsit_tcp_zmc_kmap_data_sg(cmd, 
			cmd->se_dev->dev_attrib.block_size, (void **)&buf);

	if (ret || !buf) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -EINVAL;
		goto exit;
	}
#if 0
	if (qlib_thin_lun(&dev->dev_dr) && !qlib_is_ib_fbdisk_dev(&dev->dev_dr))
	{
		if (qlib_check_zero_buf(buf, len) 
		&& ops->execute_qnap_ws_fast_zero 
		&& qlib_check_support_special_discard() 
		)
		{
			izmcd->ops->kunmap_wsgl(izmcd);			

			rc = ops->execute_qnap_ws_fast_zero(cmd);
			if (rc == TCM_NO_SENSE)
				target_complete_cmd(cmd, SAM_STAT_GOOD);
			return rc;
		}
	}
#endif
	/* step 1:
	 * to create new sg and copy write-same buffer if can't use fast-zero
	 */
	page = alloc_page(GFP_KERNEL);
	if (!page) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto free_buf;
	}

	page_buf = (u8 *)kmap(page);
	if (!page_buf) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto free_buf;
	}

	page_size = PAGE_SIZE;
	while (page_size) {
		copy_size = min_t(int, page_size, dev->dev_attrib.block_size);
		memcpy(page_buf, buf, copy_size);
		page_buf += copy_size;
		page_size -= copy_size;
	}
	kunmap(page);

	/* step 2: to submit io */
	ibr = kzalloc(sizeof(struct iblock_req), GFP_KERNEL);
	if (!ibr) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto free_buf;
	}
	cmd->priv = ibr;

	bio = op_set->ib_get_bio(cmd, block_lba, 1, REQ_OP_WRITE, op_flags);
	if (!bio) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto fail_free_ibr;
	}

	bio_list_init(&list);
	bio_list_add(&list, bio);
	refcount_set(&ibr->pending, 1);

	sg_init_table(&work_sg, 1);
	sg_set_page(&work_sg, page, PAGE_SIZE, 0);

	while (new_sectors) {

		min_sectors = min_t(sector_t, new_sectors, 
					(PAGE_SIZE >> SECTOR_SHIFT));
		work_len = (min_sectors << SECTOR_SHIFT);

		while (bio_add_page(bio, sg_page(&work_sg), work_len, 
				work_sg.offset) != work_len)
		{
			bio = op_set->ib_get_bio(cmd, block_lba, 1, REQ_OP_WRITE, 
						op_flags);
			if (!bio)
				goto fail_put_bios;

			refcount_inc(&ibr->pending);
			bio_list_add(&list, bio);
		}

		/* Always in 512 byte units for Linux/Block */
		block_lba += min_sectors;
		new_sectors -= min_sectors;
	}

	op_set->ib_submit_bio(&list);
	tcm_rc = TCM_NO_SENSE;
	ret = 0;
	goto free_buf;

fail_put_bios:
	while ((bio = bio_list_pop(&list)))
		bio_put(bio);
fail_free_ibr:
	kfree(ibr);
free_buf:
	if (page)
		__free_page(page);
	if (buf)
		__qnap_iscsit_tcp_zmc_kunmap_data_sg(cmd);
exit:
	cmd->cmd_dr.work_io_err = tcm_rc;
	return ret;

}

static int __qnap_iscsit_tcp_zmc_fio_write_same(
	struct se_cmd *cmd,
	struct scatterlist *sgl,
	u32 sgl_nents,
	void *data
	)
{
#define MIN_NOLB	128

	/* refer from fd_execute_write_same() */
	struct se_device *se_dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct fd_dev *fd_dev = se_dev->transport->get_dev(se_dev);
	struct iov_iter iter;
	struct kvec *k_iov = NULL;
	struct scatterlist *sg;
	loff_t pos = cmd->t_task_lba * se_dev->dev_attrib.block_size;
	sector_t nolb = sbc_get_write_same_sectors(cmd), min_nolb;
	u8 *buf = NULL;
	u32 len = 0, i;
	int ret, tmp_ret, tcm_rc;
	bool is_thin_dev = qlib_thin_lun(dr);

	if (!nolb) {
		tcm_rc = TCM_NO_SENSE;
		ret = 0;
		goto exit;
	}

	if (cmd->prot_op) {
		ISCSI_TCP_ZMC_ERR("WRITE_SAME: Protection information with FILEIO"
		       " backends not supported on zmc-enabled\n");
		cmd->cmd_dr.work_io_err = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		return -EINVAL;
	}

	for_each_sg(sgl, sg, sgl_nents, i)
		len += sg->length;

	WARN_ON(len != cmd->se_dev->dev_attrib.block_size);
	ISCSI_TCP_ZMC_DEBUG("ws: sgl_nents:0x%x, len:0x%x\n", sgl_nents, len);

	ret = __qnap_iscsit_tcp_zmc_kmap_data_sg(cmd, 
			cmd->se_dev->dev_attrib.block_size, (void **)&buf);

	if (ret || !buf) {
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto exit;
	}
#if 0
	/* fast-zero only works on thin device (file i/o + block backend) */
	if (qlib_thin_lun(&se_dev->dev_dr) && !qlib_is_fio_blk_dev(&se_dev->dev_dr))
	{
		if (qlib_check_zero_buf(buf, len) 
		&& ops->execute_qnap_ws_fast_zero 
		&& qlib_check_support_special_discard() 
		)
		{
			izmcd->ops->kunmap_wsgl(izmcd);			

			rc = ops->execute_qnap_ws_fast_zero(cmd);
			if (rc == TCM_NO_SENSE)
				target_complete_cmd(cmd, SAM_STAT_GOOD);
			return rc;
		}
	}
#endif
	/* fall-back */

	/* actually, we can't use struct bio_vec due to we can't make sure
	 * whether the data is all in one sg element or not.
	 * and, we may receive a lot of lba requests and cause memory allocation
	 * will be failure. So to limit this.
	 */

	k_iov = kzalloc((MIN_NOLB * sizeof(struct kvec)), GFP_KERNEL);
	if (!k_iov) {
		__qnap_iscsit_tcp_zmc_kunmap_data_sg(cmd);
		tcm_rc = TCM_OUT_OF_RESOURCES;
		ret = -ENOMEM;
		goto exit;
	}

	while (nolb) {
		min_nolb = min_t(sector_t, MIN_NOLB, nolb);
		len = 0;

		for (i = 0; i < min_nolb; i++) {
			k_iov[i].iov_base = buf;
			k_iov[i].iov_len = se_dev->dev_attrib.block_size;
			len += se_dev->dev_attrib.block_size;
		}

		iov_iter_kvec(&iter, WRITE, k_iov, min_nolb, len);
		ret = vfs_iter_write(fd_dev->fd_file, &iter, &pos, 0);

		if (ret < 0 || ret != len)
			break;

		nolb -= min_nolb;
	}

	kfree(k_iov);
	__qnap_iscsit_tcp_zmc_kunmap_data_sg(cmd);

	if (is_thin_dev && (ret > 0)) {

		/* we did write for write back behavior, so to go this way
		 * if it is thin lun, write operatin to thin lun will be
		 * sync i/o if occupied data size hits the sync i/o threshold
		 * of pool. the ret variable is written bytes from vfs_iter_write() 
		 */
		tmp_ret = ret;
		qnap_transport_fd_upadte_write_result(cmd, &tmp_ret);
		if (tmp_ret != ret) {
			ret = tmp_ret;
			if (ret == -ENOSPC) {
				ISCSI_TCP_ZMC_WARN("%s: space was full already\n", 
						__func__);
				tcm_rc = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
			} else {
				tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
				ret = -EINVAL;
			}
			goto exit;
		} else {
			/* after every write operation, we check whether size hits 
			 * the thin threshold or not. If yes, it shall response 
			 * the UA status for 'hit threshold'
			 */
			if (!qnap_transport_check_cmd_hit_thin_threshold(cmd)) {
				tcm_rc = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
				ret = -EINVAL;
				goto exit;
			}
		}
	}

	if (ret < 0 || ret != len) {
		ISCSI_TCP_ZMC_ERR("vfs_iter_write() returned %zd for write same\n", ret);

		tcm_rc = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		ret = -EINVAL;
	} else {
		tcm_rc = TCM_NO_SENSE;
		ret = 0;
	}

exit:
	cmd->cmd_dr.work_io_err = tcm_rc;

	if (!ret)
		target_complete_cmd(cmd, SAM_STAT_GOOD);

	return ret;
}

static int __qnap_iscsit_tcp_zmc_sbc_ib_write(
	struct se_cmd *cmd, 
	struct scatterlist *sgl,
	u32 sgl_nents,
	void *data
	)
{
	/* refer from iblock_execute_rw() and only handle write operation */
	struct iblock_op_set *op_set = (struct iblock_op_set *)data;
	struct se_device *dev = cmd->se_dev;
	struct iblock_dev *ib_dev = dev->transport->get_dev(dev);
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct iblock_req *ibr;
	struct bio *bio, *bio_start;
	struct bio_list list;
	sector_t block_lba;
	unsigned bio_cnt;
	unsigned int sg_len, sg_off;
	struct scatterlist *sg;
	struct bio_sgl_rec *sgl_rec = NULL;
	struct sg_info _sg_info;
	u32 sg_num = sgl_nents, expected_bytes = cmd->data_length;
	int op_flags = 0, i, ret, rec_idx, tcm_rc;
	struct drain_io_data drain_data = {};

	/* task lba may be 4096b, it shall be converted again for linux block layer (512b) */	
	block_lba = ((cmd->t_task_lba << ilog2(dev->dev_attrib.block_size)) >> 9);

	ibr = kzalloc(sizeof(struct iblock_req), GFP_KERNEL);
	if (!ibr) {
		ret = -ENOMEM;
		tcm_rc = TCM_OUT_OF_RESOURCES;
		goto fail;
	}

	cmd->priv = ibr;

	if (!sgl_nents) {
		refcount_set(&ibr->pending, 1);
		op_set->ib_comp_cmd(cmd);
		goto exit;
	}

	sgl_rec = qlib_ib_alloc_bio_sgl_rec(qlib_ib_get_min_nr_vecs(q));
	if (!sgl_rec) {
		ret = -ENOMEM;
		tcm_rc = TCM_OUT_OF_RESOURCES;
		goto free_ibr;
	}

	_sg_info.sg = sgl;
	_sg_info.sg_len = sgl[0].length;
	_sg_info.sg_off = sgl[0].offset;
	_sg_info.sg_nents = sgl_nents; 
	qlib_ib_setup_bio_sgl_rec(ib_dev->ibd_bd, sgl_rec, &_sg_info, expected_bytes);

	qnap_zmc_iblock_get_write_op_flags(cmd, q, &op_flags);

	bio = op_set->ib_get_bio(cmd, block_lba, qlib_ib_get_min_nr_vecs(q), 
					REQ_OP_WRITE, op_flags);
	if (!bio) {
		ret = -ENOMEM;
		tcm_rc = TCM_OUT_OF_RESOURCES;
		goto free_sgl_rec;
	}

	bio_start = bio;
	bio_list_init(&list);
	bio_list_add(&list, bio);
	refcount_set(&ibr->pending, 2);
	bio_cnt = 1;

	if (cmd->cmd_dr.io_ops.pre_do_drain_io) {
		drain_data.data0 = (void *)cmd;
		drain_data.data1 = (void *)bio;
		cmd->cmd_dr.io_ops.pre_do_drain_io(cmd, &drain_data);
	}

	for (rec_idx = 0; ;rec_idx++) {
		if (sgl_rec[rec_idx].total_len == 0)
			break;

		u32 tmp_total = 0, sg_total_len = sgl_rec[rec_idx].total_len;
		sg = sgl_rec[rec_idx].sgl;

		pr_debug("s1: rec_idx:0x%x, sg total:0x%x\n", rec_idx, 
						sg_total_len);

		for (i = 0; i < sgl_rec[rec_idx].sg_nents; i++) {

			pr_debug("s2: i:%d, sg len:0x%x, off:0x%x\n", i, 
					sg[i].length, sg[i].offset);

			ret = bio_add_page(bio, sg_page(&sg[i]), sg[i].length, 
					sg[i].offset);

			if (ret != sg[i].length) {
				WARN_ON(true);
#if 0
				pr_warn(
				"rec_idx:%d, i:%d, ret:0x%x, sg.len:0x%x, "
				"rec total:0x%x, total:0x%x, "
				"bi_vcnt:0x%x, bi max vcent:0x%x, "
				"bio->bi_iter.bi_size:0x%x, "
				"max sectors:0x%x, lba:0x%llx\n", 
				rec_idx, i, ret, sg[i].length, 
				sgl_rec[rec_idx].total_len, total, 
				bio->bi_vcnt, bio->bi_max_vecs,
				bio->bi_iter.bi_size,
				blk_max_size_offset(q, bio->bi_iter.bi_sector, 0),
				(unsigned long long)block_lba);
#endif
			}

			tmp_total += sg[i].length;
			sg_total_len -= sg[i].length;

			if (!sg_total_len) {
				pr_debug("s3: i:%d, tmp_total:0x%x\n", i, 
						tmp_total);
				break;
			}
		}

		/* total must be aligned by 512 bytes */
		WARN_ON(tmp_total & ((1 << 9) - 1));
		WARN_ON(tmp_total != sgl_rec[rec_idx].total_len);

		block_lba += (tmp_total >> 9);
		expected_bytes -= tmp_total;
		if (!expected_bytes)
			break;

		bio = op_set->ib_get_bio(cmd, block_lba, qlib_ib_get_min_nr_vecs(q),
						REQ_OP_WRITE, op_flags);
		if (!bio) {
			ret = -ENOMEM;
			tcm_rc = TCM_OUT_OF_RESOURCES;
			goto put_bios;
		}

		refcount_inc(&ibr->pending);
		bio_list_add(&list, bio);
		bio_cnt++;

		if (cmd->cmd_dr.io_ops.pre_do_drain_io) {
			drain_data.data0 = (void *)cmd;
			drain_data.data1 = (void *)bio;
			cmd->cmd_dr.io_ops.pre_do_drain_io(cmd, &drain_data);
		}
	}

	WARN_ON(expected_bytes);

	qlib_ib_free_bio_sgl_rec(&sgl_rec);

	op_set->ib_submit_bio(&list);
	op_set->ib_comp_cmd(cmd);
exit:
	cmd->cmd_dr.work_io_err = TCM_NO_SENSE;
	return 0;

put_bios:
	while ((bio = bio_list_pop(&list)))
		bio_put(bio);

free_sgl_rec:
	qlib_ib_free_bio_sgl_rec(&sgl_rec);

free_ibr:
	kfree(ibr);
fail:
	cmd->cmd_dr.work_io_err = tcm_rc;
	return ret;

}

static void __qnap_iscsit_tcp_zmc_sbc_fio_aio_write_cb(
	void *priv,
	int err,
	int done_blks
	)
{
	struct se_cmd *cmd = (struct se_cmd *)priv;
	u8 status;

	if (err) {
		status = SAM_STAT_CHECK_CONDITION;
		if (err == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			cmd->cmd_dr.work_io_err = 
				TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		} else {
			cmd->cmd_dr.work_io_err = 
				TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}
	} else {
		status = SAM_STAT_GOOD;
		cmd->cmd_dr.work_io_err = TCM_NO_SENSE;
	}

	target_complete_cmd(cmd, status);
}

static int __qnap_iscsit_tcp_zmc_sbc_fio_write(
	struct se_cmd *cmd, 
	void *data	
	)
{
	struct se_device *dev = cmd->se_dev;
	struct fio_data_set *data_set = (struct fio_data_set *)data;
	struct tcp_zmc_cmd_priv *zmc_data = 
		(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	if (data_set->async_io) {
		/* here depends on native code, the async io mode will use 
		 * dio with bio methanism (please refer the fd_execute_rw_aio()
		 */
		struct __rw_task task;
		struct __dev_info *dev_info = &task.dev_info;
		struct __fe_info *fe_info = &dev_info->fe_info;
		int ret;

		qnap_transport_create_devinfo(cmd, dev_info);
		task.sg_list = zmc_data->sgl;
		task.sg_nents = zmc_data->sgl_nents;
		task.nr_bytes = cmd->data_length;
		task.lba = cmd->t_task_lba;
		task.bs_order = ilog2(dev->dev_attrib.block_size);
		task.dir = DMA_TO_DEVICE;
		task.ret = 0;
		task.async_io = true;
		task.priv = (void *)cmd;
		task.priv_cb_complete = __qnap_iscsit_tcp_zmc_sbc_fio_aio_write_cb;

		/* flush caches and truncate them before to submit bio(s) */
		qlib_fd_flush_and_truncate_cache(fe_info->__fd.fd_file, task.lba, 
				(task.nr_bytes >> task.bs_order), task.bs_order, 
				true, fe_info->is_thin);

		ret = qlib_blockio_rw(&task);
		if (ret) {
			ret = -EINVAL;
			cmd->cmd_dr.work_io_err = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		} else
			cmd->cmd_dr.work_io_err = TCM_NO_SENSE;

		return ret;
	} 

	/* for fio type, we only replace sgl, sgl_nents, please refer 
	 * the fd_execute_rw_buffered()
	 */
	data_set->sgl = zmc_data->sgl;
	data_set->sgl_nents = zmc_data->sgl_nents;
	cmd->cmd_dr.work_io_err = TCM_NO_SENSE;
	return 0;
}

/**/
static int __qnap_iscsit_tcp_zmc_sbc_write_same(
	struct se_cmd *cmd, 
	void *data
	)
{
	struct tcp_zmc_cmd_priv *zmc_data = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;
	int ret;

	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	if (!qlib_is_ib_fbdisk_dev(&cmd->se_dev->dev_dr)
		|| (!qlib_is_ib_blk_dev(&cmd->se_dev->dev_dr))
	)
	{
		WARN_ON(!zmc_data->sgl_in_used);
		return __qnap_iscsit_tcp_zmc_ib_write_same(cmd, zmc_data->sgl,
				zmc_data->sgl_nents, data);
	}
	else if (!qlib_is_fio_blk_dev(&cmd->se_dev->dev_dr)
		|| (!qlib_is_fio_file_dev(&cmd->se_dev->dev_dr))
	)
	{
		WARN_ON(!zmc_data->sgl_in_used);
		return __qnap_iscsit_tcp_zmc_fio_write_same(cmd, zmc_data->sgl,
				zmc_data->sgl_nents, data);
	}

	return -ENOTSUPP;
}

static int __qnap_iscsit_tcp_zmc_sbc_ats_callback(
	struct se_cmd *cmd, 
	bool success, 
	int *post_ret
	)
{
	int ret;

	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	if (!qlib_is_ib_fbdisk_dev(&cmd->se_dev->dev_dr)
		|| (!qlib_is_ib_blk_dev(&cmd->se_dev->dev_dr))
	)
	{
		return __qnap_iscsit_tcp_zmc_sbc_ib_ats_callback(
			cmd, success, post_ret);
	}
	else if (!qlib_is_fio_blk_dev(&cmd->se_dev->dev_dr)
		|| (!qlib_is_fio_file_dev(&cmd->se_dev->dev_dr))
	) 
	{
		return __qnap_iscsit_tcp_zmc_sbc_fio_ats_callback(
				cmd, success, post_ret);
	}

	return -ENOTSUPP;
}

static int __qnap_iscsit_tcp_zmc_sbc_write(
	struct se_cmd *cmd, 
	void *data
	)
{
	struct tcp_zmc_cmd_priv *zmc_data = NULL;

	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	if (!qlib_is_ib_fbdisk_dev(&cmd->se_dev->dev_dr)
	|| (!qlib_is_ib_blk_dev(&cmd->se_dev->dev_dr))
	) 
	{
		zmc_data = (struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;
		return __qnap_iscsit_tcp_zmc_sbc_ib_write(cmd, zmc_data->sgl, 
				zmc_data->sgl_nents, data);
	}
	else if (!qlib_is_fio_blk_dev(&cmd->se_dev->dev_dr)
		|| (!qlib_is_fio_file_dev(&cmd->se_dev->dev_dr))
	)
	{
		return __qnap_iscsit_tcp_zmc_sbc_fio_write(cmd, data);
	}

	return -ENOTSUPP;
}

static int __qnap_iscsit_tcp_zmc_kunmap_data_sg(
	struct se_cmd *cmd
	)
{
	struct tcp_zmc_cmd_priv *data = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	if (!data->sgl_in_used)
		return -EPERM;

	if (!data->data_vmap) {
		WARN_ON(true);
		return -EINVAL;
	}

	__qnap_iscsit_tcp_zmc_core_kunmap_data_sg(data->sgl, 
					data->sgl_nents, data->data_vmap);
	data->data_vmap = NULL;
	return 0;
}

static int __qnap_iscsit_tcp_zmc_kmap_data_sg(
	struct se_cmd *cmd, 
	u32 len, 
	void **vmap
	)
{
	struct tcp_zmc_cmd_priv *data = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	WARN_ON(data->data_vmap);
	data->data_vmap = NULL;

	/* remember for READ direction command still need run in normal path */
	if (!data->sgl_in_used)
		return -EPERM;

	data->data_vmap = __qnap_iscsit_tcp_zmc_core_kmap_data_sg(data->sgl, 
					data->sgl_nents, len);
	*vmap = data->data_vmap;
	return 0;
}

static int __qnap_iscsit_tcp_zmc_alloc_sgl(
	struct se_cmd *cmd
	)
{
	if (!cmd->cmd_dr.transport_zmc_ops)
		return -ENOTSUPP;

	/* this is dummy code since we prepare the sgl in iscsi layer already */

	/* For every write-direction command, we always use zmc wsgl. 
	 * But, here is tricky method we do NOT allocate fixed wsgls, 
	 * all wsgls we want will be first handled in 
	 * __qnap_iscsit_zmc_skb_to_sgvec() then prepare final one 
	 * in qnap_target_zmc_prep_wsgl() called by sbc_execute_rw()
	 */ 
	if (cmd->data_direction == DMA_TO_DEVICE)
		return 0;

	return -EPERM;

}

static int __qnap_iscsit_tcp_zmc_prep_final_sgl(
	struct se_cmd *cmd
	)
{
	struct tcp_zmc_cmd_priv *priv = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	struct iscsit_zmc_ops *ops = 
		(struct iscsit_zmc_ops *)cmd->cmd_dr.transport_zmc_ops;

	if (!ops)
		return -ENOTSUPP;

	/* for non-write operation, still go to normal path */
	if (!priv->sgl_in_used)
		return -EPERM;

	/* prepare final sgl from all sgl_node / sgl_nents before to call
	 * cmd->execute_cmd() in __target_execute_cmd()
	 */
	priv->sgl = ops->convert_sgl_node(cmd, priv->sgl_node, priv->sgl_nents);

	ops->free_sgl_node(cmd);
	if (!priv->sgl) 
		return -ENOMEM;

	return 0;
}


static struct target_zmc_ops __iscsit_zmc_ops = {
	/* same as iscsi_ops in iscsi_target_config.c */
	.fabric_name		= "iSCSI",
	.kmap_data_sg		= __qnap_iscsit_tcp_zmc_kmap_data_sg,
	.kunmap_data_sg 	= __qnap_iscsit_tcp_zmc_kunmap_data_sg,
	.alloc_sgl 		= __qnap_iscsit_tcp_zmc_alloc_sgl,
	.prep_final_sgl		= __qnap_iscsit_tcp_zmc_prep_final_sgl,

	/* simple wrapper for some sbc functions for zmc feature 
	 *
	 * -ENOTSUPP: not support zmc feature
	 * 0: good to call function (support zmc feature)
	 * others: fail to call function (support zmc feature) 
	 *
	 * for zmc_sbc_xxxx(), the cmd->cmd_dr.work_io_err will carry the 
	 * TCM_XXX code finally
	 */
	.sbc_write 		= __qnap_iscsit_tcp_zmc_sbc_write,
	.sbc_ats_callback 	= __qnap_iscsit_tcp_zmc_sbc_ats_callback,
	.sbc_write_same 	= __qnap_iscsit_tcp_zmc_sbc_write_same,

};


struct target_zmc_ops *iscsi_target_zmc_ops = &__iscsit_zmc_ops;



