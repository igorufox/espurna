/*

OTA MODULE

Chip part of the firmware updates: one path on every chip. Every transport (/upgrade of ota_web.cpp, espota of
ota_arduinoota.cpp) feeds the one image target of ota.cpp, and the target hands the image to these functions, which
the chip implements in arch/<chip>/ota_<chip>.cpp. What the image is, and where it goes, is the chip's:
- ESP8266: the RBL of a new head (or of an image of the flash from 0x0) under the modules' directory, the bootloader
  installs it on the next boot (bootloader/espurna_rbl.c)
- ESP32: the app image into the other app partition (esp_ota_*)
- Beken: the RBL of a new head into 'download', the bootloader installs it; the .uf2 of the wire through the UF2 writer

begin(data, len, size)      the first bytes of an upload (at least its header; size 0: not known in advance). nullptr:
                            the chip takes it and has started writing; else the error, nothing was written
write(data, len)            the next bytes, in any parts
end()                       the whole image is in: checked, the next boot installs / runs it
abort()                     the upload stopped; nothing half-written is ever installed
confirm()                   once at boot: a new head confirms itself to the bootloader
web_setup()                 the chip's own web routes besides /upgrade (ESP8266: /upgrade/bootloader)

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ota {
namespace driver {

const char* begin(const uint8_t* data, size_t len, size_t size);
const char* write(const uint8_t* data, size_t len);
const char* end();
void abort();

void confirm();
void web_setup();

} // namespace driver
} // namespace ota
} // namespace espurna
