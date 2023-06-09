#ifndef TARGET_CORE_QODX_SCSI_H
#define TARGET_CORE_QODX_SCSI_H


#include <linux/types.h>
#include <linux/kernel.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

/**/
struct id_cscd_desc;

/**/
#define IS_TPC_SCSI_OP(cdb_byte0) \
	((cdb_byte0 == EXTENDED_COPY) || (cdb_byte0 == RECEIVE_COPY_RESULTS))

#define IS_TPC_SCSI_PT_OP(cdb_byte1) (((cdb_byte1 & 0x1f) == 0x10))
#define IS_TPC_SCSI_WUT_OP(cdb_byte1) (((cdb_byte1 & 0x1f) == 0x11))
#define IS_TPC_SCSI_RRTI_OP(cdb_byte1) (((cdb_byte1 & 0x1f) == 0x07))

#define SPC_SENSE_INFO_VALIDITY_OFFSET		0
#define SPC_SENSE_SENSE_KEY_OFFSET		2
#define SPC_SENSE_ADD_SENSE_LEN_OFFSET		7
#define SPC_SENSE_DESC_TYPE_OFFSET		8
#define SPC_SENSE_ADDITIONAL_DESC_LEN_OFFSET	9
#define SPC_SENSE_VALIDITY_OFFSET		10
#define SPC_SENSE_ASC_KEY_OFFSET		12
#define SPC_SENSE_ASCQ_KEY_OFFSET		13


/*
 * FIXED ME !! FIXED ME !!
 */
#define ROD_TIMEOUT                             (3500)  /* 3500 msec*/

#define SUPPORT_ROD_CSCD_DESC                   0
#define SUPPORT_TOKEN_IN_BIT                    0
#define SUPPORT_NONZERO_ROD_TYPE                1
#define SUPPORT_ROD_TOKEN_DESC_IN_TPC_VPD       0 // disable first


/* SPC4R36, page 783 */
#define SUPPORT_CSCD_ID_F800                    0
#if ((SUPPORT_ROD_CSCD_DESC == 1) && (SUPPORT_NONZERO_ROD_TYPE == 1) && (SUPPORT_TOKEN_IN_BIT == 1))
#undef SUPPORT_CSCD_ID_F800
#define SUPPORT_CSCD_ID_F800                    1
#endif

#define BLK_DEV_ROD_TOKEN_LIMIT_DESC_LEN        (36) /* SBC3R31, page 281 */
#define ROD_DEV_TYPE_SPECIFIC_DESC_LEN          (48) /* SPC4R36, page 781 */
#define ROD_TYPE_DESC_LEN                       (64) /* SPC4R36, page 783 */

/**/
#define ROD_TOKEN_MIN_SIZE                      0x200
#define ROD_TYPE_SPECIFICED_BY_ROD_TOKEN        0x00000000
#define ROD_TYPE_ACCESS_UPON_REFERENCE          0x00010000
#define ROD_TYPE_PIT_COPY_D4                    0x00800000
#define ROD_TYPE_PIT_COPY_CHANGE_VULNERABLE     0x00800001
#define ROD_TYPE_PIT_COPY_CHANGE_PERSISTENT     0x00800002
#define ROD_TYPE_PIT_COPY_CHANGE_ANY            0x0080ffff
#define ROD_TYPE_BLK_DEV_ZERO                   0xffff0001

/*
 * From include/scsi/scsi_cmnd.h:SCSI_SENSE_BUFFERSIZE, currently
 * defined 96, but the real limit is 252 (or 260 including the header)
 */
#define ROD_SENSE_DATA_LEN                      SCSI_SENSE_BUFFERSIZE

/*
 * FIXED ME !! FIXED ME !! SPC4R36, page 777
 *
 * The remote_tokens filed indicates the level of support the copy manager
 * provides for ROD tokens that are NOT created by the copy manager that is
 * processing the copy operation
 *
 * p.s.
 *
 * We need to figure out what this is !!!
 */
#define R_TOKENS_CODE_0     1
#define R_TOKENS_CODE_4     0
#define R_TOKENS_CODE_6     0
#define R_TOKENS_KEY        (R_TOKENS_CODE_0 + R_TOKENS_CODE_4 + R_TOKENS_CODE_6)  

#if (R_TOKENS_KEY != 1)
#error Only one setting can be set to 1 !!
#endif


/* There are two types for timeout value. One is for ROD token life-time and
 * the other is tpc obj life-time, that the obj will record the some data / 
 * status like copy operation or others. 
 * 
 * Specification defines we need to keep the data after the completion of copy 
 * operation for some 3rd-party copy commands.
 */

/* These timeout value relate to token expired time */
#define MAX_INACTIVITY_TIMEOUT          32      /* second unit */
#define D4_INACTIVITY_TIMEOUT           30      /* second unit */
#define TPC_LIFETIME                    (MAX_INACTIVITY_TIMEOUT + 4)

/* SPC4R36, page 424 
 *
 * This value indicates the number of msecs that the copy manager recommands that
 * the application client wait before send another 3rd-party copy command on the
 * same I_T nexus with the same list identifier.
 *
 * If the COPY OPERATION STATUS field is set to 01h,02h,03h,04h or 60h, this value
 * shall be set to 0xffff_fffe. If this value is set to 0xffff_ffff, it means
 * copy manager is unable to recommand a delay interval.
 */
#define ESTIMATE_STATUS_UPDATE_DELAY	(1 * 1000)  /* FIXED ME */


/* FIXED ME !!
 * SPC4R36, page 778
 * This OPTIMAL BLOCK ROD LEN GRANULARITY field is two bytes (0xffff) ONLY
 */
#define OPTIMAL_BLK_ROD_LEN_GRANULARITY_IN_BYTES	0x400000 /* 4MB */
#define OPTIMAL_TRANSFER_SIZE_IN_BYTES			0x4000000 /* 64 MB */

/* TBD
 * Set to 128 MB for max transfer size. Also, remember Windows expects the
 * copy manager to complete the synchronous offload read/write SCSI commands
 * within 4 seconds. We may reduce the transfer size for some cases.
 * The wrost case is the tester will use only one drive to do WHLK testing :(
 *
 * https://docs.microsoft.com/en-us/windows-hardware/drivers/storage/offloaded-data-transfer
 */
#define MAX_TRANSFER_SIZE_IN_BYTES      (OPTIMAL_TRANSFER_SIZE_IN_BYTES * 2)


