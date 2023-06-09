/*******************************************************************************
 * Filename:  target_core_qconfigfs.c
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
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <generated/utsrelease.h>
#include <linux/utsname.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/unistd.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/syscalls.h>
#include <linux/configfs.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>

#include <target/target_core_base.h>
#include <target/target_core_backend.h>
#include <target/target_core_fabric.h>
#include "target_core_qconfigfs.h"
#include "target_core_qlib.h"
#include "target_core_qtransport.h"
#include "../target_core_iblock.h"
#include "../target_core_pr.h"


/* Copied / modified from include/linux/configfs.h */
#define QNAP_CONFIGFS_ATTR(_pfx, _name)			\
struct configfs_attribute _pfx##attr_##_name = {	\
	.ca_name	= __stringify(_name),		\
	.ca_mode	= S_IRUGO | S_IWUSR,		\
	.ca_owner	= THIS_MODULE,			\
	.show		= qnap_##_pfx##_name##_show,	\
	.store		= qnap_##_pfx##_name##_store,	\
}

#define QNAP_CONFIGFS_ATTR_RO(_pfx, _name)		\
struct configfs_attribute _pfx##attr_##_name = {	\
	.ca_name	= __stringify(_name),		\
	.ca_mode	= S_IRUGO,			\
	.ca_owner	= THIS_MODULE,			\
	.show		= qnap_##_pfx##_name##_show,	\
}

/* Copied from target_core_configfs.c */
#define QNAP_CONFIGFS_ATTRIB_SHOW(_name)					\
static ssize_t qnap_##_name##_show(struct config_item *item, char *page)	\
{										\
	return snprintf(page, PAGE_SIZE, "%u\n", to_attrib(item)->_name);	\
}

static inline struct se_dev_attrib *to_attrib(struct config_item *item)
{
	return container_of(to_config_group(item), struct se_dev_attrib,
			da_group);
}

static inline struct se_device *to_device(struct config_item *item)
{
	return container_of(to_config_group(item), struct se_device, dev_group);
}

static ssize_t qnap_emulate_v_sup_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	bool flag;
	int ret;

	ret = strtobool(page, &flag);
	if (ret < 0)
		return ret;

	da->dev_attr_dr.emulate_v_sup = flag;

	/* Set WCE to 0 while changing V_SUP */
	if (da->da_dev->transport->set_write_cache)
		da->da_dev->transport->set_write_cache(da->da_dev, false);
	else
		da->emulate_write_cache = 0;

	pr_debug("dev[%p]: SE Device V_SUP_EMULATION flag: %d\n",
			da->da_dev, da->dev_attr_dr.emulate_v_sup);
	return count;
}

static ssize_t qnap_emulate_v_sup_show(
	struct config_item *item,
	char *page
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;
	int ret;

	if (!da->da_dev)
		return -ENODEV;
	return snprintf(page, PAGE_SIZE, "%d\n", dev_attr_dr->emulate_v_sup);
}

static ssize_t qnap_emulate_fua_write_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	bool is_fio_blkdev;
	bool flag;
	int ret;
	struct se_dev_attrib *da = to_attrib(item);
	struct se_device *se_dev = da->da_dev;
	struct iblock_dev *ib_dev = NULL;
	struct request_queue *q = NULL;

	ret = strtobool(page, &flag);
	if (ret < 0)
		return ret;

	da->emulate_fua_write = flag;

	if (qlib_is_fio_blk_dev(&se_dev->dev_dr) == 0)
		is_fio_blkdev = true;
	else if (qlib_is_ib_fbdisk_dev(&se_dev->dev_dr) == 0)
		is_fio_blkdev = false;
	else
		return -ENODEV;

	if (is_fio_blkdev == false) {
		ib_dev = (struct iblock_dev *)se_dev->transport->get_dev(se_dev);
		q = bdev_get_queue(ib_dev->ibd_bd);

		/* Use blk_queue_write_cache() maybe? */
		spin_lock_irq(&q->queue_lock);
		if (da->emulate_fua_write)
			blk_queue_flag_set(QUEUE_FLAG_FUA, q);
		else
			blk_queue_flag_clear(QUEUE_FLAG_FUA, q);
		spin_unlock_irq(&q->queue_lock);
	}

	pr_debug("dev[%p]: SE Device FUA_WRITE_EMULATION flag: %d\n",
			da->da_dev, da->emulate_fua_write);
	return count;
}

QNAP_CONFIGFS_ATTRIB_SHOW(emulate_fua_write);
QNAP_CONFIGFS_ATTR(, emulate_v_sup);
QNAP_CONFIGFS_ATTR(, emulate_fua_write);

static ssize_t qnap_lun_index_show(
	struct config_item *item,
	char *page
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;

	return snprintf(page, PAGE_SIZE, "%u\n", dev_attr_dr->lun_index);
}

