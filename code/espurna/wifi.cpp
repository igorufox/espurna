/*

WIFI MODULE

Original code based on JustWifi, Wifi Manager for ESP8266 (GPLv3+)
Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

Modified for ESPurna
Copyright (C) 2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Simplified (owner 2026-11-18): the SDK picks the AP and roams (802.11k/v), no action queue and state machine of the
NONOS era, no roaming scans of ESPurna's own:
- networks wifi0..N are tried in their order, each WIFI_CONNECT_RETRIES times WIFI_CONNECT_INTERVAL apart; the SDK
  scans every channel and joins the strongest AP of the network (the arch driver)
- a round without a connection: the fallback AP (wifiApMode fallback) and a new round after WIFI_RECONNECT_INTERVAL
- a lost connection: a new round after WIFI_CONNECT_INTERVAL
- an attempt the SDK never reports the end of times out (Beken, ESP32 seen)
Every request comes from the loop context (terminal, WebUI, buttons, the driver's events) and is done right there

*/

#include "espurna.h"

#include "espurna_wifi.h"
#include "wifi_driver.h"
#include "rtcmem.h"
#include <wifi_sdk.h>

#include "ip4.h"

#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/stats.h>
#if !NO_SYS
#include <lwip/tcpip.h>
#endif

#include <algorithm>
#include <array>
#include <forward_list>
#include <functional>

#if WEB_SUPPORT
#include "ws.h"
#endif

// -----------------------------------------------------------------------------
// INTERNAL
// -----------------------------------------------------------------------------

namespace espurna {
namespace wifi {
namespace {

namespace build {

constexpr float txPower() {
    return WIFI_OUTPUT_POWER_DBM;
}

constexpr sleep_type_t sleep() {
    return WIFI_SLEEP_MODE;
}

constexpr BootMode bootMode() {
    return WIFI_BOOT_MODE;
}

} // namespace build

namespace settings {
namespace options {

PROGMEM_STRING(Disabled, "off");
PROGMEM_STRING(Enabled, "on");

} // namespace options
} // namespace settings

namespace ap {
namespace settings {
namespace options {

PROGMEM_STRING(Fallback, "fallback");

static constexpr espurna::settings::options::Enumeration<ApMode> ApModeOptions[] PROGMEM {
    {ApMode::Disabled, wifi::settings::options::Disabled},
    {ApMode::Enabled, wifi::settings::options::Enabled},
    {ApMode::Fallback, Fallback},
};

} // namespace options
} // namespace settings
} // namespace ap

namespace settings {
namespace options {

PROGMEM_STRING(None, "none");
PROGMEM_STRING(Modem, "modem");
PROGMEM_STRING(Light, "light");

static constexpr espurna::settings::options::Enumeration<sleep_type_t> SleepTypeOptions[] PROGMEM {
    {NONE_SLEEP_T, None},
    {MODEM_SLEEP_T, Modem},
    {LIGHT_SLEEP_T, Light},
};

} // namespace options
} // namespace settings

} // namespace
} // namespace wifi

namespace settings {
namespace internal {

template<>
wifi::BootMode convert(const espurna::string& value) {
    return convert<bool>(value)
        ? wifi::BootMode::Enabled
        : wifi::BootMode::Disabled;
}

espurna::string serialize(wifi::BootMode mode) {
    return serialize(mode == wifi::BootMode::Enabled);
}

template<>
wifi::StaMode convert(const espurna::string& value) {
    return convert<bool>(value)
        ? wifi::StaMode::Enabled
        : wifi::StaMode::Disabled;
}

espurna::string serialize(wifi::StaMode mode) {
    return serialize(mode == wifi::StaMode::Enabled);
}

template<>
wifi::ApMode convert(const espurna::string& value) {
    return convert(wifi::ap::settings::options::ApModeOptions, value, wifi::ApMode::Fallback);
}

espurna::string serialize(wifi::ApMode mode) {
    return serialize(wifi::ap::settings::options::ApModeOptions, mode);
}

template <>
sleep_type_t convert(const espurna::string& value) {
    return convert(wifi::settings::options::SleepTypeOptions, value, wifi::build::sleep());
}

espurna::string serialize(sleep_type_t sleep) {
    return serialize(wifi::settings::options::SleepTypeOptions, sleep);
}

template <>
espurna::Ip4 convert(const espurna::string& value) {
    espurna::Ip4 out;
    out.fromString(value.c_str());
    return out;
}

// xxxxxxxxxxxx or xx:xx:xx:xx:xx:xx
template <>
wifi::Mac convert(const espurna::string& value) {
    wifi::Mac out{};

    static constexpr size_t Min { 12 };
    static constexpr size_t Max { 17 };

    char buffer[Min + 1];
    size_t length { 0 };
    if (value.length() == Min) {
        memcpy(buffer, value.c_str(), Min);
        length = Min;
    } else if (value.length() == Max) {
        for (auto it = value.begin(); (it != value.end()) && (length < Min); ++it) {
            if ((*it) != ':') {
                buffer[length++] = *it;
            }
        }
    }

    if (length == Min) {
        hexDecode(buffer, length, out.data(), out.size());
    }

    return out;
}

// both 0.0.0.0 and 255.255.255.255 are saved as an empty string
espurna::string serialize(const espurna::Ip4& ip) {
    return wifi::driver::ip_set(ip) ? ipToString(ip) : espurna::empty_string;
}

espurna::string serialize(wifi::Mac mac) {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return espurna::string(buffer);
}

} // namespace internal
} // namespace settings

namespace wifi {
namespace {

static constexpr uint8_t OpmodeNull { driver::OpmodeNull };
static constexpr uint8_t OpmodeSta { driver::OpmodeSta };
static constexpr uint8_t OpmodeAp { driver::OpmodeAp };
static constexpr uint8_t OpmodeApSta { driver::OpmodeApSta };

namespace internal {

bool enabled { false };

std::forward_list<EventCallback> callbacks;

} // namespace internal

void publish(Event event) {
    for (auto& callback : internal::callbacks) {
        callback(event);
    }
}

void tx_power(float dbm) {
    if (std::isinf(dbm) || std::isnan(dbm)) {
        return;
    }

    driver::tx_power(dbm);
}

sleep_type_t sleep_type() {
    return driver::sleep_type();
}

bool sleep_type(sleep_type_t type) {
    return driver::sleep_type(type);
}

uint8_t opmode() {
    return driver::opmode();
}

// the mode really changes or the board restarts: every later step depends on it
void ensure_opmode(uint8_t mode) {
    const auto is_set = [&]() {
        return (opmode() == mode);
    };

    if (!is_set()) {
        const auto current = opmode();
        driver::opmode(mode);

        const auto result = time::blockingDelay(
            duration::Seconds(1),
            duration::Milliseconds(10),
            [&]() {
                return !is_set();
            });
        if (result) {
            abort();
        }

        if (current == OpmodeNull) {
            driver::wakeup();
        }
    }
}

namespace debug {

espurna::string mac(Mac mac) {
    return espurna::settings::internal::serialize(mac);
}

espurna::string authmode(AUTH_MODE mode) {
    return driver::authmode(mode);
}

const char* opmode(uint8_t mode) {
    switch (mode) {
    case OpmodeApSta:
        return "AP+STA";
    case OpmodeSta:
        return "STA";
    case OpmodeAp:
        return "AP";
    }

    return "NULL";
}

espurna::string sleep_type(sleep_type_t type) {
    return espurna::settings::internal::serialize(type);
}

} // namespace debug

namespace settings {
namespace keys {

PROGMEM_STRING(TxPower, "wifiTxPwr");
PROGMEM_STRING(Sleep, "wifiSleep");
PROGMEM_STRING(Boot, "wifiBoot");

} // namespace keys

float txPower() {
    return getSetting(keys::TxPower, build::txPower());
}

sleep_type_t sleep() {
    return getSetting(keys::Sleep, build::sleep());
}

BootMode bootMode() {
    return getSetting(keys::Boot, build::bootMode());
}

namespace query {
namespace internal {

#define EXACT_VALUE(NAME, FUNC)\
espurna::string NAME () {\
    return espurna::settings::internal::serialize(FUNC());\
}

#define ID_VALUE(NAME, FUNC)\
espurna::string NAME (size_t id) {\
    return espurna::settings::internal::serialize(FUNC(id));\
}

EXACT_VALUE(sleep, settings::sleep)
EXACT_VALUE(txPower, settings::txPower)
EXACT_VALUE(bootMode, settings::bootMode)

} // namespace internal
} // namespace query
} // namespace settings

// Scan results are byte arrays, not C strings (ref. driver::ScanResult)

espurna::string convertSsid(const driver::ScanResult& info) {
    espurna::string ssid;
    ssid.concat(reinterpret_cast<const char*>(info.ssid), info.ssid_len);
    return ssid;
}

Mac convertBssid(const driver::ScanResult& info) {
    static_assert(sizeof(info.bssid) == 6, "");
    Mac mac;
    std::copy(info.bssid, info.bssid + sizeof(info.bssid), mac.begin());
    return mac;
}

// -----------------------------------------------------------------------------
// STATION
// -----------------------------------------------------------------------------

namespace sta {
namespace build {

static constexpr size_t NetworksMax { WIFI_MAX_NETWORKS };

static constexpr auto ConnectionInterval = duration::Milliseconds{ WIFI_CONNECT_INTERVAL };
static constexpr auto ReconnectionInterval = duration::Milliseconds{ WIFI_RECONNECT_INTERVAL };
static constexpr int ConnectionRetries { WIFI_CONNECT_RETRIES };

constexpr StaMode mode() {
    return WIFI_STA_MODE;
}

#define WIFI_SETTING_STRING_RESULT(FIRST, SECOND, THIRD, FOURTH, FIFTH)\
    (index == 0) ? STRING_VIEW_SETTING(FIRST) :\
    (index == 1) ? STRING_VIEW_SETTING(SECOND) :\
    (index == 2) ? STRING_VIEW_SETTING(THIRD) :\
    (index == 3) ? STRING_VIEW_SETTING(FOURTH) :\
    (index == 4) ? STRING_VIEW_SETTING(FIFTH) : StringView()

StringView ssid(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_SSID, WIFI2_SSID, WIFI3_SSID, WIFI4_SSID, WIFI5_SSID);
}

StringView passphrase(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_PASS, WIFI2_PASS, WIFI3_PASS, WIFI4_PASS, WIFI5_PASS);
}

StringView ip(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_IP, WIFI2_IP, WIFI3_IP, WIFI4_IP, WIFI5_IP);
}

StringView gateway(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_GW, WIFI2_GW, WIFI3_GW, WIFI4_GW, WIFI5_GW);
}

StringView netmask(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_MASK, WIFI2_MASK, WIFI3_MASK, WIFI4_MASK, WIFI5_MASK);
}

