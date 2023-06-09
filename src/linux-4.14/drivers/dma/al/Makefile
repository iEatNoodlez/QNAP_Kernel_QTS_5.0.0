KBUILD_CFLAGS_KERNEL += -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/dma/al/
KBUILD_CFLAGS_MODULE += -I$(srctree)/include/soc/alpine/ -I$(srctree)/drivers/dma/al/

obj-$(CONFIG_AL_DMA)   += al_dma.o
al_dma-objs :=  al_dma_main.o al_dma_core.o al_dma_cleanup.o al_dma_sysfs.o al_dma_prep_interrupt.o al_dma_prep_memcpy.o \
		al_dma_prep_memset.o \
		al_dma_prep_xor.o \
		al_dma_prep_xor_val.o \
		al_dma_prep_pq.o \
		al_dma_prep_sg.o \
		al_dma_prep_pq_val.o \
		al_hal_ssm_raid.o
#
#obj-$(CONFIG_AL_DMA) += al_dma.o
#obj-$(CONFIG_AL_DMA) += al_dma_core.o
#obj-$(CONFIG_AL_DMA) += al_dma_cleanup.o
#obj-$(CONFIG_AL_DMA) += al_dma_sysfs.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_interrupt.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_memcpy.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_memset.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_xor.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_xor_val.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_pq.o
#obj-$(CONFIG_AL_DMA) += al_dma_prep_pq_val.o
#obj-$(CONFIG_AL_DMA) += al_hal_ssm_raid.o
#
