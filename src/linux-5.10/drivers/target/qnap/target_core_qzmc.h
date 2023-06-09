#ifndef __TARGET_CORE_QZMC_H__
#define __TARGET_CORE_QZMC_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

/**/
struct target_zmc_ops;
struct request_queue;
struct se_device;
struct se_cmd;
struct bio_list;
struct bio;
struct scatterlist;
struct sg_mapping_iter;
struct zmc_sgl_node;


/**/
struct fio_data_set {
	struct scatterlist	*sgl;
	u32 			sgl_nents;
	bool 			async_io;
};

struct iblock_op_set {
	struct bio *(*ib_get_bio)(struct se_cmd *cmd, sector_t lba, u32 sg_num, 
		int op, int op_flags);
	int (*ib_alloc_bip)(struct se_cmd *cmd, struct bio *bio, 
		struct sg_mapping_iter *miter);

	void (*ib_submit_bio)(struct bio_list *list);
	void (*ib_comp_cmd)(struct se_cmd *cmd);
};

/**/
void qnap_zmc_iblock_get_write_op_flags(struct se_cmd *cmd, 
	struct request_queue *q, int *op_flags);

/**/
extern int __attribute__((weak)) qnap_zmc_create_sgl_node_cache(
	struct se_device *dev, u32 dev_index);

extern void __attribute__((weak)) qnap_zmc_destroy_sgl_node_cache(
	struct se_device *dev);

extern void __attribute__((weak)) qnap_zmc_init_data(struct se_cmd *cmd);

extern void __attribute__((weak)) qnap_target_zmc_register(struct target_zmc_ops *p);
extern void __attribute__((weak)) qnap_target_zmc_unregister(struct target_zmc_ops *p);

#endif
