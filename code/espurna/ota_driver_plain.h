/*

OTA MODULE, chips where Update writes every upload (ref. ota_driver.h)

Every upload Update takes is fine for the board.
Included by arch/esp8266/ota_driver_esp8266.h and arch/esp32/ota_driver_esp32.h

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

constexpr const char* upload_check(const uint8_t*, size_t) {
    return nullptr;
}

} // namespace driver
} // namespace ota
} // namespace espurna
