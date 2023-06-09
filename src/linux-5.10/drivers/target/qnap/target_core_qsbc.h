#ifndef TARGET_CORE_QSBC_H
#define TARGET_CORE_QSBC_H

/**/
struct qnap_se_cmd_dr;
struct qnap_sbc_odx_ops;

/* this defines max size (xx MB) passed to filemap() per Get Lba Status call */
#define MAX_FILEMAP_SIZE		(64 << 20)

#define PROVISIONING_GROUP_DESC_LEN	(20)

/* sbc4r13, p 140 */
enum {
	PROVISIONING_STATUS_CODE_MAPPED_OR_UNKNOWN = 0,
	PROVISIONING_STATUS_CODE_DEALLOCATED = 1,
	PROVISIONING_STATUS_CODE_ANCHORED = 2,	
	PROVISIONING_STATUS_CODE_MAPPED = 3,
	PROVISIONING_STATUS_CODE_UNKNOWN_PROVISIONING_STATUS = 4,
};

typedef enum{
	VPD_B2h_PROVISION_TYPE_NONE = 0x0 , // not report provisioning type
	VPD_B2h_PROVISION_TYPE_RP         , // provisioning type is resource provisioning
	VPD_B2h_PROVISION_TYPE_TP         , // provisioning type is thin provisioning
	MAX_VPD_B2h_PROVISION_TYPE        ,
}VPD_B2h_PROVISION_TYPE;


/** 
 * @enum      THRESHOLD_TYPE
 * @brief     Define threshold type value
 */
typedef enum{
	THRESHOLD_TYPE_SOFTWARE = 0x0 ,
	MAX_THRESHOLD_TYPE            ,
} THRESHOLD_TYPE;

/** 
 * @enum      THRESHOLD_ARM_TYPE
 * @brief     THRESHOLD_ARM_XXX defines which type will be triggerred when resource was changed
 */
typedef enum{
	THRESHOLD_ARM_DESC = 0x0,
	THRESHOLD_ARM_INC       ,
	MAX_THRESHOLD_ARM_TYPE  ,
} THRESHOLD_ARM_TYPE;

/** 
 * @enum      LBP_LOG_PARAMS_TYPE
 * @brief     Define the logical block provisioning parameter type
 */
typedef enum{
	LBP_LOG_PARAMS_AVAILABLE_LBA_MAP_RES_COUNT      = 0x001,
	LBP_LOG_PARAMS_USED_LBA_MAP_RES_COUNT           = 0x002,
	LBP_LOG_PARAMS_DEDUPLICATED_LBA_MAP_RES_COUNT   = 0x100,
	LBP_LOG_PARAMS_COMPRESSED_LBA_MAP_RES_COUNT     = 0x101,
	LBP_LOG_PARAMS_TOTAL_LBA_MAP_RES_COUNT          = 0x102,
	MAX_LBP_LOG_PARAMS_TYPE,
}LBP_LOG_PARAMS_TYPE;

/* sbc3r35j, page 116 */
typedef struct lba_status_desc{
	u8	lba[8];
	u8	nr_blks[4];
	u8	provisioning_status:4;
	u8	reserved0:4;
	u8	reserved1[3];
} __attribute__ ((packed)) LBA_STATUS_DESC;

/**/
static inline int __qnap_sbc_get_threshold_exp(
	unsigned long long total
	)
{

	/* if the logical block provisioning threshold sets are supported, 
	 * then the threshold exponent shall be a non-zero value selected 
	 * such taht:
	 *
	 * (capacity /  2^threshold_exponent) < 2^32
	 *
	 * where:
	 *
	 * capacity is 1 + the LBA of the last logical block as returned in the 
	 * READ CAPACITY (16) parameter data (see 5.19.2) (i.e., the number of 
	 * logical blocks on the direct access block device);
	 *
	 * threshold exponent is the contents of the THRESHOLD EXPONENT field; and
	 *
	 * 2(32) is the constant value 1_0000_0000h (i.e., 4 294 967 296).
	 */
	int count = 0;

	while (total) {
		if (total <= 0x00000000ffffffffULL)
			break;

		total -= 0x0000000100000000ULL;
		count++;
	}

	if (!count)
		return 1;

	if (total)
		count++;

	return ((count + 2) >> 1);

}

/**/
unsigned int qnap_sbc_get_io_min(struct se_device *se_dev);
unsigned int qnap_sbc_get_io_opt(struct se_device *se_dev);

/**/
extern void __attribute__((weak)) qnap_setup_sbc_ops(struct qnap_se_cmd_dr *cmd_dr);

#endif
