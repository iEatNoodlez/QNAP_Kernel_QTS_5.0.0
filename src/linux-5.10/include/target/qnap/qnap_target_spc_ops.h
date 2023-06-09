/**
 *
 * @file	qnap_target_spc_ops.h
 * @brief	QNAP LIO target code strcuture for spc ops
 */
#ifndef __QNAP_TARGET_SPC_OPS_H__
#define __QNAP_TARGET_SPC_OPS_H__


/* forward declaration */
struct se_cmd;

/* sense_reason_t MUST be same as one in target_core_base.h */
typedef unsigned __bitwise sense_reason_t;

/* ========================
 * QNAP spc ops 
 * ========================
 */
struct qnap_spc_odx_ops {
	sense_reason_t (*sbc_evpd_8f)(struct se_cmd *se_cmd, unsigned char *buf);
};

struct qnap_spc_ops {
	sense_reason_t (*spc_logsense)(struct se_cmd *cmd);
	sense_reason_t (*spc_modeselect)(struct se_cmd *cmd);
	sense_reason_t (*spc_modesense_lbp)(struct se_cmd *cmd, u8 pc, u8 *p);
	sense_reason_t (*spc_modesense_caching)(struct se_cmd *cmd, u8 pc, u8 *p);
	sense_reason_t (*spc_evpd_b2)(struct se_cmd *cmd, u8 *buf);

	struct qnap_spc_odx_ops spc_odx_ops;
};

#endif

