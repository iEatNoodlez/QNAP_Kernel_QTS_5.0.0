/*
 * Copyright (C) 2015 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */


#include "dm.h"

#include "dm-cache-manager.h"

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX   "cache manager"


struct entry_space_list {
	struct entry_space es;
	struct list_head list;
};

static struct entry_space_list *entry_space_list_create(unsigned nr_entries)
{
	int r;
	struct entry_space_list *esl;

	esl = kzalloc(sizeof(struct entry_space_list), GFP_KERNEL);
	if (!esl)
		return NULL;

	INIT_LIST_HEAD(&esl->list);
	r = es_init(&esl->es, nr_entries);
	if (r) {
		kfree(esl);
		return NULL;
	}

	return esl;
}

static void entry_space_list_destroy(struct entry_space_list *esl)
{
	list_del(&esl->list);
	es_exit(&esl->es);
	kfree(esl);
}

enum cache_usage_state {
	FULL = 0,
	NORMAL,
	FREE,
	NR_CACHE_USAGE_STATES,
};

unsigned cache_free_target[NR_CACHE_USAGE_STATES] = {
	25u,
	50u,
	75u,
};

struct entry_segment{
	unsigned shift;
	struct entry_alloc ea;
	struct entry_indexer ei;
};

struct manager {
	seqlock_t lock;
	dm_cblock_t cache_size;
	sector_t cache_block_size;

	struct cache_manager cm;

	unsigned nr_allocated;
	unsigned next_avail_seg;
	struct entry_segment segments[16];
	struct list_head entry_spaces;

	struct list_head wait_for_fb;
	struct rw_semaphore fb_lock;
	struct cache_feedback fb;

	struct workqueue_struct *wq;
	struct work_struct feeder;
	struct work_struct shrinker;
	enum cache_usage_state state;

	int register_clients;
};

static unsigned percent_to_target(struct manager *m, unsigned p)
{
	return from_cblock(m->cache_size) * p / 100u;
}

static void free_target_met(struct manager *m)
{
	enum cache_usage_state state;
	unsigned nr_free;

	nr_free = from_cblock(m->cache_size) - m->nr_allocated;

	for (state = FULL; state <= FREE; state++) {
		if (nr_free < percent_to_target(m, cache_free_target[state]))
			break;
	}

	if (m->state != state) {
		m->state = state;
		queue_work(m->wq, &m->feeder);
	}
}

static void take_entry_back(struct manager *m)
{
	queue_work(m->wq, &m->shrinker);
}

static struct manager *to_mgr(struct cache_manager *cm)
{
	return container_of(cm, struct manager, cm);
}

static void cm_destroy(struct cache_manager *cm)
{
	struct manager *m = to_mgr(cm);
	struct entry_space_list *esl, *tmp;

	destroy_workqueue(m->wq);
	list_for_each_entry_safe(esl, tmp, &m->entry_spaces, list)
		entry_space_list_destroy(esl);
	kfree(m);
}

static struct entry_alloc *seg_ea(struct manager *m, int i)
{
	return &((m->segments + i)->ea);
}

static struct entry *cm_alloc_entry(struct cache_manager *cm)
{
	struct manager *m = to_mgr(cm);
	unsigned long flags;
	struct entry *e = NULL;
	unsigned i;

	write_seqlock_irqsave(&m->lock, flags);
	for (i = 0; i < m->next_avail_seg; i++) {
		e = ea_alloc_entry(seg_ea(m, i));
		if (e) {
			BUG_ON(i >= 16);
			e->allocator = i;
			m->nr_allocated++;
			free_target_met(m);
			break;
		} else {
			take_entry_back(m);
		}
	}
	write_sequnlock_irqrestore(&m->lock, flags);

	return e;
}

static void cm_free_entry(struct cache_manager *cm, struct entry *e)
{
	struct manager *m = to_mgr(cm);
	unsigned long flags;

	if (!e)
		return;

	write_seqlock_irqsave(&m->lock, flags);
	ea_free_entry(seg_ea(m, e->allocator), e);
	m->nr_allocated--;
	free_target_met(m);
	write_sequnlock_irqrestore(&m->lock, flags);
}

