# The images of the Beken head, without ltchiptool (owner 2026-11-18: no LibreTiny). What ltchiptool's link2bin and
# uf2 write did (LibreTiny, MIT, Kuba Szczodrzyński: ltchiptool/soc/bk72xx, uf2tool), in one file:
#
#   image_bk7231n_app.0x011000.crc   the application: encrypted with the chip's coefficients (bkcrypt_coeffs of the
#                                    board), the CRC16 of every 32 bytes, for the 'app' partition (the first flash)
#   image_bk7231n_app.rblh           the RBL header of it, at the end of the 'app' partition
#   image_bk7231n_app.ota.rbl        the OTA package for 'download': gzip + AES-256-CBC, its RBL header in front
#   firmware.uf2                     the three above in LibreTiny's UF2 format (the flasher writes 'app', the board's
#                                    OTA takes 'download'), the FAL partition table of the board in its header
#
# A post: script of platformio_beken.ini (after the head's link: firmware.elf, firmware.bin), and a library of
# scripts/beken_module_link.py and scripts/beken_quectel_image.py (the initial images with the modules, the Quectel
# layout). The packages are byte for byte what ltchiptool wrote for the same input (the RBL timestamp aside).

import gzip
import json
import os
import struct
import sys
import time
from binascii import crc32
from datetime import datetime

FAMILY_BK7231N = 0x7B3EF230

# --- CRC16 of the Beken flash format: CRC-16/CMS (poly 0x8005, init 0xFFFF, not reflected) --------------------------

_CRC16_TABLE = []
for _byte in range(256):
    _crc = _byte << 8
    for _ in range(8):
        _crc = ((_crc << 1) ^ 0x8005) if (_crc & 0x8000) else (_crc << 1)
    _CRC16_TABLE.append(_crc & 0xFFFF)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC16_TABLE[((crc >> 8) ^ byte) & 0xFF]
    return crc


