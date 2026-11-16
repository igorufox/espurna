/*

OTA MODULE, ESP32 part (ref. ota_driver.h)

Update writes the app image into the other app partition, every upload it takes is fine
(the default upload_check() of ota_driver.h).

*/

#pragma once

#include <Arduino.h>
#include <Update.h>

#include <esp_app_format.h>
#include <esp_task_wdt.h>
#include <sdkconfig.h>

namespace espurna {
namespace system {
namespace chip {

// system.h: a loop() pass for the stall watcher
void loop();

} // namespace chip
} // namespace system
} // namespace espurna

#include <cstddef>
#include <cstdint>

// eepromForceCommit()
#include "../../storage_eeprom.h"

namespace espurna {
namespace ota {
namespace driver {

// ESP32 Update does not unpack gzip, and flash size is not encoded the same way.
// Instead, check the app image header (esp_image_header_t): magic byte, and that the
// image was built for this chip. esp8266 image fails here as well, instead of at the end.
inline bool verify_header(const uint8_t* data, size_t len) {
    if (len < sizeof(esp_image_header_t)) {
        return false;
    }

    const auto* header = reinterpret_cast<const esp_image_header_t*>(data);
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }

    return header->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID;
}

inline size_t update_size() {
    return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

inline void update_prepare() {
}

// same as esp8266, mDNS is handled by the mdns module (and only when enabled)
template <typename T>
inline void arduino_ota_begin(T& ota) {
    ota.setMdnsEnabled(false);
    ota.begin();
}

// no backup sectors on ESP32, make sure pending settings reach NVS before the restart
inline void arduino_ota_started() {
    eepromForceCommit();
}

// ArduinoOTA receives the whole image inside of a single loop() call: the task WDT and the loop stall watcher
// (system_esp32.cpp, 60 s without a loop() pass) are both fed between the chunks, a slow upload is not aborted
inline void arduino_ota_progress() {
    esp_task_wdt_reset();
    ::espurna::system::chip::sdk::loop();
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
