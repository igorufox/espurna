/*

WIFI MODULE, the shared driver of ESP32 and Beken (ref. wifi_driver_rtos.h, wifi_driver.h)

*/

#include "../../espurna.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(LIBRETINY)

#include "wifi_driver_rtos.h"

#include <WiFi.h>
#include <lwip/dns.h>

#include "../../espurna_tasks.h"

namespace espurna {
namespace wifi {
namespace driver {
namespace {

static_assert(OpmodeNull == WIFI_MODE_NULL, "");
static_assert(OpmodeSta == WIFI_MODE_STA, "");
static_assert(OpmodeAp == WIFI_MODE_AP, "");
static_assert(OpmodeApSta == WIFI_MODE_APSTA, "");

namespace build {

// Arduino fixes the minimal active dwell time at 100ms, this is the maximum. Default
// is 300ms, and every channel spent away from the AP on an already weak link risks
// missed beacons (and a disconnect). Driver returns to the home channel in between
// channels while connected.
static constexpr uint32_t ScanDwellMax { 120 };

// Disconnects that happen too soon after begin() are from the disconnect of the previous attempt
static constexpr uint32_t StaleDisconnect { 500 };

// a connection attempt makes a few, a scan one; loop() takes them every few ms
static constexpr size_t EventsMax { 16 };

} // namespace build

// What the SDK reports from its event task, whole and in order: loop() takes them one by one (ref. deliver_events()).
// An old disconnect never mixes with the fields of a newer one, and a disconnect of the previous link is never
// handled after the address of the new one
struct Event {
    enum class Type : uint8_t {
        Associated,
        GotIp,
        Disconnected,
        ScanDone,
    };

    Type type;
    uint8_t bssid[6];
    int reason;
    uint32_t at;
};

namespace internal {

rtos::State state;

espurna::system::EventQueue<Event, build::EventsMax> events;

// Station associated during the current connection attempt
bool associated { false };

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
rtos::Scan scan;

} // namespace internal

using rtos::expired;

Mac to_mac(const uint8_t* ptr) {
    Mac out{};
    if (ptr) {
        std::copy(ptr, ptr + out.size(), out.begin());
    }

    return out;
}

// millis() based, safe across the wrap
bool before(uint32_t at, uint32_t since) {
    return static_cast<int32_t>(at - since) < 0;
}

// SDK event task: the event goes to loop() as it is
void event(arduino_event_id_t id, arduino_event_info_t info) {
    Event out{};
    out.at = millis();

    switch (id) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        out.type = Event::Type::GotIp;
        break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        out.type = Event::Type::Associated;
        break;

    case ARDUINO_EVENT_WIFI_SCAN_DONE:
        out.type = Event::Type::ScanDone;
        break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        out.type = Event::Type::Disconnected;
        std::copy(info.wifi_sta_disconnected.bssid,
            info.wifi_sta_disconnected.bssid + sizeof(out.bssid), out.bssid);
        out.reason = static_cast<int>(info.wifi_sta_disconnected.reason);
        break;

    default:
        return;
    }

