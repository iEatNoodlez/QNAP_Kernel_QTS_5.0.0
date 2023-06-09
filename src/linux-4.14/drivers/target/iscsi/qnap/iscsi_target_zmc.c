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

#include "../iscsi_target.h"
#include "../iscsi_target_util.h"

#ifdef CONFIG_MACH_QNAPTS
#ifdef QNAP_ISCSI_TCP_ZMC_FEATURE

/**/
unsigned int iscsi_tcp_zmc_enable = 0;
module_param_named(iscsi_tcp_zmc_en, iscsi_tcp_zmc_enable, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(iscsi_tcp_zmc_en, "iscsi tcp zero-memory-copy feature (0:disabled, 1:enabled)");
EXPORT_SYMBOL(iscsi_tcp_zmc_enable);


/**/
int tcp_zmc_set_cb = 0;


#include <uapi/asm-generic/ioctls.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <net/tcp.h>
#include <net/sock.h>

/**/
extern ssize_t do_tcp_sg_sendpages_locked(
	struct sock *sk,
	struct scatterlist *sgl,
	int first_sg_offset,
	size_t total_size,
	int flags
	);

/* iscsi zmc conn ops */
static int qnap_iscsit_tcp_zmc_set_cb_check__(struct iscsi_conn *conn);
static int qnap_iscsit_tcp_zmc_set_sock_callbacks__(struct iscsi_conn *conn);
static int qnap_iscsit_tcp_zmc_restore_sock_callbacks__(struct iscsi_conn *conn);
static int qnap_iscsit_tcp_zmc_get_rx_pdu__(struct iscsi_conn *conn);


/* iscsi zmc cmd ops */

static int qnap_iscsit_tcp_zmc_dump_data_payload__(struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,	u32 buf_len, int dump_padding_digest);

static int qnap_iscsit_tcp_zmc_handle_immediate_data__(struct iscsi_cmd *cmd,
	struct iscsi_scsi_req *hdr, u32 length);

static int qnap_iscsit_tcp_zmc_get_dataout__(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, struct iscsi_data *hdr);

static int qnap_iscsit_tcp_zmc_xmit_datain_pdu__(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, const struct iscsi_datain *datain);


/**/
struct kmem_cache *zmc_obj2cmd_cache = NULL;

struct iscsit_zmc_conn_ops __conn_ops = {
	.set_cb_check		= qnap_iscsit_tcp_zmc_set_cb_check__,
	.set_sock_callbacks	= qnap_iscsit_tcp_zmc_set_sock_callbacks__,
	.restore_sock_callbacks	= qnap_iscsit_tcp_zmc_restore_sock_callbacks__,
	.get_rx_pdu		= qnap_iscsit_tcp_zmc_get_rx_pdu__,
};

struct iscsit_zmc_cmd_ops __cmd_ops = {
	.dump_data_payload	= qnap_iscsit_tcp_zmc_dump_data_payload__,
	.get_dataout		= qnap_iscsit_tcp_zmc_get_dataout__,
	.handle_immediate_data	= qnap_iscsit_tcp_zmc_handle_immediate_data__,
	.xmit_datain_pdu	= qnap_iscsit_tcp_zmc_xmit_datain_pdu__,
};


/**/
static u8 hex[] = {
	'0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};


void __dump_mem(
	u8 *buf,
	size_t dump_size
	)
{
	u8 *Data, Val[50], Str[20], c;
	size_t Size, Index;

	Data = buf;

	while (dump_size) {
		Size = 16;
		if (Size > dump_size)
			Size = dump_size;

		for (Index = 0; Index < Size; Index += 1) {
			c = Data[Index];
			Val[Index * 3 + 0] = hex[c >> 4];
			Val[Index * 3 + 1] = hex[c & 0xF];
			Val[Index * 3 + 2] = (u8) ((Index == 7) ? '-' : ' ');
			Str[Index] = (u8) ((c < ' ' || c > 'z') ? '.' : c);
		}

		Val[Index * 3] = 0;
		Str[Index] = 0;
		pr_info("addr-0x%p: %s *%s*\n",Data, Val, Str);
		Data += Size;
		dump_size -= Size;
	}
	return;
}


static int __skb_nsg(
	struct sk_buff *skb, 
	int offset, 
	int len,
	unsigned int recursion_level
	)
{
	int start = skb_headlen(skb);
	int i, chunk = start - offset;
	struct sk_buff *frag_iter;
	int elt = 0;

	if (unlikely(recursion_level >= 48)) {
		WARN_ON(true);
		return -EMSGSIZE;
	}

	if (chunk > 0) {
		if (chunk > len)
			chunk = len;
		elt++;
		len -= chunk;
		if (len == 0)
			return elt;
		offset += chunk;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;
		WARN_ON(start > offset + len);

		end = start + skb_frag_size(&skb_shinfo(skb)->frags[i]);
		chunk = end - offset;
		if (chunk > 0) {
			if (chunk > len)
				chunk = len;
			elt++;
			len -= chunk;
			if (len == 0)
				return elt;
			offset += chunk;
		}
		start = end;
	}

	if (unlikely(skb_has_frag_list(skb))) {
		skb_walk_frags(skb, frag_iter) {
			int end, ret;
			WARN_ON(start > offset + len);

			end = start + frag_iter->len;
			chunk = end - offset;
			if (chunk > 0) {
				if (chunk > len)
					chunk = len;
				ret = __skb_nsg(frag_iter, offset - start, chunk,
					recursion_level + 1);
				if (unlikely(ret < 0))
					return ret;
				elt += ret;
				len -= chunk;
				if (len == 0)
					return elt;
				offset += chunk;
			}
			start = end;
		}
	}

	BUG_ON(len);
	return elt;
}

/* Refer from https://www.spinics.net/lists/netdev/msg520370.html, THANKS opensource
 *
 * Return the number of scatterlist elements required to completely map the
 * skb, or -EMSGSIZE if the recursion depth is exceeded.
 */
static int skb_nsg(struct sk_buff *skb, int offset, int len)
{
	return __skb_nsg(skb, offset, len, 0);
}

static struct obj2cmd_node *qnap_iscsit_zmc_alloc_obj2cmd_cache(void)
{
	struct obj2cmd_node *node = NULL;

	if (!zmc_obj2cmd_cache)
		return NULL;

	node = kmem_cache_zalloc(zmc_obj2cmd_cache, 
		((in_interrupt()) ? GFP_ATOMIC: GFP_KERNEL));

	if (node) {
		node->obj = NULL;
		node->next = NULL;
	}
	return node;
}

static void qnap_iscsit_zmc_free_obj2cmd_cache(
	struct obj2cmd_node *p
	)
{
	if (zmc_obj2cmd_cache && p)
		kmem_cache_free(zmc_obj2cmd_cache, p);
}

static int qnap_iscsit_zmc_push_obj2cmd__(
	struct iscsi_cmd *cmd,
	struct zmc_obj *obj
	)
{
	struct obj2cmd_node *new = NULL, *curr = NULL;

	new = qnap_iscsit_zmc_alloc_obj2cmd_cache();
	if (!new)
		return -ENOMEM;

	new->obj = obj;

	if (!cmd->obj2cmd_node) {
		cmd->obj2cmd_node = new;
		ISCSI_ZMC_DEBUG("push new obj2cmd:0x%p\n", new);
	} else {
		curr = (struct obj2cmd_node *)cmd->obj2cmd_node;
		while (curr->next)
			curr = curr->next;
		curr->next = new;
		ISCSI_ZMC_DEBUG("push new obj2cmd:0x%p after curr obj2cmd:0x%p\n", 
			new, curr);
	}

	return 0;
}

static bool qnap_iscsit_zmc_search_obj2cmd__(
	struct iscsi_cmd *cmd,
	struct zmc_obj *obj
	)
{
	struct obj2cmd_node *curr = NULL, *head = NULL;
	bool found = false;

	head = (struct obj2cmd_node *)cmd->obj2cmd_node;
	if (!head)
		goto exit;

	curr = head;
	while (curr) {
		if (curr->obj == obj) {
			found = true;
			break;
		}
		curr = curr->next;
	}
exit:
#if 0
	if (atomic_read(&obj->touch) > 2) {
		ZMC_INFO("%s find obj:0x%p in cmd:0x%p\n", 
			((found) ? "" : "not"), obj, cmd);
	}
#endif

	ISCSI_ZMC_DEBUG("%s find obj:0x%p in cmd:0x%p\n", 
			((found) ? "" : "not"), obj, cmd);
	return found;
}

static int __qnap_iscsit_zmc_sw_recv(
	read_descriptor_t *rd_desc, 
	struct sk_buff *skb,
	unsigned int offset, 
	size_t len
	)
{
	struct iscsi_conn *conn = rd_desc->arg.data;
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct zmc_obj *obj = NULL;

	ISCSI_ZMC_DEBUG("%s: conn: %p, skb_len:%d, off:%d, len:%d, "
			"skb_cb->seq:0x%x\n", __func__, conn, skb->len, 
			offset, len, TCP_SKB_CB(skb)->seq);

	/* The skb will be removed from sk_receive_queue list via sk_eat_skb()
	 * if process it successfully. If fail, it is still in list.
	 * For objs we collected before, just keep them due to we may have
	 * chances to handle under failure case
	 */
	obj = kmem_cache_zalloc(dr->obj_cache, GFP_ATOMIC);
	if (!obj)
		return -ENOMEM;

	obj->skb = skb_clone(skb, GFP_ATOMIC);
	if (!obj->skb) {
		kmem_cache_free(dr->obj_cache, obj);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&obj->obj_node);
	obj->offset = offset;
	obj->len = len;

	INIT_LIST_HEAD(&obj->work_node);
	obj->work_len = 0;
	obj->work_off = 0;
	obj->cmd = NULL;
	atomic_set(&obj->touch, 1);

	list_add_tail(&obj->obj_node, &dr->local_obj_list);
	atomic_inc(&dr->obj_cnt);

	ISCSI_ZMC_DEBUG("%s: conn: %p, read %d bytes, len:%d\n", __func__, 
			conn, skb->len - offset, len);
	return len;
}

static inline int __qnap_iscsit_zmc_sk_state_check(struct sock *sk)
{
	if (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE) {
		ISCSI_ZMC_WARN("%s: got TCP_CLOSE or TCP_CLOSE_WAIT\n", __func__);
		return -ECONNRESET;
	}
	return 0;
}

static void __qnap_iscsit_zmc_sk_data_ready(struct sock *sk)
{
	struct iscsi_conn *conn = NULL;
	struct zmc_dr *dr = NULL;
	read_descriptor_t rd_desc;
	bool rc;
	int copied;

	ISCSI_ZMC_DEBUG("Entering %s: conn: %p\n", __func__, conn);

	read_lock_bh(&sk->sk_callback_lock);
	if (!sk->sk_user_data) {
		read_unlock_bh(&sk->sk_callback_lock);
		return;
	}

	conn = (struct iscsi_conn *)sk->sk_user_data;
	dr = (struct zmc_dr *)conn->zmc_dr; 
	/*
	 * Use rd_desc to pass 'conn' to __qnap_iscsit_zmc_sw_recv.
	 * We set count to 1 because we want the network layer to
	 * hand us all the skbs that are available.
	 */
	rd_desc.arg.data = conn;
	rd_desc.count = 1;
	copied = tcp_read_sock(sk, &rd_desc, __qnap_iscsit_zmc_sw_recv);
	read_unlock_bh(&sk->sk_callback_lock);

	if (copied <= 0)
		return;

	spin_lock_bh(&dr->obj_list_lock);

	/* add current copied to previous total len */
	dr->total_len += copied;
	list_splice_tail_init(&dr->local_obj_list, &dr->obj_list);

	if (dr->query_len && (dr->total_len >= dr->query_len)) {
		ISCSI_ZMC_DEBUG("wake zmc-thread, total objs:0x%x, "
			"copied: 0x%x, query len:0x%x\n", atomic_read(&dr->obj_cnt),
			copied, dr->query_len);

		spin_unlock_bh(&dr->obj_list_lock);

		wake_up_interruptible(&dr->thread_wq);
		return;
	}
	spin_unlock_bh(&dr->obj_list_lock);
}

static void __qnap_iscsit_zmc_sk_state_change(struct sock *sk)
{
	struct iscsi_conn *conn;
	struct zmc_dr *dr;
	void (*state_change)(struct sock *sk);
	bool wake_rx = false;

	ISCSI_ZMC_DEBUG("Entering %s\n", __func__);

	read_lock_bh(&sk->sk_callback_lock);
	conn = sk->sk_user_data;
	if (conn) {
		dr = (struct zmc_dr *)conn->zmc_dr;
		state_change = conn->orig_state_change;

		if (__qnap_iscsit_zmc_sk_state_check(sk))
			wake_rx = true;
	} else
		state_change = sk->sk_state_change;

	read_unlock_bh(&sk->sk_callback_lock);

	if (wake_rx) {
		ISCSI_ZMC_WARN("wake rx while tcp close wait or tcp close\n");
		wake_up_interruptible(&dr->thread_wq);
	}

	state_change(sk);
}

static void __qnap_iscsit_zmc_sk_write_space(struct sock *sk)
{
	struct iscsi_conn *conn;
	void (*sk_write_space)(struct sock *sk);

	ISCSI_ZMC_DEBUG("Entering %s\n", __func__);	
	ISCSI_ZMC_DEBUG("sk->sk_sndbuf:0x%x, sk->sk_wmem_queued:0x%x, "
			"sk_stream_memory_free(sk):0x%x\n", sk->sk_sndbuf,
			sk->sk_wmem_queued, sk_stream_memory_free(sk));

//	dump_stack();

	read_lock_bh(&sk->sk_callback_lock);
	conn = sk->sk_user_data;
	if (conn) 
		sk_write_space = conn->orig_write_space;
	else
		sk_write_space = sk->sk_write_space;

	read_unlock_bh(&sk->sk_callback_lock);

	sk_write_space(sk);
#if 0
	if (sk_stream_is_writeable(sk)) {
		ZMC_INFO("clear sk nospc bit\n");
		clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
	}
#endif
}

static int qnap_iscsit_tcp_zmc_set_cb_check__(
	struct iscsi_conn *conn
	)
{
	/* we did setup sk callbacks() in qnap_iscsit_tcp_zmc_set_sock_callbacks() 
	 * called from iscsi_post_login_handler()
	 */
	struct sock *sk;
	bool sk_user_data_null = false;

	/* sock shall be exists ... */
	BUG_ON(!conn);
	BUG_ON(!conn->sock);

	sk = conn->sock->sk;

	/* after iscsi_target_restore_sock_callbacks(), sk_user_data will be null */
	read_lock_bh(&sk->sk_callback_lock);
	if (!sk->sk_user_data)
		sk_user_data_null = true;
	read_unlock_bh(&sk->sk_callback_lock);

	if (conn->zmc_dr && sk_user_data_null) {
		ISCSI_ZMC_INFO("zmc sock cb not set yet, start to set it\n");
		return 0;
	}

	return -ENOTSUPP;

}

/* for qnap_iscsit_tcp_zmc_set_sock_callbacks(), we do it in iscsi_post_login_handler() */
static int qnap_iscsit_tcp_zmc_set_sock_callbacks__(
	struct iscsi_conn *conn
	)
{
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct sock *sk;

	/* sock shall be exists ... */	
	BUG_ON(!conn);
	BUG_ON(!conn->sock);
	BUG_ON(!dr);

	ISCSI_ZMC_DEBUG("Entering %s: conn: %p\n", __func__, conn);

	sk = conn->sock->sk;

	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = conn;
	conn->orig_data_ready = sk->sk_data_ready;
	conn->orig_state_change = sk->sk_state_change;
	conn->orig_write_space = sk->sk_write_space;

	sk->sk_data_ready = __qnap_iscsit_zmc_sk_data_ready;
	sk->sk_state_change = __qnap_iscsit_zmc_sk_state_change;
	sk->sk_write_space = __qnap_iscsit_zmc_sk_write_space;
	atomic_set(&dr->set_cb, 1);
	write_unlock_bh(&sk->sk_callback_lock);

	ISCSI_ZMC_INFO("done to set zmc sock cb\n");
	return 0;

}

static int qnap_iscsit_tcp_zmc_restore_sock_callbacks__(
	struct iscsi_conn *conn
	)
{
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct sock *sk;

	BUG_ON(!conn);
	BUG_ON(!dr);

	ISCSI_ZMC_DEBUG("Entering %s: conn: %p\n", __func__, conn);

	if (conn->sock) {
		sk = conn->sock->sk;
		write_lock_bh(&sk->sk_callback_lock);
		if (sk->sk_user_data) {
			sk->sk_user_data = NULL;
			sk->sk_data_ready = conn->orig_data_ready;
			sk->sk_state_change = conn->orig_state_change;
			sk->sk_write_space = conn->orig_write_space;
			atomic_set(&dr->set_cb, 0);
			
			sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;
			sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
		}
		write_unlock_bh(&sk->sk_callback_lock);		
	}

	qnap_iscsit_tcp_zmc_free_priv(conn);

}

static void qnap_iscsit_zmc_each_obj_walk__(
	struct zmc_work_data *wdata,
	int buf_len
	)
{
	struct iscsi_conn *conn = wdata->conn;
	struct iscsi_cmd *cmd = wdata->cmd;
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct zmc_obj *obj = NULL, *tmp_obj = NULL;
	int size, rc;
	u32 work_off;

	/* handle each obj from local list, use xxx_entry_safe() version
	 * due to we may remove obj from local ist
	 */
	list_for_each_entry_safe(obj, tmp_obj, &wdata->local_obj_list, obj_node) {
		if (cmd) {
			if (!obj->cmd)
				obj->cmd = cmd;

			/* Check this obj was stored in iscsi cmd or not, 
			 * if not, to do it. In iscsit_release_cmd(), we
			 * will check the obj->touch again to make sure we 
			 * can free obj or not to avoid obj (multiple skb's data)
			 * still was used by other iscsi cmd
			 */
			if (!qnap_iscsit_zmc_search_obj2cmd__(cmd, obj)) {
				rc = qnap_iscsit_zmc_push_obj2cmd__(cmd, obj);
				WARN_ON(rc);
				atomic_inc(&obj->touch);
			}
		}

		if (atomic_read(&obj->touch) > 2) {
			if (obj->cmd && cmd) {
				ISCSI_ZMC_DEBUG("obj:0x%p, obj->cmd:0x%p, "
					"cmd:0x%p, skb:0x%p, touch:%d\n", obj, 
					obj->cmd, cmd, obj->skb, 
					atomic_read(&obj->touch));
			}
		}

		work_off = obj->offset;

		ISCSI_ZMC_DEBUG("before: obj: %p, len:0x%x, off:0x%x\n", 
			obj, obj->len, obj->offset);

		if (obj->len > buf_len) {
			size = buf_len;
			obj->offset += size;
		} else
			size = obj->len;

		wdata->total_len -= size;
		obj->len -= size;

		if (obj->len == 0) {
			wdata->obj_count--;
			list_del_init(&obj->obj_node);
			list_add_tail(&obj->obj_node, &wdata->free_list);
		}

		if (wdata->put_to_wlist) {
			obj->work_off = work_off;
			obj->work_len = size;
			list_add_tail(&obj->work_node, &wdata->work_list);

			ISCSI_ZMC_DEBUG("after: obj: %p, copy:0x%x, work len:0x%x, "
				"work off:0x%x\n", obj, size, obj->work_len, 
				obj->work_off);
		}

		buf_len -= size;
		if (!buf_len)
			break;
	}

	if (list_empty(&wdata->work_list))
		WARN_ON(true);

}

static int qnap_iscsit_zmc_skb_copy_to_iov__(
	struct zmc_work_data *wdata,
	struct kvec *iov,
	int iov_count
	)
{
	struct zmc_obj *zmc_obj = NULL, *tmp_obj = NULL;
	int i = 0, copy = 0, len, ret, total_len;
	void *base;

	if (list_empty(&wdata->work_list))
		WARN_ON(true);

	total_len = iov[i].iov_len;
	base = iov[i].iov_base;

	/* use xxx_entry_safe() version due to we will remove node */
	list_for_each_entry_safe(zmc_obj, tmp_obj, &wdata->work_list, work_node) {
		list_del_init(&zmc_obj->work_node);

		ISCSI_ZMC_DEBUG("base:%p, total_len:0x%x, zmc_obj->work_len:0x%x\n", 
			base, total_len, zmc_obj->work_len);

		while(zmc_obj->work_len) {	
			len = min_t(u32, total_len, zmc_obj->work_len);
			/* return 0 if it's good to copy */
			ret = skb_copy_bits(zmc_obj->skb, zmc_obj->work_off, 
					base, len);
			WARN_ON(ret < 0);
			copy += len;

			if (len >= total_len) {
				if (++i == iov_count) {
					zmc_obj->work_off += len;
					zmc_obj->work_len -= len;
					goto exit;
				}
				total_len = iov[i].iov_len;
				base = iov[i].iov_base;
			} else  {
				base += len;
				total_len -= len;
			}
			zmc_obj->work_off += len;
			zmc_obj->work_len -= len;
		}
	}

exit:
	if (!list_empty(&wdata->work_list))
		WARN_ON(true);

	return copy;
}

static int qnap_iscsit_zmc_skb_to_sgvec__(
	struct zmc_work_data *wdata
	)
{
	struct iscsi_cmd *cmd = wdata->cmd;
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct target_iscsi_zmc_data *p = 
		(struct target_iscsi_zmc_data *)&se_cmd->zmc_data.u.izmc;

	struct zmc_obj *obj = NULL, *tmp_obj = NULL;
	struct zmc_wsgl_node *wsgl_node = NULL;
	struct scatterlist *first_sg = NULL, *work_sg = NULL;
	int nsg = 0, copy = 0, updated = 0, curr_sg_nents;
	int wanted_nsg = 0;

	if (list_empty(&wdata->work_list))
		return -EINVAL;

	/* 1. at this time to load data per obj's skb from obj list, try find 
	 * how many total sg elements we need 
	 */
	list_for_each_entry(obj, &wdata->work_list, work_node)
		wanted_nsg += skb_nsg(obj->skb, obj->work_off, obj->work_len);

	if (wanted_nsg <= 0)
		return -EINVAL;

	wsgl_node = p->ops->alloc_wsgl_node(p, &se_cmd->se_dev->dev_dr, wanted_nsg);
	if (!wsgl_node) {
		p->ops->free_all_wsgl_node(p, &se_cmd->se_dev->dev_dr);
		return -ENOMEM;
	}

	curr_sg_nents = atomic_read(&p->wsgl_nents);

	/* push wsgl node to list */
	p->ops->push_wsgl_node(p, wsgl_node);
	work_sg = first_sg = wsgl_node->zmc_wsgl;

	ISCSI_ZMC_DEBUG("%s: curr_sg_nents: %d, work sg: %p\n", __func__, 
		curr_sg_nents, first_sg);

	/* 2. 
	 * use xxx_entry_safe() version due to we will remove node from 
	 * work_list (temp local storage, remember the node data still keeps
	 * in obj_node
	 */
	list_for_each_entry_safe(obj, tmp_obj, &wdata->work_list, work_node) {
		list_del_init(&obj->work_node);

		nsg = skb_to_sgvec_nomark(obj->skb, work_sg, 
					obj->work_off, obj->work_len);
		BUG_ON(nsg < 0);

		ISCSI_ZMC_DEBUG("%s, work_sg:%p, sg page:%p, sg len:0x%x, "
			"sg off:0x%x, nsg: %d\n", __func__, work_sg, 
			sg_page(work_sg), work_sg->length, work_sg->offset, nsg);

		updated += nsg;
		work_sg = &first_sg[updated];

		ISCSI_ZMC_DEBUG("%s, obj: %p, skb: %p, work off:0x%x, work len:0x%x, "
			"nsg:%d, next sg indx:%d\n", __func__, obj, 
			obj->skb, obj->work_off, obj->work_len, 
			nsg, (curr_sg_nents + updated));

		copy += obj->work_len;
	}


	/* we do NOT call sg_mark_end() due to we MAY NOT get ALL data completely here */
	WARN_ON((updated != wsgl_node->zmc_wsgl_count));

	curr_sg_nents += updated;
	atomic_set(&p->wsgl_nents, curr_sg_nents);

	ISCSI_ZMC_DEBUG("updated: %d, total_sg_nents:%d\n", updated, curr_sg_nents);

	if (!list_empty(&wdata->work_list))
		WARN_ON(true);

	return copy;
}

static void qnap_iscsit_zmc_free_obj2cmd(
	struct iscsi_cmd *cmd
	)
{
	struct iscsi_conn *conn = NULL;
	struct obj2cmd_node *curr = NULL, *next = NULL;
	struct zmc_dr *dr = NULL;
	struct zmc_obj *obj = NULL, *tmp_obj = NULL;

	if (!cmd->obj2cmd_node)
		return;

	curr = (struct obj2cmd_node *)cmd->obj2cmd_node;

	while(curr) {
		obj = curr->obj;
		/* decrease touch count since we want to free obj ... ,
		 * the obj may be in dr->free_obj_list or still in dr->obj_list
		 */
		if (obj) 
			atomic_dec(&obj->touch);

		next = curr->next;

		ISCSI_ZMC_DEBUG("free obj2cmd:0x%p\n", curr);

		qnap_iscsit_zmc_free_obj2cmd_cache(curr);
		curr = next;
	}


}

static void qnap_iscsit_zmc_free_objs(
	struct iscsi_cmd *cmd,
	struct list_head *local_free_list
	)
{
	struct iscsi_conn *conn = NULL;
	struct zmc_dr *dr = NULL;
	struct zmc_obj *obj = NULL, *tmp_obj = NULL;

	/**/
	conn = cmd->conn;
	dr = (struct zmc_dr *)conn->zmc_dr;

	list_for_each_entry_safe(obj, tmp_obj, local_free_list, obj_node) {
		/* someone may still use this obj, it means some skb data is
		 * for other iscsi cmd
		 */
		if (atomic_read(&obj->touch) == 1) {
			/* nobody uses this obj */
			list_del_init(&obj->obj_node);

			ISCSI_ZMC_DEBUG("free zmc obj:0x%p, skb:0x%p, touch:%d\n", 
				obj, obj->skb, atomic_read(&obj->touch));

			kfree_skb(obj->skb);
			kmem_cache_free(dr->obj_cache, obj);
		}
	}
}

static void qnap_iscsit_tcp_zmc_release_cmd__(
	struct iscsi_cmd *cmd
	)
{
	LIST_HEAD(free_list);
	struct zmc_dr *dr = (struct zmc_dr *)cmd->conn->zmc_dr;

	qnap_iscsit_zmc_free_obj2cmd(cmd);

	/* Only use spin_lock() / spin_unlock() due to we may come from 
	 * target_put_sess_cmd() -> target_release_cmd_kref()
	 * 
	 * Here is a chance to free unwanted obj for iscsi cmd. Please remember
	 * the obj (skb's data) may be used by single or multiple iscsi cmd
	 */
	spin_lock(&dr->free_obj_list_lock);
	list_splice_tail_init(&dr->free_obj_list, &free_list);
	spin_unlock(&dr->free_obj_list_lock);

	if (!list_empty(&free_list))
		qnap_iscsit_zmc_free_objs(cmd, &free_list);

	/* insert again due to obj (some skb data) may be used
	 * by other iscsi command, please refer qnap_iscsit_zmc_free_objs()
	 */
	if (!list_empty(&free_list)) {
		spin_lock(&dr->free_obj_list_lock);
		list_splice_tail_init(&free_list, &dr->free_obj_list);
		spin_unlock(&dr->free_obj_list_lock);
	}	

}

static inline void qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data__(
	struct zmc_dr *dr,
	struct zmc_work_data *wdata
	)
{
	/* Not to free obj now, we still may need them ... */
	spin_lock_bh(&dr->free_obj_list_lock);
	list_splice_tail_init(&wdata->free_list, &dr->free_obj_list);
	spin_unlock_bh(&dr->free_obj_list_lock);

	/* if any obj in local obj_list, need to insert them to 
	 * head of obj_list again
	 */
	if (!list_empty(&wdata->local_obj_list)) {
		spin_lock_bh(&dr->obj_list_lock);
		list_splice_init(&wdata->local_obj_list, &dr->obj_list);
		dr->total_len += wdata->total_len;
		atomic_add(wdata->obj_count, &dr->obj_cnt);
		spin_unlock_bh(&dr->obj_list_lock);
	}
}

static int qnap_iscsit_tcp_zmc_rx_data_step2__(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct zmc_work_data *wdata,
	struct kvec *iov,
	int iov_count,
	int data_len
	)
{
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	int copy = 0;

	wdata->cmd = cmd;
	wdata->conn = conn;
	wdata->put_to_wlist = true;

	/* before to consume data, to depend on data_len to pick up whcih obj
	 * we wnat and put to free list and work list
	 */
	qnap_iscsit_zmc_each_obj_walk__(wdata, data_len);

	if (iov)
		copy = qnap_iscsit_zmc_skb_copy_to_iov__(wdata, iov, iov_count);
	else
		copy = qnap_iscsit_zmc_skb_to_sgvec__(wdata);

	qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data__(dr, wdata);

	return copy;
}

static int qnap_iscsit_tcp_zmc_rx_data_step1__(
	struct iscsi_conn *conn,
	struct zmc_work_data *wdata,
	int data_len
	)
{
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct sock *sk = conn->sock->sk;
	DEFINE_WAIT(__wait);
	bool exit_wait;	
	int ret;

	do {
		/* check sk state and other sk status */
		lock_sock(sk);
		ret = __qnap_iscsit_zmc_sk_state_check(sk);
		if (!ret) {
			if (sk->sk_err) {
				ISCSI_ZMC_WARN("%s: got sk_errT\n", __func__);
				ret = -EINVAL;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN) {
				ISCSI_ZMC_WARN("%s: got RCV_SHUTDOWN\n", __func__);
				ret = -EINVAL;
			}

			if (signal_pending(current)) {
				ISCSI_ZMC_WARN("%s: got singal\n", __func__);
				ret = -EINVAL;
			}

		}
		release_sock(sk);
		if (ret)
			return ret;

		/* check data is ready or not */
		spin_lock_bh(&dr->obj_list_lock);
		dr->query_len = data_len;

		ISCSI_ZMC_DEBUG("%s: [1] total_len: 0x%x, data len:0x%x\n", 
			__func__, dr->total_len, data_len);

		if ((dr->total_len >= data_len) && (!list_empty(&dr->obj_list))) {
			/* move all objs to temp local list first */
			list_splice_tail_init(&dr->obj_list, &wdata->local_obj_list);
			wdata->obj_count = atomic_read(&dr->obj_cnt);
			wdata->total_len = dr->total_len;
			atomic_set(&dr->obj_cnt, 0);
			dr->total_len = 0;
			dr->query_len = 0;
			exit_wait = true;
		} else {
			init_wait(&__wait);
			prepare_to_wait(&dr->thread_wq, &__wait, TASK_INTERRUPTIBLE);
			spin_unlock_bh(&dr->obj_list_lock);

			schedule_timeout(MAX_SCHEDULE_TIMEOUT);
			exit_wait = false;

			spin_lock_bh(&dr->obj_list_lock);
			finish_wait(&dr->thread_wq, &__wait);
		} 

		ISCSI_ZMC_DEBUG("%s: [2] total_len: 0x%x, data_len: 0x%x\n", 
			__func__, dr->total_len, data_len);

		spin_unlock_bh(&dr->obj_list_lock);

		if (exit_wait)
			break;

	} while (true);

	return 0;
}

static int qnap_iscsit_tcp_zmc_rx_data__(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct kvec *iov,
	int iov_count,
	int data_len
	)
{
	int copy = 0, ret;
	struct zmc_work_data wdata;

	INIT_LIST_HEAD(&wdata.local_obj_list);
	INIT_LIST_HEAD(&wdata.free_list);
	INIT_LIST_HEAD(&wdata.work_list);

	ret = qnap_iscsit_tcp_zmc_rx_data_step1__(conn, &wdata, data_len);
	if (ret)
		return ret;

	copy = qnap_iscsit_tcp_zmc_rx_data_step2__(conn, cmd, &wdata, iov, 
			iov_count, data_len);

	return copy;
}

static int qnap_iscsit_tcp_zmc_dump_data_payload__(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,	
	u32 buf_len,
	int dump_padding_digest
	)
{
	/* refer from iscsit_dump_data_payload() */
	struct sock *sk = conn->sock->sk;
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	int padding, ret;
	struct zmc_work_data wdata;

	if (conn->sess->sess_ops->RDMAExtensions)
		return 0;

#warning "todo, need oto check"
//	ret = DATAOUT_CANNOT_RECOVER;

	if (dump_padding_digest) {
		padding = ((-buf_len) & 3);
		if (padding != 0)
			buf_len += padding;
		if (conn->conn_ops->DataDigest)
			buf_len += ISCSI_CRC_LEN;
	}

	INIT_LIST_HEAD(&wdata.local_obj_list);
	INIT_LIST_HEAD(&wdata.free_list);

	ret = qnap_iscsit_tcp_zmc_rx_data_step1__(conn, &wdata, buf_len);
	if (ret)
		return ret;

	wdata.cmd = cmd;
	wdata.conn = conn;
	wdata.put_to_wlist = false;

	/* before to consume data, to depend on data_len to pick up whcih obj
	 * we wnat and put to free list and work list
	 */
	qnap_iscsit_zmc_each_obj_walk__(&wdata, buf_len);

	qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data__(dr, &wdata);
	return 0;
}

static int qnap_iscsit_tcp_zmc_get_dataout__(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd,
	struct iscsi_data *hdr
	)
{
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct target_iscsi_zmc_data *p =
		(struct target_iscsi_zmc_data *)&se_cmd->zmc_data.u.izmc;

	struct zmc_wsgl_node *prev_wsgl_node;
	struct scatterlist *data_sg = NULL;
	struct kvec iov;
	u8 *buf = NULL;
	u8 buffer[4 + ISCSI_CRC_LEN] = {0}; /* 4 bytes for padding space */
	u32 checksum, padding = 0, rx_got = 0;
	u32 payload_len = ntoh24(hdr->dlength);
	int data_crc_failed = 0, pad_plus_crc_len = 0, idx, prev_wsgl_nents, ret;

	ISCSI_ZMC_DEBUG("hdr off:0x%x\n", be32_to_cpu(hdr->offset));

	/* step 1:
	 * we will put all skb's data to work_sg list, the data may contain
	 * (data seg + padding bytes + data digest)
	 */
	padding = ((-payload_len) & 3);

	if (padding != 0)
		pad_plus_crc_len += padding;
	if (conn->conn_ops->DataDigest)
		pad_plus_crc_len += ISCSI_CRC_LEN;

	/* record current wsgl counts and get last node first */
	prev_wsgl_nents = atomic_read(&p->wsgl_nents);
	prev_wsgl_node = (struct zmc_wsgl_node *)p->wsgl_node;

	/* NOT need to set ZMC_W_SG_INUSE_VALID due to it is dataout command */

	rx_got = qnap_iscsit_tcp_zmc_rx_data__(conn, cmd, NULL, 0, payload_len);
	if (rx_got != payload_len) {
		pr_err("rx_got (%d) != expected len (%d)\n", rx_got, payload_len);
		return -EINVAL;
	}

	if (pad_plus_crc_len == 0)
		return data_crc_failed;

	/* step 2: to get padding data, data digest if necessary */
	
	memset(&buffer, 0, pad_plus_crc_len);
	memset(&iov, 0, sizeof(struct kvec));
	iov.iov_base = buffer;
	iov.iov_len  = pad_plus_crc_len;

	rx_got = qnap_iscsit_tcp_zmc_rx_data__(conn, cmd, &iov, 1, iov.iov_len);
	if (rx_got != pad_plus_crc_len) {
		pr_err("rx_got (%d) != pad_plus_crc_len (%d)\n", rx_got, 
				pad_plus_crc_len);
		return -EINVAL;
	}
	
	if (padding) {
		memset(cmd->pad_bytes, 0, 4);
		for (idx = 0; idx < padding; idx++)
			cmd->pad_bytes[idx] = buffer[idx];
	}

	/* step 3:
	 * to check data digest is correct or not 
	 */
	if (conn->conn_ops->DataDigest) {
		u32 data_crc;

		checksum = *(u32 *)&buffer[padding];
		ISCSI_ZMC_DEBUG("data digest:0x%x\n", checksum);

		data_sg = p->ops->prep_wsgl(p, prev_wsgl_node, 
					atomic_read(&p->wsgl_nents));
		if (!data_sg)
			return -ENOMEM;

		cmd->first_data_sg = &data_sg[prev_wsgl_nents];
		/* set to 0, please refer iscsit_do_crypto_hash_sg() */		
		cmd->first_data_sg_off = 0;

		data_crc = qnap_iscsit_tcp_zmc_do_crypto_hash_sg(conn->conn_rx_hash, 
				cmd, be32_to_cpu(hdr->offset),
				payload_len, padding, cmd->pad_bytes);

		kfree(data_sg);
		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;
		
		if (checksum != data_crc) {
			pr_err("ITT: 0x%08x, Offset: %u, Length: %u,"
				" DataSN: 0x%08x, CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n",
				hdr->itt, hdr->offset, payload_len,
				hdr->datasn, checksum, data_crc);
			data_crc_failed = 1;
		} else {
			ISCSI_ZMC_DEBUG("Got CRC32C DataDigest 0x%08x for"
				" %u bytes of Data Out\n", checksum,
				payload_len);
		}
	}

	return data_crc_failed;
}

static int qnap_iscsit_tcp_zmc_handle_immediate_data__(
	struct iscsi_cmd *cmd,
	struct iscsi_scsi_req *hdr,
	u32 length
	)
{
	struct iscsi_conn *conn = cmd->conn;	
	struct zmc_dr *dr = (struct zmc_dr *)conn->zmc_dr;
	struct se_cmd *se_cmd = &cmd->se_cmd;	
	struct target_iscsi_zmc_data *p =
		(struct target_iscsi_zmc_data *)&se_cmd->zmc_data.u.izmc;

	struct scatterlist *data_sg = NULL;
	struct kvec iov;
	int rx_got = 0, idx, ret;
	u32 checksum, padding = 0, pad_plus_crc_len = 0;	
	u8 *buf = NULL;
	u8 buffer[4 + ISCSI_CRC_LEN] = {0}; /* 4 bytes for padding space */

	/* step 1:
	 * we will put all skb's data to work_sg list, the data may contain
	 * (data seg + padding bytes + data digest)
	 */
	padding = ((-length) & 3);

	if (padding != 0)
		pad_plus_crc_len += padding;
	if (conn->conn_ops->DataDigest)
		pad_plus_crc_len += ISCSI_CRC_LEN;

	/* to indicate we will use zmc wsgl mechanism */
	atomic_set(&p->write_sg_inuse, ZMC_W_SG_INUSE_VALID);

	rx_got = qnap_iscsit_tcp_zmc_rx_data__(conn, cmd, NULL, 0, length);
	if (rx_got != length) {
		pr_err("rx_got (%d) != expected len (%d)\n", rx_got, length);
		qnap_iscsit_tcp_zmc_rx_thread_wait_for_tcp(conn);
		return IMMEDIATE_DATA_CANNOT_RECOVER;
	}

	if (pad_plus_crc_len == 0)
		goto no_padding;

	/* step 2: to get padding data, data digest if necessary */
	memset(&buffer, 0, pad_plus_crc_len);
	memset(&iov, 0, sizeof(struct kvec));
	iov.iov_base = buffer;
	iov.iov_len  = pad_plus_crc_len;

	rx_got = qnap_iscsit_tcp_zmc_rx_data__(conn, cmd, &iov, 1, iov.iov_len);
	if (rx_got != pad_plus_crc_len) {
		pr_err("rx_got (%d) != pad_plus_crc_len (%d)\n", rx_got, 
				pad_plus_crc_len);
		qnap_iscsit_tcp_zmc_rx_thread_wait_for_tcp(conn);
		return IMMEDIATE_DATA_CANNOT_RECOVER;
	}
	
	if (padding) {
		memset(cmd->pad_bytes, 0, 4);
		for (idx = 0; idx < padding; idx++)
			cmd->pad_bytes[idx] = buffer[idx];
	}

	/* step 3:
	 * to check data digest is correct or not 
	 */
	if (conn->conn_ops->DataDigest) {
		u32 data_crc; 

		checksum = *(u32 *)&buffer[padding];
		ISCSI_ZMC_DEBUG("data digest:0x%x\n", checksum);

		/* start from 1st node due to it is immediate data */
		data_sg = p->ops->prep_wsgl(p, p->wsgl_node, 
					atomic_read(&p->wsgl_nents));
		if (!data_sg)
			return IMMEDIATE_DATA_CANNOT_RECOVER;

		/* now is for immediate data, so to use first sg */
		cmd->first_data_sg = &data_sg[0];
		/* set to 0, please refer iscsit_do_crypto_hash_sg() */
		cmd->first_data_sg_off = 0;

		data_crc = qnap_iscsit_tcp_zmc_do_crypto_hash_sg(
				conn->conn_rx_hash, cmd, cmd->write_data_done, 
				length, padding, cmd->pad_bytes);

		kfree(data_sg);
		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;

		if (checksum != data_crc) {
			pr_err("ImmediateData CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n", checksum,
				data_crc);
	
			if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
				pr_err("Unable to recover from"
					" Immediate Data digest failure while"
					" in ERL=0.\n");
				iscsit_reject_cmd(cmd,
						ISCSI_REASON_DATA_DIGEST_ERROR,
						(unsigned char *)hdr);
				return IMMEDIATE_DATA_CANNOT_RECOVER;
			} else {
				iscsit_reject_cmd(cmd,
						ISCSI_REASON_DATA_DIGEST_ERROR,
						(unsigned char *)hdr);
				return IMMEDIATE_DATA_ERL1_CRC_FAILURE;
			}
		} else {
			ISCSI_ZMC_DEBUG("Got CRC32C DataDigest 0x%08x for"
				" %u bytes of Immediate Data\n", checksum,
				length);
		}

	}

no_padding:

	cmd->write_data_done += length;
	
	if (cmd->write_data_done == cmd->se_cmd.data_length) {
		spin_lock_bh(&cmd->istate_lock);
		cmd->cmd_flags |= ICF_GOT_LAST_DATAOUT;
		cmd->i_state = ISTATE_RECEIVED_LAST_DATAOUT;
		spin_unlock_bh(&cmd->istate_lock);
	}

	return IMMEDIATE_DATA_NORMAL_OPERATION;
}

static int qnap_iscsit_tcp_zmc_get_rx_pdu__(
	struct iscsi_conn *conn
	)
{
	int ret = -EINVAL;

	u8 buffer[ISCSI_HDR_LEN + ISCSI_CRC_LEN], opcode;
	u32 checksum = 0, digest = 0;
	struct kvec iov;

	while (!kthread_should_stop()) {

		/* we do NOT ensure that both TX and RX per connection kthreads
 		 * are scheduled to run on the same CPU here. It is different
 		 * from LIO-TARGET code
		 */

		/* Hummmmm ... take care iscsi pdu header has AHS 
		 * (Additional Header Segment), it is optional . But, it seems 
		 * that LIO-TARGET doesn't support it, we skip it now
		 */
		memset(buffer, 0, (ISCSI_HDR_LEN + ISCSI_CRC_LEN));
		memset(&iov, 0, sizeof(struct kvec));

		iov.iov_base	= buffer;
		iov.iov_len	= ISCSI_HDR_LEN;

		if (conn->conn_ops->HeaderDigest)
			iov.iov_len += ISCSI_CRC_LEN;			

		ret = qnap_iscsit_tcp_zmc_rx_data__(conn, NULL, &iov, 1, iov.iov_len);
#if 0
		__dump_mem(buffer, iov.iov_len);
#endif
		if (ret != iov.iov_len) {
			ISCSI_ZMC_WARN("ret: (%d) != expected len (%d)\n", ret, 
					iov.iov_len);
			qnap_iscsit_tcp_zmc_rx_thread_wait_for_tcp(conn);
			return ret;
		}

		if (conn->conn_ops->HeaderDigest) {
			digest = *(u32 *)&buffer[ISCSI_HDR_LEN];
			ISCSI_ZMC_DEBUG("hdr digest:0x%x\n", digest);
#if 0
			__dump_mem((u8 *)&digest, ISCSI_CRC_LEN);
#endif
			qnap_iscsit_tcp_zmc_do_crypto_hash_buf(conn->conn_rx_hash, 
				buffer, ISCSI_HDR_LEN, 0, NULL, (u8 *)&checksum);

			if (digest != checksum) {
				pr_err("HeaderDigest CRC32C failed,"
					" received 0x%08x, computed 0x%08x\n",
					digest, checksum);
				/*
				 * Set the PDU to 0xff so it will intentionally
				 * hit default in the switch below.
				 */
				memset(buffer, 0xff, ISCSI_HDR_LEN);
				atomic_long_inc(&conn->sess->conn_digest_errors);
			} else {
				ISCSI_ZMC_DEBUG("Got HeaderDigest CRC32C"
						" 0x%08x\n", checksum);
			}
		}

		if (conn->conn_state == TARG_CONN_STATE_IN_LOGOUT)
			return ret;

		opcode = buffer[0] & ISCSI_OPCODE_MASK;

		if (conn->sess->sess_ops->SessionType &&
		   ((!(opcode & ISCSI_OP_TEXT)) ||
		    (!(opcode & ISCSI_OP_LOGOUT)))) {
			pr_err("Received illegal iSCSI Opcode: 0x%02x"
			" while in Discovery Session, rejecting.\n", opcode);
			qnap_iscsit_tcp_zmc_add_reject(conn, 
				ISCSI_REASON_PROTOCOL_ERROR, buffer);
			return ret;
		}

		ret = qnap_iscsit_tcp_zmc_target_rx_opcode(conn, buffer);
		if (ret < 0)
			return ret;
	}

	/* if kthread stop ... */
	return -EIO;
}

static int qnap_iscsit_tcp_zmc_xmit_datain_pdu__(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd,
	const struct iscsi_datain *datain
	)
{
	/* refer from iscsit_xmit_datain_pdu() */

	u32 tx_size = 0;
	int total_head_len = 0, total_tail_len = 0;
	int ret = -EINVAL, ent, page_off, i;
	struct page *pdu_head_page = NULL, *pdu_tail_page = NULL;
	struct scatterlist pdu_head_sg, pdu_tail_sg;
	struct scatterlist *data_sg = NULL;
	u8 *buf = NULL;

	struct {
		struct scatterlist *sg;
		int first_sg_off;
		int total_len;
		char *str;
	} sg_send_rec[3] = {
		{ .sg = NULL, .first_sg_off = 0, .total_len = 0, .str = "send hdr" },
		{ .sg = NULL, .first_sg_off = 0, .total_len = 0, .str = "send data body" },
		{ .sg = NULL, .first_sg_off = 0, .total_len = 0, .str = "send tail" },
	};

	pdu_head_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	pdu_tail_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!pdu_head_page || !pdu_tail_page) 
		goto exit;

	/* step 1: pdu header + header digest */
	total_head_len += ISCSI_HDR_LEN;

	if (conn->conn_ops->HeaderDigest) {
		u32 *header_digest = (u32 *)&cmd->pdu[ISCSI_HDR_LEN];

		qnap_iscsit_tcp_zmc_do_crypto_hash_buf(conn->conn_tx_hash, 
			cmd->pdu, ISCSI_HDR_LEN, 0, NULL, 
			(u8 *)header_digest);

		total_head_len += ISCSI_CRC_LEN;
		ISCSI_ZMC_DEBUG("Attaching CRC32 HeaderDigest for DataIN PDU "
				"0x%08x\n",
			 *header_digest);
	}

	buf = (u8 *)kmap(pdu_head_page);
	if (!buf)
		goto exit;

	memcpy(buf, cmd->pdu, total_head_len);
	kunmap(pdu_head_page);
	sg_init_table(&pdu_head_sg, 1);
	sg_set_page(&pdu_head_sg, pdu_head_page, total_head_len, 0);

	sg_send_rec[0].sg = &pdu_head_sg;
	sg_send_rec[0].first_sg_off = 0;
	sg_send_rec[0].total_len = total_head_len;


	/* step 2:padding bytes + data digest */
	cmd->padding = ((-datain->length) & 3);
	if (cmd->padding)
		total_tail_len += cmd->padding;

	/* find sg location we want */
	ent = datain->offset / PAGE_SIZE;
	page_off = (datain->offset % PAGE_SIZE);
	data_sg = &cmd->se_cmd.t_data_sg[ent];

	sg_send_rec[1].sg = data_sg;
	sg_send_rec[1].first_sg_off = page_off;
	sg_send_rec[1].total_len = datain->length;

	ISCSI_ZMC_DEBUG("%s: dlen:0x%x, datain->offset:0x%x, nr sg:%d, ent:%d, "
		"page_off:0x%x\n", __func__, cmd->se_cmd.data_length, datain->offset, 
		cmd->se_cmd.t_data_nents, ent, page_off);

	if (conn->conn_ops->DataDigest) {
		cmd->first_data_sg = data_sg;
		cmd->first_data_sg_off = data_sg->offset + page_off;
				
		cmd->data_crc = qnap_iscsit_tcp_zmc_do_crypto_hash_sg(
				conn->conn_tx_hash, cmd, datain->offset, 
				datain->length, cmd->padding, cmd->pad_bytes);

		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;
		
		total_tail_len += ISCSI_CRC_LEN;	
		ISCSI_ZMC_DEBUG("Attached CRC32C DataDigest %d bytes, crc 0x%08x\n",
			 datain->length + cmd->padding, cmd->data_crc);
		
	}

	if (total_tail_len) {
		buf = NULL;
		buf = (u8 *)kmap(pdu_tail_page);
		if (!buf)
			goto exit;

		if (cmd->padding) {
			memcpy(buf, cmd->pad_bytes, cmd->padding);
			buf += (unsigned long)cmd->padding;
		}

		if (conn->conn_ops->DataDigest)
			*(u32 *)buf = cmd->data_crc;

		kunmap(pdu_tail_page);
		sg_init_table(&pdu_tail_sg, 1);
		sg_set_page(&pdu_tail_sg, pdu_tail_page, total_tail_len, 0);

		sg_send_rec[2].sg = &pdu_tail_sg;
		sg_send_rec[2].first_sg_off = 0;
		sg_send_rec[2].total_len = total_tail_len;
	}

	/* step 3: start to send all data */
	lock_sock(conn->sock->sk);

	for (i = 0; i < 3; i++) {
		if (i == 2 && !sg_send_rec[i].sg)
			break;
repeat_send:
		ret = do_tcp_sg_sendpages_locked(conn->sock->sk, 
			sg_send_rec[i].sg, sg_send_rec[i].first_sg_off, 
			sg_send_rec[i].total_len, 0);

		if (ret != sg_send_rec[i].total_len) {
			ISCSI_ZMC_WARN("(%s) do_tcp_sg_sendpages_locked() failure, "
				"ret: %d\n", sg_send_rec[i].str, ret);
			if (ret == -EAGAIN)
				goto repeat_send;
			goto out_release_sock;
		}
	}

	ret = 0;

out_release_sock:
	release_sock(conn->sock->sk);

exit:
	if (ret)
		qnap_iscsit_tcp_zmc_tx_thread_wait_for_tcp(conn);

	if (pdu_head_page)
		__free_page(pdu_head_page);
	if (pdu_tail_page)
		__free_page(pdu_tail_page);

	return ret;

}