static struct entry_segment *locate_segment(struct manager *m, unsigned index)
{
	unsigned i, j;

	for (i = 0, j = 1; i < m->next_avail_seg - 1; i++, j++) {
		struct entry_segment *cur_s, *next_s;

		cur_s  = m->segments + i;
		next_s = m->segments + j;
		if (index >= cur_s->shift && index < next_s->shift)
			break;
	}

	return m->segments + i;
}

static struct entry *cm_to_entry(struct cache_manager *cm, unsigned index)
{
	struct entry *e;
	struct manager *m = to_mgr(cm);
	struct entry_segment *seg;
	unsigned seq;

	if (index == INDEXER_NULL)
		return NULL;
retry:
	seq = read_seqbegin(&m->lock);
	seg = locate_segment(m, index);
	e = ea_get_entry(&seg->ea, index - seg->shift);
	if (read_seqretry(&m->lock, seq))
		goto retry;

	return e;
}

static unsigned cm_to_index(struct cache_manager *cm, struct entry *e)
{
	unsigned i;
	struct manager *m = to_mgr(cm);
	unsigned seq;

retry:
	seq = read_seqbegin(&m->lock);
	i = (m->segments + e->allocator)->shift;
	i += ea_get_index(seg_ea(m, e->allocator), e);
	if (read_seqretry(&m->lock, seq))
		goto retry;

	BUG_ON(i > m->cache_size);

	return i;
}

static int cm_resize(struct cache_manager *cm, dm_cblock_t expand_size)
{
	int r = 0;
	unsigned long flags;
	struct entry_space_list *esl;
	struct entry_segment *seg;
	struct manager *m = to_mgr(cm);

	esl = entry_space_list_create(from_cblock(expand_size));
	if (!esl)
		return -ENOMEM;

	write_seqlock_irqsave(&m->lock, flags);
	if (m->next_avail_seg == (1 << 4)) {
		entry_space_list_destroy(esl);
		r = -ENOMEM;
		goto fail;
	}

	list_add_tail(&esl->list, &m->entry_spaces);
	seg = m->segments + m->next_avail_seg;
	es_indexer_init(&seg->ei, &esl->es);
	ea_init(&seg->ea, &seg->ei, 0, from_cblock(expand_size));
	seg->shift = from_cblock(m->cache_size);
	m->next_avail_seg++;
	m->cache_size += expand_size;
fail:
	write_sequnlock_irqrestore(&m->lock, flags);
	return r;
}

static struct entry *cm_alloc_particular_entry(struct cache_manager *cm, unsigned i)
{
	struct manager *m = to_mgr(cm);
	struct entry *e;
	struct entry_segment *seg;
	unsigned long flags;

	write_seqlock_irqsave(&m->lock, flags);
	seg = locate_segment(m, i);
	e = ea_alloc_particular_entry(&seg->ea, i - seg->shift);
	write_sequnlock_irqrestore(&m->lock, flags);

	return e;
}

static int cm_register_feedback(struct cache_manager *cm, struct cache_feedback_channel *ch)
{
	struct manager *m = to_mgr(cm);

	down_write(&m->fb_lock);
	list_add_tail(&ch->list, &m->wait_for_fb);
	m->register_clients++;
	up_write(&m->fb_lock);

	return 0;
}

static void cm_unregister_feedback(struct cache_manager *cm, struct cache_feedback_channel *ch)
{
	struct manager *m = to_mgr(cm);

	down_write(&m->fb_lock);
	list_del(&ch->list);
	m->register_clients--;
	up_write(&m->fb_lock);
}

static dm_cblock_t cm_nr_allocated(struct cache_manager *cm)
{
	dm_cblock_t allocated = 0;
	struct manager *m = to_mgr(cm);
	unsigned seq;

retry:
	seq = read_seqbegin(&m->lock);
	allocated = m->nr_allocated;
	if (read_seqretry(&m->lock, seq))
		goto retry;

	return allocated;
}

