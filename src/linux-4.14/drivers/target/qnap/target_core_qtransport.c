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

#include "../target_core_internal.h"
#include "../target_core_iblock.h"
#include "../target_core_file.h"
#include "../target_core_pr.h"

#include <qnap/iscsi_priv.h>
#include <qnap/fbdisk.h>
#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qsbc.h"

#ifdef SUPPORT_FAST_BLOCK_CLONE
#include "target_core_qfbc.h"
#endif
#ifdef CONFIG_QTS_CEPH
#include "target_core_rbd.h"
#endif
#ifdef SUPPORT_TPC_CMD
#include "target_core_qodx.h"
#endif

#ifdef QNAP_TARGET_PERF_FEATURE
#include "target_core_perf_en.h"
#endif

#ifdef CONFIG_QTS_CEPH
static inline struct tcm_rbd_dev *TCM_RBD_DEV(struct se_device *dev)
{
	return container_of(dev, struct tcm_rbd_dev, dev);
}
#endif

#if !defined(CONFIG_ARCH_ALPINE)
static void __qnap_do_mybio_end_io(
	struct bio *bio,
	int err
	)
{
	CB_DATA *p = NULL;
	IO_REC *rec = NULL;

	rec = (IO_REC *)bio->bi_private;
	p = rec->cb_data;

	if (!err)
		err = blk_status_to_errno(bio->bi_status);

	rec->transfer_done = 1;
	if(err != 0){
		if (err == -ENOSPC)
			p->nospc_err = 1;

		rec->transfer_done = -1; // treat it as error
		atomic_inc(&p->bio_err_count);
		smp_mb__after_atomic();
	}

	bio_put(bio);

	if (atomic_dec_and_test(&p->bio_count))
		complete(p->wait);

	return;
}
#endif

static inline void __qnap_do_init_cb_data(
	CB_DATA *data,
	void *p
	)
{
	data->wait = p;
	data->nospc_err= 0;
	atomic_set(&data->bio_count, 1);
	atomic_set(&data->bio_err_count, 0);
	return;
}


static inline void __qnap_do_free_io_rec_by_io_rec_list(
	struct list_head *io_rec_list
	)
{
	IO_REC *rec = NULL, *tmp_rec = NULL;

	list_for_each_entry_safe(rec, tmp_rec, io_rec_list, node)
		kfree(rec);
	return;
}


static void __qnap_do_pop_put_bio(
	struct bio_list *biolist
	)
{
	struct bio *bio = NULL;

	if (!biolist)
		return;

	while (1){
		bio = bio_list_pop(biolist);
		if (!bio)
			break;
		bio_put(bio);
	}
	return;
}

static sector_t __qnap_do_get_done_blks_by_io_rec_list(
	struct list_head *io_rec_list
	)
{
	IO_REC *rec;
	sector_t done = 0;
	
	list_for_each_entry(rec, io_rec_list, node){
		/* Only computed the transferred-done part. This shall
		 * match the __bio_end_io() function
		 */
		if (rec->transfer_done != 1)
			break;
		done += (sector_t)rec->nr_blks;
	}
	return done;
}


static int  __qnap_do_submit_bio_wait(
	struct bio_list *bio_lists,
	u8 cmd,
	unsigned long timeout
	)
{
#define D4_T_S  10

	DECLARE_COMPLETION_ONSTACK(wait);
	IO_REC *rec = NULL;
	CB_DATA cb_data;
	unsigned long t;
	struct bio *mybio = NULL;
	struct blk_plug plug;

	if (bio_lists == NULL)
		BUG_ON(1);

	if (timeout)
		t = timeout;
	else
		t = msecs_to_jiffies(D4_T_S * 1000);

	__qnap_do_init_cb_data(&cb_data, &wait);

	blk_start_plug(&plug);
	while (1) {
		mybio = bio_list_pop(bio_lists);
		if (!mybio)
			break;

		rec = (IO_REC *)mybio->bi_private;
		rec->cb_data = &cb_data;
		atomic_inc(&(cb_data.bio_count));
		mybio->bi_opf |= cmd;
		submit_bio(mybio);
	}

	blk_finish_plug(&plug);

	if (!atomic_dec_and_test(&(cb_data.bio_count))) {
		while (wait_for_completion_timeout(&wait, t) == 0)
			pr_err("wait bio to be done\n");
	}

	if (atomic_read(&cb_data.bio_err_count)) {
		if (cb_data.nospc_err)
			return -ENOSPC;
		else
			return -EIO;
	}
	return 0;
}


static struct bio *__qnap_do_get_one_mybio(
	GEN_RW_TASK *task,
	sector_t block_lba
	)
{
	struct iblock_dev *ib_dev = NULL; 
	struct bio *mybio = NULL;
	struct se_device *se_dev = NULL;

	if (!task)
		return NULL;

	se_dev = (struct se_device *)task->se_dev;
	ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);

	/* To limit to allocate one bio for this function */
	mybio = bio_alloc_bioset(GFP_NOIO, 1, ib_dev->ibd_bio_set);
	if (!mybio) {
		pr_err("%s: unable to allocate mybio\n", __func__);
		return NULL;
	}

	mybio->bi_disk = ib_dev->ibd_bd->bd_disk;
#if defined(CONFIG_ARCH_ALPINE) || defined(CONFIG_ARCH_MVEBU)
	mybio->bi_end_io = NULL;
#else
	mybio->bi_end_io = &__qnap_do_mybio_end_io;
#endif
	mybio->bi_iter.bi_sector = block_lba;

	pr_debug("%s - allocated bio: 0x%p, lba:0x%llx\n", __func__, 
		mybio, (unsigned long long)mybio->bi_iter.bi_sector);

	return mybio;
}

