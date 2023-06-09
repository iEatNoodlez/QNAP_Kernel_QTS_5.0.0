#ifndef __ISCSI_TARGET_ZMC_H__
#define __ISCSI_TARGET_ZMC_H__

#include <linux/types.h>

/* forward declaration */
struct ahash_request;
struct iscsi_cmd;
struct iscsi_conn;
struct iscsi_scsi_req;
struct iscsi_datain;
struct iscsi_data;
struct zmc_obj;
struct obj2cmd_node;

struct zmc_obj {
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

struct zmc_work_data {
	struct iscsi_cmd *cmd;
	struct iscsi_conn *conn;
	struct list_head work_list;
	struct list_head free_list;
	struct list_head local_obj_list;
	int obj_count;
	int total_len;
	bool put_to_wlist;
};


/* record objs per iscsi cmd */
struct obj2cmd_node {
	struct zmc_obj *obj;	
	struct obj2cmd_node *next;
};

/**/
struct iscsit_zmc_conn_ops {
	int (*set_cb_check)(struct iscsi_conn *);
	int (*set_sock_callbacks)(struct iscsi_conn *);
	int (*restore_sock_callbacks)(struct iscsi_conn *);
	int (*get_rx_pdu)(struct iscsi_conn *);
};

struct iscsit_zmc_cmd_ops {
	int (*dump_data_payload)(struct iscsi_conn *, struct iscsi_cmd *, u32, int);
	int (*get_dataout)(struct iscsi_conn *, struct iscsi_cmd *, struct iscsi_data *);
	int (*handle_immediate_data)(struct iscsi_cmd *, struct iscsi_scsi_req *, u32);
	int (*xmit_datain_pdu)(struct iscsi_conn *, struct iscsi_cmd *, 
		const struct iscsi_datain *);
};


/**/
void qnap_iscsit_tcp_zmc_release_cmd(struct iscsi_cmd *cmd);


struct __rx_act {
	/* work_sg       -> se_cmd->zmc_write_sg
	 * work_sg_nents -> se_cmd->zmc_write_nents
	 */
	struct scatterlist	*work_sg;
	int			*work_sg_nents;
	bool			copy_to_iov;
};


struct zmc_dr {
	struct kmem_cache	*obj_cache;
	wait_queue_head_t	thread_wq;

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
	struct __rx_act		rx_act;
};


void qnap_iscsit_tcp_zmc_reset_ops(void);
void qnap_iscsit_tcp_zmc_setup_ops(void);
int qnap_iscsit_tcp_zmc_check_ops(void);
int qnap_iscsit_zmc_create_obj2cmd_cache(void);
void qnap_iscsit_zmc_destory_obj2cmd_cache(void);
int qnap_iscsit_tcp_zmc_alloc_priv(struct iscsi_conn *conn);
void qnap_iscsit_tcp_zmc_free_priv(struct iscsi_conn *conn);

void qnap_iscsit_tcp_zmc_setup_conn_ops_on_conn(struct iscsi_conn *conn);
void qnap_iscsit_tcp_zmc_setup_cmd_ops_on_iscsi_cmd(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd);


/* iscsi tcp zmc conn ops */
int qnap_iscsit_tcp_zmc_set_sock_callbacks(struct iscsi_conn *conn);
int qnap_iscsit_tcp_zmc_restore_sock_callbacks(struct iscsi_conn *conn);
int qnap_iscsit_tcp_zmc_get_rx_pdu(struct iscsi_conn *conn);

/* iscsi tcp zmc cmd ops */
int qnap_iscsit_tcp_zmc_rx_data(struct iscsi_conn *conn, struct iscsi_cmd *cmd, 
	struct kvec *iov, int iov_count, int data_len);

int qnap_iscsit_tcp_zmc_dump_data_payload(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, u32 payload_len, int dump_padding_digest);

int qnap_iscsit_tcp_zmc_get_dataout(struct iscsi_conn *conn, struct iscsi_cmd *cmd, 
	struct iscsi_data *hdr);

int qnap_iscsit_tcp_zmc_handle_immediate_data(struct iscsi_cmd *cmd, 
	struct iscsi_scsi_req *hdr, u32 length);

int qnap_iscsit_tcp_zmc_xmit_datain_pdu(struct iscsi_conn *conn, 
	struct iscsi_cmd *cmd, const struct iscsi_datain *datain);


/* === wrapper code from iscsi_target.c === */
int qnap_iscsit_tcp_zmc_add_reject(struct iscsi_conn *conn,
	u8 reason, unsigned char *buf);

int qnap_iscsit_tcp_zmc_target_rx_opcode(struct iscsi_conn * conn, 
	unsigned char * buf);

void qnap_iscsit_tcp_zmc_do_crypto_hash_buf(struct ahash_request *hash,
	const void *buf, u32 payload_length, u32 padding,
	u8 *pad_bytes, u8 *data_crc);

u32 qnap_iscsit_tcp_zmc_do_crypto_hash_sg(struct ahash_request * hash, 
	struct iscsi_cmd *cmd, u32 data_offset, u32 data_length, 
	u32 padding, u8 *pad_bytes);

void qnap_iscsit_tcp_zmc_rx_thread_wait_for_tcp(struct iscsi_conn * conn);
void qnap_iscsit_tcp_zmc_tx_thread_wait_for_tcp(struct iscsi_conn * conn);




#endif
