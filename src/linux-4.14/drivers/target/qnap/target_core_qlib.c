/*******************************************************************************
 * Filename:  target_core_qlib.c
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
#include <linux/blkdev.h>
#include <scsi/scsi.h>
#include <asm/unaligned.h>
#include <qnap/iscsi_priv.h>
#include <qnap/fbdisk.h>
#include <target/qnap/qnap_target_struct.h>
#include "target_core_qlib.h"
#ifdef SUPPORT_FAST_BLOCK_CLONE
#include "target_core_qfbc.h"
#endif

/**/
struct kmem_cache *iorec_cache = NULL;	/* io rec */

/* max # of bios to submit at a time, please refer the target_core_iblock.c */
#define BLOCK_MAX_BIO_PER_TASK		32

/**/
/* This function was referred from blkdev_issue_discard() */
static void __qlib_bio_batch_end_io(struct bio *bio);
static int  __qlib_submit_bio_wait(struct bio_list *bio_lists);
static inline void __qlib_pop_put_bio(struct bio_list *biolist);
static inline void __qlib_free_io_rec_by_list(struct list_head *io_rec_list);
static inline sector_t __qlib_get_done_blks_by_list(struct list_head *io_rec_list);
static void __qlib_mybio_end_io(struct bio *bio);
static struct bio *__qlib_get_one_mybio(struct ___bd *bd, void *priv, 
	sector_t block_lba, u32 sg_num, int op, int op_flags);


/**/
/* ture : is thin device
 * false: NOT thin device
 */
bool qlib_thin_lun(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (!strncasecmp(dev_dr->dev_provision, "thin", 
			sizeof(dev_dr->dev_provision)))
		return true;
	return false;
}
EXPORT_SYMBOL(qlib_thin_lun);

int qlib_is_fio_blk_dev(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (dev_dr->dev_type == QNAP_DT_FIO_BLK)
		return 0;
	return -ENODEV;
}
EXPORT_SYMBOL(qlib_is_fio_blk_dev);

int qlib_is_ib_fbdisk_dev(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (dev_dr->dev_type == QNAP_DT_IBLK_FBDISK)
		return 0;
	return -ENODEV;
}
EXPORT_SYMBOL(qlib_is_ib_fbdisk_dev);

/* BUG 29894: copy from target module in kernel 2.6.33.2 
 * This will be used to create NAA for old firmware. And,
 * it is also used for compatible upgrade case
 */
static unsigned char __qlib_asciihex_to_binaryhex(
	unsigned char val[2]
	)
{
	unsigned char result = 0;
	/*
	 * MSB
	 */
	if ((val[0] >= 'a') && (val[0] <= 'f'))
		result = ((val[0] - 'a' + 10) & 0xf) << 4;
	else {
		if ((val[0] >= 'A') && (val[0] <= 'F'))
			result = ((val[0] - 'A' + 10) & 0xf) << 4;
		else /* digit */
			result = ((val[0] - '0') & 0xf) << 4;
	}
	/*
	 * LSB
	 */
	if ((val[1] >= 'a') && (val[1] <= 'f'))
		result |= ((val[1] - 'a' + 10) & 0xf);
	else {
		if ((val[1] >= 'A') && (val[1] <= 'F'))
			result |= ((val[1] - 'A' + 10) & 0xf);
		else /* digit */
			result |= ((val[1] - '0') & 0xf);
	}

	return result;
}

void qlib_make_naa_6h_hdr_old_style(
	unsigned char *buf
	)
{
	u8 off = 0;

	/* Start NAA IEEE Registered Extended Identifier/Designator */
	buf[off++] = (0x6 << 4);
	
	/* Use OpenFabrics IEEE Company ID: 00 14 05 */
	buf[off++] = 0x01;
	buf[off++] = 0x40;
	buf[off] = (0x5 << 4);
	return;
}

void qlib_make_naa_6h_body_old_style(
	unsigned char *wwn_sn_buf,
	unsigned char *out_buf
	)
{
	u8 binary = 0, binary_new =0 , off= 0, i = 0;

	binary = __qlib_asciihex_to_binaryhex(&wwn_sn_buf[0]);

	out_buf[off++] |= (binary & 0xf0) >> 4;

	for (i = 0; i < 24; i += 2) {
	    binary_new = __qlib_asciihex_to_binaryhex(&wwn_sn_buf[i+2]);
	    out_buf[off] = (binary & 0x0f) << 4;
	    out_buf[off++] |= (binary_new & 0xf0) >> 4;
	    binary = binary_new;
	}
	return;
}

void qlib_make_naa_6h_hdr_new_style(
	unsigned char *buf
	)
{
	u8 off = 0;

	buf[off++] = (0x6 << 4)| 0x0e;
	
	/* Use QNAP IEEE Company ID: */
	buf[off++] = 0x84;
	buf[off++] = 0x3b;
	buf[off] = (0x6 << 4);
	return;
}

int qlib_get_naa_6h_code(
	void *se_dev,
	unsigned char *dev_naa_sign,
	unsigned char *buf,
	void (*lio_spc_parse_naa_6h)(void *, u8 *)
	)
{
	if (!se_dev || !dev_naa_sign || !buf || !lio_spc_parse_naa_6h)
		return -EINVAL;

	/* BUG 29894
	 * We have three dev_naa type: (1) legacy (2) 3.8.1 and
	 * (3) qnap. For compatible issue, we shall use old type method
	 * to create naa body when naa hdr is qnap (new style)
	 * or legacy (old style). For others, we go new style to create
	 * naa body
	 */

	/* spc4r37a, p758, table 604
	 * buf will point to byte0 of NAA IEEE Registered Extended Designator 
	 */
	if(!strcmp(dev_naa_sign, "qnap")) {
		pr_debug("%s: NAA with QNAP IEEE company ID.\n", __func__);
		qlib_make_naa_6h_hdr_new_style(buf);

	} else {
		pr_debug("%s: invalid dev_naa value, try use NAA with "
			"OpenFabrics IEEE company ID.\n", __func__);		     
		qlib_make_naa_6h_hdr_old_style(buf);
	}

	if (lio_spc_parse_naa_6h)	 
		lio_spc_parse_naa_6h(se_dev, &buf[3]);
	return 0;
}

