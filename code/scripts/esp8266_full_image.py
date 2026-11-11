# PROTOTYPE (docs/modules.md): the two ESP8266 images of the head + modules build (the shared part:
# module_full_image.py).
#
#   initial  espurna-esp8266.bin(.gz): the head, every ESP8266 (1 MB layout on any chip size), and from the sector
#            behind it a modules file (EMC2) of the modules on by default (custom_modules_on: the captive portal). One
#            file for the serial flash and the OTA: both put the tail right behind the head (eboot writes the whole file
#            from 0x0), and a head with no directory of its own installs them from there, switched on
#            (modules_esp8266.cpp arch::imageModules). Other modules are added on its page (or the WebUI's MODULES
#            panel) from espurna-esp8266-modules.bin or the modules part of `full`.
#   full     espurna-esp8266-full.bin: a flash image from 0x0, for serial (4 MB boards, or anything that should have
#            everything). The head, every module in flash and switched off, the module directory; the bytes up to 1 MB
#            are 0xFF (the settings and SDK sectors start empty), and behind 1 MB the modules file (EMC2) itself: the
#            packages the board places a module from when it is switched on, and the modules page's file for 1 MB
#            boards. On a 4 MB chip that part lands in flash the 1 MB layout does not use. Flash it with the header's
#            flash size kept (esptool: --flash_size keep, the default).
#
#            The ESP8266 runs a module's image where it lies: the image is relocated here for its slot (its flash
#            words); its RAM and IRAM words stay at the link bases until the board places them (modules_esp8266.cpp
#            arch::place, from the package behind 1 MB).
#
# Used by scripts/esp8266_module_link.py; also runs alone:
#   py -3 esp8266_full_image.py <head.bin> <modules.bin> <out dir> [<module whose RAM goes to IRAM> ...]

import os
import struct
import sys

# loaded by esp8266_module_link.py or run alone: the shared scripts sit next to this file
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from module_full_image import ENTRY_DISABLED, ENTRY_UNPLACED, FullImage, relocate, sectors  # noqa: E402
from module_link import CONTAINER_ENTRY, CONTAINER_MAGIC, round_up  # noqa: E402
from release_image import path, write, write_gz  # noqa: E402

FLASH_MAPPED = 0x40200000
MiB = 0x100000

# 1 MB layout: EEPROM_Rotate keeps the settings in sectors 251 and 250, the directory is the sector under them
# (modules_esp8266.cpp directoryOffset(): (base - size) * 4096 with base 251, size 2)
DIRECTORY_OFFSET = 0xF9000

# ram_offset of an entry whose RAM goes to the IRAM tail (IRAM48, PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48)
RAM_IN_IRAM = 0x80000000

# modules with interrupt code: their RAM stays in DRAM (byte access to IRAM is emulated, not from an interrupt)
DRAM_ONLY = {"light", "rf"}


class Esp8266FullImage(FullImage):
    image = "esp8266"  # espurna-esp8266.bin(.gz) (initial), espurna-esp8266-full.bin; ESPURNA_IMAGE of the head
    # the modules on by default travel as packages in the tail of the head's file, not placed (an OTA writes the file
    # alone, not the modules area and the directory)
    initial_carries_on = False

    def __init__(self, head_bin, container, out_dir, ram_in_iram=(), on=()):
        super().__init__(head_bin, container, out_dir, on)
        self.ram_in_iram = set(ram_in_iram) - DRAM_ONLY

    def tail(self):
        """the modules file (EMC2) of the modules on by default, b"" without any"""
        magic, count = struct.unpack_from("<II", self.modules_file, 0)
        if magic != CONTAINER_MAGIC:
            raise ValueError("not a modules file (EMC2)")
        rows = []
        for index in range(count):
            fields = list(CONTAINER_ENTRY.unpack_from(self.modules_file, 8 + CONTAINER_ENTRY.size * index))
            if fields[0].rstrip(b"\0").decode() in self.on:
                rows.append(fields)
        if not rows:
            return b""
        table = b""
        body = b""
        at = 8 + CONTAINER_ENTRY.size * len(rows)
        for fields in rows:
            package = self.modules_file[fields[1]: fields[1] + fields[2]]
            fields[1] = at + len(body)
            table += CONTAINER_ENTRY.pack(*fields)
            body += package
        return struct.pack("<II", CONTAINER_MAGIC, len(rows)) + table + body

    def layout(self):
        tail = self.tail()
        initial = self.head
        if tail:
            initial = self.head + b"\xff" * (sectors(len(self.head)) - len(self.head)) + tail
            print(f"  initial: the head + {', '.join(sorted(self.on))} in its tail ({len(tail)} bytes at "
                  f"0x{sectors(len(self.head)):X})")
        # `full` brings its directory: the tail is not read there, its slots may take the tail's sectors
        return initial, 0, MiB, sectors(len(self.head)), DIRECTORY_OFFSET

    def module(self, name, fields, image, maps, slot):
        link_flash, ram_size, iram_size = fields[7], round_up(fields[5], 4), round_up(fields[6], 4)
        placed = relocate(image, maps, {1: (FLASH_MAPPED + slot - link_flash) & 0xFFFFFFFF, 2: 0, 3: 0})
        # what the loader checks at boot (start()): the header at its own address
        if struct.unpack_from("<I", placed, 40)[0] != FLASH_MAPPED + slot:
            raise ValueError(f"{name}: relocated header does not check out")
        in_iram = name in self.ram_in_iram
        flags = ENTRY_DISABLED | (ENTRY_UNPLACED if ram_size or iram_size else 0)
        summary = f"off, RAM {ram_size} + IRAM {iram_size} when switched on" + (" (RAM in IRAM)" if in_iram else "")
        return placed, RAM_IN_IRAM if in_iram else 0, ram_size, (0, iram_size, 0), flags, summary

    def outputs(self, initial, full):
        out = write(path(self.out_dir, self.image), initial)
        return [out, write_gz(out, initial)]


def build(head_bin, container, out_dir, ram_in_iram=(), on=()):
    """on: the modules on by default (custom_modules_on), in the tail of the initial image too"""
    return Esp8266FullImage(head_bin, container, out_dir, ram_in_iram, on).build()


if __name__ == "__main__":
    args = sys.argv[1:]
    build(args[0], args[1], args[2], set(args[3:]))
