#include <linux/io.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/sysfs.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>

#include <asm/signal.h>

#include <soc/alpine/al_hal_ssm.h>
#include <soc/alpine/al_hal_udma.h>
#include <soc/alpine/al_hal_pcie.h>
#include <soc/alpine/al_hal_m2m_udma.h>
#include <soc/alpine/al_hal_addr_map.h>
#include <soc/alpine/al_hal_udma_fast.h>
#include <soc/alpine/al_hal_pbs_utils.h>
#include <soc/alpine/al_hal_udma_config.h>
#include <soc/alpine/al_hal_plat_services.h>
#include <soc/alpine/al_hal_ssm_crc_memcpy.h>
#include <soc/alpine/al_hal_unit_adapter_regs.h>

#include "qnap_pcie_ep.h"
#include "qnap_pcie_ep_cpld.h"
#include "qnap_pcie_ep_hal_event.h"

static struct qnap_ep_fabrics_ops *qnap_ep_ops[QNAP_EP_TYPE_MAX];
DECLARE_RWSEM(qnap_ep_config_sem);

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
static cpld_power_status_reg_params g_cpld_power_status_reg;
static cable_vbus_status_reg_params g_cable_vbus_status_reg;
static cable_status_reg_params g_cable_status_reg;
unsigned char tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
EXPORT_SYMBOL(tbt1_usb_insert_flag);
unsigned char tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
EXPORT_SYMBOL(tbt2_usb_insert_flag);
unsigned char tbt1_tbt_insert_flag = CPLD_TBT1_TBT_MODE_NULL;
EXPORT_SYMBOL(tbt1_tbt_insert_flag);
unsigned char tbt2_tbt_insert_flag = CPLD_TBT2_TBT_MODE_NULL;
EXPORT_SYMBOL(tbt2_tbt_insert_flag);
unsigned char tbt1_power_status = 0;
EXPORT_SYMBOL(tbt1_power_status);
unsigned char tbt2_power_status = 0;
EXPORT_SYMBOL(tbt2_power_status);
char tr_power_status = -1;
EXPORT_SYMBOL(tr_power_status);
static unsigned char tbt_power_debug = 0;
unsigned char cpld_i2c_ready = 0;
EXPORT_SYMBOL(cpld_i2c_ready);
static unsigned char cable_debug = 0;
static unsigned char power_button_status = 0;
EXPORT_SYMBOL(power_button_status);
#endif

struct qnap_ep_data *g_dev_qnap_ep;
static int pasw_mem_port_init_count = 0;
static struct proc_dir_entry *proc_qnap_pcie_ep_root = NULL;
static int g_i_worked = 0;
EXPORT_SYMBOL(g_dev_qnap_ep);

#if 0
static uint g_mode = 0;

static char *mode_name[] =
{
	"NULL",
};
#endif

