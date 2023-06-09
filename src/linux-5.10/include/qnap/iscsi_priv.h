/*  
 */
#ifndef __ISCSI_PRIVATE_H__
#define __ISCSI_PRIVATE_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/uio.h>

struct bio_rec {
	struct          list_head node;
	struct          bio *bio;
	void            *misc;
	unsigned long   flags;
};

/* ITER_QNAP_ISCSI_IO_DROP_BIT is 31th bit and it MUST matches for type member
 * in struct iov_iter in uio.h, we MUST take care this
 * 
 * include/linux/uio.h
 *
 * enum {
 *	ITER_IOVEC = 0,
 *	ITER_KVEC = 2,
 *	ITER_BVEC = 4,
 *	ITER_PIPE = 8,
 *      ITER_QNAP_ISCSI_IO_DROP = (1 << ITER_QNAP_ISCSI_IO_DROP_BIT),
 * }; 
 *
 * struct iov_iter {
 *	int type;           
 *	size_t iov_offset;
 *	size_t count;
 *
 *	// snipped ... //
 * }	
 */
#define ITER_QNAP_ISCSI_IO_DROP_BIT	31

/* 
 * following code qnap_iscsi_iov_xxxx is used to drop i/o issued from 
 * iscsi to block based lun via file i/o
 */
static inline void qnap_iscsi_iov_set_drop(struct iov_iter *iter)
{
	/* TODO: need to find better way ...
	 * 
	 * dangerous code here due to kernel need (unsigned long) type but
	 * we pass int type. however, we restrict the pos to bit 31 
	 * (range is from 0 ~ 31, not over bit 31, it shall be ok)
	 */
	set_bit(ITER_QNAP_ISCSI_IO_DROP_BIT, (unsigned long *)&iter->type);
}

static inline bool qnap_iscsi_iov_is_dropped(struct iov_iter *iter)
{
	/* TODO: need to find better way ...
	 * 
	 * dangerous code here due to kernel need (unsigned long) type but
	 * we pass int type. however, we restrict the pos to bit 31 
	 * (range is from 0 ~ 31, not over bit 31, it shall be ok)
	 *
	 * not clear this bit due to it will be checked in fd_do_rw() again
	 */
	return test_bit(ITER_QNAP_ISCSI_IO_DROP_BIT, (unsigned long *)&iter->type);
}

//
/* ITER_QNAP_ISCSI_IO_DROP_BIT is 31th bit and it MUST matches for type member
 * in struct iov_iter in uio.h, we MUST take care this
 * 
 * include/linux/uio.h
 *
 * enum iter_type {
 *	 ITER_IOVEC = 4,
 *	 ITER_KVEC = 8,
 *	 ITER_BVEC = 16,
 *	 ITER_PIPE = 32,
 *	 ITER_DISCARD = 64,
 *       ITER_QNAP_ISCSI_IO_DRAIN = (1 << ITER_QNAP_ISCSI_IO_DRAIN_BIT), 
 * };
 *
 */
#define ITER_QNAP_ISCSI_IO_DRAIN_BIT	31

/* 
 * following code qnap_iscsi_iov_xxxx is used to drop i/o issued from 
 * iscsi to block based lun via file i/o
 */
static inline void qnap_iscsi_iov_set_drain(struct iov_iter *iter)
{
	/* TODO: need to find better way ...
	 * 
	 * dangerous code here due to kernel need (unsigned long) type but
	 * we pass int type. however, we restrict the pos to bit 31 
	 * (range is from 0 ~ 31, not over bit 31, it shall be ok)
	 */
	set_bit(ITER_QNAP_ISCSI_IO_DRAIN_BIT, (unsigned long *)&iter->type);
}

static inline bool qnap_iscsi_iov_is_drained(struct iov_iter *iter)
{
	/* TODO: need to find better way ...
	 * 
	 * dangerous code here due to kernel need (unsigned long) type but
	 * we pass int type. however, we restrict the pos to bit 31 
	 * (range is from 0 ~ 31, not over bit 31, it shall be ok)
	 *
	 * not clear this bit due to it will be checked in fd_do_rw() again
	 */
	return test_bit(ITER_QNAP_ISCSI_IO_DRAIN_BIT, (unsigned long *)&iter->type);
}

/* tricky method like PageAnon(struct page *page), take care this */
#define BI_PRIVATE_BREC		(1)

static inline int qnap_bi_private_is_brec(
	void *data
	)
{
	return (((unsigned long)data & BI_PRIVATE_BREC) != 0);
}

static inline void *qnap_bi_private_set_brec_bit(
	void *data
	) 
{
	unsigned long tmp = (unsigned long)data;
	return (void *)(tmp + BI_PRIVATE_BREC);
}

static inline void *qnap_bi_private_clear_brec_bit(
	void *data
	) 
{
	unsigned long tmp = (unsigned long)data;
	return (void *)(tmp - BI_PRIVATE_BREC);
}

/* 
 * following code qnap_iscsi_fbdisk_xxxx is used to drop i/o issued from
 * iscsi to fbdisk vio block bio
 */
#define BIO_QNAP_ISCSI_IO_DRAIN_BIT	0

static inline void qnap_iscsi_fbdisk_set_bio_drain(
	struct bio_rec *brec
	) 
{
	set_bit(BIO_QNAP_ISCSI_IO_DRAIN_BIT, &brec->flags);
}

/* qnap_iscsi_fbdisk_bio_is_drained() can be called in fbdisk driver */
static inline bool qnap_iscsi_fbdisk_bio_is_drained(
	void *bi_private
	) 
{
	struct bio_rec *brec = NULL;
	bool ret = false;

	if (qnap_bi_private_is_brec(bi_private)) {
		brec = qnap_bi_private_clear_brec_bit(bi_private);
		ret = test_and_clear_bit(BIO_QNAP_ISCSI_IO_DRAIN_BIT, &brec->flags);
	}

	return ret;
}

#endif