StringView dns(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_DNS, WIFI2_DNS, WIFI3_DNS, WIFI4_DNS, WIFI5_DNS);
}

StringView bssid(size_t index) {
    return WIFI_SETTING_STRING_RESULT(WIFI1_BSSID, WIFI2_BSSID, WIFI3_BSSID, WIFI4_BSSID, WIFI5_BSSID);
}

#undef WIFI_SETTING_STRING_RESULT

constexpr uint8_t channel(size_t index) {
    return (
        (index == 0) ? WIFI1_CHANNEL :
        (index == 1) ? WIFI2_CHANNEL :
        (index == 2) ? WIFI3_CHANNEL :
        (index == 3) ? WIFI4_CHANNEL :
        (index == 4) ? WIFI5_CHANNEL : 0
    );
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Mode, "wifiStaMode");

PROGMEM_STRING(Ssid, "ssid");
PROGMEM_STRING(Passphrase, "pass");

PROGMEM_STRING(Ip, "ip");
PROGMEM_STRING(Gateway, "gw");
PROGMEM_STRING(Netmask, "mask");
PROGMEM_STRING(Dns, "dns");

// a network pinned to one AP: the SDK joins only this BSSID (on this channel)
PROGMEM_STRING(Bssid, "bssid");
PROGMEM_STRING(Channel, "chan");

} // namespace keys

espurna::Ip4 from_ipaddress(espurna::settings::Key key, StringView defaultValue) {
    return espurna::settings::internal::convert<espurna::Ip4>(
        getSetting(key, defaultValue));
}

StaMode mode() {
    return getSetting(keys::Mode, build::mode());
}

espurna::string ssid(size_t index) {
    return getSetting({keys::Ssid, index}, build::ssid(index));
}

espurna::string passphrase(size_t index) {
    return getSetting({keys::Passphrase, index}, build::passphrase(index));
}

espurna::Ip4 ip(size_t index) {
    return from_ipaddress({keys::Ip, index}, build::ip(index));
}

espurna::Ip4 gateway(size_t index) {
    return from_ipaddress({keys::Gateway, index}, build::gateway(index));
}

espurna::Ip4 netmask(size_t index) {
    return from_ipaddress({keys::Netmask, index}, build::netmask(index));
}

espurna::Ip4 dns(size_t index) {
    return from_ipaddress({keys::Dns, index}, build::dns(index));
}

Mac bssid(size_t index) {
    return espurna::settings::internal::convert<Mac>(
        getSetting({keys::Bssid, index}, build::bssid(index)));
}

int8_t channel(size_t index) {
    return getSetting({keys::Channel, index}, build::channel(index));
}

