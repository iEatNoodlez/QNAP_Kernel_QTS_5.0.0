/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-metadata.h"

#include "persistent-data/dm-array.h"
#include "persistent-data/dm-bitset.h"
#include "persistent-data/dm-space-map.h"
#include "persistent-data/dm-space-map-disk.h"
#include "persistent-data/dm-transaction-manager.h"

#include <linux/device-mapper.h>
#include <linux/refcount.h>

/*----------------------------------------------------------------*/

#define DM_MSG_PREFIX   "cache metadata"

#define CACHE_SUPERBLOCK_MAGIC 06142003
#define CACHE_SUPERBLOCK_LOCATION 0

/*
 * defines a range of metadata versions that this module can handle.
 */
#define MIN_CACHE_VERSION 2
#define MAX_CACHE_VERSION 2

/*
 *  3 for btree insert +
 *  2 for btree lookup used within space map
 */
#define CACHE_MAX_CONCURRENT_LOCKS 6
#define SPACE_MAP_ROOT_SIZE 128

enum superblock_flag_bits {
	/* for spotting crashes that would invalidate the dirty bitset */
	CLEAN_SHUTDOWN,
	/* metadata must be checked using the tools */
	NEEDS_CHECK,
};

/*
 * Each mapping from cache block -> origin block carries a set of flags.
 */
enum mapping_bits {
	/*
	 * A valid mapping.  Because we're using an array we clear this
	 * flag for an non existant mapping.
	 */
	M_VALID = 1,
};

struct cache_disk_superblock {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__u8 uuid[16];
	__le64 magic;
	__le32 version;

	__le32 policy_hint_size;

	__u8 metadata_space_map_root[SPACE_MAP_ROOT_SIZE];

	__le64 clients_root;
	__le64 mapping_root;
	__le64 hint_root;

	__le32 data_block_size;
	__le32 metadata_block_size;
	__le32 cache_blocks;

	__le32 compat_flags;
	__le32 compat_ro_flags;
	__le32 incompat_flags;

	/*
	 * Metadata format 2 fields.
	 */
	__le64 dirty_root;

	/*
	 * Metadata support partial update in cblock
	 */
	__le64 sub_dirty_root;
	__le64 sub_valid_root;
	__le32 sub_block_size;
} __packed;

struct client_disk_superblock {
	__le64 discard_root;
	__le64 discard_block_size;
	__le64 discard_nr_blocks;

	__le32 read_hits;
	__le32 read_misses;
	__le32 write_hits;
	__le32 write_misses;

	__u8 policy_name[CACHE_POLICY_NAME_SIZE];
	__le32 policy_version[CACHE_POLICY_VERSION_SIZE];
} __packed;

struct dm_cache_client_metadata {
	struct list_head list;

	uint64_t dev_id;
	struct dm_cache_metadata *cmd;

	struct dm_block_manager *bm;
	refcount_t ref_count;

	union {
		dm_block_t discard_root;
		dm_block_t mapping_root;
	};
	struct dm_disk_bitset discard_info;

	struct dm_cache_statistics stats;
	dm_cblock_t cache_blocks;

	sector_t discard_block_size;
	dm_dblock_t discard_nr_blocks;

	char policy_name[CACHE_POLICY_NAME_SIZE];
	unsigned policy_version[CACHE_POLICY_VERSION_SIZE];

	int open_count;
	bool changed:1;
	bool aborted_with_changes:1;

	/*
	 * Set if a transaction has to be aborted but the attempt to roll
	 * back to the previous (good) transaction failed.  The only
	 * metadata operation permissible in this state is the closing of
	 * the device.
	 */
	bool fail_io:1;
};

struct dm_cache_metadata {
	refcount_t ref_count;
	struct list_head list;

	unsigned version;
	struct block_device *bdev;
	struct dm_block_manager *bm;
	struct dm_space_map *metadata_sm;
	struct dm_transaction_manager *tm;

	struct rw_semaphore root_lock;
	unsigned long flags;
	
	dm_block_t root;
	struct dm_btree_info info;
	struct dm_btree_info tl_info;
	struct dm_btree_info bl_info;

	dm_block_t clients_root;
	struct dm_btree_info clients_info;

	dm_block_t hint_root;
	struct dm_array_info hint_info;

	size_t policy_hint_size;
	sector_t sub_block_size;
	sector_t data_block_size;
	dm_cblock_t cache_blocks;
	bool changed:1;
	bool clean_when_opened:1;

	/*
	 * Reading the space map root can fail, so we read it into this
	 * buffer before the superblock is locked and updated.
	 */
	__u8 metadata_space_map_root[SPACE_MAP_ROOT_SIZE];

	/*
	 * Set if a transaction has to be aborted but the attempt to roll
	 * back to the previous (good) transaction failed.  The only
	 * metadata operation permissible in this state is the closing of
	 * the device.
	 */
	bool fail_io:1;

	/*
	 * Metadata format 2 fields.
	 */
	dm_block_t dirty_root;
	struct dm_disk_bitset dirty_info;

	/*
	 * Metadata support partial update in cblock
	 */
	dm_block_t sub_dirty_root;
	struct dm_disk_bitset sub_dirty_info;

	dm_block_t sub_valid_root;
	struct dm_disk_bitset sub_valid_info;

	/*
	 * These structures are used when loading metadata.  They're too
	 * big to put on the stack.
	 */
	struct dm_array_cursor hint_cursor;
	struct dm_bitset_cursor dirty_cursor;

	struct list_head clients;
	struct list_head recycle_clients;
};

struct dm_cache_client_device {
};

/*-------------------------------------------------------------------
 * superblock validator
 *-----------------------------------------------------------------*/

#define SUPERBLOCK_CSUM_XOR 9031977

static void sb_prepare_for_write(struct dm_block_validator *v,
				 struct dm_block *b,
				 size_t sb_block_size)
{
	struct cache_disk_superblock *disk_super = dm_block_data(b);

	disk_super->blocknr = cpu_to_le64(dm_block_location(b));
	disk_super->csum = cpu_to_le32(dm_bm_checksum(&disk_super->flags,
						      sb_block_size - sizeof(__le32),
						      SUPERBLOCK_CSUM_XOR));
}

static int check_metadata_version(struct cache_disk_superblock *disk_super)
{
	uint32_t metadata_version = le32_to_cpu(disk_super->version);

	if (metadata_version < MIN_CACHE_VERSION || metadata_version > MAX_CACHE_VERSION) {
		DMERR("Cache metadata version %u found, but only versions between %u and %u supported.",
		      metadata_version, MIN_CACHE_VERSION, MAX_CACHE_VERSION);
		return -EINVAL;
	}

	return 0;
}

static int sb_check(struct dm_block_validator *v,
		    struct dm_block *b,
		    size_t sb_block_size)
{
	struct cache_disk_superblock *disk_super = dm_block_data(b);
	__le32 csum_le;

	if (dm_block_location(b) != le64_to_cpu(disk_super->blocknr)) {
		DMERR("sb_check failed: blocknr %llu: wanted %llu",
		      le64_to_cpu(disk_super->blocknr),
		      (unsigned long long)dm_block_location(b));
		return -ENOTBLK;
	}

	if (le64_to_cpu(disk_super->magic) != CACHE_SUPERBLOCK_MAGIC) {
		DMERR("sb_check failed: magic %llu: wanted %llu",
		      le64_to_cpu(disk_super->magic),
		      (unsigned long long)CACHE_SUPERBLOCK_MAGIC);
		return -EILSEQ;
	}

	csum_le = cpu_to_le32(dm_bm_checksum(&disk_super->flags,
					     sb_block_size - sizeof(__le32),
					     SUPERBLOCK_CSUM_XOR));
	if (csum_le != disk_super->csum) {
		DMERR("sb_check failed: csum %u: wanted %u",
		      le32_to_cpu(csum_le), le32_to_cpu(disk_super->csum));
		return -EILSEQ;
	}

	return check_metadata_version(disk_super);
}

static struct dm_block_validator sb_validator = {
	.name = "superblock",
	.prepare_for_write = sb_prepare_for_write,
	.check = sb_check
};

/*----------------------------------------------------------------*/

static int superblock_read_lock(struct dm_cache_metadata *cmd,
				struct dm_block **sblock)
{
	return dm_bm_read_lock(cmd->bm, CACHE_SUPERBLOCK_LOCATION,
			       &sb_validator, sblock);
}

static int superblock_lock_zero(struct dm_cache_metadata *cmd,
				struct dm_block **sblock)
{
	return dm_bm_write_lock_zero(cmd->bm, CACHE_SUPERBLOCK_LOCATION,
				     &sb_validator, sblock);
}

static int superblock_lock(struct dm_cache_metadata *cmd,
			   struct dm_block **sblock)
{
	return dm_bm_write_lock(cmd->bm, CACHE_SUPERBLOCK_LOCATION,
				&sb_validator, sblock);
}