/* Here to do preprocessor-condition checking. We hope all byte counts
 * which can be divided by 4096 bytes at least
 */
#if (OPTIMAL_TRANSFER_SIZE_IN_BYTES & (PAGE_SIZE - 1))
#error error !! OPTIMAL_TRANSFER_SIZE_IN_BYTES shall be divided by 4096 bytes
#endif

#if (MAX_TRANSFER_SIZE_IN_BYTES & (PAGE_SIZE - 1))
#error error !! MAX_TRANSFER_SIZE_IN_BYTES shall be divided by 4096 bytes
#endif

#if (OPTIMAL_BLK_ROD_LEN_GRANULARITY_IN_BYTES & (PAGE_SIZE - 1))
#error error !! OPTIMAL_BLK_ROD_LEN_GRANULARITY_IN_BYTES shall be divided by 4096 bytes
#endif


/**/
/*
 * @enum      TRANSFER_COUNT_UNIT
 * @brief
 * @note      SPC4R36, page 425
 */
typedef enum{
    UNIT_BYTES      = 0x0,      /* 2^0 = 1 */
    UNIT_KB         = 0x1,      /* 2^10 = 1024 */
    UNIT_MB         = 0x2,      /* 2^20 */
    UNIT_GB         = 0x3,      /* 2^30 */
    UNIT_TB         = 0x4,      /* 2^40 */
    UNIT_PB         = 0x5,      /* 2^50 */
    UNIT_EB         = 0x6,      /* 2^60 */
    UNIT_NA         = 0xF1,     /* block size of copy destination */

    /* other values are reserved*/
    MAX_UNIT_VALUE        ,
}TRANSFER_COUNT_UNIT;

/*
 * @enum      COPY_OP_STATUS
 * @brief
 * @note      SPC4R36, page 423
 */
typedef enum{
    OP_COMPLETED_WO_ERR                                     = 0x01,
    OP_COMPLETED_W_ERR                                      = 0x02,

    /* without error but with partial ROD token usage */
    OP_COMPLETED_WO_ERR_WITH_ROD_TOKEN_USAGE                = 0x03,
    OP_COMPLETED_WO_ERR_BUT_WITH_RESIDUAL_DATA              = 0x04,
    OP_IN_PROGRESS_WITHIN_FG_OR_BG_UNKNOWN                  = 0x10,
    OP_IN_PROGRESS_WITHIN_FG                                = 0x11,
    OP_IN_PROGRESS_WITHIN_BG                                = 0x12,
    /* e.x. by the preemption of a persistent reservation ... */
    OP_TERMINATED                                           = 0x60,
} COPY_OP_STATUS;

/*
 * @enum      COPY_CMD_STATUS
 * @brief
 * @note      SPC4R36, page 427
 */
typedef enum{
    COPY_CMD_IN_PROGRESS          = 0x00000000,
    COPY_CMD_COMPLETED_WO_ERR     = 0x00000001,
    COPY_CMD_COMPLETED_W_ERR      = 0x00000002,
} COPY_CMD_STATUS;


/** 
 * @enum      TPC_DESC_TYPE_CODE
 * @brief     Declare for 3rd party descriptor type code
 * @note      SPC4R36, page 769
 */
typedef enum{
    TOKEN_FROM_SAME_CPMGR_IN_SAME_SCSI_TARGET   = 0 ,
    TOKEN_FROM_OTHER_CPMGR_IN_SAME_SCSI_TARGET  = 1 ,
    TOKEN_FROM_OTHER_SCSI_TARGET                = 2 ,
    MAX_ROD_TOKEN_SRC                               ,
}ROD_TOKEN_SRC;

/** 
 * @enum      TPC_DESC_TYPE_CODE
 * @brief     Declare for 3rd party descriptor type code
 * @note      SPC4R36, page 769
 */
typedef enum{
    TPC_DESC_BLOCK_DEV_ROD_LIMITS    =  0x0000, 
    TPC_DESC_SUPPORTED_CMDS          =  0x0001,
    TPC_DESC_PARAMETER_DATA          =  0x0004, 
    TPC_DESC_SUPPORTED_DESCS         =  0x0008, 
    TPC_DESC_SUPPORTED_CSCD_IDS      =  0x000c, 
    TPC_DESC_ROD_TOKEN_FEATURES      =  0x0106, 
    TPC_DESC_SUPPORTED_ROD           =  0x0108, 
    TPC_DESC_GENERAL_COPY_OP         =  0x8001, 
    TPC_DESC_STREAM_COPY_OP          =  0x9101, 
    TPC_DESC_HOLD_DATA               =  0xC001, 
    MAX_TPC_DESC_TYPE,
} TPC_DESC_TYPE_CODE;


/** 
 * @struct
 * @brief
 * @note SPC4R36, page 778,780,781
 */
typedef struct rod_dev_type_specific_desc{
    u8      u8DevType:5;                        // byte 0
    u8      u8DescFormat:3;
    u8      u8Reserved0;                        // byte 1
    u8      u8DescLen[2];                       // byte 2 ~ byte 3

    /* a) for ROD of block device
     *
     * byte 4 ~ byte 5 : reserved
     * byte 6 ~ byte 7 : optimal block rod len granularity
     *
     * b) for ROD of stream and processor device
     *
     * byte 4 ~ byte 8 : reserved
     *
     */
    u8      u8Byte4_7[4];

    /* hbyte 8 ~ byte 15 -  max bytes in block (stream / processor) rod */
    u8      u8Byte8_15[8];

    /* hbyte 16 ~ byte 23 -  optimal bytes in block (stream / processor) rod transfer */
    u8      u8Byte16_23[8];

    /* a) for ROD of block device
     *
     * byte 24 ~ byte 31 : optimal bytes to token per segment
     * byte 32 ~ byte 39 : optimal bytes from token per segment
     * byte 40 ~ byte 47 : reserved
     *
     * b) for ROD of stream and processor device
     *
     * byte 24 ~ byte 47 : reserved
     *
     */
    u8      u8Byte24_47[24];

}__attribute__ ((packed)) ROD_DEV_TYPE_SPECIFIC_DESC;

/** 
 * @struct
 * @brief
 * @note SPC4R36, page 783
 */
