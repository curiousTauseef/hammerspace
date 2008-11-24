#ifndef __KERNEL__
#ifndef trace
#define trace trace_off
#endif

#define main notmain0
#include "balloc.c"
#undef main

#define main notmain1
#include "dleaf.c"
#undef main

#define main notmain3
#include "dir.c"
#undef main

#define main notmain2
#include "xattr.c"
#undef main

#define iattr_notmain_from_inode
#define main iattr_notmain_from_inode
#include "ileaf.c"
#undef main

#define main notmain4
#include "btree.c"
#undef main
#endif /* !__KERNEL */

#include "tux3.h"

#undef trace
#define trace trace_on

/*
 * Extrapolate from single buffer flush or blockread to opportunistic exent IO
 *
 * For write, try to include adjoining buffers above and below:
 *  - stop at first uncached or clean buffer in either direction
 *
 * For read (essentially readahead):
 *  - stop at first present buffer
 *  - stop at end of file
 *
 * For both, stop when extent is "big enough", whatever that means.
 */
void guess_extent(struct buffer_head *buffer, block_t *start, block_t *limit, int write)
{
	struct inode *inode = buffer_inode(buffer);
	block_t ends[2] = { bufindex(buffer), bufindex(buffer) };
	for (int up = !write; up < 2; up++) {
		while (ends[1] - ends[0] + 1 < MAX_EXTENT) {
			block_t next = ends[up] + (up ? 1 : -1);
//			struct buffer_head *nextbuf = peekblk(buffer->map, next);
			struct buffer_head *nextbuf = NULL;
			if (!nextbuf) {
				if (write)
					break;
				if (next > inode->i_size >> tux_sb(inode->i_sb)->blockbits)
					break;
			} else {
				unsigned stop = write ? !buffer_dirty(nextbuf) : buffer_empty(nextbuf);
				brelse(nextbuf);
				if (stop)
					break;
			}
			ends[up] = next; /* what happens to the beer you send */
		}
	}
	*start = ends[0];
	*limit = ends[1] + 1;
}