static void cm_take_particular_entry(struct cache_manager *cm, unsigned i)
{
	struct manager *m = to_mgr(cm);
	struct entry_segment *seg;
	unsigned long flags;

	write_seqlock_irqsave(&m->lock, flags);
	seg = locate_segment(m, i);
	ea_take_particular_entry(&seg->ea, i - seg->shift);
	m->nr_allocated++;
	write_sequnlock_irqrestore(&m->lock, flags);
}

static bool cm_entry_taken(struct cache_manager *cm, unsigned i)
{
	struct manager *m = to_mgr(cm);
	struct entry_segment *seg;
	bool r;
	unsigned seq;

retry:
	seq = read_seqbegin(&m->lock);
	seg = locate_segment(m, i);
	r = ea_entry_taken(&seg->ea, i - seg->shift);
	if (read_seqretry(&m->lock, seq))
		goto retry;

	return r;
}

static dm_cblock_t cm_nr_blocks(struct cache_manager *cm)
{
	dm_cblock_t nr_blocks;
	struct manager *m = to_mgr(cm);
	unsigned seq;

retry:
	seq = read_seqbegin(&m->lock);
	nr_blocks = m->cache_size;
	if (read_seqretry(&m->lock, seq))
		goto retry;

	return nr_blocks;
}

static sector_t cm_block_size(struct cache_manager *cm)
{
	struct manager *m = to_mgr(cm);

	return m->cache_block_size;
}

static void init_functions(struct cache_manager *cm)
{
	cm->destroy = cm_destroy;
	cm->alloc_particular_entry = cm_alloc_particular_entry;
	cm->alloc_entry = cm_alloc_entry;
	cm->take_particular_entry = cm_take_particular_entry;
	cm->entry_taken = cm_entry_taken;
	cm->free_entry = cm_free_entry;
	cm->to_entry = cm_to_entry;
	cm->to_index = cm_to_index;
	cm->resize = cm_resize;
	cm->nr_allocated = cm_nr_allocated;
	cm->nr_blocks = cm_nr_blocks;
	cm->block_size = cm_block_size;
	cm->register_feedback = cm_register_feedback;
	cm->unregister_feedback = cm_unregister_feedback;
}

static void do_feedback(struct work_struct *ws)
{
	struct cache_feedback_channel *ch;
	struct manager *m = container_of(ws, struct manager, feeder);
	enum cache_usage_state state = m->state;

	switch (state) {
	case FULL:
		m->fb.escalate = false;
		m->fb.shrink_required = true;
		break;
	case FREE:
		m->fb.escalate = true;
		m->fb.shrink_required = false;
		break;
	default:
		m->fb.escalate = false;
		m->fb.shrink_required = false;
		break;
	};

	down_read(&m->fb_lock);
	list_for_each_entry(ch, &m->wait_for_fb, list) {
		ch->feedback(ch->private, &m->fb);
	}
	up_read(&m->fb_lock);
}

static void do_shrink(struct work_struct *ws)
{
	struct cache_feedback_channel *ch;
	struct manager *m = container_of(ws, struct manager, shrinker);
	enum cache_usage_state state = m->state;

	if (state != FULL)
		return;

	down_read(&m->fb_lock);
	list_for_each_entry(ch, &m->wait_for_fb, list)
			ch->shrink(ch->private, HIGH, m->register_clients);
	up_read(&m->fb_lock);
}

static struct entry *__cm_to_entry(void *context, unsigned index)
{
	return cm_to_entry((struct cache_manager *)context, index);
}

static unsigned __cm_to_index(void *context, struct entry *e)
{
	return cm_to_index((struct cache_manager *)context, e);
}

