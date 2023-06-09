/*
 * Copyright (C) 2012-2017 Red Hat, Inc.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include "dm-bio-prison-v2.h"

#include <linux/spinlock.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rwsem.h>

/*----------------------------------------------------------------*/

#define MIN_CELLS 1024

struct dm_bio_prison_v2 {
	struct workqueue_struct *wq;

	spinlock_t lock;
	struct rb_root cells;
	mempool_t cell_pool;

	unsigned bed_per_cell;
	struct kmem_cache *cell_cache;

	unsigned int llb_shift;
};

//static struct kmem_cache *_cell_cache;
static atomic_t stuck_bio;
static atomic_t lock_cnt;
static atomic_t lock0_cnt;

/*----------------------------------------------------------------*/
#define BED_BITMAP_BYTES(prison) (sizeof(long) * BITS_TO_LONGS((prison)->bed_per_cell))
#define LOCK_BITMAP_BYTES(prison)						\
	(sizeof(long) *								\
	 BITS_TO_LONGS((prison)->bed_per_cell << (prison)->llb_shift))
#define CELL_SIZE(prison)							\
	(sizeof(struct dm_bio_prison_cell_v2) +					\
	 BED_BITMAP_BYTES(prison) + LOCK_BITMAP_BYTES(prison))

static inline unsigned next_entry(unsigned entry)
{
	return (entry + 1) % MAX_PLOCK;
}

static inline unsigned prev_entry(unsigned entry)
{
	return entry ? entry - 1 : MAX_PLOCK - 1;
}

static inline bool is_power_of_two(unsigned int i)
{
	return !(i & (i - 1));
}

static inline unsigned bed_begin(struct dm_bio_prison_v2 *prison,
				 struct dm_cell_key_v2 *key)
{
	return (unsigned)key->bed_begin;
}

static inline unsigned bed_end(struct dm_bio_prison_v2 *prison,
			       struct dm_cell_key_v2 *key)
{
	return key->bed_end;
}

/*
 * @nr_cells should be the number of cells you want in use _concurrently_.
 * Don't confuse it with the number of distinct keys.
 */
struct dm_bio_prison_v2 *dm_bio_prison_create_v2(struct workqueue_struct *wq,
						 unsigned int bed_per_cell,
						 unsigned int lock_levels)
{
	struct dm_bio_prison_v2 *prison = kzalloc(sizeof(*prison), GFP_KERNEL);
	int ret;
	unsigned int lock_level_bits_shift;

	if (!prison)
		return NULL;

	if (!is_power_of_two(bed_per_cell))
		return NULL;

	lock_level_bits_shift = fls(fls(lock_levels - 1) - 1);
	if (lock_level_bits_shift > 3)
		return NULL;

	pr_err("%s: llb_shift = %d\n", __func__, prison->llb_shift);

	prison->wq = wq;
	prison->bed_per_cell = bed_per_cell;
	prison->llb_shift = lock_level_bits_shift;
	spin_lock_init(&prison->lock);

	prison->cell_cache = kmem_cache_create("dm_bio_prison_cell_v2",
					       CELL_SIZE(prison),
					       __alignof__(struct dm_bio_prison_cell_v2),
					       0, NULL);
	if (!prison->cell_cache)
		goto free_prison;

	ret = mempool_init_slab_pool(&prison->cell_pool, MIN_CELLS, prison->cell_cache);
	if (ret)
		goto free_cell_cache;

	prison->cells = RB_ROOT;

	return prison;

free_cell_cache:
	kmem_cache_destroy(prison->cell_cache);
free_prison:
	kfree(prison);
	return NULL;
}
EXPORT_SYMBOL_GPL(dm_bio_prison_create_v2);

void dm_bio_prison_destroy_v2(struct dm_bio_prison_v2 *prison)
{
	mempool_exit(&prison->cell_pool);
	kmem_cache_destroy(prison->cell_cache);
	kfree(prison);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_destroy_v2);

