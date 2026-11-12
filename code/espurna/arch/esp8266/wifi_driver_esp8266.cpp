/*

WIFI MODULE, esp8266 NONOS SDK part (ref. wifi_driver.h)

Original code based on JustWifi, Wifi Manager for ESP8266 (GPLv3+)
Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

Modified for ESPurna
Copyright (C) 2021 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

*/

#include "../../espurna.h"

#if defined(ESP8266)

#include "../../wifi_driver.h"

#include <AddrList.h>

// ref.
// https://github.com/d-a-v/esp82xx-nonos-linklayer/blob/master/README.md#how-it-works
//
// Current esp8266 Arduino Core is based on the NONOS SDK using the lwip1.4 APIs
// To handle static IPs, these need to be called when current IP differs from the one set via the setting.
//
// Can't include the original headers, since they refer to the ip_addr_t and IPAddress depends on a specific overload to extract v4 addresses
// (SDK layer *only* works with ipv4 addresses)

#undef netif_set_addr
extern "C" netif* eagle_lwip_getif(int);
extern "C" void netif_set_addr(netif* netif, ip4_addr_t*, ip4_addr_t*, ip4_addr_t*);

namespace espurna {
namespace wifi {
namespace driver {
namespace {

static_assert(OpmodeNull == NULL_MODE, "");
static_assert(OpmodeSta == STATION_MODE, "");
static_assert(OpmodeAp == SOFTAP_MODE, "");

static_assert(SsidMax == sizeof(softap_config::ssid), "");
static_assert(SsidMax == sizeof(station_config::ssid), "");
static_assert(PassphraseMax == sizeof(softap_config::password), "");
static_assert(PassphraseMax == sizeof(station_config::password), "");

namespace build {

static constexpr int8_t RssiThreshold { -127 };

static constexpr int Hidden { 0 };
static constexpr uint8_t ConnectionsMax { 4u };

} // namespace build

// We are guaranteed to have '\0' when <32 b/c the SDK zeroes out the data
// But, these are byte arrays, not C strings. When ssid_len is available, use it.
// When not, we are still expecting the <32 arrays to have '\0' at the end and we manually
// set the 32'nd char to '\0' to prevent conversion issues

String convertSsid(const softap_config& config) {
    String ssid;
    ssid.concat(reinterpret_cast<const char*>(config.ssid), config.ssid_len);
    return ssid;
}

template <typename T, size_t SsidSize = sizeof(T::ssid)>
String convertSsid(const T& config) {
    static_assert(SsidSize == 32, "");

    const char* ptr { reinterpret_cast<const char*>(config.ssid) };
    char ssid[SsidSize + 1];
    std::copy(ptr, ptr + SsidSize, ssid);
    ssid[SsidSize] = '\0';

    return ssid;
}

template <typename T, size_t PassphraseSize = sizeof(T::password)>
String convertPassphrase(const T& config) {
    static_assert(PassphraseSize == 64, "");

    const char* ptr { reinterpret_cast<const char*>(config.password) };
    char passphrase[PassphraseSize + 1];
    std::copy(ptr, ptr + PassphraseSize, passphrase);
    passphrase[PassphraseSize] = '\0';

    return passphrase;
}

template <typename T, size_t MacSize = sizeof(T::bssid)>
Mac convertBssid(const T& info) {
    static_assert(MacSize == 6, "");
    Mac mac;
    std::copy(info.bssid, info.bssid + MacSize, mac.begin());
    return mac;
}

String ip(const IPAddress& addr) {
    return addr.toString();
}

String ip(ip4_addr_t addr) {
    String out;
    out.reserve(16);

    bool delim { false };
    for (int byte = 0; byte < 4; ++byte) {
        if (delim) {
            out += '.';
        }
        out += ip4_addr_get_byte_val(addr, byte);
        delim = true;
    }

    return out;
}

ip_info toIpInfo(const StaConnect& connect) {
    ip_info info{};
    info.ip.addr = connect.ip.v4();
    info.netmask.addr = connect.netmask.v4();
    info.gw.addr = connect.gateway.v4();

    return info;
}

// STATUS comes from c_types.h, and it seems this is the only place that uses it
// instead of some ESP-specific type.

void scan_complete(void* result, STATUS status) {
    if (status) { // aka anything but OK / 0
        on_scan_done(false, 0);
        return;
    }

    size_t networks { 0ul };
    bss_info* head = reinterpret_cast<bss_info*>(result);
    for (bss_info* it = head; it; it = STAILQ_NEXT(it, next), ++networks) {
        on_scan_result(*it);
    }

    on_scan_done(true, networks);
}

// SDK keeps its own softAP config in the last flash sectors and applies it as soon as AP mode is enabled,
// before WiFi.softAP() sets ours. When these sectors hold garbage (e.g. overwritten by an OTA image),
// beacon allocation fails inside wifi_softap_start() and every fallback to AP mode is a crash. Board
// may not be reachable by wire, so wipe the SDK config (ESPurna settings are not there) and restart
bool sdk_terminated(const uint8_t* data, size_t size) {
    return memchr(data, '\0', size) != nullptr;
}

// Persistent mode is off, so this is never our config. Only the SDK defaults (ESP_XXXXXX, channel 1,
// open, 4 connections, 100ms beacon) are expected here; random garbage easily passes the documented
// API limits though, so check the strings too and keep ranges tight
bool sdk_softap_config_valid() {
    softap_config config{};
    if (!wifi_softap_get_config_default(&config)) {
        return false;
    }

    const bool ssid = (config.ssid_len > 0)
        ? (config.ssid_len <= sizeof(config.ssid))
        : sdk_terminated(config.ssid, sizeof(config.ssid));

    return ssid
        && sdk_terminated(config.password, sizeof(config.password))
        && (config.channel >= 1) && (config.channel <= 13)
        && (config.authmode < AUTH_MAX) && (config.authmode != AUTH_WEP)
        && (config.ssid_hidden <= 1)
        && (config.max_connection >= 1) && (config.max_connection <= 4)
        && (config.beacon_interval >= 100) && (config.beacon_interval <= 1000);
}

// Erased sectors mean defaults are used. Never erase twice, restarting again would not change anything
bool sdk_config_erased() {
    uint32_t word { 0 };
    const auto address = ESP.getFlashChipSize() - 0x3000;
    return ESP.flashRead(address, &word, sizeof(word))
        && (word == 0xffffffff);
}

} // namespace

bool setup() {
    if (!sdk_softap_config_valid() && !sdk_config_erased()) {
        DEBUG_MSG_P(PSTR("[WIFI] SDK config is corrupted, erasing it and restarting\n"));
        ESP.eraseConfig();
        ESP.restart();
        return false;
    }

    return true;
}

void init() {
    WiFi.persistent(false);

    // event objects must remain in scope, otherwise dtor removes them from the sdk handler queue
    static auto disconnected = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
        Mac bssid;
        std::copy(std::begin(event.bssid), std::end(event.bssid), bssid.begin());
        on_station_disconnected(bssid, event.reason);
    });

