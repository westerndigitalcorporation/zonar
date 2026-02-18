/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_FS_H
#define ZNR_FS_H

#include "config.h"
#include "znr_device.h"
#include "znr_bg.h"

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>

enum znr_supported_fs {
	ZNR_FS_XFS,
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
	char			info[ZNR_FS_EXT_INFO_SIZE];
} __attribute__ ((packed));

/*
 * File information
 */
struct znr_fs_file {
	char			*path;
	char			*relative_path;
	struct znr_fs		*fs;
	unsigned long long	ino;
	off_t			size;
	mode_t			mode;
	int			fd;
};

/*
 * File system operations.
 */
struct znr_fs_ops {
	int (*init_fs)(struct znr_fs_file *f);
	int (*get_file_extents)(struct znr_fs_file *f,
				struct znr_extent **extents,
				unsigned int *nr_extents);
	int (*get_extents_in_range)(unsigned long long sector,
				    unsigned long long nr_sectors,
				    struct znr_extent **extents,
				    unsigned int *nr_extents);
	int (*get_blockgroups)(struct znr_bg **blockgroups,
			       unsigned int *nr_blockgroups);
};

static inline int znr_openat2(int dirfd, const char *pathname,
			      struct open_how *how, size_t size)
{
	return syscall(SYS_openat2, dirfd, pathname, how, size);
}

/* XFS functions (znr_xfs.c) */
extern const struct znr_fs_ops znr_xfs_ops;

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
				struct znr_extent **ext, unsigned int *nr_ext);
int znr_fs_get_blockgroups(struct znr_bg **blockgroups,
			   unsigned int *nr_blockgroups);

#endif /* ZNR_FS_H */
