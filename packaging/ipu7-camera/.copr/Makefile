# COPR "make_srpm" hook (package subdirectory packaging/ipu7-camera)
outdir ?= $(CURDIR)/dist
top := $(shell git rev-parse --show-toplevel)
ver := $(shell rpmspec -q --srpm --qf '%{version}' ipu7-camera.spec 2>/dev/null)

srpm:
	dnf -y install git rpm-build
	git config --global --add safe.directory '*'
	git -C $(top) archive --prefix=ipu7-camera-$(ver)/ -o $(CURDIR)/ipu7-camera-$(ver).tar.gz \
	    HEAD README.md graph bundles packaging/ipu7-camera/files
	rpmbuild -bs --define "_sourcedir $(CURDIR)" --define "_srcrpmdir $(outdir)" ipu7-camera.spec
