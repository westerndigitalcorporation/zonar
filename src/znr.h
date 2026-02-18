/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_H
#define ZNR_H

#include "config.h"
#include "znr_device.h"
#include "znr_fs.h"
#include "znr_net.h"
#include "znr_bg.h"

/*
 * Main data structure to share FS and device information.
 */
struct znr {
	/*
	 * To run as a client or a server.
	 */
	bool			is_net_client;
	bool			is_net_server;
	bool			connect;
	bool			listen;
	char			*ipaddr;
	int			port;
	int			listen_sd;
	int			listen_port;
	struct znr_net_client	ncli;

	/*
	 * Mount directory & file system.
	 */
	struct znr_fs_file	mnt_dir;

	/*
	 * Device information.
	 */
	char			*dev_path;
	struct znr_device	dev;
	unsigned int            nr_zones;
	unsigned int            nr_conv_zones;
	struct blk_zone		*blk_zones;

	/*
	 * Blockgroups information.
	 */
	unsigned int            nr_blockgroups;
	struct znr_bg           *blockgroups;

	bool			abort;
	bool			verbose;
};

extern struct znr znr;

void znr_init(void);
int znr_open(const char *mntdir);
void znr_close(void);
void znr_print_info(void);

int znr_gui_run(void);

#define znr_printf(stream,format,args...)			\
	do {							\
		fprintf((stream), "[zonar]" format, ## args);	\
		fflush(stream);					\
	} while (0)

#define znr_err(format,args...)	\
	znr_printf(stderr, "[ERROR] " format, ##args)

#define znr_verbose(format,args...)					\
	do {								\
		if (znr.verbose)					\
			znr_printf(stderr, "[DBG] " format, ##args);	\
	} while (0);

#endif /* ZNR_H */