    static auto connected = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) {
        on_station_got_ip();
    });
}

// SDK events are delivered from SYS, which never overlaps with loop()
void loop() {
}

uint8_t opmode() {
    return wifi_get_opmode();
}

void opmode(uint8_t mode) {
    wifi_set_opmode_current(mode);
}

void wakeup() {
    wakeupModemForcedSleep();
}

void tx_power(float dbm) {
    // system_phy_set_max_tpw() unit is .25dBm
    constexpr auto Min = float{ 0.0f };
    constexpr auto Max = float{ 20.5f };
    dbm = std::clamp(dbm, Min, Max);
    dbm *= 4.0f;

    system_phy_set_max_tpw(dbm);
}

// IPv6 is a build option of lwIP here
void ipv6(bool) {
}

sleep_type_t sleep_type() {
    return wifi_get_sleep_type();
}

bool sleep_type(sleep_type_t type) {
    return wifi_set_sleep_type(type);
}

String authmode(AUTH_MODE mode) {
    StringView out;

    switch (mode) {
    case AUTH_OPEN:
        out = STRING_VIEW("OPEN");
        break;
    case AUTH_WEP:
        out = STRING_VIEW("WEP");
        break;
    case AUTH_WPA_PSK:
        out = STRING_VIEW("WPAPSK");
        break;
    case AUTH_WPA2_PSK:
        out = STRING_VIEW("WPA2PSK");
        break;
    case AUTH_WPA_WPA2_PSK:
        out = STRING_VIEW("WPAWPA2-PSK");
        break;
    case AUTH_MAX:
    default:
        out = STRING_VIEW("UNKNOWN");
        break;
    }

    return out.toString();
}

