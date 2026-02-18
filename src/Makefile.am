# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.

AM_CFLAGS = \
        $(CFLAGS) \
        -Wall -Wextra -Wno-unused-parameter

if HAS_XFS
XFS_SOURCES = znr_xfs.c
endif

bin_PROGRAMS = zonar_srv

zonar_srv_SOURCES = \
	znr.h znr.c \
	znr_fs.h znr_fs.c \
	znr_device.h znr_device.c \
	znr_net.h znr_net.c \
	znr_bg.h znr_bg.c \
	${XFS_SOURCES} \
	zonar_srv.c

zonar_srv_CFLAGS = -D_GNU_SOURCE $(AM_CFLAGS)
zonar_srv_LDADD = -lm

if GUI_ENABLED

bin_PROGRAMS += zonar

zonar_SOURCES = \
	znr.h znr.c \
	znr_fs.h znr_fs.c \
	znr_device.h znr_device.c \
	znr_net.h znr_net.c \
	znr_bg.h znr_bg.c \
	znr_gui.c \
	${XFS_SOURCES} \
	zonar.c

zonar_CFLAGS = -D_GNU_SOURCE $(AM_CFLAGS) $(GTK_CFLAGS) $(LIBADWAITA_CFLAGS)
zonar_LDADD = $(GTK_LIBS) $(LIBADWAITA_LIBS) -lm

endif
