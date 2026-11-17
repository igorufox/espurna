/*

OTA MODULE, ESP8266: the stall watcher of espota (ArduinoOTA, ref. ota_driver_chip.h)

ArduinoOTA receives the whole image inside one loop() call. When the upload stops taking data, its receive loop keeps
waiting while the connection looks open: the board hung until a power cycle (W58, 10.0.5.0 11-16). The SDK timer runs
while that loop waits in delay(): no new byte for StallSeconds, the trace says so (ota_arduinoota.cpp reports it at
the next boot) and the board restarts.

*/

#include "espurna.h"

#if OTA_ARDUINOOTA_SUPPORT

#include "rtcmem.h"
#include "../../espurna_ota.h"
#include "../../ota_driver.h"

#include <osapi.h>
#include <user_interface.h>

namespace espurna {
namespace ota {
namespace driver {
namespace {

constexpr uint32_t StallSeconds { 20 };

os_timer_t stall_timer;
uint32_t stall_bytes { 0 };
uint32_t stall_idle { 0 };

void stall_check(void*) {
    const uint32_t bytes = Rtcmem->ota_bytes;
    if (bytes != stall_bytes) {
        stall_bytes = bytes;
        stall_idle = 0;
        return;
    }

    if (++stall_idle >= StallSeconds) {
        os_timer_disarm(&stall_timer);
        Rtcmem->ota_trace = TraceStalled;
        system_restart();
    }
}

} // namespace

void arduino_ota_started() {
    stall_bytes = Rtcmem->ota_bytes;
    stall_idle = 0;
    os_timer_disarm(&stall_timer);
    os_timer_setfn(&stall_timer, stall_check, nullptr);
    os_timer_arm(&stall_timer, 1000, true);
}

void arduino_ota_stopped() {
    os_timer_disarm(&stall_timer);
}

} // namespace driver
} // namespace ota
} // namespace espurna

#endif // OTA_ARDUINOOTA_SUPPORT
