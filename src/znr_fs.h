/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_FS_H
#define ZNR_FS_H

#include "config.h"
#include "znr_device.h"
#include "znr_bg.h"
#ifdef HAS_XFS
#include "znr_xfs.h"
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <mntent.h>
#include <sys/syscall.h>

enum znr_supported_fs {
	ZNR_FS_XFS,
	ZNR_FS_BTRFS,
	ZNR_FS_UNKNOWN,
};

/*
 * Supported file system.
 */
struct znr_fs {
	enum znr_supported_fs	type;
	const char		*name;

	const struct znr_fs_ops	*ops;
};

/*
 * Extent information.
 */
#define ZNR_FS_EXT_INFO_SIZE	352

enum znr_extent_type {
	ZNR_FS_FILE_EXTENT,
	ZNR_FS_ZONE_EXTENT,
};

struct znr_extent {
	enum znr_extent_type	type;
	unsigned int		idx;
	unsigned long long	ino;
	unsigned long long	sector;
	unsigned long long	nr_sectors;
	unsigned int		flags;
	char			info[ZNR_FS_EXT_INFO_SIZE];
} __attribute__ ((packed));

/*
 * File information
 */
struct znr_fs_file {
	char			*path;
	char			*relative_path;
	struct znr_fs		*fs;
	/* Valid if this is this file refers to the  mount directory */
	struct mntent		*mnt;
	unsigned long long	ino;
	off_t			size;
	mode_t			mode;
	int			fd;
};

struct znr_mnt_dir {
	union {
#ifdef HAS_XFS
		struct znr_fs_xfs	xfs;
#endif
		/*
		 * Incase we are building without any FS support (i.e. GUI only
		 * for remote use), define rsvd to avoid an empty union. Which
		 * would be undefined behaviour.
		 */
		uint8_t			rsvd;
	} fs;
	struct znr_fs_file		f;
};

/*
 * File system operations.
 */
struct znr_fs_ops {
	int (*init_fs)(struct znr_fs_file *f);
	int (*destroy_fs)(void);
	int (*get_file_extents)(struct znr_fs_file *f,
				struct znr_extent **extents,
				unsigned int *nr_extents);
	int (*get_extents_in_range)(unsigned long long sector,
				    unsigned long long nr_sectors,
				    unsigned int flags,
				    struct znr_extent **extents,
				    unsigned int *nr_extents);
	int (*get_blockgroups)(struct znr_blockgroup **bgs,
			       unsigned int *nr_bgs);
	int (*report_blockgroups)(struct znr_blockgroup *bgs,
				  unsigned int bg_no,
				  unsigned int nr_bgs);
};

static inline int znr_openat2(int dirfd, const char *pathname,
			      struct open_how *how, size_t size)
{
	return syscall(SYS_openat2, dirfd, pathname, how, size);
}

static inline bool znr_fs_ext_in_bg(struct znr_extent *ext,
				    struct znr_blockgroup *bg)
{
	/*
	 * This can be extended later as required, but we don't want to make
	 * calls to the filesystem for this assertion because it adds
	 * significant overhead to the GUIs responsiveness when extents are
	 * rendered. Particularly when connected to remote server over
	 * network.
	 */
	if (ext->flags != bg->fs_device_type)
		return false;

	return ext->sector >= bg->sector &&
	       ext->sector + ext->nr_sectors <= bg->sector + bg->nr_sectors;
}

/*
 * An FS specific helper function that returns true if metadata can be stored
 * in sequential write zones/blockgroups.
 */
static inline bool znr_fs_seqwr_has_metadata(struct znr_fs_file *f)
{
	if (!f)
		return false;

	switch(f->fs->type) {
	case ZNR_FS_BTRFS:
		return true;
	default:
		return false;
	}
}


/* XFS functions (znr_xfs.c) */
extern const struct znr_fs_ops znr_xfs_ops;

/* btrfs functions (znr_btrfs.c) */
extern const struct znr_fs_ops znr_btrfs_ops;

struct znr_fs *znr_fs_get(enum znr_supported_fs type);
int znr_fs_open(const char *path);
void znr_fs_close(void);

int znr_fs_get_file_extents_by_path(const char *path,
				    struct znr_fs_file **f,
				    struct znr_extent **extents,
				    unsigned int *nr_extents);
int znr_fs_get_file_extents_by_ino(unsigned long long ino,
				   struct znr_fs_file **f,
				   struct znr_extent **extents,
				   unsigned int *nr_extents);
void znr_fs_free_file(struct znr_fs_file *f);

int znr_fs_get_extents_in_range(unsigned long long sector,
				unsigned long long nr_sectors,
				unsigned int flags,
				struct znr_extent **ext, unsigned int *nr_ext);
int znr_fs_get_blockgroups(struct znr_blockgroup **bgs,
			   unsigned int *nr_bgs);
int znr_fs_report_blockgroups(struct znr_blockgroup *bgs,
			      unsigned int bg_no,
			      unsigned int nr_bgs);

#endif /* ZNR_FS_H */
