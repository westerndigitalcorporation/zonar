// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.
 *
 * Authors: Wilfred Mallawa (wilfred.mallawa@wdc.com)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#include "znr.h"

int main(int argc, char **argv)
{
	gboolean version = FALSE;
	gboolean verbose = FALSE;
	gboolean listen = FALSE;
	gchar *connect_addr = NULL;
	gint port = 0;
	char *mntdir = NULL;
	GError *error = NULL;
	GOptionContext *context;
	GOptionEntry options[] = {
		{
			"verbose", 'v', 0,
			G_OPTION_ARG_NONE, &verbose,
			"Turn on verbose mode",
			NULL
		},
		{
			"version", 'V', 0,
			G_OPTION_ARG_NONE, &version,
			"Display version information and exit",
			NULL
		},
		{
			"connect", 'c', 0,
			G_OPTION_ARG_STRING, &connect_addr,
			"Connect to the specified server IP address",
			NULL
		},
		{
			"listen", 'l', 0,
			G_OPTION_ARG_NONE, &listen,
			"Reverse mode: wait for connection from a server",
			NULL
		},
		{
			"port", 'p', 0,
			G_OPTION_ARG_INT, &port,
			"Specify the connection port",
			NULL
		},
		G_OPTION_ENTRY_NULL
	};
	int ret = 0;

	printf("%s, version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
	printf("Copyright 2026 (C) Western Digital Corporation or its affiliates.\n\n");

	context = g_option_context_new("[<mntdir>]");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "Option parsing failed: %s\n", error->message);
		exit(1);
	}

	if (version)
		return 0;

	/* Init */
	znr_init();
	znr.verbose = verbose;
	znr.ipaddr = connect_addr;
	znr.port = port;

	if (connect_addr)
		znr.connect = true;
	znr.listen = listen;
	znr.is_net_client = znr.connect || znr.listen;

	if (!znr.is_net_client) {
		if (argc < 2) {
			fprintf(stderr, "No mount directory specified\n");
			return 1;
		}

		if (argc != 2) {
			fprintf(stderr, "Invalid command line\n");
			return 1;
		}

		mntdir = argv[1];
	} else if (argc != 1) {
		fprintf(stderr, "Invalid command line\n");
		return 1;
	}

	if (connect_addr && mntdir) {
		fprintf(stderr,
			"--connect and --mntdir are mutually exclusive\n");
		return 1;
	}

	if (connect_addr && listen) {
		fprintf(stderr,
			"--connect and --listen are mutually exclusive\n");
		return 1;
	}

	if (listen && mntdir) {
		fprintf(stderr,
			"--listen and --mntdir are mutually exclusive\n");
		return 1;
	}

	if (znr.verbose)
		znr_verbose("Verbose mode enabled\n");

	if (znr.connect)
		ret = znr_net_connect(&znr.ncli);
	else if (znr.listen)
		ret = znr_net_listen(&znr.ncli);
	if (ret)
		return ret;

	ret = znr_open(mntdir);
	if (ret) {
		fprintf(stderr, "Failed to open device\n");
		goto out;
	}

	/* Run GUI locally. */
	znr_print_info();
	ret = znr_gui_run();

	znr_close();
out:
	znr_net_disconnect(&znr.ncli);
	if (ret)
		return 1;

	return 0;
}
