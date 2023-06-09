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
#include <linux/fiemap.h>
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
#include "target_core_qodx.h"

/**/
static inline void __replace_core_unmap(
	struct se_cmd *cmd
	)
{
	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;

	cmd->cmd_dr.sbc_ops.sbc_unmap_ops.orig_sbc_core_unmap = ops->execute_unmap;
	ops->execute_unmap = cmd->cmd_dr.sbc_ops.sbc_unmap_ops.sbc_core_unmap;
}

static inline void __restore_core_unmap(
	struct se_cmd *cmd
	)
{
	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;

	ops->execute_unmap = cmd->cmd_dr.sbc_ops.sbc_unmap_ops.orig_sbc_core_unmap;
}

static int _qnap_fd_blk_sbc_ws_do_unaligned_write_by_desc(
	struct __dev_info *dev_info,
	struct ____align_desc_blk_range	*blk_range,
	int blk_desc_count,
	bool is_head_tail
	)
{

	sector_t write_lba;
	u64 alloc_bytes = ALIGNED_SIZE_BYTES, total_nr_bytes, write_nr_bytes;
	u32 bs_order = dev_info->bs_order;
	int i, done_blks = 0, ret = 0;
	struct __rw_task write_task;

	memset(&write_task, 0, sizeof(struct __rw_task));
	write_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, 
							&write_task.sg_nents);
	if (!write_task.sg_list)
		return -ENOMEM;

	memcpy((void *)&write_task.dev_info, (void *)dev_info, 
						sizeof(struct __dev_info));

	for (i = 0; i < blk_desc_count; i++) {
		if (!blk_range->nr_blks)
			goto next_range;
	
		pr_debug("blk range[%s]: lba:0x%llx, blks:0x%llx\n",
			((is_head_tail) ? "head_tail": "non head_tail"),
			(unsigned long long)blk_range->lba, 
			(unsigned long long)blk_range->nr_blks);

		write_task.dir = DMA_TO_DEVICE;
		write_task.bs_order = bs_order;
		write_task.ret = 0;

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

			if (write_task.ret) {
				ret = write_task.ret;
				goto fail_ret;
			} else
				ret = 0;
	
			if (((sector_t)write_nr_bytes >> bs_order) != done_blks)
				WARN_ON(true);
			
			write_lba += ((sector_t)write_nr_bytes >> bs_order);
			total_nr_bytes -= write_nr_bytes;
		}
next_range:
		blk_range++;
	}

fail_ret:	
	qlib_free_sg_list(write_task.sg_list, write_task.sg_nents);
	return ret;
}

/* this function is for regular file type, to use fiemap() from inode 
 * to get the file extent mapping information and update it into LBA_STATUS_DESC, 
 * remember the implemetation depends on the native kernel code design ...
 */
