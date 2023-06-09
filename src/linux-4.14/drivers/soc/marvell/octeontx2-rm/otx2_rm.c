// SPDX-License-Identifier: GPL-2.0
/* OcteonTX2 RVU Resource Manager driver
 *
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sysfs.h>

#include "rvu_reg.h"
#include "rvu_struct.h"
#include "otx2_rm.h"

#ifdef CONFIG_OCTEONTX2_RM_DOM_SYSFS
#include "domain_sysfs.h"
#endif

#define DRV_NAME	"octeontx2-rm"
#define DRV_VERSION	"1.0"

#define PCI_DEVID_OCTEONTX2_SSO_PF	0xA0F9
#define PCI_DEVID_OCTEONTX2_SSO_VF	0xA0FA

/* PCI BAR nos */
#define PCI_AF_REG_BAR_NUM		0
#define PCI_CFG_REG_BAR_NUM		2
#define PCI_MBOX_BAR_NUM		4

/* Supported devices */
static const struct pci_device_id rvu_rm_id_table[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVID_OCTEONTX2_SSO_PF)},
	{0} /* end of table */
};

MODULE_AUTHOR("Marvell International Ltd.");
MODULE_DESCRIPTION("Marvell OcteonTX2 SSO/SSOW/TIM/NPA PF Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DEVICE_TABLE(pci, rvu_rm_id_table);

/* All PF devices found are stored here */
static spinlock_t rm_lst_lock;
LIST_HEAD(rm_dev_lst_head);

static void
rm_write64(struct rm_dev *rvu, u64 b, u64 s, u64 o, u64 v)
{
	writeq_relaxed(v, rvu->bar2 + ((b << 20) | (s << 12) | o));
}

static u64 rm_read64(struct rm_dev *rvu, u64 b, u64 s, u64 o)
{
	return readq_relaxed(rvu->bar2 + ((b << 20) | (s << 12) | o));
}

static void enable_af_mbox_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;

	rm = pci_get_drvdata(pdev);
	/* Clear interrupt if any */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_INT, 0x1ULL);

	/* Now Enable AF-PF interrupt */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_INT_ENA_W1S, 0x1ULL);
}

static void disable_af_mbox_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;

	rm = pci_get_drvdata(pdev);
	/* Clear interrupt if any */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_INT, 0x1ULL);

	/* Now Disable AF-PF interrupt */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_INT_ENA_W1C, 0x1ULL);
}

static int
forward_to_mbox(struct rm_dev *rm, struct otx2_mbox *mbox, int devid,
		struct mbox_msghdr *req, int size, const char *mstr)
{
	struct mbox_msghdr *msg;
	int res = 0;

	msg = otx2_mbox_alloc_msg(mbox, devid, size);
	if (msg == NULL)
		return -ENOMEM;

	memcpy((uint8_t *)msg + sizeof(struct mbox_msghdr),
	       (uint8_t *)req + sizeof(struct mbox_msghdr), size);
	msg->id = req->id;
	msg->pcifunc = req->pcifunc;
	msg->sig = req->sig;
	msg->ver = req->ver;

	otx2_mbox_msg_send(mbox, devid);
	res = otx2_mbox_wait_for_rsp(mbox, devid);
	if (res == -EIO) {
		dev_err(&rm->pdev->dev, "RVU %s MBOX timeout.\n", mstr);
		goto err;
	} else if (res) {
		dev_err(&rm->pdev->dev,
			"RVU %s MBOX error: %d.\n", mstr, res);
		res = -EFAULT;
		goto err;
	}

	return 0;
err:
	return res;
}

static int
handle_af_req(struct rm_dev *rm, struct rvu_vf *vf, struct mbox_msghdr *req,
	      int size)
{
	/* We expect a request here */
	if (req->sig != OTX2_MBOX_REQ_SIG) {
		dev_err(&rm->pdev->dev,
			"UP MBOX msg with wrong signature %x, ID 0x%x\n",
			req->sig, req->id);
		return -EINVAL;
	}

	/* If handling notifs in PF is required,add a switch-case here. */
	return forward_to_mbox(rm, &rm->pfvf_mbox_up, vf->vf_id, req, size,
			       "VF");
}


static void rm_afpf_mbox_handler_up(struct work_struct *work)
{
	struct rm_dev *rm = container_of(work, struct rm_dev, mbox_wrk_up);
	struct otx2_mbox *mbox = &rm->afpf_mbox_up;
	struct otx2_mbox_dev *mdev = mbox->dev;
	struct rvu_vf *vf;
	struct mbox_hdr *req_hdr;
	struct mbox_msghdr *msg;
	int offset, id, err;

	/* sync with mbox memory region */
	smp_rmb();

	/* Process received mbox messages */
	req_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	offset = ALIGN(sizeof(*req_hdr), MBOX_MSG_ALIGN);
	for (id = 0; id < req_hdr->num_msgs; id++) {
		msg = (struct mbox_msghdr *)(mdev->mbase + mbox->rx_start +
					     offset);

		if ((msg->pcifunc >> RVU_PFVF_PF_SHIFT) != rm->pf ||
		    (msg->pcifunc & RVU_PFVF_FUNC_MASK) <= rm->num_vfs)
			err = -EINVAL;
		else {
			vf = &rm->vf_info[msg->pcifunc & RVU_PFVF_FUNC_MASK];
			err = handle_af_req(rm, vf, msg,
					    msg->next_msgoff - offset);
		}
		if (err)
			otx2_reply_invalid_msg(mbox, 0, msg->pcifunc, msg->id);
		offset = msg->next_msgoff;
	}

	otx2_mbox_msg_send(mbox, 0);
}

