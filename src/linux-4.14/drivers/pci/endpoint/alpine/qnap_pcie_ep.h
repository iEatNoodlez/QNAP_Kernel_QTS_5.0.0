#ifndef __QNAP_PCIE_EP_H
#define __QNAP_PCIE_EP_H

#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/dmapool.h>
#include <linux/cache.h>
#include <linux/circ_buf.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#define QNAP_MEM_SIZE		512 * 1024
#define HOST_MEM_BASE_0		0x4000000000ULL
#define HOST_MEM_BASE_1		0x8000000000ULL

/* This is the MMIO memory space size that we want to track for any host
 * initiated MMIO access
 */
#define MSIX_TABLE_OFFSET	0x2000
#define MSIX_PBA_OFFSET		0x3000
#define QNAP_MSIX_TABLE_SIZE	16

// 0 BAR0 0x10
// 1 BAR1 0x14
// 2 BAR2 0x18
// 3 BAR3 0x1C
// 4 BAR4 0x20
// 5 BAR5 0x24
// 6 Reserved
// 7 Reserved
enum pcie_bar_config {
	PCIE_EP_BAR0	= 0,
	PCIE_EP_BAR1	= 1,
	PCIE_EP_BAR2	= 2,
	PCIE_EP_BAR3	= 3,
	PCIE_EP_BAR4	= 4,
	PCIE_EP_BAR5	= 5,
};

enum pcie_ep_dma_direction {
	PCIE_EP_DMA_H2D = 0,	/* Host to device */
	PCIE_EP_DMA_D2H,	/* Device to host */
	PCIE_EP_DMA_M2M,	/* Device memory to Device memory, local memory only */
	PCIE_EP_MMIO_HOST,	/* MMIO from HOST */
	PCIE_EP_MMIO_DEVICE	/* MMIO from EP */
};

enum qnap_ep_event_type {
	QNAP_EP_CALLBACK	= 0,
	QNAP_EP_TYPE_MAX,
};

enum qnap_ep_port {
	QNAP_EP_PORT0	= 0,
	QNAP_EP_PORT1	= 1,
	QNAP_EP_POET2	= 2,
	QNAP_EP_PORT3	= 3,
	QNAP_EP_PORT_MAX,
};

struct qnap_pcie_ep_msix_entry {
	unsigned int msix_entry_address;
	unsigned int msix_entry_upper_address;
	unsigned int msix_entry_data;
	unsigned int msix_entry_vector;
};

struct reg_value_data {
	unsigned int reg;
	unsigned int size;
};

struct outbound_config_params {
	int pasw_mem_map;
	uint64_t pasw_mem_base;
	uint64_t pasw_mem_size_log2;
};

struct ep_id_params {
	unsigned int vendor_id;
	unsigned int device_id;
	unsigned int revision_id;
	unsigned int subsystem_vendor_id;
	unsigned int subsystem_device_id;
};

struct qnap_ep_device {
	unsigned char ep_port_id;
	unsigned char ep_uboot_init_flag;
	unsigned char crs_flag;
	void *pbs_addr_base;
	unsigned int pbs_size;
	void *pcie_port_addr_base;
	unsigned int pcie_port_size;
	void *inbound_base_addr_bar0;
	unsigned int inbound_bar0_size;
	void *inbound_base_addr_bar2;
	unsigned int inbound_bar2_size;
	void *inbound_base_addr_bar4;
	unsigned int inbound_bar4_size;
	void *outbound_base_addr;
	unsigned long long outbound_size;
	struct al_pcie_port *pcie_port_config;
	struct ep_id_params id_params;
};

struct ep_ports_list {
	unsigned char port_counts;
	unsigned char port_id;
	unsigned char port_uboot_init_flag;
	struct qnap_ep_device qnap_ep;
	struct list_head list;
};

