// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "znr.h"

static struct znr_fs znrfs_net[] = {
	{ ZNR_FS_XFS,		"XFS",	NULL },
	{ ZNR_FS_UNKNOWN,	NULL,	NULL },
};

static struct znr_fs *znr_fs_get_net(enum znr_supported_fs type)
{
	struct znr_fs *fs = &znrfs_net[0];

	if (!fs)
		return NULL;

	while (fs->type != ZNR_FS_UNKNOWN) {
		if (fs->type == type)
			return fs;
		fs++;
	}

	return NULL;
}

static int znr_net_send(struct znr_net_client *ncli,
			void *buf, size_t buf_size)
{
	ssize_t ret;

	while (buf_size) {
		ret = send(ncli->sd, buf, buf_size, 0);
		if (!ret)
			return -ECONNRESET;
		if (ret < 0) {
			znr_err("send failed (%s)\n", strerror(errno));
			return -errno;
		}

		buf_size -= ret;
		buf = (uint8_t *)buf + ret;
	}

	return 0;
}

static int znr_net_recv(struct znr_net_client *ncli,
			void *buf, size_t buf_size)
{
	ssize_t ret;

	while (buf_size) {
		ret = recv(ncli->sd, buf, buf_size, 0);
		if (!ret)
			return -ECONNRESET;
		if (ret < 0) {
			znr_err("recv failed (%s)\n", strerror(errno));
			return -errno;
		}

		buf_size -= ret;
		buf = (uint8_t *)buf + ret;
	}

	return 0;
}

static int znr_net_send_req(struct znr_net_client *ncli,
			    enum znr_net_req_id id,
			    __u32 zno, __u32 nr_zones,
			    __u64 sector, __u64 nr_sectors,
			    char *path)
{
	struct znr_net_req req = {
		.magic = htonl(ZNR_NET_MAGIC),
		.id = htonl(id),
		.zno = htonl(zno),
		.nr_zones = htonl(nr_zones),
		.sector = htonll(sector),
		.nr_sectors = htonll(nr_sectors),
	};

	if (path)
		strncpy((char *)req.path, path, sizeof(req.path) - 1);

	return znr_net_send(ncli, (void *) &req, sizeof(req));
}

static int znr_net_recv_req(struct znr_net_client *ncli,
			    struct znr_net_req *req)
{
	int ret;

	memset(req, 0, sizeof(*req));

	ret = znr_net_recv(ncli, (void *) req, sizeof(*req));
	if (ret)
		return ret;

	req->magic = ntohl(req->magic);
	if (req->magic != ZNR_NET_MAGIC) {
		znr_err("Invalid request magic (0x%08x != 0x%08x)\n",
			req->magic, ZNR_NET_MAGIC);
		return -1;
	}

	req->id = ntohl(req->id);
	switch (req->id) {
	case ZNR_NET_MNTDIR_INFO:
	case ZNR_NET_DEV_INFO:
	case ZNR_NET_FILE_EXTENTS:
	case ZNR_NET_BLOCKGROUPS:
		return 0;
	case ZNR_NET_DEV_REP_ZONES:
		req->zno = ntohl(req->zno);
		req->nr_zones = ntohl(req->nr_zones);
		return 0;
	case ZNR_NET_EXTENTS_IN_RANGE:
		req->zno = ntohl(req->zno);
		req->sector = ntohll(req->sector);
		req->nr_sectors = ntohll(req->nr_sectors);
		return 0;
	default:
		znr_err("Invalid request ID\n");
		return -1;
	}
}

static int znr_net_send_rep(struct znr_net_client *ncli,
			    enum znr_net_req_id id,
			    int err, void *data, __u32 data_size)
{
	struct znr_net_rep rep = {
		.magic = htonl(ZNR_NET_MAGIC),
		.id = htonl(id),
	};
	int ret;

	rep.err = htonl(err);
	if (err)
		data_size = 0;

	rep.data_size = htonl(data_size);
	ret = znr_net_send(ncli, (void *) &rep, sizeof(rep));
	if (!ret && data_size)
		ret = znr_net_send(ncli, data, data_size);

	return ret;
}

