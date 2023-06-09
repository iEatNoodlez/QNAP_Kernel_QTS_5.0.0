#ifndef __TARGET_CORE_ISCSI_ZMC_H__
#define __TARGET_CORE_ISCSI_ZMC_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

/* forward declaration */
struct target_iscsi_zmc_data;
struct zmc_wsgl_node;
struct qnap_se_dev_dr;
struct se_cmd;
struct scatterlist;
struct bio;
struct bio_list;

#define ZMC_W_SG_INUSE_INVALID       -1
#define ZMC_W_SG_INUSE_FREED         0
#define ZMC_W_SG_INUSE_VALID         1

#define	msg_prefix	"[iscsi-tcp-zmc]: "
#define ISCSI_ZMC_DEBUG(f, arg...) pr_debug(msg_prefix f, ## arg)
#define ISCSI_ZMC_INFO(f, arg...) pr_info(msg_prefix f, ## arg)
#define ISCSI_ZMC_WARN(f, arg...) pr_warn(msg_prefix f, ## arg)
#define ISCSI_ZMC_ERR(f, arg...) pr_err(msg_prefix f, ## arg)	

/**/
typedef unsigned __bitwise sense_reason_t;

/**/
struct zmc_wsgl_node {
	struct scatterlist	*zmc_wsgl;
	struct zmc_wsgl_node	*next;
	int			zmc_wsgl_count; /* element count for zmc_wsgl */
};

struct ib_op_set {
	struct bio *(*ib_get_bio)(struct se_cmd *, sector_t, u32, int, int);
	int (*ib_alloc_bip)(struct se_cmd *, struct bio *);
	void (*ib_submit_bio)(struct bio_list *);
	void (*ib_comp_cmd)(struct se_cmd *);
};

struct target_iscsi_zmc_scsi_op {
	sense_reason_t (*write)(struct se_cmd *, struct scatterlist *, u32, void *);
	sense_reason_t (*write_same)(struct se_cmd *, void *);
	sense_reason_t (*ats)(struct se_cmd *);
	sense_reason_t (*ats_callback)(struct se_cmd *, bool, int *);
};

struct target_iscsi_zmc_op {
	struct scatterlist *(*prep_wsgl)(struct target_iscsi_zmc_data *,
			struct zmc_wsgl_node *, int);

	struct zmc_wsgl_node *(*alloc_wsgl_node)(struct target_iscsi_zmc_data *,
			struct qnap_se_dev_dr *, int);

	void (*push_wsgl_node)(struct target_iscsi_zmc_data *,
			struct zmc_wsgl_node *);

	void (*free_all_wsgl_node)(struct target_iscsi_zmc_data *,
			struct qnap_se_dev_dr *);

	int *(*kmap_wsgl)(struct target_iscsi_zmc_data *, int);
	int (*kunmap_wsgl)(struct target_iscsi_zmc_data *);
	int (*free_wsgl_mem)(struct target_iscsi_zmc_data *);

};

/**/
struct target_iscsi_zmc_data {
	/* refer ZMC_W_SG_INUSE_XXXX */
	atomic_t				write_sg_inuse;
	atomic_t				wsgl_nents;
	void					*data_vmap;

	/* temp local store to save write sgl when to handle obj (skb's data) */
	void					*wsgl_node;
	/* final write sgl we want to write ... */
	struct scatterlist			*wsgl;

	struct target_iscsi_zmc_op		*ops;

	/* scsi_op will be prepared in qnap_target_iscsi_zmc_prep_backend_op() */
	struct target_iscsi_zmc_scsi_op		*scsi_ops;
};

/**/
int qnap_target_iscsi_zmc_alloc_sgl(enum dma_data_direction data_direction,
		bool *alloc_done);

/**/
int qnap_target_iscsi_zmc_exec_rw(struct se_cmd *cmd);
int qnap_target_iscsi_zmc_exec_write(struct se_cmd *cmd, 
		struct scatterlist *sgl, u32 sgl_nents, void *data);

/**/
void qnap_target_iscsi_zmc_destory_wsgl_node_cache(struct qnap_se_dev_dr *dr);
int qnap_target_iscsi_zmc_create_wsgl_node_cache(struct qnap_se_dev_dr *dr, 
		u32 dev_index);

void qnap_target_iscsi_zmc_free_wsgl_node(struct qnap_se_dev_dr *dr, 
		struct zmc_wsgl_node *p);

void qnap_target_iscsi_zmc_prep_backend_op(struct se_cmd *cmd);
void qnap_target_se_cmd_iscsi_zmc_free(struct se_cmd *se_cmd);
void qnap_target_se_cmd_iscsi_zmc_alloc(struct se_cmd *se_cmd);


#endif