struct scatterlist *qlib_alloc_sg_list(
	u32 *data_size,
	u32 *sg_nent
	)
{
	struct page *page = NULL;
	struct scatterlist *sgl = NULL, *tmp_sgl = NULL;
	int nents, i, real_nents = 0;
	u32 alloc_size = 0, real_alloc_size = 0, tmp_alloc_size = 0, buf_size = 0;

	if (!data_size || !sg_nent)
		return NULL;

	tmp_alloc_size = alloc_size = 
		((*data_size >= MAX_SG_LISTS_ALLOC_SIZE) ? \
			MAX_SG_LISTS_ALLOC_SIZE : *data_size);

	nents = DIV_ROUND_UP(alloc_size, PAGE_SIZE);

	tmp_sgl = kzalloc(sizeof(struct scatterlist) * nents, GFP_KERNEL);
	if (!tmp_sgl)
		return NULL;

	/* prepare tmp sg lists */
	sg_init_table(tmp_sgl, nents);

	while (tmp_alloc_size) {
		page = alloc_page((GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN));
		if (!page)
			break;

		buf_size = min_t(u32, tmp_alloc_size, PAGE_SIZE);
		sg_set_page(&tmp_sgl[real_nents++], page, buf_size, 0);
		tmp_alloc_size -= buf_size;
	}

	if (real_nents == nents) {
		*data_size = alloc_size;
		*sg_nent = nents;
		pr_debug("%s: done to alloc sg lists. alloc_size(0x%x), "
			"nents(0x%x)\n", __func__, alloc_size, nents);
		return tmp_sgl;
	}

	/* we may be fail to alloc page ... so to prepare real sg list again */
	sgl = kzalloc(sizeof(struct scatterlist) * real_nents, GFP_KERNEL);
	if (!sgl)
		goto out;

	sg_init_table(sgl, real_nents);

	for (i = 0; i < real_nents; i++) {
		sg_set_page(&sgl[i], sg_page(&tmp_sgl[i]), 
			tmp_sgl[i].length, tmp_sgl[i].offset);

		real_alloc_size += tmp_sgl[i].length;
	}

	kfree(tmp_sgl);

	pr_warn("%s: re-alloc sg lists. alloc_size(0x%x), "
		"real_nents(0x%x)\n", __func__, real_alloc_size, real_nents);

	*data_size = real_alloc_size;
	*sg_nent = real_nents;
	return sgl;

out:
	for (i = 0; i < real_nents; i++)
		__free_page(sg_page(&tmp_sgl[i]));

	kfree(tmp_sgl);
	return NULL;

}

void qlib_free_sg_list(
	struct scatterlist *sg_list,
	u32 sg_nent
	)
{
	int i = 0;

	if (!sg_list || !sg_nent)
		return;

	for (i = 0; i < sg_nent; i++)
		__free_page(sg_page(&sg_list[i]));

	kfree(sg_list);
	return;
}

void qlib_create_aligned_range_desc(
	struct ____align_desc_blk *desc,
	sector_t lba,
	sector_t nr_blks,
	u32 bs_order,
	u32 aligned_size
	)
{
	u32 aligned_size_order;
	u64 total_bytes, s_pos_bytes, e_pos_bytes;
	sector_t align_lba, align_blks;

	desc->body.lba = lba;
	desc->body.nr_blks = nr_blks;
	desc->bs_order = bs_order;
	desc->bytes_to_align = aligned_size;
	desc->is_aligned = false;

	pr_debug("%s: lba:0x%llx, nr_blks:0x%x, aligned_size:%d, "
		"bs_order:%d\n", __func__, (unsigned long long)lba,
		nr_blks, aligned_size, bs_order);

	total_bytes = (u64)(nr_blks << bs_order);
	if (total_bytes < (u64)aligned_size)
		return;

	aligned_size_order = ilog2(aligned_size);

	/* convert to byte unit first */
	s_pos_bytes = lba << bs_order;
	e_pos_bytes = s_pos_bytes + total_bytes - (1 << bs_order);

	pr_debug("%s: s_pos_bytes:0x%llx, e_pos_bytes:0x%llx, "
		"total_bytes:0x%llx\n", __func__, 
		(unsigned long long)s_pos_bytes, 
		(unsigned long long)e_pos_bytes, 
		(unsigned long long)total_bytes);

	/* get the new s_lba is aligned by aligned_size */
	s_pos_bytes =  
		(((s_pos_bytes + aligned_size - (1 << bs_order)) >> \
			aligned_size_order) << aligned_size_order);

	pr_debug("%s: new align s_pos_bytes:0x%llx\n", __func__,
		(unsigned long long)s_pos_bytes);
	
	if ((s_pos_bytes > e_pos_bytes)
	|| ((e_pos_bytes - s_pos_bytes + (1 << bs_order)) < (u64)aligned_size)
	)
		return;

	/* get how many bytes which is multiplied by aligned_size */
	total_bytes = 
		(((e_pos_bytes - s_pos_bytes + (1 << bs_order)) >> \
		aligned_size_order) << aligned_size_order);

	pr_debug("%s: new align total bytes:0x%llx\n", __func__, 
		(unsigned long long)total_bytes);
	
	/* convert to original unit finally. prepare the middle first then 
	 * is for head / tail
	 */
	desc->is_aligned = true;

	align_lba = (s_pos_bytes >> bs_order);
	align_blks = (total_bytes >> bs_order);

	if (align_lba == lba) {
		/* if we didn't align for head */
		desc->head_tail[0].lba = 0;
		desc->head_tail[0].nr_blks = 0;
	} else {
		desc->head_tail[0].lba = lba;
		desc->head_tail[0].nr_blks = (align_lba -1) - lba + 1;
	}

	desc->body.lba = align_lba;
	desc->body.nr_blks = align_blks;

	/* for tail */
	desc->head_tail[1].lba = desc->body.lba + desc->body.nr_blks; /* next lba */
	desc->head_tail[1].nr_blks = 
		nr_blks - desc->head_tail[0].nr_blks - desc->body.nr_blks;

	pr_debug("%s: (head) lba:0x%llx, blks:0x%llx\n", __func__, 
		(unsigned long long)desc->head_tail[0].lba, 
		desc->head_tail[0].nr_blks);
	pr_debug("%s: (body) lba:0x%llx, blks:0x%llx\n", __func__,
		(unsigned long long)desc->body.lba, desc->body.nr_blks);
	pr_debug("%s: (tail) lba:0x%llx, blks:0x%llx\n", __func__, 
		(unsigned long long)desc->head_tail[1].lba, 
		desc->head_tail[1].nr_blks);

	return;
}

