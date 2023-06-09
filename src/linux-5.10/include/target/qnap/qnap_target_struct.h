/**
 *
 * @file	qnap_target_struct.h
 * @brief	QNAP LIO target code strcuture declaration which will be embedded
 *              in native LIO code data structure.
 * @author	Adam Hsu
 * @date	2016/11/15
 */
#ifndef __QNAP_TARGET_STRUCT_H__
#define __QNAP_TARGET_STRUCT_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

#ifdef CONFIG_MACH_QNAPTS

#include <target/iscsi/qnap/iscsi_target_qzmc_base.h>
#include <target/qnap/target_core_qzmc_base.h>
#include <target/qnap/qnap_target_sbc_ops.h>
#include <target/qnap/qnap_target_spc_ops.h>

/* forward declaration */
struct se_portal_group;
struct se_device;
struct se_cmd;

#define QNAP_TARGET_PERF_FEATURE     1
#define INITIATOR_NAME_LEN	256

/**/
struct drain_io_data {
	void	*data0;
	void	*data1;
};

struct qnap_io_ops {
	/* for i/o draning method */
	int (*pre_do_drain_io)(struct se_cmd *cmd, struct drain_io_data *data);
	int (*after_do_drain_io)(struct se_cmd *cmd, struct drain_io_data *data);
	int (*do_drain_io)(struct se_cmd *cmd, int type);
	void (*release_drain_io_res)(struct se_cmd *cmd);
	bool (*check_io_drained)(struct se_cmd *cmd);

	/* fast-block-clone method 
	 * -ENOTSUPP: not support fast-block-clone
	 * -ENOSPC: no space case
	 * -EINVAL: other failure
	 * 0: good to execute fast-block-clone
	 */
	int (*fbc_xcopy)(struct se_device *src, struct se_device *dst,
		sector_t src_lba, sector_t dst_lba, u64 *copy_bytes);
	int (*fbc_odx)(struct se_device *src, struct se_device *dst, 
		sector_t src_lba, sector_t dst_lba, u64 *copy_bytes);

	/* simple tmr patch */
	void (*tmr_lunreset_drain_state)(struct se_cmd *cmd, 
		struct se_portal_group *tpg);
	void (*tmr_abort_task)(struct se_cmd *cmd, 
		struct se_portal_group *tpg);
};

struct qnap_se_cmd_odx {
	void		*odx_tpg;
	void		*odx_cmd;
	u64		transfer_counts;
	u64 		cmd_id_lo;
	u64 		cmd_id_hi;
	u64 		tpg_id_lo;
	u64 		tpg_id_hi;
	u64		initiator_id_lo;
	u64		initiator_id_hi;
	u32		list_id;
	u32		sac;
	int		cmd_type;
	bool		is_odx_cmd;
	atomic_t	odx_cmd_count;
	atomic_t	odx_tpg_count;
};

struct qnap_se_tpg_odx {
	void	*odx_tpg;
	u64	tpg_id_hi;
	u64	tpg_id_lo;
};

struct qnap_odx_tf_ops {
	int (*it_nexus_discard_all_tokens)(char *initiator_name, u8 *isid_buf,
			u8 *login_ip, struct qnap_se_tpg_odx *tpg_odx_dr);
};

#ifdef ISCSI_MULTI_INIT_ACL
struct qnap_se_node_acl {
	struct list_head	acl_node;
	char			initiatorname[INITIATOR_NAME_LEN];
};

struct qnap_se_nacl_dr {
	spinlock_t		acl_node_lock;
	struct list_head	acl_node_list;
};
#endif

/* setup in tmr_code of struct qnap_se_cmd_dr */
#define QNAP_TMR_CODE_ABORT_TASK                (1)
#define QNAP_TMR_CODE_ABORT_TASK_SET            (2)
#define QNAP_TMR_CODE_CLEAR_ACA                 (3)
#define QNAP_TMR_CODE_CLEAR_TASK_SET            (4)
#define QNAP_TMR_CODE_LUN_RESET                 (5)
#define QNAP_TMR_CODE_TARGET_WARM_RESET         (6)
#define QNAP_TMR_CODE_TARGET_COLD_RESET         (7)

/* 1. QNAP_CMD_T_XXXX was setup in cmd_t_state of struct qnap_se_cmd_dr
 * 2. QNAP_CMD_T_ICF_XXXX is flag for struct iscsi_cmd
 */
#define QNAP_CMD_T_ICF_DELAYED_REMOVE           (15)

#define QNAP_CMD_T_IN_TMR_ABORT_WQ              (29)
#define QNAP_CMD_T_RELEASE_FROM_CONN            (30)
#define QNAP_CMD_T_QUEUED                       (31)

/* sense_reason_t MUST be same as one in target_core_base.h */
typedef unsigned __bitwise sense_reason_t;

/* these are for io dropping method */
struct __bio_obj {
	struct list_head	bio_rec_lists;
	spinlock_t		bio_rec_lists_lock;
	atomic_t		bio_rec_count;
	atomic_t		bio_drain_count;
};

/* for fio, the iov will be prepared per se_cmd  so it is safe to do this */
struct __iov_obj {
	spinlock_t	iov_rec_lock;
	void		*iov_rec;
	bool		io_drained;
};

struct qnap_se_cmd_dr {
	int			where_cmd;

	/* this is bitmap for QNAP_CMD_T_XXXX */
	unsigned long		cmd_t_state;