IPAddress sta_ip() {
    ip_info info;
    wifi_get_ip_info(STATION_IF, &info);

    return info.ip;
}

uint8_t sta_channel() {
    return wifi_get_channel();
}

int8_t sta_rssi() {
    return wifi_station_get_rssi();
}

// Note that authmode field is a our threshold, not the one selected by an AP
void sta_info(Mac& bssid, AUTH_MODE& authmode) {
    station_config config{};
    wifi_station_get_config(&config);

    bssid = convertBssid(config);
    authmode = config.threshold.authmode;
}

StaNetwork sta_current() {
    station_config config{};
    wifi_station_get_config(&config);

    return {
        convertBssid(config),
        convertSsid(config),
        convertPassphrase(config),
        sta_rssi(),
        sta_channel()};
}

void sta_disconnect() {
    wifi_station_disconnect();
}

void sta_prepare() {
    wifi_station_disconnect();
    delay(10);

    if (wifi_station_get_reconnect_policy()) {
        wifi_station_set_reconnect_policy(false);
    }

    if (wifi_station_get_auto_connect()) {
        wifi_station_set_auto_connect(false);
    }
}

bool sta_connect(const StaConnect& connect) {
    if (!connect.dhcp) {
        wifi_station_dhcpc_stop();

        auto current = sta_ip();

        auto info = toIpInfo(connect);
        if (!wifi_set_ip_info(STATION_IF, &info)) {
            return false;
        }

        dns_setserver(0, connect.dns);

        if (current.isSet() && (current != info.ip)) {
#undef netif_set_addr
            netif_set_addr(eagle_lwip_getif(STATION_IF), &info.ip, &info.netmask, &info.gw);
        }
    }

    // Only the STA cares about the hostname setting
    // esp8266 specific Arduino-specific - this sets lwip internal structs related to the DHCPc
    WiFi.hostname(connect.hostname);

    // The rest is related to the connection routine
    // SSID & Passphrase are u8 arrays, with 0 at the end when the string is less than it's size
    // Lengths are checked by the common code before this call (ref. StaConnect)

    station_config config{};

    auto& ssid = connect.ssid;
    std::copy(ssid.c_str(), ssid.c_str() + ssid.length(),
            reinterpret_cast<char*>(config.ssid));
    if (ssid.length() < SsidMax) {
        config.ssid[ssid.length()] = 0;
    }

    auto& pass = connect.passphrase;
    if (pass.length()) {
        config.threshold.authmode = AUTH_WPA_PSK;
        std::copy(pass.c_str(), pass.c_str() + pass.length(),
                reinterpret_cast<char*>(config.password));
        if (pass.length() < PassphraseMax) {
            config.password[pass.length()] = 0;
        }
    } else {
        config.threshold.authmode = AUTH_OPEN;
        config.password[0] = 0;
    }

    config.threshold.rssi = build::RssiThreshold;

    if (connect.channel) {
        auto& bssid = connect.bssid;
        std::copy(bssid.begin(), bssid.end(), config.bssid);
        config.bssid_set = 1;
    }

    // TODO: check every return value?
    // TODO: is it sufficient for the event to fire? otherwise,
    // there needs to be a manual timeout code after this returns true

    wifi_station_set_config_current(&config);
    if (!wifi_station_connect()) {
        return false;
    }

    if (connect.channel) {
        wifi_set_channel(connect.channel);
    }

    if (connect.dhcp && (wifi_station_dhcpc_status() != DHCP_STARTED)) {
        wifi_station_dhcpc_start();
    }

    return true;
}

