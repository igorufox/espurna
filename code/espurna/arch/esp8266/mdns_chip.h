/*

MDNS MODULE, the calls of the ESP8266: LEAmDNS of the Core, polled from the loop, synchronous queries (mdns_driver.h)

*/

#pragma once

#include <ESP8266mDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

// LEAmDNS knows whether it runs (running()), begin() keeps it for the others
inline bool& started() {
    static bool value { false };
    return value;
}

inline bool begin(const String& hostname) {
    started() = MDNS.begin(hostname);
    if (started()) {
        espurnaRegisterLoop([]() {
            MDNS.update();
        });
    }

    return started();
}

inline bool running() {
    return MDNS.isRunning();
}

inline bool enable_arduino(uint16_t port, bool auth) {
    return MDNS.enableArduino(port, auth);
}

inline bool query(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
    bool result { false };

    const int found = MDNS.queryService(service, protocol);
    for (int n = 0; n < found; ++n) {
        if (callback(MDNS.IP(n).toString(), MDNS.port(n))) {
            result = true;
            break;
        }
    }

    MDNS.removeQuery();
    return result;
}

// the SDK follows the interface itself: MDNS.begin() once, at setup()
inline void setup(void (*start)()) {
    start();
}

} // namespace driver
} // namespace mdns
} // namespace espurna
