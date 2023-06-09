/*******************************************************************************
 * Filename:  target_core_qtransport.c
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
#include <qnap/fbdisk.h>

#include "../target_core_internal.h"
#include "../target_core_iblock.h"
#include "../target_core_file.h"
#include "../target_core_pr.h"

#include <qnap/iscsi_priv.h>

#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qsbc.h"
#include "target_core_qspc.h"
#include "target_core_qfbc.h"

#ifdef CONFIG_QTS_CEPH
#include "target_core_rbd.h"
#endif
#ifdef SUPPORT_TPC_CMD
#include "target_core_qodx.h"
#endif

#ifdef QNAP_TARGET_PERF_FEATURE
#include "target_core_perf_en.h"
#endif

extern int check_lun_threshold_for_each_device(   void *data);

/**/
static char *__qnap_transport_get_drop_type_str(int type);

#ifdef CONFIG_QTS_CEPH
static inline struct tcm_rbd_dev *TCM_RBD_DEV(struct se_device *dev)
{
	return container_of(dev, struct tcm_rbd_dev, dev);
}
#endif

static struct bio_rec *__qnap_transport_alloc_fb_bio_rec(
	struct se_device *dev
	)
{
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;

	return kmem_cache_zalloc(dev_dr->fb_bio_rec_kmem, GFP_KERNEL);
}

static void __qnap_transport_free_fb_bio_rec(
	struct se_device *dev,
	struct bio_rec *p	
	)
{
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;

	if (qlib_is_ib_fbdisk_dev(dev_dr))
		return;

	if (dev_dr->fb_bio_rec_kmem && p)
		kmem_cache_free(dev_dr->fb_bio_rec_kmem, p);
}

static void __qnap_transport_init_drain_io_bio_obj(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	struct __bio_obj *obj = &cmd_dr->bio_obj;
	spin_lock_init(&obj->bio_rec_lists_lock);
	INIT_LIST_HEAD(&obj->bio_rec_lists);
	atomic_set(&obj->bio_rec_count, 1);
}

static void __qnap_transport_init_drain_io_iov_obj(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	struct __iov_obj *obj = &cmd_dr->iov_obj;
	spin_lock_init(&obj->iov_rec_lock);
	obj->iov_rec = NULL;
	obj->io_drained = false;
}

static bool __qnap_transport_check_fio_io_drained(
	struct se_cmd *cmd
	)
{
	struct __iov_obj *obj = &cmd->cmd_dr.iov_obj;
	return obj->io_drained;
}

static bool __qnap_transport_check_ib_fbdisk_io_drained(
	struct se_cmd *cmd
	)
{
	struct __bio_obj *obj = &cmd->cmd_dr.bio_obj;
	return ((atomic_read(&obj->bio_drain_count)) ? true : false);
}

static void __qnap_transport_release_drain_io_fio_res(
	struct se_cmd *cmd
	)
{
	return;
}

static void __qnap_transport_release_drain_io_ib_fbdisk_res(
	struct se_cmd *cmd
	)
{
	struct __bio_obj *obj = &cmd->cmd_dr.bio_obj;
	struct bio_rec *brec = NULL, *tmp_bio_rec = NULL;
	LIST_HEAD(__free_list);

	spin_lock_bh(&obj->bio_rec_lists_lock);
	pr_debug("bio rec count:%d\n", atomic_read(&obj->bio_rec_count));	

	list_for_each_entry_safe(brec, tmp_bio_rec, &obj->bio_rec_lists, node)
		list_move_tail(&brec->node, &__free_list);

	spin_unlock_bh(&obj->bio_rec_lists_lock);

	list_for_each_entry_safe(brec, tmp_bio_rec, &__free_list, node) {
		list_del_init(&brec->node);
		atomic_dec(&obj->bio_rec_count);
		__qnap_transport_free_fb_bio_rec(cmd->se_dev, brec);
	}

}

static int __qnap_transport_drain_fio_io(
	struct se_cmd *cmd,
	int type
	)
{
	struct __iov_obj *obj = &cmd->cmd_dr.iov_obj;
	int ret;
	
	if (type == -1)
		set_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &cmd->cmd_dr.cmd_t_state);

	/* if found obj->iov_rec, it means cmd was submitted in fd_do_rw()
	 * already but may not be back yet
	 */	
	spin_lock(&obj->iov_rec_lock);
	if (obj->iov_rec)
		qnap_iscsi_iov_set_drain((struct iov_iter *)obj->iov_rec);
	spin_unlock(&obj->iov_rec_lock);
	return 0;
}

static int __qnap_transport_drain_ib_fbdisk_io(
	struct se_cmd *cmd,
	int type
	)
{
	struct __bio_obj *obj = &cmd->cmd_dr.bio_obj;
	struct se_device *dev = cmd->se_dev;
	struct iblock_dev *ib_dev = NULL;
	struct fbdisk_device *fb_dev = NULL;
	struct bio_rec *brec, *tmp_brec;

	ib_dev = (struct iblock_dev *)dev->transport->get_dev(dev);
	fb_dev = ib_dev->ibd_bd->bd_disk->private_data;
	if (!fb_dev)
		return -ENODEV;

	if (type == -1)
		set_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &cmd->cmd_dr.cmd_t_state);

	/* take care this ..., caller hold lock with spin_xxx_bh() already */
	spin_lock(&obj->bio_rec_lists_lock);
	list_for_each_entry_safe(brec, tmp_brec, &obj->bio_rec_lists, node)
	{
		if (brec->bio) {
			qnap_iscsi_fbdisk_set_bio_drain(brec);
			atomic_inc(&obj->bio_drain_count);
		}
	}
	spin_unlock(&obj->bio_rec_lists_lock);
	return 0;

}

static int __qnap_transport_after_drain_fio_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct __iov_obj *obj = &cmd->cmd_dr.iov_obj;
	struct iov_iter *iter = (struct iov_iter *)obj->iov_rec;

	spin_lock(&obj->iov_rec_lock);
	obj->iov_rec = NULL;
	spin_unlock(&obj->iov_rec_lock);

	/* drain command successfully ? */
	if (qnap_iscsi_iov_is_drained(iter))
		obj->io_drained = true;
	return 0;
}

static int __qnap_transport_after_drain_ib_fbdisk_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct __bio_obj *obj = &cmd->cmd_dr.bio_obj;
	struct bio *bio = (struct bio *)data->data0;
	struct bio_rec *brec, *tmp_brec;

	/* TODO: any smart method ? */
	spin_lock(&obj->bio_rec_lists_lock);
	list_for_each_entry_safe(brec, tmp_brec, &obj->bio_rec_lists, node) {
		if (brec->bio == bio)
			brec->bio = NULL;
	}
	spin_unlock(&obj->bio_rec_lists_lock);
	return 0;
}

static int __qnap_transport_pre_drain_fio_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct __iov_obj *obj = &cmd->cmd_dr.iov_obj;

	spin_lock(&obj->iov_rec_lock);
	obj->iov_rec = data->data0;
	spin_unlock(&obj->iov_rec_lock);
	return 0;
}

static int __qnap_transport_pre_drain_ib_fbdisk_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct se_device *dev = cmd->se_dev;
	struct __bio_obj *obj = &cmd->cmd_dr.bio_obj;
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;
	struct bio_rec *brec = NULL;
	struct bio *bio = NULL;

	if (!dev_dr->fb_bio_rec_kmem)
		goto out;

	brec = __qnap_transport_alloc_fb_bio_rec(dev);
	if (brec) {
		INIT_LIST_HEAD(&brec->node);
		brec->misc = data->data0;
		brec->bio = bio = (struct bio *)data->data1;
		brec->flags = 0;
		bio->bi_private = qnap_bi_private_set_brec_bit((void *)brec);

		spin_lock_bh(&obj->bio_rec_lists_lock);
		list_add_tail(&brec->node, &obj->bio_rec_lists);
		atomic_inc(&obj->bio_rec_count);
		spin_unlock_bh(&obj->bio_rec_lists_lock);
		atomic_set(&obj->bio_drain_count, 0);
		return 0;
	}
out:
	return -ENOMEM;

}

static int __qnap_transport_pre_drain_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return __qnap_transport_pre_drain_fio_io(cmd, data);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		return __qnap_transport_pre_drain_fio_io(cmd, data);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return __qnap_transport_pre_drain_ib_fbdisk_io(cmd, data);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
		return -ENOTSUPP;
	}

	return -ENOTSUPP;

}

static int __qnap_transport_after_drain_io(
	struct se_cmd *cmd,
	struct drain_io_data *data
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return __qnap_transport_after_drain_fio_io(cmd, data);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		return __qnap_transport_after_drain_fio_io(cmd, data);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return __qnap_transport_after_drain_ib_fbdisk_io(cmd, data);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
		return -ENOTSUPP;
	}

	return -ENOTSUPP;
}

static int __qnap_transport_drain_io(
	struct se_cmd *cmd,
	int type
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return __qnap_transport_drain_fio_io(cmd, type);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		return __qnap_transport_drain_fio_io(cmd, type);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return __qnap_transport_drain_ib_fbdisk_io(cmd, type);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
		return -ENOTSUPP;
	}

	return -ENOTSUPP;

}

static void __qnap_transport_release_drain_io_res(
	struct se_cmd *cmd
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		__qnap_transport_release_drain_io_fio_res(cmd);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		__qnap_transport_release_drain_io_fio_res(cmd);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		__qnap_transport_release_drain_io_ib_fbdisk_res(cmd);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
	}

}

static bool __qnap_transport_check_io_drained(
	struct se_cmd *cmd
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		/* fio + block dev */
		return __qnap_transport_check_fio_io_drained(cmd);

	} else if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		/* fio + regular file */
		return __qnap_transport_check_fio_io_drained(cmd);

	} else if (!qlib_is_ib_fbdisk_dev(&dev->dev_dr)) {
		/* iblock + fbdisk dev */
		return __qnap_transport_check_ib_fbdisk_io_drained(cmd);

	} else if (!qlib_is_ib_blk_dev(&dev->dev_dr)) {
		/* iblock + block dev */
		return false;
	}

	return false;
}