static int __qnap_transport_do_block_rw(
	GEN_RW_TASK *task,
	u8 cmd
	)
{
	struct iblock_dev *ib_dev = NULL;
	sector_t block_lba = 0, t_lba = 0;
	u32 i = 0, bs_order = 0, err = 0, done = 0;
	u64 expected_bcs = 0, len = 0;
	int code = -EINVAL, bio_cnt = 0;
	struct bio *mybio = NULL;	
	struct scatterlist *sg = NULL;
	struct bio_list bio_lists;
	struct list_head io_rec_list;
	IO_REC *rec = NULL;
	struct se_device *se_dev = NULL;
	
	/* TODO: this code shall be smart ... */

	se_dev = (struct se_device *)task->se_dev;
	ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
	block_lba = t_lba = task->lba;
	bs_order = task->dev_bs_order;
	
	expected_bcs = ((sector_t)task->nr_blks << bs_order);
	if (!expected_bcs){
		task->ret_code = code;
		return 0;
	}

	code = 0;
	INIT_LIST_HEAD(&io_rec_list);
	bio_list_init(&bio_lists);
	
	/**/
	for_each_sg(task->sg_list, sg, task->sg_nents, i){
		if (!expected_bcs)
			break;
	
		/* Here will map one sg to one bio */
		rec = kzalloc(sizeof(IO_REC), GFP_KERNEL);
		if (!rec){
			if (!bio_list_size(&bio_lists)){
				pr_err("unable to allocate memory for io rec\n");
				err = 1;
				code = -ENOMEM;
				break;
			}
			goto _DO_SUBMIT_;
		}
		INIT_LIST_HEAD(&rec->node);

		/* task lba may be 4096b, shall be converted again for linux
		 * block layer (512b)
		 */	
		block_lba = ((block_lba << bs_order) >> 9);

		mybio = __qnap_do_get_one_mybio(task, block_lba);
		if (!mybio) {
			kfree(rec);
			if (!bio_list_size(&bio_lists)){
				code = -ENOMEM;
				err = 1;
				break;
			}
			goto _DO_SUBMIT_;
		}
	
		/* Set something for bio */
		len = min_t(u64, expected_bcs, sg->length);
		mybio->bi_io_vec[0].bv_page = sg_page(sg);
		mybio->bi_io_vec[0].bv_len = len;
		mybio->bi_io_vec[0].bv_offset = sg->offset;
		mybio->bi_status = BLK_STS_OK;
		mybio->bi_vcnt = 1;
		mybio->bi_iter.bi_size = len;

		mybio->bi_private = (void *)rec;
		rec->cb_data = NULL;
		rec->nr_blks = (len >> bs_order);
		rec->ib_dev = ib_dev;
		list_add_tail(&rec->node, &io_rec_list);	

		pr_debug("[%s] cmd:0x%x, sg->page:0x%p, sg->length:0x%x\n",
			__func__, cmd, sg_page(sg), sg->length);
	
		pr_debug("[%s] mybio:0x%p, task lba:0x%llx, "
			"bio block_lba:0x%llx, expected_bcs:0x%llx, "
			"real len:0x%llx \n", __func__, mybio,
			(unsigned long long)t_lba, (unsigned long long)block_lba, 
			(unsigned long long)expected_bcs, (unsigned long long)len);
	
		bio_list_add(&bio_lists, mybio);
		bio_cnt++;
	
		t_lba += (sector_t)(len >> bs_order);
		block_lba = t_lba;
		expected_bcs -= len;
	
		if ((bio_cnt < BLOCK_MAX_BIO_PER_TASK) && expected_bcs)
			continue;
	
_DO_SUBMIT_:
		err = __qnap_do_submit_bio_wait(&bio_lists, cmd, 0);
	
		/* after to submit, we will do ... */
		done += (u32)__qnap_do_get_done_blks_by_io_rec_list(&io_rec_list);
                __qnap_do_pop_put_bio(&bio_lists);
		__qnap_do_free_io_rec_by_io_rec_list(&io_rec_list);	
		pr_debug("[%s] done blks:0x%x\n", __func__, done);

		if (err){
			code = err;
			break;
		}

		/* To check if timeout happens after to submit */
		if (IS_TIMEOUT(task->timeout_jiffies)){
			task->is_timeout = 1;
			break;
		}
	
		INIT_LIST_HEAD(&io_rec_list);
		bio_list_init(&bio_lists);
		bio_cnt = 0;
	}
	
	task->ret_code = code;
	
	if (err || task->is_timeout){
		if (task->is_timeout)
			pr_err("[%s] jiffies > cmd expected-timeout value !!\n", 
				__func__);

		return -1;
	}
	return done;
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

static int __qnap_transport_get_pool_sz_kb(
	struct se_device *se_dev
	)
{
#ifdef SUPPORT_FAST_BLOCK_CLONE
	if (se_dev->dev_dr.fast_blk_clone) {
		/* case
		 * -(a) new created pool only by 4.2 fw for block based lun
		 */
		return POOL_BLK_SIZE_512_KB;
	}
#endif
	/* case
	 * -(a) created pool by 4.1 fw for block based lun
	 *	or file based lun
	 * -(b) or, created pool by 4.1 fw then fw was upgraded to 4.2
	 *	for block based lun or file based lun
	 * -(c) or, new created pool only by 4.2 fw BUT
	 *	for file based lun
	 *
	 * actually, we don't care the pool blks for file based lun ...
	 */
	return POOL_BLK_SIZE_1024_KB;
}

/* called in iblock_configure_device() */
int qnap_transport_config_blkio_dev(
	struct se_device *se_dev
	)
{
	struct iblock_dev *ibd_dev = NULL;
	struct request_queue *q = NULL;
	struct qnap_se_dev_attr_dr *attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	int bs_order;

	ibd_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);

	q = bdev_get_queue(ibd_dev->ibd_bd);
	if (!q)
		return -EINVAL;

	/* check poduct type */
	if (!qnap_transport_bd_is_fbdisk_dev(ibd_dev->ibd_bd))
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

#ifdef SUPPORT_FAST_BLOCK_CLONE
	qnap_transport_setup_support_fbc(se_dev);
#endif

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
#if 0
		se_dev->dev_attrib.unmap_granularity_alignment = 0;
		se_dev->dev_attrib.unmap_zeroes_data = 0;
#endif

	}

	se_dev->dev_dr.pool_blk_kb = __qnap_transport_get_pool_sz_kb(se_dev);

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
	struct request_queue *q = NULL;
	int bs_order;

	file = fd_dev->fd_file;
	inode = file->f_mapping->host;

	if (S_ISBLK(inode->i_mode)) {
		se_dev->dev_dr.dev_type = QNAP_DT_FIO_BLK;
	} else
		se_dev->dev_dr.dev_type = QNAP_DT_FIO_FILE;

	/* overwrite setting for fio + blkdev only */
	if (se_dev->dev_dr.dev_type == QNAP_DT_FIO_FILE) {
		pr_warn("%s: unsupported FIO + FILE configuration\n", __func__);
		return -EINVAL;
	}

	/* SHALL be setup first */
	WARN_ON(!se_dev->dev_attrib.block_size);

	bs_order = ilog2(se_dev->dev_attrib.block_size);

#ifdef SUPPORT_FAST_BLOCK_CLONE
	qnap_transport_setup_support_fbc(se_dev);
#endif

	q = bdev_get_queue(inode->i_bdev);
	if (blk_queue_discard(q)) {

		se_dev->dev_attrib.max_unmap_lba_count = 
			((MAX_UNMAP_MB_SIZE << 20) >> bs_order);

		se_dev->dev_attrib.max_unmap_block_desc_count =
			QIMAX_UNMAP_DESC_COUNT;

#warning "TODO: shall be setup value"
#if 0
		se_dev->dev_attrib.unmap_granularity = 0;
		se_dev->dev_attrib.unmap_granularity_alignment = 0;
		se_dev->dev_attrib.unmap_zeroes_data = 0;
#endif
	}

	se_dev->dev_dr.pool_blk_kb = __qnap_transport_get_pool_sz_kb(se_dev);

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

int qnap_transport_do_b_rw(
	void *rw_task
	)
{
	GEN_RW_TASK *task = NULL;

	if (!rw_task)
		return 0;

	task = (GEN_RW_TASK *)rw_task;

	if ((!task->se_dev) || (task->dir == DMA_BIDIRECTIONAL)
	|| (task->dir == DMA_NONE)
	) 
	{
		task->ret_code = -EINVAL;
		return 0;
	}

	return __qnap_transport_do_block_rw(task, 
			((task->dir == DMA_FROM_DEVICE) ? 0 : REQ_OP_WRITE));

}
EXPORT_SYMBOL(qnap_transport_do_b_rw);

void qnap_transport_make_rw_task(
	void *rw_task,
	struct se_device *se_dev,
	sector_t lba,
	u32 nr_blks,
	unsigned long timeout,
	enum dma_data_direction dir
	)
{
	GEN_RW_TASK *task = (GEN_RW_TASK *)rw_task;

	task->se_dev = se_dev;
	task->lba = lba;
	task->nr_blks = nr_blks;
	task->dev_bs_order = ilog2(se_dev->dev_attrib.block_size);
	task->dir = dir;
	task->timeout_jiffies = timeout;
	task->is_timeout = 0;
	task->ret_code = 0;
	return;
}
EXPORT_SYMBOL(qnap_transport_make_rw_task);

