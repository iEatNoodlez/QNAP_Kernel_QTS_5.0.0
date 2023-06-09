/*******************************************************************************
 * Filename:  iscsi_target_qtransport.c
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
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/backing-dev.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <asm/unaligned.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>
#include <target/iscsi/iscsi_target_core.h>
#include <target/iscsi/iscsi_transport.h>

#include "../iscsi_target_parameters.h"
#include "../iscsi_target_datain_values.h"
#include "../iscsi_target_util.h"
#include "../../target_core_iblock.h"
#include "../../target_core_file.h"

#include "iscsi_target_qlog.h"
#include "iscsi_target_qtransport.h"
#include "iscsi_target_qconfigfs.h"
#include "../../qnap/target_core_qtransport.h"

#include <linux/time.h>
#include <linux/socket.h>

#ifdef CONFIG_MACH_QNAPTS

struct nl_func *nl_func_p = NULL;

void qnap_nl_create_func_table(void)
{
	/* TBD: anything is good to get functon table ??? */
	if (!nl_func_p)
		nl_func_p = (struct nl_func *)qnap_nl_get_nl_func();
}

void qnap_nl_send_post_log(
	int conn_type,
	int log_type,
	struct iscsi_session *sess,
	char *ip
)
{
	struct iscsi_portal_group *tpg = NULL;
	char *target_iqn = NULL;

	if (!nl_func_p)
		return;

	tpg = (struct iscsi_portal_group *)(sess)->tpg;
	target_iqn = (tpg && tpg->tpg_tiqn && strlen(tpg->tpg_tiqn->tiqn) > 0) \
		? (char *)tpg->tpg_tiqn->tiqn : "Discovery Session";

	if (LOGIN_OK == conn_type) {
		struct timespec64 cur_time;
		ktime_get_real_ts64(&cur_time);
		sess->login_time = cur_time.tv_sec;
	}

	nl_func_p->nl_post_log_send(conn_type, log_type, 
		sess->sess_ops->InitiatorName, target_iqn, ip);

}

void qnap_nl_rt_conf_update(
	int conn_type,
	int log_type,
	char *key,
	char *val
)
{
	if (!nl_func_p)
		return;

	nl_func_p->nl_rt_conf_update(conn_type, log_type, key, val);
}

#ifdef QNAP_KERNEL_STORAGE_V2
struct workqueue_struct *acl_notify_info_wq = NULL;
struct kmem_cache *acl_notify_info_cache = NULL;

void qnap_create_acl_notify_info_wq(void)
{
	acl_notify_info_wq = alloc_workqueue("acl_notify_info_wq",
			(WQ_HIGHPRI | WQ_MEM_RECLAIM | WQ_UNBOUND), 0);
	if (!acl_notify_info_wq)
		pr_warn("%s: fail to create acl_notify_info_wq\n", __func__);
	return;
}

void qnap_destroy_acl_notify_info_wq(void)
{
	if (acl_notify_info_wq) {
		flush_workqueue(acl_notify_info_wq);
		destroy_workqueue(acl_notify_info_wq);
	}
	return;
}

void qnap_create_acl_notify_info_cache(void)
{
	acl_notify_info_cache = kmem_cache_create("acl_notify_info_cache",
			sizeof(struct notify_data),
			__alignof__(struct notify_data), 0, NULL);

	if (!acl_notify_info_cache)
		pr_warn("%s: fail to create acl_notify_info_cache\n", __func__);
	return;
}

void qnap_destroy_acl_notify_info_cache(void)
{
	if (acl_notify_info_cache)
		kmem_cache_destroy(acl_notify_info_cache);
	return;
}

int qnap_target_nacl_update_show_info(
	struct notify_data *n_data
	)
{
	struct file *fd = NULL;
	struct kvec iov;
	struct iov_iter iter;
	int ret = 0;

	fd = filp_open(n_data->path, O_CREAT | O_WRONLY | O_APPEND , S_IRWXU);
	if (IS_ERR (fd))
		return -EINVAL;

	iov.iov_base = &n_data->data[0];
	iov.iov_len = n_data->data_size;
	iov_iter_kvec(&iter, WRITE, &iov, 1, iov.iov_len);

	ret = vfs_iter_write(fd, &iter, &fd->f_pos, 0);
	if(ret < 0)
		pr_err("%s: vfs_iter_write() error. ret:%d\n", __func__, ret);

	filp_close(fd, NULL);
	return ((ret < 0) ? ret : 0);
}

static void __qnap_target_acl_notify_info_work(
	struct work_struct *work
	)
{
	struct notify_data *data = 
		container_of(work, struct notify_data, work);

	struct se_node_acl *se_nacl = data->se_nacl;

	qnap_target_nacl_update_show_info(data);

	vfree(data->data);

	qnap_free_acl_notify_info_cache(data);

	/* to indicate we finished to write acl info data */
	spin_lock(&se_nacl->acl_read_info_lock);
	atomic_dec_mb(&se_nacl->acl_read_info);
	spin_unlock(&se_nacl->acl_read_info_lock);
	return;
}


static void __qnap_target_nacl_show_info_null_work(
	struct work_struct *work
	)
{
	struct notify_data *data = 
		container_of(work, struct notify_data, work);

	struct se_node_acl *se_nacl = data->se_nacl;

	qnap_free_acl_notify_info_cache(data);

	/* to indicate we finished to write acl info data */
	spin_lock(&se_nacl->acl_read_info_lock);
	atomic_dec_mb(&se_nacl->acl_read_info);
	spin_unlock(&se_nacl->acl_read_info_lock);
	return;
}

int qnap_put_acl_notify_info_work(
	struct notify_data *data
	)
{
	if (acl_notify_info_wq) {
		INIT_WORK(&data->work, __qnap_target_acl_notify_info_work);
		queue_work(acl_notify_info_wq, &data->work);
		return 0;
	}
	return -ENODEV;
}

int qnap_put_acl_notify_info_null_work(
	struct notify_data *data
	)
{
	if (acl_notify_info_wq) {
		INIT_WORK(&data->work, __qnap_target_nacl_show_info_null_work);
		queue_work(acl_notify_info_wq, &data->work);
		return 0;
	}
	return -ENODEV;

}
#endif /* QNAP_KERNEL_STORAGE_V2 */

