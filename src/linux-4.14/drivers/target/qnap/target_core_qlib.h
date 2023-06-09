#ifndef TARGET_CORE_QLIB_H
#define TARGET_CORE_QLIB_H


#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/device-mapper.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#ifdef __ISCSI_AUTOGEN__
#include <qnap/iscsi_autogen.h>
#endif


/**/

/* new pool blksz is 512 KB, but to use 1024 KB for legacy pool */
#define ALIGNED_SIZE_BYTES        (1 << 20)
#define MAX_SG_LISTS_ALLOC_SIZE		(4 << 20)

typedef enum{
	RC_GOOD				= 0	,
	RC_UNKNOWN_SAM_OPCODE 		= 1	,
	RC_REQ_TOO_MANY_SECTORS			,
	RC_INVALID_CDB_FIELD			,
	RC_INVALID_PARAMETER_LIST		,
	RC_LOGICAL_UNIT_COMMUNICATION_FAILURE	,
	RC_UNKNOWN_MODE_PAGE			,
	RC_WRITE_PROTECTEDS			,
	RC_RESERVATION_CONFLICT			,
	RC_CHECK_CONDITION_NOT_READY		,
	RC_CHECK_CONDITION_ABORTED_CMD		,
	RC_CHECK_CONDITION_UA			,
	RC_LBA_OUT_OF_RANGE			,
	RC_MISCOMPARE_DURING_VERIFY_OP		,
	RC_PARAMETER_LIST_LEN_ERROR		,
	RC_UNREACHABLE_COPY_TARGET		,
	RC_3RD_PARTY_DEVICE_FAILURE		,
	RC_INCORRECT_COPY_TARGET_DEV_TYPE	,
	RC_TOO_MANY_TARGET_DESCRIPTORS		,
	RC_TOO_MANY_SEGMENT_DESCRIPTORS		,
	RC_ILLEGAL_REQ_DATA_OVERRUN_COPY_TARGET	,
	RC_ILLEGAL_REQ_DATA_UNDERRUN_COPY_TARGET,
	RC_COPY_ABORT_DATA_OVERRUN_COPY_TARGET	,
	RC_COPY_ABORT_DATA_UNDERRUN_COPY_TARGET	,
	RC_INSUFFICIENT_RESOURCES		,
	RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD	,
	RC_INSUFFICIENT_RESOURCES_TO_CREATE_ROD_TOKEN	,
	RC_OPERATION_IN_PROGRESS			,
	RC_INVALID_TOKEN_OP_AND_INVALID_TOKEN_LEN	,
	RC_INVALID_TOKEN_OP_AND_CAUSE_NOT_REPORTABLE	,
	RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_CREATION_NOT_SUPPORTED	,
	RC_INVALID_TOKEN_OP_AND_REMOTE_ROD_TOKEN_USAGE_NOT_SUPPORTED	,
	RC_INVALID_TOKEN_OP_AND_TOKEN_CANCELLED		, /* 32 */
	RC_INVALID_TOKEN_OP_AND_TOKEN_CORRUPT		, /* 33 */
	RC_INVALID_TOKEN_OP_AND_TOKEN_DELETED		, /* 34 */
	RC_INVALID_TOKEN_OP_AND_TOKEN_EXPIRED		, /* 35 */
	RC_INVALID_TOKEN_OP_AND_TOKEN_REVOKED		, /* 36 */
	RC_INVALID_TOKEN_OP_AND_TOKEN_UNKNOWN		, /* 37 */
	RC_INVALID_TOKEN_OP_AND_UNSUPPORTED_TOKEN_TYPE	,
	RC_NO_SPACE_WRITE_PROTECT			,
	RC_OUT_OF_RESOURCES				,
	RC_THIN_PROVISIONING_SOFT_THRESHOLD_REACHED	,
	RC_CAPACITY_DATA_HAS_CHANGED			,
	RC_REPORTED_LUNS_DATA_HAS_CHANGED		,
	RC_NON_EXISTENT_LUN				,
	MAX_RC_VALUE					,
}RC;

struct ___fd {
	struct file		*fd_file;
};

struct ___bd {
	struct block_device	*bd;
	struct bio_set		*bio_set;
};

struct __fe_info {
	union {
		struct ___fd	__fd;
		struct ___bd	__bd;
	} __dev;

	/* refer QNAP_DT_xxx for struct qnap_se_dev_dr */
	u32	fe_type;
	bool	is_thin;
};

