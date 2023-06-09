/*
 * Copyright (C) 2015 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */


#include "dm.h"
#include "dm-cache-policy-util.h"

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX   "cache util"


inline struct entry *indexer_to_entry(struct entry_indexer *ei, unsigned index)
{
	return ei->to_entry(ei->context, index);
}

inline unsigned indexer_to_index(struct entry_indexer *ei, struct entry *e)
{
	return ei->to_index(ei->context, e);
}

/*
 * An entry_space manages a set of entries that we use for the queues.
 * The clean and dirty queues share entries, so this object is separate
 * from the queue itself.
 */

int es_init(struct entry_space *es, unsigned nr_entries)
{
	if (!nr_entries) {
		es->begin = es->end = NULL;
		return 0;
	}

	es->begin = vzalloc(array_size(nr_entries, sizeof(struct entry)));
	if (!es->begin)
		return -ENOMEM;

	es->end = es->begin + nr_entries;

	return 0;
}

void es_exit(struct entry_space *es)
{
	vfree(es->begin);
}

static struct entry *es_to_entry(void *context, unsigned index)
{
	struct entry *e;
	struct entry_space *es = context;

	if (index == INDEXER_NULL)
		return NULL;

	e = es->begin + index;
	BUG_ON(e >= es->end);

	return e;
}

static unsigned es_to_index(void *context, struct entry *e)
{
	struct entry_space *es = context;

	BUG_ON(e < es->begin || e >= es->end);

	return e - es->begin;
}

void es_indexer_init(struct entry_indexer *i, struct entry_space *es)
{
	i->to_entry = es_to_entry;
	i->to_index = es_to_index;
	i->context = es;
}

void init_entry(struct entry *e)
{
	/*
	 * We can't memset because that would clear the hotspot and
	 * sentinel bits which remain constant.
	 */
	e->hash_next = INDEXER_NULL;
	e->next = INDEXER_NULL;
	e->prev = INDEXER_NULL;
	e->level = 0u;
	e->dirty = true;	/* FIXME: audit */
	e->allocated = true;
	e->pending = 0;
	e->hashed = false;
	e->sentinel = false;
}

struct entry *ea_alloc_entry(struct entry_alloc *ea)
{
	struct entry *e;

	if (l_empty(&ea->free))
		return NULL;

	e = l_pop_head(ea->ei, &ea->free);
	init_entry(e);
	ea->nr_allocated++;

	return e;
}

/*
 * This assumes the cblock hasn't already been allocated.
 */
struct entry *ea_alloc_particular_entry(struct entry_alloc *ea, unsigned i)
{
	struct entry *e = ea->ei->to_entry(ea->ei->context, ea->begin + i);

	init_entry(e);
	return e;
}

void ea_take_particular_entry(struct entry_alloc *ea, unsigned i)
{
	struct entry *e = ea->ei->to_entry(ea->ei->context, ea->begin + i);

	BUG_ON(e->allocated);

	l_del(ea->ei, &ea->free, e);
	init_entry(e);
	ea->nr_allocated++;
}

bool ea_entry_taken(struct entry_alloc *ea, unsigned i)
{
	struct entry *e = ea->ei->to_entry(ea->ei->context, ea->begin + i);

	return e->allocated;
}

void ea_free_entry(struct entry_alloc *ea, struct entry *e)
{
	BUG_ON(!ea->nr_allocated);
	BUG_ON(!e->allocated);

	ea->nr_allocated--;
	e->allocated = false;
	l_add_tail(ea->ei, &ea->free, e);
}

bool ea_empty(struct entry_alloc *ea)
{
	return l_empty(&ea->free);
}

unsigned ea_get_index(struct entry_alloc *ea, struct entry *e)
{
	return indexer_to_index(ea->ei, e) - ea->begin;
}

struct entry *ea_get_entry(struct entry_alloc *ea, unsigned index)
{
	return indexer_to_entry(ea->ei, ea->begin + index);
}

void ea_init(struct entry_alloc *ea, struct entry_indexer *ei,
             unsigned begin, unsigned end)
{
	unsigned i;

	ea->ei = ei;
	ea->nr_allocated = 0u;
	ea->begin = begin;
	ea->length = end - begin;

	l_init(&ea->free);
	for (i = begin; i != end; i++)
		l_add_tail(ei, &ea->free, indexer_to_entry(ei, i));
}

void l_init(struct ilist *l)
{
	int i;

	l->nr_elts = 0;
	l->head = l->tail = INDEXER_NULL;
	l->current_sentinel = false;

	for (i = 0; i < NR_SENTINELS; i++)
		l->sentinel[i] = INDEXER_NULL;
}

struct entry *l_head(struct entry_indexer *i, struct ilist *l)
{
	if (l->head == INDEXER_NULL)
		return NULL;

	return indexer_to_entry(i, l->head);
}

struct entry *l_tail(struct entry_indexer *i, struct ilist *l)
{
	if (l->tail == INDEXER_NULL)
		return NULL;

	return indexer_to_entry(i, l->tail);
}

struct entry *l_next(struct entry_indexer *i, struct entry *e)
{
	if (e->next == INDEXER_NULL)
		return NULL;

	return indexer_to_entry(i, e->next);
}

struct entry *l_prev(struct entry_indexer *i, struct entry *e)
{
	if (e->prev == INDEXER_NULL)
		return NULL;

