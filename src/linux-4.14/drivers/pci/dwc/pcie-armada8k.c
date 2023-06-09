// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Marvell Armada-8K SoCs
 *
 * Armada-8K PCIe Glue Layer Source Code
 *
 * Copyright (C) 2016 Marvell Technology Group Ltd.
 *
 * Author: Yehuda Yitshak <yehuday@marvell.com>
 * Author: Shadi Ammouri <shadi@marvell.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/of_pci.h>
#include <linux/of_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "pcie-designware.h"
#include <linux/of_gpio.h>

struct armada8k_pcie {
#define MV_A8K_PCIE_MAX_WIDTH		4
	struct dw_pcie *pci;
	struct clk *clk;
	struct gpio_desc	*reset_gpio;
	struct regmap *sysctrl_base;
	u32 mac_rest_bitmask;
	struct work_struct recover_link_work;
	enum of_gpio_flags	flags;
	int phy_count;
	struct phy *comphy[MV_A8K_PCIE_MAX_WIDTH];
};

#define PCIE_VENDOR_REGS_OFFSET		0x8000

#define PCIE_GLOBAL_CONTROL_REG		(PCIE_VENDOR_REGS_OFFSET + 0x0)
#define PCIE_APP_LTSSM_EN		BIT(2)
#define PCIE_DEVICE_TYPE_SHIFT		4
#define PCIE_DEVICE_TYPE_MASK		0xF
#define PCIE_DEVICE_TYPE_RC		0x4 /* Root complex */

#define PCIE_GLOBAL_STATUS_REG		(PCIE_VENDOR_REGS_OFFSET + 0x8)
#define PCIE_GLB_STS_RDLH_LINK_UP	BIT(1)
#define PCIE_GLB_STS_PHY_LINK_UP	BIT(9)

#define PCIE_GLOBAL_INT_CAUSE1_REG	(PCIE_VENDOR_REGS_OFFSET + 0x1C)
#define PCIE_GLOBAL_INT_MASK1_REG	(PCIE_VENDOR_REGS_OFFSET + 0x20)
#define PCIE_INT_A_ASSERT_MASK		BIT(9)
#define PCIE_INT_B_ASSERT_MASK		BIT(10)
#define PCIE_INT_C_ASSERT_MASK		BIT(11)
#define PCIE_INT_D_ASSERT_MASK		BIT(12)

#define PCIE_GLOBAL_INT_CAUSE2_REG	(PCIE_VENDOR_REGS_OFFSET + 0x24)
#define PCIE_GLOBAL_INT_MASK2_REG	(PCIE_VENDOR_REGS_OFFSET + 0x28)
#define PCIE_INT2_PHY_RST_LINK_DOWN	BIT(1)

#define PCIE_ARCACHE_TRC_REG		(PCIE_VENDOR_REGS_OFFSET + 0x50)
#define PCIE_AWCACHE_TRC_REG		(PCIE_VENDOR_REGS_OFFSET + 0x54)
#define PCIE_ARUSER_REG			(PCIE_VENDOR_REGS_OFFSET + 0x5C)
#define PCIE_AWUSER_REG			(PCIE_VENDOR_REGS_OFFSET + 0x60)
/*
 * AR/AW Cache defauls: Normal memory, Write-Back, Read / Write
 * allocate
 */
#define ARCACHE_DEFAULT_VALUE		0x3511
#define AWCACHE_DEFAULT_VALUE		0x5311

#define DOMAIN_OUTER_SHAREABLE		0x2
#define AX_USER_DOMAIN_MASK		0x3
#define AX_USER_DOMAIN_SHIFT		4

#define PCIE_STREAM_ID			(PCIE_VENDOR_REGS_OFFSET + 0x64)
#define STREAM_ID_BUS_BITS		2
#define STREAM_ID_DEV_BITS		2
#define STREAM_ID_FUNC_BITS		3
#define STREAM_ID_PREFIX		0x80
#define PCIE_STREAM_ID_CFG		(STREAM_ID_PREFIX << 12 | \
					STREAM_ID_BUS_BITS << 8 | \
					STREAM_ID_DEV_BITS << 4 | \
					STREAM_ID_FUNC_BITS)

#define UNIT_SOFT_RESET_CONFIG_REG	0x268

#define to_armada8k_pcie(x)	dev_get_drvdata((x)->dev)

