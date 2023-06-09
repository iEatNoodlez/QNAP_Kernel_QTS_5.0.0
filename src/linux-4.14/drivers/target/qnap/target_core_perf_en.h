/*******************************************************************************
 * Filename:  target_core_perf_en.h
 *
 *  Copyright (C) 2019  QNAP, Inc.
 *
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

#ifndef __TARGET_CORE_PERF_ENHANCEMENT_H__
#define __TARGET_CORE_PERF_ENHANCEMENT_H__

#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <target/iscsi/iscsi_target_core.h>
#include <target/target_core_base.h>

/**/
#define NR_ISCSIT_QUEUE      TA_DEFAULT_CMDSN_DEPTH
#define MIN_NR_RANDREAD_WQ   NR_ISCSIT_QUEUE
#define MAX_NR_RANDREAD_WQ   WQ_DFL_ACTIVE
#define MIN_NR_QWORK         (32)

/**/
#define	msg_prefix	"[target-perf-feat]: "
#define PERF_FEAT_DEBUG(f, arg...) pr_debug(msg_prefix f, ## arg)
#define PERF_FEAT_INFO(f, arg...) pr_info(msg_prefix f, ## arg)
#define PERF_FEAT_WARN(f, arg...) pr_warn(msg_prefix f, ## arg)
#define PERF_FEAT_ERR(f, arg...) pr_err(msg_prefix f, ## arg)	


/**/
struct se_cmd;
struct se_device;
struct se_node_acl;
struct list_head;

/**/
typedef struct qwork {
	struct list_head	qw_node;
	int			qw_cpu;
	struct task_struct  	*qw_thread;
} qwork_t;


/**/
void qnap_transport_perf_tmr_drain_cmd_list(
	struct se_device *dev,
	struct se_cmd *prout_cmd,
	struct se_node_acl *tmr_nacl,
	int tas,
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),	
	struct list_head *preempt_and_abort_list
	);

int qnap_transport_perf_exec_wt_cmd(struct se_cmd *se_cmd);
void qnap_transport_perf_wakeup_workers(void);

int qnap_transport_perf_init(void);
void qnap_transport_perf_fini(void);

int qnap_target_perf_adjust_randread_wq(void);
int qnap_target_perf_adjust_seqread_wq(void);

/**/
extern unsigned int target_perf_en;

/**/
static inline bool qnap_target_perf_enabled(void)
{
	return ((target_perf_en == 1) ? true : false);
}

static inline bool qnap_target_perf_set_run_complete_ok_flag(struct se_cmd *cmd)
{
	if (cmd->t_task_cdb[0] == READ_6 || cmd->t_task_cdb[0] == READ_10
	|| cmd->t_task_cdb[0] == READ_12 || cmd->t_task_cdb[0] == READ_16
	|| cmd->t_task_cdb[0] == WRITE_6 || cmd->t_task_cdb[0] == WRITE_10
	|| cmd->t_task_cdb[0] == WRITE_12 || cmd->t_task_cdb[0] == WRITE_16
	)
		cmd->run_complete_ok_directly = true;

	cmd->run_complete_ok_directly = false;
	return;
}


#endif

