#!/usr/bin/env bash
# Internal ARM64 worker, always run against an isolated source snapshot.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
toolchain="$root/script/native-builder/aarch64.cmake"
jobs="$1"; run_tests="$2"; shift 2
components=("$@")
[[ ${#components[@]} -gt 0 && "$run_tests" =~ ^(true|false)$ ]] || exit 2
needs() { [[ " ${components[*]} " == *" $1 "* ]]; }
build_targets() {
    local directory="$1"; shift
    if $run_tests; then
        cmake --build "$directory" -j "$jobs"
    else
        cmake --build "$directory" -j "$jobs" --target "$@"
    fi
}
export SHADOWLINK_TEST_EMULATOR=qemu-aarch64
output="$root/.native-output"
mkdir "$output"
if needs others; then
    cmake -S "$root/resource/others/native" -B "$output/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    build_targets "$output/build" privacy-button-state fan-daemon fanctl light-sensor desk-shaking-state factory-reset-button factory-reset-dialog face-tracker-service stepper-position-service robotarm shadowlink-server
    $run_tests && ctest --test-dir "$output/build" --output-on-failure
    cmake -S "$root/resource/others/face_tracker_service/face-tracker" -B "$output/face" -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    build_targets "$output/face" yunet_tracker
    $run_tests && ctest --test-dir "$output/face" --output-on-failure
    install -m 0755 "$output/face/yunet_tracker" "$output/build/yunet_tracker"
    bash "$root/resource/others/native/bundle.sh" "$output/build" "$output/others-runtime"
fi
if needs shadowlink; then
    if ! needs others; then
        cmake -S "$root/resource/shadowlink/native" -B "$output/build/shadowlink" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="$toolchain"
        build_targets "$output/build/shadowlink" shadowlink-server
        $run_tests && ctest --test-dir "$output/build/shadowlink" --output-on-failure
    fi
    bash "$root/resource/shadowlink/native/bundle.sh" "$output/build/shadowlink" "$output/shadowlink-runtime"
fi
if needs software-update; then
    cmake -S "$root/resource/software-update/native" -B "$output/update" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    build_targets "$output/update" software-update-service software-updatectl
    $run_tests && ctest --test-dir "$output/update" --output-on-failure
    bash "$root/resource/software-update/native/bundle.sh" "$output/update" "$output/software-update-runtime"
    if $run_tests; then
        # Exercise the final ARM64 bundle on the PC, with simulated install/recovery commands.
        mkdir "$output/update-test-bin"
        for component in software-update-service software-updatectl; do
            printf '#!/bin/sh\nexec qemu-aarch64 "%s/lib/ld-linux-aarch64.so.1" --library-path "%s/lib" "%s/bin/%s" "$@"\n' \
                "$output/software-update-runtime" "$output/software-update-runtime" "$output/software-update-runtime" "$component" > "$output/update-test-bin/$component"
            chmod 0755 "$output/update-test-bin/$component"
        done
        install -m 0755 "$output/update/libsoftware-update-space-shim.so" "$output/update-test-bin/"
        python3 "$root/resource/software-update/native/test/test_service.py" "$output/update-test-bin"
        printf '#!/bin/sh\nexec qemu-aarch64 -L / "%s/software-update-auto-probe" "$@"\n' \
            "$output/update" > "$output/update-test-bin/software-update-auto-probe"
        chmod 0755 "$output/update-test-bin/software-update-auto-probe"
        python3 "$root/resource/software-update/native/test/test_auto_service.py" "$output/update-test-bin"
    fi
fi
if needs xmos-oww; then
    cmake -S "$root/resource/xmos-oww/native" -B "$output/xmos" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    build_targets "$output/xmos" xmos-oww
    if $run_tests; then
        ctest --test-dir "$output/xmos" --output-on-failure
        XMOS_TEST_RUNNER='qemu-aarch64 -L /' python3 "$root/resource/xmos-oww/native/test/test_runtime.py" \
            "$output/xmos/xmos-oww" "/opt/clawstage-sherpa/models"
    fi
    cmake -S "$root/resource/xmos-oww/native/openwakeword" -B "$output/xmos-openwakeword" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    build_targets "$output/xmos-openwakeword" xmos-oww
    if $run_tests; then
        ctest --test-dir "$output/xmos-openwakeword" --output-on-failure
        XMOS_TEST_RUNNER='qemu-aarch64 -L /' python3 "$root/resource/xmos-oww/native/openwakeword/test/test_runtime.py" \
            "$output/xmos-openwakeword/xmos-oww" "$root/resource/xmos-oww/hey_miko.tflite"
    fi
    install -m 0755 "$output/xmos-openwakeword/xmos-oww" "$output/xmos/xmos-openwakeword"
    bash "$root/resource/xmos-oww/native/bundle.sh" "$output/xmos" "$output/xmos-oww-runtime"
    qemu-aarch64 "$output/xmos-oww-runtime/lib/ld-linux-aarch64.so.1" --library-path "$output/xmos-oww-runtime/lib" \
        "$output/xmos-oww-runtime/bin/xmos-oww" --check-models --model-dir "$output/xmos-oww-runtime/models"
    qemu-aarch64 "$output/xmos-oww-runtime/lib/ld-linux-aarch64.so.1" --library-path "$output/xmos-oww-runtime/lib" \
        "$output/xmos-oww-runtime/bin/xmos-openwakeword" --check-models --model-path "$output/xmos-oww-runtime/models/openwakeword/hey_miko.tflite"
fi
if needs openclaw; then
    bash "$root/resource/openclaw/native/bundle.sh" "$output/openclaw-runtime"
fi
if needs display-shell; then
    mkdir "$output/display"
    CC=aarch64-linux-gnu-gcc STRIP=aarch64-linux-gnu-strip \
        bash "$root/resource/display-shell/build.sh" "$output/display/clawstage_display"
    if $run_tests; then
        CC=aarch64-linux-gnu-gcc DISPLAY_TEST_RUNNER='qemu-aarch64 -L /' \
            GDK_BACKEND=x11 xvfb-run -a bash "$root/test/test_display_visibility.sh" --progress-only
        python3 "$root/test/test_local_ota_websocket.py"
    fi
    bash "$root/resource/display-shell/native/bundle.sh" "$output/display" "$output/display-shell-runtime"
fi
for component in "${components[@]}"; do
    bundle="$output/$component-runtime"
    (cd "$bundle" && sha256sum -c --quiet SHA256SUMS)
    tar -C "$bundle" -czf "$output/$component-runtime-aarch64.tar.gz" .
done