namespace query {
namespace internal {

ID_VALUE(ip, settings::ip)
ID_VALUE(gateway, settings::gateway)
ID_VALUE(netmask, settings::netmask)
ID_VALUE(dns, settings::dns)
ID_VALUE(bssid, settings::bssid)
ID_VALUE(channel, settings::channel)

EXACT_VALUE(mode, settings::mode)

} // namespace internal

static constexpr std::array<espurna::settings::query::IndexedSetting, 8> Settings PROGMEM {
    {{keys::Ssid, settings::ssid},
     {keys::Passphrase, settings::passphrase},
     {keys::Ip, internal::ip},
     {keys::Gateway, internal::gateway},
     {keys::Netmask, internal::netmask},
     {keys::Dns, internal::dns},
     {keys::Bssid, internal::bssid},
     {keys::Channel, internal::channel}}
};

} // namespace query
} // namespace settings

espurna::Ip4 ip() {
    return driver::sta_ip();
}

size_t countNetworks() {
    size_t out { 0 };
    for (size_t id = 0; id < build::NetworksMax; ++id) {
        if (!settings::ssid(id).length()) {
            break;
        }

        ++out;
    }

    return out;
}

StaNetwork current() {
    return driver::sta_current();
}

bool enabled() {
    return opmode() & OpmodeSta;
}

void disconnect() {
    if (enabled()) {
        driver::sta_disconnect();
    }
}

void enable() {
    ensure_opmode(opmode() | OpmodeSta);
    driver::sta_prepare();
}

void disable() {
    ensure_opmode(opmode() & ~OpmodeSta);
}

// --- lwIP, the same on every chip ----------------------------------------------------------------------------------
//
// lwIP is called in its own context: directly without an OS (NO_SYS), else through the tcpip thread (the loop would
// race with it). A full tcpip mailbox drops the call: the next one comes on the next tick
namespace lwip {

void call(void (*func)(void*)) {
#if NO_SYS
    func(nullptr);
#else
    tcpip_try_callback(func, nullptr);
#endif
}

// gratuitous ARP on every interface that is up with an address
[[gnu::unused]]
void gratuitous(void*) {
    for (netif* interface = netif_list; interface != nullptr; interface = interface->next) {
        if ((interface->flags & NETIF_FLAG_ETHARP)
            && (interface->hwaddr_len == ETHARP_HWADDR_LEN)
            && !ip4_addr_isany_val(*netif_ip4_addr(interface))
            && (interface->flags & NETIF_FLAG_LINK_UP)
            && (interface->flags & NETIF_FLAG_UP))
        {
            etharp_gratuitous(interface);
        }
    }
}

// an ARP request to the gateway of the default interface
[[gnu::unused]]
void probe(void*) {
    auto* netif = netif_default;
    if (netif && netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_gw(netif))) {
        etharp_request(netif, netif_ip4_gw(netif));
    }
}

} // namespace lwip

