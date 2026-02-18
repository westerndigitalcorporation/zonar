// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/blkzoned.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#include "znr.h"

/*
 * To handle string conversions.
 */
struct znr_dev_str {
	unsigned int	val;
	const char	*str;
	const char	*str_short;
};

static const char *znr_dev_get_str(struct znr_dev_str *str,
				   unsigned int val, bool s)
{
	unsigned int i = 0;

	while (str[i].val != UINT_MAX) {
		if (str[i].val == val)
			break;
		i++;
	}

	if (s)
		return str[i].str_short;

	return str[i].str;
}

/*
 * Strip a string of trailing spaces and carriage return.
 */
static int znr_dev_str_strip(char *str)
{
	int i = strlen(str) - 1;

	while (i >= 0) {
		if (str[i] != ' ' &&
		    str[i] != '\t' &&
		    str[i] != '\r' &&
		    str[i] != '\n')
			break;

		str[i] = '\0';
		i--;
	}

	return i + 1;
}

static int znr_dev_get_sysfs_attr(char *devname, const char *attr,
				  char *str, int str_len)
{
	char attr_path[128];
	FILE *file;
	int ret = 0;

	snprintf(attr_path, sizeof(attr_path), "/sys/block/%s/%s",
		 devname, attr);

	file = fopen(attr_path, "r");
	if (!file)
		return -ENOENT;

	if (!fgets(str, str_len, file)) {
		ret = -EINVAL;
		goto close;
	}

	if (!znr_dev_str_strip(str))
		ret = -EINVAL;

close:
	fclose(file);

	return ret;
}

static int znr_dev_get_sysfs_attr_int64(char *devname, const char *attr,
					long long *val)
{
	char str[128];
	int ret;

	ret = znr_dev_get_sysfs_attr(devname, attr, str, sizeof(str));
	if (ret)
		return ret;

	*val = atoll(str);

	return 0;
}

static int znr_dev_get_sysfs_attr_str(char *devname, const char *attr,
				      char *val, int val_len)
{
	return znr_dev_get_sysfs_attr(devname, attr, val, val_len);
}

static struct znr_dev_str znr_dev_ztype_str[] = {
	{ BLK_ZONE_TYPE_CONVENTIONAL,	"conventional",		"cnv"	},
	{ BLK_ZONE_TYPE_SEQWRITE_REQ,	"seq-write-required",	"swr"	},
	{ BLK_ZONE_TYPE_SEQWRITE_PREF,	"seq-write-preferred",	"swp"	},
	{ UINT_MAX,			"unknown",		"???"	}
};

/**
 * znr_dev_zone_type_str - returns a string describing a zone type
 */
const char *znr_dev_zone_type_str(struct blk_zone *z, bool s)
{
	return znr_dev_get_str(znr_dev_ztype_str, z->type, s);
}

static struct znr_dev_str znr_dev_zcond_str[] = {
	{ BLK_ZONE_COND_NOT_WP,		"not-write-pointer",	"nw"	},
	{ BLK_ZONE_COND_EMPTY,		"empty",		"em"	},
	{ BLK_ZONE_COND_FULL,		"full",			"fu"	},
	{ BLK_ZONE_COND_IMP_OPEN,	"open-implicit",	"oi"	},
	{ BLK_ZONE_COND_EXP_OPEN,	"open-explicit",	"oe"	},
	{ BLK_ZONE_COND_CLOSED,		"closed",		"cl"	},
	{ BLK_ZONE_COND_READONLY,	"read-only",		"ro"	},
	{ BLK_ZONE_COND_OFFLINE,	"offline",		"ol"	},
	{ BLK_ZONE_COND_ACTIVE,		"active",		"ac"	},
	{ UINT_MAX,			"unknown",		"??"	}
};

/**
 * znr_dev_zone_cond_str - Returns a string describing a zone condition
 */
const char *znr_dev_zone_cond_str(struct blk_zone *z, bool s)
{
	return znr_dev_get_str(znr_dev_zcond_str, z->cond, s);
}

static int znr_dev_get_zoned(struct znr_device *dev)
{
	char str[128];
	int ret;

	ret = znr_dev_get_sysfs_attr_str(dev->devname, "queue/zoned",
					 str, sizeof(str));
	if (ret)
		return ret;

	dev->is_zoned = strcmp(str, "none") != 0;

	return 0;
}

static int znr_dev_get_nr_zones(struct znr_device *dev)
{
	long long nrz;
	int ret;

	ret = znr_dev_get_sysfs_attr_int64(dev->devname,
					   "queue/nr_zones", &nrz);
	if (ret < 0)
		return ret;

	dev->nr_zones = nrz;

	return 0;
}

static int znr_dev_get_zone_sectors(struct znr_device *dev)
{
	long long zs;
	int ret;

	ret = znr_dev_get_sysfs_attr_int64(dev->devname,
					   "queue/chunk_sectors", &zs);
	if (ret < 0)
		return ret;

	dev->zone_sectors = zs;
	dev->zone_size = zs << SECTOR_SHIFT;

	return 0;
}

