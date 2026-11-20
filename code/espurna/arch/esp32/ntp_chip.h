/*

NTP MODULE, the SNTP client of ESP-IDF (ref. arch/rtos/ntp_rtos.cpp, which calls these from the tcpip thread)

esp_sntp_* of ESP-IDF 5 run the lwIP call right away when they are already in the tcpip thread (esp_netif_tcpip_exec),
any other task hands it over and waits. The update interval is the IDF's own (3 hours by default)

*/

#pragma once

// esp_sntp.h includes lwip's sntp.h itself; ESP-IDF 5 (core 3.x) declares the sntp_* wrappers there and they conflict
// with a direct include of lwip/apps/sntp.h
#include <esp_sntp.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace chip {

inline bool enabled() {
    return esp_sntp_enabled();
}

inline void init() {
    esp_sntp_init();
}

inline void stop() {
    esp_sntp_stop();
}

inline void dhcp(bool value) {
    esp_sntp_servermode_dhcp(value);
}

inline void server(const char* name) {
    esp_sntp_setservername(0, name);
}

inline void interval(uint32_t ms) {
    sntp_set_sync_interval(ms);
}

} // namespace chip
} // namespace driver
} // namespace ntp
} // namespace espurna
