/*

WIFI MODULE, the shared driver of ESP32 and Beken (ref. wifi_driver_rtos.h, wifi_driver.h)

*/

#include "espurna.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(LIBRETINY)

#include "wifi_driver_rtos.h"

#include <WiFi.h>
#include <lwip/dns.h>

#include <atomic>

namespace espurna {
namespace wifi {
namespace driver {
namespace {

static_assert(OpmodeNull == WIFI_MODE_NULL, "");
static_assert(OpmodeSta == WIFI_MODE_STA, "");
static_assert(OpmodeAp == WIFI_MODE_AP, "");
static_assert(OpmodeApSta == WIFI_MODE_APSTA, "");

namespace build {

static constexpr size_t PassphraseMin { 8ul };

// WiFi.scanComplete() has its own timeout of max_ms_per_chan * 20 (6s by default) and reports
// WIFI_SCAN_FAILED after it, even though the scan is still going and results arrive later.
// Scanning 13 channels while STA is connected (driver hops back to the home channel) easily
// takes longer than that. Keep waiting until our own timeout instead.
static constexpr uint32_t ScanTimeout { 15000 };

// Scan while associated with the 'scan done' event: results only come with the event of the scan itself. Without it
// there are no results (Beken: the supplicant gets none, reports 'no AP found' on its own and tries again)
static constexpr uint32_t ScanTimeoutLinked { 5000 };

// LibreTiny reports 'scan done' for every scan results event of the supplicant, including the
// ones of its own scans and of the scan that was still running when ours was requested (2 APs
// 108 ms after the start, while a real scan of every channel takes more than a second)
static constexpr uint32_t ScanTimeMin { 500 };

// Arduino fixes the minimal active dwell time at 100ms, this is the maximum. Default
// is 300ms, and every channel spent away from the AP on an already weak link risks
// missed beacons (and a disconnect). Driver returns to the home channel in between
// channels while connected.
static constexpr uint32_t ScanDwellMax { 120 };

// Safety net, the SDK does not always report the end of a connection attempt
static constexpr uint32_t ConnectTimeout { 15000 };

// Disconnects that happen too soon after begin() are from the disconnect of the previous attempt
static constexpr uint32_t StaleDisconnect { 500 };

// 802.11 reasons: 'unspecified' (the AP dropped us), 'disassociated because sending STA is leaving' (our own)
static constexpr int ReasonUnspecified { 1 };
static constexpr int ReasonAssocLeave { 8 };

} // namespace build

namespace internal {

rtos::State state;

// Latched by the event task, delivered by loop()
std::atomic<bool> got_ip { false };
std::atomic<bool> disconnected { false };
std::atomic<int> reason { 0 };
std::atomic<uint32_t> disconnected_at { 0 };
std::atomic<uint32_t> bssid_head { 0 };
std::atomic<uint32_t> bssid_tail { 0 };

// Station associated during the current connection attempt
std::atomic<bool> associated { false };

std::atomic<bool> scan_done { false };
std::atomic<uint32_t> scan_done_at { 0 };

// Connection attempt in progress (sta_connect() called, no address and no failure yet)
bool connecting { false };
uint32_t connect_at { 0 };

// Last connection request, the SDK forgets it once disconnected
String ssid;
String passphrase;
Mac target_bssid{};
uint8_t target_channel { 0 };

// begin() was issued again in this attempt (ref. attempt_failed())
bool begun_again { false };

bool scanning { false };
uint32_t scan_at { 0 };

// scan started while associated
bool scan_linked { false };

// our scan got its 'done' event (ref. scan_poll())
bool scan_done_ok { false };

} // namespace internal

bool expired(uint32_t since, uint32_t timeout) {
    return static_cast<int32_t>(millis() - since) >= static_cast<int32_t>(timeout);
}

Mac to_mac(const uint8_t* ptr) {
    Mac out{};
    if (ptr) {
        std::copy(ptr, ptr + out.size(), out.begin());
    }

    return out;
}

void event(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        internal::got_ip.store(true);
        break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        internal::associated.store(true);
        break;

    case ARDUINO_EVENT_WIFI_SCAN_DONE:
        internal::scan_done_at.store(millis());
        internal::scan_done.store(true);
        break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        const auto* bssid = info.wifi_sta_disconnected.bssid;
        internal::bssid_head.store(
            (uint32_t(bssid[0]) << 24) | (uint32_t(bssid[1]) << 16) | (uint32_t(bssid[2]) << 8) | uint32_t(bssid[3]));
        internal::bssid_tail.store(
            (uint32_t(bssid[4]) << 8) | uint32_t(bssid[5]));
        internal::reason.store(static_cast<int>(info.wifi_sta_disconnected.reason));
        internal::disconnected_at.store(millis());
        internal::disconnected.store(true);
        break;
    }

