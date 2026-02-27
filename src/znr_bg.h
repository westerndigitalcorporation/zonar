/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_BG_H
#define ZNR_BG_H

#define ZNR_BG_MAX_ZONES	512

/*
 * Block group information.
 */
struct znr_bg {
        /* Starting sector */
	unsigned long sector;

	/* Number of sectors */
	unsigned long nr_sectors;

	/* Write pointer sector offset within this blockgroup */
	unsigned long wp_sector;

	unsigned int flags;

	/* Zones in this block group */
	struct blk_zone *zones[ZNR_BG_MAX_ZONES];
	unsigned long nr_zones;
};

int znr_bg_get_blockgroups(struct znr_bg **blockgroups,
			   unsigned int *nr_blockgroups);

int znr_bg_refresh(struct znr_device *dev, struct blk_zone *zones,
		   unsigned int max_zones, struct znr_bg *blockgroups,
		   unsigned int blockgroup_num, unsigned int nr_blockgroups);

#endif /* ZNR_BG_H */
