/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#ifndef ZNR_NET_H
#define ZNR_NET_H

#include "config.h"
#include "znr_device.h"
#include "znr_fs.h"

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if __BIG_ENDIAN__
# define htonll(x)	(x)
# define ntohll(x)	(x)
#else
# define htonll(x)	\
	(((__u64)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
# define ntohll(x)	\
	(((__u64)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif

#define ZNR_NET_DEFAULT_PORT	49152

#define ZNR_NET_SOCKBUF_SIZE	(1024 * 1024)

struct znr_net_client {
	int			sd;
	struct sockaddr_in	inaddr;
	socklen_t		inaddrlen;

	char			ip[INET_ADDRSTRLEN + 1];
	int			port;
};

#define ZNR_NET_MAGIC				   \
	(((__u32)'z' << 24) |			   \
	 ((__u32)'o' << 16) |			   \
	 ((__u32)'n' << 8) |			   \
	 ((__u32)'e'))

enum znr_net_req_id {
	ZNR_NET_MNTDIR_INFO = 1,
	ZNR_NET_DEV_INFO,
	ZNR_NET_DEV_REP_ZONES,
	ZNR_NET_FILE_EXTENTS,
	ZNR_NET_EXTENTS_IN_RANGE,
	ZNR_NET_BLOCKGROUPS,
};

struct znr_net_mntdir_info {
	__u32		fs_type;
	__u8		mnt_path[PATH_MAX];
} __attribute__ ((packed));

struct znr_net_dev_info {
	__u8		path[PATH_MAX];
	__u8		vendor_id[ZNR_DEV_VENDOR_ID_LEN + 1];
	__u64		nr_sectors;
	__u64		nr_lblocks;
	__u64		nr_pblocks;
	__u64		zone_size;
	__u32		zone_sectors;
	__u32		lblock_size;
	__u32		pblock_size;
	__u32		nr_zones;
	__u32		max_nr_open_zones;
	__u32		max_nr_active_zones;
	__u8		is_zoned;
} __attribute__ ((packed));

struct znr_net_req {
	__u32		magic;
	__u32		id;
	__u32		zno;
	__u32		nr_zones;
	__u64		sector;
	__u64		nr_sectors;
	__u8		path[PATH_MAX];
} __attribute__ ((packed));

struct znr_net_rep {
	__u32		magic;
	__u32		id;
	__u32		err;
	__u32		data_size;
} __attribute__ ((packed));

int znr_net_connect(struct znr_net_client *ncli);
int znr_net_listen(struct znr_net_client *ncli);
void znr_net_disconnect(struct znr_net_client *ncli);

void znr_net_run_server(struct znr_net_client *ncli);

int znr_net_get_mntdir_info(struct znr_net_client *ncli);
int znr_net_get_dev_info(struct znr_net_client *ncli);
int znr_net_get_dev_rep_zones(struct znr_net_client *ncli,
			      unsigned int start_zone_no,
			      struct blk_zone *zones, unsigned int nr_zones);
int znr_net_get_file_extents(struct znr_net_client *ncli, char *path,
			     struct znr_extent **extents,
			     unsigned int *nr_extents);
int znr_net_get_extents_in_range(struct znr_net_client *ncli,
				 unsigned long long sector,
				 unsigned long long nr_sectors,
				 struct znr_extent **extents,
				 unsigned int *nr_extents);
int znr_net_get_blockgroups(struct znr_net_client *ncli,
			    struct znr_bg **blockgroups,
			    unsigned int *nr_blockgroups);

#endif /* ZNR_NET_H */