/*----------------------------------------------------------------*/

static void __setup_mapping_details(struct dm_cache_metadata *cmd)
{
	if (cmd->policy_hint_size) {
		struct dm_btree_value_type vt;

		vt.context = NULL;
		vt.size = sizeof(__le32);
		vt.inc = NULL;
		vt.dec = NULL;
		vt.equal = NULL;
		dm_array_info_init(&cmd->hint_info, cmd->tm, &vt);
	}

	cmd->info.tm = cmd->tm;
	cmd->info.levels = 2;
	cmd->info.value_type.context = NULL;
	cmd->info.value_type.size = sizeof(__le64);
	cmd->info.value_type.inc = NULL;
	cmd->info.value_type.dec = NULL;
	cmd->info.value_type.equal = NULL;

	cmd->tl_info.tm = cmd->tm;
	cmd->tl_info.levels = 1;
	cmd->tl_info.value_type.context = NULL;
	cmd->tl_info.value_type.size = sizeof(__le64);
	cmd->tl_info.value_type.inc = NULL;
	cmd->tl_info.value_type.dec = NULL;
	cmd->tl_info.value_type.equal = NULL;

	cmd->bl_info.tm = cmd->tm;
	cmd->bl_info.levels = 1;
	cmd->bl_info.value_type.context = NULL;
	cmd->bl_info.value_type.size = sizeof(__le64);
	cmd->bl_info.value_type.inc = NULL;
	cmd->bl_info.value_type.dec = NULL;
	cmd->bl_info.value_type.equal = NULL;	

	cmd->clients_info.tm = cmd->tm;
	cmd->clients_info.levels = 1;
	cmd->clients_info.value_type.context = NULL;
	cmd->clients_info.value_type.size = sizeof(struct client_disk_superblock);
	cmd->clients_info.value_type.inc = NULL;
	cmd->clients_info.value_type.dec = NULL;
	cmd->clients_info.value_type.equal = NULL;
}

static int __superblock_all_zeroes(struct dm_block_manager *bm, bool *result)
{
	int r;
	unsigned i;
	struct dm_block *b;
	__le64 *data_le, zero = cpu_to_le64(0);
	unsigned sb_block_size = dm_bm_block_size(bm) / sizeof(__le64);

	/*
	 * We can't use a validator here - it may be all zeroes.
	 */
	r = dm_bm_read_lock(bm, CACHE_SUPERBLOCK_LOCATION, NULL, &b);
	if (r)
		return r;

	data_le = dm_block_data(b);
	*result = true;
	for (i = 0; i < sb_block_size; i++) {
		if (data_le[i] != zero) {
			*result = false;
			break;
		}
	}

	dm_bm_unlock(b);

	return 0;
}

static int __save_sm_root(struct dm_cache_metadata *cmd)
{
	int r;
	size_t metadata_len;

	r = dm_sm_root_size(cmd->metadata_sm, &metadata_len);
	if (r < 0)
		return r;

	return dm_sm_copy_root(cmd->metadata_sm, &cmd->metadata_space_map_root,
			       metadata_len);
}

static void __copy_sm_root(struct dm_cache_metadata *cmd,
			   struct cache_disk_superblock *disk_super)
{
	memcpy(&disk_super->metadata_space_map_root,
	       &cmd->metadata_space_map_root,
	       sizeof(cmd->metadata_space_map_root));
}

static int __write_initial_superblock(struct dm_cache_metadata *cmd)
{
	int r;
	struct dm_block *sblock;
	struct cache_disk_superblock *disk_super;
	sector_t bdev_size = i_size_read(cmd->bdev->bd_inode) >> SECTOR_SHIFT;

	/* FIXME: see if we can lose the max sectors limit */
	if (bdev_size > DM_CACHE_METADATA_MAX_SECTORS)
		bdev_size = DM_CACHE_METADATA_MAX_SECTORS;

	r = dm_tm_pre_commit(cmd->tm);
	if (r < 0)
		return r;

	/*
	 * dm_sm_copy_root() can fail.  So we need to do it before we start
	 * updating the superblock.
	 */
	r = __save_sm_root(cmd);
	if (r)
		return r;

	r = superblock_lock_zero(cmd, &sblock);
	if (r)
		return r;

	disk_super = dm_block_data(sblock);
	disk_super->flags = 0;
	memset(disk_super->uuid, 0, sizeof(disk_super->uuid));
	disk_super->magic = cpu_to_le64(CACHE_SUPERBLOCK_MAGIC);
	disk_super->version = cpu_to_le32(cmd->version);
	disk_super->policy_hint_size = cpu_to_le32(0);

	__copy_sm_root(cmd, disk_super);

	disk_super->mapping_root = cpu_to_le64(cmd->root);
	disk_super->hint_root = cpu_to_le64(cmd->hint_root);
	disk_super->metadata_block_size = cpu_to_le32(DM_CACHE_METADATA_BLOCK_SIZE);
	disk_super->data_block_size = cpu_to_le32(cmd->data_block_size);
	disk_super->sub_block_size = cpu_to_le32(cmd->sub_block_size);
	disk_super->cache_blocks = cpu_to_le32(0);

	disk_super->dirty_root = cpu_to_le64(cmd->dirty_root);
	disk_super->sub_dirty_root = cpu_to_le64(cmd->sub_dirty_root);
	disk_super->sub_valid_root = cpu_to_le64(cmd->sub_valid_root);

	disk_super->clients_root = cpu_to_le64(cmd->clients_root);

	return dm_tm_commit(cmd->tm, sblock);
}

static int __format_metadata(struct dm_cache_metadata *cmd)
{
	int r;

	r = dm_tm_create_with_sm(cmd->bm, CACHE_SUPERBLOCK_LOCATION, 0,
				 &cmd->tm, &cmd->metadata_sm);
	if (r < 0) {
		DMERR("tm_create_with_sm failed");
		return r;
	}

	dm_disk_bitset_init(cmd->tm, &cmd->dirty_info);
	r = dm_bitset_empty(&cmd->dirty_info, &cmd->dirty_root);
	if (r < 0)
		goto bad;

	__setup_mapping_details(cmd);
	DMINFO("%s setup btree details", __func__);

	r = dm_btree_empty(&cmd->info, &cmd->root);
	if (r < 0)
		goto bad;

	r = dm_btree_empty(&cmd->clients_info, &cmd->clients_root);
	if (r < 0)
		goto bad;

	dm_disk_bitset_init(cmd->tm, &cmd->sub_dirty_info);
	r = dm_bitset_empty(&cmd->sub_dirty_info, &cmd->sub_dirty_root);
	if (r < 0)
		goto bad;

	dm_disk_bitset_init(cmd->tm, &cmd->sub_valid_info);
	r = dm_bitset_empty(&cmd->sub_valid_info, &cmd->sub_valid_root);
	if (r < 0)
		goto bad;

	r = __write_initial_superblock(cmd);
	if (r)
		goto bad;

	cmd->clean_when_opened = true;
	return 0;

bad:
	dm_tm_destroy(cmd->tm);
	dm_sm_destroy(cmd->metadata_sm);

	return r;
}

static int __check_incompat_features(struct cache_disk_superblock *disk_super,
				     struct dm_cache_metadata *cmd)
{
	uint32_t incompat_flags, features;

	incompat_flags = le32_to_cpu(disk_super->incompat_flags);
	features = incompat_flags & ~DM_CACHE_FEATURE_INCOMPAT_SUPP;
	if (features) {
		DMERR("could not access metadata due to unsupported optional features (%lx).",
		      (unsigned long)features);
		return -EINVAL;
	}

	/*
	 * Check for read-only metadata to skip the following RDWR checks.
	 */
	if (get_disk_ro(cmd->bdev->bd_disk))
		return 0;

	features = le32_to_cpu(disk_super->compat_ro_flags) & ~DM_CACHE_FEATURE_COMPAT_RO_SUPP;
	if (features) {
		DMERR("could not access metadata RDWR due to unsupported optional features (%lx).",
		      (unsigned long)features);
		return -EINVAL;
	}

	return 0;
}