static int znr_net_recv_rep(struct znr_net_client *ncli,
			    enum znr_net_req_id id,
			    int *err, void **data, size_t *data_size)
{
	struct znr_net_rep rep;
	void *data_buf = NULL;
	int ret;

	ret = znr_net_recv(ncli, (void *) &rep, sizeof(rep));
	if (ret)
		return ret;

	rep.magic = ntohl(rep.magic);
	if (rep.magic != ZNR_NET_MAGIC) {
		znr_err("Invalid reply magic (0x%08x != 0x%08x)\n",
			rep.magic, ZNR_NET_MAGIC);
		return -1;
	}

	rep.id = ntohl(rep.id);
	if (rep.id != id) {
		znr_err("Invalid reply ID\n");
		return -1;
	}

	*err = ntohl(rep.err);
	if (*err) {
		errno = *err;
		*data = NULL;
		*data_size = 0;
		return 0;
	}

	/* Get the data, if any. */
	rep.data_size = ntohl(rep.data_size);
	if (rep.data_size) {
		data_buf = malloc(rep.data_size);
		if (!data_buf) {
			znr_err("Failed to allocate %u B data buffer\n",
				rep.data_size);
			return -ENOMEM;
		}

		ret = znr_net_recv(ncli, data_buf, rep.data_size);
		if (ret) {
			free(data_buf);
			data_buf = NULL;
			rep.data_size = 0;
		}
	}

	*data = data_buf;
	*data_size = rep.data_size;

	return ret;
}

static int znr_net_send_mntdir_info_rep(struct znr_net_client *ncli)
{
	struct znr_net_mntdir_info mntdir_info;

	znr_verbose("Sending mntdir info reply\n");

	memset(&mntdir_info, 0, sizeof(mntdir_info));
	mntdir_info.fs_type = znr.mnt_dir.fs->type;
	strncpy((char *)mntdir_info.mnt_path, znr.mnt_dir.path,
		sizeof(mntdir_info.mnt_path) - 1);

	return znr_net_send_rep(ncli, ZNR_NET_MNTDIR_INFO, 0,
				&mntdir_info, sizeof(mntdir_info));
}

static int znr_net_send_dev_info_rep(struct znr_net_client *ncli)
{
	struct znr_net_dev_info dev_info;
	struct znr_device *dev = &znr.dev;

	znr_verbose("Sending device info reply\n");

	memset(&dev_info, 0, sizeof(dev_info));
	strncpy((char *)dev_info.path, znr.dev_path,
		sizeof(dev_info.path) - 1);
	strncpy((char *)dev_info.vendor_id, dev->vendor_id,
		sizeof(dev_info.vendor_id));
	dev_info.nr_sectors = htonll(dev->nr_sectors);
	dev_info.nr_lblocks = htonll(dev->nr_lblocks);
	dev_info.nr_pblocks = htonll(dev->nr_pblocks);
	dev_info.zone_size = htonll(dev->zone_size);
	dev_info.zone_sectors = htonl(dev->zone_sectors);
	dev_info.lblock_size = htonl(dev->lblock_size);
	dev_info.pblock_size = htonl(dev->pblock_size);
	dev_info.nr_zones = htonl(dev->nr_zones);
	dev_info.max_nr_open_zones = htonl(dev->max_nr_open_zones);
	dev_info.max_nr_active_zones = htonl(dev->max_nr_active_zones);
	dev_info.is_zoned = dev->is_zoned;

	return znr_net_send_rep(ncli, ZNR_NET_DEV_INFO, 0,
				&dev_info, sizeof(dev_info));
}

static int znr_net_send_dev_rep_zones_rep(struct znr_net_client *ncli,
					  struct znr_net_req *req)
{
	unsigned int zno = req->zno;
	unsigned int nr_zones = req->nr_zones;
	struct blk_zone *blkz;
	__u32 data_size = 0;
	unsigned int i;
	ssize_t ret;
	int err = 0;

	znr_verbose("Sending zone report reply (from %u, %u zones)\n",
		    zno, nr_zones);

