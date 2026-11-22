/*

WIFI MODULE, the one driver of every chip (ESP8266, ESP32, Beken) (ref. wifi_driver_rtos.h, wifi_driver.h)

*/

#include "../../espurna.h"

#include "wifi_driver_rtos.h"

#include "wifi_sdk.h"

#include <lwip/dns.h>

#include "../../espurna_tasks.h"

namespace espurna {
namespace wifi {
namespace driver {
namespace {

// the SDK calls of the chip (wifi_sdk.h); `chip` here is the driver's own part of the chip (wifi_driver_rtos.h)
namespace sdk = ::espurna::core::chip;

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
espurna::string ssid;
espurna::string passphrase;
Mac target_bssid{};
uint8_t target_channel { 0 };

// begin() was issued again in this attempt (ref. attempt_failed())
bool begun_again { false };

bool scanning { false };
rtos::Scan scan;

// WIFI.DEBUG: the last disconnects the SDK reported, what the driver was doing then
struct Disconnect {
    uint32_t at;
    int reason;
    uint8_t bssid[6];
    bool linked;
    bool connecting;
    bool associated;
};
Disconnect disconnects[8]{};
size_t disconnects_next { 0 };

} // namespace internal

using rtos::expired;

Mac to_mac(const uint8_t* ptr) {
    Mac out{};
    if (ptr) {
        std::copy(ptr, ptr + out.size(), out.begin());
    }

    return out;
}

// espurna::time::ms() based, safe across the wrap
bool before(uint32_t at, uint32_t since) {
    return static_cast<int32_t>(at - since) < 0;
}

// SDK event task: the event goes to loop() as it is
void event(sdk::WifiEvent id, const sdk::WifiEventInfo& info) {
    Event out{};
    out.at = espurna::time::ms();

    switch (id) {
    case sdk::WifiEvent::StaGotIp:
        out.type = Event::Type::GotIp;
        break;

    case sdk::WifiEvent::StaConnected:
        out.type = Event::Type::Associated;
        break;

    case sdk::WifiEvent::ScanDone:
        out.type = Event::Type::ScanDone;
        break;

    case sdk::WifiEvent::StaDisconnected:
        out.type = Event::Type::Disconnected;
        std::copy(info.bssid, info.bssid + sizeof(out.bssid), out.bssid);
        out.reason = static_cast<int>(info.reason);
        break;
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
        sdk::wifi_sta_begin(internal::ssid.c_str(), passphrase, internal::target_channel, internal::target_bssid.data());
    } else {
        sdk::wifi_sta_begin(internal::ssid.c_str(), passphrase, 0, nullptr);
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
        internal::connect_at = espurna::time::ms();
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

    {
        auto& entry = internal::disconnects[internal::disconnects_next++ % 8];
        entry.at = event.at;
        entry.reason = event.reason;
        std::copy(std::begin(event.bssid), std::end(event.bssid), std::begin(entry.bssid));
        entry.linked = state.linked;
        entry.connecting = internal::connecting;
        entry.associated = internal::associated;
    }

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
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    if (!sdk::wifi_sta_address(ip, netmask, gateway) || !ip) {
        return;
    }

    if (internal::connecting) {
        internal::connecting = false;
        state.linked = true;
        sdk::WifiInfo info{};
        state.linked_bssid = sdk::wifi_sta_info(info) ? to_mac(info.bssid) : Mac{};
        // the sleep too: the setting went in with the opmode, before the association (20.11 COM3: pings 3..295 ms, a
        // 50 KB WebUI page in 8..18 s - the modem sleep between the beacons)
        chip::apply_sleep();
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
        sdk::wifi_scan_delete();
        on_scan_done(false, 0);
        return;
    }

    for (int16_t index = 0; index < count; ++index) {
        sdk::WifiInfo info{};
        if (!sdk::wifi_scan_record(static_cast<uint16_t>(index), info)) {
            continue;
        }

        ScanResult result{};
        std::copy(info.bssid, info.bssid + sizeof(result.bssid), result.bssid);
        result.ssid_len = static_cast<uint8_t>(strnlen(reinterpret_cast<const char*>(info.ssid), sizeof(result.ssid)));
        std::copy(info.ssid, info.ssid + result.ssid_len, result.ssid);
        result.channel = info.channel;
        result.rssi = info.rssi;
        result.authmode = info.authmode;

        on_scan_result(result);
    }

    sdk::wifi_scan_delete();
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

    sdk::wifi_on_event(event);

    sdk::wifi_mode(WIFI_MODE_NULL);
    internal::state.mode = OpmodeNull;
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

espurna::string authmode(AUTH_MODE mode) {
    StringView out;

    switch (mode) {
    case WIFI_AUTH_OPEN:
        out = STRING_VIEW("OPEN");
        break;
    case WIFI_AUTH_WEP:
        out = STRING_VIEW("WEP");
        break;
    case WIFI_AUTH_WPA_PSK:
        out = STRING_VIEW("WPA-PSK");
        break;
    case WIFI_AUTH_WPA2_PSK:
        out = STRING_VIEW("WPA2-PSK");
        break;
    case WIFI_AUTH_WPA_WPA2_PSK:
        out = STRING_VIEW("WPA/WPA2-PSK");
        break;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        out = STRING_VIEW("WPA2-EAP");
        break;
    case WIFI_AUTH_WPA3_PSK:
        out = STRING_VIEW("WPA3-PSK");
        break;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        out = STRING_VIEW("WPA2/WPA3-PSK");
        break;
    default: {
        const auto* name = chip::auth_name(static_cast<int>(mode));
        return espurna::string(name ? name : "UNKNOWN");
    }
    }

    return out.toString();
}

espurna::Ip4 sta_ip() {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    sdk::wifi_sta_address(ip, netmask, gateway);
    return espurna::Ip4(ip);
}

uint8_t sta_channel() {
    return sdk::wifi_channel();
}

const char* sta_security() {
    return internal::state.linked ? chip::sta_security() : "";
}

int8_t sta_rssi() {
    sdk::WifiInfo info{};
    return sdk::wifi_sta_info(info) ? info.rssi : 0;
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
    // DHCP stays disabled after a static IP, unless explicitly reset: otherwise one network with a static IP makes
    // every other one use it as well (zero IP restarts the DHCP client)
    if (!connect.dhcp) {
        sdk::wifi_sta_config(static_cast<uint32_t>(connect.ip), static_cast<uint32_t>(connect.gateway),
            static_cast<uint32_t>(connect.netmask), static_cast<uint32_t>(connect.dns), 0);
    } else {
        sdk::wifi_sta_config(0, 0, 0, 0, 0);
    }

    sdk::wifi_hostname(connect.hostname.c_str());

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
    internal::connect_at = espurna::time::ms();

    begin();
    return true;
}

bool scan_start() {
    if (internal::scanning || (sdk::wifi_scan_count() == WIFI_SCAN_RUNNING)) {
        return false;
    }

    // connected: active, the shortest dwell; else every network, hidden ones too, the SDK's default dwell (300 ms)
    const bool linked = internal::state.linked;
    sdk::wifi_scan_delete();
    const bool started = linked
        ? sdk::wifi_scan_start(false, false, build::ScanDwellMax, 0)
        : sdk::wifi_scan_start(true, false, 300, 0);
    if (!started) {
        return false;
    }

    internal::scan = rtos::Scan{};
    internal::scan.linked = linked;
    internal::scan.at = espurna::time::ms();
    internal::scanning = true;

    return true;
}

void ap_start(const char* ssid, const char* passphrase, uint8_t channel) {
    // copies first, the arguments may be these very strings (ap restart of the chip)
    const espurna::string ssid_copy(ssid);
    const espurna::string passphrase_copy(passphrase ? passphrase : "");

    const auto sdk_channel = chip::ap_prepare(channel);

    // 192.168.4.1/24, the AP itself the gateway and the DNS of its clients (the captive portal)
    const uint32_t ip = static_cast<uint32_t>(espurna::Ip4(192, 168, 4, 1));
    const uint32_t netmask = static_cast<uint32_t>(espurna::Ip4(255, 255, 255, 0));
    sdk::wifi_ap_config(ip, ip, netmask);
    const bool with_passphrase = passphrase && *passphrase;
    if (!sdk::wifi_ap_start(ssid, with_passphrase ? passphrase : nullptr, sdk_channel, false, 4)) {
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
    internal::state.ap_ssid = espurna::string();
    internal::state.ap_passphrase = espurna::string();
}

espurna::Ip4 ap_ip() {
    return espurna::Ip4(sdk::wifi_ap_ip());
}

SoftApNetwork ap_current() {
    Mac mac{};
    sdk::wifi_mac(true, mac.data());

    uint8_t ssid[33] {};
    sdk::wifi_ap_ssid(ssid);

    const auto& passphrase = internal::state.ap_passphrase;
    return {
        mac,
        espurna::string(reinterpret_cast<const char*>(ssid)),
        passphrase,
        sdk::wifi_channel(),
        passphrase.length()
            ? WIFI_AUTH_WPA2_PSK
            : WIFI_AUTH_OPEN};
}

size_t ap_stations() {
    return chip::ap_stations();
}

size_t print_stations(::espurna::Output& out) {
    return chip::print_stations(out);
}

// WIFI.DEBUG: the driver's state and the last disconnects, oldest first
void print_debug(::espurna::Output& out) {
    const auto now = espurna::time::ms();
    out.printf_P(PSTR("driver: mode %u linked %s connecting %s (%u ms) associated %s begun again %s scanning %s\n"),
        static_cast<unsigned>(internal::state.mode), internal::state.linked ? "yes" : "no",
        internal::connecting ? "yes" : "no",
        internal::connecting ? static_cast<unsigned>(now - internal::connect_at) : 0u,
        internal::associated ? "yes" : "no", internal::begun_again ? "yes" : "no",
        internal::scanning ? "yes" : "no");
    out.printf_P(PSTR("target: ssid \"%s\" bssid %s channel %u rssi %d\n"), internal::ssid.c_str(),
        espurna::settings::internal::serialize(internal::target_bssid).c_str(),
        static_cast<unsigned>(internal::target_channel), static_cast<int>(sta_rssi()));
    out.printf_P(PSTR("sleep: setting %u, driver %s\n"), static_cast<unsigned>(internal::state.sleep), chip::sleep_name());
    for (size_t index = 0; index < 8; ++index) {
        const auto& entry = internal::disconnects[(internal::disconnects_next + index) % 8];
        if (!entry.at) {
            continue;
        }
        Mac bssid{};
        std::copy(std::begin(entry.bssid), std::end(entry.bssid), bssid.begin());
        out.printf_P(PSTR("disconnect %u s ago: reason %d (%s) bssid %s, was %s%s%s\n"),
            static_cast<unsigned>((now - entry.at) / 1000), entry.reason, chip::reason_name(entry.reason),
            espurna::settings::internal::serialize(bssid).c_str(), entry.linked ? "linked " : "",
            entry.connecting ? "connecting " : "", entry.associated ? "associated" : "");
    }
}

void print_network(::espurna::Output& out) {
    const auto mode = internal::state.mode;
    if (mode & OpmodeSta) {
        uint32_t ip { 0 };
        uint32_t netmask { 0 };
        uint32_t gateway { 0 };
        sdk::wifi_sta_address(ip, netmask, gateway);
        out.printf_P(PSTR("sta %4s ip %s gateway %s mask %s\n"),
            internal::state.linked ? "up" : "down",
            ipToString(espurna::Ip4(ip)).c_str(),
            ipToString(espurna::Ip4(gateway)).c_str(),
            ipToString(espurna::Ip4(netmask)).c_str());
    }

    if (mode & OpmodeAp) {
        out.printf_P(PSTR("ap     up ip %s\n"),
            ipToString(espurna::Ip4(sdk::wifi_ap_ip())).c_str());
    }

    for (int n = 0; n < DNS_MAX_SERVERS; ++n) {
        const auto* addr = dns_getserver(n);
        if (!addr || ip_addr_isany(addr)) {
            break;
        }

        out.printf_P(PSTR("dns %s\n"),
            ipToString(espurna::Ip4(ip_2_ip4(addr)->addr)).c_str());
    }
}

} // namespace driver
} // namespace wifi
} // namespace espurna