static void __qnap_tmr_lunreset_drain_state(
        struct se_cmd *cmd_to_abort,
	struct se_portal_group *tpg        
        )
{
	struct qnap_se_cmd_dr *cmd_dr = &cmd_to_abort->cmd_dr;
	int mask = (CMD_T_ABORTED | CMD_T_TAS);
	
	if (!(cmd_to_abort->transport_state & CMD_T_ABORTED))
		return;
	
	if (qnap_transport_tmr_get_code(cmd_to_abort))
		return;

	qnap_transport_tmr_set_code(cmd_to_abort, QNAP_TMR_CODE_LUN_RESET);

	if ((cmd_to_abort->transport_state & mask) == mask)
		atomic_set(&cmd_to_abort->cmd_dr.tmr_diff_it_nexus, 1);
	else	
		atomic_set(&cmd_to_abort->cmd_dr.tmr_diff_it_nexus, 0);
	
	if (cmd_dr->io_ops.do_drain_io)
		cmd_dr->io_ops.do_drain_io(cmd_to_abort, QNAP_TMR_CODE_LUN_RESET);
}

static void __qnap_tmr_abort_task(
	struct se_cmd *cmd_to_abort,
	struct se_portal_group *tpg
	)
{
	struct qnap_se_cmd_dr *cmd_dr = &cmd_to_abort->cmd_dr;
	int mask = (CMD_T_ABORTED | CMD_T_TAS);

	if (!(cmd_to_abort->transport_state & CMD_T_ABORTED))
		return;

	if (qnap_transport_tmr_get_code(cmd_to_abort))
		return;

	qnap_transport_tmr_set_code(cmd_to_abort, QNAP_TMR_CODE_ABORT_TASK);

	if ((cmd_to_abort->transport_state & mask) == mask)
		atomic_set(&cmd_to_abort->cmd_dr.tmr_diff_it_nexus, 1);
	else	
		atomic_set(&cmd_to_abort->cmd_dr.tmr_diff_it_nexus, 0);

	if (cmd_dr->io_ops.do_drain_io)
		cmd_dr->io_ops.do_drain_io(cmd_to_abort, QNAP_TMR_CODE_ABORT_TASK);
}

static void __qnap_transport_init_io_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	__qnap_transport_init_drain_io_bio_obj(cmd_dr);
	__qnap_transport_init_drain_io_iov_obj(cmd_dr);

	/* i/o draning methods */
	cmd_dr->io_ops.pre_do_drain_io = __qnap_transport_pre_drain_io;
	cmd_dr->io_ops.after_do_drain_io = __qnap_transport_after_drain_io;
	cmd_dr->io_ops.do_drain_io = __qnap_transport_drain_io;
	cmd_dr->io_ops.release_drain_io_res = __qnap_transport_release_drain_io_res;
	cmd_dr->io_ops.check_io_drained = __qnap_transport_check_io_drained;

	/* fast-block-clone methdos */
	if (qlib_check_exported_fbc_func()) {
		cmd_dr->io_ops.fbc_xcopy = qnap_fbc_do_xcopy;
		cmd_dr->io_ops.fbc_odx = qnap_fbc_do_odx;
	}

	/* simple tmr patch */
	cmd_dr->io_ops.tmr_lunreset_drain_state = __qnap_tmr_lunreset_drain_state;
	cmd_dr->io_ops.tmr_abort_task = __qnap_tmr_abort_task;
}

static void __qnap_transport_init_spc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	if (qnap_setup_spc_ops)
		qnap_setup_spc_ops(cmd_dr);
}

static void __qnap_transport_init_sbc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	if (qnap_setup_sbc_ops)
		qnap_setup_sbc_ops(cmd_dr);
}

void qnap_transport_create_fb_bio_rec_kmem(
	struct se_device *dev,	
	int dev_index
	)
{
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;
	char tmp_name[128];

	if (qlib_is_ib_fbdisk_dev(dev_dr))
		return;

	/* only for iblock + fbdisk device */
	sprintf(tmp_name, "fb_bio_rec_cache-%d", dev_index);

	dev_dr->fb_bio_rec_kmem = kmem_cache_create(tmp_name,
			sizeof(struct bio_rec), 
			__alignof__(struct bio_rec), 
			0, NULL);

	if (!dev_dr->fb_bio_rec_kmem)
		pr_warn("fail to create fb_bio_rec_cache, dev idx: %d\n", dev_index);

}

void qnap_transport_destroy_fb_bio_rec_kmem(
	struct se_device *dev
	)
{
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;

	if (qlib_is_ib_fbdisk_dev(dev_dr))
		return;

	if (dev_dr->fb_bio_rec_kmem)
		kmem_cache_destroy(dev_dr->fb_bio_rec_kmem);
}

/* 
 * This function was referred from blkdev_issue_discard(), but something
 * is different about it will check the command was aborted or is releasing
 * from connection now
 */
static int __qnap_transport_blkdev_issue_discard(
	struct se_cmd *se_cmd,
	bool special_discard,
	struct block_device *bdev, 
	sector_t sector,
	sector_t nr_sects, 
	gfp_t gfp_mask, 
	unsigned long flags
	)
{
#define MIN_REQ_SIZE	((1 << 20) >> 9)
	
	int ret = 0;
	sector_t work_sects = 0;
	struct se_device *se_dev = se_cmd->se_dev;

	while (nr_sects) {
		if (qnap_transport_check_cmd_aborted(se_cmd, false)) {
			ret = 0;
			break;
		}

		/* split req to 1mb at least one by one */
		work_sects = min_t (sector_t,  nr_sects, MIN_REQ_SIZE);

		if (special_discard) {
			if (blkdev_issue_special_discard && 
					(!qlib_is_fio_blk_dev(&se_dev->dev_dr)))
			{
				ret = blkdev_issue_special_discard(bdev, sector, 
					work_sects, gfp_mask);
			} else {
				ret = blkdev_issue_zeroout(bdev, sector, work_sects,
					gfp_mask, (flags | BLKDEV_ZERO_NOUNMAP));
			}
		} else {
			ret = blkdev_issue_discard(bdev, sector, work_sects, 
				gfp_mask, flags);
		}

		if (ret != 0)
			break;

		sector += work_sects;
		nr_sects -= work_sects;
	}

	return ret;
}

static void __send_hal_threshold_event(
	int lun_index,
	int tp_threshold_percent,
	u64 avail_size_gb
	)
{
#if defined(QNAP_HAL)

	/* call function if it exists since we declare it as weak symbol */
	if (send_hal_netlink) {
		NETLINK_EVT hal_event;
		memset(&hal_event, 0, sizeof(NETLINK_EVT));
		hal_event.type = HAL_EVENT_ISCSI;

		hal_event.arg.action = HIT_LUN_THRESHOLD;
		hal_event.arg.param.iscsi_lun.lun_index = lun_index;
		hal_event.arg.param.iscsi_lun.tp_threshold = tp_threshold_percent;
		/* unit: GB */
		hal_event.arg.param.iscsi_lun.tp_avail = avail_size_gb;
		send_hal_netlink(&hal_event);
	}
#endif

}

static void __send_hal_under_threshold_event(
		int lun_index,
		int tp_threshold_percent,
		u64 avail_size_gb
		)
{
#if defined(QNAP_HAL)
	/* call function if it exists since we declare it as weak symbol */
	if (send_hal_netlink) {
		NETLINK_EVT hal_event;
		memset(&hal_event, 0, sizeof(NETLINK_EVT));
		hal_event.type = HAL_EVENT_ISCSI;

		hal_event.arg.action = UNDER_LUN_THRESHOLD;
		hal_event.arg.param.iscsi_lun.lun_index = lun_index;
		hal_event.arg.param.iscsi_lun.tp_threshold = tp_threshold_percent;
		/* unit: GB */
		hal_event.arg.param.iscsi_lun.tp_avail = avail_size_gb;
		send_hal_netlink(&hal_event);
	}
#endif

}
	

#if defined(CONFIG_QTS_CEPH)
struct tcm_rbd_dev *qnap_transport_get_tcm_rbd_dev(struct se_device *se_dev)
{
	return TCM_RBD_DEV(se_dev);
}
EXPORT_SYMBOL(qnap_transport_get_tcm_rbd_dev);
#endif

void qnap_transport_parse_naa_6h_vendor_specific(
	void *se_dev,
	unsigned char *buf
	)
{
	/* tricky code - spc_parse_naa_6h_vendor_specific() from LIO, 
	 * we need it ... need to take this
	 */
	spc_parse_naa_6h_vendor_specific((struct se_device *)se_dev, buf);
}

static int __qnap_transport_check_support_fbc(
	struct block_device *bd,
	int *pool_blksz
	)
{
	THIN_BLOCKCLONE_DESC tbc_desc;
	unsigned long ret_bs = 0;
	sector_t s_lba, d_lba;
	u32 data_blks;

	if (!bd)
		return -EINVAL;
 
	/* quick check for file-based lun (fbdisk) */
	if (!strncmp(bd->bd_disk->disk_name, "fbdisk", 6))
		return -ENOTSUPP;

	/* case (pool blksz 1024KB)
	 * -(a) created pool from 4.1 firmware for block based lun
	 *	or file based lun
	 * -(b) or, created pool from 4.1 firmware then firmware was
	 *	upgraded to 4.2 for block based lun or file based lun
	 * -(c) or, new created pool only from 4.2 firmware BUT
	 *	for file based lun 
	 *
	 * NOTE:
	 * actually, we don't care the pool blks for file based lun ...
	 */
	*pool_blksz = POOL_BLK_SIZE_1024_KB;
 
	if (!thin_support_block_cloning)
		return -EINVAL;

	/* unit is 512b */
	data_blks = ((POOL_BLK_SIZE_512_KB << 10) >> 9);
	s_lba = 0;
	d_lba = s_lba + data_blks;

	tbc_desc.src_dev = bd;
	tbc_desc.dest_dev = bd;
	tbc_desc.src_block_addr = s_lba;
	tbc_desc.dest_block_addr = d_lba;
	tbc_desc.transfer_blocks = data_blks;

	if (thin_support_block_cloning(&tbc_desc, &ret_bs) != 0){
		pr_warn("FD: LIO(File I/O) + Block Backend: %s not support "
			"fast block clone\n", bd->bd_disk->disk_name);
		return -ENOTSUPP;
	}

	pr_info("FD: LIO(File I/O) + Block Backend: %s support "
			"fast block clone\n", bd->bd_disk->disk_name);

	/* fast-block-clone only supports on pool blksz 512KB 
	 * this is new created pool only from 4.2 firmware for block based lun
	 */
	WARN_ON(ret_bs != POOL_BLK_SIZE_512_KB);
	*pool_blksz = ret_bs;	
	return 0;
}