namespace connection {
namespace internal {

// the round: the networks in their order, ConnectionRetries attempts each
size_t network { 0 };
int retry { 0 };
bool round { false };

bool connected { false };
bool waiting { false };
time::SystemClock::time_point attempt_at{};

// this attempt went to the AP of the last connection (last_ap)
bool fast { false };

// the quick attempts after an AP rejected the association, in this round (ref. AssocRetries)
int assoc_retry { 0 };

timer::SystemTimer timer;

// the station is wanted (wifiStaMode on, WIFI.STA): a lost connection starts a new round
bool persist { false };

} // namespace internal

bool connected() {
    return internal::connected;
}

bool connecting() {
    return internal::round;
}

namespace internal {

// WIFI.DEBUG
struct State {
    bool round;
    size_t network;
    int retry;
    bool waiting;
    uint32_t waited;
    bool connected;
    bool persist;
};

State state() {
    return State{
        .round = round,
        .network = network,
        .retry = retry,
        .waiting = waiting,
        .waited = waiting ? static_cast<uint32_t>(std::chrono::duration_cast<duration::Milliseconds>(
            time::SystemClock::now() - attempt_at).count()) : 0u,
        .connected = connected,
        .persist = persist,
    };
}

} // namespace internal

// the SDK does not always report the end of an attempt (Beken, ESP32 seen; the ESP8266 SDK may stall too): the
// attempt ends here after this long, on every chip
constexpr auto AttemptTimeout = duration::Milliseconds{ 15000 };

void stop() {
    internal::timer.stop();
    internal::round = false;
    internal::waiting = false;
}

void schedule(duration::Milliseconds next, void (*callback)()) {
    internal::timer.once(next, callback);
    DEBUG_MSG_P(PSTR("[WIFI] Next connection attempt in %u (ms)\n"),
        static_cast<unsigned>(next.count()));
}

void start();

// the AP of the last connection of each network, in the settings (flash; owner 20.11: the board learns it itself and
// keeps it with the network): the first attempt of the network goes straight to its BSSID and channel, no scan of every
// channel (2 s of every connect, 19.11). The first connection of a network goes by the SDK's scan of every channel and
// is kept then; a failed attempt to the kept AP forgets it, the next one scans again. Written only when it changes.
// lastApN = "<ssid hash, 8 hex> <bssid, 12 hex> <channel, 2 hex>"
namespace last_ap {

PROGMEM_STRING(Key, "lastAp");

uint32_t hash(StringView ssid) {
    uint32_t out = 2166136261u;
    for (const auto c : ssid) {
        out = (out ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return out ? out : 1u;
}

bool hex(const char* in, size_t digits, uint32_t& out) {
    out = 0;
    for (size_t index = 0; index < digits; ++index) {
        const char c = in[index];
        uint32_t value;
        if ((c >= '0') && (c <= '9')) {
            value = c - '0';
        } else if ((c >= 'a') && (c <= 'f')) {
            value = c - 'a' + 10;
        } else {
            return false;
        }
        out = (out << 4) | value;
    }
    return true;
}

bool take(size_t id, StringView ssid, Mac& bssid, uint8_t& channel) {
    const auto value = getSetting({Key, id});
    if (value.length() != 24) {
        return false;
    }

    const char* in = value.c_str();
    uint32_t number;
    if (!hex(in, 8, number) || (number != hash(ssid))) {
        return false;
    }

    for (size_t index = 0; index < bssid.size(); ++index) {
        if (!hex(in + 9 + (index * 2), 2, number)) {
            return false;
        }
        bssid[index] = static_cast<uint8_t>(number);
    }

    if (!hex(in + 22, 2, number) || !number || (number > 14)) {
        return false;
    }
    channel = static_cast<uint8_t>(number);

    return true;
}

void keep(size_t id, StringView ssid) {
    Mac bssid{};
    AUTH_MODE authmode{};
    driver::sta_info(bssid, authmode);
    const auto channel = driver::sta_channel();
    if (!channel) {
        return;
    }

    char value[25];
    snprintf_P(value, sizeof(value), PSTR("%08x %02x%02x%02x%02x%02x%02x %02x"),
        static_cast<unsigned>(hash(ssid)),
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        static_cast<unsigned>(channel));

    const espurna::settings::Key key{Key, id};
    const espurna::string text(value);
    if (getSetting(key) != text) {
        setSetting(key, text);
        saveSettings();
    }
}

void forget(size_t id) {
    if (delSetting(espurna::settings::Key{Key, id})) {
        saveSettings();
    }
}

} // namespace last_ap

// the attempt failed: the same network again, the next one, or the end of the round. `reason` is the 802.11 reason
// of the SDK's disconnect (0: no disconnect, the attempt timed out or could not start)
void failed(int reason);

// one attempt with the current network of the round, the result is the driver's event
void attempt() {
    if (!internal::round || internal::connected) {
        return;
    }

    const auto id = internal::network;
    const auto ssid = settings::ssid(id);
    const auto passphrase = settings::passphrase(id);

    // what the SDK station config can hold, same limits on every chip
    const bool valid = ssid.length() && (ssid.length() <= driver::SsidMax)
        && (!passphrase.length()
            || ((passphrase.length() >= driver::PassphraseMin)
             && (passphrase.length() <= driver::PassphraseMax)));

    if (valid) {
        // cancel what the SDK still does (a scan, an attempt): its disconnect event comes before the new attempt
        driver::sta_disconnect();
        espurna::time::yield();

        const auto ip = settings::ip(id);
        const bool dhcp = !driver::ip_set(ip);
        const auto netmask = dhcp ? espurna::Ip4() : settings::netmask(id);
        const auto gateway = dhcp ? espurna::Ip4() : settings::gateway(id);
        const auto dns = dhcp ? espurna::Ip4() : settings::dns(id);

        auto channel = static_cast<uint8_t>(settings::channel(id));
        auto bssid = channel ? settings::bssid(id) : Mac{};
        internal::fast = false;
        if (!channel && !internal::retry && last_ap::take(id, ssid, bssid, channel)) {
            internal::fast = true;
        }

        const auto hostname = espurna::string(systemHostname());
        if (driver::sta_connect(driver::StaConnect{
                .hostname = hostname,
                .ssid = ssid,
                .passphrase = passphrase,
                .dhcp = dhcp,
                .ip = ip,
                .netmask = netmask,
                .gateway = gateway,
                .dns = dns,
                .bssid = bssid,
                .channel = channel,
            }))
        {
            internal::waiting = true;
            internal::attempt_at = time::SystemClock::now();
            publish(Event::StationConnecting);
            return;
        }
    }

    // a network that cannot be tried is passed at once
    internal::retry = build::ConnectionRetries;
    failed(0);
}

// The AP rejected the association (WIFI_REASON_ASSOC_FAIL, the same code in the three SDKs): with PMF (foxy
// ieee80211w) it answers "rejected temporarily" (status 30) while it still holds the previous association of this
// station - after a restart or wifi.reset the station has no keys for it - and drops that one by its SA Query within
// about a second. So the next attempt goes soon and is not counted against the round, a few times per round; ending
// the round there meant the next one only after WIFI_RECONNECT_INTERVAL (regress-todo 40: 37 of 40 wifi.reset back
// after 144 s, COM3 20.11)
constexpr int AssocFail { 203 };

// attempt() cancels whatever the SDK still does (driver::sta_disconnect()), and the driver reports that cancel as a
// disconnect (WIFI_REASON_ASSOC_LEAVE of the three SDKs) from the loop - after this attempt already began. Counted as
// this attempt's failure, it scheduled the next attempt, whose cancel then cut the real SAE of this one (5 s): every
// attempt after a wifi.reset "failed" at 2.7 s and the round ended (regress-todo 40, COM3 20.11)
constexpr int AssocLeave { 8 };
constexpr auto CancelWindow = duration::Milliseconds{ 500 };
constexpr auto AssocRetryInterval = duration::Milliseconds{ 1500 };
constexpr int AssocRetries { 3 };

void failed(int reason) {
    if (internal::fast) {
        internal::fast = false;
        last_ap::forget(internal::network);
    }

    if (internal::round && (AssocFail == reason) && (internal::assoc_retry < AssocRetries)) {
        ++internal::assoc_retry;
        schedule(AssocRetryInterval, attempt);
        publish(Event::StationTimeout);
        return;
    }

    if (internal::round) {
        if (++internal::retry < build::ConnectionRetries) {
            schedule(build::ConnectionInterval, attempt);
            publish(Event::StationTimeout);
            return;
        }

        internal::retry = 0;
        if (++internal::network < countNetworks()) {
            schedule(build::ConnectionInterval, attempt);
            publish(Event::StationTimeout);
            return;
        }
    }

    // no connection after the round: the fallback AP (StationReconnect), another round later
    stop();
    if (internal::persist && enabled()) {
        schedule(build::ReconnectionInterval, start);
    }

    publish(Event::StationReconnect);
}

// a new round from the first network
void start() {
    if (!enabled() || !countNetworks()) {
        stop();
        publish(Event::StationReconnect);
        return;
    }

    // a round started by hand (WIFI.RESET, settings) while the timer holds another start or attempt: that one would
    // begin the round again a few seconds later and cut the attempt in progress (20.11: wifi.reset right after the
    // disconnect of the station, which schedules a start in WIFI_CONNECT_INTERVAL)
    internal::timer.stop();

    internal::network = 0;
    internal::retry = 0;
    internal::assoc_retry = 0;
    internal::round = true;
    attempt();
}

void persist(bool value) {
    internal::persist = value;
}

// the driver's events, always from the loop context

void disconnected(int reason) {
    const bool was = internal::connected;
    internal::connected = false;

    if (internal::waiting) {
        if ((AssocLeave == reason) && ((time::SystemClock::now() - internal::attempt_at) < CancelWindow)) {
            return;
        }

        internal::waiting = false;
        failed(reason);
        return;
    }

    if (was) {
        publish(Event::StationDisconnected);
        if (internal::persist && enabled()) {
            schedule(build::ConnectionInterval, start);
        }
    }
}

void got_ip() {
    internal::waiting = false;
    internal::connected = true;
    internal::fast = false;
    last_ap::keep(internal::network, settings::ssid(internal::network));
    stop();
    publish(Event::StationConnected);
}

void loop() {
    if (internal::waiting && ((time::SystemClock::now() - internal::attempt_at) >= AttemptTimeout)) {
        DEBUG_MSG_P(PSTR("[WIFI] Connection attempt timed out\n"));
        internal::waiting = false;
        driver::sta_disconnect();
        failed(0);
    }
}

} // namespace connection

bool connected() {
    return connection::connected();
}

bool connecting() {
    return connection::connecting();
}

// The Wi-Fi link can stay associated while the IP stack receives nothing at all: 10.0.5.116 (Beken) 2026-11-07, first
// boot after the Matter code was written by an install, DHCP done, then no ARP / ping answers for 28 min. The loop kept
// running and feeding the watchdog, so nothing reset it; a deauth from the AP did. Every Interval, while connected with
// an address, whatever arrived (ARP + IPv4 + IPv6 packets) is compared with the last check. Nothing new => an ARP
// request goes to the gateway (answers within ms, so a healthy stack always sees traffic by the next check); Misses
// checks in a row without a single received packet => restart. The packets are counted at the input of the default
// interface (its netif->input wrapped, every driver hands frames through it): lwIP's own counters (LWIP_STATS) are off
// in the ESP8266 / ESP32 SDK configs, and this costs 8 bytes instead of their table
namespace netwatch {

constexpr auto Interval = espurna::duration::Seconds(20);
constexpr int Misses = 6;

volatile uint32_t rx { 0 };
netif_input_fn original { nullptr };

uint32_t last_rx { 0 };
int misses { 0 };
timer::SystemTimer timer;

// the driver's context (Wi-Fi task / tcpip thread): one aligned word, read by the loop
err_t counted(pbuf* p, netif* interface) {
    rx = rx + 1;
    return original(p, interface);
}

// lwIP context: the default interface gets the wrapper (again after the SDK re-adds it); another input function than
// the one already wrapped is left alone
void wrap(void*) {
    auto* interface = netif_default;
    if (!interface || !interface->input || (interface->input == counted)) {
        return;
    }

    if (original && (interface->input != original)) {
        return;
    }

    original = interface->input;
    interface->input = counted;
}

bool addressed() {
    const auto* netif = netif_default;
    return netif && netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif));
}

void check() {
    lwip::call(wrap);

    const uint32_t now = rx;
    if (!connected() || !addressed() || (now != last_rx)) {
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
    lwip::call(lwip::probe);
}

void setup() {
    timer.repeat(Interval, check);
}

} // namespace netwatch

#if WIFI_GRATUITOUS_ARP_SUPPORT
// ref: https://github.com/xoseperez/espurna/pull/1877#issuecomment-525612546
// Periodically send out ARP, even if no one asked
namespace garp {
namespace build {

static constexpr auto IntervalMin = duration::Milliseconds{ WIFI_GRATUITOUS_ARP_INTERVAL_MIN };
static constexpr auto IntervalMax = duration::Milliseconds{ WIFI_GRATUITOUS_ARP_INTERVAL_MAX };

} // namespace build

duration::Milliseconds interval() {
    static const auto defaultInterval = duration::Milliseconds(
        ::randomNumber(build::IntervalMin.count(), build::IntervalMax.count()));
    return getSetting("wifiGarpIntvl", defaultInterval);
}

timer::SystemTimer timer;

void send() {
    if (connected()) {
        lwip::call(lwip::gratuitous);
    }
}

void configure() {
    const auto value = interval();
    if (value.count()) {
        timer.repeat(value, send);
    } else {
        timer.stop();
    }
}

} // namespace garp
#endif

// on demand only (WebUI, terminal, the setup page): the connection itself needs no scan of ours
namespace scan {

using Found = std::function<void(const driver::ScanResult&)>;
using Done = std::function<void(const char* error)>;

namespace internal {

Found found;
Done done;

} // namespace internal

bool scanning() {
    return static_cast<bool>(internal::done);
}

bool start(Found&& found, Done&& done) {
    if (scanning()) {
        done(PSTR("Scan already in progress"));
        return false;
    }

    if (!enabled()) {
        done(PSTR("Station is off"));
        return false;
    }

    if (!driver::scan_start()) {
        done(PSTR("System unable to start the scan"));
        return false;
    }

    internal::found = std::move(found);
    internal::done = std::move(done);
    return true;
}

// waits for the end of the scan (the terminal command)
bool wait(Found&& found, Done&& done) {
    const auto out = start(std::move(found), std::move(done));
    while (scanning()) {
        time::delay(duration::Milliseconds(100));
        driver::loop();
    }

    return out;
}

void result(const driver::ScanResult& info) {
    if (internal::found) {
        internal::found(info);
    }
}

void finished(bool ok, size_t count) {
    auto done = std::move(internal::done);
    internal::found = nullptr;
    internal::done = nullptr;
    if (done) {
        done(!ok ? PSTR("System unable to complete the scan")
            : !count ? PSTR("No networks")
            : nullptr);
    }
}

} // namespace scan

void stop() {
    connection::stop();
    if (enabled()) {
        disconnect();
        disable();
        publish(Event::Mode);
    }
}

void begin() {
    if (!enabled()) {
        enable();
        publish(Event::Mode);
    }

    if (!connected() && !connecting()) {
        connection::start();
    }
}

void toggle() {
    const bool on = enabled();
    connection::persist(!on);
    if (on) {
        stop();
    } else {
        begin();
    }
}

void configure() {
    const bool on = (StaMode::Enabled == settings::mode());
    connection::persist(on);
    if (on) {
        begin();
    } else {
        stop();
    }

#if WIFI_GRATUITOUS_ARP_SUPPORT
    garp::configure();
#endif
}

} // namespace sta