static int _file_get_lba_status(
	struct file *file,
	loff_t pos, 
	loff_t len,
	u32 bs_order,
	u32 max_desc_count,
	u8 *buf
	)
{
	struct inode *inode = file_inode(file);
	LBA_STATUS_DESC *desc_buf = (LBA_STATUS_DESC *)&buf[8];
	struct fiemap_extent file_ext;
	struct fiemap_extent_info file_info;
	loff_t tmp_len;
	sector_t curr_lba;
	u32 nr_blks;
	int ret = 0, count = 0;

	if (!inode->i_op->fiemap)
		return -EOPNOTSUPP;

	ret = vfs_fsync(file, 1);
	if (unlikely(ret))
		pr_err("%s: fail to call vfs_fsync, ret: %d\n", __func__, ret);

	pr_debug("%s: max_desc_count:0x%x\n", __func__, max_desc_count);

	/* following code is based on case - fi_extents_max = 1 , 
	 * it shall be smart here ...
	 */
	file_info.fi_flags = (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_KERNEL);
	file_info.fi_extents_max = 1;
	file_info.fi_extents_start = &file_ext;

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
			/*   pos
			 *    |
			 * +-------+------------+------
			 *	   ^		^
			 *	start mapped   end mapped
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
			/*	     pos
			 *	      |
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

		/*	pos
		 *	 |
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

static sense_reason_t _ib_blk_sbc_get_lba_status(
	struct se_device *se_dev,
	sector_t start_lba,
	u32 max_desc_count,
	u8 *buf
	)
{
	return -EINVAL;
}

static sense_reason_t _ib_fbdisk_sbc_get_lba_status(
	struct se_device *se_dev,
	sector_t start_lba,
	u32 max_desc_count,
	u8 *buf
	)
{
	struct iblock_dev *ibd = se_dev->transport->get_dev(se_dev);
	struct block_device *bd = ibd->ibd_bd;
	struct fbdisk_file *fb_file = NULL;
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size);
	int idx, ret;

	fb_file = (struct fbdisk_file *)qnap_transport_get_fbdisk_file(
			bd->bd_disk->private_data, start_lba, &idx);

	if (fb_file) {
		ret = blkdev_issue_flush(bd, GFP_KERNEL);
		if (unlikely(ret)) {
			pr_err("%s: fail to call blkdev_issue_flush, ret:%d\n", 
						__func__, ret);
		}		
		return _file_get_lba_status(fb_file->fb_backing_file, 
				((loff_t)start_lba << bs_order),
				MAX_FILEMAP_SIZE, bs_order, max_desc_count, buf);
	}
	return -EINVAL;
}

static int _fd_blk_sbc_get_lba_status(
	struct se_device *se_dev,
	sector_t start_lba,
	u32 max_desc_count,
	u8 *buf
	)	
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
//	struct file *file = fd_dev->fd_file;
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
#if 0
	//TODO: need to check
	qlib_fd_flush_and_truncate_cache(file, lba_512b, 
		(max_desc_count * sectors_per_dm_blk), bs_order, false, false);
#endif	
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

static int _fd_file_sbc_get_lba_status(
	struct se_device *se_dev,
	sector_t start_lba,
	u32 max_desc_count,
	u8 *buf
	)
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size);

	return _file_get_lba_status(fd_dev->fd_file, ((loff_t)start_lba << bs_order),
				MAX_FILEMAP_SIZE, bs_order, max_desc_count, buf);
}

static int __fd_blk_sbc_unmap_do_unaligned_write_by_desc(
	struct __dev_info *dev_info,
	struct ____align_desc_blk_range	*blk_range,
	int blk_desc_count,
	bool is_head_tail
	)
{
	sector_t write_lba;
	u64 alloc_bytes = ALIGNED_SIZE_BYTES, total_nr_bytes, write_nr_bytes;
	u32 bs_order = dev_info->bs_order;
	int i, done_blks = 0, ret = 0;
	struct __rw_task write_task;

	memset(&write_task, 0, sizeof(struct __rw_task));
	write_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, 
							&write_task.sg_nents);
	if (!write_task.sg_list)
		return -ENOMEM;

	memcpy((void *)&write_task.dev_info, (void *)dev_info, 
						sizeof(struct __dev_info));

	for (i = 0; i < blk_desc_count; i++) {
		if (!blk_range->nr_blks)
			goto next_range;
	
		pr_debug("blk range[%s]: lba:0x%llx, blks:0x%llx\n",
			((is_head_tail) ? "head_tail": "non head_tail"),
			(unsigned long long)blk_range->lba, 
			(unsigned long long)blk_range->nr_blks);

		write_task.dir = DMA_TO_DEVICE;
		write_task.bs_order = bs_order;
		write_task.ret = 0;

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

			if (write_task.ret) {
				ret = write_task.ret;
				goto fail_ret;
			} else
				ret = 0;
	
			if (((sector_t)write_nr_bytes >> bs_order) != done_blks)
				WARN_ON(true);
			
			write_lba += ((sector_t)write_nr_bytes >> bs_order);
			total_nr_bytes -= write_nr_bytes;
		}
next_range:
		blk_range++;
	}

fail_ret:	
	qlib_free_sg_list(write_task.sg_list, write_task.sg_nents);
	return ret;

}

static sense_reason_t __fd_blk_sbc_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct file *file = fd_dev->fd_file;
	struct inode *inode = file_inode(file);
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size);
	u32 aligned_size = ALIGNED_SIZE_BYTES;
	sector_t write_lba;
	int i, ret, done_blks = 0;
	sense_reason_t sense_ret;
	struct ____align_desc_blk desc; 
	struct __dev_info dev_info;

	memset(&dev_info, 0, sizeof(struct __dev_info));
	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* for fio + blk type (for qnap), the unmap only works on the aligned 
	 * range and need to do real-write-zero for other ranges
	 */
	qlib_create_aligned_range_desc(&desc, lba, nolb, bs_order, aligned_size);

	if (!desc.is_aligned) {
		/* for this case, all data size we want to write is smaller
		 * than the dm-thin data block size 
		 */
		ret = __fd_blk_sbc_unmap_do_unaligned_write_by_desc(&dev_info, 
						&desc.body, 1, false);
		goto check_ret;
	} 

	/* we did get aligned range at least, remember to flush data and 
	 * truncate page for the range we want to unmap first
	 */
	ret = qlib_fd_flush_and_truncate_cache(fd_dev->fd_file, desc.body.lba, 
				desc.body.nr_blks, bs_order, true, 
				qlib_thin_lun(&se_dev->dev_dr));
	if (ret != 0) {
		pr_warn("%s: qlib_fd_flush_and_truncate_cache call is fail, "
				"ret: %d\n", __func__, ret);
		goto check_ret;
	}
	
	/* start to unmap here for aligned range, try conver the (lba,nolb) again 
	 * cause of the block size for linux block layer is 512b but upper lun 
	 * block size may be 4096b
	 */
	lba = ((desc.body.lba << bs_order) >> 9);
	nolb = ((desc.body.nr_blks << bs_order) >> 9);

	pr_debug("%s: discard lba:0x%llx, discard blks:0x%llx\n", __func__, 
			(unsigned long long)desc.body.lba,
			(unsigned long long)desc.body.nr_blks);
	
	/* normal discard */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, false, inode->i_bdev, 
					lba, nolb, GFP_KERNEL, 0);
	if (ret != 0) {
		pr_warn("%s: qnap_transport_blkdev_issue_discard call is fail, "
			"ret: %d\n", __func__, ret);
		goto check_ret;
	}

	if (qnap_transport_check_cmd_aborted(se_cmd, false))
		goto check_ret;

	/* for unaligned range writing */
	ret = __fd_blk_sbc_unmap_do_unaligned_write_by_desc(&dev_info, 
				&desc.head_tail[0], MAX_ALIGN_DESC_HT_BLK, true);