/* 
 * This function was referred from blkdev_issue_discard(), but something
 * is different about it will check the command was aborted or is releasing
 * from connection now
 */
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
#define MIN_REQ_SIZE	((1 << 20) >> 9)
	
	bool fio_blk_dev = false, iblock_fbdisk_dev = false;
	bool dropped_by_conn = false;
	char tmp_str[256];
	int __ret = 0, ret = 0, tmr_code;
	sector_t work_sects = 0;
	struct se_device *se_dev = se_cmd->se_dev;

	/* check backend type */
	__ret = qlib_is_fio_blk_dev(&se_dev->dev_dr);
	if (__ret != 0) {
		__ret = qlib_is_ib_fbdisk_dev(&se_dev->dev_dr);
		if (__ret == 0)
			iblock_fbdisk_dev = true;
	} else
		fio_blk_dev = true;

	while (nr_sects) {
		dropped_by_conn = test_bit(QNAP_CMD_T_RELEASE_FROM_CONN, 
					&se_cmd->cmd_dr.cmd_t_state);

		tmr_code = atomic_read(&se_cmd->cmd_dr.tmr_code);

		/* treat the result to be pass for two conditions even if
		 * original result is bad
		 */
		if (tmr_code || dropped_by_conn) {

			memset(tmp_str, 0, sizeof(tmp_str));
			sprintf(tmp_str, "[iSCSI - %s]", 
				((fio_blk_dev == true) ? "block-based" : \
				((iblock_fbdisk_dev == true) ? "file-based": \
				"unknown")));
			
			if (tmr_code) {
				pr_info("%s done to abort %s discard io by "
					"TMR opcode: %d\n", tmp_str, 
					((special_discard) ? "special" : ""),
					tmr_code);
			}
	
			if (dropped_by_conn) {
				pr_info("%s done to drop %s scsi op:0x%x\n", 
					tmp_str, 
					((special_discard) ? "special" : ""),
					se_cmd->t_task_cdb[0]);
			}
			ret  = 0;
			break;
		}

		/* split req to 1mb at least one by one */
		work_sects = min_t (sector_t,  nr_sects, MIN_REQ_SIZE);

		if (special_discard) {
			ret = blkdev_issue_special_discard(bdev, sector, 
				work_sects, gfp_mask);
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

static int __qnap_transport_get_a_blks_and_u_blks_on_thin(
	struct se_device *se_dev,
	int bs_order,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)
{
	struct qnap_se_dev_dr *dev_dr = &se_dev->dev_dr;
	struct qnap_se_dev_attr_dr *attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	struct iblock_dev *ibd = NULL;
	u64 total_blks, used_blks;
	loff_t __total_bytes, __used_blks;
	sector_t total, used;
	int ret = -ENODEV;

	*ret_avail_blks = 0;
	*ret_used_blks = 0;

	if (!qlib_is_fio_blk_dev(dev_dr)) {

		if (!strlen(se_dev->udev_path)) {
			pr_err("%s: udev_path is empty\n", __func__);
			return ret;
		}

		/* the unit of total_blks and used_blks is 512 bytes */
		ret = qlib_fio_blkdev_get_thin_total_used_blks(attr_dr, 
			se_dev->udev_path, &total_blks, &used_blks);

		if (ret != 0)
			return ret;

		total = (((sector_t)total_blks << 9) >> bs_order);
		used = (((sector_t)used_blks << 9) >> bs_order);
		*ret_used_blks = used;
		*ret_avail_blks = total - used;
		ret = 0;

	}
	else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)) {

		ibd = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);

		ret = qnap_transport_bd_is_fbdisk_dev(ibd->ibd_bd);
		if (ret != 0)
			return ret;

		/* the unit of __used_blks is 512 bytes */
		ret = qlib_ib_fbdisk_get_thin_total_used_blks(ibd->ibd_bd, 
			&__total_bytes, &__used_blks);

		if (ret != 0)
			return ret;

		total = ((sector_t)__total_bytes >> bs_order);
		used = (((sector_t)__used_blks << 9) >> bs_order);
		*ret_used_blks = used;
		*ret_avail_blks = total - used;
		ret = 0;

	}

	if (!ret) {
		pr_debug("%s: total:0x%llx, used_blks:0x%llx, avail_blks:0x%llx\n", 
			__func__, (unsigned long long)total, 
			(unsigned long long)(*ret_used_blks),
			(unsigned long long)(*ret_avail_blks));

		return 0;
	}

	return -ENODEV;

}

int qnap_transport_get_ac_and_uc_on_thin(
	struct se_device *se_dev,
	u32 *ac,
	u32 *uc
	)
{
	sector_t avail_blks, used_blks;
	int ret, threshold_exp;

	*ac = 0, *uc = 0;

	ret = __qnap_transport_get_a_blks_and_u_blks_on_thin(se_dev, 
		ilog2(se_dev->dev_attrib.block_size), &avail_blks, &used_blks);

	if (ret != 0)
		return ret;

	threshold_exp = qnap_sbc_get_threshold_exp(se_dev);

	*ac = (u32)div_u64(avail_blks, (1 << threshold_exp));
	*uc = (u32)div_u64(used_blks, (1 << threshold_exp));
	return 0;
}
EXPORT_SYMBOL(qnap_transport_get_ac_and_uc_on_thin);


int qnap_transport_is_fio_blk_backend(
	struct se_device *se_dev
	)
{
	if (se_dev->dev_dr.dev_type == QNAP_DT_FIO_BLK)
		return 0;
	return -ENODEV;
}
EXPORT_SYMBOL(qnap_transport_is_fio_blk_backend);

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

void qnap_transport_tmr_abort_task(
	struct se_device *dev,
	struct se_tmr_req *tmr,
	struct se_session *se_sess
	)
{
	/* TODO: This call shall depend on your native code */
	struct se_node_acl *tmr_nacl = NULL;
	struct se_cmd *se_cmd;
	unsigned long flags;
	u64 ref_tag;

	spin_lock_irqsave(&se_sess->sess_cmd_lock, flags);
	list_for_each_entry(se_cmd, &se_sess->sess_cmd_list, se_cmd_list) {

		if (dev != se_cmd->se_dev)
			continue;

		/* skip task management functions, including tmr->task_cmd */
		if (se_cmd->se_cmd_flags & SCF_SCSI_TMR_CDB)
			continue;

		ref_tag = se_cmd->tag;
		if (tmr->ref_task_tag != ref_tag)
			continue;

		if (atomic_read(&se_cmd->cmd_dr.tmr_code))
			continue;

		if (tmr->task_cmd && tmr->task_cmd->se_sess)
			tmr_nacl = tmr->task_cmd->se_sess->se_node_acl;

		printk("ABORT_TASK: Found referenced %s task_tag:0x%llx, "
			"cmdsn:0x%08x\n",
			se_cmd->se_tfo->get_fabric_name(), 
			(unsigned long long)ref_tag,
			se_cmd->se_tfo->get_cmdsn(se_cmd));

		/* setup TMR code and check it is same or different i_t_nexus */
		atomic_set(&se_cmd->cmd_dr.tmr_code, QNAP_TMR_CODE_ABORT_TASK);

		if (tmr_nacl && (tmr_nacl != se_cmd->se_sess->se_node_acl)){
			atomic_set(&se_cmd->cmd_dr.tmr_diff_it_nexus, 1);
			if (se_cmd->se_dev->dev_attrib.emulate_tas)
				atomic_set(&se_cmd->cmd_dr.tmr_resp_tas, 1);				
		}

		qnap_transport_drop_bb_cmd(se_cmd, TMR_ABORT_TASK);
		qnap_transport_drop_fb_cmd(se_cmd, TMR_ABORT_TASK);

		se_cmd->se_tfo->set_delay_remove(se_cmd, true);

		spin_unlock_irqrestore(&se_sess->sess_cmd_lock, flags);

		printk("ABORT_TASK: Sending TMR_FUNCTION_COMPLETE for "
			"ref_tag:0x%llx. Aborted task: itt:0x%08x, "
			"cmdsn:0x%08x, scsi op:0x%x\n", 
			(unsigned long long)ref_tag, 
			se_cmd->se_tfo->get_task_tag(se_cmd),
			se_cmd->se_tfo->get_cmdsn(se_cmd), se_cmd->t_task_cdb[0]);

		tmr->response = TMR_FUNCTION_COMPLETE;
		atomic_long_inc(&dev->aborts_complete);
		return;
	}
	spin_unlock_irqrestore(&se_sess->sess_cmd_lock, flags);

	printk("ABORT_TASK: Sending TMR_TASK_DOES_NOT_EXIST for ref_tag: 0x%llx\n",
		(unsigned long long)tmr->ref_task_tag);

	tmr->response = TMR_TASK_DOES_NOT_EXIST;
	atomic_long_inc(&dev->aborts_no_task);
	return;
}

