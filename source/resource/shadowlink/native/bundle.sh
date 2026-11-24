#!/usr/bin/env bash
# Build-time only. Run on the target architecture; installation never compiles.
set -euo pipefail
[[ $# == 2 ]] || { echo "Usage: $0 BUILD_DIR NEW_OUTPUT_DIR" >&2; exit 2; }
build_dir="$(realpath "$1")"
output_dir="$(realpath -m "$2")"
source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
[[ ! -e "$output_dir" ]] || { echo "Refusing to overwrite $output_dir" >&2; exit 1; }
[[ -x "$build_dir/shadowlink-server" ]] || { echo "Build shadowlink-server first" >&2; exit 1; }
loader=ld-linux-aarch64.so.1
mkdir -p "$output_dir/bin" "$output_dir/lib" "$output_dir/licenses"
install -m 0755 "$build_dir/shadowlink-server" "$output_dir/bin/shadowlink-server"
aarch64-linux-gnu-strip --strip-unneeded "$output_dir/bin/shadowlink-server"
install -m 0755 "$source_dir/../python-shadowlink-bluez/btmgmt-disable-pairing.sh" "$output_dir/btmgmt-disable-pairing.sh"
source "$source_dir/bundle-libraries.sh"
: > "$output_dir/PACKAGES"
bundle_libraries "$build_dir/shadowlink-server"
aarch64-linux-gnu-strip --strip-unneeded "$output_dir/lib/libshadowlink_core.so"
sort -u -o "$output_dir/PACKAGES" "$output_dir/PACKAGES"
[[ -f "$output_dir/lib/$loader" ]] || { echo 'ELF loader missing' >&2; exit 1; }
cat > "$output_dir/shadowlink-server" <<EOF2
#!/bin/sh
set -eu
bundle_dir=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
exec "\$bundle_dir/lib/$loader" --library-path "\$bundle_dir/lib" "\$bundle_dir/bin/shadowlink-server" "\$@"
EOF2
chmod 0755 "$output_dir/shadowlink-server"
{
    echo 'architecture=aarch64'
    echo "built_utc=$(date -u +%FT%TZ)"
    echo "compiler=$(aarch64-linux-gnu-gcc -dumpfullversion)"
    echo "openssl=$(openssl version)"
    echo 'runtime=shadowlink-native'
} > "$output_dir/BUILD-INFO"
(cd "$source_dir" && find src CMakeLists.txt bundle.sh bundle-libraries.sh ../../../script/build-native-worker.sh ../../../script/native-builder ../../../script/bundle-native-libraries.py ../python-shadowlink-bluez/btmgmt-disable-pairing.sh -type f -print0 | sort -z | xargs -0 sha256sum) > "$output_dir/SOURCE-SHA256SUMS"
(cd "$output_dir" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
check_bundle_binary shadowlink-server
printf 'Native Shadowlink bundle: %s\n' "$output_dir"
