#ifndef __QNAP_NVMET_PCIE_H
#define __QNAP_NVMET_PCIE_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <asm/barrier.h>
#include "al_hal_pcie.h"
#include "nvmet.h"

typedef struct NvmeBar {
    uint64_t    cap;
    uint32_t    vs;
    uint32_t    intms;
    uint32_t    intmc;
    uint32_t    cc;
    uint32_t    rsvd1;
    uint32_t    csts;
    uint32_t    nssrc;
    uint32_t    aqa;
    uint64_t    asq;
    uint64_t    acq;
    uint32_t    cmbloc;
    uint32_t    cmbsz;
} NvmeBar;

enum NvmeCapShift {
    CAP_MQES_SHIFT     = 0,
    CAP_CQR_SHIFT      = 16,
    CAP_AMS_SHIFT      = 17,
    CAP_TO_SHIFT       = 24,
    CAP_DSTRD_SHIFT    = 32,
    CAP_NSSRS_SHIFT    = 36,
    CAP_CSS_SHIFT      = 37,
    CAP_MPSMIN_SHIFT   = 48,
    CAP_MPSMAX_SHIFT   = 52,
};

enum NvmeCapMask {
    CAP_MQES_MASK      = 0xffff,
    CAP_CQR_MASK       = 0x1,
    CAP_AMS_MASK       = 0x3,
    CAP_TO_MASK        = 0xff,
    CAP_DSTRD_MASK     = 0xf,
    CAP_NSSRS_MASK     = 0x1,
    CAP_CSS_MASK       = 0xff,
    CAP_MPSMIN_MASK    = 0xf,
    CAP_MPSMAX_MASK    = 0xf,
};

/*******************************************************************************/
#define NVME_REG_DBS 0x1000

/*******************************************************************************/
#define QNAP_NVME_CAP_MQES(cap)  (((cap) >> CAP_MQES_SHIFT)   & CAP_MQES_MASK)
#define QNAP_NVME_CAP_CQR(cap)   (((cap) >> CAP_CQR_SHIFT)    & CAP_CQR_MASK)
#define QNAP_NVME_CAP_AMS(cap)   (((cap) >> CAP_AMS_SHIFT)    & CAP_AMS_MASK)
#define QNAP_NVME_CAP_TO(cap)    (((cap) >> CAP_TO_SHIFT)     & CAP_TO_MASK)
#define QNAP_NVME_CAP_DSTRD(cap) (((cap) >> CAP_DSTRD_SHIFT)  & CAP_DSTRD_MASK)
#define QNAP_NVME_CAP_NSSRS(cap) (((cap) >> CAP_NSSRS_SHIFT)  & CAP_NSSRS_MASK)
#define QNAP_NVME_CAP_CSS(cap)   (((cap) >> CAP_CSS_SHIFT)    & CAP_CSS_MASK)
#define QNAP_NVME_CAP_MPSMIN(cap)(((cap) >> CAP_MPSMIN_SHIFT) & CAP_MPSMIN_MASK)
#define QNAP_NVME_CAP_MPSMAX(cap)(((cap) >> CAP_MPSMAX_SHIFT) & CAP_MPSMAX_MASK)

#define QNAP_NVME_CAP_SET_MQES(cap, val)   (cap |= (uint64_t)(val & CAP_MQES_MASK)  \
                                                           << CAP_MQES_SHIFT)
#define QNAP_NVME_CAP_SET_CQR(cap, val)    (cap |= (uint64_t)(val & CAP_CQR_MASK)   \
                                                           << CAP_CQR_SHIFT)
#define QNAP_NVME_CAP_SET_AMS(cap, val)    (cap |= (uint64_t)(val & CAP_AMS_MASK)   \
                                                           << CAP_AMS_SHIFT)
#define QNAP_NVME_CAP_SET_TO(cap, val)     (cap |= (uint64_t)(val & CAP_TO_MASK)    \
                                                           << CAP_TO_SHIFT)
#define QNAP_NVME_CAP_SET_DSTRD(cap, val)  (cap |= (uint64_t)(val & CAP_DSTRD_MASK) \
                                                           << CAP_DSTRD_SHIFT)
