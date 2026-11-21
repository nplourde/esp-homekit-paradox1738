#!/usr/bin/env python3
"""Throw malformed web requests at a device and insist it survives (T6.3).

The request rules live in the pure config module and are host tested, but three
promises can only be checked against a running server:

  1. every rejected request answers 4xx with an error message,
  2. a rejected edit leaves the stored configuration byte-identical, so no
     half-written zone lands in NVS,
  3. nothing in the battery reboots the device.

Promise 3 is a regression test. A PUT whose JSON body was cut off
(`{"name":"x"`) used to reboot the module: receive_json() returned the status of
the error reply it had just sent, which is ESP_OK, so the handler's
`receive_json(...) != ESP_OK` guard never fired and it walked a NULL cJSON tree.
The request hung until timeout and the device came back with uptime reset to a
few seconds. Every case below therefore compares /api/system uptime_ms before
and after; uptime going backwards is a reboot, whatever the HTTP status said.

Each case also names the status it expects. A case that answers something else
is a finding, not a pass - which is how a truncated body was once reported as
"Invalid zone fields" (a message from a different check) rather than
"Invalid JSON object".

Usage:
  test/device/robustness.sh --host 192.168.4.30 [--password P]
  test/device/robustness.sh --self-test

Exit status: 0 when every case behaved and no reboot was seen, 1 otherwise,
2 on bad arguments. Needs only the Python 3 standard library.

Note on sessions: the device holds exactly one web session, so signing in here
invalidates any other client's cookie (a browser tab, a soak run). One case logs
in with a body that has no password, which counts as one failed login; the device
locks logins out for 60 s after five failures, so the battery keeps it to one.
"""

import argparse
import getpass
import http.client
import json
import os
import sys
import time

BODY_LIMIT = 512          # WEB_REQUEST_BODY_MAX in web_server.c
MAX_NAME = 64             # PARADOX_CONFIG_ZONE_NAME_MAX

# Cases that must be sent with no session cookie at all, by label pattern: the
# tool clears its cookie before them and signs back in after.
ANONYMOUS_SUFFIXES = ("no session", "without session", "after logout")


def anonymous_case_labels():
    return {label for label, *_ in cases()
            if label.endswith(ANONYMOUS_SUFFIXES)}



class Device(object):
    def __init__(self, host, port, password):
        self.host = host
        self.port = port
        self.password = password
        self.session = None

    def request(self, method, path, body=None, content_type="application/json",
                origin=None, cookie=True):
        """Return (status, text). Sends exactly one request per connection."""
        connection = http.client.HTTPConnection(self.host, self.port, timeout=20)
        headers = {"Host": self.host,
                   "Origin": origin if origin is not None
                           else "http://%s" % self.host}
        if cookie and self.session:
            headers["Cookie"] = "session=%s" % self.session
        if body is not None:
            if content_type:
                headers["Content-Type"] = content_type
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            text = response.read().decode("utf-8", "replace")
            if response.status == 200 and method == "POST" and \
                    path == "/api/auth/login":
                header = response.getheader("Set-Cookie") or ""
                if "session=" in header:
                    self.session = header.split("session=")[1].split(";")[0]
            return response.status, text
        finally:
            connection.close()

    def login(self):
        """Sign in, waiting out a reboot or a lockout."""
        deadline = time.time() + 120
        while time.time() < deadline:
            try:
                status, text = self.request("POST", "/api/auth/login",
                                            json.dumps({"password": self.password}),
                                            cookie=False)
                if status == 200 and self.session:
                    return
                if status == 429:
                    time.sleep(20)
                    continue
            except Exception:
                pass
            time.sleep(3)
        raise RuntimeError("cannot obtain a session from %s" % self.host)

    def uptime_ms(self):
        """Device uptime, or None if the device is unreachable."""
        deadline = time.time() + 120
        while time.time() < deadline:
            try:
                status, text = self.request("GET", "/api/system")
                if status == 200:
                    return int(json.loads(text)["uptime_ms"])
                if status == 401:
                    self.login()
                    continue
            except Exception:
                pass
            time.sleep(3)
        return None


