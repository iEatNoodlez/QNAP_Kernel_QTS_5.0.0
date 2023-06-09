/*
    Remember sync the same file in Kernel/include/qnap and NasLib/hal
*/
#ifndef __HAL_NETLINK_HDR
#define __HAL_NETLINK_HDR

typedef enum
{
    HAL_EVENT_GENERAL_DISK = 0,
    HAL_EVENT_ENCLOSURE,
    HAL_EVENT_RAID,
    HAL_EVENT_THIN,
    HAL_EVENT_NET,
    HAL_EVENT_USB_PRINTER,
    HAL_EVENT_VOLUME,
// #ifdef STORAGE_V2
    HAL_EVENT_LVM,
    HAL_EVENT_ISCSI,
    HAL_EVENT_HA,
// #endif
    HAL_EVENT_USB_MTP,
    HAL_EVENT_GPIO,
    HAL_EVENT_MEM,
    HAL_EVENT_BBU,
    HAL_EVENT_ZFS,
    HAL_EVENT_CALLHOME,
    HAL_EVENT_MAX_EVT_NUM,
    HAL_EVENT_CONTROL,
} EVT_FUNC_TYPE;

#ifndef USB_DRV_DEFINED
typedef enum
{
    USB_DRV_UNKNOWN_HCD = 0,
    USB_DRV_UHCI_HCD,
    USB_DRV_EHCI_HCD,
    USB_DRV_XHCI_HCD,
    USB_DRV_ETXHCI_HCD,
} USB_DRV_TYPE;
#define USB_DRV_DEFINED
#endif
typedef enum
{
// For GENERAL_DISK, ENCLOSURE,NET,PRT
    ATTACH = 1,
    DETACH,
    SCAN,
    SET_IDENT,
    CHANGE,
    PARTITION_ATTACH,
    PARTITION_DETACH,
// For ENCLOSURE
    SET_FAN_FORCE_SPEED = 10,
    SET_SCROLLBAR_STEPPING,
    CLEAR_FAN_FORCE_SPEED,
    SET_FAN_MODE_SMART_INTELLIGENT,
    SET_TFAN_MODE_SMART_INTELLIGENT,
    SET_FAN_MODE_SMART_CUSTOM,
    SET_FAN_MODE_FIXED,
    SET_TFAN_PARAMETERS,
    SET_MAX_WAKEUP_PD_NUM,  //for netlink
    SET_SMART_TEMP_UNIT,
    SET_PD_STANDBY_TIMEOUT,
    NORMAL_POWER_OFF,
    SET_PWR_REDUNDANT,
    SET_POWER_BUTTON,       //for hw test
    RELOAD_USB_DRV,
    SET_PIC_WOL,
    OOM_KILLED,
    IR_INPUT,
    SET_SYS_WARNING_TEMP,
    SET_SYS_SHUTDOWN_TEMP,
// For GENERAL_DISK
    SELF_TEST = 50,
    SET_PD_STANDBY,         //for netlink
    CLEAR_PD_STANDBY,       //for netlink
    SET_PD_ERROR,           //for netlink
    CLEAR_PD_ERROR,         //for netlink
    SET_PD_TEMP_WARNING,
    SELF_TEST_SCHEDULE,
    FAIL_DRIVE,
    REMOVABLE_DRIVE_DETACH,
    SET_NCQ_BY_USER,
    SET_NCQ_BY_KERNEL,
    GET_PD_SMART,
    SET_PD_ATA_QC_ERROR,    // for netlink
    SET_PD_ATA_LINK_ERROR,    // for netlink
    SET_PD_SCSI_CMD_ERROR,    // for netlink
    SET_PD_ATA_MARVELL_QC_ERROR,    // for netlink
    SET_PD_NVME_CMD_ERROR,

// For RAID    
    REPAIR_RAID_READ_ERROR = 100,   //1,5,6,10, for netlink,reconstruct
    SET_RAID_PD_ERROR,              //1,5,6,10, for netlink
    SET_RAID_RO,                    //1,5,6,10, for netlink,degrade and other drive error
    RESYNCING_START,                //for netlink
    RESYNCING_SKIP,                 //for netlink
    RESYNCING_COMPLETE,             //for netlink
    REBUILDING_START,               //for netlink
    REBUILDING_SKIP,                //for netlink
    REBUILDING_COMPLETE,            //for netlink
    RESHAPING_START,                //for netlink
    RESHAPING_SKIP,                 //for netlink
    RESHAPING_COMPLETE,             //for netlink
    //[SDMD START] 20130124 by csw: for raid hot-replace event
    HOTREPLACING_START,             //for netlink
    HOTREPLACING_SKIP,              //for netlink
    HOTREPLACING_COMPLETE,          //for netlink
    RAID_PD_HOTREPLACED,            //for netlink
    BAD_BLOCK_ERROR_DETECT,         //for md badblock
    BAD_BLOCK_ERROR_STRIPE,         //for md badblock
    BAD_BLOCK_ERROR_REBUILD,	    //for md badblock
    //[SDMD END]
// For NET
    BLOCK_IP_FILTER = 150,          //for NVR
    NET_EVT_MASK,
    NET_IP_DEL,
    NET_IP_NEW,
    NET_SCAN,
// For THIN
    THIN_SB_BACKUP_FAIL = 160,
    THIN_META_ERROR,
    THIN_ERR_VERSION_DETECT,	    //for version-checking of dm-thin
    THIN_RECLAIM_NEEDED,
    THIN_RECLAIM_CLEARED,
    THIN_SNAP_RESERVE_LOW,
    THIN_SNAP_RESERVE_OK,
// For VOLUME
    CHECK_FREE_SIZE = 180,
    CHECK_LAZY_INIT,
    VOL_REMOUNT_READ_ONLY,
    VOL_LAZY_INIT_START,
    VOL_LAZY_INIT_END,
// #ifdef STORAGE_V2
// For LVM
    CHECK_LVM_STATUS = 190,
    HIT_LUN_THRESHOLD,	// for iscsi    
    PRINT_SHOW_INFO,
    CLEAN_SHOW_INFO,
    UNDER_LUN_THRESHOLD,
// #endif
// For Misc    
    SHOW = 200,
    RETRIEVE_LOST_EVENT,                 //for netlink
    DEBUG_LEVEL,
    PM_EVENT,
    PM_PREPARE_SUSPEND_COMP,
    RELOAD_CONF,
    CHECK_GPIO_STATUS,
    TBT_VIRTUAL_NIC_ADD,
    TBT_VIRTUAL_NIC_SUSPEND,
    TBT_VIRTUAL_NIC_RESUME,
    TBT_VIRTUAL_NIC_READY,
    TBT_VIRTUAL_NIC_ERR_RECOVERY,
// For memory 
    FILE_BASED_SWAP,
// For bbu
    BBU_LEARNING_MODE,
// For ZFS
    ZFS_RESILVERING_START,
    ZFS_RESILVERING_COMPLETE,
    ZFS_CHECK_STATUS,
    ZFS_REPLACEMENT_START,
    ZFS_SCRUB_START,
    ZFS_SCRUB_COMPLETE,
    ZFS_SCRUB_CANCEL,
    ZFS_SET_PD_ERROR,
    ZFS_ZPOOL_OVER_THRESHOLD,
    ZFS_ZPOOL_UNDER_THRESHOLD,
    ZFS_ZFS_OVER_THRESHOLD,
    ZFS_ZFS_UNDER_THRESHOLD,
    ZFS_ZPOOL_ADD_COMPLETE,
    ZFS_ZPOOL_ADD_FAIL,
    ZFS_ZPOOL_SUSPEND,
    ZFS_SNAPSYNC_CREATE_SNAPSHOT,
    ZFS_SNAPSYNC_DELETE_SNAPSHOT,
    ZFS_SNAPSYNC_DST_IO_ERROR,
    ZFS_SNAPSYNC_PAIR_STATE_CHANGED,
    ZFS_SNAPSYNC_DST_PROP_ERROR,
    ZFS_SNAPSYNC_GROUP_STATE_CHANGED,
    ZFS_SNAPSYNC_CHANNEL_GONE,
    ZFS_SNAPSYNC_PROP_RESCAN,
// For EVENT_CONTROL
    DA_EVENT_ENABLE,
// For CEPH
    SET_OSD_PD_ERROR,
// For TR-XX TBT/USB cable status event
    TRCT_CABLE_CURRENT_STATUS,
    TRCT_DAEMON_SKIP,
// For TL-XX USB JBOD Endpoint
    TL_USB_ENDPOINT,
    CALLHOME_DISK_SMART = 500,
    CALLHOME_FAN,
	CALLHOME_DISK_NVME_HW_ANOMALIES,
    CALLHOME_THERMO,
    CALLHOME_PSU,
    CALLHOME_PCIeAER,
    CALLHOME_DAILY_CHECK,
} EVT_FUNC_ACTION;

