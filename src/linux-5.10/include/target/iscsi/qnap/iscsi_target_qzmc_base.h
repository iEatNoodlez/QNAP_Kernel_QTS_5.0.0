#ifndef __ISCSI_TARGET_QZMC_BASE_H__
#define __ISCSI_TARGET_QZMC_BASE_H__

#include <linux/types.h>
#include <linux/list.h>

/* forward declaration */
struct tcp_zmc_sgl_node;
struct ahash_request;
struct se_cmd;
struct iscsi_cmd;
struct iscsi_conn;
struct iscsi_scsi_req;
struct iscsi_datain;
struct iscsi_data;
struct kvec;

/**/
struct iscsit_zmc_ops {
	char			name[64];
	int			transport_type;
	int			cmd_priv_size;
	int			conn_priv_size;
	struct list_head	node;

	int (*alloc_conn_priv)(struct iscsi_conn *conn);
	int (*free_conn_priv)(struct iscsi_conn *conn);
	int (*alloc_cmd_priv)(struct iscsi_conn *conn, struct iscsi_cmd *cmd);
	void (*free_cmd_priv)(struct iscsi_conn *conn, struct iscsi_cmd *cmd);

	/* ================ 
	 * iscsi conn ops
	 * ================ 
	 */
	int (*get_rx_data)(struct iscsi_conn *conn, struct iscsi_cmd *cmd, 
			struct kvec *iov, int iov_count, int data_size);

	int (*get_rx_pdu)(struct iscsi_conn *conn);
	int (*set_callback_check)(struct iscsi_conn *conn);
	int (*set_sock_callback)(struct iscsi_conn *conn);
	int (*restore_sock_callback)(struct iscsi_conn *conn);

	/* ================ 
	 * iscsi cmd ops
	 * ================ 
	 */
	int (*dump_data_payload)(struct iscsi_conn *conn, struct iscsi_cmd *cmd,
		u32 len, bool dump_padding_digest);

	int (*get_dataout)(struct iscsi_conn *conn, struct iscsi_cmd *cmd, 
		struct iscsi_data *hdr);

	int (*handle_immediate_data)(struct iscsi_conn *conn, 
		struct iscsi_cmd *cmd, struct iscsi_scsi_req *hdr, u32 len);

	int (*xmit_datain_pdu)(struct iscsi_conn *conn, struct iscsi_cmd *cmd, 
		const struct iscsi_datain *datain);	

	/* ========================
	 * other helpers ops
	 * ========================
	 */
	struct scatterlist *(*convert_sgl_node)(struct se_cmd *se_cmd, 
			struct tcp_zmc_sgl_node *sgl_node, int sgl_nents);

	void (*free_sgl_node)(struct se_cmd *se_cmd);

};


#endif

