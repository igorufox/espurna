/*

NTP MODULE, the SNTP client part (ref. ntp_driver.h): what every chip has alike. The rest talks to the SDK around
lwIP's SNTP client, which differs (not the client itself):
- esp8266 lwip2 (arch/esp8266/ntp_esp8266.cpp): Core calls settimeofday_cb() for every settimeofday(), SNTP or manual,
  lwip2 glue restarts SNTP by itself every time the netif gets a valid address, and asks for the update interval every
  time (weak functions)
- ESP-IDF (arch/esp32/ntp_esp32.cpp): esp_sntp.h wraps lwIP, adds a sync notification and its own update interval
- plain lwIP (arch/beken/ntp_beken.cpp): no notification, no restart on a new address, compile-time interval
ESP32 and Beken restart the client when the station gets its address (arch/rtos/ntp_rtos.cpp)

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

String server_address() {
    String out;

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