static int __open_metadata(struct dm_cache_metadata *cmd)
{
	int r;
	struct dm_block *sblock;
	struct cache_disk_superblock *disk_super;
	unsigned long sb_flags;

	r = superblock_read_lock(cmd, &sblock);
	if (r < 0) {
		DMERR("couldn't read lock superblock");
		return r;
	}

	disk_super = dm_block_data(sblock);

	/* Verify the data block size hasn't changed */
	if (le32_to_cpu(disk_super->data_block_size) != cmd->data_block_size) {
		DMERR("changing the data block size (from %u to %llu) is not supported",
		      le32_to_cpu(disk_super->data_block_size),
		      (unsigned long long)cmd->data_block_size);
		r = -EINVAL;
		goto bad;
	}

	r = __check_incompat_features(disk_super, cmd);
	if (r < 0)
		goto bad;

	r = dm_tm_open_with_sm(cmd->bm, CACHE_SUPERBLOCK_LOCATION,
			       disk_super->metadata_space_map_root,
			       sizeof(disk_super->metadata_space_map_root),
			       &cmd->tm, &cmd->metadata_sm);
	if (r < 0) {
		DMERR("tm_open_with_sm failed");
		goto bad;
	}

	__setup_mapping_details(cmd);
	DMINFO("%s setup btree details", __func__);

	dm_disk_bitset_init(cmd->tm, &cmd->dirty_info);
	dm_disk_bitset_init(cmd->tm, &cmd->sub_dirty_info);
	dm_disk_bitset_init(cmd->tm, &cmd->sub_valid_info);

	sb_flags = le32_to_cpu(disk_super->flags);
	cmd->clean_when_opened = test_bit(CLEAN_SHUTDOWN, &sb_flags);
	dm_bm_unlock(sblock);

	return 0;

bad:
	dm_bm_unlock(sblock);
	return r;
}

static int __open_or_format_metadata(struct dm_cache_metadata *cmd,
				     bool format_device)
{
	int r;
	bool unformatted = false;

	r = __superblock_all_zeroes(cmd->bm, &unformatted);
	if (r)
		return r;

	if (unformatted)
		return format_device ? __format_metadata(cmd) : -EPERM;

	return __open_metadata(cmd);
}

static int __create_persistent_data_objects(struct dm_cache_metadata *cmd,
					    bool may_format_device)
{
	int r;
	cmd->bm = dm_block_manager_create(cmd->bdev, DM_CACHE_METADATA_BLOCK_SIZE << SECTOR_SHIFT,
					  CACHE_MAX_CONCURRENT_LOCKS);
	if (IS_ERR(cmd->bm)) {
		DMERR("could not create block manager");
		r = PTR_ERR(cmd->bm);
		cmd->bm = NULL;
		return r;
	}

	r = __open_or_format_metadata(cmd, may_format_device);
	if (r) {
		dm_block_manager_destroy(cmd->bm);
		cmd->bm = NULL;
	}

	return r;
}

static void __destroy_persistent_data_objects(struct dm_cache_metadata *cmd)
{
	dm_sm_destroy(cmd->metadata_sm);
	dm_tm_destroy(cmd->tm);
	dm_block_manager_destroy(cmd->bm);
}

typedef unsigned long (*flags_mutator)(unsigned long);

static void update_flags(struct cache_disk_superblock *disk_super,
			 flags_mutator mutator)
{
	uint32_t sb_flags = mutator(le32_to_cpu(disk_super->flags));
	disk_super->flags = cpu_to_le32(sb_flags);
}

static unsigned long set_clean_shutdown(unsigned long flags)
{
	set_bit(CLEAN_SHUTDOWN, &flags);
	return flags;
}

static unsigned long clear_clean_shutdown(unsigned long flags)
{
	clear_bit(CLEAN_SHUTDOWN, &flags);
	return flags;
}

static void read_superblock(struct dm_cache_metadata *cmd,
                            struct cache_disk_superblock *disk_super)
{
	cmd->version = le32_to_cpu(disk_super->version);
	cmd->flags = le32_to_cpu(disk_super->flags);

	cmd->root = le64_to_cpu(disk_super->mapping_root);
	cmd->hint_root = le32_to_cpu(disk_super->hint_root);
	cmd->data_block_size = le32_to_cpu(disk_super->data_block_size);
	cmd->cache_blocks = to_cblock(le32_to_cpu(disk_super->cache_blocks));
	cmd->policy_hint_size = le32_to_cpu(disk_super->policy_hint_size);

	cmd->clients_root = le64_to_cpu(disk_super->clients_root);

	cmd->dirty_root = le64_to_cpu(disk_super->dirty_root);
	cmd->sub_valid_root = le64_to_cpu(disk_super->sub_valid_root);
	cmd->sub_dirty_root = le64_to_cpu(disk_super->sub_dirty_root);
	cmd->changed = false;
}

/*
 * The mutator updates the superblock flags.
 */
static int __begin_transaction_flags(struct dm_cache_metadata *cmd,
				     flags_mutator mutator)
{
	int r;
	struct cache_disk_superblock *disk_super;
	struct dm_block *sblock;

	r = superblock_lock(cmd, &sblock);
	if (r)
		return r;

	disk_super = dm_block_data(sblock);
	update_flags(disk_super, mutator);
	read_superblock(cmd, disk_super);
	dm_bm_unlock(sblock);

	return dm_bm_flush(cmd->bm);
}

static int __begin_transaction(struct dm_cache_metadata *cmd)
{
	int r;
	struct cache_disk_superblock *disk_super;
	struct dm_block *sblock;

	WARN_ON(cmd->changed);
	/*
	 * We re-read the superblock every time.  Shouldn't need to do this
	 * really.
	 */
	r = superblock_read_lock(cmd, &sblock);
	if (r)
		return r;

	disk_super = dm_block_data(sblock);
	read_superblock(cmd, disk_super);
	dm_bm_unlock(sblock);

	return 0;
}

static void read_client_md(struct dm_cache_client_metadata *ccmd)
{
	uint64_t key = ccmd->dev_id;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	struct client_disk_superblock client_sb;
	int r;

	r = dm_btree_lookup(&cmd->clients_info, cmd->clients_root,
			    &key, &client_sb);
	if (r) {
		DMERR("unable to read client metadata");
		return;
	}

	ccmd->discard_root = le64_to_cpu(client_sb.discard_root);
	ccmd->discard_block_size = le64_to_cpu(client_sb.discard_block_size);
	ccmd->discard_nr_blocks = to_dblock(le64_to_cpu(client_sb.discard_nr_blocks));
	ccmd->stats.read_hits = le32_to_cpu(client_sb.read_hits);
	ccmd->stats.read_misses = le32_to_cpu(client_sb.read_misses);
	ccmd->stats.write_hits = le32_to_cpu(client_sb.write_hits);
	ccmd->stats.write_misses = le32_to_cpu(client_sb.write_misses);
	strncpy(ccmd->policy_name, client_sb.policy_name, sizeof(client_sb.policy_name));
	ccmd->policy_version[0] = le64_to_cpu(client_sb.policy_version[0]);
	ccmd->policy_version[1] = le64_to_cpu(client_sb.policy_version[1]);
	ccmd->policy_version[2] = le64_to_cpu(client_sb.policy_version[2]);

}

static int write_client_md(struct dm_cache_client_metadata *md)
{
	int r;
	uint64_t key;
	struct client_disk_superblock sb;
	struct dm_cache_metadata *cmd = md->cmd;

	key = md->dev_id;
	r = dm_bitset_flush(&md->discard_info, md->discard_root,
			    &md->discard_root);
	if (r)
		return r;

	sb.discard_root = cpu_to_le64(md->discard_root);
	sb.discard_block_size = cpu_to_le64(md->discard_block_size);
	sb.discard_nr_blocks = cpu_to_le64(from_dblock(md->discard_nr_blocks));

	sb.read_hits = cpu_to_le32(md->stats.read_hits);
	sb.read_misses = cpu_to_le32(md->stats.read_misses);
	sb.write_hits = cpu_to_le32(md->stats.write_hits);
	sb.write_misses = cpu_to_le32(md->stats.write_misses);

	strncpy(sb.policy_name, md->policy_name, sizeof(sb.policy_name));
	sb.policy_version[0] = cpu_to_le32(md->policy_version[0]);
	sb.policy_version[1] = cpu_to_le32(md->policy_version[1]);
	sb.policy_version[2] = cpu_to_le32(md->policy_version[2]);

	__dm_bless_for_disk(&sb);

	r = dm_btree_insert(&cmd->clients_info, cmd->clients_root,
	                    &key, &sb, &cmd->clients_root);
	if (r)
		return r;

	md->changed = false;
	cmd->changed = true;
	return 0;
}

static int __write_changed_clients_md(struct dm_cache_metadata *cmd)
{
	int r;
	struct dm_cache_client_metadata *client_md, *tmp;

	list_for_each_entry_safe(client_md, tmp, &cmd->clients, list) {
		if (!client_md->changed)
			continue;

		r = write_client_md(client_md);
		if (r)
			return r;

		if (client_md->open_count)
			continue;

		list_del(&client_md->list);
		kfree(client_md);
	}

	return 0;
}