#define QNAP_NVME_CAP_SET_NSSRS(cap, val)  (cap |= (uint64_t)(val & CAP_NSSRS_MASK) \
                                                           << CAP_NSSRS_SHIFT)
#define QNAP_NVME_CAP_SET_CSS(cap, val)    (cap |= (uint64_t)(val & CAP_CSS_MASK)   \
                                                           << CAP_CSS_SHIFT)
#define QNAP_NVME_CAP_SET_MPSMIN(cap, val) (cap |= (uint64_t)(val & CAP_MPSMIN_MASK)\
                                                           << CAP_MPSMIN_SHIFT)
#define QNAP_NVME_CAP_SET_MPSMAX(cap, val) (cap |= (uint64_t)(val & CAP_MPSMAX_MASK)\
                                                            << CAP_MPSMAX_SHIFT)

enum NvmeCcShift {
    CC_EN_SHIFT     = 0,
    CC_CSS_SHIFT    = 4,
    CC_MPS_SHIFT    = 7,
    CC_AMS_SHIFT    = 11,
    CC_SHN_SHIFT    = 14,
    CC_IOSQES_SHIFT = 16,
    CC_IOCQES_SHIFT = 20,
};

enum NvmeCcMask {
    CC_EN_MASK      = 0x1,
    CC_CSS_MASK     = 0x7,
    CC_MPS_MASK     = 0xf,
    CC_AMS_MASK     = 0x7,
    CC_SHN_MASK     = 0x3,
    CC_IOSQES_MASK  = 0xf,
    CC_IOCQES_MASK  = 0xf,
};

#define QNAP_NVME_CC_EN(cc)     ((cc >> CC_EN_SHIFT)     & CC_EN_MASK)
#define QNAP_NVME_CC_CSS(cc)    ((cc >> CC_CSS_SHIFT)    & CC_CSS_MASK)
#define QNAP_NVME_CC_MPS(cc)    ((cc >> CC_MPS_SHIFT)    & CC_MPS_MASK)
#define QNAP_NVME_CC_AMS(cc)    ((cc >> CC_AMS_SHIFT)    & CC_AMS_MASK)
#define QNAP_NVME_CC_SHN(cc)    ((cc >> CC_SHN_SHIFT)    & CC_SHN_MASK)
#define QNAP_NVME_CC_IOSQES(cc) ((cc >> CC_IOSQES_SHIFT) & CC_IOSQES_MASK)
#define QNAP_NVME_CC_IOCQES(cc) ((cc >> CC_IOCQES_SHIFT) & CC_IOCQES_MASK)

enum NvmeCstsShift {
    CSTS_RDY_SHIFT      = 0,
    CSTS_CFS_SHIFT      = 1,
    CSTS_SHST_SHIFT     = 2,
    CSTS_NSSRO_SHIFT    = 4,
};

enum NvmeCstsMask {
    CSTS_RDY_MASK   = 0x1,
    CSTS_CFS_MASK   = 0x1,
    CSTS_SHST_MASK  = 0x3,
    CSTS_NSSRO_MASK = 0x1,
};

enum NvmeCsts {
    QNAP_NVME_CSTS_READY         = 1 << CSTS_RDY_SHIFT,
    QNAP_NVME_CSTS_FAILED        = 1 << CSTS_CFS_SHIFT,
    QNAP_NVME_CSTS_SHST_NORMAL   = 0 << CSTS_SHST_SHIFT,
    QNAP_NVME_CSTS_SHST_PROGRESS = 1 << CSTS_SHST_SHIFT,
    QNAP_NVME_CSTS_SHST_COMPLETE = 2 << CSTS_SHST_SHIFT,
    QNAP_NVME_CSTS_NSSRO         = 1 << CSTS_NSSRO_SHIFT,
};