static ssize_t qnap_lun_index_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *attr_dr = &da->dev_attr_dr;
	u32 val;
	int ret;

	ret = kstrtou32(page, 0, &val);
	if (ret < 0)
		return ret;

	if (val > 255)
		return -EINVAL;

	attr_dr->lun_index = val;
	return count;
}

QNAP_CONFIGFS_ATTR(, lun_index);

#ifdef SUPPORT_TP
/* for threshold notification */
static ssize_t qnap_tp_threshold_enable_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;
	bool flag;
	int ret;

	ret = strtobool(page, &flag);
	if (ret < 0)
		return ret;

	/*
	 * We expect this value to be non-zero when generic Block Layer
	 * Discard supported is detected iblock_create_virtdevice().
	 */
	if (flag && !dev_attr_dr->tp_threshold_percent) {
		pr_err("TP threshold enable not supported\n");
		return -ENOSYS;
	}

	dev_attr_dr->tp_threshold_enable = flag;
	return count;
}

static ssize_t qnap_tp_threshold_percent_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;
	u32 val;
	int ret;

	ret = kstrtou32(page, 0, &val);
	if (ret < 0)
		return ret;

	if (val > 100)
		return -EINVAL;

	dev_attr_dr->tp_threshold_percent = val;
	return count;
}

static ssize_t qnap_allocated_show(
	struct config_item *item,
	char *page
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;
	int ret;

	if (!da->da_dev)
		return -ENODEV;

	ret = qnap_transport_get_thin_allocated(da->da_dev);
	if (ret != 0)
		return ret;

	return snprintf(page, PAGE_SIZE, "%llu\n", dev_attr_dr->allocated);
}

static ssize_t qnap_tp_threshold_enable_show(
	struct config_item *item, 
	char *page
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;

	return snprintf(page, PAGE_SIZE, "%u\n", dev_attr_dr->tp_threshold_enable);
}

static ssize_t qnap_tp_threshold_percent_show(
	struct config_item *item, 
	char *page
	)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct qnap_se_dev_attr_dr *dev_attr_dr = &da->dev_attr_dr;

	return snprintf(page, PAGE_SIZE, "%u\n", dev_attr_dr->tp_threshold_percent);
}

QNAP_CONFIGFS_ATTR(, tp_threshold_enable);
QNAP_CONFIGFS_ATTR(, tp_threshold_percent);
QNAP_CONFIGFS_ATTR_RO(, allocated);
#endif

static ssize_t qnap_target_dev_qfbc_supported_show(
	struct config_item *item,
	char *page
	)
{
	return snprintf(page, PAGE_SIZE, "%u\n", to_device(item)->dev_dr.fast_blk_clone);
}

static ssize_t qnap_target_dev_qfbc_enable_show(
	struct config_item *item,
	char *page
	)
{
	return snprintf(page, PAGE_SIZE, "%u\n", to_device(item)->dev_dr.fbc_control);
}

static ssize_t qnap_target_dev_qfbc_enable_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_device *dev = to_device(item);
	bool flag;
	int ret;

	ret = strtobool(page, &flag);
	if (ret < 0)
		return ret;

	if (!dev->dev_dr.fast_blk_clone)
		return -EINVAL;

	spin_lock(&dev->dev_dr.fbc_control_lock);
	dev->dev_dr.fbc_control = flag;
	spin_unlock(&dev->dev_dr.fbc_control_lock);

	return count;

}

QNAP_CONFIGFS_ATTR_RO(target_dev_, qfbc_supported);
QNAP_CONFIGFS_ATTR(target_dev_, qfbc_enable);

static ssize_t qnap_target_dev_provision_show(
	struct config_item *item,
	char *page)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;

	if (!(dr->dev_flags & QNAP_DF_USING_PROVISION))
		return 0;

	return snprintf(page, PAGE_SIZE, "%s\n", dr->dev_provision);
}