int filemap_extent_io(struct buffer_head *buffer, int write)
{
	struct inode *inode = buffer_inode(buffer);
	struct sb *sb = tux_sb(inode->i_sb);
	trace("%s inode 0x%Lx block 0x%Lx", write ? "write" : "read", (L)tux_inode(inode)->inum, (L)bufindex(buffer));
	if (bufindex(buffer) & (-1LL << MAX_BLOCKS_BITS))
		return -EIO;
	int levels = tux_inode(inode)->btree.root.depth, try = 0, i, err;
	if (!levels) {
		if (!write) {
			trace("unmapped block %Lx", (L)bufindex(buffer));
			memset(bufdata(buffer), 0, sb->blocksize);
			set_buffer_uptodate(buffer);
			return 0;
		}
		return -EIO;
	}
	if (write && buffer_empty(buffer))
		warn("egad, writing an invalid buffer");
	if (!write && buffer_dirty(buffer))
		warn("egad, reading a dirty buffer");

	block_t start, limit;
	guess_extent(buffer, &start, &limit, write);
	printf("---- extent 0x%Lx/%Lx ----\n", (L)start, (L)limit - start);
	struct extent seg[1000];
	struct tux_path *path = alloc_path(levels + 1);
	if (!path)
		return -ENOMEM;

	if ((err = probe(&tux_inode(inode)->btree, start, path))) {
		free_path(path);
		return err;
	}
retry:
	//assert(start >= this_key(path, levels))
	/* do not overlap next leaf */
	if (limit > next_key(path, levels))
		limit = next_key(path, levels);
	unsigned segs = 0;
	struct dleaf *leaf = bufdata(path[levels].buffer);
	struct dwalk *walk = &(struct dwalk){ };
dleaf_dump(&tux_inode(inode)->btree, leaf);
	/* Probe below io start to include overlapping extents */
	dwalk_probe(leaf, sb->blocksize, walk, 0); // start at beginning of leaf just for now

	/* skip extents below start */
	for (struct extent *extent; (extent = dwalk_next(walk));)
		if (dwalk_index(walk) + extent_count(*extent) > start) {
			if (dwalk_index(walk) <= start)
				dwalk_back(walk);
			break;
		}
	struct dwalk rewind = *walk;
	printf("prior extents:");
	for (struct extent *extent; (extent = dwalk_next(walk));)
		printf(" 0x%Lx => %Lx/%x;", (L)dwalk_index(walk), (L)extent_block(*extent), extent_count(*extent));
	printf("\n");

	if (dleaf_groups(leaf))
		printf("---- rewind to 0x%Lx => %Lx/%x ----\n", (L)dwalk_index(&rewind), (L)extent_block(*rewind.extent), extent_count(*rewind.extent));
	*walk = rewind;

	struct extent *next_extent = NULL;
	block_t index = start, offset = 0;
	while (index < limit) {
		trace("index %Lx, limit %Lx", (L)index, (L)limit);
		if (next_extent) {
			trace("emit %Lx/%x", (L)extent_block(*next_extent), extent_count(*next_extent));
			seg[segs++] = *next_extent;

			unsigned count = extent_count(*next_extent);
			if (start > dwalk_index(walk))
				count -= start - dwalk_index(walk);
			index += count;
		}
		block_t next_index = limit;
		if ((next_extent = dwalk_next(walk))) {
			next_index = dwalk_index(walk);
			trace("next_index = %Lx", (L)next_index);
			if (next_index < start) {
				offset = start - next_index;
				next_index = start;
			}
		}
		if (index < next_index) {
			int gap = next_index - index;
			trace("index = %Lx, offset = %Li, next = %Lx, gap = %i", (L)index, (L)offset, (L)next_index, gap);
			if (index + gap > limit)
				gap = limit - index;
			trace("fill gap at %Lx/%x", (L)index, gap);
			block_t block = 0;
			if (write) {
				block = balloc_extent(sb, gap); // goal ???
				if (block == -1)
					goto nospace; // clean up !!!
			}
			seg[segs++] = make_extent(block, gap);
			index += gap;
		}
	}

	if (write) {
		while (next_extent) {
			trace("save tail");
			seg[segs++] = *next_extent;
			next_extent = dwalk_next(walk);
		}
	}

	printf("segs (offset = %Lx):", (L)offset);
	for (i = 0, index = start; i < segs; i++) {
		printf(" %Lx => %Lx/%x;", (L)index - offset, (L)extent_block(seg[i]), extent_count(seg[i]));
		index += extent_count(seg[i]);
	}
	printf(" (%i)\n", segs);

	if (write) {
		*walk = rewind;
		for (i = 0, index = start - offset; i < segs; i++, index += extent_count(seg[i]))
			dwalk_mock(walk, index, make_extent(extent_block(seg[i]), extent_count(seg[i])));
		trace("need %i data and %i index bytes", walk->mock.free, -walk->mock.used);
		trace("need %i bytes, %u bytes free", walk->mock.free - walk->mock.used, dleaf_free(&tux_inode(inode)->btree, leaf));
		if (dleaf_free(&tux_inode(inode)->btree, leaf) <= walk->mock.free - walk->mock.used) {
			trace_on("--------- split leaf ---------");
			assert(!try);
			if ((err = btree_leaf_split(&tux_inode(inode)->btree, path, 0)))
				goto eek;
			try = 1;
			goto retry;
		}

		*walk = rewind;
		if (dleaf_groups(leaf))
			dwalk_chop_after(walk);
		for (i = 0, index = start - offset; i < segs; i++) {
			trace("pack 0x%Lx => %Lx/%x", (L)index, (L)extent_block(seg[i]), extent_count(seg[i]));
			dwalk_pack(walk, index, make_extent(extent_block(seg[i]), extent_count(seg[i])));
			index += extent_count(seg[i]);
		}
		set_buffer_dirty(path[tux_inode(inode)->btree.root.depth].buffer);

		//dleaf_dump(&tux_inode(inode)->btree, leaf);
		/* assert we used exactly the expected space */
		/* assert(??? == ???); */
		/* check leaf */
		if (0)
			goto eek;
	}

	unsigned skip = offset;
	for (i = 0, index = start - offset; !err && index < limit; i++) {
		unsigned count = extent_count(seg[i]);
		trace_on("extent 0x%Lx/%x => %Lx", (L)index, count, (L)extent_block(seg[i]));
		for (int j = skip; !err && j < count; j++) {
			block_t block = extent_block(seg[i]) + j;
			buffer = blockget(mapping(inode), index + j);
			trace_on("block 0x%Lx => %Lx", (L)bufindex(buffer), (L)block);
			if (write) {
//				err = diskwrite(dev->fd, bufdata(buffer), sb->blocksize, block << dev->bits);
			} else {
				if (!block) { /* block zero is never allocated */
					trace("zero fill buffer");
					memset(bufdata(buffer), 0, sb->blocksize);
					continue;
				}
//				err = diskread(dev->fd, bufdata(buffer), sb->blocksize, block << dev->bits);
			}
//			brelse(set_buffer_uptodate(buffer)); // leave empty if error ???
		}
		index += count;
		skip = 0;
	}
	release_path(path, levels + 1);
	free_path(path);
	return err;
nospace:
	err = -ENOSPC;
eek:
	warn("could not add extent to tree: %d", err);
	release_path(path, levels + 1);
	free_path(path);
	// free blocks and try to clean up ???
	return -EIO;
}

