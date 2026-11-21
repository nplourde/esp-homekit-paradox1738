#!/usr/bin/env python3
"""Compare what HomeKit is being told with what the panel state model holds (T6.5).

The question: does the esp-hap accessory database agree with GET /api/status? If the
bridge updated its state model but not the database - or the other way round - Apple
Home would show something the panel no longer believes, and nothing in the web UI would
look wrong.

Why not just ask a HomeKit controller: HAP `GET /accessories` answers 470 with
{"status":-70401} unless the requester holds a verified controller session, and the only
controller on this network is a paired iPhone that will not run a script for us. So the
bridge reads its own accessory database out loud: `GET /api/debug/homekit` (authenticated,
and hidden until panel debug is enabled, like the other debug views). It reports the value
esp-hap is holding for each characteristic, which is exactly what a controller would be
served after pairing.

The expected values are not invented here; they are what app_main.c does, transcribed from
update_zone_hap_state() and update_fire_hap_state():

    contact zone    ContactSensorState = open
    motion zone     MotionDetected     = open
    smoke / leak    SmokeDetected / LeakDetected = in_alarm
    any zone        StatusFault        = in_alarm || bypassed
    any zone        StatusTampered     = tampered
    fire accessory  SmokeDetected      = fire.alarm
    fire accessory  StatusFault        = fire.fault

Two things fall out of that table and are worth stating plainly:

* A bypassed zone is shown to HomeKit as StatusFault. These sensor services have no
  bypass characteristic, so the bridge chose fault. Apple Home will therefore render a
  bypassed zone as "not responding", which is a decision, not a bug - but it should be
  visible somewhere, and this is that somewhere.
* Only characteristics the bridge actively writes are asserted. StatusActive on the fire
  accessory and StatusTampered on it are never written by the firmware, so their value is
  reported as information rather than demanded: asserting a default the firmware does not
  own would turn an innocent SDK change into a false alarm.

Sampling honestly: on a device with panel traffic, state can change between the three
GETs. A zone is compared only when it reads identically before and after the database
read; a zone that moved is counted as moved and never called a disagreement. Several
samples are taken and the one that compared the most zones is the one reported, so a busy
panel narrows the evidence instead of voiding it.

  ./test/device/homekit_consistency.py --host 192.168.4.30 --samples 5
  ./test/device/homekit_consistency.py --self-test       # no device needed

The password comes from PARADOX_WEB_PASSWORD, else a prompt - never an argument, so it
does not land in shell history or `ps`.
"""
import argparse
import getpass
import http.client
import json
import os
import sys
import time

# ---------------------------------------------------------------------------
# Fixtures copied verbatim from a live device so the tests below cannot drift
# from the wire format. Captured from 192.168.4.62 running
# v0.1.1-3-gb654948-dirty (bench module, all 16 zones configured as contact,
# panel debug on, simulator replay finished). `GET /api/debug/homekit` did not
# exist before that build, which is how the first version of this script would
# have been tested against responses it invented itself.
DEVICE_STATUS_ZONE = {"alarm": False, "bypassed": False, "id": 1,
                      "last_change_ms": 8193616, "open": False,
                      "tampered": False}
DEVICE_FIRE = {"alarm": False, "fault": False, "last_change_ms": 0,
               "restored": False}
DEVICE_ZONE_CONFIG = {"alarm": False, "bypassed": False, "enabled": True,
                      "id": 1, "last_change_ms": 108267, "name": "Zone 1",
                      "open": False, "tampered": False, "type": "contact"}
DEVICE_HAP_ITEM = {"aid": 2, "hap_service_uuid": "80", "kind": "contact",
                   "name": "Zone 1", "state": 0, "status_fault": 0,
                   "status_tampered": 0, "zone": 1}
DEVICE_HAP_FIRE = {"aid": 18, "hap_service_uuid": "87", "kind": "smoke",
                   "name": "Fire Alarm", "state": 0, "status_active": 1,
                   "status_fault": 0, "status_tampered": 0, "zone": 0}

# The characteristics update_zone_hap_state() writes on every zone service.
ZONE_WRITTEN = ("state", "status_fault", "status_tampered")
# What update_fire_hap_state() writes on the fire accessory.
FIRE_WRITTEN = ("state", "status_fault")

# ---------------------------------------------------------------------------
# Device access


