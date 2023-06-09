# SPDX-License-Identifier: GPL-2.0
iscsi_target_mod-y +=		iscsi_target_parameters.o \
				iscsi_target_seq_pdu_list.o \
				iscsi_target_auth.o \
				iscsi_target_datain_values.o \
				iscsi_target_device.o \
				iscsi_target_erl0.o \
				iscsi_target_erl1.o \
				iscsi_target_erl2.o \
				iscsi_target_login.o \
				iscsi_target_nego.o \
				iscsi_target_nodeattrib.o \
				iscsi_target_tmr.o \
				iscsi_target_tpg.o \
				iscsi_target_util.o \
				iscsi_target.o \
				iscsi_target_configfs.o \
				iscsi_target_stat.o \
				iscsi_target_transport.o

obj-$(CONFIG_ISCSI_TARGET)	+= iscsi_target_mod.o
obj-$(CONFIG_ISCSI_TARGET_CXGB4) += cxgbit/

# QNAP modules
obj-$(CONFIG_ISCSI_TARGET)	+= qnap/iscsi_target_qlog.o

# QNAP codes
iscsi_target_mod-$(CONFIG_MACH_QNAPTS)	+= qnap/iscsi_target_qtransport.o \
					   qnap/iscsi_target_qconfigfs.o
# QNAP iscsi-zmc code
iscsi_target_mod-$(CONFIG_MACH_QNAPTS)	+= qnap/iscsi_target_qzmc.o \
					qnap/iscsi_target_tcp_qzmc.o

# QNAP flags
ccflags-$(CONFIG_MACH_QNAPTS)  += -DCONFIG_MACH_QNAPTS -DCONFIG_QISCSI_LOG -DISCSI_D4_INITIATOR -DSUPPORT_ISCSI_ZERO_COPY
ccflags-$(CONFIG_MACH_QNAPTS)  += -I$(ISCSI_LOGD_INCLUDE)
