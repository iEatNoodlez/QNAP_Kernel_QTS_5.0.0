#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/mod_devicetable.h>
#include <linux/log2.h>
#include <linux/bitops.h>
#include <linux/jiffies.h>
#include <linux/property.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>

#include "qnap_pcie_ep.h"
#include "qnap_pcie_ep_cpld.h"

/*
 * Specs often allow 5 msec for a page write, sometimes 20 msec;
 * it's important to recover from write timeouts.
 */
static unsigned write_timeout = 25;
module_param(write_timeout, uint, 0);
MODULE_PARM_DESC(write_timeout, "Time (in ms) to try writes (default 25)");

/*
 * Both reads and writes fail if the previous write didn't complete yet. This
 * macro loops a few times waiting at least long enough for one entire page
 * write to work while making sure that at least one iteration is run before
 * checking the break condition.
 *
 * It takes two parameters: a variable in which the future timeout in jiffies
 * will be stored and a temporary variable holding the time of the last
 * iteration of processing the request. Both should be unsigned integers
 * holding at least 32 bits.
 */
#define loop_until_timeout(tout, op_time)				\
	for (tout = jiffies + msecs_to_jiffies(write_timeout), op_time = 0; \
	     op_time ? time_before(op_time, tout) : true;		\
	     usleep_range(1000, 1500), op_time = jiffies)

struct qnap_cpld_i2c_data {
	ssize_t (*read_func)(struct qnap_cpld_i2c_data *, char *, unsigned int, size_t);
	ssize_t (*write_func)(struct qnap_cpld_i2c_data *, const char *, unsigned int, size_t);

	/*
	 * Lock protects against activities from other Linux tasks,
	 * but not from changes by other I2C masters.
	 */
	struct mutex mutex_lock;
	spinlock_t spin_lock;

	unsigned write_max;

	/*
	 * Some chips tie up multiple I2C addresses; dummy devices reserve
	 * them for us, and we'll use them with SMBus calls.
	 */
	struct i2c_client *client[];
};

struct cpld_rw_params
{
	unsigned char rw_mode;
	unsigned char reg;
	unsigned char value;
};

struct cpld_cable_status_params
{
	unsigned char tbt_m2_modules_1;
	unsigned char tbt_m2_modules_2;
	unsigned char usb_m2_modules_1;
	unsigned char usb_m2_modules_2;
};

struct cpld_host_status_params
{
	unsigned char m2_modules_1;
	unsigned char m2_modules_2;
};

struct cpld_system_off_params
{
	unsigned char rw_mode;
	unsigned char run_status;
};

#define CPLD_REG_RW		_IOWR('c', 0x01, struct cpld_rw_params)
#define CPLD_CABLE_STATUS	_IOR('c', 0x02, struct cpld_cable_status_params)
#define CPLD_HOST_STATUS	_IOR('c', 0x03, struct cpld_host_status_params)
#define CPLD_SYSTEM_OFF_SET_GET	_IOWR('c', 0x04, struct cpld_system_off_params)

extern unsigned char cpld_i2c_ready;
extern unsigned char tbt1_usb_insert_flag;
extern unsigned char tbt2_usb_insert_flag;
extern unsigned char tbt1_tbt_insert_flag;
extern unsigned char tbt2_tbt_insert_flag;
extern unsigned char tbt1_power_status;
extern unsigned char tbt2_power_status;
extern char tr_power_status;

static int cpld_read(void *priv, unsigned int off, void *val, size_t count);
static int cpld_write(void *priv, unsigned int off, void *val, size_t count);
int qnap_cpld_read(unsigned int off, void *val, size_t count);
int qnap_cpld_write(unsigned int off, void *val, size_t count);

struct qnap_cpld_i2c_data *g_cpld;
static struct proc_dir_entry *proc_cpld_debug_root = NULL;
static uint g_mode = 0;

static char *mode_name[] =
{
	"NULL",
};

static void setting_mode(uint cmd)
{
	switch(cmd) {
	case 1:
		printk("hello word !!!\n");
		break;
	case 2:
		printk("clean all hdd status\n");
		cpld_hdd_status_off();
		break;
	default:
		printk("QNAP CPLD Debug Control function error\n");
		break;
	}

	return;
};

static ssize_t cpld_debug_read_proc(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	int len = 0;

	printk("mode__name[%d] = %s\n", g_mode, mode_name[g_mode]);

	return len;
};

