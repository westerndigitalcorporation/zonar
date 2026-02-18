// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_DEVICE_H
#define ZNR_DEVICE_H

#include "config.h"

#include <stdint.h>
#include <stdbool.h>
#include <linux/blkzoned.h>

/*
 * Device information vendor id string maximum length.
 */
#define ZNR_DEV_VENDOR_ID_LEN	32

/*
 * Zoned device information data structure
 */
struct znr_device {
	/* Device name. */
	char			*devname;

	/* File descriptor. */
	int			fd;

	/* Device vendor, model and firmware revision string. */
	char			vendor_id[ZNR_DEV_VENDOR_ID_LEN + 1];

	/* Total number of 512B sectors of the device. */
	unsigned long long	nr_sectors;

	/* Total number of logical blocks of the device. */
	unsigned long long	nr_lblocks;

	/* Total number of physical blocks of the device. */
	unsigned long long	nr_pblocks;

	/* Size in bytes of a zone. */
	unsigned long long	zone_size;

	/* Size in 512B sectors of a zone. */
	unsigned int		zone_sectors;

	/* Size in bytes of the device logical blocks. */
	unsigned int		lblock_size;

	/* Size in bytes of the device physical blocks. */
	unsigned int		pblock_size;

	/* Number of zones. */
	unsigned int		nr_zones;

	/*
	 * Maximum number of explicitly open zones. A value of 0 means that
	 * the device has no limit. A value of -1 means that the value is
	 * unknown.
	 */
	unsigned int		max_nr_open_zones;

	/*
	 * Maximum number of active zones. A value of 0 means that the device
	 * has no limit. A value of -1 means that the value is unknown.
	 */
	unsigned int		max_nr_active_zones;

	/*
	 * Device zone model.
	 */
	bool			is_zoned;
};

/*
 * 512B sector size shift.
 */
#define SECTOR_SHIFT	9

#define znr_dev_zone_type(z)		((z)->type)
#define znr_dev_zone_cnv(z)		\
	((z)->type == BLK_ZONE_TYPE_CONVENTIONAL)

#define znr_dev_zone_not_wp(z)		((z)->cond == BLK_ZONE_COND_NOT_WP)
#define znr_dev_zone_empty(z)		((z)->cond == BLK_ZONE_COND_EMPTY)
#define znr_dev_zone_imp_open(z)	((z)->cond == BLK_ZONE_COND_IMP_OPEN)
#define znr_dev_zone_exp_open(z)	((z)->cond == BLK_ZONE_COND_EXP_OPEN)
#define znr_dev_zone_is_open(z)		\
	(znr_dev_zone_imp_open(z) || znr_dev_zone_exp_open(z))
#define znr_dev_zone_closed(z)		((z)->cond == BLK_ZONE_COND_CLOSED)
#define znr_dev_zone_full(z)		((z)->cond == BLK_ZONE_COND_FULL)
#define znr_dev_zone_rdonly(z)		((z)->cond == BLK_ZONE_COND_READONLY)
#define znr_dev_zone_offline(z)		((z)->cond == BLK_ZONE_COND_OFFLINE)

const char *znr_dev_zone_type_str(struct blk_zone *z, bool s);
const char *znr_dev_zone_cond_str(struct blk_zone *z, bool s);

#ifndef BLKZONEREPORTV2
#define BLKREPORTZONEV2		_IOWR(0x12, 142, struct blk_zone_report)
#define BLK_ZONE_REP_CACHED	(1U << 31)
#define BLK_ZONE_COND_ACTIVE	0xFF
#endif

#define znr_dev_zone_active(z)	((z)->cond == BLK_ZONE_COND_ACTIVE)

/* Convert from sectors to bytes for zone accessors */
#define znr_dev_zone_start(z)		((z)->start << SECTOR_SHIFT)
#define znr_dev_zone_len(z)		((z)->len << SECTOR_SHIFT)
#define znr_dev_zone_capacity(z)	((z)->capacity << SECTOR_SHIFT)
#define znr_dev_zone_wp(z)		((z)->wp << SECTOR_SHIFT)

int znr_dev_open(void);
void znr_dev_close(void);

int znr_dev_report_zones(struct znr_device *dev, unsigned int start_zone_no,
			 struct blk_zone *zones, unsigned int nr_zones);
char *znr_dev_get_zone_info(struct blk_zone *blkz,
			    char *buffer, size_t buffer_size);

#endif /* ZNR_DEVICE_H */
