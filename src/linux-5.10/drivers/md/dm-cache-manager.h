/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#ifndef DM_CACHE_MANAGER_H
#define DM_CACHE_MANAGER_H

#include "dm-cache-block-types.h"
#include "dm-cache-policy-util.h"
#include <linux/device-mapper.h>

/*----------------------------------------------------------------*/

struct cache_feedback {
	int escalate:1;
	int shrink_required:1;
};

enum water_level {
	LOW,		/* 20% */
	MEDIUM,		/* 50% */
	MED_HIGH,	/* 70% */
	HIGH,		/* 90% */
};

struct cache_feedback_channel {
	int (*feedback)(void *, struct cache_feedback *);
	void (*shrink)(void *, enum water_level, int nr_neighbors);

	struct list_head list;
	void *private;
};

struct cache_manager {
	/*
	 * Destroys this object.
	 */
	void (*destroy)(struct cache_manager *m);

	struct entry *(*alloc_particular_entry)(struct cache_manager *m, unsigned index);
	struct entry *(*alloc_entry)(struct cache_manager *m);
	void (*take_particular_entry)(struct cache_manager *m, unsigned index);
	bool (*entry_taken)(struct cache_manager *m, unsigned index);

	void (*free_entry)(struct cache_manager *m, struct entry *e);

	struct entry *(*to_entry)(struct cache_manager *m, unsigned index);
	unsigned (*to_index)(struct cache_manager *m, struct entry *e);

	int (*register_feedback)(struct cache_manager *m, struct cache_feedback_channel *ch);
	void (*unregister_feedback)(struct cache_manager *m, struct cache_feedback_channel *ch);

	dm_cblock_t (*nr_allocated)(struct cache_manager *m);
	dm_cblock_t (*nr_blocks)(struct cache_manager *m);
	sector_t (*block_size)(struct cache_manager *m);

	int (*resize)(struct cache_manager *m, dm_cblock_t expand_size);

	void *private;
};

/*----------------------------------------------------------------*/

struct entry *cmgr_alloc_particular_entry(struct cache_manager *m, unsigned i);
struct entry *cmgr_alloc_entry(struct cache_manager *m);
bool cmgr_entry_taken(struct cache_manager *m, unsigned i);
void cmgr_take_particular_entry(struct cache_manager *m, unsigned i);
void cmgr_free_entry(struct cache_manager *m, struct entry *e);
struct entry *cmgr_to_entry(struct cache_manager *m, unsigned index);
unsigned cmgr_to_index(struct cache_manager *m, struct entry *e);
int cmgr_register_feedback(struct cache_manager *m, struct cache_feedback_channel *ch);
void cmgr_unregister_feedback(struct cache_manager *m, struct cache_feedback_channel *ch);
dm_cblock_t cmgr_nr_allocated(struct cache_manager *m);
dm_cblock_t cmgr_nr_blocks(struct cache_manager *m);
sector_t cmgr_block_size(struct cache_manager *m);
void cmgr_tick(struct cache_manager *m);
int cmgr_resize(struct cache_manager *m, dm_cblock_t expand_size);
void cmgr_destroy(struct cache_manager *m);

struct cache_manager *cmgr_create(dm_cblock_t cache_size, sector_t cache_block_size);
void cmgr_indexer_init(struct entry_indexer *i, struct cache_manager *m);

#endif	/* DM_CACHE_MANAGER_H */
