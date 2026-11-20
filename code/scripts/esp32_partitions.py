#!/usr/bin/env python3
# The ESP32 partition table of the build (esp32_4mb.csv): esp32_merge.py takes the offset of the application
# partition from it. No SCons here: esp32_merge.py imports it.

import collections

TABLE_OFFSET = 0x8000

TYPES = {"app": 0x00, "data": 0x01}
SUBTYPES = {
    "app": {"factory": 0x00, "test": 0x20, **{f"ota_{index}": 0x10 + index for index in range(16)}},
    "data": {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04, "efuse": 0x05,
             "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81, "spiffs": 0x82, "littlefs": 0x83},
}
APP_ALIGN = 0x10000
DATA_ALIGN = 0x1000

Entry = collections.namedtuple("Entry", "label type subtype offset size")


def number(text):
    """0x9000, 36864, 20K, 2M"""
    text = text.strip()
    scale = 1
    if text[-1:].upper() in ("K", "M"):
        scale = 1024 if text[-1:].upper() == "K" else 1024 * 1024
        text = text[:-1]
    return int(text, 0) * scale


def read_csv(path):
    """[Entry] of a partition csv; an empty offset follows the entry before it (aligned as gen_esp32part.py does)"""
    out = []
    end = TABLE_OFFSET + 0x1000
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [col.strip() for col in line.split(",")]
            if len(cols) < 5:
                raise ValueError(f"{path}: not a partition line: {line}")
            label, kind, subtype, offset, size = cols[:5]
            ptype = TYPES[kind] if kind in TYPES else number(kind)
            psub = SUBTYPES.get(kind, {}).get(subtype)
            if psub is None:
                psub = number(subtype)
            if offset:
                at = number(offset)
            else:
                align = APP_ALIGN if ptype == TYPES["app"] else DATA_ALIGN
                at = (end + align - 1) // align * align
            length = number(size)
            out.append(Entry(label, ptype, psub, at, length))
            end = at + length
    return out


def find(entries, ptype, subtype):
    return next((entry for entry in entries if (entry.type == ptype) and (entry.subtype == subtype)), None)


def app_offset(entries):
    """the offset of the application partition: ota_0, else the first app"""
    ota0 = find(entries, TYPES["app"], SUBTYPES["app"]["ota_0"])
    if ota0:
        return ota0.offset
    apps = [entry for entry in entries if entry.type == TYPES["app"]]
    if apps:
        return apps[0].offset
    raise ValueError("no app partition")