	/* this is for QNAP_TMR_CODE_XXXX */
	atomic_t		tmr_code;
	atomic_t		tmr_resp_tas;	     /* 0: not report TAS, 1: report TAS */
	atomic_t		tmr_diff_it_nexus;   /* 0: same i_t_nexus, 1: different i_t_nexus */

	struct list_head	se_queue_node;

	struct work_struct	random_work;
#ifdef QNAP_TARGET_PERF_FEATURE
	struct work_struct	seqrd_work;
#endif
	struct work_struct	unmap_work;
	struct work_struct	sync_cache_work;
	struct work_struct	odx_wut_work;
	/* special for __qnap_wq_iblock_comp_cmd_flush_thin() */
	struct work_struct	ib_comp_work;
	struct __bio_obj	bio_obj;
	struct __iov_obj	iov_obj;

	/* Need this for special cases due to some commands response will always 
	 * return TCM_LOGICAL_UNIT_COMMUNICATION_FAILURE when they were fail
	 * 
	 * normal: work_io_err is TCM_NO_SENSE
	 * special: work_io_err is TCM_XXXXX
	 */
	sense_reason_t		work_io_err;

#ifdef SUPPORT_ISCSI_ZERO_COPY
	bool			digest_zero_copy_skip;
#endif
#ifdef CONFIG_ISCSI_CMD_OVTHLD
	unsigned long		laty_begin_time;
	unsigned long		laty_end_time;
	unsigned long		laty_elapsed_time;
	struct timex		laty_txc;
#endif
#ifdef CONFIG_QTS_CEPH
	u32			sense_info;
	sense_reason_t		sense_reason;
#endif

	/* zmc_data, zmc_ops and transport_zmc_ops are for zero-memory-copy feature */
	void			*zmc_data;
	void			*transport_zmc_ops;
	struct target_zmc_ops	*zmc_ops;
	struct qnap_io_ops	io_ops;
	struct qnap_sbc_ops	sbc_ops;
	struct qnap_spc_ops	spc_ops;
};

#define QNAP_DF_USING_PROVISION			0x00000100
#define QNAP_DF_USING_NAA			0x00000200
#define QNAP_DF_USING_QLBS			0x00000400

#define QNAP_SE_DEV_PROVISION_LEN		32
#define QNAP_SE_DEV_NAA_LEN			32

#define QNAP_DEV_ATTR_PROVISION_WRITE_ONCE	0x00000001
#define QNAP_DEV_ATTR_NAA_WRITE_ONCE		0x00000002
#define QNAP_DEV_ATTR_QLBS_WRITE_ONCE		0x00000004

/* qnap device type */
#define QNAP_DT_UNKNOWN		0
#define QNAP_DT_FIO_BLK		1
#define QNAP_DT_FIO_FILE	2
#define QNAP_DT_IBLK_FBDISK	3
#define QNAP_DT_IBLK_BLK	4
#define QNAP_DT_RBD		5
#define QNAP_DT_PSCSI		6

/* embedded in struct se_dev */
struct qnap_se_dev_dr {
	/* refer QNAP_DT_xxx */
	u32			dev_type;
	/* refer QNAP_DF_USING_xxx */
	u32			dev_flags;
	u8			dev_provision[QNAP_SE_DEV_PROVISION_LEN];
	u8			dev_naa[QNAP_SE_DEV_NAA_LEN];
	u32			dev_attr_write_once_flag;
	u32			dev_qlbs;

	u32			se_dev_thread_cpumask;

	sector_t		prev_lba;
	u32			prev_len;
	struct kmem_cache       *fb_bio_rec_kmem;

	struct workqueue_struct *random_wq;
#ifdef QNAP_TARGET_PERF_FEATURE
	struct workqueue_struct *seqrd_wq;
#endif
	struct workqueue_struct *unmap_wq;
	struct workqueue_struct *sync_cache_wq;

	/* special for __qnap_wq_iblock_comp_cmd_flush_thin() */
	struct workqueue_struct *ib_comp_wq;

	u32			pool_blk_kb;	/* one dm data block (unit is KB) */
	atomic_t		hit_read_deletable;
	u32			fast_blk_clone;
	int			fbc_control;
	spinlock_t		fbc_control_lock;

#ifdef CONFIG_QTS_CEPH
	/* cluster api template */
	struct se_cluster_api	*cluster_api;
	void			*cluster_dev_data;
#endif
	struct workqueue_struct *odx_wut_wq;
	struct workqueue_struct *odx_rwio_wq;
	struct kmem_cache 	*zmc_sgl_node_cache;
	struct bio_set		fio_blk_bio_set;
};


/* embedded in struct se_dev_attrib */
struct qnap_se_dev_attr_dr {
	void        *gti;
	int                   emulate_v_sup;
	unsigned long long    lun_blocks;
	u32                   lun_index;

#ifdef SUPPORT_TP
	int                   tp_threshold_hit;
	int                   tp_threshold_enable;
	u32                   tp_threshold_percent;
	u64                   allocated;
#endif
};

#define CMD_FROM_NATIVE_POOL	1
#define CMD_FROM_CACHE		0

struct qnap_se_sess_dr {
	struct kmem_cache 	*lio_cmd_cache;
	atomic_t		cmd_count;
	atomic_t		sess_lun_count;
	bool			sess_got_report_lun_cmd;
};

struct qnap_tf_ops {
	struct qnap_odx_tf_ops	*odx_tf_ops;
};


#endif
#endif

