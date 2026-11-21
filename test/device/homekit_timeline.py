#!/usr/bin/env python3
"""Record the device side of a HomeKit observation (T3.5, T3.6, T7.3, T8.1).

Some checks can only be finished by a person: trigger a zone on the alarm panel
and look at what Apple Home shows. This tool supplies the other half of that
question so the observation is not left to memory. It holds the authenticated
status stream open and writes one CSV line whenever anything HomeKit-mirrored
changes - which zone is open, alarming, tampered or bypassed, and the fire
state - together with the device's own stamps (`generation`, `pushed_ms`, and
each zone's `last_change_ms`). "Did Home show it within two seconds?" becomes
"the device published it at 22:31:07, gen 41, 84 ms after the panel event"
against "I saw the tile flip at 22:31:08".

Run it in a terminal you can see, not in the background of an agent session: a
process that is killed between turns records almost nothing, which is how the
first attempt at this ended. Keep it running while you walk the doors.

  make timeline HOST=192.168.4.30 DURATION=900
  ./test/device/homekit_timeline.py --self-test   # maths, no device needed

The password comes from PARADOX_WEB_PASSWORD, else a prompt - never an argument,
so it does not land in shell history or `ps`. The device keeps a single web
session, so if another client signs in, this stream is cut off; the tool notices
the 401 and signs back in rather than treating it as the end of the run.
"""
import argparse
import getpass
import http.client
import json
import os
import select
import sys
import time

COLUMNS = ("wall", "generation", "pushed_ms", "changed", "zones_open",
           "zones_alarm", "zones_tampered", "zones_bypassed", "fire",
           "fire_fault", "fire_last_change_ms")

# Copied verbatim from a live device's `GET /api/status` response (192.168.4.30,
# 2026-11-20) so the tests below cannot drift from the wire format again. The
# first version of this script asked for `tamper` and `bypass`; the device spells
# them `tampered` and `bypassed`, and `dict.get()` answers None, so a tamper -
# one of the two things the script exists to observe - would never have shown up.
DEVICE_ZONE = {"alarm": False, "bypassed": False, "id": 3, "last_change_ms": 0,
               "open": False, "tampered": False}
DEVICE_FIRE = {"alarm": False, "fault": False, "last_change_ms": 0,
               "restored": False}


def zone(zid, **flags):
    """A zone object shaped exactly like the device's, with `flags` overridden."""
    out = dict(DEVICE_ZONE, id=zid)
    out.update(flags)
    return out


def fire(**flags):
    out = dict(DEVICE_FIRE)
    out.update(flags)
    return out


def signature(status):
    """What this script considers "a change" - the state HomeKit mirrors.

    Key names are the device's own, not invented ones: see DEVICE_ZONE above.
    """
    zones = [(z["id"], z.get("open"), z.get("alarm"), z.get("tampered"),
              z.get("bypassed")) for z in status.get("zones", [])]
    state = status.get("fire", {})
    return (tuple(zones), bool(state.get("alarm")), bool(state.get("fault")))


def changed_summary(status):
    """Human-readable summary of what is currently asserted, with device stamps.

    A zone that is merely open is not an alarm, so the two are kept apart: Apple
    Home renders them through different characteristics.
    """
    parts = []
    for z in status.get("zones", []):
        if not (z.get("open") or z.get("alarm") or z.get("tampered")):
            continue
        flags = "".join(letter for letter, key in (("O", "open"), ("A", "alarm"),
                                                   ("T", "tampered"))
                        if z.get(key))
        parts.append("zone%s=%s@%s" % (z["id"], flags, z.get("last_change_ms")))
    state = status.get("fire", {})
    flags = "".join(letter for letter, key in (("A", "alarm"), ("F", "fault"))
                    if state.get(key))
    if flags:
        parts.append("fire=%s@%s" % (flags, state.get("last_change_ms")))
    return " ".join(parts)


