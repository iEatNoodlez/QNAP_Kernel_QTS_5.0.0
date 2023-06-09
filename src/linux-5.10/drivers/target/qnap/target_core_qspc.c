/*******************************************************************************
 * Filename:  target_core_qspc.c
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
	 
#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_backend.h>
#include <qnap/fbdisk.h>

#include "../target_core_iblock.h"
#include "../target_core_file.h"
#include "../target_core_internal.h"
#include "../target_core_ua.h"

#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "target_core_qsbc.h"
#include "target_core_qspc.h"
#include "target_core_qodx.h"
#include "../iscsi/qnap/iscsi_target_qlog.h"

/**/
static int __check_mode_page_data_answer_from_modesense(u8 *p)
{
	int i, invalid = 0;
	
	for (i = 3; i < 20; i++) {
		if (i != 12) {
			if (p[i] != 0x0)
				invalid = 1;
		} else {
			if (p[i] != 0x20)
				invalid = 1;
		}
		if (invalid)
			break;
	}
	return invalid;
}

static void __dm_monitor_callback_fn(
	void *priv, 
	int dmstate
	)
{
	struct qnap_se_dev_attr_dr *dev_attr_dr = NULL;

	/* to prevent thin-lun and thin-pool both be removed */
	if (!priv)
		return;

	dev_attr_dr = (struct qnap_se_dev_attr_dr *)priv;

	if (dmstate == 1)
		dev_attr_dr->gti = NULL;
	return;
}

static inline int __file_get_total_and_used(
	struct file *file,
	loff_t *total_bytes,
	loff_t *used_blks
	)
{
	struct inode *inode = file_inode(file);

	*total_bytes = inode->i_size;
	/* this unit is 512b */
	*used_blks = inode->i_blocks;
	return 0;	
}

static int __file_sync_get_total_and_used(
	struct file *file,
	loff_t *total_bytes,
	loff_t *used_blks
	)
{
	int ret;

	ret = vfs_fsync(file, 1);
	if (unlikely(ret)) {
		pr_err("%s: file: %p, fail to call vfs_fsync, ret: %d\n", 
				__func__, file, ret);
	}

	__file_get_total_and_used(file, total_bytes, used_blks);
	return 0;	
}