typedef enum {
    SCSI_CMD_HAL_RETRY,
    SCSI_CMD_HAL_FAIL,
    SCSI_CMD_HAL_LONG_LATENCY,
    SCSI_CMD_HAL_TIMES_OUT,
} SCSI_CMD_HALTYPE;

typedef enum {
    NVME_CMD_HAL_FAIL,
    NVME_CMD_HAL_LONG_LATENCY,
    NVME_CMD_HAL_TIMES_OUT,
} NVME_CMD_HALTYPE;

typedef struct _AER_DEV_CORRECTABLE{
    uint32_t    RxErr;
    uint32_t    BadTLP;
    uint32_t    BadDLLP;
    uint32_t    RplNumRollover;
    uint32_t    ReplayTO;
    uint32_t    AdvNonFatalErr;
    uint32_t    InternalError;
    uint32_t    HeaderLogOverFlow;
    uint32_t    TOTAL_ERR_COR;
} AER_DEV_COR, *AER_DEV_COR_PTR;

typedef struct _AER_DEV_FATAL{
    uint32_t    Undefined;
    uint32_t    DataLinkProtocol;
    uint32_t    SupriseDown;
    uint32_t    PoisonedTLP;
    uint32_t    FlowCtrlProtocol;
    uint32_t    CmpltTO;
    uint32_t    CmplterAbrt;
    uint32_t    UnxCmplt;
    uint32_t    RxOverFlow;
    uint32_t    MalfTLP;
    uint32_t    ECRCErr;
    uint32_t    UnsupReq;
    uint32_t    ACSViol;
    uint32_t    InternalError;
    uint32_t    MCBlockedTLP;
    uint32_t    AtomicEgressBlk;
    uint32_t    TLPPrefixBlk;
    uint32_t    PoisonTLPBlocked;
    uint32_t    TOTAL_ERR_FATAL;
} AER_DEV_FATAL, *AER_DEV_FATAL_PTR;

