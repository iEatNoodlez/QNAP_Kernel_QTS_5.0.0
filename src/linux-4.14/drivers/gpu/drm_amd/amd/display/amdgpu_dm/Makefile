#
# Makefile for the 'dm' sub-component of DAL.
# It provides the control and status of dm blocks.



AMDGPUDM = amdgpu_dm.o amdgpu_dm_irq.o amdgpu_dm_mst_types.o amdgpu_dm_color.o

ifneq ($(CONFIG_DRM_AMD_DC),)
AMDGPUDM += amdgpu_dm_services.o amdgpu_dm_helpers.o
endif

ifneq ($(CONFIG_DEBUG_FS),)
AMDGPUDM += amdgpu_dm_crc.o
endif

subdir-ccflags-y += -I$(FULL_AMD_DISPLAY_PATH)/dc

AMDGPU_DM = $(addprefix $(AMDDALPATH)/amdgpu_dm/,$(AMDGPUDM))

AMD_DISPLAY_FILES += $(AMDGPU_DM)