void setting_mode(uint cmd)
{
	struct qnap_ep_device ep;
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	struct qnap_ep_event event;
	unsigned char tmp, i;
	unsigned char g_power_status, g_type_c_status, g_type_c_vbus_status;
	cpld_power_status_reg_params now_cpld_power_status_reg;
	cable_vbus_status_reg_params now_cable_vbus_status_reg;
	cable_status_reg_params now_cable_status_reg;
	struct cable_hal_event_status ches;
#endif

	switch(cmd) {
	case 1:
		printk("hello word !!!\n");
		break;
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	case 30:
		qnap_cpld_read(0x00, &tmp, 1);
		printk("cpld reg 0x00 data = 0x%x\n", tmp);
		break;
#endif
	case 31:
		qnap_ep_link_list_fun();
		break;
	case 32:
		printk("EP port num : %d\n", qnap_ep_get_port_count_fun());
		break;
	case 33:
		qnap_ep_pcie_port_list_parser_fun(0, &ep);
		printk("====================\n");
		printk("ep.ep_port_id = 0x%x\n", ep.ep_port_id);
		printk("ep.ep_uboot_init_flag = 0x%x\n", ep.ep_uboot_init_flag);
		printk("ep.crs_flag = 0x%x\n", ep.crs_flag);
		printk("ep.pbs_addr_base virt = 0x%lx phy = 0x%lx\n", (long unsigned int) ep.pbs_addr_base,
							(long unsigned int) virt_to_phys(ep.pbs_addr_base));
		printk("ep.pcie_port_addr_base virt = 0x%lx, phy = 0x%lx\n", (long unsigned int) ep.pcie_port_addr_base,
									(long unsigned int) virt_to_phys(ep.pcie_port_addr_base));
		printk("ep.inbound_base_addr_bar0 virt = 0x%lx, phy = 0x%lx\n",
					(long unsigned int) ep.inbound_base_addr_bar0, (long unsigned int) virt_to_phys(ep.inbound_base_addr_bar0));
		printk("ep.outbound_base_addr phy = 0x%lx\n", (long unsigned int) ep.outbound_base_addr);
		printk("ep.pcie_port_config virt = 0x%lx, phy = 0x%lx\n", (long unsigned int) ep.pcie_port_config,
					(long unsigned int) virt_to_phys(ep.pcie_port_config));
		printk("====================\n");

		qnap_ep_pcie_port_list_parser_fun(1, &ep);
		printk("====================\n");
		printk("ep.ep_port_id = 0x%x\n", ep.ep_port_id);
		printk("ep.ep_uboot_init_flag = 0x%x\n", ep.ep_uboot_init_flag);
		printk("ep.crs_flag = 0x%x\n", ep.crs_flag);
		printk("ep.pbs_addr_base virt = 0x%lx phy = 0x%lx\n", (long unsigned int) ep.pbs_addr_base,
							(long unsigned int) virt_to_phys(ep.pbs_addr_base));
		printk("ep.pcie_port_addr_base virt = 0x%lx, phy = 0x%lx\n", (long unsigned int) ep.pcie_port_addr_base,
							(long unsigned int) virt_to_phys(ep.pcie_port_addr_base));
		printk("ep.inbound_base_addr_bar0 virt = 0x%lx, phy = 0x%lx\n",
					(long unsigned int) ep.inbound_base_addr_bar0, (long unsigned int) virt_to_phys(ep.inbound_base_addr_bar0));
		printk("ep.outbound_base_addr phy = 0x%lx\n", (long unsigned int) ep.outbound_base_addr);
		printk("ep.pcie_port_config virt = 0x%lx, phy = 0x%lx\n", (long unsigned int) ep.pcie_port_config,
					(long unsigned int) virt_to_phys(ep.pcie_port_config));
		printk("====================\n");
		break;
	case 34:
		qnap_ep_rcs_fun(0, 1);
		break;
	case 35:
		qnap_ep_rcs_fun(0, 0);
		break;
	case 36:
		qnap_ep_rcs_fun(1, 1);
		break;
        case 37:
		qnap_ep_rcs_fun(1, 0);
		break;
	case 38:
		printk("gpio41 ready pin to high\n");
		gpio_set_value(41, 1);
		break;
	case 39:
		printk("gpio41 ready pin to low\n");
		gpio_set_value(41, 0);
		break;
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	case 40:
		qnap_ep_ready_set_high(0);
		break;
	case 41:
		qnap_ep_ready_set_high(1);
		break;
	case 42:
		qnap_ep_ready_set_low(0);
		break;
	case 43:
		qnap_ep_ready_set_low(1);
		break;
	case 44:
		printk("mux EP1 port\n");
		qnap_ep_usb_mux_set_high();
		break;
	case 45:
		printk("mux EP0 port\n");
		qnap_ep_usb_mux_set_low();
		break;
	case 50:
		printk("g_cable_status_reg.data = 0x%x\n", g_cable_status_reg.data);
		printk("g_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pb_tbt);
		printk("g_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pb_usb);
		printk("g_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pa_tbt);
		printk("g_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pa_usb);
		printk("g_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pb_tbt);
		printk("g_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pb_usb);
		printk("g_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pa_tbt);
		printk("g_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pa_usb);
		break;
	case 51:
		for(i=0;i<QNAP_EP_TYPE_MAX;i++) {
			if(qnap_ep_ops[i] == NULL)
				printk("qnap_ep_ops[%d] = 0x%0x\n", i, qnap_ep_ops[i]);
			else
				printk("qnap_ep_ops[%d] = 0x%0x\n", i, qnap_ep_ops[i]);
		}
		break;
	case 52:
		event.port_num = 0;
		event.type = CPLD_EVENT_CABLE_USB_INSERT;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);
		break;
	case 53:
		event.port_num = 0;
		event.type = CPLD_EVENT_CABLE_USB_REMOVED;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);
		break;
	case 54:
		event.port_num = 0;
		event.type = CPLD_EVENT_CABLE_TBT_INSERT;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);
	case 55:
		event.port_num = 0;
		event.type = CPLD_EVENT_CABLE_TBT_REMOVED;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);

		break;
	case 56:
		event.port_num = 1;
		event.type = CPLD_EVENT_CABLE_USB_INSERT;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);
		event.port_num = 1;
		event.type = CPLD_EVENT_CABLE_USB_REMOVED;
		if(qnap_ep_ops[QNAP_EP_CALLBACK] != NULL)
			qnap_ep_ops[QNAP_EP_CALLBACK]->callback_response(&event);
		break;
	case 57:
		printk("tbt1_usb_insert_flag = 0x%x\n", tbt1_usb_insert_flag);
		printk("tbt2_usb_insert_flag = 0x%x\n", tbt2_usb_insert_flag);
		printk("tbt1_tbt_insert_flag = 0x%x\n", tbt1_tbt_insert_flag);
		printk("tbt2_tbt_insert_flag = 0x%x\n", tbt2_tbt_insert_flag);
		break;
	case 58:
		tbt1_usb_insert_flag = 0;
		tbt2_usb_insert_flag = 0;
		tbt1_tbt_insert_flag = 0;
		tbt2_tbt_insert_flag = 0;
		break;
	case 59:
		cable_debug = 1;
		printk("enable cable detection message\n");
		break;
	case 60:
		cable_debug = 0;
		printk("disable cable detection message\n");
		break;
	case 61:
		printk("g_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pb_tbt);
		printk("g_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pb_usb);
		printk("g_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pa_tbt);
		printk("g_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pa_usb);
		printk("g_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pb_tbt);
		printk("g_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pb_usb);
		printk("g_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pa_tbt);
		printk("g_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pa_usb);

		printk("g_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pa_vbus);
		printk("g_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pb_vbus);
		printk("g_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pa_vbus);
		printk("g_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pb_vbus);
		break;
	case 62:
		printk("g_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pb_tbt);
		printk("g_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pb_usb);
		printk("g_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pa_tbt);
		printk("g_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pa_usb);
		printk("g_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pb_tbt);
		printk("g_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pb_usb);
		printk("g_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pa_tbt);
		printk("g_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pa_usb);

		printk("g_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pa_vbus);
		printk("g_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pb_vbus);
		printk("g_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pa_vbus);
		printk("g_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pb_vbus);

#if 1
		qnap_cpld_read(CPLD_TBT_HOSTPWR_PWR_BUTT, &g_power_status, 1);
		qnap_ep_cpld_power_status_update(g_power_status);

		qnap_cpld_read(CPLD_TYPE_C_VBUS_STATUS, &g_type_c_vbus_status, 1);
		qnap_ep_cable_vbus_status_update(g_type_c_vbus_status);

		qnap_cpld_read(CPLD_TYPE_C_CON_STATUS, &g_type_c_status, 1);
		qnap_ep_cable_status_update(g_type_c_status);

		now_cpld_power_status_reg.data = g_power_status;
		now_cable_vbus_status_reg.data = g_type_c_vbus_status;
		now_cable_status_reg.data = g_type_c_status;

		printk("now_cpld_power_status_reg.data = 0x%x\n", now_cpld_power_status_reg.data);
		printk("now_cable_vbus_status_reg.data = 0x%x\n", now_cable_vbus_status_reg.data);
		printk("now_cable_status_reg.data = 0x%x\n", now_cable_status_reg.data);
		printk("now_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pb_tbt);
		printk("now_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pb_usb);
		printk("now_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pa_tbt);
		printk("now_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pa_usb);
		printk("now_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pb_tbt);
		printk("now_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pb_usb);
		printk("now_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pa_tbt);
		printk("now_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pa_usb);
		printk("now_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pa_vbus);
		printk("now_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pb_vbus);
		printk("now_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pa_vbus);
		printk("now_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif
		break;
#endif
	case 90:
		printk("hal event to hal daemon\n");
		ches.modules = 0x01;
		ches.port = 0x0a;
		ches.type = 0x01;
		ches.cable_status = 0x01;
		qnap_send_ep_cable_hal_event(&ches);
		break;
	case 91:
		printk("off event shutdown\n");
		tbt_power_debug = 1;
		break;
	case 92:
		printk("on event shutdown\n");
		tbt_power_debug = 0;
		break;
	case 300:
		power_button_status = 1;
		break;
	case 301:
		power_button_status = 0;
		break;
	case 302:
		printk("power_button_status = 0x%x\n", power_button_status);
		break;
	default:
		printk("QNAP PCIE EP Control function error\n");
		break;
	}

	return;
};

static ssize_t qnap_pcie_ep_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;

	//printk("mode_name[%d] = %s\n", g_mode, mode_name[g_mode]);

	return len;
};

static ssize_t qnap_pcie_ep_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	int ret = -1;
	unsigned char value[100], cmd[100];
	unsigned int tmp;
	unsigned char wval, cval, aval;
	unsigned char asm_vbus_ctl_val = 0;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';
	memset(cmd,0x0,sizeof(cmd));
	sscanf(value, "%s %x\n", &cmd, &wval);
	sscanf(value, "%s %c\n", &cmd, &cval);
	sscanf(value, "%s %c %c\n", &cmd, &cval, &aval);
	if (strcmp(cmd, "btn") == 0) {
		if((cval >= 'a') && (cval <= 'z')) {
			if(qnap_ep_btn_status(cval) < 0)
				printk("value fail !! cval = 0x%x, ascii = %c\n", cval, cval);
		} else {
			if(qnap_ep_btn_status(wval) < 0)
			printk("value fail !! wval = 0x%x\n", wval);
		}
	} else if (strcmp(cmd, "nvme_ready") == 0) {
		if((cval >= '0') && (cval <= '1')) {
			if((cval == '0') && (aval == '0')) {
				qnap_ep_ready_set_low(0);
			} else if((cval == '0') && (aval == '1')) {
				qnap_ep_ready_set_high(0);
			} else if((cval == '1') && (aval == '0')) {
				qnap_ep_ready_set_low(1);
			} else if((cval == '1') && (aval == '1')) {
				qnap_ep_ready_set_high(1);
			} else {
				printk("value fail !! cval = 0x%x, ascii = %c, aval = 0x%x, ascii = %c\n", cval, cval, aval, aval);
			}
		} else {
			printk("value fail !! cval = 0x%x, ascii = %c, aval = 0x%x, ascii = %c\n", cval, cval, aval, aval);
		}
	} else if (strcmp(cmd, "asm_vbus_ctl") == 0) {
		if((cval >= '0') && (cval <= '1')) {
			//ret = qnap_cpld_read(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
			//if(ret < 0)
			//	printk("read cpld fail\n");
			if((cval == '0') && (aval == '0')) {
				qnap_ep_asm_vbus_ctl_set_low(0);
				//asm_vbus_ctl_val = asm_vbus_ctl_val & ~0x02; // EP0
			} else if((cval == '0') && (aval == '1')) {
				qnap_ep_asm_vbus_ctl_set_high(0);
				//asm_vbus_ctl_val = asm_vbus_ctl_val | 0x02;  // EP0
			} else if((cval == '1') && (aval == '0')) {
				qnap_ep_asm_vbus_ctl_set_low(1);
				//asm_vbus_ctl_val = asm_vbus_ctl_val & ~0x01; // EP1
			} else if((cval == '1') && (aval == '1')) {
				qnap_ep_asm_vbus_ctl_set_high(1);
				//asm_vbus_ctl_val = asm_vbus_ctl_val | 0x01;  // EP1
			} else {
				printk("value fail !! cval = 0x%x, ascii = %c, aval = 0x%x, ascii = %c\n", cval, cval, aval, aval);
			}
			//ret = qnap_cpld_write(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
			//if(ret < 0)
			//	printk("write cpld fail\n");
		} else {
			printk("value fail !! cval = 0x%x, ascii = %c, aval = 0x%x, ascii = %c\n", cval, cval, aval, aval);
		}
	} else if (strcmp(cmd, "usb_mux") == 0) {
		if((cval >= '0') && (cval <= '1')) {
			if(cval == '0') {
				qnap_ep_usb_mux_set_low();
			} else if(cval == '1') {
				qnap_ep_usb_mux_set_high();
			} else {
				printk("value fail !! cval = 0x%x, ascii = %c\n", cval, cval);
			}
		} else {
			printk("value fail !! cval = 0x%x, ascii = %c\n", cval, cval);
		}
	} else {
		sscanf(value,"%u\n", &tmp);
		//printk("tmp = %d\n", tmp);
		setting_mode(tmp);
		//g_mode = tmp;
	}

	return count;
};

static const struct file_operations qnap_pcie_ep_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= qnap_pcie_ep_read_proc,
	.write			= qnap_pcie_ep_write_proc
};

static int qnap_pcie_ep_boardcfg_parser(struct ep_port_info_data *data)
{
	void *fdt = initial_boot_params;
	int fdt_ep_node;
	int len, i;
	const __be32 *ep_port_prop;
	const char *ep_uboot_prop;
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	const __be32 *gpio_prop;
	const __be32 *cpld_prop;
	const __be32 *id_prop;
#endif
	fdt_ep_node = fdt_path_offset(fdt, "/soc/board-cfg/pcie");
	if (!fdt_ep_node) {
		pr_err("warning: device tree node '%s' has no address.\n", "/soc/board-cfg/pcie");
		return -EINVAL;
	}

	ep_port_prop = fdt_getprop(fdt, fdt_ep_node, "ep-ports", &len);
	if (!ep_port_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep-ports");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			data->ep_ports_counts = i + 1;
			data->ep_ports[i] = fdt32_to_cpu(ep_port_prop[i]);
		}
		if(data->ep_ports_counts > 4) {
			printk("warning: EP port suport max : 4 port\n");
			return -EINVAL;
		}
	}

	ep_uboot_prop = fdt_getprop(fdt, fdt_ep_node, "ep-uboot-init", NULL);
	if (!ep_uboot_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep-uboot-init");
		pr_err("warning: It will set to default kernel init\n");
		data->ep_uboot_init_flag = AL_FALSE;
	} else {
		if(ep_uboot_prop && (!strcmp(ep_uboot_prop, "enabled") ||
				    !strcmp(ep_uboot_prop, "okay") ||
				    !strcmp(ep_uboot_prop, "ok")))
		{
			data->ep_uboot_init_flag = AL_TRUE;
		} else {
			data->ep_uboot_init_flag = AL_FALSE;
		}
		//printk("ep_uboot_prop = %s\n", ep_uboot_prop);
		//printk("data->ep_uboot_init_flag = %d\n", data->ep_uboot_init_flag);
	}

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	gpio_prop = fdt_getprop(fdt, fdt_ep_node, "pl061,gpio-intx-pin", &len);
	if (!gpio_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "pl061,gpio-intx-pin");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			data->gpio_intx_counts = i + 1;
			data->gpio_intx_num[i] = fdt32_to_cpu(gpio_prop[i]);
		}

		if(data->gpio_intx_counts > 2) {
			printk("warning: intx suport max : 2\n");
			return -EINVAL;
		}
	}

	gpio_prop = fdt_getprop(fdt, fdt_ep_node, "pl061,gpio-reset-pin", &len);
	if (!gpio_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "pl061,gpio-reset-pin");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			data->gpio_reset_counts = i + 1;
			data->gpio_reset_num[i] = fdt32_to_cpu(gpio_prop[i]);
		}

		if(data->ep_ports_counts != data->gpio_reset_counts) {
			printk("warning: ep port support %d port\n", data->ep_ports_counts);
			printk("warning: ep reset pin %d counts\n", data->gpio_reset_counts);
			printk("warning: reset ready suport max : for pcie ep port mapping\n");
			return -EINVAL;
		}
	}

	gpio_prop = fdt_getprop(fdt, fdt_ep_node, "pl061,gpio-sw-ready-pin", &len);
	if (!gpio_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "pl061,gpio-sw-ready-pin");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			data->gpio_sw_ready_counts = i + 1;
			data->gpio_sw_ready_num[i] = fdt32_to_cpu(gpio_prop[i]);
		}

		if(data->ep_ports_counts != data->gpio_sw_ready_counts) {
			printk("warning: ep port support %d port\n", data->ep_ports_counts);
			printk("warning: ep sw ready pin %d counts\n", data->gpio_sw_ready_counts);
			printk("warning: sw ready suport max : for pcie ep port mapping\n");
			return -EINVAL;
		}
	}

	gpio_prop = fdt_getprop(fdt, fdt_ep_node, "pl061,gpio-usb-mux-pin", &len);
	if (!gpio_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "pl061,gpio-usb-mux-pin");
		return -EINVAL;
	} else {
		data->gpio_usb_mux_num = fdt32_to_cpu(gpio_prop[0]);
		data->gpio_usb_mux_init_value = fdt32_to_cpu(gpio_prop[1]);
		if(len/4 < 2 || len/4 > 2) {
			printk("warning: usb mux suport max : 1\n");
			return -EINVAL;
		}

		if((data->gpio_usb_mux_init_value != GPIOF_INIT_HIGH) &&
			(data->gpio_usb_mux_init_value != GPIOF_INIT_LOW)) {
			printk("warning: usb mux default init is support 0 or 1\n");
			printk("warning: usb mux is %d value\n", data->gpio_usb_mux_init_value);
			return -EINVAL;
		}
	}

	cpld_prop = fdt_getprop(fdt, fdt_ep_node, "cpld,i2c-bus-id", &len);
	if(!cpld_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "cpld,i2c-bus-id");
		return -EINVAL;
	} else {
		data->cpld_i2c_bus_id = fdt32_to_cpu(cpld_prop[0]);

		if(len/4 > 1) {
			printk("warning: i2c bus id suport max : 1\n");
			return -EINVAL;
		}
	}

	cpld_prop = fdt_getprop(fdt, fdt_ep_node, "cpld,i2c-slave-address", &len);
	if(!cpld_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "cpld,i2c-slave-address");
		return -EINVAL;
	} else {
		data->cpld_i2c_slave_address = fdt32_to_cpu(cpld_prop[0]);

		if(len/4 > 1) {
			printk("warning: i2c slave address suport max : 1\n");
			return -EINVAL;
		}
	}

#if !defined(CONFIG_PCIE_KCONF_ENDPOINT_ID)
	memset(data->id_params, 0xFF, sizeof(struct ep_id_params) * 4);
	id_prop = fdt_getprop(fdt, fdt_ep_node, "ep,vendor-id", &len);
	if(!id_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep,vendor-id");
		for(i=0;i<4;i++) {
			data->id_params[i].vendor_id = 0x1baa;
		}
	} else {
		for(i=0;i<len/4;i++) {
			data->id_params[i].vendor_id = fdt32_to_cpu(id_prop[i]);
			//printk("data->id_params[%d].vendor_id = 0x%x\n", i, data->id_params[i].vendor_id);
		}

		for(i=0;i<data->ep_ports_counts;i++) {
			if(data->id_params[i].vendor_id == 0xffffffff) {
				data->id_params[i].vendor_id = 0x1baa;
			}
		}
	}

	id_prop = fdt_getprop(fdt, fdt_ep_node, "ep,device-id", &len);
	if(!id_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep,device-id");
                for(i=0;i<4;i++) {
			data->id_params[i].device_id = 0xFFFF;
                }
	} else {
		for(i=0;i<len/4;i++) {
			data->id_params[i].device_id = fdt32_to_cpu(id_prop[i]);
			//printk("data->id_params[%d].device_id = 0x%x\n", i, data->id_params[i].device_id);
		}

		for(i=0;i<data->ep_ports_counts;i++) {
			if(data->id_params[i].device_id == 0xffffffff) {
				data->id_params[i].device_id = 0x0000;
			}
		}
	}

	id_prop = fdt_getprop(fdt, fdt_ep_node, "ep,revision-id", &len);
	if(!id_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep,revision-id");
		for(i=0;i<4;i++) {
			data->id_params[i].revision_id = 0x0000;
		}
	} else {
		for(i=0;i<len/4;i++) {
			data->id_params[i].revision_id = fdt32_to_cpu(id_prop[i]);
			//printk("data->id_params[%d].revision_id = 0x%x\n", i, data->id_params[i].revision_id);
		}

		for(i=0;i<data->ep_ports_counts;i++) {
			if(data->id_params[i].revision_id == 0xffffffff) {
				data->id_params[i].revision_id = 0x0000;
			}
		}
	}

	id_prop = fdt_getprop(fdt, fdt_ep_node, "ep,subsystem-vendor-id", &len);
	if(!id_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep,subsystem-vendor-id");
		for(i=0;i<4;i++) {
			data->id_params[i].subsystem_vendor_id = 0x1baa;
		}
	} else {
                for(i=0;i<len/4;i++) {
                        data->id_params[i].subsystem_vendor_id = fdt32_to_cpu(id_prop[i]);
			//printk("data->id_params[%d].subsystem_vendor_id = 0x%x\n", i, data->id_params[i].subsystem_vendor_id);
                }

		for(i=0;i<data->ep_ports_counts;i++) {
			if(data->id_params[i].subsystem_vendor_id == 0xffffffff) {
				data->id_params[i].subsystem_vendor_id = 0x1baa;
			}
		}
	}

	id_prop = fdt_getprop(fdt, fdt_ep_node, "ep,subsystem-device-id", &len);
	if(!id_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/board-cfg/pcie", "ep,subsystem-device-id");
		for(i=0;i<4;i++) {
			data->id_params[i].subsystem_device_id = 0xFFFF;
		}
	} else {
		for(i=0;i<len/4;i++) {
			data->id_params[i].subsystem_device_id = fdt32_to_cpu(id_prop[i]);
			//printk("data->id_params[%d].subsystem_device_id = 0x%x\n", i, data->id_params[i].subsystem_device_id);
		}

		for(i=0;i<data->ep_ports_counts;i++) {
			if(data->id_params[i].subsystem_device_id == 0xffffffff) {
				data->id_params[i].subsystem_device_id = 0x0000;
			}
		}
	}
#else
	memset(data->id_params, 0xFF, sizeof(struct ep_id_params) * 4);
#if defined(CONFIG_PCIE_EP0_ENDPOINT_ID) && defined(CONFIG_PCIE_KCONF_ENDPOINT_ID)
	data->id_params[0].vendor_id = (CONFIG_PCIE_EP0_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[0].device_id = (CONFIG_PCIE_EP0_DEVICE_ID_ADDR & 0x0000ffff);
	data->id_params[0].revision_id = (CONFIG_PCIE_EP0_REVISION_ID_ADDR & 0x0000ffff);
	data->id_params[0].subsystem_vendor_id = (CONFIG_PCIE_EP0_SUBSYSTEM_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[0].subsystem_device_id = (CONFIG_PCIE_EP0_SUBSYSTEM_DEVICE_ID_ADDR & 0x0000ffff);
#else
	data->id_params[0].vendor_id = 0x1baa;
	data->id_params[0].revision_id = 0x0001;
	data->id_params[0].subsystem_vendor_id = 0x1baa;
#endif
#if defined(CONFIG_PCIE_EP1_ENDPOINT_ID) && defined(CONFIG_PCIE_KCONF_ENDPOINT_ID)
	data->id_params[1].vendor_id = (CONFIG_PCIE_EP1_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[1].device_id = (CONFIG_PCIE_EP1_DEVICE_ID_ADDR & 0x0000ffff);
	data->id_params[1].revision_id = (CONFIG_PCIE_EP1_REVISION_ID_ADDR & 0x0000ffff);
	data->id_params[1].subsystem_vendor_id = (CONFIG_PCIE_EP1_SUBSYSTEM_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[1].subsystem_device_id = (CONFIG_PCIE_EP1_SUBSYSTEM_DEVICE_ID_ADDR & 0x0000ffff);
#else
	data->id_params[1].vendor_id = 0x1baa;
	data->id_params[1].revision_id = 0x0002;
	data->id_params[1].subsystem_vendor_id = 0x1baa;
#endif
#if defined(CONFIG_PCIE_EP2_ENDPOINT_ID) && defined(CONFIG_PCIE_KCONF_ENDPOINT_ID)
	data->id_params[2].vendor_id = (CONFIG_PCIE_EP2_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[2].device_id = (CONFIG_PCIE_EP2_DEVICE_ID_ADDR & 0x0000ffff);
	data->id_params[2].revision_id = (CONFIG_PCIE_EP2_REVISION_ID_ADDR & 0x0000ffff);
	data->id_params[2].subsystem_vendor_id = (CONFIG_PCIE_EP2_SUBSYSTEM_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[2].subsystem_device_id = (CONFIG_PCIE_EP2_SUBSYSTEM_DEVICE_ID_ADDR & 0x0000ffff);
#else
	data->id_params[2].vendor_id = 0x1baa;
	data->id_params[2].revision_id = 0x0003;
	data->id_params[2].subsystem_vendor_id = 0x1baa;
#endif
#if defined(CONFIG_PCIE_EP3_ENDPOINT_ID) && defined(CONFIG_PCIE_KCONF_ENDPOINT_ID)
	data->id_params[3].vendor_id = CONFIG_PCIE_EP3_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[3].device_id = CONFIG_PCIE_EP3_DEVICE_ID_ADDR & 0x0000ffff);
	data->id_params[3].revision_id = CONFIG_PCIE_EP3_REVISION_ID_ADDR & 0x0000ffff);
	data->id_params[3].subsystem_vendor_id = CONFIG_PCIE_EP3_SUBSYSTEM_VENDOR_ID_ADDR & 0x0000ffff);
	data->id_params[3].subsystem_device_id = CONFIG_PCIE_EP3_SUBSYSTEM_DEVICE_ID_ADDR & 0x0000ffff);
#else
	data->id_params[3].vendor_id = 0x1baa;
	data->id_params[3].revision_id = 0x0004;
	data->id_params[3].subsystem_vendor_id = 0x1baa;
#endif
#endif
#else
	data->gpio_intx_counts = 0;
	data->gpio_sw_ready_counts = 0;
	data->cpld_i2c_bus_id = 0;
	data->cpld_i2c_slave_address = 0;
	memset(data->id_params, 0xFF, sizeof(struct ep_id_params) * 4);
	data->id_params[0].vendor_id = 0x1baa;
	data->id_params[0].revision_id = 0x0001;
	data->id_params[0].subsystem_vendor_id = 0x1baa;
	data->id_params[1].vendor_id = 0x1baa;
	data->id_params[1].revision_id = 0x0002;
	data->id_params[1].subsystem_vendor_id = 0x1baa;
	data->id_params[2].vendor_id = 0x1baa;
	data->id_params[2].revision_id = 0x0003;
	data->id_params[2].subsystem_vendor_id = 0x1baa;
	data->id_params[3].vendor_id = 0x1baa;
	data->id_params[3].revision_id = 0x0004;
	data->id_params[3].subsystem_vendor_id = 0x1baa;
#endif

	return 0;
}

static int qnap_pcie_ep_pcie_external_parser(struct reg_value_data *data, unsigned char pcie_port_num)
{
	void *fdt = initial_boot_params;
	int fdt_pcie_node;
	const __be32 *pcie_prop;
	int pcie_port_reg[4] = {0};
	char tmpstr[30];
	int i, len;

	if(pcie_port_num > 3) {
		printk("pcie port number fail %d channel\n", pcie_port_num);
		return -EINVAL;
	}

	memset(tmpstr, 0x00, sizeof(tmpstr));
	sprintf(tmpstr, "/soc/pcie-external%d", pcie_port_num);
	fdt_pcie_node = fdt_path_offset(fdt, tmpstr);
	if (!fdt_pcie_node) {
		pr_err("warning: device tree node '%s' has no address.\n", tmpstr);
		return -EINVAL;
	}

	pcie_prop = fdt_getprop(fdt, fdt_pcie_node, "reg", &len);
	if(!pcie_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", tmpstr, "reg");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			pcie_port_reg[i] = fdt32_to_cpu(pcie_prop[i]);
			//printk("pcie_port_reg[%d] = 0x%x\n", i, pcie_port_reg[i]);
		}

		data->reg = pcie_port_reg[1];
		data->size = pcie_port_reg[3];
	}

	return 0;
}

static int qnap_pcie_ep_pbs_parser(struct reg_value_data *data)
{
	void *fdt = initial_boot_params;
	int fdt_pbs_node;
	const __be32 *pbs_prop;
	int pbs_reg[4] = {0};
	int i, len;

	fdt_pbs_node = fdt_path_offset(fdt, "/soc/pbs");
	if (!fdt_pbs_node) {
		pr_err("warning: device tree node '%s' has no address.\n", "/soc/pbs");
		return -EINVAL;
	}

	pbs_prop = fdt_getprop(fdt, fdt_pbs_node, "reg", &len);
	if(!pbs_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", "/soc/pbs", "reg");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			pbs_reg[i] = fdt32_to_cpu(pbs_prop[i]);
			//printk("pbs_reg[%d] = 0x%x\n", i, pbs_reg[i]);
		}

		data->reg = pbs_reg[1];
		data->size = pbs_reg[3];
	}

	return 0;
}

static int qnap_pcie_ep_inbound_reserved_memory_parser(struct reg_value_data *data, unsigned char pcie_port_num)
{
	void *fdt = initial_boot_params;
	int fdt_res_mem_node;
	const __be32 *res_prop;
	int res_reg[4] = {0};
	char tmpstr[30];
	int i, len;

	if(pcie_port_num > 3) {
		printk("pcie port number fail %d channel\n", pcie_port_num);
		return -EINVAL;
	}

	memset(tmpstr, 0x00, sizeof(tmpstr));
	sprintf(tmpstr, "/reserved-memory/ep%d-inbound-area", pcie_port_num);
	fdt_res_mem_node = fdt_path_offset(fdt, tmpstr);
	if (!fdt_res_mem_node) {
		pr_err("warning: device tree node '%s' has no address.\n", tmpstr);
		return -EINVAL;
	}

	res_prop = fdt_getprop(fdt, fdt_res_mem_node, "reg", &len);
	if(!res_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", tmpstr, "reg");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			res_reg[i] = fdt32_to_cpu(res_prop[i]);
			//printk("res_reg[%d] = 0x%x\n", i, res_reg[i]);
		}

		data->reg = res_reg[1];
		data->size = res_reg[3];
	}

	return 0;
}

static int qnap_pcie_ep_port_reserved_memory_parser(struct reg_value_data *data, unsigned char pcie_port_num)
{
	void *fdt = initial_boot_params;
	int fdt_res_mem_node;
	const __be32 *res_prop;
	int res_reg[4] = {0};
	char tmpstr[30];
	int i, len;

	if(pcie_port_num > 3) {
		printk("pcie port number fail %d channel\n", pcie_port_num);
		return -EINVAL;
	}

	memset(tmpstr, 0x00, sizeof(tmpstr));
	sprintf(tmpstr, "/reserved-memory/ep%d-epconfig-area", pcie_port_num);
	fdt_res_mem_node = fdt_path_offset(fdt, tmpstr);
	if (!fdt_res_mem_node) {
		pr_err("warning: device tree node '%s' has no address.\n", tmpstr);
		return -EINVAL;
	}

	res_prop = fdt_getprop(fdt, fdt_res_mem_node, "reg", &len);
	if(!res_prop) {
		pr_err("warning: device tree node '%s' '%s' has no address.\n", tmpstr, "reg");
		return -EINVAL;
	} else {
		for(i=0;i<len/4;i++) {
			res_reg[i] = fdt32_to_cpu(res_prop[i]);
			//printk("res_reg[%d] = 0x%x\n", i, res_reg[i]);
		}

		data->reg = res_reg[1];
		data->size = res_reg[3];
	}

	return 0;
}

static int qnap_ep_config_outbound_memory_windows(struct ep_ports_list *ep_ports,
				struct outbound_config_params *outbound)
{
	if(ep_ports->port_counts == 1) {
		switch(ep_ports->port_id) {
		case 0:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM0;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 1:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM1;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 2:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM2;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 3:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM3;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		default:
			return -EINVAL;
			break;
		}
	} else if(ep_ports->port_counts == 2) {
		switch(ep_ports->port_id) {
		case 0:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM0;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 1:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM1;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 2:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM2;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 3:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM3;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		default:
			return -EINVAL;
			break;
		}
	} else if(ep_ports->port_counts == 3) {
		switch(ep_ports->port_id) {
		case 0:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM0;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 1:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM1;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
                        outbound->pasw_mem_size_log2 = 38;

			break;
		case 2:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM2;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 3:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM3;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		default:
			return -EINVAL;
			break;
		}
	} else if(ep_ports->port_counts == 4) {
		switch(ep_ports->port_id) {
		case 0:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM0;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 3) {
				outbound->pasw_mem_base = 1024ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 1:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM1;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 3) {
				outbound->pasw_mem_base = 1024ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 2:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM2;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 3) {
				outbound->pasw_mem_base = 1024ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		case 3:
			outbound->pasw_mem_map = AL_ADDR_MAP_PASW_PCIE_EXT_MEM3;
			if(pasw_mem_port_init_count == 0) {
				outbound->pasw_mem_base = 256ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 1) {
				outbound->pasw_mem_base = 512ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 2) {
				outbound->pasw_mem_base = 768ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else if(pasw_mem_port_init_count == 3) {
				outbound->pasw_mem_base = 1024ULL * 1024 * 1024 * 1024;
				pasw_mem_port_init_count++;
			} else {
				return -EINVAL;
			}
			outbound->pasw_mem_size_log2 = 38;
			break;
		default:
			return -EINVAL;
			break;
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static void qnap_ep_init_identity(struct al_pcie_ep_id_params *al_id, struct ep_id_params *ep_id)
{
#if defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	al_id->vendor_id_override = AL_TRUE;
	al_id->vendor_id = 0x1baa;
	al_id->device_id_override = AL_TRUE;
	al_id->device_id = 0x0056;
	al_id->class_code_override = AL_FALSE;
	al_id->class_code = 0x00;
	al_id->revision_id_override = AL_TRUE;
	al_id->revision_id = 0x00;
	al_id->subsystem_vendor_id_override = AL_TRUE;
	al_id->subsystem_vendor_id = 0x1baa;
	al_id->subsystem_device_id_override = AL_TRUE;
	al_id->subsystem_device_id = 0x0056;
#else
	if((ep_id->vendor_id & 0x0000ffff) != 0xffff) {
		al_id->vendor_id_override = AL_TRUE;
		al_id->vendor_id = ep_id->vendor_id;
	} else {
		al_id->vendor_id_override = AL_FALSE;
		al_id->vendor_id = 0x00;
	}

	if((ep_id->device_id & 0x0000ffff) != 0xffff) {
		al_id->device_id_override = AL_TRUE;
		al_id->device_id = ep_id->device_id;
	} else {
		al_id->device_id_override = AL_FALSE;
	}

	if((ep_id->revision_id & 0x0000ffff) != 0xffff) {
		al_id->revision_id_override = AL_TRUE;
		al_id->revision_id = ep_id->revision_id;
	} else {
		al_id->revision_id_override = AL_FALSE;
		al_id->revision_id = 0x00;
	}

	if((ep_id->subsystem_device_id & 0x0000ffff) != 0xffff) {
		al_id->subsystem_vendor_id_override = AL_TRUE;
		al_id->subsystem_vendor_id = ep_id->subsystem_vendor_id;
	} else {
		al_id->subsystem_vendor_id_override = AL_FALSE;
		al_id->subsystem_vendor_id = 0x00;
	}

	if((ep_id->subsystem_vendor_id & 0x0000ffff) != 0xffff) {
		al_id->subsystem_device_id_override = AL_TRUE;
		al_id->subsystem_device_id = ep_id->subsystem_device_id;
	} else {
		al_id->subsystem_device_id_override = AL_FALSE;
		al_id->subsystem_device_id = 0x00;
	}
#endif
#if 0
	printk("ep_id->vendor_id = 0x%x\n", ep_id->vendor_id);
	printk("ep_id->device_id = 0x%x\n", ep_id->device_id);
	printk("ep_id->revision_id = 0x%x\n", ep_id->revision_id);
	printk("ep_id->subsystem_vendor_id = 0x%x\n", ep_id->subsystem_vendor_id);
	printk("ep_id->subsystem_device_id = 0x%x\n", ep_id->subsystem_device_id);

	printk("al_id->vendor_id_override = 0x%x\n", al_id->vendor_id_override);
	printk("al_id->vendor_id = 0x%x\n", al_id->vendor_id);
	printk("al_id->device_id_override = 0x%x\n", al_id->device_id_override);
	printk("al_id->device_id = 0x%x\n", al_id->device_id);
	printk("al_id->revision_id_override = 0x%x\n", al_id->revision_id_override);
	printk("al_id->revision_id = 0x%x\n", al_id->revision_id);
	printk("al_id->subsystem_vendor_id_override = 0x%x\n", al_id->subsystem_vendor_id_override);
	printk("al_id->subsystem_vendor_id = 0x%x\n", al_id->subsystem_vendor_id);
	printk("al_id->subsystem_device_id_override = 0x%x\n", al_id->subsystem_device_id_override);
	printk("al_id->subsystem_device_id = 0x%x\n", al_id->subsystem_device_id);
#endif

	return;
}

static int qnap_ep_init_pcie_port(struct ep_ports_list *ep_ports)
{
	struct qnap_ep_device *qnap_ep = &ep_ports->qnap_ep;
	struct ep_id_params *qnap_ep_id = &ep_ports->qnap_ep.id_params;
	struct al_pcie_port *pcie_port_config = ep_ports->qnap_ep.pcie_port_config;
	struct al_pcie_pf pcie_pf;
	struct al_pcie_pf_config_params pf_config_params;
	struct al_pcie_ep_bar_params *bar0_params = &pf_config_params.bar_params[0];
	struct al_pcie_ep_bar_params *bar4_params = &pf_config_params.bar_params[4];
	struct al_pcie_atu_region atu_region_in_bar0 = { 0 };
	struct al_pcie_atu_region atu_region_in_bar4 = { 0 };
	struct al_pcie_atu_region atu_region_out = { 0 };
	struct al_pcie_msix_params msix_params = { 0 };
	struct al_pcie_ep_id_params id_params = { 0 };
	struct outbound_config_params outbound_params = { 0 };
	struct al_pcie_regs *regs;
	uint32_t mask = 0, rcs_retry = 0;
	int ret;

	qnap_ep->ep_port_id = ep_ports->port_id;
	qnap_ep->ep_uboot_init_flag = ep_ports->port_uboot_init_flag;
	pcie_port_config->port_id = qnap_ep->ep_port_id;

	printk("ep%d init -------------\n", qnap_ep->ep_port_id);
	//printk("qnap_ep->ep_uboot_init_flag = 0x%x\n", qnap_ep->ep_uboot_init_flag);
	if(qnap_ep->ep_uboot_init_flag == AL_TRUE) {
		printk("First U-boot init then the kernel reference u-boot init parser.\n");
		printk("ep%d port config addr virt = 0x%lx phy = 0x%lx\n", qnap_ep->ep_port_id, pcie_port_config, virt_to_phys(pcie_port_config));
		printk("ep%d IDentity init -------------\n", qnap_ep->ep_port_id);
		qnap_ep_init_identity(&id_params, qnap_ep_id);
		printk("PCIe %d IDentity : [%04x:%04x] Rev : 0x%04x, Subsystem IDentity : [%04x:%04x]\n",
										qnap_ep->ep_port_id,
										id_params.vendor_id, id_params.device_id,
										id_params.revision_id,
										id_params.subsystem_vendor_id, id_params.subsystem_device_id);
		printk("ep%d Inbound BAR0 init -------------\n", qnap_ep->ep_port_id);
		printk("Inbound bar0 target addr virt = 0x%lx phy = 0x%lx\n", (long unsigned int) qnap_ep->inbound_base_addr_bar0,
									      (long unsigned int) virt_to_phys(qnap_ep->inbound_base_addr_bar0));
		printk("ep%d Outbound init -------------\n", qnap_ep->ep_port_id);
		if(qnap_ep_config_outbound_memory_windows(ep_ports, &outbound_params) < 0) {
			printk("outbound memory windows config fail\n");
			pasw_mem_port_init_count = 0;
			return -EINVAL;
		}
		atu_region_out.base_addr = outbound_params.pasw_mem_base;
		qnap_ep->outbound_base_addr = (void *) atu_region_out.base_addr;
		printk("Outbound target addr virt = 0x%lx phy = 0x%lx\n", (long unsigned int) phys_to_virt(qnap_ep->outbound_base_addr),
									  (long unsigned int) qnap_ep->outbound_base_addr);
		qnap_ep->crs_flag = AL_FALSE;

		return 0;
	}

	printk("This is init kernel ep port function.\n");
	//printk("ep%d port addr virt = 0x%lx phy = 0x%lx\n", qnap_ep->ep_port_idpcie_port, virt_to_phys(pcie_port_config));
	pcie_port_config->max_lanes = 4;
	pcie_port_config->max_num_of_pfs = 1;
	//printk("qnap_ep->pbs_addr_base = 0x%lx, qnap_ep->ep_port_id = 0x%x\n", qnap_ep->pbs_addr_base, qnap_ep->ep_port_id);
	ret = al_pcie_port_handle_init(pcie_port_config, qnap_ep->pcie_port_addr_base, qnap_ep->pbs_addr_base, qnap_ep->ep_port_id);
	if(ret) {
		printk("PCIe %d: pcie core controller could not be initialized.\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

	/* Initialize PF0 */
	ret = al_pcie_pf_handle_init(&pcie_pf, pcie_port_config, 0);
	if(ret) {
		printk("PCIe %d: pf handle initialized fail.\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

	al_pcie_local_cfg_space_write(&pcie_pf, PCI_CLASS_REVISION >> 2,
						0x01080200,
						//0x11111111,
						AL_FALSE,
						AL_TRUE);
	memset(&pf_config_params, 0, sizeof(struct al_pcie_pf_config_params));

	printk("ep%d IDentity init -------------\n", qnap_ep->ep_port_id);
	qnap_ep_init_identity(&id_params, qnap_ep_id);
#if defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	id_params.device_id = id_params.device_id + qnap_ep->ep_port_id;
	id_params.revision_id = qnap_ep->ep_port_id + 1;
#endif
	printk("PCIe %d IDentity : [%04x:%04x] Rev : 0x%04x, Subsystem IDentity : [%04x:%04x]\n",
										qnap_ep->ep_port_id,
										id_params.vendor_id, id_params.device_id,
										id_params.revision_id,
										id_params.subsystem_vendor_id, id_params.subsystem_device_id);
	pf_config_params.bar_params_valid =  AL_TRUE;
	pf_config_params.cap_d1_d3hot_dis = AL_TRUE;
	pf_config_params.cap_flr_dis = AL_TRUE;
	pf_config_params.cap_aspm_dis = AL_TRUE;
	//pf_config_params.relaxed_pcie_ordering = AL_TRUE;
	pf_config_params.id_params = &id_params;
	pf_config_params.exp_bar_params.enable = AL_FALSE;

	bar0_params->enable = AL_TRUE;
	bar0_params->memory_64_bit = AL_TRUE;
	bar0_params->memory_is_prefetchable = AL_FALSE;
	bar0_params->memory_space = AL_TRUE;
	bar0_params->size = QNAP_MEM_SIZE;
#if 0
	bar4_params->enable = AL_TRUE;
	bar4_params->memory_64_bit = AL_TRUE;
	bar4_params->memory_is_prefetchable = AL_FALSE;
	bar4_params->memory_space = AL_TRUE;
	bar4_params->size = QNAP_MEM_SIZE;
#endif
	ret = al_pcie_pf_config(&pcie_pf, &pf_config_params);
	if(ret) {
		printk("PCIe %d: config EP fail!!\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

#if 0
	memset(qnap_ep->inbound_base_addr_bar0,0xa5,1);
	memset(qnap_ep->inbound_base_addr_bar0+1,0x5a,1);
	memset(qnap_ep->inbound_base_addr_bar0+2,0x55,1);
	memset(qnap_ep->inbound_base_addr_bar0+3,0xaa,1);
#if 0
	printk("qnap_ep->inbound_base_addr_bar0[0] = 0x%x\n", (unsigned char) *(qnap_ep.inbound_base_addr_bar0));
	printk("qnap_ep->inbound_base_addr_bar0[1] = 0x%x\n", (unsigned char) *(qnap_ep.inbound_base_addr_bar0+1));
	printk("qnap_ep->inbound_base_addr_bar0[2] = 0x%x\n", (unsigned char) *(qnap_ep.inbound_base_addr_bar0+2));
	printk("qnap_ep->inbound_base_addr_bar0[3] = 0x%x\n", (unsigned char) *(qnap_ep.inbound_base_addr_bar0+3));
#endif
#endif

#if 0
	memset(qnap_ep->inbound_base_addr_bar4,0xa5,1);
	memset(qnap_ep->inbound_base_addr_bar4+1,0x5a,1);
	memset(qnap_ep->inbound_base_addr_bar4+2,0x55,1);
	memset(qnap_ep->inbound_base_addr_bar4+3,0xaa,1);
#if 0
	printk("qnap_ep->inbound_base_addr_bar4[0] = 0x%x\n", (unsigned char) *(qnap_ep->inbound_base_addr_bar4));
	printk("qnap_ep->inbound_base_addr_bar4[1] = 0x%x\n", (unsigned char) *(qnap_ep->inbound_base_addr_bar4+1));
	printk("qnap_ep->inbound_base_addr_bar4[2] = 0x%x\n", (unsigned char) *(qnap_ep->inbound_base_addr_bar4+2));
	printk("qnap_ep->inbound_base_addr_bar4[3] = 0x%x\n", (unsigned char) *(qnap_ep->inbound_base_addr_bar4+3));
#endif
#else
	memset(qnap_ep->inbound_base_addr_bar4, 0xff, QNAP_MEM_SIZE);
#endif

	printk("ep%d Inbound BAR0 init -------------\n", qnap_ep->ep_port_id);
	atu_region_in_bar0.index = 0;
	atu_region_in_bar0.bar_number = PCIE_EP_BAR0;
	atu_region_in_bar0.response = AL_PCIE_RESPONSE_NORMAL;
	atu_region_in_bar0.enable = AL_TRUE;
	atu_region_in_bar0.direction = AL_PCIE_ATU_DIR_INBOUND;
	atu_region_in_bar0.invert_matching = AL_FALSE;
	atu_region_in_bar0.tlp_type = AL_PCIE_TLP_TYPE_MEM;
	atu_region_in_bar0.match_mode = 1; /* BAR match mode */
	atu_region_in_bar0.attr = 0; /* not used */
	atu_region_in_bar0.msg_code = 0; /* not used */
	atu_region_in_bar0.cfg_shift_mode = AL_FALSE;
	atu_region_in_bar0.target_addr = virt_to_phys(qnap_ep->inbound_base_addr_bar0); /* PHYSICAL ADDRESS OF BUFFER */;
	printk("Inbound BAR0 target addr vire = 0x%lx phy = 0x%lx\n", (long unsigned int) qnap_ep->inbound_base_addr_bar0,
								      (long unsigned int) atu_region_in_bar0.target_addr);

	atu_region_in_bar0.enable_attr_match_mode = AL_FALSE;
	atu_region_in_bar0.enable_msg_match_mode = AL_FALSE;

	ret = al_pcie_atu_region_set(pcie_port_config, &atu_region_in_bar0);
	if(ret) {
		printk("PCIe %d: inbound BAR0 setting OB iATU after link is started is not allowed\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

#if 0
	printk("ep%d Inbound BAR4 init -------------\n", qnap_ep->ep_port_id);
	atu_region_in_bar4.index = 0;
	atu_region_in_bar4.bar_number = PCIE_EP_BAR4;
	atu_region_in_bar4.response = AL_PCIE_RESPONSE_NORMAL;
	atu_region_in_bar4.enable = AL_TRUE;
	atu_region_in_bar4.direction = AL_PCIE_ATU_DIR_INBOUND;
	atu_region_in_bar4.invert_matching = AL_FALSE;
	atu_region_in_bar4.tlp_type = AL_PCIE_TLP_TYPE_MEM;
	atu_region_in_bar4.match_mode = 1; /* BAR match mode */
	atu_region_in_bar4.attr = 0; /* not used */
	atu_region_in_bar4.msg_code = 0; /* not used */
	atu_region_in_bar4.cfg_shift_mode = AL_FALSE;
	atu_region_in_bar4.target_addr = virt_to_phys(qnap_ep->inbound_base_addr_bar4); /* PHYSICAL ADDRESS OF BUFFER */;
	printk("Inbound BAR4 target addr virt = 0x%lx phy = 0x%lx\n", (long unsigned int) qnap_ep->inbound_base_addr_bar4,
								      (long unsigned int) atu_region_in_bar4.target_addr);

	atu_region_in_bar4.enable_attr_match_mode = AL_FALSE;
	atu_region_in_bar4.enable_msg_match_mode = AL_FALSE;

	ret = al_pcie_atu_region_set(pcie_port_config, &atu_region_in_bar4);
	if(ret) {
		printk("PCIe %d: inbound BAR4 setting OB iATU after link is started is not allowed\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}
#endif

	printk("ep%d Outbound init -------------\n", qnap_ep->ep_port_id);
	if(qnap_ep_config_outbound_memory_windows(ep_ports, &outbound_params) < 0) {
		printk("outbound memory windows config fail\n");
		pasw_mem_port_init_count = 0;
		return -EINVAL;
	}

	ret = al_addr_map_pasw_set(qnap_ep->pbs_addr_base,
			outbound_params.pasw_mem_map,
			outbound_params.pasw_mem_base,
			outbound_params.pasw_mem_size_log2);
	if(ret) {
		printk("PCIe %d: addr_map: pasw base has to be aligned to size\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

	atu_region_out.index = 0;
	atu_region_out.bar_number = PCIE_EP_BAR0;
	atu_region_out.response = AL_PCIE_RESPONSE_NORMAL;
	atu_region_out.enable = AL_TRUE;
	atu_region_out.direction = AL_PCIE_ATU_DIR_OUTBOUND;
	atu_region_out.invert_matching = AL_FALSE;
	atu_region_out.tlp_type = AL_PCIE_TLP_TYPE_MEM;
	atu_region_out.match_mode = 0;		/* address match mode */
	atu_region_out.attr = 0;		/* not used */
	atu_region_out.msg_code = 0;		/* not used */
	atu_region_out.cfg_shift_mode = AL_FALSE;
	atu_region_out.base_addr = outbound_params.pasw_mem_base;
	atu_region_out.limit = atu_region_out.base_addr + (1ULL << outbound_params.pasw_mem_size_log2) - 1;
	atu_region_out.target_addr = 0;
	atu_region_out.enable_attr_match_mode = AL_FALSE;
	atu_region_out.enable_msg_match_mode = AL_FALSE;
	/* Allow OB ATU creation (despite link is up) - no traffic expected */
	atu_region_out.enforce_ob_atu_region_set = AL_TRUE;

	/* Allow read/write host addresses from all PFs */
	atu_region_out.function_match_bypass_mode = AL_FALSE;
	atu_region_out.function_match_bypass_mode_number = 0;

	ret = al_pcie_atu_region_set(pcie_port_config, &atu_region_out);
	if(ret) {
		printk("PCIe %d: outbound setting OB iATU after link is started is not allowed\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}
	qnap_ep->outbound_base_addr = (void *) atu_region_out.base_addr;
	printk("Outbound target addr virt = 0x%lx phy = 0x%lx\n", (long unsigned int) phys_to_virt(qnap_ep->outbound_base_addr),
								  (long unsigned int) qnap_ep->outbound_base_addr);

	printk("ep%d msix init -------------\n", qnap_ep->ep_port_id);
	msix_params.enabled = AL_TRUE;
	msix_params.table_size = QNAP_MSIX_TABLE_SIZE;
	msix_params.table_offset = MSIX_TABLE_OFFSET;
	msix_params.table_bar = PCIE_EP_BAR0;
	msix_params.pba_offset = MSIX_PBA_OFFSET;
	msix_params.pba_bar = PCIE_EP_BAR0;
	ret = al_pcie_msix_config(&pcie_pf, &msix_params);
	if(ret) {
		 printk("PCIe %d: msix config fail.\n", qnap_ep->ep_port_id);
		return -ENOSYS;
	}

	// default CRS is true in u-boot
	regs = pcie_port_config->regs;
	mask = (pcie_port_config->rev_id == AL_PCIE_REV_ID_3) ?
				PCIE_W_REV3_GLOBAL_CTRL_PM_CONTROL_APP_REQ_RETRY_EN :
				PCIE_W_REV1_2_GLOBAL_CTRL_PM_CONTROL_APP_REQ_RETRY_EN;

	rcs_retry = al_reg_read32(regs->app.global_ctrl.pm_control);
	if(rcs_retry & mask) {
		qnap_ep->crs_flag = AL_TRUE;
	} else {
		qnap_ep->crs_flag = AL_FALSE;
	}

	return 0;
}

void qnap_ep_link_list_fun(void)
{
	printk("traversing the list using list_for_each_entry()\n");
	list_for_each_entry(g_dev_qnap_ep->tmp_ep_ports_list, &g_dev_qnap_ep->ep_ports_mylist.list, list) {
		printk("port_id = %d\n", g_dev_qnap_ep->tmp_ep_ports_list->port_id);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.ep_uboot_init_flag = 0x%x\n", g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.ep_uboot_init_flag);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.crs_flag = 0x%x\n", g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.crs_flag);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pbs_addr_base = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pbs_addr_base);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_addr_base = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_addr_base);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0 = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0);
                printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2 = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4 = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4);
		printk("g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.outbound_base_addr = 0x%lx\n",
									(long unsigned int) g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.outbound_base_addr);
	}

	return;
}
EXPORT_SYMBOL(qnap_ep_link_list_fun);

int qnap_ep_get_port_count_fun(void)
{
	int count = 0;

	list_for_each_entry(g_dev_qnap_ep->tmp_ep_ports_list, &g_dev_qnap_ep->ep_ports_mylist.list, list) {
		++count;
	}

	if(count <= 0)
		count = -1;

	return count;
}
EXPORT_SYMBOL(qnap_ep_get_port_count_fun);

int qnap_ep_pcie_port_list_parser_fun(unsigned char port_num, struct qnap_ep_device *ep)
{

	list_for_each_entry(g_dev_qnap_ep->tmp_ep_ports_list, &g_dev_qnap_ep->ep_ports_mylist.list, list) {
		if(g_dev_qnap_ep->tmp_ep_ports_list->port_id == port_num) {
			ep->ep_port_id = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.ep_port_id;
			ep->crs_flag = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.crs_flag;
			ep->ep_uboot_init_flag = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.ep_uboot_init_flag;
			ep->pbs_addr_base = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pbs_addr_base;
			ep->pcie_port_addr_base = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_addr_base;
			ep->inbound_base_addr_bar0 = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0;
			ep->inbound_base_addr_bar2 = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2;
			ep->inbound_base_addr_bar4 = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4;
			ep->outbound_base_addr = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.outbound_base_addr;
			ep->pcie_port_config = g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config;
		}
	}

	return 0;
}
EXPORT_SYMBOL(qnap_ep_pcie_port_list_parser_fun);

void qnap_ep_rcs_fun(unsigned char port_num, unsigned crsonoff)
{
	list_for_each_entry(g_dev_qnap_ep->tmp_ep_ports_list, &g_dev_qnap_ep->ep_ports_mylist.list, list) {
		if(g_dev_qnap_ep->tmp_ep_ports_list->port_id == port_num) {
			if(g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag == AL_TRUE) {
				return;
			}

			if(crsonoff == 1) {
				al_pcie_app_req_retry_set(g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config, AL_TRUE);
				g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.crs_flag = AL_TRUE;
			} else if(crsonoff == 0) {
				al_pcie_app_req_retry_set(g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config, AL_FALSE);
				g_dev_qnap_ep->tmp_ep_ports_list->qnap_ep.crs_flag = AL_FALSE;
			} else {
				printk("CRS port id %d, crs flag 0x%x\n", g_dev_qnap_ep->tmp_ep_ports_list->port_id, crsonoff);
			}
		}
	}

	return;
}
EXPORT_SYMBOL(qnap_ep_rcs_fun);

int qnap_ep_uboot_init_fun(unsigned char port_num)
{
	list_for_each_entry(g_dev_qnap_ep->tmp_ep_ports_list, &g_dev_qnap_ep->ep_ports_mylist.list, list) {
		if(g_dev_qnap_ep->tmp_ep_ports_list->port_id == port_num) {
			if(g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag == AL_TRUE) {
				//printk("u-boot init %s (%d)\n", (g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag == AL_TRUE) ? "True" : "False",
				//				g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag);
				return AL_TRUE;
			} else {
				//printk("u-boot init %s (%d)\n", (g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag == AL_TRUE) ? "True" : "False",
				//				g_dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag);
				return AL_FALSE;
			}
		}
	}
}
EXPORT_SYMBOL(qnap_ep_uboot_init_fun);

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
int qnap_ep_btn_status(unsigned char onoff)
{
	unsigned char val;

	if(onoff == 'r') {
		printk("BTN Status Value = 0x%x, %s\n", power_button_status, (power_button_status == 1) ? "Turn-On" : "Turn-Off");
		return 0;
	}

	if(onoff == 0x01)
		power_button_status = 1;
	else if(onoff == 0x00)
		power_button_status = 0;
	else
		return -1;

	return 0;
}

void qnap_ep_cpld_power_status_update(unsigned char data)
{
	cpld_power_status_reg_params now_cpld_power_status_reg;

	g_cpld_power_status_reg.data = data;
	now_cpld_power_status_reg.data = data;

	return;
}
EXPORT_SYMBOL(qnap_ep_cpld_power_status_update);

void qnap_ep_cable_vbus_status_update(unsigned char data)
{
	cable_vbus_status_reg_params now_cable_vbus_status_reg;

	g_cable_vbus_status_reg.data = data;
	now_cable_vbus_status_reg.data = data;

	return;
}
EXPORT_SYMBOL(qnap_ep_cable_vbus_status_update);

void qnap_ep_cable_status_update(unsigned char data)
{
	cable_status_reg_params now_cable_status_reg;

	g_cable_status_reg.data = data;
	now_cable_status_reg.data = data;

#if 0
	printk("now_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pb_tbt);
	printk("now_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pb_usb);
	printk("now_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pa_tbt);
	printk("now_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pa_usb);
	printk("now_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pb_tbt);
	printk("now_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pb_usb);
	printk("now_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pa_tbt);
	printk("now_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pa_usb);

	printk("g_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pa_vbus);
	printk("g_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pb_vbus);
	printk("g_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pa_vbus);
	printk("g_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif

#if 0
	printk("EP1 TBT2\n");
	printk("g_cable_status_reg.data = 0x%x\n", g_cable_status_reg.data);
	printk("now_cable_status_reg.data = 0x%x\n", now_cable_status_reg.data);
	printk("g_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pa_vbus);
	printk("g_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pb_vbus);
	printk("g_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pa_vbus);
	printk("g_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif
	// EP1 (TBT1)
	// EP1 TBT1 PA
	if(g_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
		if(now_cable_status_reg.status.tbt1_pb_tbt == CPLD_TYPE_C_TBT_REMOVED) {
			if(now_cable_status_reg.status.tbt1_pa_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt1_usb_insert_flag == CPLD_TBT1_USB_MODE_NULL) {
					tbt1_usb_insert_flag = CPLD_TBT1_PA_USB_MODE;
					g_i_worked = 1;
					printk("tbt1 pa usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt1_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt1_tbt_insert_flag == CPLD_TBT1_TBT_MODE_NULL) {
					tbt1_tbt_insert_flag = CPLD_TBT1_PA_TBT_MODE;
					g_i_worked = 1;
					printk("tbt1 pa tbt cable default insert..\n");
				}
			}
		} else {
			if(now_cable_status_reg.status.tbt1_pa_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt1_usb_insert_flag == CPLD_TBT1_USB_MODE_NULL) {
					tbt1_usb_insert_flag = CPLD_TBT1_PA_USB_MODE;
					g_i_worked = 1;
					printk("tbt1 pa usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt1_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt1_usb_insert_flag == CPLD_TBT1_TBT_MODE_NULL) {
					tbt1_tbt_insert_flag = CPLD_TBT1_PA_TBT_MODE;
					g_i_worked = 1;
					printk("tbt1 pa tbt cable default insert..\n");
				}
			}
		}
	}

	// EP1 TBT1 PB
	if(g_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
		if(now_cable_status_reg.status.tbt1_pa_tbt == CPLD_TYPE_C_TBT_REMOVED) {
			if(now_cable_status_reg.status.tbt1_pb_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt1_usb_insert_flag == CPLD_TBT1_USB_MODE_NULL) {
					tbt1_usb_insert_flag = CPLD_TBT1_PB_USB_MODE;
					g_i_worked = 1;
					printk("tbt1 pb usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt1_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt1_tbt_insert_flag == CPLD_TBT1_TBT_MODE_NULL) {
					tbt1_tbt_insert_flag = CPLD_TBT1_PB_TBT_MODE;
					g_i_worked = 1;
					printk("tbt1 pb tbt cable default insert..\n");
				}
			}
		} else {
			if(now_cable_status_reg.status.tbt1_pb_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt1_usb_insert_flag == CPLD_TBT1_USB_MODE_NULL) {
					tbt1_usb_insert_flag = CPLD_TBT1_PB_USB_MODE;
					g_i_worked = 1;
					printk("tbt1 pb usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt1_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt1_tbt_insert_flag == CPLD_TBT1_TBT_MODE_NULL) {
					tbt1_tbt_insert_flag = CPLD_TBT1_PB_TBT_MODE;
					g_i_worked = 1;
					printk("tbt1 pb tbt cable default insert..\n");
				}
			}
		}
	}

	// EP0 TBT2 PA
//	printk("EP0 TBT2\n");
	if(g_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
		if(now_cable_status_reg.status.tbt2_pb_tbt == CPLD_TYPE_C_TBT_REMOVED) {
			if(now_cable_status_reg.status.tbt2_pa_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt2_usb_insert_flag == CPLD_TBT2_USB_MODE_NULL) {
					tbt2_usb_insert_flag = CPLD_TBT2_PA_USB_MODE;
					g_i_worked = 1;
					printk("tbt2 pa usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt2_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt2_tbt_insert_flag == CPLD_TBT2_TBT_MODE_NULL) {
					tbt2_tbt_insert_flag = CPLD_TBT2_PA_TBT_MODE;
					g_i_worked = 1;
					printk("tbt2 pa tbt cable default insert..\n");
				}
			}
		} else {
			if(now_cable_status_reg.status.tbt2_pa_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt2_usb_insert_flag == CPLD_TBT2_USB_MODE_NULL) {
					tbt2_usb_insert_flag = CPLD_TBT2_PA_USB_MODE;
					g_i_worked = 1;
					printk("tbt2 pa usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt2_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt2_usb_insert_flag == CPLD_TBT2_TBT_MODE_NULL) {
					tbt2_tbt_insert_flag = CPLD_TBT2_PA_TBT_MODE;
					g_i_worked = 1;
					printk("tbt2 pa tbt cable default insert..\n");
				}
			}
		}
	}

	// EP0 TBT2 PB
	if(g_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
		if(now_cable_status_reg.status.tbt2_pa_tbt == CPLD_TYPE_C_TBT_REMOVED) {
			if(now_cable_status_reg.status.tbt2_pb_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt2_usb_insert_flag == CPLD_TBT2_USB_MODE_NULL) {
					tbt2_usb_insert_flag = CPLD_TBT2_PB_USB_MODE;
					g_i_worked = 1;
					printk("tbt2 pb usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt2_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt2_tbt_insert_flag == CPLD_TBT2_TBT_MODE_NULL) {
					tbt2_tbt_insert_flag = CPLD_TBT2_PB_TBT_MODE;
					g_i_worked = 1;
					printk("tbt2 pb tbt cable default insert..\n");
				}
			}
		} else {
			if(now_cable_status_reg.status.tbt2_pb_usb == CPLD_TYPE_C_USB_INSERT) {
				if(tbt2_usb_insert_flag == CPLD_TBT2_USB_MODE_NULL) {
					tbt2_usb_insert_flag = CPLD_TBT2_PB_USB_MODE;
					g_i_worked = 1;
					printk("tbt2 pb usb cable default insert..\n");
				}
			} else if(now_cable_status_reg.status.tbt2_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
				if(tbt2_tbt_insert_flag == CPLD_TBT2_TBT_MODE_NULL) {
					tbt2_tbt_insert_flag = CPLD_TBT2_PB_TBT_MODE;
					g_i_worked = 1;
					printk("tbt2 pb tbt cable default insert..\n");
				}
			}
		}
	}

	return;
}
EXPORT_SYMBOL(qnap_ep_cable_status_update);
#endif

void qnap_ep_ready_set_high(unsigned char port_num)
{
	struct qnap_ep_data *ep = g_dev_qnap_ep;
	int gpio_num = 0;

	gpio_num = ep->ep_info_data.gpio_sw_ready_num[port_num];

	printk("qnap_ep_ready_set_high gpio_num = %d, 0x%x\n", gpio_num, gpio_num);
	gpio_set_value(gpio_num, 1);

	return;
}EXPORT_SYMBOL(qnap_ep_ready_set_high);

void qnap_ep_ready_set_low(unsigned char port_num)
{
	struct qnap_ep_data *ep = g_dev_qnap_ep;
	int gpio_num = 0;

	gpio_num = ep->ep_info_data.gpio_sw_ready_num[port_num];

	printk("qnap_ep_ready_set_low gpio_num = %d, 0x%x\n", gpio_num, gpio_num);
	gpio_set_value(gpio_num, 0);

	return;
}EXPORT_SYMBOL(qnap_ep_ready_set_low);

void qnap_ep_asm_vbus_ctl_set_high(unsigned char port_num)
{
	unsigned char asm_vbus_ctl_val = 0;
	int ret = -1;

	ret = qnap_cpld_read(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
	if(ret < 0)
		printk("read cpld fail\n");

	if(port_num == 0) {
		asm_vbus_ctl_val = asm_vbus_ctl_val | 0x02;  // EP0
	} else if(port_num == 1) {
		asm_vbus_ctl_val = asm_vbus_ctl_val | 0x01;  // EP1
	} else {
		printk("value fail !!, port_num = 0x%x\n", port_num);
	}

	printk("qnap_ep_asm_vbus_ctl_set_high port_num = %d, 0x%x, asm_vbus_ctl_val = 0x%x\n", port_num, port_num, asm_vbus_ctl_val);
	ret = qnap_cpld_write(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
	if(ret < 0)
		printk("write cpld fail\n");

	return;
}EXPORT_SYMBOL(qnap_ep_asm_vbus_ctl_set_high);

void qnap_ep_asm_vbus_ctl_set_low(unsigned char port_num)
{
	unsigned char asm_vbus_ctl_val = 0;
	int ret = -1;

	ret = qnap_cpld_read(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
	if(ret < 0)
		printk("read cpld fail\n");

	if(port_num == 0) {
		asm_vbus_ctl_val = asm_vbus_ctl_val & ~0x02; // EP0
	} else if(port_num == 1) {
		asm_vbus_ctl_val = asm_vbus_ctl_val & ~0x01; // EP1
	} else {
		printk("value fail !!, port_num = 0x%x\n", port_num);
	}

	printk("qnap_ep_asm_vbus_ctl_set_low port_num = %d, 0x%x, asm_vbus_ctl_val = 0x%x\n", port_num, port_num, asm_vbus_ctl_val);
	ret = qnap_cpld_write(CPLD_ASM_VBUS_CONTROL, &asm_vbus_ctl_val, 1);
	if(ret < 0)
		printk("write cpld fail\n");

	return;
}EXPORT_SYMBOL(qnap_ep_asm_vbus_ctl_set_low);

void qnap_ep_usb_mux_set_high(void)
{
	struct qnap_ep_data *ep = g_dev_qnap_ep;
	int gpio_num = 0;

	gpio_num = ep->ep_info_data.gpio_usb_mux_num;

	printk("qnap_ep_usb_mux_set_high gpio_num = %d, 0x%x\n", gpio_num, gpio_num);
	gpio_set_value(gpio_num, 1);

	return;
}EXPORT_SYMBOL(qnap_ep_usb_mux_set_high);

void qnap_ep_usb_mux_set_low(void)
{
	struct qnap_ep_data *ep = g_dev_qnap_ep;
	int gpio_num = 0;

	gpio_num = ep->ep_info_data.gpio_usb_mux_num;

	printk("qnap_ep_usb_mux_set_low gpio_num = %d, 0x%x\n", gpio_num, gpio_num);
	gpio_set_value(gpio_num, 0);

	return;
}EXPORT_SYMBOL(qnap_ep_usb_mux_set_low);

int qnap_ep_register(struct qnap_ep_fabrics_ops *ops)
{
	int ret = 0;

	down_write(&qnap_ep_config_sem);
	if (qnap_ep_ops[ops->type])
		ret = -EINVAL;
	else
		qnap_ep_ops[ops->type] = ops;
	up_write(&qnap_ep_config_sem);

	return ret;
}
EXPORT_SYMBOL_GPL(qnap_ep_register);

void qnap_ep_unregister(struct qnap_ep_fabrics_ops *ops)
{
	down_write(&qnap_ep_config_sem);
	qnap_ep_ops[ops->type] = NULL;
	up_write(&qnap_ep_config_sem);
}
EXPORT_SYMBOL_GPL(qnap_ep_unregister);

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
static int qnap_ep_intx_thread_0(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
//	struct ep_port_info_data *ep_port_info = &dev_qnap_ep->ep_info_data;
	unsigned char p_data = 0, v_data = 0, c_data = 0, p_data_tmp = 0, v_data_tmp = 0, c_data_tmp = 0;
	struct qnap_ep_event event;
	struct cable_hal_event_status ches;

	unsigned char tbt1_s3_status_retry = 0, tbt2_s3_status_retry = 0;
	cpld_power_status_reg_params now_cpld_power_status_reg;
	cable_vbus_status_reg_params now_cable_vbus_status_reg;
	cable_status_reg_params now_cable_status_reg;
	int cpld_ready_delay = 0;

	while(1) {
		msleep(500);
		if(cpld_i2c_ready) {
			if(cpld_ready_delay < 10) {
				cpld_ready_delay++;
				msleep(100);
				printk("cpld i2c ready (%d)\n", cpld_ready_delay);
				continue;
			}
			msleep(500);
			if(qnap_cpld_read(CPLD_TBT_HOSTPWR_PWR_BUTT, &p_data_tmp, 1) < 0) {
				printk("Getting CPLD Power Status Register Fail !!\n");
			} else {
				if(p_data_tmp != p_data) {
					p_data = p_data_tmp;
					now_cpld_power_status_reg.data = p_data;
#if 1
					printk("now_cpld_power_status_reg.data = 0x%x\n", now_cpld_power_status_reg.data);
					printk("now_cpld_power_status_reg.status.tbt1_s3_en = 0x%x\n", now_cpld_power_status_reg.status.tbt1_s3_en);
					printk("now_cpld_power_status_reg.status.tbt1_s0_en = 0x%x\n", now_cpld_power_status_reg.status.tbt1_s0_en);
					printk("now_cpld_power_status_reg.status.tbt2_s3_en = 0x%x\n", now_cpld_power_status_reg.status.tbt2_s3_en);
					printk("now_cpld_power_status_reg.status.tbt2_s0_en = 0x%x\n", now_cpld_power_status_reg.status.tbt2_s0_en);
					printk("now_cpld_power_status_reg.status.reserved1 = 0x%x\n", now_cpld_power_status_reg.status.reserved1);
					printk("now_cpld_power_status_reg.status.reserved2 = 0x%x\n", now_cpld_power_status_reg.status.reserved2);
					printk("now_cpld_power_status_reg.status.reserved3 = 0x%x\n", now_cpld_power_status_reg.status.reserved3);
					printk("now_cpld_power_status_reg.status.reserved4 = 0x%x\n", now_cpld_power_status_reg.status.reserved4);
#endif
RETRY1:
					if((now_cpld_power_status_reg.status.tbt1_s3_en == 0) && (now_cpld_power_status_reg.status.tbt1_s0_en == 0)) {
						printk("TBT1 to S5 mode\n");
						tbt1_power_status = TR_HOST_S5;
					} else if((now_cpld_power_status_reg.status.tbt1_s3_en == 1) && (now_cpld_power_status_reg.status.tbt1_s0_en == 0)) {
						if(tbt1_s3_status_retry > 2) {
							printk("TBT1 to S3 mode\n");
							tbt1_s3_status_retry = 0;
							tbt1_power_status = TR_HOST_S3;
						} else {
							printk("TBT1 S3 retry.....\n");
							tbt1_s3_status_retry++;
							msleep(500);
							if(qnap_cpld_read(CPLD_TBT_HOSTPWR_PWR_BUTT, &p_data_tmp, 1) < 0) {
								printk("Getting CPLD Power Status Register Fail !!\n");
							}
							p_data = p_data_tmp;
							now_cpld_power_status_reg.data = p_data;
							goto RETRY1;
						}
					} else if((now_cpld_power_status_reg.status.tbt1_s3_en == 1) && (now_cpld_power_status_reg.status.tbt1_s0_en == 1)) {
						printk("TBT1 to S0 mode\n");
						tbt1_power_status = TR_HOST_S0;
					}
RETRY2:
					if((now_cpld_power_status_reg.status.tbt2_s3_en == 0) && (now_cpld_power_status_reg.status.tbt2_s0_en == 0)) {
						printk("TBT2 to S5 mode\n");
						tbt2_power_status = TR_HOST_S5;
					} else if((now_cpld_power_status_reg.status.tbt2_s3_en == 1) && (now_cpld_power_status_reg.status.tbt2_s0_en == 0)) {
						if(tbt2_s3_status_retry > 2) {
							printk("TBT2 to S3 mode\n");
							tbt2_power_status = TR_HOST_S3;
						} else {
							printk("TBT2 S3 retry.....\n");
							tbt2_s3_status_retry++;
							msleep(500);
							if(qnap_cpld_read(CPLD_TBT_HOSTPWR_PWR_BUTT, &p_data_tmp, 1) < 0) {
								printk("Getting CPLD Power Status Register Fail !!\n");
							}
							p_data = p_data_tmp;
							now_cpld_power_status_reg.data = p_data;
							goto RETRY2;
						}
					} else if((now_cpld_power_status_reg.status.tbt2_s3_en == 1) && (now_cpld_power_status_reg.status.tbt2_s0_en == 1)) {
						printk("TBT2 to S0 mode\n");
						tbt2_power_status = TR_HOST_S0;
					}
					g_i_worked = 1;
					g_cpld_power_status_reg.data = p_data;
				}
			}

			if(g_i_worked == 1) {
				msleep(500);
				if(qnap_cpld_read(CPLD_TYPE_C_VBUS_STATUS, &v_data_tmp, 1) < 0) {
					printk("Getting Type-C Vbus Status Register Fail !!\n");
				} else {
					if(v_data != v_data_tmp) {
						v_data = v_data_tmp;
						printk("v_data_tmp = 0x%x\n", v_data_tmp);
						now_cable_vbus_status_reg.data = v_data;
#if 1
						printk("a1 now_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pa_vbus);
						printk("a1 now_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pb_vbus);
						printk("a1 now_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pa_vbus);
						printk("a1 now_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif
						msleep(500);
						if(qnap_cpld_read(CPLD_TYPE_C_CON_STATUS, &c_data_tmp, 1) < 0) {
							printk("Getting Type C Status from CPLD Cable Register Fail !!\n");
						} else {
							c_data = c_data_tmp;
							now_cable_status_reg.data = c_data;
#if 1
							printk("a g_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pb_tbt);
							printk("a g_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pb_usb);
							printk("a g_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt1_pa_tbt);
							printk("a g_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt1_pa_usb);
							printk("a g_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pb_tbt);
							printk("a g_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pb_usb);
							printk("a g_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", g_cable_status_reg.status.tbt2_pa_tbt);
							printk("a g_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", g_cable_status_reg.status.tbt2_pa_usb);
							printk("a now_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pb_tbt);
							printk("a now_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pb_usb);
							printk("a now_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pa_tbt);
							printk("a now_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pa_usb);
							printk("a now_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pb_tbt);
							printk("a now_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pb_usb);
							printk("a now_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pa_tbt);
							printk("a now_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pa_usb);
							printk("a g_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pa_vbus);
							printk("a g_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt1_pb_vbus);
							printk("a g_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pa_vbus);
							printk("a g_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", g_cable_vbus_status_reg.status.tbt2_pb_vbus);
							printk("a now_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pa_vbus);
							printk("a now_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pb_vbus);
							printk("a now_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pa_vbus);
							printk("a now_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif
							if(g_cable_vbus_status_reg.data != now_cable_vbus_status_reg.data) {
#if 1
								printk("aa qq1 g_cable_status_reg.data = 0x%x\n", g_cable_status_reg.data);
								printk("aa qq2 now_cable_status_reg.data = 0x%x\n", now_cable_status_reg.data);
#endif
								if(g_cable_status_reg.data != now_cable_status_reg.data) {
#if 1
									printk("bb qq1 g_cable_status_reg.data = 0x%x\n", g_cable_status_reg.data);
									printk("bb qq2 now_cable_status_reg.data = 0x%x\n", now_cable_status_reg.data);
									printk("b now_cable_status_reg.status.tbt1_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pb_tbt);
									printk("b now_cable_status_reg.status.tbt1_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pb_usb);
									printk("b now_cable_status_reg.status.tbt1_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt1_pa_tbt);
									printk("b now_cable_status_reg.status.tbt1_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt1_pa_usb);
									printk("b now_cable_status_reg.status.tbt2_pb_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pb_tbt);
									printk("b now_cable_status_reg.status.tbt2_pb_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pb_usb);
									printk("b now_cable_status_reg.status.tbt2_pa_tbt = 0x%x\n", now_cable_status_reg.status.tbt2_pa_tbt);
									printk("b now_cable_status_reg.status.tbt2_pa_usb = 0x%x\n", now_cable_status_reg.status.tbt2_pa_usb);
									printk("b now_type_c_vbus_status.status.tbt1_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pa_vbus);
									printk("b now_type_c_vbus_status.status.tbt1_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt1_pb_vbus);
									printk("b now_type_c_vbus_status.status.tbt2_pa_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pa_vbus);
									printk("b now_type_c_vbus_status.status.tbt2_pb_vbus = 0x%x\n", now_cable_vbus_status_reg.status.tbt2_pb_vbus);
#endif
									// EP1 (TBT1)
									// EP1 TBT1 PB TBT
RETRY3:
									msleep(300);
									if(qnap_cpld_read(CPLD_TYPE_C_CON_STATUS, &c_data_tmp, 1) < 0) {
										printk("Getting Type C Status from CPLD Cable Register Fail !!\n");
									}
									c_data = c_data_tmp;
									now_cable_status_reg.data = c_data;

									if(now_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt1_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
											if(tbt1_usb_insert_flag == CPLD_TBT1_USB_MODE_NULL) {
												tbt1_tbt_insert_flag = CPLD_TBT1_PB_TBT_MODE;
												ches.modules = CPLD_M2_1;
												ches.port = CPLD_PORTB;
												ches.type = CPLD_TYPE_TBT;
												ches.cable_status = CPLD_TYPE_C_TBT_INSERT;
												qnap_send_ep_cable_hal_event(&ches);
												printk("tbt1 pb tbt insert\n");
											}
										}
									} else if(tbt1_tbt_insert_flag == CPLD_TBT1_PB_TBT_MODE) {
										if((now_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt1_pb_tbt == CPLD_TYPE_C_TBT_REMOVED)) {
											tbt1_tbt_insert_flag = CPLD_TBT1_TBT_MODE_NULL;
											ches.modules = CPLD_M2_1;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_TBT;
											ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
											printk("tbt1 pb tbt removed\n");
										}
									}

									if(now_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt1_pa_usb == CPLD_TYPE_C_USB_INSERT) {
											if(tbt1_usb_insert_flag == CPLD_TBT2_PB_USB_MODE) {
												tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
												ches.modules = CPLD_M2_1;
												ches.port = CPLD_PORTB;
												ches.type = CPLD_TYPE_USB;
												ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
												qnap_send_ep_cable_hal_event(&ches);
												qnap_ep_asm_vbus_ctl_set_low(1);
												qnap_ep_ready_set_high(1);
												printk("tbt1 pb usb removed\n");
											}
											tbt1_usb_insert_flag = CPLD_TBT1_PA_USB_MODE;
											ches.modules = CPLD_M2_1;
											ches.port = CPLD_PORTA;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_INSERT;
											qnap_send_ep_cable_hal_event(&ches);
											printk("tbt1 pa usb insert\n");
										}
									} else if(tbt1_usb_insert_flag == CPLD_TBT1_PA_USB_MODE) {
										if((now_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt1_pa_usb == CPLD_TYPE_C_USB_REMOVED)) {
											tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
											ches.modules = CPLD_M2_1;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
											qnap_ep_asm_vbus_ctl_set_low(1);
											qnap_ep_ready_set_high(1);
											printk("tbt1 pa usb removed\n");
										}
									}

									if(now_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt1_pb_usb == CPLD_TYPE_C_USB_INSERT) {
											if(tbt2_usb_insert_flag == CPLD_TBT2_PA_USB_MODE) {
												tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
												ches.modules = CPLD_M2_1;
												ches.port = CPLD_PORTA;
												ches.type = CPLD_TYPE_USB;
												ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
												qnap_send_ep_cable_hal_event(&ches);
												qnap_ep_asm_vbus_ctl_set_low(1);
												qnap_ep_ready_set_high(1);
												printk("tbt1 pa usb removed\n");
											}
											tbt1_usb_insert_flag = CPLD_TBT1_PB_USB_MODE;
											ches.modules = CPLD_M2_1;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_INSERT;
											qnap_send_ep_cable_hal_event(&ches);
											printk("tbt1 pb usb insert\n");
										}
									} else if(tbt1_usb_insert_flag == CPLD_TBT1_PB_USB_MODE) {
										if((now_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt1_pb_usb == CPLD_TYPE_C_USB_REMOVED)) {
											tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
											ches.modules = CPLD_M2_1;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
											qnap_ep_asm_vbus_ctl_set_low(1);
											qnap_ep_ready_set_high(1);
											printk("tbt1 pb usb removed\n");
										}
									}

									// EP0 (TBT2)
									// EP0 TBT2 PB TBT
									if(now_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt2_pb_tbt == CPLD_TYPE_C_TBT_INSERT) {
											if(tbt2_usb_insert_flag == CPLD_TBT2_USB_MODE_NULL) {
												tbt2_tbt_insert_flag = CPLD_TBT2_PB_TBT_MODE;
												ches.modules = CPLD_M2_2;
												ches.port = CPLD_PORTB;
												ches.type = CPLD_TYPE_TBT;
												ches.cable_status = CPLD_TYPE_C_TBT_INSERT;
												qnap_send_ep_cable_hal_event(&ches);
												printk("tbt2 pb tbt insert\n");
											}
										}
									} else if(tbt2_tbt_insert_flag == CPLD_TBT2_PB_TBT_MODE) {
										if((now_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt2_pb_tbt == CPLD_TYPE_C_TBT_REMOVED)) {
											tbt2_tbt_insert_flag = CPLD_TBT2_TBT_MODE_NULL;
											printk("tbt2 pb tbt removed\n");
											ches.modules = CPLD_M2_2;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_TBT;
											ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
										}
									}

									if(now_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt2_pa_usb == CPLD_TYPE_C_USB_INSERT) {
											if(tbt2_usb_insert_flag == CPLD_TBT2_PB_USB_MODE) {
												tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
												ches.modules = CPLD_M2_2;
												ches.port = CPLD_PORTB;
												ches.type = CPLD_TYPE_USB;
												ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
												qnap_send_ep_cable_hal_event(&ches);
												qnap_ep_asm_vbus_ctl_set_low(0);
												qnap_ep_ready_set_high(0);
												printk("tbt2 pb usb removed\n");
											}
											tbt2_usb_insert_flag = CPLD_TBT2_PA_USB_MODE;
											ches.modules = CPLD_M2_2;
											ches.port = CPLD_PORTA;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_INSERT;
											qnap_send_ep_cable_hal_event(&ches);
											printk("tbt2 pa usb insert\n");
										}
									} else if(tbt2_usb_insert_flag == CPLD_TBT2_PA_USB_MODE) {
										if((now_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt2_pa_usb == CPLD_TYPE_C_USB_REMOVED)) {
											tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
											ches.modules = CPLD_M2_2;
											ches.port = CPLD_PORTA;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
											qnap_ep_asm_vbus_ctl_set_low(0);
											qnap_ep_ready_set_high(0);
											printk("tbt2 pa usb removed\n");
										}
									}

									if(now_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_INSERT) {
										if(now_cable_status_reg.status.tbt2_pb_usb == CPLD_TYPE_C_USB_INSERT) {
											if(tbt2_usb_insert_flag == CPLD_TBT2_PA_USB_MODE) {
												tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
												ches.modules = CPLD_M2_2;
												ches.port = CPLD_PORTA;
												ches.type = CPLD_TYPE_USB;
												ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
												qnap_send_ep_cable_hal_event(&ches);
												qnap_ep_asm_vbus_ctl_set_low(0);
												qnap_ep_ready_set_high(0);
												printk("tbt2 pa usb removed\n");
											}
											tbt2_usb_insert_flag = CPLD_TBT2_PB_USB_MODE;
											ches.modules = CPLD_M2_2;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_INSERT;
											qnap_send_ep_cable_hal_event(&ches);
											printk("tbt2 pb usb insert\n");
										}
									} else if(tbt2_usb_insert_flag == CPLD_TBT2_PB_USB_MODE) {
										if((now_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
										 (now_cable_status_reg.status.tbt2_pb_usb == CPLD_TYPE_C_USB_REMOVED)) {
											tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
											ches.modules = CPLD_M2_2;
											ches.port = CPLD_PORTB;
											ches.type = CPLD_TYPE_USB;
											ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
											qnap_send_ep_cable_hal_event(&ches);
											qnap_ep_asm_vbus_ctl_set_low(0);
											qnap_ep_ready_set_high(0);
											printk("tbt2 pb usb removed\n");
										}
									}
								}

								// EP1 TBT1 PA TBT
								if(now_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
									if(now_cable_status_reg.status.tbt1_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
										tbt1_tbt_insert_flag = CPLD_TBT1_PA_TBT_MODE;
										ches.modules = CPLD_M2_1;
										ches.port = CPLD_PORTA;
										ches.type = CPLD_TYPE_TBT;
										ches.cable_status = CPLD_TYPE_C_TBT_INSERT;
										qnap_send_ep_cable_hal_event(&ches);
										printk("tbt1 pa tbt insert\n");
									}
								} else if(tbt1_tbt_insert_flag == CPLD_TBT1_PA_TBT_MODE) {
									if((now_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
									 (now_cable_status_reg.status.tbt1_pa_tbt == CPLD_TYPE_C_TBT_INSERT)) {
										tbt1_tbt_insert_flag = CPLD_TBT1_TBT_MODE_NULL;
										ches.modules = CPLD_M2_1;
										ches.port = CPLD_PORTA;
										ches.type = CPLD_TYPE_TBT;
										ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
										qnap_send_ep_cable_hal_event(&ches);
										printk("tbt1 pa tbt removed\n");
									}
								}

								// EP0 TBT2 PA TBT
								if(now_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_INSERT) {
									if(now_cable_status_reg.status.tbt2_pa_tbt == CPLD_TYPE_C_TBT_INSERT) {
										tbt2_tbt_insert_flag = CPLD_TBT2_PA_TBT_MODE;
										ches.modules = CPLD_M2_2;
										ches.port = CPLD_PORTA;
										ches.type = CPLD_TYPE_TBT;
										ches.cable_status = CPLD_TYPE_C_TBT_INSERT;
										qnap_send_ep_cable_hal_event(&ches);
										printk("tbt2 pa tbt insert\n");
									}
								} else if(tbt2_tbt_insert_flag == CPLD_TBT2_PA_TBT_MODE) {
									if((now_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) &&
									 (now_cable_status_reg.status.tbt2_pa_tbt == CPLD_TYPE_C_TBT_INSERT)) {
										tbt2_tbt_insert_flag = CPLD_TBT2_TBT_MODE_NULL;
										ches.modules = CPLD_M2_2;
										ches.port = CPLD_PORTA;
										ches.type = CPLD_TYPE_TBT;
										ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
										qnap_send_ep_cable_hal_event(&ches);
										printk("tbt2 pa tbt removed\n");
									}
								}
							}
						}

						if(c_data == g_cable_status_reg.data) {
							// EP1 TBT1 PA USB
							if(tbt1_usb_insert_flag == CPLD_TBT1_PA_USB_MODE) {
								if(now_cable_vbus_status_reg.status.tbt1_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) {
									tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
									ches.modules = CPLD_M2_1;
									ches.port = CPLD_PORTA;
									ches.type = CPLD_TYPE_USB;
									ches.cable_status = CPLD_TYPE_C_TBT_REMOVED;
									qnap_send_ep_cable_hal_event(&ches);
									qnap_ep_asm_vbus_ctl_set_low(1);
									qnap_ep_ready_set_high(1);
									printk("tbt1 pa usb removed\n");
								}
							}
							// EP1 TBT1 PB USB
							if(tbt1_usb_insert_flag == CPLD_TBT1_PB_USB_MODE) {
								if(now_cable_vbus_status_reg.status.tbt1_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) {
									tbt1_usb_insert_flag = CPLD_TBT1_USB_MODE_NULL;
									ches.modules = CPLD_M2_1;
									ches.port = CPLD_PORTB;
									ches.type = CPLD_TYPE_USB;
									ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
									qnap_send_ep_cable_hal_event(&ches);
									qnap_ep_asm_vbus_ctl_set_low(1);
									qnap_ep_ready_set_high(1);
									printk("tbt1 pb usb removed\n");
								}
							}
							// EP0 TBT2 PA USB
							if(tbt2_usb_insert_flag == CPLD_TBT2_PA_USB_MODE) {
								if(now_cable_vbus_status_reg.status.tbt2_pa_vbus == CPLD_TYPE_C_VBUS_REMOVED) {
									tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
									ches.modules = CPLD_M2_2;
									ches.port = CPLD_PORTA;
									ches.type = CPLD_TYPE_USB;
									ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
									qnap_send_ep_cable_hal_event(&ches);
									qnap_ep_asm_vbus_ctl_set_low(0);
									qnap_ep_ready_set_high(0);
									printk("tbt2 pa usb removed\n");
								}
							}
							// EP0 TBT2 PB USB
							if(tbt2_usb_insert_flag == CPLD_TBT2_PB_USB_MODE) {
								if(now_cable_vbus_status_reg.status.tbt2_pb_vbus == CPLD_TYPE_C_VBUS_REMOVED) {
									tbt2_usb_insert_flag = CPLD_TBT2_USB_MODE_NULL;
									ches.modules = CPLD_M2_2;
									ches.port = CPLD_PORTB;
									ches.type = CPLD_TYPE_USB;
									ches.cable_status = CPLD_TYPE_C_USB_REMOVED;
									qnap_send_ep_cable_hal_event(&ches);
									qnap_ep_asm_vbus_ctl_set_low(0);
									qnap_ep_ready_set_high(0);
									printk("tbt2 pb usb removed\n");
								}
							}
						}
						g_cable_vbus_status_reg.data = v_data;
						g_cable_status_reg.data = c_data;
						g_i_worked = 0;
					}
				}
			}
		}
	}

	return 0;
}

static int qnap_ep_intx_thread_1(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	struct ep_port_info_data *ep_port_info = &dev_qnap_ep->ep_info_data;
	int i_worked = 1;
	unsigned char data;
#if 0
	while (!kthread_should_stop()) {
		msleep(100);
		if(cpld_i2c_ready)
			break;
	}

	while (!kthread_should_stop()) {
		if (!i_worked) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
		i_worked = 0;
	}
	set_current_state(TASK_RUNNING);
#endif
	return 0;
}

static irqreturn_t ep_gpio_intx_interrupt_handler_0(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	unsigned char asm_vbus_ctl_val = 0;
	int ret = -1;

#if 0
	printk("INTX IRQ 0!\n");
#endif
	return IRQ_HANDLED;
}

static irqreturn_t ep_gpio_intx_interrupt_handler_1(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
//	struct ep_port_info_data *ep_port_info = &dev_qnap_ep->ep_info_data;
	int i;

#if 0
	for(i=0;i<2;i++)
		printk("ep_info_data->gpio_intx_num[%d] = 0x%x\n", i, ep_port_info->gpio_intx_num[i]);

	for(i=0;i<2;i++)
		printk("ep_info_data->gpio_sw_ready_num[%d] = 0x%x\n", i, ep_port_info->gpio_sw_ready_num[i]);
#endif

#if 0
	if (dev_qnap_ep->ep_intx_thread[1] && dev_qnap_ep->ep_intx_thread[1]->state != TASK_RUNNING) {
		printk("INTX TASK_RUNNING 1 is not running\n");
		wake_up_process(dev_qnap_ep->ep_intx_thread[1]);
	} else {
		printk("INTX TASK_RUNNING 1 is running\n");
	}

	printk("INTX IRQ 1!\n");
#endif

	return IRQ_HANDLED;
}

#if 0
static int qnap_ep_reset_thread_0(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	int i_worked = 1;
	unsigned char data;

	while (!kthread_should_stop()) {
		msleep(100);
		if(cpld_i2c_ready)
			break;
	}

	while (!kthread_should_stop()) {
		if (!i_worked) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
		i_worked = 0;

	}
	set_current_state(TASK_RUNNING);

	return 0;
}

static int qnap_ep_reset_thread_1(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	int i_worked = 1;
	unsigned char data;

	while (!kthread_should_stop()) {
		msleep(100);
		if(cpld_i2c_ready)
			break;
	}

	while (!kthread_should_stop()) {
		if (!i_worked) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
		i_worked = 0;

	}
	set_current_state(TASK_RUNNING);

	return 0;
}

static int qnap_ep_reset_thread_2(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	int i_worked = 1;
	unsigned char data;

	while (!kthread_should_stop()) {
		msleep(100);
		if(cpld_i2c_ready)
			break;
	}

	while (!kthread_should_stop()) {
		if (!i_worked) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
		i_worked = 0;

	}
	set_current_state(TASK_RUNNING);

	return 0;
}

static int qnap_ep_reset_thread_3(void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;
	int i_worked = 1;
	unsigned char data;

	while (!kthread_should_stop()) {
		msleep(100);
		if(cpld_i2c_ready)
			break;
	}

	while (!kthread_should_stop()) {
		if (!i_worked) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			__set_current_state(TASK_RUNNING);
		}
		i_worked = 0;

	}
	set_current_state(TASK_RUNNING);

	return 0;
}

static irqreturn_t ep_gpio_reset_interrupt_handler_0(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;

	if (dev_qnap_ep->ep_reset_thread[0] && dev_qnap_ep->ep_reset_thread[0]->state != TASK_RUNNING) {
		printk("Reset TASK_RUNNING 0 is not running\n");
		wake_up_process(dev_qnap_ep->ep_reset_thread[1]);
	} else {
		printk("Reset TASK_RUNNING 0 is running\n");
	}

	printk("RESET IRQ 0!\n");
	return IRQ_HANDLED;
}

static irqreturn_t ep_gpio_reset_interrupt_handler_1(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;

	if (dev_qnap_ep->ep_reset_thread[1] && dev_qnap_ep->ep_reset_thread[1]->state != TASK_RUNNING) {
		printk("Reset TASK_RUNNING 1 is not running\n");
		wake_up_process(dev_qnap_ep->ep_reset_thread[1]);
	} else {
		printk("Reset TASK_RUNNING 1 is running\n");
	}

	printk("RESET IRQ 1!\n");
	return IRQ_HANDLED;
}

static irqreturn_t ep_gpio_reset_interrupt_handler_2(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;

	if (dev_qnap_ep->ep_reset_thread[2] && dev_qnap_ep->ep_reset_thread[2]->state != TASK_RUNNING) {
		printk("Reset TASK_RUNNING 2 is not running\n");
		wake_up_process(dev_qnap_ep->ep_reset_thread[1]);
	} else {
		printk("Reset TASK_RUNNING 2 is running\n");
	}

	printk("RESET IRQ 2!\n");
	return IRQ_HANDLED;
}

static irqreturn_t ep_gpio_reset_interrupt_handler_3(int irq, void *dev_id)
{
	struct qnap_ep_data *dev_qnap_ep = dev_id;

	if (dev_qnap_ep->ep_reset_thread[3] && dev_qnap_ep->ep_reset_thread[3]->state != TASK_RUNNING) {
		printk("Reset TASK_RUNNING 3 is not running\n");
		wake_up_process(dev_qnap_ep->ep_reset_thread[1]);
	} else {
		printk("Reset TASK_RUNNING 3 is running\n");
	}

	printk("RESET IRQ 3!\n");
	return IRQ_HANDLED;
}
#endif
#endif

static int qnap_pcie_ep_probe(struct platform_device *pdev)
{
	struct reg_value_data ep_port_reg_info_data;
	struct reg_value_data ep_port_inbound_bar0_reserved_memory;
	struct reg_value_data ep_port_config_reserved_memory;
	struct reg_value_data pbs_reg_info_data;
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	struct i2c_board_info i2c_info = {
		.type   = "qnap_cpld",
		//.addr   = 0x31,	// cpld slave address (0x62)0x31
		.addr = 0x00,
	};
	struct i2c_client *i2c_result;
#endif
	int i, ret, retval;
	struct proc_dir_entry *mode;
	struct device *dev = &pdev->dev;
	struct qnap_ep_data *dev_qnap_ep;
	struct ep_ports_list *tmp_ep_ports_list;
	struct list_head *l, *n;
        unsigned char g_type_c_status, g_type_c_vbus_status;

	void *pbs_addr_base;
	void __iomem *memptr;

	dev_qnap_ep = devm_kzalloc(dev, sizeof(*dev_qnap_ep), GFP_KERNEL);
	if (!dev_qnap_ep) {
		return -ENOMEM;
	}

	// init g_cable_status_reg default value
	g_cable_status_reg.data = 0x00;

	printk("QNAP EP Driver initial\n");
	if(qnap_pcie_ep_boardcfg_parser(&dev_qnap_ep->ep_info_data) < 0) {
		pr_err("warning: boardcfg parser error !!\n");
		printk("Can not find endpoint device !!\n");
		return -EINVAL;
	} else {
		printk("found %d endpoint device\n", dev_qnap_ep->ep_info_data.ep_ports_counts);
		for(i=0;i<dev_qnap_ep->ep_info_data.ep_ports_counts;i++) {
			if(qnap_pcie_ep_pcie_external_parser(&ep_port_reg_info_data, dev_qnap_ep->ep_info_data.ep_ports[i]) < 0) {
				pr_err("warning: pcie external parser parser error\n");
				return -EINVAL;
			}
		}

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
		dev_qnap_ep->i2c_adap = i2c_get_adapter(dev_qnap_ep->ep_info_data.cpld_i2c_bus_id);
		if(!dev_qnap_ep->i2c_adap) {
			printk("Can not find i2c adapter bus id : '%d'\n", dev_qnap_ep->ep_info_data.cpld_i2c_bus_id);
			return -ENODEV;
		} else {
			if(dev_qnap_ep->ep_info_data.cpld_i2c_slave_address != 0x00) {
				i2c_info.addr = dev_qnap_ep->ep_info_data.cpld_i2c_slave_address >> 1;
				i2c_result = i2c_new_device(dev_qnap_ep->i2c_adap, &i2c_info);
				if (i2c_result == NULL) {
					pr_err("warning: i2c added device fail, i2c name : %s, i2c slave address : 0x%x\n", i2c_info.type, i2c_info.addr);
					return -EINVAL;
				} else {
					printk("ep register i2c slave address 0x%x(0x%x) to device okay !!\n", dev_qnap_ep->ep_info_data.cpld_i2c_slave_address,
													       dev_qnap_ep->ep_info_data.cpld_i2c_slave_address >> 1);
				}
			} else {
				pr_err("warning: i2c slave address, i2c name : %s, i2c slave address : 0x%x\n", i2c_info.type, i2c_info.addr);
				return -EINVAL;
			}
		}
#endif

		if(qnap_pcie_ep_pbs_parser(&pbs_reg_info_data) < 0) {
			pr_err("warning: pbs parser error\n");
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
			retval = -EINVAL;
			goto i2c_err;
#else
			return -EINVAL;
#endif
		}
		else {
			pbs_addr_base = ioremap(pbs_reg_info_data.reg, pbs_reg_info_data.size);
			if(!pbs_addr_base) {
				printk("could not allocate memory pf memory\n");
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
				retval = -ENOMEM;
				goto i2c_err;
#else
				return -ENOMEM;
#endif
			}
		}

		INIT_LIST_HEAD(&dev_qnap_ep->ep_ports_mylist.list);
		for(i=0;i<dev_qnap_ep->ep_info_data.ep_ports_counts;i++) {
			if(qnap_pcie_ep_pcie_external_parser(&ep_port_reg_info_data, dev_qnap_ep->ep_info_data.ep_ports[i]) < 0) {
				pr_err("warning: pcoe external parser parser error\n");
				retval = -EINVAL;
				goto pbs_err;
			}
			else {
				dev_qnap_ep->tmp_ep_ports_list = kzalloc(sizeof(struct ep_ports_list), GFP_KERNEL);
				dev_qnap_ep->tmp_ep_ports_list->port_id = dev_qnap_ep->ep_info_data.ep_ports[i];
				dev_qnap_ep->tmp_ep_ports_list->port_counts = dev_qnap_ep->ep_info_data.ep_ports_counts;
				dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag = dev_qnap_ep->ep_info_data.ep_uboot_init_flag;
#if 0
				printk("ep_info_data.ep_ports[%d] = %d, port counts = %d\n", i, dev_qnap_ep->ep_info_data.ep_ports[i],
											dev_qnap_ep->ep_info_data.ep_ports_counts);
				printk("ep_uboot_init_flag = 0x%x\n", dev_qnap_ep->ep_info_data.ep_uboot_init_flag);
#endif
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pbs_addr_base = pbs_addr_base;

				// @ qnap_pcie_ep_pcie_external_parser get pcie ep port base address
				memptr = ioremap(ep_port_reg_info_data.reg, ep_port_reg_info_data.size);
				if(!memptr) {
					printk("could not allocate memory ep_port memory\n");
					return -ENOMEM;
				}
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_addr_base = memptr;

#if 0
				memptr = kzalloc(QNAP_MEM_SIZE, GFP_KERNEL);
				if(!memptr) {
					printk("could not allocate memory port bar0 memory\n");
					return -ENOMEM;
				}
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0 = memptr;
#else
				if(qnap_pcie_ep_inbound_reserved_memory_parser(&ep_port_inbound_bar0_reserved_memory, dev_qnap_ep->ep_info_data.ep_ports[i]) < 0) {
					pr_err("warning: pcie Inbound BAR0 reserved memory parser error from devices tree, it will change to kernel alloc memory used.\n");
					memptr = kzalloc(QNAP_MEM_SIZE, GFP_KERNEL);
					if(!memptr) {
						printk("could not allocate memory port bar0 memory\n");
						return -ENOMEM;
					}
					dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0 = memptr;
				} else {
					printk("PCIe %d Inbound BAR0 reserved allocated memory node: base 0x%x, size %d %s\n",
					dev_qnap_ep->ep_info_data.ep_ports[i],
					ep_port_inbound_bar0_reserved_memory.reg,
					(ep_port_inbound_bar0_reserved_memory.size >= SZ_1M) ? ep_port_inbound_bar0_reserved_memory.size / SZ_1M : ep_port_inbound_bar0_reserved_memory.size / SZ_1K,
					(ep_port_inbound_bar0_reserved_memory.size >= SZ_1M) ? "MiB" : "KiB");

					dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0 = phys_to_virt(ep_port_inbound_bar0_reserved_memory.reg);
					if(ep_port_inbound_bar0_reserved_memory.size > QNAP_MEM_SIZE) {
						pr_err("warning: This is not mismatch of the device tree reserved memory > maximum size %d, %s\n",
						(QNAP_MEM_SIZE >= SZ_1M) ? QNAP_MEM_SIZE / SZ_1M : QNAP_MEM_SIZE / SZ_1K,
						(QNAP_MEM_SIZE >= SZ_1M) ? "MiB" : "KiB");
					}
				}
#endif
				memptr = kzalloc(QNAP_MEM_SIZE, GFP_KERNEL);
				if(!memptr) {
					printk("could not allocate memory port bar2 memory\n");
					return -ENOMEM;
				}
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2 = memptr;

				memptr = kzalloc(QNAP_MEM_SIZE, GFP_KERNEL);
				if(!memptr) {
					printk("could not allocate memory port bar4 memory\n");
					return -ENOMEM;
				}
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4 = memptr;

				if(dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag == AL_TRUE) {
					if(qnap_pcie_ep_port_reserved_memory_parser(&ep_port_config_reserved_memory, dev_qnap_ep->ep_info_data.ep_ports[i]) < 0) {
						pr_err("warning: pcie port reserved memory parser error from devices tree, it will change to kernel alloc memory used.\n");
						memptr = kzalloc(sizeof(struct al_pcie_port), GFP_KERNEL);
						if(!memptr) {
							printk("could not allocate memory pcie port memory\n");
							return -ENOMEM;
						}
						dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config = memptr;
						//dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag = AL_FALSE;
					} else {
						printk("PCIe %d port config reserved allocated memory node: base 0x%x, size %d %s\n",
												dev_qnap_ep->ep_info_data.ep_ports[i],
												ep_port_config_reserved_memory.reg,
												(ep_port_config_reserved_memory.size >= SZ_1M) ? ep_port_config_reserved_memory.size / SZ_1M : ep_port_config_reserved_memory.size / SZ_1K,
												(ep_port_config_reserved_memory.size >= SZ_1M) ? "MiB" : "KiB");
						dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config = phys_to_virt(ep_port_config_reserved_memory.reg);
					}
				} else {
					memptr = kzalloc(sizeof(struct al_pcie_port), GFP_KERNEL);
					if(!memptr) {
						printk("could not allocate memory pcie port memory\n");
						return -ENOMEM;
					}
					dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config = memptr;
				}

				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.id_params.vendor_id = dev_qnap_ep->ep_info_data.id_params[i].vendor_id;
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.id_params.device_id = dev_qnap_ep->ep_info_data.id_params[i].device_id;
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.id_params.revision_id = dev_qnap_ep->ep_info_data.id_params[i].revision_id;
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.id_params.subsystem_vendor_id = dev_qnap_ep->ep_info_data.id_params[i].subsystem_vendor_id;
				dev_qnap_ep->tmp_ep_ports_list->qnap_ep.id_params.subsystem_device_id = dev_qnap_ep->ep_info_data.id_params[i].subsystem_device_id;
#if 1
				ret = qnap_ep_init_pcie_port(dev_qnap_ep->tmp_ep_ports_list);
				if(ret) {
					printk("PCIe %d: init pcie port fail\n", dev_qnap_ep->ep_info_data.ep_ports[i]);
					if(dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag != AL_TRUE)
						kfree(dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_config);
					kfree(dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0);
					kfree(dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2);
					kfree(dev_qnap_ep->tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4);
					iounmap(dev_qnap_ep->tmp_ep_ports_list->qnap_ep.pcie_port_addr_base);
					kfree(dev_qnap_ep->tmp_ep_ports_list);
					goto port_err;
				} else {
					list_add(&(dev_qnap_ep->tmp_ep_ports_list->list), &(dev_qnap_ep->ep_ports_mylist.list));
				}
#endif
			}
		}
	}

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_intx_counts;i++) {
		if(i == 0) {
			memset(dev_qnap_ep->intx_irqthreadname_0, 0x00, sizeof(dev_qnap_ep->intx_irqthreadname_0));
			sprintf(dev_qnap_ep->intx_irqthreadname_0, "qep_intx_%d", i);
			dev_qnap_ep->ep_intx_thread[i] = kthread_run(qnap_ep_intx_thread_0, dev_qnap_ep, dev_qnap_ep->intx_irqthreadname_0);
			if (dev_qnap_ep->ep_intx_thread[i] == ERR_PTR(-ENOMEM)) {
				printk("failed to create EP %d thread (out of memory)\n", i);
				goto port_err;
			}

		} else if(i == 1) {
			memset(dev_qnap_ep->intx_irqthreadname_1, 0x00, sizeof(dev_qnap_ep->intx_irqthreadname_1));
			sprintf(dev_qnap_ep->intx_irqthreadname_1, "qep_intx_%d", i);
			dev_qnap_ep->ep_intx_thread[i] = kthread_run(qnap_ep_intx_thread_1, dev_qnap_ep, dev_qnap_ep->intx_irqthreadname_1);
			if (dev_qnap_ep->ep_intx_thread[i] == ERR_PTR(-ENOMEM)) {
				printk("failed to create EP %d thread (out of memory)\n", i);
				goto kthread_err;
			}
		}
	}

	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_intx_counts;i++) {
		printk("gpio intx num [%d] = %d, 0x%x\n", i, dev_qnap_ep->ep_info_data.gpio_intx_num[i], dev_qnap_ep->ep_info_data.gpio_intx_num[i]);

		if(i == 0) {
			memset(dev_qnap_ep->intx_gpioname_0, 0x00, sizeof(dev_qnap_ep->intx_gpioname_0));
			sprintf(dev_qnap_ep->intx_gpioname_0, "GPIO_CPLD_INTX_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_intx_num[i], GPIOF_IN, dev_qnap_ep->intx_gpioname_0);
			if(retval) {
				memset(dev_qnap_ep->intx_gpioname_0, 0x00, sizeof(dev_qnap_ep->intx_gpioname_0));
				printk("gpio num %d intx init fail\n", dev_qnap_ep->ep_info_data.gpio_intx_num[i]);
				goto kthread_err;
			}
		} else if (i == 1) {
			memset(dev_qnap_ep->intx_gpioname_1, 0x00, sizeof(dev_qnap_ep->intx_gpioname_1));
			sprintf(dev_qnap_ep->intx_gpioname_1, "GPIO_CPLD_INTX_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_intx_num[i], GPIOF_IN, dev_qnap_ep->intx_gpioname_1);
			if(retval) {
				memset(dev_qnap_ep->intx_gpioname_1, 0x00, sizeof(dev_qnap_ep->intx_gpioname_1));
				printk("gpio num %d intx init fail\n", dev_qnap_ep->ep_info_data.gpio_intx_num[i]);
				goto intx_gpio_err;
			}
		} else  {
			goto intx_gpio_err;
		}

		dev_qnap_ep->intx_irqnum[i] = gpio_to_irq(dev_qnap_ep->ep_info_data.gpio_intx_num[i]);
		if(dev_qnap_ep->intx_irqnum[i] < 0) {
			printk(KERN_ERR "Unable to gpio num %d to irq: %d\n", dev_qnap_ep->ep_info_data.gpio_intx_num[i], dev_qnap_ep->intx_irqnum[i]);
			goto intx_gpio_err;
		}

		if(i == 0) {
			memset(dev_qnap_ep->intx_irqname_0, 0x00, sizeof(dev_qnap_ep->intx_irqname_0));
			sprintf(dev_qnap_ep->intx_irqname_0, "qepirq%d", i);
			retval = request_irq(dev_qnap_ep->intx_irqnum[i], ep_gpio_intx_interrupt_handler_0, IRQF_TRIGGER_RISING, dev_qnap_ep->intx_irqname_0, dev_qnap_ep);
			if (retval < 0) {
				dev_qnap_ep->intx_irqnum[i] = 0;
				printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
				goto gpioirq_err;
			}
		} else if(i == 1) {
			memset(dev_qnap_ep->intx_irqname_1, 0x00, sizeof(dev_qnap_ep->intx_irqname_1));
			sprintf(dev_qnap_ep->intx_irqname_1, "qepirq%d", i);
			retval = request_irq(dev_qnap_ep->intx_irqnum[i], ep_gpio_intx_interrupt_handler_1, IRQF_TRIGGER_RISING, dev_qnap_ep->intx_irqname_1, dev_qnap_ep);
			if (retval < 0) {
				dev_qnap_ep->intx_irqnum[i] = 0;
				printk(KERN_ERR "Unable to request IRQ: %d\n", ret);
				goto gpioirq_err;
			}
		}
	}

#if 0

#endif
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_sw_ready_counts;i++) {
		printk("gpio sw ready num [%d] = %d, 0x%x\n", i, dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i], dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);

		if(i == 0) {
			memset(dev_qnap_ep->sw_ready_gpioname_0, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_0));
			sprintf(dev_qnap_ep->sw_ready_gpioname_0, "GPIO_SW_READY_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i], GPIOF_OUT_INIT_HIGH, dev_qnap_ep->sw_ready_gpioname_0);
			if(retval) {
				memset(dev_qnap_ep->sw_ready_gpioname_0, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_0));
				printk("gpio num %d sw ready init fail\n", dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
				goto gpioirq_err;
			}
		} else if (i == 1) {
			memset(dev_qnap_ep->sw_ready_gpioname_1, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_1));
			sprintf(dev_qnap_ep->sw_ready_gpioname_1, "GPIO_SW_READY_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i], GPIOF_OUT_INIT_HIGH, dev_qnap_ep->sw_ready_gpioname_1);
			if(retval) {
				memset(dev_qnap_ep->sw_ready_gpioname_1, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_1));
				printk("gpio num %d sw ready init fail\n", dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
				goto sw_ready_gpio_err;
			}
		} else if (i == 2) {
			memset(dev_qnap_ep->sw_ready_gpioname_2, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_2));
			sprintf(dev_qnap_ep->sw_ready_gpioname_2, "GPIO_SW_READY_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i], GPIOF_OUT_INIT_HIGH, dev_qnap_ep->sw_ready_gpioname_2);
			if(retval) {
				memset(dev_qnap_ep->sw_ready_gpioname_2, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_2));
				printk("gpio num %d sw ready init fail\n", dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
				goto sw_ready_gpio_err;
			}
		} else if (i == 3) {
			memset(dev_qnap_ep->sw_ready_gpioname_3, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_3));
			sprintf(dev_qnap_ep->sw_ready_gpioname_3, "GPIO_SW_READY_%d", i);
			retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i], GPIOF_OUT_INIT_HIGH, dev_qnap_ep->sw_ready_gpioname_3);
			if(retval) {
				memset(dev_qnap_ep->sw_ready_gpioname_3, 0x00, sizeof(dev_qnap_ep->sw_ready_gpioname_3));
				printk("gpio num %d sw ready init fail\n", dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
				goto sw_ready_gpio_err;
			}
		} else  {
			goto sw_ready_gpio_err;
		}
	}

	printk("gpio usb mux num = %d, 0x%x is value %d, 0x%x\n",
				dev_qnap_ep->ep_info_data.gpio_usb_mux_num, dev_qnap_ep->ep_info_data.gpio_usb_mux_num,
				dev_qnap_ep->ep_info_data.gpio_usb_mux_init_value, dev_qnap_ep->ep_info_data.gpio_usb_mux_init_value);
	memset(dev_qnap_ep->usb_mux_gpioname, 0x00, sizeof(dev_qnap_ep->usb_mux_gpioname));
	sprintf(dev_qnap_ep->usb_mux_gpioname, "GPIO_USB_MUX");
	if(dev_qnap_ep->ep_info_data.gpio_usb_mux_init_value == GPIOF_INIT_HIGH)
		retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_usb_mux_num, GPIOF_OUT_INIT_HIGH, dev_qnap_ep->usb_mux_gpioname);
	else
		retval = gpio_request_one(dev_qnap_ep->ep_info_data.gpio_usb_mux_num, GPIOF_OUT_INIT_LOW, dev_qnap_ep->usb_mux_gpioname);

	if(retval) {
		memset(dev_qnap_ep->usb_mux_gpioname, 0x00, sizeof(dev_qnap_ep->usb_mux_gpioname));
		printk("gpio num %d usb mux init fail\n", dev_qnap_ep->ep_info_data.gpio_usb_mux_num);
		goto sw_ready_gpio_err;
	}
#endif

	// procfs
	proc_qnap_pcie_ep_root = proc_mkdir("qnapep", NULL);
	if(!proc_qnap_pcie_ep_root) {
		printk(KERN_ERR "Couldn't create qnapep folder in procfs\n");
#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
		goto gpioirq_err;
#else
		return -EINVAL;
#endif
	}

	// create file of folder in procfs
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_qnap_pcie_ep_root, &qnap_pcie_ep_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnapep procfs node\n");
		goto proc_err1;
	}

	g_dev_qnap_ep = dev_qnap_ep;
	platform_set_drvdata(pdev, dev);

	return 0;

#if 0
proc_err2:
	remove_proc_entry("mode", proc_qnap_pcie_ep_root);
#endif

proc_err1:
	remove_proc_entry("qnapep", NULL);

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
sw_ready_gpio_err:
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_sw_ready_counts;i++) {
		if(strlen(dev_qnap_ep->sw_ready_gpioname_0) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
		if(strlen(dev_qnap_ep->sw_ready_gpioname_1) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
		if(strlen(dev_qnap_ep->sw_ready_gpioname_2) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
		if(strlen(dev_qnap_ep->sw_ready_gpioname_3) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_sw_ready_num[i]);
	}

gpioirq_err:
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_intx_counts;i++) {
		if(dev_qnap_ep->intx_irqnum[i] > 0) {
			free_irq(dev_qnap_ep->intx_irqnum[i], dev_qnap_ep);
		}
	}

intx_gpio_err:
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_intx_counts;i++) {
		if(strlen(dev_qnap_ep->intx_gpioname_0) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_intx_num[i]);
		if(strlen(dev_qnap_ep->intx_gpioname_1) == 0)
			gpio_free(dev_qnap_ep->ep_info_data.gpio_intx_num[i]);
	}

kthread_err:
	for(i=0;i<dev_qnap_ep->ep_info_data.gpio_intx_counts;i++) {
		if(dev_qnap_ep->ep_intx_thread[i] != NULL) {
			kthread_stop(dev_qnap_ep->ep_intx_thread[i]);
			dev_qnap_ep->ep_intx_thread[i] = NULL;
		}
	}
#endif

port_err:
	list_for_each_safe(l, n, &dev_qnap_ep->ep_ports_mylist.list) {
		tmp_ep_ports_list = list_entry(l, struct ep_ports_list, list);
		if(dev_qnap_ep->tmp_ep_ports_list->port_uboot_init_flag != AL_TRUE)
			kfree(tmp_ep_ports_list->qnap_ep.pcie_port_config);
		kfree(tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar0);
		kfree(tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar2);
		kfree(tmp_ep_ports_list->qnap_ep.inbound_base_addr_bar4);
		iounmap(tmp_ep_ports_list->qnap_ep.pcie_port_addr_base);
		list_del(l);
		kfree(tmp_ep_ports_list);
	}

pbs_err:
	iounmap(pbs_addr_base);

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
i2c_err:
	i2c_unregister_device(i2c_result);
#endif

	return retval;
}

static int qnap_pcie_ep_remove(struct platform_device *pdev)
{
	struct device *dev = platform_get_drvdata(pdev);

	remove_proc_entry("mode", proc_qnap_pcie_ep_root);
	remove_proc_entry("qnapep", NULL);

	return 0;
}

static struct platform_driver qnap_pcie_ep_plat_driver = {
	.driver = {
		.name		= "qnap-pcie-ep",
		.owner		= THIS_MODULE
	},
	.probe = qnap_pcie_ep_probe,
	.remove = qnap_pcie_ep_remove,
};

static int __init qnap_pcie_ep_init_module(void)
{
	int rv;

	rv = platform_driver_register(&qnap_pcie_ep_plat_driver);
	if(rv) {
		printk(KERN_ERR "Unable to register "
			"driver: %d\n", rv);
		return rv;
	}

	return rv;
}

static void __exit qnap_pcie_ep_cleanup_module(void)
{
	platform_driver_unregister(&qnap_pcie_ep_plat_driver);

	return;
}

module_init(qnap_pcie_ep_init_module);
//postcore_initcall(qnap_pcie_ep_init_module);
module_exit(qnap_pcie_ep_cleanup_module);

MODULE_AUTHOR("Yong-Yu Yeh <tomyeh@qnap.com>");
MODULE_DESCRIPTION("QNAP EP Init driver");
MODULE_LICENSE("GPL");