void qnap_transport_setup_support_fbc(
	struct se_device *se_dev
	)
{
	struct fd_dev *fd_dev = NULL;
	struct inode *inode = NULL;

	se_dev->dev_dr.pool_blk_kb = 0;
	se_dev->dev_dr.fast_blk_clone = 0;
	se_dev->dev_dr.fbc_control = 0;

	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr)) {
		fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
		inode = file_inode(fd_dev->fd_file);		

		if(!__qnap_transport_check_support_fbc(inode->i_bdev, 
						&se_dev->dev_dr.pool_blk_kb))
		{
			se_dev->dev_dr.fast_blk_clone = 1;
			se_dev->dev_dr.fbc_control = 1;
		}	
	} else
		pr_warn("not support fast block clone on other dev type\n");

	return;
}

/* called in iblock_configure_device() */
int qnap_transport_config_blkio_dev(
	struct se_device *se_dev
	)
{
	struct iblock_dev *ibd_dev = NULL;
	struct request_queue *q = NULL;
	struct qnap_se_dev_attr_dr *attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	bool fbc_supported = false;
	int bs_order;

	ibd_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);

	q = bdev_get_queue(ibd_dev->ibd_bd);
	if (!q)
		return -EINVAL;

	/* check poduct type */
	if (!qlib_bd_is_fbdisk_dev(ibd_dev->ibd_bd))
		se_dev->dev_dr.dev_type = QNAP_DT_IBLK_FBDISK;
	else
		se_dev->dev_dr.dev_type = QNAP_DT_IBLK_BLK;

	/* overwrite setting for iblock + fbdisk only */
	if (se_dev->dev_dr.dev_type == QNAP_DT_IBLK_BLK) {
		pr_warn("%s: unsupported IBLOCK + BLOCK configuration\n", __func__);
		return -EINVAL;
	}

	/* SHALL be setup first */
	WARN_ON(!se_dev->dev_attrib.block_size);
	bs_order = ilog2(se_dev->dev_attrib.block_size);

	qnap_transport_setup_support_fbc(se_dev);

	if (qlib_thin_lun(&se_dev->dev_dr)) {
		if (blk_queue_discard(q)) {
			/* re-configure the discard parameter */
			se_dev->dev_attrib.max_unmap_lba_count = 
				((MAX_UNMAP_MB_SIZE << 20) >> bs_order);
		
			se_dev->dev_attrib.max_unmap_block_desc_count =
				QIMAX_UNMAP_DESC_COUNT;
		
			/* This value shall be multiplied by 4KB. We overwrite it
			 * here instead of in lower layer driver
			 */
			se_dev->dev_attrib.unmap_granularity =
				(PAGE_SIZE >> bs_order);
		
#warning "TODO: SHALL check this"
#if 0 //TODO need to check
			se_dev->dev_attrib.unmap_granularity_alignment = 0;
			se_dev->dev_attrib.unmap_zeroes_data = 0;
#endif
		}
	}

	/* remember the capacity during to configure device */
	attr_dr->lun_blocks = se_dev->transport->get_blocks(se_dev);
	return 0;
}
EXPORT_SYMBOL(qnap_transport_config_blkio_dev);

int qnap_transport_config_fio_dev(
	struct se_device *se_dev
	)
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct qnap_se_dev_attr_dr *attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	struct file *file = NULL;
	struct inode *inode = NULL;
	int bs_order, ret;
	bool fbc_supported = false;
	file = fd_dev->fd_file;
	inode = file_inode(file);

	if (S_ISBLK(inode->i_mode)) {
		se_dev->dev_dr.dev_type = QNAP_DT_FIO_BLK;
		ret = bioset_init(&se_dev->dev_dr.fio_blk_bio_set, 128, 0, 
					BIOSET_NEED_BVECS);
		WARN_ON(ret);
	} else if (S_ISREG(inode->i_mode))
		se_dev->dev_dr.dev_type = QNAP_DT_FIO_FILE;	
	else
		return -EINVAL;

	/* SHALL be setup first */
	WARN_ON(!se_dev->dev_attrib.block_size);
	bs_order = ilog2(se_dev->dev_attrib.block_size);

	qnap_transport_setup_support_fbc(se_dev);

	if (qlib_thin_lun(&se_dev->dev_dr)) {
		if (!qlib_is_fio_file_dev(&se_dev->dev_dr)
		|| ((!qlib_is_fio_blk_dev(&se_dev->dev_dr))
			&& blk_queue_discard(bdev_get_queue(inode->i_bdev)))
		)
		{
			se_dev->dev_attrib.max_unmap_lba_count = 
				((MAX_UNMAP_MB_SIZE << 20) >> bs_order);			
			se_dev->dev_attrib.max_unmap_block_desc_count =
				QIMAX_UNMAP_DESC_COUNT;

#warning "TODO: shall be setup value"
#if 0 //TODO need to check
			se_dev->dev_attrib.unmap_granularity = 0;
			se_dev->dev_attrib.unmap_granularity_alignment = 0;
			se_dev->dev_attrib.unmap_zeroes_data = 0;
#endif
		}
	}

	/* remember the capacity during to configure device */
	attr_dr->lun_blocks = se_dev->transport->get_blocks(se_dev);
	return 0;
}
EXPORT_SYMBOL(qnap_transport_config_fio_dev);

#ifdef QNAP_SHARE_JOURNAL
int qnap_transport_check_is_journal_support(struct se_device *dev)
{
	return dev->dev_bbu_journal;
}
EXPORT_SYMBOL(qnap_transport_check_is_journal_support);
#endif

int qnap_transport_blkdev_issue_discard(
	struct se_cmd *se_cmd,
	bool special_discard,
	struct block_device *bdev, 
	sector_t sector,
	sector_t nr_sects, 
	gfp_t gfp_mask, 
	unsigned long flags
	)
{
	struct se_device *se_dev = se_cmd->se_dev;

	/* for fio frontend type, the caller MUST perform sync cache first before
	 * to call this function
	 */
	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr)
	|| !qlib_is_ib_blk_dev(&se_dev->dev_dr)
	|| !qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)
	)
	{
		return __qnap_transport_blkdev_issue_discard(se_cmd, 
			special_discard, bdev, sector, nr_sects, gfp_mask, flags);
	}
	return -ENOTSUPP;

}
EXPORT_SYMBOL(qnap_transport_blkdev_issue_discard);

int qnap_transport_bd_is_fbdisk_dev(
	struct block_device *bd
	)
{
	if (!bd)
		return -ENODEV;

	/* we only handle the fbdisk dev currently */
	if (strncmp(bd->bd_disk->disk_name, "fbdisk", 6))
		return -ENODEV;
	return 0;
}
EXPORT_SYMBOL(qnap_transport_bd_is_fbdisk_dev);

/* 0: normal i/o (not hit sync i/o threshold)
 * 1: hit sync i/o threshold
 * -ENOSPC: pool space is full
 * -EINVAL: wrong parameter to call function
 * -ENODEV: no such device
 */
int qnap_transport_fd_check_dm_thin_cond(
	struct se_device *se_dev
	)
{
	/* caller shall check the device is thin or not before call this */
	struct fd_dev *fd_dev = NULL;;
	struct file *file = NULL;

	fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	file = fd_dev->fd_file;

	return qlib_fd_check_dm_thin_cond(file);
}
EXPORT_SYMBOL(qnap_transport_fd_check_dm_thin_cond);

void qnap_transport_fd_upadte_write_result(
	struct se_cmd *se_cmd,
	int *original_result
	)
{
	/* 1. go this way if original result is GOOD and this is thin lun 
	 * 2. execute this in fd_execute_rw()
	 */
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	loff_t start, len = 0, idx = 0;
	struct scatterlist *sg;
	int ret, ret1, ret2;
	bool is_fio_blk_dev;

	if (!qlib_is_fio_file_dev(&se_dev->dev_dr))
		is_fio_blk_dev = false;	
	else if (!qlib_is_fio_blk_dev(&se_dev->dev_dr))
		is_fio_blk_dev = true;
	else
		return;

	if (is_fio_blk_dev) {
		/* check whether goes to following cases or not
		 * 0. normal i/o (not hit sync i/o threshold)
		 * 1, hit sync i/o threshold
		 */
		ret = qnap_transport_fd_check_dm_thin_cond(se_dev);
		if (ret == 0 || ret == -EINVAL || ret == -ENODEV)
			return;
	}

	/* if hit condition, force to report no space. we don't care the
	 * io position was mapped or is new allocated
	 */
	if (qnap_transport_hit_read_deletable(&se_cmd->se_dev->dev_dr, 
							se_cmd->t_task_cdb)) {
		*original_result = -ENOSPC;
		return;
	}

	/* time to do sync i/o
	 * 1. hit the sync i/o threshold area
	 * 2. or, space is full BUT need to handle lba where was mapped or not
	 */

	/* To sync cache again if write ok and the sync cache behavior shall 
	 * work for thin lun only 
	 */
	start = (se_cmd->t_task_lba << bs_order);
	for_each_sg(se_cmd->t_data_sg, sg, se_cmd->t_data_nents, idx)
		len += sg->length;

	ret1 = qlib_fd_sync_cache_range(fd_dev->fd_file, start, (start + len));
	if (ret1 == 0)
		return;

	/* fail from sync cache -
	 * thin i/o may go somewhere (lba wasn't mapped to any block)
	 * or something wrong during normal sync-cache
	 */

	if (is_fio_blk_dev) {
		/* call again to make sure it is no space really or not */
		ret2 = qnap_transport_fd_check_dm_thin_cond(se_dev);
		if (ret2 == -ENOSPC) {
			pr_warn_ratelimited("%s: space was full already\n",__func__);
			ret1 = ret2;
		}
	}

	*original_result = ret1;
	return;
}
EXPORT_SYMBOL(qnap_transport_fd_upadte_write_result);

