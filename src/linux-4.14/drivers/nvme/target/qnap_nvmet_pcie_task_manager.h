/*
 * drivers/misc/al/nvme_bridge/al_nvme_task_manager.h
 *
 * Annapurna Labs NVMe Bridge Reference driver, Task Manager
 *
 * Copyright (C) 2013 Annapurna Labs Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* The NVMe Task Manager (TM) is a simple infrastructure that allows creating
 * cpu-bound generic tasks and controlling them in runtime.
 * The TM creates one process for each enabled cpu, under which all tasks
 * are executed.
 * Parameters can also be passed to tasks, if required.
 * Tasks communicate via queues (Implemented as ring buffers)- each task has
 * assigned input and output queues, which can be configured by the user.
 */

#ifndef __AL_NVME_TASK_MANAGER_H
#define __AL_NVME_TASK_MANAGER_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/cache.h>
#include <linux/circ_buf.h>
#include <linux/cpumask.h>
#include <asm/barrier.h>
#include <linux/kthread.h>
#include "qnap_nvmet_pcie.h"

/* Max. number of input or output queues per task */
#define AL_NVME_TASK_QUEUES_MAX 8
/* Max. number of parameters per task */
#define AL_NVME_TASK_PARAMS_MAX 8

/* Describes a task handler- priv is defined by the user and is passed on task
 * creation. task is task's private data and should be passed to the task
 * manager API for some queries (parameter get, queue get etc.)
 */
typedef int (*al_nvme_tm_handler_t)(void *priv, void *task);

/* An NVMe TM queue- includes a single ring buffer and a queue id. */
struct al_nvme_tm_queue {
	struct al_reqs_ring ring;
	unsigned int queue_id;
	struct list_head list;
};

/* This structure describes a single task */
struct al_nvme_tm_task {
	unsigned int params[AL_NVME_TASK_PARAMS_MAX]	____cacheline_aligned;
	al_nvme_tm_handler_t handler;
	void *priv;

	unsigned int task_id;

	struct al_nvme_tm_queue
		    *in_queues[AL_NVME_TASK_QUEUES_MAX] ____cacheline_aligned;
	struct al_nvme_tm_queue *out_queues[AL_NVME_TASK_QUEUES_MAX];

	struct list_head list;
};

/* This structure describes a context, where a context is a collection of tasks
 * that are invoked on a specific cpu.
 * The Task Manager creates a context structure for each cpu.
 */
struct al_nvme_tm_context {
	struct task_struct *ctx_kthread;
    wait_queue_head_t wq;
    int wake_up_flag;
    int wake_up_flag2;
    atomic_t nvmet_cmd_cnt;
	spinlock_t exec_queue_lock;
	spinlock_t comp_queue_lock;
	spinlock_t bridge_queue_lock;
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
	spinlock_t cpy_queue_lock;
#endif
	spinlock_t comp_cb_lock;


	struct list_head task_list;
} ____cacheline_aligned_in_smp;

struct al_nvme_task_manager {
	struct al_nvme_tm_context *cpu_ctx;
	struct cpumask cpu_mask;
	bool manager_running;
	unsigned int next_task_id;
	unsigned int next_queue_id;

	struct list_head queue_list;
};

/**
 * Create a new task manager.
 *
 * @param	cpu_mask A struct cpumask that describes the cpus that will be
 *              used in the new task manager.
 *
 * @return	0 on success
 */
struct al_nvme_task_manager *al_nvme_tm_create(const struct cpumask *cpu_mask);

/**
 * Start the task manager
 *
 * @param	tm Task manager to be started
 *
 * @return	0 on success
 */
int al_nvme_tm_start(struct al_nvme_task_manager *tm, u8 ctrl_id);

/**
 * Stop the task manager
 *
 * @param	tm Task manager to be stopped
 *
 * @return	0 on success
 */
int al_nvme_tm_stop(struct al_nvme_task_manager *tm);

/**
 * Destroy the task manager
 *
 * @param	tm Task manager to be destroyed
 *
 * @return	0 on success
 */
void al_nvme_tm_destroy(struct al_nvme_task_manager *tm);

/**
 * Creates a new task
 *
 * @param	tm Task manager
 * @param	cpu CPU to which this task should be bound
 * @param	handler Task handler
 * @param	priv Private data to be passed to the task on each invocation
 *
 * @return	> 0 - Success, the return value is the task id.
 *              < 0 - Failure.
 */
int al_nvme_tm_task_create(struct al_nvme_task_manager *tm, unsigned int cpu,
			   al_nvme_tm_handler_t handler, void *priv);