void qnap_transport_tmr_drain_state_list(
	struct se_device *dev,
	struct se_cmd *prout_cmd,
	struct se_session *tmr_sess,
	int tas,
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),
	struct list_head *preempt_and_abort_list
	)
{
	/* TODO: This call shall depend on your native code */
	struct se_node_acl *tmr_nacl = NULL;
	struct se_cmd *cmd, *next;
	unsigned long flags;

	/*
	 * Complete outstanding commands with TASK_ABORTED SAM status.
	 *
	 * This is following sam4r17, section 5.6 Aborting commands, Table 38
	 * for TMR LUN_RESET:
	 *
	 * a) "Yes" indicates that each command that is aborted on an I_T nexus
	 * other than the one that caused the SCSI device condition is
	 * completed with TASK ABORTED status, if the TAS bit is set to one in
	 * the Control mode page (see SPC-4). "No" indicates that no status is
	 * returned for aborted commands.
	 *
	 * d) If the logical unit reset is caused by a particular I_T nexus
	 * (e.g., by a LOGICAL UNIT RESET task management function), then "yes"
	 * (TASK_ABORTED status) applies.
	 *
	 * Otherwise (e.g., if triggered by a hard reset), "no"
	 * (no TASK_ABORTED SAM status) applies.
	 *
	 * Note that this seems to be independent of TAS (Task Aborted Status)
	 * in the Control Mode Page.
	 */

	if (tmr_sess)
		tmr_nacl = tmr_sess->se_node_acl;
	spin_lock_irqsave(&dev->execute_task_lock, flags);
	list_for_each_entry_safe(cmd, next, &dev->state_list, state_list) {
		/*
		 * For PREEMPT_AND_ABORT usage, only process commands
		 * with a matching reservation key.
		 */
		if (!check_cdb_and_preempt)
			continue;

		if (check_cdb_and_preempt(preempt_and_abort_list, cmd))
			continue;

		/*
		 * Not aborting PROUT PREEMPT_AND_ABORT CDB..
		 */
		if (prout_cmd == cmd)
			continue;

		if (atomic_read(&cmd->cmd_dr.tmr_code))
			continue;

		/* setup TMR code and check it is same or different i_t_nexus */
		atomic_set(&cmd->cmd_dr.tmr_code, QNAP_TMR_CODE_LUN_RESET);

		if (tmr_nacl && (tmr_nacl != cmd->se_sess->se_node_acl)){
			atomic_set(&cmd->cmd_dr.tmr_diff_it_nexus, 1);
			if (cmd->se_dev->dev_attrib.emulate_tas)
				atomic_set(&cmd->cmd_dr.tmr_resp_tas, 1);
		}

		qnap_transport_drop_bb_cmd(cmd, TMR_LUN_RESET);
		qnap_transport_drop_fb_cmd(cmd, TMR_LUN_RESET);

		cmd->se_tfo->set_delay_remove(cmd, true);
	}
	spin_unlock_irqrestore(&dev->execute_task_lock, flags);

	return;
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

/* the function for qnap_transport_enumerate_hba_for_deregister_xxxxx 
 * depends on native code
 */
static int qnap_transport_enumerate_hba_for_deregister_session_iter(
	struct se_device *se_dev,
	void *data
	)
{
	struct se_session *se_sess = data;

	spin_lock(&se_dev->dev_reservation_lock);
	if (!se_dev->dev_reserved_node_acl) {
		spin_unlock(&se_dev->dev_reservation_lock);
		return 0;
	}

	if (se_dev->dev_reserved_node_acl != se_sess->se_node_acl) {
		spin_unlock(&se_dev->dev_reservation_lock);
		return 0;
	}

	se_dev->dev_reserved_node_acl = NULL;
	se_dev->dev_flags &= ~DRF_SPC2_RESERVATIONS;
	if (se_dev->dev_flags & DRF_SPC2_RESERVATIONS_WITH_ISID) {
		se_dev->dev_res_bin_isid = 0;
		se_dev->dev_flags &= ~DRF_SPC2_RESERVATIONS_WITH_ISID;
	}
	spin_unlock(&se_dev->dev_reservation_lock);

	return 0;
}

void qnap_transport_enumerate_hba_for_deregister_session(
	struct se_session *se_sess
	)
{
	int ret = 0;

	ret = target_for_each_device(qnap_transport_enumerate_hba_for_deregister_session_iter, se_sess);

	if (ret != 0)
		pr_err_ratelimited("Unexpected error in %s()\n", __func__);

	return;
}

#ifdef SUPPORT_TP

/* 0: normal i/o (not hit sync i/o threshold)
 * 1: hit sync i/o threshold
 * -ENOSPC: pool space is full
 * -EINVAL: wrong parameter to call function
 * -ENODEV: no such device
 */
int qnap_transport_check_dm_thin_cond(
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
EXPORT_SYMBOL(qnap_transport_check_dm_thin_cond);

int qnap_transport_check_cmd_hit_thin_threshold(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	sector_t a_blks, u_blks;
	uint64_t t_min,  dividend;
	int ret, reached = 0, under_threshold = 0;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;

	if (!qlib_thin_lun(&se_dev->dev_dr))
		return -ENODEV;

	/* we ONLY handle the write-direction command */
	ret = __qnap_transport_get_a_blks_and_u_blks_on_thin(
		se_dev, bs_order, &a_blks, &u_blks);
	
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
	} else {
		if (dev_attr_dr->tp_threshold_hit != 0) {
			dev_attr_dr->tp_threshold_hit = 0;
			check_lun_threshold_for_each_device(&under_threshold);
			/* under_threshold == 0, there are some LUN is still over threshold.
			   under_threshold == 0, all the LUN is under threshold. */
		}
	}
	
	if (reached) {
#if defined(QNAP_HAL)	
		NETLINK_EVT hal_event;
		
		memset(&hal_event, 0, sizeof(NETLINK_EVT));
		hal_event.type = HAL_EVENT_ISCSI;

		hal_event.arg.action = HIT_LUN_THRESHOLD;
		hal_event.arg.param.iscsi_lun.lun_index = dev_attr_dr->lun_index;
		hal_event.arg.param.iscsi_lun.tp_threshold = dev_attr_dr->tp_threshold_percent;

		/* unit: GB */
		hal_event.arg.param.iscsi_lun.tp_avail = 
			((a_blks << bs_order) >> 30);

		/* call function if it exists since we declare it as weak symbol */
		if (send_hal_netlink)
			send_hal_netlink(&hal_event);
#endif
		return 0;
	} else if (under_threshold) {
#if defined(QNAP_HAL)	
		NETLINK_EVT hal_event;
		
		memset(&hal_event, 0, sizeof(NETLINK_EVT));
		hal_event.type = HAL_EVENT_ISCSI;

		hal_event.arg.action = UNDER_LUN_THRESHOLD;
		hal_event.arg.param.iscsi_lun.lun_index = dev_attr_dr->lun_index;
		hal_event.arg.param.iscsi_lun.tp_threshold = dev_attr_dr->tp_threshold_percent;

		/* unit: GB */
		hal_event.arg.param.iscsi_lun.tp_avail = 
			((a_blks << bs_order) >> 30);

		/* call function if it exists since we declare it as weak symbol */
		if (send_hal_netlink)
			send_hal_netlink(&hal_event);
#endif
	}

	return -EPERM;
}
EXPORT_SYMBOL(qnap_transport_check_cmd_hit_thin_threshold);

void qnap_transport_upadte_write_result_on_fio_thin(
	struct se_cmd *se_cmd,
	int *original_result
	)
{
	/* 1. go this way if original result is GOOD and this is thin lun 
	 * 2. execute this in fd_execute_rw()
	 */
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	int ret, ret1, ret2;
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	loff_t start, len = 0, idx = 0;
	struct scatterlist *sg;

	if (se_cmd->data_direction != DMA_TO_DEVICE)
		return;

	if (*original_result <= 0)
		return;

	if (!qlib_thin_lun(&se_dev->dev_dr))
		return;

	/* To sync cache again if write ok and the sync cache
	 * behavior shall work for thin lun only 
	 */
	start = (se_cmd->t_task_lba << bs_order);

	for_each_sg(se_cmd->t_data_sg, sg, se_cmd->t_data_nents, idx)
		len += sg->length;

	/* first time to check whether we goes to sync i/o condition or not */
	ret = qnap_transport_check_dm_thin_cond(se_dev);
	if (ret == 0 || ret == -EINVAL || ret == -ENODEV)
		return;

	/* if hit condition, force to report no space. we don't care the
	 * io position was mapped or is new allocated
	 */
	if (qnap_transport_hit_read_deletable(&se_cmd->se_dev->dev_dr, se_cmd->t_task_cdb)) {
		*original_result = -ENOSPC;
		return;
	}

	/* time to do sync i/o
	 * 1. hit the sync i/o threshold area
	 * 2. or, space is full BUT need to handle lba where
	 *    was mapped or not
	 */
	ret1 = qlib_fd_sync_cache_range(fd_dev->fd_file, start, (start + len));
	if (ret1 == 0)
		return;

	/* fail from sync cache -
	 * thin i/o may go somewhere (lba wasn't mapped to any block)
	 * or something wrong during normal sync-cache
	 */

	/* call again to make sure it is no space really or not */
	ret2 = qnap_transport_check_dm_thin_cond(se_dev);
	if (ret2 == -ENOSPC) {
		pr_warn("%s: space was full already\n",__func__);
		ret1 = ret2;
	}

	*original_result = ret1;
	return;
}
EXPORT_SYMBOL(qnap_transport_upadte_write_result_on_fio_thin);

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

void qnap_transport_config_zc_val(
	struct se_device *dev
	)
{
	/* zero-copy only supports on fio + blkdev configuration */
	if(qlib_is_fio_blk_dev(&dev->dev_dr) == 0) {
		dev->dev_dr.dev_zc = 0;
		return;
	}
	/* other cases is 0 */
	dev->dev_dr.dev_zc = 0;
	return;
}

static void __qnap_init_zc_val(
	struct qnap_se_dev_dr *dr
	)
{
	spin_lock_init(&dr->dev_zc_lock);
	dr->dev_zc = 0;
}

static void __qnap_init_queue_obj(
	struct se_queue_obj *qobj
	)
{
	atomic_set(&qobj->queue_cnt, 0);
	INIT_LIST_HEAD(&qobj->qobj_list);
	init_waitqueue_head(&qobj->thread_wq);
	spin_lock_init(&qobj->queue_lock);
}

static void __qnap_init_wt_val(
	struct qnap_se_dev_dr *dr
	)
{
	spin_lock_init(&dr->dev_wt_lock);
	dr->dev_wt = 1;
	dr->process_thread = NULL;
	__qnap_init_queue_obj(&dr->dev_queue_obj);
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

#ifdef QNAP_TARGET_PERF_FEATURE
	if (qnap_target_perf_enabled())
		return;
#endif
	/* following items are for target perf disabled */
	__qnap_init_zc_val(dr);
	__qnap_init_wt_val(dr);

	dr->read_process_thread = NULL;
	__qnap_init_queue_obj(&dr->dev_read_queue_obj);

	atomic_set(&dr->hit_read_deletable, 0);
}

static void __qnap_transport_add_cmd_to_queue(
	struct se_cmd *cmd, 
	struct se_queue_obj *qobj,
	int t_state,
	bool at_head
	)
{
	unsigned long flags;

	cmd->b_se_state |= BS_SE_CMD_ADD_TO_Q;
	cmd->b_se_state_last = BS_SE_CMD_ADD_TO_Q;
	
	if (t_state) {
		spin_lock_irqsave(&cmd->t_state_lock, flags);
		cmd->t_state = t_state;
		cmd->transport_state |= CMD_T_ACTIVE;
		spin_unlock_irqrestore(&cmd->t_state_lock, flags);
	}
	
	spin_lock_irqsave(&qobj->queue_lock, flags);

	/* If the cmd is already on the list, remove it before we add it */
	if (!list_empty(&cmd->cmd_dr.se_queue_node))
		list_del_init(&cmd->cmd_dr.se_queue_node);
	else
		atomic_inc(&qobj->queue_cnt);
	
	if (at_head)
		list_add(&cmd->cmd_dr.se_queue_node, &qobj->qobj_list);
	else
		list_add_tail(&cmd->cmd_dr.se_queue_node, &qobj->qobj_list);

	set_bit(QNAP_CMD_T_QUEUED, &cmd->cmd_dr.cmd_t_state);

	spin_unlock_irqrestore(&qobj->queue_lock, flags);

	wake_up_interruptible(&qobj->thread_wq);

}

static void __qnap_transport_remove_cmd_from_queue(
	struct se_cmd *cmd
	)
{
	struct se_queue_obj *qobj = &cmd->se_dev->dev_dr.dev_queue_obj;
	unsigned long flags;

	spin_lock_irqsave(&qobj->queue_lock, flags);
	if (!test_bit(QNAP_CMD_T_QUEUED, &cmd->cmd_dr.cmd_t_state)) {
		spin_unlock_irqrestore(&qobj->queue_lock, flags);
		return;
	}

	clear_bit(QNAP_CMD_T_QUEUED, &cmd->cmd_dr.cmd_t_state);

	atomic_dec(&qobj->queue_cnt);
	list_del_init(&cmd->cmd_dr.se_queue_node);
	spin_unlock_irqrestore(&qobj->queue_lock, flags);
}

static struct se_cmd *__qnap_transport_get_cmd_from_queue(
	struct se_queue_obj *qobj
	)
{
	struct se_cmd *cmd;
	struct qnap_se_cmd_dr *cmd_dr = NULL;
	unsigned long flags;

	spin_lock_irqsave(&qobj->queue_lock, flags);
	if (list_empty(&qobj->qobj_list)) {
		spin_unlock_irqrestore(&qobj->queue_lock, flags);
		return NULL;
	}

	cmd_dr = list_first_entry(&qobj->qobj_list, struct qnap_se_cmd_dr,
			se_queue_node);

	cmd = container_of(cmd_dr, struct se_cmd, cmd_dr);
	clear_bit(QNAP_CMD_T_QUEUED, &cmd_dr->cmd_t_state);

	if (list_empty(&cmd_dr->se_queue_node))
		WARN_ON(1);

	list_del_init(&cmd_dr->se_queue_node);
	atomic_dec(&qobj->queue_cnt);
	spin_unlock_irqrestore(&qobj->queue_lock, flags);

	return cmd;
}

int qnap_transport_read_processing_thread(
	void *param
	)
{
	int ret;
	struct se_cmd *cmd;
	struct se_device *dev = param;
	
	set_user_nice(current, -20);
	
	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(dev->dev_dr.dev_read_queue_obj.thread_wq,
				atomic_read(&dev->dev_dr.dev_read_queue_obj.queue_cnt) ||
				kthread_should_stop());
		if (ret < 0)
			goto out;
	
get_cmd:
		cmd = __qnap_transport_get_cmd_from_queue(&dev->dev_dr.dev_read_queue_obj);
		if (!cmd)
			continue;
	
		switch (cmd->t_state) {
		case TRANSPORT_WORK_THREAD_PROCESS:
			target_execute_cmd(cmd);
			break;
		default:
			pr_err("Unknown t_state: %d, "
				"i_state: %d on SE LUN: %llu\n",
				cmd->t_state,
				cmd->se_tfo->get_cmd_state(cmd),
				cmd->se_lun->unpacked_lun);
			BUG();
		}
	
		goto get_cmd;
	}
	
out:
	WARN_ON(!list_empty(&dev->dev_dr.dev_queue_obj.qobj_list));
	dev->dev_dr.read_process_thread = NULL;
	return 0;
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

int qnap_transport_processing_thread(
	void *param
	)
{
	int ret;
	struct se_cmd *cmd;
	struct se_device *dev = param;
	
	set_user_nice(current, -20);
	
	qnap_transport_thread_cpu_setting(dev);
	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(dev->dev_dr.dev_queue_obj.thread_wq,
				atomic_read(&dev->dev_dr.dev_queue_obj.queue_cnt) ||
				kthread_should_stop());
		if (ret < 0)
			goto out;
	
get_cmd:
		cmd = __qnap_transport_get_cmd_from_queue(&dev->dev_dr.dev_queue_obj);
		if (!cmd)
			continue;

		if (cmd->dev_cpumask != 0 && dev->dev_dr.se_dev_thread_cpumask ==0xffff) {
			dev->dev_dr.se_dev_thread_cpumask = cmd->dev_cpumask;
			qnap_transport_thread_cpu_setting(dev);
		}

		switch (cmd->t_state) {
		case TRANSPORT_WORK_THREAD_PROCESS:
			target_execute_cmd(cmd);
			break;
		default:
			pr_err("Unknown t_state: %d, "
				"i_state: %d on SE LUN: %llu\n",
				cmd->t_state,
				cmd->se_tfo->get_cmd_state(cmd),
				cmd->se_lun->unpacked_lun);
			BUG();
		}
	
		goto get_cmd;
	}
	
out:
	WARN_ON(!list_empty(&dev->dev_dr.dev_queue_obj.qobj_list));
	dev->dev_dr.process_thread = NULL;
	return 0;
}

