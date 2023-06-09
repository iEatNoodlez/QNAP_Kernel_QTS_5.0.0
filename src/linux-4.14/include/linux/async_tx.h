/*
 * Copyright © 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */
#ifndef _ASYNC_TX_H_
#define _ASYNC_TX_H_
#include <linux/dmaengine.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

/* on architectures without dma-mapping capabilities we need to ensure
 * that the asynchronous path compiles away
 */
#ifdef CONFIG_HAS_DMA
#define __async_inline
#else
#define __async_inline __always_inline
#endif

/**
 * dma_chan_ref - object used to manage dma channels received from the
 *   dmaengine core.
 * @chan - the channel being tracked
 * @node - node for the channel to be placed on async_tx_master_list
 * @rcu - for list_del_rcu
 * @count - number of times this channel is listed in the pool
 *	(for channels with multiple capabiities)
 */
struct dma_chan_ref {
	struct dma_chan *chan;
	struct list_head node;
	struct rcu_head rcu;
	atomic_t count;
};

/**
 * async_tx_flags - modifiers for the async_* calls
 * @ASYNC_TX_XOR_ZERO_DST: this flag must be used for xor operations where the
 * the destination address is not a source.  The asynchronous case handles this
 * implicitly, the synchronous case needs to zero the destination block.
 * @ASYNC_TX_XOR_DROP_DST: this flag must be used if the destination address is
 * also one of the source addresses.  In the synchronous case the destination
 * address is an implied source, whereas the asynchronous case it must be listed
 * as a source.  The destination address must be the first address in the source
 * array.
 * @ASYNC_TX_ACK: immediately ack the descriptor, precludes setting up a
 * dependency chain
 * @ASYNC_TX_FENCE: specify that the next operation in the dependency
 * chain uses this operation's result as an input
 * @ASYNC_TX_PQ_XOR_DST: do not overwrite the syndrome but XOR it with the
 * input data. Required for rmw case.
 */
enum async_tx_flags {
	ASYNC_TX_XOR_ZERO_DST	 = (1 << 0),
	ASYNC_TX_XOR_DROP_DST	 = (1 << 1),
	ASYNC_TX_ACK		 = (1 << 2),
	ASYNC_TX_FENCE		 = (1 << 3),
	ASYNC_TX_PQ_XOR_DST	 = (1 << 4),
};

/**
 * struct async_submit_ctl - async_tx submission/completion modifiers
 * @flags: submission modifiers
 * @depend_tx: parent dependency of the current operation being submitted
 * @cb_fn: callback routine to run at operation completion
 * @cb_param: parameter for the callback routine
 * @scribble: caller provided space for dma/page address conversions
 */
struct async_submit_ctl {
	enum async_tx_flags flags;
	struct dma_async_tx_descriptor *depend_tx;
	dma_async_tx_callback cb_fn;
	void *cb_param;
	void *scribble;
};

#if defined(CONFIG_DMA_ENGINE) && !defined(CONFIG_ASYNC_TX_CHANNEL_SWITCH)
#define async_tx_issue_pending_all dma_issue_pending_all

/**
 * async_tx_issue_pending - send pending descriptor to the hardware channel
 * @tx: descriptor handle to retrieve hardware context
 *
 * Note: any dependent operations will have already been issued by
 * async_tx_channel_switch, or (in the case of no channel switch) will
 * be already pending on this channel.
 */
static inline void async_tx_issue_pending(struct dma_async_tx_descriptor *tx)
{
	if (likely(tx)) {
		struct dma_chan *chan = tx->chan;
		struct dma_device *dma = chan->device;

		dma->device_issue_pending(chan);
	}
}
#ifdef CONFIG_ARCH_HAS_ASYNC_TX_FIND_CHANNEL
#include <asm/async_tx.h>
#else
#define async_tx_find_channel(dep, type, dst, dst_count, src, src_count, len) \
	 __async_tx_find_channel(dep, type)
struct dma_chan *
__async_tx_find_channel(struct async_submit_ctl *submit,
			enum dma_transaction_type tx_type);

#ifdef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
#define async_tx_find_channel_by_index(dep, type, dst, dst_count, src, src_count, len, index) \
	 __async_tx_find_channel_by_index(dep, type, index)