	if (zno >= znr.dev.nr_zones) {
		znr_err("Invalid start zone number %u / %u\n",
			zno, znr.dev.nr_zones);
		err = EINVAL;
		goto reply;
	}
	if (!nr_zones || zno + nr_zones - 1 >= znr.dev.nr_zones) {
		znr_err("Invalid number of zones %u\n",
			nr_zones);
		err = EINVAL;
		goto reply;
	}

	ret = znr_dev_report_zones(&znr.dev, zno,
				   &znr.blk_zones[zno], nr_zones);
	if (ret < 0) {
		znr_err("Get zone information failed %d (%s)\n",
			errno, strerror(errno));
		err = -ret;
		goto reply;
	}

	if ((unsigned int)ret != nr_zones) {
		znr_err("Got %zd zones, expected %u zones\n",
			ret, nr_zones);
		err = EINVAL;
		goto reply;
	}

	blkz = &znr.blk_zones[zno];
	for (i = 0; i < nr_zones; i++, blkz++) {
		blkz->start = htonll(blkz->start);
		blkz->len = htonll(blkz->len);
		blkz->wp = htonll(blkz->wp);
		blkz->capacity = htonll(blkz->capacity);
	}

	data_size = nr_zones * sizeof(struct blk_zone);

reply:
	ret = znr_net_send_rep(ncli, ZNR_NET_DEV_REP_ZONES, err,
			       &znr.blk_zones[zno], data_size);

	if (data_size) {
		/* We used znr zone array, so we must restore it ! */
		blkz = &znr.blk_zones[zno];
		for (i = 0; i < nr_zones; i++, blkz++) {
			blkz->start = ntohll(blkz->start);
			blkz->len = ntohll(blkz->len);
			blkz->wp = ntohll(blkz->wp);
			blkz->capacity = ntohll(blkz->capacity);
		}
	}

	return ret;
}

static int znr_net_send_file_extents_rep(struct znr_net_client *ncli,
					 struct znr_net_req *req)
{
	struct znr_fs_file *f = NULL;
	struct znr_extent *extents = NULL, *ext;
	unsigned int nr_extents = 0;
	__u32 data_size = 0;
	unsigned int i;
	int ret, err = 0;

	znr_verbose("Sending file %s extents reply\n", req->path);

	ret = znr_fs_get_file_extents_by_path((char *)req->path,
					      &f, &extents, &nr_extents);
	if (ret < 0) {
		err = -ret;
		goto reply;
	}

	if (nr_extents) {
		ext = &extents[0];
		for (i = 0; i < nr_extents; i++, ext++) {
			ext->idx = htonl(ext->idx);
			ext->sector = htonll(ext->sector);
			ext->nr_sectors = htonll(ext->nr_sectors);
			ext->ino = htonll(ext->ino);
		}

		data_size = nr_extents * sizeof(struct znr_extent);
	}

reply:
	ret = znr_net_send_rep(ncli, ZNR_NET_FILE_EXTENTS, err,
			       extents, data_size);

	znr_fs_free_file(f);
	free(extents);
	return ret;
}

static int znr_net_send_blockgroups(struct znr_net_client *ncli,
				    struct znr_net_req *req)
{
	struct znr_bg *bg = NULL, *bg_start;
	unsigned int data_size, nr_blockgroups, i;
	int ret, err = 0;

	znr_verbose("Sending blockgroups information\n");

	ret = znr_bg_get_blockgroups(&bg, &nr_blockgroups);
	if (ret < 0) {
		fprintf(stderr, "Failed to get blockgroups\n");
		err = ret;
		goto err_reply;
	}
	bg_start = bg;
	for (i = 0; i < nr_blockgroups; i++, bg++) {
		bg->sector = htonll(bg->sector);
		bg->nr_sectors = htonll(bg->nr_sectors);
		bg->wp_sector = htonll(bg->wp_sector);
		bg->flags = htonl(bg->flags);
	}