static void rm_afpf_mbox_handler(struct work_struct *work)
{
	struct rm_dev *rm;
	struct mbox_hdr *rsp_hdr;
	struct mbox_msghdr *msg, *fwd;
	struct otx2_mbox *af_mbx, *vf_mbx;
	struct free_rsrcs_rsp *rsp;
	int offset, i, vf_id, size;
	struct rvu_vf *vf;

	/* Read latest mbox data */
	smp_rmb();

	rm = container_of(work, struct rm_dev, mbox_wrk);
	af_mbx = &rm->afpf_mbox;
	vf_mbx = &rm->pfvf_mbox;
	rsp_hdr = (struct mbox_hdr *)(af_mbx->dev->mbase + af_mbx->rx_start);
	if (rsp_hdr->num_msgs == 0)
		return;
	offset = ALIGN(sizeof(struct mbox_hdr), MBOX_MSG_ALIGN);

	for (i = 0; i < rsp_hdr->num_msgs; i++) {
		msg = (struct mbox_msghdr *)(af_mbx->dev->mbase +
					     af_mbx->rx_start + offset);
		size = msg->next_msgoff - offset;

		if (msg->id >= MBOX_MSG_MAX) {
			dev_err(&rm->pdev->dev,
				"MBOX msg with unknown ID 0x%x\n", msg->id);
			goto end;
		}

		if (msg->sig != OTX2_MBOX_RSP_SIG) {
			dev_err(&rm->pdev->dev,
				"MBOX msg with wrong signature %x, ID 0x%x\n",
				msg->sig, msg->id);
			goto end;
		}

		vf_id = (msg->pcifunc & RVU_PFVF_FUNC_MASK);
		if (vf_id > 0) {
			if (vf_id > rm->num_vfs) {
				dev_err(&rm->pdev->dev,
					"MBOX msg to unknown VF: %d >= %d\n",
					vf_id, rm->num_vfs);
				goto end;
			}
			vf = &rm->vf_info[vf_id - 1];
			/* Ignore stale responses and VFs in FLR. */
			if (!vf->in_use || vf->got_flr)
				goto end;
			fwd = otx2_mbox_alloc_msg(vf_mbx, vf_id - 1, size);
			if (!fwd) {
				dev_err(&rm->pdev->dev,
					"Forwarding to VF%d failed.\n", vf_id);
				goto end;
			}
			memcpy((uint8_t *)fwd + sizeof(struct mbox_msghdr),
			       (uint8_t *)msg + sizeof(struct mbox_msghdr),
			       size);
			fwd->id = msg->id;
			fwd->pcifunc = msg->pcifunc;
			fwd->sig = msg->sig;
			fwd->ver = msg->ver;
			fwd->rc = msg->rc;
		} else {
			if (msg->ver < OTX2_MBOX_VERSION) {
				dev_err(&rm->pdev->dev,
					"MBOX msg with version %04x != %04x\n",
					msg->ver, OTX2_MBOX_VERSION);
				goto end;
			}

			switch (msg->id) {
			case MBOX_MSG_READY:
				rm->pf = (msg->pcifunc >> RVU_PFVF_PF_SHIFT) &
					 RVU_PFVF_PF_MASK;
				break;
			case MBOX_MSG_FREE_RSRC_CNT:
				rsp = (struct free_rsrcs_rsp *)msg;
				memcpy(&rm->limits, msg, sizeof(*rsp));
				break;
			default:
				dev_err(&rm->pdev->dev,
					"Unsupported msg %d received.\n",
					msg->id);
				break;
			}
		}
end:
		offset = msg->next_msgoff;
		af_mbx->dev->msgs_acked++;
	}
	otx2_mbox_reset(af_mbx, 0);
}

static int
reply_free_rsrc_cnt(struct rm_dev *rm, struct rvu_vf *vf,
		    struct mbox_msghdr *req, int size)
{
	struct free_rsrcs_rsp *rsp;

	rsp = (struct free_rsrcs_rsp *)otx2_mbox_alloc_msg(&rm->pfvf_mbox,
							   vf->vf_id,
							   sizeof(*rsp));
	if (rsp == NULL)
		return -ENOMEM;

	rsp->hdr.id = MBOX_MSG_FREE_RSRC_CNT;
	rsp->hdr.pcifunc = req->pcifunc;
	rsp->hdr.sig = OTX2_MBOX_RSP_SIG;
	mutex_lock(&rm->lock);
	rsp->sso = rm->vf_limits.sso->a[vf->vf_id].val;
	rsp->ssow = rm->vf_limits.ssow->a[vf->vf_id].val;
	rsp->npa = rm->vf_limits.npa->a[vf->vf_id].val;
	rsp->tim = rm->vf_limits.tim->a[vf->vf_id].val;
	rsp->nix = 0;
	rsp->cpt = 0;
	mutex_unlock(&rm->lock);
	return 0;
}

static int
check_attach_rsrcs_req(struct rm_dev *rm, struct rvu_vf *vf,
		       struct mbox_msghdr *req, int size)
{
	struct rsrc_attach *rsrc_req;

	rsrc_req = (struct rsrc_attach *)req;
	mutex_lock(&rm->lock);
	if (rsrc_req->sso > rm->vf_limits.sso->a[vf->vf_id].val ||
	    rsrc_req->ssow > rm->vf_limits.ssow->a[vf->vf_id].val ||
	    rsrc_req->npalf > rm->vf_limits.npa->a[vf->vf_id].val ||
	    rsrc_req->timlfs > rm->vf_limits.tim->a[vf->vf_id].val ||
	    rsrc_req->nixlf > 0) {
		dev_err(&rm->pdev->dev,
			"Invalid ATTACH_RESOURCES request from %s\n",
			dev_name(&vf->pdev->dev));
		mutex_unlock(&rm->lock);
		return -EINVAL;
	}
	mutex_unlock(&rm->lock);
	return forward_to_mbox(rm, &rm->afpf_mbox, 0, req, size, "AF");
}

static int
handle_vf_req(struct rm_dev *rm, struct rvu_vf *vf, struct mbox_msghdr *req,
	      int size)
{
	int err = 0;

	/* Check if valid, if not reply with a invalid msg */
	if (req->sig != OTX2_MBOX_REQ_SIG) {
		dev_err(&rm->pdev->dev,
			"VF MBOX msg with wrong signature %x, ID 0x%x\n",
			req->sig, req->id);
		return -EINVAL;
	}

	switch (req->id) {
	case MBOX_MSG_READY:
		if (req->ver < OTX2_MBOX_VERSION) {
			dev_err(&rm->pdev->dev,
				"VF MBOX msg with version %04x != %04x\n",
				req->ver, OTX2_MBOX_VERSION);
			return -EINVAL;
		}
		vf->in_use = true;
		err = forward_to_mbox(rm, &rm->afpf_mbox, 0, req, size, "AF");
		break;
	case MBOX_MSG_FREE_RSRC_CNT:
		if (req->ver < OTX2_MBOX_VERSION) {
			dev_err(&rm->pdev->dev,
				"VF MBOX msg with version %04x != %04x\n",
				req->ver, OTX2_MBOX_VERSION);
			return -EINVAL;
		}
		err = reply_free_rsrc_cnt(rm, vf, req, size);
		break;
	case MBOX_MSG_ATTACH_RESOURCES:
		if (req->ver < OTX2_MBOX_VERSION) {
			dev_err(&rm->pdev->dev,
				"VF MBOX msg with version %04x != %04x\n",
				req->ver, OTX2_MBOX_VERSION);
			return -EINVAL;
		}
		err = check_attach_rsrcs_req(rm, vf, req, size);
		break;
	default:
		err = forward_to_mbox(rm, &rm->afpf_mbox, 0, req, size, "AF");
		break;
	}

	return err;
}

