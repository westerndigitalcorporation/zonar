// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 *
 * Authors: Wilfred Mallawa (wilfred.mallawa@wdc.com)
 */
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "znr.h"
#include "znr_bg.h"
#include "znr_fs.h"

int znr_bg_get_blockgroups(struct znr_bg **blockgroups,
			   unsigned int *nr_blockgroups)
{
	return znr_fs_get_blockgroups(blockgroups, nr_blockgroups);
}

static int znr_bg_map_zones_to_blockgroups(struct znr_bg *blockgroups,
					   unsigned int nr_blockgroups,
					   struct blk_zone *zones,
					   unsigned int nr_zones)
{
	unsigned long bg_sector_end, zone_sector_end;
	unsigned int bg_zone_idx, j, i, zone_start_idx = 0;

	znr_verbose("Mapping %u zones to %u blockgroups\n", nr_zones,
		    nr_blockgroups);

	if (!blockgroups || !zones)
		return -EINVAL;

	if (nr_zones < nr_blockgroups)
		return -EINVAL;

	if (nr_blockgroups > znr.nr_blockgroups)
		return -EINVAL;

	if (nr_zones > znr.nr_zones)
		return -EINVAL;

	for (i = 0; i < nr_blockgroups; ++i) {
		memset(blockgroups[i].zones, 0, sizeof(blockgroups[i].zones));
		blockgroups[i].nr_zones = 0;
		bg_sector_end = blockgroups[i].sector +
				blockgroups[i].nr_sectors;
		bg_zone_idx = 0;

		/*
		 * Start from where we left off, but check the last zone back.
		 * For conventional zones, blockgroups may overlap zones.
		 */
		j = (zone_start_idx > 1) ? zone_start_idx - 1 : 0;
		for (; j < nr_zones; ++j) {
			zone_sector_end = zones[j].start + zones[j].len;

			/* Skip zones that end before this blockgroup starts */
			if (zone_sector_end <= blockgroups[i].sector) {
				zone_start_idx = j + 1;
				continue;
			}

			/*
			 * Stop checking zones that start at or after this
			 * blockgroup ends
			 */
			if (zones[j].start >= bg_sector_end)
				break;

			/* This zone overlaps with the i'th blockgroup */
			blockgroups[i].zones[bg_zone_idx] = &zones[j];
			bg_zone_idx++;
			if (bg_zone_idx >= ZNR_BG_MAX_ZONES) {
				fprintf(stderr,
					"Too many zones in blockgroup\n");
				return -EINVAL;
			}
			blockgroups[i].nr_zones = bg_zone_idx;
		}

		if (!blockgroups[i].nr_zones) {
			fprintf(stderr,
				"No zones mapped to blockgroup %u\n", i);
			return -EINVAL;
		}

		blockgroups[i].flags = blockgroups[i].zones[0]->type;
		if (blockgroups[i].flags == BLK_ZONE_TYPE_SEQWRITE_REQ)
			blockgroups[i].wp_sector =
				blockgroups[i].zones[0]->wp -
				blockgroups[i].sector;
		else
			blockgroups[i].wp_sector = 0;
	}

	return 0;
}

static int znr_bg_to_zno(struct znr_device *dev,
			 struct znr_bg *blockgroup_start,
			 struct znr_bg *blockgroup_end,
			 unsigned int *zno_start, unsigned int *zno_end)
{
	unsigned int start_zone_no, end_zone_no;

	if (!blockgroup_start || !blockgroup_end || !zno_start || !zno_end)
		return -EINVAL;

	if (blockgroup_start->sector > blockgroup_end->sector)
		return -EINVAL;
	/*
	 * A blockgroup could use multiple zones on the device, in which case,
	 * we need to get the actual zone numbers on the device to do a zone
	 * report
	 */
	start_zone_no = blockgroup_start->sector / dev->zone_sectors;
	end_zone_no = (blockgroup_end->sector + blockgroup_end->nr_sectors) /
		dev->zone_sectors;

	if (end_zone_no > dev->nr_zones) {
		fprintf(stderr, "Invalid zone in blockgroup\n");
		return -EINVAL;
	}

	*zno_start = start_zone_no;
	*zno_end = end_zone_no;

	return 0;
}

static int znr_bg_report(struct znr_device *dev, struct blk_zone *zones,
			 unsigned int max_zones, struct znr_bg *blockgroups,
			 unsigned int blockgroup_no,
			 unsigned int nr_blockgroups)
{
	unsigned int last_zone_no, start_zone_no, nr_zones, i;
	unsigned long max_sector;
	int ret;

	if (!blockgroups || !nr_blockgroups ||
	    blockgroup_no + nr_blockgroups > znr.nr_blockgroups)
		return -EINVAL;

	if (!dev->is_zoned) {
		/*
		 * If the device is not zoned, treat all zones as
		 * conventional. When filesystems support it we can add a
		 * fetch the allocation pointer directly from the FS.
		 */
		for (i = 0; i < nr_blockgroups; i++)
			blockgroups[i].flags = BLK_ZONE_TYPE_CONVENTIONAL;
		return nr_blockgroups;
	}

	if (!dev || !zones || !max_zones || max_zones > dev->nr_zones)
		return -EINVAL;

	znr_verbose("Do blockgroup reports from group %u, %u groups\n",
		    blockgroup_no, nr_blockgroups);

	/* The last sector in this set of blockgroups */
	max_sector = blockgroups[nr_blockgroups - 1].sector +
		     blockgroups[nr_blockgroups - 1].nr_sectors;
	if (max_sector > dev->nr_sectors) {
		fprintf(stderr, "Sector out of bounds: sector: %ld | max: %lld\n",
			max_sector, dev->nr_sectors);
		return -EINVAL;
	}

	ret = znr_bg_to_zno(dev, blockgroups, &blockgroups[nr_blockgroups - 1],
			    &start_zone_no, &last_zone_no);
	if (ret)
		return ret;

	nr_zones = last_zone_no - start_zone_no;
	if (!nr_zones || nr_zones > max_zones)
		return -EINVAL;

	/* Do zone report */
	ret = znr_dev_report_zones(dev, start_zone_no,
				   &zones[start_zone_no], nr_zones);
	if ((unsigned int)ret != nr_zones)
		return -EINVAL;

	ret = znr_bg_map_zones_to_blockgroups(blockgroups, nr_blockgroups,
					      &zones[start_zone_no],
					      nr_zones);
	if (ret)
		return ret;

	return nr_blockgroups;
}

int znr_bg_refresh(struct znr_device *dev, struct blk_zone *zones,
		   unsigned int max_zones, struct znr_bg *blockgroups,
		   unsigned int blockgroup_no, unsigned int nr_blockgroups)
{
	znr_verbose("Refreshing %u blockgroups, starting at blockgroup %u\n",
		    nr_blockgroups, blockgroup_no);

	return znr_bg_report(dev, zones, max_zones, blockgroups,
			     blockgroup_no, nr_blockgroups);
}

