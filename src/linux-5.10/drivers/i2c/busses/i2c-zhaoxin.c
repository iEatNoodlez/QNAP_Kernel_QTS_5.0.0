/*
    i2c-zhaoxin.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* * * * * * * * * * * * * * Release History * * * * * * * * * * * * * * * * */
/*                                                                           */
/*            AnnieLiu Version                                               */
/*            SilviaZhao Version                                             */
/* 2019-07-04 HansHu V2.0.0  adjust framework based on i2c-zhaoxin.c v1.1.0  */
/* 2019-08-09 HansHu V2.0.1  add support for ZXSMB_V02, add version id check */
/* 2020-05-08 HansHu V2.0.2  add process about SMBALERT function,            */
/*                           add support for ZXSMB_V10                       */
/* 2021-06-01 JuliusZhu V2.0.3-rc  Add support for ZXSMB_V20;                */
/*                                 Add host PEC check                        */
/*                                 Add get semaphore before transfer cycle   */


#define DRIVER_VERSION  "2.0.3"
#define VERIFY_VERSION
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/init.h>
#include <asm/io.h>
#include <linux/netlink.h>
#include <net/sock.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/platform_device.h>


#define   MAX_HOSTS_SURPPORT  4

static bool alert_func = 0;
module_param(alert_func, bool, 0444);
MODULE_PARM_DESC(alert_func, "If you want enable SMBALERT#, set alert_func=1");


static u8 clock_freq[MAX_HOSTS_SURPPORT] = {[0 ... (MAX_HOSTS_SURPPORT - 1)] = 0};
module_param_array(clock_freq, byte, NULL, 0664);
MODULE_PARM_DESC(clock_freq, "Clock Frequency Select. Array for multi hosts. "
				"Value range 10~100(kHz)");

//#define SMBDEBUG
#ifdef  SMBDEBUG
#define zx_smb_printk   printk	
#else
#define zx_smb_printk(...)
#endif

/*
 * registers
 */
/* SMBus PCI Address Constants */
#define CFGHST0BA           0xD0
#define CFGHST1BA           0xDA
#define CFGHSTCFG           0xD2
#define   SMB0EN                  (1 << 0) /* Enable host0 controller */
#define   ENSMB0IRQ               (1 << 1) /* Enable host0 IRQ        */
#define   SMBISEL                 (1 << 3) /* Select IRQ SCI/SMI      */
#define   SMB1EN                  (1 << 4) /* Enable host1 controller */
#define   ENSMB1IRQ               (1 << 5) /* Enable host1 IRQ        */
#define CFGRevID            0xD6
#define CFGHSTCSE           0xD7
#define   EN_HST0_CLK             (1 << 0)
#define   EN_HST1_CLK             (1 << 1)
#define CFGHST0CS           0xD8
#define CFGHST1CS           0xD9

/* SMBus MMIO address offsets */
#define SMBHSTSTS           0x00
#define   HST_BUSY                (1 << 0) /* Host busy               */
#define   HST_CMD_CMPLET          (1 << 1) /* Host CMD trans complete */
#define   HST_DEV_ERR             (1 << 2) /* Device error            */
#define   HST_BUS_CLSI            (1 << 3) /* Bus collision           */
#define   HST_FAIL_TRANS          (1 << 4) /* Fail trans (killed ...) */
#define   HST_PEC_ERR             (1 << 7) /* Fail trans (killed ...) */

#define SMBSLVSTS           0x01
#define SMBHSTCTL           0x02
#define   HST_CMPLT_EN            (1 << 0) /* Enable trans complete   */
#define   HST_START               (1 << 6) /* Start                   */
#define   HST_PEC_EN              (1 << 7) /* PEC enable              */
#define SMBHSTCMD           0x03
#define SMBHSTADD           0x04
#define SMBHSTDAT0          0x05
#define SMBHSTDAT1          0x06
#define SMBBLKDAT           0x07
#define SMBHSTALERTBANK     0x0E
#define   HST_ALERT_EN            (1 << 0)
#define   HST_ALERT_STS           (1 << 1)
#define   HST_BANK_SEL            (1 << 2)
#define SMB_MMIO_SIZE       0x10

/* PM PCI Address Constants */
#define PM_CONFIG           0x81
#define PM_INT_SEL_REG      0x82
#define   SSCI_DISABLED           (0)
#define   SSCI_RESERVED           (2)
#define PM_BASE             0x88
#define   ACPI_ENABLE             (1 << 7) /* ACPI Eable */

/* PM MMIO address offsets */
#define GPSCI_STS           0x51
#define   VPI2SB_SCISLV_STS       (1 << 3)
#define GPSCI_EN            0x57
#define   VPI2SB_SCISLV_EN        (1 << 3)

/* Other settings */
#define MAX_TIMEOUT         500
#define calc_clock_value(x) ( (100000/(x)-240) / 72 )       

/*
 * platform related informations
 */
 /* zhaoxin protocol cmd constants */
#define ZX_QUICK             0x00
#define ZX_BYTE              0x04
#define ZX_BYTE_DATA         0x08
#define ZX_WORD_DATA         0x0C
#define ZX_PROC_CALL         0x10
#define ZX_BLOCK_DATA        0x14
#define ZX_I2C_10_BIT_ADDR   0x18
#define ZX_SET_BANK_ADDR     0x1C
#define ZX_READ_BANK_ADDR    0x20
#define ZX_I2C_PROC_CALL     0x30
#define ZX_I2C_BLOCK_DATA    0x34
#define ZX_I2C_7_BIT_ADDR    0x38
#define ZX_UNIVERSAL         0x3C

