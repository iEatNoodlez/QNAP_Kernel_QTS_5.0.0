/*******************************************************************************
 * Filename:  target_core_qsbc.c
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
#include <asm/unaligned.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>
#include <qnap/fbdisk.h>
#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_backend.h>

#include "../target_core_iblock.h"
#include "../target_core_file.h"
#include "../target_core_pr.h"
#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qsbc.h"
#include "target_core_qspc.h"

#ifdef SUPPORT_TP
static void __qnap_sbc_build_provisioning_group_desc(struct se_device *se_dev, 
	u8 *buf);
#endif

static sense_reason_t __qnap_sbc_ib_fbdisk_fast_zero(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct iblock_dev *ibd = 
		(struct iblock_dev *)se_dev->transport->get_dev(se_dev);

	struct block_device *bd = ibd->ibd_bd;
	sector_t sectors = sbc_get_write_same_sectors(se_cmd);
	sector_t block_lba = se_cmd->t_task_lba;
	u32 bs_order = ilog2(se_cmd->se_dev->dev_attrib.block_size);
	sector_t tmp_lba, tmp_range;
	int ret;
    int i, blk_desc_count, done_blks = 0;
	sense_reason_t sense_ret;
	struct __dev_info dev_info;
	struct ____align_desc_blk desc;	
	struct __rw_task write_task;
	struct ____align_desc_blk_range *blk_range;
	bool is_ht;

    /* following code is similar with block-based's one, they shall be refactored */


	/* use alloc_bytes to be 1mb size, we will run special call provided by block layer and
     * shall consider legacy pool design (blksz is 1mb)
	 */
	u64 alloc_bytes = ALIGNED_SIZE_BYTES, total_nr_bytes, write_nr_bytes;
	u32 aligned_size = ALIGNED_SIZE_BYTES;
	sector_t write_lba;

	memset(&write_task, 0, sizeof(struct __rw_task));
	memset(&dev_info, 0, sizeof(struct __dev_info));

	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* step1: get alinged part or it is non-aligned case */
	qlib_create_aligned_range_desc(&desc, block_lba, sectors, bs_order, aligned_size);

	write_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, &write_task.sg_nents);
	if (!write_task.sg_list)
		return TCM_OUT_OF_RESOURCES;

	if (!desc.is_aligned) {
		/* for this case, all data we want to write is <= dm-thin data block size */
		blk_desc_count = 1;
		blk_range = &desc.body;
		is_ht = false;
		goto unaligned_write;
	} 

	/* step2: do fast zero here
	 * try conver the (lba,nolb) again cause of the block size
	 * for linux block layer is 512b but upper lun block size may
	 * be 4096b ... */
	block_lba = ((desc.body.lba << bs_order) >> 9);
	sectors = ((desc.body.nr_blks << bs_order) >> 9);

	pr_debug("%s: special discard lba:0x%llx, speacial blks:0x%llx\n", 
		(unsigned long long)desc.body.lba, (unsigned long long)desc.body.nr_blks);

	/* special discard - fast zero */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, true, bd, 
		block_lba, sectors, GFP_KERNEL, 0);

	if (ret != 0) {
		pr_warn("%s: fail to call blkdev_issue_special_discard, ret: %d\n",
			__func__, ret);
		goto err_case;
	}

	/* step3: handle the non-aligned part */
	blk_desc_count = MAX_ALIGN_DESC_HT_BLK;
	blk_range = &desc.head_tail[0];
	is_ht = true;

unaligned_write:

	for (i = 0; i < blk_desc_count; i++) {
		if (!blk_range->nr_blks)
			continue;

		pr_debug("blk range[%s]: lba:0x%llx, blks:0x%llx\n", i, 
			((is_ht) ? "HT": "non-HT"),
			(unsigned long long)blk_range->lba, 
			(unsigned long long)blk_range->nr_blks);

		write_task.dir = DMA_TO_DEVICE;
		write_task.bs_order = bs_order;
		write_task.ret = 0;
		memcpy((void *)&write_task.dev_info, (void *)&dev_info, 
			sizeof(struct __dev_info));

		write_lba = blk_range->lba;
		total_nr_bytes = ((u64)blk_range->nr_blks << bs_order);

		while (total_nr_bytes) {
			write_nr_bytes = min_t(u64, alloc_bytes, total_nr_bytes);
			write_task.lba = write_lba;
			write_task.nr_bytes = write_nr_bytes;

			/* this shall never happen ... */
			if (unlikely(write_task.nr_bytes > alloc_bytes))
				WARN_ON(true);

			done_blks = qlib_blockio_rw(&write_task);
		
			if (write_task.ret != 0) {
				ret = write_task.ret;
				goto err_case;
			}

			if (((sector_t)write_nr_bytes >> bs_order) != done_blks)
				WARN_ON(true);

			write_lba += ((sector_t)write_nr_bytes >> bs_order);
			total_nr_bytes -= write_nr_bytes;
		}

		if (blk_desc_count == MAX_ALIGN_DESC_HT_BLK)
			blk_range = &desc.head_tail[1];
	}

	sense_ret = TCM_NO_SENSE;
	goto exit;	

err_case:

