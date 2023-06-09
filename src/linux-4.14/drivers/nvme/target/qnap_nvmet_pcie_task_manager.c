/*
 * drivers/misc/al/nvme_bridge/al_nvme_task_manager.c
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

#include "qnap_nvmet_pcie_task_manager.h"
#include "qnap_nvmet_pcie_api.h"
#include "qnap_nvmet_pcie.h"

static int _nvme_tm_exec_context(void *data)
{
	struct al_nvme_tm_context *ctx = data;
	struct al_nvme_tm_task *task;

	while (!kthread_should_stop()) {
        //wait_event_interruptible(ctx->wq, atomic_read(&ctx->nvmet_cmd_cnt));
		list_for_each_entry(task, &ctx->task_list, list) {
            task->handler(task->priv, task);
		}
		/* TODO use cpuisol/cpusets to better hold on to the cpu */
//        wait_event_interruptible_timeout(ctx->wq, (ctx->wake_up_flag == 1), 
//                                                    msecs_to_jiffies(500));
//        wait_event_interruptible(ctx->wq, (ctx->wake_up_flag == 1) ||
//                                           (ctx->wake_up_flag2 == 1));

//        wait_event_interruptible(ctx->wq, (ctx->wake_up_flag == 1) || 
//                                          (ctx->fetch_flag == 1) ||
//                                          (ctx->exec_flag == 1) ||
//                                          (ctx->comp_flag == 1));
//        wait_event_interruptible(ctx->wq, atomic_read(&ctx->nvmet_cmd_cnt));
        wait_event_interruptible_timeout(ctx->wq, atomic_read(&ctx->nvmet_cmd_cnt), 
                                                            msecs_to_jiffies(500));
//        schedule();
        //ssleep(0.5); // msleep -> schedule_timeout_uninterruptible
        //udelay(10);
        //msleep(1);
	}
	return 0;
}

struct al_nvme_task_manager *al_nvme_tm_create(const struct cpumask *cpu_mask)
{
	int cpu;
	struct al_nvme_task_manager *tm = NULL;
	struct al_nvme_tm_context *ctx;

	if (cpumask_weight(cpu_mask) == 0) {
		pr_err("%s: no cpus are selected in mask\n", __func__);
		goto error;
	}

	tm = al_nvme_zalloc(sizeof(struct al_nvme_task_manager));
	if (IS_ERR(tm)) {
		pr_err("%s: Could not create task manager\n", __func__);
		goto error;
	}

	/* always make enough room for all possible cpus */
	cpumask_copy(&tm->cpu_mask, cpu_mask);
	tm->cpu_ctx = al_nvme_calloc(num_possible_cpus(), sizeof(*tm->cpu_ctx));
	if (IS_ERR(tm->cpu_ctx)) {
		pr_err("%s: Could not allocate contexts\n", __func__);
		goto error;
	}

	/* create context per-cpu */
	for_each_cpu(cpu, cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];
        init_waitqueue_head(&ctx->wq);
        atomic_set(&ctx->nvmet_cmd_cnt, 0);

		spin_lock_init(&ctx->exec_queue_lock);
		spin_lock_init(&ctx->comp_queue_lock);
		spin_lock_init(&ctx->bridge_queue_lock);
#ifdef CONFIG_NVME_TARGET_QNAP_PCIE_RWBUF
		spin_lock_init(&ctx->cpy_queue_lock);
#endif
		INIT_LIST_HEAD(&ctx->task_list);
	}

	INIT_LIST_HEAD(&tm->queue_list);

	return tm;

error:
	al_nvme_tm_destroy(tm);
	return NULL;
}


static struct al_nvme_tm_task *_find_task(struct al_nvme_task_manager *tm,
					  unsigned int task_id)
{
	int cpu;
	struct al_nvme_tm_context *ctx;
	struct al_nvme_tm_task *task;

	for_each_cpu(cpu, &tm->cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];

		list_for_each_entry(task, &ctx->task_list, list) {
			if (task->task_id == task_id)
				return task;
		}
	}

	return NULL;
}

static struct al_nvme_tm_queue *_find_queue(struct al_nvme_task_manager *tm,
					    unsigned int queue_id)
{
	struct al_nvme_tm_queue *q;

	list_for_each_entry(q, &tm->queue_list, list) {
		if (q->queue_id == queue_id)
			return q;
	}

	return NULL;
}

int al_nvme_tm_start(struct al_nvme_task_manager *tm, u8 ctrl_id)
{
	int cpu;
	struct al_nvme_tm_context *ctx;
	if (!tm || tm->manager_running)
    {
		return -EINVAL;
    }

	for_each_cpu(cpu, &tm->cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];

		ctx->ctx_kthread = kthread_create(_nvme_tm_exec_context,
						  ctx,
						  "ctrl%d_nvme_tm_%u",
						  ctrl_id, cpu);
		if (IS_ERR(ctx->ctx_kthread)) {
			pr_err("%s: Could not create kthread\n", __func__);
			goto error;
		}

		//kthread_bind(ctx->ctx_kthread, cpu);
	}

	for_each_cpu(cpu, &tm->cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];
		wake_up_process(ctx->ctx_kthread);
	}

	tm->manager_running = true;
	return 0;

error:
	al_nvme_tm_stop(tm);
	return -ENOMEM;
}

