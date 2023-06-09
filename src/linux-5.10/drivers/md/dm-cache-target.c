/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include "dm-bio-prison-v2.h"
#include "dm-bio-record.h"
#include "dm-cache-metadata.h"
#include "dm-cache-manager.h"

#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>
#include <linux/jiffies.h>
#include <linux/init.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/async_tx.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/list_sort.h>

#define DM_MSG_PREFIX "cache"
#define DM_MSG_PREFIX_CLIENT "cache-client"

DECLARE_DM_KCOPYD_THROTTLE_WITH_MODULE_PARM(cache_copy_throttle,
	"A percentage of time allocated for copying to and/or from cache");
/*----------------------------------------------------------------*/

#define SUB_BLOCK_SIZE 8
#define MAX_SUB_BLOCKS_PER_BLOCK 256

#define MAX_BITMAP_LOCK 256
#define PAGE_POOL_GROW 1024
#define PAGE_POOL_MAX	5

static atomic_t 		in_io;
static atomic_t 		rw_io;
static atomic_t 		zero_io;
static atomic_t 		h_io;
static atomic_t 		remap_io;
static atomic_t 		mg_io;
static atomic_t 		mg2_io;
static atomic_t 		mg3_io;
static atomic_t 		rq_io;
static atomic_t 		lock_cell;
static atomic_t 		lock2_cell;
static atomic_t 		nounlock;


/*----------------------------------------------------------------*/

/*
 * Glossary:
 *
 * oblock: index of an origin block
 * cblock: index of a cache block
 * promotion: movement of a block from origin to cache
 * demotion: movement of a block from cache to origin
 * migration: movement of a block between the origin and cache device,
 *	      either direction
 */

/*----------------------------------------------------------------*/

struct io_tracker {
	spinlock_t lock;

	/*
	 * Sectors of in-flight IO.
	 */
	sector_t in_flight;

	/*
	 * The time, in jiffies, when this device became idle (if it is
	 * indeed idle).
	 */
	unsigned long idle_time;
	unsigned long last_update_time;
};

static void iot_init(struct io_tracker *iot)
{
	spin_lock_init(&iot->lock);
	iot->in_flight = 0ul;
	iot->idle_time = 0ul;
	iot->last_update_time = jiffies;
}

static bool __iot_idle_for(struct io_tracker *iot, unsigned long jifs)
{
	if (iot->in_flight)
		return false;

	return time_after(jiffies, iot->idle_time + jifs);
}

static bool iot_idle_for(struct io_tracker *iot, unsigned long jifs)
{
	bool r;

	spin_lock_irq(&iot->lock);
	r = __iot_idle_for(iot, jifs);
	spin_unlock_irq(&iot->lock);

	return r;
}

static void iot_io_begin(struct io_tracker *iot, sector_t len)
{
	spin_lock_irq(&iot->lock);
	iot->in_flight += len;
	spin_unlock_irq(&iot->lock);
}

static void __iot_io_end(struct io_tracker *iot, sector_t len)
{
	if (!len)
		return;

	iot->in_flight -= len;
	if (!iot->in_flight)
		iot->idle_time = jiffies;
}

static void iot_io_end(struct io_tracker *iot, sector_t len)
{
	unsigned long flags;

	spin_lock_irqsave(&iot->lock, flags);
	__iot_io_end(iot, len);
	spin_unlock_irqrestore(&iot->lock, flags);
}

/*----------------------------------------------------------------*/

/*
 * Represents a chunk of future work.  'input' allows continuations to pass
 * values between themselves, typically error values.
 */
struct continuation {
	struct work_struct ws;
	blk_status_t input;
	void (*queue_fn)(struct work_struct *);
};

static inline void init_continuation(struct continuation *k,
				     void (*fn)(struct work_struct *))
{
	k->input = 0;
	k->queue_fn = NULL;
	INIT_WORK(&k->ws, fn);
}

static inline void init_queue_continuation(struct continuation *k,
					   void (*fn)(struct work_struct *))
{
	k->input = 0;
	k->queue_fn = fn;
	INIT_WORK(&k->ws, NULL);
}

static inline void queue_continuation(struct workqueue_struct *wq,
				      struct continuation *k)
{
	if (k->queue_fn)
		k->queue_fn(&k->ws);
	else
		queue_work(wq, &k->ws);
}

/*----------------------------------------------------------------*/

/*
 * The batcher collects together pieces of work that need a particular
 * operation to occur before they can proceed (typically a commit).
 */
struct batcher {
	/*
	 * The operation that everyone is waiting for.
	 */
	blk_status_t (*commit_op)(void *context);
	void *commit_context;

	/*
	 * This is how bios should be issued once the commit op is complete
	 * (accounted_and_submit).
	 */
	void (*issue_op)(struct bio *bio, void *context);
	void *issue_context;

	/*
	 * Queued work gets put on here after commit.
	 */
	struct workqueue_struct *wq;

	spinlock_t lock;
	struct list_head work_items;
	struct bio_list bios;
	struct work_struct commit_work;

	bool commit_scheduled;
};

static inline bool fua_done_commit(struct bio *bio);
static inline void defer_fua_bio(struct bio *bio);
static void __commit(struct work_struct *_ws)
{
	struct batcher *b = container_of(_ws, struct batcher, commit_work);
	blk_status_t r;
	struct list_head work_items;
	struct work_struct *ws, *tmp;
	struct continuation *k;
	struct bio *bio;
	struct bio_list bios;

	INIT_LIST_HEAD(&work_items);
	bio_list_init(&bios);

	/*
	 * We have to grab these before the commit_op to avoid a race
	 * condition.
	 */
	spin_lock_irq(&b->lock);
	list_splice_init(&b->work_items, &work_items);
	bio_list_merge(&bios, &b->bios);
	bio_list_init(&b->bios);
	b->commit_scheduled = false;
	spin_unlock_irq(&b->lock);

	r = b->commit_op(b->commit_context);

	list_for_each_entry_safe(ws, tmp, &work_items, entry) {
		k = container_of(ws, struct continuation, ws);
		k->input = r;
		INIT_LIST_HEAD(&ws->entry); /* to avoid a WARN_ON */
		queue_work(b->wq, ws);
	}

	while ((bio = bio_list_pop(&bios))) {
		if (r) {
			bio->bi_status = r;
			bio_endio(bio);
		} else if (bio->bi_opf & REQ_FUA && !fua_done_commit(bio)) {
			defer_fua_bio(bio);
		} else {
			b->issue_op(bio, b->issue_context);
		}
	}


}

static void batcher_init(struct batcher *b,
			 blk_status_t (*commit_op)(void *),
			 void *commit_context,
			 void (*issue_op)(struct bio *bio, void *),
			 void *issue_context,
			 struct workqueue_struct *wq)
{
	b->commit_op = commit_op;
	b->commit_context = commit_context;
	b->issue_op = issue_op;
	b->issue_context = issue_context;
	b->wq = wq;

	spin_lock_init(&b->lock);
	INIT_LIST_HEAD(&b->work_items);
	bio_list_init(&b->bios);
	INIT_WORK(&b->commit_work, __commit);
	b->commit_scheduled = false;
}

static void async_commit(struct batcher *b)
{
	queue_work(b->wq, &b->commit_work);
}

static void continue_after_commit(struct batcher *b, struct continuation *k)
{
	bool commit_scheduled;

	spin_lock_irq(&b->lock);
	commit_scheduled = b->commit_scheduled;
	list_add_tail(&k->ws.entry, &b->work_items);
	spin_unlock_irq(&b->lock);

	if (commit_scheduled)
		async_commit(b);
}

/*
 * Bios are errored if commit failed.
 */
static void issue_after_commit(struct batcher *b, struct bio *bio)
{
       bool commit_scheduled;

       spin_lock_irq(&b->lock);
       commit_scheduled = b->commit_scheduled;
       bio_list_add(&b->bios, bio);
       spin_unlock_irq(&b->lock);

       if (commit_scheduled)
	       async_commit(b);
}

/*
 * Call this if some urgent work is waiting for the commit to complete.
 */
static void schedule_commit(struct batcher *b)
{
	bool immediate;

	spin_lock_irq(&b->lock);
	immediate = !list_empty(&b->work_items) || !bio_list_empty(&b->bios);
	b->commit_scheduled = true;
	spin_unlock_irq(&b->lock);

	if (immediate)
		async_commit(b);
}

/*
 * There are a couple of places where we let a bio run, but want to do some
 * work before calling its endio function.  We do this by temporarily
 * changing the endio fn.
 */
struct dm_hook_info {
	bio_end_io_t *bi_end_io;
	void *bi_private;
};

static void dm_hook_bio(struct dm_hook_info *h, struct bio *bio,
			bio_end_io_t *bi_end_io, void *bi_private)
{
	h->bi_end_io = bio->bi_end_io;
	h->bi_private = bio->bi_private;

	bio->bi_end_io = bi_end_io;
	bio->bi_private = bi_private;
}

static void dm_unhook_bio(struct dm_hook_info *h, struct bio *bio)
{
	bio->bi_end_io = h->bi_end_io;
	bio->bi_private = h->bi_private;
}

/*----------------------------------------------------------------*/

#define MIGRATION_POOL_SIZE 2048
#define COMMIT_PERIOD HZ
#define MIGRATION_COUNT_WINDOW 10

/*
 * The block size of the device holding cache data must be
 * between 32KB and 1GB.
 */
#define DATA_DEV_BLOCK_SIZE_MIN_SECTORS (32 * 1024 >> SECTOR_SHIFT)
#define DATA_DEV_BLOCK_SIZE_MAX_SECTORS (1024 * 1024 * 1024 >> SECTOR_SHIFT)

enum cache_metadata_mode {
	CM_WRITE,		/* metadata may be changed */
	CM_READ_ONLY,		/* metadata may not be changed */
	CM_FAIL
};

enum cache_io_mode {
	/*
	 * Data is written to cached blocks only.  These blocks are marked
	 * dirty.  If you lose the cache device you will lose data.
	 * Potential performance increase for both reads and writes.
	 */
	CM_IO_WRITEBACK,

	/*
	 * Data is written to both cache and origin.  Blocks are never
	 * dirty.  Potential performance benfit for reads only.
	 */
	CM_IO_WRITETHROUGH,

	/*
	 * A degraded mode useful for various cache coherency situations
	 * (eg, rolling back snapshots).  Reads and writes always go to the
	 * origin.  If a write goes to a cached oblock, then the cache
	 * block is invalidated.
	 */
	CM_IO_PASSTHROUGH
};

enum cache_filter {
	FILTER_READ = READ,
	FILTER_WRITE = WRITE,
};

struct cache_features {
	enum cache_metadata_mode mode;
	unsigned metadata_version;
};

struct client_features {
	enum cache_io_mode io_mode;
	bool discard_passdown:1;
	unsigned long filter;
	bool optional_cache:1; /* need to check REQ_NEED_CACHE flag */
	bool discard_return_blocks:1; /* check if discard bio clears all sub valid
				       * bits and return this cache block
				       */
};

struct cache_stats {
	atomic_t reads;
	atomic_t writes;
	atomic_t read_hit;
	atomic_t read_miss;
	atomic_t write_hit;
	atomic_t write_miss;
	atomic_t dirty_write_hit;
	atomic_t demotion;
	atomic_t promotion;
	atomic_t writeback;
	atomic_t copies_avoided;
	atomic_t cache_cell_clash;
	atomic_t discard_count;
	atomic_t direct_count;
	atomic_t defer_bio;
	atomic_t disk_reads;
	atomic_t disk_writes;
	atomic_t ssd_reads;
	atomic_t ssd_writes;
	atomic_t uncached_reads;
	atomic_t uncached_writes;
	atomic_t uncached_seq_reads;
	atomic_t uncached_seq_writes;
	atomic_t page_pool_grow;
};

struct current_stats {
	unsigned long long reads;
	unsigned long long writes;
	unsigned long long read_hits;
	unsigned long long write_hits;
	unsigned long long dirty_write_hits;
	unsigned long jif;
};

struct cache_errors {
	atomic_t disk_errs;
	atomic_t ssd_errs;
};

struct cache_opt {
	bool cache_all;
	unsigned long long skip_seq_thresh;
	unsigned long idle_period;
	unsigned high_dirty_thresh;
	unsigned low_dirty_thresh;
	unsigned bgm_throttle;
	unsigned size_thresh;
	unsigned page_pool_max;
};

struct mg_worker {
	spinlock_t lock;
	struct list_head works;
	struct work_struct worker;
	void (*fn)(struct work_struct *);
};

struct cache {
	struct dm_target *ti;
	spinlock_t lock;

	struct kobject kobj;
	/*
	 * Fields for converting from sectors to blocks.
	 */
	int sectors_per_block_shift;
	sector_t sectors_per_block;

	struct mapped_device *cache_md;

	struct dm_cache_metadata *cmd;

	/*
	 * Metadata is written to this device.
	 */
	struct block_device *metadata_dev;

	/*
	 * The faster of the two data devices.  Typically an SSD.
	 */
	struct block_device *cache_dev;

	/*
	 * Size of the cache device in blocks.
	 */
	dm_cblock_t cache_size;

	/*
	 * Invalidation fields.
	 */
	spinlock_t invalidation_lock;
	struct list_head invalidation_requests;

	sector_t migration_threshold;
	wait_queue_head_t migration_wait;
	atomic_t nr_allocated_migrations;

	/*
	 * The number of in flight migrations that are performing
	 * background io. eg, promotion, writeback.
	 */
	atomic_t nr_io_migrations;

	//struct bio_list deferred_bios;

	struct rw_semaphore quiesce_lock;

	/*
	 * sub_blocks bitmap
	 */
	unsigned int sub_blocks_per_cblock;
	unsigned long sub_cache_size;
	unsigned long *sub_dirty_bitset;
	unsigned long *sub_valid_bitset;
	spinlock_t sub_locks[MAX_BITMAP_LOCK];

	/*
	 * Rather than reconstructing the table line for the status we just
	 * save it and regurgitate.
	 */
	unsigned nr_ctr_args;
	const char **ctr_args;

	struct dm_kcopyd_client *copier;
	struct dm_io_client *io_client;

	struct work_struct client_recycler;
	struct work_struct deferred_bio_worker;
	struct work_struct migration_worker;
	struct workqueue_struct *wq;
	struct delayed_work waker;
	struct dm_bio_prison_v2 *prison;

	struct mg_worker metadata_write_worker;

	/*
	 * cache_size entries, dirty if set
	 */
	unsigned long *dirty_bitset;
	atomic_t nr_dirty;

	/*
	 * Cache features such as write-through.
	 */
	struct cache_features features;

	bool need_tick_bio:1;
	bool sized:1;
	bool invalidate:1;
	bool commit_requested:1;
	bool loaded_sub_valid:1;
	bool loaded_sub_dirty:1;
	bool loaded_shared_entry:1;

	bool suspended:1;

	struct rw_semaphore background_work_lock;

	struct batcher committer;
	struct work_struct commit_ws;
	atomic_t nr_commits;
	atomic_t nr_commits_1000;
	atomic_t nr_commits_500;
	atomic_t nr_commits_100;

	struct io_tracker tracker;

	mempool_t migration_pool;

	struct bio_set bs;

	struct page_list *page_pool;
	struct page_list *page_pool_tail;
	struct list_head page_list_group;
	long page_remaining;
	spinlock_t pool_lock;
	atomic_t pool_need_grow;

	unsigned ref_count;
	struct list_head active_clients;
	struct list_head list;

	struct cache_manager *manager;

	struct cache_stats stats;
	struct cache_errors err_stats;
	struct current_stats cur_stats;

	struct cache_opt opt;

	struct completion can_destroy;

	struct list_head client_kobj;

	unsigned long last_commit_jif;
};

struct cache_c {
	struct dm_target *ti;
	struct cache *cache;
	struct dm_dev *cache_dev;
	struct dm_dev *metadata_dev;
};

struct io {
	/* Used to track sequential IO so it can be skipped */
	struct hlist_node	hash;
	struct list_head	lru;

	unsigned long		jiffies;
	unsigned int		sequential;
	sector_t		last;
};

/*
 * Device id is restricted to 24 bits.
 */
#define MAX_DEV_ID ((1 << 24) - 1)

struct cache_client {
	struct dm_target *ti;
	spinlock_t lock;
	uint64_t dev_id;

	struct kobj_holder *holder;
	/*
	 * The slower of the two data devices.  Typically a spindle.
	 */
	struct dm_dev *origin_dev;
	struct dm_dev *cache_dev;
	struct dm_dev *client_dev;
	struct cache *cache;
	/*
	 * Size of the origin device in _complete_ blocks and native sectors.
	 */
	dm_oblock_t origin_blocks;
	sector_t origin_sectors;

	struct dm_cache_client_metadata *ccmd;

	/*
	 * Size of the cache device in blocks.
	 */
	dm_cblock_t cache_size;

	/*
	 * origin_blocks entries, discarded if set.
	 */
	dm_dblock_t discard_nr_blocks;
	unsigned long *discard_bitset;
	uint32_t discard_block_size; /* a power of 2 times sectors per block */

	struct mapped_device *client_md;

	struct list_head list;

	struct completion can_destroy;

	struct client_features features;

	struct cache_stats stats;
	struct cache_errors err_stats;

	bool need_tick_bio:1;
	bool loaded_mappings:1;
	bool loaded_discards:1;
	bool sized:1;
	bool requeue_mode:1;

	struct rw_semaphore background_work_lock;

	atomic_t nr_dirty;

	unsigned policy_nr_args;
	struct dm_cache_policy *policy;

	struct workqueue_struct *wq;
	struct delayed_work ticker;

	refcount_t refcount;

	struct bio_list deferred_bios;

	/*
	 * Rather than reconstructing the table line for the status we just
	 * save it and regurgitate.
	 */
	unsigned nr_ctr_args;
	const char **ctr_args;

	struct list_head wb_queue;
	struct work_struct wb_worker;
	spinlock_t wb_lock;

	struct cache_opt opt;

	/* For tracking sequential IO */
#define RECENT_IO_BITS	7
#define RECENT_IO	(1 << RECENT_IO_BITS)
	struct io		io[RECENT_IO];
	struct hlist_head	io_hash[RECENT_IO + 1];
	struct list_head	io_lru;
	spinlock_t		io_lock;

	unsigned int		sequential_cutoff;

	refcount_t		nr_meta_updates;
};

struct dm_cache_migration;
struct per_bio_data {
	bool tick:1;
	unsigned req_nr:2;
	unsigned cell_id:4;
	struct dm_bio_prison_cell_v2 *cell;
	struct dm_hook_info hook_info;
	sector_t len;

	refcount_t count;
	struct cache_client *client;
	int error;

	struct dm_cache_migration *mg;

	int remap;
	int mg_phase2;
	int rw;

	bool commit_done:1;
};

enum mg_operation {
	MG_NONE,
	MG_PROMOTE,
	MG_DEMOTE,
	MG_INVALIDATE,
};

struct dm_cache_migration {
	struct continuation k;
	struct list_head list;

	struct cache_client *client;

	struct policy_work *op;
	struct bio *optimisable_bio;

	struct dm_cell_key_v2 key;
	struct dm_bio_prison_cell_v2 *cell;

	/* These field is only used when policy op == NULL */
	enum mg_operation mg_op;
	dm_cblock_t cblock;
	dm_oblock_t oblock;

	bool error:1;
	struct page_list *pl;

	refcount_t sub_migration;
	unsigned long migrated_sblock[BITS_TO_LONGS(MAX_SUB_BLOCKS_PER_BLOCK)];

	struct bvec_iter iter;
};

static inline enum mg_operation mg_operation(struct dm_cache_migration *mg)
{
	return mg->mg_op;
}

static inline void set_mg_operation(struct dm_cache_migration *mg, enum mg_operation op)
{
	mg->mg_op = op;
}

static inline struct cache *mg_to_cache(struct dm_cache_migration *mg)
{
	return mg->client ? mg->client->cache : NULL;
}

static inline struct work_struct *mg_to_work(struct dm_cache_migration *mg)
{
	return &mg->k.ws;
}

struct dm_cache_table {
	struct mutex mutex;
	struct list_head caches;
} dm_cache_table;

static spinlock_t *cblock_lock(struct cache *cache, dm_cblock_t b)
{
	return cache->sub_locks + (b % MAX_BITMAP_LOCK);
}

/*----------------------------------------------------------------*/

static bool writethrough_mode(struct cache_client *client)
{
	return client->features.io_mode == CM_IO_WRITETHROUGH;
}

static bool writeback_mode(struct cache_client *client)
{
	return client->features.io_mode == CM_IO_WRITEBACK;
}

static inline bool passthrough_mode(struct cache_client *client)
{
	return unlikely(client->features.io_mode == CM_IO_PASSTHROUGH);
}

/*----------------------------------------------------------------*/

static void wake_client_recycler(struct cache *cache)
{
	queue_work(cache->wq, &cache->client_recycler);
}

static void wake_deferred_bio_worker(struct cache *cache)
{
	queue_work(cache->wq, &cache->deferred_bio_worker);
}

static struct cache_client *get_first_client(struct cache *cache);
static struct cache_client *get_next_client(struct cache *cache, struct cache_client *client);
static enum cache_metadata_mode get_cache_mode(struct cache *cache);

static void wake_migration_worker(struct cache *cache)
{
	struct cache_client *client;
	int all_passthrough = 1;

	client = get_first_client(cache);
	while (client) {
		if (!passthrough_mode(client))
			all_passthrough = 0;
		client = get_next_client(cache, client);
	}

	if (all_passthrough)
		return;

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return;

	queue_work(cache->wq, &cache->migration_worker);
}

/*----------------------------------------------------------------*/

#define PAGE_POOL_SIZE 20480

struct pl_group {
	struct page_list *pl;
	struct list_head list;
};

static void free_page_list_pages(struct page_list *pl)
{
	struct page_list *next;

	while (pl) {
		next = pl->next;
		__free_page(pl->page);
		pl = next;
	}
}

static void free_page_list_group(struct pl_group *group)
{
	kvfree(group->pl);
	kfree(group);
}

static struct page_list *get_pages(struct cache *cache, unsigned nr_pages)
{
	struct page_list *head, *pl;
	unsigned long flags;
	int i;

	if (!nr_pages)
		return NULL;

	spin_lock_irqsave(&cache->pool_lock, flags);
	if (cache->page_remaining < nr_pages) {
		spin_unlock_irqrestore(&cache->pool_lock, flags);
		atomic_set(&cache->pool_need_grow, 1);
		return NULL;
	}

	BUG_ON(!cache->page_pool);
	cache->page_remaining -= nr_pages;

	head = pl = cache->page_pool;
	if (cache->page_remaining == 0) {
		cache->page_pool = NULL;
		cache->page_pool_tail = NULL;
	} else {
		for (i = 0; i < nr_pages - 1; i++)
			pl = pl->next;

		cache->page_pool = pl->next;
		pl->next = NULL;
	}
	spin_unlock_irqrestore(&cache->pool_lock, flags);

	return head;
}

static void put_pages(struct cache *cache, struct page_list *pl)
{
	struct page_list *tail;
	unsigned long flags;
	int nr = 1;

	tail = pl;
	while (tail->next) {
		tail = tail->next;
		nr++;
	}

	spin_lock_irqsave(&cache->pool_lock, flags);

	if (cache->page_pool_tail)
		cache->page_pool_tail->next = pl;
	cache->page_pool_tail = tail;
	if (!cache->page_pool)
		cache->page_pool = pl;
	cache->page_remaining += nr;
	spin_unlock_irqrestore(&cache->pool_lock, flags);
}

static struct page_list *alloc_page_list(unsigned nr_pages, struct pl_group *group)
{
	struct page_list *pl, *res_pl = NULL;

	group->pl = kvmalloc_array(nr_pages, sizeof(*pl), GFP_KERNEL | __GFP_ZERO);
	if (!group->pl) {
		DMERR("pl group alloc failed");
		return NULL;
	}

	pl = group->pl;
	do {
		pl->page = alloc_page(GFP_KERNEL);
		if (!pl->page)
			goto oom;

		pl->next = res_pl;
		res_pl = pl;
		pl++;
	} while (--nr_pages);


	return res_pl;
oom:
	if (res_pl)
		free_page_list_pages(res_pl);

	kvfree(group->pl);

	return NULL;
}

static void grow_page_pool(struct cache *cache, unsigned nr_pages)
{
	struct page_list *pl;
	struct pl_group *group;
	bool skip_grow;

	spin_lock_irq(&cache->lock);
	skip_grow = (atomic_read(&cache->stats.page_pool_grow) > cache->opt.page_pool_max)? true: false;
	spin_unlock_irq(&cache->lock);

	if (skip_grow)
		goto skip;

	group = kzalloc(sizeof(struct pl_group), GFP_KERNEL);
	if (!group)
		return;

	pl = alloc_page_list(nr_pages, group);
	if (!pl) {
		kfree(group);
		return;
	}

	put_pages(cache, pl);
	atomic_inc(&cache->stats.page_pool_grow);
	list_add(&group->list, &cache->page_list_group);
skip:
	atomic_set(&cache->pool_need_grow, 0);
}

static struct dm_bio_prison_cell_v2 *alloc_prison_cell(struct cache *cache)
{
	return dm_bio_prison_alloc_cell_v2(cache->prison, GFP_NOIO);
}

static void free_prison_cell(struct cache *cache, struct dm_bio_prison_cell_v2 *cell)
{
	dm_bio_prison_free_cell_v2(cache->prison, cell);
}

static int prison_stuck_bio(struct cache *cache)
{
	return dm_bio_prison_get_stuck_bio(cache->prison);
}

static int prison_lock_cnt(struct cache *cache)
{
	return dm_bio_prison_get_lock_cnt(cache->prison);
}

static int prison_func_lock_cnt(struct cache *cache)
{
	return dm_bio_prison_get_out_lock_cnt(cache->prison);
}

static unsigned int origin_sector_to_sbit(struct cache *, dm_cblock_t, sector_t, sector_t *);
static int mg_need_pages(struct dm_cache_migration *mg)
{
	struct bio *bio = mg->optimisable_bio;
	struct cache *cache = mg_to_cache(mg);
	dm_cblock_t cblock = mg->cblock;
	unsigned num = 0;
	unsigned rs, re, start, end;

	if (!bio || bio_data_dir(bio) != READ || mg_operation(mg) != MG_PROMOTE)
		return 0;

	start = origin_sector_to_sbit(cache, cblock, bio->bi_iter.bi_sector, NULL);
	end   = origin_sector_to_sbit(cache, cblock, bio_end_sector(bio) - 1, NULL) + 1; // not included

	bitmap_for_each_clear_region(cache->sub_valid_bitset, rs, re, start, end)
		num += re - rs;

	return num;
}

static struct dm_cache_migration *alloc_migration(struct cache_client *client,
						  struct bio *bio, struct policy_work *op)
{
	struct dm_cache_migration *mg;
	struct cache *cache = client->cache;

	mg = mempool_alloc(&cache->migration_pool, GFP_NOIO);

	memset(mg, 0, sizeof(*mg));
	INIT_LIST_HEAD(&mg->list);

	/* for refcount cannot inc on zero */
	refcount_set(&mg->sub_migration, 1);
	mg->op = op;
	mg->client = client;
	if (bio)
		mg->iter = bio->bi_iter;
	mg->optimisable_bio = bio;
	atomic_inc(&cache->nr_allocated_migrations);

	return mg;
}

static void free_migration(struct dm_cache_migration *mg)
{
	struct cache *cache = mg_to_cache(mg);

	if (atomic_dec_and_test(&cache->nr_allocated_migrations))
		wake_up(&cache->migration_wait);

	if (mg->pl)
		put_pages(cache, mg->pl);

	mempool_free(mg, &cache->migration_pool);
}

/*----------------------------------------------------------------*/

static inline dm_sblock_t sector_to_sblock(struct cache *cache, sector_t s)
{
	if (cache->sectors_per_block_shift != -1)
		return to_sblock((unsigned int)((s & (cache->sectors_per_block - 1)) >> __ffs(SUB_BLOCK_SIZE)));
	else
		return to_sblock((unsigned int)((s % cache->sectors_per_block) >> __ffs(SUB_BLOCK_SIZE)));
}

static inline sector_t oblock_to_sector(struct cache *cache, dm_oblock_t b)
{
	if (cache->sectors_per_block_shift != -1)
		return from_oblock(b) << cache->sectors_per_block_shift;
	else
		return from_oblock(b) * cache->sectors_per_block;
}

static inline dm_oblock_t oblock_succ(dm_oblock_t b)
{
	return to_oblock(from_oblock(b) + 1ull);
}

static void __build_key(dm_oblock_t begin, dm_oblock_t end,
			dm_sblock_t bed_begin, dm_sblock_t bed_end,
			uint64_t dev_id,
			struct dm_cell_key_v2 *key)
{
	key->virtual = 0;
	key->dev = dev_id;
	key->block_begin = from_oblock(begin);
	key->block_end = from_oblock(end);
	key->bed_begin = from_sblock(bed_begin);
	key->bed_end   = from_sblock(bed_end);
}

static void build_key(dm_oblock_t begin, dm_oblock_t end,
		      uint64_t dev_id, struct dm_cell_key_v2 *key)
{
	__build_key(begin, end, 0,
		    (dm_sblock_t)((1 << (PRISON_CELL_BED_BITS + 1)) - 1), dev_id, key);
}

static void build_key_from_bio(dm_oblock_t begin, dm_oblock_t end, struct bio *bio,
			       struct cache *cache, uint64_t dev_id,
			       struct dm_cell_key_v2 *key)
{
	sector_t s = bio_sectors(bio);

