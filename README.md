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
make check        # host unit tests, device-tool self-tests, both device builds
make check-fail   # prove a failing test is reported with file and line
make smoke        # reset the board and check its boot log (no flashing)
make smoke FLASH=1  # flash first - overwrites whatever firmware is running
make soak HOST=<address>      # watch memory, stack and resets over hours
make latency HOST=<address>   # measure decode-to-push delay on a real panel
make robust HOST=<address>    # malformed and unauthorized requests against a module
make timeline HOST=<address>  # record what the bridge publishes while you trigger zones
make consistency HOST=<address> # check the HomeKit database against GET /api/status
make audit HOST=<address> PHASE=boot # read what the bridge recorded about its own requests
make ui-selftest HOST=<address> # run the page's own JavaScript against a fake DOM
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
- `test/device/soak.sh` (`make soak`) — polls the authenticated API for hours and
  reports heap, web-task stack, decoder counters and any reset (the device's uptime
  going backwards). `--self-test` checks the reset detector.
- `test/device/latency.sh` (`make latency`) — holds both debug streams open, then
  measures the delay from decoding a panel frame to publishing it using stamps the
  device puts in its own payloads. Needs real panel traffic; `--self-test` checks
  the maths.
- `test/device/robustness.sh` (`make robust`) — replays 62 malformed, unauthorized
  and cross-origin requests at a running module and fails if any is accepted, if the
  zone configuration changes by a byte, if a request goes unanswered, if the module's
  uptime goes backwards, or if an object-shaped response body does not parse as JSON.
  `--self-test` checks the case list.
- `test/device/homekit_timeline.py` (`make timeline`) — for the checks that only a
  person can finish: it holds the authenticated status stream open and writes one CSV
  row whenever anything HomeKit mirrors changes (zone open, alarm, tamper, bypass, fire),
  carrying the device's own `generation`/`pushed_ms` and each zone's `last_change_ms`. Run
  it in a terminal while you walk the doors and watch Apple Home, then compare the two
  timelines. It re-logs in when another client takes the single web session, and
  `--self-test` checks which states it considers a change.
- `test/device/homekit_consistency.py` (`make consistency`, `make consistency-selftest`) —
  answers "does the esp-hap accessory database say the same thing as `GET /api/status`?". A
  HomeKit controller cannot be asked: HAP `GET /accessories` needs a verified controller session
  (`470` / `{"status":-70401}` unpaired), so the bridge serves its own database at
  `GET /api/debug/homekit`. The tool compares the two using the mapping app_main.c actually
  implements - contact and motion mirror `open`, smoke and leak mirror `in_alarm`, StatusFault is
  `in_alarm || bypassed` (a bypass is therefore shown to HomeKit as a fault), StatusTampered is
  `tampered` - and it only compares a zone that read identically before and after the database
  read, so a busy panel narrows the evidence instead of voiding it. The summary prints how many
  compared values were expected to be 1: an agreement made only of zeros proves little, and the
  self-test (57 checks) runs against responses copied verbatim off a module plus disagreements
  injected on purpose.
- `test/device/audit_trail.py` (`make audit HOST=<address> PHASE=boot|write|refusals|restart`) —
  reads the bridge's record of what it was asked to do and holds it to account: does the trail begin
  at a boot and say so, does a zone rename show up as a record naming who did it (with the
  cross-origin attempt at the same write recorded beside it), do the robustness battery's refusals
  appear with the ring admitting how many older records aged out, and what does the restart control
  do to the trail. It has no `--self-test` because nothing in it decides anything about the device -
  the judgement lives in `paradox_audit.c`, which is host tested instead.
