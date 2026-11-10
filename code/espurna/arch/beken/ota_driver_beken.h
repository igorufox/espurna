/*

OTA MODULE, LibreTiny part (ref. ota_driver.h)

LibreTiny Update takes the .uf2 package (firmware.uf2). ESPurna writes its own image for every
bootloader (espurna-beken-matter.bin) and the Matter code of the Tuya layout (matter-tail.bin)
itself, ref. arch/beken/ota_beken.cpp

*/

#pragma once

#include <Arduino.h>
#include <Update.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

// Chip family and OTA scheme are checked by the Update itself. Only make sure it is an UF2 block, both magic words
inline bool verify_header(const uint8_t* data, size_t len) {
    if (len < 8) {
        return false;
    }

    const auto word = [&](size_t offset) -> uint32_t {
        return uint32_t{data[offset]}
            | (uint32_t{data[offset + 1]} << 8)
            | (uint32_t{data[offset + 2]} << 16)
            | (uint32_t{data[offset + 3]} << 24);
    };

    return (word(0) == 0x0A324655) && (word(4) == 0x9E5D5157);
}

// .uf2 package is a lot bigger than the image it carries, Update checks the OTA partition itself
inline size_t update_size() {
    return UPDATE_SIZE_UNKNOWN;
}

inline void update_prepare() {
}

// same as esp8266, mDNS is handled by the mdns module (and only when enabled)
template <typename T>
inline void arduino_ota_begin(T& ota) {
    ota.setMdnsEnabled(false);
    ota.begin();
}

inline void arduino_ota_started() {
}

inline void arduino_ota_progress() {
}

const char* upload_blocked();
void upload_blocked_done();

OwnUpload own_begin(const uint8_t* data, size_t len);
bool own_running();
const char* own_name();
const char* own_write(const uint8_t* data, size_t len);
const char* own_finish(bool complete, bool& again);
void own_abort();

} // namespace driver
} // namespace ota
} // namespace espurna
