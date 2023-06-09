/*******************************************************************************
 * Filename:  qnap_virtual_disk.c
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
#if defined(CONFIG_MACH_QNAPTS) && defined(CONFIG_AHCI_ALPINE) && defined(CONFIG_PCI_ALPINE_ENDPOINT)
#include <linux/socket.h>
#endif
#include "../sd.h"
#include "qnap_virtual_disk.h"

/**/
extern int is_iscsi_disk(struct scsi_device *sdp);

/* 1. qnap_vdisk_sd_index_lock used to protect iscsi_dev_arr[] 
 * 2. there are 3 status for iscsi_dev_arr[] that are ISCSI_DEV_ARR_NOT_USED,
 *    ISCSI_DEV_ARR_USED and ISCSI_DEV_ARR_READY_TO_USE
 * 3. iscsi_iqn_arr[], iscsi_sn_vpd_arr[] shall be ready when ISCSI_DEV_ARR_USED
 *    is set
 */
static DEFINE_SPINLOCK(qnap_vdisk_sd_index_lock);
char qnap_disk_node[MAX_DISK_NODE_LEN];
struct proc_dir_entry *qnap_disk_node_proc_entry;

#ifdef CONFIG_VIRTUAL_DISK_EX
char *iscsi_iqn_arr[MAX_ISCSI_DISK];
unsigned char *iscsi_sn_vpd_arr[MAX_ISCSI_DISK];

char qnap_iqn_node[MAX_IQN_NODE_LEN];
struct proc_dir_entry *qnap_iqn_node_proc_entry;

// Jay Wei,20150122, Task #11550. Make LUN records in /proc/scsi/qnap_iqn_node
// consistent with reality.
int lun_status[MAX_ISCSI_DISK];
#endif

static inline bool __cmp_iscsi_dev_arr_slot(u32 index, int value)
{
	bool ret = false;

	spin_lock(&qnap_vdisk_sd_index_lock);
	if (iscsi_dev_arr[index] == value)
		ret = true;
	spin_unlock(&qnap_vdisk_sd_index_lock);
	return ret;
}

static inline void __set_iscsi_dev_arr_slot(u32 index, int value)
{
	spin_lock(&qnap_vdisk_sd_index_lock);
	iscsi_dev_arr[index] = value;
	spin_unlock(&qnap_vdisk_sd_index_lock);
}

#ifdef CONFIG_VIRTUAL_DISK_EX
/**/
static ssize_t proc_qnap_iqn_node_write(struct file *filp, const char __user *buff, 
	size_t len, loff_t *off
	)
{
	int copy_len = 0;

//	printk("qnap_iqn_node_write(%p, %lu, %p)\n", buff, len, data);
	memset(qnap_iqn_node, 0, sizeof(qnap_iqn_node));

	if (copy_from_user(qnap_iqn_node, buff, MAX_IQN_NODE_LEN-1))
		return -EFAULT;

	qnap_iqn_node[MAX_IQN_NODE_LEN-1] = 0;
	if (*qnap_iqn_node) {
		char *ptr = strchr(qnap_iqn_node, '\n');
		if (ptr)
			*ptr = 0;
		copy_len = strlen(qnap_iqn_node);
		printk("qnap_iqn_node_write copy_len=%d, get %s\n", copy_len, 
			qnap_iqn_node);
	}

	return (copy_len > len) ? copy_len : len;
}

