/*

NTP MODULE, ESP32 and LibreTiny part (ref. ntp_driver.h)

lwIP SNTP client, running in the tcpip task. ESP-IDF wraps it (esp_sntp.h) and adds a sync notification and its own
update interval; plain lwIP (LibreTiny) has neither. The difference is the SNTP API, not the chip.

*/

#include "espurna.h"

#if (defined(ESP32) || defined(LIBRETINY)) && NTP_SUPPORT

#include "ntp_driver.h"

#if __has_include(<esp_sntp.h>)
// esp_sntp.h includes lwip's sntp.h itself; ESP-IDF 5 (core 3.x) declares the sntp_* wrappers there and they
// conflict with a direct include of lwip/apps/sntp.h
#include <esp_sntp.h>
#define ESPURNA_NTP_IDF_SNTP 1
#else
#include <lwip/apps/sntp.h>
#define ESPURNA_NTP_IDF_SNTP 0
#endif

#include <lwip/tcpip.h>

static_assert(
    (SNTP_SERVER_DNS == 1),
    "lwip must be configured with SNTP_SERVER_DNS"
);

namespace espurna {
namespace ntp {
namespace driver {
namespace {

#if ESPURNA_NTP_IDF_SNTP
// ESP-IDF 5 kept sntp_init() and friends as wrappers, but not these two
bool sntp_running() {
    return esp_sntp_enabled();
}

void sntp_halt() {
    esp_sntp_stop();
}

// SNTP-only, unlike esp8266 settimeofday_cb(). Runs in the tcpip task
void sync_notification(struct timeval*) {
    espurnaRegisterOnce(on_synced);
}

void watch_setup() {
    sntp_set_time_sync_notification_cb(sync_notification);
}
#else
bool sntp_running() {
    return sntp_enabled();
}

void sntp_halt() {
    sntp_stop();
}

// Plain lwip SNTP has no sync notification, it only calls settimeofday() (ref. LibreTiny lwipopts.h), which
// LibreTiny already wraps. Clock is millis() + epoch offset there, a changed offset is a new sync (a manual one too)
void watch_sync() {
    static time_t last { 0 };

    const auto offset = ::time(nullptr) - static_cast<time_t>(::millis() / 1000);
    if ((offset - last > 1) || (last - offset > 1)) {
        last = offset;
        on_synced();
    }
}

void watch_setup() {
    ::espurnaRegisterLoop(watch_sync);
}
#endif

// esp8266 lwip2 glue restarts SNTP every time the netif gets a valid address (sntp_stop + sntp_init from the netif
// status callback), IDF and plain lwIP do not. sntp_init() from setup() runs before the station is up, the first
// request fails and lwip falls back to its retry backoff (15s, 30s, ...), so the first sync used to happen up to ~30s
// after the IP was assigned. Kick SNTP from the tcpip thread, same as esp8266 does.
void restart_on_got_ip() {
    tcpip_callback(
        [](void*) {
            if (sntp_running()) {
                sntp_halt();
                sntp_init();
            }
        }, nullptr);
}

} // namespace

void setup() {
    watch_setup();

    // StationConnected is published from the loop once the IP is assigned
    wifiRegister([](espurna::wifi::Event event) {
        if (event == espurna::wifi::Event::StationConnected) {
            on_got_ip();
            restart_on_got_ip();
        }
    });
}

// esp8266 lwip asks sntp_update_delay_MS_rfc_not_less_than_15000() every time, IDF has its own interval instead
// (3 hours by default) and ntpUpdateIntvl would be ignored otherwise. Plain lwIP keeps its compile-time interval.
void configure() {
#if ESPURNA_NTP_IDF_SNTP
    sntp_set_sync_interval(update_interval());
#endif
}

// With the IDF notification, ticks (and scheduler) never start when time is set manually. Plain lwIP: watch_sync()
// notices the new offset by itself
void time_set() {
#if ESPURNA_NTP_IDF_SNTP
    espurnaRegisterOnce(on_synced);
#endif
}

bool enabled() {
    return sntp_running();
}

void init() {
    sntp_init();
}

void stop() {
    sntp_halt();
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

    const ip_addr_t* addr = sntp_getserver(0);
    if (addr) {
        auto ip = IPAddress(ip_addr_get_ip4_u32(addr));
        if ((uint32_t)ip != 0) {
            out = ipToString(ip);
        }
    }

    return out;
}

} // namespace driver
} // namespace ntp
} // namespace espurna

#endif
