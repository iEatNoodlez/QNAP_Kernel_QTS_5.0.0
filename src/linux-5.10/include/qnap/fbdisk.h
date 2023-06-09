/**
 * fbdisk-2.0/fbdisk.h
 *
 * Copyright (c) 2009 QNAP SYSTEMS, INC.
 * All Rights Reserved.
 *
 * @file	fbdisk.h
 * @brief	file backed disk declaration file.
 *
 * @author	Nike Chen
 * @date	2009/08/01
 */

#ifndef _FILE_BACKED_DISK_HDR
#define _FILE_BACKED_DISK_HDR

#ifndef IN
#define IN
#endif

#ifndef OUT
#define OUT
#endif

#define SUPPORT_FBDISK_MQ
#define	SUPPORT_FBSNAP_DEVICE
/* #define 	ENABLE_FBSNAP_STATISTIC */
/* Enable the fbsnap statistic. (For test only.) */

#define MAX_FBDISK_NUM		256
/* MAX_FBDISK_NUM should be the same as
 * the 'max_device' in NasUtil/fbutil/fbdisk.sh
 */

#define MAX_FBDISK_NAME		64
#ifdef QNAP_HAL_SUPPORT
#define MAX_FILE_NUM		512
#else
#define MAX_FILE_NUM		64
#endif /* #ifdef QNAP_HAL_SUPPORT */

#define MAX_FILE_NAME		64
#define LUN_NOT_MAPPING (-1)
#define	CBMAX_FILE_PREFIX	256
/* The maximum size of snap-file prefix in byte.*/

#define SECTOR_SIZE 512
#define SECTOR_SHIFT 9
#define SECTOR_MASK (SECTOR_SIZE - 1)

enum fb_type_num {
	FB_TYPE_DISK,
#ifdef SUPPORT_FBSNAP_DEVICE
	FB_TYPE_SNAP,
#endif
	MAX_FB_TYPE
};

/* Request command ID to enable/disable a snapshot. */
enum IDCMD_FBSNAP {
	IDCMD_FBSNAP_ENABLE = 1,
	IDCMD_FBSNAP_DISABLE = 2,
};

/* Status of a snapshot device */
enum IDSTAT_FBSNAP {
	IDSTAT_FBSNAP_READY = 0,		/* (0) Ready */
	IDSTAT_FBSNAP_IO_ERROR = -EIO, /* (5) I/O error */
	IDSTAT_FBSNAP_NO_SPACE		= -ENOSPC,  /* (28) no space */
	IDSTAT_FBSNAP_NO_MEMORY		= -ENOMEM,  /* (12) no memory */
	IDSTAT_FBSNAP_NOT_READY		= -ENODATA, /* (61) not enabled */
	IDSTAT_FBSNAP_DEV_NOT_READY	= -ENOTBLK,	/* (15) not ready */
};

typedef struct __attribute__((packed)) {
	u64	file_length;	/* !reserved field now */
	int file_desc;
} fbdisk_file_info;

typedef struct __attribute__((packed)) {
	/** fbdisk commnad options flag. */
	/** bits in the flags field*/
	enum {
		ADD_DEVICE_NAME = 0x0001,
		ADD_FILE_NAME = 0x0002,
		ADD_LUN_INDEX = 0x0004,
	} flags;
	char dev_name[MAX_FBDISK_NAME];
	int file_count;
	fbdisk_file_info file_info[MAX_FILE_NUM];
	int bSnap;
	int lun_index;
} fbdisk_info;

/* Benjamin 20110218 for getting the lun-fbdisk mapping */
typedef struct __attribute__((packed)) {
	int status;
	int lun_index;
} fbdisk_status;

/* The data structure for the snapshot file path. */
typedef struct __attribute__((packed)) {
	/* The command ID. (Check IDCMD_FBSNAP_XXX) */
	int32_t		idCommand;
	/* The block size in 2 exponentation. (def:12, 12 ~ 20) */
	int32_t		btBlock;
	/* The submap size in 2 exponentation. (def:10, 9 ~ 12) */
	int32_t		btSubmap;
	/* The total number of the submap. (def:16, 4 ~ 64) */
	int32_t		cnSubmap;
	/* The path prefix of the snapshot files. */
	char		szfPrefix[CBMAX_FILE_PREFIX];
} fbsnap_info;

/* The data structure for the snapshot status. */
typedef struct __attribute__((packed)) {
	/* The snapshot status. (Check IDSTAT_FBSNAP_XXX) */
	int32_t		idStatus;
	/* The total number of device files. */
	uint32_t	cnFiles;
	/* The total size of this device files in megabyte. */
	uint32_t	mbFiles;
	/* The size of every device file in megabyte. */
	uint32_t	rymbFile[MAX_FILE_NUM];
} fbsnap_status;