typedef struct rod_type_desc{
    u8      u8RodType[4];                       // byte 0 ~ byte 3
    u8      u8TokenOut:1;                       // byte 4
    u8      u8TokenIn:1;                        //
    u8      u8Reserved0:5;                      //
    u8      u8EcpyInt:1;                        //
    u8      u8Reserved1;                        // byte 5
    u16     u16PreferenceIndication;            // byte 6 ~ byte 7
    u8      u8Reserved2[56];                    // byte 8 ~ byte 63
}__attribute__ ((packed)) ROD_TYPE_DESC;


/** 
 * @struct
 * @brief
 */
struct blk_dev_range_desc{
    u8  lba[8];         // byte 0 ~ byte 7
    u8  nr_blks[4];     // byte 8 ~ byte 11
    u8  reserved[4];    // byte 12 ~ byte 15
} __attribute__ ((packed));

/** 
 * @struct
 * @brief
 */
struct populate_token_param{
	u8  token_data_len[2];          // byte 0 ~ byte 1 (N-1)
	u8  immed:1;                    // byte 2
	u8  rtv:1;                      // rod type valid bit
	u8  reserved0:6;
	u8  reserved1;                  // byte 3
	u8  inactivity_timeout[4];      // byte 4 ~ byte 7
	u8  rod_type[4];                // byte 8 ~ byte 11
	u8  reserved2[2];               // byte 12 ~ byte 13
	u8  blkdev_range_desc_len[2];   // byte 14 ~ byte 15 (N -15)

	/* From byte 16 to byte N is for Block device range descriptor list */
} __attribute__ ((packed));


/** 
 * @struct
 * @brief
 * @note SBC3R31, page 207
 */
struct write_by_token_param {
	u8  token_data_len[2];          // byte 0 ~ byte 1 (N-1)
	u8  immed:1;                    // byte 2
	u8  reserved0:7;
	u8  reserved1[5];               // byte 3 ~ byte 7

	/* 
	 * This is blocks based on the logical block size
	 * of the logical unit to which the WRITE USING TOKEN command
	 * is to write data.
	 */
	u8  off_into_rod[8];                // byte 8 ~ byte 15
	u8  rod_token[ROD_TOKEN_MIN_SIZE];  // byte 16 ~ byte 527
	u8  reserved2[6];                   // byte 528 ~ byte 533
	u8  blkdev_range_desc_len[2];       // byte 534 ~ byte 535 (N - 535)

	/* From byte 536 to byte N is for Block device range descriptor list */

} __attribute__ ((packed));

/** 
 * @struct
 * @brief
 * @note SPC4R36, page 430
 */
struct rod_token_info_param{
    u8      avaiable_data_len[4];               // byte 0 ~ byte 3 (n - 3)
    u8      res_to_sac:5;                       // byte 4 (response to service action)
    u8      reserved0:3;
    u8      cp_op_status:7;                     // byte 5
    u8      reserved1:1;
    u8      op_counter[2];                      // byte 6 ~ byte 7
    u8      estimated_status_update_delay[4];   // byte 8 ~ byte 11
    u8      extcp_completion_status;            // byte 12
    u8      sense_data_len_field;               // byte 13 (x - 31)
    u8      sense_data_len;                     // byte 14
    u8      transfer_count_units;               // byte 15
    u8      transfer_count[8];                  // byte 16 ~ byte 23
    u8      seg_processed[2];                   // byte 24 ~ byte 25
    u8      reserved2[6];                       // byte 26 ~ byte 31

    /* from byte 32 ~ byte (x) is for sense data */
    /* from byte (x+1) ~ byte (x+4) is for rod token desc length (n-(x+4)) */
    /* from byte (x+5) ~ byte n is for rod token desc lists */
}__attribute__ ((packed));


/* SPC4R36, page 300 */
#define CSCD_ID_IS_NULL_LU_ON_BLOCK                 0xc000
#define CSCD_ID_IS_NULL_LU_ON_STREAM                0xc001
#define CSCD_ID_LU_IS_SPECIFIED_BY_ROD              0xf800
#define CSCD_ID_LU_IS_PROCESSING_XCOPY              0xffff

/** 
 * @enum      CSCD_DESC_TYPE_CODE
 * @brief     Define the descriptor type code for CSCD
 */
typedef enum{
    FC_N_PORT_NAME_DESC     = 0xe0,
    FC_N_PORT_ID_DESC       = 0xe1,
    FC_N_PORT_ID_WITH_N_PORT_NAME_CHECK_CSCD = 0xe2,
    PARALLEL_INTERFACE_DESC = 0xe3,
    ID_DESC                 = 0xe4, 
    IPv4_DESC               = 0xe5,
    ALIAS_DESC              = 0xe6, 
    RDMA_DESC               = 0xe7, 
    IEEE_1394_DESC          = 0xe8, 
    SAS_DESC                = 0xe9, 
    IPv6_DESC               = 0xea, 
    IP_COPY_SERVICE_DESC    = 0xeb, 

    /* From 0xEC to 0xFD, there are reserved */
    ROD_DESC                = 0xfe, 
    EXT_DESC                = 0xff,   /* this was used for cscd desc extension format */
    MAX_CSCD_DESC_TYPE_CODE,
}CSCD_DESC_TYPE_CODE;

/** 
 * @enum      LU_ID_TYPE
 * @brief     Define List Usage ID type
 */
typedef enum{
    LU_NUM          = 0,
    PROXY_TOKEN     = 1,
    MAX_LU_ID_TYPE  = 4,
}LU_ID_TYPE;


/** 
 * @struct SEQ_ACCESS_DEV_TYPE
 * @brief
 */
typedef struct seq_access_dev_type{
    u8 Fixed:1;
    u8 Reserved0:1;
    u8 Pad:1;
    u8 Reserved1:5;
    u8 StreamBlkLen[3];
}SEQ_ACCESS_DEV_TYPE;

/** 
 * @struct BLOCK_DEV_TYPE
 * @brief
 */
typedef struct block_dev_type{
    u8 Reserved0:2;
    u8 Pad:1;
    u8 Reserved1:5;
    u8 DiskBlkLen[3];
}BLOCK_DEV_TYPE;

/** 
 * @struct PROCESSOR_DEV_TYPE
 * @brief
 */
typedef struct processor_dev_type{
    u8 Reserved0:2;
    u8 Pad:1;
    u8 Reserved1:5;
    u8 Reserved2[3];
}PROCESSOR_DEV_TYPE;


