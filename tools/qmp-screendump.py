#!/usr/bin/env python3
"""Headless graphics validation for K93OS-x86.

Connects to a QEMU QMP unix socket, takes a screendump of the emulated
VGA output (what a viewer on the VNC port would see), and verifies the
frame contains rendered pixels (i.e. the guest framebuffer stack drew
something). Usage: qmp-screendump.py <qmp-socket> <out.ppm>
Exit 0 = rendered pixels found."""
import json
import socket
import sys


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
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sys.argv[1])
    greet = qmp_read(s)                      # {"QMP": {...}}
    if "QMP" not in greet:
        raise SystemExit("qmp: bad greeting")
    qmp_cmd(s, "qmp_capabilities")
    out = sys.argv[2]
    rec = qmp_cmd(s, "screendump", {"filename": out})
    if "error" in rec:
        print("screendump failed:", rec["error"])
        return 1
    with open(out, "rb") as f:
        magic = f.readline().strip()         # P6
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        maxv = int(f.readline())
        data = f.read()
    nonblack = 0
    for i in range(0, min(len(data), w * h * 3), 3):
        if data[i] or data[i + 1] or data[i + 2]:
            nonblack += 1
    print(f"screendump {magic.decode()} {w}x{h} max={maxv} nonblack_px={nonblack}")
    return 0 if nonblack > 500 else 2


if __name__ == "__main__":
    sys.exit(main())
