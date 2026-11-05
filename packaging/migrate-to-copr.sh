#!/bin/bash
# Replace a hand-installed IPU7 stack (old install-system.sh) by the COPR packages. Run with sudo.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

echo "== remove the hand-installed pieces"
dnf versionlock delete $(dnf versionlock list 2>/dev/null | sed -n 's/^Package name: //p' | grep -E '^(libcamera|python3-libcamera)') 2>/dev/null || :
if dkms status -m ipu7-ptl -v 1.0 2>/dev/null | grep -q . && ! rpm -q ipu7-ptl-dkms >/dev/null; then
    dkms remove -m ipu7-ptl -v 1.0 --all || :
    rm -rf /usr/src/ipu7-ptl-1.0
fi
rpm -qf /usr/share/libcamera/ipa/ipu7/imx471 >/dev/null 2>&1 || rm -rf /usr/share/libcamera/ipa/ipu7/imx471
rm -f /etc/depmod.d/ipu7-ptl.conf /etc/dracut.conf.d/ipu7-ptl.conf /etc/udev/rules.d/70-ipu7-psys.rules

echo "== install from COPR"
dnf -y copr enable ahahah/ipu7-camera
# the COPR libcamera (release N.ipu7.2+) replaces the local N.ipu7.1 build
dnf -y --refresh install ipu7-camera
dnf -y --refresh upgrade 'libcamera*' 'python3-libcamera'
rpm -q libcamera libcamera-ipa ipu7-ptl-dkms ipu7-camera
dkms status -m ipu7-ptl

echo
echo "Done. Remove ~/.config/wireplumber/wireplumber.conf.d/90-ipu7-hide-raw-nodes.conf (now shipped"
echo "system-wide by ipu7-camera) and reboot."
