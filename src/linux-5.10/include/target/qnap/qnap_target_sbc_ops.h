/**
 *
 * @file	qnap_target_sbc_ops.h
 * @brief	QNAP LIO target code strcuture for sbc ops
 */
#ifndef __QNAP_TARGET_SBC_OPS_H__
#define __QNAP_TARGET_SBC_OPS_H__

/* forward declaration */
struct se_device;
struct se_cmd;

/* sense_reason_t MUST be same as one in target_core_base.h */
typedef unsigned __bitwise sense_reason_t;

/* ========================
 * QNAP sbc ops 
 * ========================
 */
struct qnap_sbc_odx_ops {
	/* Populate Token */
	sense_reason_t (*sbc_odx_pt)(struct se_cmd *se_cmd);
	/* Write Using Token */
	sense_reason_t (*sbc_odx_wut)(struct se_cmd *se_cmd);
	/* Receive ROD Token Information */
	sense_reason_t (*sbc_odx_rrti)(struct se_cmd *se_cmd);
};

struct qnap_sbc_unmap_ops {
	sense_reason_t (*orig_sbc_unmap)(struct se_cmd *cmd);
	sense_reason_t (*orig_sbc_core_unmap)(struct se_cmd *cmd, sector_t lba, 
				sector_t nolb); 	
	sense_reason_t (*sbc_unmap)(struct se_cmd *cmd);
	sense_reason_t (*sbc_core_unmap)(struct se_cmd *cmd, sector_t lba, 
				sector_t nolb); 	
};
	
struct qnap_sbc_sync_ops {
	sense_reason_t (*orig_sbc_sync_cache)(struct se_cmd *cmd);
	sense_reason_t (*sbc_sync_cache)(struct se_cmd *cmd);
};
	
struct qnap_sbc_write_same_ops {
	/* sbc_ws_fastzero() will be called in original write-same function from
	 * backend, the necessary checking conditions were already ready in
	 * origianl write-same code
	 */
	int (*sbc_ws_fastzero)(struct se_cmd *cmd, sense_reason_t *ret);

	/* sbc_core_ws_fastzero() is real function to handle the fastzero operation */
	sense_reason_t (*sbc_core_ws_fastzero)(struct se_cmd *cmd);
};

struct qnap_sbc_ops {
	sense_reason_t (*sbc_get_lba_status)(struct se_cmd *cmd);
	sense_reason_t (*sbc_write_same_fast_zero)(struct se_cmd *cmd);
	sense_reason_t (*sbc_verify_10_16)(struct se_cmd *cmd);
	int (*sbc_get_threshold_exp)(struct se_device *se_dev);

	struct qnap_sbc_unmap_ops sbc_unmap_ops;
	struct qnap_sbc_sync_ops sbc_sync_ops;
	struct qnap_sbc_write_same_ops sbc_ws_ops;
	struct qnap_sbc_odx_ops sbc_odx_ops;
};

#endif

