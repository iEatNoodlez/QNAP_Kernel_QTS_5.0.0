/*
 * Copyright IBM Corporation, 2010
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/nfs_fs.h>
#include <linux/xattr.h>
#include <linux/nfs_idmap.h>
#include <linux/nfs4_acl.h>
#include "nfs4idmap.h"
#include "nfs4_fs.h"

#ifdef CONFIG_NFSV4_FS_RICHACL
#include <linux/richacl_xattr.h>

#define GET32(var, buf, buflen) do {				\
		buflen -= sizeof(uint32_t);                     \
		if (buflen < 0)                                 \
			goto failed_xattr_to_acl;               \
		var = (uint32_t)ntohl((*(uint32_t *)(buf)));    \
		buf += sizeof(uint32_t);                        \
	} while (0)

static int nfs4_xattr_to_acl(struct nfs_server *server, struct nfs4_acl **acl,
			     void *buf, int buflen)
{
	int ret;
	char *name;
	uint32_t naces, namelen;
	struct nfs4_ace *ace;
	*acl = NULL;

	/* get the number of ace */
	GET32(naces, buf, buflen);

	*acl = kmalloc(sizeof(struct nfs4_acl) +
		       naces * sizeof(struct nfs4_ace), GFP_KERNEL);
	if (*acl == NULL)
		return -ENOMEM;
	(*acl)->naces = naces;
	for (ace = (*acl)->aces; ace < (*acl)->aces + naces; ace++) {

		GET32(ace->type, buf, buflen);
		GET32(ace->flag, buf, buflen);
		GET32(ace->access_mask, buf, buflen);

		GET32(namelen, buf, buflen);
		/*
		 * check whether we have enough space in buf
		 * Should the last ace have just namelen ? FIXME
		 */
		if (buflen <  (XDR_QUADLEN(namelen)  << 2))
			goto failed_xattr_to_acl;

		name = kmalloc(namelen + 1 , GFP_KERNEL);
		if (name == NULL)
			goto failed_xattr_to_acl;
		memcpy(name, buf, namelen);
		name[namelen] = '\0';
		buf += XDR_QUADLEN(namelen)  << 2;
		buflen -= XDR_QUADLEN(namelen)  << 2;

		ace->whotype = nfs4_acl_get_whotype(name , namelen);
		if (ace->whotype != NFS4_ACL_WHO_NAMED) {
			ace->who_uid.val = 0;
			ace->who_gid.val = 0;
		} else if (ace->flag & NFS4_ACE_IDENTIFIER_GROUP)
			ret = nfs_map_group_to_gid(server, name,
						    namelen, &ace->who_gid);
		else
			ret = nfs_map_name_to_uid(server, name,
						  namelen, &ace->who_uid);
		kfree(name);
	}
	return 0;

failed_xattr_to_acl:
	kfree(*acl);
	return -ENOMEM;
}

static bool
nfsv4_xattr_richacl_list(struct dentry *dentry)
{
	return nfs4_server_supports_acls(NFS_SERVER(dentry->d_inode));
}

static int
nfsv4_xattr_richacl_get(const struct xattr_handler *handler,
			struct dentry *unused, struct inode *inode,
			const char *name, void *buffer, size_t buflen)
{
	size_t size;
	struct richacl *racl;
	struct nfs4_acl *acl;
	struct nfs_server *server;
	int ret, size_request = 0;

	if (strcmp(name, "") != 0)
		return -EINVAL;

	server = NFS_SERVER(inode);
	if (buflen == 0) {
		ssize_t len;
		/*
		 * request is to find the size of buffer
		 * needed to hold the xattr value
		 */
		size_request = 1;
		len = nfs4_proc_get_acl(inode, NULL, 0);
		if (len < 0)
			return len;
		buffer = kmalloc(len, GFP_KERNEL);
		if (!buffer)
			/*FIXME!! what should be the error */
			return -ENOMEM;
		buflen = len;
	}
	ret = nfs4_proc_get_acl(inode, buffer, buflen);
	if (ret <= 0)
		goto free_acl_buffer;

	ret = nfs4_xattr_to_acl(server, &acl, buffer, ret);
	if (ret)
		goto free_acl_buffer;

	racl = nfs4_acl_nfsv4_to_richacl(acl);
	if (IS_ERR(racl)) {
		ret = PTR_ERR(racl);
		goto free_nfsv4_acl;
	}

	size = richacl_xattr_size(racl);
	if (!size_request) {
		if (size > buflen) {
			ret = -ERANGE;
			goto free_richacl;
		}
		richacl_to_xattr(racl, buffer);
		ret = size;
	} else
		/* return the max of richacl/nfsv4 acl */
		ret = max(size, buflen);

free_richacl:
	richacl_put(racl);

free_nfsv4_acl:
	kfree(acl);

free_acl_buffer:
	if (size_request)
		kfree(buffer);
	return ret;
}

