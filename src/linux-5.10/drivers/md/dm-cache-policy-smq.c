/*
 * Copyright (C) 2015 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-background-tracker.h"
#include "dm-cache-policy-internal.h"
#include "dm-cache-policy.h"
#include "dm.h"

#include "dm-cache-policy-util.h"
#include "dm-cache-manager.h"

#include <linux/hash.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/rbtree.h>

#define DM_MSG_PREFIX "cache-policy-smq"

#define CACHE_HIT_FILTER_SIZE (1 << 23)
/*----------------------------------------------------------------*/

/*
 * Safe division functions that return zero on divide by zero.
 */
static unsigned safe_div(unsigned n, unsigned d)
{
	return d ? n / d : 0u;
}

static unsigned safe_mod(unsigned n, unsigned d)
{
	return d ? n % d : 0u;
}

/*----------------------------------------------------------------*/

/*
 * The stochastic-multi-queue is a set of lru lists stacked into levels.
 * Entries are moved up levels when they are used, which loosely orders the
 * most accessed entries in the top levels and least in the bottom.  This
 * structure is *much* better than a single lru list.
 */

#define MAX_LEVELS 64u

struct queue {
	struct entry_indexer *ei;

	unsigned nr_elts;
	unsigned nr_levels;

	struct ilist qs[MAX_LEVELS];

	/*
	 * We maintain a count of the number of entries we would like in each
	 * level.
	 */
	unsigned last_target_nr_elts;
	unsigned nr_top_levels;
	unsigned nr_in_top_levels;
	unsigned target_count[MAX_LEVELS];

	int q_tune:1;
};

static void q_init(struct queue *q, struct entry_indexer *ei, unsigned nr_levels,
		   int q_tune)
{
	unsigned i;

	q->ei = ei;
	q->nr_elts = 0;
	q->nr_levels = nr_levels;

	for (i = 0; i < q->nr_levels; i++) {
		l_init(q->qs + i);
		q->target_count[i] = 0u;
	}

	q->last_target_nr_elts = 0u;
	q->nr_top_levels = 0u;
	q->nr_in_top_levels = 0u;

	q->q_tune = q_tune;
}

static unsigned q_size(struct queue *q)
{
	return q->nr_elts;
}

/*
 * Insert an entry to the back of the given level.
 */
static void q_push(struct queue *q, struct entry *e)
{
	BUG_ON(e->pending);

	q->nr_elts++;
	l_add_tail(q->ei, q->qs + e->level, e);
}

static void q_push_front(struct queue *q, struct entry *e)
{
	BUG_ON(e->pending);

	q->nr_elts++;
	l_add_head(q->ei, q->qs + e->level, e);
}

static void q_push_before_sentinel(struct queue *q, bool last_sentinel, struct entry *e)
{
	BUG_ON(e->pending);

	q->nr_elts++;
	l_add_before_sentinel(q->ei, q->qs + e->level, last_sentinel, e);
}

static void q_del(struct queue *q, struct entry *e)
{
	l_del(q->ei, q->qs + e->level, e);
	q->nr_elts--;
}

static void q_splice(struct queue *q1, struct queue *q2)
{
	q1->nr_elts += q2->nr_elts;
	l_splice(q1->ei, q1->qs, q2->qs);
}

/*
 * Return the oldest entry of the lowest populated level.
 */
static struct entry *q_peek(struct queue *q, unsigned max_level, bool can_cross_sentinel)
{
	unsigned level;
	struct entry *e;

	max_level = min(max_level, q->nr_levels);

	for (level = 0; level < max_level; level++)
		for (e = l_head(q->ei, q->qs + level); e; e = l_next(q->ei, e)) {
			if (e->sentinel && !can_cross_sentinel)
				break;

			return e;
		}

	return NULL;
}

static struct entry *q_pop(struct queue *q)
{
	struct entry *e = q_peek(q, q->nr_levels, true);

	if (e)
		q_del(q, e);

	return e;
}

static void q_set_targets_subrange_(struct queue *q, unsigned nr_elts, unsigned lbegin, unsigned lend)
{
	unsigned level, nr_levels, entries_per_level, remainder;

	BUG_ON(lbegin > lend);
	BUG_ON(lend > q->nr_levels);
	nr_levels = lend - lbegin;
	entries_per_level = safe_div(nr_elts, nr_levels);
	remainder = safe_mod(nr_elts, nr_levels);

	for (level = lbegin; level < lend; level++)
		q->target_count[level] =
			(level < (lbegin + remainder)) ? entries_per_level + 1u : entries_per_level;
}

/*
 * Typically we have fewer elements in the top few levels which allows us
 * to adjust the promote threshold nicely.
 */
static void q_set_targets(struct queue *q)
{
	if (q->last_target_nr_elts == q->nr_elts)
		return;

	q->last_target_nr_elts = q->nr_elts;

	if (q->nr_top_levels > q->nr_levels)
		q_set_targets_subrange_(q, q->nr_elts, 0, q->nr_levels);

	else {
		q_set_targets_subrange_(q, q->nr_in_top_levels,
					q->nr_levels - q->nr_top_levels, q->nr_levels);

		if (q->nr_in_top_levels < q->nr_elts)
			q_set_targets_subrange_(q, q->nr_elts - q->nr_in_top_levels,
						0, q->nr_levels - q->nr_top_levels);
		else
			q_set_targets_subrange_(q, 0, 0, q->nr_levels - q->nr_top_levels);
	}
}

static void __redist_from_above(struct queue *q, unsigned level,
                                unsigned count, struct ilist *l)
{
	struct entry *e, *next;
	unsigned above, target = count;
	unsigned dest = q->q_tune? min(q->nr_levels, level + 4): q->nr_levels;

	for (above = level + 1u; above < dest; above++)
		for (e = l_head(q->ei, q->qs + above); e;) {
			next = l_next(q->ei, e);
			l_del(q->ei, q->qs + e->level, e);
			e->level = level;
			l_add_tail(q->ei, l, e);
			count--;
			if (!count)
				return;
			e = next;
		}

}

static void q_redistribute(struct queue *q)
{
	unsigned target, level;
	struct ilist *l, *l_above;
	struct entry *e;

	q_set_targets(q);

	for (level = 0u; level < q->nr_levels - 1u; level++) {
		l = q->qs + level;
		target = q->target_count[level];

		/*
		 * Pull down some entries from the level above.
		 */
		if (l->nr_elts < target)
			__redist_from_above(q, level, target - l->nr_elts, l);

		/*
		 * Push some entries up.
		 */
		l_above = q->qs + level + 1u;
		while (l->nr_elts > target) {
			e = l_pop_tail(q->ei, l);

			if (!e)
				/* bug in nr_elts */
				break;

			e->level = level + 1u;
			l_add_head(q->ei, l_above, e);
		}
	}
}

static void q_requeue(struct queue *q, struct entry *e, unsigned extra_levels, bool respect_sentinel)
{
	struct entry *de;
	unsigned new_level = min(q->nr_levels - 1u, e->level + extra_levels);
	int tuned = q->q_tune;

	/* try and find an entry to swap with */
	if (!tuned && extra_levels && (e->level < q->nr_levels - 1u)) {
		struct ilist *list = q->qs + new_level;

		de = l_head(q->ei, list);
		if (!de)
			goto out;

		q_del(q, de);
		de->level = e->level;
		if (respect_sentinel) {
			if (!de->sentinel)
				q_push_before_sentinel(q, true, de);
			else if (l_current_sentinel(q->ei, list) != de)
				q_push_before_sentinel(q, false, de);
			else
				q_push(q, de);
		} else
			q_push(q, de);
	}
out:
	q_del(q, e);
	e->level = new_level;
	q_push(q, e);
}

