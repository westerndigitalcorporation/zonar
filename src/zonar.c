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
#include <sys/wait.h>

#include "znr.h"

int znr_waitpid(pid_t pid) {
	int status;
	int ret = 0;

	ret = waitpid(pid, &status, 0);
	if (ret < 0)
		return -1;
	ret = WIFEXITED(status);
	if (!ret) {
		znr_err("zonar_srv: exited with status: %d\n", status);
		return -1;
	}
	return 0;
}

/*
 * Setup the zonar server @srv on @mntdir mount directory as a root
 * child process.
 */
int znr_pkexec(const char* srv, char *mntdir)
{
	char *rpath, *rsrv;
	int ret = -1;
	pid_t pid;

	rpath = realpath(mntdir, NULL);
	if (!rpath) {
		znr_err("Failed to resolve path %s (%s)\n",
			mntdir, strerror(errno));
		return ret;
	}

	/* If the user did not specify a path, default to ZONAR_SRV_PATH, which
	 * is an abosolute path. This needs to match the policy exec.path
	 * for our rules to apply.
	 */
	if (!srv)
		rsrv = strdup(ZONAR_SRV_PATH);
	else
		rsrv = realpath(srv, NULL);
	if (!rsrv) {
		znr_err("Failed to resolve server path %s (%s)\n",
			srv, strerror(errno));
		goto free_rpath;
	}

	pid = fork();
	if (pid < 0)
		goto free_rsrv;
	znr.pid = pid;

	/* child */
	if (pid == 0) {
		char *args[12];
		char port[8];
		uint8_t argc = 0;

		args[argc++] = "pkexec";
		args[argc++] = rsrv;

		if (znr.verbose)
			args[argc++] = "-v";

		args[argc++] = "-c";
		args[argc++] = ZNR_INADDR_LOOPBACK;

		if (znr.port) {
		    if (snprintf(port, sizeof(port), "%d", znr.port) < 0) {
			    znr_err("Failed to convert port to string\n");
		    } else {
			args[argc++] = "-p";
			args[argc++] = port;
		    }
		}

		args[argc++] = rpath;
		args[argc] = NULL;

		/* returns only on error */
		execvp("pkexec", args);
		perror("execvp");
		_exit(127);
	}

	znr_verbose("running Zonar server from: %s\n", rsrv);
	ret = 0;
free_rsrv:
	free(rsrv);
free_rpath:
	free(rpath);
	return ret;
}

int main(int argc, char **argv)
{
	gboolean version = FALSE;
	gboolean verbose = FALSE;
	gboolean listen = FALSE;
	gchar *connect_addr = NULL;
	gint port = 0;
	char *mntdir = NULL;
	char *srvpath = NULL;
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
		{
			"server", 's', 0,
			G_OPTION_ARG_FILENAME, &srvpath,
			"Path to a Zonar server binary to run instead of the system default",
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

	/*
	 * Running locally, so setup the server with escalated privileges and
	 * the GUI as a network client in reverse mode (server connects to us).
	 *
	 * Note that we are just falling back to network symantics here.
	 */
	if (!znr.connect && !znr.listen) {
		znr.listen = true;
		znr.is_net_client = true;
		ret = znr_pkexec(srvpath, mntdir);
		if (ret < 0)
			return 1;
	}

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
	if (znr.pid) {
		znr_net_shutdown(&znr.ncli);
		znr_waitpid(znr.pid);
	} else {
		znr_net_disconnect(&znr.ncli);
	}
	if (ret)
		return 1;

	return 0;
}