check_ret:
	if (!ret)
		sense_ret = TCM_NO_SENSE;
	else {
		if (ret == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;			
		} else if (ret == -ENOMEM)
			sense_ret = TCM_OUT_OF_RESOURCES;
		else 
			sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	return sense_ret;
}

static sense_reason_t __fd_file_sbc_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	/* simple way to use native code, we don't handle more special case 
	 * for qnap feature
	 */
	struct se_device *dev = se_cmd->se_dev;
	return se_cmd->cmd_dr.sbc_ops.sbc_unmap_ops.orig_sbc_core_unmap(se_cmd,
			lba, nolb);
}

static sense_reason_t __ib_fbdisk_sbc_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct iblock_dev *ibd = 
			(struct iblock_dev *)se_dev->transport->get_dev(se_dev);
	
	struct block_device *bd = ibd->ibd_bd;
	u32 bs_order = ilog2(se_cmd->se_dev->dev_attrib.block_size);
	int ret;
	
	/* The kernel block layer is 512b unit, so to convert the lba, nolb again 
	 * if now is 4KB sector size 
	 */
	lba = ((lba << bs_order) >> 9);
	nolb = ((nolb << bs_order) >> 9);
	
	/* normal discard */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, false, bd, lba, 
							nolb, GFP_KERNEL, 0);
	if (ret < 0) {
		pr_err("%s: qnap_transport_blkdev_issue_discard() call is fail, "
			"ret: %d\n", __func__, ret);

		if (ret == -ENOMEM)
			return TCM_OUT_OF_RESOURCES;
	
		if (ret == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		}

		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}	
	return TCM_NO_SENSE;

}

static sense_reason_t __ib_blk_sbc_unmap(
	struct se_cmd *se_cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	return TCM_INVALID_CDB_FIELD;
}