static unsigned char _qnap_iscsit_decode_base64_digit(
	char base64
	)
{
	switch (base64) {
	case '=':
		return 64;
	case '/':
		return 63;
	case '+':
		return 62;
	default:
		if ((base64 >= 'A') && (base64 <= 'Z'))
			return base64 - 'A';
		else if ((base64 >= 'a') && (base64 <= 'z'))
			return 26 + (base64 - 'a');
		else if ((base64 >= '0') && (base64 <= '9'))
			return 52 + (base64 - '0');
		else
			return -1;
	}
}

int qnap_iscsit_decode_base64_string(
	char *string, 
	unsigned char *out_buf, 
	int out_buf_len
	)
{
	int len;
	int count;
	int intptr;
	unsigned char num[4];
	int octets;

	if ((string == NULL) || (out_buf == NULL))
		return -EINVAL;
	len = strlen(string);
	if (len == 0)
		return -EINVAL;

	if ((len % 4) != 0)
		return -EINVAL;

	count = 0;
	intptr = 0;
	while (count < len - 4) {
		num[0] = _qnap_iscsit_decode_base64_digit(string[count]);
		num[1] = _qnap_iscsit_decode_base64_digit(string[count + 1]);
		num[2] = _qnap_iscsit_decode_base64_digit(string[count + 2]);
		num[3] = _qnap_iscsit_decode_base64_digit(string[count + 3]);
		if ((num[0] == 65) || (num[1] == 65) 
		|| (num[2] == 65) || (num[3] == 65)
		)
			return -EINVAL;

		count += 4;
		octets =
		    (num[0] << 18) | (num[1] << 12) | (num[2] << 6) | num[3];

		if(intptr + 3 > out_buf_len) {
			pr_err("error: %s,%d: buffer size is not enough, intptr:%d, out_buf_Len:%d\n", __func__, __LINE__, intptr + 3, out_buf_len);
			return -EINVAL;
		}
		
		out_buf[intptr] = (octets & 0xFF0000) >> 16;
		out_buf[intptr + 1] = (octets & 0x00FF00) >> 8;
		out_buf[intptr + 2] = octets & 0x0000FF;
		intptr += 3;
	}
	num[0] = _qnap_iscsit_decode_base64_digit(string[count]);
	num[1] = _qnap_iscsit_decode_base64_digit(string[count + 1]);
	num[2] = _qnap_iscsit_decode_base64_digit(string[count + 2]);
	num[3] = _qnap_iscsit_decode_base64_digit(string[count + 3]);
	if ((num[0] == 64) || (num[1] == 64))
		return  -EINVAL;
	if (num[2] == 64) {
		if (num[3] != 64)
			return  -EINVAL;

		if(intptr + 2 > out_buf_len) {
			pr_err("error: %s,%d: buffer size is not enough, intptr:%d, out_buf_Len:%d\n", __func__, __LINE__, intptr + 2, out_buf_len);
			return -EINVAL;
		}
		
		out_buf[intptr] = (num[0] << 2) | (num[1] >> 4);
		out_buf[intptr + 1] = '\0';
	} else if (num[3] == 64) {

		if(intptr + 3 > out_buf_len) {
			pr_err("error: %s,%d: buffer size is not enough, intptr:%d, out_buf_Len:%d\n", __func__, __LINE__, intptr + 3, out_buf_len);
			return -EINVAL;
		}
	
		out_buf[intptr] = (num[0] << 2) | (num[1] >> 4);
		out_buf[intptr + 1] = (num[1] << 4) | (num[2] >> 2);
		out_buf[intptr + 2] = '\0';
	} else {
		octets =
		    (num[0] << 18) | (num[1] << 12) | (num[2] << 6) | num[3];

		if(intptr + 4 > out_buf_len) {
			pr_err("error: %s,%d: buffer size is not enough, intptr:%d, out_buf_Len:%d\n", __func__, __LINE__, intptr + 4, out_buf_len);
			return -EINVAL;
		}
		    
		out_buf[intptr] = (octets & 0xFF0000) >> 16;
		out_buf[intptr + 1] = (octets & 0x00FF00) >> 8;
		out_buf[intptr + 2] = octets & 0x0000FF;
		out_buf[intptr + 3] = '\0';
	}

	return 0;
}

void qnap_iscsit_encode_base64_string(
	unsigned char *in_buf, 
	int in_buf_len,
	char *out_string
	)
{
	int count, octets, strptr, delta;
	static const char base64code[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G',
					   'H', 'I', 'J', 'K', 'L', 'M', 'N',
					   'O', 'P', 'Q', 'R', 'S', 'T', 'U',
					   'V', 'W', 'X', 'Y', 'Z', 'a', 'b',
					   'c', 'd', 'e', 'f', 'g', 'h', 'i',
					   'j', 'k', 'l', 'm', 'n', 'o', 'p',
					   'q', 'r', 's', 't', 'u', 'v', 'w',
					   'x', 'y', 'z', '0', '1', '2', '3',
					   '4', '5', '6', '7', '8', '9', '+',
					   '/', '=' };

	if ((!in_buf) || (!out_string) || (!in_buf_len))
		return;

	count = 0;
	octets = 0;
	strptr = 0;

	while ((delta = (in_buf_len - count)) > 2) {
		octets = (in_buf[count] << 16) | (in_buf[count + 1] << 8) | in_buf[count + 2];
		out_string[strptr] = base64code[(octets & 0xfc0000) >> 18];
		out_string[strptr + 1] = base64code[(octets & 0x03f000) >> 12];
		out_string[strptr + 2] = base64code[(octets & 0x000fc0) >> 6];
		out_string[strptr + 3] = base64code[octets & 0x00003f];
		count += 3;
		strptr += 4;
	}
	if (delta == 1) {
		out_string[strptr] = base64code[(in_buf[count] & 0xfc) >> 2];
		out_string[strptr + 1] = base64code[(in_buf[count] & 0x03) << 4];
		out_string[strptr + 2] = base64code[64];
		out_string[strptr + 3] = base64code[64];
		strptr += 4;
	} else if (delta == 2) {
		out_string[strptr] = base64code[(in_buf[count] & 0xfc) >> 2];
		out_string[strptr + 1] = 
			base64code[((in_buf[count] & 0x03) << 4) | ((in_buf[count + 1] & 0xf0) >> 4)];
		out_string[strptr + 2] = base64code[(in_buf[count + 1] & 0x0f) << 2];
		out_string[strptr + 3] = base64code[64];
		strptr += 4;
	}
	out_string[strptr] = '\0';

}