	/* First send the number of blockgroups */
	data_size = sizeof(nr_blockgroups);
	ret = znr_net_send_rep(ncli, ZNR_NET_BLOCKGROUPS, err, &nr_blockgroups,
			       data_size);
	if (ret) {
		znr_err("Failed to send number of blockgroups\n");
		goto free;
	}

	/* Send the blockgroups */
	data_size = sizeof(struct znr_bg) * nr_blockgroups;
	ret = znr_net_send_rep(ncli, ZNR_NET_BLOCKGROUPS, err,
			       bg_start, data_size);
	if (ret)
		znr_err("Failed to send %u blockgroups\n", nr_blockgroups);
free:
	free(bg_start);
	return ret;

err_reply:
	ret = znr_net_send_rep(ncli, ZNR_NET_BLOCKGROUPS, err, NULL, 0);
	return ret;
}

static int znr_net_send_extents_in_range_rep(struct znr_net_client *ncli,
					     struct znr_net_req *req)
{
	struct znr_extent *extents = NULL, *ext;
	unsigned int nr_extents = 0;
	__u32 data_size = 0;
	unsigned int i;
	int ret, err = 0;

	znr_verbose("Sending extents in range %llu + %llu reply\n",
		    req->sector, req->nr_sectors);

	ret = znr_fs_get_extents_in_range(req->sector, req->nr_sectors,
					  &extents, &nr_extents);
	if (ret < 0) {
		znr_err("Extents in range %llu + %llu failed\n",
			req->sector, req->nr_sectors);
		err = -ret;
		goto reply;
	}

	if (nr_extents) {
		ext = &extents[0];
		for (i = 0; i < nr_extents; i++, ext++) {
			ext->idx = htonl(ext->idx);
			ext->sector = htonll(ext->sector);
			ext->nr_sectors = htonll(ext->nr_sectors);
			ext->ino = htonll(ext->ino);
		}

		data_size = nr_extents * sizeof(struct znr_extent);
	}

reply:
	ret = znr_net_send_rep(ncli, ZNR_NET_EXTENTS_IN_RANGE, err,
			       extents, data_size);

	free(extents);
	return ret;
}

static void znr_net_client_init(struct znr_net_client *ncli)
{
	memset(ncli, 0, sizeof(*ncli));
	ncli->inaddrlen = sizeof(struct sockaddr_in);
}

static int znr_net_get_port(void)
{
	/* Prepare the listening socket. */
	if (!znr.port)
		return ZNR_NET_DEFAULT_PORT;
	if (znr.port < 65535)
		return znr.port;

	znr_err("Invalid port %d\n", znr.port);

	return -1;
}

static void znr_net_setsockopt(struct znr_net_client *ncli)
{
	size_t sockbuf_size;
	int ret;

	/* Change the socket send and receive buffer size */
	sockbuf_size = ZNR_NET_SOCKBUF_SIZE;
	ret = setsockopt(ncli->sd, SOL_SOCKET, SO_RCVBUF, &sockbuf_size,
			 sizeof(size_t));
	if (ret < 0) {
		znr_err("setsockopt SO_RCVBUF failed (%s)\n", strerror(errno));
		return;
	}

	sockbuf_size = ZNR_NET_SOCKBUF_SIZE;
	ret = setsockopt(ncli->sd, SOL_SOCKET, SO_SNDBUF, &sockbuf_size,
			 sizeof(size_t));
	if (ret < 0) {
		znr_err("setsockopt SO_SNDBUF failed (%s)\n", strerror(errno));
		return;
	}
}

void znr_net_disconnect(struct znr_net_client *ncli)
{
	if (ncli->sd > 0) {
		printf("Disconnecting client %s:%d\n",
		       ncli->ip, ncli->port);

		shutdown(ncli->sd, SHUT_RDWR);
		close(ncli->sd);
		ncli->sd = 0;
	}
}

