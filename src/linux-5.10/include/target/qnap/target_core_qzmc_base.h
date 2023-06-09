#ifndef __TARGET_CORE_QZMC_BASE_H__
#define __TARGET_CORE_QZMC_BASE_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

/**/
struct se_cmd;

/**/
struct target_zmc_ops {
	char			fabric_name[64];
	struct list_head	node;

	/* simple wrapper for zmc feature 
	 *
	 * -ENOTSUPP: not support zmc feature
	 * -EPERM: not need to do (it supports zmc feature but for fall-through case))
	 * 0: good to call function (support zmc feature)
	 * others: fail to call function (support zmc feature)
	 */
	int (*kmap_data_sg)(struct se_cmd *cmd, u32 len, void **vmap);
	int (*kunmap_data_sg)(struct se_cmd *cmd);
	int (*alloc_sgl)(struct se_cmd *cmd);
	int (*free_sgl)(struct se_cmd *cmd);
	int (*prep_final_sgl)(struct se_cmd *cmd);

	/* simple wrapper for some sbc functions for zmc feature 
	 *
	 * -ENOTSUPP: not support zmc feature
	 * 0: good to call function (support zmc feature)
	 * others: fail to call function (support zmc feature) 
	 *
	 * for zmc_sbc_xxxx(), the cmd->cmd_dr.work_io_err will carry the 
	 * TCM_XXX code finally
	 */
	int (*sbc_write)(struct se_cmd *cmd, void *data);
	int (*sbc_ats_callback)(struct se_cmd *cmd, bool success, int *post_ret);
	int (*sbc_write_same)(struct se_cmd *cmd, void *data);

};

/**/
static inline bool scsi_read_6_10_12_16(u8 *cdb) 
{
	if (cdb[0] == READ_6 || cdb[0] == READ_10 
	|| cdb[0] == READ_12 || cdb[0] == READ_16
	)
		return true;
	return false;
}

static inline bool scsi_write_6_10_12_16(u8 *cdb)
{
	if (cdb[0] == WRITE_6 || cdb[0] == WRITE_10 
	|| cdb[0] == WRITE_12 || cdb[0] == WRITE_16
	)
		return true;
	return false;
}

/**/
int qnap_zmc_setup_transport_zmc_ops(struct se_cmd *cmd, void *ops);

#endif

