#ifndef TARGET_CORE_QFBC_H
#define TARGET_CORE_QFBC_H

#include <linux/fast_clone.h>

#define MAX_FBC_IO      	(64*1024*1024)	/* size per one FBC i/o */
#define FAST_CLONE_TIMEOUT_SEC  4


typedef struct __fast_clone_io_cb_data{
	atomic_t io_done;
	atomic_t io_err_count;
	struct completion *io_wait;

	/* used on thin provisioning case */
	int nospc_err;
	int no_srcdata_mapping;
} FCCB_DATA;

typedef struct __create_record{
	struct block_device *s_blkdev;
	struct block_device *d_blkdev;
	sector_t s_lba;
	sector_t d_lba;
	u64 transfer_bytes;
} CREATE_REC;

typedef struct __fast_clone_io_rec{
	struct list_head io_node;
	THIN_BLOCKCLONE_DESC desc;

	/* 0: init value , 1: io done w/o error , -1: io done with any error */
	int io_done;
} FCIO_REC;

typedef struct _fast_clone_obj{
	struct se_device	*s_se_dev;
	struct se_device	*d_se_dev;
	sector_t	s_lba;
	sector_t	d_lba;
	u32		s_dev_bs;
	u32		d_dev_bs;
	u64		data_bytes;
	int		nospc_err;
	int		no_srcdata_mapping;
} FC_OBJ;

typedef struct __tbc_desc_data{
	THIN_BLOCKCLONE_DESC tbc_desc;

	/* the unit for lba data bytes listed below is original unit */
	sector_t	h_d_lba;	/* head */
	sector_t	t_d_lba;	/* tail */

	/* align area */
	sector_t	d_align_lba;
	u64		data_bytes;	/* shall be multiple by pool block size */
} TBC_DESC_DATA;

#warning "TODO: need to implement in dm layer code"

/* Fast Block Cloning feature code was provided by dm module, 
 * so to declare it as weak symbol
 */
extern int __attribute__((weak)) thin_support_block_cloning(
	THIN_BLOCKCLONE_DESC *clone_desc, unsigned long *block_size);

extern int __attribute__((weak)) thin_do_block_cloning(
	THIN_BLOCKCLONE_DESC *clone_desc, CLONE_CALLBACK callback);

extern int __attribute__((weak)) qnap_fbc_do_xcopy(struct se_device *src,
	struct se_device *dst, sector_t src_lba, sector_t dst_lba, u64 *copy_bytes);

extern int __attribute__((weak)) qnap_fbc_do_odx(struct se_device *src, 
	struct se_device *dst, sector_t src_lba, sector_t dst_lba, u64 *copy_bytes);


#endif