static int send_flr_msg(struct otx2_mbox *mbox, int dev_id, int pcifunc)
{
	struct msg_req *req;

	req = (struct msg_req *)
		otx2_mbox_alloc_msg(mbox, dev_id, sizeof(*req));
	if (req == NULL)
		return -ENOMEM;

	req->hdr.pcifunc = pcifunc;
	req->hdr.id = MBOX_MSG_VF_FLR;
	req->hdr.sig = OTX2_MBOX_REQ_SIG;

	otx2_mbox_msg_send(mbox, 0);

	return 0;
}

static void rm_send_flr_msg(struct rm_dev *rm, struct rvu_vf *vf)
{
	int res, pcifunc;

	pcifunc = (vf->rm->pf << RVU_PFVF_PF_SHIFT) |
		((vf->vf_id + 1) & RVU_PFVF_FUNC_MASK);

	if (send_flr_msg(&rm->afpf_mbox, 0, pcifunc) != 0) {
		dev_err(&rm->pdev->dev, "Sending FLR to AF failed\n");
		return;
	}

	res = otx2_mbox_wait_for_rsp(&rm->afpf_mbox, 0);
	if (res == -EIO) {
		dev_err(&rm->pdev->dev, "RVU AF MBOX timeout.\n");
	} else if (res) {
		dev_err(&rm->pdev->dev,
			"RVU MBOX error: %d.\n", res);
	}
}

static void rm_send_flr_to_dpi(struct rm_dev *rm)
{
	/* TODO: DPI VF's needs to be handled */
}

static void rm_pfvf_flr_handler(struct work_struct *work)
{
	struct rvu_vf *vf = container_of(work, struct rvu_vf, pfvf_flr_work);
	struct rm_dev *rm = vf->rm;
	struct otx2_mbox *mbox = &rm->pfvf_mbox;

	rm_send_flr_to_dpi(rm);
	rm_send_flr_msg(rm, vf);

	/* Disable interrupts from AF and wait for any pending
	 * responses to be handled for this VF and then reset the
	 * mailbox
	 */
	disable_af_mbox_int(rm->pdev);
	flush_workqueue(rm->afpf_mbox_wq);
	otx2_mbox_reset(mbox, vf->vf_id);
	vf->in_use = false;
	vf->got_flr = false;
	enable_af_mbox_int(rm->pdev);
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(vf->vf_id / 64),
		   BIT_ULL(vf->intr_idx));
}

static void rm_pfvf_mbox_handler_up(struct work_struct *work)
{
	struct rm_dev *rm;
	struct mbox_hdr *rsp_hdr;
	struct mbox_msghdr *msg, *fwd;
	struct otx2_mbox *af_mbx, *vf_mbx;
	int offset, i, size;
	struct rvu_vf *vf;

	/* Read latest mbox data */
	smp_rmb();

	vf = container_of(work, struct rvu_vf, mbox_wrk_up);
	rm = vf->rm;
	af_mbx = &rm->afpf_mbox;
	vf_mbx = &rm->pfvf_mbox;
	rsp_hdr = (struct mbox_hdr *)(vf_mbx->dev[vf->vf_id].mbase +
				      vf_mbx->rx_start);
	if (rsp_hdr->num_msgs == 0)
		return;
	offset = ALIGN(sizeof(struct mbox_hdr), MBOX_MSG_ALIGN);

	for (i = 0; i < rsp_hdr->num_msgs; i++) {
		msg = (struct mbox_msghdr *)(vf_mbx->dev->mbase +
					     vf_mbx->rx_start + offset);
		size = msg->next_msgoff - offset;

		if (msg->sig != OTX2_MBOX_RSP_SIG) {
			dev_err(&rm->pdev->dev,
				"UP MBOX msg with wrong signature %x, ID 0x%x\n",
				msg->sig, msg->id);
			goto end;
		}

		/* override message value with actual values */
		msg->pcifunc = (rm->pf << RVU_PFVF_PF_SHIFT) | vf->vf_id;

		fwd = otx2_mbox_alloc_msg(af_mbx, 0, size);
		if (!fwd) {
			dev_err(&rm->pdev->dev,
				"UP Forwarding from VF%d to AF failed.\n",
				vf->vf_id);
			goto end;
		}
		memcpy((uint8_t *)fwd + sizeof(struct mbox_msghdr),
			(uint8_t *)msg + sizeof(struct mbox_msghdr),
			size);
		fwd->id = msg->id;
		fwd->pcifunc = msg->pcifunc;
		fwd->sig = msg->sig;
		fwd->ver = msg->ver;
		fwd->rc = msg->rc;
end:
		offset = msg->next_msgoff;
		vf_mbx->dev->msgs_acked++;
	}
	otx2_mbox_reset(vf_mbx, vf->vf_id);
}

static void rm_pfvf_mbox_handler(struct work_struct *work)
{
	struct rvu_vf *vf = container_of(work, struct rvu_vf, mbox_wrk);
	struct rm_dev *rm = vf->rm;
	struct otx2_mbox *mbox = &rm->pfvf_mbox;
	struct otx2_mbox_dev *mdev = &mbox->dev[vf->vf_id];
	struct mbox_hdr *req_hdr;
	struct mbox_msghdr *msg;
	int offset, id, err;

	/* sync with mbox memory region */
	smp_rmb();

	/* Process received mbox messages */
	req_hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	offset = ALIGN(sizeof(*req_hdr), MBOX_MSG_ALIGN);
	for (id = 0; id < req_hdr->num_msgs; id++) {
		msg = (struct mbox_msghdr *)(mdev->mbase + mbox->rx_start +
					     offset);

		/* Set which VF sent this message based on mbox IRQ */
		msg->pcifunc = ((u16)rm->pf << RVU_PFVF_PF_SHIFT) |
				((vf->vf_id + 1) & RVU_PFVF_FUNC_MASK);
		err = handle_vf_req(rm, vf, msg, msg->next_msgoff - offset);
		if (err)
			otx2_reply_invalid_msg(mbox, vf->vf_id, msg->pcifunc,
					       msg->id);
		offset = msg->next_msgoff;
	}
	/* Send mbox responses to VF */
	if (mdev->num_msgs)
		otx2_mbox_msg_send(mbox, vf->vf_id);
}

static irqreturn_t rm_af_pf_mbox_intr(int irq, void *arg)
{
	struct rm_dev *rm = (struct rm_dev *)arg;
	struct mbox_hdr *hdr;
	struct otx2_mbox *mbox;
	struct otx2_mbox_dev *mdev;

	/* Read latest mbox data */
	smp_rmb();

	mbox = &rm->afpf_mbox;
	mdev = &mbox->dev[0];
	hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	/* Handle PF => AF channel response */
	if (hdr->num_msgs)
		queue_work(rm->afpf_mbox_wq, &rm->mbox_wrk);

	mbox = &rm->afpf_mbox_up;
	mdev = &mbox->dev[0];
	hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
	/* Handle AF => PF request */
	if (hdr->num_msgs)
		queue_work(rm->afpf_mbox_wq, &rm->mbox_wrk_up);

	/* Clear and ack the interrupt */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_INT, 0x1ULL);

	return IRQ_HANDLED;
}

