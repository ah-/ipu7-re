#!/bin/bash
# Builds a libcamera source RPM: Fedora's own dist-git package (pinned in fedora-commit) plus the
# ipu7 patches in patches/, with ".ipu7.N" appended to Fedora's Release so it sorts just above
# the Fedora build it is based on. When Fedora ships a newer libcamera, Fedora's build wins and
# the camera falls back to libcamera's software ISP until this package is rebuilt.
#
#   packaging/libcamera/make-srpm.sh [OUTDIR]     # used by COPR via .copr/Makefile
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=$(realpath -m "${1:-$here/dist}")
commit=$(cat "$here/fedora-commit")
ipu7rel=$(cat "$here/ipu7-release")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git init -q "$work/pkg"
cd "$work/pkg"
git fetch -q --depth 1 https://src.fedoraproject.org/rpms/libcamera.git "$commit"
git checkout -q FETCH_HEAD

# Upstream tarball from Fedora's lookaside cache (only the one the spec uses)
version=$(rpmspec -q --srpm --qf '%{version}' libcamera.spec)
tarball="libcamera-v$version.tar.bz2"
hash=$(sed -n "s/^SHA512 ($tarball) = //p" sources)
[ -n "$hash" ] || { echo "no $tarball in Fedora's sources file" >&2; exit 1; }
curl -fsSL -o "$tarball" "https://src.fedoraproject.org/repo/pkgs/rpms/libcamera/$tarball/sha512/$hash/$tarball"
echo "$hash  $tarball" | sha512sum -c --quiet

# Add our patches after Fedora's (applied by %autosetup -p1)
n=90
lines=""
for p in "$here"/patches/*.patch; do
    cp "$p" .
    lines+="Patch$n: $(basename "$p")\n"
    n=$((n + 1))
done
last=$(grep -n '^Patch[0-9]*:' libcamera.spec | tail -1 | cut -d: -f1)
[ -n "$last" ] || last=$(grep -n '^Source[0-9]*:' libcamera.spec | tail -1 | cut -d: -f1)
sed -i "${last}a # Intel IPU7 hardware ISP: ipu7 pipeline handler + IPA (https://github.com/ah-/ipu7-re)\n${lines%\\n}" libcamera.spec
grep -q '^%autosetup -p1' libcamera.spec || { echo "spec no longer uses %autosetup -p1" >&2; exit 1; }

# Release: 5%{?dist} -> 5.ipu7.N%{?dist}
grep -qE '^Release:\s*[0-9.]+%\{\?dist\}' libcamera.spec || { echo "unexpected Release: line" >&2; exit 1; }
sed -i -E "s/^(Release:\s*)([0-9.]+)%\{\?dist\}/\1\2.ipu7.$ipu7rel%{?dist}/" libcamera.spec
rel=$(rpmspec -q --srpm --qf '%{release}' --define 'dist %{nil}' libcamera.spec)
if grep -q '^%changelog' libcamera.spec && ! grep -q '^%autochangelog' libcamera.spec; then
    sed -i "/^%changelog/a * $(LC_ALL=C date -u '+%a %b %d %Y') ipu7-re - $version-$rel\n- Add the ipu7 pipeline handler and IPA for the Intel IPU7 hardware ISP\n" libcamera.spec
fi

mkdir -p "$out"
rpmbuild -bs --define "_sourcedir $PWD" --define "_srcrpmdir $out" libcamera.spec