struct dm_bio_prison_cell_v2 *dm_bio_prison_alloc_cell_v2(struct dm_bio_prison_v2 *prison, gfp_t gfp)
{
	return mempool_alloc(&prison->cell_pool, gfp);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_alloc_cell_v2);

void dm_bio_prison_free_cell_v2(struct dm_bio_prison_v2 *prison,
				struct dm_bio_prison_cell_v2 *cell)
{
	mempool_free(cell, &prison->cell_pool);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_free_cell_v2);

static void __setup_new_cell(struct dm_cell_key_v2 *key,
			     struct dm_bio_prison_cell_v2 *cell,
			     struct dm_bio_prison_v2 *prison)
{
	int i = 0;
	memset(cell, 0, CELL_SIZE(prison));
	memcpy(&cell->key, key, sizeof(cell->key));
	bio_list_init(&cell->bios);

	for (i = 0; i < MAX_PLOCK; i++)
		INIT_LIST_HEAD(cell->cont_list + i);
}

static int cmp_keys(struct dm_cell_key_v2 *lhs,
		    struct dm_cell_key_v2 *rhs)
{
	if (lhs->virtual < rhs->virtual)
		return -1;

	if (lhs->virtual > rhs->virtual)
		return 1;

	if (lhs->dev < rhs->dev)
		return -1;

	if (lhs->dev > rhs->dev)
		return 1;

	if (lhs->block_end <= rhs->block_begin)
		return -1;

	if (lhs->block_begin >= rhs->block_end)
		return 1;

	return 0;
}

/*
 * Returns true if node found, otherwise it inserts a new one.
 */
static bool __find_or_insert(struct dm_bio_prison_v2 *prison,
			     struct dm_cell_key_v2 *key,
			     struct dm_bio_prison_cell_v2 *cell_prealloc,
			     struct dm_bio_prison_cell_v2 **result)
{
	int r;
	struct rb_node **new = &prison->cells.rb_node, *parent = NULL;

	while (*new) {
		struct dm_bio_prison_cell_v2 *cell =
		    rb_entry(*new, struct dm_bio_prison_cell_v2, node);

		r = cmp_keys(key, &cell->key);

		parent = *new;
		if (r < 0)
			new = &((*new)->rb_left);

		else if (r > 0)
			new = &((*new)->rb_right);

		else {
			*result = cell;
			return true;
		}
	}

	__setup_new_cell(key, cell_prealloc, prison);
	*result = cell_prealloc;
	rb_link_node(&cell_prealloc->node, parent, new);
	rb_insert_color(&cell_prealloc->node, &prison->cells);

	return false;
}

static bool bed_occupied(struct dm_bio_prison_v2 *prison,
			 unsigned int begin, unsigned int end,
			 struct dm_bio_prison_cell_v2 *cell)
{
	if (cell->key.block_end - cell->key.block_begin == 1) {
		int r;

		if (begin == end)
			return test_bit(begin, cell->exclusive);

		r = find_next_bit(cell->exclusive,
				  prison->bed_per_cell,
				  begin);

		if (r == prison->bed_per_cell)
			return false;
		else
			return r <= end;
	} else
		return !bitmap_empty(cell->exclusive, prison->bed_per_cell);

}

static bool __cell_occupied(struct dm_bio_prison_v2 *prison,
			    struct dm_bio_prison_cell_v2 *cell)
{
	return bed_occupied(prison, 0, prison->bed_per_cell - 1, cell);
}

static void set_bed_level(struct dm_bio_prison_v2 *prison,
			  struct dm_bio_prison_cell_v2 *cell,
			  unsigned int bed_idx, unsigned int level)
{
	unsigned int bed_lock_offset = bed_idx << prison->llb_shift;
	size_t map_idx = BIT_WORD(bed_lock_offset);
	unsigned long offset = bed_lock_offset % BITS_PER_LONG;
	unsigned long *map =
	    cell->exclusive + BITS_TO_LONGS(prison->bed_per_cell);

	map[map_idx] |= ((unsigned long)level << offset);
}

