# The migration image of the ESP8266 (owner 20.11: «миграцию с 1.15 и 1.22 из mainstream»): one upload through the web
# /upgrade of an ESPurna 1.15 / 1.22 board (the NONOS build, Arduino core 2.7.4 / 3.x) moves it onto this build.
#
# The Updater of the Arduino core takes an image that starts with 0xE9, or a gzip; it stages it under the file system
# and eboot copies it to 0x0 on the next start - its own sector 0 too - inflating a gzip on the way, then loads the image
# at 0x0 (esp8266/Arduino Updater.cpp end(): ACTION_COPY_RAW, args = staging, 0x0, size; bootloaders/eboot/eboot.c). So the
# image here is the flash of this build from 0x0, gzip'ed:
#   0x0       the SDK's bootloader with the ESPurna step (bootloader.bin)
#   0x8000    the partition table (partition-table.bin)
#   0x9000    nvs, phy_init: blank (0xFF)
#   0x10000   the head (firmware.bin)
# It ends under 0x8C000: the settings of the NONOS build (EEPROM_Rotate, sectors 250 / 251 on 1 MB) stay where the head
# reads them (arch/esp8266/storage_flash_esp8266.cpp), the modules come after by the Apply, as on every board.
# The Updater refuses an image larger than the space between the running sketch and the file system: the gzip is
# ~75 % of the raw one (a 1.22 sketch of ~500 KB on 1 MB leaves ~500 KB).
#
# eboot does not reset after the copy: it starts the application at 0x1000 (load_app_from_flash_raw(0) reads the image
# header at 0x0 + APP_START_OFFSET, eboot.c of the core 2.7.4) - there the SDK's bootloader has its code, eboot took it
# for 146 segments and the board went silent (21.11, the first migration on the bench). So the bootloader here carries a
# padding segment first (loaded by the ROM into free IRAM, harmless) whose last bytes, at 0x1000 of the flash, are an
# image for eboot: one IRAM segment that resets the chip by the hardware watchdog, as the head's restart does
# (core_esp8266.cpp chip::restart(); not eboot's SWRST: after it 1.15 did not start, 21.11). The ROM then boots 0x0: the
# SDK's bootloader, the head.
#
#   py -3 esp8266_migrate.py <bootloader.bin> <partition-table.bin> <head.bin> <out.bin.gz>    by hand; the build calls make()

import gzip
import struct
import sys

TABLE_AT = 0x8000
HEAD_AT = 0x10000
SETTINGS_AT = 0xFA000       # sector 250, the first of the NONOS settings on 1 MB

EBOOT_APP_AT = 0x1000       # APP_START_OFFSET of eboot
TRAMPOLINE_AT = 0x40104000  # free IRAM: the SDK's bootloader has 0x40100000..0x40103000, eboot runs at 0x4010F000
# the literal (WDT_CTL 0x60000900), then: l32r a2, [lit]; l32i a4, a2, 0; movi a5, -2; and a4, a4, a5; s32i a4, a2, 0
# (off); movi a3, 1; s32i a3, a2, 4 (WDT_OP); s32i a3, a2, 8 (WDT_OP_ND); movi a5, -64; and a4, a4, a5; movi a5, 0x39;
# or a4, a4, a5; s32i a4, a2, 0 (RSTLEN 7, RSPMOD 0, EN); j .     (xtensa-esp-elf-as, the literal written by hand)
TRAMPOLINE_ENTRY = 4
TRAMPOLINE_CODE = (struct.pack("<I", 0x60000900)
                   + bytes.fromhex("21ffff" "422200" "52affe" "504410" "426200" "32a001" "326201" "326202"
                                   "52afc0" "504410" "52a039" "504420" "426200" "06ffff") + b"\x00\x00")


def segments(image):
    """an ESP8266 image: header bytes 2..3, entry, [(address, data)]"""
    if image[0] != 0xE9:
        raise ValueError("not an ESP8266 image")
    count, flash, entry = image[1], image[2:4], struct.unpack_from("<I", image, 4)[0]
    out, at = [], 8
    for _ in range(count):
        address, size = struct.unpack_from("<II", image, at)
        out.append((address, image[at + 8:at + 8 + size]))
        at += 8 + size
    return flash, entry, out


def image(flash, entry, parts):
    """the segments back into an image: the checksum byte at the end of the 16-byte padding (0xEF ^ every data byte)"""
    out = bytearray(b"\xe9" + bytes([len(parts)]) + flash + struct.pack("<I", entry))
    checksum = 0xEF
    for address, data in parts:
        out += struct.pack("<II", address, len(data)) + data
        for byte in data:
            checksum ^= byte
    out += b"\x00" * (15 - (len(out) % 16)) + bytes([checksum])
    return bytes(out)


def with_trampoline(bootloader):
    """the SDK's bootloader with the eboot image at 0x1000 inside a padding segment of its own"""
    flash, entry, parts = segments(bootloader)
    for address, data in parts:
        if (address < TRAMPOLINE_AT + 0x2000) and (address + len(data) > TRAMPOLINE_AT):
            raise ValueError(f"the bootloader's segment 0x{address:08x} reaches the padding at 0x{TRAMPOLINE_AT:08x}")
    eboot_app = (b"\xe9\x01" + flash + struct.pack("<I", TRAMPOLINE_AT + TRAMPOLINE_ENTRY)
                 + struct.pack("<II", TRAMPOLINE_AT, len(TRAMPOLINE_CODE)) + TRAMPOLINE_CODE)
    # the padding's data starts at 16 (image header 8, its segment header 8): the eboot image at EBOOT_APP_AT
    pad = b"\xff" * (EBOOT_APP_AT - 16) + eboot_app
    pad += b"\xff" * (-len(pad) % 4)
    out = image(flash, entry, [(TRAMPOLINE_AT, pad)] + parts)
    assert out[EBOOT_APP_AT:EBOOT_APP_AT + len(eboot_app)] == eboot_app
    assert segments(out)[2][1:] == parts
    return out


def make(bootloader, table, head):
    """the raw flash from 0x0 up to the end of the head, gzip'ed"""
    bootloader = with_trampoline(bootloader)
    if len(bootloader) > TABLE_AT:
        raise ValueError(f"bootloader of {len(bootloader)} bytes runs into the partition table")
    if (HEAD_AT + len(head)) > SETTINGS_AT:
        raise ValueError(f"head of {len(head)} bytes runs into the settings")
    raw = bytearray(b"\xff" * (HEAD_AT + len(head)))
    raw[0:len(bootloader)] = bootloader
    raw[TABLE_AT:TABLE_AT + len(table)] = table
    raw[HEAD_AT:HEAD_AT + len(head)] = head
    assert raw[0] == 0xE9 and raw[HEAD_AT] == 0xE9
    # mtime 0: the same build gives the same image
    return gzip.compress(bytes(raw), 9, mtime=0), len(raw)


def write(bootloader_path, table_path, head_path, out_path):
    with open(bootloader_path, "rb") as f:
        bootloader = f.read()
    with open(table_path, "rb") as f:
        table = f.read()
    with open(head_path, "rb") as f:
        head = f.read()
    data, raw = make(bootloader, table, head)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"  migrate: {out_path}: flash 0x0..0x{raw:06x}, gzip {len(data)} ({len(data) * 100 // raw} %)")
    return out_path


if __name__ == "__main__":
    write(*sys.argv[1:5])
