#!/usr/bin/env bash
# Run on the target architecture. This is a core-validation bundle, NOT the
# production BLE daemon; no installer/service switch is performed here.
set -euo pipefail

if [[ $# != 2 ]]; then
    echo "Usage: $0 BUILD_DIR NEW_OUTPUT_DIR" >&2
    exit 2
fi
build_dir="$(realpath "$1")"
output_dir="$(realpath -m "$2")"
[[ ! -e "$output_dir" ]] || { echo "Output already exists: $output_dir" >&2; exit 1; }
binary="$build_dir/shadowlink-core-test"
[[ -x "$binary" ]] || { echo "Build shadowlink-core-test first" >&2; exit 1; }
case "$(uname -m)" in
    aarch64) loader=ld-linux-aarch64.so.1 ;;
    x86_64) loader=ld-linux-x86-64.so.2 ;;
    *) echo "Unsupported build architecture" >&2; exit 1 ;;
esac
mkdir -p "$output_dir/bin" "$output_dir/lib" "$output_dir/licenses"
install -m 0755 "$binary" "$output_dir/bin/shadowlink-core-test"

# ldd is used only on our freshly built, trusted executable. It returns the
# transitive ELF dependency closure, including libc and the matching loader.
dependencies="$(ldd "$binary")"
if [[ "$dependencies" == *"not found"* ]]; then
    echo "$dependencies" >&2
    exit 1
fi
while IFS= read -r library; do
    [[ -f "$library" ]] || { echo "Missing dependency: $library" >&2; exit 1; }
    install -m 0755 "$(realpath "$library")" "$output_dir/lib/$(basename "$library")"
done < <(awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' <<< "$dependencies" | sort -u)
[[ -f "$output_dir/lib/$loader" ]] || { echo "Missing ELF loader" >&2; exit 1; }

cat > "$output_dir/run-core-test" <<EOF
#!/bin/sh
set -eu
bundle_dir=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
exec "\$bundle_dir/lib/$loader" --library-path "\$bundle_dir/lib" "\$bundle_dir/bin/shadowlink-core-test" "\$@"
EOF
chmod 0755 "$output_dir/run-core-test"
for package in libc6 libssl3 libssl3t64 zlib1g libzstd1; do
    if [[ -f "/usr/share/doc/$package/copyright" ]]; then
        cp "/usr/share/doc/$package/copyright" "$output_dir/licenses/$package.copyright"
    fi
done
{
    echo 'purpose=shadowlink-core-validation-only'
    echo "architecture=$(uname -m)"
    echo "built_utc=$(date -u +%FT%TZ)"
    echo "compiler=$(cc -dumpfullversion)"
    echo "openssl=$(openssl version)"
    echo 'production_ble_daemon=false'
} > "$output_dir/BUILD-INFO"
(cd "$output_dir" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
"$output_dir/run-core-test"
echo "Validation bundle: $output_dir"