int qnap_iscsit_check_received_cmdsn(
	struct iscsi_session *sess, 
	u32 cmdsn
	)
{
	/* This code was referred from iscsit_check_received_cmdsn(), we don't
	 * want too much but check status for cmdsn
	 */
	u32 max_cmdsn;
	int ret;

	/*
	 * This is the proper method of checking received CmdSN against
	 * ExpCmdSN and MaxCmdSN values, as well as accounting for out
	 * or order CmdSNs due to multiple connection sessions and/or
	 * CRC failures.
	 */
	max_cmdsn = atomic_read(&sess->max_cmd_sn);

	if (iscsi_sna_gt(cmdsn, max_cmdsn))
		ret = CMDSN_MAXCMDSN_OVERRUN;
	else if (cmdsn == sess->exp_cmd_sn)
		ret = CMDSN_NORMAL_OPERATION;
	else if (iscsi_sna_gt(cmdsn, sess->exp_cmd_sn))
		/* MC/S may come here */
		ret = CMDSN_HIGHER_THAN_EXP;
	else
		ret = CMDSN_LOWER_THAN_EXP;

	return ret;
}

#ifdef SUPPORT_SINGLE_INIT_LOGIN
/* Jonathan Ho, 20140416, one target can be logged in from only one initiator IQN */
int qnap_iscsit_search_tiqn_for_initiator(
	struct iscsi_tiqn *tiqn,
	char *InitiatorName)
{
	struct iscsi_portal_group *tpg = NULL;
	struct se_portal_group *se_tpg = NULL;
	struct qnap_se_nacl_dr *dr = NULL;
	struct qnap_se_node_acl *qnap_nacl = NULL;

	pr_debug("search Target: %s for Initiator IQN: %s\n", tiqn->tiqn, InitiatorName);
	spin_lock(&tiqn->tiqn_tpg_lock);
	list_for_each_entry(tpg, &tiqn->tiqn_tpg_list, tpg_list) {
		pr_debug("get Target Portal Group Tag: %hu\n", tpg->tpgt);
		spin_lock(&tpg->tpg_state_lock);
		if (tpg->tpg_state == TPG_STATE_FREE) {
			spin_unlock(&tpg->tpg_state_lock);
			continue;
		}
		spin_unlock(&tpg->tpg_state_lock);

		se_tpg = &tpg->tpg_se_tpg;
		dr = &se_tpg->se_nacl_dr;

		spin_lock(&dr->acl_node_lock);
		list_for_each_entry(qnap_nacl, &dr->acl_node_list, acl_node) {
			if (!strcmp(qnap_nacl->initiatorname, DEFAULT_INITIATOR))
				continue;

			if (!strcmp(qnap_nacl->initiatorname, FC_DEFAULT_INITIATOR))
				continue;

			if (strcmp(InitiatorName, qnap_nacl->initiatorname)) {
				pr_debug("get different, i_buf:%s, "
					"nacl initiator name: %s\n", 
					InitiatorName, qnap_nacl->initiatorname);
			
				/* make sure to unlock all spin lock before return */
				spin_unlock(&dr->acl_node_lock);
				spin_unlock(&tiqn->tiqn_tpg_lock);
				return -1;
			} else
				pr_debug("get same IQN: %s\n", qnap_nacl->initiatorname);
		}
		spin_unlock(&dr->acl_node_lock);
	}
	spin_unlock(&tiqn->tiqn_tpg_lock);
	return 0;
}
#endif /* SUPPORT_SINGLE_INIT_LOGIN */

#ifdef QNAP_KERNEL_STORAGE_V2

#define TMP_TGT_INFO_FILE_PAT	"/tmp/%s/%s/target_info"

static int __qnap_check_tmp_tgt_info_file(
	struct se_node_acl *se_nacl,
   	struct se_portal_group *se_tpg,
   	struct notify_data *n_data
	)
{
	struct se_wwn *wwn = NULL;
	struct file *fd = NULL;

	if (!se_nacl || !se_tpg)
		return -EINVAL;

	wwn = se_tpg->se_tpg_wwn;
	snprintf(n_data->path, sizeof(n_data->path), TMP_TGT_INFO_FILE_PAT, 
		wwn->wwn_group.cg_item.ci_name,se_nacl->initiatorname);

	fd = filp_open(n_data->path, O_CREAT | O_WRONLY | O_TRUNC , S_IRWXU);
	if (!IS_ERR(fd)) {
		filp_close(fd, NULL);
		return 0;
	}

	pr_err("%s: path error:%s \n", __func__, n_data->path);
	return -EINVAL;

}

static ssize_t __qnap_update_tmp_tgt_info_buf_s1(
	struct iscsi_session *sess,
	char *buf,
	ssize_t read_bytes
	)
{
	struct iscsi_sess_ops *sess_ops = sess->sess_ops;

	read_bytes += sprintf(buf + read_bytes, "SID,%u,%hu,"
		"%d,", sess->sid, sess->tsih, atomic_read(&sess->nconn));

	read_bytes += sprintf(buf + read_bytes, "%s,",
		(sess_ops->SessionType) ? "Discovery" : "Normal");

	read_bytes += sprintf(buf + read_bytes, 
		"%hu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
		sess_ops->TargetPortalGroupTag, sess_ops->InitialR2T, 
		sess_ops->ImmediateData, sess_ops->MaxOutstandingR2T, 
		sess_ops->FirstBurstLength, sess_ops->MaxBurstLength,
		sess_ops->DataSequenceInOrder, sess_ops->DataPDUInOrder,
		sess_ops->ErrorRecoveryLevel, sess_ops->MaxConnections,
		sess_ops->DefaultTime2Wait, sess_ops->DefaultTime2Retain
		);

	return read_bytes;
}