int qlib_fileio_rw(
	struct __rw_task *task
	)
{
	struct __dev_info *dev_info = &task->dev_info;
	struct ___fd *__fd = &dev_info->fe_info.__dev.__fd;
	struct scatterlist *sg = NULL;
	struct iov_iter iter;
	struct bio_vec *bvec;

	sector_t dest_lba = task->lba;
	u64 len, tmp_total = 0, nr_bytes = task->nr_bytes;
	loff_t pos = 0, start = 0, end = 0;
	int ret = -EINVAL, i = 0, done_blks = 0, sync_ret;

	bvec = kcalloc(task->sg_nents, sizeof(struct bio_vec), GFP_KERNEL);
	if (!bvec) {
		pr_err("Unable to allocate qlib_fileio_rw iov[]\n");
		task->ret = -ENOMEM;
		return 0;
	}

	for_each_sg(task->sg_list, sg, task->sg_nents, i) {

		len = min_t(u64, nr_bytes, sg->length);
		bvec[i].bv_page = sg_page(sg);
		bvec[i].bv_offset = sg->offset;
		bvec[i].bv_len = len;

		tmp_total += len;
		nr_bytes -= len;
	}

	WARN_ON(nr_bytes);

	pos = ((u64)dest_lba << task->bs_order);

	iov_iter_bvec(&iter, ITER_BVEC, bvec, task->sg_nents, tmp_total);

	if (task->dir == DMA_TO_DEVICE)
		ret = vfs_iter_write(__fd->fd_file, &iter, &pos, 0);
	else
		ret = vfs_iter_read(__fd->fd_file, &iter, &pos, 0);

	if (ret <= 0)
		task->ret = -EIO;

	done_blks += (ret >> task->bs_order); 

	if (tmp_total != (u64)ret)
		task->ret = -EIO;

	if ((task->dir == DMA_TO_DEVICE) && (done_blks > 0)
	&& (dev_info->dev_attr & DEV_ATTR_SUPPORT_WRITE_CACHE)
	&& (dev_info->dev_attr & DEV_ATTR_SUPPORT_FUA_WRITE)
	)
	{
		start = (task->lba << task->bs_order);
		end = start + ((sector_t)done_blks << task->bs_order) - 1;
	
		sync_ret = vfs_fsync_range(__fd->fd_file, start, end, 1);
		if (sync_ret != 0) {
			task->ret = sync_ret;
			pr_err("[%s] write w/ FUA is failed: %d\n", 
				__func__, sync_ret);
		}
	}

	kfree(bvec);

	return done_blks;
}

int qlib_create_iorec_cache(void)
{
	iorec_cache = kmem_cache_create("iorec_cache",  
			sizeof(struct __io_rec), 
			__alignof__(struct __io_rec), 0, NULL);

	if (!iorec_cache)
		return -ENOMEM;

	return 0;
}

void qlib_destroy_iorec_cache(void)
{
	if (iorec_cache)
		kmem_cache_destroy(iorec_cache);
}

static void __qlib_bio_batch_end_io(struct bio *bio)
{
	struct __cb_data *p = bio->bi_private;
	int err;

	err = blk_status_to_errno(bio->bi_status);
	if (err != 0) {
		pr_err("%s: bio error: %d\n", __func__, err);

		if (err == -ENOSPC)
			p->nospc_err = 1;

		atomic_inc(&p->bio_err_count);
		smp_mb__after_atomic();
	}

	bio_put(bio);

	if (atomic_dec_and_test(&p->bio_count))
		complete(p->wait);

}

void qlib_init_cb_data(
	struct __cb_data *data,
	void *p
	)
{
	data->wait = p;
	data->nospc_err = 0;
	atomic_set(&data->bio_count, 1);
	atomic_set(&data->bio_err_count, 0);
	return;
}