static void _qnap_sbc_evpd_b2_build_provisioning_group_desc(
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

static int _fd_blk_spc_logsense_lbp_get_ac_and_uc(
	struct se_device *se_dev,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &se_dev->dev_attrib.dev_attr_dr;
	int ret, bs_order = ilog2(se_dev->dev_attrib.block_size);
	sector_t total, used;
	u64 __total_blks = 0, __used_blks = 0;
	unsigned long long max_blocks = se_dev->transport->get_blocks(se_dev);

	*ret_avail_blks = 0;
	*ret_used_blks = 0;

	if (!strlen(se_dev->udev_path)) {
		pr_err("%s: udev_path is empty\n", __func__);
		return -ENODEV;
	}

	/* we declare it to be weak symbol ... */
	if (!thin_get_dmtarget || !thin_set_dm_monitor || !thin_get_data_status) {
		pr_err("thin_get_dmtarget() or thin_set_dm_monitor() "
			"or thin_get_data_status() funcs not supported "
			"by dm-thin layer, please check them\n");
		return -EOPNOTSUPP;
	}

	if (!dev_attr_dr->gti) {
		/* try to get dm target and set the dm monitor */
		ret = thin_get_dmtarget(se_dev->udev_path, 
				(struct dm_target **)&dev_attr_dr->gti);
	} else
		ret = 0;

	if (ret != 0) {
		pr_err("%s: fail to call thin_get_dmtarget()\n", __func__);
		return ret;
	}

	thin_set_dm_monitor((struct dm_target *)dev_attr_dr->gti, 
					dev_attr_dr, __dm_monitor_callback_fn);

	qlib_fd_flush_and_truncate_cache(fd_dev->fd_file, 0, max_blocks, bs_order, 
					false, qlib_thin_lun(&se_dev->dev_dr));

	/* start get data status, the unit for __total_blks and __used_blks is 512 bytes */
	ret = thin_get_data_status(dev_attr_dr->gti, &__total_blks, &__used_blks);
	if (ret != 0) {
		pr_err("%s: fail to call thin_get_data_status()\n", __func__);
		return ret;
	}

	total = (((sector_t)__total_blks << 9) >> bs_order);
	used = (((sector_t)__used_blks << 9) >> bs_order);

	*ret_used_blks = used;
	*ret_avail_blks = total - used;

	return 0;

}

static int _fd_file_spc_logsense_lbp_get_ac_and_uc(
	struct se_device *se_dev,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)
{
	struct fd_dev *fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
	int bs_order = ilog2(se_dev->dev_attrib.block_size);
	loff_t __total_bytes, __used_blks;
	sector_t total, used;
	int ret;

	*ret_avail_blks = 0;
	*ret_used_blks = 0;

	__file_sync_get_total_and_used(fd_dev->fd_file, &__total_bytes, 
			&__used_blks);

	total = ((sector_t)__total_bytes >> bs_order);
	used = (((sector_t)__used_blks << 9) >> bs_order);

	*ret_used_blks = used;
	*ret_avail_blks = total - used;
	return 0;
}

static int _ib_fbdisk_spc_logsense_lbp_get_ac_and_uc(
	struct se_device *se_dev,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)
{
	struct iblock_dev *ibd = NULL;
	struct block_device *bd = NULL;
	struct fbdisk_device *fb_dev = NULL;
	struct fbdisk_file *fb_file = NULL;
	loff_t __total_bytes = 0, __used_blks = 0;
	loff_t tmp_total_bytes = 0, tmp_used_blks = 0;
	sector_t total, used;
	int i, bs_order = ilog2(se_dev->dev_attrib.block_size);

	ibd = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
	bd = ibd->ibd_bd;

	if (!bd->bd_disk || !bd->bd_disk->private_data)
		return -ENODEV;

	fb_dev = bd->bd_disk->private_data;

	if (!fb_dev->fb_file_num)
		return -ENODEV;

	/* to flush first before to get information from file inode */
	blkdev_issue_flush(bd, GFP_KERNEL);

	for (i = 0; i < fb_dev->fb_file_num; i++) {
		fb_file = &fb_dev->fb_backing_files_ary[i];
		__file_get_total_and_used(fb_file->fb_backing_file, 
				&tmp_total_bytes, &tmp_used_blks);

		__total_bytes += tmp_total_bytes;
		__used_blks += tmp_used_blks;
	}

	total = ((sector_t)__total_bytes >> bs_order);
	used = (((sector_t)__used_blks << 9) >> bs_order);

	*ret_used_blks = used;
	*ret_avail_blks = total - used;
	return 0;

}

static int _ib_blk_spc_logsense_lbp_get_ac_and_uc(
	struct se_device *se_dev,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)
{
	return -EOPNOTSUPP;
}

static int _qnap_spc_logsense_lbp_get_ac_and_uc(
	struct se_cmd *cmd,
	u32 *ac,
	u32 *uc
	)
{
	struct se_device *se_dev = cmd->se_dev;
	struct qnap_se_dev_dr *dev_dr = &se_dev->dev_dr;
	sector_t avail_blks, used_blks;
	int ret, threshold_exp;

	*ac = 0, *uc = 0;

	/* only support this command for thin lun */
	if (!qlib_thin_lun(&se_dev->dev_dr))
		return -EOPNOTSUPP;

	ret = qnap_spc_get_ac_and_uc(se_dev, &avail_blks, &used_blks);
	if (!ret) {
		threshold_exp = cmd->cmd_dr.sbc_ops.sbc_get_threshold_exp(se_dev);
		*ac = (u32)div_u64(avail_blks, (1 << threshold_exp));
		*uc = (u32)div_u64(used_blks, (1 << threshold_exp));

		pr_info("%s: threshold_exp:%d, avail_blks:0x%llx, "
			"used_blks:0x%llx\n", __func__, threshold_exp,
			(unsigned long long)avail_blks,
			(unsigned long long)used_blks);
	}

	return ret;

}

int qnap_transport_notify_ua_to_other_it_nexus(
	struct se_cmd *se_cmd,
	u8 asc, 
	u8 ascq	
	)
{
	unsigned long flags;
	struct se_session *se_sess;
	struct se_portal_group *se_tpg;
	struct se_node_acl *se_acl;
	struct se_dev_entry *se_deve;
	unsigned char isid_buf[PR_REG_ISID_LEN];

	if (!se_cmd->se_sess)
		return -EINVAL;

	se_sess = se_cmd->se_sess;
	if (!se_sess->se_tpg)
		return -EINVAL;

	se_tpg = se_sess->se_tpg;

	mutex_lock(&se_tpg->acl_node_mutex);
	list_for_each_entry(se_acl, &se_tpg->acl_node_list, acl_list) {

		spin_lock_irqsave(&se_acl->nacl_sess_lock, flags);

		if (!strncasecmp(se_acl->initiatorname, DEFAULT_INITIATOR, 
			sizeof(DEFAULT_INITIATOR)))
		{
			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;		
		}

		if (!strncasecmp(se_acl->initiatorname, FC_DEFAULT_INITIATOR,
			sizeof(FC_DEFAULT_INITIATOR)))
		{
			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;
		}

		if (!se_acl->nacl_sess) {
			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;
		}

		/* skip it_nexus received the mode select command */
		if (se_sess == se_acl->nacl_sess) {
			pr_debug("found sess as cmd issuer, skip it..., "
				"node:%s, isid:%s\n", 
				se_acl->initiatorname, isid_buf);

			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;
		}

		mutex_lock(&se_acl->lun_entry_mutex);
		se_deve = target_nacl_find_deve(se_acl, se_cmd->se_lun->unpacked_lun);
		if (!se_deve) {
			mutex_unlock(&se_acl->lun_entry_mutex);
			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;
		}

		if (se_deve->lun_access_ro) {
			mutex_unlock(&se_acl->lun_entry_mutex);
			spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
			continue;
		}

		mutex_unlock(&se_acl->lun_entry_mutex);
		spin_unlock_irqrestore(&se_acl->nacl_sess_lock, flags);
		mutex_unlock(&se_tpg->acl_node_mutex);


		if (se_tpg->se_tpg_tfo->sess_get_initiator_sid != NULL) {
			memset(&isid_buf[0], 0, PR_REG_ISID_LEN);
			se_tpg->se_tpg_tfo->sess_get_initiator_sid(
				se_acl->nacl_sess, &isid_buf[0], PR_REG_ISID_LEN);
		}


		/* find the it_nexus we want ... */
		pr_info("alloc UA (asc:0x%x, ascq:0x%x) for lun:%llu, "
			"on node:%s, isid:%s\n", asc, ascq, 
			se_deve->mapped_lun, se_acl->initiatorname, isid_buf);

		core_scsi3_ua_allocate(se_deve, asc, ascq);

		mutex_lock(&se_tpg->acl_node_mutex);
	}
	mutex_unlock(&se_tpg->acl_node_mutex);
	return 0;
}

static int _qnap_spc_modeselect_caching(
	struct se_cmd *se_cmd, 
	u8 sp,
	u8 *p
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	struct fd_dev *fd_dev = NULL;
	struct iblock_dev *ib_dev = NULL;
	bool got_qnap_dev_type = true;
	int wce, i, ret = 0;

	if (!se_dev->transport->set_write_cache) {
		pr_warn("not support mode select - caching mode page\n");
		return -ENOTSUPP;
	}

	if (p[1] != 0x12)
		return -EINVAL;

	wce = ((p[2] & 0x4) ? 1: 0);

	if (target_check_wce(se_dev) == (bool)wce)
		return 0;

	/* Do not support changing WCE while V_SUP is 0 */
	if(!qnap_check_v_sup(se_dev))
		return -EINVAL;

	/* TBD: 
	 * hmmm ... here will do something checking , actually, we don't want 
	 * to update others except the wce bit (it shall depend on the answer
	 * from mode sense command for caching modepage)
	 */
	if (__check_mode_page_data_answer_from_modesense(p)) {
		pr_err("mode page data contents not same as answer from"
			" mode sense - caching mode page\n");
		return -EINVAL;
	}

	if ((target_check_wce(se_dev) == true) && (wce == 0)) {
		/* we shall flush cache here if wce bit becomes from 1 to 0 
		 * sbc3r35j, p265
		 */
		if (!qlib_is_fio_blk_dev(&se_dev->dev_dr)
				|| !qlib_is_fio_file_dev(&se_dev->dev_dr))
		{
			fd_dev = (struct fd_dev *)se_dev->transport->get_dev(se_dev);
			ret = vfs_fsync(fd_dev->fd_file, 1);

		} else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)
				|| !qlib_is_ib_blk_dev(&se_dev->dev_dr))
		{
			ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
			ret = blkdev_issue_flush(ib_dev->ibd_bd, GFP_KERNEL);

		} else {
			got_qnap_dev_type = false;
			ret = -ENODEV;
			pr_warn("%s: unsupported qnap dev type\n");			
		}

		if (got_qnap_dev_type) {
			if (ret != 0) {
				pr_warn("%s: fail to flush when WCE bit (1->0), "
					"ret: %d\n", ret);
			}
		}

	}

	if (got_qnap_dev_type) {
		se_dev->transport->set_write_cache(se_dev, (bool)wce);
		pr_info("%s: %s WCE bit\n", __func__, 
					((wce == 1) ? "enable": "disable"));
		ret = 0;
	}

