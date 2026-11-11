# PROTOTYPE (docs/modules.md): the `full` image of the head + modules build, every chip.
#
#   initial  the head alone (the chip's release image)
#   full     the initial image with every module of the modules file (EMC2) in flash and switched off: the head, 0xFF,
#            the module slots from the top of the modules area down, the module directory (modules_core.h Directory)
#            at its end.
#
# Modules switched on by default (`on`: the head's list of them, e.g. the captive portal: the user wants it with the head) are
# the one exception: on in `full`, and the initial image holds them too (the head, them, a directory of them alone).
# The board places their RAM at the first boot like any module switched on (EntryUnplaced without EntryDisabled).
#
# A module that is off takes no RAM (EntryDisabled | EntryUnplaced, modules_core.h, the same on every chip): only its
# flash slot is placed here. Switched on, the board places its RAM and relocates the image in its slot (arch::place),
# then the core starts it: no download, no restart. So this build never runs out of RAM; the board says "not enough
# RAM" when too much is switched on at once. A module that does not fit the flash is left out of `full` (the board
# installs it from the modules file when there is room).
#
# A chip script (esp8266_full_image.py, esp32_full_image.py) subclasses FullImage with its flash layout, how a module
# lies in its slot and its directory entry, and what it writes.

import hashlib
import os
import struct
import sys
import zlib

# loaded by a chip's link script or run alone: the shared scripts sit next to this file
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from module_link import CONTAINER_ENTRY, CONTAINER_MAGIC, CHUNK, HEADER_ID, HEADER_MAGIC, MAP_SIZE, PACKAGE, \
    PACKAGE_MAGIC, module_name, round_up  # noqa: E402
from release_image import listing, path, write  # noqa: E402

SECTOR = 4096

DIRECTORY_MAGIC = 0x35524445  # "EDR5": modules_core.h Directory
DIRECTORY_ENTRIES = 16
# name, offset, slot, size, crc32, ram_offset, ram_size, arch[3], flags (modules_core.h Entry)
ENTRY = struct.Struct("<16sIIIIII3II")
OFF_REASON = 96


def directory_bytes(magic, entries):
    """modules_core.h Directory: magic, count, the entries, the guard's reason, generation 1 and the CRC-32 of all
    before it. The sector under it is its second copy (DirectorySectors): no slot there"""
    out = struct.pack("<II", magic, len(entries)) + b"".join(entries)
    out += b"\0" * (ENTRY.size * (DIRECTORY_ENTRIES - len(entries)) + OFF_REASON)
    out += struct.pack("<I", 1)
    return out + struct.pack("<I", zlib.crc32(out) & 0xFFFFFFFF)

# modules_core.h: EntryDisabled, EntryUnplaced
ENTRY_DISABLED = 1
ENTRY_UNPLACED = 2


def modules_on(project_dir):
    """the modules switched on by default, one list for every chip: MODULES_ON_BY_DEFAULT "name ..." of
    espurna/config/head.h (none when it is not there)"""
    import re
    try:
        with open(os.path.join(project_dir, "espurna", "config", "head.h"), encoding="utf-8") as f:
            found = re.search(r'^\s*#define\s+MODULES_ON_BY_DEFAULT\s+"([^"]*)"', f.read(), re.M)
    except OSError:
        return []
    return found.group(1).split() if found else []


def sectors(size):
    return round_up(size, SECTOR)


class Space:
    """modules_core.h Space: pieces in [lo, hi), best fit, flash at the top of the gap, arenas at the bottom"""

    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi
        self.pieces = []

    def add(self, start, size):
        if size:
            self.pieces.append((start, start + size))
            self.pieces.sort()

    def gaps(self):
        out = []
        for index in range(len(self.pieces) + 1):
            a = max(self.lo, self.pieces[index - 1][1]) if index else self.lo
            b = min(self.hi, self.pieces[index][0]) if index < len(self.pieces) else self.hi
            out.append((a, b, max(0, b - a)))
        return out

    def place(self, size, top):
        found = None
        best = 0
        for a, b, length in self.gaps():
            if length >= size and (found is None or length < best or (top and length == best)):
                found = (b - size) if top else a
                best = length
        return found