/* move all blocks to certain level */
static void q_migration(struct queue *q, unsigned level)
{
	struct ilist *l = q->qs + level;
	unsigned from;
	struct entry *e, *next;

	for (from = 0; from < q->nr_levels; from++) {
		if (from == level)
			continue;

		for (e = l_head(q->ei, q->qs + from); e;) {
			next = l_next(q->ei, e);
			l_del(q->ei, q->qs + e->level, e);
			e->level = level;
			l_add_tail(q->ei, l, e);
			e = next;
		}
	}
}

/*----------------------------------------------------------------*/

#define FP_SHIFT 8
#define SIXTEENTH (1u << (FP_SHIFT - 4u))
#define EIGHTH (1u << (FP_SHIFT - 3u))
#define QUARTER (1u << (FP_SHIFT - 2u))
#define HALF (1u << (FP_SHIFT - 1u))

struct stats {
	unsigned hit_threshold;
	unsigned hits;
	unsigned misses;
	unsigned no_rooms;
};

enum performance {
	Q_POOR,
	Q_FAIR,
	Q_WELL
};

static void stats_init(struct stats *s, unsigned nr_levels)
{
	s->hit_threshold = (nr_levels * 3u) / 4u;
	s->hits = 0u;
	s->misses = 0u;
	s->no_rooms = 0u;
}

static void stats_reset(struct stats *s)
{
	s->hits = s->misses = s->no_rooms = 0u;
}

static void stats_level_accessed(struct stats *s, unsigned level)
{
	if (level >= s->hit_threshold)
		s->hits++;
	else
		s->misses++;
}

static void stats_miss(struct stats *s)
{
	s->misses++;
}

static void stats_no_room(struct stats *s)
{
	s->no_rooms++;
}

/*
 * There are times when we don't have any confidence in the hotspot queue.
 * Such as when a fresh cache is created and the blocks have been spread
 * out across the levels, or if an io load changes.  We detect this by
 * seeing how often a lookup is in the top levels of the hotspot queue.
 */
static enum performance stats_assess(struct stats *s)
{
	unsigned confidence = safe_div(s->hits << FP_SHIFT, s->hits + s->misses);

	if (confidence < SIXTEENTH)
		return Q_POOR;

	else if (confidence < EIGHTH)
		return Q_FAIR;

	else
		return Q_WELL;
}

/*----------------------------------------------------------------*/

struct smq_hash_table {
	struct entry_indexer *ei;
	struct smq_policy *mq;
	unsigned long long hash_bits;
	unsigned *buckets;
};

/*
 * All cache entries are stored in a chained hash table.  To save space we
 * use indexing again, and only store indexes to the next entry.
 */
static int h_init(struct smq_hash_table *ht, struct entry_indexer *ei, unsigned nr_entries)
{
	unsigned i, nr_buckets;

	ht->ei = ei;
	nr_buckets = roundup_pow_of_two(max(nr_entries / 4u, 16u));
	ht->hash_bits = __ffs(nr_buckets);

	ht->buckets = vmalloc(array_size(nr_buckets, sizeof(*ht->buckets)));
	if (!ht->buckets)
		return -ENOMEM;

	for (i = 0; i < nr_buckets; i++)
		ht->buckets[i] = INDEXER_NULL;

	return 0;
}

static void h_exit(struct smq_hash_table *ht)
{
	vfree(ht->buckets);
}

static struct entry *h_head(struct smq_hash_table *ht, unsigned bucket)
{
	return indexer_to_entry(ht->ei, ht->buckets[bucket]);
}

static struct entry *h_next(struct smq_hash_table *ht, struct entry *e)
{
	return indexer_to_entry(ht->ei, e->hash_next);
}

static void __h_insert(struct smq_hash_table *ht, unsigned bucket, struct entry *e)
{
	e->hash_next = ht->buckets[bucket];
	ht->buckets[bucket] = indexer_to_index(ht->ei, e);
}

static void h_insert(struct smq_hash_table *ht, struct entry *e)
{
	unsigned h = hash_64(from_oblock(e->oblock), ht->hash_bits);

	BUG_ON(e->hashed);
	__h_insert(ht, h, e);
	e->hashed = true;
}

static struct entry *__h_lookup(struct smq_hash_table *ht, unsigned h, dm_oblock_t oblock,
				struct entry **prev)
{
	struct entry *e;

	*prev = NULL;
	for (e = h_head(ht, h); e; e = h_next(ht, e)) {
		if (e->oblock == oblock)
			return e;

		*prev = e;
	}

	return NULL;
}

static void __h_unlink(struct smq_hash_table *ht, unsigned h,
		       struct entry *e, struct entry *prev)
{
	if (prev)
		prev->hash_next = e->hash_next;
	else
		ht->buckets[h] = e->hash_next;
}

/*
 * Also moves each entry to the front of the bucket.
 */
static struct entry *h_lookup(struct smq_hash_table *ht, dm_oblock_t oblock)
{
	struct entry *e, *prev;
	unsigned h = hash_64(from_oblock(oblock), ht->hash_bits);

	e = __h_lookup(ht, h, oblock, &prev);
	if (e && prev) {
		/*
		 * Move to the front because this entry is likely
		 * to be hit again.
		 */
		__h_unlink(ht, h, e, prev);
		__h_insert(ht, h, e);
	}

	return e;
}

static void h_remove(struct smq_hash_table *ht, struct entry *e)
{
	unsigned h = hash_64(from_oblock(e->oblock), ht->hash_bits);
	struct entry *prev;

	/*
	 * The down side of using a singly linked list is we have to
	 * iterate the bucket to remove an item.
	 */
	e = __h_lookup(ht, h, e->oblock, &prev);
	if (e) {
		__h_unlink(ht, h, e, prev);
		e->hashed = false;
	}
}

/*----------------------------------------------------------------*/

#define NR_HOTSPOT_LEVELS 64u
#define NR_CACHE_LEVELS 64u

#define WRITEBACK_PERIOD (10ul * HZ)
#define DEMOTE_PERIOD (60ul * HZ)

#define HOTSPOT_UPDATE_PERIOD (HZ)
#define CACHE_UPDATE_PERIOD (60ul * HZ)

struct inval_entry {
	struct rb_node rb_node;
	dm_oblock_t oblock;
};

struct smq_policy {
	struct dm_cache_policy policy;

	/* protects everything */
	spinlock_t lock;

	sector_t hotspot_block_size;
	unsigned nr_hotspot_blocks;
	unsigned cache_blocks_per_hotspot_block;
	unsigned hotspot_level_jump;

	struct entry_space hotspot_es;
	struct entry_alloc hotspot_alloc;
	struct entry_indexer hotspot_indexer;

	unsigned long *hotspot_hit_bits;
	unsigned long *cache_hit_bits;

	/*
	 * We maintain three queues of entries.  The cache proper,
	 * consisting of a clean and dirty queue, containing the currently
	 * active mappings.  The hotspot queue uses a larger block size to
	 * track blocks that are being hit frequently and potential
	 * candidates for promotion to the cache.
	 */
	struct queue hotspot;
	struct queue clean;
	struct queue dirty;
	struct queue sort;
	struct rb_root sort_list;
	struct inval_entry *es_begin;
	struct inval_entry *es_end;
	struct entry_indexer cache_indexer;

	struct stats hotspot_stats;
	struct stats cache_stats;

	/*
	 * Keeps track of time, incremented by the core.  We use this to
	 * avoid attributing multiple hits within the same tick.
	 */
	unsigned tick;

	/*
	 * The hash tables allows us to quickly find an entry by origin
	 * block.
	 */
	struct smq_hash_table table;
	struct smq_hash_table hotspot_table;

	unsigned long next_writeback_period;
	unsigned long next_demote_period;

	unsigned write_promote_level;
	unsigned read_promote_level;

	unsigned long next_hotspot_period;
	unsigned long next_cache_period;

	struct background_tracker *bg_work;

	bool migrations_allowed;

	int migration_dir;
	int q_tune; /* for tuned policy */

	struct cache_manager *manager;
	struct cache_feedback_channel ch;

	int promote_more:1;
	int shrink_required:1;
	atomic_t suspended;

	int low_dirty_thresh;
	int high_dirty_thresh;

