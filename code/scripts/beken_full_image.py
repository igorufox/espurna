# PROTOTYPE (docs/modules.md, "Beken"): the `full` image of the Beken build (the shared part: module_full_image.py).
#
#   initial  espurna-beken-tuya.uf2: the head (LibreTiny's UF2), first flash over UART and OTA
#   full     espurna-beken-full.bin: flash from 0x11000 (the app partition, then the OTA 'download' partition up to its
#            last sector), for the serial flash only:
#                ltchiptool flash write -f BK7231N -s 0x11000 espurna-beken-full.bin
#            The app partition: LibreTiny's encrypted + CRC'd app (image_<mcu>_app.0x011000.crc) and its RBL header; the
#            download partition: its first sector empty (no OTA to install), every module in its slot from the top down,
#            encrypted + CRC'd like the code the flash mapping runs, the module directory (EDB3) in the last sector.
#
# The flash format (modules_beken.cpp): logical address L in flash at L / 32 * 34, each 32 byte block XORed with a pad
# of its address and the chip's key (the key of the board, build.bkcrypt_coeffs: the Tuya one, the same LibreTiny
# encrypts the head with), then its CRC-16 (CMS, big endian). Checked here against LibreTiny's own .crc of the head.
#
# Every module is off (EntryDisabled | EntryUnplaced) but the ones on by default (MODULES_ON_BY_DEFAULT of
# espurna/config/head.h: placed at the first boot). The Beken does not rewrite a module's image to move its RAM (a flash
# rewrite with no copy to go back to on a power cut): each module gets its own place in the RAM arena here, the biggest
# at the bottom, the rest from the top down, the smallest at the very top; switched on, the board runs it when no module
# switched on takes any of that RAM (modules_beken.cpp arch::place).
#
# Used by scripts/beken_module_link.py; also runs alone:
#   py -3 beken_full_image.py <build dir> <modules.bin> <head id hex> <arena address hex> <arena size> <out dir> [<on> ...]

import glob
import json
import os
import re
import struct
import sys
import zlib

# loaded by beken_module_link.py or run alone: the shared scripts sit next to this file
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from module_full_image import directory_bytes, ENTRY, ENTRY_DISABLED, ENTRY_UNPLACED, DIRECTORY_ENTRIES, OFF_REASON, SECTOR, Space, \
    read_container  # noqa: E402
from module_link import CHUNK, HEADER_ID, HEADER_MAGIC, MAP_SIZE, PACKAGE, PACKAGE_MAGIC, module_name, \
    round_up  # noqa: E402
from release_image import listing, path, write  # noqa: E402

BLOCK = 32
BLOCK_RAW = 34
PACKAGE_VERSION = 3
DIRECTORY_MAGIC = 0x33424445  # "EDB3", modules_beken.cpp
BRANCH_THUMB = 1
BRANCH_ARM = 2


def bits(value, hi, lo):
    return (value >> lo) & ((1 << (hi - lo + 1)) - 1)