static sense_reason_t _qnap_sbc_ib_fbdisk_ws_fsatzero(
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
	sense_reason_t sense_ret;
	int ret;

	/* The sector unit is 512 in block i/o layer of kernel, 
	 * so need to transfer it again */
	tmp_lba = ((block_lba << bs_order) >> 9);
	tmp_range = ((sectors << bs_order) >> 9);

	/* special discard - fast zero */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, true, bd, tmp_lba, 
			tmp_range, GFP_KERNEL, 0);

	if (!ret) {
		if (!qnap_transport_check_cmd_hit_thin_threshold(se_cmd)) {
			se_cmd->cmd_dr.work_io_err = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;		
			sense_ret = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
		} else
			sense_ret = TCM_NO_SENSE;
	} else {
		pr_warn("%s: qnap_transport_blkdev_issue_discard call is fail, "
			"ret: %d\n", __func__, ret);

		if (ret == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		} else
			sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
	return sense_ret;
}

static sense_reason_t _qnap_sbc_fd_blk_ws_fsatzero(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct file *file = fd_dev->fd_file;
	struct inode *inode = file_inode(file);
	sector_t sectors = sbc_get_write_same_sectors(se_cmd);
	sector_t block_lba = se_cmd->t_task_lba;
	sense_reason_t sense_ret;
	struct ____align_desc_blk desc;	
	struct __dev_info dev_info;
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size);
	u32 aligned_size = ALIGNED_SIZE_BYTES;
	int ret;

	memset(&dev_info, 0, sizeof(struct __dev_info));
	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* for fio + blk type (for qnap), the fastzero only works on aligned
	 * range and need to do read-write-zero for other ranges
	 */
	qlib_create_aligned_range_desc(&desc, block_lba, sectors, bs_order, 
			aligned_size);

	if (!desc.is_aligned) {
		/* for this case, all data size we want to write is smaller
		 * than the dm-thin data block size 
		 */
		ret = _qnap_fd_blk_sbc_ws_do_unaligned_write_by_desc(&dev_info, 
						&desc.body, 1, false);
		goto check_ret;
	} 

	/* we did get aligned range at least, remember to flush data and 
	 * truncate page for the range we want to do write same first-zero first
	 */
	ret = qlib_fd_flush_and_truncate_cache(file, desc.body.lba, 
				desc.body.nr_blks, bs_order, true, 
				qlib_thin_lun(&se_dev->dev_dr));
	if (ret != 0) {
		pr_warn("%s: qlib_fd_flush_and_truncate_cache call is fail, "
				"ret: %d\n", __func__, ret);
		goto check_ret;
	}

	/* start to do write same fastzero here for aligned range, try conver the
	 * (block_lba, sectors) again  cause of the block size for linux block
	 * layer is 512b but upper lun block size may be 4096b
	 */
	block_lba = ((desc.body.lba << bs_order) >> 9);
	sectors = ((desc.body.nr_blks << bs_order) >> 9);

	pr_debug("%s: special discard, lba:0x%llx, blks:0x%llx\n", __func__, 
			(unsigned long long)desc.body.lba, 
			(unsigned long long)desc.body.nr_blks);

	/* special discard - fast zero */
	ret = qnap_transport_blkdev_issue_discard(se_cmd, true, inode->i_bdev, 
					block_lba, sectors, GFP_KERNEL, 0);

	if (ret != 0) {
		pr_warn("%s: qnap_transport_blkdev_issue_discard call is fail, "
			"ret: %d\n", __func__, ret);
		goto check_ret;
	}

	/* for unaligned range writing */
	ret = _qnap_fd_blk_sbc_ws_do_unaligned_write_by_desc(&dev_info, 
				&desc.head_tail[0], MAX_ALIGN_DESC_HT_BLK, true);

check_ret:
	if (!ret) {
		if (!qnap_transport_check_cmd_hit_thin_threshold(se_cmd)) {
			se_cmd->cmd_dr.work_io_err = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;		
			sense_ret = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
		} else
			sense_ret = TCM_NO_SENSE;
	} else {
		if (ret == -ENOSPC) {
			pr_warn("%s: space was full\n", __func__);
			sense_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		} else if (ret == -ENOMEM)
			sense_ret = TCM_OUT_OF_RESOURCES;
		else 
			sense_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
	return sense_ret;

}

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

