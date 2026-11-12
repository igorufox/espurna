/*

OTA MODULE

Chip part of the firmware updates, ref. arch/<chip>/ota_driver_<chip>.h

Images
- verify_header(data, len)        first bytes of an image look like something Update takes
- update_size()                   Update.begin() size of an upload, when the image size is not known
- update_prepare()                right before Update.begin() of an upload received in parts

ArduinoOTA receives the whole image inside of a single loop() call
- arduino_ota_begin(ota)          start without its own mDNS responder (the mdns module handles it)
- arduino_ota_started()           before the image is written, pending settings must reach the flash
- arduino_ota_progress()          between the chunks

Web OTA (ref. ota_web.cpp)
- upload_check(data, len)         first chunk of an upload: nullptr - this board takes it, else a PSTR() error
                                  answered with 400 before anything is written (Beken: the bootloader it is for)

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#if defined(ESP8266)
#include "arch/esp8266/ota_driver_esp8266.h"
#elif defined(ESP32)
#include "arch/esp32/ota_driver_esp32.h"
#elif defined(LIBRETINY)
#include "arch/beken/ota_driver_beken.h"
#endif

// Chips where Update writes every upload it takes: every one of them is fine for the board
#if !defined(LIBRETINY)
namespace espurna {
namespace ota {
namespace driver {

constexpr const char* upload_check(const uint8_t*, size_t) {
    return nullptr;
}

} // namespace driver
} // namespace ota
} // namespace espurna
#endif