	if (s < SUB_BLOCK_SIZE)
		s = SUB_BLOCK_SIZE;

	s += bio->bi_iter.bi_sector;

	__build_key(begin, end,
		    sector_to_sblock(cache, bio->bi_iter.bi_sector),
		    sector_to_sblock(cache, s - 1),
		    dev_id,
		    key);
}

/*
 * We have two lock levels.  Level 0, which is used to prevent WRITEs, and
 * level 1 which prevents *both* READs and WRITEs.
 */
#define WRITE_LOCK_LEVEL 0
#define READ_WRITE_LOCK_LEVEL 1

static unsigned lock_level(struct bio *bio)
{
	return bio_data_dir(bio) == WRITE ?
		WRITE_LOCK_LEVEL :
		READ_WRITE_LOCK_LEVEL;
}

/*----------------------------------------------------------------
 * Per bio data
 *--------------------------------------------------------------*/

static struct per_bio_data *get_per_bio_data(struct bio *bio)
{
	struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
	BUG_ON(!pb);
	return pb;
}

static struct per_bio_data *init_per_bio_data(struct cache_client *client,
					      struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	memset(pb, 0, sizeof(struct per_bio_data));

	pb->req_nr = dm_bio_get_target_bio_nr(bio);

	pb->client = client;
	/* for refcount cannot inc on zero */
	refcount_set(&pb->count, 1);

	pb->remap = 0;

	pb->mg_phase2 = 0;

	pb->rw = 0;

	pb->commit_done = false;

	return pb;
}

/*----------------------------------------------------------------*/

static void defer_bio(struct cache_client *client, struct bio *bio)
{
	spin_lock_irq(&client->lock);
	bio_list_add(&client->deferred_bios, bio);
	spin_unlock_irq(&client->lock);

	atomic_inc(&client->stats.defer_bio);
	wake_deferred_bio_worker(client->cache);
}

static void defer_bios(struct cache_client *client, struct bio_list *bios)
{
	spin_lock_irq(&client->lock);
	bio_list_merge(&client->deferred_bios, bios);
	bio_list_init(bios);
	spin_unlock_irq(&client->lock);

	wake_deferred_bio_worker(client->cache);
}

static void defer_fua_bio(struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	pb->commit_done = true;
	defer_bio(pb->client, bio);
}

/*----------------------------------------------------------------*/

static bool bio_detain_shared(struct cache_client *client, dm_oblock_t oblock,
			      struct bio *bio)
{
	int cell_id;
	struct per_bio_data *pb;
	struct dm_cell_key_v2 key;
	dm_oblock_t end = to_oblock(from_oblock(oblock) + 1ULL);
	struct dm_bio_prison_cell_v2 *cell_prealloc, *cell;
	struct cache *cache = client->cache;

	cell_prealloc = alloc_prison_cell(cache); /* FIXME: allow wait if calling from worker */

	build_key_from_bio(oblock, end, bio, cache, client->dev_id, &key);
	cell_id = dm_cell_get_v2(cache->prison, &key, lock_level(bio), bio, cell_prealloc, &cell);
	if (cell_id < 0) {
		/*
		 * Failed to get the lock.
		 */
		free_prison_cell(cache, cell_prealloc);
		return false;
	}

	if (cell != cell_prealloc)
		free_prison_cell(cache, cell_prealloc);

	pb = get_per_bio_data(bio);
	pb->cell = cell;
	pb->cell_id = (unsigned)cell_id;

	return true;
}

static void mg_drop_lock(struct dm_cache_migration *mg)
{
	struct bio_list bios;
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;

	bio_list_init(&bios);

	if (mg->cell) {
		atomic_dec(&lock_cell);
		atomic_dec(&lock2_cell);
		if (dm_cell_unlock_v2(cache->prison, mg->cell, &mg->key, &bios))
			free_prison_cell(cache, mg->cell);
	}

	defer_bios(client, &bios);
}

/*----------------------------------------------------------------*/

static void inc_nr_dirty(struct cache_client *client)
{
	struct cache *cache = client->cache;

	atomic_inc(&client->nr_dirty);
	atomic_inc(&cache->nr_dirty);
}

static bool block_size_is_power_of_two(struct cache *cache)
{
	return cache->sectors_per_block_shift >= 0;
}

static unsigned int cache_sector_to_sbit(struct cache *cache, sector_t s,
					 sector_t *remain)
{
	if (remain)
		*remain = sector_div(s, SUB_BLOCK_SIZE);
	else
		sector_div(s, SUB_BLOCK_SIZE);
	return (unsigned int)s;
}

static unsigned int cache_block_to_sbit(struct cache *cache, dm_cblock_t b)
{
	sector_t s = from_cblock(b);

	if (block_size_is_power_of_two(cache))
		s <<= cache->sectors_per_block_shift;
	else
		s *= cache->sectors_per_block;

	return cache_sector_to_sbit(cache, s, NULL);
}

static void remap_to_cache_sector(struct cache *cache,
				  sector_t *s, dm_cblock_t b);

static unsigned int origin_sector_to_sbit(struct cache *cache,
					  dm_cblock_t cblock, sector_t s,
					  sector_t *remain)
{
	remap_to_cache_sector(cache, &s, cblock);
	return cache_sector_to_sbit(cache, s, remain);
}

static bool is_dirty(struct cache *cache, dm_cblock_t b)
{
	return test_bit(from_cblock(b), cache->dirty_bitset);
}

static void __clear_dirty(struct cache_client *client, dm_cblock_t cblock)
{
	struct cache *cache = client->cache;

	if (test_and_clear_bit(from_cblock(cblock), cache->dirty_bitset)) {
		atomic_dec(&client->nr_dirty);
		if (atomic_dec_and_test(&cache->nr_dirty))
			dm_table_event(cache->ti->table);
	}

	policy_clear_dirty(client->policy, cblock);
}

static void set_clear_sub_bitset(struct cache *cache, dm_cblock_t b,
				 unsigned long *bitset, unsigned long *map,
				 bool clear)
{
	unsigned int start = cache_block_to_sbit(cache, b);

	if (start & (BITS_PER_LONG - 1)) {
		unsigned int rs, re;

		bitmap_for_each_set_region(map, rs, re, 0,
					   cache->sub_blocks_per_cblock) {
			if (clear)
				bitmap_clear(bitset, rs + start, re - rs);
			else
				bitmap_set(bitset, rs + start, re - rs);
		}
	} else {
		unsigned long *cblock_p;

		cblock_p = bitset + BITS_TO_LONGS(start);
		if (clear)
			bitmap_andnot(cblock_p, cblock_p, map,
				      cache->sub_blocks_per_cblock);
		else
			bitmap_or(cblock_p, cblock_p, map,
				  cache->sub_blocks_per_cblock);
	}
}

static void metadata_operation_failed(struct cache *cache, const char *op, int r);

static int __do_valid_bitset(struct cache *cache, dm_cblock_t b, unsigned long *valid, bool clear)
{
	int r;
	unsigned long flags;

	spin_lock_irqsave(cblock_lock(cache, b), flags);
	set_clear_sub_bitset(cache, b, cache->sub_valid_bitset, valid, clear);
	spin_unlock_irqrestore(cblock_lock(cache, b), flags);

	r = dm_cache_set_sub_valid_bits(cache->cmd,
					(uint32_t)cache_block_to_sbit(cache, b),
					cache->sub_blocks_per_cblock, valid,
					clear);
	if (r)
		metadata_operation_failed(cache, "dm_cache_set_sub_valid_bits",
					  r);

	return r;
}

static inline int set_valid_bitset(struct cache *cache, dm_cblock_t b,
				   unsigned long *valid)
{
	return __do_valid_bitset(cache, b, valid, false);
}

static int clear_valid_bitset(struct cache *cache, dm_cblock_t b,
			      unsigned long *valid)
{
	return __do_valid_bitset(cache, b, valid, true);
}

static int __do_valid_bitset_full(struct cache *cache, dm_cblock_t b, bool clear)
{
	int r;
	unsigned long flags;

	spin_lock_irqsave(cblock_lock(cache, b), flags);
	if (clear)
		bitmap_clear(cache->sub_valid_bitset,
			     cache_block_to_sbit(cache, b),
			     cache->sub_blocks_per_cblock);
	else
		bitmap_set(cache->sub_valid_bitset,
			   cache_block_to_sbit(cache, b),
			   cache->sub_blocks_per_cblock);
	spin_unlock_irqrestore(cblock_lock(cache, b), flags);

	r = dm_cache_set_sub_valid_bits(cache->cmd,
					(uint32_t)cache_block_to_sbit(cache, b),
					cache->sub_blocks_per_cblock, NULL,
					clear);
	if (r)
		metadata_operation_failed(cache, "dm_cache_set_sub_valid_bits",
					  r);

	return r;
}

static inline int clear_valid_bitset_full(struct cache *cache, dm_cblock_t b)
{
	return __do_valid_bitset_full(cache, b, true);
}

static inline int set_valid_bitset_full(struct cache *cache, dm_cblock_t b)
{
	return __do_valid_bitset_full(cache, b, false);
}

static inline unsigned find_next_bit_subrange(unsigned long *, unsigned long,
											  unsigned long, unsigned int);
static inline unsigned find_next_zero_bit_subrange(unsigned long *, unsigned long,
												   unsigned long, unsigned int);
static void clear_dirty_bitset(struct cache_client *client, dm_cblock_t b,
			       unsigned long *clean_bitset)
{
	bool clean;
	unsigned long flags;
	struct cache *cache = client->cache;

	spin_lock_irqsave(cblock_lock(cache, b), flags);
	set_clear_sub_bitset(cache, b, cache->sub_dirty_bitset,
			     clean_bitset, true);
	clean = find_next_bit_subrange(cache->sub_dirty_bitset,
				       cache->sub_blocks_per_cblock,
					   cache->sub_cache_size,
				       cache_block_to_sbit(cache, b)) >=
				       cache_block_to_sbit(cache, b + 1);
	spin_unlock_irqrestore(cblock_lock(cache, b), flags);

	if (clean)
		__clear_dirty(client, b);
}

static void set_sub_dirty_bio(struct cache *cache,
			      dm_cblock_t cblock,
			      struct bio *bio)
{
	unsigned int head, tail;
	unsigned long flags;

	/* do partial dirty here */
	head = cache_sector_to_sbit(cache, bio->bi_iter.bi_sector, NULL);
	tail = cache_sector_to_sbit(cache, bio_end_sector(bio) - 1, NULL);

	spin_lock_irqsave(cblock_lock(cache, cblock), flags);
	bitmap_set(cache->sub_dirty_bitset, head, tail - head + 1);
	spin_unlock_irqrestore(cblock_lock(cache, cblock), flags);
}

/* this bio must already be remapped to cache device */
static void set_dirty_bio(struct cache_client *client, dm_cblock_t cblock, struct bio *bio)
{
	struct cache *cache = client->cache;

	if (!test_and_set_bit(from_cblock(cblock), cache->dirty_bitset)) {
		//atomic_inc(&cache->nr_dirty);
		inc_nr_dirty(client);
		policy_set_dirty(client->policy, cblock);
	}
	set_sub_dirty_bio(cache, cblock, bio);
}

static void set_dirty_range(struct cache_client *client, dm_cblock_t cblock,
			    unsigned int offset, unsigned int len)
{
	unsigned long flags;
	struct cache *cache = client->cache;

	if (!test_and_set_bit(from_cblock(cblock), cache->dirty_bitset)) {
		inc_nr_dirty(client);
		policy_set_dirty(client->policy, cblock);
	}

	spin_lock_irqsave(cblock_lock(cache, cblock), flags);
	bitmap_set(cache->sub_dirty_bitset, offset, len);
	spin_unlock_irqrestore(cblock_lock(cache, cblock), flags);
}

/*
 * These two are called when setting after migrations to force the policy
 * and dirty bitset to be in sync.
 */
static void force_set_dirty_bio(struct cache_client *client, dm_cblock_t cblock,
				struct bio *bio)
{
	struct cache *cache = client->cache;

	if (!test_and_set_bit(from_cblock(cblock), cache->dirty_bitset))
		//atomic_inc(&cache->nr_dirty);
		inc_nr_dirty(client);
	policy_set_dirty(client->policy, cblock);
	set_sub_dirty_bio(cache, cblock, bio);
}

static void __clear_dirty_subrange(struct cache *cache, dm_cblock_t cblock,
				   unsigned head, unsigned len)
{
	head += cache_block_to_sbit(cache, cblock);
	bitmap_clear(cache->sub_dirty_bitset, head, len);
}

static void clear_dirty_subrange(struct cache *cache, dm_cblock_t cblock,
				 unsigned head, unsigned len)
{
	unsigned long flags;

	spin_lock_irqsave(cblock_lock(cache, cblock), flags);
	__clear_dirty_subrange(cache, cblock, head, len);
	spin_unlock_irqrestore(cblock_lock(cache, cblock), flags);
}

static void force_clear_dirty(struct cache_client *client, dm_cblock_t cblock)
{
	struct cache *cache = client->cache;

	/* do partial dirty here */
	clear_dirty_subrange(cache, cblock, 0, cache->sub_blocks_per_cblock);
	__clear_dirty(client, cblock);
}

/*----------------------------------------------------------------*/

static dm_block_t block_div(dm_block_t b, uint32_t n)
{
	do_div(b, n);

	return b;
}

static dm_block_t oblocks_per_dblock(struct cache_client *client)
{
	struct cache *cache = client->cache;
	dm_block_t oblocks = client->discard_block_size;

	if (block_size_is_power_of_two(cache))
		oblocks >>= cache->sectors_per_block_shift;
	else
		oblocks = block_div(oblocks, cache->sectors_per_block);

	return oblocks;
}

static dm_dblock_t oblock_to_dblock(struct cache_client *client, dm_oblock_t oblock)
{
	return to_dblock(block_div(from_oblock(oblock),
				   oblocks_per_dblock(client)));
}

static void set_sub_valid_and_dirty_bit(void *ptr, dm_block_t offset)
{
	struct cache *cache = (struct cache *)ptr;
	dm_block_t cblock = offset;

	sector_div(cblock, cache->sub_blocks_per_cblock);

	spin_lock_irq(cblock_lock(cache, to_cblock(cblock)));
	set_bit(offset, cache->sub_valid_bitset);
	set_bit(offset, cache->sub_dirty_bitset);
	spin_unlock_irq(cblock_lock(cache, to_cblock(cblock)));
}

static void set_sub_valid_bit(void *ptr, dm_block_t offset)
{
	struct cache *cache = (struct cache *)ptr;
	dm_block_t cblock = offset;

	sector_div(cblock, cache->sub_blocks_per_cblock);

	spin_lock_irq(cblock_lock(cache, to_cblock(cblock)));
	set_bit(offset, cache->sub_valid_bitset);
	spin_unlock_irq(cblock_lock(cache, to_cblock(cblock)));
}

static void set_sub_dirty_bit(void *ptr, dm_block_t offset)
{
	struct cache *cache = (struct cache *)ptr;
	dm_block_t cblock = offset;

	sector_div(cblock, cache->sub_blocks_per_cblock);
	spin_lock_irq(cblock_lock(cache, to_cblock(cblock)));
	set_bit(offset, cache->sub_dirty_bitset);
	spin_unlock_irq(cblock_lock(cache, to_cblock(cblock)));
}

static void set_discard(void *ptr, dm_block_t b)
{
	struct cache_client *client = (struct cache_client *)ptr;

	BUG_ON(b >= from_dblock(client->discard_nr_blocks));
	atomic_inc(&client->stats.discard_count);

	spin_lock_irq(&client->lock);
	set_bit(b, client->discard_bitset);
	spin_unlock_irq(&client->lock);
}

static void clear_discard(struct cache_client *client, dm_dblock_t b)
{
	spin_lock_irq(&client->lock);
	clear_bit(from_dblock(b), client->discard_bitset);
	spin_unlock_irq(&client->lock);
}

static bool is_discarded(struct cache_client *client, dm_dblock_t b)
{
	int r;
	spin_lock_irq(&client->lock);
	r = test_bit(from_dblock(b), client->discard_bitset);
	spin_unlock_irq(&client->lock);

	return r;
}

static bool is_discarded_oblock(struct cache_client *client, dm_oblock_t b)
{
	int r;
	spin_lock_irq(&client->lock);
	r = test_bit(from_dblock(oblock_to_dblock(client, b)),
		     client->discard_bitset);
	spin_unlock_irq(&client->lock);

	return r;
}

/*----------------------------------------------------------------*/

static void inc_disk_io_counter(struct cache_client *client, int rw)
{
	struct cache *cache = client->cache;

	atomic_inc(rw == READ ?
		   &client->stats.disk_reads : &client->stats.disk_writes);
	atomic_inc(rw == READ ?
		   &cache->stats.disk_reads : &cache->stats.disk_writes);
}

static void inc_ssd_io_counter(struct cache_client *client, int rw)
{
	struct cache *cache = client->cache;

	atomic_inc(rw == READ ?
		   &client->stats.ssd_reads : &client->stats.ssd_writes);
	atomic_inc(rw == READ ?
		   &cache->stats.ssd_reads : &cache->stats.ssd_writes);
}

static void inc_rw_counter(struct cache_client *client, struct bio *bio)
{
	struct cache *cache = client->cache;

	atomic_inc(bio_data_dir(bio) == READ ?
		   &client->stats.reads : &client->stats.writes);
	atomic_inc(bio_data_dir(bio) == READ ?
		   &cache->stats.reads : &cache->stats.writes);

	if (bio_data_dir(bio) == READ)
		client->cache->cur_stats.reads++;
	else
		client->cache->cur_stats.writes++;
}

static void inc_dirty_write_hit(struct cache_client *client)
{
	struct cache *cache = client->cache;

	atomic_inc(&client->stats.dirty_write_hit);
	atomic_inc(&cache->stats.dirty_write_hit);
	client->cache->cur_stats.dirty_write_hits++;
}

static void inc_hit_counter(struct cache_client *client, struct bio *bio, dm_cblock_t cblock)
{
	struct cache *cache = client->cache;

	atomic_inc(bio_data_dir(bio) == READ ?
		   &client->stats.read_hit : &client->stats.write_hit);
	atomic_inc(bio_data_dir(bio) == READ ?
		   &cache->stats.read_hit : &cache->stats.write_hit);

	if (bio_data_dir(bio) == READ)
		client->cache->cur_stats.read_hits++;
	else
		client->cache->cur_stats.write_hits++;

	inc_rw_counter(client, bio);

	if (is_dirty(client->cache, cblock) && bio_data_dir(bio) == WRITE)
		inc_dirty_write_hit(client);
}

static void inc_miss_counter(struct cache_client *client, struct bio *bio)
{
	struct cache *cache = client->cache;

	atomic_inc(bio_data_dir(bio) == READ ?
		   &client->stats.read_miss : &client->stats.write_miss);
	atomic_inc(bio_data_dir(bio) == READ ?
		   &cache->stats.read_miss : &cache->stats.write_miss);

	inc_rw_counter(client, bio);
}

static void inc_uncached_seq_counter(struct cache_client *client, struct bio *bio)
{
	struct cache *cache = client->cache;

	atomic_inc(bio_data_dir(bio) == READ ?
		   &client->stats.uncached_seq_reads : &client->stats.uncached_seq_writes);
	atomic_inc(bio_data_dir(bio) == READ ?
		   &cache->stats.uncached_seq_reads : &cache->stats.uncached_seq_writes);
	inc_disk_io_counter(client, bio_data_dir(bio));
}

static void inc_uncached_counter(struct cache_client *client, struct bio *bio, int skip)
{
	struct cache *cache = client->cache;

	atomic_inc(bio_data_dir(bio) == READ ?
		   &client->stats.uncached_reads : &client->stats.uncached_writes);
	atomic_inc(bio_data_dir(bio) == READ ?
		   &cache->stats.uncached_reads : &cache->stats.uncached_writes);
	inc_rw_counter(client, bio);
	inc_disk_io_counter(client, bio_data_dir(bio));

	if (skip)
		inc_uncached_seq_counter(client, bio);
}

static void inc_disk_ioerror(struct cache_client *client)
{
	atomic_inc(&client->err_stats.disk_errs);
}

static void inc_ssd_ioerror(struct cache_client *client)
{
	atomic_inc(&client->err_stats.ssd_errs);
	atomic_inc(&client->cache->err_stats.ssd_errs);
}

/*----------------------------------------------------------------
 * Remapping
 *--------------------------------------------------------------*/
static void remap_to_origin(struct cache_client *client, struct bio *bio)
{
	bio_set_dev(bio, client->origin_dev->bdev);

	if (!op_is_flush(bio->bi_opf) && !op_is_discard(bio->bi_opf))
		inc_disk_io_counter(client, bio_data_dir(bio));
}

static void remap_to_cache_sector(struct cache *cache,
				  sector_t *s, dm_cblock_t b)
{
	sector_t sector = *s;
	sector_t block = from_cblock(b);

	if (!block_size_is_power_of_two(cache))
		*s = (block * cache->sectors_per_block) +
		      sector_div(sector, cache->sectors_per_block);
	else
		*s = (block << cache->sectors_per_block_shift) |
		      (sector & (cache->sectors_per_block - 1));
}

static void remap_to_cache(struct cache_client *client, struct bio *bio,
			   dm_cblock_t cblock)
{
	struct cache *cache = client->cache;

	bio_set_dev(bio, cache->cache_dev);
	remap_to_cache_sector(cache, &bio->bi_iter.bi_sector, cblock);

	if (!op_is_flush(bio->bi_opf))
		inc_ssd_io_counter(client, bio_data_dir(bio));
}

static void check_if_tick_bio_needed(struct cache *cache, struct bio *bio)
{
	struct per_bio_data *pb;

	spin_lock_irq(&cache->lock);
	if (cache->need_tick_bio && !op_is_flush(bio->bi_opf) &&
	    bio_op(bio) != REQ_OP_DISCARD) {
		pb = get_per_bio_data(bio);
		pb->tick = true;
		cache->need_tick_bio = false;
	}
	spin_unlock_irq(&cache->lock);
}

static void __remap_to_origin_clear_discard(struct cache_client *client, struct bio *bio,
					    dm_oblock_t oblock, bool bio_has_pbd)
{
	struct cache *cache = client->cache;

	if (bio_has_pbd)
		check_if_tick_bio_needed(cache, bio);
	remap_to_origin(client, bio);
	if (bio_data_dir(bio) == WRITE)
		clear_discard(client, oblock_to_dblock(client, oblock));
}

static void remap_to_origin_clear_discard(struct cache_client *client, struct bio *bio,
					  dm_oblock_t oblock)
{
	// FIXME: check_if_tick_bio_needed() is called way too much through this interface
	__remap_to_origin_clear_discard(client, bio, oblock, true);
}

static void remap_to_cache_dirty(struct cache_client *client, struct bio *bio,
				 dm_oblock_t oblock, dm_cblock_t cblock)
{
	struct cache *cache = client->cache;

	check_if_tick_bio_needed(cache, bio);
	remap_to_cache(client, bio, cblock);
	if (bio_data_dir(bio) == WRITE) {
		set_dirty_bio(client, cblock, bio);
		clear_discard(client, oblock_to_dblock(client, oblock));
	}
}

static dm_oblock_t get_bio_block(struct cache *cache, struct bio *bio)
{
	sector_t block_nr = bio->bi_iter.bi_sector;

	if (!block_size_is_power_of_two(cache))
		(void) sector_div(block_nr, cache->sectors_per_block);
	else
		block_nr >>= cache->sectors_per_block_shift;

	return to_oblock(block_nr);
}

static bool accountable_bio(struct cache *cache, struct bio *bio)
{
	return bio_op(bio) != REQ_OP_DISCARD;
}

static void accounted_begin(struct cache *cache, struct bio *bio)
{
	struct per_bio_data *pb;

	if (accountable_bio(cache, bio)) {
		pb = get_per_bio_data(bio);
		pb->len = bio_sectors(bio);
		iot_io_begin(&cache->tracker, pb->len);
	}
}

static void accounted_complete(struct cache *cache, struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	iot_io_end(&cache->tracker, pb->len);
}

static void accounted_and_submit(struct cache *cache, struct bio *bio)
{
	accounted_begin(cache, bio);
	submit_bio_noacct(bio);
}

static void issue_op(struct bio *bio, void *context)
{
	struct cache *cache = context;
	accounted_and_submit(cache, bio);
}

/*
 * When running in writethrough mode we need to send writes to clean blocks
 * to both the cache and origin devices.  Clone the bio and send them in parallel.
 */
static void remap_to_origin_and_cache(struct cache_client *client, struct bio *bio,
				      dm_oblock_t oblock, dm_cblock_t cblock)
{
	struct cache *cache = client->cache;
	struct bio *origin_bio = bio_clone_fast(bio, GFP_NOIO, &cache->bs);

	BUG_ON(!origin_bio);

	bio_chain(origin_bio, bio);
	/*
	 * Passing false to __remap_to_origin_clear_discard() skips
	 * all code that might use per_bio_data (since clone doesn't have it)
	 */
	__remap_to_origin_clear_discard(client, origin_bio, oblock, false);
	submit_bio(origin_bio);

	remap_to_cache(client, bio, cblock);
}

/*----------------------------------------------------------------
 * Failure modes
 *--------------------------------------------------------------*/
static enum cache_metadata_mode get_cache_mode(struct cache *cache)
{
	return cache->features.mode;
}

static const char *cache_device_name(struct cache *cache)
{
	return dm_table_device_name(cache->ti->table);
}

static const char *client_device_name(struct cache_client *client)
{
	return dm_table_device_name(client->ti->table);
}

static void notify_mode_switch(struct cache *cache, enum cache_metadata_mode mode)
{
	const char *descs[] = {
		"write",
		"read-only",
		"fail"
	};

	dm_table_event(cache->ti->table);
	DMINFO("%s: switching cache to %s mode",
	       cache_device_name(cache), descs[(int)mode]);
}

static void set_cache_mode(struct cache *cache, enum cache_metadata_mode new_mode)
{
	bool needs_check;
	enum cache_metadata_mode old_mode = get_cache_mode(cache);

	if (dm_cache_metadata_needs_check(cache->cmd, &needs_check)) {
		DMERR("%s: unable to read needs_check flag, setting failure mode.",
		      cache_device_name(cache));
		new_mode = CM_FAIL;
	}

	if (new_mode == CM_WRITE && needs_check) {
		DMERR("%s: unable to switch cache to write mode until repaired.",
		      cache_device_name(cache));
		if (old_mode != new_mode)
			new_mode = old_mode;
		else
			new_mode = CM_READ_ONLY;
	}

	/* Never move out of fail mode */
	if (old_mode == CM_FAIL)
		new_mode = CM_FAIL;

	switch (new_mode) {
	case CM_FAIL:
	case CM_READ_ONLY:
		dm_cache_metadata_set_read_only(cache->cmd);
		break;

	case CM_WRITE:
		dm_cache_metadata_set_read_write(cache->cmd);
		break;
	}

	cache->features.mode = new_mode;

	if (new_mode != old_mode)
		notify_mode_switch(cache, new_mode);
}

static void abort_transaction(struct cache *cache)
{
	const char *dev_name = cache_device_name(cache);

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return;

	if (dm_cache_metadata_set_needs_check(cache->cmd)) {
		DMERR("%s: failed to set 'needs_check' flag in metadata", dev_name);
		set_cache_mode(cache, CM_FAIL);
	}

	DMERR_LIMIT("%s: aborting current metadata transaction", dev_name);
	if (dm_cache_metadata_abort(cache->cmd)) {
		DMERR("%s: failed to abort metadata transaction", dev_name);
		set_cache_mode(cache, CM_FAIL);
	}
}

static void metadata_operation_failed(struct cache *cache, const char *op, int r)
{
	DMERR_LIMIT("%s: metadata operation '%s' failed: error = %d",
		    cache_device_name(cache), op, r);
	abort_transaction(cache);
	set_cache_mode(cache, CM_READ_ONLY);
}

/*----------------------------------------------------------------*/

static void load_stats(struct cache_client *client)
{
	struct dm_cache_statistics stats;

	dm_cache_client_metadata_get_stats(client->ccmd, &stats);
	atomic_set(&client->stats.read_hit, stats.read_hits);
	atomic_set(&client->stats.read_miss, stats.read_misses);
	atomic_set(&client->stats.write_hit, stats.write_hits);
	atomic_set(&client->stats.write_miss, stats.write_misses);
}

static void save_stats(struct cache_client *client)
{
	struct dm_cache_statistics stats;

	if (get_cache_mode(client->cache) >= CM_READ_ONLY)
		return;

	stats.read_hits = atomic_read(&client->stats.read_hit);
	stats.read_misses = atomic_read(&client->stats.read_miss);
	stats.write_hits = atomic_read(&client->stats.write_hit);
	stats.write_misses = atomic_read(&client->stats.write_miss);

	dm_cache_client_metadata_set_stats(client->ccmd, &stats);
}

static void update_stats(struct cache_stats *stats, enum policy_operation op)
{
	switch (op) {
	case POLICY_PROMOTE:
		atomic_inc(&stats->promotion);
		break;

	case POLICY_ABANDON:
	case POLICY_DEMOTE:
		atomic_inc(&stats->demotion);
		break;

	case POLICY_WRITEBACK_F:
	case POLICY_WRITEBACK:
		atomic_inc(&stats->writeback);
		break;
	}
}

/*----------------------------------------------------------------
 * Migration processing
 *
 * Migration covers moving data from the origin device to the cache, or
 * vice versa.
 *--------------------------------------------------------------*/

static void inc_io_migrations(struct cache *cache)
{
	atomic_inc(&cache->nr_io_migrations);
}

