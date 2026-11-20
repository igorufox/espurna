/*

MODULES: what ESP32 and Beken share of the module loader (modules_core.h arch::), the FreeRTOS part

*/

#include "espurna.h"

#if MODULES_SUPPORT

#include "modules.h"
#include "modules_core.h"


namespace espurna {
namespace modules {
namespace arch {
namespace {

TimerHandle_t guard_timer { nullptr };

void guardTimerCallback(TimerHandle_t) {
    guardTick();
}

} // namespace

// the guard's timer: a FreeRTOS one, its own task, runs while loop() waits or hangs
void guardTimerStart() {
    if (!guard_timer) {
        guard_timer = xTimerCreate("modules", pdMS_TO_TICKS(1000), pdTRUE, nullptr, guardTimerCallback);
    }
    if (guard_timer) {
        xTimerStart(guard_timer, 0);
    }
}

void guardTimerStop() {
    if (guard_timer) {
        xTimerStop(guard_timer, 0);
    }
}

} // namespace arch
} // namespace modules
} // namespace espurna

#endif // MODULES_SUPPORT
