// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <stdint.h>

#include "znr.h"

#include <linux/fsmap.h>
#include <xfs/xfs.h>

/*
 * Get FS geometry.
 */
static struct xfs_fsop_geom		fs_geo;

/*
 * Return the number of bytes in an rtgroup.
 */
static inline uint64_t bytes_per_rtgroup(const struct xfs_fsop_geom *fsgeo)
{
	if (!fsgeo->rgcount)
		return 0;

	return (uint64_t)fsgeo->rgextents * fsgeo->rtextsize *
		fsgeo->blocksize;
}

static int znr_xfs_init_fs(struct znr_fs_file *f)
{
	int ret;

	memset(&fs_geo, 0, sizeof(fs_geo));

	ret = ioctl(f->fd, XFS_IOC_FSGEOMETRY, &fs_geo);
	if (ret == -1) {
		fprintf(stderr, "Get XFS geometry failed (%s)\n",
			strerror(errno));
		return -errno;
	}

	return 0;
}

/*
 * Get XFS file extents, based on xfsprogs/io/bmap.c
 */
static int znr_xfs_get_file_extents_map(struct znr_fs_file *f,
					struct getbmapx **_map,
					unsigned int *nr_entries)
{
	unsigned int map_size = 32;
	struct getbmapx *map;
	struct fsxattr fsx;
	int retry = 0;
	int ret;

again:
	/* Get actual extent count using FSGETXATTR. */
	ret = xfsctl(f->path, f->fd, FS_IOC_FSGETXATTR, &fsx);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to get XFS file %s attributes (%s)\n",
			f->path, strerror(errno));
		return -errno;
	}

	if (!fsx.fsx_nextents) {
		/* No extents */
		*_map = NULL;
		*nr_entries = 0;
		return 0;
	}

	/* Allocate map with header + entries */
	map_size = fsx.fsx_nextents * 2 + 1;
	map = malloc(map_size * sizeof(*map));
	if (!map) {
		fprintf(stderr,
			"Failed to allocate memory for extent query\n");
		return -ENOMEM;
	}

	memset(map, 0, sizeof(*map));
	map->bmv_length = -1;
	map->bmv_count = map_size + 1;
	map->bmv_iflags = 0;

	ret = xfsctl(f->path, f->fd, XFS_IOC_GETBMAPX, map);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to get file %s extents map (%s)\n",
			f->path, strerror(errno));
		free(map);
		return -errno;
	}

	/* If we got all extents, we are done */
	if (map->bmv_entries >= map->bmv_count - 1) {
		free(map);
		retry++;
		if (retry >= 2) {
			fprintf(stderr,
				"Failed to get all extents for file %s\n",
				f->path);
			return -EIO;
		}
		goto again;
	}

	if (map->bmv_entries <= 0) {
		/* No extents */
		free(map);
		*_map = NULL;
		*nr_entries = 0;
		return 0;
	}

	*_map = map;
	*nr_entries = map->bmv_entries;

	return 0;
}

/*
 * Fill an extent information using data from GETBMAPX.
 */
static void znr_xfs_get_file_extent_from_map(struct znr_extent *extent,
					struct getbmapx *bmap, unsigned int idx,
					int is_rt, off_t bstart, off_t bbperag)
{
	unsigned long long offset_start, offset_end;
	const char *ag_rg = is_rt ? "RG" : "AG";
	off_t bno;

	/* Calculate RG information once for this extent */
	if (bbperag > 0) {
		bno = bmap->bmv_block - bstart;
		offset_start = bno % bbperag;
		offset_end = offset_start + bmap->bmv_length - 1;
	} else {
		offset_start = 0;
		offset_end = 0;
	}

	/* Populate extent data. */
	extent->type = ZNR_FS_FILE_EXTENT;
	extent->idx = idx;
	extent->sector = bmap->bmv_block;
	extent->nr_sectors = bmap->bmv_length;
	snprintf(extent->info, sizeof(extent->info) - 1,
		 "<tt><b>-- Extent %u --</b>\n"
		 "  <b>File Offset</b>:  [%llu..%llu]\n"
		 "  <b>Length</b>:       %llu\n"
		 "  <b>%s Range</b>:     [%llu..%llu]\n"
		 "  <b>Sector Range</b>: [%llu..%llu]\n"
		 "</tt>\n",
		 idx,
		 (unsigned long long)bmap->bmv_offset,
		 (unsigned long long)(bmap->bmv_offset + bmap->bmv_length - 1),
		 extent->nr_sectors,
		 ag_rg, offset_start, offset_end,
		 extent->sector,
		 extent->sector + extent->nr_sectors - 1);
}

