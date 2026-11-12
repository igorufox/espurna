/*

WIFI MODULE, ESP32 (Arduino core 3, ESP-IDF) part: what only ESP-IDF knows (ref. wifi_driver_rtos.h). The driver
itself (events, link state, connection, scan, AP settings) is the shared one, wifi_driver_rtos.cpp

*/

#include "../../espurna.h"

#if defined(ARDUINO_ARCH_ESP32)

#include "../rtos/wifi_driver_rtos.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#if __has_include(<esp_wifi_ap_get_sta_list.h>)
#include <esp_wifi_ap_get_sta_list.h>    // ESP-IDF 5 (core 3.x)
#endif
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

namespace espurna {
namespace wifi {
namespace driver {
namespace chip {
namespace {

bool started() {
    return WiFi.getMode() != WIFI_MODE_NULL;
}

// EU country code, channels 12 and 13 are scanned and used as well
void apply_country() {
    if (!started()) {
        return;
    }

    wifi_country_t country = {.cc = "EU", .schan = 1, .nchan = 13, .policy = WIFI_COUNTRY_POLICY_AUTO};
    esp_wifi_set_country(&country);
}

} // namespace

void init() {
#if WIFI_DISABLE_BROWNOUT_DETECTOR
    // Opt-in: disables hardware brownout protection. Leaving this on can
    // protect against TX-current spikes on weak PSUs but also masks real
    // brownouts and risks flash corruption mid-write.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
}

void opmode(uint8_t mode) {
    const bool was = started();
    WiFi.mode(static_cast<wifi_mode_t>(mode));

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

// in 0.25dBm units, driver accepts 2..20dBm
void apply_tx_power() {
    if (!started()) {
        return;
    }

    const auto quarters = static_cast<int>(std::lround(rtos::state().tx_power * 4.0f));
    esp_wifi_set_max_tx_power(static_cast<int8_t>(std::clamp(quarters, 8, 80)));
}

// auto reconnect stays off: the common code decides when and where to connect
void stop_station() {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
}

// the driver does not reconnect by itself
void dropped() {
}

// ESP-IDF reports the 802.11 reason and the AP as they are
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

const char* reason_name(int reason) {
    return WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
}

// WiFi.scanComplete(), keeps waiting past its own timeout until ours (ref. rtos::ScanTimeout)
bool scan_finished(rtos::Scan& scan, int16_t& count) {
    count = WiFi.scanComplete();
    return (count >= 0) || rtos::expired(scan.at, rtos::ScanTimeout);
}

// WiFi.setHostname() only takes effect when the STA interface is created (before WiFi.mode(WIFI_STA)), so DHCP sent
// the default 'esp32-XXXXXX' (10.0.5.66, 2026-11-09). The interface exists by now, it gets the name directly.
// The driver keeps the AP on the station channel while the station is connected
void before_connect(const StaConnect& connect) {
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_set_hostname(netif, connect.hostname.c_str());
    }
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
    return WiFi.softAPgetStationNum();
}

size_t print_stations(Print& out) {
    wifi_sta_list_t list{};
#if ESP_IDF_VERSION_MAJOR >= 5
    // ESP-IDF 5 (core 3.x): esp_netif_get_sta_list() became esp_wifi_ap_get_sta_list_with_ip()
    wifi_sta_mac_ip_list_t netif_list{};
    if ((esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        || (esp_wifi_ap_get_sta_list_with_ip(&list, &netif_list) != ESP_OK))
#else
    esp_netif_sta_list_t netif_list{};
    if ((esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        || (esp_netif_get_sta_list(&list, &netif_list) != ESP_OK))
#endif
    {
        return 0;
    }

    for (int index = 0; index < netif_list.num; ++index) {
        const auto& sta = netif_list.sta[index];
        out.printf_P(PSTR("%02X:%02X:%02X:%02X:%02X:%02X %s\n"),
            sta.mac[0], sta.mac[1], sta.mac[2], sta.mac[3], sta.mac[4], sta.mac[5],
            IPAddress(sta.ip.addr).toString().c_str());
    }

    return static_cast<size_t>(netif_list.num);
}

// the Arduino core adds the link-local address on every STA connect once enabled; a station that is connected
// already (the Matter module starts after wifiSetup()) gets it now
void ipv6(bool enabled) {
    WiFi.enableIPv6(enabled);
    if (enabled && (WiFi.status() == WL_CONNECTED)) {
        auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_create_ip6_linklocal(netif);
        }
    }
}

} // namespace chip
} // namespace driver
} // namespace wifi
} // namespace espurna

#endif
