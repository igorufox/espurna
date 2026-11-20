/*

OTA MODULE, ESP8266 on ESP8266_RTOS_SDK (ref. ota_driver.h)

A new head comes as an RBL package (own_image(), ota_esp8266.cpp): there is one app partition ('factory'), the SDK's
bootloader installs the package on the next boot (bootloader/espurna_rbl.c). A plain app image (0xE9) goes to Update,
which has no slot for it here ("No OTA partition").

*/

#pragma once



#include <cstddef>
#include <cstdint>

// espurna::system::stall::pass()
#include "system_chip.h"

// eepromForceCommit()
#include "../../storage_eeprom.h"

namespace espurna {
namespace ota {
namespace driver {

// The app image of the SDK (esptool elf2image): the first byte is the image magic. Not gzip, and the NONOS image
// (also 0xE9, but a single image of the Arduino core) is told apart by Update at the end, not here
inline bool verify_header(const uint8_t* data, size_t len) {
    return (len >= 8) && (data[0] == 0xE9);
}

inline size_t update_size() {
    return (::espurna::core::chip::sketch_space() - 0x1000) & 0xFFFFF000;
}

inline void update_prepare() {
}

// make sure pending settings reach the flash before the restart
inline void espota_started() {
    eepromForceCommit();
}

// espota (libs/espota.h) receives the whole image inside of a single loop() call: the loop stall watcher (arch/rtos/system_rtos.cpp,
// SYSTEM_LOOP_STALL s without a loop() pass) is fed between the chunks, a slow upload is not aborted (the hardware
// watchdog is fed by the SDK's idle task)
inline void espota_progress() {
    ::espurna::system::stall::pass();
}

inline void espota_stopped() {
}

// Update writes every upload it takes
constexpr const char* upload_check(const uint8_t*, size_t) {
    return nullptr;
}

// The chip writes this upload itself, not Update: the RBL of a new head (espurna-esp8266.ota), under the modules'
// directory for the SDK's bootloader to install (ota_esp8266.cpp, rbl_esp8266.h; the Tuya bootloader's scheme)
bool own_image(const uint8_t* data, size_t len);
const char* own_begin(const uint8_t* data, size_t len);
const char* own_write(const uint8_t* data, size_t len);
const char* own_end();
void own_abort();

} // namespace driver
} // namespace ota
} // namespace espurna
