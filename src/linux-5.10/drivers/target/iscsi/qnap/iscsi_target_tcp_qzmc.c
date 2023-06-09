/*******************************************************************************
 * @file        iscsi_target_tcp_qzmc.c
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
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <asm/unaligned.h>
#include <uapi/asm-generic/ioctls.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>
#include <target/iscsi/iscsi_target_core.h>
#include <target/iscsi/iscsi_transport.h>

#include "../iscsi_target.h"
#include "../iscsi_target_util.h"

#include "iscsi_target_qzmc.h"
#include "iscsi_target_tcp_qzmc.h"
#include "../../qnap/target_core_iscsi_qzmc.h"

/**/
unsigned int iscsi_tcp_zmc_enable = 0;
module_param_named(iscsi_tcp_zmc_en, iscsi_tcp_zmc_enable, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(iscsi_tcp_zmc_en, "iscsi tcp zero-memory-copy feature (0:disabled, 1:enabled)");
EXPORT_SYMBOL(iscsi_tcp_zmc_enable);

/**/

/* called from net/ipv4/tcp.c file */
extern ssize_t do_tcp_sendpages(struct sock *sk, struct page *page, int offset,
			 size_t size, int flags);

extern void qnap_iscsit_zmc_tx_thread_wait_for_tcp(struct iscsi_conn *conn);
extern void qnap_iscsit_zmc_rx_thread_wait_for_tcp(struct iscsi_conn *conn);

extern void qnap_iscsit_zmc_do_crypto_hash_buf(struct ahash_request *hash,
	const void *buf, u32 payload_length, u32 padding, u8 *pad_bytes, 
	u8 *data_crc);

u32 qnap_iscsit_zmc_do_crypto_hash_sg(struct ahash_request * hash,  
	struct iscsi_cmd * cmd,  u32 data_offset, u32 data_length, 
	u32 padding, u8 *pad_bytes);

extern int qnap_iscsit_zmc_target_rx_opcode(struct iscsi_conn *conn, 
	unsigned char * buf);

extern int qnap_iscsit_zmc_add_reject(struct iscsi_conn *conn, u8 reason,
	unsigned char *buf);

/**/
DEFINE_IDA(zmc2skb_obj_ida);
DEFINE_IDA(obj2cmd_ida);

/**/
static int __qnap_iscsit_tcp_zmc_get_rx_data(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, struct kvec *iov, int iov_count, int data_len);

static void __qnap_iscsit_tcp_zmc_free_sgl_node(struct se_cmd *cmd);

static struct scatterlist *__qnap_iscsit_tcp_zmc_convert_sgl_node(
	struct se_cmd *cmd, struct tcp_zmc_sgl_node *sgl_node, int sgl_nents);


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

static ssize_t __qnap_iscsit_tcp_zmc_push_sg(
	struct sock *sk,
	struct scatterlist *sg,
	int first_sg_offset,
	int total,
	char *type_str
	)
{
	int flags = MSG_SENDPAGE_NOTLAST, offset = first_sg_offset, ret;
	struct page *p;
	size_t size, remain, done = 0;

	size = sg->length - offset;
	offset += sg->offset;

	while (total) {
		remain = total - size;
		if (!remain)
			flags &= ~MSG_SENDPAGE_NOTLAST;

		ISCSI_TCP_ZMC_DEBUG("type:%s, total_size:%d, off:%d, size:%d, "
				"remain:%d, flag:%d\n", type_str, total, offset, 
				size, remain, flags);

		tcp_rate_check_app_limited(sk);
		p = sg_page(sg);
repeat_send:		
		ret = do_tcp_sendpages(sk, p, offset, size, flags);
		if (ret != size) {
			if (ret > 0) {
				ISCSI_TCP_ZMC_WARN("(%s) do_tcp_sendpages() "
					"return value is not same as expected "
					"send-size, ret: %d, send-size:%d\n", 
					type_str, ret, size);

				done += ret;				
				offset += ret;
				size -= ret;
				goto repeat_send;
			}
			return ret;
		}

		done += ret;
		total = remain;
		if (total) {
			sg = sg_next(sg);
			offset = sg->offset;
			size = sg->length;
		}
	}

	return done;
}

static void __qnap_iscsit_tcp_zmc_free_single_sgl_node(
	struct se_cmd *cmd,
	struct tcp_zmc_sgl_node *p
	)
{
	struct se_device *dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &dev->dev_dr;

	if (dr->zmc_sgl_node_cache && p)
		kmem_cache_free(dr->zmc_sgl_node_cache, p);
}

static void __qnap_iscsit_tcp_zmc_push_sgl_node(
	struct se_cmd *cmd,
	struct tcp_zmc_sgl_node *node
	)
{
	struct tcp_zmc_cmd_priv *p = 
		(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;
	
	struct tcp_zmc_sgl_node *curr = NULL;

	if (!p->sgl_node) {
		p->sgl_node = node;
		ISCSI_TCP_ZMC_DEBUG("push first new sgl_node:0x%p in se_cmd:0x%p\n", 
					node, cmd);
	} else {
		curr = p->sgl_node;
		while (curr->next)
			curr = curr->next;
		curr->next = node;
		ISCSI_TCP_ZMC_DEBUG("push new sgl_node:0x%p after curr "
			"sgl_node:0x%p\n", node, curr);
	}

}

static struct tcp_zmc_sgl_node *__qnap_iscsit_tcp_zmc_alloc_sgl_node(
	struct se_cmd *cmd,
	int wanted_sg_count
	)
{
	struct se_device *dev = cmd->se_dev;
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	struct tcp_zmc_sgl_node *p = NULL;
	int wsgl_size = (wanted_sg_count * sizeof(struct scatterlist));
	
	if (!dr->zmc_sgl_node_cache)
		return NULL;

	p = kmem_cache_zalloc(dr->zmc_sgl_node_cache, GFP_KERNEL);
	if (!p)
		return NULL;

	p->sgl = kzalloc(wsgl_size, GFP_KERNEL);
	if (!p->sgl) {
		__qnap_iscsit_tcp_zmc_free_single_sgl_node(cmd, p);
		return NULL;
	}

	/* NOT set end of table NOW */
	memset(p->sgl, 0, wsgl_size);
	p->sgl_count = wanted_sg_count;
	p->next = NULL;

	ISCSI_TCP_ZMC_DEBUG("alloc sgl node:0x%p, sgl:0x%p, wanted sg count:%d\n", 
		p, p->sgl, wanted_sg_count);

	return p;

}

static void *__qnap_iscsit_tcp_zmc_core_kmap_data_sg(
	struct scatterlist *sgl,
	int sgl_nents,
	int len
	)
{
	/* refer from transport_kmap_data_sg() and se_cmd->zmc_wsgl MUST/NEED
	 * to be prepread already when to use this call
	 */
	struct scatterlist *sg;
	int i, copied;
	u8 *src = NULL, *dest = NULL, *buf = NULL;

	if (!sgl || !sgl_nents)
		return NULL;

	if (sgl_nents == 1)
		return kmap(sg_page(sgl)) + sgl->offset;

	/* >1 page. can NOT use vmap due to our pages in sgl may not
	 * be full size (PAGE_SIZE) we need to do copy one by one ...
	 */
	dest = buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return NULL;

	for_each_sg(sgl, sg, sgl_nents, i) {
		src = kmap(sg_page(sg)) + sg->offset;
		if (!src) 
			goto fail;
		/* the len may be smaller than your sg length ... */
		copied = min_t(int, len, sg->length);
		memcpy(dest, src, copied);
		kunmap(sg_page(sg));
		dest += copied;
		len -= copied;
		if (!len)
			break;
	}
	return buf;
fail:
	kfree(buf);
	return NULL;

}

static void __qnap_iscsit_tcp_zmc_core_kunmap_data_sg(
	struct scatterlist *sgl,
	int sgl_nents,
	void *zmc_data_vmap
	)
{
	if (!sgl || !sgl_nents)
		return;

	if (sgl_nents == 1) {
		kunmap(sg_page(sgl));
		return;
	}

	kfree(zmc_data_vmap);
}

/**/
static int __qnap_iscsit_tcp_zmc_insert_obj2cmd(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct tcp_zmc2skb_obj *obj
	)
{
	struct iscsit_tcp_zmc_conn_priv *conn_priv = 
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;

	struct iscsit_tcp_zmc_cmd_priv *cmd_priv =
			(struct iscsit_tcp_zmc_cmd_priv *)cmd->zmc_cmd_priv;

	struct obj2cmd_node *new = NULL, *curr = NULL;

	if (!conn_priv->obj2cmd_cache)
		return -ENOMEM;

	new = kmem_cache_zalloc(conn_priv->obj2cmd_cache, 
				((in_interrupt()) ? GFP_ATOMIC: GFP_KERNEL));
	if (!new)
		return -ENOMEM;

	new->next = NULL;
	new->obj = obj;

	if (!cmd_priv->node) {
		cmd_priv->node = new;
		ISCSI_TCP_ZMC_DEBUG("push first new obj:0x%p in cmd:0x%p\n", 
				new, cmd);
	} else {
		curr = cmd_priv->node;
		while (curr->next)
			curr = curr->next;
		curr->next = new;
		ISCSI_TCP_ZMC_DEBUG("push new obj:0x%p after curr obj:0x%p\n", 
				new, curr);
	}

	return 0;
}

static bool __qnap_iscsit_tcp_zmc_search_obj2cmd(
	struct iscsi_cmd *cmd,
	struct tcp_zmc2skb_obj *obj
	)
{
	struct iscsit_tcp_zmc_cmd_priv *priv =
			(struct iscsit_tcp_zmc_cmd_priv *)cmd->zmc_cmd_priv;

	struct obj2cmd_node *curr = NULL, *head = NULL;
	bool found = false;

	head = priv->node;
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
		ISCSI_TCP_ZMC_INFO("%s find obj:0x%p in cmd:0x%p\n", 
				((found) ? "" : "not"), obj, cmd);
	}
#endif

	ISCSI_TCP_ZMC_DEBUG("%s find obj:0x%p in cmd:0x%p\n", 
			((found) ? "" : "not"), obj, cmd);

	return found;
}

static int __qnap_iscsit_tcp_zmc_sw_recv(
	read_descriptor_t *rd_desc, 
	struct sk_buff *skb,
	unsigned int offset, 
	size_t len
	)
{
	struct iscsi_conn *conn = rd_desc->arg.data;
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
			(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	struct tcp_zmc2skb_obj *obj = NULL;

	ISCSI_TCP_ZMC_DEBUG("%s: conn: %p, skb_len:%d, off:%d, len:%d, "
			"skb_cb->seq:0x%x\n", __func__, conn, skb->len, 
			offset, len, TCP_SKB_CB(skb)->seq);

	/* The skb will be removed from sk_receive_queue list via sk_eat_skb()
	 * if process it successfully. If fail, it is still in list.
	 * For objs we collected before, just keep them due to we may have
	 * chances to handle under failure case
	 */
	obj = kmem_cache_zalloc(zmc_data->zmc2skb_cache, GFP_ATOMIC);
	if (!obj)
		return -ENOMEM;

	obj->skb = skb_clone(skb, GFP_ATOMIC);
	if (!obj->skb) {
		kmem_cache_free(zmc_data->zmc2skb_cache, obj);
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

	list_add_tail(&obj->obj_node, &zmc_data->local_obj_list);
	atomic_inc(&zmc_data->obj_cnt);

	ISCSI_TCP_ZMC_DEBUG("%s: conn: %p, read %d bytes, len:%d\n", __func__, 
			conn, skb->len - offset, len);

	return len;
}

static inline int __qnap_iscsit_tcp_zmc_sk_state_check(struct sock *sk)
{
	if (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE) {
		ISCSI_TCP_ZMC_WARN("%s: got TCP_CLOSE or TCP_CLOSE_WAIT\n", __func__);
		return -ECONNRESET;
	}
	return 0;
}

static void __qnap_iscsit_tcp_zmc_sk_data_ready(struct sock *sk)
{
	struct iscsi_conn *conn = NULL;
	struct iscsit_tcp_zmc_conn_priv *zmc_data = NULL;
	read_descriptor_t rd_desc;
	bool rc;
	int copied;

	ISCSI_TCP_ZMC_DEBUG("entering %s: conn: %p\n", __func__, conn);

	read_lock_bh(&sk->sk_callback_lock);
	if (!sk->sk_user_data) {
		read_unlock_bh(&sk->sk_callback_lock);
		return;
	}

	conn = (struct iscsi_conn *)sk->sk_user_data;
	zmc_data = (struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	/*
	 * Use rd_desc to pass 'conn' to __qnap_iscsit_tcp_zmc_sw_recv.
	 * We set count to 1 because we want the network layer to
	 * hand us all the skbs that are available.
	 */
	rd_desc.arg.data = conn;
	rd_desc.count = 1;
	copied = tcp_read_sock(sk, &rd_desc, __qnap_iscsit_tcp_zmc_sw_recv);
	read_unlock_bh(&sk->sk_callback_lock);

	if (copied <= 0)
		return;

	spin_lock_bh(&zmc_data->obj_list_lock);

	/* add current copied to previous total len */
	zmc_data->total_len += copied;
	list_splice_tail_init(&zmc_data->local_obj_list, &zmc_data->obj_list);

	if (zmc_data->query_len && (zmc_data->total_len >= zmc_data->query_len)) {
		ISCSI_TCP_ZMC_DEBUG("wake zmc-thread, total objs:0x%x, "
				"copied: 0x%x, query len:0x%x\n", 
				atomic_read(&zmc_data->obj_cnt),
				copied, zmc_data->query_len);

		spin_unlock_bh(&zmc_data->obj_list_lock);

		wake_up_interruptible(&zmc_data->thread_wq);
		return;
	}
	spin_unlock_bh(&zmc_data->obj_list_lock);

}

static void __qnap_iscsit_tcp_zmc_sk_state_change(struct sock *sk)
{
	struct iscsi_conn *conn;
	struct iscsit_tcp_zmc_conn_priv *zmc_data = NULL;
	void (*state_change)(struct sock *sk);
	bool wake_rx = false;

	ISCSI_TCP_ZMC_DEBUG("entering %s\n", __func__);

	read_lock_bh(&sk->sk_callback_lock);
	conn = sk->sk_user_data;
	if (conn) {
		zmc_data = (struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
		state_change = conn->orig_state_change;

		if (__qnap_iscsit_tcp_zmc_sk_state_check(sk))
			wake_rx = true;
	} else
		state_change = sk->sk_state_change;

	read_unlock_bh(&sk->sk_callback_lock);

	if (wake_rx) {
		ISCSI_TCP_ZMC_WARN("wake rx while tcp close wait or tcp close\n");
		wake_up_interruptible(&zmc_data->thread_wq);
	}

	state_change(sk);

}

static void __qnap_iscsit_tcp_zmc_sk_write_space(struct sock *sk)
{
	struct iscsi_conn *conn;
	void (*sk_write_space)(struct sock *sk);

	ISCSI_TCP_ZMC_DEBUG("entering %s, sk->sk_sndbuf:0x%x, "
		"sk->sk_wmem_queued:0x%x, sk_stream_memory_free(sk):0x%x\n", 
		__func__, sk->sk_sndbuf, sk->sk_wmem_queued, 
		sk_stream_memory_free(sk));

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

static int __qnap_iscsit_tcp_zmc_set_callback_check(
	struct iscsi_conn *conn
	)
{
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

	if (conn->zmc_conn_priv && sk_user_data_null) {
		ISCSI_TCP_ZMC_INFO("zmc sock callback not set yet, start to set it\n");
		return 0;
	}

	return -ENOTSUPP;
}

static int __qnap_iscsit_tcp_zmc_set_sock_callback(
	struct iscsi_conn *conn
	)
{
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
			(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	struct sock *sk;

	/* sock shall be exists ... */	
	BUG_ON(!conn);
	BUG_ON(!conn->sock);
	BUG_ON(!zmc_data);

	ISCSI_TCP_ZMC_DEBUG("Entering %s: conn: %p\n", __func__, conn);

	sk = conn->sock->sk;

	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = conn;
	conn->orig_data_ready = sk->sk_data_ready;
	conn->orig_state_change = sk->sk_state_change;
	conn->orig_write_space = sk->sk_write_space;

	sk->sk_data_ready = __qnap_iscsit_tcp_zmc_sk_data_ready;
	sk->sk_state_change = __qnap_iscsit_tcp_zmc_sk_state_change;
	sk->sk_write_space = __qnap_iscsit_tcp_zmc_sk_write_space;
	atomic_set(&zmc_data->set_cb, 1);
	write_unlock_bh(&sk->sk_callback_lock);

	ISCSI_TCP_ZMC_INFO("done to set zmc sock cb\n");
	return 0;

}

static int __qnap_iscsit_tcp_zmc_restore_sock_callback(
	struct iscsi_conn *conn
	)
{
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
			(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	struct sock *sk;

	BUG_ON(!conn);
	BUG_ON(!zmc_data);

	ISCSI_TCP_ZMC_DEBUG("Entering %s: conn: %p\n", __func__, conn);

	if (conn->sock) {
		sk = conn->sock->sk;
		write_lock_bh(&sk->sk_callback_lock);
		if (sk->sk_user_data) {
			sk->sk_user_data = NULL;
			sk->sk_data_ready = conn->orig_data_ready;
			sk->sk_state_change = conn->orig_state_change;
			sk->sk_write_space = conn->orig_write_space;
			atomic_set(&zmc_data->set_cb, 0);
			
			sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;
			sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
		}
		write_unlock_bh(&sk->sk_callback_lock);		
	}

	return 0;
}

static int __qnap_iscsit_tcp_zmc_get_rx_pdu(
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

		ret = __qnap_iscsit_tcp_zmc_get_rx_data(conn, NULL, &iov, 1, 
				iov.iov_len);
#if 0
		__dump_mem(buffer, iov.iov_len);
#endif
		if (ret != iov.iov_len) {
			ISCSI_TCP_ZMC_WARN("ret: (%d) != expected len (%d)\n", 
					ret, iov.iov_len);
			qnap_iscsit_zmc_rx_thread_wait_for_tcp(conn);
			return ret;
		}

		if (conn->conn_ops->HeaderDigest) {
			digest = *(u32 *)&buffer[ISCSI_HDR_LEN];
			ISCSI_TCP_ZMC_DEBUG("hdr digest:0x%x\n", digest);
#if 0
			__dump_mem((u8 *)&digest, ISCSI_CRC_LEN);
#endif
			qnap_iscsit_zmc_do_crypto_hash_buf(conn->conn_rx_hash, 
				buffer, ISCSI_HDR_LEN, 0, NULL, (u8 *)&checksum);

			if (digest != checksum) {
				ISCSI_TCP_ZMC_ERR("HeaderDigest CRC32C failed,"
					" received 0x%08x, computed 0x%08x\n",
					digest, checksum);
				/*
				 * Set the PDU to 0xff so it will intentionally
				 * hit default in the switch below.
				 */
				memset(buffer, 0xff, ISCSI_HDR_LEN);
				atomic_long_inc(&conn->sess->conn_digest_errors);
			} else {
				ISCSI_TCP_ZMC_DEBUG("Got HeaderDigest CRC32C"
						" 0x%08x\n", checksum);
			}
		}

		if (conn->conn_state == TARG_CONN_STATE_IN_LOGOUT)
			return ret;

		opcode = buffer[0] & ISCSI_OPCODE_MASK;

		if (conn->sess->sess_ops->SessionType &&
		   ((!(opcode & ISCSI_OP_TEXT)) ||
		    (!(opcode & ISCSI_OP_LOGOUT)))) {
			ISCSI_TCP_ZMC_ERR("Received illegal iSCSI Opcode: 0x%02x"
			" while in Discovery Session, rejecting.\n", opcode);
			qnap_iscsit_zmc_add_reject(conn, 
				ISCSI_REASON_PROTOCOL_ERROR, buffer);
			return ret;
		}

		ret = qnap_iscsit_zmc_target_rx_opcode(conn, buffer);
		if (ret < 0)
			return ret;
	}

	/* if kthread stop ... */
	return -EIO;
}

static void __qnap_iscsit_tcp_zmc_each_obj_walk(
	struct tcp_zmc_work_data *wdata,
	int buf_len
	)
{
	struct iscsi_conn *conn = wdata->conn;
	struct iscsi_cmd *cmd = wdata->cmd;
	struct tcp_zmc2skb_obj *obj = NULL, *tmp_obj = NULL;
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
			if (!__qnap_iscsit_tcp_zmc_search_obj2cmd(cmd, obj)) {
				rc = __qnap_iscsit_tcp_zmc_insert_obj2cmd(
						conn, cmd, obj);
				WARN_ON(rc);
				atomic_inc(&obj->touch);
			}
		}

		if (atomic_read(&obj->touch) > 2) {
			if (obj->cmd && cmd) {
				ISCSI_TCP_ZMC_DEBUG("obj:0x%p, obj->cmd:0x%p, "
					"cmd:0x%p, skb:0x%p, touch:%d\n", obj, 
					obj->cmd, cmd, obj->skb, 
					atomic_read(&obj->touch));
			}
		}

		work_off = obj->offset;

		ISCSI_TCP_ZMC_DEBUG("before: obj: %p, len:0x%x, off:0x%x\n", 
			obj, obj->len, obj->offset);

		if (obj->len > buf_len) {
			size = buf_len;
			obj->offset += size;
		} else
			size = obj->len;

		wdata->total_len -= size;
		obj->len -= size;

		if (obj->len == 0) {
			/* may free it since it was consumed already */
			wdata->obj_count--;
			list_del_init(&obj->obj_node);
			list_add_tail(&obj->obj_node, &wdata->free_list);
		}

		if (wdata->put_to_wlist) {
			obj->work_off = work_off;
			obj->work_len = size;
			list_add_tail(&obj->work_node, &wdata->work_list);

			ISCSI_TCP_ZMC_DEBUG("after: obj: %p, copy:0x%x, "
				"work len:0x%x, work off:0x%x\n", obj, size, 
				obj->work_len, obj->work_off);
		}

		buf_len -= size;
		if (!buf_len)
			break;
	}

	if (wdata->put_to_wlist) {
		/* check this for non-dump case */
		if (list_empty(&wdata->work_list))
			WARN_ON(true);
	}

}

static int __qnap_iscsit_tcp_zmc_skb_copy_to_iov(
	struct tcp_zmc_work_data *wdata,
	struct kvec *iov,
	int iov_count
	)
{
	int i = 0, copy = 0, len, ret, total_len;
	struct tcp_zmc2skb_obj *zmc_obj = NULL, *tmp_obj = NULL;
	void *base;

	if (list_empty(&wdata->work_list))
		WARN_ON(true);

	total_len = iov[i].iov_len;
	base = iov[i].iov_base;

	/* use xxx_entry_safe() version due to we will remove node */
	list_for_each_entry_safe(zmc_obj, tmp_obj, &wdata->work_list, work_node) {
		list_del_init(&zmc_obj->work_node);

		ISCSI_TCP_ZMC_DEBUG("base:%p, total_len:0x%x, zmc_obj->work_len:0x%x\n", 
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

static int __qnap_iscsit_tcp_zmc_skb_to_sgvec(
	struct tcp_zmc_work_data *wdata
	)
{
	int nsg = 0, copy = 0, updated = 0, curr_sg_nents;
	struct iscsi_cmd *cmd = wdata->cmd;
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct tcp_zmc_cmd_priv *p = 
		(struct tcp_zmc_cmd_priv *)se_cmd->cmd_dr.zmc_data;

	struct tcp_zmc2skb_obj *obj = NULL, *tmp_obj = NULL;
	struct tcp_zmc_sgl_node *sgl_node = NULL;
	struct scatterlist *first_sg = NULL, *work_sg = NULL;
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

	sgl_node = __qnap_iscsit_tcp_zmc_alloc_sgl_node(se_cmd, wanted_nsg);
	if (!sgl_node) {
		/* free everything since it is fail to alloc sgl node at this time */
		__qnap_iscsit_tcp_zmc_free_sgl_node(se_cmd);
		return -ENOMEM;
	}

	curr_sg_nents = p->sgl_nents;

	/* push sgl node to list */
	__qnap_iscsit_tcp_zmc_push_sgl_node(se_cmd, sgl_node);

	work_sg = first_sg = sgl_node->sgl;

	ISCSI_TCP_ZMC_DEBUG("%s: curr_sg_nents: %d, work sg: %p\n", __func__, 
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

		ISCSI_TCP_ZMC_DEBUG("%s, work_sg:%p, sg page:%p, sg len:0x%x, "
			"sg off:0x%x, nsg: %d\n", __func__, work_sg, 
			sg_page(work_sg), work_sg->length, work_sg->offset, nsg);

		updated += nsg;
		work_sg = &first_sg[updated];

		ISCSI_TCP_ZMC_DEBUG("%s, obj: %p, skb: %p, work off:0x%x, "
			"work len:0x%x, nsg:%d, next sg indx:%d\n", __func__, 
			obj, obj->skb, obj->work_off, obj->work_len, 
			nsg, (curr_sg_nents + updated));

		copy += obj->work_len;
	}

	/* we DO NOT call sg_mark_end() due to we MAY NOT get ALL data completely here */
	WARN_ON((updated != sgl_node->sgl_count));

	curr_sg_nents += updated;
	p->sgl_nents = curr_sg_nents;

	ISCSI_TCP_ZMC_DEBUG("updated: %d, total_sg_nents:%d\n", updated, 
					curr_sg_nents);

	if (!list_empty(&wdata->work_list))
		WARN_ON(true);

	return copy;
}

static void __qnap_iscsit_tcp_zmc_free_obj2cmd_obj(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	struct iscsit_tcp_zmc_conn_priv *conn_priv =
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;

	struct iscsit_tcp_zmc_cmd_priv *cmd_priv = 
		(struct iscsit_tcp_zmc_cmd_priv *)cmd->zmc_cmd_priv;

	struct tcp_zmc2skb_obj *obj = NULL, *tmp_obj = NULL;
	struct obj2cmd_node *curr = NULL, *next = NULL;

	if (!cmd_priv->node)
		return;

	curr = cmd_priv->node;

	while(curr) {
		obj = curr->obj;
		/* decrease touch count since we want to free obj, the obj may be 
		 * still in free_obj_list or in obj_list of the conn priv area
		 */
		if (obj) 
			atomic_dec(&obj->touch);

		next = curr->next;
		ISCSI_TCP_ZMC_DEBUG("free obj2cmd:0x%p\n", curr);

		kmem_cache_free(conn_priv->obj2cmd_cache, curr);
		curr = next;
	}

}

static void __qnap_iscsit_tcp_zmc_free_zmc2skb_obj(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct list_head *local_free_list
	)
{
	struct iscsit_tcp_zmc_conn_priv *conn_priv =
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;

	struct tcp_zmc2skb_obj *obj = NULL, *tmp_obj = NULL;

	/**/
	list_for_each_entry_safe(obj, tmp_obj, local_free_list, obj_node) {
		/* someone may still use this obj, it means some skb data is
		 * for other iscsi cmd
		 */
		if (atomic_read(&obj->touch) == 1) {
			/* nobody uses this obj */
			list_del_init(&obj->obj_node);

			ISCSI_TCP_ZMC_DEBUG("free zmc2skb obj:0x%p, skb:0x%p\n", 
					obj, obj->skb);
			kfree_skb(obj->skb);
			kmem_cache_free(conn_priv->zmc2skb_cache, obj);
		}
	}

}

static void __qnap_iscsit_tcp_zmc_release_se_cmd_priv(
	struct se_cmd *se_cmd
	)
{
	struct tcp_zmc_cmd_priv *priv = 
		(struct tcp_zmc_cmd_priv *)se_cmd->cmd_dr.zmc_data;

	if (priv) {
		se_cmd->cmd_dr.zmc_data = NULL;
		priv->sgl_in_used = false;			

		if (priv->sgl)
			kfree(priv->sgl);

		/* priv->sgl_node was free in prep_final_sgl() in  __target_execute_cmd() */
		kfree(priv);
	}
}


static void __qnap_iscsit_tcp_zmc_release_cmd_priv(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	LIST_HEAD(free_list);
	struct iscsit_tcp_zmc_conn_priv *conn_priv =
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;

	struct iscsit_tcp_zmc_cmd_priv *cmd_priv = 
		(struct iscsit_tcp_zmc_cmd_priv *)cmd->zmc_cmd_priv;
	
	__qnap_iscsit_tcp_zmc_free_obj2cmd_obj(conn, cmd);

	/* Only use spin_lock() / spin_unlock() due to we may come from 
	 * target_put_sess_cmd() -> target_release_cmd_kref()
	 * 
	 * Here is a chance to free unwanted obj for iscsi cmd. Please remember
	 * the obj (skb's data) may be used by single or multiple iscsi cmd
	 */
	spin_lock(&conn_priv->free_obj_list_lock);
	list_splice_tail_init(&conn_priv->free_obj_list, &free_list);
	spin_unlock(&conn_priv->free_obj_list_lock);

	if (!list_empty(&free_list))
		__qnap_iscsit_tcp_zmc_free_zmc2skb_obj(conn, cmd, &free_list);

	/* insert again due to obj (some skb data) may be used
	 * by other iscsi command, please refer qnap_iscsit_zmc_free_objs()
	 */
	if (!list_empty(&free_list)) {
		spin_lock(&conn_priv->free_obj_list_lock);
		list_splice_tail_init(&free_list, &conn_priv->free_obj_list);
		spin_unlock(&conn_priv->free_obj_list_lock);
	}	

	kfree(cmd_priv);

}

static inline void __qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data(
	struct iscsi_conn *conn,
	struct tcp_zmc_work_data *wdata
	)
{
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;

	/* Not to free obj now, we still may need them ... */
	spin_lock_bh(&zmc_data->free_obj_list_lock);
	list_splice_tail_init(&wdata->free_list, &zmc_data->free_obj_list);
	spin_unlock_bh(&zmc_data->free_obj_list_lock);

	/* if any obj in local obj_list, need to insert them to 
	 * head of obj_list again
	 */
	if (!list_empty(&wdata->local_obj_list)) {
		spin_lock_bh(&zmc_data->obj_list_lock);
		list_splice_init(&wdata->local_obj_list, &zmc_data->obj_list);
		zmc_data->total_len += wdata->total_len;
		atomic_add(wdata->obj_count, &zmc_data->obj_cnt);
		spin_unlock_bh(&zmc_data->obj_list_lock);
	}
}

static int __qnap_iscsit_tcp_zmc_rx_data_step2(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct tcp_zmc_work_data *wdata,
	struct kvec *iov,
	int iov_count,
	int data_len
	)
{
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
		(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	int copy = 0;

	wdata->cmd = cmd;
	wdata->conn = conn;
	wdata->put_to_wlist = true;

	/* before to consume data, to depend on data_len to pick up whcih obj
	 * we wnat and put to free list and work list
	 */
	__qnap_iscsit_tcp_zmc_each_obj_walk(wdata, data_len);

	if (iov)
		copy = __qnap_iscsit_tcp_zmc_skb_copy_to_iov(wdata, iov, iov_count);
	else
		copy = __qnap_iscsit_tcp_zmc_skb_to_sgvec(wdata);

	__qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data(conn, wdata);

	return copy;
}

static int __qnap_iscsit_tcp_zmc_rx_data_step1(
	struct iscsi_conn *conn,
	struct tcp_zmc_work_data *wdata,
	int data_len
	)
{
	struct iscsit_tcp_zmc_conn_priv *zmc_data = 
			(struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
	struct sock *sk = conn->sock->sk;
	DEFINE_WAIT(__wait);
	bool exit_wait;	
	int ret;

	do {
		/* check sk state and other sk status */
		lock_sock(sk);
		ret = __qnap_iscsit_tcp_zmc_sk_state_check(sk);
		if (!ret) {
			if (sk->sk_err) {
				ISCSI_TCP_ZMC_WARN("%s: got sk_err\n", __func__);
				ret = -EINVAL;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN) {
				ISCSI_TCP_ZMC_WARN("%s: got RCV_SHUTDOWN\n", __func__);
				ret = -EINVAL;
			}

			if (signal_pending(current)) {
				ISCSI_TCP_ZMC_WARN("%s: got singal\n", __func__);
				ret = -EINVAL;
			}

		}
		release_sock(sk);
		if (ret)
			return ret;

		/* check data is ready or not */
		spin_lock_bh(&zmc_data->obj_list_lock);
		zmc_data->query_len = data_len;

		ISCSI_TCP_ZMC_DEBUG("%s: [1] total_len: 0x%x, data len:0x%x\n", 
			__func__, zmc_data->total_len, data_len);

		if ((zmc_data->total_len >= data_len) 
			&& (!list_empty(&zmc_data->obj_list))
		)
		{
			/* move all objs to temp local list first */
			list_splice_tail_init(&zmc_data->obj_list, 
							&wdata->local_obj_list);
			wdata->obj_count = atomic_read(&zmc_data->obj_cnt);
			wdata->total_len = zmc_data->total_len;
			atomic_set(&zmc_data->obj_cnt, 0);
			zmc_data->total_len = 0;
			zmc_data->query_len = 0;
			exit_wait = true;
		} else {
			init_wait(&__wait);
			prepare_to_wait(&zmc_data->thread_wq, &__wait, 
							TASK_INTERRUPTIBLE);
			spin_unlock_bh(&zmc_data->obj_list_lock);

			schedule_timeout(MAX_SCHEDULE_TIMEOUT);
			exit_wait = false;

			spin_lock_bh(&zmc_data->obj_list_lock);
			finish_wait(&zmc_data->thread_wq, &__wait);
		} 

		ISCSI_TCP_ZMC_DEBUG("%s: [2] total_len: 0x%x, data_len: 0x%x\n", 
			__func__, zmc_data->total_len, data_len);

		spin_unlock_bh(&zmc_data->obj_list_lock);

		if (exit_wait)
			break;

	} while (true);

	return 0;
}

static int __qnap_iscsit_tcp_zmc_get_rx_data(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct kvec *iov,
	int iov_count,
	int data_len
	)
{
	int copy = 0, ret;
	struct tcp_zmc_work_data wdata;

	INIT_LIST_HEAD(&wdata.local_obj_list);
	INIT_LIST_HEAD(&wdata.free_list);
	INIT_LIST_HEAD(&wdata.work_list);

	ret = __qnap_iscsit_tcp_zmc_rx_data_step1(conn, &wdata, data_len);
	if (ret)
		return ret;

	copy = __qnap_iscsit_tcp_zmc_rx_data_step2(conn, cmd, &wdata, iov, 
			iov_count, data_len);

	return copy;
}

static int __qnap_iscsit_tcp_zmc_dump_data_payload(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	u32 buf_len,
	bool dump_padding_digest
	)
{
	/* refer from iscsit_dump_data_payload() */
	struct tcp_zmc_work_data wdata;
	int padding, ret;

	if (conn->sess->sess_ops->RDMAExtensions)
		return 0;

	if (dump_padding_digest) {
		buf_len = ALIGN(buf_len, 4);
		if (conn->conn_ops->DataDigest)
			buf_len += ISCSI_CRC_LEN;
	}

	INIT_LIST_HEAD(&wdata.local_obj_list);
	INIT_LIST_HEAD(&wdata.free_list);

	ret = __qnap_iscsit_tcp_zmc_rx_data_step1(conn, &wdata, buf_len);
	if (ret)
		return ret;

	wdata.cmd = cmd;
	wdata.conn = conn;
	wdata.put_to_wlist = false;

	/* before to consume data, to depend on data_len to pick up whcih obj
	 * we wnat and put to free list and work list
	 */
	__qnap_iscsit_tcp_zmc_each_obj_walk(&wdata, buf_len);

	__qnap_iscsit_tcp_zmc_handle_wdata_after_rx_data(conn, &wdata);
	return 0;

}

static int __qnap_iscsit_tcp_zmc_get_dataout(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd,
	struct iscsi_data *hdr
	)
{
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct tcp_zmc_cmd_priv *cmd_priv =
		(struct tcp_zmc_cmd_priv *)se_cmd->cmd_dr.zmc_data;

	struct tcp_zmc_sgl_node *prev_sgl_node;
	struct scatterlist *data_sg = NULL;
	struct kvec iov;
	u8 *buf = NULL;
	u8 buffer[4 + ISCSI_CRC_LEN] = {0}; /* 4 bytes for padding space */
	u32 checksum, padding = 0, rx_got = 0;
	u32 payload_len = ntoh24(hdr->dlength);
	int data_crc_failed = 0, pad_plus_crc_len = 0, idx, prev_sgl_nents, ret;

	ISCSI_TCP_ZMC_DEBUG("hdr off:0x%x\n", be32_to_cpu(hdr->offset));

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
	prev_sgl_nents = cmd_priv->sgl_nents;
	prev_sgl_node = (struct tcp_zmc_sgl_node *)cmd_priv->sgl_node;

	/* NOT need to set cmd_priv->sgl_in_used due to it is dataout command */
	WARN_ON(!cmd_priv->sgl_in_used);

	rx_got = __qnap_iscsit_tcp_zmc_get_rx_data(conn, cmd, NULL, 0, payload_len);
	if (rx_got != payload_len) {
		ISCSI_TCP_ZMC_ERR("rx_got (%d) != payload_len len (%d)\n", rx_got, 
				payload_len);
		return -EINVAL;
	}

	if (pad_plus_crc_len == 0)
		return data_crc_failed;

	/* step 2: to get padding data, data digest if necessary */
	
	memset(&buffer, 0, pad_plus_crc_len);
	memset(&iov, 0, sizeof(struct kvec));
	iov.iov_base = buffer;
	iov.iov_len  = pad_plus_crc_len;

	rx_got = __qnap_iscsit_tcp_zmc_get_rx_data(conn, cmd, &iov, 1, iov.iov_len);
	if (rx_got != pad_plus_crc_len) {
		ISCSI_TCP_ZMC_ERR("rx_got (%d) != pad_plus_crc_len (%d)\n", rx_got, 
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
		ISCSI_TCP_ZMC_DEBUG("data digest:0x%x\n", checksum);

		data_sg = __qnap_iscsit_tcp_zmc_convert_sgl_node(se_cmd, 
				prev_sgl_node, prev_sgl_nents);
		if (!data_sg)
			return -ENOMEM;

		cmd->first_data_sg = &data_sg[prev_sgl_nents];
		/* set to 0, please refer qnap_iscsit_zmc_do_crypto_hash_sg() */		
		cmd->first_data_sg_off = 0;

		data_crc = qnap_iscsit_zmc_do_crypto_hash_sg(conn->conn_rx_hash, 
				cmd, be32_to_cpu(hdr->offset),
				payload_len, padding, cmd->pad_bytes);

		kfree(data_sg);
		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;
		
		if (checksum != data_crc) {
			ISCSI_TCP_ZMC_ERR("ITT: 0x%08x, Offset: %u, Length: %u,"
				" DataSN: 0x%08x, CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n",
				hdr->itt, hdr->offset, payload_len,
				hdr->datasn, checksum, data_crc);
			data_crc_failed = 1;
		} else {
			ISCSI_TCP_ZMC_DEBUG("Got CRC32C DataDigest 0x%08x for"
				" %u bytes of Data Out\n", checksum,
				payload_len);
		}
	}

	return data_crc_failed;

}

static int __qnap_iscsit_tcp_zmc_handle_immediate_data(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd,
	struct iscsi_scsi_req *hdr,
	u32 length
	)
{
	struct se_cmd *se_cmd = &cmd->se_cmd;	
	struct tcp_zmc_cmd_priv *cmd_priv =
		(struct tcp_zmc_cmd_priv *)se_cmd->cmd_dr.zmc_data;

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

	cmd_priv->sgl_in_used = true;

	rx_got = __qnap_iscsit_tcp_zmc_get_rx_data(conn, cmd, NULL, 0, length);
	if (rx_got != length) {
		ISCSI_TCP_ZMC_ERR("rx_got (%d) != expected len (%d)\n", 
					rx_got, length);
		qnap_iscsit_zmc_rx_thread_wait_for_tcp(conn);
		return IMMEDIATE_DATA_CANNOT_RECOVER;
	}

	if (pad_plus_crc_len == 0)
		goto no_padding;

	/* step 2: to get padding data, data digest if necessary */
	memset(&buffer, 0, pad_plus_crc_len);
	memset(&iov, 0, sizeof(struct kvec));
	iov.iov_base = buffer;
	iov.iov_len  = pad_plus_crc_len;

	rx_got = __qnap_iscsit_tcp_zmc_get_rx_data(conn, cmd, &iov, 1, iov.iov_len);
	if (rx_got != pad_plus_crc_len) {
		ISCSI_TCP_ZMC_ERR("rx_got (%d) != pad_plus_crc_len (%d)\n", 
				rx_got, pad_plus_crc_len);
		qnap_iscsit_zmc_rx_thread_wait_for_tcp(conn);
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
		ISCSI_TCP_ZMC_DEBUG("data digest:0x%x\n", checksum);

		/* start from 1st node due to it is immediate data */
		data_sg = __qnap_iscsit_tcp_zmc_convert_sgl_node(se_cmd, 
					cmd_priv->sgl_node, cmd_priv->sgl_nents);
		if (!data_sg)
			return IMMEDIATE_DATA_CANNOT_RECOVER;

		/* now is for immediate data, so to use first sg */
		cmd->first_data_sg = &data_sg[0];
		/* set to 0, please refer qnap_iscsit_zmc_do_crypto_hash_sg() */
		cmd->first_data_sg_off = 0;

		data_crc = qnap_iscsit_zmc_do_crypto_hash_sg(
				conn->conn_rx_hash, cmd, cmd->write_data_done, 
				length, padding, cmd->pad_bytes);

		kfree(data_sg);
		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;

		if (checksum != data_crc) {
			ISCSI_TCP_ZMC_ERR("ImmediateData CRC32C DataDigest 0x%08x"
				" does not match computed 0x%08x\n", checksum,
				data_crc);
	
			if (!conn->sess->sess_ops->ErrorRecoveryLevel) {
				ISCSI_TCP_ZMC_ERR("Unable to recover from"
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
			ISCSI_TCP_ZMC_DEBUG("Got CRC32C DataDigest 0x%08x for"
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

static int __qnap_iscsit_tcp_zmc_xmit_datain_pdu(
	struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd,
	const struct iscsi_datain *datain
	)
{
	/* refer from iscsit_xmit_datain_pdu() */
	int ret = -ENOMEM, ent, page_off, i;
	u32 tx_size = 0;
	int total_head_len = 0, total_tail_len = 0;
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

		qnap_iscsit_zmc_do_crypto_hash_buf(conn->conn_tx_hash, 
			cmd->pdu, ISCSI_HDR_LEN, 0, NULL, 
			(u8 *)header_digest);

		total_head_len += ISCSI_CRC_LEN;
		ISCSI_TCP_ZMC_DEBUG("Attaching CRC32 HeaderDigest for DataIN PDU "
					"0x%08x\n", *header_digest);
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

	ISCSI_TCP_ZMC_DEBUG("%s: dlen:0x%x, datain->offset:0x%x, nr sg:%d, "
		"ent:%d, page_off:0x%x\n", __func__, cmd->se_cmd.data_length, 
		datain->offset, cmd->se_cmd.t_data_nents, ent, page_off);

	if (conn->conn_ops->DataDigest) {
		cmd->first_data_sg = data_sg;
		cmd->first_data_sg_off = data_sg->offset + page_off;
				
		cmd->data_crc = qnap_iscsit_zmc_do_crypto_hash_sg(
				conn->conn_tx_hash, cmd, datain->offset, 
				datain->length, cmd->padding, cmd->pad_bytes);

		cmd->first_data_sg = NULL;
		cmd->first_data_sg_off = 0;
		
		total_tail_len += ISCSI_CRC_LEN;	
		ISCSI_TCP_ZMC_DEBUG("Attached CRC32C DataDigest %d bytes, "
				"crc 0x%08x\n", datain->length + cmd->padding, 
				cmd->data_crc);		
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
	ret = 0;

	lock_sock(conn->sock->sk);
	
	for (i = 0; i < 3; i++) {
		/* we may not have data digest */
		if (i == 2 && !sg_send_rec[i].sg)
			break;

		ret = __qnap_iscsit_tcp_zmc_push_sg(conn->sock->sk, 
				sg_send_rec[i].sg, sg_send_rec[i].first_sg_off,
				sg_send_rec[i].total_len, sg_send_rec[i].str);

		if (ret != sg_send_rec[i].total_len) {
			if (ret < 0) {
				ISCSI_TCP_ZMC_WARN("(%s) fail to call "
						"do_tcp_sendpages(), ret: %d\n", 
						sg_send_rec[i].str, ret);
				goto out_release_sock;
			}
			ISCSI_TCP_ZMC_WARN("ret:%d != total:%d\n", ret, 
					sg_send_rec[i].total_len);
			WARN_ON(true);
		}
	}

	ret = 0;

out_release_sock:
	release_sock(conn->sock->sk);
exit:

	if (ret)
		qnap_iscsit_zmc_tx_thread_wait_for_tcp(conn);

	if (pdu_head_page)
		__free_page(pdu_head_page);
	if (pdu_tail_page)
		__free_page(pdu_tail_page);

	return ret;
}

static struct scatterlist *__qnap_iscsit_tcp_zmc_convert_sgl_node(
	struct se_cmd *cmd,
	struct tcp_zmc_sgl_node *sgl_node, 
	int sgl_nents
	)
{
	struct tcp_zmc_cmd_priv *priv = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;
	
	struct tcp_zmc_sgl_node *curr = NULL;
	int total, sgl_idx = 0, i, copy_count, remain;
	struct scatterlist *sgl = NULL;
	
	if (!sgl_node || !sgl_nents)
		return NULL;
	
	remain = total = sgl_nents;
	
	ISCSI_TCP_ZMC_DEBUG("sgl count:%d\n", total);
	
	sgl = kzalloc((total * sizeof(struct scatterlist)), GFP_KERNEL);
	if (!sgl)
		return NULL;
	
	ISCSI_TCP_ZMC_DEBUG("alloc sgl:0x%p\n", sgl);
	
	curr = sgl_node;
	
	/* copy all sg elements whcih limits on sgl_nents condition */
	while(curr && remain) {
		WARN_ON(!curr->sgl_count);
		copy_count = min_t(int, curr->sgl_count, remain);
#if 0
		for (i = 0; i < copy_count; i++) {
			ISCSI_TCP_ZMC_INFO("%d: page:0x%p, len:0x%x, off;0x%x\n",
				i, sg_page(&curr->zmc_wsgl[i]), 
				curr->zmc_wsgl[i].length,
				curr->zmc_wsgl[i].offset
				);
		}
#endif
		memcpy(&sgl[sgl_idx], &curr->sgl[0], 
			(copy_count * sizeof(struct scatterlist)));
#if 0
		for (i = 0; i < copy_count; i++) {
			ISCSI_TCP_ZMC_INFO("sgl[%d]: page:0x%p, len:0x%x, off;0x%x\n",
				i, sg_page(&sgl[i]), sgl[i].length, sgl[i].offset);
		}
#endif
		remain -= copy_count;
		sgl_idx += copy_count;
		curr = curr->next;
	}
	
	/* set end of table */
	sg_mark_end(&sgl[total - 1]);
	return sgl;

}

static void __qnap_iscsit_tcp_zmc_free_sgl_node(
	struct se_cmd *cmd
	)
{
	/* free all sgl nodes */
	struct tcp_zmc_cmd_priv *priv = 
			(struct tcp_zmc_cmd_priv *)cmd->cmd_dr.zmc_data;

	struct tcp_zmc_sgl_node *curr = NULL, *next = NULL;

	if (!priv->sgl_node)
		return;

	curr = (struct tcp_zmc_sgl_node *)priv->sgl_node;
	
	while (curr) {
		next = curr->next;
		WARN_ON(!curr->sgl);

		ISCSI_TCP_ZMC_DEBUG("free sgl_node:0x%p, sgl:0x%p\n", 
					curr, curr->sgl);
		kfree(curr->sgl);
		__qnap_iscsit_tcp_zmc_free_single_sgl_node(cmd, curr);
		curr = next;
	}

	priv->sgl_node = NULL;

}

static int __qnap_iscsit_tcp_zmc_destroy_obj2cmd_cache(
	struct iscsi_conn *conn,
	struct iscsit_tcp_zmc_conn_priv *priv
	)
{
	if (priv->obj2cmd_cache_id >= 0)
		ida_free(&obj2cmd_ida, priv->obj2cmd_cache_id);

	if (priv->obj2cmd_cache) {
		kmem_cache_destroy(priv->obj2cmd_cache);
		ISCSI_TCP_ZMC_DEBUG("done to destroy obj2cmd_cache for conn:0x%p\n",
				conn);	
	}
	return 0;
}

static int __qnap_iscsit_tcp_zmc_create_obj2cmd_cache(
	struct iscsi_conn *conn,
	struct iscsit_tcp_zmc_conn_priv *p
	)
{
	char name[64];

	p->obj2cmd_cache_id = ida_alloc(&obj2cmd_ida, GFP_KERNEL);
	if (p->obj2cmd_cache_id < 0) {
		ISCSI_TCP_ZMC_ERR("obj2cmd id allocation failed, %d\n", 
					p->obj2cmd_cache_id);
		return -ENOMEM;
	}

	snprintf(name, 64, "obj2cmd_cache_%d", p->obj2cmd_cache_id);
	p->obj2cmd_cache = kmem_cache_create(name, sizeof(struct obj2cmd_node),
				__alignof__(struct obj2cmd_node), 0, NULL);

	if (!p->obj2cmd_cache) {
		ISCSI_TCP_ZMC_WARN("fail to create %s for conn:0x%p\n", name, conn);
		return -ENOMEM;
	}

	ISCSI_TCP_ZMC_DEBUG("done create %s for conn:0x%p\n", name, conn);
	return 0;
}

static int __qnap_iscsit_tcp_zmc_destroy_zmc2skb_obj2_cache(
	struct iscsi_conn *conn,
	struct iscsit_tcp_zmc_conn_priv *priv
	)
{
	if (priv->zmc2skb_cache_id >= 0)
		ida_free(&zmc2skb_obj_ida, priv->zmc2skb_cache_id);

	if (priv->zmc2skb_cache) {
		kmem_cache_destroy(priv->zmc2skb_cache);
		ISCSI_TCP_ZMC_DEBUG("done to destroy zmc2skb_cache for conn:0x%p\n",
			conn);	
	}
	return 0;
}

static int __qnap_iscsit_tcp_zmc_create_zmc2skb_obj_cache(
	struct iscsi_conn *conn,
	struct iscsit_tcp_zmc_conn_priv *p
	)
{
	char name[64];

	p->zmc2skb_cache_id = ida_alloc(&zmc2skb_obj_ida, GFP_KERNEL);
	if (p->zmc2skb_cache_id < 0) {
		ISCSI_TCP_ZMC_ERR("zmc2skb_obj id allocation failed, %d\n", 
				p->zmc2skb_cache_id);
		return -ENOMEM;
	}

	snprintf(name, 64, "zmc2skb_%d", p->zmc2skb_cache_id);
	p->zmc2skb_cache = kmem_cache_create(name, sizeof(struct tcp_zmc2skb_obj),
				__alignof__(struct tcp_zmc2skb_obj), 0, NULL);
	
	if (!p->zmc2skb_cache) {
		ISCSI_TCP_ZMC_WARN("fail to create %s for conn:0x%p\n", name, conn);
		return -ENOMEM;
	}
	ISCSI_TCP_ZMC_DEBUG("done create %s for conn:0x%p\n", name, conn);
	return 0;
}

void __qnap_iscsit_tcp_zmc_free_conn_zmc2skb_obj(
	struct iscsit_tcp_zmc_conn_priv *p
	)
{
	struct tcp_zmc2skb_obj *obj = NULL, *tmp_obj = NULL;
	LIST_HEAD(__free_list);

	spin_lock_bh(&p->free_obj_list_lock);
	list_splice_tail_init(&p->free_obj_list, &__free_list);
	spin_unlock_bh(&p->free_obj_list_lock);

	list_for_each_entry_safe(obj, tmp_obj, &__free_list, obj_node) {
		list_del_init(&obj->obj_node);

		if (atomic_read(&obj->touch) != 1) {
			ISCSI_TCP_ZMC_WARN("obj touched not 1, obj:0x%p, "
					"touched:%d\n", obj, atomic_read(&obj->touch));
			WARN_ON(true);
		}
		ISCSI_TCP_ZMC_DEBUG("%s: free zmc obj:%p, skb:%p\n", __func__, 
					obj, obj->skb);
		kfree_skb(obj->skb);
		kmem_cache_free(p->zmc2skb_cache, obj);
	}

}

int __qnap_iscsit_tcp_zmc_free_conn_priv(
	struct iscsi_conn *conn
	)
{
	struct iscsit_tcp_zmc_conn_priv *priv = NULL;

	if (conn->zmc_conn_priv) {
		priv = (struct iscsit_tcp_zmc_conn_priv *)conn->zmc_conn_priv;
		conn->zmc_conn_priv = NULL;

		__qnap_iscsit_tcp_zmc_free_conn_zmc2skb_obj(priv);
		__qnap_iscsit_tcp_zmc_destroy_zmc2skb_obj2_cache(conn, priv);	
		__qnap_iscsit_tcp_zmc_destroy_obj2cmd_cache(conn, priv);
		kfree(priv);		
	}

	return 0;
}

int __qnap_iscsit_tcp_zmc_alloc_conn_priv(
	struct iscsi_conn *conn
	)
{
	struct iscsit_tcp_zmc_conn_priv *priv = NULL;
	int ret = -ENOMEM, alloc_size;

	if (!iscsi_tcp_zmc_enable) {
		pr_warn("not support iscsit tcp zmc\n");
		return -ENOTSUPP;
	}

	alloc_size = conn->zmc_ops->conn_priv_size;
	priv = kzalloc(alloc_size, GFP_KERNEL);
	if (!priv)
		return ret;

	ret = __qnap_iscsit_tcp_zmc_create_obj2cmd_cache(conn, priv);
	if (ret)
		goto fail_free_obj2cmd;

	ret = __qnap_iscsit_tcp_zmc_create_zmc2skb_obj_cache(conn, priv);
	if (ret) 
		goto fail_free_zmc2skb;

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

	conn->zmc_conn_priv = (void *)priv;
	return 0;

fail_free_zmc2skb:
	__qnap_iscsit_tcp_zmc_destroy_zmc2skb_obj2_cache(conn, priv);

fail_free_obj2cmd:
	__qnap_iscsit_tcp_zmc_destroy_obj2cmd_cache(conn, priv);

fail_exit:
	if (priv)
		kfree(priv);
	return ret;
}

static void __qnap_iscsit_tcp_zmc_free_cmd_priv(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	/* release something for zmc in se_cmd */
	__qnap_iscsit_tcp_zmc_release_se_cmd_priv(&cmd->se_cmd);

	/* release something for zmc in iscsi cmd */
	__qnap_iscsit_tcp_zmc_release_cmd_priv(conn, cmd);
}

static int __qnap_iscsit_tcp_zmc_alloc_cmd_priv(
	struct iscsi_conn *conn,
	struct iscsi_cmd *cmd
	)
{
	struct iscsit_tcp_zmc_cmd_priv *i_priv = NULL;
	struct tcp_zmc_cmd_priv *priv = NULL;
	int alloc_size = conn->zmc_ops->cmd_priv_size;

	i_priv = kzalloc(alloc_size, ((in_interrupt()) ? GFP_ATOMIC: GFP_KERNEL));
	if (!i_priv)
		return -ENOMEM;

	i_priv->node = NULL;

	/* setup struct se_cmd ... */
	priv = kzalloc(sizeof(struct tcp_zmc_cmd_priv), 
				((in_interrupt()) ? GFP_ATOMIC: GFP_KERNEL));
	if (!priv) {
		kfree(i_priv);
		return -ENOMEM;
	}

	priv->sgl_node = NULL;
	priv->sgl  = NULL;
	priv->sgl_nents = 0;
	priv->sgl_in_used = false;
	priv->data_vmap = NULL;

	cmd->se_cmd.cmd_dr.zmc_data = (void *)priv;
	cmd->zmc_cmd_priv = (void *)i_priv;
	return 0;
}

/**/
static struct iscsit_zmc_ops __iscsit_tcp_zmc_ops = {
	.name			= "iscsit_tcp_zmc" ,
	/* transport_type is same as defined in iscsi_target_core.h */
	.transport_type		= ISCSI_TCP, 
	.conn_priv_size		= sizeof(struct iscsit_tcp_zmc_conn_priv),
	.cmd_priv_size		= sizeof(struct iscsit_tcp_zmc_cmd_priv),

	/**/
	.alloc_conn_priv	= __qnap_iscsit_tcp_zmc_alloc_conn_priv,
	.free_conn_priv 	= __qnap_iscsit_tcp_zmc_free_conn_priv,
	.alloc_cmd_priv		= __qnap_iscsit_tcp_zmc_alloc_cmd_priv,
	.free_cmd_priv 		= __qnap_iscsit_tcp_zmc_free_cmd_priv,

	/* conn ops */
	.get_rx_data		= __qnap_iscsit_tcp_zmc_get_rx_data,
	.get_rx_pdu		= __qnap_iscsit_tcp_zmc_get_rx_pdu,
	.set_callback_check	= __qnap_iscsit_tcp_zmc_set_callback_check ,
	.set_sock_callback	= __qnap_iscsit_tcp_zmc_set_sock_callback ,
	.restore_sock_callback	= __qnap_iscsit_tcp_zmc_restore_sock_callback , 

	/* cmd ops */
	.dump_data_payload	= __qnap_iscsit_tcp_zmc_dump_data_payload,
	.handle_immediate_data	= __qnap_iscsit_tcp_zmc_handle_immediate_data,
	.get_dataout		= __qnap_iscsit_tcp_zmc_get_dataout,
	.xmit_datain_pdu	= __qnap_iscsit_tcp_zmc_xmit_datain_pdu,

	/**/
	.convert_sgl_node	= __qnap_iscsit_tcp_zmc_convert_sgl_node,
	.free_sgl_node		= __qnap_iscsit_tcp_zmc_free_sgl_node,
};

struct iscsit_zmc_ops *iscsit_tcp_zmc_ops = &__iscsit_tcp_zmc_ops;

