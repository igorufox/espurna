/*

NTP MODULE, what ESP32 and Beken share of the SNTP client (ntp_driver.h): lwip2 of the esp8266 restarts SNTP every
time the netif gets a valid address (sntp_stop + sntp_init from the netif status callback), IDF and plain lwIP do not.
sntp_init() from setup() runs before the station is up, the first request fails and lwip falls back to its retry
backoff (15s, 30s, ...), so the first sync used to happen up to ~30s after the IP was assigned. The chip restarts it
from the tcpip thread, same as esp8266 does (restart_in_tcpip() of arch/<chip>/ntp_<chip>.cpp)

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "ntp_driver.h"
#include "wifi.h"

#include <lwip/tcpip.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace rtos {

// StationConnected is published from the loop once the IP is assigned
void got_ip_setup(void (*restart_in_tcpip)(void*)) {
    static auto* restart = restart_in_tcpip;
    wifiRegister([](espurna::wifi::Event event) {
        if (event == espurna::wifi::Event::StationConnected) {
            on_got_ip();
            tcpip_callback(restart, nullptr);
        }
    });
}

} // namespace rtos
} // namespace driver
} // namespace ntp
} // namespace espurna

#endif // NTP_SUPPORT
