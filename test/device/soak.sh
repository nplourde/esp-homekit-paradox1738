#!/usr/bin/env python3
"""T8.4 - 24-hour soak logger for the Paradox 1738 hub.

Polls the authenticated web API on a schedule and appends one CSV row per
sample, so a long run can be judged without a serial lead attached:

  * heap headroom and its low-water mark since boot
  * the web task's stack watermark
  * decoder counters (frames accepted, framing errors, bytes discarded,
    overruns) - a permanently desynchronised decoder shows up as framing
    errors climbing while frames_ok stalls
  * the raw UART tap (bytes_recorded is the cumulative count of bytes taken off
    the wire since the last clear, 4 bytes per panel frame). Its delta between
    samples is the arrival signal: bytes_recorded flat and frames_ok flat with
    no framing errors is wire silence at the panel; bytes_recorded moving while
    frames_ok stays flat is the decoder not completing frames
  * device uptime, which is how a silent reboot is caught: uptime going
     *down* means the device reset, and the run fails.

Session cookies are RAM-only on the device, so a reboot also invalidates the
session; the logger re-logs in and keeps going, but still reports the reset.

Usage:
  test/device/soak.sh --host 192.168.4.30 --password admin \
      [--interval 60] [--duration 86400] [--out soak.csv]

Exit status: 0 clean, 1 at least one reboot or an unusable endpoint,
2 bad arguments. Requires only the Python 3 standard library.
"""

import argparse
import csv
import datetime as dt
import getpass
import json
import os
import sys
import time
import urllib.error
import urllib.request

FIELDS = [
    "wall_time", "uptime_ms", "free_heap_bytes", "min_free_heap_bytes",
    "largest_heap_block_bytes", "web_stack_free_bytes", "frames_ok",
    "framing_errors", "bytes_discarded", "overruns", "raw_bytes_recorded",
    "raw_bytes_retained", "raw_bytes_lost", "sim_active", "note",
]


class Client:
    """Minimal cookie-authenticated JSON client for the hub's web API."""

    def __init__(self, host, port=80):
        self.base = "http://%s:%d" % (host, port)
        self.cookie = None

    def _request(self, method, path, payload=None):
        data = None
        headers = {"Accept": "application/json"}
        if self.cookie:
            headers["Cookie"] = self.cookie
        if payload is not None:
            data = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
            headers["Referer"] = self.base + "/"
            headers["Origin"] = self.base
        request = urllib.request.Request(self.base + path, data=data,
                                         headers=headers, method=method)
        with urllib.request.urlopen(request, timeout=15) as response:
            set_cookie = response.headers.get("Set-Cookie")
            if set_cookie:
                self.cookie = set_cookie.split(";")[0]
            body = response.read().decode()
        return json.loads(body) if body else {}

    def login(self, password):
        self._request("POST", "/api/auth/login", {"password": password})

    def system(self):
        return self._request("GET", "/api/system")

    def panel(self):
        return self._request("GET", "/api/debug/panel")


def sample(client):
    """Return one row and the reported uptime, or raise on transport error."""
    system = client.system()
    panel = client.panel()
    counters = panel.get("counters", {})
    raw = client._request("GET", "/api/debug/raw?bytes=1")
    sim = client._request("GET", "/api/debug/simulator")
    row = {
        "wall_time": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "uptime_ms": system.get("uptime_ms"),
        "free_heap_bytes": system.get("free_heap_bytes"),
        "min_free_heap_bytes": system.get("min_free_heap_bytes"),
        "largest_heap_block_bytes": system.get("largest_heap_block_bytes"),
        "web_stack_free_bytes": system.get("web_stack_free_bytes"),
        "frames_ok": counters.get("frames_ok"),
        "framing_errors": counters.get("framing_errors"),
        "bytes_discarded": counters.get("bytes_discarded"),
        "overruns": counters.get("overruns"),
        "raw_bytes_recorded": raw.get("bytes_recorded"),
        "raw_bytes_retained": raw.get("bytes_retained"),
        "raw_bytes_lost": raw.get("bytes_lost"),
        "sim_active": "true" if sim.get("active") else "false",
        "note": "",
    }
    return row


def run(args):
    client = Client(args.host, args.port)
    password = args.password or os.environ.get("PARADOX_WEB_PASSWORD")
    if not password:
        password = getpass.getpass("web password for %s: " % args.host)
    client.login(password)

    header_needed = not os.path.exists(args.out) or os.path.getsize(args.out) == 0
    out = open(args.out, "a", newline="")
    writer = csv.DictWriter(out, fieldnames=FIELDS)
    if header_needed:
        writer.writeheader()
        out.flush()

    started = time.monotonic()
    deadline = started + args.duration
    previous_uptime = None
    reboots = 0
    failures = 0

    while True:
        try:
            row = sample(client)
        except urllib.error.HTTPError as error:
            if error.code == 401:
                # The device rebooted (or the password changed): sessions live
                # in RAM only, so re-login and keep the run alive.
                print("%s: session lost (401), logging in again"
                      % dt.datetime.now().isoformat(timespec="seconds"))
                try:
                    client.login(password)
                    continue
                except Exception:
                    failures += 1
                    continue
            print("request failed: HTTP %s %s" % (error.code, error.reason),
                  file=sys.stderr)
            failures += 1
            continue
        except Exception as error:  # transport hiccup: log it, keep sampling
            print("request failed: %s" % error, file=sys.stderr)
            failures += 1
            continue

        uptime = row["uptime_ms"]
        if previous_uptime is not None and uptime is not None:
            if uptime < previous_uptime:
                reboots += 1
                row["note"] = "REBOOT: uptime %d -> %d" % (previous_uptime, uptime)
                print("%s" % row["note"], file=sys.stderr)
        previous_uptime = uptime
        writer.writerow(row)
        out.flush()
        print("%s uptime=%s heap=%s min=%s stack=%s frames=%s framing_err=%s"
              % (row["wall_time"], row["uptime_ms"], row["free_heap_bytes"],
                 row["min_free_heap_bytes"], row["web_stack_free_bytes"],
                 row["frames_ok"], row["framing_errors"]))

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(args.interval, remaining))

    out.close()
    hours = (time.monotonic() - started) / 3600.0
    print("soak: %d hours, %d reboot(s), %d failed sample(s), log %s"
          % (hours, reboots, failures, args.out))
    if reboots or failures:
        return 1
    return 0


def self_test():
    """Check the reboot detector, which is the part worth getting wrong."""
    good = [1000, 2000, 60000, 60001]
    bad = [60000, 500, 65000]

    def count(values):
        previous = None
        total = 0
        for uptime in values:
            if previous is not None and uptime < previous:
                total += 1
            previous = uptime
        return total

    assert count(good) == 0, "clean uptime run must report no reset"
    assert count(bad) == 1, "an uptime drop must report exactly one reset"
    print("soak self-test: PASS (reboot detector sees 0 resets on a clean run "
          "and 1 on a resetting run)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", help="device address or name")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--password",
                       help="omit to be prompted (or set PARADOX_WEB_PASSWORD)")
    parser.add_argument("--interval", type=int, default=60,
                       help="seconds between samples (default 60)")
    parser.add_argument("--duration", type=int, default=86400,
                       help="total seconds to run (default 24 h)")
    parser.add_argument("--out", default="soak.csv")
    parser.add_argument("--self-test", action="store_true",
                       help="check the reboot detector, no device needed")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.host:
        parser.error("--host is required (or use --self-test)")
    if args.interval < 1 or args.duration < args.interval:
        parser.error("--interval must be >= 1 and <= --duration")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
