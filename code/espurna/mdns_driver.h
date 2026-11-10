/*

MDNS MODULE

Chip part of the mDNS responder, ref. arch/<chip>/mdns_<chip>.h. Only mdns.cpp includes it, after the mDNS library
of the chip; every function is inline (mdns.cpp is the mdns module of the esp8266, ref. custom_modules).
The services are added through the MDNS object itself, addService() and addServiceTxt() are the same everywhere

- begin(hostname)                 start the responder, plus whatever keeps it running
- running()                       responder was started
- enable_arduino(port, auth)      ArduinoOTA service, false when the responder has none
- query(service, protocol, cb)    blocking query, `cb` with every result until it returns true
- setup(start)                    when to call `start` (right away, or once the station is up)

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

#include "espurna_mdns.h"

#if defined(ESP8266)
#include "arch/esp8266/mdns_esp8266.h"
#elif defined(ESP32)
#include "arch/esp32/mdns_esp32.h"
#elif defined(LIBRETINY)
#include "arch/beken/mdns_beken.h"
#endif
