/*

MATTER, WHAT ONLY THE ESP32 HAS (ESP-IDF 5.5)

The Matter application and its platform layer are libMatterApp.a of matter-espurna, the same on every chip; the
platform layer's API (matter_port_*: key-value store on FlashDB, Wi-Fi, events, OTA) is matter_port.cpp, the rest of
the Matter layer matter.cpp, matter_app.cpp and matter_identity.cpp. Here only what the chip has: IPv6 of the station,
the heap. No factory data on any ESP32

*/

#include "../../espurna.h"

#if MATTER_SUPPORT

#include "../../espurna_matter.h"
#include "../../matter_identity.h"

#include <esp_heap_caps.h>

// what the Matter library needs from the SDK of this firmware (FreeRTOS, lwIP, mbedTLS, the layout of the types it
// shares with it): a mismatch stops the build here. Libraries before that check have no such header
#if __has_include(<app/matter_requirements.h>)
#include <app/matter_requirements.h>
#endif

#include <cstring>

namespace espurna {
namespace matter {
namespace backend {

// the identity is made in the server init of the library (matter_identity_install())
bool prepare() {
    return true;
}

uint16_t device_type() {
    return 0;
}

void network(bool) {
}

size_t heap_total() {
    return heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
}

} // namespace backend

// The chip's part of the identity (matter_identity.cpp): no factory data on any ESP32
namespace identity {

bool factory_read(const char*, uint8_t*, size_t, size_t&) {
    return false;
}

espurna::string factory_source() {
    return espurna::string();
}

} // namespace identity
} // namespace matter
} // namespace espurna

#endif
