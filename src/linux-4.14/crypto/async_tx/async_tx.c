/*
 * core routines for the asynchronous memory transfer/transform api
 *
 * Copyright © 2006, Intel Corporation.
 *
 *	Dan Williams <dan.j.williams@intel.com>
 *
 *	with architecture considerations by:
 *	Neil Brown <neilb@suse.de>
 *	Jeff Garzik <jeff@garzik.org>
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
#include <linux/rculist.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/async_tx.h>
#include <linux/slab.h>

#ifdef CONFIG_DMA_ENGINE
static int __init async_tx_init(void)
{
	async_dmaengine_get();

	printk(KERN_INFO "async_tx: api initialized (async)\n");

	return 0;
}

static void __exit async_tx_exit(void)
{
	async_dmaengine_put();
}

module_init(async_tx_init);
module_exit(async_tx_exit);

/**
 * __async_tx_find_channel - find a channel to carry out the operation or let
 *	the transaction execute synchronously
 * @submit: transaction dependency and submission modifiers
 * @tx_type: transaction type
 */
struct dma_chan *
__async_tx_find_channel(struct async_submit_ctl *submit,
			enum dma_transaction_type tx_type)
{
	struct dma_async_tx_descriptor *depend_tx = submit->depend_tx;

	/* see if we can keep the chain on one channel */
	if (depend_tx &&
	    dma_has_cap(tx_type, depend_tx->chan->device->cap_mask))
		return depend_tx->chan;
	return async_dma_find_channel(tx_type);
}
EXPORT_SYMBOL_GPL(__async_tx_find_channel);

#ifdef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
struct dma_chan *
__async_tx_find_channel_by_index(struct async_submit_ctl *submit,
			enum dma_transaction_type tx_type, int index)
{
	struct dma_async_tx_descriptor *depend_tx = submit->depend_tx;

	/* see if we can keep the chain on one channel */
	if (depend_tx &&
	    dma_has_cap(tx_type, depend_tx->chan->device->cap_mask))
		return depend_tx->chan;
	return async_dma_find_channel_by_index(tx_type, index);

}
EXPORT_SYMBOL_GPL(__async_tx_find_channel_by_index);
#endif

#endif


/**
 * async_tx_channel_switch - queue an interrupt descriptor with a dependency
 * 	pre-attached.
 * @depend_tx: the operation that must finish before the new operation runs
 * @tx: the new operation
 */
static void
async_tx_channel_switch(struct dma_async_tx_descriptor *depend_tx,
			struct dma_async_tx_descriptor *tx)
{
	struct dma_chan *chan = depend_tx->chan;
	struct dma_device *device = chan->device;
	struct dma_async_tx_descriptor *intr_tx = (void *) ~0;

	/* first check to see if we can still append to depend_tx */
	txd_lock(depend_tx);
	if (txd_parent(depend_tx) && depend_tx->chan == tx->chan) {
		txd_chain(depend_tx, tx);
		intr_tx = NULL;
	}
	txd_unlock(depend_tx);

	/* attached dependency, flush the parent channel */
	if (!intr_tx) {
		device->device_issue_pending(chan);
		return;
	}

	/* see if we can schedule an interrupt
	 * otherwise poll for completion
	 */
	if (dma_has_cap(DMA_INTERRUPT, device->cap_mask))
		intr_tx = device->device_prep_dma_interrupt(chan, 0);
	else
		intr_tx = NULL;

	if (intr_tx) {
		intr_tx->callback = NULL;
		intr_tx->callback_param = NULL;
		/* safe to chain outside the lock since we know we are
		 * not submitted yet
		 */
		txd_chain(intr_tx, tx);

		/* check if we need to append */
		txd_lock(depend_tx);
		if (txd_parent(depend_tx)) {
			txd_chain(depend_tx, intr_tx);
			async_tx_ack(intr_tx);
			intr_tx = NULL;
		}
		txd_unlock(depend_tx);

		if (intr_tx) {
			txd_clear_parent(intr_tx);
			intr_tx->tx_submit(intr_tx);
			async_tx_ack(intr_tx);
		}
		device->device_issue_pending(chan);
	} else {
		if (dma_wait_for_async_tx(depend_tx) != DMA_COMPLETE)
			panic("%s: DMA error waiting for depend_tx\n",
			      __func__);
		tx->tx_submit(tx);
	}
}