// -----------------------------------------------------------------------------
// ACCESS POINT
// -----------------------------------------------------------------------------

namespace ap {
namespace build {

static constexpr size_t SsidMax { driver::SsidMax };

static constexpr size_t PassphraseMin { driver::PassphraseMin };
static constexpr size_t PassphraseMax { driver::PassphraseMax };

PROGMEM_STRING(ApSsid, WIFI_AP_SSID);

constexpr StringView ssid() {
    return ApSsid;
}

PROGMEM_STRING(ApPass, WIFI_AP_PASS);

constexpr StringView passphrase() {
    return ApPass;
}

constexpr bool captive() {
    return 1 == WIFI_AP_CAPTIVE_ENABLED;
}

constexpr ApMode mode() {
    return WIFI_AP_MODE;
}

constexpr uint8_t channel() {
    return WIFI_AP_CHANNEL;
}

constexpr auto FallbackTimeout = duration::Milliseconds{ WIFI_FALLBACK_TIMEOUT };

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Mode, "wifiApMode");
PROGMEM_STRING(Ssid, "wifiApSsid");
PROGMEM_STRING(Passphrase, "wifiApPass");
PROGMEM_STRING(Channel, "wifiApChan");
PROGMEM_STRING(Captive, "wifiApCaptive");

} // namespace keys

