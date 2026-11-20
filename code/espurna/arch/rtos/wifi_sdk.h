/*

WIFI: the calls of the chip's SDK, the same names on every FreeRTOS chip. ESPurna's driver (wifi_driver_rtos.cpp) calls
them directly, the WiFi class of lib/espurna-core (third-party libraries only) as well. Implemented by the chip:
arch/esp32/core_esp32.cpp, arch/esp8266/core_esp8266.cpp, arch/beken/core_wifi_beken.cpp. The SDK types (wifi_mode_t,
WIFI_MODE_*, wifi_auth_mode_t, WIFI_AUTH_*, wifi_err_reason_t) are the chip's own: arch/<chip>/core_wifi_chip.h

IPv4 addresses in network order (lwIP's), 0 = none

*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "core_wifi_chip.h"

// the scan count while there are no results
#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif
#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED (-2)
#endif

namespace espurna {
namespace core {
namespace chip {

struct WifiInfo {
    uint8_t ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    wifi_auth_mode_t authmode;
};

bool wifi_mode(wifi_mode_t mode);
wifi_mode_t wifi_get_mode();
bool wifi_sleep(bool enabled);
bool wifi_sta_begin(const char* ssid, const char* passphrase, uint8_t channel, const uint8_t* bssid);
bool wifi_sta_disconnect();
bool wifi_sta_config(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns1, uint32_t dns2);
bool wifi_sta_address(uint32_t& ip, uint32_t& netmask, uint32_t& gateway);
bool wifi_sta_info(WifiInfo& out);
bool wifi_hostname(const char* hostname);
const char* wifi_get_hostname();
bool wifi_mac(bool ap, uint8_t* out);
bool wifi_set_mac(bool ap, const uint8_t* mac);
uint8_t wifi_channel();
bool wifi_ipv6(bool enable);
const char* wifi_reason_name(int reason);

// scan: count() is WIFI_SCAN_RUNNING / WIFI_SCAN_FAILED while there are no results
bool wifi_scan_start(bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel);
int16_t wifi_scan_count();
bool wifi_scan_record(uint16_t index, WifiInfo& out);
void wifi_scan_delete();

bool wifi_ap_start(const char* ssid, const char* passphrase, uint8_t channel, bool hidden, uint8_t max_connection);
bool wifi_ap_config(uint32_t ip, uint32_t gateway, uint32_t netmask);
bool wifi_ap_stop();
uint32_t wifi_ap_ip();
bool wifi_ap_ssid(uint8_t (&out)[33]);
uint8_t wifi_ap_stations();

// The events the driver takes, from the SDK's event task: one handler (ESPurna's driver)
enum class WifiEvent : uint8_t {
    StaConnected,
    StaDisconnected,
    StaGotIp,
    ScanDone,
};

struct WifiEventInfo {
    uint8_t bssid[6];
    uint8_t reason;
};

using WifiEventHandler = void (*)(WifiEvent, const WifiEventInfo&);
void wifi_on_event(WifiEventHandler handler);

} // namespace chip
} // namespace core
} // namespace espurna
