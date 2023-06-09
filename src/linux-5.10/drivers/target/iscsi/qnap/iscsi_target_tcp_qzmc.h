#ifndef __ISCSI_TARGET_TCP_QZMC_H__
#define __ISCSI_TARGET_TCP_QZMC_H__

#include <linux/types.h>

/**/
struct iscsit_zmc_ops;
struct iscsi_conn;
struct iscsi_cmd;
struct obj2cmd_node;
struct kmem_cache;

/**/
#define	msg_prefix	"[iscsi-tcp-zmc]: "
#define ISCSI_TCP_ZMC_DEBUG(f, arg...) pr_debug(msg_prefix f, ## arg)
#define ISCSI_TCP_ZMC_INFO(f, arg...) pr_info(msg_prefix f, ## arg)
#define ISCSI_TCP_ZMC_WARN(f, arg...) pr_warn(msg_prefix f, ## arg)
#define ISCSI_TCP_ZMC_ERR(f, arg...) pr_err(msg_prefix f, ## arg)	


/* obj to record the skb in __qnap_iscsit_tcp_zmc_sw_recv() */
struct tcp_zmc2skb_obj {
	/* work area to map skb to sgvec */	
	struct list_head work_node;
	struct list_head obj_node;
	struct sk_buff *skb;
	u32 len;
	u32 offset;	

	/* work area to map skb to sgvec */
	u32 work_len;
	u32 work_off;
	struct iscsi_cmd *cmd;
	atomic_t touch;
};

/* obj(s) per iscsi cmd which contains the tcp_zmc2skb obj */
struct obj2cmd_node {
	struct tcp_zmc2skb_obj *obj;	
	struct obj2cmd_node *next;
};

/* temp storage to record work data */
struct tcp_zmc_work_data {
	struct iscsi_cmd *cmd;
	struct iscsi_conn *conn;
	struct list_head work_list;
	struct list_head free_list;
	struct list_head local_obj_list;
	int obj_count;
	int total_len;
	bool put_to_wlist;
};

struct iscsit_tcp_zmc_cmd_priv {
	struct obj2cmd_node	*node;
};

struct iscsit_tcp_zmc_conn_priv {	
	struct kmem_cache 	*obj2cmd_cache;
	struct kmem_cache	*zmc2skb_cache;
	wait_queue_head_t	thread_wq;

	/* following obj(s) is for tcp_zmc2skb obj */
	spinlock_t		free_obj_list_lock;
	spinlock_t		obj_list_lock;

	/* local storage to be used in __qnap_iscsit_zmc_sw_recv() to collect objs */
	struct list_head	local_obj_list;

	/* to record obj we want to free */
	struct list_head	free_obj_list;

	/* obj_list, query_len, total_len and obj_cnt 
	 * are protected by obj_list_lock 
	 */
	struct list_head	obj_list;
	int			query_len;
	int			total_len;
	atomic_t		obj_cnt;
	atomic_t		set_cb;

	int			obj2cmd_cache_id;
	int			zmc2skb_cache_id;	
};

/**/
extern struct iscsit_zmc_ops __attribute__((weak)) *iscsit_tcp_zmc_ops;

#endif

