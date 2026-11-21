# The head of the ESP8266 as an RBL package (espurna/arch/esp8266/rbl_esp8266.h): what the head writes under the modules'
# directory for the SDK's bootloader to install (bootloader/espurna_rbl.c, the Tuya bootloader's scheme, owner 2026-11-18).
#
# The header of the Beken bootloaders (RT-Thread OTA, ltchiptool soc/bk72xx/util/rbl.py), 96 bytes, then the package:
# the head a sector (4 KB) at a time, each sector its own raw deflate (zlib wbits -12) as a chunk - u16 length, the deflate
# bytes, padding to 4 bytes (owner 20.11: the bootloader swaps the heads sector by sector and keeps the old one compressed
# in the same chunks for a rollback; a power cut resumes at a sector, so no sector depends on the one before).
#
# --flash: the same package of an image of the whole flash from 0x0 (compression 3, ESPURNA_RBL_FLASH): another firmware
# with its own bootloader, e.g. ESPurna 1.15 / 1.22 of the Arduino core (owner 21.11 «пиши 1.15 по воздуху»). The SDK's
# bootloader writes it over itself and resets the chip; no rollback.
#
#   py -3 esp8266_rbl.py <head.bin> <out.rbl> [version]        by hand; the build calls make()
#   py -3 esp8266_rbl.py --flash <firmware.bin> <out.rbl>      an image of the flash (an Arduino firmware .bin)

import struct
import sys
import time
import zlib

MAGIC = b"RBL\x00"
SECTORS = 2         # ESPURNA_RBL_SECTORS of rbl_esp8266.h
FLASH = 3           # ESPURNA_RBL_FLASH: an image of the flash from 0x0
SECTOR = 4096
NAME = b"esp8266"
HEADER = struct.Struct("<4sBBxxI16s24s24sIIII")


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def chunks(head):
    """the package: one padded chunk per sector of the head"""
    out = b""
    for at in range(0, len(head), SECTOR):
        packer = zlib.compressobj(9, zlib.DEFLATED, -12, 9)
        data = packer.compress(head[at:at + SECTOR]) + packer.flush()
        chunk = struct.pack("<H", len(data)) + data
        out += chunk + b"\xff" * (-len(chunk) % 4)
    return out


def unchunk(package):
    """what the bootloader will write: each chunk inflated on its own"""
    out, at = b"", 0
    while at < len(package):
        size = struct.unpack_from("<H", package, at)[0]
        out += zlib.decompress(package[at + 2:at + 2 + size], -12)
        at += (2 + size + 3) & ~3
    return out


def make(head, version="", timestamp=None, flash=False):
    """head: the image bytes -> the RBL bytes (header + package); flash: an image of the flash from 0x0"""
    if flash and head[:1] != b"\xe9":
        raise ValueError("an image of the flash starts with 0xE9 (a gzip'ed .bin is not taken: unpack it)")
    package = chunks(head)
    header = HEADER.pack(MAGIC, 0, FLASH if flash else SECTORS, int(timestamp if timestamp is not None else time.time()),
                         NAME.ljust(16, b"\0"), version.encode()[:23].ljust(24, b"\0"), b"\0" * 24,
                         zlib.crc32(package) & 0xFFFFFFFF, fnv1a(head), len(head), len(package))
    header += struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    assert len(header) == 96
    # the bootloader inflates it again: what it will write must be the head
    assert unchunk(package) == head
    return header + package


def write(head_path, out_path, version="", flash=False):
    with open(head_path, "rb") as f:
        head = f.read()
    rbl = make(head, version, flash=flash)
    with open(out_path, "wb") as f:
        f.write(rbl)
    print(f"  rbl: {out_path}: head {len(head)}, package {len(rbl) - 96} ({(len(rbl) - 96) * 100 // len(head)} %)")
    return out_path


if __name__ == "__main__":
    args = [arg for arg in sys.argv[1:] if arg != "--flash"]
    write(args[0], args[1], args[2] if len(args) > 2 else "", flash="--flash" in sys.argv[1:])
