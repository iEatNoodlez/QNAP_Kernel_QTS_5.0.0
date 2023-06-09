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
#include "target_core_qfbc.h"

/**/
struct kmem_cache *iorec_cache = NULL;	/* io rec */

/* max # of bios to submit at a time, please refer the target_core_iblock.c */
#define BLOCK_MAX_BIO_PER_TASK		32

/**/
static inline void __qlib_ib_pop_put_bio(struct bio_list *biolist);
static inline void __qlib_ib_free_io_rec_by_list(struct list_head *io_rec_list);
static inline sector_t __qlib_ib_get_done_blks_by_list(struct list_head *io_rec_list);

struct bio_cb_data {
	void			*priv;
	atomic_t		bio_count;
	atomic_t		bio_err_count;
	int			err;
	int			nospc_err;
	struct completion	cb_wait;
	struct list_head	io_rec_list;

	/* called in __qlib_ib_bio_end_io() and depends on what you want to do */
	void (*complete)(struct bio_cb_data *cb_data);	

	/* called in void (*complete)(), and you may want to pass some information
	 * to the caller who calls qlib_blockio_rw() (for aio version)
	 */
	void (*complete2)(void *priv, int err, int done_blks);
};

struct bio_data {
	struct ___bd		*bd;
	struct bio_cb_data	*cb_data;
	struct bio_list 	bio_lists;
};

struct __io_rec {
	struct	list_head	node;
	u32			nr_blks;
	bool			transfer_done;
	struct bio_cb_data	*cb_data;	
};

/**/
static void __qlib_ib_free_io_rec(
	struct __io_rec *io_rec
	)
{
	if (io_rec)
		kmem_cache_free(iorec_cache, io_rec);
}

static struct __io_rec *__qlib_ib_alloc_io_rec(void)
{
	struct __io_rec *io_rec;

	io_rec = kmem_cache_zalloc(iorec_cache, GFP_KERNEL);
	if (io_rec) {
		INIT_LIST_HEAD(&io_rec->node);
		io_rec->cb_data = NULL;
		io_rec->nr_blks = 0;
		io_rec->transfer_done = false;	
	}
	return io_rec;
}


static void __qlib_init_bdata(
	struct bio_data *bdata
	)
{
	memset(bdata, 0, sizeof(struct bio_data));
	bio_list_init(&bdata->bio_lists);
}

static void __qlib_ib_bio_complete(
	struct bio_cb_data *cb_data
	)
{
	int done_blks;
	u8 status;

	if (!atomic_dec_and_test(&cb_data->bio_count))
		return;

	done_blks += __qlib_ib_get_done_blks_by_list(&cb_data->io_rec_list);
	__qlib_ib_free_io_rec_by_list(&cb_data->io_rec_list);	

	cb_data->complete2(cb_data->priv, cb_data->err, done_blks);
	kfree(cb_data);
}

static void __qlib_ib_bio_wait_complete(
	struct bio_cb_data *cb_data
	)
{
	if (atomic_dec_and_test(&cb_data->bio_count))
		complete(&cb_data->cb_wait);
}

static void __qlib_ib_bio_end_io(
	struct bio *bio
	)
{
	struct bio_cb_data *cb_data = NULL;
	struct __io_rec *rec = NULL;

	rec = (struct __io_rec *)bio->bi_private;
	cb_data = rec->cb_data;
	rec->transfer_done = 1;

	cb_data->err = blk_status_to_errno(bio->bi_status);
	if (cb_data->err) {
		pr_err("%s: bio error: %d\n", __func__, cb_data->err);
		rec->transfer_done = -1; // treat it as error
		atomic_inc(&cb_data->bio_err_count);
		smp_mb__after_atomic();
	}

	bio_put(bio);

	cb_data->complete(cb_data);
}