typedef union{
    PROCESSOR_DEV_TYPE    ProcessorDevTypes;
    SEQ_ACCESS_DEV_TYPE   SeqDevTypes;
    BLOCK_DEV_TYPE        BlockDevTypes;
}DEV_TYPE_PARAM;


/** 
 * @struct GEN_CSCD_HDR
 * @brief  General CSCD header
 */
typedef struct gen_cscd_hdr{
    u8   DescTypeCode;                 /* byte 0 */
    u8   DevType:5;                    /* byte 1 */
    u8   Obsolete:1;
    u8   LuIdType:2;                   /* LU ID type shall be ignored for ID CSCD Desc */
}GEN_CSCD_HDR;

/** 
 * @struct IP_COPY_SERVICE_EXT_CSCD_DESC
 * @brief  Extened CSCD descriptor for IP COPY SERVICE
 */
typedef struct ip_copy_service_ext_cscd_desc{
    u8   ExtDescTypeCode;              /* byte 32 */
    u8   Reserved0[3];                 /* byte 33 ~ byte 35 */
    u8   PortNum[2];                   /* byte 36 ~ byte 37 */
    u8   InternetProtocolNum[2];       /* byte 38 ~ byte 39 */
    u8   CodeSet:4;                    /* byte 40 */
    u8   Reserved1:4;
    u8   DesignatorType:4;             /* byte 41 */
    u8   Assoc:2;
    u8   Reserved2:2;
    u8   Reserved3;                    /* byte 42 */
    u8   DesignatorLen;                /* byte 43 */
    u8   Designator[20];               /* byte 44 ~ byte 63 */
}IP_COPY_SERVICE_EXT_CSCD_DESC;

/** 
 * @struct IPV6_EXT_CSCD_DESC
 * @brief  Extened CSCD descriptor for IPV6 COPY SERVICE
 */
typedef struct ipv6_ext_cscd_desc{
    u8   ExtDescTypeCode;              /* byte 32 */
    u8   Reserved0[3];                 /* byte 33 ~ byte 35 */
    u8   PortNum[2];                   /* byte 36 ~ byte 37 */
    u8   InternetProtocolNum[2];       /* byte 38 ~ byte 39 */
    u8   Designator[24];               /* byte 40 ~ byte 63 */
}IPV6_EXT_CSCD_DESC;


/** 
 * @struct ID_CSCD_DESC
 * @brief  Structure for ID CSCD descriptor
 */
struct id_cscd_desc {
	GEN_CSCD_HDR	GenCSCDHdr;			// byet 0 ~ byte 1
	u8		InitiatorPortIdentifier[2];	// byte 2 ~ byte 3
	u8		CodeSet:4;			// byte 4
	u8		Reserved0:4;
	u8		DesignatorType:4;	// byte 5
	u8		Assoc:2;
	u8		Reserved1:2;
	u8		Reserved2;		// byte 6
	u8		DesignatorLen;		// byte 7
	u8		Designator[20];		// byte 8 ~ byte 27
	DEV_TYPE_PARAM	DevTypeSpecificParam;	// byte 28 ~ byte 31
} __attribute__ ((packed));


/** 
 * @struct ALIAS_CSCD_DESC
 * @brief  Structure for ALIAS CSCD descriptor
 */
struct alias_cscd_desc {
    GEN_CSCD_HDR    GenCSCDHdr;                   // byet 0 ~ byte 1
    u8              InitiatorPortIdentifier[2];   // byte 2 ~ byte 3
    u8              LU_Identifier[8];             // byte 4 ~ byte 11
    u8              AliasValue[8];                // byte 12 ~ byte 19
    u8              Reserved0[8];                 // byte 20 ~ byte 27
    DEV_TYPE_PARAM  DevTypeSpecificParam;         // byte 28 ~ byte 31
};

/** 
 * @struct IP_COPY_SERVICE_CSCD_DESC
 * @brief  Structure for IP COPY SERVICE CSCD descriptor
 */
struct ip_copy_service_cscd_desc {
    GEN_CSCD_HDR                  GenCSCDHdr;             // byet 0 ~ byte 1
    u8                            IpType:1;               // byte 2
    u8                            Reserved0:7;
    u8                            Reserved1[8];           // byte 3 ~ byte 11
    u8                            CopyServiceIpAddr[16];  // byte 12 ~ byte 27
    DEV_TYPE_PARAM                DevTypeSpecificParam;   // byte 28 ~ byte 31
    IP_COPY_SERVICE_EXT_CSCD_DESC ExtCscdDesc;            // byte 32 ~ byte 63
};

/** 
 * @struct ROD_CSCD_DESC
 * @brief  Structure for ROD CSCD descriptor
 */
struct rod_cscd_desc {
    GEN_CSCD_HDR    GenCSCDHdr;                       // byet 0 ~ byte 1
    u8              InitiatorPortIdentifier[2];       // byte 2 ~ byte 3
    u8              Rod_Producer_Id[2];               // byte 4 ~ byte 5
    u8              Reserved0[2];                     // byte 6 ~ byte 7
    u8              RodType[4];                       // byte 8 ~ byte 11
    u8              ReqRodTokenLife[4];               // byte 12 ~ byte 15
    u8              ReqRodTokenInactivityTimeOut[4];  // byte 16 ~ byte 19
    u8              Reserved1[2];                     // byte 20 ~ byte 21
    u8              R_Token:1;                        // byte 22 , remote token bit
    u8              Reserved2:7;
    u8              Del_Tkn:1;                        // byte 23
    u8              Reserved3:7;
    u8              RodTokenOffset[4];                // byte 24 ~ byte 27
    DEV_TYPE_PARAM  DevTypeSpecificParam;             // byte 28 ~ byte 31
};

typedef union{
	struct id_cscd_desc			IdCscdDesc;
	struct alias_cscd_desc			AliasCscdDesc;
	struct ip_copy_service_cscd_desc	IpCopyServiceCscdDesc;
	struct rod_cscd_desc			RodCscdDesc;
} GEN_CSCD_DESC;

/** 
 * @enum      SEG_DESC_TYPE_CODE
 * @brief     Define descriptor type code for SEG
 */