/**
 * submit_disposition - flags for routing an incoming operation
 * @ASYNC_TX_SUBMITTED: we were able to append the new operation under the lock
 * @ASYNC_TX_CHANNEL_SWITCH: when the lock is dropped schedule a channel switch
 * @ASYNC_TX_DIRECT_SUBMIT: when the lock is dropped submit directly
 *
 * while holding depend_tx->lock we must avoid submitting new operations
 * to prevent a circular locking dependency with drivers that already
 * hold a channel lock when calling async_tx_run_dependencies.
 */
enum submit_disposition {
	ASYNC_TX_SUBMITTED,
	ASYNC_TX_CHANNEL_SWITCH,
	ASYNC_TX_DIRECT_SUBMIT,
};

void
async_tx_submit(struct dma_chan *chan, struct dma_async_tx_descriptor *tx,
		struct async_submit_ctl *submit)
{
	struct dma_async_tx_descriptor *depend_tx = submit->depend_tx;

	tx->callback = submit->cb_fn;
	tx->callback_param = submit->cb_param;

	if (depend_tx) {
		enum submit_disposition s;

		/* sanity check the dependency chain:
		 * 1/ if ack is already set then we cannot be sure
		 * we are referring to the correct operation
		 * 2/ dependencies are 1:1 i.e. two transactions can
		 * not depend on the same parent
		 */
		BUG_ON(async_tx_test_ack(depend_tx) || txd_next(depend_tx) ||
		       txd_parent(tx));

		/* the lock prevents async_tx_run_dependencies from missing
		 * the setting of ->next when ->parent != NULL
		 */
		txd_lock(depend_tx);
		if (txd_parent(depend_tx)) {
			/* we have a parent so we can not submit directly
			 * if we are staying on the same channel: append
			 * else: channel switch
			 */
			if (depend_tx->chan == chan) {
				txd_chain(depend_tx, tx);
				s = ASYNC_TX_SUBMITTED;
			} else
				s = ASYNC_TX_CHANNEL_SWITCH;
		} else {
			/* we do not have a parent so we may be able to submit
			 * directly if we are staying on the same channel
			 */
			if (depend_tx->chan == chan)
				s = ASYNC_TX_DIRECT_SUBMIT;
			else
				s = ASYNC_TX_CHANNEL_SWITCH;
		}
		txd_unlock(depend_tx);

		switch (s) {
		case ASYNC_TX_SUBMITTED:
			break;
		case ASYNC_TX_CHANNEL_SWITCH:
			async_tx_channel_switch(depend_tx, tx);
			break;
		case ASYNC_TX_DIRECT_SUBMIT:
			txd_clear_parent(tx);
			tx->tx_submit(tx);
			break;
		}
	} else {
		txd_clear_parent(tx);
		tx->tx_submit(tx);
	}

	if (submit->flags & ASYNC_TX_ACK)
		async_tx_ack(tx);

	if (depend_tx)
		async_tx_ack(depend_tx);
}
EXPORT_SYMBOL_GPL(async_tx_submit);

/**
 * async_trigger_callback - schedules the callback function to be run
 * @submit: submission and completion parameters
 *
 * honored flags: ASYNC_TX_ACK
 *
 * The callback is run after any dependent operations have completed.
 */
struct dma_async_tx_descriptor *
async_trigger_callback(struct async_submit_ctl *submit)
{
	struct dma_chan *chan;
	struct dma_device *device;
	struct dma_async_tx_descriptor *tx;
	struct dma_async_tx_descriptor *depend_tx = submit->depend_tx;

	if (depend_tx) {
		chan = depend_tx->chan;
		device = chan->device;

		/* see if we can schedule an interrupt
		 * otherwise poll for completion
		 */
		if (device && !dma_has_cap(DMA_INTERRUPT, device->cap_mask))
			device = NULL;

		tx = device ? device->device_prep_dma_interrupt(chan, 0) : NULL;
	} else
		tx = NULL;

	if (tx) {
		pr_debug("%s: (async)\n", __func__);

		async_tx_submit(chan, tx, submit);
	} else {
		pr_debug("%s: (sync)\n", __func__);

		/* wait for any prerequisite operations */
		async_tx_quiesce(&submit->depend_tx);

		async_tx_sync_epilog(submit);
	}

	return tx;
}
EXPORT_SYMBOL_GPL(async_trigger_callback);

