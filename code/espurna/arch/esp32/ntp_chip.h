/*

NTP MODULE, the SNTP client of ESP-IDF (ref. arch/rtos/ntp_rtos.cpp, which calls these from the task that asks)

esp_sntp_* of ESP-IDF 5.5 post the lwIP call to the tcpip thread and wait for it (tcpip_callback, tcpip_api_call) from
any task, the tcpip thread included (it then waits for itself): never called from there. The update interval is the IDF's own (3 hours by default)

*/

#pragma once

// esp_sntp.h includes lwip's sntp.h itself; ESP-IDF 5 (core 3.x) declares the sntp_* wrappers there and they conflict
// with a direct include of lwip/apps/sntp.h
#include <esp_sntp.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace chip {

// esp_sntp_* hand their work to the tcpip thread themselves and wait for it (tcpip_callback / tcpip_api_call): called
// from that thread, esp_sntp_setservername() waited for itself (22.11, the first run on the DevKit: the tcpip thread
// stuck, the loop behind it in lwipCall(), the task watchdog reset the board 20 s after every boot). Called from the
// task that asks, never from the tcpip thread
constexpr bool PostsItself { true };

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