static int _qnap_sbc_get_lba_status(
	struct se_device *se_dev,
	sector_t start_lba,
	u32 max_desc_count,
	u8 *buf
	)
{
	int ret;

	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr)) {
		/* fio + block dev */
		ret = _fd_blk_sbc_get_lba_status(se_dev, start_lba, 
					max_desc_count, buf);

	} else if (!qlib_is_fio_file_dev(&se_dev->dev_dr)) {
		/* fio + regular file */
		ret = _fd_file_sbc_get_lba_status(se_dev, start_lba, 
					max_desc_count, buf);

	} else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)) {
		/* iblock + fbdisk dev */
		ret = _ib_fbdisk_sbc_get_lba_status(se_dev, start_lba, 
					max_desc_count, buf);

	} else if (!qlib_is_ib_blk_dev(&se_dev->dev_dr)) {
		/* iblock + block dev */
		ret = _ib_blk_sbc_get_lba_status(se_dev, start_lba, 
					max_desc_count, buf);
	} else
		ret = -EOPNOTSUPP;

	return ret;
}


static sense_reason_t do_qnap_sbc_get_lba_status(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	sense_reason_t reason = TCM_INVALID_CDB_FIELD;
	u8 *buf = NULL;
	sector_t start_lba;
	u32 para_data_length, max_desc_count;
	int ret;

	/* only support this command for thin lun */
	if (!qlib_thin_lun(&se_dev->dev_dr))
		return TCM_INVALID_CDB_FIELD;

	/* minimun value should be 24 = (16 bytes descriptor + 8 bytes) */
	if (get_unaligned_be32(&se_cmd->t_task_cdb[10]) < 24 )	
		return TCM_PARAMETER_LIST_LENGTH_ERROR;
	
	if (get_unaligned_be64(&se_cmd->t_task_cdb[2]) >
			se_cmd->se_dev->transport->get_blocks(se_cmd->se_dev))
		return TCM_ADDRESS_OUT_OF_RANGE;

	start_lba = get_unaligned_be64(&se_cmd->t_task_cdb[2]);
	para_data_length = get_unaligned_be32(&se_cmd->t_task_cdb[10]);
	max_desc_count = ((para_data_length - 8) >> 4);
	
	buf = transport_kmap_data_sg(se_cmd);
	if (!buf)
		return TCM_OUT_OF_RESOURCES;

	ret = _qnap_sbc_get_lba_status(se_cmd->se_dev, start_lba, 
						max_desc_count, buf);
	if (ret == 0)
		reason = TCM_NO_SENSE;
	else {
		if (ret == -EOPNOTSUPP)
			reason = TCM_UNSUPPORTED_SCSI_OPCODE;
		else
			reason = TCM_INVALID_PARAMETER_LIST;
	}

	if (buf)
		transport_kunmap_data_sg(se_cmd);

	if (reason == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, GOOD);

	return reason;

}

static int do_qnap_sbc_get_threshold_exp(
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

static sense_reason_t __do_qnap_sbc_core_unmap(
	struct se_cmd *cmd,
	sector_t lba, 
	sector_t nolb
	)
{
	struct se_device *dev = cmd->se_dev;
	int ret;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return __fd_blk_sbc_unmap(cmd, lba, nolb);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		return __fd_file_sbc_unmap(cmd, lba, nolb);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return __ib_fbdisk_sbc_unmap(cmd, lba, nolb);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
		return __ib_blk_sbc_unmap(cmd, lba, nolb);
	}

	return TCM_INVALID_CDB_FIELD; 
}

