# Paradox 1738 HomeKit Bridge

ESP32 project for exposing Paradox alarm-system sensors to Apple HomeKit.
The project is a work in progress.

## Dependencies

- ESP-IDF v5.5.6, installed separately and activated using Espressif's
  `export.sh` (or `export.bat` on Windows). The project does not vendor
  ESP-IDF.
- The [ESP HomeKit SDK](https://github.com/nplourde/esp-homekit-sdk), included
  as the `esp-homekit-sdk` Git submodule.

Clone this repository and initialize its submodules:

```sh
git clone --recurse-submodules https://github.com/nplourde/esp-homekit-paradox1738.git
```

For an existing checkout:

```sh
git submodule update --init --recursive
```

## Build

Activate the ESP-IDF v5.5.6 environment, then build the ESP32 project:

```sh
cd apps/paradox1738
idf.py set-target esp32
idf.py build
```

The project downloads managed ESP-IDF components during configuration. Generated
build output, managed components, and the local `sdkconfig` are intentionally
not tracked. Project defaults are kept in `apps/paradox1738/sdkconfig.defaults`.

The HomeKit SDK path can be overridden with the `HOMEKIT_PATH` environment
variable if needed; by default it points to the checked-out submodule.

## Testing

From the repository root (with the ESP-IDF environment activated):

```sh
make check        # host unit tests, boot-log analyzer self-test, both device builds
make check-fail   # prove a failing test is reported with file and line
make smoke        # reset the board and check its boot log (no flashing)
make smoke FLASH=1  # flash first - overwrites whatever firmware is running
```

- `test/host/` — Unity unit tests for pure logic, compiled with the host `gcc`
  against the Unity sources vendored in ESP-IDF. They must not include any
  ESP-IDF header; anything needing the RTOS, UART driver or NVS belongs in a
  device test. Set `IDF_PATH` if the SDK is not at `~/Source/esp-idf`.
  Sanitizers are opt-in (`make -C test/host check SAN=1`) because they need
  `libasan`/`libubsan` to be installed.
- `test/device/smoke.sh` — hard-resets the board through the CP2102 auto-reset
  circuit, captures the boot log, and fails on a missing `HAP Started` /
  `Returned from app_main()`, a crash marker, or any error-level log line.
  `--self-test` checks the analyzer with no hardware attached.
- `make build-sim` builds with `CONFIG_PARADOX_SIMULATOR=y`, which replays
  recorded panel frames instead of reading UART2 so the firmware can be run with
  no alarm panel wired up. Never flash a simulator build and expect it to report
  real sensor states.

## Roadmap

The refactor plan is tracked in [TODO.md](TODO.md): nine milestones, from the
test harness through the multi-zone HomeKit hub, SoftAP provisioning and the web
server. Each milestone lists its work items and the validation tests that must
pass before it counts as done. Test IDs (`T4.5`, `T7.7`, ...) are stable, so use
them in commit messages and issue threads.