/**
 * Set task parameter.
 *
 * @param	tm Task manager
 * @param	task_id Task id, as returned by al_nvme_tm_task_create()
 * @param	param Param index to set.
 * @param	value Param value to set.
 *
 * @return	0 on success
 */
int al_nvme_tm_task_param_set(struct al_nvme_task_manager *tm,
			      unsigned int task_id, unsigned int param,
			      unsigned int value);

/**
 * Creates a new queue
 *
 * @param	tm Task manager
 * @param	size Queue size
 *
 * @return	> 0 - Success, the return value is the queue id.
 *              < 0 - Failure.
 */
int al_nvme_tm_queue_create(struct al_nvme_task_manager *tm,
			    unsigned int size);

/**
 * Resize an existing queue
 *
 * @param	tm Task manager
 * @param	size Queue size
 *
 * @return	0 on success
 */
int al_nvme_tm_queue_resize(struct al_nvme_task_manager *tm,
			    int queue_id,
			    unsigned int size);

/**
 * Sets a task input queue
 *
 * @param	tm Task manager
 * @param	task_id The task's id
 * @param	queue_idx The input queue index to set
 * @param	queue_id The input queue id to set (As returned by
 *		al_nvme_tm_queue_create())
 *
 * @return	0 on success
 */
int al_nvme_tm_tasks_queue_in_set(struct al_nvme_task_manager *tm,
				  unsigned int task_id,
				  unsigned int queue_idx,
				  unsigned int queue_id);

/**
 * Sets a task output queue
 *
 * @param	tm Task manager
 * @param	task_id The task's id
 * @param	queue_idx The output queue index to set
 * @param	queue_id The output queue id to set (As returned by
 *		al_nvme_tm_queue_create())
 *
 * @return	0 on success
 */
int al_nvme_tm_tasks_queue_out_set(struct al_nvme_task_manager *tm,
				   unsigned int task_id,
				   unsigned int queue_idx,
				   unsigned int queue_id);

/**
 * Returns a task's parameter (This API is meant to be called by task handlers)
 *
 * @param	task Task's private data, as passed to the task's handler.
 * @param	param The param's index to return.
 *
 * @return	Parameter value.
 */
static inline unsigned int al_nvme_tm_task_param_get(void *task,
						     unsigned int param)
{
	struct al_nvme_tm_task *_task = task;
	return _task->params[param];
}

/**
 * Push a request to given output queue
 *
 * @param	task Task's private data, as passed to the task's handler.
 * @param	queue_idx Output queue index.
 * @param	req The request to be pushed.
 *
 */
static inline void al_nvme_tm_queue_push(void *task, unsigned int queue_idx,
					 struct al_nvme_req *req)
{
	struct al_nvme_tm_task *_task = task;
	struct al_reqs_ring *ring = &_task->out_queues[queue_idx]->ring;

	unsigned int tail = ACCESS_ONCE(ring->tail);
	unsigned int head = ring->head;

	if (likely(CIRC_SPACE(head, tail, ring->size))) {
		ring->reqs[head] = req;

		/* make sure request is written to head before advancing
		 * the pointer
		 */
		AL_NVME_WMB();

		ring->head = (head + 1) & ring->size_mask;
	} else {
		/* TODO error handling */
		BUG();
	}
}

/**
 * Get one (or more) requests from a given input queue
 *
 * @param	task Task's private data, as passed to the task's handler.
 * @param	queue_idx Output queue index.
 * @param	reqs An array of requests.
 * @param	num Max. number of requests to be returned.
 *
 * @return	Returns the number of requests pulled from the queue.
 *
 */
static inline unsigned int al_nvme_tm_queue_get(void *task,
						unsigned int queue_idx,
						struct al_nvme_req **reqs,
						unsigned int num)
{
	struct al_nvme_tm_task *_task = task;
	struct al_reqs_ring *ring = &_task->in_queues[queue_idx]->ring;
	unsigned int head = ACCESS_ONCE(ring->head);
	unsigned int tail = ring->tail;
	unsigned int i;
	num = min_t(unsigned int, num, CIRC_CNT(head, tail, ring->size));

	if (num == 0)
		goto done;

	for (i = 0; i < num; ++i)
		reqs[i] = ring->reqs[(tail + i) & ring->size_mask];

	/* make sure the request was read before advancing the
	 * pointer
	 */
	AL_NVME_MB();

	ring->tail = (tail + num) & ring->size_mask;

done:
	return num;
}

#endif /* __AL_NVME_TASK_MANAGER_H */