void qnap_transport_tmr_drain_cmd_list(
	struct se_device *dev,
	struct se_queue_obj *qobj,
	struct se_cmd *prout_cmd,
	struct se_node_acl *tmr_nacl,
	int tas,
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),
	struct list_head *preempt_and_abort_list
	)
{
	struct qnap_se_cmd_dr *cmd_dr, *t_cmd_dr;
	struct se_cmd *cmd;
	unsigned long flags;

	/*
	 * Release all commands remaining in the struct se_device cmd queue.
	 *
	 * This follows the same logic as above for the struct se_device
	 * struct se_task state list, where commands are returned with
	 * TASK_ABORTED status, if there is an outstanding $FABRIC_MOD
	 * reference, otherwise the struct se_cmd is released.
	 */
	spin_lock_irqsave(&qobj->queue_lock, flags);

	list_for_each_entry_safe(cmd_dr, t_cmd_dr, &qobj->qobj_list, se_queue_node) {

		cmd = container_of(cmd_dr, struct se_cmd, cmd_dr);

		BUG_ON(!cmd);

		/*
		 * For PREEMPT_AND_ABORT usage, only process commands
		 * with a matching reservation key.
		 */
		if (!check_cdb_and_preempt)
			continue;

		if (check_cdb_and_preempt(preempt_and_abort_list, cmd))
			continue;
		/*
		 * Not aborting PROUT PREEMPT_AND_ABORT CDB..
		 */
		if (prout_cmd == cmd)
			continue;

		if (atomic_read(&cmd_dr->tmr_code))
			continue;


		/* setup TMR code and check it is same or different i_t_nexus */
		atomic_set(&cmd_dr->tmr_code, QNAP_TMR_CODE_LUN_RESET);

		if (tmr_nacl && (tmr_nacl != cmd->se_sess->se_node_acl)){
			atomic_set(&cmd_dr->tmr_diff_it_nexus, 1);
			if (cmd->se_dev->dev_attrib.emulate_tas)
				atomic_set(&cmd_dr->tmr_resp_tas, 1);
		}

		cmd->se_tfo->set_delay_remove(cmd, true);
	}

	spin_unlock_irqrestore(&qobj->queue_lock, flags);
}