static ssize_t cpld_debug_write_proc(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	unsigned char value[100], cmd[100];
	unsigned int tmp;
	unsigned char wval, cval;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	memset(cmd,0x0,sizeof(cmd));
	sscanf(value, "%s %x\n", &cmd, &wval);
	sscanf(value, "%s %c\n", &cmd, &cval);
	if (strcmp(cmd, "erp") == 0) {
		if((cval >= 'a') && (cval <= 'z')) {
			if(cpld_erp_onoff(cval) < 0)
				printk("value fail !! cval = 0x%x, ascii = %c\n", cval, cval);
		} else {
			if(cpld_erp_onoff(wval) < 0)
				printk("value fail !! wval = 0x%x\n", wval);
		}
	} else {
		sscanf(value,"%u\n", &tmp);
		//printk("tmp = %d\n", tmp);
		setting_mode(tmp);
		g_mode = tmp;
	}

	return count;
};

static const struct file_operations qnap_cpld_debug_proc_fileops = {
	.owner			= THIS_MODULE,
	.read			= cpld_debug_read_proc,
	.write			= cpld_debug_write_proc
};

static ssize_t dump_read_reg(struct file *filp, char __user *buffer, size_t count, loff_t *offp)
{
	return 0;
};

static ssize_t dump_write_reg(struct file *filp, const char __user *buffer, size_t count, loff_t *offp)
{
	int len = count;
	unsigned char value[60], cmd;
	unsigned char reg;
	unsigned char rval;
	unsigned char wval;

	if(copy_from_user(value, buffer, len)) {
		return 0;
	}
	value[len]='\0';

	sscanf(value, "%c %x %x\n", &cmd, &reg, &wval);
	printk("=============\n");
	printk("cmd = %c\n", cmd);
	printk("register = 0x%x\n", reg);
	if(cmd == 'W' || cmd == 'w')
		printk("wval = 0x%x\n", wval);
	printk("=============\n\n");

	if(cmd == 'R' || cmd == 'r') {
		rval = 0;
		if(cpld_read(g_cpld, reg, &rval, 1) < 0) {
			printk("CPLD read fail !\n");
			return -1;
		}

		printk("Read reg = 0x%x, rval = 0x%x\n", reg, rval);
	} else if(cmd == 'W' || cmd == 'w') {
		printk("Write reg = 0x%x, wval = 0x%x\n", reg, wval);
		if(cpld_write(g_cpld, reg, &wval, 1) < 0) {
			printk("CPLD write fail !\n");
			return -1;
		}

		mdelay(1);
		rval = 0;
		if(cpld_read(g_cpld, reg, &rval, 1) < 0) {
			printk("CPLD read fail !\n");
			return -1;
		}
		printk("Read reg = 0x%x, rval = 0x%x\n", reg, rval);
	} else {
		printk("cpld regiser debug commmand error !!\n");
	}

	return count;
};

static const struct file_operations dump_reg_fileops = {
	.owner		= THIS_MODULE,
	.read		= dump_read_reg,
	.write		= dump_write_reg
};

static ssize_t cpld_read_smbus(struct qnap_cpld_i2c_data *cpld, char *buf,
				      unsigned int offset, size_t count)
{
	unsigned long timeout, read_time;
	struct i2c_client *client;
	int status;

	client = cpld->client[0];

	/* Smaller eeproms can work given some SMBus extension calls */
	if (count > I2C_SMBUS_BLOCK_MAX) {
		count = I2C_SMBUS_BLOCK_MAX;
	}

	loop_until_timeout(timeout, read_time) {
		status = i2c_smbus_read_i2c_block_data_or_emulated(client,
								   offset,
								   count, buf);
		dev_dbg(&client->dev, "read %zu@%d --> %d (%ld)\n",
				count, offset, status, jiffies);

		if (status == count) {
			return count;
		}
	}

	return -ETIMEDOUT;
}

static ssize_t cpld_write_smbus_byte(struct qnap_cpld_i2c_data *cpld,
					    const char *buf,
					    unsigned int offset, size_t count)
{
	unsigned long timeout, write_time;
	struct i2c_client *client;
	ssize_t status = 0;

	client = cpld->client[0];

	loop_until_timeout(timeout, write_time) {
		status = i2c_smbus_write_byte_data(client, offset, buf[0]);
		if (status == 0)
			status = count;

		dev_dbg(&client->dev, "write %zu@%d --> %zd (%ld)\n",
				count, offset, status, jiffies);

		if (status == count)
			return count;
	}

	return -ETIMEDOUT;
}