static int __commit_transaction(struct dm_cache_metadata *cmd,
				flags_mutator mutator)
{
	int r;
	struct cache_disk_superblock *disk_super;
	struct dm_block *sblock;

	/*
	 * We need to know if the cache_disk_superblock exceeds a 512-byte sector.
	 */
	BUILD_BUG_ON(sizeof(struct cache_disk_superblock) > 512);

	r = __write_changed_clients_md(cmd);
	if (r)
		return r;

	if (!cmd->changed)
		return 0;

	r = dm_bitset_flush(&cmd->dirty_info, cmd->dirty_root,
			    &cmd->dirty_root);
	if (r)
		return r;

	r = dm_bitset_flush(&cmd->sub_valid_info, cmd->sub_valid_root,
	                    &cmd->sub_valid_root);
	if (r)
		return r;

	r = dm_bitset_flush(&cmd->sub_dirty_info, cmd->sub_dirty_root,
	                    &cmd->sub_dirty_root);
	if (r)
		return r;

	r = dm_tm_pre_commit(cmd->tm);
	if (r < 0)
		return r;

	r = __save_sm_root(cmd);
	if (r)
		return r;

	r = superblock_lock(cmd, &sblock);
	if (r)
		return r;

	disk_super = dm_block_data(sblock);

	disk_super->flags = cpu_to_le32(cmd->flags);
	if (mutator)
		update_flags(disk_super, mutator);

	disk_super->mapping_root = cpu_to_le64(cmd->root);
	disk_super->hint_root = cpu_to_le64(cmd->hint_root);
	disk_super->dirty_root = cpu_to_le64(cmd->dirty_root);
	disk_super->sub_dirty_root = cpu_to_le64(cmd->sub_dirty_root);
	disk_super->sub_valid_root = cpu_to_le64(cmd->sub_valid_root);
	disk_super->cache_blocks = cpu_to_le32(from_cblock(cmd->cache_blocks));
	disk_super->policy_hint_size = cpu_to_le32(cmd->policy_hint_size);
	disk_super->clients_root = cpu_to_le32(cmd->clients_root);

	__copy_sm_root(cmd, disk_super);

	r = dm_tm_commit(cmd->tm, sblock);
	if (!r)
		cmd->changed = false;

	return r;
}

/*----------------------------------------------------------------*/

/*
 * The mappings are held in a dm-array that has 64-bit values stored in
 * little-endian format.  The index is the cblock, the high 48bits of the
 * value are the oblock and the low 16 bit the flags.
 */
#define FLAGS_MASK ((1 << 16) - 1)

static __le64 pack_value(dm_oblock_t block, unsigned flags)
{
	uint64_t value = from_oblock(block);
	value <<= 16;
	value = value | (flags & FLAGS_MASK);
	return cpu_to_le64(value);
}

static void unpack_value(__le64 value_le, dm_oblock_t *block, unsigned *flags)
{
	uint64_t value = le64_to_cpu(value_le);
	uint64_t b = value >> 16;
	*block = to_oblock(b);
	if (flags)
		*flags = value & FLAGS_MASK;
}

/*----------------------------------------------------------------*/

static struct dm_cache_metadata *metadata_open(struct block_device *bdev,
					       sector_t data_block_size,
					       sector_t sub_block_size,
					       bool may_format_device,
					       size_t policy_hint_size,
					       unsigned metadata_version)
{
	int r;
	struct dm_cache_metadata *cmd;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		DMERR("could not allocate metadata struct");
		return ERR_PTR(-ENOMEM);
	}

	cmd->version = metadata_version;
	refcount_set(&cmd->ref_count, 1);
	init_rwsem(&cmd->root_lock);
	cmd->bdev = bdev;
	cmd->data_block_size = data_block_size;
	cmd->sub_block_size = sub_block_size;
	cmd->cache_blocks = 0;
	cmd->policy_hint_size = policy_hint_size;
	cmd->changed = true;
	cmd->fail_io = false;

	INIT_LIST_HEAD(&cmd->clients);
	INIT_LIST_HEAD(&cmd->recycle_clients);

	r = __create_persistent_data_objects(cmd, may_format_device);
	if (r) {
		kfree(cmd);
		return ERR_PTR(r);
	}

	r = __begin_transaction_flags(cmd, clear_clean_shutdown);
	if (r < 0) {
		dm_cache_metadata_close(cmd);
		return ERR_PTR(r);
	}

	return cmd;
}

/*
 * We keep a little list of ref counted metadata objects to prevent two
 * different target instances creating separate bufio instances.  This is
 * an issue if a table is reloaded before the suspend.
 */
static DEFINE_MUTEX(table_lock);
static LIST_HEAD(table);

static struct dm_cache_metadata *lookup(struct block_device *bdev)
{
	struct dm_cache_metadata *cmd;

	list_for_each_entry(cmd, &table, list)
		if (cmd->bdev == bdev) {
			refcount_inc(&cmd->ref_count);
			return cmd;
		}

	return NULL;
}

static struct dm_cache_metadata *lookup_or_open(struct block_device *bdev,
						sector_t data_block_size,
						sector_t sub_block_size,
						bool may_format_device,
						size_t policy_hint_size,
						unsigned metadata_version)
{
	struct dm_cache_metadata *cmd, *cmd2;

	mutex_lock(&table_lock);
	cmd = lookup(bdev);
	mutex_unlock(&table_lock);

	if (cmd)
		return cmd;

	cmd = metadata_open(bdev, data_block_size, sub_block_size,
	                    may_format_device, policy_hint_size,
	                    metadata_version);
	if (!IS_ERR(cmd)) {
		mutex_lock(&table_lock);
		cmd2 = lookup(bdev);
		if (cmd2) {
			mutex_unlock(&table_lock);
			__destroy_persistent_data_objects(cmd);
			kfree(cmd);
			return cmd2;
		}
		list_add(&cmd->list, &table);
		mutex_unlock(&table_lock);
	}

	return cmd;
}

static bool same_params(struct dm_cache_metadata *cmd,
                        sector_t data_block_size, sector_t sub_block_size)
{
	if (cmd->data_block_size != data_block_size) {
		DMERR("data_block_size (%llu) different from that in metadata (%llu)",
		      (unsigned long long) data_block_size,
		      (unsigned long long) cmd->data_block_size);
		return false;
	}

	if (cmd->sub_block_size != sub_block_size) {
		DMERR("sub_block_size (%llu) different from that in metadata (%llu)",
		      (unsigned long long) sub_block_size,
		      (unsigned long long) cmd->sub_block_size);
		return false;
	}

	return true;
}

struct dm_cache_metadata *dm_cache_metadata_open(struct block_device *bdev,
						 sector_t data_block_size,
						 sector_t sub_block_size,
						 bool may_format_device,
						 size_t policy_hint_size,
						 unsigned metadata_version)
{
	struct dm_cache_metadata *cmd;

	cmd = lookup_or_open(bdev, data_block_size, sub_block_size,
	                     may_format_device, policy_hint_size, metadata_version);

	if (!IS_ERR(cmd) && !same_params(cmd, data_block_size, sub_block_size)) {
		dm_cache_metadata_close(cmd);
		return ERR_PTR(-EINVAL);
	}

	return cmd;
}

void dm_cache_metadata_close(struct dm_cache_metadata *cmd)
{
	if (refcount_dec_and_test(&cmd->ref_count)) {
		mutex_lock(&table_lock);
		list_del(&cmd->list);
		mutex_unlock(&table_lock);

		if (!cmd->fail_io)
			__destroy_persistent_data_objects(cmd);
		kfree(cmd);
	}
}

struct mapping_walker {
	struct dm_cache_client_metadata *ccmd;
	load_mapping_fn fn;
	void *context;
	bool hints_valid;
	uint32_t marker;
};

static int block_clean(void *context, uint64_t *keys, void *leaf)
{
	int r;
	uint32_t skip, index;
	__le64 value = *(__le64 *)leaf;
	dm_oblock_t oblock;
	dm_cblock_t cblock = to_cblock((uint32_t)*keys);
	struct mapping_walker *w = context;
	struct dm_cache_client_metadata *md = w->ccmd;
	struct dm_cache_metadata *cmd = md->cmd;
	bool *not_dirtied = w->context;

	unpack_value(value, &oblock, NULL);

	index = (uint32_t)oblock;

	*not_dirtied = true;
	if (from_cblock(cmd->cache_blocks) == 0)
		/* Nothing to do */
		return 0;

	skip = from_cblock(cblock) - w->marker;
	if (skip) {
		r = dm_bitset_cursor_skip(&cmd->dirty_cursor, skip);
		if (r)
			return r;
	}

	*not_dirtied &= ~dm_bitset_cursor_get_value(&cmd->dirty_cursor);
	w->marker = from_cblock(cblock);

	return 0;
}