sense_reason_t qnap_transport_check_capacity_changed(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct qnap_se_dev_attr_dr *attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	unsigned long long blocks = se_dev->transport->get_blocks(se_dev);
	unsigned long long old;

	/* dev_attr_dr.lun_blocks will be set during 
	 * 1. target_configure_device() or
	 * 2. when current capacity is not same as previous one during to
	 * handle any i/o
	 */
	if (attr_dr->lun_blocks != blocks) {
		/* update to new one */
		old = attr_dr->lun_blocks;
		attr_dr->lun_blocks = blocks;

		/* shall plus 1 cause of it is 0 based */
		pr_warn("capacity size (new:0x%llx) is not same as "
			"previous one (old:0x%llx). send "
			"CAPACITY_DATA_HAS_CHANGED sense code\n",
			(unsigned long long)(blocks + 1), 
			(unsigned long long)(old + 1));
		return TCM_CAPACITY_DATA_HAS_CHANGED;
	}
	return 0;
}

void qnap_transport_show_set_task_aborted_msg(
	struct se_cmd *cmd,
	bool to_set
	)
{
	struct se_device *dev = cmd->se_dev;

	if (!strcmp(cmd->se_tfo->fabric_name, "iSCSI")) {
		pr_debug("[%s] %s Setting SAM_STAT_TASK_ABORTED status "
			"for cdb[0]: 0x%02x, tag: 0x%08llx, cmdsn: 0x%08llx "
			"on dev: %s (%s)\n", cmd->se_tfo->fabric_name, 
			((to_set) ? "" : "NOT"), cmd->t_task_cdb[0], cmd->tag, 
			cmd->se_tfo->get_cmdsn(cmd), 
			qlib_get_dev_type_str(&dev->dev_dr),
			((qlib_thin_lun(&dev->dev_dr)) ? "thin" : "thick"));
	}
}

void qnap_transport_show_task_aborted_msg(
	struct se_cmd *cmd,
	int type,
	bool done_wait,
	bool print_msg
	)
{
	struct se_device *dev = cmd->se_dev;
	char tmp_str[256];

	if (!print_msg)
		return;

	snprintf(tmp_str, 256, "task aborted by %s", 
			__qnap_transport_get_drop_type_str(type));

	qnap_transport_show_cmd(tmp_str, cmd);
}

void qnap_transport_tmr_set_code(
	struct se_cmd *cmd,
	u8 code
	)
{
	atomic_set(&cmd->cmd_dr.tmr_code, code);
}

int qnap_transport_tmr_get_code(
	struct se_cmd *cmd
	)
{
	return atomic_read(&cmd->cmd_dr.tmr_code);
}

bool qnap_transport_check_cmd_aborted_by_release_conn(
	struct se_cmd *cmd,
	bool print_msg
	)
{
	struct se_device *dev = cmd->se_dev;

	if (test_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &cmd->cmd_dr.cmd_t_state)) {
		qnap_transport_show_task_aborted_msg(cmd, -1, false, print_msg);
		return true;
	}
	return false;
}
EXPORT_SYMBOL(qnap_transport_check_cmd_aborted_by_release_conn);

bool qnap_transport_check_cmd_in_tmr_abort_wq(struct se_cmd *cmd) 
{
	if (test_bit(QNAP_CMD_T_IN_TMR_ABORT_WQ, &cmd->cmd_dr.cmd_t_state))
		return true;
	return false;
}
EXPORT_SYMBOL(qnap_transport_check_cmd_in_tmr_abort_wq);

bool qnap_transport_check_cmd_aborted_by_tmr(
	struct se_cmd *cmd,
	bool print_msg
	)
{
	struct se_device *dev = cmd->se_dev;

	if ((cmd->transport_state & CMD_T_ABORTED)
	|| qnap_transport_tmr_get_code(cmd) 
	|| test_bit(QNAP_CMD_T_IN_TMR_ABORT_WQ, &cmd->cmd_dr.cmd_t_state)
	)
	{
			qnap_transport_show_task_aborted_msg(cmd, 
				qnap_transport_tmr_get_code(cmd), false, print_msg);
		return true;
	}
	return false;
}
EXPORT_SYMBOL(qnap_transport_check_cmd_aborted_by_tmr);

bool qnap_transport_check_cmd_aborted(
	struct se_cmd *cmd,
	bool print_msg
	)
{
	if (qnap_transport_check_cmd_aborted_by_release_conn(cmd, print_msg))
		return true;

	if (qnap_transport_check_cmd_aborted_by_tmr(cmd, print_msg))
		return true;
	return false;
}
EXPORT_SYMBOL(qnap_transport_check_cmd_aborted);

static char * __qnap_transport_get_drop_type_str(
	int type
	)
{
	char *str;

	/* refer from enum tcm_tmreq_table in target_core_base.h file 
	 * and it matches for rfc3720 spec
	 */
	switch(type) {
	case -1:
		str = "RELEASE CONN";
		break;
	case TMR_ABORT_TASK:
		str = "ABORT TASK";
		break;
	case TMR_ABORT_TASK_SET:
		str = "ABORT TASK SET";
		break;
	case TMR_CLEAR_ACA:
		str = "CLAER ACA";
		break;
	case TMR_CLEAR_TASK_SET:
		str = "CLAER TASK SET";
		break;
	case TMR_LUN_RESET:
		str = "LUN RESET";
		break;
	case TMR_TARGET_WARM_RESET:
		str = "TARGET WARM RESET";
		break;
	case TMR_TARGET_COLD_RESET:
		str = "TARGET COLD RESET";
		break;
	default:
		str = "(?)";
		break;
	}

	return str;
}

# warning "TODO: shall check this function"
int qnap_change_dev_size(
	struct se_device *se_dev
	)
{
	struct fd_dev *fd_dev = NULL;
	struct iblock_dev *ib_dev = NULL;
	struct block_device *bd = NULL;
	struct inode *inode = NULL;
	unsigned long long total_blks;
	int ret = -ENODEV, bs_order = ilog2(se_dev->dev_attrib.block_size);

	if (qlib_is_fio_blk_dev(&se_dev->dev_dr) == 0) {
		fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
		inode = fd_dev->fd_file->f_mapping->host;

		if (S_ISBLK(inode->i_mode))
			total_blks = (i_size_read(inode) >> bs_order);
		else
			total_blks = (fd_dev->fd_dev_size >> bs_order);

		pr_debug("FILEIO: Using size: %llu blks and block size: %d\n", 
			total_blks, (1 << bs_order));

		ret = 0;

	} else if (qlib_is_ib_fbdisk_dev(&se_dev->dev_dr) == 0) {
		ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		bd = ib_dev->ibd_bd;
		total_blks = (i_size_read(bd->bd_inode) >> bs_order);

		pr_debug("iBlock: Using size: %llu blks and block size: %d\n", 
			total_blks, (1 << bs_order));

		ret = 0;
	}

	return ret;
}
EXPORT_SYMBOL(qnap_change_dev_size);

int qnap_transport_check_dev_hit_thin_threshold(
	struct se_device *se_dev
	)
{
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	sector_t a_blks, u_blks;
	uint64_t t_min,  dividend;
	int ret, reached = 0;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;

	if (!qlib_thin_lun(&se_dev->dev_dr))
		return -ENODEV;

	/* we ONLY handle the write-direction command */
	ret = qnap_spc_get_ac_and_uc(se_dev, &a_blks, &u_blks);
	if (ret != 0)
		return ret;
	
	dividend = ((a_blks + u_blks) << bs_order);
	dividend = (dividend * dev_attr_dr->tp_threshold_percent);
	t_min = div_u64(dividend, 100);
	
	if ((u_blks << bs_order) > t_min) {
		if (dev_attr_dr->tp_threshold_hit == 0){
			dev_attr_dr->tp_threshold_hit++;
			reached = 1;
		}
	} else
		dev_attr_dr->tp_threshold_hit = 0;
	
	if (reached) {
		__send_hal_threshold_event(dev_attr_dr->lun_index, 
					dev_attr_dr->tp_threshold_percent, 
					((a_blks << bs_order) >> 30));
		return 0;
	}

	return -EPERM;

}
EXPORT_SYMBOL(qnap_transport_check_dev_hit_thin_threshold);

void qnap_transport_check_dev_under_thin_threshold(
	struct se_device *se_dev
	)
{
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	sector_t a_blks, u_blks;
	uint64_t t_min,  dividend;
	int ret, under_threshold = 0;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;

	if (!qlib_thin_lun(&se_dev->dev_dr)) {
		return -ENODEV;
	}
	/* we ONLY handle the write-direction command */
	ret = qnap_spc_get_ac_and_uc(se_dev, &a_blks, &u_blks);
	if (ret != 0) {
		return ret;
	}

	dividend = ((a_blks + u_blks) << bs_order);
	dividend = (dividend * dev_attr_dr->tp_threshold_percent);
	t_min = div_u64(dividend, 100);

	if ((u_blks << bs_order) <= t_min) {
		if (dev_attr_dr->tp_threshold_hit != 0) {
			dev_attr_dr->tp_threshold_hit = 0;
			check_lun_threshold_for_each_device(&under_threshold);
			/* under_threshold == 0, there are some LUN is still over threshold.
			   under_threshold == 1, all the LUN is under threshold. */
		}
	}
	
	if (under_threshold) {
		__send_hal_under_threshold_event(dev_attr_dr->lun_index, 
					dev_attr_dr->tp_threshold_percent, 
					((a_blks << bs_order) >> 30));
		return;
	}

	return;

}
EXPORT_SYMBOL(qnap_transport_check_dev_under_thin_threshold);


int qnap_transport_check_cmd_hit_thin_threshold(
	struct se_cmd *se_cmd
	)
{
	return qnap_transport_check_dev_hit_thin_threshold(se_cmd->se_dev);
}
EXPORT_SYMBOL(qnap_transport_check_cmd_hit_thin_threshold);

