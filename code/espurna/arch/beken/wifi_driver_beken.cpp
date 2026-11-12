/*

WIFI MODULE, BEKEN (LibreTiny) part: what only the BDK knows (ref. wifi_driver_rtos.h). The driver itself (events,
link state, connection, scan, AP settings) is the shared one, wifi_driver_rtos.cpp

BDK specifics: no 802.11 reason of the AP (synthetic reasons), a supplicant that associates again by itself unless it
is stopped, the 'scan done' event as the end of a scan, one radio for the AP and the station (the AP follows the
station channel), AP stations only from the supplicant. The watch on the IP stack (netwatch) is common, wifi.cpp.

*/

#include "../../espurna.h"

#include "../rtos/wifi_driver_rtos.h"

#include <WiFi.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/stats.h>
#include <lwip/tcpip.h>

// BDK rwnx interface (rw_msg_tx.c, rw_ieee80211.c), no public header
extern "C" {
uint8_t rwm_mgmt_vif_mac2idx(void* mac);
int rw_msg_set_power(uint8_t vif_idx, uint8_t power);
}

// BDK supplicant control (wlan_ui_pub.h). Unlike WiFi.disconnect(), which only fakes a connection loss
// (bk_wlan_connection_loss) and lets the supplicant associate again by itself, this one stops it
extern "C" int wlan_sta_disconnect(void);

// LibreTiny softAPgetStationNum() is always 0 on Beken, ask the AP part of the supplicant instead
extern "C" int wlan_ap_sta_num(int* num);

// AP channel when softAP() gets channel 0 and the station is not associated (wlan_ui.c). With channel 0
// bk_wlan_ap_init_adv() takes the channel of the station first, and the SDK moves the AP to the station
// channel by itself (CSA) when the station associates later, ref. bk_wlan_reg_csa_cb_coexist_mode()
extern "C" void bk_wlan_ap_set_default_channel(uint8_t channel);
extern "C" uint8_t bk_wlan_ap_get_channel_config(void);

namespace espurna {
namespace wifi {
namespace driver {
namespace chip {
namespace {

namespace build {

// Our own AP start / stop moves the radio, a station disconnect right after it says nothing about the AP
static constexpr uint32_t ApChangeQuiet { 15000 };

} // namespace build

namespace internal {

bool ap_started { false };
bool ap_changed { false };
uint32_t ap_changed_at { 0 };

} // namespace internal

using rtos::expired;

// SDK only has DTIM sleep (on / off), which needs the driver to be started
void apply_sleep(uint8_t mode) {
    if (mode != OpmodeNull) {
        WiFi.setSleep(rtos::state().sleep != NONE_SLEEP_T);
    }
}

int ap_station_count() {
    int num { 0 };
    if ((wlan_ap_sta_num(&num) != 0) || (num < 0)) {
        num = 0;
    }

    return num;
}

// the network watch (netwatch) is common: wifi.cpp

} // namespace

void init() {
}

void opmode(uint8_t mode) {
    WiFi.mode(static_cast<WiFiMode>(mode));
    apply_sleep(mode);
}

void apply_sleep() {
    apply_sleep(rtos::state().mode);
}

// MAC keeps a TX power limit per interface (MM_SET_POWER_REQ, in dBm), its own calibration
// tables still cap it. Interface only exists while connected or while the AP is up.
// STA interface only. Same request for the AP interface while both are up made the station drop
// right after getting an IP, every time (reason 8), and the AP never showed up
void apply_tx_power() {
    if (!rtos::state().linked) {
        return;
    }

    uint8_t mac[6];
    const auto index = rwm_mgmt_vif_mac2idx(WiFi.macAddress(mac));
    if (index == 0xff) {
        return;
    }

    const auto dbm = std::max(0l, std::min(20l, std::lround(rtos::state().tx_power)));
    rw_msg_set_power(index, static_cast<uint8_t>(dbm));
}

// Stop the supplicant, it would otherwise associate again by itself (and run its own DHCP)
void stop_station() {
    wlan_sta_disconnect();
}

void dropped() {
    stop_station();
}

bool ap_changed_recently() {
    return internal::ap_changed && !expired(internal::ap_changed_at, build::ApChangeQuiet);
}

// 'connection failed' / 'leaving' is all the SDK says for an association that is lost, ours or the AP's
bool leaving(int reason) {
    return (reason == WIFI_REASON_CONNECTION_FAIL) || (reason == WIFI_REASON_ASSOC_LEAVE);
}

// Beken SDK does not pass the reason of the AP (SM_DISCONNECT_IND reason_code is only printed by
// mhdr_disconnect_ind(), the event has none). Signal loss stays BEACON_TIMEOUT. Neither the AP: the one linked to
bool map_reason(int reported, const Mac&, const Mac& linked_bssid, LinkLost& out) {
    if (reported == WIFI_REASON_NO_AP_FOUND) {
        // Not about the link. Supplicant scan without results (a scan while associated) sets the station
        // status to 'no AP found' (Beken hostapd_intf.c wpa_get_scan_rst), the association stays
        DEBUG_MSG_P(PSTR("[WIFI] Ignoring 'no AP found' while connected\n"));
        return false;
    }

    // the AP dropped us, unless it happened right after our own AP start / stop
    out.ours = ap_changed_recently();
    out.reason = out.ours
        ? rtos::ReasonAssocLeave
        : leaving(reported)
            ? rtos::ReasonUnspecified
            : reported;
    out.bssid = linked_bssid;

    return true;
}

// Without real reasons, a 'leaving' before the station associated is the end of the previous association, not
// necessarily within the stale window. The SDK handles this late 'leaving' after our begin() and drops the new attempt
// with it, nothing else comes until the attempt timeout (15 s, twice in a row on 10.0.5.139 after WIFI.RESET,
// 2026-11-09): the same attempt starts again right away, once. A failed attempt is about the AP it was for
Attempt attempt_event(const AttemptDisconnect& disconnect, Mac& bssid) {
    if (disconnect.stale || (leaving(disconnect.reason) && !disconnect.associated)) {
        return disconnect.begun_again
            ? Attempt::Ignored
            : Attempt::BeginAgain;
    }

    bssid = disconnect.target;
    return Attempt::Failed;
}

// LibreTiny WiFiClass has no disconnectReasonName(), only the ones seen in practice
const char* reason_name(int reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_UNSPECIFIED:
        return "UNSPECIFIED";
    }