static int client_blocks_are_clean(struct dm_cache_client_metadata *ccmd,
				   dm_cblock_t begin, dm_cblock_t end,
				   bool *result)
{
	int r;
	__le64 value;
	uint64_t key = ccmd->dev_id;
	dm_block_t mapping_root;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	struct mapping_walker walker = {
		.ccmd = ccmd,
		.context = result,
		.marker = 0,
	};

	*result = true;
	if (from_cblock(cmd->cache_blocks) == 0)
		/* Nothing to do */
		return 0;

	r = dm_btree_lookup(&cmd->tl_info, cmd->root, &key, &value);
	if (r)
		return r;

	r = dm_bitset_cursor_begin(&cmd->dirty_info, cmd->dirty_root,
				   from_cblock(cmd->cache_blocks),
				   &cmd->dirty_cursor);
	if (r)
		return r;

	mapping_root = le64_to_cpu(value);
	r = dm_btree_walk(&cmd->bl_info, mapping_root, block_clean, &walker);

	dm_bitset_cursor_end(&cmd->dirty_cursor);

	return r;
}

static int blocks_are_clean_separate_dirty(struct dm_cache_metadata *cmd,
					   dm_cblock_t begin, dm_cblock_t end,
					   bool *result)
{
	int r;
	bool dirty_flag;
	*result = true;

	if (from_cblock(cmd->cache_blocks) == 0)
		/* Nothing to do */
		return 0;

	r = dm_bitset_cursor_begin(&cmd->dirty_info, cmd->dirty_root,
				   from_cblock(cmd->cache_blocks), &cmd->dirty_cursor);
	if (r) {
		DMERR("%s: dm_bitset_cursor_begin for dirty failed", __func__);
		return r;
	}

	r = dm_bitset_cursor_skip(&cmd->dirty_cursor, from_cblock(begin));
	if (r) {
		DMERR("%s: dm_bitset_cursor_skip for dirty failed", __func__);
		dm_bitset_cursor_end(&cmd->dirty_cursor);
		return r;
	}

	while (begin != end) {
		/*
		 * We assume that unmapped blocks have their dirty bit
		 * cleared.
		 */
		dirty_flag = dm_bitset_cursor_get_value(&cmd->dirty_cursor);
		if (dirty_flag) {
			DMERR("%s: cache block %llu is dirty", __func__,
			      (unsigned long long) from_cblock(begin));
			dm_bitset_cursor_end(&cmd->dirty_cursor);
			*result = false;
			return 0;
		}

		begin = to_cblock(from_cblock(begin) + 1);
		if (begin == end)
			break;

		r = dm_bitset_cursor_next(&cmd->dirty_cursor);
		if (r) {
			DMERR("%s: dm_bitset_cursor_next for dirty failed", __func__);
			dm_bitset_cursor_end(&cmd->dirty_cursor);
			return r;
		}
	}

	dm_bitset_cursor_end(&cmd->dirty_cursor);

	return 0;
}

static int blocks_are_unmapped_or_clean(struct dm_cache_metadata *cmd,
					dm_cblock_t begin, dm_cblock_t end,
					bool *result)
{
	return blocks_are_clean_separate_dirty(cmd, begin, end, result);
}

static bool cmd_write_lock_core(struct dm_cache_metadata *cmd)
{
	down_write(&cmd->root_lock);
	if (cmd->fail_io || dm_bm_is_read_only(cmd->bm)) {
		up_write(&cmd->root_lock);
		return false;
	}

	return true;
}

static bool cmd_write_lock(struct dm_cache_metadata *cmd)
{
	if (!cmd_write_lock_core(cmd))
		return false;

	if (!cmd->changed)
		cmd->changed = true;

	return true;
}

#define WRITE_LOCK(cmd)				\
	do {					\
		if (!cmd_write_lock((cmd)))	\
			return -EINVAL;		\
	} while(0)

#define WRITE_LOCK_CORE(cmd)				\
	do {						\
		if (!cmd_write_lock_core((cmd)))	\
			return -EINVAL;			\
	} while(0)

#define WRITE_LOCK_VOID(cmd)			\
	do {					\
		if (!cmd_write_lock((cmd)))	\
			return;			\
	} while(0)

#define WRITE_UNLOCK(cmd) \
	up_write(&(cmd)->root_lock)

static bool cmd_read_lock(struct dm_cache_metadata *cmd)
{
	down_read(&cmd->root_lock);
	if (cmd->fail_io) {
		up_read(&cmd->root_lock);
		return false;
	}
	return true;
}

#define READ_LOCK(cmd)				\
	do {					\
		if (!cmd_read_lock((cmd)))	\
			return -EINVAL;		\
	} while(0)

#define READ_LOCK_VOID(cmd)			\
	do {					\
		if (!cmd_read_lock((cmd)))	\
			return;			\
	} while(0)

#define READ_UNLOCK(cmd) \
	up_read(&(cmd)->root_lock)

static int __open_device_md(struct dm_cache_metadata *cmd,
			    uint64_t dev_id, int create,
			    struct dm_cache_client_metadata **md)
{
	int r, changed = 0;
	struct dm_cache_client_metadata *md2;
	uint64_t key = dev_id;
	struct client_disk_superblock csb;

	list_for_each_entry(md2, &cmd->clients, list)
		// FIXME: always block this case
		if (md2->dev_id == dev_id) {
			if (create)
				return -EEXIST;

			md2->open_count++;
			*md = md2;
			return 0;
		}

	r = dm_btree_lookup(&cmd->clients_info, cmd->clients_root,
			    &key, &csb);
	if (r) {
		if (r != -ENODATA || !create)
			return r;

		changed = 1;
	}

	*md = kzalloc(sizeof(**md), GFP_KERNEL);
	if (!*md)
		return -ENOMEM;

	(*md)->cmd = cmd;
	(*md)->dev_id = dev_id;
	(*md)->open_count = 1;
	(*md)->changed = changed;

	dm_disk_bitset_init(cmd->tm, &(*md)->discard_info);

	if (!create)
		read_client_md(*md);


	list_add(&(*md)->list, &cmd->clients);

	return 0;
}

static void __close_device_md(struct dm_cache_client_metadata *client_md)
{
	--client_md->open_count;
}

static int __create_client(struct dm_cache_metadata *cmd,
			   uint64_t dev_id)
{
	int r;
	dm_block_t dev_root;
	uint64_t key = dev_id;
	struct client_disk_superblock sb;
	struct dm_cache_client_metadata *md;
	__le64 value;

	r = dm_btree_lookup(&cmd->clients_info, cmd->clients_root,
			    &key, &sb);
	if (!r)
		return -EEXIST;

	r = dm_btree_empty(&cmd->bl_info, &dev_root);
	if (r)
		return r;

	value = cpu_to_le64(dev_root);
	__dm_bless_for_disk(&value);
	r = dm_btree_insert(&cmd->tl_info, cmd->root, &key, &value, &cmd->root);
	if (r)
		goto bad_tl_root;

	r = __open_device_md(cmd, dev_id, 1, &md);
	if (r)
		goto bad_client_md;

	r = dm_bitset_empty(&md->discard_info, &md->discard_root);
	if (r < 0)
		goto bad_discard_bitset;

	r = write_client_md(md);
	if (r)
		goto bad_client_sb;

	__close_device_md(md);

	return r;

bad_client_sb:
	dm_bitset_del(&md->discard_info, md->discard_root);
bad_discard_bitset:
	__close_device_md(md);
bad_client_md:
	dm_btree_remove(&cmd->tl_info, cmd->root, &key, &cmd->root);
bad_tl_root:
	dm_btree_del(&cmd->bl_info, dev_root);
	return r;

}

int dm_cache_create_client(struct dm_cache_metadata *cmd,
			   uint64_t dev_id)
{
	int r = -EINVAL;

	WRITE_LOCK(cmd);
	if (!cmd->fail_io)
		r = __create_client(cmd, dev_id);
	if (!r)
		cmd->changed = true;
	WRITE_UNLOCK(cmd);

	return r;
}

static int __delete_device(struct dm_cache_metadata *cmd,
			   uint64_t dev_id)
{
	int r;
	__le64 value;
	uint64_t key = dev_id;
	struct dm_cache_client_metadata *md;

	r = __open_device_md(cmd, dev_id, 0, &md);
	if (r)
		return r;

	if (md->open_count > 1) {
		__close_device_md(md);
		return -EBUSY;
	}

	list_del_init(&md->list);

	r = dm_btree_remove(&cmd->clients_info, cmd->clients_root, &key, &cmd->clients_root);
	if (r)
		return r;

	r = dm_bitset_del(&md->discard_info, md->discard_root);
	if (r)
		return r;

	r = dm_btree_lookup(&cmd->tl_info, cmd->root, &key, &value);
	if (r)
		return r;

	md->mapping_root = le64_to_cpu(value);
	r = dm_btree_remove(&cmd->tl_info, cmd->root, &key, &cmd->root);
	if (r)
		return r;

	list_add_tail(&md->list, &cmd->recycle_clients);
	return 0;
}

int dm_cache_delete_client(struct dm_cache_metadata *cmd,
			   uint64_t dev_id)
{
	int r = -EINVAL;

	WRITE_LOCK(cmd);
	if (!cmd->fail_io)
		r = __delete_device(cmd, dev_id);
	WRITE_UNLOCK(cmd);

	return r;
}

