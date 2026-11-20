/*

WIFI MODULE, types of the FreeRTOS chips (ESP32, BK72xx with LibreTiny), ref. espurna_wifi.h

The SDK types of the chip (wifi_sdk.h, arch/<chip>/core_wifi_chip.h), the esp8266 SDK names the common code uses

*/

#pragma once

#include "wifi_sdk.h"

// the SDK headers of the chip (arch/<chip>/compat_sdk.h)
#include "compat_chip.h"

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