static void znr_dev_get_max_resources(struct znr_device *dev)
{
	long long val;
	int ret;

	ret = znr_dev_get_sysfs_attr_int64(dev->devname,
					   "queue/max_open_zones", &val);
	if (ret)
		val = 0;
	dev->max_nr_open_zones = val;

	ret = znr_dev_get_sysfs_attr_int64(dev->devname,
					   "queue/max_active_zones", &val);
	if (ret)
		val = 0;
	dev->max_nr_active_zones = val;
}

/*
 * Get vendor ID.
 */
static int znr_dev_get_vendor_id(struct znr_device *dev)
{
	char str[128];
	int ret, n = 0;

	ret = znr_dev_get_sysfs_attr_str(dev->devname, "device/vendor",
					 str, sizeof(str));
	if (!ret)
		n = snprintf(dev->vendor_id, ZNR_DEV_VENDOR_ID_LEN,
			     "%s ", str);

	ret = znr_dev_get_sysfs_attr_str(dev->devname, "device/model",
					 str, sizeof(str));
	if (!ret)
		n += snprintf(&dev->vendor_id[n], ZNR_DEV_VENDOR_ID_LEN - n,
			      "%s ", str);

	ret = znr_dev_get_sysfs_attr_str(dev->devname, "device/rev",
					 str, sizeof(str));
	if (!ret)
		n += snprintf(&dev->vendor_id[n], ZNR_DEV_VENDOR_ID_LEN - n,
			      "%s", str);

	return n > 0;
}

