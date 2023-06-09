KBUILD_CFLAGS_KERNEL += -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/dma/al/
KBUILD_CFLAGS_MODULE += -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/dma/al/

obj-$(CONFIG_PCI_ALPINE_ENDPOINT)   += qnap_pcie_ep.o

ifeq ($(CONFIG_AL_DMA),y)
qnap_pcie_ep-objs := qnap_pcie_ep_main.o qnap_pcie_ep_dma.o qnap_pcie_ep_mmio.o qnap_pcie_ep_hal_event.o
else
qnap_pcie_ep-objs := qnap_pcie_ep_main.o qnap_pcie_ep_mmio.o qnap_pcie_ep_hal_event.o
endif

obj-$(CONFIG_PCI_ALPINE_ENDPOINT_CPLD)  += qnap_pcie_ep_cpld.o
obj-$(CONFIG_PCI_ALPINE_ENDPOINT_DEBUG)	+= qnap_pcie_ep_debug.o
obj-$(CONFIG_PCI_ALPINE_ENDPOINT_CABLE_DEBUG)	+= qnap_pcie_ep_cable_debug.o
