/*******************************************************************************
 * Filename:  target_core_qodx.c
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

#include <crypto/hash.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/in.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <asm/unaligned.h>
#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>

#if 0
#include "../target_core_file.h"
#include "../target_core_iblock.h"
#include "../target_core_pr.h" /* used for spc_parse_naa_6h_vendor_specific() */

#endif

#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qodx_lib.h"
#include "target_core_qodx_rb.h"
#include "target_core_qodx.h"

/**/
extern int qnap_odx_core_rrti(struct odx_work_request *odx_wr);
extern int qnap_odx_core_before_pt(struct odx_work_request *odx_wr, bool *go_pt);
extern int qnap_odx_core_pt(struct odx_work_request *odx_wr);

extern int qnap_odx_core_wut_pre_work(struct odx_work_request *odx_wr, bool *go_wut);
extern int qnap_odx_core_wut(struct odx_work_request *odx_wr);

extern int qnap_odx_vpd_emulate_evpd_8f_step1(u16 alloc_len);
extern int qnap_odx_vpd_emulate_evpd_8f_step2(u8 *buffer, u32 blk_size);

/**/
struct lba_len_desc_data {
	u8	*start_desc;
	u16	desc_count;
	u8	cdb0;
	u8	cdb1;
};

struct lba_len_data {
	sector_t lba;
	u32 nr_blks;
};

/**/
static void _qnap_odx_setup_odx_dr(
	struct qnap_se_cmd_odx	*odx_dr,
	void *odx_tpg,
	struct __reg_data *data
	)
{
	odx_dr->is_odx_cmd = true;
	odx_dr->cmd_type = data->cmd_type;
	odx_dr->odx_tpg = odx_tpg;
	odx_dr->cmd_id_lo = data->cmd_id_lo;
	odx_dr->cmd_id_hi = data->cmd_id_hi;
	odx_dr->tpg_id_hi = data->tpg_id_hi;
	odx_dr->tpg_id_lo = data->tpg_id_lo;
	odx_dr->initiator_id_hi = data->initiator_id_hi;
	odx_dr->initiator_id_lo = data->initiator_id_lo;
	odx_dr->list_id = data->list_id;
	odx_dr->sac = data->sac;
}

static int _qnap_odx_create_initiator_id_with_initiator_name_isid(
	u8 *initiator_name,
	u8 *isid,
	u64 *id_hi,
	u64 *id_lo
	)
{
	struct crypto_shash *tfm = NULL;
	struct shash_desc *desc = NULL;
	u8 *data = NULL;
	u8 md5_digest[MD5_SIGNATURE_SIZE];
	int ret = -EINVAL;

	data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	
	tfm = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("%s: Unable to allocate struct crypto_shash\n", __func__);
		tfm = NULL;
		goto out;
	}
	
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		pr_err("%s: Unable to allocate struct shash_desc\n", __func__);
		goto out;
	}
		
	desc->tfm = tfm;
			
	ret = crypto_shash_init(desc);
	if (ret < 0) {
		pr_err("%s: crypto_shash_init() failed\n", __func__);
		goto out;
	}
	
	snprintf(data, PAGE_SIZE, "%s", initiator_name);
	ret = crypto_shash_update(desc, data, strlen(initiator_name));
	if (ret < 0) {
		pr_err("%s: crypto_shash_update() failed for list id\n", __func__);
		goto out;
	}
		
	snprintf(data, PAGE_SIZE, "%s", isid);
	ret = crypto_shash_update(desc, data, strlen(isid));
	if (ret < 0) {
		pr_err("%s: crypto_shash_update() failed for isid\n", __func__);
		goto out;
	}
		
	ret = crypto_shash_final(desc, md5_digest);
	if (ret < 0) {
		pr_err("%s: crypto_shash_final() failed for md5 digest for "
			"initiator name + isid\n", __func__);
		goto out;
	}
	
	*id_hi = *(u64 *)&md5_digest[0];
	*id_lo = *(u64 *)&md5_digest[8];
	ret = 0;
out:
	if (data)
		kfree(data);
	if (desc)
		kfree(desc);
	if (tfm)
		crypto_free_shash(tfm);
	return ret;

}

static int _qnap_odx_create_initiator_id(
	struct se_cmd *se_cmd, 
	u64 *id_hi,
	u64 *id_lo
	)
{
	u8 initiator_name[256];
	u8 isid[PR_REG_ISID_LEN];
	int ret = -EINVAL;
	
	if (!se_cmd->se_lun) {
		pr_err("%s: not found se_lun in se_cmd\n", __func__);
		return -ENODEV;
	}
	
	if (!se_cmd->se_tfo->sess_get_initiator_sid
	|| !se_cmd->se_tfo->get_initiator_name
	)
	{
		pr_err("%s: either sess_get_initiator_sid() or "
			"get_initiator_name() not implemented\n", __func__);
		return -EINVAL;
	}

	memset(isid, 0, sizeof(isid));
	se_cmd->se_tfo->sess_get_initiator_sid(se_cmd->se_sess, isid,
			PR_REG_ISID_LEN);
	
	memset(initiator_name, 0, sizeof(initiator_name));
	memcpy(initiator_name, se_cmd->se_tfo->get_initiator_name(se_cmd), 
			min(sizeof(initiator_name), 
			strlen(se_cmd->se_tfo->get_initiator_name(se_cmd)))
			);

	return _qnap_odx_create_initiator_id_with_initiator_name_isid(
				initiator_name, isid, id_hi, id_lo);

}

