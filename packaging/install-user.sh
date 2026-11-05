#!/bin/bash
# Retire the temporary/old camera setup in the user session (run as the user, not root).
set -uo pipefail
B=~/.config/camera-setup-backup-$(date +%Y%m%d)
mkdir -p "$B"
# old software-ISP tuning: VBLANK/colour service and its wireplumber drop-in
systemctl --user disable --now webcam-settings.service 2>/dev/null
[ -f ~/.config/systemd/user/wireplumber.service.d/60-webcam-tuning.conf ] && mv ~/.config/systemd/user/wireplumber.service.d/60-webcam-tuning.conf "$B/"
[ -f ~/.config/systemd/user/autoframe.service.d/webcam-settings.conf ] && mv ~/.config/systemd/user/autoframe.service.d/webcam-settings.conf "$B/"
# temporary runtime drop-ins from development
rm -f /run/user/$(id -u)/systemd/user/wireplumber.service.d/libcamera-ipu7.conf \
      /run/user/$(id -u)/systemd/user/autoframe.service.d/intel-isp.conf
# keep hiding the raw IPU7 capture nodes from PipeWire (they only give raw Bayer)
W=~/.config/wireplumber/wireplumber.conf.d
[ -f $W/90-intel-isp-no-libcamera.conf ] && mv $W/90-intel-isp-no-libcamera.conf "$B/"
cat > $W/90-ipu7-hide-raw-nodes.conf <<'X'
# The IPU7 ISYS capture nodes deliver raw Bayer only; the camera is provided by libcamera
# (ipu7 hardware-ISP pipeline). Hide the raw V4L2 nodes from PipeWire.
monitor.v4l2.rules = [
  {
    matches = [ { device.bus-path = "pci-0000:00:05.0" } ]
    actions = { update-props = { device.disabled = true } }
  }
]
X
systemctl --user daemon-reload
echo "Old setup backed up to $B. Reboot to start on the installed stack."