int tux3_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create)
{
	if (create)
		return -EIO;

	struct sb *sbi = tux_sb(inode->i_sb);
	int levels = tux_inode(inode)->btree.root.depth, i, err;
	if (!levels) {
		trace("unmapped block %Lx", (L)iblock);
		return 0;
	}

	bh_result->b_blocknr = iblock;

	block_t start = iblock, limit = iblock + 1;
	struct extent seg[10];
	struct tux_path *path = alloc_path(levels + 1);
	if (!path)
		return -ENOMEM;

	if ((err = probe(&tux_inode(inode)->btree, start, path))) {
		free_path(path);
		return err;
	}

	//assert(start >= this_key(path, levels))
	/* do not overlap next leaf */
	if (limit > next_key(path, levels))
		limit = next_key(path, levels);
	unsigned segs = 0;
	struct dleaf *leaf = bufdata(path[levels].buffer);
	struct dwalk *walk = &(struct dwalk){ };
	dleaf_dump(&tux_inode(inode)->btree, leaf);
	/* Probe below io start to include overlapping extents */
	dwalk_probe(leaf, sbi->blocksize, walk, 0); // start at beginning of leaf just for now

	/* skip extents below start */
	for (struct extent *extent; (extent = dwalk_next(walk));)
		if (dwalk_index(walk) + extent_count(*extent) > start) {
			if (dwalk_index(walk) <= start)
				dwalk_back(walk);
			break;
		}
	struct dwalk rewind = *walk;
	printf("prior extents:");
	for (struct extent *extent; (extent = dwalk_next(walk));)
		printf(" 0x%Lx => %Lx/%x;", (L)dwalk_index(walk), (L)extent_block(*extent), extent_count(*extent));
	printf("\n");

	if (dleaf_groups(leaf))
		printf("---- rewind to 0x%Lx => %Lx/%x ----\n", (L)dwalk_index(&rewind), (L)extent_block(*rewind.extent), extent_count(*rewind.extent));
	*walk = rewind;

	struct extent *next_extent = NULL;
	block_t index = start, offset = 0;
	while (index < limit) {
		trace("index %Lx, limit %Lx", (L)index, (L)limit);
		if (next_extent) {
			trace("emit %Lx/%x", (L)extent_block(*next_extent), extent_count(*next_extent));
			seg[segs++] = *next_extent;

			unsigned count = extent_count(*next_extent);
			if (start > dwalk_index(walk))
				count -= start - dwalk_index(walk);
			index += count;
		}
		block_t next_index = limit;
		if ((next_extent = dwalk_next(walk))) {
			next_index = dwalk_index(walk);
			trace("next_index = %Lx", (L)next_index);
			if (next_index < start) {
				offset = start - next_index;
				next_index = start;
			}
		}
		if (index < next_index) {
			int gap = next_index - index;
			trace("index = %Lx, offset = %Li, next = %Lx, gap = %i", (L)index, (L)offset, (L)next_index, gap);
			if (index + gap > limit)
				gap = limit - index;
			trace("fill gap at %Lx/%x", (L)index, gap);
			block_t block = 0;
			if (create) {
				block = balloc_extent(sbi, gap); // goal ???
				if (block == -1)
					goto nospace; // clean up !!!
			}
			seg[segs++] = make_extent(block, gap);
			index += gap;
		}
	}

	printf("segs (offset = %Lx):", (L)offset);
	for (i = 0, index = start; i < segs; i++) {
		printf(" %Lx => %Lx/%x;", (L)index - offset, (L)extent_block(seg[i]), extent_count(seg[i]));
		index += extent_count(seg[i]);
	}
	printf(" (%i)\n", segs);


	unsigned skip = offset;
	for (i = 0, index = start - offset; !err && index < limit; i++) {
		unsigned count = extent_count(seg[i]);
		trace_on("extent 0x%Lx/%x => %Lx", (L)index, count, (L)extent_block(seg[i]));
		for (int j = skip; !err && j < count; j++) {
			block_t block = extent_block(seg[i]) + j;
			map_bh(bh_result, inode->i_sb, block);
			goto out;
		}
		index += count;
		skip = 0;
	}
out:
	release_path(path, levels + 1);
	free_path(path);

	return err;

nospace:
	err = -ENOSPC;
	warn("could not add extent to tree: %d", err);
	release_path(path, levels + 1);
	free_path(path);
	// free blocks and try to clean up ???
	return -EIO;
}

