/*

WIFI MODULE, BEKEN (LibreTiny) part: what only the BDK knows (ref. wifi_driver_rtos.h). The driver itself (events,
link state, connection, scan, AP settings) is the shared one, wifi_driver_rtos.cpp

BDK specifics: no 802.11 reason of the AP (synthetic reasons), a supplicant that associates again by itself unless it
is stopped, the 'scan done' event as the end of a scan, one radio for the AP and the station (the AP follows the
station channel), AP stations only from the supplicant, and a watch on the IP stack (netwatch).

*/

#include "../../espurna.h"

#if defined(LIBRETINY)

#include "../../wifi_driver_rtos.h"

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

bool expired(uint32_t since, uint32_t timeout) {
    return static_cast<int32_t>(millis() - since) >= static_cast<int32_t>(timeout);
}

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

// The Wi-Fi link can stay associated while the IP stack receives nothing at all: 10.0.5.116 2026-11-07, first boot after
// the Matter code was written by an install, DHCP done, then no ARP / ping answers for 28 min. The loop kept running and
// feeding the watchdog, so nothing reset it; a deauth from the AP did. Every Interval, while connected with an address,
// whatever arrived (ARP + IPv4 + IPv6 packets) is compared with the last check. Nothing new => an ARP request goes to
// the gateway (answers within ms, so a healthy stack always sees traffic by the next check); Misses checks in a row
// without a single received packet => restart
namespace netwatch {

constexpr auto Interval = espurna::duration::Seconds(20);
constexpr int Misses = 6;

uint32_t last_rx { 0 };
int misses { 0 };
timer::SystemTimer timer;

uint32_t received() {
    uint32_t out { 0 };
#if ETHARP_STATS
    out += lwip_stats.etharp.recv;
#endif
#if IP_STATS
    out += lwip_stats.ip.recv;
#endif
#if IP6_STATS
    out += lwip_stats.ip6.recv;
#endif
    return out;
}

// tcpip thread
void probe(void*) {
    auto* netif = netif_default;
    if (netif && netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_gw(netif))) {
        etharp_request(netif, netif_ip4_gw(netif));
    }
}

bool addressed() {
    const auto* netif = netif_default;
    return rtos::state().linked
        && netif && netif_is_up(netif)
        && !ip4_addr_isany_val(*netif_ip4_addr(netif));
}

void check() {
    const auto now = received();
    if (!addressed() || (now != last_rx)) {
        last_rx = now;
        misses = 0;
        return;
    }

    ++misses;
    if (misses >= Misses) {
        DEBUG_MSG_P(PSTR("[WIFI] Nothing received for %lu s while connected, restarting\n"),
            static_cast<unsigned long>(Interval.count() * Misses));
        timer.stop();
        prepareReset(CustomResetReason::Network);
        return;
    }

    DEBUG_MSG_P(PSTR("[WIFI] Nothing received for %lu s, asking the gateway\n"),
        static_cast<unsigned long>(Interval.count() * misses));
    // a full tcpip mailbox is the same story, the next check counts it
    tcpip_try_callback(probe, nullptr);
}

void setup() {
    last_rx = received();
    timer.repeat(Interval, check);
}

} // namespace netwatch

} // namespace

void init() {
    netwatch::setup();
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

// Beken SDK does not pass the reason of the AP (SM_DISCONNECT_IND reason_code is only printed by
// mhdr_disconnect_ind(), the event has none). Signal loss stays BEACON_TIMEOUT
bool synthetic_reasons() {
    return true;
}

bool ap_changed_recently() {
    return internal::ap_changed && !expired(internal::ap_changed_at, build::ApChangeQuiet);
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

bool scan_done_event() {
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
void ap_start(const char* ssid, const char* passphrase, uint8_t channel) {
    // A connected station keeps the radio on its channel: the AP goes there, not to the configured one. WIFI.AP while
    // connected to tiger (ch 1) started the AP on ch 13, the station dropped and every later attempt failed until a
    // reset (10.0.5.139, 2026-11-09)
    if (rtos::state().linked && WiFi.channel()) {
        channel = static_cast<uint8_t>(WiFi.channel());
    }

    bk_wlan_ap_set_default_channel(channel);

    internal::ap_changed = true;
    internal::ap_changed_at = millis();

    const IPAddress ip(192, 168, 4, 1);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(ssid, passphrase, 0)) {
        DEBUG_MSG_P(PSTR("[WIFI] SoftAP start failed\n"));
    }

    // AP start takes a few seconds, quiet time counts from the end of it
    internal::ap_changed_at = millis();
    internal::ap_started = true;
    apply_sleep();
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

#endif