#ifdef SUPPORT_TP
	if (ret == -ENOSPC) {
		pr_warn("%s: space was full\n", __func__);
		sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		goto exit;
	}
#endif
	sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

exit:
	qlib_free_sg_list(write_task.sg_list, write_task.sg_nents);
	return sense_ret;
}

static sense_reason_t __qnap_sbc_fio_blkdev_fast_zero(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct inode *inode = fd_dev->fd_file->f_mapping->host;
	sector_t sectors = sbc_get_write_same_sectors(se_cmd);
	sector_t block_lba = se_cmd->t_task_lba;
	u32 bs_order = ilog2(se_cmd->se_dev->dev_attrib.block_size);
	int ret, i, blk_desc_count, done_blks = 0;
	sense_reason_t sense_ret;
	struct ____align_desc_blk desc;	
	struct __rw_task write_task;
	struct __dev_info dev_info;
	struct ____align_desc_blk_range *blk_range;
	bool is_ht;

	/* alloc_bytes - 1mb size is enough since current dm-thin data block size
	 * is 512 KB or 1MB 
	 */
	u64 alloc_bytes = (1 << 20), total_nr_bytes, write_nr_bytes;
	u32 aligned_size = ALIGNED_SIZE_BYTES;
	sector_t write_lba;

	memset(&write_task, 0, sizeof(struct __rw_task));
	memset(&dev_info, 0, sizeof(struct __dev_info));

	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* get alinged part or it is non-aligned case */
	qlib_create_aligned_range_desc(&desc, block_lba, sectors, bs_order, aligned_size);

	write_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, &write_task.sg_nents);
	if (!write_task.sg_list)
		return TCM_OUT_OF_RESOURCES;

	if (!desc.is_aligned) {
		/* for this case, all data we want to write is <= dm-thin data block size */
		blk_desc_count = 1;
		blk_range = &desc.body;
		is_ht = false;
		goto unaligned_write;
	} 

	/* case for we have alignd block at least 
	 * step1: flush and truncate page first
	 */ 
	ret = qlib_fd_flush_and_truncate_cache(fd_dev->fd_file, 
		desc.body.lba, desc.body.nr_blks, bs_order, true, 
		qlib_thin_lun(&se_dev->dev_dr));

	if (ret != 0) {
		pr_warn("%s: fail to call qlib_fd_flush_and_truncate_cache, "
			"ret: %d\n", __func__, ret);
		goto err_case;
	}

	/* step2: do fast zero here
	 * try conver the (lba,nolb) again cause of the block size
	 * for linux block layer is 512b but upper lun block size may
	 * be 4096b ... */
	block_lba = ((desc.body.lba << bs_order) >> 9);
	sectors = ((desc.body.nr_blks << bs_order) >> 9);

	pr_debug("%s: special discard lba:0x%llx, speacial blks:0x%llx\n", 
		(unsigned long long)desc.body.lba, (unsigned long long)desc.body.nr_blks);

	/* special discard - fast zero */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, true, inode->i_bdev, 
		block_lba, sectors, GFP_KERNEL, 0);

	if (ret != 0) {
		pr_warn("%s: fail to call blkdev_issue_special_discard, ret: %d\n",
			__func__, ret);
		goto err_case;
	}

	/* step3: handle the non-aligned part */
	blk_desc_count = MAX_ALIGN_DESC_HT_BLK;
	blk_range = &desc.head_tail[0];
	is_ht = true;

unaligned_write:

	for (i = 0; i < blk_desc_count; i++) {
		if (!blk_range->nr_blks)
			continue;

		pr_debug("blk range[%s]: lba:0x%llx, blks:0x%llx\n", i, 
			((is_ht) ? "HT": "non-HT"),
			(unsigned long long)blk_range->lba, 
			(unsigned long long)blk_range->nr_blks);

		write_task.dir = DMA_TO_DEVICE;
		write_task.bs_order = bs_order;
		write_task.ret = 0;
		memcpy((void *)&write_task.dev_info, (void *)&dev_info, 
			sizeof(struct __dev_info));

		write_lba = blk_range->lba;
		total_nr_bytes = ((u64)blk_range->nr_blks << bs_order);

		while (total_nr_bytes) {
			write_nr_bytes = min_t(u64, alloc_bytes, total_nr_bytes);
			write_task.lba = write_lba;
			write_task.nr_bytes = write_nr_bytes;

			/* this shall never happen ... */
			if (unlikely(write_task.nr_bytes > alloc_bytes))
				WARN_ON(true);

			done_blks = qlib_fileio_rw(&write_task);
		
			if (write_task.ret != 0) {
				ret = write_task.ret;
				goto err_case;
			}

			if (((sector_t)write_nr_bytes >> bs_order) != done_blks)
				WARN_ON(true);

			write_lba += ((sector_t)write_nr_bytes >> bs_order);
			total_nr_bytes -= write_nr_bytes;
		}

		if (blk_desc_count == MAX_ALIGN_DESC_HT_BLK)
			blk_range = &desc.head_tail[1];
	}

	sense_ret = TCM_NO_SENSE;
	goto exit;	

err_case:

