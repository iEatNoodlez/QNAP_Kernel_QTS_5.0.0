#ifndef ISCSI_TARGET_QCONFIGFS_H
#define ISCSI_TARGET_QCONFIGFS_H

#ifdef SUPPORT_SINGLE_INIT_LOGIN

ssize_t iscsi_stat_tgt_attr_cluster_enable_show(
	struct config_item *item, char *page);

ssize_t iscsi_stat_tgt_attr_cluster_enable_store(
	struct config_item *item, const char *page, size_t count);
#endif

#endif
