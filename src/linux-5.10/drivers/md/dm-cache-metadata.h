/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is released under the GPL.
 */

#ifndef DM_CACHE_METADATA_H
#define DM_CACHE_METADATA_H

#include "dm-cache-block-types.h"
#include "dm-cache-policy-internal.h"
#include "persistent-data/dm-space-map-metadata.h"

/*----------------------------------------------------------------*/



#define DM_CACHE_METADATA_BLOCK_SIZE (4096 >> SECTOR_SHIFT)

/* FIXME: remove this restriction */
/*
 * The metadata device is currently limited in size.
 */
#define DM_CACHE_METADATA_MAX_SECTORS \
	(255 * (1 << 14) * (DM_CACHE_METADATA_BLOCK_SIZE / (1 << SECTOR_SHIFT)))

/*
 * A metadata device larger than 16GB triggers a warning.
 */
#define DM_CACHE_METADATA_MAX_SECTORS_WARNING (16 * (1024 * 1024 * 1024 >> SECTOR_SHIFT))

/*----------------------------------------------------------------*/

/*
 * Ext[234]-style compat feature flags.
 *
 * A new feature which old metadata will still be compatible with should
 * define a DM_CACHE_FEATURE_COMPAT_* flag (rarely useful).
 *
 * A new feature that is not compatible with old code should define a
 * DM_CACHE_FEATURE_INCOMPAT_* flag and guard the relevant code with
 * that flag.
 *
 * A new feature that is not compatible with old code accessing the
 * metadata RDWR should define a DM_CACHE_FEATURE_RO_COMPAT_* flag and
 * guard the relevant code with that flag.
 *
 * As these various flags are defined they should be added to the
 * following masks.
 */

#define DM_CACHE_FEATURE_COMPAT_SUPP	  0UL
#define DM_CACHE_FEATURE_COMPAT_RO_SUPP	  0UL
#define DM_CACHE_FEATURE_INCOMPAT_SUPP	  0UL

struct dm_cache_metadata;
struct dm_cache_client_metadata;

/*
 * Reopens or creates a new, empty metadata volume.  Returns an ERR_PTR on
 * failure.  If reopening then features must match.
 */
struct dm_cache_metadata *dm_cache_metadata_open(struct block_device *bdev,
						 sector_t data_block_size,
						 sector_t sub_block_size,
						 bool may_format_device,
						 size_t policy_hint_size,
						 unsigned metadata_version);

void dm_cache_metadata_close(struct dm_cache_metadata *cmd);

int dm_cache_client_metadata_open(struct dm_cache_metadata *cmd, uint64_t dev_id,
				  struct dm_cache_client_metadata **ccmd);
int dm_cache_client_metadata_close(struct dm_cache_client_metadata *ccmd);

int dm_cache_create_client(struct dm_cache_metadata *cmd, uint64_t dev_id);
int dm_cache_delete_client(struct dm_cache_metadata *cmd, uint64_t dev_id);

uint64_t dm_cache_client_dev_id(struct dm_cache_client_metadata *ccmd);
/*
 * The metadata needs to know how many cache blocks there are.  We don't
 * care about the origin, assuming the core target is giving us valid
 * origin blocks to map to.
 */
int dm_cache_resize(struct dm_cache_metadata *cmd, dm_cblock_t new_cache_size);
int dm_cache_client_resize(struct dm_cache_client_metadata *ccmd, dm_cblock_t new_cache_size);
int dm_cache_size(struct dm_cache_metadata *cmd, dm_cblock_t *result);

int dm_cache_client_discard_bitset_resize(struct dm_cache_client_metadata *ccmd,
				          sector_t discard_block_size,
				          dm_dblock_t new_nr_entries);

typedef int (*load_bitset_fn)(void *context, sector_t bit_block_size,
                              uint32_t bit, bool set);
int dm_cache_load_sub_valids(struct dm_cache_metadata *cmd,
			     load_bitset_fn fn, void *context);
int dm_cache_load_sub_dirties(struct dm_cache_metadata *cmd,
			      load_bitset_fn fn, void *context, bool *skipped);