    internal::events.push(out);
}

void link_lost(const Event& event, const chip::LinkLost& lost) {
    auto& state = internal::state;
    const int reason = event.reason;

    // Common code is the only one deciding when and where to connect
    chip::dropped();

    DEBUG_MSG_P(PSTR("[WIFI] Connection to %s lost, reason %d (%s)%s\n"),
        espurna::settings::internal::serialize(lost.bssid).c_str(),
        reason, chip::reason_name(reason), lost.ours ? PSTR(" after our AP change") : PSTR(""));

    state.linked = false;
    internal::connecting = false;
    on_station_disconnected(lost.bssid, lost.reason);
}

void begin() {
    const char* passphrase = internal::passphrase.length() ? internal::passphrase.c_str() : nullptr;
    if (internal::target_channel) {
        WiFi.begin(internal::ssid.c_str(), passphrase, internal::target_channel, internal::target_bssid.data());
    } else {
        WiFi.begin(internal::ssid.c_str(), passphrase);
    }
}

// The disconnect right before begin() reports itself later (within the stale window). What else is about the previous
// attempt and what the AP of a failed one is, the chip says (ref. chip::attempt_event()). Anything else (no AP found,
// wrong password, etc.) is about the current attempt. An AP rejecting the association ends up as the attempt timeout
void attempt_failed(const Event& event) {
    const int reason = event.reason;

    const chip::AttemptDisconnect disconnect{
        .reason = reason,
        .bssid = to_mac(event.bssid),
        .target = internal::target_bssid,
        .stale = before(event.at, internal::connect_at + build::StaleDisconnect),
        .associated = internal::associated,
        .begun_again = internal::begun_again,
    };

    Mac bssid{};
    switch (chip::attempt_event(disconnect, bssid)) {
    case chip::Attempt::BeginAgain:
        internal::begun_again = true;
        DEBUG_MSG_P(PSTR("[WIFI] Disconnect of the previous connection during the attempt, reason %d (%s), "
            "starting it again\n"), reason, chip::reason_name(reason));
        internal::associated = false;
        internal::connect_at = millis();
        begin();
        return;

    case chip::Attempt::Ignored:
        DEBUG_MSG_P(PSTR("[WIFI] Ignoring a disconnect while connecting, reason %d (%s)\n"),
            reason, chip::reason_name(reason));
        return;

    case chip::Attempt::Failed:
        break;
    }

    chip::dropped();

    DEBUG_MSG_P(PSTR("[WIFI] Connection attempt failed, reason %d (%s)\n"),
        reason, chip::reason_name(reason));

    internal::connecting = false;
    on_station_disconnected(bssid, reason);
}

void disconnected(const Event& event) {
    auto& state = internal::state;

    if (state.linked) {
        chip::LinkLost lost{};
        if (chip::map_reason(event.reason, to_mac(event.bssid), state.linked_bssid, lost)) {
            link_lost(event, lost);
        }
    } else if (internal::connecting) {
        attempt_failed(event);
    }
}

void got_ip() {
    auto& state = internal::state;

    // the station is gone again by now: its disconnect is the next event in the queue
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

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

// Every event since the last loop(), in the order the SDK sent them. Without `station`, the station events are only
// taken out of the queue (ref. sta_connect())
void deliver_events(bool station = true) {
    const auto lost = internal::events.dropped();
    if (lost) {
        DEBUG_MSG_P(PSTR("[WIFI] %u events lost, the event queue was full\n"), lost);
    }

    Event event;
    while (internal::events.pop(event)) {
        // a scan only takes the 'done' events from after its start (ref. scan_start())
        if (event.type == Event::Type::ScanDone) {
            if (internal::scanning && !before(event.at, internal::scan.at)) {
                internal::scan.done = true;
                internal::scan.done_at = event.at;
            }
            continue;
        }

        if (!station) {
            continue;
        }

        switch (event.type) {
        case Event::Type::Associated:
            internal::associated = true;
            break;

        case Event::Type::Disconnected:
            disconnected(event);
            break;

        case Event::Type::GotIp:
            got_ip();
            break;

        case Event::Type::ScanDone:
            break;
        }
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

// The chip says when the scan is over (ref. chip::scan_finished())
void scan_poll() {
    if (!internal::scanning) {
        return;
    }

    int16_t count { -1 };
    if (!chip::scan_finished(internal::scan, count)) {
        return;
    }

    internal::scanning = false;
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
    const bool connecting = internal::connecting;

    state.linked = false;
    internal::connecting = false;
    chip::stop_station();

    // A cancelled attempt is an event as well, same as the esp8266 SDK reports it: the common code leaves
    // WaitConnected right away instead of waiting for its attempt timeout
    if (linked) {
        on_station_disconnected(state.linked_bssid, rtos::ReasonAssocLeave);
    } else if (connecting) {
        on_station_disconnected(internal::target_bssid, rtos::ReasonAssocLeave);
    }
}

// After the STA is enabled, nothing is supposed to connect besides us (and a scan does not work while the SDK tries)
void sta_prepare() {
    sta_disconnect();
}

// Lengths are checked by the common code before this call (ref. StaConnect)
bool sta_connect(const StaConnect& connect) {
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

    internal::ssid = connect.ssid;
    internal::passphrase = connect.passphrase;
    internal::target_bssid = connect.channel
        ? connect.bssid
        : Mac{};
    internal::target_channel = connect.channel;

    // what the SDK reported until now is about the attempt or the link before this one
    deliver_events(false);

    internal::associated = false;
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

    internal::scan = rtos::Scan{};
    internal::scan.linked = linked;
    internal::scan.at = millis();
    internal::scanning = true;

    return true;
}

void ap_start(const char* ssid, const char* passphrase, uint8_t channel) {
    // copies first, the arguments may be these very strings (ap restart of the chip)
    const String ssid_copy(ssid);
    const String passphrase_copy(passphrase ? passphrase : "");

    const auto sdk_channel = chip::ap_prepare(channel);

    const IPAddress ip(192, 168, 4, 1);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(ssid, passphrase, sdk_channel)) {
        DEBUG_MSG_P(PSTR("[WIFI] SoftAP start failed\n"));
    }

    chip::apply_sleep();
    chip::ap_started();

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
