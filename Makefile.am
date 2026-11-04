# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPDX-FileCopyrightText: 2026 Western Digital Corporation or its affiliates.

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = src man

EXTRA_DIST = \
	autogen.sh \
	README.md \
	data/org.wdc.zonar.policy.in

# Polkit policy for running zonar_srv through pkexec from the GUI. Only the
# GUI uses pkexec, so only install the policy when the GUI is enabled. The
# installation directory polkitactiondir is set by configure.
if GUI_ENABLED
polkitaction_DATA = data/org.wdc.zonar.policy

# Generate the policy from its template. The policy exec.path annotation must
# be the literal absolute path of the installed zonar_srv.
data/org.wdc.zonar.policy: $(srcdir)/data/org.wdc.zonar.policy.in Makefile
	$(AM_V_GEN)$(MKDIR_P) data && \
	sed -e 's|@bindir[@]|$(bindir)|g' \
		$(srcdir)/data/org.wdc.zonar.policy.in > $@
endif

install-srv:
	@echo "Installing zonar server..."
	$(MAKE) -C src zonar_srv
	$(MKDIR_P) "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)/man8"
	$(INSTALL_PROGRAM) $(top_builddir)/src/zonar_srv "$(DESTDIR)$(bindir)"
	$(INSTALL_DATA) $(top_srcdir)/man/zonar_srv.8 "$(DESTDIR)$(mandir)/man8"

CLEANFILES = *.rpm *.tar.gz zonar zonar_srv data/org.wdc.zonar.policy
DISTCLEANFILES = ${CLEANFILES} configure *~ *.*~ src/*.*~
