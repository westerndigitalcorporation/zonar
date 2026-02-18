# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.

AC_INIT([zonar], [1.0.0],
	[wilfred.mallawa@wdc.com, alistair.francis@wdc.com, dlemoal@kernel.org],
	[zonar], [https://github.com/westerndigitalcorporation/zonar])

AC_CONFIG_AUX_DIR([build-aux])
AC_CANONICAL_TARGET
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([src/config.h])
AC_PREFIX_DEFAULT(/usr)
AM_INIT_AUTOMAKE([-Wall foreign subdir-objects])
AM_SILENT_RULES([yes])

# Change default CFLAGS from "-g -O2" to "-O2" for regular builds.
AC_ARG_ENABLE(debug,
    [  --enable-debug          Compile with "-g" option],
    [DBGCFLAGS="-ggdb"],
    [DBGCFLAGS="-O2"])
CFLAGS="$CFLAGS $DBGCFLAGS"

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
m4_pattern_allow([AM_PROG_AR])
LT_INIT

# Package version details: <major>.<minor>.<release>
PACKAGE_VERSION_MAJOR=$(echo $PACKAGE_VERSION | awk -F. '{print $1}')
PACKAGE_VERSION_MINOR=$(echo $PACKAGE_VERSION | awk -F. '{print $2}')
PACKAGE_VERSION_RELEASE=$(echo $PACKAGE_VERSION | awk -F. '{print $3}')

# zonar friendly version format
ZONAR_VERSION_LT=$PACKAGE_VERSION_MAJOR:$PACKAGE_VERSION_MINOR:$PACKAGE_VERSION_RELEASE
AC_SUBST([ZONAR_VERSION_LT])

AC_ARG_ENABLE([gui],
	[AS_HELP_STRING([--disable-gui], [Disable zonar GUI (Default: no)])],
	[
		case "$enableval" in
		yes)
			disable_gui=no;;
		no)
			disable_gui=yes;;
		*)
			AC_MSG_ERROR([Invalid value "$enableval" for --enable-gui]);;
		esac
	],
	[disable_gui=no]
)

# Checks for header files
AC_CHECK_HEADER(linux/blkzoned.h, [],
		[AC_MSG_ERROR([Couldn't find linux/blkzoned.h. Kernel too old ?])],
		[[#include <linux/blkzoned.h>]])

AC_CHECK_HEADER(linux/openat2.h, [],
		[AC_MSG_ERROR([Couldn't find linux/openat2.h. Kernel too old ?])],
		[[#include <linux/openat2.h>]])

AC_CHECK_HEADER(sys/syscall.h, [],
		[AC_MSG_ERROR([Couldn't find sys/syscall.h. Kernel too old ?])],
		[[#include <sys/syscall.h>]])

AC_CHECK_HEADERS([xfs/xfs.h], [],
		[AC_MSG_ERROR([xfs/xfs.h not found])])

# Checks for macros and struct members
AC_CHECK_MEMBER([struct blk_zone.capacity],
		[AC_DEFINE(HAVE_BLK_ZONE_CAPACITY, [1],
			[report zones includes zone capacity])],
		[], [[#include <linux/blkzoned.h>]])

AC_COMPILE_IFELSE(
	[AC_LANG_SOURCE([[
		#include <sys/syscall.h>
		#ifndef SYS_openat2
		# error SYS_openat2 is not defined
		#endif
	]])],
	[OPTION_FOUND="yes"],
	[OPTION_FOUND="no"]
)
if test "x$OPTION_FOUND" = "xno"; then
	AC_MSG_ERROR([SYS_openat2 system call is undefined])
fi

AS_IF([test "x$disable_gui" != xno],
	[AC_MSG_NOTICE([Building without GUI support])],
	[AC_MSG_NOTICE([Building with GUI support])
		AC_DEFINE(HAVE_GUI, [1], [GUI is enabled])
		PKG_CHECK_MODULES([GTK], ["gtk4"],
				  [AC_DEFINE(HAVE_GTK4, [1], [GTK4 support])],
				  [AC_MSG_ERROR([gtk4 not found.])])
		PKG_CHECK_MODULES([LIBADWAITA], ["libadwaita-1"],
			  	  [AC_DEFINE(HAVE_LIBADWAITA, [1], [libadwaita support])],
			  	  [AC_MSG_ERROR([libadwaita not found.])])])
AM_CONDITIONAL([GUI_ENABLED], [test "x$disable_gui" != xyes])

# Check if the zoned fields in struct xfs_fsop_geom actually exist. These were
# all added around the same time, so checking for one (e.g. rtreserved) should
# be enough.
AC_CHECK_MEMBERS([struct xfs_fsop_geom.rtreserved],
		 [AC_DEFINE(HAS_XFS, [1], [XFS support])
		  has_xfs=yes],
		 [AC_MSG_NOTICE([Missing zoned support in xfs_fsop_geom])],
		 [[#include <xfs/xfs.h>]])

AM_CONDITIONAL([HAS_XFS], [test "x$has_xfs" = "xyes"])

AC_CONFIG_FILES([
	Makefile
	src/Makefile
	man/Makefile
])

AC_OUTPUT