static unsigned int get_bed_level(struct dm_bio_prison_v2 *prison,
				  struct dm_bio_prison_cell_v2 *cell,
				  unsigned int bed_idx)
{
	unsigned int bed_lock_offset = bed_idx << prison->llb_shift;
	size_t map_idx = BIT_WORD(bed_lock_offset);
	unsigned long offset = bed_lock_offset % BITS_PER_LONG;
	unsigned long *map =
	    cell->exclusive + BITS_TO_LONGS(prison->bed_per_cell);

	return (unsigned int)((map[map_idx] >> offset) &
			      ((1 << (1 << prison->llb_shift)) - 1));
}

static unsigned exclusive_level(struct dm_bio_prison_v2 *prison,
				unsigned int begin, unsigned int end,
				struct dm_bio_prison_cell_v2 *cell)
{
	unsigned int max_level = 0;

	if (cell->key.block_end - cell->key.block_begin == 1) {
		for (; begin <= end; begin++) {
			unsigned level = get_bed_level(prison, cell, begin);

			if (level > max_level)
				max_level = level;
		}

		return max_level;
	}

	return get_bed_level(prison, cell, 0);
}

static inline void forward_entry(struct dm_bio_prison_cell_v2 *cell)
{
	if (!cell->shared_count[next_entry(cell->entry)])
		cell->entry = next_entry(cell->entry);
}

static inline bool cell_full(struct dm_bio_prison_cell_v2 *cell)
{
	int i;

	for (i = 0; i < MAX_PLOCK; i++)
		if (!cell->shared_count[i])
			return false;

	return true;
}

static int __get(struct dm_bio_prison_v2 *prison,
		 struct dm_cell_key_v2 *key,
		 unsigned lock_level,
		 struct bio *inmate,
		 struct dm_bio_prison_cell_v2 *cell_prealloc,
		 struct dm_bio_prison_cell_v2 **cell)
{
	if (__find_or_insert(prison, key, cell_prealloc, cell)) {
		unsigned begin = bed_begin(prison, key);
		unsigned end   = bed_end(prison, key);

		if (bed_occupied(prison, begin, end, *cell)) {
			unsigned cell_level = exclusive_level(prison,
							      begin,
							      end,
							      *cell);
			if (lock_level <= cell_level) {
				atomic_inc(&stuck_bio);
				bio_list_add(&(*cell)->bios, inmate);
				return -EBUSY;
			}
		}

		if (!list_empty(&(*cell)->cont_list[(*cell)->entry]))
			forward_entry(*cell);
		(*cell)->shared_count[(*cell)->entry]++;
	} else
		(*cell)->shared_count[(*cell)->entry] = 1;

	return (*cell)->entry;
}

int dm_cell_get_v2(struct dm_bio_prison_v2 *prison,
		   struct dm_cell_key_v2 *key,
		   unsigned lock_level,
		   struct bio *inmate,
		   struct dm_bio_prison_cell_v2 *cell_prealloc,
		   struct dm_bio_prison_cell_v2 **cell_result)
{
	int r;

	spin_lock_irq(&prison->lock);
	r = __get(prison, key, lock_level, inmate, cell_prealloc, cell_result);
	spin_unlock_irq(&prison->lock);

	return r;
}
EXPORT_SYMBOL_GPL(dm_cell_get_v2);

static bool need_quiesce(struct dm_bio_prison_cell_v2 *cell)
{
	return cell->sweeper != cell->entry || cell->shared_count[cell->entry];
}

static void __drain_sweeper(struct dm_bio_prison_v2 *prison,
			    struct dm_bio_prison_cell_v2 *cell)
{
	struct work_struct *w, *b;

	BUG_ON(cell->shared_count[cell->sweeper]);

	list_for_each_entry_safe(w, b, cell->cont_list + cell->sweeper, entry) {
		list_del_init(&w->entry);
		queue_work(prison->wq, w);
	}
}

