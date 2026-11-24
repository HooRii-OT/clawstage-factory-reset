#!/usr/bin/env bash
# Build-host only. Install ARM64 libraries, extract target tools without replacing host tools.
set -euo pipefail
packages=(v4l-utils curl jq util-linux coreutils)
mkdir -p /opt/clawstage-camera /tmp/clawstage-camera-debs
apt-get update
libraries=()
while IFS= read -r library; do libraries+=("${library%%:*}:arm64"); done < <(
    apt-cache depends "${packages[@]/%/:arm64}" \
      | sed -nE 's/^[ |]*(Pre)?Depends: (lib[^ <>]+).*$/\2/p' | sort -u
)
[ "${#libraries[@]}" -gt 0 ] || { echo 'No ARM64 camera libraries resolved' >&2; exit 1; }
# apt resolves and installs the full transitive library closure and copyright files.
# FFmpeg also builds host-side helpers; the ARM64 cross compiler cannot build them.
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    "${libraries[@]}" libx264-dev:arm64 gcc libc6-dev
cd /tmp/clawstage-camera-debs
apt-get download "${packages[@]/%/:arm64}"
for archive in ./*.deb; do
    [ "$(dpkg-deb -f "$archive" Architecture)" = arm64 ]
    dpkg-deb -x "$archive" /opt/clawstage-camera
    dpkg-deb -f "$archive" Package Version | tr '\n' ' ' >> /opt/clawstage-camera/PACKAGES
    printf '\n' >> /opt/clawstage-camera/PACKAGES
done
# Some tool packages link their documentation to an installed library package.
# Preserve that link and copy its copyright into the extracted package tree.
for package in "${packages[@]}"; do
    documentation="/opt/clawstage-camera/usr/share/doc/$package"
    if [[ -L "$documentation" ]]; then
        target="$(readlink "$documentation")"
        [[ "$target" =~ ^[a-z0-9][a-z0-9.+-]*$ ]] || {
            echo "Unexpected documentation link for $package: $target" >&2; exit 1;
        }
        mkdir -p "/opt/clawstage-camera/usr/share/doc/$target"
        cp -L "/usr/share/doc/$target/copyright" "/opt/clawstage-camera/usr/share/doc/$target/copyright"
    fi
    [[ -s "$documentation/copyright" ]] || {
        echo "Missing copyright for $package" >&2; exit 1;
    }
done
sha256sum ./*.deb > /opt/clawstage-camera/DEB-SHA256SUMS
cd /
rm -rf /tmp/clawstage-camera-debs /var/lib/apt/lists/*

bash /opt/clawstage-builder/build-camera-ffmpeg.sh /opt/clawstage-camera