#ifdef SUPPORT_TP
	if (ret == -ENOSPC) {
		pr_warn("%s: space was full\n", __func__);
		sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		goto exit;
	}
#endif
	sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

exit:
	qlib_free_sg_list(write_task.sg_list, write_task.sg_nents);
	return sense_ret;
}

sense_reason_t qnap_sbc_write_same_fast_zero(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr))
		return __qnap_sbc_fio_blkdev_fast_zero(se_cmd);
	else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr))
		return __qnap_sbc_ib_fbdisk_fast_zero(se_cmd);

	return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE; 	 
}
EXPORT_SYMBOL(qnap_sbc_write_same_fast_zero);

bool qnap_sbc_write_same_is_zero_buf(
	struct se_cmd *se_cmd
	)
{
	struct page *zero_page = ZERO_PAGE(0);
	struct scatterlist *sg = &se_cmd->t_data_sg[0];
	unsigned char *zero_buf = NULL, *data_buf = NULL;
	int ret = 0;
	u32 len, buf_len = se_cmd->data_length;

	WARN_ON(!buf_len);

	data_buf = kmap(sg_page(sg)) + sg->offset;
	zero_buf = kmap(zero_page);

	if (!data_buf || !zero_buf) {
		ret = -ENOMEM;
		goto exit;
	}

	while (buf_len) {
		len = min_t(u32, buf_len, PAGE_SIZE);
		ret = memcmp(data_buf, zero_buf, len);
		if (ret) {
			ret = -EINVAL;
			break;
		}

		buf_len -= len;
		data_buf += len;
	}

exit:
	if (zero_buf)
		kunmap(zero_page);		

	if (data_buf)
		kunmap(sg_page(sg));

	return ((!ret) ? true : false);

}
EXPORT_SYMBOL(qnap_sbc_write_same_is_zero_buf);

#ifdef SUPPORT_TP

static int __qnap_fd_blk_update_lba_status_desc(
	struct se_device *se_dev,
	sector_t start_lba,
	int max_desc_count,
	unsigned char *buf
	)
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct file *file = fd_dev->fd_file;
	LBA_STATUS_DESC *desc_buf = (LBA_STATUS_DESC *)&buf[8];
	sector_t max_lba = se_dev->transport->get_blocks(se_dev);
	u32 nr_blks, bs_order = ilog2(se_dev->dev_attrib.block_size);
	sector_t lba_512b = 0, curr_lba = 0;
	u64 req_index, req_range;
	u32 sectors_per_dm_blk; 
	u8 pro_status[256];
	int ret = 0, count = 0, idx;
	bool stop_update = false;

	if (!thin_get_sectors_per_block || !thin_get_lba_status)
		return -ENOTSUPP;

	/* for block-based lun, se_dev_udev_path will store the
	 * lv dev name which maps to iscsi lun
	 */		
	if (!strlen(&se_dev->udev_path[0])) 
		return -EINVAL;

	/* get how many blocks (512b) per one dm thin data block */
	ret = thin_get_sectors_per_block(se_dev->udev_path, &sectors_per_dm_blk);
	if (ret != 0){
		pr_err("fail to call thin_get_sectors_per_block\n");
		return -EINVAL;
	}

	pr_debug("start_lba:0x%llx, max_desc_count:%d, sectors_per_dm_blk(512b):%d\n", 
		(unsigned long long)start_lba, max_desc_count, sectors_per_dm_blk);

	/* convert to 512b unit and don't care it is thin or not since 
	 * what we want is to flush cache ... 
	 */
	lba_512b = ((start_lba << bs_order) >> 9);

	qlib_fd_flush_and_truncate_cache(file, lba_512b, 
		(max_desc_count * sectors_per_dm_blk), bs_order, false, false);

	req_index = div_u64((u64)lba_512b, sectors_per_dm_blk);
	req_range = sizeof(pro_status);

	pr_debug("lba_512b:0x%llx, req_index:0x%llx, req_range:0x%llx\n", 
		(unsigned long long)lba_512b, (unsigned long long)req_index, 
		(unsigned long long)req_range);

	while (true) {
		/* here won't get one by one due to the mutex lock will be called
		 * at each time ... to reduce the cost ..
		 */
		ret = thin_get_lba_status(se_dev->udev_path, req_index, 
				req_range, &pro_status[0]);
		if (ret != 0){
			pr_err("%s: fail call thin_get_lba_status, ret:%d\n", 
				__func__, ret);
			goto exit;
		}

		/* get the start pos */
		curr_lba = ((((sector_t)req_index * sectors_per_dm_blk) << 9) >> bs_order);

		for (idx = 0; idx < req_range; idx++) {

			nr_blks = ((sectors_per_dm_blk << 9) >> bs_order);

			if ((curr_lba + (sector_t)nr_blks) > max_lba) {
				nr_blks = max_lba - curr_lba + 1;
				stop_update = true;
			}
	
			pr_debug("curr_lba:0x%llx, nr_blks:0x%llx, pro_status:%d\n",
				(unsigned long long)curr_lba, 
				(unsigned long long)nr_blks, pro_status[idx]);
	
			put_unaligned_be64((u64)curr_lba, &desc_buf[count].lba[0]);
			put_unaligned_be32(nr_blks, &desc_buf[count].nr_blks[0]);
			desc_buf[count].provisioning_status = pro_status[idx];
			count++;

			/* next ... */
			curr_lba += (sector_t)nr_blks;			
	
			if (count == max_desc_count)
				goto exit;

			if (stop_update)
				goto exit;
			
		}

		req_index += req_range;

	}
	
