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
#include <linux/fs.h>
#include <linux/nfs4_acl.h>
#ifdef CONFIG_NFSV4_FS_RICHACL
#include <linux/richacl.h>
#endif

#ifdef CONFIG_NFSV4_FS_RICHACL
static inline struct nfs4_acl *nfs4_acl_new(int n)
{
	struct nfs4_acl *acl;

	acl = kmalloc(sizeof(*acl) + n * sizeof(struct nfs4_ace), GFP_KERNEL);
	if (!acl)
		return NULL;

	acl->naces = 0;
	return acl;
}

/*
 * @racl should have file mask applied using
 * richacl_apply_masks
 */
struct nfs4_acl *
nfs4_acl_richacl_to_nfsv4(struct richacl *racl)
{
	struct nfs4_acl *acl;
	struct richace *race;
	struct nfs4_ace *ace;

	acl = nfs4_acl_new(racl->a_count);
	if (acl == NULL)
		return ERR_PTR(-ENOMEM);

	ace = acl->aces;
	richacl_for_each_entry(race, racl) {
		ace->type = race->e_type;
		ace->access_mask = race->e_mask;
		/*
		 * FIXME!! when we support dacl remove the
		 * clearing of ACE4_INHERITED_ACE
		 */
		ace->flag = race->e_flags &
			~(ACE4_SPECIAL_WHO | ACE4_INHERITED_ACE);
		if (richace_is_owner(race))
			ace->whotype = NFS4_ACL_WHO_OWNER;
		else if (richace_is_group(race))
			ace->whotype = NFS4_ACL_WHO_GROUP;
		else if (richace_is_everyone(race))
			ace->whotype = NFS4_ACL_WHO_EVERYONE;
		else {
			ace->whotype = NFS4_ACL_WHO_NAMED;
			if(race->e_flags & ACE4_IDENTIFIER_GROUP){
				ace->who_gid.val = race->e_id;
				ace->flag |= NFS4_ACE_IDENTIFIER_GROUP;
			} else {
				ace->who_uid.val = race->e_id;
			}
		}
		ace++;
		acl->naces++;
	}
	return acl;
}
EXPORT_SYMBOL(nfs4_acl_richacl_to_nfsv4);

struct richacl *
nfs4_acl_nfsv4_to_richacl(struct nfs4_acl *acl)
{
	struct richacl *racl;
	struct richace *race;
	struct nfs4_ace *ace;

	racl = richacl_alloc(acl->naces);
	if (racl == NULL)
		return ERR_PTR(-ENOMEM);
	race = racl->a_entries;
	for (ace = acl->aces; ace < acl->aces + acl->naces; ace++) {
		race->e_type  = ace->type;
		race->e_flags = ace->flag;
		race->e_mask  = ace->access_mask;
		switch (ace->whotype) {
		case NFS4_ACL_WHO_OWNER:
			richace_set_who(race, richace_owner_who);
			break;
		case NFS4_ACL_WHO_GROUP:
			richace_set_who(race, richace_group_who);
			break;
		case NFS4_ACL_WHO_EVERYONE:
			richace_set_who(race, richace_everyone_who);
			break;
		case NFS4_ACL_WHO_NAMED:
			if(ace->flag & NFS4_ACE_IDENTIFIER_GROUP) {
				race->e_flags |= ACE4_IDENTIFIER_GROUP;
				race->e_id = ace->who_gid.val;
			} else
				race->e_id = ace->who_uid.val;
			break;
		default:
			richacl_put(racl);
			return ERR_PTR(-EINVAL);
		}
		race++;
	}
	/*
	 * NFSv4 acl don't have file mask.
	 * Derive max mask out of ACE values
	 */
	richacl_compute_max_masks(racl);
	return racl;
}
EXPORT_SYMBOL(nfs4_acl_nfsv4_to_richacl);
#endif

MODULE_LICENSE("GPL");
