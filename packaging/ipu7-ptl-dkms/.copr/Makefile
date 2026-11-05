# COPR "make_srpm" hook (package subdirectory packaging/ipu7-ptl-dkms)
outdir ?= $(CURDIR)/dist

srpm:
	dnf -y install rpm-build rpmdevtools
	spectool -g -C . ipu7-ptl-dkms.spec
	rpmbuild -bs --define "_sourcedir $(CURDIR)" --define "_srcrpmdir $(outdir)" ipu7-ptl-dkms.spec
