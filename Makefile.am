
SUBDIRS = src util tests

EXTRA_DIST = VERSION config.site

CLEANFILES = so_locations

ACLOCAL_AMFLAGS = -I config

install-data-local:
	$(mkinstalldirs) $(GMAPDB)

dist-hook:
#	svn log -v --xml | ./svncl.pl > ChangeLog

