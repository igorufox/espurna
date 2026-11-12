/*

NTP MODULE, the SNTP client of plain lwIP (LibreTiny, ntp_driver.h): no sync notification, no restart on a new address,
the compile-time interval. The sntp_* calls belong to the tcpip thread

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "ntp_driver.h"

#include <lwip/apps/sntp.h>
#include <lwip/tcpip.h>

namespace espurna {
namespace ntp {
namespace driver {
namespace rtos {

void got_ip_setup(void (*restart_in_tcpip)(void*));

} // namespace rtos

namespace {

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

void restart_in_tcpip(void*) {
    if (sntp_enabled()) {
        sntp_stop();
        sntp_init();
    }
}

bool run_tcpip(tcpip_callback_fn callback, void* arg) {
    if (tcpip_callback(callback, arg) != ERR_OK) {
        DEBUG_MSG_P(PSTR("[NTP] tcpip thread did not take the call\n"));
        return false;
    }

    return true;
}

} // namespace

void setup() {
    ::espurnaRegisterLoop(watch_sync);
    rtos::got_ip_setup(restart_in_tcpip);
}

// the compile-time interval
void configure() {
}

// watch_sync() notices the new offset by itself
void time_set() {
}

bool enabled() {
    return sntp_enabled();
}

// posted in the order of the calls, and they run in that order. lwIP keeps the name pointer, the thread has its own
// copy of it
void init() {
    run_tcpip([](void*) {
        sntp_init();
    }, nullptr);
}

void stop() {
    run_tcpip([](void*) {
        sntp_stop();
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

} // namespace driver
} // namespace ntp
} // namespace espurna

#endif // NTP_SUPPORT