void cmgr_indexer_init(struct entry_indexer *i, struct cache_manager *cm)
{
	i->to_entry = __cm_to_entry;
	i->to_index = __cm_to_index;
	i->context  = cm;
}
EXPORT_SYMBOL(cmgr_indexer_init);

struct cache_manager *cmgr_create(dm_cblock_t cache_size, sector_t cache_block_size)
{
	struct manager *m;
	int r = -ENOMEM;

	m = kzalloc(sizeof(struct manager), GFP_KERNEL);
	if (!m)
		return NULL;

	INIT_WORK(&m->feeder, do_feedback);
	INIT_WORK(&m->shrinker, do_shrink);
	m->wq = alloc_ordered_workqueue("dm-" DM_MSG_PREFIX, WQ_MEM_RECLAIM);
	if (!m->wq)
		goto bad_wq;

	init_functions(&m->cm);

	init_rwsem(&m->fb_lock);
	INIT_LIST_HEAD(&m->wait_for_fb);
	INIT_LIST_HEAD(&m->entry_spaces);

	seqlock_init(&m->lock);
	m->cm.private = m;

	m->cache_block_size = cache_block_size;
	r = cm_resize(&m->cm, from_cblock(cache_size));
	if (r)
		goto bad_resize;

	m->register_clients = 0;

	return &m->cm;

bad_resize:
	destroy_workqueue(m->wq);
bad_wq:
	kfree(m);
	return NULL;
}

void cmgr_take_particular_entry(struct cache_manager *m, unsigned i)
{
	return m->take_particular_entry(m, i);
}
EXPORT_SYMBOL(cmgr_take_particular_entry);

bool cmgr_entry_taken(struct cache_manager *m, unsigned i)
{
	return m->entry_taken(m, i);
}
EXPORT_SYMBOL(cmgr_entry_taken);

struct entry *cmgr_alloc_particular_entry(struct cache_manager *m, unsigned i)
{
	return m->alloc_particular_entry(m, i);
}
EXPORT_SYMBOL(cmgr_alloc_particular_entry);

struct entry *cmgr_alloc_entry(struct cache_manager *m)
{
	return m->alloc_entry(m);
}
EXPORT_SYMBOL(cmgr_alloc_entry);

void cmgr_free_entry(struct cache_manager *m, struct entry *e)
{
	return m->free_entry(m, e);
}
EXPORT_SYMBOL(cmgr_free_entry);

struct entry *cmgr_to_entry(struct cache_manager *m, unsigned index)
{
	return m->to_entry(m, index);
}
EXPORT_SYMBOL(cmgr_to_entry);

unsigned cmgr_to_index(struct cache_manager *m, struct entry *e)
{
	return m->to_index(m, e);
}
EXPORT_SYMBOL(cmgr_to_index);

int cmgr_register_feedback(struct cache_manager *m, struct cache_feedback_channel *ch)
{
	return m->register_feedback(m, ch);
}
EXPORT_SYMBOL(cmgr_register_feedback);

void cmgr_unregister_feedback(struct cache_manager *m, struct cache_feedback_channel *ch)
{
	return m->unregister_feedback(m, ch);
}
EXPORT_SYMBOL(cmgr_unregister_feedback);

dm_cblock_t cmgr_nr_allocated(struct cache_manager *m)
{
	return m->nr_allocated(m);
}
EXPORT_SYMBOL(cmgr_nr_allocated);

dm_cblock_t cmgr_nr_blocks(struct cache_manager *m)
{
	return m->nr_blocks(m);
}
EXPORT_SYMBOL(cmgr_nr_blocks);

sector_t cmgr_block_size(struct cache_manager *m)
{
	return m->block_size(m);
}
EXPORT_SYMBOL(cmgr_block_size);

int cmgr_resize(struct cache_manager *m, dm_cblock_t expand_size)
{
	return m->resize(m, expand_size);
}
EXPORT_SYMBOL(cmgr_resize);

void cmgr_destroy(struct cache_manager *m)
{
	m->destroy(m);
}
EXPORT_SYMBOL(cmgr_destroy);