static int _qnap_odx_create_tpg_id(
	struct se_portal_group *se_tpg,
	u64 *id_hi,
	u64 *id_lo
	)
{
	struct crypto_shash *tfm = NULL;
	struct shash_desc *desc = NULL;
	u8 *data = NULL;
	unsigned char md5_digest[MD5_SIGNATURE_SIZE];
	u8 target_name[256];
	u16 pg_tag = 0;
	int ret = -EINVAL;

	*id_hi = 0;
	*id_lo = 0;
	
	if (!se_tpg->se_tpg_tfo->tpg_get_tag
		|| !se_tpg->se_tpg_tfo->tpg_get_wwn
	)
	{
		pr_err("%s: either tpg_get_tag() or tpg_get_wwn() "
			"not implemented\n", __func__);
		return -EINVAL;
	}

	data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	
	pg_tag = se_tpg->se_tpg_tfo->tpg_get_tag(se_tpg);
	
	memset(target_name, 0, sizeof(target_name));
	memcpy(target_name, se_tpg->se_tpg_tfo->tpg_get_wwn(se_tpg), 
			min(sizeof(target_name), 
			strlen(se_tpg->se_tpg_tfo->tpg_get_wwn(se_tpg)))
			);
	
	tfm = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("%s: Unable to allocate struct crypto_shash\n", __func__);
		tfm = NULL;
		goto out;
	}
	
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		pr_err("%s: Unable to allocate struct shash_desc\n", __func__);
		goto out;
	}

	desc->tfm = tfm;
		
	ret = crypto_shash_init(desc);
	if (ret < 0) {
		pr_err("%s: crypto_shash_init() failed\n", __func__);
		goto out;
	}
	
	snprintf(data, PAGE_SIZE, "%s", target_name);
	ret = crypto_shash_update(desc, data, strlen(target_name));
	if (ret < 0) {
		pr_err("%s: crypto_shash_update() failed for target name\n", __func__);
		goto out;
	}
		
	*(u16 *)&data[0] = pg_tag;
	data[1] = 0;
	ret = crypto_shash_update(desc, data, sizeof(u16));
	if (ret < 0) {
		pr_err("%s: crypto_shash_update() failed for pg tag id\n", __func__);
		goto out;
	}
	
	ret = crypto_shash_final(desc, md5_digest);
	if (ret < 0) {
		pr_err("%s: crypto_hash_final() failed for md5 digest for "
			"pg tag + target name\n", __func__);
		goto out;
	}
	
	*id_lo = *(u64 *)&md5_digest[0];
	*id_hi = *(u64 *)&md5_digest[8];
	ret = 0;
out:
	if (data)
		kfree(data);	
	if (desc)
		kfree(desc);
	if (tfm)
		crypto_free_shash(tfm);
	return ret;

}

static int _qnap_odx_create_cmd_id(
	struct se_cmd *se_cmd, 
	u64 *id_hi,
	u64 *id_lo,
	u32 list_id
	)
{
	struct crypto_shash *tfm = NULL;
	struct shash_desc *desc = NULL;
	u8 *data = NULL;
	unsigned char md5_digest[MD5_SIGNATURE_SIZE];
	int ret = -EINVAL;
		
	if (!se_cmd->se_lun) {
		pr_err("%s: not found se_lun in se_cmd\n", __func__);
		return -ENODEV;
	}

	data = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
		
	tfm = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("%s: Unable to allocate struct crypto_shash\n", __func__);
		tfm = NULL;
		goto out;
	}

	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		pr_err("%s: Unable to allocate struct shash_desc\n", __func__);
		goto out;
	}
		
	desc->tfm = tfm;
			
	ret = crypto_shash_init(desc);
	if (ret < 0) {
		pr_err("%s: crypto_shash_init() failed\n", __func__);
		goto out;
	}
		
	*(u32 *)&data[0] = list_id;
	ret = crypto_shash_update(desc, data, sizeof(u32));
	if (ret < 0) {
		pr_err("%s: crypto_shash_update() failed for list id\n", __func__);
		goto out;
	}

	ret = crypto_shash_final(desc, md5_digest);
	if (ret < 0) {
		pr_err("%s: crypto_shash_final() failed for md5 digest for "
			"list id + sac\n", __func__);
		goto out;
	}
		
	*id_lo = *(u64 *)&md5_digest[0];
	*id_hi = *(u64 *)&md5_digest[8];
	ret = 0;
out:
	if (data)
		kfree(data);
	if (desc)
		kfree(desc);
	if (tfm)
		crypto_free_shash(tfm);

	return ret;

}

/* 1   : format is not for (generic) write dir cmd 
 * 0   : format is what we want
 * < 0 : other error
 */
static int _qnap_odx_parse_write_dir_cmd_data(
	struct se_cmd *se_cmd,
	void *desc_data
	)
{
	u8 *cdb = se_cmd->t_task_cdb;
	struct lba_len_data *desc = (struct lba_len_data *)desc_data;
	u16 sac = 0;
	int ret = 0;

	switch(cdb[0]){
	case WRITE_6:
		desc->nr_blks = (u32)(cdb[4] ? : 256);
		desc->lba = (sector_t)(get_unaligned_be24(&cdb[1]) & 0x1fffff);
		break;

	case XDWRITEREAD_10:
	case WRITE_VERIFY:
	case WRITE_10:
		desc->nr_blks = (u32)get_unaligned_be16(&cdb[7]);
		desc->lba = (sector_t)get_unaligned_be32(&cdb[2]);
	        break;

	case WRITE_VERIFY_12:
	case WRITE_12:
		desc->nr_blks = (u32)get_unaligned_be32(&cdb[6]);
		desc->lba = (sector_t)get_unaligned_be32(&cdb[2]);
		break;

//	case WRITE_VERIFY_16:
 	case WRITE_16:
		desc->nr_blks = (u32)get_unaligned_be32(&cdb[10]);
		desc->lba = (sector_t)get_unaligned_be64(&cdb[2]);
		break;

	case WRITE_SAME:
		desc->nr_blks = (u32)get_unaligned_be16(&cdb[7]);
		desc->lba = get_unaligned_be32(&cdb[2]);
		break;

	case WRITE_SAME_16:
		desc->nr_blks = (u32)get_unaligned_be32(&cdb[10]);
		desc->lba = get_unaligned_be64(&cdb[2]);
		break;

	case COMPARE_AND_WRITE:
		desc->nr_blks = (u32)cdb[13];
		desc->lba = get_unaligned_be64(&cdb[2]);
		break;

	case VARIABLE_LENGTH_CMD:
		sac = get_unaligned_be16(&cdb[8]);
		switch (sac) {
		case XDWRITEREAD_32:
			desc->nr_blks = (u32)get_unaligned_be32(&cdb[28]);	
			/* For VARIABLE_LENGTH_CDB w/ 32 byte extended CDBs */
			desc->lba = (sector_t)get_unaligned_be64(&cdb[12]);
			break;
		case WRITE_SAME_32:
			desc->nr_blks = (u32)get_unaligned_be32(&cdb[28]);
			desc->lba = get_unaligned_be64(&cdb[12]);
			break;
		default:
			pr_warn("%s: warning, VARIABLE_LENGTH_CMD service "
				"action 0x%04x not supported\n", __func__, sac);
			ret = 1;
			break;
		}
		break;
	default:
		ret = 1;
		break;
	}

	return ret;

}

/* 1   : format in not in (generic) write dir cmd 
 * 0   : format is what we want
 * < 0 : other error
 */