def to_offset(addr):
    """the flash offset of an address of the application (34 bytes in the flash per 32 of the image)"""
    return int(addr + (addr // 32) * 2)


# --- the flash encryption of the chip (ltchiptool soc/bk72xx/util/crypto.py, from the Tuya SDK's encrypt_crc) ---------

def _bits(value, start, stop):
    return (value >> stop) & ((1 << (start - stop + 1)) - 1)


def _pn15(addr):
    a = ((addr % 0x80) * 0x200) + ((addr // 0x80) % 0x200)
    b = (addr // 0x20) % 0x10
    c = 0x6371 & (b * 0x1111)
    return a ^ c


def _pn16(addr):
    a = ((addr % 0x400) * 0x80) + ((addr // 0x400) % 0x80)
    b = (addr // 0x2000) % 2
    b += ((addr // 0x200) % 2) * 2
    b += ((addr // 0x20) % 2) * 4
    b += ((addr // 0x2) % 2) * 8
    c = (addr // 0x10) % 2
    d = 0x13659 & (c * 0x10000 + b * 0x1111)
    return a ^ d


def _pn32(addr):
    a = ((addr % 0x8000) * 0x20000) + ((addr // 0x8000) % 0x20000)
    b = (addr // 0x4) % 0x10
    c = 0xE519A4F1 & (b * 0x11111111)
    return a ^ c


class BekenCrypto:
    def __init__(self, coeffs):
        if isinstance(coeffs, str):
            coeffs = bytes.fromhex(coeffs)
        if len(coeffs) != 16:
            raise ValueError(f"encryption coefficients: {len(coeffs)} bytes, not 16")
        coef0, coef1, coef2, coef3 = struct.unpack(">IIII", coeffs)
        self.coef0 = coef0
        self.pn15_args = None
        self.pn16_args = None
        self.pn32_args = None
        self.random = 0
        self.bypass = _bits(coef3, 31, 24) in (0x00, 0xFF)
        if self.bypass:
            return
        self.coef1_mix = (_bits(coef1, 15, 8) << 9) | (_bits(coef3, 4, 4) << 8) | _bits(coef1, 7, 0)
        self.coef1_hi16 = _bits(coef1, 31, 16)
        if _bits(coef3, 3, 0) == 0xF:
            self.bypass = True
            return
        if not _bits(coef3, 0, 0):
            sel = _bits(coef3, 6, 5)
            self.pn15_args = [
                ((31, 24), (23, 16), (15, 8), (7, 0)),
                ((31, 24), (23, 16), (7, 0), (15, 8)),
                ((23, 16), (31, 24), (15, 8), (7, 0)),
                ((23, 16), (31, 24), (7, 0), (15, 8)),
            ][sel]
        if not _bits(coef3, 1, 1):
            sel = _bits(coef3, 9, 8)
            self.pn16_args = (16 + sel, sel)
        if not _bits(coef3, 2, 2):
            sel = _bits(coef3, 12, 11)
            self.pn32_args = ((0, 0), (2**8, 2**24), (2**16, 2**16), (2**24, 2**8))[sel]
        self.random = 0 if _bits(coef3, 3, 3) else coef2

    def encrypt_u32(self, addr, data):
        if self.bypass:
            return data
        pn15_v = 0
        pn16_v = 0
        pn32_v = 0
        if self.pn15_args:
            a0, a1, a2, a3 = self.pn15_args
            pn15_a = (_bits(addr, *a0) * 0x100) + _bits(addr, *a1)
            pn15_b = (_bits(addr, *a2) * 0x100) + _bits(addr, *a3)
            pn15_v = _pn15((pn15_a ^ pn15_b) ^ self.coef1_hi16)
        if self.pn16_args:
            pn16_v = _pn16(_bits(addr, *self.pn16_args) ^ self.coef1_mix)
        if self.pn32_args:
            div, mul = self.pn32_args
            pn32_a = ((addr // div) + (addr * mul)) if div else addr
            pn32_v = _pn32((pn32_a ^ self.coef0) & 0xFFFFFFFF)
        pnout = pn32_v ^ ((pn15_v * 0x10000) + (pn16_v % 0x10000)) ^ self.random
        return (data ^ pnout) & 0xFFFFFFFF


# --- RBL (the header the Beken bootloader reads) ---------------------------------------------------------------------

RBL_STRUCT = struct.Struct("<4sbbxxI16s24s24sIIII")


class Rbl:
    def __init__(self, version, encryption=0, compression=0, container_size=0, name="app"):
        self.encryption = encryption
        self.compression = compression
        self.timestamp = int(time.time())
        self.name = name.encode()
        self.version = version.encode()
        self.sn = b"0" * 23
        self.data_crc = 0
        self.data_hash = 0x811C9DC5
        self.raw_size = 0
        self.data_size = 0
        self.container_size = container_size

    @property
    def container_size_crc(self):
        return int(self.container_size + (self.container_size // 32) * 2)

    def update(self, data):
        self.data_crc = crc32(data, self.data_crc)
        for byte in data:
            if self.data_size < self.raw_size:
                self.data_hash ^= byte
                self.data_hash = (self.data_hash * 0x01000193) % 0x100000000
            self.data_size += 1

    def serialize(self):
        def pad(value, size):
            return value[:size] + b"\x00" * (size - len(value[:size]))

        data = RBL_STRUCT.pack(b"RBL\x00", self.encryption, self.compression, self.timestamp,
                               pad(self.name, 16), pad(self.version, 24), pad(self.sn, 24),
                               self.data_crc, self.data_hash, self.raw_size, self.data_size)
        return data + struct.pack("<I", crc32(data))


def with_crc(data):
    """the CRC16 after every 32 bytes (the last block padded with 0xFF)"""
    out = bytearray()
    for at in range(0, len(data), 32):
        block = data[at:at + 32]
        if len(block) < 32:
            block += b"\xff" * (32 - len(block))
        out += block + struct.pack(">H", crc16(block))
    return bytes(out)


def package_app(raw, addr, container_size, version, coeffs):
    """the application for the 'app' partition: (encrypted image with CRCs, RBL header with CRC, flash offset of the
    header from the start of the partition)"""
    crypto = BekenCrypto(coeffs)
    rbl = Rbl(version, container_size=container_size)
    rbl.raw_size = ((len(raw) + 16 + 31) // 32) * 32 + 16

    padded = raw + b"\xff" * (-len(raw) % 32)
    encrypted = bytearray()
    for at in range(0, len(padded), 4):
        word, = struct.unpack_from("<I", padded, at)
        encrypted += struct.pack("<I", crypto.encrypt_u32(addr + at, word))

    image = bytearray()
    crc_count = 0
    for at in range(0, len(encrypted), 32):
        block = bytes(encrypted[at:at + 32])
        image += with_crc(block)
        crc_count += 2
        rbl.update(block)

    tail = b"\xff" * 16
    rbl.update(tail)
    rbl.update(b"\x10" * 16)
    tail += b"\xff" * 16
    image += with_crc(tail)
    crc_count += 2 * (len(tail) // 32)

    header = with_crc(rbl.serialize())
    header_offset = to_offset(container_size) - 102
    return bytes(image), header, header_offset


def package_ota(raw, version, key, iv):
    """the OTA package for 'download': RBL header, then the image gzip'ed and AES-256-CBC encrypted"""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    rbl = Rbl(version, encryption=2, compression=1)
    rbl.raw_size = len(raw)
    rbl.update(raw)

    data = gzip.compress(raw, compresslevel=9, mtime=0)
    padding = 16 - (len(data) % 16)
    data += bytes([padding] * padding)
    encryptor = Cipher(algorithms.AES(key.encode()), modes.CBC(iv.encode())).encryptor()
    data = encryptor.update(data) + encryptor.finalize()

    rbl.data_size = len(data)
    rbl.data_crc = crc32(data)
    return rbl.serialize() + data


# --- UF2 of LibreTiny's OTA format (uf2tool) ----------------------------------------------------------------------

TAG_VERSION = 0x9FC7BC
TAG_DEVICE_ID = 0xC8A729
TAG_OTA_FORMAT_1 = 0x5D57D0
TAG_OTA_FORMAT_2 = 0x6C8492
TAG_OTA_PART_LIST = 0x6EC68A
TAG_OTA_PART_INFO = 0xC0EE0C
TAG_BOARD = 0xCA25C8
TAG_FIRMWARE = 0x00DE43
TAG_BUILD_DATE = 0x822F30
TAG_FAL_PTABLE = 0x8288ED
TAG_LEGACY_PART_1 = 0x805946
TAG_LEGACY_PART_2 = 0xA1E4D7
TAG_LEGACY_HAS_OTA1 = 0xBBD965
TAG_LEGACY_HAS_OTA2 = 0x92280E

FLAG_NOT_MAIN_FLASH = 0x00000001
FLAG_FAMILY_ID = 0x00002000
FLAG_HAS_TAGS = 0x00008000

# the OTA schemes, in their order: device single / dual 1 / dual 2, flasher single / dual 1 / dual 2
SCHEMES = 6
BLOCK_DATA = 256


def _tags(tags):
    out = b""
    for key, value in tags.items():
        size = 4 + len(value)
        out += bytes([size]) + struct.pack("<I", key)[:3] + value
        out += b"\x00" * (-size % 4)
    return out


def _block(address, data, tags, flags, seq, count):
    if tags:
        flags |= FLAG_HAS_TAGS
    out = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, flags | FLAG_FAMILY_ID, address, len(data), seq, count,
                      FAMILY_BK7231N)
    out += data + (_tags(tags) if tags else b"") + b"\x00\x00\x00\x00"
    if len(out) > 508:
        raise ValueError("UF2 block too long")
    return out + b"\x00" * (508 - len(out)) + struct.pack("<I", 0x0AB16F30)


class Uf2Image:
    """`file[+offset]=target:part`, target device or flasher (one file, one partition per target)"""

    def __init__(self, spec):
        file, target = spec.split("=")
        file, _, offset = file.partition("+")
        self.file = file
        self.offset = int(offset or "0", 0)
        self.schemes = [None] * SCHEMES
        for item in target.split(";"):
            kind, part = item.split(":")
            self.schemes[3 if kind == "flasher" else 0] = part
        names = sorted(set(filter(None, self.schemes)))
        indexes = "".join(str(names.index(name) + 1) if name else "0" for name in self.schemes)
        self.part_info = bytes.fromhex(indexes) + b"\x00".join(name.encode() for name in names) + b"\x00"

    def data(self):
        with open(self.file, "rb") as f:
            return f.read()


def write_uf2(output, board, specs, firmware, date, legacy=True):
    """the UF2 of the images `specs` (Uf2Image strings) for the board (its json: name, flash)"""
    images = [Uf2Image(spec) for spec in specs]

    header = {}
    header[TAG_BOARD] = board["name"].lower().encode()
    header[TAG_DEVICE_ID] = struct.pack("<I", crc32(f"ESPurna {board['name'].lower()}".encode()))
    name, _, version = firmware.partition(":")
    header[TAG_FIRMWARE] = name.encode()
    if version:
        header[TAG_VERSION] = version.encode()
    header[TAG_BUILD_DATE] = struct.pack("<I", date)

    # the FAL partition table of the partitions the images use (struct fal_partition, names of 16)
    used = set()
    for image in images:
        used.update(filter(None, image.schemes))
    table = []
    for part, value in board["flash"].items():
        if part in used:
            offset, length = (int(x, 16) for x in value.split("+"))
            table.append((offset, part, length))
    table.sort()
    ptable = b"".join(struct.pack("<I16s16sII4x", 0x45503130, part.encode(), b"flash0", offset, length)
                      for offset, part, length in table)
    if len(ptable) > 255:
        raise ValueError("UF2 partition table too long")
    header[TAG_FAL_PTABLE] = ptable
    header[TAG_OTA_FORMAT_2] = bytes([2])

    blocks = []
    schemes = set()
    for image in images:
        tags = {}
        device = image.schemes[0] or ""
        flasher = image.schemes[3] or ""
        if legacy and (device or flasher):
            tags[TAG_LEGACY_PART_1] = flasher.encode()
            tags[TAG_LEGACY_PART_2] = device.encode()
            header[TAG_LEGACY_HAS_OTA1] = bytes([0])
            header[TAG_LEGACY_HAS_OTA2] = bytes([1])
            header[TAG_OTA_FORMAT_1] = bytes([1])
        for index, part in enumerate(image.schemes):
            if part:
                schemes.add(index)
        tags[TAG_OTA_PART_INFO] = image.part_info
        data = image.data()
        for at in range(0, len(data), BLOCK_DATA):
            blocks.append((image.offset + at, data[at:at + BLOCK_DATA], tags))
            tags = {}

    header[TAG_OTA_PART_LIST] = bytes.fromhex("".join("1" if index in schemes else "0" for index in range(SCHEMES)))

    count = len(blocks) + 1
    with open(output, "wb") as f:
        f.write(_block(0, b"", header, FLAG_NOT_MAIN_FLASH, 0, count))
        for seq, (address, data, tags) in enumerate(blocks, start=1):
            f.write(_block(address, data, tags, 0, seq, count))
    return output


# --- the head's images, after its link -------------------------------------------------------------------------------

def head_images(folder, board, raw, mcu="bk7231n"):
    """the images of the head (firmware.bin) for the board: the paths of (crc, rblh, ota rbl), the RBL header offset"""
    version = f"{datetime.now():%y.%m.%d}-{mcu}"
    build = board["build"]
    app_offset = int(board["flash"]["app"].split("+")[0], 16)
    container = int(build["bkrbl_size_app"], 16)
    if len(raw) > container:
        raise ValueError(f"the head ({len(raw)} bytes) does not fit the application ({container})")

    image, header, header_offset = package_app(raw, int(build["bkoffset_app"], 16), container, version,
                                               build.get("bkcrypt_coeffs") or ("0" * 32))
    ota = package_ota(raw, version, build["bkota"]["key"], build["bkota"]["iv"])
    download = int(board["flash"]["download"].split("+")[1], 16)
    if len(ota) > download:
        raise ValueError(f"the OTA image ({len(ota)} bytes) does not fit 'download' ({download})")

    paths = (
        os.path.join(folder, f"image_{mcu}_app.0x{app_offset:06X}.crc"),
        os.path.join(folder, f"image_{mcu}_app.0x{app_offset + header_offset:06X}.rblh"),
        os.path.join(folder, f"image_{mcu}_app.ota.rbl"),
    )
    for path, data in zip(paths, (image, header, ota)):
        with open(path, "wb") as f:
            f.write(data)
    return paths, header_offset


def uf2_inputs(paths, header_offset):
    crc, rblh, ota = paths
    return [f"{crc}=flasher:app", f"{rblh}+{header_offset}=flasher:app", f"{ota}=device:download"]


# a post: script of the env (an import as the library of the other scripts registers nothing)
if __name__ != "beken_image" and "SCons.Script" in sys.modules:
    from SCons.Script import Import

    Import("env")  # noqa: F821

    def _images(target, source, env):
        folder = env.subst("$BUILD_DIR")
        # the board of the platform (igorufox/platform-beken boards/), not a copy in the project: the platforms/ folder
        # of the project is gone since the platform is its own repository (the Beken build stopped here 20.11)
        board = env.BoardConfig().manifest
        with open(os.path.join(folder, "board.json"), "w", encoding="utf-8") as f:
            json.dump(board, f, indent=1)
        with open(str(source[0]), "rb") as f:
            raw = f.read()
        paths, header_offset = head_images(folder, board, raw)
        now = datetime.now()
        write_uf2(str(target[0]), board, uf2_inputs(paths, header_offset), f"espurna:{now:%y.%m.%d}",
                  int(now.timestamp()))
        # the inputs of the UF2 for scripts/beken_module_link.py (the initial image with the modules on)
        env["UF2OTA"] = uf2_inputs(paths, header_offset)

    # once: the post: script of the env; scripts/beken_module_link.py imports this file for its functions only
    _env = env  # noqa: F821
    if not _env.get("ESPURNA_BEKEN_IMAGE"):
        _env["ESPURNA_BEKEN_IMAGE"] = True
        _uf2 = _env.Command(os.path.join("$BUILD_DIR", "firmware.uf2"), os.path.join("$BUILD_DIR", "firmware.bin"),
                            _env.VerboseAction(_images, "Building the Beken images $TARGET"))
        _env.Default(_uf2)