static int znr_dev_get_info(int fd, char *devname)
{
	unsigned long long size64;
	struct znr_device *dev = &znr.dev;
	int ret, size32;

	dev->fd = fd;
	dev->devname = strdup(devname);
	if (!dev->devname)
		goto err;

	/* Get zone model */
	ret = znr_dev_get_zoned(dev);
	if (ret) {
		znr_err("Failed to determine device type\n");
		goto err;
	}

	/* Get logical block size */
	ret = ioctl(fd, BLKSSZGET, &size32);
	if (ret != 0) {
		znr_err("ioctl BLKSSZGET failed %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	dev->lblock_size = size32;
	if (dev->lblock_size <= 0) {
		znr_err("invalid logical sector size %d\n",
			size32);
		goto err;
	}

	/* Get physical block size */
	ret = ioctl(fd, BLKPBSZGET, &size32);
	if (ret != 0) {
		znr_err("ioctl BLKPBSZGET failed %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	dev->pblock_size = size32;
	if (dev->pblock_size <= 0) {
		znr_err("Invalid physical sector size %d\n",
			size32);
		goto err;
	}

	/* Get capacity (Bytes) */
	ret = ioctl(fd, BLKGETSIZE64, &size64);
	if (ret != 0) {
		znr_err("ioctl BLKGETSIZE64 failed %d (%s)\n",
			errno, strerror(errno));
		goto err;
	}
	dev->nr_sectors = size64 >> SECTOR_SHIFT;

	dev->nr_lblocks = size64 / dev->lblock_size;
	if (!dev->nr_lblocks) {
		znr_err("Invalid capacity (logical blocks)\n");
		goto err;
	}

	dev->nr_pblocks = size64 / dev->pblock_size;
	if (!dev->nr_pblocks) {
		znr_err("Invalid capacity (physical blocks)\n");
		goto err;
	}

	/* Get zone size */
	ret = znr_dev_get_zone_sectors(dev);
	if (ret)
		goto err;

	/* Get number of zones */
	ret = znr_dev_get_nr_zones(dev);
	if (ret)
		goto err;

	/* Get max number of open/active zones */
	znr_dev_get_max_resources(dev);

	/* Finish setting */
	if (!znr_dev_get_vendor_id(dev))
		strcpy(dev->vendor_id, "Unknown");

	return 0;

err:
	if (dev->devname) {
		free(dev->devname);
		dev->devname = NULL;
	}
	return -1;
}

void znr_dev_close(void)
{
	struct znr_device *dev = &znr.dev;

	free(dev->devname);
	dev->devname = NULL;
	if (dev->fd > 0) {
		close(dev->fd);
		dev->fd = -1;
	}
}

int znr_dev_open(void)
{
	char *path, *devname;
	int ret, fd;
	char *p;

	if (znr.is_net_client)
		return znr_net_get_dev_info(&znr.ncli);

	/* Follow symlinks (required for device mapped devices) */
	p = realpath(znr.dev_path, NULL);
	if (!p) {
		znr_err("%s: Failed to get real path %d (%s)\n",
			znr.dev_path, errno, strerror(errno));
		return -1;
	}

	path = p;
	devname = basename(p);

	/* Open block device */
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		znr_err("open %s failed %d (%s)\n",
			znr.dev_path, errno, strerror(errno));
		goto err;
	}

	/* Get device information */
	ret = znr_dev_get_info(fd, devname);
	if (ret)
		goto err;

	free(path);

	return 0;

err:
	if (fd >= 0)
		close(fd);
	free(path);

	return -1;
}

/*
 * Maximum number of zones to report in one call to znr_dev_report_zones.
 */
#define ZNR_DEV_REPORT_MAX_NR_ZONES	8192

/*
 * znr_dev_report_zones - Get zone information
 */
int znr_dev_report_zones(struct znr_device *dev, unsigned int start_zone_no,
			 struct blk_zone *zones, unsigned int nr_zones)
{
	__u64 zone_mask = dev->zone_sectors - 1;
	__u64 sector, end_sector;
	struct blk_zone_report *rep;
	size_t rep_size;
	unsigned int rep_nr_zones;
	unsigned int n = 0, i;
	struct blk_zone *blkz;
	int ret = 0;

	if (!dev || !zones || !nr_zones || start_zone_no >= dev->nr_zones)
		return -EINVAL;

	znr_verbose("Do report zones from zone %u, %u zones\n",
		    start_zone_no, nr_zones);

	if (znr.is_net_client)
		return znr_net_get_dev_rep_zones(&znr.ncli, start_zone_no,
						 zones, nr_zones);

	sector = (__u64)dev->zone_sectors * start_zone_no;
	end_sector = (sector + dev->nr_sectors + zone_mask) & (~zone_mask);
	if (end_sector > dev->nr_sectors)
		end_sector = dev->nr_sectors;

	/* Get all zones information */
	rep_nr_zones = ZNR_DEV_REPORT_MAX_NR_ZONES;
	if (nr_zones < rep_nr_zones)
		rep_nr_zones = nr_zones;
	rep_size = sizeof(struct blk_zone_report) +
		sizeof(struct blk_zone) * rep_nr_zones;
	rep = (struct blk_zone_report *)malloc(rep_size);
	if (!rep) {
		znr_err("%s: No memory for array of zones\n",
			dev->devname);
		return -ENOMEM;
	}

	while (n < nr_zones && sector < end_sector) {
		memset(rep, 0, rep_size);
		rep->sector = sector;
		rep->nr_zones = rep_nr_zones;
		rep->flags = BLK_ZONE_REP_CACHED;

		ret = ioctl(dev->fd, BLKREPORTZONEV2, rep);
		if (ret && errno == ENOTTY) {
			rep->sector = sector;
			rep->nr_zones = rep_nr_zones;
			rep->flags = 0;
			ret = ioctl(dev->fd, BLKREPORTZONE, rep);
		}
		if (ret) {
			ret = -errno;
			znr_err("%s: ioctl BLKREPORTZONE at zone %u failed %d (%s)\n",
				dev->devname, start_zone_no,
				errno, strerror(errno));
			goto out;
		}

		if (!rep->nr_zones)
			break;

		blkz = (struct blk_zone *)(rep + 1);
		for (i = 0; i < rep->nr_zones; i++) {
			if (n >= nr_zones || sector >= end_sector)
				break;

			/* Copy zone directly from kernel structure */
			memcpy(&zones[n], blkz, sizeof(*blkz));
			n++;

			sector = blkz->start + blkz->len;
			blkz++;
		}
	}

	return n;

out:
	free(rep);

	return ret;
}

char *znr_dev_get_zone_info(struct blk_zone *blkz,
			    char *buffer, size_t buffer_size)
{
	if (znr_dev_zone_cnv(blkz))
		snprintf(buffer, buffer_size,
			 "<tt>"
			 "<b>Zone No</b>:       %llu\n"
			 "<b>Type</b>:          %s\n"
			 "<b>Start</b>:         %llu\n"
			 "<b>Length</b>:        %llu\n"
			 "<b>Capacity</b>:      %llu\n"
			 "<b>Condition</b>:     %s\n"
			 "</tt>",
			 blkz->start / blkz->len,
			 znr_dev_zone_type_str(blkz, false),
			 blkz->start,
			 blkz->len,
			 blkz->capacity,
			 znr_dev_zone_cond_str(blkz, false));
	else
		snprintf(buffer, buffer_size,
			 "<tt>"
			 "<b>Zone No</b>:       %llu\n"
			 "<b>Type</b>:          %s\n"
			 "<b>Start</b>:         %llu\n"
			 "<b>Length</b>:        %llu\n"
			 "<b>Capacity</b>:      %llu\n"
			 "<b>WP Offset</b>:     +%llu\n"
			 "<b>Condition</b>:     %s\n"
			 "</tt>",
			 blkz->start / blkz->len,
			 znr_dev_zone_type_str(blkz, false),
			 blkz->start,
			 blkz->len,
			 blkz->capacity,
			 (blkz->wp - blkz->start),
			 znr_dev_zone_cond_str(blkz, false));

	return buffer;
}
