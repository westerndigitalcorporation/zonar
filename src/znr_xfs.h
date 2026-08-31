/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_XFS_H
#define ZNR_XFS_H

#include <xfs/xfs.h>

struct znr_fs_xfs {
	struct xfs_fsop_geom		geo;

	char				*data_path;
	dev_t				data_dev;

	char				*rt_path;
	dev_t				rt_dev;
};

#endif /* ZNR_XFS_H */