/* zhaoxin chipset's features */
#define FEATURE_CHECK_ALERT_LINE  (1 << 0)
#define FEATURE_TWO_HOST          (1 << 1)
#define FEATURE_I2CBLOCK          (1 << 2)
#define FEATURE_ALERT             (1 << 3)
#define FEATURE_BANK_SET_READ     (1 << 4)
#define FEATURE_ENLARGE_BUFFER    (1 << 5)

/* zhaoxin chipset information */
#define MAX_I2C_LEN_SURPPORT      32
#define ZX_SMBUS_NAME             "Zhaoxin-SMBus"
#define ZX_PLATFORM_NAME_COMPAT   "COMPAT"
#define ZX_PLATFORM_NAME_V00      "ZXSMB_V00"
#define ZX_PLATFORM_NAME_V01      "ZXSMB_V01"
#define ZX_PLATFORM_NAME_V02      "ZXSMB_V02"
#define ZX_PLATFORM_NAME_V10      "ZXSMB_V10"
#define ZX_PLATFORM_NAME_V20      "ZXSMB_V20"

#define I2C_ZX_DEVICE(VEND,DEV,RID,NAME) \
	.vendor_id = (VEND), .dev_id = (DEV),\
	.rev_id = (RID), .name = (NAME)
static struct platform_device_id zxsmb_host_ids[] = {
	{
		.name = ZX_PLATFORM_NAME_COMPAT,
	}, 
	{
		.name = ZX_PLATFORM_NAME_V00,
	},
	{
		.name = ZX_PLATFORM_NAME_V01,
	},
	{
		.name = ZX_PLATFORM_NAME_V02,
	},
	{
		.name = ZX_PLATFORM_NAME_V10,
	},
	{
		.name = ZX_PLATFORM_NAME_V20,
	},
	{},
};
MODULE_DEVICE_TABLE(platform, zxsmb_host_ids);

struct ZX_SMBUS_PLATFORM_INFO {
	__u32 dev_id, vendor_id;
	__u8  rev_id;
	__u8  name[PLATFORM_NAME_SIZE];
	__u32 caps;
};

static const struct ZX_SMBUS_PLATFORM_INFO plat_info_table[] = {
	{ I2C_ZX_DEVICE(0x1d17,0x1001,0x20,ZX_PLATFORM_NAME_V20),
	  .caps = FEATURE_ALERT | FEATURE_BANK_SET_READ | FEATURE_I2CBLOCK },
	
	{ I2C_ZX_DEVICE(0x1d17,0x1001,0x10,ZX_PLATFORM_NAME_V10),      
	  .caps =FEATURE_ALERT | FEATURE_BANK_SET_READ | FEATURE_TWO_HOST \
	  | FEATURE_ENLARGE_BUFFER  | FEATURE_I2CBLOCK },
	  
	{ I2C_ZX_DEVICE(0x1d17,0x1001,0x02,ZX_PLATFORM_NAME_V02),
	  .caps = FEATURE_ALERT | FEATURE_BANK_SET_READ | FEATURE_TWO_HOST |FEATURE_I2CBLOCK},
	  
	{ I2C_ZX_DEVICE(0x1d17,0x1001,0x01,ZX_PLATFORM_NAME_V01),
	  .caps = FEATURE_ALERT | FEATURE_BANK_SET_READ | FEATURE_I2CBLOCK},
	  
	{ I2C_ZX_DEVICE(0x1d17,0x1001,0x00,ZX_PLATFORM_NAME_V00)},
	
	{ I2C_ZX_DEVICE(0x1d17,0x300A,0,ZX_PLATFORM_NAME_COMPAT),
	  .caps = FEATURE_CHECK_ALERT_LINE},
	  
	/* old version for VIA id */
	{ I2C_ZX_DEVICE(0x1106,0x1001,0,ZX_PLATFORM_NAME_V00)},
	{ I2C_ZX_DEVICE(0x1106,0x300A,0,ZX_PLATFORM_NAME_COMPAT),
	  .caps = FEATURE_CHECK_ALERT_LINE},
	{}
};

struct zxsmb_ctrl {
	u16 base_reg;
	unsigned short smba;
	unsigned int caps;
	const unsigned char *plat_name;
	struct pci_dev   *pci;
	struct device    *dev;
	struct i2c_adapter adapter;

	/* for two host */
	unsigned char smben;
	unsigned char ensmbirq;
	
	/* for dual socket */
	unsigned short master_smba;
	unsigned char is_dual_socket;
	unsigned char is_master;
	unsigned short master_acpi_ioba;

	/* for interrupt */
	u8 irq;
	int irq_req_success;          /* use interrupt or polling mode */
	int trans_ret_code;           /* indicate weather trans success or fail. 0:success */
	wait_queue_head_t waitq;
	struct i2c_client	*ara_client;
	u8 alert_processing;

	/* record current trans info */
	u8 addr;
	char read_write;
	int size;
	bool pec;
};

struct zxsmb_hosts_info {
	struct zxsmb_ctrl *ctrl[MAX_HOSTS_SURPPORT];
	struct platform_device *pdev[MAX_HOSTS_SURPPORT];
	u8 count;
};
static struct zxsmb_hosts_info hosts;

static inline u8 zx_inb_p(unsigned long port)
{
	void __iomem *ioaddr;
	u8 data;

	ioaddr = ioport_map(port, 1);
	data = ioread8(ioaddr);
	ioport_unmap(ioaddr);
	return data;
}
static inline void zx_outb_p(u8 data, unsigned long port)
{
	void __iomem *ioaddr;

	ioaddr = ioport_map(port, 1);
	iowrite8(data, ioaddr);
	ioport_unmap(ioaddr);
}


static inline void smb_clear_alert_status(unsigned short smba)
{
	u8 tmp;
	tmp = zx_inb_p(smba + SMBHSTALERTBANK);
	tmp = tmp & HST_ALERT_STS;
	zx_outb_p(tmp, smba + SMBHSTALERTBANK);
}

