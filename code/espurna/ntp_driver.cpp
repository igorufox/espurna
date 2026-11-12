/*

NTP MODULE, the SNTP client part (ref. ntp_driver.h)

lwIP SNTP client on every chip, the API calls are the same. What differs is the SDK around it, not the chip:
- esp8266 lwip2: Core calls settimeofday_cb() for every settimeofday(), SNTP or manual, lwip2 glue restarts SNTP by
  itself every time the netif gets a valid address, and asks for the update interval every time (weak functions below)
- ESP-IDF (ESP32): esp_sntp.h wraps lwIP, adds a sync notification and its own update interval
- plain lwIP (LibreTiny): no notification, no restart on a new address, compile-time interval

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "espurna_chip.h"
#include "ntp_driver.h"

#if defined(ESP8266)
#include <coredecls.h>
#include <lwip/apps/sntp.h>
#define ESPURNA_NTP_IDF_SNTP 0
#elif __has_include(<esp_sntp.h>)
// esp_sntp.h includes lwip's sntp.h itself; ESP-IDF 5 (core 3.x) declares the sntp_* wrappers there and they
// conflict with a direct include of lwip/apps/sntp.h
#include <esp_sntp.h>
#define ESPURNA_NTP_IDF_SNTP 1
#else
#include <lwip/apps/sntp.h>
#define ESPURNA_NTP_IDF_SNTP 0
#endif

#if !defined(ESP8266)
#include <lwip/tcpip.h>
#endif

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
#else
bool sntp_running() {
    return sntp_enabled();
}

void sntp_halt() {
    sntp_stop();
}
#endif

#if defined(ESP8266)
// will be called every time after ntp syncs AND loop() finishes
void watch_setup() {
    settimeofday_cb(on_synced);
}

// make sure our logic does know about the actual server in case dhcp sends out ntp settings.
// lwip2 glue already restarts SNTP from the netif status callback
void got_ip_setup() {
    static auto track_active_server = WiFi.onStationModeGotIP(
        [](WiFiEventStationModeGotIP) {
            on_got_ip();
        });
}
#else
#if ESPURNA_NTP_IDF_SNTP
// SNTP-only, unlike esp8266 settimeofday_cb(). Runs in the tcpip task
void sync_notification(struct timeval*) {
    espurnaRegisterOnce(on_synced);
}

void watch_setup() {
    sntp_set_time_sync_notification_cb(sync_notification);
}
#else
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

// StationConnected is published from the loop once the IP is assigned
void got_ip_setup() {
    wifiRegister([](espurna::wifi::Event event) {
        if (event == espurna::wifi::Event::StationConnected) {
            on_got_ip();
            restart_on_got_ip();
        }
    });
}
#endif

} // namespace

void setup() {
    watch_setup();
    got_ip_setup();
}

// esp8266 lwip asks sntp_update_delay_MS_rfc_not_less_than_15000() every time, IDF has its own interval instead
// (3 hours by default) and ntpUpdateIntvl would be ignored otherwise. Plain lwIP keeps its compile-time interval.
void configure() {
#if ESPURNA_NTP_IDF_SNTP
    sntp_set_sync_interval(update_interval());
#endif
}

// esp8266: settimeofday_cb() already fired. With the IDF notification, ticks (and scheduler) never start when time is
// set manually. Plain lwIP: watch_sync() notices the new offset by itself
void time_set() {
#if ESPURNA_NTP_IDF_SNTP
    espurnaRegisterOnce(on_synced);
#endif
}

bool enabled() {
    return sntp_running();
}

#if defined(ESP8266) || ESPURNA_NTP_IDF_SNTP
// esp8266: no other context. ESP-IDF: the esp_sntp wrappers hand the call over to the tcpip task themselves
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
#else
// Plain lwIP (LibreTiny): the sntp_* calls belong to the tcpip thread, same as the restart above. Posted in the order
// of the calls, and they run in that order. lwIP keeps the name pointer, the thread has its own copy of it
namespace {

bool run_tcpip(tcpip_callback_fn callback, void* arg) {
    if (tcpip_callback(callback, arg) != ERR_OK) {
        DEBUG_MSG_P(PSTR("[NTP] tcpip thread did not take the call\n"));
        return false;
    }

    return true;
}

} // namespace

void init() {
    run_tcpip([](void*) {
        sntp_init();
    }, nullptr);
}

void stop() {
    run_tcpip([](void*) {
        sntp_halt();
    }, nullptr);
}

void dhcp(bool value) {
    run_tcpip([](void* arg) {
        sntp_servermode_dhcp(arg != nullptr);
    }, value ? reinterpret_cast<void*>(1) : nullptr);
}

void server(const char* name) {
    auto* copy = new String(name ? name : "");
    const bool posted = run_tcpip([](void* arg) {
        // only ever touched in the tcpip thread
        static String held;

        auto* name = static_cast<String*>(arg);
        held = std::move(*name);
        delete name;

        sntp_setservername(0, held.c_str());
    }, copy);

    if (!posted) {
        delete copy;
    }
}
#endif

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

#if defined(ESP8266)
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

#endif // NTP_SUPPORT