static ssize_t qnap_target_dev_provision_store(
	struct config_item *item,
	const char *page,
	size_t count)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	struct se_hba *hba = dev->se_hba;
	ssize_t read_bytes;
	unsigned char dev_provision_str[QNAP_SE_DEV_PROVISION_LEN];

	if (count > (QNAP_SE_DEV_PROVISION_LEN - 1)) {
		pr_err("provision count: %d exceeds "
			"QNAP_SE_DEV_PROVISION_LEN - 1: %u\n", (int)count,
			QNAP_SE_DEV_PROVISION_LEN - 1);
		return -EINVAL;
	}

	if (dr->dev_attr_write_once_flag & QNAP_DEV_ATTR_PROVISION_WRITE_ONCE) {
		pr_err("se_dev_provision was set already. can't update again.\n");
		return -EINVAL;
	}

	memset(dev_provision_str, 0, sizeof(dev_provision_str));
	read_bytes = snprintf(&dev_provision_str[0], QNAP_SE_DEV_PROVISION_LEN,
			"%s", page);
	if (!read_bytes){
		pr_err("cat't format dev_provision_str string\n");
		return -EINVAL;
	}

	if (dev_provision_str[read_bytes - 1] == '\n')
		dev_provision_str[read_bytes - 1] = '\0';

	/* check the dev provision string format */
	if (strncasecmp(dev_provision_str, "thin", sizeof(dev_provision_str)) 
	&& strncasecmp(dev_provision_str, "thick", sizeof(dev_provision_str))
	)
	{
		pr_err("neither thick nor thin for dev_provision string\n");
		return -EINVAL;
	}

	read_bytes = snprintf(&dr->dev_provision[0], QNAP_SE_DEV_PROVISION_LEN,
			"%s", page);
	if (!read_bytes)
		return -EINVAL;
	if (dr->dev_provision[read_bytes - 1] == '\n')
		dr->dev_provision[read_bytes - 1] = '\0';

	dr->dev_flags |= QNAP_DF_USING_PROVISION;
	dr->dev_attr_write_once_flag |= QNAP_DEV_ATTR_PROVISION_WRITE_ONCE;

	pr_debug("Target_Core_ConfigFS: %s/%s set provision: %s\n",
		config_item_name(&hba->hba_group.cg_item),
		config_item_name(&dev->dev_group.cg_item),
		dr->dev_provision);

	return read_bytes;
}

static ssize_t qnap_target_dev_naa_vendor_show(
	struct config_item *item,
	char *page)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;

	if (!(dr->dev_flags & QNAP_DF_USING_NAA))
		return 0;

	return snprintf(page, PAGE_SIZE, "%s\n", dr->dev_naa);
}

static ssize_t qnap_target_dev_naa_vendor_store(
	struct config_item *item,
	const char *page,
	size_t count)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	struct se_hba *hba = dev->se_hba;
	ssize_t read_bytes;

	if (count > (QNAP_SE_DEV_NAA_LEN - 1)) {
		pr_err("naa count: %d exceeds SE_DEV_NAA_LEN-1: %u\n", 
			(int)count, QNAP_SE_DEV_NAA_LEN - 1);
		return -EINVAL;
	}

	read_bytes = snprintf(&dr->dev_naa[0], QNAP_SE_DEV_NAA_LEN,
			"%s", page);
	if (!read_bytes)
		return -EINVAL;

	if (dr->dev_naa[read_bytes - 1] == '\n')
		dr->dev_naa[read_bytes - 1] = '\0';

	if (dr->dev_attr_write_once_flag & QNAP_DEV_ATTR_NAA_WRITE_ONCE) {
		pr_err("naa was setup already. Can't update again.\n");
		return -EINVAL;
	}

	dr->dev_attr_write_once_flag |= QNAP_DEV_ATTR_NAA_WRITE_ONCE;
	dr->dev_flags |= QNAP_DF_USING_NAA;

	pr_debug("Target_Core_ConfigFS: %s/%s set naa: %s\n",
		config_item_name(&hba->hba_group.cg_item),
		config_item_name(&dev->dev_group.cg_item),
		dr->dev_naa);

	return read_bytes;
}

struct configfs_attribute target_dev_attr_naa_vendor = {
	.ca_name	= __stringify(naa),
	.ca_mode	= S_IRUGO | S_IWUSR,
	.ca_owner	= THIS_MODULE,
	.show		= qnap_target_dev_naa_vendor_show,
	.store		= qnap_target_dev_naa_vendor_store,
};

static ssize_t qnap_target_dev_naa_code_show(
	struct config_item *item,
	char *page)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;

	u8 hex[] = {'0', '1', '2', '3', '4', '5', '6', '7',
		'8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	int i;
	u8 tmp_buf[QNAP_SE_DEV_NAA_LEN + 1], naa_buf[QNAP_SE_DEV_NAA_LEN + 1], c;

	if (!(dr->dev_flags & QNAP_DF_USING_NAA))
		return 0;

	memset(tmp_buf, 0, sizeof(tmp_buf));
	memset(naa_buf, 0, sizeof(naa_buf));

	qlib_get_naa_6h_code((void *)dev, &dr->dev_naa[0], tmp_buf, 
		qnap_transport_parse_naa_6h_vendor_specific);
	
	for (i = 0; i < 16; i++) {
		c = tmp_buf[i];
		naa_buf[i*2 + 0] = hex[c >> 4];
		naa_buf[i*2 + 1] = hex[c & 0xf];
	}
	return snprintf(page, PAGE_SIZE, "%s\n", naa_buf);
}

/* adamhsu 2013/06/07 - Support to set the logical block size from NAS GUI.
 *
 * Cause of the attribute item information in attrib folder are still not be set
 * before to enable the LU, we need to put this information into another attribute
 * item.
 */
