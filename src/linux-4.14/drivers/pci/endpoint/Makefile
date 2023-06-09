#
# Makefile for PCI Endpoint Support
#

ifneq ($(CONFIG_ARCH_ALPINE),y)
obj-$(CONFIG_PCI_ENDPOINT_CONFIGFS)	+= pci-ep-cfs.o
obj-$(CONFIG_PCI_ENDPOINT)		+= pci-epc-core.o pci-epf-core.o\
					   pci-epc-mem.o functions/
endif

ifeq ($(CONFIG_ARCH_ALPINE),y)
obj-y					+= alpine/
endif

obj-$(CONFIG_PCIE_ARMADA_DW_EP) += pcie-armada-dw-ep.o
