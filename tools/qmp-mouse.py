#!/usr/bin/env python3
"""QEMU mouse + super-key injector for K93OS GUI drive-by tests.

usage: qmp-mouse.py <qmp-socket> <command> [args]
  move DX DY              relative cursor motion (may be negative)
  click left|right|middle press+release one button
  scroll N                wheel detents; +down / -up
  keydown KEY             hold a key down (qcode: meta-l, meta-r, ...)
  keyup KEY               release it
  tap KEY                 press+release
  hold KEY SECS           press, wait, release (super-key multitasking test)"""
import json
import socket
import sys
import time


def qmp_read(sock):
    line = b""
    while not line.endswith(b"\n"):
        c = sock.recv(1)
        if not c:
            raise SystemExit("qmp: connection closed")
        line += c
    return json.loads(line)


def qmp_cmd(sock, execute, args=None):
    msg = {"execute": execute}
    if args:
        msg["arguments"] = args
    sock.sendall((json.dumps(msg) + "\n").encode())
    while True:
        rec = qmp_read(sock)
        if "return" in rec or "error" in rec:
            return rec


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sys.argv[1])
    if "QMP" not in qmp_read(s):
        raise SystemExit("qmp: bad greeting")
    qmp_cmd(s, "qmp_capabilities")
    cmd = sys.argv[2]
    a = sys.argv[3:]
    if cmd == "move":
        dx, dy = int(a[0]), int(a[1])
        ev = []
        if dx:
            ev.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            ev.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    elif cmd == "click":
        # usb-mouse holds buttons as levels: press+release inside one QMP
        # call collapse before the next HID poll and the click is lost.
        rec = qmp_cmd(s, "input-send-event", {"events":
            [{"type": "btn", "data": {"down": True, "button": a[0]}}]})
        if "error" in rec:
            print("input-send-event failed:", rec["error"])
            return 1
        time.sleep(0.08)
        ev = [{"type": "btn", "data": {"down": False, "button": a[0]}}]
    elif cmd == "scroll":
        # QEMU >= 10: wheel detents are wheel-up / wheel-down buttons
        btn = "wheel-down" if int(a[0]) > 0 else "wheel-up"
        ev = []
        for _ in range(abs(int(a[0]))):
            ev += [{"type": "btn", "data": {"down": True, "button": btn}},
                   {"type": "btn", "data": {"down": False, "button": btn}}]
    elif cmd in ("keydown", "keyup"):
        ev = [{"type": "key", "data": {"key": {"type": "qcode", "data": a[0]},
                                       "down": cmd == "keydown"}}]
    elif cmd == "tap":
        ev = [{"type": "key", "data": {"key": {"type": "qcode", "data": a[0]},
                                       "down": True}},
              {"type": "key", "data": {"key": {"type": "qcode", "data": a[0]},
                                       "down": False}}]
    elif cmd == "hold":
        rec = qmp_cmd(s, "input-send-event", {"events":
            [{"type": "key", "data": {"key": {"type": "qcode", "data": a[0]},
                                      "down": True}}]})
        if "error" in rec:
            print("input-send-event failed:", rec["error"])
            return 1
        time.sleep(float(a[1]))
        ev = [{"type": "key", "data": {"key": {"type": "qcode", "data": a[0]},
                                       "down": False}}]
    else:
        sys.exit(__doc__)
    rec = qmp_cmd(s, "input-send-event", {"events": ev})
    if "error" in rec:
        print("input-send-event failed:", rec["error"])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