#if 0 //shall be checked 

	/* ack MODE PARAMETERS CHANGED via UA to other I_T nexuses */
	qnap_transport_notify_ua_to_other_it_nexus(se_cmd, 0x2A,
		ASCQ_2AH_MODE_PARAMETERS_CHANGED);
#endif
	return ret;
}

static struct {
	uint8_t		page;
	uint8_t		subpage;
	int		(*emulate)(struct se_cmd *, u8, unsigned char *);
} modeselect_handlers[] = {
	{ .page = 0x08, .subpage = 0x00, .emulate = _qnap_spc_modeselect_caching },
};

static sense_reason_t do_qnap_spc_modeselect(
	struct se_cmd *se_cmd
	)
{
	struct se_device *se_dev = se_cmd->se_dev;
	char *cdb = se_cmd->t_task_cdb;
	bool ten = cdb[0] == MODE_SELECT_10;
	int dev_type, llba_bit, blk_desc_len;
	u8 page, subpage;
	unsigned char *buf, *mode_pdata;
	sense_reason_t reason = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
	int i, ret;

	if (!se_cmd->data_length) {
		target_complete_cmd(se_cmd, GOOD);
		return TCM_NO_SENSE;
	}

	/* PF (page format)
	 * 0 - all parameters after the block descriptors are vendor specific
	 * 1 - all parameters after header and block descriptors are structured
	 *     as pages of related parameters
	 */
	if (!(cdb[1] & 0x10))
		return TCM_INVALID_CDB_FIELD;

	buf = transport_kmap_data_sg(se_cmd);
	if (!buf)
		return TCM_OUT_OF_RESOURCES;

	/* spc4r37a, sectio 7.5.4 mode parameter list format */
	if (ten) {
		dev_type = buf[2];
		llba_bit = (buf[4] & 1);
		blk_desc_len = get_unaligned_be16(&buf[6]);
	} else {
		dev_type = buf[1];
		llba_bit = 0;
		blk_desc_len = buf[3];
	}

	/* 1. for MODE SELECT command, the MODE_DATA_LENGTH shall be reserved 
	 * 2. check the medium type (shall be 00h for sbc)
	 * 3. for device-specific parameter (WP bit and DPOFUA bit shall be
	 *    ignored and reserved for MODE SELECT command)
	 */
	if ((se_dev->transport->get_device_type(se_dev) != TYPE_DISK)
	|| (dev_type != TYPE_DISK)
	)
	{
		pr_err("%s: medium type is not correct\n", __func__);
		transport_kunmap_data_sg(se_cmd);
		return TCM_INVALID_PARAMETER_LIST;
	}

	/* TBD: if block descriptor length is 0, it is not error condition */
	if (blk_desc_len){
		/* we do not support to change any value reported in block descriptor. 
		 * So to ignore it
		 */
	} 

	if (ten)
		mode_pdata = &buf[8 + blk_desc_len];
	else
		mode_pdata = &buf[4 + blk_desc_len];

	/* spc4r37a, section 7.5.7 */
	page = mode_pdata[0] & 0x3f;
	subpage = mode_pdata[0] & 0x40 ? mode_pdata[1] : 0;

	for (i = 0; i < ARRAY_SIZE(modeselect_handlers); i++) {
		if (modeselect_handlers[i].page == page &&
		    modeselect_handlers[i].subpage == subpage) {
			ret = modeselect_handlers[i].emulate(se_cmd, 
				(cdb[1] & 0x01), mode_pdata);

			if (ret == -EINVAL)
				reason = TCM_INVALID_PARAMETER_LIST;
			goto out;
		}
	}

	ret = -EINVAL;
	reason = TCM_UNKNOWN_MODE_PAGE;
out:
	transport_kunmap_data_sg(se_cmd);

	if (!ret) {
		reason = TCM_NO_SENSE;
		target_complete_cmd(se_cmd, GOOD);
	}

	return reason;
}

