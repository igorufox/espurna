/*

MDNS MODULE

Chip part of the mDNS responder: only the calls of the mDNS library of the chip. Only mdns.cpp includes it;
every function is inline (mdns.cpp is the mdns module, ref. ESPURNA_MODULES of config/head.h).
The services are added through the MDNS object itself, addService() and addServiceTxt() are the same everywhere

- begin(hostname)                 start the responder, plus whatever keeps it running
- running()                       responder was started
- enable_arduino(port, auth)      ArduinoOTA service, false when the responder has none
- query(service, protocol, cb)    blocking query, `cb` with every result until it returns true
- setup(start)                    when to call `start` (right away, or once the station is up)

Libraries:
- esp8266, LEAmDNS of the Core: polled from the loop, queries are synchronous
- ESP32, ESPmDNS of the core on top of the IDF mdns component: it runs in a task of its own and follows the
  interfaces itself
- LibreTiny, lwIP responder: lwIP itself follows the interface going up and down (nothing to update from the loop).
  No queries and no ArduinoOTA service

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

#include "espurna_mdns.h"

#if defined(ESP8266)
#include <ESP8266mDNS.h>
#elif defined(ESP32)
#include <ESPmDNS.h>
#elif defined(LIBRETINY)
#include <mDNS.h>
#endif

namespace espurna {
namespace mdns {
namespace driver {

// LEAmDNS knows whether it runs, the other two libraries have no isRunning()
inline bool& started() {
    static bool value { false };
    return value;
}

inline bool begin(const String& hostname) {
#if defined(LIBRETINY)
    started() = MDNS.begin(hostname.c_str());
#else
    started() = MDNS.begin(hostname);
#endif

#if defined(ESP8266)
    if (started()) {
        espurnaRegisterLoop([]() {
            MDNS.update();
        });
    }
#endif

    return started();
}

inline bool running() {
#if defined(ESP8266)
    return MDNS.isRunning();
#else
    return started();
#endif
}

inline bool enable_arduino(uint16_t port, bool auth) {
#if defined(ESP8266)
    return MDNS.enableArduino(port, auth);
#elif defined(ESP32)
    MDNS.enableArduino(port, auth);
    return true;
#else
    (void)port;
    (void)auth;
    return false;
#endif
}

inline bool query(const String& service, const String& protocol, MdnsServerQueryCallback callback) {
#if defined(ESP8266) || defined(ESP32)
    bool result { false };

    const int found = MDNS.queryService(service, protocol);
    for (int n = 0; n < found; ++n) {
#if defined(ESP8266)
        const auto address = MDNS.IP(n);
#else
        const auto address = MDNS.address(n);
#endif
        if (callback(address.toString(), MDNS.port(n))) {
            result = true;
            break;
        }
    }

#if defined(ESP8266)
    MDNS.removeQuery();
#endif

    return result;
#else
    (void)service;
    (void)protocol;
    (void)callback;
    return false;
#endif
}

// esp8266 Core 2.7.x require MDNS.begin() when interface is UP
//       note that end() aka close() would clear internal state
// 3.x.x and the other chips only need to do MDNS.begin() once at setup()
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