#define QNAP_NVME_CSTS_RDY(csts)     ((csts >> CSTS_RDY_SHIFT)   & CSTS_RDY_MASK)
#define QNAP_NVME_CSTS_CFS(csts)     ((csts >> CSTS_CFS_SHIFT)   & CSTS_CFS_MASK)
#define QNAP_NVME_CSTS_SHST(csts)    ((csts >> CSTS_SHST_SHIFT)  & CSTS_SHST_MASK)
#define QNAP_NVME_CSTS_NSSRO(csts)   ((csts >> CSTS_NSSRO_SHIFT) & CSTS_NSSRO_MASK)

enum NvmeAqaShift {
    AQA_ASQS_SHIFT  = 0,
    AQA_ACQS_SHIFT  = 16,
};

enum NvmeAqaMask {
    AQA_ASQS_MASK   = 0xfff,
    AQA_ACQS_MASK   = 0xfff,
};

#define QNAP_NVME_AQA_ASQS(aqa) ((aqa >> AQA_ASQS_SHIFT) & AQA_ASQS_MASK)
#define QNAP_NVME_AQA_ACQS(aqa) ((aqa >> AQA_ACQS_SHIFT) & AQA_ACQS_MASK)

/* Support Controller Memort Buffer */
enum NvmeCmblocShift {
    CMBLOC_BIR_SHIFT     = 0,
    CMBLOC_OFST_SHIFT    = 12,
};

enum NvmeCmblocMask {
    CMBLOC_BIR_MASK      = 0x7,
    CMBLOC_OFST_MASK     = 0xfffff,
};

/* Controller Memory Buffer Config */
#define QNAP_NVME_CMB_BAR        0
#define QNAP_NVME_CMB_OFST_SZU   0x4
#define QNAP_NVME_CMB_MULTI_SZU  0x40

enum {
	/* Size Units (SZU):
	 * Indicates the granularity of the Size field.
	 */
	QNAP_NVME_CMB_SZU_4K     = 0x00,
	QNAP_NVME_CMB_SZU_64K    = 0x01,
	QNAP_NVME_CMB_SZU_1M     = 0x02,
	QNAP_NVME_CMB_SZU_16M    = 0x03,
	QNAP_NVME_CMB_SZU_256M   = 0x04,
	QNAP_NVME_CMB_SZU_4G     = 0x05,
	QNAP_NVME_CMB_SZU_16G    = 0x06,
};

#define QNAP_NVME_CMBLOC_BIR(cmbloc)    (((cmbloc) >> CMBLOC_BIR_SHIFT)   & CMBLOC_BIR_MASK)
#define QNAP_NVME_CMBLOC_OFST(cmbloc)   (((cmbloc) >> CMBLOC_OFST_SHIFT)  & CMBLOC_OFST_MASK)

#define QNAP_NVME_CMBLOC_SET_BIR(cmbloc, val)   (cmbloc |= (uint64_t)(val & CMBLOC_BIR_MASK)  \
                                                           << CMBLOC_BIR_SHIFT)
#define QNAP_NVME_CMBLOC_SET_OFST(cmbloc, val)    (cmbloc |= (uint64_t)(val & CMBLOC_OFST_MASK)   \
                                                           << CMBLOC_OFST_SHIFT)

enum NvmeCmbszShift {
    CMBSZ_SQS_SHIFT     = 0,
    CMBSZ_CQS_SHIFT     = 1,
    CMBSZ_LISTS_SHIFT   = 2,
    CMBSZ_RDS_SHIFT     = 3,
    CMBSZ_WDS_SHIFT     = 4,
    CMBSZ_SZU_SHIFT     = 8,
    CMBSZ_SZ_SHIFT      = 12,
};

enum NvmeCmbszMask {
    CMBSZ_SQS_MASK      = 0x1,
    CMBSZ_CQS_MASK      = 0x1,
    CMBSZ_LISTS_MASK    = 0x1,
    CMBSZ_RDS_MASK      = 0x1,
    CMBSZ_WDS_MASK      = 0x1,
    CMBSZ_SZU_MASK      = 0xf,
    CMBSZ_SZ_MASK       = 0xfffff,
};