def read_container(data, at=0):
    """[(name, package)] of a modules file (EMC2) at `at`"""
    magic, count = struct.unpack_from("<II", data, at)
    if magic != CONTAINER_MAGIC:
        raise ValueError("not a modules file (EMC2)")
    out = []
    for index in range(count):
        name, offset, size, *_ = CONTAINER_ENTRY.unpack_from(data, at + 8 + CONTAINER_ENTRY.size * index)
        out.append((name.rstrip(b"\0").decode(), data[at + offset: at + offset + size]))
    return out


def split_package(name, package, version):
    """(header fields, the image as linked, its relocation maps) of a package of versions 1 and 2: the header, then per
    chunk its map and its bytes"""
    fields = PACKAGE.unpack_from(package, 0)
    magic, found, _, _, image_size = fields[:5]
    if magic != PACKAGE_MAGIC or found != version:
        raise ValueError(f"{name}: not a module package of version {version}")
    if zlib.crc32(package[PACKAGE.size:]) & 0xFFFFFFFF != fields[10]:
        raise ValueError(f"{name}: package crc")
    image = bytearray()
    maps = bytearray()
    pos = PACKAGE.size
    while len(image) < image_size:
        maps += package[pos: pos + MAP_SIZE]
        pos += MAP_SIZE
        part = min(CHUNK, image_size - len(image))
        image += package[pos: pos + part]
        pos += part
    if pos != len(package):
        raise ValueError(f"{name}: package size")
    return fields, bytes(image), bytes(maps)