exit:
	if (ret == 0) {
		/* to update PARAMETER DATA LENGTH finally */
		put_unaligned_be32(((count << 4) + 4), &buf[0]);
	}

	return ret;

}

static sense_reason_t __qnap_fd_blk_get_lba_status(
	struct se_cmd *se_cmd
	)
{
	u8 *buf = NULL;
	sector_t start_lba;
	u32 para_data_length, max_desc_count;
	sense_reason_t reason;
	int ret;

	start_lba = get_unaligned_be64(&se_cmd->t_task_cdb[2]);
	para_data_length = get_unaligned_be32(&se_cmd->t_task_cdb[10]);
	max_desc_count = ((para_data_length - 8) >> 4);

	buf = transport_kmap_data_sg(se_cmd);
	if (!buf) {
		reason = TCM_OUT_OF_RESOURCES;
		goto exit;
	}

	ret = __qnap_fd_blk_update_lba_status_desc(se_cmd->se_dev, start_lba, 
			max_desc_count, buf);

	if (ret == 0)
		reason = TCM_NO_SENSE;
	else {
		if (ret == -EOPNOTSUPP)
			reason = TCM_UNSUPPORTED_SCSI_OPCODE;
		else
			reason = TCM_INVALID_PARAMETER_LIST;
	}
exit:

	if (buf)
		transport_kunmap_data_sg(se_cmd);
	
	if (reason == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, GOOD);

	return reason;

}

/* - tricky code here since fiemap_check_ranges() was declared as static function 
 *   this shall depend on your native code and remove it if it is NOT static
 * - it was referred from fiemap_check_ranges()
 */
static int __qnap_fiemap_check_ranges(
	struct super_block *sb,
	u64 start, 
	u64 len, 
	u64 *new_len
	)
{
	u64 maxbytes = (u64) sb->s_maxbytes;

	*new_len = len;

	if (len == 0)
		return -EINVAL;

	if (start > maxbytes)
		return -EFBIG;

	/*
	 * Shrink request scope to what the fs can actually handle.
	 */
	if (len > maxbytes || (maxbytes - len) < start)
		*new_len = maxbytes - start;

	return 0;
}

