#!/usr/bin/env python3
"""Measure panel-to-bridge push latency over the web API (T6.4).

Opens the two server-sent-event streams at once and stamps each one with the
client's wall clock:

  * /api/debug/raw/stream  - the panel UART bytes as the device reads them
  * /api/status/stream     - the zone/fire state the bridge publishes

A physical trigger on the alarm panel therefore shows up twice: first as raw
bytes arriving, then as a status push. The gap between the two is the bridge's
own pipeline latency (UART read -> decode -> state model -> SSE write), which is
what T6.4 asks to keep under two seconds. The time between the panel actually
changing and the bytes arriving is panel-side and cannot be measured from this
side of the wire, so the script reports what it can attribute and says so.

Note on sessions: the device holds exactly one web session, so a second client
signing in (another browser tab, a soak run) invalidates this one. The streams
are reopened after re-logging in, and the run says how often that happened.

Usage:
  test/device/latency.sh --host 192.168.4.30 [--duration 120] [--out latency.csv]

Exit status: 0 if every measured pair was within the 2 s budget, 1 if the run
caught no state change to measure (trigger a zone on the panel) or a pair was
over budget, 2 on bad arguments. Needs only the Python 3 standard library.
"""

import argparse
import csv
import datetime as dt
import getpass
import http.client
import json
import os
import select
import sys
import threading
import time
import urllib.error
import urllib.request

LATENCY_BUDGET_S = 2.0


class Client:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.cookie = None
        self.password = None

    def _request(self, method, path, payload=None):
        data = None
        headers = {"Accept": "application/json"}
        if self.cookie:
            headers["Cookie"] = self.cookie
        if payload is not None:
            data = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
            headers["Referer"] = "http://%s:%d/" % (self.host, self.port)
            headers["Origin"] = "http://%s:%d" % (self.host, self.port)
        request = urllib.request.Request(
            "http://%s:%d%s" % (self.host, self.port, path), data=data,
            headers=headers, method=method)
        with urllib.request.urlopen(request, timeout=15) as response:
            set_cookie = response.headers.get("Set-Cookie")
            if set_cookie:
                self.cookie = set_cookie.split(";")[0]
            body = response.read().decode()
        return json.loads(body) if body else {}

    def login(self, password):
        self.password = password
        self._request("POST", "/api/auth/login", {"password": password})

    def stream(self, path, stop):
        """Yield (event_name, data) for every SSE payload until stop is set.

        select() rather than a socket timeout: an idle stream is normal - the
        panel goes quiet between changes - and readline() on a socket that just
        timed out leaves the reader in an indeterminate state. The response
        timeout has to exceed the stream's 15 s keepalive: with the panel idle,
        the first bytes the device sends for /api/debug/raw/stream are that
        keepalive, and a shorter timeout aborts before the headers arrive.
        """
        connection = http.client.HTTPConnection(self.host, self.port, timeout=45)
        connection.request("GET", path, headers={
            "Accept": "text/event-stream", "Cookie": self.cookie})
        response = connection.getresponse()
        if response.status != 200:
            connection.close()
            raise RuntimeError("%s answered HTTP %d" % (path, response.status))
        event = None
        try:
            while not stop.is_set():
                ready, _, _ = select.select([connection.sock], [], [], 2.0)
                if not ready:
                    continue
                line = response.fp.readline()
                if not line:
                    break
                line = line.decode(errors="replace").rstrip("\r\n")
                if line.startswith("event:"):
                    event = line.split(":", 1)[1].strip()
                elif line.startswith("data:"):
                    yield event or "message", line.split(":", 1)[1].strip()
        finally:
            connection.close()


def zone_signature(status):
    """Compact, comparable summary of what a status push carries."""
    zones = status.get("zones", [])
    active = tuple(z["id"] for z in zones if z.get("open") or z.get("alarm"))
    tampered = tuple(z["id"] for z in zones if z.get("tampered"))
    fire = status.get("fire", {})
    return (active, tampered, bool(fire.get("alarm")))


