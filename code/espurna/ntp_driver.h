/*

NTP MODULE

Chip part of the NTP module. ntp.cpp holds the settings, timezone, ticks, terminal and WebUI;
everything that talks to the SNTP client of the SDK is behind these functions,
ref. arch/<chip>/ntp_driver_<chip>.cpp

*/

#pragma once

#include <Arduino.h>

#include <cstdint>

namespace espurna {
namespace ntp {
namespace driver {

// Sync notification and the station IP tracking, ref. on_synced() and on_got_ip()
void setup();

// Settings were (re)loaded
void configure();

// Time was set manually, not by the SNTP client
void time_set();

// SNTP client
bool enabled();
void init();
void stop();
void dhcp(bool);

// Name is expected to persist while the client is running, change it only while stopped
void server(const char*);

// First server, by name or by the address (e.g. from DHCP). Empty string when there is none
const char* server_name();
String server_address();

// Implemented by ntp.cpp. on_synced() is called after every sync, from loop(). on_got_ip() is
// called once the station gets its address (esp8266 from SYS, other chips from loop())
void on_synced();
void on_got_ip();

// Randomized at boot, in milliseconds
uint32_t start_delay();
uint32_t update_interval();

} // namespace driver
} // namespace ntp
} // namespace espurna