static int _qnap_spc_logsense_lbp(
	struct se_cmd *se_cmd, 
	u8 *buf
	)
{
	LBP_LOG_PARAMETER_FORMAT *format = NULL;
	u16 off = 4, len = 0;
	u32 avail = 0, used = 0;
	struct se_device *se_dev = se_cmd->se_dev;
	int ret = 0, tmp_len = 4;

	/* currently, we support
	 * (1) Available LBA Mapping Resource count log parameter
	 * (2) and, Used LBA Mapping Resource count log parameter
	 */
	len = (2 * sizeof(LBP_LOG_PARAMETER_FORMAT));

	/* sbc3r35j, page 244
	 * Logical Block Provisioning log page Header (4-bytes) 
	 */
	buf[0] = (0x0c | 0x80); /* set= SPF bit (bit 6) to 0, DS bit (bit 7) to 1 */
	buf[1] = 0x00;

	if (se_cmd->data_length < (4 + len))
		ret = -EINVAL;

	if (_qnap_spc_logsense_lbp_get_ac_and_uc(se_cmd, &avail, &used)) {
		pr_warn("%s: fail to get avail/used res count\n", __func__);
		ret = -EINVAL;
	}

	if (ret == -EINVAL) {
		/* only return 4 bytes if got any error */
		put_unaligned_be16(tmp_len, &buf[2]);	    
		return tmp_len;
	}
	
	/* Available LBA Mapping Resource count log parameter format */
	format = (LBP_LOG_PARAMETER_FORMAT *)&buf[off];
	format->parameter_code[0] = (0x0001 >> 8) & 0xff;
	format->parameter_code[1] = 0x0001 & 0xff;
	format->du = 0;
	format->tsd = 1;
	format->etc = 0;
	format->tmc = 0;
	format->format_and_linking = 3;
	format->parameter_length = 0x8;
	format->resource_count[0] = (avail >> 24 ) & 0xff;
	format->resource_count[1] = (avail >> 16 ) & 0xff;
	format->resource_count[2] = (avail >> 8 ) & 0xff;
	format->resource_count[3] = avail  & 0xff;

	/* set to 10b to indicate the RESOURCE COUNT field may or may not be
	 * dedicated to any logical unit including the addressed logical unit.
	 * Usage of resources on other logical units may impact the resource count
	 */
	format->scope = 2;

	/* Used LBA Mapping Resource count log parameter */
	off += 12;
	format  = (LBP_LOG_PARAMETER_FORMAT *)&buf[off];
	format->parameter_code[0] = (0x0002 >> 8) & 0xff;
	format->parameter_code[1] = 0x0002 & 0xff;
	format->du = 0;
	format->tsd = 1;
	format->etc = 0;
	format->tmc = 0;
	format->format_and_linking = 3;
	format->parameter_length = 0x8;
	format->resource_count[0] = (used >> 24 ) & 0xff;
	format->resource_count[1] = (used >> 16 ) & 0xff;
	format->resource_count[2] = (used >> 8 ) & 0xff;
	format->resource_count[3] = used	& 0xff;

	/* set to 01b to indicate the RESOURCE COUNT field is dedicated to the
	 * logical unit. Usage of resources on other logical units does not
	 * impact the resource count
	 */
	format->scope = 1;
	
	put_unaligned_be16(len, &buf[2]);
	return (4 + len);

}

