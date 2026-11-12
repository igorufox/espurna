/*

NTP MODULE, the SNTP client of ESP-IDF (ntp_driver.h): esp_sntp.h wraps lwIP, adds a sync notification (from the
tcpip task) and its own update interval (3 hours by default, ntpUpdateIntvl would be ignored otherwise)

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "ntp_driver.h"

// esp_sntp.h includes lwip's sntp.h itself; ESP-IDF 5 (core 3.x) declares the sntp_* wrappers there and they conflict
// with a direct include of lwip/apps/sntp.h
#include <esp_sntp.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace rtos {

void got_ip_setup(void (*restart_in_tcpip)(void*));

} // namespace rtos

namespace {

// SNTP-only, unlike esp8266 settimeofday_cb(). Runs in the tcpip task
void sync_notification(struct timeval*) {
    espurnaRegisterOnce(on_synced);
}

void restart_in_tcpip(void*) {
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        sntp_init();
    }
}

} // namespace

void setup() {
    sntp_set_time_sync_notification_cb(sync_notification);
    rtos::got_ip_setup(restart_in_tcpip);
}

void configure() {
    sntp_set_sync_interval(update_interval());
}

// with the IDF notification, ticks (and scheduler) never start when the time is set manually
void time_set() {
    espurnaRegisterOnce(on_synced);
}

// ESP-IDF 5 kept sntp_init() and friends as wrappers, but not these two
bool enabled() {
    return esp_sntp_enabled();
}

// the esp_sntp wrappers hand the call over to the tcpip task themselves
void init() {
    sntp_init();
}

void stop() {
    esp_sntp_stop();
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

#endif // NTP_SUPPORT