def row(status):
    """One CSV row for one status push. Split out so the maths is testable."""
    state = status.get("fire", {})

    def ids(key):
        return [z["id"] for z in status.get("zones", []) if z.get(key)]

    return [
        time.strftime("%H:%M:%S"),
        status.get("generation"),
        status.get("pushed_ms"),
        changed_summary(status) or "-",
        ids("open"),
        ids("alarm"),
        ids("tampered"),
        ids("bypassed"),
        bool(state.get("alarm")),
        bool(state.get("fault")),
        state.get("last_change_ms"),
    ]


def login(host, port, password):
    body = json.dumps({"password": password}).encode()
    conn = http.client.HTTPConnection(host, port, timeout=10)
    try:
        # Origin must not carry an explicit port here, or it stops matching the
        # Host header the device sends itself and the write is refused.
        conn.request("POST", "/api/auth/login", body=body,
                     headers={"Content-Type": "application/json",
                              "Origin": "http://%s" % host})
        response = conn.getresponse()
        cookie = response.getheader("Set-Cookie")
        text = response.read().decode(errors="replace")
    finally:
        conn.close()
    if response.status != 200 or not cookie:
        print("login -> %d %s cookie=%r" % (response.status, text[:120], cookie),
              file=sys.stderr)
        return None
    return cookie.split(";")[0]


def record(host, port, password, duration, out_path):
    deadline = time.monotonic() + duration
    cookie = None
    # Five failed logins lock the route for 60 s, and this route is shared with
    # every other client of the module, so one failure is retried, not fatal.
    for attempt in range(12):
        cookie = login(host, port, password)
        if cookie is not None:
            break
        print("login attempt %d failed; retrying in 10 s" % (attempt + 1),
              file=sys.stderr)
        time.sleep(10.0)
    if cookie is None:
        print("timeline: login failed after retries", file=sys.stderr)
        return 1

    rows = 0
    reopens = 0
    previous = None
    with open(out_path, "w") as out:
        out.write(",".join(COLUMNS) + "\n")
        while time.monotonic() < deadline:
            conn = http.client.HTTPConnection(host, port, timeout=45)
            try:
                conn.request("GET", "/api/status/stream",
                             headers={"Accept": "text/event-stream",
                                      "Cookie": cookie})
                response = conn.getresponse()
                if response.status == 401:
                    cookie = login(host, port, password)
                    reopens += 1
                    continue
                if response.status != 200:
                    print("timeline: stream answered %d" % response.status,
                          file=sys.stderr)
                    return 1
                print("recording %s for %.0f s -> %s (Ctrl-C stops early)"
                      % (host, deadline - time.monotonic(), out_path))
                sys.stdout.flush()
                while time.monotonic() < deadline:
                    # An idle stream is normal: a quiet panel pushes nothing but
                    # keepalives. select() waits without putting the reader in an
                    # indeterminate state, which a socket timeout would.
                    ready, _, _ = select.select([conn.sock], [], [], 2.0)
                    if not ready:
                        continue
                    line = response.fp.readline()
                    if not line:
                        break
                    line = line.decode(errors="replace").strip()
                    if not line.startswith("data:"):
                        continue
                    status = json.loads(line[5:].strip())
                    current = signature(status)
                    if current == previous:
                        continue
                    previous = current
                    fields = row(status)
                    out.write(",".join(str(value) for value in fields) + "\n")
                    out.flush()
                    rows += 1
                    print("%s gen %s pushed %s  %s"
                          % (fields[0], fields[1], fields[2], fields[3]))
                    sys.stdout.flush()
            finally:
                conn.close()
            if time.monotonic() < deadline:
                # Stream dropped (device reboot, or another client took the single
                # session). Sign back in and keep the window going.
                cookie = login(host, port, password)
                reopens += 1
                time.sleep(1.0)
    print("done: %d change row(s), %d re-login(s)" % (rows, reopens))
    return 0