static void dec_io_migrations(struct cache *cache)
{
	atomic_dec(&cache->nr_io_migrations);
}

static bool discard_or_flush(struct bio *bio)
{
	return bio_op(bio) == REQ_OP_DISCARD || (bio->bi_opf & REQ_PREFLUSH);
}

static void calc_discard_block_range(struct cache_client *client, struct bio *bio,
				     dm_dblock_t *b, dm_dblock_t *e)
{
	sector_t sb = bio->bi_iter.bi_sector;
	sector_t se = bio_end_sector(bio);

	*b = to_dblock(dm_sector_div_up(sb, client->discard_block_size));

	if (se - sb < client->discard_block_size)
		*e = *b;
	else
		*e = to_dblock(block_div(se, client->discard_block_size));
}

/*----------------------------------------------------------------*/

static void prevent_background_work(struct cache_client *client)
{
	lockdep_off();
	down_write(&client->background_work_lock);
	lockdep_on();
}

static void allow_background_work(struct cache_client *client)
{
	lockdep_off();
	up_write(&client->background_work_lock);
	lockdep_on();
}

static bool background_work_begin(struct cache_client *client)
{
	bool r;

	lockdep_off();
	r = down_read_trylock(&client->background_work_lock);
	lockdep_on();

	return r;
}

static void background_work_end(struct cache_client *client)
{
	lockdep_off();
	up_read(&client->background_work_lock);
	lockdep_on();
}

/*----------------------------------------------------------------*/
static int skip_sequential_io(struct cache_client *client, struct bio *bio);
/*
 * With sub block patch, we can optimise every bio with aligned head
 * and at least one SUB_BLOCK_SIZE big
 */

static bool sub_bio_optimisable(struct bio *bio)
{
	return !(bio_sectors(bio) & (SUB_BLOCK_SIZE - 1)) &&
	       !(bio->bi_iter.bi_sector & (SUB_BLOCK_SIZE - 1));
}

static inline bool optimisable_bio(struct cache_client *client, struct bio *bio,
				   dm_oblock_t block)
{
	/* We no longer check discarded bio since we can handle partial cache now */
	/* basically we could drop complete block check */

	return writeback_mode(client) && sub_bio_optimisable(bio);
}

typedef void (*continuation_fn)(struct work_struct *);

static void quiesce(struct dm_cache_migration *mg,
		    continuation_fn continuation)
{
	init_continuation(&mg->k, continuation);
	dm_cell_quiesce_v2(mg_to_cache(mg)->prison, mg->cell, &mg->k.ws);
}

static struct dm_cache_migration *ws_to_mg(struct work_struct *ws)
{
	struct continuation *k = container_of(ws, struct continuation, ws);
	return container_of(k, struct dm_cache_migration, k);
}

static void policy_mg_complete(struct dm_cache_migration *mg, bool success)
{
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	struct policy_work *op = mg->op;
	dm_cblock_t cblock = op->cblock;

	if (success)
		update_stats(&client->stats, op->op);

	switch (op->op) {
	case POLICY_PROMOTE:
		if (mg->optimisable_bio) {
			struct per_bio_data *pb = get_per_bio_data(mg->optimisable_bio);
			/*
			 * There is nothing we can do if metadata update failed.
			 * We might wanna remap this bio directly to origin.
			 */
			pb->mg_phase2 = 1;
			if (success) {
				BUG_ON(bio_detain_shared(client, mg->op->oblock, mg->optimisable_bio));
				atomic_inc(&mg2_io);
			} else {
				bio_io_error(mg->optimisable_bio);
				atomic_dec(&mg3_io);
				atomic_dec(&in_io);
				if (pb->rw) {
					pb->rw = 0;
					atomic_dec(&rw_io);
				}
			}
			atomic_dec(&mg_io);
		}
		policy_complete_background_work(client->policy, op, success);
		if (success)
			force_clear_dirty(client, cblock);

		/*
		 * Now we can have only two cases for policy promotion:
		 * 1. bio trigger foreground promotion - we only update metadata
		 *    in this case and reset all valid and dirty sub bits.
		 * 2. background full block promotion
		 */
		if (!mg->optimisable_bio) {
			clear_discard(client,
				      oblock_to_dblock(client, op->oblock));
			dec_io_migrations(cache);
		}
		break;

	case POLICY_ABANDON:
	case POLICY_DEMOTE:
		/*
		 * We clear dirty here to update the nr_dirty counter.
		 */
	case POLICY_WRITEBACK_F:
	case POLICY_WRITEBACK:
		// FIXME: check cell bitmap to set dirty status
		if (success)
			force_clear_dirty(client, cblock);
		policy_complete_background_work(client->policy, op, success);
		dec_io_migrations(cache);
		break;
	}
}

static void target_mg_complete(struct dm_cache_migration *mg, bool success)
{
	dm_cblock_t cblock = mg->cblock;
	struct cache_client *client = mg->client;

	switch (mg_operation(mg)) {
	case MG_PROMOTE:
		if (mg->optimisable_bio) {
			struct per_bio_data *pb = get_per_bio_data(mg->optimisable_bio);

			BUG_ON(bio_data_dir(mg->optimisable_bio) != WRITE);
			if (success && writeback_mode(client))
				force_set_dirty_bio(client, cblock, mg->optimisable_bio);
			else if (!success || mg->k.input)
				mg->optimisable_bio->bi_status = mg->k.input ?: BLK_STS_IOERR;

			if (pb->rw) {
				pb->rw = 0;
				atomic_dec(&rw_io);
			}
			atomic_dec(&in_io);
			atomic_dec(&h_io);
			bio_endio(mg->optimisable_bio);
			mg->optimisable_bio = NULL;
			break;
		} else {
			dec_io_migrations(client->cache);
		}
		/* fallthrough */
	case MG_DEMOTE:
		if (success)
			clear_dirty_bitset(client, cblock, mg->migrated_sblock);

		if (mg->optimisable_bio) {
			struct per_bio_data *pb = get_per_bio_data(mg->optimisable_bio);

			if (!success)
				mg->optimisable_bio->bi_status = mg->k.input ?: BLK_STS_IOERR;

			if (pb->rw) {
				pb->rw = 0;
				atomic_dec(&rw_io);
			}
			atomic_dec(&in_io);
			atomic_dec(&h_io);
			bio_endio(mg->optimisable_bio);
		}

		break;
	case MG_INVALIDATE:
	case MG_NONE:
		BUG_ON(1);
	}
}

static inline bool policy_operation(struct dm_cache_migration *mg)
{
	return mg->op? true: false;
}

/*
 * Migration steps:
 *
 * 1) exclusive lock preventing WRITEs
 * 2) quiesce
 * 3) copy or issue overwrite bio
 * 4) upgrade to exclusive lock preventing READs and WRITEs
 * 5) quiesce
 * 6) update metadata and commit
 * 7) unlock
 */
static void mg_complete(struct dm_cache_migration *mg, bool success)
{
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	bool policy_op = policy_operation(mg);

	if (policy_op)
		policy_mg_complete(mg, success);
	else
		target_mg_complete(mg, success);

	mg_drop_lock(mg);

	free_migration(mg);
	wake_migration_worker(cache);

	if (policy_op)
		background_work_end(client);
}

static void migration_get(struct dm_cache_migration *mg)
{
	refcount_inc(&mg->sub_migration);
}

static void migration_ref_init(struct dm_cache_migration *mg)
{
	refcount_set(&mg->sub_migration, 1);
}

static void migration_put(unsigned long error, void *context)
{
	struct dm_cache_migration *mg = context;

	if (error)
		mg->error = true;

	if (refcount_dec_and_test(&mg->sub_migration)) {
		if (mg->error && mg->k.input == BLK_STS_OK)
			mg->k.input = BLK_STS_IOERR;

		queue_continuation(mg_to_cache(mg)->wq, &mg->k);
	}
}

static void writeback_complete(int read_err, unsigned long write_err, void *context)
{
	migration_put(write_err || read_err, context);
}

static void mg_update_metadata(struct work_struct *ws);

static void mg_update_valid_bitset(struct work_struct *ws)
{
	int r = 0;
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache *cache = mg_to_cache(mg);

	if (mg->k.input) {
		mg_complete(mg, false);
		return;
	}

	switch (mg_operation(mg)) {
	case MG_PROMOTE:
		r = set_valid_bitset(cache, mg->cblock, mg->migrated_sblock);
		break;
	case MG_DEMOTE:
		r = clear_valid_bitset(cache, mg->cblock, mg->migrated_sblock);
		break;
	case MG_INVALIDATE:
	case MG_NONE:
		BUG_ON(1);
	}

	if (r)
		DMERR_LIMIT(
			"%s: migration failed; couldn't update on disk bitset",
			cache_device_name(cache));
	mg_complete(mg, !r);
}

static void mg_queue_work(struct dm_cache_migration *mg, struct mg_worker *w)
{
	struct cache *cache = mg_to_cache(mg);

	spin_lock_irq(&w->lock);
	list_add_tail(&mg->list, &w->works);
	spin_unlock_irq(&w->lock);

	queue_work(cache->wq, &w->worker);
}

static void mg_queue_metadata_update(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache *cache = mg_to_cache(mg);

	mg_queue_work(mg, &cache->metadata_write_worker);
}

static void mg_upgrade_lock(struct work_struct *ws)
{
	int r;
	struct dm_cache_migration *mg = ws_to_mg(ws);

	/*
	 * Did the copy succeed?
	 */
	if (mg->k.input)
		mg_complete(mg, false);

	else {
		/*
		 * Now we want the lock to prevent both reads and writes.
		 */
		r = dm_cell_lock_promote_v2(mg_to_cache(mg)->prison, mg->cell,
                                            &mg->key, READ_WRITE_LOCK_LEVEL);
		if (r < 0)
			mg_complete(mg, false);

		else if (r)
			quiesce(mg, mg_queue_metadata_update);

		else
			mg_queue_metadata_update(ws);
	}
}

static void writeback(struct dm_cache_migration *mg)
{
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	unsigned int rs, re, start, end;
	struct dm_io_region o_region, c_region;
	int count = 0;

	migration_ref_init(mg);
	init_continuation(&mg->k, mg_upgrade_lock);

	/* 
	 * We don't support sub block size other than 4K for now,
	 * but this could be easily changed
	 */
	BUG_ON(SUB_BLOCK_SIZE != PAGE_SIZE >> SECTOR_SHIFT);

	start = cache_block_to_sbit(cache, mg->op->cblock);
	end   = cache_block_to_sbit(cache, mg->op->cblock + 1);

	bitmap_for_each_set_region(cache->sub_dirty_bitset, rs, re, start, end) {
		o_region.bdev = client->origin_dev->bdev;
		o_region.sector = oblock_to_sector(cache, mg->op->oblock) + (rs - start) * SUB_BLOCK_SIZE;
		o_region.count = (re - rs) * SUB_BLOCK_SIZE;

		c_region.bdev = cache->cache_dev;
		c_region.sector = rs * SUB_BLOCK_SIZE;
		c_region.count = o_region.count;

		migration_get(mg);
		bitmap_set(mg->migrated_sblock, rs - start, re - rs);

		count++;
		dm_kcopyd_copy(cache->copier, &c_region, 1, &o_region, 0, writeback_complete, mg);
		inc_ssd_io_counter(client, READ);
		inc_disk_io_counter(client, WRITE);
	}

	migration_put(0, mg);
}

static void bio_drop_shared_lock(struct cache_client *client, struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	if (pb->cell && dm_cell_put_v2(client->cache->prison, pb->cell, pb->cell_id))
		free_prison_cell(client->cache, pb->cell);
	pb->cell = NULL;
}

static void bio_work_dec(struct bio *bio, int error)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	pb->error |= error;
	if (refcount_dec_and_test(&pb->count)) {
		bio_drop_shared_lock(pb->client, bio);
		if (pb->rw) {
			pb->rw = 0;
			atomic_dec(&rw_io);
		}
		if (pb->error)
			bio_io_error(bio);
		else
			bio_endio(bio);
		atomic_dec(&in_io);
		atomic_dec(&h_io);
	}
}

static void bio_work_inc(struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	refcount_inc(&pb->count);
}

static void bio_work_ref_init(struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	refcount_set(&pb->count, 1);
}

static void cache_io_finish(unsigned long error, void *context)
{
	struct per_bio_data *pb = get_per_bio_data((struct bio *)context);

	if (error)
		inc_ssd_ioerror(pb->client);

	bio_work_dec((struct bio *)context, error);
}

static void origin_io_finish(unsigned long error, void *context)
{
	struct per_bio_data *pb = get_per_bio_data((struct bio *)context);

	if (error)
		inc_disk_ioerror(pb->client);

	bio_work_dec((struct bio *)context, error);
}

static void mg_origin_io_finish(unsigned long error, void *context)
{
	struct dm_cache_migration *mg = context;

	if (error)
		inc_disk_ioerror(mg->client);

	migration_put(error, mg);
}

/*
 * @pos : bits from bio start
 * @shift : bits from cblock start
 */
static void read_disk(struct cache_client *client, struct bio *bio,
		      unsigned offset, unsigned len)
{
	int r;
	struct cache *cache = client->cache;
	struct dm_io_region region;
	struct dm_io_request io_req = {
		.bi_op = REQ_OP_READ,
		.bi_op_flags = 0,
		.mem.type = DM_IO_BIO,
		.mem.ptr.bio = bio,
		.client = cache->io_client,
	};
	struct per_bio_data *pb = get_per_bio_data(bio);
	dm_oblock_t b = get_bio_block(cache, bio);

	if (pb->mg) {
		io_req.notify.fn = mg_origin_io_finish;
		io_req.notify.context = pb->mg;
		migration_get(pb->mg);
		bitmap_set(pb->mg->migrated_sblock, offset, len);
	} else {
		io_req.notify.fn = origin_io_finish;
		io_req.notify.context = bio;
		bio_work_inc(bio);
	}

	region.bdev = client->origin_dev->bdev;
	region.sector = oblock_to_sector(cache, b) + offset * SUB_BLOCK_SIZE;
	region.count = len * SUB_BLOCK_SIZE;

	r = dm_io(&io_req, 1, &region, NULL);
	if (r)
		io_req.notify.fn(1, io_req.notify.context);

	inc_disk_io_counter(client, READ);
}

static void read_cache(struct cache_client *client, struct bio *bio,
		       dm_cblock_t b, unsigned int offset, unsigned int len)
{
	int r;
	struct cache *cache = client->cache;
	struct dm_io_region region;
	struct dm_io_request io_req = {
		.bi_op = bio_op(bio),
		.bi_op_flags = 0,
		.mem.type = DM_IO_BIO,
		.mem.ptr.bio = bio,
		.notify.fn = cache_io_finish,
		.notify.context = bio,
		.client = cache->io_client,
	};

	bio_work_inc(bio);

	region.bdev = cache->cache_dev;
	region.sector = (cache_block_to_sbit(cache, b) + offset) * SUB_BLOCK_SIZE;
	region.count = len * SUB_BLOCK_SIZE;

	r = dm_io(&io_req, 1, &region, NULL);
	if (r)
		cache_io_finish(1, bio);

	inc_ssd_io_counter(client, READ);
}

static void bio_io_read(struct cache_client *client, struct bio *bio, dm_cblock_t cblock,
                        unsigned int offset, unsigned int len, bool valid)
{
	if (valid)
		read_cache(client, bio, cblock, offset, len);
	else
		read_disk(client, bio, offset, len);
}

typedef void (*bio_io_fn)(struct cache_client *client, struct bio *bio,
			  dm_cblock_t cblock, unsigned int offset,
			  unsigned int len, bool valid);
static void __handle_bio_io(struct cache_client *client, struct bio *bio,
			    dm_cblock_t cblock, bio_io_fn io_fn)
{
	unsigned int valid;
	unsigned int rs, re, start, end, len = 0, chead;
	struct cache *cache = client->cache;
	struct per_bio_data *pb = get_per_bio_data(bio);

	start = origin_sector_to_sbit(cache, cblock, bio->bi_iter.bi_sector, NULL);
	end   = origin_sector_to_sbit(cache, cblock, bio_end_sector(bio) - 1, NULL);
	chead = origin_sector_to_sbit(cache, cblock, 0, NULL);

	valid = test_bit(start, cache->sub_valid_bitset);
	rs = start;

	if (pb->mg)
		bio_work_inc(bio);
	else
		bio_work_ref_init(bio);
	while (rs <= end) {
		bio_advance_iter(bio, &bio->bi_iter,
		                 to_bytes(len * SUB_BLOCK_SIZE));

		if (valid)
			re = find_next_zero_bit_subrange(cache->sub_valid_bitset,
							 cache->sub_blocks_per_cblock,
							 cache->sub_cache_size,
							 rs) - 1;
		else
			re = find_next_bit_subrange(cache->sub_valid_bitset,
						    cache->sub_blocks_per_cblock,
							cache->sub_cache_size,
						    rs) - 1;

		if (re > end)
			re = end;

		len = re - rs + 1;
		io_fn(client, bio, cblock, rs - chead, len, valid);

		valid ^= 1;
		rs = re + 1;
	}
	bio_work_dec(bio, 0);
}

static void bio_io_write(struct cache_client *client, struct bio *bio,
                         dm_cblock_t cblock, unsigned int offset,
                         unsigned int len, bool valid)
{
	int r;
	struct cache *cache = client->cache;
	struct dm_io_region regions[2], *region = regions;
	struct dm_io_request io_req = {
		.bi_op = REQ_OP_WRITE,
		.bi_op_flags = 0,
		.mem.type = DM_IO_BIO,
		.mem.ptr.bio = bio,
		.notify.context = bio,
		.client = cache->io_client,
	};
	dm_oblock_t oblock = get_bio_block(cache, bio);

	switch ((int)valid) {
	case 1:
		bio_work_inc(bio);
		region->bdev = cache->cache_dev;
		region->sector = (cache_block_to_sbit(cache, cblock) + offset) *
				 SUB_BLOCK_SIZE;
		region->count = len * SUB_BLOCK_SIZE;
		inc_ssd_io_counter(client, WRITE);
		region++;
		if (writeback_mode(client)) {
			set_dirty_range(client, cblock, cache_block_to_sbit(cache, cblock) + offset, len);
			break;
		}
		/* fallthrough */
	case 0:
		bio_work_inc(bio);
		region->bdev = client->origin_dev->bdev;
		region->sector = oblock_to_sector(cache, oblock) +
		                 offset * SUB_BLOCK_SIZE;
		region->count = len * SUB_BLOCK_SIZE;
		inc_disk_io_counter(client, WRITE);
		region++;
		break;
	}

	if (valid) {
		io_req.notify.fn = cache_io_finish;
		r = dm_io(&io_req, 1, &regions[0], NULL);
		if (r) {
			atomic_dec(&client->stats.ssd_writes);
			cache_io_finish(1, bio);
		}
	}

	if (region - regions > 1 || !valid) {
		io_req.notify.fn = origin_io_finish;
		if (region - regions > 1)
			r = dm_io(&io_req, 1, &regions[1], NULL);
		else
			r = dm_io(&io_req, 1, &regions[0], NULL);

		if (r) {
			atomic_dec(&client->stats.disk_writes);
			origin_io_finish(1, bio);
		}
	}
}

static inline void handle_bio(struct cache_client *client, struct bio *bio, dm_cblock_t cblock)
{
	accounted_begin(client->cache, bio);
	if (bio_data_dir(bio) == WRITE) {
		__handle_bio_io(client, bio, cblock, bio_io_write);
		clear_discard(client, oblock_to_dblock(client, get_bio_block(client->cache, bio)));
	} else {
		__handle_bio_io(client, bio, cblock, bio_io_read);
	}
}

static int mg_write_page_list(struct dm_cache_migration *mg, sector_t offset,
			      unsigned nr_pages, struct page_list *pl)
{
	int r;
	struct cache *cache = mg_to_cache(mg);
	struct dm_io_request io_req = {
		.bi_op = REQ_OP_WRITE,
		.bi_op_flags = 0,
		.mem.type = DM_IO_PAGE_LIST,
		.mem.offset = 0,
		.mem.ptr.pl = pl,
		.notify.fn = migration_put,
		.notify.context = mg,
		//FIXME: we need to use our own client
		.client = cache->io_client,
	};
	struct dm_io_region region = {
		.bdev = cache->cache_dev,
		.sector = offset,
		.count = nr_pages * SUB_BLOCK_SIZE,
	};

	BUG_ON(!pl);

	migration_get(mg);
	r = dm_io(&io_req, 1, &region, NULL);
	if (r)
		migration_put(1, mg);

	inc_ssd_io_counter(mg->client, WRITE);

	return r;
}

static void page_copy_complete(void *ptr)
{
	struct dm_cache_migration *mg = (struct dm_cache_migration *) ptr;
	struct bio *bio = mg->optimisable_bio;
	struct cache *cache = mg_to_cache(mg);
	unsigned int start, rs, re;
	unsigned long *map = mg->migrated_sblock;
	struct page_list *pl = mg->pl;
	int r = 0;

	bio_work_dec(bio, 0);

	init_continuation(&mg->k, mg_upgrade_lock);
	migration_ref_init(mg);

	start = origin_sector_to_sbit(cache, 0, mg->iter.bi_sector, NULL);
	bitmap_for_each_set_region (map, rs, re, start,
				    cache->sub_blocks_per_cblock) {
		sector_t coff = rs * SUB_BLOCK_SIZE;
		unsigned int cur = rs;
		struct page_list *head_page = pl;

		do {
			BUG_ON(!pl);
			pl = pl->next;
			cur++;
		} while (cur < re);

		remap_to_cache_sector(cache, &coff, mg->cblock);
		r = mg_write_page_list(mg, coff, re - rs, head_page);
		if (r) {
			bitmap_clear(map, rs,
				     cache->sub_blocks_per_cblock - rs);
			break;
		}
	}

	mg->optimisable_bio = NULL;
	inc_io_migrations(cache);
	migration_put(!!r, mg);
}

static void mg_redirect_write(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache *cache = mg_to_cache(mg);
	struct bio *bio = mg->optimisable_bio;
	struct page_list *pl = mg->pl;
	unsigned long *map = mg->migrated_sblock;
	unsigned int start, rs, re;
	struct async_submit_ctl submit;
	enum async_tx_flags flags = 0;
	struct dma_async_tx_descriptor *tx = NULL;

	flags |= ASYNC_TX_FENCE;
	init_async_submit(&submit, flags, tx, NULL, NULL, NULL);

	start = origin_sector_to_sbit(cache, 0, mg->iter.bi_sector, NULL);
	bitmap_for_each_set_region (map, rs, re, start,
				    cache->sub_blocks_per_cblock) {
		struct bio_vec bvec;
		struct bvec_iter iter = mg->iter;

		int off;
		unsigned int cur = rs;
		unsigned long len;
		struct page *page;

		bvec_iter_advance(bio->bi_io_vec, &iter,
				  to_bytes((rs - start) * SUB_BLOCK_SIZE));

		do {
			bvec = bvec_iter_bvec(bio->bi_io_vec, iter);
			len = bvec.bv_len;
			off = bvec.bv_offset;
			page = bvec.bv_page;
			if (off || len != PAGE_SIZE) {
				bitmap_clear(map, rs,
					     cache->sub_blocks_per_cblock - rs);
				goto out;
			}

			BUG_ON(!pl->page);
			BUG_ON(!page);

			tx = async_memcpy(pl->page, page, 0, 0, PAGE_SIZE, &submit);

			/* chain the operations */
			submit.depend_tx = tx;

			bvec_iter_advance(bio->bi_io_vec, &iter, PAGE_SIZE);
			BUG_ON(!pl);
			pl = pl->next;
			cur++;
		} while (cur < re);

	}

out:
	init_async_submit(&submit, flags | ASYNC_TX_ACK, tx, page_copy_complete, mg, NULL);
	async_trigger_callback(&submit);
}

static void handle_mg_read(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);

	migration_ref_init(mg);
	/*
	 * This reference is holded for mg_redirect_write
	 */
	bio_work_ref_init(mg->optimisable_bio);
	init_continuation(&mg->k, mg_redirect_write);
	handle_bio(mg->client, mg->optimisable_bio, mg->cblock);
	migration_put(0, mg);
}

static void mg_write_endio(struct bio *bio)
{
	struct dm_cache_migration *mg = bio->bi_private;
	struct per_bio_data *pb = get_per_bio_data(bio);

	dm_unhook_bio(&pb->hook_info, bio);

	if (bio->bi_status)
		mg->k.input = bio->bi_status;

	mg_queue_metadata_update(mg_to_work(mg));
}


static void handle_mg_write(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache *cache = mg_to_cache(mg);
	struct bio *bio = mg->optimisable_bio;
	struct per_bio_data *pb = get_per_bio_data(bio);

	dm_hook_bio(&pb->hook_info, bio, mg_write_endio, mg);

	BUG_ON(bio->bi_iter.bi_sector % SUB_BLOCK_SIZE);
	bitmap_set(mg->migrated_sblock,
		   from_sblock(sector_to_sblock(cache, bio->bi_iter.bi_sector)),
		   dm_sector_div_up(bio_sectors(bio), SUB_BLOCK_SIZE));
	/*
	 * The overwrite bio is part of the copy operation, as such it does
	 * not set/clear discard or dirty flags.
	 */

	if (mg_operation(mg) == MG_PROMOTE) {
		if (writeback_mode(mg->client))
			remap_to_cache(mg->client, bio, mg->cblock);
		else
			remap_to_origin_and_cache(mg->client, bio, mg->oblock, mg->cblock);
	} else if (mg_operation(mg) == MG_DEMOTE) {
		remap_to_origin(mg->client, bio);
	} else {
		BUG_ON(1);
	}

	accounted_and_submit(cache, bio);
}

enum bio_valid { BIO_VALID, BIO_INVALID, BIO_PARTIAL };
enum handle_method { NONE, MIGRATION, BACKGROUND };

static inline unsigned find_next_bit_subrange(unsigned long *bitset, unsigned long sub_size,
					       unsigned long bitset_size, unsigned int head)
{
	return find_next_bit(bitset, min((head | (sub_size - 1)) + 1, bitset_size), head);
}

static inline unsigned find_next_zero_bit_subrange(unsigned long *bitset, unsigned long sub_size,
						    unsigned long bitset_size, unsigned int head)
{
	return find_next_zero_bit(bitset, min((head | (sub_size - 1)) + 1, bitset_size), head);
}

static enum bio_valid bio_range_valid(struct cache_client *client,
				      dm_cblock_t b, struct bio *bio)
{
	bool start_valid;
	struct cache *cache = client->cache;
	sector_t head_remain, tail_remain;
	sector_t bi_sector = bio->bi_iter.bi_sector;
	unsigned int bit_head, bit_tail, res_tail;
	unsigned long flags;

	bit_head = origin_sector_to_sbit(cache, b, bi_sector, &head_remain);
	bit_tail = origin_sector_to_sbit(cache, b, bio_end_sector(bio) - 1,
					 &tail_remain);

	spin_lock_irqsave(cblock_lock(cache, b), flags);
	start_valid = test_bit(bit_head, cache->sub_valid_bitset);
	if (start_valid)
		/* first bit set, we only need to find next unset bit */
		res_tail = find_next_zero_bit_subrange(cache->sub_valid_bitset,
						       cache->sub_blocks_per_cblock,
							   cache->sub_cache_size, bit_head) - 1;
	else if (!head_remain && bit_tail > bit_head)
		/* first bit not set, aligned to sub_block_size, larger than one sub_block */
		res_tail = find_next_bit_subrange(cache->sub_valid_bitset,
						  cache->sub_blocks_per_cblock,
						  cache->sub_cache_size, bit_head) - 1;
	else
		/* bio only covers one sub_block */
		res_tail = bit_head;
	spin_unlock_irqrestore(cblock_lock(cache, b), flags);

	/* bio mapped range are not all valid */
	if (!start_valid || res_tail < bit_tail)
		return res_tail < bit_tail ? BIO_PARTIAL : BIO_INVALID;

	return BIO_VALID;
}

static enum mg_operation required_mg(struct cache_client *client, dm_cblock_t b,
				     struct bio *bio, enum bio_valid *status, enum handle_method method)
{
	*status = bio_range_valid(client, b, bio);

	if (*status != BIO_INVALID && bio_data_dir(bio) == WRITE && method == NONE)
		return MG_DEMOTE;

	if (*status != BIO_VALID && method != NONE)
		return MG_PROMOTE;

	return MG_NONE;
}

static int mg_lock_with_holder(struct dm_cache_migration *mg)
{
	int r;
	struct bio *bio = mg->optimisable_bio;
	struct cache *cache = mg_to_cache(mg);
	dm_oblock_t end = to_oblock(from_oblock(mg->oblock) + 1ULL);
	unsigned nr_pages = 0;

	build_key_from_bio(mg->oblock, end, bio, cache, mg->client->dev_id, &mg->key);
	if (bio_data_dir(bio) == WRITE)
		r = dm_cell_lock_v2(cache->prison, &mg->key,
				    READ_WRITE_LOCK_LEVEL, NULL, &mg->cell);
	else
		r = dm_cell_lock_v2(cache->prison, &mg->key, WRITE_LOCK_LEVEL,
				    NULL, &mg->cell);

	BUG_ON(!r);

	if (r > 0) {
		atomic_inc(&lock_cell);
		atomic_inc(&lock2_cell);
		nr_pages = mg_need_pages(mg);
		mg->pl = get_pages(cache, nr_pages);
		if (nr_pages && !mg->pl) {
			r = -ENOMEM;
			atomic_inc(&nounlock);
			mg_drop_lock(mg);
		}
	}

	return r > 0 ? 0 : r;
}