	int demote_entries;

};

/*----------------------------------------------------------------*/

enum direction {
	UPWARD,
	NONE,
	DOWNWARD,
	FORCE_DOWNWARD,
};

static inline int always_upward(struct smq_policy *mq)
{
	return mq->migration_dir == UPWARD;
}

static inline int always_downward(struct smq_policy *mq)
{
	return mq->migration_dir >= DOWNWARD;
}

static inline int force_downward(struct smq_policy *mq)
{
	return mq->migration_dir == FORCE_DOWNWARD;
}

/*----------------------------------------------------------------*/

static void __update_sentinels(struct queue *q)
{
	unsigned level;

	for (level = 0; level < q->nr_levels; level++)
		l_update_sentinel(q->ei, q->qs + level);
}

static void update_sentinels(struct smq_policy *mq)
{
	if (time_after(jiffies, mq->next_writeback_period)) {
		mq->next_writeback_period = jiffies + WRITEBACK_PERIOD;
		__update_sentinels(&mq->clean);
	}

	if (time_after(jiffies, mq->next_demote_period)) {
		mq->next_demote_period = jiffies + DEMOTE_PERIOD;
		__update_sentinels(&mq->dirty);
	}
}

/*----------------------------------------------------------------*/

static void del_queue(struct smq_policy *mq, struct entry *e)
{
	q_del(e->dirty ? &mq->dirty : &mq->clean, e);
}

static void __push_queue(struct smq_policy *mq, struct entry *e)
{
	if (e->dirty)
		q_push(&mq->dirty, e);
	else
		q_push(&mq->clean, e);
}

static void push_queue(struct smq_policy *mq, struct entry *e)
{
	if (!e->pending)
		__push_queue(mq, e);
}

// !h, !q, a -> h, q, a
static void push(struct smq_policy *mq, struct entry *e)
{
	if (!e->hashed)
		h_insert(&mq->table, e);
	if (!e->pending)
		push_queue(mq, e);
}

static void __push_queue_front(struct smq_policy *mq, struct entry *e)
{
	if (e->dirty)
		q_push_front(&mq->dirty, e);
	else
		q_push_front(&mq->clean, e);
}

static void push_front(struct smq_policy *mq, struct entry *e)
{
	if (!e->hashed)
		h_insert(&mq->table, e);
	if (!e->pending)
		__push_queue_front(mq, e);
}

static void requeue(struct smq_policy *mq, struct entry *e)
{
	/*
	 * Pending work has temporarily been taken out of the queues.
	 */
	if (e->pending)
		return;

	if (mq->q_tune ||
	    !test_and_set_bit(cmgr_to_index(mq->manager, e), mq->cache_hit_bits)) {
		if (!e->dirty) {
			q_requeue(&mq->clean, e, 5u, false);
			return;
		}

		q_requeue(&mq->dirty, e, 1u, true);
	}
}

/*----------------------------------------------------------------*/

static bool clean_target_met(struct smq_policy *mq, bool idle)
{
	if (force_downward(mq))
		return true;

	/*
	 * Cache entries may not be populated.  So we cannot rely on the
	 * size of the clean queue.
	 */
	if (idle) {
		/*
		 * We'd like to clean everything.
		 */
		return q_size(&mq->dirty) == 0u;
	}

	/*
	 * If we're busy we don't worry about cleaning at all.
	 */
	return true;
}

static bool has_residents(struct smq_policy *mq)
{
	return !!(q_size(&mq->dirty) + q_size(&mq->clean));
}

/*----------------------------------------------------------------*/

static void inc_pending(struct smq_policy *mq, struct entry *e)
{
	BUG_ON(!e->allocated);
	BUG_ON(e->pending == ((1 << 13) - 1));
	e->pending++;
}

static bool dec_pending(struct smq_policy *mq, struct entry *e)
{
	BUG_ON(!e->pending);
	e->pending--;

	return e->pending == 0;
}

static inline dm_cblock_t infer_cblock(struct smq_policy *mq, struct entry *e)
{
	return to_cblock(cmgr_to_index(mq->manager, e));
}

static void __queue_writeback_single(struct smq_policy *mq, bool idle, bool all)
{
	int r;
	struct policy_work work;
	struct entry *e;

	do {
		e = q_peek(&mq->dirty, mq->dirty.nr_levels, idle);
		if (e) {
			inc_pending(mq, e);
			q_del(&mq->dirty, e);

			work.op = always_downward(mq)? POLICY_WRITEBACK_F: POLICY_WRITEBACK;
			work.oblock = e->oblock;
			work.cblock = infer_cblock(mq, e);


			r = btracker_queue(mq->bg_work, &work, NULL);
			if (r) {
				if (dec_pending(mq, e))
					q_push_front(&mq->dirty, e);
			}
		}
	} while (all && e && !r);
}

static void __queue_writeback_all(struct smq_policy *mq)
{
	__queue_writeback_single(mq, true, true);
}

static void queue_writeback(struct smq_policy *mq, bool idle)
{
	if (unlikely(always_downward(mq)))
		__queue_writeback_all(mq);
	else
		__queue_writeback_single(mq, idle, false);
}

static void demote_entry_cal(struct smq_policy *mq)
{
	struct stats *s = &mq->cache_stats;
	unsigned missing = safe_div(s->no_rooms << FP_SHIFT, s->hits + s->misses);

	if (missing < SIXTEENTH)
		/* good, decrease demote count */
		mq->demote_entries = mq->demote_entries >> 1;

	else if (missing < EIGHTH)
		/* fair, keep it */
		mq->demote_entries = (mq->demote_entries - 4) > 0? (mq->demote_entries - 4): 1;

	else if (missing < QUARTER)
		/* a bit high, increase a little */
		mq->demote_entries = mq->demote_entries + 8;

	else if (missing < HALF)
		/* high, double it */
		mq->demote_entries = mq->demote_entries << 1;

	else
		/* too high, quadruple */
		mq->demote_entries = mq->demote_entries << 2;
}

static void force_clean(struct smq_policy *mq)
{
	struct entry *e;

	e = q_peek(&mq->dirty, mq->dirty.nr_levels, true);
	if (!e)
		return;

	do {
		inc_pending(mq, e);
		q_del(&mq->dirty, e);
		e->dirty = false;
		BUG_ON(!dec_pending(mq, e));
		push_queue(mq, e);
		e = q_peek(&mq->dirty, mq->dirty.nr_levels, true);
	} while (e);

}

static void queue_demotion(struct smq_policy *mq, bool force)
{
	int r;
	struct policy_work work;
	struct entry *e;
	int demote_entries;
	unsigned max_level = always_downward(mq)? mq->clean.nr_levels: mq->clean.nr_levels / 2;

	if ((!mq->shrink_required && !force) ||
	    WARN_ON_ONCE(!mq->migrations_allowed && !always_downward(mq)))
		return;

	if (force_downward(mq))
		force_clean(mq);

	demote_entry_cal(mq);
	demote_entries = min((unsigned)mq->demote_entries, q_size(&mq->clean))? : 1;

	while (demote_entries) {
		e = q_peek(&mq->clean, max_level, true);
		if (!e) {
			if (!clean_target_met(mq, true)) {
				atomic_inc(&mq->policy.stat.force_cleaning);
				queue_writeback(mq, force);
			}
			break;
		}

		atomic_inc(&mq->policy.stat.reclaim);
		inc_pending(mq, e);
		q_del(&mq->clean, e);

		work.op = force_downward(mq)? POLICY_ABANDON: POLICY_DEMOTE;
		work.oblock = e->oblock;
		work.cblock = infer_cblock(mq, e);

		r = btracker_queue(mq->bg_work, &work, NULL);
		if (r && dec_pending(mq, e) )
			q_push_front(&mq->clean, e);

		demote_entries--;
	}

	mq->shrink_required = false;
}

static int queue_promotion(struct smq_policy *mq, dm_oblock_t oblock,
			   struct policy_work **workp)
{
	struct entry *e;
	struct policy_work work;