/*
 * Map XFS extents to zones and populate all extent information
 * Gets extent data directly from GETBMAPX ioctl
 */
static int znr_xfs_get_file_extents(struct znr_fs_file *f,
				    struct znr_extent **extents,
				    unsigned int *nr_extents)
{
	struct fsxattr fsx;
	struct getbmapx *map = NULL;
	unsigned int ext_idx = 0, nr_ext = 0;
	unsigned int bmv_entries;
	struct znr_extent *ext = NULL;
	off_t bstart = 0;
	off_t bbperag = 0;
	int is_rt = 0;
	int i, ret;

	/* Get file attributes to check if realtime */
	ret = xfsctl(f->path, f->fd, XFS_IOC_FSGETXATTR, &fsx);
	if (ret < 0) {
		fprintf(stderr, "Failed to get file attributes: %s\n",
			strerror(errno));
		goto out;
	}

	/* Get all extents in one call */
	ret = znr_xfs_get_file_extents_map(f, &map, &bmv_entries);
	if (ret)
		goto out;

	/* Count actual extents (skip holes and delayed allocation) */
	for (i = 0; i < map[0].bmv_entries; i++) {
		if (map[i + 1].bmv_block != -1 && map[i + 1].bmv_block != -2)
			nr_ext++;
	}
	if (!nr_ext)
		goto out;

	/* Allocate extent array */
	ext = calloc(nr_ext, sizeof(struct znr_extent));
	if (!ext) {
		fprintf(stderr, "Failed to allocate memory for extents\n");
		ret = -ENOMEM;
		goto out;
	}

	/* Setup extent array */
	if (fsx.fsx_xflags & FS_XFLAG_REALTIME) {
		is_rt = 1;
		bstart = fs_geo.rtstart * (fs_geo.blocksize / BBSIZE);
		bbperag = bytes_per_rtgroup(&fs_geo) / BBSIZE;
	} else {
		bstart = 0;
		bbperag = (off_t)fs_geo.agblocks *
			(off_t)fs_geo.blocksize / BBSIZE;
	}

	for (i = 0; i < map[0].bmv_entries && ext_idx < nr_ext; i++) {
		/* Skip holes and delayed allocation */
		if (map[i + 1].bmv_block == -1 || map[i + 1].bmv_block == -2)
			continue;

		ext[ext_idx].ino = f->ino;
		znr_xfs_get_file_extent_from_map(&ext[ext_idx],
						 &map[i + 1], ext_idx, is_rt,
						 bstart, bbperag);
		ext_idx++;
	}

out:
	if (ret || !nr_ext) {
		free(ext);
		*extents = NULL;
		*nr_extents = 0;
	} else {
		*extents = ext;
		*nr_extents = nr_ext;
	}

	free(map);

	return ret;
}

