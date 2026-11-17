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
- arduino_ota_stopped()           the upload ended (an error, or the end before the restart)

Web OTA (ref. ota_web.cpp)
- upload_check(data, len)         first chunk of an upload: nullptr - this board takes it, else a PSTR() error
                                  answered with 400 before anything is written (Beken: the bootloader it is for)
- own_image(data, len)            the chip writes this upload itself instead of Update (Beken: the RBL head of the .ota),
  own_begin/write/end/abort       its writer; other chips: never

*/

#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

// arch/<chip>/ota_driver_chip.h: the chip's folder (-I of its ini)
#include "ota_driver_chip.h"