static void handle_io(struct cache_client *client, struct bio *bio,
                      dm_cblock_t cblock, enum handle_method method)
{
	struct dm_cache_migration *mg;
	struct cache *cache = client->cache;
	bool is_write = bio_data_dir(bio) == WRITE;
	struct per_bio_data *pb = get_per_bio_data(bio);
	enum bio_valid status;
	enum mg_operation op = required_mg(client, cblock, bio, &status, method);

	if (client->requeue_mode) {
		bio->bi_status = BLK_STS_DM_REQUEUE;
		atomic_inc(&rq_io);
		atomic_dec(&in_io);
		if (pb->rw) {
			pb->rw = 0;
			atomic_dec(&rw_io);
		}
		atomic_dec(&h_io);
		bio_endio(bio);
		return;
	}

	if (op == MG_NONE)
		goto out;

	mg = alloc_migration(client, bio, NULL);
	if (!mg)
		goto out;

	set_mg_operation(mg, op);
	mg->cblock = cblock;
	mg->oblock = get_bio_block(cache, bio);

	if (mg_lock_with_holder(mg))
		goto out_free_mg;

	BUG_ON(pb->cell != mg->cell);

	pb->mg = mg;

	if (!is_write || op == MG_DEMOTE)
		inc_miss_counter(client, bio);
	else
		inc_hit_counter(client, bio, cblock);

	quiesce(mg, is_write ? handle_mg_write : handle_mg_read);

	BUG_ON(!pb->cell);
	bio_drop_shared_lock(client, bio);
	return;

out_free_mg:
	free_migration(mg);
out:
	if (op == MG_DEMOTE) {
		bio_drop_shared_lock(client, bio);
		defer_bio(client, bio);
	} else {
		if (status != BIO_VALID)
			inc_miss_counter(client, bio);
		else
			inc_hit_counter(client, bio, cblock);
		handle_bio(client, bio, cblock);
	}
}

static void mg_success(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	mg_complete(mg, mg->k.input == 0);
}

static void mg_update_metadata(struct work_struct *ws)
{
	int r = 0;
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	struct policy_work *op = mg->op;

	if (mg->k.input) {
		mg_complete(mg, false);
		return;
	}

	switch (op->op) {
	case POLICY_PROMOTE:
		// Nothing happens if mapping doesn't change at all
		r = dm_cache_client_insert_mapping(client->ccmd, op->cblock, op->oblock);
		if (r) {
			metadata_operation_failed(cache, "dm_cache_client_insert_mapping", r);
			break;
		}

		if (mg->optimisable_bio)
			r = clear_valid_bitset_full(cache, op->cblock);
		else
			r = set_valid_bitset_full(cache, op->cblock);

		break;

	case POLICY_ABANDON:
	case POLICY_DEMOTE:
		r = clear_valid_bitset_full(cache, op->cblock);
		if (r)
			break;

		r = dm_cache_client_remove_mapping(client->ccmd, op->cblock);
		if (r) {
			metadata_operation_failed(cache, "dm_cache_client_remove_mapping", r);
			break;
		}
		/*
		 * It would be nice if we only had to commit when a REQ_FLUSH
		 * comes through.  But there's one scenario that we have to
		 * look out for:
		 *
		 * - vblock x in a cache block
		 * - domotion occurs
		 * - cache block gets reallocated and over written
		 * - crash
		 *
		 * When we recover, because there was no commit the cache will
		 * rollback to having the data for vblock x in the cache block.
		 * But the cache block has since been overwritten, so it'll end
		 * up pointing to data that was never in 'x' during the history
		 * of the device.
		 *
		 * To avoid this issue we require a commit as part of the
		 * demotion operation.
		 */
		init_continuation(&mg->k, mg_success);
		continue_after_commit(&cache->committer, &mg->k);
		schedule_commit(&cache->committer);
		return;

	case POLICY_WRITEBACK_F:
	case POLICY_WRITEBACK:
		mg_complete(mg, true);
		return;
	}

	if (r)
		DMERR_LIMIT("%s: migration failed; couldn't update on disk metadata",
			    cache_device_name(cache));
	mg_complete(mg, !r);
}

static void copy_complete(int read_err, unsigned long write_err, void *context)
{
	struct dm_cache_migration *mg =
		container_of(context, struct dm_cache_migration, k);

	if (read_err || write_err)
		mg->k.input = BLK_STS_IOERR;

	queue_continuation(mg_to_cache(mg)->wq, &mg->k);
}

static void promote(struct dm_cache_migration *mg)
{
	struct dm_io_region o_region, c_region;
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;

	init_continuation(&mg->k, mg_upgrade_lock);

	o_region.bdev = client->origin_dev->bdev;
	o_region.sector =
		from_oblock(mg->op->oblock) * cache->sectors_per_block;
	o_region.count = cache->sectors_per_block;

	c_region.bdev = cache->cache_dev;
	c_region.sector =
		from_cblock(mg->op->cblock) * cache->sectors_per_block;
	c_region.count = cache->sectors_per_block;

	dm_kcopyd_copy(cache->copier, &o_region, 1, &c_region, 0,
	               copy_complete, &mg->k);
}

static inline bool op_is_writeback(struct policy_work *op)
{
	return op->op == POLICY_WRITEBACK || op->op == POLICY_WRITEBACK_F;
}

static bool skip_migration(struct cache_client *client, struct policy_work *op)
{
	if (op->op == POLICY_ABANDON || is_discarded_oblock(client, op->oblock))
		return true;

	if ((op_is_writeback(op) || op->op == POLICY_DEMOTE) &&
	    !is_dirty(client->cache, op->cblock))
		return true;

	return false;
}

static void mg_full_copy(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache_client *client = mg->client;
	struct policy_work *op = mg->op;
	bool is_policy_promote = (op->op == POLICY_PROMOTE);

	if (skip_migration(client, op)) {
		mg_upgrade_lock(ws);
		return;
	}

	if (is_policy_promote)
		promote(mg);
	else
		writeback(mg);
}

static void mg_copy(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);

	if (mg->optimisable_bio)
		/*
		 * It's safe to do this here, even though it's new data
		 * because all IO has been locked out of the block.
		 *
		 * mg_lock_writes() already took READ_WRITE_LOCK_LEVEL
		 * so _not_ using mg_upgrade_lock() as continutation.
		 */
		mg_queue_metadata_update(ws);
	else
		mg_full_copy(ws);
}

static int mg_lock_writes(struct dm_cache_migration *mg)
{
	int r;
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	struct dm_bio_prison_cell_v2 *prealloc;

	prealloc = alloc_prison_cell(cache);

	/*
	 * Prevent writes to the block, but allow reads to continue.
	 * Unless we're using an overwrite bio, in which case we lock
	 * everything.
	 */
	build_key(mg->op->oblock, oblock_succ(mg->op->oblock), client->dev_id,
		  &mg->key);
	r = dm_cell_lock_v2(cache->prison, &mg->key,
	                    mg->optimisable_bio ? READ_WRITE_LOCK_LEVEL : WRITE_LOCK_LEVEL,
	                    prealloc, &mg->cell);
	if (r < 0)
		goto bad_cell;


	atomic_inc(&lock_cell);
	atomic_inc(&lock2_cell);
	if (mg->cell != prealloc)
		free_prison_cell(cache, prealloc);

	if (r == 0) {
		init_continuation(&mg->k, mg_copy);
		queue_continuation(cache->wq, &mg->k);
	} else {
		quiesce(mg, mg_copy);
	}

	return 0;

bad_cell:
	if (mg->optimisable_bio) {
		defer_bio(client, mg->optimisable_bio);
		mg->optimisable_bio = NULL;
	}

	free_prison_cell(cache, prealloc);
	mg_complete(mg, false);

	return r;
}

static void queue_wb(struct cache_client *client, struct policy_work *op);
static int get_background_work(struct cache_client *client, bool idle, struct policy_work **op);
static int mg_start(struct cache_client *client, struct policy_work *op, struct bio *bio, bool idle)
{
	struct dm_cache_migration *mg;
	struct cache *cache = client->cache;

	if (!background_work_begin(client)) {
		if (op)
			policy_complete_background_work(client->policy, op, false);
		if (bio)
			defer_bio(client, bio);
		return -EPERM;
	}

	if (client->requeue_mode) {
		if (op)
			policy_complete_background_work(client->policy, op, false);
		background_work_end(client);
		if (bio) {
			struct per_bio_data *pb = get_per_bio_data(bio);

			bio->bi_status = BLK_STS_DM_REQUEUE;
			atomic_dec(&in_io);
			atomic_inc(&rq_io);
			if (pb->rw) {
				pb->rw = 0;
				atomic_dec(&rw_io);
			}
			bio_endio(bio);
		}
		return -EPERM;
	}

	if (!op) {
		int r;
		/*
		 * get background work here make sure we are under the
		 * protection by background work lock
		 */
		r = get_background_work(client, idle, &op);
		if (r == -ENODATA) {
			background_work_end(client);
			return r;
		}

		if (r && r != -ENODATA) {
			DMERR_LIMIT("%s: policy_background_work failed",
				    cache_device_name(cache));
			background_work_end(client);
			return r;
		}

		if (op->op == POLICY_WRITEBACK_F) {
			queue_wb(client, op);
			background_work_end(client);
			return -EAGAIN;
		}
	}

	mg = alloc_migration(client, bio, op);
	if (!bio)
		inc_io_migrations(cache);

	if (bio) {
		atomic_inc(&mg_io);
		atomic_inc(&mg3_io);
	}
	return mg_lock_writes(mg);
}

/*----------------------------------------------------------------
 * invalidation processing
 *--------------------------------------------------------------*/

static void invalidate_complete(struct dm_cache_migration *mg, bool success)
{
	struct cache_client *client = mg->client;

	mg_drop_lock(mg);

	if (!success && mg->optimisable_bio)
		bio_io_error(mg->optimisable_bio);

	free_migration(mg);

	background_work_end(client);
}

static void invalidate_completed(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	invalidate_complete(mg, !mg->k.input);
}

static int invalidate_cblock(struct cache_client *client, dm_cblock_t cblock)
{
	int r = -ENODATA;

	r = policy_invalidate_mapping(client->policy, cblock);
	if (!r) {
		r = dm_cache_client_remove_mapping(client->ccmd, cblock);
		if (r) {
			DMERR_LIMIT("%s: invalidation failed; couldn't update on disk metadata",
				    cache_device_name(client->cache));
			metadata_operation_failed(client->cache, "dm_cache_client_remove_mapping", r);
		}

	} else if (r == -ENODATA) {
		/*
		 * Harmless, already unmapped.
		 */
		r = 0;

	} else
		DMERR("%s: policy_invalidate_mapping failed", cache_device_name(client->cache));

	return r;
}

static void invalidate_remove(struct work_struct *ws)
{
	int r;
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;

	r = invalidate_cblock(client, mg->cblock);
	if (r) {
		invalidate_complete(mg, false);
		return;
	}

	init_continuation(&mg->k, invalidate_completed);
	continue_after_commit(&cache->committer, &mg->k);
	remap_to_origin_clear_discard(client, mg->optimisable_bio, mg->oblock);
	mg->optimisable_bio = NULL;
	schedule_commit(&cache->committer);
}

static int invalidate_lock(struct dm_cache_migration *mg)
{
	int r;
	struct cache_client *client = mg->client; // FIXME could this be NULL?
	struct cache *cache = client->cache;
	struct dm_bio_prison_cell_v2 *prealloc;

	prealloc = alloc_prison_cell(cache);

	build_key(mg->oblock, oblock_succ(mg->oblock),
		  client->dev_id, &mg->key);
	r = dm_cell_lock_v2(cache->prison, &mg->key,
			    READ_WRITE_LOCK_LEVEL, prealloc, &mg->cell);
	if (r < 0) {
		free_prison_cell(cache, prealloc);
		invalidate_complete(mg, false);
		return r;
	}

	atomic_inc(&lock_cell);
	atomic_inc(&lock2_cell);
	if (mg->cell != prealloc)
		free_prison_cell(cache, prealloc);

	if (r)
		quiesce(mg, invalidate_remove);

	else {
		/*
		 * We can't call invalidate_remove() directly here because we
		 * might still be in request context.
		 */
		init_continuation(&mg->k, invalidate_remove);
		queue_work(cache->wq, &mg->k.ws);
	}

	return 0;
}

static int invalidate_start(struct cache_client *client, dm_cblock_t cblock,
			    dm_oblock_t oblock, struct bio *bio)
{
	struct dm_cache_migration *mg;

	if (!background_work_begin(client))
		return -EPERM;

	mg = alloc_migration(client, bio, NULL);

	mg->optimisable_bio = bio;
	set_mg_operation(mg, MG_INVALIDATE);
	mg->cblock = cblock;
	mg->oblock = oblock;

	return invalidate_lock(mg);
}

/*----------------------------------------------------------------
 * bio processing
 *--------------------------------------------------------------*/

enum busy {
	IDLE,
	NOT_TOO_BUSY,
	BUSY
};

static enum busy spare_migration_bandwidth(struct cache *cache)
{
	bool idle = iot_idle_for(&cache->tracker, cache->opt.idle_period);
	sector_t current_volume = (atomic_read(&cache->nr_io_migrations) + 1) *
		cache->sectors_per_block;

	if (idle && current_volume <= cache->migration_threshold)
		return IDLE;
	else if (!idle && current_volume <= cache->migration_threshold << 10)
		return NOT_TOO_BUSY;
	else
		return BUSY;
}

#define BGM_THROTTLE_LOW	128
#define BGM_THROTTLE_MID	256
#define BGM_THROTTLE_HIGH	360

#define STATS_SHIFT		1
#define STATS_SHIFT_TIME	60 * HZ
#define CLEAR_STATS_PERIOD	180 * HZ

static void clear_rw_stats(struct cache *cache)
{
	cache->cur_stats.reads = 0;
	cache->cur_stats.writes = 0;
	cache->cur_stats.read_hits = 0;
	cache->cur_stats.write_hits = 0;
	cache->cur_stats.dirty_write_hits = 0;

	cache->cur_stats.jif = jiffies;
}

static void shift_stats(struct cache *cache)
{
	if (!time_after(jiffies, cache->cur_stats.jif + STATS_SHIFT_TIME))
		return;

	if (iot_idle_for(&cache->tracker, CLEAR_STATS_PERIOD)) {
		clear_rw_stats(cache);
		return;
	}

	cache->cur_stats.reads >>= STATS_SHIFT;
	cache->cur_stats.writes >>= STATS_SHIFT;
	cache->cur_stats.read_hits >>= STATS_SHIFT;
	cache->cur_stats.write_hits >>= STATS_SHIFT;
	cache->cur_stats.dirty_write_hits >>= STATS_SHIFT;

	cache->cur_stats.jif = jiffies;
}

static void clear_client_stats(struct cache_client *client)
{
	atomic_set(&client->stats.reads, 0);
	atomic_set(&client->stats.writes, 0);
	atomic_set(&client->stats.read_hit, 0);
	atomic_set(&client->stats.write_hit, 0);
	atomic_set(&client->stats.dirty_write_hit, 0);
	atomic_set(&client->stats.direct_count, 0);
	atomic_set(&client->stats.discard_count, 0);
	atomic_set(&client->stats.defer_bio, 0);
	atomic_set(&client->stats.disk_reads, 0);
	atomic_set(&client->stats.disk_writes, 0);
	atomic_set(&client->stats.ssd_reads, 0);
	atomic_set(&client->stats.ssd_writes, 0);
	atomic_set(&client->stats.uncached_reads, 0);
	atomic_set(&client->stats.uncached_writes, 0);
	atomic_set(&client->stats.uncached_seq_reads, 0);
	atomic_set(&client->stats.uncached_seq_writes, 0);
}

static void clear_all_stats(struct cache *cache)
{
	struct cache_client *client;

	atomic_set(&cache->stats.reads, 0);
	atomic_set(&cache->stats.writes, 0);
	atomic_set(&cache->stats.read_hit, 0);
	atomic_set(&cache->stats.write_hit, 0);
	atomic_set(&cache->stats.dirty_write_hit, 0);
	atomic_set(&cache->stats.direct_count, 0);
	atomic_set(&cache->stats.discard_count, 0);
	atomic_set(&cache->stats.defer_bio, 0);
	atomic_set(&cache->stats.disk_reads, 0);
	atomic_set(&cache->stats.disk_writes, 0);
	atomic_set(&cache->stats.ssd_reads, 0);
	atomic_set(&cache->stats.ssd_writes, 0);
	atomic_set(&cache->stats.uncached_reads, 0);
	atomic_set(&cache->stats.uncached_writes, 0);
	atomic_set(&cache->stats.uncached_seq_reads, 0);
	atomic_set(&cache->stats.uncached_seq_writes, 0);

	client = get_first_client(cache);
	while (client) {
		clear_client_stats(client);
		client = get_next_client(cache, client);
	}
}

/*----------------------------------------------------------------*/

#define ewma_add(ewma, val, weight, factor)		\
({							\
	(ewma) *= (weight) -  1;			\
	(ewma) += (val) << factor;			\
	(ewma) /= (weight);				\
	(ewma) >> factor;				\
})

static void add_sequential(struct task_struct *t)
{
	ewma_add(t->sequential_io_avg,
		 t->sequential_io, 8, 0);

	t->sequential_io = 0;
}

static struct hlist_head *iohash(struct cache_client *client, uint64_t k)
{
	return &client->io_hash[hash_64(k, RECENT_IO_BITS)];
}

static void seq_io_detect_init(struct cache_client *client)
{
	struct io *io;

	INIT_LIST_HEAD(&client->io_lru);
	spin_lock_init(&client->io_lock);

	for (io = client->io; io < client->io + RECENT_IO; io++) {
		list_add(&io->lru, &client->io_lru);
		hlist_add_head(&io->hash, client->io_hash + RECENT_IO);
	}
}

static int skip_sequential_io(struct cache_client *client, struct bio *bio)
{
	int skip = 0;
	struct io *i;
	struct task_struct *task = current;
	unsigned sectors;

	spin_lock_irq(&client->io_lock);

	hlist_for_each_entry(i, iohash(client, bio->bi_iter.bi_sector), hash)
		if (i->last == bio->bi_iter.bi_sector &&
		    time_before(jiffies, i->jiffies))
			goto found;

	i = list_first_entry(&client->io_lru, struct io, lru);

	add_sequential(task);
	i->sequential = 0;
found:
	if (i->sequential + bio->bi_iter.bi_size > i->sequential)
		i->sequential += bio->bi_iter.bi_size;

	i->last = bio_end_sector(bio);
	i->jiffies = jiffies + msecs_to_jiffies(5000);
	task->sequential_io = i->sequential;

	hlist_del(&i->hash);
	hlist_add_head(&i->hash, iohash(client, i->last));
	list_move_tail(&i->lru, &client->io_lru);

	spin_unlock_irq(&client->io_lock);

	sectors = max(task->sequential_io,
		      task->sequential_io_avg) >> 9;

	if (client->opt.skip_seq_thresh &&
	    sectors >= client->opt.skip_seq_thresh)
		skip = 1;

	return skip;
}

static inline bool bio_filtered(struct cache_client *client, struct bio *bio)
{
	return test_bit(bio_data_dir(bio), &(client->features.filter));
}

static enum handle_method required_method(struct cache_client *client, struct bio *bio, dm_oblock_t b, int *skip)
{
	*skip = 0;

	if ((client->features.optional_cache && !(bio->bi_opf & REQ_NEED_CACHE)) ||
	     bio_filtered(client, bio))
		return NONE;

	*skip = skip_sequential_io(client, bio);
	if (*skip)
		return NONE;

	if (bio_data_dir(bio) == READ &&
	    bio_sectors(bio) <= client->opt.size_thresh)
		return BACKGROUND;

	if (!optimisable_bio(client, bio, b))
		return BACKGROUND;

	return MIGRATION;
}