	if (!mq->migrations_allowed)
		return -ENOENT;

	if (btracker_promotion_already_present(mq->bg_work, oblock))
		return workp ? -EBUSY : -ENOENT;

	e = cmgr_alloc_entry(mq->manager);
	if (!e) {
		/*
		 * We always claim to be 'idle' to ensure some 
		 * demotions happen with continuous loads.
		*/
		atomic_inc(&mq->policy.stat.no_room);
		stats_no_room(&mq->cache_stats);
		return -ENOENT;
	}
	atomic_inc(&mq->policy.stat.callocated);

	inc_pending(mq, e);
	work.op = POLICY_PROMOTE;
	work.oblock = oblock;
	work.cblock = infer_cblock(mq, e);
	if (btracker_queue(mq->bg_work, &work, workp)) {
		cmgr_free_entry(mq->manager, e);
		atomic_dec(&mq->policy.stat.callocated);
	}

	return -ENOENT;
}


static unsigned default_promote_level(struct smq_policy *mq)
{
	/*
	 * The promote level depends on the current performance of the
	 * cache.
	 *
	 * If the cache is performing badly, then we can't afford
	 * to promote much without causing performance to drop below that
	 * of the origin device.
	 *
	 * If the cache is performing well, then we don't need to promote
	 * much.  If it isn't broken, don't fix it.
	 *
	 * If the cache is middling then we promote more.
	 *
	 * This scheme reminds me of a graph of entropy vs probability of a
	 * binary variable.
	 */
	static unsigned table[] = {1, 1, 1, 2, 4, 6, 7, 8, 7, 6, 4, 4, 3, 3, 2, 2, 1};

	unsigned hits = mq->cache_stats.hits;
	unsigned misses = mq->cache_stats.misses;
	unsigned index = safe_div(hits << 4u, hits + misses);
	return table[index];
}

static void update_promote_levels(struct smq_policy *mq)
{
	/*
	 * If there are unused cache entries then we want to be really
	 * eager to promote.
	 */
	unsigned threshold_level = mq->promote_more ?
		(NR_HOTSPOT_LEVELS / 2u) : default_promote_level(mq);

	threshold_level = max(threshold_level, NR_HOTSPOT_LEVELS);

	/*
	 * If the hotspot queue is performing badly then we have little
	 * confidence that we know which blocks to promote.  So we cut down
	 * the amount of promotions.
	 */
	switch (stats_assess(&mq->hotspot_stats)) {
	case Q_POOR:
		threshold_level /= 4u;
		break;

	case Q_FAIR:
		threshold_level /= 2u;
		break;

	case Q_WELL:
		break;
	}

	mq->read_promote_level = NR_HOTSPOT_LEVELS - threshold_level;
	mq->write_promote_level = (NR_HOTSPOT_LEVELS - threshold_level);
}

/*
 * If the hotspot queue is performing badly, then we try and move entries
 * around more quickly.
 */
static void update_level_jump(struct smq_policy *mq)
{
	switch (stats_assess(&mq->hotspot_stats)) {
	case Q_POOR:
		mq->hotspot_level_jump = 1u;
		break;

	case Q_FAIR:
		mq->hotspot_level_jump = 2u;
		break;

	case Q_WELL:
		mq->hotspot_level_jump = 4u;
		break;
	}
}

static void end_hotspot_period(struct smq_policy *mq)
{
	clear_bitset(mq->hotspot_hit_bits, mq->nr_hotspot_blocks);
	update_promote_levels(mq);

	if (time_after(jiffies, mq->next_hotspot_period)) {
		update_level_jump(mq);
		q_redistribute(&mq->hotspot);
		stats_reset(&mq->hotspot_stats);
		mq->next_hotspot_period = jiffies + HOTSPOT_UPDATE_PERIOD;
	}
}

static void end_cache_period(struct smq_policy *mq)
{
	if (time_after(jiffies, mq->next_cache_period)) {
		clear_bitset(mq->cache_hit_bits, CACHE_HIT_FILTER_SIZE);

		q_redistribute(&mq->dirty);
		q_redistribute(&mq->clean);
		stats_reset(&mq->cache_stats);
		queue_demotion(mq, false);
		mq->next_cache_period = jiffies + CACHE_UPDATE_PERIOD;
	}
}

/*----------------------------------------------------------------*/

enum promote_result {
	PROMOTE_NOT,
	PROMOTE_TEMPORARY,
	PROMOTE_PERMANENT
};

/*
 * Converts a boolean into a promote result.
 */
static enum promote_result maybe_promote(bool promote)
{
	return promote ? PROMOTE_PERMANENT : PROMOTE_NOT;
}

static enum promote_result should_promote(struct smq_policy *mq, struct entry *hs_e,
					  int data_dir, bool fast_promote)
{
	if (data_dir == WRITE) {
		if (mq->promote_more && fast_promote)
			return PROMOTE_TEMPORARY;

		return maybe_promote(hs_e->level >= mq->write_promote_level);
	} else
		return maybe_promote(hs_e->level >= mq->read_promote_level);
}

static dm_oblock_t to_hblock(struct smq_policy *mq, dm_oblock_t b)
{
	sector_t r = from_oblock(b);
	(void) sector_div(r, mq->cache_blocks_per_hotspot_block);
	return to_oblock(r);
}

static void update_hit_hotspot_queue(struct smq_policy *mq, dm_oblock_t b)
{
	dm_oblock_t hb = to_hblock(mq, b);
	struct entry *e = h_lookup(&mq->hotspot_table, hb);

	/* update hot spot queue on hit only for tuned policy */
	if (!mq->q_tune || !e)
		return;

	q_requeue(&mq->hotspot, e, mq->hotspot_level_jump, false);
}

static struct entry *update_miss_hotspot_queue(struct smq_policy *mq, dm_oblock_t b)
{
	unsigned hi;
	dm_oblock_t hb = to_hblock(mq, b);
	struct entry *e = h_lookup(&mq->hotspot_table, hb);

	if (e) {
		stats_level_accessed(&mq->hotspot_stats, e->level);

		hi = ea_get_index(&mq->hotspot_alloc, e);
		q_requeue(&mq->hotspot, e,
			  test_and_set_bit(hi, mq->hotspot_hit_bits) ?
			  0u : mq->hotspot_level_jump,
			  false);

	} else {
		stats_miss(&mq->hotspot_stats);

		e = ea_alloc_entry(&mq->hotspot_alloc);
		if (!e) {
			e = q_pop(&mq->hotspot);
			if (e) {
				h_remove(&mq->hotspot_table, e);
				hi = ea_get_index(&mq->hotspot_alloc, e);
				clear_bit(hi, mq->hotspot_hit_bits);
			}

		}

		if (e) {
			e->oblock = hb;
			q_push(&mq->hotspot, e);
			h_insert(&mq->hotspot_table, e);
		}
	}

	return e;
}

/*----------------------------------------------------------------*/

/*
 * Public interface, via the policy struct.  See dm-cache-policy.h for a
 * description of these.
 */

static struct smq_policy *to_smq_policy(struct dm_cache_policy *p)
{
	return container_of(p, struct smq_policy, policy);
}

static void smq_destroy(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);

	btracker_destroy(mq->bg_work);
	h_exit(&mq->hotspot_table);
	h_exit(&mq->table);
	free_bitset(mq->hotspot_hit_bits);
	free_bitset(mq->cache_hit_bits);
	es_exit(&mq->hotspot_es);
	kfree(mq);
}

static void inval_destroy(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);

	btracker_destroy(mq->bg_work);
	h_exit(&mq->table);
	kfree(mq);
}

/*----------------------------------------------------------------*/

static int __lookup_smq(struct smq_policy *mq, dm_oblock_t oblock,
			dm_cblock_t *cblock, int data_dir, bool fast_copy,
			struct policy_work **work, bool queue, bool *background_work)
{
	struct entry *e, *hs_e;
	enum promote_result pr;

	*background_work = false;

