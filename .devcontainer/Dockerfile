# SolarOS development: PlatformIO (pioarduino ESP-IDF platform), host test
# toolchain, and Espressif's QEMU. Multi-arch (amd64 and arm64).
FROM mcr.microsoft.com/devcontainers/python:3.12-bookworm

ARG TARGETARCH
ARG PLATFORMIO_VERSION=6.2.0
ARG ESPTOOL_VERSION=5.6.0
ARG QEMU_TAG=esp-develop-9.2.2-20260417
ARG QEMU_VER=esp_develop_9.2.2_20260417

# Host tests compile C/C++ with cc/c++ and cmake. The QEMU libs are what
# Espressif's prebuilt qemu-system-* binaries link against.
# cc/c++ is a wrapper (see cc-wrapper): GCC, except AddressSanitizer builds
# use clang 14, the only ASan here that starts on arm64 kernels with 39-bit
# virtual addresses (GCC 12/14 and clang 15+ assume 48-bit). Firmware builds
# use Espressif's own toolchains, unaffected.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
        build-essential clang libclang-rt-14-dev liblz4-1 cmake ninja-build ccache git curl xz-utils jq \
        libusb-1.0-0 udev \
        libglib2.0-0 libpixman-1-0 libsdl2-2.0-0 libslirp0 libgcrypt20 \
    && rm -rf /var/lib/apt/lists/*
COPY strl.c /tmp/strl.c
RUN gcc -O2 -fPIC -c /tmp/strl.c -o /tmp/strl.o \
    && ar rcs /usr/local/lib/libstrl.a /tmp/strl.o && rm /tmp/strl.*
COPY --chmod=0755 cc-wrapper /usr/local/bin/cc
RUN ln -s cc /usr/local/bin/c++

# Espressif QEMU (xtensa: ESP32/ESP32-S3; riscv32: ESP32-C3), checksummed.
RUN set -eux; \
    case "$TARGETARCH" in \
      amd64) host=x86_64-linux-gnu; \
             sum_x=0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35; \
             sum_r=547f03e04701a92cbb699f7f7d015adc1f5b5ef93cbb94c0dd9b7107e2d84e77 ;; \
      arm64) host=aarch64-linux-gnu; \
             sum_x=00de5985094c14e47d1b38464a006b8ed64fd0fa7a289c56da24f3a329f65339; \
             sum_r=a9f7b98636008edcf7a11c96f10b3a3ec83c2a890fc54c3e3ceb3ec9edace427 ;; \
      *) echo "unsupported arch $TARGETARCH"; exit 1 ;; \
    esac; \
    base="https://github.com/espressif/qemu/releases/download/${QEMU_TAG}"; \
    for pair in "xtensa:$sum_x" "riscv32:$sum_r"; do \
      arch="${pair%%:*}"; sum="${pair#*:}"; \
      f="qemu-${arch}-softmmu-${QEMU_VER}-${host}.tar.xz"; \
      curl -fsSL -o "/tmp/$f" "$base/$f"; \
      echo "$sum  /tmp/$f" | sha256sum -c -; \
      mkdir -p "/opt/qemu-$arch"; \
      tar -xJf "/tmp/$f" -C "/opt/qemu-$arch" --strip-components=1; \
      rm "/tmp/$f"; \
    done
ENV PATH="/opt/qemu-xtensa/bin:/opt/qemu-riscv32/bin:${PATH}"

# PlatformIO Core and the Python modules the build scripts and tests import.
RUN pip install --no-cache-dir \
        "platformio==${PLATFORMIO_VERSION}" \
        "esptool==${ESPTOOL_VERSION}" \
        pyserial pillow cryptography lz4

# ~/.platformio (toolchains, ESP-IDF), ~/.cache and ~/.claude are named
# volumes (devcontainer.json) so they survive rebuilds. A new volume copies
# the ownership of the directory it mounts over, so create them as vscode.
USER vscode
RUN mkdir -p /home/vscode/.platformio /home/vscode/.claude /home/vscode/.cache

# Claude Code (native install, ~/.local/bin). Its login and settings live in
# CLAUDE_CONFIG_DIR, a named volume, so they survive rebuilds.
RUN curl -fsSL https://claude.ai/install.sh | bash
ENV PATH="/home/vscode/.local/bin:${PATH}" \
    CLAUDE_CONFIG_DIR=/home/vscode/.claude