def relocate(image, maps, deltas):
    """the image with every word moved by the delta of its kind (deltas[kind], kind 0 stays)"""
    out = bytearray(image)
    for index in range(0, len(image), 4):
        word = index // 4
        kind = (maps[(word // (CHUNK // 4)) * MAP_SIZE + ((word % (CHUNK // 4)) >> 2)] >> ((word & 3) * 2)) & 3
        if kind:
            value, = struct.unpack_from("<I", out, index)
            struct.pack_into("<I", out, index, (value + deltas[kind]) & 0xFFFFFFFF)
    return bytes(out)


class FullImage:
    # the release name (espurna-<image>.bin, espurna-<image>-full.bin), the package version of the chip
    image = ""
    package_version = 1
    # the relocation maps lie behind the image in the slot (the board relocates from them when it is switched on)
    maps_in_slot = False
    # the initial image holds the modules on by default placed, with their directory (ESP8266: no, they are packages in
    # the tail of the head's file, esp8266_full_image.py)
    initial_carries_on = True

    def __init__(self, head_bin, container, out_dir, on=()):
        with open(head_bin, "rb") as f:
            self.head = f.read()
        with open(container, "rb") as f:
            self.modules_file = f.read()
        self.head_id = hashlib.md5(self.head).digest()
        self.out_dir = out_dir
        # modules switched on by default: in the initial image too
        self.on = set(on)

    # --- chip hooks ------------------------------------------------------------------------------------------------

    def layout(self):
        """(initial image, flash offset of its first byte, size of full, modules area start, directory offset)"""
        raise NotImplementedError

    def module(self, name, fields, image, maps, slot):
        """(the image as it lies in its slot, ram_offset, ram_size, arch[3], flags, summary); fields: PACKAGE"""
        raise NotImplementedError

    def outputs(self, initial, full):
        """write the initial image, return its path. No full image any more (decision 11-10: the install image .ota
        and the Apply replace it); `full` is still built, it checks that every module fits the area"""
        return [write(path(self.out_dir, self.image), initial)]

    # --- the image -------------------------------------------------------------------------------------------------

    def slot_size(self, image_size):
        """modules_core.h / the backend's slotSize(): the image and, when kept, its relocation maps, each in sectors"""
        size = sectors(image_size)
        if self.maps_in_slot:
            size += sectors((image_size + CHUNK - 1) // CHUNK * MAP_SIZE)
        return size

    @staticmethod
    def directory(entries):
        """modules_core.h Directory with these entries"""
        return directory_bytes(DIRECTORY_MAGIC, entries)

    def build(self):
        initial, base, size, lo, directory_offset = self.layout()
        full = bytearray(b"\xff" * size)
        full[: len(initial)] = initial
        flash = Space(lo, directory_offset - SECTOR)

        entries = []
        on_entries = []
        on_pieces = []
        left_out = []
        # the ones on by default first: the top slots, the same in both images
        modules = sorted(read_container(self.modules_file), key=lambda item: item[0] not in self.on)
        for name, package in modules:
            fields, image, maps = split_package(name, package, self.package_version)
            if fields[3] != self.head_id:
                raise ValueError(f"{name}: built for another head")
            if struct.unpack_from("<I", image, 0)[0] != HEADER_MAGIC or image[HEADER_ID: HEADER_ID + 16] != self.head_id:
                raise ValueError(f"{name}: no module header for this head")

            slot_size = self.slot_size(len(image))
            slot = flash.place(slot_size, True)
            if slot is None:
                free = max(length for _, _, length in flash.gaps())
                print(f"  full: {name:<14} {len(image):7} bytes, does NOT fit (largest gap {free}): "
                      f"only in the modules file")
                left_out.append(name)
                continue
            flash.add(slot, slot_size)

            placed, ram_offset, ram_size, arch, flags, summary = self.module(name, fields, image, maps, slot)
            on = name in self.on
            if on:
                flags &= ~ENTRY_DISABLED
                summary = "ON by default, in the initial image too" + (", RAM placed at the first boot"
                                                                        if flags & ENTRY_UNPLACED else "")
            pieces = [(slot - base, placed)]
            if self.maps_in_slot:
                pieces.append((slot - base + sectors(len(image)), maps))
            for at, data in pieces:
                full[at: at + len(data)] = data
            entry = ENTRY.pack(module_name(name), slot, slot_size, len(placed), zlib.crc32(placed) & 0xFFFFFFFF,
                               ram_offset, ram_size, *arch, flags)
            entries.append(entry)
            if on:
                on_entries.append(entry)
                on_pieces += pieces
            print(f"  full: {name:<14} 0x{slot:06X} {len(placed):7} bytes, {summary}")

        missing = self.on - {name for name, _ in modules}
        if missing:
            print(f"  full: WARNING on by default, but no such module: {' '.join(sorted(missing))}")
        if len(entries) > DIRECTORY_ENTRIES:
            raise ValueError("more modules than directory entries")
        at = directory_offset - base
        directory = self.directory(entries)
        full[at: at + len(directory)] = directory

        free = sum(length for _, _, length in flash.gaps())
        print(f"  full: {len(entries)} modules, {len(on_entries)} on; flash free {free} bytes; directory at "
              f"0x{directory_offset:06X}" + (f"; left out: {', '.join(left_out)}" if left_out else ""))

        # the initial image with the modules on by default: the head, them, their directory
        if on_entries and self.initial_carries_on:
            with_on = bytearray(b"\xff" * sectors(at + len(directory)))
            with_on[: len(initial)] = initial
            for piece_at, data in on_pieces:
                with_on[piece_at: piece_at + len(data)] = data
            on_directory = self.directory(on_entries)
            with_on[at: at + len(on_directory)] = on_directory
            initial = bytes(with_on)
            print(f"  initial: the head + {len(on_entries)} module(s) on by default, {len(initial)} bytes")

        out = self.outputs(initial, bytes(full))
        listing(out)
        return out[-1]
