/*

WIFI MODULE

Chip part of the WiFi module. wifi.cpp holds the state machine (scan, connection, roaming,
AP fallback, settings, terminal, WebUI); everything that talks to the SDK is behind these
functions. One driver on every chip: arch/rtos/wifi_driver_rtos.cpp on the SDK calls of arch/rtos/wifi_sdk.h, with
arch/<chip>/wifi_driver_<chip>.cpp (ESP8266, ESP32, Beken) for what only that chip's SDK knows

Network events reach the common code through the `on_...` functions below, always from the context of loop(): the
SDK runs Wi-Fi in a task of its own, the driver queues the events and delivers them from loop().

*/

#pragma once

#include "text_types.h"

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

// WPA-PSK passphrase is 8..63 characters (64 is the hex PSK)
static constexpr size_t PassphraseMin { 8 };

// AuthOpen and ScanResult (same fields as the esp8266 SDK bss_info, i.e. ssid is not null-terminated)
// are the chip's, ref. espurna_wifi.h
static_assert(sizeof(ScanResult::ssid) == SsidMax, "");

// Same as the esp8266 espurna::Ip4::isSet(), neither 'any' nor 'none'
inline bool ip_set(const espurna::Ip4& ip) {
    const auto v4 = static_cast<uint32_t>(ip);
    return (v4 != 0) && (v4 != 0xffffffff);
}

// Station connection request. References are only valid during the sta_connect() call.
// Checked by the common code before the call: ssid is 1..SsidMax, passphrase is empty or PassphraseMin..PassphraseMax
struct StaConnect {
    const espurna::string& hostname;
    const espurna::string& ssid;
    const espurna::string& passphrase;

    // static address otherwise
    bool dhcp;
    const espurna::Ip4& ip;
    const espurna::Ip4& netmask;
    const espurna::Ip4& gateway;
    const espurna::Ip4& dns;

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

espurna::string authmode(AUTH_MODE);

// Station
espurna::Ip4 sta_ip();
uint8_t sta_channel();
int8_t sta_rssi();
// what the connection negotiated: "WPA3-PSK", "WPA2-PSK", ... (STATUS of the WebUI)
const char* sta_security();

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
espurna::Ip4 ap_ip();
SoftApNetwork ap_current();
size_t ap_stations();

// Terminal helpers. Return the number of the printed entries
size_t print_stations(::espurna::Output&);
void print_network(::espurna::Output&);
// WIFI.DEBUG: what the driver does and why the last attempts ended
void print_debug(::espurna::Output&);

} // namespace driver
} // namespace wifi
} // namespace espurna
