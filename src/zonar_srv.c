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
#include <signal.h>

#include "znr.h"

static void zonar_srv_sigcatcher(int sig)
{
	znr.abort = true;
	printf("\n");
}

static void zonar_srv_usage(char *cmd)
{
	printf("Usage: %s [options] <FS mount directory>\n", cmd);
	printf("Options:\n");
	printf("  --help | -h             : Print this help and exit\n");
	printf("  --version | -V          : Print version and exit\n");
	printf("  --verbose | -v          : Enable verbose output\n");
	printf("  --connect | -c <ipaddr> : Reverse mode (Connect to client)\n");
	printf("  --port | -p <port>      : Specify connection port number\n");
	printf("                            Default: %d\n",
	       ZNR_NET_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
	char *mntdir = NULL;
	struct sigaction act;
	int ret, i;

	/* By default: listen for connections. */
	znr_init();
	znr.is_net_server = true;
	znr.listen = true;

	/* Setup signal handler */
	act.sa_flags = 0;
	act.sa_handler = zonar_srv_sigcatcher;
	sigemptyset(&act.sa_mask);
	(void)sigaction(SIGPIPE, &act, NULL);
	(void)sigaction(SIGINT, &act, NULL);
	(void)sigaction(SIGTERM, &act, NULL);

	printf("%s (server), version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
	printf("Copyright 2026 (C) Western Digital Corporation or its affiliates.\n\n");

	/* Parse command line */
	if (argc < 2) {
		zonar_srv_usage(argv[0]);
		return -1;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 ||
		    strcmp(argv[i], "-h") == 0) {
			zonar_srv_usage(argv[0]);
			return 0;
		}

		if (strcmp(argv[i], "--version") == 0 ||
		    strcmp(argv[i], "-V") == 0)
			return 0;

		if (strcmp(argv[i], "--verbose") == 0 ||
		    strcmp(argv[i], "-v") == 0) {
			znr.verbose = true;
			continue;
		}

		if (strcmp(argv[i], "--port") == 0 ||
		    strcmp(argv[i], "-p") == 0) {
			i++;
			if (i >= argc - 1) {
				fprintf(stderr, "Invalid command line\n");
				return 1;
			}

			znr.port = atoi(argv[i]);
			if (znr.port <= 0) {
				fprintf(stderr, "Invalid port\n");
				return 1;
			}
			continue;
		}

		if (strcmp(argv[i], "--connect") == 0 ||
		    strcmp(argv[i], "-c") == 0) {
			i++;
			if (i >= argc - 1) {
				fprintf(stderr, "Invalid command line\n");
				return 1;
			}

			znr.connect = true;
			znr.listen = false;
			znr.ipaddr = argv[i];
			continue;
		}

		if (argv[i][0] == '-') {
			fprintf(stderr, "Invalid option %s\n", argv[i]);
			return 1;
		}

		break;
	}

	if (i != argc - 1) {
		zonar_srv_usage(argv[0]);
		return 1;
	}

	mntdir = argv[i];

	if (znr.verbose)
		printf("Verbose mode enabled\n");

	ret = znr_open(mntdir);
	if (ret)
		return 1;

	znr_print_info();

	/* Run as a server (no GUI). */
	znr_net_run_server(&znr.ncli);

	znr_close();

	return 0;
}
