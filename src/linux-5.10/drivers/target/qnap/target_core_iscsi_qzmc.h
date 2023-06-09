#ifndef __TARGET_CORE_ISCSI_QZMC_H__
#define __TARGET_CORE_ISCSI_QZMC_H__

/**/
struct target_zmc_ops;
struct se_cmd;
struct scatterlist;
struct tcp_zmc_sgl_node;

/**/
struct tcp_zmc_sgl_node {
	struct scatterlist	*sgl;
	struct tcp_zmc_sgl_node	*next;
	int			sgl_count; /* element count for sgl */
};

struct tcp_zmc_cmd_priv {
	/* temp local store to save write sgl when to handle obj (skb's data) */
	void			*sgl_node;
	/* final write sgl we want to write ... */
	struct scatterlist	*sgl;
	u32			sgl_nents;
	bool			sgl_in_used;
	void			*data_vmap;
};

/**/
extern void __attribute__((weak)) qnap_iscsi_tcp_zmc_free_zmc_data(struct se_cmd *cmd);
extern int __attribute__((weak)) qnap_iscsi_tcp_zmc_create_zmc_data(struct se_cmd *cmd);

/**/
extern struct target_zmc_ops __attribute__((weak)) *iscsi_target_zmc_ops;

#endif