ApMode mode() {
    return getSetting(keys::Mode, build::mode());
}

espurna::string ssid() {
    return getSetting(keys::Ssid, build::ssid().length()
        ? build::ssid()
        : espurna::StringView(systemHostname()));
}

espurna::string passphrase() {
    return getSetting(keys::Passphrase, build::passphrase().length()
        ? build::passphrase()
        : espurna::StringView(systemPassword()));
}

int8_t channel() {
    return getSetting(keys::Channel, build::channel());
}

bool captive() {
    return getSetting(keys::Captive, build::captive());
}

namespace query {
namespace internal {

EXACT_VALUE(captive, ap::settings::captive)
EXACT_VALUE(channel, ap::settings::channel)
EXACT_VALUE(mode, ap::settings::mode)

#undef ID_VALUE
#undef EXACT_VALUE

} // namespace internal
} // namespace query
} // namespace settings

namespace internal {

// when the AP was started, ref. check()
time::SystemClock::time_point started_at{};

} // namespace internal

espurna::Ip4 ip() {
    return driver::ap_ip();
}

bool enabled() {
    return opmode() & OpmodeAp;
}

size_t stations() {
    return driver::ap_stations();
}

SoftApNetwork current() {
    return driver::ap_current();
}

// Always a valid AP config, even when the configured credentials do not fit the SDK limits
void start() {
    if (enabled()) {
        return;
    }

    ensure_opmode(opmode() | OpmodeAp);

    const auto ssid = settings::ssid();
    const auto defaultSsid = espurna::string(systemIdentifier());
    const auto passphrase = settings::passphrase();

    const char* apSsid {
        (ssid.length() && (ssid.length() < build::SsidMax))
        ? ssid.c_str() : defaultSsid.c_str() };

    const char* apPass {
        (passphrase.length()
         && (passphrase.length() >= build::PassphraseMin)
         && (passphrase.length() < build::PassphraseMax))
        ? passphrase.c_str() : nullptr };

    driver::ap_start(apSsid, apPass, settings::channel());
    internal::started_at = time::SystemClock::now();
    publish(Event::Mode);
}

// the captive portal DNS is the captive module's (captive.cpp), it follows the AP through Event::Mode
void stop() {
    if (!enabled()) {
        return;
    }

    driver::ap_stop();
    ensure_opmode(opmode() & ~OpmodeAp);
    publish(Event::Mode);
}

void toggle() {
    if (enabled()) {
        stop();
    } else {
        start();
    }
}

// The fallback AP goes away once the station is connected and nobody uses the AP: not sooner than one timeout after
// its start (nobody had a chance to join it yet; the check also runs on every WebSocket disconnect, ws_server.cpp)
void check() {
    if (enabled()
        && (ApMode::Fallback == settings::mode())
        && sta::connected()
        && !stations()
        && (time::SystemClock::now() - internal::started_at >= build::FallbackTimeout))
    {
        stop();
    }
}

namespace fallback {

timer::SystemTimer timer;

} // namespace fallback

void configure() {
    switch (settings::mode()) {
    case ApMode::Enabled:
        fallback::timer.stop();
        start();
        break;
    case ApMode::Disabled:
        fallback::timer.stop();
        stop();
        break;
    case ApMode::Fallback:
        fallback::timer.repeat(build::FallbackTimeout, check);
        break;
    }
}

// the station got no connection in a whole round
void station_lost() {
    if (ApMode::Fallback == settings::mode()) {
        start();
    }
}

} // namespace ap

// -----------------------------------------------------------------------------
// SETTINGS
// -----------------------------------------------------------------------------

namespace settings {

STRING_VIEW_INLINE(Prefix, "wifi");

namespace query {

static constexpr std::array<espurna::settings::query::Setting, 9> Settings PROGMEM {
    {{ap::settings::keys::Ssid, ap::settings::ssid},
     {ap::settings::keys::Passphrase, ap::settings::passphrase},
     {ap::settings::keys::Captive, ap::settings::query::internal::captive},
     {ap::settings::keys::Channel, ap::settings::query::internal::channel},
     {ap::settings::keys::Mode, ap::settings::query::internal::mode},
     {sta::settings::keys::Mode, sta::settings::query::internal::mode},
     {settings::keys::TxPower, query::internal::txPower},
     {settings::keys::Sleep, query::internal::sleep},
     {settings::keys::Boot, query::internal::bootMode},
    }
};

// indexed settings for 'sta' connections
bool checkIndexedPrefix(StringView key) {
    return espurna::settings::query::hasSamePrefix(
        sta::settings::query::Settings, key);
}

// generic 'ap' and 'modem' configuration
bool checkExactPrefix(StringView key) {
    return key.startsWith(Prefix);
}

espurna::settings::query::Result findIndexedFrom(StringView key) {
    return espurna::settings::query::findFrom(
        sta::countNetworks(),
        sta::settings::query::Settings, key);
}

espurna::settings::query::Result findFrom(StringView key) {
    return espurna::settings::query::findFrom(Settings, key);
}

void setup() {
    settingsRegisterQueryHandler({
        .check = checkIndexedPrefix,
        .get = findIndexedFrom,
    });

    settingsRegisterQueryHandler({
        .check = checkExactPrefix,
        .get = findFrom,
    });
}

} // namespace query

void migrate(int version) {
    if (version < 5) {
        moveSetting("apmode", ap::settings::keys::Mode);
    }
}

void configure() {
    if (!internal::enabled) {
        return;
    }

    ap::configure();
    sta::configure();

    // the station is off or has no networks to try: the fallback AP right away
    if (!sta::connected() && !sta::connecting()) {
        ap::station_lost();
    }

    sleep_type(settings::sleep());
    tx_power(settings::txPower());
}

} // namespace settings

void turn_on(bool boot) {
    if (internal::enabled) {
        return;
    }

    internal::enabled = true;
#if SYSTEM_CHECK_ENABLED
    // the board restarted several times in a row: only the AP, the station could be the cause
    if (boot && !systemCheck()) {
        ap::start();
        return;
    }
#endif
    settings::configure();
}

void turn_off() {
    if (!internal::enabled) {
        return;
    }

    ap::fallback::timer.stop();
    sta::connection::stop();
    sta::disconnect();
    ap::stop();
    ensure_opmode(OpmodeNull);
    internal::enabled = false;
    publish(Event::Mode);
}

// -----------------------------------------------------------------------------
// TERMINAL
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

PROGMEM_STRING(Stations, "WIFI.STATIONS");