class Crypt:
    """modules_beken.cpp Crypt (encrypt_crc/abc.c of the SDK, ltchiptool's BekenCrypto)"""

    def __init__(self, coef):
        coef3 = coef[3]
        self.bypass = bits(coef3, 31, 24) in (0x00, 0xFF) or bits(coef3, 3, 0) == 0xF
        self.coef0 = coef[0]
        self.coef1_hi16 = bits(coef[1], 31, 16)
        self.coef1_mix = (bits(coef[1], 15, 8) << 9) | (bits(coef3, 4, 4) << 8) | bits(coef[1], 7, 0)
        self.use15 = not bits(coef3, 0, 0)
        self.use16 = not bits(coef3, 1, 1)
        self.use32 = not bits(coef3, 2, 2)
        self.random = 0 if bits(coef3, 3, 3) else coef[2]
        self.sel15 = bits(coef3, 6, 5)
        self.sel16 = bits(coef3, 9, 8)
        self.sel32 = bits(coef3, 12, 11)

    @staticmethod
    def pn15(addr):
        a = (bits(addr, 6, 0) << 9) | bits(addr, 15, 7)
        c = 0x6371 & (bits(addr, 8, 5) * 0x1111)
        return (a ^ c) & 0xFFFF

    @staticmethod
    def pn16(addr):
        a = (bits(addr, 9, 0) << 7) | bits(addr, 16, 10)
        b = bits(addr, 13, 13) | (bits(addr, 9, 9) << 1) | (bits(addr, 5, 5) << 2) | (bits(addr, 1, 1) << 3)
        d = 0x13659 & ((bits(addr, 4, 4) * 0x10000) + (b * 0x1111))
        return (a ^ d) & 0x1FFFF

    @staticmethod
    def pn32(addr):
        a = ((bits(addr, 14, 0) << 17) | bits(addr, 31, 15)) & 0xFFFFFFFF
        c = 0xE519A4F1 & ((bits(addr, 5, 2) * 0x11111111) & 0xFFFFFFFF)
        return a ^ c

    def pad(self, addr):
        if self.bypass:
            return 0
        v15 = 0
        if self.use15:
            b3, b2, b1, b0 = bits(addr, 31, 24), bits(addr, 23, 16), bits(addr, 15, 8), bits(addr, 7, 0)
            hi = ((b2 << 8) | b3) if (self.sel15 & 2) else ((b3 << 8) | b2)
            lo = ((b0 << 8) | b1) if (self.sel15 & 1) else ((b1 << 8) | b0)
            v15 = self.pn15((hi ^ lo) ^ self.coef1_hi16)
        v16 = 0
        if self.use16:
            v16 = self.pn16(bits(addr, 16 + self.sel16, self.sel16) ^ self.coef1_mix)
        v32 = 0
        if self.use32:
            a = addr
            if self.sel32:
                shift = 8 * self.sel32
                a = ((addr >> shift) | (addr << (32 - shift))) & 0xFFFFFFFF
            v32 = self.pn32(a ^ self.coef0)
        return (v32 ^ ((v15 << 16) | (v16 & 0xFFFF)) ^ self.random) & 0xFFFFFFFF


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encrypt(crypt, logical, plain):
    """32 byte blocks of plain data at a logical address into the flash format (modules_beken.cpp writeBlocks)"""
    if logical % BLOCK or len(plain) % BLOCK:
        raise ValueError("not whole blocks")
    out = bytearray()
    for block in range(0, len(plain), BLOCK):
        raw = bytearray(BLOCK)
        for word in range(0, BLOCK, 4):
            value, = struct.unpack_from("<I", plain, block + word)
            struct.pack_into("<I", raw, word, value ^ crypt.pad(logical + block + word))
        out += raw + struct.pack(">H", crc16(raw))
    return bytes(out)


def physical(logical):
    return logical // BLOCK * BLOCK_RAW


def logical_from(offset):
    return (offset + BLOCK_RAW - 1) // BLOCK_RAW * BLOCK


def slot_size(image_size):
    """modules_beken.cpp slotSize()"""
    return round_up(BLOCK_RAW + round_up(image_size, BLOCK) // BLOCK * BLOCK_RAW, SECTOR)


def split_package(name, package):
    """(PACKAGE fields, the image as linked, its maps, [(offset in the image, branch kind)]) of a version 3 package"""
    fields = PACKAGE.unpack_from(package, 0)
    if fields[0] != PACKAGE_MAGIC or fields[1] != PACKAGE_VERSION:
        raise ValueError(f"{name}: not a module package of version {PACKAGE_VERSION}")
    if zlib.crc32(package[PACKAGE.size:]) & 0xFFFFFFFF != fields[10]:
        raise ValueError(f"{name}: package crc")
    image_size = fields[4]
    image = bytearray()
    maps = bytearray()
    branches = []
    pos = PACKAGE.size
    while len(image) < image_size:
        maps += package[pos: pos + MAP_SIZE]
        pos += MAP_SIZE
        count, = struct.unpack_from("<I", package, pos)
        pos += 4
        for index in range(count):
            word, = struct.unpack_from("<I", package, pos + index * 4)
            branches.append((len(image) + (word & 0xFFFF), word >> 16))
        pos += count * 4
        part = min(CHUNK, image_size - len(image))
        image += package[pos: pos + part]
        pos += part
    if pos != len(package):
        raise ValueError(f"{name}: package size")
    return fields, bytes(image), bytes(maps), branches


def move_thumb(image, at, delta):
    """modules_beken.cpp moveThumb: a BL / BLX into the head, its offset shrinks by delta"""
    h1, h2 = struct.unpack_from("<HH", image, at)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xE800) != 0xE800:
        raise ValueError(f"+0x{at:X}: not a Thumb BL / BLX")
    offset = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    offset = offset - (1 << 23) if offset & (1 << 22) else offset
    offset -= delta
    if not -(1 << 22) <= offset < (1 << 22):
        raise ValueError(f"+0x{at:X}: branch out of range")
    struct.pack_into("<HH", image, at, 0xF000 | ((offset >> 12) & 0x7FF), (h2 & 0xF800) | ((offset >> 1) & 0x7FF))


def move_arm(image, at, delta):
    insn, = struct.unpack_from("<I", image, at)
    if (insn & 0x0E000000) != 0x0A000000:
        raise ValueError(f"+0x{at:X}: not an ARM BL / BLX")
    offset = insn & 0xFFFFFF
    offset = offset - (1 << 24) if offset & (1 << 23) else offset
    offset -= delta // 4
    if not -(1 << 23) <= offset < (1 << 23):
        raise ValueError(f"+0x{at:X}: branch out of range")
    struct.pack_into("<I", image, at, (insn & 0xFF000000) | (offset & 0xFFFFFF))