    default:
        break;
    }
}

Mac disconnected_bssid() {
    const auto head = internal::bssid_head.load();
    const auto tail = internal::bssid_tail.load();

    return Mac{{
        static_cast<uint8_t>(head >> 24),
        static_cast<uint8_t>(head >> 16),
        static_cast<uint8_t>(head >> 8),
        static_cast<uint8_t>(head),
        static_cast<uint8_t>(tail >> 8),
        static_cast<uint8_t>(tail)}};
}

void link_lost(int reason) {
    auto& state = internal::state;
    const bool synthetic = chip::synthetic_reasons();

    // the SDK gives no reason: AP dropped us, unless it happened right after our own AP start / stop
    const bool ours = synthetic && chip::ap_changed_recently();
    const int ieee = !synthetic
        ? reason
        : ours
            ? build::ReasonAssocLeave
            : ((reason == WIFI_REASON_CONNECTION_FAIL) || (reason == WIFI_REASON_ASSOC_LEAVE))
                ? build::ReasonUnspecified
                : reason;

    const auto bssid = synthetic ? state.linked_bssid : disconnected_bssid();

    // Common code is the only one deciding when and where to connect
    chip::dropped();

    DEBUG_MSG_P(PSTR("[WIFI] Connection to %s lost, reason %d (%s)%s\n"),
        espurna::settings::internal::serialize(bssid).c_str(),
        reason, chip::reason_name(reason), ours ? PSTR(" after our AP change") : PSTR(""));

    state.linked = false;
    internal::connecting = false;
    on_station_disconnected(bssid, ieee);
}

void begin() {
    const char* passphrase = internal::passphrase.length() ? internal::passphrase.c_str() : nullptr;
    if (internal::target_channel) {
        WiFi.begin(internal::ssid.c_str(), passphrase, internal::target_channel, internal::target_bssid.data());
    } else {
        WiFi.begin(internal::ssid.c_str(), passphrase);
    }
}

void attempt_failed(uint32_t at, int reason) {
    const bool synthetic = chip::synthetic_reasons();

    // The disconnect right before begin() reports itself later. Without real reasons it is the same 'connection failed'
    // / 'leaving' event the SDK sends for an association that is lost, which can only happen after the station
    // associated, and not necessarily within the stale window. Anything else (no AP found, wrong password, etc.) is
    // about the current attempt. An AP rejecting the association ends up as the attempt timeout
    const bool stale = static_cast<int32_t>(at - (internal::connect_at + build::StaleDisconnect)) < 0;
    const bool leaving = synthetic
        && ((reason == WIFI_REASON_CONNECTION_FAIL) || (reason == WIFI_REASON_ASSOC_LEAVE))
        && !internal::associated.load();
    if (stale || leaving) {
        // Beken: the SDK handles this late 'leaving' of the previous association after our begin() and drops the new
        // attempt with it, nothing else comes until the attempt timeout (15 s, twice in a row on 10.0.5.139 after
        // WIFI.RESET, 2026-11-09). Start the same attempt again right away, once
        if (synthetic && !internal::begun_again) {
            internal::begun_again = true;
            DEBUG_MSG_P(PSTR("[WIFI] Disconnect of the previous connection during the attempt, reason %d (%s), "
                "starting it again\n"), reason, chip::reason_name(reason));
            internal::associated.store(false);
            internal::connect_at = millis();
            begin();
            return;
        }

        DEBUG_MSG_P(PSTR("[WIFI] Ignoring a disconnect while connecting, reason %d (%s)\n"),
            reason, chip::reason_name(reason));
        return;
    }

    chip::dropped();

    DEBUG_MSG_P(PSTR("[WIFI] Connection attempt failed, reason %d (%s)\n"),
        reason, chip::reason_name(reason));

    internal::connecting = false;
    on_station_disconnected(synthetic ? internal::target_bssid : disconnected_bssid(), reason);
}

void deliver_events() {
    auto& state = internal::state;

    // Disconnect first, then the address. Both may have been latched since the last loop()
    if (internal::disconnected.exchange(false)) {
        const auto at = internal::disconnected_at.load();
        const auto reason = internal::reason.load();

        if (state.linked && chip::synthetic_reasons() && (reason == WIFI_REASON_NO_AP_FOUND)) {
            // Not about the link. Supplicant scan without results (a scan while associated) sets the station
            // status to 'no AP found' (Beken hostapd_intf.c wpa_get_scan_rst), the association stays
            DEBUG_MSG_P(PSTR("[WIFI] Ignoring 'no AP found' while connected\n"));
        } else if (state.linked) {
            link_lost(reason);
        } else if (internal::connecting) {
            attempt_failed(at, reason);
        }
    }

    if (internal::got_ip.exchange(false) && (WiFi.status() == WL_CONNECTED)) {
        if (internal::connecting) {
            internal::connecting = false;
            state.linked = true;
            state.linked_bssid = to_mac(WiFi.BSSID());
            chip::apply_tx_power();
            on_station_got_ip();
        } else if (!state.linked) {
            // auto reconnect is off, an SDK that connected by itself is stopped
            DEBUG_MSG_P(PSTR("[WIFI] Connected by the SDK itself, dropping the connection\n"));
            chip::stop_station();
        }
        // otherwise, DHCP renewed the address of the current connection
    }

    if (internal::connecting && expired(internal::connect_at, build::ConnectTimeout)) {
        DEBUG_MSG_P(PSTR("[WIFI] Connection attempt timed out (status %d)\n"),
            static_cast<int>(WiFi.status()));
        chip::stop_station();
        internal::connecting = false;
        on_station_disconnected(internal::target_bssid, 0);
    }
}

void scan_report(int16_t count) {
    if (count < 0) {
        WiFi.scanDelete();
        on_scan_done(false, 0);
        return;
    }

    for (int16_t index = 0; index < count; ++index) {
        ScanResult result{};

        const auto* bssid = WiFi.BSSID(index);
        if (bssid) {
            std::copy(bssid, bssid + sizeof(result.bssid), result.bssid);
        }

        const auto ssid = WiFi.SSID(index);
        result.ssid_len = std::min(ssid.length(), sizeof(result.ssid));
        std::copy(ssid.c_str(), ssid.c_str() + result.ssid_len, result.ssid);

        result.channel = WiFi.channel(index);
        result.rssi = WiFi.RSSI(index);
        result.authmode = WiFi.encryptionType(index);

        on_scan_result(result);
    }

    WiFi.scanDelete();
    on_scan_done(true, count);
}

// The end of the scan is WiFi.scanComplete() (ESP32), or our own 'scan done' event (Beken: LibreTiny keeps updating
// the results with every scan results event, an earlier one may come from a different scan, ref. ScanTimeMin)
void scan_poll() {
    if (!internal::scanning) {
        return;
    }

    if (!chip::scan_done_event()) {
        const auto count = WiFi.scanComplete();
        if ((count < 0) && !expired(internal::scan_at, build::ScanTimeout)) {
            return;
        }

        internal::scanning = false;
        scan_report(count);
        return;
    }

    const auto timeout = internal::scan_linked
        ? build::ScanTimeoutLinked
        : build::ScanTimeout;

    if (!expired(internal::scan_at, timeout)) {
        if (!internal::scan_done.exchange(false)) {
            return;
        }

        const auto after = internal::scan_done_at.load() - internal::scan_at;
        if (after < build::ScanTimeMin) {
            DEBUG_MSG_P(PSTR("[WIFI] Ignoring scan results reported %u ms after the start\n"),
                static_cast<unsigned>(after));
            return;
        }

        internal::scan_done_ok = true;
    }

    internal::scanning = false;

    // While associated, what the core has without the 'done' event is not from this scan
    const bool done = internal::scan_done_ok;
    internal::scan_done_ok = false;

    auto count = WiFi.scanComplete();
    if (internal::scan_linked && !done) {
        DEBUG_MSG_P(PSTR("[WIFI] Scan while connected got no results in %u ms\n"),
            static_cast<unsigned>(millis() - internal::scan_at));
        count = -1;
    } else {
        DEBUG_MSG_P(PSTR("[WIFI] Scan done in %u ms, %d network(s)\n"),
            static_cast<unsigned>(millis() - internal::scan_at), static_cast<int>(count));
    }

    scan_report(count);
}

} // namespace

namespace rtos {

State& state() {
    return internal::state;
}

} // namespace rtos

bool setup() {
    return true;
}

void init() {
    chip::init();

    WiFi.mode(WIFI_MODE_NULL);
    internal::state.mode = OpmodeNull;

    WiFi.onEvent(event);
}

void loop() {
    deliver_events();
    scan_poll();
}

// The requested mode. Beken reports the STA bit only while its interface exists and fakes the AP bit
uint8_t opmode() {
    return internal::state.mode;
}

void opmode(uint8_t mode) {
    chip::opmode(mode);
    internal::state.mode = mode;
}

void wakeup() {
}

void tx_power(float dbm) {
    internal::state.tx_power = dbm;
    chip::apply_tx_power();
}

void ipv6(bool enabled) {
    chip::ipv6(enabled);
}

sleep_type_t sleep_type() {
    return internal::state.sleep;
}

bool sleep_type(sleep_type_t type) {
    internal::state.sleep = type;
    chip::apply_sleep();
    return true;
}

String authmode(AUTH_MODE mode) {
    StringView out;

    switch (mode) {
    case WIFI_AUTH_OPEN:
        out = STRING_VIEW("OPEN");
        break;
    case WIFI_AUTH_WEP:
        out = STRING_VIEW("WEP");
        break;
    case WIFI_AUTH_WPA_PSK:
        out = STRING_VIEW("WPAPSK");
        break;
    case WIFI_AUTH_WPA2_PSK:
        out = STRING_VIEW("WPA2PSK");
        break;
    case WIFI_AUTH_WPA_WPA2_PSK:
        out = STRING_VIEW("WPAWPA2-PSK");
        break;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        out = STRING_VIEW("WPA2-EAP");
        break;
    case WIFI_AUTH_WPA3_PSK:
        out = STRING_VIEW("WPA3PSK");
        break;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        out = STRING_VIEW("WPA2WPA3-PSK");
        break;
    case WIFI_AUTH_WAPI_PSK:
        out = STRING_VIEW("WAPIPSK");
        break;
    default:
        out = STRING_VIEW("UNKNOWN");
        break;
    }

    return out.toString();
}

IPAddress sta_ip() {
    return WiFi.localIP();
}

uint8_t sta_channel() {
    return static_cast<uint8_t>(WiFi.channel());
}

int8_t sta_rssi() {
    return WiFi.RSSI();
}

// Same as the esp8266 station config, last requested network stays known after the disconnect
void sta_info(Mac& bssid, AUTH_MODE& authmode) {
    bssid = internal::state.linked
        ? internal::state.linked_bssid
        : internal::target_bssid;
    authmode = internal::passphrase.length()
        ? WIFI_AUTH_WPA_PSK
        : WIFI_AUTH_OPEN;
}

StaNetwork sta_current() {
    Mac bssid;
    AUTH_MODE authmode;
    sta_info(bssid, authmode);

    return {
        bssid,
        internal::ssid,
        internal::passphrase,
        sta_rssi(),
        sta_channel()};
}

// Same as esp8266, the event is reported right away and the common code reconnects when it needs to.
// Whatever the SDK reports afterwards is about the connection that is already gone
void sta_disconnect() {
    auto& state = internal::state;
    const bool linked = state.linked;

    state.linked = false;
    internal::connecting = false;
    chip::stop_station();

    if (linked) {
        on_station_disconnected(state.linked_bssid, build::ReasonAssocLeave);
    }
}

// After the STA is enabled, nothing is supposed to connect besides us (and a scan does not work while the SDK tries)
void sta_prepare() {
    sta_disconnect();
}

bool sta_connect(const StaConnect& connect) {
    auto& ssid = connect.ssid;
    if (!ssid.length() || (ssid.length() > SsidMax)) {
        return false;
    }

    auto& pass = connect.passphrase;
    if (pass.length() && ((pass.length() < build::PassphraseMin) || (pass.length() > PassphraseMax))) {
        return false;
    }

    // DHCP stays disabled after WiFi.config() with a static IP, unless explicitly reset.
    // Otherwise one network with a static IP makes every other one use it as well.
    // (zero IP restarts the DHCP client, not INADDR_NONE, lwip defines it as 255.255.255.255)
    if (!connect.dhcp) {
        WiFi.config(connect.ip, connect.gateway, connect.netmask, connect.dns);
    } else {
        const IPAddress none(uint32_t{0});
        WiFi.config(none, none, none);
    }

    WiFi.setHostname(connect.hostname.c_str());

    chip::before_connect(connect);

    internal::ssid = ssid;
    internal::passphrase = pass;
    internal::target_bssid = connect.channel
        ? connect.bssid
        : Mac{};
    internal::target_channel = connect.channel;

    internal::disconnected.store(false);
    internal::got_ip.store(false);
    internal::associated.store(false);
    internal::connecting = true;
    internal::begun_again = false;
    internal::connect_at = millis();

    begin();
    return true;
}

bool scan_start() {
    if (internal::scanning || (WiFi.scanComplete() == WIFI_SCAN_RUNNING)) {
        return false;
    }

    const bool linked = internal::state.linked;
    const auto result = linked
        ? WiFi.scanNetworks(true, false, false, build::ScanDwellMax, 0)
        : WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_FAILED) {
        return false;
    }