int znr_net_connect(struct znr_net_client *ncli)
{
	int ret;

	znr_net_client_init(ncli);

	ncli->port = znr_net_get_port();
	if (ncli->port < 0)
		return ncli->port;

	if (inet_pton(AF_INET, znr.ipaddr, &ncli->inaddr.sin_addr) <= 0) {
		znr_err("Invalid address %s\n", znr.ipaddr);
		return -errno;
	}

	inet_ntop(AF_INET, &ncli->inaddr.sin_addr, ncli->ip,
		  INET_ADDRSTRLEN);

	ncli->inaddr.sin_family = AF_INET;
	ncli->inaddr.sin_addr.s_addr = inet_addr(ncli->ip);
	ncli->inaddr.sin_port = htons((short)ncli->port);

	ncli->sd = socket(PF_INET, SOCK_STREAM, 0);
	if (ncli->sd < 0) {
		znr_err("socket failed (%s)", strerror(errno));
		return -errno;
	}

	printf("Connecting to %s:%d...\n",
	       ncli->ip, (int)ncli->port);

	ret = connect(ncli->sd,
		      (struct sockaddr *) &ncli->inaddr,
		      (socklen_t) sizeof(struct sockaddr_in));
	if (ret) {
		znr_err("connect failed (%s)", strerror(errno));
		znr_net_disconnect(ncli);
		return ret;
	}

	znr_net_setsockopt(ncli);

	return 0;
}

static void znr_net_listen_close(void)
{
	if (znr.listen_sd > 0) {
		close(znr.listen_sd);
		znr.listen_sd = -1;
	}
}

int znr_net_listen(struct znr_net_client *ncli)
{
	struct sockaddr_in bindaddr;
	int val, ret;

	if (!znr.listen_sd) {
		znr.listen_port = znr_net_get_port();
		if (znr.listen_port < 0)
			return znr.listen_port;

		znr.listen_sd = socket(PF_INET, SOCK_STREAM, 0);
		if (znr.listen_sd < 0) {
			znr_err("socket failed (%s)", strerror(errno));
			return -1;
		}

		val = 1;
		ret = setsockopt(znr.listen_sd, SOL_SOCKET, SO_REUSEADDR,
				 &val, sizeof(int));
		if (ret) {
			znr_err("setsockopt failed (%s)", strerror(errno));
			goto close;
		}

		memset(&bindaddr, 0, sizeof(bindaddr));
		bindaddr.sin_family = PF_INET;
		bindaddr.sin_port = htons(znr.listen_port);
		ret = bind(znr.listen_sd, (struct sockaddr *) &bindaddr,
			   sizeof(struct sockaddr_in));
		if (ret) {
			znr_err("bind failed (%s)", strerror(errno));
			ret = -errno;
			goto close;
		}

		/* Listen for connections. */
		if (listen(znr.listen_sd, 1) < 0) {
			znr_err("listen failed (%s)", strerror(errno));
			ret = -errno;
			goto close;
		}

		printf("Listening for connections on port %d...\n",
		       znr.listen_port);
	}

	znr_net_client_init(ncli);
	ncli->sd = accept(znr.listen_sd,
			  (struct sockaddr *) &ncli->inaddr,
			  &ncli->inaddrlen);
	if (ncli->sd < 0) {
		if (errno != EINTR)
			znr_err("accept failed (%s)", strerror(errno));
		ret = -errno;
		goto close;
	}

	inet_ntop(AF_INET, &ncli->inaddr.sin_addr, ncli->ip,
		  INET_ADDRSTRLEN);
	ncli->port = ntohs(ncli->inaddr.sin_port);

	znr_net_setsockopt(ncli);

	printf("Connection from %s:%d\n", ncli->ip, ncli->port);

	return 0;

close:
	znr_net_listen_close();

	return ret;
}