static void __do_qnap_sbc_unmap_wq(
	struct work_struct *work
	)
{
	struct qnap_se_cmd_dr *dr = container_of(work, struct qnap_se_cmd_dr, 
								unmap_work);
	struct se_cmd *cmd = container_of(dr, struct se_cmd, cmd_dr);
	int ret;

	/* to replace orig core unmap function registered in backend,
	 * we will restore it after the function completed
	 */
	__replace_core_unmap(cmd);
	ret = cmd->cmd_dr.sbc_ops.sbc_unmap_ops.orig_sbc_unmap(cmd);
	__restore_core_unmap(cmd);

	if (ret) {
		spin_lock_irq(&cmd->t_state_lock);
		cmd->transport_state &= ~CMD_T_SENT;
		spin_unlock_irq(&cmd->t_state_lock);
		transport_generic_request_failure(cmd, ret);
	}
}

static sense_reason_t __do_qnap_sbc_unmap(
	struct se_cmd *cmd
	)
{
	struct se_device *dev = cmd->se_dev;
	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;
	int ret;

	if (dev->dev_dr.unmap_wq) {
		INIT_WORK(&cmd->cmd_dr.unmap_work, __do_qnap_sbc_unmap_wq);
		queue_work(dev->dev_dr.unmap_wq,  &cmd->cmd_dr.unmap_work);
		return TCM_NO_SENSE;
	}

	/* to replace orig core unmap function registered in backend,
	 * we will restore it after the function completed
	 */
	__replace_core_unmap(cmd);
	ret = cmd->cmd_dr.sbc_ops.sbc_unmap_ops.orig_sbc_unmap(cmd);
	__restore_core_unmap(cmd);
	return ret;
}

static void __do_qnap_sbc_sync_cache_wq(
	struct work_struct *work
	)
{
	struct qnap_se_cmd_dr *dr = container_of(work, struct qnap_se_cmd_dr, 
							sync_cache_work);
	struct se_cmd *cmd = container_of(dr, struct se_cmd, cmd_dr);
	int ret;

	ret = cmd->cmd_dr.sbc_ops.sbc_sync_ops.orig_sbc_sync_cache(cmd);
	if (ret) {
		spin_lock_irq(&cmd->t_state_lock);
		cmd->transport_state &= ~CMD_T_SENT;
		spin_unlock_irq(&cmd->t_state_lock);
		transport_generic_request_failure(cmd, ret);
	}
}

static sense_reason_t __do_qnap_sbc_sync_cache(
	struct se_cmd *cmd
	)
{
	struct se_device *dev = cmd->se_dev;

	if (dev->dev_dr.sync_cache_wq) {
		INIT_WORK(&cmd->cmd_dr.sync_cache_work,
					__do_qnap_sbc_sync_cache_wq);
		queue_work(dev->dev_dr.sync_cache_wq,  
					&cmd->cmd_dr.sync_cache_work);
		return TCM_NO_SENSE;
	}

	return cmd->cmd_dr.sbc_ops.sbc_sync_ops.orig_sbc_sync_cache(cmd);
}

static sense_reason_t _qnap_sbc_verify_10_16(
	struct se_cmd *se_cmd
	)
{
	struct se_device *dev = se_cmd->se_dev;
	sector_t lba;
	u32 nr_blks;
	sense_reason_t reason;

	/* FIXED ME
	 * we do the sumple implementation here, maybe need to change in the future
	 */

	/* FIXED ME
	 * we don't support protection information now ... 
	 */
	if (se_cmd->t_task_cdb[1] & 0xe0) {
		reason = TCM_INVALID_CDB_FIELD;
		goto exit;
	}
		
	if (se_cmd->t_task_cdb[0] == VERIFY_16) {
		nr_blks = get_unaligned_be32(&se_cmd->t_task_cdb[10]);
		lba = get_unaligned_be64(&se_cmd->t_task_cdb[2]);
	} else {
		nr_blks = get_unaligned_be16(&se_cmd->t_task_cdb[7]);
		lba = get_unaligned_be32(&se_cmd->t_task_cdb[2]);
	}
		
	/* sbcr35j, p174, if nr blks is zero, it means no any blks shall be
	 * transferred or verified. This condition shall not be considered an error 
	 */
	if (!nr_blks) {
		reason = TCM_NO_SENSE;
		goto exit;
	}
		
	if ((lba + nr_blks) > dev->transport->get_blocks(dev) + 1) {
		pr_err("%s: lba + verified len > the capacity size\n", __func__);
		reason = TCM_ADDRESS_OUT_OF_RANGE;
		goto exit;
	}
		
	/* sbc3r35j, p173 */
	if (((se_cmd->t_task_cdb[1] & 0x06) == 0x00)
	|| ((se_cmd->t_task_cdb[1] & 0x06) == 0x04)
	)
	{
		/* sbc3r35j, p173, do nothing here */
		reason = TCM_NO_SENSE;
	} else
		reason = TCM_INVALID_CDB_FIELD;

exit:
	if (reason == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, GOOD);

	return reason;

}