static void __handle_vf_flr(struct rm_dev *rm, struct rvu_vf *vf_ptr)
{
	if (vf_ptr->in_use) {
		/* Using the same MBOX workqueue here, so that we can
		 * synchronize with other VF->PF messages being forwarded to
		 * AF
		 */
		vf_ptr->got_flr = true;
		queue_work(rm->pfvf_mbox_wq, &vf_ptr->pfvf_flr_work);
	} else
		rm_write64(rm, BLKADDR_RVUM, 0,
			   RVU_PF_VFTRPENDX(vf_ptr->vf_id / 64),
			   BIT_ULL(vf_ptr->intr_idx));
}

static irqreturn_t rm_pf_vf_flr_intr(int irq, void *arg)
{
	struct rm_dev *rm = (struct rm_dev *)arg;
	u64 intr;
	struct rvu_vf *vf_ptr;
	int vf, i;

	/* Check which VF FLR has been raised and process accordingly */
	for (i = 0; i < 2; i++) {
		/* Read the interrupt bits */
		intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(i));

		for (vf = i * 64; vf < rm->num_vfs; vf++) {
			vf_ptr = &rm->vf_info[vf];
			if (intr & (1ULL << vf_ptr->intr_idx)) {
				/* Clear the interrupts */
				rm_write64(rm, BLKADDR_RVUM, 0,
					   RVU_PF_VFFLR_INTX(i),
					   BIT_ULL(vf_ptr->intr_idx));
				__handle_vf_flr(rm, vf_ptr);
			}
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t rm_pf_vf_mbox_intr(int irq, void *arg)
{
	struct rm_dev *rm = (struct rm_dev *)arg;
	struct mbox_hdr *hdr;
	struct otx2_mbox *mbox;
	struct otx2_mbox_dev *mdev;
	u64 intr;
	struct rvu_vf *vf;
	int i, vfi;

	/* Check which VF has raised an interrupt and schedule corresponding
	 * workq to process the MBOX
	 */
	for (i = 0; i < 2; i++) {
		/* Read the interrupt bits */
		intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(i));

		for (vfi = i * 64; vfi < rm->num_vfs; vfi++) {
			vf = &rm->vf_info[vfi];
			if ((intr & (1ULL << vf->intr_idx)) == 0)
				continue;
			mbox = &rm->pfvf_mbox;
			mdev = &mbox->dev[vf->vf_id];
			hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
			/* Handle VF => PF channel request */
			if (hdr->num_msgs)
				queue_work(rm->pfvf_mbox_wq, &vf->mbox_wrk);

			mbox = &rm->pfvf_mbox_up;
			mdev = &mbox->dev[vf->vf_id];
			hdr = (struct mbox_hdr *)(mdev->mbase + mbox->rx_start);
			/* Handle PF => VF channel response */
			if (hdr->num_msgs)
				queue_work(rm->pfvf_mbox_wq, &vf->mbox_wrk_up);
			/* Clear the interrupt */
			rm_write64(rm, BLKADDR_RVUM, 0,
				   RVU_PF_VFPF_MBOX_INTX(i),
				   BIT_ULL(vf->intr_idx));
		}
	}

	return IRQ_HANDLED;
}

static int rm_register_flr_irq(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int err, vec, i;

	rm = pci_get_drvdata(pdev);

	/* Register for VF FLR interrupts
	 * There are 2 vectors starting at index 0x0
	 */
	for (vec = RVU_PF_INT_VEC_VFFLR0, i = 0;
	     vec + i <= RVU_PF_INT_VEC_VFFLR1; i++) {
		sprintf(&rm->irq_names[(vec + i) * NAME_SIZE],
			"PF%02d_VF_FLR_IRQ%d", pdev->devfn, i);
		err = request_irq(pci_irq_vector(pdev, vec + i),
				  rm_pf_vf_flr_intr, 0,
				  &rm->irq_names[(vec + i) * NAME_SIZE], rm);
		if (err) {
			dev_err(&pdev->dev,
				"request_irq() failed for PFVF FLR intr %d\n",
				vec);
			goto reg_fail;
		}
		rm->irq_allocated[vec + i] = true;
	}

	return 0;

reg_fail:

	return err;
}

static void rm_free_flr_irq(struct pci_dev *pdev)
{
	(void) pdev;
	/* Nothing here but will free workqueues */
}

static int rm_alloc_irqs(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int err;

	rm = pci_get_drvdata(pdev);

	/* Get number of MSIX vector count and allocate vectors first */
	rm->msix_count = pci_msix_vec_count(pdev);

	err = pci_alloc_irq_vectors(pdev, rm->msix_count, rm->msix_count,
				    PCI_IRQ_MSIX);

	if (err < 0) {
		dev_err(&pdev->dev, "pci_alloc_irq_vectors() failed %d\n", err);
		return err;
	}

	rm->irq_names = kmalloc_array(rm->msix_count, NAME_SIZE, GFP_KERNEL);
	if (!rm->irq_names) {
		err = -ENOMEM;
		goto err_irq_names;
	}

	rm->irq_allocated = kcalloc(rm->msix_count, sizeof(bool), GFP_KERNEL);
	if (!rm->irq_allocated) {
		err = -ENOMEM;
		goto err_irq_allocated;
	}

	return 0;

err_irq_allocated:
	kfree(rm->irq_names);
	rm->irq_names = NULL;
err_irq_names:
	pci_free_irq_vectors(pdev);
	rm->msix_count = 0;

	return err;
}

static void rm_free_irqs(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int irq;

	rm = pci_get_drvdata(pdev);
	for (irq = 0; irq < rm->msix_count; irq++) {
		if (rm->irq_allocated[irq])
			free_irq(pci_irq_vector(rm->pdev, irq), rm);
	}

	pci_free_irq_vectors(pdev);

	kfree(rm->irq_names);
	kfree(rm->irq_allocated);
}

static int rm_register_mbox_irq(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int err, vec = RVU_PF_INT_VEC_VFPF_MBOX0, i;

	rm = pci_get_drvdata(pdev);

	/* Register PF-AF interrupt handler */
	sprintf(&rm->irq_names[RVU_PF_INT_VEC_AFPF_MBOX * NAME_SIZE],
		"PF%02d_AF_MBOX_IRQ", pdev->devfn);
	err = request_irq(pci_irq_vector(pdev, RVU_PF_INT_VEC_AFPF_MBOX),
			  rm_af_pf_mbox_intr, 0,
			  &rm->irq_names[RVU_PF_INT_VEC_AFPF_MBOX * NAME_SIZE],
			  rm);
	if (err) {
		dev_err(&pdev->dev,
			"request_irq() failed for AF_PF MSIX vector\n");
		return err;
	}
	rm->irq_allocated[RVU_PF_INT_VEC_AFPF_MBOX] = true;

	err = otx2_mbox_init(&rm->afpf_mbox, rm->af_mbx_base, pdev, rm->bar2,
			     MBOX_DIR_PFAF, 1);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize PF/AF MBOX\n");
		goto error;
	}
	err = otx2_mbox_init(&rm->afpf_mbox_up, rm->af_mbx_base, pdev, rm->bar2,
			     MBOX_DIR_PFAF_UP, 1);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize PF/AF UP MBOX\n");
		goto error;
	}

	/* Register for PF-VF mailbox interrupts
	 * There are 2 vectors starting at index 0x4
	 */
	for (vec = RVU_PF_INT_VEC_VFPF_MBOX0, i = 0;
	     vec + i <= RVU_PF_INT_VEC_VFPF_MBOX1; i++) {
		sprintf(&rm->irq_names[(vec + i) * NAME_SIZE],
			"PF%02d_VF_MBOX_IRQ%d", pdev->devfn, i);
		err = request_irq(pci_irq_vector(pdev, vec + i),
				  rm_pf_vf_mbox_intr, 0,
				  &rm->irq_names[(vec + i) * NAME_SIZE], rm);
		if (err) {
			dev_err(&pdev->dev,
				"request_irq() failed for PFVF Mbox intr %d\n",
				vec + i);
			goto error;
		}
		rm->irq_allocated[vec + i] = true;
	}

	rm->afpf_mbox_wq = alloc_workqueue(
	    "rm_pfaf_mailbox", WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!rm->afpf_mbox_wq)
		goto error;

	INIT_WORK(&rm->mbox_wrk, rm_afpf_mbox_handler);
	INIT_WORK(&rm->mbox_wrk_up, rm_afpf_mbox_handler_up);

	return err;