static int armada8k_pcie_link_up(struct dw_pcie *pci)
{
	u32 reg;
	u32 mask = PCIE_GLB_STS_RDLH_LINK_UP | PCIE_GLB_STS_PHY_LINK_UP;

	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_STATUS_REG);

	if ((reg & mask) == mask)
		return 1;

	dev_dbg(pci->dev, "No link detected (Global-Status: 0x%08x).\n", reg);
	return 0;
}

static void armada8k_pcie_establish_link(struct armada8k_pcie *pcie)
{
	struct dw_pcie *pci = pcie->pci;
	u32 reg;

	/* Setup Requester-ID to Stream-ID mapping */
	dw_pcie_writel_dbi(pci, PCIE_STREAM_ID, PCIE_STREAM_ID_CFG);

	if (!dw_pcie_link_up(pci)) {
		/* Disable LTSSM state machine to enable configuration */
		reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
		reg &= ~(PCIE_APP_LTSSM_EN);
		dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);
	}

	/* Set the device to root complex mode */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
	reg &= ~(PCIE_DEVICE_TYPE_MASK << PCIE_DEVICE_TYPE_SHIFT);
	reg |= PCIE_DEVICE_TYPE_RC << PCIE_DEVICE_TYPE_SHIFT;
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);

	/* Set the PCIe master AxCache attributes */
	dw_pcie_writel_dbi(pci, PCIE_ARCACHE_TRC_REG, ARCACHE_DEFAULT_VALUE);
	dw_pcie_writel_dbi(pci, PCIE_AWCACHE_TRC_REG, AWCACHE_DEFAULT_VALUE);

	/* Set the PCIe master AxDomain attributes */
	reg = dw_pcie_readl_dbi(pci, PCIE_ARUSER_REG);
	reg &= ~(AX_USER_DOMAIN_MASK << AX_USER_DOMAIN_SHIFT);
	reg |= DOMAIN_OUTER_SHAREABLE << AX_USER_DOMAIN_SHIFT;
	dw_pcie_writel_dbi(pci, PCIE_ARUSER_REG, reg);

	reg = dw_pcie_readl_dbi(pci, PCIE_AWUSER_REG);
	reg &= ~(AX_USER_DOMAIN_MASK << AX_USER_DOMAIN_SHIFT);
	reg |= DOMAIN_OUTER_SHAREABLE << AX_USER_DOMAIN_SHIFT;
	dw_pcie_writel_dbi(pci, PCIE_AWUSER_REG, reg);

	/* Enable INT A-D interrupts */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_MASK1_REG);
	reg |= PCIE_INT_A_ASSERT_MASK | PCIE_INT_B_ASSERT_MASK |
	       PCIE_INT_C_ASSERT_MASK | PCIE_INT_D_ASSERT_MASK;
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_MASK1_REG, reg);

	/* Also enable link down interrupts */
	reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_MASK2_REG);
	reg |= PCIE_INT2_PHY_RST_LINK_DOWN;
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_MASK2_REG, reg);

	if (!dw_pcie_link_up(pci)) {
		/* Configuration done. Start LTSSM */
		reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
		reg |= PCIE_APP_LTSSM_EN;
		dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);
	}

	/* Wait until the link becomes active again */
	if (dw_pcie_wait_for_link(pci))
		dev_err(pci->dev, "Link not up after reconfiguration\n");
}

static int armada8k_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct armada8k_pcie *pcie = to_armada8k_pcie(pci);

	dw_pcie_setup_rc(pp);
	armada8k_pcie_establish_link(pcie);

	return 0;
}

/* armada8k_pcie_reset
 * The function implements the PCIe reset via GPIO.
 * First, pull down the GPIO used for PCIe reset, and wait 200ms;
 * Second, set the GPIO output value with setting from DTS, and wait
 * 200ms for taking effect.
 * Return: void, always success.
 */