static sense_reason_t _qnap_sbc_core_ws_fastzero(
	struct se_cmd *cmd
	)
{
	struct se_device *dev = cmd->se_dev;
	int ret;

	/* fastzero only works on 
	 * 1) fio + block (qnap thin)
	 * 2) iblock + fbdisk
	 */
	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return _qnap_sbc_fd_blk_ws_fsatzero(cmd);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return _qnap_sbc_ib_fbdisk_ws_fsatzero(cmd);
	}

	return TCM_INVALID_CDB_FIELD; 

}

static int _qnap_sbc_ws_fastzero(
	struct se_cmd *cmd,
	sense_reason_t *s_ret
	)
{
	struct se_device *se_dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct qnap_sbc_write_same_ops *op;
	u8 *buf;
	bool is_zero;

	if (qlib_is_fio_blk_dev(dr) && qlib_is_ib_fbdisk_dev(dr))
		goto out;

	if (!qlib_check_support_special_discard())
		goto out;

	buf = kmap(sg_page(&cmd->t_data_sg[0])) + cmd->t_data_sg[0].offset;
	if (buf) {
		is_zero = qlib_check_zero_buf(buf, cmd->data_length);
		kunmap(sg_page(&cmd->t_data_sg[0]));

		if (is_zero) {
			op = &cmd->cmd_dr.sbc_ops.sbc_ws_ops;
			*s_ret = op->sbc_core_ws_fastzero(cmd);
			if (*s_ret == TCM_NO_SENSE)
				target_complete_cmd(cmd, SAM_STAT_GOOD);
			/* caller will handle remain case */
			return 0;
		}
	}
out:
	return -ENOTSUPP;
}

/**/
void qnap_setup_sbc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	cmd_dr->sbc_ops.sbc_get_threshold_exp = do_qnap_sbc_get_threshold_exp;
	cmd_dr->sbc_ops.sbc_get_lba_status = do_qnap_sbc_get_lba_status;
	cmd_dr->sbc_ops.sbc_verify_10_16 = _qnap_sbc_verify_10_16;

	/* QNAP SBC unmap ops
	 * orig_xxx was used to backup original func, we will call them 
	 */
	cmd_dr->sbc_ops.sbc_unmap_ops.orig_sbc_unmap = NULL;
	cmd_dr->sbc_ops.sbc_unmap_ops.orig_sbc_core_unmap = NULL;
	cmd_dr->sbc_ops.sbc_unmap_ops.sbc_unmap = __do_qnap_sbc_unmap;
	cmd_dr->sbc_ops.sbc_unmap_ops.sbc_core_unmap = __do_qnap_sbc_core_unmap;

	/* QNAP SBC sync-cache ops */
	cmd_dr->sbc_ops.sbc_sync_ops.orig_sbc_sync_cache = NULL;
	cmd_dr->sbc_ops.sbc_sync_ops.sbc_sync_cache = __do_qnap_sbc_sync_cache;

	/* QNAP SBC write-same ops 
	 * - sbc_ws_fastzero() will be called in original write-same function 
	 *   from backend, the necessary checking conditions were already ready
	 *   in origianl write-same code
	 * - sbc_core_ws_fastzero() is real function to handle the fastzero operation
	 * 
	 */
	cmd_dr->sbc_ops.sbc_ws_ops.sbc_ws_fastzero = _qnap_sbc_ws_fastzero;
	cmd_dr->sbc_ops.sbc_ws_ops.sbc_core_ws_fastzero = _qnap_sbc_core_ws_fastzero;

	/* QNAP SBC odx (offload data transfer) ops */
	if (qnap_odx_setup_sbc_ops)
		qnap_odx_setup_sbc_ops(cmd_dr);

}