static inline void smb_hst_alert_irq_disable(unsigned short smba)
{
	u8 tmp;
	tmp = zx_inb_p(smba + SMBHSTALERTBANK);
	tmp = tmp & (~HST_ALERT_EN);
	zx_outb_p(tmp, smba + SMBHSTALERTBANK);
}

static inline void smb_hst_alert_irq_enable(unsigned short smba)
{
	u8 tmp;
	tmp = zx_inb_p(smba + SMBHSTALERTBANK);
	tmp = tmp | HST_ALERT_EN;
	zx_outb_p(tmp, smba + SMBHSTALERTBANK);
}
static inline void smb_main_irq_disable(struct zxsmb_ctrl *ctrl)
{
	struct pci_dev *pci = ctrl->pci;
	u8 tmp;
	if (pci) {
		pci_read_config_byte(pci, CFGHSTCFG, &tmp);
		zx_smb_printk("Before disable,CFGHSTCFG config is 0x%02x \n",tmp);
		pci_write_config_byte(pci, CFGHSTCFG, tmp &~ ctrl->ensmbirq);
	}
}

static inline void smb_main_irq_enable(struct zxsmb_ctrl *ctrl)
{
	struct pci_dev *pci = ctrl->pci;
	u8 tmp;
	if (pci) {
		pci_read_config_byte(pci, CFGHSTCFG, &tmp);
		zx_smb_printk("Before enable, CFGHSTCFG config is 0x%02x\n",tmp);
		pci_write_config_byte(pci, CFGHSTCFG, tmp | ctrl->ensmbirq);
	}
}

static inline void smb_clear_host_status(unsigned short smba)
{
	u8 tmp;
	tmp = zx_inb_p(smba + SMBHSTSTS);
	zx_outb_p(tmp,smba + SMBHSTSTS);
	return;
}

/* Return -1 on error, 0 on success */
static int zxsmb_transaction(struct zxsmb_ctrl *ctrl)
{
	unsigned short smba = ctrl->smba;
	u8 tmp = 0;
	int timeout = 0;

	/* Make sure the SMBus host is ready to start transmitting */
	if ((tmp = zx_inb_p(smba + SMBHSTSTS)) & 0x1F) {
		zx_outb_p(tmp, smba + SMBHSTSTS);
		if ((tmp = zx_inb_p(smba + SMBHSTSTS)) & 0x1F) {
			dev_dbg(ctrl->dev,"SMBus reset failed! "
			     "(0x%02x)\n", tmp);
			return -1;
		}
	}
	
	ctrl->trans_ret_code = 0;
	
	/* Start the transaction by setting bit 6 */
	tmp = HST_START | ctrl->size;
	
	if (ctrl->pec)
		tmp |= HST_PEC_EN;
	else 
		tmp &= (~ HST_PEC_EN);
	
	if (ctrl->irq_req_success == true)
		tmp |= HST_CMPLT_EN;

	zx_smb_printk("Before transfer, the Rx02 = 0x%02x \n", tmp);
	
	zx_outb_p(tmp, smba + SMBHSTCTL);

	/* If interrupt enabled, this should got execute */
	if (likely(ctrl->irq_req_success == true)){
		/* We will always wait for a fraction of a second */
		
		timeout = wait_event_interruptible_timeout(ctrl->waitq, 
					(zx_inb_p(smba + SMBHSTSTS) & HST_BUSY) == 0,
					msecs_to_jiffies(MAX_TIMEOUT));
		/* If the SMBus is still busy, we give up */
		if (timeout == 0) {
			zx_smb_printk("%s SMBus timeout!\n",__FUNCTION__);
			ctrl->trans_ret_code  = -ETIMEDOUT;
		}
	} else {
		do {
			msleep(1);
			tmp = zx_inb_p(smba + SMBHSTSTS);
		} while ((tmp & HST_BUSY) && (timeout++ < MAX_TIMEOUT));
		if (timeout >= MAX_TIMEOUT) {
			zx_smb_printk("%s SMBus timeout!\n",__FUNCTION__);
			ctrl->trans_ret_code = -ETIMEDOUT;
		}

		if (tmp & HST_PEC_ERR) {
			ctrl->trans_ret_code = -EBADMSG;
			zx_smb_printk("PEC err!\n");
		}
		
		if (tmp & HST_FAIL_TRANS) {
			ctrl->trans_ret_code = -EINVAL;
			zx_smb_printk("Transaction failed!\n");
		}
		if (tmp & HST_BUS_CLSI) {
			ctrl->trans_ret_code = -EAGAIN;
			zx_smb_printk("Bus collision!\n");
		}
		if (tmp & HST_DEV_ERR) {
			ctrl->trans_ret_code = -EAGAIN;
			if (!((ctrl->size == ZX_QUICK && !ctrl->read_write) ||
				(ctrl->size == ZX_BYTE && ctrl->read_write) ||
				ctrl->size == ZX_READ_BANK_ADDR))
				zx_smb_printk("Transaction error! "
				"About to enter retransmission ... \n");
		}
		/* Resetting status register */
		if (tmp & 0x1F)
			zx_outb_p(tmp, smba + SMBHSTSTS);
	}
	zx_smb_printk("ctrl->trans_ret_code = %d\n",ctrl->trans_ret_code);
	return ctrl->trans_ret_code;
}