static void armada8k_pcie_reset(struct armada8k_pcie *pcie)
{
	/* Set the reset gpio to low first */
	gpiod_direction_output(pcie->reset_gpio, 0);
	/* After 200ms to reset pcie */
	mdelay(200);
	gpiod_direction_output(pcie->reset_gpio,
			       (pcie->flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1);
	mdelay(200);
}

static void armada8k_pcie_recover_link(struct work_struct *ws)
{
	struct armada8k_pcie *pcie = container_of(ws, struct armada8k_pcie,
						  recover_link_work);
	struct pci_bus *bus = pcie->pci->pp.root_bus;
	struct pcie_port *pp = &pcie->pci->pp;
	struct regmap *sysctrl_base = pcie->sysctrl_base;
	struct pci_dev *root_port;
	u32 reg;

	root_port = pci_get_slot(bus, 0);
	if (!root_port) {
		dev_err(pcie->pci->dev, "failed to get root port\n");
		return;
	}
	pci_lock_rescan_remove();
	/* remove bus */
	pci_stop_and_remove_bus_device(root_port);

	/* reset endpoint */
	if (pcie->reset_gpio)
		armada8k_pcie_reset(pcie);
	else
	/*
	 * delay is included in reset ep.
	 * without reset ep delay needed before resetting mac.
	 */
		mdelay(100);

	/* reset mac */
	regmap_read(sysctrl_base,
		    UNIT_SOFT_RESET_CONFIG_REG, &reg);
	reg &= ~pcie->mac_rest_bitmask;
	regmap_write(sysctrl_base,
		     UNIT_SOFT_RESET_CONFIG_REG, reg);
	udelay(1);
	reg |= pcie->mac_rest_bitmask;
	regmap_write(sysctrl_base,
		     UNIT_SOFT_RESET_CONFIG_REG, reg);
	udelay(1);
	armada8k_pcie_host_init(pp);

	/* rescan bus */
	pci_rescan_bus(bus);
	pci_unlock_rescan_remove();
}

static irqreturn_t armada8k_pcie_irq_handler(int irq, void *arg)
{
	struct armada8k_pcie *pcie = arg;
	struct dw_pcie *pci = pcie->pci;
	u32 val;

	/*
	 * Interrupts are directly handled by the device driver of the
	 * PCI device. However, they are also latched into the PCIe
	 * controller, so we simply discard them.
	 */
	val = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_CAUSE1_REG);
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_CAUSE1_REG, val);

	val = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_CAUSE2_REG);

	if (PCIE_INT2_PHY_RST_LINK_DOWN & val) {
		u32 reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_CONTROL_REG);
		/*
		 * The link went down. Disable LTSSM immediately. This
		 * unlocks the root complex config registers. Downstream
		 * device accesses will return all-Fs without freezing the
		 * CPU.
		 */
		reg &= ~(PCIE_APP_LTSSM_EN);
		dw_pcie_writel_dbi(pci, PCIE_GLOBAL_CONTROL_REG, reg);
		/*
		 * Mask link down interrupts. They can be re-enabled once
		 * the link is retrained.
		 */

		reg = dw_pcie_readl_dbi(pci, PCIE_GLOBAL_INT_MASK2_REG);
		reg &= ~PCIE_INT2_PHY_RST_LINK_DOWN;
		dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_MASK2_REG, reg);
		/*
		 * At this point a worker thread can be triggered to
		 * initiate a link retrain. If link retrains were
		 * possible, that is.
		 */

		if (pcie->sysctrl_base && pcie->mac_rest_bitmask)
			schedule_work(&pcie->recover_link_work);
		dev_dbg(pci->dev, "%s: link went down\n", __func__);
	}

	/* Now clear the second interrupt cause. */
	dw_pcie_writel_dbi(pci, PCIE_GLOBAL_INT_CAUSE2_REG, val);

	return IRQ_HANDLED;
}

static const struct dw_pcie_host_ops armada8k_pcie_host_ops = {
	.host_init = armada8k_pcie_host_init,
};

static int armada8k_add_pcie_port(struct armada8k_pcie *pcie,
				  struct platform_device *pdev)
{
	struct dw_pcie *pci = pcie->pci;
	struct pcie_port *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->root_bus_nr = -1;
	pp->ops = &armada8k_pcie_host_ops;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0) {
		dev_err(dev, "failed to get irq for port\n");
		return pp->irq;
	}

	ret = devm_request_irq(dev, pp->irq, armada8k_pcie_irq_handler,
			       IRQF_SHARED, "armada8k-pcie", pcie);
	if (ret) {
		dev_err(dev, "failed to request irq %d\n", pp->irq);
		return ret;
	}

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.link_up = armada8k_pcie_link_up,
};

static int armada8k_phy_config(struct platform_device *pdev,
			       struct armada8k_pcie *pcie)
{
	struct phy *comphy;
	int err;
	int i;

	pcie->phy_count = of_count_phandle_with_args(pdev->dev.of_node, "phys",
					       "#phy-cells");
	if (pcie->phy_count <= 0)
		return 0;

