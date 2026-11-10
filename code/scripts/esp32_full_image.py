# PROTOTYPE (docs/modules.md): the two ESP32 images of the head + modules build (the shared part:
# module_full_image.py).
#
#   initial  espurna-esp32.bin: the head (the merged image from 0x0, or the app image when no merged one is given);
#            modules are added in the WebUI's MODULES panel from espurna-esp32-modules.bin.
#            The modules switched on by default (the head's list, the captive portal) are in it too, on.
#   full     espurna-esp32-full.bin: the initial image with every module in flash and switched off. app0 is filled to
#            its end: the head, 0xFF, the modules from the top down under the directory, the directory in the last
#            sector of app0. Flash it at 0x0 by wire, or update app0 with it through the safeboot (it writes the ota_0
#            part of a merged image).
#
#            The ESP32 relocates a module on the board: its image lies in the slot exactly as linked (the placeholder
#            bases of the link are its bases in the directory) with its relocation maps behind it
#            (arch/esp32/modules_esp32.cpp arch::place). All modules do not fit app0 on 4 MB (Matter + the NSPanel are
#            1.4 MB): the ones that do not fit are left out of `full`.
#
# Used by scripts/esp32_module_link.py; also runs alone:
#   py -3 esp32_full_image.py <head firmware.bin> <modules.bin> <out dir> [<merged head image> [<app0 offset> <app0 size>]]

import os
import sys

# loaded by esp32_module_link.py or run alone: the shared scripts sit next to this file
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from module_full_image import ENTRY_DISABLED, ENTRY_UNPLACED, SECTOR, FullImage  # noqa: E402
from module_link import round_up  # noqa: E402

# esp32_4mb_safeboot.csv: app0 (ota_0)
APP_OFFSET = 0xE0000
APP_SIZE = 0x2D0000

PAGE = 0x10000  # the MMU page: the modules area starts on one (modules_esp32.cpp findArea())


class Esp32FullImage(FullImage):
    image = "esp32"  # espurna-esp32.bin (initial), espurna-esp32-full.bin
    package_version = 2  # ESP32 kinds: 1 data view, 2 RAM, 3 code view
    maps_in_slot = True

    def __init__(self, head_bin, container, out_dir, merged_bin=None, app_offset=APP_OFFSET, app_size=APP_SIZE, on=()):
        super().__init__(head_bin, container, out_dir, on)
        self.merged_bin = merged_bin
        self.app_offset = app_offset
        self.app_size = app_size

    def layout(self):
        # from 0x0 with the merged image (bootloader, partitions, safeboot, app0), else app0 alone
        if self.merged_bin:
            with open(self.merged_bin, "rb") as f:
                initial = f.read()
            base = 0
            if initial[self.app_offset: self.app_offset + len(self.head)] != self.head:
                raise ValueError("the merged image does not hold this head at the app0 offset")
        else:
            initial = self.head
            base = self.app_offset
        end = self.app_offset + self.app_size
        # modules_esp32.cpp findArea(): from the head's end on a 64 KB page to the directory, the last sector of app0
        return initial, base, end - base, round_up(self.app_offset + len(self.head), PAGE), end - SECTOR

    def module(self, name, fields, image, maps, slot):
        link_flash, link_ram, link_iram = fields[7:10]
        ram_size = round_up(fields[5], 8)
        return image, 0, ram_size, (link_flash, link_iram, link_ram), ENTRY_DISABLED | ENTRY_UNPLACED, \
            f"off (RAM {ram_size} when on)"


def build(head_bin, container, out_dir, merged_bin=None, app_offset=APP_OFFSET, app_size=APP_SIZE, on=()):
    """on: the modules switched on by default, in the initial image too"""
    return Esp32FullImage(head_bin, container, out_dir, merged_bin, app_offset, app_size, on).build()


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) < 3:
        print("usage: esp32_full_image.py <head.bin> <modules.bin> <out dir> [<merged> [<app0> <size>]]")
        sys.exit(2)
    build(args[0], args[1], args[2], args[3] if len(args) > 3 else None,
          int(args[4], 0) if len(args) > 4 else APP_OFFSET, int(args[5], 0) if len(args) > 5 else APP_SIZE)
