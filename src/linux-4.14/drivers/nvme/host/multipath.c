/*
 * Copyright (c) 2017 Christoph Hellwig.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/moduleparam.h>
#include "nvme.h"

static bool multipath = true;
module_param(multipath, bool, 0644);
MODULE_PARM_DESC(multipath,
		 "turn on native support for multiple controllers per subsystem");


#define SUBSYS_ATTR_RW(_name, _mode, _show, _store)	\
	struct device_attribute subsys_attr_##_name =	\
		__ATTR(_name, _mode, _show, _store)

static const char *nvme_iopolicy_names[] = {
	[NVME_IOPOLICY_NUMA] = "numa",
	[NVME_IOPOLICY_RR] = "round-robin"
};


static ssize_t nvme_subsys_iopolicy_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);

	return sprintf(buf,
		       "%s\n",
		       nvme_iopolicy_names[READ_ONCE(subsys->iopolicy)]);

}

static ssize_t nvme_subsys_iopolicy_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t count)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(nvme_iopolicy_names); i++) {
		if (sysfs_streq(buf, nvme_iopolicy_names[i])) {
			WRITE_ONCE(subsys->iopolicy, i);
			return count;
		}
	}

	return -EINVAL;
}

SUBSYS_ATTR_RW(iopolicy, S_IRUGO | S_IWUSR,
	       nvme_subsys_iopolicy_show, nvme_subsys_iopolicy_store);

void nvme_failover_req(struct request *req)
{
	struct nvme_ns *ns = req->q->queuedata;
	unsigned long flags;

	spin_lock_irqsave(&ns->head->requeue_lock, flags);
	blk_steal_bios(&ns->head->requeue_list, req);
	spin_unlock_irqrestore(&ns->head->requeue_lock, flags);
	blk_mq_end_request(req, 0);

	nvme_reset_ctrl(ns->ctrl);
	kblockd_schedule_work(&ns->head->requeue_work);
}

bool nvme_req_needs_failover(struct request *req)
{
	if (!(req->cmd_flags & REQ_NVME_MPATH))
		return false;

	switch (nvme_req(req)->status & 0x7ff) {
	/*
	 * Generic command status:
	 */
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:
	case NVME_SC_LBA_RANGE:
	case NVME_SC_CAP_EXCEEDED:
	case NVME_SC_RESERVATION_CONFLICT:
		return false;

	/*
	 * I/O command set specific error.  Unfortunately these values are
	 * reused for fabrics commands, but those should never get here.
	 */
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_INVALID_PI:
	case NVME_SC_READ_ONLY:
	case NVME_SC_ONCS_NOT_SUPPORTED:
		WARN_ON_ONCE(nvme_req(req)->cmd->common.opcode ==
			nvme_fabrics_command);
		return false;

	/*
	 * Media and Data Integrity Errors:
	 */
	case NVME_SC_WRITE_FAULT:
	case NVME_SC_READ_ERROR:
	case NVME_SC_GUARD_CHECK:
	case NVME_SC_APPTAG_CHECK:
	case NVME_SC_REFTAG_CHECK:
	case NVME_SC_COMPARE_FAILED:
	case NVME_SC_ACCESS_DENIED:
	case NVME_SC_UNWRITTEN_BLOCK:
		return false;
	}

	/* Everything else could be a path failure, so should be retried */
	return true;
}

void nvme_kick_requeue_lists(struct nvme_ctrl *ctrl)
{
	struct nvme_ns *ns;

	mutex_lock(&ctrl->namespaces_mutex);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (ns->head->disk)
			kblockd_schedule_work(&ns->head->requeue_work);
	}
	mutex_unlock(&ctrl->namespaces_mutex);
}

static struct nvme_ns *__nvme_find_path(struct nvme_ns_head *head)
{
	struct nvme_ns *ns;

	list_for_each_entry_rcu(ns, &head->list, siblings) {
		if (ns->ctrl->state == NVME_CTRL_LIVE) {
			rcu_assign_pointer(head->current_path, ns);
			return ns;
		}
	}

	return NULL;
}

static struct nvme_ns *nvme_next_ns(struct nvme_ns_head *head,
				    struct nvme_ns *ns)
{
	ns = list_next_or_null_rcu(&head->list,
				   &ns->siblings,
				   struct nvme_ns,
				   siblings);
	if (ns)
		return ns;

	return list_first_or_null_rcu(&head->list,
				      struct nvme_ns,
				      siblings);
}

static struct nvme_ns *nvme_round_robin_path(struct nvme_ns_head *head,
					     struct nvme_ns *old)
{
	struct nvme_ns *ns;

	if (list_is_singular(&head->list))
		return old;

	for (ns = nvme_next_ns(head, old);
	     ns != old;
	     ns = nvme_next_ns(head, ns)) {
		if (ns->ctrl->state == NVME_CTRL_LIVE)
			break;
	}

	rcu_assign_pointer(head->current_path, ns);
	return ns;
}

inline struct nvme_ns *nvme_find_path(struct nvme_ns_head *head)
{
	struct nvme_ns *ns = srcu_dereference(head->current_path, &head->srcu);

	if (READ_ONCE(head->subsys->iopolicy) == NVME_IOPOLICY_RR && ns)
		ns = nvme_round_robin_path(head, ns);

	if (unlikely(!ns || ns->ctrl->state != NVME_CTRL_LIVE))
		ns = __nvme_find_path(head);

	return ns;
}