typedef enum{
    COPY_BLK_TO_STREAM                = 0x0,
    COPY_STREAM_TO_BLK                = 0x1,
    COPY_BLK_TO_BLK                   = 0x2,
    COPY_STREAM_TO_STREAM             = 0x3,
    COPY_INLINE_DATA_TO_STREAM        = 0x4,
    COPY_EMB_DATA_TO_STREAM           = 0x5,
    READ_STREAM_AND_DISCARD           = 0x6,
    VERIFY_CSCD                       = 0x7,
    COPY_BLK_OFF_TO_STREAM            = 0x8,
    COPY_STREAM_TO_BLK_OFF            = 0x9,
    COPY_BLK_OFF_TO_BLK_OFF           = 0xA,
    COPY_BLK_TO_STREAM_AND_HOLD       = 0xB,
    COPY_STREAM_TO_BLK_AND_HOLD       = 0xC,
    COPY_BLK_TO_BLK_AND_HOLD          = 0xD,
    COPY_STREAM_TO_STREAM_AND_HOLD    = 0xE,
    READ_STREAM_AND_HOLD              = 0xF,
    WRITE_FILEMARK_TO_SEQ_ACCESS_DEV  = 0x10,
    SPACE_OR_FILEMARKS_SEQ_ACCESS_DEV = 0x11,
    LOCATE_SEQ_ACCESS_DEV             = 0x12,
    TAPE_IMAGE_COPY                   = 0x13,
    REG_PR_KEY                        = 0x14,
    THIRD_PARTY_PR_SRC                = 0x15,
    BLK_IMAGE_COPY                    = 0x16,

    /* 17h to BDh are reserved */
    POPULATE_ROD_FROM_ONE_OR_MORE_BLK_RANGES  = 0xBE,
    POPULATE_ROD_FROM_ONE_BLK_RANGES          = 0xBF,

    /* FIXED ME !! Should I assign value to this ??? */
    MAX_SEG_DESC_TYPE_CODE ,
}SEG_DESC_TYPE_CODE;


/** 
 * @struct GEN_SEG_DESC_HDR
 * @brief  Structure for general SEG descriptor header
 */
typedef struct gen_seg_desc_hdr{
    u8   DescTypeCode;       // byte 0
    u8   CAT:1;              // byte 1
    u8   DC:1;               // dest count, for blk to blk
    u8   Reserved1:6;
    u8   DescLen[2];         // byte 2 ~ byte 3
    u8   SrcCscdDescId[2];   // byte 4 ~ byte 5
    u8   DestCscdDescId[2];  // byte 6 ~ byte 7
}GEN_SEG_DESC_HDR;

/** 
 * @struct BLK_STREAM_SEG_DESC
 * @brief  Structure for block/stream SEG descriptor
 * @note blk_stream_seg_desc structure can be used for ...
 * @note (1) block dev to stream dev seg desc (type code: 00h)
 * @note (2) block dev to stream dev seg desc and hold a processed copy data for client (type code: 0Bh)
 * @note (3) stream dev to blk dev seg desc (type code: 01h)
 * @note (4) stream dev to blk dev seg desc  and hold a processed copy data for client (type code: 0Ch)
 */
typedef struct blk_stream_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;         // byte 0 ~ byte 7
    u8                Reserved0;          // byte 8

    // amount of data to be written/read by each write/read cmd
    // into/from stream dest/src device
    u8                StreamDevXfsLen[3]; // byte 9 ~ byte 11
    u8                Reserved1;          // byte 12
    u8                Reserved2;          // byte 13

    // logical block lengths of data to be processed in this seg
    u8                BlkDevNumBlks[2];   // byte 14 ~ byte 15

    // start src/dest lba to be processed
    u8                BlkDevLba[8];       // byte 16 ~ byte 23
}BLK_STREAM_SEG_DESC;

/** 
 * @struct BLK_TO_BLK_SEG_DESC
 * @brief  Structure for block SEG descriptor
 * @note blk_stream_seg_desc structure can be used for ...
 * @note (1) block dev to block dev seg desc (type code: 02h)
 * @note (2) block dev to block dev seg desc and hold a processed copy data for client (type code: 0Dh)
 */
typedef struct blk_to_blk_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;         // byte 0 ~ byte 7
    u8                Reserved0;          // byte 8
    u8                Reserved1;          // byte 9

    // DC = 1, num of blocks to be written into copy dest
    // DC = 0, num of blocks to be processed in this seg
    u8                BlkDevNumBlks[2];   // byte 10 ~ byte 11
    u8                SrcBlkDevLba[8];    // byte 12 ~ byte 19
    u8                DestBlkDevLba[8];   // byte 20 ~ byte 27
}BLK_TO_BLK_SEG_DESC;


/** 
 * @struct STREAM_TO_STREAM_SEG_DESC
 * @brief  Structure for stream SEG descriptor
 */
typedef struct stream_to_stream_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                Reserved0;            // byte 8

    // amount of data to be read by each read cmd from stream src device
    u8                SrcStreamXfsLen[3];   // byte 9 ~ byte 11
    u8                Reserved1;            // byte 12

    // amount of data to be written by each write cmd into stream dest device
    u8                DestStreamXfsLen[3];  // byte 13 ~ byte 15

    // number of bytes shall be processed in this seg
    u8                ByteCounts[4];        // byte 16 ~ byte 19
}STREAM_TO_STREAM_SEG_DESC;

/** 
 * @struct INLINE_DATA_TO_STREAM_SEG_DESC
 * @brief  Structure for inline data to stream SEG descriptor
 */
typedef struct inline_data_to_stream_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                Reserved0;            // byte 8

    // amount of data to be written by each write cmd into stream dest device
    u8                StreamXfsLen[3];      // byte 9 ~ byte 11
    u8                InlineDataOff;        // byte 12 ~ byte 15
    u8                InlineDataInBytes[4]; // byte 16 ~ byte 19
}INLINE_DATA_TO_STREAM_SEG_DESC;

/** 
 * @struct EMBEDDED_DATA_TO_STREAM_SEG_DESC
 * @brief  Structure for embedded data to stream SEG descriptor
 */
typedef struct embedded_data_to_stream_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                Reserved0;            // byte 8
    u8                StreamDevXfsLen[3];   // byte 9 ~ byte 11
    u8                EmbDataInBytes[2];    // byte 12 ~ byte 13
    u8                Reserved1[2];         // byte 14 ~ byte 15
    //
    // from byte 16 to byte N, it is embedded data
    //
}EMBEDDED_DATA_TO_STREAM_SEG_DESC;

/** 
 * @struct STREAM_DISCARD_SEG_DESC
 * @brief
 */
