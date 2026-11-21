# Wiring, levels and the things a multimeter has to settle

This is the hardware-facing half of the record: what connects to what, why the
3.3 V / 5 V question is still open, and how to settle it without destroying the
board. The protocol side lives in `TODO.md` (M1); this file is about electrons.

## Pin map

| Signal | ESP32 | Direction | Notes |
|---|---|---|---|
| Panel TX → bridge RX | GPIO16 | input | The only pin that can see 5 V. See below. |
| Bridge TX → panel RX | GPIO17 | output | 3.3 V out. Whether the panel reads 3.3 V as a valid high is an open question, not an assumption. |
| Ground | GND | — | Common ground with the panel. Not optional. |

UART2, 9600 baud, 8N1. The decoder works on 4-byte frames; see
`apps/paradox1738/main/paradox1738.c` for the read loop and `paradox_proto.c`
for the decode.

Nothing here transmits to the panel today. The bridge is a receiver: there is no
TX path in the firmware, and M9 keeps it that way until captured keypad frames
decode in a host test and the credential work lands.

## The level question, stated honestly

The panel may drive its TX line at 5 V. The ESP32 GPIOs are **not** 5 V tolerant:
feeding 5 V into GPIO16 can damage the pin or the chip. Two sub-questions, and
they have different answers:

1. **Push-pull 5 V output.** The line idles high at ~5 V and is actively pulled
   low during a frame. Then the ESP32 needs a real level shifter or at minimum a
   series resistor plus a clamp to 3.3 V.
2. **Open-collector / open-drain output with a pull-up.** The line only ever
   pulls low; something external decides the high level. In that case the pull-up
   may be inside the panel (pulling to 5 V, which is still a problem) or may be
   expected to be provided by the receiver (which is fine at 3.3 V).

There is a trap specific to this board in case 2. GPIO16 has a weak pull-up only
in the boot state; once the pin is configured as UART2 RX, the ESP32 stops
providing any pull-up. If the panel's TX is open-collector and the panel expects
the receiver to pull the line up, an unwired or mis-pulled GPIO16 will read
continuous low, and the decoder will report framing errors or complete silence
with the wiring "connected". If the panel's line is open-collector and pulled to
5 V inside the panel, the pull-up must be moved to 3.3 V rather than left alone.

Symptoms to expect while this is unresolved:
- No bytes at all while the panel is demonstrably transmitting.
- Every accepted frame looks identical, or the framing-error counter climbs while
  the byte count does not.

Both of those were investigated from the web UI without a serial cable:
`GET /api/debug/panel` shows `frames_ok`, `framing_errors`, `bytes_discarded` and
`overruns`, and `GET /api/debug/raw?bytes=512` shows the verbatim bytes as hex.
An idle panel with everything wired correctly really is silent: the panel
broadcasts repeating groups of four quartets only while something changes.
A silent capture is therefore not evidence of broken wiring on its own.

## Settling it with a multimeter (the safe version)

Do **not** touch the panel TX line to the ESP32 5 V rail to "see what happens".
The measurement that answers the question is passive and needs no risk:

1. Unplug the bridge from the panel line (or unclip the module) so the panel side
   is floating relative to the ESP32.
2. Put the meter on DC volts between the panel TX wire and panel ground.
3. Watch the idle reading, then make the panel transmit - open a zone with a
   magnet or walk past a PIR, or press a keypad button.
4. Record the idle voltage and what happens during transmission.

Interpretation:

| Idle reading | During transmission | Conclusion | What to fit |
|---|---|---|---|
| ~5 V | dips to ~0 V | push-pull 5 V | level shifter (TXB0101 / BSS138 inverter pair), or series resistor to a 3.3 V clamp |
| ~3.3 V | dips to ~0 V | 3.3 V signalling | direct connection is fine; confirm with the raw capture |
| ~0 V, and rises to ~5 V only while transmitting, or floats | pulses high | open-collector, pulled high somewhere | find the pull-up. If it pulls to 5 V, move it to 3.3 V |
| 0 V, never moves, while the panel is known to be transmitting | — | wrong pin, no common ground, or wrong wire | re-check the pin map above before believing the decoder |

