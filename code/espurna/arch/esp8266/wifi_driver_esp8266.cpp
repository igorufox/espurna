/*

WIFI MODULE, ESP8266 on ESP8266_RTOS_SDK 3.4: what only the SDK knows (ref. wifi_driver_rtos.h). The driver itself
(events, link state, connection, scan, AP settings) is the shared one, wifi_driver_rtos.cpp; the calls of the SDK are
the Wi-Fi calls of the chip (arch/rtos/wifi_sdk.h, core_esp8266.cpp)

*/

#include "../../espurna.h"

#include "../rtos/wifi_driver_rtos.h"

#include "../rtos/wifi_sdk.h"
#include <esp_wifi.h>
#include <tcpip_adapter.h>

#include <algorithm>
#include <cmath>

namespace espurna {
namespace wifi {
namespace driver {
namespace chip {
namespace {

bool started() {
    return ::espurna::core::chip::wifi_get_mode() != WIFI_MODE_NULL;
}

// EU country code, channels 12 and 13 are scanned and used as well
void apply_country() {
    if (!started()) {
        return;
    }

    wifi_country_t country{};
    country.cc[0] = 'E';
    country.cc[1] = 'U';
    country.schan = 1;
    country.nchan = 13;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    esp_wifi_set_country(&country);
}

} // namespace

// nothing kept in the SDK's flash (no NVS), the driver never reconnects by itself (core_esp8266.cpp)
void init() {
}

void opmode(uint8_t mode) {
    const bool was = started();
    ::espurna::core::chip::wifi_mode(static_cast<wifi_mode_t>(mode));

    if (!was) {
        apply_country();
    }

    apply_sleep();
    apply_tx_power();
}

// Needs the driver to be started, i.e. called after the opmode is set
void apply_sleep() {
    if (!started()) {
        return;
    }

    wifi_ps_type_t type { WIFI_PS_NONE };
    switch (rtos::state().sleep) {
    case LIGHT_SLEEP_T:
        type = WIFI_PS_MAX_MODEM;
        break;
    case MODEM_SLEEP_T:
        type = WIFI_PS_MIN_MODEM;
        break;
    case NONE_SLEEP_T:
        break;
    }

    esp_wifi_set_ps(type);
}

// what the driver runs now (WIFI.DEBUG): the setting is applied when the radio allows it, the SDK may hold its own
const char* sleep_name() {
    wifi_ps_type_t type { WIFI_PS_NONE };
    if (!started() || (esp_wifi_get_ps(&type) != ESP_OK)) {
        return "-";
    }

    switch (type) {
    case WIFI_PS_NONE:
        return "none";
    case WIFI_PS_MIN_MODEM:
        return "modem (min)";
    case WIFI_PS_MAX_MODEM:
        return "modem (max)";
    default:
        break;
    }

    return "?";
}

// in 0.25dBm units, the driver takes 0..82 (20.5 dBm)
void apply_tx_power() {
    if (!started()) {
        return;
    }

    const auto quarters = static_cast<int>(std::lround(rtos::state().tx_power * 4.0f));
    esp_wifi_set_max_tx_power(static_cast<int8_t>(std::clamp(quarters, 0, 82)));
}

// auto reconnect stays off: the common code decides when and where to connect
void stop_station() {
    ::espurna::core::chip::wifi_sta_disconnect();
}

// the driver does not reconnect by itself
void dropped() {
}

// the SDK reports the 802.11 reason and the AP as they are
bool map_reason(int reported, const Mac& reported_bssid, const Mac&, LinkLost& out) {
    out.reason = reported;
    out.bssid = reported_bssid;
    out.ours = false;
    return true;
}

// only the late disconnect of the previous attempt is not about this one
Attempt attempt_event(const AttemptDisconnect& disconnect, Mac& bssid) {
    if (disconnect.stale) {
        return Attempt::Ignored;
    }

    bssid = disconnect.bssid;
    return Attempt::Failed;
}

// the key management the supplicant negotiated (its WPA_KEY_MGMT_* bits)
extern "C" unsigned int esp_wifi_sta_key_mgmt_espurna(void);

const char* sta_security() {
    const auto akm = esp_wifi_sta_key_mgmt_espurna();
    if (akm & ((1u << 10) | (1u << 11))) {      // SAE, FT_SAE
        return (akm & (1u << 11)) ? "WPA3-PSK (FT)" : "WPA3-PSK";
    }
    if (akm & (1u << 8)) {                      // PSK_SHA256
        return "WPA2-PSK-SHA256";
    }
    if (akm & ((1u << 1) | (1u << 6))) {        // PSK, FT_PSK
        return "WPA2-PSK";
    }
    if (akm & (1u << 0)) {                      // IEEE8021X
        return "WPA2-EAP";
    }
    return "OPEN";
}

// no auth mode of its own besides the common ones
const char* auth_name(int) {
    return nullptr;
}

const char* reason_name(int reason) {
    return ::espurna::core::chip::wifi_reason_name(reason);
}

// the scan count (-1 while it runs), keeps waiting past its own timeout until ours (ref. rtos::ScanTimeout)
bool scan_finished(rtos::Scan& scan, int16_t& count) {
    count = ::espurna::core::chip::wifi_scan_count();
    return (count >= 0) || rtos::expired(scan.at, rtos::ScanTimeout);
}

// the name DHCP sends: the interface exists once the mode is set
void before_connect(const StaConnect& connect) {
    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, connect.hostname.c_str());
}

uint8_t ap_prepare(uint8_t channel) {
    return channel;
}

void ap_started() {
    apply_tx_power();
}

void ap_stop() {
}

size_t ap_stations() {
    return ::espurna::core::chip::wifi_ap_stations();
}

size_t print_stations(::espurna::Output& out) {
    wifi_sta_list_t list{};
    tcpip_adapter_sta_list_t netif_list{};
    if ((esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        || (tcpip_adapter_get_sta_list(&list, &netif_list) != ESP_OK))
    {
        return 0;
    }

    for (int index = 0; index < netif_list.num; ++index) {
        const auto& sta = netif_list.sta[index];
        out.printf_P(PSTR("%02X:%02X:%02X:%02X:%02X:%02X %s\n"),
            sta.mac[0], sta.mac[1], sta.mac[2], sta.mac[3], sta.mac[4], sta.mac[5],
            ipToString(espurna::Ip4(sta.ip.addr)).c_str());
    }

    return static_cast<size_t>(netif_list.num);
}

// lwIP of the SDK is built without IPv6 (as the NONOS build's lwip2)
void ipv6(bool) {
}

} // namespace chip
} // namespace driver
} // namespace wifi
} // namespace espurna

// ESP-NOW is not used: the Wi-Fi library (ieee80211_api esp_wifi_deinit) calls only esp_now_deinit() of it, which with
// nothing started has nothing to do. Defined here, libespnow (espnow.o, manatick.o) stays out of the head
extern "C" esp_err_t esp_now_deinit() {
    return ESP_OK;
}
