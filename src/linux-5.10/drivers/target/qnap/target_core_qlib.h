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
#define DM_BLOCK_SIZE_BYTES        (4 << 20)

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
	struct ___fd	__fd;
	struct ___bd	__bd;

	/* refer QNAP_DT_xxx for struct qnap_se_dev_dr */
	u32	fe_type;
	bool	is_thin;
};

struct __dev_attr {
	/* byte 0 */
	u8	supported_unmap:1;
	u8	supported_write_same:1;
	u8	supported_unmap_read_zero:1;
	u8	supported_write_cache:1;
	u8	supported_fua_write:1;
	u8	reserved0:3;
	/* byte 1 */
	u8	reserved1;
	/* byte 2 */
	u8	supported_dev_fbc:1;
	u8	supported_dm_fbc:1;
	u8	reserved2:6;
	/* byte 3 */
	u8	reserved3;
};

#define NAA_LEN		16
struct __dev_info {
	struct __fe_info	fe_info;
	struct __dev_attr	dev_attr;
	sector_t		dev_max_lba;
	u8			bs_order;
	/* dev type for sbc */
	u8			sbc_dev_type;	
	/* 16 bytes is for NAA IEEE Registered Extended DESIGNATOR field */
	u8			naa[NAA_LEN];
	/* relative initiator port id from vpd 0x83 */
	u16			initiator_rtpi;
	/* initiator protocol id */
	u32			initiator_prot_id;

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

#define RW_TASK_TYPE_FUA       0x00000001

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
	/* refer RW_TASK_TYPE_XXXX */
	u32			task_flag;
	enum dma_data_direction	dir;
	int			ret;

	/* fow now, priv and priv_cb_complete are used to do async i/o 
	 * (async_io set to true) when to call qlib_blockio_rw() 
	 */
	void			*priv;
	bool			async_io;
	void (*priv_cb_complete)(void *priv, int err, int done_blks);
};

#define MAX_BIO_SGL_REC_COUNTS	16

struct sg_info {
	struct scatterlist *sg;
	u32 sg_len;
	u32 sg_off;

	/* if sg_old_off != sg_off, means last sg not be used completely */
	u32 sg_old_off;

	/* if 0, no any sg can be used */
	u32 sg_nents;
};

struct bio_sgl_rec {
	struct scatterlist *sgl;
	u32 sg_nents;
	u32 total_len;
};


/**/
int qlib_create_iorec_cache(void);
void qlib_destroy_iorec_cache(void);
int qlib_bd_is_fbdisk_dev(struct block_device *bd);
bool qlib_thin_lun(struct qnap_se_dev_dr *dev_dr);
int qlib_is_fio_blk_dev(struct qnap_se_dev_dr *dev_dr);
int qlib_is_ib_fbdisk_dev(struct qnap_se_dev_dr *dev_dr);
int qlib_is_fio_file_dev(struct qnap_se_dev_dr *dev_dr);
int qlib_is_ib_blk_dev(struct qnap_se_dev_dr *dev_dr);
char *qlib_get_dev_type_str(struct qnap_se_dev_dr *dev_dr);

int qlib_get_naa_6h_code(void *se_dev, unsigned char *dev_naa_sign,
	unsigned char *buf, void (*lio_spc_parse_naa_6h)(void *, u8 *));

void qlib_make_naa_6h_hdr_old_style(unsigned char * buf);
void qlib_make_naa_6h_hdr_new_style(unsigned char * buf);
void qlib_make_naa_6h_body_old_style(unsigned char *wwn_sn_buf, unsigned char *out_buf);

static inline int qlib_ib_get_min_nr_vecs(struct request_queue *q)
{
	/* please refer bio_add_page() (it's different from kernel 4.2), 
	 * we indicate max vecs for one bio to BIO_MAX_PAGES here
	 */
	return BIO_MAX_PAGES;
}

void qlib_ib_setup_bio_sgl_rec(struct block_device *bd,
	struct bio_sgl_rec *sgl_rec, struct sg_info *_sg_info, u64 work_bytes);

void qlib_ib_free_bio_sgl_rec(struct bio_sgl_rec **p);
struct bio_sgl_rec *qlib_ib_alloc_bio_sgl_rec(int nr_vecs);


/* qlib_create_aligned_range_desc() was used to create aligned range descs
 * by incoming data
 *
 * example:
 *
 * <ORIG>
 * 0x1234                        0x12345
 * +-----------------------------+
 * |                             |
 * |                             |
 * +-----------------------------+ 
 *
 * <NEW>
 * 0x1234   0x8000   0x10000     0x12345
 * +---------+----------+--------+
 * |         |          |        |
 * |  HEAD   |  BODY    |  TAIL  |
 * +---------+----------+--------+ 
 *
 */
void qlib_create_aligned_range_desc(struct ____align_desc_blk *desc,
	sector_t lba, sector_t nr_blks, u32 bs_order, u32 aligned_size);

struct scatterlist *qlib_alloc_sg_list(u32 *data_size, u32 *sg_nent);
void qlib_free_sg_list(struct scatterlist *sg_list, u32 sg_nent);
int qlib_fileio_rw(struct __rw_task *task);
int qlib_blockio_rw(struct __rw_task *task);
int qlib_fd_sync_cache_range(struct file *file, loff_t start_byte, loff_t end_byte);
int qlib_fd_flush_and_truncate_cache(struct file *fd, sector_t lba, u32 nr_blks,
	u32 bs_order, bool truncate_cache, bool is_thin);

