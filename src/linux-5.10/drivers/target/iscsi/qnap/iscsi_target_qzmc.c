/*******************************************************************************
 * @file        iscsi_target_zmc.c
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

#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/crypto.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/backing-dev.h>
#include <linux/net.h>
#include <net/sock.h>
#include <asm/unaligned.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>
#include <target/iscsi/iscsi_target_core.h>
#include <target/iscsi/iscsi_transport.h>

#include "iscsi_target_qzmc.h"
#include "iscsi_target_tcp_qzmc.h"

/**/
static LIST_HEAD(g_iscsit_zmc_list);
static DEFINE_MUTEX(g_iscsit_zmc_lock);


/**/
struct iscsit_zmc_ops *qnap_iscsit_zmc_get(int type);

/* called in iscsit_handle_scsi_cmd() */
int qnap_iscsit_zmc_setup_transport_zmc_ops(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	int ret;

	if (conn->zmc_ops && conn->zmc_conn_priv && cmd->zmc_cmd_priv) {
		/* setup transport_zmc_ops to se_cmd since it is good
		 * to alloc conn_priv and cmd_priv. this must be PASSED 
		 */
		ret = qnap_zmc_setup_transport_zmc_ops(&cmd->se_cmd, 
							(void *)conn->zmc_ops);
		if (ret) {
			ISCSI_TCP_ZMC_ERR("fail to call "
			"qnap_zmc_setup_transport_zmc_ops(), ret:%d\n", ret);
		}
		if (ret == -ENOTSUPP)
			ret = -EINVAL;
	} else
		ret = -ENOTSUPP;

	return ret;
}

/* called in iscsit_release_cmd() */
void qnap_iscsit_zmc_release_cmd_priv(
	struct iscsi_cmd *cmd
	)
{
	struct iscsi_conn *conn = cmd->conn;

	if (!conn) {
		WARN_ON(true);
		return;
	}

	if (conn->zmc_ops && conn->zmc_conn_priv && cmd->zmc_cmd_priv)
		conn->zmc_ops->free_cmd_priv(conn, cmd);
}

/* called in iscsit_allocate_cmd(), we may be in interrupt code */
int qnap_iscsit_zmc_create_cmd_priv(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	int ret = -ENOTSUPP;

	if (conn->zmc_ops && conn->zmc_conn_priv) {
		/* -ENOMEM or 0 */
		ret = conn->zmc_ops->alloc_cmd_priv(conn, cmd);
	}

	return ret;
}

void qnap_iscsit_zmc_release_conn_priv(
	struct iscsi_conn *conn
	)
{
	if (conn->zmc_ops) {
		conn->zmc_ops->free_conn_priv(conn);
		conn->zmc_ops = NULL;
	}
}

void qnap_iscsit_zmc_create_conn_priv(
	struct iscsi_conn *conn
	)
{
	conn->zmc_ops = qnap_iscsit_zmc_get(conn->conn_transport->transport_type);
	if (conn->zmc_ops) {
		if (conn->zmc_ops->alloc_conn_priv(conn)) {
			conn->zmc_ops = NULL;
		}
	}
}

void qnap_iscsit_zmc_register(
	struct iscsit_zmc_ops *p
	)
{
	if (!p)
		return;

	INIT_LIST_HEAD(&p->node);
	mutex_lock(&g_iscsit_zmc_lock);
	list_add_tail(&p->node, &g_iscsit_zmc_list);
	mutex_unlock(&g_iscsit_zmc_lock);
}

void qnap_iscsit_zmc_unregister(
	struct iscsit_zmc_ops *p
	)
{
	if (!p)
		return;

	mutex_lock(&g_iscsit_zmc_lock);
	if (!list_empty(&g_iscsit_zmc_list))
		list_del_init(&p->node);
	mutex_unlock(&g_iscsit_zmc_lock);	
}

struct iscsit_zmc_ops *qnap_iscsit_zmc_get(int type)
{
	struct iscsit_zmc_ops *p;

	mutex_lock(&g_iscsit_zmc_lock);
	list_for_each_entry(p, &g_iscsit_zmc_list, node) {
		if (p->transport_type == type) {
			mutex_unlock(&g_iscsit_zmc_lock);
			return p;
		}
	}

	mutex_unlock(&g_iscsit_zmc_lock);
	return NULL;
}