static int map_bio(struct cache_client *client, struct bio *bio, dm_oblock_t block,
		   bool *commit_needed)
{
	int r, data_dir;
	dm_cblock_t cblock;
	struct cache *cache = client->cache;
	int skip = 0;
	enum handle_method method;
	struct policy_work *op = NULL;
	bool background_queued;


	*commit_needed = false;

	if (!bio_detain_shared(client, block, bio)) {
		/*
		 * An exclusive lock is held for this block, so we have to
		 * wait.  We set the commit_needed flag so the current
		 * transaction will be committed asap, allowing this lock
		 * to be dropped.
		 */
		*commit_needed = true;
		return DM_MAPIO_SUBMITTED;
	}

	data_dir = bio_data_dir(bio);

	switch ((method = required_method(client, bio, block, &skip))) {
	case MIGRATION:

		r = policy_lookup_with_work(client->policy, block, &cblock, data_dir, true, &op);
		switch (r) {
		case -EBUSY:
			/*
			 * Someone is promoting this block, let's wait a while for promotion to 
 			 * complete.
			 */
			bio_drop_shared_lock(client, bio);
			defer_bio(client, bio);
			return DM_MAPIO_SUBMITTED;
		case -ENOENT:
			if (op) {
				bio_drop_shared_lock(client, bio);
				BUG_ON(op->op != POLICY_PROMOTE);
				mg_start(client, op, bio, false);
				return DM_MAPIO_SUBMITTED;
			}
		case 0:
			break;
		default:
			DMERR_LIMIT("%s: policy_lookup_with_work() failed with r = %d",
					client_device_name(client), r);
			bio_io_error(bio);
			return DM_MAPIO_SUBMITTED;
		}
		break;

	case BACKGROUND:
		r = policy_lookup(client->policy, block, &cblock, data_dir, false, &background_queued);
		if (unlikely(r && r != -ENOENT)) {
			DMERR_LIMIT("%s: policy_lookup() failed with r = %d",
				    client_device_name(client), r);
			bio_io_error(bio);
			return DM_MAPIO_SUBMITTED;
		}

		if (background_queued)
			wake_migration_worker(cache);
		break;

	case NONE:
		r = policy_lookup_simple(client->policy, block, &cblock);
		break;
	}

	if (r != -ENOENT && bio_sectors(bio) < SUB_BLOCK_SIZE &&
	    bio_range_valid(client, cblock, bio) == BIO_INVALID)
		r = -ENOENT;

	if (r == -ENOENT) {
		struct per_bio_data *pb = get_per_bio_data(bio);

		/*
		 * Miss.
		 */
		inc_miss_counter(client, bio);

		if (pb->req_nr == 0) {
			accounted_begin(cache, bio);
			atomic_inc(&remap_io);
			pb->remap = 1;
			remap_to_origin_clear_discard(client, bio, block);
			inc_uncached_counter(client, bio, skip);
		} else {
			/*
			 * This is a duplicate writethrough io that is no
			 * longer needed because the block has been demoted.
			 */
			atomic_dec(&in_io);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
	} else {
		struct per_bio_data *pb = get_per_bio_data(bio);
		/*
		 * Hit.
		 */
		/*
		 * Passthrough always maps to the origin, invalidating any
		 * cache blocks that are written to.
		 */
		if (passthrough_mode(client)) {
			inc_hit_counter(client, bio, cblock);

			if (bio_data_dir(bio) == WRITE) {
				bio_drop_shared_lock(client, bio);
				atomic_inc(&client->stats.demotion);
				invalidate_start(client, cblock, block, bio);
			} else {
				remap_to_origin_clear_discard(client, bio, block);
			}
		} else if (bio_sectors(bio) < SUB_BLOCK_SIZE) {
			inc_hit_counter(client, bio, cblock);
			accounted_begin(cache, bio);
			atomic_inc(&remap_io);
			pb->remap = 1;
			if (bio_data_dir(bio) == READ || writeback_mode(client))
				remap_to_cache_dirty(client, bio, block, cblock);
			else
				remap_to_origin_and_cache(client, bio, block, cblock);
		} else {
			if (pb->mg_phase2) {
				pb->mg_phase2 = 0;
				atomic_dec(&mg2_io);
				atomic_dec(&mg3_io);
			}
			atomic_inc(&h_io);
			handle_io(client, bio, cblock, method);
			return DM_MAPIO_SUBMITTED;
		}
	}

	/*
	 * dm core turns FUA requests into a separate payload and FLUSH req.
	 */
	if (bio->bi_opf & REQ_FUA) {
		/*
		 * issue_after_commit will call accounted_begin a second time.  So
		 * we call accounted_complete() to avoid double accounting.
		 */
		accounted_complete(cache, bio);
		issue_after_commit(&cache->committer, bio);
		*commit_needed = true;
		return DM_MAPIO_SUBMITTED;
	}

	return DM_MAPIO_REMAPPED;
}

static bool process_bio(struct cache_client *client, struct bio *bio)
{
	bool commit_needed;

	if (map_bio(client, bio, get_bio_block(client->cache, bio), &commit_needed) == DM_MAPIO_REMAPPED)
		submit_bio_noacct(bio);

	return commit_needed;
}

/*
 * A non-zero return indicates read_only or fail_io mode.
 */
static int commit(struct cache *cache, bool clean_shutdown)
{
	int r;

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return -EINVAL;

	spin_lock_irq(&cache->lock);
	if (!time_after(jiffies, cache->last_commit_jif + HZ))
		atomic_inc(&cache->nr_commits_1000);
	else if (!time_after(jiffies, cache->last_commit_jif + msecs_to_jiffies(500))) {
		DMWARN("update metadata in less than 500 msecs");
		atomic_inc(&cache->nr_commits_500);
	} else if (!time_after(jiffies, cache->last_commit_jif + msecs_to_jiffies(500))) {
		atomic_inc(&cache->nr_commits_100);
	}
	cache->last_commit_jif = jiffies;
	spin_unlock_irq(&cache->lock);

	atomic_inc(&cache->nr_commits);
	r = dm_cache_commit(cache->cmd, clean_shutdown);
	if (r)
		metadata_operation_failed(cache, "dm_cache_commit", r);

	return r;
}

/*
 * Used by the batcher.
 */
static blk_status_t commit_op(void *context)
{
	struct cache *cache = context;

	if (dm_cache_changed_this_transaction(cache->cmd))
		return errno_to_blk_status(commit(cache, false));

	return 0;
}

/*----------------------------------------------------------------*/

/*----------------------------------------------------------------
 * reclaim processing
 *--------------------------------------------------------------*/

static void handle_discard_bio(struct cache_client *client, struct bio *bio)
{
	dm_dblock_t b, e;

	// FIXME: do we need to lock the region?  Or can we just assume the
	// user wont be so foolish as to issue discard concurrently with
	// other IO?
	atomic_inc(&client->stats.discard_count);
	calc_discard_block_range(client, bio, &b, &e);
	while (b != e) {
		set_discard(client, b);
		b = to_dblock(from_dblock(b) + 1);
	}

	if (client->features.discard_passdown) {
		remap_to_origin(client, bio);
		submit_bio_noacct(bio);
	} else
		bio_endio(bio);
	atomic_dec(&in_io);
}

static void reclaim_complete(struct dm_cache_migration *mg, bool success)
{
	struct cache_client *client = mg->client;
	struct bio *bio = mg->optimisable_bio;

	if (success) {
		mg_drop_lock(mg);
		handle_discard_bio(client, bio);
	}

	free_migration(mg);
	background_work_end(client);
}

static void reclaim_clear_bitset(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct bio *bio = mg->optimisable_bio;
	struct cache *cache = mg_to_cache(mg);

	bitmap_set(mg->migrated_sblock,
		   from_sblock(sector_to_sblock(cache, bio->bi_iter.bi_sector)),
		   dm_sector_div_up(bio_sectors(bio), SUB_BLOCK_SIZE));

	clear_valid_bitset(cache, mg->cblock, mg->migrated_sblock);
	reclaim_complete(mg, true);
}

static void reclaim_remove(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;

	(void) invalidate_cblock(client, mg->cblock);

	init_continuation(&mg->k, reclaim_clear_bitset);
	continue_after_commit(&cache->committer, &mg->k);
	schedule_commit(&cache->committer);
}

static int reclaim_lock(struct dm_cache_migration *mg)
{
	int r;
	struct cache_client *client = mg->client;
	struct cache *cache = client->cache;
	struct dm_bio_prison_cell_v2 *prealloc;
	continuation_fn reclaim_fn;
	struct bio *bio = mg->optimisable_bio;

	prealloc = alloc_prison_cell(cache);

	build_key(mg->oblock, oblock_succ(mg->oblock),
		  client->dev_id, &mg->key);
	r = dm_cell_lock_v2(cache->prison, &mg->key,
			    READ_WRITE_LOCK_LEVEL, prealloc, &mg->cell);
	if (r < 0) {
		free_prison_cell(cache, prealloc);
		defer_bio(client, bio);
		reclaim_complete(mg, false);
		return r;
	}

	atomic_inc(&lock_cell);
	atomic_inc(&lock2_cell);
	if (dm_sector_div_up(bio_sectors(bio), SUB_BLOCK_SIZE) == cache->sub_blocks_per_cblock &&
	    client->features.discard_return_blocks)
		reclaim_fn = reclaim_remove;
	else
		reclaim_fn = reclaim_clear_bitset;

	if (mg->cell != prealloc)
		free_prison_cell(cache, prealloc);

	if (r)
		quiesce(mg, reclaim_fn);

	else {
		init_continuation(&mg->k, reclaim_fn);
		queue_work(cache->wq, &mg->k.ws);
	}

	return 0;
}

static bool handle_discard_reclaim(struct cache_client *client, struct bio *bio,
				   dm_oblock_t oblock, dm_cblock_t cblock)
{
	struct dm_cache_migration *mg;

	if (!background_work_begin(client)) {
		defer_bio(client, bio);
		return true;
	}

	mg = alloc_migration(client, bio, NULL);

	mg->optimisable_bio = bio;
	mg->cblock = cblock;
	mg->oblock = oblock;

	return (bool)!!reclaim_lock(mg);
}

/*----------------------------------------------------------------*/

static bool process_flush_bio(struct cache_client *client, struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	if (!pb->req_nr)
		remap_to_origin(client, bio);
	else
		remap_to_cache(client, bio, 0);

	issue_after_commit(&client->cache->committer, bio);
	return true;
}

static inline bool fua_done_commit(struct bio *bio)
{
	struct per_bio_data *pb = get_per_bio_data(bio);

	return pb->commit_done;
}

static bool process_fua_bio(struct cache_client *client, struct bio *bio)
{
	issue_after_commit(&client->cache->committer, bio);

	return true;
}

static bool process_discard_bio(struct cache_client *client, struct bio *bio)
{
	struct cache *cache = client->cache;
	dm_oblock_t oblock = get_bio_block(cache, bio);
	dm_cblock_t cblock;
	int r;

	r = bio_detain_shared(client, oblock, bio);
	if (!r)
		return true;

	r = policy_lookup_simple(client->policy, oblock, &cblock);
	bio_drop_shared_lock(client, bio);

	if (r == -ENOENT) {
		handle_discard_bio(client, bio);
		return false;
	}

	return handle_discard_reclaim(client, bio, oblock, cblock);
}

static void requeue_client_deferred_bios(struct cache_client *client);
static void process_client_deferred_bios(struct cache_client *client)
{
	bool commit_needed = false;
	struct bio_list bios;
	struct bio *bio;
	struct cache *cache = client->cache;

	if (client->requeue_mode) {
		requeue_client_deferred_bios(client);
		return;
	}

	bio_list_init(&bios);

	spin_lock_irq(&client->lock);
	bio_list_merge(&bios, &client->deferred_bios);
	bio_list_init(&client->deferred_bios);
	spin_unlock_irq(&client->lock);

	while ((bio = bio_list_pop(&bios))) {
		if (bio->bi_opf & REQ_PREFLUSH)
			commit_needed = process_flush_bio(client, bio) || commit_needed;

		else if ((bio->bi_opf & REQ_FUA) && !fua_done_commit(bio))
			commit_needed = process_fua_bio(client, bio) || commit_needed;

		else if (bio_op(bio) == REQ_OP_DISCARD)
			commit_needed = process_discard_bio(client, bio) || commit_needed;

		else
			commit_needed = process_bio(client, bio) || commit_needed;
	}

	if (commit_needed)
		schedule_commit(&cache->committer);

}

static void process_deferred_bios(struct work_struct *ws)
{
	struct cache *cache = container_of(ws, struct cache, deferred_bio_worker);
	struct cache_client *client;

	client = get_first_client(cache);
	while (client) {
		process_client_deferred_bios(client);
		client = get_next_client(cache, client);
	}
}

static int __recycle(void *context, dm_cblock_t b)
{
	struct cache *cache = context;
	struct entry *e = cmgr_to_entry(cache->manager, from_cblock(b));

	cmgr_free_entry(cache->manager, e);
	return 0;
}

static void recycle_clients(struct work_struct *ws)
{
	struct cache *cache = container_of(ws, struct cache, client_recycler);

	dm_cache_recycle_clients(cache->cmd, __recycle, cache);
	return;
}

static void mg_worker_do_work_sync(struct cache *cache, struct mg_worker *w)
{
	queue_work(cache->wq, &w->worker);
}

static void mg_worker_do_work(struct work_struct *ws)
{
	struct list_head pending;
	struct mg_worker *w = container_of(ws, struct mg_worker, worker);
	struct dm_cache_migration *mg, *tmp;

	INIT_LIST_HEAD(&pending);

	spin_lock_irq(&w->lock);
	list_splice_init(&w->works, &pending);
	spin_unlock_irq(&w->lock);

	list_for_each_entry_safe(mg, tmp, &pending, list) {
		list_del_init(&mg->list);
		w->fn(mg_to_work(mg));
	}
}

static void update_metadata(struct work_struct *ws)
{
	struct dm_cache_migration *mg = ws_to_mg(ws);

	if (policy_operation(mg))
		mg_update_metadata(mg_to_work(mg));
	else
		mg_update_valid_bitset(mg_to_work(mg));
}

/*----------------------------------------------------------------
 * Main worker loop
 *--------------------------------------------------------------*/

static void requeue_client_deferred_bios(struct cache_client *client)
{
	struct bio *bio;
	struct bio_list bios;

	spin_lock_irq(&client->lock);
	bio_list_init(&bios);
	bio_list_merge(&bios, &client->deferred_bios);
	bio_list_init(&client->deferred_bios);
	spin_unlock_irq(&client->lock);

	while ((bio = bio_list_pop(&bios))) {
		bio->bi_status = BLK_STS_DM_REQUEUE;
		atomic_inc(&rq_io);
		atomic_dec(&in_io);
		bio_endio(bio);
	}
}

static void requeue_deferred_bios(struct cache *cache)
{
	struct cache_client *client;

	client = get_first_client(cache);
	while (client) {
		requeue_client_deferred_bios(client);
		client = get_next_client(cache, client);
	}
}

/*
 * We want to commit periodically so that not too much
 * unwritten metadata builds up.
 */
static void sync_cache_metadata(struct cache *cache, bool clean_shutdown);
static void do_waker(struct work_struct *ws)
{
	struct cache *cache = container_of(to_delayed_work(ws), struct cache, waker);
	struct cache_client *client;

	shift_stats(cache);
	wake_migration_worker(cache);
	schedule_commit(&cache->committer);
	if (get_cache_mode(cache) == CM_WRITE)
		(void) commit(cache, false);
	queue_delayed_work(cache->wq, &cache->waker, COMMIT_PERIOD);
}

static void client_ticker(struct work_struct *ws)
{
	struct cache_client *client = container_of(to_delayed_work(ws), struct cache_client, ticker);

	policy_tick(client->policy, true);

	queue_delayed_work(client->wq, &client->ticker, COMMIT_PERIOD);
}

static void abort_writeback(struct cache_client *client)
{
	struct policy_work *op;
	struct list_head wb;

	INIT_LIST_HEAD(&wb);

	spin_lock_irq(&client->wb_lock);
	list_splice_init(&client->wb_queue, &wb);
	spin_unlock_irq(&client->wb_lock);

	while (!list_empty(&wb)) {
		op = list_first_entry(&wb, struct policy_work, list);
		list_del(&op->list);
		policy_complete_background_work(client->policy, op, false);
	}
}

static void issue_writeback(struct work_struct *ws)
{
	struct cache_client *client = container_of(ws, struct cache_client, wb_worker);
	int r;
	struct policy_work *op;
	struct list_head wb;

	if (dm_suspended(client->ti))
		return;

	INIT_LIST_HEAD(&wb);

	spin_lock_irq(&client->wb_lock);
	list_splice_init(&client->wb_queue, &wb);
	spin_unlock_irq(&client->wb_lock);

	while (!list_empty(&wb)) {
		op = list_first_entry(&wb, struct policy_work, list);
		list_del(&op->list);
		r = mg_start(client, op, NULL, false);
		if (r)
			break;
	}

}

static void wake_wb_worker(struct cache_client *client)
{
	queue_work(client->wq, &client->wb_worker);
}

static void queue_wb(struct cache_client *client, struct policy_work *op)
{
	spin_lock_irq(&client->wb_lock);
	list_add_tail(&op->list, &client->wb_queue);
	spin_unlock_irq(&client->wb_lock);
}

static int get_background_work(struct cache_client *client, bool idle, struct policy_work **op)
{
	/* We should not be here when target suspended */

	return policy_get_background_work(client->policy, idle, op);
}

static void check_migrations(struct work_struct *ws)
{
	int r, count;
	struct policy_work *op;
	struct cache *cache = container_of(ws, struct cache, migration_worker);
	enum busy b;
	struct cache_client *client;

	// FIXME parallel?

	client = get_first_client(cache);
	while (client) {
		count = 0;
		if (atomic_read(&cache->pool_need_grow))
			grow_page_pool(cache, PAGE_POOL_GROW);

		for (;;) {
			if (passthrough_mode(client))
				break;

			b = spare_migration_bandwidth(cache);

			r = mg_start(client, NULL, NULL, b == IDLE);
			if (r && r != -EAGAIN)
				break;
			if (r == -EAGAIN)
				continue;

			if (count++ > client->opt.bgm_throttle)
				break;
		}
		wake_wb_worker(client);
		client = get_next_client(cache, client);
	}
}

/*----------------------------------------------------------------
 * Target methods
 *--------------------------------------------------------------*/

static void __cache_table_remove(struct cache *cache);
/*
 * This function gets called on the error paths of the constructor, so we
 * have to cope with a partially initialised struct.
 */
static void destroy(struct cache *cache)
{
	unsigned i;
	struct pl_group *group;

	__cache_table_remove(cache);

	mempool_exit(&cache->migration_pool);

	if (cache->prison)
		dm_bio_prison_destroy_v2(cache->prison);

	if (cache->wq)
		destroy_workqueue(cache->wq);

	if (cache->dirty_bitset)
		free_bitset(cache->dirty_bitset);

	if (cache->sub_dirty_bitset)
		free_bitset(cache->sub_dirty_bitset);

	if (cache->sub_valid_bitset)
		free_bitset(cache->sub_valid_bitset);

	if (cache->copier)
		dm_kcopyd_client_destroy(cache->copier);

	if (cache->io_client)
		dm_io_client_destroy(cache->io_client);

	if (cache->cmd) {
		dm_cache_metadata_close(cache->cmd);
		DMWARN("%s: cache pool metadata close", __func__);
	}

	if (cache->page_pool) {
		free_page_list_pages(cache->page_pool);
		while (!list_empty(&cache->page_list_group)) {
			group = list_first_entry(&cache->page_list_group, struct pl_group, list);
			list_del(&group->list);
			free_page_list_group(group);
		}
	}

	if (cache->manager)
		cmgr_destroy(cache->manager);

	for (i = 0; i < cache->nr_ctr_args ; i++)
		kfree(cache->ctr_args[i]);
	kfree(cache->ctr_args);

	bioset_exit(&cache->bs);

	kfree(cache);
}

static void __cache_dec(struct cache *cache);
static void cache_dtr(struct dm_target *ti)
{
	struct cache_c *cache_c = ti->private;
	char buf[BDEVNAME_SIZE];

	mutex_lock(&dm_cache_table.mutex);
	__cache_dec(cache_c->cache);
	mutex_unlock(&dm_cache_table.mutex);

	if (cache_c->metadata_dev) {
		DMWARN("%s: put metadata dev (%s)", __func__, bdevname(cache_c->metadata_dev->bdev, buf));
		dm_put_device(ti, cache_c->metadata_dev);
	}

	if (cache_c->cache_dev) {
		DMWARN("%s: put cache dev (%s)", __func__, bdevname(cache_c->cache_dev->bdev, buf));
		dm_put_device(ti, cache_c->cache_dev);
	}

	DMWARN("%s: cache-pool removed", __func__);
}

static sector_t get_bdev_size(struct block_device *bdev)
{
	return i_size_read(bdev->bd_inode) >> SECTOR_SHIFT;
}

static sector_t get_dev_size(struct dm_dev *dev)
{
	return i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT;
}

static void cache_table_init(void)
{
	mutex_init(&dm_cache_table.mutex);
	INIT_LIST_HEAD(&dm_cache_table.caches);
}

static void cache_table_exit(void)
{
	mutex_destroy(&dm_cache_table.mutex);
}

static void __cache_table_insert(struct cache *cache)
{
	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	list_add(&cache->list, &dm_cache_table.caches);
}

static struct cache *__cache_table_lookup(struct mapped_device *md)
{
	struct cache *cache = NULL, *tmp;

	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	list_for_each_entry(tmp, &dm_cache_table.caches, list) {
		if (tmp->cache_md == md) {
			cache = tmp;
			break;
		}

	}

	return cache;
}

static struct cache *__cache_table_lookup_metadata_dev(struct block_device *md_dev)
{
	struct cache *cache = NULL, *tmp;

	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	list_for_each_entry(tmp, &dm_cache_table.caches, list) {
		if (tmp->metadata_dev == md_dev) { // FIXME distinct type
			cache = tmp;
			break;
		}

	}

	return cache;
}

static void __cache_table_remove(struct cache *cache)
{
	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	if (__cache_table_lookup(cache->cache_md))
		list_del(&cache->list);
}

/*----------------------------------------------------------------*/

struct kobj_holder {
	struct kobject kobj;
	struct cache_client *client;
	uint64_t id;
	struct list_head list;
	refcount_t ref;
};

static struct kobj_holder *init_kobj_holder(struct cache_client *client,
					    uint64_t id)
{
	struct kobj_holder *holder;

	holder = kzalloc(sizeof(struct kobj_holder), GFP_KERNEL);
	if (!holder)
		return NULL;

	holder->client = client;
	holder->id = id;
	refcount_set(&holder->ref, 1);

	return holder;
}

static struct kobj_holder *__kobj_find(struct cache *cache, uint64_t id)
{
	struct kobj_holder *tmp, *holder = NULL;

	list_for_each_entry(tmp, &cache->client_kobj, list) {
		if (tmp->id == id) {
			holder = tmp;
			break;
		}
	}

	return holder;
}

static struct kobj_holder *kobj_find(struct cache *cache, uint64_t id)
{
	struct kobj_holder *holder;

	spin_lock_irq(&cache->lock);
	holder = __kobj_find(cache, id);
	spin_unlock_irq(&cache->lock);

	return holder;
}

static void __kobj_store(struct cache *cache, struct kobj_holder *holder)
{
	kobject_get(&holder->kobj);
	list_add(&holder->list, &cache->client_kobj);
}

static void kobj_store(struct cache *cache, struct kobj_holder *holder)
{
	spin_lock_irq(&cache->lock);
	__kobj_store(cache, holder);
	spin_unlock_irq(&cache->lock);
}

static void __kobj_remove(struct cache *cache, uint64_t id)
{
	struct kobj_holder *holder;

	holder = __kobj_find(cache, id);
	if (!holder)
		return;

	list_del(&holder->list);
	spin_unlock_irq(&cache->lock);
	kobject_put(&holder->kobj);
	spin_lock_irq(&cache->lock);
	kfree(holder);

}

static void kobj_remove(struct cache *cache, uint64_t id)
{
	spin_lock_irq(&cache->lock);
	__kobj_remove(cache, id);
	spin_unlock_irq(&cache->lock);
}

static void kobj_get(struct kobj_holder *holder)
{
	BUG_ON(!holder);
	refcount_inc(&holder->ref);
	kobject_get(&holder->kobj);
}

static void kobj_holder_drop(struct kobj_holder *holder, struct cache *cache)
{
	BUG_ON(!holder);

	if (!refcount_dec_not_one(&holder->ref))
		/* not one */
		kobj_remove(cache, holder->client->dev_id);
}

/*----------------------------------------------------------------*/
struct dm_sysfs_attr {
	struct attribute attr;
	ssize_t (*show)(struct cache *, char *);
	ssize_t (*store)(struct cache *, const char *, size_t);
};

struct dm_client_sysfs_attr {
	struct attribute attr;
	ssize_t (*show)(struct cache_client *, char *);
	ssize_t (*store)(struct cache_client *,const char *, size_t);
};

#define DM_ATTR_RO(_name) \
	struct dm_sysfs_attr dm_attr_##_name = \
	__ATTR(_name, S_IRUGO, dm_attr_##_name##_show, NULL)

#define DM_ATTR_WO(_name) \
	struct dm_sysfs_attr dm_attr_##_name = \
	__ATTR(_name, S_IWUSR, NULL, dm_attr_##_name##_store)

#define DM_ATTR_WR(_name) \
	struct dm_sysfs_attr dm_attr_##_name = \
	__ATTR(_name, S_IRUGO|S_IWUSR, dm_attr_##_name##_show, dm_attr_##_name##_store)

#define DM_CLIENT_ATTR_WR(_name) \
	struct dm_client_sysfs_attr dm_attr_##_name = \
	__ATTR(_name, S_IRUGO|S_IWUSR, dm_attr_##_name##_show, dm_attr_##_name##_store)

#define DM_CLIENT_ATTR_RO(_name) \
	struct dm_client_sysfs_attr dm_attr_##_name = \
	__ATTR(_name, S_IRUGO, dm_attr_##_name##_show, NULL)

static ssize_t dm_attr_show(struct kobject *kobj, struct attribute *attr,
			    char *page)
{
	struct dm_sysfs_attr *dm_attr;
	struct cache *cache;
	ssize_t ret;

	dm_attr = container_of(attr, struct dm_sysfs_attr, attr);
	if (!dm_attr->show)
		return -EIO;

	cache = container_of(kobj, struct cache, kobj);
	ret = dm_attr->show(cache, page);

	return ret;
}

static ssize_t dm_attr_store(struct kobject *kobj, struct attribute *attr,
			     const char *buf, size_t count)
{
	struct dm_sysfs_attr *dm_attr;
	struct cache *cache;
	ssize_t ret;

	dm_attr = container_of(attr, struct dm_sysfs_attr, attr);
	if (!dm_attr->store)
		return -EIO;

	cache = container_of(kobj, struct cache, kobj);
	ret = dm_attr->store(cache, buf, count);

	return ret;
}

static ssize_t dm_client_attr_show(struct kobject *kobj, struct attribute *attr,
				   char *page)
{
	struct dm_client_sysfs_attr *dm_attr;
	struct cache_client *client;
	ssize_t ret;

	dm_attr = container_of(attr, struct dm_client_sysfs_attr, attr);
	if (!dm_attr->show)
		return -EIO;

	client = container_of(kobj, struct kobj_holder, kobj)->client;
	ret = dm_attr->show(client, page);

	return ret;
}

static ssize_t dm_client_attr_store(struct kobject *kobj, struct attribute *attr,
				    const char *buf, size_t count)
{
	struct dm_client_sysfs_attr *dm_attr;
	struct cache_client *client;
	ssize_t ret;

	dm_attr = container_of(attr, struct dm_client_sysfs_attr, attr);
	if (!dm_attr->store)
		return -EIO;

	client = container_of(kobj, struct kobj_holder, kobj)->client;
	ret = dm_attr->store(client, buf, count);

	return ret;
}

static ssize_t dm_attr_zero_stats_store(struct cache *cache, const char *buf,
					size_t count)
{
	int input;

	if (kstrtoint(buf, 10, &input))
		return -EIO;

	if (input)
		clear_all_stats(cache);

	return count;
}
static DM_ATTR_WO(zero_stats);

static ssize_t dm_attr_idle_period_show(struct cache *cache, char *buf)
{
	return sprintf(buf, "%lu\n", cache->opt.idle_period);
}

static ssize_t dm_attr_idle_period_store(struct cache *cache, const char *buf,
					 size_t count)
{
	int new;

	if (kstrtoint(buf, 10, &new))
		return -EIO;

	spin_lock_irq(&cache->lock);
	cache->opt.idle_period = new;
	spin_unlock_irq(&cache->lock);

	atomic_set(&cache->pool_need_grow, 1);
	return count;
}
static DM_ATTR_WR(idle_period);

static ssize_t dm_attr_page_pool_max_show(struct cache *cache, char *buf)
{
	return sprintf(buf, "%u\n", cache->opt.page_pool_max);
}

static ssize_t dm_attr_page_pool_max_store(struct cache *cache, const char *buf,
					   size_t count)
{
	int new;

	if (kstrtoint(buf, 10, &new))
		return -EIO;

	spin_lock_irq(&cache->lock);
	cache->opt.page_pool_max = new;
	spin_unlock_irq(&cache->lock);

	return count;
}
static DM_ATTR_WR(page_pool_max);

static ssize_t dm_attr_raw_stats_show(struct cache *cache, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "reads : %d\n", atomic_read(&cache->stats.reads));
	len += sprintf(buf + len, "writes : %d\n", atomic_read(&cache->stats.writes));
	len += sprintf(buf + len, "read hit : %d\n", atomic_read(&cache->stats.read_hit));
	len += sprintf(buf + len, "read miss : %d\n", atomic_read(&cache->stats.read_miss));
	len += sprintf(buf + len, "write hit : %d\n", atomic_read(&cache->stats.write_hit));
	len += sprintf(buf + len, "write miss : %d\n", atomic_read(&cache->stats.write_miss));
	len += sprintf(buf + len, "dirty write hit : %d\n", atomic_read(&cache->stats.dirty_write_hit));
	len += sprintf(buf + len, "discard : %d\n", atomic_read(&cache->stats.discard_count));
	len += sprintf(buf + len, "direct : %d\n", atomic_read(&cache->stats.direct_count));
	len += sprintf(buf + len, "defer bio : %d\n", atomic_read(&cache->stats.defer_bio));
	len += sprintf(buf + len, "ssd reads : %d\n", atomic_read(&cache->stats.ssd_reads));
	len += sprintf(buf + len, "ssd writes : %d\n", atomic_read(&cache->stats.ssd_writes));
	len += sprintf(buf + len, "uncached reads : %d\n", atomic_read(&cache->stats.uncached_reads));
	len += sprintf(buf + len, "uncached writes : %d\n", atomic_read(&cache->stats.uncached_writes));
	len += sprintf(buf + len, "uncached seq reads : %d\n", atomic_read(&cache->stats.uncached_seq_reads));
	len += sprintf(buf + len, "uncached seq writes : %d\n", atomic_read(&cache->stats.uncached_seq_writes));
	len += sprintf(buf + len, "page pool grow : %d\n", atomic_read(&cache->stats.page_pool_grow));

	return len;
}
static DM_ATTR_RO(raw_stats);

static ssize_t dm_attr_curr_stats_show(struct cache *cache, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "reads : %llu\n", cache->cur_stats.reads);
	len += sprintf(buf + len, "writes : %llu\n", cache->cur_stats.writes);
	len += sprintf(buf + len, "read hit : %llu\n", cache->cur_stats.read_hits);
	len += sprintf(buf + len, "write hit : %llu\n", cache->cur_stats.write_hits);
	len += sprintf(buf + len, "dirty write hit : %llu\n", cache->cur_stats.dirty_write_hits);
	len += sprintf(buf + len, "read hit percent (%%) : %llu\n", cache->cur_stats.reads?
								    cache->cur_stats.read_hits * 100 / cache->cur_stats.reads:
								    0);
	len += sprintf(buf + len, "write hit percent (%%) : %llu\n", cache->cur_stats.writes?
								     cache->cur_stats.write_hits * 100 / cache->cur_stats.writes:
								     0);

	return len;
}
static DM_ATTR_RO(curr_stats);

static ssize_t dm_attr_nr_commit_show(struct cache *cache, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "nr commits: %d\n", atomic_read(&cache->nr_commits));
	len += sprintf(buf + len, "nr commits (less than 1 sec): %d\n", atomic_read(&cache->nr_commits_1000));
	len += sprintf(buf + len, "nr commits (less than 0.5 secs): %d\n", atomic_read(&cache->nr_commits_500));
	len += sprintf(buf + len, "nr commits (less than 0.1 min): %d\n", atomic_read(&cache->nr_commits_100));

	return len;
}
static DM_ATTR_RO(nr_commit);

static ssize_t dm_attr_low_dirty_thresh_show(struct cache_client *client, char *buf)
{
	return sprintf(buf, "%u\n", client->opt.low_dirty_thresh);
}

static ssize_t dm_attr_low_dirty_thresh_store(struct cache_client *client, const char *buf,
					      size_t count)
{
	unsigned new;

	if (kstrtouint(buf, 10, &new))
		return -EIO;

	if (new >= client->opt.high_dirty_thresh) {
		DMERR("low_dirty_thresh cannot be higher to high_dirty_thresh (%u)", client->opt.high_dirty_thresh);
		return -EINVAL;
	}

	spin_lock_irq(&client->lock);
	client->opt.low_dirty_thresh = new;
	spin_unlock_irq(&client->lock);

	policy_set_config_value(client->policy, "low_dirty_thresh", buf);
	return count;
}
static DM_CLIENT_ATTR_WR(low_dirty_thresh);

static ssize_t dm_attr_high_dirty_thresh_show(struct cache_client *client, char *buf)
{
	return sprintf(buf, "%u\n", client->opt.high_dirty_thresh);
}

static ssize_t dm_attr_high_dirty_thresh_store(struct cache_client *client, const char *buf,
					       size_t count)
{
	unsigned new;

	if (kstrtouint(buf, 10, &new))
		return -EIO;

	if (new <= client->opt.low_dirty_thresh) {
		DMERR("high_dirty_thresh cannot be lower to low_dirty_thresh (%u)", client->opt.low_dirty_thresh);
		return -EINVAL;
	}

	spin_lock_irq(&client->lock);
	client->opt.high_dirty_thresh = new;
	spin_unlock_irq(&client->lock);

	policy_set_config_value(client->policy, "high_dirty_thresh", buf);
	return count;
}
static DM_CLIENT_ATTR_WR(high_dirty_thresh);

static ssize_t dm_attr_raw_stat_show(struct cache_client *client, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "reads : %d\n", atomic_read(&client->stats.reads));
	len += sprintf(buf + len, "writes : %d\n", atomic_read(&client->stats.writes));
	len += sprintf(buf + len, "read hit : %d\n", atomic_read(&client->stats.read_hit));
	len += sprintf(buf + len, "read miss : %d\n", atomic_read(&client->stats.read_miss));
	len += sprintf(buf + len, "write hit : %d\n", atomic_read(&client->stats.write_hit));
	len += sprintf(buf + len, "write miss : %d\n", atomic_read(&client->stats.write_miss));
	len += sprintf(buf + len, "dirty write hit : %d\n", atomic_read(&client->stats.dirty_write_hit));
	len += sprintf(buf + len, "discard : %d\n", atomic_read(&client->stats.discard_count));
	len += sprintf(buf + len, "direct : %d\n", atomic_read(&client->stats.direct_count));
	len += sprintf(buf + len, "defer bio : %d\n", atomic_read(&client->stats.defer_bio));
	len += sprintf(buf + len, "disk reads : %d\n", atomic_read(&client->stats.disk_reads));
	len += sprintf(buf + len, "disk writes : %d\n", atomic_read(&client->stats.disk_writes));
	len += sprintf(buf + len, "ssd reads : %d\n", atomic_read(&client->stats.ssd_reads));
	len += sprintf(buf + len, "ssd writes : %d\n", atomic_read(&client->stats.ssd_writes));
	len += sprintf(buf + len, "uncached reads : %d\n", atomic_read(&client->stats.uncached_reads));
	len += sprintf(buf + len, "uncached writes : %d\n", atomic_read(&client->stats.uncached_writes));
	len += sprintf(buf + len, "uncached seq reads : %d\n", atomic_read(&client->stats.uncached_seq_reads));
	len += sprintf(buf + len, "uncached seq writes : %d\n", atomic_read(&client->stats.uncached_seq_writes));

	return len;
}
static DM_CLIENT_ATTR_RO(raw_stat);

static ssize_t dm_attr_stuck_io_show(struct cache_client *client, char *buf)
{
	ssize_t len = 0;

	len += sprintf(buf + len, "in_io %d\n", atomic_read(&in_io));
	len += sprintf(buf + len, "remap_io %d\n", atomic_read(&remap_io));
	len += sprintf(buf + len, "handle_io %d\n", atomic_read(&h_io));
	len += sprintf(buf + len, "rw_io %d\n", atomic_read(&rw_io));
	len += sprintf(buf + len, "mg_io %d\n", atomic_read(&mg_io));
	len += sprintf(buf + len, "mg2_io %d\n", atomic_read(&mg2_io));
	len += sprintf(buf + len, "mg3_io %d\n", atomic_read(&mg3_io));
	len += sprintf(buf + len, "rq_io %d\n", atomic_read(&rq_io));
	len += sprintf(buf + len, "lock_cell %d\n", atomic_read(&lock_cell));
	len += sprintf(buf + len, "lock2_cell %d\n", atomic_read(&lock2_cell));
	len += sprintf(buf + len, "no unlock %d\n", atomic_read(&nounlock));
	len += sprintf(buf + len, "defer_io %u\n", bio_list_size(&client->deferred_bios));
	len += sprintf(buf + len, "prison stuck bio %u\n", prison_stuck_bio(client->cache));
	len += sprintf(buf + len, "prison lock cnt %u\n", prison_lock_cnt(client->cache));
	len += sprintf(buf + len, "prison lock func cnt %u\n", prison_func_lock_cnt(client->cache));
	len += sprintf(buf + len, "zero size io %d\n", atomic_read(&zero_io));

	return len;
}
static DM_CLIENT_ATTR_RO(stuck_io);

static ssize_t dm_attr_cache_all_show(struct cache_client *client, char *buf)
{
	return sprintf(buf, "%d\n", client->opt.cache_all);
}

static ssize_t dm_attr_cache_all_store(struct cache_client *client, const char *buf,
				       size_t count)
{
	int new;

	if (kstrtoint(buf, 10, &new))
		return -EIO;

	if (new == client->opt.cache_all)
		return count;

	spin_lock_irq(&client->lock);
	client->opt.cache_all = new;
	spin_unlock_irq(&client->lock);

	return count;
}
static DM_CLIENT_ATTR_WR(cache_all);

static ssize_t dm_attr_size_thresh_show(struct cache_client *client, char *buf)
{
	return sprintf(buf, "%d\n", client->opt.size_thresh);
}

static ssize_t dm_attr_size_thresh_store(struct cache_client *client, const char *buf,
					 size_t count)
{
	int new;

	if (kstrtoint(buf, 10, &new))
		return -EIO;

	if (new == client->opt.size_thresh)
		return count;

	spin_lock_irq(&client->lock);
	client->opt.size_thresh = new;
	spin_unlock_irq(&client->lock);

	return count;
}
static DM_CLIENT_ATTR_WR(size_thresh);

static ssize_t dm_attr_skip_seq_thresh_kb_show(struct cache_client *client, char *buf)
{
	return sprintf(buf, "%llu\n", client->opt.skip_seq_thresh >> 1);
}

static ssize_t dm_attr_skip_seq_thresh_kb_store(struct cache_client *client, const char *buf,
						size_t count)
{
	unsigned long long thresh;

	if (kstrtoull(buf, 10, &thresh))
		return -EIO;

	spin_lock_irq(&client->lock);
	client->opt.skip_seq_thresh = thresh << 1;
	spin_unlock_irq(&client->lock);

	return count;
}
static DM_CLIENT_ATTR_WR(skip_seq_thresh_kb);

static void dm_cache_kobj_release(struct kobject *kobj)
{
	struct cache *cache = container_of(kobj, struct cache, kobj);

	complete(&cache->can_destroy);
	return;
}

static void dm_client_kobj_release(struct kobject *kobj)
{

	DMINFO("release client kobj");
	return;
}

static struct attribute *dm_attrs[] = {
	&dm_attr_zero_stats.attr,
	&dm_attr_idle_period.attr,
	&dm_attr_curr_stats.attr,
	&dm_attr_raw_stats.attr,
	&dm_attr_page_pool_max.attr,
	&dm_attr_nr_commit.attr,
	NULL,
};

static struct attribute *dm_client_attrs[] = {
	&dm_attr_low_dirty_thresh.attr,
	&dm_attr_high_dirty_thresh.attr,
	&dm_attr_raw_stat.attr,
	&dm_attr_cache_all.attr,
	&dm_attr_stuck_io.attr,
	&dm_attr_skip_seq_thresh_kb.attr,
	&dm_attr_size_thresh.attr,
	NULL,
};

static const struct sysfs_ops dm_sysfs_ops = {
	.show	= dm_attr_show,
	.store	= dm_attr_store,
};

static const struct sysfs_ops dm_client_sysfs_ops = {
	.show	= dm_client_attr_show,
	.store	= dm_client_attr_store,
};

static struct kobj_type dm_ktype = {
	.sysfs_ops	= &dm_sysfs_ops,
	.default_attrs	= dm_attrs,
	.release	= dm_cache_kobj_release,
};

static struct kobj_type dm_client_ktype = {
	.sysfs_ops	= &dm_client_sysfs_ops,
	.default_attrs	= dm_client_attrs,
	.release	= dm_client_kobj_release,
};

/*----------------------------------------------------------------*/

/*
 * Construct a cache device mapping.
 *
 * cache <metadata dev> <cache dev> <block size>
 *       <#feature args> [<feature arg>]*
 *
 * metadata dev    : fast device holding the persistent metadata
 * cache dev	   : fast device holding cached data blocks
 * block size	   : cache unit size in sectors
 *
 * #feature args   : number of feature arguments passed
 * feature args    : writethrough.  (The default is writeback.)
 *
 *
 * Optional feature arguments are:
 *   writethrough  : write through caching that prohibits cache block
 *		     content from being different from origin block content.
 *		     Without this argument, the default behaviour is to write
 *		     back cache block contents later for performance reasons,
 *		     so they may differ from the corresponding origin blocks.
 */
struct cache_args {
	struct dm_target *ti;

	struct dm_dev *metadata_dev;

	struct dm_dev *cache_dev;
	sector_t cache_sectors;
	struct mapped_device *cache_md;

	uint32_t block_size;

	struct cache_features features;
};

static void destroy_cache_args(struct cache_args *ca)
{
	if (ca->metadata_dev)
		dm_put_device(ca->ti, ca->metadata_dev);

	if (ca->cache_dev)
		dm_put_device(ca->ti, ca->cache_dev);

	if (ca->cache_md)
		dm_put(ca->cache_md);

	kfree(ca);
}

static bool at_least_one_arg(struct dm_arg_set *as, char **error)
{
	if (!as->argc) {
		*error = "Insufficient args";
		return false;
	}

	return true;
}

static int parse_metadata_dev(struct cache_args *ca, struct dm_arg_set *as,
			      char **error)
{
	int r;
	sector_t metadata_dev_size;
	char b[BDEVNAME_SIZE];

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	r = dm_get_device(ca->ti, dm_shift_arg(as), FMODE_READ | FMODE_WRITE,
			  &ca->metadata_dev);
	if (r) {
		*error = "Error opening metadata device";
		return r;
	}

	metadata_dev_size = get_dev_size(ca->metadata_dev);
	if (metadata_dev_size > DM_CACHE_METADATA_MAX_SECTORS_WARNING)
		DMWARN("Metadata device %s is larger than %u sectors: excess space will not be used.",
		       bdevname(ca->metadata_dev->bdev, b), DM_CACHE_METADATA_MAX_SECTORS);

	return 0;
}

static int parse_cache_dev(struct cache_args *ca, struct dm_arg_set *as,
			   char **error)
{
	int r;
	struct mapped_device *md;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	r = dm_get_device(ca->ti, dm_shift_arg(as), FMODE_READ | FMODE_WRITE,
			  &ca->cache_dev);
	if (r) {
		*error = "Error opening cache device";
		return r;
	}

	md = dm_get_md(ca->cache_dev->bdev->bd_dev);
	if (!md) {
		*error = "Error getting cache md";
		return -EINVAL;
	}

	ca->cache_md = md;

	ca->cache_sectors = get_dev_size(ca->cache_dev);

	return 0;
}

static int parse_block_size(struct cache_args *ca, struct dm_arg_set *as,
			    char **error)
{
	unsigned long block_size;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	if (kstrtoul(dm_shift_arg(as), 10, &block_size) || !block_size ||
	    block_size < DATA_DEV_BLOCK_SIZE_MIN_SECTORS ||
	    block_size > DATA_DEV_BLOCK_SIZE_MAX_SECTORS ||
	    block_size & (DATA_DEV_BLOCK_SIZE_MIN_SECTORS - 1)) {
		*error = "Invalid data block size";
		return -EINVAL;
	}

	if (block_size > ca->cache_sectors) {
		*error = "Data block size is larger than the cache device";
		return -EINVAL;
	}

	ca->block_size = block_size;

	return 0;
}

static void init_features(struct cache_features *cf)
{
	cf->mode = CM_WRITE;
	cf->metadata_version = 2;
}

static int parse_cache_args(struct cache_args *ca, int argc, char **argv,
			    char **error)
{
	int r;
	struct dm_arg_set as;

	as.argc = argc;
	as.argv = argv;

	r = parse_metadata_dev(ca, &as, error);
	if (r)
		return r;

	r = parse_cache_dev(ca, &as, error);
	if (r)
		return r;

	r = parse_block_size(ca, &as, error);
	if (r)
		return r;

	init_features(&ca->features);

	return 0;
}

/*----------------------------------------------------------------*/

static struct kmem_cache *migration_cache;

#define NOT_CORE_OPTION 1

static int process_config_option(struct cache *cache, const char *key, const char *value)
{
	unsigned long tmp;

	if (!strcasecmp(key, "migration_threshold")) {
		if (kstrtoul(value, 10, &tmp))
			return -EINVAL;

		cache->migration_threshold = tmp;
		return 0;
	}

	return NOT_CORE_OPTION;
}

static int set_config_value(struct cache *cache, const char *key, const char *value)
{
	int r;

	r = process_config_option(cache, key, value);

	if (r)
		DMWARN("bad config value for %s: %s", key, value);

	return r;
}

static int set_policy_config_value(struct cache_client *client, const char *key, const char *value)
{
	int r;

	r = policy_set_config_value(client->policy, key, value);

	if (r)
		DMWARN("bad config value for %s: %s", key, value);

	return r;
}

static int set_config_values(struct cache_client *client, int argc, const char **argv)
{
	int r = 0;
	struct cache *cache = client->cache;

	if (argc & 1) {
		DMWARN("Odd number of policy arguments given but they should be <key> <value> pairs.");
		return -EINVAL;
	}

	while (argc) {
		r = set_config_value(cache, argv[0], argv[1]);
		if (r)
			break;

		argc -= 2;
		argv += 2;
	}

	return r;
}

/*
 * We want the discard block size to be at least the size of the cache
 * block size and have no more than 2^14 discard blocks across the origin.
 */
#define MAX_DISCARD_BLOCKS (1 << 14)

static bool too_many_discard_blocks(sector_t discard_block_size,
				    sector_t origin_size)
{
	(void) sector_div(origin_size, discard_block_size);

	return origin_size > MAX_DISCARD_BLOCKS;
}

static sector_t calculate_discard_block_size(sector_t cache_block_size,
					     sector_t origin_size)
{
	sector_t discard_block_size = cache_block_size;

	if (origin_size)
		while (too_many_discard_blocks(discard_block_size, origin_size))
			discard_block_size *= 2;

	return discard_block_size;
}

static void set_cache_size(struct cache *cache, dm_cblock_t size)
{
	dm_block_t nr_blocks = from_cblock(size);

	if (nr_blocks > (1 << 20) && cache->cache_size != size)
		DMWARN_LIMIT("You have created a cache device with a lot of individual cache blocks (%llu)\n"
			     "All these mappings can consume a lot of kernel memory, and take some time to read/write.\n"
			     "Please consider increasing the cache block size to reduce the overall cache block count.",
			     (unsigned long long) nr_blocks);

	cache->cache_size = size;
}

#define DEFAULT_MIGRATION_THRESHOLD 2048

static void mg_worker_init(struct mg_worker *w, void (*fn)(struct work_struct *))
{
	w->fn = fn;
	spin_lock_init(&w->lock);
	INIT_LIST_HEAD(&w->works);
	INIT_WORK(&w->worker, mg_worker_do_work);
}

static int cache_create(struct cache_args *ca, struct cache **result)
{
	int i, r = 0;
	char **error = &ca->ti->error;
	struct cache *cache;
	struct dm_target *ti = ca->ti;
	struct dm_cache_metadata *cmd;
	bool may_format = ca->features.mode == CM_WRITE;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache) {
		*error = "cache allocation failed";
		return -ENOMEM;
	}

	cache->ti = ca->ti;
	ti->num_flush_bios = 2;
	ti->flush_supported = true;

	ti->num_discard_bios = 1;
	ti->discards_supported = true;

	ti->per_io_data_size = sizeof(struct per_bio_data);

	cache->features = ca->features;
	/* Create bioset for writethrough bios issued to origin */
	r = bioset_init(&cache->bs, BIO_POOL_SIZE, 0, 0);
	if (r) {
		*error = "bioset create failed";
		goto bad;
	}

	cache->metadata_dev = ca->metadata_dev->bdev;
	cache->cache_dev = ca->cache_dev->bdev;
	cache->cache_md = dm_table_get_md(ca->ti->table);

	cache->sectors_per_block = ca->block_size;
	if (dm_set_target_max_io_len(ti, cache->sectors_per_block)) {
		*error = "set max io len error";
		r = -EINVAL;
		goto bad;
	}

	if (ca->block_size & (ca->block_size - 1)) {
		dm_block_t cache_size = ca->cache_sectors;

		cache->sectors_per_block_shift = -1;
		cache_size = block_div(cache_size, ca->block_size);
		set_cache_size(cache, to_cblock(cache_size));
	} else {
		cache->sectors_per_block_shift = __ffs(ca->block_size);
		set_cache_size(cache, to_cblock(ca->cache_sectors >> cache->sectors_per_block_shift));
	}

	if (ca->block_size & (SUB_BLOCK_SIZE - 1)) {
		*error = "block size not aligned";
		r = -EINVAL;
		goto bad;
	}

	cache->sub_blocks_per_cblock =
		dm_sector_div_up(cache->sectors_per_block, SUB_BLOCK_SIZE);
	if (cache->sub_blocks_per_cblock > MAX_SUB_BLOCKS_PER_BLOCK) {
		*error = "block size too large";
		r = -EINVAL;
		goto bad;
	}

	cache->sub_cache_size = from_sblock(cache->sub_blocks_per_cblock);
	cache->sub_cache_size *= from_cblock(cache->cache_size);
	cache->sub_dirty_bitset = alloc_bitset(cache->sub_cache_size);
	if (!cache->sub_dirty_bitset) {
		*error = "could not allocate sub dirty bitset";
		r = -ENOMEM;
		goto bad;
	}

	cache->sub_valid_bitset = alloc_bitset(cache->sub_cache_size);
	if (!cache->sub_valid_bitset) {
		*error = "could not allocate sub valid bitset";
		r = -ENOMEM;
		goto bad;
	}

	for (i = 0; i < MAX_BITMAP_LOCK; i++)
		spin_lock_init(cache->sub_locks + i);

	cache->migration_threshold = DEFAULT_MIGRATION_THRESHOLD;

	cmd = dm_cache_metadata_open(cache->metadata_dev,
				     ca->block_size,
				     (sector_t)SUB_BLOCK_SIZE,
				     may_format,
				     POLICY_HINT_SIZE,
				     ca->features.metadata_version);
	if (IS_ERR(cmd)) {
		*error = "Error creating metadata object";
		r = PTR_ERR(cmd);
		goto bad;
	}
	cache->cmd = cmd;
	set_cache_mode(cache, CM_WRITE);
	if (get_cache_mode(cache) != CM_WRITE) {
		*error = "Unable to get write access to metadata, please check/repair metadata.";
		r = -EINVAL;
		goto bad;
	}

	spin_lock_init(&cache->lock);
	atomic_set(&cache->nr_allocated_migrations, 0);
	atomic_set(&cache->nr_io_migrations, 0);
	atomic_set(&cache->nr_commits, 0);
	atomic_set(&cache->nr_commits_1000, 0);
	atomic_set(&cache->nr_commits_500, 0);
	atomic_set(&cache->nr_commits_100, 0);
	atomic_set(&cache->stats.page_pool_grow, 0);
	atomic_set(&cache->err_stats.ssd_errs, 0);
	init_waitqueue_head(&cache->migration_wait);
	init_completion(&cache->can_destroy);

	cache->opt.idle_period = HZ;
	cache->opt.page_pool_max = PAGE_POOL_MAX;

	r = -ENOMEM;
	atomic_set(&cache->nr_dirty, 0);
	cache->dirty_bitset = alloc_bitset(from_cblock(cache->cache_size));
	if (!cache->dirty_bitset) {
		*error = "could not allocate dirty bitset";
		r = -ENOMEM;
		goto bad;
	}
	clear_bitset(cache->dirty_bitset, from_cblock(cache->cache_size));

	cache->copier = dm_kcopyd_client_create(&dm_kcopyd_throttle);
	if (IS_ERR_OR_NULL(cache->copier)) {
		*error = "could not create kcopyd client";
		r = cache->copier? PTR_ERR(cache->copier) : -ENOMEM;
		cache->copier = NULL;
		goto bad;
	}

	cache->io_client = dm_io_client_create();
	if (IS_ERR_OR_NULL(cache->io_client)) {
		*error = "could not create dm-io client";
		r = cache->io_client? PTR_ERR(cache->io_client) : -ENOMEM;
		cache->io_client = NULL;
		goto bad;
	}

	cache->wq = alloc_workqueue("dm-" DM_MSG_PREFIX, WQ_MEM_RECLAIM, 0);
	if (!cache->wq) {
		*error = "could not create workqueue for metadata object";
		r = -EINVAL;
		goto bad;
	}
	INIT_WORK(&cache->client_recycler, recycle_clients);
	INIT_WORK(&cache->deferred_bio_worker, process_deferred_bios);
	INIT_WORK(&cache->migration_worker, check_migrations);
	INIT_DELAYED_WORK(&cache->waker, do_waker);

	mg_worker_init(&cache->metadata_write_worker, update_metadata);

	cache->prison = dm_bio_prison_create_v2(cache->wq,
	                                        cache->sub_blocks_per_cblock,
						2);
	if (!cache->prison) {
		*error = "could not create bio prison";
		r = -EINVAL;
		goto bad;
	}

	r = mempool_init_slab_pool(&cache->migration_pool, MIGRATION_POOL_SIZE,
				   migration_cache);
	if (r) {
		*error = "Error creating cache's migration mempool";
		r = -ENOMEM;
		goto bad;
	}

	spin_lock_init(&cache->pool_lock);

	INIT_LIST_HEAD(&cache->page_list_group);
	grow_page_pool(cache, PAGE_POOL_SIZE);
	if (!cache->page_pool) {
		*error = "No memory for page pool";
		r = -ENOMEM;
		goto bad;
	}

	cache->manager = cmgr_create(cache->cache_size, cache->sectors_per_block);
	if (!cache->manager) {
		*error = "Could not create cache manager";
		DMERR("could not create cache manager");
		r = -ENOMEM;
		goto bad;
	}

	r = kobject_init_and_add(&cache->kobj, &dm_ktype,
				 dm_kobject(cache->cache_md), "%s", "cache");
	if (r) {
		*error = "Could not create kobject";
		goto bad;
	}

	cache->need_tick_bio = true;
	cache->sized = false;
	cache->invalidate = false;
	cache->commit_requested = false;

	cache->loaded_sub_valid = false;
	cache->loaded_sub_dirty = false;
	cache->loaded_shared_entry = false;

	cache->last_commit_jif = jiffies;

	cache->ref_count = 1;

	spin_lock_init(&cache->invalidation_lock);
	INIT_LIST_HEAD(&cache->invalidation_requests);
	INIT_LIST_HEAD(&cache->active_clients);
	INIT_LIST_HEAD(&cache->client_kobj);

	batcher_init(&cache->committer, commit_op, cache,
		     issue_op, cache, cache->wq);
	iot_init(&cache->tracker);

	__cache_table_insert(cache);

	*result = cache;

	DMWARN("cache pool created");
	return 0;
bad:
	destroy(cache);
	return r;
}

static void __cache_inc(struct cache *cache)
{
	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	cache->ref_count++;
	kobject_get(&cache->kobj);
}

static void __cache_dec(struct cache *cache)
{
	BUG_ON(!mutex_is_locked(&dm_cache_table.mutex));
	BUG_ON(!cache->ref_count);
	kobject_put(&cache->kobj);
	if (!--cache->ref_count) {
		wait_for_completion(&cache->can_destroy);
		destroy(cache);
	}
}

static struct cache *__cache_find(struct mapped_device *cache_md,
				  struct cache_args *ca,
				  char **error, int *created)
{
	struct cache *cache = __cache_table_lookup_metadata_dev(ca->metadata_dev->bdev);
	int r;

	if (cache) {
		if (cache->cache_md != cache_md) {
			*error = "metadata device already in use by a cache";
			return ERR_PTR(-EBUSY);
		}

		if (cache->cache_dev != ca->cache_dev->bdev) {
			*error = "cache device already in use by a cache";
			return ERR_PTR(-EBUSY);
		}

		__cache_inc(cache);
	} else {
		cache = __cache_table_lookup(cache_md);
		if (cache) {
			if (cache->metadata_dev != ca->metadata_dev->bdev ||
			    cache->cache_dev != ca->cache_dev->bdev) {
				*error = "different cache cannot replace a cache";
				return ERR_PTR(-EINVAL);
			}

			__cache_inc(cache);
		} else {
			r = cache_create(ca, &cache);
			if (r) {
				DMERR("cache create failed");
				return ERR_PTR(r);
			}
			*created = 1;
		}
	}

	return cache;
}

static int copy_ctr_args(struct cache *cache, int argc, const char **argv)
{
	unsigned i;
	const char **copy;

	cache->nr_ctr_args = argc;

	if (!argc)
		return 0;

	copy = kcalloc(argc, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	for (i = 0; i < argc; i++) {
		copy[i] = kstrdup(argv[i], GFP_KERNEL);
		if (!copy[i]) {
			while (i--)
				kfree(copy[i]);
			kfree(copy);
			return -ENOMEM;
		}
	}

	cache->ctr_args = copy;

	return 0;
}

static int cache_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	int r = -EINVAL;
	struct cache_args *ca;
	struct cache_c *cache_c;
	struct cache *cache = NULL;
	int created;

	mutex_lock(&dm_cache_table.mutex);

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca) {
		ti->error = "Error allocating memory for cache";
		r = -ENOMEM;
		goto out_unlock;
	}
	ca->ti = ti;

	r = parse_cache_args(ca, argc, argv, &ti->error);
	if (r)
		goto out;

	cache_c = kzalloc(sizeof(*cache_c), GFP_KERNEL);
	if (!cache_c)
		goto out;

	cache = __cache_find(dm_table_get_md(ti->table), ca, &ti->error, &created);
	if (IS_ERR(cache)) {
		r = PTR_ERR(cache);
		goto bad_cache;
	}

	r = copy_ctr_args(cache, argc - 2, (const char **)argv + 2);
	if (r) {
		destroy(cache);
		goto bad_args;
	}

	ti->private = cache_c;
	cache_c->cache = cache;
	cache_c->metadata_dev = ca->metadata_dev;
	cache_c->cache_dev = ca->cache_dev;

	cache->ti = ca->ti;

	ca->metadata_dev = ca->cache_dev = NULL;

	destroy_cache_args(ca);
	mutex_unlock(&dm_cache_table.mutex);
	return r;

bad_args:
	__cache_dec(cache);
bad_cache:
	kfree(cache_c);
out:
	destroy_cache_args(ca);
out_unlock:
	mutex_unlock(&dm_cache_table.mutex);
	return r;
}