static void __qlib_ib_get_bio_op_flag(
	struct ___bd *bd,
	int dir,
	int task_flag,
	int *op,
	int *op_flags
	)
{
	if (dir == DMA_TO_DEVICE) {
		struct request_queue *q = bdev_get_queue(bd->bd);
		/*
		 * Force writethrough using REQ_FUA if a volatile write cache
		 * is not enabled, or if initiator set the Force Unit Access bit.
		 */
		*op = REQ_OP_WRITE;
		if (test_bit(QUEUE_FLAG_FUA, &q->queue_flags)) {
			if (task_flag & RW_TASK_TYPE_FUA)
				*op_flags = REQ_FUA;
			else if (!test_bit(QUEUE_FLAG_WC, &q->queue_flags))
				*op_flags = REQ_FUA;
		}
	} else {
		*op = REQ_OP_READ;
	}

}

static struct bio *__qlib_ib_get_bio(
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
	mybio->bi_end_io = &__qlib_ib_bio_end_io;
	mybio->bi_iter.bi_sector = block_lba;
	bio_set_op_attrs(mybio, op, op_flags);

	pr_debug("%s - allocated bio: 0x%p, lba:0x%llx\n", __func__, 
		mybio, (unsigned long long)mybio->bi_iter.bi_sector);

	return mybio;
}

static struct bio *__qlib_ib_get_bio_with_rec(
	struct bio_data *bdata,
	sector_t block_lba,
	u32 sg_num,
	int op,
	int op_flags
	)
{
	struct bio_cb_data *cb_data = bdata->cb_data;
	struct __io_rec *io_rec = NULL;
	struct bio *mybio = NULL;

	io_rec = __qlib_ib_alloc_io_rec();
	if (io_rec) {
		mybio = __qlib_ib_get_bio(bdata->bd, io_rec, block_lba, sg_num, 
						op, op_flags);
		if (mybio) {
			io_rec->cb_data = cb_data;
			list_add_tail(&io_rec->node, &cb_data->io_rec_list);			
		} else
			__qlib_ib_free_io_rec(io_rec);
	}
	return mybio;
}

static void __qlib_ib_free_bio_res(
	struct bio_data *bdata
	)
{
	struct bio_cb_data *cb_data = bdata->cb_data;

	__qlib_ib_pop_put_bio(&bdata->bio_lists);
	__qlib_ib_free_io_rec_by_list(&cb_data->io_rec_list);	
}

static void __qlib_ib_submit_bio(
	struct bio_data *bdata
	)
{
	struct __io_rec *rec = NULL;
	struct bio *mybio = NULL;
	struct bio_cb_data *cb_data;
	struct blk_plug plug;

	blk_start_plug(&plug);
	while (1) {
		mybio = bio_list_pop(&bdata->bio_lists);
		if (!mybio)
			break;
	
		rec = (struct __io_rec *)mybio->bi_private;
		cb_data = rec->cb_data;
	
		pr_debug("%s: bio bpf(0x%x), bio lba(0x%llx), bio len(0x%llx), "
			"rec br_blks(0x%x)\n", __func__, mybio->bi_opf,
			(unsigned long long)mybio->bi_iter.bi_sector, 
			(unsigned long long)mybio->bi_iter.bi_size,
			rec->nr_blks);
	
		atomic_inc(&cb_data->bio_count);
		submit_bio(mybio);
	}
	
	blk_finish_plug(&plug);
}

static inline void __qlib_ib_pop_put_bio(
	struct bio_list *biolist
	)
{
	struct bio *bio = NULL;

	while ((bio = bio_list_pop(biolist)))
		bio_put(bio);
}