error:
	if (rm->afpf_mbox_up.dev != NULL)
		otx2_mbox_destroy(&rm->afpf_mbox_up);
	if (rm->afpf_mbox.dev != NULL)
		otx2_mbox_destroy(&rm->afpf_mbox);

	return err;
}

static int rm_get_pcifunc(struct rm_dev *rm)
{
	struct msg_req *ready_req;
	int res = 0;

	ready_req = (struct msg_req *)
		otx2_mbox_alloc_msg_rsp(&rm->afpf_mbox, 0, sizeof(ready_req),
					sizeof(struct ready_msg_rsp));
	if (ready_req == NULL) {
		dev_err(&rm->pdev->dev, "RVU MBOX failed to get message.\n");
		return -EFAULT;
	}

	ready_req->hdr.id = MBOX_MSG_READY;
	ready_req->hdr.sig = OTX2_MBOX_REQ_SIG;
	otx2_mbox_msg_send(&rm->afpf_mbox, 0);
	res = otx2_mbox_wait_for_rsp(&rm->afpf_mbox, 0);
	if (res == -EIO) {
		dev_err(&rm->pdev->dev, "RVU AF MBOX timeout.\n");
	} else if (res) {
		dev_err(&rm->pdev->dev, "RVU MBOX error: %d.\n", res);
		res = -EFAULT;
	}
	return res;
}

static int rm_get_available_rsrcs(struct rm_dev *rm)
{
	struct mbox_msghdr *rsrc_req;
	int res = 0;

	rsrc_req = otx2_mbox_alloc_msg(&rm->afpf_mbox, 0, sizeof(*rsrc_req));
	if (rsrc_req == NULL) {
		dev_err(&rm->pdev->dev, "RVU MBOX failed to get message.\n");
		return -EFAULT;
	}
	rsrc_req->id = MBOX_MSG_FREE_RSRC_CNT;
	rsrc_req->sig = OTX2_MBOX_REQ_SIG;
	rsrc_req->pcifunc = RVU_PFFUNC(rm->pf, 0);
	otx2_mbox_msg_send(&rm->afpf_mbox, 0);
	res = otx2_mbox_wait_for_rsp(&rm->afpf_mbox, 0);
	if (res == -EIO) {
		dev_err(&rm->pdev->dev, "RVU AF MBOX timeout.\n");
	} else if (res) {
		dev_err(&rm->pdev->dev,
			"RVU MBOX error: %d.\n", res);
		res = -EFAULT;
	}
	return res;
}

static void rm_afpf_mbox_term(struct pci_dev *pdev)
{
	struct rm_dev *rm = pci_get_drvdata(pdev);

	flush_workqueue(rm->afpf_mbox_wq);
	destroy_workqueue(rm->afpf_mbox_wq);
	otx2_mbox_destroy(&rm->afpf_mbox);
	otx2_mbox_destroy(&rm->afpf_mbox_up);
}

static ssize_t vf_in_use_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct rvu_vf *vf = container_of(attr, struct rvu_vf, in_use_attr);

	return snprintf(buf, PAGE_SIZE, "%d\n", vf->in_use);
}

static void vf_sysfs_destroy(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	struct rvu_vf *vf;
	int i;

	rm = pci_get_drvdata(pdev);

	quotas_free(rm->vf_limits.sso);
	quotas_free(rm->vf_limits.ssow);
	quotas_free(rm->vf_limits.npa);
	quotas_free(rm->vf_limits.tim);
	rm->vf_limits.sso = NULL;
	rm->vf_limits.ssow = NULL;
	rm->vf_limits.npa = NULL;
	rm->vf_limits.tim = NULL;

	for (i = 0; i < rm->num_vfs; i++) {
		vf = &rm->vf_info[i];
		if (vf->limits_kobj == NULL)
			continue;
		if (vf->in_use_attr.attr.mode != 0) {
			sysfs_remove_file(&vf->pdev->dev.kobj,
					  &vf->in_use_attr.attr);
			vf->in_use_attr.attr.mode = 0;
		}
		kobject_del(vf->limits_kobj);
		vf->limits_kobj = NULL;
		pci_dev_put(vf->pdev);
		vf->pdev = NULL;
	}
}

static int check_vf_in_use(void *arg, struct quota *quota, int new_val)
{
	struct rvu_vf *vf = arg;

	if (vf->in_use) {
		dev_err(quota->dev, "Can't modify limits, device is in use.\n");
		return 1;
	}
	return 0;
}

static struct quota_ops vf_limit_ops = {
	.pre_store = check_vf_in_use,
};

