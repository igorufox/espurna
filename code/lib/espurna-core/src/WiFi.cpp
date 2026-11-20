#include "WiFi.h"

#include "core_chip.h"


namespace chip = espurna::core::chip;

WiFiClass WiFi;

namespace {

String mac_string(const uint8_t* mac) {
    char out[18];
    snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(out);
}

String ssid_string(const uint8_t (&ssid)[33]) {
    char out[33];
    memcpy(out, ssid, 32);
    out[32] = '\0';
    return String(out);
}

uint8_t* bssid_copy(const uint8_t* bssid, uint8_t* out) {
    static uint8_t buffer[6];
    uint8_t* dest = out ? out : buffer;
    memcpy(dest, bssid, 6);
    return dest;
}

} // namespace

bool WiFiClass::mode(wifi_mode_t mode) {
    return chip::wifi_mode(mode);
}

wifi_mode_t WiFiClass::getMode() {
    return chip::wifi_get_mode();
}

bool WiFiClass::enableSTA(bool enable) {
    const auto current = static_cast<int>(getMode());
    const auto next = enable
        ? (current | static_cast<int>(WIFI_MODE_STA))
        : (current & ~static_cast<int>(WIFI_MODE_STA));
    return mode(static_cast<wifi_mode_t>(next));
}

bool WiFiClass::enableAP(bool enable) {
    const auto current = static_cast<int>(getMode());
    const auto next = enable
        ? (current | static_cast<int>(WIFI_MODE_AP))
        : (current & ~static_cast<int>(WIFI_MODE_AP));
    return mode(static_cast<wifi_mode_t>(next));
}

bool WiFiClass::setSleep(bool enabled) {
    return chip::wifi_sleep(enabled);
}

wl_status_t WiFiClass::begin(const char* ssid, const char* passphrase, int32_t channel,
        const uint8_t* bssid, bool connect)
{
    if (!ssid || !*ssid) {
        return WL_CONNECT_FAILED;
    }

    if (!enableSTA(true)) {
        return WL_CONNECT_FAILED;
    }

    if (!connect) {
        return status();
    }

    return chip::wifi_sta_begin(ssid, passphrase, static_cast<uint8_t>(channel), bssid)
        ? status()
        : WL_CONNECT_FAILED;
}

bool WiFiClass::disconnect(bool wifioff, bool) {
    const bool out = chip::wifi_sta_disconnect();
    if (wifioff) {
        enableSTA(false);
    }

    return out;
}

bool WiFiClass::config(IPAddress ip, IPAddress gateway, IPAddress subnet, IPAddress dns1, IPAddress dns2) {
    return chip::wifi_sta_config(
        static_cast<uint32_t>(ip), static_cast<uint32_t>(gateway), static_cast<uint32_t>(subnet),
        static_cast<uint32_t>(dns1), static_cast<uint32_t>(dns2));
}

bool WiFiClass::setHostname(const char* hostname) {
    return chip::wifi_hostname(hostname);
}

const char* WiFiClass::getHostname() {
    return chip::wifi_get_hostname();
}

wl_status_t WiFiClass::status() {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    return (chip::wifi_sta_address(ip, netmask, gateway) && ip)
        ? WL_CONNECTED
        : WL_DISCONNECTED;
}

bool WiFiClass::isConnected() {
    return status() == WL_CONNECTED;
}

IPAddress WiFiClass::localIP() {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    chip::wifi_sta_address(ip, netmask, gateway);
    return IPAddress(ip);
}

IPAddress WiFiClass::subnetMask() {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    chip::wifi_sta_address(ip, netmask, gateway);
    return IPAddress(netmask);
}

IPAddress WiFiClass::gatewayIP() {
    uint32_t ip { 0 };
    uint32_t netmask { 0 };
    uint32_t gateway { 0 };
    chip::wifi_sta_address(ip, netmask, gateway);
    return IPAddress(gateway);
}

String WiFiClass::macAddress() {
    uint8_t mac[6] {};
    chip::wifi_mac(false, mac);
    return mac_string(mac);
}

uint8_t* WiFiClass::macAddress(uint8_t* mac) {
    chip::wifi_mac(false, mac);
    return mac;
}

bool WiFiClass::setMacAddress(const uint8_t* mac) {
    return chip::wifi_set_mac(false, mac);
}

String WiFiClass::SSID() {
    chip::WifiInfo info{};
    return chip::wifi_sta_info(info)
        ? ssid_string(info.ssid)
        : String();
}