void stations(::terminal::CommandContext&& ctx) {
    if (!driver::print_stations(ctx.output)) {
        terminalError(ctx, "No stations connected");
        return;
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Network, "NETWORK");

void network(::terminal::CommandContext&& ctx) {
    driver::print_network(ctx.output);
}

PROGMEM_STRING(Wifi, "WIFI");

void wifi(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() == 2) {
        auto id = espurna::settings::internal::convert<size_t>(ctx.argv[1]);
        if (id < sta::build::NetworksMax) {
            settingsDump(ctx, sta::settings::query::Settings, id);
            terminalOK(ctx);
            return;
        }

        terminalError(ctx, "Network ID out of configurable range");
        return;
    }

    const auto mode = wifi::opmode();
    ctx.output.printf_P(PSTR("OPMODE: %s\n"), debug::opmode(mode));

    const auto sleep = wifi::sleep_type();
    if (sleep != NONE_SLEEP_T) {
        ctx.output.printf_P(PSTR("SLEEP: %s\n"), debug::sleep_type(sleep).c_str());
    }

    if (mode & OpmodeAp) {
        auto current = ap::current();
        ctx.output.printf_P(PSTR("SoftAP: bssid %s channel %hhu auth %s ssid \"%s\"\n"),
            debug::mac(current.bssid).c_str(),
            current.channel,
            debug::authmode(current.authmode).c_str(),
            current.ssid.c_str());
    }

    if (mode & OpmodeSta) {
        if (sta::connected()) {
            auto network = sta::current();
            ctx.output.printf_P(PSTR("STA: bssid %s rssi %hhd channel %hhu ssid \"%s\"\n"),
                debug::mac(network.bssid).c_str(),
                network.rssi, network.channel, network.ssid.c_str());
        } else {
            ctx.output.printf_P(PSTR("STA: %s\n"),
                sta::connecting() ? "connecting" : "disconnected");
        }
    }

    settingsDump(ctx, settings::query::Settings);
    terminalOK(ctx);
}

PROGMEM_STRING(Reset, "WIFI.RESET");

void reset(::terminal::CommandContext&& ctx) {
    sta::connection::stop();
    sta::disconnect();
    settings::configure();
    terminalOK(ctx);
}

PROGMEM_STRING(Station, "WIFI.STA");

void station(::terminal::CommandContext&& ctx) {
    sta::toggle();
    terminalOK(ctx);
}

PROGMEM_STRING(AccessPoint, "WIFI.AP");

void access_point(::terminal::CommandContext&& ctx) {
    ap::toggle();
    terminalOK(ctx);
}

PROGMEM_STRING(Off, "WIFI.OFF");

void off(::terminal::CommandContext&& ctx) {
    turn_off();
    terminalOK(ctx);
}

PROGMEM_STRING(On, "WIFI.ON");

void on(::terminal::CommandContext&& ctx) {
    turn_on(false);
    terminalOK(ctx);
}

PROGMEM_STRING(Scan, "WIFI.SCAN");

void scan(::terminal::CommandContext&& ctx) {
    const char* failed { nullptr };
    sta::scan::wait(
        [&](const driver::ScanResult& info) {
            ctx.output.printf_P(PSTR("BSSID: %s AUTH: %11s RSSI: %3hhd CH: %2hhu SSID: %s\n"),
                debug::mac(convertBssid(info)).c_str(),
                debug::authmode(info.authmode).c_str(),
                info.rssi,
                info.channel,
                convertSsid(info).c_str());
        },
        [&](const char* error) {
            failed = error;
        });

    if (failed) {
        terminalError(ctx, espurna::StringView(failed));
        return;
    }

    terminalOK(ctx);
}

PROGMEM_STRING(Debug, "WIFI.DEBUG");

// why the station does (not) connect: the round of the attempts, the driver, the last disconnects of the SDK
void debug(::terminal::CommandContext&& ctx) {
    const auto& c = sta::connection::internal::state();
    ctx.output.printf_P(PSTR("round: %s network %u of %u retry %u of %u, waiting %s (%u ms), connected %s, persist %s\n"),
        c.round ? "yes" : "no", static_cast<unsigned>(c.network), static_cast<unsigned>(sta::countNetworks()),
        static_cast<unsigned>(c.retry), static_cast<unsigned>(sta::build::ConnectionRetries),
        c.waiting ? "yes" : "no", static_cast<unsigned>(c.waited), c.connected ? "yes" : "no", c.persist ? "yes" : "no");
    driver::print_debug(ctx.output);
    terminalOK(ctx);
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Stations, commands::stations},
    {Debug, commands::debug},
    {Network, commands::network},
    {Wifi, commands::wifi},
    {Reset, commands::reset},
    {Station, commands::station},
    {AccessPoint, commands::access_point},
    {Scan, commands::scan},
    {Off, commands::off},
    {On, commands::on},
};

} // namespace commands

void init() {
    espurna::terminal::add(commands::List);
}

} // namespace terminal
#endif

// -----------------------------------------------------------------------------
// WEB
// -----------------------------------------------------------------------------

#if WEB_SUPPORT
namespace web {

void onConnected(espurna::json::Writer& root) {
    for (const auto& setting : settings::query::Settings) {
        root.field(setting.key().c_str(), setting.value());
    }

    // {"networks": [[<values of network 0>], ...], "schema": [<the key prefixes>], "max": N} (EnumerableConfig of the
    // tools module, written here: the head links nothing of it)
    root.object(STRING_VIEW("wifiConfig"));
    root.array(STRING_VIEW("networks"));
    const auto networks = sta::countNetworks();
    for (size_t index = 0; index < networks; ++index) {
        root.beginArray();
        for (const auto& setting : sta::settings::query::Settings) {
            root.value(setting.value(index));
        }
        root.end();
    }
    root.end();

    root.array(STRING_VIEW("schema"));
    for (const auto& setting : sta::settings::query::Settings) {
        root.value(setting.prefix());
    }
    root.end();

    root.field(STRING_VIEW("max"), sta::build::NetworksMax);
    root.end();
}

bool onKeyCheck(StringView key, espurna::json::Value) {
    return settings::query::checkExactPrefix(key)
        || settings::query::checkIndexedPrefix(key);
}

struct ScanFound {
    Mac bssid;
    AUTH_MODE authmode;
    int8_t rssi;
    uint8_t channel;
    espurna::string ssid;
};

void onScan(uint32_t client_id) {
    sta::scan::start(
        [client_id](const driver::ScanResult& found) {
            ScanFound result{convertBssid(found), found.authmode, found.rssi, found.channel, convertSsid(found)};
            wsPost(client_id, [result](espurna::json::Writer& root) {
                root.array(STRING_VIEW("scanResult"));
                root.value(debug::mac(result.bssid));
                root.value(debug::authmode(result.authmode));
                root.value(result.rssi);
                root.value(result.channel);
                root.value(result.ssid);
                root.end();
            });
        },
        [client_id](const char* error) {
            if (!error) {
                return;
            }

            const espurna::string text(error);
            wsPost(client_id, [text](espurna::json::Writer& root) {
                root.field("scanError", text);
            });
        });
}

void onAction(uint32_t client_id, const char* action, espurna::json::Value) {
    if (STRING_VIEW("scan") == action) {
        onScan(client_id);
    }
}

} // namespace web
#endif