    internal::scan_linked = linked;
    internal::scan_done_ok = false;
    internal::scan_done.store(false);
    internal::scanning = true;
    internal::scan_at = millis();

    return true;
}

void ap_start(const char* ssid, const char* passphrase, uint8_t channel) {
    // copies first, the arguments may be these very strings (ap restart of the chip)
    const String ssid_copy(ssid);
    const String passphrase_copy(passphrase ? passphrase : "");

    chip::ap_start(ssid, passphrase, channel);

    internal::state.ap_ssid = ssid_copy;
    internal::state.ap_passphrase = passphrase_copy;
}

// Not softAPdisconnect(true): it first pushes an empty AP config (channel 6)
// through esp_wifi_set_config() and skips enableAP(false) when that fails,
// e.g. while STA is associated on another channel. Dropping the AP bit
// from the opmode is what actually stops the beacons, ref. opmode()
void ap_stop() {
    chip::ap_stop();
    internal::state.ap_ssid = String();
    internal::state.ap_passphrase = String();
}

IPAddress ap_ip() {
    return WiFi.softAPIP();
}

SoftApNetwork ap_current() {
    Mac mac{};
    WiFi.softAPmacAddress(mac.data());

    const auto& passphrase = internal::state.ap_passphrase;
    return {
        mac,
        WiFi.softAPSSID(),
        passphrase,
        static_cast<uint8_t>(WiFi.channel()),
        passphrase.length()
            ? WIFI_AUTH_WPA2_PSK
            : WIFI_AUTH_OPEN};
}

size_t ap_stations() {
    return chip::ap_stations();
}

// Not sent, lwip calls from loop() would race with the tcpip task
bool garp_send() {
    return false;
}

size_t print_stations(Print& out) {
    return chip::print_stations(out);
}

void print_network(Print& out) {
    const auto mode = internal::state.mode;
    if (mode & OpmodeSta) {
        out.printf_P(PSTR("sta %4s ip %s gateway %s mask %s\n"),
            internal::state.linked ? "up" : "down",
            ipToString(WiFi.localIP()).c_str(),
            ipToString(WiFi.gatewayIP()).c_str(),
            ipToString(WiFi.subnetMask()).c_str());
    }

    if (mode & OpmodeAp) {
        out.printf_P(PSTR("ap     up ip %s\n"),
            ipToString(WiFi.softAPIP()).c_str());
    }

    for (int n = 0; n < DNS_MAX_SERVERS; ++n) {
        const auto* addr = dns_getserver(n);
        if (!addr || ip_addr_isany(addr)) {
            break;
        }

        out.printf_P(PSTR("dns %s\n"),
            ipToString(IPAddress(ip_2_ip4(addr)->addr)).c_str());
    }
}

} // namespace driver
} // namespace wifi
} // namespace espurna

#endif
