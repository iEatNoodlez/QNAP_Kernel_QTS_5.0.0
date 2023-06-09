/*******************************************************************************
 * Filename:  
 * @file        target_core_qzmc.c
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

#include "target_core_qlib.h"
#include "target_core_qzmc.h"
#include "target_core_iscsi_qzmc.h"

/**/
static LIST_HEAD(g_target_zmc_list);
static DEFINE_MUTEX(g_target_zmc_lock);


/**/
struct target_zmc_ops *__qnap_zmc_get_ops(
	const char *fabric_name
	)
{
	struct target_zmc_ops *p = NULL;

	mutex_lock(&g_target_zmc_lock);
	list_for_each_entry(p, &g_target_zmc_list, node) {
		if (!strcmp(fabric_name, p->fabric_name)) {
			mutex_unlock(&g_target_zmc_lock);
			return p;
		}
	}

	mutex_unlock(&g_target_zmc_lock);
	return NULL;
}

/**/
void qnap_zmc_iblock_get_write_op_flags(
	struct se_cmd *cmd, 
	struct request_queue *q,
	int *op_flags
	)
{
	/* refer from target_core_iblock.c */

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

void qnap_zmc_destroy_sgl_node_cache(
	struct se_device *dev
	)
{
	struct qnap_se_dev_dr *dr = &dev->dev_dr;

	if (dr->zmc_sgl_node_cache)
		kmem_cache_destroy(dr->zmc_sgl_node_cache);
}

int qnap_zmc_create_sgl_node_cache(
	struct se_device *dev,
	u32 dev_index
	)
{
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	char name[64];
	int ret = 0;

	snprintf(name, 64, "zmc_sglnode_cache_%d", dev_index);

	dr->zmc_sgl_node_cache = kmem_cache_create(name, 
			sizeof(struct tcp_zmc_sgl_node),
			__alignof__(struct tcp_zmc_sgl_node), 0, NULL);
	
	if (!dr->zmc_sgl_node_cache) {
		pr_warn("Unable to create zmc_wsg_node_cache\n");
		ret = -ENOMEM;
	}

	return ret;
}

/* called from transport_init_se_cmd() in iscsit_setup_scsi_cmd() and
 * the cmd_dr->transport_zmc_ops will be set after call iscsit_setup_scsi_cmd()
 */
void qnap_zmc_init_data(
	struct se_cmd *cmd
	)
{
	cmd->cmd_dr.zmc_ops = __qnap_zmc_get_ops(cmd->se_tfo->fabric_name);
}

struct target_zmc_ops *qnap_zmc_get_ops(
	struct se_cmd *cmd
	)
{
	return __qnap_zmc_get_ops(cmd->se_tfo->fabric_name);
}

/* called after iscsit_setup_scsi_cmd() 
 * and the qnap_iscsit_zmc_create_cmd_priv() returns good
 */
int qnap_zmc_setup_transport_zmc_ops(
	struct se_cmd *cmd,
	void *ops
	)
{
	/* zmc_ops wil be setup in qnap_zmc_init_data() */
	if (!cmd->cmd_dr.zmc_ops)
		return -EINVAL;
		
	cmd->cmd_dr.transport_zmc_ops = ops;
	return 0;
}
EXPORT_SYMBOL(qnap_zmc_setup_transport_zmc_ops);

void qnap_target_zmc_register(
	struct target_zmc_ops *p
	)
{
	if (p) {
		INIT_LIST_HEAD(&p->node);
		mutex_lock(&g_target_zmc_lock);
		list_add_tail(&p->node, &g_target_zmc_list);
		mutex_unlock(&g_target_zmc_lock);
	}
}

void qnap_target_zmc_unregister(
	struct target_zmc_ops *p
	)
{
	if (!p) 
		return;

	mutex_lock(&g_target_zmc_lock);
	if (!list_empty(&g_target_zmc_list))
		list_del_init(&p->node);
	mutex_unlock(&g_target_zmc_lock);	
}


