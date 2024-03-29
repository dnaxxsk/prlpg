#
# PostgreSQL top level makefile
#
# $PostgreSQL: pgsql/GNUmakefile.in,v 1.57 2010/01/29 01:06:18 adunstan Exp $
#

subdir =
top_builddir = .
include $(top_builddir)/src/Makefile.global

all:
	$(MAKE) -C doc all
	$(MAKE) -C src all
	$(MAKE) -C config all
	@echo "All of PostgreSQL successfully made. Ready to install."

world:
	$(MAKE) -C doc html
	$(MAKE) -C src all
	$(MAKE) -C config all
	$(MAKE) -C contrib all
	@echo "PostgreSQL, contrib and HTML documentation successfully made. Ready to install."

html man:
	$(MAKE) -C doc $@

install:
	$(MAKE) -C doc $@
	$(MAKE) -C src $@
	$(MAKE) -C config $@
	@echo "PostgreSQL installation complete."

install-world:
	$(MAKE) -C doc install
	$(MAKE) -C src install
	$(MAKE) -C config install
	$(MAKE) -C contrib install
	@echo "PostgreSQL and contrib installation complete."

installdirs uninstall coverage:
	$(MAKE) -C doc $@
	$(MAKE) -C src $@
	$(MAKE) -C config $@

distprep:
	$(MAKE) -C doc $@
	$(MAKE) -C src $@
	$(MAKE) -C config $@
	$(MAKE) -C contrib $@

# clean, distclean, etc should apply to contrib too, even though
# it's not built by default
clean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C src $@
	$(MAKE) -C config $@
# Garbage from autoconf:
	@rm -rf autom4te.cache/

# Important: distclean `src' last, otherwise Makefile.global
# will be gone too soon.
distclean maintainer-clean:
	$(MAKE) -C doc $@
	$(MAKE) -C contrib $@
	$(MAKE) -C config $@
	$(MAKE) -C src $@
	rm -f config.cache config.log config.status GNUmakefile
# Garbage from autoconf:
	@rm -rf autom4te.cache/

check: all

check installcheck installcheck-parallel:
	$(MAKE) -C src/test $@

installcheck-world:
	$(MAKE) -C src/test installcheck
	$(MAKE) -C src/pl installcheck
	$(MAKE) -C src/interfaces/ecpg installcheck
	$(MAKE) -C contrib installcheck

GNUmakefile: GNUmakefile.in $(top_builddir)/config.status
	./config.status $@


##########################################################################

distdir	= postgresql-$(VERSION)
dummy	= =install=
garbage = =*  "#"*  ."#"*  *~*  *.orig  *.rej  core  postgresql-*

dist: $(distdir).tar.gz $(distdir).tar.bz2
	rm -rf $(distdir)

$(distdir).tar: distdir
	$(TAR) chf $@ $(distdir)

.INTERMEDIATE: $(distdir).tar

distdir-location:
	@echo $(distdir)

distdir:
	rm -rf $(distdir)* $(dummy)
	for x in `cd $(top_srcdir) && find . \( -name CVS -prune \) -o \( -name .git -prune \) -o -print`; do \
	  file=`expr X$$x : 'X\./\(.*\)'`; \
	  if test -d "$(top_srcdir)/$$file" ; then \
	    mkdir "$(distdir)/$$file" && chmod 777 "$(distdir)/$$file";	\
	  else \
	    ln "$(top_srcdir)/$$file" "$(distdir)/$$file" >/dev/null 2>&1 \
	      || cp "$(top_srcdir)/$$file" "$(distdir)/$$file"; \
	  fi || exit; \
	done
	$(MAKE) -C $(distdir) distprep
	$(MAKE) -C $(distdir)/doc/src/sgml/ HISTORY INSTALL regress_README
	cp $(distdir)/doc/src/sgml/HISTORY $(distdir)/
	cp $(distdir)/doc/src/sgml/INSTALL $(distdir)/
	cp $(distdir)/doc/src/sgml/regress_README $(distdir)/src/test/regress/README
	$(MAKE) -C $(distdir) distclean
	rm -f $(distdir)/README.CVS

distcheck: dist
	rm -rf $(dummy)
	mkdir $(dummy)
	$(GZIP) -d -c $(distdir).tar.gz | $(TAR) xf -
	install_prefix=`cd $(dummy) && pwd`; \
	cd $(distdir) \
	&& ./configure --prefix="$$install_prefix"
	$(MAKE) -C $(distdir) -q distprep
	$(MAKE) -C $(distdir)
	$(MAKE) -C $(distdir) install
	$(MAKE) -C $(distdir) uninstall
	@echo "checking whether \`$(MAKE) uninstall' works"
	test `find $(dummy) ! -type d | wc -l` -eq 0
	$(MAKE) -C $(distdir) dist
# Room for improvement: Check here whether this distribution tarball
# is sufficiently similar to the original one.
	rm -rf $(distdir) $(dummy)
	@echo "Distribution integrity checks out."

.PHONY: dist distdir distcheck
unexport split-dist
