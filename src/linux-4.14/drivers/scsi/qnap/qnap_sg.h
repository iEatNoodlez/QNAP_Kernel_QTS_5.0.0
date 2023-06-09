#ifndef _QNAP_SG_H
#define _QNAP_SG_H

#include <linux/device.h>
#include "../../base/base.h"

int qanp_scsi_add_sg(struct scsi_device *sdev);
int qnap_check_skip_add_sg(struct scsi_device *sdev);

#endif /* _QNAP_SG_H */
