/*

WIFI MODULE, the shared driver of the chips with an Arduino WiFi class on an RTOS (ESP32, Beken / LibreTiny)

wifi_driver_rtos.cpp implements every function of wifi_driver.h on top of the Arduino WiFi class both cores have:
WiFi events arrive from another task, the handler only latches them and loop() delivers them to the common code, so the
state machine in wifi.cpp runs in loop() only. The link state (connecting / linked), the last connection request, the
scan and the AP settings are kept here. What only the chip's SDK knows is behind `chip::` below, ref.
arch/esp32/wifi_driver_esp32.cpp and arch/beken/wifi_driver_beken.cpp.

*/

#pragma once

#include "wifi_driver.h"

namespace espurna {
namespace wifi {
namespace driver {
namespace rtos {

// what the shared part keeps, for the chip's own functions
struct State {
    uint8_t mode { OpmodeNull };
    sleep_type_t sleep { NONE_SLEEP_T };
    float tx_power { WIFI_OUTPUT_POWER_DBM };

    // connected with an address, as reported to the common code
    bool linked { false };
    Mac linked_bssid{};

    // the AP settings of the last ap_start()
    String ap_ssid;
    String ap_passphrase;
};

State& state();

} // namespace rtos

// -----------------------------------------------------------------------------
// Implemented by the chip
// -----------------------------------------------------------------------------

namespace chip {

// before the radio is set to NULL mode and the event handler is installed
void init();

// set the mode; apply what needs the radio on (country, sleep, TX power)
void opmode(uint8_t mode);

// the current sleep and TX power settings of rtos::state(), applied when the radio allows it. TX power is applied again
// once the station got its address
void apply_sleep();
void apply_tx_power();

// stop the station: no association, no retries by the SDK
void stop_station();

// the station lost the link or the attempt failed (before the common code is told): a chip whose SDK reconnects by
// itself stops it here
void dropped();

// the SDK gives no 802.11 reason of its own (Beken): an unexpected drop of a connected station is reported as
// 'unspecified' (the AP dropped us), 'no AP found' while connected is a scan result, a 'leaving' before the station
// associated is the end of the previous attempt, a drop right after our own AP change is not the AP's doing
bool synthetic_reasons();

// a drop right after our own AP start / stop (synthetic reasons only)
bool ap_changed_recently();

const char* reason_name(int reason);

// the end of a scan is the SDK's 'scan done' event (LibreTiny reports results of other scans as well), not
// WiFi.scanComplete(); a scan while connected without it gave no results
bool scan_done_event();

// right before WiFi.begin(), e.g. to move our AP to the channel of the target
void before_connect(const StaConnect&);

void ap_start(const char* ssid, const char* passphrase, uint8_t channel);
void ap_stop();
size_t ap_stations();
size_t print_stations(Print&);

void ipv6(bool);

} // namespace chip
} // namespace driver
} // namespace wifi
} // namespace espurna