static void znr_net_server(struct znr_net_client *ncli)
{
	struct znr_net_req req;
	int ret = 0;

	printf("Waiting for client %s:%d requests\n",
	       ncli->ip, ncli->port);

	while (!znr.abort && !ret) {
		/* Get a request */
		ret = znr_net_recv_req(ncli, &req);
		if (ret)
			break;

		switch (req.id) {
		case ZNR_NET_MNTDIR_INFO:
			ret = znr_net_send_mntdir_info_rep(ncli);
			break;
		case ZNR_NET_DEV_INFO:
			ret = znr_net_send_dev_info_rep(ncli);
			break;
		case ZNR_NET_DEV_REP_ZONES:
			ret = znr_net_send_dev_rep_zones_rep(ncli, &req);
			break;
		case ZNR_NET_FILE_EXTENTS:
			ret = znr_net_send_file_extents_rep(ncli, &req);
			break;
		case ZNR_NET_EXTENTS_IN_RANGE:
			ret = znr_net_send_extents_in_range_rep(ncli, &req);
			break;
		case ZNR_NET_BLOCKGROUPS:
			ret = znr_net_send_blockgroups(ncli, &req);
			break;
		default:
			ret = -1;
			break;
		}
	}
}

void znr_net_run_server(struct znr_net_client *ncli)
{
	int ret;

	if (znr.connect) {
		/* Connect to client. */
		ret = znr_net_connect(ncli);
		if (!ret) {
			znr_net_server(ncli);
			znr_net_disconnect(ncli);
		}
		return;
	}

	/* Wait for client connections. */
	while (!znr.abort) {
		ret = znr_net_listen(ncli);
		if (ret)
			break;

		znr_net_server(ncli);
		znr_net_disconnect(ncli);
	}

	znr_net_disconnect(ncli);
	znr_net_listen_close();
}

int znr_net_get_mntdir_info(struct znr_net_client *ncli)
{
	struct znr_net_mntdir_info *mntdir_info = NULL;
	size_t data_size = 0;
	int ret, err;

	znr_verbose("Sending mntdir info request\n");

	ret = znr_net_send_req(ncli, ZNR_NET_MNTDIR_INFO, 0, 0, 0, 0, NULL);
	if (ret)
		return ret;

	ret = znr_net_recv_rep(ncli, ZNR_NET_MNTDIR_INFO, &err,
			       (void **)&mntdir_info, &data_size);
	if (ret)
		return ret;

	if (err) {
		znr_err("Get mntdir info failed\n");
		return -err;
	}

	if (data_size != sizeof(*mntdir_info)) {
		znr_err("Invalid mntdir info size (%zu != %zu)\n",
			data_size, sizeof(*mntdir_info));
		ret = -1;
		goto free;
	}

	znr.mnt_dir.path = strdup((char *)mntdir_info->mnt_path);
	if (!znr.mnt_dir.path) {
		znr_err("Get FS path failed\n");
		ret = -1;
		goto free;
	}

	znr.mnt_dir.fs = znr_fs_get_net(mntdir_info->fs_type);
	if (!znr.mnt_dir.fs) {
		znr_err("Get FS type failed\n");
		ret = -1;
	}

free:
	free(mntdir_info);

	return ret;
}

int znr_net_get_dev_info(struct znr_net_client *ncli)
{
	struct znr_device *dev = &znr.dev;
	struct znr_net_dev_info *dev_info = NULL;
	size_t data_size = 0;
	int ret, err;

	znr_verbose("Sending device info request\n");

	ret = znr_net_send_req(ncli, ZNR_NET_DEV_INFO, 0, 0, 0, 0, NULL);
	if (ret)
		return ret;

	ret = znr_net_recv_rep(ncli, ZNR_NET_DEV_INFO, &err,
			       (void **)&dev_info, &data_size);
	if (ret)
		return ret;

	if (err) {
		znr_err("Get device info failed\n");
		return -1;
	}

	if (data_size != sizeof(*dev_info)) {
		znr_err("Invalid device info size (%zu != %zu)\n",
			data_size, sizeof(dev_info));
		ret = -1;
		goto free;
	}

	znr.dev_path = strdup((char *)dev_info->path);
	if (!znr.dev_path) {
		znr_err("Failed to get device path\n");
		goto free;
	}

	strncpy(dev->vendor_id, (char *)dev_info->vendor_id,
		sizeof(dev->vendor_id));
	dev->nr_sectors = ntohll(dev_info->nr_sectors);
	dev->nr_lblocks = ntohll(dev_info->nr_lblocks);
	dev->nr_pblocks = ntohll(dev_info->nr_pblocks);
	dev->zone_size = ntohll(dev_info->zone_size);
	dev->zone_sectors = ntohl(dev_info->zone_sectors);
	dev->lblock_size = ntohl(dev_info->lblock_size);
	dev->pblock_size = ntohl(dev_info->pblock_size);
	dev->nr_zones = ntohl(dev_info->nr_zones);
	dev->max_nr_open_zones = ntohl(dev_info->max_nr_open_zones);
	dev->max_nr_active_zones = ntohl(dev_info->max_nr_active_zones);
	dev->is_zoned = dev_info->is_zoned;

free:
	free(dev_info);

	return ret;
}