uint8_t* WiFiClass::BSSID(uint8_t* out) {
    chip::WifiInfo info{};
    if (!chip::wifi_sta_info(info)) {
        return nullptr;
    }

    return bssid_copy(info.bssid, out);
}

String WiFiClass::BSSIDstr() {
    chip::WifiInfo info{};
    return chip::wifi_sta_info(info)
        ? mac_string(info.bssid)
        : String();
}

int8_t WiFiClass::RSSI() {
    chip::WifiInfo info{};
    return chip::wifi_sta_info(info)
        ? info.rssi
        : 0;
}

int32_t WiFiClass::channel() {
    return chip::wifi_channel();
}

bool WiFiClass::enableIPv6(bool enable) {
    return chip::wifi_ipv6(enable);
}

const char* WiFiClass::disconnectReasonName(wifi_err_reason_t reason) {
    return chip::wifi_reason_name(static_cast<int>(reason));
}

int16_t WiFiClass::scanNetworks(bool async, bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel) {
    if (scanComplete() == WIFI_SCAN_RUNNING) {
        return WIFI_SCAN_RUNNING;
    }

    scanDelete();
    if (!enableSTA(true) || !chip::wifi_scan_start(show_hidden, passive, max_ms_per_chan, channel)) {
        return WIFI_SCAN_FAILED;
    }

    if (async) {
        return WIFI_SCAN_RUNNING;
    }

    int16_t out;
    while ((out = scanComplete()) == WIFI_SCAN_RUNNING) {
        delay(10);
    }

    return out;
}

int16_t WiFiClass::scanComplete() {
    return chip::wifi_scan_count();
}

void WiFiClass::scanDelete() {
    chip::wifi_scan_delete();
}

String WiFiClass::SSID(uint8_t index) {
    chip::WifiInfo info{};
    return chip::wifi_scan_record(index, info)
        ? ssid_string(info.ssid)
        : String();
}

uint8_t* WiFiClass::BSSID(uint8_t index, uint8_t* out) {
    chip::WifiInfo info{};
    if (!chip::wifi_scan_record(index, info)) {
        return nullptr;
    }

    return bssid_copy(info.bssid, out);
}

String WiFiClass::BSSIDstr(uint8_t index) {
    chip::WifiInfo info{};
    return chip::wifi_scan_record(index, info)
        ? mac_string(info.bssid)
        : String();
}

int32_t WiFiClass::RSSI(uint8_t index) {
    chip::WifiInfo info{};
    return chip::wifi_scan_record(index, info)
        ? info.rssi
        : 0;
}

int32_t WiFiClass::channel(uint8_t index) {
    chip::WifiInfo info{};
    return chip::wifi_scan_record(index, info)
        ? info.channel
        : 0;
}

wifi_auth_mode_t WiFiClass::encryptionType(uint8_t index) {
    chip::WifiInfo info{};
    return chip::wifi_scan_record(index, info)
        ? info.authmode
        : WIFI_AUTH_OPEN;
}

bool WiFiClass::softAP(const char* ssid, const char* passphrase, int channel, int ssid_hidden, int max_connection) {
    if (!ssid || !*ssid) {
        return false;
    }

    if (!enableAP(true)) {
        return false;
    }

    return chip::wifi_ap_start(ssid, (passphrase && *passphrase) ? passphrase : nullptr,
        static_cast<uint8_t>(channel), ssid_hidden != 0, static_cast<uint8_t>(max_connection));
}

bool WiFiClass::softAPConfig(IPAddress local_ip, IPAddress gateway, IPAddress subnet) {
    return chip::wifi_ap_config(
        static_cast<uint32_t>(local_ip), static_cast<uint32_t>(gateway), static_cast<uint32_t>(subnet));
}

bool WiFiClass::softAPdisconnect(bool wifioff) {
    const bool out = chip::wifi_ap_stop();
    if (wifioff) {
        enableAP(false);
    }

    return out;
}

IPAddress WiFiClass::softAPIP() {
    return IPAddress(chip::wifi_ap_ip());
}

String WiFiClass::softAPSSID() {
    uint8_t ssid[33] {};
    return chip::wifi_ap_ssid(ssid)
        ? ssid_string(ssid)
        : String();
}

uint8_t* WiFiClass::softAPmacAddress(uint8_t* mac) {
    chip::wifi_mac(true, mac);
    return mac;
}

String WiFiClass::softAPmacAddress() {
    uint8_t mac[6] {};
    chip::wifi_mac(true, mac);
    return mac_string(mac);
}

uint8_t WiFiClass::softAPgetStationNum() {
    return chip::wifi_ap_stations();
}
