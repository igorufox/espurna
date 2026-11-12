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

// the calls of the chip's library: arch/<chip>/mdns_chip.h (the chip's folder, -I of its ini)
#include "mdns_chip.h"