	return indexer_to_entry(i, e->prev);
}

struct entry *l_current_sentinel(struct entry_indexer *i, struct ilist *l)
{
	return indexer_to_entry(i, l->sentinel[l->current_sentinel]);
}

struct entry *l_last_sentinel(struct entry_indexer *i, struct ilist *l)
{
	return indexer_to_entry(i, l->sentinel[!l->current_sentinel]);
}

bool l_empty(struct ilist *l)
{
	if (!l->nr_elts) {
		BUG_ON(l->head != l->tail);
		BUG_ON(l->head != INDEXER_NULL);
		BUG_ON(l->sentinel[0] != INDEXER_NULL);
		BUG_ON(l->sentinel[1] != INDEXER_NULL);
	}

	return !l->nr_elts;
}

static void __l_add_tail(struct entry_indexer *i, struct ilist *l, struct entry *e)
{
	struct entry *tail = l_tail(i, l);
	unsigned index = i->to_index(i->context, e);

	e->next = INDEXER_NULL;
	e->prev = l->tail;

	if (l_empty(l))
		l->head = l->tail = index;
	else
		tail->next = l->tail = index;

	l->nr_elts++;
}

static void __appoint_sentinel(struct entry_indexer *i, struct ilist *l,
                       struct entry *e, int sid)
{
	if (l->sentinel[sid] != INDEXER_NULL) {
		struct entry *oe;

		oe = indexer_to_entry(i, l->sentinel[sid]);
		oe->sentinel = false;
		l->sentinel[sid] = INDEXER_NULL;
	}

	if (e && !e->sentinel) {
		e->sentinel = true;
		// FIXME: take a look what if entry does not exist
		l->sentinel[sid] = indexer_to_index(i, e);
	}
}

void l_update_sentinel(struct entry_indexer *i, struct ilist *l)
{
	l->current_sentinel = !l->current_sentinel;
	__appoint_sentinel(i, l, NULL, l->current_sentinel);
}

void l_add_tail(struct entry_indexer *i, struct ilist *l, struct entry *e)
{
	__l_add_tail(i, l, e);
	if (l->sentinel[l->current_sentinel] == INDEXER_NULL)
		__appoint_sentinel(i, l, e, l->current_sentinel);
}

void l_del(struct entry_indexer *i, struct ilist *l, struct entry *e)
{
	int s;
	struct entry *prev = l_prev(i, e);
	struct entry *next = l_next(i, e);

	if (prev)
		prev->next = e->next;
	else
		l->head = e->next;

	if (next)
		next->prev = e->prev;
	else
		l->tail = e->prev;

	for (s = 0; s < NR_SENTINELS; s++) {
		if (l->sentinel[s] != indexer_to_index(i, e))
			continue;

		__appoint_sentinel(i, l, next, s);
		break;
	}

	e->sentinel = false;
	l->nr_elts--;
}

void l_add_head(struct entry_indexer *i, struct ilist *l, struct entry *e)
{
	struct entry *head = l_head(i, l);

	e->next = l->head;
	e->prev = INDEXER_NULL;

	if (head)
		head->prev = l->head = indexer_to_index(i, e);
	else
		l->head = l->tail = indexer_to_index(i, e);

	l->nr_elts++;
}

void l_add_before(struct entry_indexer *i, struct ilist *l,
                  struct entry *old, struct entry *e)
{
	struct entry *prev = l_prev(i, old);

	e->prev = old->prev;
	e->next = indexer_to_index(i, old);
	old->prev = indexer_to_index(i, e);
	if (prev)
		prev->next = old->prev;
	else
		l->head = old->prev;

	l->nr_elts++;

	if (l_current_sentinel(i, l) == old && !l_last_sentinel(i, l))
		__appoint_sentinel(i, l, e, !l->current_sentinel);
}

void l_add_before_sentinel(struct entry_indexer *i, struct ilist *l,
			   bool last_sentinel, struct entry *e)
{
	struct entry *sent;

	sent = i->to_entry(i->context, l->sentinel[l->current_sentinel ^ last_sentinel]);
	if (!sent) {
		if (!last_sentinel) {
			__l_add_tail(i, l, e);
			__appoint_sentinel(i, l, NULL, l->current_sentinel);
		} else {
			l_add_head(i, l, e);
		}

		return;
	}

	l_add_before(i, l, sent, e);
}

//FIXME: this should fix sentinel handling
void l_splice(struct entry_indexer *i, struct ilist *l1, struct ilist *l2)
{
	struct entry *l1_tail= l_tail(i, l1);
	struct entry *l2_head = l_head(i, l2);

	if (!l2->nr_elts)
		return;

	if (!l_head(i, l1)) {
		/* l1 empty */
		l1->head = indexer_to_index(i, l2_head);
	} else {
		/* l1 not empty */
		l1_tail->next = indexer_to_index(i, l2_head);
		l2_head->prev = indexer_to_index(i, l1_tail);
	}

	l1->tail = l2->tail;
	l1->nr_elts += l2->nr_elts;
}

struct entry *l_pop_head(struct entry_indexer *i, struct ilist *l)
{
	struct entry *e = l_head(i, l);

	if (e)
		l_del(i, l, e);

	return e;
}

struct entry *l_pop_tail(struct entry_indexer *i, struct ilist *l)
{
	struct entry *e = l_tail(i, l);

	if (e)
		l_del(i, l, e);

	return e;
}