def pair_latency(events):
    """For each status push whose content changed, pair it with the newest raw
    frame that arrived before it. Returns (latency_s, raw_hex, description)."""
    pairs = []
    previous_signature = None
    raw = []          # (monotonic_time, hex)
    for _wall, monotonic, kind, payload in events:
        if kind == "raw":
            try:
                hex_text = json.loads(payload).get("hex", "")
            except ValueError:
                hex_text = ""
            if hex_text:
                raw.append((monotonic, hex_text))
            continue
        if kind != "status":
            continue
        try:
            status = json.loads(payload)
        except ValueError:
            continue
        signature = zone_signature(status)
        if signature == previous_signature:
            continue
        previous_signature = signature
        frame = None
        for when, hex_text in raw:
            if when <= monotonic:
                frame = (when, hex_text)
        if frame is None:
            continue
        opened = ",".join(str(zone) for zone in signature[0]) or "none"
        tampered = ",".join(str(zone) for zone in signature[1]) or "none"
        description = "open/alarm zones: %s, tamper: %s, fire: %s" % (
            opened, tampered, "ALARM" if signature[2] else "clear")
        pairs.append((monotonic - frame[0], frame[1], description))
    return pairs


def run(args):
    client = Client(args.host, args.port)
    password = args.password or os.environ.get("PARADOX_WEB_PASSWORD")
    if not password:
        password = getpass.getpass("web password for %s: " % args.host)
    client.login(password)

    status = client._request("GET", "/api/status")
    print("initial state: open/alarm zones %s, fire alarm %s"
          % ([z["id"] for z in status.get("zones", [])
              if z.get("open") or z.get("alarm")],
             status.get("fire", {}).get("alarm")))
    sys.stdout.flush()
    print("Trigger a zone on the panel now (open, then close, a door); "
          "capturing %d s." % args.duration)
    sys.stdout.flush()

    stop = threading.Event()
    events = []
    lock = threading.Lock()
    reopened = {}
    last_signature = [None]

    def pump(path, kind):
        failures = 0
        while not stop.is_set():
            try:
                for event_name, payload in client.stream(path, stop):
                    failures = 0
                    if event_name == "error":
                        print("%s sent event: error (%s)" % (path, payload),
                              file=sys.stderr)
                        continue
                    with lock:
                        events.append((dt.datetime.now().astimezone()
                                       .isoformat(timespec="milliseconds"),
                                       time.monotonic(), kind, payload))
                    # Live feedback, so an operator can tell whether to trigger
                    # again before the run ends.
                    if kind == "raw":
                        try:
                            hex_text = json.loads(payload).get("hex", "")
                        except ValueError:
                            hex_text = "<unparseable>"
                        print("[raw    ] %s" % hex_text)
                        sys.stdout.flush()
                    else:
                        try:
                            signature = zone_signature(json.loads(payload))
                        except ValueError:
                            continue
                        with lock:
                            changed = signature != last_signature[0]
                            if changed:
                                last_signature[0] = signature
                        if changed:
                            print("[status ] open/alarm zones %s, tamper %s, "
                                  "fire %s"
                                  % (list(signature[0]) or "-",
                                     list(signature[1]) or "-",
                                     "ALARM" if signature[2] else "clear"))
                            sys.stdout.flush()
                if stop.is_set():
                    break
                # Stream closed by the device (idle timeout): reopen.
                continue
            except Exception as error:
                if "HTTP 401" in str(error):
                    client.login(password)
                    reopened[path] = reopened.get(path, 0) + 1
                    continue
                failures += 1
                print("stream %s failed (%d/8): %s" % (path, failures, error),
                      file=sys.stderr)
                sys.stdout.flush()
                if failures >= 8:
                    return
                time.sleep(2.0)

    threads = [
        threading.Thread(target=pump,
                         args=("/api/debug/raw/stream", "raw"), daemon=True),
        threading.Thread(target=pump,
                         args=("/api/status/stream", "status"), daemon=True),
    ]
    for thread in threads:
        thread.start()
    time.sleep(args.duration)
    stop.set()
    for thread in threads:
        thread.join(timeout=15)

    events.sort(key=lambda item: item[1])
    out = open(args.out, "w", newline="")
    writer = csv.writer(out)
    writer.writerow(["wall_time", "stream", "payload"])
    for wall, _monotonic, kind, payload in events:
        writer.writerow([wall, kind, payload])
    out.close()

    raw_count = sum(1 for event in events if event[2] == "raw")
    status_count = sum(1 for event in events if event[2] == "status")
    print("captured %d raw frame event(s) and %d status push(es) -> %s"
          % (raw_count, status_count, args.out))
    for path, count in reopened.items():
        print("%s was reopened %d time(s): another client took the single "
              "web session" % (path, count))

    pairs = pair_latency(events)
    if not pairs:
        print("latency: no raw-then-status pair to measure. Trigger a real "
              "zone on the panel - the panel goes quiet when nothing changes, "
              "so an idle run proves nothing.")
        return 1
    latencies = [latency for latency, _hex, _description in pairs]
    for latency, hex_text, description in pairs:
        verdict = "within" if latency <= LATENCY_BUDGET_S else "OVER"
        print("  %6.3f s (%s %.1f s budget)  bytes %s -> %s"
              % (latency, verdict, LATENCY_BUDGET_S, hex_text, description))
    print("latency: %d pair(s), max %.3f s, mean %.3f s"
          % (len(pairs), max(latencies), sum(latencies) / len(pairs)))
    return 0 if max(latencies) <= LATENCY_BUDGET_S else 1