static ssize_t __qnap_update_tmp_tgt_info_buf_s2(
	struct iscsi_session *sess,
	struct iscsi_conn *conn,
	char *buf,
	ssize_t read_bytes,
	int first_conn
	)
{
	struct iscsi_sess_ops *sess_ops = sess->sess_ops;

	if (!first_conn) {
	        if(conn->conn_transport->name[0] != '\0'){
        	    read_bytes += sprintf(buf + read_bytes, "IQN,%s,%pISc,%lld,%s\n", 
                	    sess_ops->InitiatorName, &conn->login_sockaddr, 
	                    sess->login_time,conn->conn_transport->name);
        	}else{
	            read_bytes += sprintf(buf + read_bytes, "IQN,%s,%pISc,%lld\n", 
        	            sess_ops->InitiatorName, &conn->login_sockaddr, 
                	    sess->login_time);
		}
	} else
		read_bytes += sprintf(buf + read_bytes, 
			"!,%pISc\n", &conn->login_sockaddr);

	return read_bytes;
}

#endif /* QNAP_KERNEL_STORAGE_V2 */

#ifdef ISCSI_D4_INITIATOR
ssize_t qnap_lio_nacl_show_info(
	struct se_node_acl *se_nacl,
	char *page
	)
{
	struct iscsi_session *sess;
	struct iscsi_conn *conn;
	struct se_session *se_sess, *tmp_se_sess;
   	struct se_portal_group *se_tpg = NULL;
	struct se_node_acl *acl;
	struct iscsi_sess_ops *sess_ops;
	ssize_t rb = 0;
	bool is_default_initiator;

#ifdef QNAP_KERNEL_STORAGE_V2
	int first_conn;
	struct notify_data *n_data = NULL;

	/* TODO (shall be smart here ...)
	 * size for one target_info file per session (one conn) is about 115
	 * bytes now, we consider case about there are many sessions (conns)
	 * connect to one target ... hope the buffer is enough
	 */
#define DATA_SIZE	(8 * PAGE_SIZE)

#else /* !QNAP_KERNEL_STORAGE_V2 */

	char *tmp_page = NULL, *old_page = NULL;
	ssize_t old_rb = 0;
	int buf_result = 0;

#endif

	if (se_nacl)
		se_tpg = se_nacl->se_tpg;
	else {
		pr_err("%s:can't get the se_tpg\n", __func__);
		return rb;
	}

	if (se_tpg) {

		if (!strcmp(se_nacl->initiatorname, DEFAULT_INITIATOR))
			is_default_initiator = true;
		else if (!strcmp(se_nacl->initiatorname, FC_DEFAULT_INITIATOR))
			is_default_initiator = true;
		else
			is_default_initiator = false;

#ifdef QNAP_KERNEL_STORAGE_V2

		n_data = qnap_alloc_acl_notify_info_cache();
		if (!n_data) {
			pr_warn("%s: fail to alloc mem from "
				"acl_notify_info_cache\n", __func__);
			return rb;
		}

		n_data->se_nacl = se_nacl;

		spin_lock(&se_nacl->acl_read_info_lock);
		if (atomic_read(&se_nacl->acl_read_info) != 0) {
			spin_unlock(&se_nacl->acl_read_info_lock);
			pr_debug("%s: previuos reading is not finished, "
				"to exit\n", __func__);

			qnap_free_acl_notify_info_cache(n_data);
			return rb;
		}

		/* indicate we will read it now ... */
		atomic_inc_mb(&se_nacl->acl_read_info);
		spin_unlock(&se_nacl->acl_read_info_lock);

		spin_lock(&se_nacl->acl_drop_node_lock);
		if (atomic_read(&se_nacl->acl_drop_node) == 1){
			spin_unlock(&se_nacl->acl_drop_node_lock);
			pr_debug("%s: someone starts to drop acl data now, "
				"to exit now\n", __func__);

			if (qnap_put_acl_notify_info_null_work(n_data) == 0)
				return rb;
			goto _exit_;
		}
		spin_unlock(&se_nacl->acl_drop_node_lock);

		if (__qnap_check_tmp_tgt_info_file(se_nacl, se_tpg, 
				n_data) != 0) {
			if (qnap_put_acl_notify_info_null_work(n_data) == 0)
				return rb;
			goto _exit_;
		}

		n_data->data = NULL;
		n_data->data = vmalloc(DATA_SIZE);
		if (!n_data->data) {
			if (qnap_put_acl_notify_info_null_work(n_data) == 0)
				return rb;
			goto _exit_;
		}

#else /* !QNAP_KERNEL_STORAGE_V2 */

		/* since read of configfs only can carry PAGE_SIZE data, 
		 * we setup tmp buffer size to be (PAGE_SIZE * 2) first
		 */
		tmp_page = kzalloc((PAGE_SIZE * 2), GFP_KERNEL);
		if (!tmp_page)
			return rb;

		/* save original page address and use new one first */
		old_page = page;
		page = tmp_page;
#endif /* QNAP_KERNEL_STORAGE_V2 */
		mutex_lock(&se_tpg->acl_node_mutex);
		list_for_each_entry(acl, &se_tpg->acl_node_list, acl_list) {

			spin_lock_bh(&acl->nacl_sess_lock);

			/* Since we will get EVERY acl node in this tpg, we
			 * also need check whether passing se_nacl equals to 
			 * acl node from tpg or not. But, this condition shall
			 * be skipped for default initiator.
			 * This shall cowork with solution of bugzilla 79317
			 */
			if ((se_nacl != acl) && (is_default_initiator == false)) {
				spin_unlock_bh(&acl->nacl_sess_lock);
				continue;
			}

			/* please refer __transport_register_session()
			 * so we safe do this 
			 */
			if (!acl->nacl_sess) {
				spin_unlock_bh(&acl->nacl_sess_lock);
				continue;
			}

			/* get each sess from acl node */
			list_for_each_entry_safe(se_sess, tmp_se_sess, 
					&acl->acl_sess_list, sess_acl_list) 

			{
				sess = (struct iscsi_session *)se_sess->fabric_sess_ptr;
				sess_ops = sess->sess_ops;

				spin_lock(&sess->conn_lock);
				if (atomic_read(&sess->sess_now_release)) {
					spin_unlock(&sess->conn_lock);
					continue;
				}

#ifdef QNAP_KERNEL_STORAGE_V2
				rb = __qnap_update_tmp_tgt_info_buf_s1(sess, 
					n_data->data, rb);

				/* force to go this way */
				goto _show_conn_;
#endif
				/* Benjamin 20121214 add for showing the
				 * smi-s session parameters. 
				 */
				rb += sprintf(page+rb, "Session ID=%u,TSIH=%hu,"
					"CurrentConnections=%d,", sess->sid, 
					sess->tsih, atomic_read(&sess->nconn));
			
				rb += sprintf(page+rb, "SessionType=%s,",
					(sess_ops->SessionType) ? 
					"Discovery" : "Normal");
			
				rb += sprintf(page+rb, 
					TARGETPORTALGROUPTAG"=%hu,"INITIALR2T"=%u,"
					IMMEDIATEDATA"=%u,"MAXOUTSTANDINGR2T"=%u,"
					FIRSTBURSTLENGTH"=%u,"MAXBURSTLENGTH"=%u,"
					DATASEQUENCEINORDER"=%u,"DATAPDUINORDER"=%u,"
					ERRORRECOVERYLEVEL"=%u,"MAXCONNECTIONS"=%u,"
					DEFAULTTIME2WAIT"=%u,"DEFAULTTIME2RETAIN"=%u\n",
					sess_ops->TargetPortalGroupTag, sess_ops->InitialR2T, 
					sess_ops->ImmediateData, sess_ops->MaxOutstandingR2T, 
					sess_ops->FirstBurstLength, sess_ops->MaxBurstLength,
					sess_ops->DataSequenceInOrder, 
					sess_ops->DataPDUInOrder, sess_ops->ErrorRecoveryLevel, 
					sess_ops->MaxConnections, sess_ops->DefaultTime2Wait, 
					sess_ops->DefaultTime2Retain);

				// show the first connection only
#ifdef QNAP_KERNEL_STORAGE_V2
_show_conn_:
				first_conn = 0;
#endif
				list_for_each_entry(conn, &sess->sess_conn_list, 
					conn_list) 
				{
					if (conn) {
#ifdef QNAP_KERNEL_STORAGE_V2
						rb = __qnap_update_tmp_tgt_info_buf_s2(
							sess, conn, n_data->data, 
							rb, first_conn);

						if (!first_conn)
							first_conn = 1;

						/* force to go this way */
						continue;
#endif
						rb += sprintf(page + rb, 
						"InitiatorName=%s,IP=%pISc,Login=%lld\n",
						sess_ops->InitiatorName, 
						conn->login_sockaddr, sess->login_time);
					}
				}
				spin_unlock(&sess->conn_lock);
			}
			spin_unlock_bh(&acl->nacl_sess_lock);

#ifndef QNAP_KERNEL_STORAGE_V2
			/* Benjamin 20130315 for BUG 31457: fill_read_buffer()
			 * in configfs can only read one page. 
			 * If exceeds, BUG_ON! 
			 */
			if (rb > PAGE_SIZE) {
				buf_result = -ENOMEM;
				break;
			} else
				old_rb = rb;
#endif
		}
		mutex_unlock(&se_tpg->acl_node_mutex);

#ifdef QNAP_KERNEL_STORAGE_V2
		if (rb) {
			n_data->data_size = rb;

			/* if someone is removing tree now, we put the work to
			 * workqueue
			 */
			spin_lock(&se_nacl->acl_drop_node_lock);
			if (atomic_read(&se_nacl->acl_drop_node) == 1){
				spin_unlock(&se_nacl->acl_drop_node_lock);

				if (qnap_put_acl_notify_info_work(n_data) == 0)
					return 0;	
			}
			spin_unlock(&se_nacl->acl_drop_node_lock);

			/* if nobody is removing tree or fail to put wq, 
			 * try normal path 
			 */
			qnap_target_nacl_update_show_info(n_data);
		} 

		vfree(n_data->data);

		if (rb == 0) {
			if (qnap_put_acl_notify_info_null_work(n_data) == 0)
				return 0;
		}
_exit_:
		qnap_free_acl_notify_info_cache(n_data);
	
		spin_lock(&se_nacl->acl_read_info_lock);
		atomic_dec_mb(&se_nacl->acl_read_info);
		spin_unlock(&se_nacl->acl_read_info_lock);
		rb = 0;

#else /* !QNAP_KERNEL_STORAGE_V2 */

		/* restore original page address */
		page = old_page;

		if (!buf_result)
			memcpy(page, tmp_page, old_rb);
		else {
			if (old_rb) {
				memcpy(page, tmp_page, old_rb);
				pr_warn("%s: nacl info exceeds PAGE_SIZE, "
					"stop to get remain info\n", __func__);			
			} else
				pr_err("%s: fail to get nacl info at "
					"1st round ( > PAGE_SIZE )\n", __func__);
		}

		rb = old_rb;
		kfree(tmp_page);
#endif
	}
	return rb;
}