static int _qnap_odx_parse_write_blk_desc_cmd_data(
	struct se_cmd *se_cmd,
	void *desc_data
	)
{
	u8 *cdb = se_cmd->t_task_cdb;
	struct lba_len_desc_data *ll_desc_data = 
					(struct lba_len_desc_data *)desc_data;
	u8 *p = NULL;
	int ret = 0;

	/* we will do kunmap data sg outside if to parse format successfully */
	if((p = (u8 *)transport_kmap_data_sg(se_cmd)) == NULL)
	    return -ENOMEM;
	
	switch(cdb[0]){
	case UNMAP:
		ll_desc_data->start_desc = (u8 *)(p + 8);
		ll_desc_data->desc_count = 
			__qnap_odx_get_desc_counts(get_unaligned_be16(&p[2]));

		if (!ll_desc_data->desc_count)
			ret = 1;
		break;
	default:
		ret = 1;
		break;
	}

	/* if parse format successfully, transport_kunmap_data_sg() will be called
	 * by caller
	 */
	if (ret && p)
	     transport_kunmap_data_sg(se_cmd);

	return ret;

}


/* 0	  : cancel token successfully or not need to cancel token 
 *	    even the parsing format is correct
 * others : format is not we want or other errors
 */
static int _qnap_odx_check_cancel_token(
	struct se_cmd *se_cmd,
	int (*qnap_parse_cmd_data)(struct se_cmd *, void *)
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct se_portal_group *se_tpg = NULL;
	struct tpc_tpg_data *tpg_p = NULL;
	struct blk_dev_range_desc *start_desc = NULL;
	struct blk_dev_range_desc tmp_range_desc;
	struct lba_len_desc_data ll_desc_data;
	struct lba_len_data ll_data;
	struct __dev_info dev_info;
	bool parse_blk_desc = false;
	u16 desc_idx = 0;
	int ret;	

	ret = qnap_transport_create_devinfo(se_cmd, &dev_info);
	if (ret)
		return -EINVAL;

	if (qnap_parse_cmd_data != _qnap_odx_parse_write_dir_cmd_data) {
		parse_blk_desc = true;		
		ret = qnap_parse_cmd_data(se_cmd, (void *)&ll_desc_data);
	} else
		ret = qnap_parse_cmd_data(se_cmd, (void *)&ll_data);

	/* 0   : format is we want
	 * 1   : format is not we want
	 * < 0 : other error
	 */
	if (ret)
		return ret;

	/* no any data to be transfered if len is zero */
	if (!parse_blk_desc && !ll_data.nr_blks)
		return 0;
	else if (parse_blk_desc && !ll_desc_data.desc_count)
		return 0;

	if (!parse_blk_desc) {
		put_unaligned_be64(ll_data.lba, &tmp_range_desc.lba[0]);
		put_unaligned_be32(ll_data.nr_blks, &tmp_range_desc.nr_blks[0]);
		ll_desc_data.desc_count = 1;
		ll_desc_data.start_desc = (u8 *)&tmp_range_desc;
	}

	ll_desc_data.cdb0 = se_cmd->t_task_cdb[0];
	ll_desc_data.cdb1 = se_cmd->t_task_cdb[1];

	/* start to check each token range [lba, nr_blks] with passing command */
	se_tpg = se_cmd->se_lun->lun_tpg;
	tpg_p = qnap_odx_rb_tpg_get(se_tpg->odx_dr.tpg_id_hi, 
						se_tpg->odx_dr.tpg_id_lo);
	if (tpg_p) {	
		start_desc = (struct blk_dev_range_desc *)ll_desc_data.start_desc;
		
		for (desc_idx = 0; desc_idx < ll_desc_data.desc_count; desc_idx++) {
			if (!get_unaligned_be32(&start_desc[desc_idx].nr_blks[0]))
				continue;
		
			qnap_odx_rb_parse_conflict_token_range(tpg_p, &dev_info, 
				&start_desc[desc_idx], desc_idx, 
				ll_desc_data.cdb0, ll_desc_data.cdb1);
		}
		qnap_odx_rb_tpg_put(tpg_p);
	}

	/* take care we call transport_kmap_data_sg() in
	 * _qnap_odx_parse_write_blk_desc_cmd_data already 
	 */
	if (parse_blk_desc && ll_desc_data.desc_count)
		transport_kunmap_data_sg(se_cmd);

	return 0;
}

static void _qnap_odx_cmd_put_and_del(
	struct se_cmd *se_cmd,
	struct tpc_cmd_data *tc_p
	)
{
	struct se_portal_group *se_tpg = NULL;
	int count;

	if (!se_cmd->odx_dr.is_odx_cmd || !tc_p)
		return;

	se_tpg = se_cmd->se_sess->se_tpg;

	do {
		count = qnap_odx_rb_cmd_put(tc_p);
		WARN_ON(count < 0);

		if (count == 1)
			break;

		pr_debug("%s: odx (count: %d) may still works\n", __func__, count);

		/* just follow core_tpg_deregister() ... :( */
		cpu_relax();
	} while(1);

	qnap_odx_rb_cmd_del(se_tpg->odx_dr.odx_tpg, tc_p);

}