#ifdef SUPPORT_TP
int qnap_transport_get_thin_allocated(
	struct se_device *se_dev
	)
{
	struct iblock_dev *ibd = NULL;
	int ret;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	loff_t total_bytes = 0, used_blks = 0;

	if (!(se_dev->dev_flags & DF_CONFIGURED))
		return -ENODEV;

	/* - currently, we support blk io + fbdisk block dev (file based lun) 
	 * - QNAP_DT_IBLK_FBDISK will be set after bd (struct block_device)
	 *   was set in ib_dev->ibd_bd
	 */
	ret = qlib_is_ib_fbdisk_dev(&se_dev->dev_dr);
	if (ret != 0)
		return ret;

	ibd = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
	if (!ibd->ibd_bd)
		return -ENODEV;

	ret = qlib_ib_fbdisk_get_thin_total_used_blks(ibd->ibd_bd, &total_bytes, 
		&used_blks);

	if (ret != 0)
		return ret;

	/* the unit of used_blks is 512 bytes */
	dev_attr_dr->allocated = ((u64)used_blks << 9);
	return 0;

}
#endif

#ifdef ISCSI_D4_INITIATOR
struct se_node_acl *qnap_tpg_get_initiator_node_acl(
	struct se_portal_group *tpg,
	unsigned char *initiatorname
	)
{
	struct se_node_acl *acl = NULL;

	// 2009/09/23 Nike Chen add for default initiator
	mutex_lock(&tpg->acl_node_mutex);

	list_for_each_entry(acl, &tpg->acl_node_list, acl_list) {
		if (!tpg->default_acl && !(strcmp(acl->initiatorname, 
			DEFAULT_INITIATOR))
			)
		{
			tpg->default_acl = acl;
			pr_debug("Get default acl %p for tpg %p.\n", 
				tpg->default_acl, tpg);
		}

		if (!tpg->default_acl && !(strcmp(acl->initiatorname,
			FC_DEFAULT_INITIATOR))
			)
		{
			tpg->default_acl = acl;
			pr_debug("Get FC default acl %p for tpg %p.\n",
				tpg->default_acl, tpg);
		}

		if (!strcasecmp(acl->initiatorname, initiatorname) &&
			!acl->dynamic_node_acl) 
		{
			mutex_unlock(&tpg->acl_node_mutex);
			return acl;
		}
	}

	mutex_unlock(&tpg->acl_node_mutex);
	return NULL;
}

void qnap_tpg_copy_node_devs(
	struct se_node_acl *dest,
	struct se_node_acl *src,
	struct se_portal_group *tpg
	)
{
	struct se_dev_entry *deve;
	struct se_lun *lun = NULL;

	rcu_read_lock();

	hlist_for_each_entry_rcu(deve, &src->lun_entry_hlist, link) {

		lun = rcu_dereference(deve->se_lun);
		if (!lun)
			continue;
	
		pr_debug("TARGET_CORE[%s]->TPG[%u]_LUN[%u] - Copying %s"
			" access for LUN\n", 
			tpg->se_tpg_tfo->get_fabric_name(),
			tpg->se_tpg_tfo->tpg_get_tag(tpg), 
			lun->unpacked_lun,
			deve->lun_access_ro ?
			"READ-ONLY" : "READ-WRITE");
		
		rcu_read_unlock();

		/* unlock here since core_enable_device_list_for_node() will
		 * allocate memory and do lock ...
		 */
		core_enable_device_list_for_node(lun, NULL,
			lun->unpacked_lun, deve->lun_access_ro, dest, tpg);

		rcu_read_lock();
	}

	rcu_read_unlock();

	return;
}

/* 2019/06/12 Jonathan Ho: Only add the lun instead of all luns to dynamic acls. */
void qnap_tpg_add_node_to_devs_when_add_lun(
	struct se_portal_group *tpg,
	struct se_lun *lun
	)
{
	struct se_node_acl *acl = NULL;

	mutex_lock(&tpg->acl_node_mutex);
	list_for_each_entry(acl, &tpg->acl_node_list, acl_list) {
		// Benjamin 20120719: Note that acl->dynamic_node_acl should be
		// set in core_tpg_check_initiator_node_acl(), and no
		// NAF_DYNAMIC_NODE_ACL anymore.
		if (acl->dynamic_node_acl) {
			mutex_unlock(&tpg->acl_node_mutex);
			core_tpg_add_node_to_devs(acl, tpg, lun);
			mutex_lock(&tpg->acl_node_mutex);
		}
	}
	mutex_unlock(&tpg->acl_node_mutex);
	return;
}
#endif

void qnap_transport_init_se_cmd_dr(
	struct se_cmd *cmd
	)
{
	__qnap_transport_init_io_ops(&cmd->cmd_dr);
	__qnap_transport_init_sbc_ops(&cmd->cmd_dr);
	__qnap_transport_init_spc_ops(&cmd->cmd_dr);
}

void qnap_init_se_dev_dr(
	struct qnap_se_dev_dr *dr
	)
{
	memset(dr, 0, sizeof(struct qnap_se_dev_dr));

	dr->pool_blk_kb = 0;

#ifdef SUPPORT_FAST_BLOCK_CLONE
	dr->fast_blk_clone = 0;
	dr->fbc_control = 0;
	spin_lock_init(&dr->fbc_control_lock);
#endif
	atomic_set(&dr->hit_read_deletable, 0);

#ifdef QNAP_TARGET_PERF_FEATURE
	if (qnap_target_perf_enabled())
		return;
#endif
}

void qnap_init_se_dev_attr_dr(
	struct qnap_se_dev_attr_dr *dr
	)
{
	memset(dr, 0, sizeof(struct qnap_se_dev_attr_dr));
}

int qnap_transport_thread_cpu_setting(
    struct se_device *dev)
{
    cpumask_var_t cpumask;
    int cpu = 0;

    if (!(dev->dev_dr.se_dev_thread_cpumask == 0) &&
            !(dev->dev_dr.se_dev_thread_cpumask == 0xffff)) {

        if (!zalloc_cpumask_var(&cpumask, GFP_KERNEL)) {
            pr_err("Unable to allocate conn->conn_cpumask\n");
        } else {
            for_each_online_cpu(cpu) {
                if ((1 <<cpu) & dev->dev_dr.se_dev_thread_cpumask) {
                    cpumask_set_cpu(cpu, cpumask);
                    pr_debug("%s: set cpu %d\n",__func__,cpu);
                }
            }
            set_cpus_allowed_ptr(current,cpumask);

			free_cpumask_var(cpumask);
            pr_debug("%s: set cpumask %u\n",__func__,
                     dev->dev_dr.se_dev_thread_cpumask);
        }
    }

    return 0;

}

static void __qnap_target_exec_random_work(
	struct work_struct *work
	)
{
	int ret;
	struct qnap_se_cmd_dr *dr = container_of(work, struct qnap_se_cmd_dr, random_work);
	struct se_cmd *cmd = container_of(dr, struct se_cmd, cmd_dr);

	/* here was referred from __target_execute_cmd() and this work will be 
	 * handled after any condition checking was done
	 */

	if (!cmd->execute_cmd) {
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto err;
	}

	ret = cmd->execute_cmd(cmd);
	if (!ret)
		return;
err:
	spin_lock_irq(&cmd->t_state_lock);
	cmd->transport_state &= ~CMD_T_SENT;
	spin_unlock_irq(&cmd->t_state_lock);

	transport_generic_request_failure(cmd, ret);
}

int qnap_target_exec_random_task(
	struct se_cmd *se_cmd
	)
{
	u32 len, bs_order;
	bool go_wq = false, is_gen_read = false;
	struct qnap_se_dev_dr *dr = &se_cmd->se_dev->dev_dr;
	
	if (!dr->random_wq)
		return -EPERM;

	if(strcmp(se_cmd->se_dev->transport->name, "fileio"))
		return -EPERM;

	if ((se_cmd->t_task_cdb[0] == READ_6) || (se_cmd->t_task_cdb[0] == READ_10)
	||  (se_cmd->t_task_cdb[0] == READ_12) || (se_cmd->t_task_cdb[0] == READ_16)
	)
		is_gen_read = true;

	if (is_gen_read == true) {

		bs_order = ilog2(se_cmd->se_dev->dev_attrib.block_size);
		len = (dr->prev_len >> bs_order);

		if(se_cmd->t_task_lba == (dr->prev_lba + len))
			go_wq = false;
		else
			go_wq = true;

		dr->prev_lba = se_cmd->t_task_lba;
		dr->prev_len = se_cmd->data_length;
		if (go_wq == true) {
			INIT_WORK(&se_cmd->cmd_dr.random_work,
				__qnap_target_exec_random_work);
			queue_work(dr->random_wq, &se_cmd->cmd_dr.random_work);
			return 0;
		}
	}
	return -EPERM;
}

int qnap_set_dev_write_cache(struct se_device *se_dev, bool set_wc)
{
	struct iblock_dev *ib_dev = NULL;
	struct request_queue *q = NULL;
	int ret = 0;

	if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)
		|| !qlib_is_ib_blk_dev(&se_dev->dev_dr))
	{
		ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		q = bdev_get_queue(ib_dev->ibd_bd);

		spin_lock_irq(&q->queue_lock);
		blk_queue_write_cache(q, set_wc, false);
		spin_unlock_irq(&q->queue_lock);

	} else if (!qlib_is_fio_blk_dev(&se_dev->dev_dr) ||
			!qlib_is_fio_file_dev(&se_dev->dev_dr)) {
		/* for fio type device .. not find way to set "write cache" now */
	} else
		ret = -ENODEV;

	if (!ret)
		se_dev->dev_attrib.emulate_write_cache = (int)set_wc;

	return ret;

}
EXPORT_SYMBOL(qnap_set_dev_write_cache);

enum {
	aptpl_Opt_initiator_fabric, aptpl_Opt_initiator_node, 
	aptpl_Opt_initiator_sid, aptpl_Opt_sa_res_key, aptpl_Opt_res_holder, 
	aptpl_Opt_res_type, aptpl_Opt_res_scope, aptpl_Opt_res_all_tg_pt, 
	aptpl_Opt_mapped_lun, aptpl_Opt_target_fabric, aptpl_Opt_target_node, 
	aptpl_Opt_tpgt, aptpl_Opt_port_rtpi, aptpl_Opt_target_lun, 
	aptpl_Opt_pr_data_start, aptpl_Opt_pr_data_end, aptpl_Opt_err
};