	for (i = 0; i < pcie->phy_count; i++) {
		comphy = devm_of_phy_get_by_index(&pdev->dev,
						  pdev->dev.of_node, i);
		if (IS_ERR(comphy)) {
			dev_err(&pdev->dev, "Failed to get phy %d\n", i);
			return PTR_ERR(comphy);
		}

		pcie->comphy[i] = comphy;

		switch (pcie->phy_count) {
		case PCIE_LNK_X1:
		case PCIE_LNK_X2:
		case PCIE_LNK_X4:
			phy_set_bus_width(comphy, pcie->phy_count);
			break;
		default:
			dev_err(&pdev->dev, "wrong pcie width %d",
				pcie->phy_count);
			return -EINVAL;
		}

		err = phy_set_mode(comphy, PHY_MODE_PCIE);
		if (err) {
			dev_err(&pdev->dev, "failed to set comphy\n");
			return err;
		}

		err = phy_init(comphy);
		if (err < 0) {
			dev_err(&pdev->dev, "phy init failed %d",
				pcie->phy_count);
			return err;
		}

		err = phy_power_on(comphy);
		if (err < 0) {
			dev_err(&pdev->dev, "phy init failed %d",
				pcie->phy_count);
			phy_exit(comphy);
			return err;
		}
	}

	return err;
}

static void armada8k_phy_deconfig(struct armada8k_pcie *pcie)
{
	int i;

	for (i = 0; i < pcie->phy_count; i++) {
		phy_power_off(pcie->comphy[i]);
		phy_exit(pcie->comphy[i]);
	}
}

static int armada8k_pcie_probe(struct platform_device *pdev)
{
	struct dw_pcie *pci;
	struct armada8k_pcie *pcie;
	struct device *dev = &pdev->dev;
	struct resource *base;
	int reset_gpio;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;
	pcie->pci = pci;

	INIT_WORK(&pcie->recover_link_work, armada8k_pcie_recover_link);

	pcie->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(pcie->clk))
		return PTR_ERR(pcie->clk);

	ret = clk_prepare_enable(pcie->clk);
	if (ret)
		return ret;

	/* Get the dw-pcie unit configuration/control registers base. */
	base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl");
	pci->dbi_base = devm_pci_remap_cfg_resource(dev, base);
	if (IS_ERR(pci->dbi_base)) {
		dev_err(dev, "couldn't remap regs base %p\n", base);
		ret = PTR_ERR(pci->dbi_base);
		goto fail;
	}

	/* Config reset gpio for pcie if the reset connected to gpio */
	reset_gpio = of_get_named_gpio_flags(pdev->dev.of_node,
					     "reset-gpio", 0,
					     &pcie->flags);
	if (gpio_is_valid(reset_gpio)) {
		pcie->reset_gpio = gpio_to_desc(reset_gpio);
		armada8k_pcie_reset(pcie);
	}

	pcie->sysctrl_base =
		syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						"marvell,system-controller");

	ret = of_property_read_u32(pdev->dev.of_node,
				   "mac-reset-bit-mask",
				   &pcie->mac_rest_bitmask);
	if (ret < 0) {
		dev_err(dev, "couldn't find mac reset bit mask: %d\n", ret);
		pcie->mac_rest_bitmask = 0x0;
	}

	ret = armada8k_phy_config(pdev, pcie);
	if (ret < 0) {
		dev_err(dev, "PHYs config failed: %d\n", ret);
		goto fail;
	}

	platform_set_drvdata(pdev, pcie);

	ret = armada8k_add_pcie_port(pcie, pdev);
	if (ret)
		goto fail_phy;

	return 0;

fail_phy:
	armada8k_phy_deconfig(pcie);
fail:
	if (!IS_ERR(pcie->clk))
		clk_disable_unprepare(pcie->clk);

	return ret;
}

static int armada8k_pcie_remove(struct platform_device *pdev)
{
	struct armada8k_pcie *pcie = platform_get_drvdata(pdev);
	struct dw_pcie *pci = pcie->pci;
	struct device *dev = &pdev->dev;

	cancel_work_sync(&pcie->recover_link_work);

	dw_pcie_host_deinit(&pci->pp);

	armada8k_phy_deconfig(pcie);

	if (!IS_ERR(pcie->clk))
		clk_disable_unprepare(pcie->clk);

	dev_dbg(dev, "%s\n", __func__);

	return 0;
}

static const struct of_device_id armada8k_pcie_of_match[] = {
	{ .compatible = "marvell,armada8k-pcie", },
	{},
};

static struct platform_driver armada8k_pcie_driver = {
	.probe		= armada8k_pcie_probe,
	.remove		= armada8k_pcie_remove,
	.driver = {
		.name	= "armada8k-pcie",
		.of_match_table = of_match_ptr(armada8k_pcie_of_match),
	},
};
builtin_platform_driver(armada8k_pcie_driver);