irqreturn_t zxsmb_irq_handle(int irq, void *dev_id)
{
	u8 status,tmp;
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)dev_id;
	unsigned short smba = ctrl->smba;

	smb_main_irq_disable(ctrl);
	
	/* means dual socket and this is slave */
	if(0 == strcmp(ctrl->plat_name,"CHX002")){
		if (ctrl->is_master == false && ctrl->is_dual_socket) {
			tmp = zx_inb_p(ctrl->master_acpi_ioba + GPSCI_STS);
			zx_outb_p(tmp & VPI2SB_SCISLV_STS, ctrl->master_acpi_ioba + GPSCI_STS); 
		}
	}	
	status = zx_inb_p(smba + SMBHSTSTS);
	if (status & (HST_PEC_ERR | HST_DEV_ERR | HST_BUS_CLSI | HST_FAIL_TRANS | HST_CMD_CMPLET)) { 
		if (status & HST_CMD_CMPLET) {
			if (status & HST_PEC_ERR) {
				zx_smb_printk("SMB Host PEC Error, status = 0x%x!!!\n",status);
				ctrl->trans_ret_code = -EBADMSG;
			}else{
				ctrl->trans_ret_code = 0;
			}	
		} else {		
			if (status & HST_BUS_CLSI) {
				ctrl->trans_ret_code = -EAGAIN;
				zx_smb_printk("Bus Collision, status = 0x%x!!!\n",status);
			}
			if (status & HST_FAIL_TRANS) {
				ctrl->trans_ret_code = -EINVAL;
				zx_smb_printk("Failed Bus Transaction, status = 0x%x!!!\n",status);
			}			
			if (status & HST_DEV_ERR) {
				ctrl->trans_ret_code = -EAGAIN;
				zx_smb_printk("Device Error, status = 0x%x!!!\n",status);
			} else{
				ctrl->trans_ret_code = -EAGAIN;
				zx_smb_printk("Unexpected host interrupt, status = 0x%x!!!\n",status);
			}	
		}
		smb_clear_host_status(smba);
		wake_up(&ctrl->waitq);
	}
#ifdef CONFIG_I2C_SMBUS
	/* Check if HOST alert event occurred */
	if (ctrl->caps & FEATURE_ALERT) {
		status = zx_inb_p(smba + SMBHSTALERTBANK);
		if (status & HST_ALERT_STS) {
			if (ctrl->alert_processing != true) {
				ctrl->alert_processing = true;
				smb_hst_alert_irq_disable(smba);
				/* Handle alert request, once handle failed, 
				   this function will be disabled */
				i2c_handle_smbus_alert(ctrl->ara_client);
				smb_clear_alert_status(smba);
			}
		} else {
			/* renable alert irq until ALERT# push up again */
			ctrl->alert_processing = false;
			smb_hst_alert_irq_enable(smba);
		}
	}	
#endif
	smb_main_irq_enable(ctrl);
	return IRQ_HANDLED;
}

static int zxsmb_smbus_xfer(struct i2c_adapter *adap, u16 addr,
						unsigned short flags, char read_write, u8 command,
						int size, union i2c_smbus_data *data)
{
	int i;
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)i2c_get_adapdata(adap);
	unsigned short smba = ctrl->smba;
	u8 len;
	u8 reg8;
	u8 BankSel;
	bool pec = false;
	int ret;
	
	/* Get Semaphore before transfer */
	reg8 = zx_inb_p(smba + SMBHSTSTS);
	reg8 |= (1<<6);
	zx_outb_p(reg8, smba + SMBHSTSTS);
	reg8 = zx_inb_p(smba + SMBHSTSTS);
	if(reg8 & (1<<6)){
		dev_err(ctrl->dev,"Have not got semaphoren\n");
		return -EBUSY;
	}
	
	if((flags & I2C_CLIENT_PEC)  && (size != I2C_SMBUS_QUICK ))
		pec = true;
	else
		pec = false;
							
	zx_smb_printk("addr  = 0x%02x \n", addr);
	if(pec)
		zx_smb_printk("current transfer with PEC \n");
	zx_smb_printk("read_write = %d \n", read_write);
	zx_smb_printk("command = 0x%02x \n", command);
	zx_smb_printk("size = 0x%02x \n", size);


	if (!((size == I2C_SMBUS_QUICK) || ((size == I2C_SMBUS_BYTE) && (read_write == I2C_SMBUS_WRITE)))){
		if(data == NULL){
			zx_smb_printk("data is NULL pointer");
			return -EINVAL;
		}
	}

	switch (size) {
	case I2C_SMBUS_QUICK:
		size = ZX_QUICK;
		break;
	
	case I2C_SMBUS_BYTE:
		size = ZX_BYTE;
		if(read_write == I2C_SMBUS_WRITE)
			zx_outb_p(command, smba + SMBHSTCMD);	
		else if((addr == 0x36) && (ctrl->caps & FEATURE_BANK_SET_READ))
			size = ZX_READ_BANK_ADDR;
		break;
		
	case I2C_SMBUS_BYTE_DATA:
		size = ZX_BYTE_DATA;
		zx_outb_p(command, smba + SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			zx_outb_p(data->byte, smba + SMBHSTDAT0);
			if ((addr == 0x36 || addr == 0x37) && (ctrl->caps & FEATURE_BANK_SET_READ))
				size = ZX_SET_BANK_ADDR;
		}	
		break;
		
	case I2C_SMBUS_PROC_CALL:
		if (read_write == I2C_SMBUS_READ) {
			dev_info(ctrl->dev,"I2C_SMBUS_PROC_CALL read is Reserved\n");
			return -EINVAL;
		}
		zx_outb_p(command, smba + SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			zx_outb_p(data->word & 0xff, smba + SMBHSTDAT0);
			zx_outb_p((data->word & 0xff00) >> 8, smba + SMBHSTDAT1);
		}
		size = ZX_PROC_CALL;
		break;
		
	case I2C_SMBUS_WORD_DATA:
		zx_outb_p(command, smba + SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			zx_outb_p(data->word & 0xff, smba + SMBHSTDAT0);
			zx_outb_p((data->word & 0xff00) >> 8, smba + SMBHSTDAT1);
		}
		size = ZX_WORD_DATA;
		break;
	
	case I2C_SMBUS_BLOCK_PROC_CALL:   
		if (read_write == I2C_SMBUS_READ)
			goto exit_unsupported;
		len = data->block[0];
		if (len > I2C_SMBUS_BLOCK_MAX)
			len = I2C_SMBUS_BLOCK_MAX;
		zx_outb_p(len+2, smba + SMBHSTDAT0);
		zx_outb_p(32-len, smba + SMBHSTDAT1);
		zx_inb_p(smba + SMBHSTCTL); /* Reset SMBBLKDAT */
		zx_outb_p(command, smba + SMBBLKDAT);
		zx_outb_p(len, smba + SMBBLKDAT);
		for (i = 1; i <= len; i++)
			zx_outb_p(data->block[i], smba + SMBBLKDAT);
		size = ZX_UNIVERSAL;
		break;

	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (!(ctrl->caps & FEATURE_I2CBLOCK))
			goto exit_unsupported;
		if (read_write == I2C_SMBUS_READ)
			zx_outb_p(data->block[0], smba + SMBHSTDAT0);  
		/* Reset SMBBLKDAT */
		zx_inb_p(smba + SMBHSTCTL);  
		zx_outb_p(command, smba + SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			len = data->block[0];
			if (len > I2C_SMBUS_BLOCK_MAX)
				len = I2C_SMBUS_BLOCK_MAX;
			zx_outb_p(len, smba + SMBHSTDAT0);  
			zx_outb_p(0, smba + SMBHSTDAT1);
			for (i = 1; i <= len; i++)
				zx_outb_p(data->block[i], smba + SMBBLKDAT);
		}
		size = ZX_I2C_BLOCK_DATA;
		break;	
		
	case I2C_SMBUS_BLOCK_DATA:    
		/* Reset SMBBLKDAT */
		zx_inb_p(smba + SMBHSTCTL);  
		zx_outb_p(command, smba + SMBHSTCMD);
		if (read_write == I2C_SMBUS_WRITE) {
			len = data->block[0];
			if (len > I2C_SMBUS_BLOCK_MAX)
				len = I2C_SMBUS_BLOCK_MAX;
			zx_outb_p(len, smba + SMBHSTDAT0);  
			zx_outb_p(0, smba + SMBHSTDAT1);
			for (i = 1; i <= len; i++)
				zx_outb_p(data->block[i], smba + SMBBLKDAT);
		}
		size = ZX_BLOCK_DATA;
		break;	
	default: 
		goto exit_unsupported;
	}

	zx_outb_p(((addr & 0x7f) << 1) | read_write, smba + SMBHSTADD);
	ctrl->addr = addr;
	ctrl->size = size;
	ctrl->read_write = read_write;
	ctrl->pec = pec;

	ret = zxsmb_transaction(ctrl);
	if (ret == 0){
		zx_smb_printk("SMB transfer success\n");
	}else if(ret < 0){
		zx_smb_printk("SMB transfer fail,ret = %d\n",ret);
		return ret;
	}else{
		zx_smb_printk("undefined ret code, ret = %d\n",ret);
		return -1;
	}
	
	if ((read_write == I2C_SMBUS_WRITE) || (size == ZX_QUICK)) {
		if (unlikely(size == ZX_PROC_CALL))
			goto prepare_read;
		return 0;
	}