static match_table_t tokens = {
	{aptpl_Opt_initiator_fabric, "initiator_fabric=%s"},
	{aptpl_Opt_initiator_node, "initiator_node=%s"},
	{aptpl_Opt_initiator_sid, "initiator_sid=%s"},
	{aptpl_Opt_sa_res_key, "sa_res_key=%s"},
	{aptpl_Opt_res_holder, "res_holder=%d"},
	{aptpl_Opt_res_type, "res_type=%d"},
	{aptpl_Opt_res_scope, "res_scope=%d"},
	{aptpl_Opt_res_all_tg_pt, "res_all_tg_pt=%d"},
	{aptpl_Opt_mapped_lun, "mapped_lun=%d"},
	{aptpl_Opt_target_fabric, "target_fabric=%s"},
	{aptpl_Opt_target_node, "target_node=%s"},
	{aptpl_Opt_tpgt, "tpgt=%d"},
	{aptpl_Opt_port_rtpi, "port_rtpi=%d"},
	{aptpl_Opt_target_lun, "target_lun=%d"},
	{aptpl_Opt_pr_data_start, "PR_REG_START: %d"},
	{aptpl_Opt_pr_data_end, "PR_REG_END: %d"},
	{aptpl_Opt_err, NULL}
};

int __qnap_scsi3_parse_aptpl_data(
	struct se_device *se_dev,
	char *data,
	struct node_info *s,
	struct node_info *d
	)
{
	unsigned char *i_fabric = NULL, *i_port = NULL, *isid = NULL;
	unsigned char *t_fabric = NULL, *t_port = NULL;
	char *ptr;
	substring_t args[MAX_OPT_ARGS];
	unsigned long long tmp_ll;
	u64 sa_res_key = 0;
	u32 mapped_lun = 0, target_lun = 0;
	int ret = -1, res_holder = 0, all_tg_pt = 0, arg, token;
	int token_start = 0, token_end = 0, found_match = 0;
	u16 port_rpti = 0, tpgt = 0;
	u8 type = 0, scope = 0;

	while ((ptr = strsep(&data, ",\n")) != NULL) {
		if (!*ptr)
			continue;
	
		token = match_token(ptr, tokens, args);
		switch (token) {
		case aptpl_Opt_pr_data_end:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			token_end = 1;
			break;
		case aptpl_Opt_pr_data_start:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			token_start = 1;
			break;
		case aptpl_Opt_initiator_fabric:
			i_fabric = match_strdup(&args[0]);
			if (!i_fabric) {
				ret = -ENOMEM;
				goto out;
			}
			break;
		case aptpl_Opt_initiator_node:
			i_port = match_strdup(&args[0]);
			if (!i_port) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(i_port) >= PR_APTPL_MAX_IPORT_LEN) {
				pr_err("APTPL metadata initiator_node="
					" exceeds PR_APTPL_MAX_IPORT_LEN: %d\n",
					PR_APTPL_MAX_IPORT_LEN);
				ret = -EINVAL;
				break;
			}
			break;
		case aptpl_Opt_initiator_sid:
			isid = match_strdup(&args[0]);
			if (!isid) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(isid) >= PR_REG_ISID_LEN) {
				pr_err("APTPL metadata initiator_isid"
					"= exceeds PR_REG_ISID_LEN: %d\n",
					PR_REG_ISID_LEN);
				ret = -EINVAL;
				break;
			}
			break;
		case aptpl_Opt_sa_res_key:
			ret = kstrtoull(args->from, 0, &tmp_ll);
			if (ret < 0) {
				pr_err("kstrtoull() failed for sa_res_key=\n");
				goto out;
			}
			sa_res_key = (u64)tmp_ll;
			break;
		/*
		 * PR APTPL Metadata for Reservation
		 */
		case aptpl_Opt_res_holder:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			res_holder = arg;
			break;
		case aptpl_Opt_res_type:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			type = (u8)arg;
			break;
		case aptpl_Opt_res_scope:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			scope = (u8)arg;
			break;
		case aptpl_Opt_res_all_tg_pt:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			all_tg_pt = (int)arg;
			break;
		case aptpl_Opt_mapped_lun:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			mapped_lun = (u32)arg;
			break;
		/*
		 * PR APTPL Metadata for Target Port
		 */
		case aptpl_Opt_target_fabric:
			t_fabric = match_strdup(&args[0]);
			if (!t_fabric) {
				ret = -ENOMEM;
				goto out;
			}
			break;
		case aptpl_Opt_target_node:
			t_port = match_strdup(&args[0]);
			if (!t_port) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(t_port) >= PR_APTPL_MAX_TPORT_LEN) {
				pr_err("APTPL metadata target_node="
					" exceeds PR_APTPL_MAX_TPORT_LEN: %d\n",
					PR_APTPL_MAX_TPORT_LEN);
				ret = -EINVAL;
				break;
			}
			break;
		case aptpl_Opt_tpgt:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			tpgt = (u16)arg;
			break;
		case aptpl_Opt_port_rtpi:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			port_rpti = (u16)arg;
			break;
		case aptpl_Opt_target_lun:
			ret = match_int(args, &arg);
			if (ret)
				goto out;
			target_lun = (u32)arg;
			break;
		default:
			break;
		}

		if (!token_start) {
			pr_info("%s: not found any information in APTPL "
				"metafile\n", __func__);
			ret = -ENOENT;
			goto out;
		}
	
		if (token_end && token_start) {
			if (!i_port || !t_port || !sa_res_key) {
				pr_err("Illegal parameters for APTPL registration\n");
				ret = -EINVAL;
				goto out;
			}
				
			if (res_holder && !(type)) {
				pr_err("Illegal PR type: 0x%02x for reservation"
						" holder\n", type);
				ret = -EINVAL;
				goto out;
			}
	
			if (!strcmp(s->i_port, i_port) &&
			(!strncmp(s->i_sid, isid, PR_REG_ISID_LEN)) &&
			(!strcmp(s->t_port, t_port)) &&
			(s->tpgt == tpgt) && (s->mapped_lun == mapped_lun) &&
			(s->target_lun == target_lun)
			)
			{
				pr_debug("\n==== source info ====\n");
				pr_debug("i_port: %s, i_sid: %s\n", 
					s->i_port, s->i_sid);
				pr_debug("t_port: %s, tpgt: %d, "
					"mapped_lun: %d, target_lun: %d\n", 
					s->t_port, s->tpgt, s->mapped_lun, 
					s->target_lun);
				pr_debug("\n");

				pr_debug("\n==== found match info ====\n");
				pr_debug("initiator: %s, initiator node: %s, "
					"initiator sid: %s, t_port: %s, "
					"tpgt: %d, target_lun: %d\n", 
					i_fabric, i_port, isid, t_port,
					tpgt, target_lun);
				pr_debug("res key: %llu, res holder: %d\n", 
					sa_res_key, res_holder);
				pr_debug("res type: %d, scope: %d, "
					"all_tg_pt: %d, port_rpti: %d\n", 
					type, scope, all_tg_pt, port_rpti);
				pr_debug("\n");

				memcpy(d->i_port, i_port, PR_APTPL_MAX_IPORT_LEN);
				memcpy(d->t_port, t_port, PR_APTPL_MAX_IPORT_LEN);
				memcpy(d->i_sid, isid, PR_REG_ISID_LEN);
				d->tpgt = tpgt;
				d->sa_res_key = sa_res_key;
				d->mapped_lun = mapped_lun;
				d->target_lun = target_lun;
				d->res_holder = res_holder;
				d->all_tg_pt = all_tg_pt;
				d->port_rpti = port_rpti;
				d->type = type;
				d->scope = scope;

				found_match = 1;
			}

			if (found_match)
				goto out;

			token_start = 0;
			token_end = 0;
		}

		if (i_fabric)
			kfree(i_fabric);

		if (i_port)
			kfree(i_port);

		if (isid)
			kfree(isid);

		if (t_fabric)
			kfree(t_fabric);

		if (t_port)
			kfree(t_port);
	
		i_fabric = NULL;
		i_port = NULL;
		isid = NULL;
		t_fabric = NULL;
		t_port = NULL;
	
	}
	
out:
	if (i_fabric)
		kfree(i_fabric);
	if (t_fabric)
		kfree(t_fabric);
	if (i_port)
		kfree(i_port);
	if (isid)
		kfree(isid);
	if (t_port)
		kfree(t_port);

	if (found_match)
		return 0;

	return -ENOENT;

}

int __qnap_scsi3_check_aptpl_metadata_file_exists(
	struct se_device *dev,
	struct file **fp
	)
{
	struct t10_wwn *wwn = &dev->t10_wwn;
	struct file *file;
	int flags = O_RDONLY;
	char path[512];

	/* check aptpl meta file path */
	if (strlen(&wwn->unit_serial[0]) >= 512) {
		pr_err("%s: WWN value for struct se_device does not fit"
			" into path buffer\n", __func__);
		return -EMSGSIZE;
	}

	memset(path, 0, 512);
	snprintf(path, 512, "/var/target/pr/aptpl_%s", &wwn->unit_serial[0]);

	file = filp_open(path, flags, 0600);
	if (IS_ERR(file)) {
		pr_debug("%s: filp_open(%s) for APTPL metadata"
			" failed\n", __func__, path);
		return PTR_ERR(file);
	}

	*fp = file;
	return 0;
}

int qnap_transport_check_aptpl_registration(
	struct se_session *se_sess,
	struct se_node_acl *nacl,
	struct se_portal_group *tpg
	)
{
	struct se_lun *lun;
	struct se_dev_entry *deve;

	mutex_lock(&nacl->lun_entry_mutex);

	hlist_for_each_entry_rcu(deve, &nacl->lun_entry_hlist, link) {

		lun = rcu_dereference_check(deve->se_lun, 
			lockdep_is_held(&nacl->lun_entry_mutex));

		if (!lun)
			continue;
	
		mutex_unlock(&nacl->lun_entry_mutex);

		qnap_transport_scsi3_check_aptpl_registration(
			lun->lun_se_dev, tpg, lun, 
			se_sess, nacl, lun->unpacked_lun
			);

		mutex_lock(&nacl->lun_entry_mutex);
	}
	mutex_unlock(&nacl->lun_entry_mutex);

	return 0;
}