static int _qnap_is_odx_in_progress(struct se_cmd *cmd, u32 list_id)
{
	struct se_portal_group *se_tpg = NULL;
	u8 *cdb = &cmd->t_task_cdb[0];	
	struct tpc_cmd_data *tc_p = NULL;
	struct __reg_data reg_data;
	int ret;

	/* create md5 from necessary information */
	ret = _qnap_odx_create_cmd_id(cmd, &reg_data.cmd_id_hi,
			&reg_data.cmd_id_lo, list_id);
	if (ret != 0)
		return -EINVAL;

	ret = _qnap_odx_create_initiator_id(cmd, &reg_data.initiator_id_hi,
			&reg_data.initiator_id_lo);
	if (ret != 0)
		return -EINVAL;

	se_tpg = cmd->se_lun->lun_tpg;
	reg_data.tpg_id_lo = se_tpg->odx_dr.tpg_id_lo;
	reg_data.tpg_id_hi = se_tpg->odx_dr.tpg_id_hi;
	reg_data.list_id = list_id;
	reg_data.sac = (cdb[1] & 0x1f);

	if (IS_TPC_SCSI_OP(cdb[0]) && IS_TPC_SCSI_RRTI_OP(cdb[1]))
		reg_data.cmd_type = ODX_MONITOR_OP;
	else
		reg_data.cmd_type = ODX_CP_OP;

	ret = qnap_transport_create_devinfo(cmd, &reg_data.dev_info);
	if (ret)
		return -EINVAL;

	/* now to check current odx cmd exists in tree or not */
	tc_p = qnap_odx_rb_cmd_get(se_tpg->odx_dr.odx_tpg, &reg_data, 
			false, false);
	if (tc_p) {
		/* if found it, the cmd is in progress */
		pr_warn("%s: odx is in progress. list_id(0x%x), sac(0x%x)\n", 
			__func__, tc_p->reg_data.list_id, tc_p->reg_data.sac);

		qnap_odx_rb_cmd_put(tc_p);
		return -EBUSY;
	}

	/* try add new cmd reocrd if not find it */
	cmd->odx_dr.odx_cmd = (void *)qnap_odx_rb_cmd_add_and_get(
						se_tpg->odx_dr.odx_tpg, &reg_data);
	if (!cmd->odx_dr.odx_cmd)
		return -EINVAL;

	_qnap_odx_setup_odx_dr(&cmd->odx_dr, se_tpg->odx_dr.odx_tpg, &reg_data);

	pr_debug("%s: op(0x%x), list_id(0x%x), sac(0x%x), type:%d, tpg_p(0x%p), "
		"odx_p(0x%p), cmd id(hi):0x%llx, cmd id(lo):0x%llx, "
		"initiator id(hi):0x%llx, initiator id(lo):0x%llx\n",
		__func__, cdb[0], list_id, (cdb[1] & 0x1f), cmd->odx_dr.cmd_type, 
		cmd->odx_dr.odx_tpg, cmd->odx_dr.odx_cmd, 
		(unsigned long long)reg_data.cmd_id_hi,
		(unsigned long long)reg_data.cmd_id_lo,
		(unsigned long long)reg_data.initiator_id_hi,
		(unsigned long long)reg_data.initiator_id_lo);

	return 0;
}

static int _qnap_cancel_odx_token(struct se_cmd *cmd)
{
	int ret;

	if (cmd->data_direction != DMA_TO_DEVICE)
		return -EINVAL;

	if (!cmd->se_lun || !cmd->se_lun->lun_tpg)
		return -EINVAL;

	ret = _qnap_odx_check_cancel_token(cmd, 
			_qnap_odx_parse_write_dir_cmd_data);
	if (ret) {
		_qnap_odx_check_cancel_token(cmd, 
				_qnap_odx_parse_write_blk_desc_cmd_data);
	}
	return ret;
}

static sense_reason_t _qnap_odx_do_wut(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	u8 *cdb = se_cmd->t_task_cdb;
	struct odx_work_request odx_wr;
	struct tpc_tpg_data *tpg_p = NULL;
	struct tpc_cmd_data *tc_p = NULL;
	struct __reg_data tmp_reg_data;
	void *p = NULL;
	sector_t all_nr_blks = 0;
	sense_reason_t ret;
	bool go_wut;

	if (!se_cmd->odx_dr.is_odx_cmd)
		return TCM_UNSUPPORTED_SCSI_OPCODE;
	
	/* if length is zero, means no data shall be sent, not treat it as error */
	if(get_unaligned_be32(&cdb[10]) == 0) {
		target_complete_cmd(se_cmd, SAM_STAT_GOOD);
		return TCM_NO_SENSE;
	}

	/* prepare sg list since we need i/o for WUT */
	memset(&odx_wr, 0, sizeof(struct odx_work_request));

	qnap_odxlib_alloc_sg_lists(&odx_wr, 
				((se_dev->dev_dr.odx_rwio_wq) ? true : false));

	/* we need one at least */
	if (!odx_wr.sg_io[0].data_sg) {
		ret = TCM_INSUFFICIENT_RESOURCES;
		goto _out;
	}

	p = transport_kmap_data_sg(se_cmd);
	if (!p) {
		ret = TCM_INSUFFICIENT_RESOURCES;
		goto _out;
	}

	/* following to get tpg_p and tc_p again ... */
	tpg_p = qnap_odx_rb_tpg_get(se_cmd->odx_dr.tpg_id_hi, se_cmd->odx_dr.tpg_id_lo);
	if (!tpg_p) {
		ret = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;
	}

	if (tpg_p != se_cmd->odx_dr.odx_tpg) {
		BUG_ON(true);
		qnap_odx_rb_tpg_put(tpg_p);
		ret = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;	
	}

	tmp_reg_data.initiator_id_hi = se_cmd->odx_dr.initiator_id_hi;
	tmp_reg_data.initiator_id_lo = se_cmd->odx_dr.initiator_id_lo;
	tmp_reg_data.tpg_id_hi = se_cmd->odx_dr.tpg_id_hi;
	tmp_reg_data.tpg_id_lo = se_cmd->odx_dr.tpg_id_lo;
	tmp_reg_data.cmd_id_hi = se_cmd->odx_dr.cmd_id_hi;
	tmp_reg_data.cmd_id_lo = se_cmd->odx_dr.cmd_id_lo;
	tmp_reg_data.list_id = se_cmd->odx_dr.list_id;
	tmp_reg_data.sac = se_cmd->odx_dr.sac;
	tmp_reg_data.cmd_type = se_cmd->odx_dr.cmd_type;

	tc_p = qnap_odx_rb_cmd_get(tpg_p, &tmp_reg_data, false, false);
	if (!tc_p) {
		qnap_odx_rb_tpg_put(tpg_p);
		ret = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;
	}	

	if (tc_p != se_cmd->odx_dr.odx_cmd) {
		BUG_ON(true);
		qnap_odx_rb_cmd_put(tc_p);
		qnap_odx_rb_tpg_put(tpg_p);	
		ret = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;	
	}

	/* create odx request */
	odx_wr.io_ops = &se_cmd->cmd_dr.io_ops;
	odx_wr.cdb = se_cmd->t_task_cdb;
	odx_wr.odx_rwio_wq = se_dev->dev_dr.odx_rwio_wq;
	odx_wr.buff = p;
	odx_wr.tpg_p = tpg_p;
	odx_wr.tc_p = tc_p;
	odx_wr.se_dev = (void *)se_cmd->se_dev;
	odx_wr.rc = RC_GOOD;
	memcpy(&odx_wr.reg_data, &tc_p->reg_data, sizeof(struct __reg_data));

	/* filter something first */
	if (qnap_odx_core_wut_pre_work(&odx_wr, &go_wut) == 0) {
		if (go_wut)
			qnap_odx_core_wut(&odx_wr);
	}

	/* report transfer count here */
	se_cmd->odx_dr.transfer_counts = odx_wr.transfer_counts;

	qnap_odx_rb_cmd_put(tc_p);
	qnap_odx_rb_tpg_put(tpg_p);	

	/* convert RC to TCM_xxx */
	ret = qnap_transport_convert_rc_to_tcm_sense_reason((u32)odx_wr.rc);
_out:
	if (p)
		transport_kunmap_data_sg(se_cmd);

	qnap_odxlib_free_sg_lists(&odx_wr);

	if (ret == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, SAM_STAT_GOOD);

	return ret;

}