static void __qnap_iscsit_copy_node_attribues(
	struct iscsi_node_acl *dest_acl, 
	struct iscsi_node_acl *src_acl
	)
{
	struct iscsi_node_attrib *src_a = &src_acl->node_attrib;
	struct iscsi_node_attrib *dest_a = &dest_acl->node_attrib;
	struct iscsi_node_auth *src_auth = &src_acl->node_auth;
	struct iscsi_node_auth *dest_auth = &dest_acl->node_auth;
	
	dest_a->dataout_timeout = src_a->dataout_timeout;
	dest_a->dataout_timeout_retries = src_a->dataout_timeout_retries;
	dest_a->nopin_timeout = src_a->nopin_timeout;
	dest_a->nopin_response_timeout = src_a->nopin_response_timeout;
	dest_a->random_datain_pdu_offsets = src_a->random_datain_pdu_offsets;
	dest_a->random_datain_seq_offsets = src_a->random_datain_seq_offsets;
	dest_a->random_r2t_offsets = src_a->random_r2t_offsets;
	dest_a->default_erl = src_a->default_erl;
	
	// should copy the auth parameters as well
	// struct config_group	   auth_attrib_group;
	dest_auth->naf_flags = src_auth->naf_flags;
	dest_auth->authenticate_target = src_auth->authenticate_target;
	strcpy(dest_auth->userid, src_auth->userid);
	//printk("Nike src_auth = %p, copy userid = %s.\n", src_auth, dest_auth->userid);
	strcpy(dest_auth->password, src_auth->password);
	//printk("Nike copy password = %s.\n", dest_auth->password);
	strcpy(dest_auth->userid_mutual, src_auth->userid_mutual);
	//printk("Nike copy userid_mutual = %s.\n", dest_auth->userid_mutual);
	strcpy(dest_auth->password_mutual, src_auth->password_mutual);
	//printk("Nike copy password_mutual = %s.\n", dest_auth->password_mutual);
	return;
}