LOGSENSE_FUNC_TABLE g_logsense_table[] ={
    {0xc, 0x0, _qnap_spc_logsense_lbp, 0x0},
    {0x0, 0x0, NULL, 0x1},
};

static sense_reason_t do_qnap_spc_logsense(
	struct se_cmd *se_cmd
	)
{
	u8 *rbuf = NULL, *data_buf = NULL;
	u8 pagecode, sub_pagecode;
	int length = 0, i;
	sense_reason_t reason;

	pagecode = (se_cmd->t_task_cdb[2] & 0x3f);
	sub_pagecode = se_cmd->t_task_cdb[3];

	if (!se_cmd->data_length){
		target_complete_cmd(se_cmd, GOOD);
		return TCM_NO_SENSE;
	}

	/* spc4r37a , p373
	 * sp bit (save parameters bit) set to one specifies that device server
	 * shall perform the specified LOG SENSE command and save all log
	 * parameters as saveavle by DS bit to a nonvolatile, vendor specific
	 * location.
	 * If sp bit set to one and LU doesn't implement saveing log parameters,
	 * the device shall terminate the command with CHECK CONDITION status 
	 * with the sense key set to ILLEGAL REQUEST, and the additinal sense
	 * code set to INVALID FIELD IN CDB
	 */
	if (se_cmd->t_task_cdb[1] & 0x1) {
		reason = TCM_INVALID_CDB_FIELD;
		goto _exit_;
	}

	/* at least, we need 4 bytes even not report any log parameter */
	if (se_cmd->data_length < 4){
		reason = TCM_INVALID_CDB_FIELD;
 		goto _exit_;
	}

	rbuf = transport_kmap_data_sg(se_cmd);
	if (!rbuf){
		reason = TCM_OUT_OF_RESOURCES;
		goto _exit_;
	}

	data_buf = kmalloc(se_cmd->data_length, GFP_KERNEL);
	if (!data_buf){
		reason = TCM_OUT_OF_RESOURCES;
		goto _exit_;
	}

	/* spec4r37a, page374
	 * If the log page specified by the page code and subpage code
	 * combination is reserved or not implemented, then the device server
	 * shall terminate the command with CHECK CONDITION status with the
	 * sense key set to ILLEGAL REQUEST, and the additinal sense code set
	 * to INVALID FIELD IN CDB
	 */
	reason = TCM_INVALID_CDB_FIELD;
	memset(data_buf, 0, se_cmd->data_length);

	for (i = 0; i < ARRAY_SIZE(g_logsense_table); i++){
		if ((g_logsense_table[i].end != 0x1) 
		&& (g_logsense_table[i].logsense_func != NULL)
		&& (g_logsense_table[i].page_code == pagecode)
		&& (g_logsense_table[i].sub_page_code == sub_pagecode)
		)
		{
			length = g_logsense_table[i].logsense_func(se_cmd, 
				&data_buf[0]);

			if (length != 0){
				reason = TCM_NO_SENSE;
				memcpy(rbuf, data_buf, length);
				target_complete_cmd_with_length(se_cmd, 
					GOOD, length);
			}
			break;
		}
	}

_exit_:
	if (data_buf)
		kfree(data_buf);

	if (rbuf)
		transport_kunmap_data_sg(se_cmd);

	return reason;
}