int znr_net_get_dev_rep_zones(struct znr_net_client *ncli,
			      unsigned int zno,
			      struct blk_zone *zones, unsigned int nr_zones)
{
	void *data = NULL;
	struct blk_zone *blkz;
	size_t data_size = 0;
	unsigned int i;
	int err, ret = 0;

	znr_verbose("Sending zone report request (from %u, %u zones)\n",
		    zno, nr_zones);

	ret = znr_net_send_req(ncli, ZNR_NET_DEV_REP_ZONES,
			       zno, nr_zones, 0, 0, NULL);
	if (ret)
		return ret;

	ret = znr_net_recv_rep(ncli, ZNR_NET_DEV_REP_ZONES, &err,
			       &data, &data_size);
	if (ret)
		return ret;

	if (err) {
		znr_err("Get report zones failed\n");
		return -1;
	}

	if (data_size != sizeof(struct blk_zone) * nr_zones) {
		znr_err("Invalid number of zones in report\n");
		ret = -1;
		goto free;
	}

	znr_verbose("Zone report: %u zones from zone %u\n",
		    nr_zones, zno);

	blkz = data;
	for (i = 0; i < nr_zones; i++, blkz++, zones++) {
		zones->start = ntohll(blkz->start);
		zones->len = ntohll(blkz->len);
		zones->wp = ntohll(blkz->wp);
		zones->type = blkz->type;
		zones->cond = blkz->cond;
		zones->non_seq = blkz->non_seq;
		zones->reset = blkz->reset;
		zones->capacity = ntohll(blkz->capacity);
	}

free:
	free(data);

	if (ret)
		return ret;
	return nr_zones;
}

int znr_net_get_file_extents(struct znr_net_client *ncli, char *path,
			     struct znr_extent **extents,
			     unsigned int *nr_extents)
{
	struct znr_extent *ext = NULL;
	unsigned int i, nr_ext = 0;
	size_t data_size;
	int err, ret;

	znr_verbose("Sending file %s extent request\n", path);

	*extents = NULL;
	*nr_extents = 0;

	if (!path || !strlen(path)) {
		znr_err("Invalid file path\n");
		return -1;
	}
	ret = znr_net_send_req(ncli, ZNR_NET_FILE_EXTENTS, 0, 0, 0, 0, path);
	if (ret)
		return ret;

	ret = znr_net_recv_rep(ncli, ZNR_NET_FILE_EXTENTS, &err,
			       (void **)&ext, &data_size);
	if (ret)
		return ret;

	if (err) {
		znr_err("Get file %s extents failed\n", path);
		return -1;
	}

	if (data_size % sizeof(struct znr_extent)) {
		znr_err("Data size is not aligned to struct znr_extent\n");
		free(ext);
		return -1;
	}

	nr_ext = data_size / sizeof(struct znr_extent);
	*extents = ext;
	*nr_extents = nr_ext;

	znr_verbose("File %s: %u extents\n", path, nr_ext);

	for (i = 0; i < nr_ext; i++, ext++) {
		ext->idx = ntohl(ext->idx);
		ext->sector = ntohll(ext->sector);
		ext->nr_sectors = ntohll(ext->nr_sectors);
		ext->ino = ntohll(ext->ino);
	}

	return ret;
}

