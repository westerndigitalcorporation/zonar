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

	/*
	 * The device indicated write pointer sector offset within this
	 * blockgroup. Valid if ZNR_BG_HAS_DEV_ZONE_WP is set in flags.
	 */
	unsigned long dev_zone_wp;

	/*
	 * The current write pointer offset within this blockgroup used by the
	 * FS to issue writes. Valif if ZNR_BG_HAS_FS_WP is set in flags.
	 */
	unsigned long fs_wp;

	/* Is true if the backing zones has been mapped to this blockgroup */
	bool has_zone_mapping;

	/*
	 * Flags about this blockgroup. The upper 8bits are reserved for
	 * runtime flags for the local context (in the server/client model).
	 * Thus, these bits should be masked out when sending the flags field
	 * over a network. The ZNR_BG_SHARED_FLAGS mask can be used for this.
	 */
	unsigned int flags;

	/* Zones in this block group */
	struct blk_zone **zones;
	unsigned long nr_zones;
};

#define ZNR_BG_SHARED_FLAGS ((1U << 24) - 1)

enum znr_bg_flags {
	/*
	 * Set for a blockgroup backed by a sequential write required zone of
	 * a zoned device.
	 */
	ZNR_BG_HAS_DEV_ZONE_WP	= (1U << 0),
	/* Set if the filesystem provides a write pointer. */
	ZNR_BG_HAS_FS_WP	= (1U << 1),
	/*
	 * Set if the backing zone mapping has been initialized for this
	 * blockgroup. This flag is valid only for the local context, as such
	 * shall not be sent over the network.
	 */
	ZNR_BG_MAPPING_INITIALIZED = (1U << 24),
};

/*
 * Helper function for zone backed blockgroups to check if the zone is
 * fully written. Must only be called for zone backed blockgroups.
 */
static inline bool znr_bg_dev_zone_full(struct znr_blockgroup *bg)
{
	if (!bg->nr_zones || !bg->zones)
		return false;

	return bg->zones[0]->cond == BLK_ZONE_COND_FULL;
}

/* Helper function that checks if the blockgroup is fully written. */
static inline bool znr_bg_full(struct znr_blockgroup *bg)
{
	/* If this is a zoned device check the zone condition */
	if (bg->nr_zones && bg->zones)
		return znr_bg_dev_zone_full(bg);

	return bg->fs_wp >= bg->nr_sectors;
}

static inline bool znr_bg_has_wp(struct znr_blockgroup *bg)
{
	return bg->flags & (ZNR_BG_HAS_DEV_ZONE_WP | ZNR_BG_HAS_FS_WP);
}

int znr_bg_get_blockgroups(struct znr_blockgroup **bgs,
			   unsigned int *nr_bgs);

int znr_bg_refresh(struct znr_device *dev, struct blk_zone *zones,
		   unsigned int max_zones, struct znr_blockgroup *bgs,
		   unsigned int bg_num, unsigned int nr_bgs);

void znr_bg_destroy_blockgroups(struct znr_blockgroup *bgs,
				unsigned int nr_bgs);

#endif /* ZNR_BG_H */
