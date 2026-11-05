# COPR "make_srpm" hook (package subdirectory packaging/libcamera)
outdir ?= $(CURDIR)/dist

srpm:
	dnf -y install git rpm-build curl
	git config --global --add safe.directory '*'
	./make-srpm.sh $(outdir)
