#!/usr/bin/env python3
"""Build test fixtures for selfcheck: a K93-compatible .ipa (zip), a hostile
.ipa containing a fake ARM Mach-O (must be rejected by the installer), and a
minimal but structurally valid UDZO .dmg (koly trailer + XML plist + mish block
table + one zlib block + one zero block)."""
import base64
import io
import plistlib
import struct
import sys
import zipfile
import zlib
import os

out = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
os.makedirs(out, exist_ok=True)

# ------------------------------------------------------------------ test.ipa
info = {
    "CFBundleName": "Fixture",
    "CFBundleDisplayName": "Fixture App",
    "CFBundleIdentifier": "com.k93.fixture",
    "CFBundleExecutable": "Fixture",
    "CFBundleShortVersionString": "1.0",
    "CFBundleVersion": "1",
}
# a tiny native ELF-looking payload: we ship a real host-independent marker;
# the installer only checks the magic, so a 4-byte stub suffices.
elf_stub = b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 56

with zipfile.ZipFile(f"{out}/test.ipa", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Payload/Fixture.app/", "")
    z.writestr("Payload/Fixture.app/Info.plist", plistlib.dumps(info))
    z.writestr("Payload/Fixture.app/Fixture", elf_stub)
    z.writestr("Payload/Fixture.app/readme.txt", "K93 fixture app payload\n")

# ---------------------------------------------------------------- bad ipa (ARM)
bad = dict(info)
bad["CFBundleName"] = "ArmApp"
bad["CFBundleIdentifier"] = "com.k93.armapp"
bad["CFBundleExecutable"] = "ArmApp"
with zipfile.ZipFile(f"{out}/arm.ipa", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Payload/ArmApp.app/Info.plist", plistlib.dumps(bad))
    # Mach-O arm64 magic (little-endian CF FA ED FE) - must be refused
    z.writestr("Payload/ArmApp.app/ArmApp", b"\xcf\xfa\xed\xfe" + b"\x00" * 60)

# ---------------------------------------------------------------- test.dmg
SECTOR = 512
sectors = 2048                       # 1 MiB logical image
plain = bytearray(sectors * SECTOR)
plain[0:10] = b"K93DMGTEST"
plain[10:100] = b"The quick brown fox " * 5
for s in range(300, 320):            # marker data deeper in the image
    plain[s * SECTOR:s * SECTOR + 16] = b"K93DEEP-%05d" % s

# block layout (as UDZO stores it): one zlib block covering sectors 0..1023,
# one ZERO block covering 1024..2047.
head = bytes(plain[: 1024 * SECTOR])
comp = zlib.compress(head)

def mish_block(typ, sec, cnt, coff, clen):
    return struct.pack(">IIQQQQ", typ, 0, sec, cnt, coff, clen)

mish = struct.pack(">IIQQQII6I", 0x6D697368, 1, 0, sectors, 0, 0, 0, *([0] * 6))
mish += b"\x00" * 132                                # checksum area (UDIF checksum)
mish += b"\x00" * 8                                  # pad to 0xCC block-header size
assert len(mish) == 204, len(mish)
mish += mish_block(0x80000005, 0, 1024, 0, len(comp))
mish += mish_block(0x00000000, 1024, 1024, len(comp), 0)
mish += mish_block(0xFFFFFFFF, 0, 0, 0, 0)

data_fork = comp
mish_b64 = base64.b64encode(mish).decode()
xml_obj = {
    "resource-fork": {
        "blkx": [
            {"Name": "k93-fixture", "Data": mish},
            {"CFBundleIdentifier": "com.apple.diskimage"},
        ]
    }
}
xml = plistlib.dumps(xml_obj, fmt=plistlib.FMT_XML)

# koly trailer
def be32(v): return struct.pack(">I", v)
def be64(v): return struct.pack(">Q", v)
checksum = b"\x00" * 132
koly = b"koly" + be32(4) + be32(512) + be32(1)       # sig, version, hdr size, flags
koly += be64(0)                                       # running data fork offset
koly += be64(0) + be64(len(data_fork))                # data fork offset/length
koly += be64(0) + be64(0)                             # rsrc fork offset/length
koly += be32(1) + be32(1)                             # segment number/count
koly += b"\x00" * 16                                  # segment id
koly += checksum                                      # data checksum
koly += be64(0)                                       # xml offset (filled later)
koly += be64(len(xml))                                # xml length
koly += be32(0) + be32(0)                             # reserved
koly += checksum                                      # xml checksum
koly += be32(1) + be64(sectors)                       # image variant, sector count
koly += be32(0) * 3
koly += b"\x00" * (512 - len(koly))                   # pad to full 512-byte trailer
assert len(koly) == 512, len(koly)
xml_off = len(data_fork)
koly = koly[:212] + be64(xml_off) + koly[220:]

with open(f"{out}/test.dmg", "wb") as f:
    f.write(data_fork)
    f.write(xml)
    f.write(koly)

print(f"fixtures in {out}: test.ipa, arm.ipa, test.dmg")