struct dma_chan *
__async_tx_find_channel_by_index(struct async_submit_ctl *submit,
			enum dma_transaction_type tx_type, int index);
#endif
#endif /* CONFIG_ARCH_HAS_ASYNC_TX_FIND_CHANNEL */
#else
static inline void async_tx_issue_pending_all(void)
{
	do { } while (0);
}

static inline void async_tx_issue_pending(struct dma_async_tx_descriptor *tx)
{
	do { } while (0);
}

static inline struct dma_chan *
async_tx_find_channel(struct async_submit_ctl *submit,
		      enum dma_transaction_type tx_type, struct page **dst,
		      int dst_count, struct page **src, int src_count,
		      size_t len)
{
	return NULL;
}
#endif

/**
 * async_tx_sync_epilog - actions to take if an operation is run synchronously
 * @cb_fn: function to call when the transaction completes
 * @cb_fn_param: parameter to pass to the callback routine
 */
static inline void
async_tx_sync_epilog(struct async_submit_ctl *submit)
{
	if (submit->cb_fn)
		submit->cb_fn(submit->cb_param);
}

typedef union {
	unsigned long addr;
	struct page *page;
	dma_addr_t dma;
} addr_conv_t;

static inline void
init_async_submit(struct async_submit_ctl *args, enum async_tx_flags flags,
		  struct dma_async_tx_descriptor *tx,
		  dma_async_tx_callback cb_fn, void *cb_param,
		  addr_conv_t *scribble)
{
	args->flags = flags;
	args->depend_tx = tx;
	args->cb_fn = cb_fn;
	args->cb_param = cb_param;
	args->scribble = scribble;
}