/*----------------------------------------------------------------*/

static int cache_map(struct dm_target *ti, struct bio *bio)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	spin_lock_irq(&cache->lock);
	bio_set_dev(bio, cache->cache_dev);
	spin_unlock_irq(&cache->lock);

	return DM_MAPIO_REMAPPED;
}

static int write_dirty_bitset(struct cache *cache)
{
	int r;

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return -EINVAL;

	r = dm_cache_set_dirty_bits(cache->cmd, from_cblock(cache->cache_size), cache->dirty_bitset);
	if (r) {
		metadata_operation_failed(cache, "dm_cache_set_dirty_bits", r);
		return r;
	}

	r = dm_cache_set_sub_dirty_bits(cache->cmd,
	                                cache->sub_cache_size,
	                                cache->sub_dirty_bitset);
	if (r)
		metadata_operation_failed(cache,
		                          "dm_cache_set_sub_dirty_bits", r);

	return r;
}

static int write_discard_bitset(struct cache_client *client)
{
	unsigned i, r;
	struct cache *cache = client->cache;

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return -EINVAL;

	r = dm_cache_client_discard_bitset_resize(client->ccmd,
	                                          client->discard_block_size,
	                                          client->discard_nr_blocks);
	if (r) {
		DMERR("%s: could not resize on-disk discard bitset", cache_device_name(cache));
		metadata_operation_failed(cache, "dm_cache_client_discard_bitset_resize", r);
		return r;
	}

	for (i = 0; i < from_dblock(client->discard_nr_blocks); i++) {
		r = dm_cache_client_set_discard(client->ccmd, to_dblock(i),
		                                is_discarded(client, to_dblock(i)));
		if (r) {
			metadata_operation_failed(cache, "dm_cache_client_set_discard", r);
			return r;
		}
	}

	return 0;
}

static int write_hints(struct cache_client *client)
{
	int r;
	struct cache *cache = client->cache;

	if (get_cache_mode(cache) >= CM_READ_ONLY)
		return -EINVAL;

	r = dm_cache_client_write_hints(client->ccmd, client->policy);
	if (r) {
		metadata_operation_failed(cache, "dm_cache_client_write_hints", r);
		return r;
	}

	return 0;
}

/*
 * returns true on success
 */
static bool sync_client_metadata(struct cache_client *client)
{
	int r1, r2;
	struct cache *cache = client->cache;

	r1 = write_discard_bitset(client);
	if (r1)
		DMERR("%s: could not write discard bitset", cache_device_name(cache));

	save_stats(client);

	r2 = write_hints(client);
	if (r2)
		DMERR("%s: could not write hints", cache_device_name(cache));

	return !r1 && !r2;
}

static int sync_active_client_metadata(struct cache *cache)
{
	struct cache_client *client;
	int r = 1;

	client = get_first_client(cache);
	while (client) {
		r &= sync_client_metadata(client);
		client = get_next_client(cache, client);
	}

	return r;
}

static void sync_cache_metadata(struct cache *cache, bool clean_shutdown)
{
	int r1, r2;

	/*
	 * If writing the above metadata failed, we still commit, but don't
	 * set the clean shutdown flag.  This will effectively force every
	 * dirty bit to be set on reload.
	 */
	r1 = write_dirty_bitset(cache);
	if (r1)
		DMERR("%s: could not write dirty bitset", cache_device_name(cache));

	r2 = commit(cache, !r1 && clean_shutdown);
	if (r2)
		DMERR("%s: could not write cache metadata", cache_device_name(cache));
}

static void cache_postsuspend(struct dm_target *ti)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	BUG_ON(atomic_read(&cache->nr_io_migrations));
	cancel_delayed_work_sync(&cache->waker);
	mg_worker_do_work_sync(cache, &cache->metadata_write_worker);
	flush_workqueue(cache->wq);
	WARN_ON(cache->tracker.in_flight);

	/*
	 * If it's a flush suspend there won't be any deferred bios, so this
	 * call is harmless.
	 */
	requeue_deferred_bios(cache);

	if (get_cache_mode(cache) == CM_WRITE) {
		int r;
		r = sync_active_client_metadata(cache);
		sync_cache_metadata(cache, r);
	}
}

static int load_mapping(void *context, dm_oblock_t oblock, dm_cblock_t cblock,
			bool dirty, uint32_t hint, bool hint_valid)
{
	int r;
	struct cache_client *client = context;
	struct cache *cache = client->cache;

	if (dirty) {
		set_bit(from_cblock(cblock), cache->dirty_bitset);
		inc_nr_dirty(client);
	} else
		clear_bit(from_cblock(cblock), cache->dirty_bitset);

	r = policy_load_mapping(client->policy, oblock, cblock, dirty, hint, hint_valid);
	if (r)
		return r;

	return 0;
}

typedef void (*bitset_setter_fn)(void *, dm_block_t);

/*
 * The block size in the on disk metadata is not
 * neccessarily the same as we're currently using.  So we have to
 * be careful to only set the attribute if we know it
 * covers a complete block of the new size.
 */
struct bitset_load_info {
	//struct cache *cache;
	void *private;

	/*
	 * These blocks are sized using the on disk bit block size, rather
	 * than the current one.
	 */
	sector_t on_disk_block_size;
	sector_t requested_block_size;
	dm_block_t bit_begin, bit_end, max_bit;

	unsigned max_bits;
	bitset_setter_fn setter;
};

static void bitset_load_info_init(void *ptr, unsigned max_bits,
                                  sector_t requested_block_size,
                                  bitset_setter_fn fn,
				  struct bitset_load_info *li)
{
	li->private = ptr;
	li->bit_begin = li->bit_end = 0;
	li->max_bits = max_bits;
	li->setter = fn;
	li->requested_block_size = requested_block_size;
}

static void set_bitset_range(struct bitset_load_info *li)
{
	sector_t b = li->bit_begin, e = li->bit_end;

	if (li->bit_begin == li->bit_end)
		return;

	if (li->on_disk_block_size != li->requested_block_size) {
		/*
		 * Convert to sectors.
		 */
		b = li->bit_begin * li->on_disk_block_size;
		e = li->bit_end * li->on_disk_block_size;

		/*
		 * Then convert back to the current dblock size.
		 */
		b = dm_sector_div_up(b, li->requested_block_size);
		sector_div(e, li->requested_block_size);
	}

	/*
	 * The origin may have shrunk, so we need to check we're still in
	 * bounds.
	 */
	if (li->max_bits && e > li->max_bits)
		e = li->max_bits;

	BUG_ON(b >= (sector_t)UINT_MAX);

	for (; b < e; b++)
		li->setter(li->private, (unsigned)(b));
}

static int load_bitset(void *context, sector_t bit_block_size, uint32_t bit, bool set)
{
	struct bitset_load_info *li = context;

	li->on_disk_block_size = bit_block_size;

	if (set) {
		if (bit == li->bit_end)
			/*
			 * We're already in a bit range, just extend it.
			 */
			li->bit_end = li->bit_end + 1ULL;

		else {
			/*
			 * Emit the old range and start a new one.
			 */
			set_bitset_range(li);
			li->bit_begin = bit;
			li->bit_end = li->bit_begin + 1ULL;
		}
	} else {
		set_bitset_range(li);
		li->bit_begin = li->bit_end = 0;
	}

	return 0;
}

static int take_entry(void *context, dm_cblock_t cblock)
{
	struct cache *cache = context;

	cmgr_take_particular_entry(cache->manager, cblock);

	return 0;
}

static bool entry_taken(struct cache *cache, dm_cblock_t cblock)
{
	return cmgr_entry_taken(cache->manager, cblock);
}


static dm_cblock_t get_cache_dev_size(struct cache *cache)
{
	sector_t size = get_bdev_size(cache->cache_dev);
	(void) sector_div(size, cache->sectors_per_block);
	return to_cblock(size);
}

static bool can_resize(struct cache *cache, dm_cblock_t new_size)
{
	if (from_cblock(new_size) > from_cblock(cache->cache_size)) {
#if 0
		if (cache->sized) {
			DMERR("%s: unable to extend cache due to missing cache table reload",
			      cache_device_name(cache));
			return false;
		}
#endif
		return true;
	}

	/*
	 * We can't drop a dirty block when shrinking the cache.
	 */
	while (from_cblock(new_size) < from_cblock(cache->cache_size)) {
		new_size = to_cblock(from_cblock(new_size) + 1);
		if (is_dirty(cache, new_size)) {
			DMERR("%s: unable to shrink cache; cache block %llu is dirty",
			      cache_device_name(cache),
			      (unsigned long long) from_cblock(new_size));
			return false;
		}
	}

	return true;
}

