# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ARMADA375_USBCLUSTER_PHY)	+= phy-armada375-usb2.o
obj-$(CONFIG_PHY_BERLIN_SATA)		+= phy-berlin-sata.o
obj-$(CONFIG_PHY_BERLIN_USB)		+= phy-berlin-usb.o
obj-$(CONFIG_PHY_MVEBU_CP110_COMPHY)	+= phy-mvebu-cp110-comphy.o
ifeq ($(CONFIG_ARCH_MVEBU),y)
obj-m += phy-qnap-comphy-init.o
endif
obj-$(CONFIG_PHY_MVEBU_SATA)		+= phy-mvebu-sata.o
obj-$(CONFIG_PHY_PXA_28NM_HSIC)		+= phy-pxa-28nm-hsic.o
obj-$(CONFIG_PHY_PXA_28NM_USB2)		+= phy-pxa-28nm-usb2.o
