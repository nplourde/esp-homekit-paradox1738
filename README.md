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

To flash a connected ESP32 and open its serial monitor (this overwrites the
firmware currently on the board):

```sh
idf.py -p /dev/ttyUSB0 flash monitor
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
- On a production build, the authenticated Debug tab can start the recorded
  panel replay at runtime for a short test. It pauses live UART reads, runs the
  recorded events through the normal decoder and HomeKit path, then restores
  the prior state and resumes live input.

## Panel wiring

The panel interface uses ESP32 UART2 at 9600 baud, 8 data bits, no parity, and
one stop bit:

| ESP32 | Paradox panel |
|---|---|
| GPIO17 (TX) | RX |
| GPIO16 (RX) | TX, through a 5 V-to-3.3 V level shifter or suitable voltage divider |
| GND | GND |

Do not connect a 5 V panel TX signal directly to the ESP32 RX pin; ESP32 GPIOs
are not 5 V tolerant. Use a properly rated level interface and verify the
panel's signal voltage and pin labels before connecting. The panel and ESP32
must share ground.

## HomeKit bridge status

The current firmware registers a bridge, one bridged accessory for each enabled
zone, and a bridged smoke-sensor accessory for fire events. Zone service type
(contact, motion, smoke, or leak) and name come from persisted configuration;
the default configuration exposes all 16 zones. Stable per-zone IDs keep
accessory AIDs independent of zone names. Panel events update HomeKit
characteristics through the shared state model, including alarm, bypass,
tamper, fire alarm/restore, and fire-loop trouble status.

Host tests and production/simulator firmware builds pass. On the attached
device, the bridge and all 16 zones plus Fire Alarm were enumerated, and the
accessory AIDs persisted across reboot and reflash. Apple Home pairing was
restored and confirmed after the production image was flashed without erasing
NVS; the production image also passed the boot smoke test. A 120-second
simulator run exercised
zone, alarm, bypass, tamper, fire, and fire-loop-trouble updates and logged
HomeKit value changes. During a paired-controller test, the user saw Zone 1
change to open in Apple Home while the simulator emitted that update, then
confirmed that all zone sensors and the fire detector transitioned open/alarm
and returned to closed/clear one by one. The two-hour simulator soak also
passed with steady heap and no boot/crash/error analyzer findings. The user
confirmed a clean Zone 1-only test in Home: alarm/fault and tamper indicators
set and cleared, open/close transitions appeared, and no other zones changed.
Bypass-specific StatusFault behavior remains unverified. The user also
observed fire-loop StatusFault change in Home. Production firmware was
restored after the test without erasing NVS and passed the boot smoke check.
Fire StatusActive and StatusTampered still need explicit characteristic
verification. Fire
`StatusTampered` is currently exposed as clear because no independent fire-loop
tamper event is modeled. See the M3 tests in [TODO.md](TODO.md) for validation
status and remaining checks.

## Wi-Fi provisioning

Wi-Fi setup uses ESP-IDF's stock unified provisioning manager over BLE. When no
Wi-Fi credentials are saved, the device starts the BLE provisioning service and
prints its QR payload and proof of possession to the serial log. Use a
provisioning app that supports ESP-IDF provisioning, select the device shown by
the provisioning QR payload/service name, and enter the printed proof of
possession and the target Wi-Fi credentials. Bluetooth must remain enabled.
Holding the BOOT button for more than three seconds and releasing it resets the
saved network credentials and returns the device to provisioning.

There is no project-specific provisioning SoftAP or web-based Wi-Fi credential
setup. Once the device joins Wi-Fi, the HomeKit bridge is available on that
network; the web UI provides status and configuration, while Wi-Fi credentials
remain managed by the stock BLE provisioning flow.

## Web server

The web server listens on port 80 and serves an embedded, phone-usable UI with
overview, zones, events, panel debug, device/Wi-Fi status, firmware update, and
password settings. HomeKit uses port 8080. Open `http://<device-ip>/` from a
client on the same trusted LAN. Until an admin password is set, the device
signs in with the default password `admin`; the serial log warns about it on
each such boot, and you should change it to a new password of 4–128 printable
ASCII characters right after signing in. The password
is stored in NVS as a salted PBKDF2-HMAC-SHA256 hash. Sessions use random
256-bit tokens, expire after 30 minutes, and are held in an HttpOnly,
SameSite=Strict cookie. Failed logins are temporarily rate-limited.

To reset a forgotten web password, hold BOOT for more than 10 seconds to invoke
the factory-reset path. It clears the admin hash and HomeKit data; the next boot
signs in with the default password `admin` again.

