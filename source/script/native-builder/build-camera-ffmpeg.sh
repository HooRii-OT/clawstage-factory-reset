#!/usr/bin/env bash
# Builder/test-host only. Minimal FFmpeg for USB camera JPEG and H.264 recording.
set -euo pipefail
[[ $# == 1 ]] || { echo 'Usage: build-camera-ffmpeg.sh PACKAGE_ROOT' >&2; exit 2; }
package_root="$(realpath -m "$1")"
version=7.1.2
sha256=089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
curl --fail --location --retry 2 --retry-all-errors --connect-timeout 15 --max-time 180 \
    "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$work/source.tar.xz"
printf '%s  %s\n' "$sha256" "$work/source.tar.xz" | sha256sum -c -
tar -xf "$work/source.tar.xz" -C "$work"
cd "$work/ffmpeg-$version"
cross=()
if [ "$(uname -m)" != aarch64 ]; then
    cross=(--enable-cross-compile --cross-prefix=aarch64-linux-gnu-)
fi
./configure --arch=aarch64 --target-os=linux "${cross[@]}" \
    --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug \
    --disable-shared --enable-static --disable-ffplay --disable-ffprobe --enable-ffmpeg \
    --enable-gpl --enable-libx264 --enable-protocol=file,pipe \
    --enable-indev=v4l2,lavfi --enable-demuxer=mjpeg,image2 \
    --enable-muxer=image2,image2pipe,mp4,null \
    --enable-decoder=mjpeg,rawvideo,wrapped_avframe --enable-encoder=mjpeg,libx264,wrapped_avframe \
    --enable-parser=mjpeg,h264 --enable-bsf=extract_extradata,h264_mp4toannexb \
    --enable-filter=color,null,format,scale,trim,setpts,buffer,buffersink
make -j "${FFMPEG_BUILD_JOBS:-2}" ffmpeg
mkdir -p "$package_root/usr/bin" "$package_root/usr/share/doc/ffmpeg"
install -m 0755 ffmpeg "$package_root/usr/bin/ffmpeg"
cp COPYING.GPLv2 "$package_root/usr/share/doc/ffmpeg/copyright"
printf 'ffmpeg-source %s sha256=%s\n' "$version" "$sha256" >> "$package_root/PACKAGES"
printf '%s  ffmpeg-%s.tar.xz\n' "$sha256" "$version" > "$package_root/FFMPEG-SOURCE-SHA256SUMS"
# Preserve the exact configuration so its enabled capabilities can be audited.
cp ffbuild/config.mak "$package_root/FFMPEG-CONFIG.mak"
