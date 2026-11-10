/*

MDNS MODULE, LibreTiny part (ref. mdns_driver.h)

lwIP responder: services are announced, lwIP itself follows the interface going up and down (nothing to update
from the loop). No queries and no ArduinoOTA service, no isRunning(): the started state is kept here

*/

#pragma once

#include <Arduino.h>
#include <mDNS.h>

namespace espurna {
namespace mdns {
namespace driver {

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

inline void setup(void (*start)()) {
    start();
}

} // namespace driver
} // namespace mdns
} // namespace espurna
