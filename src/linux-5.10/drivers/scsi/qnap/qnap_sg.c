/*******************************************************************************
 * Filename:  qnap_sg.c
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
 ****************************************************************************/

#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <asm/unaligned.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>

#include "qnap_sg.h"
#include "qnap_virtual_jbod.h"

extern int qnap_is_iscsi_disk(struct scsi_device *sdev);

int qanp_scsi_add_sg(struct scsi_device *sdev)
{
	/* This was referred the device_add() and it will call add_dev().
	 * In add_dev(), we need know it was added or not
	 */
	struct device *dev = &sdev->sdev_dev;
	struct class_interface *intf;

	if (dev->class) {
		mutex_lock(&dev->class->p->mutex);

		/* notify any interfaces that the device is here */
		list_for_each_entry(intf, &dev->class->p->interfaces, node) {
			if (intf->add_dev)
				intf->add_dev(dev, intf);
		}
		mutex_unlock(&dev->class->p->mutex);
	}
	return 0;
}
EXPORT_SYMBOL(qanp_scsi_add_sg);

/* 0: skip add sg, 1: add sg */
int qnap_check_skip_add_sg(struct scsi_device *sdev)
{
	if (sdev->add_sg_done == 1)
		return 0;

	/* check it is QNAP iSCSI device or not ? */
	if (qnap_is_iscsi_disk(sdev))
		return 1;

	/* check to skip probe if we are QNAP iSCSI device (VJBOD) ? */
	if (sdev->host->skip_probe_sd == 0)
		return 1;

	/* check is VJBOD disk type for QNAP iSCSI device ? */
	if (sdev->probe_sd_type != SD_VJBOD)
		return 0;

	/* if sd still not probed, not to add sg */
	if (sdev->probe_sd_done != 1) {
		pr_info("skip add sg due to sd was not probed yet\n");
		return 0;
	}

	pr_info("start to add sg\n");
	return 1;

}
EXPORT_SYMBOL(qnap_check_skip_add_sg);