	e = h_lookup(&mq->table, oblock);
	if (e) {
		stats_level_accessed(&mq->cache_stats, e->level);
		update_hit_hotspot_queue(mq, oblock);

		requeue(mq, e);
		*cblock = infer_cblock(mq, e);
		return 0;
	}

	stats_miss(&mq->cache_stats);

	if (unlikely(!queue))
		return -ENOENT;
	/*
	 * The hotspot queue only gets updated with misses.
	 */
	hs_e = update_miss_hotspot_queue(mq, oblock);

	pr = always_upward(mq)? PROMOTE_PERMANENT:
				should_promote(mq, hs_e, data_dir, fast_copy);
	if (pr != PROMOTE_NOT) {
		*background_work = true;
		return queue_promotion(mq, oblock, work);
	}

	return -ENOENT;
}

static int __lookup_inval(struct smq_policy *mq, dm_oblock_t oblock,
			  dm_cblock_t *cblock, bool *background_work)
{
	struct entry *e;

	*background_work = false;

	e = h_lookup(&mq->table, oblock);
	if (e) {
		stats_level_accessed(&mq->cache_stats, e->level);

		*cblock = infer_cblock(mq, e);
		return 0;
	}

	stats_miss(&mq->cache_stats);

	return -ENOENT;
}

static int __lookup(struct smq_policy *mq, dm_oblock_t oblock,
		    dm_cblock_t *cblock, int data_dir, bool fast_copy,
		    struct policy_work **work, bool queue, bool *background_work)
{
	WARN_ON(atomic_read(&mq->suspended));

	return always_downward(mq)? __lookup_inval(mq, oblock, cblock, background_work):
				    __lookup_smq(mq, oblock, cblock, data_dir, fast_copy,
						 work, queue, background_work);
}

static int smq_lookup(struct dm_cache_policy *p, dm_oblock_t oblock,
                      dm_cblock_t *cblock, int data_dir, bool fast_copy,
                      bool *background_work)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = __lookup(mq, oblock, cblock,
		     data_dir, fast_copy,
		     NULL, true, background_work);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static int smq_lookup_with_work(struct dm_cache_policy *p,
                                dm_oblock_t oblock,
				dm_cblock_t *cblock,
				int data_dir, bool fast_copy,
				struct policy_work **work)
{
	int r;
	bool background_queued;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = __lookup(mq, oblock, cblock, data_dir,
	            fast_copy, work, true, &background_queued);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static int smq_lookup_simple(struct dm_cache_policy *p,
			     dm_oblock_t oblock,
			     dm_cblock_t *cblock)
{
	int r;
	bool background_queued;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = __lookup(mq, oblock, cblock, 0,
		     0, NULL, false, &background_queued);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static struct inval_entry *get_inval_entry(struct smq_policy *mq, unsigned index)
{
	struct inval_entry *ie;

	BUG_ON(index == INDEXER_NULL);

	ie = mq->es_begin + index;
	BUG_ON(ie >= mq->es_end);

	return ie;
}

static struct entry *node_to_entry(struct smq_policy *mq, struct inval_entry *ie)
{
	unsigned index;

	BUG_ON(ie < mq->es_begin || ie >= mq->es_end);

	index = ie - mq->es_begin;

	return cmgr_to_entry(mq->manager, from_cblock(index));
}

#define get_entry(node) rb_entry((node), struct inval_entry, rb_node)

static void entry_rb_add(struct smq_policy *mq, struct inval_entry *new)
{
	struct rb_node **rentry, *parent;
	struct inval_entry *e;
	dm_oblock_t oblock = new->oblock;

	rentry = &mq->sort_list.rb_node;
	parent = NULL;
	while (*rentry) {
		parent = *rentry;
		e = get_entry(parent);

		if (oblock < e->oblock)
			rentry = &(*rentry)->rb_left;
		else
			rentry = &(*rentry)->rb_right;
	}

	rb_link_node(&new->rb_node, parent, rentry);
	rb_insert_color(&new->rb_node, &mq->sort_list);
}

static void extracted_sorted_entry(struct smq_policy *mq)
{
	struct rb_node *node;
	struct inval_entry *ie;
	struct entry *e;
	struct queue q;
	unsigned long flags;

	q_init(&q, &mq->cache_indexer, 1, 0);

	for (node = rb_first(&mq->sort_list); node; node = rb_next(node)) {
		ie = get_entry(node);
		e = node_to_entry(mq, ie);
		e->level = 0;

		q_push(&q, e);
		rb_erase(&ie->rb_node, &mq->sort_list);
	}

	WARN_ON(!RB_EMPTY_ROOT(&mq->sort_list));

	spin_lock_irqsave(&mq->lock, flags);
	q_splice(&mq->dirty, &q);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void inval_sort_mapping(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);

	extracted_sorted_entry(mq);
	vfree(mq->es_begin);
}

static int smq_get_background_work(struct dm_cache_policy *p, bool idle,
				   struct policy_work **result)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);