void qnap_lio_copy_node_attributes(
	struct se_node_acl *dest, 
	struct se_node_acl *src
	)
{
	struct iscsi_node_acl *dest_acl = 
		container_of(dest, struct iscsi_node_acl, se_node_acl);

	struct iscsi_node_acl *src_acl = 
		container_of(src, struct iscsi_node_acl, se_node_acl);
    
	dest_acl->node_attrib.nacl = dest_acl;
	__qnap_iscsit_copy_node_attribues(dest_acl, src_acl);
	return;
}
#endif /* ISCSI_D4_INITIATOR */

int qnap_iscsi_lio_drop_cmd_from_lun_acl(
	struct se_lun *se_lun
	)
{
	/* this function only will be used on case about if 
	 * someone will delete se_lun (se_dev) from lun_acl 
	 * even the target is still online
	 */
	LIST_HEAD(free_cmd_list);
	struct se_node_acl *se_nacl;
	struct se_session *se_sess;
	struct se_portal_group *se_tpg;
	struct se_cmd *se_cmd;
	struct iscsi_session *i_sess;
	struct iscsi_conn *i_conn;
	struct iscsi_cmd *i_cmd, *i_cmd_p;
	
	if (!se_lun->lun_tpg)
		return -EINVAL;
	
	se_tpg = se_lun->lun_tpg;

	/* find out all conns per sess on the tpg for this lun .... */
	mutex_lock(&se_tpg->acl_node_mutex);

	list_for_each_entry(se_nacl, &se_tpg->acl_node_list, acl_list) {

		spin_lock_bh(&se_nacl->nacl_sess_lock);
		se_sess = se_nacl->nacl_sess;
		if (!se_sess) {
			spin_unlock_bh(&se_nacl->nacl_sess_lock);
			continue;
		}
	
		i_sess = (struct iscsi_session *)se_sess->fabric_sess_ptr;

		/* check all conns per sess */
		spin_lock(&i_sess->conn_lock);
		list_for_each_entry(i_conn, &i_sess->sess_conn_list, conn_list) {

			/* take care use list_for_each_entry_safe() since we 
			 * will use list_move_tail() later
			 */
			spin_lock(&i_conn->cmd_lock);
			list_for_each_entry_safe(i_cmd, i_cmd_p, &i_conn->conn_cmd_list, 
				i_conn_node) 
			{
				if (i_cmd->iscsi_opcode != ISCSI_OP_SCSI_CMD)
					continue;

				se_cmd = &i_cmd->se_cmd;
				if (se_lun->lun_se_dev != se_cmd->se_dev)
					continue;

				/* if backend se_dev for se_lun matches
				 * the cmd->se_cmd.se_dev, try drop cmds
				 */		
				pr_debug("%s: found i_cmd:0x%p,se_cmd:0x%p, "
					"se_dev:0x%p\n", __func__, i_cmd, 
					se_cmd, se_cmd->se_dev);

				list_move_tail(&i_cmd->i_conn_node, &free_cmd_list);
			}
			spin_unlock(&i_conn->cmd_lock);
		}
		spin_unlock(&i_sess->conn_lock);
		spin_unlock_bh(&se_nacl->nacl_sess_lock);
	}
	mutex_unlock(&se_tpg->acl_node_mutex);
	

	list_for_each_entry_safe(i_cmd, i_cmd_p, &free_cmd_list, i_conn_node) {
		list_del_init(&i_cmd->i_conn_node);
		iscsit_free_cmd(i_cmd, false);
	}

	return 0;
}

#ifdef ISCSI_MULTI_INIT_ACL
int qnap_iscsit_get_matched_initiator_count(
	struct iscsi_tiqn *tiqn,
	char *initiator_name
	)
{
	/* take care this call due to tiqn_lock needs be locked before call this */

	int tmp_acl_count = 0;
	struct iscsi_portal_group *tpg = NULL;
	struct qnap_se_nacl_dr *dr = NULL;
	struct qnap_se_node_acl *tmp_acl = NULL;
	
	spin_lock(&tiqn->tiqn_tpg_lock);
	list_for_each_entry(tpg, &tiqn->tiqn_tpg_list, tpg_list) {

		dr = &tpg->tpg_se_tpg.se_nacl_dr;

		spin_lock(&dr->acl_node_lock);
		list_for_each_entry(tmp_acl, &dr->acl_node_list, acl_node) {
			if (!(strcmp(tmp_acl->initiatorname, DEFAULT_INITIATOR))
			|| !(strcmp(tmp_acl->initiatorname, FC_DEFAULT_INITIATOR))
			|| !(strcasecmp(tmp_acl->initiatorname, initiator_name))
			)
				tmp_acl_count++;
		}
		spin_unlock(&dr->acl_node_lock);
	}
	spin_unlock(&tiqn->tiqn_tpg_lock);

	return tmp_acl_count;
}


#endif

#ifdef ISCSI_LOGIN_REDIRECT
/* Separate char ip:port into char ip and u16 port,
 * e.g.,str = 10.10.1.1:3260, ip = 10.10.1.1, port = 3260
 */
int sepstrtoipport(const char *str,
    char *get_ip,
    u16 *get_port)
{
    char buf[MAX_REDIRECT_IPPORT_LEN + 1];
    char *ip_str = NULL, *port_str = NULL;
    u16 port = 0;
    int ret = 0;

    memset(buf, 0, MAX_REDIRECT_IPPORT_LEN + 1);
    snprintf(buf, MAX_REDIRECT_IPPORT_LEN + 1, "%s", str);

    ip_str = &buf[0];
    port_str = strstr(ip_str, ":");
    if (!port_str) {
        pr_err("Unable to locate \":\" ignoring request.\n");
        ret = -EINVAL;
        goto out;
    }
    *port_str = '\0';
    port_str++;
    snprintf(get_ip, IPV6_ADDRESS_SPACE, "%s", ip_str);

    ret = kstrtou16(port_str, 0, &port);
    if (ret < 0) {
        pr_err("kstrtoul() ret:%d, port_str:%s port:%d\n",
                ret, port_str, port);
        goto out;
    }
    *get_port = port;

out:
    return ret;
}

