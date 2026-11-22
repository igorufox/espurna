/*

NTP MODULE, the SNTP client of plain lwIP (BK72xx; ref. arch/rtos/ntp_rtos.cpp, which calls these from the tcpip
thread). The update interval is the compile-time SNTP_UPDATE_DELAY of the lwIP options (the SDK glue lwipopts, framework-beken-bdk)

*/

#pragma once

#include <lwip/apps/sntp.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace chip {

// plain lwIP: the raw API, run in the tcpip thread (ntp_rtos.cpp run_tcpip)
constexpr bool PostsItself { false };

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

inline void interval(uint32_t) {
}

} // namespace chip
} // namespace driver
} // namespace ntp
} // namespace espurna