typedef struct _AER_DEV_NONFATAL{
    uint32_t    Undefined;
    uint32_t    DataLinkProtocol;
    uint32_t    SupriseDown;
    uint32_t    PoisonedTLP;
    uint32_t    FlowCtrlProtocol;
    uint32_t    CmpltTO;
    uint32_t    CmplterAbrt;
    uint32_t    UnxCmplt;
    uint32_t    RxOverFlow;
    uint32_t    MalfTLP;
    uint32_t    ECRCErr;
    uint32_t    UnsupReq;
    uint32_t    ACSViol;
    uint32_t    InternalError;
    uint32_t    MCBlockedTLP;
    uint32_t    AtomicEgressBlk;
    uint32_t    TLPPrefixBlk;
    uint32_t    PoisonTLPBlocked;
    uint32_t    TOTAL_ERR_NONFATAL;
} AER_DEV_NONFATAL, *AER_DEV_NONFATAL_PTR;

typedef struct
{
    EVT_FUNC_ACTION action;
    union __EVT_FUNC_ARG
    {
#if !defined(__KERNEL__) && !defined(KERRD)   
        struct
        {
            int                 action;
        }__attribute__ ((__packed__)) pwr_redundant_check;
        struct
        {
            int                 action;
            int                 value;
        }__attribute__ ((__packed__)) vol_free_size;   
// #ifdef STORAGE_V2
        struct
        {
            int                 pool_id;
            int                 vol_id;
            int                 value;
        }__attribute__ ((__packed__)) check_lvm_status;
        struct
        {
            int                 pool_id;
            int                 vol_id;
            int                 value;
        }__attribute__ ((__packed__)) check_zfs_status;
// #endif
        struct
        {
            int action;
            int time;//min
            char enc_sys_id[MAX_SYS_ID_LEN];
        }__attribute__ ((__packed__)) pd_standby_timeout;
        struct
        {
            int unit;
        }__attribute__ ((__packed__)) smart_unit;
        struct
        {
            PD_DEV_ID           dev_id;
            int                 action;
            int                 value;
        }__attribute__ ((__packed__)) temp_warning;
        struct
        {
            unsigned int flags;
        }__attribute__ ((__packed__)) debug_level;
        struct
        {
            char dev_sys_id[MAX_SYS_ID_LEN];
            char enc_sys_id[MAX_SYS_ID_LEN];
            time_t time_stamp;
        } __attribute__ ((__packed__)) add_remove;
        struct
        {
            PD_DEV_ID           dev_id;
            PD_SELFTEST_MODE    mode;
            unsigned long long  value;
        } __attribute__ ((__packed__)) self_test;
        struct
        {
            FAN_SPEED speed_level;
            int custom_unit;
            int custom_stop_temp;
            int custom_low_temp;
            int custom_high_temp;        
            int stepping;
            FAN_REGION fan_region;
            TFAN_MODE tfan_mode;
        } __attribute__ ((__packed__)) fan_control;
        struct
        {
            int temp_critical;
            int temp_hyst_high;
            int temp_hyst_low;
            FAN_REGION fan_region;
        } __attribute__ ((__packed__)) tfan_parameters;
        struct
        {
            PD_DEV_ID           dev_id;
            int                 enable;
            int                 timeout;//min
        } __attribute__ ((__packed__)) ident_led;
        struct
        {
            PD_DEV_ID           dev_id;
        } __attribute__ ((__packed__)) get_pd_smart;
        struct
        {
            int enc_id;
            int fan_id;
            char block_name[8];
            int fan_state;
        } __attribute__ ((__packed__)) get_fan;
        struct
        {
            int enc_id;
            int psu_id;
            char block_name[8];
            int psu_state;
        } __attribute__ ((__packed__)) get_psu;
        struct
        {
            int enc_id;
            int thermo_id;
            char block_name[8];
            int current_temp;
            int warning_temp;
            int shutdown_temp;
        } __attribute__ ((__packed__)) get_thermo;
        struct
        {
            int enc_id;
            char aer_dev_id[13];
            AER_DEV_COR aer_dev_correctable;
            AER_DEV_FATAL aer_dev_fatal;
            AER_DEV_NONFATAL aer_dev_nonfatal;
        } __attribute__ ((__packed__)) get_pciaer;
#endif 
        struct __get_pd_nvme_hw_anomalies
        {
            char pd_name[32];
        } __attribute__ ((__packed__)) get_pd_nvme_hw_anomalies;       
        struct __netlink_enc_cb
        {
            int enc_id;
            int max_wakeup_pds;
            unsigned long wakeup_interval;
        } __attribute__ ((__packed__)) netlink_enc;
        struct __netlink_pd_cb
        {
            int enc_id;             //only use for set_pd_standby
            int scsi_bus[4];
            unsigned char error_sense_key[3]; //sense_key,ASC,ASCQ
            unsigned char error_scsi_cmd[16];
        } __attribute__ ((__packed__)) netlink_pd;
        struct __netlink_pd_scsi_cmd_err
        {
            int action; // retry = 0, fail = 1
            int value; 
            int scsi_bus[4];
            unsigned char error_scsi_cmd[16];
        } __attribute__ ((__packed__)) netlink_pd_scsi_cmd_err;
        struct __netlink_pd_ata_qc_err
        {
            int scsi_bus[4];
            unsigned char state;
            unsigned char err;
        } __attribute__ ((__packed__)) netlink_pd_ata_qc_err;
        struct __netlink_pd_ata_marvell_qc_err
        {
            char serial_no[20];
            unsigned char state;
            unsigned char err;
        } __attribute__ ((__packed__)) netlink_pd_ata_marvell_qc_err;
        struct __netlink_pd_ata_link_err
        {
            unsigned int serror;
            int scsi_bus[4];
            int ata_print_id;
        } __attribute__ ((__packed__)) netlink_pd_ata_link_err;
	struct __netlink_pd_nvme_cmd_err
	{
		//int action;
		int err_group;
		int err_id;
		char sn[20];
	} __attribute__ ((__packed__)) netlink_pd_nvme_cmd_err;
        struct __netlink_raid_cb
        {
            int raid_id;
            char pd_scsi_name[32];
            unsigned long long pd_repair_sector;
            //[SDMD] 20130221 by csw: add structure for log
            char pd_scsi_spare_name[32];
        } __attribute__ ((__packed__)) netlink_raid;
// #ifdef QTS_CEPH
        struct __netlink_ceph_cb
        {
            char pd_scsi_name[32];
        } __attribute__ ((__packed__)) netlink_ceph;
// #endif
// #ifdef QTS_ZFS
        struct __netlink_zfs_cb
        {
            char zpool_name[64];
            char dev_name[64];
            char zfs_name[128];
            int error_type;
            int vdev_id;
            int d_dev_id;
            int s_dev_id;
            unsigned long long threshold;
            unsigned long long avail;
        } __attribute__ ((__packed__)) netlink_zfs;
        struct __netlink_zfs_snapsync_cb
        {
            unsigned long long groupkey;
            int channel_id;
            int error;          /* 0 if not used */
            char srcvol[256];
            char destvol[256];
            char name[256];     /* snapshotname or propname */
            char oldstate[32];  /* 0 if not used */
            char newstate[32];  /* 0 if not used */
        } __attribute__ ((__packed__)) netlink_zfs_snapsync;
// #endif
        struct __net_link_change_cb
        {
            char ifi_name[16];
            int ifi_role; //0->standlone, 1->slave in bonding mode, 2->master in bonding mode
        } __attribute__ ((__packed__)) net_link_change;
        struct __net_ip_change_cb
        {
            int monitor;
            char dev_name[16]; //eth0,
            char ip_addr[64]; //string
        } __attribute__ ((__packed__)) net_ip_change;
        struct __netlink_ip_block_cb
        {
            unsigned int addr;
            unsigned short protocol;
            unsigned short port;
        } __attribute__ ((__packed__)) netlink_ip_block;
        struct __netlink_usb_drv
        {
            USB_DRV_TYPE type;
        } __attribute__ ((__packed__)) netlink_usb_drv;
        struct __netlink_pic_wol
        {
            int enable;
        } __attribute__ ((__packed__)) netlink_pic_wol;
        struct __netlink_pm_cb
        {
            int pm_event; //0->suspend_prepare, 1->post_suspend
        } __attribute__ ((__packed__)) netlink_pm;
        struct __iscsi_lun
        {
            /**
             * @brief struct __iscsi_lun is used by hal event mechanism
             * for some purposes between kenrel-space and user-space,
             * when to make kernel, this file will also be copied to following
             * path - Kernel/linux-<??>/include/qnap/hal_event.h
             *
             * currently, it will be used by following events
             *
             * 1. hit lun-threshold event
             *    - user can choose dev_name[] or lun_index to be parameter,
             *      if uses dev_name[], lun_index MUST set to -1 (< 0)
             *      or, if uses lun_index (>= 0), the dev_name[] MUST be clear
             *
             *    dev_name format:
             *    - for zfs, it is 'zpool<x>/zfs<abc>'
             *    - for lvm, NOT defined yet
             *
             */
            char dev_name[256];
            int lun_index;
            int tp_threshold;
            int tp_avail;
        } __attribute__ ((__packed__)) iscsi_lun;
        struct __badblock
        {
            int raid_id;
            char pd_scsi_name[32];
            unsigned long long first_bad;
            unsigned long long bad_sectors;
            unsigned int count;
			int testMode;
        }__attribute__((__packed__)) badblock;
        struct __thin_err_version
        {
            char thin_pool_name[32];
        }__attribute__((__packed__)) thin_err_version;
        struct __tbt_virtual_nic
        {
            char tbt_virtual_nic_name[32];
            unsigned int nic_up_down_flag;
            unsigned int err_recovery_type;
        }__attribute__((__packed__)) tbt_virtual_nic;
        struct __cpu_thermal_throttling_event
        {
            int throttle_enable;
            int cpu_id;
        }__attribute__((__packed__)) cpu_thermal_throttling_event;
        struct __netlink_ncq_cb
        {
            int scsi_bus[4];
            int on_off;
        } __attribute__ ((__packed__)) netlink_ncq;
        struct __pd_message
        {
            char pd_name[32];
        } __attribute__ ((__packed__)) pd_message;
        struct __pool_message
        {
            char pool_name[32];
        } __attribute__ ((__packed__)) pool_message;
        struct __vol_message
        {
            char device_name[32];
        }__attribute__((__packed__)) vol_message;
        struct __oom_cb
        {
            unsigned int pid;
            char comm[32];
            char QNAP_QPKG[64];
        } __attribute__ ((__packed__)) oom_cb;        
        struct __netlink_mem_cb
        {
            int swap_file_action;
        } __attribute__ ((__packed__)) netlink_mem;
        struct __ir_input_value
        {
            unsigned short type;
            unsigned short code;
            int value;
        } __attribute__ ((__packed__)) ir_input_value;
        struct __bbu_learning
        {
            int learning_action;
        } __attribute__ ((__packed__)) bbu_learning;
        struct __event_control
        {
            int value;
        } __attribute__ ((__packed__)) event_control;
        struct __trct_cable_current_status_action
        {
            unsigned int modules;
            unsigned int port;
            unsigned int type;
            unsigned int cable_status;
        } __attribute__ ((__packed__)) trct_cable_current_status_action;
        struct __trct_daemon_status
        {
            unsigned int skip;
        } __attribute__ ((__packed__)) trct_daemon_status;
        struct __sys_temp
        {
            int enc_id;
            int warning_temp;
            int shutdown_temp;
        } __attribute__ ((__packed__)) sys_temp;
    } param;
}__attribute__ ((__packed__))
EVT_FUNC_ARG;

typedef struct
{
    EVT_FUNC_TYPE type;
    EVT_FUNC_ARG arg;
} __attribute__ ((__packed__))
NETLINK_EVT;

#ifdef __KERNEL__
extern int send_hal_netlink(NETLINK_EVT *event);
#endif

#endif