    return "UNKNOWN";
}

// Scan while associated with the 'scan done' event: results only come with the event of the scan itself. Without it
// there are no results (the supplicant gets none, reports 'no AP found' on its own and tries again)
constexpr uint32_t ScanTimeoutLinked { 5000 };

// LibreTiny reports 'scan done' for every scan results event of the supplicant, including the
// ones of its own scans and of the scan that was still running when ours was requested (2 APs
// 108 ms after the start, while a real scan of every channel takes more than a second)
constexpr uint32_t ScanTimeMin { 500 };

// The end of the scan is our own 'scan done' event: LibreTiny keeps updating the results with every scan results event,
// an earlier one may come from a different scan (ref. ScanTimeMin)
bool scan_finished(rtos::Scan& scan, int16_t& count) {
    const auto timeout = scan.linked
        ? ScanTimeoutLinked
        : rtos::ScanTimeout;

    if (!expired(scan.at, timeout)) {
        if (!scan.done) {
            return false;
        }
        scan.done = false;

        const auto after = scan.done_at - scan.at;
        if (after < ScanTimeMin) {
            DEBUG_MSG_P(PSTR("[WIFI] Ignoring scan results reported %u ms after the start\n"),
                static_cast<unsigned>(after));
            return false;
        }

        scan.done_ok = true;
    }

    // While associated, what the core has without the 'done' event is not from this scan
    count = WiFi.scanComplete();
    if (scan.linked && !scan.done_ok) {
        DEBUG_MSG_P(PSTR("[WIFI] Scan while connected got no results in %u ms\n"),
            static_cast<unsigned>(millis() - scan.at));
        count = -1;
    } else {
        DEBUG_MSG_P(PSTR("[WIFI] Scan done in %u ms, %d network(s)\n"),
            static_cast<unsigned>(millis() - scan.at), static_cast<int>(count));
    }

    return true;
}

// One radio. Station looks for the AP on its channel only, our AP beaconing elsewhere makes it
// fail with 'no AP found' every time. Move our AP there first (SDK keeps it there after associating)
void before_connect(const StaConnect& connect) {
    if (!internal::ap_started || !connect.channel || (bk_wlan_ap_get_channel_config() == connect.channel)) {
        return;
    }

    DEBUG_MSG_P(PSTR("[WIFI] Moving the AP from channel %u to %u\n"),
        static_cast<unsigned>(bk_wlan_ap_get_channel_config()),
        static_cast<unsigned>(connect.channel));

    const auto& state = rtos::state();
    driver::ap_start(state.ap_ssid.c_str(),
        state.ap_passphrase.length() ? state.ap_passphrase.c_str() : nullptr,
        connect.channel);
}

// One radio for both. With STA connected the AP has to use its channel: on the configured one
// the two kept switching the radio, STA dropped every few seconds and the AP never showed up.
// The SDK picks the channel itself (channel 0, ref. bk_wlan_ap_set_default_channel()): the station
// channel when associated, the configured one otherwise, and follows the station when it associates
uint8_t ap_prepare(uint8_t channel) {
    // A connected station keeps the radio on its channel: the AP goes there, not to the configured one. WIFI.AP while
    // connected to tiger (ch 1) started the AP on ch 13, the station dropped and every later attempt failed until a
    // reset (10.0.5.139, 2026-11-09)
    if (rtos::state().linked && WiFi.channel()) {
        channel = static_cast<uint8_t>(WiFi.channel());
    }

    bk_wlan_ap_set_default_channel(channel);

    internal::ap_changed = true;
    internal::ap_changed_at = millis();

    return 0;
}

// AP start takes a few seconds, quiet time counts from the end of it
void ap_started() {
    internal::ap_changed_at = millis();
    internal::ap_started = true;
}

void ap_stop() {
    internal::ap_changed = true;
    internal::ap_changed_at = millis();
    internal::ap_started = false;
}

// AP start blocks for a few seconds (bk_wlan_start_ap_adv) and drops the WebUI clients of the STA side: the common
// fallback check keeps a fresh AP for one interval, ref. wifi.cpp ap::fallback::check()
size_t ap_stations() {
    return static_cast<size_t>(ap_station_count());
}

size_t print_stations(Print& out) {
    const size_t count = static_cast<size_t>(ap_station_count());
    if (count) {
        out.printf_P(PSTR("%u station(s) connected\n"), count);
    }

    return count;
}

// The head is built with CONFIG_IPV6 (BDK lwIP): the station netif gets its link-local address by itself on every
// connect (checked 2026-11-04: ping6 fe80::… of the board answered; 2026-11-09: the Matter module showed IPv4 and two
// IPv6 addresses in NetworkInterfaces), nothing to switch on
void ipv6(bool) {
}

} // namespace chip
} // namespace driver
} // namespace wifi
} // namespace espurna

