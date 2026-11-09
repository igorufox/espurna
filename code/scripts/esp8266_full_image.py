# PROTOTYPE (docs/esp8266-modules.md): the two ESP8266 images of the head + modules build.
#
#   initial  espurna-esp8266-1m-initial.bin(.gz): the head alone. 1 MB boards; modules are added on its page (or the
#            WebUI's MODULES panel) from the modules part of `full`.
#   full     espurna-esp8266-1m-full.bin: a flash image from 0x0, for serial (4 MB boards, or anything that should have
#            everything). The head, every module placed and relocated the way the board's loader places an upload
#            (modules_esp8266.cpp: prepare / place / finish), the module directory; the bytes up to 1 MB are 0xFF (the
#            settings and SDK sectors start empty), and behind 1 MB the modules file (EMC1) itself, where the modules
#            page finds the packages for 1 MB boards. On a 4 MB chip that part lands in flash the 1 MB layout does not
#            use. Flash it with the header's flash size kept (esptool: --flash_size keep, the default).
#
# Used by scripts/esp8266_module_link.py; also runs alone:
#   py -3 esp8266_full_image.py <head.bin> <modules.bin> <out dir> <ram arena addr> <ram arena size>
#       <iram arena addr> <iram arena size> <_text_end> [<module whose RAM goes to IRAM> ...]

import gzip
import os
import struct
import sys
import zlib

FLASH_MAPPED = 0x40200000
SECTOR = 4096
CHUNK = 4096
MAP_SIZE = CHUNK // 16
MiB = 0x100000

# 1 MB layout: EEPROM_Rotate keeps the settings in sectors 251 and 250, the directory is the sector under them
# (modules_esp8266.cpp directoryOffset(): (base - size) * 4096 with base 251, size 2)
DIRECTORY_OFFSET = 0xF9000
DIRECTORY_MAGIC = 0x32524445  # "EDR2"
DIRECTORY_ENTRIES = 12
ENTRY = struct.Struct("<16sIIIIIII")
OFF_REASON = 96

# IRAM48 layout (PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48 in platformio_modules.ini): IRAM up to 0x4010C000
IRAM_END = 0x40100000 + 0xC000
RAM_IN_IRAM = 0x80000000

PACKAGE = struct.Struct("<II16s16sIIIIIIII")
PACKAGE_MAGIC = 0x31504D45
HEADER_MAGIC = 0x31444D45
CONTAINER_MAGIC = 0x31434D45
CONTAINER_ENTRY = struct.Struct("<16sIIIII12x")


def sectors(size):
    return (size + SECTOR - 1) & ~(SECTOR - 1)


class Space:
    """modules_esp8266.cpp Space: pieces in [lo, hi), best fit, flash at the top of the gap, arenas at the bottom"""

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
    magic, count = struct.unpack_from("<II", data, at)
    if magic != CONTAINER_MAGIC:
        raise ValueError("not a modules file")
    out = []
    for index in range(count):
        name, offset, size, _, _, _ = CONTAINER_ENTRY.unpack_from(data, at + 8 + CONTAINER_ENTRY.size * index)
        out.append((name.rstrip(b"\0").decode(), data[at + offset : at + offset + size]))
    return out


