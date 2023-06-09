#ifndef _QNAP_SD_H
#define _QNAP_SD_H

#include <linux/device.h>

int qnap_check_skip_probe_sd(struct scsi_device *sdev);
void qnap_scsi_probe_sd(struct scsi_device *sdev);

#endif /* _QNAP_SD_H */
