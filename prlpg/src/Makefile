#-------------------------------------------------------------------------
#
# Makefile for src - my own change
#
# Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/Makefile,v 1.50 2010/01/20 09:16:23 heikki Exp $
#
#-------------------------------------------------------------------------

subdir = src
top_builddir = ..
include Makefile.global


all install installdirs uninstall distprep:
	$(MAKE) -C port $@
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/utils/mb/conversion_procs $@
	$(MAKE) -C backend/snowball $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C backend/replication/libpqwalreceiver $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@
	$(MAKE) -C makefiles $@
	$(MAKE) -C test/regress $@

install: install-local

install-local: installdirs-local
	$(INSTALL_DATA) Makefile.global '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.global'
	$(INSTALL_DATA) Makefile.port '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.port'
	$(INSTALL_DATA) $(srcdir)/Makefile.shlib '$(DESTDIR)$(pgxsdir)/$(subdir)/Makefile.shlib'
	$(INSTALL_DATA) $(srcdir)/nls-global.mk '$(DESTDIR)$(pgxsdir)/$(subdir)/nls-global.mk'

installdirs: installdirs-local

installdirs-local:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

uninstall: uninstall-local

uninstall-local:
	rm -f $(addprefix '$(DESTDIR)$(pgxsdir)/$(subdir)'/, Makefile.global Makefile.port Makefile.shlib nls-global.mk)

clean:
	$(MAKE) -C port $@
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/snowball $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C backend/replication/libpqwalreceiver $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@
	$(MAKE) -C makefiles $@
	$(MAKE) -C test $@
	$(MAKE) -C tutorial NO_PGXS=1 $@
	$(MAKE) -C test/thread $@

distclean maintainer-clean:
	$(MAKE) -C port $@
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/snowball $@
	$(MAKE) -C include $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C backend/replication/libpqwalreceiver $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@
	$(MAKE) -C makefiles $@
	$(MAKE) -C test $@
	$(MAKE) -C tutorial NO_PGXS=1 $@
	$(MAKE) -C test/thread $@
	rm -f Makefile.port Makefile.global

coverage:
	$(MAKE) -C timezone $@
	$(MAKE) -C backend $@
	$(MAKE) -C backend/utils/mb/conversion_procs $@
	$(MAKE) -C backend/snowball $@
	$(MAKE) -C interfaces $@
	$(MAKE) -C backend/replication/libpqwalreceiver $@
	$(MAKE) -C bin $@
	$(MAKE) -C pl $@


.PHONY: install-local installdirs-local uninstall-local