typedef struct stream_discard_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                Reserved0;            // byte 8
    u8                StreamDevXfsLen[3];   // byte 9 ~ byte 11

    // number of bytes to be removed from src data
    u8                DataInBytes[4];       // byte 12 ~ byte 15
}STREAM_DISCARD_SEG_DESC;

/** 
 * @struct VERIFY_CSCD_SEG_DESC
 * @brief
 */
typedef struct verify_cscd_seg_desc{
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                u8TUR:1;              // byte 8
    u8                Reserved0:7;
    u8                Reserved1[3];         // byte 9 ~ byte 11
}VERIFY_CSCD_SEG_DESC;

/** 
 * @struct BLK_STREAM_OFF_SEG_DESC
 * @brief  Structure for block(offset)/stream(offset) SEG descriptor
 * @note blk_stream_offset_seg_desc structure can be used for  ...
 * @note (1) block dev with offset to stream dev seg desc
 * @note (2) or stream dev with offset to blk dev seg desc
 */
typedef struct blk_stream_off_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;           // byte 0 ~ byte 7
    u8                Reserved0;            // byte 8

    // amount of data to be written/read by each write/read cmd
    // into/from stream dest/src device
    u8                StreamDevXfsLen[3];   // byte 9 ~ byte 11

    // number of bytes to be read/write from/into src/dest blk dev
    u8                DataInBytes[4];       // byte 12 ~ byte 15

    // lba address for src/dest blk dev
    u8                BlkDevLba[8];         // byte 16 ~ byte 23
    u8                Reserved1;            // byte 24
    u8                Reserved2;            // byte 25

    //
    // offset into the first copy src/dest block 
    // to being read/written from/into copy src/dest blk dev
    //
    u8                BlkDevByteOffset[2];  // byte 26 ~ byte 27

}BLK_STREAM_OFF_SEG_DESC;

/** 
 * @struct BLK_OFF_TO_BLK_OFF_SEG_DESC
 * @brief  Structure for block(offset)/block(offset) SEG descriptor
 */
typedef struct blk_off_to_blk_off_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7

    // bytes data to be read
    u8                DataInBytes[4];         // byte 8 ~ byte 11
    u8                SrcBlkDevLba[8];        // byte 12 ~ byte 19
    u8                DestBlkDevLba[8];       // byte 20 ~ byte 27

    // offset into first copy src block at which to be read
    u8                SrcBlkDevByteOffset[2]; // byte 28 ~ byte 29

    // offset into first copy dest block at which to be read
    u8                DestBlkDevByteOffset[2];// byte 30 ~ byte 31

}BLK_OFF_TO_BLK_OFF_SEG_DESC;


/** 
 * @struct WRITE_FILEMARKS_SEG_DESC
 * @brief
 */
typedef struct write_filemarks_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                W_IMMED:1;              // byte 8
    u8                Obsolete:1;
    u8                Reserved0:6;
    u8                FileMarksCount[3];      // byte 9 ~ byte 11
}WRITE_FILEMARKS_SEG_DESC;

/** 
 * @struct SPACE_SEG_DESC
 * @brief
 */
typedef struct space_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                CODE:3;                 // byte 8
    u8                Reserved0:5;
    u8                Count[3];               // byte 9 ~ byte 11
}SPACE_SEG_DESC;

/** 
 * @struct LOCATE_SEG_DESC
 * @brief
 */
typedef struct locate_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                LogicalObjId[4];        // byte 8 ~ byte 11
}LOCATE_SEG_DESC;

/** 
 * @struct TAPE_IMAGE_COPY_SEG_DESC
 * @brief
 */
typedef struct tape_image_copy_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                Count[4];               // byte 8 ~ byte 11
}TAPE_IMAGE_COPY_SEG_DESC;

/** 
 * @struct REG_PR_KEY_SEG_DESC
 * @brief
 */
typedef struct reg_pr_key_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                ReservationKey[8];      // byte 8 ~ byte 15
    u8                SA_ReservationKey[8];   // byte 16 ~ byte 23
    u8                Reserved0[4];           // byte 24 ~ byte 27
}REG_PR_KEY_SEG_DESC;

/** 
 * @struct THIRD_PARTY_PR_SRC_SEG_DESC
 * @brief
 */
typedef struct third_party_pr_src_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                ReservationKey[8];      // byte 8 ~ byte 15
    u8                SA_ReservationKey[8];   // byte 16 ~ byte 23
    u8                Reserved0;              // byte 24
    u8                APTPL:1;                // byte 25
    u8                UNREG:1;
    u8                Reserved1:6;
    u8                TargetPortId[2];        // byte 26 ~ byte 27
    u8                TransportIdLen[4];      // byte 28 ~ byte 31
    //
    // from byte 32 to byte N is for TransportID data
    //
}THIRD_PARTY_PR_SRC_SEG_DESC;

/** 
 * @struct BLK_IMAGE_COPY_SEG_DESC
 * @brief
 */
typedef struct blk_image_copy_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;       // byte 0 ~ byte 7
    u8                SrcLba[8];        // byte 8 ~ byte 15
    u8                DestLba[8];       // byte 16 ~ byte 23
    u8                NumBlocks[4];     // byte 24 ~ byte 27
}BLK_IMAGE_COPY_SEG_DESC;


/** 
 * @enum      RANGE_DESC_TYPE
 * @brief
 */
typedef enum{
    FOUR_GIBI_BLOCK_RANGE_DESC  = 1,
    //
    // All others are reserved
    //
    MAX_RANGE_DESC_TYPE,
}RANGE_DESC_TYPE;

/** 
 * @struct FOUR_GIBI_BLK_RANGE_DESC
 * @brief
 */
typedef struct four_gibi_blk_range_desc{
    u8     Lba[8];         // byte 0 ~ byte 7
    u8     NumBlocks[4];   // byte 8 ~ byte 11
    u8     Reserved0[4];   // byte 12 ~ byte 15
}FOUR_GIBI_BLK_RANGE_DESC;

/** 
 * @struct POPULATE_ROD_FROM_ONE_OR_MORE_BLK_SEG_DESC
 * @brief
 */
typedef struct populate_rod_from_one_or_more_blk_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;             // byte 0 ~ byte 7
    u8                Reserved0[5];           // byte 8 ~ byte 12
    RANGE_DESC_TYPE   RangeDescType;          // byte 13
    u8                RangeDescLen[2];        // byte 14 ~ byte 15
    //
    // from byte 16 to byte N is for range desc list data
    // and it is orderring from first to last
    //
}POPULATE_ROD_FROM_ONE_OR_MORE_BLK_SEG_DESC;