def login(host, port, password):
    body = json.dumps({"password": password}).encode()
    conn = http.client.HTTPConnection(host, port, timeout=10)
    try:
        # Origin carries no explicit port here: the device compares it against its
        # own Host header, and "http://host:80" does not match "host".
        conn.request("POST", "/api/auth/login", body=body,
                     headers={"Content-Type": "application/json",
                              "Origin": "http://%s" % host})
        response = conn.getresponse()
        cookie = response.getheader("Set-Cookie")
        text = response.read().decode(errors="replace")
    finally:
        conn.close()
    if response.status != 200 or not cookie:
        print("login -> %d %s" % (response.status, text[:160]), file=sys.stderr)
        return None
    return cookie.split(";")[0]


def get_json(host, port, path, cookie):
    conn = http.client.HTTPConnection(host, port, timeout=25)
    try:
        conn.request("GET", path, headers={"Cookie": cookie} if cookie else {})
        response = conn.getresponse()
        body = response.read().decode(errors="replace")
    finally:
        conn.close()
    if response.status != 200:
        return response.status, body
    try:
        return response.status, json.loads(body)
    except ValueError:
        return response.status, body


def fetch_triple(host, port, cookie_factory):
    """status, accessory dump, status - re-logging in once if the session died.

    The device keeps exactly one web session, so another client signing in cuts this
    one off mid-sample. That is a normal event on a shared bench, not a failure.
    """
    cookie = None
    for attempt in range(2):
        status, first = get_json(host, port, "/api/status", cookie)
        if status == 401:
            cookie = cookie_factory()
            if cookie is None:
                return None
            continue
        if status != 200:
            print("/api/status answered %d: %s" % (status, str(first)[:160]),
                  file=sys.stderr)
            return None
        status, dump = get_json(host, port, "/api/debug/homekit", cookie)
        if status == 401:
            cookie = cookie_factory()
            if cookie is None:
                return None
            continue
        if status == 404:
            print("/api/debug/homekit answered 404. Two reasons this happens: panel "
                  "debug is off on the device (Settings -> Panel debug), or the "
                  "firmware predates this endpoint.", file=sys.stderr)
            return None
        if status != 200:
            print("/api/debug/homekit answered %d: %s" % (status, str(dump)[:160]),
                  file=sys.stderr)
            return None
        status, second = get_json(host, port, "/api/status", cookie)
        if status != 200:
            print("second /api/status answered %d" % status, file=sys.stderr)
            return None
        status, zones = get_json(host, port, "/api/zones", cookie)
        if status != 200:
            print("/api/zones answered %d" % status, file=sys.stderr)
            return None
        # Order matters: compare() wants before, config, database, after.
        return first, zones, dump, second
    return None


# ---------------------------------------------------------------------------
# Comparison


def strip_time(state):
    """State without the change timestamp: timestamps move on no-op events."""
    return {k: v for k, v in state.items() if k != "last_change_ms"}


def expected_zone(kind, state):
    """What the database must hold for one zone, per app_main.c."""
    alarm = bool(state["alarm"])
    if kind in ("smoke", "leak"):
        state_value = alarm
    else:
        state_value = bool(state["open"])
    return {
        "state": 1 if state_value else 0,
        "status_fault": 1 if (alarm or bool(state["bypassed"])) else 0,
        "status_tampered": 1 if bool(state["tampered"]) else 0,
    }


def expected_fire(fire):
    return {
        "state": 1 if bool(fire["alarm"]) else 0,
        "status_fault": 1 if bool(fire["fault"]) else 0,
    }