static int __qnap_ib_fbdisk_update_lba_status_desc(
	struct se_device *se_dev,
	sector_t start_lba,
	int max_desc_count,
	unsigned char *buf
	)
{
	LBA_STATUS_DESC *desc_buf = (LBA_STATUS_DESC *)&buf[8];
	struct iblock_dev *ibd = se_dev->transport->get_dev(se_dev);
	struct block_device *bd = ibd->ibd_bd;
	struct inode *inode = NULL;
	struct file *file = NULL;
	struct fbdisk_file *fb_file = NULL;
	struct fiemap_extent file_ext;
	struct fiemap_extent_info file_info;
	sector_t curr_lba;
	u32 nr_blks, bs_order = ilog2(se_dev->dev_attrib.block_size);
	loff_t pos, tmp_len, len = MAX_FILEMAP_SIZE;
	int ret = 0, count = 0, idx;

	/* it shall be better to use ioctl to get lba map status directly
	 * than to use this function 
	 */
	fb_file = (struct fbdisk_file *)qnap_transport_get_fbdisk_file(
			bd->bd_disk->private_data, start_lba, &idx);

	if (!fb_file)
		return -ENODEV;

	file = fb_file->fb_backing_file;
	if (!file)
		return -ENODEV;

	inode = file_inode(file);
	if (!inode->i_op->fiemap)
		return -EOPNOTSUPP;

	ret = blkdev_issue_flush(bd, GFP_KERNEL, NULL);
	if (unlikely(ret))
		pr_err("%s: fail to call blkdev_issue_flush, ret:%d\n", __func__, ret);

	pos = ((loff_t)start_lba << bs_order);
	
	pr_debug("%s: max_desc_count:0x%x\n", __func__, max_desc_count);
	pr_debug("%s: (before call fiemap_check_ranges) pos:0x%llx, "
		"len:0x%llx\n", __func__, (unsigned long long)pos,
		(unsigned long long)len);

	/* final tmp_len may be smaller than len */
	ret = __qnap_fiemap_check_ranges(inode->i_sb, pos, len, &tmp_len);
	if (ret != 0)
		return ret;

	if (len != tmp_len)
		len = tmp_len;

	pr_debug("%s, (after call fiemap_check_ranges) pos:0x%llx, "
		"len:0x%llx\n", __func__, (unsigned long long)pos,
		(unsigned long long)len);

	/* following code is based on case - fi_extents_max = 1 , 
	 * it shall be smart here ...
	 */
	file_info.fi_flags = FIEMAP_FLAG_SYNC;;
	file_info.fi_extents_max = 1;
	file_info.fi_extents_start = &file_ext;

	if (file_info.fi_flags & FIEMAP_FLAG_SYNC)
		filemap_write_and_wait(inode->i_mapping);

	while (true) {
		ret = 0;
		file_info.fi_extents_mapped = 0;
		memset(&file_ext, 0, sizeof(struct fiemap_extent));

		ret = inode->i_op->fiemap(inode, &file_info, pos, len);
		if (ret != 0) {
			pr_err("%s: fail call i_op->fiemap, ret:%d\n", __func__, ret);
			goto exit;
		}

		pr_debug("%s: mapped extent count:%d\n", __func__, 
				file_info.fi_extents_mapped);

		if (unlikely(!file_info.fi_extents_mapped)){
			pr_debug("=== no mapped case ===\n");
			pr_debug("fe_logical:0x%llx, fe_physical:0x%llx, "
				"fe_length:0x%llx, fe_flags:0x%x\n", 
				(unsigned long long)file_ext.fe_logical,
				(unsigned long long)file_ext.fe_physical,
				(unsigned long long)file_ext.fe_length,
				file_ext.fe_flags);

			curr_lba = ((sector_t)pos >> bs_order);
			nr_blks = ((sector_t)len >> bs_order);
			pos += len;

			put_unaligned_be64((u64)curr_lba, &desc_buf[count].lba[0]);
			put_unaligned_be32(nr_blks, &desc_buf[count].nr_blks[0]);
			desc_buf[count].provisioning_status = PROVISIONING_STATUS_CODE_DEALLOCATED;
			count++;
			goto next;
		}

		/* start to update lba status desc */
		pr_debug("pos:0x%llx, fe_logical:0x%llx, "
			"fe_physical:0x%llx, fe_length:0x%llx, "
			"fe_flags:0x%x\n", (unsigned long long)pos,
			(unsigned long long)file_ext.fe_logical,
			(unsigned long long)file_ext.fe_physical,
			(unsigned long long)file_ext.fe_length,
			file_ext.fe_flags);

		if (file_ext.fe_logical > pos) {
			/* case: 
			 *
			 *   pos
			 *    |
			 * +-------+------------+------
			 *         ^            ^
			 *      start mapped   end mapped
			 */

			/* to update gap range first */
			curr_lba = ((sector_t)pos >> bs_order);
			nr_blks = ((sector_t)(file_ext.fe_logical - pos) >> bs_order);
			put_unaligned_be64((u64)curr_lba, &desc_buf[count].lba[0]);
			put_unaligned_be32(nr_blks, &desc_buf[count].nr_blks[0]);
			desc_buf[count].provisioning_status = PROVISIONING_STATUS_CODE_DEALLOCATED;
			count++;

			if (count == max_desc_count)
				break;		

		}
		else if (file_ext.fe_logical < pos) {
			/* case: 
			 *
			 *           pos
			 *            |
			 * +-----+------------+------
			 *	 ^	      ^
			 *	start mapped   end mapped
			 */
			curr_lba = ((sector_t)pos >> bs_order);
			tmp_len = (pos - file_ext.fe_logical);
			nr_blks = ((sector_t)(file_ext.fe_length - tmp_len) >> bs_order);
			pos += (file_ext.fe_length - tmp_len);
			goto update_desc;
		} 

		/* case: 
		 *
		 *      pos
		 *       |
		 * +-----+------------+------
		 *	 ^	      ^
		 *	start mapped   end mapped
		 */
		curr_lba = ((sector_t)file_ext.fe_logical >> bs_order);
		nr_blks = ((sector_t)file_ext.fe_length >> bs_order);
		pos = file_ext.fe_logical + file_ext.fe_length;

update_desc:
		put_unaligned_be64((u64)curr_lba, &desc_buf[count].lba[0]);
		put_unaligned_be32(nr_blks, &desc_buf[count].nr_blks[0]);
		desc_buf[count].provisioning_status = PROVISIONING_STATUS_CODE_MAPPED_OR_UNKNOWN;
		count++;

next:
		if (count == max_desc_count)
			break;

		/* current extent may NOT be last one ... */
		if (file_ext.fe_flags & FIEMAP_EXTENT_LAST)
			break;

	}

exit:
	if (ret == 0) {
		/* to update PARAMETER DATA LENGTH finally */
		put_unaligned_be32(((count << 4) + 4), &buf[0]);
	}
	
	return ret;

}

static sense_reason_t __qnap_ib_fbdisk_get_lba_status(
	struct se_cmd *se_cmd
	)
{
	unsigned char *buf = NULL;	
	sector_t start_lba;
	u32 para_data_length, max_desc_count;
	sense_reason_t reason;
	int ret;

	buf = transport_kmap_data_sg(se_cmd);
	if (!buf)
		return TCM_OUT_OF_RESOURCES;

	start_lba = get_unaligned_be64(&se_cmd->t_task_cdb[2]);
	para_data_length = get_unaligned_be32(&se_cmd->t_task_cdb[10]);
	max_desc_count = ((para_data_length - 8) >> 4);

	ret = __qnap_ib_fbdisk_update_lba_status_desc(se_cmd->se_dev, start_lba, 
			max_desc_count, buf);

	if (ret == 0)
		reason = TCM_NO_SENSE;
	else {
		if (ret == -ENODEV || ret == -EOPNOTSUPP)
			reason = TCM_UNSUPPORTED_SCSI_OPCODE;
		else if (ret == -ENOMEM)
			reason = TCM_OUT_OF_RESOURCES;
		else if (ret == -EFBIG)
			reason = TCM_INVALID_CDB_FIELD;
		else
			reason = TCM_INVALID_PARAMETER_LIST;
	}

	if (buf)
		transport_kunmap_data_sg(se_cmd);

	if (reason == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, GOOD);

	return reason;
}

