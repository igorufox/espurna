/*

WIFI MODULE

Original code based on JustWifi, Wifi Manager for ESP8266 (GPLv3+)
Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

Modified for ESPurna
Copyright (C) 2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "espurna.h"

#include "espurna_wifi.h"
#include "wifi_driver.h"

#include <IPAddress.h>

#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/stats.h>
#if !NO_SYS
#include <lwip/tcpip.h>
#endif

#include <algorithm>
#include <array>
#include <list>
#include <queue>
#include <vector>

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
namespace compat {

#if defined(ESP8266)
[[gnu::unused, gnu::deprecated("WIFI_MODEM_SLEEP_{NONE, MODEM, LIGHT} should be used instead, see config/general.h")]]
constexpr sleep_type_t arduino_sleep(WiFiSleepType type) {
    return static_cast<sleep_type_t>(type);
}
#endif

[[gnu::unused]]
constexpr sleep_type_t arduino_sleep(sleep_type_t type) {
    return type;
}

} // namespace compat

constexpr float txPower() {
    return WIFI_OUTPUT_POWER_DBM;
}

constexpr sleep_type_t sleep() {
    return compat::arduino_sleep(WIFI_SLEEP_MODE);
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
wifi::BootMode convert(const String& value) {
    return convert<bool>(value)
        ? wifi::BootMode::Enabled
        : wifi::BootMode::Disabled;
}

String serialize(wifi::BootMode mode) {
    return serialize(mode == wifi::BootMode::Enabled);
}

template<>
wifi::StaMode convert(const String& value) {
    return convert<bool>(value)
        ? wifi::StaMode::Enabled
        : wifi::StaMode::Disabled;
}

String serialize(wifi::StaMode mode) {
    return serialize(mode == wifi::StaMode::Enabled);
}

template<>
wifi::ApMode convert(const String& value) {
    return convert(wifi::ap::settings::options::ApModeOptions, value, wifi::ApMode::Fallback);
}

String serialize(wifi::ApMode mode) {
    return serialize(wifi::ap::settings::options::ApModeOptions, mode);
}

template <>
sleep_type_t convert(const String& value) {
    return convert(wifi::settings::options::SleepTypeOptions, value, wifi::build::sleep());
}

String serialize(sleep_type_t sleep) {
    return serialize(wifi::settings::options::SleepTypeOptions, sleep);
}

template <>
IPAddress convert(const String& value) {
    IPAddress out;
    out.fromString(value);
    return out;
}

template <>
wifi::Mac convert(const String& value) {
    wifi::Mac out{};

    static constexpr size_t Min { 12 };
    static constexpr size_t Max { 17 };

    switch (value.length()) {
    // xxxxxxxxxx
    case Min:
        hexDecode(value.c_str(), value.length(), out.data(), out.size());
        break;

    // xx:xx:xx:xx:xx:xx
    case Max: {
        String buffer;
        buffer.reserve(value.length());

        for (auto it = value.begin(); it != value.end(); ++it) {
            if ((*it) != ':') {
                buffer += *it;
            }
        }
        if (buffer.length() == Min) {
            hexDecode(buffer.c_str(), buffer.length(), out.data(), out.size());
        }
        break;
    }

    }

    return out;
}

// XXX: "(IP unset)" when not set, no point saving these :/
// XXX: both 0.0.0.0 and 255.255.255.255 will be saved as empty string

String serialize(const IPAddress& ip) {
    return wifi::driver::ip_set(ip) ? ipToString(ip) : emptyString;
}

String serialize(wifi::Mac mac) {
    String out;
    out.reserve(18);

    bool delim { false };
    char buffer[3] = {0};
    for (auto& byte : mac) {
        hexEncode(&byte, 1, buffer, sizeof(buffer));
        if (delim) {
            out += ':';
        }
        out += buffer;
        delim = true;
    }

    return out;
}

} // namespace internal
} // namespace settings

namespace wifi {
namespace {

// Use SDK constants directly. Provide a constexpr version of the Core enum, since the code never
// actually uses `WiFi::mode(...)` directly, *but* opmode is retrieved using the SDK function.

static constexpr uint8_t OpmodeNull { driver::OpmodeNull };
static constexpr uint8_t OpmodeSta { driver::OpmodeSta };
static constexpr uint8_t OpmodeAp { driver::OpmodeAp };
static constexpr uint8_t OpmodeApSta { driver::OpmodeApSta };

enum class ScanError {
    None,
    AlreadyScanning,
    Busy,
    NoNetworks,
    System,
};

enum class Action {
    AccessPointFallback,
    AccessPointFallbackCheck,
    AccessPointStart,
    AccessPointStop,
    Boot,
    StationConnect,
    StationContinueConnect,
    StationDisconnect,
    StationTryConnectBetter,
    TurnOff,
    TurnOn,
};

using Actions = std::list<Action>;
using ActionsQueue = std::queue<Action, Actions>;

enum class State {
    Boot,
    Connect,
    TryConnectBetter,
    Connected,
    Idle,
    Init,
    Timeout,
    Fallback,
    WaitScan,
    WaitScanWithoutCurrent,
    WaitConnected
};

namespace internal {

// Module actions are controled in a serialzed manner, when internal loop is done with the
// current task and is free to take up another one. Allow to toggle OFF for the whole module,
// discarding any actions involving an active WiFi. Default is ON

bool enabled { false };
ActionsQueue actions;

State state { State::Boot };
State last_state { state };

} // namespace internal

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

void ensure_opmode(uint8_t mode) {
    const auto is_set = [&]() {
        return (opmode() == mode);
    };

    // `std::abort()` calls are the to ensure the mode actually changes, but it should be extremely rare
    // it may be also wise to add these for when the mode is already the expected one,
    // since we should enforce mode changes to happen *only* through the configuration loop

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

bool enabled() {
    return internal::enabled;
}

void enable() {
    internal::enabled = true;
}

void disable() {
    internal::enabled = false;
}

void action(Action value) {
    switch (value) {
    case Action::StationConnect:
    case Action::StationTryConnectBetter:
    case Action::StationContinueConnect:
    case Action::StationDisconnect:
    case Action::AccessPointFallback:
    case Action::AccessPointFallbackCheck:
    case Action::AccessPointStart:
    case Action::AccessPointStop:
        if (!enabled()) {
            return;
        }
        break;
    case Action::TurnOff:
    case Action::TurnOn:
    case Action::Boot:
        break;
    }

    internal::actions.push(value);
}

template <typename T>
State handle_action(State state, T&& handler) {
    if (!internal::actions.empty()) {
        state = handler(state, internal::actions.front());
        internal::actions.pop();
    }

    return state;
}

namespace debug {

String error(ScanError error) {
    StringView out;

    switch (error) {
    case ScanError::None:
        out = STRING_VIEW("OK");
        break;
    case ScanError::AlreadyScanning:
        out = STRING_VIEW("Scan already in progress");
        break;
    case ScanError::Busy:
        out = STRING_VIEW("State machine is busy");
        break;
    case ScanError::NoNetworks:
        out = STRING_VIEW("No networks");
        break;
    case ScanError::System:
        out = STRING_VIEW("System unable to start the scan");
        break;
    }

    return out.toString();
}

String mac(Mac mac) {
    return espurna::settings::internal::serialize(mac);
}

String authmode(AUTH_MODE mode) {
    return driver::authmode(mode);
}

String opmode(uint8_t mode) {
    StringView out;

    switch (mode) {
    case OpmodeApSta:
        out = STRING_VIEW("AP+STA");
        break;
    case OpmodeSta:
        out = STRING_VIEW("STA");
        break;
    case OpmodeAp:
        out = STRING_VIEW("AP");
        break;
    case OpmodeNull:
        out = STRING_VIEW("NULL");
        break;
    }

    return out.toString();
}

String sleep_type(sleep_type_t type) {
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
String NAME () {\
    return espurna::settings::internal::serialize(FUNC());\
}

#define ID_VALUE(NAME, FUNC)\
String NAME (size_t id) {\
    return espurna::settings::internal::serialize(FUNC(id));\
}

EXACT_VALUE(sleep, settings::sleep)
EXACT_VALUE(txPower, settings::txPower)
EXACT_VALUE(bootMode, settings::bootMode)

} // namespace internal
} // namespace query
} // namespace settings

// Scan results are byte arrays, not C strings (ref. driver::ScanResult)

String convertSsid(const driver::ScanResult& info) {
    String ssid;
    ssid.concat(reinterpret_cast<const char*>(info.ssid), info.ssid_len);
    return ssid;
}

template <typename T, size_t MacSize = sizeof(T::bssid)>
Mac convertBssid(const T& info) {
    static_assert(MacSize == 6, "");
    Mac mac;
    std::copy(info.bssid, info.bssid + MacSize, mac.begin());
    return mac;
}

struct Info {
    Info() = default;
    Info(const Info&) = default;
    Info(Info&&) = default;

    Info(Mac&& bssid, AUTH_MODE authmode, int8_t rssi, uint8_t channel) :
        _bssid(std::move(bssid)),
        _authmode(authmode),
        _rssi(rssi),
        _channel(channel)
    {}

    explicit Info(const driver::ScanResult& info) :
        _bssid(convertBssid(info)),
        _authmode(info.authmode),
        _rssi(info.rssi),
        _channel(info.channel)
    {}

    Info& operator=(const Info&) = default;
    Info& operator=(Info&&) = default;

    Info& operator=(const driver::ScanResult& info) {
        _bssid = convertBssid(info);
        _authmode = info.authmode;
        _channel = info.channel;
        _rssi = info.rssi;
        return *this;
    }

    explicit operator bool() const {
        return _rssi != 0 && _channel != 0;
    }

    bool operator<(const Info& rhs) const {
        return _rssi < rhs._rssi;
    }

    bool operator>(const Info& rhs) const {
        return _rssi > rhs._rssi;
    }

    const Mac& bssid() const {
        return _bssid;
    }

    AUTH_MODE authmode() const {
        return _authmode;
    }

    int8_t rssi() const {
        return _rssi;
    }

    uint8_t channel() const {
        return _channel;
    }

private:
    Mac _bssid{};
    AUTH_MODE _authmode { driver::AuthOpen };
    int8_t _rssi { 0 };
    uint8_t _channel { 0u };
};

struct SsidInfo {
    SsidInfo() = delete;

    explicit SsidInfo(const driver::ScanResult& info) :
        _ssid(convertSsid(info)),
        _info(info)
    {}

    SsidInfo(String&& ssid, Info&& info) :
        _ssid(std::move(ssid)),
        _info(std::move(info))
    {}

    const String& ssid() const {
        return _ssid;
    }

    const Info& info() const {
        return _info;
    }

    // decreasing order by rssi (default sort() order is increasing)
    bool operator<(const SsidInfo& rhs) const {
        if (!_info.rssi()) {
            return false;
        }

        return info() > rhs.info();
    }

private:
    String _ssid;
    Info _info;
};

using SsidInfos = std::forward_list<SsidInfo>;

// Note that lwip config allows up to 3 DNS servers. But, most of the time we use DHCP.
// TODO: ::dns(size_t index)? how'd that look with settings?

struct IpSettings {
    IpSettings() = default;
    IpSettings(const IpSettings&) = default;
    IpSettings(IpSettings&&) = default;

    IpSettings& operator=(const IpSettings&) = default;
    IpSettings& operator=(IpSettings&&) = default;

    template <typename Ip, typename Netmask, typename Gateway, typename Dns>
    IpSettings(Ip&& ip, Netmask&& netmask, Gateway&& gateway, Dns&& dns) :
        _ip(std::forward<Ip>(ip)),
        _netmask(std::forward<Netmask>(netmask)),
        _gateway(std::forward<Gateway>(gateway)),
        _dns(std::forward<Dns>(dns))
    {}

    const IPAddress& ip() const {
        return _ip;
    }

    const IPAddress& netmask() const {
        return _netmask;
    }

    const IPAddress& gateway() const {
        return _gateway;
    }

    const IPAddress& dns() const {
        return _dns;
    }

    explicit operator bool() const {
        return driver::ip_set(_ip)
            && driver::ip_set(_netmask)
            && driver::ip_set(_gateway);
    }

private:
    IPAddress _ip;
    IPAddress _netmask;
    IPAddress _gateway;
    IPAddress _dns;
};

struct Network {
    Network() = delete;
    Network(const Network&) = default;
    Network(Network&&) = default;

    Network& operator=(Network&&) = default;

    explicit Network(String&& ssid) :
        _ssid(std::move(ssid))
    {}

    Network(String&& ssid, String&& passphrase) :
        _ssid(std::move(ssid)),
        _passphrase(std::move(passphrase))
    {}

    Network(String&& ssid, String&& passphrase, IpSettings&& settings) :
        _ssid(std::move(ssid)),
        _passphrase(std::move(passphrase)),
        _ipSettings(std::move(settings))
    {}

    // TODO(?): in case SDK API is used directly, this also could use an authmode field
    // Arduino wrapper sets WPAPSK minimum by default, so one use-case is to set it to WPA2PSK

    Network(Network other, Mac bssid, uint8_t channel) :
        _ssid(std::move(other._ssid)),
        _passphrase(std::move(other._passphrase)),
        _ipSettings(std::move(other._ipSettings)),
        _bssid(bssid),
        _channel(channel)
    {}

    bool dhcp() const {
        return !_ipSettings;
    }

    const String& ssid() const {
        return _ssid;
    }

    const String& passphrase() const {
        return _passphrase;
    }

    const IpSettings& ipSettings() const {
        return _ipSettings;
    }

    const Mac& bssid() const {
        return _bssid;
    }

    uint8_t channel() const {
        return _channel;
    }

private:
    String _ssid;
    String _passphrase;
    IpSettings _ipSettings;

    Mac _bssid {};
    uint8_t _channel { 0u };
};

using Networks = std::list<Network>;

// -----------------------------------------------------------------------------
// STATION
// -----------------------------------------------------------------------------

namespace sta {
namespace build {

static constexpr size_t NetworksMax { WIFI_MAX_NETWORKS };

// aka short interval
static constexpr auto ConnectionInterval = duration::Milliseconds{ WIFI_CONNECT_INTERVAL };

// aka long interval
static constexpr auto ReconnectionInterval = duration::Milliseconds{ WIFI_RECONNECT_INTERVAL };

static constexpr int ConnectionRetries { WIFI_CONNECT_RETRIES };
static constexpr auto RecoveryInterval = ConnectionInterval * ConnectionRetries;

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
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_SSID,
        WIFI2_SSID,
        WIFI3_SSID,
        WIFI4_SSID,
        WIFI5_SSID
    );
}

StringView passphrase(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_PASS,
        WIFI2_PASS,
        WIFI3_PASS,
        WIFI4_PASS,
        WIFI5_PASS
    );
}

StringView ip(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_IP,
        WIFI2_IP,
        WIFI3_IP,
        WIFI4_IP,
        WIFI5_IP
    );
}

StringView gateway(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_GW,
        WIFI2_GW,
        WIFI3_GW,
        WIFI4_GW,
        WIFI5_GW
    );
}

StringView netmask(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_MASK,
        WIFI2_MASK,
        WIFI3_MASK,
        WIFI4_MASK,
        WIFI5_MASK
    );
}

StringView dns(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_DNS,
        WIFI2_DNS,
        WIFI3_DNS,
        WIFI4_DNS,
        WIFI5_DNS
    );
}

StringView bssid(size_t index) {
    return WIFI_SETTING_STRING_RESULT(
        WIFI1_BSSID,
        WIFI2_BSSID,
        WIFI3_BSSID,
        WIFI4_BSSID,
        WIFI5_BSSID
    );
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

PROGMEM_STRING(Bssid, "bssid");
PROGMEM_STRING(Channel, "chan");

} // namespace keys

String from_string(espurna::settings::Key key, StringView defaultValue) {
    return getSetting(key, defaultValue);
}

IPAddress from_ipaddress(espurna::settings::Key key, StringView defaultValue) {
    return espurna::settings::internal::convert<IPAddress>(
        getSetting(key, defaultValue));
}

StaMode mode() {
    return getSetting(keys::Mode, build::mode());
}

String ssid(size_t index) {
    return from_string({keys::Ssid, index}, build::ssid(index));
}

String passphrase(size_t index) {
    return from_string({keys::Passphrase, index}, build::passphrase(index));
}

IPAddress ip(size_t index) {
    return from_ipaddress({keys::Ip, index}, build::ip(index));
}

IPAddress gateway(size_t index) {
    return from_ipaddress({keys::Gateway, index}, build::gateway(index));
}

IPAddress netmask(size_t index) {
    return from_ipaddress({keys::Netmask, index}, build::netmask(index));
}

IPAddress dns(size_t index) {
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

IPAddress ip() {
    return driver::sta_ip();
}

uint8_t channel() {
    return driver::sta_channel();
}

int8_t rssi() {
    return driver::sta_rssi();
}

Networks networks() {
    Networks out;

    for (size_t id = 0; id < build::NetworksMax; ++id) {
        auto ssid = settings::ssid(id);
        if (!ssid.length()) {
            break;
        }

        auto pass = settings::passphrase(id);

        auto ip = settings::ip(id);
        auto ipSettings = driver::ip_set(ip)
            ? IpSettings{
                std::move(ip),
                settings::netmask(id),
                settings::gateway(id),
                settings::dns(id)}
            : IpSettings{};

        Network network(std::move(ssid), settings::passphrase(id), std::move(ipSettings));
        auto channel = settings::channel(id);
        if (channel) {
            out.emplace_back(std::move(network), settings::bssid(id), channel);
        } else {
            out.push_back(std::move(network));
        }
    }

    return out;
}

size_t countNetworks() {
    size_t networks { 0 };

    for (size_t id = 0; id < build::NetworksMax; ++id) {
        auto ssid = settings::ssid(id);
        if (!ssid.length()) {
            break;
        }

        ++networks;
    }

    return networks;
}

// Note that authmode field is a our threshold, not the one selected by an AP

Info info() {
    Mac bssid{};
    AUTH_MODE authmode{};
    driver::sta_info(bssid, authmode);

    return Info{
        std::move(bssid),
        authmode,
        rssi(),
        channel()};
}

StaNetwork current() {
    return driver::sta_current();
}

// --- lwIP, the same on every chip ----------------------------------------------------------------------------------
//
// lwIP is called in its own context: directly without an OS (the ESP8266 NONOS SDK, NO_SYS), else through the tcpip
// thread (the loop would race with it). A full tcpip mailbox drops the call: the next one comes on the next tick
namespace lwip {

void call(void (*func)(void*)) {
#if NO_SYS
    func(nullptr);
#else
    tcpip_try_callback(func, nullptr);
#endif
}

// gratuitous ARP on every interface that is up with an address (the ESP8266 code of upstream)
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
void probe(void*) {
    auto* netif = netif_default;
    if (netif && netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_gw(netif))) {
        etharp_request(netif, netif_ip4_gw(netif));
    }
}

} // namespace lwip

// The Wi-Fi link can stay associated while the IP stack receives nothing at all: 10.0.5.116 (Beken) 2026-11-07, first
// boot after the Matter code was written by an install, DHCP done, then no ARP / ping answers for 28 min. The loop kept
// running and feeding the watchdog, so nothing reset it; a deauth from the AP did. Every Interval, while connected with
// an address, whatever arrived (ARP + IPv4 + IPv6 packets) is compared with the last check. Nothing new => an ARP
// request goes to the gateway (answers within ms, so a healthy stack always sees traffic by the next check); Misses
// checks in a row without a single received packet => restart. The same on every chip; it needs lwIP's receive
// counters (LWIP_STATS): an lwIP built without them has no netwatch
namespace netwatch {

#if LWIP_STATS && (ETHARP_STATS || IP_STATS || IP6_STATS)
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

bool addressed() {
    const auto* netif = netif_default;
    return netif && netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif));
}

void check();

void setup() {
    last_rx = received();
    timer.repeat(Interval, check);
}
#else
void setup() {
}
#endif

} // namespace netwatch

#if WIFI_GRATUITOUS_ARP_SUPPORT
namespace garp {
namespace build {

static constexpr auto IntervalMin = duration::Milliseconds{ WIFI_GRATUITOUS_ARP_INTERVAL_MIN };
static constexpr auto IntervalMax = duration::Milliseconds{ WIFI_GRATUITOUS_ARP_INTERVAL_MAX };

} // namespace build

namespace settings {
namespace internal {

template <typename T>
T randomInterval(T minimum, T maximum) {
    return T(::randomNumber(minimum.count(), maximum.count()));
}

duration::Milliseconds randomInterval() {
    return randomInterval(build::IntervalMin, build::IntervalMax);
}

} // namespace internal

duration::Milliseconds interval() {
    static const auto defaultInterval = internal::randomInterval();
    return getSetting("wifiGarpIntvl", defaultInterval);
}

} // namespace settings

namespace internal {

timer::SystemTimer timer;
bool wait { false };

} // namespace internal

bool send() {
    lwip::call(lwip::gratuitous);
    return true;
}

bool wait() {
    if (internal::wait) {
        return true;
    }

    internal::wait = true;
    return false;
}

void stop() {
    internal::timer.stop();
}

void reset() {
    internal::wait = false;
}

void start(duration::Milliseconds next) {
    internal::timer.repeat(next, reset);
}

} // namespace garp
#endif

namespace scan {
namespace settings {
namespace keys {

PROGMEM_STRING(Enabled, "wifiScan");

} // namespace keys
} // namespace settings

using SsidInfosPtr = std::shared_ptr<SsidInfos>;

using Success = std::function<void(const driver::ScanResult*)>;
using Error = std::function<void(ScanError)>;

struct Task {
    Task() = delete;

    Task(Success&& success, Error&& error) :
        _success(std::move(success)),
        _error(std::move(error))
    {}

    void success(const driver::ScanResult* info) {
        _success(info);
    }

    void error(ScanError error) {
        _error(error);
    }

private:
    Success _success;
    Error _error;
};

using TaskPtr = std::unique_ptr<Task>;

namespace internal {

bool flag { false };
TaskPtr task;

void stop() {
    flag = false;
    task = nullptr;
}

// Driver reports every network found, then the end of the scan (ref. driver::on_scan_result & on_scan_done)

void result(const driver::ScanResult& info) {
    if (task) {
        task->success(&info);
    }
}

void done(bool ok, size_t networks) {
    if (!task) {
        stop();
        return;
    }

    if (!ok) {
        task->error(ScanError::System);
        stop();
        return;
    }

    if (!networks) {
        task->error(ScanError::NoNetworks);
    }

    stop();
}

} // namespace internal

bool start(Success&& success, Error&& error) {
    if (internal::flag) {
        error(ScanError::Busy);
        return false;
    }

    if (internal::task) {
        error(ScanError::AlreadyScanning);
        return false;
    }

    // scan parameters are the chip's, ref. driver::scan_start()
    if (driver::scan_start()) {
        internal::task = std::make_unique<Task>(std::move(success), std::move(error));
        internal::flag = true;
        return true;
    }

    error(ScanError::System);
    return false;
}

// Alternative to the stock WiFi method, where we wait for the task to finish before returning
bool wait(Success&& success, Error&& error) {
    auto result = start(std::move(success), std::move(error));
    while (internal::task) {
        time::delay(duration::Milliseconds(100));
        driver::loop();
    }

    return result;
}

// Another alternative to the stock WiFi method, return a shared Info list
// Caller is expected to wait for the scan to complete before using the contents
SsidInfosPtr ssidinfos() {
    auto infos = std::make_shared<SsidInfos>();

    start(
        [infos](const driver::ScanResult* found) {
            infos->emplace_front(*found);
        },
        [infos](ScanError) {
            infos->clear();
        });

    return infos;
}

} // namespace scan

bool enabled() {
    return wifi::opmode() & wifi::OpmodeSta;
}

// XXX: WiFi.disconnect() also implicitly disables STA mode *and* erases the current STA config

void disconnect() {
    if (enabled()) {
        driver::sta_disconnect();
    }
}

// Some workarounds for built-in WiFi management:
// - don't *intentionally* perist current SSID & PASS even when persistance is disabled from the Arduino Core side.
// while this seems like a good idea in theory, we end up with a bunch of async actions coming our way.
// - station disconnect events are linked with the connection routine as well, single WiFi::begin() may trigger up to
// 3 events (as observed with `WiFi::waitForConnectResult()`) before the connection loop stops further attempts
// - explicit OPMODE changes to both notify the userspace when the change actually happens (alternative is SDK event, but it is SYS context),
// since *all* STA & AP start-up methods will implicitly change the mode (`WiFi.begin()`, `WiFi.softAP()`, `WiFi.config()`)

void enable() {
    ensure_opmode(opmode() | OpmodeSta);
    driver::sta_prepare();
}

void disable() {
    ensure_opmode(opmode() & ~OpmodeSta);
}

namespace connection {
namespace internal {

struct Task {
    using Iterator = Networks::iterator;

    Task() = delete;
    Task(const Task&) = delete;
    Task(Task&&) = delete;

    Task(String hostname, Networks networks, int retries) :
        _hostname(std::move(hostname)),
        _networks(std::move(networks)),
        _begin(_networks.begin()),
        _end(_networks.end()),
        _current(_begin),
        _retries(retries),
        _retry(_retries)
    {}

    bool empty() const {
        return _networks.empty();
    }

    size_t count() const {
        return _networks.size();
    }

    bool done() const {
        return _current == _end;
    }

    bool next() {
        if (!done()) {
            if (--_retry < 0) {
                _retry = _retries;
                _current = std::next(_current);
            }
            return !done();
        }

        return false;
    }

    bool connect() const {
        if (!done() && sta::enabled()) {
            // Need to call this to cancel SDK tasks (previous scan, connection, etc.)
            // Otherwise, it will fail the initial attempt and force a retry.
            sta::disconnect();

            // SDK sends EVENT_STAMODE_DISCONNECTED right after the disconnect() call, which is likely to happen
            // after being connected and disconnecting for the first time. Not doing this will cause the connection loop
            // to cancel the `wait` lock too early, forcing the Timeout state despite the EVENT_STAMODE_GOTIP coming in later.
            // Allow the event to come in right now to allow `wifi_station_connect()` down below trigger a real one.
            yield();

            auto& network = *_current;
            auto& ipsettings = network.ipSettings();

            // what the SDK station config can hold, same limits on every chip
            const auto& ssid = network.ssid();
            if (!ssid.length() || (ssid.length() > driver::SsidMax)) {
                return false;
            }

            const auto& passphrase = network.passphrase();
            if (passphrase.length()
                && ((passphrase.length() < driver::PassphraseMin)
                 || (passphrase.length() > driver::PassphraseMax)))
            {
                return false;
            }

            return driver::sta_connect(driver::StaConnect{
                .hostname = _hostname,
                .ssid = network.ssid(),
                .passphrase = network.passphrase(),
                .dhcp = network.dhcp(),
                .ip = ipsettings.ip(),
                .netmask = ipsettings.netmask(),
                .gateway = ipsettings.gateway(),
                .dns = ipsettings.dns(),
                .bssid = network.bssid(),
                .channel = network.channel(),
            });
        }

        return false;
    }

    Networks& networks() {
        return _networks;
    }

private:
    String _hostname;

    Networks _networks;
    Iterator _begin;
    Iterator _end;
    Iterator _current;

    const int _retries;
    int _retry;
};

using ActionPtr = void(*)();

void action_next() {
    action(Action::StationContinueConnect);
}

void action_new() {
    action(Action::StationConnect);
}

sta::scan::SsidInfosPtr scanResults;
Networks preparedNetworks;

bool connected { false };
bool wait { false };
time::SystemClock::time_point attempt_at{};

// the AP that dropped us (deauth / disassoc from its side, e.g. a usteer kick), not a lost signal or our own leave.
// The SDK cannot answer 802.11v requests, so a steering AP kicks the station, and the next scan most likely finds
// the same AP the strongest again: try it last for a while, so the station goes to another one instead of a kick loop
static constexpr auto KickedTime = duration::Minutes{ WIFI_KICKED_TIME };
Mac kicked{};
time::SystemClock::time_point kicked_at;

timer::SystemTimer timer;
bool persist { false };

using TaskPtr = std::unique_ptr<Task>;
TaskPtr task;

} // namespace internal

void persist(bool value) {
    internal::persist = value;
}

bool persist() {
    return internal::persist;
}

void stop() {
    scan::internal::flag = false;
    internal::scanResults = nullptr;
    internal::preparedNetworks.clear();
    internal::timer.stop();
    internal::task.reset();
}

bool start(String&& hostname) {
    if (!internal::task) {
        internal::task = std::make_unique<internal::Task>(
            std::move(hostname),
            std::move(internal::preparedNetworks),
            build::ConnectionRetries);
        internal::timer.stop();
        return true;
    }

    internal::preparedNetworks.clear();
    return false;
}

void schedule(duration::Milliseconds next, internal::ActionPtr ptr) {
    internal::timer.once(next, ptr);
    DEBUG_MSG_P(PSTR("[WIFI] Next connection attempt in %u (ms)\n"), next.count());
}

void schedule_next() {
    schedule(build::ConnectionInterval, internal::action_next);
}

void schedule_new(duration::Milliseconds next) {
    schedule(next, internal::action_new);
}

void schedule_new() {
    schedule_new(build::ReconnectionInterval);
}

bool next() {
    return internal::task->next();
}

// the SDK does not always report the end of an attempt (Beken, ESP32 seen; the ESP8266 SDK may stall too): the
// attempt ends here after this long, on every chip
constexpr auto AttemptTimeout = duration::Milliseconds{ 15000 };

bool connect() {
    scan::internal::flag = true;
    if (internal::task->connect()) {
        internal::wait = true;
        internal::attempt_at = time::SystemClock::now();
        return true;
    }

    scan::internal::flag = false;

    return false;
}

// Note that `wifi_station_get_connect_status()` may never actually change the state from CONNECTING when AP is not available.
// Wait for the WiFi stack event instead (handled on setup with a static object) and continue after it is either connected or disconnected

bool wait() {
    if (internal::wait && ((time::SystemClock::now() - internal::attempt_at) >= AttemptTimeout)) {
        DEBUG_MSG_P(PSTR("[WIFI] Connection attempt timed out\n"));
        driver::sta_disconnect();
        internal::wait = false;
    }

    return internal::wait;
}

// TODO(Core 2.7.4): `WiFi.isConnected()` is a simple `wifi_station_get_connect_status() == STATION_GOT_IP`,
// Meaning, it will never detect link up / down updates when AP silently kills the connection or something else unexpected happens.
// Running JustWiFi with autoconnect + reconnect enabled, it silently avoided the issue b/c the SDK reconnect routine disconnected the STA,
// causing our state machine to immediately cancel it (since `WL_CONNECTED != WiFi.status()`) and then try to connect again using it's own loop.
// We could either (* is used currently):
// - (*) listen for the SDK event through the `WiFi.onStationModeDisconnected()`
// - ( ) poll NETIF_FLAG_LINK_UP for the lwip's netif, since the SDK will bring the link down on disconnection
//   find the `interface` in the `netif_list`, where `interface->num == STATION_IF`
// - ( ) use lwip's netif event system from the recent Core, track UP and DOWN for a specific interface number
//   this one is probably only used internally, thus should be treated as a private API
// - ( ) poll whether `wifi_get_ip_info(STATION_IF, &ip);` is set to something valid
//   (tuple of ip, gw and mask)
// - ( ) poll `WiFi.localIP().isSet()`
//   (will be unset when the link is down)

// placing status into a simple bool to avoid extracting ip info every time someone needs to check the connection

bool connected() {
    return internal::connected;
}

bool connecting() {
    return static_cast<bool>(internal::task);
}

bool lost() {
    static bool last { internal::connected };

    if (internal::connected != last) {
        last = internal::connected;
        return !last;
    }

    return false;
}

void prepare(Networks&& networks) {
    std::swap(internal::preparedNetworks, networks);
}

bool prepared() {
    return internal::preparedNetworks.size() > 0;
}

} // namespace connection

bool connected() {
    return connection::connected();
}

bool connecting() {
    return connection::connecting();
}

#if LWIP_STATS && (ETHARP_STATS || IP_STATS || IP6_STATS)
void netwatch::check() {
    const auto now = received();
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
#endif

bool scanning() {
    return static_cast<bool>(scan::internal::task);
}

// 802.11 reason codes, same values with every SDK
static constexpr int ReasonUnspecified { 1 };
static constexpr int ReasonAssocTooMany { 5 };

// note that pass a simple request for state change, not the reason behind it
// TODO 'lock' / 'trigger' object instead of multiple flags
void disconnected(const Mac& bssid, int reason) {
    // only an intentional drop by the AP counts. usteer kicks with 'AP busy' (its default load_kick_reason_code 5),
    // manual hostapd del_client is 'unspecified'. AP restart (not authed / not assoced / leaving), inactivity and
    // handshake failures say nothing about the AP itself
    if (connection::internal::connected
        && ((reason == ReasonAssocTooMany)
         || (reason == ReasonUnspecified)))
    {
        connection::internal::kicked = bssid;
        connection::internal::kicked_at = time::SystemClock::now();
        DEBUG_MSG_P(PSTR("[WIFI] Dropped by %s (reason %u), tried last for %u min\n"),
            debug::mac(connection::internal::kicked).c_str(), static_cast<unsigned>(reason),
            static_cast<unsigned>(connection::internal::KickedTime.count()));
    }
    connection::internal::wait = false;
    connection::internal::connected = false;
}

void got_ip() {
    connection::internal::wait = false;
    connection::internal::connected = true;
}

void init() {
    // event handlers, ref. driver::on_station_disconnected & on_station_got_ip
    disconnect();
    disable();
    yield();
}

void toggle() {
    auto current = enabled();
    connection::persist(!current);
    action(current
        ? Action::StationDisconnect
        : Action::StationConnect);
}

namespace scan {
namespace build {

constexpr bool enabled() {
    return 1 == WIFI_SCAN_NETWORKS;
}

} // namespace build

namespace settings {

bool enabled() {
    return getSetting(keys::Enabled, build::enabled());
}

namespace query {

EXACT_VALUE(enabled, settings::enabled)

} // namespace query
} // namespace settings

namespace periodic {
namespace build {

static constexpr auto Interval = duration::Milliseconds{ WIFI_SCAN_RSSI_CHECK_INTERVAL };
static constexpr auto Checks = int8_t{ WIFI_SCAN_RSSI_CHECKS };

constexpr int8_t threshold() {
    return WIFI_SCAN_RSSI_THRESHOLD;
}

constexpr int8_t delta() {
    return WIFI_ROAM_RSSI_DELTA;
}

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Threshold, "wifiScanRssi");
PROGMEM_STRING(Delta, "wifiRoamDelta");

} // namespace keys

int8_t threshold() {
    return getSetting(FPSTR(keys::Threshold), build::threshold());
}

int8_t delta() {
    return std::clamp(
        getSetting(FPSTR(keys::Delta), build::delta()),
        int8_t{ 1 }, int8_t{ 40 });
}

namespace query {

EXACT_VALUE(threshold, settings::threshold)
EXACT_VALUE(delta, settings::delta)

} // namespace query
} // namespace settings

namespace internal {

int8_t threshold { build::threshold() };
int8_t counter { build::Checks };
timer::SystemTimer timer;

// every scan that found nothing better makes the next one come later, up to MaxStays times the usual
// (3, 6, ... 15 min with the defaults). weak link with no better AP around should not keep scanning
uint8_t stays { 0 };
constexpr uint8_t MaxStays { 4 };

int8_t checks() {
    return build::Checks * (1 + stays);
}

void task() {
    if (!sta::connected()) {
        counter = checks();
        return;
    }

    auto rssi = sta::rssi();
    if (rssi > threshold) {
        counter = checks();
    } else if (rssi < threshold) {
        if (counter < 0) {
            return;
        }

        if (!--counter) {
            action(Action::StationTryConnectBetter);
        }
    }
}

void start() {
    counter = checks();
    timer.repeat(build::Interval, task);
}

void stop() {
    counter = checks();
    timer.stop();
}

} // namespace internal

void threshold(int8_t value) {
    internal::threshold = value;
}

void stop() {
    internal::stop();
}

// new connection
void start() {
    internal::stays = 0;
    internal::start();
}

// still on the same AP after looking for a better one
void stayed() {
    if (internal::stays < internal::MaxStays) {
        ++internal::stays;
    }

    internal::start();
}

bool check() {
    if (internal::counter <= 0) {
        internal::counter = internal::checks();
        return true;
    }

    return false;
}

} // namespace periodic
} // namespace scan

namespace connection {

// After scan attempt, generate a new networks list based on the results sorted by the rssi value.
// For the initial connection, add every matching network with the scan result bssid and channel info.
// For the attempt to find a better network, filter out every network with worse than the current network's rssi

void scanNetworks() {
    internal::scanResults = sta::scan::ssidinfos();
}

bool suitableNetwork(const Network& network, const SsidInfo& ssidInfo) {
    return (ssidInfo.ssid() == network.ssid())
        && ((ssidInfo.info().authmode() != driver::AuthOpen)
                ? network.passphrase().length()
                : !network.passphrase().length());
}

bool kicked(const Mac& bssid) {
    return (internal::KickedTime.count() > 0)
        && (bssid == internal::kicked)
        && (time::SystemClock::now() - internal::kicked_at < internal::KickedTime);
}

bool scanProcessResults(int8_t threshold) {
    if (internal::scanResults) {
        decltype(internal::scanResults) results;
        std::swap(results, internal::scanResults);
        results->sort();

        if (threshold < 0) {
            results->remove_if(
                [threshold](const SsidInfo& result) {
                    return result.info().rssi() < threshold;
                });
        }

        decltype(internal::preparedNetworks) networks;
        std::swap(networks, internal::preparedNetworks);

        // strongest first. The AP that kicked us goes behind the other usable ones, but not behind the ones weaker than
        // the roaming threshold (wifiScanRssi): a kick (or, on Beken, any drop by the AP: its SDK gives no reason) must not
        // send the station from a -30 dBm AP to a -84 dBm one (10.0.5.139, 2026-11-09), it tries the kicker first then
        const auto usable = scan::periodic::settings::threshold();

        decltype(internal::preparedNetworks) sortedNetworks;
        decltype(internal::preparedNetworks) kickedNetworks;
        decltype(internal::preparedNetworks) weakNetworks;

        for (auto& result : *results) {
            for (auto& network : networks) {
                if (suitableNetwork(network, result)) {
                    const auto& info = result.info();
                    auto& out = kicked(info.bssid())
                        ? kickedNetworks
                        : (info.rssi() < usable)
                            ? weakNetworks
                            : sortedNetworks;
                    out.emplace_back(network, info.bssid(), info.channel());
                    break;
                }
            }
        }

        for (auto* list : {&kickedNetworks, &weakNetworks}) {
            for (auto& network : *list) {
                sortedNetworks.push_back(std::move(network));
            }
        }

        std::swap(sortedNetworks, internal::preparedNetworks);
        internal::scanResults.reset();
    }

    return internal::preparedNetworks.size() > 0;
}

// Looking for a better network while connected. Only another AP at least wifiRoamDelta stronger
// is worth the reconnection, current one (or a similarly weak one) would make us reconnect on every check.
// Neither is the AP that recently kicked us, it would most likely do that again
bool scanProcessResults(const Info& info) {
    if (internal::scanResults) {
        internal::scanResults->remove_if(
            [&](const SsidInfo& result) {
                return (result.info().bssid() == info.bssid())
                    || kicked(result.info().bssid());
            });
    }

    const auto threshold = std::min(
        info.rssi() + scan::periodic::settings::delta(), -1);
    if (scanProcessResults(static_cast<int8_t>(threshold))) {
        return true;
    }

    DEBUG_MSG_P(PSTR("[WIFI] No other AP with RSSI %d or better (current %d), staying\n"),
        threshold, info.rssi());
    return false;
}

bool scanProcessResults() {
    return scanProcessResults(0);
}

} // namespace connection

void configure() {
    auto enabled = (StaMode::Enabled == sta::settings::mode());
    connection::persist(enabled);
    action(enabled
        ? Action::StationConnect
        : Action::StationDisconnect);

    scan::periodic::threshold(
        scan::periodic::settings::threshold());

#if WIFI_GRATUITOUS_ARP_SUPPORT
    auto interval = garp::settings::interval();
    if (interval.count()) {
        garp::start(interval);
    } else {
        garp::stop();
    }
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

constexpr bool hasSsid() {
    return ssid().length() > 0;
}

PROGMEM_STRING(ApPass, WIFI_AP_PASS);

constexpr StringView passphrase() {
    return ApPass;
}

constexpr bool hasPassphrase() {
    return passphrase().length() > 0;
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

} // namespace build

namespace settings {
namespace keys {

PROGMEM_STRING(Mode, "wifiApMode");

PROGMEM_STRING(Ssid, "wifiApSsid");
PROGMEM_STRING(Passphrase, "wifiApPass");

PROGMEM_STRING(Channel, "wifiApChan");

[[gnu::unused]] PROGMEM_STRING(Captive, "wifiApCaptive");

} // namespace keys

ApMode mode() {
    return getSetting(FPSTR(keys::Mode), build::mode());
}

String defaultSsid() {
    return String(systemIdentifier());
}

String ssid() {
    return getSetting(FPSTR(keys::Ssid), build::hasSsid()
        ? build::ssid()
        : StringView(systemHostname()));
}

String passphrase() {
    return getSetting(FPSTR(keys::Passphrase), build::hasPassphrase()
        ? build::passphrase()
        : StringView(systemPassword()));
}

int8_t channel() {
    return getSetting(FPSTR(keys::Channel), build::channel());
}

[[gnu::unused]]
bool captive() {
    return getSetting(FPSTR(keys::Captive), build::captive());
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

// when the AP was started, ref. fallback::check()
time::SystemClock::time_point started_at{};

void start(String&& defaultSsid, String&& ssid, String&& passphrase, uint8_t channel) {
    // Always generate valid AP config, even when user-provided credentials fail to comply with the requirements
    // TODO: configuration routine depends on a lwip dhcpserver, which is a custom module made specifically for the ESP.
    // while it's possible to hijack this and control the process manually, right now it's easier to delegate this to the Core helpers
    // (plus, it makes it not compatible with the esp-idf stack anyway, since wifi_softap_dhcps_... calls don't do anything here)

    const char* apSsid {
        (ssid.length() && (ssid.length() < build::SsidMax))
        ? ssid.c_str() : defaultSsid.c_str() };

    const char* apPass {
        (passphrase.length() \
         && (passphrase.length() >= build::PassphraseMin) \
         && (passphrase.length() < build::PassphraseMax))
        ? passphrase.c_str() : nullptr };

    driver::ap_start(apSsid, apPass, channel);
    started_at = time::SystemClock::now();
}

} // namespace internal

IPAddress ip() {
    return driver::ap_ip();
}

void enable() {
    ensure_opmode(opmode() | OpmodeAp);
}

void disable() {
    ensure_opmode(opmode() & ~OpmodeAp);
}

bool enabled() {
    return opmode() & OpmodeAp;
}

void toggle() {
    action(ap::enabled()
        ? Action::AccessPointStop
        : Action::AccessPointStart);
}

// the captive portal DNS is the captive module's (captive.cpp), it follows the AP through Event::Mode
void stop() {
    driver::ap_stop();
}

void start(String&& defaultSsid, String&& ssid, String&& passphrase, uint8_t channel) {
    internal::start(std::move(defaultSsid), std::move(ssid),
        std::move(passphrase), channel);
}

SoftApNetwork current() {
    return driver::ap_current();
}

void init() {
    disable();
}

size_t stations() {
    return driver::ap_stations();
}

namespace fallback {
namespace build {

constexpr auto Timeout = duration::Milliseconds{ WIFI_FALLBACK_TIMEOUT };

} // namespace build

namespace internal {

auto timeout = build::Timeout;
bool enabled { false };
timer::SystemTimer timer;

} // namespace internal

void enable() {
    internal::enabled = true;
}

void disable() {
    internal::enabled = false;
}

bool enabled() {
    return internal::enabled;
}

void remove() {
    internal::timer.stop();
}

void check();

void schedule() {
    internal::timer.repeat(
        internal::timeout,
        []() {
            action(Action::AccessPointFallbackCheck);
        });
}

// Nobody had a chance to join an AP that just started: the check also runs on every WebSocket disconnect (ws.cpp),
// e.g. right after WIFI.AP was typed into a WebUI terminal, and the AP start itself drops the STA side clients of some
// chips. The AP stays at least one fallback interval
void check() {
    if (ap::enabled()
        && sta::connected()
        && !ap::stations()
        && (time::SystemClock::now() - ap::internal::started_at >= build::Timeout))
    {
        action(Action::AccessPointStop);
        return;
    }
}

} // namespace fallback

void configure() {
    auto current = settings::mode();
    if (ApMode::Fallback == current) {
        fallback::enable();
    } else {
        fallback::disable();
        fallback::remove();
        action((ApMode::Enabled == current)
                ? Action::AccessPointStart
                : Action::AccessPointStop);
    }
}

} // namespace ap

// -----------------------------------------------------------------------------
// SETTINGS
// -----------------------------------------------------------------------------

namespace settings {

STRING_VIEW_INLINE(Prefix, "wifi");

namespace query {

static constexpr std::array<espurna::settings::query::Setting, 12> Settings PROGMEM {
    {{ap::settings::keys::Ssid, ap::settings::ssid},
     {ap::settings::keys::Passphrase, ap::settings::passphrase},
     {ap::settings::keys::Captive, ap::settings::query::internal::captive},
     {ap::settings::keys::Channel, ap::settings::query::internal::channel},
     {ap::settings::keys::Mode, ap::settings::query::internal::mode},
     {sta::settings::keys::Mode, sta::settings::query::internal::mode},
     {sta::scan::settings::keys::Enabled, sta::scan::settings::query::enabled},
     {sta::scan::periodic::settings::keys::Threshold, sta::scan::periodic::settings::query::threshold},
     {sta::scan::periodic::settings::keys::Delta, sta::scan::periodic::settings::query::delta},
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

void configure() {
    ap::configure();
    sta::configure();

    sleep_type(settings::sleep());
    tx_power(settings::txPower());
}

} // namespace settings

// -----------------------------------------------------------------------------
// TERMINAL
// -----------------------------------------------------------------------------

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

PROGMEM_STRING(Stations, "WIFI.STATIONS");

void stations(::terminal::CommandContext&& ctx) {
    const auto stations = driver::print_stations(ctx.output);
    if (!stations) {
        terminalError(ctx, F("No stations connected"));
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

        terminalError(ctx, F("Network ID out of configurable range"));
        return;
    }

    const auto mode = wifi::opmode();
    ctx.output.printf_P(PSTR("OPMODE: %s\n"),
            debug::opmode(mode).c_str());

    const auto sleep = wifi::sleep_type();
    if (sleep != NONE_SLEEP_T) {
        ctx.output.printf_P(PSTR("SLEEP: %s\n"),
            debug::sleep_type(sleep).c_str());
    }


    if (mode & OpmodeAp) {
        auto current = ap::current();

        ctx.output.printf_P(PSTR("SoftAP: bssid %s channel %hhu auth %s ssid \"%s\"\n"),
            debug::mac(current.bssid).c_str(),
            current.channel,
            debug::authmode(current.authmode).c_str(),
            current.ssid.c_str());

        if (ap::fallback::enabled() && ap::fallback::internal::timer) {
            ctx.output.printf_P(PSTR("fallback check every %u ms\n"),
                ap::fallback::build::Timeout.count());
        }
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
    action(Action::TurnOff);
    terminalOK(ctx);
}

PROGMEM_STRING(On, "WIFI.ON");

void on(::terminal::CommandContext&& ctx) {
    action(Action::TurnOn);
    terminalOK(ctx);
}

PROGMEM_STRING(Scan, "WIFI.SCAN");

void scan(::terminal::CommandContext&& ctx) {
    bool failed { false };
    sta::scan::wait(
        [&](const driver::ScanResult* info) {
            ctx.output.printf_P(PSTR("BSSID: %s AUTH: %11s RSSI: %3hhd CH: %2hhu SSID: %s\n"),
                debug::mac(convertBssid(*info)).c_str(),
                debug::authmode(info->authmode).c_str(),
                info->rssi,
                info->channel,
                convertSsid(*info).c_str()
            );
        },
        [&](ScanError error) {
            failed = true;
            terminalError(ctx, debug::error(error));
        }
    );

    if (!failed) {
        terminalOK(ctx);
    }
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Stations, commands::stations},
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

void onConnected(JsonObject& root) {
    for (const auto& setting : settings::query::Settings) {
        root[FPSTR(setting.key().c_str())] = setting.value();
    }

    espurna::web::ws::EnumerableConfig config{root, STRING_VIEW("wifiConfig")};
    config(STRING_VIEW("networks"), sta::countNetworks(), sta::settings::query::Settings);

    auto& container = config.root();
    container[F("max")] = sta::build::NetworksMax;
}

bool onKeyCheck(StringView key, const JsonVariant&) {
    return settings::query::checkExactPrefix(key)
        || settings::query::checkIndexedPrefix(key);
}

void onScan(uint32_t client_id) {
    sta::scan::start([client_id](const driver::ScanResult* found) {
        SsidInfo result(*found);
        wsPost(client_id, [result](JsonObject& root) {
            JsonArray& scan = root.createNestedArray("scanResult");

            auto& info = result.info();
            scan.add(debug::mac(info.bssid()));
            scan.add(debug::authmode(info.authmode()));
            scan.add(info.rssi());
            scan.add(info.channel());

            scan.add(result.ssid());
        });
    },
    [client_id](ScanError error) {
        wsPost(client_id, [error](JsonObject& root) {
            root["scanError"] = debug::error(error);
        });
    });
}

void onAction(uint32_t client_id, const char* action, JsonObject&) {
    if (STRING_VIEW("scan") == action) {
        onScan(client_id);
    }
}

} // namespace web
#endif

// -----------------------------------------------------------------------------
// INITIALIZATION
// -----------------------------------------------------------------------------

namespace settings {

void migrate(int version) {
    if (version < 5) {
        moveSetting(F("apmode"), ap::settings::keys::Mode);
    }
}

} // namespace settings

namespace debug {

[[gnu::unused]]
String event(Event value) {
    String out;

    switch (value) {
    case Event::Initial:
        out = F("Initial");
        break;
    case Event::Mode: {
        const auto mode = wifi::opmode();
        out = F("Mode changed to ");
        out += debug::opmode(mode);
        break;
    }
    case Event::StationInit:
        out = F("Station init");
        break;
    case Event::StationScan:
        out = F("Scanning");
        break;
    case Event::StationConnecting:
        out = F("Connecting");
        break;
    case Event::StationConnected: {
        auto current = sta::current();
        out += F("Connected to BSSID ");
        out += debug::mac(current.bssid);
        out += F(" SSID ");
        out += current.ssid;
        break;
    }
    case Event::StationTimeout:
        out = F("Connection timeout");
        break;
    case Event::StationDisconnected: {
        auto current = sta::current();
        out += F("Disconnected from ");
        out += current.ssid;
        break;
    }
    case Event::StationReconnect:
        out = F("Reconnecting");
        break;
    }

    return out;
}

[[gnu::unused]]
const char* state(State value) {
    const char* out = "?";

    switch (value) {
    case State::Boot:
        out = PSTR("Boot");
        break;
    case State::Connect:
        out = PSTR("Connect");
        break;
    case State::TryConnectBetter:
        out = PSTR("TryConnectBetter");
        break;
    case State::Fallback:
        out = PSTR("Fallback");
        break;
    case State::Connected:
        out = PSTR("Connected");
        break;
    case State::Idle:
        out = PSTR("Idle");
        break;
    case State::Init:
        out = PSTR("Init");
        break;
    case State::Timeout:
        out = PSTR("Timeout");
        break;
    case State::WaitScan:
        out = PSTR("WaitScan");
        break;
    case State::WaitScanWithoutCurrent:
        out = PSTR("WaitScanWithoutCurrent");
        break;
    case State::WaitConnected:
        out = PSTR("WaitConnected");
        break;
    }

    return out;
}

} // namespace debug

namespace internal {

// STA + AP FALLBACK:
// - try connection
// - if ok, stop existing AP
// - if not, keep / start AP
//
// STA:
// - try connection
// - don't do anything on completion
//
// TODO? WPS / SMARTCONFIG + STA + AP FALLBACK
// - same as above
// - when requested, make sure there are no active connections
//   abort when sta connected or ap is connected
// - run autoconf, receive credentials and store in a free settings slot

// TODO: provide a clearer 'unroll' of the current state?

using EventCallbacks = std::forward_list<EventCallback>;
EventCallbacks callbacks;

void publish(Event event) {
    for (auto& callback : callbacks) {
        callback(event);
    }
}

void subscribe(EventCallback callback) {
    callbacks.push_front(callback);
}

State handle_action(State state, Action action) {
    switch (action) {
    case Action::StationConnect:
        if (!sta::enabled()) {
            sta::enable();
            publish(Event::Mode);
        }

        if (!sta::connected()) {
            if (sta::connecting()) {
                sta::connection::schedule_next();
            } else {
                state = State::Init;
            }
        }
        break;

    case Action::StationContinueConnect:
        if (sta::connecting()) {
            state = State::Connect;
        }
        break;

    case Action::StationDisconnect:
        if (sta::connected()) {
            ap::fallback::remove();
            sta::disconnect();
        }

        sta::connection::stop();

        if (sta::enabled()) {
            sta::disable();
            publish(Event::Mode);
        }
        break;

    case Action::StationTryConnectBetter:
        if (!sta::connected() || sta::connecting()) {
            sta::scan::periodic::stop();
            break;
        }

        if (sta::scan::periodic::check()) {
            state = State::TryConnectBetter;
        }
        break;

    case Action::AccessPointFallback:
    case Action::AccessPointStart:
        if (!ap::enabled()) {
            ap::enable();
            ap::start(
                ap::settings::defaultSsid(),
                ap::settings::ssid(),
                ap::settings::passphrase(),
                ap::settings::channel());
            publish(Event::Mode);
            if ((Action::AccessPointFallback == action)
                    && ap::fallback::enabled()) {
                ap::fallback::schedule();
            }
        }
        break;

    case Action::AccessPointFallbackCheck:
        if (ap::fallback::enabled()) {
            ap::fallback::check();
        }
        break;

    case Action::AccessPointStop:
        if (ap::enabled()) {
            ap::fallback::remove();
            ap::stop();
            ap::disable();
            publish(Event::Mode);
        }
        break;

    case Action::TurnOff:
        if (wifi::enabled()) {
            ap::fallback::remove();
            ap::stop();
            ap::disable();
            sta::scan::periodic::stop();
            sta::connection::stop();
            sta::disconnect();
            sta::disable();
            wifi::disable();
            publish(Event::Mode);
            break;
        }
        break;

    case Action::Boot:
    case Action::TurnOn:
        if (!wifi::enabled()) {
            wifi::enable();
#if SYSTEM_CHECK_ENABLED
            if ((action == Action::Boot) && !systemCheck()) {
                wifi::action(Action::AccessPointStart);
                break;
            }
#endif
            settings::configure();
        }
        break;

    }

    return state;
}

bool prepareConnection() {
    if (sta::enabled()) {
        sta::connection::prepare(sta::networks());
        return sta::connection::prepared();
    }

    return false;
}

void loop() {
    driver::loop();

    if (last_state != state) {
        DEBUG_MSG_P(PSTR("[WIFI] State %s -> %s\n"),
            debug::state(last_state),
            debug::state(state));
        last_state = state;
    }

    switch (state) {

    case State::Boot:
        state = State::Idle;
        publish(Event::Initial);
        break;

    case State::Init: {
        if (!prepareConnection()) {
            state = State::Fallback;
            break;
        }

        sta::scan::periodic::stop();
        if (sta::scan::settings::enabled()) {
            if (sta::scanning()) {
                break;
            }
            sta::connection::scanNetworks();
            state = State::WaitScan;
            break;
        }

        state = State::Connect;
        break;
    }

    case State::TryConnectBetter:
        if (sta::scan::settings::enabled()) {
            if (sta::scanning()) {
                break;
            }

            if (!prepareConnection()) {
                state = State::Idle;
                break;
            }

            sta::scan::periodic::stop();
            sta::connection::scanNetworks();
            state = State::WaitScanWithoutCurrent;
            break;
        }
        state = State::Idle;
        break;

    case State::Fallback:
        state = State::Idle;
        sta::connection::schedule_new();
        if (ApMode::Fallback == ap::settings::mode()) {
            action(Action::AccessPointFallback);
        }
        publish(Event::StationReconnect);
        break;

    case State::WaitScan:
        if (sta::scanning()) {
            break;
        }

        sta::connection::scanProcessResults();
        state = State::Connect;
        break;

    case State::WaitScanWithoutCurrent:
        if (sta::scanning()) {
            break;
        }

        if (sta::connection::scanProcessResults(sta::info())) {
            sta::disconnect();
            state = State::Connect;
            break;
        }

        // still connected to the same AP, keep checking (less often)
        if (sta::connected() && sta::scan::settings::enabled()) {
            sta::scan::periodic::stayed();
        }

        state = State::Idle;
        break;

    case State::Connect: {
        if (!sta::connecting()) {
            if (!sta::connection::start(systemHostname())) {
                state = State::Timeout;
                break;
            }
        }

        if (sta::connection::connect()) {
            state = State::WaitConnected;
            publish(Event::StationConnecting);
        } else {
            state = State::Timeout;
        }
        break;
    }

    case State::WaitConnected:
        if (sta::connection::wait()) {
            break;
        }

        if (sta::connected()) {
            state = State::Connected;
            break;
        }

        state = State::Timeout;
        break;

    // Current logic closely follows the SDK connection routine with reconnect enabled,
    // and will retry the same network multiple times before giving up.
    case State::Timeout:
        if (sta::connecting() && sta::connection::next()) {
            state = State::Idle;
            sta::connection::schedule_next();
            publish(Event::StationTimeout);
        } else {
            sta::connection::stop();
            state = State::Fallback;
        }
        break;

    case State::Connected:
        sta::connection::stop();
        if (sta::scan::settings::enabled()) {
            sta::scan::periodic::start();
        }
        state = State::Idle;
        publish(Event::StationConnected);
        break;

    case State::Idle: {
        state = wifi::handle_action(
            state, internal::handle_action);
        break;
    }

    }

    // SDK disconnection event is specific to the phy layer. i.e. it will happen all the same
    // when trying to connect and being unable to find the AP, being forced out by the AP with bad credentials
    // or being disconnected when the wireless signal is lost.
    // Thus, provide a specific connected -> disconnected event specific to the IP network availability.
    if (sta::connection::lost()) {
        sta::scan::periodic::stop();
        if (sta::connection::persist()) {
            sta::connection::schedule_new(sta::build::RecoveryInterval);
        }
        publish(Event::StationDisconnected);
    }

#if WIFI_GRATUITOUS_ARP_SUPPORT
    // ref: https://github.com/xoseperez/espurna/pull/1877#issuecomment-525612546
    // Periodically send out ARP, even if no one asked
    if (sta::connected() && !sta::garp::wait()) {
        sta::garp::send();
    }
#endif
}

// XXX: With Arduino Core 3.0.0, WiFi is asleep on boot
// It will wake up when calling WiFi::mode(...):
// - WiFi.begin(...)
// - WiFi.softAP(...)
// - WiFi.enableSTA(...)
// - WiFi.enableAP(...)
// ref. https://github.com/esp8266/Arduino/pull/7902

void init() {
    driver::init();
    ap::init();
    sta::init();
}

} // namespace internal

void setup() {
    if (!driver::setup()) {
        return;
    }

    internal::init();
    sta::netwatch::setup();

    migrateVersion(settings::migrate);
    settings::query::setup();

    if (BootMode::Enabled == settings::bootMode()) {
        action(Action::Boot);
    }

#if DEBUG_SUPPORT
    wifiRegister([](Event event) {
        DEBUG_MSG_P(PSTR("[WIFI] %s\n"), debug::event(event).c_str());
    });
#endif

#if WEB_SUPPORT
    wsRegister()
        .onAction(web::onAction)
        .onConnected(web::onConnected)
        .onKeyCheck(web::onKeyCheck);
#endif

#if TERMINAL_SUPPORT
    terminal::init();
#endif

    espurnaRegisterLoop(internal::loop);
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
    sta::scan::internal::result(info);
}

void on_scan_done(bool ok, size_t count) {
    sta::scan::internal::done(ok, count);
}

void on_station_disconnected(const Mac& bssid, int reason) {
    sta::disconnected(bssid, reason);
}

void on_station_got_ip() {
    sta::got_ip();
}

} // namespace driver
} // namespace wifi
} // namespace espurna

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

void wifiRegister(espurna::wifi::EventCallback callback) {
    espurna::wifi::internal::subscribe(callback);
}

bool wifiConnectable() {
    return espurna::wifi::ap::enabled();
}

bool wifiConnected() {
    return espurna::wifi::sta::connected();
}

espurna::wifi::StaNetwork wifiStaInfo() {
    if (espurna::wifi::opmode() & espurna::wifi::OpmodeSta) {
        return espurna::wifi::sta::current();
    }

    return {};
}

IPAddress wifiStaIp() {
    if (espurna::wifi::opmode() & espurna::wifi::OpmodeSta) {
        return espurna::wifi::sta::ip();
    }

    return {};
}

String wifiStaSsid() {
    if (espurna::wifi::opmode() & espurna::wifi::OpmodeSta) {
        auto current = espurna::wifi::sta::current();
        return current.ssid;
    }

    return emptyString;
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
    espurna::wifi::action(
        espurna::wifi::Action::AccessPointStart);
}

bool wifiDisabled() {
    return espurna::wifi::opmode()
        == espurna::wifi::OpmodeNull;
}

void wifiDisable() {
    espurna::wifi::ap::fallback::remove();
    espurna::wifi::sta::scan::periodic::stop();
    espurna::wifi::ensure_opmode(
        espurna::wifi::OpmodeNull);
}

void wifiTurnOff() {
    espurna::wifi::action(
        espurna::wifi::Action::TurnOff);
}

void wifiTurnOn() {
    espurna::wifi::action(
        espurna::wifi::Action::TurnOn);
}

void wifiApCheck() {
    espurna::wifi::action(
        espurna::wifi::Action::AccessPointFallbackCheck);
}

size_t wifiApStations() {
    if (espurna::wifi::ap::enabled()) {
        return espurna::wifi::ap::stations();
    }

    return 0;
}

espurna::wifi::SoftApNetwork wifiApInfo() {
    if (espurna::wifi::opmode() & espurna::wifi::OpmodeAp) {
        return espurna::wifi::ap::current();
    }

    return {};
}

IPAddress wifiApIp() {
    return espurna::wifi::ap::ip();
}

bool wifiApCaptive() {
    return espurna::wifi::ap::settings::captive();
}

void wifiSetup() {
    espurna::wifi::setup();
}
