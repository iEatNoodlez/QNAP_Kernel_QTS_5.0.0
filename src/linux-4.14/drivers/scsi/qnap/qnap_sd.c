/*******************************************************************************
 * Filename:  qnap_sd.c
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

#include "../sd.h"

#include "qnap_virtual.h"
#include "qnap_virtual_jbod.h"

extern int qnap_is_iscsi_disk(struct scsi_device *sdev);

/* 0: skip probe sd
 * 1: NOT skip probe sd
 */
int qnap_check_skip_probe_sd(struct scsi_device *sdev)
{
	int ret = 0;

	/* check is QNAP iSCSI device ? */
	if (qnap_is_iscsi_disk(sdev))
		return 1;

	/* check to skip probe if we are QNAP iSCSI device ? */
	if(sdev->host->skip_probe_sd == 0)
		return 1;

	/* check is VJBOD disk type for QNAP iSCSI device ? */
	if (!(sdev->probe_sd_type == SD_VJBOD
	|| sdev->probe_sd_type == SD_VJBOD_RDISK)
	)
		return 0;

	/* skip if sd was probed done before */
	if (sdev->probe_sd_done == 1)
		return 0;

	if (sdev->probe_sd == 1) {
		pr_debug("start to probe sd\n");
		return 1;
	}

	pr_debug("skip to probe sd\n");
	return 0;
}
EXPORT_SYMBOL(qnap_check_skip_probe_sd);

void qnap_scsi_probe_sd(struct scsi_device *sdev)
{
	if(qnap_is_iscsi_disk(sdev)) {
		pr_warn("%s: not QNAP iSCSI device, skip probe op\n",
			__func__);
		return;
	}

	if (!(sdev->probe_sd_type == SD_VJBOD
	|| sdev->probe_sd_type == SD_VJBOD_RDISK)
	)
	{
		pr_warn("not VJBOD dev, skip probe op\n");
		return;
	}

	if (sdev->probe_sd_done == 1) {
		pr_warn("sd was probed already\n");
		return;
	}

	pr_info("start to probe sd (type: %s)\n",
		((sdev->probe_sd_type == SD_VJBOD) ? "VJBOD" : "Remote Disk of VJBOD"));

	sdev->probe_sd = 1;
	device_attach(&sdev->sdev_gendev);
	return;
}
EXPORT_SYMBOL(qnap_scsi_probe_sd);