sense_reason_t qnap_sbc_get_lba_status(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	sense_reason_t reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* minimun value should be 24 = (16 bytes descriptor + 8 bytes) */
	if (get_unaligned_be32(&se_cmd->t_task_cdb[10]) < 24 )	
		return TCM_PARAMETER_LIST_LENGTH_ERROR;

	if (get_unaligned_be64(&se_cmd->t_task_cdb[2]) >
		se_cmd->se_dev->transport->get_blocks(se_cmd->se_dev))	
		return TCM_ADDRESS_OUT_OF_RANGE;


	if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr))
		return __qnap_ib_fbdisk_get_lba_status(se_cmd);
	else if (!qlib_is_fio_blk_dev(&se_dev->dev_dr))
		return __qnap_fd_blk_get_lba_status(se_cmd);

	return reason;

}
EXPORT_SYMBOL(qnap_sbc_get_lba_status);
#endif

static sense_reason_t __qnap_sbc_fd_blk_backend_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct file *file = fd_dev->fd_file;
	struct inode *inode = file->f_mapping->host;
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size);
	u32 aligned_size = ALIGNED_SIZE_BYTES;

	/* alloc_bytes - 1mb size is enough since current dm-thin data block size
	 * is 512 KB or 1MB 
	 */
	u64 alloc_bytes = (1 << 20), total_nr_bytes, write_nr_bytes;
	sector_t write_lba;
	int i, ret, blk_desc_count, done_blks = 0;
	sense_reason_t sense_ret;
	struct ____align_desc_blk desc;	
	struct __rw_task write_task;
	struct __dev_info dev_info;
	struct ____align_desc_blk_range *blk_range;
	bool is_ht;

	memset(&write_task, 0, sizeof(struct __rw_task));
	memset(&dev_info, 0, sizeof(struct __dev_info));

	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* get alinged part or it is non-aligned case */
	qlib_create_aligned_range_desc(&desc, lba, nolb, bs_order, aligned_size);

	write_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, &write_task.sg_nents);
	if (!write_task.sg_list)
		return TCM_OUT_OF_RESOURCES;

	if (!desc.is_aligned) {
		/* for this case, all data we want to write is <= dm-thin data block size */
		blk_desc_count = 1;
		blk_range = &desc.body;
		is_ht = false;
		goto unaligned_write;
	} 

	/* we did get aligned range at least
	 * step1: flush and truncate page first 
	 */
	ret = qlib_fd_flush_and_truncate_cache(fd_dev->fd_file, 
		desc.body.lba, desc.body.nr_blks, bs_order, true, 
		qlib_thin_lun(&se_dev->dev_dr));

	if (ret != 0) {
		pr_warn("%s: fail to call qlib_fd_flush_and_truncate_cache, "
			"ret: %d\n", __func__, ret);
		goto err_case;
	}

	/* step2: do unmap here for aligned range
	 * try conver the (lba,nolb) again cause of the block size
	 * for linux block layer is 512b but upper lun block size may
	 * be 4096b ... */
	lba = ((desc.body.lba << bs_order) >> 9);
	nolb = ((desc.body.nr_blks << bs_order) >> 9);

	pr_debug("%s: discard lba:0x%llx, discard blks:0x%llx\n", 
		__func__, (unsigned long long)desc.body.lba,
		(unsigned long long)desc.body.nr_blks);

	/* normal discard */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, false, inode->i_bdev, 
			lba, nolb, GFP_KERNEL, 0);

	if (ret != 0) {
		pr_warn("%s: fail to call blkdev_issue_special_discard, ret: %d\n",
			__func__, ret);
		goto err_case;
	}

	if (test_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &se_cmd->cmd_dr.cmd_t_state)
		|| atomic_read(&se_cmd->cmd_dr.tmr_code)
	)
	{
		sense_ret = TCM_NO_SENSE;
		goto exit;
	}

	/* step3: handle the non-aligned part */
	blk_desc_count = MAX_ALIGN_DESC_HT_BLK;
	blk_range = &desc.head_tail[0];
	is_ht = true;