static bool __put(struct dm_bio_prison_v2 *prison,
		  struct dm_bio_prison_cell_v2 *cell,
		  unsigned put_entry)
{
	bool cell_occupied = __cell_occupied(prison, cell);

	BUG_ON(!cell->shared_count[put_entry]);
	cell->shared_count[put_entry]--;

	if (put_entry != cell->sweeper)
		return false;

	// FIXME: shared locks granted above the lock level could starve this
	while((cell->sweeper != cell->entry) &&
	       !cell->shared_count[cell->sweeper]) {
		BUG_ON(!cell_occupied);
		__drain_sweeper(prison, cell);
		cell->sweeper = next_entry(cell->sweeper);
	}

	if (need_quiesce(cell))
		return false;

	__drain_sweeper(prison, cell);
	if (cell_occupied)
		return false;

	rb_erase(&cell->node, &prison->cells);
	return true;
}



bool dm_cell_put_v2(struct dm_bio_prison_v2 *prison,
		    struct dm_bio_prison_cell_v2 *cell,
		    unsigned put_entry)
{
	bool r;
	unsigned long flags;

	spin_lock_irqsave(&prison->lock, flags);
	r = __put(prison, cell, put_entry);
	spin_unlock_irqrestore(&prison->lock, flags);

	return r;
}
EXPORT_SYMBOL_GPL(dm_cell_put_v2);

static void leave_bed(struct dm_bio_prison_v2 *prison,
		      unsigned int begin, unsigned int end,
		      struct dm_bio_prison_cell_v2 *cell)
{
	bitmap_clear(cell->exclusive, begin, end - begin + 1);

	if (cell->key.block_end - cell->key.block_begin == 1) {
		for (; begin <= end; begin++)
			set_bed_level(prison, cell, begin, 0);
	} else {
		set_bed_level(prison, cell, 0, 0);
	}
}

static void occupy_bed(struct dm_bio_prison_v2 *prison, unsigned int begin,
		       unsigned int end, unsigned int level,
		       struct dm_bio_prison_cell_v2 *cell)
{
	bitmap_set(cell->exclusive, begin, end - begin + 1);

	if (level == 0)
		return;

	if (cell->key.block_end - cell->key.block_begin == 1) {
		for (; begin <= end; begin++)
			set_bed_level(prison, cell, begin, level);
	} else {
		set_bed_level(prison, cell, 0, level);
	}
}

static void __quiesce(struct dm_bio_prison_v2 *prison,
		      struct dm_bio_prison_cell_v2 *cell,
		      struct work_struct *continuation)
{
	if (!cell->shared_count[cell->entry]) {
		if (cell->sweeper == cell->entry)
			queue_work(prison->wq, continuation);
		else
			list_add_tail(&continuation->entry,
				      &cell->cont_list[prev_entry(cell->entry)]);
	} else {
		list_add_tail(&continuation->entry, &cell->cont_list[cell->entry]);
		forward_entry(cell);
	}
}

void dm_cell_quiesce_v2(struct dm_bio_prison_v2 *prison,
			struct dm_bio_prison_cell_v2 *cell,
			struct work_struct *continuation)
{
	spin_lock_irq(&prison->lock);
	__quiesce(prison, cell, continuation);
	spin_unlock_irq(&prison->lock);
}
EXPORT_SYMBOL_GPL(dm_cell_quiesce_v2);

static int __lock(struct dm_bio_prison_v2 *prison,
		  struct dm_cell_key_v2 *key,
		  unsigned lock_level,
		  struct dm_bio_prison_cell_v2 *cell_prealloc,
		  struct dm_bio_prison_cell_v2 **cell_result)
{
	struct dm_bio_prison_cell_v2 *cell;
	unsigned begin = bed_begin(prison, key);
	unsigned end   = bed_end(prison, key);

	if (__find_or_insert(prison, key, cell_prealloc, &cell)) {
		if (bed_occupied(prison, begin, end, cell))
			return -EBUSY;

		// FIXME: we don't yet know what level these shared locks
		// were taken at, so have to quiesce them all.

	} else {
		cell = cell_prealloc;
		memset(cell->shared_count, 0, sizeof(cell->shared_count));
	}
	occupy_bed(prison, begin, end, lock_level, cell);
	*cell_result = cell;

