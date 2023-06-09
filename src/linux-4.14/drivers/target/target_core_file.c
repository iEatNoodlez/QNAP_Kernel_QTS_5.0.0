/*******************************************************************************
 * Filename:  target_core_file.c
 *
 * This file contains the Storage Engine <-> FILEIO transport specific functions
 *
 * (c) Copyright 2005-2013 Datera, Inc.
 *
 * Nicholas A. Bellinger <nab@kernel.org>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ******************************************************************************/

#include <linux/string.h>
#include <linux/parser.h>
#include <linux/timer.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/falloc.h>
#include <linux/uio.h>
#include <scsi/scsi_proto.h>
#include <asm/unaligned.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>

#include "target_core_file.h"

#ifdef CONFIG_MACH_QNAPTS
#include <qnap/iscsi_priv.h>
#include <target/target_core_fabric.h>
#include "qnap/target_core_qtransport.h"
#include "qnap/target_core_qsbc.h"
static int fileio_cpumask=0xffff;

extern int check_lun_threshold_for_each_device(void *data);

#endif

static inline struct fd_dev *FD_DEV(struct se_device *dev)
{
	return container_of(dev, struct fd_dev, dev);
}

static int fd_attach_hba(struct se_hba *hba, u32 host_id)
{
	struct fd_host *fd_host;

	fd_host = kzalloc(sizeof(struct fd_host), GFP_KERNEL);
	if (!fd_host) {
		pr_err("Unable to allocate memory for struct fd_host\n");
		return -ENOMEM;
	}

	fd_host->fd_host_id = host_id;

	hba->hba_ptr = fd_host;

	pr_debug("CORE_HBA[%d] - TCM FILEIO HBA Driver %s on Generic"
		" Target Core Stack %s\n", hba->hba_id, FD_VERSION,
		TARGET_CORE_VERSION);
	pr_debug("CORE_HBA[%d] - Attached FILEIO HBA: %u to Generic\n",
		hba->hba_id, fd_host->fd_host_id);

	return 0;
}

static void fd_detach_hba(struct se_hba *hba)
{
	struct fd_host *fd_host = hba->hba_ptr;

	pr_debug("CORE_HBA[%d] - Detached FILEIO HBA: %u from Generic"
		" Target Core\n", hba->hba_id, fd_host->fd_host_id);

	kfree(fd_host);
	hba->hba_ptr = NULL;
}

static struct se_device *fd_alloc_device(struct se_hba *hba, const char *name)
{
	struct fd_dev *fd_dev;
	struct fd_host *fd_host = hba->hba_ptr;

	fd_dev = kzalloc(sizeof(struct fd_dev), GFP_KERNEL);
	if (!fd_dev) {
		pr_err("Unable to allocate memory for struct fd_dev\n");
		return NULL;
	}

	fd_dev->fd_host = fd_host;

	pr_debug("FILEIO: Allocated fd_dev for %p\n", name);

	return &fd_dev->dev;
}

