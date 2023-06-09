# SPDX-License-Identifier: GPL-2.0

target_core_mod-y		:= target_core_configfs.o \
				   target_core_device.o \
				   target_core_fabric_configfs.o \
				   target_core_fabric_lib.o \
				   target_core_hba.o \
				   target_core_pr.o \
				   target_core_alua.o \
				   target_core_tmr.o \
				   target_core_tpg.o \
				   target_core_transport.o \
				   target_core_sbc.o \
				   target_core_spc.o \
				   target_core_ua.o \
				   target_core_rd.o \
				   target_core_stat.o \
				   target_core_xcopy.o

obj-$(CONFIG_TARGET_CORE)	+= target_core_mod.o

# Subsystem modules
obj-$(CONFIG_TCM_IBLOCK)	+= target_core_iblock.o
obj-$(CONFIG_TCM_FILEIO)	+= target_core_file.o
obj-$(CONFIG_TCM_PSCSI)		+= target_core_pscsi.o
obj-$(CONFIG_TCM_USER2)		+= target_core_user.o

# Fabric modules
obj-$(CONFIG_LOOPBACK_TARGET)	+= loopback/
obj-$(CONFIG_TCM_FC)		+= tcm_fc/
obj-$(CONFIG_ISCSI_TARGET)	+= iscsi/
obj-$(CONFIG_SBP_TARGET)	+= sbp/

# QNAP codes
target_core_mod-$(CONFIG_MACH_QNAPTS)	+= qnap/target_core_qtransport.o \
					   qnap/target_core_qconfigfs.o \
					   qnap/target_core_qspc.o \
					   qnap/target_core_qsbc.o \
					   qnap/target_core_qlib.o

# QNAP codes (Fast Block Clone)
target_core_mod-$(CONFIG_MACH_QNAPTS)	+= qnap/target_core_qfbc.o

# QNAP codes (ODX)
target_core_mod-$(CONFIG_MACH_QNAPTS)	+= qnap/target_core_qodx.o \
					   qnap/target_core_qodx_core.o \
					   qnap/target_core_qodx_lib.o \
					   qnap/target_core_qodx_rb.o \
					   qnap/target_core_qodx_vpd.o

# QNAP iscsi-zmc code
target_core_mod-$(CONFIG_MACH_QNAPTS) += qnap/target_core_qzmc.o \
		qnap/target_core_iscsi_qzmc.o

# QNAP perf enhancement code
target_core_mod-$(CONFIG_MACH_QNAPTS) += qnap/target_core_perf_en.o

# QNAP flags
ccflags-$(CONFIG_MACH_QNAPTS)	+= -DCONFIG_MACH_QNAPTS -DISCSI_D4_INITIATOR -DSUPPORT_ISCSI_ZERO_COPY -DCONFIG_QISCSI_LOG

# QNAP way to check all supported func, please refer script
PWD=$(shell pwd)
ccflags-$(CONFIG_MACH_QNAPTS)	+= $(shell sh $(PWD)/drivers/target/qnap/check_func.sh)
