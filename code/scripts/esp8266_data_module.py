# PROTOTYPE (docs/esp8266-modules.md): package a file as an ESP8266 data module (no code), e.g. the WebUI page
#
#   python esp8266_data_module.py <name> <data file> <head firmware.bin> <out.module>
#
# Image: the module header (espurna/modules.h, entry 0, data_end = data size) + the data, padded to 4 bytes.
# Only flash_start / flash_end hold addresses: they are the two flash relocations of the package.

import hashlib
import struct
import sys
import zlib

HEADER = struct.Struct("<II16s16s14I")
PACKAGE = struct.Struct("<II16s16sIIIIIIII")
HEADER_MAGIC = 0x31444D45
PACKAGE_MAGIC = 0x31504D45
LINK_FLASH = 0x40400000
CHUNK = 4096


def main():
    name, data_path, head_path, out_path = sys.argv[1:5]
    with open(data_path, "rb") as f:
        data = f.read()
    with open(head_path, "rb") as f:
        head_md5 = hashlib.md5(f.read()).digest()

    size = HEADER.size + len(data)
    size += (-size) % 4
    fields = [0] * 14  # the last one: priority 0
    fields[0] = LINK_FLASH  # flash_start
    fields[1] = LINK_FLASH + size  # flash_end
    fields[4] = len(data)  # data_end: the data size
    image = HEADER.pack(HEADER_MAGIC, 1, name.encode()[:15].ljust(16, b"\0"), head_md5, *fields) + data
    image += b"\0" * (size - len(image))

    body = bytearray()
    for start in range(0, len(image), CHUNK):
        bitmap = bytearray(CHUNK // 16)
        if start == 0:
            # words 10 and 11: flash_start, flash_end (after magic, version, name, md5)
            for word in (10, 11):
                bitmap[word >> 2] |= 1 << ((word & 3) * 2)
        body += bitmap + image[start : start + CHUNK]

    header = PACKAGE.pack(PACKAGE_MAGIC, 1, name.encode()[:15].ljust(16, b"\0"), head_md5, len(image), 0, 0,
                          LINK_FLASH, 0, 0, zlib.crc32(body) & 0xFFFFFFFF, 0)
    with open(out_path, "wb") as f:
        f.write(header + body)
    print(f"data module '{name}': {len(data)} bytes of data, image {len(image)}, package {len(header) + len(body)}, "
          f"head md5 {head_md5.hex()}")


if __name__ == "__main__":
    main()