/** 
 * @struct POPULATE_ROD_FROM_ONE_BLK_SEG_DESC
 * @brief
 */
typedef struct populate_rod_from_one_blk_seg_desc {
    GEN_SEG_DESC_HDR  SegDescHdr;   // byte 0 ~ byte 7
    u8                Lba[8];       // byte 8 ~ byte 15
    u8                NumBlocks[4]; // byte 16 ~ byte 19
}POPULATE_ROD_FROM_ONE_BLK_SEG_DESC;

typedef union{
    BLK_STREAM_SEG_DESC                         BlkStreamSegDesc;
    BLK_TO_BLK_SEG_DESC                         Blk2BlkSegDesc;
    STREAM_TO_STREAM_SEG_DESC                   Stream2StreamSegDesc;
    INLINE_DATA_TO_STREAM_SEG_DESC              InlineData2StreamSegDesc;
    EMBEDDED_DATA_TO_STREAM_SEG_DESC            EmbeddedData2StreamSegDesc;
    STREAM_DISCARD_SEG_DESC                     StreamAndDiscardSegDesc;
    VERIFY_CSCD_SEG_DESC                        VerifyCscdSegDesc;
    BLK_STREAM_OFF_SEG_DESC                     BlkStreamOffSegDesc;
    BLK_OFF_TO_BLK_OFF_SEG_DESC                 BlkOff2BlkOffSegDesc;
    WRITE_FILEMARKS_SEG_DESC                    WriteFilemarksSegDesc;
    SPACE_SEG_DESC                              SpaceSegDesc;
    LOCATE_SEG_DESC                             LocateSegDesc;
    TAPE_IMAGE_COPY_SEG_DESC                    TapeImgCopySegDesc;
    REG_PR_KEY_SEG_DESC                         RegPrKeySegDesc;
    THIRD_PARTY_PR_SRC_SEG_DESC                 ThirdPartyPrSrcSegDesc;
    BLK_IMAGE_COPY_SEG_DESC                     BlkImgCopySegDesc;
    POPULATE_ROD_FROM_ONE_OR_MORE_BLK_SEG_DESC  PopulateRodOneMoreBlkSegDesc;
    POPULATE_ROD_FROM_ONE_BLK_SEG_DESC          PopulateRodOneBlkSegDesc;
}GEN_SEG_DESC;

/** 
 * @struct RECEIVE_COPY_OP_PARAM
 * @brief  Structure of parameter for receive copy command
 */
typedef struct receive_copy_op_param{
    u8    DataLen[4];                 // byte 0 ~ byte 3 (N-3)
    u8    SNLID:1;                    // byte 4
    u8    Reserved0:7;
    u8    Reserved1[3];               // byte 5 ~ byte 7
    u8    MaxCscdDescCount[2];        // byte 8 ~ byte 9
    u8    MaxSegDescCount[2];         // byte 10 ~ byte 11
    u8    MaxDescListLen[4];          // byte 12 ~ byte 15
    u8    MaxSegLen[4];               // byte 16 ~ byte 19
    u8    MaxInlineDataLen[4];        // byte 20 ~ byte 23
    u8    HeldDataLimit[4];           // byte 24 ~ byte 27
    u8    MaxStreamDevXfsSize[4];     // byte 28 ~ byte 31
    u8    Reserved2[2];               // byte 32 ~ byte 33
    u8    TotalConCurrentCpSize[2];   // byte 34 ~ byte 35  
    u8    MaxConCurrentCp;            // byte 36
    u8    DataSegGranularityLog2;     // byte 37
    u8    InlineDataGranularityLog2;  // byte 38
    u8    HeldDataGranularityLog2;    // byte 39
    u8    Reserved3[3];               // byte 40 ~ byte 42
    u8    ImplementedDescListLen;     // byte 43 (N - 43)
    //
    // from byte 44 to byte N is for list of implemented desc type code (ordered)
    //
}__attribute__ ((packed)) RECEIVE_COPY_OP_PARAM;

/** 
 * @struct LID1_XCOPY_PARAMS
 * @brief  Structure of parameter for extened copy LID1 command 
 */
typedef struct lid1_xcopy_params{
    u8    ListId;                     // byte 0
    u8    Priority:3;                 // byte 1 
    u8    ListIdUsage:2;
    u8    Str:1;
    u8    Reserved0:2;
    u8    CscdDescListLen[2];         // byte 2 ~ byte 3
    u8    Reserved1[4];               // byte 4 ~ byte 7
    u8    SegDescListLen[4];          // byte 8 ~ byte 11
    u8    InlineDataLen[4];           // byte 12 ~ byte 15

    /*
    //
    // From byte 16 ~ byte K, it includes the information like ...
    //
    // < byte 16 >  ---
    //    |            \
    //    |              { A lists of CSCD descriptors }
    //    |            /
    // < byte N >   ---
    // < byte N+1 > ---
    //    |            \
    //    |              { A lists of Segment descriptors }
    //    |            /
    // < byte M >   ---
    // < byte M+1 > ---
    //    |            \
    //    |              { Inline data }
    //    |            /
    // < byte K >   ---
    //
    */
}__attribute__ ((packed)) LID1_XCOPY_PARAMS;

/** 
 * @struct LID4_XCOPY_PARAMS
 * @brief  Structure of parameter for extened copy LID4 command 
 */
typedef struct lid4_xcopy_params{
    u8    ParamListFormat;            // byte 0
    u8    Priority:3;                 // byte 1 
    u8    ListIdUsage:2;
    u8    Str:1;
    u8    Reserved0:2;
    u8    HdrCscdDescListLen[2];      // byte 2 ~ byte 3
    u8    Reserved1[11];              // byte 4 ~ byte 14
    u8    Immed:1;                    // byte 15
    u8    G_Sense:1;
    u8    Reserved2:6;
    u8    HdrCscdDescTypeCode;        // byte 16
    u8    Reserved3[3];               // byte 17 ~ byte 19
    u8    ListId[4];                  // byte 20 ~ byte 23
    u8    Reserved4[18];              // byte 24 ~ byte 41
    u8    CscdDescListLen[2];         // byte 42 ~ byte 43
    u8    SegDescListLen[2];          // byte 44 ~ byte 45
    u8    InlineDataLen[2];           // byte 46 ~ byte 47

    /*
     * From byte 48 ~ byte K, it includes the information like ...
     *
     * < byte 48 >  ---
     *    |            \
     *    |              { A lists of CSCD descriptors }
     *    |            /
     * < byte N >   ---
     * < byte N+1 > ---
     *    |            \
     *    |              { A lists of Segment descriptors }
     *    |            /
     * < byte M >   ---
     * < byte M+1 > ---
     *    |            \
     *    |              { Inline data }
     *    |            /
     * < byte K >   ---
     *
     */
}__attribute__ ((packed)) LID4_XCOPY_PARAMS;

