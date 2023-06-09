/*
 * Driver for Aquantia PHY
 *
 * Author: Shaohui Xie <Shaohui.Xie@freescale.com>
 *
 * Copyright 2015 Freescale Semiconductor, Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/mdio.h>

#define PHY_ID_AQ1202	0x03a1b445
#define PHY_ID_AQ2104	0x03a1b460
#define PHY_ID_AQR105	0x03a1b4a2
#define PHY_ID_AQR106	0x03a1b4d0
#define PHY_ID_AQR107	0x03a1b4e0
#define PHY_ID_AQR405	0x03a1b4b0
#define PHY_ID_AQR112C  0x03a1b792

#define PHY_AQUANTIA_FEATURES	(SUPPORTED_10000baseT_Full | \
				 SUPPORTED_1000baseT_Full | \
				 SUPPORTED_100baseT_Full | \
				 PHY_DEFAULT_FEATURES)


#if defined(CONFIG_MACH_QNAPTS) && defined(CONFIG_ARCH_MVEBU)
#include <linux/netdevice.h>
#define AQR112C_SUPPORTED_2500baseT_Full    0
#define PHY_AQR112C_FEATURES (AQR112C_SUPPORTED_2500baseT_Full | SUPPORTED_2500baseX_Full | \
				SUPPORTED_1000baseT_Full | SUPPORTED_100baseT_Full | \
				PHY_DEFAULT_FEATURES)


static int aqr112c_config_aneg(struct phy_device *phydev)
{
	struct net_device *ndev = phydev->attached_dev;
	int reg;
	int speed;
	int duplex;
	int pause;
	int asym_pause;
	int err = 0;
	
	if (!ndev)
		return 0;

	dev_dbg(&ndev->dev, "cur_speed = 0x%x, autoneg = %d, link = %d", phydev->speed, phydev->advertising & ADVERTISED_Autoneg, phydev->link);
	if (!phydev->link)
	{
		phydev->speed = -1;
		phydev->duplex = -1;
		phydev->autoneg = AUTONEG_ENABLE;
		dev_dbg(&ndev->dev, "phydev->link = 0, phydev->state = 0x%x", phydev->state);
		return 0;
	}

	if (phydev->link && (phydev->advertising & ADVERTISED_Autoneg) && (phydev->speed == -1))
	{
		dev_dbg(&ndev->dev, "link = %d, advertising = 0x%x, speed = %d, run autoneg", phydev->link, (phydev->advertising & ADVERTISED_Autoneg), phydev->speed);
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x20, 0xe1);
		if (err < 0)
			return err;
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xc400, 0x9440);
		if (err < 0)
			return err;
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x10, 0x9101);
		if (err < 0)
			return err;
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x0, 0x3200);
		if (err < 0)
			return err;
		mdelay(10);

		reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);
		mdelay(10);
		reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);
		switch (reg) {
		case 0x9:
			speed = SPEED_2500;
			duplex = DUPLEX_FULL;
			break;
		case 0x5:
			speed = SPEED_1000;
			duplex = DUPLEX_FULL;
			break;
		case 0x2:
			speed = SPEED_100;
			duplex = DUPLEX_HALF;
			break;
		case 0x3:
			speed = SPEED_100;
			duplex = DUPLEX_FULL;
			break;
		default:
			speed = -1;
			duplex = -1;
			break;
		}
		phydev->speed = speed;
		phydev->duplex = duplex;
		phydev->autoneg = AUTONEG_ENABLE;
		dev_dbg(&ndev->dev, "phydev: link = 1, autoneg = 1, speed = %d, duplex = %d", phydev->speed, phydev->duplex);
		return 0;
	}

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);
	mdelay(10);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);

	switch (reg) {
		case 0x9:
			speed = SPEED_2500;
			duplex = DUPLEX_FULL;
			break;
		case 0x5:
			speed = SPEED_1000;
			duplex = DUPLEX_FULL;
			break;
		case 0x2:
			speed = SPEED_100;
			duplex = DUPLEX_HALF;
			break;
		case 0x3:
			speed = SPEED_100;
			duplex = DUPLEX_FULL;
			break;
		default:
			speed = -1;
			duplex = -1;
			break;
	}

	if (phydev->speed != speed)
	{
		dev_dbg(&ndev->dev, "link speed from %d to %d", speed, phydev->speed);
		switch (phydev->speed) {
			case SPEED_2500:
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x20, 0xe1);
			if (err < 0)
				return err;

			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xc400, 0x400);
			if (err < 0)
				return err;

			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x10, 0x9101);
			if (err < 0)
				return err;

			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x0, 0x3200);
			if (err < 0)
				return err;
			mdelay(10);
			break;
			
			case SPEED_1000:
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x20, 0x61);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xc400, 0xc000);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x10, 0x9101);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x0, 0x3200);
			if (err < 0)
				return err;
			mdelay(10);
			break;
			
			case SPEED_100:
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x20, 0x61);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xc400, 0x1000);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x10, 0x9101);
			if (err < 0)
				return err;
			err = phy_write_mmd(phydev, MDIO_MMD_AN, 0x0, 0x3200);
			if (err < 0)
				return err;
			mdelay(10);
			break;

			default:
			break;
		}
	}
	phydev->autoneg = AUTONEG_ENABLE;
	return err;
}
#endif

static int aquantia_config_aneg(struct phy_device *phydev)
{
	phydev->supported = PHY_AQUANTIA_FEATURES;
	phydev->advertising = phydev->supported;

	return 0;
}

static int aquantia_aneg_done(struct phy_device *phydev)
{
	int reg;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	return (reg < 0) ? reg : (reg & BMSR_ANEGCOMPLETE);
}

static int aquantia_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xd401, 1);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xff00, 1);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xff01, 0x1001);
	} else {
		err = phy_write_mmd(phydev, MDIO_MMD_AN, 0xd401, 0);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xff00, 0);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0xff01, 0);
	}

	return err;
}

static int aquantia_ack_interrupt(struct phy_device *phydev)
{
	int reg;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xcc01);
	return (reg < 0) ? reg : 0;
}

static int aquantia_read_status(struct phy_device *phydev)
{
	int reg;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	if (reg & MDIO_STAT1_LSTATUS)
		phydev->link = 1;
	else
		phydev->link = 0;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);
	mdelay(10);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);

	switch (reg) {
	case 0x9:
		phydev->speed = SPEED_2500;
		break;
	case 0x5:
		phydev->speed = SPEED_1000;
		break;
	case 0x3:
		phydev->speed = SPEED_100;
		break;
	case 0x7:
	default:
		phydev->speed = SPEED_10000;
		break;
	}
	phydev->duplex = DUPLEX_FULL;

	return 0;
}

#if defined(CONFIG_MACH_QNAPTS) && defined(CONFIG_ARCH_MVEBU)
static int aqr112c_read_status(struct phy_device *phydev)
{
	int reg;
	struct net_device *ndev = phydev->attached_dev;
    static unsigned long j1 = 0, j2 = 0;

	if (!ndev)
		return 0;

	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	if (reg & MDIO_STAT1_LSTATUS)
		phydev->link = 1;
	else
		phydev->link = 0;

	if (phydev->link == 0)
	{
		phydev->speed = -1;
		phydev->duplex = -1;
	if (printk_timed_ratelimit(&j1, 6000))
		dev_dbg(&ndev->dev, "phydev->link = 0, phydev->autoneg = %d, phydev->speed = %d", (phydev->advertising & ADVERTISED_Autoneg), phydev->speed);
		return 0;
	}


	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);
	mdelay(10);
	reg = phy_read_mmd(phydev, MDIO_MMD_AN, 0xc800);

	switch (reg) {
	case 0x9:
		phydev->speed = SPEED_2500;
		phydev->duplex = DUPLEX_FULL;
		break;
	case 0x5:
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
		break;
    case 0x2:
		phydev->speed = SPEED_100;
		phydev->duplex = DUPLEX_HALF;
		break;
	case 0x3:
		phydev->speed = SPEED_100;
		phydev->duplex = DUPLEX_FULL;
		break;
	case 0x7:
	default:
		phydev->speed = -1;
		phydev->duplex = -1;
		break;
	}

	if (printk_timed_ratelimit(&j2, 6000))
		dev_dbg(&ndev->dev, "cur_speed = %d, cur_duplex = %d", phydev->speed, phydev->duplex);
	return 0;
}

static int aqr112c_set_wol(struct phy_device *phydev, struct ethtool_wolinfo *wol)
{
	struct net_device *ndev = phydev->attached_dev;
	int reg = 0;
	u32 phy_id = 0;
	u8 mac[6] = {0};
	u16 data;
	int err = 0;
	int i = 0;

	if (!ndev)	
		return 0;

	memcpy(mac, ndev->dev_addr, sizeof(mac));

	reg = phy_read_mmd(phydev, 0x3, 0x2);
	phy_id = reg << 16;
	reg = phy_read_mmd(phydev, 0x3, 0x3);
	phy_id |= reg;
	dev_dbg(&ndev->dev, "phy_id = 0x%x", phy_id);

	dev_dbg(&ndev->dev, "WoL on MAC = %02x:%02x:%02x:%02x:%02x:%02x",  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);


	if (wol->supported == (WAKE_MAGIC | WAKE_MAGICSECURE))
	{
		data = mac[0] | mac[1]<<8;
		err = phy_write_mmd(phydev, 0x1d, 0xc339, data);
		if (err < 0)
			return err;

		data = mac[2] | mac[3]<<8;
		err = phy_write_mmd(phydev, 0x1d, 0xc33a, data);
		if (err < 0)
			return err;

		data = mac[4] | mac[5]<<8;
		err = phy_write_mmd(phydev, 0x1d, 0xc33b, data);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, 0x7, 0x20, 0x1);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x7, 0xc400);
		if (reg < 0)
			return reg;
		data = 0x9000 | (u16)(reg & 0xff);
		err = phy_write_mmd(phydev, 0x7, 0xc400, data);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, 0x1d, 0xc355, 0x1);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, 0x1d, 0xc356, 0x1);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x7, 0xc410);
		if (reg < 0)
			return reg;
		data = (0x1 << 6) | (u16)reg;
		err = phy_write_mmd(phydev, 0x7, 0xc410, data);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x1d, 0xc357);
		if (reg < 0)
			return reg;
		data = (0x1 << 15) | (u16)reg;
		err = phy_write_mmd(phydev, 0x1d, 0xc357, data);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x1d, 0xf420);
		if (reg < 0)
			return reg;
		data = (0x3 << 4) | (u16)reg;
		err = phy_write_mmd(phydev, 0x1d, 0xf420, data);
		if (err < 0)
			return err;

        reg = phy_read_mmd(phydev, 0x1e, 0xff00);
		if (reg < 0)
			return reg;
		data = 0x1 | (u16)reg;
		err = phy_write_mmd(phydev, 0x1e, 0xff00, data);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x1e, 0xff01);
		if (reg < 0)
			return reg;
		data = (0x1 << 0xb) | (u16)reg;
		err = phy_write_mmd(phydev, 0x1e, 0xff01, data);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, 0x1e, 0x31b, 0xb);
		if (err < 0)
			return err;

		err = phy_write_mmd(phydev, 0x1e, 0x31c, 0xb);
		if (err < 0)
			return err;

		reg = phy_read_mmd(phydev, 0x7, 0x0);
		if (reg < 0)
			return reg;
		data = (0x1 << 0x9) | (u16)reg;
		err = phy_write_mmd(phydev, 0x7, 0x0, data);
		if (err < 0)
			return err;

		for (i = 0; i < 6; i++)
		{   // it takes a lot time to change the speed from other speed to 1 G for wol ready 
			msleep(1000);
			reg = phy_read_mmd(phydev, 0x7, 0xc812);
			if (reg < 0)
				return reg;

			if (reg & 0x1)
				break;
		}

		if (reg & 0x1)
			dev_dbg(&ndev->dev, "wol enabled (wait %d s)", i + 1);
		else
			dev_dbg(&ndev->dev, "wol disabled (wait %d s)", i + 1);
	}

	return 0;
}

static void aqr112c_get_wol(struct phy_device *dev, struct ethtool_wolinfo *wol)
{
}
#endif

static struct phy_driver aquantia_driver[] = {
{
	.phy_id		= PHY_ID_AQ1202,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQ1202",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
{
	.phy_id		= PHY_ID_AQ2104,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQ2104",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
{
	.phy_id		= PHY_ID_AQR105,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQR105",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
{
	.phy_id		= PHY_ID_AQR106,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQR106",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
{
	.phy_id		= PHY_ID_AQR107,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQR107",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
{
	.phy_id		= PHY_ID_AQR405,
	.phy_id_mask	= 0xfffffff0,
	.name		= "Aquantia AQR405",
	.features	= PHY_AQUANTIA_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.aneg_done	= aquantia_aneg_done,
	.config_aneg    = aquantia_config_aneg,
	.config_intr	= aquantia_config_intr,
	.ack_interrupt	= aquantia_ack_interrupt,
	.read_status	= aquantia_read_status,
},
#if defined(CONFIG_MACH_QNAPTS) && defined(CONFIG_ARCH_MVEBU)
{
	.phy_id     = PHY_ID_AQR112C,
	.phy_id_mask    = 0xfffffff0,
	.name       = "Aquantia AQR112C",
	.features   = PHY_AQR112C_FEATURES,
	.flags      = PHY_HAS_INTERRUPT,
	.aneg_done  = aquantia_aneg_done,
	.config_aneg    = aqr112c_config_aneg,
	.config_intr    = aquantia_config_intr,
	.ack_interrupt  = aquantia_ack_interrupt,
	.read_status    = aqr112c_read_status,
	.set_wol        = aqr112c_set_wol,
	.get_wol        = aqr112c_get_wol,
},
#endif
};

module_phy_driver(aquantia_driver);

static struct mdio_device_id __maybe_unused aquantia_tbl[] = {
	{ PHY_ID_AQ1202, 0xfffffff0 },
	{ PHY_ID_AQ2104, 0xfffffff0 },
	{ PHY_ID_AQR105, 0xfffffff0 },
	{ PHY_ID_AQR106, 0xfffffff0 },
	{ PHY_ID_AQR107, 0xfffffff0 },
	{ PHY_ID_AQR405, 0xfffffff0 },
#if defined(CONFIG_MACH_QNAPTS)
	{ PHY_ID_AQR112C, 0xfffffff0 },
#endif
	{ }
};

MODULE_DEVICE_TABLE(mdio, aquantia_tbl);

MODULE_DESCRIPTION("Aquantia PHY driver");
MODULE_AUTHOR("Shaohui Xie <Shaohui.Xie@freescale.com>");
MODULE_LICENSE("GPL v2");