static int nfsv4acl_xattr_size(int naces)
{
	int enc_len;
	/* size of naces */
	enc_len = sizeof(uint32_t);

	/* total size of all naces */
	enc_len += naces * 4 * sizeof(uint32_t);
	enc_len += naces * (XDR_QUADLEN(IDMAP_NAMESZ) << 2);

	return enc_len;
}

static int
nfsv4acl_write_who(int who, char *p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s2t_map); i++) {
		if (s2t_map[i].type == who) {
			memcpy(p, s2t_map[i].string, s2t_map[i].stringlen);
			return s2t_map[i].stringlen;
		}
	}
	BUG();
	return -1;
}

static int nfsv4acl_to_xattr(struct nfs_server *server,
			     struct nfs4_acl *acl, void **buffer)
{
	void *buf;
	int ret, enc_len = 0;
	struct nfs4_ace *ace;

	enc_len = nfsv4acl_xattr_size(acl->naces);
	*buffer = buf = kmalloc(enc_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	enc_len = 0;
	*((uint32_t *)buf) = htonl(acl->naces);
	buf += sizeof(uint32_t);
	enc_len += sizeof(uint32_t);

	for (ace = acl->aces; ace < acl->aces + acl->naces; ace++) {
		*((uint32_t *)buf) = htonl(ace->type);
		buf += sizeof(uint32_t);
		enc_len += sizeof(uint32_t);
		*((uint32_t *)buf) = htonl(ace->flag);
		buf += sizeof(uint32_t);
		enc_len += sizeof(uint32_t);
		*((uint32_t *)buf) = htonl(ace->access_mask);
		buf += sizeof(uint32_t);
		enc_len += sizeof(uint32_t);

		if (ace->whotype != NFS4_ACL_WHO_NAMED) {
			ret = nfsv4acl_write_who(ace->whotype,
					 (char *)(buf + sizeof(uint32_t)));
			if (ret > 0)
				*((uint32_t *)buf) = htonl(ret);
			else {
				enc_len = -EINVAL;
				goto free_xattr_buffer;
			}
		} else if (ace->flag & NFS4_ACE_IDENTIFIER_GROUP)
			ret = nfs_map_gid_to_group(server, ace->who_gid,
					   (char *)(buf + sizeof(uint32_t)),
					   IDMAP_NAMESZ);
		else
			ret = nfs_map_uid_to_name(server, ace->who_uid,
					  (char *)(buf + sizeof(uint32_t)),
					  IDMAP_NAMESZ);
		if (ret > 0)
			*((uint32_t *)buf) = htonl(ret);
		else {
			enc_len = -EINVAL;
			goto free_xattr_buffer;
		}
		buf += (XDR_QUADLEN(ret)  << 2) + sizeof(uint32_t);
		enc_len += (XDR_QUADLEN(ret)  << 2) + sizeof(uint32_t);
	}

	return enc_len;

free_xattr_buffer:
	kfree(buf);
	return enc_len;
}

static int
nfsv4_xattr_richacl_set(const struct xattr_handler *handler,
			struct dentry *unused, struct inode *inode,
			const char *name, const void *value,
			size_t size, int flags)
{
	int ret;
	struct nfs4_acl *acl;
	struct nfs_server *server;
	void *acl_xattr_buffer;
	struct richacl *racl = NULL;

	if (strcmp(name, "") != 0)
		return -EINVAL;

	server = NFS_SERVER(inode);
	if (value) {
		racl = richacl_from_xattr(value, size);
		if (IS_ERR(racl))
			return PTR_ERR(racl);

		ret = richacl_apply_masks(&racl);
		if (ret)
			goto free_richacl;

		acl = nfs4_acl_richacl_to_nfsv4(racl);
		if (IS_ERR(acl)) {
			ret = PTR_ERR(acl);
			goto free_richacl;
		}
		ret = nfsv4acl_to_xattr(server, acl, &acl_xattr_buffer);
		if (ret < 0)
			goto free_richacl;

		size = ret;
	} else
		acl_xattr_buffer = NULL;

	ret = nfs4_proc_set_acl(inode, acl_xattr_buffer, size);

	kfree(acl_xattr_buffer);
free_richacl:
	richacl_put(racl);
	return ret;
}

const struct xattr_handler nfsv4_xattr_richacl_handler = {
	.prefix	= RICHACL_XATTR,
	.list	= nfsv4_xattr_richacl_list,
	.get	= nfsv4_xattr_richacl_get,
	.set	= nfsv4_xattr_richacl_set,
};
#endif
