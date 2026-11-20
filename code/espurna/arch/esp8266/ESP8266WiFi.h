// The name of the esp8266 Arduino core's Wi-Fi header, which the libraries of the modules include under ESP8266
// (custom_module_lib_flags of platformio_esp8266.ini): the one WiFi of lib/espurna-core
#pragma once

#include <WiFi.h>

#include <memory>

// the esp8266 core's handle of an event subscription (WiFi.onStation*): the libraries keep one, the layer has no such
// subscriptions
using WiFiEventHandler = std::shared_ptr<void>;
