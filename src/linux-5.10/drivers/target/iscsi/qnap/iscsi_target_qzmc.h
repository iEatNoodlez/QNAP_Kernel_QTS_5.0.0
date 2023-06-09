#ifndef __ISCSI_TARGET_QZMC_H__
#define __ISCSI_TARGET_QZMC_H__

#include <linux/types.h>

/**/
struct iscsit_zmc_ops;
struct iscsi_cmd;
struct iscsi_conn;

/**/
extern int __attribute__((weak)) qnap_iscsit_zmc_setup_transport_zmc_ops(
	struct iscsi_conn *conn, struct iscsi_cmd *cmd);

extern void __attribute__((weak)) qnap_iscsit_zmc_release_cmd_priv(
	struct iscsi_cmd *cmd);

extern int __attribute__((weak)) qnap_iscsit_zmc_create_cmd_priv(
	struct iscsi_conn *conn, struct iscsi_cmd *cmd);

extern void __attribute__((weak)) qnap_iscsit_zmc_release_conn_priv(
	struct iscsi_conn *conn);

extern void __attribute__((weak)) qnap_iscsit_zmc_create_conn_priv(
	struct iscsi_conn *conn);

extern void __attribute__((weak)) qnap_iscsit_zmc_register(struct iscsit_zmc_ops *p);
extern void __attribute__((weak)) qnap_iscsit_zmc_unregister(struct iscsit_zmc_ops *p);

#endif

