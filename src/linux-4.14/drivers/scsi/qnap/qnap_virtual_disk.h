#ifndef _QNAP_VIRTUAL_DISK_H
#define _QNAP_VIRTUAL_DISK_H

#include <linux/proc_fs.h>
#include "qnap_virtual.h"

#define QNAP_DISK_NODE "scsi/qnap_disk_node"
#define MAX_DISK_NODE_LEN       16

#ifdef CONFIG_VIRTUAL_DISK_EX
#define QNAP_IQN_NODE "scsi/qnap_iqn_node"
#define MAX_IQN_NODE_LEN       260
#define QNAP_SN_VPD_NODE "scsi/qnap_sn_vpd_node"

extern char qnap_disk_node[MAX_DISK_NODE_LEN];
extern char *iscsi_iqn_arr[MAX_ISCSI_DISK];
extern unsigned char *iscsi_sn_vpd_arr[MAX_ISCSI_DISK];

extern char qnap_iqn_node[MAX_IQN_NODE_LEN];
extern struct proc_dir_entry *qnap_iqn_node_proc_entry;
extern int lun_status[MAX_ISCSI_DISK];
#endif

int qnap_sd_prepare_nas_virt_info(struct scsi_device *sdp, u32 index);
int QNAP_get_iscsi_index(struct scsi_device *sdp, u32 *index);
void QNAP_clear_iscsi_index(struct scsi_device *sdp, u32 index);
int QNAP_check_iscsi_sn(struct scsi_device *sdp, u32 index);
void create_qnap_disk_node_proc(void);

#endif /* _QNAP_VIRTUAL_DISK_H */