	atomic_inc(&lock_cnt);
	return need_quiesce(cell);
}

int dm_cell_lock_v2(struct dm_bio_prison_v2 *prison,
		    struct dm_cell_key_v2 *key,
		    unsigned lock_level,
		    struct dm_bio_prison_cell_v2 *cell_prealloc,
		    struct dm_bio_prison_cell_v2 **cell_result)
{
	int r;

	spin_lock_irq(&prison->lock);
	r = __lock(prison, key, lock_level, cell_prealloc, cell_result);
	spin_unlock_irq(&prison->lock);

	if (r >= 0)
		atomic_inc(&lock0_cnt);
	return r;
}
EXPORT_SYMBOL_GPL(dm_cell_lock_v2);

int __promote(struct dm_bio_prison_v2 *prison,
	      struct dm_bio_prison_cell_v2 *cell,
	      struct dm_cell_key_v2 *key,
	      unsigned new_lock_level)
{
	unsigned begin = bed_begin(prison, key);
	unsigned end   = bed_end(prison, key);

	if (!bed_occupied(prison, begin, end, cell))
		return -EINVAL;

	occupy_bed(prison, begin, end, new_lock_level, cell);
	return need_quiesce(cell);
}

int dm_cell_lock_promote_v2(struct dm_bio_prison_v2 *prison,
			    struct dm_bio_prison_cell_v2 *cell,
			    struct dm_cell_key_v2 *key,
			    unsigned new_lock_level)
{
	int r;

	spin_lock_irq(&prison->lock);
	r = __promote(prison, cell, key, new_lock_level);
	spin_unlock_irq(&prison->lock);

	return r;
}
EXPORT_SYMBOL_GPL(dm_cell_lock_promote_v2);

static bool __unlock(struct dm_bio_prison_v2 *prison,
		     struct dm_bio_prison_cell_v2 *cell,
		     struct dm_cell_key_v2 *key,
		     struct bio_list *bios)
{
	atomic_dec(&lock_cnt);
	leave_bed(prison, bed_begin(prison, key), bed_end(prison, key), cell);

	bio_list_merge(bios, &cell->bios);
	bio_list_init(&cell->bios);
	atomic_sub(bio_list_size(bios), &stuck_bio);

	if (need_quiesce(cell) || __cell_occupied(prison, cell))
		return false;

	rb_erase(&cell->node, &prison->cells);
	return true;
}

bool dm_cell_unlock_v2(struct dm_bio_prison_v2 *prison,
		       struct dm_bio_prison_cell_v2 *cell,
		       struct dm_cell_key_v2 *key,
		       struct bio_list *bios)
{
	bool r;

	atomic_dec(&lock0_cnt);
	spin_lock_irq(&prison->lock);
	r = __unlock(prison, cell, key, bios);
	spin_unlock_irq(&prison->lock);

	return r;
}
EXPORT_SYMBOL_GPL(dm_cell_unlock_v2);

int dm_bio_prison_get_stuck_bio(struct dm_bio_prison_v2 *prison)
{
	return atomic_read(&stuck_bio);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_get_stuck_bio);

int dm_bio_prison_get_lock_cnt(struct dm_bio_prison_v2 *prison)
{
	return atomic_read(&lock_cnt);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_get_lock_cnt);

int dm_bio_prison_get_out_lock_cnt(struct dm_bio_prison_v2 *prison)
{
	return atomic_read(&lock0_cnt);
}
EXPORT_SYMBOL_GPL(dm_bio_prison_get_out_lock_cnt);

/*----------------------------------------------------------------*/

int __init dm_bio_prison_init_v2(void)
{
	atomic_set(&stuck_bio, 0);
	atomic_set(&lock_cnt, 0);
	atomic_set(&lock0_cnt, 0);
	return 0;
}

void dm_bio_prison_exit_v2(void) {}
