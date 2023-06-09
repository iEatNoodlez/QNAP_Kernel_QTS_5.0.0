/* SPDX-License-Identifier: GPL-2.0 */
/*
 * File: linux/nfsacl.h
 *
 * (C) 2003 Andreas Gruenbacher <agruen@suse.de>
 */
#ifndef __LINUX_NFS4ACL_H
#define __LINUX_NFS4ACL_H

#include <linux/posix_acl.h>
#include <linux/nfs4.h>
#ifdef CONFIG_NFSV4_FS_RICHACL
#include <linux/richacl.h>
#endif

static struct {
	char *string;
	int   stringlen;
	int type;
} s2t_map[] = {
	{
		.string    = "OWNER@",
		.stringlen = sizeof("OWNER@") - 1,
		.type      = NFS4_ACL_WHO_OWNER,
	},
	{
		.string    = "GROUP@",
		.stringlen = sizeof("GROUP@") - 1,
		.type      = NFS4_ACL_WHO_GROUP,
	},
	{
		.string    = "EVERYONE@",
		.stringlen = sizeof("EVERYONE@") - 1,
		.type      = NFS4_ACL_WHO_EVERYONE,
	},
};

static inline int
nfs4_acl_get_whotype(char *p, u32 len)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s2t_map); i++) {
		if (s2t_map[i].stringlen == len &&
			0 == memcmp(s2t_map[i].string, p, len))
			return s2t_map[i].type;
	}
	return NFS4_ACL_WHO_NAMED;
}

#ifdef CONFIG_NFSV4_FS_RICHACL
struct nfs4_acl *nfs4_acl_richacl_to_nfsv4(struct richacl *racl);
struct richacl *nfs4_acl_nfsv4_to_richacl(struct nfs4_acl *acl);

extern const struct xattr_handler nfsv4_xattr_richacl_handler;
#endif

#endif  /* __LINUX_NFS4ACL_H */
