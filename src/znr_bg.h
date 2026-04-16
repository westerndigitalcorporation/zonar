/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_BG_H
#define ZNR_BG_H

/*
 * Block group information.
 */
struct znr_blockgroup {
        /* Starting sector */
	unsigned long sector;

	/* Number of sectors */
	unsigned long nr_sectors;

	/* Write pointer sector offset within this blockgroup */
	unsigned long wp_sector;

	unsigned int flags;

	/* Zones in this block group */
	struct blk_zone **zones;
	unsigned long nr_zones;
};

int znr_bg_get_blockgroups(struct znr_blockgroup **bgs,
			   unsigned int *nr_bgs);

int znr_bg_refresh(struct znr_device *dev, struct blk_zone *zones,
		   unsigned int max_zones, struct znr_blockgroup *bgs,
		   unsigned int bg_num, unsigned int nr_bgs);

void znr_bg_destroy_blockgroups(struct znr_blockgroup *bgs,
				unsigned int nr_bgs);

#endif /* ZNR_BG_H */
