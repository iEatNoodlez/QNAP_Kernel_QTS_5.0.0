#ifndef ISCSI_TARGET_QTRANSPORT_H
#define ISCSI_TARGET_QTRANSPORT_H

#ifdef CONFIG_MACH_QNAPTS

#define BASE64	2	/* iscsi_target_nego.h */
int qnap_iscsi_chap_r_base64_to_hex(unsigned char *dst, unsigned char *src, int buf_len);
int qnap_iscsi_chap_c_base64_to_hex(unsigned char *dst, unsigned char *src);

#ifdef QNAP_KERNEL_STORAGE_V2
extern struct workqueue_struct *acl_notify_info_wq;
extern struct kmem_cache *acl_notify_info_cache;

struct notify_data {
	char path[512];
	char *data;
	ssize_t data_size;
	struct work_struct work;
	struct se_node_acl *se_nacl;
};

void qnap_create_acl_notify_info_wq(void);
void qnap_destroy_acl_notify_info_wq(void);
void qnap_create_acl_notify_info_cache(void);
void qnap_destroy_acl_notify_info_cache(void);
int qnap_put_acl_notify_info_work(struct notify_data *data);

static inline void *qnap_alloc_acl_notify_info_cache(void)
{
	return kmem_cache_zalloc(acl_notify_info_cache, GFP_ATOMIC);
}

static inline void qnap_free_acl_notify_info_cache(
	struct notify_data *data
	)
{
	kmem_cache_free(acl_notify_info_cache, data);
}
#endif

#ifdef SUPPORT_ISCSI_ZERO_COPY

struct RECV_FILE_CONTROL_BLOCK
{
	struct page *rv_page;
	loff_t rv_pos;
	size_t  rv_count;
	void *rv_fsdata;
};

ssize_t qnap_iscsit_zc_splice(struct se_cmd *se_cmd, struct socket *sock,
	u32 hdr_off, u32 size);

int qnap_iscsit_zc_splice_work_on_scsi_op(struct se_cmd *se_cmd);
int qnap_iscsit_check_do_zc_splice(struct se_cmd *se_cmd);

#endif

#ifdef ISCSI_D4_INITIATOR
ssize_t qnap_lio_nacl_show_info(struct se_node_acl *se_nacl, char *page);
void qnap_lio_copy_node_attributes(struct se_node_acl *dest, 
	struct se_node_acl *src);
#endif

#ifdef SUPPORT_SINGLE_INIT_LOGIN
int qnap_iscsit_search_tiqn_for_initiator(struct iscsi_tiqn *tiqn, 
	char *InitiatorName);
#endif

int qnap_iscsit_check_received_cmdsn(struct iscsi_session *sess, u32 cmdsn);
void qnap_iscsi_set_delay_remove(struct se_cmd *se_cmd, bool set);
void __qnap_iscsi_set_delay_remove(struct iscsi_cmd *cmd, bool set);

int qnap_transport_tmr_handle_send_response(struct iscsi_cmd *cmd, 
	struct iscsi_conn *conn, int *ret_code);

void qnap_transport_tmr_handle_send_datain(struct iscsi_cmd *cmd,
	struct iscsi_conn *conn, struct iscsi_datain_req *dr,
	int *end_of_datain_req, int tas);

#if defined(ISCSI_LOGIN_REDIRECT)
int sepstrtoipport(const char *str,
    char *get_ip,
    u16 *get_port);

void login_redirect_cnt_add(
    struct iscsi_conn *conn,
    const char *get_ip,
    u16 get_port);

int login_redirect_algo_rr(
    struct iscsi_conn *conn,
    char *get_ip,
    u16 *get_port,
    u8 *get_type);

int iscsit_tx_login_rsp_redirect(
    struct iscsi_conn *conn,
    u8 status_class,
    u8 status_detail);

void login_redirect_cnt_sub(
    struct iscsi_conn *conn);
#endif

#endif

void qnap_nl_create_func_table(void);
void qnap_nl_send_post_log(int conn_type, int log_type, 
	struct iscsi_session *sess, char *ip);

void qnap_nl_rt_conf_update(int conn_type, int log_type, char *key, char *val);
int qnap_iscsi_lio_drop_cmd_from_lun_acl(struct se_lun *se_lun);

#ifdef ISCSI_MULTI_INIT_ACL
int qnap_iscsit_get_matched_initiator_count(
	struct iscsi_tiqn *tiqn,
	char *initiator_name
	);
#endif

/* For target_core_fabric_ops */
char *qnap_iscsi_get_initiator_name(struct se_cmd *se_cmd);
u32 qanp_iscsi_get_task_tag(struct se_cmd *se_cmd);
struct sockaddr_storage *qnap_iscsi_get_login_ip(struct se_cmd *se_cmd);
u32 qnap_iscsi_get_cmdsn(struct se_cmd *se_cmd);
bool qnap_check_inaddr_loopback(struct sockaddr_storage *ss);
u8 qnap_iscsit_check_allow_np(struct iscsi_portal_group *tpg, struct iscsi_conn *conn, enum iscsit_transport_type network_transport);

#endif