static void _qnap_odx_wut_io_work(
	struct work_struct *work
	)
{
	struct qnap_se_cmd_dr *dr = container_of(work, struct qnap_se_cmd_dr, 
						odx_wut_work);
	struct se_cmd *cmd = container_of(dr, struct se_cmd, cmd_dr);
	sense_reason_t reason;

	reason = _qnap_odx_do_wut(cmd);
	if (reason) {
		spin_lock_irq(&cmd->t_state_lock);
		cmd->transport_state &= ~CMD_T_SENT;
		spin_unlock_irq(&cmd->t_state_lock);
		transport_generic_request_failure(cmd, reason);
	}
}

static sense_reason_t _qnap_odx_wut(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;

	if (se_dev->dev_dr.odx_wut_wq) {
		INIT_WORK(&se_cmd->cmd_dr.odx_wut_work, _qnap_odx_wut_io_work);
		queue_work(se_dev->dev_dr.odx_wut_wq, &se_cmd->cmd_dr.odx_wut_work);
		return TCM_NO_SENSE;
	}

	return _qnap_odx_do_wut(se_cmd);
}

static sense_reason_t _qnap_odx_pt(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	u8 *cdb = se_cmd->t_task_cdb;
	void *p = NULL;
	sector_t all_nr_blks = 0;
	struct tpc_tpg_data *tpg_p = NULL;
	struct tpc_cmd_data *tc_p = NULL;
	struct odx_work_request odx_wr;
	struct __reg_data tmp_reg_data;
	sense_reason_t reason;
	bool go_pt;

	if (!se_cmd->odx_dr.is_odx_cmd)
		return TCM_UNSUPPORTED_SCSI_OPCODE;

	/* if length is zero, means no data shall be sent, not treat it as error */
	if(get_unaligned_be32(&cdb[10]) == 0) {
		target_complete_cmd(se_cmd, SAM_STAT_GOOD);
		return TCM_NO_SENSE;
	}

	/* we do NOT need to do any io for PT command so not set data_sg_nents,
	 * data_len and data_sg here
	 */	
	p = transport_kmap_data_sg(se_cmd);
	if (!p) {
		reason = TCM_INSUFFICIENT_RESOURCES;
		goto _out;
	}

	/* following to get tpg_p and tc_p again ... */
	tpg_p = qnap_odx_rb_tpg_get(se_cmd->odx_dr.tpg_id_hi, 
						se_cmd->odx_dr.tpg_id_lo);
	if (!tpg_p) {
		reason = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;
	}

	if (tpg_p != se_cmd->odx_dr.odx_tpg) {
		WARN_ON(true);
		qnap_odx_rb_tpg_put(tpg_p);
		reason = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;	
	}

	tmp_reg_data.initiator_id_hi = se_cmd->odx_dr.initiator_id_hi;
	tmp_reg_data.initiator_id_lo = se_cmd->odx_dr.initiator_id_lo;
	tmp_reg_data.tpg_id_hi = se_cmd->odx_dr.tpg_id_hi;
	tmp_reg_data.tpg_id_lo = se_cmd->odx_dr.tpg_id_lo;
	tmp_reg_data.cmd_id_hi = se_cmd->odx_dr.cmd_id_hi;
	tmp_reg_data.cmd_id_lo = se_cmd->odx_dr.cmd_id_lo;
	tmp_reg_data.list_id = se_cmd->odx_dr.list_id;
	tmp_reg_data.sac = se_cmd->odx_dr.sac;
	tmp_reg_data.cmd_type = se_cmd->odx_dr.cmd_type;

	tc_p = qnap_odx_rb_cmd_get(tpg_p, &tmp_reg_data, false, false);
	if (!tc_p) {
		qnap_odx_rb_tpg_put(tpg_p);
		reason = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;
	}	

	if (tc_p != se_cmd->odx_dr.odx_cmd) {
		WARN_ON(true);
		qnap_odx_rb_cmd_put(tc_p);
		qnap_odx_rb_tpg_put(tpg_p);
		reason = TCM_3RD_PARTY_DEVICE_FAILURE;
		goto _out;	
	}

	/* create odx request */
	memset(&odx_wr, 0, sizeof(struct odx_work_request));
	odx_wr.cdb = se_cmd->t_task_cdb;
	odx_wr.odx_rwio_wq = se_dev->dev_dr.odx_rwio_wq;
	odx_wr.buff = p;
	odx_wr.tpg_p = tpg_p;
	odx_wr.tc_p = tc_p;
	odx_wr.se_dev = (void *)se_cmd->se_dev;
	odx_wr.rc = RC_GOOD;
	memcpy(&odx_wr.reg_data, &tc_p->reg_data, sizeof(struct __reg_data));

	/* filter something first */
	if (qnap_odx_core_before_pt(&odx_wr, &go_pt) == 0) {
		if (go_pt)
			qnap_odx_core_pt(&odx_wr);
	}

	qnap_odx_rb_cmd_put(tc_p);
	qnap_odx_rb_tpg_put(tpg_p);

	/* report transfer count here */
	se_cmd->odx_dr.transfer_counts = odx_wr.transfer_counts;

	/* convert RC to TCM_xxx */
	reason = qnap_transport_convert_rc_to_tcm_sense_reason((u32)odx_wr.rc);
_out:
	if (p)
		transport_kunmap_data_sg(se_cmd);

	if (reason == TCM_NO_SENSE)
		target_complete_cmd(se_cmd, SAM_STAT_GOOD);

	return reason;

}

