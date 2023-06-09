#ifndef __TARGET_CORE_ZMC_H__
#define __TARGET_CORE_ZMC_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

struct zmc_data {
	bool	valid;
	union {
		struct target_iscsi_zmc_data izmc;		
	} u ;
};

static inline bool scsi_read_6_10_12_16(
	u8 *cdb
	)
{
	if (cdb[0] == READ_6 || cdb[0] == READ_10 
	|| cdb[0] == READ_12 || cdb[0] == READ_16
	)
		return true;
	return false;
}

static inline bool scsi_write_6_10_12_16(
	u8 *cdb
	)
{
	if (cdb[0] == WRITE_6 || cdb[0] == WRITE_10 
	|| cdb[0] == WRITE_12 || cdb[0] == WRITE_16
	)
		return true;
	return false;
}

void qnap_target_se_cmd_zmc_free(struct se_cmd *se_cmd);
void qnap_target_se_cmd_zmc_init(struct se_cmd *se_cmd);
void qnap_target_zmc_prep_backend_op(struct se_cmd *cmd);

#endif

