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

Uploads the chip writes itself instead of Update (web OTA, ref. ota_web.cpp). Functions returning
const char* return nullptr on success or a PSTR() error
- upload_blocked()                no upload is possible right now, the board has to reboot first.
                                  Answered with 409, upload_blocked_done() once its request ended
- own_begin(data, len)            first chunk of an upload, whether the chip takes it
- own_running(), own_name()       an upload taken by own_begin() is being written
- own_write(data, len)            next chunk, aborted on error
- own_finish(complete, again)     last chunk was received, aborted on error.
                                  `again` - the same file has to be uploaded once more after the reboot
- own_abort()                     connection is gone

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

struct OwnUpload {
    // chip writes this upload itself
    bool own;

    // when not nullptr, the upload is rejected with this HTTP code
    int code;
    const char* error;
};

} // namespace driver
} // namespace ota
} // namespace espurna

#if defined(ESP8266)
#include "arch/esp8266/ota_driver_esp8266.h"
#elif defined(ESP32)
#include "arch/esp32/ota_driver_esp32.h"
#elif defined(LIBRETINY)
#include "arch/beken/ota_driver_beken.h"
#endif
