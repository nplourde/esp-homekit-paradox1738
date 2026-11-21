#!/usr/bin/env python3
"""Read the bridge's record of what it was asked to do, and hold it to account (T9.6).

    make audit HOST=<address> PHASE=boot
    make audit HOST=<address> PHASE=write
    make audit HOST=<address> PHASE=refusals
    make audit HOST=<address> PHASE=restart

The trail itself is firmware logic (apps/paradox1738/main/paradox_audit.c) and its
containment rules are host tested (make -C test/host check TEST=audit). What can only
be checked against a running server is what the web layer chose to write down, so
each phase is a different question about a real request:

  boot      right after a flash or reboot: does the trail begin at the boot and say
            so? A 16-record ring that arrives with no context is indistinguishable
            from a device that has never been asked for anything.
  write     a zone rename through the API: is the write recorded, named as an
            authenticated one, and is the cross-origin attempt at the same write
            recorded next to it? A refusal that is not recorded is indistinguishable
            from a refusal that never happened.
  refusals  after the 62-case robustness battery: are the refusals in the trail, and
            does the document admit that older records aged out? Because the ring is
            16 deep, cases early in the battery are already gone by the time it
            finishes, so this phase also watches one refusal happen live rather than
            trusting the window.
  restart   what the bridge restart control does to the trail. It answers that
            question rather than assuming the answer: the restart is the one action
            the UI can take that erases the record of everything before it.

There is no --self-test here because nothing in this file decides anything about the
device: the judgement lives in paradox_audit.c, and it is host tested there.
"""
import argparse
import http.client
import json
import os
import sys
import time


def request(host, port, method, path, body=None, cookie=None, origin=None,
            ctype="application/json"):
    conn = http.client.HTTPConnection(host, port, timeout=20)
    headers = {}
    if origin:
        headers["Origin"] = origin
    if cookie:
        headers["Cookie"] = cookie
    data = None
    if body is not None:
        data = body if isinstance(body, str) else json.dumps(body)
        headers["Content-Type"] = ctype
    conn.request(method, path, body=data, headers=headers)
    response = conn.getresponse()
    payload = response.read().decode("utf-8", "replace")
    set_cookie = response.getheader("Set-Cookie")
    conn.close()
    return response.status, payload, set_cookie


def login(host, port, password):
    for attempt in range(6):
        status, payload, cookie = request(
            host, port, "POST", "/api/auth/login", {"password": password},
            origin="http://%s" % host)
        if status == 200 and cookie:
            return cookie.split(";")[0]
        if status == 429:
            time.sleep(15)
            continue
        raise RuntimeError("login refused: %s %s" % (status, payload))
    raise RuntimeError("login lockout")


def trail(host, port, cookie):
    status, payload, _ = request(host, port, "GET", "/api/audit", cookie=cookie)
    if status != 200:
        raise RuntimeError("GET /api/audit -> %s %s" % (status, payload))
    document = json.loads(payload)
    return document