static int resize_cache_content(struct cache *cache, dm_cblock_t old_size, dm_cblock_t new_size)
{
	unsigned long *sub_dirty_bitset;
	unsigned long *sub_valid_bitset;
	unsigned long *dirty_bitset;
	unsigned long sub_cache_size, old_sub_cache_size;
	int r;

	sub_cache_size =
		from_sblock(cache->sub_blocks_per_cblock) *
		from_cblock(cache->cache_size);

	sub_dirty_bitset = alloc_bitset(sub_cache_size);
	if (!sub_dirty_bitset) {
		DMERR("could not allocate sub dirty bitset");
		return -ENOMEM;
	}

	sub_valid_bitset = alloc_bitset(sub_cache_size);
	if (!sub_valid_bitset) {
		DMERR("could not allocate sub valid bitset");
		free_bitset(sub_dirty_bitset);
		return -ENOMEM;
	}

	dirty_bitset = alloc_bitset(from_cblock(cache->cache_size));
	if (!dirty_bitset) {
		DMERR("could not allocate dirty bitset");
		free_bitset(sub_valid_bitset);
		free_bitset(sub_dirty_bitset);
		return -ENOMEM;
	}

	r = cmgr_resize(cache->manager, new_size - old_size);
	if (r) {
		DMERR("could not resize cache manager");
		free_bitset(sub_valid_bitset);
		free_bitset(sub_dirty_bitset);
		free_bitset(dirty_bitset);
		return r;
	}

	clear_bitset(sub_dirty_bitset, sub_cache_size);
	clear_bitset(sub_valid_bitset, sub_cache_size);
	clear_bitset(dirty_bitset, cache->cache_size);

	old_sub_cache_size = from_sblock(cache->sub_blocks_per_cblock) * from_cblock(old_size);
	copy_bitset(sub_dirty_bitset, cache->sub_dirty_bitset, old_sub_cache_size);
	copy_bitset(sub_valid_bitset, cache->sub_valid_bitset, old_sub_cache_size);
	copy_bitset(dirty_bitset, cache->dirty_bitset, from_cblock(old_size));

	cache->sub_cache_size = sub_cache_size;
	free_bitset(cache->sub_dirty_bitset);
	free_bitset(cache->sub_valid_bitset);
	free_bitset(cache->dirty_bitset);

	cache->sub_dirty_bitset = sub_dirty_bitset;
	cache->sub_valid_bitset = sub_valid_bitset;
	cache->dirty_bitset = dirty_bitset;

	return 0;
}

static int resize_cache_dev(struct cache *cache, dm_cblock_t new_size)
{
	int r;
	int changed = (cache->cache_size != 0 && new_size != cache->cache_size);
	struct cache_client *client;
	dm_cblock_t old_size;


	r = dm_cache_resize(cache->cmd, new_size);
	if (r) {
		DMERR("%s: could not resize cache metadata", cache_device_name(cache));
		metadata_operation_failed(cache, "dm_cache_resize", r);
		return r;
	}

	old_size = cache->cache_size;
	set_cache_size(cache, new_size);

	if (!changed)
		return 0;

	r = resize_cache_content(cache, old_size, new_size);
	if (r) {
		set_cache_size(cache, old_size);
		return r;
	}

	client = get_first_client(cache);
	while (client) {
		r = policy_cache_resize(client->policy, client->origin_sectors);
		client = get_next_client(cache, client);
	}

	return 0;
}

static int cache_preresume(struct dm_target *ti)
{
	int r = 0;
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;
	dm_cblock_t csize = get_cache_dev_size(cache);

	/*
	 * Check to see if the cache has resized.
	 */
	if (!cache->sized) {
		r = resize_cache_dev(cache, csize);
		if (r)
			return r;

		cache->sized = true;

	} else if (csize != cache->cache_size) {
		if (!can_resize(cache, csize))
			return -EINVAL;

		r = resize_cache_dev(cache, csize);
		if (r)
			return r;
	}

	// FIXME: looks like this could be postponed
	if (!cache->loaded_shared_entry) {
		r = dm_cache_walk(cache->cmd, take_entry, cache);
		cache->loaded_shared_entry = true;
	}

	if (!cache->loaded_sub_dirty) {
		bool skipped;
		struct bitset_load_info li;

		clear_bitset(cache->sub_dirty_bitset, cache->sub_cache_size);

		bitset_load_info_init(cache,
		                      cache->sub_cache_size,
		                      SUB_BLOCK_SIZE, set_sub_dirty_bit, &li);
		r = dm_cache_load_sub_dirties(cache->cmd, load_bitset, &li, &skipped);
		if (r) {
			DMERR("%s: could not load sub block dirty bitmap",
			       cache_device_name(cache));
			metadata_operation_failed(cache, "dm_cache_load_sub_dirties", r);
			return r;
		}
		set_bitset_range(&li);

		cache->loaded_sub_dirty = !skipped;
	}

	if (!cache->loaded_sub_valid) {
		struct bitset_load_info li;

		clear_bitset(cache->sub_valid_bitset, cache->sub_cache_size);

		bitset_load_info_init(cache,
		                      cache->sub_cache_size,
		                      SUB_BLOCK_SIZE,
		                      cache->loaded_sub_dirty ?
		                      set_sub_valid_bit : set_sub_valid_and_dirty_bit,
		                      &li);
		r = dm_cache_load_sub_valids(cache->cmd, load_bitset, &li);
		if (r) {
			DMERR("%s: could not load sub block valid bitmap",
			       cache_device_name(cache));
			metadata_operation_failed(cache, "dm_cache_load_sub_valids", r);
			return r;
		}
		set_bitset_range(&li);

		cache->loaded_sub_valid = cache->loaded_sub_dirty = true;
	}

	return r;
}

static void cache_client_get(struct cache_client *client);
static void cache_client_put(struct cache_client *client);
static struct cache_client *get_first_client(struct cache *cache)
{
	struct cache_client *client = NULL;

	rcu_read_lock();
	if (!list_empty(&cache->active_clients)) {
		client = list_entry_rcu(cache->active_clients.next, struct cache_client, list);
		cache_client_get(client);
	}
	rcu_read_unlock();

	return client;
}

static struct cache_client *get_next_client(struct cache *cache, struct cache_client *client)
{
	struct cache_client *old_client = client;

	rcu_read_lock();
	list_for_each_entry_continue_rcu(client, &cache->active_clients, list) {
		cache_client_get(client);
		cache_client_put(old_client);
		rcu_read_unlock();
		return client;
	}
	cache_client_put(old_client);
	rcu_read_unlock();

	return NULL;
}

static void cache_suspend_active_clients(struct cache *cache)
{
	struct cache_client *client;

	client = get_first_client(cache);
	while (client) {
		dm_internal_suspend_noflush(client->client_md);
		client = get_next_client(cache, client);
	}
}

static void cache_resume_active_clients(struct cache *cache)
{
	struct cache_client *client;

	client = get_first_client(cache);
	while (client) {
		dm_internal_resume(client->client_md);
		refcount_set(&client->nr_meta_updates, 1);
		allow_background_work(client);
		client = get_next_client(cache, client);
	}
}

static void cache_resume(struct dm_target *ti)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	cache_resume_active_clients(cache);

	cache->need_tick_bio = true;

	spin_lock_irq(&cache->lock);
	cache->suspended = false;
	spin_unlock_irq(&cache->lock);

	do_waker(&cache->waker.work);
}

static void cache_presuspend(struct dm_target *ti)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	spin_lock_irq(&cache->lock);
	cache->suspended = true;
	spin_unlock_irq(&cache->lock);

	cache_suspend_active_clients(cache);
}

static void cache_presuspend_undo(struct dm_target *ti)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	cache_resume_active_clients(cache);

	spin_lock_irq(&cache->lock);
	cache->suspended = false;
	spin_unlock_irq(&cache->lock);
}

static void emit_flags(struct cache *cache, char *result,
		       unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;
	struct cache_features *cf = &cache->features;
	unsigned count = (cf->metadata_version == 2);

	DMEMIT("%u ", count);

	if (cf->metadata_version == 2)
		DMEMIT("metadata2 ");

	*sz_ptr = sz;
}

/*
 * Status format:
 *
 * <metadata block size> <#used metadata blocks>/<#total metadata blocks>
 * <cache block size> <#used cache blocks>/<#total cache blocks>
 * <#read hits> <#read misses> <#write hits> <#write misses>
 * <#demotions> <#promotions> <#dirty>
 * <#features> <features>*
 * <#core args> <core args>
 * <policy name> <#policy args> <policy args>* <cache metadata mode> <needs_check>
 */
static void cache_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	int r = 0;
	unsigned i;
	ssize_t sz = 0;
	dm_block_t nr_free_blocks_metadata = 0;
	dm_block_t nr_blocks_metadata = 0;
	char buf[BDEVNAME_SIZE];
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;
	dm_cblock_t residency;
	bool needs_check;

	switch (type) {
	case STATUSTYPE_INFO:
		if (get_cache_mode(cache) == CM_FAIL) {
			DMEMIT("Fail");
			break;
		}

		/* Commit to ensure statistics aren't out-of-date */
		if (!(status_flags & DM_STATUS_NOFLUSH_FLAG) && !dm_suspended(ti))
			(void) commit(cache, false);

		r = dm_cache_get_free_metadata_block_count(cache->cmd, &nr_free_blocks_metadata);
		if (r) {
			DMERR("%s: dm_cache_get_free_metadata_block_count returned %d",
			      cache_device_name(cache), r);
			goto err;
		}

		r = dm_cache_get_metadata_dev_size(cache->cmd, &nr_blocks_metadata);
		if (r) {
			DMERR("%s: dm_cache_get_metadata_dev_size returned %d",
			      cache_device_name(cache), r);
			goto err;
		}

		residency = to_cblock(cmgr_nr_allocated(cache->manager));

		DMEMIT("%u %llu/%llu %llu %llu/%llu %llu ",
		       (unsigned)DM_CACHE_METADATA_BLOCK_SIZE,
		       (unsigned long long)(nr_blocks_metadata - nr_free_blocks_metadata),
		       (unsigned long long)nr_blocks_metadata,
		       (unsigned long long)cache->sectors_per_block,
		       (unsigned long long) from_cblock(residency),
		       (unsigned long long) from_cblock(cache->cache_size),
		       (unsigned long long) atomic_read(&cache->err_stats.ssd_errs));

		emit_flags(cache, result, maxlen, &sz);

		DMEMIT("2 migration_threshold %llu ", (unsigned long long) cache->migration_threshold);

		if (get_cache_mode(cache) == CM_READ_ONLY)
			DMEMIT("ro ");
		else
			DMEMIT("rw ");

		r = dm_cache_metadata_needs_check(cache->cmd, &needs_check);

		if (r || needs_check)
			DMEMIT("needs_check ");
		else
			DMEMIT("- ");

		break;

	case STATUSTYPE_TABLE:
		format_dev_t(buf, cache_c->metadata_dev->bdev->bd_dev);
		DMEMIT("%s ", buf);
		format_dev_t(buf, cache_c->cache_dev->bdev->bd_dev);
		DMEMIT("%s ", buf);

		if (cache->nr_ctr_args > 1)
			for (i = 0; i < cache->nr_ctr_args - 1; i++)
				DMEMIT(" %s", cache->ctr_args[i]);

		if (cache->nr_ctr_args)
			DMEMIT(" %s", cache->ctr_args[cache->nr_ctr_args - 1]);
	}

	return;

err:
	DMEMIT("Error");
}

/*
 * Defines a range of cblocks, begin to (end - 1) are in the range.  end is
 * the one-past-the-end value.
 */
struct cblock_range {
	dm_cblock_t begin;
	dm_cblock_t end;
};

/*
 * A cache block range can take two forms:
 *
 * i) A single cblock, eg. '3456'
 * ii) A begin and end cblock with a dash between, eg. 123-234
 */
static int parse_cblock_range(struct cache *cache, const char *str,
			      struct cblock_range *result)
{
	char dummy;
	uint64_t b, e;
	int r;

	/*
	 * Try and parse form (ii) first.
	 */
	r = sscanf(str, "%llu-%llu%c", &b, &e, &dummy);
	if (r < 0)
		return r;

	if (r == 2) {
		result->begin = to_cblock(b);
		result->end = to_cblock(e);
		return 0;
	}

	/*
	 * That didn't work, try form (i).
	 */
	r = sscanf(str, "%llu%c", &b, &dummy);
	if (r < 0)
		return r;

	if (r == 1) {
		result->begin = to_cblock(b);
		result->end = to_cblock(from_cblock(result->begin) + 1u);
		return 0;
	}

	DMERR("%s: invalid cblock range '%s'", cache_device_name(cache), str);
	return -EINVAL;
}

static int validate_cblock_range(struct cache *cache, struct cblock_range *range)
{
	uint64_t b = from_cblock(range->begin);
	uint64_t e = from_cblock(range->end);
	uint64_t n = from_cblock(cache->cache_size);

	if (b >= n) {
		DMERR("%s: begin cblock out of range: %llu >= %llu",
		      cache_device_name(cache), b, n);
		return -EINVAL;
	}

	if (e > n) {
		DMERR("%s: end cblock out of range: %llu > %llu",
		      cache_device_name(cache), e, n);
		return -EINVAL;
	}

	if (b >= e) {
		DMERR("%s: invalid cblock range: %llu >= %llu",
		      cache_device_name(cache), b, e);
		return -EINVAL;
	}

	return 0;
}

static inline dm_cblock_t cblock_succ(dm_cblock_t b)
{
	return to_cblock(from_cblock(b) + 1);
}

static int validate_cblock_avail(struct cache *cache, struct cblock_range range)
{
	int r = 0;

	while (range.begin != range.end) {
		r = entry_taken(cache, range.begin);
		if (r) {
			r = -EINVAL;
			DMERR("Cannot do trim on range %u-%u", range.begin, range.end);
			break;
		}

		range.begin = cblock_succ(range.begin);
	}

	return r;
}

static int request_invalidation(struct cache *cache, struct cblock_range *range)
{
	int r = 0;
#if 0
	/*
	 * We don't need to do any locking here because we know we're in
	 * passthrough mode.  There's is potential for a race between an
	 * invalidation triggered by an io and an invalidation message.  This
	 * is harmless, we must not worry if the policy call fails.
	 */
	while (range->begin != range->end) {
		r = invalidate_cblock(cache, range->begin);
		if (r)
			return r;

		range->begin = cblock_succ(range->begin);
	}
#endif
	cache->commit_requested = true;
	return r;
}

static void trim_endio(struct bio *bio)
{
	struct cache *cache = bio->bi_private;
	dm_cblock_t begin, end;

	begin = to_cblock(dm_sector_div_up(bio->bi_iter.bi_sector, cache->sectors_per_block));
	end = to_cblock(dm_sector_div_up(bio_end_sector(bio), cache->sectors_per_block));

	if (bio->bi_status)
		DMERR("trim on range %u-%u failed", begin, end);

	bio_put(bio);
}

static void request_trim(struct cache *cache, struct cblock_range *range)
{
	sector_t begin, end, len;
	struct bio *bio;

	DMINFO("%s do trim on range %u-%u", __func__, range->begin, range->end);

	while (range->begin < range->end) {

		begin = range->begin * cache->sectors_per_block;
		end = min(range->end, to_cblock(from_cblock(range->begin) + 10)) * cache->sectors_per_block;

		bio = bio_alloc_bioset(GFP_NOIO, 0, &cache->bs);
		bio->bi_iter.bi_sector = begin;
		bio->bi_iter.bi_size = len = to_bytes(end - begin);
		bio_set_dev(bio, cache->cache_dev);
		bio->bi_end_io = trim_endio;
		bio_set_op_attrs(bio, REQ_OP_DISCARD, 0);
		bio->bi_private = cache;

		submit_bio_noacct(bio);

		if (len < to_bytes(cache->sectors_per_block * 10))
			break;

		range->begin = to_cblock(from_cblock(range->begin) + 10);
	}

}

static int process_invalidate_cblocks_message(struct cache *cache, unsigned count,
					      const char **cblock_ranges)
{
	int r = 0;
	unsigned i;
	struct cblock_range range;

#if 0 //  FIXME
	if (!passthrough_mode(cache)) {
		DMERR("%s: cache has to be in passthrough mode for invalidation",
		      cache_device_name(cache));
		return -EPERM;
	}
#endif

	for (i = 0; i < count; i++) {
		r = parse_cblock_range(cache, cblock_ranges[i], &range);
		if (r)
			break;

		r = validate_cblock_range(cache, &range);
		if (r)
			break;

		/*
		 * Pass begin and end origin blocks to the worker and wake it.
		 */
		r = request_invalidation(cache, &range);
		if (r)
			break;
	}

	return r;
}

static int process_dump_sub_bitset(struct cache *cache, unsigned argc,
                                   const char **argv)
{
	unsigned int i, target, blocks;
	unsigned long *bitset;

	if (argc != 2)
		return -EINVAL;

	if (!strcasecmp(argv[0], "dirty"))
		bitset = cache->sub_dirty_bitset;
	else if (!strcasecmp(argv[0], "valid"))
		bitset = cache->sub_valid_bitset;
	else
		return -EINVAL;

	if (kstrtouint(argv[1], 10, &blocks))
		return -EINVAL;

	DMERR("=================== %s bitset ===================", argv[0]);
	target = cache_block_to_sbit(cache, to_cblock(blocks));
	for (i = 0; i < target; i += BITS_PER_LONG) {
		DMERR("%8d: %018lx", i, *bitset);
		bitset++;
	}

	return 0;
}

static int read_dev_id(const char *arg, uint64_t *dev_id)
{
	if (!kstrtoull(arg, 10, (unsigned long long *)dev_id) &&
	    *dev_id <= MAX_DEV_ID)
		return 0;

	DMWARN("Message received with invalid device id: %s", arg);

	return -EINVAL;
}

static int process_create_client_message(struct cache *cache, unsigned argc,
					 const char **argv)
{
	uint64_t dev_id;
	int r;

	if (argc != 1)
		return -EINVAL;

	r = read_dev_id(argv[0], &dev_id);
	if (r)
		return r;
#if 1
	r = dm_cache_create_client(cache->cmd, dev_id);
	if (r) {
		DMWARN("Device id (%s) create failed", argv[0]);
		return r;
	}

#else
	r = client_in_use(cache, dev_id);
	if (r) {
		DMWARN("Device id (%s) alread in use", argv[0]);
		return -EEXIST;
	}
#endif

	return 0;
}

static int process_delete_message(struct cache *cache, unsigned argc,
				  const char **argv)
{
	uint64_t dev_id;
	int r;

	if (argc != 1)
		return -EINVAL;

	r = read_dev_id(argv[0], &dev_id);
	if (r)
		return r;

	r = dm_cache_delete_client(cache->cmd, dev_id);
	if (r) {
		DMWARN("Unable to remove device, error %d", r);
		return r;
	}

	wake_client_recycler(cache);

	return 0;
}

static int process_trim_message(struct cache *cache, unsigned count, const char **argv)
{
	int r = 0;
	unsigned i;
	uint64_t dev_id;
	struct cblock_range range;
	const char **cblock_ranges = argv + 1;

	if (count < 2)
		return -EINVAL;

	r = read_dev_id(argv[0], &dev_id);
	if (r)
		return r;

	count--;

	for (i = 0; i < count; i++) {
		r = parse_cblock_range(cache, cblock_ranges[i], &range);
		if (r)
			break;

		r = validate_cblock_range(cache, &range);
		if (r)
			break;

		r = validate_cblock_avail(cache, range);
		if (r)
			break;
		/*
		 * Pass begin and end origin blocks to the worker and wake it.
		 */
		request_trim(cache, &range);
	}

	return r;
}

/*
 * Supports
 *	"<key> <value>"
 * and
 *     "invalidate_cblocks [(<begin>)|(<begin>-<end>)]*
 *
 * The key migration_threshold is supported by the cache target core.
 */
static int cache_message(struct dm_target *ti, unsigned argc, char **argv,
			 char *result, unsigned maxlen)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;

	if (!argc)
		return -EINVAL;

	if (get_cache_mode(cache) >= CM_READ_ONLY) {
		DMERR("%s: unable to service cache target messages in READ_ONLY or FAIL mode",
		      cache_device_name(cache));
		return -EOPNOTSUPP;
	}

	if (!strcasecmp(argv[0], "invalidate_cblocks"))
		return process_invalidate_cblocks_message(cache, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "dump_sub_bitset"))
		return process_dump_sub_bitset(cache, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "create-client"))
		return process_create_client_message(cache, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "delete"))
		return process_delete_message(cache, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "trim"))
		return process_trim_message(cache, argc - 1, (const char **) argv + 1);
	if (argc != 2)
		return -EINVAL;

	return set_config_value(cache, argv[0], argv[1]);
}

static int cache_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	int r = 0;
	struct cache_c *cache_c = ti->private;

	r = fn(ti, cache_c->cache_dev, 0, get_dev_size(cache_c->cache_dev), data);

	return r;
}

static bool origin_dev_supports_discard(struct block_device *origin_bdev)
{
	struct request_queue *q = bdev_get_queue(origin_bdev);

	return q && blk_queue_discard(q);
}

/*
 * If discard_passdown was enabled verify that the origin device
 * supports discards.  Disable discard_passdown if not.
 */
static void disable_passdown_if_not_supported(struct cache_client *client)
{
	struct block_device *origin_bdev = client->origin_dev->bdev;
	struct queue_limits *origin_limits = &bdev_get_queue(origin_bdev)->limits;
	struct cache *cache = client->cache;
	const char *reason = NULL;
	char buf[BDEVNAME_SIZE];

	if (!client->features.discard_passdown)
		return;

	if (!origin_dev_supports_discard(origin_bdev))
		reason = "discard unsupported";

	else if (origin_limits->max_discard_sectors < cache->sectors_per_block)
		reason = "max discard sectors smaller than a block";

	if (reason) {
		DMWARN("Origin device (%s) %s: Disabling discard passdown.",
		       bdevname(origin_bdev, buf), reason);
		client->features.discard_passdown = false;
	}
}

static void set_discard_limits(struct cache_client *client, struct queue_limits *limits)
{
	struct block_device *origin_bdev = client->origin_dev->bdev;
	struct queue_limits *origin_limits = &bdev_get_queue(origin_bdev)->limits;

	if (!client->features.discard_passdown) {
		/* No passdown is done so setting own virtual limits */
		limits->max_discard_sectors = min_t(sector_t, client->cache->sectors_per_block,
						    client->origin_sectors);
		limits->discard_granularity = client->cache->sectors_per_block << SECTOR_SHIFT;
		return;
	}

	/*
	 * cache_iterate_devices() is stacking both origin and fast device limits
	 * but discards aren't passed to fast device, so inherit origin's limits.
	 */
	limits->max_discard_sectors = origin_limits->max_discard_sectors;
	limits->max_hw_discard_sectors = origin_limits->max_hw_discard_sectors;
	limits->discard_granularity = origin_limits->discard_granularity;
	limits->discard_alignment = origin_limits->discard_alignment;
	limits->discard_misaligned = origin_limits->discard_misaligned;
}

static void cache_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct cache_c *cache_c = ti->private;
	struct cache *cache = cache_c->cache;
	uint64_t io_opt_sectors = limits->io_opt >> SECTOR_SHIFT;

	/*
	 * If the system-determined stacked limits are compatible with the
	 * cache's blocksize (io_opt is a factor) do not override them.
	 */
	if (io_opt_sectors < cache->sectors_per_block ||
	    do_div(io_opt_sectors, cache->sectors_per_block)) {
		blk_limits_io_min(limits, cache->sectors_per_block << SECTOR_SHIFT);
		blk_limits_io_opt(limits, cache->sectors_per_block << SECTOR_SHIFT);
	}

}

/*----------------------------------------------------------------*/

struct cache_work {
	struct work_struct worker;
	struct completion complete;
};

static struct cache_work *to_cache_work(struct work_struct *ws)
{
	return container_of(ws, struct cache_work, worker);
}

static void cache_work_complete(struct cache_work *cw)
{
	complete(&cw->complete);
}

static void cache_work_wait(struct cache_work *cw, struct cache *cache,
			    void (*fn)(struct work_struct *))
{
	INIT_WORK_ONSTACK(&cw->worker, fn);
	init_completion(&cw->complete);
	queue_work(cache->wq, &cw->worker);
	wait_for_completion(&cw->complete);
}

/*----------------------------------------------------------------*/

struct noflush_work {
	struct cache_work cw;
	struct cache_client *client;
};

static struct noflush_work *to_noflush(struct work_struct *ws)
{
	return container_of(to_cache_work(ws), struct noflush_work, cw);
}

static void do_noflush_start(struct work_struct *ws)
{
	struct noflush_work *w = to_noflush(ws);
	w->client->requeue_mode = true;
	requeue_client_deferred_bios(w->client);
	cache_work_complete(&w->cw);
}

static void do_noflush_stop(struct work_struct *ws)
{
	struct noflush_work *w = to_noflush(ws);
	w->client->requeue_mode = false;
	cache_work_complete(&w->cw);
}

static void noflush_work(struct cache_client *client, void (*fn)(struct work_struct *))
{
	struct noflush_work w;

	w.client = client;
	cache_work_wait(&w.cw, client->cache, fn);
}

/*----------------------------------------------------------------*/

static struct target_type cache_target = {
	.name = "cache",
	.version = {2, 2, 0},
	.module = THIS_MODULE,
	.ctr = cache_ctr,
	.dtr = cache_dtr,
	.map = cache_map,
	.presuspend = cache_presuspend,
	.presuspend_undo = cache_presuspend_undo,
	.postsuspend = cache_postsuspend,
	.preresume = cache_preresume,
	.resume = cache_resume,
	.status = cache_status,
	.message = cache_message,
	.iterate_devices = cache_iterate_devices,
	.io_hints = cache_io_hints,
};

/*
 * Construct a cache client device mapping.
 *
 * cache_client <cache dev> <origin dev> <dev id>
 *		<policy> <#policy args> [<policy arg>]*
 *		[<rw mode>]
 *
 * cache dev	   : fast device holding cached data blocks
 * origin dev	   : slow device holding original data blocks
 * dev id	   : the internal device identifier
 *
 * policy	   : the replacement policy to use
 * #policy args    : an even number of policy arguments corresponding
 *		     to key/value pairs passed to the policy
 * policy args	   : key/value pairs passed to the policy
 *		     E.g. 'sequential_threshold 1024'
 *		     See cache-policies.txt for details.
 * rw mode	   : rw, ro, wo
 *
 */

struct client_args {
	struct dm_target *ti;

	struct dm_dev *cache_dev;
	sector_t cache_sectors;
	struct mapped_device *cache_md;

	struct dm_dev *origin_dev;
	sector_t origin_sectors;

	uint64_t dev_id;

	const char *policy_name;
	int policy_argc;
	const char **policy_argv;

	struct client_features features;

};

static void destroy_client_args(struct client_args *ca)
{
	if (ca->cache_dev)
		dm_put_device(ca->ti, ca->cache_dev);

	if (ca->origin_dev)
		dm_put_device(ca->ti, ca->origin_dev);

	if (ca->cache_md)
		dm_put(ca->cache_md);

	kfree(ca);
}

static int parse_client_cache_dev(struct client_args *ca, struct dm_arg_set *as,
				  char **error)
{
	int r;
	struct mapped_device *md;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	r = dm_get_device(ca->ti, dm_shift_arg(as), FMODE_READ | FMODE_WRITE,
			  &ca->cache_dev);
	if (r) {
		*error = "Error opening cache device";
		return r;
	}

	md = dm_get_md(ca->cache_dev->bdev->bd_dev);
	if (!md) {
		*error = "Error getting cache md";
		return -EINVAL;
	}

	ca->cache_md = md;

	ca->cache_sectors = get_dev_size(ca->cache_dev);

	return 0;
}

static int parse_origin_dev(struct client_args *ca, struct dm_arg_set *as,
			    char **error)
{
	int r;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	r = dm_get_device(ca->ti, dm_shift_arg(as), FMODE_READ | FMODE_WRITE,
			  &ca->origin_dev);
	if (r) {
		*error = "Error opening origin device";
		return r;
	}

	ca->origin_sectors = get_dev_size(ca->origin_dev);
	if (ca->ti->len > ca->origin_sectors) {
		*error = "Device size larger than cached device";
		return -EINVAL;
	}

	return 0;
}

static int parse_device_id(struct client_args *ca, struct dm_arg_set *as,
			   char **error)
{
	uint64_t id;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	if (kstrtou64(dm_shift_arg(as), 10, &id)) {
		*error = "Invalid device id";
		return -EINVAL;
	}

	ca->dev_id = id;

	return 0;
}

static void init_client_features(struct client_features *cf)
{
	cf->io_mode = CM_IO_WRITEBACK;
	cf->discard_passdown = true;
	cf->optional_cache = false;
	cf->discard_return_blocks = false;
}