static int  __qlib_submit_bio_wait(
	struct bio_list *bio_lists
	)
{
#define D4_TIME2WAIT  10

	DECLARE_COMPLETION_ONSTACK(wait);
	struct __io_rec *rec = NULL;
	struct __cb_data cb_data;
	struct bio *mybio = NULL;
	struct blk_plug plug;
	unsigned long t;

	if (!bio_lists)
		BUG_ON(1);

	t = msecs_to_jiffies(D4_TIME2WAIT * 1000);

	qlib_init_cb_data(&cb_data, &wait);

	blk_start_plug(&plug);
	while (1) {
		mybio = bio_list_pop(bio_lists);
		if (!mybio)
			break;

		rec = (struct __io_rec *)mybio->bi_private;

		pr_debug("%s: bio bpf(0x%x), bio lba(0x%llx), bio len(0x%llx), "
			"rec br_blks(0x%x)\n", __func__, mybio->bi_opf,
			(unsigned long long)mybio->bi_iter.bi_sector, 
			(unsigned long long)mybio->bi_iter.bi_size,
			rec->nr_blks);

		rec->cb_data = &cb_data;
		atomic_inc(&(cb_data.bio_count));
		submit_bio(mybio);
	}

	blk_finish_plug(&plug);

	if (!atomic_dec_and_test(&(cb_data.bio_count))) {
		while (wait_for_completion_timeout(&wait, t) == 0)
			pr_err("%s: wait bio to be done\n", __func__);
	}

	if (atomic_read(&cb_data.bio_err_count)) {
		if (cb_data.nospc_err)
			return -ENOSPC;
		else
			return -EIO;
	}
	return 0;
}

static inline void __qlib_pop_put_bio(
	struct bio_list *biolist
	)
{
	struct bio *bio = NULL;

	while ((bio = bio_list_pop(biolist)))
		bio_put(bio);
}

static inline void __qlib_free_io_rec_by_list(
	struct list_head *io_rec_list
	)
{
	struct __io_rec *rec = NULL, *tmp_rec = NULL;

	list_for_each_entry_safe(rec, tmp_rec, io_rec_list, node)
		kfree(rec);
}