prepare_read:
	switch (size) {
	case ZX_BYTE:
	case ZX_BYTE_DATA:
		data->byte = zx_inb_p(smba + SMBHSTDAT0);
		break;
	
	case ZX_READ_BANK_ADDR:
		data->byte = zx_inb_p(smba + SMBHSTDAT0);
		BankSel = zx_inb_p(smba + SMBHSTALERTBANK);
		if (BankSel & HST_BANK_SEL) {
			zx_smb_printk(KERN_ERR "Bank_0 selected\n");
			zx_outb_p(BankSel | HST_BANK_SEL, smba + SMBHSTALERTBANK);
		} else 
			zx_smb_printk(KERN_ERR "Bank_1 selected\n");
		break;
		
	case ZX_PROC_CALL:
	case ZX_WORD_DATA:
		data->word = zx_inb_p(smba + SMBHSTDAT0) + (zx_inb_p(smba + SMBHSTDAT1) << 8);
		break;
	
	case ZX_I2C_BLOCK_DATA:
	case ZX_BLOCK_DATA:
		data->block[0] = zx_inb_p(smba + SMBHSTDAT0);
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX)
			data->block[0] = I2C_SMBUS_BLOCK_MAX;
		/* Reset SMBBLKDAT */
		zx_inb_p(smba + SMBHSTCTL);
		for (i = 1; i <= data->block[0]; i++) {
			data->block[i] = zx_inb_p(smba + SMBBLKDAT);
		}
		break;
	case ZX_UNIVERSAL:
		zx_inb_p(smba + SMBHSTCTL); /* Reset SMBBLKDAT */
		data->block[0] = zx_inb_p(smba + SMBBLKDAT);
		for (i = 1; i <= data->block[0]; i++)
			data->block[i] = zx_inb_p(smba + SMBBLKDAT);
		break;
	}

	return 0;

exit_unsupported:
	dev_err(ctrl->dev,"Unsupported command invoked! (0x%02x),addr %x\n",size, ctrl->addr);
	return -1;
}

static u32 zxsmb_func(struct i2c_adapter *adapter)
{
	u32 func = I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
			 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
			 I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_I2C;
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)i2c_get_adapdata(adapter);

	if (ctrl->caps & FEATURE_I2CBLOCK)
		func |= I2C_FUNC_SMBUS_I2C_BLOCK;
	func |= I2C_FUNC_SMBUS_PROC_CALL;
	func |= I2C_FUNC_SMBUS_BLOCK_PROC_CALL;
	func |= I2C_FUNC_SMBUS_PEC;

	return func;
}

