/*
 * Compat system call wrappers
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors: Will Deacon <will.deacon@arm.com>
 *	    Catalin Marinas <catalin.marinas@arm.com>
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

#include <linux/linkage.h>
#include <linux/const.h>

#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/errno.h>
#include <asm/page.h>

/*
 * Note: off_4k (w5) is always in units of 4K. If we can't do the
 * requested offset because it is not page-aligned, we return -EINVAL.
 */
ENTRY(compat_sys_mmap2_wrapper)
#if PAGE_SHIFT > 12
	tst	w5, #~PAGE_MASK >> 12
	b.ne	1f
	lsr	w5, w5, #PAGE_SHIFT - 12
#endif
	b	sys_mmap_pgoff
1:	mov	x0, #-EINVAL
	ret
ENDPROC(compat_sys_mmap2_wrapper)

/*
 * Wrappers for AArch32 syscalls that either take 64-bit parameters
 * in registers or that take 32-bit parameters which require sign
 * extension.
 */
ENTRY(compat_sys_pread64_wrapper)
	regs_to_64	x3, x4, x5
	b	sys_pread64
ENDPROC(compat_sys_pread64_wrapper)

ENTRY(compat_sys_pwrite64_wrapper)
	regs_to_64	x3, x4, x5
	b	sys_pwrite64
ENDPROC(compat_sys_pwrite64_wrapper)

ENTRY(compat_sys_truncate64_wrapper)
	regs_to_64	x1, x2, x3
	b	sys_truncate
ENDPROC(compat_sys_truncate64_wrapper)

ENTRY(compat_sys_ftruncate64_wrapper)
	regs_to_64	x1, x2, x3
	b	sys_ftruncate
ENDPROC(compat_sys_ftruncate64_wrapper)

ENTRY(compat_sys_readahead_wrapper)
	regs_to_64	x1, x2, x3
	mov	w2, w4
	b	sys_readahead
ENDPROC(compat_sys_readahead_wrapper)

ENTRY(compat_sys_fadvise64_64_wrapper)
	mov	w6, w1
	regs_to_64	x1, x2, x3
	regs_to_64	x2, x4, x5
	mov	w3, w6
	b	sys_fadvise64_64
ENDPROC(compat_sys_fadvise64_64_wrapper)

ENTRY(compat_sys_sync_file_range2_wrapper)
	regs_to_64	x2, x2, x3
	regs_to_64	x3, x4, x5
	b	sys_sync_file_range2
ENDPROC(compat_sys_sync_file_range2_wrapper)

ENTRY(compat_sys_fallocate_wrapper)
	regs_to_64	x2, x2, x3
	regs_to_64	x3, x4, x5
	b	sys_fallocate
ENDPROC(compat_sys_fallocate_wrapper)

ENTRY(compat_sys_statfs64_wrapper)
	mov	w3, #84
	cmp	w1, #88
	csel	w1, w3, w1, eq
	b	compat_sys_statfs64
ENDPROC(compat_sys_statfs64_wrapper)

ENTRY(compat_sys_fstatfs64_wrapper)
	mov	w3, #84
	cmp	w1, #88
	csel	w1, w3, w1, eq
	b	compat_sys_fstatfs64
ENDPROC(compat_sys_fstatfs64_wrapper)
