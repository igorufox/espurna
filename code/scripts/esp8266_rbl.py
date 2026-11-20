# The head of the ESP8266 as an RBL package (espurna/arch/esp8266/rbl_esp8266.h): what the head writes under the modules'
# directory for the SDK's bootloader to install (bootloader/espurna_rbl.c, the Tuya bootloader's scheme, owner 2026-11-18).
#
# The header of the Beken bootloaders (RT-Thread OTA, ltchiptool soc/bk72xx/util/rbl.py), 96 bytes, then the package:
# the head as raw deflate with a 4 KB window (zlib wbits -12; the bootloader's window is one sector of RAM).
#
#   py -3 esp8266_rbl.py <head.bin> <out.rbl> [version]      by hand; the build calls make()

import struct
import sys
import time
import zlib

MAGIC = b"RBL\x00"
DEFLATE = 1
NAME = b"esp8266"
HEADER = struct.Struct("<4sBBxxI16s24s24sIIII")


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def make(head, version="", timestamp=None):
    """head: the image bytes -> the RBL bytes (header + package)"""
    packer = zlib.compressobj(9, zlib.DEFLATED, -12, 9)
    package = packer.compress(head) + packer.flush()
    header = HEADER.pack(MAGIC, 0, DEFLATE, int(timestamp if timestamp is not None else time.time()),
                         NAME.ljust(16, b"\0"), version.encode()[:23].ljust(24, b"\0"), b"\0" * 24,
                         zlib.crc32(package) & 0xFFFFFFFF, fnv1a(head), len(head), len(package))
    header += struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    assert len(header) == 96
    # the bootloader inflates it again: what it will write must be the head
    assert zlib.decompress(package, -12) == head
    return header + package


def write(head_path, out_path, version=""):
    with open(head_path, "rb") as f:
        head = f.read()
    rbl = make(head, version)
    with open(out_path, "wb") as f:
        f.write(rbl)
    print(f"  rbl: {out_path}: head {len(head)}, package {len(rbl) - 96} ({(len(rbl) - 96) * 100 // len(head)} %)")
    return out_path


if __name__ == "__main__":
    write(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "")