def compare(status_before, zone_config, dump, status_after):
    """Findings as (severity, kind, subject, detail); severity is fail or info.

    Also returns how many items were comparable, how many moved, and which expected
    values were 1. The last of those matters: a device sitting quiet agrees with
    itself everywhere at zero, and an agreement made entirely of zeros proves very
    little about the values. Counting the ones that were asserted true turns "no
    disagreement" into something that can be weighed.
    """
    findings = []
    moved = []
    asserted_true = []

    config_by_id = {z["id"]: z for z in zone_config["zones"]}
    before_by_id = {z["id"]: z for z in status_before["zones"]}
    after_by_id = {z["id"]: z for z in status_after["zones"]}

    by_zone = {}
    for item in dump["accessories"]:
        by_zone.setdefault(item["zone"], []).append(item)
    for zone_id, items in sorted(by_zone.items()):
        if len(items) > 1:
            findings.append((
                "fail", "duplicate-accessory", "zone %d" % zone_id,
                "%d sensor services claim this zone: aids %s"
                % (len(items), ", ".join(str(i["aid"]) for i in items))))

    compared = 0
    for zone_id in sorted(before_by_id):
        state_before = before_by_id[zone_id]
        state_after = after_by_id.get(zone_id)
        if state_after is None or strip_time(state_before) != strip_time(state_after):
            moved.append(zone_id)
            continue
        items = by_zone.get(zone_id)
        if not items:
            findings.append((
                "fail", "missing-accessory", "zone %d" % zone_id,
                "the state model holds this zone but no sensor service advertises it"))
            continue
        item = items[0]
        compared += 1
        config = config_by_id.get(zone_id)

        if config and config["type"] != item["kind"]:
            findings.append((
                "fail", "kind-mismatch", "zone %d" % zone_id,
                "config says %r, the accessory database advertises a %s service (%s)"
                % (config["type"], item["kind"], item["hap_service_uuid"])))

        if config and config["name"] != item["name"]:
            # HomeKit shows the database name, so a difference here is what a person
            # sees as "the rename did not take". The pending flag says which case
            # this is, and the two must agree.
            if zone_config.get("reboot_required_for_homekit"):
                findings.append((
                    "info", "name-pending-restart", "zone %d" % zone_id,
                    "config says %r, HomeKit still shows %r; the device reports a "
                    "restart is pending, which matches"
                    % (config["name"], item["name"])))
            else:
                findings.append((
                    "fail", "name-mismatch", "zone %d" % zone_id,
                    "config says %r, HomeKit shows %r, and the device does not "
                    "report a pending restart"
                    % (config["name"], item["name"])))

        expected = expected_zone(item["kind"], state_before)
        for field in ZONE_WRITTEN:
            have = item.get(field, -1)
            want = expected[field]
            if want == 1:
                asserted_true.append("zone %d %s" % (zone_id, field))
            if have == -1:
                findings.append((
                    "fail", "characteristic-missing", "zone %d" % zone_id,
                    "%s is written by the bridge but the %s service does not carry it"
                    % (field, item["kind"])))
            elif have != want:
                findings.append((
                    "fail", "value-mismatch", "zone %d" % zone_id,
                    "%s: HomeKit is told %d, the panel model says it should be %d "
                    "(open=%d alarm=%d tampered=%d bypassed=%d)"
                    % (field, have, want, state_before["open"],
                       state_before["alarm"], state_before["tampered"],
                       state_before["bypassed"])))

        if config and not config["enabled"]:
            findings.append((
                "info", "disabled-zone-advertised", "zone %d" % zone_id,
                "the zone is disabled on the panel but HomeKit still shows %r; it "
                "will sit at whatever state it last had" % item["name"]))

    # The fire accessory is labelled zone 0 because no panel zone has that id.
    fire_items = by_zone.get(0, [])
    if not fire_items:
        findings.append(("fail", "missing-accessory", "fire",
                         "no smoke sensor service in the accessory database"))
    else:
        if strip_time(status_before["fire"]) != strip_time(status_after["fire"]):
            moved.append("fire")
        else:
            compared += 1
            item = fire_items[0]
            expected = expected_fire(status_before["fire"])
            for field in FIRE_WRITTEN:
                have = item.get(field, -1)
                want = expected[field]
                if want == 1:
                    asserted_true.append("fire %s" % field)
                if have == -1:
                    findings.append((
                        "fail", "characteristic-missing", "fire",
                        "%s is written by the bridge but the smoke service does not "
                        "carry it" % field))
                elif have != want:
                    findings.append((
                        "fail", "value-mismatch", "fire",
                        "%s: HomeKit is told %d, the panel model says it should be %d "
                        "(alarm=%d fault=%d restored=%d)"
                        % (field, have, want, status_before["fire"]["alarm"],
                           status_before["fire"]["fault"],
                           status_before["fire"]["restored"])))
            for field in ("status_active", "status_tampered"):
                # Never written by update_fire_hap_state(): report the value the
                # database holds without demanding a particular one.
                findings.append((
                    "info", "unowned-characteristic", "fire",
                    "%s reads %s; the bridge never writes it, so whatever esp-hap "
                    "created it with is what HomeKit sees"
                    % (field, item.get(field, -1))))

    # Services in the database that mirror no zone the state model knows about.
    for item in dump["accessories"]:
        if item["zone"] == 0:
            continue
        if item["zone"] not in before_by_id:
            findings.append((
                "fail", "accessory-without-zone", "aid %d" % item["aid"],
                "advertises %r as zone %d, which the state model does not report"
                % (item["name"], item["zone"])))

    return findings, compared, moved, asserted_true