`GET /api/health` and `GET /api/auth/status` are public. Login, password
change, and logout are under `/api/auth/`. Authenticated API endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /api/zones` | All configured zones plus live state |
| `PUT /api/zones/{1..16}` | Update a zone's `name`, `type` (`contact`, `motion`, `smoke`, `leak`), and/or `enabled`; response reports `reboot_required_for_homekit` |
| `GET /api/status` | Current zone and fire state snapshot |
| `GET /api/status/stream` | Server-sent status updates |
| `GET /api/events?class=zone&zone=1&limit=32` | Recent decoded events, newest first; class, zone, and limit filters are optional |
| `POST /api/events/clear` | Clear the in-memory event history |
| `PUT /api/debug` | Set `{"enabled":true}` or `{"enabled":false}` |
| `GET /api/debug/panel` | Recent raw frames and decoder counters; available only when panel debug is enabled |
| `GET /api/debug/raw?bytes=N` | Verbatim panel UART bytes as hex, newest last, plus ring statistics; available only when panel debug is enabled |
| `GET /api/debug/raw/stream` | Server-sent live stream of the same raw bytes (`event: raw`) |
| `GET /api/debug/simulator` | Check whether the runtime recorded-frame test is available or running |
| `POST /api/debug/simulator` | Start a one-shot recorded-frame test from the Debug tab |
| `GET /api/system` | Firmware version, running/update slots, Wi-Fi connection, debug status, uptime, and memory headroom (`free_heap_bytes`, `min_free_heap_bytes`, `largest_heap_block_bytes`, `web_stack_free_bytes`) |
| `POST /api/ota` | Stream a raw `application/octet-stream` firmware image into the inactive OTA slot |

`GET /api/system` is the memory readout for long runs: it reports internal-RAM
free bytes, the low-water mark since boot, the largest contiguous internal
block, and the web server task's own smallest free stack since boot
(`web_stack_free_bytes`). The Device tab shows the same figures, and a soak run
is healthy when the low-water mark stays flat and the stack figure stays well
above zero.

`test/device/soak.sh` automates that observation over a long run; `make
soak HOST=<device address>` runs it for 24 h, sampling once a minute
(`INTERVAL` and `DURATION` override). Each row of the CSV carries the uptime,
the three heap figures, the web stack watermark, the decoder counters, and the
raw tap's `bytes_recorded` - the cumulative count of bytes taken off the panel
wire, whose delta is the arrival signal. Read the two together: a stretch where
`bytes_recorded` and `frames_ok` both freeze is the panel not transmitting, while
`bytes_recorded` climbing and `frames_ok` flat means bytes are arriving but not
framing. Uptime going backwards means the device reset, which the logger notes in
the CSV and reports with a non-zero exit; it logs back in first, because sessions
live in RAM only and a reboot invalidates them. The password is asked for (or
taken from `PARADOX_WEB_PASSWORD`) rather than passed as an argument, so it does
not land in shell history or `ps`. `make soak-selftest` checks the reboot
detector with no device attached.

The Debug tab can also mirror the panel UART byte-for-byte over Wi-Fi. The raw
tap records every byte the UART task reads, including the bytes the decoder
discards while it resynchronises framing, so a framing or phase problem can be
inspected from another machine without a serial lead. It is a RAM ring buffer
whose size is set under Paradox 1738 Configuration → Raw panel UART tap size, it stays off until panel debug is enabled, and it never logs bytes as
text.

The event history is RAM-only; its capacity is configurable under the Paradox
1738 menu in `idf.py menuconfig`. Zone changes persist in NVS and require a
reboot before HomeKit applies changed names, service types, or enabled state.
The authenticated Debug tab can run a short recorded-frame replay through the
normal decoder, state, and HomeKit paths. Live UART reads pause during the
replay, buffered panel traffic is processed afterward, and the previous HomeKit
state is restored when the test ends. The simulated frames remain in the
in-memory event log until it is cleared or the device reboots; simulated alarm
states may be briefly visible to HomeKit controllers.
The OTA endpoint refuses images larger than the inactive partition and only
selects an image after ESP-IDF image validation and a project-name check. The
bootloader rollback option is enabled; the application marks a pending image
valid after its HomeKit and web services initialize. This is not a signed
firmware distribution mechanism.

When uploading with `curl` rather than the browser, suppress the `Expect:
100-continue` header (`-H 'Expect:'`); otherwise the upload stalls for the
server's request-read timeout and returns HTTP 408. A whole-image upload of
about 1.4 MB takes roughly 20 seconds, after which the device reboots into the
new slot:

```sh
curl -c jar -H 'Referer: http://<device-ip>/' -H 'Content-Type: application/json' \
     -d '{"password":"admin"}' http://<device-ip>/api/auth/login
curl -b jar -H 'Expect:' -H 'Referer: http://<device-ip>/' \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @apps/paradox1738/build/paradox1738.bin http://<device-ip>/api/ota
```

The web UI and API are not HTTPS: passwords and session cookies travel in
plaintext on the LAN. Use only on a trusted network and do not expose port 80
to the internet. Disable the web server in `idf.py menuconfig` under Example
Configuration → Enable the authenticated web server.

## Roadmap

The refactor plan is tracked in [TODO.md](TODO.md): nine milestones, from the
test harness through the multi-zone HomeKit hub and authenticated web server.
Custom SoftAP provisioning was dropped; the stock ESP-IDF BLE provisioning flow
is retained. Each milestone lists its work items and validation tests. Test
IDs (`T4.5`, `T7.7`, ...) are stable, so use them in commit messages and issue
threads.