int qnap_transport_exec_rt_cmd(
	struct se_cmd *se_cmd
	)
{
	struct se_device *dev = se_cmd->se_dev;
	struct se_queue_obj *r_qobj = &dev->dev_dr.dev_read_queue_obj;

	if (se_cmd->se_dev->dev_dr.read_process_thread) {
		__qnap_transport_add_cmd_to_queue(se_cmd, r_qobj,
			TRANSPORT_WORK_THREAD_PROCESS, false);
		return 0;
	}
	return -1;
}
EXPORT_SYMBOL(qnap_transport_exec_rt_cmd);

int qnap_transport_exec_wt_cmd(
	struct se_cmd *se_cmd
	)
{
	struct se_device *dev = se_cmd->se_dev;
	struct se_queue_obj *w_qobj = &dev->dev_dr.dev_queue_obj;

	se_cmd->b_se_state |= BS_SE_CMD_EXEC_WT_CMD;
	se_cmd->b_se_state_last = BS_SE_CMD_EXEC_WT_CMD;

	spin_lock(&se_cmd->se_dev->dev_dr.dev_wt_lock);

	if ((se_cmd->se_dev->dev_dr.dev_wt == 1)
	&& se_cmd->se_dev->dev_dr.process_thread
	)
	{
		spin_unlock(&se_cmd->se_dev->dev_dr.dev_wt_lock);
		__qnap_transport_add_cmd_to_queue(se_cmd, w_qobj,
			TRANSPORT_WORK_THREAD_PROCESS, false);
		return 0;
	}

	spin_unlock(&se_cmd->se_dev->dev_dr.dev_wt_lock);
	return -1;
}
EXPORT_SYMBOL(qnap_transport_exec_wt_cmd);

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

static char * __qnap_get_drop_type_str(
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
		str = NULL;
		break;
	}

	return str;
}

/* this function will be called when
 *
 * 1. dev->execute_task_lock was hold with spin_xxx_irqxxx() for LUN RESET
 * 2. or, se_sess->sess_cmd_lock was hold with spin_xxx_irqxxx() for ABORT TASK
 * 3. or, conn->cmd_lock was hold with spin_xx_bh()
 */
int qnap_transport_drop_bb_cmd(
	struct se_cmd *se_cmd,
	int type
	)
{
	int ret;
	char *type_str;
	char tmp_str[256];
	struct se_device *se_dev = se_cmd->se_dev;

	/* we do this only for fio + block-backend configuration */
	if (qlib_is_fio_blk_dev(&se_dev->dev_dr) != 0)
		return -ENODEV;

	type_str = __qnap_get_drop_type_str(type);
	if (!type_str)
		return -EINVAL;

	memset(tmp_str, 0, sizeof(tmp_str));

#ifdef SUPPORT_TPC_CMD
	qnap_odx_drop_cmd(se_cmd, type);
#endif

	if (type == -1) {
		set_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &se_cmd->cmd_dr.cmd_t_state);
	} else {

		ret = qlib_drop_bb_ask_drop(&se_dev->dev_dr, 
				&se_cmd->cmd_dr.iov_obj, &tmp_str[0]);

		if (ret != 0)
			return ret;
	}

	pr_info("[iSCSI (block-based)][%s] ip: %pISc, drop itt:0x%08x, "
		"scsi op:0x%x. %s, b_se_state:0x%llx(0x%llx), ptime:%ld(s)\n", type_str,
		se_cmd->se_tfo->get_login_ip(se_cmd),
		be32_to_cpu(se_cmd->se_tfo->get_task_tag(se_cmd)), 
		se_cmd->t_task_cdb[0], tmp_str,
		se_cmd->b_se_state, se_cmd->b_se_state_last, 
		((jiffies - se_cmd->start_jiffies) / HZ));

	return 0;

}
EXPORT_SYMBOL(qnap_transport_drop_bb_cmd);

int qnap_transport_drop_fb_cmd(
	struct se_cmd *se_cmd,
	int type
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	int count = 0;
	struct fbdisk_device *fb = NULL;
	struct iblock_dev *ib_dev = NULL;
	struct bio_rec *brec, *tmp_brec;
	char *type_str;
	struct __bio_obj *obj = NULL;

	if (qlib_is_ib_fbdisk_dev(&se_dev->dev_dr) != 0)
		return -ENODEV;

	ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
	fb = ib_dev->ibd_bd->bd_disk->private_data;
	if (!fb)
		return -ENODEV;

	type_str = __qnap_get_drop_type_str(type);
	if (!type_str)
		return -EINVAL;

#ifdef SUPPORT_TPC_CMD
	qnap_odx_drop_cmd(se_cmd, type);
#endif

	/* try to drop all possible bio for iblock + block-backend */
	if (type == -1) {
		set_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &se_cmd->cmd_dr.cmd_t_state);
	}

	obj = &se_cmd->cmd_dr.bio_obj;

	/* take care this ..., caller hold lock with spin_xxx_bh() already */
	spin_lock(&obj->bio_rec_lists_lock);
	list_for_each_entry_safe(brec, tmp_brec, &obj->bio_rec_lists, node)
	{
		if (brec->bio) {
			count++;
			qnap_iscsi_fbdisk_set_bio_drop(brec);

			pr_info("[iSCSI (file-based)][%s] ip: %pISc, "
				"drop itt:0x%08x, scsi op:0x%x, "
				"bio:0x%p, dev:%s\n", 
				type_str, se_cmd->se_tfo->get_login_ip(se_cmd),
				be32_to_cpu(se_cmd->se_tfo->get_task_tag(se_cmd)),
				se_cmd->t_task_cdb[0],
				brec->bio, fb->fb_device->bd_disk->disk_name);
		}
	}
	spin_unlock(&obj->bio_rec_lists_lock);

	return 0;

}
EXPORT_SYMBOL(qnap_transport_drop_fb_cmd);