Then connect through whatever the answer called for and prove it in software:
`GET /api/debug/raw` should start holding bytes, and `GET /api/debug/panel`
should show `framing_errors 0` against a transmitting panel. The regression test
`test/host/test_framing.c:test_idle_panel_broadcast_decodes_cleanly` replays a
captured idle-panel group, so a level problem shows up as a decode failure rather
than a mystery.

## Serial console, log levels and a warning about DEBUG builds

The CP2102 on the bench board auto-resets via RTS, which is how
`test/device/smoke.sh` works. A normal build logs at INFO (217 lines at boot).

Two facts about credentials on the console, one reassuring and one not:

- The shipped configuration compiles debug logs **out**
  (`CONFIG_LOG_DEFAULT_LEVEL_INFO=y`, `CONFIG_LOG_MAXIMUM_LEVEL=3`). That matters
  because the vendored ESP-IDF provisioning stack contains
  `protocomm/src/crypto/srp6a/esp_srp.c`, which logs the provisioning **passphrase
  in plaintext** at `ESP_LOGD`. At the shipping log level it does not exist in the
  image. **If you raise the log level to DEBUG to chase a bug, do not treat that
  build as shippable, and do not paste its log into a bug report**: the
  provisioning passphrase and the SRP username can appear in it.
- The network name is logged by the Wi-Fi driver itself
  (`wifi:connected with <SSID>, ...`). The SSID is not a secret in the same sense
  as the key, but it is on the console, so treat serial logs as mildly sensitive.

There is no code path in this repository that logs a Wi-Fi key, a web password or
a session token. The web layer stores a PBKDF2 salt+hash and compares against
that. One deliberate exception, and it is a constant rather than a secret: the
first boot that bootstraps the web password logs
`Initial web password "admin" (change it after sign-in)`. It prints the compile-time
default, it prints once, and it never prints again once a password is set.

## Credentials at rest

There is no flash encryption and no secure boot
(`# CONFIG_SECURE_FLASH_ENC_ENABLED is not set`, `# CONFIG_SECURE_BOOT is not set`).
So the Wi-Fi key and the HomeKit pairing keys sit **in plaintext inside NVS**, and
anyone with a serial cable can read them off the chip. A `read_flash` of the NVS
partition recovers them; that is not a theory, it is what those two config lines
mean.

Enabling flash encryption on classic ESP32 is a real option, but it is a one-way
efuse operation with a good chance of bricking a board if done wrong. Treat it as
a deliberate decision with its own procedure, not a config line to flip on a
panel that is protecting a house.

The web password is not stored in plaintext, and no HTTP endpoint returns a
password, a salt, a hash or the SSID. `GET /api/system` reports
`wifi_connected` as a boolean and nothing else about credentials.

## The reset button, because it is destructive

GPIO0 (the Boot button) is the example's reset button and it does real damage:

- held > 3 s and released: clears Wi-Fi credentials (the bridge goes back to
  SoftAP/BLE provisioning);
- held > 10 s: factory reset - clears the web password, the HomeKit pairing **and**
  Wi-Fi provisioning.

A power-on hold of Boot > 10 s therefore strands the bridge: it has to be paired
in Apple Home again and reprovisioned to the house network, which needs the Wi-Fi
key. Do not let this happen to a panel that is in service. It also cannot be
undone from the web API: the web layer has no factory-reset route, by design.

## Building and flashing without destroying a pairing

Both boards keep their HomeKit pairing and NVS through updates, and that is not
an accident - it is which command was used:

```sh
idf.py -p /dev/ttyUSB0 app-flash     # app partition only: NVS and OTA state untouched
```

`idf.py flash` also rewrites the partition table and `ota_data_initial.bin`, which
resets the boot-slot selection. It will not usually erase NVS, but there is no
reason to accept the extra risk on a paired device. Over the network, use
`POST /api/ota` (or the Device tab): it writes the inactive slot, validates the
image, flips the boot partition and reboots. Pairing survives that, and the proof
is on the network rather than in the log: mDNS keeps answering `_hap._tcp` with
`sf=0`, HomeKit's paired-and-idle flag. An accessory that lost its pairing
advertises the not-paired bit instead.

After any flash or reboot, `make smoke` re-reads the boot log and fails on a
missing `HAP Started`, a crash marker, or any error-level line.
