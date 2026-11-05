#!/usr/bin/env bash
# Runs once when the dev container is created.
set -euo pipefail

# Fetch the pinned Espressif32 platform, toolchains and ESP-IDF now, so the
# first build doesn't. Cached in the solaros-platformio volume.
pio pkg install -e solar_term

echo
echo "SolarOS dev container ready:"
echo "  pio run -e solar_term      build firmware"
echo "  solar-test                 host unit tests (solar-test backlight: one module)"
echo "  solar-qemu ENV             boot a build in QEMU (UART0 console builds only)"
echo "  claude                     Claude Code (run 'claude' once to log in)"