void async_tx_submit(struct dma_chan *chan, struct dma_async_tx_descriptor *tx,
		     struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_xor(struct page *dest, struct page **src_list, unsigned int offset,
	  int src_cnt, size_t len, struct async_submit_ctl *submit);

#ifdef CONFIG_MACH_QNAPTS
struct dma_async_tx_descriptor *
async_xor_phys(phys_addr_t dest, phys_addr_t *src_list, unsigned int offset,
	  int src_cnt, size_t len, struct async_submit_ctl *submit);
#endif

struct dma_async_tx_descriptor *
async_xor_val(struct page *dest, struct page **src_list, unsigned int offset,
	      int src_cnt, size_t len, enum sum_check_flags *result,
	      struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_memcpy(struct page *dest, struct page *src, unsigned int dest_offset,
	     unsigned int src_offset, size_t len,
	     struct async_submit_ctl *submit);

#ifdef CONFIG_MACH_QNAPTS
struct dma_async_tx_descriptor *
async_memcpy_phys(phys_addr_t dest, phys_addr_t src, unsigned int dest_offset,
         unsigned int src_offset, size_t len,
         struct async_submit_ctl *submit);
#ifdef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
struct dma_async_tx_descriptor *
async_memcpy_phys_by_index(phys_addr_t dest, phys_addr_t src, unsigned int dest_offset,
         unsigned int src_offset, size_t len,
         struct async_submit_ctl *submit, int index);
#endif
#endif

struct dma_async_tx_descriptor *async_trigger_callback(struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_gen_syndrome(struct page **blocks, unsigned int offset, int src_cnt,
		   size_t len, struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_syndrome_val(struct page **blocks, unsigned int offset, int src_cnt,
		   size_t len, enum sum_check_flags *pqres, struct page *spare,
		   struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_raid6_2data_recov(int src_num, size_t bytes, int faila, int failb,
			struct page **ptrs, struct async_submit_ctl *submit);

struct dma_async_tx_descriptor *
async_raid6_datap_recov(int src_num, size_t bytes, int faila,
			struct page **ptrs, struct async_submit_ctl *submit);

void async_tx_quiesce(struct dma_async_tx_descriptor **tx);

#ifdef CONFIG_MACH_QNAPTS
/**
 * @fn int async_memcpy_sg(struct scatterlist *sg_src, unsigned int src_cnt, struct scatterlist *sg_dest, unsigned int dest_cnt,
 *                         dma_async_tx_callback cb_fn, void *cb_param)
 * @brief Request DMA engine to perform memory copy in asynchronous and scatter gather list mode.
 *
 * @param[in] sg_src sg list for memory copy source.
 * @param[in] src_cnt sg list entry count for memory copy source.
 * @param[in] sg_dest sg list for memory copy destination.
 * @param[in] dest_cnt sg list entry count for memory copy destination.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @retval size_str Return the converted output binary buffer length.
 * @retval 0 Success, the async_memcpy_sg submit successfully.
 * @retval -1 Generic failure.
 */
int async_memcpy_sg(struct scatterlist *sg_src, int src_cnt, struct scatterlist *sg_dest, int dest_cnt, dma_async_tx_callback cb_fn, void *cb_param);

/**
 * @fn int async_sg_to_single_buf(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)

 * @brief Request DMA engine to perform memory copy in asynchronous and scatter gather list to single buffer mode.
 *
 * @param[in] sg_src sg list for memory copy source.
 * @param[in] src_cnt sg list entry count for memory copy source.
 * @param[in] vaddr contiguous single buffer virtual address for destination.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @retval 0 Success, the async_sg_to_single_buf submit successfully.
 * @retval -1 Generic failure.
 */
int async_sg_to_single_buf(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param);

/**
 * @fn int async_single_buf_to_sg(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)
 * @brief Request DMA engine to perform memory copy in asynchronous and single buffer to scatter gather list mode.
 *
 * @param[in] sg_dest sg list for memory copy destination.
 * @param[in] dest_cnt sg list entry count for memory copy destination.
 * @param[in] vaddr single buffer virtual address for source.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @retval 0 Success, the async_single_buf_to_sg submit successfully.
 * @retval -1 Generic failure.
 */
int async_single_buf_to_sg(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param);

#ifdef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
/**
 * @fn int async_memcpy_sg_by_index(struct scatterlist *sg_src, unsigned int src_cnt, struct scatterlist *sg_dest, unsigned int dest_cnt,
 *                         dma_async_tx_callback cb_fn, void *cb_param, int index)
 * @brief Request DMA engine to perform memory copy in asynchronous and scatter gather list mode.
 *
 * @param[in] sg_src sg list for memory copy source.
 * @param[in] src_cnt sg list entry count for memory copy source.
 * @param[in] sg_dest sg list for memory copy destination.
 * @param[in] dest_cnt sg list entry count for memory copy destination.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @param[in] index Hint for choose dma engine, -1 for choose from all dma engine.
 * @retval size_str Return the converted output binary buffer length.
 * @retval 0 Success, the async_memcpy_sg submit successfully.
 * @retval -1 Generic failure.
 */
int async_memcpy_sg_by_index(struct scatterlist *sg_src, int src_cnt, struct scatterlist *sg_dest, int dest_cnt, dma_async_tx_callback cb_fn, void *cb_param, int index);

/**
 * @fn int async_sg_to_single_buf_by_index(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index)

 * @brief Request DMA engine to perform memory copy in asynchronous and scatter gather list to single buffer mode.
 *
 * @param[in] sg_src sg list for memory copy source.
 * @param[in] src_cnt sg list entry count for memory copy source.
 * @param[in] vaddr contiguous single buffer virtual address for destination.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @param[in] index Hint for choose dma engine, -1 for choose from all dma engine.
 * @retval 0 Success, the async_sg_to_single_buf submit successfully.
 * @retval -1 Generic failure.
 */
int async_sg_to_single_buf_by_index(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index);

/**
 * @fn int async_single_buf_to_sg_by_index(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index)
 * @brief Request DMA engine to perform memory copy in asynchronous and single buffer to scatter gather list mode.
 *
 * @param[in] sg_dest sg list for memory copy destination.
 * @param[in] dest_cnt sg list entry count for memory copy destination.
 * @param[in] vaddr single buffer virtual address for source.
 * @param[in] cb_fn memory copy complete callback funtion.
 * @param[in] cb_param memory copy complete callback parameter.
 * @param[in] index Hint for choose dma engine, -1 for choose from all dma engine.
 * @retval 0 Success, the async_single_buf_to_sg submit successfully.
 * @retval -1 Generic failure.
 */
int async_single_buf_to_sg_by_index(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index);
#endif
#endif

#endif /* _ASYNC_TX_H_ */
