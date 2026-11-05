# Intel's out-of-tree IPU7 drivers (ISYS + PSYS + ACPI) for Panther Lake, built by DKMS
%global commit 4d85ac16db16b8e6382602ec4e3b4df9a2ab2da6
%global shortcommit %(c=%{commit}; echo ${c:0:7})
%global dkms_name ipu7-ptl
%global dkms_ver 1.0

Name:           ipu7-ptl-dkms
Version:        1.0
Release:        1.git%{shortcommit}%{?dist}
Summary:        Intel IPU7 camera drivers with the hardware ISP (PSYS), via DKMS
License:        GPL-2.0-only
URL:            https://github.com/ah-/ipu7-drivers
Source0:        %{url}/archive/%{commit}/ipu7-drivers-%{shortcommit}.tar.gz
Source1:        ipu7-ptl-depmod.conf
Source2:        ipu7-ptl-dracut.conf
BuildArch:      noarch

Requires:       dkms
Requires:       kernel-devel-matched
Requires:       gcc
Requires:       make
Requires(post): dkms
Requires(preun): dkms

%description
Intel's out-of-tree IPU7 kernel drivers (intel-ipu7, intel-ipu7-isys, intel-ipu7-psys and the
IPU ACPI helpers), packaged for DKMS so they are rebuilt for every new kernel. Unlike the
in-kernel staging driver they include the processing system (PSYS), which runs the hardware
ISP used by libcamera's ipu7 pipeline handler. If a build fails on a new kernel, the in-kernel
drivers load instead and libcamera uses its software ISP.

%prep
%autosetup -n ipu7-drivers-%{commit}

%build

%install
mkdir -p %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_ver}
cp -a . %{buildroot}%{_usrsrc}/%{dkms_name}-%{dkms_ver}/
install -Dpm 644 %{SOURCE1} %{buildroot}%{_prefix}/lib/depmod.d/ipu7-ptl.conf
install -Dpm 644 %{SOURCE2} %{buildroot}%{_prefix}/lib/dracut/dracut.conf.d/90-ipu7-ptl.conf

%post
dkms add -m %{dkms_name} -v %{dkms_ver} -q --rpm_safe_upgrade || :
# build for the running kernel now; newer kernels are handled by DKMS autoinstall
dkms build -m %{dkms_name} -v %{dkms_ver} -k "$(uname -r)" -q || :
dkms install -m %{dkms_name} -v %{dkms_ver} -k "$(uname -r)" -q --force || :

%preun
dkms remove -m %{dkms_name} -v %{dkms_ver} -q --all --rpm_safe_upgrade || :

%posttrans
# keep the in-kernel IPU7 modules out of the running kernel's initramfs
dracut -f --kver "$(uname -r)" >/dev/null 2>&1 || :

%files
%license LICENSE
%doc README.md
%{_usrsrc}/%{dkms_name}-%{dkms_ver}
%{_prefix}/lib/depmod.d/ipu7-ptl.conf
%{_prefix}/lib/dracut/dracut.conf.d/90-ipu7-ptl.conf

%changelog
* Fri Nov 06 2026 ipu7-re - 1.0-1.git4d85ac1
- Initial package: Intel ipu7-drivers 484d713 + Fedora/PTL DKMS config