static inline sector_t __qlib_ib_get_done_blks_by_list(
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

static inline void __qlib_ib_free_io_rec_by_list(
	struct list_head *io_rec_list
	)
{
	struct __io_rec *rec = NULL, *tmp_rec = NULL;

	list_for_each_entry_safe(rec, tmp_rec, io_rec_list, node)
		kfree(rec);
}

static inline void __qlib_ib_update_sg_info(
	struct sg_info *_sg_info,
	struct scatterlist *new_start_sg,
	u32 new_sg_nents,
	u32 remain_sg_len
	)
{
	_sg_info->sg = new_start_sg;
	
	if (remain_sg_len) {
		_sg_info->sg_len = remain_sg_len;
		_sg_info->sg_off = new_start_sg->offset + \
				new_start_sg->length - remain_sg_len;

		_sg_info->sg_old_off = new_start_sg->offset;		
	} else {
		_sg_info->sg_len = new_start_sg->length;
		_sg_info->sg_off = new_start_sg->offset;
		_sg_info->sg_old_off = new_start_sg->offset;		
	}

	_sg_info->sg_nents = new_sg_nents;

}

static void __qlib_ib_prep_sgl_rec(
	struct bio_sgl_rec *sgl_rec,
	struct sg_info *_sg_info,
	u32 total_len,
	u32 *remain_len
	)
{
	struct scatterlist *sgl, *sg;
	u32 sgl_nents, sg_len, sg_off, i, add_len, consume_len = total_len;
	int new_sg_idx = 0;

	sgl = _sg_info->sg;
	sgl_nents = _sg_info->sg_nents;

	for_each_sg(sgl, sg, sgl_nents, i) {
		if (_sg_info->sg == sg) {
			sg_len = _sg_info->sg_len;
			sg_off = _sg_info->sg_off;
		} else {
			sg_len = sg->length;
			sg_off = sg->offset;
		}

		if (consume_len >= sg_len)
			add_len = sg_len;
		else {
			/* we have remain len (still in same sg) */
			add_len = consume_len;
			*remain_len = sg_len - consume_len;			
		}

		sg_set_page(&sgl_rec->sgl[new_sg_idx++], sg_page(sg), add_len, sg_off);
		consume_len -= add_len;
		if (!consume_len)
			break;
	}
	sg_mark_end(&sgl_rec->sgl[new_sg_idx - 1]);

	/* remember it is 0-based and we increase by 1 at sg_set_page() */
	sgl_rec->sg_nents = new_sg_idx; 
	sgl_rec->total_len = total_len;
}

static void __qlib_ib_get_sgl_min_work_len(
	struct block_device *bd,
	struct sg_info *_sg_info,
	u32 *expected_size
	)
{
	struct request_queue *q = bdev_get_queue(bd);
	struct scatterlist *work_sgl, *sg;
	u32 data_len = 0, tmp_len = 0, work_size, work_sg_nents, i;

	/* we hope to get max i/o size per bio */
	work_size = min_t(u32, *expected_size, (queue_max_sectors(q) << 9));
	work_sgl = _sg_info->sg;
	work_sg_nents = _sg_info->sg_nents;

	for_each_sg(work_sgl, sg, work_sg_nents, i) {
		if (i >= qlib_ib_get_min_nr_vecs(q))
			break;

		tmp_len = ((_sg_info->sg == sg) ? _sg_info->sg_len : sg->length);

		pr_debug("data_len:0x%x, tmp_len:0x%x\n", data_len, tmp_len);

		if ((data_len + tmp_len) > work_size) {
			if (data_len == 0)
				data_len = work_size;
			break;
		}
#if 0
		if ((data_len + __len) >= (queue_max_sectors(q) << 9))
			break;
#endif
		data_len += tmp_len;

		if (data_len == work_size)
			break;
	}

	if (data_len < (1 << 9))
		tmp_len = data_len;
	else
		/* must be multipled by 512b */
		tmp_len = ((data_len >> 9) << 9);

	pr_debug("data_len:0x%x, final data len:0x%x, real sg nents:%d\n", 
				data_len, tmp_len, (i + 1));

	*expected_size = tmp_len;
}

static void __qlib_ib_setup_bio_sgl_rec(
	struct __rw_task *task,
	struct bio_sgl_rec *sgl_rec
	)
{
	struct __dev_info *dev_info = &task->dev_info;
	struct ___bd *bd = &dev_info->fe_info.__bd;
	struct sg_info _sg_info;

	_sg_info.sg = task->sg_list;
	_sg_info.sg_len = task->sg_list[0].length;
	_sg_info.sg_off = task->sg_list[0].offset;
	_sg_info.sg_nents = task->sg_nents;
	qlib_ib_setup_bio_sgl_rec(bd->bd, sgl_rec, &_sg_info, task->nr_bytes);
}

static void __qlib_init_bio_cb_data(
	struct bio_cb_data *data,
	void (*cb_complete)(struct bio_cb_data *)
	)
{
	data->complete = cb_complete;
	data->err = 0;
	data->nospc_err = 0;
	atomic_set(&data->bio_count, 1);
	atomic_set(&data->bio_err_count, 0);
	init_completion(&data->cb_wait);
	INIT_LIST_HEAD(&data->io_rec_list);
}

int qlib_bd_is_fbdisk_dev(
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
EXPORT_SYMBOL(qlib_bd_is_fbdisk_dev);

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

int qlib_is_fio_file_dev(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (dev_dr->dev_type == QNAP_DT_FIO_FILE)
		return 0;
	return -ENODEV;
}
EXPORT_SYMBOL(qlib_is_fio_file_dev);


int qlib_is_ib_blk_dev(
	struct qnap_se_dev_dr *dev_dr
	)
{
	if (dev_dr->dev_type == QNAP_DT_IBLK_BLK)
		return 0;
	return -ENODEV;
}
EXPORT_SYMBOL(qlib_is_ib_blk_dev);

char *qlib_get_dev_type_str(
	struct qnap_se_dev_dr *dev_dr
	)
{
	char *type;

	switch (dev_dr->dev_type) {
	case QNAP_DT_FIO_BLK:
		type = "fio_blk";
		break;
	case QNAP_DT_FIO_FILE:
		type = "fio_file";
		break;
	case QNAP_DT_IBLK_FBDISK:
		type = "iblk_fbdisk";
		break;
	case QNAP_DT_IBLK_BLK:
		type = "iblk_blk";
		break;
	case QNAP_DT_RBD:
		type = "rbd";
		break;
	case QNAP_DT_PSCSI:
		type = "pscsi";
		break;	
	default:
		type = "unknown";
		break;
	}

	return type;

}
EXPORT_SYMBOL(qlib_get_dev_type_str);

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

	pr_debug("%s: [head] lba:0x%llx, blks:0x%llx, "
		"[body] lba:0x%llx, blks:0x%llx, "
		"[tail] lba:0x%llx, blks:0x%llx\n", __func__, 
		(unsigned long long)desc->head_tail[0].lba, 
		desc->head_tail[0].nr_blks,
		(unsigned long long)desc->body.lba, desc->body.nr_blks,
		(unsigned long long)desc->head_tail[1].lba, 
		desc->head_tail[1].nr_blks);

	return;
}

int qlib_fileio_rw(
	struct __rw_task *task
	)
{
	struct __dev_info *dev_info = &task->dev_info;
	struct ___fd *__fd = &dev_info->fe_info.__fd;
	struct scatterlist *sg = NULL;
	struct iov_iter iter;
	struct bio_vec *bvec;

	sector_t dest_lba = task->lba;
	u64 len, tmp_total = 0, nr_bytes = task->nr_bytes;
	loff_t pos = 0, start = 0, end = 0;
	int ret = -EINVAL, i = 0, done_blks = 0, sync_ret;

	if ((task->dir != DMA_TO_DEVICE) && (task->dir != DMA_FROM_DEVICE)) {
		pr_err("%s: unsupported task dir: %d\n", task->dir);
		task->ret = ENOTSUPP;
		return 0;
	}

	if (!task->nr_bytes || !task->sg_list || !task->sg_nents) {
		task->ret = -EINVAL;	
		return 0;
	}

	bvec = kcalloc(task->sg_nents, sizeof(struct bio_vec), GFP_KERNEL);
	if (!bvec) {
		pr_err("%s: unable to allocate bio_vec[]\n");
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
	iov_iter_bvec(&iter, ((task->dir == DMA_TO_DEVICE) ? WRITE : READ), 
			bvec, task->sg_nents, tmp_total);

	if (task->dir == DMA_TO_DEVICE)
		ret = vfs_iter_write(__fd->fd_file, &iter, &pos, 0);
	else
		ret = vfs_iter_read(__fd->fd_file, &iter, &pos, 0);

	if (ret < 0)
		task->ret = ret;
	else {
		if (tmp_total != (u64)ret)
			task->ret = -EIO;
		else
			task->ret = 0;
		done_blks += (ret >> task->bs_order); 
	}

	if ((task->dir == DMA_TO_DEVICE) && (done_blks > 0) 
			&& (task->task_flag & RW_TASK_TYPE_FUA))
	{
		start = (task->lba << task->bs_order);
		end = start + ((sector_t)done_blks << task->bs_order) - 1;
		sync_ret = vfs_fsync_range(__fd->fd_file, start, end, 1);
		if (sync_ret != 0) {
			task->ret = sync_ret;
			pr_err("%s: write with sync behavior is failed: %d\n", 
				__func__, sync_ret);
		}
	}

	kfree(bvec);

	return done_blks;
}

static int ____qlib_blockio_rw(
	struct __rw_task *task,
	struct bio_data *bdata	
	)
{
	struct request_queue *q = bdev_get_queue(bdata->bd->bd);
	struct bio_sgl_rec *sgl_rec = NULL;
	struct scatterlist *sg = NULL;
	struct bio *mybio = NULL;
	struct __io_rec *io_rec = NULL;
	sector_t bio_lba = 0;
	int i = 0, done_blks = 0, err, rec_idx, ret;
	int bio_op = 0, bio_op_flags = 0;
	u64 len = 0, expected_bytes = 0;

	/* task lba may be 4096b, it shall be converted again for linux block layer (512b) */	
	bio_lba = ((task->lba << task->bs_order) >> 9);
	expected_bytes = task->nr_bytes;

	if ((task->dir != DMA_TO_DEVICE) && (task->dir != DMA_FROM_DEVICE)) {
		pr_err("%s: unsupported task dir: %d\n", task->dir);
		task->ret = ENOTSUPP;
		return 0;
	}

	if (!task->nr_bytes || !task->sg_list || !task->sg_nents) {
		task->ret = -EINVAL;	
		return 0;
	}

	/* prepare sgl record information by task->sg_list */
	sgl_rec = qlib_ib_alloc_bio_sgl_rec(qlib_ib_get_min_nr_vecs(q));
	if (!sgl_rec) {
		ret = -ENOMEM;
		task->ret = ret;
		goto fail;
	}
	__qlib_ib_setup_bio_sgl_rec(task, sgl_rec);

	/* prepare 1st bio */
	__qlib_ib_get_bio_op_flag(bdata->bd, task->dir, task->task_flag, &bio_op, 
					&bio_op_flags);

	mybio = __qlib_ib_get_bio_with_rec(bdata, bio_lba, qlib_ib_get_min_nr_vecs(q),
					bio_op, bio_op_flags);

	if (!mybio) {
		ret = -ENOMEM;
		task->ret = ret;
		goto fail_free_sgl_rec;
	}

	io_rec = (struct __io_rec *)mybio->bi_private;
	bio_list_add(&bdata->bio_lists, mybio);

	for (rec_idx = 0; ;rec_idx++) {
		if (sgl_rec[rec_idx].total_len == 0)
			break;

		u32 tmp_total = 0, sg_total_len = sgl_rec[rec_idx].total_len;
		sg = sgl_rec[rec_idx].sgl;

		for (i = 0; i < sgl_rec[rec_idx].sg_nents; i++) {

			ret = bio_add_page(mybio, sg_page(&sg[i]), sg[i].length, 
						sg[i].offset);
			if (ret != sg[i].length) {
				/* DEBUG DEBUG */				
				WARN_ON(true);
			}
			tmp_total += sg[i].length;
			sg_total_len -= sg[i].length;
			if (!sg_total_len)
				break;
		}

		/* total must be aligned by 512 bytes */	
		WARN_ON(tmp_total & ((1 << 9) - 1));
		WARN_ON(tmp_total != sgl_rec[rec_idx].total_len);

		bio_lba += (tmp_total >> 9);
		io_rec->nr_blks = (tmp_total >> task->bs_order);

		expected_bytes -= tmp_total;
		if (!expected_bytes)
			break;

		mybio = __qlib_ib_get_bio_with_rec(bdata, bio_lba, 
				qlib_ib_get_min_nr_vecs(q), bio_op, bio_op_flags);
		if (!mybio) {
			ret = -ENOMEM;
			task->ret = ret;
			goto fail_put_bios;
		}

		io_rec = (struct __io_rec *)mybio->bi_private;	
		bio_list_add(&bdata->bio_lists, mybio);

	}

	WARN_ON(expected_bytes);

	qlib_ib_free_bio_sgl_rec(&sgl_rec);
	__qlib_ib_submit_bio(bdata);

	task->ret = 0;
	return 0;

fail_put_bios:
	__qlib_ib_free_bio_res(bdata);
fail_free_sgl_rec:
	qlib_ib_free_bio_sgl_rec(&sgl_rec);
fail:
	return ret;
}

static int __qlib_blockio_rw_async_io(
	struct __rw_task *task
	)
{
	struct __dev_info *dev_info = &task->dev_info;
	struct bio_cb_data *cb_data = NULL;
	struct bio_data bdata;
	int ret, done_blks = 0;
	
	cb_data = kzalloc(sizeof(struct bio_cb_data), GFP_KERNEL);
	if (!cb_data) {
		task->ret = -ENOMEM;
		return -ENOMEM;
	}
	
	__qlib_init_bio_cb_data(cb_data, __qlib_ib_bio_complete);
	cb_data->priv = task->priv;
	cb_data->complete2 = task->priv_cb_complete;
	
	__qlib_init_bdata(&bdata);
	bdata.cb_data = cb_data;
	bdata.bd = &dev_info->fe_info.__bd;
	
	ret = ____qlib_blockio_rw(task, &bdata);
	if (ret || task->ret) {
		kfree(cb_data);		
		return ret;
	}

	/* cb_data will be free in complete code, we may not get done_blks here */
	__qlib_ib_bio_complete(cb_data);
	return 0;

}

static int __qlib_blockio_rw_sync_io(
	struct __rw_task *task
	)
{
#define D4_TIME2WAIT  10

	struct __dev_info *dev_info = &task->dev_info;
	struct bio_cb_data *cb_data = NULL;
	struct bio_data bdata;
	unsigned long t = msecs_to_jiffies(D4_TIME2WAIT * 1000);
	int ret, done_blks = 0;

	cb_data = kzalloc(sizeof(struct bio_cb_data), GFP_KERNEL);
	if (!cb_data) {
		task->ret = -ENOMEM;
		return -ENOMEM;
	}

	__qlib_init_bio_cb_data(cb_data, __qlib_ib_bio_wait_complete);

	__qlib_init_bdata(&bdata);
	bdata.cb_data = cb_data;
	bdata.bd = &dev_info->fe_info.__bd;

	ret = ____qlib_blockio_rw(task, &bdata);
	if (ret || task->ret) {
		kfree(cb_data);
		return ret;
	}

	if (!atomic_dec_and_test(&cb_data->bio_count)) {
		while (wait_for_completion_timeout(&cb_data->cb_wait, t) == 0)
			pr_err("%s: wait bio to be done\n", __func__);
	}

	done_blks += __qlib_ib_get_done_blks_by_list(&cb_data->io_rec_list);
	__qlib_ib_free_io_rec_by_list(&cb_data->io_rec_list);	

	pr_debug("%s: done blks(0x%x)\n", __func__, done_blks);

	if (cb_data->err) {
		pr_warn("%s: done blks(0x%x), err:%d\n", __func__, done_blks, 
						cb_data->err);
		task->ret = cb_data->err;
	}

	kfree(cb_data);
	return done_blks;
}

int qlib_blockio_rw(
	struct __rw_task *task
	)
{
	if (task->async_io)
		return __qlib_blockio_rw_async_io(task);

	return __qlib_blockio_rw_sync_io(task);
}
EXPORT_SYMBOL(qlib_blockio_rw);

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

/* 
 * we nned to consider following case 
 * - length of w,x and y from data are not multipled by 512 bytes but we
 *   need to map them to one bio and data len carried by one bio is multipled 
 *   by 512 bytes ...
 *
 * 
 * data                              bio
 *  |                                 |
 *  +---> sg[0] ---> +---------+      +---> bio vec[0] ---> +----------+
 *  |                | len: w  |      |                     | len: a   |
 *  |                +---------+      |                     +----------+
 *  |                                 |
 *  +---> sg[1] ---> +---------+      +---> bio vec[1] ---> +----------+
 *  |                | len: x  |      |                     | len: b   |
 *  |                +---------+      |                     +----------+
 *  |				   
 *  +---> sg[2] ---> +---------+
 *  |                | len: y  |
 *  |                +---------+
 *
 */
void qlib_ib_setup_bio_sgl_rec(
	struct block_device *bd,
	struct bio_sgl_rec *sgl_rec,
	struct sg_info *_sg_info,
	u64 work_bytes
	)
{
	struct scatterlist *sgl, *org_sgl, *sg;
	u32 i, rec_idx = 0;
	u32 expected_size, prev_idx = 0;
	u32 remain_sg_len, sgl_nents, remain;
	bool get_data_len = false;	

	/* get value for first time */
	sgl = org_sgl = _sg_info->sg;
	sgl_nents = _sg_info->sg_nents;

	/* we hope each bio which can carry the max i/o size */
	do {
		/* we hope to get max i/o size per bio everytime */
		expected_size = min_t(u32, work_bytes, 
				(queue_max_sectors(bdev_get_queue(bd)) << 9));

		pr_debug("s1: esize:0x%x, wsize:0x%x, q max sectors bytes:0x%X\n", 
				expected_size, work_bytes, 
				(queue_max_sectors(bdev_get_queue(bd)) << 9));

		/* returned expected_size may be smaller than orginal expected_size */
		__qlib_ib_get_sgl_min_work_len(bd, _sg_info, &expected_size);

		pr_debug("s2: min esize:0x%x\n", expected_size);

		/* use expected_size to construct our sgl_rec, remain_sg_len will 
		 * records last sg len if expected_size can't occupy full sg data
		 */
		remain_sg_len = 0;
		__qlib_ib_prep_sgl_rec(&sgl_rec[rec_idx], _sg_info, 
						expected_size, &remain_sg_len);

		pr_debug("s3: remain sg len:0x%x\n", remain_sg_len);

		prev_idx += sgl_rec[rec_idx].sg_nents;
		sgl_nents -= sgl_rec[rec_idx].sg_nents;
		rec_idx++;

		/* update _sg_info and wait for next round */
		if (sgl_nents) {
			if (remain_sg_len) {
				/* if we have remain len, need to handle last sg,
				 * so back to previos sgl pos (for old one)
				 */
				prev_idx -= 1;
				sgl_nents += 1;
			} 

			sgl = &org_sgl[prev_idx];
			__qlib_ib_update_sg_info(_sg_info, sgl, sgl_nents, 
							remain_sg_len);
			get_data_len = true;

			pr_debug("s4: size:0x%x, wsize:0x%x\n", expected_size, 
				work_bytes);

			work_bytes -= expected_size;
			if (!work_bytes)
				get_data_len = false;

		} else {
			_sg_info->sg_nents = sgl_nents;
			get_data_len = false;
		}

		pr_debug("s5: rec_idx:%d, sg_nents:%d, sg total len:0x%x\n", 
			rec_idx - 1, sgl_rec[rec_idx - 1].sg_nents, sgl_rec[rec_idx -1].total_len);

	} while (get_data_len);

}
EXPORT_SYMBOL(qlib_ib_setup_bio_sgl_rec);

void qlib_ib_free_bio_sgl_rec(
	struct bio_sgl_rec **p
	)
{
	int i;
	struct bio_sgl_rec *rec = *p;

	for (i = 0; i < MAX_BIO_SGL_REC_COUNTS; i++) {
		if (rec[i].sgl)
			kfree(rec[i].sgl);
	}

	kfree(rec);
	*p = NULL;
}
EXPORT_SYMBOL(qlib_ib_free_bio_sgl_rec);

struct bio_sgl_rec *qlib_ib_alloc_bio_sgl_rec(
	int nr_vecs
	)
{
	int i;

	struct bio_sgl_rec *p = kzalloc(
		(MAX_BIO_SGL_REC_COUNTS* sizeof(struct bio_sgl_rec)), GFP_KERNEL);

	if (!p)
		return NULL;

	for (i = 0; i < MAX_BIO_SGL_REC_COUNTS; i++) {
		p[i].sgl = kzalloc(nr_vecs * sizeof(struct scatterlist), GFP_KERNEL);
		if (!p[i].sgl)
			goto fail_free;
	}
	return p;

fail_free:
	qlib_ib_free_bio_sgl_rec(&p);
	return NULL;

}
EXPORT_SYMBOL(qlib_ib_alloc_bio_sgl_rec);

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
	struct inode *inode = file_inode(fd);
	struct address_space *mapping = inode->i_mapping;
	loff_t first_page = 0, last_page = 0, start = 0, len = 0;
	loff_t first_page_offset = 0, last_page_offset = 0;
	int ret = 0, err;

	/* convert to byte-unit */
	start = (loff_t)(lba << bs_order);
	len = (loff_t)((loff_t)nr_blks << bs_order);

	first_page = (start) >> PAGE_SHIFT;
	last_page = (start + len) >> PAGE_SHIFT;
	first_page_offset = first_page	<< PAGE_SHIFT;
	last_page_offset = (last_page << PAGE_SHIFT) + (PAGE_SIZE - 1);

	pr_debug("%s: lba(0x%llx), nr_blks(0x%x), bs_order(0x%x), "
		"start pos(0x%llx), len(0x%llx), first_page(0x%llx), "
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
		ret = filemap_write_and_wait_range(mapping, first_page_offset, 
						last_page_offset);
		if (unlikely(ret)){
			pr_err_ratelimited("%s: filemap_write_and_wait_range() call is fail, "
				"ret:%d\n", __func__, ret);

			/* if the target is thin, need to check the following case */
			if (is_thin) {
				if (ret != -ENOSPC) {
					err = qlib_fd_check_dm_thin_cond(fd);
					if (err == -ENOSPC)
						ret = err;
				}
			}
			return ret;
		}
	}

	if (truncate_cache)
		truncate_pagecache_range(inode, first_page_offset, 
			last_page_offset);

	return 0;
}
EXPORT_SYMBOL(qlib_fd_flush_and_truncate_cache);

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

	inode = file_inode(file);
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

void qlib_se_cmd_dr_init(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	INIT_LIST_HEAD(&cmd_dr->se_queue_node);
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

bool qlib_check_exported_fbc_func(void)
{
	if (!thin_support_block_cloning || !thin_do_block_cloning 
	||!qnap_fbc_do_odx || !qnap_fbc_do_xcopy
	)
		return false;
	return true;
}

void qlib_check_exported_func(void)
{
	/* check any func exported by other layer is ready or not */
	if (!qlib_check_exported_fbc_func()) {
		pr_warn("%s: fast-block-clone interfaces may not be ready, "
			"please check it\n", __func__);
	}

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
