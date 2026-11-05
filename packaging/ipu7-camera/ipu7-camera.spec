Name:           ipu7-camera
Version:        1.0
Release:        1%{?dist}
Summary:        Intel IPU7 hardware-ISP camera support (ISP data, udev and PipeWire config)
# Graph and register templates were recorded from Intel's IPU7 camera stack
License:        LicenseRef-Intel-recorded-data AND MIT
URL:            https://github.com/ah-/ipu7-re
Source0:        %{name}-%{version}.tar.gz
BuildArch:      noarch

Requires:       ipu7-ptl-dkms
# the ipu7 pipeline handler and IPA (built from this repo's packaging/libcamera)
Requires:       libcamera-ipa >= 0.7.2-5.ipu7.1
Requires:       wireplumber

%description
Everything besides the kernel drivers and libcamera that the open Intel IPU7 hardware-ISP
camera stack needs: the processing graph and per-gain register templates for the Sony IMX471
(ThinkPad X1 Carbon Gen 14), a udev rule giving the logged-in user access to the PSYS
device, and a WirePlumber rule hiding the raw Bayer capture nodes. Installing it pulls in the
DKMS drivers and the patched libcamera.

%prep
%autosetup

%build

%install
d=%{buildroot}%{_datadir}/libcamera/ipa/ipu7/imx471
mkdir -p $d
cp -a graph bundles $d/
install -Dpm 644 packaging/ipu7-camera/files/70-ipu7-psys.rules %{buildroot}%{_udevrulesdir}/70-ipu7-psys.rules
install -Dpm 644 packaging/ipu7-camera/files/90-ipu7-hide-raw-nodes.conf \
    %{buildroot}%{_datadir}/wireplumber/wireplumber.conf.d/90-ipu7-hide-raw-nodes.conf

%post
udevadm control --reload >/dev/null 2>&1 || :
udevadm trigger --subsystem-match=intel-ipu7-psys >/dev/null 2>&1 || :

%files
%doc README.md
%dir %{_datadir}/libcamera/ipa/ipu7
%{_datadir}/libcamera/ipa/ipu7/imx471/
%{_udevrulesdir}/70-ipu7-psys.rules
%{_datadir}/wireplumber/wireplumber.conf.d/90-ipu7-hide-raw-nodes.conf

%changelog
* Fri Nov 06 2026 ipu7-re - 1.0-1
- Initial package
