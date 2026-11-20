/*

WIFI MODULE, the shared driver of the FreeRTOS chips (ESP32, Beken)

wifi_driver_rtos.cpp implements every function of wifi_driver.h on the SDK calls of the chip (wifi_sdk.h, no WiFi class):
WiFi events arrive from another task, the handler queues each one whole (espurna::system::EventQueue) and loop() delivers
them to the common code in the order they came, so the state machine in wifi.cpp runs in loop() only. The link state (connecting / linked), the last connection request, the
scan and the AP settings are kept here. What only the chip's SDK knows is behind `chip::` below, ref.
arch/esp32/wifi_driver_esp32.cpp and arch/beken/wifi_driver_beken.cpp.

*/

#pragma once

#include "text_types.h"

#include "../../wifi_driver.h"

namespace espurna {
namespace wifi {
namespace driver {
namespace rtos {

// espurna::time::ms() based, safe across the wrap
inline bool expired(uint32_t since, uint32_t timeout) {
    return static_cast<int32_t>(espurna::time::ms() - since) >= static_cast<int32_t>(timeout);
}

// 802.11 reasons: 'unspecified' (the AP dropped us), 'disassociated because sending STA is leaving' (our own)
constexpr int ReasonUnspecified { 1 };
constexpr int ReasonAssocLeave { 8 };

// A scan ends after this long at the latest. WiFi.scanComplete() has its own timeout of max_ms_per_chan * 20 (6s by
// default) and reports WIFI_SCAN_FAILED after it, even though the scan is still going and results arrive later.
// Scanning 13 channels while STA is connected (driver hops back to the home channel) easily takes longer than that
constexpr uint32_t ScanTimeout { 15000 };

// The scan the shared part started (ref. chip::scan_finished())
struct Scan {
    // espurna::time::ms() of the start
    uint32_t at { 0 };

    // started while the station was connected
    bool linked { false };

    // the SDK's 'scan done' event came after the start and was not taken yet, when
    bool done { false };
    uint32_t done_at { 0 };

    // the chip's: a 'done' event of this very scan was taken
    bool done_ok { false };
};

// what the shared part keeps, for the chip's own functions
struct State {
    uint8_t mode { OpmodeNull };
    sleep_type_t sleep { NONE_SLEEP_T };
    float tx_power { WIFI_OUTPUT_POWER_DBM };

    // connected with an address, as reported to the common code
    bool linked { false };
    Mac linked_bssid{};

    // the AP settings of the last ap_start()
    espurna::string ap_ssid;
    espurna::string ap_passphrase;
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

// the power save the driver runs now ("none", "modem (min)" ...), WIFI.DEBUG
const char* sleep_name();

// stop the station: no association, no retries by the SDK
void stop_station();

// the station lost the link or the attempt failed (before the common code is told): a chip whose SDK reconnects by
// itself stops it here
void dropped();

// A disconnect of the connected station, what the common code is told. ESP-IDF passes the 802.11 reason and the AP
// of the SDK as they are. The BDK gives no reason of its own: an unexpected drop is 'unspecified' (the AP dropped
// us), a drop right after our own AP change is not the AP's doing, 'no AP found' is a scan result, not about the link
struct LinkLost {
    int reason;
    Mac bssid;
    bool ours;
};

// false: not about the link, the event is ignored
bool map_reason(int reported, const Mac& reported_bssid, const Mac& linked_bssid, LinkLost& out);

// A disconnect while the connection attempt runs
struct AttemptDisconnect {
    int reason;

    // what the SDK reported, and the AP of the attempt
    Mac bssid;
    Mac target;

    // within the stale window right after begin(): the disconnect of the previous attempt reporting itself late
    bool stale;

    // the station associated in this attempt
    bool associated;

    // the attempt was started again once already
    bool begun_again;
};

enum class Attempt {
    // the attempt is over, `bssid` is the AP for the common code
    Failed,

    // about the previous attempt or link, nothing changes
    Ignored,

    // the same attempt is started again right away (BDK: its late 'leaving' of the previous association drops the new
    // attempt, nothing else comes until the attempt timeout)
    BeginAgain,
};

Attempt attempt_event(const AttemptDisconnect&, Mac& bssid);

const char* reason_name(int reason);

// the security of the current connection as negotiated ("WPA3-PSK", "WPA2-PSK", "OPEN"), empty when unknown
const char* sta_security();

// the name of an auth mode only this chip's SDK has (ESP-IDF and BDK: WAPI), nullptr for any other
const char* auth_name(int mode);

// Polled while our scan runs. false while it goes on, otherwise `count` is the result (negative: failed, no results).
// ESP-IDF: WiFi.scanComplete(). BDK: the SDK's 'scan done' event (LibreTiny reports results of other scans as well),
// a scan while connected without it gave no results
bool scan_finished(rtos::Scan&, int16_t& count);

// right before WiFi.begin(), e.g. to move our AP to the channel of the target
void before_connect(const StaConnect&);

// right before the AP is (re)started with 192.168.4.1/24, returns the channel for WiFi.softAP(): the requested one, or
// what the chip's SDK needs instead (Beken: 0, the SDK picks it)
uint8_t ap_prepare(uint8_t channel);

// after WiFi.softAP() returned, before the sleep setting is applied again
void ap_started();

// the AP is being stopped (dropping the AP bit from the opmode is what stops it, ref. wifi.cpp ap::disable())
void ap_stop();
size_t ap_stations();
size_t print_stations(::espurna::Output&);

void ipv6(bool);

} // namespace chip
} // namespace driver
} // namespace wifi
} // namespace espurna
