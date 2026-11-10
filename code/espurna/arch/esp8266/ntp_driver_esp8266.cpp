/*

NTP MODULE, esp8266 part (ref. ntp_driver.h)

lwip2 SNTP client. Core calls settimeofday_cb() for every settimeofday(), SNTP or manual,
and lwip2 glue restarts SNTP by itself every time the netif gets a valid address.

*/

#include "../../espurna.h"

#if defined(ESP8266) && NTP_SUPPORT

#include "../../ntp_driver.h"

#include <coredecls.h>
#include <lwip/apps/sntp.h>

static_assert(
    (SNTP_SERVER_DNS == 1),
    "lwip must be configured with SNTP_SERVER_DNS"
);

namespace espurna {
namespace ntp {
namespace driver {

void setup() {
    // will be called every time after ntp syncs AND loop() finishes
    settimeofday_cb(on_synced);

    // make sure our logic does know about the actual server
    // in case dhcp sends out ntp settings
    static auto track_active_server = WiFi.onStationModeGotIP(
        [](WiFiEventStationModeGotIP) {
            on_got_ip();
        });
}

// lwip asks sntp_update_delay_MS_rfc_not_less_than_15000() every time, see below
void configure() {
}

// settimeofday_cb() already fired
void time_set() {
}

bool enabled() {
    return sntp_enabled();
}

void init() {
    sntp_init();
}

void stop() {
    sntp_stop();
}

void dhcp(bool value) {
    sntp_servermode_dhcp(value);
}

void server(const char* name) {
    sntp_setservername(0, name);
}

const char* server_name() {
    return sntp_getservername(0);
}

String server_address() {
    String out;

    auto ip = IPAddress(sntp_getserver(0));
    if (ip.isSet()) {
        out = ip.toString();
    }

    return out;
}

} // namespace driver
} // namespace ntp
} // namespace espurna

// Weak in lwip2, declared by its lwipopts.h (included by lwip/apps/sntp.h above)

// aka `SNTP_STARTUP_DELAY_FUNC`
uint32_t sntp_startup_delay_MS_rfc_not_less_than_60000() {
    return espurna::ntp::driver::start_delay();
}

// aka `SNTP_UPDATE_DELAY`
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000() {
    return espurna::ntp::driver::update_interval();
}

#endif
