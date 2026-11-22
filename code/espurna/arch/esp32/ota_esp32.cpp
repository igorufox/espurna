/*

OTA MODULE, ESP32 (ref. ota_driver.h)

The app image goes into the other app partition (chip::ota_* of core_esp32.cpp, esp_ota_*), the next boot runs it.
The first bytes are checked before anything is erased: the app image header (esp_image_header_t), its magic and that
it was built for this chip - an ESP8266 image or an RBL fails here, not at the end.

*/

#include "espurna.h"

#include "ota_driver.h"

#include <chip_sdk.h>
#include <esp_app_format.h>
#include <sdkconfig.h>

namespace espurna {
namespace ota {
namespace driver {
namespace {

bool app_image(const uint8_t* data, size_t len) {
    if (len < sizeof(esp_image_header_t)) {
        return false;
    }

    const auto* header = reinterpret_cast<const esp_image_header_t*>(data);
    return (header->magic == ESP_IMAGE_HEADER_MAGIC)
        && (header->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID);
}

} // namespace

const char* begin(const uint8_t* data, size_t len, size_t size) {
    if (!app_image(data, len)) {
        return PSTR("ERROR: Not a firmware image for this chip (magic byte, chip id)");
    }

    return ::espurna::core::chip::ota_begin(size ? size : ::espurna::core::chip::OtaSizeUnknown);
}

const char* write(const uint8_t* data, size_t len) {
    return ::espurna::core::chip::ota_write(data, len);
}

const char* end() {
    return ::espurna::core::chip::ota_end();
}

void abort() {
    ::espurna::core::chip::ota_abort();
}

// the bootloader boots the partition ota_end() set, nothing to confirm
void confirm() {
}

void web_setup() {
}

} // namespace driver
} // namespace ota
} // namespace espurna