struct ep_port_info_data {
	unsigned char ep_ports_counts;
	unsigned char ep_uboot_init_flag;
	unsigned char ep_ports[4];
	unsigned char gpio_intx_counts;
	unsigned char gpio_intx_num[2];
	unsigned char gpio_reset_counts;
	unsigned char gpio_reset_num[4];
	unsigned char gpio_sw_ready_counts;
	unsigned char gpio_sw_ready_num[4];
	unsigned char gpio_usb_mux_num;
	unsigned char gpio_usb_mux_init_value;
	unsigned char cpld_i2c_bus_id;
	unsigned char cpld_i2c_slave_address;
	struct ep_id_params id_params[4];
};

struct qnap_ep_data {
	int intx_irqnum[2];
	char intx_irqname_0[10];
	char intx_irqname_1[10];
	char intx_irqthreadname_0[10];
	char intx_irqthreadname_1[10];
	char intx_gpioname_0[30];
	char intx_gpioname_1[30];
	int reset_irqnum[4];
	char reset_irqname_0[10];
	char reset_irqname_1[10];
	char reset_irqname_2[10];
	char reset_irqname_3[10];
	char reset_irqthreadname_0[10];
	char reset_irqthreadname_1[10];
	char reset_irqthreadname_2[10];
	char reset_irqthreadname_3[10];
	char reset_gpioname_0[30];
	char reset_gpioname_1[30];
	char reset_gpioname_2[30];
	char reset_gpioname_3[30];
	char sw_ready_gpioname_0[30];
	char sw_ready_gpioname_1[30];
	char sw_ready_gpioname_2[30];
	char sw_ready_gpioname_3[30];
	char usb_mux_gpioname[30];
	struct ep_port_info_data ep_info_data;
	struct i2c_adapter *i2c_adap;
	struct ep_ports_list *tmp_ep_ports_list;
	struct ep_ports_list ep_ports_mylist;
	struct task_struct *ep_intx_thread[2];
	struct task_struct *ep_reset_thread[4];
};

struct qnap_ep_event {
	unsigned int port_num;
	unsigned int type;
};

struct qnap_ep_fabrics_ops {
	struct module *owner;
	unsigned int type;
	int (*callback_response)(struct qnap_ep_event *event);
};

extern int qnap_ep_get_port_count_fun(void);
extern int qnap_ep_pcie_port_list_parser_fun(unsigned char port_num, struct qnap_ep_device *ep);
extern void qnap_ep_rcs_fun(unsigned char port_num, unsigned crsonoff);
extern int qnap_ep_uboot_init_fun(unsigned char port_num);
extern int qnap_ep_register(struct qnap_ep_fabrics_ops *ops);
extern void qnap_ep_unregister(struct qnap_ep_fabrics_ops *ops);
extern void qnap_ep_cpld_power_status_update(unsigned char data);
extern void qnap_ep_cable_vbus_status_update(unsigned char data);
extern void qnap_ep_cable_status_update(unsigned char data);
extern void qnap_ep_ready_set_high(unsigned char port_num);
extern void qnap_ep_ready_set_low(unsigned char port_num);
extern void qnap_ep_asm_vbus_ctl_set_high(unsigned char port_num);
extern void qnap_ep_asm_vbus_ctl_set_low(unsigned char port_num);
extern void qnap_ep_usb_mux_set_high(void);
extern void qnap_ep_usb_mux_set_low(void);
extern int qnap_ep_btn_status(unsigned char onoff);

extern void qnap_ep_link_list_fun(void);
extern int qnap_ep_get_port_count_fun(void);
extern int qnap_ep_pcie_port_list_parser_fun(unsigned char port_num, struct qnap_ep_device *ep);
extern void qnap_ep_rcs_fun(unsigned char port_num, unsigned crsonoff);

extern int qnap_udma_memcpy(int len, dma_addr_t source, dma_addr_t destination, int direction);
extern unsigned int pcie_ep_mmio_read(u32 base_reg, int direction);
extern void pcie_ep_mmio_write(u32 base_reg, u32 val, int direction);

#endif /* __QNAP_PCIE_EP_H */