static int znr_xfs_get_range_extents(unsigned long long sector,
				     unsigned long long nr_sectors,
				     struct znr_extent **extents,
				     unsigned int *nr_extents)
{
	struct fsmap_head *head, *new_head;
	struct fsmap *l, *h, *p;
	unsigned int map_size = 512;
	int ret = 0;
	off_t bperag, bperrtg, agoff, agno;
	uint64_t start;
	unsigned int i, max_extents, nr_ext = 0;
	unsigned long long sector_end = sector + nr_sectors;
	struct znr_extent *ext, *ext_base = NULL;
	char *ag_rg;

	bperag = (off_t)fs_geo.agblocks * (off_t)fs_geo.blocksize;
	bperrtg = bytes_per_rtgroup(&fs_geo);

	head = malloc(fsmap_sizeof(map_size));
	if (!head) {
		fprintf(stderr, "No memory for FSMAP");
		return -ENOMEM;
	}

	memset(head, 0, fsmap_sizeof(map_size));
	l = head->fmh_keys;	/* Start of range */
	h = head->fmh_keys + 1;	/* End of range */

	/* Setup Zone Query Range */
	l->fmr_physical = BBTOB(sector);
	l->fmr_owner = 0;
	l->fmr_offset = 0;
	l->fmr_flags = 0;

	h->fmr_physical = BBTOB(sector_end);
	h->fmr_owner = ULLONG_MAX;
	h->fmr_flags = UINT_MAX;
	h->fmr_offset = ULLONG_MAX;

	/* Determine device type from filesystem geometry */
	if (fs_geo.rtstart) {
		/* This seems to work... but is it correct? */
		if (sector > fs_geo.rtstart * (fs_geo.blocksize / BBSIZE)) {
			l->fmr_device = XFS_DEV_RT;
			h->fmr_device = XFS_DEV_RT;
		} else {
			l->fmr_device = XFS_DEV_DATA;
			h->fmr_device = XFS_DEV_DATA;
		}
	} else {
		fprintf(stderr, "TODO: Unsupported filesystem geometry\n");
		ret = -ENOTSUP;
		goto out;
	}

	/*
	 * Allocate our array of extents, up to the maximum we can have in a
	 * zone (zone size / FS block size).
	 */
	max_extents = nr_sectors * 512 / fs_geo.blocksize;
	ext = calloc(max_extents, sizeof(struct znr_extent));
	if (!ext) {
		fprintf(stderr, "No memory for extents\n");
		ret = -ENOMEM;
		goto out;
	}
	ext_base = ext;

	/* Get the count first */
	head->fmh_count = 0;

	while (1) {
		ret = ioctl(znr.mnt_dir.fd, FS_IOC_GETFSMAP, head);
		if (ret < 0) {
			fprintf(stderr, "Failed to get FSMAP: (%d) - [%d] %s",
				ret, errno, strerror(errno));
			ret = -EIO;
			goto out;
		}

		/* If we queried for count and need more space, reallocate */
		if (!head->fmh_count && head->fmh_entries > map_size) {
			map_size = head->fmh_entries;
			new_head = realloc(head, fsmap_sizeof(map_size));
			if (!new_head) {
				fprintf(stderr, "No memory for FSMAP");
				ret = -ENOMEM;
				goto out;
			}
			head = new_head;
			head->fmh_count = map_size;
			/* Restart loop to fetch data with new count */
			continue;
		}

		/* Set count for first real query if we had enough space */
		if (!head->fmh_count) {
			head->fmh_count = map_size;
			continue;
		}

		if (!head->fmh_entries)
			break;

		for (i = 0; i < head->fmh_entries; i++) {
			p = &head->fmh_recs[i];
			/*
			 * The fmr_owner field contains the owner of the extent.
			 * This is an inode number unless FMR_OF_SPECIAL_OWNER
			 * is set in the fmr_flags field, in which case the
			 * value is determined by the filesystem. We only want
			 * actual file data extents, so ignore the rest.
			 */
			if (p->fmr_flags & FMR_OF_SPECIAL_OWNER)
				continue;

			/*
			 * Only include extents within the zone boundaries,
			 * in case the high/low key phys_addr filter failed
			 */
			if (BTOBBT(p->fmr_physical) < sector ||
			    BTOBBT(p->fmr_physical) >= sector_end)
				continue;

			if (p->fmr_device == XFS_DEV_DATA) {
				agno = p->fmr_physical / bperag;
				agoff = p->fmr_physical - (agno * bperag);
				ag_rg = "AG";

			} else if (p->fmr_device == XFS_DEV_RT &&
				   fs_geo.rgcount > 0) {
				start = p->fmr_physical -
					fs_geo.rtstart * fs_geo.blocksize;
				agno = start / bperrtg;
				if (agno < 0)
					agno = -1;
				agoff = start % bperrtg;
				ag_rg = "RG";
			} else {
				continue;
			}

			if (nr_ext >= max_extents) {
				fprintf(stderr,
					"Too many extents in range %llu + %llu (max: %u)\n",
					sector, nr_sectors, max_extents);
				ret = -EIO;
				goto out;
			}

			ext->type = ZNR_FS_ZONE_EXTENT;
			ext->idx = nr_ext;
			ext->ino = p->fmr_owner;
			ext->sector = BTOBBT(p->fmr_physical);
			ext->nr_sectors = BTOBBT(p->fmr_length);

			snprintf(ext->info, sizeof(ext->info) - 1,
				 "<tt><b>-- Extent %u --</b>\n"
				 "  <b>Inode</b>:        %llu\n"
				 "  <b>File Offset</b>:  [%llu..%llu]\n"
				 "  <b>Length</b>:       %llu\n"
				 "  <b>%s Range</b>:     [%lld..%lld)\n"
				 "  <b>Sector Range</b>: [%llu..%llu]\n"
				 "</tt>\n",
				 ext->idx,
				 ext->ino,
				 BTOBBT(p->fmr_offset),
				 BTOBBT(p->fmr_offset + p->fmr_length - 1),
				 BTOBBT(p->fmr_length),
				 ag_rg, BTOBBT(agoff),
				 BTOBBT(agoff + p->fmr_length - 1),
				 BTOBBT(p->fmr_physical),
				 BTOBBT(p->fmr_physical + p->fmr_length - 1));

			nr_ext++;
			ext++;
		}

		/* Check if we are done */
		p = &head->fmh_recs[head->fmh_entries - 1];
		if (p->fmr_flags & FMR_OF_LAST)
			break;

		/* Advance to the next batch */
		fsmap_advance(head);
	}

out:
	if (ret || !nr_ext) {
		free(ext_base);
		*extents = NULL;
		*nr_extents = 0;
	} else {
		*extents = ext_base;
		*nr_extents = nr_ext;
	}

	free(head);
	return ret;
}