/**
 * async_tx_quiesce - ensure tx is complete and freeable upon return
 * @tx - transaction to quiesce
 */
void async_tx_quiesce(struct dma_async_tx_descriptor **tx)
{
	if (*tx) {
		/* if ack is already set then we cannot be sure
		 * we are referring to the correct operation
		 */
		BUG_ON(async_tx_test_ack(*tx));
		if (dma_wait_for_async_tx(*tx) != DMA_COMPLETE)
			panic("%s: DMA error waiting for transaction\n",
			      __func__);
		async_tx_ack(*tx);
		*tx = NULL;
	}
}
EXPORT_SYMBOL_GPL(async_tx_quiesce);


#ifdef CONFIG_MACH_QNAPTS
#define MAX_ISSUE_SG_COUNT 16
#define BREAK_COUNT 10000000
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
 * @retval 0 Success, the async_memcpy_sg submit successfully.
 * @retval -1 Generic failure.
 */
// FIXME this api only support ARM model without iommu, 
// If you want to support iommu, need to map page in this function.
// This should modify this api interface.

#ifndef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
int async_memcpy_sg(struct scatterlist *sg_src, int src_cnt, struct scatterlist *sg_dest, int dest_cnt, dma_async_tx_callback cb_fn, void *cb_param)
{

    int offset = 0;
    struct dma_chan *chan = NULL;
    struct dma_async_tx_descriptor *tx = NULL;
    int transmit_count = 0;
    unsigned long flags = 0;
    int last = 0;
    int count = 0;

    if (sg_src == NULL || sg_dest == NULL)
        return -1;

    if (dest_cnt != src_cnt)
        return -1;

    if (sg_dma_len(sg_src) != sg_dma_len(sg_dest))
        return -1;
           
    // Get dma channel
    chan = async_dma_find_channel(DMA_SG);

    if (!chan || !chan->device || !chan->device->device_prep_dma_sg)
        return -1;

    while (dest_cnt > 0)
    {

        flags = DMA_CTRL_ACK;

        // calculate transmit count 
        if (dest_cnt <= MAX_ISSUE_SG_COUNT)
        {
            transmit_count = dest_cnt; 
            if (cb_fn)
            {
                last = 1;
                flags |= DMA_PREP_INTERRUPT;
            }
        }
        else
        {
            transmit_count = MAX_ISSUE_SG_COUNT;
        }


        transmit_count = dest_cnt > MAX_ISSUE_SG_COUNT ? MAX_ISSUE_SG_COUNT : dest_cnt;
        
        tx = chan->device->device_prep_dma_sg(chan, sg_dest + offset, transmit_count, sg_src + offset, transmit_count, flags);


        count = 0;
        while (!tx)
        {
            // FIXME: need a timeout to break;
            // This is easy to break the loop
            count++;
            if (count == BREAK_COUNT)
                return -1;

            // no tx description, issue pending request and get again
            dma_async_issue_pending(chan);
            tx = chan->device->device_prep_dma_sg(chan, sg_dest + offset, transmit_count, sg_src + offset, transmit_count, flags);

        }

        if (last)
        {
            pr_debug("%s: callback %p and param %p, flags = %lx \n", __func__, cb_fn, cb_param, flags);
            tx->callback = cb_fn;
            tx->callback_param = cb_param;
        }

        dmaengine_submit(tx);
        dma_async_issue_pending(chan);

        dest_cnt -= transmit_count;
        offset += transmit_count;
        
    }
    
    return 0;
}
EXPORT_SYMBOL(async_memcpy_sg);
#else
int async_memcpy_sg(struct scatterlist *sg_src, int src_cnt, struct scatterlist *sg_dest, int dest_cnt, dma_async_tx_callback cb_fn, void *cb_param)
{
    return async_memcpy_sg_by_index(sg_src, src_cnt, sg_dest, dest_cnt, cb_fn, cb_param, -1);

}
EXPORT_SYMBOL(async_memcpy_sg);

