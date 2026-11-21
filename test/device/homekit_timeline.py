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
           "zones_alarm", "zones_tamper", "zones_bypass", "fire",
           "fire_last_change_ms")


def signature(status):
    """What this script considers "a change" - the state HomeKit mirrors."""
    zones = [(z["id"], z.get("open"), z.get("alarm"), z.get("tamper"),
              z.get("bypass")) for z in status.get("zones", [])]
    fire = status.get("fire", {})
    return (tuple(zones), bool(fire.get("alarm")), bool(fire.get("tamper")))


def changed_summary(status):
    """Human-readable summary of what is currently asserted, with device stamps.

    A zone that is merely open is not an alarm, so the two are kept apart: Apple
    Home renders them through different characteristics.
    """
    parts = []
    for zone in status.get("zones", []):
        if not (zone.get("open") or zone.get("alarm") or zone.get("tamper")):
            continue
        flags = "".join(letter for letter, key in (("O", "open"), ("A", "alarm"),
                                                   ("T", "tamper"))
                        if zone.get(key))
        parts.append("zone%s=%s@%s" % (zone["id"], flags,
                                       zone.get("last_change_ms")))
    fire = status.get("fire", {})
    if fire.get("alarm") or fire.get("tamper"):
        parts.append("fire=%s@%s" % ("A" if fire.get("alarm") else "T",
                                     fire.get("last_change_ms")))
    return " ".join(parts)


def row(status):
    """One CSV row for one status push. Split out so the maths is testable."""
    fire = status.get("fire", {})

    def ids(flag):
        return [z["id"] for z in status.get("zones", []) if z.get(flag)]

    return [
        time.strftime("%H:%M:%S"),
        status.get("generation"),
        status.get("pushed_ms"),
        changed_summary(status) or "-",
        ids("open"),
        ids("alarm"),
        ids("tamper"),
        ids("bypass"),
        bool(fire.get("alarm")),
        fire.get("last_change_ms"),
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
    """The row maths with no device: what counts as a change, and what a row says."""
    def zone(zid, **kw):
        out = {"id": zid, "open": False, "alarm": False, "tamper": False,
               "bypass": False, "last_change_ms": 0}
        out.update(kw)
        return out

    quiet = {"generation": 1, "pushed_ms": 1000,
             "zones": [zone(1), zone(2)], "fire": {"alarm": False,
                                                   "last_change_ms": 0}}
    opened = {"generation": 2, "pushed_ms": 1123,
              "zones": [zone(1, open=True, last_change_ms=1039), zone(2)],
              "fire": {"alarm": False, "last_change_ms": 0}}
    alarming = {"generation": 3, "pushed_ms": 1300,
                "zones": [zone(1, open=True, alarm=True, last_change_ms=1240),
                          zone(2, tamper=True, last_change_ms=1250)],
                "fire": {"alarm": True, "tamper": False, "last_change_ms": 1260}}

    assert signature(quiet) != signature(opened), "a zone opening must be a change"
    assert signature(quiet) != signature(alarming), "alarm must be a change"
    assert signature(opened) == signature(dict(opened)), \
        "the same state must not look like a change"

    fields = row(opened)
    assert fields[3] == "zone1=O@1039", fields[3]
    assert fields[4] == [1] and fields[5] == [], fields
    assert fields[8] is False, "an open zone is not a fire alarm"

    fields = row(alarming)
    assert fields[3] == "zone1=OA@1240 zone2=T@1250 fire=A@1260", fields[3]
    assert fields[4] == [1], "open-and-alarming still reads open"
    assert fields[5] == [1] and fields[6] == [2], fields
    assert fields[8] is True and fields[9] == 1260, fields

    # A tampered but closed zone is still worth a row: Home renders the
    # tamper characteristic on a contact sensor that never opened.
    tamper_only = {"generation": 4, "pushed_ms": 2000,
                   "zones": [zone(1, tamper=True, last_change_ms=1990), zone(2)],
                   "fire": {"alarm": False, "last_change_ms": 0}}
    assert signature(tamper_only) != signature(quiet)
    assert row(tamper_only)[3] == "zone1=T@1990", row(tamper_only)[3]
    assert row(tamper_only)[4] == [], "a tampered zone is not open"

    assert len(COLUMNS) == len(row(quiet)), "header and row must stay in step"
    print("timeline self-test: PASS (%d checks)" % 14)
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
