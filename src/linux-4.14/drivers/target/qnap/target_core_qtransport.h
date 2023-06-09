#ifndef TARGET_CORE_QTRANSPORT_H
#define TARGET_CORE_QTRANSPORT_H

#include "target_core_qlib.h"


/* 2009/09/23 Nike Chen add for default initiator */
#define QNAP_DEFAULT_INITIATOR "iqn.2004-04.com.qnap:all:iscsi.default.ffffff"
#define FC_DEFAULT_INITIATOR "ff:ff:ff:ff:ff:ff:ff:ff"

#if defined(IS_G)
#define DEFAULT_INITIATOR	"iqn.2004-04.com.nas:all:iscsi.default.ffffff"
#elif defined(Athens)
#define DEFAULT_INITIATOR	"iqn.2004-04.com.cisco:all:iscsi.default.ffffff"
#else
#define DEFAULT_INITIATOR	QNAP_DEFAULT_INITIATOR
#endif

#define POOL_BLK_SIZE_512_KB	(512)
#define POOL_BLK_SIZE_1024_KB	(1024)
#define QIMAX_UNMAP_DESC_COUNT	(1)
#define MAX_TRANSFER_LEN_MB	(1)
#define MAX_UNMAP_MB_SIZE	(256)

#define MAX_IO_KB_PER_SG		(PAGE_SIZE)
#define D4_SG_LIST_IO_ALLOC_SIZE	(1 << 20)

/* max # of bios to submit at a time, please refer the target_core_iblock.c */
#define BLOCK_MAX_BIO_PER_TASK		32
#define RW_TASK_FLAG_DO_FUA		0x1

typedef enum {
	SUBSYSTEM_BLOCK    = 0,
	SUBSYSTEM_FILE        ,
	SUBSYSTEM_PSCSI       ,
	MAX_SUBSYSTEM_TYPE    ,
} SUBSYSTEM_TYPE;

#define IS_TIMEOUT(time) \
    ((time_after(jiffies, time) && (!time)))

typedef ssize_t (*VFS_RW)(
	struct file *file, const struct iovec __user *vec,
	unsigned long vlen, loff_t *pos
	);

typedef struct cb_data {
	atomic_t		bio_count;
	atomic_t		bio_err_count;
	struct completion	*wait;
	int			nospc_err;
} __attribute__ ((packed)) CB_DATA;

typedef struct _io_rec{
	struct	list_head	node;
	void			*ib_dev;
	CB_DATA 		*cb_data;
	u32			nr_blks;
	bool			transfer_done;
} __attribute__ ((packed)) IO_REC;


struct __bio_batch {
	atomic_t		done;
	unsigned long		flags;
	struct completion	*wait;
	int			nospc_err;
};

#define TASK_FLAG_DO_FUA        0x1
typedef struct gen_rw_task{
	void			*se_dev;
	struct scatterlist	*sg_list;
	unsigned long		timeout_jiffies;
	sector_t		lba;
	u32			sg_nents;

	/* 1. (nr_blks << dev_bs_order) = sum of len for all sg elements 
	 * 2. the purpose of sg lists
	 * - for read: Usually, they are i/o buffer
	 * - for write:
	 * (a) Usually, they are buffer for normal write i/o
	 * (b) for special write discard, they are buffer for non-aligned /
	 * non-multipled write data i/o
	 */
	u32			nr_blks;
	u32			s_nr_blks;
	u32			dev_bs_order;
	u32			task_flag;
	enum dma_data_direction	dir;
	bool			is_timeout;
	int			ret_code;
}__attribute__ ((packed)) GEN_RW_TASK;

void qnap_transport_parse_naa_6h_vendor_specific(void *se_dev, unsigned char *buf);
int qnap_transport_config_blkio_dev(struct se_device *se_dev);
int qnap_transport_config_fio_dev(struct se_device *se_dev);

#ifdef QNAP_SHARE_JOURNAL
int qnap_transport_check_is_journal_support(struct se_device *se_dev);
#endif

void qnap_transport_make_rw_task(void *rw_task, struct se_device *se_dev,
	sector_t lba, u32 nr_blks, unsigned long timeout, 
	enum dma_data_direction dir);

int qnap_transport_blkdev_issue_discard(struct se_cmd *se_cmd,
	bool special_discard, struct block_device *bdev, sector_t sector, 
	sector_t nr_sects, gfp_t gfp_mask, unsigned long flags);

int qnap_transport_bd_is_fbdisk_dev(struct block_device *bd);

int qnap_transport_get_thin_data_status_on_thin(struct se_device *se_dev, 
	u64 *total_512_sector,	u64 *used_512_sector);

int qnap_transport_is_fio_blk_backend(struct se_device *se_dev);

sense_reason_t qnap_transport_check_capacity_changed(struct se_cmd *se_cmd);

void qnap_tmf_init_tmf_val(struct se_cmd *se_cmd);

void qnap_transport_tmr_abort_task(struct se_device *dev, struct se_tmr_req *tmr,
	struct se_session *se_sess);

void qnap_transport_tmr_drain_state_list(struct se_device *dev,
	struct se_cmd *prout_cmd, struct se_session *tmr_sess, int tas, 
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),
	struct list_head *preempt_and_abort_list);

int qnap_transport_get_ac_and_uc_on_thin(struct se_device *se_dev, 
	u32 *ac, u32 *uc);

int qnap_change_dev_size(struct se_device *se_dev);