struct dev_type_data {
	/* byte 0 */
	u8  supported_unmap:1; /* 1: yes, 0: no */
	u8  supported_unmap_read_zero:1; /* 1: yes, 0: no */
	u8  supported_write_same:1; /* 1: yes, 0: no */
	u8  supported_write_cache:1; /* 1: yes, 0: no */
	u8  supported_fua_write:1; /* 1: yes, 0: no */
	u8  reserved0:3;
	/* byte 1 */
	u8  reserved1;
	/* byte 2 */
	u8  supported_fbc:1; /* dev support Fast Block Clone ?  1: yes, 0: no */
	u8  reserved2:7;
	/* byte 3 */
	u8  reserved3;
};

/** 
 * @struct
 * @brief ROD_TOKEN
 * @note SPC4R36, page 248, 250
 */
struct rod_token{
	u8  type[4];
	u8  reserved0[2];
	u8  token_len[2];

	/* byte 8 ~ byte 15 (copy manager rod token identifier) */
	u8  cm_rod_token_id[8];

	/* byte 16 ~ byte 47 (creator logical unit descriptor) */
	struct id_cscd_desc  desc;

	/* byte 48 ~ byte 63 (number of bytes represented) */
	u8  nr_represented_bytes[16];

	union {
		/* for rod type is NOT 00h */
		u8  reserved1[32];

		/* for rod type is 00h */
		u8  rod_token_specific_data[32];
	} __attribute__ ((packed)) byte64_95;

	/* byte 96 ~ byte 127, it is for specific data by command standard for
	 * peripheral device type
	 */
	struct dev_type_data  data;	

}__attribute__ ((packed));

/* 
 * (1) ROD_TOKEN_512B structure is for ROD types other than 00h 
 * (2) Currently, we support ROD_TYPE_PIT_COPY_D4 type now (SPC4R36, page 251)
 * (3) spc4r36e, page251 (table 115), TARGET DEVICE DESCRIPTOR FIELD size is 128 bytes
 */
#define ROD_TARGET_DEV_DESC_LEN		128
#define EXT_ROD_TOKEN_DATA_LEN	\
	(ROD_TOKEN_MIN_SIZE - ROD_TARGET_DEV_DESC_LEN - 128)

struct rod_token_512b{

	/* 128 bytes */
	struct rod_token	gen_data;

	/* FIXED ME !!
	 * (1) For rod type 0x0000_0000h, from byte 128 ~ byte N is rod token
	 * type and copy manager specific data 
	 *
	 * (2) For rod type other than 0x0000_0000hh, from byte 128 ~ byte T
	 * is target device descriptor and from byte (T+1) ~ byte N is extended
	 * ROD token data 
	 */
	u8		target_dev_desc[ROD_TARGET_DEV_DESC_LEN];
	u8		ext_rod_token[EXT_ROD_TOKEN_DATA_LEN];

}__attribute__ ((packed));

struct ext_rod_token_data{
	u64	cmd_id_lo;
	u64	cmd_id_hi;
	u64	tpg_id_lo;
	u64	tpg_id_hi;
	u64	initiator_id_lo;
	u64	initiator_id_hi;
} __attribute__ ((packed));


/* one cp src and one cp target per one copy command */
#define	MAX_CSCD_DESC_COUNT	2
/* one action per one copy command */
#define	MAX_SEG_DESC_COUNT	1

static inline u16 __qnap_tpc_get_max_supported_cscd_desc_count(void)
{
	return MAX_CSCD_DESC_COUNT;
}

static inline u16 __qnap_tpc_get_max_supported_seg_desc_count(void)
{
	return MAX_SEG_DESC_COUNT;
}

static inline u32 __qnap_tpc_get_total_supported_desc_len(void)
{
    return (
    	__qnap_tpc_get_max_supported_cscd_desc_count() * sizeof(GEN_CSCD_DESC) + \
    	__qnap_tpc_get_max_supported_seg_desc_count() * sizeof(GEN_SEG_DESC)
    	);
}

/* spc4r36, p72 
 * not include additional sense bytes 
 */
#define MIN_FIXED_FORMAT_SENSE_LEN	18	

static inline u32 __qnap_odx_get_min_rrti_param_len(void)
{
	/* SPC4R36 , page 430 
	 * the min len will be 0x238
	 */
	return (sizeof(struct rod_token_info_param) + \
		MIN_FIXED_FORMAT_SENSE_LEN + 4 + (ROD_TOKEN_MIN_SIZE + 2));
}

static inline u16 __qnap_odx_get_desc_counts(
	u16 len
	)
{
	/* The block dev range desc length shall be a multiple of 16 */
	return ((len & (~0x000f)) / sizeof(struct blk_dev_range_desc));
}

static inline u16 __qnap_odx_get_max_supported_blk_dev_range(void)
{
	/* FIXED ME !!
	 *
	 * Here, The value of max supported blk device range can not be larger
	 * than 0x400. The reason is the some testing items in Offload SCSI
	 * Compliance Test (LOGO) in WINDOWS HCK (ver: 8.59.29757) will be fail.
	 * Actually, we can not find any information (or document) about the
	 * limitation of this.
	 * Therefore, we limit it to 512 / 64MB = 8
	 */
	 return (MAX_TRANSFER_SIZE_IN_BYTES / OPTIMAL_TRANSFER_SIZE_IN_BYTES);
}

static inline sector_t __qnap_odx_get_total_nr_blks_by_desc(
	struct blk_dev_range_desc *s,
	u16 desc_counts
	)
{
	u16 index = 0;
	sector_t nr_blks = 0;

	for(index = 0; index < desc_counts; index++)
		nr_blks += (sector_t)get_unaligned_be32(&s[index].nr_blks[0]);

	return nr_blks;
}



#endif

