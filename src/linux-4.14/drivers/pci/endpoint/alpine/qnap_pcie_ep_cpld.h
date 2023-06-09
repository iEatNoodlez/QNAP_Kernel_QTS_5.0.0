#ifndef __QNAP_PCIE_EP_CPLD_H
#define __QNAP_PCIE_EP_CPLD_H

#define BIT0  (0x00000001)
#define BIT1  (0x00000002)
#define BIT2  (0x00000004)
#define BIT3  (0x00000008)
#define BIT4  (0x00000010)
#define BIT5  (0x00000020)
#define BIT6  (0x00000040)
#define BIT7  (0x00000080)
#define BIT8  (0x00000100)
#define BIT9  (0x00000200)
#define BIT10 (0x00000400)
#define BIT11 (0x00000800)
#define BIT12 (0x00001000)
#define BIT13 (0x00002000)
#define BIT14 (0x00004000)
#define BIT15 (0x00008000)
#define BIT16 (0x00010000)
#define BIT17 (0x00020000)
#define BIT18 (0x00040000)
#define BIT19 (0x00080000)
#define BIT20 (0x00100000)
#define BIT21 (0x00200000)
#define BIT22 (0x00400000)
#define BIT23 (0x00800000)
#define BIT24 (0x01000000)
#define BIT25 (0x02000000)
#define BIT26 (0x04000000)
#define BIT27 (0x08000000)
#define BIT28 (0x10000000)
#define BIT29 (0x20000000)
#define BIT30 (0x40000000)
#define BIT31 (0x80000000)

typedef enum _CPLD_REG
{
	CPLD_VERSION = 0x00,
	CPLD_HDD_STATUS1,
	CPLD_HDD_STATUS2,
	CPLD_HDD_STATUS3,
	CPLD_HDD_STATUS4,
	CPLD_HDD_STATUS5,
	CPLD_HDD_STATUS6,
	CPLD_HDD_STATUS7,
	CPLD_HDD_STATUS8,
	CPLD_SYS_STATUS_GREEN,
	CPLD_SYS_STATUS_RED,
	CPLD_QTT_LINK_LED,
	CPLD_SYS_PWR_CTL,
	CPLD_TBT_HOSTPWR_PWR_BUTT = 0x0E,
	CPLD_TYPE_C_CON_STATUS,
	CPLD_TYPE_C_VBUS_STATUS,
	CPLD_ASM_VBUS_CONTROL,
} CPLD_REG;

typedef enum _CPLD_TYPE_C_STATUS
{
	CPLD_TYPE_C_USB_INSERT = 1,
	CPLD_TYPE_C_USB_REMOVED = 0,
	CPLD_TYPE_C_TBT_INSERT = 1,
	CPLD_TYPE_C_TBT_REMOVED = 0,
	CPLD_TYPE_C_VBUS_INSERT = 1,
	CPLD_TYPE_C_VBUS_REMOVED = 0,
} CPLD_TYPE_C_STATUS;

typedef enum _CPLD_EVENT_STATUS
{
	CPLD_EVENT_CABLE_USB_INSERT = 0x0a,
	CPLD_EVENT_CABLE_USB_REMOVED = 0x0b,
	CPLD_EVENT_CABLE_TBT_INSERT = 0x0c,
	CPLD_EVENT_CABLE_TBT_REMOVED = 0x0d,
} CPLD_EVENT_STATUS;

typedef enum _CPLD_CABLE_MODE
{
	CPLD_TBT1_USB_MODE_NULL	= 0,
	CPLD_TBT1_PA_USB_MODE	= 1,
	CPLD_TBT1_PB_USB_MODE	= 2,
	CPLD_TBT1_TBT_MODE_NULL = 0,
	CPLD_TBT1_PA_TBT_MODE   = 1,
	CPLD_TBT1_PB_TBT_MODE   = 2,
	CPLD_TBT2_USB_MODE_NULL = 0,
	CPLD_TBT2_PA_USB_MODE   = 1,
	CPLD_TBT2_PB_USB_MODE   = 2,
	CPLD_TBT2_TBT_MODE_NULL = 0,
	CPLD_TBT2_PA_TBT_MODE   = 1,
	CPLD_TBT2_PB_TBT_MODE   = 2,
};

typedef enum _CPLD_TBT_HOST_STATUS
{
	TR_HOST_S0 = 0,
	TR_HOST_S3,
	TR_HOST_S5,
        TR_HOST_S5B,
} CPLD_TBT_HOST_STATUS;

typedef enum _CPLD_MODULES_TYPE
{
	CPLD_M2_1 = 0x00,
	CPLD_M2_2 = 0x01
} CPLD_MODULES_TYPE;

typedef enum _CPLD_PORT_TYPE
{
	CPLD_PORTA = 0x00,
	CPLD_PORTB = 0x01
} CPLD_PORT_TYPE;

typedef enum _CPLD_STATUS_TYPE
{
	CPLD_TYPE_TBT = 0x00,
	CPLD_TYPE_USB = 0x01
} CPLD_STATUS_TYPE;

struct cpld_power_status_bits {
	unsigned char tbt1_s3_en:1;
	unsigned char tbt1_s0_en:1;
	unsigned char tbt2_s3_en:1;
	unsigned char tbt2_s0_en:1;
        unsigned char reserved1:1;
        unsigned char reserved2:1;
        unsigned char reserved3:1;
        unsigned char reserved4:1;
};

typedef union {
	struct cpld_power_status_bits status;
	unsigned char data;
} cpld_power_status_reg_params;

struct cable_status_bits {
	unsigned char tbt1_pa_tbt:1;
	unsigned char tbt1_pa_usb:1;
	unsigned char tbt1_pb_tbt:1;
	unsigned char tbt1_pb_usb:1;
	unsigned char tbt2_pa_tbt:1;
	unsigned char tbt2_pa_usb:1;
	unsigned char tbt2_pb_tbt:1;
	unsigned char tbt2_pb_usb:1;
};

typedef union {
	struct cable_status_bits status;
	unsigned char data;
} cable_status_reg_params;

struct cable_status_event_params {
	unsigned char tbt1_usb_insert_flag;
	unsigned char tbt2_usb_insert_flag;
	unsigned char tbt1_tbt_insert_flag;
	unsigned char tbt2_tbt_insert_flag;
	cable_status_reg_params g_cable_status_reg;
};

struct cable_vbus_status_bits {
	unsigned char tbt1_pa_vbus:1;
	unsigned char tbt1_pb_vbus:1;
	unsigned char tbt2_pa_vbus:1;
	unsigned char tbt2_pb_vbus:1;
};

typedef union {
	struct cable_vbus_status_bits status;
	unsigned char data;
} cable_vbus_status_reg_params;

extern int qnap_cpld_read(unsigned int off, void *val, size_t count);
extern int qnap_cpld_write(unsigned int off, void *val, size_t count);
extern int cpld_erp_onoff(unsigned char onoff);
extern void cpld_hdd_status_off(void);

#endif /* __QNAP_PCIE_EP_CPLD_H */