unaligned_write:

	for (i = 0; i < blk_desc_count; i++) {
		if (!blk_range->nr_blks)
			continue;

		pr_debug("blk range[%s]: lba:0x%llx, blks:0x%llx\n",
			((is_ht) ? "HT": "non-HT"),
			(unsigned long long)blk_range->lba, 
			(unsigned long long)blk_range->nr_blks);

		write_task.dir = DMA_TO_DEVICE;
		write_task.bs_order = bs_order;
		write_task.ret = 0;
		memcpy((void *)&write_task.dev_info, (void *)&dev_info, 
			sizeof(struct __dev_info));

		write_lba = blk_range->lba;
		total_nr_bytes = ((u64)blk_range->nr_blks << bs_order);

		while (total_nr_bytes) {
			write_nr_bytes = min_t(u64, alloc_bytes, total_nr_bytes);
			write_task.lba = write_lba;
			write_task.nr_bytes = write_nr_bytes;

			/* this shall never happen ... */
			if (unlikely(write_task.nr_bytes > alloc_bytes))
				WARN_ON(true);

			done_blks = qlib_fileio_rw(&write_task);

			if (write_task.ret != 0) {
				ret = write_task.ret;
				goto err_case;
			}

			if (((sector_t)write_nr_bytes >> bs_order) != done_blks)
				WARN_ON(true);
			
			write_lba += ((sector_t)write_nr_bytes >> bs_order);
			total_nr_bytes -= write_nr_bytes;
		}

		if (blk_desc_count == MAX_ALIGN_DESC_HT_BLK)
			blk_range = &desc.head_tail[1];

	}

	sense_ret = TCM_NO_SENSE;
	goto exit;

err_case:

#ifdef SUPPORT_TP
	if (ret == -ENOSPC) {
		pr_warn("%s: space was full\n", __func__);
		sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		goto exit;
	}
#endif
	sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

exit:
	qlib_free_sg_list(write_task.sg_list, write_task.sg_nents);
	return sense_ret;
	
}

static sense_reason_t __qnap_sbc_iblock_blk_backend_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct iblock_dev *ibd = 
		(struct iblock_dev *)se_dev->transport->get_dev(se_dev);

	struct block_device *bd = ibd->ibd_bd;
	int ret;
	u32 bs_order = ilog2(se_cmd->se_dev->dev_attrib.block_size);

	/* The kernel block layer is 512b unit, so to convert 
	 * the lba, nolb again if now is 4KB sector size */
	lba = ((lba << bs_order) >> 9);
	nolb = ((nolb << bs_order) >> 9);

	/* normal discard */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, false, bd, lba, 
			nolb, GFP_KERNEL, 0);
	if (ret < 0) {
		pr_err("%s: fail from qnap_transport_blkdev_issue_discard(), "
			"ret: %d\n", __func__, ret);


		if (ret == -ENOMEM)
			return TCM_OUT_OF_RESOURCES;

#ifdef SUPPORT_TP
		if (ret == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		}
#endif
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	return TCM_NO_SENSE;
}

sense_reason_t qnap_sbc_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *se_dev = se_cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr))
		return __qnap_sbc_fd_blk_backend_unmap(se_cmd, lba, nolb);
	else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr))
		return __qnap_sbc_iblock_blk_backend_unmap(se_cmd, lba, nolb);

	return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE; 	 

}
EXPORT_SYMBOL(qnap_sbc_unmap);

unsigned int qnap_sbc_get_io_min(
	struct se_device *se_dev
	)
{
	/* for OPTIMAL TRANSFER LENGTH GRANULARITY in vpd 0xb0 */
	int bs_order = ilog2(se_dev->dev_attrib.block_size);

	/* hard coding to set 1MB */
	return ((MAX_TRANSFER_LEN_MB << 20) >> bs_order);
}
EXPORT_SYMBOL(qnap_sbc_get_io_min);

unsigned int qnap_sbc_get_io_opt(
	struct se_device *se_dev
	)
{
	/* for OPTIMAL TRANSFER LENGTH in vpd 0xb0 */
	int bs_order = ilog2(se_dev->dev_attrib.block_size);

	/* hard coding to set 1MB */
	return ((MAX_TRANSFER_LEN_MB << 20) >> bs_order);
}
EXPORT_SYMBOL(qnap_sbc_get_io_opt);

int qnap_sbc_get_threshold_exp(
	struct se_device *se_dev
	)
{
	/* sbc3r35j, p289 
	 * THRESHOLD_EXPONENT shall be larger than 0, 0 means
	 * the logical unit doesn't support logical block provisioning
	 * threshold
	 */
	if (!qlib_thin_lun(&se_dev->dev_dr))
		return 0;

	return __qnap_sbc_get_threshold_exp(
			(se_dev->transport->get_blocks(se_dev) + 1));
}
EXPORT_SYMBOL(qnap_sbc_get_threshold_exp);

#ifdef SUPPORT_TP
static void __qnap_sbc_build_provisioning_group_desc(
	struct se_device *se_dev,
	u8 *buf
	)
{
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;

	/* 1. pg_desc information starts from offset 8 of vpd 0xb2 
	 * 2. buf location was pointed to offset 8 already
	 */
	buf[0] |= 0x01;	// CODE SET == Binary
	buf[1] |= 0x00;	// ASSOCIATION == addressed logical unit: 0)b
	buf[1] |= 0x03;	// Identifier/Designator type == NAA identifier
	buf[3] = 0x10;	// Identifier/Designator length

	/* here the off = 4 and header len is 4 bytes
	 * (end location is byte7[bit7-4] ) 
	 */
	qlib_get_naa_6h_code((void *)se_dev, &dr->dev_naa[0], &buf[4], 
			qnap_transport_parse_naa_6h_vendor_specific);
}


