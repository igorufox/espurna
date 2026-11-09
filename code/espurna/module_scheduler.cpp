/*

PROTOTYPE: the scheduler module (docs/esp8266-modules.md), env:esp8266-1m-modules of platformio_modules.ini

*/

#include "espurna.h"

#if MODULE_BUILD && SCHEDULER_SUPPORT

#include "modules.h"
#include "scheduler.h"

ESPURNA_MODULE(scheduler, "scheduler", 50) {
    schSetup();
}

#endif