static ssize_t proc_qnap_iqn_node_read(struct file *page, char __user *start, 
	size_t size, loff_t *off
	)
{
	int i, total_count = 0, read_count;
	char *ptr, *readBuf;
	bool cmp_ret;

	readBuf = kmalloc(128*MAX_ISCSI_DISK, GFP_KERNEL);
	if (!readBuf)
		return 0;

	memset(readBuf, 0, 128*MAX_ISCSI_DISK);
	for (i = 0; i < MAX_ISCSI_DISK; i ++) { 
		// Jay Wei,20150122, Task #11550
		cmp_ret = __cmp_iscsi_dev_arr_slot(i, ISCSI_DEV_ARR_USED);
		if (cmp_ret && iscsi_iqn_arr[i] && lun_status[i]) {
			if (iscsi_sn_vpd_arr[i])
				ptr = iscsi_sn_vpd_arr[i] + 4;
			else
				ptr = "";

			read_count = sprintf(readBuf+total_count, 
				"%4d %s #%s\n", i, iscsi_iqn_arr[i], ptr);
			total_count += read_count;
		}
	}

	read_count = sprintf(readBuf+total_count, "END\n");
	total_count += read_count;
	ptr = readBuf + (*off);
	
	for (i = 0; i < total_count; i++){
		if (ptr[0] == 0)
			break;
		put_user(*(ptr++), start++);
	}

	kfree(readBuf);

	(*off) += i;
	return i;
}

// Jay Wei,20150122, Task #11550. Make LUN records in /proc/scsi/qnap_iqn_node 
// consistent with reality.
/**
 * Check if this LUN SN is in iscsi_sn_vpd_arr array.
 * @sdp: device to get a reference to
 * @index: LUN device index
 * @Return: 
 *         -1, error
 *          0, new LUN (happen when sd_probe() not run)
 *          1, existed LUN which is enable now;
 *          2, LUN is disable
 */
int QNAP_check_iscsi_sn(struct scsi_device *sdp, u32 index)
{
	int vpd_len = 64, ret = 0;
	unsigned char *buffer = kmalloc(vpd_len, GFP_KERNEL);
	bool cmp_ret;

	if (buffer) {
		// try get LUN SN compare
		if (!scsi_get_vpd_page(sdp, 0x80, buffer, vpd_len)) {
			vpd_len = 64;
			cmp_ret = __cmp_iscsi_dev_arr_slot(index, 
							ISCSI_DEV_ARR_USED);
			if (cmp_ret) {
			 	// LUN is in array and enable now.
			 	if(!strncmp(buffer+4,iscsi_sn_vpd_arr[index]+4, 36))
					ret =  1;
				else {
					// It's a new LUN
					if (iscsi_sn_vpd_arr[index])
						kfree(iscsi_sn_vpd_arr[index]);

					iscsi_sn_vpd_arr[index] = buffer;
					vpd_len = iscsi_sn_vpd_arr[index][3] + 4;
					iscsi_sn_vpd_arr[index][vpd_len] = '\0';
					ret = 0;
					goto exit;
				}
			}
		} else
			// No VPD. This LUN is disable.
			ret = 2;
	} else {
	        printk("QNAP_check_iscsi_sn: kmalloc failed!\n");
		ret = -1;
	}           

	if (buffer)
		kfree(buffer);    
exit:
	return ret;     
}

/* locked by qnap_vdisk_sd_index_lock */
static int __QNAP_get_iscsi_free_slot(const char *iqn)
{
	int i;

	if (!iqn || !*iqn)
		return -1;

	/* here need to protect all ranges */
	for (i = 0; i < MAX_ISCSI_DISK; i ++) {//qnap_vdisk_sd_index_lock
		if (iscsi_dev_arr[i] == ISCSI_DEV_ARR_NOT_USED) {
			iscsi_dev_arr[i] = ISCSI_DEV_ARR_READY_TO_USE;
			return i;
		}
	}
	return -1;
}

#endif /* defined(CONFIG_VIRTUAL_DISK_EX) */

static ssize_t proc_qnap_disk_node_write(struct file *filp, const char __user *buff, 
	size_t len, loff_t *off
	)
{
	if (len > MAX_DISK_NODE_LEN) {
		printk(KERN_INFO "qnap_disk_node: only can write %d bytes!\n", 
				MAX_DISK_NODE_LEN);
		return -ENOSPC;
	}

	if (copy_from_user(qnap_disk_node, buff, len ))
		return -EFAULT;

	return len;
}

static int QnapDiskNodeOpen = 0;
static int proc_qnap_disk_node_open(struct inode *inode, struct file *file)
{
	if (QnapDiskNodeOpen > 0) 
		return -EBUSY;

	QnapDiskNodeOpen++;
	return 0;
}

