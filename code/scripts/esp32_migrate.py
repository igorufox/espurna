# The migration of ESP32 boards both ways (owner 22.11: «не может быть что для ESP8266 есть миграция для ESP32 нет»,
# «унификация»): between the safeboot layout of the Arduino builds (esp32_4mb_safeboot.csv up to 2026-11-17: safeboot in
# factory 0x10000, ESPurna in app0 0xE0000) and the one app partition of this build (esp32_4mb.csv: app0 0x10000). Both
# keep nvs 0x9000, otadata 0xE000, spiffs 0x3B0000 (the settings pool in its top) and coredump at the same places, so the
# settings and the NVS (Matter's keys) stay. The tool of both ways is the ESP8266's: an RBL package of an image of the
# flash from 0x0 (scripts/esp8266_rbl.py --flash, ESPURNA_RBL_FLASH), written by the bootloader step
# (arch/rtos/bootloader/espurna_rbl.c) from the top sector down, itself last; the NVS kept (chunks of length 0).
#
# Over (old -> this build), espurna-esp32-migrate.rbl, built with the head:
#   1. the old ESPurna's update (its safeboot page) takes this build's firmware.bin (0xE9): safeboot writes it into app0
#      at 0xE0000 and this head starts there, under the old bootloader
#   2. its /upgrade takes espurna-esp32-migrate.rbl: the image of this build's flash from 0x0 (bootloader 0x1000,
#      partition table 0x8000, the head 0x10000; nvs kept, otadata blank: the old one may hold the state of an image
#      safeboot wrote, this layout has no factory app and boots ota_0). The old bootloader has no RBL step, so the head
#      writes the new bootloader itself (ota_esp32.cpp: the stored sectors of the package) and restarts; the new
#      bootloader writes the image. Then the modules by the Apply, as after every head
# Back (this build -> old), by hand from the merged image of an old release (espurna-esp32-matter.bin):
#   py -3 esp32_migrate.py back <old merged .bin> <out.rbl>
#   the image of the old flash under app0 (bootloader, partition table, safeboot; otadata blank, the first sector of app0
#   blank so the old bootloader starts safeboot), nvs kept. This head's /upgrade takes it, the bootloader writes it;
#   safeboot comes up on the old settings (Wi-Fi, admin password) and its page takes the old ESPurna (.bin)
#
#   py -3 esp32_migrate.py over <bootloader.bin> <partitions.bin> <firmware.bin> <out.rbl>    the build calls over()
#   py -3 esp32_migrate.py back <old merged .bin> <out.rbl>

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import esp8266_rbl  # noqa: E402

SECTOR = 0x1000
BOOTLOADER_AT = 0x1000
TABLE_AT = 0x8000
TABLE_SIZE = 0xC00
NVS = (0x9000, 0x5000)
OTADATA = (0xE000, 0x2000)
HEAD_AT = 0x10000
OLD_APP_AT = 0xE0000        # app0 of esp32_4mb_safeboot.csv
NAME = b"esp32"

# a partition table entry (esp_partition_info_t): magic 0x50AA, type, subtype, offset, size, label[16], flags
ENTRY = struct.Struct("<HBBII16sI")


def table(data):
    """[(label, type, subtype, offset, size)] of a partition table"""
    out = []
    for at in range(0, TABLE_SIZE, ENTRY.size):
        magic, kind, sub, offset, size, label, _ = ENTRY.unpack_from(data, at)
        if magic != 0x50AA:
            break
        out.append((label.rstrip(b"\0").decode(), kind, sub, offset, size))
    return out


def check_table(entries, what):
    """the places both layouts share: nvs, otadata, spiffs"""
    places = {label: (offset, size) for label, _, _, offset, size in entries}
    for label, place in (("nvs", NVS), ("otadata", OTADATA)):
        if places.get(label) != place:
            raise ValueError(f"{what}: '{label}' is not at 0x{place[0]:X} size 0x{place[1]:X}: {places.get(label)}")
    if "spiffs" not in places:
        raise ValueError(f"{what}: no 'spiffs' (the settings pool)")
    return places


def over(bootloader, partitions, head):
    """this build's flash from 0x0 to the head's end: nvs kept, otadata blank"""
    if bootloader[:1] != b"\xe9" or head[:1] != b"\xe9":
        raise ValueError("the bootloader and the head start with 0xE9")
    if BOOTLOADER_AT + len(bootloader) > TABLE_AT:
        raise ValueError("the bootloader reaches the partition table")
    places = check_table(table(partitions), "this build")
    if places.get("app0", (0, 0))[0] != HEAD_AT:
        raise ValueError("this build's app0 is not at 0x10000")

    image = bytearray(b"\xff" * (HEAD_AT + len(head)))
    image[BOOTLOADER_AT:BOOTLOADER_AT + len(bootloader)] = bootloader
    image[TABLE_AT:TABLE_AT + len(partitions)] = partitions
    image[HEAD_AT:] = head
    return esp8266_rbl.make(bytes(image), "migrate", flash=True, name=NAME, keep=[NVS])


def back(merged):
    """the old flash under its app0: bootloader, table, safeboot; otadata and app0's first sector blank, nvs kept"""
    if len(merged) < OLD_APP_AT + SECTOR:
        raise ValueError("not a merged image of the safeboot layout (too short)")
    entries = table(merged[TABLE_AT:TABLE_AT + TABLE_SIZE])
    places = check_table(entries, "the old image")
    if places.get("app0", (0, 0))[0] != OLD_APP_AT or places.get("safeboot", (0, 0))[0] != HEAD_AT:
        raise ValueError(f"not the safeboot layout (safeboot 0x10000, app0 0xE0000): {places}")
    if merged[HEAD_AT:HEAD_AT + 1] != b"\xe9":
        raise ValueError("no safeboot image at 0x10000")

    image = bytearray(merged[:OLD_APP_AT + SECTOR])
    image[OTADATA[0]:OTADATA[0] + OTADATA[1]] = b"\xff" * OTADATA[1]
    image[OLD_APP_AT:] = b"\xff" * SECTOR
    return esp8266_rbl.make(bytes(image), "back", flash=True, name=NAME, keep=[NVS])


def save(rbl, out):
    with open(out, "wb") as f:
        f.write(rbl)
    print(f"  migrate: {out}: image {struct.unpack_from('<I', rbl, 84)[0]}, package {len(rbl) - 96}")
    return out


def read(path):
    with open(path, "rb") as f:
        return f.read()


if __name__ == "__main__":
    if (len(sys.argv) == 6) and (sys.argv[1] == "over"):
        save(over(read(sys.argv[2]), read(sys.argv[3]), read(sys.argv[4])), sys.argv[5])
    elif (len(sys.argv) == 4) and (sys.argv[1] == "back"):
        save(back(read(sys.argv[2])), sys.argv[3])
    else:
        print(__doc__ or "usage: esp32_migrate.py over <bootloader> <partitions> <firmware> <out> | back <merged> <out>")
        sys.exit(1)
