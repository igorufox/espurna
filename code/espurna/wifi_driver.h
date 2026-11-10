/*

WIFI MODULE

Chip part of the WiFi module. wifi.cpp holds the state machine (scan, connection, roaming,
AP fallback, settings, terminal, WebUI); everything that talks to the SDK is behind these
functions, ref. arch/<chip>/wifi_driver_<chip>.cpp

Network events reach the common code through the `on_...` functions below, always from
the context of loop(). esp8266 SDK runs them in SYS, which never overlaps with loop().
Chips running WiFi in a task of their own latch the events and deliver them from loop().

*/

#pragma once

#include "espurna_wifi.h"

namespace espurna {
namespace wifi {
namespace driver {

// Same values as the esp8266 SDK (NULL_MODE, STATION_MODE, SOFTAP_MODE)
static constexpr uint8_t OpmodeNull { 0 };
static constexpr uint8_t OpmodeSta { 1 };
static constexpr uint8_t OpmodeAp { 2 };
static constexpr uint8_t OpmodeApSta { OpmodeSta | OpmodeAp };

// Sizes of the SDK ssid & passphrase buffers
static constexpr size_t SsidMax { 32 };
static constexpr size_t PassphraseMax { 64 };

#if defined(ESP8266)
static constexpr AUTH_MODE AuthOpen { AUTH_OPEN };

using ScanResult = bss_info;
#else
static constexpr AUTH_MODE AuthOpen { WIFI_AUTH_OPEN };

// Same fields as the esp8266 SDK bss_info, i.e. ssid is not null-terminated
struct ScanResult {
    uint8_t bssid[6];
    uint8_t ssid[SsidMax];
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;
    AUTH_MODE authmode;
};
#endif

#if defined(ESP8266)
inline bool ip_set(const IPAddress& ip) {
    return ip.isSet();
}
#else
// Same as the esp8266 IPAddress::isSet(), neither 'any' nor 'none'
inline bool ip_set(const IPAddress& ip) {
    const auto v4 = static_cast<uint32_t>(ip);
    return (v4 != 0) && (v4 != 0xffffffff);
}
#endif

// Station connection request. References are only valid during the sta_connect() call
struct StaConnect {
    const String& hostname;
    const String& ssid;
    const String& passphrase;

    // static address otherwise
    bool dhcp;
    const IPAddress& ip;
    const IPAddress& netmask;
    const IPAddress& gateway;
    const IPAddress& dns;

    // channel is 0 when bssid is not set
    const Mac& bssid;
    uint8_t channel;
};

// -----------------------------------------------------------------------------
// Implemented by wifi.cpp, called by the driver
// -----------------------------------------------------------------------------

// Scan found a network. Data is only valid during the call
void on_scan_result(const ScanResult&);

// Scan finished. Results were reported right before this call
void on_scan_done(bool ok, size_t count);

// Station lost the connection or the connection attempt failed. Reason is the 802.11 one
void on_station_disconnected(const Mac& bssid, int reason);

// Station got its IP address
void on_station_got_ip();

// -----------------------------------------------------------------------------
// Implemented by the driver
// -----------------------------------------------------------------------------

// Before anything else. When false, setup() should not continue (e.g. chip is restarting)
bool setup();

// Once, event handlers and the initial radio state
void init();

// From the beginning of every wifi loop(), e.g. to deliver the latched events
void loop();

// Current and requested mode, Opmode... values. opmode(mode) only requests the change,
// wakeup() is called when the radio was off (OpmodeNull) before the change
uint8_t opmode();
void opmode(uint8_t);
void wakeup();

// dBm, applied as soon as possible
void tx_power(float);

// IPv6 on the station interface (link-local at least), applied at once when the station is connected
void ipv6(bool);

sleep_type_t sleep_type();
bool sleep_type(sleep_type_t);

String authmode(AUTH_MODE);

// Station
IPAddress sta_ip();
uint8_t sta_channel();
int8_t sta_rssi();

// Current BSSID and our authmode threshold
void sta_info(Mac& bssid, AUTH_MODE& authmode);
StaNetwork sta_current();

// After the STA is enabled, nothing is supposed to connect besides us
void sta_prepare();
void sta_disconnect();

// Start the connection attempt, the result is one of the events above
bool sta_connect(const StaConnect&);

// Start the scan, results and the end of the scan are reported as the events above
bool scan_start();

// Access point
void ap_start(const char* ssid, const char* passphrase, uint8_t channel);
void ap_stop();
IPAddress ap_ip();
SoftApNetwork ap_current();
size_t ap_stations();

// Gratuitous ARP on every active interface. Returns true when anything was sent
bool garp_send();

// Terminal helpers. Return the number of the printed entries
size_t print_stations(Print&);
void print_network(Print&);

} // namespace driver
} // namespace wifi
} // namespace espurna