static ssize_t qnap_target_dev_qlbs_show(
	struct config_item *item,
	char *page
	)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	ssize_t rb;

	if (!(dr->dev_flags & QNAP_DF_USING_QLBS))
		return 0;

	rb = snprintf(page, PAGE_SIZE, "%llu\n", (u64)dr->dev_qlbs);
	return rb;
}

static ssize_t qnap_target_dev_qlbs_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_device *dev = to_device(item);
	struct qnap_se_dev_dr *dr = &dev->dev_dr;
	struct se_hba *hba = dev->se_hba;
	unsigned long val = 0;
	int ret = 0;

	ret = kstrtoul(page, 0, &val);
	if (ret < 0) {
		pr_err("%s: kstrtoul() failed with  ret: %d\n", __func__, ret);
		return -EINVAL;
	}

	if ((val != 512) && (val != 4096)) {
		pr_err("%s: dev[%p]: Illegal value for block_device: %lu"
			" for se sub device, must be 512, or 4096\n", __func__, 
			dev, val);
		return -EINVAL;
	}

	if (dr->dev_attr_write_once_flag & QNAP_DEV_ATTR_QLBS_WRITE_ONCE) {
		pr_err("qlbs was setup already. Can't update again.\n");
		return -EINVAL;
	}

	dr->dev_attr_write_once_flag |= QNAP_DEV_ATTR_QLBS_WRITE_ONCE;
	dr->dev_flags |= QNAP_DF_USING_QLBS;
	dr->dev_qlbs = val;

	pr_debug("Target_Core_ConfigFS: dev[%p], %s/%s set qlbs: %u\n",
		dev, config_item_name(&hba->hba_group.cg_item), 
		config_item_name(&dev->dev_group.cg_item),
		dr->dev_qlbs
		);

	return count;
}

QNAP_CONFIGFS_ATTR(target_dev_, provision);
QNAP_CONFIGFS_ATTR_RO(target_dev_, naa_code);
QNAP_CONFIGFS_ATTR(target_dev_, qlbs);

ssize_t qnap_target_dev_read_deletable_show(
	struct config_item *item,
	char *page
	)
{
	struct se_device *dev = to_device(item);
	ssize_t rb;
	int val;

	val = atomic_read(&dev->dev_dr.hit_read_deletable);
	rb = snprintf(page, PAGE_SIZE, "%llu\n", (u64)val);
	return rb;
}

ssize_t qnap_target_dev_read_deletable_store(
	struct config_item *item,
	const char *page,
	size_t count
	)
{
	struct se_device *dev = to_device(item);
	unsigned long val = 0;
	int ret = 0;
	char *naa_buf = NULL;

	ret = kstrtoul(page, 0, &val);
	if (ret < 0) {
		pr_err("%s: kstrtoul() failed with  ret: %d\n", __func__, ret);
		return -EINVAL;
	}

	if ((val != 0) && (val != 1)) {
		pr_err("%s: Illegal value. Must be 0 or 1\n", __func__);
		return -EINVAL;
	}

	naa_buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!naa_buf)
		return -ENOMEM;

	atomic_set(&dev->dev_dr.hit_read_deletable, (int)val);

	/* buffer size passed to target_core_show_dev_naa_code() must be PAGE_SIZE */
	qnap_target_dev_naa_code_show(item, naa_buf);

	if (naa_buf[strlen(naa_buf) - 1] == '\n')
		naa_buf[strlen(naa_buf) - 1] = 0x00;

	pr_info("dev[naa:%s] read_deletable status was %s\n", naa_buf,
			((val == 1) ? "set": "clear"));

	kfree(naa_buf);
	return count;
}

QNAP_CONFIGFS_ATTR(target_dev_, read_deletable);

static ssize_t qnap_productid_show(struct config_item *item, char *page)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct se_device *dev = da->da_dev;

	return snprintf(page, PAGE_SIZE, "%s\n", dev->t10_wwn.model);
}

static ssize_t qnap_productid_store(struct config_item *item,
		const char *page, size_t count)
{
	struct se_dev_attrib *da = to_attrib(item);
	struct se_device *dev = da->da_dev;
	ssize_t read_bytes;

	if (count > 15) {
		pr_err("modle name count: %d exceeds"
			" PRODUCT_ID_LEN-1: %u\n", (int)count,
			15);
		return -EINVAL;
	}

	read_bytes = snprintf(&dev->t10_wwn.model[0], 15,
			"%s", page);
	if (!read_bytes)
		return -EINVAL;
	if (dev->t10_wwn.model[read_bytes - 1] == '\n')
		dev->t10_wwn.model[read_bytes - 1] = '\0';
	dev->t10_wwn.model_is_set = 1;

	return read_bytes;
}
QNAP_CONFIGFS_ATTR(, productid);