typedef enum{
	DEV_ATTR_SUPPORT_UNMAP			= 1 << 0 ,
	DEV_ATTR_SUPPORT_WRITE_SAME		= 1 << 1 ,
	/* return zero when execute unmap */
	DEV_ATTR_SUPPORT_READ_ZERO_UNMAP	= 1 << 2 ,
	DEV_ATTR_SUPPORT_WRITE_CACHE		= 1 << 3 ,
	DEV_ATTR_SUPPORT_FUA_WRITE		= 1 << 4 ,

#ifdef SUPPORT_TPC_CMD
#ifdef SUPPORT_FAST_BLOCK_CLONE
	/* check device from configfs attr supports it or not */
	DEV_ATTR_SUPPORT_DEV_FBC		= 1 << 30 ,
	/* fast block cloning must be supported by dm-thin layer */
	DEV_ATTR_SUPPORT_DM_FBC			= 1 << 31 ,
#endif
#endif
} DEV_ATTR;


#define NAA_LEN		16
struct __dev_info {
	struct __fe_info	fe_info;
	sector_t		dev_max_lba;

	/* refer DEV_ATTR_XXXX */
	u32			dev_attr;
	u32			bs_order;
	/* 16 bytes is for NAA IEEE Registered Extended DESIGNATOR field */
	u8			naa[NAA_LEN];

	/* relative initiator port id from vpd 0x83 */
	u32			initiator_rtpi;
	/* initiator protocol id */
	u32			initiator_prot_id;
	/* dev type for sbc */
	u32			sbc_dev_type;
} __attribute__ ((packed));

struct ____align_desc_blk_range {
	sector_t	lba;
	sector_t	nr_blks;
};

#define MAX_ALIGN_DESC_HT_BLK	2
struct ____align_desc_blk {
	/* [0]: head, [1]: tail */
	struct ____align_desc_blk_range	head_tail[MAX_ALIGN_DESC_HT_BLK];
	struct ____align_desc_blk_range	body;
	u32	bs_order;
	u32	bytes_to_align;
	bool	is_aligned;
};

struct __rw_task {
	/* dev info for device you want to read from or write to */
	struct __dev_info	dev_info;
	struct scatterlist	*sg_list;
	u32			sg_nents;
	u64			nr_bytes;  /* nr bytes to read or write */
	sector_t		lba;       /* lba to read or write */

	/* 1. (nr_blks << dev_bs_order) = sum of len for all sg elements 
	 * 2. the purpose of sg lists
	 * - for read: Usually, they are i/o buffer
	 * - for write:
	 * (a) Usually, they are buffer for normal write i/o
	 * (b) for special write discard, they are buffer for non-aligned /
	 * non-multipled write data i/o
	 */
//	u32			nr_blks;
//	u32			s_nr_blks;

	u32			bs_order;
//	u32			task_flag;
	enum dma_data_direction	dir;
	int			ret;
};

struct __cb_data {
	atomic_t		bio_count;
	atomic_t		bio_err_count;
	struct completion	*wait;
	int			nospc_err;
} __attribute__ ((packed));


struct __io_rec {
	struct	list_head	node;
//	void			*ib_dev;
	struct __cb_data	*cb_data;
	u32			nr_blks;
	bool			transfer_done;
} __attribute__ ((packed));


/**/
int qlib_create_iorec_cache(void);
void qlib_destroy_iorec_cache(void);
bool qlib_thin_lun(struct qnap_se_dev_dr *dev_dr);
int qlib_is_fio_blk_dev(struct qnap_se_dev_dr *dev_dr);
int qlib_is_ib_fbdisk_dev(struct qnap_se_dev_dr *dev_dr);
int qlib_get_naa_6h_code(void *se_dev, unsigned char *dev_naa_sign,
	unsigned char *buf, void (*lio_spc_parse_naa_6h)(void *, u8 *));

void qlib_make_naa_6h_hdr_old_style(unsigned char * buf);
void qlib_make_naa_6h_hdr_new_style(unsigned char * buf);
void qlib_make_naa_6h_body_old_style(unsigned char *wwn_sn_buf, unsigned char *out_buf);

void qlib_create_aligned_range_desc(struct ____align_desc_blk *desc,
	sector_t lba, sector_t nr_blks, u32 bs_order, u32 aligned_size);

struct scatterlist *qlib_alloc_sg_list(u32 *data_size, u32 *sg_nent);
void qlib_free_sg_list(struct scatterlist *sg_list, u32 sg_nent);
int qlib_fileio_rw(struct __rw_task *task);
int qlib_blockio_rw(struct __rw_task *task);

int qlib_fd_sync_cache_range(struct file *file, loff_t start_byte, loff_t end_byte);
int qlib_fd_flush_and_truncate_cache(struct file *fd, sector_t lba, u32 nr_blks,
	u32 bs_order, bool truncate_cache, bool is_thin);