- The command record never trusts its own inputs (`test/host/test_audit.c`, 12 assertions): a hostile
  path cannot forge a record or break the JSON, because every field is confined to printable ASCII,
  an over-long value is truncated with a marker rather than dropped, and the renderer refuses a buffer
  that is too small instead of emitting half a record. Writing it turned up two defects: the
  truncation marker spent the terminator byte and so wrote one byte past its own field into the next,
  and asking `request_is_authorized()` who the principal was turned out to answer the request with a
  401 or 403 on the way to saying no - a defect the host suite could not see and the on-device battery
  caught on its first run.
- `test/device/web_ui_dom.mjs` (`make ui-selftest HOST=<address>`) — runs the JavaScript the device
  is actually serving against a small fake DOM with a fake `fetch`, and asserts what the zone editor
  is supposed to do: one save action writes every zone that differs, a zone the device refused stays
  marked unsaved, switching tabs does not throw away typed edits, a refresh with edits in progress
  warns, and no zone row carries a save button of its own. It fetches the page rather than a copy of
  the source, so a broken C string escape shows up here. It needs `node`, which is why it is not part
  of `make check` - that has to stay runnable with gcc, python3 and idf.py alone.
- Request bodies are bounded before the JSON parser sees them: 512 bytes, and at
  most eight levels of nested containers (`test/host/test_json_guard.c` covers the
  counting, including brackets that sit inside strings). cJSON recurses once per
  nesting level and the web server task has an 8 KiB stack, so a body built out of
  two hundred open brackets overflowed it and rebooted the module. A body that
  fails to parse now answers 400 and returns, where it used to answer 200 and then
  dereference the NULL parse result.
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

Whether the panel drives push-pull or is open-collector changes what has to be
fitted, and it is still an open question on this install: if the panel's TX is
open-collector, GPIO16 provides no pull-up once it becomes a UART RX input, so a
line that looks connected can read continuously low. [docs/WIRING.md](docs/WIRING.md)
records the pin map, the four multimeter readings and what each one implies,
the DEBUG-build warning about the provisioning passphrase, the fact that
credentials sit in plaintext in NVS while flash encryption is off, what the reset
button destroys, and the difference between `app-flash` and a full flash. The
measurement itself is T8.5, and it has been redefined: measure the signalling
level rather than touch the line to the 5 V rail, which is a way to kill the board.

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
SameSite=Strict cookie. Five consecutive failed logins lock the login endpoint
out for 60 seconds, which answers `429 Too Many Requests` with
`{"error":"Try again later"}`; other authenticated endpoints are unaffected. A
successful `POST /api/auth/login` answers `{"password_change_required":false,
"authenticated":true}` - the session cookie is only in the response headers, so
`GET /api/auth/status` on a later request is what reports the live session.

Requests that change something are also required to name the device in their
`Origin` header, or in `Referer` when there is no `Origin`: the value has to be
`http://` followed by the exact `Host` the request was addressed to, and then
either the end of the header or a `/`, `?` or `#`. There is no CSRF token on this
device, so that comparison is the entire defence against a page in the user's
browser writing to the alarm configuration; a page at
`http://192.168.4.30.evil.example`, an origin naming a different port, and an
origin with no scheme are each rejected with `403 Same-origin request required`
before the password is examined. The rule lives in `paradox_same_origin.c` so it
can be tested without a web server, and the running module is checked against the
same strings by `make robust`.

To reset a forgotten web password, hold BOOT for more than 10 seconds to invoke
the factory-reset path. It clears the admin hash and HomeKit data; the next boot
signs in with the default password `admin` again.