int qnap_iscsit_tcp_zmc_alloc_priv(
	struct iscsi_conn *conn
	)
{
	struct zmc_dr *priv = NULL;
	char name[64];

	conn->zmc_dr = NULL;

	priv = kzalloc(sizeof(struct zmc_dr), GFP_KERNEL);
	if (!priv)
		goto out;

	snprintf(name, 64, "iscsi_tcp_zmc_cache_%d", conn->cid);

	priv->obj_cache = kmem_cache_create(name, sizeof(struct zmc_obj), 
		__alignof__(struct zmc_obj), 0, NULL);

	if (!priv->obj_cache) {
		kfree(priv);
out:
		ISCSI_ZMC_WARN("Unable to alloc iscsi tcp zmc priv data\n");
		return -ENOMEM;
	}

	/* setup default value */
	atomic_set(&priv->set_cb, 0);
	atomic_set(&priv->obj_cnt, 0);

	spin_lock_init(&priv->obj_list_lock);
	spin_lock_init(&priv->free_obj_list_lock);
	INIT_LIST_HEAD(&priv->obj_list);
	INIT_LIST_HEAD(&priv->free_obj_list);
	INIT_LIST_HEAD(&priv->local_obj_list);

	init_waitqueue_head(&priv->thread_wq);
	priv->query_len = 0;
	priv->total_len = 0;

	priv->rx_act.copy_to_iov = false;
	priv->rx_act.work_sg = NULL;
	priv->rx_act.work_sg_nents = NULL;

	conn->zmc_dr = (void *)priv;
	return 0;
}

