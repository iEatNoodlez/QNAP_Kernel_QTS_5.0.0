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
#define MAX_UNMAP_MB_SIZE	(64)

#define MAX_IO_KB_PER_SG		(PAGE_SIZE)
#define D4_SG_LIST_IO_ALLOC_SIZE	(1 << 20)

void qnap_transport_parse_naa_6h_vendor_specific(void *se_dev, unsigned char *buf);
int qnap_transport_config_blkio_dev(struct se_device *se_dev);
int qnap_transport_config_fio_dev(struct se_device *se_dev);

#ifdef QNAP_SHARE_JOURNAL
int qnap_transport_check_is_journal_support(struct se_device *se_dev);
#endif

int qnap_transport_blkdev_issue_discard(struct se_cmd *se_cmd,
	bool special_discard, struct block_device *bdev, sector_t sector, 
	sector_t nr_sects, gfp_t gfp_mask, unsigned long flags);

int qnap_transport_bd_is_fbdisk_dev(struct block_device *bd);

int qnap_transport_get_thin_data_status_on_thin(struct se_device *se_dev, 
	u64 *total_512_sector,	u64 *used_512_sector);

sense_reason_t qnap_transport_check_capacity_changed(struct se_cmd *se_cmd);

int qnap_change_dev_size(struct se_device *se_dev);

void qnap_init_se_dev_dr(struct qnap_se_dev_dr *dr);
void qnap_init_se_dev_attr_dr(struct qnap_se_dev_attr_dr *dr);

void qnap_transport_tmr_drain_cmd_list(struct se_device *dev, 
	struct se_queue_obj *qobj, struct se_cmd *prout_cmd, 
	struct se_node_acl *tmr_nacl, int tas, 
	int (*check_cdb_and_preempt)(struct list_head *, struct se_cmd *),
	struct list_head *preempt_and_abort_list);

void qnap_transport_fd_upadte_write_result(
	struct se_cmd *se_cmd, int *original_result);

int qnap_transport_fd_check_dm_thin_cond(struct se_device *se_dev);
int qnap_transport_check_cmd_hit_thin_threshold(struct se_cmd *se_cmd);
int qnap_transport_check_dev_hit_thin_threshold(struct se_device *se_dev);
void qnap_transport_check_dev_under_thin_threshold(struct se_device *se_dev);



#ifdef SUPPORT_TP
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

bool qnap_check_v_sup(struct se_device *dev);
int qnap_set_dev_write_cache(struct se_device *se_dev, bool set_wc);
int qnap_transport_vfs_fsync_range(struct se_device *dev, loff_t s, 
					loff_t e, int data_sync);

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
sense_reason_t qnap_transport_convert_rc_to_tcm_sense_reason(u32 _rc);
int qnap_transport_create_devinfo(struct se_cmd *cmd, struct __dev_info *dev_info);
void *qnap_transport_get_fbdisk_file(void *fb_dev, sector_t lba, u32 *index);
void qnap_transport_show_cmd(char *prefix_str, struct se_cmd *se_cmd);

void qnap_transport_show_set_task_aborted_msg(struct se_cmd *cmd, bool to_set);
void qnap_transport_show_task_aborted_msg(struct se_cmd *cmd, int type, 
	bool done_wait, bool print_msg);

void qnap_transport_tmr_set_code(struct se_cmd *cmd, u8 code);
int qnap_transport_tmr_get_code(struct se_cmd *cmd);
bool qnap_transport_check_cmd_aborted_by_release_conn(struct se_cmd *cmd, bool print_msg);
bool qnap_transport_check_cmd_aborted_by_tmr(struct se_cmd *cmd, bool print_msg);
bool qnap_transport_check_cmd_in_tmr_abort_wq(struct se_cmd *cmd);
bool qnap_transport_check_cmd_aborted(struct se_cmd *cmd, bool print_msg);
void qnap_transport_create_fb_bio_rec_kmem(struct se_device *dev, int dev_index);
void qnap_transport_destroy_fb_bio_rec_kmem(struct se_device *dev);
void qnap_transport_init_se_cmd_dr(struct se_cmd *cmd);

#endif

