#ifndef TARGET_CORE_QODX_H
#define TARGET_CORE_QODX_H


#include <linux/types.h>
#include <linux/kernel.h>

/* forward declaration */
struct qnap_se_cmd_dr;
struct se_cmd;
struct se_portal_group;

#if 0
#include "target_core_qodx_scsi.h"

/**/
#endif
struct workqueue_struct *qnap_odx_alloc_rwio_wq(char *name, int idx);
void qnap_odx_destroy_rwio_wq(struct workqueue_struct *wq);
struct workqueue_struct *qnap_odx_alloc_wut_io_wq(char *name, int idx);
void qnap_odx_destroy_wut_io_wq(struct workqueue_struct *wq);
int qnap_odx_alloc_cache_res(void);
void qnap_odx_free_cache_res(void);
void qnap_odx_transport_init(struct qnap_se_cmd_odx *odx_dr,
		struct qnap_tf_ops *q_tf_ops);

int qnap_odx_pre_check_odx(struct se_cmd *cmd);
int qnap_odx_tpg_add_and_get(struct se_portal_group *se_tpg);
void qnap_odx_tpg_del(struct se_portal_group *se_tpg);

void qnap_odx_free_cmd(struct se_cmd *se_cmd);
int qnap_odx_set_scsi_sense_information(struct se_cmd *cmd);


#if 0
void qnap_odx_drop_cmd(struct se_cmd *se_cmd, int type);
int qnap_tmr_odx_discard_all_tokens(struct se_device *se_dev,
	struct se_cmd *se_cmd, struct se_portal_group *se_tpg);

int qnap_it_nexus_odx_discard_all_tokens(char *initiator_name, u8 *isid_buf,
	char *login_ip, struct se_portal_group *se_tpg);
#endif

extern void __attribute__((weak)) qnap_odx_setup_sbc_ops(
	struct qnap_se_cmd_dr *cmd_dr);

extern void __attribute__((weak)) qnap_odx_setup_spc_ops(
	struct qnap_se_cmd_dr *cmd_dr);

#endif