bool scan_start() {
    // Note that esp8266 callback only reports the resulting status and will (always?) timeout all by itself
    // Default values are an active scan with some unspecified channel times.
    // (zeroed out scan_config struct or simply nullptr)

    // For example, c/p config from the current esp32 Arduino Core wrapper which are close to the values mentioned here:
    // https://github.com/espressif/ESP8266_NONOS_SDK/issues/103#issuecomment-383440370
    // Which could be useful if scanning needs to be more aggressive or switched into PASSIVE scan type

    //scan_config config{};
    //config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    //config.scan_time.active.min = 100;
    //config.scan_time.active.max = 300;

    return wifi_station_scan(nullptr, &scan_complete);
}

void ap_start(const char* ssid, const char* passphrase, uint8_t channel) {
    // TODO: when using `softap_config`, can also tweak the beacon intvl
    // static constexpr uint16_t BeaconInterval { 100u };
    WiFi.softAP(ssid, passphrase, channel, build::Hidden, build::ConnectionsMax);
}

void ap_stop() {
    WiFi.softAPdisconnect();
}

IPAddress ap_ip() {
    ip_info info;
    wifi_get_ip_info(SOFTAP_IF, &info);

    return info.ip;
}

SoftApNetwork ap_current() {
    softap_config config{};
    wifi_softap_get_config(&config);

    Mac mac;
    WiFi.softAPmacAddress(mac.data());

    return {
        mac,
        convertSsid(config),
        convertPassphrase(config),
        config.channel,
        config.authmode};
}

size_t ap_stations() {
    return WiFi.softAPgetStationNum();
}

size_t print_stations(Print& out) {
    size_t stations { 0ul };
    for (auto* it = wifi_softap_get_station_info(); it; it = STAILQ_NEXT(it, next), ++stations) {
        out.printf_P(PSTR("%s %s\n"),
            espurna::settings::internal::serialize(convertBssid(*it)).c_str(),
            ip(it->ip).c_str());
    }

    wifi_softap_free_station_info();

    return stations;
}

void print_network(Print& out) {
    for (auto& addr : addrList) {
        out.printf_P(PSTR("%s%d %4s %6s "),
            addr.ifname().c_str(),
            addr.ifnumber(),
            addr.ifUp() ? "up" : "down",
            addr.isLocal() ? "local" : "global");

#if LWIP_IPV6
        if (addr.isV4()) {
#endif
            out.printf_P(PSTR("ip %s gateway %s mask %s\n"),
                ip(addr.ipv4()).c_str(),
                ip(addr.gw()).c_str(),
                ip(addr.netmask()).c_str());
#if LWIP_IPV6
        } else {
            // TODO: ip6_addr[...] array is included in the list
            // we'll just see another entry
            // TODO: routing info is not attached to the netif :/
            // ref. nd6.h (and figure out what it does)
            out.printf_P(PSTR("ip %s\n"),
                ip(netif->ip6_addr[i]).c_str());
        }
#endif

    }

    for (int n = 0; n < DNS_MAX_SERVERS; ++n) {
        auto addr = IPAddress(dns_getserver(n));
        if (!addr.isSet()) {
            break;
        }
        out.printf_P(PSTR("dns %s\n"), ip(addr).c_str());
    }
}

} // namespace driver
} // namespace wifi
} // namespace espurna

#endif
