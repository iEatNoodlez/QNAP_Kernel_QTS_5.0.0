/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#ifndef DM_CACHE_UTIL_H
#define DM_CACHE_UTIL_H

#include "dm-cache-block-types.h"

#include <linux/device-mapper.h>

struct entry {
	unsigned hash_next:28;
	unsigned prev:28; 
	unsigned next:28;
	unsigned level:6;
	unsigned pending:12;
	unsigned allocator:4;
	unsigned padding:4;
	bool dirty:1;
	bool allocated:1;
	bool hashed:1;
	bool sentinel:1;

	dm_oblock_t oblock;
};

void init_entry(struct entry *e);

#define NR_SENTINELS 2

struct ilist {
	unsigned nr_elts;	/* excluding sentinel entries */
	unsigned head, tail;
	bool current_sentinel;
	unsigned sentinel[NR_SENTINELS];
};

struct entry_indexer {
	unsigned (*to_index)(void *, struct entry *);
	struct entry* (*to_entry)(void *, unsigned index);

	void *context;
};

struct entry *indexer_to_entry(struct entry_indexer *ei, unsigned index);
unsigned indexer_to_index(struct entry_indexer *ei, struct entry *e);

void l_init(struct ilist *l);
bool l_empty(struct ilist *l);
unsigned l_nr_sentinels(struct ilist *l);
struct entry *l_head(struct entry_indexer *i, struct ilist *l);
struct entry *l_tail(struct entry_indexer *i, struct ilist *l);
struct entry *l_next(struct entry_indexer *i, struct entry *e);
struct entry *l_prev(struct entry_indexer *i, struct entry *e);
struct entry *l_pop_head(struct entry_indexer *i, struct ilist *l);
struct entry *l_pop_tail(struct entry_indexer *i, struct ilist *l);
struct entry *l_current_sentinel(struct entry_indexer *i, struct ilist *l);
void l_del(struct entry_indexer *i, struct ilist *l, struct entry *e);
void l_add_tail(struct entry_indexer *i, struct ilist *l, struct entry *e);
void l_add_head(struct entry_indexer *i, struct ilist *l, struct entry *e);
void l_add_before(struct entry_indexer *i, struct ilist *l,
                  struct entry *old, struct entry *e);
void l_add_before_sentinel(struct entry_indexer *i, struct ilist *l,
                           bool last_sentinel, struct entry *e);
void l_update_sentinel(struct entry_indexer *i, struct ilist *l);
void l_swap(struct entry_indexer *i, struct ilist *l,
	    struct entry *e1, struct entry *e2);
void l_splice(struct entry_indexer *i, struct ilist *l1, struct ilist *l2);


struct entry_space {
	struct entry *begin;
	struct entry *end;
	bool private;
};

int es_init(struct entry_space *es, unsigned nr_entries);
void es_exit(struct entry_space *es);

void es_indexer_init(struct entry_indexer *i, struct entry_space *es);

struct entry_alloc {
	struct entry_indexer *ei;
	unsigned begin;
	unsigned length;
	unsigned nr_allocated;
	struct ilist free;
};

#define PRIVATE_MASK (1u << 28u)
#define INDEXER_NULL ((1u << 28u) - 1u)

struct entry *ea_alloc_entry(struct entry_alloc *ea);
struct entry *ea_alloc_particular_entry(struct entry_alloc *ea, unsigned i);
void ea_take_particular_entry(struct entry_alloc *ea, unsigned i);
bool ea_entry_taken(struct entry_alloc *ea, unsigned i);
void ea_free_entry(struct entry_alloc *ea, struct entry *e);
bool ea_empty(struct entry_alloc *ea);
void ea_init(struct entry_alloc *ea, struct entry_indexer *ei,
             unsigned begin, unsigned end);
unsigned ea_get_index(struct entry_alloc *ea, struct entry *e);
struct entry *ea_get_entry(struct entry_alloc *ea, unsigned index);
/*----------------------------------------------------------------*/
#endif	/* DM_CACHE_UTIL_H */
