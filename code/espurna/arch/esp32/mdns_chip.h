/*

MDNS MODULE, the calls of the ESP32: ESPmDNS of the core on top of the IDF mdns component, its own task, it follows the interfaces itself (mdns_driver.h)

*/

#pragma once

#include <ESPmDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

// the library has no isRunning()
inline bool& started() {
    static bool value { false };
    return value;
}

inline bool begin(const String& hostname) {
    started() = MDNS.begin(hostname);
    return started();
}

inline bool running() {
    return started();
}

inline bool enable_arduino(uint16_t port, bool auth) {
    MDNS.enableArduino(port, auth);
    return true;
}

inline bool query(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
    bool result { false };

    const int found = MDNS.queryService(service, protocol);
    for (int n = 0; n < found; ++n) {
        if (callback(MDNS.address(n).toString(), MDNS.port(n))) {
            result = true;
            break;
        }
    }

    return result;
}

// the SDK follows the interface itself: MDNS.begin() once, at setup()
inline void setup(void (*start)()) {
    start();
}

} // namespace driver
} // namespace mdns
} // namespace espurna
