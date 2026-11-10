/*

OTA MODULE, chips where Update writes every upload (ref. ota_driver.h)

Nothing blocks an upload and nothing is written by the chip itself: every own_...() function is a constant.
Included by arch/esp8266/ota_driver_esp8266.h and arch/esp32/ota_driver_esp32.h

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

constexpr const char* upload_blocked() {
    return nullptr;
}

inline void upload_blocked_done() {
}

constexpr OwnUpload own_begin(const uint8_t*, size_t) {
    return OwnUpload{false, 0, nullptr};
}

constexpr bool own_running() {
    return false;
}

constexpr const char* own_name() {
    return "";
}

constexpr const char* own_write(const uint8_t*, size_t) {
    return nullptr;
}

inline const char* own_finish(bool, bool&) {
    return nullptr;
}

inline void own_abort() {
}

} // namespace driver
} // namespace ota
} // namespace espurna