static int parse_features(struct client_args *ca, struct dm_arg_set *as,
			  char **error)
{
	static const struct dm_arg _args[] = {
		{0, 3, "Invalid number of cache feature arguments"},
	};

	int r, mode_ctr = 0, filter_ctr = 0;
	unsigned argc;
	const char *arg;
	struct client_features *cf = &ca->features;

	init_client_features(cf);

	r = dm_read_arg_group(_args, as, &argc, error);
	if (r)
		return -EINVAL;

	while (argc--) {
		arg = dm_shift_arg(as);

		if (!strcasecmp(arg, "writeback")) {
			cf->io_mode = CM_IO_WRITEBACK;
			mode_ctr++;
		}

		else if (!strcasecmp(arg, "writethrough")) {
			cf->io_mode = CM_IO_WRITETHROUGH;
			mode_ctr++;
		}

		else if (!strcasecmp(arg, "passthrough")) {
			cf->io_mode = CM_IO_PASSTHROUGH;
			mode_ctr++;
		}

		else if (!strcasecmp(arg, "no_discard_passdown"))
			cf->discard_passdown = false;

		else if (!strcasecmp(arg, "read-only")) {
			set_bit(FILTER_WRITE, &cf->filter);
			filter_ctr++;
		}

		else if (!strcasecmp(arg, "write-only")) {
			set_bit(FILTER_READ, &cf->filter);
			filter_ctr++;
		}

		else if (!strcasecmp(arg, "remap-only")) {
			set_bit(FILTER_READ, &cf->filter);
			set_bit(FILTER_WRITE, &cf->filter);
			filter_ctr++;
		}

		else if (!strcasecmp(arg, "read-write"))
			filter_ctr++;

		else if (!strcasecmp(arg, "need_cache_check"))
			cf->optional_cache = true;

		else if (!strcasecmp(arg, "discard_reclaim"))
			cf->discard_return_blocks = true;

		else {
			*error = "Unrecognised cache feature requested";
			return -EINVAL;
		}
	}

	if (mode_ctr > 1 || filter_ctr > 1) {
		*error = "Duplicate cache io_mode features requested";
		return -EINVAL;
	}

	if (test_bit(FILTER_WRITE, &(cf->filter)) && cf->io_mode != CM_IO_WRITEBACK) {
		*error = "Read-only cache mode could only be paired with writeback mode";
		return -EINVAL;
	}

	return 0;
}

static int parse_policy(struct client_args *ca, struct dm_arg_set *as,
			char **error)
{
	static const struct dm_arg _args[] = {
		{0, 1024, "Invalid number of policy arguments"},
	};

	int r;

	if (!at_least_one_arg(as, error))
		return -EINVAL;

	ca->policy_name = dm_shift_arg(as);

	r = dm_read_arg_group(_args, as, &ca->policy_argc, error);
	if (r)
		return -EINVAL;

	ca->policy_argv = (const char **)as->argv;
	dm_consume_args(as, ca->policy_argc);

	return 0;
}

static int parse_client_args(struct client_args *ca, int argc, char **argv,
			     char **error)
{
	int r;
	struct dm_arg_set as;

	as.argc = argc;
	as.argv = argv;

	r = parse_client_cache_dev(ca, &as, error);
	if (r)
		return r;

	r = parse_origin_dev(ca, &as, error);
	if (r)
		return r;

	r = parse_device_id(ca, &as, error);
	if (r)
		return r;

	r = parse_features(ca, &as, error);
	if (r)
		return r;

	r = parse_policy(ca, &as, error);
	if (r)
		return r;

	return 0;
}

static void destroy_client(struct cache_client *client)
{
	int i;
	char buf[BDEVNAME_SIZE];

	if (client->discard_bitset)
		free_bitset(client->discard_bitset);

	if (client->wq)
		destroy_workqueue(client->wq);

	if (client->origin_dev) {
		DMWARN("%s: put origin dev (%s)", __func__, bdevname(client->origin_dev->bdev, buf));
		dm_put_device(client->ti, client->origin_dev);
	}

	if (client->cache_dev) {
		DMWARN("%s: put cache dev (%s)", __func__, bdevname(client->cache_dev->bdev, buf));
		dm_put_device(client->ti, client->cache_dev);
	}

	if (client->policy)
		dm_cache_policy_destroy(client->policy);

	for (i = 0; i < client->nr_ctr_args ; i++)
		kfree(client->ctr_args[i]);

	kfree(client);
}

static void cache_client_get(struct cache_client *client)
{
	kobject_get(&client->holder->kobj);
	refcount_inc(&client->refcount);
}

static void cache_client_put(struct cache_client *client)
{
	kobject_put(&client->holder->kobj);
	if (refcount_dec_and_test(&client->refcount))
		complete(&client->can_destroy);
}

static void cache_client_dtr(struct dm_target *ti)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;

	abort_writeback(client);

	spin_lock_irq(&cache->lock);
	list_del_rcu(&client->list);
	spin_unlock_irq(&cache->lock);

	synchronize_rcu();

	cache_client_put(client);
	kobj_holder_drop(client->holder, cache);
	wait_for_completion(&client->can_destroy);

	mutex_lock(&dm_cache_table.mutex);

	__cache_dec(cache);
	dm_cache_client_metadata_close(client->ccmd);

	destroy_client(client);

	mutex_unlock(&dm_cache_table.mutex);
	DMWARN("%s: cache client removed", __func__);
}

static int create_client_policy(struct cache_client *client, struct client_args *ca,
			       char **error)
{
	struct cache *cache = client->cache;
	struct dm_cache_policy *p = dm_cache_policy_create(ca->policy_name,
							   client->origin_sectors,
							   cache->manager);
	if (IS_ERR_OR_NULL(p)) {
		*error = "Error creating cache's policy";
		return  p ? PTR_ERR(p) : -ENOMEM;
	}
	client->policy = p;

	return 0;
}

static int client_create(struct client_args *ca, struct cache_client **result)
{
	int r = 0;
	char **error = &ca->ti->error;
	struct cache_client *client;
	struct dm_target *ti = ca->ti;
	dm_block_t origin_blocks;
	struct dm_cache_client_metadata *ccmd;
	struct kobj_holder *holder;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	mutex_lock(&dm_cache_table.mutex); // FIXME

	client->ti = ca->ti;
	ti->private = client;
	ti->num_flush_bios = 2;
	ti->flush_supported = true;

	ti->num_discard_bios = 1;
	ti->discards_supported = true;

	ti->per_io_data_size = sizeof(struct per_bio_data);

	client->features = ca->features;
	client->client_md = dm_table_get_md(ti->table);

	client->origin_dev = ca->origin_dev;
	client->cache_dev = ca->cache_dev;

	client->dev_id = ca->dev_id;

	client->cache = __cache_table_lookup(ca->cache_md);
	if (!client->cache) {
		*error = "Counld not find cache object";
		r = -EINVAL;
		goto bad_val;
	}

	__cache_inc(client->cache);

	if (dm_set_target_max_io_len(ti, client->cache->sectors_per_block)) {
		r = -EINVAL;
		goto bad_args;
	}

	ca->origin_dev = ca->cache_dev = NULL;

	origin_blocks = client->origin_sectors = ca->origin_sectors;
	origin_blocks = block_div(origin_blocks, client->cache->sectors_per_block);
	client->origin_blocks = to_oblock(origin_blocks);

	r = create_client_policy(client, ca, error);
	if (r)
		goto bad_args;

	client->policy_nr_args = ca->policy_argc;

	r = set_config_values(client, ca->policy_argc, ca->policy_argv);
	if (r) {
		*error = "Error setting cache policy's config values";
		goto bad_args;
	}

	if (passthrough_mode(client))
		policy_allow_migrations(client->policy, false);

	client->discard_block_size =
		calculate_discard_block_size(client->cache->sectors_per_block,
					     client->origin_sectors);
	client->discard_nr_blocks = to_dblock(dm_sector_div_up(client->origin_sectors,
							       client->discard_block_size));
	client->discard_bitset = alloc_bitset(from_dblock(client->discard_nr_blocks));
	if (!client->discard_bitset) {
		*error = "Could not allocate discard bitset";
		r = -ENOMEM;
		goto bad_args;
	}

	client->cache_size = client->cache->cache_size;

	r = dm_cache_client_metadata_open(client->cache->cmd, client->dev_id, &ccmd);
	if (r) {
		*error = "Could not open client internal device";
		goto bad_args;
	}
	client->ccmd = ccmd;

	load_stats(client);

	atomic_set(&client->stats.demotion, 0);
	atomic_set(&client->stats.promotion, 0);
	atomic_set(&client->stats.copies_avoided, 0);
	atomic_set(&client->stats.cache_cell_clash, 0);
	atomic_set(&client->stats.discard_count, 0);
	atomic_set(&client->err_stats.ssd_errs, 0);

	client->opt.cache_all = 1;
	client->opt.low_dirty_thresh = 30;
	client->opt.high_dirty_thresh = 60;
	client->opt.bgm_throttle = BGM_THROTTLE_LOW;
	client->opt.size_thresh = 0;

	policy_set_config_value(client->policy, "low_dirty_thresh", "30");
	policy_set_config_value(client->policy, "high_dirty_thresh", "60");

	if (passthrough_mode(client)) {
		bool all_clean;

		r = dm_cache_client_metadata_all_clean(ccmd, &all_clean); // FIXME
		if (r) {
			*error = "dm_cache_client_metadata_all_clean() failed";
			goto bad_args;
		}

		if (!all_clean) {
			*error = "Cannot enter passthrough mode unless all blocks are clean";
			r = -EINVAL;
			goto bad_args;
		}

	}

	spin_lock_init(&client->lock);

	bio_list_init(&client->deferred_bios);

	INIT_LIST_HEAD(&client->wb_queue);
	INIT_WORK(&client->wb_worker, issue_writeback);
	INIT_DELAYED_WORK(&client->ticker, client_ticker);
	spin_lock_init(&client->wb_lock);

	client->wq = alloc_workqueue("dm-" DM_MSG_PREFIX_CLIENT, WQ_MEM_RECLAIM, 0);
	if (!client->wq) {
		*error = "could not create workqueue for metadata object";
		goto bad;
	}

	r = -ENOMEM;
	holder = kobj_find(client->cache, client->dev_id);
	if (holder) {
		r = kobject_move(&holder->kobj, dm_kobject(client->client_md));
		if (r) {
			*error = "Count not move kobject";
			goto bad;
		}

		holder->client = client;
		kobj_get(holder);
	} else {
		holder = init_kobj_holder(client, client->dev_id);
		if (!holder) {
			*error = "Could not create kobject";
			r = -ENOMEM;
			goto bad;
		}

		r = kobject_init_and_add(&holder->kobj, &dm_client_ktype,
								 dm_kobject(client->client_md), "%s", "cache_client");
		if (r) {
			*error = "Could not create kobject";
			goto bad;
		}
		kobj_store(client->cache, holder);
	}

	client->holder = holder;

	spin_lock_irq(&client->cache->lock);
	if (client->cache->suspended) {
		ti->error = "Unable to activate cache client device while cache is suspended";
		r = -EINVAL;
		spin_unlock_irq(&client->cache->lock);
		goto bad;
	}
	refcount_set(&client->refcount, 1);
	init_completion(&client->can_destroy);
	list_add_tail_rcu(&client->list, &client->cache->active_clients);
	spin_unlock_irq(&client->cache->lock);

	atomic_set(&client->nr_dirty, 0);

	client->loaded_mappings = false;
	client->loaded_discards = false;
	client->sized = false;

	client->opt.skip_seq_thresh = 0;
	seq_io_detect_init(client);

	atomic_set(&client->stats.demotion, 0);
	atomic_set(&client->stats.promotion, 0);
	atomic_set(&client->stats.copies_avoided, 0);
	atomic_set(&client->stats.cache_cell_clash, 0);
	atomic_set(&client->stats.discard_count, 0);
	init_policy_stats(client->policy);

	client->need_tick_bio = true;

	*result = client;

	mutex_unlock(&dm_cache_table.mutex);

	synchronize_rcu();

	init_rwsem(&client->background_work_lock);
	prevent_background_work(client);

	DMWARN("cache client created");
	return 0;

bad:
	/* required for __cache_dec */
	mutex_lock(&dm_cache_table.mutex);
	dm_cache_client_metadata_close(client->ccmd);
bad_args:
	__cache_dec(client->cache);
bad_val:
	mutex_unlock(&dm_cache_table.mutex);
	destroy_client(client);

	return r;
}

static int copy_client_ctr_args(struct cache_client *client, int argc,
				const char **argv)
{
	unsigned i;
	const char **copy;

	client->nr_ctr_args = argc;

	if (!argc)
		return 0;

	copy = kcalloc(argc, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	for (i = 0; i < argc; i++) {
		copy[i] = kstrdup(argv[i], GFP_KERNEL);
		if (!copy[i]) {
			while (i--)
				kfree(copy[i]);
			kfree(copy);
			return -ENOMEM;
		}
	}

	client->ctr_args = copy;

	return 0;
}

/*
 * Cache client parameters:
 * <cache dev> <dev_id> <origin dev>
 *
 * cache dev: the path to the cache
 * dev_id: the internal device identifier
 * origin dev: the path to the origin device
 */


static int cache_client_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	int r = -EINVAL;
	struct client_args *ca;
	struct cache_client *client = NULL;

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca) {
		ti->error = "Error allocating memory for cache client";
		return -ENOMEM;
	}
	ca->ti = ti;

	r = parse_client_args(ca, argc, argv, &ti->error);
	if (r)
		goto out;

	r = client_create(ca, &client);
	if (r)
		goto out;

	ti->private = client;
	r = copy_client_ctr_args(client, argc - 2, (const char **)argv + 2);
	if (r)
		destroy_client(client);
out:
	destroy_client_args(ca);
	return r;
}

static void cut_unaligned(struct bio *bio)
{
	sector_t head_remain, tail_remain;
	sector_t bi_sector = bio->bi_iter.bi_sector;
	sector_t end_sector = bio_end_sector(bio);

	if (sub_bio_optimisable(bio))
		return;

	head_remain = sector_div(bi_sector, SUB_BLOCK_SIZE);
	tail_remain = sector_div(end_sector, SUB_BLOCK_SIZE);

	if (bi_sector == end_sector)
		/* in one sub block */
		return;

	if (head_remain)
		dm_accept_partial_bio(bio, SUB_BLOCK_SIZE - head_remain);
	else if (tail_remain)
		dm_accept_partial_bio(bio, bio_sectors(bio) - tail_remain);
}

static void aligned_to_block_size(struct cache_client *client, struct bio *bio)
{
	sector_t bi_sector = bio->bi_iter.bi_sector;
	dm_oblock_t begin, end;
	struct cache *cache = client->cache;

	begin = get_bio_block(cache, bio);
	bio->bi_iter.bi_sector = bio_end_sector(bio) - 1;
	end = get_bio_block(cache, bio);
	bio->bi_iter.bi_sector = bi_sector;

	if (begin == end)
		return;

	dm_accept_partial_bio(bio, oblock_to_sector(cache, oblock_succ(begin)) - bio->bi_iter.bi_sector);
}

static int cache_client_map(struct dm_target *ti, struct bio *bio)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;
	int r;
	bool commit_needed;
	dm_oblock_t block = get_bio_block(cache, bio);
	struct per_bio_data *pb = get_per_bio_data(bio);

	init_per_bio_data(client, bio);
	atomic_inc(&in_io);

	if (client->requeue_mode) {
		bio->bi_status = BLK_STS_DM_REQUEUE;
		atomic_inc(&rq_io);
		atomic_dec(&in_io);
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	if (unlikely(from_oblock(block) >= from_oblock(client->origin_blocks))) {
		/*
		 * This can only occur if the io goes to a partial block at
		 * the end of the origin device.  We don't cache these.
		 * Just remap to the origin and carry on.
		 */
		remap_to_origin(client, bio);
		accounted_begin(cache, bio);
		return DM_MAPIO_REMAPPED;
	}

	if (discard_or_flush(bio)) {
		if (op_is_discard(bio->bi_opf))
			aligned_to_block_size(client, bio);
		defer_bio(client, bio);
		return DM_MAPIO_SUBMITTED;
	}

	cut_unaligned(bio);

	if (!bio_sectors(bio))
		atomic_inc(&zero_io);

	if (bio->bi_opf & REQ_FUA) {
		defer_bio(client, bio);
		return DM_MAPIO_SUBMITTED;
	}

	if (get_cache_mode(cache) >= CM_READ_ONLY) {
		bio_io_error(bio);
		atomic_dec(&in_io);
		return DM_MAPIO_SUBMITTED;;
	}

	pb->rw = 1;
	atomic_inc(&rw_io);
	r = map_bio(client, bio, block, &commit_needed);
	if (commit_needed)
		schedule_commit(&cache->committer); // FIXME

	return r;
}

static int cache_client_endio(struct dm_target *ti, struct bio *bio, blk_status_t *error)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;
	unsigned long flags;
	struct per_bio_data *pb = get_per_bio_data(bio);

	if (pb->tick) {
		policy_tick(client->policy, false);

		spin_lock_irqsave(&client->lock, flags);
		client->need_tick_bio = true;
		spin_unlock_irqrestore(&client->lock, flags);
	}

	if (pb->remap) {
		pb->remap = 0;
		if (pb->rw) {
			pb->rw = 0;
			atomic_dec(&rw_io);
		}
		atomic_dec(&in_io);
		atomic_dec(&remap_io);
	}

	bio_drop_shared_lock(client, bio);
	accounted_complete(cache, bio);

	return DM_ENDIO_DONE;
}

static void cache_client_presuspend(struct dm_target *ti)
{
	struct cache_client *client = ti->private;

	if (dm_noflush_suspending(ti))
		noflush_work(client, do_noflush_start);
}

static void cache_client_postsuspend(struct dm_target *ti)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;
	int r;

	/*
	 * The dm_noflush_suspending flag has been cleared by now, so
	 * unfortunately we must always run this.
	 */
	noflush_work(client, do_noflush_stop);

	cancel_delayed_work_sync(&client->ticker);
	prevent_background_work(client);

	policy_suspend(client->policy);

	if (get_cache_mode(cache) == CM_WRITE) {
		r = sync_client_metadata(client);
		sync_cache_metadata(cache, r);
	}
}

static int cache_client_preresume(struct dm_target *ti)
{
	int r = 0;
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;

	/*
	 * Check to see if the cache has resized.
	 */

	if (!client->loaded_mappings) {
		r = dm_cache_client_load_mappings(client->ccmd, client->policy,
					   load_mapping, client);
		if (r) {
			DMERR("%s: could not load cache mappings", client_device_name(client));
			metadata_operation_failed(cache, "dm_cache_client_load_mappings", r);
			return r;
		}

		policy_sort_mapping(client->policy);
		client->loaded_mappings = true;
	}

	if (!client->loaded_discards) {
		struct bitset_load_info li;

		/*
		 * The discard bitset could have been resized, or the
		 * discard block size changed.  To be safe we start by
		 * setting every dblock to not discarded.
		 */
		clear_bitset(client->discard_bitset, from_dblock(client->discard_nr_blocks));

		bitset_load_info_init((void *)client,
		                      from_dblock(client->discard_nr_blocks),
		                      client->discard_block_size, set_discard, &li);
		r = dm_cache_client_load_discards(client->ccmd, load_bitset, &li);
		if (r) {
			DMERR("%s: could not load origin discards", cache_device_name(cache));
			metadata_operation_failed(cache, "dm_cache_client_load_discards", r);
			return r;
		}
		set_bitset_range(&li);

		client->loaded_discards = true;
	}

	r = policy_resume(client->policy);
	if (r) {
		DMERR("%s: policy resume failed", client_device_name(client));
		return r;
	}

	refcount_set(&client->nr_meta_updates, 1);
	allow_background_work(client);
	do_waker(&cache->waker.work);
	client_ticker(&client->ticker.work);
	return 0;
}

static void emit_client_flags(struct cache_client *client, char *result,
			      unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;
	struct client_features *cf = &client->features;
	unsigned count = !cf->discard_passdown + cf->optional_cache +
			 cf->discard_return_blocks  + 2;

	DMEMIT("%u ", count);

	if (writethrough_mode(client))
		DMEMIT("writethrough ");

	else if (passthrough_mode(client))
		DMEMIT("passthrough ");

	else if (writeback_mode(client))
		DMEMIT("writeback ");

	else {
		DMEMIT("unknown ");
		DMERR("%s: internal error: unknown io mode: %d",
		      client_device_name(client), (int) cf->io_mode);
	}

	if (!cf->discard_passdown)
		DMEMIT("no_discard_passdown ");

	if (test_bit(READ, &(client->features.filter)) &&
	    test_bit(WRITE, &(client->features.filter)))
		DMEMIT("remap-only ");
	else if (test_bit(READ, &(client->features.filter)))
		DMEMIT("write-only ");
	else if (test_bit(WRITE, &(client->features.filter)))
		DMEMIT("read-only ");
	else
		DMEMIT("read-write ");

	if (cf->optional_cache)
		DMEMIT("need_cache_check ");

	if (cf->discard_return_blocks)
		DMEMIT("discard_reclaim ");

	*sz_ptr = sz;
}

static void cache_client_status(struct dm_target *ti, status_type_t type,
				unsigned status_flags, char *result, unsigned maxlen)
{
	int r = 0;
	ssize_t sz = 0;
	char buf[BDEVNAME_SIZE];
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;
	dm_cblock_t residency;
	int i;

	switch (type) {
	case STATUSTYPE_INFO:
		if (get_cache_mode(cache) == CM_FAIL) {
			DMEMIT("Fail");
			break;
		}

		/* Commit to ensure statistics aren't out-of-date */
		if (!(status_flags & DM_STATUS_NOFLUSH_FLAG) && !dm_suspended(ti))
			(void) commit(cache, false);

		residency = policy_residency(client->policy);

		DMEMIT("%llu/%llu %u %u %u %u %u %u %u %u %u ",
		       (unsigned long long) from_cblock(residency),
		       (unsigned long long) from_cblock(cache->cache_size),
		       (unsigned) atomic_read(&client->stats.read_hit),
		       (unsigned) atomic_read(&client->stats.read_miss),
		       (unsigned) atomic_read(&client->stats.write_hit),
		       (unsigned) atomic_read(&client->stats.write_miss),
		       (unsigned) atomic_read(&client->err_stats.disk_errs),
		       (unsigned) atomic_read(&client->err_stats.ssd_errs),
		       (unsigned) atomic_read(&client->stats.demotion),
		       (unsigned) atomic_read(&client->stats.promotion),
		       (unsigned) atomic_read(&client->nr_dirty));

		emit_client_flags(client, result, maxlen, &sz);

		DMEMIT("2 migration_threshold %llu ", (unsigned long long) cache->migration_threshold);

		DMEMIT("%s ", dm_cache_policy_get_name(client->policy));
		if (sz < maxlen) {
			r = policy_emit_config_values(client->policy, result, maxlen, &sz);
			if (r)
				DMERR("%s: policy_emit_config_values returned %d",
				      client_device_name(client), r);
		}

		if (sz < maxlen) {
			r = policy_show_stats(client->policy, result, maxlen, &sz);
			if (r)
				DMERR("%s: policy_show_stats returned %d",
				      cache_device_name(cache), r);
		}

		break;

	case STATUSTYPE_TABLE:
		format_dev_t(buf, client->cache_dev->bdev->bd_dev);
		DMEMIT("%s ", buf);
		format_dev_t(buf, client->origin_dev->bdev->bd_dev);
		DMEMIT("%s", buf);

		if (client->nr_ctr_args > 1)
			for (i = 0; i < client->nr_ctr_args - 1; i++)
				DMEMIT(" %s", client->ctr_args[i]);

		if (client->nr_ctr_args)
			DMEMIT(" %s", client->ctr_args[client->nr_ctr_args - 1]);
	}

	return;
}

static int cache_client_iterate_devices(struct dm_target *ti,
					iterate_devices_callout_fn fn, void *data)
{
	int r = 0;
	struct cache_client *client = ti->private;
	dm_cblock_t size;

	if (!client)
		DMERR("%s client is null", __func__);
	if (!client->origin_dev)
		DMERR("%s client->origin_dev is null", __func__);

	size = get_dev_size(client->origin_dev);

	r = fn(ti, client->origin_dev, 0, get_dev_size(client->origin_dev), data);

	return r;
}

static void cache_client_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;
	uint64_t io_opt_sectors = limits->io_opt >> SECTOR_SHIFT;

	/*
	 * If the system-determined stacked limits are compatible with the
	 * cache's blocksize (io_opt is a factor) do not override them.
	 */
	if (io_opt_sectors < cache->sectors_per_block ||
	    do_div(io_opt_sectors, cache->sectors_per_block)) {
		blk_limits_io_min(limits, cache->sectors_per_block << SECTOR_SHIFT);
		blk_limits_io_opt(limits, cache->sectors_per_block << SECTOR_SHIFT);
	}

	disable_passdown_if_not_supported(client);
	set_discard_limits(client, limits);
}

static int process_client_data_cache_message(struct cache_client *client, unsigned argc,
					     const char **argv)
{
	unsigned long flags;
	int r = 0;

	if (argc != 1)
		return -EINVAL;

	spin_lock_irqsave(&client->lock, flags);
	if (!strcasecmp(argv[0], "on"))
		client->features.optional_cache = true;
	else if (!strcasecmp(argv[0], "off"))
		client->features.optional_cache = false;
	else
		r = -EINVAL;
	spin_unlock_irqrestore(&client->lock, flags);

	return r;
}

static int process_client_discard_reclaim_message(struct cache_client *client, unsigned argc,
						  const char **argv)
{
	unsigned long flags;
	int r = 0;

	if (argc != 1)
		return -EINVAL;

	spin_lock_irqsave(&client->lock, flags);
	if (!strcasecmp(argv[0], "on")) {
		client->features.discard_return_blocks = true;
		DMINFO("%s: discard_reclaim on", client_device_name(client));
	} else if (!strcasecmp(argv[0], "off")) {
		client->features.discard_return_blocks = false;
		DMINFO("%s: discard_reclaim off", client_device_name(client));
	} else {
		r = -EINVAL;
	}
	spin_unlock_irqrestore(&client->lock, flags);

	return r;
}

static int process_client_policy_reset_message(struct cache_client *client, unsigned argc,
						const char **argv)
{
	unsigned long flags;
	int r = 0;

	if (argc != 1)
		return -EINVAL;

	spin_lock_irqsave(&client->lock, flags);
	if (!strcasecmp(argv[0], "yes")) {
		policy_shuffle(client->policy);
		DMINFO("%s: policy reset", client_device_name(client));
	}
	spin_unlock_irqrestore(&client->lock, flags);

	return r;
}

/*
 * Supports
 *	"<key> <value>"
 * and
 *     "invalidate_cblocks [(<begin>)|(<begin>-<end>)]*
 *
 * The key migration_threshold is supported by the cache target core.
 */
static int cache_client_message(struct dm_target *ti, unsigned argc, char **argv,
			 char *result, unsigned maxlen)
{
	struct cache_client *client = ti->private;
	struct cache *cache = client->cache;

	if (!argc)
		return -EINVAL;

	if (get_cache_mode(cache) >= CM_READ_ONLY) {
		DMERR("%s: unable to service cache client target messages in cache READ_ONLY or FAIL mode",
		      client_device_name(client));
		return -EOPNOTSUPP;
	}

	if (!strcasecmp(argv[0], "data_cache"))
		return process_client_data_cache_message(client, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "discard_reclaim"))
		return process_client_discard_reclaim_message(client, argc - 1, (const char **) argv + 1);

	if (!strcasecmp(argv[0], "policy_reset"))
		return process_client_policy_reset_message(client, argc - 1, (const char **) argv + 1);

	if (argc != 2)
		return -EINVAL;

	return set_policy_config_value(client, argv[0], argv[1]);
}

static struct target_type cache_client_target = {
	.name = "cache_client",
	.version = {2, 2, 0},
	.module = THIS_MODULE,
	.ctr = cache_client_ctr,
	.dtr = cache_client_dtr,
	.map = cache_client_map,
	.end_io = cache_client_endio,
	.presuspend = cache_client_presuspend,
	.postsuspend = cache_client_postsuspend,
	.preresume = cache_client_preresume,
	.status = cache_client_status,
	.iterate_devices = cache_client_iterate_devices,
	.io_hints = cache_client_io_hints,
	.message = cache_client_message,
};

static int __init dm_cache_init(void)
{
	int r;

	cache_table_init();

	migration_cache = KMEM_CACHE(dm_cache_migration, 0);
	if (!migration_cache)
		return -ENOMEM;

	r = dm_register_target(&cache_target);
	if (r) {
		DMERR("cache target registration failed: %d", r);
		goto cache_failed;
	}

	r = dm_register_target(&cache_client_target);
	if (r) {
		DMERR("cache client target registration failed: %d", r);
		goto client_failed;
	}

	atomic_set(&mg_io, 0);
	atomic_set(&mg2_io, 0);
	atomic_set(&mg3_io, 0);
	atomic_set(&rw_io, 0);
	atomic_set(&zero_io, 0);
	atomic_set(&h_io, 0);
	atomic_set(&remap_io, 0);
	atomic_set(&in_io, 0);
	atomic_set(&rq_io, 0);
	atomic_set(&lock_cell, 0);
	atomic_set(&lock2_cell, 0);
	atomic_set(&nounlock, 0);

	return 0;

client_failed:
	dm_unregister_target(&cache_target);
cache_failed:
	kmem_cache_destroy(migration_cache);

	return r;
}

static void __exit dm_cache_exit(void)
{
	dm_unregister_target(&cache_client_target);
	dm_unregister_target(&cache_target);
	kmem_cache_destroy(migration_cache);

	cache_table_exit();
}

module_init(dm_cache_init);
module_exit(dm_cache_exit);

MODULE_DESCRIPTION(DM_NAME " cache target");
MODULE_AUTHOR("Joe Thornber <ejt@redhat.com>");
MODULE_LICENSE("GPL");