#define QNAP_NVME_CMBSZ_SQS(cmbloc)    (((cmbloc) >> CMBSZ_SQS_SHIFT)    & CMBSZ_SQS_MASK)
#define QNAP_NVME_CMBSZ_CQS(cmbloc)    (((cmbloc) >> CMBSZ_CQS_SHIFT)    & CMBSZ_CQS_MASK)
#define QNAP_NVME_CMBSZ_LISTS(cmbloc)  (((cmbloc) >> CMBSZ_LISTS_SHIFT)  & CMBSZ_LISTS_MASK)
#define QNAP_NVME_CMBSZ_RDS(cmbloc)    (((cmbloc) >> CMBSZ_RDS_SHIFT)    & CMBSZ_RDS_MASK)
#define QNAP_NVME_CMBSZ_WDS(cmbloc)    (((cmbloc) >> CMBSZ_WDS_SHIFT)    & CMBSZ_WDS_MASK)
#define QNAP_NVME_CMBSZ_SZU(cmbloc)    (((cmbloc) >> CMBSZ_SZU_SHIFT)    & CMBSZ_SZU_MASK)
#define QNAP_NVME_CMBSZ_SZ(cmbloc)     (((cmbloc) >> CMBSZ_SZ_SHIFT)     & CMBSZ_SZ_MASK)

#define QNAP_NVME_CMBSZ_SET_SQS(cmbsz, val)    (cmbsz |= (uint64_t)(val & CMBSZ_SQS_MASK)   \
                                                           << CMBSZ_SQS_SHIFT)
#define QNAP_NVME_CMBSZ_SET_CQS(cmbsz, val)    (cmbsz |= (uint64_t)(val & CMBSZ_CQS_MASK)   \
                                                           << CMBSZ_CQS_SHIFT)
#define QNAP_NVME_CMBSZ_SET_LISTS(cmbsz, val)  (cmbsz |= (uint64_t)(val & CMBSZ_LISTS_MASK) \
                                                           << CMBSZ_LISTS_SHIFT)
#define QNAP_NVME_CMBSZ_SET_RDS(cmbsz, val)    (cmbsz |= (uint64_t)(val & CMBSZ_RDS_MASK)   \
                                                           << CMBSZ_RDS_SHIFT)
#define QNAP_NVME_CMBSZ_SET_WDS(cmbsz, val)    (cmbsz |= (uint64_t)(val & CMBSZ_WDS_MASK)   \
                                                           << CMBSZ_WDS_SHIFT)
#define QNAP_NVME_CMBSZ_SET_SZU(cmbsz, val)    (cmbsz |= (uint64_t)(val & CMBSZ_SZU_MASK)   \
                                                           << CMBSZ_SZU_SHIFT)
#define QNAP_NVME_CMBSZ_SET_SZ(cmbsz, val)     (cmbsz |= (uint64_t)(val & CMBSZ_SZ_MASK)    \
                                                           << CMBSZ_SZ_SHIFT)

enum PCIeMlbar0Shift {
    MLBAR0_RTE_SHIFT  = 0,
    MLBAR0_TP_SHIFT   = 1,
    MLBAR0_PF_SHIFT   = 3,
    MLBAR0_BA_SHIFT   = 14,
};

enum PCieMlbar0Mask {
    MLBAR0_RTE_MASK   = 0x1,
    MLBAR0_TP_MASK    = 0x3,
    MLBAR0_PF_MASK    = 0x1,
    MLBAR0_BA_MASK    = 0x3ffff,
};

#define QNAP_PCIE_MLBAR0_BA(mlbar0) ((mlbar0 >> MLBAR0_BA_SHIFT) & MLBAR0_BA_MASK)

/*******************************************************************************/
/* Last DB offset- depends on max. number of queues we support */
#define NVME_LAST_DOORBELL (0x1000 + ((2*(AL_NVME_QUEUES_MAX - 1) + 1) * 4))

#define NVME_CTRL_DMA_H2D       0
#define NVME_CTRL_DMA_D2H       1

#define QNAP_NVME_CTRL_NUM      2
/* Cleanup tasks parameters */
#define TASK_CLEANUP_PARAM_DMA_IDX  0
#define TASK_CLEANUP_PARAM_DMA_CHAN 1

