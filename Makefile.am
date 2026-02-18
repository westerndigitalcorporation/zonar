# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src man

EXTRA_DIST = \
	autogen.sh \
	README.md

install-srv:
	@echo "Installing zonar server..."
	install -m 755 $(top_srcdir)/src/zonar_srv $(DESTDIR)${bindir}
	install -m 644 $(top_srcdir)/man/zonar_srv.8 $(DESTDIR)${mandir}/man8

CLEANFILES = *.rpm *.tar.gz zonar zonar_srv
DISTCLEANFILES = ${CLEANFILES} configure *~ *.*~ src/*.*~