#ifdef ISCSI_MULTI_INIT_ACL
void *qnap_target_add_qnap_se_nacl(
	char *initiator_name,
	struct qnap_se_nacl_dr *dr
	)
{
	struct qnap_se_node_acl	*acl = NULL;

	acl = kzalloc(sizeof(struct qnap_se_node_acl), GFP_KERNEL);
	if (!acl) {
		pr_warn("%s: fail alloc mem for qnap_se_node_acl\n", __func__);
		return NULL;
	}

	INIT_LIST_HEAD(&acl->acl_node);
	memcpy(acl->initiatorname, initiator_name, strlen(initiator_name));

	spin_lock(&dr->acl_node_lock);
	list_add_tail(&acl->acl_node, &dr->acl_node_list);
	spin_unlock(&dr->acl_node_lock);

	pr_debug("%s: add qnap se nacl:0x%p, initiator name:%s\n", __func__,
		acl, acl->initiatorname);

	return (void *)acl;
}

void qnap_target_init_qnap_se_nacl(
	struct qnap_se_nacl_dr *dr
	)
{
	INIT_LIST_HEAD(&dr->acl_node_list);
	spin_lock_init(&dr->acl_node_lock);
}

void qnap_target_free_qnap_se_nacl(
	void *map,
	struct qnap_se_nacl_dr *dr
	)
{
	struct qnap_se_node_acl *nacl, *nacl_tmp;
	bool found = false;
	int count = 0;

	spin_lock(&dr->acl_node_lock);
	list_for_each_entry_safe(nacl, nacl_tmp, &dr->acl_node_list, acl_node) {
		if (map != nacl)
			continue;

		pr_debug("%s: found map:0x%p, del qnap se nacl:0x%p\n", 
			__func__, map, nacl);
		count++;
		found = true;
		list_del_init(&nacl->acl_node);
		kfree(nacl);
	}

	spin_unlock(&dr->acl_node_lock);

	if (found == false)
		pr_warn("%s: not found qnap nacl, need to check\n", __func__);

	if (count > 1)
		pr_warn("%s: count > 1, need to check\n", __func__);

	return;

}

void qnap_target_free_all_qnap_se_nacls(
	struct qnap_se_nacl_dr *dr
	)
{
	struct qnap_se_node_acl *nacl, *nacl_tmp;
	LIST_HEAD(node_list);

	spin_lock(&dr->acl_node_lock);
	list_splice_init(&dr->acl_node_list, &node_list);
	spin_unlock(&dr->acl_node_lock);

	list_for_each_entry_safe(nacl, nacl_tmp, &node_list, acl_node) {
		pr_debug("%s: del qnap se nacl:0x%p\n", __func__, nacl);
		list_del_init(&nacl->acl_node);
		kfree(nacl);
	}
	return;
}
#endif

int qnap_transport_spc_cmd_size_check(
	struct se_cmd *cmd
	)
{
	int ret = 1;
	struct se_device *se_dev = cmd->se_dev;
	u8 *cdb = cmd->t_task_cdb;
	u32 size;

	/* Some host testing tools will set the ExpectedDataTransferLength
	 * in iscsi header to non-zero but the parameter list length
	 * (or allocation length) is zero in scsi cdb. According to the
	 * SPC, either parameter list length or allocation length is
	 * zero, they are NOT error condition (indicates no data shall
	 * be transferred).
	 * Therefore, here will do filter this condition again
	 */
	switch(cdb[0]){
	case MAINTENANCE_IN:
	case MAINTENANCE_OUT:
		if (se_dev->transport->get_device_type(se_dev) != TYPE_ROM) {
			if (((cdb[1] & 0x1f) == MI_REPORT_TARGET_PGS)
			|| ((cdb[1] & 0x1f) == MO_SET_TARGET_PGS)
			) 
			{
				size = get_unaligned_be32(&cdb[6]);
				if (0 == size)
					ret = 0;
			}

		}
		break;
	case SERVICE_ACTION_IN_16:
		switch (cmd->t_task_cdb[1] & 0x1f) {
		case SAI_READ_CAPACITY_16:
			size = get_unaligned_be32(&cdb[10]);
			if (!size)
				ret = 0;
			break;
		default:
			break;
		}

		break;
	default:
		break;
	}


	if (ret == 0) {
		pr_warn("%s: allocation len or parameter list len is 0 "
			"for cdb:0x%x. it isn't error condition "
			"(indicates no data shall be transferred)\n", 
			__func__, cdb[0]);
	}

	return ret;
}

sense_reason_t qnap_transport_check_report_lun_changed(
	struct se_cmd *se_cmd
	)
{
	struct se_dev_entry *deve;
	struct se_session *sess = se_cmd->se_sess;
	struct se_node_acl *nacl;
	u32 lun_count = 0;
	
	/* how can I do ??? */
	if (!sess)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	
	if (!sess->sess_dr.sess_got_report_lun_cmd)
		return 0;

	nacl = sess->se_node_acl;
		
	rcu_read_lock();

	hlist_for_each_entry_rcu(deve, &nacl->lun_entry_hlist, link)
		lun_count++;

	rcu_read_unlock();

	if (atomic_read(&sess->sess_dr.sess_lun_count) != lun_count) {
		pr_warn("lun counts was changed. send REPORTED LUNS DATA "
				"HAS CHANGED sense code\n");
		/* reset it ... */
		sess->sess_dr.sess_got_report_lun_cmd = false;
		return TCM_REPORTED_LUNS_DATA_HAS_CHANGED;
	}
	return 0;
}

bool qnap_check_v_sup(struct se_device *dev)
{
	struct se_dev_attrib *dev_attrib = &dev->dev_attrib;
	return (dev_attrib->dev_attr_dr.emulate_v_sup > 0);
}

sense_reason_t qnap_transport_convert_rc_to_tcm_sense_reason(
	u32 _rc
	)
{
	sense_reason_t ret;
	RC rc = (RC)_rc;

	switch (rc) {
	case RC_GOOD:
		ret = TCM_NO_SENSE;
		break;		
	case RC_UNKNOWN_SAM_OPCODE:
		ret = TCM_UNSUPPORTED_SCSI_OPCODE;
		break;		
	case RC_REQ_TOO_MANY_SECTORS:
		ret = TCM_SECTOR_COUNT_TOO_MANY;
		break;		
	case RC_INVALID_CDB_FIELD:
		ret = TCM_INVALID_CDB_FIELD;
		break;		
	case RC_INVALID_PARAMETER_LIST:
		ret = TCM_INVALID_PARAMETER_LIST;
		break;		
	case RC_UNKNOWN_MODE_PAGE:
		ret = TCM_UNKNOWN_MODE_PAGE;
		break;		
	case RC_WRITE_PROTECTEDS:
		ret = TCM_WRITE_PROTECTED;
		break;		
	case RC_RESERVATION_CONFLICT:
		ret = TCM_RESERVATION_CONFLICT;
		break;		
	case RC_CHECK_CONDITION_NOT_READY:
		ret = TCM_CHECK_CONDITION_NOT_READY;
		break;		
	case RC_CHECK_CONDITION_ABORTED_CMD:
		ret = TCM_CHECK_CONDITION_ABORT_CMD;
		break;		
	case RC_CHECK_CONDITION_UA:
		ret = TCM_CHECK_CONDITION_UNIT_ATTENTION;
		break;		
	case RC_LBA_OUT_OF_RANGE:
		ret = TCM_ADDRESS_OUT_OF_RANGE;
		break;		
	case RC_MISCOMPARE_DURING_VERIFY_OP:
		ret = TCM_MISCOMPARE_VERIFY;
		break;		
	case RC_PARAMETER_LIST_LEN_ERROR:
		ret = TCM_PARAMETER_LIST_LENGTH_ERROR;
		break;		
	case RC_UNREACHABLE_COPY_TARGET:
		ret = TCM_UNREACHABLE_COPY_TARGET;
		break;		
	case RC_3RD_PARTY_DEVICE_FAILURE:
		ret = TCM_3RD_PARTY_DEVICE_FAILURE;
		break;		
	case RC_INCORRECT_COPY_TARGET_DEV_TYPE:
		ret = TCM_INCORRECT_COPY_TARGET_DEV_TYPE;
		break;		
	case RC_TOO_MANY_TARGET_DESCRIPTORS:
		ret = TCM_TOO_MANY_TARGET_DESCS;
		break;		
	case RC_TOO_MANY_SEGMENT_DESCRIPTORS:
		ret = TCM_TOO_MANY_SEGMENT_DESCS;
		break;		
	case RC_ILLEGAL_REQ_DATA_OVERRUN_COPY_TARGET:
		ret = TCM_ILLEGAL_REQ_DATA_OVERRUN_COPY_TARGET;
		break;		
	case RC_ILLEGAL_REQ_DATA_UNDERRUN_COPY_TARGET:
		ret = TCM_ILLEGAL_REQ_DATA_UNDERRUN_COPY_TARGET;
		break;		
	case RC_COPY_ABORT_DATA_OVERRUN_COPY_TARGET:
		ret = TCM_COPY_ABORT_DATA_OVERRUN_COPY_TARGET;
		break;		
	case RC_COPY_ABORT_DATA_UNDERRUN_COPY_TARGET:
		ret = TCM_COPY_ABORT_DATA_UNDERRUN_COPY_TARGET;
		break;		
	case RC_INSUFFICIENT_RESOURCES:
		ret = TCM_INSUFFICIENT_RESOURCES;
		break;		
	case RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD:
		ret = TCM_INSUFFICIENT_RESOURCES_TO_CREATE_ROD;
		break;		
	case RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD_TOKEN:
		ret = TCM_INSUFFICIENT_RESOURCES_TO_CREATE_ROD_TOKEN;
		break;		
	case RC_OPERATION_IN_PROGRESS:
		ret = TCM_OPERATION_IN_PROGRESS;
		break;		
	case RC_INVALID_TOKEN_OP_AND_INVALID_TOKEN_LEN:
		ret =  TCM_INVALID_TOKEN_OP_AND_INVALID_TOKEN_LEN;
		break;		
	case RC_INVALID_TOKEN_OP_AND_CAUSE_NOT_REPORTABLE:
		ret = TCM_INVALID_TOKEN_OP_AND_CAUSE_NOT_REPORTABLE;
		break;		
	case RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_CREATION_NOT_SUPPORTED:
		ret = TCM_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_CREATION_NOT_SUPPORTED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_USAGE_NOT_SUPPORTED:
		ret = TCM_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_USAGE_NOT_SUPPORTED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_CANCELLED:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_CANCELLED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_CORRUPT:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_CORRUPT;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_DELETED:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_DELETED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_EXPIRED:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_EXPIRED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_REVOKED:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_REVOKED;
		break;		
	case RC_INVALID_TOKEN_OP_AND_TOKEN_UNKNOWN:
		ret = TCM_INVALID_TOKEN_OP_AND_TOKEN_UNKNOWN;
		break;		
	case RC_INVALID_TOKEN_OP_AND_UNSUPPORTED_TOKEN_TYPE:
		ret = TCM_INVALID_TOKEN_OP_AND_UNSUPPORTED_TOKEN_TYPE;
		break;		
	case RC_NO_SPACE_WRITE_PROTECT:
		ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
		break;		
	case RC_OUT_OF_RESOURCES:
		ret = TCM_OUT_OF_RESOURCES;
		break;		
	case RC_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED:
		ret = TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
		break;		
	case RC_CAPACITY_DATA_HAS_CHANGED:
		ret = TCM_CAPACITY_DATA_HAS_CHANGED;
		break;		
	case RC_NON_EXISTENT_LUN:
		ret = TCM_NON_EXISTENT_LUN;
		break;		
	case RC_REPORTED_LUNS_DATA_HAS_CHANGED:
		ret = TCM_REPORTED_LUNS_DATA_HAS_CHANGED;
		break;		
	case RC_LOGICAL_UNIT_COMMUNICATION_FAILURE:
	default:
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		break;
	}

	return ret;

}