int al_nvme_tm_stop(struct al_nvme_task_manager *tm)
{
	int cpu;
	struct al_nvme_tm_context *ctx;
	if (!tm)
		return -EINVAL;

	for_each_cpu(cpu, &tm->cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];
		if (ctx->ctx_kthread) {
			kthread_stop(ctx->ctx_kthread);
			ctx->ctx_kthread = NULL;
		}
	}
	tm->manager_running = false;
	return 0;
}

void al_nvme_tm_destroy(struct al_nvme_task_manager *tm)
{
	int cpu;
	struct al_nvme_tm_context *ctx;
	struct al_nvme_tm_task *task, *task_temp;
	struct al_nvme_tm_queue *q, *q_temp;

	if (!tm || tm->manager_running)
		return;

	for_each_cpu(cpu, &tm->cpu_mask) {
		ctx = &tm->cpu_ctx[cpu];

		list_for_each_entry_safe(task, task_temp,
					 &ctx->task_list, list) {
			list_del(&task->list);
			al_nvme_free(task);
		}
	}
	al_nvme_free(tm->cpu_ctx);

	list_for_each_entry_safe(q, q_temp, &tm->queue_list, list) {
        if (&q->ring) {
		    al_nvme_reqs_ring_free(&q->ring);
        }

		list_del(&q->list);
		al_nvme_free(q);
	}

	al_nvme_free(tm);
}

int al_nvme_tm_task_create(struct al_nvme_task_manager *tm, unsigned int cpu,
			   al_nvme_tm_handler_t handler, void *priv)
{
	struct al_nvme_tm_context *ctx;
	struct al_nvme_tm_task *task;
	if (!tm || !handler || !cpumask_test_cpu(cpu, &tm->cpu_mask))
		return -EINVAL;

	if (tm->manager_running)
		return -EACCES;

	/* create a new task, add it to context */
	ctx = &tm->cpu_ctx[cpu];

	task = al_nvme_zalloc(sizeof(*task));
	if (IS_ERR(task))
		return -ENOMEM;
	task->task_id = tm->next_task_id++;
	task->handler = handler;
	task->priv = priv;

	INIT_LIST_HEAD(&task->list);

	list_add_tail(&task->list, &ctx->task_list);

	return task->task_id;
}

int al_nvme_tm_task_param_set(struct al_nvme_task_manager *tm,
			      unsigned int task_id, unsigned int param,
			      unsigned int value)
{
	struct al_nvme_tm_task *task;
	if (!tm || task_id >= tm->next_task_id ||
	    param >= AL_NVME_TASK_PARAMS_MAX)
		return -EINVAL;

	task = _find_task(tm, task_id);
	BUG_ON(!task);

	task->params[param] = value;

	return 0;
}

int al_nvme_tm_queue_create(struct al_nvme_task_manager *tm,
			    unsigned int size)
{
	struct al_nvme_tm_queue *q;
	if (!tm || !size)
		return -EINVAL;

	if (tm->manager_running)
		return -EACCES;

	q = al_nvme_zalloc(sizeof(*q));
	if (IS_ERR(q))
		goto err;

	/* in ring buffer implementation, one request is unusable */
	if (al_nvme_reqs_ring_init(&q->ring, size + 1))
		goto err_ring;

	q->queue_id = tm->next_queue_id++;
	INIT_LIST_HEAD(&q->list);

	list_add_tail(&q->list, &tm->queue_list);

	return q->queue_id;

err_ring:
	al_nvme_free(q);
err:
	return -ENOMEM;
}

int al_nvme_tm_queue_resize(struct al_nvme_task_manager *tm,
			    int queue_id,
			    unsigned int size)
{
	struct al_nvme_tm_queue *q;
	if (!tm || !size)
		return -EINVAL;
#if 0
	if (tm->manager_running)
		return -EACCES;
#endif
	q = _find_queue(tm, queue_id);
	BUG_ON(!q);

	if (&q->ring) {
        al_nvme_reqs_ring_free(&q->ring);
    }
 	/* in ring buffer implementation, one request is unusable */
	if (al_nvme_reqs_ring_init(&q->ring, size + 1))
		goto err_ring;

	return 0;

err_ring:
	al_nvme_free(q);
	return -ENOMEM;
}

int al_nvme_tm_tasks_queue_in_set(struct al_nvme_task_manager *tm,
				  unsigned int task_id,
				  unsigned int queue_idx,
				  unsigned int queue_id)
{
	struct al_nvme_tm_task *task;
	struct al_nvme_tm_queue *q;

	if (!tm						||
	    task_id >= tm->next_task_id			||
	    queue_idx >= AL_NVME_TASK_QUEUES_MAX	||
	    queue_id >= tm->next_queue_id)
		return -EINVAL;

	task = _find_task(tm, task_id);
	q = _find_queue(tm, queue_id);
	BUG_ON(!task || !q);

	task->in_queues[queue_idx] = q;

	return 0;
}

int al_nvme_tm_tasks_queue_out_set(struct al_nvme_task_manager *tm,
				   unsigned int task_id,
				   unsigned int queue_idx,
				   unsigned int queue_id)
{
	struct al_nvme_tm_task *task;
	struct al_nvme_tm_queue *q;

	if (!tm						||
	    task_id >= tm->next_task_id			||
	    queue_idx >= AL_NVME_TASK_QUEUES_MAX	||
	    queue_id >= tm->next_queue_id)
		return -EINVAL;

	task = _find_task(tm, task_id);
	q = _find_queue(tm, queue_id);
	BUG_ON(!task || !q);

	task->out_queues[queue_idx] = q;

	return 0;
}