// -----------------------------------------------------------------------------
// INITIALIZATION
// -----------------------------------------------------------------------------

namespace debug {

[[gnu::unused]]
espurna::string event(Event value) {
    espurna::string out;

    switch (value) {
    case Event::Initial:
        out = "Initial";
        break;
    case Event::Mode:
        out = "Mode changed to ";
        out += opmode(wifi::opmode());
        break;
    case Event::StationInit:
        out = "Station init";
        break;
    case Event::StationScan:
        out = "Scanning";
        break;
    case Event::StationConnecting:
        out = "Connecting";
        break;
    case Event::StationConnected: {
        auto current = sta::current();
        out = "Connected to BSSID ";
        out += mac(current.bssid);
        out += " SSID ";
        out += current.ssid;
        break;
    }
    case Event::StationTimeout:
        out = "Connection attempt failed";
        break;
    case Event::StationDisconnected:
        out = "Disconnected";
        break;
    case Event::StationReconnect:
        out = "No connection, next round later";
        break;
    }

    return out;
}

} // namespace debug

void loop() {
    driver::loop();
    sta::connection::loop();
}

void setup() {
    if (!driver::setup()) {
        return;
    }

    driver::init();
    sta::disconnect();
    ensure_opmode(OpmodeNull);
    sta::netwatch::setup();

    migrateVersion(settings::migrate);
    settings::query::setup();

#if DEBUG_SUPPORT
    wifiRegister([](Event event) {
        DEBUG_MSG_P(PSTR("[WIFI] %s\n"), debug::event(event).c_str());
    });
#endif

    // the fallback AP comes up when a round of the station ends without a connection
    wifiRegister([](Event event) {
        if (Event::StationReconnect == event) {
            ap::station_lost();
        }
    });

#if WEB_SUPPORT
    wsRegister()
        .onAction(web::onAction)
        .onConnected(web::onConnected)
        .onKeyCheck(web::onKeyCheck);
#endif

#if TERMINAL_SUPPORT
    terminal::init();
#endif

    publish(Event::Initial);
    if (BootMode::Enabled == settings::bootMode()) {
        turn_on(true);
    }

    espurnaRegisterLoop(loop);
    espurnaRegisterReload(settings::configure);
}

} // namespace
} // namespace wifi
} // namespace espurna

// -----------------------------------------------------------------------------
// DRIVER EVENTS
// -----------------------------------------------------------------------------

namespace espurna {
namespace wifi {
namespace driver {

void on_scan_result(const ScanResult& info) {
    sta::scan::result(info);
}

void on_scan_done(bool ok, size_t count) {
    sta::scan::finished(ok, count);
}

void on_station_disconnected(const Mac&, int reason) {
    sta::connection::disconnected(reason);
}

void on_station_got_ip() {
    sta::connection::got_ip();
}

} // namespace driver
} // namespace wifi
} // namespace espurna

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

void wifiRegister(espurna::wifi::EventCallback callback) {
    espurna::wifi::internal::callbacks.push_front(callback);
}

bool wifiConnectable() {
    return espurna::wifi::ap::enabled();
}

bool wifiConnected() {
    return espurna::wifi::sta::connected();
}

espurna::wifi::StaNetwork wifiStaInfo() {
    if (espurna::wifi::sta::enabled()) {
        return espurna::wifi::sta::current();
    }

    return {};
}

espurna::Ip4 wifiStaIp() {
    if (espurna::wifi::sta::enabled()) {
        return espurna::wifi::sta::ip();
    }

    return {};
}

namespace {

espurna::string mac_upper(const espurna::wifi::Mac& mac) {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return espurna::string(buffer);
}

} // namespace

espurna::string wifiStaMac() {
    espurna::wifi::Mac mac{};
    ::espurna::core::chip::wifi_mac(false, mac.data());
    return mac_upper(mac);
}

espurna::string wifiStaBssid() {
    return wifiConnected()
        ? mac_upper(wifiStaInfo().bssid)
        : espurna::string();
}

const char* wifiStaSecurity() {
    return wifiConnected() ? espurna::wifi::driver::sta_security() : "";
}

int8_t wifiStaRssi() {
    return wifiConnected()
        ? wifiStaInfo().rssi
        : 0;
}

espurna::string wifiStaSsid() {
    if (espurna::wifi::sta::enabled()) {
        return espurna::wifi::sta::current().ssid;
    }

    return espurna::empty_string;
}

void wifiDisconnect() {
    espurna::wifi::sta::disconnect();
}

void wifiIpv6(bool enabled) {
    espurna::wifi::driver::ipv6(enabled);
}

void wifiToggleAp() {
    espurna::wifi::ap::toggle();
}

void wifiToggleSta() {
    espurna::wifi::sta::toggle();
}

void wifiStartAp() {
    espurna::wifi::ap::start();
}

bool wifiDisabled() {
    return espurna::wifi::opmode() == espurna::wifi::OpmodeNull;
}

void wifiDisable() {
    espurna::wifi::ap::fallback::timer.stop();
    espurna::wifi::sta::connection::stop();
    espurna::wifi::ensure_opmode(espurna::wifi::OpmodeNull);
}

void wifiTurnOff() {
    espurna::wifi::turn_off();
}

void wifiTurnOn() {
    espurna::wifi::turn_on(false);
}

void wifiApCheck() {
    espurna::wifi::ap::check();
}

size_t wifiApStations() {
    if (espurna::wifi::ap::enabled()) {
        return espurna::wifi::ap::stations();
    }

    return 0;
}

espurna::wifi::SoftApNetwork wifiApInfo() {
    if (espurna::wifi::ap::enabled()) {
        return espurna::wifi::ap::current();
    }

    return {};
}

espurna::Ip4 wifiApIp() {
    return espurna::wifi::ap::ip();
}

bool wifiApCaptive() {
    return espurna::wifi::ap::settings::captive();
}

bool wifiScanStart(WifiScanFound found, WifiScanError error) {
    namespace wifi = espurna::wifi;
    return wifi::sta::scan::start(
        [found](const wifi::driver::ScanResult& info) {
            found(wifi::convertSsid(info), info.rssi, info.authmode == wifi::driver::AuthOpen);
        },
        [error](const char* text) {
            if (text) {
                error(espurna::string(text));
            }
        });
}

bool wifiScanning() {
    return espurna::wifi::sta::scan::scanning();
}

void wifiSetup() {
    espurna::wifi::setup();
}
