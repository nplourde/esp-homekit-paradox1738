#!/usr/bin/env python3
"""
T0.3 - device smoke test.

Flashes (opt-in), hard-resets the ESP32 through the CP2102 auto-reset circuit,
then captures the boot log and fails on any crash marker.

    test/device/smoke.sh                     # reset + capture only (safe)
    test/device/smoke.sh --flash             # build+flash first, then capture
    test/device/smoke.sh --port /dev/ttyUSB1 --seconds 20

Exit status:
    0  every required marker seen and no crash markers
    1  a required marker was missing, or a crash marker appeared
    2  usage / setup error (port busy, missing idf.py, ...)
"""

import argparse
import fcntl
import os
import re
import struct
import subprocess
import sys
import termios
import time

# Markers that must appear in the boot log.
REQUIRED = [
    ("Returned from app_main()", "firmware reached the end of app_main()"),
    ("HAP Started", "HomeKit accessory server started"),
]

# Any hit here is a hard failure. Kept narrow on purpose: benign E() lines from
# the Wi-Fi stack are common and must not make the smoke test flap.
CRASH = [
    "Guru Meditation",
    "abort()",
    "Stack canary",
    "LoadProhibited",
    "StoreProhibited",
    "FetchInvalid",
    "IllegalInstruction",
    "assert failed",
    "Brownout detector was triggered",
]

# ESP-IDF error-level log lines: "E (1234) tag: message". A healthy boot has
# none, so any hit is a failure. --allow-errors downgrades these to warnings for
# runs where a known-benign error is expected (e.g. a Wi-Fi retry).
ERROR_LINE = re.compile(r"^E \(\s*\d+\) \S+")

TIOCM_DTR = 0x002
TIOCM_RTS = 0x004


def modem(fd, dtr=None, rts=None):
    """Set the DTR/RTS modem control lines (True == asserted)."""
    s = struct.unpack("I", fcntl.ioctl(fd, termios.TIOCMGET, struct.pack("I", 0)))[0]
    if dtr is not None:
        if dtr:
            s |= TIOCM_DTR
        else:
            s &= ~TIOCM_DTR
    if rts is not None:
        if rts:
            s |= TIOCM_RTS
        else:
            s &= ~TIOCM_RTS
    fcntl.ioctl(fd, termios.TIOCMSET, struct.pack("I", s))


def hard_reset(fd):
    """Pulse EN low then high. Both lines end de-asserted so the chip runs and
    the serial read is not held in reset or parked in the bootloader."""
    modem(fd, dtr=False, rts=True)  # EN=low: chip in reset
    time.sleep(0.1)
    modem(fd, dtr=False, rts=False)  # EN=high: out of reset, run mode


def raw_mode(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                      # input: no CR/NL translation, no parity
    attrs[1] = 0                      # output: raw
    attrs[3] = 0                      # local: no echo, no signals
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def set_baud(fd, baud):
    speed = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
             57600: termios.B57600, 115200: termios.B115200,
             230400: termios.B230400, 921600: termios.B921600}.get(baud)
    if speed is None:
        die(f"unsupported baud rate {baud}")
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def die(msg, code=2):
    print(f"smoke: error: {msg}", file=sys.stderr)
    sys.exit(code)


def analyze(log, allow_errors=False):
    """Return the list of problems found in a boot log ([] == healthy)."""
    failures = []
    for needle, why in REQUIRED:
        if needle not in log:
            failures.append(f"missing marker {needle!r} ({why})")

    crash_re = re.compile("|".join(re.escape(c) for c in CRASH))
    hits = [line.strip() for line in log.splitlines() if crash_re.search(line)]
    failures += [f"crash marker: {line}" for line in hits]

    errors = [line.strip() for line in log.splitlines() if ERROR_LINE.match(line)]
    if errors:
        msg = f"{len(errors)} error-level log line(s), first: {errors[0]}"
        if allow_errors:
            print(f"smoke: warning: {msg}")
        else:
            failures.append(msg)
    return failures


def self_test():
    """Validate the log analyzer without hardware (the failure path of T0.3)."""
    good = ("I (840) main_task: Returned from app_main()\n"
            "HAP Started\n"
            "I (4690) app_wifi: Connected with IP Address:192.168.4.30\n")
    crash = ("I (840) main_task: Returned from app_main()\n"
             "Guru Meditation Error: Core  0 panic'ed (LoadProhibited).\n")
    truncated = "I (10) cpu_start: Pro cpu start user code\n"
    errored = ("I (840) main_task: Returned from app_main()\n"
               "HAP Started\n"
               "E (2211) wifi: wifi station is not connected\n")

    cases = [
        ("healthy log", good, {}, 0),
        ("crashing log", crash, {}, 1),
        ("truncated log", truncated, {}, 1),
        ("error-level lines", errored, {}, 1),
        ("error-level lines, allowed", errored, {"allow_errors": True}, 0),
    ]
    bad = 0
    for name, log, kw, want_fail in cases:
        problems = analyze(log, **kw)
        ok = bool(problems) == bool(want_fail)
        print(f"self-test {name}: {'ok' if ok else 'WRONG'} ({len(problems)} problem(s))")
        if not ok:
            bad += 1
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--flash", action="store_true",
                    help="run idf.py flash first (overwrites the running firmware)")
    ap.add_argument("--app", default=None, help="app dir to flash (default: repo apps/paradox1738)")
    ap.add_argument("--self-test", action="store_true",
                    help="only check the log analyzer, no hardware needed")
    ap.add_argument("--allow-errors", action="store_true",
                    help="warn instead of fail on error-level log lines")
    args = ap.parse_args()

    if args.self_test:
        sys.exit(self_test())

    if not os.path.exists(args.port):
        die(f"{args.port} does not exist")

    if args.flash:
        app = args.app or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "..", "..", "apps", "paradox1738")
        cmd = ["idf.py", "-p", args.port, "flash"]
        print(f"smoke: {' '.join(cmd)}")
        if subprocess.call(cmd, cwd=app) != 0:
            die("idf.py flash failed")

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        raw_mode(fd)
        set_baud(fd, args.baud)
        # De-assert first in case a previous tool left the chip held in reset.
        modem(fd, dtr=False, rts=False)
        time.sleep(0.2)
        hard_reset(fd)

        deadline = time.monotonic() + args.seconds
        chunks = []
        while time.monotonic() < deadline:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.05)
                continue
            if not data:
                time.sleep(0.05)
                continue
            chunks.append(data)
    finally:
        os.close(fd)

    log = b"".join(chunks).decode("utf-8", "replace")
    print("---- boot log ----")
    print(log)
    print("---- end log ----")

    if not log.strip():
        die("no output on the serial port: check wiring, port, and that the board is powered", 1)

    failures = analyze(log, allow_errors=args.allow_errors)
    if failures:
        print(f"smoke: FAIL ({len(failures)} problem(s))")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)

    print(f"smoke: PASS ({len(log.splitlines())} log lines, no crash markers, "
          f"no error-level lines, all required markers present)")


if __name__ == "__main__":
    main()
