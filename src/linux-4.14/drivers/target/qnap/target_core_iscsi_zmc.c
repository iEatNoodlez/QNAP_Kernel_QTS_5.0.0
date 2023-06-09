/*******************************************************************************
 * Filename:  
 * @file        target_core_zmc.c
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
#include "target_core_qlib.h"

extern int qnap_transport_create_devinfo(struct se_cmd *cmd, struct __dev_info *dev_info);
extern int qnap_transport_check_cmd_hit_thin_threshold(struct se_cmd *se_cmd);
extern void qnap_transport_upadte_write_result_on_fio_thin(struct se_cmd *se_cmd, 
		int *original_result);

/**/
static sense_reason_t qnap_target_iscsi_zmc_backend_ats_callback__(
	struct se_cmd *cmd, bool success, int *post_ret,
	sense_reason_t (*backend_ats_cb_write)(struct se_cmd *cmd, 
			loff_t write_pos, u8 *write_buf, int write_len)
	);

/**/
struct sg_info {
	struct scatterlist *sg;
	u32 sg_len;
	u32 sg_off;

	/* if sg_old_off != sg_off, means last sg not be used completely */
	u32 sg_old_off;

	/* if 0, no any sg can be used */
	u32 sg_nents;
};

/**/
static inline int ib_convert_lba(
	u32 dev_blksz,
	sector_t org_task_lba,
	sector_t org_sectors,
	sector_t *new_lba,
	sector_t *new_sectors
	)
{
	/*
	 * Convert the blocksize advertised to the initiator to the 512 byte
	 * units unconditionally used by the Linux block layer.
	 */
	if (dev_blksz == 4096) {
		if (new_lba)
			*new_lba = (org_task_lba << 3);
		if (new_sectors)
			*new_sectors = (org_sectors << 3);
	} else if (dev_blksz == 2048) {
		if (new_lba)
			*new_lba = (org_task_lba << 2);
		if (new_sectors)
			*new_sectors = (org_sectors << 2);
	} else if (dev_blksz == 1024) {
		if (new_lba)
			*new_lba = (org_task_lba << 1);
		if (new_sectors)
			*new_sectors = (org_sectors << 1);
	} else if (dev_blksz == 512) {
		if (new_lba)
			*new_lba = org_task_lba;
		if (new_sectors)		
			*new_sectors = org_sectors;
	} else {
		ISCSI_ZMC_ERR("Unsupported SCSI -> BLOCK LBA conversion: %u\n", 
				dev_blksz);
		return -ENOTSUPP;
	}

	return 0;
}

static inline void ib_get_write_op_flags(
	struct se_cmd *cmd, 
	struct request_queue *q,
	int *op_flags
	)
{
	/*
	 * Force writethrough using REQ_FUA if a volatile write cache
	 * is not enabled, or if initiator set the Force Unit Access bit.
	 */
	if (test_bit(QUEUE_FLAG_FUA, &q->queue_flags)) {
		if (cmd->se_cmd_flags & SCF_FUA)
			*op_flags = REQ_FUA;
		else if (!test_bit(QUEUE_FLAG_WC, &q->queue_flags))
			*op_flags = REQ_FUA;
	}
}

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


