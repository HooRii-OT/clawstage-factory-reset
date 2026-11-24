# Runs on the Linux PC; only the resulting programs/libraries target ARM64.
FROM debian:trixie-slim
# APT 3.0.3 can flood warnings for stale delayed retries (Debian #1111554).
# Retry immediately instead; keep real download failures fatal.
RUN printf '%s\n' 'Acquire::Retries "3";' 'Acquire::Retries::Delay "false";' \
       'Acquire::http::Timeout "30";' 'Acquire::https::Timeout "30";' \
       > /etc/apt/apt.conf.d/80clawstage-downloads \
    && dpkg --add-architecture arm64 \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates cmake make pkg-config python3 binutils file \
       gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user \
       libssl-dev:arm64 libglib2.0-dev:arm64 libjson-c-dev:arm64 \
       libcurl4-openssl-dev:arm64 libopencv-dev:arm64 \
       openssl tar gzip xz-utils coreutils \
    && rm -rf /var/lib/apt/lists/*
ENV PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
ENV LC_ALL=C
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       curl libsoup-3.0-dev:arm64 libusb-1.0-0:arm64 libsentencepiece-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
# Extract the target executable: installing dfu-util:arm64 would replace the
# build host's Python because the Debian package also contains a Python helper.
RUN apt-get update && mkdir -p /opt/clawstage-dfu /tmp/dfu-package \
    && cd /tmp/dfu-package && apt-get download dfu-util:arm64=0.11-3 \
    && dpkg-deb -x ./*.deb /opt/clawstage-dfu \
    && sha256sum ./*.deb > /opt/clawstage-dfu/DEB-SHA256SUMS \
    && printf 'dfu-util:arm64 0.11-3\n' > /opt/clawstage-dfu/PACKAGE \
    && rm -rf /tmp/dfu-package /var/lib/apt/lists/*
COPY aarch64.cmake /opt/clawstage-toolchain.cmake
COPY prepare-sherpa.py xmos-sources.json /opt/clawstage-builder/
COPY download-cache /opt/clawstage-source-cache/
RUN python3 /opt/clawstage-builder/prepare-sherpa.py
COPY build-tflite.py tflite_compat.py openwakeword-sources.json /opt/clawstage-builder/
RUN python3 /opt/clawstage-builder/build-tflite.py
# Camera tools are built into the release; OTA never runs apt.
COPY prepare-camera-packages.sh build-camera-ffmpeg.sh /opt/clawstage-builder/
RUN bash /opt/clawstage-builder/prepare-camera-packages.sh
# Compile display-shell for the Pi; execute its GTK tests under QEMU + host Xvfb.
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       libgtk-3-dev:arm64 libgtk-layer-shell-dev:arm64 wayland-protocols libwayland-bin \
       xvfb xauth python3-websockets \
    && rm -rf /var/lib/apt/lists/*