struct recycler {
	recycle_fn fn;
	void *context;
};

static int __recycle(void *context, uint64_t *keys, void *leaf)
{
	struct recycler *r = context;
	dm_cblock_t b = *keys;

	return r->fn(r->context, b);
}

int dm_cache_recycle_clients(struct dm_cache_metadata *cmd, recycle_fn fn, void *context)
{
	int r = 0;
	struct list_head free_list;
	struct dm_cache_client_metadata *md, *tmp;
	struct recycler recycler = {
		.fn = fn,
		.context = context
	};

	INIT_LIST_HEAD(&free_list);

	READ_LOCK(cmd);
	if (list_empty(&cmd->recycle_clients))
		goto out;

	list_for_each_entry_safe(md, tmp, &cmd->recycle_clients, list) {
		r = dm_btree_walk(&cmd->bl_info, md->mapping_root, __recycle, &recycler);
		if (r)
			break;

		list_del_init(&md->list);
		list_add_tail(&md->list, &free_list);
	}
out:
	READ_UNLOCK(cmd);

	list_for_each_entry_safe(md, tmp, &free_list, list) {
		list_del(&md->list);
		WRITE_LOCK(cmd);
		if (dm_btree_del(&cmd->bl_info, md->mapping_root))
			DMERR("delete mapping root %llu for client %llu failed", md->mapping_root, md->dev_id);
		WRITE_UNLOCK(cmd);
		kfree(md);
	}

	return r;
}

int dm_cache_client_metadata_open(struct dm_cache_metadata *cmd, uint64_t dev_id,
				  struct dm_cache_client_metadata **ccmd)
{
	int r = -EINVAL;

	WRITE_LOCK(cmd);
	if (!cmd->fail_io)
		r = __open_device_md(cmd, dev_id, 0, ccmd);
	WRITE_UNLOCK(cmd);

	return r;
}

int dm_cache_client_metadata_close(struct dm_cache_client_metadata *ccmd)
{
	WRITE_LOCK(ccmd->cmd);
	__close_device_md(ccmd);
	WRITE_UNLOCK(ccmd->cmd);

	return 0;
}

int dm_cache_resize(struct dm_cache_metadata *cmd, dm_cblock_t new_cache_size)
{
	int r;
	bool clean;
	unsigned sub_bits_per_cblock;

	WRITE_LOCK(cmd);

	if (from_cblock(new_cache_size) < from_cblock(cmd->cache_blocks)) {
		r = blocks_are_unmapped_or_clean(cmd, new_cache_size, cmd->cache_blocks, &clean);
		if (r)
			goto out;

		if (!clean) {
			DMERR("unable to shrink cache due to dirty blocks");
			r = -EINVAL;
			goto out;
		}
	}

	r = dm_bitset_resize(&cmd->dirty_info, cmd->dirty_root,
			     from_cblock(cmd->cache_blocks), from_cblock(new_cache_size),
			     false, &cmd->dirty_root);
	if (r)
		goto out;

	sub_bits_per_cblock = dm_sector_div_up(cmd->data_block_size, cmd->sub_block_size);
	r = dm_bitset_resize(&cmd->sub_valid_info, cmd->sub_valid_root,
	                     from_cblock(cmd->cache_blocks) * sub_bits_per_cblock,
	                     from_cblock(new_cache_size) * sub_bits_per_cblock,
	                     false, &cmd->sub_valid_root);
	if (r)
		goto out;

	r = dm_bitset_resize(&cmd->sub_dirty_info, cmd->sub_dirty_root,
	                     from_cblock(cmd->cache_blocks) * sub_bits_per_cblock,
	                     from_cblock(new_cache_size) * sub_bits_per_cblock,
	                     false, &cmd->sub_dirty_root);
	if (r)
		goto out;

	cmd->cache_blocks = new_cache_size;
	cmd->changed = true;

out:
	WRITE_UNLOCK(cmd);

	return r;
}

int dm_cache_client_discard_bitset_resize(struct dm_cache_client_metadata *ccmd,
                                          sector_t discard_block_size,
                                          dm_dblock_t new_nr_entries)
{
	int r;
	struct dm_cache_metadata *cmd = ccmd->cmd;

	WRITE_LOCK(cmd);
	r = dm_bitset_resize(&ccmd->discard_info,
			     ccmd->discard_root,
			     from_dblock(ccmd->discard_nr_blocks),
			     from_dblock(new_nr_entries),
			     false, &ccmd->discard_root);
	if (!r) {
		ccmd->discard_block_size = discard_block_size;
		ccmd->discard_nr_blocks = new_nr_entries;
	}

	ccmd->changed = true;
	cmd->changed = true;
	WRITE_UNLOCK(cmd);

	return r;
}

static int __set_discard(struct dm_cache_client_metadata *ccmd, dm_dblock_t b)
{
	return dm_bitset_set_bit(&ccmd->discard_info, ccmd->discard_root,
	                         from_dblock(b), &ccmd->discard_root);
}

static int __clear_discard(struct dm_cache_client_metadata *ccmd, dm_dblock_t b)
{
	return dm_bitset_clear_bit(&ccmd->discard_info, ccmd->discard_root,
	                           from_dblock(b), &ccmd->discard_root);
}

static int __discard(struct dm_cache_client_metadata *ccmd,
		     dm_dblock_t dblock, bool discard)
{
	int r;

	r = (discard ? __set_discard : __clear_discard)(ccmd, dblock);
	if (r)
		return r;

	ccmd->changed = true;
	ccmd->cmd->changed = true;
	return 0;
}

int dm_cache_client_set_discard(struct dm_cache_client_metadata *ccmd,
                                dm_dblock_t dblock, bool discard)
{
	int r;

	WRITE_LOCK(ccmd->cmd);
	r = __discard(ccmd, dblock, discard);
	WRITE_UNLOCK(ccmd->cmd);

	return r;
}

static int __load_bits(struct dm_disk_bitset *info, dm_block_t *root,
                       sector_t bit_block_size,
                       uint32_t nr_bits, load_bitset_fn fn, void *context)
{
	int r;
	uint32_t b;
	struct dm_bitset_cursor c;

	r = dm_bitset_flush(info, *root, root);
	if (r)
		return r;

	r = dm_bitset_cursor_begin(info, *root, nr_bits, &c);
	if (r)
		return r;

	for (b = 0; ; b++) {
		r = fn(context, bit_block_size, b, dm_bitset_cursor_get_value(&c));
		if (r)
			break;

		if (b >= (nr_bits - 1))
			break;

		r = dm_bitset_cursor_next(&c);
		if (r)
			break;
	}

	dm_bitset_cursor_end(&c);
	return r;
}

static int __load_discards(struct dm_cache_client_metadata *ccmd,
			   load_bitset_fn fn, void *context)
{
	int r = 0;
	uint32_t b;
	struct dm_cache_metadata *cmd = ccmd->cmd;

	if (from_dblock(ccmd->discard_nr_blocks) == 0)
		/* nothing to do */
		return 0;

	if (cmd->clean_when_opened) {
		r = __load_bits(&ccmd->discard_info, &ccmd->discard_root,
		                ccmd->discard_block_size, ccmd->discard_nr_blocks,
				fn, context);
		if (r)
			return r;

	} else {
		for (b = 0; b < from_dblock(ccmd->discard_nr_blocks); b++) {
			r = fn(context, ccmd->discard_block_size, b, false);
			if (r)
				return r;
		}
	}

	return r;
}

int dm_cache_client_load_discards(struct dm_cache_client_metadata *ccmd,
                                  load_bitset_fn fn, void *context)
{
	int r;

	READ_LOCK(ccmd->cmd);
	r = __load_discards(ccmd, fn, context);
	READ_UNLOCK(ccmd->cmd);

	return r;
}


static int __load_sub_bitset(struct dm_cache_metadata *cmd,
                             struct dm_disk_bitset *info, dm_block_t *root,
			     load_bitset_fn fn, void *context)
{
	sector_t sub_blocks_per_cblock = dm_sector_div_up(cmd->data_block_size,
	                                                  cmd->sub_block_size);

	if (from_cblock(cmd->cache_blocks) == 0)
		/* nothing to do */
		return 0;

	return __load_bits(info, root, cmd->sub_block_size,
	                   from_cblock(cmd->cache_blocks) * sub_blocks_per_cblock,
			   fn, context);
}

int dm_cache_load_sub_valids(struct dm_cache_metadata *cmd,
			     load_bitset_fn fn, void *context)
{
	int r;

	READ_LOCK(cmd);
	r = __load_sub_bitset(cmd, &cmd->sub_valid_info,
	                      &cmd->sub_valid_root, fn, context);
	READ_UNLOCK(cmd);

	return r;
}

