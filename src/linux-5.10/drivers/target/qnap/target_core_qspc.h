#ifndef TARGET_CORE_QSPC_H
#define TARGET_CORE_QSPC_H

/**/
struct qnap_se_dev_dr;
struct qnap_spc_odx_ops;

typedef struct _logsense_func_table{
	u8	page_code;
	u8	sub_page_code;
	int	(*logsense_func)(struct se_cmd *se_cmd, u8 *buf);
	int	end;
}__attribute__ ((packed)) LOGSENSE_FUNC_TABLE;

typedef struct lbp_log_parameter_format{
	u8  parameter_code[2];		// byte 0~1
	u8  format_and_linking:2;	// byte 2
	u8  tmc:2;
	u8  etc:1;
	u8  tsd:1;
	u8  obsolete:1;
	u8  du:1;
	u8  parameter_length;		// byte 3
	u8  resource_count[4];		// byte 4~7
	u8  scope:2;			// byte 8
	u8  reserved0:6;
	u8  reserved1[3];		// byte 9~11
}__attribute__ ((packed)) LBP_LOG_PARAMETER_FORMAT;

typedef struct threshold_desc_format{
	u8  threshold_arming:3;	// byte 0
	u8  threshold_type:3;
	u8  reserved0:1;
	u8  enabled:1;
	u8  threshold_resource;	// byte 1
	u8  reserved1[2];	// byte 2 ~ byte 3
	u8  threshold_count[4];	// byte 4 ~ byte 7
} __attribute__ ((packed)) THRESHOLD_DESC_FORMAT;


/**/
int qnap_spc_get_ac_and_uc(struct se_device *se_dev, 
		sector_t *ret_avail_blks, sector_t *ret_used_blks);

/**/
extern void __attribute__((weak)) qnap_setup_spc_ops(struct qnap_se_cmd_dr *cmd_dr);

#endif
