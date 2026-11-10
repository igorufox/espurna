/*

MDNS MODULE, esp8266 part (ref. mdns_driver.h)

LEAmDNS of the Core: polled from the loop, queries are synchronous

*/

#pragma once

#include <Arduino.h>
#include <ESP8266mDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

inline bool begin(const String& hostname) {
    if (MDNS.begin(hostname)) {
        espurnaRegisterLoop([]() {
            MDNS.update();
        });
        return true;
    }

    return false;
}

inline bool running() {
    return MDNS.isRunning();
}

inline bool enable_arduino(uint16_t port, bool auth) {
    return MDNS.enableArduino(port, auth);
}

inline bool query(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
    bool result { false };

    auto found = MDNS.queryService(service, protocol);
    for (decltype(found) n = 0; n < found; ++n) {
        if (callback(MDNS.IP(n).toString(), MDNS.port(n))) {
            result = true;
            break;
        }
    }

    MDNS.removeQuery();

    return result;
}

// 2.7.x require MDNS.begin() when interface is UP
//       note that end() aka close() would clear internal state
// 3.x.x only need to do MDNS.begin() once at setup()
//       interface mode changes are handled by the sdk
// note that calling mdns methods *before* MDNS.begin() is prone to crashing
inline void setup(void (*start)()) {
#if defined(ARDUINO_ESP8266_RELEASE_2_7_2) \
    || defined(ARDUINO_ESP8266_RELEASE_2_7_3) \
    || defined(ARDUINO_ESP8266_RELEASE_2_7_4)
    static void (*callback)() = start;
    wifiRegister([](espurna::wifi::Event event) {
        if ((event == espurna::wifi::Event::StationConnected) && !MDNS.isRunning()) {
            callback();
        }
    });
#else
    start();
#endif
}

} // namespace driver
} // namespace mdns
} // namespace espurna