static int cpld_read(void *priv, unsigned int off, void *val, size_t count)
{
	struct qnap_cpld_i2c_data *cpld = priv;
	char *buf = val;
	//unsigned long flags;

	if (unlikely(!count)) {
		return count;
	}

	/*
	 * Read data from chip, protecting against concurrent updates
	 * from this host, but not from other I2C masters.
	 */
	mutex_lock(&cpld->mutex_lock);
	//spin_lock_irqsave(&cpld->spin_lock, flags);

	while (count) {
		int	status;

		status = cpld->read_func(cpld, buf, off, count);
		if (status < 0) {
			mutex_unlock(&cpld->mutex_lock);
			//spin_unlock_irqrestore(&cpld->spin_lock, flags);
			return status;
		}
		buf += status;
		off += status;
		count -= status;
	}

	mutex_unlock(&cpld->mutex_lock);
	//spin_unlock_irqrestore(&cpld->spin_lock, flags);

	return 0;
}

static int cpld_write(void *priv, unsigned int off, void *val, size_t count)
{
	struct qnap_cpld_i2c_data *cpld = priv;
	char *buf = val;
	//unsigned long flags;

	if (unlikely(!count))
		return -EINVAL;

	/*
	 * Write data to chip, protecting against concurrent updates
	 * from this host, but not from other I2C masters.
	 */
	mutex_lock(&cpld->mutex_lock);
	//spin_lock_irqsave(&cpld->spin_lock, flags);

	while (count) {
		int status;

		status = cpld->write_func(cpld, buf, off, count);
		if (status < 0) {
			mutex_unlock(&cpld->mutex_lock);
			//spin_unlock_irqrestore(&cpld->spin_lock, flags);
			return status;
		}
		buf += status;
		off += status;
		count -= status;
	}

	mutex_unlock(&cpld->mutex_lock);
	//spin_unlock_irqrestore(&cpld->spin_lock, flags);

	return 0;
}

int qnap_cpld_read(unsigned int off, void *val, size_t count)
{
	int ret;

	if (unlikely(!count))
		return -EINVAL;

	ret = cpld_read(g_cpld, off, val, count);

	return ret;
}
EXPORT_SYMBOL(qnap_cpld_read);

int qnap_cpld_write(unsigned int off, void *val, size_t count)
{
	int ret;

	if (unlikely(!count))
		return -EINVAL;

	ret = cpld_write(g_cpld, off, val, count);

	return ret;
}
EXPORT_SYMBOL(qnap_cpld_write);

int cpld_erp_onoff(unsigned char onoff)
{
	unsigned char val;

	if(cpld_read(g_cpld, CPLD_SYS_PWR_CTL, &val, 1) < 0) {
		printk("CPLD read fail !\n");
		return -1;
	}

	if(onoff == 'r') {
		printk("CPLD Reg : 0x%x, Value : 0x0%x\n", CPLD_SYS_PWR_CTL, val);
		printk("Erp Value = 0x%x, %s\n", val, (val & BIT7) ? "Turn-On" : "Turn-Off");
		return 0;
	}

	if(onoff == 0x01)
		val |= BIT7;
	else if(onoff == 0x00)
		val = val & ~BIT7;
	else
		return -1;

	//printk("erp val = 0x%x\n", val);
	if(cpld_write(g_cpld, CPLD_SYS_PWR_CTL, &val, 1) < 0) {
		printk("CPLD write fail !\n");
		return -1;
	}

	return 0;
}

void cpld_hdd_status_off(void)
{
	unsigned char i;
	unsigned char val;

	// clean all hdd status
	for(i=0;i<8;i++) {
		val = 0;
		if(cpld_read(g_cpld, CPLD_HDD_STATUS1 + i, &val, 1) < 0) {
			printk("CPLD read fail !\n");
			return;
		}
		if(val & BIT0) {
			val = val & ~BIT0;
			if(cpld_write(g_cpld, CPLD_HDD_STATUS1 + i, &val, 1) < 0) {
				printk("CPLD write fail !\n");
				return;
			}
		}
	}

	return;
}
EXPORT_SYMBOL(cpld_hdd_status_off);

static int cpld_gpio_open(struct inode *inode, struct file *file)
{
	return 0;
};

static int cpld_gpio_release(struct inode *inode, struct file *file)
{
	return 0;
};

