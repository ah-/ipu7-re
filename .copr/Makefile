# COPR "make_srpm" entry point. COPR runs this file from the package's subdirectory
# (packaging/<package>), so hand over to that package's own .copr/Makefile.
outdir ?= $(CURDIR)/dist

srpm:
	$(MAKE) -f $(CURDIR)/.copr/Makefile srpm outdir=$(outdir)