int async_memcpy_sg_by_index(struct scatterlist *sg_src, int src_cnt, struct scatterlist *sg_dest, int dest_cnt, dma_async_tx_callback cb_fn, void *cb_param, int index)
{

    int offset = 0;
    struct dma_chan *chan = NULL;
    struct dma_async_tx_descriptor *tx = NULL;
    int transmit_count = 0;
    unsigned long flags = 0;
    int last = 0;
    int count = 0;

    if (sg_src == NULL || sg_dest == NULL)
        return -1;

    if (dest_cnt != src_cnt)
        return -1;

    if (sg_dma_len(sg_src) != sg_dma_len(sg_dest))
        return -1;
           
    // Get dma channel
    if (index < 0)
        chan = async_dma_find_channel(DMA_SG);
    else
        chan = async_dma_find_channel_by_index(DMA_SG, index);

    if (!chan || !chan->device || !chan->device->device_prep_dma_sg)
        return -1;

    while (dest_cnt > 0)
    {

        flags = DMA_CTRL_ACK;

        // calculate transmit count 
        if (dest_cnt <= MAX_ISSUE_SG_COUNT)
        {
            transmit_count = dest_cnt; 
            if (cb_fn)
            {
                last = 1;
                flags |= DMA_PREP_INTERRUPT;
            }
        }
        else
        {
            transmit_count = MAX_ISSUE_SG_COUNT;
        }


        transmit_count = dest_cnt > MAX_ISSUE_SG_COUNT ? MAX_ISSUE_SG_COUNT : dest_cnt;
        
        tx = chan->device->device_prep_dma_sg(chan, sg_dest + offset, transmit_count, sg_src + offset, transmit_count, flags);


        count = 0;
        while (!tx)
        {
            // FIXME: need a timeout to break;
            // This is easy to break the loop
            count++;
            if (count == BREAK_COUNT)
                return -1;

            // no tx description, issue pending request and get again
            dma_async_issue_pending(chan);
            tx = chan->device->device_prep_dma_sg(chan, sg_dest + offset, transmit_count, sg_src + offset, transmit_count, flags);

        }

        if (last)
        {
            pr_debug("%s: callback %p and param %p, flags = %lx \n", __func__, cb_fn, cb_param, flags);
            tx->callback = cb_fn;
            tx->callback_param = cb_param;
        }

        dmaengine_submit(tx);
        dma_async_issue_pending(chan);

        dest_cnt -= transmit_count;
        offset += transmit_count;
        
    }
    
    return 0;
}
EXPORT_SYMBOL(async_memcpy_sg_by_index);
#endif

typedef struct async_sg_to_sg_cb
{
    struct scatterlist *sg_list;
    dma_async_tx_callback callback;
    void *contextP;

} SG_SG_CB;

void async_sg_to_sg_common_callback(void *cb_parm)
{
    SG_SG_CB *tmp;
    if (cb_parm)
    {
        tmp = (SG_SG_CB *) cb_parm;
        pr_debug("%s: callback %p and param %p\n", __func__, tmp->callback, tmp->contextP);
        if (tmp->callback)
        {
            tmp->callback(tmp->contextP);
        }
        kfree(tmp->sg_list);
        kfree(tmp);
    }
    return;
}

#ifndef CONFIG_ASYNC_TX_DMA_ROUND_ROBIN
int async_sg_to_single_buf(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)
{
    SG_SG_CB *cb;
    int i;
    dma_addr_t paddr;
    struct scatterlist *sg_tmp;
    int ret;



    if (sg_src == NULL || vaddr == NULL || src_cnt == 0)
        return -1;

    cb = kzalloc(sizeof(SG_SG_CB), GFP_KERNEL | GFP_NOWAIT);
    if (!cb)
        return -1;

    cb->sg_list = kzalloc(sizeof(struct scatterlist) * src_cnt, GFP_KERNEL | GFP_NOWAIT);
    if (!cb->sg_list)
    {
        kfree(cb);
        return -1;
    }

    paddr = virt_to_phys(vaddr);

    for (i = 0; i < src_cnt; i++)
    {
        sg_tmp = sg_src + i;
        sg_dma_address(cb->sg_list + i) = paddr;

        if (!sg_dma_address(sg_tmp))
        {

            sg_dma_len(sg_tmp) = sg_tmp->length;
            sg_dma_address(sg_tmp) = sg_page(sg_tmp) + sg_tmp->offset;
        }
        pr_debug("%s: index %d sg_src addr 0x%llx, dest addr = %llx len = %u\n", __func__, i, sg_dma_address(sg_tmp), paddr, sg_dma_len(sg_tmp));
        sg_dma_len(cb->sg_list + i) = sg_dma_len(sg_tmp);
        paddr += sg_dma_len(sg_tmp);
    }
    if (cb_fn)
    {
        pr_debug("%s: setting up cb %p cb_fn %p and param %p\n", __func__, cb, cb_fn, cb_param);
        cb->callback = cb_fn;
        cb->contextP = cb_param;
    }

    ret = async_memcpy_sg(sg_src, src_cnt, cb->sg_list, src_cnt, async_sg_to_sg_common_callback, cb);

    if (ret)
    {
        // free resource
        kfree(cb->sg_list);
        kfree(cb);
    }

    return ret;
}