# ---------------------------------------------------------------------------
# Self-test: the maths, against fixtures that came off a real device


def zone_state(zid, **flags):
    out = dict(DEVICE_STATUS_ZONE, id=zid)
    out.update(flags)
    return out


def zone_cfg(zid, **flags):
    out = dict(DEVICE_ZONE_CONFIG, id=zid)
    out.update(flags)
    return out


def hap_item(zid, **flags):
    out = dict(DEVICE_HAP_ITEM, zone=zid)
    out.update(flags)
    return out


def self_test():
    checks = 0
    failures = []

    def check(condition, what):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(what)

    status = {"fire": dict(DEVICE_FIRE), "generation": 0, "pushed_ms": 108267,
              "zones": [zone_state(1), zone_state(2)]}
    config = {"reboot_required_for_homekit": False,
              "zones": [zone_cfg(1), zone_cfg(2, name="Zone 2")]}
    dump = {"generation": 0, "sensors": 3,
            "accessories": [hap_item(1, name="Zone 1"),
                            hap_item(2, aid=3, name="Zone 2"),
                            dict(DEVICE_HAP_FIRE)]}

    # Every key this script reads must exist on the device's own objects. The
    # timeline recorder once asked for `tamper` where the device sends `tampered`:
    # dict.get() answered None, so the thing being observed never appeared, and the
    # self-test agreed with the bug because it invented the same name.
    for path, obj in (("/api/status zones[]", DEVICE_STATUS_ZONE),
                      ("/api/status fire", DEVICE_FIRE),
                      ("/api/zones zones[]", DEVICE_ZONE_CONFIG),
                      ("/api/debug/homekit accessories[]", DEVICE_HAP_ITEM)):
        for key in ("id", "open", "alarm", "tampered", "bypassed"):
            if "zones[]" in path and "homekit" not in path:
                check(key in obj, "%s is missing the key %r this script reads"
                      % (path, key))
        if "homekit" in path:
            for key in ("zone", "kind", "name", "state", "status_fault",
                        "status_tampered", "hap_service_uuid"):
                check(key in obj, "%s is missing the key %r this script reads"
                      % (path, key))
        if obj is DEVICE_ZONE_CONFIG:
            for key in ("name", "type", "enabled"):
                check(key in obj, "/api/zones zones[] missing %r" % key)

    # A real quiet device: the fixtures above are its actual responses, and the
    # comparison must say nothing is wrong.
    findings, compared, moved, asserted = compare(status, config, dump, status)
    check(not [f for f in findings if f[0] == "fail"],
          "quiet real-device fixtures reported failures: %s" % findings)
    check(compared == 3, "expected 3 comparable items, got %d" % compared)
    check(not moved, "quiet fixtures reported movement: %s" % moved)
    check(asserted == [],
          "a quiet device asserts nothing true, so this must be empty: %s" % asserted)

    # Injected disagreements: one per characteristic the bridge writes, so a
    # comparison that ignores values cannot pass.
    cases = [
        ("state", "zone 1", dict(DEVICE_STATUS_ZONE, id=1, open=True), None,
         "an open contact zone must appear as ContactSensorState=1"),
        ("status_tampered", "zone 1", dict(DEVICE_STATUS_ZONE, id=1,
                                           tampered=True), None,
         "tamper must appear as StatusTampered=1"),
        ("status_fault", "zone 1", dict(DEVICE_STATUS_ZONE, id=1,
                                        bypassed=True), None,
         "a bypass must appear as StatusFault=1"),
        ("status_fault", "zone 1", dict(DEVICE_STATUS_ZONE, id=1, alarm=True),
         None, "an alarm must appear as StatusFault=1 as well as the state char"),
        ("state", "fire", None, dict(DEVICE_FIRE, alarm=True),
         "a fire alarm must appear as SmokeDetected=1"),
        ("status_fault", "fire", None, dict(DEVICE_FIRE, fault=True),
         "a fire fault must appear as StatusFault=1"),
    ]
    for field, subject, zone_override, fire_override, why in cases:
        noisy_status = {"fire": fire_override or dict(DEVICE_FIRE),
                        "generation": 7, "pushed_ms": 0,
                        "zones": [zone_override or zone_state(1), zone_state(2)]}
        # The database is left at the quiet fixtures: it disagrees with the state
        # model, which is exactly the failure this tool exists to catch.
        findings, compared, moved, asserted = compare(noisy_status, config, dump,
                                            noisy_status)
        values = [f for f in findings
                  if f[1] == "value-mismatch" and f[2] == subject]
        check(len(values) == 1,
              "%s: expected one value-mismatch on %s, got %s" % (why, subject,
                                                                 values))
        if values:
            check(field in values[0][3],
                  "%s: finding does not name %s: %s" % (why, field, values[0][3]))
        # The point of the case is a value the panel says is true; if the count of
        # true assertions were empty, "agreement" would mean nothing more than two
        # piles of zeros matching.
        check(any(a.startswith(subject + " ") and a.endswith(" " + field)
                  for a in asserted),
              "%s: the comparison should record that %s/%s was expected to be 1 (%s)"
              % (why, subject, field, asserted))

    # Motion zones: the state characteristic is a bool on the wire and is read from
    # a different member of the value union.
    motion_status = {"fire": dict(DEVICE_FIRE), "generation": 9, "pushed_ms": 0,
                     "zones": [zone_state(1, open=True)]}
    motion_config = {"reboot_required_for_homekit": False,
                     "zones": [zone_cfg(1, type="motion")]}
    motion_dump = {"generation": 9, "sensors": 2,
                   "accessories": [hap_item(1, kind="motion",
                                           hap_service_uuid="85", state=1),
                                   dict(DEVICE_HAP_FIRE)]}
    findings, compared, moved, asserted = compare(motion_status, motion_config, motion_dump,
                                        motion_status)
    check(not [f for f in findings if f[0] == "fail"],
          "a motion zone with MotionDetected=1 should agree: %s" % findings)
    motion_dump["accessories"][0]["state"] = 0
    findings, _, _, _ = compare(motion_status, motion_config, motion_dump,
                             motion_status)
    check(any(f[1] == "value-mismatch" for f in findings),
          "a motion zone advertising MotionDetected=0 while open must be caught")
    motion_dump["accessories"][0]["state"] = 1
    motion_dump["accessories"][0]["kind"] = "contact"
    findings, _, _, _ = compare(motion_status, motion_config, motion_dump,
                             motion_status)
    check(any(f[1] == "kind-mismatch" for f in findings),
          "config says motion but the database advertises contact: must be caught")

    # The rename case, and the pending flag that is supposed to explain it.
    renamed = {"reboot_required_for_homekit": True,
               "zones": [zone_cfg(1, name="Front Door")]}
    findings, _, _, _ = compare(status, renamed, dump, status)
    check(any(f[0] == "info" and f[1] == "name-pending-restart" for f in findings),
          "a name the config has changed but HomeKit still shows, with the flag set, "
          "should be reported as pending rather than as a failure")
    check(not [f for f in findings if f[0] == "fail"],
          "a pending rename must not be a failure: %s" % findings)
    renamed["reboot_required_for_homekit"] = False
    findings, _, _, _ = compare(status, renamed, dump, status)
    check(any(f[0] == "fail" and f[1] == "name-mismatch" for f in findings),
          "a stale HomeKit name with no pending-restart flag is a real disagreement")

    # Missing and extra accessories.
    findings, _, _, _ = compare(status, config, {"generation": 0, "sensors": 1,
                                              "accessories": [hap_item(1)]},
                             status)
    check(any(f[1] == "missing-accessory" for f in findings),
          "a zone the state model holds with no accessory advertising it must be caught")
    extra = {"generation": 0, "sensors": 3,
             "accessories": [hap_item(1), hap_item(2, aid=3),
                             hap_item(99, aid=99), dict(DEVICE_HAP_FIRE)]}
    findings, _, _, _ = compare(status, config, extra, status)
    check(any(f[1] == "accessory-without-zone" for f in findings),
          "an accessory advertising a zone the state model does not know must be caught")
    dupe = {"generation": 0, "sensors": 4,
            "accessories": [hap_item(1), hap_item(1, aid=77), hap_item(2, aid=3),
                            dict(DEVICE_HAP_FIRE)]}
    findings, _, _, _ = compare(status, config, dupe, status)
    check(any(f[1] == "duplicate-accessory" for f in findings),
          "two services claiming one zone must be caught")

    # A characteristic the service does not carry reads as -1, and must not be
    # mistaken for "present and zero".
    absent = {"generation": 0, "sensors": 3,
              "accessories": [hap_item(1, status_tampered=-1), hap_item(2, aid=3),
                              dict(DEVICE_HAP_FIRE)]}
    findings, _, _, _ = compare(status, config, absent, status)
    check(any(f[1] == "characteristic-missing" for f in findings),
          "a missing characteristic (-1) must be reported as missing, not as zero")

    # A zone that changed while the database was being read is not a disagreement.
    shifted = {"fire": dict(DEVICE_FIRE), "generation": 12, "pushed_ms": 0,
               "zones": [zone_state(1, open=True), zone_state(2)]}
    findings, compared, moved, asserted = compare(status, config, dump, shifted)
    check(1 in moved, "a zone that changed between samples must be listed as moved")
    check(compared == 2, "the moved zone must not be counted as compared: %d"
          % compared)
    check(not [f for f in findings
               if f[1] == "value-mismatch" and f[2] == "zone 1"],
          "a zone that moved mid-sample must not be called a disagreement: %s"
          % findings)

    print("homekit_consistency self-test: %d checks, %d failure(s)"
          % (checks, len(failures)))
    for failure in failures:
        print("  FAIL " + failure)
    return 1 if failures else 0


# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", help="device address, e.g. 192.168.4.30")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--samples", type=int, default=5,
                       help="how many status/dump/status samples to take (default 5)")
    parser.add_argument("--password", help="prefer PARADOX_WEB_PASSWORD over this")
    parser.add_argument("--self-test", action="store_true",
                        help="run the comparison against recorded fixtures")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.host:
        parser.error("--host is required unless --self-test is given")

    password = args.password or os.environ.get("PARADOX_WEB_PASSWORD")
    if not password:
        password = getpass.getpass("web password for %s: " % args.host)

    def sign_in():
        # Five failed logins lock the route for 60 s and the device serves one
        # session at a time, so one retry with a gap, not a burst.
        for attempt in range(6):
            cookie = login(args.host, args.port, password)
            if cookie:
                return cookie
            time.sleep(10.0)
        return None

    cookie = sign_in()
    if cookie is None:
        print("consistency: login failed", file=sys.stderr)
        return 1

    best = None
    for sample in range(args.samples):
        triple = fetch_triple(args.host, args.port, sign_in)
        if triple is None:
            return 1
        findings, compared, moved, asserted = compare(*triple)
        print("sample %d: generation %s (database %s), %d compared, %d moved, "
              "%d expected true"
              % (sample + 1, triple[0].get("generation"),
                 triple[2].get("generation"), compared, len(moved), len(asserted)))
        # Prefer the sample that compared the most items and, among those, the one
        # with the most assertions of "this must be 1": that is the sample with the
        # most to lose if the two sides disagreed.
        if best is None or (compared, len(asserted)) > (best[1], len(best[3])):
            best = (findings, compared, moved, asserted, triple)

    findings, compared, moved, asserted, triple = best
    failures = [f for f in findings if f[0] == "fail"]
    infos = [f for f in findings if f[0] == "info"]
    print()
    for severity, kind, subject, detail in infos:
        print("info  %-28s %s: %s" % (kind, subject, detail))
    for severity, kind, subject, detail in failures:
        print("FAIL  %-28s %s: %s" % (kind, subject, detail))
    print()
    print("%d of %d state items compared (%d moved during the sample); "
          "%d failure(s), %d note(s)"
          % (compared, len(triple[0]["zones"]) + 1, len(moved), len(failures),
             len(infos)))
    if asserted:
        print("expected to be 1, and found to be 1: %s"
              % ", ".join(asserted[:24]) + (" ..." if len(asserted) > 24 else ""))
    else:
        print("Nothing was expected to be 1 in this sample: every compared value was "
              "zero, so this agreement is weak evidence. Drive some zones (walk a "
              "door, or run the panel simulator) and compare again.")
    if failures:
        print("The panel state model and the HomeKit database disagree. Until that is "
              "fixed, Apple Home is showing something the panel does not believe.")
        return 1
    print("The accessory database agrees with GET /api/status for every item that "
          "stayed still during the sample.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