def relocate(package, slot, ram_base, iram_base):
    """the image as the loader writes it at flash offset `slot`, RAM at ram_base, IRAM at iram_base"""
    (magic, version, name, head_md5, image_size, ram_size, iram_size, link_flash, link_ram, link_iram, crc,
     _) = PACKAGE.unpack_from(package, 0)
    if magic != PACKAGE_MAGIC or version != 1:
        raise ValueError(f"{name}: not a module package")
    body = package[PACKAGE.size:]
    if zlib.crc32(body) & 0xFFFFFFFF != crc:
        raise ValueError(f"{name}: package crc")

    delta = [0, (FLASH_MAPPED + slot - link_flash) & 0xFFFFFFFF, (ram_base - link_ram) & 0xFFFFFFFF,
             (iram_base - link_iram) & 0xFFFFFFFF]
    image = bytearray()
    at = 0
    while len(image) < image_size:
        bitmap = body[at : at + MAP_SIZE]
        size = min(CHUNK, image_size - len(image))
        chunk = body[at + MAP_SIZE : at + MAP_SIZE + size]
        at += MAP_SIZE + size
        for index in range(0, size, 4):
            word, = struct.unpack_from("<I", chunk, index)
            kind = (bitmap[(index // 4) >> 2] >> (((index // 4) & 3) * 2)) & 3
            image += struct.pack("<I", (word + delta[kind]) & 0xFFFFFFFF)
    if at != len(body):
        raise ValueError(f"{name}: package size")
    return bytes(image), head_md5, ram_size, iram_size


def build(head_bin, container, out_dir, head, ram_in_iram=()):
    """head: dict ram_arena, ram_arena_size, iram_arena, iram_arena_size, text_end (from the head ELF)"""
    with open(head_bin, "rb") as f:
        head_image = f.read()
    with open(container, "rb") as f:
        modules_file = f.read()

    import hashlib
    head_md5 = hashlib.md5(head_image).digest()

    iram_data = (head["text_end"] + 32 + 3) & ~3
    flash = Space(sectors(len(head_image)), DIRECTORY_OFFSET)
    ram = Space(0, head["ram_arena_size"])
    iram = Space(0, head["iram_arena_size"])
    iram_ram = Space(0, max(0, IRAM_END - iram_data))

    full = bytearray(b"\xff" * MiB)
    full[: len(head_image)] = head_image

    entries = []
    for name, package in read_container(modules_file):
        _, _, _, md5, image_size, ram_size, iram_size, _, _, _, _, _ = PACKAGE.unpack_from(package, 0)
        if md5 != head_md5:
            raise ValueError(f"{name}: built for another head")

        slot = flash.place(sectors(image_size), True)
        if slot is None:
            raise ValueError(f"{name}: not enough flash")

        ram_size = (ram_size + 3) & ~3
        in_iram = bool(ram_size) and (name in ram_in_iram)
        ram_offset = 0
        if ram_size:
            target = iram_ram if in_iram else ram
            ram_offset = target.place(ram_size, False)
            if ram_offset is None:
                raise ValueError(f"{name}: not enough {'IRAM for its RAM' if in_iram else 'RAM in the arena'}")

        iram_size = (iram_size + 3) & ~3
        iram_offset = 0
        if iram_size:
            iram_offset = iram.place(iram_size, False)
            if iram_offset is None:
                raise ValueError(f"{name}: not enough IRAM in the arena")

        ram_base = (iram_data if in_iram else head["ram_arena"]) + ram_offset
        image, _, _, _ = relocate(package, slot, ram_base, head["iram_arena"] + iram_offset)

        # what the loader checks at boot (start()): the header at its own address, the head it was made for
        magic, = struct.unpack_from("<I", image, 0)
        flash_start, = struct.unpack_from("<I", image, 40)
        if magic != HEADER_MAGIC or flash_start != FLASH_MAPPED + slot or image[24:40] != head_md5:
            raise ValueError(f"{name}: relocated header does not check out")

        full[slot : slot + len(image)] = image
        flash.add(slot, sectors(len(image)))
        (iram_ram if in_iram else ram).add(ram_offset, ram_size)
        iram.add(iram_offset, iram_size)
        entries.append(ENTRY.pack(name.encode()[:15].ljust(16, b"\0"), slot, len(image),
                                  zlib.crc32(image) & 0xFFFFFFFF, ram_offset | (RAM_IN_IRAM if in_iram else 0),
                                  ram_size, iram_offset, iram_size))
        print(f"  full: {name:<14} 0x{slot:05X} {len(image):6} bytes, RAM {ram_size} @{ram_offset}"
              f"{' (in IRAM)' if in_iram else ''}, IRAM {iram_size} @{iram_offset}")

    if len(entries) > DIRECTORY_ENTRIES:
        raise ValueError("more modules than directory entries")
    directory = struct.pack("<II", DIRECTORY_MAGIC, len(entries)) + b"".join(entries)
    directory += b"\0" * (ENTRY.size * (DIRECTORY_ENTRIES - len(entries)) + OFF_REASON)
    full[DIRECTORY_OFFSET : DIRECTORY_OFFSET + len(directory)] = directory

    free = sum(length for _, _, length in flash.gaps())
    print(f"  full: flash free {free} bytes, RAM arena {sum(l for _, _, l in ram.gaps())} of {ram.hi} free, "
          f"IRAM {sum(l for _, _, l in iram.gaps())} of {iram.hi}, RAM in IRAM "
          f"{sum(l for _, _, l in iram_ram.gaps())} of {iram_ram.hi}")

    initial = os.path.join(out_dir, "espurna-esp8266-1m-initial.bin")
    with open(initial, "wb") as f:
        f.write(head_image)
    with open(initial + ".gz", "wb") as f:
        f.write(gzip.compress(head_image, 9, mtime=0))

    out = os.path.join(out_dir, "espurna-esp8266-1m-full.bin")
    with open(out, "wb") as f:
        f.write(bytes(full) + modules_file)
    print(f"Images: {initial} (+ .gz), {out} ({MiB + len(modules_file)} bytes, modules file at 0x{MiB:X})")
    return out


if __name__ == "__main__":
    args = sys.argv[1:]
    build(args[0], args[1], args[2], {
        "ram_arena": int(args[3], 0), "ram_arena_size": int(args[4], 0),
        "iram_arena": int(args[5], 0), "iram_arena_size": int(args[6], 0), "text_end": int(args[7], 0),
    }, set(args[8:]))