void qnap_iscsit_tcp_zmc_free_priv(
	struct iscsi_conn *conn
	)
{
	struct zmc_obj *obj = NULL, *tmp_obj = NULL;
	struct zmc_dr *p = NULL;
	LIST_HEAD(__free_list);

	if (!conn->zmc_dr)
		return;

	p = (struct zmc_dr *)conn->zmc_dr;
	conn->zmc_dr = NULL;

	spin_lock_bh(&p->free_obj_list_lock);
	list_splice_tail_init(&p->free_obj_list, &__free_list);
	spin_unlock_bh(&p->free_obj_list_lock);

	list_for_each_entry_safe(obj, tmp_obj, &__free_list, obj_node) {
		list_del_init(&obj->obj_node);

		if (atomic_read(&obj->touch) != 1) {
			ISCSI_ZMC_WARN("obj touched not 1, obj:0x%p, touched:%d\n",
				obj, atomic_read(&obj->touch));
			WARN_ON(true);
		}
		ISCSI_ZMC_DEBUG("%s: free zmc obj:%p, skb:%p\n", __func__, 
			obj, obj->skb);
		kfree_skb(obj->skb);
		kmem_cache_free(p->obj_cache, obj);
	}

	kmem_cache_destroy(p->obj_cache);
	kfree(p);
}

