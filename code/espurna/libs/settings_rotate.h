/*

THE ON-FLASH FORMAT OF THE SETTINGS, one for every chip and every image that reads them (ESPurna storage_eeprom.cpp,
the ESP32 safeboot, the ESP8266 migration image)

The format of EEPROM_Rotate 0.9.2, which the ESP8266 boards always had (the boards in use keep their settings as they
are): a pool of 4 KiB sectors, `base`, `base - 1`, ... `base - (pool - 1)`; each a whole copy of the settings. At 11..13
of a copy its check and its counter:
  11, 12  sum of every other byte of the sector (16 bit, big endian)
  13      counter, + 1 on every write; the newest valid copy is the one loaded (the counter wraps: 255 -> 0 is newer)
A write goes into the next sector of the pool (the current one stays valid until the new one is complete), or into
the base sector while the rotation is off (OTA).

No Arduino, no SDK: only the bytes. Where the pool is and how a sector is read / erased / written is the caller's

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace settings_rotate {

constexpr size_t SectorSize { 4096 };

constexpr size_t Offset { 11 };
constexpr size_t CrcOffset { Offset };
constexpr size_t CounterOffset { Offset + 2 };
constexpr size_t ReservedEnd { Offset + 3 };

inline uint16_t crc(const uint8_t* sector) {
    uint16_t out { 0 };
    for (size_t address = 0; address < SectorSize; ++address) {
        if ((address >= Offset) && (address < ReservedEnd)) {
            continue;
        }
        out = static_cast<uint16_t>(out + sector[address]);
    }

    return out;
}

inline bool valid(const uint8_t* sector) {
    const auto stored = static_cast<uint16_t>((sector[CrcOffset] << 8) | sector[CrcOffset + 1]);
    return crc(sector) == stored;
}

inline uint8_t counter(const uint8_t* sector) {
    return sector[CounterOffset];
}

// `value` written after `best` (the counter wraps)
inline bool newer(uint8_t value, uint8_t best) {
    return ((value < best) && ((best - value) > 128)) || ((value > best) && ((value - best) < 128));
}

// check and counter of a copy about to be written
inline void seal(uint8_t* sector, uint8_t value) {
    const auto sum = crc(sector);
    sector[CrcOffset] = static_cast<uint8_t>(sum >> 8);
    sector[CrcOffset + 1] = static_cast<uint8_t>(sum & 0xff);
    sector[CounterOffset] = value;
}

struct Found {
    bool valid;
    size_t index;    // into the pool, 0 - the base sector
    uint8_t counter;
};

// The newest valid copy of the pool into `buffer` (SectorSize bytes). `read(index, buffer)` reads the index-th sector
// of the pool, false when it cannot. Nothing valid: `buffer` holds whatever was read last
template <typename Read>
Found find(size_t pool, uint8_t* buffer, Read&& read) {
    Found out { false, 0, 0 };
    for (size_t index = 0; index < pool; ++index) {
        if (!read(index, buffer) || !valid(buffer)) {
            continue;
        }

        const auto value = counter(buffer);
        if (!out.valid || newer(value, out.counter)) {
            out = Found{ true, index, value };
        }
    }

    if (out.valid && !read(out.index, buffer)) {
        out.valid = false;
    }

    return out;
}

} // namespace settings_rotate
} // namespace espurna