def failures():
    return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--password", default=os.environ.get("PARADOX_WEB_PASSWORD", "admin"))
    parser.add_argument("--phase", required=True,
                        choices=["boot", "write", "refusals", "restart"])
    args = parser.parse_args()

    cookie = login(args.host, args.port, args.password)
    problems = []

    if args.phase == "boot":
        document = trail(args.host, args.port, cookie)
        entries = document["entries"]
        print("boot phase: %d entries, next_sequence %s, capacity %s"
              % (len(entries), document["next_sequence"], document["capacity"]))
        for entry in entries:
            print("   #%s %s %-9s %-6s %-22s %s %s" % (
                entry["sequence"], entry["uptime_ms"], entry["principal"],
                entry["method"], entry["path"], entry["status"], entry["note"]))
        oldest = entries[-1] if entries else None
        if oldest is None:
            problems.append("the trail is empty right after a boot")
        elif oldest["sequence"] != 1 or oldest["note"] != "web server started" \
                or oldest["principal"] != "system":
            problems.append("the trail does not begin with the boot record")
        else:
            print("   the trail begins at the boot and says so; the only other "
                  "record is this script's own sign-in")
            for entry in entries[:-1]:
                if entry["note"] != "web session issued":
                    problems.append("unexpected record before anything was asked: %s"
                                    % entry["note"])

    elif args.phase == "write":
        status, payload, _ = request(args.host, args.port, "GET", "/api/zones",
                                     cookie=cookie)
        original = [z for z in json.loads(payload)["zones"] if z["id"] == 16][0]["name"]
        print("zone 16 is called %r today" % original)
        before = trail(args.host, args.port, cookie)["next_sequence"]
        status, payload, _ = request(args.host, args.port, "PUT", "/api/zones/16",
                                     {"name": "Trail Test"}, cookie=cookie,
                                     origin="http://%s" % args.host)
        print("rename: %s %s" % (status, payload))
        if status != 200:
            problems.append("rename failed with %s" % status)
        document = trail(args.host, args.port, cookie)
        newest = document["entries"][0]
        print("   newest record: #%s %s %s %s -> %s (%s)"
              % (newest["sequence"], newest["principal"], newest["method"],
                 newest["path"], newest["status"], newest["note"]))
        if newest["path"] != "/api/zones/16" or newest["status"] != "200":
            problems.append("the write is not in the trail")
        if newest["principal"] != "admin":
            problems.append("an authenticated write recorded as %s" % newest["principal"])
        if document["next_sequence"] <= before:
            problems.append("sequence did not advance")
        # Cross-origin attempt at the same write must be refused AND recorded.
        status, payload, _ = request(args.host, args.port, "PUT", "/api/zones/16",
                                     {"name": "Not Recorded?"}, cookie=cookie,
                                     origin="http://evil.example")
        print("cross-origin rename: %s %s" % (status, payload))
        newest = trail(args.host, args.port, cookie)["entries"][0]
        print("   newest record: %s %s %s -> %s (%s)"
              % (newest["principal"], newest["method"], newest["path"],
                 newest["status"], newest["note"]))
        if status != 403 or newest["status"] != "403":
            problems.append("cross-origin refusal not paired with a record")
        if newest["principal"] != "admin":
            problems.append("a session-carrying cross-origin attempt recorded as %s"
                            % newest["principal"])
        # Put the name back.
        status, payload, _ = request(args.host, args.port, "PUT", "/api/zones/16",
                                     {"name": original}, cookie=cookie,
                                     origin="http://%s" % args.host)
        print("restore: %s" % status)
        if status != 200:
            problems.append("restore failed with %s" % status)

    elif args.phase == "refusals":
        document = trail(args.host, args.port, cookie)
        entries = document["entries"]
        refused = [e for e in entries if e["status"].startswith("4")
                   and e["note"] == "refused"]
        print("refusals phase: %d entries, %d of them 4xx refusals, "
              "next_sequence %s" % (len(entries), len(refused),
                                    document["next_sequence"]))
        for entry in entries:
            print("   #%s %s %-9s %-6s %-22s %s %s" % (
                entry["sequence"], entry["uptime_ms"], entry["principal"],
                entry["method"], entry["path"], entry["status"], entry["note"]))
        if not refused:
            problems.append("the battery produced no refusals in the trail")
        seen = {(e["method"], e["path"], e["status"]) for e in entries}
        wanted = {("GET", "/api/homekit/restart", "405"),
                  ("POST", "/api/homekit/restart", "401")}
        missing = wanted - seen
        if missing:
            problems.append("expected refusals missing from the trail: %s" % missing)
        anonymous = [e for e in entries if e["principal"] == "anonymous"]
        print("   %d records name nobody (anonymous), %d name a session"
              % (len(anonymous), len(entries) - len(anonymous)))
        # The ring is 16 deep, so cases early in a 62-case battery have aged out.
        # Watch a refusal happen instead of assuming it is still in the window: one
        # bad sign-in (the counter was reset by the battery's own successful login,
        # so one failure cannot trip the 5-failure lockout), then sign in properly.
        before = document["next_sequence"]
        bad, _, _ = request(args.host, args.port, "POST", "/api/auth/login",
                            {"password": "definitely-not-the-password"},
                            origin="http://%s" % args.host)
        print("live bad sign-in: %s" % bad)
        newest = trail(args.host, args.port, cookie)["entries"][0]
        print("   recorded: %s POST %s -> %s (%s)"
              % (newest["principal"], newest["path"], newest["status"],
                 newest["note"]))
        if bad != 401 or newest["status"] != "401" or newest["sequence"] < before:
            problems.append("a live refusal was not recorded")
        if newest["principal"] != "anonymous":
            problems.append("a stranger's attempt recorded as %s" % newest["principal"])
        cookie = login(args.host, args.port, args.password)
        newest = trail(args.host, args.port, cookie)["entries"][0]
        if newest["note"] != "web session issued" or newest["status"] != "200":
            problems.append("the successful sign-in after it was not recorded")
        if document["next_sequence"] > len(entries) + 1:
            print("   %d older records aged out of the 16-deep ring, and the "
                  "document says so" % (document["next_sequence"] - 1 - len(entries)))

    elif args.phase == "restart":
        document = trail(args.host, args.port, cookie)
        print("before restart: %d entries, next_sequence %s, newest note %r"
              % (len(document["entries"]), document["next_sequence"],
                 document["entries"][0]["note"]))
        status, payload, _ = request(args.host, args.port, "POST",
                                     "/api/homekit/restart", {"confirm": True},
                                     cookie=cookie, origin="http://%s" % args.host)
        print("restart: %s %s" % (status, payload))
        deadline = time.time() + 180
        while time.time() < deadline:
            time.sleep(5)
            code, _, _ = request(args.host, args.port, "GET", "/api/health")
            if code == 200:
                break
        cookie = login(args.host, args.port, args.password)
        document = trail(args.host, args.port, cookie)
        print("after restart: %d entries, next_sequence %s"
              % (len(document["entries"]), document["next_sequence"]))
        for entry in document["entries"]:
            print("   #%s %s %-9s %-6s %-22s %s %s" % (
                entry["sequence"], entry["uptime_ms"], entry["principal"],
                entry["method"], entry["path"], entry["status"], entry["note"]))

    if problems:
        for problem in problems:
            print("FAIL %s" % problem)
        return 1
    print("%s phase: PASS" % args.phase)
    return 0


if __name__ == "__main__":
    sys.exit(main())
