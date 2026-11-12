/*

WIFI MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#pragma once

#include <Arduino.h>

// SDK, lwIP and WiFi class headers, AUTH_MODE, driver::AuthOpen and driver::ScanResult
#if defined(ESP8266)
#include "arch/esp8266/wifi_types_esp8266.h"
#else
#include "arch/rtos/wifi_types_rtos.h"
#endif

namespace espurna {
namespace wifi {

using Mac = std::array<uint8_t, 6>;

struct StaNetwork {
    Mac bssid;
    String ssid;
    String passphrase;
    int8_t rssi;
    uint8_t channel;
};

struct SoftApNetwork {
    Mac bssid;
    String ssid;
    String passphrase;
    uint8_t channel;
    AUTH_MODE authmode;
};

enum class Event {
    Initial,              // aka boot
    Mode,                 // when opmode changes
    StationInit,          // station initialized by the connetion routine
    StationScan,          // scanning before the connection
    StationConnecting,    // network was selected and connection is in progress
    StationConnected,     // successful connection
    StationDisconnected,  // disconnected from the current network
    StationTimeout,       // timeout after the previous connecting state
    StationReconnect      // timeout after all connection loops failed
};

using EventCallback = void(*)(Event event);

enum class BootMode {
    Disabled,
    Enabled,
};

enum class StaMode {
    Disabled,
    Enabled,
};

enum class ApMode {
    Disabled,
    Enabled,
    Fallback,
};

} // namespace wifi

namespace settings {
namespace internal {

String serialize(wifi::Mac);

} // namespace internal
} // namespace settings
} // namespace espurna

// Note that 'connected' status is *only* for the WiFi STA.
// Overall connectivity depends on low-level network stack and it may be
// useful to check whether relevant interfaces are up and have a routable IP
// instead of exclusively depending on the WiFi API.
// (e.g. when we have injected ethernet, wireguard, etc. interfaces.
// esp8266 implementation specifically uses lwip, ref. `netif_list`)
bool wifiConnected();

// When AP is up and running
espurna::wifi::SoftApNetwork wifiApInfo();
bool wifiConnectable();
size_t wifiApStations();
IPAddress wifiApIp();
// the wifiApCaptive setting, for the captive module (captive.cpp): DNS + connectivity check redirects while the AP is on
bool wifiApCaptive();

// Current STA connection
espurna::wifi::StaNetwork wifiStaInfo();
String wifiStaSsid();
IPAddress wifiStaIp();

// Request to change the current STA / AP status
// Current state persists until reset or configuration reload
void wifiStartAp();
void wifiToggleAp();
void wifiToggleSta();

// Disconnects STA intefrace, will trigger reconnection
void wifiDisconnect();

// Toggle WiFi modem
bool wifiDisabled();
void wifiDisable();

void wifiTurnOff();
void wifiTurnOn();

// Trigger fallback check for the AP
void wifiApCheck();

void wifiRegister(espurna::wifi::EventCallback);

// IPv6 on the station (Matter runs over link-local IPv6). Applied at once when connected, kept for the next connection.
// Nothing on chips where IPv6 is a build option of lwIP (ESP8266, Beken)
void wifiIpv6(bool);

void wifiSetup();