int znr_net_get_extents_in_range(struct znr_net_client *ncli,
				 unsigned long long sector,
				 unsigned long long nr_sectors,
				 struct znr_extent **extents,
				 unsigned int *nr_extents)
{
	struct znr_extent *ext = NULL;
	unsigned int i, nr_ext = 0;
	size_t data_size;
	int err, ret;

	znr_verbose("Sending extent request in range %llu + %llu\n",
		    sector, nr_sectors);

	*extents = NULL;
	*nr_extents = 0;

	if (sector >= znr.dev.nr_sectors ||
	    sector + nr_sectors > znr.dev.nr_sectors) {
		znr_err("Invalid sector range %llu + %llu\n",
			sector, nr_sectors);
		return -EINVAL;
	}

	ret = znr_net_send_req(ncli, ZNR_NET_EXTENTS_IN_RANGE, 0, 0,
			       sector, nr_sectors, NULL);
	if (ret)
		return ret;

	ret = znr_net_recv_rep(ncli, ZNR_NET_EXTENTS_IN_RANGE, &err,
			       (void **)&ext, &data_size);
	if (ret)
		return ret;

	if (err) {
		znr_err("Get extent range %llu + %llu reply failed\n",
			sector, nr_sectors);
		return -1;
	}

	if (data_size % sizeof(struct znr_extent)) {
		znr_err("Data size is not aligned to struct znr_extent\n");
		free(ext);
		return -1;
	}

	nr_ext = data_size / sizeof(struct znr_extent);
	*extents = ext;
	*nr_extents = nr_ext;

	znr_verbose("Sector range %llu + %llu: %u extents\n",
		    sector, nr_sectors, nr_ext);

	for (i = 0; i < nr_ext; i++, ext++) {
		ext->idx = ntohl(ext->idx);
		ext->sector = ntohll(ext->sector);
		ext->nr_sectors = ntohll(ext->nr_sectors);
		ext->ino = ntohll(ext->ino);
	}

	return ret;
}

int znr_net_get_blockgroups(struct znr_net_client *ncli,
			    struct znr_bg **blockgroups,
			    unsigned int *nr_blockgroups)
{
	void *data = NULL;
	size_t data_size = 0;
	struct znr_bg *bg;
	unsigned int i;
	int err, ret;

	znr_verbose("Sending get blockgroup information\n");

	if (!nr_blockgroups || !blockgroups)
		return -EINVAL;

	ret = znr_net_send_req(ncli, ZNR_NET_BLOCKGROUPS, 0, 0, 0, 0, NULL);
	if (ret)
		return ret;

	/* First receive the number of blockgroups */
	ret = znr_net_recv_rep(ncli, ZNR_NET_BLOCKGROUPS, &err,
			       &data, &data_size);
	if (ret) {
		fprintf(stderr, "Get number of blockgroups failed\n");
		return ret;
	}

	if (data_size != sizeof(*nr_blockgroups)) {
		fprintf(stderr, "Number of blockgroups, receive error\n");
		ret = -EINVAL;
		goto free;
	}

	*nr_blockgroups = *((unsigned int *)data);
	free(data);

	znr_verbose("Get blockgroups: attempting to retrieve  %u blockgroups\n",
		    *nr_blockgroups);
	/* Get blockgroups data */
	ret = znr_net_recv_rep(ncli, ZNR_NET_BLOCKGROUPS, &err,
			       &data, &data_size);
	if (ret) {
		fprintf(stderr, "Get blockgroups information failed\n");
		return ret;
	}

	if (data_size != sizeof(struct znr_bg) * (*nr_blockgroups)) {
		fprintf(stderr, "Invalid blockgroups information received\n");
		ret = -EINVAL;
		goto free;
	}

	znr_verbose("Get blockgroups: retrieved %u blockgroups\n",
		    *nr_blockgroups);

	bg = data;
	/* It is the callers responsibility to free this memory */
	*blockgroups = bg;
	for (i = 0; i < *nr_blockgroups; i++, bg++) {
		bg->sector = ntohll(bg->sector);
		bg->nr_sectors = ntohll(bg->nr_sectors);
		bg->wp_sector = ntohll(bg->wp_sector);
		bg->flags = ntohl(bg->flags);
	}

	return (int)*nr_blockgroups;
free:
	free(data);

	return ret;
}