EXPORT_SYMBOL(async_sg_to_single_buf);

int async_single_buf_to_sg(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)
{
    SG_SG_CB *cb;
    int i;
    dma_addr_t paddr;
    struct scatterlist *sg_tmp;
    int ret;


    if (sg_dest == NULL || vaddr == NULL || dest_cnt == 0)
        return -1;

    cb = kzalloc(sizeof(SG_SG_CB), GFP_KERNEL | GFP_NOWAIT);
    if (!cb)
        return -1;

    cb->sg_list = kzalloc(sizeof(struct scatterlist) * dest_cnt, GFP_KERNEL | GFP_NOWAIT);
    if (!cb->sg_list)
    {
        kfree(cb);
        return -1;
    }

    paddr = virt_to_phys(vaddr);


    for (i = 0; i < dest_cnt; i++)
    {
        sg_tmp = sg_dest + i;

        sg_dma_address(cb->sg_list + i) = paddr;
        if (!sg_dma_address(sg_tmp))
        {
            sg_dma_len(sg_tmp) = sg_tmp->length;
            sg_dma_address(sg_tmp) = sg_page(sg_tmp) + sg_tmp->offset;
        }
        pr_debug("%s: index %d sg_dest addr 0x%llx, src addr = 0x%llx len = %u\n", __func__, i, sg_dma_address(sg_tmp), paddr, sg_dma_len(sg_tmp));
        sg_dma_len(cb->sg_list + i) = sg_dma_len(sg_tmp);
        paddr += sg_dma_len(sg_tmp);
    }
    if (cb_fn)
    {
        pr_debug("%s: setting up cb %p cb_fn %p and param %p\n", __func__, cb, cb_fn, cb_param);
        cb->callback = cb_fn;
        cb->contextP = cb_param;
    }
    ret =  async_memcpy_sg(cb->sg_list, dest_cnt, sg_dest, dest_cnt, async_sg_to_sg_common_callback, cb);

    if (ret)
    {
        // free resource
        kfree(cb->sg_list);
        kfree(cb);
    }

    return ret;
}
EXPORT_SYMBOL(async_single_buf_to_sg);
#else
int async_sg_to_single_buf(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)
{

    return async_sg_to_single_buf_by_index(sg_src, src_cnt, vaddr, cb_fn, cb_param, -1);
}
EXPORT_SYMBOL(async_sg_to_single_buf);

int async_single_buf_to_sg(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param)
{
    return async_single_buf_to_sg_by_index(sg_dest, dest_cnt, vaddr, cb_fn, cb_param, -1);
}
EXPORT_SYMBOL(async_single_buf_to_sg);

