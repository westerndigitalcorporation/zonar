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

int znr_bg_get_blockgroups(struct znr_blockgroup **bgs,
			   unsigned int *nr_bgs)
{
	return znr_fs_get_blockgroups(bgs, nr_bgs);
}

void znr_bg_destroy_blockgroups(struct znr_blockgroup *bgs,
				unsigned int nr_bgs)
{
	unsigned int i;

	for (i = 0; i < nr_bgs; i++) {
		free(bgs[i].zones);
		bgs[i].zones = NULL;
	}
}

static inline void znr_bg_set_dev_zone_wp(struct znr_blockgroup *bg)
{
	struct blk_zone *zone;

	if (bg->nr_zones == 0 || !bg->zones)
		return;

	/* Set the device write pointer. */
	zone = bg->zones[0];
	if (zone->type == BLK_ZONE_TYPE_SEQWRITE_REQ) {
		bg->flags |= ZNR_BG_HAS_DEV_ZONE_WP;
		bg->dev_zone_wp = zone->wp - zone->start;
	}
}

static int znr_bg_get_zone_mapping(struct znr_blockgroup *bg,
				   struct blk_zone *zones,
				   unsigned int nr_zones,
				   unsigned int zone_sectors)
{
	unsigned long bg_end;
	unsigned int j, max_zones_per_bg, bg_zone_idx = 0;
	unsigned int start_zno, end_zno;

	if (!bg || !zones || !nr_zones)
		return -EINVAL;

	bg_end = bg->sector + bg->nr_sectors;
	start_zno = bg->sector / zone_sectors;
	end_zno = bg_end / zone_sectors;

	if (start_zno > end_zno)
		return -EINVAL;

	/* This is inclusive of the start and the end zone, hence + 1. */
	max_zones_per_bg = (end_zno - start_zno) + 1;
	bg->zones =
		calloc(max_zones_per_bg, sizeof(struct blk_zone *));
	if (!bg->zones) {
		fprintf(stderr, "No memory for blockgroup zone array\n");
		return -ENOMEM;
	}

	bg_zone_idx = 0;
	for (j = 0; j < max_zones_per_bg; j++) {
		if (start_zno + j > nr_zones) {
			fprintf(stderr, "Invalid zone index in blockgroup [%u/%u]\n",
				start_zno + j, nr_zones);
			free(bg->zones);
			bg->zones = NULL;
			return -EINVAL;
		}
		bg->nr_zones = bg_zone_idx;
		bg->zones[bg_zone_idx++] = &zones[start_zno + j];
	}

	if (!bg_zone_idx) {
		fprintf(stderr, "No zones in blockgroup\n");
		free(bg->zones);
		bg->zones = NULL;
		return -EINVAL;
	}

	znr_bg_set_dev_zone_wp(bg);
	bg->flags |= ZNR_BG_MAPPING_INITIALIZED;
	return 0;
}

static int znr_bg_get_zone_info(struct znr_blockgroup *bgs,
				unsigned int nr_bgs,
				struct blk_zone *zones,
				unsigned int nr_zones,
				unsigned int zone_sectors)
{
	unsigned int i;
	int ret = 0;

	if (!bgs || !zones)
		return -EINVAL;

	if (nr_zones < nr_bgs)
		return -EINVAL;

	if (nr_bgs > znr.nr_bgs)
		return -EINVAL;

	if (nr_zones > znr.nr_zones)
		return -EINVAL;

	znr_verbose("Getting zone info for %u blockgroup starting at sector: 0x%lx\n",
		    nr_bgs, bgs[0].sector);

	for (i = 0; i < nr_bgs; i++) {
		if (bgs[i].flags & ZNR_BG_MAPPING_INITIALIZED) {
			/*
			 * Zone backing information cannot change, if zone
			 * backing information already exists, only update the
			 * writepointer.
			 */
			znr_bg_set_dev_zone_wp(&bgs[i]);
		} else {
			ret = znr_bg_get_zone_mapping(&bgs[i], zones,
						      nr_zones, zone_sectors);
			if (ret)
				goto out_free;
		}
	}

	return 0;
out_free:
	znr_bg_destroy_blockgroups(bgs, i);
	return ret;
}

static int znr_bg_to_zno(struct znr_device *dev,
			 struct znr_blockgroup *bg_start,
			 struct znr_blockgroup *bg_end,
			 unsigned int *zno_start, unsigned int *zno_end)
{
	unsigned int start_zone_no, end_zone_no;

	if (!bg_start || !bg_end || !zno_start || !zno_end)
		return -EINVAL;

	if (bg_start->sector > bg_end->sector)
		return -EINVAL;
	/*
	 * A blockgroup could use multiple zones on the device, in which case,
	 * we need to get the actual zone numbers on the device to do a zone
	 * report
	 */
	start_zone_no = bg_start->sector / dev->zone_sectors;
	end_zone_no = (bg_end->sector + bg_end->nr_sectors) /
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
			 unsigned int max_zones, struct znr_blockgroup *bgs,
			 unsigned int bg_no,
			 unsigned int nr_bgs)
{
	unsigned int last_zone_no, start_zone_no, nr_zones;
	unsigned long max_sector;
	int ret;

	if (!bgs || !nr_bgs || bg_no + nr_bgs > znr.nr_bgs)
		return -EINVAL;

	znr_verbose("Do blockgroup reports from group %u, %u groups\n",
		    bg_no, nr_bgs);

	/* Do the zone report first for zoned devices. */
	if (dev->is_zoned) {
		if (!dev || !zones || !max_zones || max_zones > dev->nr_zones)
			return -EINVAL;

		/* The last sector in this set of blockgroups. */
		max_sector = bgs[nr_bgs - 1].sector +
			bgs[nr_bgs - 1].nr_sectors;
		if (max_sector > dev->nr_sectors) {
			fprintf(stderr, "Sector out of bounds: sector: %ld | max: %lld\n",
				max_sector, dev->nr_sectors);
			return -EINVAL;
		}

		ret = znr_bg_to_zno(dev, bgs, &bgs[bg_no + (nr_bgs - 1)],
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
	}

	/*
	 * This needs to happen after a zone report to ensure we get the most
	 * upto date FS writepointer.
	 */
	ret = znr_fs_report_blockgroups(&bgs[bg_no],
					bg_no,
					nr_bgs);
	if (ret < 0 || (unsigned int)ret != nr_bgs)
		return -EINVAL;

	if (!dev->is_zoned)
		return nr_bgs;

	ret = znr_bg_get_zone_info(&bgs[bg_no],
				   nr_bgs,
				   &zones[start_zone_no], nr_zones,
				   dev->zone_sectors);
	if (ret)
		return ret;

	return nr_bgs;
}

int znr_bg_refresh(struct znr_device *dev, struct blk_zone *zones,
		   unsigned int max_zones, struct znr_blockgroup *bgs,
		   unsigned int bg_no, unsigned int nr_bgs)
{
	znr_verbose("Refreshing %u blockgroups, starting at blockgroup %u\n",
		    nr_bgs, bg_no);

	return znr_bg_report(dev, zones, max_zones, bgs,
			     bg_no, nr_bgs);
}