static int fd_configure_device(struct se_device *dev)
{
	struct fd_dev *fd_dev = FD_DEV(dev);
	struct fd_host *fd_host = dev->se_hba->hba_ptr;
	struct file *file;
	struct inode *inode = NULL;
	int flags, ret = -EINVAL;

	if (!(fd_dev->fbd_flags & FBDF_HAS_PATH)) {
		pr_err("Missing fd_dev_name=\n");
		return -EINVAL;
	}

	/*
	 * Use O_DSYNC by default instead of O_SYNC to forgo syncing
	 * of pure timestamp updates.
	 */
	flags = O_RDWR | O_CREAT | O_LARGEFILE | O_DSYNC;

	/*
	 * Optionally allow fd_buffered_io=1 to be enabled for people
	 * who want use the fs buffer cache as an WriteCache mechanism.
	 *
	 * This means that in event of a hard failure, there is a risk
	 * of silent data-loss if the SCSI client has *not* performed a
	 * forced unit access (FUA) write, or issued SYNCHRONIZE_CACHE
	 * to write-out the entire device cache.
	 */
	if (fd_dev->fbd_flags & FDBD_HAS_BUFFERED_IO_WCE) {
		pr_debug("FILEIO: Disabling O_DSYNC, using buffered FILEIO\n");
		flags &= ~O_DSYNC;
	}

	file = filp_open(fd_dev->fd_dev_name, flags, 0600);
	if (IS_ERR(file)) {
		pr_err("filp_open(%s) failed\n", fd_dev->fd_dev_name);
		ret = PTR_ERR(file);
		goto fail;
	}
	fd_dev->fd_file = file;
	/*
	 * If using a block backend with this struct file, we extract
	 * fd_dev->fd_[block,dev]_size from struct block_device.
	 *
	 * Otherwise, we use the passed fd_size= from configfs
	 */
	inode = file->f_mapping->host;
	if (S_ISBLK(inode->i_mode)) {
		struct request_queue *q = bdev_get_queue(inode->i_bdev);
		unsigned long long dev_size;

#ifdef CONFIG_MACH_QNAPTS
#ifdef USE_BLKDEV_WRITEPAGES 
		/* turn on multiple-pages-write */
		inode->i_bdev->bd_disk->flags |= GENHD_FL_QNAP_MPAGE;
#endif
		/* consider case if we use 4KB block size */
		if ((dev->dev_dr.dev_flags & QNAP_DF_USING_QLBS) && dev->dev_dr.dev_qlbs) {
			fd_dev->fd_block_size = dev->dev_dr.dev_qlbs;
			/* setup block_size first in target_configure_device() */
			dev->dev_attrib.block_size = fd_dev->fd_block_size;
		} else
#endif
		fd_dev->fd_block_size = bdev_logical_block_size(inode->i_bdev);
		/*
		 * Determine the number of bytes from i_size_read() minus
		 * one (1) logical sector from underlying struct block_device
		 */
		dev_size = (i_size_read(file->f_mapping->host) -
				       fd_dev->fd_block_size);

		pr_debug("FILEIO: Using size: %llu bytes from struct"
			" block_device blocks: %llu logical_block_size: %d\n",
			dev_size, div_u64(dev_size, fd_dev->fd_block_size),
			fd_dev->fd_block_size);

		if (target_configure_unmap_from_queue(&dev->dev_attrib, q))
			pr_debug("IFILE: BLOCK Discard support available,"
				 " disabled by default\n");
		/*
		 * Enable write same emulation for IBLOCK and use 0xFFFF as
		 * the smaller WRITE_SAME(10) only has a two-byte block count.
		 */
		dev->dev_attrib.max_write_same_len = 0xFFFF;

		if (blk_queue_nonrot(q))
			dev->dev_attrib.is_nonrot = 1;
	} else {
		if (!(fd_dev->fbd_flags & FBDF_HAS_SIZE)) {
			pr_err("FILEIO: Missing fd_dev_size="
				" parameter, and no backing struct"
				" block_device\n");
			goto fail;
		}

#ifdef CONFIG_MACH_QNAPTS
		/* consider case if we use 4KB block size */
		if ((dev->dev_dr.dev_flags & QNAP_DF_USING_QLBS) && dev->dev_dr.dev_qlbs) {
			fd_dev->fd_block_size = dev->dev_dr.dev_qlbs;
			/* setup block_size first in target_configure_device() */
			dev->dev_attrib.block_size = fd_dev->fd_block_size;	
		} else
#endif
		fd_dev->fd_block_size = FD_BLOCKSIZE;
		/*
		 * Limit UNMAP emulation to 8k Number of LBAs (NoLB)
		 */
		dev->dev_attrib.max_unmap_lba_count = 0x2000;
		/*
		 * Currently hardcoded to 1 in Linux/SCSI code..
		 */
		dev->dev_attrib.max_unmap_block_desc_count = 1;
		dev->dev_attrib.unmap_granularity = 1;
		dev->dev_attrib.unmap_granularity_alignment = 0;

		/*
		 * Limit WRITE_SAME w/ UNMAP=0 emulation to 8k Number of LBAs (NoLB)
		 * based upon struct iovec limit for vfs_writev()
		 */
		dev->dev_attrib.max_write_same_len = 0x1000;
	}

#ifdef CONFIG_MACH_QNAPTS

	dev->dev_dr.se_dev_thread_cpumask = fileio_cpumask;

	/* Here to call qnap_transport_config_fio_dev() to overwrite
	 * original setting if necessary to do it. Take care the calling position
	 */
	qnap_transport_config_fio_dev(dev);
#endif

	dev->dev_attrib.hw_block_size = fd_dev->fd_block_size;
	dev->dev_attrib.max_bytes_per_io = FD_MAX_BYTES;
	dev->dev_attrib.hw_max_sectors = FD_MAX_BYTES / fd_dev->fd_block_size;
	dev->dev_attrib.hw_queue_depth = FD_MAX_DEVICE_QUEUE_DEPTH;

	if (fd_dev->fbd_flags & FDBD_HAS_BUFFERED_IO_WCE) {
		pr_debug("FILEIO: Forcing setting of emulate_write_cache=1"
			" with FDBD_HAS_BUFFERED_IO_WCE\n");
		dev->dev_attrib.emulate_write_cache = 1;
	}

	fd_dev->fd_dev_id = fd_host->fd_host_dev_id_count++;
	fd_dev->fd_queue_depth = dev->queue_depth;

#ifdef CONFIG_MACH_QNAPTS

 #if defined(Athens)
	pr_debug("CORE_FILE[%u] - Added Cisco FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);
 #elif defined(IS_G)
	pr_debug("CORE_FILE[%u] - Added FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);
 #else
	pr_debug("CORE_FILE[%u] - Added QNAP FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);
 #endif
#else
	pr_debug("CORE_FILE[%u] - Added TCM FILEIO Device ID: %u at %s,"
		" %llu total bytes\n", fd_host->fd_host_id, fd_dev->fd_dev_id,
			fd_dev->fd_dev_name, fd_dev->fd_dev_size);
#endif

	return 0;
fail:
	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}
	return ret;
}

static void fd_dev_call_rcu(struct rcu_head *p)
{
	struct se_device *dev = container_of(p, struct se_device, rcu_head);
	struct fd_dev *fd_dev = FD_DEV(dev);

	kfree(fd_dev);
}

static void fd_free_device(struct se_device *dev)
{
	int under_threshold = 0;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &dev->dev_attrib.dev_attr_dr;

#ifdef CONFIG_MACH_QNAPTS
	dev_attr_dr->tp_threshold_hit = 0;
	check_lun_threshold_for_each_device(&under_threshold);
		
	if (under_threshold) {
#if defined(QNAP_HAL)	
		NETLINK_EVT hal_event;
		
		memset(&hal_event, 0, sizeof(NETLINK_EVT));
		hal_event.type = HAL_EVENT_ISCSI;

		hal_event.arg.action = UNDER_LUN_THRESHOLD;
		hal_event.arg.param.iscsi_lun.lun_index = 0;
		hal_event.arg.param.iscsi_lun.tp_threshold = 0;

		/* unit: GB */
		hal_event.arg.param.iscsi_lun.tp_avail = 0;

		/* call function if it exists since we declare it as weak symbol */
		if (send_hal_netlink)
			send_hal_netlink(&hal_event);
	}
#endif
#endif

	call_rcu(&dev->rcu_head, fd_dev_call_rcu);
}

static void fd_destroy_device(struct se_device *dev)
{
	struct fd_dev *fd_dev = FD_DEV(dev);

	if (fd_dev->fd_file) {
		filp_close(fd_dev->fd_file, NULL);
		fd_dev->fd_file = NULL;
	}
}

static int fd_do_rw(struct se_cmd *cmd, struct file *fd,
		    u32 block_size, struct scatterlist *sgl,
		    u32 sgl_nents, u32 data_length, int is_write)
{
	struct scatterlist *sg;
	struct iov_iter iter;
	struct bio_vec *bvec;
	ssize_t len = 0;
	loff_t pos = (cmd->t_task_lba * block_size);
	int ret = 0, i;

#ifdef CONFIG_MACH_QNAPTS
	struct se_device *se_dev = cmd->se_dev;
#endif

	cmd->b_se_state |= BS_SE_CMD_FD_DO_RW;
	cmd->b_se_state_last = BS_SE_CMD_FD_DO_RW;

	bvec = kcalloc(sgl_nents, sizeof(struct bio_vec), GFP_KERNEL);
	if (!bvec) {
		pr_err("Unable to allocate fd_do_readv iov[]\n");
		return -ENOMEM;
	}

	for_each_sg(sgl, sg, sgl_nents, i) {
		bvec[i].bv_page = sg_page(sg);
		bvec[i].bv_len = sg->length;
		bvec[i].bv_offset = sg->offset;

		len += sg->length;
	}

	iov_iter_bvec(&iter, ITER_BVEC, bvec, sgl_nents, len);

#ifdef CONFIG_MACH_QNAPTS
	qlib_drop_bb_prepare_rec(&se_dev->dev_dr, &cmd->cmd_dr.iov_obj,
		(void *)&iter, len);
#endif

	cmd->b_se_state |= BS_SE_CMD_VFS_ITER_RW;
	cmd->b_se_state_last = BS_SE_CMD_VFS_ITER_RW;
	if (is_write)
		ret = vfs_iter_write(fd, &iter, &pos, 0);
	else
		ret = vfs_iter_read(fd, &iter, &pos, 0);
	cmd->b_se_state |= BS_SE_CMD_VFS_ITER_RW_DONE;
	cmd->b_se_state_last = BS_SE_CMD_VFS_ITER_RW_DONE;


#ifdef CONFIG_MACH_QNAPTS
	qlib_drop_bb_set_rec_null(&se_dev->dev_dr, &cmd->cmd_dr.iov_obj);

	/* always return good since it was dropped and final status will be
	 * prepared in tx again
	 */
	if (qnap_iscsi_iov_is_dropped(&iter)) {
		qlib_drop_bb_set_drop_val(&se_dev->dev_dr, &cmd->cmd_dr.iov_obj, true);

		/* actually, we don't care the ret value  (done bytes for read 
		 * or write) since this command was dropped already
		 */
		kfree(bvec);
		return 1;
	}
#endif

	if (is_write) {
		if (ret < 0 || ret != data_length) {
			pr_err("%s() write returned %d\n", __func__, ret);
			if (ret >= 0)
				ret = -EINVAL;
		}
	} else {
		/*
		 * Return zeros and GOOD status even if the READ did not return
		 * the expected virt_size for struct file w/o a backing struct
		 * block_device.
		 */
		if (S_ISBLK(file_inode(fd)->i_mode)) {
			if (ret < 0 || ret != data_length) {
				pr_err("%s() returned %d, expecting %u for "
						"S_ISBLK\n", __func__, ret,
						data_length);
				if (ret >= 0)
					ret = -EINVAL;
			}
		} else {
			if (ret < 0) {
				pr_err("%s() returned %d for non S_ISBLK\n",
						__func__, ret);
			} else if (ret != data_length) {
				/*
				 * Short read case:
				 * Probably some one truncate file under us.
				 * We must explicitly zero sg-pages to prevent
				 * expose uninizialized pages to userspace.
				 */
				if (ret < data_length)
					ret += iov_iter_zero(data_length - ret, &iter);
				else
					ret = -EINVAL;
			}
		}
	}
	kfree(bvec);
	return ret;
}

static sense_reason_t
fd_execute_sync_cache(struct se_cmd *cmd)
{
	struct se_device *dev = cmd->se_dev;
	struct fd_dev *fd_dev = FD_DEV(dev);
	int immed = (cmd->t_task_cdb[1] & 0x2);
	loff_t start, end;
	int ret;

	cmd->b_se_state |= BS_SE_CMD_FD_EXEC_SYNC;
	cmd->b_se_state_last = BS_SE_CMD_FD_EXEC_SYNC;

	/*
	 * If the Immediate bit is set, queue up the GOOD response
	 * for this SYNCHRONIZE_CACHE op
	 */
	if (immed)
		target_complete_cmd(cmd, SAM_STAT_GOOD);

	/*
	 * Determine if we will be flushing the entire device.
	 */
	if (cmd->t_task_lba == 0 && cmd->data_length == 0) {
		start = 0;
		end = LLONG_MAX;
	} else {
		start = cmd->t_task_lba * dev->dev_attrib.block_size;
		if (cmd->data_length)
			end = start + cmd->data_length - 1;
		else
			end = LLONG_MAX;
	}

	ret = vfs_fsync_range(fd_dev->fd_file, start, end, 1);
	if (ret != 0)
		pr_err("FILEIO: vfs_fsync_range() failed: %d\n", ret);

	if (immed)
		return 0;

	if (ret)
		target_complete_cmd(cmd, SAM_STAT_CHECK_CONDITION);
	else
		target_complete_cmd(cmd, SAM_STAT_GOOD);

	return 0;
}

#ifdef CONFIG_MACH_QNAPTS
static void __qnap_fd_execute_sync_cache_work(
	struct work_struct *work
	)
{
	int ret;
	struct qnap_se_cmd_dr *dr = container_of(work, struct qnap_se_cmd_dr, sync_cache_work);
	struct se_cmd *cmd = container_of(dr, struct se_cmd, cmd_dr);

	ret = fd_execute_sync_cache(cmd);

	if (ret) {
		spin_lock_irq(&cmd->t_state_lock);
		cmd->transport_state &= ~CMD_T_SENT;
		spin_unlock_irq(&cmd->t_state_lock);

		transport_generic_request_failure(cmd, ret);
	}
}

static sense_reason_t qnap_fd_execute_sync_cache(
	struct se_cmd *cmd
	)
{
	if (cmd->se_dev->dev_dr.sync_cache_wq) {
		INIT_WORK(&cmd->cmd_dr.sync_cache_work,
			__qnap_fd_execute_sync_cache_work);
		queue_work(cmd->se_dev->dev_dr.sync_cache_wq,  &cmd->cmd_dr.sync_cache_work);
		return 0;
	}

	return fd_execute_sync_cache(cmd);
}
#endif

static sense_reason_t
fd_execute_write_same(struct se_cmd *cmd)
{
	struct se_device *se_dev = cmd->se_dev;
	struct fd_dev *fd_dev = FD_DEV(se_dev);
	loff_t pos = cmd->t_task_lba * se_dev->dev_attrib.block_size;
	sector_t nolb = sbc_get_write_same_sectors(cmd);
	struct iov_iter iter;
	struct bio_vec *bvec;
	unsigned int len = 0, i;
	ssize_t ret;

	cmd->b_se_state |= BS_SE_CMD_FD_EXEC_WRITE_SAME;
	cmd->b_se_state_last = BS_SE_CMD_FD_EXEC_WRITE_SAME;

#ifdef CONFIG_MACH_QNAPTS
#ifdef QNAP_TARGET_ZMC_FEATURE	
	if (!cmd->zmc_data.valid) 
		goto no_zmc;
#ifdef QNAP_ISCSI_TCP_ZMC_FEATURE
	if (qnap_transport_check_iscsi_fabric(cmd)) {
		struct target_iscsi_zmc_data *p = &cmd->zmc_data.u.izmc;
		return p->scsi_ops->write_same(cmd, NULL);
	}
#endif

no_zmc:
#endif
#endif

	if (!nolb) {
		target_complete_cmd(cmd, SAM_STAT_GOOD);
		return 0;
	}
	if (cmd->prot_op) {
		pr_err("WRITE_SAME: Protection information with FILEIO"
		       " backends not supported\n");
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	if (cmd->t_data_nents > 1 ||
	    cmd->t_data_sg[0].length != cmd->se_dev->dev_attrib.block_size) {
		pr_err("WRITE_SAME: Illegal SGL t_data_nents: %u length: %u"
			" block_size: %u\n",
			cmd->t_data_nents,
			cmd->t_data_sg[0].length,
			cmd->se_dev->dev_attrib.block_size);
		return TCM_INVALID_CDB_FIELD;
	}

#ifdef CONFIG_MACH_QNAPTS
	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;
	sense_reason_t sr_ret;

	if (ops->execute_qnap_ws_is_zero_buf 
	&& ops->execute_qnap_ws_fast_zero
	)
	{
		if (qlib_check_support_special_discard()
		&& ops->execute_qnap_ws_is_zero_buf(cmd)
		)
		{
			sr_ret = ops->execute_qnap_ws_fast_zero(cmd);
			if (sr_ret == TCM_NO_SENSE)
				target_complete_cmd(cmd, SAM_STAT_GOOD);

			return sr_ret;	
		}
	}
	/* fall-back */

#endif

	bvec = kcalloc(nolb, sizeof(struct bio_vec), GFP_NOWAIT);
	if (!bvec){
		/* try to get it with vmalloc again.. */
		bvec = vmalloc(nolb * sizeof(struct bio_vec));
		if (!bvec){
			return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}
	}

	for (i = 0; i < nolb; i++) {
		bvec[i].bv_page = sg_page(&cmd->t_data_sg[0]);
		bvec[i].bv_len = cmd->t_data_sg[0].length;
		bvec[i].bv_offset = cmd->t_data_sg[0].offset;

		len += se_dev->dev_attrib.block_size;
	}

	iov_iter_bvec(&iter, ITER_BVEC, bvec, nolb, len);
	
	cmd->b_se_state |= BS_SE_CMD_VFS_ITER_RW;
	cmd->b_se_state_last = BS_SE_CMD_VFS_ITER_RW;
	
	ret = vfs_iter_write(fd_dev->fd_file, &iter, &pos, 0);
	
	cmd->b_se_state |= BS_SE_CMD_VFS_ITER_RW_DONE;
	cmd->b_se_state_last = BS_SE_CMD_VFS_ITER_RW_DONE;

	if (is_vmalloc_addr(bvec))
		vfree(bvec);
	else
		kfree(bvec);
	if (ret < 0 || ret != len) {
		pr_err("vfs_iter_write() returned %zd for write same\n", ret);
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}

static int
fd_do_prot_fill(struct se_device *se_dev, sector_t lba, sector_t nolb,
		void *buf, size_t bufsize)
{
	struct fd_dev *fd_dev = FD_DEV(se_dev);
	struct file *prot_fd = fd_dev->fd_prot_file;
	sector_t prot_length, prot;
	loff_t pos = lba * se_dev->prot_length;

	if (!prot_fd) {
		pr_err("Unable to locate fd_dev->fd_prot_file\n");
		return -ENODEV;
	}

	prot_length = nolb * se_dev->prot_length;

	for (prot = 0; prot < prot_length;) {
		sector_t len = min_t(sector_t, bufsize, prot_length - prot);
		ssize_t ret = kernel_write(prot_fd, buf, len, &pos);

		if (ret != len) {
			pr_err("vfs_write to prot file failed: %zd\n", ret);
			return ret < 0 ? ret : -ENODEV;
		}
		prot += ret;
	}

	return 0;
}

static int
fd_do_prot_unmap(struct se_cmd *cmd, sector_t lba, sector_t nolb)
{
	void *buf;
	int rc;

	buf = (void *)__get_free_page(GFP_KERNEL);
	if (!buf) {
		pr_err("Unable to allocate FILEIO prot buf\n");
		return -ENOMEM;
	}
	memset(buf, 0xff, PAGE_SIZE);

	rc = fd_do_prot_fill(cmd->se_dev, lba, nolb, buf, PAGE_SIZE);

	free_page((unsigned long)buf);

	return rc;
}

static sense_reason_t
fd_execute_unmap(struct se_cmd *cmd, sector_t lba, sector_t nolb)
{
	struct file *file = FD_DEV(cmd->se_dev)->fd_file;
	struct inode *inode = file->f_mapping->host;
	int ret;

	cmd->b_se_state |= BS_SE_CMD_FD_EXEC_UNMAP;
	cmd->b_se_state_last = BS_SE_CMD_FD_EXEC_UNMAP;

	if (!nolb) {
		return 0;
	}

	if (cmd->se_dev->dev_attrib.pi_prot_type) {
		ret = fd_do_prot_unmap(cmd, lba, nolb);
		if (ret)
			return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	if (S_ISBLK(inode->i_mode)) {

		/* The backend is block device, use discard */
		struct block_device *bdev = inode->i_bdev;
		struct se_device *dev = cmd->se_dev;

		ret = blkdev_issue_discard(bdev,
					   target_to_linux_sector(dev, lba),
					   target_to_linux_sector(dev,  nolb),
					   GFP_KERNEL, 0);
		if (ret < 0) {
			pr_warn("FILEIO: blkdev_issue_discard() failed: %d\n",
				ret);
			return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}
	} else {
		/* The backend is normal file, use fallocate */
		struct se_device *se_dev = cmd->se_dev;
		loff_t pos = lba * se_dev->dev_attrib.block_size;
		unsigned int len = nolb * se_dev->dev_attrib.block_size;
		int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

		if (!file->f_op->fallocate)
			return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

		ret = file->f_op->fallocate(file, mode, pos, len);
		if (ret < 0) {
			pr_warn("FILEIO: fallocate() failed: %d\n", ret);
			return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}
	}

	cmd->b_se_state |= BS_SE_CMD_FD_EXEC_UNMAP_DONE;
	cmd->b_se_state_last = BS_SE_CMD_FD_EXEC_UNMAP_DONE;

	return 0;
}

static sense_reason_t
fd_execute_rw(struct se_cmd *cmd, struct scatterlist *sgl, u32 sgl_nents,
	      enum dma_data_direction data_direction)
{
	struct se_device *dev = cmd->se_dev;
	struct fd_dev *fd_dev = FD_DEV(dev);
	struct file *file = fd_dev->fd_file;
	struct file *pfile = fd_dev->fd_prot_file;
	sense_reason_t rc;
	int ret = 0;
	/*
	 * We are currently limited by the number of iovecs (2048) per
	 * single vfs_[writev,readv] call.
	 */

	cmd->b_se_state |= BS_SE_CMD_FD_EXEC_RW;
	cmd->b_se_state_last = BS_SE_CMD_FD_EXEC_RW;
	
	if (cmd->data_length > FD_MAX_BYTES) {
		pr_err("FILEIO: Not able to process I/O of %u bytes due to"
		       "FD_MAX_BYTES: %u iovec count limitation\n",
			cmd->data_length, FD_MAX_BYTES);
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
	/*
	 * Call vectorized fileio functions to map struct scatterlist
	 * physical memory addresses to struct iovec virtual memory.
	 */
	if (data_direction == DMA_FROM_DEVICE) {
		if (cmd->prot_type && dev->dev_attrib.pi_prot_type) {
			ret = fd_do_rw(cmd, pfile, dev->prot_length,
				       cmd->t_prot_sg, cmd->t_prot_nents,
				       cmd->prot_length, 0);
			cmd->b_se_state |= BS_SE_CMD_FD_DO_RW_RETURN;
			cmd->b_se_state_last = BS_SE_CMD_FD_DO_RW_RETURN;
			if (ret < 0)
				return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}

		ret = fd_do_rw(cmd, file, dev->dev_attrib.block_size,
			       sgl, sgl_nents, cmd->data_length, 0);
		cmd->b_se_state |= BS_SE_CMD_FD_DO_RW_RETURN;
		cmd->b_se_state_last = BS_SE_CMD_FD_DO_RW_RETURN;

#ifdef CONFIG_MACH_QNAPTS
		if (qlib_drop_bb_get_drop_val(&dev->dev_dr, &cmd->cmd_dr.iov_obj)) {
			/* always return good if cmd was dropped, the real status
			 * will be handled in tx
			 */
			qlib_drop_bb_set_drop_val(&dev->dev_dr, 
				&cmd->cmd_dr.iov_obj, false);
			target_complete_cmd(cmd, SAM_STAT_GOOD);
			return 0;
		}
#endif

		if (ret > 0 && cmd->prot_type && dev->dev_attrib.pi_prot_type &&
		    dev->dev_attrib.pi_prot_verify) {
			u32 sectors = cmd->data_length >>
					ilog2(dev->dev_attrib.block_size);

			rc = sbc_dif_verify(cmd, cmd->t_task_lba, sectors,
					    0, cmd->t_prot_sg, 0);
			if (rc)
				return rc;
		}
	} else {

#if defined(CONFIG_MACH_QNAPTS) && defined(SUPPORT_ISCSI_ZERO_COPY)
		if (cmd->cmd_dr.digest_zero_copy_skip == false) {
			ret = 1;
			goto _EXIT_2_;
		}
#endif
		if (cmd->prot_type && dev->dev_attrib.pi_prot_type &&
		    dev->dev_attrib.pi_prot_verify) {
			u32 sectors = cmd->data_length >>
					ilog2(dev->dev_attrib.block_size);

			rc = sbc_dif_verify(cmd, cmd->t_task_lba, sectors,
					    0, cmd->t_prot_sg, 0);
			if (rc)
				return rc;
		}

		ret = fd_do_rw(cmd, file, dev->dev_attrib.block_size,
			       sgl, sgl_nents, cmd->data_length, 1);
		cmd->b_se_state |= BS_SE_CMD_FD_DO_RW_RETURN;
		cmd->b_se_state_last = BS_SE_CMD_FD_DO_RW_RETURN;

#ifdef CONFIG_MACH_QNAPTS
_EXIT_2_:
		if (qlib_drop_bb_get_drop_val(&dev->dev_dr, &cmd->cmd_dr.iov_obj)) {
			/* always return good if cmd was dropped, the real status
			 * will be handled in tx
			 */
			qlib_drop_bb_set_drop_val(&dev->dev_dr, &cmd->cmd_dr.iov_obj, false);
			target_complete_cmd(cmd, SAM_STAT_GOOD);
			return 0;
		}

#ifdef SUPPORT_TP
		/* after every write operation, we check whether data size hits
		 * thin threshold or not. If yes, it shall response the UA status
		 */
		if (ret > 0) {
			if (qnap_transport_check_cmd_hit_thin_threshold(cmd) == 0)
				return TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
		}
#endif
#ifdef SUPPORT_ISCSI_ZERO_COPY
		if (cmd->cmd_dr.digest_zero_copy_skip == false)
			goto _EXIT_1_;
#endif
#ifdef SUPPORT_TP
		/* we did write for write back behavior, so to go this way
		 * if it is thin lun, write operatin to thin lun will be
		 * sync i/o if occupied data size hits the sync i/o threshold
		 * of pool
		 */
		int tmp_ret;

		/* ret is written bytes from fd_do_rw() */
		tmp_ret = ret;

		qnap_transport_upadte_write_result_on_fio_thin(cmd, &tmp_ret);
		if (tmp_ret != ret)
			ret = tmp_ret;
#endif
_EXIT_1_:
#endif
		/*
		 * Perform implicit vfs_fsync_range() for fd_do_writev() ops
		 * for SCSI WRITEs with Forced Unit Access (FUA) set.
		 * Allow this to happen independent of WCE=0 setting.
		 */
		if (ret > 0 && (cmd->se_cmd_flags & SCF_FUA)) {
			loff_t start = cmd->t_task_lba *
				dev->dev_attrib.block_size;
			loff_t end;

			if (cmd->data_length)
				end = start + cmd->data_length - 1;
			else
				end = LLONG_MAX;

#if defined(CONFIG_MACH_QNAPTS) && defined(SUPPORT_TP)
			ret =  qlib_vfs_fsync_range(fd_dev->fd_file, start, 
				end, 1, qlib_thin_lun(&dev->dev_dr));

			if (ret == 0)
				ret = 1;
#else
			vfs_fsync_range(fd_dev->fd_file, start, end, 1);
#endif
		}

		if (ret > 0 && cmd->prot_type && dev->dev_attrib.pi_prot_type) {
			ret = fd_do_rw(cmd, pfile, dev->prot_length,
				       cmd->t_prot_sg, cmd->t_prot_nents,
				       cmd->prot_length, 1);
			cmd->b_se_state |= BS_SE_CMD_FD_DO_RW_RETURN;
			cmd->b_se_state_last = BS_SE_CMD_FD_DO_RW_RETURN;
			if (ret < 0)
				return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		}
	}

#if defined(CONFIG_MACH_QNAPTS) && defined(SUPPORT_TP)
	if (ret == -ENOSPC)
		return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
#endif
	if (ret < 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}

enum {
	Opt_fd_dev_name, Opt_fd_dev_size, Opt_fd_buffered_io, Opt_err
};

static match_table_t tokens = {
	{Opt_fd_dev_name, "fd_dev_name=%s"},
	{Opt_fd_dev_size, "fd_dev_size=%s"},
	{Opt_fd_buffered_io, "fd_buffered_io=%d"},
	{Opt_err, NULL}
};

static ssize_t fd_set_configfs_dev_params(struct se_device *dev,
		const char *page, ssize_t count)
{
	struct fd_dev *fd_dev = FD_DEV(dev);
	char *orig, *ptr, *arg_p, *opts;
	substring_t args[MAX_OPT_ARGS];
	int ret = 0, arg, token;

	opts = kstrdup(page, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	orig = opts;

	while ((ptr = strsep(&opts, ",\n")) != NULL) {
		if (!*ptr)
			continue;

		token = match_token(ptr, tokens, args);
		switch (token) {
		case Opt_fd_dev_name:
			if (match_strlcpy(fd_dev->fd_dev_name, &args[0],
				FD_MAX_DEV_NAME) == 0) {
				ret = -EINVAL;
				break;
			}
			pr_debug("FILEIO: Referencing Path: %s\n",
					fd_dev->fd_dev_name);
			fd_dev->fbd_flags |= FBDF_HAS_PATH;
			break;
		case Opt_fd_dev_size:
			arg_p = match_strdup(&args[0]);
			if (!arg_p) {
				ret = -ENOMEM;
				break;
			}
			ret = kstrtoull(arg_p, 0, &fd_dev->fd_dev_size);
			kfree(arg_p);
			if (ret < 0) {
				pr_err("kstrtoull() failed for"
						" fd_dev_size=\n");
				goto out;
			}
			pr_debug("FILEIO: Referencing Size: %llu"
					" bytes\n", fd_dev->fd_dev_size);
			fd_dev->fbd_flags |= FBDF_HAS_SIZE;
			break;
		case Opt_fd_buffered_io:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			if (arg != 1) {
				pr_err("bogus fd_buffered_io=%d value\n", arg);
				ret = -EINVAL;
				goto out;
			}

			pr_debug("FILEIO: Using buffered I/O"
				" operations for struct fd_dev\n");

			fd_dev->fbd_flags |= FDBD_HAS_BUFFERED_IO_WCE;
			break;
		default:
			break;
		}
	}

out:
	kfree(orig);
	return (!ret) ? count : ret;
}

static ssize_t fd_show_configfs_dev_params(struct se_device *dev, char *b)
{
	struct fd_dev *fd_dev = FD_DEV(dev);
	ssize_t bl = 0;

#ifdef CONFIG_MACH_QNAPTS
 #if defined(Athens)
	bl = sprintf(b + bl, "Cisco FILEIO ID: %u", fd_dev->fd_dev_id);
 #elif defined(IS_G)
	bl = sprintf(b + bl, "FILEIO ID: %u", fd_dev->fd_dev_id);
 #else
	bl = sprintf(b + bl, "QNAP FILEIO ID: %u", fd_dev->fd_dev_id);
 #endif
#else
	bl = sprintf(b + bl, "TCM FILEIO ID: %u", fd_dev->fd_dev_id);
#endif
	bl += sprintf(b + bl, "        File: %s  Size: %llu  Mode: %s\n",
		fd_dev->fd_dev_name, fd_dev->fd_dev_size,
		(fd_dev->fbd_flags & FDBD_HAS_BUFFERED_IO_WCE) ?
		"Buffered-WCE" : "O_DSYNC");
	return bl;
}

static sector_t fd_get_blocks(struct se_device *dev)
{
	struct fd_dev *fd_dev = FD_DEV(dev);
	struct file *f = fd_dev->fd_file;
	struct inode *i = f->f_mapping->host;
	unsigned long long dev_size;
	/*
	 * When using a file that references an underlying struct block_device,
	 * ensure dev_size is always based on the current inode size in order
	 * to handle underlying block_device resize operations.
	 */
	if (S_ISBLK(i->i_mode))
		dev_size = i_size_read(i);
	else
		dev_size = fd_dev->fd_dev_size;

	return div_u64(dev_size - dev->dev_attrib.block_size,
		       dev->dev_attrib.block_size);
}

static int fd_init_prot(struct se_device *dev)
{
	struct fd_dev *fd_dev = FD_DEV(dev);
	struct file *prot_file, *file = fd_dev->fd_file;
	struct inode *inode;
	int ret, flags = O_RDWR | O_CREAT | O_LARGEFILE | O_DSYNC;
	char buf[FD_MAX_DEV_PROT_NAME];

	if (!file) {
		pr_err("Unable to locate fd_dev->fd_file\n");
		return -ENODEV;
	}

	inode = file->f_mapping->host;
	if (S_ISBLK(inode->i_mode)) {
		pr_err("FILEIO Protection emulation only supported on"
		       " !S_ISBLK\n");
		return -ENOSYS;
	}

	if (fd_dev->fbd_flags & FDBD_HAS_BUFFERED_IO_WCE)
		flags &= ~O_DSYNC;

	snprintf(buf, FD_MAX_DEV_PROT_NAME, "%s.protection",
		 fd_dev->fd_dev_name);

	prot_file = filp_open(buf, flags, 0600);
	if (IS_ERR(prot_file)) {
		pr_err("filp_open(%s) failed\n", buf);
		ret = PTR_ERR(prot_file);
		return ret;
	}
	fd_dev->fd_prot_file = prot_file;

	return 0;
}

static int fd_format_prot(struct se_device *dev)
{
	unsigned char *buf;
	int unit_size = FDBD_FORMAT_UNIT_SIZE * dev->dev_attrib.block_size;
	int ret;

	if (!dev->dev_attrib.pi_prot_type) {
		pr_err("Unable to format_prot while pi_prot_type == 0\n");
		return -ENODEV;
	}

	buf = vzalloc(unit_size);
	if (!buf) {
		pr_err("Unable to allocate FILEIO prot buf\n");
		return -ENOMEM;
	}

	pr_debug("Using FILEIO prot_length: %llu\n",
		 (unsigned long long)(dev->transport->get_blocks(dev) + 1) *
					dev->prot_length);

	memset(buf, 0xff, unit_size);
	ret = fd_do_prot_fill(dev, 0, dev->transport->get_blocks(dev) + 1,
			      buf, unit_size);
	vfree(buf);
	return ret;
}

static void fd_free_prot(struct se_device *dev)
{
	struct fd_dev *fd_dev = FD_DEV(dev);

	if (!fd_dev->fd_prot_file)
		return;

	filp_close(fd_dev->fd_prot_file, NULL);
	fd_dev->fd_prot_file = NULL;
}

#ifdef CONFIG_MACH_QNAPTS

static void *qnap_get_fd_dev(
	struct se_device *se_dev
	)
{
	return (void *)FD_DEV(se_dev);
}

#endif

static struct sbc_ops fd_sbc_ops = {
	.execute_rw		= fd_execute_rw,
	.execute_sync_cache	= fd_execute_sync_cache,
	.execute_write_same	= fd_execute_write_same,
	.execute_unmap		= fd_execute_unmap,
#ifdef CONFIG_MACH_QNAPTS
	.execute_qnap_sync_cache     = qnap_fd_execute_sync_cache,
#ifdef SUPPORT_TP
	.execute_qnap_get_lba_status = qnap_sbc_get_lba_status,
	.execute_qnap_unmap          = qnap_sbc_unmap,
#endif
	.execute_qnap_ws_fast_zero   = qnap_sbc_write_same_fast_zero,
	.execute_qnap_ws_is_zero_buf = qnap_sbc_write_same_is_zero_buf,
#endif
};

static sense_reason_t
fd_parse_cdb(struct se_cmd *cmd)
{
	return sbc_parse_cdb(cmd, &fd_sbc_ops);
}

static const struct target_backend_ops fileio_ops = {
	.name			= "fileio",
#ifdef CONFIG_MACH_QNAPTS
	.inquiry_prod		= "Storage",
#else
	.inquiry_prod		= "FILEIO",
#endif
	.inquiry_rev		= FD_VERSION,
	.owner			= THIS_MODULE,
	.attach_hba		= fd_attach_hba,
	.detach_hba		= fd_detach_hba,
	.alloc_device		= fd_alloc_device,
	.configure_device	= fd_configure_device,
	.destroy_device		= fd_destroy_device,
	.free_device		= fd_free_device,
	.parse_cdb		= fd_parse_cdb,
	.set_configfs_dev_params = fd_set_configfs_dev_params,
	.show_configfs_dev_params = fd_show_configfs_dev_params,
	.get_device_type	= sbc_get_device_type,
	.get_blocks		= fd_get_blocks,
	.init_prot		= fd_init_prot,
	.format_prot		= fd_format_prot,
	.free_prot		= fd_free_prot,
	.tb_dev_attrib_attrs	= sbc_attrib_attrs,
#ifdef CONFIG_MACH_QNAPTS
	.change_dev_size	= qnap_change_dev_size,
	.set_write_cache	= qnap_set_write_cache,
	.get_dev		= qnap_get_fd_dev,
	.get_io_min		= qnap_sbc_get_io_min,
	.get_io_opt		= qnap_sbc_get_io_opt,
#endif
};

static int __init fileio_module_init(void)
{
	return transport_backend_register(&fileio_ops);
}

static void __exit fileio_module_exit(void)
{
	target_backend_unregister(&fileio_ops);
}

MODULE_DESCRIPTION("TCM FILEIO subsystem plugin");
MODULE_AUTHOR("nab@Linux-iSCSI.org");
MODULE_LICENSE("GPL");

#if defined(CONFIG_MACH_QNAPTS)
module_param(fileio_cpumask, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(fileio_cpumask, "FILEIO thread binding CPU");
#endif

module_init(fileio_module_init);
module_exit(fileio_module_exit);