void qnap_transport_enumerate_hba_for_deregister_session(
	struct se_session *se_sess);

void qnap_transport_config_zc_val(struct se_device *dev);
void qnap_init_se_dev_dr(struct qnap_se_dev_dr *dr);
int qnap_transport_exec_wt_cmd(struct se_cmd *se_cmd);
int qnap_transport_processing_thread(void *param);
int qnap_transport_exec_rt_cmd(struct se_cmd *se_cmd);
int qnap_transport_read_processing_thread(void *param);

void qnap_transport_tmr_drain_cmd_list(struct se_device *dev, 
	struct se_queue_obj *qobj, struct se_cmd *prout_cmd, 
	struct se_node_acl *tmr_nacl, int tas, 
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),
	struct list_head *preempt_and_abort_list);

#ifdef SUPPORT_TP
void qnap_transport_upadte_write_result_on_fio_thin(
	struct se_cmd *se_cmd, int *original_result);

int qnap_transport_check_cmd_hit_thin_threshold(struct se_cmd *se_cmd);
int qnap_transport_check_dm_thin_cond(struct se_device *se_dev);
int qnap_transport_get_thin_allocated(struct se_device *se_dev);
#endif

#ifdef ISCSI_D4_INITIATOR
extern void core_tpg_add_node_to_devs(struct se_node_acl *acl, 
	struct se_portal_group *tpg, struct se_lun *lun_orig);

extern int core_enable_device_list_for_node(struct se_lun *lun, 
	struct se_lun_acl *lun_acl, u64 mapped_lun, bool lun_access_ro,
	struct se_node_acl *nacl, struct se_portal_group *tpg);

struct se_node_acl *qnap_tpg_get_initiator_node_acl(
	struct se_portal_group *tpg, unsigned char *initiatorname);

void qnap_tpg_copy_node_devs(struct se_node_acl *dest, 
	struct se_node_acl *src, struct se_portal_group *tpg);

void qnap_tpg_add_node_to_devs_when_add_lun(
	struct se_portal_group *tpg, struct se_lun *lun);
#endif

int qnap_target_exec_random_task(struct se_cmd *se_cmd);
void qnap_transport_print_local_time(void);

int qnap_transport_drop_fb_cmd(struct se_cmd *se_cmd, int type);
int qnap_transport_drop_bb_cmd(struct se_cmd *se_cmd, int type);

bool qnap_transport_create_sess_lio_cmd_cache(struct qnap_se_sess_dr *dr,
	int idx, size_t alloc_size,size_t align_size);
void qnap_transport_destroy_sess_lio_cmd_cache(struct qnap_se_sess_dr *dr);
void *qnap_transport_alloc_sess_lio_cmd(struct qnap_se_sess_dr *dr, gfp_t gfp_mask);
void qnap_transport_free_sess_lio_cmd(struct qnap_se_sess_dr *dr, void *p);

sense_reason_t qnap_transport_iblock_execute_write_same_direct(
	struct block_device *bdev, struct se_cmd *cmd);

bool qnap_check_v_sup(struct se_device *dev);
int qnap_set_write_cache(struct se_device *se_dev, bool set_wc);

struct node_info {
	unsigned char *i_port;
	unsigned char *i_sid;
	unsigned char *t_port;
	u64 sa_res_key;
	u32 mapped_lun;
	u32 target_lun;
	bool res_holder;
	bool all_tg_pt;
	u16 tpgt;
	u16 port_rpti;
	u16 type;
	u16 scope;
};


int __qnap_scsi3_parse_aptpl_data(
	struct se_device *se_dev,
	char *data,
	struct node_info *s,
	struct node_info *d
	);

int __qnap_scsi3_check_aptpl_metadata_file_exists(
	struct se_device *dev,
	struct file **fp
	);

int qnap_transport_scsi3_check_aptpl_registration(
	struct se_device *dev,
	struct se_portal_group *tpg,
	struct se_lun *lun,
	struct se_session *se_sess,
	struct se_node_acl *nacl,
	u32 mapped_lun
	);

int qnap_transport_check_aptpl_registration(
	struct se_session *se_sess,
	struct se_node_acl *nacl,
	struct se_portal_group *tpg
	);

#ifdef ISCSI_MULTI_INIT_ACL
void *qnap_target_add_qnap_se_nacl(char *initiator_name, struct qnap_se_nacl_dr *dr);
void qnap_target_init_qnap_se_nacl(struct qnap_se_nacl_dr *dr);
void qnap_target_free_qnap_se_nacl(void *map, struct qnap_se_nacl_dr *dr);
void qnap_target_free_all_qnap_se_nacls(struct qnap_se_nacl_dr *dr);
#endif

int qnap_transport_spc_cmd_size_check(struct se_cmd *cmd);
sense_reason_t qnap_transport_check_report_lun_changed(struct se_cmd *se_cmd);
bool qnap_transport_hit_read_deletable(struct qnap_se_dev_dr *dev_dr, u8 *cdb);
sense_reason_t qnap_transport_convert_rc_to_tcm_sense_reason(RC rc);
int qnap_transport_create_devinfo(struct se_cmd *cmd, struct __dev_info *dev_info);
void *qnap_transport_get_fbdisk_file(void *fb_dev, sector_t lba, u32 *index);
void qnap_transport_show_cmd(char *prefix_str, struct se_cmd *se_cmd);
bool qnap_transport_check_iscsi_fabric(struct se_cmd *se_cmd);

#endif