static inline sector_t __qlib_get_done_blks_by_list(
	struct list_head *io_rec_list
	)
{
	struct __io_rec *rec;
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

static void __qlib_mybio_end_io(
	struct bio *bio
	)
{
	struct __cb_data *p = NULL;
	struct __io_rec *rec = NULL;
	int err;

	rec = (struct __io_rec *)bio->bi_private;
	p = rec->cb_data;
	rec->transfer_done = 1;

	err = blk_status_to_errno(bio->bi_status);
	if (err != 0) {
		pr_err("%s: bio error: %d\n", __func__, err);

		if (err == -ENOSPC)
			p->nospc_err = 1;

		rec->transfer_done = -1; // treat it as error
		atomic_inc(&p->bio_err_count);
		smp_mb__after_atomic();
	}

	bio_put(bio);

	if (atomic_dec_and_test(&p->bio_count))
		complete(p->wait);

}

static struct bio *__qlib_get_one_mybio(
	struct ___bd *bd,
	void *priv, 
	sector_t block_lba,
	u32 sg_num,
	int op,
	int op_flags	
	)
{
	struct bio *mybio = NULL;

	if (sg_num > BIO_MAX_PAGES)
		sg_num = BIO_MAX_PAGES;

	mybio = bio_alloc_bioset(GFP_NOIO, sg_num, bd->bio_set);
	if (!mybio) {
		pr_err("%s: unable to allocate mybio\n", __func__);
		return NULL;
	}

	bio_set_dev(mybio, bd->bd);
	mybio->bi_private = priv;
	mybio->bi_end_io = &__qlib_mybio_end_io;
	mybio->bi_iter.bi_sector = block_lba;
	bio_set_op_attrs(mybio, op, op_flags);

	pr_debug("%s - allocated bio: 0x%p, lba:0x%llx\n", __func__, 
		mybio, (unsigned long long)mybio->bi_iter.bi_sector);

	return mybio;
}


int qlib_blockio_rw(
	struct __rw_task *task
	)
{
	struct bio *mybio = NULL;
	struct __io_rec *rec = NULL;
	struct scatterlist *sg = NULL;
	struct bio_list bio_lists;
	struct list_head io_rec_list;
	struct __dev_info *dev_info = &task->dev_info;
	struct ___bd *bd = &dev_info->fe_info.__dev.__bd;
	sector_t bio_lba = 0;
	int i = 0, bio_cnt = 0, done_blks = 0, err, bio_op;
	u32 dest_bs_order = dev_info->bs_order, sg_num = task->sg_nents;
	u64 len = 0, expected_bytes = 0;

	/* task lba may be 4096b, it shall be converted again for linux block layer (512b) */	
	bio_lba = ((task->lba << dest_bs_order) >> 9);
	expected_bytes = task->nr_bytes;

	if (task->dir == DMA_BIDIRECTIONAL || task->dir == DMA_NONE) {
		task->ret = -EINVAL;	
		return 0;
	}

	if (!task->nr_bytes || !sg_num) {
		task->ret = -EINVAL;	
		return 0;
	}

	rec = kmem_cache_zalloc(iorec_cache, GFP_KERNEL);
	if (!rec) {
		task->ret = -ENOMEM;
		return 0;
	}

	bio_op = ((task->dir == DMA_TO_DEVICE) ? REQ_OP_WRITE : REQ_OP_READ);

	mybio = __qlib_get_one_mybio(bd, rec, bio_lba, sg_num, bio_op, 0);
	if (!mybio) {
		kmem_cache_free(iorec_cache, rec);
		task->ret = -ENOMEM;
		return 0;
	}

	/* prepare io rec for 1st bio, we still not insert sg page yet */
 	INIT_LIST_HEAD(&io_rec_list);
	INIT_LIST_HEAD(&rec->node);
	rec->cb_data = NULL;
	rec->nr_blks = 0;
	list_add_tail(&rec->node, &io_rec_list);

	bio_list_init(&bio_lists);
	bio_list_add(&bio_lists, mybio);
	bio_cnt = 1;

	for_each_sg(task->sg_list, sg, task->sg_nents, i) {

		len = min_t(u64, expected_bytes, sg->length);

		while (bio_add_page(mybio, sg_page(sg), len, sg->offset) != len)
		{
			/* 1st bio was fail to be inserted ... */
			if ((bio_list_size(&bio_lists) == 1) && (!rec->nr_blks))
				goto fail_put_bios;

			if (bio_cnt >= BLOCK_MAX_BIO_PER_TASK) {

				err = __qlib_submit_bio_wait(&bio_lists); 
				
				/* after to submit, we will do ... */
				done_blks += __qlib_get_done_blks_by_list(&io_rec_list);
				
				__qlib_pop_put_bio(&bio_lists);
				__qlib_free_io_rec_by_list(&io_rec_list);	
				
				pr_debug("%s: done blks(0x%x)\n", __func__, done_blks);
				
				if (err < 0) {
					pr_warn("%s: done blks(0x%x), err:%d "
					"for (bio_cnt >= BLOCK_MAX_BIO_PER_TASK) "
					"case\n", __func__, done_blks, err);
					task->ret = err;
					return done_blks;
				}

				bio_cnt = 0;
			}

			/* prepare new bio */
			rec = kmem_cache_zalloc(iorec_cache, GFP_KERNEL);
			if (!rec)
				goto fail_put_bios;

			mybio = __qlib_get_one_mybio(bd, rec, bio_lba, sg_num, 
					bio_op, 0);
			if (!mybio) {
				kmem_cache_free(iorec_cache, rec);
				goto fail_put_bios;
			}

			INIT_LIST_HEAD(&rec->node);
			rec->cb_data = NULL;
			rec->nr_blks = 0;
			list_add_tail(&rec->node, &io_rec_list);

			bio_list_add(&bio_lists, mybio);
			bio_cnt++;
		}

		bio_lba += len >> 9;

		/* this size is for real destination side */
		rec->nr_blks += (len >> dest_bs_order);
		expected_bytes -= len;
		sg_num--;
	}

	err = __qlib_submit_bio_wait(&bio_lists);

	/* after to submit, we will do ... */
	done_blks += __qlib_get_done_blks_by_list(&io_rec_list);
	
	__qlib_pop_put_bio(&bio_lists);
	__qlib_free_io_rec_by_list(&io_rec_list);

	pr_debug("%s: done blks(0x%x)\n", __func__, done_blks);

	WARN_ON(expected_bytes);

	if (err < 0) {
		pr_debug("%s: done blks(0x%x), err:%d\n", __func__, done_blks, err);
		task->ret = err;
	}

	return done_blks;

fail_put_bios:
	__qlib_pop_put_bio(&bio_lists);
	__qlib_free_io_rec_by_list(&io_rec_list);
	task->ret = -EIO;
	return 0;

}

bool qlib_check_support_special_discard(void)
{
	/* we declare it to be weak attribute in iscsi layer */
	if (!blkdev_issue_special_discard) {
		pr_warn("blk-lib.c NOT support blkdev_issue_special_discard(), "
			"please check it\n");
		return false;
	}
	return true;
}
EXPORT_SYMBOL(qlib_check_support_special_discard);

int qlib_fd_sync_cache_range(
	struct file *file,
	loff_t start_byte,
	loff_t end_byte	
	)
{
	int err_1, err_msg;

	err_msg = 1;
	err_1 = filemap_fdatawrite_range(
		file->f_mapping, start_byte, end_byte);

	if (unlikely(err_1 != 0))
		goto _err_;

	err_msg = 2;
	err_1 = filemap_fdatawait_range(
		file->f_mapping, start_byte, end_byte);

	if (unlikely(err_1 != 0))
		goto _err_;

	return 0;
_err_:
	pr_debug("%s: %s is failed: %d\n", __func__, 
		((err_msg == 1) ? "filemap_fdatawrite_range": \
		"filemap_fdatawait_range"), err_1);

	return err_1;
}

int qlib_fd_flush_and_truncate_cache(
	struct file *fd,
	sector_t lba,
	u32 nr_blks,
	u32 bs_order,
	bool truncate_cache,
	bool is_thin
	)
{
	struct inode *inode = NULL;
	struct address_space *mapping = NULL;
	loff_t first_page = 0, last_page = 0, start = 0, len = 0;
	loff_t first_page_offset = 0, last_page_offset = 0;
	int ret = 0;

	inode = fd->f_mapping->host;
	mapping = inode->i_mapping;

	/* convert to byte-unit */
	start = (loff_t)(lba << bs_order);
	len = (loff_t)((loff_t)nr_blks << bs_order);

	first_page = (start) >> PAGE_SHIFT;
	last_page = (start + len) >> PAGE_SHIFT;
	first_page_offset = first_page	<< PAGE_SHIFT;
	last_page_offset = (last_page << PAGE_SHIFT) + \
			((PAGE_SIZE - 1));

	pr_debug("%s: lba(0x%llx), nr_blks(0x%x), bs_order(0x%x), "
		"start(0x%llx), len(0x%llx), first_page(0x%llx), "
		"last_page(0x%llx), first_page_offset(0x%llx), "
		"last_page_offset(0x%llx)\n", __func__, 
		(unsigned long long)lba, (unsigned long long)nr_blks, bs_order,
		(unsigned long long)start, (unsigned long long)len,
		(unsigned long long)first_page, (unsigned long long)last_page,
		(unsigned long long)first_page_offset, 
		(unsigned long long)last_page_offset);

	if (mapping->nrpages 
	&& mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
	{
		ret = filemap_write_and_wait_range(mapping, 
			first_page_offset, last_page_offset);

		if (unlikely(ret)){
			pr_err("%s: fail from filemap_write_and_wait_range(), "
				"ret:%d\n", __func__, ret);
#ifdef SUPPORT_TP
			if (!is_thin)
				return ret;

			int err;

			if (ret != -ENOSPC) {
				err = qlib_fd_check_dm_thin_cond(fd);
				if (err == -ENOSPC)
					ret = err;
			}
#endif
			return ret;
		}
	}

	if (truncate_cache)
		truncate_pagecache_range(inode, first_page_offset, 
			last_page_offset);

	return 0;
}

/* 0: normal i/o (not hit sync i/o threshold)
 * 1: hit sync i/o threshold
 * -ENOSPC: pool space is full
 * -ENODEV: no such device
 * -others: other failure
 *
 * this call shall cowork with dm-thin layer
 */
int qlib_fd_check_dm_thin_cond(
	struct file *file
	)
{
	struct inode *inode = NULL;
	struct request_queue *q = NULL; 

	if (!file)
		return -ENODEV;

	inode = file->f_mapping->host;
	if (!S_ISBLK(inode->i_mode))
		return -ENODEV;

	/* here is fio + block backend */
	if (!inode->i_bdev)
		return -ENODEV;

	q = bdev_get_queue(inode->i_bdev);
	if (!q)
		return -ENODEV; 

	/* check these functions exists or not since it was declared by
	 * weak symbol in target_core_qlib.h
	 */
	if (!dm_thin_volume_is_full)
		return -EINVAL;

	if (!qlib_rq_get_thin_hook(q))
		return -EINVAL;
	
	return dm_thin_volume_is_full(qlib_rq_get_thin_hook(q));

}
EXPORT_SYMBOL(qlib_fd_check_dm_thin_cond);

int qlib_vfs_fsync_range(
	struct file *file,
	loff_t s,
	loff_t e,
	int data_sync,
	int is_thin
	)
{
	int ret;
	
	ret = vfs_fsync_range(file, s, e, data_sync);
	if (ret == 0)
		return 0;
	
	/* if fail to sync, try check this is thin or thick lun */
	if (!is_thin)
		return ret;

#ifdef SUPPORT_TP
	/* if this is thin lun, now to ... */
	ret = qlib_fd_check_dm_thin_cond(file);
	if (ret != -ENOSPC)
		return ret;
	
	pr_warn("%s: space was full already\n", __func__);
	return -ENOSPC;
#endif

}
EXPORT_SYMBOL(qlib_vfs_fsync_range);

/* qlib_drop_bb_xxx() is for i/o dropping feature code for block based lun */
void qlib_drop_bb_init_rec_val(
	struct __iov_obj *obj
	)
{
	spin_lock_init(&obj->iov_rec_lock);
	obj->iov_rec = NULL;
	obj->iov_drop = false;
	obj->iov_len = 0;
}

void qlib_drop_bb_prepare_rec(
	struct qnap_se_dev_dr *dev_dr,
	struct __iov_obj *obj,
	void *iter,
	u32 len
	)
{
	if (qlib_is_fio_blk_dev(dev_dr) != 0)
		return;

	spin_lock(&obj->iov_rec_lock);
	obj->iov_rec = iter;
	obj->iov_len = len;
	spin_unlock(&obj->iov_rec_lock);
}
EXPORT_SYMBOL(qlib_drop_bb_prepare_rec);

void qlib_drop_bb_set_rec_null(
	struct qnap_se_dev_dr *dev_dr,
	struct __iov_obj *obj
	)
{
	if (qlib_is_fio_blk_dev(dev_dr) != 0)
		return;

	spin_lock(&obj->iov_rec_lock);
	obj->iov_rec = NULL;
	spin_unlock(&obj->iov_rec_lock);
}
EXPORT_SYMBOL(qlib_drop_bb_set_rec_null);

void qlib_drop_bb_set_drop_val(
	struct qnap_se_dev_dr *dev_dr,
	struct __iov_obj *obj,
	bool val
	)
{
	if (qlib_is_fio_blk_dev(dev_dr) != 0)
		return;

	obj->iov_drop = val;
}
EXPORT_SYMBOL(qlib_drop_bb_set_drop_val);

bool qlib_drop_bb_get_drop_val(
	struct qnap_se_dev_dr *dev_dr,
	struct __iov_obj *obj
	)
{
	if (qlib_is_fio_blk_dev(dev_dr) != 0)
		return false;

	return obj->iov_drop;
}
EXPORT_SYMBOL(qlib_drop_bb_get_drop_val);

int qlib_drop_bb_ask_drop(
	struct qnap_se_dev_dr *dev_dr,
	struct __iov_obj *obj,
	char *msg
	)
{
	if (qlib_is_fio_blk_dev(dev_dr) != 0)
		return -ENODEV;

	spin_lock(&obj->iov_rec_lock);
	
	if (obj->iov_rec) {
		/* if found obj->iov_rec, it means cmd was submitted
		 * in fd_do_rw() already but may not back yet
		 */
		if (msg)
			sprintf(msg, "iov:0x%p, len:0x%x", obj->iov_rec, obj->iov_len);

		qnap_iscsi_iov_set_drop((struct iov_iter *)obj->iov_rec);
	}

	spin_unlock(&obj->iov_rec_lock);
	return 0;

}

/* qlib_drop_fb_xxx() is for i/o dropping feature code for file based lun */
int qlib_drop_fb_alloc_bio_rec(
	struct qnap_se_dev_dr *dev_dr,
	struct __bio_obj *obj,
	void *misc,
	struct bio *bio
	)
{
	struct bio_rec *brec = NULL;

	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return -ENODEV;

	if (dev_dr->fb_bio_rec_kmem) {
		brec = kmem_cache_zalloc(dev_dr->fb_bio_rec_kmem, GFP_KERNEL);
		if (brec) {
			INIT_LIST_HEAD(&brec->node);
			brec->bio = bio;
			brec->misc = misc;
			brec->flags = 0;
			bio->bi_private = qnap_bi_private_set_brec_bit((void *)brec);

			spin_lock(&obj->bio_rec_lists_lock);
			list_add_tail(&brec->node, &obj->bio_rec_lists);
			atomic_inc(&obj->bio_rec_count);
			spin_unlock(&obj->bio_rec_lists_lock);
			return 0;
		}
	}
	return -ENOMEM;
}
EXPORT_SYMBOL(qlib_drop_fb_alloc_bio_rec);

void qlib_drop_fb_init_bio_rec_val(
	struct __bio_obj *obj
	)
{
	spin_lock_init(&obj->bio_rec_lists_lock);
	INIT_LIST_HEAD(&obj->bio_rec_lists);
	atomic_set(&obj->bio_rec_count, 1);
}

void qlib_drop_fb_destroy_fb_bio_rec_kmem(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return;

	if (dev_dr->fb_bio_rec_kmem)
		kmem_cache_destroy(dev_dr->fb_bio_rec_kmem);
}

void qlib_drop_fb_set_bio_rec_null(
	struct qnap_se_dev_dr *dev_dr,
	struct __bio_obj *obj,
	struct bio *bio
	)
{
	struct bio_rec *brec, *tmp_brec;

	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return;

	/* TODO: any smart method ? */
	spin_lock(&obj->bio_rec_lists_lock);
	list_for_each_entry_safe(brec, tmp_brec, &obj->bio_rec_lists, node) {
		if (brec->bio == bio)
			brec->bio = NULL;
	}
	spin_unlock(&obj->bio_rec_lists_lock);

}
EXPORT_SYMBOL(qlib_drop_fb_set_bio_rec_null);

void qlib_drop_fb_create_fb_bio_rec_kmem(
	struct qnap_se_dev_dr *dev_dr,
	int dev_index
	)
{
	char tmp_name[128];

	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return;

	/* only for iblock + fbdisk device */
	sprintf(tmp_name, "fb_bio_rec_cache-%d", dev_index);

	dev_dr->fb_bio_rec_kmem = kmem_cache_create(tmp_name,
			sizeof(struct bio_rec), 
			__alignof__(struct bio_rec), 
			0, NULL);

	if (!dev_dr->fb_bio_rec_kmem)
		pr_warn("fail to create fb_bio_rec_cache, idx: %d\n", dev_index);

}

static void qlib_drop_fb_free_bio_rec(
	struct qnap_se_dev_dr *dev_dr,
	struct bio_rec *rec
	)
{
	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return;

	if (dev_dr->fb_bio_rec_kmem && rec)
		kmem_cache_free(dev_dr->fb_bio_rec_kmem, rec);
}

int qlib_drop_fb_free_bio_rec_lists(
	struct qnap_se_dev_dr *dev_dr,
	struct __bio_obj *obj
	)
{
	struct bio_rec *brec = NULL, *tmp_bio_rec = NULL;
	LIST_HEAD(__free_list);

	if (qlib_is_ib_fbdisk_dev(dev_dr) != 0)
		return -ENODEV;

	spin_lock(&obj->bio_rec_lists_lock);
	
	pr_debug("bio rec count:%d\n", atomic_read(&obj->bio_rec_count));	

	list_for_each_entry_safe(brec, tmp_bio_rec, &obj->bio_rec_lists, 
		node)
	{
		list_move_tail(&brec->node, &__free_list);
	}
	spin_unlock(&obj->bio_rec_lists_lock);

	list_for_each_entry_safe(brec, tmp_bio_rec, &__free_list, node) {
		list_del_init(&brec->node);
		atomic_dec(&obj->bio_rec_count);
		qlib_drop_fb_free_bio_rec(dev_dr, brec);
	}

	return 0;
}
EXPORT_SYMBOL(qlib_drop_fb_free_bio_rec_lists);

void qlib_se_cmd_dr_init(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	INIT_LIST_HEAD(&cmd_dr->se_queue_node);

	qlib_drop_fb_init_bio_rec_val(&cmd_dr->bio_obj);
	qlib_drop_bb_init_rec_val(&cmd_dr->iov_obj);

	cmd_dr->cmd_t_state = 0;
	atomic_set(&cmd_dr->tmr_code, 0);
	atomic_set(&cmd_dr->tmr_resp_tas, 0);
	atomic_set(&cmd_dr->tmr_diff_it_nexus, 0);	
}

/* take care this, please refer qlib_fio_blkdev_get_thin_total_used_blks() */
static void __dm_monitor_callback_fn(
	void *priv, 
	int dmstate
	)
{
	struct qnap_se_dev_attr_dr *dev_attr_dr = NULL;

	/* to prevent thin-lun and thin-pool both be removed */
	if (priv == NULL)
		return;

	dev_attr_dr = (struct qnap_se_dev_attr_dr *)priv;

	if (dmstate == 1)
		dev_attr_dr->gti = NULL;
	return;
}

int qlib_ib_fbdisk_get_thin_total_used_blks(
	struct block_device *bd,
	loff_t *total_bytes,
	loff_t *used_blks
	)
{
	struct fbdisk_device *fb_dev = NULL;
	struct fbdisk_file *fb_file = NULL;
	struct inode *inode = NULL;
	int i;
	loff_t __total_bytes = 0, __used_blks = 0;

	*total_bytes = 0;
	*used_blks = 0;

	if (!bd->bd_disk)
		return -ENODEV;

	if (!bd->bd_disk->private_data)
		return -ENODEV;

#warning "TODO: Is it better to use ioctl() ?? "
	/* TODO */
	fb_dev = bd->bd_disk->private_data;

	for (i = 0; i < fb_dev->fb_file_num; i++) {
		fb_file = &fb_dev->fb_backing_files_ary[i];
		inode = fb_file->fb_backing_file->f_mapping->host;
		__total_bytes += inode->i_size;
		/* this unit is 512b */
		__used_blks += inode->i_blocks;
	}

	*total_bytes = __total_bytes;
	*used_blks = __used_blks;

	return 0;
}

int qlib_fio_blkdev_get_thin_total_used_blks(
	struct qnap_se_dev_attr_dr *dev_attr_dr,
	char *lv_name,
	u64 *total_blks,
	u64 *used_blks
	)
{
	u64 total = 0, used = 0;
	int ret;

	*total_blks = 0;
	*used_blks = 0;

	if (!lv_name)
		return -ENODEV;

	/* we declare it to be weak symbol ... */
	if (!thin_get_dmtarget || !thin_set_dm_monitor || !thin_get_data_status)
		return -EINVAL;

	if (!dev_attr_dr->gti) {
		/* try to get dm target and set the dm monitor */
		ret = thin_get_dmtarget(lv_name, (struct dm_target **)&dev_attr_dr->gti);
	} else
		ret = 0;

	if (ret != 0) {
		pr_warn("%s: fail to call thin_get_dmtarget()\n", __func__);
		return ret;
	}

	thin_set_dm_monitor((struct dm_target *)dev_attr_dr->gti, 
		dev_attr_dr, __dm_monitor_callback_fn);

	/* start get data status, the unit of total and used is 512 bytes */
	ret = thin_get_data_status(dev_attr_dr->gti, &total, &used);
	if (ret != 0) {
		pr_warn("%s: fail to call thin_get_data_status()\n", __func__);
		return ret;
	}

	*total_blks = total;
	*used_blks = used;

	return ret;

}

void qlib_check_exported_func(void)
{
	/* check any func exported by other layer is ready or not */

#ifdef SUPPORT_FAST_BLOCK_CLONE
	if (!thin_support_block_cloning)
		pr_warn("%s: [iSCSI Porting] thin_support_block_cloning() "
			"isn't prepared, please check it\n", __func__);

	if (!thin_do_block_cloning)
		pr_warn("%s: [iSCSI Porting] thin_do_block_cloning() "
			"isn't prepared, please check it\n", __func__);
#endif

#ifdef SUPPORT_TP
#ifdef QNAP_HAL
	if (!send_hal_netlink)
		pr_warn("%s: [iSCSI Porting] send_hal_netlink() "
			"isn't prepared, please check it\n", __func__);
#endif

	if (!thin_get_dmtarget)
		pr_warn("%s: [iSCSI Porting] thin_get_dmtarget() "
			"isn't prepared, please check it\n", __func__);

	if (!thin_get_data_status)
		pr_warn("%s: [iSCSI Porting] thin_get_data_status() "
			"isn't prepared, please check it\n", __func__);

	if (!thin_set_dm_monitor)
		pr_warn("%s: [iSCSI Porting] thin_set_dm_monitor() "
			"isn't prepared, please check it\n", __func__);

	if (!dm_thin_volume_is_full)
		pr_warn("%s: [iSCSI Porting] dm_thin_volume_is_full() "
			"isn't prepared, please check it\n", __func__);

	if (!thin_get_sectors_per_block)
		pr_warn("%s: [iSCSI Porting] thin_get_sectors_per_block() "
			"isn't prepared, please check it\n", __func__);
	
	if (!thin_get_lba_status)
		pr_warn("%s: [iSCSI Porting] thin_get_lba_status() "
			"isn't prepared, please check it\n", __func__);

	if (!qlib_rq_get_thin_hook)
		pr_warn("%s: [iSCSI Porting] rq_get_thin_hook() "
			"isn't prepared, please check it\n", __func__);
	
#endif

}

bool qlib_check_zero_buf(
	u8 *buf,
	u32 buf_len
	)
{
	struct page *zero_page = ZERO_PAGE(0);
	unsigned char *zero_buf = NULL;
	int ret = 0;
	u32 len;

	WARN_ON(!buf);
	WARN_ON(!buf_len);

	zero_buf = kmap(zero_page);
	if (!zero_buf)
		return false;

	while (buf_len) {
		len = min_t(u32, buf_len, PAGE_SIZE);
		ret = memcmp(buf, zero_buf, len);
		if (ret)
			break;
		buf_len -= len;
		buf += len;
	}

	kunmap(zero_page);		

	return ((!ret) ? true : false);	
}
