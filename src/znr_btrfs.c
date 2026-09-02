// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 Western Digital Corporation or its affiliates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <endian.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "znr.h"

#include <linux/btrfs.h>
#include <linux/btrfs_tree.h>

#define BTRFS_SEARCH_BUF_SIZE	(128 * 1024)

/*
 * A single stripe of a btrfs chunk that lives on the inspected device. This
 * gives us the logical <-> physical translation as well as the physical
 * placement of block groups. On a single device filesystem a chunk has one
 * stripe, except for DUP metadata which has two stripes on the same device.
 */
struct znr_btrfs_chunk {
	uint64_t	logical;
	uint64_t	physical;
	uint64_t	length;
	uint64_t	type;
};

static struct znr_btrfs_chunk	*btrfs_chunks;
static unsigned int		btrfs_nr_chunks;

/*
 * The btrfs device id of the device being inspected. A btrfs filesystem can
 * span multiple devices, so chunk stripes are filtered to only those that live
 * on this device.
 */
static uint64_t			btrfs_devid;

/* Unaligned little-endian accessors for tree search results. */
static inline uint64_t get_le64(const void *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

static inline uint32_t get_le32(const void *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static inline uint16_t get_le16(const void *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return le16toh(v);
}

/*
 * Callback invoked for every item returned by a tree search. Return 0 to
 * continue, a negative errno to abort with an error, or a positive value to
 * stop the search successfully.
 */
typedef int (*znr_btrfs_item_cb)(const struct btrfs_ioctl_search_header *sh,
				 const void *item, void *priv);

/*
 * Iterate over all items in [min_key, max_key] of the given tree, invoking
 * @cb for each one. Handles the multi-call search protocol of TREE_SEARCH_V2.
 */
static int znr_btrfs_tree_search(int fd, uint64_t tree_id,
				 uint64_t min_obj, uint64_t max_obj,
				 uint32_t min_type, uint32_t max_type,
				 uint64_t min_off, uint64_t max_off,
				 znr_btrfs_item_cb cb, void *priv)
{
	struct btrfs_ioctl_search_args_v2 *args;
	struct btrfs_ioctl_search_key *sk;
	int ret = 0;

	args = malloc(sizeof(*args) + BTRFS_SEARCH_BUF_SIZE);
	if (!args) {
		fprintf(stderr, "No memory for btrfs tree search\n");
		return -ENOMEM;
	}

	sk = &args->key;
	memset(sk, 0, sizeof(*sk));
	sk->tree_id = tree_id;
	sk->min_objectid = min_obj;
	sk->max_objectid = max_obj;
	sk->min_type = min_type;
	sk->max_type = max_type;
	sk->min_offset = min_off;
	sk->max_offset = max_off;
	sk->min_transid = 0;
	sk->max_transid = (uint64_t)-1;
	args->buf_size = BTRFS_SEARCH_BUF_SIZE;

	while (1) {
		struct btrfs_ioctl_search_header sh;
		const char *buf = (const char *)args->buf;
		uint64_t last_obj = 0, last_off = 0;
		uint32_t last_type = 0;
		unsigned long off = 0;
		unsigned int i;

		sk->nr_items = 4096;

		if (ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args) < 0) {
			fprintf(stderr, "btrfs tree search (tree %llu) failed (%s)\n",
				(unsigned long long)tree_id, strerror(errno));
			ret = -errno;
			goto out;
		}

		if (sk->nr_items == 0)
			break;

		for (i = 0; i < sk->nr_items; i++) {
			const void *item;

			memcpy(&sh, buf + off, sizeof(sh));
			off += sizeof(sh);
			item = buf + off;
			off += sh.len;

			last_obj = sh.objectid;
			last_type = sh.type;
			last_off = sh.offset;

			ret = cb(&sh, item, priv);
			if (ret)
				goto out;
		}

		/* Advance the search key past the last returned item. */
		sk->min_objectid = last_obj;
		sk->min_type = last_type;
		sk->min_offset = last_off;
		if (sk->min_offset != (uint64_t)-1) {
			sk->min_offset++;
		} else {
			sk->min_offset = 0;
			if (sk->min_type < UINT8_MAX) {
				sk->min_type++;
			} else {
				sk->min_type = 0;
				if (sk->min_objectid == (uint64_t)-1)
					break;
				sk->min_objectid++;
			}
		}

		if (sk->min_objectid > sk->max_objectid)
			break;
	}

out:
	free(args);

	return ret > 0 ? 0 : ret;
}

static const struct znr_btrfs_chunk *znr_btrfs_chunk_for_logical(uint64_t logical)
{
	unsigned int i;

	for (i = 0; i < btrfs_nr_chunks; i++) {
		const struct znr_btrfs_chunk *c = &btrfs_chunks[i];

		if (logical >= c->logical && logical < c->logical + c->length)
			return c;
	}

	return NULL;
}

static int znr_btrfs_logical_to_physical(uint64_t logical, uint64_t *physical)
{
	const struct znr_btrfs_chunk *c = znr_btrfs_chunk_for_logical(logical);

	if (!c)
		return -ENOENT;

	*physical = c->physical + (logical - c->logical);
	return 0;
}

static int znr_btrfs_chunk_cmp(const void *a, const void *b)
{
	const struct znr_btrfs_chunk *ca = a;
	const struct znr_btrfs_chunk *cb = b;

	if (ca->physical < cb->physical)
		return -1;
	if (ca->physical > cb->physical)
		return 1;
	return 0;
}

struct znr_btrfs_chunk_ctx {
	struct znr_btrfs_chunk	*chunks;
	unsigned int		nr_chunks;
	unsigned int		max_chunks;
	int			ret;
};

static int znr_btrfs_chunk_cb(const struct btrfs_ioctl_search_header *sh,
			      const void *item, void *priv)
{
	struct znr_btrfs_chunk_ctx *ctx = priv;
	uint64_t length, type;
	uint16_t num_stripes;
	const char *sp;
	uint16_t s;

	if (sh->type != BTRFS_CHUNK_ITEM_KEY)
		return 0;

	if (sh->len < sizeof(struct btrfs_chunk))
		return 0;

	length = get_le64(item + offsetof(struct btrfs_chunk, length));
	type = get_le64(item + offsetof(struct btrfs_chunk, type));
	num_stripes = get_le16(item + offsetof(struct btrfs_chunk, num_stripes));

	if (!num_stripes)
		return 0;

	/* Each stripe on the inspected device becomes a chunk map entry. */
	sp = (const char *)item + offsetof(struct btrfs_chunk, stripe);
	for (s = 0; s < num_stripes; s++) {
		const char *st = sp + s * sizeof(struct btrfs_stripe);
		struct znr_btrfs_chunk *c;

		if ((size_t)(st + sizeof(struct btrfs_stripe) -
			     (const char *)item) > sh->len)
			break;

		/* Skip stripes that live on a different device. */
		if (get_le64(st + offsetof(struct btrfs_stripe, devid)) !=
		    btrfs_devid)
			continue;

		if (ctx->nr_chunks >= ctx->max_chunks) {
			ctx->ret = -ENOSPC;
			return -ENOSPC;
		}

		c = &ctx->chunks[ctx->nr_chunks++];
		c->logical = sh->offset;
		c->length = length;
		c->type = type;
		c->physical = get_le64(st + offsetof(struct btrfs_stripe,
						     offset));
	}

	return 0;
}

/* Find the btrfs device id matching the block device zonar is inspecting. */
static int znr_btrfs_find_devid(int fd)
{
	struct btrfs_ioctl_fs_info_args fi;
	char real_dev[PATH_MAX], real_path[PATH_MAX];
	uint64_t id;

	memset(&fi, 0, sizeof(fi));
	if (ioctl(fd, BTRFS_IOC_FS_INFO, &fi) < 0) {
		fprintf(stderr, "btrfs fs info failed (%s)\n", strerror(errno));
		return -errno;
	}

	if (!realpath(znr.dev_path, real_dev))
		snprintf(real_dev, sizeof(real_dev), "%s", znr.dev_path);

	for (id = 1; id <= fi.max_id; id++) {
		struct btrfs_ioctl_dev_info_args di;

		memset(&di, 0, sizeof(di));
		di.devid = id;
		/* Device ids are not necessarily contiguous, skip the gaps. */
		if (ioctl(fd, BTRFS_IOC_DEV_INFO, &di) < 0)
			continue;

		di.path[sizeof(di.path) - 1] = '\0';
		if (!di.path[0])
			continue;

		if (!realpath((char *)di.path, real_path))
			snprintf(real_path, sizeof(real_path), "%s", di.path);

		if (strcmp(real_path, real_dev) == 0) {
			btrfs_devid = id;
			return 0;
		}
	}

	fprintf(stderr, "Could not find btrfs device id for %s\n",
		znr.dev_path);
	return -ENODEV;
}

static int znr_btrfs_init_fs(struct znr_fs_file *f)
{
	struct znr_btrfs_chunk_ctx ctx = { 0 };
	int ret;

	ret = znr_btrfs_find_devid(f->fd);
	if (ret)
		return ret;

	ctx.max_chunks = 4096;
	ctx.chunks = calloc(ctx.max_chunks, sizeof(*ctx.chunks));
	if (!ctx.chunks)
		return -ENOMEM;

	ret = znr_btrfs_tree_search(f->fd, BTRFS_CHUNK_TREE_OBJECTID,
				    0, (uint64_t)-1,
				    BTRFS_CHUNK_ITEM_KEY, BTRFS_CHUNK_ITEM_KEY,
				    0, (uint64_t)-1,
				    znr_btrfs_chunk_cb, &ctx);
	if (ret) {
		free(ctx.chunks);
		return ret;
	}

	qsort(ctx.chunks, ctx.nr_chunks, sizeof(*ctx.chunks),
	      znr_btrfs_chunk_cmp);

	free(btrfs_chunks);
	btrfs_chunks = ctx.chunks;
	btrfs_nr_chunks = ctx.nr_chunks;

	return 0;

}

static int znr_btrfs_get_blockgroups(struct znr_blockgroup **bgs_out,
				     unsigned int *nr_bgs)
{
	struct znr_blockgroup *bgs;
	unsigned int i;

	if (!bgs_out || !nr_bgs)
		return -EINVAL;

	if (!btrfs_nr_chunks)
		return -ENODEV;

	bgs = calloc(btrfs_nr_chunks, sizeof(*bgs));
	if (!bgs)
		return -ENOMEM;

	/* One block group per chunk stripe, in device (physical) order. */
	for (i = 0; i < btrfs_nr_chunks; i++) {
		bgs[i].sector = btrfs_chunks[i].physical >> SECTOR_SHIFT;
		bgs[i].nr_sectors = btrfs_chunks[i].length >> SECTOR_SHIFT;
	}

	*bgs_out = bgs;
	*nr_bgs = btrfs_nr_chunks;

	return 0;
}

static int znr_btrfs_report_blockgroups(struct znr_blockgroup *bgs,
					unsigned int bg_no, unsigned int nr_bgs)
{
	/*
	 * btrfs does not expose the per block group allocation offset (its
	 * write pointer) to user space, so there is no filesystem write
	 * pointer to update. The device zone write pointer is refreshed by the
	 * generic block group layer for zoned devices.
	 */
	return nr_bgs;
}

/* Resolve the path of the directory containing @ino within subvolume @root. */
static void znr_btrfs_ino_path(uint64_t root, uint64_t ino,
			       char *out, size_t out_size)
{
	struct btrfs_ioctl_ino_lookup_args args;

	out[0] = '\0';

	memset(&args, 0, sizeof(args));
	args.treeid = root;
	args.objectid = ino;

	if (ioctl(znr.mnt_dir.fd, BTRFS_IOC_INO_LOOKUP, &args) < 0)
		return;

	args.name[sizeof(args.name) - 1] = '\0';
	snprintf(out, out_size, "%s", args.name);
}

struct znr_btrfs_ext_arr {
	struct znr_extent	*ext;
	unsigned int		nr;
	unsigned int		cap;
};

static struct znr_extent *znr_btrfs_ext_next(struct znr_btrfs_ext_arr *arr)
{
	if (arr->nr == arr->cap) {
		unsigned int cap = arr->cap ? arr->cap * 2 : 64;
		struct znr_extent *ext;

		ext = realloc(arr->ext, cap * sizeof(*ext));
		if (!ext)
			return NULL;

		memset(ext + arr->cap, 0, (cap - arr->cap) * sizeof(*ext));
		arr->ext = ext;
		arr->cap = cap;
	}

	return &arr->ext[arr->nr++];
}

struct znr_btrfs_file_ctx {
	struct znr_btrfs_ext_arr	arr;
	unsigned long long		ino;
	int				ret;
};

static int znr_btrfs_file_extent_cb(const struct btrfs_ioctl_search_header *sh,
				    const void *item, void *priv)
{
	struct znr_btrfs_file_ctx *ctx = priv;
	uint64_t disk_bytenr, ext_offset, num_bytes, logical, physical;
	struct znr_extent *e;
	uint8_t type;

	if (sh->type != BTRFS_EXTENT_DATA_KEY)
		return 0;

	if (sh->len < sizeof(struct btrfs_file_extent_item))
		return 0;

	type = *((const uint8_t *)item +
		 offsetof(struct btrfs_file_extent_item, type));

	/* Inline data is not placed in a block group, skip it. */
	if (type == BTRFS_FILE_EXTENT_INLINE)
		return 0;

	disk_bytenr = get_le64(item + offsetof(struct btrfs_file_extent_item,
					       disk_bytenr));
	/* A zero disk_bytenr denotes a hole. */
	if (!disk_bytenr)
		return 0;

	ext_offset = get_le64(item + offsetof(struct btrfs_file_extent_item,
					      offset));
	num_bytes = get_le64(item + offsetof(struct btrfs_file_extent_item,
					     num_bytes));

	logical = disk_bytenr + ext_offset;
	if (znr_btrfs_logical_to_physical(logical, &physical))
		return 0;

	e = znr_btrfs_ext_next(&ctx->arr);
	if (!e) {
		ctx->ret = -ENOMEM;
		return -ENOMEM;
	}

	e->type = ZNR_FS_FILE_EXTENT;
	e->idx = ctx->arr.nr - 1;
	e->ino = ctx->ino;
	e->sector = physical >> SECTOR_SHIFT;
	e->nr_sectors = num_bytes >> SECTOR_SHIFT;
	snprintf(e->info, sizeof(e->info) - 1,
		 "<tt><b>-- Extent %u --</b>\n"
		 "  <b>File Offset</b>:  [%llu..%llu]\n"
		 "  <b>Length</b>:       %llu\n"
		 "  <b>Logical</b>:      [%llu..%llu]\n"
		 "  <b>Sector Range</b>: [%llu..%llu]\n"
		 "</tt>\n",
		 e->idx,
		 (unsigned long long)(sh->offset >> SECTOR_SHIFT),
		 (unsigned long long)((sh->offset + num_bytes - 1) >> SECTOR_SHIFT),
		 (unsigned long long)e->nr_sectors,
		 (unsigned long long)(logical >> SECTOR_SHIFT),
		 (unsigned long long)((logical + num_bytes - 1) >> SECTOR_SHIFT),
		 (unsigned long long)e->sector,
		 (unsigned long long)(e->sector + e->nr_sectors - 1));

	return 0;
}

static int znr_btrfs_get_file_extents(struct znr_fs_file *f,
				      struct znr_extent **extents,
				      unsigned int *nr_extents)
{
	struct znr_btrfs_file_ctx ctx = { 0 };
	struct btrfs_ioctl_ino_lookup_args lookup;
	int ret;

	ctx.ino = f->ino;

	/* Find the subvolume tree that contains this file. */
	memset(&lookup, 0, sizeof(lookup));
	lookup.treeid = 0;
	lookup.objectid = f->ino;
	if (ioctl(f->fd, BTRFS_IOC_INO_LOOKUP, &lookup) < 0) {
		fprintf(stderr, "btrfs inode lookup for %s failed (%s)\n",
			f->path, strerror(errno));
		return -errno;
	}

	ret = znr_btrfs_tree_search(f->fd, lookup.treeid,
				    f->ino, f->ino,
				    BTRFS_EXTENT_DATA_KEY, BTRFS_EXTENT_DATA_KEY,
				    0, (uint64_t)-1,
				    znr_btrfs_file_extent_cb, &ctx);
	if (!ret)
		ret = ctx.ret;

	if (ret || !ctx.arr.nr) {
		free(ctx.arr.ext);
		*extents = NULL;
		*nr_extents = 0;
		return ret;
	}

	*extents = ctx.arr.ext;
	*nr_extents = ctx.arr.nr;

	return 0;
}

struct znr_btrfs_range_ctx {
	struct znr_btrfs_ext_arr	arr;
	const struct znr_btrfs_chunk	*chunk;
	uint64_t			log_lo;
	uint64_t			log_hi;
	int				ret;
};

/*
 * Extract the owning root and inode from the first inline back reference of an
 * extent item. Only a plain (non-shared) data back reference carries the inode
 * inline; anything else leaves the inode unresolved.
 */
static void znr_btrfs_extent_owner(const void *item, uint32_t item_len,
				   uint64_t *root, uint64_t *ino)
{
	const void *iref = item + sizeof(struct btrfs_extent_item);
	uint8_t type;

	*root = 0;
	*ino = 0;

	if (item_len < sizeof(struct btrfs_extent_item) + 1)
		return;

	type = *(const uint8_t *)iref;
	if (type != BTRFS_EXTENT_DATA_REF_KEY)
		return;

	if (item_len < sizeof(struct btrfs_extent_item) + 1 +
	    sizeof(struct btrfs_extent_data_ref))
		return;

	*root = get_le64(iref + 1 +
			 offsetof(struct btrfs_extent_data_ref, root));
	*ino = get_le64(iref + 1 +
			offsetof(struct btrfs_extent_data_ref, objectid));
}

static int znr_btrfs_range_extent_cb(const struct btrfs_ioctl_search_header *sh,
				     const void *item, void *priv)
{
	struct znr_btrfs_range_ctx *ctx = priv;
	uint64_t flags, elog, elen, physical, root, ino;
	char path[BTRFS_INO_LOOKUP_PATH_MAX];
	struct znr_extent *e;

	if (sh->type != BTRFS_EXTENT_ITEM_KEY)
		return 0;

	if (sh->len < sizeof(struct btrfs_extent_item))
		return 0;

	flags = get_le64(item + offsetof(struct btrfs_extent_item, flags));
	if (!(flags & BTRFS_EXTENT_FLAG_DATA))
		return 0;

	elog = sh->objectid;
	elen = sh->offset;

	/* Only keep extents that intersect the requested logical range. */
	if (elog + elen <= ctx->log_lo || elog >= ctx->log_hi)
		return 0;

	physical = ctx->chunk->physical + (elog - ctx->chunk->logical);

	znr_btrfs_extent_owner(item, sh->len, &root, &ino);
	if (root && ino)
		znr_btrfs_ino_path(root, ino, path, sizeof(path));
	else
		snprintf(path, sizeof(path), "<unresolved>");

	e = znr_btrfs_ext_next(&ctx->arr);
	if (!e) {
		ctx->ret = -ENOMEM;
		return -ENOMEM;
	}

	e->type = ZNR_FS_ZONE_EXTENT;
	e->idx = ctx->arr.nr - 1;
	e->ino = ino;
	e->sector = physical >> SECTOR_SHIFT;
	e->nr_sectors = elen >> SECTOR_SHIFT;
	snprintf(e->info, sizeof(e->info) - 1,
		 "<tt><b>-- Extent %u --</b>\n"
		 "  <b>Inode</b>:        %llu\n"
		 "  <b>Path</b>:         %.128s\n"
		 "  <b>Length</b>:       %llu\n"
		 "  <b>Logical</b>:      [%llu..%llu]\n"
		 "  <b>Sector Range</b>: [%llu..%llu]\n"
		 "</tt>\n",
		 e->idx,
		 (unsigned long long)ino,
		 path,
		 (unsigned long long)e->nr_sectors,
		 (unsigned long long)(elog >> SECTOR_SHIFT),
		 (unsigned long long)((elog + elen - 1) >> SECTOR_SHIFT),
		 (unsigned long long)e->sector,
		 (unsigned long long)(e->sector + e->nr_sectors - 1));

	return 0;
}

static int znr_btrfs_get_range_extents(unsigned long long sector,
				       unsigned long long nr_sectors,
				       struct znr_extent **extents,
				       unsigned int *nr_extents)
{
	struct znr_btrfs_range_ctx ctx = { 0 };
	uint64_t phys_start = (uint64_t)sector << SECTOR_SHIFT;
	uint64_t phys_end = (uint64_t)(sector + nr_sectors) << SECTOR_SHIFT;
	unsigned int i;
	int ret = 0;

	/*
	 * The requested physical range may span several chunks. For each one,
	 * translate the overlapping physical window to a logical range and
	 * search the extent tree for data extents in it.
	 */
	for (i = 0; i < btrfs_nr_chunks && !ret; i++) {
		const struct znr_btrfs_chunk *c = &btrfs_chunks[i];
		uint64_t ov_start, ov_end;

		ov_start = phys_start > c->physical ? phys_start : c->physical;
		ov_end = phys_end < c->physical + c->length ?
			phys_end : c->physical + c->length;
		if (ov_start >= ov_end)
			continue;

		ctx.chunk = c;
		ctx.log_lo = c->logical + (ov_start - c->physical);
		ctx.log_hi = c->logical + (ov_end - c->physical);

		ret = znr_btrfs_tree_search(znr.mnt_dir.fd,
					    BTRFS_EXTENT_TREE_OBJECTID,
					    c->logical, ctx.log_hi - 1,
					    BTRFS_EXTENT_ITEM_KEY,
					    BTRFS_EXTENT_ITEM_KEY,
					    0, (uint64_t)-1,
					    znr_btrfs_range_extent_cb, &ctx);
		if (!ret)
			ret = ctx.ret;
	}

	if (ret || !ctx.arr.nr) {
		free(ctx.arr.ext);
		*extents = NULL;
		*nr_extents = 0;
		return ret;
	}

	*extents = ctx.arr.ext;
	*nr_extents = ctx.arr.nr;

	return 0;
}

const struct znr_fs_ops znr_btrfs_ops = {
	.init_fs		= znr_btrfs_init_fs,
	.get_file_extents	= znr_btrfs_get_file_extents,
	.get_extents_in_range	= znr_btrfs_get_range_extents,
	.get_blockgroups	= znr_btrfs_get_blockgroups,
	.report_blockgroups	= znr_btrfs_report_blockgroups,
};