def relocate(image, maps, branches, flash_delta, ram_delta):
    """modules_beken.cpp chunkDone: the words by kind (1 flash, 2 RAM), the branches into the head by the flash delta"""
    out = bytearray(image)
    deltas = {1: flash_delta, 2: ram_delta}
    for index in range(0, len(out), 4):
        word = index // 4
        kind = (maps[(word // (CHUNK // 4)) * MAP_SIZE + ((word % (CHUNK // 4)) >> 2)] >> ((word & 3) * 2)) & 3
        if kind == 3:
            raise ValueError("unknown relocation kind")
        if kind:
            value, = struct.unpack_from("<I", out, index)
            struct.pack_into("<I", out, index, (value + deltas[kind]) & 0xFFFFFFFF)
    for at, kind in branches:
        if kind == BRANCH_THUMB:
            move_thumb(out, at, flash_delta)
        elif kind == BRANCH_ARM:
            move_arm(out, at, flash_delta)
        else:
            raise ValueError(f"+0x{at:X}: branch list: bad entry")
    return bytes(out)


def region(board, name):
    start, size = board["flash"][name].split("+")
    return int(start, 16), int(size, 16)


def find_one(build_dir, pattern):
    found = glob.glob(os.path.join(build_dir, pattern))
    if len(found) != 1:
        raise RuntimeError(f"full image: {pattern} in {build_dir}: {len(found)} files")
    return found[0], int(re.search(r"\.0x([0-9A-Fa-f]+)\.", os.path.basename(found[0])).group(1), 16)


class BekenFullImage:
    image = "beken"

    def __init__(self, build_dir, container, head_id, arena, arena_size, on=(), out_dir=None):
        with open(os.path.join(build_dir, "board.json"), encoding="utf-8") as f:
            board = json.load(f)
        coeffs = board["build"]["bkcrypt_coeffs"]
        self.crypt = Crypt([int(coeffs[index: index + 8], 16) for index in range(0, 32, 8)])
        self.app_offset, app_size = region(board, "app")
        self.download_offset, download_size = region(board, "download")
        self.end = self.download_offset + download_size
        self.app_crc, crc_at = find_one(build_dir, "image_*_app.0x*.crc")
        self.rblh, self.rblh_at = find_one(build_dir, "image_*_app.0x*.rblh")
        if crc_at != self.app_offset:
            raise RuntimeError(f"full image: the app at 0x{crc_at:X}, its partition at 0x{self.app_offset:X}")
        if not (self.app_offset <= self.rblh_at < self.app_offset + app_size):
            raise RuntimeError("full image: the RBL header is outside the app partition")
        self.raw_bin = os.path.join(build_dir, "raw_firmware.bin")
        with open(container, "rb") as f:
            self.modules_file = f.read()
        self.head_id = head_id
        self.arena = arena
        self.arena_size = arena_size
        self.out_dir = out_dir or build_dir
        self.on = set(on)

    def check_crypt(self, app):
        """our flash format against LibreTiny's encrypted head: the first 64 blocks of the app"""
        with open(self.raw_bin, "rb") as f:
            plain = f.read(BLOCK * 64)
        logical = self.app_offset // BLOCK_RAW * BLOCK
        if encrypt(self.crypt, logical, plain) != app[: BLOCK_RAW * 64]:
            raise RuntimeError("full image: the flash format does not match LibreTiny's encrypted app")

    def ram_places(self, sizes):
        """{name: offset}: the biggest at the bottom of the arena, the rest from the top down, the smallest on top"""
        out = {}
        order = sorted((size, name) for name, size in sizes.items() if size)
        if not order:
            return {name: 0 for name in sizes}
        biggest = order.pop()
        out[biggest[1]] = 0
        top = self.arena_size
        for size, name in order:
            top = (top - size) // 8 * 8
            if top < 0:
                top = (self.arena_size - size) // 8 * 8
            out[name] = top
        for name, size in sizes.items():
            out.setdefault(name, 0)
        return out

    def build(self):
        base = self.app_offset
        full = bytearray(b"\xff" * (self.end - base))
        with open(self.app_crc, "rb") as f:
            app = f.read()
        with open(self.rblh, "rb") as f:
            rblh = f.read()
        self.check_crypt(app)
        if self.app_offset + len(app) > self.rblh_at:
            raise RuntimeError("full image: the app runs into its RBL header")
        full[0: len(app)] = app
        full[self.rblh_at - base: self.rblh_at - base + len(rblh)] = rblh

        directory_offset = self.end - SECTOR
        flash = Space(self.download_offset + SECTOR, directory_offset - SECTOR)

        modules = []
        for name, package in sorted(read_container(self.modules_file), key=lambda item: item[0] not in self.on):
            fields, image, maps, branches = split_package(name, package)
            if fields[3] != self.head_id:
                raise ValueError(f"{name}: built for another head")
            if struct.unpack_from("<I", image, 0)[0] != HEADER_MAGIC or image[HEADER_ID: HEADER_ID + 16] != self.head_id:
                raise ValueError(f"{name}: no module header for this head")
            if len(image) % BLOCK or fields[6]:
                raise ValueError(f"{name}: bad image size")
            modules.append((name, fields, image, maps, branches, round_up(fields[5], 8)))

        places = self.ram_places({name: ram for name, _, _, _, _, ram in modules if ram <= self.arena_size})

        # The area holds not all of them (Matter alone takes most of it): the ones on by default, then the most modules
        # that fit, of those the most bytes (Matter is in: left out, it could not be installed later either, the small
        # ones can when the user removes another). The biggest first, from the top down
        on = [module for module in modules if module[0] in self.on]
        rest = [module for module in modules if module[0] not in self.on and module[5] <= self.arena_size]
        room = (directory_offset - SECTOR - (self.download_offset + SECTOR)) - sum(slot_size(len(m[2])) for m in on)
        best = (-1, -1, ())
        for mask in range(1 << len(rest)):
            chosen = tuple(m for index, m in enumerate(rest) if mask & (1 << index))
            total = sum(slot_size(len(m[2])) for m in chosen)
            if total <= room and (len(chosen), total) > best[:2]:
                best = (len(chosen), total, chosen)
        chosen = {m[0] for m in best[2]}
        modules = on + sorted((m for m in modules if m[0] not in self.on),
                              key=lambda m: (m[0] not in chosen, -len(m[2])))

        entries = []
        left_out = []
        for name, fields, image, maps, branches, ram_size in modules:
            if ram_size > self.arena_size:
                print(f"  full: {name:<14} RAM {ram_size} > the arena {self.arena_size}: only in the modules file")
                left_out.append(name)
                continue
            size = slot_size(len(image))
            slot = flash.place(size, True)
            if slot is None:
                free = max(length for _, _, length in flash.gaps())
                print(f"  full: {name:<14} {len(image):7} bytes, does NOT fit (largest gap {free}): "
                      f"only in the modules file")
                left_out.append(name)
                continue
            flash.add(slot, size)

            logical = logical_from(slot)
            ram_offset = places[name]
            # signed deltas: the branches move by a byte count, the words wrap at 32 bits in relocate()
            placed = relocate(image, maps, branches, logical - fields[7], self.arena + ram_offset - fields[8])
            data = encrypt(self.crypt, logical, placed)
            at = physical(logical) - base
            full[at: at + len(data)] = data

            flags = ENTRY_UNPLACED if name in self.on else (ENTRY_DISABLED | ENTRY_UNPLACED)
            entries.append(ENTRY.pack(module_name(name), slot, size, len(placed), zlib.crc32(placed) & 0xFFFFFFFF,
                                      ram_offset, ram_size, logical, 0, 0, flags))
            state = "ON by default" if name in self.on else "off"
            print(f"  full: {name:<14} 0x{slot:06X} at 0x{logical:06X} {len(placed):7} bytes, {state}, RAM {ram_size} "
                  f"at +{ram_offset}")

        missing = self.on - {name for name, *_ in modules}
        if missing:
            print(f"  full: WARNING on by default, but no such module: {' '.join(sorted(missing))}")
        if len(entries) > DIRECTORY_ENTRIES:
            raise ValueError("more modules than directory entries")
        directory = directory_bytes(DIRECTORY_MAGIC, entries)
        at = directory_offset - base
        full[at: at + len(directory)] = directory

        free = sum(length for _, _, length in flash.gaps())
        print(f"  full: {len(entries)} modules; flash free {free} bytes; directory at 0x{directory_offset:06X}"
              + (f"; left out: {', '.join(left_out)}" if left_out else ""))
        print(f"  full: ltchiptool flash write -f BK7231N -s 0x{base:X} {path(self.out_dir, self.image, 'full')}")
        out = [write(path(self.out_dir, self.image, "full"), bytes(full))]
        listing(out)
        return out[-1]


def build(build_dir, container, head_id, arena, arena_size, on=(), out_dir=None):
    """on: the modules on by default (MODULES_ON_BY_DEFAULT of espurna/config/head.h)"""
    return BekenFullImage(build_dir, container, head_id, arena, arena_size, on, out_dir).build()


if __name__ == "__main__":
    args = sys.argv[1:]
    build(args[0], args[1], bytes.fromhex(args[2]), int(args[3], 16), int(args[4]), args[6:], args[5])
