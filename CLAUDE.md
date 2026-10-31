# SolarOS

ESP32 operating environment (shell, apps, jobs, services, Python/Lua
scripting). ESP-IDF 5.5.5 through PlatformIO (pinned pioarduino platform in
`platformio.ini`). README.md covers targets and flavors; the user manual is
`doc/manual/` (canonical; device help and the website are generated from it).

This checkout is a fork (`origin`); upstream is `nilseuropa/solar_os`.
Upstream PRs go against the `beta` branch, one feature each, and board or
driver changes need hardware-in-the-loop evidence (see README "Contributing").

## Environment

Work happens in the dev container (`.devcontainer/`). Everything below is
installed there. There is no serial device in the container by default, and
no board attached: you can build and run host tests, but not
flash or run HIL tests unless the user passes a device through.

QEMU (Espressif's fork, `qemu-system-xtensa`/`-riscv32`) is installed and
`solar-qemu ENV` boots a build's flash image, but the shipped targets log to
the ESP32-S3 USB-Serial-JTAG console, which QEMU doesn't emulate: the
bootloader runs with no visible output. A QEMU boot needs a build with the
console on UART0. Board peripherals (display, keyboard, radios, SD) are not
emulated either.

## Commands

```sh
pio run -e solar_term                     # build one target (default env)
SOLAR_OS_FLAVOR=writerdeck pio run -e solar_term   # flavor override (full, classic, writerdeck)
solar-test                                # all host unit tests, in parallel
solar-test backlight ble_                 # modules starting with these names
python3 -m unittest test_backlight        # one module directly (PYTHONPATH is set)
solar-qemu ENV                            # boot ENV's last build in QEMU (UART0 console only)
```

- Firmware builds are slow and one checkout serializes them (a POSIX lock).
  Build only the target you are changing; don't loop over every env.
- Firmware builds take ~20 min cold and several minutes incremental on a
  slow arm64 host; the toolchains are cached in a named volume.
- Host tests (`tests/test_*.py`) compile production C from `src/` against
  stubs in `tests/host/`, mostly with `-fsanitize=address,undefined`. In the
  container `cc`/`c++` is `.devcontainer/cc-wrapper`: GCC, except ASan
  builds go to clang 14 (GCC's ASan can't start on arm64 kernels with
  39-bit address spaces, e.g. Raspberry Pi), plus a `strlcpy`/`strlcat`
  shim for bookworm's glibc. `solar-test` logs land in `.pio/solar-test/`.
- Known failing on upstream main v.4.17.3 too (not environment problems):
  `test_agent_reference`, `test_inference`, `test_ppp_sdkconfig`,
  `test_rtspd_job`, `test_script_net_bindings`.
- `tests/hil/` scripts talk to a real device over serial/network. Don't run
  them without a board.

## Layout

- `src/`: firmware (`main.c`, `apps/`, `jobs/`, `services/`, `shell/`,
  `drivers/`, `board/`). `include/`: public and native-module ABIs.
- `boards/`: PlatformIO board JSON, `manifests/`, `expansion_drivers.toml`.
  `sdkconfig.defaults.*`: per-board ESP-IDF config.
- `flavors/*.toml`, `packages/solar_os_packages.toml`: feature packages.
- `modules/`: external native modules (apps like doom, apple2, gameboy).
- `components/`: vendored ESP-IDF components. `scripts/`: build-time
  generators (boards, flavors, keymaps, manual) and PlatformIO hooks.

## Rules

- Never flash (`pio run -t upload`) or erase a device without asking.
- Don't edit generated output (`generated/`, `doc/manual/README.md`); change
  the source and regenerate.
- Keep docs in `doc/manual/` current when behavior changes.