/***
 * func: zxsmb_master_xfer
 * desp: use size=ZX_UNIVERSAL handle iic protocol interface .master_xfer
 * note: for now, only support 1~2 msgs
 *       1 msg for write or read
 *       2 msgs for first write , then read with restart
 * return: 1-success, Other situation will return -EOPNOTSUPP
 */
static int zxsmb_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)i2c_get_adapdata(adap);
	unsigned short smba = ctrl->smba;
	unsigned char count, i, tmp = MAX_I2C_LEN_SURPPORT;
	
	tmp = (ctrl->caps & FEATURE_ENLARGE_BUFFER) ? (tmp + 2) : tmp;
	if(msgs->len > tmp) {
		dev_err(ctrl->dev,"Access %d-bytes at one time, "
		"more than surpported %d\n", msgs->len, tmp);
		goto exit_unsupported;
	}
	
	/* Reset SMBBLKDAT */
	zx_inb_p(smba + SMBHSTCTL);
	zx_outb_p(0, smba + SMBHSTCMD);
	
	ctrl->size = ZX_UNIVERSAL;
	ctrl->addr = msgs->addr;
	ctrl->read_write = (msgs->flags & I2C_M_RD) ? I2C_SMBUS_READ : I2C_SMBUS_WRITE;
	ctrl->pec = false;   //I2C device have no PEC
	
	count = msgs->len;

	if (num == 1) {
		if (ctrl->read_write == I2C_SMBUS_WRITE) {
			zx_outb_p(count, smba + SMBHSTDAT0);
			zx_outb_p(0, smba + SMBHSTDAT1);
			for (i = 0; i < count; i ++)
				zx_outb_p(msgs->buf[i], smba + SMBBLKDAT);
		} else {
			zx_outb_p(0, smba + SMBHSTDAT0);
			zx_outb_p(count, smba + SMBHSTDAT1);
		}
	} else if (num == 2) {
		if (ctrl->read_write == I2C_SMBUS_WRITE) {
			for (i = 0; i < count; i ++)
				zx_outb_p(msgs->buf[i], smba + SMBBLKDAT);
			zx_outb_p(count, smba + SMBHSTDAT0);
		} else {
			dev_err(ctrl->dev,"For 2 msgs access, first one should write mode\n");
			goto exit_unsupported;
		}
		/* prepare handle second msg */
		msgs ++;
		
		ctrl->read_write = (msgs->flags & I2C_M_RD) ? I2C_SMBUS_READ : I2C_SMBUS_WRITE;
		count = msgs->len;
		if(count > MAX_I2C_LEN_SURPPORT) {
			dev_err(ctrl->dev,"Access %d-bytes at one time, "
		"more than surpported %d\n", msgs->len, MAX_I2C_LEN_SURPPORT);
			goto exit_unsupported;
		}
		if (ctrl->read_write == I2C_SMBUS_WRITE) {
			dev_err(ctrl->dev,"For 2 msgs access, second one should read mode\n");
			goto exit_unsupported;
		} else {
			ctrl->read_write = I2C_SMBUS_WRITE;
			zx_outb_p(count, smba + SMBHSTDAT1);
		}
	} else {
			dev_err(ctrl->dev,"Request %d msgs, only surpported 1~2.\n",num);
			goto exit_unsupported;
	}
	
	zx_outb_p(((ctrl->addr & 0x7f) << 1) | ctrl->read_write, smba + SMBHSTADD);
	
	if (zxsmb_transaction(ctrl))
		return ctrl->trans_ret_code;
	
	if ((ctrl->read_write == I2C_SMBUS_WRITE) && num == 1)
		return 1;
	
	/* Reset SMBBLKDAT */
	zx_inb_p(smba + SMBHSTCTL);
	
	for (i = 0; i < count; i ++)
		msgs->buf[i] = zx_inb_p(smba + SMBBLKDAT);

	return num;
	
exit_unsupported:
	return -EOPNOTSUPP;
}

static const struct i2c_algorithm smbus_algorithm = {
	.master_xfer    = zxsmb_master_xfer,
	.smbus_xfer     = zxsmb_smbus_xfer,
	.functionality  = zxsmb_func,
};

static int zxsmb_remove(struct platform_device *pdev)
{
	struct zxsmb_ctrl *ctrl = platform_get_drvdata(pdev);

	if (ctrl) {
		if (ctrl->irq_req_success) {
			smb_hst_alert_irq_disable(ctrl->smba);
			smb_main_irq_disable(ctrl);
			free_irq(ctrl->irq, ctrl);
		}
		if (ctrl->pci != NULL) {
			if (NULL != &(ctrl->adapter))
				i2c_del_adapter(&(ctrl->adapter));
			if (ctrl->smba)
				release_region(ctrl->smba, SMB_MMIO_SIZE);
			ctrl->pci = NULL;
		}
	}
	platform_set_drvdata(pdev, NULL);
	return 0;
}

/***
 * func: get host's MMIO base address
 */
static int zx_get_host_smba(struct zxsmb_ctrl *ctrl)
{
	unsigned short smba;
	
	pci_read_config_word(ctrl->pci, ctrl->base_reg, &smba);
	if ((smba & 0xfff0) == 0 || (smba & 0x1) == 0) 
		return -ENODEV;
	
	smba &= 0xfff0;
	
	if (!request_region(smba, SMB_MMIO_SIZE, ZX_SMBUS_NAME)) {
		dev_info(ctrl->dev,"SMBus region 0x%x already in use!\n",smba);
		return -ENODEV;
	}
	
	ctrl->smba = smba;
	
	return 0;
}
/***
 * func: init interrupt related registers
 *   default support: trans completion, bus collision, device err
 *           support: alert
 *           support: PEC   
 */
