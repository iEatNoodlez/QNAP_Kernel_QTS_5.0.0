# SPDX-License-Identifier: GPL-2.0

ifeq ($(CONFIG_ARCH_ALPINE),y)
KBUILD_CFLAGS_KERNEL += -I$(srctree)/drivers/soc/alpine/ -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/dma/al/ -I$(srctree)/drivers/pci/endpoint/alpine/ -I$(srctree)/drivers/dma/al/
KBUILD_CFLAGS_MODULE += -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/pci/endpoint/alpine/
endif

obj-$(CONFIG_NVME_TARGET)		+= nvmet.o
obj-$(CONFIG_NVME_TARGET_LOOP)		+= nvme-loop.o
obj-$(CONFIG_NVME_TARGET_RDMA)		+= nvmet-rdma.o
obj-$(CONFIG_NVME_TARGET_FC)		+= nvmet-fc.o
obj-$(CONFIG_NVME_TARGET_FCLOOP)	+= nvme-fcloop.o

#obj-$(CONFIG_NVME_TARGET_QNAP_PCIE)     += qnap_nvmet_pcie_dma.o
obj-$(CONFIG_NVME_TARGET_QNAP_PCIE)     += nvmet-pcie.o

nvmet-y		+= core.o configfs.o admin-cmd.o io-cmd.o fabrics-cmd.o \
			discovery.o
nvme-loop-y	+= loop.o
nvmet-rdma-y	+= rdma.o
nvmet-fc-y	+= fc.o
nvme-fcloop-y	+= fcloop.o

ifeq ($(CONFIG_ARCH_ALPINE),y)
#obj-y   += qnap_nvmet_pcie.o
#qnap_nvmet_pcie-objs := qnap_pcie_ep_subsystem.o qnap_pcie_ep_ctrl.o \
                        qnap_pcie_ep_task_manager.o qnap_pcie_ep_execute.o

nvmet-pcie-y   += qnap_nvmet_pcie_subsystem.o qnap_nvmet_pcie_task_manager.o qnap_nvmet_pcie_admin.o

endif

