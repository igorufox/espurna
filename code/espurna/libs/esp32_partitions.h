/*

The ESP-IDF partition table in binary (the table at 0x8000 of the flash and of a merged image), one version for the
ESP32 images: ESPurna (arch/esp32/safeboot_esp32.cpp: the migration writes a table, an uploaded merged image is
looked up) and code/safeboot-app (a merged image is looked up). The csv side is scripts/esp32_partitions.py, which also
writes the entries of the build's csv as esp32_partition_csv.h (ESPURNA_PARTITION_ENTRIES). Plain C++, no framework.

An entry is 32 bytes: magic 0xAA 0x50, type, subtype, offset, size (little endian), label (16 bytes), flags; the table
ends at the first entry without the magic (the MD5 entry 0xEB 0xEB, or 0xFF).

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espurna {
namespace esp32_partitions {

constexpr uint32_t TableOffset { 0x8000 };
constexpr size_t TableSize { 0xC00 };
constexpr size_t EntrySize { 32 };

constexpr uint8_t TypeApp { 0x00 };
constexpr uint8_t TypeData { 0x01 };
constexpr uint8_t SubtypeFactory { 0x00 };
constexpr uint8_t SubtypeOta0 { 0x10 };

struct Entry {
    uint8_t type;
    uint8_t subtype;
    uint32_t offset;
    uint32_t size;
    const char* label;
};

inline uint32_t le32(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// the entry of `type` and `subtype` in the table (`size` bytes of it, from its first entry); the label is not taken
inline bool find(const uint8_t* table, size_t size, uint8_t type, uint8_t subtype, Entry& out) {
    for (size_t pos = 0; (pos + EntrySize) <= size; pos += EntrySize) {
        const uint8_t* entry = table + pos;
        if ((entry[0] != 0xAA) || (entry[1] != 0x50)) {
            break;
        }

        if ((entry[2] == type) && (entry[3] == subtype)) {
            out.type = type;
            out.subtype = subtype;
            out.offset = le32(entry + 4);
            out.size = le32(entry + 8);
            out.label = nullptr;
            return true;
        }
    }

    return false;
}

// one 32-byte entry of a table, flags 0
inline void write(uint8_t* out, const Entry& entry) {
    std::memset(out, 0, EntrySize);
    out[0] = 0xAA;
    out[1] = 0x50;
    out[2] = entry.type;
    out[3] = entry.subtype;
    for (size_t index = 0; index < 4; ++index) {
        out[4 + index] = static_cast<uint8_t>(entry.offset >> (8 * index));
        out[8 + index] = static_cast<uint8_t>(entry.size >> (8 * index));
    }

    if (entry.label) {
        std::strncpy(reinterpret_cast<char*>(out + 12), entry.label, 16);
    }
}

} // namespace esp32_partitions
} // namespace espurna