static int vf_sysfs_create(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	struct pci_dev *vdev;
	struct rvu_vf *vf;
	int err, i;

	vdev = NULL;
	vf = NULL;
	rm = pci_get_drvdata(pdev);
	err = 0;
	i = 0;

	/* Create limit structures for all resource types */
	rm->vf_limits.sso = quotas_alloc(rm->num_vfs, rm->limits.sso,
					 rm->limits.sso, 0, &rm->lock,
					 &vf_limit_ops);
	if (rm->vf_limits.sso == NULL) {
		dev_err(&pdev->dev,
			"Failed to allocate sso limits structures.\n");
		err = -EFAULT;
		goto error;
	}
	rm->vf_limits.ssow = quotas_alloc(rm->num_vfs, rm->limits.ssow,
					  rm->limits.ssow, 0, &rm->lock,
					  &vf_limit_ops);
	if (rm->vf_limits.ssow == NULL) {
		dev_err(&pdev->dev,
			"Failed to allocate ssow limits structures.\n");
		err = -EFAULT;
		goto error;
	}
	/* AF currently reports only 0-1 for PF but there's more free LFs.
	 * Until we implement proper limits in AF, use max num_vfs in total.
	 */
	rm->vf_limits.npa = quotas_alloc(rm->num_vfs, 1, rm->num_vfs, 0,
					 &rm->lock, &vf_limit_ops);
	if (rm->vf_limits.npa == NULL) {
		dev_err(&pdev->dev,
			"Failed to allocate npa limits structures.\n");
		err = -EFAULT;
		goto error;
	}
	rm->vf_limits.tim = quotas_alloc(rm->num_vfs, rm->limits.tim,
					 rm->limits.tim, 0, &rm->lock,
					 &vf_limit_ops);
	if (rm->vf_limits.tim == NULL) {
		dev_err(&pdev->dev,
			"Failed to allocate tim limits structures.\n");
		err = -EFAULT;
		goto error;
	}

	/* loop through all the VFs and create sysfs entries for them */
	while ((vdev = pci_get_device(pdev->vendor, PCI_DEVID_OCTEONTX2_SSO_VF,
				      vdev))) {
		if (!vdev->is_virtfn || (vdev->physfn != pdev))
			continue;
		vf = &rm->vf_info[i];
		vf->pdev = pci_dev_get(vdev);
		vf->limits_kobj = kobject_create_and_add("limits",
							 &vdev->dev.kobj);
		if (vf->limits_kobj == NULL) {
			err = -ENOMEM;
			goto error;
		}
		if (quota_sysfs_create("sso", vf->limits_kobj, &vdev->dev,
				       &rm->vf_limits.sso->a[i], vf) != 0) {
			dev_err(&pdev->dev,
				"Failed to create sso limits sysfs for %s\n",
				pci_name(vdev));
			err = -EFAULT;
			goto error;
		}
		if (quota_sysfs_create("ssow", vf->limits_kobj, &vdev->dev,
				       &rm->vf_limits.ssow->a[i], vf) != 0) {
			dev_err(&pdev->dev,
				"Failed to create ssow limits sysfs for %s\n",
				pci_name(vdev));
			err = -EFAULT;
			goto error;
		}
		if (quota_sysfs_create("npa", vf->limits_kobj, &vdev->dev,
				       &rm->vf_limits.npa->a[i], vf) != 0) {
			dev_err(&pdev->dev,
				"Failed to create npa limits sysfs for %s\n",
				pci_name(vdev));
			err = -EFAULT;
			goto error;
		}
		if (quota_sysfs_create("tim", vf->limits_kobj, &vdev->dev,
				       &rm->vf_limits.tim->a[i], vf) != 0) {
			dev_err(&pdev->dev,
				"Failed to create tim limits sysfs for %s\n",
				pci_name(vdev));
			err = -EFAULT;
			goto error;
		}

		vf->in_use_attr.show = vf_in_use_show;
		vf->in_use_attr.attr.name = "in_use";
		vf->in_use_attr.attr.mode = 0444;
		sysfs_attr_init(&vf->in_use_attr.attr);
		if (sysfs_create_file(&vdev->dev.kobj, &vf->in_use_attr.attr)) {
			dev_err(&pdev->dev,
				"Failed to create in_use sysfs entry for %s\n",
				pci_name(vdev));
			err = -EFAULT;
			goto error;
		}
		i++;
	}

	return 0;
error:
	vf_sysfs_destroy(pdev);
	return err;
}

static int rm_check_pf_usable(struct rm_dev *rm)
{
	u64 rev;

	rev = rm_read64(rm, BLKADDR_RVUM, 0,
			RVU_PF_BLOCK_ADDRX_DISC(BLKADDR_RVUM));
	rev = (rev >> 12) & 0xFF;
	/* Check if AF has setup revision for RVUM block,
	 * otherwise this driver probe should be deferred
	 * until AF driver comes up.
	 */
	if (!rev) {
		dev_warn(&rm->pdev->dev,
			 "AF is not initialized, deferring probe\n");
		return -EPROBE_DEFER;
	}
	return 0;
}

static int rm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct rm_dev *rm;
	int err;

	rm = devm_kzalloc(dev, sizeof(struct rm_dev), GFP_KERNEL);
	if (rm == NULL)
		return -ENOMEM;

	rm->pdev = pdev;
	pci_set_drvdata(pdev, rm);

	mutex_init(&rm->lock);

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCI device\n");
		goto enable_failed;
	}

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(dev, "PCI request regions failed 0x%x\n", err);
		goto map_failed;
	}

	if (pci_sriov_get_totalvfs(pdev) <= 0) {
		err = -ENODEV;
		goto set_mask_failed;
	}

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to set DMA mask\n");
		goto set_mask_failed;
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to set DMA mask\n");
		goto set_mask_failed;
	}

	pci_set_master(pdev);

	/* CSR Space mapping */
	rm->bar2 = pcim_iomap(pdev, PCI_CFG_REG_BAR_NUM,
			       pci_resource_len(pdev, PCI_CFG_REG_BAR_NUM));
	if (!rm->bar2) {
		dev_err(&pdev->dev, "Unable to map BAR2\n");
		err = -ENODEV;
		goto set_mask_failed;
	}

	err = rm_check_pf_usable(rm);
	if (err)
		goto pf_unusable;

	/* Map PF-AF mailbox memory */
	rm->af_mbx_base = ioremap_wc(pci_resource_start(pdev, PCI_MBOX_BAR_NUM),
				     pci_resource_len(pdev, PCI_MBOX_BAR_NUM));
	if (!rm->af_mbx_base) {
		dev_err(&pdev->dev, "Unable to map BAR4\n");
		err = -ENODEV;
		goto pf_unusable;
	}

	/* Request IRQ for PF-VF mailbox here - TBD: check if this can be moved
	 * to sriov enable function
	 */
	if (rm_alloc_irqs(pdev)) {
		dev_err(&pdev->dev,
			"Unable to allocate MSIX Interrupt vectors\n");
		err = -ENODEV;
		goto alloc_irqs_failed;
	}

	if (rm_register_mbox_irq(pdev) != 0) {
		dev_err(&pdev->dev,
			"Unable to allocate MBOX Interrupt vectors\n");
		err = -ENODEV;
		goto reg_mbox_irq_failed;
	}

	if (rm_register_flr_irq(pdev) != 0) {
		dev_err(&pdev->dev,
			"Unable to allocate FLR Interrupt vectors\n");
		err = -ENODEV;
		goto reg_flr_irq_failed;
	}

	enable_af_mbox_int(pdev);

	if (rm_get_pcifunc(rm)) {
		dev_err(&pdev->dev,
			"Failed to retrieve pcifunc from AF\n");
		err = -ENODEV;
		goto get_pcifunc_failed;
	}

	/* Add to global list of PFs found */
	spin_lock(&rm_lst_lock);
	list_add(&rm->list, &rm_dev_lst_head);
	spin_unlock(&rm_lst_lock);

	return 0;