int dm_cache_changed_this_transaction(struct dm_cache_metadata *cmd);


int dm_cache_client_load_discards(struct dm_cache_client_metadata *ccmd,
			   load_bitset_fn fn, void *context);
int dm_cache_client_set_discard(struct dm_cache_client_metadata *ccmd, dm_dblock_t dblock, bool discard);

int dm_cache_client_remove_mapping(struct dm_cache_client_metadata *ccmd, dm_cblock_t cblock);
int dm_cache_client_insert_mapping(struct dm_cache_client_metadata *ccmd, dm_cblock_t cblock, dm_oblock_t oblock);

typedef int (*recycle_fn)(void *context, dm_cblock_t b);
int dm_cache_recycle_clients(struct dm_cache_metadata *cmd,
                             recycle_fn fn, void *context);

typedef int (*load_mapping_fn)(void *context, dm_oblock_t oblock,
			       dm_cblock_t cblock, bool dirty,
			       uint32_t hint, bool hint_valid);

typedef int (*walk_mapping_fn)(void *context, dm_cblock_t cblock);
int dm_cache_client_load_mappings(struct dm_cache_client_metadata *ccmd,
                                  struct dm_cache_policy *policy,
                                  load_mapping_fn fn,
                                  void *context);

int dm_cache_walk(struct dm_cache_metadata *cmd, walk_mapping_fn fn, void *context);

int dm_cache_set_dirty_bits(struct dm_cache_metadata *cmd,
			    unsigned nr_bits, unsigned long *bits);

int dm_cache_set_sub_dirty_bits(struct dm_cache_metadata *cmd,
			        unsigned nr_bits, unsigned long *bits);

int dm_cache_set_sub_valid_bits(struct dm_cache_metadata *cmd,
                                uint32_t index,
                                unsigned nr_bits,
                                unsigned long *bits,
                                bool invalid);

struct dm_cache_statistics {
	uint32_t read_hits;
	uint32_t read_misses;
	uint32_t write_hits;
	uint32_t write_misses;
};

void dm_cache_client_metadata_get_stats(struct dm_cache_client_metadata *md,
					struct dm_cache_statistics *stats);

	/*
 * 'void' because it's no big deal if it fails.
 */
void dm_cache_client_metadata_set_stats(struct dm_cache_client_metadata *md,
				        struct dm_cache_statistics *stats);

int dm_cache_commit(struct dm_cache_metadata *cmd, bool clean_shutdown);

int dm_cache_get_free_metadata_block_count(struct dm_cache_metadata *cmd,
					   dm_block_t *result);

int dm_cache_get_metadata_dev_size(struct dm_cache_metadata *cmd,
				   dm_block_t *result);

/*
 * The policy is invited to save a 32bit hint value for every cblock (eg,
 * for a hit count).  These are stored against the policy name.  If
 * policies are changed, then hints will be lost.  If the machine crashes,
 * hints will be lost.
 *
 * The hints are indexed by the cblock, but many policies will not
 * neccessarily have a fast way of accessing efficiently via cblock.  So
 * rather than querying the policy for each cblock, we let it walk its data
 * structures and fill in the hints in whatever order it wishes.
 */
int dm_cache_client_write_hints(struct dm_cache_client_metadata *ccmd, struct dm_cache_policy *p);

/*
 * Query method.  Are all the blocks in the cache clean?
 */
int dm_cache_metadata_all_clean(struct dm_cache_metadata *cmd, bool *result);
int dm_cache_client_metadata_all_clean(struct dm_cache_client_metadata *ccmd, bool *result);

int dm_cache_metadata_needs_check(struct dm_cache_metadata *cmd, bool *result);
int dm_cache_metadata_set_needs_check(struct dm_cache_metadata *cmd);
void dm_cache_metadata_set_read_only(struct dm_cache_metadata *cmd);
void dm_cache_metadata_set_read_write(struct dm_cache_metadata *cmd);
int dm_cache_metadata_abort(struct dm_cache_metadata *cmd);

/*----------------------------------------------------------------*/

#endif /* DM_CACHE_METADATA_H */