struct buffer_head *blockread(struct address_space *mapping, block_t block)
{
	struct inode *inode = mapping->host;
	struct buffer_head map_bh;
	struct page page;
	int err;

	printk("%s: ino %Lu, block %Lu\n", __func__, tux_inode(inode)->inum, block);

	page.mapping = mapping;
	map_bh.b_page = &page;
	map_bh.b_state = 0;
	map_bh.b_blocknr = 0;
	map_bh.b_size = 1 << inode->i_blkbits;

	err = tux3_get_block(mapping->host, block, &map_bh, 0);
	if (err)
		return NULL;

	return sb_bread(inode->i_sb, map_bh.b_blocknr);
}

struct buffer_head *blockget(struct address_space *mapping, block_t block)
{
	struct inode *inode = mapping->host;
	struct buffer_head map_bh;
	struct page page;
	int err;

	printk("%s: ino %Lu, block %Lu\n", __func__, tux_inode(inode)->inum, block);

	page.mapping = mapping;
	map_bh.b_page = &page;
	map_bh.b_state = 0;
	map_bh.b_blocknr = 0;
	map_bh.b_size = 1 << inode->i_blkbits;

	err = tux3_get_block(mapping->host, block, &map_bh, 0);
	if (err)
		return NULL;

	return sb_getblk(inode->i_sb, map_bh.b_blocknr);
}

#ifndef __KERNEL__
int filemap_block_read(struct buffer_head *buffer)
{
	return filemap_extent_io(buffer, 0);
}

int filemap_block_write(struct buffer_head *buffer)
{
	return filemap_extent_io(buffer, 1);
}

struct map_ops filemap_ops = {
	.blockread = filemap_block_read,
	.blockwrite = filemap_block_write,
};

#ifndef filemap_included
int main(int argc, char *argv[])
{
	if (argc < 2)
		error("usage: %s <volname>", argv[0]);
	char *name = argv[1];
	fd_t fd = open(name, O_CREAT|O_TRUNC|O_RDWR, S_IRWXU);
	ftruncate(fd, 1 << 24);
	u64 size = 0;
	if (fdsize64(fd, &size))
		error("fdsize64 failed for '%s' (%s)", name, strerror(errno));
	struct dev *dev = &(struct dev){ fd, .bits = 8 };
	SB = &(struct sb){
		.max_inodes_per_block = 64,
		.entries_per_node = 20,
		.devmap = new_map(dev, NULL),
		.blockbits = dev->bits,
		.blocksize = 1 << dev->bits,
		.blockmask = (1 << dev->bits) - 1,
		.volblocks = size >> dev->bits,
	};
	sb->bitmap = &(struct inode){ .i_sb = sb, .map = new_map(dev, &filemap_ops) },
	sb->bitmap->map->inode = sb->bitmap;
	init_buffers(dev, 1 << 20);
	struct inode *inode = &(struct inode){ .i_sb = sb, .map = new_map(dev, &filemap_ops) };
	inode->btree = new_btree(sb, &dtree_ops); // error???
	inode->map->inode = inode;
	inode = inode;

#if 1
	brelse_dirty(blockread(mapping(inode), 0));
	brelse_dirty(blockread(mapping(inode), 1));
	printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));
	filemap_extent_io(blockget(mapping(inode), 0), 0);
	return 0;
#endif

#if 1
	filemap_extent_io(blockget(mapping(inode), 5), 0);
	return 0;
#endif

#if 0
	for (int i = 0; i < 20; i++) {
		brelse_dirty(blockget(mapping(inode), i));
		printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));
	}
	return 0;
#endif

#if 1
	brelse_dirty(blockget(mapping(inode), 5));
	brelse_dirty(blockget(mapping(inode), 6));
	printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));

	brelse_dirty(blockget(mapping(inode), 6));
	brelse_dirty(blockget(mapping(inode), 7));
	printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));

	return 0;
#endif

	brelse_dirty(blockget(mapping(inode), 0));
	brelse_dirty(blockget(mapping(inode), 1));
	brelse_dirty(blockget(mapping(inode), 2));
	brelse_dirty(blockget(mapping(inode), 3));
	printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));

	brelse_dirty(blockget(mapping(inode), 0));
	brelse_dirty(blockget(mapping(inode), 1));
	brelse_dirty(blockget(mapping(inode), 2));
	brelse_dirty(blockget(mapping(inode), 3));
	brelse_dirty(blockget(mapping(inode), 4));
	brelse_dirty(blockget(mapping(inode), 5));
	brelse_dirty(blockget(mapping(inode), 6));
	printf("flush... %s\n", strerror(-flush_buffers(mapping(inode))));

	//show_buffers(mapping(inode));
	return 0;
}
#endif
#endif /* !__KERNEL */