get_pcifunc_failed:
	disable_af_mbox_int(pdev);
	rm_free_flr_irq(pdev);
reg_flr_irq_failed:
	rm_afpf_mbox_term(pdev);
reg_mbox_irq_failed:
	rm_free_irqs(pdev);
alloc_irqs_failed:
	iounmap(rm->af_mbx_base);
pf_unusable:
	pcim_iounmap(pdev, rm->bar2);
set_mask_failed:
	pci_release_regions(pdev);
map_failed:
	pci_disable_device(pdev);
enable_failed:
	pci_set_drvdata(pdev, NULL);
	devm_kfree(dev, rm);
	return err;
}

static void enable_vf_flr_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int ena_bits;

	rm = pci_get_drvdata(pdev);
	/* Clear any pending interrupts */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(0), ~0x0ULL);
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(0), ~0x0ULL);

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(1), ~0x0ULL);
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(1), ~0x0ULL);
	}

	/* Enable for first 64 VFs here - upto number of VFs enabled */
	ena_bits = ((rm->num_vfs - 1) % 64);
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INT_ENA_W1SX(0),
		   GENMASK_ULL(ena_bits, 0));

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		/* Enable for VF interrupts for VFs 64  to 128 */
		ena_bits = rm->num_vfs - 64 - 1;
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INT_ENA_W1SX(1),
			   GENMASK_ULL(ena_bits, 0));
	}
}

static void disable_vf_flr_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int ena_bits;
	u64 intr;

	rm = pci_get_drvdata(pdev);
	/* clear any pending interrupt */

	intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(0));
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(0), intr);
	intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(0));
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(0), intr);

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(1));
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INTX(1), intr);
		intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(1));
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFTRPENDX(1), intr);
	}

	/* Disable for first 64 VFs here - upto number of VFs enabled */
	ena_bits = ((rm->num_vfs - 1) % 64);

	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INT_ENA_W1CX(0),
		   GENMASK_ULL(ena_bits, 0));

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		/* Enable for VF interrupts for VFs 64  to 128 */
		ena_bits = rm->num_vfs - 64 - 1;
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFFLR_INT_ENA_W1CX(1),
			   GENMASK_ULL(ena_bits, 0));
	}
}

static void enable_vf_mbox_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int ena_bits;

	rm = pci_get_drvdata(pdev);
	/* Clear any pending interrupts */
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(0), ~0x0ULL);

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(1),
			   ~0x0ULL);
	}

	/* Enable for first 64 VFs here - upto number of VFs enabled */
	ena_bits = ((rm->num_vfs - 1) % 64);
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INT_ENA_W1SX(0),
		   GENMASK_ULL(ena_bits, 0));

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		/* Enable for VF interrupts for VFs 64  to 128 */
		ena_bits = rm->num_vfs - 64 - 1;
		rm_write64(rm, BLKADDR_RVUM, 0,
			   RVU_PF_VFPF_MBOX_INT_ENA_W1SX(1),
			   GENMASK_ULL(ena_bits, 0));
	}
}

static void disable_vf_mbox_int(struct pci_dev *pdev)
{
	struct rm_dev *rm;
	int ena_bits;
	u64 intr;

	rm = pci_get_drvdata(pdev);
	/* clear any pending interrupt */

	intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(0));
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(0), intr);

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		intr = rm_read64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(1));
		rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INTX(1), intr);
	}

	/* Disable for first 64 VFs here - upto number of VFs enabled */
	ena_bits = ((rm->num_vfs - 1) % 64);
	rm_write64(rm, BLKADDR_RVUM, 0, RVU_PF_VFPF_MBOX_INT_ENA_W1CX(0),
			GENMASK_ULL(ena_bits, 0));

	if (rm->num_vfs > 64) { /* For VF 64 to 127(MAX) */
		/* Enable for VF interrupts for VFs 64  to 128 */
		ena_bits = rm->num_vfs - 64 - 1;
		rm_write64(rm, BLKADDR_RVUM, 0,
			   RVU_PF_VFPF_MBOX_INT_ENA_W1CX(1),
			   GENMASK_ULL(ena_bits, 0));
	}
}

static int __sriov_disable(struct pci_dev *pdev)
{
	struct rm_dev *rm;

	rm = pci_get_drvdata(pdev);
	if (pci_vfs_assigned(pdev)) {
		dev_err(&pdev->dev, "Disabing VFs while VFs are assigned\n");
		dev_err(&pdev->dev, "VFs will not be freed\n");
		return -EPERM;
	}

	disable_vf_flr_int(pdev);
	disable_vf_mbox_int(pdev);

#ifdef CONFIG_OCTEONTX2_RM_DOM_SYSFS
	domain_sysfs_destroy(rm);
#endif
	vf_sysfs_destroy(pdev);

	if (rm->pfvf_mbox_wq) {
		flush_workqueue(rm->pfvf_mbox_wq);
		destroy_workqueue(rm->pfvf_mbox_wq);
		rm->pfvf_mbox_wq = NULL;
	}
	if (rm->pfvf_mbx_base) {
		iounmap(rm->pfvf_mbx_base);
		rm->pfvf_mbx_base = NULL;
	}

	otx2_mbox_destroy(&rm->pfvf_mbox);
	otx2_mbox_destroy(&rm->pfvf_mbox_up);

	pci_disable_sriov(pdev);

	kfree(rm->vf_info);
	rm->vf_info = NULL;

	return 0;
}