void login_redirect_cnt_add(
    struct iscsi_conn *conn,
    const char *ip,
    u16 port)
{
    struct iscsi_redirect *redirect = NULL;
    int ret = 0;

    list_for_each_entry(
            redirect, &conn->tpg->tpg_redirect.tpg_redirect_list,
            redirect_list) {
        if (!strncmp(redirect->ip, ip, IPV6_ADDRESS_SPACE) &&
                redirect->port == port) {
            redirect->cnt++;
        }
    }
}

int login_redirect_algo_rr(
    struct iscsi_conn *conn,
    char *get_ip,
    u16 *get_port,
    u8 *get_type)
{
    struct iscsi_redirect *redirect_tmp = NULL;
    struct iscsi_redirect *redirect_lt = NULL; //Last time picked ip:port
    struct iscsi_redirect *redirect_next = NULL;
    struct iscsi_redirect *redirect_picked = NULL;
    int ret = 0;

    list_for_each_entry(
            redirect_tmp, &conn->tpg->tpg_redirect.tpg_redirect_list,
            redirect_list) {
        if (redirect_tmp->algo_rr_picked) {
            redirect_tmp->algo_rr_picked = false;
            redirect_lt = redirect_tmp;
            break;
        }
    }
    redirect_tmp = NULL;

    /* Check the list from next of last time picked to the tail
     * to pick a ip:port
     */
    redirect_next = list_next_entry(redirect_lt, redirect_list);
    redirect_tmp = list_prepare_entry(redirect_next,
            &conn->tpg->tpg_redirect.tpg_redirect_list, redirect_list);
    list_for_each_entry_from(redirect_tmp,
            &conn->tpg->tpg_redirect.tpg_redirect_list,redirect_list) {
        if (redirect_tmp->enable) {
            redirect_tmp->algo_rr_picked = true;
            redirect_picked = redirect_tmp;
            break;
        }
    }
    redirect_tmp = NULL;

    /* Check the list from the first to last time picked
     * to pick a ip:port
     */
    if (!redirect_picked) {
        list_for_each_entry(redirect_tmp,
                &conn->tpg->tpg_redirect.tpg_redirect_list, redirect_list) {
            if (redirect_tmp == redirect_lt) {
                if (redirect_tmp->enable) {
                    redirect_tmp->algo_rr_picked = true;
                    redirect_picked = redirect_tmp;
                } else {
                    redirect_picked = NULL;
                }
                break;
            } else {
                if (redirect_tmp->enable) {
                    redirect_tmp->algo_rr_picked = true;
                    redirect_picked = redirect_tmp;
                    break;
                }
            }
        }
    }
    redirect_tmp = NULL;

    /* Finish flow of login redirect and send response when setting is wrong */
    if (!redirect_picked) {
        pr_err("iSCSI Login Redirect setting or algorithm is wrong.\n");
        ret = iscsit_tx_login_rsp(
                conn, ISCSI_STATUS_CLS_TARGET_ERR,
                ISCSI_LOGIN_STATUS_TARGET_ERROR);
        ret = -1;
    } else if (redirect_picked->type != ISCSI_LOGIN_STATUS_TGT_MOVED_TEMP &&
            redirect_picked->type != ISCSI_LOGIN_STATUS_TGT_MOVED_PERM) {
        pr_err("iSCSI Login Redirect setting wrong.\n");
        ret = iscsit_tx_login_rsp(
                conn, ISCSI_STATUS_CLS_TARGET_ERR,
                ISCSI_LOGIN_STATUS_TARGET_ERROR);
        ret = -1;
    } else {
        snprintf(get_ip, IPV6_ADDRESS_SPACE, "%s", redirect_picked->ip);
        *get_port = redirect_picked->port;
        *get_type = redirect_picked->type;
    }

    return ret;
}

int iscsit_tx_login_rsp_redirect(
    struct iscsi_conn *conn,
    u8 status_class,
    u8 status_detail)
{
    struct iscsi_login_rsp *hdr = NULL;
    struct iscsi_login *login = conn->conn_login;
    int ret = 0, len = 0;
    u32 padding = 0, payload_length = 0;
    unsigned char buf[MAX_KEY_VALUE_PAIRS];
    char ip[IPV6_ADDRESS_SPACE];
    u16 port = 0;


    memset(ip, 0, IPV6_ADDRESS_SPACE);
    memset(buf, 0, sizeof(buf));
    memset(login->rsp_buf, 0, MAX_KEY_VALUE_PAIRS);
    if (conn->tpg->tpg_redirect.algo == ISCSI_LOGIN_REDIRECT_ALGO_RR) {
        ret = login_redirect_algo_rr(conn, &ip[0], &port, &status_detail);
        if (ret)
            goto out;
    } else {
        pr_err("iSCSI Login Redirect setting wrong.\n");
        ret = iscsit_tx_login_rsp(
                conn, ISCSI_STATUS_CLS_TARGET_ERR,
                ISCSI_LOGIN_STATUS_TARGET_ERROR);
        goto out;
    }
    login_redirect_cnt_add(conn, &ip[0], port);

    login->login_failed = 1;
    iscsit_collect_login_stats(conn, status_class, status_detail);

    memset(&login->rsp[0], 0, ISCSI_HDR_LEN);

    hdr = (struct iscsi_login_rsp *)&login->rsp[0];
    hdr->opcode     = ISCSI_OP_LOGIN_RSP;
    /* Follow SCST */
    hdr->flags     |= ISCSI_FLAG_LOGIN_CURRENT_STAGE1;
    hdr->status_class   = status_class;
    hdr->status_detail  = status_detail;
    hdr->itt        = conn->login_itt;

    len = snprintf(buf, sizeof(buf), "TargetAddress=%s:%u",
                    ip, port);
    len += 1;

    memcpy(login->rsp_buf, buf, len);
    hton24(hdr->dlength, strlen(login->rsp_buf) + 1);
    payload_length = ntoh24(hdr->dlength);
    padding = ((-payload_length) & 3);

    pr_err("iSCSI Login Target from %pISpc Redirect to %s:%u.\n",
            &conn->login_sockaddr, ip, port);
    ret = conn->conn_transport->iscsit_put_login_tx(
            conn, login, payload_length + padding);
    memset(login->rsp_buf, 0, MAX_KEY_VALUE_PAIRS);
out:
    return ret;
}