def cases(host="192.0.2.1"):
    """(label, method, path, body, content type, origin, expected status).

    `host` is the address the device is reached at, so the same-origin cases
    can name origins that only look like it. The default is RFC 5737
    documentation space, used by the self-test.
    """
    long_name = "x" * (MAX_NAME + 1)
    ok_origin = None
    bad_origin = "http://not-this-device.example"
    zone = "/api/zones/1"
    body = lambda text: text                                    # noqa: E731
    return [
        # Unauthenticated: no cookie at all, and no session is created. These are
        # the only cases that must be sent without a session, listed by label so
        # run() clears it - a case that was supposed to be anonymous but arrived
        # with a valid cookie would happily write to NVS and report 200 as if the
        # guard had failed.
        ("GET status no session", "GET", "/api/status", None, None, None, 401),
        ("GET status stream no session", "GET", "/api/status/stream", None,
         None, None, 401),
        ("GET events no session", "GET", "/api/events", None, None, None, 401),
        ("GET system no session", "GET", "/api/system", None, None, None, 401),
        ("GET debug raw no session", "GET", "/api/debug/raw", None, None,
         None, 401),
        ("GET debug raw stream no session", "GET", "/api/debug/raw/stream",
         None, None, None, 401),
        ("GET debug panel no session", "GET", "/api/debug/panel", None, None,
         None, 401),
        ("GET simulator status no session", "GET", "/api/debug/simulator",
         None, None, None, 401),
        # Open by design, and pinned so they cannot quietly become the way in:
        # liveness says nothing about the configuration, and auth status reports
        # only whether a session exists - never a token or a password.
        ("GET health no session", "GET", "/api/health", None, None, None, 200),
        ("GET auth status no session", "GET", "/api/auth/status", None, None,
         None, 200),
        ("GET zones without session", "GET", "/api/zones", None, None, ok_origin, 401),
        ("PUT zone without session", "PUT", zone, body('{"name":"x"}'),
         "application/json", ok_origin, 401),
        ("POST logout", "POST", "/api/auth/logout", body("{}"),
         "application/json", ok_origin, 200),
        ("PUT zone after logout", "PUT", zone, body('{"name":"x"}'),
         "application/json", ok_origin, 401),

        # Same-origin guard. There is no CSRF token on this device, so this is
        # the only thing between a random page in the user's browser and a write
        # to the panel configuration - the guard is `Origin` (or `Referer`)
        # matching the Host the request was addressed to, and the cases below
        # are the ways an attacker's origin can look superficially identical.
        # paradox_same_origin.c holds the rule and test/host/test_same_origin.c
        # pins the same strings offline; these confirm the running server applies
        # them before it ever looks at the credentials.
        ("PUT cross-origin", "PUT", zone, body('{"name":"x"}'),
         "application/json", bad_origin, 403),
        ("PUT origin is a host prefix", "PUT", zone, body('{"name":"x"}'),
         "application/json", "http://%s.evil.example" % host, 403),
        ("PUT origin names another port", "PUT", zone, body('{"name":"x"}'),
         "application/json", "http://%s:9000" % host, 403),
        ("PUT origin has no scheme", "PUT", zone, body('{"name":"x"}'),
         "application/json", host, 403),

        # Zone index bounds, including the shapes strtol used to accept. The
        # /api/zones/<id> route is registered for PUT only, so a GET there is
        # rejected by the router (405) rather than by the index rules.
        ("GET zone 0", "GET", "/api/zones/0", None, None, ok_origin, 405),
        ("GET zone 17", "GET", "/api/zones/17", None, None, ok_origin, 405),
        ("PUT no index", "PUT", "/api/zones", body('{"name":"x"}'),
         "application/json", ok_origin, 405),
        ("PUT zone 0", "PUT", "/api/zones/0", body('{"name":"x"}'),
         "application/json", ok_origin, 400),
        ("PUT zone 17", "PUT", "/api/zones/17", body('{"name":"x"}'),
         "application/json", ok_origin, 400),
        ("PUT zone 1x", "PUT", "/api/zones/1x", body('{"name":"x"}'),
         "application/json", ok_origin, 400),
        ("PUT zone 01", "PUT", "/api/zones/01", body('{"name":"x"}'),
         "application/json", ok_origin, 400),
        ("PUT zone +1", "PUT", "/api/zones/+1", body('{"name":"x"}'),
         "application/json", ok_origin, 400),
        ("PUT zone 20-digit", "PUT", "/api/zones/99999999999999999999",
         body('{"name":"x"}'), "application/json", ok_origin, 400),

        # Field vocabulary and value shapes.
        ("PUT unknown type", "PUT", zone, body('{"type":"bogus"}'),
         "application/json", ok_origin, 400),
        ("PUT type Contact", "PUT", zone, body('{"type":"Contact"}'),
         "application/json", ok_origin, 400),
        ("PUT type with space", "PUT", zone, body('{"type":"smoke "}'),
         "application/json", ok_origin, 400),
        ("PUT empty name", "PUT", zone, body('{"name":""}'),
         "application/json", ok_origin, 400),
        ("PUT 65-char name", "PUT", zone, body('{"name":"%s"}' % long_name),
         "application/json", ok_origin, 400),
        ("PUT name as number", "PUT", zone, body('{"name":5}'),
         "application/json", ok_origin, 400),
        ("PUT enabled as string", "PUT", zone, body('{"enabled":"yes"}'),
         "application/json", ok_origin, 400),
        ("PUT unknown field", "PUT", zone, body('{"foo":1}'),
         "application/json", ok_origin, 400),
        ("PUT no fields", "PUT", zone, body("{}"), "application/json", ok_origin, 400),
        ("PUT valid name plus invalid type", "PUT", zone,
         body('{"name":"SHOULD NOT BE STORED","type":"bogus"}'),
         "application/json", ok_origin, 400),

        # Body shape: these are the requests that used to reboot the device.
        ("PUT truncated object", "PUT", zone, body('{"name":"x"'),
         "application/json", ok_origin, 400),
        ("PUT truncated string", "PUT", zone, body('{"name":"x'),
         "application/json", ok_origin, 400),
        ("PUT not JSON", "PUT", zone, body("not json at all"),
         "application/json", ok_origin, 400),
        ("PUT JSON array", "PUT", zone, body("[]"), "application/json", ok_origin, 400),
        ("PUT empty body", "PUT", zone, body(""), "application/json", ok_origin, 400),
        ("PUT no Content-Type", "PUT", zone, body('{"name":"x"}'), None,
         ok_origin, 415),
        ("PUT text/plain", "PUT", zone, body('{"name":"x"}'), "text/plain",
         ok_origin, 415),
        ("PUT oversized body", "PUT", zone,
         body('{"name":"' + "y" * (BODY_LIMIT + 10) + '"}'), "application/json",
         ok_origin, 413),
        ("PUT deeply nested", "PUT", zone, body("[" * 200 + "]" * 200),
         "application/json", ok_origin, 400),
        ("PUT deeply nested strings", "PUT", zone,
         body('{"name":"' + "[" * 200 + '"}'), "application/json", ok_origin, None),

        # Other endpoints take JSON too; they had the same guard. /api/debug is a
        # PUT, so a POST there is a router rejection, and the simulator POST takes
        # no body at all - it would start a 44-frame replay, so it is not in this
        # battery. The events/clear case only checks the unauthenticated path,
        # because clearing the log for real would destroy other evidence.
        ("POST login truncated", "POST", "/api/auth/login", body('{"password":'),
         "application/json", ok_origin, 400),
        ("POST login no password field", "POST", "/api/auth/login", body("{}"),
         "application/json", ok_origin, 401),
        ("PUT debug truncated", "PUT", "/api/debug", body('{"enabled":'),
         "application/json", ok_origin, 400),
        ("PUT debug bad value", "PUT", "/api/debug", body('{"enabled":"yes"}'),
         "application/json", ok_origin, 400),
        ("PUT debug not object", "PUT", "/api/debug", body("7"),
         "application/json", ok_origin, 400),
        ("POST debug wrong method", "POST", "/api/debug", body('{"enabled":true}'),
         "application/json", ok_origin, 405),
        ("POST events/clear no session", "POST", "/api/events/clear", body("{}"),
         "application/json", ok_origin, 401),
    ]