bool qlib_check_support_special_discard(void);

/**
 * @brief function to perform the special discard for block layer
 *
 * This function is similar to blkdev_issue_discard(), the diffrence is it
 * will NOT discard the block but to do fast-zero behavior. This function MUST
 * be supported by block layer (dm-thin layer)
 */
extern int  __attribute__((weak)) blkdev_issue_special_discard(
	struct block_device *bdev, sector_t sector, sector_t nr_sects, gfp_t gfp_mask);

/* the following APIs are provided by dm-thin layer (for thin provisioning)
 * here to use weak symbol to avoid nobody prepare them but we still need insert module
 */

/**
 * @brief try get the dm target instance by device name
 *
 * @param[in] dev_name Device name which format is vg<?>-lv<???>
 * @param[out] dti DM target instance for the device (dev_name)
 *
 * @retval 0 success and dti be updated
 * @retval Other faliure
 */
extern int __attribute__((weak)) thin_get_dmtarget(char *dev_name, 
	struct dm_target **dti);

/**
 * @brief try get total blocks and used blocks for dm target instance
 *
 * @param[in] dti DM target instance, we can get this by thin_get_dmtarget()
 * @param[out] total_blks How many total blks (512b) for the dm target instance
 * @param[out] used_blks How many used blks (512b) for the dm target instance
 *
 * @retval 0 success and total_blks, used_blks will be updated
 * @retval Other faliure
 */
extern int __attribute__((weak)) thin_get_data_status(struct dm_target *dti, 
	uint64_t *total_blks, uint64_t *used_blks);

/**
 * @brief try set monitor to the dm target instacne
 *
 * @param[in] dti DM target instance, we can get this by thin_get_dmtarget()
 * @param[in] dev
 * @param[in] dm_monitor_fn DM monitor callback function, it will be called by
 * dm thin layer (thin_dtr() function) and pass the dev variable set by dev argument
 * and dmstate to the callback
 *
 * @retval 0 success
 * @retval Other faliure
 */
extern int __attribute__((weak)) thin_set_dm_monitor(struct dm_target *dti,
	void *dev, void (*dm_monitor_fn)(void *dev, int dmstate));

/**
 * @brief get how many blocks (512b) per one dm thin data block
 *
 * @param[in] name device name which format is vg<?>-lv<???>
 * @param[out] sectors_per_dm_blk How many sectors (512b) per one dm block size
 *
 * @retval 0 success and sectors_per_dm_blk will be updated
 * @retval Other faliure
 */
extern int __attribute__((weak)) thin_get_sectors_per_block(char *dev_name, 
		uint32_t *sectors_per_dm_blk);

/**
 * @brief Try get the lba status value from dm block (indicated by index) to the
 * range(s) we want to get. 
 *
 * @param[in] name device name which format is vg<?>-lv<???>
 * @param[in] index DM block index calculated by the following fomula
 * - div_u64((u64)START_LBA, sectors_per_dm_blk);
 *
 * @param[in] len Length to be queried from the dm block index. For example, if
 * we have the array - status[10], the len is sizeof(status[10])
 *
 * @param[out] status LBA status is mappeed or deallocated
 *
 * @retval 0 success and status will be updated to 0 (mapped) or 1 (deallocated)
 * @retval Other faliure
 */
extern int __attribute__((weak)) thin_get_lba_status(char *dev_name, 
	uint64_t index, uint64_t len, uint8_t *status);

/**
 * @brief To check dm thin volume is full or not. If it was full, any i/o from 
 * iSCSI layer shall return the no-space sense code
 *
 * @param[in] data Instance for q->thindata from request queue struct, we can get
 * this from qlib_rq_get_thin_hook()
 *
 * @retval 0 success (normal i/o)
 * @retval 1 success (hit sync i/o threshold)
 * @retval -ENOSPC hit case that pool space is full
 * @retval Other faliure
 */
extern int __attribute__((weak)) dm_thin_volume_is_full(void *data);

/**
 * @brief Wrapper function to get thindata member from q for request_queue struct
 */
static inline void *qlib_rq_get_thin_hook(struct request_queue *q)
{
#ifdef __HAVE_RQ_GET_THIN_HOOK__
	return rq_get_thin_hook(q);
#else
#warning "[iSCSI Porting]: need to implement in blkdev.h"
	return NULL;
#endif
}

#ifdef QNAP_HAL
#include <qnap/hal_event.h>
/**
 * @brief to send hal event to hal daemon
 */
extern int __attribute__((weak)) send_hal_netlink(NETLINK_EVT *event);
#endif

int qlib_fd_check_dm_thin_cond(struct file *file);

void qlib_se_cmd_dr_init(struct qnap_se_cmd_dr *cmd_dr);

int qlib_ib_fbdisk_get_thin_total_used_blks(struct block_device *bd,
	loff_t *total_bytes, loff_t *used_blks);

int qlib_fio_blkdev_get_thin_total_used_blks(struct qnap_se_dev_attr_dr *dev_attr_dr,
	char *lv_name, u64 *total_blks, u64 *used_blks);

void qlib_check_exported_func(void);
bool qlib_check_exported_fbc_func(void);
bool qlib_check_zero_buf(u8 *buf, u32 buf_len);

#endif