/* Fetch task parameters */
#define TASK_FETCH_PARAM_DMA_CHAN	0
#define TASK_FETCH_PARAM_SQ_FIRST	1
#define TASK_FETCH_PARAM_SQ_LAST	2

/* Execute task parameters */
#define TASK_EXECUTE_PARAM_DMA_CHAN 0

/* The default batch size used by various tasks (Fetch, Read, Write etc.) */
#define AL_NVME_CTRL_BATCH_SIZE	    32

/* Max. number of PRP entries we support for each command- for a host with a
 * 4K page size, this means we support exactly one PRP list (4K/64b = 64 +
 * PRP1)
 * 4096(Bytes)/8(Bytes) = 512
 */
#define AL_NVME_CTRL_MAX_PRP_ENTS	512

/* TODO */
#define AL_NVME_AVAIL_REQS_PER_SQ	1024

/* RTC cpus- TODO */
#define AL_NVME_1_RTC_FIRST_CPU		0
#define AL_NVME_1_RTC_LAST_CPU		1
#define AL_NVME_2_RTC_FIRST_CPU		2
#define AL_NVME_2_RTC_LAST_CPU		3

/* Should be set to 1 if fences (memory barriers) are needed when passing data
 * between NVMe tasks. By design, memory barriers are not required
 * as tasks that pass data to each other all run on the same cpu and under the
 * same process.
 * If at some point data is passed between tasks that run on different cpus,
 * memory barriers should be re-enabled.
 */
#define AL_NVME_FENCES_NEEDED		1

#if AL_NVME_FENCES_NEEDED
	#define AL_NVME_MB_READ_DEPENDS()	smp_read_barrier_depends()
	#define AL_NVME_MB()			smp_mb()
	#define AL_NVME_WMB()			smp_wmb()
#else
	#define AL_NVME_MB_READ_DEPENDS()
	#define AL_NVME_MB()
	#define AL_NVME_WMB()
#endif

struct al_nvme_req;

/* a generic ring buffer of requests- used to pass requests between
 * different stages of command execution.
 * ring size is always a power of 2.
 */
struct al_reqs_ring {
	struct al_nvme_req **reqs;
	unsigned int head;
	unsigned int tail;
	unsigned int size;
	unsigned int size_mask;
};


/* Slab related helpers */
typedef struct kmem_cache al_nvme_cache;

#define al_nvme_malloc(size) kmalloc(size, GFP_KERNEL)
#define al_nvme_zalloc(size) kzalloc(size, GFP_KERNEL)
#define al_nvme_calloc(n, size) kcalloc(n, size, GFP_KERNEL)
#define al_nvme_cache_create(name, size)				\
			kmem_cache_create(name, size, 0,		\
					  SLAB_RECLAIM_ACCOUNT |	\
					  SLAB_HWCACHE_ALIGN, NULL)
#define al_nvme_cache_destroy(cache) kmem_cache_destroy(cache)
#define al_nvme_cache_alloc(cache) kmem_cache_alloc(cache, GFP_KERNEL)
#define al_nvme_cache_free(cache, ptr) kmem_cache_free(cache, ptr)
#define al_nvme_free(ptr) kfree(ptr)

/* this is an absolute max.- if host requests more than 129
 * queues (incl. admin), we won't let it
 */
#define AL_NVME_QUEUES_MAX		9  // admin q + i/o q
#define AL_NVME_MAX_DMAS		2

#define AL_NVME_MAX_NUM_NAMESPACES 8
#define AL_NVME_TEMPERATURE        0x143

#define AL_NVME_MSIX_TABLE_SIZE		AL_NVME_QUEUES_MAX
#define AL_NVME_MSIX_SHADOW_SIZE	(AL_NVME_MSIX_TABLE_SIZE *	\
					 PCI_MSIX_ENTRY_SIZE)

/* TODO- implement a way to use an alternative to a LIFO requests cache */
#define AL_NVME_REQS_LIFO		1