static int znr_xfs_get_nr_blockgroups(unsigned int *nr_blockgroups)
{
	unsigned long nr_bgs;

	if (!fs_geo.blocksize)
		return -ENODEV;

	nr_bgs = (unsigned long)fs_geo.agcount + fs_geo.rgcount;
	if (nr_bgs > UINT_MAX)
		return -ENOSPC;

	*nr_blockgroups = nr_bgs;
	return 0;
}

static int znr_xfs_get_blockgroups(struct znr_bg **blockgroups,
				   unsigned int *nr_blockgroups)
{
	struct znr_bg *bgs = NULL;
	unsigned int max_blockgroups = 0;
	unsigned long rtstart, bbperag, bbperrg, rgcount, agcount;
	unsigned int ag, rg, idx = 0;
	int ret;

	if (!blockgroups || !nr_blockgroups)
		return -EINVAL;

	ret = znr_xfs_get_nr_blockgroups(&max_blockgroups);
	if (ret)
		return ret;

	bgs = calloc(max_blockgroups, sizeof(struct znr_bg));
	if (!bgs)
		return -ENOMEM;

	bbperrg = bytes_per_rtgroup(&fs_geo) / BBSIZE;
	bbperag = (off_t)fs_geo.agblocks * (off_t)fs_geo.blocksize / BBSIZE;
	rtstart = (off_t)fs_geo.rtstart * (off_t)fs_geo.blocksize / BBSIZE;
	rgcount = fs_geo.rgcount;
	agcount = fs_geo.agcount;

	/* Into blockgroups, contiguously append all AGs and RGs. */
	for (ag = 0; ag < agcount && idx < max_blockgroups; ag++, idx++) {
		bgs[idx].sector = ag * bbperag;
		bgs[idx].nr_sectors = bbperag;
	}

	for (rg = 0; rg < rgcount && idx < max_blockgroups; rg++, idx++) {
		bgs[idx].sector = rtstart + (rg * bbperrg);
		bgs[idx].nr_sectors = bbperrg;
	}

	*blockgroups = bgs;
	*nr_blockgroups = max_blockgroups;
	return 0;
}

const struct znr_fs_ops znr_xfs_ops = {
	.init_fs		= znr_xfs_init_fs,
	.get_file_extents	= znr_xfs_get_file_extents,
	.get_extents_in_range	= znr_xfs_get_range_extents,
	.get_blockgroups        = znr_xfs_get_blockgroups,
};
