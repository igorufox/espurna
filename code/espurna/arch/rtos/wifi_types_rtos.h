/*

WIFI MODULE, types of the FreeRTOS chips (ESP32, BK72xx with LibreTiny), ref. espurna_wifi.h

Arduino WiFi class of the core, the esp8266 SDK names the common code uses

*/

#pragma once

#include <Arduino.h>
#include <WiFi.h>

// ESP32: SDK calls of arch/esp32/wifi_driver_esp32.cpp
#if __has_include(<esp_wifi.h>)
#include <esp_wifi.h>
#endif

using AUTH_MODE = wifi_auth_mode_t;

namespace espurna {
namespace wifi {
namespace driver {

static constexpr AUTH_MODE AuthOpen { WIFI_AUTH_OPEN };

// Same fields as the esp8266 SDK bss_info, i.e. ssid is not null-terminated
struct ScanResult {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;
    AUTH_MODE authmode;
};

} // namespace driver
} // namespace wifi
} // namespace espurna