	r = btracker_issue(mq->bg_work, result);
	if (r == -ENODATA) {
		if (!clean_target_met(mq, idle)) {
			atomic_inc(&p->stat.periodic_cleaning);
			queue_writeback(mq, idle);
			r = btracker_issue(mq->bg_work, result);
		} else if (always_downward(mq) && has_residents(mq)) {
			queue_demotion(mq, true);
			r = btracker_issue(mq->bg_work, result);
		}
	}
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static unsigned get_promote_level(struct smq_policy *mq, dm_oblock_t b)
{
	dm_oblock_t hb = to_hblock(mq, b);
	struct entry *e = h_lookup(&mq->hotspot_table, hb);

	if (!mq->q_tune || (e && e->level >= mq->read_promote_level))
		return NR_CACHE_LEVELS - 1;

	return NR_CACHE_LEVELS >> 1;
}

/*
 * We need to clear any pending work flags that have been set, and in the
 * case of promotion free the entry for the destination cblock.
 */
static void __complete_background_work(struct smq_policy *mq,
				       struct policy_work *work,
				       bool success)
{
	struct entry *e = cmgr_to_entry(mq->manager,
	                                from_cblock(work->cblock));
	BUG_ON(!dec_pending(mq, e));

	switch (work->op) {
	case POLICY_PROMOTE:
		// !h, !q, a

		if (success) {
			e->oblock = work->oblock;
			e->level = get_promote_level(mq, e->oblock);
			push(mq, e);
			// h, q, a
		} else {
			cmgr_free_entry(mq->manager, e);
			atomic_dec(&mq->policy.stat.callocated);
			// !h, !q, !a
		}
		break;

	case POLICY_ABANDON:
	case POLICY_DEMOTE:
		// h, !q, a
		if (success) {
			h_remove(&mq->table, e);
			cmgr_free_entry(mq->manager, e);
			atomic_dec(&mq->policy.stat.callocated);
			// !h, !q, !a
		} else {
			push_queue(mq, e);
			atomic_inc(&mq->policy.stat.requeue);
			// h, q, a
		}
		break;

	case POLICY_WRITEBACK:
	case POLICY_WRITEBACK_F:
		// h, !q, a
		push_queue(mq, e);
		if (!success)
			atomic_inc(&mq->policy.stat.requeue);
		// h, q, a
		break;
	}

	btracker_complete(mq->bg_work, work);
}

static void smq_complete_background_work(struct dm_cache_policy *p,
					 struct policy_work *work,
					 bool success)
{
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	__complete_background_work(mq, work, success);
	spin_unlock_irqrestore(&mq->lock, flags);
}

// in_hash(oblock) -> in_hash(oblock)
static void __smq_set_clear_dirty(struct smq_policy *mq, dm_cblock_t cblock,
				  bool set)
{
	struct entry *e = cmgr_to_entry(mq->manager, from_cblock(cblock));

	if (e->pending) {
		e->dirty = set;
	} else {
		del_queue(mq, e);
		e->dirty = set;
		push_queue(mq, e);
	}
}

static void smq_set_dirty(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	__smq_set_clear_dirty(mq, cblock, true);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void smq_clear_dirty(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);
	__smq_set_clear_dirty(mq, cblock, false);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static unsigned random_level(dm_cblock_t cblock)
{
	return hash_32(from_cblock(cblock), 9) & (NR_CACHE_LEVELS - 1);
}

static int smq_load_mapping(struct dm_cache_policy *p,
			    dm_oblock_t oblock, dm_cblock_t cblock,
			    bool dirty, uint32_t hint, bool hint_valid)
{
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e;
	struct inval_entry *ie;

	e = cmgr_alloc_particular_entry(mq->manager, from_cblock(cblock));
	atomic_inc(&mq->policy.stat.callocated);
	e->oblock = oblock;
	e->dirty = dirty;
	e->level = (always_downward(mq) && dirty)? 0 : hint_valid ? min(hint, NR_CACHE_LEVELS - 1) : random_level(cblock);
	e->pending = 0;

	/*
	 * When we load mappings we push ahead of both sentinels in order to
	 * allow demotions and cleaning to occur immediately.
	 */

	if (always_downward(mq) && dirty) {
		ie = get_inval_entry(mq, from_cblock(cblock));
		ie->oblock = oblock;

		WARN_ON(e->hashed);
		if (!e->hashed)
			h_insert(&mq->table, e);
		entry_rb_add(mq, ie);
	} else {
		push_front(mq, e);
	}

	return 0;
}

static int smq_invalidate_mapping(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e = cmgr_to_entry(mq->manager, from_cblock(cblock));

	if (!e->allocated)
		return -ENODATA;

	// FIXME: what if this block has pending background work?
	atomic_inc(&p->stat.invalidate);
	del_queue(mq, e);
	h_remove(&mq->table, e);
	cmgr_free_entry(mq->manager, e);
	atomic_dec(&mq->policy.stat.callocated);
	return 0;
}

static uint32_t smq_get_hint(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e = cmgr_to_entry(mq->manager, from_cblock(cblock));

	if (!e->allocated)
		return 0;

	return e->level;
}

static dm_cblock_t smq_residency(struct dm_cache_policy *p)
{
	dm_cblock_t r;
	struct smq_policy *mq = to_smq_policy(p);

	r = to_cblock(atomic_read(&mq->policy.stat.callocated));

	return r;
}

static void smq_tick(struct dm_cache_policy *p, bool can_block)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags); // FIXME
	mq->tick++;
	update_sentinels(mq);
	end_hotspot_period(mq);
	end_cache_period(mq);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static int smq_feedback(void *context, struct cache_feedback *fb)
{
	unsigned long flags;
	struct smq_policy *mq = context;

	spin_lock_irqsave(&mq->lock, flags);
	mq->promote_more = fb->escalate;
	mq->shrink_required = fb->shrink_required;
	spin_unlock_irqrestore(&mq->lock, flags);

	return 0;
}

static void smq_shrink(void *context, enum water_level level, int nr_neighbors)
{
	unsigned long flags;
	struct smq_policy *mq = context;

	spin_lock_irqsave(&mq->lock, flags);
	if (atomic_read(&mq->policy.stat.callocated) <
	    (cmgr_nr_blocks(mq->manager) / nr_neighbors)) {
		spin_unlock_irqrestore(&mq->lock, flags);
		return;
	}
	queue_demotion(mq, level == HIGH);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void smq_suspend(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);

	mq->policy.tick = NULL;
	if (!always_downward(mq))
		cmgr_unregister_feedback(mq->manager, &mq->ch);

	atomic_set(&mq->suspended ,1);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static int smq_resume(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;
	int r = 0;

	spin_lock_irqsave(&mq->lock, flags);

	if (!always_downward(mq)) {
		INIT_LIST_HEAD(&mq->ch.list);
		mq->ch.private = mq;
		mq->ch.feedback = smq_feedback;
		mq->ch.shrink = smq_shrink;

		r = cmgr_register_feedback(mq->manager, &mq->ch);
		mq->policy.tick = smq_tick;
	}

	atomic_set(&mq->suspended, 0);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static void smq_allow_migrations(struct dm_cache_policy *p, bool allow)
{
	struct smq_policy *mq = to_smq_policy(p);
	mq->migrations_allowed = allow;
}

static void calc_hotspot_params(sector_t, sector_t, unsigned, sector_t *, unsigned *);
static int __hotspot_resize(struct smq_policy *mq, sector_t origin_size)
{
	dm_cblock_t cache_size;
	sector_t cache_block_size;
	struct entry_space es;
	unsigned long *hit_bits;
	struct smq_hash_table htable;
	unsigned nr_hotspot_blocks;

	cache_size = cmgr_nr_blocks(mq->manager);
	cache_block_size = cmgr_block_size(mq->manager);

	calc_hotspot_params(origin_size, cache_block_size,
				from_cblock(cache_size),
				&mq->hotspot_block_size,
				&nr_hotspot_blocks);

	mq->cache_blocks_per_hotspot_block = div64_u64(mq->hotspot_block_size, cache_block_size);
	mq->hotspot_level_jump = 1u;

	if (es_init(&es, nr_hotspot_blocks)) {
		DMERR("couldn't initialize entry space");
		goto failed_es;
	}

	hit_bits = alloc_bitset(nr_hotspot_blocks);
	if (!hit_bits) {
		DMERR("couldn't allocate hotspot hit bitset");
		goto failed_bitset;
	}
	clear_bitset(hit_bits, nr_hotspot_blocks);


	if (h_init(&htable, &mq->hotspot_indexer, nr_hotspot_blocks))
		goto failed_h;

	mq->hotspot_es = es;
	es_indexer_init(&mq->hotspot_indexer, &mq->hotspot_es);
	ea_init(&mq->hotspot_alloc, &mq->hotspot_indexer, 0, mq->nr_hotspot_blocks);

	free_bitset(mq->hotspot_hit_bits);
	mq->hotspot_hit_bits = hit_bits;
	mq->nr_hotspot_blocks = nr_hotspot_blocks;

	q_init(&mq->hotspot, &mq->hotspot_indexer, NR_HOTSPOT_LEVELS, 0);
	mq->hotspot.nr_top_levels = 8;
	mq->hotspot.nr_in_top_levels = min(mq->nr_hotspot_blocks / NR_HOTSPOT_LEVELS,
					   from_cblock(cache_size) / mq->cache_blocks_per_hotspot_block);

	stats_init(&mq->hotspot_stats, NR_HOTSPOT_LEVELS);

	h_exit(&mq->hotspot_table);
	mq->hotspot_table = htable;
	mq->hotspot_table.ei = &mq->hotspot_indexer;

	return 0;

failed_h:
	free_bitset(hit_bits);
failed_bitset:
	es_exit(&es);
failed_es:
	return -EINVAL;
}

static int smq_hotspot_resize(struct dm_cache_policy *p, sector_t origin_size)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;
	int r;

	spin_lock_irqsave(&mq->lock, flags);
	r = __hotspot_resize(mq, origin_size);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

/*
 * Status format:
 *
 * <#no_room> <#reclaim> <#invalidate> <#periodic_cleaning> <#force_cleaning>
 */
static int smq_show_stats(struct dm_cache_policy *p, char *result,
				 unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;
	DMEMIT("policy: %u %u %u %u %u %u", (unsigned) atomic_read(&p->stat.no_room),
					    (unsigned) atomic_read(&p->stat.reclaim),
					    (unsigned) atomic_read(&p->stat.invalidate),
					    (unsigned) atomic_read(&p->stat.periodic_cleaning),
					    (unsigned) atomic_read(&p->stat.force_cleaning),
					    (unsigned) atomic_read(&p->stat.requeue));

	*sz_ptr = sz;
	return 0;
}

static void set_low_dirty_thresh(struct dm_cache_policy *p, int val)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);
	mq->low_dirty_thresh = val;
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void set_high_dirty_thresh(struct dm_cache_policy *p, int val)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);
	mq->high_dirty_thresh = val;
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void smq_blocks_shuffle(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;
	unsigned level;
	struct queue *q = &mq->clean;
	struct ilist *l;

	spin_lock_irqsave(&mq->lock, flags);
	clear_bitset(mq->hotspot_hit_bits, mq->nr_hotspot_blocks);
	clear_bitset(mq->cache_hit_bits, CACHE_HIT_FILTER_SIZE);
	stats_reset(&mq->hotspot_stats);
	stats_reset(&mq->cache_stats);
	q_migration(&mq->clean, mq->clean.nr_levels / 2);

	for (level = 0u; level < q->nr_levels; level++) {
		l = q->qs + level;
		DMDEBUG("level %u: count %d ", level, l->nr_elts);
	}
	spin_unlock_irqrestore(&mq->lock, flags);
}

/*
 * smq has no config values, but the old mq policy did.  To avoid breaking
 * software we continue to accept these configurables for the mq policy,
 * but they have no effect.
 */
static int smq_set_config_value(struct dm_cache_policy *p,
			       const char *key, const char *value)
{
	int tmp;

	if (kstrtoint(value, 10, &tmp))
		return -EINVAL;

	if (!strcasecmp(key, "low_dirty_thresh")) {
		set_low_dirty_thresh(p, tmp);
		return 0;
	}

	if (!strcasecmp(key, "high_dirty_thresh")) {
		set_high_dirty_thresh(p, tmp);
		return 0;
	}

	return -EINVAL;
}

static int mq_emit_config_values(struct dm_cache_policy *p, char *result,
				 unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;

	DMEMIT("10 random_threshold 0 "
	       "sequential_threshold 0 "
	       "discard_promote_adjustment 0 "
	       "read_promote_adjustment 0 "
	       "write_promote_adjustment 0 ");

	*sz_ptr = sz;
	return 0;
}

/* Init the policy plugin interface function pointers. */
static void init_policy_functions(struct smq_policy *mq, bool mimic_mq)
{
	mq->policy.destroy = smq_destroy;
	mq->policy.lookup = smq_lookup;
	mq->policy.lookup_with_work = smq_lookup_with_work;
	mq->policy.lookup_simple = smq_lookup_simple;
	mq->policy.get_background_work = smq_get_background_work;
	mq->policy.complete_background_work = smq_complete_background_work;
	mq->policy.set_dirty = smq_set_dirty;
	mq->policy.clear_dirty = smq_clear_dirty;
	mq->policy.load_mapping = smq_load_mapping;
	mq->policy.invalidate_mapping = smq_invalidate_mapping;
	mq->policy.get_hint = smq_get_hint;
	mq->policy.residency = smq_residency;
	mq->policy.tick = smq_tick;
	mq->policy.allow_migrations = smq_allow_migrations;
	mq->policy.show_stats = smq_show_stats;
	mq->policy.cache_resize = smq_hotspot_resize;
	mq->policy.set_config_value = smq_set_config_value;
	mq->policy.suspend = smq_suspend;
	mq->policy.resume = smq_resume;
	mq->policy.shuffle = smq_blocks_shuffle;

	if (mimic_mq)
		mq->policy.emit_config_values = mq_emit_config_values;
}

static void init_policy_functions_for_inval(struct smq_policy *mq)
{
	mq->policy.destroy = inval_destroy;
	mq->policy.lookup = smq_lookup;
	mq->policy.lookup_with_work = smq_lookup_with_work;
	mq->policy.lookup_simple = smq_lookup_simple;
	mq->policy.get_background_work = smq_get_background_work;
	mq->policy.complete_background_work = smq_complete_background_work;
	mq->policy.set_dirty = smq_set_dirty;
	mq->policy.clear_dirty = smq_clear_dirty;
	mq->policy.load_mapping = smq_load_mapping;
	mq->policy.sort_mapping = inval_sort_mapping;
	mq->policy.get_hint = smq_get_hint;
	mq->policy.residency = smq_residency;
	mq->policy.allow_migrations = smq_allow_migrations;
	mq->policy.show_stats = smq_show_stats;
	mq->policy.suspend = smq_suspend;
	mq->policy.resume = smq_resume;
}

static bool too_many_hotspot_blocks(sector_t origin_size,
				    sector_t hotspot_block_size,
				    unsigned nr_hotspot_blocks)
{
	return (hotspot_block_size * nr_hotspot_blocks) > origin_size;
}

static void calc_hotspot_params(sector_t origin_size,
				sector_t cache_block_size,
				unsigned nr_cache_blocks,
				sector_t *hotspot_block_size,
				unsigned *nr_hotspot_blocks)
{
	*hotspot_block_size = cache_block_size * 16u;
	*nr_hotspot_blocks = max(nr_cache_blocks / 4u, 1024u);

	while ((*hotspot_block_size > cache_block_size) &&
	       too_many_hotspot_blocks(origin_size, *hotspot_block_size, *nr_hotspot_blocks))
		*hotspot_block_size /= 2u;
}

static struct dm_cache_policy *__smq_create(sector_t origin_size,
					    struct cache_manager *m,
					    bool mimic_mq,
					    bool migrations_allowed,
					    int migration_dir,
					    bool tuned)
{
	int r;
	sector_t cache_block_size;
	dm_cblock_t cache_size;
	struct smq_policy *mq = kzalloc(sizeof(*mq), GFP_KERNEL);

	if (!mq)
		return NULL;

	init_policy_functions(mq, mimic_mq);

	cache_size = cmgr_nr_blocks(m);
	cache_block_size = cmgr_block_size(m);

	calc_hotspot_params(origin_size, cache_block_size,
				from_cblock(cache_size),
				&mq->hotspot_block_size,
				&mq->nr_hotspot_blocks);

	mq->cache_blocks_per_hotspot_block = div64_u64(mq->hotspot_block_size, cache_block_size);
	mq->hotspot_level_jump = 1u;

	if (es_init(&mq->hotspot_es, mq->nr_hotspot_blocks)) {
		DMERR("couldn't initialize entry space");
		goto bad_pool_init;
	}
	es_indexer_init(&mq->hotspot_indexer, &mq->hotspot_es);
	ea_init(&mq->hotspot_alloc, &mq->hotspot_indexer, 0, mq->nr_hotspot_blocks);

	mq->manager = m;

	mq->hotspot_hit_bits = alloc_bitset(mq->nr_hotspot_blocks);
	if (!mq->hotspot_hit_bits) {
		DMERR("couldn't allocate hotspot hit bitset");
		goto bad_hotspot_hit_bits;
	}
	clear_bitset(mq->hotspot_hit_bits, mq->nr_hotspot_blocks);

	mq->cache_hit_bits = alloc_bitset(CACHE_HIT_FILTER_SIZE);
	if (!mq->cache_hit_bits) {
		DMERR("couldn't allocate cache hit bitset");
		goto bad_cache_hit_bits;
	}
	clear_bitset(mq->cache_hit_bits, CACHE_HIT_FILTER_SIZE);

	mq->tick = 0;
	spin_lock_init(&mq->lock);

	q_init(&mq->hotspot, &mq->hotspot_indexer, NR_HOTSPOT_LEVELS, 0);
	mq->hotspot.nr_top_levels = 8;
	mq->hotspot.nr_in_top_levels = min(mq->nr_hotspot_blocks / NR_HOTSPOT_LEVELS,
					   from_cblock(cache_size) / mq->cache_blocks_per_hotspot_block);

	cmgr_indexer_init(&mq->cache_indexer, m);
	q_init(&mq->clean, &mq->cache_indexer, NR_CACHE_LEVELS, tuned);
	q_init(&mq->dirty, &mq->cache_indexer, NR_CACHE_LEVELS, 0);
	q_init(&mq->sort, &mq->cache_indexer, 1, 0);

	stats_init(&mq->hotspot_stats, NR_HOTSPOT_LEVELS);
	stats_init(&mq->cache_stats, NR_CACHE_LEVELS);

	if (h_init(&mq->table, &mq->cache_indexer, from_cblock(cache_size)))
		goto bad_alloc_table;

	if (h_init(&mq->hotspot_table, &mq->hotspot_indexer, mq->nr_hotspot_blocks))
		goto bad_alloc_hotspot_table;

	mq->write_promote_level = mq->read_promote_level = NR_HOTSPOT_LEVELS;

	mq->next_hotspot_period = jiffies;
	mq->next_cache_period = jiffies;

	mq->low_dirty_thresh = 0;
	mq->high_dirty_thresh = 100;

	atomic_set(&mq->suspended, 1);

	mq->bg_work = btracker_create(4096); /* FIXME: hard coded value */
	if (!mq->bg_work)
		goto bad_btracker;

	mq->migrations_allowed = migrations_allowed;
	mq->migration_dir = migration_dir;
	mq->q_tune = tuned;

	mq->demote_entries = 32;

	return &mq->policy;

bad_btracker:
	h_exit(&mq->hotspot_table);
bad_alloc_hotspot_table:
	h_exit(&mq->table);
bad_alloc_table:
	free_bitset(mq->cache_hit_bits);
bad_cache_hit_bits:
	free_bitset(mq->hotspot_hit_bits);
bad_hotspot_hit_bits:
	es_exit(&mq->hotspot_es);
bad_pool_init:
	kfree(mq);


	return NULL;
}

static struct dm_cache_policy *__invalidator_create(sector_t origin_size,
						    struct cache_manager *m,
						    int migration_dir)
{
	sector_t cache_block_size;
	dm_cblock_t cache_size;
	struct smq_policy *mq = kzalloc(sizeof(*mq), GFP_KERNEL);

	if (!mq)
		return NULL;

	init_policy_functions_for_inval(mq);

	cache_size = cmgr_nr_blocks(m);
	cache_block_size = cmgr_block_size(m);

	mq->manager = m;

	mq->tick = 0;
	spin_lock_init(&mq->lock);

	cmgr_indexer_init(&mq->cache_indexer, m);
	q_init(&mq->clean, &mq->cache_indexer, NR_CACHE_LEVELS, 0);
	q_init(&mq->dirty, &mq->cache_indexer, NR_CACHE_LEVELS, 0);
	q_init(&mq->sort, &mq->cache_indexer, 1, 0);

	stats_init(&mq->cache_stats, NR_CACHE_LEVELS);

	if (h_init(&mq->table, &mq->cache_indexer, from_cblock(cache_size)))
		goto bad_alloc_table;

	mq->bg_work = btracker_create(4096); /* FIXME: hard coded value */
	if (!mq->bg_work)
		goto bad_btracker;

	mq->migrations_allowed = false;
	mq->migration_dir = migration_dir;
	mq->q_tune = 0;

	atomic_set(&mq->suspended, 1);

	mq->sort_list = RB_ROOT;
	mq->es_begin = vzalloc(array_size(from_cblock(cache_size), sizeof(struct inval_entry)));
	if (!mq->es_begin)
		goto bad_es;
	mq->es_end = mq->es_begin + from_cblock(cache_size);

	mq->demote_entries = 32;

	return &mq->policy;
bad_es:
	btracker_destroy(mq->bg_work);
bad_btracker:
	h_exit(&mq->table);
bad_alloc_table:
	kfree(mq);

	return NULL;
}

static struct dm_cache_policy *smq_create(sector_t origin_size,
					  struct cache_manager *m)
{
	return __smq_create(origin_size, m, false, true, NONE, false);
}

static struct dm_cache_policy *smqv_create(sector_t origin_size,
					   struct cache_manager *m)
{
	return __smq_create(origin_size, m, false, true, UPWARD, true); // FIXME
}

static struct dm_cache_policy *mq_create(sector_t origin_size,
					 struct cache_manager *m)
{
	return __smq_create(origin_size, m, true, true, NONE, false);
}

static struct dm_cache_policy *cleaner_create(sector_t origin_size,
					      struct cache_manager *m)
{
	return __smq_create(origin_size, m, false, false, NONE, false);
}

static struct dm_cache_policy *invalidator_create(sector_t origin_size,
						  struct cache_manager *m)
{
	return __invalidator_create(origin_size, m, DOWNWARD);
}

static struct dm_cache_policy *remover_create(sector_t origin_size,
					      struct cache_manager *m)
{
	return __invalidator_create(origin_size, m, FORCE_DOWNWARD);
}

/*----------------------------------------------------------------*/

static struct dm_cache_policy_type smq_policy_type = {
	.name = "smq",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = smq_create
};

/* smq + always promote + tuned for pattern learning */
static struct dm_cache_policy_type smqv_policy_type = {
	.name = "smqv",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = smqv_create
};

static struct dm_cache_policy_type mq_policy_type = {
	.name = "mq",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = mq_create,
};

static struct dm_cache_policy_type cleaner_policy_type = {
	.name = "cleaner",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = cleaner_create,
};

static struct dm_cache_policy_type invalidator_policy_type = {
	.name = "invalidator",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = invalidator_create,
};

static struct dm_cache_policy_type remover_policy_type = {
	.name = "remover",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = remover_create,
};

static struct dm_cache_policy_type default_policy_type = {
	.name = "default",
	.version = {2, 0, 0},
	.hint_size = POLICY_HINT_SIZE,
	.owner = THIS_MODULE,
	.create = smqv_create,
	.real = &smqv_policy_type
};

static int __init smq_init(void)
{
	int r;

	r = dm_cache_policy_register(&smq_policy_type);
	if (r) {
		DMERR("register failed %d", r);
		return -ENOMEM;
	}

	r = dm_cache_policy_register(&smqv_policy_type);
	if (r) {
		DMERR("register failed %d", r);
		goto out_smqv;
	}

	r = dm_cache_policy_register(&mq_policy_type);
	if (r) {
		DMERR("register failed (as mq) %d", r);
		goto out_mq;
	}

	r = dm_cache_policy_register(&cleaner_policy_type);
	if (r) {
		DMERR("register failed (as cleaner) %d", r);
		goto out_cleaner;
	}

	r = dm_cache_policy_register(&invalidator_policy_type);
	if (r) {
		DMERR("register failed (as invalidator) %d", r);
		goto out_invalidator;
	}

	r = dm_cache_policy_register(&remover_policy_type);
	if (r) {
		DMERR("register failed (as remover) %d", r);
		goto out_remover;
	}

	r = dm_cache_policy_register(&default_policy_type);
	if (r) {
		DMERR("register failed (as default) %d", r);
		goto out_default;
	}

	return 0;

out_default:
	dm_cache_policy_unregister(&remover_policy_type);
out_remover:
	dm_cache_policy_unregister(&invalidator_policy_type);
out_invalidator:
	dm_cache_policy_unregister(&cleaner_policy_type);
out_cleaner:
	dm_cache_policy_unregister(&mq_policy_type);
out_mq:
	dm_cache_policy_unregister(&smqv_policy_type);
out_smqv:
	dm_cache_policy_unregister(&smq_policy_type);

	return -ENOMEM;
}

static void __exit smq_exit(void)
{
	dm_cache_policy_unregister(&remover_policy_type);
	dm_cache_policy_unregister(&invalidator_policy_type);
	dm_cache_policy_unregister(&cleaner_policy_type);
	dm_cache_policy_unregister(&smq_policy_type);
	dm_cache_policy_unregister(&smqv_policy_type);
	dm_cache_policy_unregister(&mq_policy_type);
	dm_cache_policy_unregister(&default_policy_type);
}

module_init(smq_init);
module_exit(smq_exit);

MODULE_AUTHOR("Joe Thornber <dm-devel@redhat.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("smq cache policy");

MODULE_ALIAS("dm-cache-default");
MODULE_ALIAS("dm-cache-mq");
MODULE_ALIAS("dm-cache-smqv");
MODULE_ALIAS("dm-cache-cleaner");
MODULE_ALIAS("dm-cache-invalidator");