/* Queue is considered full when tail is one step behind the head */
#define CQ_IS_FULL(cq) ((cq->tail < (cq->queue_size - 1)) ?	\
			(cq->head == (cq->tail+1)) :		\
			(cq->head == 0))

//#define NVMET_MAX_INLINE_BIOVEC 8

enum al_req_execution_status {
	NVME_REQ_IN_PROGRESS,
	NVME_REQ_COMPLETE,
};

/* forward declaration */
struct al_nvme_ctrl;
struct al_nvme_cq;

struct al_nvme_namespace {
	u8 ns_id;
};

/* Describes a DMA buffer allocated using the DMA API */
struct al_dma_buffer {
	void *buf;
	dma_addr_t dma_addr;
};

/* Describes a requests LIFO (Last In First Out) structure where requests
 * are pre-allocated and are served out (get or put) in a LIFO fashion.
 */
struct al_reqs_lifo {
	struct al_nvme_req **reqs;
	unsigned int head;
	unsigned int size;
};

/* Describes a shared buffer which might be used by multiple requests.
 * Used in practice for shared buffers when batching multiple commands
 * or completion descriptors in one IO-DMA transaction
 */
struct al_shared_buf {
	void *buf;
    dma_addr_t dma_addr;
	unsigned int count;
};

struct al_bufs_lifo {
	struct al_shared_buf **bufs;
	unsigned int head;
	unsigned int size;
};

struct al_nvme_req {
    /* SQ and CQ this request is associated with */
	unsigned int sq_id;
	unsigned int cq_id;

	/* Associated SQ head when this request is fetched from host */
	/* TODO might be better to serve this directly from SQ */
	u16 sq_head;

	/* Requests LIFO this request belongs to */
	struct al_reqs_lifo *reqs_lifo;
	struct al_bufs_lifo *cmds_lifo;

	/* Shared command/completion descriptor buffer- used when batching
	 * multiple descriptors in one transaction
	 */
	struct al_shared_buf *shared_cmd_buf;
	struct al_shared_buf *shared_comp_buf;
	unsigned int batch_size;

	/* NVMe command buffer */
	struct nvme_command *cmd;
	bool in_host_mem; /* false in RIB mode */
	u32 result;
	u16 status;

    struct page *pages;
	unsigned int prp_nents;
	unsigned int sg_nents;

	/* A pre-allocated buffer used when a page of PRP entries needs to be
	 * fetched for this request
	 */
	struct al_dma_buffer prp_list;
    void __iomem *vbase1[AL_NVME_CTRL_MAX_PRP_ENTS];

    /* The current task this request is currently at and the next task's
	 * output queue this task should be sent to.
	 * This is used to keep track of where this request should be
	 * sent to next.
	 */
	void *task;
	unsigned int task_queue;

	/* DMA channel to be used for the next transaction */
	unsigned int chan;

	struct al_nvme_ctrl *ctrl;
	struct nvmet_req target_req;
	struct al_nvme_cq *cq;
	struct al_nvme_sq *sq;
	unsigned int sqe_size_2n;
	unsigned int cqe_size_2n;

    struct al_dma_buffer log_page_smart;
    dma_addr_t dma_map_addr;
	struct scatterlist *src_sgl;
	/* Points to the next request in a batch.
	 * This is used when batching multiple requests- each request will
	 * link to the next.
	 */
	struct al_nvme_req *next_req;

#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
	void *rw_buf;
	struct scatterlist rw_sgl;
	struct nvmet_req rwbuf_treq;
	bool test_flag;
	int store_sgcnt;
	struct scatterlist *store_sg;
#endif
	struct {
		phys_addr_t msix_addr;
		phys_addr_t msix_data_phy_addr;
		uint32_t *msix_data;
		u8 async_cmd_free;
		unsigned int pending_reqs;
	} comp_cb_ctx;
};

struct al_nvme_sq {
	u64 base_addr				____cacheline_aligned;
	u32 queue_size;

	u32 head				____cacheline_aligned;
	u32 tail				____cacheline_aligned;

	struct al_nvme_ctrl *ctrl		____cacheline_aligned;
    struct percpu_ref   ref;
	u16 sq_id;
	u16 cq_id;

