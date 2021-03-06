/*
 * Copyright (c) 2008, Daniel Phillips
 * Copyright (c) 2008, OGAWA Hirofumi
 */

#include "tux3.h"

#ifndef trace
#define trace trace_off
#endif

int unpack_sb(struct sb *sb, struct disksuper *super, struct root *iroot, int silent)
{
	u64 iroot_val = from_be_u64(super->iroot);
	u64 hroot_val = from_be_u64(super->hroot);/*  DREAMZ */
	if (memcmp(super->magic, (char[])SB_MAGIC, sizeof(super->magic))) {
		if (!silent)
			printf("invalid superblock [%Lx]\n",
			       (L)from_be_u64(*(be_u64 *)super->magic));
		return -EINVAL;
	}

//	sb->rootbuf;
	sb->blockbits = from_be_u16(super->blockbits);
	sb->blocksize = 1 << sb->blockbits;
	sb->blockmask = (1 << sb->blockbits) - 1;
	/* FIXME: those should be initialized based on blocksize. */
	sb->entries_per_node = 20;
	sb->max_inodes_per_block = 64;
//	sb->version;
	sb->atomref_base = 1 << (40 - sb->blockbits); // see xattr.c
	sb->unatom_base = sb->atomref_base + (1 << (34 - sb->blockbits));
	sb->volblocks = from_be_u64(super->volblocks);
	sb->freeblocks = from_be_u64(super->freeblocks);
	sb->nextalloc = from_be_u64(super->nextalloc);
	sb->atomgen = from_be_u32(super->atomgen);
	sb->freeatom = from_be_u32(super->freeatom);
	sb->dictsize = from_be_u64(super->dictsize);
	sb->entries_per_bucket = (sb->blocksize - offsetof(struct bucket,entries)) / sizeof(struct bucket_entry);
	*iroot = unpack_root(iroot_val);
	sb->htree.root = unpack_root(hroot_val);

	return 0;
}

void pack_sb(struct sb *sb, struct disksuper *super)
{
	super->blockbits = to_be_u16(sb->blockbits);
	super->volblocks = to_be_u64(sb->volblocks);
	super->freeblocks = to_be_u64(sb->freeblocks); // probably does not belong here
	super->nextalloc = to_be_u64(sb->nextalloc); // probably does not belong here
	super->atomgen = to_be_u32(sb->atomgen); // probably does not belong here
	super->freeatom = to_be_u32(sb->freeatom); // probably does not belong here
	super->dictsize = to_be_u64(sb->dictsize); // probably does not belong here
	super->iroot = to_be_u64(pack_root(&itable_btree(sb)->root));
	super->hroot = to_be_u64(pack_root(&sb->htree.root));/*  DREAMZ  */	
}
