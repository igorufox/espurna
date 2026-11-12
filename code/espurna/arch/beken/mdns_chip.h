/*

MDNS MODULE, the calls of the Beken: the lwIP responder of LibreTiny, lwIP follows the interface going up and down (nothing to update from the loop); no queries and no ArduinoOTA service (mdns_driver.h)

*/

#pragma once

#include <mDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

// the library has no isRunning()
inline bool& started() {
    static bool value { false };
    return value;
}

inline bool begin(const String& hostname) {
    started() = MDNS.begin(hostname.c_str());
    return started();
}

inline bool running() {
    return started();
}

inline bool enable_arduino(uint16_t, bool) {
    return false;
}

inline bool query(const String&, const String&, MdnsServerQueryCallback) {
    return false;
}

// the SDK follows the interface itself: MDNS.begin() once, at setup()
inline void setup(void (*start)()) {
    start();
}

} // namespace driver
} // namespace mdns
} // namespace espurna
