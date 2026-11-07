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
git clone --recurse-submodules https://github.com/nplourde/esp-homekit-paradox.git
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