static long cpld_gpio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cpld_rw_params cpld_rw;
	struct cpld_cable_status_params cpld_cable_status;
	struct cpld_host_status_params cpld_host_status;
	struct cpld_system_off_params cpld_system_off;
	unsigned char val = 0;

	switch (cmd) {
	case CPLD_REG_RW:
		if(copy_from_user (&cpld_rw, (struct cpld_rw_params *)arg, sizeof(struct cpld_rw_params)))
			return -EFAULT;

		if(cpld_rw.rw_mode == 0) {
			if(qnap_cpld_read(cpld_rw.reg, &val, 1) < 0) {
				printk("CPLD read fail !\n");
				return -EFAULT;
			}

			cpld_rw.value = val;
			//printk("read reg = 0x%x, val = 0x%x\n", cpld_rw.reg, cpld_rw.value);
			if(copy_to_user ((void *)arg, &cpld_rw, sizeof(struct cpld_rw_params)))
				return -EFAULT;
		} else if(cpld_rw.rw_mode == 1) {
			val = cpld_rw.value;
			//printk("write reg = 0x%x, val = 0x%x\n", cpld_rw.reg, cpld_rw.value);
			if(qnap_cpld_write(cpld_rw.reg, &val, 1) < 0) {
				printk("CPLD write fail !\n");
				return -EFAULT;
			}
		} else {
			printk("cpld read or write mode issue\n");
			return -EFAULT;
		}

		break;
	case CPLD_CABLE_STATUS:
		if(copy_from_user (&cpld_cable_status, (struct cpld_cable_status_params *)arg, sizeof(struct cpld_cable_status_params)))
			return -EFAULT;

		cpld_cable_status.tbt_m2_modules_1 = tbt2_tbt_insert_flag;
		cpld_cable_status.tbt_m2_modules_2 = tbt1_tbt_insert_flag;
		cpld_cable_status.usb_m2_modules_1 = tbt2_usb_insert_flag;
		cpld_cable_status.usb_m2_modules_2 = tbt1_usb_insert_flag;

		if(copy_to_user ((void *)arg, &cpld_cable_status, sizeof(struct cpld_cable_status_params)))
			return -EFAULT;

		break;
	case CPLD_HOST_STATUS:
		if(copy_from_user (&cpld_host_status, (struct cpld_host_status_params *)arg, sizeof(struct cpld_host_status_params)))
			return -EFAULT;

		cpld_host_status.m2_modules_1 = tbt1_power_status;
		cpld_host_status.m2_modules_2 = tbt2_power_status;

		if(copy_to_user ((void *)arg, &cpld_host_status, sizeof(struct cpld_host_status_params)))
			return -EFAULT;

		break;
	case CPLD_SYSTEM_OFF_SET_GET:
                if(copy_from_user (&cpld_system_off, (struct cpld_system_off_params *)arg, sizeof(struct cpld_system_off_params)))
                        return -EFAULT;

		if(cpld_system_off.rw_mode == 0) {
			tr_power_status = cpld_system_off.run_status;
		} else if(cpld_system_off.rw_mode == 1) {
			cpld_system_off.run_status = tr_power_status;
			if(copy_to_user ((void *)arg, &cpld_system_off, sizeof(struct cpld_system_off_params)))
				return -EFAULT;
		} else {
			printk("This is argument issue. rw_mode = 0x%x\n", cpld_system_off.rw_mode);
		}

		break;
	default:
		printk("CPLD ioctl function error\n");
		return -EINVAL;
	}

	return 0;
};

static const struct file_operations qnap_cpld_gpio_ioctl_fops = {
	.owner			= THIS_MODULE,
	.unlocked_ioctl		= cpld_gpio_ioctl,
	.open			= cpld_gpio_open,
	.release		= cpld_gpio_release
};

static struct miscdevice qnap_cpld_gpio_miscdev = {
	.minor			= MISC_DYNAMIC_MINOR,
	.name			= "qcpld",
	.fops			= &qnap_cpld_gpio_ioctl_fops
};

/*
 * i2c_driver function
 */
static int qnap_cpld_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *did)