sense_reason_t
qnap_transport_iblock_execute_write_same_direct(
	struct block_device *bdev, 
	struct se_cmd *cmd
	)
{
#define NORMAL_IO_TIMEOUT	5

	struct se_device *se_dev = cmd->se_dev;
	sector_t block_lba = cmd->t_task_lba, work_lba;
	sector_t total_sects = sbc_get_write_same_sectors(cmd);
	struct scatterlist *sgl = NULL;
	struct scatterlist *ori_sg = &cmd->t_data_sg[0];
	u64 alloc_bytes = (1 << 20), total_bytes, work_bytes;
	u32 bs_order = ilog2(se_dev->dev_attrib.block_size), idx, total_copy;
	GEN_RW_TASK w_task;
	sense_reason_t s_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	void *tmp_buf = NULL;
	int ret;

	memset(&w_task, 0, sizeof(GEN_RW_TASK));

	w_task.sg_list = qlib_alloc_sg_list((u32 *)&alloc_bytes, &w_task.sg_nents);
	if (!w_task.sg_list) {
		pr_err("%s: fail to alloc sg list\n", __func__);
		return TCM_OUT_OF_RESOURCES;
	}

	/* copy original sg data to our sg lists ... */
	sgl = w_task.sg_list;

	for (idx = 0; idx < w_task.sg_nents; idx++) {
		total_copy = sgl[idx].length;
		tmp_buf = kmap(sg_page(&sgl[idx])) + sgl[idx].offset;

		/* original sg data size (512b or 4096b) may <= PAGE_SIZE, 
		 * here will try fill full to our sg lists
		 */
		while (total_copy) {
			sg_copy_to_buffer(ori_sg, cmd->t_data_nents, 
				tmp_buf, (1 << bs_order));

			tmp_buf += (size_t)(1 << bs_order);
			total_copy -= (1 << bs_order);
		}
		kunmap(sg_page(&sgl[idx]));		
	}

	/* start to write data directly ... */
	total_bytes = ((u64)total_sects << bs_order);

	while (total_bytes) {
		work_bytes = min_t(u64, total_bytes, alloc_bytes);
		work_lba = (work_bytes >> bs_order);

		qnap_transport_make_rw_task((void *)&w_task, se_dev,
			block_lba, work_lba,
			msecs_to_jiffies(NORMAL_IO_TIMEOUT*1000), 
			DMA_TO_DEVICE);

		ret = qnap_transport_do_b_rw((void *)&w_task);

		if((ret <= 0) || w_task.is_timeout || w_task.ret_code != 0){
			if (w_task.ret_code == -ENOSPC)
				s_ret = TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
			else
				s_ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

			goto _exit_;
		}
		block_lba += work_lba;
		total_bytes -= work_bytes;
	}

	s_ret = TCM_NO_SENSE;

_exit_:
	qlib_free_sg_list(w_task.sg_list, w_task.sg_nents);

	if (s_ret == TCM_NO_SENSE)
		target_complete_cmd(cmd, GOOD);

	return s_ret;
}
EXPORT_SYMBOL(qnap_transport_iblock_execute_write_same_direct);

int qnap_set_write_cache(struct se_device *se_dev, bool set_wc)
{
	bool is_fio_blkdev;
	struct iblock_dev *ib_dev = NULL;
	struct request_queue *q = NULL;

	if (qlib_is_fio_blk_dev(&se_dev->dev_dr) == 0)
		is_fio_blkdev = true;
	else if (qlib_is_ib_fbdisk_dev(&se_dev->dev_dr) == 0)
		is_fio_blkdev = false;
	else
		return -ENODEV;

	if (is_fio_blkdev == false) {
		ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		q = bdev_get_queue(ib_dev->ibd_bd);

		/* Use blk_queue_write_cache() maybe? */
		spin_lock_irq(q->queue_lock);
		if (set_wc)
			queue_flag_set(QUEUE_FLAG_WC, q);
		else
			queue_flag_clear(QUEUE_FLAG_WC, q);
		spin_unlock_irq(q->queue_lock);
	}

	se_dev->dev_attrib.emulate_write_cache = (int)set_wc;

	return 0;

}
EXPORT_SYMBOL(qnap_set_write_cache);

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

bool qnap_transport_create_sess_lio_cmd_cache(
	struct qnap_se_sess_dr *dr,
	int idx,
	size_t alloc_size,
	size_t align_size
	)
{
	char tmp[256];
	size_t real_size;

	real_size = ((alloc_size - 1 + align_size) / align_size) * align_size;

	sprintf(tmp, "lio_cmd_cache_%d", idx);

	dr->lio_cmd_cache = kmem_cache_create(tmp, real_size, align_size, 0, NULL);

	if (!dr->lio_cmd_cache) {
		pr_err("%s: Unable create sess_lio_cmd_cache\n", __func__);
		return false;
	}

	atomic_set(&dr->cmd_count, 0);
	pr_debug("%s: alloc_size:%d, align_size:%d\n", __func__, 
			alloc_size, align_size);

	return true;
}

void qnap_transport_destroy_sess_lio_cmd_cache(
	struct qnap_se_sess_dr *dr
	)
{
	if (dr->lio_cmd_cache) {
		pr_debug("%s: cmd count:%d\n", __func__, 
				atomic_read(&dr->cmd_count));
		kmem_cache_destroy(dr->lio_cmd_cache);
	}
}

void *qnap_transport_alloc_sess_lio_cmd(
	struct qnap_se_sess_dr *dr, 
	gfp_t gfp_mask
	)
{
	void *cmd = NULL;

	if (dr->lio_cmd_cache) {
		cmd = kmem_cache_zalloc(dr->lio_cmd_cache, gfp_mask);
		if (cmd) {
			pr_debug("alloc icmd from sess_lio_cmd_cache\n"); 	
			atomic_inc(&dr->cmd_count);
		} else
			pr_err("%s: fail alloc icmd from sess_lio_cmd_cache\n",
				__func__); 	
	}

	return cmd;
}

void qnap_transport_free_sess_lio_cmd(
	struct qnap_se_sess_dr *dr, 
	void *p
	)
{
	if (dr->lio_cmd_cache) {
		kmem_cache_free(dr->lio_cmd_cache, p);
		atomic_dec(&dr->cmd_count);
	}
}

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
	return dev->dev_attrib.emulate_v_sup > 0;
}

