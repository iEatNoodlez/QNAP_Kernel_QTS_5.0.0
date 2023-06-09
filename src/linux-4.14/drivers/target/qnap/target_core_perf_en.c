/*******************************************************************************
 * Filename:  target_core_perf_en.c
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

#include <linux/net.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
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

#include <target/iscsi/iscsi_target_core.h>
#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_backend.h>
#include "../target_core_iblock.h"
#include "../target_core_file.h"

#include "target_core_qtransport.h"
#include "target_core_qsbc.h"
#include "target_core_perf_en.h"

/**/
unsigned int target_perf_en = 0;
module_param_named(target_perf_enhancement, target_perf_en, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(target_perf_enhancement, "target perf enhancement feature (0:disabled, 1:enabled)");
EXPORT_SYMBOL(target_perf_en);

static int qwork_num_rnd_workers = -1;
module_param_named(target_num_qworks, qwork_num_rnd_workers, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(target_num_qworks, "number of qworks (default: 32)");

static int seqread_num_wq = -1;
module_param_named(target_num_seqread_wqs, seqread_num_wq, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(target_num_seqread_wqs, "number of seq read wqs (default: number of cpu cores)");

static int randread_num_wq = -1;
module_param_named(target_num_randread_wqs, randread_num_wq, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(target_num_randread_wqs, "number of rand read wqs (default: number of iscsit queue)");


/* Worker list and the lock to protect the worker list. */
static struct list_head		qwork_worker_list;
static spinlock_t		qwork_worker_lock;

/* command count, command list, lock and waitqueue
 * for non-sequential read commands.
 */
static atomic_t			qwork_rnd_cmd_cnt;
static struct list_head		qwork_rnd_cmd_list;
static spinlock_t		qwork_rnd_cmd_lock;
static wait_queue_head_t	qwork_rnd_cmd_waitq;

static inline int __adjust_qworks_value(int nr_cpus)
{
	int qworks_value;

	if (qwork_num_rnd_workers < MIN_NR_QWORK) {
		qworks_value = roundup(MIN_NR_QWORK, nr_cpus);

		PERF_FEAT_INFO("nr of qworks < min setting (%d), set it to %d\n", 
				MIN_NR_QWORK, qworks_value);
		return qworks_value;

	} else if (qwork_num_rnd_workers > NR_ISCSIT_QUEUE) {
		qworks_value = roundup(NR_ISCSIT_QUEUE, nr_cpus);

		PERF_FEAT_INFO("nr of qworks > max iscsit queue (%d), set it to %d\n", 
				NR_ISCSIT_QUEUE, qworks_value);
		return qworks_value;
	}

	qworks_value = roundup(qwork_num_rnd_workers, nr_cpus);

	PERF_FEAT_INFO("set nr of qworks to (%d)\n", qworks_value);
	return qworks_value;
}

int qnap_target_perf_adjust_seqread_wq(void)
{
	int nr_cpus = num_online_cpus();

	/* use default value */
	if (seqread_num_wq <= 0) {
		PERF_FEAT_INFO("nr of seq read wq <= 0, set it to %d\n", nr_cpus);
		return nr_cpus;
	} else if (seqread_num_wq > NR_ISCSIT_QUEUE) {
		PERF_FEAT_INFO("nr of seq read wq > max iscsit queue (%d), "
				"set it to %d\n", NR_ISCSIT_QUEUE, NR_ISCSIT_QUEUE);
		return NR_ISCSIT_QUEUE;
	}

	PERF_FEAT_INFO("set nr of seq read wqs (%d)\n", seqread_num_wq);
	return seqread_num_wq;
}

int qnap_target_perf_adjust_randread_wq(void)
{
	/* use default value */
	if (randread_num_wq <= 0 || randread_num_wq >= MAX_NR_RANDREAD_WQ) {
		PERF_FEAT_INFO("nr of rand read wq <= 0 or > max setting, "
				"set it to %d\n", MAX_NR_RANDREAD_WQ);
		return MAX_NR_RANDREAD_WQ;
	} else if (randread_num_wq < MIN_NR_RANDREAD_WQ) {
		PERF_FEAT_INFO("nr of rand read wq < min setting (%d), "
				"set it to %d\n", MIN_NR_RANDREAD_WQ, 
				MIN_NR_RANDREAD_WQ);
		return MIN_NR_RANDREAD_WQ;
	}

	PERF_FEAT_INFO("set nr of rand read wqs (%d)\n", randread_num_wq);
	return randread_num_wq;
}

static inline bool __small_io_case(
	int curr_len,
	int prev_len
	)
{
	if (((curr_len == 4096) || (curr_len == 8192))
	&& ((prev_len == 4096 || prev_len == 8192))
	)
			return true;
	return false;
}

/* this function likes __target_execute_cmd() but do simple behavior */
static void qnap_transport_tiny_exec_cmd(
	struct se_cmd *cmd
	)
{
	int ret;

	if (!cmd->execute_cmd) {
		ret = TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE;
		goto err;
	}

	if (test_bit(QNAP_CMD_T_RELEASE_FROM_CONN, &cmd->cmd_dr.cmd_t_state)) {
		qnap_transport_show_cmd("done to drop cmd: ", cmd);
		target_complete_cmd(cmd, SAM_STAT_GOOD);
		return;
	}

	if (atomic_read(&cmd->cmd_dr.tmr_code)) {
		target_complete_cmd(cmd, SAM_STAT_GOOD);
		return;
	}

	ret = cmd->execute_cmd(cmd);
	if (!ret)
		return;
err:
	spin_lock_irq(&cmd->t_state_lock);
	cmd->transport_state &= ~(CMD_T_SENT);
	spin_unlock_irq(&cmd->t_state_lock);
	transport_generic_request_failure(cmd, ret);
}

static void __qnap_target_exec_random_work(
	struct work_struct *work
	)
{
	struct se_cmd *cmd;
	struct qnap_se_cmd_dr *dr;
	int ret;

	dr = container_of(work, struct qnap_se_cmd_dr, random_work);
	cmd = container_of(dr, struct se_cmd, cmd_dr);
	qnap_transport_tiny_exec_cmd(cmd);	
}

/*
 * Work function to execute a sequential read.
 */
static void qnap_transport_perf_exec_seqrd(struct work_struct *work)
{
	struct se_cmd *cmd;
	struct qnap_se_cmd_dr *dr;
	int ret;

	dr = container_of(work, struct qnap_se_cmd_dr, seqrd_work);
	cmd = container_of(dr, struct se_cmd, cmd_dr);

	qnap_transport_tiny_exec_cmd(cmd);
}

/*
 * Enqueue one command to tail of qwork_xxx_cmd_list, and
 * wake up one worker thread to service this command.
 */
static void qnap_transport_perf_enqueue_cmd(struct se_cmd *cmd)
{
	unsigned long flags;
	bool random, small_read_io = false;
	struct qnap_se_dev_dr *dr = &cmd->se_dev->dev_dr;
	u32 bs_order;

	random = true;

	if (strcmp(cmd->se_dev->transport->name, "fileio"))
		goto queue_cmd;

	switch (cmd->t_task_cdb[0]) {
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		small_read_io = __small_io_case(cmd->data_length, dr->prev_len);
		break;
	default:
		goto save_lba_data;
	}

	if (cmd->t_task_lba == dr->prev_lba)
		random = false;

save_lba_data:
	bs_order = ilog2(cmd->se_dev->dev_attrib.block_size);
	dr->prev_lba = cmd->t_task_lba + (cmd->data_length >> bs_order);
	dr->prev_len = cmd->data_length;

queue_cmd:

	if (random && small_read_io) {
		INIT_WORK(&cmd->cmd_dr.random_work, __qnap_target_exec_random_work);
		queue_work(dr->random_wq, &cmd->cmd_dr.random_work);
	} else if (random) {
		spin_lock_irqsave(&cmd->t_state_lock, flags);
		cmd->t_state = TRANSPORT_WORK_THREAD_PROCESS;
		cmd->transport_state |= CMD_T_ACTIVE;
		spin_unlock_irqrestore(&cmd->t_state_lock, flags);

		spin_lock_bh(&qwork_rnd_cmd_lock);
		atomic_inc(&qwork_rnd_cmd_cnt);
		list_add_tail(&cmd->cmd_dr.se_queue_node, &qwork_rnd_cmd_list);
		set_bit(QNAP_CMD_T_QUEUED, &cmd->cmd_dr.cmd_t_state);
		spin_unlock_bh(&qwork_rnd_cmd_lock);

		/* wake up one worker to service this command. */
		wake_up_interruptible(&qwork_rnd_cmd_waitq);
	} else {
		INIT_WORK(&cmd->cmd_dr.seqrd_work, qnap_transport_perf_exec_seqrd);
		queue_work(dr->seqrd_wq, &cmd->cmd_dr.seqrd_work);
	}
}

static struct se_cmd* qnap_transport_perf_dequeue_cmd(void)
{
	struct se_cmd *cmd;
	struct qnap_se_cmd_dr *dr;

	cmd = NULL;

	spin_lock_bh(&qwork_rnd_cmd_lock);
	if (!list_empty(&qwork_rnd_cmd_list)) {
		dr = list_first_entry(&qwork_rnd_cmd_list,
				struct qnap_se_cmd_dr, se_queue_node);
		cmd = container_of(dr, struct se_cmd, cmd_dr);

		clear_bit(QNAP_CMD_T_QUEUED, &dr->cmd_t_state);
		list_del_init(&dr->se_queue_node);
		atomic_dec(&qwork_rnd_cmd_cnt);
	}
	spin_unlock_bh(&qwork_rnd_cmd_lock);

	return cmd;
}

int qnap_transport_perf_exec_wt_cmd(
	struct se_cmd *se_cmd
	)
{
	qnap_transport_perf_enqueue_cmd(se_cmd);
	return 0;
}
EXPORT_SYMBOL(qnap_transport_perf_exec_wt_cmd);

void qnap_transport_perf_wakeup_workers(void)
{
	wake_up_interruptible_all(&qwork_rnd_cmd_waitq);
}

static int qnap_transport_perf_cmd_thread(void *arg)
{
	qwork_t *qw = (qwork_t *)arg;
	struct se_cmd *cmd;

	allow_signal(SIGINT);

	while (!kthread_should_stop()) {

		if ((cmd = qnap_transport_perf_dequeue_cmd()) != NULL) {

			switch (cmd->t_state) {
			case TRANSPORT_WORK_THREAD_PROCESS:
				target_execute_cmd(cmd);
				break;
			default:
				pr_err("Unknown t_state: %d, "
					"i_state: %d on SE LUN: %llu\n",
					cmd->t_state,
					cmd->se_tfo->get_cmd_state(cmd),
					cmd->se_lun->unpacked_lun);
				BUG();
			}
			cond_resched();
		} else {

			wait_event_interruptible_exclusive(qwork_rnd_cmd_waitq,
				atomic_read(&qwork_rnd_cmd_cnt));
		}
	}

	spin_lock(&qwork_worker_lock);
	list_del_init(&qw->qw_node);
	qw->qw_thread = NULL;
	spin_unlock(&qwork_worker_lock);

	return 0;
}

void qnap_transport_perf_tmr_drain_cmd_list(
	struct se_device *dev,
	struct se_cmd *prout_cmd,
	struct se_node_acl *tmr_nacl,
	int tas,
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),	
	struct list_head *preempt_and_abort_list
	)
{
	struct se_cmd *cmd;
	struct qnap_se_cmd_dr *dr, *t_dr;

	/*
	 * Release all commands remaining in the struct se_device cmd queue.
	 *
	 * This follows the same logic as above for the struct se_device
	 * struct se_task state list, where commands are returned with
	 * TASK_ABORTED status, if there is an outstanding $FABRIC_MOD
	 * reference, otherwise the struct se_cmd is released.
	 */
	spin_lock_bh(&qwork_rnd_cmd_lock);
	list_for_each_entry_safe(dr, t_dr, &qwork_rnd_cmd_list, se_queue_node) {

		cmd = container_of(dr, struct se_cmd, cmd_dr);

		if (cmd->se_dev != dev) {
			continue;
		}

		/*
		 * For PREEMPT_AND_ABORT usage, only process commands
		 * with a matching reservation key.
		 */
		if (!check_cdb_and_preempt)
			continue;

		if (check_cdb_and_preempt(preempt_and_abort_list, cmd))
			continue;

		/*
		 * Not aborting PROUT PREEMPT_AND_ABORT CDB..
		 */
		if (prout_cmd == cmd)
			continue;

		if (atomic_read(&dr->tmr_code))
			continue;

		/* setup TMR code and check it is same or different i_t_nexus */
		atomic_set(&dr->tmr_code, QNAP_TMR_CODE_LUN_RESET);

		if (tmr_nacl && (tmr_nacl != cmd->se_sess->se_node_acl)){
			atomic_set(&dr->tmr_diff_it_nexus, 1);
			if (cmd->se_dev->dev_attrib.emulate_tas)
				atomic_set(&dr->tmr_resp_tas, 1);
		}

		cmd->se_tfo->set_delay_remove(cmd, true);
	}

	spin_unlock_bh(&qwork_rnd_cmd_lock);

}

int qnap_transport_perf_init(void)
{
	int i, cpu, ret, nr_cpus;
	int total_workers;
	qwork_t *qw;

	INIT_LIST_HEAD(&qwork_rnd_cmd_list);
	spin_lock_init(&qwork_rnd_cmd_lock);
	init_waitqueue_head(&qwork_rnd_cmd_waitq);

	INIT_LIST_HEAD(&qwork_worker_list);
	spin_lock_init(&qwork_worker_lock);

	nr_cpus = num_online_cpus();	
	total_workers = __adjust_qworks_value(nr_cpus);

	for (i = 0 ; i < total_workers ; i++) {

		/* Create a few workers per cpu to run target commands. */
		cpu = i % nr_cpus;
		qw = kmalloc(sizeof(*qw), GFP_KERNEL);
		if (!qw) {
			pr_err("%s: can't allocate qt\n", __func__);
			ret = -ENOMEM;
			goto cleanup_qw;
		}
		qw->qw_cpu = cpu;
		qw->qw_thread = kthread_run(qnap_transport_perf_cmd_thread,
			(void *)qw, "qwork-%d-%d", cpu, i);
		if (!qw->qw_thread) {
			pr_err("%s: run kthread failed\n", __func__);
			ret = -EFAULT;
			goto cleanup_qw;
		}
		spin_lock(&qwork_worker_lock);
		list_add(&qw->qw_node, &qwork_worker_list);
		spin_unlock(&qwork_worker_lock);
	}

	pr_info("%s: starts ...\n", __func__);
	return 0;

cleanup_qw:
	while (1) {

		spin_lock(&qwork_worker_lock);
		if (list_empty(&qwork_worker_list)) {
			qw = NULL;
		} else {
			qw = list_first_entry(&qwork_worker_list, qwork_t, qw_node);
			list_del_init(&qw->qw_node);
		}
		spin_unlock(&qwork_worker_lock);
		if (!qw) {
			break;
		}

		if (qw->qw_thread) {
			kthread_stop(qw->qw_thread);
		}
		kfree(qw);
	}

	return ret;
}

void qnap_transport_perf_fini(void)
{
	qwork_t *qw;

	pr_info("%s starts\n", __func__);

	/* Set qwork_rnd_cmd_cndto make workers
	 * leave wait_event_interruptible_exclusive().
	 */
	atomic_set(&qwork_rnd_cmd_cnt, 0x7fffffff);

	while (1) {

		spin_lock(&qwork_worker_lock);
		if (list_empty(&qwork_worker_list)) {
			qw = NULL;
		} else {
			qw = list_first_entry(&qwork_worker_list, qwork_t, qw_node);
			list_del_init(&qw->qw_node);
		}
		spin_unlock(&qwork_worker_lock);
		if (!qw) {
			break;
		}

		if (qw->qw_thread) {
			kthread_stop(qw->qw_thread);
		}
		kfree(qw);
	}
	pr_info("%s ends\n", __func__);
}