{
	struct qnap_cpld_i2c_data *cpld;
	int err, ret;
	unsigned char g_ver, g_power_status, g_type_c_status, g_type_c_vbus_status;
	struct proc_dir_entry *mode;

	// procfs
	proc_cpld_debug_root = proc_mkdir("qcpld", NULL);
	if(!proc_cpld_debug_root) {
		printk(KERN_ERR "Couldn't create qnap_cpld folder in procfs\n");
		return -1;
	}

	// create file of folder in procfs
	mode = proc_create("mode", S_IRUGO | S_IXUGO | S_IFREG, proc_cpld_debug_root, &qnap_cpld_debug_proc_fileops);
	if(!mode) {
		printk(KERN_ERR "Couldn't create qnap cpld debug procfs node\n");
		return -1;
	}

	mode = proc_create("reg", S_IRUGO | S_IXUGO | S_IFREG, proc_cpld_debug_root, &dump_reg_fileops);
	if (!mode) {
		printk(KERN_ERR "Couldn't create qnap cpld debug dump procfs node\n");
		return -1;
	}

	/* let's see whether this adapter can support what we need */
	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_READ_BYTE_DATA |
			I2C_FUNC_SMBUS_WRITE_BYTE_DATA)) {
		dev_err(&client->dev,
			"I2C-Adapter doesn't support I2C_FUNC_SMBUS_BYTE_DATA or I2C_FUNC_SMBUS_WRITE_BYTE_DATA\n");
		return -EIO;
	}

	cpld = devm_kzalloc(&client->dev, sizeof(struct qnap_cpld_i2c_data) + sizeof(struct i2c_client *), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	mutex_init(&cpld->mutex_lock);
	spin_lock_init(&cpld->spin_lock);
	cpld->read_func = cpld_read_smbus;
	cpld->write_func = cpld_write_smbus_byte;
	cpld->client[0] = client;

	i2c_set_clientdata(client, cpld);
	g_cpld = cpld;

	//printk("client->adapter = 0x%x, client->addr = 0x%x\n", client->adapter, client->addr);
	printk("QNAP CPLD driver\n");

	/*
	 * Perform a one-byte test read to verify that the
	 * chip is functional.
	 */
	err = cpld_read(cpld, CPLD_VERSION, &g_ver, 1);
	if (err) {
		err = -ENODEV;
		printk("Getting CPLD Version fail!\n");
		goto err_clients;
	} else {
		cpld_i2c_ready = 1;
		printk("CPLD Version : 0x%x\n", g_ver & 0x7f);
	}

#if !defined(CONFIG_PCI_ALPINE_ENDPOINT_DISABLE_INTERRUPT_WITH_GPIO)
#if 1
	if(qnap_cpld_read(CPLD_TBT_HOSTPWR_PWR_BUTT, &g_power_status, 1) < 0) {
		printk("Get CPLD Power register status fail!!\n");
	} else {
		qnap_ep_cpld_power_status_update(g_power_status);
	}
#endif
#if 1
	if(qnap_cpld_read(CPLD_TYPE_C_VBUS_STATUS, &g_type_c_vbus_status, 1) < 0) {
		printk("Get CPLD Type-C Vbus register status fail!!\n");
	} else {
		qnap_ep_cable_vbus_status_update(g_type_c_vbus_status);
	}
#endif
	// getting boot-on cable status from CPLD register
	if(qnap_cpld_read(CPLD_TYPE_C_CON_STATUS, &g_type_c_status, 1) < 0) {
		printk("Get CPLD Type-C register status fail!!\n");
	} else {
		qnap_ep_cable_status_update(g_type_c_status);
	}
#endif

	// miscellaneous
	ret = misc_register(&qnap_cpld_gpio_miscdev);
	if(ret < 0) {
		printk(KERN_ERR "GPIO: misc_register returns %d.\n", ret);
		ret = -EINVAL;
		goto err_clients;
	}

	return 0;

err_clients:
	i2c_unregister_device(cpld->client[0]);
	devm_kfree(&client->dev, cpld);
	remove_proc_entry("mode", proc_cpld_debug_root);
	remove_proc_entry("reg", proc_cpld_debug_root);
	remove_proc_entry("qcpld", NULL);

	return 0;
}

static int qnap_cpld_i2c_remove(struct i2c_client *client)
{
	struct qnap_cpld_i2c_data *cpld;

	cpld = i2c_get_clientdata(client);
	i2c_unregister_device(cpld->client[0]);
	devm_kfree(&client->dev, cpld);
	remove_proc_entry("mode", proc_cpld_debug_root);
	remove_proc_entry("reg", proc_cpld_debug_root);
	remove_proc_entry("qcpld", NULL);
	misc_deregister(&qnap_cpld_gpio_miscdev);

	return 0;
}

static const struct i2c_device_id qnap_cpld_i2c_id[] = {
	{ "qnap_cpld", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, qnap_cpld_i2c_id);

static struct i2c_driver qnap_cpld_i2c_driver = {
	.driver = {
		.name = "qnap_cpld",
	},
	.probe    = qnap_cpld_i2c_probe,
	.remove   = qnap_cpld_i2c_remove,
	.id_table = qnap_cpld_i2c_id,
};
module_i2c_driver(qnap_cpld_i2c_driver);

MODULE_DESCRIPTION("NVME QNAP CPLD");
MODULE_AUTHOR("Tom Yeh");
MODULE_LICENSE("GPL v2");
