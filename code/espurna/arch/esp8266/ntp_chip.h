/*

NTP MODULE, the SNTP client of the SDK's lwIP (ESP8266_RTOS_SDK 3.4; ref. arch/rtos/ntp_rtos.cpp, which calls these from
the tcpip thread). The SDK's sntp.h adds the update interval to plain lwIP's client

*/

#pragma once

// lwIP's client (sntp_init, sntp_setservername ...) and the SDK's additions (sntp_set_sync_interval)
#include <lwip/apps/sntp.h>
#include <sntp.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace chip {

inline bool enabled() {
    return sntp_enabled();
}

inline void init() {
    sntp_init();
}

inline void stop() {
    sntp_stop();
}

inline void dhcp(bool value) {
    sntp_servermode_dhcp(value);
}

inline void server(const char* name) {
    sntp_setservername(0, name);
}

inline void interval(uint32_t ms) {
    sntp_set_sync_interval(ms);
}

} // namespace chip
} // namespace driver
} // namespace ntp
} // namespace espurna