/* unit for block is 512bytes here */
extern int  __attribute__((weak)) blkdev_issue_special_discard(
	struct block_device *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp_mask);

bool qlib_check_support_special_discard(void);
int qlib_blkdev_issue_special_discard(struct block_device *bdev, sector_t sector,
	sector_t nr_sects, gfp_t gfp_mask, unsigned long flags);

void qlib_init_cb_data(struct __cb_data *data, void *p);

/* api provided by dm-thin layer (for thin provisioning)
 * here to use weak symbol to avoid nobody prepare them but we still need insert module
 */
extern int __attribute__((weak)) thin_get_dmtarget(char *name, struct dm_target **result);
extern int __attribute__((weak)) thin_get_data_status(struct dm_target *ti, 
	uint64_t *total_size, uint64_t *used_size);

extern int __attribute__((weak)) thin_set_dm_monitor(struct dm_target *ti,
	void *dev, void (*dm_monitor_fn)(void *dev, int));

extern int __attribute__((weak)) thin_get_sectors_per_block(char *name, uint32_t *result);
extern int __attribute__((weak)) thin_get_lba_status(char *name, uint64_t index, 
	uint64_t len, uint8_t *result);

#ifdef QNAP_HAL
#include <qnap/hal_event.h>
extern int __attribute__((weak)) send_hal_netlink(NETLINK_EVT *event);
#endif

/* qlib_fd_check_dm_thin_cond()
 *
 * 0: normal i/o (not hit sync i/o threshold)
 * 1: hit sync i/o threshold
 * - ENOSPC: pool space is full
 * - ENODEV: no such device
 * - others: other failure
 *
 * this call shal cowork with dm-thin layer
 */
extern int __attribute__((weak)) dm_thin_volume_is_full(void *data);

static inline void *qlib_rq_get_thin_hook(struct request_queue *q)
{
#ifdef __HAVE_RQ_GET_THIN_HOOK__
	return rq_get_thin_hook(q);
#else
#warning "[iSCSI Porting]: need to implement in blkdev.h"
	return NULL;
#endif
}

int qlib_fd_check_dm_thin_cond(struct file *file);
int qlib_vfs_fsync_range(struct file *file, loff_t s, loff_t e, int data_sync, int is_thin);

/* qlib_drop_bb_cmd_xxx() is for i/o dropping feature code for block based lun */
void qlib_drop_bb_init_rec_val(struct __iov_obj *obj);
void qlib_drop_bb_prepare_rec(struct qnap_se_dev_dr *dev_dr, 
	struct __iov_obj *obj, void *iter, u32 len);

void qlib_drop_bb_set_rec_null(struct qnap_se_dev_dr *dev_dr, struct __iov_obj *obj);
void qlib_drop_bb_set_drop_val(struct qnap_se_dev_dr *dev_dr, struct __iov_obj *obj, bool val);
bool qlib_drop_bb_get_drop_val(struct qnap_se_dev_dr *dev_dr, struct __iov_obj *obj);
int qlib_drop_bb_ask_drop(struct qnap_se_dev_dr *dev_dr, struct __iov_obj *obj, char *msg);

/* qlib_drop_fb_xxx() is for i/o dropping feature code for file based lun */
int qlib_drop_fb_alloc_bio_rec(struct qnap_se_dev_dr *dev_dr, struct __bio_obj *obj,
	void *misc, struct bio *bio);

void qlib_drop_fb_init_bio_rec_val(struct __bio_obj *obj);
void qlib_drop_fb_destroy_fb_bio_rec_kmem(struct qnap_se_dev_dr *dev_dr);
void qlib_drop_fb_set_bio_rec_null(struct qnap_se_dev_dr *dev_dr, 
	struct __bio_obj *obj, struct bio *bio);

void qlib_drop_fb_create_fb_bio_rec_kmem(struct qnap_se_dev_dr *dev_dr,	int dev_index);
int qlib_drop_fb_free_bio_rec_lists(struct qnap_se_dev_dr *dev_dr, struct __bio_obj *obj);
void qlib_se_cmd_dr_init(struct qnap_se_cmd_dr *cmd_dr);

int qlib_ib_fbdisk_get_thin_total_used_blks(struct block_device *bd,
	loff_t *total_bytes, loff_t *used_blks);

int qlib_fio_blkdev_get_thin_total_used_blks(struct qnap_se_dev_attr_dr *dev_attr_dr,
	char *lv_name, u64 *total_blks, u64 *used_blks);

void qlib_check_exported_func(void);
bool qlib_check_zero_buf(u8 *buf, u32 buf_len);

#endif