static void zxsmb_interrupt_init(struct platform_device *pdev)
{
	unsigned char tmp;
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)platform_get_drvdata(pdev);
	unsigned short smba = ctrl->smba;

	pci_read_config_byte(ctrl->pci, CFGHSTCFG, &tmp);
	pci_write_config_byte(ctrl->pci, CFGHSTCFG, tmp | ctrl->ensmbirq | SMBISEL);

	smb_clear_host_status(smba);

	/* for dual-socket, we need make sure VPI2SB_SCISLV_EN enabled */
	if (ctrl->is_dual_socket && ctrl->is_master){
		if(  0!=strcmp(ctrl->plat_name,"CHX003") &&  0!=strcmp(ctrl->plat_name,"CHX004")){
			tmp = zx_inb_p(ctrl->master_acpi_ioba + GPSCI_EN);
			if ((tmp & VPI2SB_SCISLV_EN) == 0) {
				zx_outb_p(tmp | VPI2SB_SCISLV_EN, ctrl->master_acpi_ioba + GPSCI_EN);
				if ((zx_inb_p(ctrl->master_acpi_ioba + GPSCI_EN) & VPI2SB_SCISLV_EN) == 0) {
					dev_err(ctrl->dev, "Master's VPI2SB_SCISLV_EN failed enable!!!\n");
					ctrl->irq_req_success = false;
					return;
				}
			}
		}
	}

	if ((ctrl->irq == SSCI_DISABLED) || (ctrl->irq == SSCI_RESERVED)) {
		dev_err(ctrl->dev,"ACPI IRQ select disabled or error. "
				"ctrl->irq = %d\n",ctrl->irq);
	} else {
		if (request_irq(ctrl->irq, zxsmb_irq_handle, 
						 IRQF_SHARED,
						 pdev->name, ctrl)) {
			dev_err(ctrl->dev, "SMBus IRQ%d allocation failed.\n", ctrl->irq);
			ctrl->irq_req_success = false;
		} else {
			ctrl->irq_req_success = true;
	
#ifdef CONFIG_I2C_SMBUS
			/* enable host alert interrupt function */
			if (ctrl->caps & FEATURE_ALERT) {
				struct i2c_smbus_alert_setup setup;
				
				#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,14,243)
				setup.alert_edge_triggered = 0;/* level mode */
				#endif
				
				setup.irq = 0;/* disable irq mode */

				#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,7,0)
		        	ctrl->ara_client = i2c_new_smbus_alert_device(&ctrl->adapter, &setup);
			    #else 
		        	ctrl->ara_client = i2c_setup_smbus_alert(&ctrl->adapter, &setup);
			    #endif
				/* TODO: add some NETLINK init codes */
				smb_hst_alert_irq_enable(smba);
			}
#endif
		}
	}
	return;
}

static int zxsmb_probe(struct platform_device *pdev)
{
	int error;
	struct zxsmb_ctrl *ctrl = (struct zxsmb_ctrl *)platform_get_drvdata(pdev);
	struct i2c_adapter *adap = NULL;

	error = zx_get_host_smba(ctrl);
	if (error < 0)
		return error;
	
	strcpy(ctrl->adapter.name,ZX_SMBUS_NAME);
	ctrl->adapter.algo = &smbus_algorithm;
	ctrl->adapter.retries = 2;
	ctrl->adapter.timeout = msecs_to_jiffies(5);
	
	adap = &ctrl->adapter;
	i2c_set_adapdata(adap, ctrl);
	adap->owner = THIS_MODULE;
	adap->dev.parent = &pdev->dev;
	error = i2c_add_adapter(adap);
	if (unlikely(error)) {
		dev_err(ctrl->dev, "Failure adding adapter error=%d\n", error);
		release_region(ctrl->smba, SMB_MMIO_SIZE);
		return error;
	}

	zxsmb_interrupt_init(pdev);
	init_waitqueue_head(&ctrl->waitq);
	
	dev_info(ctrl->dev,"driver(version %s) load success "
		"at /dev/i2c-%d.\n",DRIVER_VERSION, adap->nr);
	return 0;
}


static struct platform_driver zx_driver = {
	.probe  = zxsmb_probe,
	.remove = zxsmb_remove,
	.id_table = zxsmb_host_ids,
	.driver = {
		.name  = ZX_SMBUS_NAME,
		.owner = THIS_MODULE,
	},
};

static void zxsmb_init_continue(struct zxsmb_ctrl  *ctrl,
							unsigned char clock_sel_en,
							unsigned char clock_sel,
							unsigned char host_index,
							unsigned char table_index)
{
	u8 tmp;
	unsigned char clock_val = clock_freq[host_index];
	
	ctrl->plat_name = plat_info_table[table_index].name;
	ctrl->caps = plat_info_table[table_index].caps;
	
	/* check if frequency clock need be adjust */
	if (clock_val >= 10 && clock_val <= 100) {   
		/* Host Clock 0/1 Select Enable */
		pci_read_config_byte(ctrl->pci, CFGHSTCSE, &tmp);
		pci_write_config_byte(ctrl->pci, CFGHSTCSE, tmp | clock_sel_en);
		
		/* calculate appropriate register value */
		tmp = calc_clock_value(clock_val);
		pci_write_config_byte(ctrl->pci, clock_sel, tmp);
		dev_info(&ctrl->pci->dev,"SMBus Host%d's clock frequency has been changed "
				 "to %d kHz.\n", host_index, clock_val);
	}
	
	/* if not set the alert slave address, then disable this capability */
	if(alert_func == 0)
		ctrl->caps &= ~FEATURE_ALERT;

	return;
}
							
/***
 * Detect hosts by vendor id device id and version number.
 * After detected the host, do some initialization
 */
