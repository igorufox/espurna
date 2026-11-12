# The install image of a chip, espurna-<chip>.ota: the head (Beken: both, one per bootloader) and the modules file (EMC2)
# in one file. Its name keeps it off a serial flash tool; the MODULES panel takes it apart (html/src/modules.mjs, otaParts):
# a head of another build goes on first (/upgrade, the ESP32 through its safeboot), then the modules file to
# /modules/apply with the ticked names.
#
#   "EOT1", u32 count, then count entries: u32 kind (1 head, 2 modules file), char name[16] (head: esp8266, esp32,
#   tuya, quectel; modules: modules), u32 offset, u32 size, u32 crc32; then the parts, each at its offset

import os
import struct
import zlib

MAGIC = b"EOT1"
KIND_HEAD = 1
KIND_MODULES = 2
ENTRY = struct.Struct("<I16sIII")


def build(out, heads, modules):
    """heads: [(name, path)], modules: path of the modules file -> out"""
    parts = [(KIND_HEAD, name, path) for name, path in heads] + [(KIND_MODULES, "modules", modules)]
    blobs = []
    for kind, name, path in parts:
        with open(path, "rb") as f:
            blobs.append((kind, name, f.read()))

    offset = 8 + ENTRY.size * len(blobs)
    table = b""
    for kind, name, data in blobs:
        table += ENTRY.pack(kind, name.encode()[:15], offset, len(data), zlib.crc32(data) & 0xFFFFFFFF)
        offset += len(data)

    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<I", len(blobs)) + table)
        for _, _, data in blobs:
            f.write(data)

    summary = ", ".join(f"{name} {len(data)}" for _, name, data in blobs)
    print(f"  ota: {os.path.basename(out)}: {summary}")
    return out


def read(data):
    """[(kind, name, bytes)] of an install image, the CRC of every part checked"""
    if data[:4] != MAGIC:
        raise ValueError("not an install image (EOT1)")
    count = struct.unpack_from("<I", data, 4)[0]
    out = []
    for index in range(count):
        kind, name, offset, size, crc = ENTRY.unpack_from(data, 8 + ENTRY.size * index)
        part = data[offset: offset + size]
        if (len(part) != size) or ((zlib.crc32(part) & 0xFFFFFFFF) != crc):
            raise ValueError(f"part {index}: size or crc")
        out.append((kind, name.rstrip(b"\0").decode(), part))
    return out