`GET /api/health` and `GET /api/auth/status` are public. Login, password
change, and logout are under `/api/auth/`. Authenticated API endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /api/zones` | All configured zones plus live state |
| `PUT /api/zones/{1..16}` | Update a zone's `name`, `type` (`contact`, `motion`, `smoke`, `leak`), and/or `enabled`; response reports `reboot_required_for_homekit` |
| `POST /api/homekit/restart` | Restart the process so pending zone names and types reach HomeKit; requires body `{"confirm":true}` and answers `202` before rebooting |
| `GET /api/status` | Current zone and fire state snapshot, plus `pushed_ms`: the device time the snapshot was produced |
| `GET /api/status/stream` | Server-sent status updates |
| `GET /api/events?class=zone&zone=1&limit=32` | Recent decoded events, newest first; class, zone, and limit filters are optional |
| `POST /api/events/clear` | Clear the in-memory event history |
| `PUT /api/debug` | Set `{"enabled":true}` or `{"enabled":false}` |
| `GET /api/debug/panel` | Recent raw frames and decoder counters; available only when panel debug is enabled |
| `GET /api/debug/raw?bytes=N` | Verbatim panel UART bytes as hex, newest last, plus ring statistics; available only when panel debug is enabled |
| `GET /api/audit` | The bridge's own record of the requests made of it, newest first: sequence number, uptime, whether the requester held a session, method, path, status, and a note. Read-only and authenticated |
| `GET /api/debug/homekit` | The values the esp-hap accessory database currently holds for every sensor service - state characteristic, StatusFault, StatusTampered, and StatusActive where present - so the HomeKit view can be compared with `GET /api/status` without a paired controller; available only when panel debug is enabled |
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

Two things make those counters readable correctly. **The panel broadcasts on
activity, not on a fixed cadence.** A quiet house means no bytes: a wired module
sat 354 s with `bytes_recorded: 0` while nothing happened, then produced 48 bytes
and 12 clean frames the moment two doors were opened and closed, with
`framing_errors: 0` and `bytes_lost: 0` throughout. So "no traffic for ten
minutes" is not a fault, and a long soak will show flat counters unless the
household is active - prove the link with a deliberate trigger instead (next
paragraph), and read flat-but-error-free as idle rather than broken.
**The device keeps exactly one web session:** `s_session_token` is a single slot,
and every login replaces it. A second browser tab, a second machine, or a soak
run signing in invalidates the first client's cookie, which gets `401` on its next
request - this is also why a session that seemed fine died after nine minutes
when another client logged in. The scripts here re-login on `401`, but whoever
signed in last owns the session, so do not run two API clients against one device
and expect both to keep working.

`make latency HOST=<device address> SOAK_ARGS="--duration 300"` runs
`test/device/latency.sh`. Start it, **then open and close a protected door**; the
tool reports the bridge's own delay in two ways.

The authoritative one comes from the device. Every status payload carries
`pushed_ms` (when the JSON was produced) and every zone and fire block carries
`last_change_ms` (when the decode applied the frame that changed it). Both are
the device clock, so `pushed_ms - last_change_ms` is the pipeline delay - UART
read, decode, state model, SSE write - with no client clock involved and no
guess about which stream arrived first:

```text
[status ] open/alarm zones [2], tamper -, fire clear  device delay: zone 2 12 ms
Device-reported pipeline delay (status pushed_ms - changed block's last_change_ms):
      15 ms  zone 2   within budget
     172 ms  zone 2   within budget
device delay: 8 change(s), max 172 ms, mean 91 ms
```

Eight zone transitions on the live module measured 15-172 ms, against a budget of
2000 ms. The same run also checks that closing a stream releases what it held: the
device counts live Server-Sent Events sessions, and both counts have to come back
to zero, because each stream owns an httpd request and a task of its own. Streams
were held for 300 s to confirm the long-lived case - nineteen heartbeats each, no
drop - before this was written.

The tool also pairs the two streams on the client clock, which is only
indicative. Each stream is written by its own task and small chunks on the raw
stream sit in the httpd output buffer until enough accumulate, so a raw event can
surface seconds after the byte arrived; in a live capture that made the byte
stream trail the state pushes by two to three seconds, which is invisible in the
Debug tab (it only ever shows the bytes in order) but makes the paired gap
worthless as a latency figure. It is printed under an explicit heading so it
cannot be mistaken for the measurement, and firmware that predates `pushed_ms`
falls back to it.

Pushes count only when their content changes, so keepalives and repeats create no
pairs; a run where nothing changed exits non-zero, because an idle panel is not a
passing measurement. Streams reopen automatically when they drop or when another
client steals the session, and the run says how often that happened.
`make latency-selftest` checks the maths against synthetic streams - a 0.600 s
paired gap and 600 ms / 1500 ms device delays, plus the cases that must produce
nothing.

One firmware change came out of measuring this. `paradox1738.c` used to call
`uart_read_bytes()` asking for a whole 32-byte buffer with a one second timeout,
and that call returns early only once it holds the requested count: a four-byte
frame from a quiet panel therefore sat in the UART driver for up to a full second
before the decoder saw it. The loop now asks the driver how much it already has
and reads that, waiting for a single byte when nothing is buffered, so a frame is
processed as it arrives. A frame split across two reads is still fine, because
the decoder is a byte-stream state machine and host tests cover short reads.

The Debug tab can also mirror the panel UART byte-for-byte over Wi-Fi. The raw
tap records every byte the UART task reads, including the bytes the decoder
discards while it resynchronises framing, so a framing or phase problem can be
inspected from another machine without a serial lead. It is a RAM ring buffer
whose size is set under Paradox 1738 Configuration → Raw panel UART tap size, it stays off until panel debug is enabled, and it never logs bytes as
text.

The event history is RAM-only; its capacity is configurable under the Paradox
1738 menu in `idf.py menuconfig`. Zone changes persist in NVS and require a
reboot before HomeKit applies changed names, service types, or enabled state:
the accessory database is built when the bridge starts and cannot be rebuilt in
place. While such a change is pending, `GET /api/zones` reports
`reboot_required_for_homekit: true` and the Zones tab shows a banner saying so;
that flag goes back to false only when the bridge restarts, which is the moment
HomeKit stops being stale.
The zone editor keeps edits on the page until you save them, and one save button
writes every zone you changed - there used to be a button per zone, which meant six
round trips to rename six zones and a page that could hold a mix of saved and
unsaved zones without saying which was which. The page remembers what the device last
accepted for every field, so it can name the zones that differ, keep a refused zone
marked unsaved instead of letting it look saved, and leave your typing alone when you
switch tabs.
The banner carries the button that takes the action: `POST /api/homekit/restart`
restarting the process, since the vendored esp-hap documents `hap_stop()` and
`hap_deinit()` as no-ops and only allows characteristics to be added before
`hap_start()`. The endpoint requires `{"confirm":true}` in the body, because the
restart drops every HomeKit controller's session for several seconds and sensors
answer "no response" meanwhile; the page warns you first, then waits for the
device to answer `/api/health` again and reloads. Pairing is not at risk - the keys
live in NVS, and `mDNS` keeps answering `_hap._tcp` with `sf=0` (paired and idle)
across the restart. The `robustness` battery keeps the refusals pinned: no session,
cross-origin, wrong method, and every way of not saying confirm.

The bridge also keeps a record of what it was asked to do, and reads it back over
`GET /api/audit`. The recording point is the function whose job is to refuse, so a
refusal cannot be missed by being recorded in the wrong place, and a new endpoint
inherits the record for free. Requests that no route matches are recorded too - a
`POST` aimed at an arm command the bridge refuses to have is exactly what the
record should show - while ordinary reads are not, because a browser polling the
dashboard would push interesting records out of a 16-deep ring. The trail never
contains a request body, a password or a token, and every field is confined to
printable ASCII so a hostile path cannot forge a record or break the JSON.
It is 16 records deep, it names its own capacity and its next sequence number so a
reader can tell that records aged out rather than that nothing happened, and it is
currently **RAM-only**: a reboot - including the restart button above - starts it
again at sequence 1. Persisting it across reboots is an open work item
(`audit_trail_persistence` in `TODO.md`), and it is a requirement before anything
can send commands to the panel.
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