static blk_qc_t nvme_ns_head_make_request(struct request_queue *q,
					  struct bio *bio)
{
	struct nvme_ns_head *head = q->queuedata;
	struct device *dev = disk_to_dev(head->disk);
	struct nvme_ns *ns;
	blk_qc_t ret = BLK_QC_T_NONE;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&head->srcu);

	ns = nvme_find_path(head);
	if (likely(ns)) {
		bio->bi_disk = ns->disk;
		bio->bi_opf |= REQ_NVME_MPATH;
		ret = direct_make_request(bio);
	} else if (!list_empty_careful(&head->list)) {
		dev_warn_ratelimited(dev, "no path available - requeing I/O\n");
		spin_lock_irq(&head->requeue_lock);
		bio_list_add(&head->requeue_list, bio);
		spin_unlock_irq(&head->requeue_lock);
	} else {
		dev_warn_ratelimited(dev, "no path - failing I/O\n");
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
	}

	srcu_read_unlock(&head->srcu, srcu_idx);
	return ret;
}

static void nvme_requeue_work(struct work_struct *work)
{
	struct nvme_ns_head *head =
		container_of(work, struct nvme_ns_head, requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&head->requeue_lock);
	next = bio_list_get(&head->requeue_list);
	spin_unlock_irq(&head->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;

		/*
		 * Reset disk to the mpath node and resubmit to select a new
		 * path.
		 */
		bio->bi_disk = head->disk;
		generic_make_request(bio);
	}
}

int nvme_mpath_alloc_disk(struct nvme_ctrl *ctrl, struct nvme_ns_head *head)
{
	struct request_queue *q;
	bool vwc = false;

	bio_list_init(&head->requeue_list);
	spin_lock_init(&head->requeue_lock);
	INIT_WORK(&head->requeue_work, nvme_requeue_work);

	if(!(ctrl->subsys->cmic & (1 << 1)) || !multipath)
		return 0;

	q = blk_alloc_queue_node(GFP_KERNEL, NUMA_NO_NODE);
	if (!q)
		goto out;
	q->queuedata = head;
	blk_queue_make_request(q, nvme_ns_head_make_request);
	blk_set_stacking_limits(&q->limits);
	nvme_set_queue_limits(ctrl, q);

	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, q);
	blk_queue_logical_block_size(q, 512);
	if (ctrl->vwc & NVME_CTRL_VWC_PRESENT)
		vwc = true;
	blk_queue_write_cache(q, vwc, vwc);
	head->disk = alloc_disk(0);
	if (!head->disk)
		goto out_cleanup_queue;
	head->disk->fops = &nvme_ns_head_ops;
	head->disk->private_data = head;
	head->disk->queue = q;
	head->disk->flags = GENHD_FL_EXT_DEVT;
	sprintf(head->disk->disk_name, "nvme%dn%d",
		ctrl->subsys->instance, head->instance);

	return 0;

 out_cleanup_queue:
	blk_cleanup_queue(q);
 out:
	return -ENOMEM;
}

void nvme_mpath_add_disk(struct nvme_ns_head *head)
{
	struct nvme_ns_head **p = NULL;
	if (!head->disk)
		return;
	p = &head;
	device_add_disk(&head->subsys->dev, head->disk);
	if (sysfs_create_group(&disk_to_dev(head->disk)->kobj,
			       &nvme_ns_id_attr_group))
		pr_warn("%s: failed to create sysfs group for identification\n",
			head->disk->disk_name);
}

void nvme_mpath_add_disk_links(struct nvme_ns *ns)
{
	struct kobject *slave_disk_kobj, *holder_disk_kobj;

	if (!ns->head->disk)
		return ;

	slave_disk_kobj = &disk_to_dev(ns->disk)->kobj;
	if (sysfs_create_link(ns->head->disk->slave_dir,
			    slave_disk_kobj,
			    kobject_name(slave_disk_kobj)))
		return ;

	holder_disk_kobj = &disk_to_dev(ns->head->disk)->kobj;
	if (sysfs_create_link(ns->disk->part0.holder_dir,
			    holder_disk_kobj,
			    kobject_name(holder_disk_kobj)))
		sysfs_remove_link(ns->head->disk->slave_dir,
				kobject_name(slave_disk_kobj));

}


void nvme_mpath_remove_disk(struct nvme_ns_head *head)
{
	if (!head->disk)
		return;

	sysfs_remove_group(&disk_to_dev(head->disk)->kobj,
			   &nvme_ns_id_attr_group);
	del_gendisk(head->disk);
	blk_set_queue_dying(head->disk->queue);
	kblockd_schedule_work(&head->requeue_work);
	flush_work(&head->requeue_work);
	blk_cleanup_queue(head->disk->queue);
	put_disk(head->disk);
}

void nvme_mpath_remove_disk_links(struct nvme_ns *ns)
{
	if (!ns->head->disk)
		return;

	sysfs_remove_link(ns->disk->part0.holder_dir,
			  kobject_name(&disk_to_dev(ns->head->disk)->kobj));
	sysfs_remove_link(ns->head->disk->slave_dir,
			  kobject_name(&disk_to_dev(ns->disk)->kobj));
}