static sense_reason_t do_qnap_spc_modesense_lbp(
	struct se_cmd *cmd,
	u8 pc, 
	unsigned char *p
	)
{
	struct se_device *dev = cmd->se_dev;
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;
	struct qnap_se_dev_attr_dr *dev_attr_dr = &dev->dev_attrib.dev_attr_dr;
	unsigned long long total_blocks = (dev->transport->get_blocks(dev) + 1);
	THRESHOLD_DESC_FORMAT *desc = NULL;
	u16 off = 16, len = 0;
	u64 dividend;
	u32 threshold_count, threshold_exp;

	if ((cmd->data_length == 0) || (off > (u16)cmd->data_length) 
			|| !dev_attr_dr->tp_threshold_percent)
		return 0;
	
	if (!cmd->cmd_dr.sbc_ops.sbc_get_threshold_exp)
		return 0;

	/* TODO: need to check the formula */
	dividend = (total_blocks * dev_attr_dr->tp_threshold_percent);
	dividend = div_u64(dividend, 100);
	
	threshold_exp = cmd->cmd_dr.sbc_ops.sbc_get_threshold_exp(dev);
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

static sense_reason_t do_qnap_spc_modesense_caching(
	struct se_cmd *cmd,
	u8 pc, 
	unsigned char *p
	)
{
	struct se_device *dev = cmd->se_dev;

	p[0] = 0x08;
	p[1] = 0x12;

	/* Do not support changing WCE while V_SUP is 0 */
	if(!qnap_check_v_sup(dev))
		goto out_1;
	
	/* PC (page controal)
	 * 1 - changeable value
	 */
	if (pc == 1) {
		/* TODO: 
		 * this shall be checked, what we can do in modeselect (page caching),
		 * then what we do here
		 */
		if (!qlib_is_fio_blk_dev(&dev->dev_dr) 
				|| !qlib_is_fio_file_dev(&dev->dev_dr)
				|| !qlib_is_ib_fbdisk_dev(&dev->dev_dr)
				|| !qlib_is_ib_blk_dev(&dev->dev_dr))
			p[2] = 0x04;
		goto out;
	}
	
	if (target_check_wce(cmd->se_dev))
		p[2] = 0x04; /* Write Cache Enable */
out_1:
	p[12] = 0x20; /* Disabled Read Ahead */
out:
	return 20;

}

static sense_reason_t do_qnap_spc_evpd_b2(
	struct se_cmd *cmd, 
	u8 *buf
	)
{
	/* Take care this !!
	 * following code depends on (refer from) native code (spc_emulate_evpd_b2)
	 */
	struct se_device *dev = cmd->se_dev;
	struct qnap_se_dev_dr *dev_dr = &dev->dev_dr;
	bool qnap_thin_setting = false;

	/*
	 * From spc3r22 section 6.5.4 Thin Provisioning VPD page:
	 *
	 * The PAGE LENGTH field is defined in SPC-4. If the DP bit is set to
	 * zero, then the page length shall be set to 0004h.  If the DP bit
	 * is set to one, then the page length shall be set to the value
	 * defined in table 162.
	 */
	buf[0] = dev->transport->get_device_type(dev);

	if ((dev->dev_attrib.emulate_tpu || dev->dev_attrib.emulate_tpws) 
				&& qlib_thin_lun(&dev->dev_dr))
		qnap_thin_setting = true;

	if (qnap_thin_setting)
		put_unaligned_be16(4 + PROVISIONING_GROUP_DESC_LEN, &buf[2]);		
	else {
		/*
		 * Set Hardcoded length mentioned above for DP=0
		 */
		put_unaligned_be16(0x0004, &buf[2]);
	}

	/*
	 * The THRESHOLD EXPONENT field indicates the threshold set size in
	 * LBAs as a power of 2 (i.e., the threshold set size is equal to
	 * 2(threshold exponent)).
	 *
	 * Note that this is currently set to 0x00 as mkp says it will be
	 * changing again.  We can enable this once it has settled in T10
	 * and is actually used by Linux/SCSI ML code.
	 */
	if (qnap_thin_setting) {
		if (cmd->cmd_dr.sbc_ops.sbc_get_threshold_exp)
			buf[4] = (u8)cmd->cmd_dr.sbc_ops.sbc_get_threshold_exp(dev);
		else
			buf[4] = 1;

		buf[5] |= 0x1; /* set DP bit */
	} else
		buf[4] = 0x00;

	/*
	 * A TPU bit set to one indicates that the device server supports
	 * the UNMAP command (see 5.25). A TPU bit set to zero indicates
	 * that the device server does not support the UNMAP command.
	 */
	if (dev->dev_attrib.emulate_tpu != 0)
		buf[5] |= 0x80;

	/*
	 * A TPWS bit set to one indicates that the device server supports
	 * the use of the WRITE SAME (16) command (see 5.42) to unmap LBAs.
	 * A TPWS bit set to zero indicates that the device server does not
	 * support the use of the WRITE SAME (16) command to unmap LBAs.
	 */ 
	if (dev->dev_attrib.emulate_tpws != 0) {
		if (qnap_thin_setting)
			buf[5] |= 0x40;
		else
			buf[5] |= 0x40 | 0x20;
	}

	if (qnap_thin_setting) {
		/* LBPRZ bit should be the same setting as LBPRZ bit in
		 * Read Capacity 16 */
		if (dev->dev_attrib.unmap_zeroes_data)
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
		_qnap_sbc_evpd_b2_build_provisioning_group_desc(dev, &buf[8]);

	} else {
		/*
		 * The unmap_zeroes_data set means that the underlying device supports
		 * REQ_OP_DISCARD and has the discard_zeroes_data bit set. This
		 * satisfies the SBC requirements for LBPRZ, meaning that a subsequent
		 * read will return zeroes after an UNMAP or WRITE SAME (16) to an LBA
		 * See sbc4r36 6.6.4.
		 */
		if (((dev->dev_attrib.emulate_tpu != 0) ||
			     (dev->dev_attrib.emulate_tpws != 0)) &&
			     (dev->dev_attrib.unmap_zeroes_data != 0))
			buf[5] |= 0x04;
	}

	return 0;

}

int qnap_spc_get_ac_and_uc(
	struct se_device *se_dev,
	sector_t *ret_avail_blks,
	sector_t *ret_used_blks
	)	
{
	int ret;

	*ret_avail_blks = 0;
	*ret_used_blks = 0;

	if (!qlib_is_fio_blk_dev(&se_dev->dev_dr)) {
		/* fio + block dev */
		ret = _fd_blk_spc_logsense_lbp_get_ac_and_uc(se_dev, 
						ret_avail_blks, ret_used_blks);

	} else if (!qlib_is_fio_file_dev(&se_dev->dev_dr)) {
		/* fio + regular file */
		ret = _fd_file_spc_logsense_lbp_get_ac_and_uc(se_dev, 
						ret_avail_blks, ret_used_blks);

	} else if (!qlib_is_ib_fbdisk_dev(&se_dev->dev_dr)) {
		/* iblock + fbdisk dev */
		ret = _ib_fbdisk_spc_logsense_lbp_get_ac_and_uc(se_dev, 
						ret_avail_blks, ret_used_blks);

	} else if (!qlib_is_ib_blk_dev(&se_dev->dev_dr)) {
		/* iblock + block dev */
		ret = _ib_blk_spc_logsense_lbp_get_ac_and_uc(se_dev, 
						ret_avail_blks, ret_used_blks);
	} else
		ret = -EOPNOTSUPP;

	return ret;

}

/**/
void qnap_setup_spc_ops(
	struct qnap_se_cmd_dr *cmd_dr
	)
{
	cmd_dr->spc_ops.spc_logsense = do_qnap_spc_logsense;
	cmd_dr->spc_ops.spc_modeselect = do_qnap_spc_modeselect;
	cmd_dr->spc_ops.spc_modesense_lbp = do_qnap_spc_modesense_lbp;
	cmd_dr->spc_ops.spc_modesense_caching = do_qnap_spc_modesense_caching;	
	cmd_dr->spc_ops.spc_evpd_b2 = do_qnap_spc_evpd_b2;	

	/* QNAP SPC odx (offload data transfer) ops */
	if (qnap_odx_setup_spc_ops)
		qnap_odx_setup_spc_ops(cmd_dr);
}