def self_test():
    """The pairing maths, runnable with no device."""
    def event(seconds, kind, payload):
        return ("t%.1f" % seconds, seconds, kind, payload)

    events = [
        event(0.0, "status", json.dumps({"zones": [{"id": 1, "open": False}],
                                         "fire": {"alarm": False}})),
        event(1.0, "raw", '{"seq":1,"dropped":0,"hex":"04119a30"}'),
        event(1.6, "status", json.dumps({"zones": [{"id": 1, "open": True}],
                                         "fire": {"alarm": False}})),
        event(2.0, "raw", '{"seq":5,"dropped":0,"hex":"00119a30"}'),
        event(2.4, "status", json.dumps({"zones": [{"id": 1, "open": True}],
                                         "fire": {"alarm": False}})),
        event(3.5, "status", json.dumps({"zones": [{"id": 1, "open": False}],
                                         "fire": {"alarm": False}})),
    ]
    pairs = pair_latency(events)
    assert len(pairs) == 2, "expected two state changes, got %d" % len(pairs)
    assert abs(pairs[0][0] - 0.6) < 1e-9, pairs[0]
    assert abs(pairs[1][0] - 1.5) < 1e-9, pairs[1]
    assert pairs[0][1] == "04119a30" and pairs[1][1] == "00119a30", pairs
    assert "open/alarm zones: 1" in pairs[0][2], pairs[0]
    assert pair_latency([]) == [], "no events must yield no pairs"
    assert len(pair_latency([e for e in events if e[2] == "status"])) == 0, \
        "status pushes with no preceding bytes must be skipped"
    print("latency self-test: PASS (pairs each state change with the newest "
          "preceding frame; recovered 0.600 s and 1.500 s)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", help="device address or name")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--password",
                        help="omit to be prompted (or set PARADOX_WEB_PASSWORD)")
    parser.add_argument("--duration", type=int, default=120)
    parser.add_argument("--out", default="latency.csv")
    parser.add_argument("--self-test", action="store_true",
                        help="check the pairing maths, no device needed")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.host:
        parser.error("--host is required (or use --self-test)")
    if not 5 <= args.duration <= 3600:
        parser.error("--duration must be between 5 and 3600 seconds")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