static int __sriov_enable(struct pci_dev *pdev, int num_vfs)
{
	int curr_vfs, vf = 0;
	int err;
	struct rm_dev *rm;
	struct rvu_vf *vf_ptr;
	u64 pf_vf_mbox_base;

	curr_vfs = pci_num_vf(pdev);
	if (!curr_vfs && !num_vfs)
		return -EINVAL;

	if (curr_vfs) {
		dev_err(
		    &pdev->dev,
		    "Virtual Functions are already enabled on this device\n");
		return -EINVAL;
	}
	if (num_vfs > RM_MAX_VFS)
		num_vfs = RM_MAX_VFS;

	rm = pci_get_drvdata(pdev);

	if (rm_get_available_rsrcs(rm)) {
		dev_err(&pdev->dev, "Failed to get resource limits.\n");
		return -EFAULT;
	}

	rm->vf_info = kcalloc(num_vfs, sizeof(struct rvu_vf), GFP_KERNEL);
	if (rm->vf_info == NULL)
		return -ENOMEM;

	err = pci_enable_sriov(pdev, num_vfs);
	if (err) {
		dev_err(&pdev->dev, "Failed to enable to SRIOV VFs: %d\n", err);
		goto err_enable_sriov;
	}

	rm->num_vfs = num_vfs;

	/* Map PF-VF mailbox memory */
	pf_vf_mbox_base = (u64)rm->bar2 + RVU_PF_VF_BAR4_ADDR;
	pf_vf_mbox_base = readq((void __iomem *)(unsigned long)pf_vf_mbox_base);
	if (!pf_vf_mbox_base) {
		dev_err(&pdev->dev, "PF-VF Mailbox address not configured\n");
		err = -ENOMEM;
		goto err_mbox_mem_map;
	}
	rm->pfvf_mbx_base = ioremap_wc(pf_vf_mbox_base, MBOX_SIZE * num_vfs);
	if (!rm->pfvf_mbx_base) {
		dev_err(&pdev->dev,
			"Mapping of PF-VF mailbox address failed\n");
		err = -ENOMEM;
		goto err_mbox_mem_map;
	}
	err = otx2_mbox_init(&rm->pfvf_mbox, rm->pfvf_mbx_base, pdev, rm->bar2,
			     MBOX_DIR_PFVF, num_vfs);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize PF/VF MBOX for %d VFs\n",
			num_vfs);
		goto err_mbox_init;
	}

	err = otx2_mbox_init(&rm->pfvf_mbox_up, rm->pfvf_mbx_base, pdev,
			     rm->bar2, MBOX_DIR_PFVF_UP, num_vfs);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize PF/VF MBOX UP for %d VFs\n",
			num_vfs);
		goto err_mbox_up_init;
	}

	/* Allocate a single workqueue for VF/PF mailbox because access to
	 * AF/PF mailbox has to be synchronized.
	 */
	rm->pfvf_mbox_wq =
		alloc_workqueue("rm_pfvf_mailbox",
				WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (rm->pfvf_mbox_wq == NULL) {
		dev_err(&pdev->dev,
			"Workqueue allocation failed for PF-VF MBOX\n");
		err = -ENOMEM;
		goto err_workqueue_alloc;
	}

	for (vf = 0; vf < num_vfs; vf++) {
		vf_ptr = &rm->vf_info[vf];
		vf_ptr->vf_id = vf;
		vf_ptr->rm = (void *)rm;
		vf_ptr->intr_idx = vf % 64;
		INIT_WORK(&vf_ptr->mbox_wrk, rm_pfvf_mbox_handler);
		INIT_WORK(&vf_ptr->mbox_wrk_up, rm_pfvf_mbox_handler_up);
		INIT_WORK(&vf_ptr->pfvf_flr_work, rm_pfvf_flr_handler);
	}

	err = vf_sysfs_create(pdev);
	if (err) {
		dev_err(&pdev->dev,
			"Failed to initialize VF sysfs entries. Err=%d\n",
			err);
		err = -EFAULT;
		goto err_vf_sysfs_create;
	}

#ifdef CONFIG_OCTEONTX2_RM_DOM_SYSFS
	err = domain_sysfs_create(rm);
	if (err) {
		dev_err(&pdev->dev, "Failed to create RM domain sysfs\n");
		err = -EFAULT;
		goto err_domain_sysfs_create;
	}
#endif

	enable_vf_mbox_int(pdev);
	enable_vf_flr_int(pdev);
	return num_vfs;

#ifdef CONFIG_OCTEONTX2_RM_DOM_SYSFS
err_domain_sysfs_create:
	vf_sysfs_destroy(pdev);
#endif
err_vf_sysfs_create:
err_workqueue_alloc:
	destroy_workqueue(rm->pfvf_mbox_wq);
	if (rm->pfvf_mbox_up.dev != NULL)
		otx2_mbox_destroy(&rm->pfvf_mbox_up);
err_mbox_up_init:
	if (rm->pfvf_mbox.dev != NULL)
		otx2_mbox_destroy(&rm->pfvf_mbox);
err_mbox_init:
	iounmap(rm->pfvf_mbx_base);
err_mbox_mem_map:
	pci_disable_sriov(pdev);
err_enable_sriov:
	kfree(rm->vf_info);

	return err;
}

static int rm_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	if (num_vfs == 0)
		return __sriov_disable(pdev);
	else
		return __sriov_enable(pdev, num_vfs);
}

static void rm_remove(struct pci_dev *pdev)
{
	struct rm_dev *rm = pci_get_drvdata(pdev);

	spin_lock(&rm_lst_lock);
	list_del(&rm->list);
	spin_unlock(&rm_lst_lock);

	if (rm->num_vfs)
		__sriov_disable(pdev);

	disable_af_mbox_int(pdev);
	rm_free_flr_irq(pdev);
	rm_afpf_mbox_term(pdev);
	rm_free_irqs(pdev);

	if (rm->af_mbx_base)
		iounmap(rm->af_mbx_base);
	if (rm->bar2)
		pcim_iounmap(pdev, rm->bar2);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
	devm_kfree(&pdev->dev, rm);
}

static struct pci_driver rm_driver = {
	.name = DRV_NAME,
	.id_table = rvu_rm_id_table,
	.probe = rm_probe,
	.remove = rm_remove,
	.sriov_configure = rm_sriov_configure,
};

static int __init otx2_rm_init_module(void)
{
	pr_info("%s\n", DRV_NAME);

	spin_lock_init(&rm_lst_lock);
	return pci_register_driver(&rm_driver);
}

static void __exit otx2_rm_exit_module(void)
{
	pci_unregister_driver(&rm_driver);
}

module_init(otx2_rm_init_module);
module_exit(otx2_rm_exit_module);