/**/
static void *qnap_target_iscsi_zmc_core_kmap_wsgl(
	struct scatterlist *zmc_wsgl,
	int zmc_wsgl_nents,
	int len
	)
{
	/* refer from transport_kmap_data_sg() and se_cmd->zmc_wsgl MUST/NEED
	 * to be prepread already when to use this call
	 */
	struct scatterlist *sg, *sgl;
	int i, wsgl_count, copied;
	u8 *src = NULL, *dest = NULL, *buf = NULL;

	sgl = zmc_wsgl;
	wsgl_count = zmc_wsgl_nents;

	if (!sgl || !wsgl_count)
		return NULL;

	if (wsgl_count == 1)
		return kmap(sg_page(sgl)) + sgl->offset;

	/* >1 page. can NOT use vmap due to our pages in sgl may not
	 * be full size (PAGE_SIZE) we need to do copy one by one ...
	 */
	dest = buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return NULL;

	for_each_sg(sgl, sg, wsgl_count, i) {
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

static void qnap_target_iscsi_zmc_core_kunmap_wsgl(
	void **zmc_data_vmap,
	struct scatterlist *zmc_wsgl,
	int zmc_wsgl_nents
	)
{
	struct scatterlist *sgl;
	int wsgl_count;

	sgl = zmc_wsgl;
	wsgl_count = zmc_wsgl_nents;

	if (!sgl || !wsgl_count)
		return;

	if (wsgl_count == 1) {
		kunmap(sg_page(sgl));
		return;
	}

	WARN_ON(!*zmc_data_vmap);

	kfree(*zmc_data_vmap);
	*zmc_data_vmap = NULL;
}


/**/
static void qnap_target_iscsi_zmc_free_wsgl_node__(
	struct qnap_se_dev_dr *dr,
	struct zmc_wsgl_node *p
	)
{
	if (dr->zmc_wsgl_node_cache && p)
		kmem_cache_free(dr->zmc_wsgl_node_cache, p);
}

static void qnap_target_iscsi_zmc_free_all_wsgl_node__(
	struct target_iscsi_zmc_data *data,
	struct qnap_se_dev_dr *dr
	)
{
	struct zmc_wsgl_node *curr = NULL, *next = NULL;

	if (!data->wsgl_node)
		return;

	curr = (struct zmc_wsgl_node *)data->wsgl_node;
	
	while (curr) {
		next = curr->next;
		WARN_ON(!curr->zmc_wsgl);

		ISCSI_ZMC_DEBUG("free  wsgl_node:0x%p, zmc_wsgl:0x%p\n", 
				curr, curr->zmc_wsgl);

		kfree(curr->zmc_wsgl);
		qnap_target_iscsi_zmc_free_wsgl_node__(dr, curr);
		curr = next;
	}

	data->wsgl_node = NULL;

}

static struct zmc_wsgl_node *qnap_target_iscsi_zmc_alloc_wsgl_node__(
	struct target_iscsi_zmc_data *data,		
	struct qnap_se_dev_dr *dr,	
	int wanted_sg_count
	)
{
	struct zmc_wsgl_node *p = NULL;
	int wsgl_size = (wanted_sg_count * sizeof(struct scatterlist));
	
	if (!dr->zmc_wsgl_node_cache)
		return NULL;

	p = kmem_cache_zalloc(dr->zmc_wsgl_node_cache, GFP_KERNEL);
	if (!p)
		return NULL;

	p->zmc_wsgl = kzalloc(wsgl_size, GFP_KERNEL);
	if (!p->zmc_wsgl) {
		qnap_target_iscsi_zmc_free_wsgl_node__(dr, p);
		return NULL;
	}

	/* not set end of table */
	memset(p->zmc_wsgl, 0, wsgl_size);
	p->zmc_wsgl_count = wanted_sg_count;
	p->next = NULL;

	ISCSI_ZMC_DEBUG("alloc wsgl node:0x%p, zmc_wsgl:0x%p, wanted sg count:%d\n", 
		p, p->zmc_wsgl, wanted_sg_count);

	return p;
}

static void qnap_target_iscsi_zmc_push_wsgl_node__(
	struct target_iscsi_zmc_data *data,
	struct zmc_wsgl_node *new
	)
{
	struct zmc_wsgl_node *curr = NULL;

	if (!data->wsgl_node) {
		data->wsgl_node = new;
		ISCSI_ZMC_DEBUG("push new wsgl_node:0x%p\n", new);
	} else {
		curr = data->wsgl_node;
		while (curr->next)
			curr = curr->next;
		curr->next = new;
		ISCSI_ZMC_DEBUG("push new wsgl_node:0x%p after curr wsgl_node:0x%p\n", 
			new, curr);
	}

}

/**
 * @brief to collect all sgl from all nodes and copy to new created sgl,
 */
static struct scatterlist *qnap_target_iscsi_zmc_prep_wsgl__(
	struct target_iscsi_zmc_data *data,
	struct zmc_wsgl_node *wsgl_node, 
	int wsgl_nents
	)
{
	struct zmc_wsgl_node *curr = NULL;
	int total, sgl_idx = 0, i, copy_count, remain;
	struct scatterlist *sgl = NULL;

	if (!wsgl_node || !wsgl_nents)
		return NULL;

	remain = total = wsgl_nents;

	ISCSI_ZMC_DEBUG("wsgl count:%d\n", total);

	sgl = kzalloc((total * sizeof(struct scatterlist)), GFP_KERNEL);
	if (!sgl)
		return NULL;

	ISCSI_ZMC_DEBUG("alloc sgl:0x%p\n", sgl);

	curr = wsgl_node;

	/* copy all sg elements whcih limits on zmc_wsgl_nents condition */
	while(curr && remain) {
		WARN_ON(!curr->zmc_wsgl_count);
		copy_count = min_t(int, curr->zmc_wsgl_count, remain);
#if 0
		for (i = 0; i < copy_count; i++) {
			ISCSI_ZMC_INFO("%d: page:0x%p, len:0x%x, off;0x%x\n",
				i, sg_page(&curr->zmc_wsgl[i]), 
				curr->zmc_wsgl[i].length,
				curr->zmc_wsgl[i].offset
				);
		}
#endif
		memcpy(&sgl[sgl_idx], &curr->zmc_wsgl[0], 
			(copy_count * sizeof(struct scatterlist)));
#if 0
		for (i = 0; i < copy_count; i++) {
			ISCSI_ZMC_INFO("sgl[%d]: page:0x%p, len:0x%x, off;0x%x\n",
				i, sg_page(&sgl[i]), sgl[i].length, sgl[i].offset);
		}
#endif
		remain -= copy_count;
		sgl_idx += copy_count;
		curr = curr->next;
	}

	/* set end of table */
	sg_mark_end(&sgl[total - 1]);
	return sgl;

}

static int qnap_target_iscsi_zmc_free_wsgl__(
	struct target_iscsi_zmc_data *data
	)
{
	if (atomic_cmpxchg(&data->write_sg_inuse, 
		ZMC_W_SG_INUSE_VALID, ZMC_W_SG_INUSE_FREED) == ZMC_W_SG_INUSE_VALID)
	{
		if (data->wsgl)
			kfree(data->wsgl);
		atomic_set(&data->wsgl_nents, 0);
	}
	return 0;
}

static int qnap_target_iscsi_zmc_kunmap_wsgl__(
	struct target_iscsi_zmc_data *data
	)
{
	/* only used when zmc_write_sg_inuse is ZMC_W_SG_INUSE_VALID or
	 * ZMC_W_SG_INUSE_FREED
	 */
	int write_sg_state = atomic_read(&data->write_sg_inuse);

	if (write_sg_state == ZMC_W_SG_INUSE_INVALID)
		return -ENOTSUPP;

	if (write_sg_state == ZMC_W_SG_INUSE_VALID) {
#warning "warn"
		qnap_target_iscsi_zmc_core_kunmap_wsgl(&data->data_vmap, 
				data->wsgl, atomic_read(&data->wsgl_nents));
	}

	/* treat it's ok if write_sg_state is ZMC_W_SG_INUSE_FREED */
	return 0;

}

static int *qnap_target_iscsi_zmc_kmap_wsgl__(
	struct target_iscsi_zmc_data *data,
	int len
	)
{
	/* only used when zmc_write_sg_inuse is ZMC_W_SG_INUSE_VALID or
	 * ZMC_W_SG_INUSE_FREED
	 */
	int write_sg_state = atomic_read(&data->write_sg_inuse);
	int nents;

	data->data_vmap = NULL;

	/* remember for READ direction command still need run in normal path */
	if (write_sg_state == ZMC_W_SG_INUSE_INVALID)
		return -ENOTSUPP;

	if (write_sg_state == ZMC_W_SG_INUSE_VALID) {
		nents = atomic_read(&data->wsgl_nents);
#warning "warn"
		data->data_vmap = qnap_target_iscsi_zmc_core_kmap_wsgl(
						data->wsgl, nents, len);
	} 

	/* treat it's ok if write_sg_state is ZMC_W_SG_INUSE_FREED */
	return 0;
}



struct bio_sgl_rec {
	struct scatterlist *sgl;
	u32 sg_nents;
	u32 total_len;
};


static inline int get_min_nr_vecs(
	struct request_queue *q
	)
{
	/* please refer bio_add_page() (it's different from kernel 4.2), 
	 * we indicate max vecs for one bio to BIO_MAX_PAGES here
	 */
	return BIO_MAX_PAGES;
}

#define MAX_BIO_SGL_REC_COUNTS	16

static void qnap_target_zmc_ib_free_bio_sgl_rec__(
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

static struct bio_sgl_rec *qnap_target_zmc_ib_alloc_bio_sgl_rec__(
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
	qnap_target_zmc_ib_free_bio_sgl_rec__(&p);
	return NULL;
}

static void get_sgl_min_work_len(
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

		if (i >= get_min_nr_vecs(q))
			break;

		tmp_len = ((_sg_info->sg == sg) ? _sg_info->sg_len : sg->length);

		if ((data_len + tmp_len) >= work_size) {
			if (data_len == 0)
				data_len = work_size;
			break;
		}
#if 0
		if ((data_len + __len) >= (queue_max_sectors(q) << 9))
			break;
#endif
		data_len += tmp_len;
	}

	if (data_len < (1 << IBLOCK_LBA_SHIFT))
		tmp_len = data_len;
	else
		/* must be multipled by 512b */
		tmp_len = ((data_len >> IBLOCK_LBA_SHIFT) << IBLOCK_LBA_SHIFT);

	ISCSI_ZMC_DEBUG("data_len:0x%x, final data len:0x%x, real sg nents:%d\n", 
			data_len, tmp_len, (i + 1));

	*expected_size = tmp_len;
}


static void prep_sgl_rec(
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

static inline void update_sg_info(
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

/* 
 * we nned to consider following case 
 * - length of w,x and y from skb data are not multipled by 512 bytes but we
 *   need to map them to one bio and data len carried by one bio is multipled 
 *   by 512 bytes ...
 *
 * 
 * skb data                          bio
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
static void qnap_target_zmc_ib_prep_w_bio_sgl_rec__(
	struct block_device *bd,
	struct bio_sgl_rec *sgl_rec,
	struct sg_info *_sg_info
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

	do {
		/* we hope to get max i/o size per bio everytime */
		expected_size = (queue_max_sectors(bdev_get_queue(bd)) << 9);

		/* returned expected_size may be smaller than orginal expected_size */
		get_sgl_min_work_len(bd, _sg_info, &expected_size);

		/* use expected_size to construct our sgl_rec, remain_sg_len will 
		 * records last sg len if expected_size can't occupy full sg data
		 */
		remain_sg_len = 0;
		prep_sgl_rec(&sgl_rec[rec_idx], _sg_info, expected_size, 
				&remain_sg_len);

		prev_idx += sgl_rec[rec_idx].sg_nents;
		sgl_nents -= sgl_rec[rec_idx].sg_nents;;
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
			update_sg_info(_sg_info, sgl, sgl_nents, remain_sg_len);
			get_data_len = true;

		} else {
			_sg_info->sg_nents = sgl_nents;
			get_data_len = false;
		}

	} while (get_data_len);

}

static sense_reason_t qnap_target_iscsi_zmc_ib_exec_write__(
	struct se_cmd *cmd, 
	struct scatterlist *sgl,
	u32 sgl_nents,
	void *data
	)
{
	struct ib_op_set *op_set = (struct ib_op_set *)data;

	/* refer from iblock_execute_rw() and only handle write operation */
	struct se_device *dev = cmd->se_dev;
	struct iblock_dev *ib_dev = dev->transport->get_dev(dev);
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct iblock_req *ibr;
	struct bio *bio, *bio_start;
	struct bio_list list;
	struct scatterlist *sg;
	sector_t block_lba;
	unsigned bio_cnt;
	unsigned int sg_len, sg_off;
	struct bio_sgl_rec *sgl_rec = NULL;
	struct sg_info _sg_info;
	u32 sg_num = sgl_nents;
	int op_flags = 0, i, ret, rec_idx;

	/**/
	ib_get_write_op_flags(cmd, q, &op_flags);

	ret = ib_convert_lba(dev->dev_attrib.block_size, cmd->t_task_lba, 0, 
			&block_lba, NULL);
	if (ret)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;;

	ibr = kzalloc(sizeof(struct iblock_req), GFP_KERNEL);
	if (!ibr)
		goto fail;

	cmd->priv = ibr;

	if (!sgl_nents) {
		refcount_set(&ibr->pending, 1);
		op_set->ib_comp_cmd(cmd);
		return 0;
	}

	sgl_rec = qnap_target_zmc_ib_alloc_bio_sgl_rec__(get_min_nr_vecs(q));
	if (!sgl_rec)
		goto fail_free_ibr;

	_sg_info.sg = sgl;
	_sg_info.sg_len = sgl[0].length;
	_sg_info.sg_off = sgl[0].offset;
	_sg_info.sg_nents = sgl_nents; 

	qnap_target_zmc_ib_prep_w_bio_sgl_rec__(ib_dev->ibd_bd, sgl_rec, &_sg_info);

	bio = op_set->ib_get_bio(cmd, block_lba, get_min_nr_vecs(q), REQ_OP_WRITE, 
					op_flags);
	if (!bio)
		goto fail_free_sgl_rec;

	bio_start = bio;
	bio_list_init(&list);
	bio_list_add(&list, bio);
	refcount_set(&ibr->pending, 2);
	bio_cnt = 1;

	qlib_drop_fb_alloc_bio_rec(&dev->dev_dr, &cmd->cmd_dr.bio_obj, 
			(void *)cmd, bio);

	for (rec_idx = 0; ;rec_idx++) {
		if (sgl_rec[rec_idx].total_len == 0)
			break;

		int total = 0;
		sg = sgl_rec[rec_idx].sgl;

		for (i = 0; i < sgl_rec[rec_idx].sg_nents; i++) {
			ret = bio_add_page(bio, sg_page(&sg[i]), sg[i].length, 
					sg[i].offset);

			if (ret != sg[i].length) {
				WARN_ON(true);
				ISCSI_ZMC_WARN(
				"rec_idx:%d, i:%d, ret:0x%x, sg.len:0x%x, "
				"rec total:0x%x, total:0x%x, "
				"bi_vcnt:0x%x, bi max vcent:0x%x, "
				"bio->bi_iter.bi_size:0x%x, "
				"max sectors:0x%x, lba:0x%llx\n", 
				rec_idx, i, ret, sg[i].length, 
				sgl_rec[rec_idx].total_len, total, 
				bio->bi_vcnt, bio->bi_max_vecs,
				bio->bi_iter.bi_size,
				blk_max_size_offset(q, bio->bi_iter.bi_sector),
				(unsigned long long)block_lba);

			}
			total += sg[i].length;
		}

		/* total must be aligned by 512 bytes */
		WARN_ON(total & ((1 << IBLOCK_LBA_SHIFT) - 1));
		WARN_ON(total != sgl_rec[rec_idx].total_len);

		block_lba += (total >> IBLOCK_LBA_SHIFT);
		bio = op_set->ib_get_bio(cmd, block_lba, get_min_nr_vecs(q),
						REQ_OP_WRITE, op_flags);
		if (!bio)
			goto fail_put_bios;

		refcount_inc(&ibr->pending);
		bio_list_add(&list, bio);
		bio_cnt++;

		qlib_drop_fb_alloc_bio_rec(&dev->dev_dr, &cmd->cmd_dr.bio_obj, 
				(void *)cmd, bio);
	}

	qnap_target_zmc_ib_free_bio_sgl_rec__(&sgl_rec);

	op_set->ib_submit_bio(&list);

	if (cmd->prot_type && dev->dev_attrib.pi_prot_type) {
		int rc = op_set->ib_alloc_bip(cmd, bio_start);
		if (rc)
			goto fail_put_bios;
	}
	
	op_set->ib_submit_bio(&list);
	op_set->ib_comp_cmd(cmd);
	return 0;

fail_put_bios:
	while ((bio = bio_list_pop(&list)))
		bio_put(bio);

fail_free_sgl_rec:
	qnap_target_zmc_ib_free_bio_sgl_rec__(&sgl_rec);

fail_free_ibr:
	kfree(ibr);
fail:
	return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
}

/* =======================================
 * iscsi zmc helper code
 * =======================================
 */

struct target_iscsi_zmc_op zmc_ops = {
	.prep_wsgl           = qnap_target_iscsi_zmc_prep_wsgl__,
	.alloc_wsgl_node     = qnap_target_iscsi_zmc_alloc_wsgl_node__,
	.push_wsgl_node      = qnap_target_iscsi_zmc_push_wsgl_node__,
	.free_all_wsgl_node  = qnap_target_iscsi_zmc_free_all_wsgl_node__,
	.kmap_wsgl           = qnap_target_iscsi_zmc_kmap_wsgl__,
	.kunmap_wsgl         = qnap_target_iscsi_zmc_kunmap_wsgl__,
	.free_wsgl_mem       = qnap_target_iscsi_zmc_free_wsgl__,
};


/* =======================================
 * iscsi zmc ib scsi op handler
 * =======================================
 */
static sense_reason_t qnap_target_iscsi_zmc_ib_ats_cb_write__(
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
	int ret, done_blks = 0;	

	memset(&task, 0, sizeof(struct __rw_task));

	ret = qnap_transport_create_devinfo(cmd, &task.dev_info);
	if (ret != 0)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	/* currently, linux supports one sector for ATS command .. so ... */
	WARN_ON(write_len != (1 << task.dev_info.bs_order));

	ret = -ENOMEM;
	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		goto fail;

	page_buf = kmap(page);
	if (!page_buf) 
		goto fail;

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

	done_blks = qlib_blockio_rw(&task);
	ret = task.ret;

	WARN_ON((done_blks != (task.nr_bytes >> task.bs_order)));

fail:
	if (page)
		__free_page(page);

	if (!ret)
		return TCM_NO_SENSE;

	if (ret == -ENOSPC)
		return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;

 	return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;	
}


static sense_reason_t qnap_target_iscsi_zmc_ib_ats_callback__(
	struct se_cmd *cmd, 
	bool success,
	int *post_ret
	)
{
	return qnap_target_iscsi_zmc_backend_ats_callback__(cmd, success, 
			post_ret, qnap_target_iscsi_zmc_ib_ats_cb_write__);	
}


static sense_reason_t qnap_target_iscsi_zmc_ib_exec_write_same__(
	struct se_cmd *cmd,
	void *data
	)
{
	/* refer from iblock_execute_write_same() */

	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;	
	struct ib_op_set *op_set = (struct ib_op_set *)data;
	struct target_iscsi_zmc_data *izmcd = &cmd->zmc_data.u.izmc;	
	struct se_device *dev = cmd->se_dev;
	struct iblock_dev *ib_dev = dev->transport->get_dev(dev);
	struct request_queue *q = bdev_get_queue(ib_dev->ibd_bd);
	struct page *page = NULL;	
	struct iblock_req *ibr;
	struct bio *bio;
	struct bio_list list;
	struct scatterlist sg;
	struct scatterlist *zmc_sgl, *zmc_sg;
	unsigned int len = 0, zmc_sgl_nents;
	int page_size, copy_size, op_flags = 0, i;
	u8 *buf = NULL, *page_buf = NULL;
	sense_reason_t rc = TCM_OUT_OF_RESOURCES;
	sector_t sectors = sbc_get_write_same_sectors(cmd);
	sector_t block_lba, new_sectors;

	if (cmd->prot_op) {
		ISCSI_ZMC_ERR("WRITE_SAME: Protection information with IBLOCK"
		       " backends not supported for zmc-enabled\n");
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	ib_get_write_op_flags(cmd, q, &op_flags);

	rc = ib_convert_lba(dev->dev_attrib.block_size, cmd->t_task_lba, sectors, 
			&block_lba, &new_sectors);
	if (rc)
		return rc;

	WARN_ON(atomic_read(&izmcd->write_sg_inuse) != ZMC_W_SG_INUSE_VALID);

	/* count total buf len to write */
	zmc_sgl = izmcd->wsgl;
	zmc_sgl_nents = atomic_read(&izmcd->wsgl_nents);

	for_each_sg(zmc_sgl, zmc_sg, zmc_sgl_nents, i)
		len += zmc_sg->length;

	WARN_ON(len != cmd->se_dev->dev_attrib.block_size);
	ISCSI_ZMC_DEBUG("ws: sgl_nents:0x%x, len:0x%x\n", zmc_sgl_nents, len);

	izmcd->ops->kmap_wsgl(izmcd, dev->dev_attrib.block_size);
	buf = izmcd->data_vmap;
	if (!buf)
		return rc;

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

	/* step 1:
	 * to create new sg and copy write-same buffer if can't use fast-zero
	 */
	page = alloc_page(GFP_KERNEL);
	if (!page)
		goto exit;

	page_buf = (u8 *)kmap(page);
	if (!page_buf) 
		goto exit;

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
	if (!ibr)
		goto exit;
	cmd->priv = ibr;

	bio = op_set->ib_get_bio(cmd, block_lba, 1, REQ_OP_WRITE, op_flags);
	if (!bio)
		goto fail_free_ibr;

	bio_list_init(&list);
	bio_list_add(&list, bio);
	refcount_set(&ibr->pending, 1);

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, PAGE_SIZE, 0);

	while (new_sectors) {
		u32 len;
		sector_t min_sectors;

		min_sectors = min_t(sector_t, new_sectors, 
				(PAGE_SIZE >> IBLOCK_LBA_SHIFT));
		len = (min_sectors << IBLOCK_LBA_SHIFT);

		while (bio_add_page(bio, sg_page(&sg), len, sg.offset) != len) {
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
	rc = TCM_NO_SENSE;
	goto exit;

fail_put_bios:
	while ((bio = bio_list_pop(&list)))
		bio_put(bio);
fail_free_ibr:
	kfree(ibr);
exit:
	if (page)
		__free_page(page);
	if (buf)
		izmcd->ops->kunmap_wsgl(izmcd);

	return rc;

}

static struct target_iscsi_zmc_scsi_op ib_scsi_op = {
	.ats              = NULL,
	.ats_callback     = qnap_target_iscsi_zmc_ib_ats_callback__,
	.write_same       = qnap_target_iscsi_zmc_ib_exec_write_same__,
	.write            = qnap_target_iscsi_zmc_ib_exec_write__,
};

/* =======================================
 * iscsi zmc fd scsi op handler
 * =======================================
 */

static sense_reason_t qnap_target_iscsi_zmc_fd_ats_cb_write__(
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
	sense_reason_t ret = TCM_NO_SENSE;
	int sync_ret;	
	int ret_bytes, tmp_ret;	

	if(!fd_dev->fd_file)
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;

	k_iov.iov_base = write_buf;
	k_iov.iov_len = write_len;
	iov_iter_kvec(&iter, (WRITE | ITER_KVEC), &k_iov, 1, write_len);

	ret_bytes = vfs_iter_write(fd_dev->fd_file, &iter, &write_pos, 0);

#ifdef SUPPORT_TP

	if ((ret_bytes > 0) && (!qnap_transport_check_cmd_hit_thin_threshold(cmd)))
		return TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;

	/* we did write for write back behavior, so to go this way if it is thin
	 * lun, write operatin to thin lun will be sync i/o if occupied data size
	 * hits the sync i/o threshold of pool
	 */
	tmp_ret = ret_bytes;

	qnap_transport_upadte_write_result_on_fio_thin(cmd, &tmp_ret);
	if (tmp_ret != ret_bytes)
		ret_bytes = tmp_ret;

	if (ret_bytes == -ENOSPC) {
		ISCSI_ZMC_WARN("%s: space was full already\n", __func__);
		return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
	}
#endif

	if (ret_bytes < 0 || (ret_bytes != write_len)) {
		ISCSI_ZMC_ERR("vfs_iter_write() returned %zd for ATS callback\n", 
					ret_bytes);
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
out:
	return ret;

}

static sense_reason_t qnap_target_iscsi_zmc_backend_ats_callback__(
	struct se_cmd *cmd, 
	bool success,
	int *post_ret,
	sense_reason_t (*backend_ats_cb_write)(struct se_cmd *cmd, 
			loff_t write_pos, u8 *write_buf, int write_len)
	)
{
	/* refer from compare_and_write_callback() */

	struct target_iscsi_zmc_data *izmcd = &cmd->zmc_data.u.izmc;	
	struct se_device *dev = cmd->se_dev;
	struct scatterlist *sg;
	unsigned char *buf = NULL, *addr;
	unsigned int offset = 0, zmc_wsgl_nents, len;
	unsigned int nlbas = cmd->t_task_nolb;
	unsigned int block_size = dev->dev_attrib.block_size;
	unsigned int compare_len, write_len;
	sense_reason_t ret = TCM_NO_SENSE;
	loff_t pos;
	int i;
	
	compare_len = write_len = (nlbas * block_size);

	/* Handle early failure in transport_generic_request_failure(),
	 * which will not have taken ->caw_sem yet..
	 */
	if (!success && (!izmcd->wsgl || !cmd->t_bidi_data_sg))
		return TCM_NO_SENSE;

	/* Handle special case for zero-length COMPARE_AND_WRITE */
	if (!cmd->data_length)
		goto out;

	/* Immediately exit + release dev->caw_sem if command has already
	 * been failed with a non-zero SCSI status.
	 */
	if (cmd->scsi_status) {
		ISCSI_ZMC_ERR("compare_and_write_callback: "
			"non zero scsi_status: 0x%02x\n", cmd->scsi_status);
		*post_ret = 1;
		if (cmd->scsi_status == SAM_STAT_CHECK_CONDITION)
				ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto out;
	}

	/* step 1: compare the read data */
	zmc_wsgl_nents = atomic_read(&izmcd->wsgl_nents);

	/* we map buf len to (2 * compare_len), one is for read-verification
	 * and the other is for written data
	 */
	izmcd->ops->kmap_wsgl(izmcd, (compare_len << 1));
	buf = izmcd->data_vmap;
	if (!buf) {
		ret = TCM_OUT_OF_RESOURCES;
		goto out;
	}

#warning "todo, need to modify"
	/* Compare against SCSI READ payload against verify payload, 
	 * we can NOT use transport_kmap_data_sg() here
	 */
	for_each_sg(cmd->t_bidi_data_sg, sg, cmd->t_bidi_data_nents, i) {
		addr = (unsigned char *)kmap_atomic(sg_page(sg));
		if (!addr) {
			ret = TCM_OUT_OF_RESOURCES;
			goto out;
		}

		len = min(sg->length, compare_len);

		if (memcmp(addr, buf + offset, len)) {
			ISCSI_ZMC_WARN("Detected MISCOMPARE at lba:0x%llx, "
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
	ISCSI_ZMC_WARN("Target/%s: Send MISCOMPARE check condition and sense\n",
			dev->transport->name);

	ret = TCM_MISCOMPARE_VERIFY;
out:
	/*
	 * In the MISCOMPARE or failure case, unlock ->caw_sem obtained in
	 * sbc_compare_and_write() before the original READ I/O submission.
	 */
	up(&dev->caw_sem);

	if (buf)
		izmcd->ops->kunmap_wsgl(izmcd);
	return ret;
}


static sense_reason_t qnap_target_iscsi_zmc_fd_ats_callback__(
	struct se_cmd *cmd, 
	bool success,
	int *post_ret
	)
{
	return qnap_target_iscsi_zmc_backend_ats_callback__(cmd, success, 
			post_ret, qnap_target_iscsi_zmc_fd_ats_cb_write__);
}

static sense_reason_t qnap_target_iscsi_zmc_fd_exec_write_same__(
	struct se_cmd *cmd,
	void *data
	)
{
#define MIN_NOLB	128

	/* refer from fd_execute_write_same() */

	struct sbc_ops *ops = (struct sbc_ops *)cmd->protocol_data;	
	struct target_iscsi_zmc_data *izmcd = &cmd->zmc_data.u.izmc;	
	struct se_device *se_dev = cmd->se_dev;
	struct fd_dev *fd_dev = se_dev->transport->get_dev(se_dev);
	struct scatterlist *zmc_sgl, *zmc_sg;
	struct iov_iter iter;
	struct kvec *k_iov = NULL;
	loff_t pos = cmd->t_task_lba * se_dev->dev_attrib.block_size;
	sector_t nolb = sbc_get_write_same_sectors(cmd), min_nolb;
	unsigned char *buf = NULL;
	unsigned int len = 0, i, zmc_sgl_nents;
	int ret, tmp_ret;
	sense_reason_t rc;

	if (!nolb) {
		target_complete_cmd(cmd, SAM_STAT_GOOD);
		return 0;
	}

	if (cmd->prot_op) {
		ISCSI_ZMC_ERR("WRITE_SAME: Protection information with FILEIO"
		       " backends not supported on zmc-enabled\n");
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}

	WARN_ON(atomic_read(&izmcd->write_sg_inuse) != ZMC_W_SG_INUSE_VALID);

	/* count total buf len to write */
	zmc_sgl = izmcd->wsgl;
	zmc_sgl_nents = atomic_read(&izmcd->wsgl_nents);

	for_each_sg(zmc_sgl, zmc_sg, zmc_sgl_nents, i)
		len += zmc_sg->length;

	WARN_ON(len != cmd->se_dev->dev_attrib.block_size);
	ISCSI_ZMC_DEBUG("ws: sgl_nents:0x%x, len:0x%x\n", zmc_sgl_nents, len);

	izmcd->ops->kmap_wsgl(izmcd, cmd->se_dev->dev_attrib.block_size);	
	buf = izmcd->data_vmap;
	if (!buf)
		return TCM_OUT_OF_RESOURCES;

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

	/* fall-back */
normal_path:

	/* actually, we can't use struct bio_vec due to we can't make sure
	 * whether the data is all in one sg element or not.
	 * and, we may receive a lot of lba requests and cause memory allocation
	 * will be failure. So to limit this.
	 */

	k_iov = kzalloc((MIN_NOLB * sizeof(struct kvec)), GFP_KERNEL);
	if (!k_iov) {
		izmcd->ops->kunmap_wsgl(izmcd);
		return TCM_OUT_OF_RESOURCES;
	}

	while (nolb) {
		min_nolb = min_t(sector_t, MIN_NOLB, nolb);
		len = 0;

		for (i = 0; i < min_nolb; i++) {
			k_iov[i].iov_base = buf;
			k_iov[i].iov_len = se_dev->dev_attrib.block_size;
			len += se_dev->dev_attrib.block_size;
		}

		iov_iter_kvec(&iter, (WRITE | ITER_KVEC), k_iov, min_nolb, len);
		ret = vfs_iter_write(fd_dev->fd_file, &iter, &pos, 0);

		if (ret < 0 || ret != len)
			break;

		nolb -= min_nolb;
	}

	kfree(k_iov);
	izmcd->ops->kunmap_wsgl(izmcd);

#ifdef SUPPORT_TP

	if ((ret > 0) && (!qnap_transport_check_cmd_hit_thin_threshold(cmd)))
		return TCM_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED;

	/* we did write for write back behavior, so to go this way if it is thin
	 * lun, write operatin to thin lun will be sync i/o if occupied data size
	 * hits the sync i/o threshold of pool
	 */
	tmp_ret = ret;

	qnap_transport_upadte_write_result_on_fio_thin(cmd, &tmp_ret);
	if (tmp_ret != ret)
		ret = tmp_ret;

	if (ret == -ENOSPC) {
		ISCSI_ZMC_WARN("%s: space was full already\n", __func__);
		return TCM_SPACE_ALLOCATION_FAILED_WRITE_PROTECT;
	}
#endif

	if (ret < 0 || ret != len) {
		ISCSI_ZMC_ERR("vfs_iter_write() returned %zd for write same\n", ret);
		return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	}
	
	target_complete_cmd(cmd, SAM_STAT_GOOD);
	return 0;
}


static struct target_iscsi_zmc_scsi_op fd_scsi_op = {
	.ats              = NULL,
	.ats_callback     = qnap_target_iscsi_zmc_fd_ats_callback__,
	.write_same       = qnap_target_iscsi_zmc_fd_exec_write_same__,
	.write            = NULL,
};

/**/
int qnap_target_iscsi_zmc_alloc_sgl(
	enum dma_data_direction	data_direction,
	bool *alloc_done
	)
{
	/* For every write-direction command, we always use zmc wsgl. 
	 * But, here is tricky method we do NOT allocate fixed wsgls, 
	 * all wsgls we want will be first handled in 
	 * __qnap_iscsit_zmc_skb_to_sgvec() then prepare final one 
	 * in qnap_target_zmc_prep_wsgl() called by sbc_execute_rw()
	 */ 
	if (data_direction == DMA_TO_DEVICE)
		*alloc_done = true;

	return 0;
}

void qnap_target_iscsi_zmc_destory_wsgl_node_cache(
	struct qnap_se_dev_dr *dr
	)
{
	if (dr->zmc_wsgl_node_cache)
		kmem_cache_destroy(dr->zmc_wsgl_node_cache);
}

int qnap_target_iscsi_zmc_create_wsgl_node_cache(
	struct qnap_se_dev_dr *dr,
	u32 dev_index
	)
{
	char name[64];
	int ret = 0;

	snprintf(name, 64, "zmc_wsgl_node_cache_%d", dev_index);

	dr->zmc_wsgl_node_cache = kmem_cache_create(name, 
			sizeof(struct zmc_wsgl_node),
			__alignof__(struct zmc_wsgl_node), 0, NULL);
	
	if (!dr->zmc_wsgl_node_cache) {
		ISCSI_ZMC_WARN("Unable to create zmc_wsgl_node_cache\n");
		ret = -ENOMEM;
	}

	return ret;
}

void qnap_target_iscsi_zmc_prep_backend_op(
	struct se_cmd *cmd
	)
{

	struct target_iscsi_zmc_data *p = &cmd->zmc_data.u.izmc;

	if (!qlib_is_ib_fbdisk_dev(&cmd->se_dev->dev_dr))
		p->scsi_ops = &ib_scsi_op;
	else if (!qlib_is_fio_blk_dev(&cmd->se_dev->dev_dr))
		p->scsi_ops = &fd_scsi_op;
}

void qnap_target_se_cmd_iscsi_zmc_alloc(
	struct se_cmd *se_cmd
	)
{
	struct target_iscsi_zmc_data *p;

	BUG_ON(!se_cmd->se_tfo);

	p = &se_cmd->zmc_data.u.izmc;

	atomic_set(&p->write_sg_inuse, ZMC_W_SG_INUSE_INVALID);
	atomic_set(&p->wsgl_nents, 0);
	p->data_vmap = NULL;
	p->wsgl_node = NULL;
	p->wsgl = NULL;
	p->scsi_ops = NULL;  /* prepared in qnap_target_iscsi_zmc_prep_backend_op() */
	p->ops = &zmc_ops;
}
