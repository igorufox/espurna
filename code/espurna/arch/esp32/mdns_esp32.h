/*

MDNS MODULE, ESP32 part (ref. mdns_driver.h)

ESPmDNS of the core, on top of the IDF mdns component: it runs in a task of its own and follows the
interfaces itself. No isRunning(), the started state is kept here

*/

#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

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
    const int found = MDNS.queryService(service, protocol);
    for (int n = 0; n < found; ++n) {
        if (callback(MDNS.address(n).toString(), MDNS.port(n))) {
            return true;
        }
    }

    return false;
}

inline void setup(void (*start)()) {
    start();
}

} // namespace driver
} // namespace mdns
} // namespace espurna
