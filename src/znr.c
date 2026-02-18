// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 *
 * Authors: Wilfred Mallawa (wilfred.mallawa@wdc.com)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "znr.h"

/*
 * Main structure shared by all files.
 */
struct znr znr;

void znr_init(void)
{
	memset(&znr, 0, sizeof(znr));
}

void znr_close(void)
{
	znr_fs_close();
	znr_dev_close();
	free(znr.blk_zones);
	free(znr.blockgroups);
}

int znr_open(const char *mntdir)
{
	unsigned int i;
	int ret;

	/* Open the mount directory to get the device name. */
	ret = znr_fs_open(mntdir);
	if (ret)
		return ret;

	ret = znr_dev_open();
	if (ret)
		return ret;

	znr.nr_zones = znr.dev.nr_zones;
	if (znr.dev.is_zoned && !znr.nr_zones) {
		fprintf(stderr, "%s: No zones reported\n",
			znr.dev.devname);
		ret = -EINVAL;
		goto err;
	}

	/* Allocate zone array and get zone information. */
	znr.blk_zones = calloc(znr.nr_zones, sizeof(struct blk_zone));
	if (!znr.blk_zones)
		return -ENOMEM;

	ret = znr_bg_get_blockgroups(&znr.blockgroups, &znr.nr_blockgroups);
	if (ret < 0) {
		fprintf(stderr, "Failed to get blockgroups\n");
		goto err;
	}

	if (znr.dev.is_zoned) {
		ret = znr_dev_report_zones(&znr.dev, 0,
					   znr.blk_zones, znr.nr_zones);
		if (ret < 0) {
			fprintf(stderr, "%s: znr_dev_report_zones failed %d\n",
				znr.dev.devname, ret);
			goto err;
		}

		if ((unsigned int)ret != znr.nr_zones) {
			fprintf(stderr,
				"%s: Got %d zones, expected %u reported\n",
				znr.dev.devname, ret, znr.nr_zones);
			ret = -EINVAL;
			goto err;
		}

		for (i = 0; i < znr.nr_zones; i++) {
			if (znr_dev_zone_cnv(&znr.blk_zones[i]))
				znr.nr_conv_zones++;
			else
				break;
		}
	}

	ret = znr_bg_refresh(&znr.dev, znr.blk_zones, znr.nr_zones,
			     znr.blockgroups, 0, znr.nr_blockgroups);
	if (ret < 0) {
		fprintf(stderr, "Failed to report block groups: %d\n", ret);
		goto err;
	}

	if ((unsigned int)ret != znr.nr_blockgroups) {
		fprintf(stderr,
			"%s: Got %d blockgroups, expected %u reported\n",
			znr.dev.devname, ret, znr.nr_blockgroups);
		ret = -EINVAL;
		goto err;
	}

	return 0;

err:
	znr_close();
	return ret;
}

void znr_print_info(void)
{
	printf("Mount directory %s: %s on device %s\n",
	       znr.mnt_dir.path, znr.mnt_dir.fs->name,
	       znr.dev_path);
	printf("  Vendor ID: %s\n", znr.dev.vendor_id);
	printf("  Capacity: %llu GB (%llu 512-bytes sectors)\n",
	       znr.dev.nr_sectors * 512 / 1000000000,
	       znr.dev.nr_sectors);
	printf("  Logical block size: %u B\n", znr.dev.lblock_size);
	printf("  Physical block size: %u B\n", znr.dev.pblock_size);
	printf("  %u zones of %llu MiB (%u 512-bytes sectors)\n",
	       znr.dev.nr_zones,
	       znr.dev.zone_size / 1048576,
	       znr.dev.zone_sectors);
	printf("  Max open zones: %u\n", znr.dev.max_nr_open_zones);
	printf("  Max active zones: %u\n", znr.dev.max_nr_active_zones);
}
