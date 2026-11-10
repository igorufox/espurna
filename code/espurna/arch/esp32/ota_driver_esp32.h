/*

OTA MODULE, ESP32 part (ref. ota_driver.h)

Update writes the app image into the other app partition. Nothing else writes an upload,
every one of the own_...() functions is a constant.

*/

#pragma once

#include <Arduino.h>
#include <Update.h>

#include <esp_app_format.h>
#include <esp_task_wdt.h>
#include <sdkconfig.h>

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

// ArduinoOTA receives the whole image inside of a single loop() call
inline void arduino_ota_progress() {
    esp_task_wdt_reset();
}

} // namespace driver
} // namespace ota
} // namespace espurna

#include "../../ota_driver_plain.h"
