/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define __ARCH_WANT_RENAMEAT
#define __ARCH_WANT_NEW_STAT
#define __ARCH_WANT_SET_GET_RLIMIT
#define __ARCH_WANT_TIME32_SYSCALLS
#define __ARCH_WANT_SYS_CLONE3

#include <asm-generic/unistd.h>


#define __NR_set_ipsec_rules          450
__SYSCALL(__NR_set_ipsec_rules, sys_set_ipsec_rules)
#define __NR_get_ipsec_vio_acc_list   451
__SYSCALL(__NR_get_ipsec_vio_acc_list, sys_get_ipsec_vio_acc_list)
#define __NR_qnap_rmdir               452
__SYSCALL(__NR_qnap_rmdir, sys_qnap_rmdir)
#define __NR_qnap_unlink              453
__SYSCALL(__NR_qnap_unlink, sys_qnap_unlink)
#define __NR_qnap_find_filename       454
__SYSCALL(__NR_qnap_find_filename, sys_qnap_find_filename)