def self_test():
    """The row maths with no device: what counts as a change, and what a row says.

    Every fixture here is built by zone()/fire(), which copy their key names from
    DEVICE_ZONE/DEVICE_FIRE above. That is the point: the first version of this
    script read invented key names, its self-test used the same invented names, and
    so the two agreed with each other and with nothing on the device.
    """
    checks = 0

    def expect(condition, what):
        nonlocal checks
        assert condition, what
        checks += 1

    expect(set(zone(1)) == set(DEVICE_ZONE), "zone fixture keeps the device's keys")
    expect(set(fire()) == set(DEVICE_FIRE), "fire fixture keeps the device's keys")
    for key in ("open", "alarm", "tampered", "bypassed"):
        expect(key in DEVICE_ZONE, "the script reads %s, which the device sends" % key)
    for key in ("alarm", "fault"):
        expect(key in DEVICE_FIRE, "the script reads fire.%s, which the device sends" % key)

    quiet = {"generation": 1, "pushed_ms": 1000, "zones": [zone(1), zone(2)],
             "fire": fire()}
    opened = {"generation": 2, "pushed_ms": 1123,
              "zones": [zone(1, open=True, last_change_ms=1039), zone(2)],
              "fire": fire()}
    tampered = {"generation": 3, "pushed_ms": 1300,
                "zones": [zone(1, tampered=True, last_change_ms=1250), zone(2)],
                "fire": fire()}
    alarming = {"generation": 4, "pushed_ms": 1500,
                "zones": [zone(1, open=True, alarm=True, last_change_ms=1440),
                          zone(2, tampered=True, last_change_ms=1450)],
                "fire": fire(alarm=True, last_change_ms=1460)}
    trouble = {"generation": 5, "pushed_ms": 1700, "zones": [zone(1), zone(2)],
               "fire": fire(fault=True, last_change_ms=1690)}

    expect(signature(quiet) != signature(opened), "a zone opening is a change")
    expect(signature(quiet) != signature(tampered),
           "a tamper with nothing else moving is a change - the bug this caught")
    expect(signature(quiet) != signature(trouble),
           "fire-loop trouble is a change even with no zone open")
    expect(signature(opened) == signature(dict(opened)),
           "the same state pushed twice is not a change")

    fields = row(opened)
    expect(fields[3] == "zone1=O@1039", fields[3])
    expect(fields[4] == [1] and fields[5] == [] and fields[6] == [], fields)
    expect(fields[8] is False and fields[9] is False, "an open zone is not a fire alarm")

    fields = row(alarming)
    expect(fields[3] == "zone1=OA@1440 zone2=T@1450 fire=A@1460", fields[3])
    expect(fields[4] == [1] and fields[5] == [1] and fields[6] == [2], fields)
    expect(fields[8] is True and fields[10] == 1460, fields)

    fields = row(tampered)
    expect(fields[3] == "zone1=T@1250", fields[3])
    expect(fields[4] == [] and fields[6] == [1],
           "a tampered zone reads tampered, not open")

    fields = row(trouble)
    expect(fields[3] == "fire=F@1690", fields[3])
    expect(fields[8] is False and fields[9] is True, "fire fault is not fire alarm")

    expect(len(COLUMNS) == len(row(quiet)), "header and row stay in step")
    print("timeline self-test: PASS (%d checks)" % checks)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=os.environ.get("HOST", "192.168.4.30"))
    parser.add_argument("--port", default=os.environ.get("PORT", "80"))
    parser.add_argument("--duration", type=float,
                        default=float(os.environ.get("DURATION", "300")))
    parser.add_argument("--out", default=os.environ.get("OUT",
                                                        "homekit-timeline.csv"))
    parser.add_argument("--self-test", action="store_true",
                        help="check the row maths and exit; needs no device")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    password = os.environ.get("PARADOX_WEB_PASSWORD")
    if not password:
        password = getpass.getpass("web password for %s: " % args.host)
    try:
        return record(args.host, int(args.port), password, args.duration,
                      args.out)
    except KeyboardInterrupt:
        print("\nstopped; %s holds what was recorded" % args.out)
        return 0


if __name__ == "__main__":
    sys.exit(main())