static sense_reason_t _qnap_odx_rrti(
	struct se_cmd *se_cmd
	)
{
	struct tpc_tpg_data *tpg_p = NULL;
	u8 *cdb = se_cmd->t_task_cdb;
	struct odx_work_request odx_wr;
	void *p = NULL;
	int ret;

	if (!se_cmd->odx_dr.is_odx_cmd)
		return TCM_UNSUPPORTED_SCSI_OPCODE;

	/*
	 * SPC4R36, page 428
	 *
	 * The copy manager shall discard the parameter data for the created
	 * ROD tokens:
	 *
	 * a) after all ROD tokens created by a specific copy operation have
	 *    been transferred without errors to the application client
	 * b) if a RECEIVE ROD TOKEN INFORMATION command has been received on
	 *    the same I_T nexus with a matching list id with the
	 *    ALLOCATION LENGTH field set to zero
	 * c) if another a 3rd party command that originates a copy operation
	 *    is received on the same I_T nexus and the list id matches the
	 *    list id associated with the ROD tokens
	 * d) if the copy manager detects a LU reset conditionor I_T nexus loss
	 *    condition or
	 * e) if the copy manager requires the resources used to preserve the
	 *    data
	 *
	 */
	if (get_unaligned_be32(&cdb[10]) < __qnap_odx_get_min_rrti_param_len()) {
		pr_err("%s: allocation length:0x%x < min RRTI param len\n", 
			__func__, get_unaligned_be32(&cdb[10]));
		return TCM_PARAMETER_LIST_LENGTH_ERROR;
	}

	p = transport_kmap_data_sg(se_cmd);
	if (!p)
		return TCM_INSUFFICIENT_RESOURCES;

	tpg_p = qnap_odx_rb_tpg_get(se_cmd->odx_dr.tpg_id_hi, 
						se_cmd->odx_dr.tpg_id_lo);
	if (!tpg_p) {
		transport_kunmap_data_sg(se_cmd);
		return TCM_3RD_PARTY_DEVICE_FAILURE;
	}

	if (tpg_p != se_cmd->odx_dr.odx_tpg) {
		WARN_ON(true);
		qnap_odx_rb_tpg_put(tpg_p);
		transport_kunmap_data_sg(se_cmd);
		return TCM_3RD_PARTY_DEVICE_FAILURE;
	}

	/* create odx request */
	memset(&odx_wr, 0, sizeof(struct odx_work_request));
	odx_wr.cdb = cdb;
	odx_wr.buff = p;
	odx_wr.tpg_p = tpg_p;
	odx_wr.tc_p = NULL;
	odx_wr.rc = RC_GOOD;
	odx_wr.reg_data.initiator_id_hi = se_cmd->odx_dr.initiator_id_hi;
	odx_wr.reg_data.initiator_id_lo = se_cmd->odx_dr.initiator_id_lo;
	odx_wr.reg_data.tpg_id_hi = se_cmd->odx_dr.tpg_id_hi;
	odx_wr.reg_data.tpg_id_lo = se_cmd->odx_dr.tpg_id_lo;
	odx_wr.reg_data.cmd_id_hi = se_cmd->odx_dr.cmd_id_hi;
	odx_wr.reg_data.cmd_id_lo = se_cmd->odx_dr.cmd_id_lo;
	odx_wr.reg_data.list_id = se_cmd->odx_dr.list_id;
	odx_wr.reg_data.sac = se_cmd->odx_dr.sac;

	/* list id in RRTI will be used to track previous ODX cmd (PT or WUT), 
	 * so set cmd type to CP_OP here due to either CP_OP or MONITOR_OP will
	 * be in cmd rb tree
	 */
	odx_wr.reg_data.cmd_type = ODX_CP_OP;	

	ret = qnap_odx_core_rrti(&odx_wr);

	qnap_odx_rb_tpg_put(tpg_p);
	transport_kunmap_data_sg(se_cmd);

	if (ret != 0)
		return qnap_transport_convert_rc_to_tcm_sense_reason((u32)odx_wr.rc);

	target_complete_cmd(se_cmd, SAM_STAT_GOOD);
	return TCM_NO_SENSE;

}

static sense_reason_t _qnap_odx_emulate_evpd_8f(
	struct se_cmd *cmd, 
	unsigned char *buffer
	)
{
	struct se_device *dev = cmd->se_dev;
	u8 *cdb = cmd->t_task_cdb;
	u16 total_len = 0;
	int ret;

	/* For the offload scsicompliance (LOGO) test in HCK, the allocation
	 * len will be 0x1000 but the iscsi initiator only will give 0xff length.
	 * Actually, the 0xff size is too small to return suitable data for
	 * third-party copy command vpd 0x8f
	 */ 
	pr_debug("%s: allocation len (0x%x) in vpd 0x8f\n", __func__, 
					       get_unaligned_be16(&cdb[3]));
	
	/* SPC4R36, page 767 */
	if (get_unaligned_be16(&cdb[3]) < 4) 
		return TCM_INVALID_CDB_FIELD;

	ret = qnap_odx_vpd_emulate_evpd_8f_step1(get_unaligned_be16(&cdb[3]));
	if (ret <= 0)
		return TCM_INVALID_PARAMETER_LIST;

	pr_debug("%s: total len (0x%x) for all descriptors to be built\n", 
				__func__, ret);

	ret = qnap_odx_vpd_emulate_evpd_8f_step2(buffer, dev->dev_attrib.block_size);
	if (ret)
		return TCM_INVALID_PARAMETER_LIST;

	buffer[0] = dev->transport->get_device_type(dev);
	return TCM_NO_SENSE;

}

static int _qnap_odx_it_nexus_discard_all_tokens(
	char *initiator_name,
	u8 *isid_buf,
	u8 *login_ip,
	struct qnap_se_tpg_odx *tpg_odx_dr
	)
{
	struct tpc_tpg_data *tpg_p = NULL;
	char prefix_str[256];
	struct __reg_data reg_data;	
	int ret;

	ret = _qnap_odx_create_initiator_id_with_initiator_name_isid(
			initiator_name, isid_buf, &reg_data.initiator_id_hi, 
			&reg_data.initiator_id_lo);
	if (ret != 0)
		return ret;

	tpg_p = qnap_odx_rb_tpg_get(tpg_odx_dr->tpg_id_hi, tpg_odx_dr->tpg_id_lo);
	if (!tpg_p)
		return -EINVAL;

	reg_data.tpg_id_hi = tpg_odx_dr->tpg_id_hi;
	reg_data.tpg_id_lo = tpg_odx_dr->tpg_id_lo;

	snprintf(prefix_str, sizeof(prefix_str), "[iSCSI][RELEASE CONN] ip: %s", 
			login_ip);

	qnap_odx_rb_discard_all_tokens(tpg_p, &reg_data, 
				DISCARD_TOKEN_I_T_NEXUS_LOSS, prefix_str);

	qnap_odx_rb_tpg_put(tpg_p);
	return 0;

}