int qnap_iscsit_tcp_zmc_check_ops(void)
{
	if (!iscsi_tcp_zmc_enable) {
		ISCSI_ZMC_WARN("iscsi_tcp_zmc_enable was disabled, skip setup "
			"conn_ops and cmd_ops\n");
		return -ENOTSUPP;
	}

	if (!__conn_ops.set_cb_check
	|| !__conn_ops.set_sock_callbacks
	|| !__conn_ops.restore_sock_callbacks
	|| !__conn_ops.get_rx_pdu
	) 
	{
		ISCSI_ZMC_WARN("__conn_ops was't completed, skip setup conn_ops\n");
		return -ENOTSUPP;
	}

	if (!__cmd_ops.dump_data_payload
	|| !__cmd_ops.get_dataout
	|| !__cmd_ops.handle_immediate_data
	|| !__cmd_ops.xmit_datain_pdu	
	) 
	{
		ISCSI_ZMC_WARN("__cmd_ops was't completed, skip setup conn_ops\n");
		return -ENOTSUPP;
	}

	return 0;

}

void qnap_iscsit_tcp_zmc_setup_conn_ops_on_conn(
	struct iscsi_conn *conn
	)
{
	conn->zmc_conn_ops = &__conn_ops;
	ISCSI_ZMC_INFO("setup conn_ops (0x%p) on conn (0x%p)\n", 
			conn->zmc_conn_ops, conn);
}

