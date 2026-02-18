// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <mntent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/magic.h>

#include "znr.h"

static struct znr_fs znrfs[] = {
#ifdef HAS_XFS
	{ ZNR_FS_XFS,		"XFS",	&znr_xfs_ops },
#endif
	{ ZNR_FS_UNKNOWN,	NULL,	NULL },
};

struct znr_fs *znr_fs_get(enum znr_supported_fs type)
{
	struct znr_fs *fs = &znrfs[0];

	if (!fs)
		return NULL;

	while (fs->type != ZNR_FS_UNKNOWN) {
		if (fs->type == type)
			return fs;
		fs++;
	}

	return NULL;
}

static int znr_fs_get_file_fs(struct znr_fs_file *f)
{
	struct statfs stf;

	if (statfs(f->path, &stf) < 0) {
		fprintf(stderr, "Statfs %s failed %d (%s)\n",
			f->path, errno, strerror(errno));
		return -errno;
	}

	switch (stf.f_type) {
#ifdef HAS_XFS
	case XFS_SUPER_MAGIC:
		f->fs = &znrfs[ZNR_FS_XFS];
		break;
#endif
	default:
		fprintf(stderr, "%s: unsupported file system\n", f->path);
		return -ENOTSUP;
	}

	return 0;
}

static int znr_fs_init_fs(struct znr_fs_file *f)
{
	return f->fs->ops->init_fs(f);
}

static void znr_fs_close_file(struct znr_fs_file *f)
{
	if (f->fd > 0)
		close(f->fd);
	f->fd = 0;
}

/*
 * Open a file.
 *
 * If @mnt_restricted is true, the file opened must reside within the mount
 * directory.
 */
static int znr_fs_open_file(struct znr_fs_file *f, bool mnt_restricted)
{
	struct stat st;
	struct open_how how = {
		.flags = O_RDONLY,
		.resolve = RESOLVE_IN_ROOT,
	};
	int ret;

	if (mnt_restricted) {
		f->relative_path = f->path;
		ret = asprintf(&f->path, "%s/%s", znr.mnt_dir.path, f->path);
		if (ret < 0)
			return ret;

		f->fd = znr_openat2(znr.mnt_dir.fd, f->relative_path,
				    &how, sizeof(how));
		if (f->fd < 0) {
			if (errno != ENOENT)
				fprintf(stderr, "Openat2 %s failed %d (%s)\n",
					f->path, errno, strerror(errno));
			return -errno;
		}
	} else {
		free(f->relative_path);
		f->relative_path = NULL;
		f->fd = open(f->path, O_RDONLY);
		if (f->fd < 0) {
			fprintf(stderr, "Open %s failed %d (%s)\n",
				f->path, errno, strerror(errno));
			return -errno;
		}
	}

	if (fstat(f->fd, &st) < 0) {
		if (mnt_restricted)
			fprintf(stderr, "Stat file path %s/%s failed %d (%s)\n",
				znr.mnt_dir.path, f->path,
				errno, strerror(errno));
		else
			fprintf(stderr, "Stat file path %s failed %d (%s)\n",
				f->path, errno, strerror(errno));
		return -errno;
	}

	f->ino = st.st_ino;
	f->size = st.st_size;
	f->mode = st.st_mode;

	ret = znr_fs_get_file_fs(f);
	if (ret)
		goto close;

	if (f->fs != znr.mnt_dir.fs) {
		fprintf(stderr,
			"File %s is not on the same file system as %s\n",
			f->path, znr.mnt_dir.path);
		ret = -EINVAL;
		goto close;
	}

	return 0;

close:
	znr_fs_close_file(f);

	return ret;
}

static int znr_fs_get_file_extents(struct znr_fs_file *f,
				   struct znr_extent **extents,
				   unsigned int *nr_extents)
{
	int ret;

	ret = znr_fs_open_file(f, true);
	if (ret != 0)
		return ret;

	if ((f->mode & S_IFMT) != S_IFREG) {
		fprintf(stderr, "%s is not a regular file\n", f->path);
		ret = -EINVAL;
		goto close;
	}

	/* Skip empty files */
	if (!f->size) {
		ret = 0;
		goto close;
	}

	ret = f->fs->ops->get_file_extents(f, extents, nr_extents);

close:
	znr_fs_close_file(f);

	return ret;
}

static struct znr_fs_file *znr_fs_alloc_file(const char *path)
{
	static struct znr_fs_file *f;

	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;

	if (path) {
		f->path = strdup(path);
		if (!f->path) {
			free(f);
			return NULL;
		}
	}

	return f;
}

static void znr_fs_clear_file(struct znr_fs_file *f)
{
	znr_fs_close_file(f);
	free(f->path);
	f->path = NULL;
	free(f->relative_path);
	f->relative_path = NULL;
	f->fs = NULL;
	f->ino = 0;
	f->size = 0;
	f->mode = 0;
}