static int __qnap_transport_create_feinfo(
	struct se_device *se_dev,
	struct __fe_info *fe_info
	)
{
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct inode *inode = NULL;
	struct fd_dev *__fd_dev;
	struct iblock_dev *__ib_dev;

	if(!se_dev->transport->get_dev)
		return -ENODEV;

	if (!qlib_is_fio_blk_dev(dr) || !qlib_is_fio_file_dev(dr)) {
		__fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
		fe_info->__fd.fd_file = __fd_dev->fd_file;

		if (!qlib_is_fio_blk_dev(dr)) {
			inode = file_inode(__fd_dev->fd_file);
			fe_info->__bd.bd = inode->i_bdev;
			fe_info->__bd.bio_set = &dr->fio_blk_bio_set;
		}

	} else if (!qlib_is_ib_fbdisk_dev(dr) || !qlib_is_ib_blk_dev(dr)) {
		__ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		fe_info->__bd.bd = __ib_dev->ibd_bd;
		fe_info->__bd.bio_set = &__ib_dev->ibd_bio_set;
	} else
		return -ENODEV;

	fe_info->is_thin = qlib_thin_lun(dr);
	fe_info->fe_type = dr->dev_type;
	return 0;

}

static void __qnap_transport_create_dev_attr(
	struct se_cmd *cmd,
	struct __dev_attr *dev_attr,
	bool is_thin
	)
{
	struct se_device *dev = cmd->se_dev;

	if (dev->dev_attrib.emulate_fua_write)
		dev_attr->supported_fua_write = 1;

	if (dev->dev_attrib.emulate_write_cache)
		dev_attr->supported_write_cache = 1;

	if (is_thin) {
		if (dev->dev_attrib.emulate_tpu)
			dev_attr->supported_unmap = 1;

		if (dev->dev_attrib.emulate_tpws)
			dev_attr->supported_write_same = 1;

		/* support read zero after unmap, it needs by WHLK */
		if (dev_attr->supported_unmap || dev_attr->supported_write_same)
			dev_attr->supported_unmap_read_zero = 1;
	}
#if 0
	if (dev->dev_dr.fast_blk_clone)
		dev_attr->supported_dm_fbc = 1;

	if (dev->dev_dr.fbc_control)
		dev_attr->supported_dev_fbc = 1;
#endif
}

int qnap_transport_create_devinfo(
	struct se_cmd *cmd, 
	struct __dev_info *dev_info
	)
{
	struct se_device *se_dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct se_portal_group *se_tpg;
	int ret;

	if (!cmd->se_lun)
		return -ENODEV;

	ret = __qnap_transport_create_feinfo(se_dev, &dev_info->fe_info);
	if (ret)
		return ret;

	__qnap_transport_create_dev_attr(cmd, &dev_info->dev_attr, qlib_thin_lun(dr));

	dev_info->bs_order = ilog2(se_dev->dev_attrib.block_size);
	dev_info->dev_max_lba = se_dev->transport->get_blocks(se_dev);
	dev_info->sbc_dev_type = se_dev->transport->get_device_type(se_dev);

	/* Refer the target_emulate_evpd_83() to crete initiatior port
	 * identifier field value 
	 */
	se_tpg = cmd->se_lun->lun_tpg;
	dev_info->initiator_rtpi = cmd->se_lun->lun_rtpi;
	dev_info->initiator_prot_id = se_tpg->proto_id;

	qlib_get_naa_6h_code((void *)se_dev, &dr->dev_naa[0], &dev_info->naa[0],
			qnap_transport_parse_naa_6h_vendor_specific);

	return 0;
}
EXPORT_SYMBOL(qnap_transport_create_devinfo);

void *qnap_transport_get_fbdisk_file(
	void *fb_dev, 
	sector_t lba, 
	u32 *index
	)
{
	u32 i = 0;
	struct fbdisk_device *fbd = (struct fbdisk_device *)fb_dev; 
	struct fbdisk_file *fb_file = NULL;

	if ((fbd == NULL) ||(index == NULL))
		return NULL;

	for (i = 0; i < (fbd->fb_file_num); i++){
		fb_file = &fbd->fb_backing_files_ary[i];
		if ((lba >= fb_file->fb_start_byte) && (lba <= fb_file->fb_end_byte))
			break;
	}

	if (fb_file)
		*index = i;

	return (void *)fb_file;
}

void qnap_transport_show_cmd(
	char *prefix_str,
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;

	if ((se_cmd->se_cmd_flags & SCF_SE_LUN_CMD)
		&& !(se_cmd->se_cmd_flags & SCF_SCSI_TMR_CDB)
	)
	{
		/* here won't use xxx_ratelimited() due to we need some hints
		 * from cmd status (especially, in __transport_wait_for_tasks()
		 */
		if (!strcmp(se_cmd->se_tfo->fabric_name, "iSCSI")) {
			pr_warn("[%s]: %s, ITT: 0x%08x, i_state: %d, t_state: %d, "
				"transport_state: 0x%x, filebasd: %s, thin: %s, "
				"cdb[0]:0x%x, cdb[1]:0x%x, ref:%d, bs_state:0x%llx(0x%llx), ptime:%ld(s)\n",
				se_cmd->se_tfo->fabric_name, prefix_str, 
				se_cmd->se_tfo->get_task_tag(se_cmd),
				se_cmd->se_tfo->get_cmd_state(se_cmd), 
				se_cmd->t_state, se_cmd->transport_state, 
				((!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)) ? "yes" : \
					((!qlib_is_fio_blk_dev(&se_dev->dev_dr)) ? \
					"no" : "unknown")),
				(qlib_thin_lun(&se_dev->dev_dr) ? "yes" : "no"),
				se_cmd->t_task_cdb[0], se_cmd->t_task_cdb[1],
				kref_read(&se_cmd->cmd_kref),
				se_cmd->b_se_state, se_cmd->b_se_state_last,
				((jiffies - se_cmd->start_jiffies) / HZ));
		}
	}

}
EXPORT_SYMBOL(qnap_transport_show_cmd);

bool qnap_transport_hit_read_deletable(
	struct qnap_se_dev_dr *dev_dr,
	u8 *cdb
	)
{
	bool check = false;

	switch (cdb[0]) {
#if 0
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
#endif
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case WRITE_VERIFY:
	case XDWRITEREAD_10:
	case COMPARE_AND_WRITE:
	case UNMAP:
	case WRITE_SAME_16:
	case WRITE_SAME:
	case EXTENDED_COPY:
		check = true;
		break;
	case VARIABLE_LENGTH_CMD:
	{
		u16 service_action = get_unaligned_be16(&cdb[8]);
		switch (service_action) {
		case XDWRITEREAD_32:
		case WRITE_SAME_32:
			check = true;
		default:
			break;
		}
		break;
	}
	default:
		break;
	}

	if (check && (atomic_read(&dev_dr->hit_read_deletable) == 1))
		return true;

	return false;
}

int qnap_transport_vfs_fsync_range(
	struct se_device *dev, 
	loff_t s, 
	loff_t e, 
	int data_sync
	)
{
	struct fd_dev *fd_dev = NULL;
	int ret;

	if (!qlib_is_ib_blk_dev(&dev->dev_dr) || !qlib_is_ib_blk_dev(&dev->dev_dr))
		return -EINVAL;

	if (!qlib_is_fio_file_dev(&dev->dev_dr)) {
		fd_dev = (struct fd_dev *)dev->transport->get_dev(dev);
		return vfs_fsync_range(fd_dev->fd_file, s, e, data_sync);
	}

	if (!qlib_is_fio_blk_dev(&dev->dev_dr)) {
		fd_dev = (struct fd_dev *)dev->transport->get_dev(dev);		
		ret = vfs_fsync_range(fd_dev->fd_file, s, e, data_sync);
		if (!ret)
			return 0;
			
		/* if fail to sync, try check this is thin or thick lun */
		if (!qlib_thin_lun(&dev->dev_dr))
			return ret;
		
		/* if this is thin lun, now to ... */
		ret = qlib_fd_check_dm_thin_cond(fd_dev->fd_file);
		if (ret != -ENOSPC)
			return ret;

		pr_warn("%s: space was full already\n", __func__);
		return -ENOSPC;
	}

	return -EINVAL;

}
EXPORT_SYMBOL(qnap_transport_vfs_fsync_range);
