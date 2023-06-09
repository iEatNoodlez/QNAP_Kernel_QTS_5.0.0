#ifndef TARGET_CORE_QODX_RB_H
#define TARGET_CORE_QODX_RB_H


#include <linux/types.h>
#include <linux/kernel.h>
#include <asm/unaligned.h>

struct tpc_tpg_data;
struct tpc_token_data;
struct tpc_cmd_data;
struct __reg_data;
struct __dev_info;
struct blk_dev_range_desc;


/* odx functions about rb-tree */
struct tpc_token_data *qnap_odx_rb_token_get(
	struct tpc_tpg_data *tpg_p,
	u64 cmd_id_hi, 
	u64 cmd_id_lo, 
	u64 initiator_id_hi, 
	u64 initiator_id_lo,
	bool monitor_cp_req
	);

int qnap_odx_rb_token_put(struct tpc_token_data *token_p);
int qnap_odx_rb_token_add(struct tpc_tpg_data *tpg_p, struct tpc_token_data *token_p);

struct tpc_tpg_data *qnap_odx_rb_tpg_get(u64 tpg_id_hi, u64 tpg_id_lo);
int qnap_odx_rb_tpg_put(struct tpc_tpg_data *tpg_p);
void qnap_odx_rb_tpg_del(struct tpc_tpg_data *tpg_p);

struct tpc_tpg_data *qnap_odx_rb_tpg_add_and_get(
	u64 tpg_id_hi,
	u64 tpg_id_lo
	);

int qnap_odx_rb_cmd_put(struct tpc_cmd_data *tc_p);

struct tpc_cmd_data *qnap_odx_rb_cmd_get(
	struct tpc_tpg_data *tpg_p, 
	struct __reg_data *reg_data,
	bool skip_iid, 
	bool skip_sac
	);

struct tpc_cmd_data *qnap_odx_cmd_add_and_get(
	struct tpc_tpg_data *tpg_p, 
	struct __reg_data *reg_data
	);

void qnap_odx_rb_cmd_del(struct tpc_tpg_data *tpg_p, struct tpc_cmd_data *tc_p);

struct tpc_cmd_data *qnap_odx_rb_cmd_add_and_get(
	struct tpc_tpg_data *tpg_p, 
	struct __reg_data *reg_data
	);

void qnap_odx_rb_cmd_free(struct tpc_cmd_data *tc_p);

int qnap_odx_rb_parse_conflict_token_range(struct tpc_tpg_data *tpg_p,
	struct __dev_info *dev_info, struct blk_dev_range_desc *blk_range_desc, 
	u16 desc_idx, u8 cdb0, u8 cdb1);

int qnap_odx_rb_discard_all_tokens(struct tpc_tpg_data *tpg_p, 
	struct __reg_data *reg_data, int discard_type, char *type_str);

#endif

