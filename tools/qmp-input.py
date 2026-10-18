#!/usr/bin/env python3
"""Send QEMU key events for K93OS GUI drive-by tests.

usage: qmp-input.py <qmp-socket> <qcode> [<qcode> ...]
qcodes are QEMU key names: up down left right ret esc sp tab f12 ...
One send-key per qcode, 0.15s apart (guest UI polls at 250ms)."""
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
    for qc in sys.argv[2:]:
        keys = [{"type": "qcode", "data": part} for part in qc.split("+")]
        rec = qmp_cmd(s, "send-key", {"keys": keys})
        if "error" in rec:
            print(f"send-key {qc} failed:", rec["error"])
            return 1
        time.sleep(0.15)
    return 0


if __name__ == "__main__":
    sys.exit(main())