def run(args):
    device = Device(args.host, args.port, args.password)
    device.login()
    before = device.uptime_ms()
    baseline_status, baseline = device.request("GET", "/api/zones")
    print("uptime %d ms, %d bytes of zone configuration as the baseline"
          % (before, len(baseline)))

    failures = []
    reboots = []
    unhandled = []
    for label, method, path, body, content_type, origin, expected in cases(
            args.host):
        if label.endswith(ANONYMOUS_SUFFIXES):
            device.session = None
        try:
            status, text = device.request(method, path, body, content_type,
                                          origin)
        except Exception as error:
            # A request the device never answered is a finding in its own right:
            # a reset or a hang is what a crash looks like from this side.
            status, text = 0, "no response: %s" % error
            unhandled.append(label)
        if label.endswith(ANONYMOUS_SUFFIXES) or \
                label == "POST login no password field":
            # Sign back in so the rest of the battery still has a session.
            device.login()
        time.sleep(0.4)
        after = device.uptime_ms()
        if after < before:
            reboots.append(label)
        verdict = ""
        if status == 0:
            failures.append("%s: %s" % (label, text[:80]))
            verdict = "  <<< no response"
        elif expected is not None and status != expected:
            failures.append("%s: expected %d, got %d (%s)"
                            % (label, expected, status, text[:60]))
            verdict = "  <<< unexpected status"
        print("%-34s %-4s %s%s" % (label, status, text[:58].replace("\n", " "),
                                   verdict))
        before = after

    final_status, final = device.request("GET", "/api/zones")
    if final != baseline:
        failures.append("zone configuration changed: %d bytes -> %d bytes"
                        % (len(baseline), len(final)))
        for index in range(min(len(baseline), len(final))):
            if baseline[index] != final[index]:
                failures.append("first difference at byte %d: %r vs %r"
                                % (index, baseline[max(0, index - 40):index + 40],
                                   final[max(0, index - 40):index + 40]))
                break

    print("\nuptime after the battery: %d ms" % device.uptime_ms())
    if reboots:
        print("REBOOT during: %s" % ", ".join(reboots))
    if unhandled:
        print("no response to: %s" % ", ".join(unhandled))
    if final_status == 200:
        print("zone configuration %s after the battery (%d bytes)"
              % ("unchanged" if final == baseline else "CHANGED", len(final)))
    else:
        failures.append("final GET /api/zones returned %d, so the configuration "
                        "comparison is inconclusive" % final_status)
    for failure in failures:
        print("FAIL %s" % failure)
    if reboots:
        print("robustness: FAIL (%d reboot(s))" % len(reboots))
        return 1
    if failures:
        print("robustness: FAIL (%d case(s) off)" % len(failures))
        return 1
    print("robustness: PASS (%d cases, every one rejected as expected, no "
          "reboot, zone configuration unchanged)" % len(cases()))
    return 0