int async_sg_to_single_buf_by_index(struct scatterlist *sg_src, int src_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index)
{
    SG_SG_CB *cb;
    int i;
    dma_addr_t paddr;
    struct scatterlist *sg_tmp;
    int ret;



    if (sg_src == NULL || vaddr == NULL || src_cnt == 0)
        return -1;

    cb = kzalloc(sizeof(SG_SG_CB), GFP_KERNEL | GFP_NOWAIT);
    if (!cb)
        return -1;

    cb->sg_list = kzalloc(sizeof(struct scatterlist) * src_cnt, GFP_KERNEL | GFP_NOWAIT);
    if (!cb->sg_list)
    {
        kfree(cb);
        return -1;
    }

    paddr = virt_to_phys(vaddr);

    for (i = 0; i < src_cnt; i++)
    {
        sg_tmp = sg_src + i;
        sg_dma_address(cb->sg_list + i) = paddr;

        if (!sg_dma_address(sg_tmp))
        {

            sg_dma_len(sg_tmp) = sg_tmp->length;
            sg_dma_address(sg_tmp) = sg_page(sg_tmp) + sg_tmp->offset;
        }
        pr_debug("%s: index %d sg_src addr 0x%llx, dest addr = %llx len = %u\n", __func__, i, sg_dma_address(sg_tmp), paddr, sg_dma_len(sg_tmp));
        sg_dma_len(cb->sg_list + i) = sg_dma_len(sg_tmp);
        paddr += sg_dma_len(sg_tmp);
    }
    if (cb_fn)
    {
        pr_debug("%s: setting up cb %p cb_fn %p and param %p\n", __func__, cb, cb_fn, cb_param);
        cb->callback = cb_fn;
        cb->contextP = cb_param;
    }

    if (index < 0)
        ret = async_memcpy_sg(sg_src, src_cnt, cb->sg_list, src_cnt, async_sg_to_sg_common_callback, cb);
    else
        ret = async_memcpy_sg_by_index(sg_src, src_cnt, cb->sg_list, src_cnt, async_sg_to_sg_common_callback, cb, index);

    if (ret)
    {
        // free resource
        kfree(cb->sg_list);
        kfree(cb);
    }

    return ret;
}


EXPORT_SYMBOL(async_sg_to_single_buf_by_index);

int async_single_buf_to_sg_by_index(struct scatterlist *sg_dest, int dest_cnt, void *vaddr, dma_async_tx_callback cb_fn, void *cb_param, int index)
{
    SG_SG_CB *cb;
    int i;
    dma_addr_t paddr;
    struct scatterlist *sg_tmp;
    int ret;


    if (sg_dest == NULL || vaddr == NULL || dest_cnt == 0)
        return -1;

    cb = kzalloc(sizeof(SG_SG_CB), GFP_KERNEL | GFP_NOWAIT);
    if (!cb)
        return -1;

    cb->sg_list = kzalloc(sizeof(struct scatterlist) * dest_cnt, GFP_KERNEL | GFP_NOWAIT);
    if (!cb->sg_list)
    {
        kfree(cb);
        return -1;
    }

    paddr = virt_to_phys(vaddr);


    for (i = 0; i < dest_cnt; i++)
    {
        sg_tmp = sg_dest + i;

        sg_dma_address(cb->sg_list + i) = paddr;
        if (!sg_dma_address(sg_tmp))
        {
            sg_dma_len(sg_tmp) = sg_tmp->length;
            sg_dma_address(sg_tmp) = sg_page(sg_tmp) + sg_tmp->offset;
        }
        pr_debug("%s: index %d sg_dest addr 0x%llx, src addr = 0x%llx len = %u\n", __func__, i, sg_dma_address(sg_tmp), paddr, sg_dma_len(sg_tmp));
        sg_dma_len(cb->sg_list + i) = sg_dma_len(sg_tmp);
        paddr += sg_dma_len(sg_tmp);
    }
    if (cb_fn)
    {
        pr_debug("%s: setting up cb %p cb_fn %p and param %p\n", __func__, cb, cb_fn, cb_param);
        cb->callback = cb_fn;
        cb->contextP = cb_param;
    }
    if (index < 0)
        ret =  async_memcpy_sg(cb->sg_list, dest_cnt, sg_dest, dest_cnt, async_sg_to_sg_common_callback, cb);
    else
        ret =  async_memcpy_sg_by_index(cb->sg_list, dest_cnt, sg_dest, dest_cnt, async_sg_to_sg_common_callback, cb, index);

    if (ret)
    {
        // free resource
        kfree(cb->sg_list);
        kfree(cb);
    }

    return ret;
}
EXPORT_SYMBOL(async_single_buf_to_sg_by_index);
#endif
#endif


MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Asynchronous Bulk Memory Transactions API");
MODULE_LICENSE("GPL");