sense_reason_t qnap_transport_convert_rc_to_tcm_sense_reason(
	RC rc
	)
{
	sense_reason_t ret;

	switch (rc) {
	case RC_GOOD:
		return TCM_NO_SENSE;
	case RC_UNKNOWN_SAM_OPCODE:
		return TCM_UNSUPPORTED_SCSI_OPCODE;
	case RC_REQ_TOO_MANY_SECTORS:
		return TCM_SECTOR_COUNT_TOO_MANY;
	case RC_INVALID_CDB_FIELD:
		return TCM_INVALID_CDB_FIELD;
	case RC_INVALID_PARAMETER_LIST:
		return TCM_INVALID_PARAMETER_LIST;
	case RC_UNKNOWN_MODE_PAGE:
		return TCM_UNKNOWN_MODE_PAGE;
	case RC_WRITE_PROTECTEDS:
		return TCM_WRITE_PROTECTED;
	case RC_RESERVATION_CONFLICT:
		return TCM_RESERVATION_CONFLICT;
	case RC_CHECK_CONDITION_NOT_READY:
		return TCM_CHECK_CONDITION_NOT_READY;
	case RC_CHECK_CONDITION_ABORTED_CMD:
		return TCM_CHECK_CONDITION_ABORT_CMD;
	case RC_CHECK_CONDITION_UA:
		return TCM_CHECK_CONDITION_UNIT_ATTENTION;
	case RC_LBA_OUT_OF_RANGE:
		return TCM_ADDRESS_OUT_OF_RANGE;
	case RC_MISCOMPARE_DURING_VERIFY_OP:
		return TCM_MISCOMPARE_VERIFY;
	case RC_PARAMETER_LIST_LEN_ERROR:
		return TCM_PARAMETER_LIST_LENGTH_ERROR;
	case RC_UNREACHABLE_COPY_TARGET:
		return TCM_UNREACHABLE_COPY_TARGET;
	case RC_3RD_PARTY_DEVICE_FAILURE:
		return TCM_3RD_PARTY_DEVICE_FAILURE;
	case RC_INCORRECT_COPY_TARGET_DEV_TYPE:
		return TCM_INCORRECT_COPY_TARGET_DEV_TYPE;
	case RC_TOO_MANY_TARGET_DESCRIPTORS:
		return TCM_TOO_MANY_TARGET_DESCS;
	case RC_TOO_MANY_SEGMENT_DESCRIPTORS:
		return TCM_TOO_MANY_SEGMENT_DESCS;
	case RC_ILLEGAL_REQ_DATA_OVERRUN_COPY_TARGET:
		return TCM_ILLEGAL_REQ_DATA_OVERRUN_COPY_TARGET;
	case RC_ILLEGAL_REQ_DATA_UNDERRUN_COPY_TARGET:
		return TCM_ILLEGAL_REQ_DATA_UNDERRUN_COPY_TARGET;
	case RC_COPY_ABORT_DATA_OVERRUN_COPY_TARGET:
		return TCM_COPY_ABORT_DATA_OVERRUN_COPY_TARGET;
	case RC_COPY_ABORT_DATA_UNDERRUN_COPY_TARGET:
		return TCM_COPY_ABORT_DATA_UNDERRUN_COPY_TARGET;
	case RC_INSUFFICIENT_RESOURCES:
		return TCM_INSUFFICIENT_RESOURCES;
	case RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD:
		return TCM_INSUFFICIENT_RESOURCES_TO_CREATE_ROD;
	case RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD_TOKEN:
		return TCM_INSUFFICIENT_RESOURCES_TO_CREATE_ROD_TOKEN;
	case RC_OPERATION_IN_PROGRESS:
		return TCM_OPERATION_IN_PROGRESS;
	case RC_INVALID_TOKEN_OP_AND_INVALID_TOKEN_LEN:
		return TCM_INVALID_TOKEN_OP_AND_INVALID_TOKEN_LEN;
	case RC_INVALID_TOKEN_OP_AND_CAUSE_NOT_REPORTABLE:
		return TCM_INVALID_TOKEN_OP_AND_CAUSE_NOT_REPORTABLE;
	case RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_CREATION_NOT_SUPPORTED:
		return TCM_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_CREATION_NOT_SUPPORTED;
	case RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_USAGE_NOT_SUPPORTED:
		return TCM_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_USAGE_NOT_SUPPORTED;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_CANCELLED:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_CANCELLED;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_CORRUPT:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_CORRUPT;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_DELETED:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_DELETED;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_EXPIRED:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_EXPIRED;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_REVOKED:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_REVOKED;
	case RC_INVALID_TOKEN_OP_AND_TOKEN_UNKNOWN:
		return TCM_INVALID_TOKEN_OP_AND_TOKEN_UNKNOWN;
	case RC_INVALID_TOKEN_OP_AND_UNSUPPORTED_TOKEN_TYPE:
		return TCM_INVALID_TOKEN_OP_AND_UNSUPPORTED_TOKEN_TYPE;
	case RC_NO_SPACE_WRITE_PROTECT:
		return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
	case RC_OUT_OF_RESOURCES:
		return TCM_OUT_OF_RESOURCES;
	case RC_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED:
		return TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;
	case RC_CAPACITY_DATA_HAS_CHANGED:
		return TCM_CAPACITY_DATA_HAS_CHANGED;
	case RC_NON_EXISTENT_LUN:
		return TCM_NON_EXISTENT_LUN;
	case RC_REPORTED_LUNS_DATA_HAS_CHANGED:
		return TCM_REPORTED_LUNS_DATA_HAS_CHANGED;
	case RC_LOGICAL_UNIT_COMMUNICATION_FAILURE:
	default:
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		break;
	}

	return ret;

}

int qnap_transport_create_devinfo(
	struct se_cmd *cmd, 
	struct __dev_info *dev_info
	)
{
	struct se_device *se_dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct se_portal_group *se_tpg;
	struct fd_dev *__fd_dev;
	struct iblock_dev *__ib_dev;
	struct __fe_info *fe_info = &dev_info->fe_info;

	if (!cmd->se_lun)
		return -ENODEV;

	if(!qlib_is_fio_blk_dev(dr) && (se_dev->transport->get_dev)) {
		__fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
		fe_info->__dev.__fd.fd_file = __fd_dev->fd_file;
	} 
	else if(!qlib_is_ib_fbdisk_dev(dr) && (se_dev->transport->get_dev)) {
		__ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		fe_info->__dev.__bd.bd = __ib_dev->ibd_bd;
		fe_info->__dev.__bd.bio_set = __ib_dev->ibd_bio_set;
	} else
		return -ENODEV;

	se_tpg = cmd->se_lun->lun_tpg;

	dev_info->fe_info.is_thin = qlib_thin_lun(dr);
	dev_info->fe_info.fe_type = se_dev->dev_dr.dev_type;
	dev_info->bs_order = ilog2(se_dev->dev_attrib.block_size);
	dev_info->dev_max_lba = se_dev->transport->get_blocks(se_dev);
	dev_info->sbc_dev_type = se_dev->transport->get_device_type(se_dev);

	/* Refer the target_emulate_evpd_83() to crete initiatior port
	 * identifier field value 
	 */
	dev_info->initiator_rtpi = cmd->se_lun->lun_rtpi;
	dev_info->initiator_prot_id = se_tpg->proto_id;

	dr = &se_dev->dev_dr;
	qlib_get_naa_6h_code((void *)se_dev, &dr->dev_naa[0], &dev_info->naa[0],
		qnap_transport_parse_naa_6h_vendor_specific);
		
	if (cmd->se_dev->dev_attrib.emulate_fua_write)
		dev_info->dev_attr |= DEV_ATTR_SUPPORT_FUA_WRITE;

	if (cmd->se_dev->dev_attrib.emulate_write_cache)
		dev_info->dev_attr |= DEV_ATTR_SUPPORT_WRITE_CACHE;

	if (dev_info->fe_info.is_thin == true) {

		if (cmd->se_dev->dev_attrib.emulate_tpu)
			dev_info->dev_attr |= DEV_ATTR_SUPPORT_UNMAP;

		/* TBD */
		if (cmd->se_dev->dev_attrib.emulate_tpws)
			dev_info->dev_attr |= DEV_ATTR_SUPPORT_WRITE_SAME;

		/* support read zero after unmap, it needs by windows certification testing */
		if (dev_info->dev_attr & 
				(DEV_ATTR_SUPPORT_UNMAP | DEV_ATTR_SUPPORT_WRITE_SAME))
			dev_info->dev_attr |= DEV_ATTR_SUPPORT_READ_ZERO_UNMAP;
	}

#ifdef SUPPORT_TPC_CMD
#ifdef SUPPORT_FAST_BLOCK_CLONE
	if (se_dev->dev_dr.fast_blk_clone)
		dev_info->dev_attr |= DEV_ATTR_SUPPORT_DM_FBC;

	if (se_dev->dev_dr.fbc_control)
		dev_info->dev_attr |= DEV_ATTR_SUPPORT_DEV_FBC;
#endif
#endif
	return 0;
}

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
		pr_info("%s %p ITT: 0x%08x, i_state: %d, t_state: %d, "
			"transport_state: 0x%x, filebasd: %s, thin: %s, "
			"cdb[0]:0x%x, cdb[1]:0x%x, bs_state:0x%llx(0x%llx), ptime:%ld(s)\n", 
			prefix_str, se_cmd, se_cmd->se_tfo->get_task_tag(se_cmd),
			se_cmd->se_tfo->get_cmd_state(se_cmd), se_cmd->t_state,
			se_cmd->transport_state, 
			((!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)) ? "yes" : \
				((!qlib_is_fio_blk_dev(&se_dev->dev_dr)) ? \
				"no" : "unknown")),
			(qlib_thin_lun(&se_dev->dev_dr) ? "yes" : "no"),
			se_cmd->t_task_cdb[0], se_cmd->t_task_cdb[1],
			se_cmd->b_se_state, se_cmd->b_se_state_last,
			((jiffies - se_cmd->start_jiffies) / HZ)
			);

	}

}

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

bool qnap_transport_check_iscsi_fabric(
	struct se_cmd *se_cmd
	)
{
	return ((!strcmp(se_cmd->se_tfo->get_fabric_name(),"iSCSI")) ? true: false);
}
EXPORT_SYMBOL(qnap_transport_check_iscsi_fabric);