static ssize_t proc_qnap_disk_node_read(struct file *page, char __user *start, 
	size_t size, loff_t *off
	)
{
	int len = 0;
	char *ptr;

	ptr = qnap_disk_node+(*off);

	while (len < size && ptr[0] != 0){
		put_user(*(ptr++), start++);
		len++;
	}

	(*off) += len;

	return len;
}

static int proc_qnap_disk_node_release(struct inode *inode, struct file *file)
{
	QnapDiskNodeOpen--;
	return 0;
}

void create_qnap_disk_node_proc(void)
{
	static const struct proc_ops disk_node_fops = {
		.proc_open	= proc_qnap_disk_node_open,
		.proc_read	= proc_qnap_disk_node_read,
		.proc_write	= proc_qnap_disk_node_write,
		.proc_release	= proc_qnap_disk_node_release,
	};

	static const struct proc_ops iqn_node_fops = {
		.proc_read	= proc_qnap_iqn_node_read,
		.proc_write	= proc_qnap_iqn_node_write,
	};

	qnap_disk_node_proc_entry = proc_create( QNAP_DISK_NODE, 0644, NULL, &disk_node_fops);
	if (!qnap_disk_node_proc_entry) 
		return ;

#ifdef CONFIG_VIRTUAL_DISK_EX
	qnap_iqn_node_proc_entry = proc_create( QNAP_IQN_NODE, 0644, NULL, &iqn_node_fops);
	if (!qnap_iqn_node_proc_entry) 
		return ;
#endif

}
void remove_qnap_disk_node_proc(void)
{
	remove_proc_entry(QNAP_DISK_NODE, NULL);
#ifdef CONFIG_VIRTUAL_DISK_EX
	remove_proc_entry(QNAP_IQN_NODE, NULL);
#endif
}

static int __qnap_sd_prepare_nas_virt_info(struct scsi_device *sdp, u32 *index)
{
	int error = -EBUSY;
	u32 idx = *index;
	bool cmp_ret;

	pr_info("get sd index %d.\n", idx);

#ifdef CONFIG_VIRTUAL_DISK_EX
	if (Is_iSCSI_Index(idx)) { //sdwa ~ sdwz
		int ip = idx - ISCSI_DEV_START_INDEX;
		pr_info("index of iscsi_dev_arr is %d\n", ip);

		/* if got REAL slot already, to setup everything */
		cmp_ret = __cmp_iscsi_dev_arr_slot(ip, ISCSI_DEV_ARR_READY_TO_USE);
		if (cmp_ret) {
			int vpd_len = 64;
			unsigned char *buffer = kmalloc(vpd_len, GFP_KERNEL);
			if (buffer) {
				if (!scsi_get_vpd_page(sdp, 0x80, buffer, vpd_len)) {
					if (iscsi_iqn_arr[ip])
						kfree(iscsi_iqn_arr[ip]);

					iscsi_iqn_arr[ip] = kstrdup(qnap_iqn_node,
						GFP_KERNEL);

					if (iscsi_sn_vpd_arr[ip])
						kfree(iscsi_sn_vpd_arr[ip]);

					iscsi_sn_vpd_arr[ip] = buffer;
					vpd_len = iscsi_sn_vpd_arr[ip][3] + 4;
					iscsi_sn_vpd_arr[ip][vpd_len] = '\0';
					error = 0;

					/* set to USED after everything is ready */
					__set_iscsi_dev_arr_slot(ip,
							ISCSI_DEV_ARR_USED);
				} else
					kfree(buffer);
			} else {
				pr_warn("%s: kmalloc failed!\n", __func__);

				__set_iscsi_dev_arr_slot(ip,
						ISCSI_DEV_ARR_NOT_USED);
			}
		}
	}
#else
	if ((idx >= ISCSI_DEV_START_INDEX) && (idx < (ISCSI_DEV_START_INDEX+26))) { //sdwa ~ sdwz
		int ip = idx - ISCSI_DEV_START_INDEX;

		spin_lock(&qnap_vdisk_sd_index_lock);
		if (!iscsi_dev_arr[ip]) {
			iscsi_dev_arr[ip] = 1;
			error = 0;
		}
		spin_unlock(&qnap_vdisk_sd_index_lock);
	}
#endif
    return error;
}

