# The head as an RBL package (espurna/libs/rbl.h, the places in arch/<chip>/rbl_<chip>.h): what the head writes under the
# modules' directory for the bootloader to install (arch/rtos/bootloader/espurna_rbl.c, the Tuya bootloader's scheme,
# owner 2026-11-18). ESP8266 and ESP32 (owner 22.11: «делай как ESP8266 у нас общая схема»): the chip's name in the
# header (--name, NAME by default).
#
# The header of the Beken bootloaders (RT-Thread OTA, ltchiptool soc/bk72xx/util/rbl.py), 96 bytes, then the package:
# the head a sector (4 KB) at a time, each sector its own raw deflate (zlib wbits -12) as a chunk - u16 length, the deflate
# bytes, padding to 4 bytes (owner 20.11: the bootloader swaps the heads sector by sector and keeps the old one compressed
# in the same chunks for a rollback; a power cut resumes at a sector, so no sector depends on the one before).
#
# --flash: the same package of an image of the whole flash from 0x0 (compression 3, ESPURNA_RBL_FLASH): another firmware
# with its own bootloader, e.g. ESPurna 1.15 / 1.22 of the Arduino core (owner 21.11 «пиши 1.15 по воздуху»). The SDK's
# bootloader writes it over itself and resets the chip; no rollback. ESP32 the same (owner 22.11: the migration both ways
# on both chips, esp32_migrate.py): its image starts with the empty sector under the bootloader (0xFF, the bootloader at
# 0x1000); the sectors under STORED_BELOW (the bootloader, the partition table, otadata) go in as stored deflate blocks:
# a head under another bootloader (the safeboot layout) takes the bootloader out of the package without an inflater and
# writes it itself (ota_esp32.cpp), the new bootloader then writes the whole image
#
#   py -3 esp8266_rbl.py [--name esp32] <head.bin> <out.rbl> [version]    by hand; the build calls make()
#   py -3 esp8266_rbl.py --flash <firmware.bin> <out.rbl>      an image of the flash (an Arduino firmware .bin)

import struct
import sys
import time
import zlib

MAGIC = b"RBL\x00"
SECTORS = 2         # ESPURNA_RBL_SECTORS of rbl_<chip>.h
FLASH = 3           # ESPURNA_RBL_FLASH: an image of the flash from 0x0
SECTOR = 4096
NAME = b"esp8266"
HEADER = struct.Struct("<4sBBxxI16s24s24sIIII")
# ESP32 images of the flash: the bootloader at 0x1000, the boot sectors stored, not deflated (above)
ESP32_BOOT = 0x1000
STORED_BELOW = 0x10000


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def stored(data):
    """one final stored deflate block (RFC 1951 3.2.4): 0x01, LEN, NLEN, the bytes"""
    return b"\x01" + struct.pack("<HH", len(data), len(data) ^ 0xFFFF) + data


def kept(at, keep):
    """the sector at `at` is one the bootloader leaves as it is (a chunk of length 0, flash images only)"""
    return any(start <= at < start + size for start, size in keep)


def chunks(head, stored_below=0, keep=()):
    """the package: one padded chunk per sector of the head; the sectors under stored_below as stored blocks, the kept
    ones as empty chunks"""
    out = b""
    for at in range(0, len(head), SECTOR):
        if kept(at, keep):
            data = b""
        elif at < stored_below:
            data = stored(head[at:at + SECTOR])
        else:
            packer = zlib.compressobj(9, zlib.DEFLATED, -12, 9)
            data = packer.compress(head[at:at + SECTOR]) + packer.flush()
        chunk = struct.pack("<H", len(data)) + data
        out += chunk + b"\xff" * (-len(chunk) % 4)
    return out


def unchunk(package):
    """what the bootloader will write: each chunk inflated on its own, None for a kept sector"""
    out, at = [], 0
    while at < len(package):
        size = struct.unpack_from("<H", package, at)[0]
        out.append(zlib.decompress(package[at + 2:at + 2 + size], -12) if size else None)
        at += (2 + size + 3) & ~3
    return out


def written(head, keep):
    """the bytes of the image the bootloader writes (and hashes): the kept sectors left out"""
    return b"".join(head[at:at + SECTOR] for at in range(0, len(head), SECTOR) if not kept(at, keep))


def make(head, version="", timestamp=None, flash=False, name=NAME, keep=()):
    """head: the image bytes -> the RBL bytes (header + package); flash: an image of the flash from 0x0; keep: the
    (offset, size) ranges of it the bootloader leaves as they are (sector aligned, flash images only)"""
    if keep and not flash:
        raise ValueError("kept sectors are for an image of the flash only")
    stored_below = 0
    if flash and (name == b"esp32"):
        if (head[ESP32_BOOT:ESP32_BOOT + 1] != b"\xe9") or head[:ESP32_BOOT].strip(b"\xff"):
            raise ValueError("an ESP32 image of the flash: 0xFF under 0x1000, the bootloader (0xE9) at 0x1000")
        stored_below = STORED_BELOW
    elif flash and head[:1] != b"\xe9":
        raise ValueError("an image of the flash starts with 0xE9 (a gzip'ed .bin is not taken: unpack it)")
    package = chunks(head, stored_below, keep)
    header = HEADER.pack(MAGIC, 0, FLASH if flash else SECTORS, int(timestamp if timestamp is not None else time.time()),
                         name.ljust(16, b"\0"), version.encode()[:23].ljust(24, b"\0"), b"\0" * 24,
                         zlib.crc32(package) & 0xFFFFFFFF, fnv1a(written(head, keep)), len(head), len(package))
    header += struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    assert len(header) == 96
    # the bootloader inflates it again: what it will write must be the head
    sectors = unchunk(package)
    assert len(sectors) == (len(head) + SECTOR - 1) // SECTOR
    for index, data in enumerate(sectors):
        at = index * SECTOR
        assert (data is None) == kept(at, keep)
        assert (data is None) or (data == head[at:at + SECTOR])
    return header + package


def write(head_path, out_path, version="", flash=False, name=NAME):
    with open(head_path, "rb") as f:
        head = f.read()
    rbl = make(head, version, flash=flash, name=name)
    with open(out_path, "wb") as f:
        f.write(rbl)
    print(f"  rbl: {out_path}: head {len(head)}, package {len(rbl) - 96} ({(len(rbl) - 96) * 100 // len(head)} %)")
    return out_path


if __name__ == "__main__":
    argv = sys.argv[1:]
    name = NAME
    if "--name" in argv:
        at = argv.index("--name")
        name = argv[at + 1].encode()
        del argv[at:at + 2]
    args = [arg for arg in argv if arg != "--flash"]
    write(args[0], args[1], args[2] if len(args) > 2 else "", flash="--flash" in argv, name=name)