int qnap_sbc_config_tp_on_evpd_b2(
	struct se_device *se_dev,
	unsigned char *buf
	)
{
	if (se_dev->dev_attrib.emulate_tpu 
	|| se_dev->dev_attrib.emulate_tpws
	)
	{
		if (!qlib_thin_lun(&se_dev->dev_dr))
			return -ENODEV;

		/* only do this when it is thin lun */
		put_unaligned_be16(4 + PROVISIONING_GROUP_DESC_LEN, &buf[2]);
	
		/* The THRESHOLD EXPONENT field indicates the threshold set
		 * size in LBAs as a power of 2 (i.e., the threshold set size
		 * is equal to 2(threshold exponent)).
		 *
		 * The RESOURCE COUNT filed (unit is threshold set size = 
		 * 2 ^ THRESHOLD EXPONENT nr blks) in Logical Block Provisioning
		 * log page is 32bit only, so we need to adjust
		 * THRESHOLD EXPONENT field to suitable value
		 */
		buf[4] = (unsigned char)qnap_sbc_get_threshold_exp(se_dev);
		buf[5] |= 0x1;
	
		/*
		 * A TPU bit set to one indicates that the device server
		 * supports the UNMAP command (see 5.25). A TPU bit set
		 * to zero indicates that the device server does not
		 * support the UNMAP command.
		 */
		if (se_dev->dev_attrib.emulate_tpu != 0)
			buf[5] |= 0x80;
	
		/*
		 * A TPWS bit set to one indicates that the device server
		 * supports the use of the WRITE SAME (16) command (see 5.42)
		 * to unmap LBAs. A TPWS bit set to zero indicates that the
		 * device server does not support the use of the
		 * WRITE SAME (16) command to unmap LBAs.
		 */
		if (se_dev->dev_attrib.emulate_tpws != 0)
			buf[5] |= 0x40;
	
		/* LBPRZ bit should be the same setting as LBPRZ bit in
		 * Read Capacity 16 */
		buf[5] |= 0x04;
		buf[6] |= VPD_B2h_PROVISION_TYPE_TP;
	
		/*
		 * FIXED ME
		 *
		 * Here to report the PROVISIONING GROUP DESCRIPTOR
		 * information. The PROVISIONING GROUP DESCRIPTOR field
		 * contains a designation descriptor for the LBA
		 * mapping resources used by logical unit.
		 *
		 * The ASSOCIATION field should be set to 00b
		 * The DESIGNATOR TYPE field should be 01h
		 * (T10 vendor ID based) or 03h (NAA)
		 *
		 * NOTE: 
		 * This code depends on target_emulate_evpd_83(), 
		 * please take care it...
		 */
	
		/* SBC3R31, page 279 */ 		
		__qnap_sbc_build_provisioning_group_desc(se_dev, &buf[8]);
		return 0;
	}

	return -ENODEV;
}

int qnap_sbc_modesense_lbp(
	struct se_cmd *se_cmd, 
	u8 pc, 
	unsigned char *p
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	THRESHOLD_DESC_FORMAT *desc = NULL;
	unsigned long long total_blocks = 
		(se_dev->transport->get_blocks(se_dev) + 1);
	u16 off = 16, len = 0;
	u64 dividend;
	u32 threshold_count, threshold_exp;

	if ((se_cmd->data_length == 0) || (off > (u16)se_cmd->data_length))
		return 0;

	if (!dev_attr_dr->tp_threshold_percent)
		return 0;

	dividend = (total_blocks * dev_attr_dr->tp_threshold_percent);
	dividend = div_u64(dividend, 100);

	threshold_exp = qnap_sbc_get_threshold_exp(se_dev);
	threshold_count = (u32)div_u64(dividend, (1 << threshold_exp));

	p[0] = (0x1c | 0x40); /* set SPF bit (bit 6) to 1 */
	p[1] = 0x02;

	/* No changeable values for now */
	if (pc == 1)
		goto out;

	/* FIXED ME !! 
	 * set the SITUA (single initiator threshold unit attention) bit
	 */
	p[4] = 0x01;

	desc = (THRESHOLD_DESC_FORMAT *)&p[off];
	desc->threshold_arming = THRESHOLD_ARM_INC;
	desc->threshold_type = THRESHOLD_TYPE_SOFTWARE;
	desc->enabled = 1;

	/* should be less than 0100h */
	desc->threshold_resource = LBP_LOG_PARAMS_USED_LBA_MAP_RES_COUNT;

	desc->threshold_count[0] = (threshold_count >> 24) & 0xff;
	desc->threshold_count[1] = (threshold_count >> 16) & 0xff;
	desc->threshold_count[2] = (threshold_count >> 8) & 0xff;
	desc->threshold_count[3] = threshold_count  & 0xff;

out:
	len = (sizeof(THRESHOLD_DESC_FORMAT) + 12);
	put_unaligned_be16(len, &p[2]);
	return (4 + len);
}

#endif