static int __qnap_get_iscsi_index(struct scsi_device *sdp, u32 *index)
{
#ifdef CONFIG_VIRTUAL_DISK_EX
	int idx;

	spin_lock(&qnap_vdisk_sd_index_lock);
	idx = __QNAP_get_iscsi_free_slot(qnap_iqn_node);
	spin_unlock(&qnap_vdisk_sd_index_lock);

	if (idx >= 0) {
		*index = Get_iSCSI_Index('w', ('a'+idx));	//sdwX
		printk("qnap_iqn_node=%s.\n", qnap_iqn_node);
	}
	else {
#endif
		if (strlen(qnap_disk_node) >= 4 && strncmp(qnap_disk_node, "sd", 2) == 0)
			*index = Get_iSCSI_Index(qnap_disk_node[2], qnap_disk_node[3]);
		else
			*index = 0;
#ifdef CONFIG_VIRTUAL_DISK_EX
	}
#endif
	
	if (!Is_iSCSI_Index(*index))
		return -ENODEV;
	return 0;
}

/* -ENODEV: NOT iscsi disk
 * 0: good to get disk idx
 * others: fail to get disk idx or fail to prepare nas virt info
 */
int QNAP_get_iscsi_index(struct scsi_device *sdp, u32 *index)
{
	int ret = -ENODEV;

	if (is_iscsi_disk(sdp)) {
		ret = __qnap_get_iscsi_index(sdp, index);
		if (!ret) {
			ret = __qnap_sd_prepare_nas_virt_info(sdp, index);
			if (!ret)
				goto exit;

			sdev_printk(KERN_WARNING, sdp, 
					"fail to prepare nas_virt_info.\n");
		}
		
		if (ret == -ENODEV)
			ret = -EINVAL;
		*index = ret;
	}
exit:
	return ret;
}

static void __qnap_clear_iscsi_index(struct scsi_device *sdp, u32 index)
{
	index -= ISCSI_DEV_START_INDEX;

	/* locked by qnap_vdisk_sd_index_lock */
	__set_iscsi_dev_arr_slot(index, ISCSI_DEV_ARR_NOT_USED);

#ifdef CONFIG_VIRTUAL_DISK_EX
	sdev_printk(KERN_WARNING, sdp, "iSCSI LUN %d released\n", index);
	if (iscsi_iqn_arr[index]) {
		kfree(iscsi_iqn_arr[index]);
		iscsi_iqn_arr[index]=NULL;
	}
	if (iscsi_sn_vpd_arr[index]) {
		kfree(iscsi_sn_vpd_arr[index]);
		iscsi_sn_vpd_arr[index]=NULL;
	}
#endif
}

/* -ENODEV: not iscsi disk
 * 0: good to clear disk idx
 */
int QNAP_clear_iscsi_index(struct scsi_device *sdp, u32 index)
{
	int ret = -ENODEV;

	if(Is_iSCSI_Index(index)) {
		__qnap_clear_iscsi_index(sdp, index);
		ret = 0;
	}

	return ret;
}

#ifdef CONFIG_VIRTUAL_DISK_EX
void qnap_init_virtual_disk_ex_vars(void)
{
	memset(iscsi_dev_arr, 0, sizeof(iscsi_dev_arr));
	memset(iscsi_iqn_arr, 0, sizeof(iscsi_iqn_arr));
	memset(iscsi_sn_vpd_arr, 0, sizeof(iscsi_sn_vpd_arr));
	memset(qnap_disk_node, 0, sizeof(qnap_disk_node));
	memset(qnap_iqn_node, 0, sizeof(qnap_iqn_node));
	/* Jay Wei,20150122, Task #11550. Make LUN records in /proc/scsi/qnap_iqn_node 
	 * consistent with reality.
	 */
	memset(lun_status, 0, sizeof(lun_status));
}

#endif


