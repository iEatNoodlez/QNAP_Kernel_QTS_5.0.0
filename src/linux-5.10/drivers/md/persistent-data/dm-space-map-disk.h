/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This file is released under the GPL.
 */

#ifndef _LINUX_DM_SPACE_MAP_DISK_H
#define _LINUX_DM_SPACE_MAP_DISK_H

#include "dm-block-manager.h"
#include "dm-space-map.h"

struct dm_space_map;
struct dm_transaction_manager;

/*
 * Unfortunately we have to use two-phase construction due to the cycle
 * between the tm and sm.
 */
struct dm_space_map *dm_sm_disk_create(struct dm_transaction_manager *tm,
				       dm_block_t nr_blocks,
				       struct free_map *free_map);

struct dm_space_map *dm_sm_disk_open(struct dm_transaction_manager *tm,
				     void *root_le, size_t len,
				     struct free_map *free_map);

#endif /* _LINUX_DM_SPACE_MAP_DISK_H */