/* The data structure for the snapshot statistic. */
typedef struct __attribute__((packed)) {
	/* < query count bit map. (By set/get APIs) */
	unsigned long long	cnQuery;
	/* < switching count from one submap to another. */
	unsigned long long	cnQueryCache;
	/* < count of a new submap loaded from disks. */
	unsigned long long	cnSwap;
	/* < flush count of a swapped-out submap to disks. */
	unsigned long long	cnFlush;
} fbsnap_statistic;

#define FBDISK_IOC_MAGIC 'F'
#define FBDISK_IOCSETFD		_IOW(FBDISK_IOC_MAGIC, 1, fbdisk_info)
#define FBDISK_IOCCLRFD		_IO(FBDISK_IOC_MAGIC, 2)
#define FBDISK_IOCGETSTATUS	_IOR(FBDISK_IOC_MAGIC, 3, fbdisk_status)
#define FBDISK_IOCSETSTATUS	_IOW(FBDISK_IOC_MAGIC, 4, fbdisk_status)
#define FBDISK_IOCADDFD		_IOW(FBDISK_IOC_MAGIC, 5, fbdisk_info)
#define	FBDISK_IOC_SETSNAP	_IOW(FBDISK_IOC_MAGIC, 6, fbsnap_info)
#define	FBDISK_IOC_GETSNAP	_IOR(FBDISK_IOC_MAGIC, 7, fbsnap_status)
#define	FBDISK_IOC_SNAP_STATISTIC _IOR(FBDISK_IOC_MAGIC, 8, fbsnap_statistic)
#define FBDISK_IOC_REFRESH	_IO(FBDISK_IOC_MAGIC, 9)

#ifdef __KERNEL__
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/blk-mq.h>
#include <linux/kthread.h>

#define TRACE_ERROR		0x0001
#define TRACE_WARNING		0x0002
#define TRACE_ENTRY		0x0004
#define TRACE_REQ		0x0008
#define TRACE_ALL		0xFFFF

/* States of fbdisk device */
enum {
	Fb_unbound,
	Fb_bound,
	Fb_rundown,
};

/*
 * fbdisk flags
 */
enum {
	FB_FLAGS_READ_ONLY = 1,
	FB_FLAGS_USE_AOPS = 2,
	FB_FLAGS_AUTOCLEAR  = 4,
	FB_FLAGS_PARTSCAN   = 8,
};

struct fbdisk_file {
	sector_t fb_start_byte;
	sector_t fb_end_byte;
	struct file *fb_backing_file;
	unsigned fb_blocksize;
	loff_t fb_file_size;	/* file size in sector (512 byte) */
	gfp_t old_gfp_mask;
	struct file *fb_snap_file;
	gfp_t old_gfp_snap;
};

struct fbdisk_device {
	int		fb_number;
	atomic_t	fb_refcnt;
	loff_t		fb_sizelimit;
	int		fb_flags;
	int		(*ioctl)(struct fbdisk_device *, int cmd,
			unsigned long arg);
	struct block_device *fb_device;
	spinlock_t		fb_lock;
	struct bio_list		fb_bio_list;
	int			fb_state;
	struct mutex		fb_ctl_mutex;
	struct task_struct	*worker_task;
	struct kthread_worker   worker;
	bool use_dio;
	wait_queue_head_t	fb_event;
	struct request_queue *fb_queue[MAX_FB_TYPE];
	struct blk_mq_tag_set tag_set[MAX_FB_TYPE];
	struct gendisk   *fb_disk[MAX_FB_TYPE];
	struct list_head fb_list;
	int  fb_file_num;
	struct fbdisk_file   fb_backing_files_ary[MAX_FILE_NUM];
	int  fb_lun_index[MAX_FB_TYPE];
#ifdef	SUPPORT_FBSNAP_DEVICE
	/* The snapshot status. (Check IDSTAT_FBSNAP_XXX) */
	int idStatusSnap;
	/* Spin lock to protect idStatusSnap*/
	spinlock_t fbsnap_lock;
	/* The block size in 2 exponentation. (def:12, 12 ~ 20) */
	int btBlock;
	/* The block size in byte. */
	int cbBlock;
	/* The submap size in 2 exponentation. (def:10, 9 ~ 12) */
	int btSubmap;
	/* total number of the submap (def:16, 4 ~ 64) */
	int cnSubmap;
	/* The size of bitmap file in byte. */
	loff_t  cbBmpFile;
	/* snapshot device capacity (in sector) */
	loff_t  csCapSnap;
	/* The bio list of the snapshot device. */
	struct bio_list fb_sbio_list;
	/* The snapshot bitmap file object */
	struct file *ptfSnapBmp;
	/* The snapshot bitmap object. */
	void *ptBitmap;
#endif

#define FB_BIO_DROPPED_RET_MAGIC_NUM	0x12345678

};

struct fbdisk_cmd {
        struct kthread_work work;
        bool use_aio; /* use AIO interface to handle I/O */
        atomic_t ref; /* only for aio */
        long ret;
        struct kiocb iocb;
        struct bio_vec *bvec;
};

#endif /* __KERNEL__ */

#endif /* _FILE_BACKED_DISK_HDR */

