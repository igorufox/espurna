# PROTOTYPE (docs/modules.md): the initial image of the head + modules build, the same on every chip.
#
# The initial image is the head and, at the start of the module area (arch::area().lo), a modules file (EMC2, the
# format of module_link.py write_container) of the modules on out of the box: MODULES_ON_BY_DEFAULT of
# espurna/config/head.h (the captive portal). Nothing else: no module placed, no directory. On the first boot of a head
# with no directory of its own the board finds EMC2 at area.lo and installs every package of it by its normal Apply
# (modules.cpp applyInitial), then erases the file. Every other module comes from the install image (.ota) or the
# modules file by the Apply of the MODULES panel.
#
# Where the area starts, per chip (what the flash layout forces):
#   ESP8266  roundUp(head size, 4096): right behind the head in espurna-esp8266.bin (arch/esp8266/modules_esp8266.cpp headEnd())
#   ESP32    roundUp(app0 + app image size, 0x10000) in the merged espurna-esp32.bin (modules_esp32.cpp findArea())
#   Beken    the OTA 'download' partition + 4096: not behind the head, a separate run of the UF2 written by the flasher
#            (beken_module_link.py, beken_quectel_image.py: `<file>+0x1000=flasher:download`)
#
# Used by the chip link scripts; no SCons here (beken_quectel_image.py is a plain library as well).

import os
import re
import struct
import sys

# loaded by a chip's link script or by beken_quectel_image.py: the shared scripts sit next to this file
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from module_link import CONTAINER_ENTRY, CONTAINER_MAGIC, PACKAGE, PACKAGE_MAGIC, PACKAGE_VERSION, \
    round_up  # noqa: E402

SECTOR = 4096


def modules_on(project_dir):
    """the modules on out of the box, one list for every chip: MODULES_ON_BY_DEFAULT "name ..." of
    espurna/config/head.h (none when it is not there)"""
    try:
        with open(os.path.join(project_dir, "espurna", "config", "head.h"), encoding="utf-8") as f:
            found = re.search(r'^\s*#define\s+MODULES_ON_BY_DEFAULT\s+"([^"]*)"', f.read(), re.M)
    except OSError:
        return []
    return found.group(1).split() if found else []


def modules_file(container, on):
    """a modules file (EMC2) of the packages of `container` (the bytes of the build's modules file) named in `on`,
    b"" when there is none of them"""
    magic, count = struct.unpack_from("<II", container, 0)
    if magic != CONTAINER_MAGIC:
        raise ValueError("not a modules file (EMC2)")
    rows = []
    for index in range(count):
        fields = list(CONTAINER_ENTRY.unpack_from(container, 8 + CONTAINER_ENTRY.size * index))
        if fields[0].rstrip(b"\0").decode() in on:
            rows.append(fields)

    found = {fields[0].rstrip(b"\0").decode() for fields in rows}
    missing = set(on) - found
    if missing:
        print(f"  initial: WARNING on by default, but no such module: {' '.join(sorted(missing))}")
    if not rows:
        return b""

    table = b""
    body = b""
    at = 8 + CONTAINER_ENTRY.size * len(rows)
    for fields in rows:
        package = container[fields[1]: fields[1] + fields[2]]
        magic, version = PACKAGE.unpack_from(package, 0)[:2]
        if magic != PACKAGE_MAGIC or version != PACKAGE_VERSION:
            name = fields[0].rstrip(b"\0").decode()
            raise ValueError(f"{name}: not a package of version {PACKAGE_VERSION}")
        fields[1] = at + len(body)
        table += CONTAINER_ENTRY.pack(*fields)
        body += package
    return struct.pack("<II", CONTAINER_MAGIC, len(rows)) + table + body


def read_modules_on(container_path, on):
    """modules_file() of the build's modules file at `container_path`"""
    with open(container_path, "rb") as f:
        return modules_file(f.read(), on)


def initial_image(head, area_offset, container_path, on, area_end=None):
    """the initial image: `head` (the bytes from the image's first byte to the end of the head), 0xFF up to
    `area_offset` (the module area's start, from the image's first byte), the modules file of `on` there; the head
    alone when none of them is in the build. area_end: the end of what the modules may take (the directory's sectors),
    from the image's first byte, checked when given"""
    if area_offset < len(head) or area_offset % SECTOR:
        raise ValueError(f"the module area at 0x{area_offset:X} is not a sector behind the head (0x{len(head):X})")
    tail = read_modules_on(container_path, on)
    if not tail:
        return bytes(head)
    if area_end is not None and area_offset + round_up(len(tail), SECTOR) > area_end:
        raise ValueError(f"the modules on by default ({len(tail)} bytes at 0x{area_offset:X}) do not fit the area "
                         f"(up to 0x{area_end:X})")
    print(f"  initial: the head + {', '.join(sorted(on))} at the area start ({len(tail)} bytes at 0x{area_offset:X})")
    return bytes(head) + b"\xff" * (area_offset - len(head)) + tail