int dm_cache_load_sub_dirties(struct dm_cache_metadata *cmd,
			      load_bitset_fn fn, void *context, bool *skipped)
{
	int r = 0;

	READ_LOCK(cmd);
	if (cmd->clean_when_opened) {
		r = __load_sub_bitset(cmd, &cmd->sub_dirty_info,
				      &cmd->sub_dirty_root, fn, context);
		*skipped = false;
	} else {
		DMERR("metadata is dirtied, treat all valid blocks dirty");
		*skipped = true;
	}
	READ_UNLOCK(cmd);

	return r;
}

int dm_cache_size(struct dm_cache_metadata *cmd, dm_cblock_t *result)
{
	READ_LOCK(cmd);
	*result = cmd->cache_blocks;
	READ_UNLOCK(cmd);

	return 0;
}

static int __remove(struct dm_cache_client_metadata *ccmd, dm_cblock_t cblock)
{
	int r;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	dm_block_t keys[2] = {ccmd->dev_id, cblock};

	r = dm_btree_remove(&cmd->info, cmd->root, keys, &cmd->root);
	if (r)
		return r;

	ccmd->changed = true;
	cmd->changed = true;
	return 0;
}

int dm_cache_client_remove_mapping(struct dm_cache_client_metadata *ccmd,
                                   dm_cblock_t cblock)
{
	int r;

	WRITE_LOCK(ccmd->cmd);
	r = __remove(ccmd, cblock);
	WRITE_UNLOCK(ccmd->cmd);

	return r;
}

static int __insert(struct dm_cache_client_metadata *ccmd,
		    dm_cblock_t cblock, dm_oblock_t oblock)
{
	int r;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	dm_block_t keys[2] = {ccmd->dev_id, cblock};
	__le64 value = pack_value(oblock, M_VALID);

	__dm_bless_for_disk(&value);
	r = dm_btree_insert(&cmd->info, cmd->root, keys, &value, &cmd->root);
	if (r)
		return r;

	ccmd->changed = true;
	cmd->changed = true;

	return 0;
}

int dm_cache_client_insert_mapping(struct dm_cache_client_metadata *ccmd,
                                   dm_cblock_t cblock, dm_oblock_t oblock)
{
	int r;

	WRITE_LOCK(ccmd->cmd);
	r = __insert(ccmd, cblock, oblock);
	WRITE_UNLOCK(ccmd->cmd);

	return r;
}

static bool policy_unchanged(struct dm_cache_client_metadata *client_md,
			     struct dm_cache_policy *policy)
{
	const char *policy_name = dm_cache_policy_get_name(policy);
	const unsigned *policy_version = dm_cache_policy_get_version(policy);
	size_t policy_hint_size = dm_cache_policy_get_hint_size(policy);

	/*
	 * Ensure policy names match.
	 */
	if (strncmp(client_md->policy_name, policy_name, sizeof(client_md->policy_name)))
		return false;

	/*
	 * Ensure policy major versions match.
	 */
	if (client_md->policy_version[0] != policy_version[0])
		return false;

	/*
	 * Ensure policy hint sizes match.
	 */
	if (client_md->cmd->policy_hint_size != policy_hint_size)
		return false;

	return true;
}

static bool hints_array_initialized(struct dm_cache_metadata *cmd)
{
	return cmd->hint_root && cmd->policy_hint_size;
}

static bool hints_array_available(struct dm_cache_client_metadata *client_md,
				  struct dm_cache_policy *policy)
{
	struct dm_cache_metadata *cmd = client_md->cmd;

	return cmd->clean_when_opened &&
	       policy_unchanged(client_md, policy) &&
	       hints_array_initialized(cmd);
}

static int __load_mapping(void *context, uint64_t *keys, void *leaf)
{
	int r;
	bool dirtied = true;
	uint32_t skip, index;
	__le32 hint = 0;
	__le64 value = *(__le64 *)leaf;
	dm_oblock_t oblock;
	dm_cblock_t cblock = to_cblock((uint32_t)*keys);
	struct mapping_walker *w = context;
	struct dm_cache_client_metadata *md = w->ccmd;
	struct dm_cache_metadata *cmd = md->cmd;

	unpack_value(value, &oblock, NULL);

	index = (uint32_t)oblock;

	skip = from_cblock(cblock) - w->marker;
	if (cmd->clean_when_opened) {
		if (skip) {
			r = dm_bitset_cursor_skip(&cmd->dirty_cursor, skip);
			if (r)
				return r;
		}
		dirtied = dm_bitset_cursor_get_value(&cmd->dirty_cursor);
	}

	if (w->hints_valid) {
		__le32 *hint_value_le;
		if (skip) {
			r = dm_array_cursor_skip(&cmd->hint_cursor, skip);
			if (r)
				return r;
		}
		dm_array_cursor_get_value(&cmd->hint_cursor,
		                          (void **)&hint_value_le);
		memcpy(&hint, hint_value_le, sizeof(hint));
	}

	r = w->fn(w->context, oblock, cblock, dirtied, le32_to_cpu(hint), w->hints_valid);
	if (r)
		DMERR("policy couldn't load cache block %llu",
	               (unsigned long long) cblock);

	w->marker = from_cblock(cblock);
	return r;
}

static int __load_mappings(struct dm_cache_client_metadata *ccmd,
			   struct dm_cache_policy *policy,
			   load_mapping_fn fn, void *context)
{
	int r;
	__le64 value;
	uint64_t key = ccmd->dev_id;
	dm_block_t mapping_root;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	struct mapping_walker walker = {
		.ccmd = ccmd,
		.fn = fn,
		.context = context,
		.hints_valid = hints_array_available(ccmd, policy),
		.marker = 0,
	};

	if (from_cblock(cmd->cache_blocks) == 0)
		/* Nothing to do */
		return 0;

	r = dm_btree_lookup(&cmd->tl_info, cmd->root, &key, &value);
	if (r)
		return r;

	if (walker.hints_valid) {
		r = dm_array_cursor_begin(&cmd->hint_info, cmd->hint_root, &cmd->hint_cursor);
		if (r)
			return r;
	}

	r = dm_bitset_cursor_begin(&cmd->dirty_info, cmd->dirty_root,
				   from_cblock(cmd->cache_blocks),
				   &cmd->dirty_cursor);
	if (r) {
		dm_array_cursor_end(&cmd->hint_cursor);
		return r;
	}

	mapping_root = le64_to_cpu(value);
	r = dm_btree_walk(&cmd->bl_info, mapping_root, __load_mapping, &walker);

	if (walker.hints_valid)
		dm_array_cursor_end(&cmd->hint_cursor);

	dm_bitset_cursor_end(&cmd->dirty_cursor);

	return r;
}

int dm_cache_client_load_mappings(struct dm_cache_client_metadata *ccmd,
                                  struct dm_cache_policy *policy,
                                  load_mapping_fn fn, void *context)
{
	int r;

	READ_LOCK(ccmd->cmd);
	r = __load_mappings(ccmd, policy, fn, context);
	READ_UNLOCK(ccmd->cmd);

	return r;
}

int dm_cache_changed_this_transaction(struct dm_cache_metadata *cmd)
{
	int r;

	READ_LOCK(cmd);
	r = cmd->changed;
	READ_UNLOCK(cmd);

	return r;
}

struct walker {
	walk_mapping_fn fn;
	struct dm_cache_metadata *cmd;
	void *context;
};

static int walk_cblock(void *context, uint64_t *keys, void *leaf)
{
	int r = 0;
	dm_cblock_t cblock = to_cblock((uint32_t)*keys);
	struct walker *w = (struct walker *)context;

	r = w->fn(w->context, cblock);

	return r;
}

static int walk_client(void *context, uint64_t *keys, void *leaf)
{
	int r = 0;
	__le64 value;
	struct walker *w = (struct walker *)context;
	struct dm_cache_metadata *cmd = w->cmd;
	dm_block_t mapping_root;

	r = dm_btree_lookup(&cmd->tl_info, cmd->root, keys, &value);
	if (r)
		return r;

	mapping_root = le64_to_cpu(value);
	r = dm_btree_walk(&cmd->bl_info, mapping_root, walk_cblock, w);

	return r;
}

int dm_cache_walk(struct dm_cache_metadata *cmd, walk_mapping_fn fn, void *context)
{
	int r;
	struct walker w = {
		.fn = fn,
		.context = context,
		.cmd = cmd,
	};

	READ_LOCK(cmd);

	if (from_cblock(cmd->cache_blocks) == 0) {
		READ_UNLOCK(cmd);
		/* nothing to do */
		return 0;
	}

	r = dm_btree_walk(&cmd->tl_info, cmd->root, walk_client, &w);

	READ_UNLOCK(cmd);

	return r;
}

static int is_dirty_callback(uint32_t index, bool *value, void *context)
{
	unsigned long *bits = context;
	*value = test_bit(index, bits);
	return 0;
}

