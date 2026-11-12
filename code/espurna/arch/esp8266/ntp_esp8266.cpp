/*

NTP MODULE, the SNTP client of the esp8266 lwip2 SDK (ntp_driver.h): Core calls settimeofday_cb() for every
settimeofday(), SNTP or manual; lwip2 glue restarts SNTP by itself every time the netif gets a valid address and asks
for the start delay and the update interval every time (its weak functions, below)

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "ntp_driver.h"

#include <coredecls.h>
#include <lwip/apps/sntp.h>

namespace espurna {
namespace ntp {
namespace driver {

// will be called every time after ntp syncs AND loop() finishes; make sure our logic does know about the actual
// server in case dhcp sends out ntp settings (lwip2 glue already restarts SNTP from the netif status callback)
void setup() {
    settimeofday_cb(on_synced);

    static auto track_active_server = WiFi.onStationModeGotIP(
        [](WiFiEventStationModeGotIP) {
            on_got_ip();
        });
}

// lwip asks sntp_update_delay_MS_rfc_not_less_than_15000() every time
void configure() {
}

// settimeofday_cb() already fired
void time_set() {
}

bool enabled() {
    return sntp_enabled();
}

// no other context than the loop and SYS
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

#endif // NTP_SUPPORT