static int zxsmb_device_detect_init(struct device *dev, void *data)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct zxsmb_ctrl  *ctrl;
	u8 index = 0, rev_id;
	u8 platform_id_num = sizeof(plat_info_table)/sizeof(plat_info_table[0]);

	while (index < platform_id_num) {
		pci_read_config_byte(pci, CFGRevID, &rev_id);
		if (pci->vendor == plat_info_table[index].vendor_id
		 && pci->device == plat_info_table[index].dev_id
		 && rev_id == plat_info_table[index].rev_id) {
			/* get host's base information */
			ctrl = (struct zxsmb_ctrl *)kzalloc(sizeof(struct zxsmb_ctrl), GFP_KERNEL);
			if (!ctrl)
				goto out;
			ctrl->pci = pci;
			ctrl->base_reg = CFGHST0BA;
			ctrl->smben    = SMB0EN;
			ctrl->ensmbirq = ENSMB0IRQ;
			zxsmb_init_continue(ctrl, EN_HST0_CLK, CFGHST0CS, hosts.count, index);
			
			hosts.ctrl[hosts.count] = ctrl;
			hosts.count++;
			
			if (ctrl->caps & FEATURE_TWO_HOST) {
				ctrl = (struct zxsmb_ctrl *)kzalloc(sizeof(struct zxsmb_ctrl), GFP_KERNEL);
				if (!ctrl)
					goto out;
				ctrl->pci = pci;
				ctrl->base_reg = CFGHST1BA;
				ctrl->smben = SMB1EN;
				ctrl->ensmbirq = ENSMB1IRQ;
				zxsmb_init_continue(ctrl, EN_HST1_CLK, CFGHST1CS, hosts.count, index);
				
				hosts.ctrl[hosts.count] = ctrl;
				hosts.count++;
			}
			break;
		}
		index++;
	}
	return 0;
out:
	dev_err(&pci->dev,"SMBus: kmalloc zxsmb_ctrl failed!\n");
	return -1;
}

static int __init zxsmb_init(void)
{
	int i;
	int ret;
	u8  irq;
	unsigned short acpiioba;
	struct platform_device *pdev;
	struct zxsmb_ctrl  *ctrl;

	hosts.count = 0;

	/* Find the supportted platform we currently working on */
	bus_for_each_dev(&pci_bus_type, NULL, NULL, zxsmb_device_detect_init);
	if (hosts.count > MAX_HOSTS_SURPPORT || hosts.count == 0) {
		printk(KERN_ERR "SMBus host's number is #%d,"
			" there is something wrong!\n",hosts.count);
		return 0;
	}

	/* register platform_device */
	for (i = 0; i < hosts.count; i++) {
		ctrl = hosts.ctrl[i];
		pdev = platform_device_alloc(ctrl->plat_name, i);
		if (!pdev) {
			dev_err(&ctrl->pci->dev,"platform_device_alloc failed!\n");
			goto out1;              
		}

		ctrl->dev = &pdev->dev;

		/* dual socket share common irq number */
		if (i == 0)
			pci_read_config_byte(ctrl->pci, PM_INT_SEL_REG, &irq);
		ctrl->irq = irq & 0x0F;

		platform_set_drvdata(pdev, (void*)ctrl);
		ret = platform_device_add(pdev);
		if (ret != 0) {
			dev_err(ctrl->dev,"platform_device_add failed!\n");
			goto out2;
		}
		
		hosts.pdev[i] = pdev;

		/* only for dual socket */
		if (((ctrl->caps & FEATURE_TWO_HOST) == 0 && hosts.count > 1)
		 || hosts.count > 2) {
			ctrl->is_dual_socket = true;
			if (i == 0) {
				ctrl->is_master = true;   /* default the first found host is master */
				if (pci_read_config_word(ctrl->pci, PM_BASE, &acpiioba)
					|| ! (acpiioba & 0x0001)) {
					dev_err(ctrl->dev,"Cannot read PMIO Base address\n");
					ret =  -ENODEV;
					goto out3;
				}
				acpiioba &= 0xfff0;
			} else ctrl->is_master = false;
			/* slave socket need operation master's apci mmio space to clear status */
			ctrl->master_acpi_ioba = acpiioba;
		}
	}

	/* register platform_driver */
	ret = platform_driver_register(&zx_driver);
	if (ret != 0) {
		printk("platform_driver_register failed!\n");
		goto out3;
	}
	return ret;
	
out3:
	for (i = 0; i < hosts.count; i++) {
		if (hosts.pdev[i])
			platform_device_del(hosts.pdev[i]);	
	}
	
out2:
	for (i = 0; i < hosts.count; i++) {
		if (hosts.pdev[i]) {
			platform_device_put(hosts.pdev[i]);
			hosts.pdev[i] = NULL;
		}
	}
	
out1: 
	for (i = 0; i < hosts.count; i++) {
		if (hosts.ctrl[i]) {
			kfree(hosts.ctrl[i]);
			hosts.ctrl[i] = NULL;
		}
	}
	return ret;
}


static void __exit zxsmb_exit(void)
{
	int i;
	platform_driver_unregister(&zx_driver);
	for (i = 0; i < hosts.count; i++) {
		if (*hosts.pdev[i]->name) {
			if(hosts.pdev[i]){
				platform_device_unregister(hosts.pdev[i]);
				hosts.pdev[i] = NULL;
			}
			
			if(hosts.ctrl[i]){
				kfree(hosts.ctrl[i]);
				hosts.ctrl[i] = NULL;
			}
		}
	}
	printk(KERN_INFO "zhaoxin-smbus driver(version %s) unloaded.\n",DRIVER_VERSION);
}

MODULE_AUTHOR("www.zhaoxin.com");
MODULE_DESCRIPTION("ZHAOXIN SMBus driver");
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

module_init(zxsmb_init);
module_exit(zxsmb_exit);