void znr_fs_free_file(struct znr_fs_file *f)
{
	if (f) {
		znr_fs_clear_file(f);
		free(f);
	}
}

int znr_fs_get_file_extents_by_path(const char *path,
				    struct znr_fs_file **file,
				    struct znr_extent **extents,
				    unsigned int *nr_extents)
{
	struct znr_fs_file *f;
	int ret;

	f = znr_fs_alloc_file(path);
	if (!f)
		return -ENOMEM;

	if (znr.is_net_client)
		ret = znr_net_get_file_extents(&znr.ncli,
					       f->path, extents, nr_extents);
	else
		ret = znr_fs_get_file_extents(f, extents, nr_extents);
	if (ret) {
		znr_fs_free_file(f);
		return ret;
	}

	*file = f;

	return 0;
}

int znr_fs_get_file_extents_by_ino(unsigned long long ino,
				   struct znr_fs_file **file,
				   struct znr_extent **extents,
				   unsigned int *nr_extents)
{
	fprintf(stderr, "Getting files by inode number is not supported yet\n");

	*file = NULL;
	*extents = NULL;
	*nr_extents = 0;

	return -ENOTSUP;
}

int znr_fs_get_extents_in_range(unsigned long long sector,
				unsigned long long nr_sectors,
				struct znr_extent **ext, unsigned int *nr_ext)
{
	if (znr.is_net_client)
		return znr_net_get_extents_in_range(&znr.ncli, sector,
						    nr_sectors, ext, nr_ext);

	return znr.mnt_dir.fs->ops->get_extents_in_range(sector,
						nr_sectors, ext, nr_ext);
}

int znr_fs_get_blockgroups(struct znr_bg **blockgroups,
			   unsigned int *nr_blockgroups)
{
	if (znr.is_net_client)
		return znr_net_get_blockgroups(&znr.ncli, blockgroups,
					       nr_blockgroups);

	return znr.mnt_dir.fs->ops->get_blockgroups(blockgroups, nr_blockgroups);
}

int znr_fs_open(const char *path)
{
	struct mntent *mnt;
	struct stat st;
	FILE *mtab;
	int ret = 0;

	if (znr.is_net_client)
		return znr_net_get_mntdir_info(&znr.ncli);

	/* Check path is a directory */
	if (stat(path, &st) < 0) {
		fprintf(stderr, "Stat path %s failed %d (%s)\n",
			path, errno, strerror(errno));
		return -EINVAL;
	}

	if ((st.st_mode & S_IFMT) != S_IFDIR) {
		fprintf(stderr, "Path %s is not a directory\n", path);
		return -EINVAL;
	}

	/* Search mount directory */
	mtab = setmntent("/etc/mtab", "r");
	if (!mtab) {
		fprintf(stderr, "Failed to open /etc/mtab (%s)\n",
			strerror(errno));
		return -errno;
	}

	while (1) {
		mnt = getmntent(mtab);
		if (!mnt)
			break;

		if (strcmp(mnt->mnt_dir, path) == 0)
			break;
	}

	if (!mnt) {
		fprintf(stderr, "Directory %s is not a mount directory\n",
			path);
		ret = -EINVAL;
		goto end;
	}

	/* Check mount device */
	if (stat(mnt->mnt_fsname, &st) < 0) {
		fprintf(stderr, "Stat path %s failed %d (%s)\n",
			mnt->mnt_fsname, errno, strerror(errno));
		ret = -EINVAL;
		goto end;
	}

	if ((st.st_mode & S_IFMT) != S_IFBLK) {
		fprintf(stderr, "%s is not a block device\n",
			mnt->mnt_fsname);
		ret = -EINVAL;
		goto end;
	}

	ret = asprintf(&znr.mnt_dir.path, "%s", mnt->mnt_dir);
	if (ret < 0) {
		fprintf(stderr, "Initialization failed\n");
		goto end;
	}

	ret = asprintf(&znr.dev_path, "%s", mnt->mnt_fsname);
	if (ret < 0) {
		fprintf(stderr, "Initialization failed\n");
		free(znr.mnt_dir.path);
		znr.mnt_dir.path = NULL;
		goto end;
	}

	/* Open the directory to get the file system type. */
	ret = znr_fs_open_file(&znr.mnt_dir, false);
	if (ret)
		goto cleanup;

	ret = znr_fs_get_file_fs(&znr.mnt_dir);
	if (ret)
		goto cleanup;

	ret = znr_fs_init_fs(&znr.mnt_dir);
	if (ret)
		goto cleanup;

	goto end;

cleanup:
	free(znr.mnt_dir.path);
	znr.mnt_dir.path = NULL;
	free(znr.dev_path);
	znr.dev_path = NULL;

end:
	endmntent(mtab);

	return ret;
}

void znr_fs_close(void)
{
	znr_fs_clear_file(&znr.mnt_dir);
}
