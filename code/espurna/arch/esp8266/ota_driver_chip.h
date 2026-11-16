/*

OTA MODULE, esp8266 part (ref. ota_driver.h)

Updater takes .bin and .bin.gz images, written right after the running one, every upload it
takes is fine (the default upload_check() of ota_driver.h).

*/

#pragma once

#include <Arduino.h>
#include <Updater.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

inline bool verify_header(const uint8_t* data, size_t len) {
    if (len < 4) {
        return false;
    }

    // ref: https://github.com/esp8266/Arduino/pull/6820
    // accept gzip, let unpacker figure things out later
    if (data[0] == 0x1F && data[1] == 0x8B) {
        return true;
    }

    // Check for magic byte with a normal .bin
    if (data[0] != 0xE9) {
        return false;
    }

    // Make sure that flash config can be recognized and fit the flash
    const auto flash_config = ESP.magicFlashChipSize((data[3] & 0xf0) >> 4);
    if (flash_config && (flash_config > ESP.getFlashChipRealSize())) {
        return false;
    }

    return true;
}

inline size_t update_size() {
    return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

// written from the network callbacks, Update must not yield()
inline void update_prepare() {
    Update.runAsync(true);
}

template <typename T>
inline void arduino_ota_begin(T& ota) {
    ota.begin(false);
}

// settings are backed up to the flash sectors right away (ref. eepromBackup())
inline void arduino_ota_started() {
}

inline void arduino_ota_progress() {
}

} // namespace driver
} // namespace ota
} // namespace espurna

// Update writes every upload it takes: every one of them is fine for the board (Beken checks its own)
namespace espurna {
namespace ota {
namespace driver {

constexpr const char* upload_check(const uint8_t*, size_t) {
    return nullptr;
}

// the chip writes this upload itself, not Update (Beken: the RBL head of the install image, ref. ota_beken.cpp)
constexpr bool own_image(const uint8_t*, size_t) {
    return false;
}

constexpr const char* own_begin(const uint8_t*, size_t) {
    return nullptr;
}

constexpr const char* own_write(const uint8_t*, size_t) {
    return nullptr;
}

constexpr const char* own_end() {
    return nullptr;
}

inline void own_abort() {
}

} // namespace driver
} // namespace ota
} // namespace espurna
