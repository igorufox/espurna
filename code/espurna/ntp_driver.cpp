/*

NTP MODULE, the SNTP client part (ref. ntp_driver.h): what every chip has alike. The rest talks to the SDK around
lwIP's SNTP client, which differs (not the client itself):
- esp8266 lwip2 (arch/esp8266/ntp_esp8266.cpp): Core calls settimeofday_cb() for every settimeofday(), SNTP or manual,
  lwip2 glue restarts SNTP by itself every time the netif gets a valid address, and asks for the update interval every
  time (weak functions)
- ESP32 and Beken (arch/rtos/ntp_rtos.cpp): one driver, every SNTP call in the tcpip thread, the sync seen by the
  clock offset, the client restarted when the station gets its address; the SDK calls in arch/<chip>/ntp_chip.h
  (ESP-IDF esp_sntp_* with its own update interval, plain lwIP with the compile-time one)

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "espurna_chip.h"
#include "ntp_driver.h"

// the SNTP API: ESP-IDF 5 declares the sntp_* wrappers in esp_sntp.h, they conflict with lwip's own header (what the
// SDK has, not the chip)
#if __has_include(<esp_sntp.h>)
#include <esp_sntp.h>
#else
#include <lwip/apps/sntp.h>
#endif

static_assert(
    (SNTP_SERVER_DNS == 1),
    "lwip must be configured with SNTP_SERVER_DNS"
);

namespace espurna {
namespace ntp {
namespace driver {

const char* server_name() {
    return sntp_getservername(0);
}

espurna::string server_address() {
    espurna::string out;

    const ip_addr_t* addr = sntp_getserver(0);
    if (addr) {
        const auto ip = ::espurna::chip::address(addr);
        if (::espurna::chip::address_set(ip)) {
            out = ipToString(ip);
        }
    }

    return out;
}

} // namespace driver
} // namespace ntp
} // namespace espurna

#endif // NTP_SUPPORT
