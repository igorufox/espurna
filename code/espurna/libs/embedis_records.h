/*

The records of the ESPurna settings (settings_embedis.h) read in place, one version for the images that read them
without ESPurna: code/migration-app (the EEPROM_Rotate sector of the ESP8266) and code/safeboot-app (the NVS blob of the
ESP32). Plain C++, no framework.

The records go from the end of the area towards its beginning. A record, in the address order: value, value length
(2 bytes, big endian), key, key length; 0xffff instead of the key length is the free space after the last record.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espurna {
namespace embedis_records {

// offsets in the area: the record is [begin, end), its value [value, value + value_len), its key [key, key + key_len)
struct Record {
    size_t begin;
    size_t end;
    size_t key;
    size_t key_len;
    size_t value;
    size_t value_len;
};

constexpr size_t Free { 0xffff };

inline size_t length(const uint8_t* data, size_t pos) {
    return (static_cast<size_t>(data[pos - 2]) << 8) | data[pos - 1];
}

// the record that ends at `pos`, records lie between `low` and the end of the area; `pos` goes to its beginning.
// false at the end: `clean` when the record is the free space marker, not when the data stops making sense
inline bool next(const uint8_t* data, size_t low, size_t& pos, Record& out, bool& clean) {
    clean = false;
    if (pos < (low + 2)) {
        return false;
    }

    const size_t key_len = length(data, pos);
    if (key_len == Free) {
        clean = true;
        return false;
    }

    if (!key_len || ((pos - low) < (key_len + 4))) {
        return false;
    }

    const size_t key = pos - 2 - key_len;
    const size_t value_len = length(data, key);
    if ((value_len == Free) || ((key - 2 - low) < value_len)) {
        return false;
    }

    out.end = pos;
    out.key = key;
    out.key_len = key_len;
    out.value_len = value_len;
    out.value = key - 2 - value_len;
    out.begin = out.value;
    pos = out.begin;

    return true;
}

inline bool match(const uint8_t* data, const Record& record, const char* key) {
    const size_t len = std::strlen(key);
    return (record.key_len == len) && (std::memcmp(data + record.key, key, len) == 0);
}

// the record of `key` in the area [low, high)
inline bool find(const uint8_t* data, size_t low, size_t high, const char* key, Record& out) {
    size_t pos { high };
    bool clean;
    while (next(data, low, pos, out, clean)) {
        if (match(data, out, key)) {
            return true;
        }
    }

    return false;
}

} // namespace embedis_records
} // namespace espurna
