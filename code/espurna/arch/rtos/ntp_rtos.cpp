/*

NTP MODULE, the SNTP client of the FreeRTOS chips, ESP32 and Beken (ntp_driver.h). The calls of the SDK are in
arch/<chip>/ntp_chip.h, every one of them runs in the tcpip thread, posted in the order of the calls (or, where the
chip's calls post themselves, chip::PostsItself, called right away)

lwip2 of the esp8266 restarts SNTP every time the netif gets a valid address (sntp_stop + sntp_init from the netif
status callback), IDF and plain lwIP do not. sntp_init() from setup() runs before the station is up, the first request
fails and lwip falls back to its retry backoff (15s, 30s, ...), so the first sync used to happen up to ~30s after the IP
was assigned. Here it is restarted from the tcpip thread once the station has its address, same as esp8266 does

*/

#include "espurna.h"

#if NTP_SUPPORT

#include "ntp_driver.h"
#include "ntp_chip.h"
#include "../../espurna_wifi.h"

#include <lwip/tcpip.h>

#include <new>

namespace espurna {
namespace ntp {
namespace driver {
namespace {

// a clock before this has not been set: no sync yet
constexpr time_t SetSince { 1577836800 }; // 2020-01-01

// No sync notification is taken from the SDK (plain lwIP has none): lwIP SNTP only calls settimeofday(), and a changed
// offset between the clock and espurna::time::ms() is a new sync (a manual one too). Only a clock that is set counts: time() and
// espurna::time::ms() run on different timers of the BK7231N and drift apart by seconds while nothing set the clock, that was
// taken for a sync - NTP "SYNC'D" with 1970-01-01 in the WebUI (10.0.5.139 2026-11-15, W51)
void watch_sync() {
    static time_t last { 0 };

    const auto now = ::time(nullptr);
    if (now < SetSince) {
        return;
    }

    const auto offset = now - static_cast<time_t>(espurna::time::ms() / 1000);
    if ((offset - last > 1) || (last - offset > 1)) {
        last = offset;
        on_synced();
    }
}

// in the tcpip thread, or right here where the chip's calls post themselves there (chip::PostsItself, ESP32)
bool run_tcpip(tcpip_callback_fn callback, void* arg) {
    if (chip::PostsItself) {
        callback(arg);
        return true;
    }

    if (tcpip_callback(callback, arg) != ERR_OK) {
        DEBUG_MSG_P(PSTR("[NTP] tcpip thread did not take the call\n"));
        return false;
    }

    return true;
}

void restart_in_tcpip(void*) {
    if (chip::enabled()) {
        chip::stop();
        chip::init();
    }
}

} // namespace

void setup() {
    ::espurnaRegisterLoop(watch_sync);

    // StationConnected is published from the loop once the IP is assigned
    wifiRegister([](espurna::wifi::Event event) {
        if (event == espurna::wifi::Event::StationConnected) {
            on_got_ip();
            run_tcpip(restart_in_tcpip, nullptr);
        }
    });
}

void configure() {
    run_tcpip([](void* arg) {
        chip::interval(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(update_interval())));
}

// watch_sync() notices the new offset by itself
void time_set() {
}

bool enabled() {
    return chip::enabled();
}

void init() {
    run_tcpip([](void*) {
        chip::init();
    }, nullptr);
}

void stop() {
    run_tcpip([](void*) {
        chip::stop();
    }, nullptr);
}

void dhcp(bool value) {
    run_tcpip([](void* arg) {
        chip::dhcp(arg != nullptr);
    }, value ? reinterpret_cast<void*>(1) : nullptr);
}

// lwIP keeps the name pointer, the thread has its own copy of it
void server(const char* name) {
    auto* copy = new (std::nothrow) espurna::string(name ? name : "");
    if (!copy) {
        return;
    }

    const bool posted = run_tcpip([](void* arg) {
        // only ever touched in the tcpip thread
        static espurna::string held;

        auto* name = static_cast<espurna::string*>(arg);
        held = std::move(*name);
        delete name;

        chip::server(held.c_str());
    }, copy);

    if (!posted) {
        delete copy;
    }
}

} // namespace driver
} // namespace ntp
} // namespace espurna

#endif // NTP_SUPPORT