void qnap_iscsit_tcp_zmc_setup_cmd_ops_on_iscsi_cmd(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	if (conn->zmc_conn_ops) {
#if 0
		ISCSI_ZMC_INFO("setup cmd_ops on iscsi cmd (0x%p)\n", cmd);
#endif
		cmd->zmc_cmd_ops = &__cmd_ops;

		/* even set to true, still depend on real case what command will
		 * be handled by zmc feature 
		 */
		cmd->se_cmd.zmc_data.valid = true;
	} else 
		cmd->se_cmd.zmc_data.valid = false;
}

void qnap_iscsit_tcp_zmc_release_cmd(
	struct iscsi_cmd *cmd
	)
{
	qnap_iscsit_tcp_zmc_release_cmd__(cmd);
}

int qnap_iscsit_zmc_create_obj2cmd_cache(void)
{
	if (zmc_obj2cmd_cache) {
		/* avoid multi conns case */		
		ISCSI_ZMC_INFO("zmc_obj2cmd_cache was created already, "
				"skip to create it\n");
		return 0;
	}

	zmc_obj2cmd_cache = kmem_cache_create("zmc_obj2cmd_cache", 
			sizeof(struct obj2cmd_node),
			__alignof__(struct obj2cmd_node), 0, NULL);

	if (!zmc_obj2cmd_cache) {
		ISCSI_ZMC_WARN("fail to create zmc_obj2cmd_cache\n");
		return -ENOMEM;
	}

	ISCSI_ZMC_INFO("done create zmc_obj2cmd_cache\n");
	return 0;
}

void qnap_iscsit_zmc_destory_obj2cmd_cache(void)
{
	if (!zmc_obj2cmd_cache)
		return;

	kmem_cache_destroy(zmc_obj2cmd_cache);
	ISCSI_ZMC_INFO("done to destory zmc_obj2cmd_cache\n");	
}

/* iscsi tcp zmc conn ops */
int qnap_iscsit_tcp_zmc_rx_data(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, 
	struct kvec *iov, 
	int iov_count, 
	int data_len
	)
{
	return qnap_iscsit_tcp_zmc_rx_data__(conn, cmd, iov, iov_count, data_len);
}

int qnap_iscsit_tcp_zmc_xmit_datain_pdu(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, 
	const struct iscsi_datain *datain
	)
{
	return cmd->zmc_cmd_ops->xmit_datain_pdu(conn, cmd, datain);
}

#endif /* end of QNAP_ISCSI_TCP_ZMC_FEATURE */
#endif



