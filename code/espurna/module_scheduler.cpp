/*

PROTOTYPE: the scheduler module (docs/modules.md), env:esp8266-1m-modules of platformio_esp8266.ini

*/

#include "espurna.h"

#if MODULE_BUILD && SCHEDULER_SUPPORT

#include "modules.h"
#include "scheduler.h"

ESPURNA_MODULE(scheduler, "scheduler", 50) {
    schSetup();
}

#endif
