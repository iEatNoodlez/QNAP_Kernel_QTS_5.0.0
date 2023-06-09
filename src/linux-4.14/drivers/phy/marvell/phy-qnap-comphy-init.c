#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

//#include <dt-bindings/phy/phy-comphy-mvebu.h>


#ifdef CONFIG_MACH_QNAPTS
static int qnap_mvebu_comphy_probe(struct platform_device *pdev)
{
/*
    struct mvebu_comphy_priv *priv;
    const struct mvebu_comphy_data *data;
    struct phy_provider *provider;
    struct device_node *child;
    struct resource *res;
    int i;
    struct fwnode_handle *fwnode = pdev->dev.fwnode;
    struct fwnode_handle *port_fwnode;
    struct device_node *port_node = to_of_node(port_fwnode);
*/
    int ret;
    int num;
    u32 base;
    u32 sz;
    u32 val;
    int idx;
    void __iomem *reg;

//printk("== QC2000: %s:%s(%d): %s Start\n", __FILE__, __func__, __LINE__, dev_name(&pdev->dev));

    ret = of_property_read_u32_index(pdev->dev.of_node, "reg-map", 0, &base);
    if (ret)
        goto get_property_reg_err;

    ret = of_property_read_u32_index(pdev->dev.of_node, "reg-map", 1, &sz);
    if (ret)
        goto get_property_reg_err;

    num = of_property_count_u32_elems(pdev->dev.of_node, "comphy_settings");
    if (num <= 0)
        goto get_property_comphy_settings_err;

    for (idx = 0; idx < num; idx += 2)
    {
        u32 rd_val;
        u32 new_val;
        ret = of_property_read_u32_index(pdev->dev.of_node, "comphy_settings", idx, &base);
        if (ret)
            goto get_bsae_err;
        ret = of_property_read_u32_index(pdev->dev.of_node, "comphy_settings", idx+1, &val);
        if (ret)
            goto get_val_err;
/*        printk("elem[%d][0x%x] = 0x%x\n", idx, base, val); */

        reg = devm_ioremap(&pdev->dev, base, 0x100);
        if (!reg)
        {
            dev_err(&pdev->dev, "ioremap failed\n");
            return -1;
        }
        rd_val = readl(reg);
        writel(val, reg);
        new_val = readl(reg);
        devm_iounmap(&pdev->dev, reg);
/*        printk("readl virt addr 0x%x(reg 0x%x): old val = 0x%x, new val = 0x%x\n", (u32)reg, base, rd_val, new_val); */
    }
//printk("== QC2000: %s:%s(%d): %s End\n", __FILE__, __func__, __LINE__, dev_name(&pdev->dev));
/*
        ret = of_property_read_u32(child, "reg", &val);
        if (ret < 0) {
            dev_err(&pdev->dev, "missing 'reg' property (%d)\n",
                ret);
     /       continue;
        }
*/
    return 0;
get_property_reg_err:
    printk("Error: get_property_reg_err\n");
    return -1;
get_property_comphy_settings_err:
    printk("Error: get_property_reg_err\n");
    return -1;
get_bsae_err:
    printk("Error: get_base_err\n");
    return -1;
get_val_err:
    printk("Error: get_val_err\n");
    return -1;
}

static const struct of_device_id qnap_mvebu_comphy_of_match_table[] = {
    {
        .compatible = "qnap,comphy-cp110",
    },
    { },
};


MODULE_DEVICE_TABLE(of, qnap_mvebu_comphy_of_match_table);

static struct platform_driver qnap_mvebu_comphy_driver = {
    .probe  = qnap_mvebu_comphy_probe,
    .driver = {
        .name = "qnap-mvebu-comphy",
        .of_match_table = qnap_mvebu_comphy_of_match_table,
    },
};
module_platform_driver(qnap_mvebu_comphy_driver);

#endif

//MODULE_AUTHOR("Antoine Tenart <antoine.tenart@free-electrons.com>, Grzegorz Jaszczyk <jaz@semihalf.com>");
//MODULE_DESCRIPTION("Common PHY driver for mvebu SoCs");
MODULE_LICENSE("GPL v2");

