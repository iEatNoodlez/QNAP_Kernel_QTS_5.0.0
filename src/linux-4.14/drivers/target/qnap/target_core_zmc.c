/*******************************************************************************
 * Filename:  
 * @file        target_core_zmc.c
 * @brief       
 *
 * @author      Adam Hsu
 * @date        2018/XX/YY
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/

#include <linux/net.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/in.h>
#include <linux/cdrom.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/vmalloc.h>
#include <linux/device-mapper.h>
#include <linux/parser.h>
#include <asm/unaligned.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>

#include <target/target_core_base.h>
#include <target/target_core_fabric.h>
#include <target/target_core_backend.h>

#include "../target_core_iblock.h"
#include "../target_core_file.h"
#include "target_core_qtransport.h"
#include "target_core_qsbc.h"

static u8 hex[] = {
	'0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};


static void zmc_dump_mem(
	u8 *buf,
	size_t dump_size
	)
{
	u8 *Data, Val[50], Str[20], c;
	size_t Size, Index;

	Data = buf;

	while (dump_size) {
		Size = 16;
		if (Size > dump_size)
			Size = dump_size;

		for (Index = 0; Index < Size; Index += 1) {
			c = Data[Index];
			Val[Index * 3 + 0] = hex[c >> 4];
			Val[Index * 3 + 1] = hex[c & 0xF];
			Val[Index * 3 + 2] = (u8) ((Index == 7) ? '-' : ' ');
			Str[Index] = (u8) ((c < ' ' || c > 'z') ? '.' : c);
		}

		Val[Index * 3] = 0;
		Str[Index] = 0;
		pr_info("addr-0x%p: %s *%s*\n",Data, Val, Str);
		Data += Size;
		dump_size -= Size;
	}
	return;
}

void qnap_target_se_cmd_zmc_init(
	struct se_cmd *se_cmd
	)
{
#ifdef QNAP_ISCSI_TCP_ZMC_FEATURE
	if (qnap_transport_check_iscsi_fabric(se_cmd)) {
		qnap_target_se_cmd_iscsi_zmc_alloc(se_cmd);
		return;
	}
#endif
	return;
}


/*
 *
 */ 
void qnap_target_zmc_prep_backend_op(
	struct se_cmd *cmd
	)
{
	if (cmd->zmc_data.valid) {
#ifdef QNAP_ISCSI_TCP_ZMC_FEATURE
		if (qnap_transport_check_iscsi_fabric(cmd))
			qnap_target_iscsi_zmc_prep_backend_op(cmd);
#endif
	}

	return;
}