static void _qnap_odx_se_cmd_init(struct qnap_se_cmd_odx *odx_dr)
{
	odx_dr->is_odx_cmd = false;
	odx_dr->cmd_id_lo = 0;
	odx_dr->cmd_id_hi = 0;
	odx_dr->tpg_id_lo = 0;
	odx_dr->tpg_id_hi = 0;
	odx_dr->initiator_id_lo = 0;
	odx_dr->initiator_id_hi = 0;
	odx_dr->list_id = 0;
	odx_dr->sac = 0;
	atomic_set(&odx_dr->odx_cmd_count, 1);
	atomic_set(&odx_dr->odx_tpg_count, 1);
}


int qnap_odx_tpg_add_and_get(
	struct se_portal_group *se_tpg
	)
{
	int ret;

	if (se_tpg->proto_id < 0)
		return -EINVAL;

	ret = _qnap_odx_create_tpg_id(se_tpg, &se_tpg->odx_dr.tpg_id_hi, 
			&se_tpg->odx_dr.tpg_id_lo);
	if (ret != 0)
		return ret;

	se_tpg->odx_dr.odx_tpg = qnap_odx_rb_tpg_add_and_get(
			se_tpg->odx_dr.tpg_id_hi, se_tpg->odx_dr.tpg_id_lo);

	if (!se_tpg->odx_dr.odx_tpg) {
		pr_warn("%s: fail to get odx_tpg\n", __func__);
		return -ENODEV;
	}

	pr_debug("%s: odx_tpg:0x%p, id(hi):0x%llx, id(lo):0x%llx\n",  
		__func__, se_tpg->odx_dr.odx_tpg, 
		(unsigned long long)se_tpg->odx_dr.tpg_id_hi,
		(unsigned long long)se_tpg->odx_dr.tpg_id_lo);
	return 0;

}

void qnap_odx_tpg_del(
	struct se_portal_group *se_tpg
	)
{
	int count;

	if (!se_tpg->odx_dr.odx_tpg || (se_tpg->proto_id < 0))
		return;

	do {
		count = qnap_odx_rb_tpg_put(se_tpg->odx_dr.odx_tpg);
		WARN_ON(count < 0);
		
		if (count == 1)
			break;

		pr_debug("%s: odx (count: %d) may still works on tpg\n", 
				__func__, count);

		/* just follow core_tpg_deregister() ... :( */
		cpu_relax();
	} while(1);

	qnap_odx_rb_tpg_del(se_tpg->odx_dr.odx_tpg);

}

void qnap_odx_free_cmd(struct se_cmd *se_cmd)
{
	struct tpc_cmd_data *tc_p = NULL;

	if (se_cmd->odx_dr.is_odx_cmd && se_cmd->odx_dr.odx_cmd) {
		tc_p = (struct tpc_cmd_data *)se_cmd->odx_dr.odx_cmd;
		se_cmd->odx_dr.odx_cmd = NULL;
		_qnap_odx_cmd_put_and_del(se_cmd, tc_p);
		qnap_odx_rb_cmd_free(tc_p);
	}
}

#if 0
static void __qnap_odx_ask_to_drop(
	struct se_cmd *se_cmd,
	int type
	)
{
	struct tpc_cmd_data *tp_data = NULL;

	if (!se_cmd->odx_dr.is_odx_cmd)
		return;

	if (!se_cmd->odx_dr.odx_cmd)
		return;

	tp_data = (struct tpc_cmd_data *)se_cmd->odx_dr.odx_cmd;


	switch(type) {
	case -1:
		/* RELEASE CONN */
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_RELEASE_CONN);
		break;
	case TMR_ABORT_TASK:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_ABORT_TASK);
		break;
	case TMR_ABORT_TASK_SET:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_ABORT_TASK_SET);
		break;
	case TMR_CLEAR_ACA:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_CLEAR_ACA);
		break;
	case TMR_CLEAR_TASK_SET:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_CLEAR_TASK_SET);
		break;
	case TMR_LUN_RESET:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_LUN_RESET);
		break;
	case TMR_TARGET_WARM_RESET:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_TARGET_WARM_RESET);
		break;
	case TMR_TARGET_COLD_RESET:
		atomic_set(&tp_data->cmd_asked, CMD_ASKED_BY_TARGET_COLD_RESET);
		break;
	default:
		break;
	}

	return;
}

void qnap_odx_drop_cmd(
	struct se_cmd *se_cmd,
	int type
	)
{
	__qnap_odx_ask_to_drop(se_cmd, type);
}
#endif


#if 0
static int __qnap_odx_discard_all_tokens_on_dev(
	struct se_device *se_dev,
	struct se_cmd *se_cmd,
	struct se_portal_group *se_tpg
	)
{
	struct qnap_se_dev_dr *dr = &se_dev->dev_dr;
	struct tpc_tpg_data *tpg_p = NULL;	
	struct __reg_data reg_data;
	struct __dev_info *dev_info = &reg_data.dev_info;
	char msg_str[256];
	int ret;	

	/* hmmmm ... we only want NAA to identify the device is we want or not */
	tpg_p = odx_rb_tpg_get(se_tpg->odx_dr.tpg_id_hi, se_tpg->odx_dr.tpg_id_lo);
	if (!tpg_p)
		return -EINVAL;

	qlib_get_naa_6h_code((void *)se_dev, &dr->dev_naa[0], &dev_info->naa[0],
		qnap_transport_parse_naa_6h_vendor_specific);

	snprintf(msg_str, sizeof(msg_str), "[iSCSI][LUN RESET] ip: %pISc", 
		se_cmd->se_tfo->get_login_ip(se_cmd));

	odx_rb_discard_all_tokens(tpg_p, &reg_data, DISCARD_TOKEN_LUN_RESET, msg_str);

	odx_rb_tpg_put(tpg_p);
	return 0;
}

int qnap_tmr_odx_discard_all_tokens(
	struct se_device *se_dev,
	struct se_cmd *se_cmd, 
	struct se_portal_group *se_tpg
	)
{
	return __qnap_odx_discard_all_tokens_on_dev(se_dev, se_cmd, se_tpg);
}
#endif