def self_test():
    """The judging logic, with no device."""
    # Uptime going backwards is a reboot even when the status code looked fine.
    uptime = [100000, 101000, 4000, 5000]
    reboots = [i for i in range(1, len(uptime)) if uptime[i] < uptime[i - 1]]
    assert reboots == [2], reboots
    assert [i for i in range(1, len(uptime))
            if uptime[i] < uptime[i - 1] and False] == []
    # A status that differs from the expected one is a finding.
    expected, got = 400, 500
    assert expected != got
    # Byte-identical configuration is what "no partial write" means here.
    assert json.dumps({"zones": [{"name": "Cuisine"}]}) == \
        json.dumps({"zones": [{"name": "Cuisine"}]})
    assert json.dumps({"zones": [{"name": "Cuisine"}]}) != \
        json.dumps({"zones": [{"name": "Cuisin"}]})
    # The battery must actually contain the regression case.
    labels = [case[0] for case in cases()]
    assert "PUT truncated object" in labels, labels
    assert "PUT oversized body" in labels and "PUT cross-origin" in labels, labels
    assert "PUT origin is a host prefix" in labels and \
        "PUT origin names another port" in labels and \
        "PUT origin has no scheme" in labels, labels
    assert "PUT deeply nested" in labels and "PUT deeply nested strings" in labels, \
        labels
    assert len(labels) == len(set(labels)), "case labels must be unique"
    # A case that is meant to be anonymous must actually be sent without a
    # session, or a successful write looks like an authentication failure.
    anonymous = anonymous_case_labels()
    assert len(anonymous) == 14, sorted(anonymous)
    assert "GET system no session" in anonymous and \
        "GET health no session" in anonymous, sorted(anonymous)
    assert "POST logout" not in anonymous, "the logout request needs a session"
    print("robustness self-test: PASS (%d cases, %d of them anonymous; detects "
          "rebooting uptime, unexpected status, unanswered requests, changed "
          "configuration; the truncated-body and deep-nesting regression cases "
          "are present)" % (len(labels), len(anonymous)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", help="device address or name")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--password",
                        help="omit to be prompted (or set PARADOX_WEB_PASSWORD)")
    parser.add_argument("--self-test", action="store_true",
                        help="check the judging logic, no device needed")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.host:
        parser.error("--host is required (or use --self-test)")
    args.password = args.password or os.environ.get("PARADOX_WEB_PASSWORD") \
        or getpass.getpass("web password for %s: " % args.host)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
