#!/usr/bin/env bash
# Read ARM64 ELF metadata on the build PC; never use the host's ldd.
bundle_libraries() {
    python3 "$source_dir/../../../script/bundle-native-libraries.py" "$1" "$output_dir" "${@:2}"
}
check_bundle_binary() {
    local binary="$1" resolved
    resolved="$(qemu-aarch64 "$output_dir/lib/ld-linux-aarch64.so.1" --inhibit-cache --library-path "$output_dir/lib" --list "$output_dir/bin/$binary")"
    if awk '/=> \// {print $3}' <<< "$resolved" | grep -v "^$output_dir/lib/"; then
        echo "Dependency escaped runtime bundle: $binary" >&2; return 1
    fi
    [[ "$binary" == yunet_tracker ]] || qemu-aarch64 "$output_dir/lib/ld-linux-aarch64.so.1" --library-path "$output_dir/lib" "$output_dir/bin/$binary" --version
}