int qnap_odx_pre_check_odx(struct se_cmd *cmd)
{
	/* this function will be put in target_execute_cmd() */

	struct se_device *dev = cmd->se_dev;
	u32 list_id;

	if (!__qnap_odx_is_odx_opcode(cmd->t_task_cdb) 
		|| __qnap_odx_get_list_id_by_cdb(cmd->t_task_cdb, &list_id)
	)
		return -ENOTSUPP;

	if (!cmd->se_lun || !cmd->se_lun->lun_tpg)
		return -EINVAL;

	/* SPC4R36, page 240
	 *
	 * Unless otherwise specificed, the list id is the uniquely
	 * identifies a copy operation among all those being processed
	 * that were received on a specific I_T nexus. If the copy 
	 * manager detects a duplicate list identifier value,then the
	 * originating 3rd copy command shall be terminated with 
	 * CHECK CONDITION status,with the sense key set to 
	 * ILLEGAL REQUEST, and the ASC set to OPERATION IN PROGRESS
	 */
	if (_qnap_is_odx_in_progress(cmd, list_id) == -EBUSY)
		return -EBUSY;

	/* for WRITE behavior command, if their (lba, nr_blks) overlay the
	 * token range, the token must be cancel
	 */
	_qnap_cancel_odx_token(cmd);
	return 0;

}

struct workqueue_struct *qnap_odx_alloc_rwio_wq(char *name, int idx)
{
	return alloc_workqueue("odxrwio_%s%d",
			(WQ_HIGHPRI |WQ_SYSFS | WQ_MEM_RECLAIM | WQ_UNBOUND), 0, 
			name, idx);
}

void qnap_odx_destroy_rwio_wq(struct workqueue_struct *wq)
{
	if (wq) {
		flush_workqueue(wq);
		destroy_workqueue(wq);
	}
}

struct workqueue_struct *qnap_odx_alloc_wut_io_wq(
	char *name,
	int idx
	)
{
	return alloc_workqueue("odxwut_%s%d",
			(WQ_HIGHPRI |WQ_SYSFS | WQ_MEM_RECLAIM | WQ_UNBOUND), 0, 
			name, idx);
}

void qnap_odx_destroy_wut_io_wq(
	struct workqueue_struct *wq
	)
{
	if (wq) {
		flush_workqueue(wq);
		destroy_workqueue(wq);
	}
}

int qnap_odx_alloc_cache_res(void)
{
	odx_tpg_data_cache = kmem_cache_create("odx_tpg_data_cache", 
			sizeof(struct tpc_tpg_data),
			__alignof__(struct tpc_tpg_data), 0, NULL);

	if (!odx_tpg_data_cache) {
		pr_err("unable to create odx_tpg_data_cache\n");
		goto _exit_;
	}

	odx_cmd_data_cache = kmem_cache_create("odx_cmd_data_cache", 
			sizeof(struct tpc_cmd_data),
			__alignof__(struct tpc_cmd_data), 0, NULL);

	if (!odx_cmd_data_cache) {
		pr_err("unable to create odx_cmd_data_cache\n");
		goto _exit_;
	}

	odx_token_data_cache = kmem_cache_create("odx_token_data_cache", 
			sizeof(struct tpc_token_data),
			__alignof__(struct tpc_token_data), 0, NULL);

	if (!odx_token_data_cache) {
		pr_err("unable to create odx_token_data_cache\n");
		goto _exit_;
	}

	token512b_cache = kmem_cache_create("token512b_cache",
			sizeof(struct rod_token_512b),
			__alignof__(struct rod_token_512b), 0, NULL);

	if (!token512b_cache) {
		pr_err("unable to create token512b_cache\n");
		goto _exit_;
	}


	odx_br_cache = kmem_cache_create("odx_br_cache",
		sizeof(struct blk_range_data),
		__alignof__(struct blk_range_data), 0, NULL);

	if (!odx_br_cache) {
		pr_err("unable to create odx_br_cache\n");
		goto _exit_;
	}

	return 0;

_exit_:
	qnap_odx_free_cache_res();

	return -ENOMEM;

}

void qnap_odx_free_cache_res(void)
{
	if (odx_tpg_data_cache)
		kmem_cache_destroy(odx_tpg_data_cache);

	if (odx_cmd_data_cache)
		kmem_cache_destroy(odx_cmd_data_cache);

	if (odx_token_data_cache)
		kmem_cache_destroy(odx_token_data_cache);

	if (token512b_cache)
		kmem_cache_destroy(token512b_cache);

	if (odx_br_cache)
		kmem_cache_destroy(odx_br_cache);
}

int qnap_odx_set_scsi_sense_information(
	struct se_cmd *cmd
	)
{
	u8 *cdb = cmd->t_task_cdb;
	int ret = -ENOTSUPP;

	/* report transfer count to INFORMATION filed for ODX command */
	if ((cdb[0] == EXTENDED_COPY)
		&& (IS_TPC_SCSI_PT_OP(cdb[1]) || IS_TPC_SCSI_WUT_OP(cdb[1])))
	{
		pr_debug("%s: report transfer_counts:0x%llx to Information Field\n",
			__func__, (unsigned long long)cmd->odx_dr.transfer_counts);
		ret = scsi_set_sense_information(cmd->sense_buffer, 
				cmd->scsi_sense_length, cmd->odx_dr.transfer_counts);
		if (ret)
			ret = -EINVAL;
	}
	return ret;
}

/**/
static struct qnap_odx_tf_ops qnap_odx_tf_ops_table = {
	.it_nexus_discard_all_tokens = _qnap_odx_it_nexus_discard_all_tokens,
};

void qnap_odx_transport_init(
	struct qnap_se_cmd_odx *odx_dr,
	struct qnap_tf_ops *q_tf_ops
	)
{
	_qnap_odx_se_cmd_init(odx_dr);

	/* remember the tfo may NOT contain YOUR own interface (i.e. xcopy code) */
	if (q_tf_ops)
		q_tf_ops->odx_tf_ops = &qnap_odx_tf_ops_table;
}

void qnap_odx_setup_sbc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	cmd_dr->sbc_ops.sbc_odx_ops.sbc_odx_pt = _qnap_odx_pt;
	cmd_dr->sbc_ops.sbc_odx_ops.sbc_odx_wut = _qnap_odx_wut;
	cmd_dr->sbc_ops.sbc_odx_ops.sbc_odx_rrti = _qnap_odx_rrti;
}

void qnap_odx_setup_spc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	cmd_dr->spc_ops.spc_odx_ops.sbc_evpd_8f = _qnap_odx_emulate_evpd_8f;
}