void login_redirect_cnt_sub(
    struct iscsi_conn *conn)
{
    char ip_port[IPV6_ADDRESS_SPACE] = "";
    char ip[IPV6_ADDRESS_SPACE] = "";
    u16 port = 0;
    int ret = 0;
    struct iscsi_redirect *redirect = NULL;

    snprintf(ip_port, IPV6_ADDRESS_SPACE, "%pISpc", &conn->login_sockaddr);

    ret = sepstrtoipport(ip_port, ip, port);

    list_for_each_entry(
            redirect, &conn->tpg->tpg_redirect.tpg_redirect_list,
            redirect_list) {
        if (conn->tpg->tpg_redirect.algo != ISCSI_LOGIN_REDIRECT_ALGO_NONE &&
                !strncmp(redirect->ip, ip, IPV6_ADDRESS_SPACE) &&
                redirect->port == port) {
            redirect->cnt--;
        }
    }
}
#endif /* ISCSI_LOGIN_REDIRECT */

char *qnap_iscsi_get_initiator_name(
	struct se_cmd *se_cmd
	)
{
	struct iscsi_cmd *cmd = container_of(se_cmd, struct iscsi_cmd, se_cmd);
	struct iscsi_conn *conn = cmd->conn;
	struct iscsi_session *sess = conn->sess;

	return sess->sess_ops->InitiatorName;
}

u32 qanp_iscsi_get_task_tag(
	struct se_cmd *se_cmd
	)
{
	struct iscsi_cmd *cmd = container_of(se_cmd, struct iscsi_cmd, se_cmd);

	return (__force u32)cmd->init_task_tag;
}

struct sockaddr_storage *qnap_iscsi_get_login_ip(
	struct se_cmd *se_cmd
	)
{
	struct iscsi_cmd *cmd = container_of(se_cmd, struct iscsi_cmd, se_cmd);
	if (cmd->conn)
		return &cmd->conn->login_sockaddr;
	return NULL;
}

u32 qnap_iscsi_get_cmdsn(
	struct se_cmd *se_cmd
	)
{
	struct iscsi_cmd *cmd = container_of(se_cmd, struct iscsi_cmd, se_cmd);
	return cmd->cmd_sn;
}

bool qnap_check_inaddr_loopback(struct sockaddr_storage *ss)
{
	bool ret = false;

	if (ss->ss_family == AF_INET6) {
		const struct sockaddr_in6 sin6 = {
			.sin6_addr = IN6ADDR_LOOPBACK_INIT };
		struct sockaddr_in6 *sock_in6 = (struct sockaddr_in6 *)ss;

		if (!memcmp(sock_in6->sin6_addr.s6_addr,
				sin6.sin6_addr.s6_addr, 16))
			ret = true;
	} else {
		struct sockaddr_in * sock_in = (struct sockaddr_in *)ss;

		if (sock_in->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
			ret = true;
	}

	return ret;
}

void qnap_odx_iscsit_it_nexus_discard_all_tokens(
	struct iscsi_conn *conn
	)
{
	struct iscsi_session *sess = conn->sess;
	struct iscsi_portal_group *tpg = sess->tpg;
	struct se_portal_group *se_tpg = &tpg->tpg_se_tpg;
	const struct target_core_fabric_ops *tfo = se_tpg->se_tpg_tfo;	
	u8 isid_buf[PR_REG_ISID_LEN];
	char login_ip[IPV6_ADDRESS_SPACE];

	if (atomic_read(&sess->nconn) != 1)
		return;

	/* to discard all tokens if this is last connection if connection was 
	 * closed or session reinstatment case
	 */
	if (tfo->q_tf_ops && tfo->q_tf_ops->odx_tf_ops) {
		if (tfo->q_tf_ops->odx_tf_ops->it_nexus_discard_all_tokens) {
			snprintf(login_ip, sizeof(login_ip), "%pISc", 
							&conn->login_sockaddr);
			tfo->sess_get_initiator_sid(sess->se_sess, isid_buf, 
								PR_REG_ISID_LEN);

			tfo->q_tf_ops->odx_tf_ops->it_nexus_discard_all_tokens(
					sess->sess_ops->InitiatorName, isid_buf, 
					login_ip, &se_tpg->odx_dr);
		}
	}

}

struct qnap_tf_ops qnap_tf_ops_table = {
	/* odx_tf_ops will be setup in qnap_odx_transport_init() */
	.odx_tf_ops = NULL,
};

/*
 * This function will check if the request IP address (conn->local_sockaddr) is in the allow network potal of specific Target.
 * If yes, if will return allow_flag = 1, otherwise, it will return allow_flag = 0.
 */
u8 qnap_iscsit_check_allow_np(struct iscsi_portal_group *tpg, struct iscsi_conn *conn, enum iscsit_transport_type network_transport)
{
	struct iscsi_tpg_np *tpg_np;
	u8 allow_flag = 0;
	char client_ip[IPV6_ADDRESS_SPACE] = "";
	char np_ip[IPV6_ADDRESS_SPACE] = "";

	snprintf(client_ip, sizeof(client_ip), "%pISc", &conn->local_sockaddr);

	spin_lock(&tpg->tpg_np_lock);
	list_for_each_entry(tpg_np, &tpg->tpg_gnp_list, tpg_np_list) {
		struct iscsi_np *np = tpg_np->tpg_np;
		struct sockaddr_storage *sockaddr;

		/* Ignore loopback IP */
		bool login_loopback, np_loopback;

		if (np->np_sockaddr.ss_family != conn->login_sockaddr.ss_family)
			continue;

		login_loopback = qnap_check_inaddr_loopback(&conn->login_sockaddr);
		np_loopback = qnap_check_inaddr_loopback(&np->np_sockaddr);

		if (!login_loopback && np_loopback)
			continue;

		if (np->np_network_transport != network_transport)
			continue;

		if (inet_addr_is_any((struct sockaddr *)&np->np_sockaddr))
			sockaddr = &conn->local_sockaddr;
		else
			sockaddr = &np->np_sockaddr;

		snprintf(np_ip, sizeof(np_ip), "%pISc", sockaddr);

		if (!strcmp(client_ip, np_ip)) {
			allow_flag = 1;
			break;
		}
			
	}
	spin_unlock(&tpg->tpg_np_lock);

	return allow_flag;
}

#endif /* CONFIG_MACH_QNAPTS */