static int __set_dirty_bits_v2(struct dm_cache_metadata *cmd, unsigned nr_bits, unsigned long *bits)
{
	int r = 0;

	/* nr_bits is really just a sanity check */
	if (nr_bits != from_cblock(cmd->cache_blocks)) {
		DMERR("dirty bitset is wrong size");
		return -EINVAL;
	}

	r = dm_bitset_del(&cmd->dirty_info, cmd->dirty_root);
	if (r)
		return r;

	cmd->changed = true;
	return dm_bitset_new(&cmd->dirty_info, &cmd->dirty_root, nr_bits, is_dirty_callback, bits);
}

int dm_cache_set_dirty_bits(struct dm_cache_metadata *cmd,
			    unsigned nr_bits,
			    unsigned long *bits)
{
	int r;

	WRITE_LOCK(cmd);
	r = __set_dirty_bits_v2(cmd, nr_bits, bits);
	WRITE_UNLOCK(cmd);

	return r;
}

static int __set_sub_dirty_bits(struct dm_cache_metadata *cmd, unsigned nr_bits, unsigned long *bits)
{
	int r = 0;

	r = dm_bitset_del(&cmd->sub_dirty_info, cmd->sub_dirty_root);
	if (r)
		return r;

	cmd->changed = true;
	return dm_bitset_new(&cmd->sub_dirty_info, &cmd->sub_dirty_root, nr_bits, is_dirty_callback, bits);
}

int dm_cache_set_sub_dirty_bits(struct dm_cache_metadata *cmd,
			        unsigned nr_bits,
			        unsigned long *bits)
{
	int r;

	WRITE_LOCK(cmd);
	r = __set_sub_dirty_bits(cmd, nr_bits, bits);
	WRITE_UNLOCK(cmd);

	return r;
}

int dm_cache_set_sub_valid_bits(struct dm_cache_metadata *cmd,
                                uint32_t index,
                                unsigned nr_bits,
                                unsigned long *bits,
                                bool invalid)
{
	int r;

	WRITE_LOCK(cmd);
	if (invalid)
		r = dm_bitset_clear_bits(&cmd->sub_valid_info,
		                         cmd->sub_valid_root,
		                         index, bits, nr_bits,
		                         &cmd->sub_valid_root);
	else
		r = dm_bitset_set_bits(&cmd->sub_valid_info,
		                       cmd->sub_valid_root,
		                       index, bits, nr_bits,
		                       &cmd->sub_valid_root);
	cmd->changed = true;
	WRITE_UNLOCK(cmd);

	return r;
}

void dm_cache_client_metadata_get_stats(struct dm_cache_client_metadata *md,
				        struct dm_cache_statistics *stats)
{
	READ_LOCK_VOID(md->cmd);
	*stats = md->stats;
	READ_UNLOCK(md->cmd);
}

void dm_cache_client_metadata_set_stats(struct dm_cache_client_metadata *md,
					struct dm_cache_statistics *stats)
{
	WRITE_LOCK_VOID(md->cmd);
	md->stats = *stats;
	if (!md->changed)
		md->changed = true;
	WRITE_UNLOCK(md->cmd);
}

int dm_cache_commit(struct dm_cache_metadata *cmd, bool clean_shutdown)
{
	int r = -EINVAL;
	struct dm_cache_client_metadata *md, *tmp;
	flags_mutator mutator = (clean_shutdown ? set_clean_shutdown :
				 clear_clean_shutdown);

	WRITE_LOCK_CORE(cmd);
	if (cmd->fail_io)
		goto out;

	r = __commit_transaction(cmd, mutator);
	if (r)
		goto out;

	r = __begin_transaction(cmd);
	if (r)
		goto out;

	list_for_each_entry_safe(md, tmp, &cmd->clients, list)
		read_client_md(md);
out:
	WRITE_UNLOCK(cmd);
	return r;
}

int dm_cache_get_free_metadata_block_count(struct dm_cache_metadata *cmd,
					   dm_block_t *result)
{
	int r = -EINVAL;

	READ_LOCK(cmd);
	if (!cmd->fail_io)
		r = dm_sm_get_nr_free(cmd->metadata_sm, result);
	READ_UNLOCK(cmd);

	return r;
}

int dm_cache_get_metadata_dev_size(struct dm_cache_metadata *cmd,
				   dm_block_t *result)
{
	int r = -EINVAL;

	READ_LOCK(cmd);
	if (!cmd->fail_io)
		r = dm_sm_get_nr_blocks(cmd->metadata_sm, result);
	READ_UNLOCK(cmd);

	return r;
}

/*----------------------------------------------------------------*/

static int get_hint(uint32_t index, void *value_le, void *context)
{
	uint32_t value;
	struct dm_cache_policy *policy = context;

	value = policy_get_hint(policy, to_cblock(index));
	*((__le32 *) value_le) = cpu_to_le32(value);

	return 0;
}

/*
 * It's quicker to always delete the hint array, and recreate with
 * dm_array_new().
 */
static int write_hints(struct dm_cache_client_metadata *ccmd, struct dm_cache_policy *policy)
{
	int r;
	size_t hint_size;
	struct dm_cache_metadata *cmd = ccmd->cmd;
	const char *policy_name = dm_cache_policy_get_name(policy);
	const unsigned *policy_version = dm_cache_policy_get_version(policy);

	if (!policy_name[0] ||
	    (strlen(policy_name) > sizeof(ccmd->policy_name) - 1))
		return -EINVAL;

	strncpy(ccmd->policy_name, policy_name, sizeof(ccmd->policy_name));
	memcpy(ccmd->policy_version, policy_version, sizeof(ccmd->policy_version));

	hint_size = dm_cache_policy_get_hint_size(policy);
	if (!hint_size)
		return 0; /* short-circuit hints initialization */

	if (cmd->policy_hint_size && cmd->policy_hint_size != hint_size)
		return -EINVAL;

	cmd->policy_hint_size = hint_size;
	if (cmd->hint_root) {
		r = dm_array_del(&cmd->hint_info, cmd->hint_root);
		if (r)
			return r;
	}

	return dm_array_new(&cmd->hint_info, &cmd->hint_root,
			    from_cblock(cmd->cache_blocks),
			    get_hint, policy);
}

int dm_cache_client_write_hints(struct dm_cache_client_metadata *ccmd,
                                struct dm_cache_policy *policy)
{
	int r;

	WRITE_LOCK(ccmd->cmd);
	r = write_hints(ccmd, policy);
	WRITE_UNLOCK(ccmd->cmd);

	return r;
}

int dm_cache_metadata_all_clean(struct dm_cache_metadata *cmd, bool *result)
{
	int r;

	READ_LOCK(cmd);
	r = blocks_are_unmapped_or_clean(cmd, 0, cmd->cache_blocks, result);
	READ_UNLOCK(cmd);

	return r;
}

int dm_cache_client_metadata_all_clean(struct dm_cache_client_metadata *ccmd, bool *result)
{
	int r;

	READ_LOCK(ccmd->cmd);
	r = client_blocks_are_clean(ccmd, 0, ccmd->cmd->cache_blocks, result);
	READ_UNLOCK(ccmd->cmd);

	return r;
}

void dm_cache_metadata_set_read_only(struct dm_cache_metadata *cmd)
{
	WRITE_LOCK_VOID(cmd);
	dm_bm_set_read_only(cmd->bm);
	WRITE_UNLOCK(cmd);
}

void dm_cache_metadata_set_read_write(struct dm_cache_metadata *cmd)
{
	WRITE_LOCK_VOID(cmd);
	dm_bm_set_read_write(cmd->bm);
	WRITE_UNLOCK(cmd);
}

int dm_cache_metadata_set_needs_check(struct dm_cache_metadata *cmd)
{
	int r;
	struct dm_block *sblock;
	struct cache_disk_superblock *disk_super;

	WRITE_LOCK(cmd);
	set_bit(NEEDS_CHECK, &cmd->flags);

	r = superblock_lock(cmd, &sblock);
	if (r) {
		DMERR("couldn't read superblock");
		goto out;
	}

	disk_super = dm_block_data(sblock);
	disk_super->flags = cpu_to_le32(cmd->flags);

	dm_bm_unlock(sblock);

out:
	WRITE_UNLOCK(cmd);
	return r;
}

int dm_cache_metadata_needs_check(struct dm_cache_metadata *cmd, bool *result)
{
	READ_LOCK(cmd);
	*result = !!test_bit(NEEDS_CHECK, &cmd->flags);
	READ_UNLOCK(cmd);

	return 0;
}

int dm_cache_metadata_abort(struct dm_cache_metadata *cmd)
{
	int r;

	WRITE_LOCK(cmd);
	__destroy_persistent_data_objects(cmd);
	r = __create_persistent_data_objects(cmd, false);
	if (r)
		cmd->fail_io = true;
	WRITE_UNLOCK(cmd);

	return r;
}