	bool in_host_mem; /* false in RIB mode */

	/* a LIFO of available requests- all requests in this LIFO are
	 * pre-allocated. when the fetcher needs an available
	 * request, it takes one from the head. requests that are no longer
	 * needed are put back into the head.
	 */
	struct al_reqs_lifo avail_reqs		____cacheline_aligned;

	/* a LIFO of available shared command buffers */
	struct al_bufs_lifo avail_cmds		____cacheline_aligned;
    struct nvmet_sq nvmet_sq;
    struct completion   free_done;
    struct completion   confirm_done;
};

struct al_nvme_cq {
	struct al_nvme_ctrl *ctrl;
	u16 cq_id;
	u32 head;
	u32 tail;
	u16 int_vector;
	u8 int_enabled;
	u32 queue_size;
	u64 host_addr;
	u8 phase;
	void *mapped_host_addr;
	void *host_addr_phys;

	/* msix_host_addr is cached and mapped when vector is first
	 * needed
	 */
	u64 msix_host_addr;
	void *mapped_msix_host_addr;

	/* a LIFO of available shared completion buffers */
	struct al_bufs_lifo avail_comps		____cacheline_aligned;
    struct nvmet_cq nvmet_cq;
};

struct al_nvme_ctrl {
	struct nvmet_ctrl *target_ctrl;
	struct al_nvme_task_manager *tm;
    struct scatterlist *sg;
	unsigned char **data_buffer;
	u8 ctrl_id;
    u64 ns_size;

	/* first queue of each type (SQ/CQ) is the admin queue, and is created
	 * on init (the rest of the queues are created on SW request)
	 */
	struct al_nvme_sq **sq;
	struct al_nvme_cq **cq;

	/* place holder for execution engine/admin private data */
	void *admin_data;

	struct al_dma_buffer ctrl_regs;
	/* protects access to the control registers */
	spinlock_t ctrl_regs_lock;
	spinlock_t bar_regs_lock;

	/* available requests are allocated per sq from this cache on init */
	al_nvme_cache *reqs_cache;
	/* general nvme params that are set by the host */
	struct {
		u16 page_bits;
		u64 page_size;
		u16 sqe_size_2n;
		u16 cqe_size_2n;
		u8 cmb_szu;
	} params;
    unsigned int ns_num;
    struct {
        u32 arbitration;
        u32 power_mgmt;
        u32 temp_thresh;
        u32 err_rec;
        u32 volatile_wc;
        u32 num_queues;
        u32 int_coalescing;
        u32 *int_vector_config;
        u32 write_atomicity;
        u32 async_config;
        u32 sw_prog_marker;
    } feat_val;
	struct {
		char sn[20];
		char mn[40];
		char fr[8];
	} id_ctrl;

    int fetch_task_id[NR_CPUS];
    int exec_task_id[NR_CPUS];
    int comp_task_id[NR_CPUS];
    int fetchprp_task_id[NR_CPUS];
    int bridge_task_id[NR_CPUS];
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
    int cpy_task_id[NR_CPUS];
#endif

    int exec_queue_id[NR_CPUS];
    int bridge_queue_id[NR_CPUS];
    int comp_queue_id[NR_CPUS];
    int fetchprp_queue_id[NR_CPUS];
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
    int cpy_queue_id[NR_CPUS];
#endif

	unsigned int first_cpu;
	unsigned int last_cpu;
	int cmd_cnt[AL_NVME_QUEUES_MAX];
	int cmd_prp_cnt[AL_NVME_QUEUES_MAX][1024];
    /* general wq used for starting/stopping controller */
	struct workqueue_struct *ctrl_wq;
	struct work_struct start_ctrl_work;
	struct work_struct stop_ctrl_work;

    struct nvmet_port *pcie_port;
    struct nvme_dma_template *dma[AL_NVME_MAX_DMAS];

	struct qnap_ep_device *pcie_data;
	struct task_struct *db_kthread;
	struct task_struct *bar_kthread;
};

#endif //__QNAP_NVMET_PCIE_H
